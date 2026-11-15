/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.180: the boot prompt of a build with it on (S-05.025): its lines,
 * and what its keys choose. The prompt is shown before held mode, so its
 * key rules are kept apart from the menus' (s15_menu.h): the keys down
 * when the screen is shown act only after they have gone up and down
 * again; the first digit 1 or 2 to go down after that chooses.
 */
#ifndef WAFFLE_S15_PROMPT_H
#define WAFFLE_S15_PROMPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s15_console.h"
#include "waffle/s15_keytest.h"
#include "waffle/s15_lines.h"
#include "waffle/s15_menu.h"

/* The prompt's title. */
extern const char s15_prompt_title[];

/* What the prompt shows (S-15.180). A slot is its index, 0 or 1, or -1
 * when it cannot be established (the running slot, S-01.002) or read (the
 * selector's). */
struct s15_prompt_values {
	int running;		/* S-01.002 */
	int selector;		/* S-02: the slot the selector names */
	int verified;		/* S-02.012: the verified byte, or -1 */
	bool sp_kept;		/* S-01.007: the INIT entry SP is kept */
	uint32_t entry_sp;
	bool debug;		/* S-07.032: the debug value */
};

/* The 7 lines, each in its style, in order. */
#define S15_PROMPT_LINES 7u
void s15_prompt_lines(const struct s15_prompt_values *v,
		      struct s15_buf out[S15_PROMPT_LINES],
		      enum s15_style style[S15_PROMPT_LINES]);
/*
 * The screen on the menus' console: cleared under the title Boot
 * and the lines appended, and so logged. The menus' screen stays
 * S15_SCR_NONE: no key acts on them before held mode (S-15.013), and
 * s15_menu_frame() then gives the prompt's frame.
 */
void s15_prompt_show(struct s15_menu *m, const struct s15_prompt_values *v);

enum s15_prompt_choice {
	S15_PROMPT_WAIT,	/* no digit yet */
	S15_PROMPT_START,	/* digit 1: the boot path goes on */
	S15_PROMPT_HELD		/* digit 2: held mode */
};

struct s15_prompt {
	uint8_t stale[(S15_NKEYS + 7u) / 8u];	/* down when shown */
	enum s15_prompt_choice choice;
};

/*
 * The screen is shown: down_now is S-07.031's known key state (9 bytes,
 * key k bit k & 7 of byte k >> 3), after every bitmap waiting on the link
 * has been taken into account; NULL: no key down.
 */
void s15_prompt_open(struct s15_prompt *p, const uint8_t *down_now);
/* A key event after the screen was shown (S-07.031). */
void s15_prompt_key(struct s15_prompt *p, unsigned key, bool down);
enum s15_prompt_choice s15_prompt_choice(const struct s15_prompt *p);

#endif /* WAFFLE_S15_PROMPT_H */
