/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The quad reply test (S-09.080, S-13b.084) end to end at the protocol's
 * level: s09_master_quad_test() on the master, s09_slave serving 0x1C
 * through a loopback of the S-09 bus, and a slave store whose test op
 * stands in for the board's (src/board/opxy_slave.c): it arms, builds the
 * test reply with s09_qt_build() and keeps a record as the driver would
 * fill it. The loopback's quad receive hands the master the test reply,
 * whole or damaged, and the record says how the slave's candidate ended.
 *
 * Covered: the request's arguments and the reply's words (patterns, check
 * word, data lines); each candidate when its flag comes; the flag that never
 * comes (the bound: the reply whole, the flag missing); a slave that ends
 * early, so that the session breaks and S-09.013 rule 4 brings it back, the
 * slave settled and answering in single I/O, the next trial whole; the last
 * word damaged; one data line stuck; a slave lost (the cell says so); a
 * refused arm and a slave without the test (nothing more sent); the link
 * lowered first and the boot's rung state as it was; and that a master
 * start and plain requests send no 0x1C and never reach the quad receive.
 */
#include <string.h>

#include "waffle/bytes.h"
#include "waffle/s09_master.h"
#include "waffle/s09_slave.h"

#include "s06_test.h"
#include "s06_fake.h"

/* ---- the slave: no storage, and the test op ---- */

static uint32_t st_erase_group(void *c) { (void)c; return 1024u; }
static uint32_t st_boot_size(void *c) { (void)c; return 0x40000u; }
static uint32_t st_os(void *c) { (void)c; return 1u; }
static uint64_t st_cycles(void *c) { (void)c; return 0u; }
static void st_adjust(void *c, int64_t d) { (void)c; (void)d; }
static unsigned storage;		/* storage commands that reached the store */

static int32_t st_emmc(void *c, uint8_t cmd, uint32_t a0, uint32_t a1,
		       uint8_t *data, uint32_t n)
{
	(void)c; (void)cmd; (void)a0; (void)a1; (void)data; (void)n;
	storage++;
	return -1;
}

static int32_t st_file(void *c, const struct s09_req *r, const char *p1,
		       const char *p2, const uint8_t *d, uint32_t n, uint8_t *o,
		       uint32_t *err)
{
	(void)c; (void)p1; (void)p2; (void)d; (void)n; (void)o;
	storage += r->cmd != S09_STAT;
	*err = S09_ENOENT;
	return -1;
}

static int32_t st_read(void *c, uint32_t fd, uint8_t *buf, uint32_t n, uint32_t *err)
{
	(void)c; (void)fd; (void)buf; (void)n;
	*err = S09_EBADF;
	return -1;
}

/* What the stand-in for the board's send does with the next test reply. */
enum { Q_FLAG, Q_NO_FLAG, Q_EARLY, Q_LAST, Q_LINE, Q_DEAD };

static struct {
	bool armed, due;
	uint32_t arg0, len, sends, arms, records;
	uint32_t rec[S09_QT_REC_WORDS];
	uint8_t small[8], out[S09_QT_REC_LEN];
	_Alignas(4) uint8_t buf[S09_QT_MAX];
	unsigned mode;		/* Q_... for every test reply */
	unsigned mode_once;	/* ... for the next one alone, then Q_FLAG */
	bool once;
	unsigned line;		/* Q_LINE: this data line reads high */
	uint32_t delay;		/* the flag's cycles after the last feed */
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
		qs.arms++;
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
		qs.records++;
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

static struct {
	bool sel, reply_session, done, dead;
	uint8_t req[0x11000];
	size_t req_n;
	const uint8_t *rep;
	size_t rep_n, rep_at;
	unsigned sessions, die_after;	/* sessions before the bus dies, 0 never */
	unsigned cmds[256];		/* request sessions by command */
	unsigned quad, quad_bad;	/* quad receives; ... not of a test reply */
	unsigned clocks[S09_QT_CLOCKS];
	unsigned configures;		/* configure(0) calls: the plain set-up */
	bool in_quad;			/* the slave's SPI2 in quad I/O */
	uint32_t log;			/* every session's bytes, hashed */
} lb;

static void lb_log(const uint8_t *d, size_t n, unsigned tag)
{
	uint32_t h = lb.log ^ tag;

	for (size_t i = 0; i < n; i++)
		h = (h ^ d[i]) * 16777619u;
	lb.log = h;
}

static bool lb_ready(void *c)
{
	(void)c;
	if (lb.dead)
		return false;
	return lb.sel ? !lb.done : true;
}

static bool lb_attention(void *c)
{
	(void)c;
	return !lb.dead && s09_slave_attention(&sl);
}

static void lb_select(void *c, bool on)
{
	(void)c;
	if (on && !lb.sel) {
		lb.sessions++;
		if (lb.die_after != 0u && lb.sessions > lb.die_after)
			lb.dead = true;
		/* S-13b.084: every session after a test reply finds the slave
		 * in single I/O again */
		CHECK(!lb.in_quad);
		lb.reply_session = sl.reply_len != 0u;
		lb.done = false;
		lb.req_n = 0;
		lb.rep_at = 0;
		if (lb.reply_session && !lb.dead)
			(void)s09_slave_reply(&sl, &lb.rep, &lb.rep_n);
	} else if (!on && lb.sel && !lb.dead) {
		if (lb.reply_session) {
			lb_log(lb.rep, lb.rep_at, 2u);
			lb.in_quad = false;
		} else if (lb.req_n >= S09_HDR) {
			const size_t k = (size_t)s09_slave_rx_len(&sl, lb.req);

			lb_log(lb.req, lb.req_n, 1u);
			lb.cmds[lb.req[0]]++;
			s09_slave_request(&sl, lb.req, k < lb.req_n ? k : lb.req_n);
		}
	}
	lb.sel = on;
}

static bool lb_send(void *c, const uint8_t *d, size_t n)
{
	(void)c;
	if (lb.dead || lb.reply_session || lb.req_n + n > sizeof lb.req)
		return false;
	memcpy(lb.req + lb.req_n, d, n);
	lb.req_n += n;
	lb.done = true;
	return true;
}

static bool lb_recv(void *c, uint8_t *d, size_t n)
{
	(void)c;
	if (lb.dead || !lb.reply_session || lb.rep_at + n > lb.rep_n)
		return false;
	/* a test reply taken in single I/O is garbled */
	if (qs.due && lb.rep == qs.buf)
		return false;
	memcpy(d, lb.rep + lb.rep_at, n);
	lb.rep_at += n;
	lb.done = lb.rep_at == lb.rep_n;
	return true;
}

static void lb_configure(void *c, int phase)
{
	(void)c;
	lb.configures += phase == 0;
}

/* The slave's record of the reply it sent, as the board's driver fills it. */
static void qs_record(unsigned end, uint32_t flags)
{
	uint32_t *r = qs.rec;

	memset(r, 0, sizeof qs.rec);
	r[S09_QR_NO] = ++qs.sends;
	r[S09_QR_ARM] = qs.arg0;
	r[S09_QR_LEN] = qs.len;
	r[S09_QR_END] = end | flags | S09_QF_BEGAN | S09_QF_ENDED | S09_QF_SINGLE;
	r[S09_QR_STAT] = 0x00C40801u;
	r[S09_QR_STAT_OR] = 0x00C40801u;
	r[S09_QR_T_START] = 50u;
	r[S09_QR_T_FEED] = 400u;
	r[S09_QR_T_FLAG] = end == S09_QE_FLAG ? 400u + qs.delay : S09_QT_NONE;
	r[S09_QR_T_END] = end == S09_QE_FLAG ? 410u + qs.delay : 12000000u;
	r[S09_QR_STAT_BOUND] = end == S09_QE_BOUND ? 0x00C40001u : 0u;
}

/* S-13b.084 on the master: the test reply's session in quad I/O. */
static bool lb_quad(void *c, uint8_t *d, size_t n, unsigned clock, uint32_t out[4])
{
	const unsigned mode = qs.once ? qs.mode_once : qs.mode;

	(void)c;
	out[0] = out[1] = out[2] = out[3] = 0u;
	lb.quad++;
	if (lb.dead || !lb.reply_session || !qs.due || lb.rep != qs.buf || n != lb.rep_n) {
		lb.quad_bad++;
		return false;
	}
	qs.once = false;
	qs.due = false;
	lb.clocks[clock % S09_QT_CLOCKS]++;
	lb.in_quad = true;
	if (mode == Q_DEAD) {
		lb.dead = true;
		return false;
	}
	if (mode == Q_EARLY) {
		/* the slave took its line back before the last word: the
		 * master's receive stops with a word to go */
		memcpy(d, lb.rep, n - 4u);
		lb.rep_at = n - 4u;
		lb.done = true;
		qs_record(S09_QE_FLAG, S09_QF_TF_ILAT | S09_QF_FED);
		out[1] = 1u;
		return false;
	}
	memcpy(d, lb.rep, n);
	lb.rep_at = n;
	lb.done = true;
	if (mode == Q_LAST)
		d[n - 1u] ^= 0x10u;
	if (mode == Q_LINE)
		for (size_t i = 0; i < n; i += 4u)
			wb_put_le32(d + i, wb_get_le32(d + i) | 0x11111111u << qs.line);
	if (mode == Q_NO_FLAG) {
		sim.now += 30000u;	/* the slave's bound */
		qs_record(S09_QE_BOUND, S09_QF_FED);
	} else {
		qs_record(S09_QE_FLAG, S09_QF_TF_STAT | S09_QF_TF_ILAT | S09_QF_FED);
	}
	out[0] = 1u << 10;		/* RF */
	out[2] = 1000u;
	out[3] = 1u;
	return true;
}

static const struct s09_bus_ops bus = {
	0, lb_ready, lb_attention, lb_select, lb_send, lb_recv, lb_configure, NULL,
};
/* the same bus, given no quad receive */
static const struct s09_bus_ops bus_old = {
	0, lb_ready, lb_attention, lb_select, lb_send, lb_recv, lb_configure, NULL,
};

static struct s09_master m;

static void fresh(const struct s09_bus_ops *b)
{
	memset(&lb, 0, sizeof lb);
	memset(&qs, 0, sizeof qs);
	storage = 0;
	t_sim_init(&sim, S06_MS(2000000));
	s09_slave_init(&sl, &store);
	s09_slave_qtest_source(&sl, st_qtest, NULL);
	s09_master_init(&m, b, &sim.clock);
	if (b == &bus)
		s09_master_quad_recv(&m, lb_quad, NULL);
	CHECK(s09_master_start(&m));
}

static unsigned between_calls;

static void between(void *ctx)
{
	(void)ctx;
	between_calls++;
}

static void cell_init(struct s09_qt_cell *c, unsigned cand, bool ren,
		      unsigned clock, uint32_t len, uint16_t n)
{
	memset(c, 0, sizeof *c);
	c->cand = (uint8_t)cand;
	c->ren = ren;
	c->clock = (uint8_t)clock;
	c->len = len;
	c->n = n;
	c->limit_us = S06_MS(120000);
	c->between = between;
}

/* Only the test's requests and the way back's: no storage reached. */
static bool only_test_requests(void)
{
	for (unsigned k = 0; k < 256u; k++)
		if (lb.cmds[k] != 0u && k != S09_QUAD_TEST && k != S09_ECHO &&
		    k != S09_POLL && k != S09_LINK_STATUS && k != S09_LINK_MODE &&
		    k != S09_INIT && k != S09_PULL && k != S09_LINK_TEST)
			return false;
	return storage == 0u;
}

/* ---- the request's arguments and the reply's words ---- */

static void test_words(void)
{
	static uint8_t d[S09_QT_MAX];

	t_case = "words";
	CHECK(s09_kind(S09_QUAD_TEST) == S09_K_IMMEDIATE);
	CHECK(s09_plain_safe(S09_QUAD_TEST));
	CHECK(!s09_unnumbered(S09_QUAD_TEST));
	CHECK(s09_qt_args_ok(S09_QT_ARG0(S09_QT_TF_DMA, false, 0, 0), 12u));
	CHECK(s09_qt_args_ok(S09_QT_ARG0(S09_QT_TF_CORE, true, 5, 0xFFFF), 0x10000u));
	CHECK(!s09_qt_args_ok(S09_QT_ARG0(S09_QT_SPIF, true, 0, 0), 12u));	/* REN: TF only */
	CHECK(s09_qt_args_ok(S09_QT_ARG0(S09_QT_SPIF, false, 0, 0), 12u));
	CHECK(!s09_qt_args_ok(S09_QT_ARG0(0, false, 0, 0), 12u));
	CHECK(!s09_qt_args_ok(S09_QT_ARG0(S09_QT_TF_DMA, false, S09_QT_PATS, 0), 12u));
	CHECK(!s09_qt_args_ok(S09_QT_ARG0(S09_QT_TF_DMA, false, 0, 0), 8u));
	CHECK(!s09_qt_args_ok(S09_QT_ARG0(S09_QT_TF_DMA, false, 0, 0), 14u));
	CHECK(!s09_qt_args_ok(S09_QT_ARG0(S09_QT_TF_DMA, false, 0, 0), 0x10004u));
	CHECK(!s09_qt_args_ok(S09_QT_SEND, 12u));
	CHECK(s09_check_ready());
	/* the header word, the pattern, then the check word of the rest */
	s09_qt_build(d, 64u, 2u, S09_QT_ARG0(S09_QT_TF_DMA, false, 0, 3));
	CHECK_EQ(wb_get_le32(d), 0x021Cu);
	CHECK_EQ(wb_get_le32(d + 4), 1u << 3);
	CHECK_EQ(wb_get_le32(d + 8), 1u << 4);
	CHECK_EQ(wb_get_le32(d + 60), s09_check_word(d, 60u));
	/* each pattern: a walking one and zero over all 32 bits, one line at a
	 * time high and low, the lines alternating, and random words */
	{
		uint32_t or_walk = 0, and_walk0 = ~0u, lines = 0;

		for (uint32_t j = 0; j < 32u; j++) {
			or_walk |= s09_qt_word(0u, 0u, j);
			and_walk0 &= s09_qt_word(1u, 0u, j);
			CHECK_EQ(__builtin_popcount(s09_qt_word(0u, 0u, j)), 1);
			CHECK_EQ(__builtin_popcount(s09_qt_word(1u, 0u, j)), 31);
		}
		CHECK_EQ(or_walk, 0xFFFFFFFFu);
		CHECK_EQ(and_walk0, 0u);
		for (uint32_t j = 0; j < 4u; j++) {
			lines |= 1u << s09_qt_lines(s09_qt_word(2u, 0u, j));
			CHECK_EQ(s09_qt_lines(~s09_qt_word(3u, 0u, j)), s09_qt_lines(s09_qt_word(2u, 0u, j)));
		}
		CHECK_EQ(lines, 0x116u);	/* lines 0 to 3, each alone */
		CHECK(s09_qt_word(4u, 0u, 0u) == 0x55555555u && s09_qt_word(4u, 0u, 1u) == 0xAAAAAAAAu);
		CHECK(s09_qt_word(5u, 1u, 0u) != s09_qt_word(5u, 2u, 0u));
	}
	CHECK_EQ(s09_qt_lines(0x00000001u), 1u);
	CHECK_EQ(s09_qt_lines(0x80000000u), 8u);
	CHECK_EQ(s09_qt_lines(0x00000022u), 2u);
	CHECK_EQ(S09_QT_REC_LEN, 72u);
}

/* ---- the cells ---- */

/* Each candidate, its flag coming: every reply whole, the flag on each,
 * its delay, each clock; nothing else reached the slave. */
static void test_flag(void)
{
	static const unsigned cands[] = { S09_QT_TF_DMA, S09_QT_TF_CORE, S09_QT_SPIF };
	struct s09_qt_cell c;

	t_case = "flag";
	for (unsigned k = 0; k < 3u; k++) {
		for (unsigned clock = 0; clock < S09_QT_CLOCKS; clock++) {
			fresh(&bus);
			qs.delay = 40u + clock;
			between_calls = 0;
			memset(lb.cmds, 0, sizeof lb.cmds);
			cell_init(&c, cands[k], cands[k] != S09_QT_SPIF && clock == 1u, clock,
				  clock == 3u ? 0x10000u : 12u + 64u * clock, 12);
			CHECK(s09_master_quad_test(&m, &c));
			CHECK(!c.refused && !c.cut && c.settled);
			CHECK_EQ(c.sent, 12);
			CHECK_EQ(c.whole, 12);
			CHECK_EQ(c.flag, 12);
			CHECK(c.missing == 0u && c.last_wrong == 0u && c.word_wrong == 0u &&
			      c.broke == 0u && c.no_record == 0u);
			CHECK_EQ(c.ends[S09_QE_FLAG], 12);
			CHECK(c.dmin == 40u + clock && c.dmax == 40u + clock);
			CHECK(c.lines == 0u && c.first == S09_QX_OK);
			CHECK(c.m_stat_and == (1u << 10) && c.m_stat_or == (1u << 10));
			CHECK_EQ(lb.quad, 12);
			CHECK_EQ(lb.clocks[clock], 12);
			CHECK_EQ(between_calls, 12);
			/* ARM, SEND and RECORD each, and the closing echo */
			CHECK_EQ(lb.cmds[S09_QUAD_TEST], 36);
			CHECK_EQ(qs.arms, 12);
			CHECK(only_test_requests());
			CHECK(s09_master_rung(&m) == S09_RUNG_PLAIN && s09_master_settled(&m));
			CHECK(S09_QT_CAND(qs.arg0) == cands[k]);
			CHECK(((qs.arg0 & S09_QT_REN) != 0u) == c.ren);
			/* the trials' patterns and seeds: the last one's */
			CHECK_EQ(S09_QT_PAT(qs.arg0), 11u % S09_QT_PATS);
			CHECK_EQ(S09_QT_SEED(qs.arg0), 11u);
		}
	}
}

/* No flag within the slave's bound: each reply whole, the flag missing,
 * the record's end the bound; the slave in single I/O after, settled. */
static void test_no_flag(void)
{
	struct s09_qt_cell c;
	uint32_t seen;

	t_case = "no flag";
	fresh(&bus);
	qs.mode = Q_NO_FLAG;
	cell_init(&c, S09_QT_TF_DMA, false, 0, 0x1000u, 5);
	CHECK(s09_master_quad_test(&m, &c));
	CHECK(c.settled && c.sent == 5u && c.whole == 5u && c.flag == 0u && c.missing == 5u);
	CHECK_EQ(c.ends[S09_QE_BOUND], 5);
	CHECK(c.dmin == S09_QT_NONE && c.dmax == S09_QT_NONE);
	CHECK_EQ(c.first, S09_QX_NO_FLAG);
	CHECK_EQ(c.first_no, 0u);
	CHECK((c.first_rec[S09_QR_END] & 0xFFu) == S09_QE_BOUND &&
	      (c.first_rec[S09_QR_END] & S09_QF_SINGLE) != 0u &&
	      c.first_rec[S09_QR_STAT_BOUND] == 0x00C40001u &&
	      c.first_rec[S09_QR_LEN] == 0x1000u);
	CHECK(only_test_requests());
	/* a plain request after it: the slave answers in single I/O */
	seen = lb.quad;
	{
		struct s09_req r = { S09_ERASE_GROUP, 0, 0, 0, 0 };
		uint32_t err = 0;

		CHECK_EQ(s09_master_call(&m, &r, NULL, 0, NULL, 0, NULL, &err), 1024);
	}
	CHECK_EQ(lb.quad, seen);
}

/* The slave ends early: the session breaks with the last word to go, S-09.013
 * rule 4 brings the slave back (echoes, polls, 0x1B), the record still comes,
 * and the next trials are whole. */
static void test_early(void)
{
	struct s09_qt_cell c;

	t_case = "early";
	fresh(&bus);
	qs.once = true;
	qs.mode_once = Q_EARLY;
	cell_init(&c, S09_QT_TF_CORE, false, 2, 256u, 4);
	CHECK(s09_master_quad_test(&m, &c));
	CHECK(c.settled && !m.absent);
	CHECK(c.sent == 4u && c.broke == 1u && c.whole == 3u && c.flag == 4u);
	CHECK(c.first == S09_QX_BROKE && c.first_no == 0u && c.first_m[1] == 1u);
	CHECK((c.first_rec[S09_QR_END] & 0xFFu) == S09_QE_FLAG);
	CHECK(lb.cmds[S09_ECHO] >= 2u && lb.cmds[S09_POLL] >= 1u && lb.cmds[S09_LINK_STATUS] >= 1u);
	CHECK(only_test_requests());
	CHECK(s09_master_settled(&m));
}

/* The last word damaged on every reply, or one data line stuck high. */
static void test_damage(void)
{
	struct s09_qt_cell c;

	t_case = "last word";
	fresh(&bus);
	qs.mode = Q_LAST;
	cell_init(&c, S09_QT_TF_DMA, false, 1, 64u, 6);
	CHECK(s09_master_quad_test(&m, &c));
	CHECK(c.sent == 6u && c.whole == 0u && c.last_wrong == 6u && c.word_wrong == 0u);
	CHECK(c.first == S09_QX_LAST && c.first_word == 15u && c.lines == 1u);
	CHECK(c.flag == 6u && c.settled);

	t_case = "a line stuck";
	for (unsigned line = 0; line < 4u; line++) {
		fresh(&bus);
		qs.mode = Q_LINE;
		qs.line = line;
		cell_init(&c, S09_QT_SPIF, false, 3, 0x4000u, S09_QT_PATS);
		CHECK(s09_master_quad_test(&m, &c));
		CHECK(c.sent == S09_QT_PATS && c.whole == 0u && c.word_wrong == S09_QT_PATS);
		CHECK_EQ(c.lines, 1u << line);
		CHECK(c.first == S09_QX_WORD && c.first_word != S09_QT_NONE);
	}
}

/* A slave lost in a reply: the cell says so, and nothing is sent after. */
static void test_lost(void)
{
	struct s09_qt_cell c;
	unsigned sessions;

	t_case = "lost";
	fresh(&bus);
	qs.mode = Q_DEAD;
	cell_init(&c, S09_QT_TF_DMA, false, 0, 12u, 8);
	CHECK(!s09_master_quad_test(&m, &c));
	CHECK(m.absent && !c.settled && c.sent == 1u && c.broke == 0u);
	sessions = lb.sessions;
	CHECK(!s09_master_quad_test(&m, &c));
	CHECK_EQ(lb.sessions, sessions);
}

/* Refused before anything but the arm: arguments out of range, a bus
 * without the quad receive, a slave without the test. */
static void test_refused(void)
{
	struct s09_qt_cell c;
	unsigned sessions;

	t_case = "refused";
	fresh(&bus);
	sessions = lb.sessions;
	cell_init(&c, S09_QT_SPIF, true, 0, 12u, 4);	/* REN: TF only */
	CHECK(s09_master_quad_test(&m, &c) && c.refused && c.why == S09_QT_BAD);
	cell_init(&c, S09_QT_TF_DMA, false, 4, 12u, 4);	/* no such clock */
	CHECK(s09_master_quad_test(&m, &c) && c.refused);
	cell_init(&c, S09_QT_TF_DMA, false, 0, 10u, 4);
	CHECK(s09_master_quad_test(&m, &c) && c.refused);
	cell_init(&c, S09_QT_TF_DMA, false, 0, 12u, 0);
	CHECK(s09_master_quad_test(&m, &c) && c.refused);
	CHECK_EQ(lb.sessions, sessions);

	fresh(&bus_old);
	sessions = lb.sessions;
	cell_init(&c, S09_QT_TF_DMA, false, 0, 12u, 4);
	CHECK(s09_master_quad_test(&m, &c) && c.refused && c.sent == 0u);
	CHECK_EQ(lb.sessions, sessions);

	fresh(&bus);
	s09_slave_qtest_source(&sl, NULL, NULL);
	cell_init(&c, S09_QT_TF_DMA, false, 0, 12u, 4);
	CHECK(s09_master_quad_test(&m, &c));
	CHECK(c.refused && c.why == S09_QT_NO_SLAVE && c.sent == 0u && lb.quad == 0u);
	CHECK_EQ(lb.cmds[S09_QUAD_TEST], 1);
}

/* On a rung: lowered first (S-09.078), the cell plain, the boot's rung
 * state as it was, the slave in the plain mode. */
static void test_rung(void)
{
	struct s09_qt_cell c;
	uint8_t ceiling;

	t_case = "rung";
	fresh(&bus);
	CHECK(s09_master_link_fast(&m, S09_RUNG_TOP));
	CHECK(s09_master_rung(&m) != S09_RUNG_PLAIN);
	ceiling = m.ceiling;
	cell_init(&c, S09_QT_TF_DMA, false, 0, 12u, 2);
	CHECK(s09_master_quad_test(&m, &c));
	CHECK(c.whole == 2u && c.flag == 2u && c.settled);
	CHECK(s09_master_rung(&m) == S09_RUNG_PLAIN && s09_slave_mode(&sl) == S09_MODE_PLAIN);
	CHECK_EQ(m.ceiling, ceiling);
	CHECK_EQ(s09_master_fell(&m), 0u);
}

/* A slave outside the plain mode answers 0x1C with S09_QT_NOT_PLAIN and
 * sends nothing in quad I/O. */
static void test_slave_rules(void)
{
	const uint8_t *d;
	size_t n;
	uint8_t h[S09_HDR];
	struct s09_req r = { S09_QUAD_TEST, 0, 0, S09_QT_SEND, 0 };

	t_case = "slave rules";
	memset(&qs, 0, sizeof qs);
	s09_slave_init(&sl, &store);
	s09_slave_qtest_source(&sl, st_qtest, NULL);
	sl.linked = true;
	/* SEND unarmed: the 8-byte value, single I/O */
	s09_req_pack(&r, h);
	s09_slave_request(&sl, h, sizeof h);
	CHECK(s09_slave_reply(&sl, &d, &n) && n == 8u && d != qs.buf &&
	      wb_get_le32(d + 4) == S09_QT_NOT_ARMED);
	/* any 0x1C disarms an armed test */
	r.arg0 = S09_QT_ARG0(S09_QT_TF_DMA, false, 0, 0);
	r.arg1 = 16u;
	s09_req_pack(&r, h);
	s09_slave_request(&sl, h, sizeof h);
	CHECK(s09_slave_reply(&sl, &d, &n) && wb_get_le32(d + 4) == S09_QT_ARMED && qs.armed);
	r.arg0 = S09_QT_RECORD;
	r.arg1 = 0u;
	s09_req_pack(&r, h);
	s09_slave_request(&sl, h, sizeof h);
	CHECK(s09_slave_reply(&sl, &d, &n) && n == S09_QT_REC_LEN && !qs.armed);
	/* outside the plain mode: a numbered 0x1C answers that the link is
	 * not plain, with its check word */
	sl.mode = S09_MODE_CHECKED;
	{
		uint8_t q[S09_HDR + S09_CHECK_LEN];

		r.arg0 = S09_QT_ARG0(S09_QT_TF_DMA, false, 0, 0);
		r.arg1 = 16u;
		s09_req_pack(&r, q);
		(void)s09_trailer(q, S09_HDR, 7u, NULL, NULL);
		s09_slave_request(&sl, q, sizeof q);
		CHECK(s09_slave_reply(&sl, &d, &n) && n == 12u &&
		      wb_get_le32(d + 4) == S09_QT_NOT_PLAIN && !qs.armed);
	}
	sl.mode = S09_MODE_PLAIN;
}

/* An ordinary start and plain requests: no 0x1C, no quad receive, and the
 * sessions' bytes as without the test: the same with a bus that has no
 * quad receive, and the same as this loopback hashed them before the test
 * existed (TRACE_BEFORE, 48 sessions: the start, 0x03, a raise to rung 4
 * with its trial, tests and gate, and the lowering). */
#define TRACE_BEFORE 0xE09CA053u
#define TRACE_SESSIONS 48u

static void test_ordinary(void)
{
	struct s09_req r = { S09_ERASE_GROUP, 0, 0, 0, 0 };
	uint32_t err = 0;

	t_case = "ordinary";
	for (unsigned k = 0; k < 2u; k++) {
		fresh(k == 0u ? &bus : &bus_old);
		CHECK_EQ(s09_master_call(&m, &r, NULL, 0, NULL, 0, NULL, &err), 1024);
		CHECK(s09_master_link_fast(&m, S09_RUNG_TOP));
		CHECK(s09_master_link_plain(&m));
		CHECK_EQ(lb.cmds[S09_QUAD_TEST], 0);
		CHECK_EQ(lb.quad, 0);
		CHECK_EQ(lb.log, TRACE_BEFORE);
		CHECK_EQ(lb.sessions, TRACE_SESSIONS);
	}
}

int main(void)
{
	test_words();
	test_flag();
	test_no_flag();
	test_early();
	test_damage();
	test_lost();
	test_refused();
	test_rung();
	test_slave_rules();
	test_ordinary();
	return t_finish("test_s09_qtest");
}
