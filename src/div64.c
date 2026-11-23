/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The 64-bit divisions of waffle/div64.h, out of line. */
#include "waffle/div64.h"

uint64_t wb_udiv64(uint64_t n, uint32_t d)
{
	return wb_udiv64_32(n, d);
}

uint32_t wb_percent32(uint32_t done, uint32_t total)
{
	const uint32_t q = done / total, r = done - q * total;

	return 100u * q + (uint32_t)wb_udiv64((uint64_t)r * 100u + total / 2u, total);
}
