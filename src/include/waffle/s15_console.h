/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 screen console: the text lines the display shows, each in a line
 * style, and the frame they are drawn in, through a small draw interface
 * (S-15.002 - S-15.004;
 * wiki/Screen-Style-Guide.md).
 *
 * The geometry and glyphs are the display's (S-13c) and come in as
 * parameters; no display driver is used here.
 */
#ifndef WAFFLE_S15_CONSOLE_H
#define WAFFLE_S15_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s14_log.h"

#define S15_ROWS 9u      /* Body rows shown */
#define S15_COLS 56u     /* Body columns */
/* A step row's label starts in column 3; its bar or result starts one
 * column after the longest label. */
#define S15_STEP_LABEL_MAX 18u
#define S15_STEP_COL (3u + S15_STEP_LABEL_MAX + 1u)
#define S15_LINE 80u     /* characters a line keeps, tabs included */

/* RGB565 colors (wiki/Screen-Style-Guide.md) */
#define S15_BLACK 0x0000u
#define S15_WHITE 0xffffu
#define S15_TEXT  0xef5du    /* E8E8E8 */
#define S15_DIM   0x8c71u    /* 8C8C8C */
#define S15_FAINT 0x5acbu    /* 5A5A5A */
#define S15_UNLIT 0x4a49u    /* 484848 */
#define S15_RULE  0x39c7u    /* 383838 */
#define S15_AMBER 0xfd84u    /* FFB020 */
#define S15_RED   0xfa88u    /* FF5040 */
#define S15_GREEN 0x56ceu    /* 50D870 */
#define S15_AMBER_DARK 0x82c2u /* 805810: the hollow squares on amber */
#define S15_RED_DARK   0x8103u /* 802018: the hollow squares on red */
#define S15_STATUS_BG  0x2124u /* 212421: the status bar */

/*
 * The title bar: solid in its color, then black, and at its right the
 * boot logo's group of four squares, S15_MARK_SIZE pixels each and
 * S15_MARK_GAP apart, the last one the bars' edge from the right side.
 * The first is filled with the bar's color; the others are outlines
 * S15_MARK_LINE thick in its dark color. The solid part ends
 * S15_MARK_CLEAR pixels left of the first square.
 */
#define S15_MARK_COUNT 4u
#define S15_MARK_SIZE  14u
#define S15_MARK_GAP   8u
#define S15_MARK_LINE  2u
#define S15_MARK_CLEAR 10u
/* x of the first square, and the solid part's width, for a geometry. */
#define S15_MARK_X(g) ((unsigned)(g)->width - (g)->edge - \
		       S15_MARK_COUNT * S15_MARK_SIZE - \
		       (S15_MARK_COUNT - 1u) * S15_MARK_GAP)
#define S15_BAR_W(g) (S15_MARK_X(g) - S15_MARK_CLEAR + 1u)

/* The line styles; s15_style_names[] holds their names. A tab
 * separates a line's parts; in S15_ST_KEYS a backquote starts and ends an
 * amber span. */
enum s15_style {
	S15_ST_TEXT,
	S15_ST_DIM,
	S15_ST_HEAD,
	S15_ST_KEYS,
	S15_ST_ITEM,        /* "D Label" or "D Label\tHint" */
	S15_ST_ITEM_ON,
	S15_ST_ARMED,
	S15_ST_BACK,
	S15_ST_OK,
	S15_ST_WARN,
	S15_ST_ERROR,
	S15_ST_KV,          /* "Key\tValue[\tKey\tValue]" */
	S15_ST_STEP_RUN,    /* "Label\tStatus" */
	S15_ST_STEP_OK,
	S15_ST_STEP_SKIP,
	S15_ST_STEP_FAIL,
	S15_ST_COUNT
};
extern const char *const s15_style_names[S15_ST_COUNT];

struct s15_console {
	char text[S15_ROWS][S15_LINE];
	uint8_t len[S15_ROWS];
	uint8_t style[S15_ROWS];
	uint8_t first;     /* ring index of the oldest shown line */
	uint8_t count;     /* lines shown, 0..9 */
	uint32_t changes;  /* bumped on every change, for frame pacing */
	struct s14_log *log;  /* lines are logged here; may be NULL */
};

void s15_console_init(struct s15_console *con, struct s14_log *log);
void s15_console_clear(struct s15_console *con);
/* Append a line in a style and log its text at level 3. */
void s15_console_add(struct s15_console *con, enum s15_style style,
		     const char *text);
/* s15_console_add() in S15_ST_TEXT. */
void s15_console_append(struct s15_console *con, const char *text);
/* Append without logging: for the S-14.025 screen sink only. */
void s15_console_append_raw(struct s15_console *con, const uint8_t *text,
			    size_t len);
/* Replace the last line and its style, not logged; appends on an empty
 * console. */
void s15_console_set(struct s15_console *con, enum s15_style style,
		     const char *text);
/* Shown row i (0: top): its text, tabs and backquotes included. */
const char *s15_console_row(const struct s15_console *con, unsigned i,
			    size_t *len);
enum s15_style s15_console_row_style(const struct s15_console *con,
				     unsigned i);

/* The S-14.025 screen sink, writing to a console (ctx: the console). */
void s15_console_sink(void *ctx, const uint8_t *body, size_t len);

/*
 * The frame's geometry (S-15.002, S-15.003), in screen positions
 * (S-13c.023): x from the left, y from the top; the draw implementation
 * maps it to the panel's frame window (S-13c.014, S-13c.015).
 */
struct s15_geometry {
	uint16_t width, height;   /* visible area */
	uint16_t edge;            /* x of the bars' text */
	uint16_t margin;          /* x of a body row's column 0 */
	uint16_t top;             /* y of body row 0 */
	uint16_t pitch;           /* distance between body rows */
	uint16_t glyph_w, glyph_h;
	uint16_t bar_h;           /* the title bar's height */
	uint16_t status_y;        /* y of the status bar's rule */
	uint16_t title_w, title_h; /* the title bar's glyphs */
	uint16_t title_y;         /* y of the title bar's text */
};

/* Whether the title bar, 9 rows of 56 glyphs and the status bar fit the
 * visible area without overlapping (S-15.002, S-15.003). */
bool s15_geometry_ok(const struct s15_geometry *g);
/* y of body row i, and of the status bar's text. */
uint16_t s15_row_y(const struct s15_geometry *g, unsigned i);
uint16_t s15_status_text_y(const struct s15_geometry *g);

struct s15_draw_ops {
	void (*fill)(void *ctx, uint16_t x, uint16_t y, uint16_t w,
		     uint16_t h, uint16_t rgb565);
	/* n glyphs from (x, y), left to right, in fg on bg. */
	void (*text)(void *ctx, uint16_t x, uint16_t y, const char *s,
		     size_t n, uint16_t fg, uint16_t bg);
	void *ctx;
	/* n glyphs from (x, y) in fg, the pixels between their strokes left
	 * as they are: the bars' text on the bars. Optional:
	 * without it the bars' text is drawn by text on the bar's base
	 * color. */
	void (*ink)(void *ctx, uint16_t x, uint16_t y, const char *s,
		    size_t n, uint16_t fg);
	/* As ink, in the title bar's glyphs (S-15.003), title_w apart.
	 * Optional: without it that text is drawn as the other bar's. */
	void (*bold)(void *ctx, uint16_t x, uint16_t y, const char *s,
		     size_t n, uint16_t fg);
};

/* The number a person reads for a slot: index 0 is slot 1 (M1), index 1
 * is slot 2 (M2) (S-15 terms). Flash and the wire keep the index. */
unsigned s15_slot_number(unsigned index);

/* The status words of the status bar. */
#define S15_STATUS_USB  0x01u
#define S15_SLOT_UNKNOWN 0xffu   /* the running slot is not established */

/* What the title bar and the status bar show. */
struct s15_frame {
	const char *title;     /* the active screen, title case */
	const char *version;   /* waffle-boot's own version text */
	uint8_t status;        /* S15_STATUS_* */
	uint8_t slot;          /* the running slot's index, or S15_SLOT_UNKNOWN */
	bool option_on;        /* Red bar, "Protected writes on" */
	bool fatal;            /* Red bar */
	bool gray;             /* S-15.173: every pixel in its gray */
};

/* S-15.173: the gray an RGB565 color is drawn in while the color is
 * off; a color already gray (r = b = g / 2) is its own. */
uint16_t s15_gray(uint16_t rgb565);

/* S-15.173: draw ops that draw through in, each color in its gray.
 * Fill a struct s15_gray_draw and draw with its ops. */
struct s15_gray_draw {
	struct s15_draw_ops ops;
	const struct s15_draw_ops *in;
};
const struct s15_draw_ops *s15_gray_ops(struct s15_gray_draw *gd,
					const struct s15_draw_ops *in);

extern const char s15_indicator_text[];

/* The title bar and the status bar alone. */
void s15_render_frame(const struct s15_frame *f,
		      const struct s15_geometry *g,
		      const struct s15_draw_ops *d);

/* One body row: text in a style. */
void s15_render_row(const struct s15_geometry *g,
		    const struct s15_draw_ops *d, unsigned row,
		    enum s15_style style, const char *s, size_t n);

/*
 * A frame of the console (S-15.002), also the fatal-error frame: black,
 * the frame, and the shown rows.
 */
void s15_render_console(const struct s15_console *con,
			const struct s15_geometry *g,
			const struct s15_draw_ops *d,
			const struct s15_frame *f);

/* Every pixel black (S-15.065's cleared screen). */
void s15_render_black(const struct s15_geometry *g,
		      const struct s15_draw_ops *d);

#endif /* WAFFLE_S15_CONSOLE_H */
