/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-13c.017 in the test OS: a frame sent without a TE edge is logged as a
 * timeout, and a paced frame that saw its edge is not. The source is
 * included with OPXY_TEST_OS, so the test does not use the host library's
 * display/panel.o (the same interface, which this object then defines).
 */
#define OPXY_TEST_OS 1
#include "../../src/core/display/panel.c"

#include <string.h>

#include "support/check.h"

static unsigned timeouts;
static bool te_edge;
static uint64_t clock_us;

void opxy_testos_display_timeout(const char *what, uint32_t waited_us)
{
	(void)what;
	(void)waited_us;
	timeouts++;
}

static void f_pin(void *ctx, bool on) { (void)ctx; (void)on; }
static void f_cmd(void *ctx, uint8_t c) { (void)ctx; (void)c; }
static void f_data(void *ctx, const uint8_t *d, size_t n) { (void)ctx; (void)d; (void)n; }
static void f_repeat(void *ctx, const uint8_t *grp, size_t len, uint32_t count)
{
	(void)ctx; (void)grp; (void)len; (void)count;
}
static int f_read(void *ctx, uint8_t c, uint8_t *out, size_t n)
{
	(void)ctx; (void)c;
	memset(out, 0, n);
	return 0;
}
static int f_frame_start(void *ctx, const uint8_t *b, uint32_t n)
{
	(void)ctx; (void)b; (void)n;
	return 0;
}
static bool f_frame_busy(void *ctx) { (void)ctx; return false; }
static int f_te_wait(void *ctx, uint32_t timeout_us)
{
	(void)ctx;
	clock_us += te_edge ? 100u : timeout_us;
	return te_edge ? 0 : 1;
}
static uint64_t f_now_us(void *ctx) { (void)ctx; return clock_us; }
static void f_delay_us(void *ctx, uint32_t us) { (void)ctx; clock_us += us; }

static const struct s13c_bus fake_bus = {
	f_pin, f_pin, f_cmd, f_data, f_repeat, f_read, f_frame_start,
	f_frame_busy, NULL, f_te_wait, f_now_us, f_delay_us, NULL,
};

static const uint8_t fb[S13C_FRAME_BYTES];

int main(void)
{
	struct s13c_panel p;

	clock_us = 50000u;
	te_edge = true;
	s13c_begin(&p, &fake_bus, &wb_device_opxy, 0, 0, 1);
	CHECK(s13c_start(&p, false) == S13C_OK);
	timeouts = 0;
	CHECK(s13c_frame(&p, fb, true) == S13C_OK);
	CHECK(s13c_frame(&p, fb, true) == S13C_OK);
	CHECK(timeouts == 0 && p.te_misses == 0);

	te_edge = false;
	CHECK(s13c_frame(&p, fb, true) == S13C_OK);
	CHECK(timeouts == 1 && p.te_misses == 1);
	return check_summary("test_s13c_panel_testos");
}
