/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.172: the settings file, /yaffs2/bootloader.settings. Held mode's
 * settings that last across resets, as text:
 *
 *     waffle-boot settings 1
 *     developer_mode=on|off
 *     color=on|off
 *     mtp_target=macos|windows
 *
 * A missing or unusable file gives every setting its default; a line
 * this does not know is ignored. A developer_menu line (S-15.172) gives
 * developer mode when no developer_mode line does. Reading and
 * writing the volume is the board's (s15_env_ops.settings_save); this is
 * the format alone.
 */
#ifndef WAFFLE_S15_SETTINGS_H
#define WAFFLE_S15_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define S15_SETTINGS_PATH     "/yaffs2/bootloader.settings"
#define S15_SETTINGS_NEW_PATH "/yaffs2/bootloader.settings.new"
#define S15_SETTINGS_MAX      512u   /* a longer file is unusable */
#define S15_SETTINGS_OUT_MAX  96u    /* room for what s15_settings_write writes */

struct s15_settings {
	bool dev_mode;   /* Developer mode: the developer menu, the serial port */
	bool color;      /* S-15.173: drawn in color, not gray */
	bool mtp_macos;  /* S-15.174: MTP for macOS, not Windows */
};

/* The defaults: developer mode off, in color, MTP for macOS. */
void s15_settings_defaults(struct s15_settings *s);
/* S-15.172: the settings the file's n bytes give. False, with every
 * setting at its default, when the file is unusable (n over
 * S15_SETTINGS_MAX, a byte not text, or no header). A missing file is
 * read as n = 0. */
bool s15_settings_read(const uint8_t *p, size_t n, struct s15_settings *s);
/* S-15.172: the bytes written for s, into buf; their count, or 0 when cap
 * is too small. */
size_t s15_settings_write(const struct s15_settings *s, uint8_t *buf,
			  size_t cap);

#endif /* WAFFLE_S15_SETTINGS_H */
