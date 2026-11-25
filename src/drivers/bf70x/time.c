/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Time from the core cycle counter (see waffle/drivers/bf70x_time.h). */
#include "waffle/drivers/bf70x_time.h"

#include "waffle/drivers/bf70x_core.h"
#include "waffle/util/div64.h"

uint64_t wb_time_us(const struct wb_time *t)
{
	return wb_udiv64(wb_core_cycles(), t->cycles_per_us);
}

/* floor(floor(c / a) / b) is floor(c / (a * b)). */
uint64_t wb_time_ms(const struct wb_time *t)
{
	return wb_udiv64(wb_time_us(t), 1000u);
}

uint64_t wb_time_deadline(const struct wb_time *t, uint32_t us)
{
	return wb_core_cycles() + (uint64_t)us * t->cycles_per_us;
}

bool wb_time_passed(uint64_t deadline)
{
	return wb_core_cycles() >= deadline;
}

void wb_delay_us(const struct wb_time *t, uint32_t us)
{
	uint64_t end = wb_time_deadline(t, us);

	while (!wb_time_passed(end))
		;
}

int wb_poll32(const struct wb_time *t, wb_mmr_addr addr, uint32_t mask,
	      uint32_t value, uint32_t timeout_us)
{
	uint64_t end = wb_time_deadline(t, timeout_us);

	for (;;) {
		bool late = wb_time_passed(end);

		if ((wb_mmr_read32(addr) & mask) == value)
			return WB_TIME_OK;
		if (late)
			return WB_TIME_TIMEOUT;
	}
}

/* cppcheck-suppress constParameterPointer ; s06_clock fixes the type */
uint32_t wb_time_now_us(void *ctx)
{
	return (uint32_t)wb_time_us(ctx);
}
