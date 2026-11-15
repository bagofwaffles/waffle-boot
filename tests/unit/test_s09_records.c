/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09 records and the boot stream: S-09.012, S-09.020, S-09.021, S-09.023
 * (requests.json: the master's session bytes, the slave's request length),
 * S-09.042 (errno.json), S-09.043 (records.json), S-09.001-S-09.007
 * (slave_stream_*.json against the published slot images).
 */
#include "waffle/s09_master.h"
#include "waffle/s09_slave.h"
#include "waffle/s06_bytes.h"

#include "s06_test.h"
#include "s06_fake.h"
#include "vectors/s09_vectors.h"

static uint8_t sess[70000];

static void test_requests(void)
{
	static struct s09_slave sl;

	s09_slave_init(&sl, 0);
	sl.linked = true;
	for (size_t i = 0; i < s09v_requests_count; i++) {
		const struct s09v_req *v = &s09v_requests[i];
		struct s09_req r;
		uint32_t n, wire;

		t_case = v->name;
		s09_req_unpack(v->session, &r);
		s09_req_pack(&r, sess);
		CHECK_MEM(sess, 12, v->session, 12);
		/* the master's session: header, payload, zero pad (S-09.021) */
		n = s09_payload_len(&r);
		wire = s09_master_payload_wire(n);
		memcpy(sess + 12, v->session + 12, n);
		memset(sess + 12 + n, 0, wire - n);
		CHECK_MEM(sess, 12 + wire, v->session, v->len);
		/* the slave's request length */
		CHECK_EQ(s09_slave_rx_len(&sl, v->session), v->slave_rx_len);
	}
}

static void test_records(void)
{
	uint8_t b[S09_DIRENT_LEN];

	t_case = "records.json";
	for (size_t i = 0; i < s09v_records_count; i++) {
		const struct s09v_record *v = &s09v_records[i];

		if (v->kind == 0) {
			struct s09_stat s = { v->v[0], v->v[1], v->v[2], v->v[3],
					      v->v[4], v->v[5], v->v[6], v->v[7],
					      v->v[8], v->v[9], v->v[10], v->v[11],
					      v->v[12] }, u;

			s09_stat_pack(&s, b);
			CHECK_MEM(b, S09_STAT_LEN, v->b, v->len);
			s09_stat_unpack(v->b, &u);
			CHECK(!memcmp(&s, &u, sizeof s));
		} else if (v->kind == 1) {
			struct s09_statfs s = { v->v[0], v->v[1], v->v[2] }, u;

			s09_statfs_pack(&s, b);
			CHECK_MEM(b, S09_STATFS_LEN, v->b, v->len);
			s09_statfs_unpack(v->b, &u);
			CHECK_EQ(u.bfree, s.bfree);
		} else {
			struct s09_dirent d, u;

			memset(&d, 0, sizeof d);
			d.ino = v->v[0];
			d.off = v->v[1];
			d.type = (uint8_t)v->v[2];
			strcpy(d.name, v->name);
			s09_dirent_pack(&d, b);
			CHECK_MEM(b, S09_DIRENT_LEN, v->b, v->len);
			s09_dirent_unpack(v->b, &u);
			CHECK(!strcmp(u.name, v->name));
			CHECK_EQ(u.type, d.type);
		}
	}
}

static void test_errno(void)
{
	static const struct {
		const char *n;
		uint32_t v;
	} mine[] = {
		{ "EACCES", S09_EACCES }, { "EBADF", S09_EBADF }, { "EBUSY", S09_EBUSY },
		{ "EEXIST", S09_EEXIST }, { "EFAULT", S09_EFAULT }, { "EINVAL", S09_EINVAL },
		{ "EISDIR", S09_EISDIR }, { "ELOOP", S09_ELOOP },
		{ "ENAMETOOLONG", S09_ENAMETOOLONG }, { "ENFILE", S09_ENFILE },
		{ "ENODATA", S09_ENODATA }, { "ENODEV", S09_ENODEV }, { "ENOENT", S09_ENOENT },
		{ "ENOMEM", S09_ENOMEM }, { "ENOSPC", S09_ENOSPC }, { "ENOTDIR", S09_ENOTDIR },
		{ "ENOTEMPTY", S09_ENOTEMPTY }, { "ERANGE", S09_ERANGE }, { "EROFS", S09_EROFS },
		{ "EXDEV", S09_EXDEV },
	};

	t_case = "errno.json";
	CHECK_EQ(s09v_errno_count, sizeof mine / sizeof mine[0]);
	for (size_t i = 0; i < s09v_errno_count; i++) {
		CHECK(!strcmp(s09v_errno[i].name, mine[i].n));
		CHECK_EQ(s09v_errno[i].v, mine[i].v);
	}
}

/* ---- the boot stream ---- */

struct fake_boot {
	/* 0 always ready, 1 never, 2 ready until byte 100, 3 no_open_stuck,
	 * 4 slow_block: high for hold_us once hold_at bytes are sent */
	int ready_mode;
	const struct s06_clock *clock;
	size_t hold_at;
	uint32_t hold_us, hold_from;
	bool held;
	uint8_t answer;		/* returned for the opening byte */
	bool selected;
	size_t bytes;
	/* PA6 high after each byte: for `after` samples of the line, or, when
	 * after_us is set, for that long */
	unsigned after, polls;
	uint32_t after_us, after_from;
	size_t started_high;	/* bytes started while PA6 read high */
	size_t unselected;	/* bytes outside a chip-select session */
	size_t samples;		/* every sample of the line */
	/* BOOT_LINK_FC: the sessions handed over whole, and the one that
	 * the controller gives up (0: none) */
	size_t sends, send_fails_at;
	/* the driver stops moving bytes from this one on (1-based; 0: never) */
	size_t dead_at;
	bool dead;
	size_t after_dead;	/* bytes asked for after the first that failed */
	uint8_t sess[64][1100];
	size_t len[64];
	size_t n;
};

static bool fb_line(struct fake_boot *f);

/* The slave still holds PA6 high after the last byte. */
static bool fb_held(const struct fake_boot *f)
{
	if (f->bytes == 0)
		return false;
	if (f->after != 0 && f->polls < f->after)
		return true;
	return f->after_us != 0 && s06_now(f->clock) - f->after_from < f->after_us;
}

/* cppcheck-suppress constParameterCallback ; s09_boot_ops fixes the type */
static bool fb_ready(void *ctx)
{
	struct fake_boot *f = ctx;

	f->samples++;
	if (fb_held(f)) {
		f->polls++;
		return false;
	}
	return fb_line(f);
}

static bool fb_line(struct fake_boot *f)
{
	if (f->ready_mode == 4 && f->bytes == f->hold_at) {
		uint32_t t = s06_now(f->clock);

		if (!f->held) {
			f->held = true;
			f->hold_from = t;
		}
		if (t - f->hold_from < f->hold_us)
			return false;
	}
	if (f->ready_mode == 1)
		return false;
	if (f->ready_mode == 2 && f->bytes >= 100)
		return false;
	if (f->ready_mode == 3)		/* no_open_stuck: low, never rises */
		return true;
	/* after the eleven 0F the slave raises PA6 */
	return !(f->n == 1 && f->len[0] == 12);
}

static void fb_select(void *ctx, bool on)
{
	struct fake_boot *f = ctx;

	if (on && !f->selected)
		f->len[f->n] = 0;
	if (!on && f->selected)
		f->n++;
	f->selected = on;
}

static uint8_t fb_xfer(void *ctx, uint8_t tx)
{
	struct fake_boot *f = ctx;

	/* S-13b.022: the line as it is when the byte starts (the eleven 0F
	 * of S-09.007 are sent whatever it reads) */
	if (tx != 0x0F || f->answer == 0xFF) {
		if (fb_held(f) || !fb_line(f))
			f->started_high++;
	}
	if (!f->selected)
		f->unselected++;
	if (f->dead)
		f->after_dead++;
	if (f->dead_at != 0 && f->bytes + 1u >= f->dead_at) {
		f->dead = true;
		f->bytes++;
		return 0xFF;		/* as a driver's timed-out byte reads */
	}
	f->polls = 0;
	if (f->clock != NULL)
		f->after_from = s06_now(f->clock);
	if (f->len[f->n] < sizeof f->sess[0])
		f->sess[f->n][f->len[f->n]] = tx;
	f->len[f->n]++;
	f->bytes++;
	return f->bytes == 1 ? f->answer : 0;
}

/* cppcheck-suppress constParameterCallback ; s09_boot_ops fixes the type */
static bool fb_failed(void *ctx)
{
	const struct fake_boot *f = ctx;

	return f->dead;
}

/* BOOT_LINK_FC: a stream session's bytes, moved by the controller. */
static bool fb_send(void *ctx, const uint8_t *d, size_t n)
{
	struct fake_boot *f = ctx;

	if (!f->selected)
		f->unselected++;
	f->sends++;
	if (f->sends == f->send_fails_at)
		return false;
	for (size_t i = 0; i < n; i++) {
		if (f->len[f->n] < sizeof f->sess[0])
			f->sess[f->n][f->len[f->n]] = d[i];
		f->len[f->n]++;
		f->bytes++;
	}
	return true;
}

/* The offset just past the first FILL block's header (0: none). */
static size_t first_fill_end(const uint8_t *slot, size_t len)
{
	size_t off = 0;

	while (len - off >= 16) {
		uint32_t code = s06_rd_le32(slot + off);
		uint32_t count = s06_rd_le32(slot + off + 8);

		if (code & 0x0100u)
			return off + 16;
		if (code & 0x8000u)
			break;
		off += 16u + count;
	}
	return 0;
}

static void test_stream(void)
{
	static struct fake_boot fb;
	static struct t_sim sim;
	struct s09_boot_ops ops = { &fb, fb_ready, fb_select, fb_xfer, NULL, fb_failed };

	for (size_t i = 0; i < s09v_streams_count; i++) {
		const struct s09v_stream *v = &s09v_streams[i];
		int end, rc;

		t_case = v->name;
		memset(&fb, 0, sizeof fb);
		fb.answer = 0xFF;
		t_sim_init(&sim, S06_MS(60000));
		rc = s09_boot_slave(&ops, &sim.clock, v->slot, v->len, true, &end);
		CHECK_EQ(rc, S09_BOOT_STREAMED);
		CHECK_EQ(end, v->end);
		CHECK_EQ(fb.n, v->sn);
		for (size_t s = 0; s < fb.n && s < v->sn; s++) {
			const struct s09v_sess *e = &s09v_sessions[v->s0 + s];

			if (e->at < 0)
				CHECK(fb.len[s] == 1 && fb.sess[s][0] == 0x03);
			else
				CHECK_MEM(fb.sess[s], fb.len[s], v->slot + e->at, e->len);
		}

		/* S-09.007: the no-open session, then nothing; absent */
		memset(&fb, 0, sizeof fb);
		fb.answer = 0x00;
		rc = s09_boot_slave(&ops, &sim.clock, v->slot, v->len, true, &end);
		CHECK_EQ(rc, S09_BOOT_NO_OPEN);
		CHECK_EQ(fb.n, 1);
		CHECK_MEM(fb.sess[0], fb.len[0], v->no_open, v->no_open_len);
	}

	t_case = "bad_crc";			/* S-09.002 */
	memset(&fb, 0, sizeof fb);
	{
		int end;

		CHECK_EQ(s09_boot_slave(&ops, &sim.clock, s09v_streams[0].slot,
					s09v_streams[0].len, false, &end),
			 S09_BOOT_BAD_CRC);
		CHECK_EQ(fb.bytes, 0);

		t_case = "no_ready";		/* S-09.003 */
		fb.ready_mode = 1;
		sim.now = 0;
		CHECK_EQ(s09_boot_slave(&ops, &sim.clock, s09v_streams[0].slot,
					s09v_streams[0].len, true, &end),
			 S09_BOOT_NOT_READY);
		CHECK_EQ(fb.bytes, 0);
		CHECK(sim.now >= S06_MS(200) && sim.now <= S06_MS(1000) + 200);

		t_case = "no_open_stuck";	/* PA6 never rises: released in time */
		memset(&fb, 0, sizeof fb);
		fb.ready_mode = 3;
		sim.now = 0;
		CHECK_EQ(s09_boot_slave(&ops, &sim.clock, s09v_streams[0].slot,
					s09v_streams[0].len, true, &end),
			 S09_BOOT_NO_OPEN);
		CHECK(!fb.selected);
		CHECK(sim.now <= S06_MS(1000) + 200);

		t_case = "stream_stuck";	/* PA6 high after the 100th byte */
		memset(&fb, 0, sizeof fb);
		fb.answer = 0xFF;
		fb.ready_mode = 2;
		sim.now = 0;
		CHECK_EQ(s09_boot_slave(&ops, &sim.clock, s09v_streams[0].slot,
					s09v_streams[0].len, true, &end),
			 S09_BOOT_STUCK);
		CHECK(!fb.selected);
		CHECK(sim.now >= S09_T_STREAM_US);	/* absent within T_STREAM */
		CHECK(sim.now <= S09_T_STREAM_US + S06_MS(50));

		/* B-STREAM slow_block: PA6 high for 900 ms after the first
		 * FILL block's header; the stream goes on (T_STREAM's floor) */
		t_case = "slow_block";
		memset(&fb, 0, sizeof fb);
		fb.answer = 0xFF;
		fb.ready_mode = 4;
		fb.clock = &sim.clock;
		fb.hold_at = 1 + first_fill_end(s09v_streams[0].slot,
						s09v_streams[0].len);
		fb.hold_us = S06_MS(900);
		CHECK(fb.hold_at > 1);
		sim.now = 0;
		CHECK_EQ(s09_boot_slave(&ops, &sim.clock, s09v_streams[0].slot,
					s09v_streams[0].len, true, &end),
			 S09_BOOT_STREAMED);
		CHECK_EQ(end, S09_END_FINAL);
		CHECK(fb.held);
		CHECK_EQ(fb.n, s09v_streams[0].sn);
		CHECK(sim.now >= S06_MS(900));
	}

	/* S-09.005, S-13b.022: a slave that raises PA6 after every byte. No
	 * byte starts while it reads high; the bytes and sessions are the
	 * same; a hold of a few samples costs no clock read at all, and a
	 * longer one is waited out on the clock. */
	for (unsigned mode = 0; mode < 3u; mode++) {
		const struct s09v_stream *v = &s09v_streams[0];
		size_t total = 0;
		int end;

		t_case = mode == 0 ? "pace_none" : mode == 1 ? "pace_short" : "pace_long";
		memset(&fb, 0, sizeof fb);
		fb.answer = 0xFF;
		fb.clock = &sim.clock;
		fb.after = mode == 1 ? 40u : 0u;
		fb.after_us = mode == 2 ? 250u : 0u;
		sim.now = 0;
		t_sim_init(&sim, S06_MS(600000));
		CHECK_EQ(s09_boot_slave(&ops, &sim.clock, v->slot, v->len, true, &end),
			 S09_BOOT_STREAMED);
		CHECK_EQ(end, v->end);
		CHECK_EQ(fb.started_high, 0);
		CHECK_EQ(fb.unselected, 0);
		CHECK_EQ(fb.n, v->sn);
		for (size_t k = 0; k < fb.n && k < v->sn; k++) {
			const struct s09v_sess *e = &s09v_sessions[v->s0 + k];

			total += fb.len[k];
			if (e->at >= 0)
				CHECK_MEM(fb.sess[k], fb.len[k], v->slot + e->at, e->len);
		}
		CHECK_EQ(fb.bytes, total);
		if (mode == 0) {
			/* one sample a byte, and one before the opening byte */
			CHECK_EQ(fb.samples, fb.bytes);
			CHECK_EQ(sim.now, 0);
		} else if (mode == 1) {
			CHECK_EQ(fb.samples, 1u + (fb.bytes - 1u) * 41u);
			CHECK_EQ(sim.now, 0);		/* the clock never asked */
		} else {
			CHECK(sim.now >= (fb.bytes - 1u) * 250u);
		}
	}

	/* BOOT_LINK_FC: the stream's sessions go to send(), whole and the
	 * same; the opening byte and S-09.007's still go byte by byte; a
	 * session the controller gives up ends the stream (S-09.060). */
	{
		struct s09_boot_ops fc = ops;
		const struct s09v_stream *v;
		int end;

		fc.send = fb_send;
		t_case = "fc_stream";
		for (size_t i = 0; i < s09v_streams_count; i++) {
			v = &s09v_streams[i];
			memset(&fb, 0, sizeof fb);
			fb.answer = 0xFF;
			sim.now = 0;
			CHECK_EQ(s09_boot_slave(&fc, &sim.clock, v->slot, v->len, true, &end),
				 S09_BOOT_STREAMED);
			CHECK_EQ(end, v->end);
			CHECK_EQ(fb.n, v->sn);
			CHECK_EQ(fb.sends + 1u, fb.n);		/* all but the opening session */
			CHECK_EQ(fb.unselected, 0);
			for (size_t k = 0; k < fb.n && k < v->sn; k++) {
				const struct s09v_sess *e = &s09v_sessions[v->s0 + k];

				if (e->at < 0)
					CHECK(fb.len[k] == 1 && fb.sess[k][0] == 0x03);
				else
					CHECK_MEM(fb.sess[k], fb.len[k], v->slot + e->at, e->len);
			}
		}
		v = &s09v_streams[0];
		t_case = "fc_no_open";
		memset(&fb, 0, sizeof fb);
		fb.answer = 0x00;
		CHECK_EQ(s09_boot_slave(&fc, &sim.clock, v->slot, v->len, true, &end),
			 S09_BOOT_NO_OPEN);
		CHECK_EQ(fb.sends, 0);
		CHECK_EQ(fb.n, 1);
		CHECK_MEM(fb.sess[0], fb.len[0], v->no_open, v->no_open_len);
		t_case = "fc_bad_crc";
		memset(&fb, 0, sizeof fb);
		CHECK_EQ(s09_boot_slave(&fc, &sim.clock, v->slot, v->len, false, &end),
			 S09_BOOT_BAD_CRC);
		CHECK_EQ(fb.bytes + fb.sends, 0);
		t_case = "fc_no_ready";
		fb.ready_mode = 1;
		sim.now = 0;
		CHECK_EQ(s09_boot_slave(&fc, &sim.clock, v->slot, v->len, true, &end),
			 S09_BOOT_NOT_READY);
		CHECK_EQ(fb.bytes + fb.sends, 0);
		t_case = "fc_stuck";
		memset(&fb, 0, sizeof fb);
		fb.answer = 0xFF;
		fb.send_fails_at = 2;
		CHECK_EQ(s09_boot_slave(&fc, &sim.clock, v->slot, v->len, true, &end),
			 S09_BOOT_STUCK);
		CHECK(!fb.selected);
		CHECK_EQ(fb.sends, 2);			/* nothing after it */
	}

	/* A driver that stops moving bytes: the stream ends at the first such
	 * byte, the slave absent (S-09.008), the select released, and no time
	 * spent on the bytes that are left: at the opening byte, whose 0xFF is
	 * no answer; mid-stream; and among S-09.007's bytes. */
	{
		const struct s09v_stream *v = &s09v_streams[0];
		static const size_t at[] = { 1, 2, 17, 500 };
		int end;

		t_case = "driver_dead";
		for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
			memset(&fb, 0, sizeof fb);
			fb.answer = 0xFF;
			fb.dead_at = at[i];
			sim.now = 0;
			CHECK_EQ(s09_boot_slave(&ops, &sim.clock, v->slot, v->len, true, &end),
				 S09_BOOT_STUCK);
			CHECK_EQ(fb.bytes, at[i]);
			CHECK_EQ(fb.after_dead, 0);
			CHECK(!fb.selected);
			CHECK(sim.now <= S06_MS(1));
		}
		t_case = "driver_dead_no_open";
		memset(&fb, 0, sizeof fb);
		fb.answer = 0x00;
		fb.dead_at = 5;				/* the fourth 0F */
		sim.now = 0;
		CHECK_EQ(s09_boot_slave(&ops, &sim.clock, v->slot, v->len, true, &end),
			 S09_BOOT_NO_OPEN);
		CHECK_EQ(fb.bytes, 5);
		CHECK_EQ(fb.after_dead, 0);
		CHECK(!fb.selected);
		CHECK(sim.now <= S06_MS(1));		/* no wait for PA6 either */
	}

	/* S-09.006: a count that wraps a 32-bit end is past the slot's end;
	 * the block is not sent. Room for a first piece, should one go. */
	t_case = "count_wraps";
	{
		static const uint32_t counts[] = { 0xFFFFFFF0u, 0xFFFFFFFFu };
		static uint8_t wrap[16 + 1024];

		for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
			int end = -1;

			memset(wrap, 0, sizeof wrap);
			s06_wr_le32(wrap, 0xAD008000u);	/* FINAL, not FILL */
			s06_wr_le32(wrap + 8, counts[i]);
			memset(&fb, 0, sizeof fb);
			fb.answer = 0xFF;
			sim.now = 0;
			CHECK_EQ(s09_boot_slave(&ops, &sim.clock, wrap, sizeof wrap,
						true, &end),
				 S09_BOOT_STREAMED);
			CHECK_EQ(end, S09_END_SLOT);
			CHECK_EQ(fb.n, 1);		/* the open session only */
		}
	}
}

int main(void)
{
	test_requests();
	test_records();
	test_errno();
	test_stream();
	return t_finish("test_s09_records");
}
