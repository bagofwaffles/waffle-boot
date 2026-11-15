/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The boot store (src/s02_store.c) on the flash fake: its record's bytes,
 * what a reader accepts, slot 1's record deciding whether the sector may be
 * used at all, the write's operations (one sector, 0xFE000, and nothing
 * else), and a power cut or a torn operation at every point of a write.
 * Requirements: S-02.096 - S-02.099, S-15.175 (the flag it keeps).
 */
#include <string.h>

#include "support/check.h"
#include "s02_fake_flash.h"

#include "waffle/s02_store.h"

#define OFF S02_STORE_OFF
#define REC1 S02_RECORD1_OFF

static struct fake_flash g_f;
static struct s02_flash_ops g_ops;
static struct s02_env g_env;
static uint8_t g_before[S02_FLASH_SIZE];

/* Slot 1's record: tag and length, its CRC complemented for a trial mark;
 * the slot's bytes up to the length filled. */
static void slot1(uint32_t len, uint32_t tag, bool trial)
{
	uint32_t n = len < S02_SLOT_CAPACITY ? len : S02_SLOT_CAPACITY;
	uint32_t crc;

	memset(g_f.mem + S02_SLOT1_OFF, 0xFF, S02_SLOT_CAPACITY);
	memset(g_f.mem + REC1, 0xFF, S02_SECTOR_SIZE);
	ff_fill_image(g_f.mem + S02_SLOT1_OFF, n, 7u);
	crc = ref_crc32(0, g_f.mem + S02_SLOT1_OFF, n);
	ff_put32(g_f.mem + REC1, trial ? ~crc : crc);
	ff_put32(g_f.mem + REC1 + 4u, len);
	ff_put32(g_f.mem + REC1 + 8u, tag);
}

/* A unit with slot 1 holding an image that leaves the sector alone, and
 * something in every other area to see it untouched. */
static void setup(void)
{
	ff_init(&g_f);
	g_env = ff_env(&g_f, &g_ops);
	ff_set_slot(&g_f, 0, "good", 0x30000u, 3u);
	ff_set_selector(&g_f, 0x00u, 0x01u);
	slot1(0x40000u, S02_RECORD_TAG, false);
	ff_fill_image(g_f.mem + S02_META_OFF, S02_SECTOR_SIZE, 4u);
	/* slot 1's last sector: the start log and trial marks (S-02.095) */
	ff_fill_image(g_f.mem + OFF + S02_STORE_SIZE, S02_SECTOR_SIZE, 5u);
}

static void snapshot(void)
{
	memcpy(g_before, g_f.mem, sizeof(g_before));
}

static bool outside_same(void)
{
	return memcmp(g_before, g_f.mem, OFF) == 0 &&
	       memcmp(g_before + OFF + S02_STORE_SIZE, g_f.mem + OFF + S02_STORE_SIZE,
		      S02_FLASH_SIZE - OFF - S02_STORE_SIZE) == 0;
}

/* Every erase and program in the log lies inside the store's sector. */
static bool only_store_written(void)
{
	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++) {
		const struct ff_op *o = &g_f.log[i];

		if (o->kind != FF_READ &&
		    (o->off < OFF || o->off + o->len > OFF + S02_STORE_SIZE))
			return false;
	}
	return !g_f.bad_call;
}

/* Nothing in the log reads, erases or programs the store's sector. */
static bool sector_untouched(void)
{
	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++) {
		const struct ff_op *o = &g_f.log[i];

		if (o->off < OFF + S02_STORE_SIZE && o->off + o->len > OFF)
			return false;
	}
	return true;
}

/* A record with the given header fields and payload, its CRC computed
 * (crc_xor 0) or spoiled. */
static size_t build(uint8_t *b, uint32_t magic, uint16_t version, uint16_t len,
		    const uint8_t *payload, uint32_t crc_xor)
{
	ff_put32(b, magic);
	b[4] = (uint8_t)version;
	b[5] = (uint8_t)(version >> 8);
	b[6] = (uint8_t)len;
	b[7] = (uint8_t)(len >> 8);
	memcpy(b + 8, payload, len);
	ff_put32(b + 8u + len, ref_crc32(0, b, 8u + len) ^ crc_xor);
	return 12u + len;
}

static void test_encode(void)
{
	/* boot_store.json's records "on" and "off" */
	static const uint8_t on[] = { 0x57, 0x42, 0x53, 0x54, 0x01, 0x00, 0x01, 0x00,
				      0x01, 0x5b, 0x37, 0x05, 0x55 };
	static const uint8_t off_rec[] = { 0x57, 0x42, 0x53, 0x54, 0x01, 0x00, 0x01, 0x00,
					   0x00, 0xcd, 0x07, 0x02, 0x22 };
	const struct s02_store s = { true }, z = { false };
	uint8_t b[32];
	size_t n;

	n = s02_store_encode(&s, b, sizeof(b));
	CHECK(n == 13u && n == S02_STORE_WRITTEN);
	CHECK(memcmp(b, on, sizeof(on)) == 0);
	CHECK(((uint32_t)b[9] | (uint32_t)b[10] << 8 | (uint32_t)b[11] << 16 |
	       (uint32_t)b[12] << 24) == ref_crc32(0, b, 9u));
	n = s02_store_encode(&z, b, sizeof(b));
	CHECK(n == 13u && memcmp(b, off_rec, sizeof(off_rec)) == 0);
	CHECK(s02_store_encode(&s, b, 12u) == 0u);
	CHECK(S02_STORE_PAYLOAD_MAX == 4080u);
}

static void test_decode(void)
{
	static uint8_t b[S02_SECTOR_SIZE];
	uint8_t p[S02_STORE_PAYLOAD_MAX + 1u];
	struct s02_store s;
	size_t n;

	memset(p, 0, sizeof(p));
	p[0] = 0x01u;
	n = build(b, S02_STORE_MAGIC, 1u, 1u, p, 0u);
	s.start_menu = false;
	CHECK(s02_store_decode(b, n, &s) && s.start_menu);
	/* the bytes it does not know: other flag bits and a longer payload */
	p[0] = 0xFEu;
	p[1] = 0x55u;
	p[4] = 0xAAu;
	n = build(b, S02_STORE_MAGIC, 1u, 5u, p, 0u);
	s.start_menu = true;
	CHECK(s02_store_decode(b, n, &s) && !s.start_menu);
	p[0] = 0xFFu;
	n = build(b, S02_STORE_MAGIC, 1u, (uint16_t)S02_STORE_PAYLOAD_MAX, p, 0u);
	CHECK(n == S02_SECTOR_SIZE - 4u);
	CHECK(s02_store_decode(b, sizeof(b), &s) && s.start_menu);
	/* not valid: the defaults */
	p[0] = 0x01u;
	n = build(b, S02_STORE_MAGIC, 2u, 1u, p, 0u);		/* version */
	s.start_menu = true;
	CHECK(!s02_store_decode(b, n, &s) && !s.start_menu);
	n = build(b, S02_STORE_MAGIC, 0u, 1u, p, 0u);
	CHECK(!s02_store_decode(b, n, &s) && !s.start_menu);
	n = build(b, 0x54534258u, 1u, 1u, p, 0u);		/* magic */
	CHECK(!s02_store_decode(b, n, &s) && !s.start_menu);
	n = build(b, S02_STORE_MAGIC, 1u, 0u, p, 0u);		/* length 0 */
	CHECK(!s02_store_decode(b, n, &s));
	(void)build(b, S02_STORE_MAGIC, 1u, 1u, p, 0u);
	b[6] = (uint8_t)(S02_STORE_PAYLOAD_MAX + 1u);		/* length too long */
	b[7] = (uint8_t)((S02_STORE_PAYLOAD_MAX + 1u) >> 8);
	CHECK(!s02_store_decode(b, sizeof(b), &s));
	n = build(b, S02_STORE_MAGIC, 1u, 1u, p, 0x00010000u);	/* CRC */
	CHECK(!s02_store_decode(b, n, &s) && !s.start_menu);
	n = build(b, S02_STORE_MAGIC, 1u, 1u, p, 0u);
	CHECK(!s02_store_decode(b, n - 1u, &s));		/* cut short */
	memset(b, 0xFF, sizeof(b));				/* erased */
	CHECK(!s02_store_decode(b, sizeof(b), &s) && !s.start_menu);
	memset(b, 0x00, sizeof(b));
	CHECK(!s02_store_decode(b, sizeof(b), &s) && !s.start_menu);
}

/* Every truncation point of the program: the bytes before it written, the
 * rest still erased. Only the whole record reads. */
static void test_torn_prefix(void)
{
	const struct s02_store s = { true };
	uint8_t rec[S02_STORE_WRITTEN], b[S02_SECTOR_SIZE];
	struct s02_store out;

	(void)s02_store_encode(&s, rec, sizeof(rec));
	for (size_t k = 0; k <= sizeof(rec); k++) {
		memset(b, 0xFF, sizeof(b));
		memcpy(b, rec, k);
		out.start_menu = true;
		CHECK(s02_store_decode(b, sizeof(b), &out) == (k == sizeof(rec)));
		CHECK(out.start_menu == (k == sizeof(rec)));
	}
}

static void test_place(void)
{
	CHECK(S02_STORE_OFF == 0xFE000u && S02_STORE_SIZE == 0x1000u);
	CHECK(S02_STORE_SLOT_MAX == 0x76000u);
	/* inside slot 1, below its last sector */
	CHECK(S02_STORE_OFF >= S02_SLOT1_OFF &&
	      S02_STORE_OFF + S02_STORE_SIZE == S02_SLOT1_OFF + S02_SLOT_CAPACITY - S02_SECTOR_SIZE);
}

static void test_write_ops(void)
{
	const struct s02_store on = { true }, off = { false };
	struct s02_store out;

	setup();
	snapshot();
	ff_reset(&g_f);
	CHECK(s02_store_write(&g_env, &on) == S02_STORE_OK);
	/* slot 1's record, the erase, one program within the page, the read-back */
	CHECK(g_f.nlog == 4u && !g_f.bad_call);
	CHECK(g_f.log[0].kind == FF_READ && g_f.log[0].off == REC1 &&
	      g_f.log[0].len == S02_RECORD_SIZE);
	CHECK(g_f.log[1].kind == FF_ERASE && g_f.log[1].off == OFF);
	CHECK(g_f.log[2].kind == FF_PROGRAM && g_f.log[2].off == OFF &&
	      g_f.log[2].len == S02_STORE_WRITTEN);
	CHECK(g_f.log[3].kind == FF_READ && g_f.log[3].off == OFF);
	CHECK(outside_same() && only_store_written());
	ff_reset(&g_f);
	CHECK(s02_store_read(&g_env, &out) && out.start_menu);
	/* the record, then one plain read; reading writes nothing */
	CHECK(g_f.nlog == 2u && g_f.log[1].kind == FF_READ && g_f.log[1].off == OFF &&
	      g_f.log[1].len == S02_STORE_SIZE && g_f.nmut == 0u);
	CHECK(s02_store_write(&g_env, &off) == S02_STORE_OK);
	CHECK(s02_store_read(&g_env, &out) && !out.start_menu);
	CHECK(outside_same() && only_store_written());

	/* failures: reported, and the read gives what is there */
	setup();
	ff_reset(&g_f);
	g_f.fail = 1u;					/* the erase */
	CHECK(s02_store_write(&g_env, &on) == S02_STORE_IO);
	ff_reset(&g_f);
	g_f.fail = 2u;					/* the program */
	CHECK(s02_store_write(&g_env, &on) == S02_STORE_IO);
	ff_reset(&g_f);
	CHECK(!s02_store_read(&g_env, &out) && !out.start_menu);
	ff_reset(&g_f);
	g_f.corrupt = 2u;
	g_f.corrupt_mask = 0xFEu;
	CHECK(s02_store_write(&g_env, &on) == S02_STORE_VERIFY);
	ff_reset(&g_f);
	g_f.fail_read = 2u;				/* the read-back */
	CHECK(s02_store_write(&g_env, &on) == S02_STORE_IO);
	ff_reset(&g_f);
	g_f.fail_read = 2u;				/* the sector's read */
	CHECK(!s02_store_read(&g_env, &out) && !out.start_menu);
	CHECK(g_f.nmut == 0u);
}

/* Whether slot 1's record leaves the sector alone; unusable ones make no
 * write and no read of the sector, and a refused save leaves an image that
 * covers it byte for byte. */
static void test_usable(void)
{
	static const struct {
		uint32_t len, tag;
		bool trial, usable;
	} cases[] = {
		{ 1u, S02_RECORD_TAG, false, true },
		{ 0x76000u, S02_RECORD_TAG, false, true },
		{ 0x76000u, S02_RECORD_TAG, true, true },
		{ 0x76001u, S02_RECORD_TAG, false, false },
		{ 0x76001u, S02_RECORD_TAG, true, false },
		{ 0x77000u, S02_RECORD_TAG, false, false },
		{ 0x78000u, S02_RECORD_TAG, false, false },
		{ 0x78000u, S02_RECORD_TAG, true, false },
		{ 0x78001u, S02_RECORD_TAG, false, false },
		{ 0u, S02_RECORD_TAG, false, false },
		{ 0x40000u, 0x0000B005u, false, false },
		{ 0x40000u, 0xFFFFFFFFu, false, false },
	};
	const struct s02_store on = { true };
	struct s02_store out;

	for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
		setup();
		slot1(cases[i].len, cases[i].tag, cases[i].trial);
		ff_reset(&g_f);
		CHECK(s02_store_usable(&g_env) == cases[i].usable);
		if (!cases[i].usable) {
			/* a valid record in the sector's place is not read */
			(void)s02_store_encode(&on, g_f.mem + OFF, S02_STORE_SIZE);
			snapshot();
			ff_reset(&g_f);
			CHECK(!s02_store_read(&g_env, &out) && !out.start_menu);
			CHECK(s02_store_write(&g_env, &on) == S02_STORE_UNUSABLE);
			CHECK(g_f.nmut == 0u && sector_untouched());
			CHECK(memcmp(g_before, g_f.mem, sizeof(g_before)) == 0);
		} else {
			snapshot();
			ff_reset(&g_f);
			CHECK(s02_store_write(&g_env, &on) == S02_STORE_OK);
			CHECK(s02_store_read(&g_env, &out) && out.start_menu);
			CHECK(outside_same() && only_store_written());
		}
	}
	/* an erased record, and one that cannot be read */
	setup();
	memset(g_f.mem + REC1, 0xFF, S02_SECTOR_SIZE);
	snapshot();
	ff_reset(&g_f);
	CHECK(!s02_store_usable(&g_env));
	CHECK(s02_store_write(&g_env, &on) == S02_STORE_UNUSABLE && g_f.nmut == 0u);
	setup();
	snapshot();
	ff_reset(&g_f);
	g_f.fail_regions = FF_R_RECORD1;
	CHECK(!s02_store_read(&g_env, &out) && !out.start_menu && sector_untouched());
	CHECK(s02_store_write(&g_env, &on) == S02_STORE_UNUSABLE);
	CHECK(g_f.nmut == 0u && memcmp(g_before, g_f.mem, sizeof(g_before)) == 0);
}

/* A value the next start reads after a cut: old, new or the defaults; and
 * nothing outside the sector changed. */
static void after_cut(bool old, bool now)
{
	struct s02_store out;
	bool valid;

	ff_reset(&g_f);
	valid = s02_store_read(&g_env, &out);
	CHECK(!valid || out.start_menu == old || out.start_menu == now);
	if (!valid)
		CHECK(!out.start_menu);
	CHECK(outside_same());
}

static void test_cuts(void)
{
	for (int from = 0; from < 3; from++) {
		/* from: the old value off, on, or an erased sector */
		const struct s02_store old = { from == 1 }, now = { from != 1 };

		/* a cut after each operation, the reads counted too */
		for (unsigned cut = 1; cut <= 4u; cut++) {
			struct s02_store out;

			setup();
			if (from != 2)
				(void)s02_store_write(&g_env, &old);
			snapshot();
			ff_reset(&g_f);
			g_f.count_reads = true;
			g_f.cut_after = cut;
			(void)s02_store_write(&g_env, &now);
			CHECK(only_store_written());
			ff_reset(&g_f);
			/* 1: after the record's read; 2: after the erase */
			CHECK(s02_store_read(&g_env, &out) == (cut != 2u && (cut != 1u || from != 2)));
			CHECK(out.start_menu == (cut == 1u ? (from != 2 && old.start_menu) :
						 cut == 2u ? false : now.start_menu));
			after_cut(old.start_menu, now.start_menu);
		}
		/* the erase or the program torn, many ways */
		for (unsigned op = 1; op <= 2u; op++) {
			for (uint32_t seed = 0; seed < 64u; seed++) {
				setup();
				if (from != 2)
					(void)s02_store_write(&g_env, &old);
				snapshot();
				ff_reset(&g_f);
				g_f.tear = op;
				g_f.tear_kind = FF_TEAR_SEED;
				g_f.tear_a = seed;
				CHECK(s02_store_write(&g_env, &now) == S02_STORE_IO);
				CHECK(only_store_written());
				after_cut(old.start_menu, now.start_menu);
			}
			for (uint32_t byte = 0; byte < S02_STORE_WRITTEN; byte++) {
				for (uint32_t bitn = 0; bitn < 8u; bitn++) {
					setup();
					if (from != 2)
						(void)s02_store_write(&g_env, &old);
					snapshot();
					ff_reset(&g_f);
					g_f.tear = op;
					g_f.tear_kind = FF_TEAR_BIT;
					g_f.tear_a = byte;
					g_f.tear_b = bitn;
					(void)s02_store_write(&g_env, &now);
					after_cut(old.start_menu, now.start_menu);
				}
			}
		}
	}
}

/* From every starting content of the sector, a save erases and programs
 * that sector alone. */
static void test_any_content(void)
{
	const struct s02_store on = { true };

	for (uint32_t seed = 0; seed < 16u; seed++) {
		setup();
		if (seed == 0u)
			memset(g_f.mem + OFF, 0x00, S02_STORE_SIZE);
		else
			ff_fill_image(g_f.mem + OFF, S02_STORE_SIZE, seed);
		snapshot();
		ff_reset(&g_f);
		CHECK(s02_store_write(&g_env, &on) == S02_STORE_OK);
		CHECK(only_store_written() && outside_same());
		CHECK(memcmp(g_before + REC1, g_f.mem + REC1, S02_SECTOR_SIZE) == 0 &&
		      memcmp(g_before + OFF + S02_STORE_SIZE, g_f.mem + OFF + S02_STORE_SIZE,
			     S02_SECTOR_SIZE) == 0);
	}
}

int main(void)
{
	RUN(test_place);
	RUN(test_encode);
	RUN(test_decode);
	RUN(test_torn_prefix);
	RUN(test_write_ops);
	RUN(test_usable);
	RUN(test_cuts);
	RUN(test_any_content);
	return check_summary("test_s02_store");
}
