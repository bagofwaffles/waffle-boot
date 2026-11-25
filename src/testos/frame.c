/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The test OS's frame timing (scenario HW-FRAME; S-13c.017, S-13c.018):
 * TE's period and high time, and frames started on a TE rising edge
 * through held mode's frame path, each timed with the cycle counter from
 * its start to the end of its transfer and to the next TE rising edge.
 * Sampling, not interrupts: a time is that of the first sample that saw
 * the change, late by at most one pass of the loop. Nothing here is
 * compiled into the normal image.
 */
#include "waffle/testos.h"

#if defined(OPXY_TEST_OS)

#include "waffle/log/log.h"

static void span_add(struct opxy_testos_span *s, uint64_t v)
{
	if (s->n == 0u || v < s->min)
		s->min = v;
	if (s->n == 0u || v > s->max)
		s->max = v;
	s->sum += v;
	s->n++;
}

/* The cycle count of the first sample at level after one at the other
 * level; false after limit cycles without. */
static bool edge(const struct opxy_testos_frame_ops *o, bool level,
		 uint64_t limit, uint64_t *at)
{
	const uint64_t end = o->cycles(o->ctx) + limit;
	bool other = false;

	for (;;) {
		const bool te = o->te(o->ctx);
		const uint64_t now = o->cycles(o->ctx);

		if (te != level) {
			other = true;
		} else if (other) {
			*at = now;
			return true;
		}
		if (now >= end)
			return false;
	}
}

int opxy_testos_frame_measure(const struct opxy_testos_frame_ops *o,
			      struct opxy_testos_frame_time *ft)
{
	const uint64_t te_limit = (uint64_t)OPXY_FT_TE_WAIT_US * o->cycles_per_us;
	const uint64_t frame_limit = (uint64_t)OPXY_FT_FRAME_WAIT_US * o->cycles_per_us;
	uint64_t rise, fall, next;
	unsigned k;

	*ft = (struct opxy_testos_frame_time){ 0 };
	/* TE: a rising edge, then each period's high time and length */
	if (!edge(o, true, te_limit, &rise))
		return OPXY_FT_NO_TE;
	for (k = 0; k < OPXY_FT_PERIODS; k++) {
		if (!edge(o, false, te_limit, &fall) ||
		    !edge(o, true, te_limit, &next))
			return OPXY_FT_NO_TE;
		span_add(&ft->high, fall - rise);
		span_add(&ft->period, next - rise);
		rise = next;
	}
	/* The frames, each started on a fresh rising edge */
	if (!edge(o, true, te_limit, &rise))
		return OPXY_FT_NO_TE;
	for (k = 0; k < OPXY_FT_FRAMES; k++) {
		const uint64_t start = o->cycles(o->ctx);
		uint64_t end;
		bool low = false, overran = false;

		if (o->start(o->ctx) != 0)
			return OPXY_FT_NO_FRAME;
		for (;;) {
			const bool te = o->te(o->ctx);
			const bool busy = o->busy(o->ctx);

			end = o->cycles(o->ctx);
			if (!te)
				low = true;
			else if (low)
				overran = true;	/* a rising edge came first */
			if (!busy)
				break;
			if (end - start >= frame_limit)
				return OPXY_FT_STUCK;
		}
		span_add(&ft->latency, start - rise);
		span_add(&ft->frame, end - start);
		if (overran) {
			ft->overruns++;
			if (!edge(o, true, te_limit, &rise))
				return OPXY_FT_NO_TE;
			continue;
		}
		if (!edge(o, true, te_limit, &next))
			return OPXY_FT_NO_TE;
		span_add(&ft->left, next - end);
		rise = next;
	}
	return OPXY_FT_OK;
}

/* v cycles as microseconds */
static uint32_t us_of(uint64_t v, uint32_t cpu)
{
	const uint64_t us = v / cpu;

	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

/* "N.NN" milliseconds */
static void ms(struct s14_text *t, uint64_t v, uint32_t cpu)
{
	const uint32_t us = us_of(v, cpu), frac = us % 1000u / 10u;

	s14_text_dec(t, us / 1000u);
	s14_text_str(t, frac < 10u ? ".0" : ".");
	s14_text_dec(t, frac);
}

/* "mean (min-max) ms", or in us */
static void span(struct s14_text *t, const struct opxy_testos_span *s,
		 uint32_t cpu, bool in_ms)
{
	const uint64_t v[3] = { s->n != 0u ? s->sum / s->n : 0u, s->min, s->max };
	unsigned i;

	for (i = 0; i < 3u; i++) {
		s14_text_str(t, i == 0u ? "" : i == 1u ? " (" : "-");
		if (in_ms)
			ms(t, v[i], cpu);
		else
			s14_text_dec(t, us_of(v[i], cpu));
	}
	s14_text_str(t, in_ms ? ") ms" : ") us");
}

void opxy_testos_frame_lines(int rc, const struct opxy_testos_frame_time *ft,
			     uint32_t cycles_per_us, struct s14_text *te,
			     struct s14_text *frame)
{
	s14_text_init(te);
	s14_text_init(frame);
	s14_text_str(te, "te (HW-FRAME, mean (min-max)): ");
	if (ft->period.n != 0u) {
		s14_text_str(te, "period ");
		span(te, &ft->period, cycles_per_us, true);
		s14_text_str(te, " over ");
		s14_text_dec(te, ft->period.n);
		s14_text_str(te, ", high ");
		span(te, &ft->high, cycles_per_us, false);
	} else {
		s14_text_str(te, "no rising edge within ");
		s14_text_dec(te, OPXY_FT_TE_WAIT_US / 1000u);
		s14_text_str(te, " ms");
	}
	s14_text_str(frame, "frames on te, held mode's path: ");
	if (ft->frame.n == 0u) {
		s14_text_str(frame, rc == OPXY_FT_NO_FRAME ? "the frame did not start" :
				    rc == OPXY_FT_STUCK ? "the transfer did not end" :
				    "none sent");
		return;
	}
	s14_text_dec(frame, ft->frame.n);
	s14_text_str(frame, ", transfer ");
	span(frame, &ft->frame, cycles_per_us, true);
	s14_text_str(frame, " (15.05 at 70 ns a byte), te to start ");
	span(frame, &ft->latency, cycles_per_us, false);
	if (ft->left.n != 0u) {
		s14_text_str(frame, ", left to next te ");
		span(frame, &ft->left, cycles_per_us, true);
	}
	s14_text_str(frame, ", past next te ");
	s14_text_dec(frame, ft->overruns);
	if (rc != OPXY_FT_OK)
		s14_text_str(frame, rc == OPXY_FT_STUCK ? ", then a transfer did not end" :
				    rc == OPXY_FT_NO_FRAME ? ", then a frame did not start" :
				    ", then te stopped");
}

#else

/* ISO C wants a declaration in every translation unit. */
typedef int opxy_testos_frame_not_built;

#endif /* OPXY_TEST_OS */
