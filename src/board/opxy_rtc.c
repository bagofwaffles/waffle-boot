/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The RTC as the master's services use it: S-04.070's count register,
 * read and written through opxy_rtc_board.c's hooks over RTC0, the UTC
 * seconds S-12.016's stamps take, and the slave's clock message.
 */
#include "waffle/board/opxy_services.h"

#include "waffle/board/opxy_boot.h"

#include "waffle/s04_records.h"

/* Days from 1970-01-01 to y-m-d in the Gregorian calendar. */
static int64_t days_from_civil(int64_t y, unsigned mo, unsigned d)
{
	int64_t era, yoe, doy;

	y -= mo <= 2u;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (int64_t)(mo > 2u ? mo - 3u : mo + 9u) + 2) / 5 +
	      (int64_t)d - 1;
	return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

bool opxy_rtc_utc(struct opxy_master *m, uint64_t *utc)
{
	struct s04_datetime t;
	uint32_t clk;

	(void)m;
	if (!opxy_rtc_read(&clk))
		return false;
	s04_rtc_decode(clk, &t);
	*utc = (uint64_t)days_from_civil((int64_t)t.year, t.month, t.day) *
		       86400u +
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

	if (m->slave_present && opxy_rtc_peek(&clk))
		s09_clock_rtc(&m->clock_msg, &m->node, S06_LINK_CPU, clk);
}
