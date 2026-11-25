/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The test OS's frame timing (src/testos/frame.c, scenario
 * HW-FRAME): TE's period and high time and the frames' times measured on
 * a model of TE and of a transfer, at 400 cycles a microsecond, each
 * sample 40 cycles later than the one before. The source is included with
 * OPXY_TEST_OS; the host library's object of it is empty.
 */
#define OPXY_TEST_OS 1
#include "../../src/testos/frame.c"

#include <string.h>

#include "support/check.h"

#define CPU  400u
#define STEP 40u

static struct {
	uint64_t t;		/* cycles */
	uint64_t period, high;	/* TE; period 0: TE stays low */
	uint64_t frame;		/* a transfer's length */
	uint64_t end;		/* the transfer's end */
	unsigned starts;
	bool refuse;
} g;

static uint64_t f_cycles(void *ctx)
{
	(void)ctx;
	g.t += STEP;
	return g.t;
}

static bool f_te(void *ctx)
{
	(void)ctx;
	return g.period != 0u && g.t % g.period < g.high;
}

static int f_start(void *ctx)
{
	(void)ctx;
	if (g.refuse)
		return -1;
	g.starts++;
	g.end = g.t + g.frame;
	return 0;
}

static bool f_busy(void *ctx)
{
	(void)ctx;
	return g.t < g.end;
}

static const struct opxy_testos_frame_ops ops = {
	f_cycles, f_te, f_start, f_busy, CPU, NULL,
};

static void setup(uint64_t period_us, uint64_t high_us, uint64_t frame_us)
{
	memset(&g, 0, sizeof g);
	g.t = 12345u;
	g.period = period_us * CPU;
	g.high = high_us * CPU;
	g.frame = frame_us * CPU;
}

static bool near(uint64_t v, uint64_t want)
{
	return v + 3u * STEP >= want && v <= want + 3u * STEP;
}

static bool has(const struct s14_text *t, const char *s)
{
	char b[S14_TEXT_CAP + 1u];

	memcpy(b, t->buf, t->len);
	b[t->len] = '\0';
	return strstr(b, s) != NULL;
}

/* 60 Hz, a 15.05 ms frame: every period, high time and frame as the
 * model's; each frame starts at its edge and leaves the rest of the
 * period. */
static void test_measure(void)
{
	struct opxy_testos_frame_time ft;
	struct s14_text te, fr;

	setup(16667, 120, 15050);
	CHECK(opxy_testos_frame_measure(&ops, &ft) == OPXY_FT_OK);
	CHECK(ft.period.n == OPXY_FT_PERIODS && ft.high.n == OPXY_FT_PERIODS);
	CHECK(near(ft.period.min, g.period) && near(ft.period.max, g.period));
	CHECK(near(ft.high.min, g.high) && near(ft.high.max, g.high));
	CHECK(g.starts == OPXY_FT_FRAMES && ft.frame.n == OPXY_FT_FRAMES);
	CHECK(near(ft.frame.min, g.frame) && near(ft.frame.max, g.frame));
	CHECK(ft.latency.max <= 3u * STEP);
	CHECK(ft.left.n == OPXY_FT_FRAMES && ft.overruns == 0u);
	CHECK(near(ft.left.min, g.period - g.frame) &&
	      near(ft.left.max, g.period - g.frame));
	opxy_testos_frame_lines(OPXY_FT_OK, &ft, CPU, &te, &fr);
	CHECK(has(&te, "period 16.66 (16.66-16.66) ms over 8, high 120 (120-120) us"));
	CHECK(has(&fr, ": 4, transfer 15.05 (15.05-15.05) ms (15.05 at 70 ns a byte), te to start 0 (0-0) us"));
	CHECK(has(&fr, ", left to next te 1.61 (1.61-1.61) ms, past next te 0"));
	CHECK(!te.overflow && !fr.overflow && fr.len < 200u && te.len < 200u);
}

/* A frame longer than the period: each ends after the next edge. */
static void test_overrun(void)
{
	struct opxy_testos_frame_time ft;

	setup(15385, 100, 16000);
	CHECK(opxy_testos_frame_measure(&ops, &ft) == OPXY_FT_OK);
	CHECK(ft.overruns == OPXY_FT_FRAMES && ft.left.n == 0u);
	CHECK(near(ft.frame.max, g.frame));
}

/* No TE, a frame refused, a transfer that never ends. */
static void test_failures(void)
{
	struct opxy_testos_frame_time ft;
	struct s14_text te, fr;
	uint64_t t0;

	setup(0, 0, 15050);
	t0 = g.t;
	CHECK(opxy_testos_frame_measure(&ops, &ft) == OPXY_FT_NO_TE);
	CHECK(g.t - t0 >= (uint64_t)OPXY_FT_TE_WAIT_US * CPU && g.starts == 0u);
	opxy_testos_frame_lines(OPXY_FT_NO_TE, &ft, CPU, &te, &fr);
	CHECK(has(&te, "no rising edge within 100 ms") && has(&fr, "none sent"));

	setup(16667, 120, 15050);
	g.refuse = true;
	CHECK(opxy_testos_frame_measure(&ops, &ft) == OPXY_FT_NO_FRAME);
	CHECK(ft.period.n == OPXY_FT_PERIODS && ft.frame.n == 0u);
	opxy_testos_frame_lines(OPXY_FT_NO_FRAME, &ft, CPU, &te, &fr);
	CHECK(has(&fr, "the frame did not start"));

	setup(16667, 120, 1000000);
	CHECK(opxy_testos_frame_measure(&ops, &ft) == OPXY_FT_STUCK);
	CHECK(g.starts == 1u && ft.frame.n == 0u);
	opxy_testos_frame_lines(OPXY_FT_STUCK, &ft, CPU, &te, &fr);
	CHECK(has(&fr, "the transfer did not end"));
}

int main(void)
{
	RUN(test_measure);
	RUN(test_overrun);
	RUN(test_failures);
	return check_summary("test_testos_frame");
}
