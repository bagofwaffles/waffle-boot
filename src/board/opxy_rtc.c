/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The RTC as the master's services use it: S-04.070's count register,
 * read and written through opxy_rtc_board.c's hooks over RTC0, the UTC
 * seconds S-12.016's stamps take, and the slave's clock message.
 */
#include "waffle/board/opxy_services.h"

#include "waffle/board/opxy_boot.h"

#include "waffle/boot/records.h"

/*
 * Days from 1970-01-01 to y-m-d in the Gregorian calendar, for the years
 * S-04.070's count holds (1970 to 2059): 32-bit arithmetic throughout.
 */
static uint32_t days_from_civil(uint32_t y, unsigned mo, unsigned d)
{
	uint32_t era, yoe, doy;

	y -= mo <= 2u ? 1u : 0u;
	era = y / 400u;
	yoe = y - era * 400u;
	doy = (153u * (mo > 2u ? mo - 3u : mo + 9u) + 2u) / 5u + d - 1u;
	return era * 146097u + yoe * 365u + yoe / 4u - yoe / 100u + doy - 719468u;
}

/* S-12.016: below 2^32 for every count (32769 days of 86400 seconds and
 * a time of day of at most 31:63:63). */
bool opxy_rtc_utc(struct opxy_master *m, uint64_t *utc)
{
	struct s04_datetime t;
	uint32_t clk;

	(void)m;
	if (!opxy_rtc_read(&clk))
		return false;
	s04_rtc_decode(clk, &t);
	*utc = days_from_civil(t.year, t.month, t.day) * 86400u +
	       t.hour * 3600u + t.minute * 60u + t.second;
	return true;
}

/*
 * S-09.046 (a clock that does not run): the register as it reads, sent
 * without waiting for the RTC's first second, 0 included; a clock that
 * never ticks never changes, so nothing follows. With the slave absent
 * nothing is sent: S-09.008 allows no bridge request, and the CPU link is
 * the tunnel's.
 */
void opxy_clock_to_slave(struct opxy_master *m)
{
	uint32_t clk;

	/* S-10b.063, S-09.046: second-cpu's alone */
	if (m->slave_present && wb_has(opxy_dev(m), WB_F_SECOND_CPU) && opxy_rtc_peek(&clk))
		s09_clock_rtc(&m->clock_msg, &m->node, S06_LINK_CPU, clk);
}
