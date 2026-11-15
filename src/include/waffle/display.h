/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The display for the board: S-15's draw interface over a frame buffer,
 * the font S-15.003 declares, the hand-over frame (S-15.132), and the
 * panel's calls (S-13c, src/s13c_panel.c) in the order a run makes them.
 *
 * S-15's draw interface takes screen positions (S-13c.023): x 0..479 from
 * the screen's left edge, y 0..221 from its top. Screen (x, y) is window
 * row 479 - x and window column y (panel column 49 + y), so a frame's
 * first row is the screen's right edge. The frame buffer holds the frame
 * window of S-13c.014 (224 columns by 480 rows) in S-13c.015's byte
 * order; its columns 222 and 223 (panel 271, 272) are not visible and
 * stay black. The hand-over frame (S-15.132) goes out row by row, not
 * through the frame buffer.
 *
 * The PSoC's brightness messages (S-13c.019) are S-07's and the board's:
 * 0x51 {00 00 00} before wb_display_start(), {3C FF 02} after it for held
 * mode, {3C FF 32} after wb_display_boot_logo(), {00 00 02} after
 * wb_display_poweroff().
 */
#ifndef WAFFLE_DISPLAY_H
#define WAFFLE_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s13c_panel.h"
#include "waffle/s15_console.h"

#define WB_DISPLAY_W 480u		/* screen x */
#define WB_DISPLAY_H 222u		/* screen y */

/* The font (S-15.003): Spleen 8x16, 0x20..0x7E, each 16 rows of 8
 * pixels, bit 7 the leftmost; rows 12 to 15 are below the baseline. */
#define WB_FONT_FIRST 0x20u
#define WB_FONT_COUNT 95u
#define WB_FONT_W     8u
#define WB_FONT_H     16u
extern const uint8_t wb_display_font[WB_FONT_COUNT][WB_FONT_H];
/* The title bar's font: the same glyphs bold, each 17 rows of 10 pixels,
 * bit 9 the leftmost; the last column stays clear. */
#define WB_TITLE_W    10u
#define WB_TITLE_H    17u
extern const uint16_t wb_display_title_font[WB_FONT_COUNT][WB_TITLE_H];

/* The frame (S-15.002): the title bar y 0 - 23, 9 body rows 19
 * apart from y 27 (the last ends at 195), x from 16 (56 glyphs end at
 * 464), the status bar's rule at y 202 and its text at y 205. */
extern const struct s15_geometry wb_display_geometry;

struct wb_display {
	struct s13c_panel panel;
	uint8_t *fb;			/* S13C_FRAME_BYTES */
	struct s15_draw_ops draw;
	bool stopped;			/* the fatal-error frame went out */
	bool dirty;			/* drawn since the last frame */
};

/*
 * S-15.132: an image for the hand-over frame in place of the logo, given
 * at build time (make image LOGO=<file>.png; tools/png2logo.py writes
 * it). data holds the visible pixels in the frame's order, window row 0
 * to 479 and in each its columns 0 to 221, as records: a byte h, then for
 * h < 0x80 h + 1 pixels, for h >= 0x80 one pixel standing for h - 0x7F;
 * a pixel is RGB565, high byte first. Pixels past the data are black.
 */
struct wb_display_image {
	const uint8_t *data;
	size_t len;
};

/* The build's image; data NULL when it was given none (the board's weak
 * default). */
extern const struct wb_display_image wb_display_boot_image;

/*
 * Ties d to its frame buffer (S13C_FRAME_BYTES, kept by the caller until
 * the run ends; DMA reads it) and clears it. Draws nothing on the panel.
 */
void wb_display_init(struct wb_display *d, uint8_t *fb);

/*
 * S-13c.004, S-13c.005: the panel's reset, from the first stage's
 * handshake (DMA9_XMOD, DMA9_YMOD as the bootloader found them) and the
 * PA8 strap level. Early, before any DMA9 write (S-13b.081); the bus
 * must be ready (the BF70x one: wb_display_bf70x_init()).
 */
void wb_display_begin(struct wb_display *d, const struct s13c_bus *bus,
		      uint32_t xmod, uint32_t ymod, unsigned pa8);
/* The panel as the front left it (s13c_adopt()). */
void wb_display_adopt(struct wb_display *d, const struct s13c_bus *bus,
		      uint32_t reset_ms);

/* S-13c.006 - S-13c.013: initialize and turn on (black); gamma on the
 * boot path. Once per run; later calls send nothing. */
int wb_display_start(struct wb_display *d, bool gamma);

/* S-15's draw interface on the frame buffer; each call first waits for a
 * frame in flight to end (S-13c.024), stopping one that has not ended
 * within S13C_FRAME_TIMEOUT_US, and marks the buffer drawn
 * (wb_display_update). */
const struct s15_draw_ops *wb_display_draw(struct wb_display *d);

/* Held mode (S-13c.017): the frame buffer as the next frame, after a TE
 * edge. Returns once it has started. */
int wb_display_flush(struct wb_display *d);

/* The frame buffer at once, ended before the return; no frame
 * follows it in this run. */
int wb_display_flush_fatal(struct wb_display *d);

bool wb_display_busy(struct wb_display *d);
/* Wait for a frame in flight to end, S13C_FRAME_TIMEOUT_US at most. */
int wb_display_wait(struct wb_display *d);

/* wb_display_flush() when anything was drawn since the last frame;
 * otherwise nothing (0). */
int wb_display_update(struct wb_display *d);

/* S-05.004, S-13c.020, S-15.132: the hand-over frame before the OS is
 * read: the logo alone, or image when it has data, with no text. Starts
 * the panel with gamma if not yet started; the frame buffer is not used. */
int wb_display_boot_logo(struct wb_display *d, const struct wb_display_image *image);

/* The same frame into frame (S13C_FRAME_BYTES): for pictures and tests, and
 * what the boot path leaves for the OS (S-05.016). */
void wb_display_handover_frame(const struct wb_display_image *image, uint8_t *frame);

/* S-13c.020, S-13c.021: at the kernel call; rec (the 44 bytes at
 * S01_DISPLAY_RECORD_START) gets the record. No transaction follows. */
int wb_display_handover(struct wb_display *d, uint8_t rec[S13C_RECORD_SIZE]);

/*
 * S-13c.021 without the hand-over: rec gets the record once the hand-over
 * frame has gone out (the state the record describes holds), and frames
 * may still follow (a failed load goes on to held mode, S-05.006). For a
 * board with no hook at the kernel call itself.
 */
int wb_display_record(struct wb_display *d, uint8_t rec[S13C_RECORD_SIZE]);

/*
 * S-13c.022: no new frame; the frame in progress ends, or is stopped
 * after S13C_POWEROFF_FRAME_US; nothing more is sent, no DISPOFF and no
 * last frame: the panel keeps the frame it shows.
 */
int wb_display_poweroff(struct wb_display *d);

#endif /* WAFFLE_DISPLAY_H */
