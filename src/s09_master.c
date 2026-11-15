/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09, the master's side: the slave boot stream and the bridge client.
 */
#include "waffle/s09_master.h"

#include <string.h>

#include "waffle/s06_bytes.h"
#include "waffle/testos.h"

#define LINE_STEP_US 100u		/* how often a line is looked at */
#define PIECE 1024u			/* S-09.005 */
#define LDR_FILL 0x0100u
#define LDR_FINAL 0x8000u

static uint32_t now(const struct s06_clock *c)
{
	return s06_now(c);
}

/* Wait until line() == want, at most us. */
static bool wait_line(const struct s06_clock *c, bool (*line)(void *),
		      void *ctx, bool want, uint32_t us)
{
	uint32_t end = now(c) + us;

	for (;;) {
		if (line(ctx) == want)
			return true;
		if (s06_time_reached(now(c), end))
			return false;
		s06_idle(c, now(c) + LINE_STEP_US);
	}
}

/* ---- the slave boot stream (S-09.001-S-09.007) ---- */

/*
 * How often a ready line found high is sampled again before the clock is
 * asked: a slave that holds the line for a few microseconds after a byte
 * costs no clock read.
 */
#define LINE_SPIN 256u

/* The driver could not move the last byte: nothing more is sent. */
static bool link_failed(const struct s09_boot_ops *o)
{
	return o->failed != NULL && o->failed(o->ctx);
}

/* S-09.005, S-13b.022: PA6 sampled before a stream byte. The clock is read
 * only for a line that stays high, and then the wait is T_STREAM's
 * (S-09.060), which leaves the slave's boot ROM time to process a block. */
static bool byte_ready(const struct s09_boot_ops *o, const struct s06_clock *c)
{
	unsigned spin;

	/*
	 * SPEC-Q: how soon after a byte's last clock edge the slave's boot ROM
	 * raises PA6 is not measured. This sample comes
	 * the link's settle time after the byte before it was received in
	 * full; that margin stands until the line's behavior is measured, which
	 * the timing build does (waffle/boot_time.h).
	 */
	if (o->ready(o->ctx))
		return true;
	for (spin = 0; spin < LINE_SPIN; spin++)
		if (o->ready(o->ctx))
			return true;
	return wait_line(c, o->ready, o->ctx, true, S09_T_STREAM_US);
}

static bool stream_bytes(const struct s09_boot_ops *o, const struct s06_clock *c,
			 const uint8_t *d, size_t n)
{
	o->select(o->ctx, true);
#if OPXY_BOOT_LINK_FC
	if (o->send != NULL) {
		/*
		 * SPEC-Q: the controller samples PA6 between bytes with no gap
		 * of the master's own. How soon after a byte the slave's boot
		 * ROM raises PA6 is not measured, so this is built only on request; the timing build's
		 * measurement of the line on the default path (byte_ready())
		 * is what settles it.
		 */
		const bool sent = o->send(o->ctx, d, n);

		o->select(o->ctx, false);
		return sent;
	}
#endif
	/* one byte in flight at a time: xfer() returns when its byte is in */
	for (size_t i = 0; i < n; i++) {
		if (!byte_ready(o, c)) {
			o->select(o->ctx, false);
			return false;
		}
		(void)o->xfer(o->ctx, d[i]);
		if (link_failed(o)) {
			o->select(o->ctx, false);
			return false;
		}
	}
	o->select(o->ctx, false);
	return true;
}

int s09_boot_slave(const struct s09_boot_ops *o, const struct s06_clock *c,
		   const uint8_t *slot, size_t len, bool crc_ok, int *end)
{
	/* the walk counts in 32 bits, as on the target */
	uint32_t n = (size_t)(uint32_t)len == len ? (uint32_t)len : UINT32_MAX;
	uint32_t off = 0;
	uint8_t opened;

	*end = S09_END_SLOT;
	if (!crc_ok)
		return S09_BOOT_BAD_CRC;		/* S-09.002: no byte */
	if (!wait_line(c, o->ready, o->ctx, true, S09_T_BOOT_READY_US))
		return S09_BOOT_NOT_READY;		/* S-09.003 */
	o->select(o->ctx, true);			/* S-09.004 */
	opened = o->xfer(o->ctx, 0x03);
	if (link_failed(o)) {
		o->select(o->ctx, false);		/* no answer to judge: absent */
		return S09_BOOT_STUCK;
	}
	if (opened != 0xFF) {
		/* S-09.007, S-16.115: eleven 0F, PA6 high (bounded), release */
		for (int i = 0; i < 11 && !link_failed(o); i++)
			(void)o->xfer(o->ctx, 0x0F);
		if (!link_failed(o))
			(void)wait_line(c, o->ready, o->ctx, false, S09_T_BOOT_READY_US);
		o->select(o->ctx, false);
		return S09_BOOT_NO_OPEN;
	}
	o->select(o->ctx, false);

	for (;;) {					/* S-09.005, S-09.006 */
		const uint8_t *h = slot + off;
		uint32_t code, count, flags;
		uint32_t stop, at, first, k;
		bool header_only;

		if (n - off < 16) {
			*end = S09_END_SLOT;
			return S09_BOOT_STREAMED;
		}
		if (h[3] != 0xAD) {
			*end = S09_END_BADSIG;		/* that header is not sent */
			return S09_BOOT_STREAMED;
		}
		code = s06_rd_le32(h);
		count = s06_rd_le32(h + 8);
		flags = code & 0xFFF0u;
		header_only = (flags & LDR_FILL) || count == 0;
		if (!header_only && count > n - off - 16u) {
			/* S-09.006, proposed: nothing past the slot's end */
			*end = S09_END_SLOT;
			return S09_BOOT_STREAMED;
		}
		stop = off + 16u + (header_only ? 0u : count);
		/* the header and the first piece in one session */
		first = header_only ? 16u : 16u + (count < PIECE ? count : PIECE);
		if (!stream_bytes(o, c, h, first))
			return S09_BOOT_STUCK;
		for (at = off + first; at < stop; at += k) {
			k = stop - at < PIECE ? stop - at : PIECE;
			if (!stream_bytes(o, c, slot + at, k))
				return S09_BOOT_STUCK;
		}
		off = stop;
		if (flags & LDR_FINAL) {
			*end = S09_END_FINAL;
			return S09_BOOT_STREAMED;
		}
	}
}

/* ---- sessions (S-09.010) ---- */

static bool attention(const struct s09_master *m)
{
	return m->bus->attention(m->bus->ctx);
}

/*
 * S-09.013: a session let go before its end; the slave may still be in it,
 * receiving a request or offering a reply to one it took whole, until its
 * own T_REPLY has run from PC11 high. The next session waits that out.
 */
static void let_go(struct s09_master *m)
{
	m->bus->select(m->bus->ctx, false);
	m->cut = true;
}

/* Rule 1: PA6 low (bounded), then PC11 low; after a session let go, PC11
 * high for T_QUIET first (S-09.013). */
static bool session_begin(struct s09_master *m, uint32_t bound)
{
	if (m->cut) {
		s06_idle(m->clock, now(m->clock) + m->t_quiet_us);
		m->cut = false;
	}
	if (!wait_line(m->clock, m->bus->ready, m->bus->ctx, true, bound))
		return false;
	m->bus->select(m->bus->ctx, true);
	return true;
}

/* Rule 3: the session's transfer closed (S-13b.083), PA6 high (bounded by
 * T_REPLY), then PC11 high. */
static bool session_end(struct s09_master *m)
{
	if (m->bus->end != NULL)
		m->bus->end(m->bus->ctx);
	if (!wait_line(m->clock, m->bus->ready, m->bus->ctx, false, S09_T_REPLY_US)) {
		let_go(m);
		return false;
	}
	m->bus->select(m->bus->ctx, false);
	return true;
}

static void absent(struct s09_master *m)
{
	if (!m->absent) {
		/* the first cause is the one kept (s09_master_lost()) */
		m->lost.cmd = m->cur_cmd;
		m->lost.arg0 = m->cur_arg0;
		m->lost.at = (uint8_t)(m->why.at | (m->in_poll ? S09_LOST_IN_POLL : 0u));
		for (unsigned i = 0; i < 4u; i++)
			m->lost.fault[i] = m->why.words[i];
	}
	m->absent = true;
	m->bus->select(m->bus->ctx, false);
}

/* A request the bridge serves from here on, for s09_master_lost(): nothing
 * has broken in it yet. */
static void serving(struct s09_master *m, uint8_t cmd, uint32_t arg0)
{
	m->cur_cmd = cmd;
	m->cur_arg0 = arg0;
	m->in_poll = false;
	m->io_failed = false;
	s06_fill((uint8_t *)&m->why, 0, sizeof m->why);
}

/* Where an exchange broke, in *f; a send's or a receive's failure with the
 * driver's fault words. */
static void broke_in(const struct s09_master *m, struct s09_fault *f, unsigned at)
{
	s06_fill((uint8_t *)f, 0, sizeof *f);
	f->at = (uint8_t)at;
	if (at != S09_AT_SEND && at != S09_AT_REPLY_BROKE)
		return;
	if (m->fault_words != NULL)
		m->fault_words(m->fault_ctx, f->words);
	if (m->fault_more != NULL)
		m->fault_more(m->more_ctx, f->more);
}

/* Where the exchange under way broke, for the link's record. */
static void broke(struct s09_master *m, unsigned at)
{
	broke_in(m, &m->why, at);
}

static bool rx(struct s09_master *m, uint8_t *d, uint32_t n)
{
	if (m->bus->recv(m->bus->ctx, d, n))
		return true;
	broke(m, S09_AT_REPLY_BROKE);
	return false;
}

/* What follows a request session at once (S-09.030-S-09.034). */
enum { X_NONE, X_IMM, X_PULL, X_POLL, X_STATUS };

/*
 * One exchange: a request session and the reply session that follows it
 * at once, if its kind has one. Outside the plain mode it carries its
 * number (S-09.076), and sent again it is the same bytes.
 */
struct xreq {
	struct s09_req r;
	const uint8_t *payload;
	uint32_t plen;
	uint32_t seq;		/* S-09.076: 0, none */
	uint8_t reply;		/* X_... */
	uint8_t rung;		/* the rung it was last sent on */
	bool engine;		/* its check came from the bus's engine */
	uint8_t tries;		/* times it was sent again */
	uint8_t *at;		/* where its session is built; NULL: buf or small */
	uint32_t bound;		/* the reply session's start */
	bool started;		/* its last request session began (PA6 low) */
	bool link;		/* the link's own 0x19 or 0x1A (link_req()) */
};

static void xreq_init(struct xreq *x, const struct s09_req *r,
		      const uint8_t *payload, uint32_t plen, uint8_t reply)
{
	s06_fill((uint8_t *)x, 0, sizeof *x);
	x->r = *r;
	x->payload = payload;
	x->plen = plen;
	x->reply = reply;
	x->bound = r->cmd == S09_RELEASE ? S09_T_RELEASE_US : S09_T_REPLY_US;
}

/* S-09.076: every new request outside the plain mode gets the next number,
 * 1, 2, 3, ... for the whole boot; 0x0A, 0x19 and 0x1A none. */
static void number(struct s09_master *m, struct xreq *x)
{
	x->seq = S09_SEQ_NONE;
	if (m->rung != S09_RUNG_PLAIN && !s09_unnumbered(x->r.cmd)) {
		if (++m->seq == S09_SEQ_NONE)
			m->seq = 1;
		x->seq = m->seq;
	}
}

static uint8_t *session_buf(struct s09_master *m, const struct xreq *x)
{
	return x->at != NULL ? x->at : x->plen != 0u ? m->buf : m->small;
}

/*
 * A request session's bytes: header, payload, zero pad (S-09.021), and on
 * a rung the trailer (S-09.072, S-09.076). One with a payload is built in
 * m->buf, which keeps it until the next one, so that it can be sent again;
 * one without in m->small.
 */
static const uint8_t *request_bytes(struct s09_master *m, struct xreq *x,
				    uint32_t *len)
{
	const bool plain = m->rung == S09_RUNG_PLAIN;
	/* S-09.072: on a rung the payload is not lengthened */
	const uint32_t wire = plain ? s09_master_payload_wire(x->plen) :
				      s09_pad4(x->plen);
	uint32_t n = S09_HDR + wire;
	uint8_t *d = session_buf(m, x);

	s09_req_pack(&x->r, d);
	if (x->plen && x->payload != d + S09_HDR)
		memcpy(d + S09_HDR, x->payload, x->plen);
	s06_fill(d + S09_HDR + x->plen, 0, wire - x->plen);
	x->rung = m->rung;
	x->engine = false;
	if (!plain) {
		x->engine = m->check != NULL && !m->soft_check;
		(void)s09_trailer(d, n, x->seq, x->engine ? m->check : NULL,
				  m->check_ctx);
		n += S09_CHECK_LEN;
	}
	*len = n;
	return d;
}

/* S-09.075: the bus told that a request session on the rung follows, and
 * that it is over. */
static void rung_on(const struct s09_master *m, bool on)
{
	if (m->rung != S09_RUNG_PLAIN)
		m->bus->configure(m->bus->ctx, on ? S09_PHASE_RUNG(m->rung) : 0);
}

/* A request session. */
static bool request(struct s09_master *m, struct xreq *x)
{
	uint32_t n, t0 = 0;
	const uint8_t *d = request_bytes(m, x, &n);
	bool ok;

	rung_on(m, true);
	/* S-09.060: T_REPLY bounds PA6 at the start of a request session */
	ok = session_begin(m, S09_T_REPLY_US);
	x->started = ok;
	if (ok)
		t0 = now(m->clock);	/* the session's own time, from its select */
	if (!ok) {
		broke(m, S09_AT_READY);
	} else if (!m->bus->send(m->bus->ctx, d, n)) {
		broke(m, S09_AT_SEND);
		let_go(m);
		ok = false;
	} else if (!session_end(m)) {
		broke(m, S09_AT_READY);
		ok = false;
	}
	m->session_us = now(m->clock) - t0;
	rung_on(m, false);
	return ok;
}

/* ---- slots (S-09.022) ---- */

static int free_slots(const struct s09_master *m)
{
	int n = 0;

	for (unsigned i = 0; i < S09_SLOTS; i++)
		n += m->slot[i].state == S09_SLOT_FREE;
	return n;
}

static int lowest_free(const struct s09_master *m)
{
	for (unsigned i = 0; i < S09_SLOTS; i++)
		if (m->slot[i].state == S09_SLOT_FREE)
			return (int)i;
	return -1;
}

static bool poll_once(struct s09_master *m, bool *got);
static bool poll_take(struct s09_master *m);

/*
 * The lowest free slot. A deferred request or a pull never takes the last
 * free one (proposed, S-09.022): it polls until a poll has freed another.
 */
static int take_slot(struct s09_master *m, int kind)
{
	bool last_ok = kind != S09_K_DEFERRED && kind != S09_K_PULL;
	uint32_t deadline = now(m->clock) + S09_T_EXEC_US;

	while (free_slots(m) < (last_ok ? 1 : 2)) {
		const int had = free_slots(m);
		bool got;

		if (attention(m)) {
			if (!poll_once(m, &got))
				return -1;
			/* the wait runs from the last slot freed: polls that
			 * free none do not lengthen it */
			if (free_slots(m) > had)
				deadline = now(m->clock) + S09_T_EXEC_US;
		} else {
			s06_idle(m->clock, now(m->clock) + LINE_STEP_US);
		}
		if (s06_time_reached(now(m->clock), deadline))
			return -1;
	}
	return lowest_free(m);
}

/* ---- replies (S-09.030-S-09.035), taken whole before they are used ---- */

/* A pull reply's count word and bytes, at `at`; *got: the reply's length
 * with its header word. */
static bool pull_body(struct s09_master *m, uint8_t *at, uint32_t *got)
{
	uint32_t n;

	if (!rx(m, at, 4))
		return false;
	n = s06_rd_le32(at) & 0xFFFFu;
	if (n > S09_TUNNEL_MAX)
		return false;
	*got = 8u + s09_pad4(n);
	return n == 0 || rx(m, at + 4, s09_pad4(n));
}

/* The body of a deferred reply for a slot, after its header word. */
static bool deferred_body(struct s09_master *m, const struct s09_slot_state *st,
			  uint32_t *got)
{
	long dlen;

	if (st->cmd == S09_READ) {			/* S-09.035 */
		int32_t r;

		if (!rx(m, m->rep + 4, 4))
			return false;
		r = (int32_t)s06_rd_le32(m->rep + 4);
		*got = 8;
		if (r > 0) {
			uint32_t want = s09_read_ask(st->arg1, st->data_len,
						     S09_READ_CHUNK);

			*got += s09_pad4((uint32_t)r);
			return (uint32_t)r <= want &&
			       st->data_len + (uint32_t)r <= st->data_max &&
			       rx(m, m->rep + 8, s09_pad4((uint32_t)r));
		}
		*got += r < 0 ? 4u : 0u;
		return r == 0 || rx(m, m->rep + 8, 4);
	}
	dlen = s09_deferred_data_len(st->cmd, st->arg1);
	*got = 12u + (uint32_t)dlen;
	return dlen >= 0 && rx(m, m->rep + 4, 8u + (uint32_t)dlen);
}

/*
 * A poll's reply. Its first word is 06 FF 00 00 or the header word of a
 * request that is outstanding (S-09.031 rule 5): neither is all zeros or
 * all ones, so a line that carries nothing is never taken for a reply.
 */
static bool poll_body(struct s09_master *m, uint32_t *got)
{
	const struct s09_slot_state *st;
	uint32_t w;
	uint8_t rs;

	if (!rx(m, m->rep, 4))
		return false;
	w = s06_rd_le32(m->rep);
	m->poll_word = w;		/* whose reply this session hands out */
	m->poll_seen = true;
	*got = 4;
	if (w == S09_POLL_NONE)
		return true;
	rs = (uint8_t)(w >> 8);
	if ((w >> 16) || rs >= S09_SLOTS || m->slot[rs].cmd != (uint8_t)w)
		return false;
	st = &m->slot[rs];
	if (st->state == S09_SLOT_PULL)
		return pull_body(m, m->rep + 4, got);
	if (st->state == S09_SLOT_WAIT || st->state == S09_SLOT_READ)
		return deferred_body(m, st, got);
	return false;
}

/* The reply session that follows x at once, whole in m->rep. Its first
 * word is checked; nothing of it is used before the session is complete
 * (S-09.013). */
static bool reply(struct s09_master *m, const struct xreq *x)
{
	uint32_t got = 0;
	bool ok;

	uint8_t *at = x->reply == X_IMM ? m->imm :
		      x->reply == X_STATUS ? m->stat : m->rep;

	if (x->reply == X_NONE)
		return true;
	if (!session_begin(m, x->bound)) {
		broke(m, S09_AT_REPLY_START);
		return false;
	}
	m->why.at = S09_AT_NONE;
	if (x->reply == X_IMM || x->reply == X_STATUS) {	/* S-09.030 */
		got = x->reply == X_IMM ? 8u : S09_STATUS_LEN;
		ok = rx(m, at, got) &&
		     s06_rd_le32(at) == s09_hdr_word(x->r.cmd, x->r.slot);
	} else if (x->reply == X_PULL) {	/* S-09.034 */
		ok = rx(m, m->rep, 4) &&
		     s06_rd_le32(m->rep) == s09_hdr_word(S09_PULL, x->r.slot) &&
		     pull_body(m, m->rep + 4, &got);
	} else {
		ok = poll_body(m, &got);
	}
	if (!ok && m->why.at == S09_AT_NONE)
		broke(m, S09_AT_FIRST_WORD);	/* not the shape it must have */
	if (ok && x->rung != S09_RUNG_PLAIN) {
		/* S-09.072: outside the plain mode the reply ends with the
		 * check word of every word before it; a wrong one is a reply
		 * that did not come */
		ok = rx(m, at + got, 4);
		if (ok && s06_rd_le32(at + got) != s09_check_word(at, got)) {
			broke(m, S09_AT_CHECK);
			ok = false;
		}
	}
	if (!ok) {
		let_go(m);
		return false;
	}
	if (!session_end(m)) {
		broke(m, S09_AT_READY);
		return false;
	}
	return true;
}

/* ---- exchanges and the way back (S-09.074, S-09.076) ---- */

#define LINK_TRIES 8u
#define REPEATS 3u		/* S-09.076: a request sent again at most */

/* An echo in the plain mode, in a slot of its own; where
 * it broke in m->echo_why, and m->echo_busy when nothing was clocked: PA6
 * stayed high through the wait for it, the slave still at work. */
static bool echo_plain(struct s09_master *m, uint32_t arg)
{
	struct s09_req r = { S09_ECHO, 0, 0, arg, 0 };
	uint8_t b[S09_HDR];
	int s = lowest_free(m);
	unsigned at = S09_AT_NONE;

	if (s < 0) {
		broke_in(m, &m->echo_why, S09_AT_NONE);
		return false;
	}
	r.slot = (uint8_t)s;
	s09_req_pack(&r, b);
	m->echo_busy = false;
	if (!session_begin(m, S09_T_REPLY_US)) {
		at = S09_AT_READY;
		m->echo_busy = true;
	} else if (!m->bus->send(m->bus->ctx, b, S09_HDR)) {
		broke_in(m, &m->echo_why, S09_AT_SEND);
		let_go(m);
		return false;
	} else if (!session_end(m)) {
		at = S09_AT_READY;
	} else if (!session_begin(m, S09_T_REPLY_US)) {
		at = S09_AT_REPLY_START;
	} else {
		if (!m->bus->recv(m->bus->ctx, b, 8)) {
			broke_in(m, &m->echo_why, S09_AT_REPLY_BROKE);
			at = S09_AT_REPLY_BROKE;
		} else if (s06_rd_le32(b) != s09_hdr_word(S09_ECHO, (uint8_t)s)) {
			at = S09_AT_FIRST_WORD;
		} else if (s06_rd_le32(b + 4) != ~arg) {
			at = S09_AT_VALUE;
		}
		if (!session_end(m) && at == S09_AT_NONE)
			at = S09_AT_READY;
	}
	if (at != S09_AT_REPLY_BROKE)
		broke_in(m, &m->echo_why, at);
	return at == S09_AT_NONE;
}

/*
 * S-09.074 steps 1 and 2: the session ended, both ends back in the plain
 * mode, the slave by itself at its own bound or at the first echo it
 * cannot take; the master asks for the recovery echo (arg0
 * S09_ECHO_RECOVER, arg1 0, no payload), plain, until one is answered. False: the slave is absent.
 */
static bool settle_plain(struct s09_master *m);

/*
 * One exchange. S-09.074: after a fall-back the slave takes nothing in the
 * plain mode but the link's own requests and 0x1B until a 0x19 has been
 * answered, so a plain request of a caller's settles it first.
 */
static bool exchange_once(struct s09_master *m, struct xreq *x)
{
	if (m->unsettled && m->rung == S09_RUNG_PLAIN && !x->link &&
	    x->reply != X_STATUS && !settle_plain(m))
		return false;
	return request(m, x) && reply(m, x);
}

/*
 * S-09.079: the link's status, asked once the recovery echo is answered,
 * plain and once: a failure leaves *st untold and is no error. What broke
 * before it stays the record's.
 */
static void ask_status(struct s09_master *m, struct s09_link_status *st)
{
	const struct s09_fault why = m->why;
	struct s09_req r = { S09_LINK_STATUS, 0, 0, 0, 0 };
	const int s = lowest_free(m);
	struct xreq x;
	struct s09_link_status got;

	if (s < 0)
		return;
	r.slot = (uint8_t)s;
	xreq_init(&x, &r, NULL, 0, X_STATUS);
	s06_fill((uint8_t *)&got, 0, sizeof got);
	if (exchange_once(m, &x)) {
		const uint32_t w = s06_rd_le32(m->stat + 8);

		got.told = true;
		got.fell = s06_rd_le32(m->stat + 4);
		got.why = (uint8_t)w;
		if (got.why == S09_WHY_REPLY_END)
			m->end_seen = true;
		got.cmd = (uint8_t)(w >> 8);
		got.mode = (uint8_t)(w >> 16);
		got.len = s06_rd_le32(m->stat + 12);
		got.got = s06_rd_le32(m->stat + 16);
		for (unsigned i = 0; i < 4u; i++)
			got.words[i] = s06_rd_le32(m->stat + 20 + 4u * i);
	}
	if (st != NULL)
		*st = got;
	m->why = why;
}

/* The bus's plain set-up, which also resets its controller after a failed
 * transfer (S-13b.040; the board's configure(0)). */
static void bus_plain(const struct s09_master *m)
{
	m->bus->configure(m->bus->ctx, 0);
}

static uint32_t t_exec(uint8_t cmd);

/*
 * S-09.074 step 2, S-09.013 rule 4: echoes until one is answered right, each
 * after the quiet wait when the one before was let go (session_begin()).
 * The first session after a let-go is always this request session: a reply
 * the slave offers late, for a request it took whole and went on executing,
 * is clocked out by it and the echo fails, so that reply is consumed, never
 * taken for another's. LINK_TRIES tries that clocked anything; a try that
 * found the slave at work (PA6 high, nothing clocked) is not counted while
 * the request being served may still be executing, T_EXEC from the first.
 * arg + k for the kth try when step is 1, so each try's reply is its own.
 */
static bool echo_until(struct s09_master *m, uint32_t arg, uint32_t step)
{
	const uint32_t t0 = now(m->clock), bound = t_exec(m->cur_cmd);
	unsigned tries = 0;

	for (uint32_t k = 0; tries < LINK_TRIES; k++) {
		if (echo_plain(m, arg + k * step))
			return true;
		if (!m->echo_busy || s06_time_reached(now(m->clock), t0 + bound))
			tries++;
	}
	return false;
}

static bool fall(struct s09_master *m)
{
	const uint32_t t0 = now(m->clock);

	m->bus->select(m->bus->ctx, false);
	m->fell++;
	m->rung = S09_RUNG_PLAIN;
	bus_plain(m);
	m->unsettled = true;		/* until a 0x19 is answered (S-09.074) */
	if (echo_until(m, S09_ECHO_RECOVER, 0u)) {	/* the recovery echo */
		struct s09_link_status *st = NULL;

		m->fall_us = now(m->clock) - t0;
		if (m->bench_st != NULL)
			st = m->bench_st;
		else if (m->quiet)
			st = NULL;
		else if (m->negotiating && m->trying != NULL)
			st = &m->trying->st;
		else if (!m->negotiating && m->falling != NULL)
			st = &m->falling->st;
		ask_status(m, st);
		return true;
	}
	m->fall_us = now(m->clock) - t0;
	absent(m);
	return false;
}

/* A fall-back of x, a request of the caller's, in the record: from its
 * rung, to the plain mode until a rung is kept. */
static bool note_fall(struct s09_master *m, const struct xreq *x)
{
	struct s09_rec_fall *f;

	if (m->quiet || m->negotiating)
		return false;		/* the negotiation's rung has it */
	f = &m->rec.fall[m->rec.falls % S09_REC_FALLS];
	m->rec.falls++;
	s06_fill((uint8_t *)f, 0, sizeof *f);
	f->from = x->rung;
	f->to = S09_RUNG_PLAIN;
	f->cmd = x->r.cmd;
	f->len = x->plen;
	f->tries = x->tries;
	f->fault = m->why;
	m->falling = f;
	return true;
}

static void fell_to(struct s09_master *m, unsigned rung)
{
	if (m->falling != NULL)
		m->falling->to = (uint8_t)rung;
	m->falling = NULL;
}

static bool rung_up(struct s09_master *m, unsigned rung);
static bool rung_one(struct s09_master *m, unsigned tries);
static void negotiation(struct s09_master *m, bool on);

/*
 * S-09.075, S-09.076: the link brought back to a checked rung after x did
 * not get through: one at a time from the rung below the one that failed,
 * never above the ceiling, the next that passes its tests and its gate,
 * else rung 1; from rung 1, rung 1 again. Rung 1 is the plain mode's wire
 * with the check, so a failure there does not strike it. When x's check
 * came from the board's engine and the table gives another, the engine was
 * wrong, not the wire: the engine is not used again and the same rung is
 * entered again. False: the slave is absent.
 */
/* x's check came from the engine and the table gives another: the engine
 * is not used again (S-09.072). Looked at before anything else is sent. */
static bool engine_wrong(struct s09_master *m, const struct xreq *x)
{
	const uint8_t *d = session_buf(m, x);
	const uint32_t body = S09_HDR + s09_pad4(x->plen);

	if (!x->engine || x->rung == S09_RUNG_PLAIN ||
	    s09_check_word(d, body + 4u) == s06_rd_le32(d + body + 4u))
		return false;
	m->soft_check = true;
	return true;
}

static bool recover(struct s09_master *m, struct xreq *x)
{
	unsigned top = x->rung;
	bool ok = false;

	note_fall(m, x);
	m->gate_ok &= (uint8_t)~(1u << x->rung);	/* its verdict ends with a fault */
	if (++x->tries > REPEATS) {
		absent(m);
		fell_to(m, S09_REC_ABSENT);
		return false;
	}
	if (engine_wrong(m, x)) {
		/* the same rung again */
	} else if (top > S09_RUNG_CHECKED) {
		top--;
		if (m->ceiling > top)
			m->ceiling = (uint8_t)top;
	}
	if (top > m->ceiling)
		top = m->ceiling;
	if (!fall(m)) {
		fell_to(m, S09_REC_ABSENT);
		return false;
	}
	/* S-09.075: the next rung down that passes its tests and its gate,
	 * one at a time; with none, rung 1, entered whatever the ceiling */
	negotiation(m, true);
	for (; top > S09_RUNG_CHECKED; top--) {
		ok = rung_up(m, top);
		if (ok || m->absent || m->check_bad)
			break;
		if (m->ceiling >= top)
			m->ceiling = (uint8_t)(top - 1u);
	}
	if (!ok && !m->absent && !m->check_bad)
		ok = rung_one(m, 2u);
	negotiation(m, false);
	if (!ok)
		absent(m);	/* the request's fate is unknown (S-09.076) */
	fell_to(m, ok ? m->rung : S09_REC_ABSENT);
	return ok;
}

/*
 * S-09.013 rule 4: what a failed plain exchange x may have cost, as slots
 * whose replies are to be drained and discarded; false when it may have
 * cost a request outside s09_plain_safe()'s set, the pull's reply among
 * them, or when the request being served is outside it. A poll's reply
 * hands out the oldest finished reply, so a poll that failed before its
 * first word came may have handed out any of them.
 */
static bool plain_stake(const struct s09_master *m, const struct xreq *x,
			uint8_t *stale)
{
	bool ok = s09_plain_safe(m->cur_cmd) || m->cur_cmd == S09_POLL;
	unsigned handed = S09_SLOTS;	/* the slot whose reply went: all */

	*stale = 0;
	if (x->r.cmd == S09_POLL && m->poll_seen) {
		const uint32_t w = m->poll_word;
		const unsigned rs = (uint8_t)(w >> 8);

		if (w == S09_POLL_NONE)
			handed = S09_SLOTS + 1u;	/* none */
		else if (!(w >> 16) && rs < S09_SLOTS && m->slot[rs].cmd == (uint8_t)w)
			handed = rs;
	} else if (x->r.cmd != S09_POLL) {
		handed = S09_SLOTS + 1u;	/* no reply of another's went */
	}
	for (unsigned i = 0; i < S09_SLOTS; i++) {
		const struct s09_slot_state *st = &m->slot[i];

		if (x->r.cmd == S09_POLL && i == x->r.slot)
			continue;		/* the failed poll's own */
		if (st->state == S09_SLOT_PULL) {
			/* the tunnel's bytes would be lost and the pull out of
			 * step: only a reply known to be another's spares it */
			ok = ok && handed != i && handed != S09_SLOTS;
		} else if (st->state == S09_SLOT_WAIT || st->state == S09_SLOT_READ) {
			ok = ok && s09_plain_safe(st->cmd);
			*stale |= (uint8_t)(1u << i);
		}
	}
	return ok;
}

/* A poll in the drain of plain_recover(), whatever PC5 reads (S-09.031): false
 * when it did not get through; *w the first word of its reply, taken
 * unless its slot is stale. */
static bool drain_poll(struct s09_master *m, uint8_t stale, uint32_t *w)
{
	struct s09_req r = { S09_POLL, 0, 0, 0, 0 };
	struct xreq x;
	const int s = lowest_free(m);
	bool ok;

	if (s < 0)
		return false;
	r.slot = (uint8_t)s;
	m->slot[s].state = S09_SLOT_WAIT;
	m->slot[s].cmd = S09_POLL;
	xreq_init(&x, &r, NULL, 0, X_POLL);
	m->poll_seen = false;
	ok = exchange_once(m, &x);
	m->slot[s].state = S09_SLOT_FREE;
	if (!ok)
		return false;
	*w = s06_rd_le32(m->rep);
	if (*w != S09_POLL_NONE && !(stale & 1u << (uint8_t)(*w >> 8)))
		(void)poll_take(m);	/* another's, the pull's: taken as any */
	return true;
}

/*
 * S-09.013 rule 4: a plain exchange x that failed, its stake within
 * s09_plain_safe()'s set: the slave is kept. After the quiet wait
 * (session_begin()) and the bus's plain set-up, which resets its
 * controller, an echo is answered; the slave executes each request before
 * it takes the next session (S-09.031), so every reply to a request it
 * took before the echo is finished by then, and polls until one answers
 * "nothing waiting" drain them, a stale slot's discarded unread by any
 * caller. Then the link's status is asked once, and the request being
 * served fails with EIO. False: no echo was answered, or the drain did not
 * end: the slave is absent.
 */
static bool plain_recover(struct s09_master *m, const struct xreq *x)
{
	uint8_t stale;
	unsigned k;

	if (!plain_stake(m, x, &stale))
		return false;
	bus_plain(m);
	/* the probe echo, its argument new at each try */
	if (!echo_until(m, S09_ECHO_ARG, 1u))
		return false;
	for (k = 0;; k++) {
		uint32_t w;

		if (k == S09_SLOTS + 1u || !drain_poll(m, stale, &w))
			return false;
		if (w == S09_POLL_NONE)
			break;
	}
	for (unsigned i = 0; i < S09_SLOTS; i++)
		if (stale & 1u << i)
			m->slot[i].state = S09_SLOT_FREE;
	m->io_failed = true;
	/* S-09.079: the status, which may be asked at any time in the plain
	 * mode: a reply whose end the slave did not see shows as cause 9
	 * (S-13b.083) */
	ask_status(m, NULL);
	return true;
}

/*
 * x did not get through. In the plain mode the slave is absent (S-09.013),
 * unless S-09.013 rule 4 keeps it.
 * Outside it a request with no number is not sent again: S-09.074's echoes
 * bring both ends back to the plain mode. A numbered one is sent again,
 * with its number, on the rung recover() finds (S-09.076). False: it did
 * not get through; the slave is absent unless it had no number and an
 * echo was answered.
 */
static bool again(struct s09_master *m, struct xreq *x)
{
	if (x->rung == S09_RUNG_PLAIN && !x->link) {
		const bool noted = note_fall(m, x);
		const bool kept = plain_recover(m, x);

		if (noted)
			fell_to(m, kept ? S09_RUNG_PLAIN : S09_REC_ABSENT);
		if (!kept)
			absent(m);
		return false;
	}
	if (x->seq == S09_SEQ_NONE) {
		/* not sent again: its slot is free for the echoes */
		const bool noted = !x->link && note_fall(m, x);
		bool ok;

		(void)engine_wrong(m, x);
		m->slot[x->r.slot].state = S09_SLOT_FREE;
		ok = fall(m);
		if (noted)
			fell_to(m, ok ? S09_RUNG_PLAIN : S09_REC_ABSENT);
		return false;
	}
	while (recover(m, x)) {
		if (exchange_once(m, x))
			return true;
	}
	return false;
}

static bool exchange(struct s09_master *m, struct xreq *x)
{
	return exchange_once(m, x) || again(m, x);
}

/* ---- the tunnel (S-09.034, S-09.045) ---- */

static void rx_put(struct s09_master *m, const uint8_t *d, uint32_t n)
{
	for (uint32_t i = 0; i < n && m->rx_n < S09_TUNNEL_RX; i++) {
		m->rx[(m->rx_r + m->rx_n) % S09_TUNNEL_RX] = d[i];
		m->rx_n++;
	}
}

static uint32_t pull_capacity(const struct s09_master *m)
{
	size_t room = S09_TUNNEL_RX - m->rx_n;

	return (uint32_t)(room < S09_TUNNEL_MAX ? room : S09_TUNNEL_MAX);
}

/* S-09.013: a complete pull reply's count word w and bytes d. */
static void pull_take(struct s09_master *m, uint32_t w, const uint8_t *d)
{
	uint32_t n = w & 0xFFFFu;

	if (w >> 16)
		m->overflow_seen = true;	/* the slave dropped bytes */
	if (n == 0)
		return;
	rx_put(m, d, n);
	m->slot[m->pull_slot].state = S09_SLOT_FREE;
	m->pull_out = false;
	m->pull_parked = false;
}

static bool send_pull(struct s09_master *m)
{
	struct s09_req r = { S09_PULL, 0, 0, 0, 0 };
	uint32_t cap = pull_capacity(m);
	struct xreq x;
	bool ok, was;
	int s;

	/* S-09.045: a pull offers at least one byte; with no room none is
	 * outstanding, and the next goes out once there is room */
	if (m->pull_out || cap == 0 || !m->linked || m->absent || m->released)
		return true;
	s = take_slot(m, S09_K_PULL);
	if (s < 0)
		return false;
	r.slot = (uint8_t)s;
	r.len = (uint16_t)cap;
	m->slot[s].state = S09_SLOT_PULL;
	m->slot[s].cmd = S09_PULL;
	m->pull_slot = (uint8_t)s;
	m->pull_out = true;
	m->pull_parked = true;
	/* its first reply session follows at once (T_REPLY) */
	xreq_init(&x, &r, NULL, 0, X_PULL);
	number(m, &x);
	was = m->in_poll;
	m->in_poll = true;
	ok = exchange(m, &x);
	m->in_poll = was;
	if (!ok) {
		/* none is outstanding: the next call sends one again */
		m->slot[s].state = S09_SLOT_FREE;
		m->pull_out = false;
		m->pull_parked = false;
		return false;
	}
	pull_take(m, s06_rd_le32(m->rep + 4), m->rep + 8);
	return true;
}

/* ---- polls (S-09.031) ---- */

/* A complete poll's reply, from m->rep, given to its slot: true when it
 * was a finished reply. */
static bool poll_take(struct s09_master *m)
{
	const uint32_t w = s06_rd_le32(m->rep);
	struct s09_slot_state *st;
	long dlen;

	if (w == S09_POLL_NONE)
		return false;
	st = &m->slot[(uint8_t)(w >> 8)];
	if (st->state == S09_SLOT_PULL) {
		pull_take(m, s06_rd_le32(m->rep + 4), m->rep + 8);
		return true;
	}
	if (st->cmd == S09_READ) {			/* S-09.035 */
		const int32_t r = (int32_t)s06_rd_le32(m->rep + 4);

		if (r < 0) {
			st->result = r;
			st->err = s06_rd_le32(m->rep + 8);
			st->state = S09_SLOT_DONE;
			return true;
		}
		if (r > 0)
			s06_copy(st->data + st->data_len, m->rep + 8, (uint32_t)r);
		if (s09_read_continues(r, S09_READ_CHUNK, st->data_len, st->arg1)) {
			st->data_len += (uint32_t)r;
			return true;			/* the read stays in its slot */
		}
		st->data_len += (uint32_t)r;
		st->result = (int32_t)st->data_len;
		st->err = 0;
		st->state = S09_SLOT_DONE;
		return true;
	}
	dlen = s09_deferred_data_len(st->cmd, st->arg1);
	st->result = (int32_t)s06_rd_le32(m->rep + 4);
	st->err = s06_rd_le32(m->rep + 8);
	st->data_len = (uint32_t)dlen < st->data_max ? (uint32_t)dlen : st->data_max;
	if (st->data_len)
		s06_copy(st->data, m->rep + 12, st->data_len);
	st->state = S09_SLOT_DONE;
	return true;
}

/* One poll: *got says whether it brought a finished reply. False: it did
 * not get through, and the slave is absent. */
static bool poll_once(struct s09_master *m, bool *got)
{
	struct s09_req r = { S09_POLL, 0, 0, 0, 0 };
	int s = lowest_free(m);
	struct xreq x;
	bool ok, was;

	*got = false;
	if (s < 0)
		return false;
	r.slot = (uint8_t)s;
	m->slot[s].state = S09_SLOT_WAIT;		/* held for its exchange */
	m->slot[s].cmd = S09_POLL;
	xreq_init(&x, &r, NULL, 0, X_POLL);
	number(m, &x);
	m->poll_seen = false;
	was = m->in_poll;
	m->in_poll = true;
	ok = exchange(m, &x);
	m->in_poll = was;
	m->slot[s].state = S09_SLOT_FREE;
	if (!ok)
		return false;
	*got = poll_take(m);
	return true;
}

/* ---- requests ---- */

void s09_master_init(struct s09_master *m, const struct s09_bus_ops *bus,
		     const struct s06_clock *clock)
{
	s06_fill((uint8_t *)m, 0, sizeof *m);
	m->bus = bus;
	m->clock = clock;
	m->ceiling = S09_RUNG_TOP;
	m->plain_word_ns = S09_PLAIN_WORD_NS;
	m->t_quiet_us = S09_T_QUIET_US;
}

void s09_master_check_engine(struct s09_master *m, s09_check_fn fn, void *ctx)
{
	m->check = fn;
	m->check_ctx = ctx;
}

void s09_master_set_absent(struct s09_master *m)
{
	if (!m->absent)
		s06_fill((uint8_t *)&m->lost, 0, sizeof m->lost);
	m->absent = true;
}

bool s09_master_lost(const struct s09_master *m, struct s09_lost *out)
{
	s06_fill((uint8_t *)out, 0, sizeof *out);
	if (!m->absent)
		return false;
	*out = m->lost;
	return true;
}

/* The operation fails: the slave is absent (S-09.008, S-09.013), unless
 * S-09.013 rule 4 kept it: then EIO. */
static int32_t fail(struct s09_master *m, uint32_t *err)
{
	if (m->io_failed && !m->absent) {
		m->io_failed = false;
		*err = S09_EIO;
		return -1;
	}
	*err = S09_ENODEV;
	absent(m);
	return -1;
}

void s09_master_shutdown(struct s09_master *m)
{
	m->shutdown = true;
}

static uint32_t t_exec(uint8_t cmd)
{
	if (cmd == S09_UNMOUNT)
		return S09_T_UNMOUNT_US;
	return cmd == S09_MOUNT ? OPXY_TESTOS_OR(OPXY_TESTOS_MOUNT_CAP_US, S09_T_EXEC_MOUNT_US) : S09_T_EXEC_US;
}

/*
 * S-09.031: a deferred request's reply, polled for while PC5 is low; T_EXEC
 * bounds the wait. Outside the plain mode (S-09.076): with no PC5 low
 * within T_EXEC the request is sent again with its number; and a complete
 * poll that finds nothing waiting while this request's reply has not come
 * shows a request the slave never took (it executes each one before it
 * takes the next session, and a reply it hands out comes whole or is
 * handed out again), so the request is sent again then too. *ran_out:
 * a shutdown's 0x09 whose T_UNMOUNT ran out, given up (S-09.060).
 */
static bool await_reply(struct s09_master *m, struct xreq *x, bool *ran_out)
{
	const struct s09_slot_state *st = &m->slot[x->r.slot];
	uint32_t deadline = now(m->clock) + t_exec(x->r.cmd);

	while (st->state != S09_SLOT_DONE) {
		bool g;

		if (attention(m)) {
			const uint32_t had = st->data_len;

			if (!poll_once(m, &g))
				return false;
			if (st->state == S09_SLOT_DONE)
				break;
			if (m->slot[x->r.slot].data_len != had) {
				/* a piece of this read: the wait starts again */
				deadline = now(m->clock) + t_exec(x->r.cmd);
				continue;
			}
			if (!g && x->seq != S09_SEQ_NONE &&
			    !(st->state == S09_SLOT_READ && st->data_len != 0u)) {
				if (x->tries >= REPEATS) {
					absent(m);
					return false;
				}
				x->tries++;
				if (!exchange(m, x))
					return false;
				deadline = now(m->clock) + t_exec(x->r.cmd);
				continue;
			}
		} else {
			s06_idle(m->clock, now(m->clock) + LINE_STEP_US);
		}
		/* bounded by this request's own wait: replies to others, and
		 * polls that find nothing, do not lengthen it (S-09.060) */
		if (s06_time_reached(now(m->clock), deadline) && m->shutdown &&
		    x->r.cmd == S09_UNMOUNT) {
			*ran_out = true;
			return false;
		}
		if (s06_time_reached(now(m->clock), deadline)) {
			broke(m, S09_AT_EXEC);
			if (!again(m, x))
				return false;
			deadline = now(m->clock) + t_exec(x->r.cmd);
		}
	}
	return true;
}

/* The reply of x, sent and through, as s09_master_call() returns it. */
static int32_t call_reply(struct s09_master *m, struct xreq *x, uint32_t *got,
			  uint32_t *err)
{
	const int kind = s09_kind(x->r.cmd);
	struct s09_slot_state *st = &m->slot[x->r.slot];
	bool ran_out = false;

	if (kind == S09_K_PUSH) {			/* S-09.033 */
		/* S-09.074: no reply shows a push the slave refused; PA6 low
		 * shows it ready for the next session, so PB2 then says
		 * whether it fell back. A poll finds out: one that does not get
		 * through brings the link back (S-09.076), and the push goes
		 * again with its number, which the slave takes at most once. */
		for (unsigned k = 0; x->rung != S09_RUNG_PLAIN && k <= REPEATS; k++) {
			const uint32_t fell = m->fell;
			bool g;

			(void)wait_line(m->clock, m->bus->ready, m->bus->ctx, true,
					S09_T_REPLY_US);
			if (!attention(m))
				break;
			if (!poll_once(m, &g)) {
				st->state = S09_SLOT_FREE;
				return fail(m, err);
			}
			if (m->fell != fell) {
				/* the push itself, in the record */
				broke(m, S09_AT_SLAVE);
				if (note_fall(m, x))
					fell_to(m, m->rung);
				if (k == REPEATS || !exchange(m, x)) {
					st->state = S09_SLOT_FREE;
					return fail(m, err);
				}
			}
			if (m->fell == fell)
				break;
		}
		st->state = S09_SLOT_FREE;
		return 0;
	}
	if (kind == S09_K_IMMEDIATE) {			/* S-09.030 */
		st->state = S09_SLOT_FREE;
		return (int32_t)s06_rd_le32(m->imm + 4);
	}
	if (!await_reply(m, x, &ran_out)) {
		if (ran_out) {
			/* S-09.060: failed, the slave kept, the slot taken */
			*err = S09_EIO;
			return -1;
		}
		st->state = S09_SLOT_FREE;
		return fail(m, err);
	}
	st->state = S09_SLOT_FREE;
	*err = st->err;
	if (got)
		*got = st->data_len;
	if (!send_pull(m))
		return fail(m, err);
	return st->result;
}

/* A slot set up for a request: waiting, with nowhere yet for its data. */
static void slot_wait(struct s09_master *m, const struct s09_req *r,
		      uint8_t *data, uint32_t max)
{
	struct s09_slot_state *st = &m->slot[r->slot];

	st->state = (uint8_t)(r->cmd == S09_READ ? S09_SLOT_READ : S09_SLOT_WAIT);
	st->cmd = r->cmd;
	st->arg1 = r->arg1;
	st->data = data;
	st->data_max = max;
	st->data_len = 0;
}

/* The write left running, as an exchange: its bytes are in m->buf. */
static void bg_xreq(struct s09_master *m, struct xreq *x)
{
	xreq_init(x, &m->bg.req, m->buf + S09_HDR, m->bg.req.arg1, X_NONE);
	x->seq = m->bg.seq;
	x->rung = m->rung;
	x->engine = m->check != NULL && !m->soft_check;
}

/* The write left running, collected: its reply is in m->bg. */
static void bg_settle(struct s09_master *m)
{
	const uint8_t cmd = m->cur_cmd;
	const uint32_t arg0 = m->cur_arg0;
	struct xreq x;
	uint32_t err = 0;

	if (!m->bg.active)
		return;
	m->bg.active = false;
	if (!m->absent)
		serving(m, m->bg.req.cmd, m->bg.req.arg0);
	bg_xreq(m, &x);
	m->bg.result = call_reply(m, &x, NULL, &err);
	m->bg.err = err;
	/* the request that was being served when it was collected */
	m->cur_cmd = cmd;
	m->cur_arg0 = arg0;
}

/* A slot taken and set up for req; -1, with *err, when there is none to
 * have or the request is not one a caller makes. */
static int call_slot(struct s09_master *m, struct s09_req *r,
		     uint8_t *data, uint32_t max, uint32_t *err)
{
	int kind = s09_kind(r->cmd);
	int s;

	*err = 0;
	bg_settle(m);
	/* S-09.008, S-09.052: no bridge traffic */
	if (m->absent || m->released) {
		*err = S09_ENODEV;
		return -1;
	}
	if (kind == S09_K_UNDEFINED || kind == S09_K_POLL || kind == S09_K_PULL ||
	    kind == S09_K_CLOCK) {
		*err = S09_EINVAL;
		return -1;		/* not a request a caller makes */
	}
	s = take_slot(m, kind);
	if (s < 0)
		return fail(m, err);
	r->slot = (uint8_t)s;
	slot_wait(m, r, data, max);
	return s;
}

int32_t s09_master_call(struct s09_master *m, const struct s09_req *req,
			const uint8_t *payload, uint32_t plen, uint8_t *data,
			uint32_t max, uint32_t *got, uint32_t *err)
{
	struct s09_req r = *req;
	struct xreq x;

	if (got)
		*got = 0;
	if (!m->absent)
		serving(m, r.cmd, r.arg0);
	if (call_slot(m, &r, data, max, err) < 0)
		return -1;
	xreq_init(&x, &r, payload, plen,
		  s09_kind(r.cmd) == S09_K_IMMEDIATE ? X_IMM : X_NONE);
	number(m, &x);
	if (!exchange(m, &x)) {
		m->slot[r.slot].state = S09_SLOT_FREE;
		return fail(m, err);
	}
	return call_reply(m, &x, got, err);
}

bool s09_master_write_begin(struct s09_master *m, uint32_t fd,
			    const uint8_t *p, uint32_t n, uint32_t *err)
{
	struct s09_req r = { S09_WRITE, 0, (uint16_t)n, fd, n };
	struct xreq x;

	if (n == 0u || n > s09_master_write_max(m)) {
		*err = S09_EINVAL;
		return false;
	}
	if (!m->absent)
		serving(m, r.cmd, r.arg0);
	if (call_slot(m, &r, NULL, 0, err) < 0)
		return false;
	xreq_init(&x, &r, p, n, X_NONE);
	number(m, &x);
	m->bg.req = r;
	m->bg.seq = x.seq;
	if (!exchange(m, &x)) {
		m->slot[r.slot].state = S09_SLOT_FREE;
		(void)fail(m, err);
		return false;
	}
	m->bg.active = true;		/* collected by write_end */
	return true;
}

int32_t s09_master_write_end(struct s09_master *m, uint32_t *err)
{
	bg_settle(m);
	*err = m->bg.err;
	return m->bg.result;
}

/* ---- the link's rungs (S-09.070-S-09.078) ---- */

unsigned s09_master_rung(const struct s09_master *m)
{
	return m->rung;
}

uint32_t s09_master_fell(const struct s09_master *m)
{
	return m->fell;
}

uint32_t s09_master_write_max(const struct s09_master *m)
{
	return m->rung != S09_RUNG_PLAIN ? S09_WRITE_MAX : S09_TUNNEL_MAX;
}

/*
 * A request of the link's own (0x19, 0x1A), immediate, with no number
 * (S-09.076), in a free slot and with no poll before it: it may run while
 * a numbered request waits to be sent again. Not sent again itself: a
 * failure is its answer, and S-09.074's echoes have brought both ends
 * back to the plain mode (in the plain mode the slave is absent). at: where
 * its session is built, or NULL.
 */
static bool link_req(struct s09_master *m, struct s09_req *r,
		     const uint8_t *payload, uint32_t plen, uint8_t *at,
		     uint32_t *value)
{
	const int s = lowest_free(m);
	struct xreq x;
	bool ok;

	if (s < 0 || m->absent)
		return false;
	r->slot = (uint8_t)s;
	m->slot[s].state = S09_SLOT_WAIT;
	m->slot[s].cmd = r->cmd;
	xreq_init(&x, r, payload, plen, X_IMM);
	x.at = at;
	x.link = true;
	ok = exchange(m, &x);
	m->slot[s].state = S09_SLOT_FREE;
	if (ok)
		*value = s06_rd_le32(m->imm + 4);
	if (ok && r->cmd == S09_LINK_MODE)
		m->unsettled = false;	/* a 0x19 taken ends S-09.074's states */
	return ok;
}

/* S-09.071: the tests' payloads, each a pattern and a length. */
enum { PAT_00, PAT_FF, PAT_AA, PAT_55, PAT_0F, PAT_F0, PAT_WALK, PAT_WALK0,
       PAT_COUNT, PAT_RANDOM };

static const struct {
	uint8_t pat;
	uint32_t len;
} link_test[] = {
	{ PAT_RANDOM, 4u }, { PAT_RANDOM, 61u }, { PAT_COUNT, 0x1000u },
	{ PAT_RANDOM, 0x1004u }, { PAT_00, 0x8000u }, { PAT_FF, 0x8000u },
	{ PAT_AA, 0x8000u }, { PAT_55, 0x8000u }, { PAT_0F, 0x7FFCu },
	{ PAT_F0, 0x8000u }, { PAT_WALK, 0x8000u }, { PAT_WALK0, 0x8000u },
	{ PAT_RANDOM, 0x8000u },
	/* the largest request, a 128-sector write's bytes (S-09.071) */
	{ PAT_RANDOM, S09_TEST_MAX },
};
#define LINK_TESTS (sizeof link_test / sizeof link_test[0])

/*
 * The walking patterns move from bit 1 of the first word, and a random
 * payload's first word has bits 0 and 3 of every nibble set: a listener in
 * single I/O hears a quad session's first word of payload on one data line
 * as the command byte of the header it takes (S-09.071), and these, like
 * the fills and the count, make that byte one that is no command.
 */
static void pattern(uint8_t *d, uint32_t n, unsigned pat, uint32_t seed)
{
	static const uint8_t fill[] = { 0x00, 0xFF, 0xAA, 0x55, 0x0F, 0xF0 };

	for (uint32_t i = 0; i < n; i++) {
		if (pat < sizeof fill) {
			d[i] = fill[pat];
		} else if (pat == PAT_WALK || pat == PAT_WALK0) {
			/* one bit set (clear) in each word, moving up */
			const uint32_t bit = (i / 4u + 1u) % 32u;
			const uint8_t b = bit / 8u == i % 4u ?
					  (uint8_t)(1u << (bit % 8u)) : 0u;

			d[i] = pat == PAT_WALK ? b : (uint8_t)~b;
		} else if (pat == PAT_COUNT) {
			d[i] = (uint8_t)(i + (i >> 8));
		} else {
			seed = seed * 1664525u + 1013904223u;
			d[i] = (uint8_t)(seed >> 24);
			if (i < 4u)
				d[i] |= 0x99u;
		}
	}
}

/* One test of n bytes of a pattern; its value right. Built in m->rep, so
 * that a request waiting to be sent again keeps m->buf. *f: how it broke,
 * S09_AT_VALUE for a whole reply with another value. */
static uint32_t test_build(struct s09_master *m, unsigned pat, uint32_t n,
			   uint32_t seed)
{
	const uint32_t w = s09_pad4(n);
	uint8_t *pl = m->rep + S09_HDR;

	pattern(pl, n, pat, 0x1234567u + seed);
	s06_fill(pl + n, 0, w - n);
	return s09_test_value(pl, w);
}

/* The test test_build() made, sent; its value want. */
static bool test_send(struct s09_master *m, uint32_t n, uint32_t seed,
		      uint32_t want, struct s09_fault *f)
{
	struct s09_req r = { S09_LINK_TEST, 0, (uint16_t)n, seed, n };
	uint32_t v;

	if (!link_req(m, &r, m->rep + S09_HDR, n, m->rep, &v)) {
		*f = m->why;
		return false;
	}
	if (v != want) {
		s06_fill((uint8_t *)f, 0, sizeof *f);
		f->at = S09_AT_VALUE;
		return false;
	}
	return true;
}

/* Every test's request answered with the value of the bytes sent; for the
 * first that is not, *pat and *len say which and *f how. */
static bool link_tests(struct s09_master *m, uint8_t *pat, uint32_t *len,
		       struct s09_fault *f)
{
	for (unsigned k = 0; k < LINK_TESTS; k++) {
		const uint32_t seed = k + m->rung;
		const uint32_t want = test_build(m, link_test[k].pat,
						 link_test[k].len, seed);

		if (!test_send(m, link_test[k].len, seed, want, f)) {
			*pat = link_test[k].pat;
			*len = link_test[k].len;
			return false;
		}
	}
	return true;
}

static unsigned rung_mode(unsigned rung)
{
	return rung >= S09_RUNG_QUAD ? S09_MODE_QUAD : S09_MODE_CHECKED;
}

/* The negotiation's record of a rung tried; with the record quiet, a
 * scratch one. */
static struct s09_rec_rung *rec_rung(struct s09_master *m, unsigned rung)
{
	static struct s09_rec_rung scratch;
	struct s09_rec_rung *e = &scratch;

	if (!m->quiet)
		e = &m->rec.rung[m->rec.nrungs < S09_REC_RUNGS ? m->rec.nrungs++ :
				 S09_REC_RUNGS - 1u];
	s06_fill((uint8_t *)e, 0, sizeof *e);
	e->rung = (uint8_t)rung;
	m->trying = e;
	return e;
}

/* A negotiation begins (its record afresh) or ends (the rung it kept). */
static void negotiation(struct s09_master *m, bool on)
{
	if (on && !m->quiet) {
		m->rec.negotiations++;
		m->rec.nrungs = 0;
	}
	if (!on) {
		m->trying = NULL;
		if (!m->quiet)
			m->rec.kept = m->rung;
	}
	m->negotiating = on;
}

/*
 * S-09.074: a slave that has taken the recovery echo takes nothing but a
 * 0x19 next; a master that stays in the plain mode after a fall-back asks
 * for the plain mode so, and then goes on. False: the slave is absent.
 */
static bool settle_plain(struct s09_master *m)
{
	struct s09_req r = { S09_LINK_MODE, 0, 0, S09_MODE_PLAIN, 0 };
	uint32_t v;

	for (unsigned k = 0; k < LINK_TRIES && !m->absent; k++) {
		if (link_req(m, &r, NULL, 0, NULL, &v) && v == S09_MODE_PLAIN)
			return true;
	}
	absent(m);
	return false;
}

/* S-09.078: from a rung to the plain mode with 0x19 mode 0; when that does
 * not get through, by the echoes, and the slave told it stays there. */
static void lower(struct s09_master *m)
{
	struct s09_req r = { S09_LINK_MODE, 0, 0, S09_MODE_PLAIN, 0 };
	uint32_t v;

	if (m->absent)
		return;
	if (m->rung == S09_RUNG_PLAIN) {
		/* plain already, after a fall-back: the slave told so (S-09.074) */
		if (m->unsettled)
			(void)settle_plain(m);
		return;
	}
	if (link_req(m, &r, NULL, 0, NULL, &v) && v == S09_MODE_PLAIN) {
		m->rung = S09_RUNG_PLAIN;
		m->bus->configure(m->bus->ctx, 0);
	} else if (!m->absent) {
		if (m->rung != S09_RUNG_PLAIN)
			(void)fall(m);
		(void)settle_plain(m);
	}
}

/*
 * S-09.073: one rung tried: asked for, tested, kept. What
 * failed goes to e. A trial that fails with its sessions whole ends with
 * 0x19 mode 0 on the rung; one whose session failed, by the echoes.
 */
static bool link_try(struct s09_master *m, unsigned rung,
		     struct s09_rec_rung *e)
{
	const unsigned mode = rung_mode(rung);
	struct s09_req r = { S09_LINK_MODE, 0, 0, mode, S09_LINK_TRY };
	uint32_t v;

	if (!link_req(m, &r, NULL, 0, NULL, &v)) {
		e->result = S09_RR_ASK;
		e->fault = m->why;
		return false;
	}
	if (v != mode) {
		e->result = S09_RR_ASK;
		s06_fill((uint8_t *)&e->fault, 0, sizeof e->fault);
		e->fault.at = S09_AT_VALUE;
		/* a plain reply has no check: the slave may be on trial all
		 * the same, and the echoes bring it back (S-09.074) */
		if (m->rung == S09_RUNG_PLAIN)
			(void)fall(m);
		return false;
	}
	m->rung = (uint8_t)rung;
	if (!link_tests(m, &e->pat, &e->len, &e->fault)) {
		e->result = S09_RR_TEST;
	} else {
		r.arg1 = S09_LINK_KEEP;
		if (link_req(m, &r, NULL, 0, NULL, &v) && v == mode)
			return true;
		e->result = S09_RR_KEEP;
		e->fault = m->why;
		if (m->rung != S09_RUNG_PLAIN)
			e->fault.at = S09_AT_VALUE;
	}
	/* a test's answer was wrong with its session whole: both ends are
	 * told, or brought, back to the plain mode */
	if (m->rung != S09_RUNG_PLAIN && !m->absent) {
		r.arg0 = S09_MODE_PLAIN;
		r.arg1 = 0;
		(void)link_req(m, &r, NULL, 0, NULL, &v);
		if (m->rung != S09_RUNG_PLAIN) {
			m->rung = S09_RUNG_PLAIN;
			m->bus->configure(m->bus->ctx, 0);
			m->fell++;
		}
	}
	return false;
}

/*
 * S-09.075's gate: three largest test requests sent as requests will go on
 * the rung, each request session timed alone (from its select to its end,
 * the request built and checked beforehand, the reply left out), the least
 * of them against the plain wire's time for the same words, 32 bits a word
 * at the plain clock. Kept only for a gain of at least a sixteenth: the
 * plain mode's own cost is no less than its wire, so the gate is the rung's
 * cost against the least the plain mode could cost; the least of three,
 * and the verdict kept for the boot, keep one slow session from deciding.
 * False with e->result set.
 */
static bool gate(struct s09_master *m, unsigned rung, struct s09_rec_rung *e)
{
	const uint64_t words = (S09_HDR + S09_TEST_MAX) / 4u;
	uint32_t least = UINT32_MAX;

	for (unsigned k = 0; k < S09_GATE_TIMES; k++) {
		const uint32_t seed = 0x60u + rung + k;
		const uint32_t want = test_build(m, PAT_RANDOM, S09_TEST_MAX, seed);
		struct s09_fault f;

		if (!test_send(m, S09_TEST_MAX, seed, want, &f)) {
			e->result = S09_RR_TEST;
			e->pat = PAT_RANDOM;
			e->len = S09_TEST_MAX;
			e->fault = f;
			return false;
		}
		e->big_us[k] = m->session_us;
		if (m->session_us < least)
			least = m->session_us;
	}
	e->plain_us = (uint32_t)((words * m->plain_word_ns + 999u) / 1000u);
	if ((uint64_t)least * 16u > (uint64_t)e->plain_us * 15u) {
		e->result = S09_RR_GATE;
		return false;
	}
	m->gate_ok |= (uint8_t)(1u << rung);
	return true;
}

/*
 * S-09.075: a rung above rung 1 entered, then its gate. True: the link is on
 * the rung.
 */
static bool rung_up(struct s09_master *m, unsigned rung)
{
	const bool soft = m->soft_check;
	struct s09_rec_rung *e = rec_rung(m, rung);

	/* S-09.072: an end that cannot verify its own check leaves the plain
	 * mode for no rung; the tables are built afresh each time */
	m->check_bad = !s09_check_ready();
	if (m->check_bad) {
		e->result = S09_RR_CHECK;
		return false;
	}
	/* a trial the engine's check failed, not the rung: once more with
	 * the table (S-09.072) */
	if (!link_try(m, rung, e) &&
	    (soft || !m->soft_check || m->absent || !link_try(m, rung, e)))
		return false;
	/* a verdict kept for the boot until the rung faults */
	if (!(m->gate_ok & 1u << rung) && !gate(m, rung, e)) {
		lower(m);
		return false;
	}
	e->result = S09_RR_KEPT;
	return true;
}

/*
 * S-09.075: rung 1, the plain wire with the check: never faster than the
 * plain mode and not gated, kept when no faster rung is, so that an
 * install's requests are checked and sent again rather than plain. Entered
 * tries times, a fault on the way back being no proof against it.
 */
static bool rung_one(struct s09_master *m, unsigned tries)
{
	for (unsigned k = 0; k < tries && !m->absent; k++) {
		struct s09_rec_rung *e = rec_rung(m, S09_RUNG_CHECKED);

		m->check_bad = !s09_check_ready();
		if (m->check_bad) {
			e->result = S09_RR_CHECK;
			return false;
		}
		if (link_try(m, S09_RUNG_CHECKED, e)) {
			e->result = S09_RR_KEPT;
			return true;
		}
	}
	return false;
}

bool s09_master_link_fast(struct s09_master *m, unsigned ceiling)
{
	if (m->absent || m->released || !m->linked || m->busy)
		return false;
	bg_settle(m);
	if (!m->absent)
		serving(m, S09_LINK_MODE, ceiling);
	if (ceiling > m->ceiling)
		ceiling = m->ceiling;
	if (!m->absent && ceiling > m->rung) {
		negotiation(m, true);
		for (unsigned rung = ceiling; !m->absent && rung > m->rung &&
		     rung > S09_RUNG_CHECKED; rung--) {
			if (rung_up(m, rung) || m->check_bad)
				break;	/* kept, or no rung with a check unverified */
			if (m->ceiling >= rung)
				m->ceiling = (uint8_t)(rung - 1u);
		}
		/* none faster: rung 1, checked at plain's wire; its own tests
		 * failing leaves the plain mode for the boot */
		if (!m->absent && !m->check_bad && m->rung == S09_RUNG_PLAIN &&
		    m->ceiling >= S09_RUNG_CHECKED && !rung_one(m, 1u))
			m->ceiling = S09_RUNG_PLAIN;
		negotiation(m, false);
	}
	if (!m->absent && m->rung == S09_RUNG_PLAIN && m->unsettled)
		(void)settle_plain(m);	/* S-09.074 */
	return !m->absent;
}

bool s09_master_settled(const struct s09_master *m)
{
	return !m->unsettled;
}

bool s09_master_end_changed(struct s09_master *m)
{
	if (!m->end_seen || m->end_told)
		return false;
	m->end_told = true;
	return true;
}

bool s09_master_check_bad(const struct s09_master *m)
{
	return m->check_bad;
}

bool s09_master_link_plain(struct s09_master *m)
{
	if (m->absent || m->released || !m->linked || m->busy)
		return !m->absent;
	bg_settle(m);
	if (!m->absent)
		serving(m, S09_LINK_MODE, S09_MODE_PLAIN);
	lower(m);
	return !m->absent;
}

void s09_master_fault_source(struct s09_master *m,
			     void (*fn)(void *ctx, uint32_t out[4]), void *ctx)
{
	m->fault_words = fn;
	m->fault_ctx = ctx;
}

void s09_master_fault_more(struct s09_master *m,
			   void (*fn)(void *ctx, uint32_t out[4]), void *ctx)
{
	m->fault_more = fn;
	m->more_ctx = ctx;
}

void s09_master_plain_word_ns(struct s09_master *m, uint32_t ns)
{
	m->plain_word_ns = ns;
}

const struct s09_record *s09_master_record(const struct s09_master *m)
{
	return &m->rec;
}

/* ---- the link bench ---- */

const uint32_t s09_bench_len[S09_BENCH_LENS] = {
	12u, 64u, 256u, 0x1000u, 0x4000u, 0x8000u, S09_TEST_MAX,
};

/* A fault's own account starts afresh: no fall-back's echoes yet, no 0x1B. */
static void bench_fresh(struct s09_master *m)
{
	m->fall_us = 0;
	s06_fill((uint8_t *)&m->echo_why, 0, sizeof m->echo_why);
	s06_fill((uint8_t *)m->bench_st, 0, sizeof *m->bench_st);
}

/* A fault of the run in the record: its first S09_REC_BENCH_FIRST, then its
 * last ones in the rest. */
static void bench_fault(struct s09_master *m, unsigned kind, unsigned rung,
			unsigned result, uint32_t len, unsigned req,
			const struct s09_fault *f)
{
	const uint32_t no = m->rec.bench_faults++;
	const uint32_t at = no < S09_REC_BENCH_FIRST ? no : S09_REC_BENCH_FIRST +
			    (no - S09_REC_BENCH_FIRST) % (S09_REC_BENCH - S09_REC_BENCH_FIRST);
	struct s09_rec_bench *e = &m->rec.bench[at];

	s06_fill((uint8_t *)e, 0, sizeof *e);
	e->no = (uint16_t)(no < 0xFFFFu ? no : 0xFFFFu);
	e->kind = (uint8_t)kind;
	e->rung = (uint8_t)rung;
	e->result = (uint8_t)result;
	e->len = len;
	e->req = (uint16_t)req;
	e->to = m->absent ? (uint8_t)S09_REC_ABSENT : m->rung;
	e->fall_us = m->fall_us;
	e->fault = *f;
	if (m->absent)
		e->echo = m->echo_why;	/* the last echo, none answered */
	e->st = *m->bench_st;
}

/* Where the run ended, a cut or a lost slave. */
static void bench_end(struct s09_bench *b, bool stopped, unsigned rung,
		      uint32_t len, unsigned req)
{
	b->stopped = stopped;
	b->end_rung = (uint8_t)rung;
	b->end_len = len;
	b->end_req = (uint16_t)req;
}

/*
 * On the rung, entered when the link is elsewhere (the plain one: lowered
 * and settled). False: not there, the entry in b and, a fault, in the
 * record.
 */
static bool bench_enter(struct s09_master *m, struct s09_bench *b,
			unsigned rung)
{
	struct s09_rec_rung *e;
	unsigned result;

	if (rung == S09_RUNG_PLAIN) {
		lower(m);
		return !m->absent && !m->unsettled;
	}
	if (m->rung == rung)
		return true;
	bench_fresh(m);
	lower(m);
	e = rec_rung(m, rung);
	m->check_bad = !s09_check_ready();
	if (m->absent) {
		result = S09_RR_NONE;	/* lost while the link was lowered */
		e->fault = m->why;
	} else if (m->check_bad) {
		result = S09_RR_CHECK;
	} else if (!link_try(m, rung, e)) {
		result = e->result;
	} else {
		b->entry[rung] = S09_RR_KEPT;
		b->entries[rung]++;
		return true;
	}
	b->entry[rung] = (uint8_t)result;
	bench_fault(m, S09_BF_ENTRY, rung, result, e->len, 0u, &e->fault);
	return false;
}

/* The bench's own time bound: false once it has run out. */
static bool bench_time(const struct s09_master *m, struct s09_bench *b)
{
	if (b->limit_us != 0u && now(m->clock) - b->t0 >= b->limit_us)
		b->cut = true;
	return !b->cut;
}

/*
 * One rung, every length. A rung that cannot be entered is left. False: the
 * run ends here, its bound run out or the slave lost, and b->end_* says
 * where.
 */
static bool bench_cells(struct s09_master *m, struct s09_bench *b,
			unsigned rung)
{
	for (unsigned l = 0; l < S09_BENCH_LENS; l++) {
		struct s09_bench_cell *c = &b->cell[rung][l];
		const uint32_t n = s09_bench_len[l];

		/* S-09.023: plain cells at the lengths plain requests have */
		if (rung == S09_RUNG_PLAIN && n > S09_BENCH_PLAIN_MAX)
			continue;
		for (unsigned i = 0; i < b->n; i++) {
			struct s09_fault f;
			uint32_t t0, want;

			if (!bench_time(m, b)) {
				bench_end(b, true, rung, n, i);
				return false;
			}
			if (!bench_enter(m, b, rung)) {
				if (!m->absent)
					return true;	/* on with the next rung */
				bench_end(b, true, rung, n, i);
				return false;
			}
			bench_fresh(m);
			want = test_build(m, PAT_RANDOM, n, i);
			t0 = now(m->clock);
			c->sent++;
			if (test_send(m, n, i, want, &f)) {
				c->passed++;
				c->us += now(m->clock) - t0;
			} else {
				c->at[f.at < S09_AT_COUNT ? f.at : S09_AT_NONE]++;
				bench_fault(m, S09_BF_REQUEST, rung, 0u, n, i, &f);
				if (m->absent) {
					bench_end(b, true, rung, n, i);
					return false;
				}
			}
			if (b->between != NULL)
				b->between(b->ctx);
			if ((unsigned)c->sent - c->passed >= S09_BENCH_FAULTS)
				break;	/* on with the next length; the rest not sent */
		}
	}
	return true;
}

bool s09_master_link_bench(struct s09_master *m, struct s09_bench *b)
{
	const uint8_t ceiling = m->ceiling, gate_ok = m->gate_ok;
	const uint32_t fell = m->fell;
	const bool check_bad = m->check_bad;
	struct s09_link_status st;
	bool on = true;

	if (m->absent || m->released || !m->linked || m->busy)
		return false;
	bg_settle(m);
	if (m->absent)
		return false;
	serving(m, S09_LINK_TEST, 0);
	b->t0 = now(m->clock);
	b->cut = false;
	bench_end(b, false, 0u, 0u, 0u);
	s06_fill(b->entry, S09_RR_NONE, sizeof b->entry);
	s06_fill(b->entries, 0, sizeof b->entries);
	m->rec.bench_faults = 0;
	s06_fill((uint8_t *)m->rec.bench, 0, sizeof m->rec.bench);
	m->quiet = true;
	m->bench_st = &st;
	/* one pass (S-09.075): each rung, then the plain mode, every
	 * session as every request's (S-13b.083) */
	for (unsigned rung = S09_RUNG_CHECKED; on && rung <= S09_RUNG_TOP; rung++)
		on = bench_cells(m, b, rung);
	if (on)
		(void)bench_cells(m, b, S09_RUNG_PLAIN);
	/* the end: plain, the slave told so after any fall-back (S-09.074) */
	lower(m);
	m->bench_st = NULL;
	m->quiet = false;
	m->trying = NULL;
	b->fell = m->fell - fell;
	b->settled = !m->absent && !m->unsettled;
	/* the boot's sticky state as it was: a rung struck, a gate's verdict;
	 * its fall-back count stays as it is */
	m->ceiling = ceiling;
	m->gate_ok = gate_ok;
	m->check_bad = check_bad;
	return !m->absent;
}

int32_t s09_master_read(struct s09_master *m, uint32_t fd, uint8_t *buf,
			uint32_t n, uint32_t *err)
{
	struct s09_req r = { S09_READ, 0, (uint16_t)S09_READ_CHUNK, fd, n };

	return s09_master_call(m, &r, 0, 0, buf, n, 0, err);
}

bool s09_master_start(struct s09_master *m)
{
	const struct s09_req init = { S09_INIT, 0, 0, 0, 0 };
	struct s09_req echo = { S09_ECHO, 0, 0, S09_ECHO_ARG, 0 };
	struct xreq x;
	uint32_t err;

	if (m->absent)
		return false;
	m->bus->configure(m->bus->ctx, 0);		/* step 1 */
	/* step 2, S-09.017: the init, slot 0, its header alone. A slave an
	 * earlier master left in a link mode takes it garbled, ends the
	 * session short or long, and falls back: the init is sent again then,
	 * at most LINK_TRIES times in all, and the slave takes it exactly in
	 * its fall-back's state (S-09.074). A reply that does not come, or
	 * another value, leaves the slave absent at once. */
	serving(m, S09_INIT, 0);
	for (unsigned k = 1;; k++) {
		xreq_init(&x, &init, NULL, 0, X_IMM);
		if (request(m, &x))
			break;
		if (!x.started || k == LINK_TRIES) {
			absent(m);
			return false;
		}
	}
	if (!reply(m, &x) || s06_rd_le32(m->imm + 4) != S09_INIT_VALUE) {
		absent(m);
		return false;
	}
	m->bus->configure(m->bus->ctx, 1);		/* step 3 */
	if ((uint32_t)s09_master_call(m, &echo, 0, 0, 0, 0, 0, &err) !=
	    ~S09_ECHO_ARG || err) {			/* step 4 */
		absent(m);
		return false;
	}
	m->linked = true;
	serving(m, S09_PULL, 0);
	if (!send_pull(m)) {				/* step 5 */
		absent(m);
		return false;
	}
	return true;
}

void s09_master_service(struct s09_master *m)
{
	bool got;

	if (m->absent || m->released || !m->linked || m->busy)
		return;
	m->busy = true;
	bg_settle(m);
	while (m->tx_n && !m->absent) {			/* S-09.045 */
		struct s09_req r = { S09_PUSH0, 0, 0, 0, 0 };
		uint32_t k = (uint32_t)(m->tx_n < S09_TUNNEL_MAX ? m->tx_n :
								    S09_TUNNEL_MAX);
		uint32_t err;

		r.len = (uint16_t)k;
		if (s09_master_call(m, &r, m->tx, k, 0, 0, 0, &err) < 0)
			break;
		for (size_t i = k; i < m->tx_n; i++)
			m->tx[i - k] = m->tx[i];
		m->tx_n -= k;
	}
	if (!m->absent)
		serving(m, S09_POLL, 0);
	while (!m->absent && attention(m))
		if (!poll_once(m, &got)) {
			m->io_failed = false;	/* S-09.013 rule 4 kept it, or absent */
			break;
		}
	if (!m->absent)
		serving(m, S09_PULL, 0);
	if (!m->absent && !send_pull(m))
		absent(m);
	m->busy = false;
}

int s09_master_release(struct s09_master *m)
{
	struct s09_req r = { S09_RELEASE, 0, 0, 0, 0 };
	uint32_t err;
	int32_t v;

	(void)s09_master_link_plain(m);			/* S-09.078 */
	v = s09_master_call(m, &r, 0, 0, 0, 0, 0, &err);
	if (err == 0 && v == 0) {			/* S-09.052 */
		m->released = true;
		return 0;
	}
	if (err == 0 && v == 1)
		return 1;
	absent(m);
	return -1;
}

/* ---- the CPU link's port ---- */

static size_t tun_read(void *ctx, uint8_t *buf, size_t max)
{
	struct s09_master *m = ctx;
	size_t n = 0;

	while (n < max && m->rx_n) {
		buf[n++] = m->rx[m->rx_r];
		m->rx_r = (m->rx_r + 1u) % S09_TUNNEL_RX;
		m->rx_n--;
	}
	return n;
}

/* cppcheck-suppress constParameterCallback ; s06_port_ops fixes the type */
static size_t tun_room(void *ctx)
{
	const struct s09_master *m = ctx;

	return m->absent || m->released ? 0 : S09_TUNNEL_TX - m->tx_n;
}

static void tun_write(void *ctx, const uint8_t *d, size_t n)
{
	struct s09_master *m = ctx;

	s06_copy(m->tx + m->tx_n, d, n);
	m->tx_n += n;
	s09_master_service(m);
}

const struct s06_port_ops s09_tunnel_port = { tun_read, tun_room, tun_write };

/* ---- CPU-link messages ---- */

void s09_clock_message(struct s09_clock_msg *c, struct s06_node *node,
		       unsigned link, uint32_t secs)
{
	uint8_t p[4];

	if (c->sent && c->last == secs)
		return;
	c->sent = true;
	c->last = secs;
	s06_wr_le32(p, secs);		/* S-16.063: no daylight-saving offset */
	(void)s06_send(node, link, 0, 0xBD, p, 4);
}

/* S-04.070: the days field is 2 short from 2001-01-01 (count 11321) on */
#define RTC_SKIP_CODE 11321u

uint32_t s09_clock_seconds(uint32_t clk)
{
	uint32_t days = clk >> 17;

	if (days >= RTC_SKIP_CODE)
		days += 2u;
	return days * 86400u + ((clk >> 12) & 0x1Fu) * 3600u +
	       ((clk >> 6) & 0x3Fu) * 60u + (clk & 0x3Fu);
}

void s09_clock_rtc(struct s09_clock_msg *c, struct s06_node *node,
		   unsigned link, uint32_t clk)
{
	s09_clock_message(c, node, link, s09_clock_seconds(clk));
}

bool s09_power_off(struct s09_master *m, struct s06_node *node, unsigned link)
{
	static const uint8_t v[1] = { 0x14 };
	uint32_t end_t;
	struct s06_frame f;

	if (m->absent)
		return false;		/* S-09.008: no power-off request */
	(void)s06_send(node, link, 0, 0x31, v, 1);
	end_t = now(m->clock) + S09_T_POWER_US;
	for (;;) {
		s09_master_service(m);
		s06_node_poll_link(node, link);
		while (s06_link_take(node->link[link], &f))
			if (f.id == 0x32)
				return true;
		if (m->absent || s06_time_reached(now(m->clock), end_t))
			return false;	/* S-09.047: go on as if it had */
		s06_idle(m->clock, now(m->clock) + LINE_STEP_US);
	}
}
