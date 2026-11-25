/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09.009, S-13b.023: the slave boot stream's sessions by the controller,
 * in parts, with the fall-back to one byte at a time (see waffle/bridge/fast.h).
 */
#include "waffle/bridge/fast.h"

#include <string.h>

#include "waffle/link/bytes_compat.h"
#include "waffle/bridge/master.h"

#if OPXY_BOOT_LINK_FC

#define LDR_FILL 0x0100u
#define HEADER 16u
#define LINE_STEP_US 100u	/* how often a line held high is looked at */
/* As the byte path's: a line held for a few samples costs no clock read. */
#define LINE_SPIN 256u

void s09_fast_init(struct s09_fast *f, const struct s09_fast_ops *ops,
		   const struct s06_clock *clk, uint32_t guard_us)
{
	memset(f, 0, sizeof *f);
	f->ops = ops;
	f->clk = clk;
	f->guard_us = guard_us;
}

static uint32_t now(const struct s09_fast *f)
{
	return s06_now(f->clk);
}

void s09_fast_pause(struct s09_fast *f)
{
	f->last_end = now(f);
	f->began = true;
}

/* S-09.005, S-09.060: the line low before a part or a byte, within
 * T_STREAM. `part`: a part's, whose first sample finding the line high is
 * counted. */
static bool line_low(struct s09_fast *f, bool part)
{
	const struct s09_fast_ops *o = f->ops;
	uint32_t from, end;
	unsigned spin;

	if (o->ready(o->ctx))
		return true;
	if (part)
		f->st.high++;
	for (spin = 0; spin < LINE_SPIN; spin++)
		if (o->ready(o->ctx))
			return true;
	from = now(f);
	end = from + S09_T_STREAM_US;
	for (;;) {
		const uint32_t t = now(f);
		const uint32_t waited = t - from;

		if (o->ready(o->ctx) || s06_time_reached(t, end)) {
			f->st.wait_us += waited;
			if (waited > f->st.wait_max_us)
				f->st.wait_max_us = waited;
			if (o->ready(o->ctx))
				return true;
			f->st.held = true;
			return false;
		}
		s06_idle(f->clk, t + LINE_STEP_US);
	}
}

/* S-13b.023: at least guard_us from the last part's end. A clock reading
 * stands for the microsecond after it, hence the one more. */
static void guard(struct s09_fast *f)
{
	const uint32_t until = f->last_end + f->guard_us + 1u;
	const uint32_t from = now(f);
	uint32_t t = from;

	if (!f->began)
		return;
	while (!s06_time_reached(t, until)) {
		s06_idle(f->clk, until);
		t = now(f);
	}
	f->st.guard_us += t - from;
}

void s09_fast_session_guard(struct s09_fast *f)
{
	if (!f->slow)
		guard(f);
}

/* The next part of n bytes: what is left of a header, or of a payload. */
static size_t part_len(const struct s09_fast *f, size_t n)
{
	const size_t k = f->left != 0u ? f->left : HEADER - f->have;

	return n < k ? n : k;
}

/* S-09.005: the walk of the stream's blocks over bytes sent, a header then
 * its count of payload bytes, none for a fill or a count of 0. */
static void walk(struct s09_fast *f, const uint8_t *d, size_t n)
{
	while (n != 0u) {
		if (f->left != 0u) {
			const size_t k = n < f->left ? n : f->left;

			f->left -= (uint32_t)k;
			d += k;
			n -= k;
			continue;
		}
		f->head[f->have++] = *d++;
		n--;
		if (f->have == HEADER) {
			const uint32_t code = s06_rd_le32(f->head);
			const uint32_t count = s06_rd_le32(f->head + 8);

			f->left = (code & LDR_FILL) != 0u ? 0u : count;
			f->have = 0;
		}
	}
}

/* After a fall-back: the byte path's pacing, the line sampled before each
 * byte and the settle time after it. */
static bool slow_rest(struct s09_fast *f, const uint8_t *d, size_t n)
{
	const struct s09_fast_ops *o = f->ops;
	size_t i;

	for (i = 0; i < n; i++) {
		if (!line_low(f, false))
			return false;
		o->byte(o->ctx, d[i]);
		if (o->failed != NULL && o->failed(o->ctx))
			return false;
		f->st.slow_bytes++;
	}
	return true;
}

bool s09_fast_send(struct s09_fast *f, const uint8_t *d, size_t n)
{
	const struct s09_fast_ops *o = f->ops;

	f->st.sessions++;
	while (n != 0u) {
		size_t k, left = 0, done;
		int rc;

		if (f->slow)
			return slow_rest(f, d, n);
		k = part_len(f, n);
		guard(f);
		if (!line_low(f, true))
			return false;
		rc = o->part(o->ctx, d, k, &left);
		f->last_end = now(f);
		f->began = true;
		if (rc == S09_PART_OK)
			left = 0;
		else if (left > k)
			left = k;
		done = k - left;
		f->st.fast_bytes += (uint32_t)done;
		walk(f, d, done);
		d += done;
		n -= done;
		if (rc == S09_PART_OK) {
			f->st.parts++;
			continue;
		}
		if (rc == S09_PART_HELD) {
			f->st.held = true;
			return false;
		}
		/* S-09.009: the controller's own failure; once, the rest by byte */
		f->st.fell = (uint8_t)(rc > S09_PART_HELD && rc < S09_PART_CAUSES ?
					       rc : S09_PART_REFUSED);
		f->st.fell_session = f->st.sessions;
		f->st.fell_at = f->st.fast_bytes;
		if (!o->slow(o->ctx))
			return false;
		f->slow = true;
		guard(f);
	}
	return true;
}

#else
typedef int s09_fast_absent;	/* a translation unit is not empty */
#endif
