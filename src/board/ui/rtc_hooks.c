/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The OP-XY master's RTC: the board's RTC hooks over RTC0
 * (waffle/drivers/bf70x_rtc.h). Strong definitions of the board's weak
 * ones; the set-up lives here too, so a link that calls it takes these
 * over the weak ones.
 */
#include "waffle/board/rtc_hooks.h"

#include <stdbool.h>
#include <stdint.h>

/* As waffle/board/services.h declares them. */
bool opxy_rtc_read(uint32_t *clk);
bool opxy_rtc_peek(uint32_t *clk);
void opxy_rtc_write(uint32_t clk);

static struct wb_rtc rtc;

void opxy_rtc_board_start(const struct wb_time *t)
{
	(void)wb_rtc_held_start(&rtc, t);
}

bool opxy_rtc_read(uint32_t *clk)
{
	return wb_rtc_read(&rtc, clk) == WB_RTC_OK;
}

bool opxy_rtc_peek(uint32_t *clk)
{
	return wb_rtc_peek(&rtc, clk) == WB_RTC_OK;
}

/* A set the RTC does not take (no seconds event, WRPEND stuck) is
 * dropped and the clock keeps its time; MTP still answers OK, as the
 * answer is the value's, not the write's (S-10b.062). */
void opxy_rtc_write(uint32_t clk)
{
	(void)wb_rtc_write(&rtc, clk);
}
