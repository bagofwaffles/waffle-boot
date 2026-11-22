/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tests for the bit-banged panel bus of the test OS's gpio variant
 * (src/drivers/bf70x/lcd_gpio.c, built only with OPXY_TEST_OS and
 * OPXY_TESTOS_PANEL_GPIO): the source is included here with those macros,
 * so the test does not depend on the host library's lcd.o (the same
 * interface, which this object then defines; the pins are lcd_pins.o's).
 *
 * What the gpio variant must do, as these tests check it:
 * every pin a GPIO with its FER bit cleared
 * only after its level and direction are set; a byte as a WR pulse on PA14
 * through PORTA_DATA with D[7:0] on PB0-PB7 bit-reversed; no SMC access.
 */
#define OPXY_TEST_OS 1
#define OPXY_TESTOS_PANEL_GPIO 1
#include "../../src/drivers/bf70x/lcd_gpio.c"

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

static const struct wb_time cclk400 = { 400u };

#define PA_BIT(n) (1u << (n))

static void reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
}

/* Index of the first access matching op, width, addr and value at or
 * after from; -1 when none. */
static long find(size_t from, enum fake_mmr_op op, unsigned width,
		 wb_mmr_addr addr, uint32_t value)
{
	const struct fake_mmr_access *a = fake_mmr_log();
	size_t i, n = fake_mmr_count();

	for (i = from; i < n; i++)
		if (a[i].op == op && a[i].width == width && a[i].addr == addr &&
		    a[i].value == value)
			return (long)i;
	return -1;
}

static long find_w32(size_t from, wb_mmr_addr addr, uint32_t value)
{
	return find(from, FAKE_MMR_WRITE, 32u, addr, value);
}

static bool any_access_to(wb_mmr_addr lo, wb_mmr_addr hi)
{
	const struct fake_mmr_access *a = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (a[i].op != FAKE_MMR_SYNC && a[i].op != FAKE_MMR_CSYNC &&
		    a[i].addr >= lo && a[i].addr < hi)
			return true;
	return false;
}

/*
 * The bytes strobed out, in order, read back from the PORTB_DATA_SET
 * writes that precede each WR falling edge (the set bits, un-reversed).
 */
static size_t strobed(uint8_t *out, size_t max)
{
	const struct fake_mmr_access *a = fake_mmr_log();
	size_t i, n = 0;
	uint32_t set = 0, clr = 0;
	bool have = false;

	for (i = 0; i < fake_mmr_count(); i++) {
		if (a[i].op != FAKE_MMR_WRITE)
			continue;
		if (a[i].addr == PORT_DATA_SET(WB_PORT_B)) {
			set = a[i].value;
			have = true;
		} else if (a[i].addr == PORT_DATA_CLR(WB_PORT_B)) {
			clr = a[i].value;
		} else if (a[i].addr == PORT_DATA_CLR(WB_PORT_A) &&
			   a[i].value == PA_BIT(14)) {
			CHECK(have);
			CHECK((set & clr) == 0 && (set | clr) == 0xFFu);
			if (n < max)
				out[n] = rev8[set];
			n++;
			have = false;
		}
	}
	return n;
}

static void test_reverse_table(void)
{
	unsigned b, i;

	for (b = 0; b < 256u; b++) {
		unsigned r = 0;

		for (i = 0; i < 8u; i++)
			if (b & (1u << i))
				r |= 0x80u >> i;
		CHECK(rev8[b] == r);
	}
}

/* Each pin: level, direction, then FER cleared; D[7:0] likewise; no SMC
 * register and no PORT_MUX is written. */
static void test_init(void)
{
	struct wb_lcd l;
	long lvl, dir, fer;

	reset();
	CHECK(wb_lcd_init(&l, &wb_lcd_opxy, &cclk400) == WB_LCD_OK);
	/* WR: high, output, then GPIO (the SMC's pin in S-13b.003). */
	lvl = find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(14));
	dir = find_w32(0, PORT_DIR_SET(WB_PORT_A), PA_BIT(14));
	fer = find_w32(0, PORT_FER_CLR(WB_PORT_A), PA_BIT(14));
	CHECK(lvl >= 0 && dir > lvl && fer > dir);
	/* D/C, RD, CS high; TE an input; each handed to the port. */
	CHECK(find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(5)) >= 0);
	CHECK(find_w32(0, PORT_FER_CLR(WB_PORT_A), PA_BIT(5)) >= 0);
	CHECK(find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(13)) >= 0);
	CHECK(find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(15)) >= 0);
	CHECK(find_w32(0, PORT_FER_CLR(WB_PORT_A), PA_BIT(15)) >= 0);
	CHECK(find_w32(0, PORT_INEN_SET(WB_PORT_A), PA_BIT(7)) >= 0);
	CHECK(find_w32(0, PORT_FER_CLR(WB_PORT_A), PA_BIT(7)) >= 0);
	/* The data pins: a level, outputs, then GPIO. */
	dir = find_w32(0, PORT_DIR_SET(WB_PORT_B), 0xFFu);
	fer = find_w32(0, PORT_FER_CLR(WB_PORT_B), 0xFFu);
	CHECK(dir >= 0 && fer > dir);
	CHECK(find_w32(0, PORT_DATA_CLR(WB_PORT_B), 0xFFu) < dir);
	/* Nothing set to a peripheral function, no MUX, no SMC. */
	CHECK(find(0, FAKE_MMR_WRITE, 32u, PORT_FER_SET(WB_PORT_A), PA_BIT(14)) < 0);
	CHECK(find(0, FAKE_MMR_WRITE, 32u, PORT_FER_SET(WB_PORT_B), 0xFFu) < 0);
	CHECK(!any_access_to(PORT_MUX(WB_PORT_A), PORT_MUX(WB_PORT_A) + 4u));
	CHECK(!any_access_to(PORT_MUX(WB_PORT_B), PORT_MUX(WB_PORT_B) + 4u));
	CHECK(!any_access_to(SMC0_BASE, SMC0_BASE + 0x100u));
	CHECK(!any_access_to(WB_LCD_SMC_BANK0, WB_LCD_SMC_BANK0 + 0x1000u));
	/* RESET is not driven by the set-up (S-13c.004). */
	CHECK(find(0, FAKE_MMR_WRITE, 32u, PORT_DIR_SET(WB_PORT_A), PA_BIT(8)) < 0);
	CHECK(fake_mmr_errors() == 0);

	reset();
	{
		struct wb_lcd_config bad = wb_lcd_opxy;

		bad.data_port = WB_PORTS;
		CHECK(wb_lcd_init(&l, &bad, &cclk400) == WB_LCD_ERR_ARG);
		CHECK(fake_mmr_count() == 0);
	}
}

/* S-13c.002: a command with D/C low, parameters with D/C high, each byte
 * one WR pulse with D[7:0] set before WR falls; the bytes bit-reversed on
 * PORTB; CS low first. */
static void test_cmd_data(void)
{
	static const uint8_t p[] = { 0x8A, 0x07, 0x3B };
	const struct fake_mmr_access *log;
	struct wb_lcd l;
	uint8_t got[8] = { 0 };
	long cs, dc_lo, data, wr_lo, wr_hi, dc_hi;

	reset();
	(void)wb_lcd_init(&l, &wb_lcd_opxy, &cclk400);
	fake_mmr_clear_log();
	wb_lcd_select(&l, true);
	wb_lcd_cmd(&l, 0xB6);
	wb_lcd_data(&l, p, sizeof p);
	wb_lcd_data(&l, p, 1);		/* D/C already high: no change */

	CHECK(strobed(got, sizeof got) == 5u);
	CHECK(got[0] == 0xB6 && got[1] == 0x8A && got[2] == 0x07 &&
	      got[3] == 0x3B && got[4] == 0x8A);
	log = fake_mmr_log();
	cs = find_w32(0, PORT_DATA_CLR(WB_PORT_A), PA_BIT(15));
	dc_lo = find_w32(0, PORT_DATA_CLR(WB_PORT_A), PA_BIT(5));
	/* 0xB6 = 1011 0110 reversed is 0110 1101 = 0x6D */
	data = find_w32(0, PORT_DATA_SET(WB_PORT_B), 0x6Du);
	wr_lo = find_w32(0, PORT_DATA_CLR(WB_PORT_A), PA_BIT(14));
	wr_hi = find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(14));
	dc_hi = find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(5));
	CHECK(cs >= 0 && dc_lo > cs && data > dc_lo && wr_lo > data &&
	      wr_hi > wr_lo && dc_hi > wr_hi);
	CHECK(find_w32(0, PORT_DATA_CLR(WB_PORT_B), 0x92u) > data);
	/* The data is at the pins before WR falls, WR stays low a sync, and
	 * the byte is latched before D/C changes. */
	CHECK(log[wr_lo - 1].op == FAKE_MMR_SYNC);
	CHECK(log[wr_lo + 1].op == FAKE_MMR_SYNC);
	CHECK(log[wr_hi + 1].op == FAKE_MMR_SYNC);
	CHECK(log[dc_lo - 1].op == FAKE_MMR_SYNC);
	CHECK(log[dc_hi - 1].op == FAKE_MMR_SYNC);
	CHECK(find_w32((size_t)dc_hi + 1, PORT_DATA_SET(WB_PORT_A), PA_BIT(5)) < 0);
	/* Still no SMC access. */
	CHECK(!any_access_to(WB_LCD_SMC_BANK0, WB_LCD_SMC_BANK0 + 0x1000u));

	fake_mmr_clear_log();
	{
		static const uint8_t px[2] = { 0x12, 0x34 };

		wb_lcd_repeat(&l, px, 2, 3);
		CHECK(strobed(got, sizeof got) == 6u);
		CHECK(got[0] == 0x12 && got[1] == 0x34 && got[4] == 0x12 &&
		      got[5] == 0x34);
	}
}

/* S-13c.011: the data pins become inputs for the strobes and outputs
 * again after, their function untouched; PBn is D(7-n). */
static void test_read(void)
{
	struct wb_lcd l;
	uint8_t out[4] = { 0 };
	long cs_hi, cs_lo, cmd, dir_clr, rd_lo, dir_set, cs_end;

	reset();
	(void)wb_lcd_init(&l, &wb_lcd_opxy, &cclk400);
	wb_lcd_select(&l, true);
	fake_mmr_clear_log();
	fake_mmr_queue_read(PORT_DATA(WB_PORT_B), 0x00u);
	fake_mmr_queue_read(PORT_DATA(WB_PORT_B), 0xEEu);	/* D = 0x77 */
	fake_mmr_queue_read(PORT_DATA(WB_PORT_B), 0x69u);	/* D = 0x96 */
	fake_mmr_queue_read(PORT_DATA(WB_PORT_B), 0xFF01u);	/* D = 0x80 */
	CHECK(wb_lcd_read(&l, 0x04, out, 4) == WB_LCD_OK);
	CHECK(out[0] == 0x00 && out[1] == 0x77 && out[2] == 0x96 &&
	      out[3] == 0x80);

	cs_hi = find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(15));
	cs_lo = find_w32((size_t)cs_hi, PORT_DATA_CLR(WB_PORT_A), PA_BIT(15));
	cmd = find_w32((size_t)cs_lo, PORT_DATA_CLR(WB_PORT_A), PA_BIT(14));
	dir_clr = find_w32((size_t)cmd, PORT_DIR_CLR(WB_PORT_B), 0xFFu);
	rd_lo = find_w32((size_t)dir_clr, PORT_DATA_CLR(WB_PORT_A), PA_BIT(13));
	dir_set = find_w32((size_t)rd_lo, PORT_DIR_SET(WB_PORT_B), 0xFFu);
	cs_end = find_w32((size_t)dir_set, PORT_DATA_SET(WB_PORT_A), PA_BIT(15));
	CHECK(cs_hi >= 0 && cs_lo > cs_hi && cmd > cs_lo && dir_clr > cmd &&
	      rd_lo > dir_clr && dir_set > rd_lo && cs_end > dir_set);
	CHECK(find_w32((size_t)dir_clr, PORT_INEN_SET(WB_PORT_B), 0xFFu) > dir_clr);
	CHECK(find_w32((size_t)rd_lo, PORT_INEN_CLR(WB_PORT_B), 0xFFu) > rd_lo);
	CHECK(find(0, FAKE_MMR_WRITE, 32u, PORT_FER_SET(WB_PORT_B), 0xFFu) < 0);
	CHECK(find(0, FAKE_MMR_WRITE, 32u, PORT_FER_CLR(WB_PORT_B), 0xFFu) < 0);
	CHECK(fake_mmr_errors() == 0);

	CHECK(wb_lcd_read(&l, 0x04, out, 0) == WB_LCD_ERR_ARG);
}

/* S-13c.016: a frame goes out whole in the foreground, byte by byte, D/C
 * high, and is never busy. */
static void test_frame(void)
{
	static const uint8_t buf[6] = { 0xF8, 0x00, 0x07, 0xE0, 0x00, 0x1F };
	struct wb_lcd l;
	uint8_t got[8] = { 0 };

	reset();
	(void)wb_lcd_init(&l, &wb_lcd_opxy, &cclk400);
	wb_lcd_select(&l, true);
	wb_lcd_cmd(&l, 0x2C);
	fake_mmr_clear_log();
	CHECK(wb_lcd_frame_start(&l, buf, sizeof buf) == WB_LCD_OK);
	CHECK(!wb_lcd_frame_busy(&l));
	CHECK(strobed(got, sizeof got) == 6u);
	CHECK(got[0] == 0xF8 && got[1] == 0x00 && got[2] == 0x07 &&
	      got[3] == 0xE0 && got[4] == 0x00 && got[5] == 0x1F);
	CHECK(find_w32(0, PORT_DATA_SET(WB_PORT_A), PA_BIT(5)) >= 0);	/* D/C high first */
	CHECK(!any_access_to(0x20000000u, 0x20040000u));	/* no DMA, no SMC */
	fake_mmr_clear_log();
	wb_lcd_frame_stop(&l);			/* the frame has ended: nothing */
	CHECK(fake_mmr_count() == 0);
	CHECK(wb_lcd_frame_start(&l, buf, 0) == WB_LCD_ERR_ARG);
}

int main(void)
{
	RUN(test_reverse_table);
	RUN(test_init);
	RUN(test_cmd_data);
	RUN(test_read);
	RUN(test_frame);
	return check_summary("test_drv_lcd_gpio");
}
