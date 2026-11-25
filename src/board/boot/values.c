/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The OP-XY board's values (see waffle/board/values.h). */
#include "waffle/board/values.h"

#include <string.h>

#include "waffle/drivers/bf70x/core.h"
#include "waffle/psoc/cypress.h"

/* S-13a.001-.005 at CLKIN 25 MHz. */
const struct wb_cgu_config opxy_cgu = {
	.clkin_hz = 25000000u,
	.msel = 32u,		/* S-13a.002: fVCO 800 MHz */
	.df = false,
	.csel = 2u,		/* S-13a.003: CCLK 400 MHz */
	.syssel = 4u,		/* SYSCLK 200 MHz */
	.s0sel = 2u,		/* SCLK0 100 MHz */
	.s1sel = 1u,		/* SCLK1 200 MHz */
	.dsel = 4u,		/* DCLK 200 MHz */
	.osel = 16u,		/* OCLK 50 MHz */
	.clkoutsel = 1u,	/* S-13a.005: CCLK / 16 */
	.hold_ddr_phy = true,	/* S-13a.008, anomaly 19000051 */
	.limits = WB_CGU_LIMITS_DS(400000000u),
};

/* The ADSP-BF703 at the 400 MHz grade, with DDR2. */
const struct wb_cgu_part opxy_cgu_part = {
	.cclk_grade_hz = 400000000u,
	.usb = true,
	.dmc = WB_CGU_DMC_DDR2,
};

/* S-13a.010-.016. The timings are S-13a.013's bounds met exactly. */
const struct wb_dmc_config opxy_dmc = {
	.phy_ctl4 = DMC_PHY_CTL4_DDR2,		/* S-13a.010 */
	.phy_ctl3 = 0x0A0000C0u,		/* S-13a.011 */
	.cal_padctl0 = DMC_CAL_PUCALEN | DMC_CAL_PDCALEN | DMC_CAL_RTTCALEN,
	.cal_padctl2 = 0x0078283Cu,
	/* S-13a.016: DLLCALRDCNT 0x4B, DATACYC 5. */
	.dllctl = 0x4Bu | 5u << DMC_DLLCTL_DATACYC_SHIFT,
	/* S-13a.012: IFWID 2, SDRWID 2, SDRSIZE 5 (2 Gbit), EXTBANK 0. */
	.cfg = 2u << DMC_CFG_IFWID_SHIFT | 2u << DMC_CFG_SDRWID_SHIFT |
	       5u << DMC_CFG_SDRSIZE_SHIFT | 0u << DMC_CFG_EXTBANK_SHIFT,
	.tr0 = 0x20B08323u,			/* S-13a.013 */
	.tr1 = 0x20270618u,
	.tr2 = 0x00323209u,
	.mr = 0x00000432u,			/* S-13a.014 */
	.emr1 = 0u,
	.emr2 = 0u,
	/* S-13a.015; S-13a.018 step 4: DLLCAL goes with the start write. */
	.ctl = 2u << DMC_CTL_RDTOWR_SHIFT | DMC_CTL_DLLCAL,
	.dclk_mhz = 200u,
};

const struct wb_time opxy_time = { OPXY_CCLK_MHZ };

/* S-13b.001, .002, .008. */
const struct wb_strap opxy_strap = {
	.port = WB_PORT_A,
	.pins = OPXY_STRAP_ROLE | OPXY_STRAP_REV0 | OPXY_STRAP_REV1,
	.settle_us = OPXY_STRAP_SETTLE_US,
	.inen_us = 2u,
};

uint8_t opxy_strap_revision(uint16_t sample)
{
	return (uint8_t)(((sample & OPXY_STRAP_REV1) ? 2u : 0u) |
			 ((sample & OPXY_STRAP_REV0) ? 1u : 0u));
}

enum opxy_role opxy_strap_role(uint16_t sample)
{
	return (sample & OPXY_STRAP_ROLE) ? OPXY_MASTER : OPXY_SLAVE;
}

/* S-13b.004: PB10 on both roles, PC4 on the master, inputs first. */
static const uint8_t first_master[] = {
	WB_PIN(WB_PORT_B, 10), WB_PIN(WB_PORT_C, 4),
};
static const uint8_t first_slave[] = { WB_PIN(WB_PORT_B, 10) };

/* S-13b.003. */
const struct wb_pinmux opxy_pinmux_master = {
	.port = {
		{ 0x30002000u, 0x4040u },	/* A */
		{ 0x1400FFFFu, 0xFFFFu },	/* B */
		{ 0x00855100u, 0x0BD3u },	/* C */
	},
	.inputs_first = first_master,
	.n_inputs_first = sizeof(first_master),
};

/* S-13b.003 on the OP-1F (S-18.030): its main CPU's six values, with no
 * pin made an input first (S-13b.004: PC4 is MSI0's there). */
const struct wb_pinmux opxy_pinmux_op1f = {
	.port = {
		{ 0x30280000u, 0x4620u },	/* A */
		{ 0x1400FFFFu, 0xFFFFu },	/* B */
		{ 0x000AAA00u, 0x17F3u },	/* C */
	},
	.inputs_first = NULL,
	.n_inputs_first = 0u,
};

const struct wb_pinmux opxy_pinmux_slave = {
	.port = {
		{ 0x00000200u, 0x0000u },
		{ 0x00000000u, 0xFF00u },
		{ 0x055AAA00u, 0x3FF0u },
	},
	.inputs_first = first_slave,
	.n_inputs_first = sizeof(first_slave),
};

#define PA(n) WB_PIN(WB_PORT_A, n)
#define PB(n) WB_PIN(WB_PORT_B, n)
#define PC(n) WB_PIN(WB_PORT_C, n)

/*
 * S-13b.006. The bridge's own pins (PC11 and PC5 on the master, PA4 and
 * PB2 on the slave) are set by the bridge (S-13b.040, .043); PA6 on the
 * master is the boot link's ready input before that.
 */
const struct opxy_gpio opxy_gpio_master[] = {
	{ PA(3), OPXY_OUT_LOW },
	{ PA(7), OPXY_IN },		/* the display's TE, before PA6 */
	{ PA(6), OPXY_IN },
	{ PC(2), OPXY_IN },
};
const unsigned opxy_gpio_master_n =
	sizeof(opxy_gpio_master) / sizeof(opxy_gpio_master[0]);

/*
 * S-13b.006 on the OP-1F: PA0, PA3, PA4, PA6 and PC13 outputs at the
 * level each PORTx_DATA bit holds (no level written: 0 at reset and not
 * written by the first stage, so low), then PA1, PA2, PA7, PC2, PC11 and
 * PC14 inputs. PA8 is never an input (S-13c.027).
 */
const struct opxy_gpio opxy_gpio_op1f[] = {
	{ PA(0), OPXY_OUT_KEEP },
	{ PA(3), OPXY_OUT_KEEP },
	{ PA(4), OPXY_OUT_KEEP },
	{ PA(6), OPXY_OUT_KEEP },
	{ PC(13), OPXY_OUT_KEEP },
	{ PA(1), OPXY_IN },
	{ PA(2), OPXY_IN },
	{ PA(7), OPXY_IN },
	{ PC(2), OPXY_IN },
	{ PC(11), OPXY_IN },
	{ PC(14), OPXY_IN },
};
const unsigned opxy_gpio_op1f_n =
	sizeof(opxy_gpio_op1f) / sizeof(opxy_gpio_op1f[0]);

const struct wb_pinmux *opxy_pinmux_main(const struct wb_device *dev)
{
	return dev->id == WB_DEV_OP1F ? &opxy_pinmux_op1f : &opxy_pinmux_master;
}

const struct opxy_gpio *opxy_gpio_main(const struct wb_device *dev, unsigned *n)
{
	if (dev->id == WB_DEV_OP1F) {
		*n = opxy_gpio_op1f_n;
		return opxy_gpio_op1f;
	}
	*n = opxy_gpio_master_n;
	return opxy_gpio_master;
}

const struct opxy_gpio opxy_gpio_slave[] = {
	{ PA(3), OPXY_OUT_LOW },
	{ PA(6), OPXY_OUT_LOW },
	{ PA(9), OPXY_OUT_LOW },
	{ PA(10), OPXY_OUT_LOW },
	{ PA(11), OPXY_OUT_LOW },
	{ PB(6), OPXY_OUT_LOW },
	{ PC(2), OPXY_OUT_HIGH },
	{ PA(1), OPXY_IN },
	{ PA(15), OPXY_IN },
	{ PB(4), OPXY_IN },
	{ PB(7), OPXY_IN },
	{ PB(15), OPXY_IN },
	{ PC(14), OPXY_IN },
};
const unsigned opxy_gpio_slave_n =
	sizeof(opxy_gpio_slave) / sizeof(opxy_gpio_slave[0]);

/*
 * S-13b.010, .011. Mode 3 (CPOL and CPHA set), which S-13b.010 allows, so
 * that the software chip select frames every command; BAUD 3 is SCLK0 / 4.
 */
const struct wb_spi_config opxy_spi_flash = {
	SPI_CTL_MSTR | SPI_CTL_CPOL | SPI_CTL_CPHA | SPI_CTL_SIZE8, 3u, 0u, 1u,
};

/* S-13b.020, .021: BAUD 2 is SCLK0 / 3 = 33.3 MHz. */
const struct wb_spi_config opxy_spi_boot_link = {
	SPI_CTL_MSTR | SPI_CTL_CPHA | SPI_CTL_SIZE8, 2u, 0u, 3u,
};

#if OPXY_BOOT_LINK_FC
/* S-13b.022: the same link with the controller's ready input on PA6, low
 * for ready, holding off each byte of the stream. */
const struct wb_spi_config opxy_spi_boot_link_fc = {
	SPI_CTL_MSTR | SPI_CTL_CPHA | SPI_CTL_SIZE8 | SPI_CTL_FCEN, 2u, 0u, 3u,
};
#endif

/* S-13b.040, .041: the register values before the link. */
const struct wb_spi_config opxy_spi_bridge_pre = { 0x00002592u, 3u, 0u, 0u };

/* S-13b.042. */
const struct wb_spi_config opxy_spi_bridge_slave_pre = {
	0x00002510u, 0u, 0u, 0u,
};

/* S-13b.070, .071: divide by one, divisor 100: 1 Mbaud from SCLK0. */
const struct wb_uart_config opxy_uart_psoc = {
	UART_CTL_WLS8, UART_CLK_EDBO | 100u,
};
const struct wb_uart_config opxy_uart_nordic = {
	UART_CTL_WLS8 | UART_CTL_ARTS | UART_CTL_ACTS, UART_CLK_EDBO | 100u,
};

/*
 * S-13b.052: CLKDIV 63 gives 50 MHz / 126 = 396.8 kHz. TMOUT and FIFOTH
 * are free (S-13b, "What is a requirement"). S-13b.057: CMD1 at least 1 s.
 */
const struct wb_msi_config opxy_msi = {
	.ident_clkdiv = 63u,
	.tmout = 0xFFFFFF40u,
	.fifoth = 0x20070008u,
	.cmd1_timeout_us = 1500000u,
	.busy_timeout_us = 2000000u,
	.xfer_timeout_us = 1000000u,	/* per command, or per block of data */
	/* SPEC-Q: how long a flush takes on this eMMC. The bound
	 * keeps one try inside a request's T_EXEC (S-09.060, 10 s here): DAT0
	 * here, then the busy wait's DAT0 and CMD13. A longer flush fails its
	 * try, and S-03.010's next try finds the eMMC further along. */
	.flush_timeout_us = 6000000u,
	/* S-13b.057: a multi-block data phase, whole. One transfer then ends
	 * inside a request's T_EXEC (S-09.060, 10 s) with 2 s to spare: a
	 * write, plain or reliable, 4 s of data, DAT0 2 s, CMD13 2 s = 8 s; a
	 * read 4 s and DAT0 2 s = 6 s. 128 sectors in 4 s is 16 KiB/s. */
	.xfer_total_us = 4000000u,
};

/*
 * S-09.018: the bring-up's own waits, shorter, so that both tries end
 * within 5 s of the stream. One try waits at most CMD1's 1.05 s, seven
 * DAT0 or CMD13 waits (the identification clock, CMD7, then Switch(183)
 * and Switch(185) with their CMD13s, the clock raise) and two CMD8
 * transfers: 1.05 + 7 * 0.25 + 2 * 0.1 = 3.0 s. The clock's raise to 25 MHz
 * after CMD7 waits on DAT0 straight after CMD7's own wait, with no command
 * between, so it ends at its first read and adds nothing. The controller ends every
 * command within its response timeout, so the command waits add nothing
 * that counts. A repeat starts only within OPXY_EMMC_REPEAT_US of the
 * first try, so the whole is at most 1.5 + 3.0 = 4.5 s.
 */
const struct wb_msi_config opxy_msi_bringup = {
	.ident_clkdiv = 63u,
	.tmout = 0xFFFFFF40u,
	.fifoth = 0x20070008u,
	.cmd1_timeout_us = 1050000u,
	.busy_timeout_us = 250000u,
	.xfer_timeout_us = 100000u,
};

/* S-13b.060, S-13b.061: the OP-1F's main CPU drives its eMMC on four data
 * lines, with the same clocks and bounds. */
const struct wb_msi_config opxy_msi_1f = {
	.ident_clkdiv = 63u,
	.tmout = 0xFFFFFF40u,
	.fifoth = 0x20070008u,
	.cmd1_timeout_us = 1500000u,
	.busy_timeout_us = 2000000u,
	.xfer_timeout_us = 1000000u,
	.flush_timeout_us = 6000000u,
	.xfer_total_us = 4000000u,
	.emmc_width = 4u,
};
const struct wb_msi_config opxy_msi_bringup_1f = {
	.ident_clkdiv = 63u,
	.tmout = 0xFFFFFF40u,
	.fifoth = 0x20070008u,
	.cmd1_timeout_us = 1050000u,
	.busy_timeout_us = 250000u,
	.xfer_timeout_us = 100000u,
	.emmc_width = 4u,
};

/* S-05.016's slave MSI row: INTEN set, the internal DMA off, the error
 * interrupts unmasked, the card read threshold on at one block. */
const struct wb_msi_leave opxy_msi_leave = {
	.ctl = 0x00000010u,
	.busmode = 0x00000600u,
	.imsk = 0x0000FFC2u,
	.fifoth = 0x607F0080u,
	.cdthrctl = 0x02000001u,
};

/* S-05.016's SEC row: the sources left enabled at the kernel call, with
 * IEN and SEN set, and the slave's source 21 at its priority alone. The
 * OP-1F's list is the master's first OPXY_SEC_KEEP_1F (S-05.037: source 1
 * alone; 19, 20, 21 and 72 clear like every other, provisional). */
const struct opxy_sec_keep opxy_sec_keep_master[OPXY_SEC_KEEP_MASTER] = {
	{ 1u, 0x00000005u }, { 19u, 0x00000005u },
	{ 20u, 0x00000105u }, { 21u, 0x00000105u },
};
const struct opxy_sec_keep opxy_sec_keep_slave[OPXY_SEC_KEEP_SLAVE] = {
	{ 1u, 0x00000005u }, { 19u, 0x00000005u },
	{ 20u, 0x00000105u }, { 21u, 0x00000100u }, { 72u, 0x00000005u },
};

/*
 * S-05.014: SP and FP in DDR, and the vectors in L1 code, which the OS
 * loads over: so each vector reaches the OS's own bytes (S-05.031).
 */
#define OS_EVTS { 0x11A00D4Cu, 0x11A00CACu, 0x11A00798u, 0x11A00798u, \
	0x11A00798u, 0x11A00798u, 0x11A00798u, 0x11A00798u, 0x11A00798u, \
	0x11A01570u, 0x11A00798u, 0x11A00798u, 0x11A00798u, 0x11A000CAu }
const struct opxy_os_entry opxy_os_entry_master = {
	0x88A65B44u, 0x88A65B68u, OS_EVTS,
};
const struct opxy_os_entry opxy_os_entry_slave = {
	0x88A65B18u, 0x88A65B3Cu, OS_EVTS,
};

/* S-05.038: the OP-1F's vectors, EVT2 first; SP and FP are not fixed
 * there, so they are the master's: a stack in DDR clear of S-05.040's
 * ranges and of the staged image. */
#define EVT_1F_OTHER 0x11A00664u
const struct opxy_os_entry opxy_os_entry_op1f = {
	0x88A65B44u, 0x88A65B68u,
	{ 0x11A00C18u, 0x11A00B78u, EVT_1F_OTHER, EVT_1F_OTHER, EVT_1F_OTHER,
	  EVT_1F_OTHER, EVT_1F_OTHER, EVT_1F_OTHER, EVT_1F_OTHER, 0x11A01438u,
	  EVT_1F_OTHER, EVT_1F_OTHER, EVT_1F_OTHER, 0x11A000CAu },
};

/*
 * S-16.095, S-01.004: neither read needs the OTP configuration entry
 * (0x04000054) first, since the ROM's preboot has configured the
 * controller, so it is not called. A read that fails takes its
 * requirement's unreadable case.
 */
#if defined(__bfin__)
typedef bool (*rom_otp_get_fn)(uint32_t cmd, uint32_t *data);
typedef uint32_t (*rom_otp_adi_fn)(uint32_t cmd, uint32_t *data);

/* S-01.004, HRM chapter 36 adi_rom_otp_get(). */
bool opxy_otp_get(uint32_t item, uint32_t *data)
{
	const rom_otp_get_fn get = (rom_otp_get_fn)OPXY_ROM_OTP_GET;

	return get(item, data);
}

/* S-16.095: the entry with the command 0x13 and the four words. */
bool opxy_otp_fsn(uint32_t words[4])
{
	const rom_otp_adi_fn adi = (rom_otp_adi_fn)OPXY_ROM_OTP_ADI;
	unsigned i;

	for (i = 0; i < 4u; i++)
		words[i] = 0;
	if (adi(OPXY_OTP_ADI_FSN, words) != 0u)
		return true;
	for (i = 0; i < 4u; i++)
		words[i] = 0;
	return false;
}
#else
bool opxy_otp_get(uint32_t item, uint32_t *data)
{
	(void)item;
	(void)data;
	return false;
}

bool opxy_otp_fsn(uint32_t words[4])
{
	(void)words;
	return false;
}
#endif

/*
 * The PSoC unlock, the six bytes that enter the PSoC's bootloader
 * (S-07.081): sent with the entry command before the backplane is
 * programmed.
 */
bool opxy_psoc_unlock(uint8_t unlock[S07_CY_UNLOCK_LEN])
{
	static const uint8_t value[S07_CY_UNLOCK_LEN] = {
		0xA5, 0xB2, 0x98, 0x32, 0xBB, 0x3E,
	};

	memcpy(unlock, value, sizeof value);
	return true;
}
