/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_CGU_H
#define WAFFLE_DRIVERS_BF70X_CGU_H

/*
 * Clock Generation Unit (ADSP-BF70x Hardware Reference chapter "Clock
 * Generation Unit (CGU)"): the registers, and checks of a board's clock
 * values. Every value comes from the board: CLKIN, the multiplier and
 * dividers, and the frequency limits. wb_cgu_check_part() adds the data
 * sheet's operating conditions for the part (ADSP-BF700/701/702/703/704/705/
 * 706/707 data sheet Rev. D, Tables 17 to 19). The clocks are set by the
 * front's INIT stage (src/front/start/init.c).
 */

#include <stdbool.h>
#include <stdint.h>

#define CGU0_BASE        0x20002000u
#define CGU0_CTL         (CGU0_BASE + 0x00u)
#define CGU0_PLLCTL      (CGU0_BASE + 0x04u)
#define CGU0_STAT        (CGU0_BASE + 0x08u)
#define CGU0_DIV         (CGU0_BASE + 0x0Cu)
#define CGU0_CLKOUTSEL   (CGU0_BASE + 0x10u)
#define CGU0_CCBF_DIS    (CGU0_BASE + 0x2Cu)
#define CGU0_SCBF_DIS    (CGU0_BASE + 0x38u)

/* CGU_CTL. MSEL 0 means 128. */
#define CGU_CTL_DF          (1u << 0)
#define CGU_CTL_MSEL_SHIFT  8u
#define CGU_CTL_MSEL_MASK   (0x7Fu << 8)
#define CGU_CTL_WFI         (1u << 30)
#define CGU_CTL_LOCK        (1u << 31)

/* CGU_DIV. */
#define CGU_DIV_CSEL_SHIFT    0u
#define CGU_DIV_S0SEL_SHIFT   5u
#define CGU_DIV_SYSSEL_SHIFT  8u
#define CGU_DIV_S1SEL_SHIFT   13u
#define CGU_DIV_DSEL_SHIFT    16u
#define CGU_DIV_OSEL_SHIFT    22u
#define CGU_DIV_FIELDS        0x1FDFFFFFu	/* every select field */
#define CGU_DIV_ALGN          (1u << 29)
#define CGU_DIV_UPDT          (1u << 30)
#define CGU_DIV_LOCK          (1u << 31)

/* CGU_STAT. The error bits are W1C. */
#define CGU_STAT_PLLEN      (1u << 0)
#define CGU_STAT_PLLBP      (1u << 1)
#define CGU_STAT_PLOCK      (1u << 2)
#define CGU_STAT_CLKSALGN   (1u << 3)
#define CGU_STAT_ADDRERR    (1u << 16)
#define CGU_STAT_LWERR      (1u << 17)
#define CGU_STAT_WDFMSERR   (1u << 19)
#define CGU_STAT_WDIVERR    (1u << 20)
#define CGU_STAT_PCFGERR    (1u << 21)
#define CGU_STAT_ERRORS     0x003B0000u

/*
 * DMC0_PHY_CTL0.RESETDLL: holds the DDR PHY's DLL (S-13a.008). Silicon
 * anomaly 19000051: the DLL may not lock to a new DCLK unless it is held
 * in reset while the CGU changes DCLK; hold_ddr_phy says a board does so.
 */
#define WB_DMC0_PHY_CTL0    0x200C1000u
#define WB_DMC_PHY_RESETDLL (1u << 11)

/* Upper limits in Hz; 0 leaves a limit unchecked. */
struct wb_cgu_limits {
	uint32_t vco_min, vco_max;
	uint32_t cclk_max, sysclk_max, sclk0_max, sclk1_max;
	uint32_t dclk_max, oclk_max;
};

struct wb_cgu_config {
	uint32_t clkin_hz;
	uint8_t msel;		/* 1 to 128 */
	bool df;		/* CLKIN / 2 into the PLL */
	uint8_t csel;		/* 1 to 31 */
	uint8_t syssel;		/* 1 to 31 */
	uint8_t s0sel;		/* 1 to 7 */
	uint8_t s1sel;		/* 1 to 7 */
	uint8_t dsel;		/* 1 to 31 */
	uint8_t osel;		/* 1 to 127 */
	uint8_t clkoutsel;	/* CGU0_CLKOUTSEL value, 0 to 15 */
	bool hold_ddr_phy;	/* hold DMC0_PHY_CTL0.RESETDLL across */
	struct wb_cgu_limits limits;
};

struct wb_cgu_freqs {
	uint32_t vco, cclk, sysclk, sclk0, sclk1, dclk, oclk;	/* Hz */
};

#define WB_CGU_OK          0
#define WB_CGU_ERR_ARG    (-1)	/* a field out of range */
#define WB_CGU_ERR_LIMIT  (-2)	/* a frequency over the board's limit */

/* CGU0_CTL and CGU0_DIV for a configuration (fields only). */
uint32_t wb_cgu_ctl_value(const struct wb_cgu_config *c);
uint32_t wb_cgu_div_value(const struct wb_cgu_config *c);

/* Check the fields and limits and compute the frequencies (f may be NULL). */
int wb_cgu_freqs(const struct wb_cgu_config *c, struct wb_cgu_freqs *f);

/*
 * The data sheet's limits for a struct wb_cgu_limits (Tables 17 to 19):
 * PLLCLK 230.2 to 800 MHz, CCLK up to the speed grade (300 or 400 MHz),
 * SYSCLK 200, SCLK0 100, SCLK1 200, DCLK 200 and OCLK 50 MHz.
 */
#define WB_CGU_LIMITS_DS(grade_hz) {                                     \
	230200000u, 800000000u, (grade_hz), 200000000u, 100000000u,      \
	200000000u, 200000000u, 50000000u }

#define WB_CGU_DMC_NONE   0u
#define WB_CGU_DMC_DDR2   1u
#define WB_CGU_DMC_LPDDR  2u

struct wb_cgu_part {
	uint32_t cclk_grade_hz;	/* 300000000 or 400000000 (ordering guide) */
	bool usb;		/* USB in use: SYSCLK >= 60, SCLK0 >= 30 MHz */
	uint8_t dmc;		/* WB_CGU_DMC_*: the DCLK range to check */
};

/*
 * wb_cgu_freqs() plus the data sheet's operating conditions: MSEL 8 to
 * 41; PLLCLK 230.2 to 800 MHz; the CCLK and SYSCLK maxima for the PLLCLK
 * band (400/200 at 800, 390/195 from 600, 380/190 from 380, else PLLCLK
 * and PLLCLK / 2), CCLK also within the speed grade; CCLK >= SYSCLK >=
 * SCLK0, SCLK1, DCLK; SCLK0 <= 100, SCLK1 <= 200, OCLK <= 50 MHz; and
 * DCLK 125 to 200 MHz for DDR2, 10 to 200 MHz for LPDDR.
 */
int wb_cgu_check_part(const struct wb_cgu_config *c,
		      const struct wb_cgu_part *p);

#endif /* WAFFLE_DRIVERS_BF70X_CGU_H */
