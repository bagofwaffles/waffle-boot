/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's panel (src/front/fr_panel.c) against the full bootloader's
 * (s13c_panel.c, display_draw.c) over the same recording bus and clock:
 * the same reset levels, commands, parameter bytes and waits, the
 * identification reads aside (S-13c.011), for each reset path of
 * S-13c.004, but for S-13c.010's form: the full one writes the visible
 * window in one color and sends DISPON in its start, the front sends its
 * first frame with the display off and DISPON after that frame's last
 * byte. The waits' bounds (S-13c.006, S-13c.007); the logo frame, the
 * logo alone, byte for byte (S-15.132); DISPON once; nothing at a power-off
 * (S-13c.022).
 */
#include <stdlib.h>
#include <string.h>

#include "support/check.h"

#include "front/fr_draw.h"
#include "front/fr_panel.h"
#include "waffle/display.h"
#include "waffle/s13c_panel.h"
#include "waffle/s15_lines.h"

/* ---- a recording bus and clock ---- */

enum { EV_RESET = 1, EV_SELECT, EV_CMD, EV_DATA, EV_REPEAT, EV_DELAY };

struct rec {
	uint8_t *log;
	size_t len, cap;
	uint64_t now;		/* the fake clock, us */
	unsigned reads;		/* identification reads (the full one only) */
	uint64_t release;	/* the time of the last RESET rising edge */
	bool level;
	uint64_t first_cmd;	/* the time of the first command */
	bool any_cmd;
};

static void put(struct rec *r, const void *p, size_t n)
{
	if (n == 0)
		return;		/* nothing to copy, and no buffer yet */
	if (r->len + n > r->cap) {
		r->cap = (r->len + n) * 2u;
		r->log = realloc(r->log, r->cap);
		if (r->log == NULL)
			abort();
	}
	memcpy(r->log + r->len, p, n);
	r->len += n;
}

static void put8(struct rec *r, uint8_t v) { put(r, &v, 1); }
static void put32(struct rec *r, uint32_t v) { put(r, &v, 4); }

static void r_reset(void *ctx, bool high)
{
	struct rec *r = ctx;

	put8(r, EV_RESET);
	put8(r, high);
	if (high && !r->level)
		r->release = r->now;
	r->level = high;
}

static void r_select(void *ctx, bool on)
{
	put8(ctx, EV_SELECT);
	put8(ctx, on);
}

static void r_cmd(void *ctx, uint8_t c)
{
	struct rec *r = ctx;

	if (!r->any_cmd) {
		r->any_cmd = true;
		r->first_cmd = r->now;
	}
	put8(r, EV_CMD);
	put8(r, c);
}

static void r_data(void *ctx, const uint8_t *p, size_t n)
{
	put8(ctx, EV_DATA);
	put32(ctx, (uint32_t)n);
	put(ctx, p, n);
}

/* The full one's only: its one-color write of the visible window. */
static void r_repeat(void *ctx, const uint8_t *g, size_t len, uint32_t count)
{
	struct rec *r = ctx;

	put8(r, EV_REPEAT);
	put32(r, (uint32_t)len);
	put32(r, count);
	put(r, g, len);
}

static uint64_t r_now(void *ctx)
{
	return ((struct rec *)ctx)->now;
}

static void r_delay(void *ctx, uint32_t us)
{
	struct rec *r = ctx;

	put8(r, EV_DELAY);
	put32(r, us);
	r->now += us;
}

static int r_read(void *ctx, uint8_t cmd, uint8_t *out, size_t n)
{
	struct rec *r = ctx;

	(void)cmd;
	r->reads++;
	memset(out, 0x5A, n);
	return 0;
}

static void rec_init(struct rec *r, uint64_t t0)
{
	free(r->log);
	memset(r, 0, sizeof *r);
	r->now = t0;
	r->level = false;	/* before the first drive: the strap or the pulse */
}

static struct fr_panel_bus fr_bus(struct rec *r)
{
	struct fr_panel_bus b = { r_reset, r_select, r_cmd, r_data, r };

	return b;
}

static struct fr_clock fr_clk(struct rec *r)
{
	struct fr_clock c = { r_now, r_delay, r };

	return c;
}

static struct s13c_bus full_bus(struct rec *r, bool reads)
{
	struct s13c_bus b;

	memset(&b, 0, sizeof b);
	b.reset = r_reset;
	b.select = r_select;
	b.cmd = r_cmd;
	b.data = r_data;
	b.repeat = r_repeat;
	b.read = reads ? r_read : NULL;
	b.now_us = r_now;
	b.delay_us = r_delay;
	b.ctx = r;
	return b;
}

static bool same(const struct rec *a, const struct rec *b)
{
	return a->len == b->len && (a->len == 0 || memcmp(a->log, b->log, a->len) == 0);
}

/* The size of the log's event at e. */
static size_t ev_len(const uint8_t *e)
{
	uint32_t n;

	switch (e[0]) {
	case EV_DATA:
		memcpy(&n, e + 1, 4);
		return 5u + n;
	case EV_REPEAT:
		memcpy(&n, e + 1, 4);
		return 9u + n;
	case EV_DELAY:
		return 5u;
	default:
		return 2u;
	}
}

/* The commands c in the log from the event at `from`. */
static unsigned count_cmd(const struct rec *r, size_t from, uint8_t c)
{
	unsigned k = 0;

	for (size_t p = from; p < r->len; p += ev_len(r->log + p))
		k += r->log[p] == EV_CMD && r->log[p + 1u] == c ? 1u : 0u;
	return k;
}

/* Whether the event at *p is the command c, followed by these n parameter
 * bytes when a is not NULL; *p moves past them. */
static bool next_is(const struct rec *r, size_t *p, uint8_t c, const uint8_t *a, uint32_t n)
{
	uint32_t got;

	if (*p + 2u > r->len || r->log[*p] != EV_CMD || r->log[*p + 1u] != c)
		return false;
	*p += 2u;
	if (a == NULL)
		return true;
	if (*p + 5u + n > r->len || r->log[*p] != EV_DATA)
		return false;
	memcpy(&got, r->log + *p + 1, 4);
	if (got != n || memcmp(r->log + *p + 5, a, n) != 0)
		return false;
	*p += 5u + n;
	return true;
}

static const uint8_t caset_vis[4] = { 0x00, 0x31, 0x01, 0x0E };
static const uint8_t caset_frame[4] = { 0x00, 0x31, 0x01, 0x10 };
static const uint8_t raset[4] = { 0x00, 0x00, 0x01, 0xDF };

/*
 * S-13c.010's two forms: out gets what the front sends for the full one's
 * log. The full one writes the visible window in one color (CASET 49-270,
 * RASET, RAMWR and the repeat), sets the frame window and sends DISPON; the
 * front sets the frame window only, and sends DISPON after its first
 * frame's last byte, when the log has one.
 */
static bool front_form(const struct rec *full, struct rec *out)
{
	size_t p = 0, w, end;
	uint32_t len, count;

	while (p < full->len) {
		size_t q = p;

		if (next_is(full, &q, S13C_CASET, caset_vis, 4))
			break;
		p += ev_len(full->log + p);
	}
	w = p;
	if (!next_is(full, &w, S13C_CASET, caset_vis, 4) ||
	    !next_is(full, &w, S13C_RASET, raset, 4) ||
	    !next_is(full, &w, S13C_RAMWR, NULL, 0) || w + 9u > full->len ||
	    full->log[w] != EV_REPEAT)
		return false;
	memcpy(&len, full->log + w + 1, 4);
	memcpy(&count, full->log + w + 5, 4);
	if (len != 2u || count != S13C_VIS_W * S13C_ROWS)
		return false;
	w += ev_len(full->log + w);
	end = w;
	if (!next_is(full, &end, S13C_CASET, caset_frame, 4) ||
	    !next_is(full, &end, S13C_RASET, raset, 4))
		return false;
	rec_init(out, 0);
	put(out, full->log, p);
	put(out, full->log + w, end - w);
	if (!next_is(full, &end, S13C_DISPON, NULL, 0))
		return false;
	if (end < full->len) {
		put(out, full->log + end, full->len - end);
		put8(out, EV_CMD);
		put8(out, S13C_DISPON);
	}
	return true;
}

/*
 * S-13c.010, S-13c.014, S-13c.016: the log's first RAMWR is a whole frame
 * of a window that covers the visible one (columns 49-270 of it, rows
 * 0-479), sent as 480 rows; no DISPON comes before it, the command after
 * its last byte is DISPON, and no other DISPON follows.
 */
static bool first_frame_then_dispon(const struct rec *r)
{
	unsigned xs = 0, xe = 0, ys = 0, ye = 0, rows = 0;
	uint8_t last = 0;
	size_t p = 0, bytes = 0;
	bool in_frame = false;

	while (p < r->len) {
		const uint8_t *e = r->log + p;

		if (e[0] == EV_CMD && in_frame)
			return e[1] == S13C_DISPON && xs <= 49u && xe >= 270u && ys == 0u &&
			       ye == 479u && rows == S13C_ROWS &&
			       bytes == 2u * (xe - xs + 1u) * (ye - ys + 1u) &&
			       count_cmd(r, p + 2u, S13C_DISPON) == 0u;
		if (e[0] == EV_CMD) {
			if (e[1] == S13C_DISPON)
				return false;
			in_frame = e[1] == S13C_RAMWR;
			last = e[1];
		} else if (e[0] == EV_DATA) {
			uint32_t n;

			memcpy(&n, e + 1, 4);
			if (in_frame) {
				bytes += n;
				rows += n == S13C_ROW_BYTES ? 1u : 0u;
			} else if (n == 4u && last == S13C_CASET) {
				xs = (unsigned)e[5] << 8 | e[6];
				xe = (unsigned)e[7] << 8 | e[8];
			} else if (n == 4u && last == S13C_RASET) {
				ys = (unsigned)e[5] << 8 | e[6];
				ye = (unsigned)e[7] << 8 | e[8];
			}
		} else if (e[0] == EV_REPEAT && in_frame) {
			return false;
		}
		p += ev_len(e);
	}
	return false;
}

/* ---- the cases ---- */

struct scenario {
	uint32_t xmod, ymod;
	unsigned pa8;
	uint64_t t0;		/* the clock at begin */
	uint64_t gap;		/* from begin to start */
	uint8_t path;
};

static const struct scenario cases[] = {
	{ S13C_HANDSHAKE_MAGIC, 50, 1, 80000, 0, FR_PANEL_HANDSHAKE },
	{ S13C_HANDSHAKE_MAGIC, 50, 1, 80000, 30000, FR_PANEL_HANDSHAKE },
	{ S13C_HANDSHAKE_MAGIC, 50, 1, 400000, 0, FR_PANEL_HANDSHAKE },	/* no wait left */
	{ S13C_HANDSHAKE_MAGIC, 0, 1, 2500, 0, FR_PANEL_HANDSHAKE },		/* 0 read as 1 */
	{ S13C_HANDSHAKE_MAGIC, 1, 1, 2500, 119000, FR_PANEL_HANDSHAKE },
	{ S13C_HANDSHAKE_MAGIC, 200, 1, 100000, 0, FR_PANEL_PULSE },		/* in the future */
	{ S13C_HANDSHAKE_MAGIC, 80, 1, 80999, 0, FR_PANEL_PULSE },		/* not earlier */
	{ 0, 50, 1, 30000, 0, FR_PANEL_PULSE },
	{ 0xDEADBEEFu, 0, 1, 0, 0, FR_PANEL_PULSE },				/* ms 0: reset_ms 1 */
	{ 0, 0, 1, 50000, 250000, FR_PANEL_PULSE },
	{ S13C_HANDSHAKE_MAGIC, 50, 0, 80000, 0, FR_PANEL_STRAP },
	{ 0, 0, 0, 0, 10, FR_PANEL_STRAP },
	{ 0, 0, 2, 12345, 60000, FR_PANEL_STRAP },				/* bit 0 is PA8 */
};

static struct rec ra, rb, rc;

static unsigned fails_seen;

/* Stop a scenario loop at its first failure, naming it. */
static int failed_once(unsigned i, int reads)
{
	if (check_failures != fails_seen) {
		printf("  scenario %u, reads %d\n", i, reads);
		fails_seen = check_failures;
		return 1;
	}
	return 0;
}

/* fr_panel_begin + fr_panel_start against s13c_begin + s13c_start(true),
 * with and without the full one's identification reads. */
static void test_start_matches(void)
{
	for (unsigned i = 0; i < ARRAY_LEN(cases); i++) {
		const struct scenario *c = &cases[i];

		for (int reads = 0; reads < 2; reads++) {
			struct fr_panel fp;
			struct s13c_panel sp;
			struct fr_panel_bus fb;
			struct fr_clock fc;
			struct s13c_bus sb;
			uint64_t call, end;

			rec_init(&ra, c->t0);
			fb = fr_bus(&ra);
			fc = fr_clk(&ra);
			fr_panel_begin(&fp, &fb, &fc, c->xmod, c->ymod, c->pa8);
			ra.now += c->gap;
			call = ra.now;
			fr_panel_start(&fp);
			end = ra.now;

			rec_init(&rb, c->t0);
			sb = full_bus(&rb, reads != 0);
			s13c_begin(&sp, &sb, c->xmod, c->ymod, c->pa8);
			rb.now += c->gap;
			CHECK(s13c_start(&sp, true) == S13C_OK);

			CHECK(front_form(&rb, &rc) && same(&ra, &rc));
			/* S-13c.010: the start ends with the frame window and
			 * the display off: no RAMWR, no DISPON in it. */
			CHECK(count_cmd(&ra, 0, S13C_RAMWR) == 0u);
			CHECK(count_cmd(&ra, 0, S13C_DISPON) == 0u);
			{
				size_t at = ra.len - 22u;

				CHECK(ra.len > 22u && next_is(&ra, &at, S13C_CASET, caset_frame, 4) &&
				      next_is(&ra, &at, S13C_RASET, raset, 4));
			}
			CHECK(fp.path == c->path);
			CHECK((unsigned)fp.path == (unsigned)sp.path);
			CHECK(fp.reset_ms == sp.reset_ms);
			CHECK(fp.ready_us == sp.ready_us);
			CHECK(fp.ready && !fp.on && fp.started);
			CHECK(ra.reads == 0u);
			CHECK(rb.reads == (reads ? 2u : 0u));
			CHECK(ra.now == rb.now);

			/* S-13c.006: no earlier than 120 ms from the release
			 * this run saw (the handshake's came before t0), and
			 * the wait at most that. S-13c.007: SLPOUT's 5.1 ms. */
			if (c->path != FR_PANEL_HANDSHAKE)
				CHECK(fp.ready_us == ra.release + 120000u);
			else
				CHECK(fp.ready_us <= c->t0 + 120000u &&
				      fp.ready_us == ((uint64_t)(c->ymod ? c->ymod : 1u) + 1u) * 1000u + 120000u);
			CHECK(ra.first_cmd >= fp.ready_us);
			CHECK(end == (call > fp.ready_us ? call : fp.ready_us) + 5100u);
			if (failed_once(i, reads))
				return;
		}
	}
}

/* A frame's bytes in the log: RAMWR and 480 rows. */
#define FRAME_LOG (2u + S13C_ROWS * (5u + S13C_ROW_BYTES))

/*
 * Nothing before start, and a second start sends nothing (S-13c.013). The
 * first frame is RAMWR, 480 rows, then DISPON, once (S-13c.010); a later
 * frame sends no DISPON. Off after the first frame sends nothing, the
 * display stays on with its last frame, and no frame follows (S-13c.022);
 * off before it sends nothing, the display never on, and no frame follows
 * either.
 */
static unsigned g_served;

static void count_serve(void)
{
	g_served++;
}

static void test_frames_and_off(void)
{
	struct fr_panel fp;
	struct fr_panel_bus fb;
	struct fr_clock fc;
	struct fr_screen s;
	size_t n;

	rec_init(&ra, 1000);
	fb = fr_bus(&ra);
	fc = fr_clk(&ra);
	fr_panel_begin(&fp, &fb, &fc, 0, 0, 1);
	fr_screen_logo(&s);
	n = ra.len;
	fr_panel_frame(&fp, fr_screen_row, &s);	/* not started: nothing */
	fr_panel_off(&fp);
	CHECK(ra.len == n);
	fr_panel_start(&fp);
	n = ra.len;
	fr_panel_start(&fp);
	CHECK(ra.len == n);
	CHECK(fp.ready && !fp.on);

	g_served = 0;
	fr_serve_hook = count_serve;	/* the link served after each row */
	fr_panel_frame(&fp, fr_screen_row, &s);
	fr_serve_hook = NULL;
	CHECK(g_served >= FR_PANEL_ROWS);
	CHECK(ra.len == n + FRAME_LOG + 2u);
	CHECK(ra.log[n] == EV_CMD && ra.log[n + 1u] == S13C_RAMWR);
	CHECK(ra.log[n + FRAME_LOG] == EV_CMD && ra.log[n + FRAME_LOG + 1u] == S13C_DISPON);
	CHECK(first_frame_then_dispon(&ra));
	CHECK(fp.on);
	n = ra.len;
	fr_panel_frame(&fp, fr_screen_row, &s);
	CHECK(ra.len == n + FRAME_LOG);
	CHECK(ra.log[n] == EV_CMD && ra.log[n + 1u] == S13C_RAMWR);
	CHECK(count_cmd(&ra, 0, S13C_DISPON) == 1u);

	n = ra.len;
	fr_panel_off(&fp);
	CHECK(ra.len == n);			/* no DISPOFF, no command */
	CHECK(fp.on && !fp.ready);
	n = ra.len;
	fr_panel_frame(&fp, fr_screen_row, &s);
	fr_panel_off(&fp);
	fr_panel_start(&fp);
	CHECK(ra.len == n);

	/* Off before the first frame. */
	rec_init(&ra, 1000);
	fr_panel_begin(&fp, &fb, &fc, 0, 0, 1);
	fr_panel_start(&fp);
	n = ra.len;
	fr_panel_off(&fp);
	CHECK(ra.len == n);
	CHECK(!fp.on && !fp.ready);
	fr_panel_frame(&fp, fr_screen_row, &s);
	fr_panel_off(&fp);
	fr_panel_start(&fp);
	CHECK(ra.len == n);
	CHECK(count_cmd(&ra, 0, S13C_DISPON) == 0u && count_cmd(&ra, 0, S13C_RAMWR) == 0u);
}

/* The whole start-up's panel bytes: the front's begin and start against
 * wb_display_begin + wb_display_boot_logo's up to the frame; the frame
 * RAMWR, its 480 rows of 448 bytes and DISPON, the rows the logo alone of
 * S-15.132 (s15_handover_frame()). */
static uint8_t fb_mem[S13C_FRAME_BYTES], ref[S13C_FRAME_BYTES];
static size_t ref_at;

static void ref_put(void *ctx, uint16_t px)
{
	(void)ctx;
	if (ref_at + 2u <= sizeof ref) {
		ref[ref_at++] = (uint8_t)(px >> 8);
		ref[ref_at++] = (uint8_t)px;
	}
}

static void test_logo_frame(void)
{
	ref_at = 0;
	s15_handover_frame(ref_put, NULL);
	CHECK(ref_at == sizeof ref);
	for (unsigned k = 0; k < 3u; k++) {
		const struct scenario *c = &cases[k == 0u ? 0u : k == 1u ? 7u : 10u];
		struct fr_panel fp;
		struct fr_panel_bus fb;
		struct fr_clock fc;
		struct fr_screen s;
		struct wb_display d;
		struct s13c_bus sb;
		size_t at;

		rec_init(&ra, c->t0);
		fb = fr_bus(&ra);
		fc = fr_clk(&ra);
		fr_panel_begin(&fp, &fb, &fc, c->xmod, c->ymod, c->pa8);
		fr_panel_start(&fp);
		fr_screen_logo(&s);
		at = ra.len;
		fr_panel_frame(&fp, fr_screen_row, &s);

		rec_init(&rb, c->t0);
		sb = full_bus(&rb, true);
		wb_display_init(&d, fb_mem);
		wb_display_begin(&d, &sb, c->xmod, c->ymod, c->pa8);
		CHECK(wb_display_boot_logo(&d, NULL) == S13C_OK);
		CHECK(front_form(&rb, &rc));
		CHECK(ra.len == at + FRAME_LOG + 2u);
		if (ra.log == NULL || ra.len != at + FRAME_LOG + 2u)
			continue;
		CHECK(at != 0u && rc.log != NULL && rc.len >= at && memcmp(ra.log, rc.log, at) == 0);

		/* RAMWR, then 480 rows of 448 bytes, each the logo's, then
		 * DISPON (S-13c.010). */
		CHECK(ra.log[at] == EV_CMD && ra.log[at + 1u] == S13C_RAMWR);
		CHECK(ra.log[at + FRAME_LOG] == EV_CMD && ra.log[at + FRAME_LOG + 1u] == S13C_DISPON);
		CHECK(first_frame_then_dispon(&ra) && fp.on);
		for (unsigned r = 0; r < S13C_ROWS; r++) {
			const uint8_t *e = ra.log + at + 2u + r * (5u + S13C_ROW_BYTES);
			uint32_t n;

			memcpy(&n, e + 1, 4);
			if (e[0] != EV_DATA || n != S13C_ROW_BYTES ||
			    memcmp(e + 5, ref + (size_t)r * S13C_ROW_BYTES, S13C_ROW_BYTES) != 0) {
				CHECK(false);
				break;
			}
		}
	}
}

int main(void)
{
	RUN(test_start_matches);
	RUN(test_frames_and_off);
	RUN(test_logo_frame);
	free(ra.log);
	free(rb.log);
	free(rc.log);
	return check_summary("test_front_panel");
}
