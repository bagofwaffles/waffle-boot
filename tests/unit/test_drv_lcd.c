/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tests for the panel bus over SMC0 and GPIO (src/drivers/bf70x/lcd.c). */
#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_lcd.h"
#include "waffle/drivers/bf70x_port.h"

static const struct wb_time cclk400 = { 400u };

#define PA_BIT(n) (1u << (n))
#define BANK      WB_LCD_SMC_BANK0

/* DMA_STAT's W1C bits; an enabled memory DMA destination completes at
 * once unless hold is set. */
static bool hold;

static bool dma_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		     wb_mmr_addr addr, uint32_t *value)
{
	unsigned ch;

	(void)ctx;
	(void)width;
	if (op != FAKE_MMR_WRITE)
		return true;
	for (ch = 16; ch < WB_DMA_CHANNELS; ch++) {
		if (addr == DMA_STAT(ch)) {
			*value = fake_mmr_get(addr) & ~(*value & 7u);
			return true;
		}
		if (addr == DMA_CFG(ch) && (*value & DMA_CFG_EN) &&
		    (ch & 1u) && !hold)
			fake_mmr_set(DMA_STAT(ch), DMA_STAT_IRQDONE);
	}
	return true;
}

static void reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
	hold = false;
	fake_mmr_set_hook(dma_hook, NULL);
}

/* Index of the first access matching op, width, addr (and value unless
 * any_value) at or after from; -1 when none. */
static long find(size_t from, enum fake_mmr_op op, unsigned width,
		 wb_mmr_addr addr, uint32_t value, bool any_value)
{
	const struct fake_mmr_access *a = fake_mmr_log();
	size_t i, n = fake_mmr_count();

	for (i = from; i < n; i++)
		if (a[i].op == op && a[i].width == width && a[i].addr == addr &&
		    (any_value || a[i].value == value))
			return (long)i;
	return -1;
}

static long find_w32(size_t from, wb_mmr_addr addr, uint32_t value)
{
	return find(from, FAKE_MMR_WRITE, 32u, addr, value, false);
}

/* The bytes written to the bank, in order. */
static size_t bank_bytes(uint8_t *out, size_t max)
{
	const struct fake_mmr_access *a = fake_mmr_log();
	size_t i, n = 0;

	for (i = 0; i < fake_mmr_count(); i++)
		if (a[i].op == FAKE_MMR_WRITE && a[i].addr == BANK) {
			CHECK(a[i].width == 8u);
			if (n < max)
				out[n] = (uint8_t)a[i].value;
			n++;
		}
	return n;
}

/* S-13c.003 at SCLK0 = 100 MHz: the write cycle meets every floor. */
static void test_timing(void)
{
	const uint32_t tim = WB_LCD_B0TIM, etim = WB_LCD_B0ETIM;
	const unsigned ns = 10;		/* one SCLK0 cycle */
	unsigned wst = tim & 7u, wht = (tim >> 4) & 7u, wat = (tim >> 8) & 63u;
	unsigned it = (etim >> 12) & 7u;

	CHECK(wst != 0);
	CHECK(wat * ns >= 15u);				/* tWRL */
	CHECK((wst + wat + wht + it) * ns >= 66u);	/* tWC */
	CHECK((wst + wat) * ns >= 15u);			/* tDST */
	CHECK(wht * ns >= 15u);				/* tDHT */
	/* S-13c.018: a frame fits the shortest assumed TE period. */
	CHECK(215040u * (wst + wat + wht + it) * ns < 15400000u);
}

static void test_init(void)
{
	struct wb_lcd l;
	long a, b, c;

	reset();
	CHECK(wb_lcd_init(&l, &wb_lcd_opxy, &cclk400) == WB_LCD_OK);
	a = find_w32(0, SMC0_B0TIM, WB_LCD_B0TIM);
	b = find_w32(0, SMC0_B0ETIM, WB_LCD_B0ETIM);
	c = find_w32(0, SMC0_B0CTL, SMC_CTL_EN);
	CHECK(a >= 0 && b > a && c > b);	/* timing before the enable */
	/* D/C, RD and CS: the level first, then the driver (S-13c.001). */
	a = find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(13));
	b = find_w32(0, PORT_DIR_SET(WB_PORT_A), PA_BIT(13));
	CHECK(a >= 0 && b > a);
	CHECK(find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(5)) >= 0);
	CHECK(find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(15)) >= 0);
	CHECK(find_w32(0, PORT_INEN_SET(WB_PORT_A), PA_BIT(7)) >= 0);
	CHECK(find_w32(0, PORT_FER_SET(WB_PORT_B), 0xFFu) >= 0);
	CHECK(find_w32(0, PORT_FER_SET(WB_PORT_A), PA_BIT(14)) >= 0);
	/* RESET is not driven by the set-up (S-13c.004). */
	CHECK(find(0, FAKE_MMR_WRITE, 32u, PORT_DIR_SET(WB_PORT_A),
		   PA_BIT(8), false) < 0);
	CHECK(fake_mmr_errors() == 0);

	reset();
	{
		struct wb_lcd_config bad = wb_lcd_opxy;

		bad.mdma = 3;
		CHECK(wb_lcd_init(&l, &bad, &cclk400) == WB_LCD_ERR_ARG);
		CHECK(fake_mmr_count() == 0);
	}
}

/* S-13c.002: D/C low for the command, high for its parameters, the bytes
 * on the bank unchanged; D/C changes only after a sync. */
static void test_cmd_data(void)
{
	static const uint8_t p[] = { 0x8A, 0x07, 0x3B };
	const struct fake_mmr_access *log;
	struct wb_lcd l;
	uint8_t got[8] = { 0 };
	long dc_lo, cmd, dc_hi;

	reset();
	(void)wb_lcd_init(&l, &wb_lcd_opxy, &cclk400);
	fake_mmr_clear_log();
	wb_lcd_select(&l, true);
	wb_lcd_cmd(&l, 0xB6);
	wb_lcd_data(&l, p, sizeof p);
	wb_lcd_data(&l, p, 1);		/* D/C already high: no change */

	CHECK(bank_bytes(got, sizeof got) == 5u);
	CHECK(got[0] == 0xB6 && got[1] == 0x8A && got[2] == 0x07 &&
	      got[3] == 0x3B && got[4] == 0x8A);
	log = fake_mmr_log();
	CHECK(find_w32(0, PORT_DATA_CLR(WB_PORT_A), PA_BIT(15)) >= 0);
	dc_lo = find_w32(0, PORT_DATA_CLR(WB_PORT_A), PA_BIT(5));
	cmd = find(0, FAKE_MMR_WRITE, 8u, BANK, 0xB6, false);
	dc_hi = find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(5));
	CHECK(dc_lo >= 1 && cmd > dc_lo && dc_hi > cmd);
	CHECK(log[dc_lo - 1].op == FAKE_MMR_SYNC);
	CHECK(log[dc_lo + 1].op == FAKE_MMR_SYNC);
	CHECK(log[dc_hi - 1].op == FAKE_MMR_SYNC);
	CHECK(find_w32((size_t)dc_hi + 1, PORT_DATA_SET(WB_PORT_A),
		       PA_BIT(5)) < 0);

	fake_mmr_clear_log();
	{
		static const uint8_t px[2] = { 0x12, 0x34 };

		wb_lcd_repeat(&l, px, 2, 3);
		CHECK(bank_bytes(got, sizeof got) == 6u);
		CHECK(got[0] == 0x12 && got[1] == 0x34 && got[4] == 0x12 &&
		      got[5] == 0x34);
	}
}

/* S-13c.011: the read's CS framing and strobes; PBn is D(7-n). */
static void test_read(void)
{
	struct wb_lcd l;
	uint8_t out[4] = { 0 };
	long cs_hi, cs_lo, cmd, fer_clr, rd_lo, fer_set, cs_end;

	reset();
	(void)wb_lcd_init(&l, &wb_lcd_opxy, &cclk400);
	wb_lcd_select(&l, true);
	fake_mmr_clear_log();
	fake_mmr_queue_read(PORT_DATA(WB_PORT_B), 0x00u);
	fake_mmr_queue_read(PORT_DATA(WB_PORT_B), 0xEEu);	/* D = 0x77 */
	fake_mmr_queue_read(PORT_DATA(WB_PORT_B), 0x69u);	/* D = 0x96 */
	fake_mmr_queue_read(PORT_DATA(WB_PORT_B), 0x01u);	/* D = 0x80 */
	CHECK(wb_lcd_read(&l, 0x04, out, 4) == WB_LCD_OK);
	CHECK(out[0] == 0x00 && out[1] == 0x77 && out[2] == 0x96 &&
	      out[3] == 0x80);

	cs_hi = find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(15));
	cs_lo = find_w32((size_t)cs_hi + 1, PORT_DATA_CLR(WB_PORT_A),
			 PA_BIT(15));
	cmd = find((size_t)cs_lo, FAKE_MMR_WRITE, 8u, BANK, 0x04, false);
	fer_clr = find_w32((size_t)cmd, PORT_FER_CLR(WB_PORT_B), 0xFFu);
	rd_lo = find_w32((size_t)fer_clr, PORT_DATA_CLR(WB_PORT_A),
			 PA_BIT(13));
	fer_set = find_w32((size_t)rd_lo, PORT_FER_SET(WB_PORT_B), 0xFFu);
	cs_end = find_w32((size_t)fer_set, PORT_DATA_SET(WB_PORT_A),
			  PA_BIT(15));
	CHECK(cs_hi >= 0 && cs_lo > cs_hi && cmd > cs_lo &&
	      fer_clr > cmd && rd_lo > fer_clr && fer_set > rd_lo &&
	      cs_end > fer_set);
	/* D/C high for the strobes; no write cycle after the command. */
	CHECK(find_w32((size_t)cmd, PORT_DATA_SET(WB_PORT_A), PA_BIT(5)) <
	      rd_lo);
	{
		uint8_t b[8];

		CHECK(bank_bytes(b, sizeof b) == 1u);
	}
	CHECK(!l.selected);
	CHECK(wb_lcd_read(&l, 0x04, out, 0) == WB_LCD_ERR_ARG);
}

/* A TE rising edge: low, then high; a level that stays high is no edge. */
static void test_te(void)
{
	struct wb_lcd l;
	unsigned i;

	reset();
	(void)wb_lcd_init(&l, &wb_lcd_opxy, &cclk400);
	fake_core_set_cycle_step(400);		/* 1 us per read */
	fake_mmr_queue_read(PORT_DATA(WB_PORT_A), PA_BIT(7));
	fake_mmr_queue_read(PORT_DATA(WB_PORT_A), 0);
	fake_mmr_queue_read(PORT_DATA(WB_PORT_A), 0);
	fake_mmr_queue_read(PORT_DATA(WB_PORT_A), PA_BIT(7));
	CHECK(wb_lcd_te_wait(&l, 1000) == WB_LCD_OK);

	fake_mmr_clear_log();
	fake_mmr_set(PORT_DATA(WB_PORT_A), PA_BIT(7));
	CHECK(wb_lcd_te_wait(&l, 50) == WB_LCD_TIMEOUT);
	fake_mmr_set(PORT_DATA(WB_PORT_A), 0);
	CHECK(wb_lcd_te_wait(&l, 50) == WB_LCD_TIMEOUT);
	for (i = 0; i < 3; i++)
		fake_mmr_queue_read(PORT_DATA(WB_PORT_A), 0);
	fake_mmr_queue_read(PORT_DATA(WB_PORT_A), PA_BIT(7) | PA_BIT(5));
	CHECK(wb_lcd_te_wait(&l, 1000) == WB_LCD_OK);
}

/* S-13c.016, S-13c.018: the frame DMA, one byte per write cycle to one
 * address; busy until the destination is done; nothing new meanwhile. */
static void test_frame(void)
{
	static uint8_t fb[64];
	const unsigned src = WB_DMA_MDMA_SRC(2), dst = WB_DMA_MDMA_DST(2);
	struct wb_lcd l;
	long d, s;

	reset();
	(void)wb_lcd_init(&l, &wb_lcd_opxy, &cclk400);
	fake_mmr_clear_log();
	hold = true;
	CHECK(wb_lcd_frame_start(&l, fb, sizeof fb) == WB_LCD_OK);
	CHECK(find_w32(0, DMA_ADDRSTART(dst), BANK) >= 0);
	CHECK(find_w32(0, DMA_XCNT(dst), sizeof fb) >= 0);
	CHECK(find_w32(0, DMA_XMOD(dst), 0) >= 0);
	d = find_w32(0, DMA_CFG(dst), DMA_CFG_EN | DMA_CFG_WNR |
		     DMA_CFG_INT_X);
	s = find(0, FAKE_MMR_WRITE, 32u, DMA_CFG(src), 0, true);
	CHECK(d >= 0 && s > d);			/* destination first */
	CHECK(fake_mmr_log()[s].value == DMA_CFG_EN);	/* 1-byte reads */
	CHECK(find_w32(0, DMA_ADDRSTART(src),
		       (uint32_t)(uintptr_t)fb) >= 0);
	CHECK(find_w32(0, DMA_XMOD(src), 1) >= 0);

	fake_mmr_set(DMA_STAT(dst), 1u << 8);	/* running */
	CHECK(wb_lcd_frame_busy(&l));
	CHECK(wb_lcd_frame_start(&l, fb, sizeof fb) == WB_LCD_BUSY);
	{
		uint8_t out[2];

		CHECK(wb_lcd_read(&l, 0xD3, out, 2) == WB_LCD_ERR_ARG);
	}
	fake_mmr_set(DMA_STAT(dst), DMA_STAT_IRQDONE);
	CHECK(!wb_lcd_frame_busy(&l));
	CHECK(fake_mmr_get(DMA_CFG(dst)) == 0 &&
	      fake_mmr_get(DMA_CFG(src)) == 0);

	CHECK(wb_lcd_frame_start(&l, fb, 0) == WB_LCD_ERR_ARG);
	CHECK(fake_mmr_errors() == 0);
}

/* S-13c.024: a frame given up: the source stopped first, then the
 * destination; no frame afterwards, and a new one may start. */
static void test_frame_stop(void)
{
	static uint8_t fb[64];
	const unsigned src = WB_DMA_MDMA_SRC(2), dst = WB_DMA_MDMA_DST(2);
	struct wb_lcd l;
	long d, s;

	reset();
	(void)wb_lcd_init(&l, &wb_lcd_opxy, &cclk400);
	wb_lcd_frame_stop(&l);			/* none running: nothing */
	fake_mmr_clear_log();
	wb_lcd_frame_stop(&l);
	CHECK(fake_mmr_count() == 0);
	hold = true;
	CHECK(wb_lcd_frame_start(&l, fb, sizeof fb) == WB_LCD_OK);
	CHECK(wb_lcd_frame_busy(&l));		/* not done */
	fake_mmr_clear_log();
	wb_lcd_frame_stop(&l);
	s = find_w32(0, DMA_CFG(src), 0);
	d = find_w32(0, DMA_CFG(dst), 0);
	CHECK(s >= 0 && d > s);			/* the source first */
	CHECK(!wb_lcd_frame_busy(&l));
	hold = false;
	CHECK(wb_lcd_frame_start(&l, fb, sizeof fb) == WB_LCD_OK);
	CHECK(fake_mmr_errors() == 0);
}

static void test_reset_pin(void)
{
	struct wb_lcd l;

	reset();
	(void)wb_lcd_init(&l, &wb_lcd_opxy, &cclk400);
	fake_mmr_clear_log();
	wb_lcd_reset(&l, false);
	CHECK(find_w32(0, PORT_DATA_CLR(WB_PORT_A), PA_BIT(8)) == 0);
	CHECK(find_w32(0, PORT_DIR_SET(WB_PORT_A), PA_BIT(8)) == 1);
	wb_lcd_reset(&l, true);
	CHECK(find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(8)) > 1);
}

int main(void)
{
	RUN(test_timing);
	RUN(test_init);
	RUN(test_cmd_data);
	RUN(test_read);
	RUN(test_te);
	RUN(test_frame);
	RUN(test_frame_stop);
	RUN(test_reset_pin);
	return check_summary("test_drv_lcd");
}
