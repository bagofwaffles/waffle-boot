/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09, the slave's side: the bridge server and its CPU-link answers.
 */
#include <stddef.h>

#include "waffle/s09_slave.h"
#include "waffle/bytes.h"

/* sector_read() hands the big buffer's data, at + 12, to the eMMC driver's
 * DMA: a 32-byte boundary in every build (S09_DMA_ALIGN). */
_Static_assert(offsetof(struct s09_slave, big_room) % S09_DMA_ALIGN == 0 &&
	       _Alignof(struct s09_slave) >= S09_DMA_ALIGN &&
	       (S09_DMA_LEAD + S09_HDR) % S09_DMA_ALIGN == 0,
	       "a sector read's data must start on a 32-byte boundary");
/* S-09.072: every reply and its check word fit where it is made. */
_Static_assert(sizeof(((struct s09_slave *)0)->big_room) - S09_DMA_LEAD >=
	       S09_HDR + S09_MAX_SECTORS * S09_SECTOR + 4u &&
	       sizeof(((struct s09_slave *)0)->out) >= 8u + S09_TUNNEL_MAX + 4u &&
	       sizeof(((struct s09_slave *)0)->out) >= 8u + S09_READ_CHUNK + 4u &&
	       sizeof(((struct s09_slave *)0)->out) >= S09_HDR + S09_DIRENT_LEN + 4u &&
	       sizeof(((struct s09_slave *)0)->np_buf) >= sizeof(((struct s09_slave *)0)->out) &&
	       sizeof(((struct s09_slave *)0)->uout) >= S09_STATUS_LEN + 4u,
	       "a reply and its check overrun the slave's buffer");

void s09_slave_init(struct s09_slave *s, const struct s09_store_ops *ops)
{
	wb_fill((uint8_t *)s, 0, sizeof *s);
	s->ops = ops;
}

void s09_slave_fault_source(struct s09_slave *s,
			    void (*fn)(void *ctx, uint32_t out[4]), void *ctx)
{
	s->fault_words = fn;
	s->fault_ctx = ctx;
}

/* S-09.079: what the last fall-back met; the words from the hook for a
 * failed receive, zeros else. A cause 9 not yet reported stays (false). */
static bool note_why(struct s09_slave *s, unsigned why, const uint8_t *hdr,
		     size_t got)
{
	struct s09_req r = { 0, 0, 0, 0, 0 };

	if (s->why_held)
		return false;
	if (hdr != NULL)
		s09_req_unpack(hdr, &r);
	s->why = (uint8_t)why;
	s->why_cmd = r.cmd;
	s->why_mode = s->mode;
	s->why_len = hdr != NULL ? s09_payload_len(&r) : 0u;
	s->why_got = (uint32_t)got;
	wb_fill((uint8_t *)s->why_words, 0, sizeof s->why_words);
	if (why == S09_WHY_RECEIVE && s->fault_words != NULL)
		s->fault_words(s->fault_ctx, s->why_words);
	return true;
}

void s09_slave_reply_end_changed(struct s09_slave *s, uint8_t cmd, size_t n)
{
	s->why_held = false;
	(void)note_why(s, S09_WHY_REPLY_END, NULL, n);
	s->why_cmd = cmd;
	s->why_held = true;
}

/* S-09.074: back to the plain mode, PB2 low, the cause kept. */
static void fall_back(struct s09_slave *s, unsigned why, const uint8_t *hdr,
		      size_t got)
{
	if (s->mode == S09_MODE_PLAIN)
		return;
	(void)note_why(s, why, hdr, got);
	s->mode = S09_MODE_PLAIN;
	s->trial = false;
	s->fell_notice = true;
	s->recovering = false;
	s->fell++;
}

bool s09_slave_attention(const struct s09_slave *s)
{
	return s->nqueue > 0 || s->fell_notice || s->replay;
}

unsigned s09_slave_mode(const struct s09_slave *s)
{
	return s->mode;
}

bool s09_slave_on_trial(const struct s09_slave *s)
{
	return s->trial;
}

void s09_slave_link_fault(struct s09_slave *s)
{
	fall_back(s, S09_WHY_RECEIVE, NULL, 0);
}

void s09_slave_rx_failed(struct s09_slave *s, const uint8_t *hdr, size_t got)
{
	fall_back(s, S09_WHY_RECEIVE, hdr, got);
}

void s09_slave_trial_lapsed(struct s09_slave *s)
{
	if (s->trial)
		fall_back(s, S09_WHY_TRIAL, NULL, 0);
}

/* S-09.072: the bytes a request's check adds in the mode. */
static uint32_t check_len(const struct s09_slave *s)
{
	return s->mode == S09_MODE_PLAIN ? 0u : S09_CHECK_LEN;
}

/* S-09.015: the init exactly as a master starting up sends it, header
 * alone: 01 00 00 00, arg0 0, arg1 0. */
static bool exact_init(const uint8_t *hdr)
{
	return wb_get_le32(hdr) == S09_INIT && wb_get_le32(hdr + 4) == 0u &&
	       wb_get_le32(hdr + 8) == 0u;
}

static bool ignored(const struct s09_slave *s, const uint8_t *hdr)
{
	struct s09_req r;

	if (s->released)
		return true;		/* S-09.052: no further header */
	s09_req_unpack(hdr, &r);
	/* S-09.016, S-16.113: before the link, a first word over 0xFF */
	if (!s->linked && wb_get_le32(hdr) > 0xFFu)
		return true;
	/* S-09.074: after a fall-back, anything but the recovery echo or a
	 * master's init, each exactly, other echoes included, is dropped at
	 * its header, with no reply and no effect: a garbled session can read
	 * as an echo, not as these */
	if ((s->fell_notice || s->recovering) && !exact_init(hdr) &&
	    (r.cmd != S09_ECHO || r.len != 0u || r.arg0 != S09_ECHO_RECOVER ||
	     r.arg1 != 0u) &&
	    /* ... and after it, until the master asks for a mode, that too:
	     * a plain 0x19 a bit error had made another command would be
	     * executed with no check (S-09.074) */
	    !(s->recovering && r.cmd == S09_LINK_MODE && r.len == 0u &&
	      r.arg0 < S09_MODE_COUNT && r.arg1 == 0u) &&
	    /* ... and the link's status, exactly (S-09.079) */
	    !(s->recovering && s09_status_exact(&r)))
		return true;
	return r.slot >= S09_SLOTS;	/* S-09.037: a bad slot, like noise */
}

size_t s09_slave_rx_keep(const struct s09_slave *s, const uint8_t *hdr)
{
	struct s09_req r;

	if (!hdr)
		return 8;		/* S-09.036 step 2 */
	if (ignored(s, hdr))
		return S09_HDR;
	s09_req_unpack(hdr, &r);
	/* S-09.021: the slave's rule, pad4 of n, never lengthened */
	return S09_HDR + s09_pad4(s09_payload_len(&r)) + check_len(s);
}

/*
 * S-09.021: when pad4(n) is 0x44 or 0x48 the slave receives
 * the master's further 8 or 4 bytes in the same session and ignores them,
 * and its ready line rises after the last of them. The request is whole
 * without them: if PB15 ends the session first, the board ends this
 * receive and the request is still executed (S-09.013).
 */
uint32_t s09_slave_rx_trail(const struct s09_slave *s, const uint8_t *hdr)
{
	struct s09_req r;
	uint32_t n;

	/* S-09.072: no lengthened payload outside the plain mode */
	if (!hdr || ignored(s, hdr) || s->mode != S09_MODE_PLAIN)
		return 0;
	s09_req_unpack(hdr, &r);
	n = s09_payload_len(&r);
	return s09_master_payload_wire(n) - s09_pad4(n);
}

uint64_t s09_slave_rx_len(const struct s09_slave *s, const uint8_t *hdr)
{
	struct s09_req r;

	if (!hdr || ignored(s, hdr))
		return s09_slave_rx_keep(s, hdr);
	s09_req_unpack(hdr, &r);
	/* S-09.037: a malformed write's arg1 x 512 bytes, a multiple of 4 */
	return (uint64_t)s09_slave_rx_keep(s, hdr) + s09_payload_discard(&r);
}

static void set_reply(struct s09_slave *s, const uint8_t *d, size_t n)
{
	s->reply = d;
	s->reply_len = n;
	s->release_queued = false;
}

bool s09_slave_reply(struct s09_slave *s, const uint8_t **d, size_t *n)
{
	if (!s->reply_len)
		return false;
	*d = s->reply;
	*n = s->reply_len;
	s->reply_len = 0;
	if (s->release_queued) {
		s->release_queued = false;
		s->release_sent = true;
	}
	return true;
}

bool s09_slave_take_release(struct s09_slave *s, uint32_t *value)
{
	if (!s->release_sent)
		return false;
	s->release_sent = false;
	*value = s->release_value;
	return true;
}

/* ---- finished replies (S-09.031) ---- */

static void finish(struct s09_slave *s, unsigned slot)
{
	s->slot[slot].state = S09_SS_DONE;
	s->queue[s->nqueue++] = (uint8_t)slot;
}

/* [hdr][result][errno] and dlen data bytes (S-09.032) */
static uint8_t *deferred_head(uint8_t *b, const struct s09_req *r, int32_t res,
			      uint32_t err)
{
	wb_put_le32(b, s09_hdr_word(r->cmd, r->slot));
	wb_put_le32(b + 4, (uint32_t)res);
	wb_put_le32(b + 8, err);
	return b + 12;
}

static void small_reply(struct s09_slave *s, unsigned slot, int32_t res,
			uint32_t err, const uint8_t *data, uint32_t dlen)
{
	struct s09_sslot *x = &s->slot[slot];
	uint8_t *p = deferred_head(x->small, &x->req, res, err);

	if (dlen) {
		if (data)
			wb_copy(p, data, dlen);
		else
			wb_fill(p, 0, dlen);
	}
	x->big = false;
	x->len = 12u + dlen;
	finish(s, slot);
}

/* S-09.034: [hdr][w] then n bytes, pad4 */
static size_t pull_reply(struct s09_slave *s, const struct s09_req *r,
			 uint8_t *b)
{
	uint32_t n = (uint32_t)(s->tx_n < r->len ? s->tx_n : r->len);

	if (n > S09_TUNNEL_MAX)
		n = S09_TUNNEL_MAX;
	wb_put_le32(b, s09_hdr_word(r->cmd, r->slot));
	wb_put_le32(b + 4, n | (s->dropped ? 1u << 16 : 0u));
	s->dropped = false;
	wb_copy(b + 8, s->tx, n);
	wb_fill(b + 8 + n, 0, s09_pad4(n) - n);
	for (size_t i = n; i < s->tx_n; i++)
		s->tx[i - n] = s->tx[i];
	s->tx_n -= n;
	return 8u + s09_pad4(n);
}

/* A parked pull's reply is finished once there are bytes to send. */
static void pull_check(struct s09_slave *s)
{
	for (unsigned i = 0; i < S09_SLOTS; i++) {
		struct s09_sslot *x = &s->slot[i];

		if (x->state != S09_SS_PULL || !s->tx_n)
			continue;
		x->len = (uint32_t)pull_reply(s, &x->req, x->small);
		x->big = false;
		finish(s, i);
	}
}

/* S-09.035: one piece into the slot's reply */
static void read_piece(struct s09_slave *s, unsigned slot)
{
	struct s09_sslot *x = &s->slot[slot];
	uint32_t c = x->req.len, n = x->req.arg1, err = 0;
	uint8_t *b = x->small;
	int32_t r;

	wb_put_le32(b, s09_hdr_word(x->req.cmd, x->req.slot));
	/* S-09.037: a C of 0 or above 0x1000: one failed piece, EINVAL */
	if (c == 0 || c > S09_READ_CHUNK) {
		r = -1;
		err = S09_EINVAL;
	} else {
		r = s->ops->read(s->ops->ctx, x->req.arg0, b + 8,
				 s09_read_ask(n, x->received, c), &err);
	}
	wb_put_le32(b + 4, (uint32_t)r);
	if (r > 0) {
		wb_fill(b + 8 + r, 0, s09_pad4((uint32_t)r) - (uint32_t)r);
		x->len = 8u + s09_pad4((uint32_t)r);
	} else if (r == 0) {
		x->len = 8;
	} else {
		wb_put_le32(b + 8, err);
		x->len = 12;
	}
	x->big = false;
	finish(s, slot);
}

/* S-09.040: a sector read, its data in the big buffer */
static void sector_read(struct s09_slave *s, unsigned slot)
{
	struct s09_sslot *x = &s->slot[slot];
	uint32_t count = x->req.cmd == S09_READ_SECTOR ? 1u : x->req.arg1;
	int32_t res;
	uint8_t *p;

	/* S-09.037: a count outside 1-128: no eMMC operation, EINVAL, no data */
	if (count < 1 || count > S09_MAX_SECTORS) {
		small_reply(s, slot, -1, S09_EINVAL, 0, 0);
		return;
	}
	if (s->big_busy) {
		x->state = S09_SS_WAIT_BIG;	/* runs when the buffer frees */
		return;
	}
	p = deferred_head(s09_slave_big(s), &x->req, 0, 0);
	res = s->ops->emmc(s->ops->ctx, x->req.cmd, x->req.arg0, x->req.arg1, p,
			   count * S09_SECTOR);
	if (res)
		wb_fill(p, 0, count * S09_SECTOR);	/* no information */
	wb_put_le32(s09_slave_big(s) + 4, (uint32_t)res);
	s->big_busy = true;
	x->big = true;
	x->len = 12u + count * S09_SECTOR;
	finish(s, slot);
}

/* S-09.041, S-16.117: a path payload; relative ones are under /yaffs2/. */
/* 0, or -1 for a malformed path, -2 for one too long to hold */
static int path_of(struct s09_slave *s, int which, const uint8_t *p,
		   uint32_t n)
{
	static const char root[8] = { '/', 'y', 'a', 'f', 'f', 's', '2', '/' };
	char *out = s->path[which];
	size_t o = 0;

	/* S-09.037: malformed when empty or when its last byte is not NUL;
	 * the path ends at its first NUL */
	if (n == 0 || p[n - 1] != 0)
		return -1;
	if (p[0] != '/')
		for (; o < sizeof root; o++)
			out[o] = root[o];
	for (uint32_t i = 0; p[i]; i++) {
		if (o + i + 1 >= sizeof s->path[which])
			return -2;	/* longer than this slave holds */
		out[o + i] = (char)p[i];
		out[o + i + 1] = '\0';
	}
	if (p[0] == 0)
		out[o] = '\0';
	return 0;
}

static bool path_cmd(uint8_t cmd)
{
	switch (cmd) {
	case S09_ACCESS:
	case S09_STAT:
	case S09_STATFS:
	case S09_UNLINK:
	case S09_MKDIR:
	case S09_RMDIR:
	case S09_SYNC:
	case S09_OPEN:
	case S09_OPENDIR:
	case S09_FREE_SPACE:
	case S09_TOTAL_SPACE:
	case S09_UTIME:
		return true;
	default:
		return false;
	}
}

static void file_cmd(struct s09_slave *s, unsigned slot, const uint8_t *pl,
		     uint32_t n)
{
	struct s09_sslot *x = &s->slot[slot];
	const struct s09_req *r = &x->req;
	uint8_t out[S09_DIRENT_LEN];
	long dlen = s09_deferred_data_len(r->cmd, r->arg1);
	const char *p1 = 0, *p2 = 0;
	uint32_t err = 0;
	int32_t res;
	bool fail_data;

	wb_fill(out, 0, sizeof out);
	if (dlen < 0)
		dlen = 0;
	if (r->cmd == S09_RENAME) {
		/* S-09.037: arg1 0 or not less than len is malformed */
		int a = r->arg1 >= n ? -1 : path_of(s, 0, pl, r->arg1);
		int b = a ? 0 : path_of(s, 1, pl + r->arg1, n - r->arg1);

		if (a || b) {
			small_reply(s, slot, -1, a == -1 || b == -1 ? S09_EINVAL :
				    S09_ENAMETOOLONG, 0, 0);
			return;
		}
		p1 = s->path[0];
		p2 = s->path[1];
	} else if (path_cmd(r->cmd)) {
		int a = path_of(s, 0, pl, n);

		if (a) {
			/* S-09.037: the failure result of S-09.041's table
			 * (0 for opendir and the space commands, else -1),
			 * zero data */
			small_reply(s, slot, r->cmd == S09_OPENDIR ||
					     r->cmd == S09_FREE_SPACE ||
					     r->cmd == S09_TOTAL_SPACE ? 0 : -1,
				    a == -1 ? S09_EINVAL : S09_ENAMETOOLONG, 0,
				    (uint32_t)dlen);
			return;
		}
		p1 = s->path[0];
	}
	res = s->ops->file(s->ops->ctx, r, p1, p2, pl, n, out, &err);
	/* S-09.032: a failed result's data are zeros */
	fail_data = r->cmd == S09_READDIR ? res == 0 :
		    (r->cmd == S09_STAT || r->cmd == S09_STATFS) && res < 0;
	small_reply(s, slot, res, err, fail_data ? 0 : out, (uint32_t)dlen);
}

static void deferred(struct s09_slave *s, unsigned slot, const uint8_t *pl,
		     uint32_t n)
{
	struct s09_sslot *x = &s->slot[slot];
	const struct s09_req *r = &x->req;
	int32_t res;

	switch (r->cmd) {
	case S09_READ_SECTOR:
	case S09_READ_SECTORS:
		sector_read(s, slot);
		return;
	case S09_WRITE_RELIABLE:
	case S09_WRITE_SECTORS:
		/* S-09.037: a count outside 1-128: the payload (arg1 x 512
		 * bytes, s09_payload_discard()) was discarded by the driver;
		 * no eMMC operation, EINVAL, no data */
		if (!s09_count_ok(r)) {
			small_reply(s, slot, -1, S09_EINVAL, 0, 0);
			return;
		}
		/* fall through */
	case S09_WRITE_SECTOR:
	case S09_FLUSH:
	case S09_ERASE:
	case S09_SELECT:
		/* S-09.040: errno is 0 */
		res = s->ops->emmc(s->ops->ctx, r->cmd, r->arg0, r->arg1,
				   (uint8_t *)(uintptr_t)pl, n);
		small_reply(s, slot, res, 0, 0, 0);
		return;
	case S09_READ:
		x->received = 0;
		read_piece(s, slot);
		return;
	default:
		file_cmd(s, slot, pl, n);
		return;
	}
}

/* ---- request sessions ---- */

static void clock_second(struct s09_slave *s, const uint8_t *d)
{
	uint64_t count = (uint64_t)wb_get_le32(d) | (uint64_t)wb_get_le32(d + 4) << 32;

	s->clock_second = false;
	if (count == 0)
		return;			/* the master timed out: nothing more */
	if (s->clock_req.arg0 == 0 && s->ops->adjust)
		s->ops->adjust(s->ops->ctx, (int64_t)(count - s->clock_sample));
	wb_put_le32(s->out, s09_hdr_word(S09_CLOCK_SYNC, s->clock_req.slot));
	wb_put_le32(s->out + 4, 0);
	wb_put_le32(s->out + 8, (uint32_t)s->clock_sample);
	wb_put_le32(s->out + 12, (uint32_t)(s->clock_sample >> 32));
	set_reply(s, s->out, 16);
}

/* S-09.070, S-09.073: the mode asked for; the value is the mode the slave
 * is in after it. */
static uint32_t link_mode(struct s09_slave *s, const struct s09_req *r)
{
	if (r->arg0 >= S09_MODE_COUNT)
		return s->mode;			/* no such mode: as it was */
	/* S-09.072: an end that cannot verify its own check stays in the
	 * plain mode; the tables are built afresh for each request */
	if (r->arg0 == S09_MODE_PLAIN || !s09_check_ready()) {
		if (r->arg0 != S09_MODE_PLAIN &&	/* S-09.079 */
		    note_why(s, S09_WHY_KAT, NULL, 0))
			s->why_cmd = S09_LINK_MODE;
		s->mode = S09_MODE_PLAIN;
		s->trial = false;
	} else if (r->arg1 == S09_LINK_KEEP) {
		if (s->mode == r->arg0)		/* kept only from its trial */
			s->trial = false;
	} else {
		s->mode = (uint8_t)r->arg0;
		s->trial = true;
	}
	return s->mode;
}

static void immediate(struct s09_slave *s, const struct s09_req *r,
		      const uint8_t *pl, uint32_t plen)
{
	uint32_t v = 0;

	switch (r->cmd) {			/* S-09.030 */
	case S09_LINK_MODE:
		v = link_mode(s, r);
		break;
	case S09_LINK_TEST:			/* S-09.071 */
		v = s09_test_value(pl, s09_pad4(plen));
		break;
	case S09_INIT:
		v = S09_INIT_VALUE;
		break;
	case S09_LINK_STATUS:			/* S-09.079: below */
		break;
	case S09_ERASE_GROUP:
		v = s->ops->erase_group(s->ops->ctx);
		break;
	case S09_BOOT_SIZE:
		v = s->ops->boot_size(s->ops->ctx);
		break;
	OPXY_TESTOS(case OPXY_TESTOS_ROW_REQ: case OPXY_TESTOS_PEEK_REQ: case OPXY_TESTOS_POKE_REQ: v = s->ops->testos_row(s->ops->ctx, r->cmd, r->arg0, r->arg1); break;)
#if !defined(OPXY_TEST_OS)
	case OPXY_DEV_PEEK_REQ: case OPXY_DEV_POKE_REQ: v = s->ops->dev_cmd(s->ops->ctx, r->cmd, r->arg0, r->arg1); break;
#endif
	case S09_RELEASE:
		/* S-09.051: staged and checked before the reply */
		v = s->ops->os_status(s->ops->ctx);
		s->released = v == 0;
		break;
	default:				/* S09_ECHO */
		v = ~r->arg0;
		s->linked = true;		/* S-09.015 step 4 */
		break;
	}
	{
		/* S-09.076: an unnumbered request's reply leaves the last
		 * numbered one's where it is */
		uint8_t *o = s09_unnumbered(r->cmd) ? s->uout : s->out;
		size_t len = 8;

		wb_put_le32(o, s09_hdr_word(r->cmd, r->slot));
		wb_put_le32(o + 4, v);
		if (r->cmd == S09_LINK_STATUS) {	/* S-09.079 */
			wb_put_le32(o + 4, s->fell);
			wb_put_le32(o + 8, (uint32_t)s->why |
					   (uint32_t)s->why_cmd << 8 |
					   (uint32_t)s->why_mode << 16);
			wb_put_le32(o + 12, s->why_len);
			wb_put_le32(o + 16, s->why_got);
			for (unsigned i = 0; i < 4u; i++)
				wb_put_le32(o + 20 + 4u * i, s->why_words[i]);
			len = S09_STATUS_LEN;
			s->why_held = false;	/* cause 9 reported */
		}
		set_reply(s, o, len);
	}
	if (r->cmd == S09_RELEASE) {	/* S-09.052: acted on once it is sent */
		s->release_queued = true;
		s->release_value = v;
	}
}

/* S-09.031 step 4: the oldest finished reply, or 06 FF 00 00 */
static void poll(struct s09_slave *s)
{
	struct s09_sslot *x;
	unsigned slot;

	if (s->replay) {
		/* S-09.076: a repeated request's finished reply, again */
		s->replay = false;
		if (s->np_big) {
			set_reply(s, s09_slave_big(s), s->np_len);
		} else {
			wb_copy(s->out, s->np_buf, s->np_len);
			set_reply(s, s->out, s->np_len);
		}
		return;
	}
	if (s->nqueue == 0) {
		wb_put_le32(s->out, S09_POLL_NONE);
		set_reply(s, s->out, 4);
		return;
	}
	slot = s->queue[0];
	for (unsigned i = 1; i < s->nqueue; i++)
		s->queue[i - 1] = s->queue[i];
	s->nqueue--;
	x = &s->slot[slot];
	/* the reply goes out now; copy it so the slot can move on */
	if (x->big) {
		set_reply(s, s09_slave_big(s), x->len);	/* freed below, sent next */
	} else {
		wb_copy(s->out, x->small, x->len);
		set_reply(s, s->out, x->len);
	}
	/* S-09.076: the last numbered request but a poll, deferred: its
	 * first finished reply is kept for a repeat of it */
	if (s->np_deferred && !s->np_handed && slot == s->np_slot) {
		s->np_handed = true;
		s->np_big = x->big;
		s->np_len = x->len;
		if (!x->big)
			wb_copy(s->np_buf, x->small, x->len);
	}
	if (x->req.cmd == S09_READ) {
		int32_t r = (int32_t)wb_get_le32(x->small + 4);

		if (s09_read_continues(r, x->req.len, x->received, x->req.arg1)) {
			x->received += (uint32_t)r;
			read_piece(s, slot);	/* the next piece */
			return;
		}
	}
	x->state = S09_SS_FREE;
	if (x->big) {
		/* reused only after its reply session: a waiting sector read
		 * runs at the next request. A numbered poll's reply stays there
		 * while the slave keeps it for a repeat (S-09.076). */
		s->big_held = s->mode != S09_MODE_PLAIN;
		s->big_busy = s->big_held;
		x->big = false;
	}
}

/* big is free again once no kept reply is in it. */
static void release_big(struct s09_slave *s)
{
	if (s->big_held && !s->np_big && !(s->p_len != 0u && s->p_reply == s09_slave_big(s))) {
		s->big_held = false;
		s->big_busy = false;
	}
}

/* S-09.076: the last poll, and the last numbered request but a poll, are
 * no longer kept. */
static void forget_poll(struct s09_slave *s)
{
	s->p_seq = 0;
	s->p_reply = NULL;
	s->p_len = 0;
	release_big(s);
}

static void forget_np(struct s09_slave *s)
{
	s->np_seq = 0;
	s->np_deferred = false;
	s->np_handed = false;
	s->np_big = false;
	s->replay = false;
	s->np_len = 0;
	release_big(s);
}

static void forget(struct s09_slave *s)
{
	forget_np(s);
	forget_poll(s);
}

/* S-09.076: a repeat of the last numbered request but a poll: nothing
 * executed; its reply again, or its finished reply queued again for a poll
 * once one has handed it out. */
static void repeat_np(struct s09_slave *s)
{
	if (s->np_deferred) {
		s->replay = s->np_handed;
		return;
	}
	if (s->np_len == 0u)
		return;			/* a push: no reply */
	set_reply(s, s->np_buf, s->np_len);
	if (s->np_cmd == S09_RELEASE) {
		s->release_queued = true;
		s->release_value = wb_get_le32(s->np_buf + 4);
		s->released = s->release_value == 0u;
	}
}

static void run_waiting(struct s09_slave *s)
{
	for (unsigned i = 0; i < S09_SLOTS && !s->big_busy; i++)
		if (s->slot[i].state == S09_SS_WAIT_BIG)
			sector_read(s, i);
}

/* A whole request, its check right: executed. d: its payload. */
static void execute(struct s09_slave *s, const struct s09_req *rq,
		    const uint8_t *d, uint32_t plen)
{
	const struct s09_req r = *rq;
	const int kind = s09_kind(r.cmd);

	if (kind == S09_K_IMMEDIATE) {
		immediate(s, &r, d, plen);
	} else if (kind == S09_K_POLL) {
		poll(s);
	} else if (kind == S09_K_PUSH) {		/* S-09.033 */
		if (r.cmd == S09_PUSH0) {
			for (uint32_t i = 0; i < plen; i++) {
				if (s->rx_n == S09_SLAVE_TUNNEL) {
					s->dropped = true;	/* S-09.045 */
					continue;
				}
				s->rx[(s->rx_r + s->rx_n) % S09_SLAVE_TUNNEL] = d[i];
				s->rx_n++;
			}
		}				/* channel 1: discarded */
	} else if (kind == S09_K_PULL) {		/* S-09.034 */
		struct s09_sslot *x = &s->slot[r.slot];
		size_t len = pull_reply(s, &r, s->out);

		set_reply(s, s->out, len);
		if (len == 8) {
			x->state = S09_SS_PULL;	/* stays outstanding */
			x->req = r;
		}
	} else if (kind == S09_K_CLOCK) {
		/* S-09.036: the first session is the 12-byte
		 * header alone. */
		s->clock_req = r;
		s->clock_sample = s->ops->cycles ? s->ops->cycles(s->ops->ctx) : 0;
		s->clock_second = true;
	} else {
		struct s09_sslot *x = &s->slot[r.slot];

		x->req = r;
		x->state = S09_SS_RUN;
		if (kind == S09_K_DEFERRED) {
			deferred(s, r.slot, d, plen);
		} else {
			/* S-09.037: the payload discarded, EINVAL */
			small_reply(s, r.slot, -1, S09_EINVAL, 0, 0);
		}
	}
}

/* S-09.015: a master's init exactly, in the plain mode: whatever an
 * earlier master left (a mode, a fall-back, kept requests and replies,
 * slots and queued replies, the tunnel) is gone; n and the last cause
 * stay (S-09.079). */
static void start_again(struct s09_slave *s)
{
	const struct s09_store_ops *ops = s->ops;
	uint32_t (*check)(void *ctx, const uint8_t *d, size_t n) = s->check;
	void *check_ctx = s->check_ctx;
	void (*words)(void *ctx, uint32_t out[4]) = s->fault_words;
	void *words_ctx = s->fault_ctx;
	const uint32_t fell = s->fell, len = s->why_len, got = s->why_got;
	const uint8_t why = s->why, cmd = s->why_cmd, mode = s->why_mode;
	const bool held = s->why_held;
	uint32_t w[4];

	wb_copy((uint8_t *)w, (const uint8_t *)s->why_words, sizeof w);
	s09_slave_init(s, ops);
	s->check = check;
	s->check_ctx = check_ctx;
	s->fault_words = words;
	s->fault_ctx = words_ctx;
	s->fell = fell;
	s->why = why;
	s->why_cmd = cmd;
	s->why_mode = mode;
	s->why_len = len;
	s->why_got = got;
	s->why_held = held;
	wb_copy((uint8_t *)s->why_words, (const uint8_t *)w, sizeof w);
}

/* S-09.072: a reply to a request that came outside the plain mode ends
 * with its check word. */
static void reply_check(struct s09_slave *s)
{
	uint8_t *w = (uint8_t *)(uintptr_t)s->reply;
	const uint32_t n = (uint32_t)s->reply_len;

	wb_put_le32(w + n, s->check != NULL ? s->check(s->check_ctx, w, n) :
					      s09_check_word(w, n));
	s->reply_len += 4u;
}

void s09_slave_request(struct s09_slave *s, const uint8_t *d, size_t n)
{
	struct s09_req r;
	uint32_t plen;
	bool checked, numbered = false, is_poll;

	if (s->clock_second) {
		if (n >= 8)
			clock_second(s, d);
		return;
	}
	if (n < S09_HDR) {
		fall_back(s, S09_WHY_SHORT, NULL, n);	/* S-09.074 */
		return;
	}
	if (ignored(s, d)) {
		fall_back(s, S09_WHY_IGNORED, d, n);	/* nothing whole came */
		return;
	}
	s09_req_unpack(d, &r);
	plen = s09_payload_len(&r);
	if (S09_HDR + s09_pad4(plen) + check_len(s) > n) {
		fall_back(s, S09_WHY_SHORT, d, n);
		return;			/* a short session: nothing to serve */
	}
	checked = s->mode != S09_MODE_PLAIN;
	is_poll = r.cmd == S09_POLL;
	if (checked) {
		/* S-09.072, S-09.076: the trailer, its number then its check;
		 * on trial nothing but a test or the mode's own request;
		 * otherwise not executed (S-09.074) */
		const uint32_t body = S09_HDR + s09_pad4(plen);
		const uint32_t seq = wb_get_le32(d + body);
		const uint32_t chk = wb_get_le32(d + body + 4u);
		const uint32_t want = s->check != NULL ?
			s->check(s->check_ctx, d, body + 4u) :
			s09_check_word(d, body + 4u);
		unsigned why = chk != want ? S09_WHY_CHECK :
			       s->trial && r.cmd != S09_LINK_TEST &&
			       r.cmd != S09_LINK_MODE ? S09_WHY_ON_TRIAL :
			       (seq == S09_SEQ_NONE) != s09_unnumbered(r.cmd) ?
			       S09_WHY_NUMBER : S09_WHY_NONE;

		/* a number kept, on another request or with another check */
		if (why == S09_WHY_NONE && seq != S09_SEQ_NONE && seq == s->np_seq &&
		    (is_poll || chk != s->np_check))
			why = S09_WHY_NUMBER;
		if (why == S09_WHY_NONE && seq != S09_SEQ_NONE && seq == s->p_seq &&
		    (!is_poll || chk != s->p_check))
			why = S09_WHY_NUMBER;
		if (why != S09_WHY_NONE) {
			fall_back(s, why, d, n);
			return;
		}
		if (seq != S09_SEQ_NONE && seq == s->np_seq) {
			repeat_np(s);	/* nothing executed */
			return;
		}
		if (seq != S09_SEQ_NONE && seq == s->p_seq) {
			set_reply(s, s->p_reply, s->p_len);	/* the same reply */
			return;
		}
		if (seq != S09_SEQ_NONE) {
			const int kind = s09_kind(r.cmd);

			numbered = true;
			if (is_poll) {
				forget_poll(s);
				s->p_seq = seq;
				s->p_check = chk;
			} else {
				forget(s);
				s->np_seq = seq;
				s->np_check = chk;
				s->np_cmd = r.cmd;
				s->np_slot = r.slot;
				s->np_deferred = kind == S09_K_DEFERRED ||
						 kind == S09_K_UNDEFINED;
			}
		}
	} else if (!s09_unnumbered(r.cmd)) {
		forget(s);
		if (exact_init(d))
			start_again(s);
	}
	/* S-09.074: the recovery echo, whole, ends the fall-back's state, and
	 * a 0x19 the recovery's */
	if (s->fell_notice)
		s->recovering = true;
	s->fell_notice = false;
	if (r.cmd == S09_LINK_MODE)
		s->recovering = false;
	run_waiting(s);
	execute(s, &r, d + S09_HDR, plen);
	if (checked && s->reply_len != 0u)
		reply_check(s);
	if (numbered && is_poll) {
		s->p_reply = s->reply;		/* S-09.076: kept for a repeat */
		s->p_len = s->reply_len;
	} else if (numbered && !s->np_deferred && s->reply_len != 0u) {
		wb_copy(s->np_buf, s->reply, s->reply_len);
		s->np_len = s->reply_len;
	}
}

/* ---- the CPU link (S-09.045, S-09.046) ---- */

static size_t sp_read(void *ctx, uint8_t *buf, size_t max)
{
	struct s09_slave *s = ctx;
	size_t n = 0;

	while (n < max && s->rx_n) {
		buf[n++] = s->rx[s->rx_r];
		s->rx_r = (s->rx_r + 1u) % S09_SLAVE_TUNNEL;
		s->rx_n--;
	}
	return n;
}

/* cppcheck-suppress constParameterCallback ; s06_port_ops fixes the type */
static size_t sp_room(void *ctx)
{
	const struct s09_slave *s = ctx;

	return S09_SLAVE_TUNNEL - s->tx_n;
}

static void sp_write(void *ctx, const uint8_t *d, size_t n)
{
	struct s09_slave *s = ctx;

	wb_copy(s->tx + s->tx_n, d, n);
	s->tx_n += n;
	pull_check(s);
}

const struct s06_port_ops s09_slave_tunnel_port = { sp_read, sp_room, sp_write };

void s09_slave_cpu_poll(struct s06_node *node, unsigned link,
			const struct s09_cpu_ops *ops)
{
	static const uint8_t off[1] = { 0x14 };
	struct s06_frame f;

	s06_node_poll_link(node, link);
	while (s06_link_take(node->link[link], &f)) {
		if (f.id == 0x01) {
			/* a ping: 0x02 with the same payload */
			(void)s06_send(node, link, s06_swap_route(f.route), 0x02,
				       f.payload, f.len);
		} else if (f.id == 0x31 && f.len == 1 && f.payload[0] == 0x14) {
			(void)s06_send(node, link, 0, 0x32, off, 1);
			if (ops->power_off)
				ops->power_off(ops->ctx);	/* S-04.025 */
		} else if (f.id == 0xBD) {
			/* S-16.009: not 4 bytes: dropped, the slave keeps on */
			if (f.len == 4 && ops->clock)
				ops->clock(ops->ctx, wb_get_le32(f.payload));
		} else if (ops->common) {
			ops->common(ops->ctx, &f);	/* S-10 */
		}
	}
}
