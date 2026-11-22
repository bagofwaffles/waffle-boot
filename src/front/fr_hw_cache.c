/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's L1 caches on the target (see fr_cache.h). Both functions run
 * from L1 code (.fr_fast), which is never cacheable, as the PRM requires of
 * the code that changes a cache's mode; interrupts are masked (fr_entry.S).
 * The sequence is the stub's (stub/stub.c) and wb_cplb_setup()'s: every
 * descriptor pair of a side written while its CPLBs are off.
 */
#include "fr_cache.h"

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_coremmr.h"
#include "waffle/drivers/mmr.h"

#define CTL_OFF_I (L1IM_ICTL_ENCPLB | L1IM_ICTL_CFG)
#define CTL_OFF_D (L1DM_DCTL_ENCPLB | L1DM_DCTL_CFG_MASK)

/* What fr_cache_on() found, for fr_cache_off(): each side's pairs. */
static bool on;
static uint32_t found_i[FR_CPLB_PAIRS][2], found_d[FR_CPLB_PAIRS][2];

/* All sixteen pairs of one side: the table's n entries, locked, then
 * invalid ones. */
FR_FAST static void pairs(wb_mmr_addr addr0, wb_mmr_addr data0, const struct wb_cplb *t,
			  unsigned n)
{
	for (unsigned i = 0; i < FR_CPLB_PAIRS; i++) {
		wb_mmr_write32(data0 + 4u * i, i < n ? t[i].data | CPLB_LOCK : 0u);
		wb_mmr_write32(addr0 + 4u * i, i < n ? t[i].addr : 0u);
	}
}

FR_FAST static void save(wb_mmr_addr addr0, wb_mmr_addr data0, uint32_t out[][2])
{
	for (unsigned i = 0; i < FR_CPLB_PAIRS; i++) {
		out[i][0] = wb_mmr_read32(data0 + 4u * i);
		out[i][1] = wb_mmr_read32(addr0 + 4u * i);
	}
}

/* cppcheck-suppress constParameter ; a const array arg trips ISO C's pointer-qualifier rule at the call */
FR_FAST static void restore(wb_mmr_addr addr0, wb_mmr_addr data0, uint32_t in[][2])
{
	for (unsigned i = 0; i < FR_CPLB_PAIRS; i++) {
		wb_mmr_write32(data0 + 4u * i, in[i][0]);
		wb_mmr_write32(addr0 + 4u * i, in[i][1]);
	}
}

/* The PRM's sequence for each side (Enabling L1 Instruction Cache, Enabling
 * L1 Data Cache), with its CPLBs off: SSYNC; CFG set (the tags invalid);
 * SSYNC; the pairs; CSYNC; ENCPLB set; SSYNC. */
FR_FAST void fr_cache_on(void)
{
	save(L1IM_ICPLB_ADDR(0), L1IM_ICPLB_DATA(0), found_i);
	save(L1DM_DCPLB_ADDR(0), L1DM_DCPLB_DATA(0), found_d);
	on = true;
	wb_mmr_sync();
	wb_mmr_write32(L1IM_ICTL, (wb_mmr_read32(L1IM_ICTL) & ~CTL_OFF_I) | L1IM_ICTL_CFG);
	wb_mmr_sync();
	pairs(L1IM_ICPLB_ADDR(0), L1IM_ICPLB_DATA(0), fr_icplb, FR_ICPLB_N);
	wb_core_csync();
	wb_mmr_write32(L1IM_ICTL, wb_mmr_read32(L1IM_ICTL) | L1IM_ICTL_ENCPLB);
	wb_mmr_sync();
	wb_mmr_write32(L1DM_DCTL, (wb_mmr_read32(L1DM_DCTL) & ~CTL_OFF_D) | L1DM_DCTL_CFG_A);
	wb_mmr_sync();
	pairs(L1DM_DCPLB_ADDR(0), L1DM_DCPLB_DATA(0), fr_dcplb, FR_DCPLB_N);
	wb_core_csync();
	wb_mmr_write32(L1DM_DCTL, wb_mmr_read32(L1DM_DCTL) | L1DM_DCTL_ENCPLB);
	wb_mmr_sync();
}

/* Both off, data first, as the stub's caches_off(); clearing CFG gives each
 * cache back as SRAM, its lines given up. Then the pairs as found, with the
 * CPLBs off. */
FR_FAST void fr_cache_off(void)
{
	if (!on)
		return;
	wb_mmr_sync();
	wb_mmr_write32(L1DM_DCTL, wb_mmr_read32(L1DM_DCTL) & ~CTL_OFF_D);
	wb_mmr_sync();
	wb_mmr_write32(L1IM_ICTL, wb_mmr_read32(L1IM_ICTL) & ~CTL_OFF_I);
	wb_mmr_sync();
	wb_core_csync();
	restore(L1IM_ICPLB_ADDR(0), L1IM_ICPLB_DATA(0), found_i);
	restore(L1DM_DCPLB_ADDR(0), L1DM_DCPLB_DATA(0), found_d);
	wb_mmr_sync();
	wb_core_csync();
	on = false;
}
