/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What the front shows: the boot logo, with no text, and a text screen in
 * held mode's frame (a solid title bar ending in four squares, nine body
 * rows of 8 x 16 glyphs, the solid status bar), in red, for recovery.
 * Drawn a window row at a time, with no frame buffer, by fr_screen_row()
 * as fr_panel_frame()'s row source.
 */
#ifndef FR_DRAW_H
#define FR_DRAW_H

#include "fr.h"

#define FR_SCREEN_W   480u	/* visible screen, x from the left */
#define FR_SCREEN_H   222u	/* y from the top */
#define FR_LINES      9u	/* body rows */
#define FR_COLS       56u	/* glyphs per body row */
#define FR_TITLE_MAX  28u
#define FR_STATUS_MAX 40u

enum fr_kind { FR_SCREEN_LOGO, FR_SCREEN_TEXT };
enum fr_ink { FR_INK_TEXT, FR_INK_DIM, FR_INK_HEAD, FR_INK_OK, FR_INK_WARN, FR_INK_ERROR };

struct fr_screen {
	uint8_t kind;			/* enum fr_kind */
	char title[FR_TITLE_MAX + 1];
	char line[FR_LINES][FR_COLS + 1];
	uint8_t ink[FR_LINES];		/* enum fr_ink */
	int8_t bar_line;		/* a progress bar on this row, -1 none */
	uint8_t bar_col;		/* its first glyph column */
	uint8_t bar_cols;		/* its width in glyphs */
	uint8_t bar_pct;		/* 0..100 */
	char status_left[FR_STATUS_MAX + 1];	/* e.g. "USB" */
	char status_right[FR_STATUS_MAX + 1];	/* e.g. "waffle-boot v1.0.0" */
};

/* An empty text screen; the logo screen, the logo alone, as the hand-over
 * frame draws it (S-15.132). */
void fr_screen_text(struct fr_screen *s, const char *title);
void fr_screen_logo(struct fr_screen *s);
/* Body row i's text and ink (cut to FR_COLS), the status texts. */
void fr_screen_line(struct fr_screen *s, unsigned i, enum fr_ink ink, const char *text);
void fr_screen_status(struct fr_screen *s, const char *left, const char *right);

/* The RGB565 pixel at screen (x, y): a pixel at a time, the model the
 * window rows are tested against and the screenshots' source. */
uint16_t fr_screen_pixel(const struct fr_screen *s, unsigned x, unsigned y);
/* Window row r (screen column 479 - r, S-13c.023): FR_PANEL_COLS pixels,
 * top down, big-endian RGB565, black past the visible 222. ctx is the
 * struct fr_screen. Row 0 measures its texts, which later rows reuse:
 * rows come from 0 up, as fr_panel_frame() asks for them. */
void fr_screen_row(void *ctx, unsigned r, uint8_t *out);

/* Text helpers for the screens, no NUL written: a decimal, and a version
 * as S-10c.055 writes it after a "v": "v1.2.3", or "v1.2.3+4" when the
 * build is not 0 (at most FR_VERSION_TEXT_MAX characters). */
#define FR_VERSION_TEXT_MAX 24u	/* "v65535.65535.65535+65535" */
size_t fr_text_dec(char *out, uint32_t v);
size_t fr_text_version(char *out, const uint16_t v[4]);

#endif /* FR_DRAW_H */
