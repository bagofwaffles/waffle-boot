/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 key test.
 */
#include "waffle/ui/keytest.h"

/* S-15.062: the device's front as a grid. */
#define GRID_COLS 17u
#define GRID_ROWS 6u

/* A place on the grid, in columns and rows. */
struct cell {
	uint8_t col, row, w, h;
};

/* Each key's place, by key number (S-07.030). */
static const struct cell cells[S15_NKEYS] = {
	/* help, tempo */
	{ 2, 1, 1, 1 }, { 3, 1, 1, 1 },
	/* synth, drum, tape, mixer */
	{ 0, 2, 1, 1 }, { 1, 2, 1, 1 }, { 2, 2, 1, 1 }, { 3, 2, 1, 1 },
	/* M1 to M4 */
	{ 4, 2, 1, 1 }, { 5, 2, 1, 1 }, { 6, 2, 1, 1 }, { 7, 2, 1, 1 },
	/* the knobs' clicks */
	{ 8, 0, 2, 2 }, { 10, 0, 2, 2 }, { 12, 0, 2, 2 }, { 14, 0, 2, 2 },
	/* sound 1 to 8 */
	{ 8, 2, 1, 1 }, { 9, 2, 1, 1 }, { 10, 2, 1, 1 }, { 11, 2, 1, 1 },
	{ 12, 2, 1, 1 }, { 13, 2, 1, 1 }, { 14, 2, 1, 1 }, { 15, 2, 1, 1 },
	/* seq, mic, com, bar */
	{ 16, 2, 1, 1 }, { 16, 0, 1, 1 }, { 16, 1, 1, 1 }, { 16, 3, 1, 1 },
	/* the piano, F3 to E5: naturals in row 5, sharps in row 4 */
	{ 3, 5, 1, 1 }, { 4, 4, 1, 1 }, { 4, 5, 1, 1 }, { 5, 4, 1, 1 },
	{ 5, 5, 1, 1 }, { 6, 4, 1, 1 }, { 6, 5, 1, 1 }, { 7, 5, 1, 1 },
	{ 8, 4, 1, 1 }, { 8, 5, 1, 1 }, { 9, 4, 1, 1 }, { 9, 5, 1, 1 },
	{ 10, 5, 1, 1 }, { 11, 4, 1, 1 }, { 11, 5, 1, 1 }, { 12, 4, 1, 1 },
	{ 12, 5, 1, 1 }, { 13, 4, 1, 1 }, { 13, 5, 1, 1 }, { 14, 5, 1, 1 },
	{ 15, 4, 1, 1 }, { 15, 5, 1, 1 }, { 16, 4, 1, 1 }, { 16, 5, 1, 1 },
	/* record, play, stop, left, right, shift */
	{ 0, 4, 1, 1 }, { 1, 4, 1, 1 }, { 2, 4, 1, 1 }, { 0, 5, 1, 1 },
	{ 1, 5, 1, 1 }, { 2, 5, 1, 1 },
	/* step 1 to 16 */
	{ 0, 3, 1, 1 }, { 1, 3, 1, 1 }, { 2, 3, 1, 1 }, { 3, 3, 1, 1 },
	{ 4, 3, 1, 1 }, { 5, 3, 1, 1 }, { 6, 3, 1, 1 }, { 7, 3, 1, 1 },
	{ 8, 3, 1, 1 }, { 9, 3, 1, 1 }, { 10, 3, 1, 1 }, { 11, 3, 1, 1 },
	{ 12, 3, 1, 1 }, { 13, 3, 1, 1 }, { 14, 3, 1, 1 }, { 15, 3, 1, 1 },
};

/* What is on the front but no key, drawn faintly: the speaker, the volume
 * knob and the display. */
static const struct cell marks[] = {
	{ 0, 0, 2, 2 }, { 2, 0, 2, 1 }, { 4, 0, 4, 2 },
};

/* S-15.062: a text line and a keys line. */
const char *const s15_keytest_lines[S15_KEYTEST_LINES] = {
	"Press each key and knob.",
	"`Shift` + `8`   Back to System",
};

static bool bit(const uint8_t *set, unsigned k)
{
	return (((unsigned)set[k / 8u] >> (k % 8u)) & 1u) != 0u;
}

static void set_bit(uint8_t *set, unsigned k, bool v)
{
	if (v)
		set[k / 8u] = (uint8_t)(set[k / 8u] | (1u << (k % 8u)));
	else
		set[k / 8u] = (uint8_t)(set[k / 8u] & ~(1u << (k % 8u)));
}

static bool any_down(const struct s15_keytest *kt)
{
	for (unsigned i = 0; i < sizeof kt->down; i++)
		if (kt->down[i] != 0u)
			return true;
	return false;
}

/* S-15.062 */
void s15_keytest_start(struct s15_keytest *kt, const struct wb_device *dev)
{
	kt->dev = dev;
	for (unsigned i = 0; i < sizeof kt->tested; i++) {
		kt->tested[i] = 0;
		kt->down[i] = 0;
	}
	kt->lone = -1;
	kt->n_tested = 0;
}

/* S-15.065, S-15.016: all the device's keys tested, or shift and digit 8
 * (0x37 and 0x15 on the OP-XY) down together. No other key ends it. */
static bool ended(const struct s15_keytest *kt)
{
	const struct wb_dev_keys *k = &kt->dev->keys;

	return kt->n_tested == k->count ||
	       (bit(kt->down, k->shift) && bit(kt->down, k->digit[7][0]));
}

/* S-15.064: a key is tested at its up when no other key went down from its
 * down to that up. Shift counts like every key. */
bool s15_keytest_event(struct s15_keytest *kt, unsigned key, bool down)
{
	if (key >= kt->dev->keys.count)
		return ended(kt);
	if (down) {
		kt->lone = any_down(kt) ? -1 : (int)key;
		set_bit(kt->down, key, true);
	} else {
		set_bit(kt->down, key, false);
		if (kt->lone == (int)key) {
			if (!bit(kt->tested, key)) {
				set_bit(kt->tested, key, true);
				kt->n_tested++;
			}
			kt->lone = -1;
		}
	}
	return ended(kt);
}

bool s15_keytest_tested(const struct s15_keytest *kt, unsigned key)
{
	return key < kt->dev->keys.count && bit(kt->tested, key);
}

/* Down, else tested, else untested; an up that did not test
 * the key returns it to the color it had. */
uint16_t s15_keytest_color(const struct s15_keytest *kt, unsigned key)
{
	if (key < S15_NKEYS && bit(kt->down, key))
		return S15_KT_DOWN;
	if (s15_keytest_tested(kt, key))
		return S15_KT_TESTED;
	return S15_KT_UNTESTED;
}

/* The fill inside each outline color. */
uint16_t s15_keytest_fill(uint16_t color)
{
	return color == S15_KT_DOWN ? S15_KT_DOWN_FILL :
	       color == S15_KT_TESTED ? S15_KT_TESTED_FILL :
	       S15_KT_UNTESTED_FILL;
}

/*
 * S-15.062: the grid, centered in the band below the two lines and above
 * the status bar, between the bars' edges. False if it leaves no room.
 */
static bool place(const struct s15_geometry *g, const struct cell *c,
		  struct s15_rect *r)
{
	uint32_t y0 = s15_row_y(g, S15_KEYTEST_LINES);
	uint32_t y1 = (uint32_t)g->status_y - 3u;
	uint32_t x0 = g->edge, x1 = (uint32_t)g->width - g->edge;
	uint32_t cw, ch;

	if (y1 <= y0 || x1 <= x0)
		return false;
	cw = (x1 - x0) / GRID_COLS;
	ch = (y1 - y0) / GRID_ROWS;
	if (cw < 4u || ch < 4u)
		return false;
	x0 += (x1 - x0 - cw * GRID_COLS) / 2u;
	y0 += (y1 - y0 - ch * GRID_ROWS) / 2u;
	/* One pixel of gap on each side keeps neighbors apart. */
	r->x = (uint16_t)(x0 + c->col * cw + 1u);
	r->y = (uint16_t)(y0 + c->row * ch + 1u);
	r->w = (uint16_t)(c->w * cw - 2u);
	r->h = (uint16_t)(c->h * ch - 2u);
	return true;
}

bool s15_keytest_box(const struct s15_geometry *g, unsigned key,
		     struct s15_rect *r)
{
	return key < S15_NKEYS && place(g, &cells[key], r);
}

/* S-15.062 */
void s15_render_keytest(const struct s15_keytest *kt,
			const struct s15_geometry *g,
			const struct s15_draw_ops *d,
			const struct s15_frame *f)
{
	struct s15_gray_draw gd;
	unsigned i;

	if (f->gray)                    /* S-15.173 */
		d = s15_gray_ops(&gd, d);
	d->fill(d->ctx, 0, 0, g->width, g->height, S15_BLACK);
	s15_render_frame(f, g, d);
	for (i = 0; i < S15_KEYTEST_LINES; i++) {
		const char *s = s15_keytest_lines[i];
		size_t n = 0;

		while (s[n] != '\0')
			n++;
		s15_render_row(g, d, i, i == 0u ? S15_ST_TEXT : S15_ST_KEYS,
			       s, n);
	}
	for (i = 0; i < sizeof marks / sizeof marks[0]; i++) {
		struct s15_rect r;

		if (!place(g, &marks[i], &r))
			continue;
		d->fill(d->ctx, r.x, r.y, r.w, 1, S15_RULE);
		d->fill(d->ctx, r.x, (uint16_t)(r.y + (r.h - 1u)), r.w, 1, S15_RULE);
		d->fill(d->ctx, r.x, r.y, 1, r.h, S15_RULE);
		d->fill(d->ctx, (uint16_t)(r.x + (r.w - 1u)), r.y, 1, r.h, S15_RULE);
	}
	for (i = 0; i < kt->dev->keys.count; i++) {
		struct s15_rect r;
		uint16_t c = s15_keytest_color(kt, i);

		if (!s15_keytest_box(g, i, &r))
			continue;
		/* A one-pixel outline in the color, then its fill. */
		d->fill(d->ctx, r.x, r.y, r.w, r.h, c);
		d->fill(d->ctx, (uint16_t)(r.x + 1u), (uint16_t)(r.y + 1u),
			(uint16_t)(r.w - 2u), (uint16_t)(r.h - 2u),
			s15_keytest_fill(c));
	}
}
