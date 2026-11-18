/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The settings sector (src/s02_store.c) on the flash fake: its record's
 * bytes and stamp, what a reader accepts, the guards that decide whether
 * the running slot keeps settings at all, the save's page append and its
 * erase when the sector is full or not valid, the factory reset's clear,
 * the footprint (the running slot's settings sector and nothing else), and
 * a power cut or a torn operation at every point of a save and a clear.
 * Requirements: S-02.096 - S-02.099, S-02.105, S-15.172.
 */
#include <string.h>

#include "support/check.h"
#include "s02_fake_flash.h"

#include "waffle/s02_store.h"

static struct fake_flash g_f;
static struct s02_flash_ops g_ops;
static struct s02_env g_env;
static uint8_t g_before[S02_FLASH_SIZE];
static struct s02_store_slot g_run;

/* Slot s's record: tag and length, its CRC complemented for a trial mark;
 * the slot's bytes up to the length filled. */
static uint32_t slot_image(unsigned s, uint32_t len, uint32_t tag, bool trial)
{
	const uint32_t base = s02_slot_base(s), rec = s02_record_off(s);
	const uint32_t n = len < S02_SLOT_CAPACITY ? len : S02_SLOT_CAPACITY;
	uint32_t crc;

	memset(g_f.mem + base, 0xFF, S02_SLOT_CAPACITY);
	memset(g_f.mem + rec, 0xFF, S02_SECTOR_SIZE);
	ff_fill_image(g_f.mem + base, n, 7u + s);
	crc = ref_crc32(0, g_f.mem + base, n);
	ff_put32(g_f.mem + rec, trial ? ~crc : crc);
	ff_put32(g_f.mem + rec + 4u, len);
	ff_put32(g_f.mem + rec + 8u, tag);
	return trial ? ~crc : crc;
}

/* What a start found for slot s: the record as it reads, good unless told. */
static void running(unsigned s, bool good)
{
	g_run.established = true;
	g_run.slot = s;
	g_run.good = good;
	(void)s02_read_record(&g_env, s, &g_run.rec);
}

/* A unit running slot s, both slots holding images that leave their
 * settings sectors alone, something in every other area to see it
 * untouched, and both settings sectors erased. */
static void setup(unsigned s)
{
	ff_init(&g_f);
	g_env = ff_env(&g_f, &g_ops);
	ff_set_selector(&g_f, (uint8_t)s, 0x01u);
	(void)slot_image(0, 0x30000u, S02_RECORD_TAG, false);
	(void)slot_image(1, 0x40000u, S02_RECORD_TAG, false);
	ff_fill_image(g_f.mem + S02_META_OFF, S02_SECTOR_SIZE, 4u);
	for (unsigned k = 0; k < 2u; k++) {
		/* the start logs */
		ff_fill_image(g_f.mem + s02_store_off(k) + S02_STORE_SIZE, S02_SECTOR_SIZE, 5u + k);
	}
	running(s, true);
	ff_reset(&g_f);
}

static void snapshot(void)
{
	memcpy(g_before, g_f.mem, sizeof(g_before));
}

/* Everything outside slot s's settings sector is as snapshot() saw it. */
static bool outside_same(unsigned s)
{
	const uint32_t off = s02_store_off(s);

	return memcmp(g_before, g_f.mem, off) == 0 &&
	       memcmp(g_before + off + S02_STORE_SIZE, g_f.mem + off + S02_STORE_SIZE,
		      S02_FLASH_SIZE - off - S02_STORE_SIZE) == 0;
}

/* Every erase and program in the log lies inside slot s's settings sector. */
static bool only_store_written(unsigned s)
{
	const uint32_t off = s02_store_off(s);

	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++) {
		const struct ff_op *o = &g_f.log[i];

		if (o->kind != FF_READ &&
		    (o->off < off || o->off + o->len > off + S02_STORE_SIZE))
			return false;
	}
	return !g_f.bad_call;
}

/* Nothing in the log reads, erases or programs either settings sector. */
static bool sectors_untouched(void)
{
	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++) {
		const struct ff_op *o = &g_f.log[i];

		for (unsigned k = 0; k < 2u; k++) {
			const uint32_t off = s02_store_off(k);

			if (o->off < off + S02_STORE_SIZE && o->off + o->len > off)
				return false;
		}
	}
	return true;
}

static unsigned count(char kind)
{
	unsigned n = 0;

	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++)
		n += g_f.log[i].kind == kind;
	return n;
}

static struct s02_store st(bool dev, bool color, bool mtp, bool menu)
{
	const struct s02_store s = { dev, color, mtp, menu };

	return s;
}

static bool eq(const struct s02_store *a, const struct s02_store *b)
{
	return a->dev_mode == b->dev_mode && a->color == b->color &&
	       a->mtp_macos == b->mtp_macos && a->start_menu == b->start_menu;
}

static bool is_default(const struct s02_store *a)
{
	const struct s02_store d = st(false, true, true, false);

	return eq(a, &d);
}

/* A record with the given fields, its CRC computed (crc_xor 0) or spoiled. */
static size_t build(uint8_t *b, uint32_t magic, uint16_t version, uint16_t len,
		    uint32_t stamp, const uint8_t *payload, uint32_t crc_xor)
{
	ff_put32(b, magic);
	b[4] = (uint8_t)version;
	b[5] = (uint8_t)(version >> 8);
	b[6] = (uint8_t)len;
	b[7] = (uint8_t)(len >> 8);
	ff_put32(b + 8, stamp);
	memcpy(b + 12, payload, len);
	ff_put32(b + 12u + len, ref_crc32(0, b, 12u + len) ^ crc_xor);
	return 16u + len;
}

static void test_place(void)
{
	CHECK(s02_store_off(0) == 0x86000u && s02_store_off(1) == 0xFE000u);
	CHECK(S02_STORE_SIZE == 0x1000u && S02_STORE_PAGES == 16u);
	CHECK(S02_STORE_SLOT_MAX == 0x76000u);
	CHECK(S02_STORE_PAYLOAD_MAX == 240u && S02_STORE_WRITTEN == 17u);
	/* inside each slot, just below its last sector, the start log */
	for (unsigned k = 0; k < 2u; k++)
		CHECK(s02_store_off(k) == s02_slot_base(k) + S02_STORE_SLOT_MAX &&
		      s02_store_off(k) + S02_STORE_SIZE ==
			      s02_slot_base(k) + S02_SLOT_CAPACITY - S02_SECTOR_SIZE);
}

static void test_encode(void)
{
	const struct s02_store all = st(true, true, true, true), none = st(false, false, false, false);
	uint8_t b[32], want[32];
	struct s02_store out;
	size_t n;

	n = s02_store_encode(&all, 0x12345678u, b, sizeof(b));
	CHECK(n == 17u);
	CHECK(b[0] == 0x57u && b[1] == 0x42u && b[2] == 0x53u && b[3] == 0x53u);
	CHECK(b[4] == 1u && b[5] == 0u && b[6] == 1u && b[7] == 0u);
	CHECK(b[8] == 0x78u && b[9] == 0x56u && b[10] == 0x34u && b[11] == 0x12u);
	CHECK(b[12] == 0x0Fu);
	(void)build(want, S02_STORE_MAGIC, 1u, 1u, 0x12345678u, (const uint8_t[]){ 0x0Fu }, 0u);
	CHECK(memcmp(b, want, 17u) == 0);
	CHECK(s02_store_decode(b, n, 0x12345678u, &out) && eq(&out, &all));
	n = s02_store_encode(&none, 0u, b, sizeof(b));
	CHECK(n == 17u && b[12] == 0x00u);
	CHECK(s02_store_decode(b, n, 0u, &out) && eq(&out, &none));
	/* each flag alone */
	for (unsigned bit = 0; bit < 4u; bit++) {
		const struct s02_store one = st(bit == 0u, bit == 1u, bit == 2u, bit == 3u);

		(void)s02_store_encode(&one, 5u, b, sizeof(b));
		CHECK(b[12] == (uint8_t)(1u << bit));
	}
	CHECK(s02_store_encode(&all, 0u, b, 16u) == 0u);
}

static void test_decode(void)
{
	uint8_t b[S02_PAGE_SIZE], p[S02_STORE_PAYLOAD_MAX + 1u];
	struct s02_store s;
	size_t n;

	memset(p, 0, sizeof(p));
	p[0] = 0x08u;
	n = build(b, S02_STORE_MAGIC, 1u, 1u, 9u, p, 0u);
	CHECK(s02_store_decode(b, n, 9u, &s) && eq(&s, &(struct s02_store){ false, false, false, true }));
	/* the stamp: another image's record, so the defaults */
	s.start_menu = true;
	CHECK(!s02_store_decode(b, n, 10u, &s) && is_default(&s));
	/* the bytes it does not know: flag bits 4 to 7 and a longer payload */
	p[0] = 0xF1u;
	p[1] = 0x55u;
	n = build(b, S02_STORE_MAGIC, 1u, 5u, 9u, p, 0u);
	CHECK(s02_store_decode(b, n, 9u, &s) && eq(&s, &(struct s02_store){ true, false, false, false }));
	n = build(b, S02_STORE_MAGIC, 1u, (uint16_t)S02_STORE_PAYLOAD_MAX, 9u, p, 0u);
	CHECK(n == S02_PAGE_SIZE && s02_store_decode(b, n, 9u, &s));
	/* not valid: the defaults */
	p[0] = 0x0Fu;
	n = build(b, S02_STORE_MAGIC, 2u, 1u, 9u, p, 0u);
	CHECK(!s02_store_decode(b, n, 9u, &s) && is_default(&s));
	n = build(b, S02_STORE_MAGIC, 0u, 1u, 9u, p, 0u);
	CHECK(!s02_store_decode(b, n, 9u, &s) && is_default(&s));
	n = build(b, 0x54534257u, 1u, 1u, 9u, p, 0u);		/* the old boot store's magic */
	CHECK(!s02_store_decode(b, n, 9u, &s) && is_default(&s));
	n = build(b, S02_STORE_MAGIC, 1u, 0u, 9u, p, 0u);	/* length 0 */
	CHECK(!s02_store_decode(b, n, 9u, &s));
	(void)build(b, S02_STORE_MAGIC, 1u, 1u, 9u, p, 0u);
	b[6] = (uint8_t)(S02_STORE_PAYLOAD_MAX + 1u);		/* too long for a page */
	CHECK(!s02_store_decode(b, sizeof(b), 9u, &s));
	n = build(b, S02_STORE_MAGIC, 1u, 1u, 9u, p, 0x00010000u);	/* CRC */
	CHECK(!s02_store_decode(b, n, 9u, &s) && is_default(&s));
	n = build(b, S02_STORE_MAGIC, 1u, 1u, 9u, p, 0u);
	CHECK(!s02_store_decode(b, n - 1u, 9u, &s));		/* cut short */
	memset(b, 0xFF, sizeof(b));				/* erased */
	CHECK(!s02_store_decode(b, sizeof(b), 0xFFFFFFFFu, &s) && is_default(&s));
	memset(b, 0x00, sizeof(b));
	CHECK(!s02_store_decode(b, sizeof(b), 0u, &s) && is_default(&s));
}

/* Every truncation point of the program: the bytes before it written, the
 * rest still erased. Only the whole record reads. */
static void test_torn_prefix(void)
{
	const struct s02_store s = st(true, false, false, true);
	uint8_t rec[S02_STORE_WRITTEN], b[S02_PAGE_SIZE];
	struct s02_store out;

	(void)s02_store_encode(&s, 77u, rec, sizeof(rec));
	for (size_t k = 0; k <= sizeof(rec); k++) {
		memset(b, 0xFF, sizeof(b));
		memcpy(b, rec, k);
		CHECK(s02_store_decode(b, sizeof(b), 77u, &out) == (k == sizeof(rec)));
		CHECK(k == sizeof(rec) ? eq(&out, &s) : is_default(&out));
	}
}

/* The highest valid page with the stamp wins; others are passed over. */
static void test_pick(void)
{
	static uint8_t sec[S02_STORE_SIZE];
	const struct s02_store a = st(true, true, true, false), b = st(false, false, true, true);
	struct s02_store out;

	memset(sec, 0xFF, sizeof(sec));
	CHECK(!s02_store_pick(sec, 1u, &out) && is_default(&out));
	(void)s02_store_encode(&a, 1u, sec, S02_PAGE_SIZE);
	(void)s02_store_encode(&b, 1u, sec + 3u * S02_PAGE_SIZE, S02_PAGE_SIZE);
	CHECK(s02_store_pick(sec, 1u, &out) && eq(&out, &b));
	/* a newer page of another image: not this one's */
	(void)s02_store_encode(&a, 2u, sec + 5u * S02_PAGE_SIZE, S02_PAGE_SIZE);
	CHECK(s02_store_pick(sec, 1u, &out) && eq(&out, &b));
	CHECK(s02_store_pick(sec, 2u, &out) && eq(&out, &a));
	CHECK(!s02_store_pick(sec, 3u, &out) && is_default(&out));
	/* a torn newest page: the one before it */
	(void)s02_store_encode(&a, 1u, sec + 6u * S02_PAGE_SIZE, 10u + 7u);
	sec[6u * S02_PAGE_SIZE + 16u] = 0xFFu;
	CHECK(s02_store_pick(sec, 1u, &out) && eq(&out, &b));
	/* the last page */
	(void)s02_store_encode(&a, 1u, sec + 15u * S02_PAGE_SIZE, S02_PAGE_SIZE);
	CHECK(s02_store_pick(sec, 1u, &out) && eq(&out, &a));
}

/* A save, the reads and their operations, in both slots. */
static void test_save_ops(void)
{
	for (unsigned k = 0; k < 2u; k++) {
		const uint32_t off = s02_store_off(k);
		const struct s02_store on = st(true, false, false, true), off_s = st(false, true, true, false);
		struct s02_store out;

		setup(k);
		/* an erased sector: the defaults */
		CHECK(!s02_store_read(&g_env, &g_run, &out) && is_default(&out));
		/* the record, then one plain read of the sector; nothing written */
		CHECK(g_f.nlog == 2u && g_f.log[0].off == s02_record_off(k) &&
		      g_f.log[1].off == off && g_f.log[1].len == S02_STORE_SIZE && g_f.nmut == 0u);
		snapshot();
		ff_reset(&g_f);
		CHECK(s02_store_write(&g_env, &g_run, &on) == S02_STORE_OK);
		/* the record, the sector, one program at page 0, its read-back */
		CHECK(g_f.nlog == 4u && count(FF_ERASE) == 0u && count(FF_PROGRAM) == 1u);
		CHECK(g_f.log[2].kind == FF_PROGRAM && g_f.log[2].off == off &&
		      g_f.log[2].len == S02_STORE_WRITTEN);
		CHECK(g_f.log[3].kind == FF_READ && g_f.log[3].off == off && g_f.log[3].len == 17u);
		CHECK(outside_same(k) && only_store_written(k));
		CHECK(s02_store_read(&g_env, &g_run, &out) && eq(&out, &on));
		/* the stamp is the record's CRC */
		CHECK(g_f.mem[off + 8] == (uint8_t)g_run.rec.crc32);
		/* the same value again: nothing written */
		ff_reset(&g_f);
		CHECK(s02_store_write(&g_env, &g_run, &on) == S02_STORE_OK && g_f.nmut == 0u);
		/* the next one, page 1 */
		ff_reset(&g_f);
		CHECK(s02_store_write(&g_env, &g_run, &off_s) == S02_STORE_OK);
		CHECK(g_f.nmut == 1u && g_f.log[2].off == off + S02_PAGE_SIZE);
		CHECK(s02_store_read(&g_env, &g_run, &out) && eq(&out, &off_s));
		CHECK(outside_same(k) && only_store_written(k));
		/* the other slot's sector is not this one's */
		CHECK(memcmp(g_before + s02_store_off(1u - k), g_f.mem + s02_store_off(1u - k),
			     S02_STORE_SIZE) == 0);
	}
}

/* Pages appended one per save; the 17th erases and takes page 0. */
static void test_append_and_wrap(void)
{
	const uint32_t off = s02_store_off(1);
	struct s02_store out;

	setup(1);
	snapshot();
	for (unsigned i = 0; i < 40u; i++) {
		const struct s02_store s = st((i & 1u) != 0u, (i & 2u) != 0u, (i & 4u) == 0u, (i & 8u) != 0u);
		const unsigned page = i % 16u;

		/* each value differs from the one before */
		ff_reset(&g_f);
		CHECK(s02_store_write(&g_env, &g_run, &s) == S02_STORE_OK);
		CHECK(count(FF_ERASE) == (i != 0u && page == 0u ? 1u : 0u));
		CHECK(count(FF_PROGRAM) == 1u);
		for (unsigned j = 0; j < g_f.nlog; j++)
			if (g_f.log[j].kind == FF_PROGRAM)
				CHECK(g_f.log[j].off == off + page * S02_PAGE_SIZE);
		CHECK(only_store_written(1) && outside_same(1));
		CHECK(s02_store_read(&g_env, &g_run, &out) && eq(&out, &s));
	}
}

/* A sector that does not read valid is erased before the program: an
 * erased page before a used one, or image bytes from a longer image. */
static void test_not_valid(void)
{
	const uint32_t off = s02_store_off(0);
	const struct s02_store s = st(true, true, false, false);
	struct s02_store out;

	setup(0);
	(void)s02_store_encode(&s, g_run.rec.crc32, g_f.mem + off + 2u * S02_PAGE_SIZE, S02_PAGE_SIZE);
	CHECK(s02_store_read(&g_env, &g_run, &out) && eq(&out, &s));
	snapshot();
	ff_reset(&g_f);
	{
		const struct s02_store t = st(false, true, false, true);

		CHECK(s02_store_write(&g_env, &g_run, &t) == S02_STORE_OK);
		CHECK(count(FF_ERASE) == 1u && g_f.log[2].kind == FF_ERASE && g_f.log[2].off == off);
		CHECK(only_store_written(0) && outside_same(0));
		CHECK(s02_store_read(&g_env, &g_run, &out) && eq(&out, &t));
		CHECK(g_f.mem[off + S02_PAGE_SIZE] == 0xFFu && g_f.mem[off + 2u * S02_PAGE_SIZE] == 0xFFu);
	}
	for (uint32_t seed = 0; seed < 8u; seed++) {
		setup(0);
		ff_fill_image(g_f.mem + off, S02_STORE_SIZE, seed + 30u);
		CHECK(!s02_store_read(&g_env, &g_run, &out) && is_default(&out));
		snapshot();
		ff_reset(&g_f);
		CHECK(s02_store_write(&g_env, &g_run, &s) == S02_STORE_OK);
		CHECK(only_store_written(0) && outside_same(0));
		CHECK(s02_store_read(&g_env, &g_run, &out) && eq(&out, &s));
	}
	/* an old boot store record at page 0: a used page, appended after */
	setup(1);
	{
		static const uint8_t old[] = { 0x57, 0x42, 0x53, 0x54, 0x01, 0x00, 0x01, 0x00,
					       0x01, 0x5b, 0x37, 0x05, 0x55 };

		memcpy(g_f.mem + s02_store_off(1), old, sizeof(old));
		CHECK(!s02_store_read(&g_env, &g_run, &out) && is_default(&out));
		ff_reset(&g_f);
		CHECK(s02_store_write(&g_env, &g_run, &s) == S02_STORE_OK && count(FF_ERASE) == 0u);
		CHECK(memcmp(g_f.mem + s02_store_off(1), old, sizeof(old)) == 0);
		CHECK(s02_store_read(&g_env, &g_run, &out) && eq(&out, &s));
	}
}

/* Another image's records give the defaults; the save appends after them. */
static void test_stamp(void)
{
	const uint32_t off = s02_store_off(1);
	const struct s02_store a = st(true, false, false, true), b = st(false, false, true, false);
	struct s02_store out;

	setup(1);
	CHECK(s02_store_write(&g_env, &g_run, &a) == S02_STORE_OK);
	/* the slot written again with another image, its sector left as it was */
	memcpy(g_before, g_f.mem + off, S02_STORE_SIZE);
	(void)slot_image(1, 0x41000u, S02_RECORD_TAG, false);
	memcpy(g_f.mem + off, g_before, S02_STORE_SIZE);
	running(1, true);
	CHECK(!s02_store_read(&g_env, &g_run, &out) && is_default(&out));
	ff_reset(&g_f);
	CHECK(s02_store_write(&g_env, &g_run, &b) == S02_STORE_OK);
	CHECK(count(FF_ERASE) == 0u && g_f.log[2].off == off + S02_PAGE_SIZE);
	CHECK(s02_store_read(&g_env, &g_run, &out) && eq(&out, &b));
	/* and the first image back: its own record again */
	memcpy(g_before, g_f.mem + off, S02_STORE_SIZE);
	(void)slot_image(1, 0x40000u, S02_RECORD_TAG, false);
	memcpy(g_f.mem + off, g_before, S02_STORE_SIZE);
	running(1, true);
	CHECK(s02_store_read(&g_env, &g_run, &out) && eq(&out, &a));
}

/* When the running slot keeps no settings: no read of the sector, and a
 * save or a clear erases and programs nothing. */
static void test_guards(void)
{
	static const struct {
		uint32_t len, tag;
		bool trial, usable;
	} cases[] = {
		{ 1u, S02_RECORD_TAG, false, true },
		{ 0x76000u, S02_RECORD_TAG, false, true },
		{ 0x76001u, S02_RECORD_TAG, false, false },
		{ 0x77000u, S02_RECORD_TAG, false, false },
		{ 0x78000u, S02_RECORD_TAG, false, false },
		{ 0u, S02_RECORD_TAG, false, false },
		{ 0x40000u, 0x0000B005u, false, false },
		{ 0x40000u, 0xFFFFFFFFu, false, false },
	};
	const struct s02_store on = st(true, false, false, true);
	struct s02_store out;

	for (unsigned k = 0; k < 2u; k++) {
		for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
			setup(k);
			(void)slot_image(k, cases[i].len, cases[i].tag, cases[i].trial);
			running(k, true);
			/* a valid record in the sector's place */
			(void)s02_store_encode(&on, g_run.rec.crc32, g_f.mem + s02_store_off(k), 64u);
			snapshot();
			ff_reset(&g_f);
			if (!cases[i].usable) {
				CHECK(!s02_store_read(&g_env, &g_run, &out) && is_default(&out));
				CHECK(s02_store_write(&g_env, &g_run, &on) == S02_STORE_UNUSABLE);
				CHECK(s02_store_clear(&g_env, &g_run) == S02_STORE_UNUSABLE);
				CHECK(g_f.nmut == 0u && sectors_untouched());
				CHECK(memcmp(g_before, g_f.mem, sizeof(g_before)) == 0);
			} else {
				CHECK(s02_store_read(&g_env, &g_run, &out) && eq(&out, &on));
			}
		}
		/* not established, not found good (a trial mark among them), a
		 * slot index out of range */
		for (unsigned why = 0; why < 4u; why++) {
			setup(k);
			if (why == 2u) {
				(void)slot_image(k, 0x30000u, S02_RECORD_TAG, true);
				running(k, false);
			}
			(void)s02_store_encode(&on, g_run.rec.crc32, g_f.mem + s02_store_off(k), 64u);
			if (why == 0u)
				g_run.established = false;
			else if (why == 1u)
				g_run.good = false;
			else if (why == 3u)
				g_run.slot = 2u;
			snapshot();
			ff_reset(&g_f);
			CHECK(!s02_store_read(&g_env, &g_run, &out) && is_default(&out));
			CHECK(s02_store_write(&g_env, &g_run, &on) == S02_STORE_UNUSABLE);
			CHECK(s02_store_clear(&g_env, &g_run) == S02_STORE_UNUSABLE);
			CHECK(g_f.nmut == 0u && sectors_untouched());
		}
		/* the record changed since the start, or cannot be read */
		setup(k);
		(void)s02_store_encode(&on, g_run.rec.crc32, g_f.mem + s02_store_off(k), 64u);
		g_f.mem[s02_record_off(k)] ^= 0x01u;
		snapshot();
		ff_reset(&g_f);
		CHECK(!s02_store_read(&g_env, &g_run, &out) && is_default(&out));
		CHECK(s02_store_write(&g_env, &g_run, &on) == S02_STORE_UNUSABLE);
		CHECK(g_f.nmut == 0u && sectors_untouched());
		setup(k);
		snapshot();
		g_f.fail_regions = k == 0u ? FF_R_RECORD0 : FF_R_RECORD1;
		CHECK(!s02_store_read(&g_env, &g_run, &out) && is_default(&out));
		CHECK(s02_store_write(&g_env, &g_run, &on) == S02_STORE_UNUSABLE);
		CHECK(s02_store_clear(&g_env, &g_run) == S02_STORE_UNUSABLE);
		CHECK(g_f.nmut == 0u && memcmp(g_before, g_f.mem, sizeof(g_before)) == 0);
	}
}

static void test_failures(void)
{
	const struct s02_store on = st(true, false, false, true);
	struct s02_store out;

	setup(1);
	g_f.fail = 1u;					/* the program */
	CHECK(s02_store_write(&g_env, &g_run, &on) == S02_STORE_IO);
	ff_reset(&g_f);
	g_f.corrupt = 1u;
	g_f.corrupt_mask = 0xFEu;
	CHECK(s02_store_write(&g_env, &g_run, &on) == S02_STORE_VERIFY);
	setup(1);
	g_f.fail_read = 3u;				/* the read-back */
	CHECK(s02_store_write(&g_env, &g_run, &on) == S02_STORE_IO);
	setup(1);
	g_f.fail_read = 2u;				/* the sector's read */
	CHECK(s02_store_write(&g_env, &g_run, &on) == S02_STORE_IO && g_f.nmut == 0u);
	ff_reset(&g_f);
	g_f.fail_read = 2u;
	CHECK(!s02_store_read(&g_env, &g_run, &out) && is_default(&out));
	/* a full sector whose erase fails, or reads back not erased */
	setup(1);
	for (unsigned i = 0; i < 16u; i++) {
		const struct s02_store s = st((i & 1u) != 0u, true, true, false);

		CHECK(s02_store_write(&g_env, &g_run, &s) == S02_STORE_OK);
	}
	ff_reset(&g_f);
	g_f.fail = 1u;
	CHECK(s02_store_write(&g_env, &g_run, &on) == S02_STORE_IO && count(FF_PROGRAM) == 0u);
	ff_reset(&g_f);
	g_f.ignore = 1u;
	CHECK(s02_store_write(&g_env, &g_run, &on) == S02_STORE_VERIFY && count(FF_PROGRAM) == 0u);
	ff_reset(&g_f);
	g_f.ignore = 1u;
	CHECK(s02_store_clear(&g_env, &g_run) == S02_STORE_VERIFY);
}

/* The value the next start reads after a cut: the old one or the new
 * one for a program; for an erase, a value saved before or the defaults.
 * Nothing outside the sector changed. */
static void after_cut(unsigned k, const struct s02_store *allowed, unsigned n, bool dflt)
{
	struct s02_store out;
	bool valid, ok = false;

	ff_reset(&g_f);
	valid = s02_store_read(&g_env, &g_run, &out);
	if (!valid)
		ok = dflt && is_default(&out);
	for (unsigned i = 0; valid && i < n; i++)
		ok = ok || eq(&out, &allowed[i]);
	CHECK(ok);
	CHECK(outside_same(k));
}

static void test_cuts(void)
{
	const struct s02_store old = st(true, false, false, true), now = st(false, true, true, true);
	struct s02_store hist[17];

	for (int from = 0; from < 2; from++) {
		/* from: an old value saved, or an erased sector */
		const struct s02_store allowed[2] = { now, old };

		/* a cut after each operation, the reads counted too */
		for (unsigned cut = 1; cut <= 4u; cut++) {
			setup(1);
			if (from == 0)
				(void)s02_store_write(&g_env, &g_run, &old);
			snapshot();
			ff_reset(&g_f);
			g_f.count_reads = true;
			g_f.cut_after = cut;
			(void)s02_store_write(&g_env, &g_run, &now);
			CHECK(only_store_written(1));
			after_cut(1, allowed, from == 0 ? 2u : 1u, true);
		}
		/* the program torn, many ways: the old value, or the new */
		for (uint32_t seed = 0; seed < 64u; seed++) {
			setup(1);
			if (from == 0)
				(void)s02_store_write(&g_env, &g_run, &old);
			snapshot();
			ff_reset(&g_f);
			g_f.tear = 1u;
			g_f.tear_kind = FF_TEAR_SEED;
			g_f.tear_a = seed;
			CHECK(s02_store_write(&g_env, &g_run, &now) == S02_STORE_IO);
			CHECK(only_store_written(1));
			after_cut(1, allowed, from == 0 ? 2u : 1u, from != 0);
		}
		for (uint32_t byte = 0; byte < S02_STORE_WRITTEN; byte++) {
			for (uint32_t bitn = 0; bitn < 8u; bitn++) {
				setup(1);
				if (from == 0)
					(void)s02_store_write(&g_env, &g_run, &old);
				snapshot();
				ff_reset(&g_f);
				g_f.tear = 1u;
				g_f.tear_kind = FF_TEAR_BIT;
				g_f.tear_a = byte;
				g_f.tear_b = bitn;
				(void)s02_store_write(&g_env, &g_run, &now);
				after_cut(1, allowed, from == 0 ? 2u : 1u, from != 0);
			}
		}
	}
	/* a full sector: the save's erase torn, or cut after it; then any value
	 * saved before, or the defaults */
	for (uint32_t seed = 0; seed < 48u; seed++) {
		for (unsigned kind = 0; kind < 3u; kind++) {
			setup(1);
			for (unsigned i = 0; i < 16u; i++) {
				hist[i] = st((i & 1u) != 0u, (i & 2u) != 0u, (i & 4u) != 0u, (i & 8u) != 0u);
				(void)s02_store_write(&g_env, &g_run, &hist[i]);
			}
			hist[16] = st(true, true, true, true);
			snapshot();
			ff_reset(&g_f);
			if (kind == 0u) {
				g_f.tear = 1u;
				g_f.tear_kind = FF_TEAR_SEED;
				g_f.tear_a = seed;
			} else if (kind == 1u) {
				g_f.tear = 1u;
				g_f.tear_kind = FF_TEAR_ZEROS;
				g_f.tear_a = (seed % 16u) * S02_PAGE_SIZE;
				g_f.tear_b = S02_STORE_SIZE;
			} else {
				g_f.cut_after = 1u + seed % 2u;	/* after the erase, after the program */
			}
			(void)s02_store_write(&g_env, &g_run, &hist[16]);
			CHECK(only_store_written(1));
			after_cut(1, hist, 17u, true);
		}
	}
}

/* The factory reset's clear: an erased sector left alone, a used one
 * erased and read back; cut or torn, a value saved before or the
 * defaults. */
static void test_clear(void)
{
	const struct s02_store a = st(true, false, false, true), b = st(false, false, true, true);
	const struct s02_store hist[2] = { a, b };
	struct s02_store out;

	for (unsigned k = 0; k < 2u; k++) {
		setup(k);
		snapshot();
		CHECK(s02_store_clear(&g_env, &g_run) == S02_STORE_OK && g_f.nmut == 0u);
		(void)s02_store_write(&g_env, &g_run, &a);
		(void)s02_store_write(&g_env, &g_run, &b);
		snapshot();
		ff_reset(&g_f);
		CHECK(s02_store_clear(&g_env, &g_run) == S02_STORE_OK);
		CHECK(count(FF_ERASE) == 1u && count(FF_PROGRAM) == 0u && only_store_written(k));
		CHECK(outside_same(k));
		CHECK(!s02_store_read(&g_env, &g_run, &out) && is_default(&out));
		/* image bytes there: erased too */
		ff_fill_image(g_f.mem + s02_store_off(k), S02_STORE_SIZE, 3u);
		ff_reset(&g_f);
		CHECK(s02_store_clear(&g_env, &g_run) == S02_STORE_OK && count(FF_ERASE) == 1u);
	}
	for (uint32_t seed = 0; seed < 64u; seed++) {
		setup(1);
		(void)s02_store_write(&g_env, &g_run, &a);
		(void)s02_store_write(&g_env, &g_run, &b);
		snapshot();
		ff_reset(&g_f);
		g_f.tear = 1u;
		g_f.tear_kind = seed < 48u ? FF_TEAR_SEED : FF_TEAR_ZEROS;
		g_f.tear_a = seed < 48u ? seed : (seed - 48u) * S02_PAGE_SIZE;
		g_f.tear_b = S02_STORE_SIZE;
		CHECK(s02_store_clear(&g_env, &g_run) == S02_STORE_IO);
		CHECK(only_store_written(1));
		after_cut(1, hist, 2u, true);
	}
}

int main(void)
{
	RUN(test_place);
	RUN(test_encode);
	RUN(test_decode);
	RUN(test_torn_prefix);
	RUN(test_pick);
	RUN(test_save_ops);
	RUN(test_append_and_wrap);
	RUN(test_not_valid);
	RUN(test_stamp);
	RUN(test_guards);
	RUN(test_failures);
	RUN(test_cuts);
	RUN(test_clear);
	return check_summary("test_s02_store");
}
