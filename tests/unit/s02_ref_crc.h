/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test-only reference CRC-32, written from the parameters of S-02.015 and
 * S-03.025: polynomial 0x04C11DB7 reflected (0xEDB88320), initial value and
 * final XOR 0xFFFFFFFF.  zlib's calling convention: start from 0, chain.
 */
#ifndef S02_REF_CRC_H
#define S02_REF_CRC_H

#include <stdint.h>

static inline uint32_t ref_crc32(uint32_t crc, const uint8_t *buf, uint32_t len)
{
	uint32_t c = crc ^ 0xFFFFFFFFu;
	uint32_t i;
	int k;

	for (i = 0; i < len; i++) {
		c ^= buf[i];
		for (k = 0; k < 8; k++)
			c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
	}
	return c ^ 0xFFFFFFFFu;
}

#endif /* S02_REF_CRC_H */
