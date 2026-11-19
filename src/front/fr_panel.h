/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The panel (S-13c), the front's own copy of what the start-up and
 * recovery need: the reset decision, the run's one initialization with the
 * gamma load, and whole frames from a frame buffer in bus order, each by
 * one memory DMA started on a TE rising edge, as held mode sends them
 * (S-13c.016 - S-13c.018), or by CPU writes after a DMA send that did not
 * end. Every wait is bounded, and the bus is write only, so a dead panel
 * delays nothing.
 */
#ifndef FR_PANEL_H
#define FR_PANEL_H

#include "fr.h"

#define FR_PANEL_ROWS      480u	/* window rows: screen columns 479..0 (S-13c.023) */
#define FR_PANEL_COLS      224u	/* frame window columns 49-272 (S-13c.014) */
#define FR_PANEL_VISIBLE   222u	/* columns 49-270 are visible (S-13c.010) */
#define FR_PANEL_ROW_BYTES (2u * FR_PANEL_COLS)
#define FR_PANEL_FRAME_BYTES (FR_PANEL_ROWS * FR_PANEL_ROW_BYTES)	/* 215,040 */
/* S-13c.025: the logo line's band, columns 189-204, rows 0-479. */
#define FR_PANEL_LINE_COL   189u
#define FR_PANEL_LINE_COLS  16u
#define FR_PANEL_LINE_BYTES (FR_PANEL_ROWS * 2u * FR_PANEL_LINE_COLS)	/* 15,360 */

/* S-13c.017: the wait for a TE rising edge (T_TE_MISS), a DMA send's
 * longest time before it is stopped, and a frame's at a power-off. */
#define FR_PANEL_TE_US   50000u
#define FR_PANEL_DMA_US  40000u
#define FR_PANEL_OFF_US  60000u

/* What a DMA send is sending. */
enum fr_panel_out { FR_OUT_NONE, FR_OUT_FRAME, FR_OUT_LINE };

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
	uint8_t te_misses;	/* frames sent without a TE edge, saturating */
	bool cpu;		/* a DMA send was stopped: every send by the CPU */
	uint8_t out;		/* enum fr_panel_out: a DMA send under way */
	const uint8_t *out_buf;	/* what it sends */
	uint64_t out_at;	/* when it started */
	bool stopped;		/* fr_panel_watch() stopped it: sent again by the CPU */
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
/*
 * S-13c.016, S-13c.017: one whole frame from buf (FR_PANEL_FRAME_BYTES in
 * bus order); nothing when the panel is not ready. The first frame, the
 * early logo frame, goes with the display off and no TE wait, its end
 * awaited, and DISPON follows its last byte (S-13c.010). Every later frame
 * starts once the previous one has ended and on a TE rising edge, or
 * FR_PANEL_TE_US without one (counted in te_misses), and is left going
 * out: buf is not written until fr_panel_busy() says it has ended
 * (S-13c.024).
 */
void fr_panel_frame(struct fr_panel *p, const uint8_t *buf);
/* S-13c.025: the logo line from band (FR_PANEL_LINE_BYTES), paced as a
 * frame, after the display is on: CASET to its band, RASET, RAMWR, its
 * bytes, then the frame window's CASET again. */
void fr_panel_line(struct fr_panel *p, const uint8_t *band);
/* A send is going out. A DMA send that has not ended FR_PANEL_DMA_US after
 * its start, or that failed, is stopped and sent again whole by the CPU,
 * paced, as is every later one (S-13c.017). */
bool fr_panel_busy(struct fr_panel *p);
/* S-13c.017: a DMA send not ended FR_PANEL_DMA_US after its start is
 * stopped here, at once; the CPU sends it again at the next
 * fr_panel_busy() or wait. Sends nothing and never waits: for the long
 * loops' link hook. */
void fr_panel_watch(struct fr_panel *p);
/* Until no send is going out; bounded, and the link served meanwhile. */
void fr_panel_idle(struct fr_panel *p);
/* S-13c.022: at a power-off a send going out is left to end, at most
 * FR_PANEL_OFF_US, and then stopped; no frame after it and nothing sent. */
void fr_panel_off(struct fr_panel *p);
/* Before the jump to the main image: no send going out, and the DMA's
 * channels disabled with their status cleared. */
void fr_panel_end(struct fr_panel *p);

#endif /* FR_PANEL_H */
