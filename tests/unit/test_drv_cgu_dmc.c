/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tests for the CGU and DMC drivers' checks (src/drivers/bf70x/cgu.c,
 * dmc.c), with this board's values from S-13a.001 to .016 as the
 * configuration. The sequence that sets them is the front's
 * (tests/unit/test_front_init.c).
 */
#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x_cgu.h"
#include "waffle/drivers/bf70x_dmc.h"

/* S-13a.002, .003, .005 for CLKIN = 25 MHz. */
static const struct wb_cgu_config s13a_cgu = {
	.clkin_hz = 25000000u,
	.msel = 32, .df = false,
	.csel = 2, .syssel = 4, .s0sel = 2, .s1sel = 1, .dsel = 4, .osel = 16,
	.clkoutsel = 1,
	.hold_ddr_phy = true,
	.limits = { 0 },
};

/* S-13a.010 to .016. */
static const struct wb_dmc_config s13a_dmc = {
	.phy_ctl4 = 0x00000001u,
	.phy_ctl3 = 0x0A0000C0u,
	.cal_padctl0 = 0xE0000000u,
	.cal_padctl2 = 0x0078283Cu,
	.dllctl = 0x0000054Bu,
	.cfg = 0x00000522u,
	.tr0 = 0x20B08323u,
	.tr1 = 0x20270618u,
	.tr2 = 0x00323209u,
	.mr = 0x00000432u,
	.emr1 = 0,
	.emr2 = 0,
	.ctl = 0x00002400u,
	.dclk_mhz = 200u,
};

#define DIV_S13A 0x04042442u

static void reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
}

static void test_values_and_freqs(void)
{
	struct wb_cgu_freqs f;
	struct wb_cgu_config c = s13a_cgu;

	CHECK(wb_cgu_div_value(&s13a_cgu) == DIV_S13A);
	CHECK(wb_cgu_ctl_value(&s13a_cgu) == 0x2000u);
	CHECK(wb_cgu_freqs(&s13a_cgu, &f) == WB_CGU_OK);
	/* S-13a.001. */
	CHECK(f.vco == 800000000u && f.cclk == 400000000u);
	CHECK(f.sysclk == 200000000u && f.sclk0 == 100000000u);
	CHECK(f.sclk1 == 200000000u && f.dclk == 200000000u);
	CHECK(f.oclk == 50000000u);

	c.msel = 128;
	CHECK(wb_cgu_ctl_value(&c) == 0);	/* MSEL 0 means 128 */
	c.df = true;
	CHECK(wb_cgu_ctl_value(&c) == CGU_CTL_DF);

	c = s13a_cgu;
	c.limits.cclk_max = 399999999u;
	CHECK(wb_cgu_freqs(&c, &f) == WB_CGU_ERR_LIMIT);
	c.limits.cclk_max = 400000000u;
	c.limits.vco_min = 800000001u;
	CHECK(wb_cgu_freqs(&c, NULL) == WB_CGU_ERR_LIMIT);
	c = s13a_cgu;
	c.s0sel = 8;
	CHECK(wb_cgu_freqs(&c, NULL) == WB_CGU_ERR_ARG);
	c = s13a_cgu;
	c.msel = 0;
	CHECK(wb_cgu_freqs(&c, NULL) == WB_CGU_ERR_ARG);
	c = s13a_cgu;
	c.clkin_hz = 0;
	CHECK(wb_cgu_freqs(&c, NULL) == WB_CGU_ERR_ARG);
}

static void test_dmc_check(void)
{
	struct wb_dmc_config c = s13a_dmc;

	CHECK(wb_dmc_check(&s13a_dmc) == WB_DMC_OK);
	c.cfg = 0x00000511u;			/* 8-bit interface */
	CHECK(wb_dmc_check(&c) == WB_DMC_ERR_ARG);
	c = s13a_dmc;
	c.ctl = 0;				/* RDTOWR 0 */
	CHECK(wb_dmc_check(&c) == WB_DMC_ERR_ARG);
	c = s13a_dmc;
	c.ctl &= ~DMC_CTL_DLLCAL;		/* S-13a.018 step 4 */
	CHECK(wb_dmc_check(&c) == WB_DMC_ERR_ARG);
	c = s13a_dmc;
	c.ctl |= DMC_CTL_INIT;			/* the sequence's */
	CHECK(wb_dmc_check(&c) == WB_DMC_ERR_ARG);
	c = s13a_dmc;
	c.ctl |= DMC_CTL_SRREQ;
	CHECK(wb_dmc_check(&c) == WB_DMC_ERR_ARG);
	c = s13a_dmc;
	c.dllctl = 0x44Bu;			/* DATACYC 4 */
	CHECK(wb_dmc_check(&c) == WB_DMC_ERR_ARG);
	c = s13a_dmc;
	c.phy_ctl4 = 3u;			/* LPDDR */
	CHECK(wb_dmc_check(&c) == WB_DMC_ERR_ARG);
	c = s13a_dmc;
	c.cal_padctl0 |= DMC_CAL_CALSTRT;
	CHECK(wb_dmc_check(&c) == WB_DMC_ERR_ARG);
	c = s13a_dmc;
	c.dclk_mhz = 0;
	CHECK(wb_dmc_check(&c) == WB_DMC_ERR_ARG);
}

#define DDR_BASE  0x80000000u
#define DDR_ALIAS 0x84000000u

/* A 64 MB device: DDR_ALIAS is DDR_BASE. */
static bool alias_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		       wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (addr != DDR_ALIAS)
		return true;
	if (op == FAKE_MMR_READ) {
		*value = fake_mmr_get(DDR_BASE);
		return true;
	}
	fake_mmr_set(DDR_BASE, *value);
	return false;
}

static void test_alias_check(void)
{
	reset();
	fake_mmr_set(DDR_BASE, 0x11111111u);
	fake_mmr_set(DDR_ALIAS, 0x22222222u);
	CHECK(!wb_dmc_aliases(DDR_BASE, DDR_ALIAS));
	CHECK(fake_mmr_get(DDR_BASE) == 0x11111111u);
	CHECK(fake_mmr_get(DDR_ALIAS) == 0x22222222u);
	CHECK(fake_core_flushinvs() == 1);

	reset();
	fake_mmr_set(DDR_BASE, 0x33333333u);
	fake_mmr_set_hook(alias_hook, NULL);
	CHECK(wb_dmc_aliases(DDR_BASE, DDR_ALIAS));
	CHECK(fake_mmr_get(DDR_BASE) == 0x33333333u);
}

static void test_part_limits(void)
{
	/* The data sheet's Tables 17-19 against S-13a.001's clocks. */
	struct wb_cgu_part part = { 400000000u, true, WB_CGU_DMC_DDR2 };
	struct wb_cgu_config c = s13a_cgu;
	const struct wb_cgu_limits ds = WB_CGU_LIMITS_DS(400000000u);

	c.limits = ds;
	CHECK(wb_cgu_freqs(&c, NULL) == WB_CGU_OK);
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_OK);

	part.cclk_grade_hz = 300000000u;	/* a 300 MHz part */
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_ERR_LIMIT);
	c.csel = 3;			/* 266.7 MHz */
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_OK);
	part.cclk_grade_hz = 400000000u;

	c = s13a_cgu;
	c.msel = 24;			/* PLLCLK 600: SYSCLK up to 195 */
	c.syssel = 3; c.dsel = 3; c.osel = 12;	/* SYSCLK 200 */
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_ERR_LIMIT);
	c.syssel = 4; c.dsel = 4;	/* SYSCLK 150 */
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_OK);
	c = s13a_cgu;
	c.clkin_hz = 12500000u;
	c.msel = 41;			/* PLLCLK 512.5 */
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_OK);
	c.msel = 42;			/* MSEL above 41, PLLCLK 525 */
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_ERR_LIMIT);
	c = s13a_cgu;
	c.dsel = 8;			/* DCLK 100 MHz: below DDR2's 125 */
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_ERR_LIMIT);
	part.dmc = WB_CGU_DMC_LPDDR;
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_OK);
	part.dmc = WB_CGU_DMC_DDR2;
	c = s13a_cgu;
	c.syssel = 2;			/* SYSCLK 400 */
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_ERR_LIMIT);
	c = s13a_cgu;
	c.s0sel = 1;			/* SCLK0 200 */
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_ERR_LIMIT);
	c = s13a_cgu;
	c.syssel = 16; c.s0sel = 1; c.s1sel = 1; c.dsel = 16;	/* SYSCLK 50 */
	part.dmc = WB_CGU_DMC_NONE;
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_ERR_LIMIT);	/* USB */
	part.usb = false;
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_OK);
	c.osel = 8;			/* OCLK 100 */
	CHECK(wb_cgu_check_part(&c, &part) == WB_CGU_ERR_LIMIT);
}

int main(void)
{
	RUN(test_values_and_freqs);
	RUN(test_part_limits);
	RUN(test_dmc_check);
	RUN(test_alias_check);
	return check_summary("test_drv_cgu_dmc");
}
