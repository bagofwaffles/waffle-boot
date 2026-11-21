/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The panel bus (S-13c.001 - S-13c.003), the front's copy of the full
 * bootloader's SMC driver (drivers/bf70x/lcd.c with the device's pins),
 * trimmed to what the front sends: every byte a write cycle of
 * SMC0 bank 0, by the CPU or by the one memory DMA of wb_lcd_frame_start()
 * on MDMA2 (DMA20 from memory, DMA21 to the bank; the ROM and the hand-over
 * use MDMA0), and no reads. D/C (the device's: PA5 on the OP-XY, PA11 on
 * the OP-1F), RD (PA13), CS (PA15) and RESET (PA8) are GPIO outputs,
 * TE (PA7) an input, D[7:0] port B and WR (PA14) the SMC's, on the
 * functions fr_hw_pins() selected (S-13b.003). Writes to the bank are
 * posted: a change of D/C or CS follows wb_mmr_sync(), so the earlier
 * write cycle has ended at the pins. Target only; nothing here waits but
 * a channel's stop, at most 100 us (wb_dma_stop_idle()).
 *
 * The front runs with the data CPLBs off, so the bank does not have the
 * I/O device space attribute the full bootloader's table gives it
 * (drivers/bf70x/cplb.c). Nothing in the processor's manuals says a run of
 * stores to one address then still reaches the bus as one write cycle
 * each, so every byte is followed by SSYNC: each byte has gone out before
 * the next is stored.
 */
#include "fr_handoff.h"
#include "fr_hw.h"

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_lcd.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/mmr.h"

#define PA_TE    (1u << 7)
#define PA_RESET (1u << 8)
#define PA_RD    (1u << 13)
#define PA_WR    (1u << 14)
#define PA_CS    (1u << 15)
#define DATA_PINS 0xFFu
#define BANK     WB_LCD_SMC_BANK0
#define MDMA_SRC WB_DMA_MDMA_SRC(2)	/* DMA20 */
#define MDMA_DST WB_DMA_MDMA_DST(2)	/* DMA21 */
#define STAT_W1C (DMA_STAT_IRQDONE | DMA_STAT_IRQERR | DMA_STAT_PIRQ)
#define STOP_US  100u

static bool dc_high = true;
static uint32_t pa_dc;	/* D/C's bit in port A (fr_hw_lcd_start()) */

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
	level(pa_dc, high);
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

/* cppcheck-suppress constParameterCallback ; fr_panel_bus fixes the type */
static bool b_te(void *ctx)
{
	(void)ctx;
	return (wb_mmr_read32(PORT_DATA(WB_PORT_A)) & PA_TE) != 0u;
}

/* wb_lcd_frame_start(): the destination one byte per write cycle at the
 * bank's fixed address, then the source, one byte at a time. The front
 * runs with the data cache off: nothing to flush. */
static void b_dma_start(void *ctx, const uint8_t *p, uint32_t n)
{
	(void)ctx;
	set_dc(true);
	wb_mmr_write32(DMA_STAT(MDMA_DST), STAT_W1C);
	wb_mmr_write32(DMA_ADDRSTART(MDMA_DST), BANK);
	wb_mmr_write32(DMA_XCNT(MDMA_DST), n);
	wb_mmr_write32(DMA_XMOD(MDMA_DST), 0);
	wb_mmr_write32(DMA_CFG(MDMA_DST), DMA_CFG_EN | DMA_CFG_WNR | DMA_CFG_FLOW_STOP |
			       DMA_CFG_INT_X | ((uint32_t)WB_DMA_1B << DMA_CFG_PSIZE_SHIFT) |
			       ((uint32_t)WB_DMA_1B << DMA_CFG_MSIZE_SHIFT));
	wb_mmr_write32(DMA_STAT(MDMA_SRC), STAT_W1C);
	wb_mmr_write32(DMA_ADDRSTART(MDMA_SRC), (uint32_t)(uintptr_t)p);
	wb_mmr_write32(DMA_XCNT(MDMA_SRC), n);
	wb_mmr_write32(DMA_XMOD(MDMA_SRC), 1);
	wb_mmr_write32(DMA_CFG(MDMA_SRC), DMA_CFG_EN | DMA_CFG_FLOW_STOP |
			       ((uint32_t)WB_DMA_1B << DMA_CFG_PSIZE_SHIFT) |
			       ((uint32_t)WB_DMA_1B << DMA_CFG_MSIZE_SHIFT));
	wb_mmr_sync();
}

/* wb_lcd_frame_busy(): the destination's status. */
/* cppcheck-suppress constParameterCallback ; fr_panel_bus fixes the type */
static int b_dma_state(void *ctx)
{
	uint32_t stat;

	(void)ctx;
	if ((wb_mmr_read32(DMA_CFG(MDMA_DST)) & DMA_CFG_EN) == 0u)
		return FR_DMA_IDLE;
	stat = wb_mmr_read32(DMA_STAT(MDMA_DST));
	if ((stat & DMA_STAT_IRQERR) != 0u)
		return FR_DMA_ERROR;
	if ((stat & DMA_STAT_IRQDONE) != 0u && (stat & DMA_STAT_RUN_MASK) == 0u)
		return FR_DMA_IDLE;
	return FR_DMA_BUSY;
}

/* wb_dma_stop_idle() on each channel, the source first. */
static void stop(unsigned ch)
{
	const uint64_t end = wb_core_cycles() + (uint64_t)STOP_US * FR_CCLK_MHZ;

	wb_mmr_write32(DMA_CFG(ch), 0);
	wb_mmr_sync();
	while ((wb_mmr_read32(DMA_STAT(ch)) & DMA_STAT_RUN_MASK) != 0u && wb_core_cycles() < end)
		;
	wb_mmr_write32(DMA_STAT(ch), STAT_W1C);
	wb_mmr_sync();
}

static void b_dma_stop(void *ctx)
{
	(void)ctx;
	stop(MDMA_SRC);
	stop(MDMA_DST);
}

static const struct fr_panel_bus bus = {
	b_reset, b_select, b_cmd, b_data, b_te, b_dma_start, b_dma_state, b_dma_stop, NULL,
};

const struct fr_panel_bus *fr_hw_lcd_start(const struct fr_dev *dev)
{
	pa_dc = WB_PIN_BIT(dev->panel_dc);	/* a pin of port A on both devices */
	/* HRM 10: the bank's timing before it is enabled (S-13c.003). */
	wb_mmr_write32(SMC0_B0TIM, WB_LCD_B0TIM);
	wb_mmr_write32(SMC0_B0ETIM, WB_LCD_B0ETIM);
	wb_mmr_write32(SMC0_B0CTL, SMC_CTL_EN);

	/* S-13c.001: the idle levels before the drivers come on; RESET is
	 * left to fr_panel_begin() (S-13c.004). */
	output(pa_dc | PA_RD | PA_CS, true);
	dc_high = true;
	wb_mmr_write32(PORT_DIR_CLR(WB_PORT_A), PA_TE);
	wb_mmr_write32(PORT_INEN_SET(WB_PORT_A), PA_TE);
	wb_mmr_write32(PORT_FER_SET(WB_PORT_B), DATA_PINS);
	wb_mmr_write32(PORT_FER_SET(WB_PORT_A), PA_WR);
	wb_mmr_sync();
	return &bus;
}
