/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The master's shutdown (opxy_shutdown(), S-04.063) over a loopback bridge
 * to s09_slave: scenario SC-03-SHUTDOWN-UNMOUNT's master half. With the
 * volume mounted, the unmount request (0x09) is sent and answered before
 * the slave's power-off request (0x31) goes out on the CPU link, and both
 * before the PSoC's reports. With no volume mounted (variant not_mounted,
 * and a mount that failed) no 0x09 is sent. An unmount with no reply
 * within T_UNMOUNT (S-09.060) does not stop the shutdown: it counts as
 * failed, the slave is not absent, and the power-off request and the
 * reports still go out (S-04.063). Outside a shutdown the same 0x09 waits
 * T_UNMOUNT and then fails as any deferred request does. test_board_storage
 * checks the slave's half.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/board/boot.h"
#include "waffle/board/services.h"
#include "waffle/drivers/bf70x/uart.h"
#include "waffle/link/link.h"
#include "waffle/bridge/master.h"
#include "waffle/bridge/slave.h"

static struct opxy_master m;
static struct s09_slave sl;

/* What happened, in order: U the slave served a 0x09, P a frame on the
 * CPU link (the power-off request), R a frame to the PSoC. */
static char ev[64];
static size_t ev_n;
static bool mount_ok, drop_unmount;

static void mark(char c)
{
	if (ev_n < sizeof(ev) - 1u)
		ev[ev_n++] = c;
}

/* ---- the slave's store: a volume that mounts, or does not ---- */

static int32_t st_emmc(void *c, uint8_t cmd, uint32_t a0, uint32_t a1,
		       uint8_t *data, uint32_t n)
{
	(void)c;
	(void)cmd;
	(void)a0;
	(void)a1;
	(void)data;
	(void)n;
	return -1;
}

static uint32_t st_erase_group(void *c) { (void)c; return S03_GROUP_SECTORS; }
static uint32_t st_boot_size(void *c) { (void)c; return 0x40000u; }
static uint32_t st_os(void *c, uint32_t len, uint32_t crc)
{
	(void)c;
	(void)len;
	(void)crc;
	return 1;
}
static uint64_t st_cycles(void *c) { (void)c; return 0; }
static void st_adjust(void *c, int64_t d) { (void)c; (void)d; }

static int32_t st_file(void *c, const struct s09_req *r, const char *p1,
		       const char *p2, const uint8_t *d, uint32_t n, uint8_t *o,
		       uint32_t *err)
{
	(void)c;
	(void)p1;
	(void)p2;
	(void)d;
	(void)n;
	(void)o;
	*err = 0;
	if (r->cmd == S09_MOUNT)
		return mount_ok ? 0 : -1;
	if (r->cmd == S09_UNMOUNT) {
		mark('U');
		return 1;
	}
	*err = S09_ENODEV;
	return -1;
}

static int32_t st_read(void *c, uint32_t fd, uint8_t *buf, uint32_t n,
		       uint32_t *err)
{
	(void)c;
	(void)fd;
	(void)buf;
	(void)n;
	*err = S09_ENODEV;
	return -1;
}

static const struct s09_store_ops store = {
	0, st_erase_group, st_boot_size, NULL /* dev_cmd */, st_emmc, st_file,
	st_read, st_os, st_cycles, st_adjust, NULL /* stage_write */
};

/* ---- the loopback bus (test_board_devtime's), on a virtual clock ---- */

static uint32_t now_us;

static uint32_t clk_now(void *c)
{
	(void)c;
	return now_us;
}

static void clk_idle(void *c, uint32_t until)
{
	(void)c;
	now_us = s06_time_reached(now_us, until) ? now_us + 1u : until;
}

static const struct s06_clock clk = { clk_now, clk_idle, NULL };

static struct {
	bool sel, reply_session, done;
	uint8_t req[4096];
	size_t req_n;
	const uint8_t *rep;
	size_t rep_n, rep_at;
} lb;

static bool lb_ready(void *c)
{
	(void)c;
	return lb.sel ? !lb.done : true;
}

static bool lb_attention(void *c)
{
	(void)c;
	return s09_slave_attention(&sl);
}

/* drop_unmount: the slave never takes a 0x09, so it never answers one. */
static void lb_select(void *c, bool on)
{
	(void)c;
	if (on && !lb.sel) {
		lb.reply_session = sl.reply_len != 0;
		lb.done = false;
		lb.req_n = 0;
		lb.rep_at = 0;
		if (lb.reply_session)
			(void)s09_slave_reply(&sl, &lb.rep, &lb.rep_n);
	} else if (!on && lb.sel && !lb.reply_session && lb.req_n >= 12) {
		size_t k = (size_t)s09_slave_rx_len(&sl, lb.req);

		if (!(drop_unmount && lb.req[0] == S09_UNMOUNT))
			s09_slave_request(&sl, lb.req, k < lb.req_n ? k : lb.req_n);
	}
	lb.sel = on;
}

static bool lb_send(void *c, const uint8_t *d, size_t n)
{
	(void)c;
	if (lb.reply_session || lb.req_n + n > sizeof lb.req)
		return false;
	memcpy(lb.req + lb.req_n, d, n);
	lb.req_n += n;
	lb.done = true;
	return true;
}

static bool lb_recv(void *c, uint8_t *d, size_t n)
{
	(void)c;
	if (!lb.reply_session || lb.rep_at + n > lb.rep_n)
		return false;
	memcpy(d, lb.rep + lb.rep_at, n);
	lb.rep_at += n;
	lb.done = lb.rep_at == lb.rep_n;
	return true;
}

static void lb_configure(void *c, int phase)
{
	(void)c;
	(void)phase;
}

static const struct s09_bus_ops bus = {
	0, lb_ready, lb_attention, lb_select, lb_send, lb_recv, lb_configure, NULL,
};

/* ---- the CPU link and the PSoC link: frames out, nothing in ---- */

static size_t no_read(void *ctx, uint8_t *buf, size_t max)
{
	(void)ctx;
	(void)buf;
	(void)max;
	return 0;
}

static size_t room(void *ctx)
{
	(void)ctx;
	return 4096u;
}

static void cpu_write(void *ctx, const uint8_t *data, size_t n)
{
	(void)ctx;
	(void)data;
	(void)n;
	mark('P');
}

static void psoc_write(void *ctx, const uint8_t *data, size_t n)
{
	(void)ctx;
	(void)data;
	(void)n;
	mark('R');
}

static const struct s06_port_ops cpu_port = { no_read, room, cpu_write };
static const struct s06_port_ops psoc_port = { no_read, room, psoc_write };

static uint8_t crc8(const uint8_t *p, size_t n)
{
	uint8_t c = 0;

	while (n--)
		c = (uint8_t)(c ^ *p++);
	return c;
}

/* The master with the bridge up; the 0x08 sent when `mount`. */
static void setup(bool mount)
{
	memset(&m, 0, sizeof(m));
	memset(&lb, 0, sizeof(lb));
	ev_n = 0;
	drop_unmount = false;
	fake_mmr_reset();
	fake_core_reset();
	fake_mmr_set(UART_STAT(OPXY_UART_PSOC), UART_STAT_THRE | UART_STAT_TEMT);
	m.hw.time = opxy_time;
	s09_slave_init(&sl, &store);
	s09_master_init(&m.bridge, &bus, &clk);
	CHECK(s09_master_start(&m.bridge));
	m.slave_present = true;
	s06_node_init(&m.node, crc8, &clk);
	s06_link_init(&m.link[S06_LINK_CPU], &cpu_port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_SLAVE);
	m.node.link[S06_LINK_CPU] = &m.link[S06_LINK_CPU];
	s06_link_init(&m.link[S06_LINK_PSOC], &psoc_port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_PSOC);
	m.node.link[S06_LINK_PSOC] = &m.link[S06_LINK_PSOC];
	m.psoc_ops.ctx = &m;
	s07_init(&m.psoc, &wb_device_opxy, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	s09_files_init(&m.sv.files, &m.bridge);
	if (mount)
		(void)s09_files_mount(&m.sv.files);
	ev_n = 0;
}

/* The first place of c in the events; ev_n if none. */
static size_t at(char c)
{
	size_t i;

	for (i = 0; i < ev_n; i++)
		if (ev[i] == c)
			return i;
	return ev_n;
}

static void test_mounted(void)
{
	mount_ok = true;
	setup(true);
	CHECK(m.sv.files.mounted);
	opxy_shutdown(&m);
	/* 0x09 served, then 0x31, then the reports */
	CHECK(at('U') == 0u && at('P') == 1u && at('R') == 2u);
	CHECK(ev_n >= 3u && ev[ev_n - 1u] == 'R');
	CHECK(!m.bridge.absent && !m.sv.files.mounted);
}

static void test_not_mounted(void)
{
	/* no mount in the session */
	mount_ok = true;
	setup(false);
	opxy_shutdown(&m);
	CHECK(at('U') == ev_n && at('P') == 0u && at('R') == 1u);

	/* a mount that failed */
	mount_ok = false;
	setup(true);
	CHECK(!m.sv.files.mounted);
	opxy_shutdown(&m);
	CHECK(at('U') == ev_n && at('P') == 0u && at('R') == 1u);
}

/* No reply within T_UNMOUNT: the unmount fails, the slave stays present,
 * and 0x31 then the reports follow. */
static void test_unmount_silent(void)
{
	uint32_t t0;

	mount_ok = true;
	setup(true);
	drop_unmount = true;
	t0 = now_us;
	opxy_shutdown(&m);
	CHECK(!m.bridge.absent && !m.sv.files.mounted);
	CHECK(m.sv.files.err == S09_EIO);
	CHECK(now_us - t0 >= S09_T_UNMOUNT_US);
	/* T_UNMOUNT, then at most the power-off's T_POWER and the drain */
	CHECK(now_us - t0 <= S09_T_UNMOUNT_US + S09_T_POWER_US + S06_MS(1000));
	CHECK(at('U') == ev_n && at('P') == 0u && at('R') == 1u);
	CHECK(ev[ev_n - 1u] == 'R');
}

/* Outside a shutdown, a 0x09 with no reply waits T_UNMOUNT, then fails as
 * any deferred request whose T_EXEC runs out (S-09.060). */
static void test_unmount_silent_alone(void)
{
	uint32_t t0;

	mount_ok = true;
	setup(true);
	drop_unmount = true;
	t0 = now_us;
	CHECK(!s09_files_unmount(&m.sv.files));
	CHECK(now_us - t0 >= S09_T_UNMOUNT_US);
	CHECK(m.bridge.absent && m.sv.files.err == S09_ENODEV);
	CHECK(at('U') == ev_n && ev_n == 0u);
}

static size_t count_of(char c)
{
	size_t i, n = 0;

	for (i = 0; i < ev_n; i++)
		n += ev[i] == c;
	return n;
}

/*
 * S-04.066 (P-SHUT-1F): with one CPU, the volume's unmount on the main
 * CPU's own store (its S-03.013 is test_board_storage_1f's), then the
 * reports: 0x51 {00 00 02} and no 0x40 {01}; no power-off request, nothing
 * on the CPU link. An unmounted volume gets no 0x09.
 */
static void test_one_cpu(void)
{
	size_t opxy_frames;

	mount_ok = true;
	setup(true);
	opxy_shutdown(&m);
	opxy_frames = count_of('R');		/* 0x51 {00 00 02} and 0x40 {01} */
	CHECK(opxy_frames == 2u);

	for (int mounted = 1; mounted >= 0; mounted--) {
		mount_ok = true;
		setup(false);
		m.dev = &wb_device_op1f;
		s09_master_init(&m.bridge, &bus, &clk);
		s09_master_local(&m.bridge, &store);
		s07_init(&m.psoc, &wb_device_op1f, &m.node, S06_LINK_PSOC, &m.psoc_ops);
		s09_files_init(&m.sv.files, &m.bridge);
		if (mounted)
			CHECK(s09_files_mount(&m.sv.files) >= 0 && m.sv.files.mounted);
		ev_n = 0;
		lb.req_n = 0;
		opxy_shutdown(&m);
		CHECK(count_of('P') == 0u);		/* no CPU link frame */
		CHECK(count_of('R') == opxy_frames - 1u);	/* no 0x40 {01} */
		CHECK(mounted ? at('U') == 0u && at('R') == 1u : at('U') == ev_n);
		CHECK(ev[ev_n - 1u] == 'R' && !m.sv.files.mounted);
		CHECK(lb.req_n == 0u);			/* nothing on any bus */
	}
}

int main(void)
{
	RUN(test_mounted);
	RUN(test_not_mounted);
	RUN(test_unmount_silent);
	RUN(test_unmount_silent_alone);
	RUN(test_one_cpu);
	return check_summary("test_board_shutdown");
}
