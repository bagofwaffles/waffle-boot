/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-02 metadata mirror: format, write order, restore and power cuts.
 * Requirements: S-02.004 (mirror tail), S-02.018, S-02.041, S-02.060-.065.
 * Vectors: metadata_writes.json, metadata_cuts.csv (rule conforming; the
 * seed patterns are replaced by seeded tears of the C fake);
 * scenarios SC-META-WRITE, SC-META-CUT.
 */
#include <string.h>

#include "s02_check.h"
#include "s02_fake_flash.h"
#include "vectors/s02_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static struct fake_flash g_f;
static struct s02_flash_ops g_ops;
static uint8_t g_start_sector[S02_SECTOR_SIZE];

/* Start states of metadata_cuts.csv: 0 mirror_old, 1 mirror_absent, 2 pending_old, 3 pending_erased. */
static void setup(int start)
{
	ff_init(&g_f);
	if (start != 3) {
		memcpy(g_f.mem + S02_META_OFF, v_meta_old_record, S02_META_SIZE);
		memcpy(g_f.mem + S02_META_OFF + v_meta_tail0_off, v_meta_tail0, sizeof v_meta_tail0);
		memcpy(g_f.mem + S02_META_OFF + v_meta_tail1_off, v_meta_tail1, sizeof v_meta_tail1);
	}
	if (start == 0)
		memcpy(g_f.mem + S02_MIRROR_OFF, v_meta_mirror_of_old, S02_MIRROR_SIZE);
	if (start >= 2) {
		uint8_t mirror[S02_MIRROR_SIZE];

		s02_mirror_build(v_meta_new_record, mirror);
		memcpy(g_f.mem + S02_MIRROR_OFF, mirror, S02_MIRROR_SIZE);
	}
	memcpy(g_start_sector, g_f.mem + S02_META_OFF, S02_SECTOR_SIZE);
	ff_reset(&g_f);
}

/* The bytes beyond the record: the vectors' tail, the rest erased. */
static bool tail_kept(void)
{
	static uint8_t want[S02_SECTOR_SIZE];

	memset(want, 0xFF, sizeof want);
	memcpy(want + v_meta_tail0_off, v_meta_tail0, sizeof v_meta_tail0);
	memcpy(want + v_meta_tail1_off, v_meta_tail1, sizeof v_meta_tail1);
	return memcmp(g_f.mem + S02_META_OFF + S02_META_SIZE, want + S02_META_SIZE,
		      S02_SECTOR_SIZE - S02_META_SIZE) == 0;
}

/* S-02.062: the factory fields, +0x04-+0x11 and +0x28-+0x33, as before. */
static bool factory_kept(const uint8_t *rec)
{
	return memcmp(rec + 0x04, v_meta_old_record + 0x04, 0x0E) == 0 &&
	       memcmp(rec + 0x28, v_meta_old_record + 0x28, 0x0C) == 0;
}

/* S-02.063 */
static void test_format(void)
{
	uint8_t m[S02_MIRROR_SIZE];
	unsigned i, b;

	s02_mirror_build(v_meta_old_record, m);
	CHECK(memcmp(m, v_meta_mirror_of_old, S02_MIRROR_SIZE) == 0);
	CHECK(s02_mirror_valid(m));
	/* Every single raised bit of a valid mirror invalidates it. */
	for (i = 0; i < S02_MIRROR_SIZE; i++)
		for (b = 0; b < 8u; b++) {
			if (m[i] & (1u << b))
				continue;
			m[i] = (uint8_t)(m[i] | (1u << b));
			CHECK(!s02_mirror_valid(m));
			m[i] = (uint8_t)(m[i] & ~(1u << b));
		}
	memset(m, 0xFF, sizeof m);
	CHECK(!s02_mirror_valid(m));
	/* The complement pairs alone do not make it valid: the magic is needed. */
	{
		uint8_t rec[S02_META_SIZE];

		memcpy(rec, v_meta_old_record, sizeof rec);
		rec[0] = 0;
		s02_mirror_build(rec, m);
		CHECK(!s02_mirror_valid(m));
	}
}

static bool ops_equal(const struct v_op *ops, unsigned nops)
{
	unsigned i, k = 0;

	/* Mutating ops exactly; reads in between are allowed (they are logged). */
	for (i = 0; i < nops; i++) {
		if (ops[i].kind == 'r')
			continue;
		while (k < g_f.nlog && g_f.log[k].kind == FF_READ)
			k++;
		if (k >= g_f.nlog || g_f.log[k].kind != ops[i].kind ||
		    g_f.log[k].off != ops[i].off || g_f.log[k].len != ops[i].len)
			return false;
		k++;
	}
	while (k < g_f.nlog && g_f.log[k].kind == FF_READ)
		k++;
	return k == g_f.nlog;
}

/* SC-META-WRITE (S-02.060, .061, .063, .064) */
static void test_write(void)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	int start;

	for (start = 0; start < 2; start++) {
		uint8_t m[S02_MIRROR_SIZE];
		unsigned i, me, mp, ce, j;
		bool restored;

		setup(start);
		CHECK_EQ(s02_meta_write(&env, v_meta_new_record), S02_OK);
		CHECK(ops_equal(v_meta_write_ops, N(v_meta_write_ops)));
		CHECK(memcmp(g_f.mem + S02_META_OFF, v_meta_new_record, S02_META_SIZE) == 0);
		CHECK(tail_kept());				/* S-02.061 */
		s02_mirror_build(v_meta_new_record, m);
		CHECK(memcmp(g_f.mem + S02_MIRROR_OFF, m, S02_MIRROR_SIZE) == 0);
		for (i = S02_MIRROR_SIZE; i < S02_SECTOR_SIZE; i++)
			CHECK_EQ(g_f.mem[S02_MIRROR_OFF + i], 0xFFu);	/* S-02.004 */
		/* The read-backs (S-02.064 steps 3 and 6). */
		me = ~0u, mp = ~0u, ce = ~0u;
		for (i = 0; i < g_f.nlog; i++) {
			if (g_f.log[i].kind == FF_ERASE && g_f.log[i].off == S02_MIRROR_OFF)
				me = i;
			if (g_f.log[i].kind == FF_PROGRAM && g_f.log[i].off == S02_MIRROR_OFF)
				mp = i;
			if (g_f.log[i].kind == FF_ERASE && g_f.log[i].off == S02_META_OFF)
				ce = i;
		}
		CHECK(me < mp && mp < ce);
		CHECK(g_f.log[mp + 1u].kind == FF_READ && g_f.log[mp + 1u].off == S02_MIRROR_OFF &&
		      g_f.log[mp + 1u].len == S02_MIRROR_SIZE);
		for (j = 0; j < S02_SECTOR_SIZE; j += S02_PAGE_SIZE) {
			bool seen = false;

			for (i = ce; i < g_f.nlog; i++)
				if (g_f.log[i].kind == FF_READ && g_f.log[i].off <= S02_META_OFF + j &&
				    g_f.log[i].off + g_f.log[i].len > S02_META_OFF + j)
					seen = true;
			CHECK(seen);
		}
		/* A completed write needs no restore. */
		ff_reset(&g_f);
		CHECK_EQ(s02_meta_restore(&env, &restored), S02_OK);
		CHECK(!restored);
		CHECK_EQ(g_f.nmut, 0);
	}
	/* A failed mirror read-back stops the write before 0xC000 is erased. */
	setup(0);
	g_f.corrupt = 2;	/* the mirror program */
	g_f.corrupt_mask = 0xEF;
	CHECK_EQ(s02_meta_write(&env, v_meta_new_record), S02_ERR_VERIFY);
	CHECK(memcmp(g_f.mem + S02_META_OFF, g_start_sector, S02_SECTOR_SIZE) == 0);
	/* A failed 0xC000 read-back is repeated from step 4, from the same copy. */
	setup(0);
	g_f.corrupt = 4;	/* the first program of 0xC000 */
	g_f.corrupt_mask = 0x00;
	CHECK_EQ(s02_meta_write(&env, v_meta_new_record), S02_OK);
	CHECK(memcmp(g_f.mem + S02_META_OFF, v_meta_new_record, S02_META_SIZE) == 0);
	CHECK(tail_kept());
}

/* SC-META-CUT restores and the stale mirror (S-02.065) */
static void test_restore(void)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	uint8_t m[S02_MIRROR_SIZE];
	bool restored;

	setup(2);
	CHECK_EQ(s02_meta_restore(&env, &restored), S02_OK);
	CHECK(restored);
	CHECK(ops_equal(v_meta_restore_old_ops, N(v_meta_restore_old_ops)));
	CHECK(memcmp(g_f.mem + S02_META_OFF, v_meta_new_record, S02_META_SIZE) == 0);
	CHECK(tail_kept());
	setup(3);
	CHECK_EQ(s02_meta_restore(&env, &restored), S02_OK);
	CHECK(restored);
	CHECK(ops_equal(v_meta_restore_erased_ops, N(v_meta_restore_erased_ops)));
	CHECK(memcmp(g_f.mem + S02_META_OFF, v_meta_new_record, S02_META_SIZE) == 0);
	/* No valid mirror: 0xC000 is used as it is, nothing written. */
	setup(1);
	CHECK_EQ(s02_meta_restore(&env, &restored), S02_OK);
	CHECK(!restored);
	CHECK_EQ(g_f.nmut, 0);
	/* The stale mirror: a writer without a mirror changed 0xC000; the mirror wins. */
	setup(0);
	memcpy(g_f.mem + S02_META_OFF, v_meta_other_writer_record, S02_META_SIZE);
	CHECK_EQ(s02_meta_restore(&env, &restored), S02_OK);
	CHECK(restored);
	CHECK(memcmp(g_f.mem + S02_META_OFF, v_meta_old_record, S02_META_SIZE) == 0);
	/* A restore never erases or programs the mirror sector. */
	s02_mirror_build(v_meta_old_record, m);
	CHECK(memcmp(g_f.mem + S02_MIRROR_OFF, m, S02_MIRROR_SIZE) == 0);
}

/* After a cut: the next boot's restore, then the record it presents. */
static void boot_and_check(const struct v_mcut *c, bool *restored, int *result)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	uint8_t mirror[S02_MIRROR_SIZE];

	memcpy(mirror, g_f.mem + S02_MIRROR_OFF, S02_MIRROR_SIZE);
	if (c != NULL)
		CHECK_EQ(s02_mirror_valid(mirror), (unsigned)c->mirror_valid);
	ff_reset(&g_f);
	CHECK_EQ(s02_meta_restore(&env, restored), S02_OK);
	/* S-02.062: the magic, the factory fields, each field old or new. */
	CHECK(memcmp(g_f.mem + S02_META_OFF, v_meta_old_record, 4u) == 0);
	CHECK(factory_kept(g_f.mem + S02_META_OFF));
	if (memcmp(g_f.mem + S02_META_OFF, v_meta_new_record, S02_META_SIZE) == 0)
		*result = 1;
	else if (memcmp(g_f.mem + S02_META_OFF, v_meta_old_record, S02_META_SIZE) == 0)
		*result = 0;
	else
		*result = -1;
	CHECK(*result >= 0);
}

static void apply_cut(const struct v_mcut *c, unsigned nops)
{
	unsigned n = c->op_n;

	g_f.count_reads = true;
	if (c->tear != 0) {
		g_f.tear = n;
		g_f.tear_kind = c->tear == 1 ? FF_TEAR_ZEROS : c->tear == 2 ? FF_TEAR_ONES : FF_TEAR_BIT;
		g_f.tear_a = c->a;
		g_f.tear_b = c->b;
	} else if (n < nops) {
		g_f.cut_after = n;	/* the ops before the final read-back coincide */
	}
}

/* metadata_cuts.csv, rule conforming */
static void test_cuts(void)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned i;

	for (i = 0; i < N(v_meta_cuts); i++) {
		const struct v_mcut *c = &v_meta_cuts[i];
		bool restored;
		int result;

		setup(c->start);
		if (c->op_n != 0u) {
			if (c->restore) {
				unsigned nops = c->start == 2 ? N(v_meta_restore_old_ops) :
							N(v_meta_restore_erased_ops);

				apply_cut(c, nops);
				(void)s02_meta_restore(&env, &restored);
			} else {
				apply_cut(c, N(v_meta_write_ops));
				(void)s02_meta_write(&env, v_meta_new_record);
			}
		}
		boot_and_check(c, &restored, &result);
		CHECK_EQ(restored, (unsigned)c->restored);
		CHECK_EQ(result, c->result);
		CHECK_EQ(tail_kept(), (unsigned)c->tail_kept);
	}

	/* Seeded tears of every erase and program of the write and the restores. */
	for (i = 0; i < 3u; i++) {
		static const int starts[] = {0, 1, 2, 3};
		unsigned s;

		for (s = 0; s < N(starts); s++) {
			unsigned total, n;

			/* Count the run's ops once. */
			setup(starts[s]);
			if (starts[s] < 2)
				(void)s02_meta_write(&env, v_meta_new_record);
			else {
				bool r;

				(void)s02_meta_restore(&env, &r);
			}
			total = g_f.nall;
			for (n = 1; n <= total; n++) {
				bool restored;
				int result;

				setup(starts[s]);
				g_f.count_reads = true;
				g_f.tear = n;		/* a read at n is a plain cut there */
				g_f.cut_after = n;
				g_f.tear_kind = FF_TEAR_SEED;
				g_f.tear_a = 1000u + i * 64u + n;
				if (starts[s] < 2)
					(void)s02_meta_write(&env, v_meta_new_record);
				else
					(void)s02_meta_restore(&env, &restored);
				boot_and_check(NULL, &restored, &result);
				/* A boot after a cut of a restore restores again until done. */
				ff_reset(&g_f);
				CHECK_EQ(s02_meta_restore(&env, &restored), S02_OK);
				CHECK(!restored);
			}
		}
	}
}

int main(void)
{
	test_format();
	test_write();
	test_restore();
	test_cuts();
	return finish("test_s02_meta");
}
