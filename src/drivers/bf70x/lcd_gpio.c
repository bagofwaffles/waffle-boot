/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The panel bus bit-banged over GPIO: waffle/drivers/bf70x_lcd.h's
 * interface with every byte driven by the port, for the test OS's gpio
 * variant (wiki/Test-OS.md, make test-os), which drives the panel
 * without the SMC. Built only with
 * OPXY_TEST_OS and OPXY_TESTOS_PANEL_GPIO, in place of lcd.c; the normal
 * image never has it.
 *
 * The pins are S-13c.001's: D[7:0] on PB0-PB7 (PBn wired to D(7-n), so a
 * byte is bit-reversed into PORTB_DATA), D/C PA5, WR PA14, RD PA13, CS
 * PA15, TE PA7; RESET (PA8) is S-13c.004's and driven by wb_lcd_reset()
 * alone. S-13b.003's master table puts PB0-PB7 and PA14 in their SMC
 * function (FER set): here each of those pins gets its level and
 * direction first and its PORT_FER bit cleared after (HRM ch. 20, "PORT
 * Functional Description": a pin whose FER bit is set is the
 * peripheral's, and PORT_DATA writes do not reach it), so the pin shows
 * the same idle level as a GPIO as it did as the SMC's, and WR makes no
 * edge at the switch. The SMC is not touched.
 *
 * A byte is one write strobe (S-13c.002): D[7:0] set, a sync so the data
 * is at the pins, WR low, a sync for its width, WR high, a sync before the
 * next change. Each sync waits for the posted writes to complete, which at
 * SCLK0 = 100 MHz is tens of nanoseconds, above S-13c.003's floors (tDST,
 * tWRL, tDHT 15 ns; tWC 66 ns). A frame of 215 040 bytes takes about 60 ms
 * this way, sent in the foreground: frame_start() returns when the frame
 * has gone out, and frame_busy() is never true.
 */
#include "waffle/drivers/bf70x_lcd.h"

#if defined(OPXY_TEST_OS) && defined(OPXY_TESTOS_PANEL_GPIO)

#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/mmr.h"

#define PA(n) WB_PIN(WB_PORT_A, n)

/* S-13c.001's pins; the SMC fields are kept for the interface and unused. */
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

/* rev8[b] has b's bits in the other order: PORTB bit n carries D(7-n). */
#define R2(n) (n), (n) + 2 * 64, (n) + 1 * 64, (n) + 3 * 64
#define R4(n) R2(n), R2((n) + 2 * 16), R2((n) + 1 * 16), R2((n) + 3 * 16)
#define R6(n) R4(n), R4((n) + 2 * 4), R4((n) + 1 * 4), R4((n) + 3 * 4)
static const uint8_t rev8[256] = { R6(0), R6(2), R6(1), R6(3) };

static void set_dc(struct wb_lcd *l, int level)
{
	if (l->dc == level)
		return;
	wb_mmr_sync();		/* the last strobe has ended */
	(void)wb_gpio_set(l->cfg->dc, level != 0);
	wb_mmr_sync();
	l->dc = level;
}

/* One byte: D[7:0], then a WR pulse (S-13c.002, S-13c.003). */
static void strobe(const struct wb_lcd *l, uint8_t b)
{
	const struct wb_lcd_config *c = l->cfg;
	const uint32_t r = rev8[b];

	wb_mmr_write32(PORT_DATA_SET(c->data_port), r);
	wb_mmr_write32(PORT_DATA_CLR(c->data_port), r ^ DATA_PINS);
	wb_mmr_sync();		/* D valid before WR falls (tDST) */
	wb_mmr_write32(PORT_DATA_CLR(WB_PIN_PORT(c->wr)), WB_PIN_BIT(c->wr));
	wb_mmr_sync();		/* WR low (tWRL) */
	wb_mmr_write32(PORT_DATA_SET(WB_PIN_PORT(c->wr)), WB_PIN_BIT(c->wr));
	wb_mmr_sync();		/* latched before D or D/C change (tDHT) */
}

/* A pin to its GPIO function at a level: level, direction, then FER. */
static void gpio_out(uint8_t pin, bool high)
{
	(void)wb_gpio_output(pin, high);
	(void)wb_gpio_gpio(pin);
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

	/* S-13c.001: the idle levels, every pin a GPIO. */
	gpio_out(cfg->dc, true);
	gpio_out(cfg->rd, true);
	gpio_out(cfg->cs, true);
	gpio_out(cfg->wr, true);
	(void)wb_gpio_input(cfg->te);
	(void)wb_gpio_gpio(cfg->te);
	wb_mmr_write32(PORT_DATA_CLR(cfg->data_port), DATA_PINS);
	wb_mmr_write32(PORT_DIR_SET(cfg->data_port), DATA_PINS);
	wb_mmr_write32(PORT_FER_CLR(cfg->data_port), DATA_PINS);
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
	strobe(l, c);
}

void wb_lcd_data(struct wb_lcd *l, const uint8_t *p, size_t n)
{
	size_t i;

	set_dc(l, 1);
	for (i = 0; i < n; i++)
		strobe(l, p[i]);
}

void wb_lcd_repeat(struct wb_lcd *l, const uint8_t *group, size_t len,
		   uint32_t count)
{
	uint32_t k;

	set_dc(l, 1);
	for (k = 0; k < count; k++)
		for (size_t i = 0; i < len; i++)
			strobe(l, group[i]);
}

/* S-13c.011: the data pins become inputs for the strobes and outputs
 * again after; their function stays GPIO. */
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
	wb_mmr_write32(PORT_DIR_CLR(c->data_port), DATA_PINS);
	wb_mmr_write32(PORT_INEN_SET(c->data_port), DATA_PINS);
	wb_mmr_sync();
	for (i = 0; i < n; i++) {
		(void)wb_gpio_set(c->rd, false);
		wb_mmr_sync();
		wb_delay_us(l->time, c->read_strobe_us);
		out[i] = rev8[wb_mmr_read32(PORT_DATA(c->data_port)) & DATA_PINS];
		(void)wb_gpio_set(c->rd, true);
		wb_mmr_sync();
		wb_delay_us(l->time, c->read_strobe_us);
	}
	wb_mmr_write32(PORT_INEN_CLR(c->data_port), DATA_PINS);
	wb_mmr_write32(PORT_DATA_CLR(c->data_port), DATA_PINS);
	wb_mmr_write32(PORT_DIR_SET(c->data_port), DATA_PINS);
	wb_mmr_sync();
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

/* S-13c.016: the frame's bytes, all of them before the return. */
int wb_lcd_frame_start(struct wb_lcd *l, const uint8_t *buf, uint32_t n)
{
	uint32_t i;

	if (n == 0)
		return WB_LCD_ERR_ARG;
	if (l->frame)
		return WB_LCD_BUSY;
	set_dc(l, 1);
	for (i = 0; i < n; i++)
		strobe(l, buf[i]);
	return WB_LCD_OK;
}

bool wb_lcd_frame_busy(struct wb_lcd *l)
{
	(void)l;
	return false;
}

void wb_lcd_frame_stop(struct wb_lcd *l)
{
	(void)l;
}

#else

/* ISO C wants a declaration in every translation unit. */
typedef int wb_lcd_gpio_not_built;

#endif /* OPXY_TEST_OS && OPXY_TESTOS_PANEL_GPIO */
