/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The display (src/display_draw.c, src/display_font.c): the draw
 * interface on the frame buffer, the declared font, the hand-over frame,
 * the shutdown and fatal-error frames, over a recording bus.
 * Requirements: S-15.002, S-15.003, S-15.132, S-13c.021, S-13c.022,
 * S-13c.026.
 */
#include <stdio.h>
#include <string.h>

#include "support/check.h"
#include "s15_sha256.h"
#include "waffle/device.h"
#include "waffle/display.h"

/* tests/oracle/S-15-menus/vectors/handover_frame.json: the logo alone */
#define HANDOVER_SHA "9e50c21e72a8979051e31a3046e98383f6bf81db56a907a210847efd4455c717"
#define HANDOVER_WHITE 784u
#define HANDOVER_FAINT 1152u
/* tests/oracle/S-13c-display/vectors/orientation.json */
#define ORIENT_SHA "bcbcc90784c60c625c6e9bf3d1cac21f642f3d14b1ce372a329444554f8d4bef"
#define ORIENT_SHA_MIRRORED "bce3a14e1975ef922ebcd0d8314f1b7b9e250a7f861f6342bf1a3e633ccfa3a6"

struct fake {
	uint64_t t;
	char log[256];
	size_t len;
	unsigned busy_polls;	/* frame_busy answers true this often */
	unsigned frames;
	struct s15_sha256 sha;
	unsigned long data_bytes;
	unsigned stops;		/* frame_stop calls */
	bool stop_same;		/* the buffer as the frame started, at the stop */
};

static struct fake g;
static uint8_t fb[S13C_FRAME_BYTES];
static uint8_t sent[S13C_FRAME_BYTES];	/* the last frame's bytes */
static uint8_t at_start[S13C_FRAME_BYTES];	/* the buffer as the last DMA frame started */

static void note(const char *s)
{
	size_t n = strlen(s);

	if (g.len + n + 1u < sizeof g.log) {
		memcpy(g.log + g.len, s, n + 1u);
		g.len += n;
	}
}

static void f_reset(void *ctx, bool high)
{
	(void)ctx;
	(void)high;
}

static void f_select(void *ctx, bool on)
{
	(void)ctx;
	(void)on;
}

static void f_cmd(void *ctx, uint8_t c)
{
	char b[8];

	(void)ctx;
	snprintf(b, sizeof b, "%02X ", c);
	note(b);
	if (c == S13C_RAMWR) {		/* hash the last frame's bytes */
		s15_sha256_init(&g.sha);
		g.data_bytes = 0;
	}
}

static void f_data(void *ctx, const uint8_t *p, size_t n)
{
	size_t i;

	(void)ctx;
	for (i = 0; i < n; i++) {
		unsigned long o = g.data_bytes + i;

		s15_sha256_byte(&g.sha, p[i]);
		if (o < sizeof sent)
			sent[o] = p[i];
	}
	g.data_bytes += n;
}

static void f_repeat(void *ctx, const uint8_t *group, size_t len,
		     uint32_t count)
{
	(void)ctx;
	(void)group;
	(void)len;
	(void)count;
}

static int f_frame_start(void *ctx, const uint8_t *buf, uint32_t n)
{
	(void)ctx;
	CHECK(n == S13C_FRAME_BYTES);
	memcpy(at_start, buf, sizeof at_start);
	g.frames++;
	note("dma ");
	return 0;
}

static bool f_frame_busy(void *ctx)
{
	(void)ctx;
	if (g.busy_polls == 0)
		return false;
	g.busy_polls--;
	return true;
}

static void f_frame_stop(void *ctx)
{
	(void)ctx;
	g.stops++;
	g.stop_same = memcmp(fb, at_start, sizeof fb) == 0;
	g.busy_polls = 0;
}

static int f_te_wait(void *ctx, uint32_t timeout_us)
{
	(void)ctx;
	(void)timeout_us;
	g.t += 100u;
	return 0;
}

static uint64_t f_now_us(void *ctx)
{
	(void)ctx;
	return g.t;
}

static void f_delay_us(void *ctx, uint32_t us)
{
	(void)ctx;
	g.t += us;
}

static const struct s13c_bus bus = {
	f_reset, f_select, f_cmd, f_data, f_repeat, NULL, f_frame_start,
	f_frame_busy, f_frame_stop, f_te_wait, f_now_us, f_delay_us, NULL,
};

/* A bus that cannot stop a frame. */
static const struct s13c_bus bus_nostop = {
	f_reset, f_select, f_cmd, f_data, f_repeat, NULL, f_frame_start,
	f_frame_busy, NULL, f_te_wait, f_now_us, f_delay_us, NULL,
};

static void clear_log(void)
{
	g.len = 0;
	g.log[0] = '\0';
}

static void up(struct wb_display *d, bool gamma)
{
	memset(&g, 0, sizeof g);
	g.t = 50000;
	wb_display_init(d, fb);
	wb_display_begin(d, &bus, &wb_device_opxy, 0, 0, 1);
	CHECK(wb_display_start(d, gamma) == S13C_OK);
	clear_log();
}

/* Window column c, row r. */
static uint16_t px(unsigned c, unsigned r)
{
	const uint8_t *p = &fb[2u * ((size_t)r * S13C_FRAME_W + c)];

	return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}

/* Screen (x, y): window row 479 - x, column y (S-13c.023). */
static uint16_t spx(unsigned x, unsigned y)
{
	return px(y, WB_DISPLAY_W - 1u - x);
}

static bool all_zero(void)
{
	size_t i;

	for (i = 0; i < sizeof fb; i++)
		if (fb[i] != 0)
			return false;
	return true;
}

static const uint8_t *glyph(char ch)
{
	return wb_display_font[(unsigned char)ch - WB_FONT_FIRST];
}

/* Whether glyph ch has ink in rows [from, to). */
static bool ink_in(char ch, unsigned from, unsigned to)
{
	unsigned j;

	for (j = from; j < to; j++)
		if (glyph(ch)[j] != 0u)
			return true;
	return false;
}

/* S-15.003: complete (by its size), pairwise distinct, the space the only
 * blank glyph; Spleen 8x16's rows with bit 7 the leftmost pixel and rows
 * 12 - 15 below the baseline; S-15.002: the title
 * bar, the 9 rows and the status bar fit and do not overlap. */
static void test_font(void)
{
	const struct s15_geometry *geo = &wb_display_geometry;
	unsigned a, b, j, distinct = 1;

	CHECK(WB_FONT_W == 8u && WB_FONT_H == 16u && WB_FONT_COUNT == 95u &&
	      WB_FONT_FIRST == 0x20u);
	for (a = 0; a < WB_FONT_COUNT; a++) {
		unsigned ink = 0;

		for (j = 0; j < WB_FONT_H; j++)
			ink |= wb_display_font[a][j];
		CHECK((ink != 0) == (a != 0));
		for (b = a + 1u; b < WB_FONT_COUNT; b++)
			if (memcmp(wb_display_font[a], wb_display_font[b],
				   WB_FONT_H) == 0)
				distinct = 0;
	}
	CHECK(distinct);
	/* L's stem is the left column, its foot ends on the baseline */
	for (j = 2; j < 11; j++)
		CHECK((glyph('L')[j] & 0x80u) != 0u && (glyph('L')[j] & 0x01u) == 0u);
	CHECK(!ink_in('L', 12, 16) && !ink_in('A', 12, 16));
	CHECK(ink_in('g', 12, 16) && ink_in('p', 12, 16));	/* descenders */
	CHECK(ink_in('_', 12, 16) && !ink_in('_', 0, 12));
	CHECK((glyph('T')[2] & 0xC0u) == 0xC0u);	/* T's bar from x 0 */

	CHECK(s15_geometry_ok(geo));
	CHECK(geo->width == WB_DISPLAY_W && geo->height == WB_DISPLAY_H);
	CHECK(geo->glyph_w == WB_FONT_W && geo->glyph_h == WB_FONT_H);
	/* wiki/Screen-Style-Guide.md: the bar y 0 - 23, rows 19 apart from y 33
	 * at x 16, the rule at y 202, the bars' text from x 8 */
	CHECK(geo->edge == 8u && geo->margin == 16u && geo->top == 33u &&
	      geo->pitch == 19u && geo->bar_h == 24u && geo->status_y == 202u);
	CHECK(s15_row_y(geo, S15_ROWS - 1u) + geo->glyph_h == 201u &&
	      s15_status_text_y(geo) == 205u &&
	      s15_status_text_y(geo) + geo->glyph_h <= WB_DISPLAY_H);
	CHECK(geo->margin + S15_COLS * geo->glyph_w == 464u);
}

/* S-15.003: the title bar's glyphs: pairwise distinct, the space the only
 * blank one, each letter and digit heavier than its plain glyph, the last
 * column clear so that letters keep their gap. */
static void test_font_title(void)
{
	unsigned a, b, j, distinct = 1;

	CHECK(wb_display_geometry.title_w == WB_TITLE_W &&
	      wb_display_geometry.title_h == WB_TITLE_H &&
	      wb_display_geometry.title_y == 4u);
	for (a = 0; a < WB_FONT_COUNT; a++) {
		const uint16_t *rows = wb_display_title_font[a];
		unsigned plain = 0, heavy = 0, last = 0, c = a + WB_FONT_FIRST;

		for (j = 0; j < WB_FONT_H; j++)
			for (b = 0; b < WB_FONT_W; b++)
				plain += (wb_display_font[a][j] >> b) & 1u;
		for (j = 0; j < WB_TITLE_H; j++) {
			for (b = 0; b < WB_TITLE_W; b++)
				heavy += (rows[j] >> b) & 1u;
			last |= rows[j] & 1u;
		}
		CHECK(last == 0u);
		CHECK((heavy != 0u) == (a != 0u));
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
		    (c >= 'a' && c <= 'z'))
			CHECK(heavy > plain);
		for (b = a + 1u; b < WB_FONT_COUNT; b++)
			if (memcmp(rows, wb_display_title_font[b],
				   sizeof wb_display_title_font[0]) == 0)
				distinct = 0;
	}
	CHECK(distinct);
}

/* Whether the glyph of ch lies upright at screen (x, y), in fg on bg. */
static bool glyph_on(unsigned x, unsigned y, char ch, uint16_t fg,
		     uint16_t bg)
{
	const uint8_t *gl = glyph(ch);
	unsigned i, j;

	for (j = 0; j < WB_FONT_H; j++)
		for (i = 0; i < WB_FONT_W; i++) {
			bool on = ((unsigned)gl[j] >> (WB_FONT_W - 1u - i)) & 1u;

			if (spx(x + i, y + j) != (on ? fg : bg))
				return false;
		}
	return true;
}

/* ... in fg on black. */
static bool glyph_at(unsigned x, unsigned y, char ch, uint16_t fg)
{
	return glyph_on(x, y, ch, fg, S15_BLACK);
}

/* S-15.002, S-15.003 */
static void test_text(void)
{
	static struct wb_display d;
	const struct s15_draw_ops *o;
	const struct s15_geometry *geo = &wb_display_geometry;
	const char s[] = "Ag~\x7f";

	up(&d, false);
	o = wb_display_draw(&d);
	o->fill(o->ctx, 0, 0, geo->width, geo->height, 0x1234u);
	o->text(o->ctx, geo->margin, geo->top, s, 4, S15_WHITE, S15_BLACK);
	CHECK(glyph_at(16, 33, 'A', S15_WHITE));
	CHECK(glyph_at(24, 33, 'g', S15_WHITE));
	CHECK(glyph_at(32, 33, '~', S15_WHITE));
	CHECK(glyph_at(40, 33, '?', S15_WHITE));
	CHECK(spx(16, 33 + 16) == 0x1234u);	/* below the cells */
	CHECK(spx(15, 33) == 0x1234u);		/* before them */
	CHECK(spx(48, 33) == 0x1234u);		/* after them */
	/* Text on a filled bar: the background is bg */
	o->text(o->ctx, geo->edge, 2, "Main", 4, S15_BLACK, S15_AMBER);
	CHECK(glyph_on(8, 2, 'M', S15_BLACK, S15_AMBER));
	CHECK(glyph_on(32, 2, 'n', S15_BLACK, S15_AMBER));
	CHECK(spx(40, 2) == 0x1234u);
	/* 56 glyphs end at 464 (S-15.002); the draw op itself clips only at
	 * the width: from x 16 a 58th glyph ends at 480, a 59th is not
	 * drawn */
	o->text(o->ctx, geo->margin, s15_row_y(geo, S15_ROWS - 1u),
		"0123456789012345678901234567890123456789012345678901234567890123",
		64, 0xF800u, S15_BLACK);
	CHECK(glyph_at(16u + 55u * 8u, 185, '5', 0xF800u));
	CHECK(glyph_at(472, 185, '7', 0xF800u));
	CHECK(spx(479, 185 + 16) == 0x1234u);
	/* the bottom rows: the status text's last row is y 220 */
	o->text(o->ctx, geo->edge, s15_status_text_y(geo), "_", 1, S15_WHITE,
		S15_BLACK);
	CHECK(glyph_at(8, 205, '_', S15_WHITE) && spx(8, 221) == 0x1234u);
	/* a glyph that runs past the bottom is clipped there */
	o->text(o->ctx, 100, 214, "g", 1, S15_WHITE, S15_BLACK);
	CHECK(spx(101, 221) == ((glyph('g')[7] & 0x40u) ? S15_WHITE : S15_BLACK));
	/* the frame window's columns 222, 223 stay black */
	CHECK(px(222, 0) == 0 && px(223, 479) == 0);
	CHECK(g.frames == 0 && g.len == 0);	/* drawing sends nothing */
}

/* S-13c.023: the screen's left edge is the frame's last row. */
static void test_orientation(void)
{
	static struct wb_display d;
	const struct s15_draw_ops *o;
	struct s15_sha256 sha;
	char hex[65];
	size_t i;

	up(&d, false);
	o = wb_display_draw(&d);
	o->fill(o->ctx, 0, 0, 1, 1, 0xABCDu);
	CHECK(px(0, 479) == 0xABCDu && px(0, 0) == 0);
	o->fill(o->ctx, 470, 215, 40, 40, 0x00FFu);	/* clipped */
	CHECK(px(221, 0) == 0x00FFu && px(215, 9) == 0x00FFu);
	CHECK(px(214, 9) == 0 && px(215, 10) == 0);
	CHECK(px(222, 0) == 0 && px(223, 0) == 0);

	/* vectors/orientation.json: the F at its screen positions */
	memset(fb, 0, sizeof fb);
	o->fill(o->ctx, 10, 10, 1, 40, S15_WHITE);
	o->fill(o->ctx, 10, 10, 30, 1, S15_WHITE);
	o->fill(o->ctx, 10, 30, 20, 1, S15_WHITE);
	CHECK(px(10, 469) == S15_WHITE && px(49, 469) == S15_WHITE &&
	      px(10, 440) == S15_WHITE && px(30, 450) == S15_WHITE &&
	      px(10, 10) == 0);
	s15_sha256_init(&sha);
	for (i = 0; i < sizeof fb; i++)
		s15_sha256_byte(&sha, fb[i]);
	s15_sha256_hex(&sha, hex);
	CHECK(strcmp(hex, ORIENT_SHA) == 0);
	CHECK(strcmp(hex, ORIENT_SHA_MIRRORED) != 0);
}

/* Screen position (x, y) of a frame in S-13c.015's order (S-13c.023). */
static uint16_t frame_px(const uint8_t *f, unsigned x, unsigned y)
{
	const uint8_t *p = &f[2u * ((size_t)(WB_DISPLAY_W - 1u - x) *
				     S13C_FRAME_W + y)];

	return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}

/* The pixels of color rgb. */
static unsigned count_px(const uint8_t *f, uint16_t rgb)
{
	unsigned x, y, n = 0;

	for (x = 0; x < WB_DISPLAY_W; x++)
		for (y = 0; y < S13C_FRAME_W; y++)
			if (frame_px(f, x, y) == rgb)
				n++;
	return n;
}

/* S-15.132, S-13c.020, S-13c.021 */
static void test_logo(void)
{
	static struct wb_display d;
	static uint8_t again[S13C_FRAME_BYTES];
	uint8_t rec[S13C_RECORD_SIZE];
	char hex[65];

	memset(&g, 0, sizeof g);
	g.t = 50000;
	wb_display_init(&d, fb);
	wb_display_begin(&d, &bus, &wb_device_opxy, S13C_HANDSHAKE_MAGIC, 3, 1);
	CHECK(wb_display_handover(&d, rec) == S13C_ERR_STATE);
	CHECK(wb_display_boot_logo(&d, NULL) == S13C_OK);
	CHECK(d.panel.gamma_on && d.panel.state == S13C_ON);
	CHECK(g.data_bytes == S13C_FRAME_BYTES);
	/* the whole frame is the logo alone: no text */
	s15_sha256_hex(&g.sha, hex);
	CHECK(strcmp(hex, HANDOVER_SHA) == 0);
	CHECK(count_px(sent, S15_WHITE) == HANDOVER_WHITE);
	CHECK(count_px(sent, S15_FAINT) == HANDOVER_FAINT);
	CHECK(count_px(sent, S15_DIM) == 0u);
	CHECK(count_px(sent, S15_BLACK) ==
	      480u * 224u - HANDOVER_WHITE - HANDOVER_FAINT);
	CHECK(all_zero());			/* the buffer is not used */
	wb_display_handover_frame(NULL, again);
	CHECK(memcmp(again, sent, sizeof sent) == 0);
	CHECK(wb_display_handover(&d, rec) == S13C_OK);
	CHECK(rec[0] == 1 && rec[4] == 3 && rec[8] == 2 && rec[0x28] == 1);
}

/* S-15.132: a build-time image in place of the logo, alone. */
static void test_image(void)
{
	static uint8_t data[3u * 834u];
	static uint8_t f[S13C_FRAME_BYTES];
	static const uint8_t lit[] = { 0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x12, 0x34 };
	static const uint8_t cut[] = { 0x80, 0x12 };
	struct wb_display_image im;
	size_t n = 0;
	unsigned left = 222u * 480u, x, y;
	bool ok = true;

	/* every visible pixel 0x1234, in repeat records of 128 and one of 64 */
	while (left != 0u) {
		unsigned k = left > 128u ? 128u : left;

		data[n++] = (uint8_t)(0x7Fu + k);
		data[n++] = 0x12;
		data[n++] = 0x34;
		left -= k;
	}
	im.data = data;
	im.logo = NULL;
	im.len = n;
	wb_display_handover_frame(&im, f);
	for (x = 0; x < WB_DISPLAY_W; x++)
		for (y = 0; y < S13C_FRAME_W; y++) {
			uint16_t want = y >= 222u ? S15_BLACK : 0x1234u;

			ok = ok && frame_px(f, x, y) == want;
		}
	CHECK(ok);

	/* a literal record, then the data ends: black after it */
	im.data = lit;
	im.len = sizeof lit;
	wb_display_handover_frame(&im, f);
	CHECK(f[0] == 0xAA && f[1] == 0xBB && f[2] == 0xCC && f[3] == 0xDD &&
	      f[4] == 0x12 && f[5] == 0x34);
	CHECK(count_px(f, S15_BLACK) == 480u * 224u - 3u);

	/* a record cut short: black, nothing read past the data */
	im.data = cut;
	im.len = sizeof cut;
	wb_display_handover_frame(&im, f);
	CHECK(count_px(f, S15_BLACK) == 480u * 224u);

	/* no data: the logo */
	im.data = NULL;
	im.len = 0;
	wb_display_handover_frame(&im, f);
	CHECK(count_px(f, S15_WHITE) == HANDOVER_WHITE);
}

/* S-13c.022: no frame from the power-off on; the frame in progress ends,
 * or is stopped once S13C_POWEROFF_FRAME_US has run out, and nothing is
 * sent after it: no last frame, no DISPOFF. What was drawn stays undrawn
 * on the panel. */
static void test_poweroff(void)
{
	static struct wb_display d;
	const struct s15_draw_ops *o;
	uint64_t t0;

	up(&d, false);
	o = wb_display_draw(&d);
	o->fill(o->ctx, 0, 0, 40, 40, 0x1234u);
	CHECK(wb_display_flush(&d) == S13C_OK);
	g.busy_polls = 2;			/* still being sent */
	o->text(o->ctx, 6, 4, "bye", 3, S15_WHITE, S15_BLACK);	/* drawn, not sent */
	clear_log();
	CHECK(wb_display_poweroff(&d) == S13C_OK);
	CHECK(g.busy_polls == 0);		/* the frame in progress ended */
	CHECK(g.len == 0 && g.frames == 1);
	CHECK(d.panel.state == S13C_OFF);
	CHECK(wb_display_flush(&d) == S13C_ERR_STATE);
	CHECK(wb_display_update(&d) == S13C_OK && g.frames == 1);
	CHECK(wb_display_poweroff(&d) == S13C_OK && g.len == 0);
	CHECK(g.stops == 0);

	/* a frame that never ends: stopped after the bound, nothing sent */
	up(&d, false);
	o = wb_display_draw(&d);
	o->fill(o->ctx, 0, 0, 40, 40, 0x1234u);
	CHECK(wb_display_flush(&d) == S13C_OK);
	g.busy_polls = ~0u;
	t0 = g.t;
	clear_log();
	CHECK(wb_display_poweroff(&d) == S13C_OK);
	CHECK(g.stops == 1 && g.t - t0 >= S13C_POWEROFF_FRAME_US);
	CHECK(g.len == 0 && g.frames == 1 && !wb_display_busy(&d));
	CHECK(d.panel.state == S13C_OFF);

	/* never started: nothing */
	memset(&g, 0, sizeof g);
	wb_display_init(&d, fb);
	CHECK(wb_display_poweroff(&d) == S13C_OK);
	CHECK(g.len == 0);
}

/* A frame only when something was drawn since the last one. */
static void test_update(void)
{
	static struct wb_display d;
	const struct s15_draw_ops *o;

	memset(&g, 0, sizeof g);
	wb_display_init(&d, fb);
	o = wb_display_draw(&d);
	o->fill(o->ctx, 0, 0, 1, 1, S15_WHITE);
	CHECK(wb_display_update(&d) == S13C_OK && g.frames == 0);	/* off */
	up(&d, false);
	o = wb_display_draw(&d);
	CHECK(wb_display_update(&d) == S13C_OK && g.frames == 0);
	o->fill(o->ctx, 0, 0, 1, 1, S15_WHITE);
	CHECK(wb_display_update(&d) == S13C_OK && g.frames == 1);
	CHECK(wb_display_update(&d) == S13C_OK && g.frames == 1);
}


/* S-13c.021 before the kernel call, frames still allowed */
static void test_record_early(void)
{
	static struct wb_display d;
	uint8_t rec[S13C_RECORD_SIZE];

	memset(&g, 0, sizeof g);
	g.t = 50000;
	wb_display_init(&d, fb);
	wb_display_begin(&d, &bus, &wb_device_opxy, S13C_HANDSHAKE_MAGIC, 3, 1);
	CHECK(wb_display_record(&d, rec) == S13C_ERR_STATE);
	CHECK(wb_display_boot_logo(&d, NULL) == S13C_OK);
	memset(rec, 0xA5, sizeof rec);
	CHECK(wb_display_record(&d, rec) == S13C_OK);
	CHECK(rec[0] == 1 && rec[4] == 3 && rec[8] == 2 && rec[0x28] == 1 &&
	      rec[0x2B] == 0);
	CHECK(d.panel.state == S13C_ON);
	CHECK(wb_display_flush(&d) == S13C_OK);		/* the load failed */
	CHECK(wb_display_record(&d, rec) == S13C_ERR_STATE);
}

/* S-13c.021, S-13c.026: on a device without digital gamma the record is
 * written once the hand-over frame is out, the same 44 bytes. */
static void test_record_1f(void)
{
	static struct wb_display d;
	uint8_t rec[S13C_RECORD_SIZE];
	unsigned i, other = 0;

	memset(&g, 0, sizeof g);
	g.t = 50000;
	wb_display_init(&d, fb);
	wb_display_begin(&d, &bus, &wb_device_op1f, S13C_HANDSHAKE_MAGIC, 3, 1);
	CHECK(!d.panel.digital_gamma);
	CHECK(wb_display_record(&d, rec) == S13C_ERR_STATE);
	CHECK(wb_display_boot_logo(&d, NULL) == S13C_OK);
	CHECK(!d.panel.gamma_on && s13c_gamma_ready(&d.panel));
	memset(rec, 0xA5, sizeof rec);
	CHECK(wb_display_record(&d, rec) == S13C_OK);
	CHECK(rec[0x00] == 1 && rec[0x28] == 1);
	CHECK(rec[0x04] == 3 && rec[0x05] == 0 && rec[0x06] == 0 && rec[0x07] == 0);
	CHECK(rec[0x08] == 2 && rec[0x09] == 0 && rec[0x0A] == 0 && rec[0x0B] == 0);
	for (i = 0; i < S13C_RECORD_SIZE; i++)
		if (i != 0x00 && i != 0x04 && i != 0x08 && i != 0x28)
			other += rec[i] != 0u;
	CHECK(other == 0);
	CHECK(d.panel.state == S13C_ON);
	/* the same bytes as the kernel call's record */
	{
		uint8_t at_call[S13C_RECORD_SIZE];

		CHECK(wb_display_handover(&d, at_call) == S13C_OK);
		CHECK(memcmp(at_call, rec, sizeof rec) == 0);
	}
}

/* One frame at once, none after it */
static void test_fatal(void)
{
	static struct wb_display d;

	up(&d, false);
	CHECK(wb_display_flush(&d) == S13C_OK);
	g.busy_polls = 3;			/* the held frame still runs */
	clear_log();
	CHECK(wb_display_flush_fatal(&d) == S13C_OK);
	CHECK(g.busy_polls == 0 && !wb_display_busy(&d));
	CHECK(strcmp(g.log, "2C dma ") == 0);
	CHECK(wb_display_flush(&d) == S13C_ERR_STATE);
	CHECK(wb_display_flush_fatal(&d) == S13C_ERR_STATE);
	clear_log();
	CHECK(wb_display_poweroff(&d) == S13C_OK);
	CHECK(g.len == 0);			/* nothing after the fatal frame */
}

/* Drawing waits for the frame in flight to end. */
static void test_settle(void)
{
	static struct wb_display d;
	const struct s15_draw_ops *o;

	up(&d, false);
	o = wb_display_draw(&d);
	CHECK(wb_display_flush(&d) == S13C_OK);
	g.busy_polls = 5;
	CHECK(wb_display_busy(&d));
	o->fill(o->ctx, 0, 0, 1, 1, S15_WHITE);
	CHECK(g.busy_polls == 0 && spx(0, 0) == S15_WHITE);
}

/*
 * S-13c.024 when the frame in flight never ends: the draw waits out the
 * frame's timeout, then stops the frame before it writes the buffer, and
 * the next frame is the whole new image. On a bus that cannot stop a
 * frame the draw is dropped: the bytes being sent stay as they are.
 */
static void test_settle_timeout(void)
{
	static struct wb_display d;
	const struct s15_draw_ops *o;
	uint64_t t0;

	up(&d, false);
	o = wb_display_draw(&d);
	o->fill(o->ctx, 0, 0, 40, 40, 0x1234u);
	CHECK(wb_display_flush(&d) == S13C_OK);
	g.busy_polls = ~0u;			/* never ends */
	t0 = g.t;
	o->fill(o->ctx, 0, 0, 1, 1, S15_WHITE);
	CHECK(g.stops == 1 && g.stop_same);	/* stopped before the write */
	CHECK(g.t - t0 >= S13C_FRAME_TIMEOUT_US);
	CHECK(spx(0, 0) == S15_WHITE && spx(1, 1) == 0x1234u);
	CHECK(!wb_display_busy(&d));
	o->text(o->ctx, 8, 8, "x", 1, S15_WHITE, S15_BLACK);
	CHECK(g.stops == 1);			/* nothing in flight: no stop */
	CHECK(wb_display_update(&d) == S13C_OK && g.frames == 2);
	CHECK(memcmp(at_start, fb, sizeof fb) == 0);

	memset(&g, 0, sizeof g);
	g.t = 50000;
	wb_display_init(&d, fb);
	wb_display_begin(&d, &bus_nostop, &wb_device_opxy, 0, 0, 1);
	CHECK(wb_display_start(&d, false) == S13C_OK);
	o = wb_display_draw(&d);
	o->fill(o->ctx, 0, 0, 40, 40, 0x1234u);
	CHECK(wb_display_flush(&d) == S13C_OK);
	g.busy_polls = ~0u;
	o->fill(o->ctx, 0, 0, 1, 1, S15_WHITE);
	o->text(o->ctx, 8, 8, "x", 1, S15_WHITE, S15_BLACK);
	CHECK(memcmp(at_start, fb, sizeof fb) == 0);	/* not written */
	CHECK(g.stops == 0 && wb_display_busy(&d));
	g.busy_polls = 0;
}

int main(void)
{
	RUN(test_font);
	RUN(test_font_title);
	RUN(test_text);
	RUN(test_orientation);
	RUN(test_logo);
	RUN(test_image);
	RUN(test_poweroff);
	RUN(test_fatal);
	RUN(test_settle);
	RUN(test_settle_timeout);
	RUN(test_update);
	RUN(test_record_early);
	RUN(test_record_1f);
	return check_summary("test_display_draw");
}
