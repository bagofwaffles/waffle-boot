/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The boot timing build's stamp store, its report lines and the record that
 * crosses a hand-over (waffle/boot_time.h).
 */
#include "support/check.h"

#include <stddef.h>
#include <string.h>

#include "waffle/boot_time.h"
#include "waffle/s09_fast.h"
#include "waffle/s09_master.h"
#include "waffle/s06_bytes.h"
#include "waffle/s06_clock.h"

#define MHZ 400u
#define US(n) ((uint64_t)(n) * MHZ)

static bool line_is(const struct boot_time *t, unsigned k, const char *want)
{
	struct s14_text out;
	const bool got = boot_time_line(t, k, MHZ, &out);

	if (!got || out.len != strlen(want) || memcmp(out.buf, want, out.len) != 0) {
		printf("    line %u: got %d \"%.*s\", want \"%s\"\n", k, got, (int)out.len,
		       (const char *)out.buf, want);
		return false;
	}
	return true;
}

static bool line_empty(const struct boot_time *t, unsigned k)
{
	struct s14_text out;

	return !boot_time_line(t, k, MHZ, &out) && out.len == 0u;
}

/* A line of the report by its text's start, BOOT_TIME_LINES when absent. */
static unsigned line_of(const struct boot_time *t, const char *start)
{
	struct s14_text out;
	unsigned k;

	for (k = 0; k < BOOT_TIME_LINES; k++)
		if (boot_time_line(t, k, MHZ, &out) && out.len >= strlen(start) &&
		    memcmp(out.buf, start, strlen(start)) == 0)
			return k;
	return BOOT_TIME_LINES;
}

static struct boot_time fresh(void)
{
	struct boot_time t;

	memset(&t, 0, sizeof t);
	return t;
}

static void test_mark(void)
{
	struct boot_time t = fresh();

	boot_time_mark(&t, BOOT_TIME_KEYS, 1234u);
	CHECK(t.at[BOOT_TIME_KEYS] == 1234u);
	CHECK(t.set == 1u << BOOT_TIME_KEYS);
	/* a stamp of 0 still counts as reached */
	boot_time_mark(&t, BOOT_TIME_INIT_ENTRY, 0u);
	CHECK(t.set & 1u);
	/* an id past the table changes nothing */
	boot_time_mark(&t, BOOT_TIME_COUNT, 99u);
	CHECK(t.set == (1u << BOOT_TIME_KEYS | 1u));
	/* the last stamp of an id wins */
	boot_time_mark(&t, BOOT_TIME_KEYS, 5678u);
	CHECK(t.at[BOOT_TIME_KEYS] == 5678u);
}

static void test_milestones(void)
{
	struct boot_time t = fresh();

	CHECK(line_empty(&t, 0u));
	boot_time_mark(&t, BOOT_TIME_INIT_ENTRY, 0x123456789ull);
	boot_time_mark(&t, BOOT_TIME_INIT_CLOCKS, 0x200000000ull);
	boot_time_mark(&t, BOOT_TIME_INIT_DDR, 0x200000000ull + US(1500));
	boot_time_mark(&t, BOOT_TIME_MAIN, 0x200000000ull + US(2000));
	boot_time_mark(&t, BOOT_TIME_KEYS, 0x200000000ull + US(812345));
	/* before the clocks: the counter as it stands */
	CHECK(line_is(&t, 0u, "00 init entry: counter 0000000123456789"));
	CHECK(line_is(&t, 1u, "01 init clocks: counter 0000000200000000"));
	/* after them: milliseconds, and the time since the line before */
	CHECK(line_is(&t, 2u, "02 init ddr: 21476.336 ms, +1.500 ms"));
	CHECK(line_is(&t, 3u, "05 main: 21476.836 ms, +0.500 ms"));
	/* milestones not reached take no line */
	CHECK(line_is(&t, 4u, "12 keys: 22287.181 ms, +810.345 ms"));
	CHECK(line_empty(&t, 5u));
	CHECK(line_empty(&t, BOOT_TIME_COUNT - 1u));
}

static void test_first_has_no_step(void)
{
	struct boot_time t = fresh();

	boot_time_mark(&t, BOOT_TIME_HELD, US(4000007));
	CHECK(line_is(&t, 0u, "28 held mode: 4000.007 ms"));
	CHECK(line_empty(&t, 1u));
}

/* The front's stamps are earlier than the main image's first, whatever
 * their numbers: the lines follow the stamps, and no step is negative. */
static void test_time_order(void)
{
	struct boot_time t = fresh();
	struct s14_text out;
	unsigned k;

	boot_time_mark(&t, BOOT_TIME_INIT_DDR, US(1000));
	boot_time_mark(&t, BOOT_TIME_MAIN, US(700000));
	boot_time_mark(&t, BOOT_TIME_BRINGUP, US(701000));
	boot_time_mark(&t, BOOT_TIME_FRONT_ENTRY, US(100000));
	boot_time_mark(&t, BOOT_TIME_PSOC_OPEN, US(150000));
	boot_time_mark(&t, BOOT_TIME_LOGO, US(160000));
	boot_time_mark(&t, BOOT_TIME_KEYS, US(200000));
	boot_time_mark(&t, BOOT_TIME_FRONT_HANDOFF, US(650000));
	CHECK(line_is(&t, 0u, "02 init ddr: 1.000 ms"));
	CHECK(line_is(&t, 1u, "03 front entry: 100.000 ms, +99.000 ms"));
	CHECK(line_is(&t, 2u, "09 psoc open: 150.000 ms, +50.000 ms"));
	CHECK(line_is(&t, 3u, "13 panel light sent: 160.000 ms, +10.000 ms"));
	CHECK(line_is(&t, 4u, "12 keys: 200.000 ms, +40.000 ms"));
	CHECK(line_is(&t, 5u, "04 front hand-off: 650.000 ms, +450.000 ms"));
	CHECK(line_is(&t, 6u, "05 main: 700.000 ms, +50.000 ms"));
	CHECK(line_is(&t, 7u, "06 bring-up: 701.000 ms, +1.000 ms"));
	CHECK(line_empty(&t, 8u));
	/* equal stamps keep their numbers' order, with a step of 0 */
	boot_time_mark(&t, BOOT_TIME_DDR_CHECK, US(701000));
	CHECK(line_is(&t, 8u, "07 ddr check: 701.000 ms, +0.000 ms"));
	/* no line shows a step of more than the whole boot */
	for (k = 0; k < BOOT_TIME_COUNT; k++)
		if (boot_time_line(&t, k, MHZ, &out))
			CHECK(out.len < 60u);
}

/* After a charge wait every later line also counts from its end, which the
 * panel's light message marks: the switch went on then. */
static void test_since_on(void)
{
	struct boot_time t = fresh();

	boot_time_mark(&t, BOOT_TIME_FRONT_ENTRY, US(500000));
	boot_time_mark(&t, BOOT_TIME_PSOC_OPEN, US(600000));
	boot_time_mark(&t, BOOT_TIME_LOGO, US(300000000u));
	boot_time_mark(&t, BOOT_TIME_KEYS, US(300020000u));
	CHECK(line_is(&t, 2u, "13 panel light sent: 300000.000 ms, +299400.000 ms"));
	t.flags = BOOT_TIME_CHARGE_WAITED;
	CHECK(line_is(&t, 0u, "03 front entry: 500.000 ms"));
	CHECK(line_is(&t, 1u, "09 psoc open: 600.000 ms, +100.000 ms"));
	CHECK(line_is(&t, 2u, "13 panel light sent: 300000.000 ms, +299400.000 ms, "
			      "since on 0.000 ms"));
	CHECK(line_is(&t, 3u, "12 keys: 300020.000 ms, +20.000 ms, since on 20.000 ms"));
	/* without a front: from the battery wait's end */
	t = fresh();
	t.flags = BOOT_TIME_BATTERY_RAN;
	boot_time_mark(&t, BOOT_TIME_BATTERY_START, US(1000));
	boot_time_mark(&t, BOOT_TIME_BATTERY_END, US(9000));
	boot_time_mark(&t, BOOT_TIME_KEYS, US(9500));
	CHECK(line_is(&t, 0u, "10 battery wait start: 1.000 ms"));
	CHECK(line_is(&t, 2u, "12 keys: 9.500 ms, +0.500 ms, since on 0.500 ms"));
}

static void test_summary(void)
{
	struct boot_time t = fresh();

	CHECK(line_empty(&t, BOOT_TIME_COUNT));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 1u));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 2u));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 3u));
	boot_time_mark(&t, BOOT_TIME_PSOC_OPEN, US(10));
	CHECK(line_is(&t, BOOT_TIME_COUNT, "psoc not greeted, battery wait did not run"));
	t.flags = BOOT_TIME_GREETED | BOOT_TIME_BATTERY_RAN;
	boot_time_mark(&t, BOOT_TIME_BATTERY_START, US(100));
	CHECK(line_is(&t, BOOT_TIME_COUNT, "psoc greeted, battery wait ran, not ended"));
	boot_time_mark(&t, BOOT_TIME_BATTERY_END, US(3000100));
	CHECK(line_is(&t, BOOT_TIME_COUNT, "psoc greeted, battery wait ran 3000.000 ms"));

	/* the front's two waits, by its hand-over flags */
	boot_time_mark(&t, BOOT_TIME_FRONT_HANDOFF, US(5));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 1u,
		      "front: greeting not waited for, charge wait did not run"));
	t.flags |= BOOT_TIME_GREET_WAITED;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 1u,
		      "front: greeting waited for, charge wait did not run"));
	t.flags |= BOOT_TIME_CHARGE_WAITED;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 1u,
		      "front: greeting waited for, charge wait ran"));

	boot_time_mark(&t, BOOT_TIME_COPY_START, US(1000000));
	boot_time_mark(&t, BOOT_TIME_COPY_LAST, US(2000000));
	t.bytes = 2300000u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 2u,
		      "os copy 2300000 bytes in 1000.000 ms, 2300 kB/s"));

	/* the stream's pacing */
	boot_time_mark(&t, BOOT_TIME_STREAM_END, US(900000));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 3u,
		      "stream: 0 bytes found the ready line high, waited 0.000 ms"));
	t.stream_high = 412u;
	t.stream_wait = US(1234567);
	CHECK(line_is(&t, BOOT_TIME_COUNT + 3u,
		      "stream: 412 bytes found the ready line high, waited 1234.567 ms"));
	/* paced by the controller, sessions are what is seen */
	t.flags |= BOOT_TIME_LINK_FC;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 3u,
		      "stream: 412 sessions began with the ready line high, "
		      "1234.567 ms beyond the bytes' own time"));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 4u));	/* no byte is seen there */
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 5u));
	t.flags &= ~BOOT_TIME_LINK_FC;

	/* the ready line after a byte */
	CHECK(line_is(&t, BOOT_TIME_COUNT + 4u,
		      "ready line after a byte: 0 high at the first sample, 0 raised later, "
		      "the latest at 0.00 us, longest hold 0.00 us"));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 5u));	/* no window, no steps */
	t.line_window = (uint32_t)US(2);
	t.line_high_first = 400000u;
	t.line_late = 12345u;
	t.line_late_max = (uint32_t)US(1) + 180u;
	t.line_hold_max = (uint32_t)US(37) + 4u;
	t.line_raise[0] = 400000u;
	t.line_raise[5] = 12345u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 4u,
		      "ready line after a byte: 400000 high at the first sample, 12345 raised "
		      "later, the latest at 1.45 us, longest hold 37.01 us"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 5u,
		      "raises in 8 steps of 0.25 us: 400000 0 0 0 0 12345 0 0"));
	t.line_high_first = t.line_late = 0u;
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 6u));	/* none to place */
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 7u));
	/* the stream's reads at the faster rate */
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 15u));
	t.slot_fast = 1u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 15u,
		      "slot read at the faster rate: 1 whole, 0 failed and read again, "
		      "0 failed the CRC and read again, 0 of those then good; "
		      "0 kept by a trial mark's CRC"));
	t.slot_fast_failed = 2u;
	t.slot_reread = 3u;
	t.slot_reread_good = 1u;
	t.slot_trial = 1u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 15u,
		      "slot read at the faster rate: 1 whole, 2 failed and read again, "
		      "3 failed the CRC and read again, 1 of those then good; "
		      "1 kept by a trial mark's CRC"));
	t.flags |= BOOT_TIME_SLOT_CRC_HW;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 15u,
		      "slot read at the faster rate: 1 whole, 2 failed and read again, "
		      "3 failed the CRC and read again, 1 of those then good; "
		      "1 kept by a trial mark's CRC; its CRC by the peripheral"));
	/* the faster read's way: by DMA whole, or the fall-back and why */
	t.flags &= ~BOOT_TIME_SLOT_CRC_HW;
	t.slot_dma = BOOT_TIME_SLOT_DMA_WHOLE;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 15u,
		      "slot read at the faster rate: 1 whole, 2 failed and read again, "
		      "3 failed the CRC and read again, 1 of those then good; "
		      "1 kept by a trial mark's CRC; by DMA: whole"));
	t.slot_dma = BOOT_TIME_SLOT_DMA_UNMOVED | 8u << BOOT_TIME_SLOT_DRAINED_SHIFT;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 15u,
		      "slot read at the faster rate: 1 whole, 2 failed and read again, "
		      "3 failed the CRC and read again, 1 of those then good; "
		      "1 kept by a trial mark's CRC; by DMA: fell back to the core, "
		      "done without the words moved, 8 words read off the receive FIFO"));
	t.slot_dma = BOOT_TIME_SLOT_DMA_CRC;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 15u,
		      "slot read at the faster rate: 1 whole, 2 failed and read again, "
		      "3 failed the CRC and read again, 1 of those then good; "
		      "1 kept by a trial mark's CRC; by DMA: fell back to the core, "
		      "its CRC failed"));
	t.slot_dma = 0x7Fu;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 15u,
		      "slot read at the faster rate: 1 whole, 2 failed and read again, "
		      "3 failed the CRC and read again, 1 of those then good; "
		      "1 kept by a trial mark's CRC; by DMA: fell back to the core, "
		      "a failure"));
	t.slot_dma = 0u;
	t.flags &= ~BOOT_TIME_SLOT_CRC_HW;
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 16u));	/* no power reported */
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 17u));	/* no hand-over begun */
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 18u));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 19u));	/* no held mode */
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 20u));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 21u));	/* no mount */
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 22u));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 23u));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 24u));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 25u));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 26u, "end of report"));
	CHECK(line_empty(&t, BOOT_TIME_LINES));
	CHECK(line_of(&t, "end of report") == BOOT_TIME_LINES - 1u);
}

/*
 * The ready line watched after each byte, against a slave that raises it
 * `rise` cycles after a byte's end and holds it for `hold` (rise < 0:
 * never). Samples every 40 cycles through the window, then, as the stream
 * does, until the line reads low.
 */
static void watch_byte(struct boot_time *t, struct boot_time_watch *w, uint32_t *sent,
		       long rise, long hold)
{
	const uint64_t window = t->line_window;
	uint64_t at;

	boot_time_watch_byte(w, (*sent)++);
	for (at = 20u; ; at += 40u) {
		const bool high = rise >= 0 && at >= (uint64_t)rise &&
				  at < (uint64_t)rise + (uint64_t)hold;

		boot_time_watch_sample(t, w, high, at, at <= window);
		if (at > window && !high)
			break;
	}
}

static void test_watch(void)
{
	struct boot_time t = fresh();
	struct boot_time_watch w;
	uint32_t sent = 0;
	unsigned i;

	memset(&w, 0, sizeof w);
	t.line_window = (uint32_t)US(2);		/* 800 cycles, steps of 100 */
	/* before any byte a sample counts for nothing */
	boot_time_watch_sample(&t, &w, true, 5u, true);
	CHECK(t.line_high_first == 0u && t.line_late == 0u);

	/* never raised: nothing */
	for (i = 0; i < 10u; i++)
		watch_byte(&t, &w, &sent, -1, 0);
	CHECK(t.line_high_first == 0u && t.line_late == 0u && t.line_hold_max == 0u);

	/* raised before the first sample: high at once, in the first step */
	for (i = 0; i < 7u; i++)
		watch_byte(&t, &w, &sent, 10, 300);
	CHECK(t.line_high_first == 7u && t.line_late == 0u);
	CHECK(t.line_raise[0] == 7u);
	CHECK(t.line_hold_max == 320u);			/* 20 to 340, by the samples */

	/* raised late, inside the window: seen at the first sample after it */
	for (i = 0; i < 3u; i++)
		watch_byte(&t, &w, &sent, 450, 100);
	watch_byte(&t, &w, &sent, 610, 4000);			/* and held past the window */
	CHECK(t.line_high_first == 7u && t.line_late == 4u);
	CHECK(t.line_late_max == 620u);
	CHECK(t.line_raise[4] == 3u && t.line_raise[6] == 1u);
	CHECK(t.line_hold_max == 4000u);		/* 620 to 4620 */

	/* raised after the window: not a raise the window saw */
	watch_byte(&t, &w, &sent, 900, 200);
	CHECK(t.line_high_first == 7u && t.line_late == 4u);
	for (i = 0; i < BOOT_TIME_RAISE_STEPS; i++)
		CHECK(t.line_raise[i] == (i == 0u ? 7u : i == 4u ? 3u : i == 6u ? 1u : 0u));

	/* a raise at the window's very end falls in the last step */
	watch_byte(&t, &w, &sent, 770, 100);
	CHECK(t.line_late == 5u && t.line_raise[7] == 1u);

	boot_time_mark(&t, BOOT_TIME_STREAM_END, US(900000));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 4u,
		      "ready line after a byte: 7 high at the first sample, 5 raised later, "
		      "the latest at 1.95 us, longest hold 10.00 us"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 5u,
		      "raises in 8 steps of 0.25 us: 7 0 0 0 3 0 1 1"));
	/* where they were: the slot's bytes sent by then (10 went first) */
	CHECK(line_is(&t, BOOT_TIME_COUNT + 6u,
		      "first high at the first sample, after slot bytes: 10 11 12 13 14 15 16"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 7u,
		      "first late raises, after slot bytes: 17 18 19 20 22"));
	/* only the first of each are kept, the counts go on */
	for (i = 0; i < 40u; i++)
		watch_byte(&t, &w, &sent, i % 2u ? 10 : 450, 100);
	CHECK(t.line_high_first == 27u && t.line_late == 25u);
	CHECK(t.line_high_at[BOOT_TIME_FIRST - 1u] == 40u);	/* 7, then nine more */
	CHECK(t.line_late_at[BOOT_TIME_FIRST - 1u] == 43u);	/* 5, then eleven more */
	CHECK(line_is(&t, BOOT_TIME_COUNT + 7u,
		      "first late raises, after slot bytes: 17 18 19 20 22 23 25 27 29 31 33 "
		      "35 37 39 41 43"));
}

/* ---- the stream's split ---- */

/* An LDR block's header: the code, with 0xAD in its top byte, and a count. */
static size_t put_block(uint8_t *at, uint32_t flags, uint32_t count, bool payload)
{
	size_t i;

	memset(at, 0, 16);
	s06_wr_le32(at, 0xAD000001u | flags);
	s06_wr_le32(at + 8, count);
	if (!payload)
		return 16u;
	for (i = 0; i < count; i++)
		at[16 + i] = (uint8_t)(i * 7u);
	return 16u + count;
}

/* A stream of five blocks: an ordinary one, a fill, one with a count of 0,
 * one in three pieces (S-09.005), and the final one. */
static size_t five_blocks(uint8_t *slot)
{
	size_t n = 0;

	n += put_block(slot + n, 0u, 20u, true);
	n += put_block(slot + n, 0x0100u, 64u, false);
	n += put_block(slot + n, 0u, 0u, false);
	n += put_block(slot + n, 0u, 2500u, true);
	n += put_block(slot + n, 0x8000u, 4u, true);
	return n;
}

/* The board's stream ops, as opxy_timing.c counts them, against a slave that
 * is always ready and answers the opening byte. */
struct fake_split {
	struct boot_time t;
	struct boot_time_walk w;
	bool begun;
	uint32_t pieces;	/* sessions the stream made, by its own count */
};

static bool fs_ready(void *ctx)
{
	(void)ctx;
	return true;
}

static void fs_select(void *ctx, bool on)
{
	struct fake_split *f = ctx;

	if (on && f->begun)
		f->t.split.sessions++;
}

static uint8_t fs_xfer(void *ctx, uint8_t tx)
{
	struct fake_split *f = ctx;

	boot_time_xfer(&f->t, 100u);
	if (!f->begun) {
		f->begun = true;
		return 0xFF;
	}
	boot_time_walk_byte(&f->t, &f->w, tx);
	return 0;
}

static bool fs_send(void *ctx, const uint8_t *d, size_t n)
{
	struct fake_split *f = ctx;

	boot_time_walk(&f->t, &f->w, d, (uint32_t)n);
	f->pieces++;
	return true;
}

/* A clock that moves only when idled: the slave here never holds a byte. */
static uint32_t fake_us;

static uint32_t fake_now(void *ctx)
{
	(void)ctx;
	return fake_us;
}

static void fake_idle(void *ctx, uint32_t until)
{
	(void)ctx;
	fake_us = until;
}

static void test_walk(void)
{
	static uint8_t slot[4096];
	static struct fake_split f;
	const struct s06_clock clock = { fake_now, fake_idle, NULL };
	struct s09_boot_ops ops = { &f, fs_ready, fs_select, fs_xfer, NULL, NULL };
	const size_t len = five_blocks(slot);
	int end;

	/* byte by byte */
	memset(&f, 0, sizeof f);
	f.t.split.xfer_min = UINT32_MAX;
	CHECK(s09_boot_slave(&ops, &clock, slot, len, true, &end) == S09_BOOT_STREAMED);
	CHECK(end == S09_END_FINAL);
	CHECK(f.t.split.blocks == 5u);
	CHECK(f.t.split.fills == 2u);			/* the fill and the count of 0 */
	CHECK(f.t.split.sessions == 7u);		/* 1, 1, 1, 3 and 1 */
	CHECK(f.t.split.bytes == len && len == 2604u);
	CHECK(f.t.split.xfers == len + 1u);		/* and the opening byte */
	CHECK(f.t.split.xfer == 100u * (len + 1u));
	CHECK(f.t.split.xfer_min == 100u && f.t.split.xfer_max == 100u);
	CHECK(f.w.have == 0u && f.w.left == 0u);	/* ended on a block's end */

	/* paced by the controller: a session's bytes at once */
	memset(&f, 0, sizeof f);
	ops.send = fs_send;
	CHECK(s09_boot_slave(&ops, &clock, slot, len, true, &end) == S09_BOOT_STREAMED);
	CHECK(f.t.split.blocks == 5u && f.t.split.fills == 2u);
	CHECK(f.t.split.sessions == 7u && f.pieces == 7u);
	CHECK(f.t.split.bytes == 2604u);
	CHECK(f.t.split.xfers == 1u);			/* the opening byte alone */

	/* a run that splits a header is counted as the bytes one at a time */
	memset(&f, 0, sizeof f);
	boot_time_walk(&f.t, &f.w, slot, 7u);
	CHECK(f.t.split.blocks == 1u && f.w.have == 7u);
	boot_time_walk(&f.t, &f.w, slot + 7, 30u);
	CHECK(f.t.split.blocks == 2u && f.w.have == 1u && f.t.split.bytes == 37u);
}

static void test_gaps(void)
{
	static const uint32_t waits[] = { 5, 9, 1, 9, 12, 0, 3, 7, 8, 2, 10, 4 };
	static const uint32_t want[BOOT_TIME_GAPS][2] = {
		{ 12, 4 }, { 10, 10 }, { 9, 1 }, { 9, 3 }, { 8, 8 }, { 7, 7 }, { 5, 0 }, { 4, 11 },
	};
	struct boot_time t = fresh();
	unsigned i;

	for (i = 0; i < sizeof waits / sizeof waits[0]; i++) {
		t.split.blocks = i;		/* none begun before the first */
		boot_time_gap(&t, waits[i], 100u * i);
	}
	for (i = 0; i < BOOT_TIME_GAPS; i++) {
		const uint32_t n = want[i][1];

		CHECK(t.split.gap[i].cycles == want[i][0]);
		CHECK(t.split.gap[i].at == 100u * n);
		CHECK(t.split.gap[i].block == (n == 0u ? BOOT_TIME_NO_BLOCK : n - 1u));
	}
	/* one no longer than the shortest kept changes nothing */
	boot_time_gap(&t, 4u, 99u);
	CHECK(t.split.gap[BOOT_TIME_GAPS - 1u].at == 1100u);
	/* a count past 32 bits is kept as the most a word holds */
	boot_time_gap(&t, (uint64_t)1 << 40, 5u);
	CHECK(t.split.gap[0].cycles == UINT32_MAX && t.split.gap[1].cycles == 12u);
}

/* The split's lines over a stream of 3000 ms: its parts and the rest add up
 * to the whole. */
static struct boot_time split_stream(void)
{
	struct boot_time t = fresh();
	unsigned i;

	boot_time_mark(&t, BOOT_TIME_SLOT_FOUND, US(500000));
	boot_time_mark(&t, BOOT_TIME_STREAM_WAIT, US(500100));
	boot_time_mark(&t, BOOT_TIME_STREAM_READY, US(500110));
	boot_time_mark(&t, BOOT_TIME_STREAM_OPENED, US(500111));
	boot_time_mark(&t, BOOT_TIME_STREAM_HEADER, US(500120));
	boot_time_mark(&t, BOOT_TIME_STREAM_LAST, US(3500000));
	boot_time_mark(&t, BOOT_TIME_STREAM_END, US(3500100));
	t.stream_high = 31u;
	t.stream_wait = US(500000);
	t.split.xfers = 421377u;
	t.split.xfer = (uint64_t)421377u * 120u;		/* 0.30 us a transfer */
	t.split.xfer_min = 110u;
	t.split.xfer_max = 2000u;
	t.split.window = US(2106880);
	t.split.sample = US(30000);
	t.split.select = US(1000);
	t.split.bytes = 421376u;
	t.split.blocks = 29u;
	t.split.fills = 3u;
	t.split.sessions = 435u;
	t.split.wire_byte = 96u;
	t.split.read = 6u;
	t.split.reads = 843000u;
	for (i = 0; i < 5u; i++) {
		t.split.gap[i].cycles = (uint32_t)US(200000 - 1000 * i);
		t.split.gap[i].at = 16u + 4096u * i;
		t.split.gap[i].block = i == 0u ? BOOT_TIME_NO_BLOCK : 2u * i;
	}
	return t;
}

static void test_split(void)
{
	struct boot_time t = split_stream();

	/* the steps in time order, among the other milestones */
	CHECK(line_is(&t, 0u, "17 slot found: 500.000 ms"));
	CHECK(line_is(&t, 1u, "30 stream ready wait: 500.100 ms, +0.100 ms"));
	CHECK(line_is(&t, 2u, "31 stream ready: 500.110 ms, +0.010 ms"));
	CHECK(line_is(&t, 3u, "32 stream opened: 500.111 ms, +0.001 ms"));
	CHECK(line_is(&t, 4u, "33 stream first header: 500.120 ms, +0.009 ms"));
	CHECK(line_is(&t, 5u, "34 stream last byte: 3500.000 ms, +2999.880 ms"));
	CHECK(line_is(&t, 6u, "18 stream end: 3500.100 ms, +0.100 ms"));

	CHECK(line_is(&t, BOOT_TIME_COUNT + 8u,
		      "stream split: 3000.000 ms in all, ready wait 0.010 ms, transfers 126.413 ms, "
		      "windows 2106.880 ms, samples 30.000 ms"));
	/* 3000 - 0.010 - 126.413 - 2106.880 - 30 - 500 - 1 */
	CHECK(line_is(&t, BOOT_TIME_COUNT + 9u,
		      "stream split: waits on a high line 500.000 ms, chip select 1.000 ms, "
		      "the rest 235.696 ms"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 10u,
		      "stream blocks: 29 blocks, 3 fill or header-only, 435 sessions, "
		      "421376 bytes"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 11u,
		      "stream bytes: 421377 transfers less a counter read, avg 0.28 us, "
		      "min 0.26 us, max 4.98 us; on the wire alone 101.130 ms, "
		      "in this build 126.413 ms"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 12u,
		      "stream longest waits: 200.000 ms at byte 16, no block yet; "
		      "199.000 ms at byte 4112, block 2; 198.000 ms at byte 8208, block 4; "
		      "197.000 ms at byte 12304, block 6"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 13u,
		      "stream longest waits, more: 196.000 ms at byte 16400, block 8"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 14u,
		      "stream measuring: 843000 counter reads added, 6 cycles a read, "
		      "at most 12.645 ms"));
	/* fewer waits: no second line; none: no first */
	t.split.gap[4].cycles = 0u;
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 13u));
	t.split.gap[0].cycles = 0u;
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 12u));

	/* parts beyond the whole show a negative rest, not a wrapped one */
	t.split.select = US(300000);
	CHECK(line_is(&t, BOOT_TIME_COUNT + 9u,
		      "stream split: waits on a high line 500.000 ms, chip select 300.000 ms, "
		      "the rest -63.303 ms"));

	/* without the wait's stamps (an older stream, or none) no split */
	t = split_stream();
	t.set &= ~((uint64_t)1 << BOOT_TIME_STREAM_WAIT);
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 8u));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 14u));
}

/* Paced by the controller: sessions, not bytes, and the waits inside them. */
static void test_split_fc(void)
{
	struct boot_time t = split_stream();

	t.flags = BOOT_TIME_LINK_FC;
	t.split.xfers = 1u;
	t.split.xfer = 200u;
	t.split.window = 0u;
	t.split.send = US(2900000);
	t.split.send_max = (uint32_t)US(150000);
	t.stream_wait = US(2798870);
	t.split.sample = US(50);
	t.split.reads = 1310u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 8u,
		      "stream split: 3000.000 ms in all, ready wait 0.010 ms, transfers 0.000 ms, "
		      "sends 2900.000 ms, samples 0.050 ms"));
	/* the waits are inside the sends: 3000 - 0.010 - 0.0005 - 2900 - 0.05 - 1 */
	CHECK(line_is(&t, BOOT_TIME_COUNT + 9u,
		      "stream split: beyond the wire in the sends 2798.870 ms, "
		      "chip select 1.000 ms, the rest 98.939 ms"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 11u,
		      "stream sends: 435 sessions in 2900.000 ms, the longest 150.000 ms; "
		      "on the wire alone 101.130 ms"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 14u,
		      "stream measuring: 1310 counter reads added, 6 cycles a read, "
		      "at most 0.019 ms"));
	/* the watch's lines stay out of it */
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 4u));
}

static void test_record(void)
{
	struct boot_time t = fresh(), back;
	uint8_t rec[BOOT_TIME_RECORD];
	uint32_t mhz = 0;
	unsigned i;

	CHECK(BOOT_TIME_RECORD == 182u * 4u + 8u * BOOT_TIME_COUNT + 4u);
	CHECK(BOOT_TIME_COUNT == 59u);
	for (i = 0; i < BOOT_TIME_COUNT; i += 3u)
		boot_time_mark(&t, i, 0x0102030405060708ull * (i + 1u));
	t.bytes = 603112u;
	t.flags = BOOT_TIME_GREETED | BOOT_TIME_CHARGE_WAITED;
	t.stream_high = 77u;
	t.stream_wait = 0x0000000A0B0C0D0Eull;
	t.line_window = 800u;
	t.line_high_first = 11u;
	t.line_late = 22u;
	t.line_late_max = 333u;
	t.line_hold_max = 4444u;
	for (i = 0; i < BOOT_TIME_RAISE_STEPS; i++)
		t.line_raise[i] = 100u + i;
	for (i = 0; i < BOOT_TIME_FIRST; i++) {
		t.line_high_at[i] = 1000u + i;
		t.line_late_at[i] = 2000u + i;
	}
	t.slot_fast = 5u;
	t.slot_fast_failed = 6u;
	t.slot_reread = 7u;
	t.slot_reread_good = 8u;
	t.slot_trial = 9u;
	t.slot_dma = 0x0803u;
	t.power_state = 0x10u;
	t.battery = 0x0FA05502u;
	t.battery_ma = 812u;
	t.copy_link = 0x00040101u;
	t.copy_fell = 2u;
	t.copy_quad = 37u;
	t.copy_qbytes = 2359296u;
	t.copy_qstate = 0x00000302u;
	t.copy_qtest_us = 15600u;
	t.mount_rc = 0xFFFFFFFFu;
	t.mount_err = 19u;
	t.greet_tries = 1000u;
	t.mount.valid = BOOT_TIME_MOUNT_MAGIC;
	t.mount.cpu = 400u;
	t.mount.mount_us = 4500000u;
	t.mount.reads = 1234u;
	t.mount.failed = 1u;
	t.mount.read_us = 4200000u;
	t.mount.read_min = 40000u;
	t.mount.read_max = 3600000u;
	t.mount.tags_reads = 700u;
	t.mount.tags_us = 70000u;
	t.mount.whole_reads = 534u;
	t.mount.fell_back = 2u;
	t.mount.allocs = 900u;
	t.mount.frees = 12u;
	t.mount.alloc_us = 3000u;
	t.mount.glue_us = 4000u;
	t.mount.dctl = 0x0000000Eu;
	t.mount.ictl = 0x00000006u;
	t.mount.dcplb_dflt = 0x10u;
	t.mount.icplb_dflt = 0x20u;
	t.mount.heap_lo = 0x88400000u;
	t.mount.heap_hi = 0x89F00000u;
	t.mount.heap_cplb = 0x0005D01Fu;
	t.mount.static_at = 0x88100000u;
	t.mount.static_cplb = 0x0005D01Fu;
	t.mount.code_at = 0x88050000u;
	t.mount.code_cplb = 0x80001003u;
	t.mount.way = 1u;
	t.mount.objects = 1007u;
	t.mount.restore_us = 171531u;
	t.mount.restore_reads = 72u;
	t.mount.check_us = 320000u;
	t.mount.check_reads = 4736u;
	t.mount.scan_us = 7u;
	t.mount.scan_reads = 8u;
	for (i = 0; i < BOOT_TIME_PHASES; i++) {
		t.mount.phase_us[i] = 10000u * (i + 1u);
		t.mount.phase_min[i] = 100u + i;
		t.mount.phase_max[i] = 9000u + i;
	}
	/* milestones past bit 31 of set */
	boot_time_mark(&t, BOOT_TIME_STREAM_LAST, 0x1122334455667788ull);
	boot_time_mark(&t, BOOT_TIME_KERNEL_CALL, 0x2122334455667788ull);
	boot_time_mark(&t, BOOT_TIME_MENU_SHOWN, 0x3122334455667788ull);
	t.split.xfer = 0x0000000100000002ull;
	t.split.window = 0x0000000300000004ull;
	t.split.sample = 5u;
	t.split.select = 0x0000000600000007ull;
	t.split.send = 8u;
	t.split.bytes = 421360u;
	t.split.blocks = 30u;
	t.split.fills = 4u;
	t.split.sessions = 440u;
	t.split.xfers = 421361u;
	t.split.xfer_min = 100u;
	t.split.xfer_max = 900u;
	t.split.send_max = 12u;
	t.split.wire_byte = 96u;
	t.split.read = 3u;
	t.split.reads = 1000000u;
	for (i = 0; i < BOOT_TIME_GAPS; i++) {
		t.split.gap[i].cycles = 5000u - i;
		t.split.gap[i].at = 16u * i;
		t.split.gap[i].block = i;
	}
	boot_time_pack(&t, MHZ, rec);
	CHECK(rec[0] == 'B' && rec[1] == 'T' && rec[2] == 'I' && rec[3] == 'M');
	memset(&back, 0xA5, sizeof back);
	CHECK(boot_time_unpack(rec, &back, &mhz));
	CHECK(mhz == MHZ);
	CHECK(back.set == t.set && back.bytes == t.bytes && back.flags == t.flags);
	CHECK(back.stream_high == t.stream_high && back.stream_wait == t.stream_wait);
	CHECK(back.line_window == 800u && back.line_high_first == 11u && back.line_late == 22u);
	CHECK(back.line_late_max == 333u && back.line_hold_max == 4444u);
	CHECK(memcmp(back.line_raise, t.line_raise, sizeof t.line_raise) == 0);
	CHECK(memcmp(back.line_high_at, t.line_high_at, sizeof t.line_high_at) == 0);
	CHECK(memcmp(back.line_late_at, t.line_late_at, sizeof t.line_late_at) == 0);
	CHECK(back.slot_fast == 5u && back.slot_fast_failed == 6u && back.slot_reread == 7u &&
	      back.slot_reread_good == 8u && back.slot_trial == 9u && back.slot_dma == 0x0803u);
	CHECK(back.power_state == 0x10u && back.battery == 0x0FA05502u && back.battery_ma == 812u &&
	      back.copy_link == 0x00040101u && back.copy_fell == 2u);
	CHECK(back.copy_quad == 37u && back.copy_qbytes == 2359296u &&
	      back.copy_qstate == 0x00000302u && back.copy_qtest_us == 15600u);
	CHECK(back.mount_rc == 0xFFFFFFFFu && back.mount_err == 19u && back.greet_tries == 1000u);
	CHECK(memcmp(&back.mount, &t.mount, sizeof t.mount) == 0);
	CHECK(memcmp(back.at, t.at, sizeof t.at) == 0);
	/* the split's fields, not its tail padding */
	CHECK(back.split.xfer == t.split.xfer && back.split.window == t.split.window &&
	      back.split.sample == t.split.sample && back.split.select == t.split.select &&
	      back.split.send == t.split.send);
	CHECK(memcmp(&back.split.bytes, &t.split.bytes,
		     offsetof(struct boot_time_split, gap) + sizeof t.split.gap -
		     offsetof(struct boot_time_split, bytes)) == 0);
	CHECK(back.set >> BOOT_TIME_STREAM_LAST & 1u);
	CHECK(back.set >> BOOT_TIME_KERNEL_CALL & 1u);
	CHECK(back.set >> BOOT_TIME_MENU_SHOWN & 1u);

	/* any one byte changed is refused, the magic and the version included */
	for (i = 0; i < BOOT_TIME_RECORD; i++) {
		rec[i] ^= 0x10u;
		CHECK(!boot_time_unpack(rec, &back, &mhz));
		rec[i] ^= 0x10u;
	}
	/* a record of another layout, its check right, is refused by its version */
	rec[4] = (uint8_t)(BOOT_TIME_VERSION - 1u);
	CHECK(!boot_time_unpack(rec, &back, &mhz));
	/* memory a test OS cleared, or never written */
	memset(rec, 0, sizeof rec);
	CHECK(!boot_time_unpack(rec, &back, &mhz));
	memset(rec, 0xFF, sizeof rec);
	CHECK(!boot_time_unpack(rec, &back, &mhz));
}

/*
 * Fed by DMA (S-09.009, S-13b.023): lines 4 and 5 after the milestones give
 * the parts and their guards, then the fall-back, in place of the line after
 * a byte, which no byte shows there. The record carries them across.
 */
static void test_dma_lines(void)
{
	struct boot_time t = fresh(), back;
	uint8_t rec[BOOT_TIME_RECORD];
	uint32_t mhz = 0;

	boot_time_mark(&t, BOOT_TIME_STREAM_END, US(900000));
	t.flags = BOOT_TIME_LINK_FC;
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 4u));
	t.flags |= BOOT_TIME_LINK_DMA;
	t.dma_parts = 437u;
	t.dma_guard = 20u;
	t.dma_guard_us = 9180u;
	t.dma_high = 3u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 4u,
		      "stream by DMA: 437 parts, guard 20 us, 9.180 ms in the guards, "
		      "3 parts began with the ready line high; FCS set in 0 of 0 samples, 0 parts"));
	t.dma_fcs_samples = 91000u;
	t.dma_fcs_held = 2400u;
	t.dma_fcs_parts = 380u;
	t.dma_fcs_first = 17u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 4u,
		      "stream by DMA: 437 parts, guard 20 us, 9.180 ms in the guards, "
		      "3 parts began with the ready line high; FCS set in 2400 of 91000 samples, "
		      "380 parts, the first at byte 16"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 5u, "stream fall-back: none"));
	t.dma_held = 1u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 5u,
		      "stream fall-back: none; the slave held the line past T_STREAM"));
	t.dma_held = 0u;
	t.dma_fell = S09_PART_STALL;
	t.dma_fell_session = 7u;
	t.dma_fell_at = 6200u;
	t.dma_slow_bytes = 415176u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 5u,
		      "stream fall-back at byte 6200 of session 7: the controller stopped with "
		      "the line low, then 415176 bytes one at a time"));
	t.dma_fell = S09_PART_DMA;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 5u,
		      "stream fall-back at byte 6200 of session 7: a DMA error, "
		      "then 415176 bytes one at a time"));
	t.dma_fell = 99u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 5u,
		      "stream fall-back at byte 6200 of session 7: ?, then 415176 bytes one at a time"));
	t.dma_fell = S09_PART_NO_END;
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 6u));
	boot_time_pack(&t, MHZ, rec);
	CHECK(boot_time_unpack(rec, &back, &mhz));
	CHECK(back.dma_parts == 437u && back.dma_guard == 20u && back.dma_guard_us == 9180u &&
	      back.dma_high == 3u && back.dma_fell == S09_PART_NO_END && back.dma_fell_session == 7u &&
	      back.dma_fell_at == 6200u && back.dma_slow_bytes == 415176u && back.dma_held == 0u &&
	      back.dma_fcs_samples == 91000u && back.dma_fcs_held == 2400u &&
	      back.dma_fcs_parts == 380u && back.dma_fcs_first == 17u);
	CHECK(line_is(&back, BOOT_TIME_COUNT + 5u,
		      "stream fall-back at byte 6200 of session 7: no end of the transmit with the "
		      "line low, then 415176 bytes one at a time"));
}

/* The power at the start: the PSoC's state and its battery report. */
static void test_power(void)
{
	struct boot_time t = fresh();

	CHECK(line_empty(&t, BOOT_TIME_COUNT + 16u));
	t.flags = BOOT_TIME_POWER_KNOWN;
	t.power_state = 0x10u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 16u,
		      "power at start: state 0x10 (on), no battery report"));
	t.flags |= BOOT_TIME_BATTERY_KNOWN;
	t.battery = 2u | 85u << 8 | 4012u << 16;
	t.battery_ma = 512u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 16u,
		      "power at start: state 0x10 (on), battery status 0x02 (charging), "
		      "85 %, 4012 mV, 512 mA"));
	t.battery = 1u | 60u << 8 | 3900u << 16;
	t.power_state = 0x16u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 16u,
		      "power at start: state 0x16 (charging, switch off), battery status 0x01, "
		      "60 %, 3900 mV, 512 mA"));
	/* the current signed; on, not charging and full: battery or USB */
	t.power_state = 0x10u;
	t.battery = 0x10626401u;	/* 100 %, 4194 mV */
	t.battery_ma = 65445u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 16u,
		      "power at start: state 0x10 (on), battery status 0x01, 100 %, 4194 mV, -91 mA; "
		      "battery or USB at a full charge, not told apart"));
	t.battery = 0x10626301u;	/* 99 % */
	CHECK(line_is(&t, BOOT_TIME_COUNT + 16u,
		      "power at start: state 0x10 (on), battery status 0x01, 99 %, 4194 mV, -91 mA"));
	t.flags = BOOT_TIME_BATTERY_KNOWN;
	t.battery = 0u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 16u,
		      "power at start: no state reported, battery status 0x00 (none)"));
}

/* The start into the OS: the copy's link, and the spans from the
 * hand-over's start to the OS. */
static void test_os_start(void)
{
	struct boot_time t = fresh();

	CHECK(line_empty(&t, BOOT_TIME_COUNT + 17u));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 18u));
	boot_time_mark(&t, BOOT_TIME_HANDOFF, US(1000));
	boot_time_mark(&t, BOOT_TIME_PMIC, US(1200));
	boot_time_mark(&t, BOOT_TIME_BOOT_REPORT, US(1500));
	boot_time_mark(&t, BOOT_TIME_PRE_LOAD, US(1600));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 17u,
		      "os load split: pmic 0.200 ms, logo and report 0.300 ms, drain 0.100 ms"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 18u, "os start split:"));
	boot_time_mark(&t, BOOT_TIME_COPY_START, US(1700));
	boot_time_mark(&t, BOOT_TIME_COPY_LAST, US(501700));
	boot_time_mark(&t, BOOT_TIME_COPY_CRC, US(511700));
	boot_time_mark(&t, BOOT_TIME_STAGED, US(511800));
	boot_time_mark(&t, BOOT_TIME_CHECKED, US(511810));
	boot_time_mark(&t, BOOT_TIME_RELEASE_SENT, US(512810));
	boot_time_mark(&t, BOOT_TIME_RELEASED, US(612810));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 17u,
		      "os load split: pmic 0.200 ms, logo and report 0.300 ms, drain 0.100 ms, "
		      "reads 500.000 ms, crc 10.000 ms, user area 0.100 ms, check 0.010 ms"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 18u,
		      "os start split: log 1.000 ms, release 100.000 ms"));
	/* the CRC by the peripheral, and computed again after a miss */
	t.flags |= BOOT_TIME_COPY_CRC_HW;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 17u,
		      "os load split: pmic 0.200 ms, logo and report 0.300 ms, drain 0.100 ms, "
		      "reads 500.000 ms, crc 10.000 ms (peripheral), user area 0.100 ms, "
		      "check 0.010 ms"));
	t.flags |= BOOT_TIME_COPY_CRC_AGAIN;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 17u,
		      "os load split: pmic 0.200 ms, logo and report 0.300 ms, drain 0.100 ms, "
		      "reads 500.000 ms, crc 10.000 ms (peripheral, then software), "
		      "user area 0.100 ms, check 0.010 ms"));
	t.flags &= ~(BOOT_TIME_COPY_CRC_HW | BOOT_TIME_COPY_CRC_AGAIN);
	/* the hand-over frame not sent: the early logo kept */
	t.flags |= BOOT_TIME_LOGO_KEPT;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 17u,
		      "os load split: pmic 0.200 ms, logo and report 0.300 ms (the early logo kept), "
		      "drain 0.100 ms, reads 500.000 ms, crc 10.000 ms, user area 0.100 ms, "
		      "check 0.010 ms"));
	t.flags &= ~BOOT_TIME_LOGO_KEPT;
	boot_time_mark(&t, BOOT_TIME_SERVER_STOP, US(612850));
	boot_time_mark(&t, BOOT_TIME_SLAVE_OS, US(812850));
	t.flags |= BOOT_TIME_PA6_POLLED;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 18u,
		      "os start split: log 1.000 ms, release 100.000 ms, server stop 0.040 ms, "
		      "slave os 200.000 ms, in all 811.850 ms; PA6 polled in the line window"));
	t.flags &= ~BOOT_TIME_PA6_POLLED;
	boot_time_mark(&t, BOOT_TIME_TEARDOWN, US(812900));
	boot_time_mark(&t, BOOT_TIME_KERNEL_CALL, US(813400));
	boot_time_mark(&t, BOOT_TIME_OS_ENTRY, US(913400));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 18u,
		      "os start split: log 1.000 ms, release 100.000 ms, server stop 0.040 ms, "
		      "slave os 200.000 ms, teardown 0.500 ms, kernel to os 100.000 ms, "
		      "in all 912.400 ms"));
	/* the new milestones take their names */
	CHECK(line_of(&t, "36 pmic step done: 1.200 ms, +0.200 ms") < BOOT_TIME_COUNT);
	CHECK(line_of(&t, "38 os crc done: 511.700 ms") < BOOT_TIME_COUNT);
	CHECK(line_of(&t, "41 slave os started: 812.850 ms") < BOOT_TIME_COUNT);
	CHECK(line_of(&t, "43 kernel call: 813.400 ms, +0.500 ms") < BOOT_TIME_COUNT);
	/* the slave's beacons: its teardown, kernel load, entry and OS start */
	boot_time_mark(&t, BOOT_TIME_SLAVE_TEARDOWN, US(612900));
	boot_time_mark(&t, BOOT_TIME_SLAVE_KERNEL, US(613850));
	boot_time_mark(&t, BOOT_TIME_SLAVE_RETURNED, US(783850));
	boot_time_mark(&t, BOOT_TIME_SLAVE_JUMP, US(783950));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 18u,
		      "os start split: log 1.000 ms, release 100.000 ms, server stop 0.040 ms, "
		      "slave os 200.000 ms (its teardown 1.000 ms, kernel load 170.000 ms, "
		      "entry 0.100 ms, os to PA4 high 28.900 ms), teardown 0.500 ms, "
		      "kernel to os 100.000 ms, in all 912.400 ms"));
	CHECK(line_of(&t, "45 slave teardown begun: 612.900 ms") < BOOT_TIME_COUNT);
	CHECK(line_of(&t, "48 slave jump: 783.950 ms") < BOOT_TIME_COUNT);

	/* the copy's link: its rung at the start and the end, the ceiling and
	 * the fall-backs */
	t.bytes = 1000000u;
	t.flags |= BOOT_TIME_COPY_LINK;
	t.copy_link = 0x00040101u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 2u,
		      "os copy 1000000 bytes in 500.000 ms, 2000 kB/s, link rung 1 (checked), "
		      "ceiling 4, 0 fall-backs, replies single I/O"));
	t.copy_link = 0x00040003u;
	t.copy_fell = 1u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 2u,
		      "os copy 1000000 bytes in 500.000 ms, 2000 kB/s, link rung 3 (quad) to rung "
		      "0 (plain), ceiling 4, 1 fall-backs, replies single I/O"));
	/* S-09.081: the copy's quad data replies: taken up, then dropped, not
	 * taken up, the CRC's read again */
	t.copy_link = 0x00020202u;
	t.copy_fell = 0u;
	t.copy_quad = 36u;
	t.copy_qbytes = 2359296u;
	t.copy_qstate = 4u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 2u,
		      "os copy 1000000 bytes in 500.000 ms, 2000 kB/s, link rung 2 (checked at 50 MHz), "
		      "ceiling 2, 0 fall-backs, sector data in quad I/O: 36 sessions, 2359296 bytes"));
	t.copy_quad = 3u;
	t.copy_qbytes = 196608u;
	t.copy_qstate = 0x0603u;
	t.copy_fell = 1u;
	t.flags |= BOOT_TIME_COPY_REREAD;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 2u,
		      "os copy 1000000 bytes in 500.000 ms, 2000 kB/s, link rung 2 (checked at 50 MHz), "
		      "ceiling 2, 1 fall-backs, sector data in quad I/O: 3 sessions, 196608 bytes, "
		      "then single I/O (why 6), its CRC failed: read again in single I/O"));
	t.flags &= ~BOOT_TIME_COPY_REREAD;
	t.copy_qstate = 0x00030201u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 2u,
		      "os copy 1000000 bytes in 500.000 ms, 2000 kB/s, link rung 2 (checked at 50 MHz), "
		      "ceiling 2, 1 fall-backs, replies single I/O, quad data not taken up (why 2, "
		      "test 0x03)"));
	/* the link's raise before the copy has its milestone, and its span
	 * with the quad reply test's time in it */
	boot_time_mark(&t, BOOT_TIME_LINK_UP, US(1650));
	CHECK(line_of(&t, "44 copy link up") < BOOT_TIME_COUNT);
	CHECK(line_is(&t, BOOT_TIME_COUNT + 17u,
		      "os load split: pmic 0.200 ms, logo and report 0.300 ms, drain 0.100 ms, "
		      "link up 0.050 ms, reads 500.000 ms, crc 10.000 ms, user area 0.100 ms, "
		      "check 0.010 ms"));
	t.copy_qtest_us = 20u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 17u,
		      "os load split: pmic 0.200 ms, logo and report 0.300 ms, drain 0.100 ms, "
		      "link up 0.050 ms (quad reply test 0.020 ms), reads 500.000 ms, crc 10.000 ms, "
		      "user area 0.100 ms, check 0.010 ms"));
}

/* Held mode's start to its first frame, in spans from the step reached
 * before each, and the slave's mount (struct boot_time_mount). */
static void test_boot_menu(void)
{
	struct boot_time t = fresh();
	unsigned i;

	CHECK(line_empty(&t, BOOT_TIME_COUNT + 19u));
	boot_time_mark(&t, BOOT_TIME_HELD, US(6000000));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 19u, "boot menu split:"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 20u, "boot menu split, links and frame:"));
	boot_time_mark(&t, BOOT_TIME_MENU_LINE, US(6000300));
	boot_time_mark(&t, BOOT_TIME_MOUNT_SENT, US(6001300));
	boot_time_mark(&t, BOOT_TIME_MOUNT_REPLY, US(10501300));
	boot_time_mark(&t, BOOT_TIME_SETTINGS, US(10621300));
	boot_time_mark(&t, BOOT_TIME_USB, US(10631300));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 19u,
		      "boot menu split: line 0.300 ms (not drawn), to mount 1.000 ms, "
		      "mount 4500.000 ms, settings 120.000 ms, usb 10.000 ms"));
	t.flags |= BOOT_TIME_LINE_DRAWN;
	t.mount_rc = (uint32_t)-1;
	t.mount_err = 5u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 19u,
		      "boot menu split: line 0.300 ms (drawn), to mount 1.000 ms, "
		      "mount 4500.000 ms (failed, error 5), settings 120.000 ms, usb 10.000 ms"));
	t.mount_rc = 0u;
	boot_time_mark(&t, BOOT_TIME_NORDIC_GREETING, US(10636300));
	t.greet_tries = 1u;
	t.flags |= BOOT_TIME_NORDIC_ANSWERED;
	boot_time_mark(&t, BOOT_TIME_PSOC_INFO, US(10637300));
	boot_time_mark(&t, BOOT_TIME_NORDIC_INFO, US(10640300));
	boot_time_mark(&t, BOOT_TIME_MENU_FRAME, US(10660300));
	boot_time_mark(&t, BOOT_TIME_MENU_SHOWN, US(10675300));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 20u,
		      "boot menu split, links and frame: nordic greeting 5.000 ms (1 tries, "
		      "answered), psoc info 1.000 ms, nordic info 3.000 ms, to the frame "
		      "20.000 ms, frame 15.000 ms, in all 4675.300 ms"));
	t.flags &= ~BOOT_TIME_NORDIC_ANSWERED;
	t.greet_tries = 1000u;
	CHECK(line_of(&t, "boot menu split, links and frame: nordic greeting 5.000 ms "
			  "(1000 tries, not answered)") == BOOT_TIME_COUNT + 20u);
	CHECK(line_of(&t, "49 loading line: 6000.300 ms, +0.300 ms") < BOOT_TIME_COUNT);
	CHECK(line_of(&t, "58 first menu frame ended: 10675.300 ms") < BOOT_TIME_COUNT);

	/* no second processor: no mount, each span from the step before */
	t = fresh();
	boot_time_mark(&t, BOOT_TIME_HELD, US(1000));
	boot_time_mark(&t, BOOT_TIME_MENU_LINE, US(1100));
	boot_time_mark(&t, BOOT_TIME_SETTINGS, US(1200));
	boot_time_mark(&t, BOOT_TIME_USB, US(1300));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 19u,
		      "boot menu split: line 0.100 ms (not drawn), settings 0.100 ms, "
		      "usb 0.100 ms, no mount"));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 21u));

	/* the slave's figures, or their absence after a mount */
	boot_time_mark(&t, BOOT_TIME_MOUNT_REPLY, US(1150));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 21u, "slave mount: no figures from the slave"));
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 22u));
	t.flags |= BOOT_TIME_MOUNT_READ;
	t.mount.valid = BOOT_TIME_MOUNT_MAGIC;
	t.mount.cpu = 400u;
	t.mount.mount_us = 4500000u;
	t.mount.reads = 1000u;
	t.mount.failed = 0u;
	t.mount.read_us = 4200000u;
	t.mount.read_min = 400u * 1000u;	/* 1 ms */
	t.mount.read_max = 400u * 9000u;
	for (i = 0; i < BOOT_TIME_PHASES; i++) {
		t.mount.phase_us[i] = 1000u * (i + 1u);	/* means of 1 to 5 us */
		t.mount.phase_min[i] = 200u;		/* 0.50 us */
		t.mount.phase_max[i] = 4000u;		/* 10.00 us */
	}
	CHECK(line_is(&t, BOOT_TIME_COUNT + 21u,
		      "slave mount: 4500.000 ms in all, 1000 chunk reads in 4200.000 ms, a read "
		      "min/mean/max 1000.00/4200.00/9000.00 us, 0 failed; 0 tags only in 0.000 ms, "
		      "0 whole, 0 fell back"));
	t.mount.tags_reads = 600u;
	t.mount.tags_us = 60000u;
	t.mount.whole_reads = 398u;
	t.mount.fell_back = 2u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 21u,
		      "slave mount: 4500.000 ms in all, 1000 chunk reads in 4200.000 ms, a read "
		      "min/mean/max 1000.00/4200.00/9000.00 us, 0 failed; 600 tags only in 60.000 ms, "
		      "398 whole, 2 fell back"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 22u,
		      "slave mount, a read's phases min/mean/max: cache 0.50/1.00/10.00 us, "
		      "set-up 0.50/2.00/10.00 us, command 0.50/3.00/10.00 us, "
		      "data 0.50/4.00/10.00 us, stop 0.50/5.00/10.00 us, the rest mean 4185.00 us"));
	/* outside the reads, and the caches at the mount's start */
	t.mount.allocs = 900u;
	t.mount.frees = 12u;
	t.mount.alloc_us = 3000u;
	t.mount.glue_us = 4000u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 23u,
		      "slave mount, outside the reads: 300.000 ms; 900 allocations and 12 frees in "
		      "3.000 ms, the tags reads' own 4.000 ms, the rest (yaffs2's scan) 293.000 ms"));
	t.mount.dctl = 0x0000000Eu;
	t.mount.ictl = 0x00000006u;
	t.mount.heap_lo = 0x88400000u;
	t.mount.heap_hi = 0x89F00000u;
	t.mount.heap_cplb = 0x0005D01Fu;
	t.mount.static_at = 0x88100000u;
	t.mount.static_cplb = 0x0005101Du | BOOT_TIME_CPLB_DFLT;
	t.mount.code_at = 0x88050000u;
	t.mount.code_cplb = 0x00051003u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 24u,
		      "slave mount, caches: L1DM_DCTL 0000000e (on), L1IM_ICTL 00000006 (on); heap "
		      "88400000-89f00000 0005d01f (write-through), static 88100000 0005101d "
		      "(write-back, default), code 88050000 00051003 (cached)"));
	t.mount.dctl = 0u;
	t.mount.heap_cplb = 0x0005001Eu;
	CHECK(line_of(&t, "slave mount, caches: L1DM_DCTL 00000000 (off), L1IM_ICTL 00000006 (on); "
			  "heap 88400000-89f00000 0005001e (invalid)") == BOOT_TIME_COUNT + 24u);
	/* how it went (S-12.061): the restore, the stale check, a scan after */
	t.mount.way = 1u;
	t.mount.objects = 1007u;
	t.mount.restore_us = 171531u;
	t.mount.restore_reads = 72u;
	t.mount.check_us = 320000u;
	t.mount.check_reads = 4736u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 25u,
		      "slave mount, how: restored, 1007 objects; restore 171.531 ms (72 reads), "
		      "stale check 320.000 ms (4736 reads), scan 0.000 ms (0 reads)"));
	t.mount.way = 4u;
	t.mount.scan_us = 4600000u;
	t.mount.scan_reads = 25943u;
	CHECK(line_of(&t, "slave mount, how: scanned, the checkpoint stale, 1007 objects; restore "
			  "171.531 ms (72 reads), stale check 320.000 ms (4736 reads), scan "
			  "4600.000 ms (25943 reads)") == BOOT_TIME_COUNT + 25u);
	/* within a log line */
	{
		struct s14_text out;

		for (i = BOOT_TIME_COUNT + 19u; i <= BOOT_TIME_COUNT + 25u; i++) {
			CHECK(boot_time_line(&t, i, MHZ, &out) || i == BOOT_TIME_COUNT + 20u);
			CHECK(!out.overflow && out.len < S14_LINE_MAX - 16u);
		}
	}
}

int main(void)
{
	RUN(test_mark);
	RUN(test_milestones);
	RUN(test_first_has_no_step);
	RUN(test_time_order);
	RUN(test_since_on);
	RUN(test_summary);
	RUN(test_watch);
	RUN(test_walk);
	RUN(test_gaps);
	RUN(test_split);
	RUN(test_split_fc);
	RUN(test_record);
	RUN(test_dma_lines);
	RUN(test_power);
	RUN(test_os_start);
	RUN(test_boot_menu);
	return check_summary("boot_time");
}
