/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-02 formats, the slot check and the first-stage table.
 * Requirements: S-02.001-.003, S-02.010-.016, S-02.020-.026 (the factory
 * model the other tests rely on), S-02.030-.036, S-02.054 (the running slot
 * after a read fault), S-02.082 (a first stage that reads a chord).
 * Vectors: tests/oracle/S-02-spi-flash/vectors records.json, first_stage_table.csv,
 * first_stage_read_faults.csv (through vectors/s02_vectors.h).
 */
#include <string.h>

#include "s02_check.h"
#include "s02_fake_flash.h"
#include "vectors/s02_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static struct fake_flash g_f;
static struct s02_flash_ops g_ops;

/* S-02.001-.003: the area map covers the flash with no gap or overlap. */
static void test_layout(void)
{
	static const uint32_t starts[] = {S02_FIRST_STAGE_OFF, S02_META_OFF,
		S02_SELECTOR_OFF, S02_RECORD0_OFF, S02_RECORD1_OFF, S02_SLOT0_OFF,
		S02_SLOT1_OFF, S02_FLASH_SIZE};
	unsigned i;

	for (i = 0; i + 1u < N(starts); i++) {
		CHECK(starts[i] < starts[i + 1u]);
		CHECK_EQ(starts[i] % S02_SECTOR_SIZE, 0);
	}
	CHECK_EQ(S02_SLOT1_OFF - S02_SLOT0_OFF, S02_SLOT_CAPACITY);
	CHECK_EQ(S02_FLASH_SIZE - S02_SLOT1_OFF, S02_SLOT_CAPACITY);
	CHECK_EQ(S02_MIRROR_OFF, S02_FIRST_STAGE_MAX);
	CHECK_EQ(S02_MIRROR_OFF + S02_SECTOR_SIZE, S02_META_OFF);
	CHECK_EQ(S02_FLASH_SIZE / S02_SECTOR_SIZE, 256);
}

/* S-02.010-.012, S-02.043 */
static void test_selector(void)
{
	unsigned i, v;

	for (i = 0; i < N(v_selectors); i++) {
		const struct v_selector *s = &v_selectors[i];

		CHECK_EQ(s02_selected_slot(s->index), s->slot);
		CHECK_EQ(s02_other_slot(s->index), 1u - s->slot);
		CHECK_EQ(!s02_verified(s->verified), (unsigned)s->check_next);
	}
	for (v = 0; v < 256u; v++) {	/* every value is valid input */
		CHECK_EQ(s02_selected_slot((uint8_t)v), v == 0u ? 0u : 1u);
		CHECK_EQ(s02_verified((uint8_t)v), v == 1u);
	}
}

/* S-02.013, S-02.014, S-02.016 items 1-2 */
static void test_records(void)
{
	unsigned i;

	for (i = 0; i < N(v_records); i++) {
		const struct v_record *r = &v_records[i];
		struct s02_record rec;
		uint8_t raw[S02_RECORD_SIZE];

		s02_record_decode(r->bytes, &rec);
		CHECK_EQ(rec.crc32, r->crc32);
		CHECK_EQ(rec.length, r->length);
		CHECK_EQ(rec.tag, r->tag);
		CHECK_EQ(s02_tag_ok(rec.tag), (unsigned)r->tag_ok);
		s02_record_encode(&rec, raw);
		CHECK(memcmp(raw, r->bytes, S02_RECORD_SIZE) == 0);
		CHECK_EQ(s02_record_plausible(&rec), (unsigned)(r->tag_ok && r->len_ok));
	}
	CHECK(!s02_tag_ok(0x0001B004u));
	CHECK(!s02_tag_ok(0xFFFFFFFFu));
}

/* S-02.015: the reference CRC used by every test, and s02_flash_crc. */
static void test_crc(void)
{
	static uint8_t buf[S02_SLOT_CAPACITY];
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned i;
	uint32_t j, crc;

	for (i = 0; i < N(v_crcs); i++) {
		const struct v_crc *c = &v_crcs[i];

		for (j = 0; j < c->len; j++) {
			if (c->kind == 0)
				buf[j] = (uint8_t)c->data[j];
			else if (c->kind == 1)
				buf[j] = c->fill;
			else
				buf[j] = (uint8_t)(((j * 7u + c->seed) ^ (j >> 8)) & 0xFFu);
		}
		CHECK_EQ(ref_crc32(0, buf, c->len), c->crc32);
		/* Chained in two parts, as s02_flash_crc reads it. */
		CHECK_EQ(ref_crc32(ref_crc32(0, buf, c->len / 2u), buf + c->len / 2u,
				   c->len - c->len / 2u), c->crc32);
		ff_init(&g_f);
		memcpy(g_f.mem + S02_SLOT0_OFF, buf, c->len);
		CHECK_EQ(s02_flash_crc(&env, S02_SLOT0_OFF, c->len, &crc), S02_OK);
		CHECK_EQ(crc, c->crc32);
	}
}

static unsigned state_index(const char *s)
{
	unsigned i;

	for (i = 0; i < N(v_states); i++)
		if (strcmp(v_states[i], s) == 0)
			return i;
	return 0;
}

static void set_slots(unsigned s0, unsigned s1)
{
	ff_set_slot(&g_f, 0, v_states[s0], 0x1345u, 3u);
	ff_set_slot(&g_f, 1, v_states[s1], 0x2345u, 5u);
}

/* S-02.016, S-02.031: the record states, judged without reading a bad record's slot. */
static void test_check_slot(void)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned s;

	for (s = 0; s < N(v_states); s++) {
		bool good = true;
		unsigned i, slot_reads = 0;

		ff_init(&g_f);
		set_slots(s, s);
		CHECK_EQ(s02_check_slot(&env, 1, &good), S02_OK);
		CHECK_EQ(good, s == state_index("good"));
		CHECK_EQ(good, ff_slot_good(&g_f, 1));
		for (i = 0; i < g_f.nlog; i++)
			if (g_f.log[i].off >= S02_SLOT1_OFF)
				slot_reads++;
		/* tag or length bad: no read of the slot (S-02.031, S-16.126). */
		if (s != state_index("good") && s != state_index("badcrc"))
			CHECK_EQ(slot_reads, 0);
	}
	ff_init(&g_f);
	set_slots(0, 0);
	g_f.fail_read = 1;
	{
		bool good = true;

		CHECK_EQ(s02_check_slot(&env, 0, &good), S02_ERR_IO);
		CHECK(!good);
	}
}

/* Index of the first record read in the log, and whether the other was read. */
static void record_reads(int *first, int *second)
{
	unsigned i;

	*first = 2;
	*second = 2;
	for (i = 0; i < g_f.nlog; i++) {
		int r = g_f.log[i].off == S02_RECORD0_OFF ? 0 :
			g_f.log[i].off == S02_RECORD1_OFF ? 1 : -1;

		if (r < 0)
			continue;
		if (*first == 2)
			*first = r;
		else if (r != *first && *second == 2)
			*second = r;
	}
}

/* Mutating ops in the log, and that they are only the selector's. */
static unsigned selector_only_writes(void)
{
	unsigned i, n = 0;

	for (i = 0; i < g_f.nlog; i++) {
		if (g_f.log[i].kind == FF_READ)
			continue;
		n++;
		CHECK_EQ(g_f.log[i].off, S02_SELECTOR_OFF);
	}
	return n;
}

/* S-02.030-.035 (conforming) and S-02.020-.026 (factory model). */
static void test_first_stage_table(void)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned i;

	for (i = 0; i < N(v_first_stage); i++) {
		const struct v_fs *r = &v_first_stage[i];
		struct s02_fs_outcome o;
		int first, second;
		unsigned writes, k;

		ff_init(&g_f);
		set_slots(r->s0, r->s1);
		ff_set_selector(&g_f, r->sel[0], r->sel[1]);
		ff_reset(&g_f);
		s02_first_stage(&env, false, &o);
		CHECK_EQ(o.status, S02_OK);
		CHECK_EQ(o.boot_slot, r->c_boot);
		CHECK_EQ(o.selected_good, r->c_pass);
		record_reads(&first, &second);
		CHECK_EQ(first, r->c_first);
		CHECK_EQ(second, r->c_second);
		writes = selector_only_writes();
		CHECK_EQ(writes, r->c_writes ? 2u : 0u);	/* S-02.035: one erase, one program */
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], r->c_after[0]);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], r->c_after[1]);
		CHECK(!g_f.bad_call);
		/* The second reset. */
		ff_reset(&g_f);
		s02_first_stage(&env, false, &o);
		CHECK_EQ(o.boot_slot, r->c_boot2);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], r->c_after2[0]);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], r->c_after2[1]);
		/* S-02.090's premise: every completed reset leaves verified = 1. */
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], 0x01u);

		/* S-02.082: with a chord read, the same reset writes nothing. */
		ff_init(&g_f);
		set_slots(r->s0, r->s1);
		ff_set_selector(&g_f, r->sel[0], r->sel[1]);
		ff_reset(&g_f);
		s02_first_stage(&env, true, &o);
		CHECK_EQ(g_f.nmut, 0);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], r->sel[1]);

		/* A failed read in this reset: a slot is still booted, nothing is written. */
		for (k = 1; k <= 2u; k++) {
			ff_init(&g_f);
			set_slots(r->s0, r->s1);
			ff_set_selector(&g_f, r->sel[0], r->sel[1]);
			ff_reset(&g_f);
			g_f.fail_read = k;	/* the selector read, then the first record read */
			s02_first_stage(&env, false, &o);
			CHECK_EQ(o.status, S02_ERR_IO);
			CHECK_EQ(g_f.nmut, 0);
			CHECK(o.boot_slot <= 1u);
		}

		/* The factory model, where the table specifies it. */
		if (r->f_boot < 0)
			continue;
		ff_init(&g_f);
		set_slots(r->s0, r->s1);
		ff_set_selector(&g_f, r->sel[0], r->sel[1]);
		ff_reset(&g_f);
		CHECK_EQ(ff_factory_first_stage(&env), (unsigned)r->f_boot);
		CHECK_EQ(selector_only_writes(), r->f_writes ? 2u : 0u);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], r->f_after[0]);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], r->f_after[1]);
		if (r->f_boot2 >= 0) {
			ff_reset(&g_f);
			CHECK_EQ(ff_factory_first_stage(&env), (unsigned)r->f_boot2);
			CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], r->f_after2[0]);
			CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], r->f_after2[1]);
		}
	}
}

/*
 * S-02.036: reads that fail.  Each row runs twice: a faulty slot's record read
 * fails, then (the record read) only its contents read.
 */
static void test_first_stage_read_faults(void)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned i, pass;

	for (i = 0; i < N(v_fs_faults); i++) {
		const struct v_fsf *r = &v_fs_faults[i];

		for (pass = 0; pass < 2u; pass++) {
			struct s02_fs_outcome o;
			int first, second;
			unsigned regions = 0;

			if (r->faults & 1u)
				regions |= FF_R_SELECTOR;
			if (r->faults & 2u)
				regions |= FF_R_SLOT0 | (pass == 0u ? FF_R_RECORD0 : 0u);
			if (r->faults & 4u)
				regions |= FF_R_SLOT1 | (pass == 0u ? FF_R_RECORD1 : 0u);
			ff_init(&g_f);
			set_slots(r->s0, r->s1);
			ff_set_selector(&g_f, r->sel[0], r->sel[1]);
			ff_reset(&g_f);
			g_f.fail_regions = regions;
			s02_first_stage(&env, false, &o);
			CHECK_EQ(o.status != S02_OK, r->seen);
			CHECK_EQ(o.boot_slot, r->boot);
			record_reads(&first, &second);
			CHECK_EQ(first, r->c_first);
			CHECK_EQ(second, r->c_second);
			CHECK_EQ(selector_only_writes(), r->writes ? 2u : 0u);
			CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], r->after[0]);
			CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], r->after[1]);
			CHECK(!g_f.bad_call);
		}
	}
}

/*
 * S-02.036, S-02.043, S-02.054, S-02.090: the running slot (the one booted,
 * which the slave is streamed) differs from the slot the selector names only
 * after a read fault, and is then the other slot. With verified 0x01 an
 * upgrade started in that boot is allowed and targets the running slot.
 */
static void test_running_slot_after_read_fault(void)
{
	static uint8_t image[0x800];
	struct s02_env env = ff_env(&g_f, &g_ops);
	unsigned i, t, seen = 0;

	ff_boot_image(image, sizeof image, 7u);
	for (i = 0; i < N(v_fs_faults); i++) {
		const struct v_fsf *r = &v_fs_faults[i];
		unsigned named = s02_selected_slot(r->after[0]);
		unsigned regions = 0;
		struct s02_fs_outcome o;

		if (r->faults & 1u)
			regions |= FF_R_SELECTOR;
		if (r->faults & 2u)
			regions |= FF_R_SLOT0 | FF_R_RECORD0;
		if (r->faults & 4u)
			regions |= FF_R_SLOT1 | FF_R_RECORD1;
		ff_init(&g_f);
		set_slots(r->s0, r->s1);
		ff_set_selector(&g_f, r->sel[0], r->sel[1]);
		ff_reset(&g_f);
		g_f.fail_regions = regions;
		s02_first_stage(&env, false, &o);
		if (o.boot_slot == named)
			continue;
		CHECK(o.status != S02_OK);
		CHECK(!o.wrote);
		CHECK_EQ(o.boot_slot, 1u - named);
		if (!s02_verified(r->after[1]))
			continue;	/* S-02.090 refuses the start */
		ff_reset(&g_f);		/* the faults gone, the same boot */
		if ((named == 0u ? r->s0 : r->s1) != 0u) {
			/* S-02.090: the selected slot is bad */
			CHECK_EQ(s02_slot_upgrade(&env, true, image, sizeof image, &t), S02_ERR_START);
			CHECK_EQ(g_f.nmut, 0);
			continue;
		}
		CHECK_EQ(s02_slot_upgrade(&env, true, image, sizeof image, &t), S02_OK);
		CHECK_EQ(t, o.boot_slot);
		seen++;
	}
	CHECK(seen > 0u);

	/*
	 * SC-SLAVE-SRC's read-fault run: selector 00 01, both slots good. A
	 * failed selector read, or a failed read of slot 0's record or
	 * contents: slot 1 boots, no selector write, the selector still reads
	 * 00 01, and an upgrade in this boot targets slot 1.
	 */
	for (i = 0; i < 3u; i++) {
		static const unsigned faults[3] = {
			FF_R_SELECTOR, FF_R_RECORD0 | FF_R_SLOT0, FF_R_SLOT0,
		};
		struct s02_fs_outcome o;

		ff_init(&g_f);
		set_slots(0, 0);
		ff_set_selector(&g_f, 0x00, 0x01);
		ff_reset(&g_f);
		g_f.fail_regions = faults[i];
		s02_first_stage(&env, false, &o);
		CHECK(o.status != S02_OK);
		CHECK_EQ(o.boot_slot, 1u);
		CHECK(!o.wrote);
		CHECK_EQ(selector_only_writes(), 0u);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF], 0x00u);
		CHECK_EQ(g_f.mem[S02_SELECTOR_OFF + 1u], 0x01u);
		ff_reset(&g_f);
		CHECK_EQ(s02_slot_upgrade(&env, true, image, sizeof image, &t), S02_OK);
		CHECK_EQ(t, 1u);
	}
}

int main(void)
{
	test_layout();
	test_selector();
	test_records();
	test_crc();
	test_check_slot();
	test_first_stage_table();
	test_first_stage_read_faults();
	test_running_slot_after_read_fault();
	return finish("test_s02_format");
}
