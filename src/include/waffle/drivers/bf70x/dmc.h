/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_DMC_H
#define WAFFLE_DRIVERS_BF70X_DMC_H

/*
 * Dynamic Memory Controller, DDR2 (ADSP-BF70x Hardware Reference chapter
 * "Dynamic Memory Controller (DMC)"): the registers, a check of the fields
 * the HRM fixes for this part, and a check of the device's size. The board
 * supplies every register value (S-13a.010 to .016 for this board); the
 * front's INIT stage (src/front/start/init.c) runs the HRM's power-up sequence
 * (S-13a.018, .019) with them.
 */

#include <stdbool.h>
#include <stdint.h>

#define DMC0_BASE         0x200C0000u
#define DMC0_CTL          (DMC0_BASE + 0x004u)
#define DMC0_STAT         (DMC0_BASE + 0x008u)
#define DMC0_CFG          (DMC0_BASE + 0x040u)
#define DMC0_TR0          (DMC0_BASE + 0x044u)
#define DMC0_TR1          (DMC0_BASE + 0x048u)
#define DMC0_TR2          (DMC0_BASE + 0x04Cu)
#define DMC0_MR           (DMC0_BASE + 0x060u)
#define DMC0_EMR1         (DMC0_BASE + 0x064u)
#define DMC0_EMR2         (DMC0_BASE + 0x068u)
#define DMC0_DLLCTL       (DMC0_BASE + 0x080u)
#define DMC0_PHY_CTL0     (DMC0_BASE + 0x1000u)
#define DMC0_PHY_CTL1     (DMC0_BASE + 0x1004u)
#define DMC0_PHY_CTL3     (DMC0_BASE + 0x100Cu)
#define DMC0_PHY_CTL4     (DMC0_BASE + 0x1010u)
#define DMC0_CAL_PADCTL0  (DMC0_BASE + 0x1034u)
#define DMC0_CAL_PADCTL2  (DMC0_BASE + 0x103Cu)

/* DMC_CTL. */
#define DMC_CTL_LPDDR       (1u << 1)
#define DMC_CTL_INIT        (1u << 2)
#define DMC_CTL_SRREQ       (1u << 3)
#define DMC_CTL_PDREQ       (1u << 4)
#define DMC_CTL_DPDREQ      (1u << 5)
#define DMC_CTL_PREC        (1u << 6)
#define DMC_CTL_ADDRMODE    (1u << 8)
#define DMC_CTL_RDTOWR_SHIFT 9u
#define DMC_CTL_RDTOWR_MASK (7u << 9)
#define DMC_CTL_PPREF       (1u << 12)
#define DMC_CTL_DLLCAL      (1u << 13)

/* DMC_STAT. */
#define DMC_STAT_IDLE       (1u << 0)
#define DMC_STAT_INITDONE   (1u << 2)
#define DMC_STAT_SRACK      (1u << 3)
#define DMC_STAT_PDACK      (1u << 4)
#define DMC_STAT_DPDACK     (1u << 5)
#define DMC_STAT_DLLCALDONE (1u << 13)

/* DMC_CFG. */
#define DMC_CFG_IFWID_SHIFT   0u
#define DMC_CFG_SDRWID_SHIFT  4u
#define DMC_CFG_SDRSIZE_SHIFT 8u
#define DMC_CFG_EXTBANK_SHIFT 12u

/* DMC_DLLCTL. */
#define DMC_DLLCTL_DATACYC_SHIFT 8u

/* DMC_PHY_CTL0, DMC_PHY_CTL4, DMC_CAL_PADCTL0. */
#define DMC_PHY_CTL0_RESETDLL (1u << 11)
#define DMC_PHY_CTL0_RESETDAT (1u << 12)
#define DMC_PHY_CTL4_DDR2     1u
#define DMC_CAL_CALSTRT       (1u << 28)
#define DMC_CAL_PUCALEN       (1u << 29)
#define DMC_CAL_PDCALEN       (1u << 30)
#define DMC_CAL_RTTCALEN      (1u << 31)

/* Register values from the board; INIT and CALSTRT are the sequence's. */
struct wb_dmc_config {
	uint32_t phy_ctl4, phy_ctl3, cal_padctl0, cal_padctl2;
	uint32_t dllctl;
	uint32_t cfg, tr0, tr1, tr2, mr, emr1, emr2;
	uint32_t ctl;
	uint32_t dclk_mhz;	/* for the waits in DCLK cycles */
};

#define WB_DMC_OK          0
#define WB_DMC_ERR_ARG    (-1)	/* a field the HRM fixes is wrong */

/* The HRM's waits, in DCLK cycles. */
#define WB_DMC_PADCAL_DCLK 300u
#define WB_DMC_DLLLOCK_DCLK 4500u

/*
 * Check what the HRM requires of a BF70x DDR2 set-up: DDR2 PHY mode,
 * 16-bit interface and device, one bank, RDTOWR = 2, DATACYC = 5, no
 * low-power request or INIT in ctl, CALSTRT clear; and DLLCAL set in ctl,
 * so the start write asks for the DLL calibration (S-13a.018 step 4).
 */
int wb_dmc_check(const struct wb_dmc_config *c);

/*
 * True if a write at alias_addr shows at base_addr, i.e. the device or
 * its configuration is smaller than expected (S-13a.021). Both words hold
 * their contents again afterwards. Runs on DDR, so only after init.
 */
bool wb_dmc_aliases(uint32_t base_addr, uint32_t alias_addr);

#endif /* WAFFLE_DRIVERS_BF70X_DMC_H */
