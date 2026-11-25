/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_TIME_H
#define WAFFLE_DRIVERS_BF70X_TIME_H

/*
 * Time from the core cycle counter (S-13a.030, S-13a.031): microseconds are
 * floor(cycles / cycles_per_us) since the processor's reset, milliseconds
 * floor(cycles / (cycles_per_us * 1000)). The master only reads the
 * counter; the slave's follows the master's (S-09.036).
 *
 * cycles_per_us is the board's nominal CCLK in MHz (400 for S-13a.001), not
 * a measured value. Every driver that waits takes a struct wb_time and a
 * timeout, so that no poll loop is unbounded.
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/drivers/mmr.h"

struct wb_time {
	uint32_t cycles_per_us;		/* nonzero */
};

#define WB_TIME_OK       0
#define WB_TIME_TIMEOUT (-1)

uint64_t wb_time_us(const struct wb_time *t);
uint64_t wb_time_ms(const struct wb_time *t);

/* A cycle count us microseconds from now, and whether it has passed. */
uint64_t wb_time_deadline(const struct wb_time *t, uint32_t us);
bool wb_time_passed(uint64_t deadline);

void wb_delay_us(const struct wb_time *t, uint32_t us);

/*
 * Wait until (register & mask) == value, reading at least once and giving
 * up timeout_us after the call. Returns WB_TIME_OK or WB_TIME_TIMEOUT.
 */
int wb_poll32(const struct wb_time *t, wb_mmr_addr addr, uint32_t mask,
	      uint32_t value, uint32_t timeout_us);

/*
 * A struct s06_clock now_us callback; ctx is a const struct wb_time *. It
 * wraps every 2^32 us as s06_clock requires.
 */
uint32_t wb_time_now_us(void *ctx);

#endif /* WAFFLE_DRIVERS_BF70X_TIME_H */
