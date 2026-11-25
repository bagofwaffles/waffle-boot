/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.172: held mode's settings that last across resets. They are kept in
 * the running slot's settings sector on the SPI flash (waffle/flash/settings.h);
 * the board reads and writes it (s15_env_ops.settings_save). No file on the
 * eMMC is read or written for them.
 */
#ifndef WAFFLE_UI_SETTINGS_H
#define WAFFLE_UI_SETTINGS_H

#include <stdbool.h>

#include "waffle/boot/device.h"

struct s15_settings {
	bool dev_mode;   /* Developer mode: the developer menu, the serial port */
	bool color;      /* S-15.173: drawn in color, not gray */
	bool mtp_macos;  /* S-15.174: MTP for macOS, not Windows */
	bool start_menu; /* S-15.175: start in the boot menu */
	bool host_link;  /* S-15.176: the host link answers */
};

/* S-15.173: the color's default on dev, where no setting is stored: on for
 * the OP-1F, off (gray) for the OP-XY and when dev is NULL.
 * SPEC-Q: S-15.172 and S-15.173 give one default, color off, for every
 * device; here the OP-1F's is on. Whether the contract states it by device
 * is the maintainer's to confirm. */
bool s15_color_default(const struct wb_device *dev);
/* The defaults on dev: developer mode off, the color by
 * s15_color_default(), MTP for macOS, start in boot menu off, host link on. */
void s15_settings_defaults(struct s15_settings *s, const struct wb_device *dev);

#endif /* WAFFLE_UI_SETTINGS_H */
