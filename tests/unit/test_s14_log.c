/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-14 log: threshold and configuration (log_config.json), the frames
 * (frames.json), relayed lines (relay.json), the sinks and the required
 * lines.
 * Requirements: S-14.008, S-14.010, S-14.011, S-14.012, S-14.013, S-14.014,
 * S-14.020, S-14.021, S-14.022, S-14.023, S-14.024, S-14.025, S-14.026,
 * S-14.027, S-14.028, S-14.029, S-14.040, S-14.041, S-14.042, S-14.060.
 */
#include <stdbool.h>
#include <stdint.h>

#include "waffle/log/log.h"
#include "s14_check.h"
#define S14V_LOG
#include "vectors/s14_vectors.h"

/* ---- fakes ----------------------------------------------------------- */

static uint32_t clock_now;

static uint32_t fake_now(void *ctx)
{
	(void)ctx;
	return clock_now;
}

static const struct s14_clock_ops clock_ops = { fake_now, NULL };

#define MAX_FRAMES 64
struct frame {
	char link;        /* 'p' PSoC, 'c' CDC, 's' screen */
	uint8_t route, id, seq;
	uint8_t payload[256];
	size_t len;
};

/* One ordered record of everything the sinks took, across sinks. */
static struct frame frames[MAX_FRAMES];
static size_t n_frames;

/* A link: takes frames while `accept`, numbering only those it takes. */
struct fake_link {
	char name;
	bool accept;
	uint8_t next_seq;
	unsigned offered;
};

static struct frame *record(char link)
{
	struct frame *f = &frames[n_frames < MAX_FRAMES ? n_frames : MAX_FRAMES - 1];

	if (n_frames < MAX_FRAMES)
		n_frames++;
	memset(f, 0, sizeof *f);
	f->link = link;
	return f;
}

static bool fake_send(void *ctx, uint8_t route, uint8_t id,
		      const uint8_t *payload, size_t len)
{
	struct fake_link *l = ctx;
	struct frame *f;

	l->offered++;
	if (!l->accept)
		return false;
	f = record(l->name);
	f->route = route;
	f->id = id;
	f->seq = l->next_seq++;      /* a taken frame only (S-14.027) */
	memcpy(f->payload, payload, len);
	f->len = len;
	return true;
}

static void fake_show(void *ctx, const uint8_t *body, size_t len)
{
	struct frame *f = record('s');

	(void)ctx;
	memcpy(f->payload, body, len);
	f->len = len;
}

static struct fake_link psoc_link = { 'p', true, 0, 0 };
static struct fake_link cdc_link = { 'c', true, 0, 0 };
static const struct s14_link_ops psoc_ops = { fake_send, &psoc_link };
static const struct s14_link_ops cdc_ops = { fake_send, &cdc_link };
static const struct s14_screen_ops screen_ops = { fake_show, NULL };

static void reset_fakes(void)
{
	n_frames = 0;
	psoc_link.accept = cdc_link.accept = true;
	psoc_link.next_seq = cdc_link.next_seq = 0;
	psoc_link.offered = cdc_link.offered = 0;
	clock_now = 0;
}

static void new_log(struct s14_log *log, enum s14_core core)
{
	reset_fakes();
	s14_log_init(log, core, &clock_ops, &psoc_ops, &cdc_ops, &screen_ops);
}

static size_t count(char link)
{
	size_t i, n = 0;

	for (i = 0; i < n_frames; i++)
		n += frames[i].link == link;
	return n;
}

/* The text of a frame's line after its timestamp and tag. */
static const uint8_t *frame_text(const struct frame *f, size_t *len)
{
	size_t tl = s14_tag_len(f->payload + 4, f->len - 4u);

	*len = f->len - 4u - tl;
	return f->payload + 4 + tl;
}

static int text_has(const struct frame *f, const char *s)
{
	size_t n, k = strlen(s), i;
	const uint8_t *t = frame_text(f, &n);

	for (i = 0; i + k <= n; i++)
		if (memcmp(t + i, s, k) == 0)
			return 1;
	return 0;
}

/* ---- S-14.011 - S-14.013: threshold and configuration --------------- */

static void test_initial(void)
{
	struct s14_log log;

	new_log(&log, S14_MASTER);
	CHECK(log.threshold == s14v_initial.threshold &&
	      log.psoc_on == s14v_initial.psoc &&
	      log.cdc_on == s14v_initial.cdc,
	      "S-14.011/022/024 initial state");
}

static void test_config_requests(void)
{
	struct s14_log log;
	size_t i;

	new_log(&log, S14_MASTER);
	for (i = 0; i < S14V_N(s14v_requests); i++) {
		const struct s14v_request *r = &s14v_requests[i];

		CHECK(log.threshold == r->before.threshold &&
		      log.psoc_on == r->before.psoc &&
		      log.cdc_on == r->before.cdc, "before: %s", r->note);
		s14_log_config(&log, (const uint8_t *)r->payload, r->len);
		CHECK(log.threshold == r->after.threshold &&
		      log.psoc_on == r->after.psoc &&
		      log.cdc_on == r->after.cdc,
		      "S-14.012/013 after: %s", r->note);
	}
}

static void test_filter(void)
{
	struct s14_log log;
	size_t i;

	for (i = 0; i < S14V_N(s14v_filter); i++) {
		const struct s14v_filter *f = &s14v_filter[i];

		new_log(&log, S14_MASTER);
		log.threshold = (uint8_t)f->threshold;
		CHECK(s14_log_passes(&log, (enum s14_level)f->level) ==
		      f->emitted, "S-14.011 level %u threshold %u",
		      f->level, f->threshold);
		s14_log_str(&log, (enum s14_level)f->level, S14_NO_TAG, "x");
		CHECK((count('p') == 1u) == f->emitted,
		      "S-14.011 emitted: level %u threshold %u",
		      f->level, f->threshold);
	}
}

/* S-14.014: the request changes only the threshold and the sink states;
 * it sends nothing itself. */
static void test_config_no_other_effect(void)
{
	static const uint8_t p[] = { 0xa1, 0x03 };
	struct s14_log log;

	new_log(&log, S14_MASTER);
	s14_log_screen_start(&log);
	s14_log_config(&log, p, sizeof p);
	CHECK(n_frames == 0u && psoc_link.offered == 0u &&
	      cdc_link.offered == 0u && log.screen_started && !log.silent,
	      "S-14.014 no other effect");
}

/* ---- S-14.021, S-14.023: frames -------------------------------------- */

static void test_frames(void)
{
	struct s14_log log;
	size_t i;

	new_log(&log, S14_MASTER);
	log.threshold = 4;
	log.cdc_on = true;
	for (i = 0; i < S14V_N(s14v_frames); i++) {
		const struct s14v_frame *v = &s14v_frames[i];
		const struct frame *f;

		/* One sink at a time, so every frame is the vector's. */
		log.psoc_on = !v->cdc;
		log.cdc_on = v->cdc;
		psoc_link.next_seq = cdc_link.next_seq = (uint8_t)v->seq;
		n_frames = 0;
		clock_now = v->timestamp;
		s14_log_emit(&log, (enum s14_level)v->level,
			     (struct s14_tag){ v->name, v->line_no },
			     (const uint8_t *)v->text, v->text_len);
		CHECK(n_frames == 1u, "frame %zu: one frame", i);
		f = &frames[0];
		CHECK(f->link == (v->cdc ? 'c' : 'p') && f->route == v->route &&
		      f->id == v->id && f->seq == v->seq,
		      "S-14.021/023 frame %zu header", i);
		CHECK(mem_eq(f->payload, f->len, v->line, v->line_len),
		      "S-14.021/023 frame %zu payload", i);
	}
}

/* ---- S-14.020 - S-14.029: sinks ------------------------------------- */

static void test_sinks(void)
{
	static const uint8_t cdc_on[] = { 0xa2, 0x03 };
	static const uint8_t psoc_off[] = { 0xa2, 0x02 };
	struct s14_log log;
	size_t n;
	const uint8_t *t;

	/* S-14.022, S-14.024: PSoC on, CDC off at the start. */
	new_log(&log, S14_MASTER);
	s14_log_str(&log, S14_INFO, S14_TAG("t"), "a");
	CHECK(count('p') == 1u && count('c') == 0u && cdc_link.offered == 0u,
	      "S-14.022/024 defaults");

	/* S-14.027: lines before the CDC sink was on never appear later. */
	s14_log_config(&log, cdc_on, sizeof cdc_on);
	CHECK(count('c') == 0u, "S-14.027 nothing replayed");
	s14_log_str(&log, S14_INFO, S14_TAG("t"), "b");
	CHECK(count('p') == 2u && count('c') == 1u, "S-14.013 CDC on");

	/* S-14.029: one line reaches the sinks before the next line. */
	CHECK(frames[1].link == 'p' && frames[2].link == 'c' &&
	      text_has(&frames[1], "b") && text_has(&frames[2], "b"),
	      "S-14.029 order");

	/* S-14.027: a link that cannot take a line drops it, consumes no
	 * sequence number, and the other sink still gets it. */
	psoc_link.accept = false;
	s14_log_str(&log, S14_INFO, S14_TAG("t"), "c");
	CHECK(count('p') == 2u && count('c') == 2u, "S-14.027 dropped on one");
	CHECK(psoc_link.offered == 3u, "S-14.028 offered once, no retry");
	psoc_link.accept = true;
	s14_log_str(&log, S14_INFO, S14_TAG("t"), "d");
	CHECK(frames[n_frames - 2u].link == 'p' &&
	      frames[n_frames - 2u].seq == 2u, "S-14.027 no gap");

	/* S-14.013: the PSoC sink switched off. */
	s14_log_config(&log, psoc_off, sizeof psoc_off);
	psoc_link.offered = 0;
	s14_log_str(&log, S14_INFO, S14_TAG("t"), "e");
	CHECK(psoc_link.offered == 0u && count('c') == 4u, "S-14.013 PSoC off");

	/* S-14.025: level 0 reaches the screen only after the display start,
	 * as the body without the timestamp. */
	new_log(&log, S14_MASTER);
	s14_log_str(&log, S14_FATAL, S14_TAG("t"), "before");
	CHECK(count('s') == 0u, "S-14.025 not before the display start");
	s14_log_screen_start(&log);
	s14_log_str(&log, S14_ERROR, S14_TAG("t"), "err");
	s14_log_str(&log, S14_INFO, S14_TAG("t"), "info");
	CHECK(count('s') == 0u, "S-14.025 other levels never on screen");
	s14_log_str(&log, S14_FATAL, S14_TAG("t"), "fatal");
	CHECK(count('s') == 1u, "S-14.025 level 0 on screen");
	t = frames[n_frames - 1u].payload;
	n = frames[n_frames - 1u].len;
	CHECK(frames[n_frames - 2u].link == 'p' &&
	      frames[n_frames - 2u].id == S14_ID_LOG &&
	      mem_eq(t, n, frames[n_frames - 2u].payload + 4,
		     frames[n_frames - 2u].len - 4u),
	      "S-14.025 the screen gets the frame's body; S-14.021 id 0x03");

	/* S-14.026: the slave emits nothing, whatever it is told. */
	new_log(&log, S14_SLAVE);
	s14_log_config(&log, cdc_on, sizeof cdc_on);
	s14_log_screen_start(&log);
	s14_log_str(&log, S14_FATAL, S14_TAG("t"), "x");
	(void)s14_log_relay(&log, 2, S14_ID_LOG,
			    (const uint8_t *)"\0\0\0\0hi", 6);
	CHECK(n_frames == 0u && psoc_link.offered == 0u &&
	      cdc_link.offered == 0u, "S-14.026 slave silent");
}

/* S-14.002, S-14.003: the line carries the clock's count. */
static void test_timestamp(void)
{
	struct s14_log log;

	new_log(&log, S14_MASTER);
	clock_now = 0x01020304u;
	s14_log_str(&log, S14_INFO, S14_NO_TAG, "t");
	CHECK(frames[0].payload[0] == 1 && frames[0].payload[1] == 2 &&
	      frames[0].payload[2] == 3 && frames[0].payload[3] == 4,
	      "S-14.002 timestamp from the clock");
}

/* ---- S-14.040 - S-14.042: relayed lines ----------------------------- */

static void test_relay(void)
{
	struct s14_log log;
	size_t i;

	for (i = 0; i < S14V_N(s14v_relays); i++) {
		const struct s14v_relay *v = &s14v_relays[i];
		bool handled;

		new_log(&log, S14_MASTER);
		log.cdc_on = true;
		handled = s14_log_relay(&log, (uint8_t)v->src, (uint8_t)v->id,
					(const uint8_t *)v->payload,
					v->payload_len);
		CHECK(handled, "relay %s: handled", v->note);
		if (!v->relayed) {
			CHECK(n_frames == 0u, "S-14.041/042 no line: %s",
			      v->note);
			continue;
		}
		CHECK(count('p') == 1u && count('c') == 1u,
		      "S-14.040 one line per sink: %s", v->note);
		if (n_frames == 2u) {
			size_t n;
			const uint8_t *t = frame_text(&frames[1], &n);
			uint8_t want_id = v->level == 1u ? S14_ID_LOG_ERROR
							 : S14_ID_LOG;
			/* S-14.006 applies to the relayed line: a long text
			 * is cut, the line ending in "..". */
			size_t tl = frames[1].len - 4u - n;
			size_t keep = v->text_len;

			if (4u + tl + v->text_len > S14_LINE_MAX)
				keep = S14_CUT_KEEP - 4u - tl;
			CHECK(frames[1].id == want_id, "S-14.040 level: %s",
			      v->note);
			CHECK(n >= keep && memcmp(t, v->text, keep) == 0,
			      "S-14.040/008 text: %s", v->note);
			if (keep != v->text_len)
				CHECK(frames[1].len == S14_LINE_MAX &&
				      t[n - 2u] == '.' && t[n - 1u] == '.',
				      "S-14.006 relayed cut: %s", v->note);
			else
				CHECK(n == v->text_len, "text length: %s",
				      v->note);
		}
	}

	/* The threshold applies to relayed lines (S-14.040, S-14.011). */
	new_log(&log, S14_MASTER);
	log.threshold = 2;
	(void)s14_log_relay(&log, 2, S14_ID_LOG,
			    (const uint8_t *)"\0\0\0\0hi", 6);
	CHECK(n_frames == 0u, "S-14.040 relayed level 3 under threshold 2");
	(void)s14_log_relay(&log, 2, S14_ID_LOG_ERROR,
			    (const uint8_t *)"\0\0\0\0hi", 6);
	CHECK(n_frames == 1u, "S-14.040 relayed level 1 under threshold 2");
	CHECK(!s14_log_relay(&log, 2, 0x05, (const uint8_t *)"abcd", 4),
	      "other ids are not log messages");
}

/* ---- S-14.060: required lines --------------------------------------- */

static void test_required(void)
{
	static const uint8_t dfu[] = "%n%s%x";
	struct s14_log log;

	/* Bad CRC: level 1, route 0x41, seq 0x5A, id 0x7C as values. */
	new_log(&log, S14_MASTER);
	s14_log_bad_crc(&log, 0x41, 0x5a, 0x7c);
	CHECK(n_frames == 1u && frames[0].id == S14_ID_LOG_ERROR &&
	      text_has(&frames[0], "41") && text_has(&frames[0], "5a") &&
	      text_has(&frames[0], "7c"), "S-14.060 bad CRC line (S-16.020)");

	/* Double fault: level 0 (id 0x03), and on the screen. */
	new_log(&log, S14_MASTER);
	log.threshold = 1;
	s14_log_screen_start(&log);
	s14_log_double_fault(&log);
	CHECK(n_frames == 2u && frames[0].id == S14_ID_LOG &&
	      frames[1].link == 's', "S-14.060 double fault at level 0");

	/* DFU error text verbatim, never a format (S-16.099). */
	new_log(&log, S14_MASTER);
	s14_log_dfu_error(&log, "nordic", dfu, sizeof dfu - 1u);
	CHECK(n_frames == 1u && text_has(&frames[0], "%n%s%x"),
	      "S-14.060 DFU error text verbatim (S-14.008)");
}

int main(void)
{
	test_initial();
	test_config_requests();
	test_filter();
	test_config_no_other_effect();
	test_frames();
	test_sinks();
	test_timestamp();
	test_relay();
	test_required();
	return check_done("test_s14_log");
}
