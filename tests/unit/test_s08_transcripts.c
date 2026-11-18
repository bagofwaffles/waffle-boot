/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-08 timed transcripts (tests/oracle/S-08-nordic/transcripts): a scripted
 * Nordic that answers by the transcript's rules, and its expectation steps
 * (consecutive matching frames, counts, time windows, no frame left
 * over, the result and when the exchange ended).
 *
 * Requirements: S-08.002, S-08.003, S-08.011, S-08.014, S-08.015,
 * S-08.020-S-08.023, S-08.030-S-08.035, S-08.040-S-08.047.
 */
#include "s08_test.h"
#include "vectors/s08_transcripts.h"

static const struct s08t *cur;
static int matched[64];
static uint8_t peer_seq;

struct sent {
	uint32_t t;
	uint8_t route, id, len;
	uint8_t p[256];
};

static struct sent sent[8192];
static size_t nsent;

static int accepts(const struct s08t_rule *r, uint8_t route, uint8_t id,
		   const uint8_t *p, size_t n)
{
	if (r->id >= 0 && r->id != id)
		return 0;
	if (r->first >= 0 && (n == 0 || p[0] != r->first))
		return 0;
	if (r->has_payload && (n != r->len || memcmp(p, r->p, n)))
		return 0;
	if (r->kind >= 0 && r->kind != (s06_src(route) >= 8))
		return 0;
	return 1;
}

static void peer(void *ctx, struct t_port *port, const uint8_t *d, size_t n)
{
	uint8_t raw[256];
	size_t k = t_nth_raw(d, n, 0, raw);
	uint32_t t = port->sim->now;

	(void)ctx;
	if (k < 5 || nsent >= sizeof sent / sizeof sent[0])
		return;
	sent[nsent].t = t;
	sent[nsent].route = raw[0];
	sent[nsent].id = raw[2];
	sent[nsent].len = raw[3];
	memcpy(sent[nsent].p, raw + 4, raw[3]);
	nsent++;
	for (int i = 0; i < cur->rn; i++) {
		const struct s08t_rule *r = &s08t_rules[cur->r0 + i];
		int m;

		if (!accepts(r, raw[0], raw[2], raw + 4, raw[3]))
			continue;
		if ((int32_t)t < r->from_us || (int32_t)t >= r->until_us)
			continue;
		m = ++matched[i] - r->skip;
		if (m < 1 || (r->times >= 0 && m > r->times))
			continue;
		for (int s = 0; s < r->sn; s++) {
			const struct s08t_send *x = &s08t_sends[r->s0 + s];

			t_port_inject_frame(port, t + (uint32_t)x->delay_us,
					    x->reply ? s06_swap_route(raw[0]) : x->route,
					    peer_seq++, x->id, x->p, x->len);
		}
		return;
	}
}

static int step_matches(const struct s08t_step *e, const struct sent *f)
{
	if (e->id >= 0 && e->id != f->id)
		return 0;
	if (e->has_payload && (f->len != e->len || memcmp(f->p, e->p, e->len)))
		return 0;
	if (e->has_prefix && (f->len < e->xlen || memcmp(f->p, e->x, e->xlen)))
		return 0;
	if (e->plen >= 0 && f->len != e->plen)
		return 0;
	if (e->kind == 1 && !(s06_src(f->route) >= 8 && s06_src(f->route) <= 11 &&
			      s06_dst(f->route) == 3))
		return 0;
	if (e->kind == 0 && f->route != 0x13)
		return 0;
	return 1;
}

static int within(int64_t v, int32_t lo, int32_t hi)
{
	return lo == S08T_NONE || (v >= lo - 1 && v <= hi + 1);
}

static void check(int result, uint32_t end)
{
	size_t i = 0;
	int64_t prev = -1;

	for (int s = 0; s < cur->en; s++) {
		const struct s08t_step *e = &s08t_steps[cur->e0 + s];
		size_t first = i;

		while (i < nsent && (int)(i - first) < e->hi && step_matches(e, &sent[i]))
			i++;
		if ((int)(i - first) < e->lo) {
			t_failures++;
			fprintf(stderr, "[%s] step %d: %zu frames, wanted %d; next is %s id %02x\n",
				cur->name, s, i - first, e->lo, i < nsent ? "frame" : "nothing",
				i < nsent ? sent[i].id : 0);
			return;
		}
		if (i == first)
			continue;
		if (!within(sent[first].t, e->t_lo, e->t_hi)) {
			t_failures++;
			fprintf(stderr, "[%s] step %d: first frame at %u us\n", cur->name, s,
				sent[first].t);
		}
		if (prev >= 0 && !within((int64_t)sent[first].t - prev, e->a_lo, e->a_hi)) {
			t_failures++;
			fprintf(stderr, "[%s] step %d: %lld us after the previous step\n",
				cur->name, s, (long long)sent[first].t - prev);
		}
		for (size_t k = first + 1; k < i; k++)
			if (!within((int64_t)sent[k].t - sent[k - 1].t, e->g_lo, e->g_hi)) {
				t_failures++;
				fprintf(stderr, "[%s] step %d: gap %lld us\n", cur->name, s,
					(long long)sent[k].t - sent[k - 1].t);
				break;
			}
		prev = sent[i - 1].t;
	}
	if (i < nsent) {
		t_failures++;
		fprintf(stderr, "[%s] unexpected frame at %u us: id %02x\n", cur->name,
			sent[i].t, sent[i].id);
	}
	if (cur->result >= 0)
		CHECK_EQ(result, cur->result);
	if (!within(end, cur->end_lo, cur->end_hi)) {
		t_failures++;
		fprintf(stderr, "[%s] ended at %u us, wanted [%d, %d]\n", cur->name, end,
			cur->end_lo, cur->end_hi);
	}
}

static int run(void)
{
	static uint8_t img[65536];
	struct s08_nordic *n = &nrig.nordic;
	uint8_t buf[256];
	long len;

	for (uint32_t i = 0; i < cur->image_len && i < sizeof img; i++)
		img[i] = (uint8_t)(i * 7 + 3);
	switch (cur->proc) {
	case 0:
		return s08_link_greeting(n);
	case 1:
		return s08_enter_dfu(n);
	case 2:
		return s08_exit_dfu(n);
	case 3:
		return s08_transfer(n, img, cur->image_len, cur->version, cur->type,
				    cur->product);
	case 4:
		s08_idle(n, (uint32_t)cur->idle_us);
		return 1;
	case 5:
		return s08_run_start(n, cur->nordic_image != 0, buf, sizeof buf, &len);
	case 6:
		return s08_run_end(n);
	default: {
		/* a whole run as S-11 drives it: start, then end (S-08.035) */
		bool ok = s08_run_start(n, cur->nordic_image != 0, buf, sizeof buf,
					&len);

		return s08_run_end(n) && ok;
	}
	}
}

/* Where a failed transfer stopped, for the transcripts that fail one:
 * the stage, what came back, and the image chunk (0: not a chunk). */
static const struct {
	const char *name;
	enum s08_stage at;
	enum s08_reply reply;
	unsigned chunk;
} stops[] = {
	{ "transfer_not_in_dfu", S08_AT_QUERY, S08_REPLY_REFUSED, 0 },
	{ "prepare_too_late", S08_AT_PREPARE, S08_REPLY_NONE, 0 },
	{ "prepare_busy_endless", S08_AT_PREPARE, S08_REPLY_BUSY, 0 },
	/* the header chunk is the first chunk request; image chunks count
	 * from 1 after it */
	{ "chunk_silent", S08_AT_CHUNK, S08_REPLY_NONE, 3 },
	{ "chunk_error_text", S08_AT_CHUNK, S08_REPLY_REFUSED, 1 },
	{ "chunk_empty_reply", S08_AT_CHUNK, S08_REPLY_REFUSED, 1 },
};

static void check_stop(int result)
{
	const struct s08_failure *f = &nrig.nordic.failure;

	if (cur->proc != 3)
		return;
	if (result) {
		CHECK_EQ(f->at, S08_AT_NONE);
		return;
	}
	CHECK(f->at != S08_AT_NONE);
	for (size_t k = 0; k < sizeof stops / sizeof stops[0]; k++) {
		if (strcmp(stops[k].name, cur->name) != 0)
			continue;
		CHECK_EQ(f->at, stops[k].at);
		CHECK_EQ(f->reply, stops[k].reply);
		if (stops[k].at == S08_AT_CHUNK) {
			CHECK_EQ(f->chunk, stops[k].chunk);
			CHECK_EQ(f->chunks, (cur->image_len + S08_CHUNK_MAX - 1u) /
						    S08_CHUNK_MAX);
		}
	}
}

int main(int argc, char **argv)
{
	for (size_t i = 0; i < s08t_count; i++) {
		int result;

		cur = &s08t[i];
		if (argc > 1 && strcmp(argv[1], cur->name))
			continue;
		t_case = cur->name;
		t_nrig_init(cur->state, S06_MS(400000));
		memset(matched, 0, sizeof matched);
		nsent = 0;
		peer_seq = 0;
		nrig.port.on_tx = peer;
		for (int g = 0; g < cur->gn; g++) {
			const struct s08t_greet *x = &s08t_greets[cur->g0 + g];

			t_port_inject_frame(&nrig.port, (uint32_t)x->at_us, x->route,
					    peer_seq++, x->id, x->p, x->len);
		}
		result = run();
		/* S-08.002's tries: all 1000 when none was answered */
		if (cur->proc == 0)
			CHECK(result ? nrig.nordic.greet_tries >= 1u &&
				       nrig.nordic.greet_tries < 1000u :
				       nrig.nordic.greet_tries == 1000u);
		check(result, nrig.sim.now);
		check_stop(result);
	}
	return t_finish("test_s08_transcripts");
}
