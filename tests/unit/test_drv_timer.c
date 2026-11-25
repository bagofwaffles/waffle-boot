/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tests for the time base, GP timers, core timer and watchdog
 * (src/drivers/bf70x/time.c, timer.c).
 */
#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x/coremmr.h"
#include "waffle/drivers/bf70x/timer.h"

static const struct wb_time cclk400 = { 400u };

static void reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
}

/* S-13a.031's vectors: cycle count, µs, ms. */
static void test_time_vectors(void)
{
	static const struct {
		uint64_t cycles, us, ms;
	} v[] = {
		{ 0, 0, 0 },
		{ 399, 0, 0 },
		{ 400, 1, 0 },
		{ 399999, 999, 0 },
		{ 400000, 1000, 1 },
		{ 400ull << 32, 1ull << 32, 4294967ull },
		{ 1ull << 40, 2748779069ull, 2748779ull },
	};
	unsigned i;

	for (i = 0; i < ARRAY_LEN(v); i++) {
		reset();
		fake_core_set_cycle_step(0);
		fake_core_set_cycles(v[i].cycles);
		CHECK(wb_time_us(&cclk400) == v[i].us);
		CHECK(wb_time_ms(&cclk400) == v[i].ms);
		CHECK(wb_time_now_us((void *)(uintptr_t)&cclk400) ==
		      (uint32_t)v[i].us);
	}
}

static void test_delay_and_deadline(void)
{
	uint64_t d;

	reset();
	fake_core_set_cycles(1000);
	fake_core_set_cycle_step(7);
	wb_delay_us(&cclk400, 10);
	/* Returns once 4000 cycles have passed, within one step. */
	CHECK(fake_core_peek_cycles() >= 1000u + 4000u);
	CHECK(fake_core_peek_cycles() <= 1000u + 4000u + 3u * 7u);

	reset();
	fake_core_set_cycle_step(0);
	d = wb_time_deadline(&cclk400, 1);
	CHECK(d == 400);
	CHECK(!wb_time_passed(d));
	fake_core_set_cycles(400);
	CHECK(wb_time_passed(d));
	/* A long timeout does not overflow 32 bits. */
	CHECK(wb_time_deadline(&cclk400, 0xFFFFFFFFu) ==
	      400u + 0xFFFFFFFFull * 400u);
}

static void test_poll(void)
{
	reset();
	fake_core_set_cycle_step(100);
	fake_mmr_queue_read(0x20001000u, 0);
	fake_mmr_queue_read(0x20001000u, 0);
	fake_mmr_set(0x20001000u, 0x5u);
	CHECK(wb_poll32(&cclk400, 0x20001000u, 0x4u, 0x4u, 10) == WB_TIME_OK);

	/* Never ready: gives up after the timeout, reading at least once. */
	reset();
	fake_core_set_cycle_step(400);
	CHECK(wb_poll32(&cclk400, 0x20001000u, 1u, 1u, 5) == WB_TIME_TIMEOUT);
	CHECK(fake_mmr_count() >= 5 && fake_mmr_count() <= 7);

	/* A zero timeout still reads once. */
	reset();
	fake_mmr_set(0x20001000u, 1u);
	CHECK(wb_poll32(&cclk400, 0x20001000u, 1u, 1u, 0) == WB_TIME_OK);
}

/* S-13a.032: at OSENTRY no timer runs. */
static void test_stop_all(void)
{
	reset();
	fake_mmr_set(TIMER0_DATA_IMSK, 0);
	wb_gptimer_stop_all();
	CHECK(fake_mmr_get(TIMER0_RUN_CLR) == 0xFFu);
	CHECK(fake_mmr_get(TIMER0_DATA_IMSK) == 0xFFu);
	CHECK(fake_mmr_get(TIMER0_DATA_ILAT) == 0xFFu);
}

static void test_core_timer(void)
{
	reset();
	fake_mmr_set(TMR_TCNTL, TMR_TCNTL_PWR | TMR_TCNTL_EN | TMR_TCNTL_INT);
	wb_coretmr_stop();
	CHECK(fake_mmr_get(TMR_TCNTL) == 0);
}

static void test_watchdog(void)
{
	static const struct fake_mmr_access want[] = {
		FAKE_W32(WDOG0_CTL, 0x8AD0u),
		FAKE_W32(WDOG0_CNT, 20000000u),
		FAKE_W32(WDOG0_CTL, 0),
		FAKE_SYNC,
	};

	reset();
	wb_wdog_disable();
	/* S-13a.060: WDEN = 0xAD. */
	CHECK(((fake_mmr_get(WDOG0_CTL) & WDOG_CTL_WDEN_MASK) >> 4) == 0xADu);

	reset();
	wb_wdog_start(20000000u);
	CHECK(fake_mmr_expect("wdog", want, ARRAY_LEN(want)));

	reset();
	wb_wdog_start(0);
	CHECK(fake_mmr_get(WDOG0_CNT) == 1u);
}

int main(void)
{
	RUN(test_time_vectors);
	RUN(test_delay_and_deadline);
	RUN(test_poll);
	RUN(test_stop_all);
	RUN(test_core_timer);
	RUN(test_watchdog);
	return check_summary("test_drv_timer");
}
