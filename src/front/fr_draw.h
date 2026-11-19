/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What the front shows: the boot logo, with no text, the line under it,
 * and a text screen in held mode's frame (a solid title bar ending in four
 * squares, nine body rows, the solid status bar), in red, for recovery.
 * Painted into a frame buffer in the panel's bus order (S-13c.015,
 * S-13c.023) by fr_paint(), which fr_panel_frame() then sends.
 */
#ifndef FR_DRAW_H
#define FR_DRAW_H

#include "fr.h"

#define FR_SCREEN_W   480u	/* visible screen, x from the left */
#define FR_SCREEN_H   222u	/* y from the top */
#define FR_LINES      9u	/* body rows */
#define FR_COLS       56u	/* 8 x 16 glyphs a body row shows */
#define FR_LINE_MAX   64u	/* a body row's bytes, its control bytes included */
#define FR_TITLE_MAX  28u
#define FR_STATUS_MAX 40u
#define FR_CLIP_X     472u	/* nothing of a body row is drawn past it */

/*
 * A body row's control bytes, below the font's first glyph: each takes
 * effect from where it stands, and an item that would cross FR_CLIP_X ends
 * the row there (nothing wraps).
 *   FR_C_INK a      the ink from here: a is FR_A_* (1 + enum fr_ink)
 *   FR_C_BADGE d    a key badge with the digit d
 *   FR_C_CHIP t     a filled chip: text t in TEXT on the status bar's gray
 *   FR_C_RING a t   an outlined chip: its outline and text t in ink a
 *   FR_C_END        the end of a chip
 *   FR_C_TITLE      the title bar's font from here; FR_C_BODY the 8 x 16
 * Every other byte is a glyph ('?' for one the font does not have).
 */
#define FR_C_INK    "\x01"
#define FR_C_BADGE  "\x02"
#define FR_C_CHIP   "\x03"
#define FR_C_END    "\x04"
#define FR_C_RING   "\x05"
#define FR_C_TITLE  "\x06"
#define FR_C_BODY   "\x07"
#define FR_A_TEXT   "\x01"
#define FR_A_DIM    "\x02"
#define FR_A_HEAD   "\x03"
#define FR_A_OK     "\x04"
#define FR_A_WARN   "\x05"
#define FR_A_ERROR  "\x06"

enum fr_kind { FR_SCREEN_LOGO, FR_SCREEN_TEXT };
enum fr_ink { FR_INK_TEXT, FR_INK_DIM, FR_INK_HEAD, FR_INK_OK, FR_INK_WARN, FR_INK_ERROR };

struct fr_screen {
	uint8_t kind;			/* enum fr_kind */
	char title[FR_TITLE_MAX + 1];
	char line[FR_LINES][FR_LINE_MAX + 1];
	uint8_t ink[FR_LINES];		/* enum fr_ink, each row's first */
	int8_t bar_line;		/* a progress bar on this row, -1 none */
	uint8_t bar_col;		/* its first glyph column */
	uint8_t bar_cols;		/* its width in glyphs */
	uint16_t bar_pm;		/* its fill, 0..1000 */
	char status_left[FR_STATUS_MAX + 1];	/* e.g. "USB" */
	char status_right[FR_STATUS_MAX + 1];	/* e.g. "waffle-boot v1.0.0" */
};

/* An empty text screen; the logo screen, the logo alone, as the hand-over
 * frame draws it (S-15.132). */
void fr_screen_text(struct fr_screen *s, const char *title);
void fr_screen_logo(struct fr_screen *s);
/* Body row i's bytes and first ink (cut to FR_LINE_MAX), the status texts. */
void fr_screen_line(struct fr_screen *s, unsigned i, enum fr_ink ink, const char *text);
void fr_screen_status(struct fr_screen *s, const char *left, const char *right);

/* The frame buffer: FR_PANEL_ROWS window rows of FR_PANEL_COLS pixels,
 * high byte first; window row r is screen column 479 - r, top down. */
#define FR_FRAME_BYTES (480u * 224u * 2u)
/* The whole screen into buf. */
void fr_paint(uint8_t *buf, const struct fr_screen *s);
/* buf holds prev: only the title bar, the status bar and the body rows
 * that differ are painted again. prev NULL: the whole screen. */
void fr_paint_rows(uint8_t *buf, const struct fr_screen *s, const struct fr_screen *prev);

/* The logo line (S-13c.025): screen y FR_LINE_Y to FR_LINE_Y + 15 of
 * every screen column, window row r (screen x 479 - r) by window row, text
 * centered in dim gray on black. */
#define FR_LINE_Y      140u
#define FR_LINE_BYTES  (480u * 16u * 2u)
void fr_paint_line(uint8_t *band, const char *text);

#if !defined(__bfin__)
/* The host's model, a pixel at a time, that the painter is tested against
 * and the screenshots' source: the RGB565 pixel at screen (x, y), and the
 * logo line's at screen x and FR_LINE_Y + dy. */
uint16_t fr_screen_pixel(const struct fr_screen *s, unsigned x, unsigned y);
uint16_t fr_line_pixel(const char *text, unsigned x, unsigned dy);
#endif

/* Text helpers for the screens, no NUL written: a decimal, and a version
 * as S-10c.055 writes it after a "v": "v1.2.3", or "v1.2.3+4" when the
 * build is not 0 (at most FR_VERSION_TEXT_MAX characters). */
#define FR_VERSION_TEXT_MAX 24u	/* "v65535.65535.65535+65535" */
size_t fr_text_dec(char *out, uint32_t v);
size_t fr_text_version(char *out, const uint16_t v[4]);

#endif /* FR_DRAW_H */
