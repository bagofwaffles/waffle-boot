/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-13c the display panel (src/s13c_panel.c) over a recording bus.
 * Requirements: S-13c.004 - S-13c.007, S-13c.011 - S-13c.013, S-13c.017,
 * S-13c.020 - S-13c.022. The whole runs are checked against
 * tests/oracle/S-13c-display/check_trace.py in tests/conformance/test_s13c.py;
 * these are the decision table, the record and the calls' states.
 */
#include <stdio.h>
#include <string.h>

#include "support/check.h"
#include "waffle/s13c_panel.h"

struct fake {
	uint64_t t;		/* us */
	char log[4096];
	size_t len;
	unsigned cmds, frames, reads;
	uint8_t last_cmd;
	bool busy;		/* the frame in flight never ends */
	unsigned stops;		/* frame_stop calls */
	uint64_t stop_t;	/* the time of the last */
	bool te;		/* TE edges come */
	int read_rc;
};

static struct fake g;
static const uint8_t fb[S13C_FRAME_BYTES];	/* a black frame */

static void logf_(const char *s)
{
	size_t n = strlen(s);

	if (g.len + n + 1u < sizeof g.log) {
		memcpy(g.log + g.len, s, n + 1u);
		g.len += n;
	}
}

static void f_reset(void *ctx, bool high)
{
	(void)ctx;
	logf_(high ? "R1 " : "R0 ");
}

static void f_select(void *ctx, bool on)
{
	(void)ctx;
	logf_(on ? "S " : "s ");
}

static void f_cmd(void *ctx, uint8_t c)
{
	char b[8];

	(void)ctx;
	snprintf(b, sizeof b, "%02X ", c);
	logf_(b);
	g.cmds++;
	g.last_cmd = c;
}

static void f_data(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	(void)p;
	(void)n;
}

static void f_repeat(void *ctx, const uint8_t *group, size_t len,
		     uint32_t count)
{
	(void)ctx;
	(void)group;
	(void)len;
	(void)count;
	logf_("fill ");
}

static int f_read(void *ctx, uint8_t cmd, uint8_t *out, size_t n)
{
	size_t i;

	(void)ctx;
	(void)cmd;
	g.reads++;
	for (i = 0; i < n; i++)
		out[i] = (uint8_t)(0x10u * i + cmd);
	return g.read_rc;
}

static int f_frame_start(void *ctx, const uint8_t *buf, uint32_t n)
{
	(void)ctx;
	(void)buf;
	CHECK(n == S13C_FRAME_BYTES);
	g.frames++;
	logf_("dma ");
	return 0;
}

static bool f_frame_busy(void *ctx)
{
	(void)ctx;
	return g.busy;
}

/* The frame's transfer stopped where it is: nothing sent. */
static void f_frame_stop(void *ctx)
{
	(void)ctx;
	g.stops++;
	g.stop_t = g.t;
	g.busy = false;
}

static int f_te_wait(void *ctx, uint32_t timeout_us)
{
	(void)ctx;
	if (!g.te) {
		g.t += timeout_us;
		return 1;
	}
	g.t += 100u;
	return 0;
}

static uint64_t f_now_us(void *ctx)
{
	(void)ctx;
	return g.t;
}

static void f_delay_us(void *ctx, uint32_t us)
{
	(void)ctx;
	g.t += us;
}

static const struct s13c_bus bus = {
	f_reset, f_select, f_cmd, f_data, f_repeat, f_read, f_frame_start,
	f_frame_busy, NULL, f_te_wait, f_now_us, f_delay_us, NULL,
};

/* The board's bus: a frame in flight can be stopped (S-13c.024). */
static const struct s13c_bus bus_stop = {
	f_reset, f_select, f_cmd, f_data, f_repeat, f_read, f_frame_start,
	f_frame_busy, f_frame_stop, f_te_wait, f_now_us, f_delay_us, NULL,
};

static void fresh(uint64_t t_us)
{
	memset(&g, 0, sizeof g);
	g.t = t_us;
	g.te = true;
}

/* S-13c.004 */
static void test_decide(void)
{
	uint32_t hs = 0;

	CHECK(s13c_reset_decide(S13C_HANDSHAKE_MAGIC, 2, 1, 50, &hs) ==
	      S13C_RESET_HANDSHAKE && hs == 2);
	/* YMOD 0 reads as 1 */
	CHECK(s13c_reset_decide(S13C_HANDSHAKE_MAGIC, 0, 1, 50, &hs) ==
	      S13C_RESET_HANDSHAKE && hs == 1);
	CHECK(s13c_reset_decide(0, 2, 1, 50, &hs) == S13C_RESET_PULSE);
	CHECK(s13c_reset_decide(S13C_HANDSHAKE_MAGIC ^ 1u, 2, 1, 50, &hs) ==
	      S13C_RESET_PULSE);
	/* a time not before the check's is no handshake */
	CHECK(s13c_reset_decide(S13C_HANDSHAKE_MAGIC, 50, 1, 50, &hs) ==
	      S13C_RESET_PULSE);
	CHECK(s13c_reset_decide(S13C_HANDSHAKE_MAGIC, 0xFFFFFFF0u, 1, 50, &hs) ==
	      S13C_RESET_PULSE);
	/* PA8 strapped low wins over the handshake */
	CHECK(s13c_reset_decide(S13C_HANDSHAKE_MAGIC, 2, 0, 50, &hs) ==
	      S13C_RESET_STRAP);
	CHECK(s13c_reset_decide(0, 0, 0, 50, &hs) == S13C_RESET_STRAP);
}

/* S-13c.021 */
static void test_record(void)
{
	uint8_t r[S13C_RECORD_SIZE];
	unsigned i, other = 0;

	memset(r, 0xA5, sizeof r);
	s13c_record(0x01020304u, r);
	CHECK(r[0] == 1 && r[0x28] == 1);
	CHECK(r[4] == 4 && r[5] == 3 && r[6] == 2 && r[7] == 1);
	CHECK(r[8] == 2 && r[9] == 0 && r[10] == 0 && r[11] == 0);
	for (i = 0; i < sizeof r; i++)
		if (i != 0 && (i < 4 || i > 11) && i != 0x28 && r[i] != 0)
			other++;
	CHECK(other == 0);
}

/* S-13c.004 - S-13c.006: the pulse, its absence, the wait. */
static void test_begin(void)
{
	struct s13c_panel p;

	fresh(50000);
	s13c_begin(&p, &bus, S13C_HANDSHAKE_MAGIC, 2, 1);
	CHECK(strcmp(g.log, "R1 ") == 0);
	CHECK(p.path == S13C_RESET_HANDSHAKE && p.reset_ms == 2);
	CHECK(p.ready_us == 3000u + 120000u);
	CHECK(p.state == S13C_RESET);

	fresh(50000);
	s13c_begin(&p, &bus, 0, 0, 1);
	CHECK(strcmp(g.log, "R1 R0 R1 ") == 0);
	CHECK(g.t == 50000u + S13C_RESET_LOW_US);
	CHECK(p.reset_ms == 50 && p.ready_us == g.t + 120000u);

	fresh(50000);
	s13c_begin(&p, &bus, S13C_HANDSHAKE_MAGIC, 2, 0);
	CHECK(strcmp(g.log, "R1 ") == 0);
	CHECK(p.path == S13C_RESET_STRAP && p.reset_ms == 50);
	CHECK(p.ready_us == 50000u + 120000u);

	/* a release in the first ms has time 1 */
	fresh(200);
	s13c_begin(&p, &bus, 0, 0, 1);
	CHECK(p.reset_ms == 1);
}

/* S-13c.006, S-13c.007, S-13c.011, S-13c.013 */
static void test_start(void)
{
	struct s13c_panel p;
	unsigned cmds;
	const char *init;

	fresh(50000);
	s13c_begin(&p, &bus, 0, 0, 1);
	g.len = 0;
	g.log[0] = '\0';
	CHECK(s13c_start(&p, false) == S13C_OK);
	CHECK(g.reads == 2 && p.id_valid);
	CHECK(p.rddid[0] == 0x14 && p.rdid4[2] == 0x03);
	init = "S 11 28 F0 F0 36 3A B4 B6 B7 B9 C0 C1 C2 C5 E8 E0 E1 F0 F0 35 21 "
	       "2A 2B 2C fill 2A 2B 29 ";
	CHECK(strcmp(g.log, init) == 0);
	if (strcmp(g.log, init) != 0)
		printf("  log: %s\n", g.log);
	CHECK(!p.gamma_loaded && !p.gamma_on && p.state == S13C_ON);
	/* the first transaction (the reads) no earlier than ready_us */
	CHECK(g.t >= p.ready_us + S13C_SLPOUT_WAIT_US);
	cmds = g.cmds;
	CHECK(s13c_start(&p, true) == S13C_OK);
	CHECK(g.cmds == cmds && g.reads == 2);	/* once per run */

	/* the gamma tables inside one more window before the black fill */
	fresh(50000);
	s13c_begin(&p, &bus, 0, 0, 1);
	g.read_rc = -1;
	CHECK(s13c_start(&p, true) == S13C_OK);
	CHECK(!p.id_valid);
	CHECK(strstr(g.log, "35 21 F0 F0 E2 E3 B9 F0 F0 2A 2B 2C fill") != NULL);
	CHECK(p.gamma_loaded && p.gamma_on);

	/* not before s13c_begin */
	memset(&p, 0, sizeof p);
	p.bus = &bus;
	CHECK(s13c_start(&p, false) == S13C_ERR_STATE);
}

/* S-13c.017 */
static void test_frames(void)
{
	struct s13c_panel p;

	fresh(50000);
	s13c_begin(&p, &bus, 0, 0, 1);
	CHECK(s13c_frame(&p, fb, true) == S13C_ERR_STATE);
	CHECK(s13c_start(&p, false) == S13C_OK);
	g.len = 0;
	g.log[0] = '\0';
	CHECK(s13c_frame(&p, fb, true) == S13C_OK);
	CHECK(strcmp(g.log, "2C dma ") == 0 && p.te_misses == 0);
	CHECK(s13c_frame_busy(&p) == false);

	/* the next frame waits for the one in flight; it never ends */
	CHECK(s13c_frame(&p, fb, false) == S13C_OK);
	g.busy = true;
	CHECK(s13c_frame_busy(&p));
	CHECK(s13c_frame(&p, fb, false) == S13C_ERR_BUS);
	CHECK(g.frames == 2);
	CHECK(s13c_gamma(&p, true) == S13C_ERR_BUS);
	g.busy = false;

	/* no TE: sent after the timeout, counted */
	g.te = false;
	CHECK(s13c_frame(&p, fb, true) == S13C_OK);
	CHECK(p.te_misses == 1 && g.frames == 3);
}

static void black_row(void *ctx, unsigned r, uint8_t *out)
{
	unsigned *rows = ctx;

	CHECK(r == *rows);
	memset(out, 0, S13C_ROW_BYTES);
	(*rows)++;
}

/* S-13c.012, S-13c.020, S-13c.021 */
static void test_handover(void)
{
	struct s13c_panel p;
	uint8_t rec[S13C_RECORD_SIZE];
	unsigned rows = 0, cmds;

	fresh(50000);
	s13c_begin(&p, &bus, S13C_HANDSHAKE_MAGIC, 7, 1);
	CHECK(s13c_start(&p, false) == S13C_OK);
	CHECK(s13c_handover(&p, rec) == S13C_ERR_STATE);	/* no boot frame */
	g.len = 0;
	g.log[0] = '\0';
	CHECK(s13c_boot_frame(&p, black_row, &rows) == S13C_OK);
	CHECK(rows == S13C_ROWS);
	CHECK(strcmp(g.log, "F0 F0 E2 E3 B9 F0 F0 2C ") == 0);
	CHECK(s13c_handover(&p, rec) == S13C_OK);
	CHECK(p.state == S13C_HANDED && rec[4] == 7 && rec[0] == 1);
	cmds = g.cmds;
	CHECK(s13c_frame(&p, fb, false) == S13C_ERR_STATE);
	CHECK(s13c_poweroff(&p) == S13C_ERR_STATE);
	CHECK(g.cmds == cmds);			/* no transaction follows */

	/* a frame after the boot frame (the failed load's held mode) */
	fresh(50000);
	s13c_begin(&p, &bus, 0, 0, 1);
	CHECK(s13c_start(&p, true) == S13C_OK);
	rows = 0;
	g.len = 0;
	g.log[0] = '\0';
	CHECK(s13c_boot_frame(&p, black_row, &rows) == S13C_OK);
	CHECK(strcmp(g.log, "2C ") == 0);	/* gamma already on */
	CHECK(s13c_frame(&p, fb, true) == S13C_OK);
	CHECK(s13c_handover(&p, rec) == S13C_ERR_STATE);
}

/* S-01.023: the front's panel taken on. This run's bus set-up leaves CS
 * high, so the first transaction selects it, once; the board then loads the
 * digital gamma itself (S-13c.012), and nothing of the init is resent. */
static void test_adopt(void)
{
	struct s13c_panel p;
	uint8_t rec[S13C_RECORD_SIZE];
	unsigned rows = 0;

	fresh(900000);
	s13c_adopt(&p, &bus, 40);
	CHECK(g.len == 0);			/* nothing on the bus yet */
	CHECK(s13c_gamma(&p, true) == S13C_OK);
	CHECK(strcmp(g.log, "S F0 F0 E2 E3 B9 F0 F0 ") == 0);
	CHECK(s13c_start(&p, true) == S13C_OK);
	CHECK(strcmp(g.log, "S F0 F0 E2 E3 B9 F0 F0 ") == 0);	/* S-13c.013 */
	CHECK(s13c_boot_frame(&p, black_row, &rows) == S13C_OK);
	CHECK(rows == S13C_ROWS);
	CHECK(strcmp(g.log, "S F0 F0 E2 E3 B9 F0 F0 2C ") == 0);

	/* Without the board's load the hand-over frame loads it first. */
	fresh(900000);
	s13c_adopt(&p, &bus, 40);
	rows = 0;
	CHECK(s13c_boot_frame(&p, black_row, &rows) == S13C_OK);
	CHECK(strcmp(g.log, "S F0 F0 E2 E3 B9 F0 F0 2C ") == 0);
	CHECK(s13c_handover(&p, rec) == S13C_OK);
	CHECK(rec[0] == 1 && rec[4] == 40 && rec[0x28] == 1);

	/* held mode after the front: frames, a gamma change, power-off */
	fresh(900000);
	s13c_adopt(&p, &bus, 40);
	CHECK(s13c_frame(&p, fb, false) == S13C_OK);
	CHECK(s13c_frame(&p, fb, false) == S13C_OK);
	CHECK(strcmp(g.log, "S 2C dma 2C dma ") == 0);
	CHECK(s13c_gamma(&p, false) == S13C_OK);
	CHECK(strncmp(g.log, "S ", 2) == 0 && strstr(g.log + 2, "S ") == NULL);

	fresh(900000);
	s13c_adopt(&p, &bus, 40);
	CHECK(s13c_gamma(&p, false) == S13C_OK);
	CHECK(strncmp(g.log, "S F0 ", 5) == 0);

	fresh(900000);
	s13c_adopt(&p, &bus, 40);
	CHECK(s13c_poweroff(&p) == S13C_OK);
	CHECK(g.len == 0);			/* nothing sent (S-13c.022) */
}

/* S-13c.022: no frame after the power-off, the one in progress waited
 * for, stopped where it is once its bound of three frame times has run
 * out, and nothing sent: no DISPOFF, no select. */
static void test_poweroff(void)
{
	struct s13c_panel p;

	fresh(50000);
	s13c_begin(&p, &bus, 0, 0, 1);
	g.len = 0;
	g.log[0] = '\0';
	CHECK(s13c_poweroff(&p) == S13C_OK);	/* never on: nothing sent */
	CHECK(g.len == 0 && p.state == S13C_OFF);
	CHECK(s13c_start(&p, false) == S13C_ERR_STATE);

	fresh(50000);
	s13c_begin(&p, &bus, 0, 0, 1);
	CHECK(s13c_start(&p, false) == S13C_OK);
	CHECK(s13c_frame(&p, fb, true) == S13C_OK);
	g.len = 0;
	g.log[0] = '\0';
	CHECK(s13c_poweroff(&p) == S13C_OK);
	CHECK(g.len == 0 && p.state == S13C_OFF);
	CHECK(s13c_frame(&p, fb, true) == S13C_ERR_STATE);
	CHECK(s13c_gamma(&p, true) == S13C_ERR_STATE);
	CHECK(s13c_poweroff(&p) == S13C_OK);
	CHECK(g.len == 0);

	/* a frame that never ends: stopped once the bound, no shorter than
	 * three frame times (S-13c.018: a frame ends before the next TE
	 * edge, at most 20 ms on), has run out; still nothing sent */
	CHECK(S13C_POWEROFF_FRAME_US >= 3u * 20000u);
	fresh(50000);
	s13c_begin(&p, &bus_stop, 0, 0, 1);
	CHECK(s13c_start(&p, false) == S13C_OK);
	CHECK(s13c_frame(&p, fb, true) == S13C_OK);
	g.busy = true;
	g.len = 0;
	g.log[0] = '\0';
	{
		const uint64_t t0 = g.t;

		CHECK(s13c_poweroff(&p) == S13C_OK);
		CHECK(g.stops == 1u);
		CHECK(g.stop_t - t0 >= S13C_POWEROFF_FRAME_US &&
		      g.stop_t - t0 < S13C_POWEROFF_FRAME_US + 1000u);
	}
	CHECK(g.len == 0 && p.state == S13C_OFF && !s13c_frame_busy(&p));
	CHECK(s13c_poweroff(&p) == S13C_OK && g.stops == 1u && g.len == 0);

	/* a frame that ends within the bound is not stopped */
	fresh(50000);
	s13c_begin(&p, &bus_stop, 0, 0, 1);
	CHECK(s13c_start(&p, false) == S13C_OK);
	CHECK(s13c_frame(&p, fb, true) == S13C_OK);
	g.len = 0;
	g.log[0] = '\0';
	CHECK(s13c_poweroff(&p) == S13C_OK && g.stops == 0u && g.len == 0);

	/* a bus that cannot stop one: the power-off still goes on after the
	 * bound, nothing sent */
	fresh(50000);
	s13c_begin(&p, &bus, 0, 0, 1);
	CHECK(s13c_start(&p, false) == S13C_OK);
	CHECK(s13c_frame(&p, fb, true) == S13C_OK);
	g.busy = true;
	g.len = 0;
	g.log[0] = '\0';
	{
		const uint64_t t0 = g.t;

		CHECK(s13c_poweroff(&p) == S13C_ERR_BUS);
		CHECK(g.t - t0 >= S13C_POWEROFF_FRAME_US &&
		      g.t - t0 < S13C_POWEROFF_FRAME_US + 1000u);
	}
	CHECK(g.len == 0 && p.state == S13C_OFF);
	g.busy = false;
}

int main(void)
{
	RUN(test_decide);
	RUN(test_record);
	RUN(test_begin);
	RUN(test_start);
	RUN(test_frames);
	RUN(test_handover);
	RUN(test_adopt);
	RUN(test_poweroff);
	return check_summary("test_s13c_panel");
}
