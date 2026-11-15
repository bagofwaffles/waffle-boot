/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test helpers shared by the s06-s09 unit tests: checks, a reference CRC-8
 * written from S-06.003's parameters, and hex dumps.
 */
#ifndef S06_TEST_H
#define S06_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_failures;
static const char *t_case = "";

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			t_failures++;                                          \
			fprintf(stderr, "%s:%d: [%s] check failed: %s\n",      \
				__FILE__, __LINE__, t_case, #cond);            \
		}                                                              \
	} while (0)

#define CHECK_FAIL(msg)                                                        \
	do {                                                                   \
		t_failures++;                                                  \
		fprintf(stderr, "%s:%d: [%s] %s\n", __FILE__, __LINE__,       \
			t_case, (msg));                                        \
	} while (0)

#define CHECK_EQ(a, b)                                                         \
	do {                                                                   \
		long long t_a_ = (long long)(a), t_b_ = (long long)(b);        \
		if (t_a_ != t_b_) {                                            \
			t_failures++;                                          \
			fprintf(stderr, "%s:%d: [%s] %s == %lld, expected %lld\n", \
				__FILE__, __LINE__, t_case, #a, t_a_, t_b_);   \
		}                                                              \
	} while (0)

static inline void t_hex(const char *label, const void *v, size_t n)
{
	const uint8_t *p = v;

	fprintf(stderr, "%s (%zu):", label, n);
	for (size_t i = 0; i < n && i < 64; i++)
		fprintf(stderr, " %02x", p[i]);
	fprintf(stderr, n > 64 ? " ...\n" : "\n");
}

static inline void t_check_mem(const char *file, int line, const char *what,
			       const void *a, size_t an, const void *b, size_t bn)
{
	if (an == bn && (an == 0 || memcmp(a, b, an) == 0))
		return;
	t_failures++;
	fprintf(stderr, "%s:%d: [%s] %s differs\n", file, line, t_case, what);
	t_hex("  got     ", a, an);
	t_hex("  expected", b, bn);
}

#define CHECK_MEM(a, an, b, bn) \
	t_check_mem(__FILE__, __LINE__, #a, (a), (an), (b), (bn))

static inline int t_finish(const char *name)
{
	if (t_failures) {
		printf("FAIL %s (%d)\n", name, t_failures);
		return 1;
	}
	printf("PASS %s\n", name);
	return 0;
}

/* S-06.003 as stated: c ^= b ^ 0xA5, then 8 reflected steps with 0xB2. */
static inline uint8_t t_crc8(const uint8_t *d, size_t n)
{
	uint8_t c = 0;

	for (size_t i = 0; i < n; i++) {
		c = (uint8_t)(c ^ d[i] ^ 0xA5u);
		for (int k = 0; k < 8; k++)
			c = (uint8_t)((c & 1u) ? (c >> 1) ^ 0xB2u : c >> 1);
	}
	return c;
}

/*
 * Reference COBS decode of the k-th delimited frame in a written byte
 * stream (independent of the receiver under test). Returns the raw length,
 * or 0 if there is no such frame. raw must hold 256 bytes.
 */
static inline size_t t_nth_raw(const uint8_t *s, size_t n, size_t k,
			       uint8_t *raw)
{
	size_t at = 0;

	memset(raw, 0, 256);
	while (at < n) {
		size_t end = at;

		while (end < n && s[end] != 0)
			end++;
		if (end == n)
			return 0;
		if (end > at && k-- == 0) {
			size_t i = at, o = 0;

			while (i < end) {
				uint8_t code = s[i++];

				for (uint8_t j = 1; j < code && i < end; j++)
					raw[o++] = s[i++];
				if (code != 0xFF && i < end)
					raw[o++] = 0;
			}
			return o;
		}
		at = end + 1;
	}
	return 0;
}

/* S-07.082: reflected 0x8408, init 0xFFFF, complemented (bytes not swapped). */
static inline uint16_t t_crc16_x25(const uint8_t *d, size_t n)
{
	uint16_t c = 0xFFFF;

	for (size_t i = 0; i < n; i++) {
		uint8_t b = d[i];

		for (int k = 0; k < 8; k++) {
			c = (uint16_t)(((c ^ b) & 1u) ? (c >> 1) ^ 0x8408u : c >> 1);
			b = (uint8_t)(b >> 1);
		}
	}
	return (uint16_t)~c;
}

/* A small deterministic PRNG for randomized tests. */
static inline uint32_t t_rand(uint32_t *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

#endif
