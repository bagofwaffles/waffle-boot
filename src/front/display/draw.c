/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's screens: the boot logo, with no text (S-15.132), the line
 * under it (S-13c.025), and held mode's frame (wiki/Screen-Style-Guide.md)
 * with a text body. Copies of the full bootloader's drawing, trimmed: the
 * logo of s15_logo_pixel(), and the bars, colors, key badges and geometry
 * of core/ui/console.c and core/display/draw.c.
 */
#include "front/display/draw.h"

#include "front/display/font.h"
#include "front/display/panel.h"

/* RGB565 colors (wiki/Screen-Style-Guide.md). */
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
#define TOP       33u	/* y of body row 0 */
#define PITCH     19u
#define BAR_H     24u	/* the title bar, y 0-23 */
#define TITLE_Y   4u
#define STATUS_Y  202u	/* the status bar's first row */
#define STATUS_TY (STATUS_Y + 3u)

/* A key badge (core/ui/console.c's put_badge()): a solid 16 x 16 cell
 * centered on the digit's column, from 4 pixels left of it and a pixel
 * above the row's top, its corner pixels left out. */
#define BADGE_W   16u
#define BADGE_H   16u
#define BADGE_L   4u	/* the cell from this far left of the digit's x */

#if !defined(__bfin__)
static size_t len_of(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n] != '\0')
		n++;
	return n;
}
#endif

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
	(void)put_text(s->line[i], FR_LINE_MAX, text);
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

/* ---- a body row's parts ----
 *
 * walk() parses a body row into the parts it draws, in drawing order, each
 * over what is drawn before it: the painter and the host's model both take
 * the parts from it, so they read the control bytes the same way. */

enum part_kind { P_GLYPH, P_TITLE, P_CHIP, P_RING, P_BADGE };

struct part {
	uint8_t kind;		/* enum part_kind */
	uint8_t ch;		/* a glyph's or a badge's byte */
	uint16_t x, w;		/* its left side and width; a badge's digit's x */
	uint16_t color;
};

typedef void (*part_fn)(void *ctx, const struct part *p);

FR_FAST static uint16_t ink_of(unsigned ink)
{
	switch (ink) {
	case FR_INK_DIM:
		return DIM;
	case FR_INK_HEAD:
	case FR_INK_WARN:
		return AMBER;
	case FR_INK_OK:
		return GREEN;
	case FR_INK_ERROR:
		return RED;
	default:
		return TEXT;
	}
}

#define CHIP_PAD   4u	/* a chip's text from its side */
#define BADGE_RIGHT 12u	/* a badge's cell ends 12 pixels right of its digit's x */

FR_FAST static void emit(part_fn fn, void *ctx, uint8_t kind, uint8_t ch, unsigned x,
			 unsigned w, uint16_t color)
{
	struct part p;

	p.kind = kind;
	p.ch = ch;
	p.x = (uint16_t)x;
	p.w = (uint16_t)w;
	p.color = color;
	fn(ctx, &p);
}

FR_FAST static void walk(const char *t, uint8_t ink, part_fn fn, void *ctx)
{
	const size_t max = FR_LINE_MAX + 1u;	/* the row's whole field */
	uint16_t c = ink_of(ink);
	unsigned x = MARGIN;
	bool title = false;
	size_t i = 0;

	while (i < max && t[i] != '\0') {
		const uint8_t b = (uint8_t)t[i++];

		if (b == 0x01u || b == 0x02u || b == 0x05u) {
			/* the byte after it is its argument */
			uint8_t a;

			if (i >= max || t[i] == '\0')
				return;
			a = (uint8_t)t[i++];
			if (b == 0x01u) {
				if (a >= 1u && a <= 6u)
					c = ink_of(a - 1u);
				continue;
			}
			if (b == 0x02u) {
				if (x + BADGE_RIGHT > FR_CLIP_X)
					return;
				emit(fn, ctx, P_BADGE, a, x, FR_FONT_W, AMBER);
				x += FR_FONT_W;
				continue;
			}
			/* an outlined chip, to its end */
			{
				const uint16_t rc = a >= 1u && a <= 6u ? ink_of(a - 1u) : c;
				size_t j = i, k;
				unsigned w;

				while (j < max && t[j] != '\0' && t[j] != '\x04')
					j++;
				w = FR_FONT_W * (unsigned)(j - i) + 2u * CHIP_PAD;
				if (x + w > FR_CLIP_X)
					return;
				emit(fn, ctx, P_RING, 0, x, w, rc);
				for (k = i; k < j; k++)
					emit(fn, ctx, P_GLYPH, (uint8_t)t[k],
					     x + CHIP_PAD + FR_FONT_W * (unsigned)(k - i), FR_FONT_W, rc);
				x += w;
				i = j < max && t[j] == '\x04' ? j + 1u : j;
			}
			continue;
		}
		if (b == 0x03u) {
			size_t j = i, k;
			unsigned w;

			while (j < max && t[j] != '\0' && t[j] != '\x04')
				j++;
			w = FR_FONT_W * (unsigned)(j - i) + 2u * CHIP_PAD;
			if (x + w > FR_CLIP_X)
				return;
			emit(fn, ctx, P_CHIP, 0, x, w, STATUS_BG);
			for (k = i; k < j; k++)
				emit(fn, ctx, P_GLYPH, (uint8_t)t[k],
				     x + CHIP_PAD + FR_FONT_W * (unsigned)(k - i), FR_FONT_W, TEXT);
			x += w;
			i = j < max && t[j] == '\x04' ? j + 1u : j;
			continue;
		}
		if (b == 0x04u)
			continue;
		if (b == 0x06u || b == 0x07u) {
			title = b == 0x06u;
			continue;
		}
		{
			const unsigned w = title ? FR_TITLE_W : FR_FONT_W;

			if (x + w > FR_CLIP_X)
				return;
			emit(fn, ctx, title ? P_TITLE : P_GLYPH, b, x, w, c);
			x += w;
		}
	}
}

/* The bar on row i, when there is one: its x range [*x0, *x0 + *w) and
 * its fill's width. */
FR_FAST static bool bar_of(const struct fr_screen *s, unsigned i, unsigned *x0, unsigned *w,
			   unsigned *fw)
{
	unsigned cols;

	if (s->bar_line != (int)i || s->bar_cols == 0u || s->bar_col >= FR_COLS)
		return false;
	cols = FR_COLS - s->bar_col;
	if (cols > s->bar_cols)
		cols = s->bar_cols;
	*x0 = MARGIN + s->bar_col * FR_FONT_W;
	*w = cols * FR_FONT_W;
	*fw = (*w - 4u) * (s->bar_pm > 1000u ? 1000u : s->bar_pm) / 1000u;
	return true;
}

FR_FAST static unsigned glyph_index(uint8_t c)
{
	unsigned ch = c;

	if (ch < FR_FONT_FIRST || ch >= FR_FONT_FIRST + FR_FONT_COUNT)
		ch = '?';
	return ch - FR_FONT_FIRST;
}

/* Body row i's first pixel row: one above row i's top, for the title
 * font; its last, one above row i + 1's first, or the status bar's. */
#define BAND_Y(i)   (TOP + (i) * PITCH - 1u)
#define BAND_END(i) ((i) + 1u < FR_LINES ? BAND_Y((i) + 1u) : STATUS_Y)

/* ---- the host's model, a pixel at a time ---- */

#if !defined(__bfin__)

/* Whether text t of n glyphs from (x0, y0) has a stroke at (x, y). */
static bool stroke(const char *t, size_t n, unsigned x0, unsigned y0, unsigned x, unsigned y)
{
	unsigned dx;

	if (x < x0 || y < y0 || y >= y0 + FR_FONT_H)
		return false;
	dx = x - x0;
	if (dx / FR_FONT_W >= n)
		return false;
	return ((unsigned)fr_font[glyph_index((uint8_t)t[dx / FR_FONT_W])][y - y0] >>
		(FR_FONT_W - 1u - dx % FR_FONT_W) & 1u) != 0u;
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

/* The title bar (core/ui/console.c's frame() and logo_squares(), for the red
 * bar): solid red from side to side with the title, and the four squares
 * at its right in dark red, the first filled, the others outlined. */
static uint16_t title_bar(const struct fr_screen *s, unsigned x, unsigned y)
{
	size_t n = len_of(s->title, FR_TITLE_MAX);

	if (x >= EDGE && y >= TITLE_Y && y < TITLE_Y + FR_TITLE_H) {
		unsigned dx = x - EDGE, k = dx / FR_TITLE_W;

		if (k < n && ((unsigned)fr_title_font[glyph_index((uint8_t)s->title[k])][y - TITLE_Y] >>
			      (FR_TITLE_W - 1u - dx % FR_TITLE_W) & 1u) != 0u)
			return WHITE;
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

static uint16_t status_bar(const struct fr_screen *s, unsigned x, unsigned y)
{
	size_t nl = len_of(s->status_left, FR_STATUS_MAX), nr = len_of(s->status_right, FR_STATUS_MAX);

	/* The right text is drawn last, over the left. */
	if (stroke(s->status_right, nr, FR_SCREEN_W - EDGE - (unsigned)nr * FR_FONT_W, STATUS_TY, x, y))
		return DIM;
	if (stroke(s->status_left, nl, EDGE, STATUS_TY, x, y))
		return AMBER;
	return STATUS_BG;
}

/* The pixel (x, y) of a row whose top is y0, as its parts leave it. */
struct probe {
	unsigned x, y, y0;
	bool hit;
	uint16_t c;
};

static bool in_box(const struct probe *q, unsigned x0, unsigned w, unsigned y0, unsigned h)
{
	return q->x >= x0 && q->x < x0 + w && q->y >= y0 && q->y < y0 + h;
}

/* A w x h box from (x0, y0) less its four corner pixels: 1 inside, 2 on its
 * outline, 0 outside. */
static unsigned box_at(const struct probe *q, unsigned x0, unsigned w, unsigned y0, unsigned h)
{
	unsigned dx, dy;
	bool sx, sy;

	if (!in_box(q, x0, w, y0, h))
		return 0;
	dx = q->x - x0;
	dy = q->y - y0;
	sx = dx == 0u || dx == w - 1u;
	sy = dy == 0u || dy == h - 1u;
	if (sx && sy)
		return 0;
	return sx || sy ? 2u : 1u;
}

static void probe_part(void *ctx, const struct part *p)
{
	struct probe *q = ctx;
	unsigned dx;

	switch (p->kind) {
	case P_GLYPH:
		if (in_box(q, p->x, FR_FONT_W, q->y0, FR_FONT_H)) {
			dx = q->x - p->x;
			if (((unsigned)fr_font[glyph_index(p->ch)][q->y - q->y0] >>
			     (FR_FONT_W - 1u - dx) & 1u) != 0u) {
				q->hit = true;
				q->c = p->color;
			}
		}
		break;
	case P_TITLE:
		if (in_box(q, p->x, FR_TITLE_W, q->y0 - 1u, FR_TITLE_H)) {
			dx = q->x - p->x;
			if (((unsigned)fr_title_font[glyph_index(p->ch)][q->y - q->y0 + 1u] >>
			     (FR_TITLE_W - 1u - dx) & 1u) != 0u) {
				q->hit = true;
				q->c = p->color;
			}
		}
		break;
	case P_CHIP:
	case P_RING: {
		const unsigned k = box_at(q, p->x, p->w, q->y0, FR_FONT_H);

		if (k == 2u || (k == 1u && p->kind == P_CHIP)) {
			q->hit = true;
			q->c = p->color;
		}
		break;
	}
	case P_BADGE:
	default:
		/* the 16 x 16 cell less its corners; the digit in black at x
		 * and x + 1 */
		if (box_at(q, p->x - BADGE_L, BADGE_W, q->y0 - 1u, BADGE_H) != 0u) {
			q->hit = true;
			q->c = p->color;
		}
		for (unsigned k = 0; k < 2u; k++)
			if (in_box(q, p->x + k, FR_FONT_W, q->y0, FR_FONT_H)) {
				dx = q->x - p->x - k;
				if (((unsigned)fr_font[glyph_index(p->ch)][q->y - q->y0] >>
				     (FR_FONT_W - 1u - dx) & 1u) != 0u) {
					q->hit = true;
					q->c = BLACK;
				}
			}
		break;
	}
}

static uint16_t body(const struct fr_screen *s, unsigned x, unsigned y)
{
	struct probe q;
	unsigned i, x0, w, fw;

	if (y < BAND_Y(0))
		return BLACK;
	i = (y - BAND_Y(0)) / PITCH;
	if (i >= FR_LINES)
		return BLACK;
	q.x = x;
	q.y = y;
	q.y0 = TOP + i * PITCH;
	q.hit = false;
	q.c = BLACK;
	/* The install screen's bar (core/ui/console.c's put_step()), over the
	 * row: a one-pixel outline 10 high, 3 below the row's top, and its
	 * fill 2 inside. */
	if (bar_of(s, i, &x0, &w, &fw) && x >= x0 && x < x0 + w && y >= q.y0 &&
	    y < q.y0 + FR_FONT_H) {
		const unsigned dy = y - q.y0, by = 3u, bh = FR_FONT_H - 6u;

		if (dy < by || dy >= by + bh)
			return BLACK;
		if (dy == by || dy == by + bh - 1u || x == x0 || x == x0 + w - 1u)
			return AMBER;
		if (dy >= by + 2u && dy < by + bh - 2u && x >= x0 + 2u && x < x0 + 2u + fw)
			return AMBER;
		return BLACK;
	}
	walk(s->line[i], s->ink[i], probe_part, &q);
	return q.hit ? q.c : BLACK;
}

uint16_t fr_screen_pixel(const struct fr_screen *s, unsigned x, unsigned y)
{
	if (x >= FR_SCREEN_W || y >= FR_SCREEN_H)
		return BLACK;
	if (s->kind == FR_SCREEN_LOGO)
		return logo(x, y);
	if (y < BAR_H)
		return title_bar(s, x, y);
	if (y >= STATUS_Y)
		return status_bar(s, x, y);
	return body(s, x, y);
}

uint16_t fr_line_pixel(const char *text, unsigned x, unsigned dy)
{
	const size_t n = len_of(text, FR_SCREEN_W / FR_FONT_W);

	if (x >= FR_SCREEN_W || dy >= FR_FONT_H)
		return BLACK;
	return stroke(text, n, (FR_SCREEN_W - (unsigned)n * FR_FONT_W) / 2u, 0, x, dy) ? DIM : BLACK;
}

#endif /* !__bfin__ */

/* ---- the painter ----
 *
 * Writes a screen into the frame buffer, window row by window row: screen
 * (x, y) is window row 479 - x, pixel y (S-13c.023), high byte first
 * (S-13c.015). It runs from L1 code, since the front runs with the caches
 * off, and serves the link every few window rows. */

#define FB_COLS 224u	/* FR_PANEL_COLS */

FR_FAST static uint8_t *at(uint8_t *buf, unsigned x, unsigned y)
{
	return buf + 2u * ((size_t)(FR_SCREEN_W - 1u - x) * FB_COLS + y);
}

/* w x h pixels from (x0, y0) in color c. */
FR_FAST static void fill(uint8_t *buf, unsigned x0, unsigned w, unsigned y0, unsigned h, uint16_t c)
{
	const uint8_t hi = (uint8_t)(c >> 8), lo = (uint8_t)c;
	unsigned x, k;

	for (x = x0; x < x0 + w; x++) {
		uint8_t *p = at(buf, x, y0);

		for (k = 0; k < h; k++) {
			*p++ = hi;
			*p++ = lo;
		}
		if ((x & 3u) == 0u)
			fr_serve();
	}
}

FR_FAST static void dot(uint8_t *buf, unsigned x, unsigned y, uint16_t c)
{
	uint8_t *p = at(buf, x, y);

	p[0] = (uint8_t)(c >> 8);
	p[1] = (uint8_t)c;
}

/* An 8 x 16 glyph's strokes from (x, y), over what is there. */
FR_FAST static void glyph8(uint8_t *buf, unsigned x, unsigned y, uint8_t ch, uint16_t c)
{
	const uint8_t *g = fr_font[glyph_index(ch)];
	unsigned dx, dy;

	for (dy = 0; dy < FR_FONT_H; dy++)
		for (dx = 0; dx < FR_FONT_W; dx++)
			if (((unsigned)g[dy] >> (FR_FONT_W - 1u - dx) & 1u) != 0u)
				dot(buf, x + dx, y + dy, c);
}

/* The title bar's 10 x 17 glyph. */
FR_FAST static void glyph10(uint8_t *buf, unsigned x, unsigned y, uint8_t ch, uint16_t c)
{
	const uint16_t *g = fr_title_font[glyph_index(ch)];
	unsigned dx, dy;

	for (dy = 0; dy < FR_TITLE_H; dy++)
		for (dx = 0; dx < FR_TITLE_W; dx++)
			if (((unsigned)g[dy] >> (FR_TITLE_W - 1u - dx) & 1u) != 0u)
				dot(buf, x + dx, y + dy, c);
}

/* A w x h box from (x0, y0) less its corner pixels: filled, or its
 * outline alone. */
FR_FAST static void box(uint8_t *buf, unsigned x0, unsigned w, unsigned y0, unsigned h, uint16_t c,
			bool filled)
{
	fill(buf, x0, 1, y0 + 1u, h - 2u, c);
	fill(buf, x0 + w - 1u, 1, y0 + 1u, h - 2u, c);
	if (filled) {
		fill(buf, x0 + 1u, w - 2u, y0, h, c);
		return;
	}
	fill(buf, x0 + 1u, w - 2u, y0, 1, c);
	fill(buf, x0 + 1u, w - 2u, y0 + h - 1u, 1, c);
}

struct pen {
	uint8_t *buf;
	unsigned y0;
};

FR_FAST static void paint_part(void *ctx, const struct part *p)
{
	const struct pen *n = ctx;

	switch (p->kind) {
	case P_GLYPH:
		glyph8(n->buf, p->x, n->y0, p->ch, p->color);
		break;
	case P_TITLE:
		glyph10(n->buf, p->x, n->y0 - 1u, p->ch, p->color);
		break;
	case P_CHIP:
	case P_RING:
		box(n->buf, p->x, p->w, n->y0, FR_FONT_H, p->color, p->kind == P_CHIP);
		break;
	case P_BADGE:
	default:
		box(n->buf, p->x - BADGE_L, BADGE_W, n->y0 - 1u, BADGE_H, p->color, true);
		glyph8(n->buf, p->x, n->y0, p->ch, BLACK);
		glyph8(n->buf, p->x + 1u, n->y0, p->ch, BLACK);
		break;
	}
}

/* Text of the bars, n glyphs from (x0, y), over what is there. */
FR_FAST static void text(uint8_t *buf, const char *t, size_t n, unsigned x0, unsigned y, uint16_t c)
{
	size_t k;

	for (k = 0; k < n; k++)
		glyph8(buf, x0 + FR_FONT_W * (unsigned)k, y, (uint8_t)t[k], c);
}

FR_FAST static size_t fast_len(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n] != '\0')
		n++;
	return n;
}

/* title_bar()'s pixels. */
FR_FAST static void paint_title(uint8_t *buf, const struct fr_screen *s)
{
	const size_t n = fast_len(s->title, FR_TITLE_MAX);
	unsigned i;

	fill(buf, 0, FR_SCREEN_W, 0, BAR_H, RED);
	for (i = 0; i < 4u; i++) {
		const unsigned x = MARK_X + i * (MARK + MARK_GAP);

		if (i == 0u) {
			fill(buf, x, MARK, MARK_Y, MARK, RED_DARK);
			continue;
		}
		fill(buf, x, MARK, MARK_Y, MARK_LINE, RED_DARK);
		fill(buf, x, MARK, MARK_Y + MARK - MARK_LINE, MARK_LINE, RED_DARK);
		fill(buf, x, MARK_LINE, MARK_Y + MARK_LINE, MARK - 2u * MARK_LINE, RED_DARK);
		fill(buf, x + MARK - MARK_LINE, MARK_LINE, MARK_Y + MARK_LINE, MARK - 2u * MARK_LINE,
		     RED_DARK);
	}
	for (i = 0; i < n; i++)
		glyph10(buf, EDGE + FR_TITLE_W * i, TITLE_Y, (uint8_t)s->title[i], WHITE);
}

/* status_bar()'s pixels: the right text over the left. */
FR_FAST static void paint_status(uint8_t *buf, const struct fr_screen *s)
{
	const size_t nl = fast_len(s->status_left, FR_STATUS_MAX);
	const size_t nr = fast_len(s->status_right, FR_STATUS_MAX);

	fill(buf, 0, FR_SCREEN_W, STATUS_Y, FR_SCREEN_H - STATUS_Y, STATUS_BG);
	text(buf, s->status_left, nl, EDGE, STATUS_TY, AMBER);
	text(buf, s->status_right, nr, FR_SCREEN_W - EDGE - (unsigned)nr * FR_FONT_W, STATUS_TY, DIM);
}

/* body()'s pixels of row i, its band cleared first. */
FR_FAST static void paint_row(uint8_t *buf, const struct fr_screen *s, unsigned i)
{
	struct pen n;
	unsigned x0, w, f;

	fill(buf, 0, FR_SCREEN_W, BAND_Y(i), BAND_END(i) - BAND_Y(i), BLACK);
	n.buf = buf;
	n.y0 = TOP + i * PITCH;
	walk(s->line[i], s->ink[i], paint_part, &n);
	if (bar_of(s, i, &x0, &w, &f)) {
		fill(buf, x0, w, n.y0, FR_FONT_H, BLACK);
		fill(buf, x0, w, n.y0 + 3u, 1, AMBER);
		fill(buf, x0, w, n.y0 + 12u, 1, AMBER);
		fill(buf, x0, 1, n.y0 + 3u, 10, AMBER);
		fill(buf, x0 + w - 1u, 1, n.y0 + 3u, 10, AMBER);
		fill(buf, x0 + 2u, f, n.y0 + 5u, 6, AMBER);
	}
}

/* S-15.132's step row: white, or faint outlined squares. */
FR_FAST static void paint_logo(uint8_t *buf)
{
	unsigned i;

	fill(buf, 0, FR_SCREEN_W, 0, FB_COLS, BLACK);
	for (i = 0; i < 16u; i++) {
		const unsigned left = 59u + 22u * i + 6u * (i / 4u);

		if (i % 4u == 0u) {
			fill(buf, left, 14, 104, 14, WHITE);
		} else {
			fill(buf, left, 14, 104, 14, FAINT);
			fill(buf, left + 2u, 10, 106, 10, BLACK);
		}
	}
}

FR_FAST void fr_paint(uint8_t *buf, const struct fr_screen *s)
{
	unsigned i;

	if (s->kind == FR_SCREEN_LOGO) {
		paint_logo(buf);
		return;
	}
	/* S-13c.014: the window's columns past the visible 222 black */
	fill(buf, 0, FR_SCREEN_W, BAR_H, BAND_Y(0) - BAR_H, BLACK);
	fill(buf, 0, FR_SCREEN_W, FR_SCREEN_H, FB_COLS - FR_SCREEN_H, BLACK);
	paint_title(buf, s);
	paint_status(buf, s);
	for (i = 0; i < FR_LINES; i++)
		paint_row(buf, s, i);
}

FR_FAST static bool same_text(const char *a, const char *b, size_t max)
{
	size_t k;

	for (k = 0; k < max; k++) {
		if (a[k] != b[k])
			return false;
		if (a[k] == '\0')
			return true;
	}
	return true;
}

/* Row i's bar is drawn the same on both screens. */
FR_FAST static bool same_bar(const struct fr_screen *a, const struct fr_screen *b, unsigned i)
{
	unsigned x0a, wa, fa, x0b, wb, fb;
	const bool on_a = bar_of(a, i, &x0a, &wa, &fa), on_b = bar_of(b, i, &x0b, &wb, &fb);

	return on_a == on_b && (!on_a || (x0a == x0b && wa == wb && fa == fb));
}

FR_FAST void fr_paint_rows(uint8_t *buf, const struct fr_screen *s, const struct fr_screen *prev)
{
	unsigned i;

	if (prev == NULL || prev->kind != s->kind || s->kind == FR_SCREEN_LOGO) {
		fr_paint(buf, s);
		return;
	}
	if (!same_text(s->title, prev->title, FR_TITLE_MAX + 1u))
		paint_title(buf, s);
	if (!same_text(s->status_left, prev->status_left, FR_STATUS_MAX + 1u) ||
	    !same_text(s->status_right, prev->status_right, FR_STATUS_MAX + 1u))
		paint_status(buf, s);
	for (i = 0; i < FR_LINES; i++)
		if (s->ink[i] != prev->ink[i] || !same_bar(s, prev, i) ||
		    !same_text(s->line[i], prev->line[i], FR_LINE_MAX + 1u))
			paint_row(buf, s, i);
}

FR_FAST void fr_paint_line(uint8_t *band, const char *t)
{
	const size_t n = fast_len(t, FR_SCREEN_W / FR_FONT_W);
	const unsigned x0 = (FR_SCREEN_W - (unsigned)n * FR_FONT_W) / 2u;
	size_t k;
	unsigned r, dx, dy;

	for (r = 0; r < FR_LINE_BYTES; r++) {
		band[r] = 0;
		if ((r & 1023u) == 0u)
			fr_serve();
	}
	for (k = 0; k < n; k++) {
		const uint8_t *g = fr_font[glyph_index((uint8_t)t[k])];

		for (dx = 0; dx < FR_FONT_W; dx++) {
			uint8_t *p = band + 2u * ((size_t)(FR_SCREEN_W - 1u - (x0 + FR_FONT_W * (unsigned)k + dx)) *
						  FR_FONT_H);

			for (dy = 0; dy < FR_FONT_H; dy++)
				if (((unsigned)g[dy] >> (FR_FONT_W - 1u - dx) & 1u) != 0u) {
					p[2u * dy] = (uint8_t)(DIM >> 8);
					p[2u * dy + 1u] = (uint8_t)DIM;
				}
		}
	}
}
