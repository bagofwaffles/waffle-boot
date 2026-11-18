/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-05.009, S-05.011: the load function's word copy into DDR and L2
 * (waffle/board/opxy_ho_copy.h) against a byte loop, for every source and
 * destination alignment, every length from 0 to well past a few words, so
 * every end alignment: the destination's bytes equal, the bytes around it
 * untouched. And which blocks it serves.
 */
#include <stdint.h>
#include <string.h>

#include "support/check.h"

#include "waffle/board/opxy_ho_copy.h"

#define MAX_LEN 96u
#define GUARD 16u
#define BUF (GUARD + 8u + MAX_LEN + GUARD)

static void byte_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

static void test_identical(void)
{
	static _Alignas(8) uint8_t src[BUF], got[BUF], want[BUF];
	unsigned so, d, fails = 0;
	uint32_t n, i;

	for (i = 0; i < BUF; i++)
		src[i] = (uint8_t)(i * 151u + 0x3Bu);
	for (so = 0; so < 8u; so++) {
		for (d = 0; d < 8u; d++) {
			for (n = 0; n <= MAX_LEN; n++) {
				memset(got, 0xA5, sizeof got);
				memset(want, 0xA5, sizeof want);
				opxy_ho_copy((uintptr_t)(got + GUARD + d),
					     (uintptr_t)(src + GUARD + so), n);
				byte_copy(want + GUARD + d, src + GUARD + so, n);
				if (memcmp(got, want, sizeof got) != 0)
					fails++;
			}
		}
	}
	CHECK(fails == 0u);
}

/* A long block at every pairing, as the kernel's payloads are. */
static void test_long(void)
{
	enum { N = 70000 };
	static _Alignas(8) uint8_t src[N + 16], got[N + 16], want[N + 16];
	unsigned so, d;
	uint32_t i;

	for (i = 0; i < sizeof src; i++)
		src[i] = (uint8_t)(i ^ (i >> 8) ^ (i >> 16));
	for (so = 0; so < 4u; so++) {
		for (d = 0; d < 4u; d++) {
			memset(got, 0x11, sizeof got);
			memset(want, 0x11, sizeof want);
			opxy_ho_copy((uintptr_t)(got + 4u + d), (uintptr_t)(src + 4u + so),
				     N - 3u);
			byte_copy(want + 4u + d, src + 4u + so, N - 3u);
			CHECK(memcmp(got, want, sizeof got) == 0);
		}
	}
}

static void test_served(void)
{
	/* DDR and L2, whole blocks only */
	CHECK(opxy_ho_words_ok(S01_DDR_START, 16u));
	CHECK(opxy_ho_words_ok(0x8A100003u, 0x1000u));
	CHECK(opxy_ho_words_ok((uint32_t)S01_DDR_END - 4u, 4u));
	CHECK(!opxy_ho_words_ok((uint32_t)S01_DDR_END - 4u, 5u));
	CHECK(!opxy_ho_words_ok(S01_DDR_START - 1u, 4u));
	CHECK(opxy_ho_words_ok(S01_L2_START, S01_L2_END - S01_L2_START));
	CHECK(!opxy_ho_words_ok(S01_L2_END - 2u, 4u));
	CHECK(!opxy_ho_words_ok(S01_L2_END, 4u));
	CHECK(!opxy_ho_words_ok(0x8FFFFFFCu, 0xFFFFFFFFu));
	/* L1 data and code, and the rest, are not */
	CHECK(!opxy_ho_words_ok(0x11800000u, 16u));
	CHECK(!opxy_ho_words_ok(0x11900000u, 16u));
	CHECK(!opxy_ho_words_ok(S01_L1_CODE_START, 16u));
	CHECK(!opxy_ho_words_ok(0x11B00000u, 16u));
	CHECK(!opxy_ho_words_ok(0x00000000u, 16u));
}

int main(void)
{
	RUN(test_identical);
	RUN(test_long);
	RUN(test_served);
	return check_summary("test_board_ho_copy");
}
