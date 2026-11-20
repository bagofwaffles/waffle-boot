/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 screen console and its frames (wiki/Screen-Style-Guide.md).
 */
#include "waffle/s15_console.h"

#include "waffle/bytes.h"

/* Required information, at the right end of the title bar's solid part. */
const char s15_indicator_text[] = "Protected writes on";

const char *const s15_style_names[S15_ST_COUNT] = {
	[S15_ST_TEXT] = "text",
	[S15_ST_DIM] = "dim",
	[S15_ST_HEAD] = "head",
	[S15_ST_KEYS] = "keys",
	[S15_ST_ITEM] = "item",
	[S15_ST_ITEM_ON] = "item-on",
	[S15_ST_ARMED] = "armed",
	[S15_ST_BACK] = "back",
	[S15_ST_OK] = "ok",
	[S15_ST_WARN] = "warn",
	[S15_ST_ERROR] = "error",
	[S15_ST_KV] = "kv",
	[S15_ST_STEP_RUN] = "step-run",
	[S15_ST_STEP_OK] = "step-ok",
	[S15_ST_STEP_SKIP] = "step-skip",
	[S15_ST_STEP_FAIL] = "step-fail",
	[S15_ST_PICK] = "pick",
	[S15_ST_PICK_ON] = "pick-on",
	[S15_ST_PICK_OFF] = "pick-off",
};

void s15_console_init(struct s15_console *con, struct s14_log *log)
{
	con->first = 0;
	con->count = 0;
	con->changes = 0;
	con->log = log;
}

/* Clearing removes every line. */
void s15_console_clear(struct s15_console *con)
{
	con->first = 0;
	con->count = 0;
	con->changes++;
}

/* The styles whose lines a tab splits into parts. */
static bool splits(enum s15_style style)
{
	switch (style) {
	case S15_ST_ITEM:
	case S15_ST_ITEM_ON:
	case S15_ST_ARMED:
	case S15_ST_BACK:
	case S15_ST_KV:
	case S15_ST_STEP_RUN:
	case S15_ST_STEP_OK:
	case S15_ST_STEP_SKIP:
	case S15_ST_STEP_FAIL:
	case S15_ST_PICK:
	case S15_ST_PICK_ON:
	case S15_ST_PICK_OFF:
		return true;
	case S15_ST_TEXT:
	case S15_ST_DIM:
	case S15_ST_HEAD:
	case S15_ST_KEYS:
	case S15_ST_OK:
	case S15_ST_WARN:
	case S15_ST_ERROR:
	case S15_ST_COUNT:
	default:
		return false;
	}
}

/* S-16.013: kept to S15_LINE characters; outside 0x20 - 0x7E
 * shown as '?', except the tabs of a line in a style they split. */
static void store(struct s15_console *con, unsigned slot,
		  enum s15_style style, const uint8_t *t, size_t len)
{
	size_t i, n = len < S15_LINE ? len : S15_LINE;

	for (i = 0; i < n; i++) {
		bool tab = t[i] == '\t' && splits(style);

		con->text[slot][i] = (char)(((t[i] >= 0x20u && t[i] <= 0x7eu) ||
					     tab) ? t[i] : '?');
	}
	con->len[slot] = (uint8_t)n;
	con->style[slot] = (uint8_t)style;
	con->changes++;
}

/* A 10th line pushes the oldest off the top. */
static void push(struct s15_console *con, enum s15_style style,
		 const uint8_t *text, size_t len)
{
	unsigned slot;

	if (con->count < S15_ROWS) {
		slot = ((unsigned)con->first + con->count) % S15_ROWS;
		con->count++;
	} else {
		slot = con->first;
		con->first = (uint8_t)((con->first + 1u) % S15_ROWS);
	}
	store(con, slot, style, text, len);
}

void s15_console_append_raw(struct s15_console *con, const uint8_t *text,
			    size_t len)
{
	push(con, S15_ST_TEXT, text, len);
}

/* Every appended line is also a level-3 log line of its full text,
 * before clipping: in a KEYS line and the styles a tab splits, the
 * backquotes left out and each tab two spaces. */
static void log_line(struct s15_console *con, enum s15_style style,
		     const uint8_t *text, size_t len)
{
	uint8_t buf[2u * S15_LINE];
	size_t i, n = 0;

	if (con->log == NULL)
		return;
	if (!splits(style) && style != S15_ST_KEYS) {
		s14_log_emit(con->log, S14_INFO, S14_TAG("screen"), text, len);
		return;
	}
	for (i = 0; i < len && n + 2u <= sizeof buf; i++) {
		if (text[i] == '`')
			continue;
		if (text[i] == '\t' && splits(style)) {
			buf[n++] = ' ';
			buf[n++] = ' ';
		} else {
			buf[n++] = text[i];
		}
	}
	s14_log_emit(con->log, S14_INFO, S14_TAG("screen"), buf, n);
}

void s15_console_add(struct s15_console *con, enum s15_style style,
		     const char *text)
{
	size_t n = wb_strlen(text);

	if ((unsigned)style >= S15_ST_COUNT)
		style = S15_ST_TEXT;
	log_line(con, style, (const uint8_t *)text, n);
	push(con, style, (const uint8_t *)text, n);
}

void s15_console_append(struct s15_console *con, const char *text)
{
	s15_console_add(con, S15_ST_TEXT, text);
}

/* Only the last row changes; on an empty console it appends.
 * Never logged. */
void s15_console_set(struct s15_console *con, enum s15_style style,
		     const char *text)
{
	size_t n = wb_strlen(text);

	if ((unsigned)style >= S15_ST_COUNT)
		style = S15_ST_TEXT;
	if (con->count == 0u) {
		push(con, style, (const uint8_t *)text, n);
		return;
	}
	store(con, ((unsigned)con->first + con->count - 1u) % S15_ROWS, style,
	      (const uint8_t *)text, n);
}

void s15_console_put(struct s15_console *con, unsigned i, enum s15_style style,
		     const char *text)
{
	if (i >= con->count)
		return;
	if ((unsigned)style >= S15_ST_COUNT)
		style = S15_ST_TEXT;
	store(con, (con->first + i) % S15_ROWS, style, (const uint8_t *)text,
	      wb_strlen(text));
}

const char *s15_console_row(const struct s15_console *con, unsigned i,
			    size_t *len)
{
	unsigned slot;

	if (i >= con->count) {
		*len = 0;
		return "";
	}
	slot = (con->first + i) % S15_ROWS;
	*len = con->len[slot];
	return con->text[slot];
}

enum s15_style s15_console_row_style(const struct s15_console *con,
				     unsigned i)
{
	if (i >= con->count)
		return S15_ST_TEXT;
	return (enum s15_style)con->style[(con->first + i) % S15_ROWS];
}

/* S-14.025: a level-0 line's body on the screen; not logged again
 * (S-15.004). */
void s15_console_sink(void *ctx, const uint8_t *body, size_t len)
{
	s15_console_append_raw(ctx, body, len);
}

/* ---- geometry ------------------------------------------------------- */

uint16_t s15_row_y(const struct s15_geometry *g, unsigned i)
{
	return (uint16_t)(g->top + i * g->pitch);
}

uint16_t s15_status_text_y(const struct s15_geometry *g)
{
	return (uint16_t)(g->status_y + 3u);
}

/* S-15.002, S-15.003 */
bool s15_geometry_ok(const struct s15_geometry *g)
{
	uint32_t right, rows_end;

	if (g->glyph_h == 0u || g->glyph_w == 0u || g->pitch < g->glyph_h ||
	    g->title_w == 0u ||
	    (uint32_t)g->title_y + g->title_h > g->bar_h ||
	    g->bar_h < S15_MARK_SIZE ||
	    (uint32_t)g->width < 2u * g->edge + S15_MARK_CLEAR +
	    S15_MARK_COUNT * (S15_MARK_SIZE + S15_MARK_GAP))
		return false;
	right = (uint32_t)g->margin + (uint32_t)S15_COLS * g->glyph_w;
	rows_end = (uint32_t)g->top + (S15_ROWS - 1u) * (uint32_t)g->pitch +
		   g->glyph_h;
	return right + g->edge <= g->width && g->top > g->bar_h &&
	       rows_end < g->status_y &&
	       (uint32_t)s15_status_text_y(g) + g->glyph_h <= g->height;
}

/* ---- drawing -------------------------------------------------------- */

static size_t chars(const char *s)
{
	return wb_strlen(s);
}

static uint16_t col_x(const struct s15_geometry *g, unsigned col)
{
	return (uint16_t)(g->margin + col * g->glyph_w);
}

/* Text from a column, clipped to the body's 56 columns. */
static void put(const struct s15_geometry *g, const struct s15_draw_ops *d,
		unsigned col, uint16_t y, const char *s, size_t n,
		uint16_t fg)
{
	if (col >= S15_COLS)
		return;
	if (n > S15_COLS - col)
		n = S15_COLS - col;
	if (n != 0u)
		d->text(d->ctx, col_x(g, col), y, s, n, fg, S15_BLACK);
}

/* Up to four tab-separated parts of s[0..n). */
struct parts {
	const char *p[4];
	size_t n[4];
	unsigned count;
};

static void split(const char *s, size_t n, struct parts *pt)
{
	size_t i, start = 0;

	pt->count = 0;
	for (i = 0; i <= n && pt->count < 4u; i++) {
		if (i == n || (s[i] == '\t' && pt->count < 3u)) {
			pt->p[pt->count] = s + start;
			pt->n[pt->count] = i - start;
			pt->count++;
			start = i + 1u;
		}
	}
}

/*
 * A key badge: the key's digit in bold black (its glyph drawn twice, one
 * pixel apart) on a solid cell BADGE_W x BADGE_H with its corner pixels
 * left out, centered on the digit's column: from BADGE_L pixels left of it
 * and a pixel above the row's top, over half of each blank column beside it.
 */
#define BADGE_W 16u
#define BADGE_H 16u
#define BADGE_L 4u

static void put_badge(const struct s15_geometry *g,
		      const struct s15_draw_ops *d, unsigned col, uint16_t y,
		      char digit, uint16_t bg)
{
	uint16_t x;

	if (col >= S15_COLS)
		return;
	x = col_x(g, col);
	d->fill(d->ctx, (uint16_t)(x - BADGE_L + 1u), (uint16_t)(y - 1u), BADGE_W - 2u, 1, bg);
	d->fill(d->ctx, (uint16_t)(x - BADGE_L), y, BADGE_W, BADGE_H - 2u, bg);
	d->fill(d->ctx, (uint16_t)(x - BADGE_L + 1u), (uint16_t)(y + BADGE_H - 2u), BADGE_W - 2u, 1,
		bg);
	if (d->ink != NULL) {
		d->ink(d->ctx, x, y, &digit, 1, S15_BLACK);
		d->ink(d->ctx, (uint16_t)(x + 1u), y, &digit, 1, S15_BLACK);
	} else {
		d->text(d->ctx, x, y, &digit, 1, S15_BLACK, bg);
	}
}

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

/* The columns s[0..n) takes: its backquotes take none. */
static size_t shown(const char *s, size_t n)
{
	size_t i, k = 0;

	for (i = 0; i < n; i++)
		k += s[i] != '`' ? 1u : 0u;
	return k;
}

/*
 * Text from a column in fg, its backquoted spans in span and the
 * backquotes not drawn; a span of one digit is a key badge in badge. The
 * badges come last, so that the text beside them leaves them whole.
 */
static void put_spans(const struct s15_geometry *g,
		      const struct s15_draw_ops *d, unsigned col, uint16_t y,
		      const char *s, size_t n, uint16_t fg, uint16_t span,
		      uint16_t badge)
{
	unsigned c = col;
	bool in = false;
	size_t i = 0;

	while (i < n) {
		size_t j = i;

		while (j < n && s[j] != '`')
			j++;
		if (!(in && j - i == 1u && is_digit(s[i])))
			put(g, d, c, y, s + i, j - i, in ? span : fg);
		c += (unsigned)(j - i);
		if (j < n)
			in = !in;
		i = j + 1u;
	}
	for (i = 0, c = col, in = false; i < n; i++) {
		if (s[i] == '`') {
			in = !in;
			continue;
		}
		if (in && i + 1u < n && s[i + 1u] == '`' && i > 0u &&
		    s[i - 1u] == '`' && is_digit(s[i]))
			put_badge(g, d, c, y, s[i], badge);
		c++;
	}
}

/* A KEYS line: gray, backquoted spans amber, a digit's a key badge. */
static void put_keys(const struct s15_geometry *g,
		     const struct s15_draw_ops *d, uint16_t y, const char *s,
		     size_t n)
{
	put_spans(g, d, 0, y, s, n, S15_DIM, S15_AMBER, S15_AMBER);
}

/* "D Label[\tHint]": the digit's key badge in column 0, the label from
 * column 3, the hint right-aligned with its spans as a KEYS line's. */
static void put_item(const struct s15_geometry *g,
		     const struct s15_draw_ops *d, uint16_t y, const char *s,
		     size_t n, uint16_t digit, uint16_t label, uint16_t hint)
{
	struct parts pt;
	size_t room = S15_COLS - 3u, hn = 0;

	split(s, n, &pt);
	if (pt.n[0] == 0u)
		return;
	/* a long label stops a column short of its hint */
	if (pt.count > 1u) {
		hn = shown(pt.p[1], pt.n[1]);
		if (hn > S15_COLS)
			hn = S15_COLS;
		room = hn + 4u < S15_COLS ? S15_COLS - 4u - hn : 0u;
	}
	if (pt.n[0] > 2u)
		put(g, d, 3, y, pt.p[0] + 2, pt.n[0] - 2u < room ? pt.n[0] - 2u
							       : room, label);
	if (pt.count > 1u)
		put_spans(g, d, (unsigned)(S15_COLS - hn), y, pt.p[1],
			  pt.n[1], hint, hint, digit);
	if (is_digit(pt.p[0][0]))
		put_badge(g, d, 0, y, pt.p[0][0], digit);
	else
		put(g, d, 0, y, pt.p[0], 1, digit);
}

/*
 * S-15.116's rows: in column S15_PICK_INDENT an amber key badge, or for
 * a dim row the digit plain; the label from S15_PICK_LABEL_COL, the
 * detail from S15_PICK_DETAIL_COL, each clipped a column short of what
 * follows; the state right-aligned, and in PICK_ON black on a solid red
 * block, so that Yes differs from No by more than its color.
 */
static void put_pick(const struct s15_geometry *g,
		     const struct s15_draw_ops *d, uint16_t y, const char *s,
		     size_t n, enum s15_style style)
{
	const bool dim = style == S15_ST_PICK_OFF;
	const uint16_t text = dim ? S15_DIM : S15_TEXT;
	struct parts pt;
	size_t sn = 0, room = S15_COLS - S15_PICK_DETAIL_COL;
	unsigned sc = S15_COLS;

	split(s, n, &pt);
	if (pt.n[0] == 0u)
		return;
	if (!dim && pt.count > 2u) {
		sn = pt.n[2] < S15_COLS - S15_PICK_DETAIL_COL ? pt.n[2] : 0u;
		sc = (unsigned)(S15_COLS - sn);
		room = sc > S15_PICK_DETAIL_COL + 1u ?
		       sc - S15_PICK_DETAIL_COL - 1u : 0u;
	}
	if (pt.n[0] > 2u)
		put(g, d, S15_PICK_LABEL_COL, y, pt.p[0] + 2, pt.n[0] - 2u <
		    S15_PICK_DETAIL_COL - S15_PICK_LABEL_COL - 1u ? pt.n[0] - 2u :
		    S15_PICK_DETAIL_COL - S15_PICK_LABEL_COL - 1u, text);
	if (pt.count > 1u)
		put(g, d, S15_PICK_DETAIL_COL, y, pt.p[1],
		    pt.n[1] < room ? pt.n[1] : room, text);
	if (sn != 0u && style == S15_ST_PICK_ON) {
		uint16_t x = col_x(g, sc);
		uint16_t w = (uint16_t)(sn * g->glyph_w + 2u * BADGE_L);

		d->fill(d->ctx, (uint16_t)(x - BADGE_L + 1u), (uint16_t)(y - 1u),
			(uint16_t)(w - 2u), 1, S15_RED);
		d->fill(d->ctx, (uint16_t)(x - BADGE_L), y, w, BADGE_H - 2u,
			S15_RED);
		d->fill(d->ctx, (uint16_t)(x - BADGE_L + 1u),
			(uint16_t)(y + BADGE_H - 2u), (uint16_t)(w - 2u), 1, S15_RED);
		d->text(d->ctx, x, y, pt.p[2], sn, S15_BLACK, S15_RED);
	} else if (sn != 0u) {
		put(g, d, sc, y, pt.p[2], sn, S15_DIM);
	}
	if (dim || !is_digit(pt.p[0][0]))
		put(g, d, S15_PICK_INDENT, y, pt.p[0], 1, S15_DIM);
	else
		put_badge(g, d, S15_PICK_INDENT, y, pt.p[0][0], S15_AMBER);
}

/* A mark in column 0 and a sentence from column 3. */
static void put_marked(const struct s15_geometry *g,
		       const struct s15_draw_ops *d, uint16_t y, char mark,
		       const char *s, size_t n, uint16_t c)
{
	put(g, d, 0, y, &mark, 1, c);
	put(g, d, 3, y, s, n, c);
}

#define STEP_COL S15_STEP_COL
#define BAR_COLS 25u

/* "<p>%" with p 0..100, or false. */
static bool percent(const char *s, size_t n, unsigned *p)
{
	unsigned v = 0;
	size_t i;

	if (n < 2u || n > 4u || s[n - 1u] != '%')
		return false;
	for (i = 0; i + 1u < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10u + (unsigned)(s[i] - '0');
	}
	*p = v > 100u ? 100u : v;
	return true;
}

static void put_step(const struct s15_geometry *g,
		     const struct s15_draw_ops *d, uint16_t y,
		     enum s15_style style, const char *s, size_t n)
{
	static const char marks[] = { '>', '+', '-', 'x' };
	static const uint16_t color[] = { S15_AMBER, S15_GREEN, S15_DIM,
					   S15_RED };
	unsigned k = (unsigned)style - (unsigned)S15_ST_STEP_RUN;
	struct parts pt;
	unsigned p;

	split(s, n, &pt);
	put(g, d, 0, y, &marks[k], 1, color[k]);
	put_spans(g, d, 3, y, pt.p[0], pt.n[0], S15_TEXT, S15_AMBER, S15_AMBER);
	if (pt.count < 2u)
		return;
	if (style == S15_ST_STEP_RUN && percent(pt.p[1], pt.n[1], &p)) {
		uint16_t x = col_x(g, STEP_COL);
		uint16_t w = (uint16_t)(BAR_COLS * g->glyph_w);
		uint16_t by = (uint16_t)(y + 3u);
		uint16_t bh = (uint16_t)(g->glyph_h - 6u);

		d->fill(d->ctx, x, by, w, 1, S15_AMBER);
		d->fill(d->ctx, x, (uint16_t)((unsigned)by + bh - 1u), w, 1,
			S15_AMBER);
		d->fill(d->ctx, x, by, 1, bh, S15_AMBER);
		d->fill(d->ctx, (uint16_t)((unsigned)x + w - 1u), by, 1, bh,
			S15_AMBER);
		d->fill(d->ctx, (uint16_t)(x + 2u), (uint16_t)(by + 2u),
			(uint16_t)((w - 4u) * p / 100u), (uint16_t)(bh - 4u),
			S15_AMBER);
		/* a third part stands after the bar in place of the percent */
		p = pt.count > 2u ? 2u : 1u;
		put(g, d, STEP_COL + BAR_COLS + 1u, y, pt.p[p], pt.n[p],
		    S15_AMBER);
		return;
	}
	put(g, d, STEP_COL, y, pt.p[1], pt.n[1], color[k]);
}

#define KV_VALUE 14u
#define KV_RIGHT 32u
#define KV_RIGHT_VALUE 43u

static void put_kv(const struct s15_geometry *g,
		   const struct s15_draw_ops *d, uint16_t y, const char *s,
		   size_t n)
{
	static const unsigned col[4] = { 0, KV_VALUE, KV_RIGHT,
					 KV_RIGHT_VALUE };
	struct parts pt;

	split(s, n, &pt);
	for (unsigned i = 0; i < pt.count; i++) {
		/* a key up to its value; the left value up to a right half */
		size_t w = i % 2u == 0u ? col[i + 1u] - col[i] - 1u
			 : i == 1u && pt.count > 2u ? KV_RIGHT - 1u - KV_VALUE
			 : pt.n[i];

		put(g, d, col[i], y, pt.p[i], pt.n[i] < w ? pt.n[i] : w,
		    i % 2u == 0u ? S15_DIM : S15_TEXT);
	}
}

void s15_render_row(const struct s15_geometry *g,
		    const struct s15_draw_ops *d, unsigned row,
		    enum s15_style style, const char *s, size_t n)
{
	uint16_t y = s15_row_y(g, row);

	switch (style) {
	case S15_ST_DIM:
		put(g, d, 0, y, s, n, S15_DIM);
		break;
	case S15_ST_HEAD:
		put(g, d, 0, y, s, n, S15_AMBER);
		break;
	case S15_ST_KEYS:
		put_keys(g, d, y, s, n);
		break;
	case S15_ST_ITEM:
		put_item(g, d, y, s, n, S15_AMBER, S15_TEXT, S15_DIM);
		break;
	case S15_ST_ITEM_ON:
		put_item(g, d, y, s, n, S15_AMBER, S15_TEXT, S15_RED);
		break;
	case S15_ST_ARMED:
		put_item(g, d, y, s, n, S15_RED, S15_RED, S15_RED);
		break;
	case S15_ST_BACK:
		put_item(g, d, y, s, n, S15_AMBER, S15_DIM, S15_DIM);
		break;
	case S15_ST_OK:
		put_marked(g, d, y, '+', s, n, S15_GREEN);
		break;
	case S15_ST_WARN:
		put_marked(g, d, y, '!', s, n, S15_AMBER);
		break;
	case S15_ST_ERROR:
		put_marked(g, d, y, '!', s, n, S15_RED);
		break;
	case S15_ST_KV:
		put_kv(g, d, y, s, n);
		break;
	case S15_ST_STEP_RUN:
	case S15_ST_STEP_OK:
	case S15_ST_STEP_SKIP:
	case S15_ST_STEP_FAIL:
		put_step(g, d, y, style, s, n);
		break;
	case S15_ST_PICK:
	case S15_ST_PICK_ON:
	case S15_ST_PICK_OFF:
		put_pick(g, d, y, s, n, style);
		break;
	case S15_ST_TEXT:
	case S15_ST_COUNT:
	default:
		put(g, d, 0, y, s, n, S15_TEXT);
		break;
	}
}

/* The title bar's group of squares, in the bar's dark color over the
 * bar: the first filled, the others outlined. */
static void logo_squares(const struct s15_draw_ops *d,
			 const struct s15_geometry *g, uint16_t dark)
{
	unsigned x = S15_MARK_X(g);
	unsigned y = ((unsigned)g->bar_h - S15_MARK_SIZE) / 2u;
	const unsigned n = S15_MARK_SIZE, t = S15_MARK_LINE;

	d->fill(d->ctx, (uint16_t)x, (uint16_t)y, (uint16_t)n, (uint16_t)n,
		dark);
	for (unsigned i = 1; i < S15_MARK_COUNT; i++) {
		x += S15_MARK_SIZE + S15_MARK_GAP;
		d->fill(d->ctx, (uint16_t)x, (uint16_t)y, (uint16_t)n,
			(uint16_t)t, dark);
		d->fill(d->ctx, (uint16_t)x, (uint16_t)(y + n - t), (uint16_t)n,
			(uint16_t)t, dark);
		d->fill(d->ctx, (uint16_t)x, (uint16_t)(y + t), (uint16_t)t,
			(uint16_t)(n - 2u * t), dark);
		d->fill(d->ctx, (uint16_t)(x + n - t), (uint16_t)(y + t),
			(uint16_t)t, (uint16_t)(n - 2u * t), dark);
	}
}

/* Text on a bar: over it, or on bg without the ink op. */
static void bar_text_n(const struct s15_draw_ops *d, uint16_t x, uint16_t y,
		       const char *s, size_t n, uint16_t fg, uint16_t bg)
{
	if (n == 0u)
		return;
	if (d->ink != NULL)
		d->ink(d->ctx, x, y, s, n, fg);
	else
		d->text(d->ctx, x, y, s, n, fg, bg);
}

static void bar_text(const struct s15_draw_ops *d, uint16_t x, uint16_t y,
		     const char *s, uint16_t fg, uint16_t bg)
{
	bar_text_n(d, x, y, s, chars(s), fg, bg);
}

/* The title bar's text: bold, or as the other bar's without the bold op. */
static void bar_bold(const struct s15_draw_ops *d, uint16_t x, uint16_t y,
		     const char *s, uint16_t fg, uint16_t bg)
{
	if (d->bold != NULL)
		d->bold(d->ctx, x, y, s, chars(s), fg);
	else
		bar_text(d, x, y, s, fg, bg);
}

/* ---- S-15.173: the grays -------------------------------------------- */

uint16_t s15_gray(uint16_t c)
{
	uint32_t r = (uint32_t)c >> 11, g = ((uint32_t)c >> 5) & 0x3fu,
		 b = (uint32_t)c & 0x1fu, y;

	if (r == b && r == g >> 1)
		return c;
	r = r * 8u + r / 4u;
	g = g * 4u + g / 16u;
	b = b * 8u + b / 4u;
	y = (77u * r + 150u * g + 29u * b + 128u) / 256u;
	return (uint16_t)((y / 8u) << 11 | (y / 4u) << 5 | y / 8u);
}

static void gray_fill(void *ctx, uint16_t x, uint16_t y, uint16_t w,
		      uint16_t h, uint16_t rgb)
{
	const struct s15_gray_draw *gd = ctx;

	gd->in->fill(gd->in->ctx, x, y, w, h, s15_gray(rgb));
}

static void gray_text(void *ctx, uint16_t x, uint16_t y, const char *s,
		      size_t n, uint16_t fg, uint16_t bg)
{
	const struct s15_gray_draw *gd = ctx;

	gd->in->text(gd->in->ctx, x, y, s, n, s15_gray(fg), s15_gray(bg));
}

static void gray_ink(void *ctx, uint16_t x, uint16_t y, const char *s,
		     size_t n, uint16_t fg)
{
	const struct s15_gray_draw *gd = ctx;

	gd->in->ink(gd->in->ctx, x, y, s, n, s15_gray(fg));
}

static void gray_bold(void *ctx, uint16_t x, uint16_t y, const char *s,
		      size_t n, uint16_t fg)
{
	const struct s15_gray_draw *gd = ctx;

	gd->in->bold(gd->in->ctx, x, y, s, n, s15_gray(fg));
}

const struct s15_draw_ops *s15_gray_ops(struct s15_gray_draw *gd,
					const struct s15_draw_ops *in)
{
	gd->in = in;
	gd->ops.fill = gray_fill;
	gd->ops.text = gray_text;
	gd->ops.ink = in->ink != NULL ? gray_ink : NULL;
	gd->ops.bold = in->bold != NULL ? gray_bold : NULL;
	gd->ops.ctx = gd;
	return &gd->ops;
}

static void frame(const struct s15_frame *f, const struct s15_geometry *g,
		  const struct s15_draw_ops *d);

void s15_render_frame(const struct s15_frame *f,
		      const struct s15_geometry *g,
		      const struct s15_draw_ops *d)
{
	struct s15_gray_draw gd;

	frame(f, g, f->gray ? s15_gray_ops(&gd, d) : d);
}

static void frame(const struct s15_frame *f, const struct s15_geometry *g,
		  const struct s15_draw_ops *d)
{
	bool red = f->option_on || f->fatal;
	uint16_t bg = red ? S15_RED : S15_AMBER;
	uint16_t ink = red ? S15_WHITE : S15_BLACK;
	uint16_t ty = g->title_y;
	uint16_t sy = s15_status_text_y(g);
	uint16_t x = g->edge;
	char slot[6] = "SLOT0";
	const char *w = "SLOT";
	uint16_t fg = S15_UNLIT;

	d->fill(d->ctx, 0, 0, g->width, g->bar_h, bg);
	logo_squares(d, g, red ? S15_RED_DARK : S15_AMBER_DARK);
	d->fill(d->ctx, 0, g->status_y, g->width,
		(uint16_t)((unsigned)g->height - g->status_y), S15_STATUS_BG);
	if (f->title != NULL)
		bar_bold(d, g->edge, ty, f->title, ink, bg);
	/* left of the squares, the bars' edge from the text part's end */
	if (f->option_on)
		bar_bold(d, (uint16_t)((size_t)S15_TEXT_W(g) - g->edge -
				       (sizeof s15_indicator_text - 1u) *
				       g->title_w),
			 ty, s15_indicator_text, ink, bg);
	/* The running slot by its number, gray; the bare word, unlit, when it
	 * is not established. USB amber while the disk or MTP is offered. */
	if (f->slot != S15_SLOT_UNKNOWN) {
		slot[4] = (char)('0' + s15_slot_number((unsigned)f->slot) % 10u);
		w = slot;
		fg = S15_DIM;
	}
	bar_text(d, x, sy, w, fg, S15_BLACK);
	x = (uint16_t)(x + (chars(w) + 2u) * g->glyph_w);
	bar_text(d, x, sy, "USB", (f->status & S15_STATUS_USB) != 0u ?
				  S15_AMBER : S15_UNLIT, S15_BLACK);
	{
		static const char name[] = "waffle-boot";
		size_t vn = f->version != NULL ? chars(f->version) : 0u;
		size_t total = sizeof name - 1u + (vn != 0u ? vn + 2u : 0u);
		uint16_t vx = (uint16_t)((size_t)g->width - g->edge -
					 total * g->glyph_w);

		bar_text(d, vx, sy, name, S15_DIM, S15_BLACK);
		if (vn != 0u) {
			vx = (uint16_t)(vx + sizeof name * g->glyph_w);
			bar_text_n(d, vx, sy, "v", 1, S15_FAINT, S15_BLACK);
			bar_text_n(d, (uint16_t)(vx + g->glyph_w), sy,
				   f->version, vn, S15_FAINT, S15_BLACK);
		}
	}
}

/* S-15.002 */
void s15_render_console(const struct s15_console *con,
			const struct s15_geometry *g,
			const struct s15_draw_ops *d,
			const struct s15_frame *f)
{
	struct s15_gray_draw gd;

	if (f->gray)                    /* S-15.173 */
		d = s15_gray_ops(&gd, d);
	d->fill(d->ctx, 0, 0, g->width, g->height, S15_BLACK);
	frame(f, g, d);
	/* the console's newest rows */
	{
		const unsigned skip = con->count > S15_ROWS ?
				      con->count - S15_ROWS : 0u;

		for (unsigned i = skip; i < con->count; i++) {
			size_t n;
			const char *s = s15_console_row(con, i, &n);

			if (n != 0u)
				s15_render_row(g, d, i - skip,
					       s15_console_row_style(con, i), s, n);
		}
	}
}

void s15_render_black(const struct s15_geometry *g,
		      const struct s15_draw_ops *d)
{
	d->fill(d->ctx, 0, 0, g->width, g->height, S15_BLACK);
}

unsigned s15_slot_number(unsigned index)
{
	return index + 1u;
}
