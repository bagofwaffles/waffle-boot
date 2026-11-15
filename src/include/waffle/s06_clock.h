/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The injected clock of the serial protocols (S-06 to S-09).
 *
 * Time is a free-running microsecond count that wraps every 2^32 us
 * (about 71 minutes); compare with s06_time_reached(), never with "<".
 * Every wait loop of these modules calls idle() when it has nothing to do:
 * the implementation may return at once (busy polling), sleep until an
 * interrupt, or, in a test, move a virtual clock on to the next input or
 * to until_us. It may return before until_us.
 */
#ifndef WAFFLE_S06_CLOCK_H
#define WAFFLE_S06_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

struct s06_clock {
	uint32_t (*now_us)(void *ctx);
	void (*idle)(void *ctx, uint32_t until_us);
	void *ctx;
};

#define S06_MS(ms) ((uint32_t)(ms) * 1000u)

/* True once now has reached or passed t (wrap-safe for spans < 2^31 us). */
static inline bool s06_time_reached(uint32_t now, uint32_t t)
{
	return (int32_t)(now - t) >= 0;
}

static inline uint32_t s06_now(const struct s06_clock *c)
{
	return c->now_us(c->ctx);
}

static inline void s06_idle(const struct s06_clock *c, uint32_t until_us)
{
	c->idle(c->ctx, until_us);
}

#endif
