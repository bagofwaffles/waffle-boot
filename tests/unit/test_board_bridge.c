/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The master's wait for the init request session (S-09.018, S-09.060,
 * B-ABSENT init_never): opxy_bridge_init_ready() waits T_INIT for PA6
 * low, past T_REPLY, and gives up within it. The line the master logs
 * for the end the slave's replies use (S-13b.083).
 */
#include "support/check.h"
#include "support/fake_core.h"

#include "waffle/board/opxy_bridge.h"
#include "waffle/drivers/bf70x_spi.h"
#include "waffle/drivers/bf70x_time.h"

#include <string.h>

/* 1 ms of the fake clock on every cycle-counter read. */
#define CCLK_MHZ 400u
#define STEP (CCLK_MHZ * 1000u)

static const struct wb_time cclk = { CCLK_MHZ };
static uint64_t low_at_us;	/* PA6 low from here on; 0: never */

static uint64_t now_us(void)
{
	return fake_core_peek_cycles() / CCLK_MHZ;
}

static bool ready(void *ctx)
{
	(void)ctx;
	return low_at_us != 0u && now_us() >= low_at_us;
}

static const struct s09_bus_ops bus = { NULL, ready, NULL, NULL, NULL,
					NULL, NULL, NULL };

/* How long the wait took, in us; *ok its result. */
static uint64_t run(uint64_t low_after_us, bool *ok)
{
	uint64_t t0;

	fake_core_reset();
	fake_core_set_cycle_step(STEP);
	t0 = now_us();
	low_at_us = low_after_us != 0u ? t0 + low_after_us : 0u;
	*ok = opxy_bridge_init_ready(&bus, wb_time_deadline(&cclk, S09_T_INIT_US));
	return now_us() - t0;
}

static void test_bounds(void)
{
	CHECK(S09_T_INIT_US >= S06_MS(6000) && S09_T_INIT_US <= S06_MS(10000));
	CHECK(S09_T_INIT_US > S09_T_REPLY_US);
}

/* init_never: absent, not before 6 s and within T_INIT. */
static void test_never(void)
{
	bool ok = true;
	uint64_t us = run(0u, &ok);

	CHECK(!ok);
	CHECK(us >= 6000000u && us >= S09_T_INIT_US);
	CHECK(us <= (uint64_t)S09_T_INIT_US + 10000u);
}

/* A slave whose start-up outlasts T_REPLY is still found. */
static void test_late(void)
{
	bool ok = false;
	uint64_t us = run(3000000u, &ok);

	CHECK(ok);
	CHECK(us >= 3000000u && us < 3010000u);
}

static void test_ready_at_once(void)
{
	bool ok = false;
	uint64_t us = run(1u, &ok);

	CHECK(ok && us < 10000u);
}

/* S-13b.083: one line for each end, the TF end the build's default. */
static void test_end_line(void)
{
	const char *f = opxy_bridge_end_line(WB_SPI_END_TF);
	const char *c = opxy_bridge_end_line(WB_SPI_END_COUNT);

	CHECK(strstr(f, "TF") != NULL);
	CHECK(strstr(c, "count end") != NULL);
	CHECK(strcmp(f, c) != 0 && strlen(f) < 100u && strlen(c) < 100u);
	CHECK(WB_SPI_REPLY_END_BUILD == WB_SPI_END_TF);
}

int main(void)
{
	RUN(test_bounds);
	RUN(test_never);
	RUN(test_late);
	RUN(test_ready_at_once);
	RUN(test_end_line);
	return check_summary("test_board_bridge");
}
