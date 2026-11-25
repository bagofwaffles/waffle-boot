/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The OP-XY master's RTC behind the board's hooks (board/ui/rtc_hooks.c): strong
 * definitions of opxy_rtc_read() and opxy_rtc_write(), whose weak ones and
 * declarations are the board's (waffle/board/services.h).
 */
#ifndef WAFFLE_BOARD_RTC_HOOKS_H
#define WAFFLE_BOARD_RTC_HOOKS_H

#include "waffle/drivers/bf70x/rtc.h"

/*
 * S-13a.071, called once as held mode starts; t is the board's time base,
 * kept until the run ends. Until it is called the hooks behave as the
 * weak ones do (the read fails, the write is dropped) and touch no RTC0
 * register (S-13a.070). A link that calls it takes board/ui/rtc_hooks.c's hooks
 * over the board's weak ones.
 */
void opxy_rtc_board_start(const struct wb_time *t);

#endif /* WAFFLE_BOARD_RTC_HOOKS_H */
