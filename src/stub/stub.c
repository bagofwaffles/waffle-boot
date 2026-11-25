/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The stub's work on the target (waffle/boot/pack.h): the caches on for the
 * inflate, the packed block unpacked over the main image's link regions,
 * the caches off again, and the main image's entry for stub_entry.S to
 * jump to.
 *
 * The front's _fr_entry leaves the clocks and DDR as its INIT stage set
 * them (S-13a.001: the core at FR_CCLK_MHZ), the caches and both CPLB sets
 * off, the branch predictor off and IMASK bits 5 to 14 clear, in the reset
 * event. The stub changes only the CPLBs and the cache configuration, and
 * puts back what the main image's entry finds today: both caches and CPLB
 * sets off (start.S clears the same bits again). The changes follow the
 * Blackfin+ Programming Reference's sequences ("Enabling L1 Instruction
 * Cache", "Enabling L1 Data Cache"), from code that is not cacheable:
 * .text.stub_ctl, in the first 1 KiB page, which the instruction pages
 * leave uncached (stub.ld). Data pages are write-through, so turning the
 * data cache off loses nothing; clearing L1DM_DCTL.CFG and L1IM_ICTL.CFG
 * invalidates both caches, and the main image's CPLB set-up sets them again
 * from 0. The instruction pages cover only the stub's region.
 */
#include "unpack.h"

#include <stddef.h>

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_coremmr.h"
#include "waffle/drivers/bf70x_cplb.h"
#include "waffle/drivers/mmr.h"

static uint8_t *dest(void *ctx, uint32_t addr, uint32_t len)
{
	(void)ctx;
	return wb_unpack_writable(addr, len) ? (uint8_t *)(uintptr_t)addr : 0;
}

static uint64_t arena[WB_UNPACK_ARENA / 8u];

#define D_RW (CPLB_VALID | DCPLB_SWRITE | DCPLB_DIRTY)
#define I_CACHE(sz) (CPLB_VALID | ICPLB_CACHE | WB_CPLB_PSIZE(sz))
#define CTL_CACHE_OFF_D (L1DM_DCTL_ENCPLB | L1DM_DCTL_CFG_MASK)
#define CTL_CACHE_OFF_I (L1IM_ICTL_ENCPLB | L1IM_ICTL_CFG)
#define STUB_CTL __attribute__((section(".text.stub_ctl"), noinline))

/* The stub's region: its first 1 KiB page, which holds the code below,
 * uncached; the rest cached, in naturally aligned pages. */
static const struct wb_cplb icplb[] = {
	{ 0x08031000u, CPLB_VALID | ICPLB_NOCACHE | WB_CPLB_PSIZE(WB_CPLB_1K) },
	{ 0x08031400u, I_CACHE(WB_CPLB_1K) },
	{ 0x08031800u, I_CACHE(WB_CPLB_1K) },
	{ 0x08031C00u, I_CACHE(WB_CPLB_1K) },
	{ 0x08032000u, I_CACHE(WB_CPLB_4K) },
	{ 0x08033000u, I_CACHE(WB_CPLB_4K) },
	{ 0x08034000u, I_CACHE(WB_CPLB_16K) },
	{ 0x08038000u, I_CACHE(WB_CPLB_16K) },
};
_Static_assert(0x08038000u + 0x4000u == WB_STUB_END, "the pages cover the stub's region");

/* L2 and DDR, write-through: all the stub reads and writes. */
static const struct wb_cplb dcplb[] = {
	{ 0x08000000u, D_RW | DCPLB_WT | WB_CPLB_PSIZE(WB_CPLB_256K) },
	{ 0x80000000u, D_RW | DCPLB_WT | WB_CPLB_PSIZE(WB_CPLB_256M) },
};

/* All sixteen pairs of one side: n entries, then invalid ones, so that no
 * entry the first stage left can match an address twice. */
static STUB_CTL void entries(uint32_t addr0, uint32_t data0, const struct wb_cplb *t,
			     unsigned n)
{
	unsigned i;

	for (i = 0; i < 16u; i++) {
		wb_mmr_write32(data0 + 4u * i, i < n ? t[i].data : 0u);
		wb_mmr_write32(addr0 + 4u * i, i < n ? t[i].addr : 0u);
	}
}

/* Each side with its CPLBs off: SSYNC, CFG set (the tags initialized), the
 * pages, CSYNC, ENCPLB set, SSYNC. */
static STUB_CTL void caches_on(void)
{
	wb_mmr_sync();
	wb_mmr_write32(L1IM_ICTL, (wb_mmr_read32(L1IM_ICTL) & ~CTL_CACHE_OFF_I) | L1IM_ICTL_CFG);
	wb_mmr_sync();
	entries(L1IM_ICPLB_ADDR(0), L1IM_ICPLB_DATA(0), icplb, sizeof icplb / sizeof icplb[0]);
	wb_core_csync();
	wb_mmr_write32(L1IM_ICTL, wb_mmr_read32(L1IM_ICTL) | L1IM_ICTL_ENCPLB);
	wb_mmr_sync();
	wb_mmr_write32(L1DM_DCTL, (wb_mmr_read32(L1DM_DCTL) & ~CTL_CACHE_OFF_D) | L1DM_DCTL_CFG_AB);
	wb_mmr_sync();
	entries(L1DM_DCPLB_ADDR(0), L1DM_DCPLB_DATA(0), dcplb, sizeof dcplb / sizeof dcplb[0]);
	wb_core_csync();
	wb_mmr_write32(L1DM_DCTL, wb_mmr_read32(L1DM_DCTL) | L1DM_DCTL_ENCPLB);
	wb_mmr_sync();
}

/* Both off again, as the front left them; every write has reached memory. */
static STUB_CTL void caches_off(void)
{
	wb_mmr_sync();
	wb_mmr_write32(L1DM_DCTL, wb_mmr_read32(L1DM_DCTL) & ~CTL_CACHE_OFF_D);
	wb_mmr_sync();
	wb_mmr_write32(L1IM_ICTL, wb_mmr_read32(L1IM_ICTL) & ~CTL_CACHE_OFF_I);
	wb_mmr_sync();
	wb_core_csync();
}

extern char wb_stub_bss_start[], wb_stub_bss_end[];

uint32_t wb_stub_main(void);
uint32_t wb_stub_main(void)
{
	volatile struct wb_stub_record *rec = (volatile struct wb_stub_record *)(uintptr_t)WB_STUB_RECORD;
	const struct wb_unpack_ops ops = { dest, 0, arena, sizeof arena };
	enum wb_unpack_rc rc;
	uint32_t entry = 0;
	size_t i, n;

	rec->entered = wb_core_cycles();
	rec->unpacked = 0u;
	rec->result = WB_STUB_RUNNING;
	rec->magic = WB_STUB_RECORD_MAGIC;
	n = (size_t)((uintptr_t)wb_stub_bss_end - (uintptr_t)wb_stub_bss_start);
	for (i = 0; i < n; i++)
		wb_stub_bss_start[i] = 0;
	caches_on();
	rc = wb_unpack((const uint8_t *)(uintptr_t)WB_PACK_START, WB_PACK_END - WB_PACK_START,
		       &ops, &entry);
	caches_off();
	rec->result = (uint32_t)rc;
	if (rc != WB_UNPACK_OK)
		return 0u;		/* stub_entry.S stops: an unfinished start (S-02.095) */
	rec->unpacked = wb_core_cycles();
	return entry;
}
