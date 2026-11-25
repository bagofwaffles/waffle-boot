/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_BOARD_HO_COPY_H
#define WAFFLE_BOARD_HO_COPY_H

/*
 * The load function's copy into DDR or L2 (S-05.009, S-05.011), by the CPU:
 * bytes up to the destination's word boundary, then aligned 32-bit stores,
 * then the bytes left. Each word stored comes from one aligned load when the
 * source has the destination's alignment, or else from two neighboring
 * aligned loads shifted together (little-endian); no load reaches a word
 * that holds none of the bytes. Every access is volatile, so the compiler
 * neither merges nor reorders them nor calls a library copy: the load
 * function runs in the handover reserve and may reach nothing outside it.
 * Always inlined into its caller there. A header so that the unit tests run
 * the same code on the host.
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/boot/memmap.h"

typedef uint32_t __attribute__((may_alias)) opxy_ho_word;

static inline __attribute__((always_inline)) void
opxy_ho_copy(uintptr_t dst, uintptr_t src, uint32_t n)
{
	uint32_t i = 0;

	for (; i < n && ((dst + i) & 3u) != 0u; i++)
		*(volatile uint8_t *)(dst + i) = *(const volatile uint8_t *)(src + i);
	if (n - i >= 4u) {
		const uint32_t sh = (uint32_t)((src + i) & 3u) * 8u;

		if (sh == 0u) {
			for (; i + 4u <= n; i += 4u)
				*(volatile opxy_ho_word *)(dst + i) =
					*(const volatile opxy_ho_word *)(src + i);
		} else {
			const volatile opxy_ho_word *p =
				(const volatile opxy_ho_word *)((src + i) & ~(uintptr_t)3u);
			uint32_t lo = *p++;

			for (; i + 4u <= n; i += 4u) {
				const uint32_t hi = *p++;

				*(volatile opxy_ho_word *)(dst + i) = lo >> sh | hi << (32u - sh);
				lo = hi;
			}
		}
	}
	for (; i < n; i++)
		*(volatile uint8_t *)(dst + i) = *(const volatile uint8_t *)(src + i);
}

static inline __attribute__((always_inline)) bool
opxy_ho_inside(uint32_t dst, uint32_t n, uint32_t start, uint32_t end)
{
	return dst >= start && dst < end && n <= end - dst;
}

/* Whether a block of n bytes at dst lies wholly in DDR or wholly in L2,
 * where opxy_ho_copy() serves; 32-bit sums only, so no library call. */
static inline __attribute__((always_inline)) bool
opxy_ho_words_ok(uint32_t dst, uint32_t n)
{
	return opxy_ho_inside(dst, n, S01_DDR_START, (uint32_t)S01_DDR_END) ||
	       opxy_ho_inside(dst, n, S01_L2_START, S01_L2_END);
}

#endif /* WAFFLE_BOARD_HO_COPY_H */
