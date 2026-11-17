/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09.009, S-13b.023: the boot stream's sessions in parts by the controller,
 * with the fall-back to one byte at a time (src/s09_fast.c), under
 * s09_boot_slave() on the published synthetic slots (slave_stream_*.json),
 * against a model of the link: a controller that clocks a byte only while
 * the ready line reads low, and a slave whose line follows each case below.
 *
 * The bytes and sessions are the default path's in every case; a part is a
 * header alone or the payload after it, each after the guard; a raise of the
 * line late after a header or a piece, within the guard, lets no byte
 * through; a hold at any point of a session is waited out; a line that never
 * falls ends the stream within T_STREAM; every fault of the controller ends
 * in the stream sent whole, the rest one byte at a time, exactly once, with
 * the controller never used again.
 */
#include "waffle/s09_fast.h"
#include "waffle/s09_master.h"
#include "waffle/s06_bytes.h"

#include "s06_test.h"
#include "vectors/s09_vectors.h"

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define GUARD 20u
#define SETTLE 2u
#define MAX_SESS 512u

static struct {
	uint32_t now;
	/* the slave's line: high from rise_at until ready_at; pending: a raise
	 * decided and not yet made */
	uint32_t rise_at, ready_at;
	bool pending, stuck;
	uint32_t late_hdr, late_piece, hold;	/* the raises after a header and a piece */
	unsigned long stuck_at, hold_at;	/* the line high for good, or for hold_us, after byte k */
	uint32_t hold_us;
	/* the slave's walk */
	uint8_t have;
	uint8_t head[16];
	uint32_t left, in_block;
	/* what came */
	unsigned long bytes, early, while_high;
	bool selected;
	unsigned n;
	size_t len[MAX_SESS];
	uint8_t sess[MAX_SESS][1100];
	/* the controller */
	unsigned parts, slow_calls, parts_after_slow, guard_short;
	uint32_t last_part_end;
	bool began;
	unsigned long fault_at;	/* the controller's fault before this byte of its own (1-based) */
	int fault;
	unsigned long ctl_bytes;
	bool slow_fail, byte_fail;
	unsigned long byte_fail_at;
	bool in_slow;
	size_t part_len[2048];
	unsigned nparts;
} m;

static struct s09_fast fast;

static uint32_t c_now(void *ctx)
{
	(void)ctx;
	return m.now;
}

static void c_idle(void *ctx, uint32_t until)
{
	(void)ctx;
	if ((int32_t)(until - m.now) > 0)
		m.now = until;
	else
		m.now++;
}

static const struct s06_clock clk = { c_now, c_idle, NULL };

static bool line(void *ctx)
{
	(void)ctx;
	if (m.stuck)
		return false;
	if (m.pending && (int32_t)(m.now - m.rise_at) >= 0) {
		m.pending = false;
	}
	return !((int32_t)(m.now - m.rise_at) >= 0 && (int32_t)(m.now - m.ready_at) < 0);
}

static void raise_after(uint32_t late, uint32_t hold)
{
	m.rise_at = m.now + late;
	m.ready_at = m.rise_at + hold;
	m.pending = late != 0u;
}

/* The slave takes a byte: a byte while its line reads high is a violation;
 * one while a raise is pending went out early. */
static void take(uint8_t b)
{
	if (!line(NULL))
		m.while_high++;
	else if (m.pending && m.bytes > 1u)
		m.early++;
	if (m.n < MAX_SESS && m.len[m.n] < sizeof m.sess[0])
		m.sess[m.n][m.len[m.n]] = b;
	if (m.n < MAX_SESS)
		m.len[m.n]++;
	m.bytes++;
	m.now++;
	if (m.stuck_at != 0u && m.bytes == m.stuck_at)
		m.stuck = true;
	if (m.bytes == 1u)
		return;		/* the opening byte */
	/* the slave's own walk of the blocks: a raise after a header, and
	 * after each piece of 1,024 payload bytes and a block's last byte */
	if (m.left != 0u) {
		m.left--;
		m.in_block++;
		if (m.left == 0u || m.in_block % 1024u == 0u)
			raise_after(m.late_piece, m.hold);
	} else {
		m.head[m.have++] = b;
		if (m.have == 16u) {
			const uint32_t code = s06_rd_le32(m.head);

			m.left = (code & 0x0100u) ? 0u : s06_rd_le32(m.head + 8);
			m.in_block = 0;
			m.have = 0;
			raise_after(m.late_hdr, m.hold);
		}
	}
	if (m.hold_at != 0u && m.bytes == m.hold_at) {
		m.rise_at = m.now;
		m.ready_at = m.now + m.hold_us;
		m.pending = false;
	}
}

static void b_select(void *ctx, bool on)
{
	(void)ctx;
	if (on && !m.selected && m.n < MAX_SESS)
		m.len[m.n] = 0;
	if (!on && m.selected)
		m.n++;
	m.selected = on;
}

static uint8_t b_xfer(void *ctx, uint8_t tx)
{
	(void)ctx;
	take(tx);
	m.now += SETTLE;
	return m.bytes == 1u ? 0xFF : 0x00;
}

/* The controller: a byte only while the line reads low, each held one for
 * at most T_STREAM; a fault of its own before its fault_at-th byte. */
static int f_part(void *ctx, const uint8_t *d, size_t n, size_t *left)
{
	(void)ctx;
	if (m.in_slow)
		m.parts_after_slow++;
	if (m.began && m.now - m.last_part_end < GUARD)
		m.guard_short++;
	if (m.nparts < ARRAY_LEN(m.part_len))
		m.part_len[m.nparts] = n;
	m.nparts++;
	for (size_t i = 0; i < n; i++) {
		const uint32_t from = m.now;

		if (m.fault != S09_PART_OK && m.ctl_bytes + 1u == m.fault_at) {
			*left = n - i;
			m.last_part_end = m.now;
			m.began = true;
			return m.fault;
		}
		while (!line(NULL)) {
			if (m.now - from >= S09_T_STREAM_US) {
				*left = n - i;
				return S09_PART_HELD;
			}
			m.now++;
		}
		take(d[i]);
		m.ctl_bytes++;
	}
	m.parts++;
	m.last_part_end = m.now;
	m.began = true;
	return S09_PART_OK;
}

static bool f_slow(void *ctx)
{
	(void)ctx;
	m.slow_calls++;
	m.in_slow = true;
	return !m.slow_fail;
}

static void f_byte(void *ctx, uint8_t tx)
{
	(void)ctx;
	take(tx);
	m.now += SETTLE;
}

static bool f_failed(void *ctx)
{
	(void)ctx;
	return m.byte_fail && m.bytes >= m.byte_fail_at;
}

static const struct s09_fast_ops fops = { NULL, line, f_part, f_slow, f_byte, f_failed };

static bool b_send(void *ctx, const uint8_t *d, size_t n)
{
	(void)ctx;
	return s09_fast_send(&fast, d, n);
}

static const struct s09_boot_ops slow_ops = { NULL, line, b_select, b_xfer, NULL, NULL };
static const struct s09_boot_ops fast_ops = { NULL, line, b_select, b_xfer, b_send, NULL };

static void reset(void)
{
	memset(&m, 0, sizeof m);
	m.rise_at = m.ready_at = 0;
	s09_fast_init(&fast, &fops, &clk, GUARD);
}

/* The vector's sessions, as the default path sends them. */
static void same_sessions(const struct s09v_stream *v)
{
	CHECK_EQ(m.n, v->sn);
	for (size_t s = 0; s < m.n && s < v->sn; s++) {
		const struct s09v_sess *e = &s09v_sessions[v->s0 + s];

		if (e->at < 0)
			CHECK(m.len[s] == 1 && m.sess[s][0] == 0x03);
		else
			CHECK_MEM(m.sess[s], m.len[s], v->slot + e->at, e->len);
	}
}

static unsigned long slot_bytes(const struct s09v_stream *v)
{
	unsigned long n = 0;

	for (size_t s = 1; s < v->sn; s++)
		n += s09v_sessions[v->s0 + s].len;
	return n;
}

/* The parts: each a header alone or a payload of 1 to 1,024 bytes. */
static void parts_shape(void)
{
	for (unsigned i = 0; i < m.nparts && i < ARRAY_LEN(m.part_len); i++)
		CHECK(m.part_len[i] >= 1u && m.part_len[i] <= 1024u);
}

static void test_streams(void)
{
	for (size_t i = 0; i < s09v_streams_count; i++) {
		const struct s09v_stream *v = &s09v_streams[i];
		unsigned headers = 0;
		int end = -1;

		t_case = v->name;
		reset();
		CHECK_EQ(s09_boot_slave(&fast_ops, &clk, v->slot, v->len, true, &end), S09_BOOT_STREAMED);
		CHECK_EQ(end, v->end);
		same_sessions(v);
		parts_shape();
		for (unsigned k = 0; k < m.nparts; k++)
			headers += m.part_len[k] == 16u ? 1u : 0u;
		CHECK(headers >= 1u);
		CHECK_EQ(m.guard_short, 0);
		CHECK_EQ(m.while_high + m.early, 0);
		CHECK_EQ(fast.st.sessions + 1u, v->sn);
		CHECK_EQ(fast.st.parts, m.nparts);
		CHECK_EQ(fast.st.fast_bytes, slot_bytes(v));
		CHECK_EQ(fast.st.slow_bytes + fast.st.fell + m.slow_calls, 0);
		CHECK(!fast.st.held);
		/* the guards: one before every part but the first */
		CHECK(fast.st.guard_us >= (m.nparts - 1u) * GUARD);
	}
}

/*
 * A slave that raises its line late after each header and each piece. Within
 * the guard the parts never meet it; past the guard, bytes go out that the
 * slave meant to hold off (what the guard's length has to cover, S-13b.023).
 * The default path's 2 us settle does the same for raises within 2 us.
 */
static void test_late_raise(void)
{
	static const uint32_t lates[] = { 1u, 5u, 10u, 19u };
	const struct s09v_stream *v = &s09v_streams[0];
	int end;

	for (size_t i = 0; i < ARRAY_LEN(lates); i++) {
		t_case = "late_within_guard";
		reset();
		m.late_hdr = m.late_piece = lates[i];
		m.hold = 50u;
		CHECK_EQ(s09_boot_slave(&fast_ops, &clk, v->slot, v->len, true, &end), S09_BOOT_STREAMED);
		same_sessions(v);
		CHECK_EQ(m.while_high + m.early, 0);
		CHECK(fast.st.high > 0u);	/* a guard ended with the line high */
	}
	t_case = "late_past_guard";
	reset();
	m.late_hdr = m.late_piece = GUARD + 10u;
	m.hold = 50u;
	CHECK_EQ(s09_boot_slave(&fast_ops, &clk, v->slot, v->len, true, &end), S09_BOOT_STREAMED);
	CHECK_EQ(m.while_high, 0);
	CHECK(m.early > 0u);
	/* the default path, against the same slave, for comparison */
	t_case = "late_default_path";
	reset();
	m.late_hdr = m.late_piece = 5u;
	m.hold = 50u;
	CHECK_EQ(s09_boot_slave(&slow_ops, &clk, v->slot, v->len, true, &end), S09_BOOT_STREAMED);
	CHECK(m.early > 0u);	/* 5 us is past the settle time */
}

/* The line held at each point of a session, for a short time and for nearly
 * T_STREAM: before a part's first byte, inside a part, after a part's last
 * byte. Waited out; every byte sent once, none while the line reads high. */
static void test_holds(void)
{
	const struct s09v_stream *v = &s09v_streams[0];
	static const uint32_t holds[] = { 3u, 150u, 900000u, S09_T_STREAM_US - 1000u };
	static const unsigned long at[] = { 2u, 10u, 17u, 18u, 300u, 1040u, 1041u };
	int end;

	for (size_t h = 0; h < ARRAY_LEN(holds); h++)
		for (size_t a = 0; a < ARRAY_LEN(at); a++) {
			t_case = "hold";
			reset();
			m.hold_at = at[a];
			m.hold_us = holds[h];
			if (m.hold_at >= slot_bytes(v))
				continue;
			CHECK_EQ(s09_boot_slave(&fast_ops, &clk, v->slot, v->len, true, &end),
				 S09_BOOT_STREAMED);
			same_sessions(v);
			CHECK_EQ(m.while_high, 0);
			CHECK_EQ(fast.st.fell, S09_PART_OK);
		}
}

/* A line that never falls again, after the opening, in a header, in a
 * payload, at a session's last byte: the stream fails within T_STREAM, no
 * byte after the hold, the select released, no fall-back. */
static void test_never_falls(void)
{
	const struct s09v_stream *v = &s09v_streams[0];
	static const unsigned long at[] = { 1u, 2u, 16u, 17u, 500u, 1041u };
	int end;

	for (size_t a = 0; a < ARRAY_LEN(at); a++) {
		uint32_t t0;

		t_case = "never_falls";
		reset();
		m.stuck_at = at[a];
		t0 = m.now;
		CHECK_EQ(s09_boot_slave(&fast_ops, &clk, v->slot, v->len, true, &end), S09_BOOT_STUCK);
		CHECK_EQ(m.bytes, at[a]);
		CHECK(!m.selected);
		CHECK(m.now - t0 <= S09_T_STREAM_US + 5000u);
		CHECK_EQ(fast.st.fell, S09_PART_OK);
		CHECK(at[a] == 1u || fast.st.held);
		CHECK_EQ(m.slow_calls, 0);
	}
}

/* Every fault of the controller's own, before a part's first byte, inside a
 * header, inside a payload, at a part's last byte and at the stream's last:
 * the stream ends whole and the same, every byte exactly once, the rest one
 * byte at a time, the controller never given a part again (no second try). */
static void test_fall_back(void)
{
	static const int causes[] = { S09_PART_DMA, S09_PART_STALL, S09_PART_NO_END, S09_PART_REFUSED };

	for (size_t i = 0; i < s09v_streams_count; i++) {
		const struct s09v_stream *v = &s09v_streams[i];
		const unsigned long total = slot_bytes(v);
		const unsigned long at[] = { 1u, 5u, 16u, 17u, 300u, 1040u, total };

		for (size_t c = 0; c < ARRAY_LEN(causes); c++)
			for (size_t a = 0; a < ARRAY_LEN(at); a++) {
				int end;

				if (at[a] > total)
					continue;
				t_case = "fall_back";
				reset();
				m.fault = causes[c];
				m.fault_at = at[a];
				CHECK_EQ(s09_boot_slave(&fast_ops, &clk, v->slot, v->len, true, &end),
					 S09_BOOT_STREAMED);
				CHECK_EQ(end, v->end);
				same_sessions(v);
				CHECK_EQ(m.bytes, 1u + total);
				CHECK_EQ(m.while_high, 0);
				CHECK_EQ(m.slow_calls, 1);
				CHECK_EQ(m.parts_after_slow, 0);
				CHECK_EQ(fast.st.fell, causes[c]);
				CHECK_EQ(fast.st.fell_at, at[a] - 1u);
				CHECK_EQ(fast.st.fast_bytes, at[a] - 1u);
				CHECK_EQ(fast.st.slow_bytes, total - (at[a] - 1u));
				CHECK(fast.st.fell_session >= 1u && fast.st.fell_session < v->sn);
			}
	}
}

/* After a fall-back: the slave holding the line for good ends the stream as
 * the default path does; a link that cannot go to single bytes, or whose
 * byte driver fails, ends it at once. */
static void test_fall_back_fails(void)
{
	const struct s09v_stream *v = &s09v_streams[0];
	int end;

	t_case = "fall_back_then_held";
	reset();
	m.fault = S09_PART_DMA;
	m.fault_at = 20u;
	m.stuck_at = 40u;
	CHECK_EQ(s09_boot_slave(&fast_ops, &clk, v->slot, v->len, true, &end), S09_BOOT_STUCK);
	CHECK_EQ(m.bytes, 40u);
	CHECK(fast.st.held);
	CHECK(!m.selected);

	t_case = "fall_back_refused";
	reset();
	m.fault = S09_PART_STALL;
	m.fault_at = 20u;
	m.slow_fail = true;
	CHECK_EQ(s09_boot_slave(&fast_ops, &clk, v->slot, v->len, true, &end), S09_BOOT_STUCK);
	CHECK_EQ(m.bytes, 20u);
	CHECK_EQ(m.parts_after_slow, 0);

	t_case = "fall_back_byte_dead";
	reset();
	m.fault = S09_PART_STALL;
	m.fault_at = 20u;
	m.byte_fail = true;
	m.byte_fail_at = 30u;
	CHECK_EQ(s09_boot_slave(&fast_ops, &clk, v->slot, v->len, true, &end), S09_BOOT_STUCK);
	CHECK_EQ(m.bytes, 30u);

	/* a hold of the controller's part is the slave's: no fall-back */
	t_case = "held_no_fall_back";
	reset();
	m.stuck_at = 100u;
	CHECK_EQ(s09_boot_slave(&fast_ops, &clk, v->slot, v->len, true, &end), S09_BOOT_STUCK);
	CHECK_EQ(m.slow_calls, 0);
	CHECK(fast.st.held);
}

/* The opening and S-09.007's bytes, a bad CRC and a slave never ready are
 * the default path's: no part. */
static void test_opening(void)
{
	const struct s09v_stream *v = &s09v_streams[0];
	int end;

	t_case = "bad_crc";
	reset();
	CHECK_EQ(s09_boot_slave(&fast_ops, &clk, v->slot, v->len, false, &end), S09_BOOT_BAD_CRC);
	CHECK_EQ(m.bytes + m.nparts, 0);
	t_case = "no_ready";
	reset();
	m.stuck = true;
	CHECK_EQ(s09_boot_slave(&fast_ops, &clk, v->slot, v->len, true, &end), S09_BOOT_NOT_READY);
	CHECK_EQ(m.bytes + m.nparts, 0);
}

int main(void)
{
	test_streams();
	test_late_raise();
	test_holds();
	test_never_falls();
	test_fall_back();
	test_fall_back_fails();
	test_opening();
	return t_finish("test_s09_fast");
}
