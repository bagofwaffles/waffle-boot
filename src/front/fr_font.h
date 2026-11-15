/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's copy of the display font (S-15.003), generated into
 * fr_font.c by tools/bdf2c.py --front: the same tables as the full
 * bootloader's wb_display_font and wb_display_title_font.
 */
#ifndef FR_FONT_INCLUDED
#define FR_FONT_INCLUDED

#include <stdint.h>

#define FR_FONT_FIRST 0x20u	/* printable ASCII, 0x20..0x7E */
#define FR_FONT_COUNT 95u
#define FR_FONT_W     8u	/* bit 7 the leftmost pixel */
#define FR_FONT_H     16u
#define FR_TITLE_W    10u	/* the title bar's glyphs: bit 9 the leftmost */
#define FR_TITLE_H    17u

extern const uint8_t fr_font[FR_FONT_COUNT][FR_FONT_H];
extern const uint16_t fr_title_font[FR_FONT_COUNT][FR_TITLE_H];

#endif /* FR_FONT_INCLUDED */
