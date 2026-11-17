/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 console, line styles, frames, key test and hand-over frame
 * (wiki/Screen-Style-Guide.md).
 * Requirements: S-15.002, S-15.003 (geometry part), S-15.004, S-15.062,
 * S-15.064, S-15.065, S-15.132, S-14.025, S-16.013.
 */
#include <stdbool.h>
#include <stdint.h>

#include "waffle/s14_log.h"
#include "waffle/s15_console.h"
#include "waffle/s15_keytest.h"
#include "waffle/s15_lines.h"
#include "s14_check.h"
#include "s15_sha256.h"
#define S15V_CONSOLE
#define S15V_KEYTEST
#define S15V_HANDOVER
#include "vectors/s15_vectors.h"

/* ---- a log that records the lines it emits -------------------------- */

static char logged[64][256];
static size_t logged_len[64];
static size_t n_logged;

static bool rec_send(void *ctx, uint8_t route, uint8_t id,
		     const uint8_t *payload, size_t len)
{
	size_t tl = s14_tag_len(payload + 4, len - 4u);

	(void)ctx;
	(void)route;
	CHECK(id == S14_ID_LOG, "S-15.004 logged at level 3 (id 0x03)");
	if (n_logged < 64) {
		logged_len[n_logged] = len - 4u - tl;
		memcpy(logged[n_logged], payload + 4 + tl, len - 4u - tl);
		n_logged++;
	}
	return true;
}

static const struct s14_link_ops rec_link = { rec_send, NULL };

static bool logged_is(size_t i, const char *want)
{
	return i < n_logged &&
	       mem_eq(logged[i], logged_len[i], want, strlen(want));
}

/* ---- a framebuffer that the draw ops paint ------------------------- */

#define FB_W 480
#define FB_H 240
static uint16_t fb[FB_H][FB_W];
/* Text calls, recorded with their position and colors. */
#define MAX_TEXTS 128
static struct {
	uint16_t x, y, fg, bg;
	char s[96];
} texts[MAX_TEXTS];
static size_t n_texts;
/* Fill calls. */
#define MAX_FILLS 512
static struct {
	uint16_t x, y, w, h, c;
} fills[MAX_FILLS];
static size_t n_fills;
static bool out_of_bounds;

static void fb_fill(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
		    uint16_t c)
{
	(void)ctx;
	if (n_fills < MAX_FILLS) {
		fills[n_fills].x = x;
		fills[n_fills].y = y;
		fills[n_fills].w = w;
		fills[n_fills].h = h;
		fills[n_fills].c = c;
		n_fills++;
	}
	if (x + w > FB_W || y + h > FB_H) {
		out_of_bounds = true;
		return;
	}
	for (unsigned r = y; r < (unsigned)(y + h); r++)
		for (unsigned k = x; k < (unsigned)(x + w); k++)
			fb[r][k] = c;
}

static void fb_text(void *ctx, uint16_t x, uint16_t y, const char *s, size_t n,
		    uint16_t fg, uint16_t bg)
{
	(void)ctx;
	CHECK(n > 0u && n < sizeof texts[0].s, "a text call of %zu glyphs", n);
	if (x + n * 8u > FB_W || y + 16u > FB_H)
		out_of_bounds = true;
	if (n_texts < MAX_TEXTS && n < sizeof texts[0].s) {
		texts[n_texts].x = x;
		texts[n_texts].y = y;
		texts[n_texts].fg = fg;
		texts[n_texts].bg = bg;
		memcpy(texts[n_texts].s, s, n);
		texts[n_texts].s[n] = '\0';
		n_texts++;
	}
}

/* The bars' text over their texture: recorded as a text on CLEAR, a
 * color no frame draws. */
#define CLEAR 0x0001u
/* The title bar's text: recorded on BOLD, another such color. */
#define BOLD 0x0002u

static void fb_ink(void *ctx, uint16_t x, uint16_t y, const char *s, size_t n,
		   uint16_t fg)
{
	fb_text(ctx, x, y, s, n, fg, CLEAR);
}

static void fb_bold(void *ctx, uint16_t x, uint16_t y, const char *s, size_t n,
		    uint16_t fg)
{
	fb_text(ctx, x, y, s, n, fg, BOLD);
}

static const struct s15_draw_ops draw = { fb_fill, fb_text, NULL, fb_ink,
					  fb_bold };
/* Without the ink and bold ops: the bars' text on the bar's base color. */
static const struct s15_draw_ops plain = { fb_fill, fb_text, NULL, NULL, NULL };

/* The board's frame (src/include/waffle/display.h): 8 x 16 glyphs on the
 * 480 x 222 visible area, the title bar y 0 - 23, 9 rows 19 apart from
 * y 27, x from 16, the status bar's rule at y 202. */
static const struct s15_geometry geom = {
	480, 222, 8, 16, 27, 19, 8, 16, 24, 202, 10, 17, 4,
};

static void reset_frame(void)
{
	memset(fb, 0xaa, sizeof fb);
	n_texts = 0;
	n_fills = 0;
	out_of_bounds = false;
}

/* The recorded text at (x, y), or NULL. */
static const char *text_at(unsigned x, unsigned y, uint16_t *fg, uint16_t *bg)
{
	for (size_t i = 0; i < n_texts; i++)
		if (texts[i].x == x && texts[i].y == y) {
			if (fg != NULL)
				*fg = texts[i].fg;
			if (bg != NULL)
				*bg = texts[i].bg;
			return texts[i].s;
		}
	return NULL;
}

/* Whether s is drawn at (x, y) in fg on bg. */
static bool drawn(unsigned x, unsigned y, const char *s, uint16_t fg,
		  uint16_t bg)
{
	uint16_t f, b;
	const char *t = text_at(x, y, &f, &b);

	return t != NULL && strcmp(t, s) == 0 && f == fg && b == bg;
}

/* Body row column c's x. */
static unsigned col(unsigned c)
{
	return geom.margin + c * geom.glyph_w;
}

static enum s15_style style_of(const char *name)
{
	for (unsigned i = 0; i < S15_ST_COUNT; i++)
		if (strcmp(s15_style_names[i], name) == 0)
			return (enum s15_style)i;
	CHECK(0, "unknown style %s", name);
	return S15_ST_TEXT;
}

/* ---- tests ----------------------------------------------------------- */

/* The style names the vectors write lines with. */
static void test_style_names(void)
{
	static const char *const want[S15_ST_COUNT] = {
		"text", "dim", "head", "keys", "item", "item-on", "armed",
		"back", "ok", "warn", "error", "kv",
		"step-run", "step-ok", "step-skip", "step-fail",
	};

	for (unsigned i = 0; i < S15_ST_COUNT; i++)
		CHECK(s15_style_names[i] != NULL &&
		      strcmp(s15_style_names[i], want[i]) == 0,
		      "style %u named %s", i, want[i]);
}

/* A shown row as the vectors write it: "style:text", or the text alone
 * in the text style. */
static void row_repr(const struct s15_console *con, unsigned i, char *out,
		     size_t cap)
{
	size_t n;
	const char *s = s15_console_row(con, i, &n);
	enum s15_style st = s15_console_row_style(con, i);

	if (st == S15_ST_TEXT)
		snprintf(out, cap, "%.*s", (int)n, s);
	else
		snprintf(out, cap, "%s:%.*s", s15_style_names[st], (int)n, s);
}

static void shown(const struct s15_console *con, const char *const *want,
		  const char *name)
{
	size_t i;

	for (i = 0; want[i] != NULL; i++) {
		char got[S15_LINE + 16];

		row_repr(con, (unsigned)i, got, sizeof got);
		CHECK(strcmp(got, want[i]) == 0,
		      "%s row %zu: '%s' want '%s'", name, i, got,
		      want[i]);
	}
	CHECK(i == con->count, "%s: %u rows, want %zu", name,
	      con->count, i);
}

static void test_console_vectors(void)
{
	size_t c, k;

	for (c = 0; c < S15V_N(s15v_console); c++) {
		const struct s15v_console *v = &s15v_console[c];
		struct s15_console con;
		char name[16];

		s15_console_init(&con, NULL);
		for (k = 0; k < v->n_ops; k++) {
			const struct s15v_op *op = &v->ops[k];
			enum s15_style st = style_of(op->style);

			switch (op->kind) {
			case S15V_APPEND:
				if (st == S15_ST_TEXT)
					s15_console_append_raw(&con,
						(const uint8_t *)op->text,
						op->len);
				else
					s15_console_add(&con, st, op->text);
				break;
			case S15V_REPLACE:
				s15_console_set(&con, st, op->text);
				break;
			case S15V_CLEAR:
				s15_console_clear(&con);
				break;
			case S15V_APPEND_N:
			default:
				for (unsigned i = 1; i <= op->n; i++) {
					snprintf(name, sizeof name, "L%u", i);
					s15_console_append(&con, name);
				}
				break;
			}
		}
		shown(&con, v->shown, v->name);
	}
}

/* A styled line keeps its tabs, a text line shows '?' for one;
 * set replaces the last line's text and style. */
static void test_styled(void)
{
	struct s15_console con;
	size_t n;
	const char *s;

	s15_console_init(&con, NULL);
	CHECK(s15_console_row_style(&con, 0) == S15_ST_TEXT,
	      "an empty row is text");
	s15_console_add(&con, S15_ST_KV, "Unit\tAB12");
	s15_console_append(&con, "a\tb");
	s = s15_console_row(&con, 0, &n);
	CHECK(mem_eq(s, n, "Unit\tAB12", 9) &&
	      s15_console_row_style(&con, 0) == S15_ST_KV,
	      "a kv row keeps its tab");
	s = s15_console_row(&con, 1, &n);
	CHECK(mem_eq(s, n, "a?b", 3) &&
	      s15_console_row_style(&con, 1) == S15_ST_TEXT,
	      "a text row shows a tab as ?");
	s15_console_set(&con, S15_ST_STEP_RUN, "BLE\t5%");
	s = s15_console_row(&con, 1, &n);
	CHECK(con.count == 2u && mem_eq(s, n, "BLE\t5%", 6) &&
	      s15_console_row_style(&con, 1) == S15_ST_STEP_RUN,
	      "set replaces the last row and its style");
	s15_console_set(&con, S15_ST_TEXT, "plain");
	CHECK(s15_console_row_style(&con, 1) == S15_ST_TEXT &&
	      s15_console_row_style(&con, 0) == S15_ST_KV,
	      "set in the text style, row 0 untouched");
	/* An out-of-range style is the text style. */
	s15_console_add(&con, (enum s15_style)S15_ST_COUNT, "x\ty");
	s = s15_console_row(&con, 2, &n);
	CHECK(s15_console_row_style(&con, 2) == S15_ST_TEXT &&
	      mem_eq(s, n, "x?y", 3), "unknown style is text");
	/* Past the 9th line the oldest leaves with its style. */
	for (unsigned i = 0; i < S15_ROWS; i++)
		s15_console_add(&con, S15_ST_OK, "ok");
	CHECK(con.count == S15_ROWS &&
	      s15_console_row_style(&con, 0) == S15_ST_OK &&
	      s15_console_row_style(&con, S15_ROWS - 1u) == S15_ST_OK,
	      "styles scroll with their lines");
	CHECK(s15_console_row_style(&con, S15_ROWS) == S15_ST_TEXT,
	      "a row past the last is text");
}

/* S-15.004: appended lines are logged with their full text, backquotes
 * dropped in the keys style and tabs as two spaces; replacements are
 * not. S-14.025: the screen sink's lines are not logged again. */
static void test_logging(void)
{
	static const char long_line[] =
		"0123456789012345678901234567890123456789"
		"0123456789012345678901234567890123456789xyz";
	struct s14_log log;
	struct s15_console con;
	struct s14_screen_ops scr = { s15_console_sink, &con };
	size_t n;

	n_logged = 0;
	s14_log_init(&log, S14_MASTER, NULL, &rec_link, NULL, &scr);
	s15_console_init(&con, &log);
	s15_console_append(&con, "");
	s15_console_append(&con, long_line);
	s15_console_set(&con, S15_ST_TEXT, "Main DSP 5%");
	s15_console_set(&con, S15_ST_STEP_RUN, "Main DSP\t5%");
	CHECK(n_logged == 2u, "S-15.004 two appended lines logged, not the "
	      "replacements (%zu)", n_logged);
	CHECK(logged_len[0] == 0u, "S-15.004 a blank line is logged");
	CHECK(logged_is(1, long_line), "S-15.004 full text, before clipping");
	s15_console_append(&con, long_line);
	(void)s15_console_row(&con, 2, &n);
	CHECK(n == S15_LINE, "kept to %u characters on screen (%zu)",
	      S15_LINE, n);

	/* Styled lines: the text as read, not as drawn. */
	n_logged = 0;
	s15_console_add(&con, S15_ST_KEYS,
			"`Hold 2` for 5 s to turn it on, `any other key` cancels");
	s15_console_add(&con, S15_ST_KV, "Unit\tAB123456\tFirmware\t1.2.0");
	s15_console_add(&con, S15_ST_STEP_OK, "BLE\tInstalled 1.0.2");
	s15_console_add(&con, S15_ST_HEAD, "a `quoted` head");
	s15_console_add(&con, S15_ST_OK, "Done!");
	CHECK(n_logged == 5u, "S-15.004 each styled line logged (%zu)",
	      n_logged);
	CHECK(logged_is(0, "Hold 2 for 5 s to turn it on, any other key cancels"),
	      "S-15.004 keys: backquotes dropped: '%.*s'",
	      (int)logged_len[0], logged[0]);
	CHECK(logged_is(1, "Unit  AB123456  Firmware  1.2.0"),
	      "S-15.004 kv: each tab two spaces: '%.*s'",
	      (int)logged_len[1], logged[1]);
	CHECK(logged_is(2, "BLE  Installed 1.0.2"),
	      "S-15.004 step: the tab two spaces");
	CHECK(logged_is(3, "a `quoted` head"),
	      "S-15.004 backquotes kept outside the keys style");
	CHECK(logged_is(4, "Done!"), "S-15.004 no mark logged");
	{
		const char *s = s15_console_row(&con, con.count - 5u, &n);

		CHECK(n > 0u && s[0] == '`',
		      "the keys row keeps its backquotes");
	}

	s14_log_screen_start(&log);
	n_logged = 0;
	s14_log_str(&log, S14_FATAL, S14_TAG("t"), "fatal body");
	CHECK(n_logged == 1u, "S-14.025 the level-0 line is logged once");
	{
		const char *s = s15_console_row(&con, con.count - 1u, &n);

		CHECK(n > 10u && memcmp(s, "t[", 2) == 0 &&
		      memcmp(s + n - 10, "fatal body", 10) == 0 &&
		      s15_console_row_style(&con, con.count - 1u) == S15_ST_TEXT,
		      "S-14.025 the body (tag and text) on the screen");
	}
}

static bool all_black(void)
{
	for (unsigned r = 0; r < geom.height; r++)
		for (unsigned c = 0; c < geom.width; c++)
			if (fb[r][c] != S15_BLACK)
				return false;
	return true;
}

/* Every recorded text lies in the title bar, a body row or the status
 * bar's text row, and nothing overlaps a bar. */
static void texts_placed(const char *what)
{
	for (size_t i = 0; i < n_texts; i++) {
		unsigned y = texts[i].y;
		bool title = y + geom.glyph_h <= geom.bar_h;
		bool status = y == s15_status_text_y(&geom);
		bool body = y >= geom.top && (y - geom.top) % geom.pitch == 0u &&
			    (y - geom.top) / geom.pitch < S15_ROWS;

		CHECK(title || status || body, "%s: text '%s' at y %u", what,
		      texts[i].s, y);
		if (body)
			CHECK(y >= geom.bar_h && y + geom.glyph_h <= geom.status_y &&
			      texts[i].x >= geom.margin &&
			      texts[i].x + strlen(texts[i].s) * geom.glyph_w <=
			      geom.margin + S15_COLS * geom.glyph_w,
			      "%s: body text '%s' clear of the bars", what,
			      texts[i].s);
	}
	CHECK(!out_of_bounds, "%s: within the visible area", what);
}

/* S-15.002, S-15.003: the geometry. */
static void test_geometry(void)
{
	struct s15_geometry g;

	CHECK(s15_geometry_ok(&geom), "S-15.002 the board's geometry fits");
	CHECK(s15_row_y(&geom, 0) == 27u && s15_row_y(&geom, 8) == 179u,
	      "S-15.002 rows from y 27, 19 apart");
	CHECK(s15_status_text_y(&geom) == 205u, "status text at 205");
	g = geom;
	g.status_y = s15_row_y(&geom, 8);
	CHECK(!s15_geometry_ok(&g), "status bar on a body row");
	g = geom;
	g.glyph_w = 9;
	CHECK(!s15_geometry_ok(&g), "56 glyphs of 9 too wide");
	g = geom;
	g.pitch = 15;
	CHECK(!s15_geometry_ok(&g), "pitch below the glyph height");
	g = geom;
	g.top = 19;
	CHECK(!s15_geometry_ok(&g), "row 0 in the title bar");
	g = geom;
	g.bar_h = 17;
	CHECK(!s15_geometry_ok(&g), "title bar lower than its text");
	g = geom;
	g.status_y = 210;
	CHECK(!s15_geometry_ok(&g), "status text below the visible area");
	g = geom;
	g.glyph_h = 0;
	CHECK(!s15_geometry_ok(&g), "no glyph height");
}

/* Whether every pixel of x0 to x1 - 1, y0 to y1 - 1 is c. */
static bool box_is(unsigned x0, unsigned y0, unsigned x1, unsigned y1,
		   uint16_t c)
{
	for (unsigned r = y0; r < y1; r++)
		for (unsigned k = x0; k < x1; k++)
			if (fb[r][k] != c)
				return false;
	return true;
}

/* The title bar's pixel as drawn: bg, with the squares of 14 at x 392,
 * 414, 436 and 458, y 5 - 18, in dark: the first filled, the others a
 * 2-pixel outline around bg. */
static uint16_t mark_pixel(unsigned x, unsigned y, uint16_t bg, uint16_t dark)
{
	for (unsigned i = 0; i < 4u; i++) {
		unsigned x0 = 392u + 22u * i;

		if (x < x0 || x >= x0 + 14u || y < 5u || y >= 19u)
			continue;
		if (i == 0u)
			return dark;
		if (x < x0 + 2u || x >= x0 + 12u || y < 7u || y >= 17u)
			return dark;
		return bg;
	}
	return bg;
}

/* The title bar: solid bg from side to side, y 0 - 23, with the squares
 * in dark; nothing drawn at y 24, the status bar one color. */
static bool title_bar_is(uint16_t bg, uint16_t dark, uint16_t status)
{
	CHECK(S15_MARK_X(&geom) == 392u && S15_TEXT_W(&geom) == 383u,
	      "the squares from x 392, the text part to x 382");
	for (unsigned y = 0; y < 24u; y++)
		for (unsigned x = 0; x < geom.width; x++)
			if (fb[y][x] != mark_pixel(x, y, bg, dark))
				return false;
	return box_is(0, 24, geom.width, 25, 0xaaaau) &&
	       box_is(0, geom.status_y + 1u, geom.width, geom.height, status);
}

/* The title bar and the status bar. */
static void test_render_frame(void)
{
	struct s15_frame f = { "Main Menu", "1.0.0", 0, S15_SLOT_UNKNOWN, false,
			       false, false };
	uint16_t ty = geom.title_y;
	uint16_t sy = s15_status_text_y(&geom);
	/* "waffle-boot v1.0.0" ends at width - edge */
	unsigned vx = geom.width - geom.edge - 18u * geom.glyph_w;
	/* the indicator ends 8 pixels before the text part does */
	unsigned ix = 383u - geom.edge -
		      (unsigned)strlen(s15_indicator_text) * geom.title_w;

	reset_frame();
	s15_render_frame(&f, &geom, &draw);
	CHECK(title_bar_is(S15_AMBER, S15_AMBER_DARK, S15_STATUS_BG),
	      "a solid amber bar side to side, the squares, no line under "
	      "it, the status bar solid");
	CHECK(fb[5][392] == S15_AMBER_DARK && fb[18][405] == S15_AMBER_DARK &&
	      fb[12][399] == S15_AMBER_DARK &&
	      fb[5][414] == S15_AMBER_DARK && fb[6][415] == S15_AMBER_DARK &&
	      fb[7][416] == S15_AMBER && fb[16][425] == S15_AMBER &&
	      fb[17][425] == S15_AMBER_DARK && fb[18][471] == S15_AMBER_DARK &&
	      fb[4][392] == S15_AMBER && fb[19][392] == S15_AMBER &&
	      fb[12][472] == S15_AMBER && fb[12][383] == S15_AMBER &&
	      fb[12][479] == S15_AMBER,
	      "the first square filled, the others 2-pixel outlines, the "
	      "bar's color between and inside");
	CHECK(S15_STATUS_BG == 0x2124u && s15_gray(S15_STATUS_BG) == S15_STATUS_BG,
	      "the status bar 212421, already gray");
	reset_frame();
	s15_render_frame(&f, &geom, &plain);
	CHECK(drawn(geom.edge, ty, "Main Menu", S15_BLACK, S15_AMBER) &&
	      drawn(8, sy, "SLOT", S15_UNLIT, S15_BLACK),
	      "without ink, the bars' text on their base colors");
	reset_frame();
	s15_render_frame(&f, &geom, &draw);
	CHECK(drawn(geom.edge, ty, "Main Menu", S15_BLACK, BOLD),
	      "the title, bold, black on amber at x %u", geom.edge);
	CHECK(fb[geom.status_y][0] == S15_STATUS_BG &&
	      fb[geom.status_y][geom.width - 1u] == S15_STATUS_BG &&
	      fb[geom.status_y - 1u][0] == 0xaaaau,
	      "the status bar from y %u, with no rule above it", geom.status_y);
	CHECK(drawn(8, sy, "SLOT", S15_UNLIT, CLEAR) &&
	      drawn(56, sy, "USB", S15_UNLIT, CLEAR),
	      "the status words unlit, two spaces apart");
	/* The running slot by its number, gray; USB moves one column right. */
	f.slot = 1;
	reset_frame();
	s15_render_frame(&f, &geom, &draw);
	CHECK(drawn(8, sy, "SLOT2", S15_DIM, CLEAR) &&
	      drawn(64, sy, "USB", S15_UNLIT, CLEAR),
	      "the running slot, gray");
	f.slot = S15_SLOT_UNKNOWN;
	reset_frame();
	s15_render_frame(&f, &geom, &draw);
	CHECK(drawn(vx, sy, "waffle-boot", S15_DIM, CLEAR) &&
	      drawn(vx + 12u * geom.glyph_w, sy, "v", S15_FAINT, CLEAR) &&
	      drawn(vx + 13u * geom.glyph_w, sy, "1.0.0", S15_FAINT, CLEAR),
	      "waffle-boot v1.0.0 ending at x %u",
	      (unsigned)(geom.width - geom.edge));
	CHECK(text_at(ix, ty, NULL, NULL) == NULL,
	      "no indicator with the option off");
	texts_placed("frame");

	/* USB lit amber; the slot stays gray. */
	f.status = S15_STATUS_USB;
	f.slot = 1;
	reset_frame();
	s15_render_frame(&f, &geom, &draw);
	CHECK(drawn(8, sy, "SLOT2", S15_DIM, CLEAR) &&
	      drawn(64, sy, "USB", S15_AMBER, CLEAR),
	      "USB amber, index 1 shown as SLOT2");
	f.status = 0;
	f.slot = 0;
	reset_frame();
	s15_render_frame(&f, &geom, &draw);
	CHECK(drawn(8, sy, "SLOT1", S15_DIM, CLEAR) &&
	      drawn(64, sy, "USB", S15_UNLIT, CLEAR),
	      "index 0 shown as SLOT1");

	/* Protected writes on: the bar red, the name white, the indicator
	 * at the text part's right end, clear of the squares. */
	f.status = 0;
	f.option_on = true;
	reset_frame();
	s15_render_frame(&f, &geom, &draw);
	CHECK(title_bar_is(S15_RED, S15_RED_DARK, S15_STATUS_BG),
	      "a solid red bar, dark red squares, no line under it");
	CHECK(drawn(geom.edge, ty, "Main Menu", S15_WHITE, BOLD),
	      "the title white on red");
	CHECK(ix == 185u && drawn(ix, ty, s15_indicator_text, S15_WHITE, BOLD) &&
	      strcmp(s15_indicator_text, "Protected writes on") == 0,
	      "the indicator ending at x 374, 8 pixels inside the bar");
	/* the longest title still ends before the indicator starts */
	CHECK(geom.edge + strlen("MTP File Transfer") * geom.title_w <= ix,
	      "the longest title clear of the indicator");
	CHECK(fb[geom.status_y][0] == S15_STATUS_BG, "the status bar stays gray");
	texts_placed("option frame");
	reset_frame();
	s15_render_frame(&f, &geom, &plain);
	CHECK(drawn(ix, ty, s15_indicator_text, S15_WHITE, S15_RED),
	      "without ink, the indicator on red");

	/* A fatal error: red, its own name, no indicator. */
	f.option_on = false;
	f.fatal = true;
	f.title = "Fatal Error";
	reset_frame();
	s15_render_frame(&f, &geom, &draw);
	CHECK(title_bar_is(S15_RED, S15_RED_DARK, S15_STATUS_BG) &&
	      drawn(geom.edge, ty, "Fatal Error", S15_WHITE, BOLD),
	      "a red bar named Fatal Error");
	CHECK(text_at(ix, ty, NULL, NULL) == NULL &&
	      fb[geom.status_y][0] == S15_STATUS_BG,
	      "no indicator, a gray status bar");

	/* S-15.173: color off, every pixel its gray; the squares on amber
	 * near 606060. */
	{
		static const bool reds[3][2] = { { false, false },
						  { true, false },
						  { false, true } };

		for (unsigned k = 0; k < 3u; k++) {
			bool gray = true;

			f.option_on = reds[k][0];
			f.fatal = reds[k][1];
			f.gray = true;
			reset_frame();
			s15_render_frame(&f, &geom, &draw);
			for (unsigned y = 0; y < geom.height; y++)
				for (unsigned x = 0; x < geom.width; x++)
					if (fb[y][x] != 0xaaaau &&
					    s15_gray(fb[y][x]) != fb[y][x])
						gray = false;
			CHECK(gray && title_bar_is(
				      s15_gray(k == 0u ? S15_AMBER : S15_RED),
				      s15_gray(k == 0u ? S15_AMBER_DARK
						       : S15_RED_DARK),
				      S15_STATUS_BG),
			      "color off, bar %u: every pixel gray, the same "
			      "layout", k);
		}
		{
			unsigned g8 = (unsigned)(s15_gray(S15_AMBER_DARK) >> 5 &
						 0x3fu) * 4u;

			CHECK(g8 >= 88u && g8 <= 100u,
			      "the squares' gray near 96 (%u)", g8);
		}
		f.gray = false;
		f.fatal = false;
		f.option_on = false;
	}

	/* No title and no version: the bar and waffle-boot alone. */
	f.title = NULL;
	f.version = NULL;
	reset_frame();
	s15_render_frame(&f, &geom, &draw);
	CHECK(text_at(geom.edge, ty, NULL, NULL) == NULL &&
	      drawn(geom.width - geom.edge - 11u * geom.glyph_w, sy,
		    "waffle-boot", S15_DIM, CLEAR),
	      "no title, no version");
}

/* Renders one row in a fresh frame. */
static void row(unsigned r, enum s15_style st, const char *s)
{
	reset_frame();
	memset(fb, 0, sizeof fb);
	s15_render_row(&geom, &draw, r, st, s, strlen(s));
}

/* Whether a fill of color c covers pixel (x, y). */
static bool filled(unsigned x, unsigned y, uint16_t c)
{
	return fb[y][x] == c;
}

/* Each style's columns and colors. */
static void test_render_row(void)
{
	unsigned y = s15_row_y(&geom, 3);
	uint16_t B = S15_BLACK;

	row(3, S15_ST_TEXT, "Connect the OP-XY");
	CHECK(n_texts == 1u &&
	      drawn(col(0), y, "Connect the OP-XY", S15_TEXT, B),
	      "text: white from column 0 on row 3's y");
	row(3, S15_ST_DIM, "Waiting");
	CHECK(n_texts == 1u && drawn(col(0), y, "Waiting", S15_DIM, B),
	      "dim");
	row(3, S15_ST_HEAD, "Upgrade");
	CHECK(n_texts == 1u && drawn(col(0), y, "Upgrade", S15_AMBER, B),
	      "head");

	/* keys: gray, the backquoted spans amber, the backquotes not
	 * drawn */
	row(3, S15_ST_KEYS, "`Shift` + `4` skips");
	CHECK(n_texts == 4u && drawn(col(0), y, "Shift", S15_AMBER, B) &&
	      drawn(col(5), y, " + ", S15_DIM, B) &&
	      drawn(col(8), y, "4", S15_AMBER, B) &&
	      drawn(col(9), y, " skips", S15_DIM, B),
	      "keys spans");

	/* item: the digit in column 0, the label from 3, the hint
	 * right-aligned to column 55 */
	row(3, S15_ST_ITEM, "7 Factory reset\tRemoves your data only");
	CHECK(n_texts == 3u && drawn(col(0), y, "7", S15_AMBER, B) &&
	      drawn(col(3), y, "Factory reset", S15_TEXT, B) &&
	      drawn(col(S15_COLS - 22u), y, "Removes your data only", S15_DIM, B),
	      "item");
	row(3, S15_ST_ITEM, "5 System");
	CHECK(n_texts == 2u && drawn(col(0), y, "5", S15_AMBER, B) &&
	      drawn(col(3), y, "System", S15_TEXT, B), "item, no hint");
	row(3, S15_ST_ITEM_ON, "6 Protected writes\tOn");
	CHECK(drawn(col(0), y, "6", S15_AMBER, B) &&
	      drawn(col(3), y, "Protected writes", S15_TEXT, B) &&
	      drawn(col(S15_COLS - 2u), y, "On", S15_RED, B),
	      "item-on: the hint red");
	row(3, S15_ST_ARMED, "7 Factory reset\tHold 7 for 5 s");
	CHECK(drawn(col(0), y, "7", S15_RED, B) &&
	      drawn(col(3), y, "Factory reset", S15_RED, B) &&
	      drawn(col(S15_COLS - 14u), y, "Hold 7 for 5 s", S15_RED, B),
	      "armed: the whole row red");
	row(3, S15_ST_BACK, "8 Back to Main Menu");
	CHECK(n_texts == 2u && drawn(col(0), y, "8", S15_AMBER, B) &&
	      drawn(col(3), y, "Back to Main Menu", S15_DIM, B),
	      "back: the label gray");

	/* marked sentences */
	row(3, S15_ST_OK, "Done!");
	CHECK(n_texts == 2u && drawn(col(0), y, "+", S15_GREEN, B) &&
	      drawn(col(3), y, "Done!", S15_GREEN, B), "ok");
	row(3, S15_ST_WARN, "Don't turn off");
	CHECK(drawn(col(0), y, "!", S15_AMBER, B) &&
	      drawn(col(3), y, "Don't turn off", S15_AMBER, B), "warn");
	row(3, S15_ST_ERROR, "Operation 6 failed");
	CHECK(drawn(col(0), y, "!", S15_RED, B) &&
	      drawn(col(3), y, "Operation 6 failed", S15_RED, B),
	      "error");

	/* kv: keys gray at 0 and 32, values white at 14 and 43, nothing
	 * between the halves */
	row(3, S15_ST_KV, "Unit\tAB123456\tFirmware\t1.2.0");
	CHECK(n_texts == 4u && drawn(col(0), y, "Unit", S15_DIM, B) &&
	      drawn(col(14), y, "AB123456", S15_TEXT, B) &&
	      drawn(col(32), y, "Firmware", S15_DIM, B) &&
	      drawn(col(43), y, "1.2.0", S15_TEXT, B), "kv");
	{
		bool mark = false;

		for (size_t i = 0; i < n_fills; i++)
			if (fills[i].c == S15_RULE && fills[i].y == y)
				mark = true;
		CHECK(!mark, "kv: no divider between the halves");
	}
	row(3, S15_ST_KV, "DSP\t00112233445566778899\tBlackfin\t1.2.0");
	CHECK(drawn(col(14), y, "00112233445566778", S15_TEXT, B),
	      "kv: the left value stops before the right half");
	row(3, S15_ST_KV, "\t\tBLE\t1.0.1");
	CHECK(n_texts == 2u && drawn(col(32), y, "BLE", S15_DIM, B) &&
	      drawn(col(43), y, "1.0.1", S15_TEXT, B),
	      "kv: a right half alone");
	row(3, S15_ST_KV, "Boot\tSlot 1 unconfirmed");
	CHECK(drawn(col(14), y, "Slot 1 unconfirmed", S15_TEXT, B),
	      "kv: a value with no right half is whole");
	row(3, S15_ST_KV, "Boot\tNormal");
	CHECK(n_texts == 2u && drawn(col(0), y, "Boot", S15_DIM, B) &&
	      drawn(col(14), y, "Normal", S15_TEXT, B), "kv, one pair");

	/* step rows: the mark, the label from 3, the status from 19 */
	row(3, S15_ST_STEP_OK, "BLE\tInstalled 1.0.2");
	CHECK(n_texts == 3u && drawn(col(0), y, "+", S15_GREEN, B) &&
	      drawn(col(3), y, "BLE", S15_TEXT, B) &&
	      drawn(col(S15_STEP_COL), y, "Installed 1.0.2", S15_GREEN, B),
	      "step-ok");
	row(3, S15_ST_STEP_SKIP, "BLE\tNot included");
	CHECK(drawn(col(0), y, "-", S15_DIM, B) &&
	      drawn(col(3), y, "BLE", S15_TEXT, B) &&
	      drawn(col(S15_STEP_COL), y, "Not included", S15_DIM, B),
	      "step-skip");
	row(3, S15_ST_STEP_FAIL, "Main DSP\tFailed");
	CHECK(drawn(col(0), y, "x", S15_RED, B) &&
	      drawn(col(3), y, "Main DSP", S15_TEXT, B) &&
	      drawn(col(S15_STEP_COL), y, "Failed", S15_RED, B), "step-fail");
	row(3, S15_ST_STEP_RUN, "Backplane\tInstalling -");
	CHECK(n_texts == 3u && n_fills == 0u &&
	      drawn(col(0), y, ">", S15_AMBER, B) &&
	      drawn(col(3), y, "Backplane", S15_TEXT, B) &&
	      drawn(col(S15_STEP_COL), y, "Installing -", S15_AMBER, B),
	      "step-run, a status other than a percentage");
	row(3, S15_ST_STEP_RUN, "Content");
	CHECK(n_texts == 2u && n_fills == 0u, "step-run, no status");

	/* step-run with a percentage: a progress bar of 25 columns from
	 * the column after the longest label a bar's row has, the
	 * percentage after it */
	{
		const unsigned bc = S15_STEP_COL;
		unsigned bx = col(bc), bw = 25u * geom.glyph_w;
		unsigned inner = (bw - 4u) * 64u / 100u;
		bool inside = true;

		row(3, S15_ST_STEP_RUN, "Main DSP\t64%");
		CHECK(n_texts == 3u && drawn(col(0), y, ">", S15_AMBER, B) &&
		      drawn(col(3), y, "Main DSP", S15_TEXT, B) &&
		      drawn(col(bc + 25u + 1u), y, "64%", S15_AMBER, B),
		      "step-run: the percentage after the bar");
		for (size_t i = 0; i < n_fills; i++)
			if (fills[i].x < bx || fills[i].x + fills[i].w > bx + bw ||
			    fills[i].y < y || fills[i].y + fills[i].h > y + geom.glyph_h ||
			    fills[i].c != S15_AMBER)
				inside = false;
		CHECK(n_fills >= 2u && inside,
		      "the bar amber, within its 25 columns of the row");
		/* the outline's corners and the filled part */
		CHECK(filled(bx, y + 3u, S15_AMBER) &&
		      filled(bx + bw - 1u, y + 3u, S15_AMBER) &&
		      filled(bx + bw - 1u, y + geom.glyph_h - 4u, S15_AMBER),
		      "the bar's outline");
		CHECK(filled(bx + 2u, y + 8u, S15_AMBER) &&
		      filled(bx + 2u + inner - 1u, y + 8u, S15_AMBER) &&
		      filled(bx + 2u + inner, y + 8u, S15_BLACK) &&
		      filled(bx + bw - 3u, y + 8u, S15_BLACK),
		      "64%% of the bar filled");
		row(3, S15_ST_STEP_RUN, "Content\t100%");
		CHECK(filled(bx + bw - 3u, y + 8u, S15_AMBER) &&
		      drawn(col(bc + 26u), y, "100%", S15_AMBER, B),
		      "100%% fills the bar");
		row(3, S15_ST_STEP_RUN, "Main DSP\t0%");
		CHECK(filled(bx + 2u, y + 8u, S15_BLACK) &&
		      filled(bx, y + 8u, S15_AMBER) &&
		      drawn(col(bc + 26u), y, "0%", S15_AMBER, B),
		      "0%%: the outline alone");
		/* a third part: the bar from the percentage, the third part
		 * after it in the percentage's place */
		inner = (bw - 4u) * 40u / 100u;
		row(3, S15_ST_STEP_RUN, "Hold 1\t40%\t52 s");
		CHECK(n_texts == 3u && drawn(col(3), y, "Hold 1", S15_TEXT, B) &&
		      drawn(col(bc + 26u), y, "52 s", S15_AMBER, B),
		      "step-run, three parts: the third after the bar");
		CHECK(filled(bx + 2u + inner - 1u, y + 8u, S15_AMBER) &&
		      filled(bx + 2u + inner, y + 8u, S15_BLACK),
		      "three parts: 40%% of the bar filled");
		for (size_t i = 0; i < n_texts; i++)
			CHECK(strstr(texts[i].s, "%") == NULL,
			      "three parts: no percentage drawn");
	}

	/* clipped to the body's 56 columns */
	row(8, S15_ST_TEXT,
	    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
	CHECK(n_texts == 1u && strlen(texts[0].s) == S15_COLS &&
	      texts[0].y == s15_row_y(&geom, 8), "56 columns shown");
	row(8, S15_ST_ITEM, "1 xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
	CHECK(n_texts == 2u && strlen(texts[1].s) == S15_COLS - 3u,
	      "an item's label clipped at column 55");
	texts_placed("rows");
}

/* S-15.002: the console in its frame. */
static void test_frames(void)
{
	struct s15_console con;
	struct s15_frame f = { "Main Menu", "1.0.0", S15_STATUS_USB,
			       S15_SLOT_UNKNOWN, false, false, false };
	unsigned i;
	char name[8];

	s15_console_init(&con, NULL);
	for (i = 1; i <= 11; i++) {
		snprintf(name, sizeof name, "L%u", i);
		s15_console_append(&con, name);
	}
	/* S-15.002: the console as it stands; 9 rows, newest at the bottom. */
	reset_frame();
	s15_render_console(&con, &geom, &draw, &f);
	CHECK(fills[0].x == 0 && fills[0].y == 0 && fills[0].w == geom.width &&
	      fills[0].h == geom.height && fills[0].c == S15_BLACK,
	      "S-15.002 black first");
	for (i = 0; i < S15_ROWS; i++) {
		snprintf(name, sizeof name, "L%u", i + 3u);
		CHECK(drawn(geom.margin, s15_row_y(&geom, i), name, S15_TEXT,
			    S15_BLACK), "S-15.002 row %u shows %s", i, name);
	}
	CHECK(drawn(geom.edge, geom.title_y, "Main Menu", S15_BLACK, BOLD) &&
	      drawn(56, s15_status_text_y(&geom), "USB", S15_AMBER, CLEAR),
	      "the frame's bars");
	texts_placed("console");

	/* Styled rows in their styles. */
	s15_console_clear(&con);
	s15_console_append(&con, "");
	s15_console_add(&con, S15_ST_ITEM, "1 Install firmware");
	s15_console_add(&con, S15_ST_ERROR, "OS needs reinstalling");
	reset_frame();
	s15_render_console(&con, &geom, &draw, &f);
	CHECK(text_at(geom.margin, s15_row_y(&geom, 0), NULL, NULL) == NULL,
	      "S-15.002 a blank row draws nothing");
	CHECK(drawn(col(0), s15_row_y(&geom, 1), "1", S15_AMBER, S15_BLACK) &&
	      drawn(col(3), s15_row_y(&geom, 1), "Install firmware", S15_TEXT,
		    S15_BLACK) &&
	      drawn(col(0), s15_row_y(&geom, 2), "!", S15_RED, S15_BLACK),
	      "rows in their styles");

	/* The indicator in the title bar, with all 9 rows. */
	s15_console_clear(&con);
	for (i = 1; i <= 9; i++) {
		snprintf(name, sizeof name, "L%u", i);
		s15_console_append(&con, name);
	}
	f.option_on = true;
	reset_frame();
	s15_render_console(&con, &geom, &draw, &f);
	CHECK(drawn(S15_TEXT_W(&geom) - geom.edge -
		    (unsigned)strlen(s15_indicator_text) * geom.title_w, geom.title_y,
		    s15_indicator_text, S15_WHITE, BOLD) &&
	      drawn(geom.margin, s15_row_y(&geom, 8), "L9", S15_TEXT, S15_BLACK),
	      "indicator with a full console");
	texts_placed("option console");

	/* A black frame */
	reset_frame();
	s15_render_black(&geom, &draw);
	CHECK(all_black() && n_texts == 0u, "black frame");
}

static bool has(const uint8_t *k, size_t n, unsigned key)
{
	for (size_t i = 0; i < n; i++)
		if (k[i] == key)
			return true;
	return false;
}

static void test_keytest(void)
{
	struct s15_keytest kt;
	size_t i, e;
	bool end = false;

	/* Colors and fills. */
	for (i = 0; i < S15V_N(s15v_colors); i++) {
		const struct s15v_color *c = &s15v_colors[i];
		uint16_t want = strcmp(c->state, "untested") == 0 ? S15_KT_UNTESTED
			      : strcmp(c->state, "down") == 0 ? S15_KT_DOWN
							      : S15_KT_TESTED;

		CHECK(c->outline == want, "color %s", c->state);
		CHECK(s15_keytest_fill(c->outline) == c->fill,
		      "fill %s", c->state);
	}

	/* S-15.064, S-15.065: the sequences. */
	for (i = 0; i < S15V_N(s15v_keyseqs); i++) {
		const struct s15v_keyseq *v = &s15v_keyseqs[i];

		s15_keytest_start(&kt);
		for (e = 0; e < v->n; e++)
			end = s15_keytest_event(&kt, v->events[e][0],
						v->events[e][1] != 0);
		for (unsigned k = 0; k < S15_NKEYS; k++)
			CHECK(s15_keytest_tested(&kt, k) ==
			      has(v->tested, v->n_tested, k),
			      "S-15.064 %s: key 0x%02x", v->name, k);
		CHECK(end == v->end, "S-15.065 %s: end", v->name);
	}

	/* All 72 keys alone: ends at the 72nd up, not before. */
	s15_keytest_start(&kt);
	for (unsigned k = 0; k < S15_NKEYS; k++) {
		bool d = s15_keytest_event(&kt, k, true);
		bool u = s15_keytest_event(&kt, k, false);

		CHECK(!d && u == (k == S15_NKEYS - 1u), "S-15.065 all keys %u", k);
	}
	/* S-15.062: a new start forgets everything. */
	s15_keytest_start(&kt);
	CHECK(!s15_keytest_tested(&kt, 0) &&
	      s15_keytest_color(&kt, 0) == S15_KT_UNTESTED, "S-15.062 restart");

	/* Down, back to the color it had after an overlapping
	 * press, tested after a lone one. */
	(void)s15_keytest_event(&kt, 6, true);
	CHECK(s15_keytest_color(&kt, 6) == S15_KT_DOWN, "down is amber");
	(void)s15_keytest_event(&kt, 7, true);
	(void)s15_keytest_event(&kt, 6, false);
	CHECK(s15_keytest_color(&kt, 6) == S15_KT_UNTESTED, "back to dark gray");
	(void)s15_keytest_event(&kt, 7, false);
	(void)s15_keytest_event(&kt, 7, true);
	(void)s15_keytest_event(&kt, 7, false);
	CHECK(s15_keytest_color(&kt, 7) == S15_KT_TESTED, "lone press light gray");
	CHECK(!s15_keytest_event(&kt, 200, true), "unknown key ignored");
}

/* S-15.062: 72 boxes, not overlapping each other, the two lines or
 * either bar, between the bars' edges; the frame shows them, its lines
 * in their styles and its bars. */
static void test_boxes(void)
{
	struct s15_rect r[S15_NKEYS];
	struct s15_keytest kt;
	struct s15_frame f = { "Key Test", "1.0.0", 0, 0, true, false, false };
	unsigned i, j;
	uint32_t lines_end = s15_row_y(&geom, S15_KEYTEST_LINES - 1u) +
			     geom.glyph_h;

	for (i = 0; i < S15_NKEYS; i++) {
		CHECK(s15_keytest_box(&geom, i, &r[i]), "box %u", i);
		CHECK(r[i].x >= geom.edge &&
		      r[i].x + r[i].w <= geom.width - geom.edge &&
		      r[i].y >= lines_end && r[i].y >= geom.bar_h &&
		      r[i].y + r[i].h <= geom.status_y && r[i].w >= 4u &&
		      r[i].h >= 4u,
		      "S-15.062 box %u placed (%u,%u %ux%u)", i, r[i].x, r[i].y,
		      r[i].w, r[i].h);
	}
	for (i = 0; i < S15_NKEYS; i++)
		for (j = i + 1; j < S15_NKEYS; j++)
			CHECK(r[i].x + r[i].w <= r[j].x || r[j].x + r[j].w <= r[i].x ||
			      r[i].y + r[i].h <= r[j].y || r[j].y + r[j].h <= r[i].y,
			      "S-15.062 boxes %u and %u overlap", i, j);
	CHECK(!s15_keytest_box(&geom, S15_NKEYS, &r[0]), "no 73rd box");
	{
		struct s15_geometry g = geom;

		g.status_y = (uint16_t)(s15_row_y(&geom, S15_KEYTEST_LINES) + 10u);
		CHECK(!s15_keytest_box(&g, 0, &r[0]), "no room for boxes");
		CHECK(s15_keytest_box(&geom, 0, &r[0]), "box 0 again");
	}

	CHECK(strcmp(s15_keytest_lines[0],
		     "Press every key and knob, one at a time.") == 0 &&
	      strcmp(s15_keytest_lines[1], "`Shift` + `8`   Back to System Menu") == 0,
	      "S-15.062 the key test's lines");
	s15_keytest_start(&kt);
	(void)s15_keytest_event(&kt, 5, true);
	reset_frame();
	s15_render_keytest(&kt, &geom, &draw, &f);
	CHECK(drawn(geom.margin, s15_row_y(&geom, 0), s15_keytest_lines[0],
		    S15_TEXT, S15_BLACK) &&
	      drawn(col(0), s15_row_y(&geom, 1), "Shift", S15_AMBER, S15_BLACK) &&
	      drawn(col(5), s15_row_y(&geom, 1), " + ", S15_DIM, S15_BLACK),
	      "S-15.062 a text line and a keys line");
	CHECK(drawn(geom.edge, geom.title_y, "Key Test", S15_WHITE, BOLD) &&
	      drawn(S15_TEXT_W(&geom) - geom.edge -
		    (unsigned)strlen(s15_indicator_text) * geom.title_w, geom.title_y,
		    s15_indicator_text, S15_WHITE, BOLD),
	      "the key test's frame with the indicator");
	CHECK(fb[r[5].y][r[5].x] == S15_KT_DOWN &&
	      fb[r[5].y + 1][r[5].x + 1] == s15_keytest_fill(S15_KT_DOWN) &&
	      fb[r[6].y][r[6].x] == S15_KT_UNTESTED,
	      "box outline and fill in the frame");
	CHECK(box_is(0, 0, S15_TEXT_W(&geom), geom.bar_h, S15_RED) &&
	      fb[geom.status_y][0] == S15_STATUS_BG,
	      "the bars drawn with the boxes");
	texts_placed("key test");
}

/* S-15.132: the hand-over frame, the logo alone, against its digest and
 * pixel counts. */
struct hstate {
	struct s15_sha256 sha;
	unsigned white, faint;
	unsigned bytes;
};

static void hput(void *ctx, uint16_t p)
{
	struct hstate *h = ctx;

	s15_sha256_byte(&h->sha, (uint8_t)(p >> 8));
	s15_sha256_byte(&h->sha, (uint8_t)p);
	h->bytes += 2;
	h->white += p == 0xffffu;
	h->faint += p == S15_FAINT;
}

static void test_handover(void)
{
	struct hstate h;
	char hex[65];
	struct s15_sha256 abc;

	/* The test digest itself, on the FIPS 180-4 example "abc". */
	s15_sha256_init(&abc);
	s15_sha256_byte(&abc, 'a');
	s15_sha256_byte(&abc, 'b');
	s15_sha256_byte(&abc, 'c');
	s15_sha256_hex(&abc, hex);
	CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223"
			  "b00361a396177a9cb410ff61f20015ad") == 0,
	      "test SHA-256 known answer");

	s15_sha256_init(&h.sha);
	h.white = h.faint = h.bytes = 0;
	s15_handover_frame(hput, &h);
	s15_sha256_hex(&h.sha, hex);
	CHECK(h.bytes == S15V_HANDOVER_BYTES, "S-15.132 frame size");
	CHECK(h.white == S15V_HANDOVER_WHITE, "S-15.132 white pixels");
	CHECK(h.faint == S15V_HANDOVER_FAINT, "S-15.132 faint pixels");
	CHECK(s15_logo_pixel(59, 104) == S15_WHITE &&
	      s15_logo_pixel(81, 104) == S15_FAINT &&
	      s15_logo_pixel(86, 109) == S15_BLACK &&
	      s15_logo_pixel(73, 110) == S15_BLACK &&
	      s15_logo_pixel(407, 117) == S15_FAINT &&
	      s15_logo_pixel(421, 110) == S15_BLACK, "S-15.132 step squares");
	CHECK(strcmp(hex, s15v_handover_sha256) == 0, "S-15.132 frame digest");
}

int main(void)
{
	test_style_names();
	test_console_vectors();
	test_styled();
	test_logging();
	test_geometry();
	test_render_frame();
	test_render_row();
	test_frames();
	test_keytest();
	test_boxes();
	test_handover();
	return check_done("test_s15_console");
}
