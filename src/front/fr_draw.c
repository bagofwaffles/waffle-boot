/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's screens, a pixel at a time: the boot logo, with no text
 * (S-15.132), and held mode's frame (docs/ui-style-guide.md) with a text
 * body. Copies of the full bootloader's drawing, trimmed: the logo of
 * s15_logo_pixel(), and the bars, colors and geometry of s15_console.c
 * and display_draw.c.
 */
#include "fr_draw.h"

#include "fr_font.h"
#include "fr_panel.h"

/* RGB565 colors (docs/ui-style-guide.md). */
#define BLACK      0x0000u
#define WHITE      0xffffu
#define TEXT       0xef5du
#define DIM        0x8c71u
#define FAINT      0x5acbu
#define AMBER      0xfd84u
#define RED        0xfa88u
#define GREEN      0x56ceu
#define RED_DARK   0xc1e6u	/* C03C30: the squares (S15_RED_DARK) */
#define STATUS_BG  0x2124u	/* the status bar (S15_STATUS_BG) */

/* The title bar's group of squares (S15_MARK_*): four, MARK pixels each
 * and MARK_GAP apart, the last one EDGE from the right side. */
#define MARK       14u
#define MARK_GAP   8u
#define MARK_LINE  2u
#define MARK_X     392u		/* 480 - EDGE - 4 * MARK - 3 * MARK_GAP */
#define MARK_Y     5u		/* (BAR_H - MARK) / 2 */
#define MARK_END   (MARK_X + 4u * MARK + 3u * MARK_GAP)

/* Held mode's geometry (wb_display_geometry). */
#define EDGE      8u	/* x of the bars' text */
#define MARGIN    16u	/* x of body column 0 */
#define TOP       27u	/* y of body row 0 */
#define PITCH     19u
#define BAR_H     24u	/* the title bar, y 0-23 */
#define TITLE_Y   4u
#define STATUS_Y  202u	/* the status bar's first row */
#define STATUS_TY (STATUS_Y + 3u)

static size_t len_of(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n] != '\0')
		n++;
	return n;
}

static size_t put_text(char *dst, size_t max, const char *src)
{
	size_t n = 0;

	if (src != NULL)
		for (; n < max && src[n] != '\0'; n++)
			dst[n] = src[n];
	dst[n] = '\0';
	return n;
}

static void clear(struct fr_screen *s)
{
	uint8_t *p = (uint8_t *)s;
	size_t i;

	for (i = 0; i < sizeof *s; i++)
		p[i] = 0;
	s->bar_line = -1;
}

void fr_screen_text(struct fr_screen *s, const char *title)
{
	clear(s);
	s->kind = FR_SCREEN_TEXT;
	(void)put_text(s->title, FR_TITLE_MAX, title);
}

/* S-15.132: the logo alone, as the hand-over frame has it. */
void fr_screen_logo(struct fr_screen *s)
{
	clear(s);
	s->kind = FR_SCREEN_LOGO;
}

void fr_screen_line(struct fr_screen *s, unsigned i, enum fr_ink ink, const char *text)
{
	if (i >= FR_LINES)
		return;
	s->ink[i] = (uint8_t)ink;
	(void)put_text(s->line[i], FR_COLS, text);
}

void fr_screen_status(struct fr_screen *s, const char *left, const char *right)
{
	(void)put_text(s->status_left, FR_STATUS_MAX, left);
	(void)put_text(s->status_right, FR_STATUS_MAX, right);
}

size_t fr_text_dec(char *out, uint32_t v)
{
	char t[10];
	size_t n = 0, i;

	do {
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0u);
	for (i = 0; i < n; i++)
		out[i] = t[n - 1u - i];
	return n;
}

/* S-10c.055's form, as s11_version_text() writes it, after a "v". */
size_t fr_text_version(char *out, const uint16_t v[4])
{
	size_t n = 0;

	out[n++] = 'v';
	n += fr_text_dec(out + n, v[0]);
	out[n++] = '.';
	n += fr_text_dec(out + n, v[1]);
	out[n++] = '.';
	n += fr_text_dec(out + n, v[2]);
	if (v[3] != 0u) {
		out[n++] = '+';
		n += fr_text_dec(out + n, v[3]);
	}
	return n;
}

/* ---- pixels ---- */

/* What the pixels of a screen share: its texts' lengths. fr_screen_row()
 * keeps one pen for a window row, so the texts are measured once a row,
 * not once a pixel. */
struct pen {
	const struct fr_screen *s;
	uint8_t n_title, n_left, n_right, n_line[FR_LINES];
};

static void pen_on(struct pen *p, const struct fr_screen *s)
{
	unsigned i;

	p->s = s;
	p->n_title = (uint8_t)len_of(s->title, FR_TITLE_MAX);
	p->n_left = (uint8_t)len_of(s->status_left, FR_STATUS_MAX);
	p->n_right = (uint8_t)len_of(s->status_right, FR_STATUS_MAX);
	for (i = 0; i < FR_LINES; i++)
		p->n_line[i] = (uint8_t)len_of(s->line[i], FR_COLS);
}

static const uint8_t *glyph(char c)
{
	unsigned ch = (unsigned char)c;

	if (ch < FR_FONT_FIRST || ch >= FR_FONT_FIRST + FR_FONT_COUNT)
		ch = '?';
	return fr_font[ch - FR_FONT_FIRST];
}

/* Whether text t of n glyphs from (x0, y0) has a stroke at (x, y). */
static bool stroke(const char *t, size_t n, unsigned x0, unsigned y0,
		   unsigned x, unsigned y, size_t *k)
{
	unsigned dx;

	if (x < x0 || y < y0 || y >= y0 + FR_FONT_H)
		return false;
	dx = x - x0;
	if (dx / FR_FONT_W >= n)
		return false;
	*k = dx / FR_FONT_W;
	return ((unsigned)glyph(t[*k])[y - y0] >> (FR_FONT_W - 1u - dx % FR_FONT_W) & 1u) != 0u;
}

/* S-15.132's step row (s15_logo_pixel()). */
static uint16_t logo(unsigned x, unsigned y)
{
	unsigned i, left = 0, dx, dy;

	if (y < 104u || y > 117u || x < 59u)
		return BLACK;
	for (i = 0; i < 16u; i++) {
		left = 59u + 22u * i + 6u * (i / 4u);
		if (x < left)
			return BLACK;
		if (x < left + 14u)
			break;
	}
	if (i == 16u)
		return BLACK;
	if (i % 4u == 0u)
		return WHITE;
	dx = x - left;
	dy = y - 104u;
	if (dx < 2u || dy < 2u || dx > 11u || dy > 11u)
		return FAINT;
	return BLACK;
}

/* The title bar (s15_console.c's frame() and logo_squares(), for the red
 * bar): solid red from side to side with the title, and the four squares
 * at its right in dark red, the first filled, the others outlined. */
static uint16_t title_bar(const struct pen *p, unsigned x, unsigned y)
{
	const struct fr_screen *s = p->s;
	size_t n = p->n_title;

	if (x >= EDGE && y >= TITLE_Y && y < TITLE_Y + FR_TITLE_H) {
		unsigned dx = x - EDGE, k = dx / FR_TITLE_W;

		if (k < n) {
			unsigned ch = (unsigned char)s->title[k];

			if (ch < FR_FONT_FIRST || ch >= FR_FONT_FIRST + FR_FONT_COUNT)
				ch = '?';
			if (((unsigned)fr_title_font[ch - FR_FONT_FIRST][y - TITLE_Y] >>
			     (FR_TITLE_W - 1u - dx % FR_TITLE_W) & 1u) != 0u)
				return WHITE;
		}
	}
	if (x >= MARK_X && x < MARK_END && y >= MARK_Y && y < MARK_Y + MARK) {
		unsigned dx = (x - MARK_X) % (MARK + MARK_GAP), dy = y - MARK_Y;

		if (dx >= MARK)
			return RED;
		if (x < MARK_X + MARK)
			return RED_DARK;
		if (dx < MARK_LINE || dy < MARK_LINE || dx >= MARK - MARK_LINE ||
		    dy >= MARK - MARK_LINE)
			return RED_DARK;
	}
	return RED;
}

static uint16_t status_bar(const struct pen *p, unsigned x, unsigned y)
{
	const struct fr_screen *s = p->s;
	size_t nl = p->n_left, nr = p->n_right, k;

	/* The right text is drawn last, over the left. */
	if (stroke(s->status_right, nr, FR_SCREEN_W - EDGE - (unsigned)nr * FR_FONT_W,
		   STATUS_TY, x, y, &k))
		return DIM;
	if (stroke(s->status_left, nl, EDGE, STATUS_TY, x, y, &k))
		return AMBER;
	return STATUS_BG;
}

/* The install screen's bar (s15_console.c's put_step()): a one-pixel
 * outline 10 high, 3 below the row's top, and its fill 2 inside. */
static uint16_t bar(const struct fr_screen *s, unsigned x, unsigned dy)
{
	unsigned cols = s->bar_cols, x0 = MARGIN + s->bar_col * FR_FONT_W, w, fill;
	const unsigned by = 3u, bh = FR_FONT_H - 6u;

	if (s->bar_col >= FR_COLS)
		return BLACK;
	if (cols > FR_COLS - s->bar_col)
		cols = FR_COLS - s->bar_col;
	w = cols * FR_FONT_W;
	fill = w >= 4u ? (w - 4u) * (s->bar_pct > 100u ? 100u : s->bar_pct) / 100u : 0u;
	if (dy < by || dy >= by + bh)
		return BLACK;
	if (dy == by || dy == by + bh - 1u || x == x0 || x == x0 + w - 1u)
		return AMBER;
	if (dy >= by + 2u && dy < by + bh - 2u && x >= x0 + 2u && x < x0 + 2u + fill)
		return AMBER;
	return BLACK;
}

static uint16_t ink_color(uint8_t ink)
{
	static const uint16_t c[] = { TEXT, DIM, AMBER, GREEN, AMBER, RED };

	return ink < sizeof c / sizeof c[0] ? c[ink] : TEXT;
}

static uint16_t body(const struct pen *p, unsigned x, unsigned y)
{
	const struct fr_screen *s = p->s;
	unsigned i, dy;
	size_t k;

	if (y < TOP)
		return BLACK;
	i = (y - TOP) / PITCH;
	dy = (y - TOP) % PITCH;
	if (i >= FR_LINES || dy >= FR_FONT_H)
		return BLACK;
	if (s->bar_line == (int)i && s->bar_cols != 0u) {
		unsigned x0 = MARGIN + s->bar_col * FR_FONT_W;
		unsigned cols = s->bar_col < FR_COLS ? FR_COLS - s->bar_col : 0u;

		if (cols > s->bar_cols)
			cols = s->bar_cols;
		if (x >= x0 && x < x0 + cols * FR_FONT_W)
			return bar(s, x, dy);
	}
	if (stroke(s->line[i], p->n_line[i], MARGIN, TOP + i * PITCH, x, y, &k))
		return ink_color(s->ink[i]);
	return BLACK;
}

static uint16_t pixel(const struct pen *p, unsigned x, unsigned y)
{
	const struct fr_screen *s = p->s;

	if (x >= FR_SCREEN_W || y >= FR_SCREEN_H)
		return BLACK;
	if (s->kind == FR_SCREEN_LOGO)
		return logo(x, y);
	if (y < BAR_H)
		return title_bar(p, x, y);
	if (y >= STATUS_Y)
		return status_bar(p, x, y);
	return body(p, x, y);
}

uint16_t fr_screen_pixel(const struct fr_screen *s, unsigned x, unsigned y)
{
	struct pen p;

	pen_on(&p, s);
	return pixel(&p, x, y);
}

/* S-13c.023: window row r is screen column 479 - r, top down; S-13c.015:
 * high byte first; S-13c.014: columns past the visible 222 black. */
void fr_screen_row(void *ctx, unsigned r, uint8_t *out)
{
	const struct fr_screen *s = ctx;
	struct pen pen;
	unsigned x = FR_SCREEN_W - 1u - r, c;

	pen_on(&pen, s);
	for (c = 0; c < FR_PANEL_COLS; c++) {
		uint16_t p = c < FR_PANEL_VISIBLE ? pixel(&pen, x, c) : BLACK;

		out[2u * c] = (uint8_t)(p >> 8);
		out[2u * c + 1u] = (uint8_t)p;
	}
}
