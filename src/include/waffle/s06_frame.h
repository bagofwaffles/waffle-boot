/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-06 frames: the raw frame, its check byte and its COBS encoding.
 */
#ifndef WAFFLE_S06_FRAME_H
#define WAFFLE_S06_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define S06_MAX_PAYLOAD 249u	/* S-06.002 */
#define S06_HEADER 4u		/* route, seq, id, len */
#define S06_MAX_RAW 254u	/* S-06.001: header, payload, check */
#define S06_MAX_WIRE 256u	/* S-06.005: encoded frame and delimiter */

/* Node addresses (S-06.010). */
#define S06_ADDR_DEFAULT 0u
#define S06_ADDR_MASTER 1u
#define S06_ADDR_HOST 2u
#define S06_ADDR_NORDIC 3u
#define S06_ADDR_PSOC 4u
#define S06_ADDR_SLAVE 5u
#define S06_ADDR_SLOT0 8u	/* reply slots 0-3 are 8-11 */

/*
 * The check byte (S-06.003) comes from outside: a CRC-8, polynomial 0x4D
 * reflected (0xB2), init 0x00, every input byte XORed with 0xA5, no final
 * XOR; equivalently refin/refout, init 0xFF, xorout 0xFF. Check value of
 * "123456789": 0xD8.
 */
typedef uint8_t (*s06_crc8_fn)(const uint8_t *data, size_t len);

struct s06_frame {
	uint8_t route;
	uint8_t seq;
	uint8_t id;
	uint8_t len;
	uint8_t payload[S06_MAX_PAYLOAD];
};

static inline uint8_t s06_route(uint8_t src, uint8_t dst)
{
	return (uint8_t)((src & 0x0Fu) << 4 | (dst & 0x0Fu));
}

static inline uint8_t s06_src(uint8_t route)
{
	return (uint8_t)(route >> 4);
}

static inline uint8_t s06_dst(uint8_t route)
{
	return (uint8_t)(route & 0x0Fu);
}

/* The reply route of S-06.031: the request's nibbles exchanged. */
static inline uint8_t s06_swap_route(uint8_t route)
{
	return (uint8_t)((route << 4) | (route >> 4));
}

/*
 * COBS-encode n bytes (n <= 254) into out, without the delimiter
 * (S-06.004). Returns the encoded length: n + 1.
 */
size_t s06_cobs_encode(const uint8_t *in, size_t n, uint8_t *out);

/*
 * Build the raw frame of S-06.001 into raw[S06_MAX_RAW]. Returns its length
 * (len + 5), or 0 when len exceeds S06_MAX_PAYLOAD (S-06.002: never cut).
 */
size_t s06_frame_raw(uint8_t route, uint8_t seq, uint8_t id,
		     const uint8_t *payload, size_t len, s06_crc8_fn crc,
		     uint8_t *raw);

/*
 * The bytes a sender writes for a frame (S-06.004-S-06.006): the encoded
 * frame and one delimiter, into out[S06_MAX_WIRE]. Returns their count
 * (7-256), or 0 when the payload is too long.
 */
size_t s06_frame_encode(uint8_t route, uint8_t seq, uint8_t id,
			const uint8_t *payload, size_t len, s06_crc8_fn crc,
			uint8_t *out);

#endif
