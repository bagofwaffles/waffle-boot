/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The stored boot logo (fr_handoff.h's format), as the front checks and
 * draws it: every check passes, or the caller draws its own logo. Every
 * loop is bounded by sizes the header check has already limited.
 */
#ifndef FR_LOGO_H
#define FR_LOGO_H

#include "fr.h"
#include "fr_handoff.h"
#include "fr_slots.h"

struct fr_logo {
	uint32_t width, height, bits, colors;
	uint32_t len, crc;	/* the data's bytes and CRC-32 */
	uint16_t bg;
};

/* The header h of a slot whose record is rec: magic, version, size and CRC,
 * every field in range, the image CRC-32 the record's, and the data lying
 * wholly between the image's end (its length rounded up to a sector) and
 * the header's sector. */
bool fr_logo_head(const uint8_t h[FR_LOGO_HEAD], const struct fr_record *rec, struct fr_logo *l);
/* The data (l->len bytes) against l->crc, then drawn whole into frame
 * (FR_FRAME_BYTES in bus order). False at the first fault: frame then holds
 * no picture to show. */
bool fr_logo_draw(const struct fr_logo *l, const uint8_t *data, uint8_t *frame);
/* Slot k's logo read into data (FR_LOGO_DATA_MAX bytes) and drawn. */
bool fr_logo_load(const struct fr_flash *f, unsigned k, uint8_t *data, uint8_t *frame);

#endif /* FR_LOGO_H */
