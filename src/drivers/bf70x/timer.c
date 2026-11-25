/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GP timers, core timer and watchdog (see waffle/drivers/bf70x/timer.h).
 */
#include "waffle/drivers/bf70x/timer.h"

#include "waffle/drivers/bf70x/coremmr.h"
#include "waffle/drivers/mmr.h"

void wb_gptimer_stop_all(void)
{
	const uint32_t all = (1u << WB_GPTIMERS) - 1u;

	wb_mmr_write32(TIMER0_RUN_CLR, all);
	wb_mmr_set32(TIMER0_DATA_IMSK, all);
	wb_mmr_write32(TIMER0_DATA_ILAT, all);
	wb_mmr_sync();
}

void wb_coretmr_stop(void)
{
	wb_mmr_write32(TMR_TCNTL, TMR_TCNTL_PWR);
	wb_mmr_write32(TMR_TCNTL, 0);
}

void wb_wdog_disable(void)
{
	wb_mmr_write32(WDOG0_CTL, WDOG_CTL_DISABLE | WDOG_CTL_WDRO);
	wb_mmr_sync();
}

void wb_wdog_start(uint32_t count)
{
	wb_mmr_write32(WDOG0_CTL, WDOG_CTL_DISABLE | WDOG_CTL_WDRO);
	wb_mmr_write32(WDOG0_CNT, count ? count : 1u);
	wb_mmr_write32(WDOG0_CTL, 0);	/* WDEN != 0xAD: counting */
	wb_mmr_sync();
}
