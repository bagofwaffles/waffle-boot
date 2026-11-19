/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The master's start-up in the front, from the stream's entry to the key
 * read, and what follows it: the full bootloader or recovery (S-01.023,
 * S-07.023, S-02.080, S-02.084). Portable: the hardware comes in
 * through struct fr_start_hw, so the host's tests drive it with fakes.
 */
#ifndef FR_START_H
#define FR_START_H

#include "fr.h"
#include "fr_draw.h"
#include "fr_handoff.h"
#include "fr_image.h"
#include "fr_panel.h"
#include "fr_proto.h"
#include "fr_psoc.h"
#include "fr_slots.h"

/* S-07.021: the longest wait for a PSoC that did not answer the greetings. */
#define FR_GREET_WAIT_US 1500000u
/* S-02.084: how long key 1 is held to confirm an action on the unit. */
#define FR_HOLD_US       5000000u
/* Recovery reads the keys this often; after a reading that did not come,
 * this often. */
#define FR_KEYS_GAP_US   50000u
#define FR_KEYS_MISS_US  500000u
/* Recovery redraws its screen, a progress bar's fill among it, at most this
 * often, and only when no frame is going out (S-13c.024). */
#define FR_DRAW_GAP_US   40000u
/* While it holds the power-off after a read that failed, recovery reads
 * the selector and the slots again this often (S-02.084). */
#define FR_RESURVEY_US   5000000u

struct fr_start_hw {
	const struct fr_clock *clock;
	const struct fr_bytes *psoc;
	const struct fr_panel_bus *panel;	/* NULL: no panel */
	const struct fr_flash *flash;
	const struct fr_mem *mem;
	/* recovery's USB serial port */
	void (*usb_start)(const struct fr_clock *clock);
	void (*usb_poll)(void);
	bool (*usb_configured)(void);
	uint32_t (*usb_ma)(void);
	const struct fr_bytes *serial;
	uint8_t *staging;			/* FR_SLOT_CAP bytes */
	uint8_t *frame;				/* FR_PANEL_FRAME_BYTES: the panel's
						 * frame buffer (S-01.023) */
};

enum fr_next {
	FR_NEXT_MAIN,		/* the image checked: jump to desc.main_entry */
	FR_NEXT_RECOVERY,	/* Power + M4, a failed image check, or three starts
				 * that did not finish (s->why) */
	FR_NEXT_HALT		/* powered off, or restarted: wait for the end */
};

/* Recovery's screens with keys (fr_recui.c). */
enum fr_ui_screen { FR_UI_MAIN, FR_UI_SLOTS, FR_UI_CONFIRM };

struct fr_ui {
	uint8_t screen;		/* enum fr_ui_screen */
	uint8_t act;		/* enum fr_act, on the confirm screen */
	bool armed;		/* every key was read up since this screen came */
	bool holding;		/* key 1 is held on the confirm screen */
	uint64_t hold_from;
	uint8_t hold_pct;	/* 0..100 of FR_HOLD_US */
	uint8_t refusal;	/* enum fr_write_rc of an action that did not start */
};

struct fr_start {
	const struct fr_start_hw *hw;
	struct fr_psoc psoc;
	struct fr_panel panel;
	bool panel_up;
	bool lit;
	struct fr_screen screen;
	struct fr_screen drawn;	/* what the frame buffer holds, when drawn_ok */
	bool drawn_ok;
	bool desc_ok;		/* the loaded descriptor parses */
	struct fr_desc desc;
	uint8_t why;		/* FR_WHY_* when recovery follows */
	/* recovery */
	struct fr_rec_status rec;
	struct fr_proto proto;
	uint64_t drawn_at;
	uint32_t drawn_changes;
	bool usb_was;
	struct fr_ui ui;
	uint64_t keys_at;	/* the last key reading */
	uint32_t keys_gap;	/* the time to the next one */
};

/*
 * The master's start-up (S-07.023's new order): the panel and the logo
 * frame, the greeting (with S-07.021's wait ending at the PSoC's greeting),
 * the power queries and the start-up report, the battery wait's first
 * requests and any charge wait, the logo lit, the subscription and the key
 * status. Then Power + M4 is recovery; otherwise the image check and the
 * start log's step (S-02.095), the one flash write of a start that goes on
 * to the main image. ho carries what the board read at entry and gets the
 * rest.
 */
enum fr_next fr_start_run(struct fr_start *s, const struct fr_start_hw *hw,
			  struct fr_handoff *ho);

/*
 * The end after FR_NEXT_HALT, which is the PSoC's: it removes the power
 * (S-07.051) or restarts the unit (S-04.065). The board calls this for
 * ever; each call serves the link for us microseconds, so the power switch
 * still turns the unit off when a restart request had no effect. The
 * request is not sent again: the full bootloader waits for the PSoC
 * (S-07.070). After a power-off nothing more is handled; the call only
 * waits out its time.
 */
void fr_halt(struct fr_start *s, uint32_t us);

/* Recovery: returns only FR_NEXT_HALT (a power-off, or the restart sent). */
enum fr_next fr_recovery_run(struct fr_start *s);

/* The recovery screen for a state (fr_recscreen.c); ui NULL is the main
 * screen. */
void fr_rec_screen(struct fr_screen *scr, const struct fr_rec_status *st,
		   const struct fr_proto *p, bool usb, const struct fr_ui *ui);

/* The main screen, nothing held. */
void fr_ui_reset(struct fr_ui *u);
/* One key reading at now_us: true when an action was confirmed, *act the
 * enum fr_act; the caller starts it (fr_proto_local()). */
bool fr_ui_key(struct fr_ui *u, const struct fr_rec_status *st, unsigned key, uint64_t now_us,
	       uint8_t *act);
/* The slot menu offers the action: it would start, confirmed. */
bool fr_ui_offers(const struct fr_rec_status *st, uint8_t act);

#endif /* FR_START_H */
