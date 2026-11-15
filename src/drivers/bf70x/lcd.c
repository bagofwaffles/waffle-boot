/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The panel bus over SMC0 bank 0 and GPIO (see waffle/drivers/bf70x_lcd.h). */
#include "waffle/drivers/bf70x_lcd.h"

#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/mmr.h"

#define PA(n) WB_PIN(WB_PORT_A, n)

/* S-13c.001's pins; SCLK0 = 100 MHz (S-13a.001). */
const struct wb_lcd_config wb_lcd_opxy = {
	.dc = PA(5),
	.rd = PA(13),
	.cs = PA(15),
	.reset = PA(8),
	.te = PA(7),
	.wr = PA(14),
	.data_port = WB_PORT_B,
	.bank = WB_LCD_SMC_BANK0,
	.b0tim = WB_LCD_B0TIM,
	.b0etim = WB_LCD_B0ETIM,
	.mdma = 2,	/* S-05.016: DMA20/DMA21; the ROM and the hand-over use MDMA0 */
	.read_strobe_us = 1,
};

#define DATA_PINS 0xFFu
#define STAT_W1C  (DMA_STAT_IRQDONE | DMA_STAT_IRQERR | DMA_STAT_PIRQ)

static void set_dc(struct wb_lcd *l, int level)
{
	if (l->dc == level)
		return;
	wb_mmr_sync();		/* the last write cycle has ended */
	(void)wb_gpio_set(l->cfg->dc, level != 0);
	wb_mmr_sync();
	l->dc = level;
}

static void put(const struct wb_lcd *l, uint8_t b)
{
	wb_mmr_write8(l->cfg->bank, b);
}

int wb_lcd_init(struct wb_lcd *l, const struct wb_lcd_config *cfg,
		const struct wb_time *t)
{
	if (cfg->mdma > 2u || cfg->data_port >= WB_PORTS)
		return WB_LCD_ERR_ARG;
	l->cfg = cfg;
	l->time = t;
	l->dc = 1;
	l->selected = false;
	l->frame = false;

	/* HRM 10: the bank's timing before it is enabled. */
	wb_mmr_write32(SMC0_B0TIM, cfg->b0tim);
	wb_mmr_write32(SMC0_B0ETIM, cfg->b0etim);
	wb_mmr_write32(SMC0_B0CTL, SMC_CTL_EN);

	/* S-13c.001: the idle levels before the drivers come on. */
	(void)wb_gpio_output(cfg->dc, true);
	(void)wb_gpio_output(cfg->rd, true);
	(void)wb_gpio_output(cfg->cs, true);
	(void)wb_gpio_input(cfg->te);
	wb_mmr_write32(PORT_FER_SET(cfg->data_port), DATA_PINS);
	(void)wb_gpio_peripheral(cfg->wr);
	wb_mmr_sync();
	return WB_LCD_OK;
}

void wb_lcd_reset(const struct wb_lcd *l, bool high)
{
	(void)wb_gpio_output(l->cfg->reset, high);
	wb_mmr_sync();
}

void wb_lcd_select(struct wb_lcd *l, bool on)
{
	wb_mmr_sync();
	(void)wb_gpio_set(l->cfg->cs, !on);
	wb_mmr_sync();
	l->selected = on;
}

void wb_lcd_cmd(struct wb_lcd *l, uint8_t c)
{
	set_dc(l, 0);
	put(l, c);
}

void wb_lcd_data(struct wb_lcd *l, const uint8_t *p, size_t n)
{
	size_t i;

	set_dc(l, 1);
	for (i = 0; i < n; i++)
		put(l, p[i]);
}

void wb_lcd_repeat(struct wb_lcd *l, const uint8_t *group, size_t len,
		   uint32_t count)
{
	uint32_t k;

	set_dc(l, 1);
	for (k = 0; k < count; k++)
		for (size_t i = 0; i < len; i++)
			put(l, group[i]);
}

/* PORTB bit n is the panel's D(7-n) (S-13c.001). */
static uint8_t reverse8(uint32_t v)
{
	uint8_t r = 0;
	unsigned i;

	for (i = 0; i < 8u; i++)
		if (v & (1u << i))
			r = (uint8_t)(r | (0x80u >> i));
	return r;
}

int wb_lcd_read(struct wb_lcd *l, uint8_t cmd, uint8_t *out, size_t n)
{
	const struct wb_lcd_config *c = l->cfg;
	size_t i;

	if (n == 0 || l->frame)
		return WB_LCD_ERR_ARG;
	if (l->selected)
		wb_lcd_select(l, false);
	wb_lcd_select(l, true);
	wb_lcd_cmd(l, cmd);
	set_dc(l, 1);
	/* The data pins from the SMC to the port, as inputs. */
	wb_mmr_write32(PORT_DIR_CLR(c->data_port), DATA_PINS);
	wb_mmr_write32(PORT_FER_CLR(c->data_port), DATA_PINS);
	wb_mmr_write32(PORT_INEN_SET(c->data_port), DATA_PINS);
	wb_mmr_sync();
	for (i = 0; i < n; i++) {
		(void)wb_gpio_set(c->rd, false);
		wb_mmr_sync();
		wb_delay_us(l->time, c->read_strobe_us);
		out[i] = reverse8(wb_mmr_read32(PORT_DATA(c->data_port)));
		(void)wb_gpio_set(c->rd, true);
		wb_mmr_sync();
		wb_delay_us(l->time, c->read_strobe_us);
	}
	wb_mmr_write32(PORT_INEN_CLR(c->data_port), DATA_PINS);
	wb_mmr_write32(PORT_FER_SET(c->data_port), DATA_PINS);
	wb_lcd_select(l, false);
	return WB_LCD_OK;
}

int wb_lcd_te_wait(const struct wb_lcd *l, uint32_t timeout_us)
{
	uint64_t end = wb_time_deadline(l->time, timeout_us);
	bool seen_low = false;

	for (;;) {
		bool late = wb_time_passed(end);
		bool te = wb_gpio_get(l->cfg->te);

		if (!te)
			seen_low = true;
		else if (seen_low)
			return WB_LCD_OK;
		if (late)
			return WB_LCD_TIMEOUT;
	}
}

int wb_lcd_frame_start(struct wb_lcd *l, const uint8_t *buf, uint32_t n)
{
	const unsigned src = WB_DMA_MDMA_SRC(l->cfg->mdma);
	const unsigned dst = WB_DMA_MDMA_DST(l->cfg->mdma);
	struct wb_dma_xfer x = { 0 };

	if (n == 0)
		return WB_LCD_ERR_ARG;
	if (l->frame || wb_dma_enabled(src) || wb_dma_enabled(dst))
		return WB_LCD_BUSY;
	set_dc(l, 1);
	/* The destination: one byte per write cycle, the address fixed. */
	wb_mmr_write32(DMA_STAT(dst), STAT_W1C);
	wb_mmr_write32(DMA_ADDRSTART(dst), l->cfg->bank);
	wb_mmr_write32(DMA_XCNT(dst), n);
	wb_mmr_write32(DMA_XMOD(dst), 0);
	wb_mmr_write32(DMA_CFG(dst), DMA_CFG_EN | DMA_CFG_WNR |
		       DMA_CFG_FLOW_STOP | DMA_CFG_INT_X |
		       ((uint32_t)WB_DMA_1B << DMA_CFG_PSIZE_SHIFT) |
		       ((uint32_t)WB_DMA_1B << DMA_CFG_MSIZE_SHIFT));
	x.addr = (uint32_t)(uintptr_t)buf;
	x.count = n;
	x.msize = WB_DMA_1B;
	x.psize = WB_DMA_1B;
	if (wb_dma_start(src, &x) != WB_DMA_OK) {
		wb_dma_stop(dst);
		return WB_LCD_ERR_ARG;
	}
	l->frame = true;
	return WB_LCD_OK;
}

static void reap(struct wb_lcd *l)
{
	wb_dma_stop(WB_DMA_MDMA_DST(l->cfg->mdma));
	wb_dma_stop(WB_DMA_MDMA_SRC(l->cfg->mdma));
	l->frame = false;
}

void wb_lcd_frame_stop(struct wb_lcd *l)
{
	if (!l->frame)
		return;
	wb_dma_stop_idle(WB_DMA_MDMA_SRC(l->cfg->mdma), l->time);
	wb_dma_stop_idle(WB_DMA_MDMA_DST(l->cfg->mdma), l->time);
	l->frame = false;
}

bool wb_lcd_frame_busy(struct wb_lcd *l)
{
	uint32_t stat;

	if (!l->frame)
		return false;
	stat = wb_mmr_read32(DMA_STAT(WB_DMA_MDMA_DST(l->cfg->mdma)));
	if ((stat & DMA_STAT_IRQERR) ||
	    ((stat & DMA_STAT_IRQDONE) && (stat & DMA_STAT_RUN_MASK) == 0)) {
		reap(l);
		return false;
	}
	return true;
}
