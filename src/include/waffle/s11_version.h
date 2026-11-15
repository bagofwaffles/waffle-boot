/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_S11_VERSION_H
#define WAFFLE_S11_VERSION_H

/*
 * Versions: four unsigned 16-bit numbers major.minor.patch.build, how they
 * are read from text (S-11.036, S-17.024), compared (S-11.037) and written
 * (S-11 terms, S-10c.055).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct s11_version {
	uint16_t major, minor, patch, build;
};

/* The window S-11.036 reads of a version file. */
#define S11_VERSION_FILE_WINDOW 32u
/* The window S-17.024 reads of a coprocessor's version value. */
#define S11_VERSION_VALUE_WINDOW 63u
/* Longest text s11_version_text() writes: "65535.65535.65535+65535". */
#define S11_VERSION_TEXT_MAX 23u
/* Longest text s11_u32_text() writes. */
#define S11_U32_TEXT_MAX 10u

/*
 * Reads a version from the first `window` bytes of `text` (fewer when `len`
 * is shorter). Returns false, leaving *out alone, when those bytes hold fewer
 * than two '.' characters.
 */
bool s11_version_parse(const uint8_t *text, size_t len, size_t window,
		       struct s11_version *out);

/*
 * The version a version file reads as (S-11.036): `present` false for an
 * absent file. An absent file, or one whose window lacks either '.', reads
 * as 1234.0.0+0.
 */
struct s11_version s11_version_file(const uint8_t *data, size_t len,
				    bool present);

/* -1, 0 or 1 as a is below, equal to or above b (S-11.037). */
int s11_version_cmp(const struct s11_version *a, const struct s11_version *b);

/* Major 0, minor 1, patch 0, any build: the forced marker (S-11.055). */
bool s11_version_is_forced_marker(const struct s11_version *v);

/*
 * Writes "M.m.p", or "M.m.p+b" when the build is not 0, with no NUL.
 * Returns the length, or 0 when `cap` is too small (nothing is written).
 */
size_t s11_version_text(const struct s11_version *v, char *buf, size_t cap);

/* Unsigned decimal, no leading zeros, no NUL; 0 when `cap` is too small. */
size_t s11_u32_text(uint32_t v, char *buf, size_t cap);

#endif /* WAFFLE_S11_VERSION_H */
