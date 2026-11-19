/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.172: held mode's settings that last across resets. They are kept in
 * the running slot's settings sector on the SPI flash (waffle/s02_store.h);
 * the board reads and writes it (s15_env_ops.settings_save). No file on the
 * eMMC is read or written for them.
 */
#ifndef WAFFLE_S15_SETTINGS_H
#define WAFFLE_S15_SETTINGS_H

#include <stdbool.h>

struct s15_settings {
	bool dev_mode;   /* Developer mode: the developer menu, the serial port */
	bool color;      /* S-15.173: drawn in color, not gray */
	bool mtp_macos;  /* S-15.174: MTP for macOS, not Windows */
	bool start_menu; /* S-15.175: start in the boot menu */
};

/* The defaults: developer mode off, in gray, MTP for macOS, start in boot
 * menu off. */
void s15_settings_defaults(struct s15_settings *s);

#endif /* WAFFLE_S15_SETTINGS_H */
