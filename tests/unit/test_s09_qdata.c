/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Quad data replies (S-09.081) end to end at the protocol's level:
 * s09_master_link_qdata() and sector reads on the master, s09_slave serving
 * them through a loopback of the S-09 bus, a slave store that reads sectors
 * of a known pattern, and a stand-in for the board's quad reply test
 * (S-09.080). The loopback's quad receive hands the master a test reply or
 * a sector read's data session, whole or damaged.
 *
 * Covered: the mode exchange (0x19 keep mode 3 from mode 1 kept, and back),
 * the slave's rules for it; take-up only after every test reply came whole
 * with its flag; a slave without quad data (refused); the keep's reply lost;
 * 0x12's data in quad, 0x11 and the rest in single I/O; a data session
 * broken, damaged or never offered, and a head broken: the fall-back, the
 * read sent again on single I/O replies, its data right and the eMMC read
 * once; the drop on request and at the link's lowering; and the record.
 */
#include <string.h>

#include "waffle/bytes.h"
#include "waffle/s09_master.h"
#include "waffle/s09_slave.h"

#include "s06_test.h"
#include "s06_fake.h"

/* ---- the slave: sectors of a known pattern, and the test op ---- */

static uint32_t st_erase_group(void *c) { (void)c; return 1024u; }
static uint32_t st_boot_size(void *c) { (void)c; return 0x40000u; }
static uint32_t st_os(void *c) { (void)c; return 1u; }
static uint64_t st_cycles(void *c) { (void)c; return 0u; }
static void st_adjust(void *c, int64_t d) { (void)c; (void)d; }
static unsigned emmc_reads;		/* sector reads that reached the store */

static uint8_t sector_byte(uint32_t sector, uint32_t i)
{
	return (uint8_t)(sector * 7u + i * 13u + (i >> 9));
}

static int32_t st_emmc(void *c, uint8_t cmd, uint32_t a0, uint32_t a1,
		       uint8_t *data, uint32_t n)
{
	(void)c; (void)a1;
	if (cmd == S09_SELECT)
		return 0;
	if (cmd != S09_READ_SECTOR && cmd != S09_READ_SECTORS)
		return -1;
	emmc_reads++;
	for (uint32_t i = 0; i < n; i++)
		data[i] = sector_byte(a0 + i / S09_SECTOR, i % S09_SECTOR);
	return 0;
}

static int32_t st_file(void *c, const struct s09_req *r, const char *p1,
		       const char *p2, const uint8_t *d, uint32_t n, uint8_t *o,
		       uint32_t *err)
{
	(void)c; (void)r; (void)p1; (void)p2; (void)d; (void)n; (void)o;
	*err = S09_ENOENT;
	return -1;
}

static int32_t st_read(void *c, uint32_t fd, uint8_t *buf, uint32_t n, uint32_t *err)
{
	(void)c; (void)fd; (void)buf; (void)n;
	*err = S09_EBADF;
	return -1;
}

/* The board's quad reply test, as test_s09_qtest.c stands in for it. */
static struct {
	bool armed, due, damage;
	uint32_t arg0, len, sends;
	uint32_t rec[S09_QT_REC_WORDS];
	uint8_t small[8], out[S09_QT_REC_LEN];
	_Alignas(4) uint8_t buf[S09_QT_MAX];
} qs;

static size_t qs_value(uint8_t slot, uint32_t v)
{
	wb_put_le32(qs.small, s09_hdr_word(S09_QUAD_TEST, slot));
	wb_put_le32(qs.small + 4, v);
	return 8u;
}

static size_t st_qtest(void *c, const struct s09_req *r, const uint8_t **out)
{
	const bool armed = qs.armed;

	(void)c;
	qs.armed = false;
	qs.due = false;
	*out = qs.small;
	switch (S09_QT_OP(r->arg0)) {
	case S09_QT_ARM:
		if (!s09_qt_args_ok(r->arg0, r->arg1) || !s09_check_ready())
			return qs_value(r->slot, S09_QT_BAD);
		qs.armed = true;
		qs.arg0 = r->arg0;
		qs.len = r->arg1;
		return qs_value(r->slot, S09_QT_ARMED);
	case S09_QT_SEND:
		if (!armed || r->arg0 != S09_QT_SEND)
			return qs_value(r->slot, S09_QT_NOT_ARMED);
		s09_qt_build(qs.buf, qs.len, r->slot, qs.arg0);
		qs.due = true;
		*out = qs.buf;
		return qs.len;
	case S09_QT_RECORD:
		qs.rec[S09_QR_HDR] = s09_hdr_word(S09_QUAD_TEST, r->slot);
		for (unsigned k = 0; k < S09_QT_REC_WORDS; k++)
			wb_put_le32(qs.out + 4u * k, qs.rec[k]);
		*out = qs.out;
		return S09_QT_REC_LEN;
	default:
		return qs_value(r->slot, S09_QT_BAD);
	}
}

static const struct s09_store_ops store = {
	0, st_erase_group, st_boot_size, NULL /* dev_cmd */, st_emmc, st_file,
	st_read, st_os, st_cycles, st_adjust,
};

static struct s09_slave sl;

/* ---- the loopback bus ---- */

static struct t_sim sim;

/* What the loopback does with the next data session. */
enum { D_OK, D_BREAK, D_FLIP, D_NONE };

static struct {
	bool sel, reply_session, data_session, done;
	uint8_t req[0x11000];
	size_t req_n;
	const uint8_t *rep;
	size_t rep_n, rep_at;
	/* the slave's data session offered after a reply: its bytes, when */
	const uint8_t *qd;
	size_t qd_n;
	uint32_t qd_at;
	unsigned data_mode;		/* D_... for the next data session, then D_OK */
	bool break_head;		/* the next 0x12 head's session breaks */
	bool break_keep;		/* the next 0x19 mode 3's reply breaks */
	uint8_t last_cmd;
	uint32_t last_arg0;
	unsigned data_sessions, data_whole, test_quad;
	unsigned clocks[S09_QT_CLOCKS];
	unsigned cmds[256];
} lb;

static bool lb_ready(void *c)
{
	(void)c;
	if (lb.sel)
		return !lb.done;
	return true;
}

static bool lb_attention(void *c)
{
	(void)c;
	return s09_slave_attention(&sl);
}

static void lb_select(void *c, bool on)
{
	(void)c;
	if (on && !lb.sel) {
		lb.done = false;
		lb.req_n = 0;
		lb.rep_at = 0;
		lb.data_session = false;
		lb.reply_session = sl.reply_len != 0u;
		/* S-13b.085: the slave gives up a data session the master has
		 * not begun within T_REPLY */
		if (lb.qd_n != 0u && sim.now - lb.qd_at > S09_T_REPLY_US)
			lb.qd_n = 0;
		if (lb.qd_n != 0u) {
			lb.data_session = true;
			lb.reply_session = false;
		} else if (lb.reply_session) {
			(void)s09_slave_reply(&sl, &lb.rep, &lb.rep_n);
		}
	} else if (!on && lb.sel) {
		if (lb.data_session) {
			lb.qd_n = 0;
		} else if (lb.reply_session) {
			const uint8_t *d;
			size_t n;

			/* the data session follows a reply that went out whole */
			if (s09_slave_qdata(&sl, &d, &n) && lb.rep_at == lb.rep_n &&
			    lb.data_mode != D_NONE) {
				lb.qd = d;
				lb.qd_n = n;
				lb.qd_at = sim.now;
			} else if (lb.data_mode == D_NONE && lb.rep_n == 20u &&
				   lb.rep[0] == S09_READ_SECTORS) {
				lb.data_mode = D_OK;
			}
		} else if (lb.req_n >= S09_HDR) {
			const size_t k = (size_t)s09_slave_rx_len(&sl, lb.req);

			lb.cmds[lb.req[0]]++;
			lb.last_cmd = lb.req[0];
			lb.last_arg0 = wb_get_le32(lb.req + 4);
			s09_slave_request(&sl, lb.req, k < lb.req_n ? k : lb.req_n);
		}
	}
	lb.sel = on;
}

static bool lb_send(void *c, const uint8_t *d, size_t n)
{
	(void)c;
	if (lb.reply_session || lb.data_session || lb.req_n + n > sizeof lb.req)
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
	/* a quad session taken in single I/O is garbled */
	if (qs.due && lb.rep == qs.buf)
		return false;
	if (lb.break_head && lb.rep_n == 20u && lb.rep[0] == S09_READ_SECTORS) {
		lb.break_head = false;
		return false;
	}
	if (lb.break_keep && lb.last_cmd == S09_LINK_MODE &&
	    lb.last_arg0 == S09_MODE_QDATA) {
		lb.break_keep = false;
		return false;
	}
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

/* The master's quad receive: a test reply, or a sector read's data. */
static bool lb_quad(void *c, uint8_t *d, size_t n, unsigned clock, uint32_t out[4])
{
	(void)c;
	out[0] = out[1] = out[2] = out[3] = 0u;
	lb.clocks[clock % S09_QT_CLOCKS]++;
	if (lb.reply_session && qs.due && lb.rep == qs.buf && n == lb.rep_n) {
		uint32_t *r = qs.rec;

		lb.test_quad++;
		qs.due = false;
		memcpy(d, lb.rep, n);
		if (qs.damage)
			d[n / 2u] ^= 0x04u;
		lb.rep_at = n;
		lb.done = true;
		memset(r, 0, sizeof qs.rec);
		r[S09_QR_NO] = ++qs.sends;
		r[S09_QR_ARM] = qs.arg0;
		r[S09_QR_LEN] = qs.len;
		r[S09_QR_END] = S09_QE_FLAG | S09_QF_TF_ILAT | S09_QF_FED | S09_QF_BEGAN |
				S09_QF_ENDED | S09_QF_SINGLE;
		r[S09_QR_T_FEED] = 100u;
		r[S09_QR_T_FLAG] = 1100u;
		return true;
	}
	if (!lb.data_session || n != lb.qd_n)
		return false;
	lb.data_sessions++;
	if (lb.data_mode == D_BREAK) {
		lb.data_mode = D_OK;
		memcpy(d, lb.qd, n / 2u);
		lb.done = true;
		return false;
	}
	memcpy(d, lb.qd, n);
	if (lb.data_mode == D_FLIP) {
		lb.data_mode = D_OK;
		d[n - 3u] ^= 0x80u;
	} else {
		lb.data_whole++;
	}
	lb.done = true;
	return true;
}

static const struct s09_bus_ops bus = {
	0, lb_ready, lb_attention, lb_select, lb_send, lb_recv, lb_configure, NULL,
};

static struct s09_master m;

static void fresh(bool able)
{
	memset(&lb, 0, sizeof lb);
	memset(&qs, 0, sizeof qs);
	emmc_reads = 0;
	t_sim_init(&sim, S06_MS(2000000));
	s09_slave_init(&sl, &store);
	s09_slave_qtest_source(&sl, st_qtest, NULL);
	s09_slave_qdata_able(&sl, able);
	s09_master_init(&m, &bus, &sim.clock);
	s09_master_quad_recv(&m, lb_quad, NULL);
	CHECK(s09_master_start(&m));
}

/* A read of n sectors from `first`, its data checked. */
static bool read_ok(uint32_t first, uint32_t n)
{
	static uint8_t buf[S09_MAX_SECTORS * S09_SECTOR];
	struct s09_req r = { n == 1u ? S09_READ_SECTOR : S09_READ_SECTORS, 0, 0, first,
			     n == 1u ? 0u : n };
	uint32_t got = 0, err = 0;
	int32_t rc;

	memset(buf, 0xEE, sizeof buf);
	rc = s09_master_call(&m, &r, NULL, 0, buf, n * S09_SECTOR, &got, &err);
	if (rc != 0 || got != n * S09_SECTOR)
		return false;
	for (uint32_t i = 0; i < n * S09_SECTOR; i++)
		if (buf[i] != sector_byte(first + i / S09_SECTOR, i % S09_SECTOR))
			return false;
	return true;
}

/* ---- the slave's rules for the mode (S-09.073, S-09.081) ---- */

static uint32_t seqno;

/* A request session in the slave's present mode, its value. */
static uint32_t slave_req(uint8_t cmd, uint32_t arg0, uint32_t arg1)
{
	struct s09_req r = { cmd, 0, 0, arg0, arg1 };
	uint8_t d[S09_HDR + S09_CHECK_LEN];
	const uint8_t *rep;
	size_t n = S09_HDR, rn;

	s09_req_pack(&r, d);
	if (s09_slave_mode(&sl) != S09_MODE_PLAIN) {
		(void)s09_trailer(d, S09_HDR, s09_unnumbered(cmd) ? 0u : ++seqno, NULL, NULL);
		n += S09_CHECK_LEN;
	}
	s09_slave_request(&sl, d, n);
	if (!s09_slave_reply(&sl, &rep, &rn) || rn < 8u)
		return 0xFFFFFFFFu;
	return wb_get_le32(rep + 4);
}

static void test_slave_rules(void)
{
	t_case = "slave rules";
	CHECK(s09_check_ready());
	s09_slave_init(&sl, &store);
	s09_slave_qdata_able(&sl, true);
	CHECK_EQ(slave_req(S09_ECHO, 1u, 0u), ~1u);
	/* plain: mode 3 is no mode to ask for */
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_QDATA, S09_LINK_KEEP), S09_MODE_PLAIN);
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_QDATA, S09_LINK_TRY), S09_MODE_PLAIN);
	/* on trial in mode 1: neither */
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_CHECKED, S09_LINK_TRY), S09_MODE_CHECKED);
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_QDATA, S09_LINK_KEEP), S09_MODE_CHECKED);
	CHECK(s09_slave_on_trial(&sl));
	/* mode 1 kept: keep mode 3, kept; no trial of it */
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_CHECKED, S09_LINK_KEEP), S09_MODE_CHECKED);
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_QDATA, S09_LINK_TRY), S09_MODE_CHECKED);
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_QDATA, S09_LINK_KEEP), S09_MODE_QDATA);
	CHECK(!s09_slave_on_trial(&sl));
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_QDATA, S09_LINK_KEEP), S09_MODE_QDATA);
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_QDATA, S09_LINK_TRY), S09_MODE_QDATA);
	/* back to mode 1 kept, then plain from mode 3 */
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_CHECKED, S09_LINK_KEEP), S09_MODE_CHECKED);
	CHECK(!s09_slave_on_trial(&sl));
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_QDATA, S09_LINK_KEEP), S09_MODE_QDATA);
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_PLAIN, 0u), S09_MODE_PLAIN);
	/* a request that arrives short in mode 3 falls back to the plain mode */
	(void)slave_req(S09_LINK_MODE, S09_MODE_CHECKED, S09_LINK_TRY);
	(void)slave_req(S09_LINK_MODE, S09_MODE_CHECKED, S09_LINK_KEEP);
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_QDATA, S09_LINK_KEEP), S09_MODE_QDATA);
	{
		uint8_t h[S09_HDR];
		const struct s09_req e = { S09_ECHO, 0, 0, S09_ECHO_RECOVER, 0 };

		s09_req_pack(&e, h);
		s09_slave_request(&sl, h, S09_HDR);
		CHECK_EQ(s09_slave_mode(&sl), S09_MODE_PLAIN);
		CHECK(s09_slave_attention(&sl));
		/* recovering: a plain 0x19 for mode 3 is dropped */
		s09_slave_request(&sl, h, S09_HDR);
		{
			const uint8_t *rp;
			size_t rn;

			CHECK(s09_slave_reply(&sl, &rp, &rn));
		}
		CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_QDATA, 0u), 0xFFFFFFFFu);
	}
	/* a slave whose board sends no quad data stays in mode 1 */
	s09_slave_init(&sl, &store);
	CHECK_EQ(slave_req(S09_ECHO, 1u, 0u), ~1u);
	(void)slave_req(S09_LINK_MODE, S09_MODE_CHECKED, S09_LINK_TRY);
	(void)slave_req(S09_LINK_MODE, S09_MODE_CHECKED, S09_LINK_KEEP);
	CHECK_EQ(slave_req(S09_LINK_MODE, S09_MODE_QDATA, S09_LINK_KEEP), S09_MODE_CHECKED);
}

/* ---- the master ---- */

/* Taken up after the test: 0x12's data in quad at the data clock, the rest
 * single; the link's lowering ends it. */
static void test_take_up(void)
{
	const struct s09_qdata_rec *q = s09_master_qdata(&m);

	t_case = "take up";
	fresh(true);
	CHECK(s09_master_link_qdata(&m, S09_RUNG_CHECKED_FAST));
	CHECK_EQ(q->state, S09_QD_ON);
	CHECK_EQ(q->why, S09_QW_NONE);
	CHECK_EQ(q->test_sent, 8u);
	CHECK_EQ(q->test_whole, 8u);
	CHECK_EQ(lb.test_quad, 8u);
	CHECK_EQ(s09_master_rung(&m), S09_RUNG_CHECKED_FAST);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_QDATA);
	CHECK(read_ok(100u, S09_MAX_SECTORS));
	CHECK(read_ok(7u, 2u));
	CHECK(read_ok(9u, 1u));		/* 0x11: single I/O */
	CHECK_EQ(lb.data_sessions, 2u);
	CHECK_EQ(q->replies, 2u);
	CHECK_EQ(q->bytes, (S09_MAX_SECTORS + 2u) * S09_SECTOR);
	CHECK_EQ(lb.clocks[S09_QD_CLOCK], 8u + 2u);
	CHECK_EQ(s09_master_fell(&m), 0u);
	CHECK_EQ(emmc_reads, 3u);
	CHECK(s09_master_link_plain(&m));
	CHECK_EQ(q->state, S09_QD_ENDED);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_PLAIN);
	CHECK(read_ok(100u, 4u));
	CHECK_EQ(lb.data_sessions, 2u);
}

/* A test reply damaged: not taken up; the link on its rung in mode 1. */
static void test_test_fails(void)
{
	const struct s09_qdata_rec *q = s09_master_qdata(&m);

	t_case = "test fails";
	fresh(true);
	qs.damage = true;
	CHECK(s09_master_link_qdata(&m, S09_RUNG_CHECKED_FAST));
	CHECK_EQ(q->state, S09_QD_NOT);
	CHECK_EQ(q->why, S09_QW_TEST);
	CHECK_EQ(q->test_first, S09_QX_WORD);
	CHECK_EQ(q->test_len, 2u * S09_SECTOR);
	CHECK(lb.cmds[S09_LINK_MODE] != 0u);
	CHECK_EQ(s09_master_rung(&m), S09_RUNG_CHECKED_FAST);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_CHECKED);
	CHECK(read_ok(100u, S09_MAX_SECTORS));
	CHECK_EQ(lb.data_sessions, 0u);
}

/* A slave without quad data answers mode 1; one with no test refuses it. */
static void test_refused(void)
{
	const struct s09_qdata_rec *q = s09_master_qdata(&m);

	t_case = "refused";
	fresh(false);
	CHECK(s09_master_link_qdata(&m, S09_RUNG_CHECKED_FAST));
	CHECK_EQ(q->state, S09_QD_NOT);
	CHECK_EQ(q->why, S09_QW_REFUSED);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_CHECKED);
	CHECK_EQ(s09_master_rung(&m), S09_RUNG_CHECKED_FAST);
	CHECK(read_ok(3u, S09_MAX_SECTORS));
	CHECK_EQ(lb.data_sessions, 0u);
	CHECK_EQ(s09_master_fell(&m), 0u);

	fresh(true);
	s09_slave_qtest_source(&sl, NULL, NULL);
	CHECK(s09_master_link_qdata(&m, S09_RUNG_CHECKED_FAST));
	CHECK_EQ(q->state, S09_QD_NOT);
	CHECK_EQ(q->why, S09_QW_TEST);
	CHECK_EQ(q->test_first, 0x80u | S09_QT_NO_SLAVE);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_CHECKED);
	CHECK(read_ok(3u, 2u));
}

/* The keep's reply lost: both ends back in the plain mode, the rung
 * entered again, mode 1; the read in single I/O. */
static void test_keep_lost(void)
{
	const struct s09_qdata_rec *q = s09_master_qdata(&m);

	t_case = "keep lost";
	fresh(true);
	lb.break_keep = true;
	CHECK(s09_master_link_qdata(&m, S09_RUNG_CHECKED_FAST));
	CHECK_EQ(q->state, S09_QD_NOT);
	CHECK_EQ(q->why, S09_QW_LOST);
	CHECK_EQ(s09_master_rung(&m), S09_RUNG_CHECKED_FAST);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_CHECKED);
	CHECK(s09_master_settled(&m));
	CHECK(read_ok(40u, S09_MAX_SECTORS));
	CHECK_EQ(lb.data_sessions, 0u);
	/* struck for the start */
	CHECK(s09_master_link_qdata(&m, S09_RUNG_CHECKED_FAST));
	CHECK_EQ(q->state, S09_QD_NOT);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_CHECKED);
}

/* One data session fails as `mode` does: the fall-back, the read again on
 * single I/O replies with its data right and the eMMC read once, the rung
 * kept, the quad data dropped for the start. */
static void data_fault(unsigned mode, unsigned why)
{
	const struct s09_qdata_rec *q = s09_master_qdata(&m);

	fresh(true);
	CHECK(s09_master_link_qdata(&m, S09_RUNG_CHECKED_FAST));
	CHECK_EQ(q->state, S09_QD_ON);
	lb.data_mode = mode;
	CHECK(read_ok(500u, S09_MAX_SECTORS));
	CHECK_EQ(emmc_reads, 1u);
	CHECK_EQ(q->state, S09_QD_DROPPED);
	CHECK_EQ(q->why, why);
	CHECK_EQ(q->replies, 0u);
	CHECK_EQ(q->failed, 1u);
	CHECK_EQ(s09_master_fell(&m), 1u);
	CHECK_EQ(s09_master_rung(&m), S09_RUNG_CHECKED_FAST);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_CHECKED);
	CHECK(s09_master_record(&m)->falls == 1u &&
	      s09_master_record(&m)->fall[0].from == S09_RUNG_CHECKED_FAST &&
	      s09_master_record(&m)->fall[0].to == S09_RUNG_CHECKED_FAST);
	/* the rest of the start in single I/O, not taken up again */
	CHECK(read_ok(600u, S09_MAX_SECTORS));
	CHECK(s09_master_link_qdata(&m, S09_RUNG_CHECKED_FAST));
	CHECK_EQ(q->state, S09_QD_DROPPED);
	CHECK(read_ok(700u, 3u));
	CHECK_EQ(lb.data_sessions, mode == D_NONE ? 0u : 1u);
}

static void test_data_faults(void)
{
	t_case = "data broke";
	data_fault(D_BREAK, S09_QW_DATA);
	t_case = "data damaged";
	data_fault(D_FLIP, S09_QW_DATA_CHECK);
	t_case = "data not offered";
	data_fault(D_NONE, S09_QW_DATA);
}

/* The head's session broken: the rung's fault; the poll sent again on the
 * rung below, and the slave gives the read's reply whole in single I/O. */
static void test_head_broke(void)
{
	const struct s09_qdata_rec *q = s09_master_qdata(&m);

	t_case = "head broke";
	fresh(true);
	CHECK(s09_master_link_qdata(&m, S09_RUNG_CHECKED_FAST));
	lb.break_head = true;
	CHECK(read_ok(800u, S09_MAX_SECTORS));
	CHECK_EQ(emmc_reads, 1u);
	CHECK_EQ(q->state, S09_QD_DROPPED);
	CHECK_EQ(q->why, S09_QW_FELL);
	CHECK_EQ(s09_master_rung(&m), S09_RUNG_CHECKED);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_CHECKED);
	CHECK_EQ(lb.data_sessions, 0u);
}

/* Dropped on request: mode 1 by 0x19 keep, no fall-back. */
static void test_off(void)
{
	const struct s09_qdata_rec *q = s09_master_qdata(&m);

	t_case = "off";
	fresh(true);
	CHECK(s09_master_link_qdata(&m, S09_RUNG_CHECKED_FAST));
	CHECK(read_ok(10u, 4u));
	CHECK(s09_master_qdata_off(&m, S09_RUNG_CHECKED_FAST));
	CHECK_EQ(q->state, S09_QD_DROPPED);
	CHECK_EQ(q->why, S09_QW_ASKED);
	CHECK_EQ(q->replies, 1u);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_CHECKED);
	CHECK_EQ(s09_master_rung(&m), S09_RUNG_CHECKED_FAST);
	CHECK_EQ(s09_master_fell(&m), 0u);
	CHECK(read_ok(10u, S09_MAX_SECTORS));
	CHECK_EQ(lb.data_sessions, 1u);
}

int main(void)
{
	test_slave_rules();
	test_take_up();
	test_test_fails();
	test_refused();
	test_keep_lost();
	test_data_faults();
	test_head_broke();
	test_off();
	return t_finish("test_s09_qdata");
}
