/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The panel bus on the OP-XY (S-13c.001 - S-13c.003), the front's copy of
 * the full bootloader's SMC driver (drivers/bf70x/lcd.c with its OP-XY
 * pins), trimmed to what the front sends: every byte a write cycle of
 * SMC0 bank 0 by the CPU, no DMA (the transfer engines stay free) and no
 * reads. D/C (PA5), RD (PA13), CS (PA15) and RESET (PA8) are GPIO outputs,
 * TE (PA7) an input, D[7:0] port B and WR (PA14) the SMC's, on the
 * functions fr_hw_pins() selected (S-13b.003). Writes to the bank are
 * posted: a change of D/C or CS follows wb_mmr_sync(), so the earlier
 * write cycle has ended at the pins. Target only; nothing here waits.
 *
 * The front runs with the data CPLBs off, so the bank does not have the
 * I/O device space attribute the full bootloader's table gives it
 * (drivers/bf70x/cplb.c). Nothing in the processor's manuals says a run of
 * stores to one address then still reaches the bus as one write cycle
 * each, so every byte is followed by SSYNC: each byte has gone out before
 * the next is stored.
 */
#include "fr_hw.h"

#include "waffle/drivers/bf70x_lcd.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/mmr.h"

#define PA_DC    (1u << 5)
#define PA_TE    (1u << 7)
#define PA_RESET (1u << 8)
#define PA_RD    (1u << 13)
#define PA_WR    (1u << 14)
#define PA_CS    (1u << 15)
#define DATA_PINS 0xFFu
#define BANK     WB_LCD_SMC_BANK0

static bool dc_high = true;

/* One byte, one write cycle, ended before the next store. */
static inline __attribute__((always_inline)) void put(uint8_t b)
{
	wb_mmr_write8(BANK, b);
	wb_mmr_sync();
}

static void level(uint32_t bits, bool high)
{
	wb_mmr_write32(high ? PORT_DATA_SET(WB_PORT_A) : PORT_DATA_CLR(WB_PORT_A), bits);
}

static void output(uint32_t bits, bool high)
{
	level(bits, high);
	wb_mmr_write32(PORT_DIR_SET(WB_PORT_A), bits);
}

FR_FAST static void set_dc(bool high)
{
	if (dc_high == high)
		return;
	wb_mmr_sync();		/* the last write cycle has ended */
	level(PA_DC, high);
	wb_mmr_sync();
	dc_high = high;
}

/* cppcheck-suppress constParameterCallback ; fr_panel_bus fixes the type */
static void b_reset(void *ctx, bool high)
{
	(void)ctx;
	output(PA_RESET, high);
	wb_mmr_sync();
}

static void b_select(void *ctx, bool on)
{
	(void)ctx;
	wb_mmr_sync();
	level(PA_CS, !on);
	wb_mmr_sync();
}

static void b_cmd(void *ctx, uint8_t c)
{
	(void)ctx;
	set_dc(false);
	put(c);
}

/* A frame's 480 rows come through here: kept in L1. */
FR_FAST static void b_data(void *ctx, const uint8_t *p, size_t n)
{
	const uint8_t *end = p + n;

	(void)ctx;
	set_dc(true);
	while (p < end)
		put(*p++);
}

static const struct fr_panel_bus bus = {
	b_reset, b_select, b_cmd, b_data, NULL,
};

const struct fr_panel_bus *fr_hw_lcd_start(void)
{
	/* HRM 10: the bank's timing before it is enabled (S-13c.003). */
	wb_mmr_write32(SMC0_B0TIM, WB_LCD_B0TIM);
	wb_mmr_write32(SMC0_B0ETIM, WB_LCD_B0ETIM);
	wb_mmr_write32(SMC0_B0CTL, SMC_CTL_EN);

	/* S-13c.001: the idle levels before the drivers come on; RESET is
	 * left to fr_panel_begin() (S-13c.004). */
	output(PA_DC | PA_RD | PA_CS, true);
	dc_high = true;
	wb_mmr_write32(PORT_DIR_CLR(WB_PORT_A), PA_TE);
	wb_mmr_write32(PORT_INEN_SET(WB_PORT_A), PA_TE);
	wb_mmr_write32(PORT_FER_SET(WB_PORT_B), DATA_PINS);
	wb_mmr_write32(PORT_FER_SET(WB_PORT_A), PA_WR);
	wb_mmr_sync();
	return &bus;
}
