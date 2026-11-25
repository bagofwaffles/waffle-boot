/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The hard reset both roles make (S-04.028, S-04.025). */
#include "waffle/board/boot.h"

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_rcu.h"
#include "waffle/drivers/bf70x_sec.h"
#include "waffle/drivers/bf70x_timer.h"
#include "waffle/drivers/mmr.h"

/*
 * S-13a.061: a cold start that keeps L1 scratch, the watchdog's expiry a
 * fault that resets the system, then the watchdog, and nothing after it.
 * Every RCU0_MSG bit is cleared, C0L1INIT among them (no stage keeps
 * another message across a reset), and the reset-cause bits, so the next
 * start-up reads this reset's alone. wb_sec_init() has set SEC0_GCTL.EN
 * in either role's bring-up.
 *
 * S-13b.072's wait for the UARTs comes before the record, so before this
 * (s04_reboot()'s drain_links): after the record nothing goes out on a
 * link (S-04.028). The slave opens no UART.
 */
void opxy_hard_reset(void)
{
	uint32_t bcode;

	(void)wb_core_cli();
	wb_mmr_write32(RCU0_MSG_CLR, RCU_MSG_ALL);
	bcode = wb_mmr_read32(RCU0_BCODE);
	bcode &= ~(RCU_BCODE_NOKERNEL | RCU_BCODE_NOVECTINIT | RCU_BCODE_HALT |
		   RCU_BCODE_HBTOVW | RCU_BCODE_NOCACHE | RCU_BCODE_NOFAULTS |
		   RCU_BCODE_NOPREBOOT);
	wb_mmr_write32(RCU0_BCODE, bcode | RCU_BCODE_NOMEMINIT);
	wb_mmr_write32(RCU0_STAT, RCU_STAT_CAUSES);
	wb_mmr_set32(RCU0_CTL, RCU_CTL_SRSTREQEN);
	(void)wb_sec_fault_reset(WB_SID_WDOG0);
	wb_wdog_start(OPXY_WDOG_COUNT);
	for (;;)
		wb_core_idle();
}
