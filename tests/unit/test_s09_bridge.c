/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09 bridge behavior. The slave server against replies.json (every reply
 * shape, S-09.030-S-09.036) and the undefined-request and noise rules
 * (S-09.016, S-09.037); then the master client run against the slave
 * server through a loopback bus: start-up and its failures (S-09.008,
 * S-09.015, S-09.017), deferred requests and polls (S-09.022, S-09.031),
 * the chunked read against file_read.json (S-09.035), the tunnel and the
 * CPU-link messages (S-09.033, S-09.034, S-09.045-S-09.047) and the release
 * (S-09.050-S-09.052).
 */
#include "waffle/boot/records.h"
#include "waffle/bridge/master.h"
#include "waffle/bridge/slave.h"
#include "waffle/link/bytes_compat.h"

#include "s06_test.h"
#include "s06_fake.h"
#include "vectors/s09_vectors.h"

/* ---- a fake store behind the slave ---- */

static struct {
	const uint8_t *emmc_data;	/* what a sector read returns */
	int32_t result;
	uint32_t err;
	const uint8_t *out;		/* the record a file command returns */
	uint32_t out_len;
	uint8_t file[0x3000];
	uint32_t file_len, pos;
	int read_calls, fail_call;
	uint32_t os;
	uint32_t os_len, os_crc;	/* the last release's arguments */
	int64_t adjusted;
	char last_path[64];
	int emmc_calls, file_calls;
} st;

static uint32_t f_erase_group(void *c) { (void)c; return 1024; }
static uint32_t f_boot_size(void *c) { (void)c; return 4194304; }

static int32_t f_emmc(void *c, uint8_t cmd, uint32_t a0, uint32_t a1,
		      uint8_t *data, uint32_t n)
{
	(void)c;
	(void)a0;
	(void)a1;
	st.emmc_calls++;
	/* the bf70x eMMC driver takes only 4-byte aligned buffers */
	if (cmd == S09_READ_SECTOR || cmd == S09_READ_SECTORS)
		CHECK(((uintptr_t)data & 3u) == 0);
	if ((cmd == S09_READ_SECTOR || cmd == S09_READ_SECTORS) && st.emmc_data)
		memcpy(data, st.emmc_data, n);
	return st.result;
}

static int32_t f_file(void *c, const struct s09_req *r, const char *p1,
		      const char *p2, const uint8_t *d, uint32_t n, uint8_t *out,
		      uint32_t *err)
{
	(void)c;
	(void)r;
	(void)p2;
	(void)d;
	(void)n;
	st.file_calls++;
	if (p1)
		snprintf(st.last_path, sizeof st.last_path, "%s", p1);
	if (st.out)
		memcpy(out, st.out, st.out_len);
	*err = st.err;
	return st.result;
}

static int32_t f_read(void *c, uint32_t fd, uint8_t *buf, uint32_t n,
		      uint32_t *err)
{
	uint32_t k;

	(void)c;
	(void)fd;
	if (++st.read_calls == st.fail_call) {
		*err = S09_ENODEV;
		return -1;
	}
	k = st.pos < st.file_len ? st.file_len - st.pos : 0;
	if (k > n)
		k = n;
	memcpy(buf, st.file + st.pos, k);
	st.pos += k;
	*err = 0;
	return (int32_t)k;
}

static uint32_t f_os(void *c, uint32_t len, uint32_t crc)
{
	(void)c;
	st.os_len = len;
	st.os_crc = crc;
	return st.os;
}
/* S-09.084: the slave's staging area, as the board's op keeps it */
static uint8_t staged[0x20000];
static uint32_t staged_calls, staged_off, staged_n;
static int32_t f_stage(void *c, uint32_t off, const uint8_t *d, uint32_t n)
{
	(void)c;
	staged_calls++;
	staged_off = off;
	staged_n = n;
	if (off > sizeof staged || n > sizeof staged - off)
		return -1;
	memcpy(staged + off, d, n);
	return 0;
}
static uint64_t f_cycles(void *c) { (void)c; return 78187493530ull; }
static void f_adjust(void *c, int64_t d) { (void)c; st.adjusted = d; }

static const struct s09_store_ops store = {
	0, f_erase_group, f_boot_size, NULL /* dev_cmd */, f_emmc, f_file, f_read, f_os, f_cycles,
	f_adjust, f_stage,
};

static struct s09_slave sl;

static void request(uint8_t cmd, uint8_t slot, uint16_t len, uint32_t a0,
		    uint32_t a1, const uint8_t *p, size_t n)
{
	static uint8_t b[70000];
	struct s09_req r = { cmd, slot, len, a0, a1 };

	s09_req_pack(&r, b);
	if (n)
		memcpy(b + 12, p, n);
	memset(b + 12 + n, 0, s09_pad4((uint32_t)n) - n);
	s09_slave_request(&sl, b, 12 + s09_pad4((uint32_t)n));
}

static const struct s09v_reply *reply_vec(const char *name)
{
	for (size_t i = 0; i < s09v_replies_count; i++)
		if (!strcmp(s09v_replies[i].name, name))
			return &s09v_replies[i];
	return 0;
}

static void expect_reply(const char *name)
{
	const struct s09v_reply *v = reply_vec(name);
	const uint8_t *d;
	size_t n;

	t_case = name;
	CHECK(v != 0);
	CHECK(s09_slave_reply(&sl, &d, &n));
	if (v)
		CHECK_MEM(d, n, v->reply, v->len);
}

static void poll_slave(uint8_t slot)
{
	request(S09_POLL, slot, 0, 0, 0, 0, 0);
}

static void fresh_slave(void)
{
	memset(&st, 0, sizeof st);
	s09_slave_init(&sl, &store);
	sl.linked = true;
}

/* S-09.030-S-09.036: every reply shape of replies.json */
static void test_slave_replies(void)
{
	static const uint8_t path[2] = { 'x', 0 };
	static const uint8_t tun[7] = { 0x02, 0x51, 0x00, 0x02, 0x01, 0xAA, 0x21 };
	const uint8_t *d;
	size_t n;

	fresh_slave();
	sl.linked = false;
	request(S09_INIT, 0, 0, 0, 0, 0, 0);
	expect_reply("imm_init");
	request(S09_ECHO, 0, 0, S09_ECHO_ARG, 0, 0, 0);
	expect_reply("imm_echo");
	CHECK(sl.linked);
	request(S09_ERASE_GROUP, 1, 0, 0, 0, 0, 0);
	expect_reply("imm_erase_group");
	request(S09_BOOT_SIZE, 1, 0, 0, 0, 0, 0);
	expect_reply("imm_boot_size");
	st.os = 1;
	{
		uint32_t v = 7;

		/* S-09.052: the hook fires once the reply session is out */
		CHECK(!s09_slave_take_release(&sl, &v));
		request(S09_RELEASE, 1, 0, 0, 0, 0, 0);
		CHECK(!s09_slave_take_release(&sl, &v));
		expect_reply("imm_release_noos");
		CHECK(s09_slave_take_release(&sl, &v));
		CHECK_EQ(v, 1);
		CHECK(!s09_slave_take_release(&sl, &v));
	}
	CHECK(!sl.released);			/* S-09.052: it keeps serving */
	poll_slave(2);
	expect_reply("poll_none");

	request(S09_SELECT, 1, 0, 1, 0, 0, 0);
	CHECK(s09_slave_attention(&sl));
	poll_slave(2);
	expect_reply("poll_select");
	CHECK(!s09_slave_attention(&sl));

	st.result = -1;
	st.err = 2;
	request(S09_ACCESS, 1, 2, 0, 0, path, 2);
	poll_slave(2);
	expect_reply("poll_access_enoent");
	CHECK(!strcmp(st.last_path, "/yaffs2/x"));	/* S-16.117 */
	st.err = 93;
	request(S09_RMDIR, 2, 2, 0, 0, path, 2);
	poll_slave(3);
	expect_reply("poll_rmdir_notempty");

	st.result = 0;
	st.err = 0;
	st.emmc_data = reply_vec("poll_read_sector")->reply + 12;
	request(S09_READ_SECTOR, 1, 0, 9, 0, 0, 0);
	poll_slave(2);
	expect_reply("poll_read_sector");
	st.emmc_data = reply_vec("poll_read_2")->reply + 12;
	request(S09_READ_SECTORS, 1, 0, 9, 2, 0, 0);
	poll_slave(2);
	expect_reply("poll_read_2");
	st.emmc_data = 0;

	st.out = reply_vec("poll_totalspace")->reply + 12;
	st.out_len = 8;
	request(S09_TOTAL_SPACE, 1, 2, 0, 0, path, 2);
	poll_slave(2);
	expect_reply("poll_totalspace");
	st.out = reply_vec("poll_stat")->reply + 12;
	st.out_len = S09_STAT_LEN;
	request(S09_STAT, 1, 2, 0, 0, path, 2);
	poll_slave(2);
	expect_reply("poll_stat");
	st.out = reply_vec("poll_statfs")->reply + 12;
	st.out_len = S09_STATFS_LEN;
	request(S09_STATFS, 1, 2, 0, 0, path, 2);
	poll_slave(2);
	expect_reply("poll_statfs");
	st.out = 0;

	/* S-09.034 */
	request(S09_PULL, 0, 0x1000, 0, 0, 0, 0);
	expect_reply("pull_none");
	CHECK(!s09_slave_attention(&sl));
	s09_slave_tunnel_port.write(&sl, tun, sizeof tun);
	CHECK(s09_slave_attention(&sl));
	poll_slave(1);
	expect_reply("pull_later");
	s09_slave_tunnel_port.write(&sl, tun, sizeof tun);
	request(S09_PULL, 0, 0x1000, 0, 0, 0, 0);
	expect_reply("pull_now");
	/* a receive overflow is reported by the next pull reply */
	{
		static uint8_t big[0x1000];

		for (int i = 0; i < 3; i++)
			request(S09_PUSH0, 3, 0x1000, 0, 0, big, sizeof big);
		s09_slave_tunnel_port.write(&sl, tun, sizeof tun);
		request(S09_PULL, 0, 0x1000, 0, 0, 0, 0);
		expect_reply("pull_overflow");
		request(S09_PULL, 0, 0x1000, 0, 0, 0, 0);
		CHECK(s09_slave_reply(&sl, &d, &n) && n == 8 && d[6] == 0);
	}

	/* S-09.035 pieces */
	for (uint32_t i = 0; i < 5; i++)
		st.file[i] = (uint8_t)i;
	st.file_len = 5;
	request(S09_READ, 1, 0x1000, 3, 5, 0, 0);
	poll_slave(2);
	expect_reply("read_piece_5");
	request(S09_READ, 1, 0x1000, 3, 5, 0, 0);
	poll_slave(2);
	expect_reply("read_piece_eof");
	st.fail_call = st.read_calls + 1;
	request(S09_READ, 1, 0x1000, 3, 5, 0, 0);
	poll_slave(2);
	expect_reply("read_piece_err");

	/* S-09.036 */
	request(S09_CLOCK_SYNC, 1, 0, 0, 0, 0, 0);
	CHECK_EQ(s09_slave_rx_len(&sl, 0), 8);
	{
		const uint8_t cnt[8] = { 0x10, 0, 0, 0, 0x12, 0, 0, 0 };

		s09_slave_request(&sl, cnt, 8);
		expect_reply("clock_sync");
		CHECK_EQ(st.adjusted, (int64_t)0x1200000010ll - (int64_t)78187493530ll);
	}

	/* S-09.051, S-09.052: after a release reply of 0 no further header */
	st.os = 0;
	request(S09_RELEASE, 1, 0, 0, 0, 0, 0);
	expect_reply("imm_release_ok");
	{
		uint32_t v = 7;

		CHECK(s09_slave_take_release(&sl, &v));
		CHECK_EQ(v, 0);
		request(S09_INIT, 0, 0, 0, 0, 0, 0);
		CHECK(!s09_slave_reply(&sl, &d, &n));
		CHECK(!s09_slave_take_release(&sl, &v));
	}
}

/* S-09.016, S-09.037 */
static void test_slave_undefined(void)
{
	static const uint8_t ids[4] = { 0x02, 0x0C, 0x33, 0x40 };
	static const uint8_t five[5] = { 1, 2, 3, 4, 5 };
	const uint8_t *d;
	size_t n;

	t_case = "noise";
	fresh_slave();
	sl.linked = false;
	{
		const uint8_t h[12] = { 0x01, 0x01, 0, 0 };

		CHECK_EQ(s09_slave_rx_len(&sl, h), 12);
		s09_slave_request(&sl, h, 12);
		CHECK(!s09_slave_reply(&sl, &d, &n));
	}
	request(S09_INIT, 0, 0, 0, 0, 0, 0);
	CHECK(s09_slave_reply(&sl, &d, &n) && n == 8);

	t_case = "undefined";
	sl.linked = true;
	for (int i = 0; i < 4; i++)
		for (int k = 0; k < 2; k++) {
			const uint8_t want[12] = { ids[i], 1, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 22 };
			uint8_t h[12];
			struct s09_req r = { ids[i], 1, (uint16_t)(k ? 5 : 0), 0, 0 };

			s09_req_pack(&r, h);
			CHECK_EQ(s09_slave_rx_len(&sl, h), k ? 20 : 12);
			request(ids[i], 1, (uint16_t)(k ? 5 : 0), 0, 0, five, k ? 5 : 0);
			CHECK(!s09_slave_reply(&sl, &d, &n));
			CHECK(s09_slave_attention(&sl));
			poll_slave(2);
			CHECK(s09_slave_reply(&sl, &d, &n));
			CHECK_MEM(d, n, want, 12);
		}
	/* then a defined request is served */
	request(S09_ERASE_GROUP, 1, 0, 0, 0, 0, 0);
	CHECK(s09_slave_reply(&sl, &d, &n) && n == 8);

	t_case = "bad_slot";
	{
		const uint8_t h[12] = { 0x21, 0x04, 0x05, 0x00 };

		CHECK_EQ(s09_slave_rx_len(&sl, h), 12);
		s09_slave_request(&sl, h, 12);
		CHECK(!s09_slave_reply(&sl, &d, &n));
		CHECK(!s09_slave_attention(&sl));
	}
}

/* ---- the loopback bus: the master against the slave ---- */

static struct t_sim sim;

static struct {
	bool sel, reply_session, done;
	uint8_t req[70000];
	size_t req_n;
	const uint8_t *rep;
	size_t rep_n, rep_at;
	int configured[2];
	int corrupt_init, silent_init, dead, no_attention;
	int late_reply;		/* PA6 never rises at a reply session's end */
	/* sessions seen: direction, header word, length */
	struct {
		int reply;
		uint32_t word;
		size_t len;
	} log[4096];
	size_t nlog;
} lb;

static bool lb_ready(void *c)
{
	(void)c;
	if (lb.dead)
		return false;
	if (!lb.sel)
		return !(lb.silent_init && sl.reply_len);
	return !lb.done || (lb.late_reply && lb.reply_session);
}

static bool lb_attention(void *c)
{
	(void)c;
	return !lb.dead && !lb.no_attention && s09_slave_attention(&sl);
}

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
	} else if (!on && lb.sel) {
		if (lb.nlog < 4096) {
			lb.log[lb.nlog].reply = lb.reply_session;
			lb.log[lb.nlog].word = lb.reply_session ?
				(lb.rep_n >= 4 ? s06_rd_le32(lb.rep) : 0) :
				(lb.req_n >= 4 ? s06_rd_le32(lb.req) : 0);
			lb.log[lb.nlog].len = lb.reply_session ? lb.rep_at : lb.req_n;
			lb.nlog++;
		}
		if (!lb.reply_session && lb.req_n >= 12) {
			size_t k = s09_slave_rx_len(&sl, lb.req);

			s09_slave_request(&sl, lb.req, k < lb.req_n ? k : lb.req_n);
		}
	}
	lb.sel = on;
}

/*
 * A request session takes the words the slave clocks in: the header's
 * s09_slave_rx_len() and s09_slave_rx_trail() bytes. Past them the slave's
 * ready line stays high and the master's next word never moves (S-09.010
 * step 2), so the send fails.
 */
static bool lb_send(void *c, const uint8_t *d, size_t n)
{
	(void)c;
	if (lb.reply_session || lb.req_n + n > sizeof lb.req)
		return false;
	if (lb.req_n == 0 && n >= S09_HDR &&
	    n > s09_slave_rx_len(&sl, d) + s09_slave_rx_trail(&sl, d))
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
	if (lb.corrupt_init && lb.rep_at == 0 && n >= 8 && d[0] == S09_INIT)
		d[4] ^= 1;
	lb.rep_at += n;
	lb.done = lb.rep_at == lb.rep_n;
	return true;
}

static void lb_configure(void *c, int phase)
{
	(void)c;
	lb.configured[phase]++;
}

static const struct s09_bus_ops bus = {
	0, lb_ready, lb_attention, lb_select, lb_send, lb_recv, lb_configure, NULL,
};

static struct s09_master ms;

static void fresh_pair(void)
{
	memset(&lb, 0, sizeof lb);
	memset(&st, 0, sizeof st);
	t_sim_init(&sim, S06_MS(600000));
	s09_slave_init(&sl, &store);
	s09_master_init(&ms, &bus, &sim.clock);
}

/* S-09.015, S-09.017, S-09.008 */
static void test_start(void)
{
	uint32_t err;
	struct s09_req sel = { S09_SELECT, 0, 0, 1, 0 };

	t_case = "M-START";
	fresh_pair();
	CHECK(s09_master_start(&ms));
	CHECK(ms.linked && sl.linked);
	CHECK_EQ(lb.configured[0], 1);
	CHECK_EQ(lb.configured[1], 1);
	/* init, its reply, echo, its reply, the first pull and its reply */
	CHECK(lb.nlog >= 6);
	CHECK_EQ(lb.log[0].word, 0x01);
	CHECK_EQ(lb.log[0].len, 12);
	CHECK_EQ(lb.log[1].len, 8);
	CHECK_EQ(lb.log[2].word, 0x0A);
	CHECK_EQ(lb.log[4].word & 0xFFFF, S09_PULL);
	CHECK_EQ(lb.log[4].word >> 16, 0x1000);
	CHECK(ms.pull_out && ms.pull_parked);
	CHECK_EQ(s09_master_call(&ms, &sel, 0, 0, 0, 0, 0, &err), 0);
	CHECK_EQ(err, 0);

	t_case = "init_wrong";
	fresh_pair();
	lb.corrupt_init = 1;
	CHECK(!s09_master_start(&ms));
	CHECK(ms.absent);
	{
		size_t before = lb.nlog;

		CHECK_EQ(s09_master_call(&ms, &sel, 0, 0, 0, 0, 0, &err), -1);
		CHECK_EQ(err, S09_ENODEV);
		CHECK_EQ(lb.nlog, before);	/* no bridge traffic */
	}

	t_case = "init_silent";
	fresh_pair();
	lb.silent_init = 1;
	CHECK(!s09_master_start(&ms));
	CHECK(ms.absent);
	CHECK(sim.now >= S06_MS(100) && sim.now <= S06_MS(1000) + 1000);
}

/* S-09.022, S-09.031 */
static void test_deferred(void)
{
	struct s09_req sel = { S09_SELECT, 0, 0, 2, 0 };
	uint32_t err;
	size_t first;

	t_case = "M-DEFER";
	fresh_pair();
	/* a sector read's data reach the eMMC's DMA on a 32-byte boundary */
	CHECK(((uintptr_t)(s09_slave_big(&sl) + S09_HDR) & (S09_DMA_ALIGN - 1u)) == 0u);
	CHECK(s09_master_start(&ms));
	first = lb.nlog;
	CHECK_EQ(s09_master_call(&ms, &sel, 0, 0, 0, 0, 0, &err), 0);
	/* the parked pull holds slot 0: the request takes 1, the poll 2 */
	CHECK_EQ(lb.log[first].word, 0x0118);
	CHECK_EQ(lb.log[first + 1].word, 0x0206);
	CHECK_EQ(lb.log[first + 2].word, 0x0118);
	CHECK_EQ(lb.log[first + 2].len, 12);

	/* PC5 never falls: the select fails within T_EXEC. It is S-09.013 rule
	 * 4's: an echo, the drain (a poll whatever PC5 reads, which takes the
	 * select's finished reply and discards it), EIO, the slave kept */
	t_case = "exec_silent";
	lb.no_attention = 1;
	{
		uint32_t t0 = sim.now;

		CHECK_EQ(s09_master_call(&ms, &sel, 0, 0, 0, 0, 0, &err), -1);
		CHECK_EQ(err, S09_EIO);
		CHECK(!ms.absent);
		CHECK(sim.now - t0 >= S09_T_EXEC_US);
		CHECK(sim.now - t0 <= S09_T_EXEC_US + S06_MS(50));
		CHECK(!s09_slave_attention(&sl));	/* nothing left for a later one */
		lb.no_attention = 0;
		CHECK_EQ(s09_master_call(&ms, &sel, 0, 0, 0, 0, 0, &err), 0);
	}

	t_case = "exec_silent_mount";	/* 0x08: within its own T_EXEC */
	fresh_pair();
	CHECK(s09_master_start(&ms));
	lb.no_attention = 1;
	{
		struct s09_req mount = { S09_MOUNT, 0, 0, 0, 0 };
		uint32_t t0 = sim.now;

		CHECK_EQ(s09_master_call(&ms, &mount, 0, 0, 0, 0, 0, &err), -1);
		CHECK_EQ(err, S09_ENODEV);
		CHECK(ms.absent);
		CHECK(sim.now - t0 >= S09_T_EXEC_MOUNT_US);
		CHECK(sim.now - t0 <= S09_T_EXEC_MOUNT_US + S06_MS(50));
	}

	/* S-09.013: the reply came whole, but PA6 stays low at the session's
	 * end beyond T_REPLY: none of it is used, the call fails with
	 * ENODEV and the slave is absent */
	t_case = "reply_late";
	fresh_pair();
	CHECK(s09_master_start(&ms));
	lb.late_reply = 1;
	{
		struct s09_req stat = { S09_STATFS, 0, 8, 0, 0 };
		uint8_t rec[12];
		uint32_t got = 99;
		size_t before;

		memset(rec, 0, sizeof rec);
		CHECK_EQ(s09_master_call(&ms, &stat, (const uint8_t *)"/yaffs2", 8,
					 rec, sizeof rec, &got, &err), -1);
		CHECK_EQ(err, S09_ENODEV);
		CHECK_EQ(got, 0);
		CHECK(ms.absent);
		before = lb.nlog;
		CHECK_EQ(s09_master_call(&ms, &sel, 0, 0, 0, 0, 0, &err), -1);
		CHECK_EQ(lb.nlog, before);
	}
}

/*
 * S-09.021: the master lengthens a padded payload of 0x44 or 0x48 to 0x4C;
 * the slave takes pad4 of the payload as the request and the rest of the
 * session as trailing bytes, so a path of every length goes through (once
 * a 64-character staging path failed with ENODEV).
 */
static void test_pad_sessions(void)
{
	char path[0x60];
	uint32_t err;

	t_case = "pad_session";
	fresh_pair();
	CHECK(s09_master_start(&ms));
	for (uint32_t n = 0x3Cu; n <= 0x50u; n++) {
		struct s09_req mk = { S09_MKDIR, 0, (uint16_t)n, 0, 0 };
		uint32_t pad = s09_pad4(n);
		uint8_t h[S09_HDR];
		int calls = st.file_calls;

		memset(path, 'a', n - 1u);
		path[n - 1u] = '\0';
		s09_req_pack(&mk, h);
		CHECK_EQ(s09_slave_rx_trail(&sl, h),
			 pad == 0x44u ? 8u : pad == 0x48u ? 4u : 0u);
		st.result = 0;
		CHECK_EQ(s09_master_call(&ms, &mk, (const uint8_t *)path, n, 0,
					 0, 0, &err), 0);
		CHECK_EQ(err, 0);
		CHECK_EQ(st.file_calls, calls + 1);
		CHECK(!ms.absent);
		if (ms.absent)
			break;
	}
	/* no trailing bytes for a header the slave ignores */
	{
		const uint8_t h[12] = { 0x21, 0x04, 0x41, 0x00 };

		CHECK_EQ(s09_slave_rx_trail(&sl, h), 0);
	}
}

/* S-09.060: the declared bounds, floors and ceilings */
static void test_bounds(void)
{
	t_case = "S-09.060";
	CHECK(S09_T_BOOT_READY_US >= S06_MS(200) &&
	      S09_T_BOOT_READY_US <= S06_MS(1000));
	CHECK(S09_T_REPLY_US >= S06_MS(100) && S09_T_REPLY_US <= S06_MS(1000));
	CHECK(S09_T_EXEC_MOUNT_US >= S06_MS(60000) &&
	      S09_T_EXEC_MOUNT_US <= S06_MS(120000));
	CHECK(S09_T_EXEC_US >= S06_MS(10000) && S09_T_EXEC_US <= S06_MS(30000));
	CHECK(S09_T_UNMOUNT_US >= S06_MS(30000) &&
	      S09_T_UNMOUNT_US <= S06_MS(60000));
	CHECK(S09_T_STREAM_US >= S06_MS(1000) && S09_T_STREAM_US <= S06_MS(5000));
	CHECK(S09_T_RELEASE_US >= S06_MS(10000) &&
	      S09_T_RELEASE_US <= S06_MS(30000));
	CHECK(S09_T_POWER_US >= S06_MS(1000) && S09_T_POWER_US <= S06_MS(5000));
}

/* S-09.035 against file_read.json */
static void test_file_reads(void)
{
	static uint8_t buf[0x3000];

	for (size_t i = 0; i < s09v_freads_count; i++) {
		const struct s09v_fread *v = &s09v_freads[i];
		size_t first, k = 0;
		uint32_t err;
		int32_t res;

		t_case = v->name;
		fresh_pair();
		CHECK(s09_master_start(&ms));
		st.file_len = v->file_len;
		st.pos = v->pos;
		for (uint32_t b = 0; b < v->file_len; b++)
			st.file[b] = (uint8_t)(b * 13u + 7u);
		st.fail_call = v->fail_piece >= 0 ? v->fail_piece + 1 : 0;
		first = lb.nlog;
		memset(buf, 0, sizeof buf);
		res = s09_master_read(&ms, 3, buf, v->n, &err);
		CHECK_EQ(res, v->result);
		CHECK_EQ(err, v->err);
		if (res > 0)
			CHECK_MEM(buf, (size_t)res, st.file + v->pos, (size_t)res);
		/* the pieces: each a poll reply session of the read */
		for (size_t s = first; s < lb.nlog; s++) {
			const struct s09v_piece *p;

			if (!lb.log[s].reply || (lb.log[s].word & 0xFF) != S09_READ)
				continue;
			CHECK(k < v->pn);
			if (k >= v->pn)
				break;
			p = &s09v_pieces[v->p0 + k++];
			CHECK_EQ(lb.log[s].len, p->reply_len);
		}
		CHECK_EQ(k, v->pn);
		/* the read's request: len is C = 0x1000 */
		CHECK_EQ(lb.log[first].word, 0x1000012Au);
	}
}

/* ---- the tunnel and the CPU link ---- */

static struct t_port unused;
static struct s06_link mlink, slink;
static struct s06_node mnode, snode;
static int slave_power_offs;
static uint32_t slave_clock;
static int slave_cpu_on;

static void s_power_off(void *c) { (void)c; slave_power_offs++; }
static void s_clock(void *c, uint32_t v) { (void)c; slave_clock = v; }

static const struct s09_cpu_ops cpu_ops = { 0, s_power_off, s_clock, 0 };

static void tick(void *c)
{
	(void)c;
	if (slave_cpu_on)
		s09_slave_cpu_poll(&snode, S06_LINK_CPU, &cpu_ops);
}

static void tunnel_setup(void)
{
	fresh_pair();
	(void)unused;
	s06_node_init(&mnode, t_crc8, &sim.clock);
	s06_node_init(&snode, t_crc8, &sim.clock);
	s06_link_init(&mlink, &s09_tunnel_port, &ms, 1, 5);
	s06_link_init(&slink, &s09_slave_tunnel_port, &sl, 5, 1);
	mnode.link[S06_LINK_CPU] = &mlink;
	snode.link[S06_LINK_CPU] = &slink;
	slave_power_offs = 0;
	slave_clock = 0;
	slave_cpu_on = 1;
	sim.tick = tick;
	CHECK(s09_master_start(&ms));
}

static int master_takes(uint8_t id, struct s06_frame *f)
{
	for (int i = 0; i < 20; i++) {
		s09_master_service(&ms);
		s09_slave_cpu_poll(&snode, S06_LINK_CPU, &cpu_ops);
		s09_master_service(&ms);
		s06_node_poll(&mnode);
		while (s06_link_take(&mlink, f))
			if (f->id == id)
				return 1;
	}
	return 0;
}

static void test_tunnel(void)
{
	static const uint8_t pl[3] = { 0xAA, 0x00, 0x55 };
	static const uint8_t bad[3] = { 1, 2, 3 };
	struct s06_frame f;

	t_case = "M-TUNNEL ping";
	tunnel_setup();
	CHECK(s06_send(&mnode, S06_LINK_CPU, 0, 0x01, pl, 3));
	CHECK(master_takes(0x02, &f));
	CHECK_EQ(f.route, 0x51);
	CHECK_MEM(f.payload, f.len, pl, 3);
	CHECK(ms.pull_out);		/* one pull outstanding again */

	t_case = "clock";
	{
		struct s09_clock_msg cm = { false, 0 };

		s09_clock_message(&cm, &mnode, S06_LINK_CPU, 1234);
		s09_clock_message(&cm, &mnode, S06_LINK_CPU, 1234);	/* no change */
		(void)master_takes(0xFF, &f);
		CHECK_EQ(slave_clock, 1234);
		CHECK_EQ(mlink.seq, 2);		/* the ping and one clock message */
	}

	t_case = "clock_rtc";			/* S-10b.063: a set, then at once */
	{
		struct s09_clock_msg cm = { false, 0 };
		uint32_t clk;

		CHECK(s04_rtc_encode(2026, 11, 1, 13, 14, 15, &clk));
		s09_clock_rtc(&cm, &mnode, S06_LINK_CPU, clk);
		(void)master_takes(0xFF, &f);
		CHECK_EQ(slave_clock, 1793538855u);	/* 2026-11-01T13:14:15Z */
		CHECK(s04_rtc_encode(2026, 11, 2, 0, 0, 0, &clk));
		s09_clock_rtc(&cm, &mnode, S06_LINK_CPU, clk);
		(void)master_takes(0xFF, &f);
		CHECK_EQ(slave_clock, 1793577600u);
	}

	t_case = "time_len3";			/* S-16.009: dropped, serving on */
	slave_clock = 0;
	CHECK(s06_send(&mnode, S06_LINK_CPU, 0, 0xBD, bad, 3));
	CHECK(s06_send(&mnode, S06_LINK_CPU, 0, 0x01, pl, 1));
	CHECK(master_takes(0x02, &f));
	CHECK_EQ(slave_clock, 0);

	t_case = "power_off";			/* S-09.047 answered */
	CHECK(s09_power_off(&ms, &mnode, S06_LINK_CPU));
	CHECK_EQ(slave_power_offs, 1);

	t_case = "power_silent";
	tunnel_setup();
	slave_cpu_on = 0;
	{
		uint32_t t0 = sim.now;

		CHECK(!s09_power_off(&ms, &mnode, S06_LINK_CPU));
		CHECK(sim.now - t0 >= S06_MS(1000));
		CHECK(sim.now - t0 <= S06_MS(5000) + S06_MS(100));
		/* S-09.046: the request on the wire is 0x31 with the one-byte
		 * payload 0x14 */
		s06_node_poll_link(&snode, S06_LINK_CPU);
		CHECK(s06_link_take(&slink, &f));
		CHECK_EQ(f.id, 0x31);
		CHECK_EQ(f.len, 1);
		CHECK_EQ(f.payload[0], 0x14);
	}

	t_case = "power_off_answer";	/* S-09.046: 0x32 {0x14}, then off */
	tunnel_setup();
	{
		static const uint8_t v14[1] = { 0x14 }, v0e[1] = { 0x0E };

		/* 14 decimal is not the power-off payload */
		CHECK(s06_send(&mnode, S06_LINK_CPU, 0, 0x31, v0e, 1));
		CHECK(!master_takes(0x32, &f));
		CHECK_EQ(slave_power_offs, 0);
		CHECK(s06_send(&mnode, S06_LINK_CPU, 0, 0x31, v14, 1));
		CHECK(master_takes(0x32, &f));
		CHECK_EQ(f.len, 1);
		CHECK_EQ(f.payload[0], 0x14);
		CHECK_EQ(slave_power_offs, 1);
	}
	sim.tick = 0;
}

/* S-09.050-S-09.052 */
static void test_release(void)
{
	struct s09_req sel = { S09_SELECT, 0, 0, 1, 0 };
	uint32_t err;
	size_t before;

	t_case = "reply_0";
	fresh_pair();
	CHECK(s09_master_start(&ms));
	st.os = 0;
	st.os_len = st.os_crc = 0xFFFFFFFFu;
	CHECK_EQ(s09_master_release(&ms), 0);
	CHECK(st.os_len == 0u && st.os_crc == 0u);	/* the boot partition's */
	before = lb.nlog;
	CHECK_EQ(s09_master_call(&ms, &sel, 0, 0, 0, 0, 0, &err), -1);
	CHECK_EQ(lb.nlog, before);		/* no further bridge request */

	t_case = "reply_1";
	fresh_pair();
	CHECK(s09_master_start(&ms));
	st.os = 1;
	CHECK_EQ(s09_master_release(&ms), 1);
	CHECK(!ms.absent);
	CHECK_EQ(s09_master_call(&ms, &sel, 0, 0, 0, 0, 0, &err), 0);

	t_case = "reply_7";
	fresh_pair();
	CHECK(s09_master_start(&ms));
	st.os = 7;
	CHECK_EQ(s09_master_release(&ms), -1);
	CHECK(ms.absent);

	/* S-09.082: a file start's release carries the file's length and CRC */
	t_case = "file";
	fresh_pair();
	CHECK(s09_master_start(&ms));
	st.os = 1;
	CHECK_EQ(s09_master_release_file(&ms, 742764u, 0x1234ABCDu), 1);
	CHECK(st.os_len == 742764u && st.os_crc == 0x1234ABCDu);
	CHECK(!ms.absent);
	st.os = 0;
	CHECK_EQ(s09_master_release_file(&ms, 9488u, 0u), 0);
	CHECK(st.os_len == 9488u && st.os_crc == 0u);
	before = lb.nlog;
	CHECK_EQ(s09_master_call(&ms, &sel, 0, 0, 0, 0, 0, &err), -1);
	CHECK_EQ(lb.nlog, before);
}

/*
 * S-09.084: an image the master holds goes to the slave's staging area in
 * writes of up to the write's most bytes, each acknowledged; a release
 * then names it as a file start's would. On the plain link a piece is at
 * most the tunnel's; a store on the master's own CPU has no staging area
 * of another CPU.
 */
static void test_stage_write(void)
{
	static uint8_t img[3u * S09_WRITE_MAX + 100u];
	uint32_t err = 0, at = 0;

	for (size_t i = 0; i < sizeof img; i++)
		img[i] = (uint8_t)(i * 13u + 5u);
	t_case = "stage_write";
	CHECK_EQ(s09_kind(S09_STAGE_WRITE), S09_K_DEFERRED);
	CHECK(!s09_plain_safe(S09_STAGE_WRITE));
	{
		const struct s09_req r = { S09_STAGE_WRITE, 0, 0x1000, 0, 0x9000 };

		CHECK_EQ(s09_payload_len(&r), 0x9000u);	/* arg1, as 0x2B's */
	}
	fresh_pair();
	memset(staged, 0, sizeof staged);
	staged_calls = 0;
	CHECK(s09_master_start(&ms));
	/* the plain link: a piece of more than the tunnel's is refused here */
	CHECK_EQ(s09_master_write_max(&ms), S09_TUNNEL_MAX);
	CHECK_EQ(s09_master_stage_write(&ms, 0, img, S09_TUNNEL_MAX + 1u, &err), -1);
	CHECK_EQ(err, S09_EINVAL);
	CHECK_EQ(s09_master_stage_write(&ms, 0, img, 0, &err), -1);
	CHECK_EQ(staged_calls, 0u);
	while (at < sizeof img) {
		const uint32_t k = sizeof img - at < S09_TUNNEL_MAX ? (uint32_t)sizeof img - at :
								     S09_TUNNEL_MAX;

		CHECK_EQ(s09_master_stage_write(&ms, at, img + at, k, &err), 0);
		CHECK(staged_off == at && staged_n == k);
		at += k;
	}
	CHECK(memcmp(staged, img, sizeof img) == 0);
	CHECK(!ms.absent);
	/* the release names the copy by its length and CRC */
	st.os = 0;
	CHECK_EQ(s09_master_release_file(&ms, (uint32_t)sizeof img, 0x0BADC0DEu), 0);
	CHECK(st.os_len == sizeof img && st.os_crc == 0x0BADC0DEu);

	/* a slave with no staging op refuses the write with EINVAL */
	t_case = "stage_write_none";
	fresh_slave();
	{
		static const struct s09_store_ops none = {
			0, f_erase_group, f_boot_size, NULL, f_emmc, f_file, f_read, f_os,
			f_cycles, f_adjust, NULL,
		};
		const uint8_t *d;
		size_t n;

		s09_slave_init(&sl, &none);
		sl.linked = true;
		request(S09_STAGE_WRITE, 1, 8, 0, 8, img, 8);
		poll_slave(2);
		CHECK(s09_slave_reply(&sl, &d, &n));
		CHECK(n >= 12u && wb_get_le32(d + 4) == 0xFFFFFFFFu && wb_get_le32(d + 8) == S09_EINVAL);
	}
	/* the same on a store with no link: there is no other CPU */
	t_case = "stage_write_local";
	{
		const struct s09_req r = { S09_STAGE_WRITE, 0, 8, 0, 8 };
		uint32_t got = 0;

		CHECK_EQ(s09_store_call(&store, &r, img, 8, NULL, 0, &got, &err), -1);
		CHECK_EQ(err, S09_EINVAL);
	}
}

/*
 * A driver reading request sessions from one byte stream, as the bus gives
 * them: each header's s09_slave_rx_len() bytes, of which the leading
 * s09_slave_rx_keep() go to the slave and the rest are discarded in pieces
 * (S-09.037). Returns the bytes left over at the end (0: read in step).
 */
static size_t feed_stream(const uint8_t *b, size_t n)
{
	size_t at = 0;

	while (n - at >= S09_HDR) {
		uint64_t want = s09_slave_rx_len(&sl, b + at);
		size_t keep = s09_slave_rx_keep(&sl, b + at);

		if (want > n - at)
			break;
		CHECK(keep <= want);
		s09_slave_request(&sl, b + at, keep);
		at += (size_t)want;
	}
	return n - at;
}

/* S-09.037: malformed requests (malformed.json) */
static void test_slave_malformed(void)
{
	static uint8_t whole[S09_HDR + 0x20000];
	static const uint8_t next[12] = { S09_ERASE_GROUP, 1, 0, 0 };

	for (size_t i = 0; i < s09v_malformed_count; i++) {
		const struct s09v_malformed *v = &s09v_malformed[i];
		size_t wn = v->len + v->fill_len;

		t_case = v->name;
		CHECK(wn + sizeof next <= sizeof whole);
		memcpy(whole, v->req, v->len);
		memset(whole + v->len, v->fill, v->fill_len);
		/* the slave takes the session whole: header and payload, a
		 * long item's arg1 x 512 bytes discarded (S-09.023) */
		fresh_slave();
		CHECK_EQ(s09_slave_rx_len(&sl, v->req), v->slave_rx_len);
		CHECK_EQ(s09_slave_rx_keep(&sl, v->req),
			 v->fill_len ? S09_HDR : v->slave_rx_len);
		/* handed the kept bytes alone, then the whole session */
		for (int pass = 0; pass < 2; pass++) {
			const uint8_t *d;
			size_t n;

			fresh_slave();
			st.result = 0;
			st.err = 0;
			s09_slave_request(&sl, whole, pass ? wn :
					  s09_slave_rx_keep(&sl, v->req));
			CHECK(!s09_slave_reply(&sl, &d, &n));	/* deferred */
			CHECK(s09_slave_attention(&sl));
			poll_slave(2);
			CHECK(s09_slave_reply(&sl, &d, &n));
			if (v->reply_len)
				CHECK_MEM(d, n, v->reply, v->reply_len);
			else	/* well formed: the file operation ran, on "ab" */
				CHECK(!strcmp(st.last_path, "/yaffs2/ab"));
			/* no eMMC operation; a file operation only when well
			 * formed */
			CHECK_EQ(st.emmc_calls, 0);
			CHECK_EQ(st.file_calls, v->reply_len ? 0 : 1);
			/* then a defined request is served */
			request(S09_ERASE_GROUP, 1, 0, 0, 0, 0, 0);
			CHECK(s09_slave_reply(&sl, &d, &n) && n == 8);
		}
		/* on one stream, the next header is read where the master
		 * sent it: the defined request after it is served */
		{
			const uint8_t *d;
			size_t n;

			fresh_slave();
			memcpy(whole + wn, next, sizeof next);
			CHECK_EQ(feed_stream(whole, wn + sizeof next), 0);
			CHECK(s09_slave_reply(&sl, &d, &n) && n == 8 &&
			      s06_rd_le32(d) == s09_hdr_word(S09_ERASE_GROUP, 1) &&
			      s06_rd_le32(d + 4) == 1024);
			CHECK(s09_slave_attention(&sl));	/* its error reply */
		}
	}

	/* S-09.023: write_128 (n = 128, len 0 on the wire) takes 128 x 512
	 * bytes, as a count of 129 or 200 does, and only those */
	t_case = "rx_len";
	fresh_slave();
	{
		static const struct { uint8_t cmd; uint16_t len; uint32_t arg1;
				      uint64_t rx; size_t keep; } c[] = {
			{ S09_WRITE_SECTORS, 0, 128, 12 + 0x10000, 12 + 0x10000 },
			{ S09_WRITE_RELIABLE, 0, 128, 12 + 0x10000, 12 + 0x10000 },
			{ S09_WRITE_SECTORS, 0x200, 129, 12 + 0x10200, 12 },
			{ S09_WRITE_RELIABLE, 5, 200, 12 + 0x19000, 12 },
			{ S09_WRITE_SECTORS, 8, 0, 12, 12 },
			{ S09_WRITE_SECTORS, 0, 0xFFFFFFFFu,
			  12 + 0xFFFFFFFFull * 512u, 12 },
			{ S09_READ_SECTORS, 4, 0, 16, 16 },	/* 0x12: pad4(len) */
			{ S09_READ_SECTORS, 5, 129, 20, 20 },
		};

		for (size_t k = 0; k < sizeof c / sizeof c[0]; k++) {
			struct s09_req r = { c[k].cmd, 1, c[k].len, 0, c[k].arg1 };
			uint8_t h[12];

			s09_req_pack(&r, h);
			CHECK(s09_slave_rx_len(&sl, h) == c[k].rx);
			CHECK_EQ(s09_slave_rx_keep(&sl, h), c[k].keep);
		}
	}
}

/* S-09.046 over S-04.070's encoding, across its skipped days */
static void test_clock_seconds(void)
{
	static const struct {
		unsigned y, mo, d, h, mi, s;
		uint32_t secs;
	} v[] = {
		{ 1970, 1, 1, 0, 0, 0, 0u },
		{ 2000, 12, 29, 23, 59, 59, 978134399u },
		{ 2001, 1, 1, 0, 0, 0, 978307200u },
		{ 2026, 11, 1, 13, 14, 15, 1793538855u },
		{ 2059, 9, 20, 23, 59, 59, 2831327999u },
	};
	static const uint32_t raw[][2] = {
		{ 0x00000000u, 0u },          { 0x55997EFBu, 946684799u },
		{ 0x5870C000u, 978091200u },  { 0x58720000u, 978307200u },
		{ 0x58740001u, 978393601u },  { 0x9F38C8B8u, 1761050096u },
		{ 0xFFFF7EFBu, 2831327999u },
	};
	uint32_t clk;

	t_case = "clock_seconds";
	for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
		CHECK(s04_rtc_encode(v[i].y, v[i].mo, v[i].d, v[i].h, v[i].mi,
				     v[i].s, &clk));
		CHECK_EQ(s09_clock_seconds(clk), v[i].secs);
	}
	/* tests/oracle/S-09-bridge/vectors/clock.json: register values, decoded date */
	for (size_t i = 0; i < sizeof raw / sizeof raw[0]; i++)
		CHECK_EQ(s09_clock_seconds(raw[i][0]), raw[i][1]);
}

int main(void)
{
	test_clock_seconds();
	test_bounds();
	test_slave_malformed();
	test_slave_replies();
	test_slave_undefined();
	test_start();
	test_deferred();
	test_pad_sessions();
	test_file_reads();
	test_tunnel();
	test_release();
	test_stage_write();
	return t_finish("test_s09_bridge");
}
