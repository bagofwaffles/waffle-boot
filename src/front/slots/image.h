/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The image descriptor (handoff.h) and the check of the loaded image
 * against it, and the hand-off record's seal.
 */
#ifndef FRONT_SLOTS_IMAGE_H
#define FRONT_SLOTS_IMAGE_H

#include "front/front.h"
#include "front/handoff.h"

struct fr_desc {
	uint32_t main_entry;
	uint32_t n_regions;
	uint32_t main_crc;
	uint32_t front_crc;
	uint16_t bl_version[4];
	uint8_t pin[FR_SHA256_LEN];
	uint32_t addr[FR_DESC_REGIONS];
	uint32_t len[FR_DESC_REGIONS];
};

/* Where the front may read the loaded image as data (S-01.002, S-01.023):
 * [addr, addr + len) inside one range the stream loads and nothing has
 * written since. Never L1 code, the L1 holes, the front's own memory, the
 * hand-off record or recovery's staging buffer. */
bool fr_mem_readable(uint32_t addr, uint32_t len);

/* The descriptor in raw[0..n): its magic, version, size and CRC. */
bool fr_desc_parse(const uint8_t *raw, size_t n, struct fr_desc *d);

/* The loaded descriptor at FR_DESC_ADDR, then the CRC-32 of the regions it
 * lists as they lie in memory. False when either fails. */
bool fr_image_check(const struct fr_mem *mem, struct fr_desc *d);

/* The record's size, version, magic and CRC, last. */
void fr_handoff_seal(struct fr_handoff *ho);

#endif /* FRONT_SLOTS_IMAGE_H */
