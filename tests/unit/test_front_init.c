/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's INIT stage (src/front/fr_init.c), the code that sets the
 * clocks and DDR: S-13a.030, the cycle counter is started before the
 * stage's first CYCLES read, with the CGU and DMC answering as a unit at
 * reset does; the clock and DDR values it writes are the board's (opxy_cgu,
 * opxy_dmc), which the drivers' checks hold to the data sheet and the HRM
 * (test_board_values.c); and the order and waits of S-13a.008, .009, .018,
 * with no wait on DMC0_STAT.IDLE.
 */
#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/board/opxy.h"
#include "waffle/drivers/bf70x_cgu.h"
#include "waffle/drivers/bf70x_dmc.h"
#include "waffle/drivers/bf70x_rcu.h"
#include "waffle/drivers/bf70x_spi.h"

/* The PLL locked with no error, the DMC idle and calibrated. */
static bool unit_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		      wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op != FAKE_MMR_READ)
		return true;
	if (addr == CGU0_STAT)
		*value = CGU_STAT_PLLEN | CGU_STAT_PLOCK;
	else if (addr == DMC0_STAT)
		*value = DMC_STAT_IDLE | DMC_STAT_INITDONE |
			 DMC_STAT_DLLCALDONE;
	return true;
}

/* src/front/fr_init.c */
void fr_init_stage(uint32_t rom_config);

static void run(uint32_t bmode)
{
	fake_mmr_reset();
	fake_core_reset();
	fake_mmr_set(RCU0_STAT, bmode << 8);
	fake_mmr_set_hook(unit_hook, NULL);
	fr_init_stage(0);
}

static void test_counter_started_first(void)
{
	/* The slave: SPI slave boot (BMODE 2), no first stage before it. */
	run(2u);
	CHECK(fake_core_cycle_starts() == 1);
	CHECK(fake_core_early_cycle_reads() == 0);
	CHECK(fake_mmr_get(SPI_CLK(2u)) == 0);

	/* The master: SPI master boot, SPI2_CLK = 1 at the end. */
	run(1u);
	CHECK(fake_core_cycle_starts() == 1);
	CHECK(fake_core_early_cycle_reads() == 0);
	CHECK(fake_mmr_get(SPI_CLK(2u)) == 1u);
	CHECK(fake_mmr_errors() == 0);
}

/* The registers end as the full bootloader's values set them. */
static void test_values(void)
{
	run(1u);
	CHECK((fake_mmr_get(CGU0_DIV) & CGU_DIV_FIELDS) == wb_cgu_div_value(&opxy_cgu));
	CHECK((fake_mmr_get(CGU0_CTL) & (CGU_CTL_MSEL_MASK | CGU_CTL_DF)) ==
	      wb_cgu_ctl_value(&opxy_cgu));
	CHECK(fake_mmr_get(CGU0_CLKOUTSEL) == opxy_cgu.clkoutsel);
	CHECK(fake_mmr_get(DMC0_CFG) == opxy_dmc.cfg);
	CHECK(fake_mmr_get(DMC0_TR0) == opxy_dmc.tr0);
	CHECK(fake_mmr_get(DMC0_TR1) == opxy_dmc.tr1);
	CHECK(fake_mmr_get(DMC0_TR2) == opxy_dmc.tr2);
	CHECK(fake_mmr_get(DMC0_MR) == opxy_dmc.mr);
	CHECK(fake_mmr_get(DMC0_DLLCTL) == opxy_dmc.dllctl);
	CHECK(fake_mmr_get(DMC0_PHY_CTL3) == opxy_dmc.phy_ctl3);
	CHECK(fake_mmr_get(DMC0_PHY_CTL4) == opxy_dmc.phy_ctl4);
	CHECK(fake_mmr_get(DMC0_CAL_PADCTL2) == opxy_dmc.cal_padctl2);
	CHECK((fake_mmr_get(DMC0_CTL) & ~DMC_CTL_INIT) == opxy_dmc.ctl);
	CHECK((fake_mmr_get(DMC0_CAL_PADCTL0) & ~DMC_CAL_CALSTRT) == opxy_dmc.cal_padctl0);
	CHECK((fake_mmr_get(WB_DMC0_PHY_CTL0) & WB_DMC_PHY_RESETDLL) == 0u);	/* released */
	CHECK(opxy_cgu.hold_ddr_phy);

	/* EMR1 and EMR2 are written, not left as they were. */
	fake_mmr_reset();
	fake_core_reset();
	fake_mmr_set(DMC0_EMR1, 0xFFFFFFFFu);
	fake_mmr_set(DMC0_EMR2, 0xFFFFFFFFu);
	fake_mmr_set(RCU0_STAT, 1u << 8);
	fake_mmr_set_hook(unit_hook, NULL);
	fr_init_stage(0);
	CHECK(fake_mmr_get(DMC0_EMR1) == opxy_dmc.emr1);
	CHECK(fake_mmr_get(DMC0_EMR2) == opxy_dmc.emr2);
}

static bool is_dmc(wb_mmr_addr a)
{
	return a >= DMC0_BASE && a < DMC0_BASE + 0x2000u && a != WB_DMC0_PHY_CTL0;
}

static bool is_cgu(wb_mmr_addr a)
{
	return a >= CGU0_BASE && a < CGU0_BASE + 0x40u;
}

/* S-13a.018, .019: the DDR2 power-up sequence, every write the board's value. */
static void test_dmc_sequence(void)
{
	const struct fake_mmr_access want[] = {
		FAKE_W32(DMC0_PHY_CTL4, opxy_dmc.phy_ctl4),
		FAKE_W32(DMC0_PHY_CTL3, opxy_dmc.phy_ctl3),
		FAKE_W32(DMC0_CAL_PADCTL2, opxy_dmc.cal_padctl2),
		FAKE_W32(DMC0_CAL_PADCTL0, opxy_dmc.cal_padctl0 | DMC_CAL_CALSTRT),
		FAKE_W32(DMC0_DLLCTL, opxy_dmc.dllctl),
		FAKE_W32(DMC0_CFG, opxy_dmc.cfg),
		FAKE_W32(DMC0_TR0, opxy_dmc.tr0),
		FAKE_W32(DMC0_TR1, opxy_dmc.tr1),
		FAKE_W32(DMC0_TR2, opxy_dmc.tr2),
		FAKE_W32(DMC0_MR, opxy_dmc.mr),
		FAKE_W32(DMC0_EMR1, opxy_dmc.emr1),
		FAKE_W32(DMC0_EMR2, opxy_dmc.emr2),
		/* S-13a.018 step 4: INIT and DLLCAL in one write. */
		FAKE_W32(DMC0_CTL, 0x00002404u),
	};
	const struct fake_mmr_access *log;
	size_t i, k = 0, first = 0, last = 0, initdone = 0;

	run(1u);
	log = fake_mmr_log();
	for (i = 0; i < fake_mmr_count(); i++) {
		const struct fake_mmr_access *a = &log[i];

		if (a->op == FAKE_MMR_READ && a->addr == DMC0_STAT) {
			/* S-13a.018: DMC0_STAT is read only for INITDONE */
			CHECK(k == ARRAY_LEN(want));
			if (k == ARRAY_LEN(want) && (a->value & DMC_STAT_INITDONE) && !initdone)
				initdone = i + 1u;
		}
		if (a->op != FAKE_MMR_WRITE || !is_dmc(a->addr))
			continue;
		if (k == 0)
			first = i + 1u;
		last = i + 1u;
		CHECK(k < ARRAY_LEN(want));
		if (k < ARRAY_LEN(want)) {
			CHECK(a->addr == want[k].addr);
			CHECK(a->value == want[k].value);
		}
		k++;
	}
	CHECK(k == ARRAY_LEN(want));
	CHECK(first != 0u);
	CHECK(initdone > last);				/* INITDONE after */
}

/* S-13a.008: the DDR PHY's DLL held across the clock change (anomaly
 * 19000051) and released before the DMC is set up. S-13a.009: a new
 * multiplier writes CGU0_DIV, then CGU0_CTL. */
static void test_cgu_sequence(void)
{
	const struct fake_mmr_access *log;
	size_t i, held = 0, released = 0, div = 0, ctl = 0, cgu_last = 0, dmc_first = 0;

	run(1u);
	log = fake_mmr_log();
	for (i = 0; i < fake_mmr_count(); i++) {
		const struct fake_mmr_access *a = &log[i];

		if (a->op != FAKE_MMR_WRITE)
			continue;
		if (a->addr == WB_DMC0_PHY_CTL0) {
			if ((a->value & WB_DMC_PHY_RESETDLL) && !held)
				held = i + 1u;
			if (!(a->value & WB_DMC_PHY_RESETDLL) && held && !released)
				released = i + 1u;
		}
		if (a->addr == CGU0_DIV && !div)
			div = i + 1u;
		if (a->addr == CGU0_CTL && !ctl)
			ctl = i + 1u;
		if (is_cgu(a->addr))
			cgu_last = i + 1u;
		if (is_dmc(a->addr) && !dmc_first)
			dmc_first = i + 1u;
	}
	CHECK(held != 0u && held < div && div < ctl && ctl < cgu_last);
	CHECK(released > cgu_last && released < dmc_first);
	CHECK(fake_mmr_get(CGU0_DIV) == wb_cgu_div_value(&opxy_cgu));
	CHECK((fake_mmr_get(CGU0_DIV) & CGU_DIV_UPDT) == 0u);

	/* The multiplier already set: the dividers alone, with UPDT. */
	fake_mmr_reset();
	fake_core_reset();
	fake_mmr_set(CGU0_CTL, wb_cgu_ctl_value(&opxy_cgu));
	fake_mmr_set(RCU0_STAT, 1u << 8);
	fake_mmr_set_hook(unit_hook, NULL);
	fr_init_stage(0);
	CHECK(fake_mmr_get(CGU0_DIV) == (wb_cgu_div_value(&opxy_cgu) | CGU_DIV_UPDT));
	CHECK(fake_mmr_get(CGU0_CTL) == wb_cgu_ctl_value(&opxy_cgu));
}

static uint64_t t_cal, t_dll;

static bool stamp_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		       wb_mmr_addr addr, uint32_t *value)
{
	if (op == FAKE_MMR_WRITE && addr == DMC0_CAL_PADCTL0)
		t_cal = fake_core_peek_cycles();
	if (op == FAKE_MMR_WRITE && addr == DMC0_DLLCTL && t_dll == 0)
		t_dll = fake_core_peek_cycles();
	return unit_hook(ctx, op, width, addr, value);
}

/* S-13a.018: WB_DMC_DLLLOCK_DCLK at the board's DCLK, at the board's CCLK,
 * between CALSTRT and the DLLCTL write. */
static void test_dll_wait(void)
{
	const uint64_t want = (uint64_t)((WB_DMC_DLLLOCK_DCLK + opxy_dmc.dclk_mhz - 1u) /
					 opxy_dmc.dclk_mhz) * OPXY_CCLK_MHZ;

	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(50);
	fake_mmr_set(RCU0_STAT, 1u << 8);
	t_cal = t_dll = 0;
	fake_mmr_set_hook(stamp_hook, NULL);
	fr_init_stage(0);
	CHECK(t_cal != 0u && t_dll >= t_cal + want);
}

int main(void)
{
	RUN(test_counter_started_first);
	RUN(test_values);
	RUN(test_dmc_sequence);
	RUN(test_cgu_sequence);
	RUN(test_dll_wait);
	return check_summary("test_front_init");
}
