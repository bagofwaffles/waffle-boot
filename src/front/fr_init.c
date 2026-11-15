/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The INIT stage (S-01.010, S-13a.018), the front's own copy: the clocks
 * (S-13a.001-.005 at CLKIN 25 MHz) and the DDR2 controller (S-13a.010-.016),
 * then SPI2_CLK = 1 in SPI master boot so the rest of the stream loads
 * faster. It and everything it reads are in L1 (front.ld): DDR does not work
 * yet.
 */
#include <stdbool.h>
#include <stdint.h>

#include "fr_handoff.h"
#include "waffle/drivers/bf70x_cgu.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_dmc.h"
#include "waffle/drivers/bf70x_rcu.h"
#include "waffle/drivers/bf70x_spi.h"
#include "waffle/drivers/mmr.h"

#if defined(__bfin__)
#define INIT_TEXT __attribute__((section(".fr_init_text")))
#define INIT_DATA __attribute__((section(".fr_init_data")))
#else
#define INIT_TEXT
#define INIT_DATA
#endif

/* Written by fr_entry.S: the ROM's SP and the first stage's L1DM_DCTL and
 * L1DM_DCPLB_DFLT at the call (S-01.007, S-01.021). */
INIT_DATA uint32_t fr_init_sp;
INIT_DATA uint32_t fr_init_dctl;
INIT_DATA uint32_t fr_init_dflt;
/* The counter at entry, after the PLL and after the DDR controller. */
INIT_DATA uint64_t fr_init_cycles[3];

/* Every wait is bounded; a bound run out spins (S-16.025). */
#define WAIT_US   100000u

/* S-13a.002, .003: MSEL 32 (fVCO 800 MHz), CCLK /2, SYSCLK /4, SCLK0 /2,
 * SCLK1 /1, DCLK /4, OCLK /16; S-13a.005: CLKOUT CCLK / 16. */
#define CGU_CTL_VALUE  (32u << CGU_CTL_MSEL_SHIFT)
#define CGU_DIV_VALUE  (2u << CGU_DIV_CSEL_SHIFT | 2u << CGU_DIV_S0SEL_SHIFT | \
			4u << CGU_DIV_SYSSEL_SHIFT | 1u << CGU_DIV_S1SEL_SHIFT | \
			4u << CGU_DIV_DSEL_SHIFT | 16u << CGU_DIV_OSEL_SHIFT)
#define CLKOUTSEL_VALUE 1u

/* S-13a.010-.016, the values opxy_values.c gives the full bootloader. */
#define DMC_PADCTL0  (DMC_CAL_PUCALEN | DMC_CAL_PDCALEN | DMC_CAL_RTTCALEN)
#define DMC_PADCTL2  0x0078283Cu
#define DMC_PHYCTL3  0x0A0000C0u
#define DMC_DLL      (0x4Bu | 5u << DMC_DLLCTL_DATACYC_SHIFT)
#define DMC_CFG_VAL  (2u << DMC_CFG_IFWID_SHIFT | 2u << DMC_CFG_SDRWID_SHIFT | \
		      5u << DMC_CFG_SDRSIZE_SHIFT)
#define DMC_TR0_VAL  0x20B08323u
#define DMC_TR1_VAL  0x20270618u
#define DMC_TR2_VAL  0x00323209u
#define DMC_MR_VAL   0x00000432u
/* S-13a.018 step 4: the start write also asks for the DLL calibration. */
#define DMC_CTL_VAL  (2u << DMC_CTL_RDTOWR_SHIFT | DMC_CTL_DLLCAL)
#define DCLK_MHZ     200u

/* RCU0_STAT.BMODE (bits 11:8): 1 is SPI master boot. */
#define BMODE(v)          (((v) >> 8) & 0xFu)
#define BMODE_SPI_MASTER  1u

static INIT_TEXT void stop(void)
{
	for (;;)
		;
}

/* The counter's value in us cycles from now, at the reset clock's rate or
 * the PLL's: either way no shorter than asked at the final rate. */
static INIT_TEXT uint64_t deadline(uint32_t us)
{
	return wb_core_cycles() + (uint64_t)us * FR_CCLK_MHZ;
}

static INIT_TEXT void poll(uint32_t addr, uint32_t mask, uint32_t value)
{
	const uint64_t end = deadline(WAIT_US);

	for (;;) {
		const bool late = wb_core_cycles() >= end;

		if ((wb_mmr_read32(addr) & mask) == value)
			return;
		if (late)
			stop();
	}
}

static INIT_TEXT void cgu_wait(void)
{
	poll(CGU0_STAT, CGU_STAT_PLLEN | CGU_STAT_PLLBP | CGU_STAT_PLOCK |
		     CGU_STAT_CLKSALGN, CGU_STAT_PLLEN | CGU_STAT_PLOCK);
}

/* wb_cgu_init()'s steps with opxy_cgu's values (anomaly 19000051: the DLL
 * held across the DCLK change). */
static INIT_TEXT void clocks(void)
{
	uint32_t stat;

	wb_mmr_write32(WB_DMC0_PHY_CTL0,
		       wb_mmr_read32(WB_DMC0_PHY_CTL0) | WB_DMC_PHY_RESETDLL);
	stat = wb_mmr_read32(CGU0_STAT);
	if ((stat & CGU_STAT_PLLEN) == 0u || (stat & CGU_STAT_PLLBP) != 0u)
		stop();
	cgu_wait();
	if ((wb_mmr_read32(CGU0_CTL) & (CGU_CTL_MSEL_MASK | CGU_CTL_DF)) !=
	    CGU_CTL_VALUE) {
		wb_mmr_write32(CGU0_DIV, CGU_DIV_VALUE);
		wb_mmr_write32(CGU0_CTL, CGU_CTL_VALUE);
	} else if ((wb_mmr_read32(CGU0_DIV) & CGU_DIV_FIELDS) != CGU_DIV_VALUE) {
		cgu_wait();
		wb_mmr_write32(CGU0_DIV, CGU_DIV_VALUE | CGU_DIV_UPDT);
	}
	cgu_wait();
	wb_mmr_write32(CGU0_CLKOUTSEL, CLKOUTSEL_VALUE);
	stat = wb_mmr_read32(CGU0_STAT);
	if (stat & CGU_STAT_ERRORS)
		stop();
	wb_mmr_write32(WB_DMC0_PHY_CTL0,
		       wb_mmr_read32(WB_DMC0_PHY_CTL0) & ~WB_DMC_PHY_RESETDLL);
	wb_mmr_sync();
}

/* wb_dmc_init()'s steps with opxy_dmc's values. S-13a.018: no wait on
 * DMC0_STAT.IDLE, before the programming or anywhere. */
static INIT_TEXT void ddr(void)
{
	uint64_t end;

	wb_mmr_write32(DMC0_PHY_CTL4, DMC_PHY_CTL4_DDR2);
	wb_mmr_write32(DMC0_PHY_CTL3, DMC_PHYCTL3);
	wb_mmr_write32(DMC0_CAL_PADCTL2, DMC_PADCTL2);
	wb_mmr_write32(DMC0_CAL_PADCTL0, DMC_PADCTL0 | DMC_CAL_CALSTRT);
	wb_mmr_sync();
	/* The DLL lock time covers the pad calibration too. */
	end = deadline((WB_DMC_DLLLOCK_DCLK + DCLK_MHZ - 1u) / DCLK_MHZ);
	while (wb_core_cycles() < end)
		;
	wb_mmr_write32(DMC0_DLLCTL, DMC_DLL);
	wb_mmr_write32(DMC0_CFG, DMC_CFG_VAL);
	wb_mmr_write32(DMC0_TR0, DMC_TR0_VAL);
	wb_mmr_write32(DMC0_TR1, DMC_TR1_VAL);
	wb_mmr_write32(DMC0_TR2, DMC_TR2_VAL);
	wb_mmr_write32(DMC0_MR, DMC_MR_VAL);
	wb_mmr_write32(DMC0_EMR1, 0u);
	wb_mmr_write32(DMC0_EMR2, 0u);
	wb_mmr_write32(DMC0_CTL, DMC_CTL_VAL | DMC_CTL_INIT);
	wb_mmr_sync();
	poll(DMC0_STAT, DMC_STAT_INITDONE, DMC_STAT_INITDONE);
}

/* cppcheck-suppress unusedFunction ; fr_entry.S calls it */
INIT_TEXT void fr_init_stage(uint32_t rom_config);
INIT_TEXT void fr_init_stage(uint32_t rom_config)
{
	(void)rom_config;
	/* S-13a.030: the slave enters with the counter stopped. */
	wb_core_cycles_start();
	fr_init_cycles[0] = wb_core_cycles();
	clocks();
	fr_init_cycles[1] = wb_core_cycles();
	ddr();
	fr_init_cycles[2] = wb_core_cycles();
	if (BMODE(wb_mmr_read32(RCU0_STAT)) == BMODE_SPI_MASTER) {
		wb_mmr_write32(SPI_CLK(2u), 1u);
		wb_mmr_sync();
	}
}
