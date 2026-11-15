/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tests for RTC0 (src/drivers/bf70x/rtc.c) and the board's RTC hooks
 * (src/board/opxy_rtc_board.c): S-13a.070 - S-13a.072 over a model of the
 * RTC's 1 Hz domain in the MMR fake.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x_rtc.h"
#include "waffle/board/opxy_rtc_board.h"

/* As waffle/board/opxy_services.h declares them. */
bool opxy_rtc_read(uint32_t *clk);
void opxy_rtc_write(uint32_t clk);

static const struct wb_time cclk400 = { 400u };

#define SECOND   400000000ull		/* cycles at 400 MHz */
#define STEP     4000000ull		/* 10 ms per cycle-counter read */

/*
 * The RTC: 1 Hz edges at phase + n * SECOND cycles. At an edge STAT.SEC
 * sets, a pending 1 Hz write lands and WRPEND clears, and CLK counts on
 * (it reads 0 until the first edge). A 1 Hz write while one is pending is
 * ignored, as on the part, and counted.
 */
static struct {
	bool running;			/* the oscillator */
	bool stuck;			/* WRPEND never clears */
	uint64_t phase;
	uint64_t edges;			/* edges handled */
	uint32_t clk, init, stat;
	bool pend;
	wb_mmr_addr pend_addr;
	uint32_t pend_val;
	unsigned ignored;		/* 1 Hz writes lost */
	unsigned clk_writes;
} m;

static uint32_t clk_next(uint32_t c)
{
	uint32_t s = c & RTC_CLK_SEC_MASK;
	uint32_t mi = (c & RTC_CLK_MIN_MASK) >> RTC_CLK_MIN_SHIFT;
	uint32_t h = (c & RTC_CLK_HOUR_MASK) >> RTC_CLK_HOUR_SHIFT;
	uint32_t d = c >> RTC_CLK_DAY_SHIFT;

	if (++s == 60u) {
		s = 0;
		if (++mi == 60u) {
			mi = 0;
			if (++h == 24u) {
				h = 0;
				d++;
			}
		}
	}
	return d << RTC_CLK_DAY_SHIFT | h << RTC_CLK_HOUR_SHIFT |
	       mi << RTC_CLK_MIN_SHIFT | s;
}

static void advance(void)
{
	uint64_t now = fake_core_peek_cycles(), n;

	if (!m.running || now < m.phase)
		return;
	n = (now - m.phase) / SECOND + 1u;
	for (; m.edges < n; m.edges++) {
		m.stat |= RTC_STAT_SEC;
		m.clk = clk_next(m.clk);
		if (m.pend && !m.stuck) {
			m.pend = false;
			if (m.pend_addr == RTC0_CLK)
				m.clk = m.pend_val;
			else if (m.pend_addr == RTC0_INIT)
				m.init = m.pend_val;
		}
	}
}

static bool one_hz(wb_mmr_addr a)
{
	return a == RTC0_CLK || a == RTC0_ALM || a == RTC0_STPWTCH ||
	       a == RTC0_INIT;
}

static bool rtc_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		     wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	advance();
	if (op == FAKE_MMR_READ) {
		if (addr == RTC0_STAT) {
			*value = m.stat | (m.pend ? RTC_STAT_WRPEND : 0u);
			m.stat = 0;
		} else if (addr == RTC0_CLK) {
			*value = m.edges != 0u ? m.clk : 0u;
		} else if (addr == RTC0_INIT) {
			*value = m.init;
		}
		return true;
	}
	if (op == FAKE_MMR_WRITE && one_hz(addr)) {
		if (addr == RTC0_CLK)
			m.clk_writes++;
		if (m.pend) {
			m.ignored++;
		} else {
			m.pend = true;
			m.pend_addr = addr;
			m.pend_val = *value;
			/* with the oscillator off, INIT lands at once (the
			 * 1 Hz domain is not clocked): the fake's guess, as
			 * the HRM does not say; a unit decides */
			if (addr == RTC0_INIT && !m.running) {
				m.pend = false;
				m.init = *value;
				m.running = (*value & RTC_INIT_PWDN) == 0u;
				m.phase = fake_core_peek_cycles() + SECOND / 2u;
			}
		}
	}
	return true;
}

static void reset(bool running, uint32_t init, uint32_t clk)
{
	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycles(1000u);
	fake_core_set_cycle_step(STEP);
	memset(&m, 0, sizeof m);
	m.running = running;
	m.phase = 1000u + SECOND / 3u;
	m.init = init;
	m.clk = clk;
	fake_mmr_set_hook(rtc_hook, NULL);
}

static bool rtc_reg(wb_mmr_addr a)
{
	return a >= RTC0_BASE && a <= 0x200D12A0u;
}

static unsigned writes(void)
{
	const struct fake_mmr_access *l = fake_mmr_log();
	unsigned n = 0;
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (l[i].op == FAKE_MMR_WRITE)
			n++;
	return n;
}

static unsigned rtc_accesses(void)
{
	const struct fake_mmr_access *l = fake_mmr_log();
	unsigned n = 0;
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (l[i].op != FAKE_MMR_CSYNC && rtc_reg(l[i].addr))
			n++;
	return n;
}

/*
 * S-13a.071's CAL rule and S-13a.072, as check_order.py rtc states them,
 * over the log; true when no rule fails.
 */
static bool protocol_ok(void)
{
	const struct fake_mmr_access *l = fake_mmr_log();
	bool init_seen = false, pending = false, have_cal = false;
	uint32_t cal = 0;
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++) {
		const struct fake_mmr_access *e = &l[i];

		if (e->op == FAKE_MMR_READ && e->addr == RTC0_INIT) {
			cal = e->value & RTC_INIT_CAL_MASK;
			have_cal = true;
		}
		if (e->op == FAKE_MMR_WRITE && e->addr == RTC0_INIT &&
		    (!have_cal || (e->value & RTC_INIT_CAL_MASK) != cal))
			return false;
		if (e->op == FAKE_MMR_READ && e->addr == RTC0_STAT) {
			if (!(e->value & RTC_STAT_WRPEND))
				pending = false;
		} else if (e->op == FAKE_MMR_WRITE && one_hz(e->addr)) {
			if (e->addr != RTC0_INIT && !init_seen)
				return false;
			if (pending)
				return false;
			init_seen = init_seen || e->addr == RTC0_INIT;
			pending = true;
		}
	}
	return true;
}

/* 2026-11-01 12:34:56 in S-04.070's encoding (days 20393). */
#define T1 (20393u << 17 | 12u << 12 | 34u << 6 | 56u)
#define T2 (20393u << 17 | 12u << 12 | 35u << 6 | 0u)

/* S-13a.070: no RTC0 access until held mode's set-up; the hooks then
 * behave as the board's weak ones. Runs first: the hooks' state is static. */
static void test_untouched_before_setup(void)
{
	struct wb_rtc r = { 0 };
	uint32_t clk = 0xA5A5u;

	reset(true, 0x35u, T1);
	CHECK(wb_rtc_read(&r, &clk) == WB_RTC_ERR_STATE);
	CHECK(wb_rtc_peek(&r, &clk) == WB_RTC_ERR_STATE);
	CHECK(wb_rtc_write(&r, T1) == WB_RTC_ERR_STATE);
	CHECK(!opxy_rtc_read(&clk));
	opxy_rtc_write(T1);
	CHECK(clk == 0xA5A5u);
	CHECK(fake_mmr_count() == 0u);
}

/* S-13a.071: CAL kept, PWDN and RDEN cleared, no interrupt enabled. */
static void test_held_start(void)
{
	static const struct fake_mmr_access want[] = {
		FAKE_R32(RTC0_STAT, 0),
		FAKE_R32(RTC0_INIT, 0x35u),
		FAKE_W32(RTC0_INIT, 0x05u),
		FAKE_W32(RTC0_IEN, 0),
	};
	const struct fake_mmr_access *l;
	struct wb_rtc r;
	size_t i;

	/* then STAT until the INIT write has landed */
	reset(true, 0x35u, T1);
	CHECK(wb_rtc_held_start(&r, &cclk400) == WB_RTC_OK);
	l = fake_mmr_log();
	CHECK(fake_mmr_count() > ARRAY_LEN(want));
	for (i = 0; i < ARRAY_LEN(want); i++)
		CHECK(l[i].op == want[i].op && l[i].addr == want[i].addr &&
		      l[i].value == want[i].value);
	for (; i < fake_mmr_count(); i++)
		CHECK(l[i].op == FAKE_MMR_READ && l[i].addr == RTC0_STAT);
	CHECK(!(l[fake_mmr_count() - 1u].value & RTC_STAT_WRPEND));
	CHECK(!m.pend && m.init == 0x05u);
	CHECK(protocol_ok());

	/* no edge: the set-up goes on after one edge's time */
	reset(true, 0x35u, T1);
	m.phase = ~0ull;
	CHECK(wb_rtc_held_start(&r, &cclk400) == WB_RTC_OK);
	CHECK(m.pend);
	CHECK(fake_core_peek_cycles() >= 400ull * WB_RTC_EDGE_US);

	/* an earlier write still pending: waits for it */
	reset(true, 0x0Au, T1);
	m.pend = true;
	m.pend_addr = RTC0_ALM;
	CHECK(wb_rtc_held_start(&r, &cclk400) == WB_RTC_OK);
	CHECK(fake_mmr_get(RTC0_INIT) == 0x0Au);
	CHECK(m.ignored == 0u);

	/* it never completes: nothing written, not set up */
	reset(true, 0x35u, T1);
	m.pend = true;
	m.stuck = true;
	CHECK(wb_rtc_held_start(&r, &cclk400) == WB_RTC_ERR_TIMEOUT);
	CHECK(writes() == 0u);
	CHECK(wb_rtc_read(&r, &(uint32_t){ 0 }) == WB_RTC_ERR_STATE);

	/* a powered-down oscillator is started */
	reset(false, RTC_INIT_PWDN | 0x3u, 0);
	CHECK(wb_rtc_held_start(&r, &cclk400) == WB_RTC_OK);
	CHECK(m.running);
	CHECK(m.init == 0x3u);
}

/* CLK reads 0 until the first seconds event: the first read waits for one. */
static void test_read(void)
{
	struct wb_rtc r;
	uint32_t clk = 0;
	uint64_t c0;

	reset(true, 0, T1);
	CHECK(wb_rtc_held_start(&r, &cclk400) == WB_RTC_OK);
	CHECK(wb_rtc_read(&r, &clk) == WB_RTC_OK);
	CHECK(clk != 0u);
	CHECK(clk == m.clk);
	CHECK(r.ticked);

	/* later reads do not wait */
	c0 = fake_core_peek_cycles();
	CHECK(wb_rtc_read(&r, &clk) == WB_RTC_OK);
	CHECK(fake_core_peek_cycles() - c0 < 4u * STEP);
}

/* No seconds event: the first read gives up after one edge's time, the
 * later ones do not wait; a tick that comes later is still taken. */
static void test_read_no_tick(void)
{
	struct wb_rtc r;
	uint32_t clk = 0;
	uint64_t c0;

	reset(true, 0, T1);
	m.phase = ~0ull;			/* no edge */
	CHECK(wb_rtc_held_start(&r, &cclk400) == WB_RTC_OK);
	/* S-09.046: the register as it is, at once: CLK reads 0 before the
	 * first seconds event, and 0 is a value to send */
	c0 = fake_core_peek_cycles();
	clk = 0xA5A5u;
	CHECK(wb_rtc_peek(&r, &clk) == WB_RTC_OK);
	CHECK(clk == 0u);
	CHECK(fake_core_peek_cycles() - c0 < 4u * STEP);
	c0 = fake_core_peek_cycles();
	CHECK(wb_rtc_read(&r, &clk) == WB_RTC_ERR_TIMEOUT);
	CHECK(fake_core_peek_cycles() - c0 >= 400ull * WB_RTC_EDGE_US);
	CHECK(fake_core_peek_cycles() - c0 < 400ull * WB_RTC_EDGE_US + 4u * STEP);
	c0 = fake_core_peek_cycles();
	CHECK(wb_rtc_read(&r, &clk) == WB_RTC_ERR_TIMEOUT);
	CHECK(fake_core_peek_cycles() - c0 < 4u * STEP);
	m.phase = fake_core_peek_cycles();
	CHECK(wb_rtc_read(&r, &clk) == WB_RTC_OK);
	CHECK(clk == T1 || clk == clk_next(T1));
}

/* S-13a.072 and S-10b.063: two sets in a row; each lands, none is
 * ignored, and a read at once after a set gives the time set. */
static void test_write(void)
{
	struct wb_rtc r;
	uint32_t clk = 0;

	reset(true, 0x35u, 0);
	CHECK(wb_rtc_held_start(&r, &cclk400) == WB_RTC_OK);
	CHECK(wb_rtc_write(&r, T1) == WB_RTC_OK);
	CHECK(m.clk == T1);
	CHECK(wb_rtc_read(&r, &clk) == WB_RTC_OK);
	CHECK(clk == T1);
	CHECK(wb_rtc_write(&r, T2) == WB_RTC_OK);
	CHECK(wb_rtc_read(&r, &clk) == WB_RTC_OK);
	CHECK(clk == T2);
	CHECK(m.ignored == 0u);
	CHECK(m.clk_writes == 2u);
	CHECK(m.init == 0x05u);
	CHECK(protocol_ok());
	CHECK(fake_mmr_errors() == 0u);

	/* the protocol check sees a write made while one is pending */
	reset(true, 0x35u, 0);
	CHECK(wb_rtc_held_start(&r, &cclk400) == WB_RTC_OK);
	wb_mmr_write32(RTC0_CLK, T1);
	CHECK(protocol_ok());
	wb_mmr_write32(RTC0_CLK, T2);
	CHECK(!protocol_ok());
}

static void test_write_refused(void)
{
	static const uint32_t bad[] = {
		60u, 60u << 6, 24u << 12, 31u << 12 | 63u << 6 | 63u,
	};
	struct wb_rtc r;
	unsigned i;

	reset(true, 0, 0);
	CHECK(wb_rtc_held_start(&r, &cclk400) == WB_RTC_OK);
	fake_mmr_clear_log();
	for (i = 0; i < ARRAY_LEN(bad); i++)
		CHECK(wb_rtc_write(&r, bad[i]) == WB_RTC_ERR_VALUE);
	CHECK(fake_mmr_count() == 0u);
	/* the largest valid value: 2059-09-20 23:59:59 */
	CHECK(wb_rtc_write(&r, 32767u << 17 | 23u << 12 | 59u << 6 | 59u) ==
	      WB_RTC_OK);

	/* no seconds event: the set is not made */
	reset(true, 0, 0);
	m.phase = ~0ull;
	CHECK(wb_rtc_held_start(&r, &cclk400) == WB_RTC_OK);
	CHECK(wb_rtc_write(&r, T1) == WB_RTC_ERR_TIMEOUT);
	CHECK(m.clk_writes == 0u);
}

/* The board's hooks over the driver after the set-up. */
static void test_hooks(void)
{
	uint32_t clk = 0;

	reset(true, 0x35u, 0);
	opxy_rtc_board_start(&cclk400);
	CHECK(m.init == 0x05u);
	opxy_rtc_write(T1);
	CHECK(opxy_rtc_read(&clk));
	CHECK(clk == T1);
	CHECK(protocol_ok());
	CHECK(rtc_accesses() == fake_mmr_count());
}

int main(void)
{
	RUN(test_untouched_before_setup);
	RUN(test_held_start);
	RUN(test_read);
	RUN(test_read_no_tick);
	RUN(test_write);
	RUN(test_write_refused);
	RUN(test_hooks);
	return check_summary("test_drv_rtc");
}
