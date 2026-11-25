/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.062 - S-15.065: the key test, as a rule over key events and a
 * frame of boxes.
 */
#ifndef WAFFLE_UI_KEYTEST_H
#define WAFFLE_UI_KEYTEST_H

#include <stdbool.h>
#include <stdint.h>

#include "waffle/boot/device.h"
#include "waffle/ui/console.h"

/* The most keys a device has (S-07.030); dev->keys.count are tested. */
#define S15_NKEYS WB_KEYS_MAX

/* Outline colors (RGB565). */
#define S15_KT_UNTESTED      0x4a49u
#define S15_KT_UNTESTED_FILL 0x1082u
#define S15_KT_DOWN          0xfe06u
#define S15_KT_DOWN_FILL     0xed04u
#define S15_KT_TESTED        0xef5du
#define S15_KT_TESTED_FILL   0xbdd7u

struct s15_keytest {
	const struct wb_device *dev;
	uint8_t tested[(S15_NKEYS + 7u) / 8u];
	uint8_t down[(S15_NKEYS + 7u) / 8u];
	int lone;           /* key down alone since its down, or -1 */
	unsigned n_tested;
};

/* S-15.062: every key of dev untested and up, whatever the state before. */
void s15_keytest_start(struct s15_keytest *kt, const struct wb_device *dev);
/* S-15.064: one key event. Returns true when the end condition of
 * S-15.065 holds after it. */
bool s15_keytest_event(struct s15_keytest *kt, unsigned key, bool down);
bool s15_keytest_tested(const struct s15_keytest *kt, unsigned key);
/* The box's outline color, and its fill. */
uint16_t s15_keytest_color(const struct s15_keytest *kt, unsigned key);
uint16_t s15_keytest_fill(uint16_t color);

/* The lines of S-15.062: a text line and a keys line. */
#define S15_KEYTEST_LINES 2u
extern const char *const s15_keytest_lines[S15_KEYTEST_LINES];

struct s15_rect {
	uint16_t x, y, w, h;
};
/* The box of a key: below the lines and above the status bar, within
 * the visible area (S-15.062). False if the geometry leaves no room for
 * boxes, and for a key past S15_NKEYS. */
bool s15_keytest_box(const struct s15_geometry *g, unsigned key,
		     struct s15_rect *r);

/* A frame of the key test: black, the frame (with the option's indicator
 * when the option is on), the lines and a box for each of the device's keys
 * (S-15.062). */
void s15_render_keytest(const struct s15_keytest *kt,
			const struct s15_geometry *g,
			const struct s15_draw_ops *d,
			const struct s15_frame *f);

#endif /* WAFFLE_UI_KEYTEST_H */
