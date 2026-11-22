/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's L1 caches on the target (see fr_cache.h). Both functions run
 * from L1 code (.fr_fast), which is never cacheable, as the PRM requires of
 * the code that changes a cache's mode; interrupts are masked (fr_entry.S).
 */
#include "fr_cache.h"

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_coremmr.h"
#include "waffle/drivers/mmr.h"

/* What fr_cache_on() found, for fr_cache_off(). */
static bool on;
static uint32_t idflt, ddflt;
static uint32_t iaddr[FR_ICPLB_N], idata[FR_ICPLB_N], daddr[FR_DCPLB_N], ddata[FR_DCPLB_N];

/* The PRM's sequence for one side (Enabling L1 Instruction Cache, Enabling
 * L1 Data Cache): with its CPLBs off, SSYNC; CFG set (the tags invalid);
 * the default and the pages; CSYNC; ENCPLB set; SSYNC. */
FR_FAST static void side_on(wb_mmr_addr ctl, uint32_t cfg, wb_mmr_addr dflt, uint32_t dflt_val,
			    wb_mmr_addr addr0, wb_mmr_addr data0, const struct wb_cplb *t,
			    unsigned n, uint32_t encplb)
{
	wb_mmr_sync();
	wb_mmr_write32(ctl, wb_mmr_read32(ctl) | cfg);
	wb_mmr_write32(dflt, dflt_val);
	for (unsigned i = 0; i < n; i++) {
		wb_mmr_write32(data0 + 4u * i, t[i].data | CPLB_LOCK);
		wb_mmr_write32(addr0 + 4u * i, t[i].addr);
	}
	wb_core_csync();
	wb_mmr_write32(ctl, wb_mmr_read32(ctl) | encplb);
	wb_mmr_sync();
}

/* SSYNC; ENCPLB off; SSYNC; CFG off; SSYNC; CSYNC: the cache returned to
 * SRAM, its lines given up; a later CFG set initializes its tags. */
FR_FAST static void side_off(wb_mmr_addr ctl, uint32_t cfg, uint32_t encplb)
{
	wb_mmr_sync();
	wb_mmr_write32(ctl, wb_mmr_read32(ctl) & ~encplb);
	wb_mmr_sync();
	wb_mmr_write32(ctl, wb_mmr_read32(ctl) & ~cfg);
	wb_mmr_sync();
	wb_core_csync();
}

FR_FAST void fr_cache_on(void)
{
	idflt = wb_mmr_read32(L1IM_ICPLB_DFLT);
	ddflt = wb_mmr_read32(L1DM_DCPLB_DFLT);
	for (unsigned i = 0; i < FR_ICPLB_N; i++) {
		iaddr[i] = wb_mmr_read32(L1IM_ICPLB_ADDR(i));
		idata[i] = wb_mmr_read32(L1IM_ICPLB_DATA(i));
	}
	for (unsigned i = 0; i < FR_DCPLB_N; i++) {
		daddr[i] = wb_mmr_read32(L1DM_DCPLB_ADDR(i));
		ddata[i] = wb_mmr_read32(L1DM_DCPLB_DATA(i));
	}
	on = true;
	side_on(L1IM_ICTL, L1IM_ICTL_CFG, L1IM_ICPLB_DFLT, FR_ICPLB_DFLT, L1IM_ICPLB_ADDR(0),
		L1IM_ICPLB_DATA(0), fr_icplb, FR_ICPLB_N, L1IM_ICTL_ENCPLB);
	side_on(L1DM_DCTL, L1DM_DCTL_CFG_A, L1DM_DCPLB_DFLT, FR_DCPLB_DFLT, L1DM_DCPLB_ADDR(0),
		L1DM_DCPLB_DATA(0), fr_dcplb, FR_DCPLB_N, L1DM_DCTL_ENCPLB);
	wb_core_csync();
}

FR_FAST void fr_cache_off(void)
{
	if (!on)
		return;
	side_off(L1DM_DCTL, L1DM_DCTL_CFG_MASK, L1DM_DCTL_ENCPLB);
	side_off(L1IM_ICTL, L1IM_ICTL_CFG, L1IM_ICTL_ENCPLB);
	wb_mmr_write32(L1IM_ICPLB_DFLT, idflt);
	wb_mmr_write32(L1DM_DCPLB_DFLT, ddflt);
	for (unsigned i = 0; i < FR_ICPLB_N; i++) {
		wb_mmr_write32(L1IM_ICPLB_DATA(i), idata[i]);
		wb_mmr_write32(L1IM_ICPLB_ADDR(i), iaddr[i]);
	}
	for (unsigned i = 0; i < FR_DCPLB_N; i++) {
		wb_mmr_write32(L1DM_DCPLB_DATA(i), ddata[i]);
		wb_mmr_write32(L1DM_DCPLB_ADDR(i), daddr[i]);
	}
	wb_mmr_sync();
	wb_core_csync();
	on = false;
}
