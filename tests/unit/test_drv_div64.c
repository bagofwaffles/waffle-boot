/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The 64-bit divisions done without the compiler's: wb_udiv64_32()
 * (waffle/div64.h), the time it gives (wb_time_us(), wb_time_ms()), and
 * yaffs2's wb_yaffs_div() (yaffs_port.h), each against the host's own
 * 64-bit division at the edges of every step and on random values.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "support/fake_core.h"
#include "waffle/div64.h"
#include "waffle/drivers/bf70x_time.h"
#include "yaffs_port.h"

static uint64_t rng = 0x9E3779B97F4A7C15u;

static uint64_t next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

/* Values near every power of two, and of d's multiples near them. */
static void edges(uint64_t *v, size_t *n, uint32_t d)
{
	for (unsigned b = 0; b < 64; b++)
		for (int k = -2; k <= 2; k++) {
			uint64_t p = ((uint64_t)1 << b) + (uint64_t)(int64_t)k;

			v[(*n)++] = p;
			v[(*n)++] = p - p % d;
			v[(*n)++] = p - p % d - 1u;
		}
	v[(*n)++] = 0u;
	v[(*n)++] = UINT64_MAX;
	v[(*n)++] = UINT64_MAX - UINT64_MAX % d;
	v[(*n)++] = UINT64_MAX - UINT64_MAX % d - 1u;
	v[(*n)++] = (uint64_t)d << 32;
	v[(*n)++] = ((uint64_t)d << 32) - 1u;
	v[(*n)++] = ((uint64_t)(d - 1u) << 32) | 0xFFFFFFFFu;
}

static void check_udiv(void)
{
	static const uint32_t ds[] = {
		1u, 2u, 3u, 7u, 10u, 100u, 399u, 400u, 401u, 1000u, 4080u,
		0x7FFFu, 0xFFFEu, 0xFFFFu, 0x10000u, 0x10001u, 400000u,
		0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu,
	};
	static uint64_t v[64 * 5 * 3 + 16];
	unsigned bad = 0;

	for (size_t i = 0; i < sizeof ds / sizeof ds[0]; i++) {
		size_t n = 0;

		edges(v, &n, ds[i]);
		for (size_t j = 0; j < n; j++)
			bad += wb_udiv64_32(v[j], ds[i]) != v[j] / ds[i];
	}
	CHECK(bad == 0, "edges: %u wrong", bad);

	bad = 0;
	for (unsigned i = 0; i < 2000000u; i++) {
		uint64_t x = next(), y = next();
		/* every width of n and of d */
		uint64_t n = x >> (y & 63u);
		uint32_t d = (uint32_t)(y >> 8) >> ((y >> 6) & 31u);

		if (d == 0u)
			d = 1u;
		bad += wb_udiv64_32(n, d) != n / d;
		bad += wb_udiv64_32(n, (uint32_t)(y >> 40) % 0xFFFFu + 1u) !=
		       n / ((uint32_t)(y >> 40) % 0xFFFFu + 1u);
	}
	CHECK(bad == 0, "random: %u wrong", bad);
}

/* The core counter's whole range at the nominal CCLK and others. */
static void check_time(void)
{
	static const uint32_t cpus[] = { 1u, 400u, 0xFFFFu, 0x10000u, 1000000u };
	unsigned bad = 0;

	fake_core_reset();
	fake_core_set_cycle_step(0);
	for (size_t i = 0; i < sizeof cpus / sizeof cpus[0]; i++) {
		const struct wb_time t = { cpus[i] };
		static uint64_t v[2 * (64 * 5 * 3 + 16) + 20000];
		size_t n = 0;

		edges(v, &n, cpus[i]);
		edges(v, &n, cpus[i] * 1000u);
		for (unsigned k = 0; k < 20000u; k++)
			v[n++] = next() >> (k & 63u);
		for (size_t j = 0; j < n; j++) {
			fake_core_set_cycles(v[j]);
			bad += wb_time_us(&t) != v[j] / cpus[i];
			bad += wb_time_ms(&t) != v[j] / ((uint64_t)cpus[i] * 1000u);
		}
	}
	CHECK(bad == 0, "time: %u wrong", bad);
}

/* yaffsfs_DoStat()'s st_blocks: signed, rounded toward zero as C does. */
static void check_yaffs_div(void)
{
	static const unsigned long ds[] = { 2u, 3u, 512u, 4080u, 4096u, 65535u };
	unsigned bad = 0;

	for (size_t i = 0; i < sizeof ds / sizeof ds[0]; i++) {
		const long long d = (long long)ds[i];
		static const long long fixed[] = {
			0, 1, -1, 4079, 4080, 4081, -4079, -4080, -4081,
			INT32_MAX, INT32_MIN, (long long)UINT32_MAX,
			-(long long)UINT32_MAX, INT64_MAX, INT64_MIN, INT64_MIN + 1,
		};

		for (size_t j = 0; j < sizeof fixed / sizeof fixed[0]; j++)
			bad += wb_yaffs_div(fixed[j], ds[i]) != fixed[j] / d;
		for (unsigned k = 0; k < 200000u; k++) {
			uint64_t r = next();
			long long n = (long long)((r >> 1) >> (r & 63u));

			if (r & 0x40u)
				n = -n;
			bad += wb_yaffs_div(n, ds[i]) != n / d;
		}
	}
	CHECK(bad == 0, "yaffs: %u wrong", bad);
}

int main(void)
{
	check_udiv();
	check_time();
	check_yaffs_div();
	return tl_done("drv_div64");
}
