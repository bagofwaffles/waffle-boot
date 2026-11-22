/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's screens (src/front/fr_draw.c): the logo screen, the logo
 * alone, against S-15.132's frame and digest; the text screen's bars,
 * rows and inks against held mode's drawing of the same parts
 * (s15_render_frame, s15_render_row); the painter's frame buffer against
 * the host's model pixel by pixel, for every screen here and every
 * recovery screen the tests build, and a repaint of the rows that changed
 * against a whole paint; the window rows' order and black columns
 * (S-13c.023, S-13c.014); chips, key badges, the title font on a row and
 * the clip; the logo line (S-13c.025); the font copy; the text helpers;
 * recovery's screen in each state against S-15.151's required information
 * (the tokens of S-15's info_tokens.json, where it names one).
 */
#include <string.h>

#include "support/check.h"

#define S15V_TOKENS
#define S15V_HANDOVER
#include "vectors/s15_vectors.h"
#include "s15_tokens.h"

#include "front/fr_draw.h"
#include "front/fr_font.h"
#include "front/fr_panel.h"
#include "front/fr_proto.h"
#include "front/fr_start.h"
#include "waffle/display.h"
#include "waffle/s11_version.h"
#include "waffle/s15_console.h"
#include "waffle/s15_lines.h"

static uint8_t fb[S13C_FRAME_BYTES], frame[S13C_FRAME_BYTES], inc[S13C_FRAME_BYTES];
static struct wb_display disp;

/* Held mode's pixel at screen (x, y) in a frame buffer. */
static uint16_t at(const uint8_t *f, unsigned x, unsigned y)
{
	const uint8_t *p = f + 2u * ((size_t)(479u - x) * S13C_FRAME_W + y);

	return (uint16_t)(p[0] << 8 | p[1]);
}

static void rows_of(const struct fr_screen *s, uint8_t *out)
{
	CHECK(FR_FRAME_BYTES == S13C_FRAME_BYTES && FR_PANEL_FRAME_BYTES == S13C_FRAME_BYTES);
	memset(out, 0xA5, S13C_FRAME_BYTES);	/* nothing left of what was there */
	fr_paint(out, s);
}

static void test_font_copy(void)
{
	CHECK(FR_FONT_COUNT == WB_FONT_COUNT && FR_FONT_W == WB_FONT_W &&
	      FR_FONT_H == WB_FONT_H && FR_TITLE_W == WB_TITLE_W &&
	      FR_TITLE_H == WB_TITLE_H && FR_FONT_FIRST == WB_FONT_FIRST);
	CHECK(memcmp(fr_font, wb_display_font, sizeof fr_font) == 0);
	wb_display_title_font_build();
	CHECK(memcmp(fr_title_font, wb_display_title_font, sizeof fr_title_font) == 0);
}

static void test_text_helpers(void)
{
	static const uint16_t v[][4] = {
		{ 1, 0, 0, 0 }, { 0, 9, 10, 0 }, { 1, 2, 3, 4 },
		{ 65535, 65535, 65535, 65535 },
	};
	char a[FR_VERSION_TEXT_MAX + 1u], b[S11_VERSION_TEXT_MAX + 1u];

	for (unsigned i = 0; i < ARRAY_LEN(v); i++) {
		struct s11_version sv = { v[i][0], v[i][1], v[i][2], v[i][3] };
		size_t n = fr_text_version(a, v[i]);
		size_t m = s11_version_text(&sv, b, S11_VERSION_TEXT_MAX);

		CHECK(n <= FR_VERSION_TEXT_MAX);
		CHECK(n == m + 1u && a[0] == 'v' && memcmp(a + 1, b, m) == 0);
	}
	CHECK(fr_text_dec(a, 0) == 1u && a[0] == '0');
	CHECK(fr_text_dec(a, 4294967295u) == 10u && memcmp(a, "4294967295", 10) == 0);
}

/* S-15.132: the early logo is the step row alone, with no name and no
 * version: every pixel s15_logo_pixel()'s, the frame's digest the one
 * handover_frame.json gives, and nothing kept in the status texts. */
static void test_logo(void)
{
	struct fr_screen s;
	struct fr_sha256 sha;
	uint8_t d[FR_SHA256_LEN];
	char hex[2u * FR_SHA256_LEN + 1u];
	unsigned bad = 0, white = 0, faint = 0;

	memset(&s, 0x5A, sizeof s);
	fr_screen_logo(&s);
	CHECK(s.kind == FR_SCREEN_LOGO && s.status_right[0] == '\0' && s.status_left[0] == '\0');
	CHECK(s.title[0] == '\0' && s.bar_line < 0);
	rows_of(&s, frame);
	fr_sha256_init(&sha);
	fr_sha256_add(&sha, frame, sizeof frame);
	fr_sha256_end(&sha, d);
	for (unsigned i = 0; i < FR_SHA256_LEN; i++) {
		hex[2u * i] = "0123456789abcdef"[d[i] >> 4];
		hex[2u * i + 1u] = "0123456789abcdef"[d[i] & 15u];
	}
	hex[2u * FR_SHA256_LEN] = '\0';
	CHECK(strcmp(hex, s15v_handover_sha256) == 0);
	CHECK(sizeof frame == S15V_HANDOVER_BYTES);
	for (unsigned x = 0; x < FR_SCREEN_W; x++)
		for (unsigned y = 0; y < FR_SCREEN_H; y++) {
			const uint16_t p = at(frame, x, y);

			bad += p != s15_logo_pixel(x, y) ? 1u : 0u;
			white += p == 0xFFFFu ? 1u : 0u;
			faint += p != 0u && p != 0xFFFFu ? 1u : 0u;
		}
	CHECK(bad == 0u);
	CHECK(white == S15V_HANDOVER_WHITE && faint == S15V_HANDOVER_FAINT);
	/* a text screen before it leaves nothing behind */
	fr_screen_text(&s, "Recovery");
	fr_screen_line(&s, 8, FR_INK_TEXT, "waffle-boot v1.0.0");
	fr_screen_status(&s, "USB", "waffle-boot v1.0.0");
	fr_screen_logo(&s);
	rows_of(&s, fb);
	CHECK(memcmp(fb, frame, sizeof fb) == 0);
}

/* Held mode's red title bar and its status bar, with the same title. */
static void held_frame(const char *title)
{
	struct s15_frame f;

	memset(&f, 0, sizeof f);
	f.title = title;
	f.fatal = true;
	wb_display_init(&disp, fb);
	s15_render_frame(&f, &wb_display_geometry, wb_display_draw(&disp));
}

static void test_bars(void)
{
	static const char *const titles[] = { "Recovery", "waffle-boot", "Ig {Wq}~" };
	const struct s15_geometry *g = &wb_display_geometry;

	for (unsigned t = 0; t < ARRAY_LEN(titles); t++) {
		struct fr_screen s;
		unsigned bad = 0;

		fr_screen_text(&s, titles[t]);
		held_frame(titles[t]);
		rows_of(&s, frame);
		for (unsigned x = 0; x < 480u; x++) {
			/* the title bar whole, and the rows under it */
			for (unsigned y = 0; y < 33u; y++)
				bad += at(frame, x, y) != at(fb, x, y);
			/* the status bar where held mode draws no words */
			for (unsigned y = 202; y < 222u; y++)
				if (x >= 100u && x < 320u)
					bad += at(frame, x, y) != at(fb, x, y);
		}
		CHECK(bad == 0u);
	}
	/* The one style there is: solid red from side to side, the squares
	 * in dark red over it, the first filled and the others outlined, and
	 * the status bar flat from its first row to the bottom, edge to
	 * edge. */
	{
		struct fr_screen s;
		const unsigned mx = S15_MARK_X(g);
		const unsigned my = (g->bar_h - S15_MARK_SIZE) / 2u;
		const unsigned end = mx + S15_MARK_COUNT * S15_MARK_SIZE +
				     (S15_MARK_COUNT - 1u) * S15_MARK_GAP;
		unsigned bad = 0;

		fr_screen_text(&s, "");
		CHECK(g->bar_h == 24u && S15_TEXT_W(g) == 383u && mx == 392u && my == 5u &&
		      end == 480u - g->edge);
		CHECK(S15_RED_DARK == 0xc1e6u);
		for (unsigned x = 0; x < 480u; x++)
			for (unsigned y = 0; y < g->bar_h; y++)
				if (x < mx || x >= end || y < my || y >= my + S15_MARK_SIZE ||
				    (x - mx) % (S15_MARK_SIZE + S15_MARK_GAP) >= S15_MARK_SIZE)
					bad += fr_screen_pixel(&s, x, y) != S15_RED;
		CHECK(bad == 0u);
		for (unsigned i = 0; i < S15_MARK_COUNT; i++) {
			const unsigned x = mx + i * (S15_MARK_SIZE + S15_MARK_GAP);
			const uint16_t in = i == 0u ? S15_RED_DARK : S15_RED;

			CHECK(fr_screen_pixel(&s, x, my) == S15_RED_DARK);
			CHECK(fr_screen_pixel(&s, x + 13u, my + 13u) == S15_RED_DARK);
			CHECK(fr_screen_pixel(&s, x + 1u, my + 7u) == S15_RED_DARK);
			CHECK(fr_screen_pixel(&s, x + 7u, my + 7u) == in);
			CHECK(fr_screen_pixel(&s, x + 2u, my + 2u) == in);
			CHECK(fr_screen_pixel(&s, x - 1u, my + 7u) == S15_RED);
		}
		bad = 0;
		for (unsigned x = 0; x < 480u; x++) {
			bad += fr_screen_pixel(&s, x, 201) != S15_BLACK;
			for (unsigned y = g->status_y; y < 222u; y++)
				bad += fr_screen_pixel(&s, x, y) != S15_STATUS_BG;
		}
		CHECK(bad == 0u);
	}
}

/* Body rows: the front's inks drawn as held mode's styles draw text, on
 * held mode's rows, 9 pixels below the title bar, with 56 glyphs a row. */
static void test_rows(void)
{
	struct s15_geometry front_rows = wb_display_geometry;
	static const struct {
		enum fr_ink ink;
		enum s15_style style;
	} map[] = {
		{ FR_INK_TEXT, S15_ST_TEXT }, { FR_INK_DIM, S15_ST_DIM },
		{ FR_INK_HEAD, S15_ST_HEAD },
	};
	static const char text[] =
		"Slot 1  waffle-boot v1.0.0  good  running, selected! 0123456789";
	static const char longer[] =
		"0123456789012345678901234567890123456789012345678901234567890123456789";
	struct fr_screen s;
	unsigned bad = 0;

	CHECK(front_rows.top == 33u && front_rows.pitch == 19u);
	fr_screen_text(&s, "Recovery");
	for (unsigned i = 0; i < FR_LINES; i++) {
		char cut[FR_COLS + 1];

		memcpy(cut, text + i, FR_COLS);
		cut[FR_COLS] = '\0';
		fr_screen_line(&s, i, map[i % 3u].ink, cut);
	}
	wb_display_init(&disp, fb);
	for (unsigned i = 0; i < FR_LINES; i++)
		s15_render_row(&front_rows, wb_display_draw(&disp), i,
			       map[i % 3u].style, text + i, strlen(text + i));
	rows_of(&s, frame);
	for (unsigned x = 0; x < 480u; x++)
		for (unsigned y = 24; y < 202u; y++)
			bad += at(frame, x, y) != at(fb, x, y);
	CHECK(bad == 0u);
	fr_screen_line(&s, 0, FR_INK_TEXT, longer);
	CHECK(strlen(s.line[0]) == FR_LINE_MAX);	/* cut to 64 */

	/* ok, warn and error: the glyph's strokes in green, amber, red. */
	fr_screen_text(&s, "");
	fr_screen_line(&s, 2, FR_INK_OK, "#");
	fr_screen_line(&s, 3, FR_INK_WARN, "#");
	fr_screen_line(&s, 4, FR_INK_ERROR, "#");
	{
		unsigned g = 0, a = 0, r = 0;

		for (unsigned x = 16; x < 24u; x++)
			for (unsigned y = 0; y < 16u; y++) {
				g += fr_screen_pixel(&s, x, 33u + 2u * 19u + y) == S15_GREEN;
				a += fr_screen_pixel(&s, x, 33u + 3u * 19u + y) == S15_AMBER;
				r += fr_screen_pixel(&s, x, 33u + 4u * 19u + y) == S15_RED;
			}
		CHECK(g != 0u && g == a && a == r);
	}
}

/* The window rows: screen column 479 - r top down, black past 222. */
static void test_window(void)
{
	struct fr_screen s;
	unsigned bad = 0;

	fr_screen_text(&s, "Recovery");
	fr_screen_line(&s, 0, FR_INK_TEXT, "Slot 1");
	fr_screen_status(&s, "USB", "front 1");
	rows_of(&s, frame);
	for (unsigned r = 0; r < FR_PANEL_ROWS; r++)
		for (unsigned c = 0; c < FR_PANEL_COLS; c++) {
			const uint8_t *p = frame + (size_t)r * FR_PANEL_ROW_BYTES + 2u * c;
			uint16_t want = c < FR_PANEL_VISIBLE ? fr_screen_pixel(&s, 479u - r, c) : 0u;

			bad += (uint16_t)(p[0] << 8 | p[1]) != want;
		}
	CHECK(bad == 0u);
	CHECK(fr_screen_pixel(&s, 480, 0) == 0u && fr_screen_pixel(&s, 0, 222) == 0u);
}

/* A whole painted frame against fr_screen_pixel(), pixel by pixel. */
static unsigned row_misses(const struct fr_screen *s)
{
	unsigned bad = 0;

	rows_of(s, frame);
	for (unsigned r = 0; r < FR_PANEL_ROWS; r++)
		for (unsigned c = 0; c < FR_PANEL_COLS; c++) {
			const uint8_t *p = frame + (size_t)r * FR_PANEL_ROW_BYTES + 2u * c;
			uint16_t want = c < FR_PANEL_VISIBLE ? fr_screen_pixel(s, 479u - r, c) : 0u;

			bad += (uint16_t)(p[0] << 8 | p[1]) != want;
		}
	return bad;
}

/* Every screen the front draws, row by row as the panel gets it, against
 * the pixel at a time: the logo, the text screen with full and empty texts,
 * texts with no end inside their fields, and the bar at its edges. */
static void test_rows_match_pixels(void)
{
	static const char wide[] =
		"Ig {Wq}~ 0123456789 abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOP";
	static const struct {
		uint8_t col, cols, pct;
	} bars[] = {
		{ 0, 40, 0 }, { 0, 40, 1 }, { 0, 40, 50 }, { 10, 20, 99 }, { 0, 56, 100 },
		{ 50, 40, 150 }, { 55, 1, 70 }, { 56, 4, 50 }, { 3, 0, 50 },
	};
	struct fr_screen s;

	fr_screen_logo(&s);
	CHECK(row_misses(&s) == 0u);

	fr_screen_text(&s, wide);
	for (unsigned i = 0; i < FR_LINES; i++)
		fr_screen_line(&s, i, (enum fr_ink)(i % 7u), wide + i);
	fr_screen_status(&s, wide, wide + 5);
	CHECK(row_misses(&s) == 0u);

	fr_screen_text(&s, "");
	CHECK(row_misses(&s) == 0u);
	fr_screen_text(&s, NULL);
	fr_screen_line(&s, 8, FR_INK_ERROR, "\x01\x7f\xff");
	fr_screen_status(&s, "USB", NULL);
	CHECK(row_misses(&s) == 0u);

	/* fields full to their last byte */
	fr_screen_text(&s, "");
	memset(s.title, 'W', sizeof s.title);
	memset(s.line, 'M', sizeof s.line);
	memset(s.status_left, 'L', sizeof s.status_left);
	memset(s.status_right, 'R', sizeof s.status_right);
	CHECK(row_misses(&s) == 0u);
	s.kind = FR_SCREEN_LOGO;
	CHECK(row_misses(&s) == 0u);

	for (unsigned b = 0; b < ARRAY_LEN(bars); b++) {
		fr_screen_text(&s, "Recovery");
		fr_screen_line(&s, 5, FR_INK_TEXT, "Writing slot 2");
		fr_screen_line(&s, 6, FR_INK_TEXT, wide);
		fr_screen_status(&s, "USB", "v1.0.10 F16");
		s.bar_line = b % 2u == 0u ? 6 : 8;
		s.bar_col = bars[b].col;
		s.bar_cols = bars[b].cols;
		s.bar_pm = (uint16_t)(bars[b].pct * 10u);
		CHECK(row_misses(&s) == 0u);
		s.bar_pm = (uint16_t)(bars[b].pct * 10u + 7u);
		CHECK(row_misses(&s) == 0u);
	}
}

/* The status texts: the left from x 8 in amber, the right ending at
 * x 472 in gray, on the bar. */
static void test_status(void)
{
	struct fr_screen s;
	unsigned left = 0, right = 0, past = 0;

	fr_screen_text(&s, "Recovery");
	fr_screen_status(&s, "USB", "front 1");
	for (unsigned y = 205; y < 221u; y++)
		for (unsigned x = 0; x < 480u; x++) {
			uint16_t p = fr_screen_pixel(&s, x, y);

			if (x >= 8u && x < 32u)
				left += p == S15_AMBER;
			else if (x >= 472u - 7u * 8u && x < 472u)
				right += p == S15_DIM;
			else
				past += p == S15_AMBER || p == S15_DIM;
		}
	CHECK(left != 0u && right != 0u && past == 0u);
	/* 'U' at x 8: its left stem from row 2 of the glyph */
	CHECK(fr_screen_pixel(&s, 8, 207) == S15_AMBER);
}

/* A progress bar on one row: outline and fill as the install screen's. */
static void test_bar(void)
{
	struct fr_screen s;
	unsigned x0 = 16u + 4u * 8u, w = 20u * 8u, y = 33u + 6u * 19u;
	unsigned fill = (w - 4u) * 50u / 100u;

	fr_screen_text(&s, "Recovery");
	fr_screen_line(&s, 6, FR_INK_TEXT, "Write ####");
	s.bar_line = 6;
	s.bar_col = 4;
	s.bar_cols = 20;
	s.bar_pm = 500;
	CHECK(fr_screen_pixel(&s, x0, y + 3u) == S15_AMBER);
	CHECK(fr_screen_pixel(&s, x0 + w - 1u, y + 8u) == S15_AMBER);
	CHECK(fr_screen_pixel(&s, x0 + w, y + 8u) == 0u);	/* the line's end */
	CHECK(fr_screen_pixel(&s, x0 + 1u, y + 8u) == 0u);
	CHECK(fr_screen_pixel(&s, x0 + 2u, y + 8u) == S15_AMBER);
	CHECK(fr_screen_pixel(&s, x0 + 2u + fill - 1u, y + 8u) == S15_AMBER);
	CHECK(fr_screen_pixel(&s, x0 + 2u + fill, y + 8u) == 0u);
	CHECK(fr_screen_pixel(&s, x0 + 5u, y + 2u) == 0u);
	CHECK(fr_screen_pixel(&s, x0 + 5u, y + 12u) == S15_AMBER);
	CHECK(fr_screen_pixel(&s, x0 + 5u, y + 13u) == 0u);
	s.bar_pm = 1000;
	CHECK(fr_screen_pixel(&s, x0 + w - 3u, y + 8u) == S15_AMBER);
	s.bar_pm = 0;
	CHECK(fr_screen_pixel(&s, x0 + 2u, y + 8u) == 0u);
	/* the text left of the bar stays */
	{
		unsigned n = 0;

		for (unsigned x = 16; x < x0; x++)
			for (unsigned dy = 0; dy < 16u; dy++)
				n += fr_screen_pixel(&s, x, y + dy) == S15_TEXT;
		CHECK(n != 0u);
	}
}

/* ---- Recovery's screen (src/front/fr_recscreen.c, S-15.151) ---- */

static struct fr_rec_status rst;
static struct fr_proto rp;
static struct fr_screen rs;
static const struct fr_ui *rui;		/* the screen with keys; NULL: the main one */
static const char *rows[FR_LINES + 1u];

static const uint16_t v_a[4] = { 1, 2, 3, 0 }, v_b[4] = { 1, 3, 0, 0 };
static const char *want_status = "v1.2.3 F21";	/* the running image's, v_a */
static bool paint_check = true;		/* rec_draw() checks the painter */
static bool clip_ok;			/* a slot line may lose parts at the clip */
static struct fr_screen rs_prev;	/* the screen inc holds */
static bool prev_ok;
static unsigned sampled;

/* Slot 0 runs and is selected, both slots good and waffle-boot images;
 * Power + M4; nothing received. */
static void rec_base(void)
{
	memset(&rst, 0, sizeof rst);
	memset(&rp, 0, sizeof rp);
	rst.why = FR_WHY_KEYS;
	rst.running.established = true;
	rst.running.slot = 0;
	rst.selector_ok = true;
	rst.sel.index = 0x00;
	rst.sel.verified = 0x01;
	for (unsigned k = 0; k < 2u; k++) {
		rst.slot[k].good = true;
		rst.slot[k].has_desc = true;
		rst.slot[k].has_front = true;
		rst.slot[k].kind = FR_KIND_WB;
		memcpy(rst.slot[k].desc.bl_version, k == 0u ? v_a : v_b, sizeof v_a);
	}
	rst.own_desc = true;
	rst.desc = rst.slot[0].desc;
	fr_rec_target(&rst);
	rp.st = &rst;
	rp.phase = FR_PH_IDLE;
	rp.w.target = FR_NO_SLOT;
	want_status = "v1.2.3 F21";
}

/* The x a body row's parts end at, as fr_draw.c lays them out with no
 * clip; *drawn: what the painter draws of it, at most FR_CLIP_X. */
static unsigned row_end(const char *t, unsigned *drawn)
{
	unsigned x = 16u;
	bool title = false, clipped = false;
	size_t i = 0;

	*drawn = x;
	while (t[i] != '\0') {
		const unsigned char b = (unsigned char)t[i++];
		unsigned w = 0, right;

		if (b == 0x01u) {
			i += t[i] != '\0';
			continue;
		}
		if (b == 0x04u || b == 0x06u || b == 0x07u) {
			title = b == 0x06u ? true : b == 0x07u ? false : title;
			continue;
		}
		if (b == 0x02u) {
			i += t[i] != '\0';
			w = 8u;
			right = x + 10u;
		} else if (b == 0x03u || b == 0x05u) {
			if (b == 0x05u)
				i += t[i] != '\0';
			w = 8u;
			while (t[i] != '\0' && t[i] != '\x04') {
				w += 8u;
				i++;
			}
			i += t[i] != '\0';
			right = x + w;
		} else {
			w = title ? 10u : 8u;
			right = x + w;
		}
		if (right > FR_CLIP_X)
			clipped = true;
		if (!clipped)
			*drawn = right;
		x += w;
	}
	return x;
}

/* A body row as it reads on the screen: the control bytes out, and
 * nothing past the clip (fr_draw.h). */
static void plain(char *t)
{
	char out[FR_LINE_MAX + 1];
	unsigned x = 16u;
	bool title = false;
	size_t i = 0, n = 0;

	while (t[i] != '\0') {
		const unsigned char b = (unsigned char)t[i++];

		if (b == 0x01u) {
			i += t[i] != '\0';
			continue;
		}
		if (b == 0x04u || b == 0x06u || b == 0x07u) {
			title = b == 0x06u ? true : b == 0x07u ? false : title;
			continue;
		}
		if (b == 0x02u) {
			if (t[i] == '\0' || x + 10u > FR_CLIP_X)
				break;
			out[n++] = t[i++];
			x += 8u;
		} else if (b == 0x03u || b == 0x05u) {
			size_t j;

			if (b == 0x05u)
				i += t[i] != '\0';
			j = i;
			while (t[j] != '\0' && t[j] != '\x04')
				j++;
			if (x + 8u * (unsigned)(j - i) + 8u > FR_CLIP_X)
				break;
			memcpy(out + n, t + i, j - i);
			n += j - i;
			x += 8u * (unsigned)(j - i) + 8u;
			i = j + (t[j] != '\0');
		} else {
			if (x + (title ? 10u : 8u) > FR_CLIP_X)
				break;
			out[n++] = (char)b;
			x += title ? 10u : 8u;
		}
	}
	out[n] = '\0';
	strcpy(t, out);
}

/* The screen; rows[0] is the title, then the body rows. Every row whole:
 * shorter than FR_COLS, so that nothing was cut, and naming no tool or
 * command of the host. */
static void rec_draw(bool usb)
{
	static const char *const never[] = { "wbrecover", "tool", "command", "python", ".py",
					     "--", "run ", "type ", "enter " };

	fr_rec_screen(&rs, &rst, &rp, usb, rui);
	CHECK(rs.kind == FR_SCREEN_TEXT);
	/* the painter's frame against the model, and a repaint over the last
	 * screen drawn against a whole paint */
	if (paint_check || ++sampled % 31u == 0u) {
		CHECK(row_misses(&rs) == 0u);
		if (prev_ok) {
			fr_paint_rows(inc, &rs, &rs_prev);
			CHECK(memcmp(inc, frame, sizeof inc) == 0);
		} else {
			memcpy(inc, frame, sizeof inc);
		}
		rs_prev = rs;
		prev_ok = true;
	}
	/* every row whole, nothing lost at the clip but a slot line's last
	 * parts where allowed */
	for (unsigned i = 0; i < FR_LINES; i++) {
		unsigned drawn, end = row_end(rs.line[i], &drawn);

		CHECK(strlen(rs.line[i]) < FR_LINE_MAX);
		CHECK(end <= FR_CLIP_X || (clip_ok && i < 2u));
		CHECK(drawn <= FR_CLIP_X);
		plain(rs.line[i]);
	}
	rows[0] = rs.title;
	for (unsigned i = 0; i < FR_LINES; i++)
		rows[i + 1u] = rs.line[i];
	for (unsigned i = 0; i <= FR_LINES; i++) {
		CHECK(strlen(rows[i]) <= FR_COLS);
		for (unsigned w = 0; w < ARRAY_LEN(never); w++)
			CHECK(!s15t_word_in(rows[i], never[w], NULL));
	}
	/* the status bar: the running image's version and the front's, and
	 * no key hint */
	CHECK(strcmp(rs.status_right, rst.own_desc ? want_status : "F21") == 0);
	CHECK(FR_FRONT_VERSION == 21u);
	CHECK(strcmp(rs.status_left, usb ? "USB" : "") == 0);
}

static bool has(const char *token, const char *const *values)
{
	return s15t_match_named(token, rows, FR_LINES + 1u, values);
}

/* A row with every one of the words (alternatives split by '|'). */
static bool row_with(const char *const *words)
{
	for (unsigned i = 0; i <= FR_LINES; i++) {
		bool all = true;

		for (unsigned w = 0; words[w] != NULL && all; w++) {
			char alt[64];
			const char *p = words[w];
			bool one = false;

			while (*p != '\0' && !one) {
				size_t n = strcspn(p, "|");

				memcpy(alt, p, n < sizeof alt ? n : sizeof alt - 1u);
				alt[n < sizeof alt ? n : sizeof alt - 1u] = '\0';
				one = s15t_word_in(rows[i], alt, NULL);
				p += n + (p[n] == '|' ? 1u : 0u);
			}
			all = one;
		}
		if (all)
			return true;
	}
	return false;
}

static bool anywhere(const char *word)
{
	for (unsigned i = 0; i <= FR_LINES; i++)
		if (s15t_word_in(rows[i], word, NULL))
			return true;
	return false;
}

/* A row holding this text as it stands. */
static bool shows(const char *text)
{
	for (unsigned i = 0; i <= FR_LINES; i++)
		if (strstr(rows[i], text) != NULL)
			return true;
	return false;
}

/* A failure, and that the unit starts as before. S-15.151's
 * recovery:failed asks for "as before" or "unchanged" in so many words;
 * the line now says "as it did before". */
static bool failed_shown(void)
{
	return row_with((const char *const[]){ "fail*|no write", NULL }) &&
	       shows("The unit starts as it did before");
}

static const char *const s1[] = { "slot", "1", NULL }, *const s2[] = { "slot", "2", NULL };
static const char *const n1[] = { "next", "1", NULL }, *const n2[] = { "next", "2", NULL };

static void test_rec_idle(void)
{
	static const char *const ver1[] = { "slot", "1", "version", "1.2.3", NULL };
	static const char *const ver2[] = { "slot", "2", "version", "1.3.0", NULL };

	rec_base();
	rec_draw(false);
	CHECK(strcmp(rs.title, "Recovery") == 0);
	CHECK(has("chord:recovery", NULL));
	/* with no computer: how to write an image, and the way to the slot
	 * menu; no target yet, as no write can come */
	CHECK(rs.line[2][0] == '\0' && rs.line[4][0] == '\0');
	CHECK(strcmp(rs.line[3], "Connect to a computer to write an image") == 0);
	CHECK(strcmp(rs.line[5], "1  Change the boot slot (optional)") == 0);
	CHECK(has("recovery:menu", NULL) && has("recovery:computer", NULL));
	CHECK(rs.status_left[0] == '\0');
	CHECK(has("recovery:slot:good", s1) && has("recovery:slot:good", s2));
	CHECK(!has("recovery:slot:bad", s1) && !has("recovery:slot:bad", s2));
	CHECK(has("recovery:version", ver1) && has("recovery:version", ver2));
	CHECK(has("recovery:running", s1) && !has("recovery:running", s2));
	CHECK(has("recovery:selected", s1) && !has("recovery:selected", s2));
	/* no key asked: none of the reasons */
	CHECK(!has("recovery:check", NULL));
	CHECK(!anywhere("three") && !anywhere("finish*"));
	/* connected: the slot a write replaces */
	rec_draw(true);
	CHECK(has("recovery:target", n2) && !has("recovery:target", n1));
	CHECK(strcmp(rs.line[3], "Connected: a write from the computer replaces slot 2") == 0);
	CHECK(strcmp(rs.line[5], "1  Change the boot slot (optional)") == 0);
	CHECK(strcmp(rs.status_left, "USB") == 0);

	/* slot 2 selected and running */
	rec_base();
	rst.running.slot = 1;
	rst.sel.index = 0x01;
	fr_rec_target(&rst);
	rec_draw(true);
	CHECK(has("recovery:running", s2) && has("recovery:selected", s2));
	CHECK(has("recovery:target", n1));

	/* a slot that failed its check: no kind, no version */
	rec_base();
	rst.slot[1].has_desc = rst.slot[1].has_front = false;
	rst.slot[1].good = false;
	rst.slot[1].kind = FR_KIND_NONE;
	fr_rec_target(&rst);
	rec_draw(false);
	CHECK(has("recovery:slot:bad", s2) && !has("recovery:slot:good", s2));
	CHECK(!has("recovery:version", ver2));
}

/* The longest version S-10c.055 writes, in both slots: the slot lines
 * keep their name, kind, version and state, and lose what does not fit at
 * the clip, never wrapping. The longest version of a real build, with
 * every chip, fits whole. */
static void test_rec_long_version(void)
{
	static const uint16_t vmax[4] = { 65535, 65535, 65535, 65535 };
	static const uint16_t vte[4] = { 1000, 0, 20, 3 };
	static const char *const ver1[] = { "slot", "1", "version", "65535.65535.65535+65535", NULL };
	static const char *const ver2[] = { "slot", "2", "version", "1000.0.20+3", NULL };

	rec_base();
	memcpy(rst.slot[0].desc.bl_version, vmax, sizeof vmax);
	memcpy(rst.slot[1].desc.bl_version, vmax, sizeof vmax);
	clip_ok = true;
	rec_draw(false);
	clip_ok = false;
	CHECK(has("recovery:version", ver1));
	CHECK(has("recovery:slot:good", s1) && has("recovery:slot:good", s2));
	/* running did not fit: the space before it is all that shows */
	CHECK(strcmp(rs.line[0], "Slot 1 WB v65535.65535.65535+65535 checked ") == 0);
	/* a TE build's longest, running and the boot slot, every part drawn */
	rec_base();
	rst.running.slot = 1;
	rst.sel.index = 0x01;
	rst.slot[1].kind = FR_KIND_TE_BUILD;
	memcpy(rst.slot[1].te_version, vte, sizeof vte);
	fr_rec_target(&rst);
	rec_draw(false);
	CHECK(strcmp(rs.line[1], "Slot 2 TE v1000.0.20+3 checked running boot slot") == 0);
	CHECK(has("recovery:version", ver2) && has("recovery:foreign", s2));
	CHECK(has("recovery:running", s2) && has("recovery:selected", s2));
}

/* The rows below the slot lines that name a slot as a write's target:
 * "slot N" with a word of writing, receiving or replacing. Their count, and
 * the last one's slot (1 or 2) in *slot. */
static unsigned target_rows(unsigned *slot)
{
	static const char *const verbs[] = { "writ*", "receiv*", "replac*" };
	unsigned n = 0;

	for (unsigned i = 3; i <= FR_LINES; i++)
		for (unsigned k = 1; k <= 2u; k++) {
			const char *const vals[] = { "n", k == 1u ? "1" : "2", NULL };
			bool verb = false;

			for (unsigned v = 0; v < ARRAY_LEN(verbs); v++)
				verb |= s15t_word_in(rows[i], verbs[v], NULL);
			if (verb && s15t_word_in(rows[i], "slot $n", vals)) {
				n++;
				*slot = k;
			}
		}
	return n;
}

/* S-15.151: while an image arrives or is written, the screen names one
 * target: the transfer's own, and no "A write replaces" line. */
static void test_rec_one_target(void)
{
	unsigned slot = 0;

	/* in place: the running slot, slot 1 */
	rec_base();
	rst.slot[1].good = false;
	rst.home.other_failed = true;
	fr_rec_target(&rst);
	rp.phase = FR_PH_WRITING;
	rp.flags = FR_BEGIN_F_RUNNING;
	rp.w.way = FR_WAY_IN_PLACE;
	rp.w.target = 0;
	rp.w.state = FR_WS_SLOT;
	rp.w.n = 0x4000u;
	rec_draw(false);
	CHECK(target_rows(&slot) == 1u && slot == 1u);
	CHECK(!anywhere("replaces"));
	CHECK(has("recovery:in_place", NULL));
	/* receiving for it */
	rp.phase = FR_PH_RECEIVING;
	rp.plan_way = FR_WAY_IN_PLACE;
	rp.plan_target = 0;
	rp.len = 0x4000u;
	rp.got = 0x1000u;
	rec_draw(false);
	CHECK(target_rows(&slot) == 1u && slot == 1u);
	CHECK(!anywhere("replaces"));
	/* a plain write of slot 2, and receiving for it */
	rec_base();
	rp.phase = FR_PH_WRITING;
	rp.w.way = FR_WAY_PLAIN;
	rp.w.target = 1;
	rp.w.state = FR_WS_SLOT;
	rp.w.n = 0x4000u;
	rec_draw(false);
	CHECK(target_rows(&slot) == 1u && slot == 2u);
	CHECK(!anywhere("replaces"));
	rp.phase = FR_PH_RECEIVING;
	rp.plan_way = FR_WAY_PLAIN;
	rp.plan_target = 1;
	rp.len = 0x4000u;
	rec_draw(true);
	CHECK(target_rows(&slot) == 1u && slot == 2u);
	CHECK(!anywhere("replaces"));
	/* idle: the target line again */
	rp.phase = FR_PH_IDLE;
	rec_draw(true);
	CHECK(has("recovery:target", n2));
	CHECK(target_rows(&slot) == 1u && slot == 2u);
}

/*
 * S-15.151 in the rescue state (a write of the running slot left it bad):
 * row 8 says what a start reaches (S-02.084): the other slot at the next
 * restart (named), at the second (fallback), or that the slot must be
 * written again before a power-off (none, also with the selector
 * unreadable). No other row tells the user to restart.
 */
static void test_rec_rescue_rows(void)
{
	static const uint8_t phases[] = { FR_PH_IDLE, FR_PH_FAILED, FR_PH_RECEIVING, FR_PH_DONE };

	for (unsigned reach = 0; reach < 4u; reach++)
		for (unsigned ph = 0; ph < ARRAY_LEN(phases); ph++) {
			rec_base();
			rst.home.spoiled = true;
			rst.slot[0].good = false;
			rst.slot[0].has_front = rst.slot[0].has_desc = false;
			switch (reach) {
			case 0:		/* named: {01 01}, slot 2 good */
				rst.sel.index = 0x01;
				break;
			case 1:		/* fallback: {00 3F}, slot 2 good, slot 1 sure to fail */
				rst.sel.verified = 0x3F;
				break;
			case 2:		/* none: {00 01}, slot 2 bad */
				rst.slot[1].good = false;
				break;
			default:	/* none: the selector unreadable */
				rst.selector_ok = false;
				break;
			}
			fr_rec_target(&rst);
			CHECK(fr_rec_reach(&rst) == (reach == 0u ? FR_REACH_NAMED :
						     reach == 1u ? FR_REACH_FALLBACK : FR_REACH_NONE));
			rp.phase = phases[ph];
			rp.result = FR_WR_FLASH;
			rp.len = 0x4000u;
			rp.w.way = FR_WAY_PLAIN;
			rp.w.target = 1;
			rec_draw(false);
			if (reach < 2u) {
				CHECK(has("recovery:damaged:other", s1));
				CHECK(!has("recovery:damaged:rewrite", s1));
				CHECK(s15t_word_in(rs.line[8], reach == 0u ? "a restart" : "2nd restart",
						   NULL));
			} else {
				CHECK(has("recovery:damaged:rewrite", s1));
				CHECK(!has("recovery:damaged:other", s1));
			}
			/* "restart" nowhere but row 8's line of what a restart boots */
			for (unsigned i = 0; i <= FR_LINES; i++)
				if (i != 9u || reach >= 2u)
					CHECK(!s15t_word_in(rows[i], "restart*", NULL));
			CHECK(!anywhere("as before"));
		}
}

/*
 * S-15.151 while the power-off is held (S-02.084): row 7 says the unit must
 * stay powered until a write has finished (recovery:held), and only then;
 * where the running slot was written and read back good but the selector
 * is not settled on it, row 8 says it must be written again
 * (recovery:unsettled). Checked by words until the vectors carry the
 * tokens. Not while a write runs.
 */
static const char *const held_words[] = { "keep|stay", "power*", "write", "finish*", NULL };
static const char *const unsettled_words[] = { "slot", "written", "not", "boot slot",
					       "again", NULL };

static void test_rec_held_rows(void)
{
	static const uint8_t phases[] = { FR_PH_IDLE, FR_PH_FAILED, FR_PH_RECEIVING, FR_PH_DONE,
					  FR_PH_WRITING };

	for (unsigned c = 0; c < 5u; c++)
		for (unsigned ph = 0; ph < ARRAY_LEN(phases); ph++) {
			bool held, unsettled;

			rec_base();
			rst.home.spoiled = true;
			rst.slot[1].good = false;	/* slot 2 bad */
			rst.slot[1].has_front = rst.slot[1].has_desc = false;
			switch (c) {
			case 0:		/* slot 1 damaged, {00 01}: held */
				rst.slot[0].good = false;
				break;
			case 1:		/* slot 1 good, unsettled, FF FF: held */
				rst.home.unsettled = true;
				rst.sel.index = 0xFF;
				rst.sel.verified = 0xFF;
				break;
			case 2:		/* slot 1 good, unsettled, {00 FF}: held */
				rst.home.unsettled = true;
				rst.sel.verified = 0xFF;
				break;
			case 3:		/* slot 1 good, unsettled, {00 01}: reached */
				rst.home.unsettled = true;
				break;
			default:	/* slot 1 good, settled: no state */
				rst.sel.verified = 0xFF;
				break;
			}
			fr_rec_target(&rst);
			held = c < 3u;
			unsettled = c >= 1u && c <= 3u;
			CHECK(fr_rec_held(&rst) == held);
			CHECK(fr_rec_rescue(&rst) == (c < 4u));
			rp.phase = phases[ph];
			rp.result = FR_WR_SELECTOR;
			rp.len = 0x4000u;
			rp.w.way = FR_WAY_IN_PLACE;
			rp.w.target = 0;
			rec_draw(false);
			if (phases[ph] == FR_PH_WRITING) {
				CHECK(!s15t_word_in(rs.line[7], "finish*", NULL));
				continue;
			}
			/* row 7, while held */
			CHECK((s15t_word_in(rs.line[7], "keep", NULL) &&
			       s15t_word_in(rs.line[7], "power", NULL) &&
			       s15t_word_in(rs.line[7], "finished", NULL)) == held);
			CHECK(row_with(held_words) == held);
			/* row 8, written but not the boot slot, while held */
			if (held && unsettled) {
				CHECK(s15t_word_in(rs.line[8], "written", NULL) &&
				      s15t_word_in(rs.line[8], "boot slot", NULL) &&
				      s15t_word_in(rs.line[8], "again", NULL));
				CHECK(s15t_word_in(rs.line[8], "1", NULL));
				CHECK(has("recovery:unsettled", s1));
				CHECK(!has("recovery:damaged:rewrite", s1));
			} else {
				CHECK(!row_with(unsettled_words));
			}
			if (c == 0u)
				CHECK(has("recovery:damaged:rewrite", s1));
		}
}

/* Results 15 and 16 (S-02.093) on the screen */
static void test_rec_new_results(void)
{
	/* 15 as a start's refusal for the running slot's request: the default
	 * target still stands; and as a refused BEGIN's result */
	rec_base();
	rst.slot[1].good = false;
	fr_rec_target(&rst);
	CHECK(rst.run_refusal == FR_WR_TRY_OTHER && rst.target == 1u);
	rp.phase = FR_PH_FAILED;
	rp.result = FR_WR_TRY_OTHER;
	rec_draw(false);
	CHECK(row_with((const char *const[]){ "no write|refus*|cannot write|not written", "other",
					       NULL }));
	CHECK(!has("recovery:failed:selector", NULL));
	/* 17 and 18 as a refused write's result: a reason, not "as before" */
	rec_base();
	rp.phase = FR_PH_FAILED;
	rp.result = FR_WR_READ;
	rec_draw(false);
	CHECK(row_with((const char *const[]){ "no write|fail*", "read", NULL }));
	rp.result = FR_WR_ONLY_GOOD;
	rec_draw(false);
	CHECK(row_with((const char *const[]){ "no write", "image", "checked slot", NULL }));
	/* 16: a failure, the unit as before */
	rec_base();
	rp.phase = FR_PH_FAILED;
	rp.result = FR_WR_CHANGED;
	rec_draw(false);
	CHECK(failed_shown());
	CHECK(!has("recovery:done", n2));
}

/* After a selector write that failed (S-02.093 status 11 and 14), where
 * "as before" may not hold: the failure and what holds. Results 12 and 13
 * are failures of the image; a start refused for 12 says why no write. */
/*
 * S-15.151: after a selector write that failed, the screen says of the
 * slots only what the survey found. "Both slots read good" shows only
 * when they do.
 */
static void test_rec_selector_survey(void)
{
	/* The running slot 2 written and read back good, slot 1 bad with a
	 * record sure to fail, the settling left {00 02}: the first stage
	 * falls back to slot 2. Not held; rows 6 and 7 say so. */
	rec_base();
	rst.running.slot = 1;
	rst.home.spoiled = true;
	rst.home.unsettled = true;
	rst.slot[0].good = false;
	rst.slot[0].has_front = rst.slot[0].has_desc = false;
	rst.slot[0].rec.tag = FR_TAG;
	rst.slot[0].rec.length = 0x1000u;
	rst.sel.index = 0x00;
	rst.sel.verified = 0x02;
	fr_rec_target(&rst);
	CHECK(fr_rec_rescue(&rst) && !fr_rec_held(&rst));
	CHECK(fr_rec_reach(&rst) == FR_REACH_FALLBACK);
	rp.phase = FR_PH_FAILED;
	rp.result = FR_WR_SELECTOR;
	rec_draw(false);
	CHECK(!anywhere("both slots"));
	CHECK(rs.line[7][0] == '\0');
	CHECK(row_with((const char *const[]){ "slot 2", "written", "2nd restart", NULL }));
	CHECK(!anywhere("as before"));

	/* The same with the selector naming slot 2 unverified: a restart
	 * boots it. */
	rst.sel.index = 0x01;
	rst.sel.verified = 0xFF;
	fr_rec_target(&rst);
	CHECK(fr_rec_reach(&rst) == FR_REACH_NAMED && !fr_rec_held(&rst));
	rec_draw(false);
	CHECK(!anywhere("both slots"));
	CHECK(row_with((const char *const[]){ "slot 2", "written", "restart boots it", NULL }));

	/* Outside that state: the running slot 1 bad with its front intact,
	 * slot 2 written and read back good, its selector write failed and
	 * the selector reads {01 FF}: a restart boots slot 2. */
	rec_base();
	rst.slot[0].good = false;
	rst.sel.index = 0x01;
	rst.sel.verified = 0xFF;
	fr_rec_target(&rst);
	CHECK(!fr_rec_rescue(&rst));
	rp.phase = FR_PH_FAILED;
	rp.result = FR_WR_SELECTOR;
	rec_draw(false);
	CHECK(!anywhere("both slots"));
	CHECK(has("recovery:failed:selector", NULL));
	CHECK(row_with((const char *const[]){ "restart boots", "slot 2", "checked", NULL }));

	/* ... and with the selector still naming the bad slot 1 as trusted:
	 * no good slot is named. */
	rst.sel.index = 0x00;
	rst.sel.verified = 0x01;
	fr_rec_target(&rst);
	rec_draw(false);
	CHECK(!anywhere("both slots"));
	CHECK(has("recovery:failed:selector", NULL));
	CHECK(row_with((const char *const[]){ "no checked slot", "write again", NULL }));

	/* Both good: the line it had. */
	rec_base();
	rp.phase = FR_PH_FAILED;
	rp.result = FR_WR_SELECTOR;
	rec_draw(false);
	CHECK(row_with((const char *const[]){ "both slots", "checked", NULL }));
}

static void test_rec_failed_selector(void)
{
	static const uint8_t sel_results[] = { FR_WR_SELECTOR, FR_WR_SELECT_FIRST };
	static const uint8_t image_results[] = { FR_WR_FRONT };

	for (unsigned i = 0; i < ARRAY_LEN(sel_results); i++) {
		rec_base();
		rp.phase = FR_PH_FAILED;
		rp.result = sel_results[i];
		rec_draw(false);
		CHECK(has("recovery:failed:selector", NULL));
		CHECK(!anywhere("as before"));
	}
	for (unsigned i = 0; i < ARRAY_LEN(image_results); i++) {
		rec_base();
		rp.phase = FR_PH_FAILED;
		rp.result = image_results[i];
		rec_draw(true);
		CHECK(failed_shown());
		CHECK(!has("recovery:failed:selector", NULL));
		CHECK(rs.line[6][0] != '\0' && s15t_word_in(rs.line[6], "failed", NULL));
		CHECK(has("recovery:target", n2));
	}
	/* a start refused because the running slot, good, holds no front */
	rec_base();
	rst.slot[0].has_front = false;
	fr_rec_target(&rst);
	CHECK(rst.target == FR_NO_SLOT);
	rec_draw(false);
	CHECK(row_with((const char *const[]){ "no write", "running slot", "front", NULL }));
	CHECK(!has("recovery:target", n1) && !has("recovery:target", n2));
	CHECK(!anywhere("image's"));
}

static void test_rec_refusals(void)
{
	/* the running slot not known */
	rec_base();
	rst.running.established = false;
	fr_rec_target(&rst);
	rec_draw(true);
	CHECK(row_with((const char *const[]){ "no write", "cannot tell", "running", NULL }));
	CHECK(!has("recovery:target", n1) && !has("recovery:target", n2));
	/* not the selected one */
	rec_base();
	rst.sel.index = 0x01;
	fr_rec_target(&rst);
	rec_draw(true);
	CHECK(row_with((const char *const[]){ "no write", "restart", "once", NULL }));
	CHECK(has("recovery:running", s1) && has("recovery:selected", s2));
	CHECK(!anywhere("cannot tell"));
	CHECK(!has("recovery:target", n1) && !has("recovery:target", n2));
	/* verified 00: a restart settles it */
	rec_base();
	rst.sel.verified = 0x00;
	fr_rec_target(&rst);
	rec_draw(false);
	CHECK(has("recovery:refused:unverified", NULL));
	/* the selected slot bad, its front not intact (status 5) */
	rec_base();
	rst.slot[0].good = false;
	rst.slot[0].has_front = false;
	fr_rec_target(&rst);
	CHECK(rst.refusal == FR_WR_SELECTED_BAD);
	rec_draw(true);
	CHECK(row_with((const char *const[]){ "no write", "boot slot", "fails", "change", NULL }));
	CHECK(has("recovery:refused:selected_bad", NULL));
	CHECK(strcmp(rs.line[5], "1  Change the boot slot first") == 0);
	CHECK(has("recovery:slot:bad", s1));
	CHECK(!has("recovery:target", n2));
	/* bad with its front intact: a write goes (S-02.084) */
	rec_base();
	rst.slot[0].good = false;
	fr_rec_target(&rst);
	rec_draw(true);
	CHECK(has("recovery:target", n2));
	CHECK(has("recovery:slot:bad", s1));
}

static void test_rec_transfer(void)
{
	static const char *const got[] = { "72", NULL };

	/* receiving: how much has come, for the slot it goes to */
	rec_base();
	rp.phase = FR_PH_RECEIVING;
	rp.len = 0x23456u;
	rp.got = 0x12000u;		/* 72 KiB of 141 */
	rp.plan_way = FR_WAY_PLAIN;
	rp.plan_target = 1;
	rec_draw(true);
	CHECK(row_with((const char *const[]){ "72", "slot 2", NULL }));
	CHECK(strcmp(rs.line[6], "Receiving 72 of 141 KiB for slot 2") == 0);
	/* its bar under it, filled got of len */
	CHECK(rs.bar_line == 7 && rs.bar_pm == (unsigned)(0x12000u * 1000u / 0x23456u));
	CHECK(has("recovery:slot:good", s2));
	(void)got;
	rp.flags = FR_BEGIN_F_RUNNING;
	rp.plan_way = FR_WAY_SELECT_FIRST;
	rp.plan_target = 0;
	rec_draw(true);
	CHECK(row_with((const char *const[]){ "72", "slot 1", NULL }));

	/* writing, plain: the slot, the progress, the power */
	rec_base();
	rp.phase = FR_PH_WRITING;
	rp.w.way = FR_WAY_PLAIN;
	rp.w.target = 1;
	rp.w.state = FR_WS_SLOT;
	rp.w.n = 0x4000u;
	rp.w.pos = 0x2000u;
	rec_draw(true);
	CHECK(has("recovery:writing", s2));
	CHECK(row_with((const char *const[]){ "writing", "slot 2", NULL }));
	CHECK(rs.bar_line >= 0 && rs.bar_cols != 0u);
	CHECK(rs.bar_pm == fr_writer_progress(&rp.w) && rs.bar_pm > 0u);
	CHECK(shows("Slot 2 being written") && !has("recovery:slot:good", s2));
	/* select first: the same */
	rp.w.way = FR_WAY_SELECT_FIRST;
	rp.w.target = 0;
	rec_draw(true);
	CHECK(has("recovery:writing", s1));
	CHECK(row_with((const char *const[]){ "writing", "slot 1", NULL }));

	/* in place: the slot, the power, that a cut leaves nothing that starts,
	 * and no progress */
	rec_base();
	rst.slot[1].good = false;
	fr_rec_target(&rst);
	rp.phase = FR_PH_WRITING;
	rp.w.way = FR_WAY_IN_PLACE;
	rp.w.target = 0;
	rp.w.state = FR_WS_SLOT;
	rp.w.n = 0x4000u;
	rp.w.pos = 0;
	rec_draw(true);
	CHECK(has("recovery:writing", s1));
	CHECK(row_with((const char *const[]){ "writing", "slot 1", NULL }));
	CHECK(row_with((const char *const[]){ "cut|power cut|power-off", "start*", NULL }));
	CHECK(rs.bar_line < 0);
	CHECK(!anywhere("%") && !anywhere("0%"));

	/* done: the slot written and that a restart boots it */
	rec_base();
	rp.phase = FR_PH_DONE;
	rp.w.way = FR_WAY_PLAIN;
	rp.w.target = 1;
	rp.w.state = FR_WS_DONE;
	rp.w.result = FR_WR_OK;
	rec_draw(true);
	CHECK(has("recovery:done", n2));
	rp.w.way = FR_WAY_SELECT_FIRST;
	rp.w.target = 0;
	rec_draw(true);
	CHECK(has("recovery:done", n1));
	rp.w.way = FR_WAY_IN_PLACE;
	rec_draw(true);
	CHECK(has("recovery:done", n1));
	CHECK(!has("recovery:failed", NULL));

	/* failed writes: the failure and that the unit boots as before */
	{
		static const uint8_t results[] = { FR_WR_FLASH, FR_WR_READBACK, FR_WR_HASH };

		for (unsigned i = 0; i < ARRAY_LEN(results); i++) {
			rec_base();
			rp.phase = FR_PH_FAILED;
			rp.result = results[i];
			rec_draw(true);
			CHECK(failed_shown());
			CHECK(!has("recovery:done", n2));
		}
	}
}

/* A write of the running slot that did not end well, the slot bad: for as
 * long as recovery runs, in place of "as before", that it must be written
 * again before a power-off, or that a restart boots the other slot. */
static void test_rec_damaged(void)
{
	static const uint8_t phases[] = { FR_PH_FAILED, FR_PH_IDLE, FR_PH_RECEIVING };

	for (unsigned moved = 0; moved < 2u; moved++)
		for (unsigned i = 0; i < ARRAY_LEN(phases); i++) {
			rec_base();
			rst.home.spoiled = true;
			rst.home.moved = moved != 0u;
			rst.slot[0].good = false;
			rst.slot[0].has_front = false;
			rst.slot[0].has_desc = false;
			if (moved)
				rst.sel.index = 0x01;
			else
				rst.slot[1].good = false;
			fr_rec_target(&rst);
			CHECK(rst.refusal == FR_WR_OK);
			rp.phase = phases[i];
			rp.result = FR_WR_FLASH;
			rp.len = 0x4000u;
			rec_draw(false);
			CHECK(!anywhere("as before") && !anywhere("unchanged"));
			if (moved)
				CHECK(row_with((const char *const[]){ "restart*", "other|slot 2", NULL }));
			else
				CHECK(row_with((const char *const[]){ "again", "power-off|power", NULL }));
			if (phases[i] == FR_PH_FAILED)
				CHECK(anywhere("fail*"));
		}
}

static void test_rec_why(void)
{
	rec_base();
	rst.why = FR_WHY_CHECK;
	rec_draw(false);
	CHECK(has("recovery:check", NULL));
	CHECK(has("chord:recovery", NULL));
	rec_base();
	rst.why = FR_WHY_COUNT;
	rec_draw(false);
	CHECK(!has("recovery:check", NULL));
	CHECK(row_with((const char *const[]){ "three|3", "start*", "finish*", NULL }));
	CHECK(has("chord:recovery", NULL));
}

/*
 * S-15.151 for a trial and a select (S-02.093): the trial asked for while
 * the image arrives; the slot written for one trial start, and how to get
 * back, once it is written; the trial start's line in the tried slot's own
 * recovery, with the keys that keep it; the select's line; one target line
 * throughout.
 */
static void test_rec_trial(void)
{
	static const char *const off_on[] = { "if", "off and on", "slot 1", "boots", NULL };
	unsigned slot = 0;

	/* announced: receiving for slot 2, slot 1 runs */
	rec_base();
	rp.phase = FR_PH_RECEIVING;
	rp.flags = FR_BEGIN_F_TRIAL;
	rp.plan_way = FR_WAY_TRIAL;
	rp.plan_target = 1;
	rp.len = 0x4000u;
	rp.got = 0x1000u;
	rec_draw(true);
	CHECK(row_with((const char *const[]){ "trial", "asked", "started once", NULL }));
	CHECK(target_rows(&slot) == 1u && slot == 2u);
	CHECK(!anywhere("replaces"));
	CHECK(!anywhere("change"));
	/* written: the slot, the progress, the power */
	rp.phase = FR_PH_WRITING;
	rp.w.way = FR_WAY_TRIAL;
	rp.w.target = 1;
	rp.w.state = FR_WS_SLOT;
	rp.w.n = 0x4000u;
	rp.w.pos = 0x1000u;
	rec_draw(true);
	CHECK(has("recovery:writing", s2));
	CHECK(target_rows(&slot) == 1u && slot == 2u);
	CHECK(rs.bar_line >= 0);
	/* done: the selector {FF FF}, slot 2 tried */
	rst.sel.index = 0xFF;
	rst.sel.verified = 0xFF;
	rst.slot[1].good = false;
	rst.slot[1].tried = true;
	fr_rec_target(&rst);
	rp.phase = FR_PH_DONE;
	rp.w.state = FR_WS_DONE;
	rp.w.result = FR_WR_OK;
	rec_draw(true);
	CHECK(has("recovery:trial:written", n2) && !has("recovery:trial:written", n1));
	CHECK(shows("Restart to start it once (host or power switch)"));
	CHECK(row_with(off_on));
	CHECK(shows("Slot 2 WB v1.3.0 tried boot slot"));
	/* the other order: slot 2 runs, slot 1 is tried */
	rec_base();
	rst.running.slot = 1;
	rst.sel.index = 0x01;
	fr_rec_target(&rst);
	rp.phase = FR_PH_RECEIVING;
	rp.flags = FR_BEGIN_F_TRIAL;
	rp.plan_way = FR_WAY_TRIAL;
	rp.plan_target = 0;
	rp.len = 0x4000u;
	rec_draw(false);
	CHECK(row_with((const char *const[]){ "trial", "asked", NULL }));
	CHECK(target_rows(&slot) == 1u && slot == 1u);

	/* a trial start: slot 2 tried and running, slot 1 good and selected
	 * with verified 01; the line in place of the refusal, with the keys */
	rec_base();
	rst.running.slot = 1;
	rst.slot[1].good = false;
	rst.slot[1].tried = true;
	fr_rec_target(&rst);
	CHECK(rst.refusal == FR_WR_NOT_SELECTED && rst.select_target == 1u);
	rec_draw(true);
	CHECK(strcmp(rs.line[6], "Trial start of slot 2: to keep it, 1 then 3") == 0);
	CHECK(!anywhere("no write"));
	CHECK(shows("Slot 2 WB v1.3.0 tried running"));
	CHECK(has("recovery:running", s2) && has("recovery:selected", s1));
	CHECK(!has("recovery:target", n1) && !has("recovery:target", n2));

	/* from the old front, slot 2 tried and started: the target line, no
	 * trial start; not started: 21 */
	rec_base();
	rst.slot[1].good = false;
	rst.slot[1].tried = true;
	fr_rec_target(&rst);
	CHECK(rst.select_target == FR_NO_SLOT && rst.select_refusal == FR_WR_NOT_STARTED);
	rec_draw(true);
	CHECK(shows("Slot 2 WB v1.3.0 tried") && !anywhere("started"));
	rst.slot[1].started = true;
	fr_rec_target(&rst);
	CHECK(rst.select_target == 1u && rst.target == 1u);
	rec_draw(true);
	CHECK(shows("Slot 2 WB v1.3.0 tried, started"));
	CHECK(!anywhere("trial start"));
	CHECK(has("recovery:target", n2));
	/* the select announced, then written, then done */
	rp.phase = FR_PH_RECEIVING;
	rp.flags = FR_BEGIN_F_SELECT;
	rp.plan_way = FR_WAY_SELECT;
	rp.plan_target = 1;
	rp.len = 0x4000u;
	rp.got = 0x2000u;
	rec_draw(false);
	CHECK(shows("Slot 2, which was tried, is to be kept") && !shows("Slot 1, which"));
	CHECK(!has("recovery:selected", s2));
	CHECK(target_rows(&slot) == 1u && slot == 2u);
	rp.phase = FR_PH_WRITING;
	rp.w.way = FR_WAY_SELECT;
	rp.w.target = 1;
	rp.w.state = FR_WS_RECORD_ERASE;
	rp.w.n = 0x4000u;
	rec_draw(false);
	CHECK(has("recovery:writing", s2));
	CHECK(target_rows(&slot) == 1u && slot == 2u);
	rp.phase = FR_PH_DONE;
	rp.w.state = FR_WS_DONE;
	rp.w.result = FR_WR_OK;
	rec_draw(false);
	CHECK(shows("Done: slot 2 is kept as the boot slot"));
	CHECK(shows("Restart to start it (host or power switch)"));
	CHECK(!shows("which was tried"));

	/* the refusals of S-02.093: 19 for a trial and a select, 20, 21, and
	 * 12 naming the trial as the way for a new front */
	rec_base();
	rp.phase = FR_PH_FAILED;
	rp.result = FR_WR_TRIAL_STATE;
	rp.flags = FR_BEGIN_F_TRIAL;
	rec_draw(false);
	CHECK(failed_shown());
	CHECK(row_with((const char *const[]){ "fail*", "trial", "restart", NULL }));
	rp.flags = FR_BEGIN_F_SELECT;
	rec_draw(false);
	CHECK(failed_shown());
	CHECK(row_with((const char *const[]){ "fail*", "keep", "write", NULL }));
	rp.result = FR_WR_NOT_TRIED;
	rec_draw(false);
	CHECK(failed_shown());
	CHECK(row_with((const char *const[]){ "fail*", "tried", NULL }));
	rp.result = FR_WR_NOT_STARTED;
	rec_draw(false);
	CHECK(failed_shown());
	CHECK(row_with((const char *const[]){ "fail*", "tried slot", "not started", NULL }));
	rp.result = FR_WR_FRONT;
	rp.flags = 0;
	rec_draw(false);
	CHECK(row_with((const char *const[]){ "new front", "trial", NULL }));
}

/* ---- the screens with keys, the countdown, and every line's fit ---- */

static const uint16_t v_real[4] = { 1, 0, 10, 0 };	/* "v1.0.10", a real build's */

/* Slot b, the one not running, as a try or a switch tiers it. */
static void tier_b(unsigned tier)
{
	struct fr_slot_info *b = &rst.slot[1u - rst.running.slot];

	switch (tier) {
	case FR_TIER_FRONT:
		memset(b->desc.pin, 0x77, sizeof b->desc.pin);
		break;
	case FR_TIER_FOREIGN:
		b->has_front = false;
		break;
	case FR_TIER_BAD:
		b->good = false;
		b->has_front = false;
		b->has_desc = false;
		b->bootable = true;	/* switched to all the same, forced */
		break;
	default:
		break;
	}
}

/* rec_base(), with both slots' streams bootable (S-02.091). */
static void ui_base(void)
{
	rec_base();
	rst.slot[0].bootable = rst.slot[1].bootable = true;
	fr_rec_target(&rst);
}

/* The main screen's slot lines, each state the words for it; the status
 * bar's right text with and without the running image's descriptor. */
static void test_rec_main(void)
{
	static const uint16_t vte[4] = { 1000, 0, 20, 0 };

	ui_base();
	memcpy(rst.desc.bl_version, v_real, sizeof v_real);
	memcpy(rst.slot[0].desc.bl_version, v_real, sizeof v_real);
	want_status = "v1.0.10 F21";
	rec_draw(true);
	CHECK(strcmp(rs.line[0], "Slot 1 WB v1.0.10 checked running boot slot") == 0);
	CHECK(strcmp(rs.line[1], "Slot 2 WB v1.3.0 checked") == 0);
	CHECK(rs.ink[0] == FR_INK_TEXT && rs.ink[1] == FR_INK_TEXT);
	CHECK(strcmp(rs.status_right, "v1.0.10 F21") == 0 && strcmp(rs.status_left, "USB") == 0);
	/* no key hint in the status bar, with or without USB */
	CHECK(!s15t_word_in(rs.status_left, "1", NULL) && !s15t_word_in(rs.status_right, "key", NULL));
	rec_draw(false);
	CHECK(rs.status_left[0] == '\0');
	CHECK(strcmp(rs.line[5], "1  Change the boot slot (optional)") == 0);
	/* tried, tried and started, failed its check, and each kind */
	rst.slot[1].good = false;
	rst.slot[1].tried = true;
	rec_draw(false);
	CHECK(strcmp(rs.line[1], "Slot 2 WB v1.3.0 tried") == 0);
	rst.slot[1].started = true;
	rec_draw(false);
	CHECK(strcmp(rs.line[1], "Slot 2 WB v1.3.0 tried, started") == 0);
	rst.slot[1].tried = rst.slot[1].started = false;
	rst.slot[1].kind = FR_KIND_NONE;
	rec_draw(false);
	CHECK(strcmp(rs.line[1], "Slot 2 check failed") == 0);
	rst.slot[1].good = true;
	rst.slot[1].has_desc = rst.slot[1].has_front = false;
	rst.slot[1].kind = FR_KIND_TE_BUILD;
	memcpy(rst.slot[1].te_version, vte, sizeof vte);
	rec_draw(false);
	CHECK(strcmp(rs.line[1], "Slot 2 TE v1000.0.20 checked") == 0);
	rst.slot[1].kind = FR_KIND_TE;
	rec_draw(false);
	CHECK(strcmp(rs.line[1], "Slot 2 TE checked") == 0);
	rst.slot[1].kind = FR_KIND_UNKNOWN;
	rec_draw(false);
	CHECK(strcmp(rs.line[1], "Slot 2 ?? checked") == 0);
	rst.slot[1].good = false;
	rst.slot[1].tried = true;
	rec_draw(false);
	CHECK(strcmp(rs.line[1], "Slot 2 ?? tried") == 0);
	/* the running image's descriptor did not parse: the front's alone */
	ui_base();
	rst.own_desc = false;
	rec_draw(false);
	CHECK(strcmp(rs.status_right, "F21") == 0);
	/* the running slot not known: neither line says running */
	ui_base();
	rst.running.established = false;
	fr_rec_target(&rst);
	rec_draw(false);
	CHECK(strcmp(rs.line[0], "Slot 1 WB v1.2.3 checked boot slot") == 0);
	CHECK(strcmp(rs.line[1], "Slot 2 WB v1.3.0 checked") == 0);
}

/* A slot line's parts: the name and the version in the title bar's font,
 * the kind in a filled chip, each state in an outlined chip of its ink. */
static void test_rec_chips(void)
{
	const unsigned y0 = 33u;
	unsigned x;

	ui_base();
	fr_rec_screen(&rs, &rst, &rp, true, NULL);
	/* the control bytes of fr_draw.h, spelled out */
	CHECK(strcmp(rs.line[0], "\x06" "Slot 1 \x03" "WB\x04 v1.2.3\x07 \x05\x04" "checked\x04 "
			     "\x05\x05" "running\x04 \x05\x01" "boot slot\x04") == 0);
	CHECK(row_misses(&rs) == 0u);
	/* "Slot 1" from x 16 in the title font, white-gray, one pixel above
	 * the row's top */
	{
		unsigned n = 0, above = 0;

		for (x = 16; x < 76u; x++)
			for (unsigned y = y0 - 1u; y < y0 + 16u; y++)
				n += fr_screen_pixel(&rs, x, y) == S15_TEXT;
		for (x = 16; x < 76u; x++)
			above += fr_screen_pixel(&rs, x, y0 - 2u) != 0u;
		CHECK(n != 0u && above == 0u);
	}
	/* the WB chip after a title-font space: x 86 to 109, filled, its
	 * corners left out */
	x = 16u + 60u + 10u;
	CHECK(fr_screen_pixel(&rs, x, y0) == 0u && fr_screen_pixel(&rs, x + 1u, y0) == S15_STATUS_BG);
	CHECK(fr_screen_pixel(&rs, x, y0 + 1u) == S15_STATUS_BG);
	CHECK(fr_screen_pixel(&rs, x + 23u, y0 + 15u) == 0u &&
	      fr_screen_pixel(&rs, x + 22u, y0 + 15u) == S15_STATUS_BG);
	CHECK(fr_screen_pixel(&rs, x + 24u, y0 + 8u) == 0u);
	/* the checked chip: a green outline after the version */
	x = 16u + 60u + 10u + 24u + 10u + 60u + 8u;
	CHECK(fr_screen_pixel(&rs, x, y0 + 8u) == S15_GREEN);
	CHECK(fr_screen_pixel(&rs, x + 1u, y0 + 8u) == 0u);
	CHECK(fr_screen_pixel(&rs, x, y0) == 0u && fr_screen_pixel(&rs, x + 1u, y0) == S15_GREEN);
	CHECK(fr_screen_pixel(&rs, x + 63u, y0 + 8u) == S15_GREEN);
	x += 64u + 8u;		/* running, amber */
	CHECK(fr_screen_pixel(&rs, x, y0 + 8u) == S15_AMBER);
	x += 64u + 8u;		/* boot slot, white-gray */
	CHECK(fr_screen_pixel(&rs, x, y0 + 8u) == S15_TEXT && fr_screen_pixel(&rs, x + 79u, y0 + 8u) == S15_TEXT);
	/* a failed slot's chip in red */
	rst.slot[1].good = false;
	rst.slot[1].kind = FR_KIND_NONE;
	fr_rec_screen(&rs, &rst, &rp, true, NULL);
	x = 16u + 60u + 8u;
	CHECK(fr_screen_pixel(&rs, x, y0 + 19u + 8u) == S15_RED);
	CHECK(row_misses(&rs) == 0u);
}

/* Every key digit on recovery's screens is a key badge (s15_console.c's):
 * an amber cell 16 x 16 centered on the digit's column, from 4 pixels left
 * of it and a pixel above the row's top, its corner pixels left out, the
 * digit in bold black. */
static void test_rec_badges(void)
{
	static struct fr_ui ui;
	const unsigned y5 = 33u + 5u * 19u, y8 = 33u + 8u * 19u;

	ui_base();
	fr_rec_screen(&rs, &rst, &rp, true, NULL);
	CHECK(row_misses(&rs) == 0u);
	/* "1  Change ...": the badge at column 0 */
	CHECK(fr_screen_pixel(&rs, 12, y5) == S15_AMBER && fr_screen_pixel(&rs, 12, y5 - 1u) == 0u);
	CHECK(fr_screen_pixel(&rs, 13, y5 - 1u) == S15_AMBER && fr_screen_pixel(&rs, 27, y5) == S15_AMBER);
	CHECK(fr_screen_pixel(&rs, 27, y5 - 1u) == 0u && fr_screen_pixel(&rs, 27, y5 + 14u) == 0u);
	CHECK(fr_screen_pixel(&rs, 12, y5 + 14u) == 0u && fr_screen_pixel(&rs, 13, y5 + 14u) == S15_AMBER);
	CHECK(fr_screen_pixel(&rs, 11, y5) == 0u && fr_screen_pixel(&rs, 28, y5) == 0u);
	CHECK(fr_screen_pixel(&rs, 16, y5 + 15u) == 0u);
	{
		unsigned black = 0;

		for (unsigned x = 16; x < 25u; x++)
			for (unsigned y = y5; y < y5 + 14u; y++)
				black += fr_screen_pixel(&rs, x, y) == 0u;
		CHECK(black > 8u);	/* the digit */
	}
	/* "8  Back" and "Hold 1 for 5 s to confirm" */
	memset(&ui, 0, sizeof ui);
	ui.screen = FR_UI_CONFIRM;
	ui.act = FR_ACT_TRY;
	fr_rec_screen(&rs, &rst, &rp, true, &ui);
	CHECK(row_misses(&rs) == 0u);
	CHECK(fr_screen_pixel(&rs, 16, y8 + 7u) == S15_AMBER || fr_screen_pixel(&rs, 17, y8 + 7u) == 0u);
	CHECK(fr_screen_pixel(&rs, 12, y8 + 1u) == S15_AMBER);
	CHECK(fr_screen_pixel(&rs, 16u + 5u * 8u - 1u, 33u + 6u * 19u + 1u) == S15_AMBER);
}

/* The menu's rows 3 to 6, in order, as one string with '|' between. */
static void menu_items(char *out, size_t max)
{
	size_t used = 0;

	out[0] = '\0';
	for (unsigned i = 3; i <= 6u; i++)
		if (rs.line[i][0] != '\0' && used + strlen(rs.line[i]) + 2u < max) {
			if (used != 0u)
				out[used++] = '|';
			strcpy(out + used, rs.line[i]);
			used += strlen(rs.line[i]);
		}
}

/* The slot menu: an item for each action that would start, confirmed, and
 * only those; key 8's way back; the line of what holds. */
static void test_rec_menu(void)
{
	static struct fr_ui ui;
	char items[256];

	memset(&ui, 0, sizeof ui);
	ui.screen = FR_UI_SLOTS;
	/* both good, slot 1 runs: try and switch */
	ui_base();
	rui = &ui;
	rec_draw(true);
	CHECK(strcmp(rs.title, "Recovery: slots") == 0);
	menu_items(items, sizeof items);
	CHECK(strcmp(items, "1  Try slot 2 once (suggested)|2  Make slot 2 the boot slot") == 0);
	CHECK(has("recovery:action:try", n2) && has("recovery:action:switch", n2) &&
	      has("recovery:back", NULL));
	CHECK(strcmp(rs.line[8], "8  Back") == 0);
	CHECK(rs.line[2][0] == '\0' && rs.line[7][0] == '\0');
	CHECK(strstr(rs.line[0], "Slot 1") == rs.line[0] && strstr(rs.line[1], "Slot 2") == rs.line[1]);
	/* slot 2 tried and started: all four */
	rst.slot[1].good = false;
	rst.slot[1].tried = true;
	rst.slot[1].started = true;
	rst.slot[1].has_log = true;
	rst.slot[1].log0 = 0xFCu;
	fr_rec_target(&rst);
	rec_draw(true);
	menu_items(items, sizeof items);
	CHECK(strcmp(items, "1  Try slot 2 once (suggested)|2  Make slot 2 the boot slot|"
			    "3  Keep slot 2|4  Put slot 2's record back") == 0);
	/* tried, not started: no keep */
	rst.slot[1].started = false;
	rst.slot[1].log0 = 0xFDu;
	rec_draw(true);
	menu_items(items, sizeof items);
	CHECK(strcmp(items, "1  Try slot 2 once (suggested)|2  Make slot 2 the boot slot|"
			    "4  Put slot 2's record back") == 0);
	/* slot 2 runs, slot 1 bad and holding no stream: nothing offered */
	ui_base();
	rst.running.slot = 1;
	rst.sel.index = 0x01;
	rst.slot[0].good = false;
	rst.slot[0].bootable = false;
	rst.slot[0].has_desc = rst.slot[0].has_front = false;
	fr_rec_target(&rst);
	rec_draw(true);
	menu_items(items, sizeof items);
	CHECK(strncmp(items, "1  Try slot 1 once", 18) == 0 && strstr(items, "Make") == NULL);
	CHECK(strcmp(rs.line[7], "Slot 1 holds nothing the unit can start") == 0);
	/* a trial start: slot 2 tried and runs, slot 1 trusted: keep slot 2,
	 * the running slot; no try or switch, slot 1 already starts */
	ui_base();
	rst.running.slot = 1;
	rst.slot[1].good = false;
	rst.slot[1].tried = true;
	rst.slot[1].has_log = true;
	rst.slot[1].log0 = 0xFDu;
	fr_rec_target(&rst);
	rec_draw(true);
	menu_items(items, sizeof items);
	CHECK(strcmp(items, "3  Keep slot 2, the running slot") == 0);
	CHECK(strcmp(rs.line[7], "Slot 1 is the boot slot: it starts on every start") == 0);
	/* a try written, not started: slot 2 named once */
	ui_base();
	rst.sel.index = 0xFF;
	rst.sel.verified = 0xFF;
	rst.slot[1].good = false;
	rst.slot[1].tried = true;
	rst.home.trial = true;
	fr_rec_target(&rst);
	rec_draw(true);
	CHECK(strcmp(rs.line[7], "Slot 2 starts once at the next start") == 0);
	menu_items(items, sizeof items);
	CHECK(strstr(items, "4  Put slot 2's record back") != NULL);
	/* the running slot not known */
	ui_base();
	rst.running.established = false;
	fr_rec_target(&rst);
	rec_draw(true);
	CHECK(strcmp(rs.line[3], "No write: the unit cannot tell which slot is running") == 0);
	CHECK(strcmp(rs.line[8], "8  Back") == 0);
	/* an action that did not start: why, on row 7 */
	ui_base();
	ui.refusal = FR_WR_FLASH;
	rec_draw(true);
	CHECK(strcmp(rs.line[7], "Failed: the flash did not take it; write again") == 0);
	CHECK(rs.ink[7] == FR_INK_ERROR);
	ui.refusal = FR_WR_OK;
	/* nothing at all to choose: the selector could not be read */
	ui_base();
	rst.read_error = true;
	fr_rec_target(&rst);
	rec_draw(true);
	CHECK(strcmp(rs.line[3], "Nothing to choose now") == 0);
	/* while a write runs, the main screen whatever the keys' screen */
	ui_base();
	rp.phase = FR_PH_WRITING;
	rp.w.way = FR_WAY_PLAIN;
	rp.w.target = 1;
	rp.w.state = FR_WS_SLOT;
	rp.w.n = 0x4000u;
	rec_draw(true);
	CHECK(strcmp(rs.title, "Recovery") == 0 && !anywhere("Back"));
	rui = NULL;
}

/* The confirm screen of each action and each tier: what will be done, the
 * warning, the hold and its bar, the way back. */
static void test_rec_confirm(void)
{
	static struct fr_ui ui;
	static const struct {
		uint8_t act, tier;
		const char *r2, *r3, *r4;
	} cs[] = {
		{ FR_ACT_TRY, FR_TIER_SAME, "Slot 2 (v1.3.0) will start once, as a trial", "",
		  "If it does not start, switch off and on: slot 1 boots" },
		{ FR_ACT_TRY, FR_TIER_FRONT, "Slot 2 (v1.3.0) will start once, as a trial",
		  "It has another front: recovery may not open from it",
		  "If it does not start, switch off and on: slot 1 boots" },
		{ FR_ACT_TRY, FR_TIER_FOREIGN, "Slot 2 (v1.3.0) will start once, as a trial",
		  "It is not a waffle-boot image: no recovery from there",
		  "If it does not start, switch off and on: slot 1 boots" },
		{ FR_ACT_TRY, FR_TIER_BAD, "Slot 2 (an image) will start once, as a trial",
		  "Its check failed: it may not start",
		  "If it does not start, switch off and on: slot 1 boots" },
		{ FR_ACT_SWITCH, FR_TIER_SAME, "Slot 2 (v1.3.0) will be the boot slot",
		  "A trial first costs one restart, and is safer", "" },
		{ FR_ACT_SWITCH, FR_TIER_FRONT, "Slot 2 (v1.3.0) will be the boot slot",
		  "It has another front: recovery may not open from it", "Try it once first: 8 then 1" },
		{ FR_ACT_SWITCH, FR_TIER_FOREIGN, "Slot 2 (v1.3.0) will be the boot slot",
		  "It is not a waffle-boot image: no recovery from there",
		  "Try it once first: 8 then 1" },
		{ FR_ACT_SWITCH, FR_TIER_BAD, "Slot 2 (an image) will be the boot slot",
		  "Its check failed: nothing will ever check it again",
		  "There may be no way back without a flash programmer" },
	};

	memset(&ui, 0, sizeof ui);
	ui.screen = FR_UI_CONFIRM;
	for (unsigned i = 0; i < ARRAY_LEN(cs); i++) {
		struct fr_act_plan plan;

		ui_base();
		tier_b(cs[i].tier);
		fr_rec_target(&rst);
		fr_rec_act(&rst, cs[i].act, true, &plan);
		CHECK(plan.status == FR_WR_OK && plan.tier == cs[i].tier);
		ui.act = cs[i].act;
		ui.hold_pct = (uint8_t)(i * 12u);
		rui = &ui;
		rec_draw(true);
		CHECK(strcmp(rs.title, "Recovery: confirm") == 0);
		CHECK(strcmp(rs.line[2], cs[i].r2) == 0);
		CHECK(strcmp(rs.line[3], cs[i].r3) == 0);
		CHECK(strcmp(rs.line[4], cs[i].r4) == 0);
		CHECK(rs.line[5][0] == '\0');
		CHECK(strcmp(rs.line[6], "Hold 1 for 5 s to confirm") == 0);
		CHECK(rs.bar_line == 7 && rs.bar_pm == ui.hold_pct * 10u && rs.bar_cols != 0u);
		CHECK(has("recovery:confirm", NULL));
		CHECK(strcmp(rs.line[8], "8  Back") == 0);
	}
	/* keep and put back of a tried slot 2; keep of the running slot 2 in
	 * its trial start */
	ui_base();
	rst.slot[1].good = false;
	rst.slot[1].tried = true;
	rst.slot[1].started = true;
	fr_rec_target(&rst);
	ui.act = FR_ACT_KEEP;
	ui.hold_pct = 100;
	rec_draw(true);
	CHECK(strcmp(rs.line[2], "Slot 2, which was tried, will be kept:") == 0);
	CHECK(strcmp(rs.line[3], "it will be the boot slot") == 0);
	CHECK(rs.bar_pm == 1000u);
	ui.act = FR_ACT_PUT_BACK;
	ui.hold_pct = 0;
	rec_draw(true);
	CHECK(strcmp(rs.line[2], "Slot 2's record will be put back:") == 0);
	CHECK(strcmp(rs.line[3], "it reads checked again; the unit starts as it does now") == 0);
	CHECK(rs.bar_line == 7 && rs.bar_pm == 0u);
	ui_base();
	rst.running.slot = 1;
	rst.slot[1].good = false;
	rst.slot[1].tried = true;
	fr_rec_target(&rst);
	ui.act = FR_ACT_KEEP;
	rec_draw(true);
	CHECK(strcmp(rs.line[2], "Slot 2, which was tried, will be kept:") == 0);
	/* slot 2 runs: a try of slot 1 falls back to slot 2 */
	ui_base();
	rst.running.slot = 1;
	rst.sel.index = 0x01;
	fr_rec_target(&rst);
	ui.act = FR_ACT_TRY;
	rec_draw(true);
	CHECK(strcmp(rs.line[2], "Slot 1 (v1.2.3) will start once, as a trial") == 0);
	CHECK(strcmp(rs.line[4], "If it does not start, switch off and on: slot 2 boots") == 0);
	rui = NULL;
}

/* S-02.084: the countdown before a host's write or action: what will be
 * written, how to stop it, the bar filling as the seconds go. */
static void test_rec_countdown(void)
{
	static struct fr_ui ui;

	ui_base();
	rp.phase = FR_PH_COUNTDOWN;
	rp.w.way = FR_WAY_PLAIN;
	rp.w.target = 1;
	rp.has_version = true;
	memcpy(rp.version, v_real, sizeof v_real);
	for (unsigned left = 15; left >= 1u; left--) {
		rp.count_left = (uint8_t)left;
		rec_draw(true);
		CHECK(strcmp(rs.title, "Recovery") == 0);
		CHECK(strcmp(rs.line[2], "Slot 2 will be erased and written with v1.0.10") == 0);
		CHECK(strcmp(rs.line[3], "and is the boot slot") == 0);
		CHECK(rs.line[4][0] == '\0' && rs.line[5][0] == '\0');
		{
			char want[FR_COLS + 1];

			strcpy(want, "Switch the unit off within ");
			want[strlen(want) + (left >= 10u ? 2u : 1u)] = '\0';
			if (left >= 10u) {
				want[strlen("Switch the unit off within ")] = (char)('0' + left / 10u);
				want[strlen("Switch the unit off within ") + 1u] = (char)('0' + left % 10u);
			} else {
				want[strlen("Switch the unit off within ")] = (char)('0' + left);
			}
			strcat(want, " s to stop");
			CHECK(strcmp(rs.line[6], want) == 0);
		}
		CHECK(rs.bar_line == 7 && rs.bar_pm == 1000u - left * 1000u / 15u);
		CHECK(!anywhere("change") && !anywhere("Back"));
	}
	/* the keys' screens give way to it */
	memset(&ui, 0, sizeof ui);
	ui.screen = FR_UI_CONFIRM;
	ui.act = FR_ACT_TRY;
	rui = &ui;
	rec_draw(true);
	CHECK(strcmp(rs.title, "Recovery") == 0 && !anywhere("Hold 1"));
	rui = NULL;
	/* forced */
	rp.flags = FR_BEGIN_F_FORCE;
	rec_draw(true);
	CHECK(strcmp(rs.line[4], "Forced: the unit's own refusals are off") == 0);
	/* in place, with no version known */
	rp.flags = FR_BEGIN_F_RUNNING;
	rp.w.way = FR_WAY_IN_PLACE;
	rp.w.target = 0;
	rp.has_version = false;
	rec_draw(true);
	CHECK(strcmp(rs.line[2], "Slot 1, the running slot, will be written in place") == 0);
	CHECK(strcmp(rs.line[3], "with an image: no slot to fall back on") == 0);
	/* a trial write */
	rp.flags = FR_BEGIN_F_TRIAL;
	rp.w.way = FR_WAY_TRIAL;
	rp.w.target = 1;
	rec_draw(true);
	CHECK(strcmp(rs.line[3], "and started once, as a trial, at the next start") == 0);
	CHECK(strcmp(rs.line[4], "If it does not start, switch off and on: slot 1 boots") == 0);
	/* a select first */
	rp.flags = FR_BEGIN_F_RUNNING;
	rp.w.way = FR_WAY_SELECT_FIRST;
	rp.w.target = 0;
	rec_draw(true);
	CHECK(strcmp(rs.line[3], "It is running; slot 2 becomes the boot slot first") == 0);
	rp.w.way = FR_WAY_PLAIN;
	rec_draw(true);
	CHECK(strcmp(rs.line[3], "It is the running slot; slot 2 stays the boot slot") == 0);
	/* an action from the host: a try, its tier's warning */
	rp.flags = 0;
	rp.w.way = FR_WAY_TRY;
	rp.w.target = 1;
	rp.tier = FR_TIER_FRONT;
	rp.count_left = 15;
	rp.has_version = true;
	memcpy(rp.version, v_b, sizeof v_b);
	rec_draw(true);
	CHECK(strcmp(rs.line[2], "Slot 2 (v1.3.0) will start once, as a trial") == 0);
	CHECK(strcmp(rs.line[3], "It has another front: recovery may not open from it") == 0);
	CHECK(strcmp(rs.line[6], "Switch the unit off within 15 s to stop") == 0);
}

/* The done screen: what was done, how to start it, never a refusal of the
 * start's state on row 2; a request refused since the write, as what to
 * do, on row 4. */
static void test_rec_done(void)
{
	static const struct {
		uint8_t way, target;
		const char *r5, *r6;
	} ds[] = {
		{ FR_WAY_PLAIN, 1, "Done: slot 2 is written and is the boot slot",
		  "Restart to start it (host or power switch)" },
		{ FR_WAY_SELECT_FIRST, 0, "Done: slot 1 is written and is the boot slot",
		  "Restart to start it (host or power switch)" },
		{ FR_WAY_TRIAL, 1, "Done: slot 2 is written for one trial start",
		  "Restart to start it once (host or power switch)" },
		{ FR_WAY_TRY, 1, "Done: slot 2 starts once, as a trial",
		  "Restart to start it once (host or power switch)" },
		{ FR_WAY_SWITCH, 1, "Done: slot 2 is the boot slot",
		  "Restart to start it (host or power switch)" },
		{ FR_WAY_KEEP, 1, "Done: slot 2 is kept as the boot slot",
		  "Restart to start it (host or power switch)" },
		{ FR_WAY_SELECT, 1, "Done: slot 2 is kept as the boot slot",
		  "Restart to start it (host or power switch)" },
		{ FR_WAY_PUT_BACK, 1, "Done: slot 2's record is back; it is checked", "" },
	};

	for (unsigned i = 0; i < ARRAY_LEN(ds); i++) {
		ui_base();
		/* the selector after a write names the slot written */
		rst.sel.index = ds[i].target;
		rst.sel.verified = 0x00;
		fr_rec_target(&rst);
		CHECK(rst.target == FR_NO_SLOT);
		rp.phase = FR_PH_DONE;
		rp.made = true;
		rp.w.way = ds[i].way;
		rp.w.target = ds[i].target;
		rp.w.state = FR_WS_DONE;
		rp.w.result = FR_WR_OK;
		rec_draw(true);
		CHECK(strcmp(rs.line[6], ds[i].r5) == 0 && rs.ink[6] == FR_INK_OK);
		CHECK(strcmp(rs.line[7], ds[i].r6) == 0);
		CHECK(rs.line[2][0] == '\0' && rs.line[3][0] == '\0' && rs.line[4][0] == '\0');
		CHECK(!anywhere("no write") && !shows("not the selected one"));
		CHECK(strcmp(rs.line[5], "1  Change the boot slot (optional)") == 0);
		if (ds[i].way == FR_WAY_TRIAL || ds[i].way == FR_WAY_TRY)
			CHECK(strcmp(rs.line[8], "If it does not start, switch off and on: slot 1 boots") == 0);
		/* a request refused since: what to do first, on row 4 */
		rp.refused = FR_WR_NOT_SELECTED;
		rec_draw(true);
		CHECK(strcmp(rs.line[4], "Restart before another write") == 0 && rs.ink[4] == FR_INK_WARN);
		CHECK(strcmp(rs.line[6], ds[i].r5) == 0);
		rp.refused = FR_WR_ALREADY;
		rec_draw(true);
		CHECK(strcmp(rs.line[4], "Nothing to do: that slot is the boot slot already") == 0);
		rp.refused = FR_WR_OK;
	}
	/* not shown in another phase */
	ui_base();
	rp.phase = FR_PH_FAILED;
	rp.result = FR_WR_FLASH;
	rp.refused = FR_WR_NOT_SELECTED;
	rec_draw(true);
	CHECK(!shows("Restart before another write"));
}

/*
 * Every line a screen can show fits a row whole, with the version a real
 * build carries (v1.0.10) in both slots, the running image and an image
 * written: each refusal, as a failed write's result, an action's on the
 * menu and a refused request's after a write; each way's lines while
 * counted down to, received, written and done, for either slot as the
 * target and either as running, forced or not, with and without a version;
 * each action's confirm screen at each tier; the menu in each state.
 * rec_draw() checks that no row is cut or clipped and names no tool, and
 * the painter on every 31st screen.
 */
static void test_rec_fits(void)
{
	static struct fr_ui ui;

	paint_check = false;

	for (unsigned run = 0; run < 2u; run++) {
		const unsigned b = 1u - run;
		unsigned n = 0;

		/* refusals and results */
		for (unsigned rc = 0; rc <= FR_ST_DAMAGED; rc++) {
			if (rc > FR_WR_STOPPED && rc < FR_ST_BAD_REQUEST)
				continue;
			for (unsigned v = 0; v < 4u; v++) {
				ui_base();
				rst.running.slot = (uint8_t)run;
				rst.sel.index = (uint8_t)run;
				memcpy(rst.slot[0].desc.bl_version, v_real, sizeof v_real);
				memcpy(rst.slot[1].desc.bl_version, v_real, sizeof v_real);
				memcpy(rst.desc.bl_version, v_real, sizeof v_real);
				want_status = "v1.0.10 F21";
				fr_rec_target(&rst);
				rp.made = (v & 1u) != 0u;
				rp.flags = (v & 2u) ? FR_BEGIN_F_SELECT : 0u;
				rp.phase = FR_PH_FAILED;
				rp.result = (uint8_t)rc;
				rec_draw(true);
				rp.phase = FR_PH_DONE;
				rp.w.way = FR_WAY_PLAIN;
				rp.w.target = (uint8_t)b;
				rp.refused = (uint8_t)rc;
				rec_draw(true);
				rp.phase = FR_PH_IDLE;
				memset(&ui, 0, sizeof ui);
				ui.screen = FR_UI_SLOTS;
				ui.refusal = (uint8_t)rc;
				rui = &ui;
				rec_draw(true);
				rui = NULL;
				n += 3u;
			}
		}
		/* every way, every phase it shows in */
		for (unsigned way = FR_WAY_NONE; way <= FR_WAY_PUT_BACK; way++)
			for (unsigned t = 0; t < 2u; t++)
				for (unsigned v = 0; v < 4u; v++)
					for (unsigned tier = FR_TIER_NONE; tier <= FR_TIER_BAD; tier++) {
						static const uint8_t ph[] = { FR_PH_COUNTDOWN, FR_PH_RECEIVING,
									     FR_PH_WRITING, FR_PH_DONE };

						for (unsigned i = 0; i < ARRAY_LEN(ph); i++) {
							ui_base();
							rst.running.slot = (uint8_t)run;
							rst.sel.index = (uint8_t)run;
							fr_rec_target(&rst);
							rp.phase = ph[i];
							rp.flags = (v & 1u) ? FR_BEGIN_F_FORCE : 0u;
							rp.has_version = (v & 2u) != 0u;
							memcpy(rp.version, v_real, sizeof v_real);
							rp.tier = (uint8_t)tier;
							rp.count_left = 15;
							rp.w.way = (uint8_t)way;
							rp.w.target = (uint8_t)t;
							rp.w.act = way >= FR_WAY_TRY ? FR_ACT_TRY : FR_ACT_NONE;
							rp.w.n = 0x4000u;
							rp.w.state = ph[i] == FR_PH_DONE ? FR_WS_DONE : FR_WS_SLOT;
							rp.w.result = FR_WR_OK;
							rp.plan_way = (uint8_t)way;
							rp.plan_target = (uint8_t)t;
							rp.len = FR_LOG_IMAGE_MAX;
							rp.got = FR_LOG_IMAGE_MAX - 1u;
							rec_draw(true);
							n++;
						}
					}
		/* every action's confirm screen at every tier, and the menu */
		for (unsigned act = FR_ACT_TRY; act <= FR_ACT_PUT_BACK; act++)
			for (unsigned tier = FR_TIER_SAME; tier <= FR_TIER_BAD; tier++)
				for (unsigned tried = 0; tried < 3u; tried++) {
					ui_base();
					rst.running.slot = (uint8_t)run;
					rst.sel.index = (uint8_t)run;
					memcpy(rst.slot[b].desc.bl_version, v_real, sizeof v_real);
					tier_b(tier);
					if (tried != 0u && tier != FR_TIER_BAD) {
						rst.slot[b].good = false;
						rst.slot[b].tried = true;
						rst.slot[b].started = tried == 2u;
					}
					fr_rec_target(&rst);
					memset(&ui, 0, sizeof ui);
					ui.screen = FR_UI_CONFIRM;
					ui.act = (uint8_t)act;
					ui.hold_pct = 50;
					rui = &ui;
					rec_draw(true);
					ui.screen = FR_UI_SLOTS;
					rec_draw(true);
					rui = NULL;
					n += 2u;
				}
		CHECK(n > 1000u);
	}
	paint_check = true;
}

/* The painter against the model on rows with every kind of part: inks,
 * title glyphs, chips and rings of each ink, badges, parts at the clip and
 * past it, a chip with no end, arguments cut off, a bar over parts; and a
 * repaint of the rows that changed against a whole paint. */
static void test_parts(void)
{
	/* fr_draw.h's control bytes spelled out: \x01 ink, \x02 badge, \x03
	 * chip, \x04 end, \x05 ring, \x06 title font, \x07 body font; an
	 * ink's argument 1 + enum fr_ink */
	static const char *const lines[] = {
		"\x06" "Slot 1 " "\x03" "WB" "\x04" " v0.9.36" "\x07" " " "\x05" "\x04"
		"checked" "\x04" " " "\x05" "\x05" "running" "\x04" " " "\x05" "\x01"
		"boot slot" "\x04",
		"\x06" "Slot 2 " "\x03" "TE" "\x04" " v1000.0.20+3" "\x07" " " "\x05"
		"\x05" "tried, started" "\x04" " " "\x05" "\x05" "running" "\x04" " "
		"\x05" "\x01" "boot slot" "\x04",
		"\x02" "1" "  Change the boot slot (optional)",
		"Hold " "\x02" "1" " for 5 s to " "\x01" "\x06" "confirm" "\x01" "\x04" "!",
		"Try it once first (" "\x02" "8" ", then " "\x02" "1" ")",
		"\x05" "\x06" "check failed" "\x04" "\x03" "??" "\x04" "\x05" "\x7f"
		"odd ink" "\x04" "\x01" "\x09" "x",
		"WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW" "\x02" "9" "\x02" "8",
		"\x06" "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM",
		"\x03" "no end, and on to the row's end" "\x05",
	};
	struct fr_screen s, t;

	fr_screen_text(&s, "Recovery: confirm");
	for (unsigned i = 0; i < FR_LINES; i++)
		fr_screen_line(&s, i, (enum fr_ink)(i % 6u), lines[i]);
	fr_screen_status(&s, "USB", "v0.9.36 F21");
	CHECK(row_misses(&s) == 0u);
	/* the cut ends: an argument, a badge's digit, a chip's text */
	fr_screen_text(&t, "Recovery");
	fr_screen_line(&t, 0, FR_INK_TEXT, "a" "\x01");
	fr_screen_line(&t, 1, FR_INK_TEXT, "b" "\x02");
	fr_screen_line(&t, 2, FR_INK_TEXT, "c" "\x05");
	fr_screen_line(&t, 3, FR_INK_TEXT, "\x04" "\x07" "d" "\x06");
	fr_screen_line(&t, 8, FR_INK_HEAD, "\x06" "gyp|" "\x02" "0");
	t.bar_line = 2;
	t.bar_col = 0;
	t.bar_cols = 10;
	t.bar_pm = 333;
	CHECK(row_misses(&t) == 0u);
	/* a whole row's 64 bytes, every one a glyph, no NUL in its field */
	memset(t.line[4], 'g', sizeof t.line[4]);
	memset(t.line[5], '\x06', sizeof t.line[5]);
	t.line[5][3] = 'Q';
	CHECK(row_misses(&t) == 0u);
	/* a repaint over another screen: only what changed, the same frame */
	rows_of(&s, inc);
	fr_paint_rows(inc, &t, &s);
	rows_of(&t, frame);
	CHECK(memcmp(inc, frame, sizeof frame) == 0);
	fr_paint_rows(inc, &s, &t);
	rows_of(&s, frame);
	CHECK(memcmp(inc, frame, sizeof frame) == 0);
	/* a bar's fill moved by a pixel, then the bar off */
	s.bar_line = 6;
	s.bar_cols = 40;
	s.bar_pm = 500;
	rows_of(&s, inc);
	t = s;
	t.bar_pm = 504;
	fr_paint_rows(inc, &t, &s);
	rows_of(&t, frame);
	CHECK(memcmp(inc, frame, sizeof frame) == 0);
	s = t;
	t.bar_line = -1;
	fr_paint_rows(inc, &t, &s);
	rows_of(&t, frame);
	CHECK(memcmp(inc, frame, sizeof frame) == 0);
	/* from the logo, or with no screen before, the whole screen */
	fr_screen_logo(&s);
	rows_of(&s, inc);
	fr_paint_rows(inc, &t, &s);
	CHECK(memcmp(inc, frame, sizeof frame) == 0);
	fr_paint_rows(inc, &t, NULL);
	CHECK(memcmp(inc, frame, sizeof frame) == 0);
}

/* S-13c.025: the logo line's band, window row by window row, 16 pixels
 * each, the text centered in dim gray on black; against the model. */
static void test_line(void)
{
	static const char text[] = "Loading recovery menu...";
	static uint8_t band[FR_LINE_BYTES];
	unsigned bad = 0, dim = 0;

	CHECK(FR_LINE_BYTES == FR_PANEL_LINE_BYTES && FR_LINE_BYTES == 15360u);
	memset(band, 0x5A, sizeof band);
	fr_paint_line(band, text);
	for (unsigned r = 0; r < 480u; r++)
		for (unsigned dy = 0; dy < 16u; dy++) {
			const uint8_t *p = band + 2u * (r * 16u + dy);
			const uint16_t got = (uint16_t)(p[0] << 8 | p[1]);

			bad += got != fr_line_pixel(text, 479u - r, dy);
			dim += got == S15_DIM;
			/* centered: 24 glyphs from x 144 */
			if (r <= 479u - 336u || r >= 480u - 144u)
				bad += got != 0u;
		}
	CHECK(bad == 0u && dim != 0u);
	CHECK(S15_DIM == 0x8C71u && FR_LINE_Y == 140u && FR_PANEL_LINE_COL == 49u + 140u);
}

/* ---- the panel's sends (src/front/fr_panel.c, S-13c.016 - S-13c.025) ----
 * A bus whose TE line, DMA and clock the test sets. */

enum { PE_CMD = 1, PE_DATA, PE_DMA, PE_STOP };

struct pev {
	uint8_t kind, cmd;
	uint32_t n;
	uint64_t t;
	const uint8_t *p;
};

static struct pbus {
	uint64_t now;
	uint32_t te_period, te_high;	/* TE high te_high us of each period; 0: none */
	uint32_t dma_us;		/* a DMA send's time; 0: it never ends */
	bool dma_error;
	bool dma_on;
	uint64_t dma_end;
	struct pev ev[8192];
	unsigned n;
} pb;

static void pb_put(uint8_t kind, uint8_t cmd, uint32_t n, const uint8_t *p)
{
	if (pb.n < ARRAY_LEN(pb.ev)) {
		pb.ev[pb.n].kind = kind;
		pb.ev[pb.n].cmd = cmd;
		pb.ev[pb.n].n = n;
		pb.ev[pb.n].t = pb.now;
		pb.ev[pb.n].p = p;
		pb.n++;
	}
}

static void pb_reset(void *ctx, bool high) { (void)ctx; (void)high; }
static void pb_select(void *ctx, bool on) { (void)ctx; (void)on; }
static void pb_cmd(void *ctx, uint8_t c) { (void)ctx; pb_put(PE_CMD, c, 0, NULL); }

static void pb_data(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	pb_put(PE_DATA, 0, (uint32_t)n, p);
}

static bool pb_te(void *ctx)
{
	(void)ctx;
	return pb.te_period != 0u && pb.now % pb.te_period < pb.te_high;
}

static void pb_dma_start(void *ctx, const uint8_t *p, uint32_t n)
{
	(void)ctx;
	CHECK(!pb.dma_on);		/* started only on a stopped channel */
	pb.dma_on = true;
	pb.dma_end = pb.now + pb.dma_us;
	pb_put(PE_DMA, 0, n, p);
}

static int pb_dma_state(void *ctx)
{
	(void)ctx;
	if (!pb.dma_on)
		return FR_DMA_IDLE;
	if (pb.dma_error)
		return FR_DMA_ERROR;
	return pb.dma_us != 0u && pb.now >= pb.dma_end ? FR_DMA_IDLE : FR_DMA_BUSY;
}

static void pb_dma_stop(void *ctx)
{
	(void)ctx;
	pb.dma_on = false;
	pb_put(PE_STOP, 0, 0, NULL);
}

static uint64_t pb_now(void *ctx)
{
	(void)ctx;
	return pb.now;
}

static void pb_delay(void *ctx, uint32_t us)
{
	(void)ctx;
	pb.now += us;
}

static const struct fr_panel_bus pbus = { pb_reset, pb_select, pb_cmd, pb_data, pb_te,
					  pb_dma_start, pb_dma_state, pb_dma_stop, NULL };
static const struct fr_clock pclock = { pb_now, pb_delay, NULL };
static struct fr_panel pp;
static unsigned p_served;

static void p_serve(void)
{
	p_served++;
}

/* A started panel with TE at 60 Hz and 15 ms DMA sends; the log from here. */
static void p_up(void)
{
	memset(&pb, 0, sizeof pb);
	pb.now = 1000;
	pb.te_period = 16667;
	pb.te_high = 400;
	pb.dma_us = 15000;
	fr_panel_begin(&pp, &pbus, &pclock, 0, 0, 1);
	fr_panel_start(&pp);
	pb.n = 0;
}

/* The log's events of kind from i on; the first's index, or pb.n. */
static unsigned p_next(unsigned i, uint8_t kind)
{
	while (i < pb.n && pb.ev[i].kind != kind)
		i++;
	return i;
}

/* Whether the events from i are a CPU send: n rows of row bytes from buf,
 * the link served after each. */
static bool p_cpu_rows(unsigned i, const uint8_t *buf, uint32_t row)
{
	for (unsigned r = 0; r < 480u; r++, i++)
		if (i >= pb.n || pb.ev[i].kind != PE_DATA || pb.ev[i].n != row ||
		    pb.ev[i].p != buf + (size_t)r * row)
			return false;
	return true;
}

static bool on_edge(uint64_t t)
{
	return pb.te_period != 0u && t % pb.te_period < pb.te_high && t % pb.te_period <= 20u;
}

/* S-13c.010, S-13c.017: the early logo frame unpaced, by one DMA, its end
 * awaited, then DISPON; every later frame from the previous one's end on a
 * TE rising edge, left going out, the buffer free once it has ended and
 * both channels stopped; at most one a TE period. */
static void test_panel_paced(void)
{
	unsigned i;
	uint64_t t0;

	p_up();
	t0 = pb.now = 5000;		/* TE high here: no edge to wait for */
	fr_panel_frame(&pp, fb);
	CHECK(pb.n == 4u && pb.ev[0].kind == PE_CMD && pb.ev[0].cmd == 0x2Cu);
	CHECK(pb.ev[1].kind == PE_DMA && pb.ev[1].n == 215040u && pb.ev[1].p == fb && pb.ev[1].t == t0);
	CHECK(pb.ev[2].kind == PE_STOP && pb.ev[2].t >= t0 + 15000u && pb.ev[2].t < t0 + 15100u);
	CHECK(pb.ev[3].kind == PE_CMD && pb.ev[3].cmd == 0x29u && pp.on && pp.te_misses == 0u);
	CHECK(!fr_panel_busy(&pp) && !pb.dma_on);
	/* the next frame: on the next rising edge, then going out */
	pb.n = 0;
	fr_panel_frame(&pp, fb);
	CHECK(pb.n == 2u && pb.ev[0].cmd == 0x2Cu && pb.ev[1].kind == PE_DMA && on_edge(pb.ev[1].t));
	CHECK(pb.ev[1].t > t0 + 15000u && pp.te_misses == 0u);
	CHECK(fr_panel_busy(&pp));
	pb.now += 14000u;
	CHECK(fr_panel_busy(&pp));
	pb.now += 1000u;
	CHECK(!fr_panel_busy(&pp) && !pb.dma_on && pb.ev[pb.n - 1u].kind == PE_STOP);
	/* two frames asked at once: the second after the first's end, on the
	 * next edge, one a TE period */
	pb.n = 0;
	p_served = 0;
	fr_serve_hook = p_serve;
	fr_panel_frame(&pp, fb);
	fr_panel_frame(&pp, frame);
	fr_serve_hook = NULL;
	i = p_next(0, PE_DMA);
	CHECK(i < pb.n && p_next(i + 1u, PE_DMA) < pb.n);
	{
		const unsigned j = p_next(i + 1u, PE_DMA);

		CHECK(pb.ev[j].p == frame && on_edge(pb.ev[j].t) && on_edge(pb.ev[i].t));
		CHECK(pb.ev[j].t >= pb.ev[i].t + 15000u && pb.ev[j].t - pb.ev[i].t + 20u >= pb.te_period);
		CHECK(p_next(i, PE_STOP) < j);	/* the first reaped before */
	}
	CHECK(p_served != 0u);		/* the link served in the waits */
	fr_panel_idle(&pp);
	CHECK(!fr_panel_busy(&pp));
}

/* S-13c.017: with no TE edge a frame starts 50 ms after it could, and is
 * counted (saturating); an edge that comes ends the wait. */
static void test_panel_te_miss(void)
{
	uint64_t t0;

	p_up();
	fr_panel_frame(&pp, fb);
	pb.te_period = 0;
	pb.n = 0;
	t0 = pb.now;
	fr_panel_frame(&pp, fb);
	CHECK(pb.ev[1].kind == PE_DMA && pb.ev[1].t >= t0 + 50000u && pb.ev[1].t <= t0 + 50020u);
	CHECK(pp.te_misses == 1u);
	pp.te_misses = 254;
	fr_panel_frame(&pp, fb);
	fr_panel_frame(&pp, fb);
	CHECK(pp.te_misses == 255u);
	/* TE high all the time: no rising edge either */
	pb.te_period = 10;
	pb.te_high = 10;
	fr_panel_idle(&pp);
	t0 = pb.now;
	pb.n = 0;
	fr_panel_frame(&pp, fb);
	CHECK(pb.ev[1].t >= t0 + 50000u && pp.te_misses == 255u);
	/* edges again */
	pb.te_period = 16667;
	pb.te_high = 400;
	fr_panel_idle(&pp);
	pp.te_misses = 3;
	pb.n = 0;
	fr_panel_frame(&pp, fb);
	CHECK(on_edge(pb.ev[1].t) && pp.te_misses == 3u);
}

/* S-13c.017's "A send that does not end": a DMA send not ended 40 ms after
 * its start is stopped and the frame sent again by the CPU, whole, with its
 * own RAMWR, paced; every later frame by the CPU too. A send that fails
 * does the same. The logo frame's, unpaced, before DISPON. */
static void test_panel_dma_stopped(void)
{
	for (unsigned failed = 0; failed < 2u; failed++) {
		unsigned i;
		uint64_t t1;

		p_up();
		fr_panel_frame(&pp, fb);
		pb.dma_us = 0;			/* never ends */
		pb.dma_error = failed != 0u;
		pb.n = 0;
		fr_panel_frame(&pp, fb);
		t1 = pb.ev[1].t;
		CHECK(pb.ev[1].kind == PE_DMA && !pp.cpu);
		if (!failed) {
			pb.now = t1 + 39000u;
			CHECK(fr_panel_busy(&pp));
			pb.now = t1 + 40000u;
		}
		CHECK(!fr_panel_busy(&pp) && pp.cpu && !pb.dma_on);
		i = p_next(2, PE_STOP);
		CHECK(i == 2u && pb.ev[i].t == (failed ? t1 : t1 + 40000u));
		/* RAMWR on an edge, then 480 rows of 448 bytes from the buffer */
		CHECK(pb.ev[3].kind == PE_CMD && pb.ev[3].cmd == 0x2Cu && on_edge(pb.ev[3].t));
		CHECK(p_cpu_rows(4, fb, 448u) && pb.n == 484u);
		/* every later frame by the CPU, paced, whole */
		pb.n = 0;
		fr_panel_frame(&pp, frame);
		CHECK(pb.ev[0].cmd == 0x2Cu && on_edge(pb.ev[0].t) && p_cpu_rows(1, frame, 448u));
		CHECK(pb.n == 481u && p_next(0, PE_DMA) == pb.n && !fr_panel_busy(&pp));
	}
	/* the logo frame's DMA stalled: the CPU's send, then DISPON */
	p_up();
	pb.dma_us = 0;
	pb.now = 5000;
	fr_panel_frame(&pp, fb);
	CHECK(pb.ev[1].kind == PE_DMA && pb.ev[2].kind == PE_STOP && pb.ev[2].t == 45000u);
	CHECK(pb.ev[3].cmd == 0x2Cu && pb.ev[3].t == 45000u && p_cpu_rows(4, fb, 448u));
	CHECK(pb.ev[484].kind == PE_CMD && pb.ev[484].cmd == 0x29u && pb.n == 485u && pp.cpu);
}

/* S-13c.022: at a power-off a frame going out is left to end, at most
 * 60 ms, then stopped; nothing sent then or after. */
static void test_panel_off(void)
{
	uint64_t t1;
	unsigned n;

	for (unsigned k = 0; k < 2u; k++) {
		p_up();
		fr_panel_frame(&pp, fb);
		pb.dma_us = k == 0u ? 15000u : 100000u;
		fr_panel_frame(&pp, fb);
		t1 = pb.now;
		pb.now += 2000u;
		n = pb.n;
		fr_panel_off(&pp);
		CHECK(pb.n == n + 1u && pb.ev[n].kind == PE_STOP && !pb.dma_on && !pp.ready);
		if (k == 0u)
			CHECK(pb.ev[n].t >= t1 + 15000u && pb.ev[n].t < t1 + 15100u);
		else
			CHECK(pb.ev[n].t >= t1 + 62000u && pb.ev[n].t < t1 + 62100u);
		fr_panel_frame(&pp, fb);
		fr_panel_line(&pp, fb);
		CHECK(!fr_panel_busy(&pp));
		fr_panel_off(&pp);
		CHECK(pb.n == n + 1u);
	}
	/* nothing going out: nothing waited, nothing sent */
	p_up();
	fr_panel_frame(&pp, fb);
	n = pb.n;
	t1 = pb.now;
	fr_panel_off(&pp);
	CHECK(pb.n == n && pb.now == t1);
}

/* S-13c.025: the logo line, paced: CASET 189-204, RASET 0-479, RAMWR, its
 * 15,360 bytes by one DMA, then the frame window's CASET at its end; by
 * the CPU, rows of 32 bytes. Not before the display is on. */
static void test_panel_line(void)
{
	static const uint8_t caset_line[4] = { 0x00, 0xBD, 0x00, 0xCC };
	static const uint8_t caset_frame[4] = { 0x00, 0x31, 0x01, 0x10 };
	static const uint8_t raset[4] = { 0x00, 0x00, 0x01, 0xDF };
	unsigned i;

	p_up();
	fr_panel_line(&pp, fb);
	CHECK(pb.n == 0u);		/* the display is off */
	fr_panel_frame(&pp, frame);
	pb.n = 0;
	fr_panel_line(&pp, fb);
	CHECK(pb.ev[0].cmd == 0x2Au && pb.ev[1].kind == PE_DATA && memcmp(pb.ev[1].p, caset_line, 4) == 0);
	CHECK(on_edge(pb.ev[0].t));
	CHECK(pb.ev[2].cmd == 0x2Bu && memcmp(pb.ev[3].p, raset, 4) == 0);
	CHECK(pb.ev[4].cmd == 0x2Cu && pb.ev[5].kind == PE_DMA && pb.ev[5].n == 15360u && pb.ev[5].p == fb);
	CHECK(pb.n == 6u && fr_panel_busy(&pp));
	fr_panel_idle(&pp);
	i = p_next(6, PE_STOP);
	CHECK(i == 6u && pb.ev[7].cmd == 0x2Au && memcmp(pb.ev[8].p, caset_frame, 4) == 0 && pb.n == 9u);
	/* by the CPU */
	pp.cpu = true;
	pb.n = 0;
	fr_panel_line(&pp, fb);
	CHECK(pb.ev[4].cmd == 0x2Cu && p_cpu_rows(5, fb, 32u));
	CHECK(pb.ev[485].cmd == 0x2Au && memcmp(pb.ev[486].p, caset_frame, 4) == 0 && pb.n == 487u);
}

/* Before the jump: the frame going out ended, both channels disabled and
 * their status cleared, so that the main image's first frame starts. */
static void test_panel_end(void)
{
	p_up();
	fr_panel_frame(&pp, fb);
	fr_panel_frame(&pp, fb);
	CHECK(pb.dma_on);
	pb.n = 0;
	fr_panel_end(&pp);
	CHECK(!pb.dma_on && pb.n >= 1u && pb.ev[pb.n - 1u].kind == PE_STOP);
	CHECK(pb.ev[pb.n - 1u].t >= pb.ev[0].t);
	/* the logo frame alone: still a stop last */
	p_up();
	fr_panel_frame(&pp, fb);
	pb.n = 0;
	fr_panel_end(&pp);
	CHECK(pb.n == 1u && pb.ev[0].kind == PE_STOP);
}

int main(void)
{
	RUN(test_rec_idle);
	RUN(test_rec_long_version);
	RUN(test_rec_refusals);
	RUN(test_rec_transfer);
	RUN(test_rec_damaged);
	RUN(test_rec_why);
	RUN(test_rec_failed_selector);
	RUN(test_rec_selector_survey);
	RUN(test_rec_one_target);
	RUN(test_rec_new_results);
	RUN(test_rec_rescue_rows);
	RUN(test_rec_held_rows);
	RUN(test_rec_trial);
	RUN(test_rec_main);
	RUN(test_rec_chips);
	RUN(test_rec_badges);
	RUN(test_rec_menu);
	RUN(test_rec_confirm);
	RUN(test_rec_countdown);
	RUN(test_rec_done);
	RUN(test_rec_fits);
	RUN(test_font_copy);
	RUN(test_text_helpers);
	RUN(test_logo);
	RUN(test_bars);
	RUN(test_rows);
	RUN(test_window);
	RUN(test_rows_match_pixels);
	RUN(test_status);
	RUN(test_bar);
	RUN(test_parts);
	RUN(test_line);
	RUN(test_panel_paced);
	RUN(test_panel_te_miss);
	RUN(test_panel_dma_stopped);
	RUN(test_panel_off);
	RUN(test_panel_line);
	RUN(test_panel_end);
	return check_summary("test_front_draw");
}
