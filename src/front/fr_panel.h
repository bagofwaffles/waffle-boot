/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The panel (S-13c), the front's own copy of what the start-up and
 * recovery need: the reset decision, the run's one initialization with the
 * gamma load, and whole frames sent by CPU writes. Every wait is bounded,
 * and the bus is write only, so a dead panel delays nothing.
 */
#ifndef FR_PANEL_H
#define FR_PANEL_H

#include "fr.h"

#define FR_PANEL_ROWS      480u	/* window rows: screen columns 479..0 (S-13c.023) */
#define FR_PANEL_COLS      224u	/* frame window columns 49-272 (S-13c.014) */
#define FR_PANEL_VISIBLE   222u	/* columns 49-270 are visible (S-13c.010) */
#define FR_PANEL_ROW_BYTES (2u * FR_PANEL_COLS)

/* S-13c.004's three reset paths. */
#define FR_PANEL_HANDSHAKE 0u
#define FR_PANEL_PULSE     1u
#define FR_PANEL_STRAP     2u

struct fr_panel {
	const struct fr_panel_bus *bus;
	const struct fr_clock *clock;
	uint64_t ready_us;	/* S-13c.006: the first transaction's earliest time */
	uint32_t reset_ms;	/* S-13c.021's reset time */
	uint8_t path;		/* FR_PANEL_* */
	bool ready;		/* initialized and the gamma loaded: frames are sent */
	bool on;		/* DISPON sent, after the first frame's last byte */
	bool started;		/* the run's one initialization began (S-13c.013) */
};

/* S-13c.004, S-13c.005: from the first stage's DMA9 values and the PA8
 * strap sample (1: PA8 high), at once; a pulse is 20 us. */
void fr_panel_begin(struct fr_panel *p, const struct fr_panel_bus *bus,
		    const struct fr_clock *clock, uint32_t xmod, uint32_t ymod,
		    unsigned pa8);
/* S-13c.006-S-13c.013: waits until ready_us (at most S-13c's 120 ms from
 * the reset's release), SLPOUT and 5.1 ms, the init sequence, the gamma
 * load and its window, the frame window. The display stays off. Once per
 * run. */
void fr_panel_start(struct fr_panel *p);
/* S-13c.016: one whole frame, row r = 0..479 from row(); nothing when the
 * panel is not ready. The first frame writes every pixel of the visible
 * window with the display off, and DISPON follows its last byte
 * (S-13c.010): the frame shows whole, with no fill before it. */
void fr_panel_frame(struct fr_panel *p,
		    void (*row)(void *ctx, unsigned r, uint8_t *out), void *ctx);
/* S-13c.022: at a power-off, no frame after it and nothing sent. */
void fr_panel_off(struct fr_panel *p);

#endif /* FR_PANEL_H */
