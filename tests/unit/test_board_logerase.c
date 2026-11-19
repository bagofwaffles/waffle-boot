/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-02.101: the main image's erase of the start log (opxy_front.c): the
 * scan of a slot's log as the front reads it, every branch of the guard,
 * the erase and its read-back, and each state a cut of that erase can
 * leave, with what the next start then does; the front's version 2 record
 * lifting the guard on the other slot's trial mark. Also the settings sector's
 * save and clear (opxy_store.c), made with the writers' tries (S-13b.017).
 */
#include "support/check.h"

#include <string.h>

#include "s02_fake_flash.h"
#include "waffle/board/opxy_boot.h"

static struct fake_flash g_f;
static struct s02_flash_ops g_ops;
static struct opxy_master m;

#define LEN 0x40000u	/* an image that leaves the log free */

static void record(unsigned slot, uint32_t len, uint32_t tag, uint32_t crc)
{
	struct s02_record r = { crc, len, tag };
	uint8_t raw[S02_RECORD_SIZE];

	s02_record_encode(&r, raw);
	memset(g_f.mem + s02_record_off(slot), 0xFF, S02_SECTOR_SIZE);
	memcpy(g_f.mem + s02_record_off(slot), raw, sizeof raw);
}

/* Slot s selected and trusted, both slots with a log, both logs erased. */
static void flash(unsigned s)
{
	ff_init(&g_f);
	g_f.mem[S02_SELECTOR_OFF] = (uint8_t)s;
	g_f.mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	record(0, LEN, S02_RECORD_TAG, 0x12345678u);
	record(1, LEN, S02_RECORD_TAG, 0x12345678u);
}

/* n finished entries from the log's start. */
static void finished(unsigned slot, unsigned n)
{
	uint8_t *p = g_f.mem + FR_LOG_OFF(slot);
	unsigned e;

	for (e = 0; e < n; e++)
		p[e / 4u] = (uint8_t)(p[e / 4u] & ~(3u << (2u * (e % 4u))));
}

/* Entry e started and not finished. */
static void unfinished(unsigned slot, unsigned e)
{
	uint8_t *p = g_f.mem + FR_LOG_OFF(slot);

	p[e / 4u] = (uint8_t)((p[e / 4u] & ~(3u << (2u * (e % 4u)))) | (2u << (2u * (e % 4u))));
}

static struct opxy_log_start normal(unsigned s, unsigned entry)
{
	struct opxy_log_start st = { true, true, (uint8_t)s, (uint16_t)entry, true, (uint8_t)s, false,
				      false, 0 };

	return st;
}

static enum opxy_log_erase guard(const struct opxy_log_start *st, uint32_t *off)
{
	struct s02_env env = ff_env(&g_f, &g_ops);

	ff_reset(&g_f);
	*off = 0;
	return opxy_log_erase_at(&env, st, off);
}

static void test_scan(void)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	struct opxy_log_scan v;

	flash(0);
	CHECK(opxy_log_scan(&env, 0, &v) == 0 && v.state == OPXY_LOG_OK && v.used == 0u);
	finished(0, 256);
	CHECK(opxy_log_scan(&env, 0, &v) == 0 && v.state == OPXY_LOG_OK && v.used == 256u &&
	      v.unfinished == 0u && !v.trial_alone);
	unfinished(0, 256);
	unfinished(0, 257);
	CHECK(opxy_log_scan(&env, 0, &v) == 0 && v.used == 258u && v.unfinished == 2u);
	/* every entry used: full; a not-valid entry anywhere: odd */
	memset(g_f.mem + FR_LOG_OFF(0), 0x00, S02_SECTOR_SIZE);
	CHECK(opxy_log_scan(&env, 0, &v) == 0 && v.state == OPXY_LOG_FULL && v.used == 0u);
	g_f.mem[FR_LOG_OFF(0) + 4095u] = 0x01u;
	CHECK(opxy_log_scan(&env, 0, &v) == 0 && v.state == OPXY_LOG_ODD);
	memset(g_f.mem + FR_LOG_OFF(0), 0xFF, S02_SECTOR_SIZE);
	g_f.mem[FR_LOG_OFF(0) + 1u] = 0x00u;	/* used after free */
	CHECK(opxy_log_scan(&env, 0, &v) == 0 && v.state == OPXY_LOG_ODD);
	/* a trial's entry alone */
	static const uint8_t trial[] = { 0xFD, 0xFC, 0xF0, 0xF9 };
	for (unsigned i = 0; i < sizeof trial; i++) {
		memset(g_f.mem + FR_LOG_OFF(1), 0xFF, S02_SECTOR_SIZE);
		g_f.mem[FR_LOG_OFF(1)] = trial[i];
		CHECK(opxy_log_scan(&env, 1, &v) == 0 && v.trial_alone && v.byte0 == trial[i]);
		CHECK(v.state == ((trial[i] & 3u) == 1u ? OPXY_LOG_ODD : OPXY_LOG_OK));
		g_f.mem[FR_LOG_OFF(1) + 100u] = 0xFE;
		CHECK(opxy_log_scan(&env, 1, &v) == 0 && !v.trial_alone);
	}
	/* the front's rule for a log: the tag and a length of 1 to 0x77000 */
	record(0, 0x77001u, S02_RECORD_TAG, 0);
	CHECK(opxy_log_scan(&env, 0, &v) == 0 && v.state == OPXY_LOG_NONE);
	record(0, 0, S02_RECORD_TAG, 0);
	CHECK(opxy_log_scan(&env, 0, &v) == 0 && v.state == OPXY_LOG_NONE);
	record(0, LEN, 0x0000B005u, 0);
	CHECK(opxy_log_scan(&env, 0, &v) == 0 && v.state == OPXY_LOG_NONE);
	record(0, 0x77000u, S02_RECORD_TAG, 0);
	CHECK(opxy_log_scan(&env, 0, &v) == 0 && v.state != OPXY_LOG_NONE);
	/* a failed read of the record or the sector */
	ff_reset(&g_f);
	g_f.fail_read = 1;
	CHECK(opxy_log_scan(&env, 0, &v) != 0 && v.state == OPXY_LOG_IO);
	ff_reset(&g_f);
	g_f.fail_read = 2;
	CHECK(opxy_log_scan(&env, 0, &v) != 0 && v.state == OPXY_LOG_IO);
	CHECK(opxy_log_scan(&env, 2, &v) != 0);
}

/* (a): 256 used entries, this start's the newest and finished. */
static void test_used(void)
{
	struct opxy_log_start st;
	uint32_t off;

	for (unsigned s = 0; s < 2u; s++) {
		flash(s);
		finished(s, 256);
		st = normal(s, 255);
		CHECK(guard(&st, &off) == OPXY_ERASE_USED && off == FR_LOG_OFF(s));
		CHECK(g_f.nmut == 0u);			/* the guard writes nothing */
		/* below the threshold */
		flash(s);
		finished(s, 255);
		st = normal(s, 254);
		CHECK(guard(&st, &off) == OPXY_ERASE_NO);
		/* this start's entry not finished (the mark failed) */
		flash(s);
		finished(s, 255);
		unfinished(s, 255);
		st = normal(s, 255);
		CHECK(guard(&st, &off) == OPXY_ERASE_NO);
		/* an older start that did not finish does not count */
		flash(s);
		finished(s, 300);
		unfinished(s, 17);
		st = normal(s, 299);
		CHECK(guard(&st, &off) == OPXY_ERASE_USED);
		/* the entry named is not the newest, or in the other log */
		flash(s);
		finished(s, 300);
		st = normal(s, 200);
		CHECK(guard(&st, &off) == OPXY_ERASE_NO);
		st = normal(s, 299);
		st.log_slot = (uint8_t)(1u - s);
		CHECK(guard(&st, &off) == OPXY_ERASE_NO);
		/* no entry named: a log that reads valid is left */
		st = normal(s, 299);
		st.logged = false;
		CHECK(guard(&st, &off) == OPXY_ERASE_NO);
	}
}

/* The conditions before any log is looked at. */
static void test_start_conditions(void)
{
	struct opxy_log_start st;
	uint32_t off;

	flash(0);
	memset(g_f.mem + FR_LOG_OFF(0), 0x00, S02_SECTOR_SIZE);	/* full */
	memset(g_f.mem + FR_LOG_OFF(1), 0x00, S02_SECTOR_SIZE);
	st = normal(0, 0);
	st.logged = false;
	CHECK(guard(&st, &off) == OPXY_ERASE_FULL && off == FR_LOG_OFF(0));
	st.front = false;
	CHECK(guard(&st, &off) == OPXY_ERASE_NO && g_f.nall == 0u);
	st.front = true;
	st.established = false;
	CHECK(guard(&st, &off) == OPXY_ERASE_NO && g_f.nall == 0u);
	st.established = true;
	st.trial = true;		/* a trial start: never */
	CHECK(guard(&st, &off) == OPXY_ERASE_NO && g_f.nall == 0u);
	st.trial = false;
	st.running = 2;
	CHECK(guard(&st, &off) == OPXY_ERASE_NO);
	/* the running slot is not the selected one: never the other's log */
	st.running = 1;
	CHECK(guard(&st, &off) == OPXY_ERASE_NO);
	g_f.mem[S02_SELECTOR_OFF] = 0x01u;
	CHECK(guard(&st, &off) == OPXY_ERASE_FULL && off == FR_LOG_OFF(1));
	/* any index but 0x00 selects slot 1 (S-02.011) */
	g_f.mem[S02_SELECTOR_OFF] = 0xFFu;
	g_f.mem[S02_SELECTOR_OFF + 1u] = 0xFFu;
	CHECK(guard(&st, &off) == OPXY_ERASE_FULL && off == FR_LOG_OFF(1));
	/* a failed read of the selector, the record or the sector */
	for (unsigned r = 1; r <= 3u; r++) {
		ff_reset(&g_f);
		g_f.fail_read = r;
		off = 0;
		{
			struct s02_env env = ff_env(&g_f, &g_ops);

			CHECK(opxy_log_erase_at(&env, &st, &off) == OPXY_ERASE_NO);
		}
	}
	/* a record that leaves no log */
	record(1, 0x77001u, S02_RECORD_TAG, 0);
	CHECK(guard(&st, &off) == OPXY_ERASE_NO);
}

/* (b) and (c): a full or odd log, and the trial's entries. */
static void test_full_odd(void)
{
	struct opxy_log_start st = normal(0, 0);
	uint32_t off;

	st.logged = false;
	/* odd: a torn erase's bytes, an old trial's entry 0 */
	static const uint8_t odd[][3] = {
		{ 0xF3, 0xFF, 0xFF }, { 0xFD, 0xFF, 0xFF }, { 0xF9, 0xFF, 0xFF }, { 0x00, 0xFF, 0x00 },
	};
	for (unsigned i = 0; i < sizeof odd / sizeof odd[0]; i++) {
		flash(0);
		memcpy(g_f.mem + FR_LOG_OFF(0), odd[i], sizeof odd[i]);
		CHECK(guard(&st, &off) == OPXY_ERASE_ODD && off == FR_LOG_OFF(0));
	}
	/* the other slot's trial pending or just started: no erase */
	static const uint8_t other[] = { 0xFD, 0xFC, 0xF0, 0xF9 };
	for (unsigned i = 0; i < sizeof other; i++) {
		flash(0);
		memset(g_f.mem + FR_LOG_OFF(0), 0x00, S02_SECTOR_SIZE);
		g_f.mem[FR_LOG_OFF(1)] = other[i];
		CHECK(guard(&st, &off) == (i < 2u ? OPXY_ERASE_NO : OPXY_ERASE_FULL));
		/* with an entry named the front made no trial mark */
		st.logged = true;
		CHECK(guard(&st, &off) == OPXY_ERASE_FULL);
		st.logged = false;
		/* an other slot with no log has no trial entry */
		record(1, 0x77001u, S02_RECORD_TAG, 0);
		CHECK(guard(&st, &off) == OPXY_ERASE_FULL);
	}
	/* the other slot's log cannot be read */
	flash(0);
	memset(g_f.mem + FR_LOG_OFF(0), 0x00, S02_SECTOR_SIZE);
	ff_reset(&g_f);
	g_f.fail_read = 5;	/* selector, record, sector, the other's record, its sector */
	{
		struct s02_env env = ff_env(&g_f, &g_ops);

		CHECK(opxy_log_erase_at(&env, &st, &off) == OPXY_ERASE_NO);
	}
	g_f.fail_read = 0;
	CHECK(guard(&st, &off) == OPXY_ERASE_FULL);
}

static void master(unsigned s, bool logged, unsigned entry)
{
	memset(&m, 0, sizeof m);
	m.flash = ff_env(&g_f, &g_ops);
	m.front = true;
	m.log_finished = true;
	m.ho.flags = logged ? FR_HO_START_LOGGED : 0u;
	m.ho.log_slot = (uint8_t)s;
	m.ho.log_entry = (uint16_t)entry;
	m.running_established = true;
	m.running_slot = (uint8_t)s;
	s14_log_init(&m.sv.log, S14_MASTER, NULL, NULL, NULL, NULL);
	ff_reset(&g_f);
}

/*
 * S-02.101 guard 4: the other slot's trial mark (0xFD pending, 0xFC started)
 * blocks the (b) or (c) erase, unless the front's version 2 record says it
 * established the selected slot and marked no trial. With version 1, the
 * running flag clear, the trial mark set, or the other slot named, the
 * guard stands. Through the tidy step, from the record as taken.
 */
static void test_front_record(void)
{
	static const struct { uint16_t version; uint32_t flags; uint8_t run; bool erased; } c[] = {
		{ 1, FR_HO_RUNNING, 0, false },			/* version 1 */
		{ 2, FR_HO_RUNNING, 0, true },			/* no trial, this slot */
		{ 2, FR_HO_RUNNING | FR_HO_TRIAL_MARKED, 0, false },
		{ 2, FR_HO_RUNNING, 1, false },			/* the other slot named */
		{ 2, 0, 0, false },				/* no running flag */
	};
	static const uint8_t other[] = { 0xFD, OPXY_TRIAL_STARTED_BYTE };

	for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++)
		for (unsigned o = 0; o < sizeof other; o++)
			for (unsigned odd = 0; odd < 2u; odd++) {
				flash(0);
				/* full, or odd */
				memset(g_f.mem + FR_LOG_OFF(0), odd ? 0xFF : 0x00, S02_SECTOR_SIZE);
				if (odd)
					g_f.mem[FR_LOG_OFF(0) + 2u] = 0x01u;
				g_f.mem[FR_LOG_OFF(1)] = other[o];
				master(0, false, 0);
				m.ho.version = c[i].version;
				m.ho.flags = c[i].flags;
				m.ho.run_slot = c[i].run;
				opxy_start_log_tidy(&m);
				CHECK(m.log_tidied);
				CHECK(g_f.nmut == (c[i].erased ? 1u : 0u));
				CHECK(g_f.mem[FR_LOG_OFF(0)] == (c[i].erased ? 0xFFu : odd ? 0xFFu : 0x00u));
				CHECK(g_f.mem[FR_LOG_OFF(0) + 2u] ==
				      (c[i].erased ? 0xFFu : odd ? 0x01u : 0x00u));
				/* never the other slot's log */
				CHECK(g_f.mem[FR_LOG_OFF(1)] == other[o]);
			}
	/* the guard alone: the record's word for this slot lifts it, another
	 * slot's does not */
	{
		struct opxy_log_start st = normal(0, 0);
		uint32_t off;

		st.logged = false;
		flash(0);
		memset(g_f.mem + FR_LOG_OFF(0), 0x00, S02_SECTOR_SIZE);
		g_f.mem[FR_LOG_OFF(1)] = 0xFD;
		CHECK(guard(&st, &off) == OPXY_ERASE_NO);
		st.no_trial = true;
		CHECK(guard(&st, &off) == OPXY_ERASE_FULL && off == FR_LOG_OFF(0));
		st.no_trial_slot = 1;
		CHECK(guard(&st, &off) == OPXY_ERASE_NO);
		/* the other conditions still hold: a trial start, not established */
		st.no_trial_slot = 0;
		st.trial = true;
		CHECK(guard(&st, &off) == OPXY_ERASE_NO);
		st.trial = false;
		st.established = false;
		CHECK(guard(&st, &off) == OPXY_ERASE_NO);
	}
}

static void test_tidy(void)
{
	/* an entry below the threshold: nothing is read */
	flash(0);
	finished(0, 10);
	master(0, true, 9);
	opxy_start_log_tidy(&m);
	CHECK(g_f.nall == 0u && m.log_tidied);
	/* due: erased, read back, once */
	flash(1);
	finished(1, 256);
	master(1, true, 255);
	opxy_start_log_tidy(&m);
	CHECK(g_f.nmut == 1u && g_f.log[3].kind == FF_ERASE && g_f.log[3].off == FR_LOG_OFF(1));
	for (unsigned i = 0; i < S02_SECTOR_SIZE; i++)
		CHECK(g_f.mem[FR_LOG_OFF(1) + i] == 0xFFu);
	CHECK(g_f.log[g_f.nlog - 1u].kind == FF_READ);
	CHECK(m.hw.flash.tries == 1u);		/* the writer's tries ended */
	ff_reset(&g_f);
	opxy_start_log_tidy(&m);
	CHECK(g_f.nall == 0u);
	/* before the finished mark: nothing */
	flash(1);
	finished(1, 256);
	master(1, true, 255);
	m.log_finished = false;
	opxy_start_log_tidy(&m);
	CHECK(g_f.nall == 0u && !m.log_tidied);
	/* the erase fails: nothing else written, the start goes on */
	m.log_finished = true;
	g_f.fail = 1;
	opxy_start_log_tidy(&m);
	CHECK(g_f.nmut == 1u && g_f.mem[FR_LOG_OFF(1)] == 0x00u);
	/* the erase reports success and changes nothing: the read-back says so */
	flash(1);
	finished(1, 256);
	master(1, true, 255);
	g_f.ignore = 1;
	opxy_start_log_tidy(&m);
	CHECK(g_f.nmut == 1u && g_f.mem[FR_LOG_OFF(1)] == 0x00u);
	/* the view for Device Info */
	flash(0);
	finished(0, 41);
	master(0, true, 40);
	opxy_start_log_view(&m);
	CHECK(m.sv.boot.start_log == S15_LOG_OK && m.sv.boot.start_log_used == 41u &&
	      m.sv.boot.start_log_slot == 0u && m.sv.boot.start_log_unfinished == 0u &&
	      m.sv.boot.start_log_erase == OPXY_LOG_ERASE_USED);
	/* once per boot, but again for Device Info's second page (S-15.082) */
	unfinished(0, 41);
	unfinished(0, 42);
	opxy_start_log_view(&m);
	CHECK(m.sv.boot.start_log_used == 41u);
	opxy_start_log_reread(&m);
	CHECK(m.sv.boot.start_log == S15_LOG_OK && m.sv.boot.start_log_used == 43u &&
	      m.sv.boot.start_log_unfinished == 2u && m.log_viewed);
	/* Slot 2 selected: its log */
	flash(1);
	finished(1, 7);
	opxy_start_log_reread(&m);
	CHECK(m.sv.boot.start_log == S15_LOG_OK && m.sv.boot.start_log_slot == 1u &&
	      m.sv.boot.start_log_used == 7u && m.sv.boot.start_log_unfinished == 0u);
	flash(0);
	memset(g_f.mem + FR_LOG_OFF(0), 0x00, S02_SECTOR_SIZE);
	m.log_viewed = false;
	opxy_start_log_view(&m);
	CHECK(m.sv.boot.start_log == S15_LOG_FULL);
	g_f.mem[FR_LOG_OFF(0)] = 0xFD;
	m.log_viewed = false;
	opxy_start_log_view(&m);
	CHECK(m.sv.boot.start_log == S15_LOG_ODD);
	record(0, 0x77001u, S02_RECORD_TAG, 0);
	m.log_viewed = false;
	opxy_start_log_view(&m);
	CHECK(m.sv.boot.start_log == S15_LOG_NONE);
	ff_reset(&g_f);
	g_f.fail_read = 1;
	m.log_viewed = false;
	opxy_start_log_view(&m);
	CHECK(m.sv.boot.start_log == S15_LOG_UNREAD);
}

/*
 * A cut during the erase. The tear raises bits as §Terms allows (each bit old
 * or 1); the datasheet does not settle what a cut leaves, so any content is
 * also tried. Whatever the sector holds after the cut, the next start reads
 * it as the front does: a valid log goes on counting (and erases at the
 * threshold), and a full or odd one is erased at the next good start.
 */
static void after_cut(unsigned s)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	struct opxy_log_scan v;
	struct opxy_log_start st = normal(s, 0);
	uint32_t off;

	ff_reset(&g_f);
	CHECK(opxy_log_scan(&env, s, &v) == 0);
	if (v.state == OPXY_LOG_OK) {
		/* the front marks entry v.used, unless three unfinished open
		 * recovery once; the count then goes on from there */
		CHECK(v.used < FR_LOG_ENTRIES);
		return;
	}
	CHECK(v.state == OPXY_LOG_FULL || v.state == OPXY_LOG_ODD);
	/* the front names no entry; the next good start erases it */
	st.logged = false;
	CHECK(guard(&st, &off) != OPXY_ERASE_NO && off == FR_LOG_OFF(s));
}

static void test_cuts(void)
{
	for (unsigned s = 0; s < 2u; s++) {
		for (unsigned k = 0; k < 6u; k++) {
			uint32_t rng = 7u + k * 977u + s;

			flash(s);
			finished(s, 256);
			master(s, true, 255);
			g_f.tear = 1;
			g_f.tear_kind = k < 3u ? FF_TEAR_SEED : k == 3u ? FF_TEAR_BIT : FF_TEAR_ZEROS;
			g_f.tear_a = k < 3u ? 100u + k : k == 3u ? 0u : 1u;
			g_f.tear_b = k == 3u ? 1u : 4096u;
			opxy_start_log_tidy(&m);
			after_cut(s);
			/* any content at all, as the datasheet leaves it */
			flash(s);
			for (unsigned i = 0; i < S02_SECTOR_SIZE; i++) {
				rng ^= rng << 13;
				rng ^= rng >> 17;
				rng ^= rng << 5;
				g_f.mem[FR_LOG_OFF(s) + i] = (uint8_t)(i < 64u * k ? 0xFFu : rng);
			}
			after_cut(s);
		}
		/* a cut that left every bit as it was: the log as before, erased
		 * at the next start that finishes */
		flash(s);
		finished(s, 256);
		after_cut(s);
		/* nearly done: byte 0 alone not yet raised */
		flash(s);
		g_f.mem[FR_LOG_OFF(s)] = 0xFD;
		after_cut(s);
		flash(s);
		g_f.mem[FR_LOG_OFF(s)] = 0x00;
		after_cut(s);
	}
}

/* The tries each erase and program of the boot store's save is made with. */
static unsigned tries_seen, writes_seen;

static int tries_erase(void *ctx, uint32_t off)
{
	tries_seen |= 1u << m.hw.flash.tries;
	writes_seen++;
	return ff_erase(ctx, off);
}

static int tries_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	tries_seen |= 1u << m.hw.flash.tries;
	writes_seen++;
	return ff_program(ctx, off, buf, len);
}

static void test_store_tries(void)
{
	static struct s02_flash_ops ops;
	const struct s02_store on = { true, false, true, true };
	struct s02_store now = { false, true, true, false };

	for (unsigned s = 0; s < 2u; s++) {
		flash(s);
		master(s, false, 0);
		m.store_slot.established = true;
		m.store_slot.slot = s;
		m.store_slot.good = true;
		(void)s02_read_record(&m.flash, s, &m.store_slot.rec);
		ops = g_ops;
		ops.erase = tries_erase;
		ops.program = tries_program;
		m.flash.flash = &ops;
		tries_seen = writes_seen = 0;
		CHECK(opxy_boot_store_save(&m, &on, &now) == S15_STORE_SAVED);
		CHECK(now.dev_mode && !now.color && now.mtp_macos && now.start_menu);
		CHECK(writes_seen == 1u && tries_seen == 1u << OPXY_FLASH_WRITER_TRIES);
		CHECK(m.hw.flash.tries == 1u);
		/* the running slot's sector alone */
		CHECK(g_f.mem[s02_store_off(s)] == 0x57u && g_f.mem[s02_store_off(1u - s)] == 0xFFu);
		tries_seen = writes_seen = 0;
		CHECK(opxy_boot_store_clear(&m) == S15_STORE_SAVED);
		CHECK(writes_seen == 1u && tries_seen == 1u << OPXY_FLASH_WRITER_TRIES);
		CHECK(g_f.mem[s02_store_off(s)] == 0xFFu);
	}
	/* a start whose running slot is not established reads the defaults
	 * and reads nothing of either sector */
	flash(1);
	master(1, false, 0);
	m.flash.flash = &g_ops;
	ff_reset(&g_f);
	{
		const struct s02_store_slot run = { false, 1u, { 0x12345678u, LEN, S02_RECORD_TAG }, true };

		opxy_boot_store_load(&m, &run);
	}
	CHECK(!m.store_slot.established && g_f.nall == 0u);
	CHECK(!m.sv.store.dev_mode && !m.sv.store.color && m.sv.store.mtp_macos &&
	      !m.sv.store.start_menu);
}

int main(void)
{
	RUN(test_scan);
	RUN(test_used);
	RUN(test_start_conditions);
	RUN(test_full_odd);
	RUN(test_tidy);
	RUN(test_front_record);
	RUN(test_cuts);
	RUN(test_store_tries);
	return check_summary("test_board_logerase");
}
