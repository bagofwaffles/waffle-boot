/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A PNG file to the boot logo's pixels (tools/wblogo.py's read_png()):
 * not interlaced, at most WB_LOGO_W x WB_LOGO_H, gray (1, 2, 4 or 8 bits),
 * gray with alpha, RGB or RGBA (8 bits), or a palette (1, 2, 4 or 8 bits);
 * each chunk's CRC checked; a translucent pixel blended onto black, each
 * channel then RGB565. The file is read through read() in pieces; its
 * image data goes through the tree's zlib (inflate's own state and window,
 * from the heap as for the zip reader, freed on return), and a row and the
 * row before it are kept here.
 */
#ifndef WAFFLE_FLASH_LOGO_PNG_H
#define WAFFLE_FLASH_LOGO_PNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum wb_png_rc {
	WB_PNG_OK,
	WB_PNG_READ,		/* the file could not be read */
	WB_PNG_NOT_PNG,		/* no PNG signature */
	WB_PNG_DAMAGED,		/* a chunk, its CRC or the image data is not right */
	WB_PNG_TYPE,		/* a color type or bit depth not taken (16 bits) */
	WB_PNG_INTERLACED,
	WB_PNG_SIZE		/* larger than WB_LOGO_W x WB_LOGO_H */
};

typedef bool (*wb_png_read_fn)(void *ctx, uint32_t off, uint8_t *buf, size_t n);

/* The size bytes of the file: px (WB_LOGO_W x WB_LOGO_H at least) gets the
 * pixels row by row, *w and *h the size. */
enum wb_png_rc wb_png_decode(wb_png_read_fn read, void *ctx, uint32_t size, uint16_t *px,
			     unsigned *w, unsigned *h);
/* A short reason for a result that is not WB_PNG_OK. */
const char *wb_png_why(enum wb_png_rc rc);

#endif /* WAFFLE_FLASH_LOGO_PNG_H */
