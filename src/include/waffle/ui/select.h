/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.116: the selection of the steps before an install run (S-11.096):
 * its lines, and what its keys and the time decide. Each step the package
 * carries has a row on a fixed digit, 2 to 7, and starts at No; a digit
 * toggles its row when the row can be picked. Key 1 held for 5 s, with a
 * row at Yes, installs; digit 8 cancels; 60 s without a key event cancel.
 * Its key rules are kept apart from the menus' (ui/menu.h): the keys come
 * from the PSoC link alone, so no host can answer.
 *
 * S-15.197: the prompt before the selection for a package whose signature
 * does not match (S-11.021) runs on the same keys and time: key 1 held for
 * 5 s goes on, with no row to pick; digit 8 or the 60 s cancel.
 */
#ifndef WAFFLE_UI_SELECT_H
#define WAFFLE_UI_SELECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/ui/console.h"
#include "waffle/ui/keytest.h"
#include "waffle/ui/lines.h"

#define S15_SELECT_TIMEOUT_MS 60000u

/* A step's bit in a selection: bit n for step n (enum s15_step). */
#define S15_SELECT_BIT(step) ((uint8_t)(1u << (unsigned)(step)))

/* The digit of a step's row, 2 to 7; and the step of a digit, or -1. */
unsigned s15_select_digit(enum s15_step step);
int s15_select_step(unsigned digit);

/* What a step's row shows. */
enum s15_row {
	S15_ROW_NONE,		/* not carried: no row */
	S15_ROW_PICK,		/* No or Yes */
	S15_ROW_OFF,		/* dim: not in this version (S-11.073) */
	S15_ROW_OPTION_OFF,	/* dim: the option off (S-11.090) */
	S15_ROW_UP_TO_DATE	/* dim: it would report Already installed */
};

struct s15_select_row {
	enum s15_row kind;
	const char *installed;	/* NULL: not known, or none (Content) */
	const char *package;	/* the package's version; Content: the pack
				 * name, NULL for none */
};

struct s15_select_info {
	struct s15_select_row row[S15_STEP_COUNT];
	int slot;	/* the slot index the bootloader replaces, or -1 */
	bool stock;	/* the bootloader's install returns to stock (S-11.091) */
	bool valid;	/* the package's signature is valid (S-15.197) */
};

enum s15_select_choice {
	S15_SELECT_WAIT,
	S15_SELECT_INSTALL,
	S15_SELECT_CANCEL
};

/* How the wait ended, for the log. */
enum s15_select_end {
	S15_SELECT_END_NONE,
	S15_SELECT_END_HELD,
	S15_SELECT_END_KEY,		/* digit 8 */
	S15_SELECT_END_TIMEOUT
};

struct s15_select {
	const struct wb_device *dev;		/* S-15.016's keys */
	uint8_t stale[(S15_NKEYS + 7u) / 8u];	/* down when shown */
	uint8_t down[(S15_NKEYS + 7u) / 8u];	/* down now (S-07.031) */
	uint8_t pickable;	/* the rows that can be picked, a bit each */
	uint8_t selected;	/* the rows at Yes */
	uint32_t last_ms;	/* the last key event, or when shown */
	uint32_t held_ms;	/* when the hold began */
	unsigned held_key;
	bool holding;
	bool prompt;		/* S-15.197's prompt: no rows */
	enum s15_select_choice choice;
	enum s15_select_end end;
};

/*
 * The screen is shown at now_ms with every row at No: down_now is
 * S-07.031's known key state (9 bytes, key k bit k & 7 of byte k >> 3),
 * NULL for no key down; a key down then acts only after an up and a down.
 */
void s15_select_open(struct s15_select *s, const struct wb_device *dev,
		     uint8_t pickable, const uint8_t *down_now, uint32_t now_ms);
/* S-15.197: the signature prompt shown at now_ms, as s15_select_open()
 * with no row: key 1 held 5 s goes on (S15_SELECT_INSTALL). */
void s15_select_prompt_open(struct s15_select *s, const struct wb_device *dev,
			    const uint8_t *down_now, uint32_t now_ms);
/*
 * A key event from the PSoC link after the screen was shown (S-07.031).
 * Every event restarts the 60 s. A digit 2 - 7 toggles its row when it can
 * be picked; digit 8 cancels. A down of a digit-1 key that is no knob click,
 * with no other of the device's keys down but precomp and a row at Yes (on
 * the prompt, none is needed), starts the hold; its release, or any other key down, ends it and does nothing more
 * (S-07.033's rule). The selection is kept.
 */
void s15_select_key(struct s15_select *s, unsigned key, bool down,
		    uint32_t now_ms);
/* The time passes: a hold of 5 s installs; 60 s since the last key event,
 * with no hold, cancel. */
enum s15_select_choice s15_select_tick(struct s15_select *s, uint32_t now_ms);
/* The hold's bar, 0 to 100, and its whole seconds left; 0 with no hold. */
unsigned s15_select_fill(const struct s15_select *s, uint32_t now_ms);
unsigned s15_select_hold_left(const struct s15_select *s, uint32_t now_ms);
/* The whole seconds left of the 60, rounded up. */
unsigned s15_select_left(const struct s15_select *s, uint32_t now_ms);

/*
 * The screen's rows at now_ms, each in its style: item 1, a row for each
 * step carried in digit order, item 8, then the slot's warning while the
 * bootloader is at Yes, then, for a valid package, that it is signed by TE
 * while a row is free; while key 1 is held, item 1 armed and the hold's
 * bar as the last row, in the warning's place when nine rows are in use.
 * On the prompt (S-15.197): its warning, item 1 and item 8, and the bar
 * while key 1 is held. Returns the count.
 */
size_t s15_select_lines(const struct s15_select_info *info,
			const struct s15_select *s, uint32_t now_ms,
			struct s15_buf out[S15_ROWS],
			enum s15_style style[S15_ROWS]);

#endif /* WAFFLE_UI_SELECT_H */
