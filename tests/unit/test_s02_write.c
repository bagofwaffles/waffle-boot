/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-02 writers against the RAM fake: the bootloader-slot upgrade and its
 * power cuts, the start rule, the selector-write rule, the first-stage
 * upgrade and the option gate.
 * Requirements: S-02.004, S-02.024 (factory model), S-02.041-.053,
 * S-02.055-.058, S-02.070, S-02.071, S-02.090, S-02.091, S-02.094.
 * Vectors: powercut_sequences.json, powercut_cuts.csv, upgrade_cut_boots.csv,
 * package_steps.json, bootable_images.json (vectors/s02_vectors.h);
 * scenarios SC-UPG-OK, SC-UPG-SIZE, SC-UPG-STREAM, SC-UPG-FAULT, SC-UPG-CUT,
 * SC-UPG-START, SC-SEL-WRITE,
 * SC-UPG-SKIP, SC-UPG-NEXT-BOOT, SC-FS-UPG, SC-FS-FAULT, SC-OPT-RESET.
 */
#include <string.h>

#include "s02_check.h"
#include "s02_fake_flash.h"
#include "vectors/s02_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static struct fake_flash g_f;
static struct fake_flash g_start;
static struct s02_flash_ops g_ops;
static uint8_t g_image[S02_SLOT_CAPACITY + 1u];

/* --- op-log helpers --- */

static unsigned count_kind(char kind)
{
	unsigned i, n = 0;

	for (i = 0; i < g_f.nlog; i++)
		if (g_f.log[i].kind == kind)
			n++;
	return n;
}

/* Every byte of [off, off + len) is covered by reads with log index in (lo, hi). */
static bool reads_cover(uint32_t off, uint32_t len, unsigned lo, unsigned hi)
{
	static uint8_t seen[S02_FLASH_SIZE];
	unsigned i;
	uint32_t j;

	memset(seen + off, 0, len);
	for (i = lo + 1u; i < hi && i < g_f.nlog; i++) {
		const struct ff_op *o = &g_f.log[i];

		if (o->kind != FF_READ)
			continue;
		for (j = o->off; j < o->off + o->len; j++)
			if (j >= off && j < off + len)
				seen[j] = 1;
	}
	for (j = off; j < off + len; j++)
		if (!seen[j])
			return false;
	return true;
}

/* The mutating ops of the log equal the vector's, in order. */
static bool mut_ops_equal(const struct v_pcop *ops, unsigned nops)
{
	unsigned i, k = 0;

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

static unsigned find_op(char kind, uint32_t off, unsigned from)
{
	unsigned i;

	for (i = from; i < g_f.nlog; i++)
		if (g_f.log[i].kind == kind && g_f.log[i].off == off)
			return i;
	return ~0u;
}

static unsigned erases_at(uint32_t off)
{
	unsigned i, k = 0;

	for (i = 0; i < g_f.nlog; i++)
		k += g_f.log[i].kind == FF_ERASE && g_f.log[i].off == off;
	return k;
}

static void set_state(unsigned slot, unsigned state, uint32_t seed)
{
	ff_set_slot(&g_f, slot, v_states[state], 0x1345u, seed);
}

/* --- SC-UPG-OK (S-02.004, S-02.041-.049, S-02.053) --- */

static void check_upgrade_log(unsigned t, uint32_t n)
{
	uint32_t base = s02_slot_base(t), rec = s02_record_off(t);
	uint32_t last = base + S02_SLOT_CAPACITY - S02_SECTOR_SIZE;
	unsigned i, rec_erase, rec_prog, sel_erase, sel_prog, last_slot = 0, last_prog = 0;
	uint32_t sectors = (n + S02_SECTOR_SIZE - 1u) / S02_SECTOR_SIZE;
	bool log_erase = n <= S02_SLOT_CAPACITY - S02_SECTOR_SIZE;
	static uint8_t programmed[S02_SLOT_CAPACITY];

	rec_erase = find_op(FF_ERASE, rec, 0);
	rec_prog = find_op(FF_PROGRAM, rec, 0);
	sel_erase = find_op(FF_ERASE, S02_SELECTOR_OFF, 0);
	sel_prog = find_op(FF_PROGRAM, S02_SELECTOR_OFF, 0);
	CHECK(rec_erase < rec_prog && rec_prog < sel_erase && sel_erase < sel_prog);
	CHECK(sel_prog != ~0u);
	CHECK_EQ(g_f.log[rec_prog].len, S02_RECORD_SIZE);
	CHECK_EQ(g_f.log[sel_prog].len, 2);
	/* S-02.053: one selector erase; S-02.045: the last sector once more if free. */
	CHECK_EQ(count_kind(FF_ERASE), sectors + (log_erase ? 1u : 0u) + 2u);
	memset(programmed, 0, sizeof programmed);
	for (i = 0; i < g_f.nlog; i++) {
		const struct ff_op *o = &g_f.log[i];
		uint32_t j;

		if (o->kind == FF_READ)
			continue;
		if (o->off == rec || o->off == S02_SELECTOR_OFF)
			continue;
		/* S-02.042: nothing outside the target slot's sectors. */
		CHECK(o->off >= base && o->off < base + S02_SLOT_CAPACITY);
		CHECK(i < rec_erase);			/* S-02.049 class 1 first */
		last_slot = i;
		if (o->kind == FF_ERASE)
			continue;
		/* S-02.045: each program after its sector's erase, once per byte. */
		CHECK(find_op(FF_ERASE, o->off - (o->off % S02_SECTOR_SIZE), 0) < i);
		last_prog = i;
		for (j = o->off - base; j < o->off - base + o->len; j++) {
			CHECK(programmed[j] == 0);
			programmed[j] = 1;
		}
	}
	for (i = 0; i < n; i++)
		if (!programmed[i])
			break;
	CHECK_EQ(i, n);
	CHECK(last_slot < rec_erase);
	/* S-02.045: a free last sector is erased once, after the last program, never programmed. */
	if (log_erase) {
		i = find_op(FF_ERASE, last, 0);
		CHECK(i > last_prog && i < rec_erase);
		CHECK_EQ(find_op(FF_ERASE, last, i + 1u), ~0u);
		CHECK_EQ(find_op(FF_PROGRAM, last, 0), ~0u);
		CHECK_EQ(last_slot, i);
	}
	/* S-02.047: the read-back strictly between the record program and the selector erase. */
	CHECK(reads_cover(rec, S02_RECORD_SIZE, rec_prog, sel_erase));
	CHECK(reads_cover(base, n, rec_prog, sel_erase));
	CHECK(!g_f.bad_call);
}

static void test_upgrade_ok(void)
{
	static const uint8_t starts[][2] = {{0x00, 0x01}, {0x01, 0x01}, {0x02, 0x01}, {0xFF, 0x01}};
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned i, t;
	uint32_t n = 0x2345u, j;

	for (i = 0; i < N(starts); i++) {
		unsigned sel = s02_selected_slot(starts[i][0]);
		struct s02_fs_outcome o;
		int rc;

		ff_init(&g_f);
		ff_set_slot(&g_f, sel, "good", 0x1345u, 1u);		/* image A */
		ff_set_slot(&g_f, 1u - sel, "good", S02_SLOT_CAPACITY, 7u);	/* image C */
		ff_set_selector(&g_f, starts[i][0], starts[i][1]);
		memcpy(&g_start.mem, g_f.mem, sizeof g_f.mem);
		ff_boot_image(g_image, n, 42u);				/* image B */
		ff_reset(&g_f);
		rc = s02_slot_upgrade(&env, true, g_image, n, &t);
		CHECK_EQ(rc, S02_OK);
		CHECK_EQ(t, starts[i][0] == 0u ? 1u : 0u);		/* S-02.043 */
		check_upgrade_log(t, n);
		/* Final flash. */
		CHECK(memcmp(g_f.mem + s02_slot_base(t), g_image, n) == 0);
		for (j = n; j < 0x3000u; j++)
			CHECK_EQ(g_f.mem[s02_slot_base(t) + j], 0xFFu);
		CHECK(memcmp(g_f.mem + s02_slot_base(t) + 0x3000u,
			     g_start.mem + s02_slot_base(t) + 0x3000u,
			     S02_SLOT_CAPACITY - S02_SECTOR_SIZE - 0x3000u) == 0);	/* S-02.045, S-16.029 */
		for (j = S02_SLOT_CAPACITY - S02_SECTOR_SIZE; j < S02_SLOT_CAPACITY; j++)
			CHECK_EQ(g_f.mem[s02_slot_base(t) + j], 0xFFu);	/* the last sector, erased */
		for (j = S02_RECORD_SIZE; j < S02_SECTOR_SIZE; j++)
			CHECK_EQ(g_f.mem[s02_record_off(t) + j], 0xFFu);	/* S-02.004 */
		for (j = 2; j < S02_SECTOR_SIZE; j++)
			CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + j], 0xFFu);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], t);			/* S-02.048 */
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], 0x00u);
		CHECK(memcmp(g_f.mem + s02_slot_base(sel), g_start.mem + s02_slot_base(sel),
			     S02_SLOT_CAPACITY) == 0);			/* S-02.042 */
		CHECK(memcmp(g_f.mem + s02_record_off(sel), g_start.mem + s02_record_off(sel),
			     S02_SECTOR_SIZE) == 0);
		CHECK(memcmp(g_f.mem, g_start.mem, S02_SELECTOR_OFF) == 0);	/* S-02.041 */
		CHECK(ff_slot_good(&g_f, t));
		/* A second upgrade in the same boot is refused (S-02.090). */
		ff_reset(&g_f);
		{
			unsigned t2;

			CHECK_EQ(s02_slot_upgrade(&env, true, g_image, n, &t2), S02_ERR_START);
		}
		CHECK_EQ(g_f.nmut, 0);
		/* After the reset, slot T boots under either profile. */
		memcpy(&g_start.mem, g_f.mem, sizeof g_f.mem);
		ff_reset(&g_f);
		s02_first_stage(&env, false, &o);
		CHECK_EQ(o.boot_slot, t);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], t);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], 0x01u);
		memcpy(g_f.mem, g_start.mem, sizeof g_f.mem);
		ff_reset(&g_f);
		CHECK_EQ(ff_factory_first_stage(&env), t);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], 0x01u);
	}
}

/*
 * S-02.044: an image named only by its size is bootable_images.json's
 * case followed by 0xFF bytes up to that size (its `padding` byte).
 */
static void padded(uint8_t *dst, const uint8_t *img, uint32_t len, uint32_t n)
{
	memset(dst, 0xFF, n);
	memcpy(dst, img, len < n ? len : n);
}

/* SC-UPG-SIZE (S-02.044), SC-UPG-SKIP and SC-OPT-RESET (S-02.055-.057, .059). */
static void test_upgrade_size_and_option(void)
{
	static const uint32_t sizes[] = {0u, 1u, sizeof v_boot_small_slot,
					 S02_SLOT_CAPACITY, S02_SLOT_CAPACITY + 1u};
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned i, t;

	for (i = 0; i < N(sizes); i++) {
		int rc;

		ff_init(&g_f);
		set_state(0, 0, 1u);
		set_state(1, 0, 2u);
		ff_set_selector(&g_f, 0x01, 0x01);
		memcpy(&g_start.mem, g_f.mem, sizeof g_f.mem);
		if (sizes[i] == 1u)
			padded(g_image, v_boot_one_byte, sizeof v_boot_one_byte, 1u);
		else
			padded(g_image, v_boot_small_slot, sizeof v_boot_small_slot,
			       sizes[i]);
		ff_reset(&g_f);
		rc = s02_slot_upgrade(&env, true, g_image, sizes[i], &t);
		if (sizes[i] == 0u || sizes[i] > S02_SLOT_CAPACITY) {
			CHECK_EQ(rc, S02_ERR_SIZE);
			CHECK_EQ(g_f.nmut, 0);
			CHECK(memcmp(g_f.mem, g_start.mem, sizeof g_f.mem) == 0);
		} else if (sizes[i] == 1u) {
			/* S-02.044, S-02.091: no block header fits: Bad file. */
			CHECK_EQ(rc, S02_ERR_STREAM);
			CHECK_EQ(g_f.nmut, 0);
			CHECK(memcmp(g_f.mem, g_start.mem, sizeof g_f.mem) == 0);
		} else {
			CHECK_EQ(rc, S02_OK);
			check_upgrade_log(0, sizes[i]);
			if (sizes[i] == S02_SLOT_CAPACITY)
				CHECK_EQ(count_kind(FF_ERASE), 120u + 2u);
		}
	}
	/* The option off: skipped, and the flash is not touched at all. */
	ff_init(&g_f);
	set_state(0, 0, 1u);
	set_state(1, 0, 2u);
	ff_set_selector(&g_f, 0x01, 0x01);
	ff_reset(&g_f);
	CHECK_EQ(s02_slot_upgrade(&env, false, g_image, 0x100u, &t), S02_SKIPPED);
	CHECK_EQ(g_f.nlog, 0);
	CHECK_EQ(t, 2u);
	CHECK_EQ(s02_first_stage_upgrade(&env, false, g_image, 0x86C0u, NULL, NULL), S02_SKIPPED);
	CHECK_EQ(g_f.nlog, 0);
}

/* SC-UPG-FAULT (S-02.047): no selector write after a fault. */
static void test_upgrade_fault(void)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	uint32_t n = 0x2345u;
	unsigned c, t;

	for (c = 1; c <= 6u; c++) {
		struct s02_fs_outcome o;
		int rc;

		ff_init(&g_f);
		set_state(0, 0, 1u);
		set_state(1, 0, 2u);
		ff_set_selector(&g_f, 0x01, 0x01);
		memcpy(&g_start.mem, g_f.mem, sizeof g_f.mem);
		ff_boot_image(g_image, n, 11u);
		ff_reset(&g_f);
		/*
		 * Ops: 1 erase, 2-17 programs, 18 erase, 19-34, 35 erase, 36-39,
		 * 40 the last sector's erase, 41 record erase, 42 its program.
		 */
		switch (c) {
		case 1: g_f.fail = 1; break;			/* the first slot erase */
		case 2: g_f.fail = 10; break;			/* a mid-slot program */
		case 3: g_f.corrupt = 10; g_f.corrupt_mask = 0xFE; break;
		case 4: g_f.fail = 42; break;			/* the record program */
		case 5: g_f.corrupt = 42; g_f.corrupt_mask = 0xFE; break;
		default: g_f.fail = 40; break;			/* the last sector's erase */
		}
		rc = s02_slot_upgrade(&env, true, g_image, n, &t);
		CHECK(rc != S02_OK);
		if (c == 6u) {
			CHECK_EQ(rc, S02_ERR_IO);
			CHECK_EQ(find_op(FF_ERASE, S02_RECORD0_OFF, 0), ~0u);
		}
		CHECK_EQ(find_op(FF_ERASE, S02_SELECTOR_OFF, 0), ~0u);
		CHECK_EQ(find_op(FF_PROGRAM, S02_SELECTOR_OFF, 0), ~0u);
		CHECK(memcmp(g_f.mem + S02_SELECTOR_OFF, g_start.mem + S02_SELECTOR_OFF,
			     S02_SECTOR_SIZE) == 0);
		CHECK(memcmp(g_f.mem + S02_SLOT1_OFF, g_start.mem + S02_SLOT1_OFF,
			     S02_SLOT_CAPACITY) == 0);
		CHECK(memcmp(g_f.mem + S02_RECORD1_OFF, g_start.mem + S02_RECORD1_OFF,
			     S02_SECTOR_SIZE) == 0);
		CHECK(memcmp(g_f.mem, g_start.mem, S02_SELECTOR_OFF) == 0);
		if (c == 4u || c == 5u)	/* the record program, by its offset */
			CHECK(find_op(FF_PROGRAM, S02_RECORD0_OFF, 0) != ~0u);
		memcpy(&g_start.mem, g_f.mem, sizeof g_f.mem);
		ff_reset(&g_f);
		s02_first_stage(&env, false, &o);
		CHECK_EQ(o.boot_slot, 1u);
		memcpy(g_f.mem, g_start.mem, sizeof g_f.mem);
		ff_reset(&g_f);
		CHECK_EQ(ff_factory_first_stage(&env), 1u);
		/* A retry repeats the writes from the start and succeeds. */
		memcpy(g_f.mem, g_start.mem, sizeof g_f.mem);
		ff_reset(&g_f);
		CHECK_EQ(s02_slot_upgrade(&env, true, g_image, n, &t), S02_OK);
		CHECK(ff_slot_good(&g_f, 0));
	}
}

/*
 * S-02.048: the selector is read back after its program. A part that ignores
 * the program, or a read-back that fails, fails the step; the selector
 * still names the slot it named, so the old slot goes on starting.
 */
static void test_selector_read_back(void)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	uint32_t n = 0x800u;
	unsigned t, last, c;

	ff_init(&g_f);
	set_state(0, 0, 1u);
	set_state(1, 0, 2u);
	ff_set_selector(&g_f, 0x01, 0x01);
	memcpy(&g_start.mem, g_f.mem, sizeof g_f.mem);
	ff_reset(&g_f);
	CHECK_EQ(s02_slot_upgrade(&env, true, g_image, n, &t), S02_OK);
	last = g_f.nmut;	/* the selector's program */
	CHECK(g_f.log[g_f.nlog - 1u].kind == FF_READ &&
	      g_f.log[g_f.nlog - 1u].off == S02_SELECTOR_OFF);
	for (c = 0; c < 2u; c++) {
		memcpy(g_f.mem, g_start.mem, sizeof g_f.mem);
		ff_reset(&g_f);
		if (c == 0u)
			g_f.ignore = last;	/* the program stores nothing */
		else
			g_f.fail_read = 0;	/* set below, by count */
		if (c == 1u) {
			unsigned reads;

			CHECK_EQ(s02_slot_upgrade(&env, true, g_image, n, &t), S02_OK);
			reads = g_f.nreads;	/* the last read is the read-back */
			memcpy(g_f.mem, g_start.mem, sizeof g_f.mem);
			ff_reset(&g_f);
			g_f.fail_read = reads;
			CHECK_EQ(s02_slot_upgrade(&env, true, g_image, n, &t), S02_ERR_IO);
			continue;
		}
		CHECK_EQ(s02_slot_upgrade(&env, true, g_image, n, &t), S02_ERR_VERIFY);
		/* the erased selector names slot 1, the slot that started */
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], 0xFFu);
		CHECK_EQ(s02_selected_slot(g_f.mem[S02_SELECTOR_OFF]), 1u);
	}
}

/* SC-UPG-START (S-02.090) and SC-SEL-WRITE (S-02.050). */
static void test_start_and_selector_rule(void)
{
	static const struct { uint8_t sel[2]; unsigned s1; } starts[] = {
		{{0x00, 0x00}, 0}, {{0x01, 0x00}, 0}, {{0xFF, 0xFF}, 0}, {{0xFF, 0xFF}, 1},
	};
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned i, t;

	for (i = 0; i < N(starts); i++) {
		struct s02_fs_outcome o;

		ff_init(&g_f);
		set_state(0, 0, 1u);
		set_state(1, starts[i].s1, 2u);
		ff_set_selector(&g_f, starts[i].sel[0], starts[i].sel[1]);
		memcpy(&g_start.mem, g_f.mem, sizeof g_f.mem);
		ff_boot_image(g_image, 0x800u, 5u);
		ff_reset(&g_f);
		CHECK_EQ(s02_slot_upgrade(&env, true, g_image, 0x800u, &t), S02_ERR_START);
		CHECK_EQ(g_f.nmut, 0);
		CHECK(memcmp(g_f.mem, g_start.mem, sizeof g_f.mem) == 0);
		ff_reset(&g_f);	/* a retry in the same boot */
		CHECK_EQ(s02_slot_upgrade(&env, true, g_image, 0x800u, &t), S02_ERR_START);
		CHECK_EQ(g_f.nmut, 0);
		/* After a reset that boots a good slot the same package installs. */
		ff_reset(&g_f);
		s02_first_stage(&env, false, &o);
		CHECK(ff_slot_good(&g_f, o.boot_slot));
		ff_reset(&g_f);
		CHECK_EQ(s02_slot_upgrade(&env, true, g_image, 0x800u, &t), S02_OK);
		check_upgrade_log(t, 0x800u);
		CHECK(t != o.boot_slot);
	}
	/*
	 * Run 4 (S-02 Q9(c)): verified 0x01 but the selected slot bad (C-10, C-11,
	 * C-13): refused with no erase or program, and again on a retry.
	 */
	{
		static const struct { uint8_t sel; unsigned s0, s1; } bad[] = {
			{0x00, 1, 0}, {0x01, 0, 1}, {0x00, 1, 1},
		};

		for (i = 0; i < N(bad); i++) {
			ff_init(&g_f);
			set_state(0, bad[i].s0, 1u);
			set_state(1, bad[i].s1, 2u);
			ff_set_selector(&g_f, bad[i].sel, 0x01);
			memcpy(&g_start.mem, g_f.mem, sizeof g_f.mem);
			ff_boot_image(g_image, 0x800u, 5u);
			ff_reset(&g_f);
			CHECK_EQ(s02_slot_upgrade(&env, true, g_image, 0x800u, &t), S02_ERR_START);
			CHECK_EQ(g_f.nmut, 0);
			CHECK(memcmp(g_f.mem, g_start.mem, sizeof g_f.mem) == 0);
			ff_reset(&g_f);
			CHECK_EQ(s02_slot_upgrade(&env, true, g_image, 0x800u, &t), S02_ERR_START);
			CHECK_EQ(g_f.nmut, 0);
		}
	}

	/* S-02.050: a selector write towards a bad slot is refused. */
	ff_init(&g_f);
	set_state(0, 1, 1u);	/* badcrc */
	set_state(1, 0, 2u);
	ff_set_selector(&g_f, 0x01, 0x01);
	ff_reset(&g_f);
	CHECK_EQ(s02_write_selector(&env, 0, 0x01), S02_ERR_NOT_GOOD);
	CHECK_EQ(g_f.nmut, 0);
	CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], 0x01u);
	CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], 0x01u);
	set_state(0, 0, 1u);
	ff_reset(&g_f);
	CHECK_EQ(s02_write_selector(&env, 0, 0x01), S02_OK);
	CHECK_EQ(g_f.nmut, 2);
	CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], 0x00u);
	CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], 0x01u);
	CHECK_EQ(s02_write_selector(&env, 2, 0x01), S02_ERR_NOT_GOOD);
}

/* --- SC-UPG-CUT, SC-UPG-NEXT-BOOT (S-02.042, S-02.051, .052, .094) --- */

static const struct v_pcseq *seq_by_id(const char *id)
{
	unsigned i;

	for (i = 0; i < N(v_pc_seqs); i++)
		if (strcmp(v_pc_seqs[i].id, id) == 0)
			return &v_pc_seqs[i];
	return NULL;
}

static void setup_seq(const struct v_pcseq *s)
{
	ff_init(&g_f);
	set_state(0, s->s0, 21u);
	set_state(1, s->s1, 22u);
	ff_set_selector(&g_f, s->sel[0], s->sel[1]);
	ff_boot_image(g_image, V_PC_IMAGE_LEN, 23u);
	ff_reset(&g_f);
}

/* The expected sequences: start rule, op order and extent (S-02.045, .049, .090). */
static void test_powercut_sequences(void)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned i, t;

	for (i = 0; i < N(v_pc_seqs); i++) {
		const struct v_pcseq *s = &v_pc_seqs[i];
		int rc;

		setup_seq(s);
		rc = s02_slot_upgrade(&env, true, g_image, V_PC_IMAGE_LEN, &t);
		if (!s->start_allowed) {
			CHECK_EQ(rc, S02_ERR_START);
			CHECK_EQ(g_f.nmut, 0);
			continue;
		}
		CHECK_EQ(rc, S02_OK);
		CHECK_EQ(t, s->target);
		CHECK_EQ(s02_selected_slot(s->sel[0]), s->selected);
		CHECK(mut_ops_equal(s->ops, s->nops));
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], s->written[0]);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], s->written[1]);
		check_upgrade_log(t, V_PC_IMAGE_LEN);
	}
}

/*
 * Run `resets` resets under a profile from the current flash; boots[] gets
 * the slots booted, and *all_good whether each booted slot was good.
 */
static void run_resets(int conforming, unsigned resets, unsigned *boots, bool *all_good,
		       unsigned tear_first, uint32_t seed)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned r;

	*all_good = true;
	for (r = 0; r < resets; r++) {
		unsigned b;

		ff_reset(&g_f);
		if (r == 0u && tear_first != 0u) {
			g_f.tear = tear_first;
			g_f.tear_kind = FF_TEAR_SEED;
			g_f.tear_a = seed;
		}
		if (conforming) {
			struct s02_fs_outcome o;

			s02_first_stage(&env, false, &o);
			b = o.boot_slot;
		} else {
			b = ff_factory_first_stage(&env);
		}
		boots[r] = b;
		if (!ff_slot_good(&g_f, b))
			*all_good = false;
	}
}

static void test_powercut_cuts(void)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	static struct fake_flash cut;
	unsigned i, t;

	for (i = 0; i < N(v_pc_cuts); i++) {
		const struct v_pccut *c = &v_pc_cuts[i];
		const struct v_pcseq *s = seq_by_id(c->id);
		unsigned seeds = c->torn ? 3u : 1u, k;

		if (s == NULL || !s->start_allowed)
			continue;
		for (k = 0; k < seeds; k++) {
			unsigned prof;

			setup_seq(s);
			if (c->torn) {
				g_f.tear = c->mut;
				g_f.tear_kind = FF_TEAR_SEED;
				g_f.tear_a = 100u + k;
			} else if (c->mut == 0u) {
				continue;	/* nothing made: the "none" row */
			} else {
				g_f.cut_after = c->mut;
			}
			(void)s02_slot_upgrade(&env, true, g_image, V_PC_IMAGE_LEN, &t);
			CHECK(g_f.dead || c->mut >= s->nops - 2u);
			/* The state the cut leaves. */
			if (c->boots_f[0] == 9u) {
				/* A torn selector: at least the 1 bits of the value given. */
				CHECK_EQ(g_f.mem[S02_SELECTOR_OFF] & c->sel_after[0], c->sel_after[0]);
				CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u] & c->sel_after[1], c->sel_after[1]);
			} else {
				CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], c->sel_after[0]);
				CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], c->sel_after[1]);
			}
			CHECK_EQ(ff_slot_good(&g_f, 0), c->s0_good);
			CHECK_EQ(ff_slot_good(&g_f, 1), c->s1_good);
			if (c->target_good >= 0)
				CHECK_EQ(ff_slot_good(&g_f, s->target), (unsigned)c->target_good);
			memcpy(cut.mem, g_f.mem, sizeof cut.mem);

			for (prof = 0; prof < 2u; prof++) {
				const uint8_t *want = prof ? c->boots_c : c->boots_f;
				int safe = prof ? c->safe_c : c->safe_f;
				int multi = prof ? c->multi_c : c->multi_f;
				unsigned boots[3], m;
				bool good;

				memcpy(g_f.mem, cut.mem, sizeof g_f.mem);
				run_resets((int)prof, 3u, boots, &good, 0u, 0u);
				if (want[0] != 9u) {
					CHECK_EQ(boots[0], want[0]);
					CHECK_EQ(boots[1], want[1]);
					CHECK_EQ(boots[2], want[2]);
				}
				/* With the precondition (S-02.052), safe means every reset boots a good slot. */
				if (safe && s->precondition)
					CHECK(good);
				/* S-02.094: no reset boots a partial target. */
				if (c->target_good == 0)
					CHECK(boots[0] != s->target && boots[1] != s->target &&
					      boots[2] != s->target);
				if (!multi)
					continue;
				/* Further cuts: tear the first reset's own selector erase or program. */
				for (m = 1; m <= 2u; m++) {
					memcpy(g_f.mem, cut.mem, sizeof g_f.mem);
					run_resets((int)prof, 1u, boots, &good, m, 200u + k * 2u + m);
					run_resets((int)prof, 3u, boots, &good, 0u, 0u);
					CHECK(good);
				}
			}
		}
	}
}

/* --- SC-FS-UPG, SC-FS-FAULT (S-02.070, S-02.071, S-02.072) --- */

struct ann {
	unsigned on_at, off_at;	/* op count when the announcement went on and off */
	int ons, offs;
};

static void announce(void *ctx, bool on)
{
	struct ann *a = ctx;

	if (on) {
		a->on_at = g_f.nall;
		a->ons++;
	} else {
		a->off_at = g_f.nall;
		a->offs++;
	}
}

static void fs_start(void)
{
	uint8_t rec[S02_META_SIZE], mirror[S02_MIRROR_SIZE];
	unsigned i;

	ff_init(&g_f);
	set_state(0, 0, 1u);
	set_state(1, 0, 2u);
	ff_set_selector(&g_f, 0x00, 0x00);
	for (i = 0; i < S02_META_SIZE; i++)
		rec[i] = (uint8_t)(i * 3u + 1u);
	ff_put32(rec, S02_META_MAGIC);
	s02_mirror_build(rec, mirror);
	memcpy(g_f.mem + S02_MIRROR_OFF, mirror, S02_MIRROR_SIZE);
	memcpy(g_f.mem + S02_META_OFF, rec, S02_META_SIZE);
	ff_fill_image(g_f.mem, 0x86C0u, 77u);
	memcpy(&g_start.mem, g_f.mem, sizeof g_f.mem);
	ff_reset(&g_f);
}

static void test_first_stage_upgrade(void)
{
	static const uint32_t sizes[] = {0u, 0x86C0u, 0xB000u, 0xB001u, 0xC000u};
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned i, j;

	CHECK_EQ(V_FS_LIMIT, S02_FIRST_STAGE_MAX);
	for (i = 0; i < N(v_fs_refused); i++) {
		fs_start();
		CHECK_EQ(s02_first_stage_upgrade(&env, true, g_image, v_fs_refused[i], NULL, NULL),
			 S02_ERR_SIZE);
		CHECK_EQ(g_f.nmut, 0);
	}
	for (i = 0; i < N(sizes); i++) {
		struct ann a = {0, 0, 0, 0};
		uint32_t n = sizes[i];
		unsigned last_prog = 0, first_erase = ~0u;
		int rc;

		fs_start();
		ff_boot_image(g_image, n, 31u + i);
		rc = s02_first_stage_upgrade(&env, true, g_image, n, announce, &a);
		if (n == 0u || n > S02_FIRST_STAGE_MAX) {
			CHECK_EQ(rc, S02_ERR_SIZE);
			CHECK_EQ(g_f.nmut, 0);
			CHECK_EQ(a.ons, 0);
			continue;
		}
		CHECK_EQ(rc, S02_OK);
		CHECK(memcmp(g_f.mem, g_image, n) == 0);
		/* Nothing at or above 0xB000 changes, the mirror included. */
		CHECK(memcmp(g_f.mem + S02_MIRROR_OFF, g_start.mem + S02_MIRROR_OFF,
			     S02_FLASH_SIZE - S02_MIRROR_OFF) == 0);
		for (j = 0; j < g_f.nlog; j++) {
			const struct ff_op *o = &g_f.log[j];

			if (o->kind == FF_READ)
				continue;
			CHECK(o->off + o->len <= n + S02_SECTOR_SIZE && o->off < S02_MIRROR_OFF);
			if (o->kind == FF_ERASE && first_erase == ~0u)
				first_erase = j;
			if (o->kind == FF_PROGRAM) {
				CHECK(find_op(FF_ERASE, o->off - (o->off % S02_SECTOR_SIZE), 0) < j);
				last_prog = j;
			}
		}
		CHECK_EQ(count_kind(FF_ERASE), (n + S02_SECTOR_SIZE - 1u) / S02_SECTOR_SIZE);
		CHECK(reads_cover(0, n, last_prog, g_f.nlog));
		/* S-02.072: announced before the first erase, kept to the read-back. */
		CHECK_EQ(a.ons, 1);
		CHECK_EQ(a.offs, 1);
		CHECK(a.on_at <= first_erase);
		CHECK(a.off_at >= g_f.nall);
		CHECK(!g_f.bad_call);
	}
	/* package_steps.json first_stage_refused_streams: a 1-byte first stage
	 * is Bad file (S-02.091) before any erase or program. */
	fs_start();
	padded(g_image, v_boot_fs_one_byte, sizeof v_boot_fs_one_byte, 1u);
	CHECK_EQ(s02_first_stage_upgrade(&env, true, g_image, 1u, NULL, NULL),
		 S02_ERR_STREAM);
	CHECK_EQ(g_f.nmut, 0);
	/* package_steps.json first_stage_ops: the erases and programs, exactly. */
	for (i = 0; i < N(v_fs_upgrades); i++) {
		const struct v_fsops *v = &v_fs_upgrades[i];
		unsigned k = 0;

		fs_start();
		/* package_steps.json: fs_small padded to the case's length. */
		padded(g_image, v_boot_fs_small, sizeof v_boot_fs_small, v->length);
		CHECK_EQ(s02_first_stage_upgrade(&env, true, g_image, v->length, NULL, NULL), S02_OK);
		for (j = 0; j < v->nops; j++) {
			if (v->ops[j].kind == 'r')
				continue;
			while (k < g_f.nlog && g_f.log[k].kind == FF_READ)
				k++;
			CHECK(k < g_f.nlog && g_f.log[k].kind == v->ops[j].kind &&
			      g_f.log[k].off == v->ops[j].off && g_f.log[k].len == v->ops[j].len);
			k++;
		}
	}
	/*
	 * SC-FS-FAULT: a corrupted program or a failed erase is made again from
	 * the first erase, inside one announcement; a fault on every attempt
	 * fails the step after a bounded number, and a later call works.
	 */
	for (i = 0; i < 3u; i++) {
		struct ann a = {0, 0, 0, 0};
		int rc;

		fs_start();
		ff_boot_image(g_image, 0x86C0u, 5u);
		if (i == 1u) {
			g_f.fail = 18;	/* the second sector's erase */
		} else {
			g_f.corrupt = 5;
			g_f.corrupt_mask = 0x7F;
			g_f.corrupt_on = i == 2u;
		}
		rc = s02_first_stage_upgrade(&env, true, g_image, 0x86C0u, announce, &a);
		CHECK_EQ(a.ons, 1);
		CHECK_EQ(a.offs, 1);
		CHECK(erases_at(0) >= 2u);	/* sector 0 erased again */
		if (i < 2u) {
			CHECK_EQ(rc, S02_OK);
			CHECK(memcmp(g_f.mem, g_image, 0x86C0u) == 0);
			continue;
		}
		CHECK(rc != S02_OK);
		CHECK_EQ(erases_at(0), 3u);	/* a bounded number of attempts */
		ff_reset(&g_f);
		CHECK_EQ(s02_first_stage_upgrade(&env, true, g_image, 0x86C0u, NULL, NULL), S02_OK);
		CHECK(memcmp(g_f.mem, g_image, 0x86C0u) == 0);
	}
	/* The flash holds the image already: success, no erase, no announcement. */
	{
		struct ann a = {0, 0, 0, 0};

		fs_start();
		ff_boot_image(g_image, 0x86C0u, 6u);
		memcpy(g_f.mem, g_image, 0x86C0u);
		CHECK_EQ(s02_first_stage_upgrade(&env, true, g_image, 0x86C0u, announce, &a), S02_OK);
		CHECK_EQ(g_f.nmut, 0);
		CHECK_EQ(a.ons, 0);
	}
}

/*
 * S-02.091, SC-UPG-STREAM: every case of bootable_images.json, by the S-01
 * rules whole and fed in pieces, and through the writers from selector
 * 01 01 with both slots good (slot cases) or the SC-FS-UPG start.
 */
static void test_bootable_images(void)
{
	static const struct s01_range factory = {V_FACTORY_FS_STACK_START, V_FACTORY_FS_STACK_END};
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned i, t;

	CHECK_EQ(V_FACTORY_FS_STACK_START, S01_FACTORY_FS_STACK_START);
	CHECK_EQ(V_FACTORY_FS_STACK_END, S01_FACTORY_FS_STACK_END);
	for (i = 0; i < N(v_bootables); i++) {
		const struct v_bootable *v = &v_bootables[i];
		enum s01_profile p = v->slot ? S01_BOOTLOADER : S01_FIRST_STAGE;
		uint32_t fail = s01_stream_check(p, v->image, v->length, v->slot ? &factory : NULL);
		struct s01_stream s;
		uint32_t k, step;
		int rc;

		if (v->accepted)
			CHECK_EQ(fail, 0u);
		else
			CHECK((fail & v->refused) != 0u);
		CHECK_EQ(s01_bootable(p, v->image, v->length), v->accepted != 0);
		for (step = 1; step <= 23u; step += 11u) {
			s01_bootable_begin(&s, p);
			for (k = 0; k < v->length; k += step)
				s01_stream_feed(&s, v->image + k,
						v->length - k < step ? v->length - k : step);
			CHECK_EQ(s01_stream_end(&s), fail);
		}
		if (v->slot) {
			ff_init(&g_f);
			set_state(0, 0, 1u);
			set_state(1, 0, 2u);
			ff_set_selector(&g_f, 0x01, 0x01);
		} else {
			fs_start();
		}
		memcpy(&g_start.mem, g_f.mem, sizeof g_f.mem);
		memcpy(g_image, v->image, v->length);
		ff_reset(&g_f);
		rc = v->slot ? s02_slot_upgrade(&env, true, g_image, v->length, &t)
			     : s02_first_stage_upgrade(&env, true, g_image, v->length, NULL, NULL);
		if (v->accepted) {
			CHECK_EQ(rc, S02_OK);
			if (v->slot)
				check_upgrade_log(0, v->length);
		} else {
			CHECK_EQ(rc, S02_ERR_STREAM);
			CHECK_EQ(g_f.nmut, 0);
			CHECK(memcmp(g_f.mem, g_start.mem, sizeof g_f.mem) == 0);
		}
	}
}

/* An image equal to the selected slot's installs as any other, and leaves
 * a unit that starts from either slot and takes a further install. */
static void test_upgrade_same_image(void)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	struct s02_fs_outcome out;
	unsigned t, t2;
	bool good;

	ff_init(&g_f);
	set_state(0, 0, 1u);
	set_state(1, 0, 2u);
	ff_set_selector(&g_f, 0x00, 0x01);
	ff_boot_image(g_image, 0x800u, 5u);
	CHECK_EQ(s02_slot_upgrade(&env, true, g_image, 0x800u, &t), S02_OK);
	CHECK_EQ(t, 1u);
	s02_first_stage(&env, false, &out);	/* the next start checks it */
	CHECK_EQ(out.boot_slot, 1u);
	ff_reset(&g_f);
	CHECK_EQ(s02_slot_upgrade(&env, true, g_image, 0x800u, &t2), S02_OK);
	CHECK_EQ(t2, 0u);
	check_upgrade_log(0, 0x800u);
	CHECK(memcmp(g_f.mem + s02_slot_base(0), g_f.mem + s02_slot_base(1), 0x800u) == 0);
	for (unsigned k = 0; k < 2u; k++) {
		CHECK_EQ(s02_check_slot(&env, k, &good), S02_OK);
		CHECK(good);
	}
	/* every later start boots a good slot, and another install goes */
	s02_first_stage(&env, false, &out);
	CHECK_EQ(out.boot_slot, 0u);
	CHECK(out.selected_good);
	ff_boot_image(g_image, 0x800u, 6u);
	CHECK_EQ(s02_slot_upgrade(&env, true, g_image, 0x800u, &t2), S02_OK);
	CHECK_EQ(t2, 1u);
}

int main(void)
{
	test_upgrade_ok();
	test_upgrade_same_image();
	test_upgrade_size_and_option();
	test_upgrade_fault();
	test_selector_read_back();
	test_start_and_selector_rule();
	test_powercut_sequences();
	test_powercut_cuts();
	test_first_stage_upgrade();
	test_bootable_images();
	return finish("test_s02_write");
}
