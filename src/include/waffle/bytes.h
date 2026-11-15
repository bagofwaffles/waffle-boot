/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Byte and string helpers shared by the core, the board and the drivers:
 * little- and big-endian access a byte at a time (the target faults on an
 * unaligned word, and these take any pointer), byte copies, fills and
 * compares, and the length and equality of C strings.
 *
 * Much of the core keeps to C11's freestanding headers, which have no
 * string functions; these stand in for the few it needs.
 *
 * Any layer may include this header (tools/layering.py), so it includes
 * nothing but the standard headers.
 */
#ifndef WAFFLE_BYTES_H
#define WAFFLE_BYTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline uint16_t wb_get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}

static inline uint16_t wb_get_be16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)(p[0] << 8) | p[1]);
}

static inline uint32_t wb_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static inline uint32_t wb_get_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static inline void wb_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void wb_put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void wb_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void wb_put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline void wb_put_le64(uint8_t *p, uint64_t v)
{
	wb_put_le32(p, (uint32_t)v);
	wb_put_le32(p + 4, (uint32_t)(v >> 32));
}

static inline void wb_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
	for (size_t i = 0; i < n; i++)
		dst[i] = src[i];
}

static inline void wb_fill(uint8_t *dst, uint8_t v, size_t n)
{
	for (size_t i = 0; i < n; i++)
		dst[i] = v;
}

/* 1 when the n bytes at a and b are equal, else 0. */
static inline int wb_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static inline size_t wb_strlen(const char *s)
{
	size_t n = 0;

	while (s[n] != '\0')
		n++;
	return n;
}

/* The length of s, or max when s has no terminator in its first max bytes. */
static inline size_t wb_strnlen(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n] != '\0')
		n++;
	return n;
}

static inline bool wb_streq(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

#endif
