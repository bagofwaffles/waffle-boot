/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The boot timing build's stamp store, its report lines and the record that
 * crosses a hand-over (waffle/boot_time.h).
 */
#include "support/check.h"

#include <string.h>

#include "waffle/boot_time.h"

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
	CHECK(line_empty(&t, BOOT_TIME_COUNT + 8u));
	t.slot_fast = 1u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 8u,
		      "slot read at the faster rate: 1 whole, 0 failed and read again, "
		      "0 failed the CRC and read again, 0 of those then good; "
		      "0 kept by a trial mark's CRC"));
	t.slot_fast_failed = 2u;
	t.slot_reread = 3u;
	t.slot_reread_good = 1u;
	t.slot_trial = 1u;
	CHECK(line_is(&t, BOOT_TIME_COUNT + 8u,
		      "slot read at the faster rate: 1 whole, 2 failed and read again, "
		      "3 failed the CRC and read again, 1 of those then good; "
		      "1 kept by a trial mark's CRC"));
	CHECK(line_is(&t, BOOT_TIME_COUNT + 9u, "end of report"));
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

static void test_record(void)
{
	struct boot_time t = fresh(), back;
	uint8_t rec[BOOT_TIME_RECORD];
	uint32_t mhz = 0;
	unsigned i;

	CHECK(BOOT_TIME_RECORD == 60u * 4u + 8u * BOOT_TIME_COUNT + 4u);
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
	      back.slot_reread_good == 8u && back.slot_trial == 9u);
	CHECK(memcmp(back.at, t.at, sizeof t.at) == 0);

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

int main(void)
{
	RUN(test_mark);
	RUN(test_milestones);
	RUN(test_first_has_no_step);
	RUN(test_time_order);
	RUN(test_since_on);
	RUN(test_summary);
	RUN(test_watch);
	RUN(test_record);
	return check_summary("boot_time");
}
