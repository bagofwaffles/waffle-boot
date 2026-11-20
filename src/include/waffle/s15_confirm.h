/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.116: the confirmation of a protected component before an install
 * run (S-11.096): its lines, and what its keys and the time decide. A key
 * that gives digit 1, held for 5 s, installs; digit 8 skips; 60 s without
 * a confirmation skips; keys that cannot be read skip. Its key rules are
 * kept apart from the menus' (s15_menu.h): the keys come from the PSoC
 * link alone, so no host can answer.
 */
#ifndef WAFFLE_S15_CONFIRM_H
#define WAFFLE_S15_CONFIRM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s15_console.h"
#include "waffle/s15_keytest.h"
#include "waffle/s15_lines.h"

#define S15_CONFIRM_HOLD_MS    5000u
#define S15_CONFIRM_TIMEOUT_MS 60000u

extern const char s15_confirm_title[];

/* What the screen says of the component. */
struct s15_confirm_info {
	const char *package;	/* the image's version in the package */
	const char *installed;	/* the running bootloader's; NULL: not known */
	int slot;		/* the slot index a bootloader replaces, or -1 */
};

/* The lines above the bar's row, each in its style, in order. */
#define S15_CONFIRM_LINES 7u
void s15_confirm_lines(const struct s15_confirm_info *info,
		       struct s15_buf out[S15_CONFIRM_LINES],
		       enum s15_style style[S15_CONFIRM_LINES]);
/* The last row (S15_ST_STEP_RUN): the bar at fill percent, then the
 * seconds left. */
void s15_confirm_bar(struct s15_buf *out, unsigned fill, unsigned left_s);

enum s15_confirm_choice {
	S15_CONFIRM_WAIT,
	S15_CONFIRM_INSTALL,
	S15_CONFIRM_SKIP
};

/* How the wait ended, for the log. */
enum s15_confirm_end {
	S15_CONFIRM_END_NONE,
	S15_CONFIRM_END_HELD,
	S15_CONFIRM_END_KEY,		/* digit 8 */
	S15_CONFIRM_END_TIMEOUT,
	S15_CONFIRM_END_UNREADABLE
};

struct s15_confirm {
	uint8_t stale[(S15_NKEYS + 7u) / 8u];	/* down when shown */
	uint8_t down[(S15_NKEYS + 7u) / 8u];	/* down now (S-07.031) */
	uint32_t opened_ms;
	uint32_t held_ms;	/* when the hold began */
	unsigned held_key;
	bool holding;
	enum s15_confirm_choice choice;
	enum s15_confirm_end end;
};

/*
 * The screen is shown at now_ms: down_now is S-07.031's known key state (9
 * bytes, key k bit k & 7 of byte k >> 3), NULL for no key down; a key down
 * then acts only after an up and a down. A hold starts only with no other of
 * the 72 keys down but precomp, and any other key down ends it (S-07.033's
 * rule). readable false: the key state
 * cannot be read, and the choice is S15_CONFIRM_SKIP at once.
 */
void s15_confirm_open(struct s15_confirm *c, const uint8_t *down_now,
		      bool readable, uint32_t now_ms);
/* A key event from the PSoC link after the screen was shown (S-07.031). */
void s15_confirm_key(struct s15_confirm *c, unsigned key, bool down,
		     uint32_t now_ms);
/* The time passes: a hold of 5 s installs, else 60 s since the screen was
 * shown skips. */
enum s15_confirm_choice s15_confirm_tick(struct s15_confirm *c,
					 uint32_t now_ms);
/* The bar, 0 to 100: 0 while no key is held. */
unsigned s15_confirm_fill(const struct s15_confirm *c, uint32_t now_ms);
/* The whole seconds left of the 60, rounded up. */
unsigned s15_confirm_left(const struct s15_confirm *c, uint32_t now_ms);

#endif /* WAFFLE_S15_CONFIRM_H */
