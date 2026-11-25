/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Table-driven CPLB and L1 cache set-up (Blackfin+ PRM chapter 7).
 */
#include "waffle/drivers/bf70x/cplb.h"

#include <stddef.h>

#include "waffle/drivers/bf70x/core.h"
#include "waffle/drivers/bf70x/coremmr.h"
#include "waffle/drivers/mmr.h"

uint32_t wb_cplb_page_bytes(uint32_t data)
{
	uint32_t psize = (data & CPLB_PSIZE_MASK) >> CPLB_PSIZE_SHIFT;

	if (psize > (uint32_t)WB_CPLB_1G)
		return 0;
	return 1024u << (2u * psize);
}

/* CPROPS values the PRM defines (Table 7-2); 2 and 6 are not listed. */
static bool cprops_ok(uint32_t data, bool is_data)
{
	uint32_t cp;

	if (!is_data)
		return true;
	cp = (data & DCPLB_CPROPS_MASK) >> CPLB_CPROPS_SHIFT;
	return cp != 2u && cp != 6u;
}

WB_FAR_CALL int wb_cplb_check(const struct wb_cplb *t, unsigned n,
			      bool is_data)
{
	unsigned i, j;

	if (n > WB_CPLB_ENTRIES)
		return WB_CPLB_ERR_COUNT;
	if (n != 0 && t == NULL)
		return WB_CPLB_ERR_ENTRY;
	for (i = 0; i < n; i++) {
		uint32_t size = wb_cplb_page_bytes(t[i].data);

		if ((t[i].data & CPLB_VALID) == 0 || size == 0 ||
		    (t[i].addr & (size - 1u)) != 0 ||
		    !cprops_ok(t[i].data, is_data))
			return WB_CPLB_ERR_ENTRY;
		/* Two descriptors matching one address raise an exception. */
		for (j = 0; j < i; j++) {
			uint64_t a0 = t[i].addr, a1 = a0 + size;
			uint64_t b0 = t[j].addr;
			uint64_t b1 = b0 + wb_cplb_page_bytes(t[j].data);

			if (a0 < b1 && b0 < a1)
				return WB_CPLB_ERR_ENTRY;
		}
	}
	return WB_CPLB_OK;
}

/*
 * Write all sixteen descriptor pairs of one side: the table's entries
 * (locked), then invalid ones. Register addresses are computed here rather
 * than through helpers so that nothing outside L1 runs during the change.
 */
static WB_L1_CODE void load_entries(wb_mmr_addr addr0, wb_mmr_addr data0,
				    const struct wb_cplb *t, unsigned n)
{
	uint32_t i;

	for (i = 0; i < WB_CPLB_ENTRIES; i++) {
		uint32_t a = 0, d = 0;

		if (i < n) {
			a = t[i].addr;
			d = t[i].data | CPLB_LOCK;
		}
		wb_mmr_write32(data0 + 4u * i, d);
		wb_mmr_write32(addr0 + 4u * i, a);
	}
}

/* RDCHK is not these functions': the start-up sets it after the L1 pass
 * and every later write keeps it (S-13a.040, wb_start_init()). */
#define ICTL_OWNED (L1IM_ICTL_ENCPLB | L1IM_ICTL_CFG)
#define DCTL_OWNED (L1DM_DCTL_ENCPLB | L1DM_DCTL_CFG_MASK | L1DM_DCTL_ENX)

WB_FAR_CALL WB_L1_CODE void wb_cplb_disable(void)
{
	wb_mmr_clear32(L1DM_DCTL, DCTL_OWNED);
	wb_mmr_sync();
	wb_mmr_clear32(L1IM_ICTL, ICTL_OWNED);
	wb_mmr_sync();
	wb_core_csync();
}

WB_FAR_CALL WB_L1_CODE int wb_cplb_setup(const struct wb_cplb_config *cfg)
{
	uint32_t ictl = 0, dctl = 0;
	int rc;

	if (cfg->dcache != 0 && cfg->dcache != L1DM_DCTL_CFG_A &&
	    cfg->dcache != L1DM_DCTL_CFG_AB)
		return WB_CPLB_ERR_CFG;
	rc = wb_cplb_check(cfg->icplb, cfg->n_icplb, false);
	if (rc == WB_CPLB_OK)
		rc = wb_cplb_check(cfg->dcplb, cfg->n_dcplb, true);
	if (rc != WB_CPLB_OK)
		return rc;

	if (cfg->icache)
		ictl |= L1IM_ICTL_CFG;
	dctl |= cfg->dcache;

	wb_cplb_disable();

	/* Instruction side. Setting CFG invalidates the cache tags. */
	wb_mmr_modify32(L1IM_ICTL, ICTL_OWNED, ictl);
	load_entries(L1IM_ICPLB_ADDR(0), L1IM_ICPLB_DATA(0), cfg->icplb,
		     cfg->n_icplb);
	wb_core_csync();
	if (cfg->n_icplb != 0) {
		wb_mmr_set32(L1IM_ICTL, L1IM_ICTL_ENCPLB);
		wb_mmr_sync();
	}

	/* Data side, the same order. */
	wb_mmr_modify32(L1DM_DCTL, DCTL_OWNED, dctl);
	load_entries(L1DM_DCPLB_ADDR(0), L1DM_DCPLB_DATA(0), cfg->dcplb,
		     cfg->n_dcplb);
	wb_core_csync();
	if (cfg->n_dcplb != 0) {
		wb_mmr_set32(L1DM_DCTL, L1DM_DCTL_ENCPLB);
		wb_mmr_sync();
	}
	wb_core_csync();
	return WB_CPLB_OK;
}

static void dcache_lines(uint32_t addr, uint32_t len,
			 void (*op)(uint32_t line))
{
	uint64_t line = addr & ~(uint64_t)(WB_DCACHE_LINE - 1u);
	uint64_t end = (uint64_t)addr + len;

	if (len == 0)
		return;
	for (; line < end; line += WB_DCACHE_LINE)
		op((uint32_t)line);
	wb_mmr_sync();
}

static void flush_line(uint32_t line)
{
	wb_core_flush_line(line);
}

static void flushinv_line(uint32_t line)
{
	wb_core_flushinv_line(line);
}

void wb_dcache_flush_range(uint32_t addr, uint32_t len)
{
	dcache_lines(addr, len, flush_line);
}

void wb_dcache_invalidate_range(uint32_t addr, uint32_t len)
{
	dcache_lines(addr, len, flushinv_line);
}

/*
 * The generic map. L1 (data A and B, code, scratch) is one 4 MB page at
 * 0x11800000, which L1 accesses need although L1 is never cached. The
 * boot ROM is a 1 MB page, the ADSP-BF703's 256 KB of L2 (data sheet,
 * ordering guide) one 256 KB page, DDR one 256 MB page, the SMC bank a
 * 64 MB I/O page (data only). The OTP memory (0x38000000, 1 KB, from the
 * data sheet's memory map) is a 4 KB uncached read-only data page: the HRM
 * forbids a cache-enabled DCPLB there, and without a page a read (by the
 * ROM's OTP API or otherwise) would miss. Data pages are write-through and
 * DIRTY is set, as the PRM asks when modifications are not tracked per
 * page.
 */
#define D_RW (CPLB_VALID | DCPLB_SWRITE | DCPLB_DIRTY)
#define D_RO (CPLB_VALID)

static const struct wb_cplb bf703_icplb[] = {
	{ 0x11800000u, CPLB_VALID | ICPLB_NOCACHE | WB_CPLB_PSIZE(WB_CPLB_4M) },
	{ 0x04000000u, CPLB_VALID | ICPLB_CACHE | WB_CPLB_PSIZE(WB_CPLB_1M) },
	{ 0x08000000u, CPLB_VALID | ICPLB_CACHE | WB_CPLB_PSIZE(WB_CPLB_256K) },
	{ 0x80000000u, CPLB_VALID | ICPLB_CACHE | WB_CPLB_PSIZE(WB_CPLB_256M) },
};

static const struct wb_cplb bf703_dcplb[] = {
	{ 0x11800000u, D_RW | DCPLB_NOCACHE | WB_CPLB_PSIZE(WB_CPLB_4M) },
	{ 0x04000000u, D_RO | DCPLB_WT | WB_CPLB_PSIZE(WB_CPLB_1M) },
	{ 0x08000000u, D_RW | DCPLB_WT | WB_CPLB_PSIZE(WB_CPLB_256K) },
	{ 0x80000000u, D_RW | DCPLB_WT | WB_CPLB_PSIZE(WB_CPLB_256M) },
	{ 0x70000000u, D_RW | DCPLB_IO | WB_CPLB_PSIZE(WB_CPLB_64M) },
	{ 0x38000000u, D_RO | DCPLB_NOCACHE | WB_CPLB_PSIZE(WB_CPLB_4K) },
};

const struct wb_cplb_config wb_cplb_bf703_default = {
	.icplb = bf703_icplb,
	.n_icplb = sizeof(bf703_icplb) / sizeof(bf703_icplb[0]),
	.dcplb = bf703_dcplb,
	.n_dcplb = sizeof(bf703_dcplb) / sizeof(bf703_dcplb[0]),
	.icache = true,
	.dcache = L1DM_DCTL_CFG_AB,
};
