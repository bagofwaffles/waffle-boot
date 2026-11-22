/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DIV64_H
#define WAFFLE_DIV64_H

/*
 * floor(n / d) for a 64-bit n and a 32-bit d other than 0, without the
 * compiler's 64-bit division: on the BF70x that is libgcc's __udivdi3,
 * which makes four calls to its 32-bit division and remainder for each
 * quotient word. Here a d below 2^16 takes one 32-bit division while n
 * fits 32 bits, two or three after: the remainder of each step is below
 * d, so 16 more bits of n beside it still fit 32. A larger d divides bit by bit. Self-contained,
 * for the yaffs2 glue too (src/glue/yaffs2/yaffs_port.h).
 */

#include <stdint.h>

static inline uint64_t wb_udiv64_32(uint64_t n, uint32_t d)
{
	uint32_t hi = (uint32_t)(n >> 32), lo = (uint32_t)n;
	uint32_t qhi = 0, qlo = 0, r = 0;

	if (hi == 0u)
		return lo / d;
	if (d <= 0xFFFFu) {
		uint32_t x, q1;

		if (hi >= d) {
			qhi = hi / d;
			hi -= qhi * d;
		}
		x = (hi << 16) | (lo >> 16);
		q1 = x / d;
		r = x - q1 * d;
		x = (r << 16) | (lo & 0xFFFFu);
		return ((uint64_t)qhi << 32) | (q1 << 16) | (x / d);
	}
	for (int i = 63; i >= 0; i--) {
		/* r < d < 2^32 before the shift; the bit shifted out is kept */
		uint32_t top = r >> 31;

		r = (r << 1) | (uint32_t)((n >> i) & 1u);
		if (top || r >= d) {
			r -= d;
			if (i >= 32)
				qhi |= 1u << (i - 32);
			else
				qlo |= 1u << i;
		}
	}
	return ((uint64_t)qhi << 32) | qlo;
}

#endif /* WAFFLE_DIV64_H */
