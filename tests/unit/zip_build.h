/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test data and deflate streams (RFC 1951) for the zip reader's tests:
 * little-endian fields, fixed-code blocks with greedy matches up to 32 KiB
 * back, and stored blocks.
 */
#ifndef WAFFLE_ZIP_BUILD_H
#define WAFFLE_ZIP_BUILD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline size_t put16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	return 2;
}

static inline size_t put32(uint8_t *p, uint32_t v)
{
	return put16(p, v & 0xFFFFu) + put16(p + 2, v >> 16);
}

/* Test data: random stretches, runs, and copies of earlier bytes from up
 * to 40,000 back (beyond the window, too). */
static inline void fill(uint8_t *p, size_t n, uint32_t seed)
{
	uint32_t x = seed;
	size_t i = 0;

	while (i < n) {
		size_t len;

		x = x * 1664525u + 1013904223u;
		len = 1 + (x >> 8) % 700u;
		if (len > n - i)
			len = n - i;
		if ((x & 3u) == 2u && i > 0) {
			const size_t d = 1 + (x >> 4) % (i < 40000 ? i : 40000);

			for (size_t k = 0; k < len; k++)
				p[i + k] = p[i + k - d];
		} else if ((x & 3u) == 3u) {
			memset(p + i, (int)(x >> 24), len);
		} else {
			for (size_t k = 0; k < len; k++) {
				x = x * 1664525u + 1013904223u;
				p[i + k] = (uint8_t)(x >> 24);
			}
		}
		i += len;
	}
}

struct bits {
	uint8_t *p;
	size_t n;
	uint32_t acc;
	unsigned have;
};

static inline void put_bits(struct bits *b, uint32_t v, unsigned k)
{
	b->acc |= v << b->have;
	b->have += k;
	while (b->have >= 8) {
		b->p[b->n++] = (uint8_t)b->acc;
		b->acc >>= 8;
		b->have -= 8;
	}
}

/* a Huffman code, sent from its top bit */
static inline void put_code(struct bits *b, uint32_t code, unsigned len)
{
	uint32_t r = 0;

	for (unsigned i = 0; i < len; i++)
		r |= ((code >> i) & 1u) << (len - 1 - i);
	put_bits(b, r, len);
}

static inline void put_sym(struct bits *b, unsigned c)
{
	if (c < 144)
		put_code(b, 0x30u + c, 8);
	else if (c < 256)
		put_code(b, 0x190u + c - 144, 9);
	else if (c < 280)
		put_code(b, c - 256, 7);
	else
		put_code(b, 0xC0u + c - 280, 8);
}

static inline void put_match(struct bits *b, unsigned len, unsigned dist)
{
	static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13,
		15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131,
		163, 195, 227, 258 };
	static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
		2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
	static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25,
		33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
		3073, 4097, 6145, 8193, 12289, 16385, 24577 };
	static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4,
		5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
	unsigned i = 28, j = 29;

	while (lbase[i] > len)
		i--;
	put_sym(b, 257 + i);
	put_bits(b, len - lbase[i], lext[i]);
	while (dbase[j] > dist)
		j--;
	put_code(b, j, 5);
	put_bits(b, dist - dbase[j], dext[j]);
}

static inline unsigned hash3(const uint8_t *p)
{
	return ((unsigned)p[0] << 8 ^ (unsigned)p[1] << 4 ^ p[2]) & 4095u;
}

/*
 * A block with fixed codes of p[from, to), its matches up to 32 KiB back
 * into p[0, to), ended by its end-of-block code. head is the match table,
 * zeroed by the caller before a stream's first block.
 */
static inline void deflate_fixed_block(struct bits *b, const uint8_t *p,
				       size_t from, size_t to, bool last,
				       size_t head[4096])
{
	size_t i = from;

	put_bits(b, last ? 1u : 0u, 1);
	put_bits(b, 1, 2);		/* fixed codes */
	while (i < to) {
		size_t len = 0, at = 0;

		if (to - i >= 3) {
			const unsigned h = hash3(p + i);

			at = head[h];
			head[h] = i + 1;
		}
		if (at != 0 && i + 1 - at <= 32768) {
			at--;
			while (len < 258 && i + len < to && p[at + len] == p[i + len])
				len++;
		}
		if (len < 3) {
			put_sym(b, p[i++]);
			continue;
		}
		put_match(b, (unsigned)len, (unsigned)(i - at));
		for (size_t k = 1; k < len; k++) {
			if (to - (i + k) >= 3)
				head[hash3(p + i + k)] = i + k + 1;
		}
		i += len;
	}
	put_sym(b, 256);
}

/* A stored block of p[from, to), at most 65,535 bytes. */
static inline void deflate_stored_block(struct bits *b, const uint8_t *p,
					size_t from, size_t to, bool last)
{
	const unsigned n = (unsigned)(to - from);

	put_bits(b, last ? 1u : 0u, 1);
	put_bits(b, 0, 2);
	if (b->have != 0)
		put_bits(b, 0, 8 - b->have);
	put_bits(b, n, 16);
	put_bits(b, ~n & 0xFFFFu, 16);
	memcpy(b->p + b->n, p + from, n);
	b->n += n;
}

/* One fixed-code block of p; dst holds n * 9 / 8 + 16 bytes. */
static inline size_t deflate_fixed(const uint8_t *p, size_t n, uint8_t *dst)
{
	static size_t head[4096];
	struct bits b = { dst, 0, 0, 0 };

	memset(head, 0, sizeof(head));
	deflate_fixed_block(&b, p, 0, n, true, head);
	if (b.have != 0)
		put_bits(&b, 0, 8 - b.have);
	return b.n;
}

/* Blocks of `block` bytes (at most 65,535) of p, fixed-code and stored in
 * turn; dst holds n * 9 / 8 + 16 + 5 * (n / block + 1) bytes. */
static inline size_t deflate_mixed(const uint8_t *p, size_t n, size_t block,
				   uint8_t *dst)
{
	static size_t head[4096];
	struct bits b = { dst, 0, 0, 0 };
	size_t at = 0;
	unsigned k = 0;

	memset(head, 0, sizeof(head));
	do {
		const size_t to = n - at > block ? at + block : n;

		if (k++ % 2 == 0)
			deflate_fixed_block(&b, p, at, to, to == n, head);
		else
			deflate_stored_block(&b, p, at, to, to == n);
		at = to;
	} while (at < n);
	if (b.have != 0)
		put_bits(&b, 0, 8 - b.have);
	return b.n;
}

#endif /* WAFFLE_ZIP_BUILD_H */
