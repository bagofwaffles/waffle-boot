/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The start log (src/front/slots/startlog.c) against S-02.095 and the
 * scenario SC-STARTLOG, over a RAM flash with an op log: each record that
 * gives no log, an empty log, each kind of odd log, a full one and a failed
 * read; the count of unfinished entries before the first free one; the one
 * 1-byte program of a mark and of a close, its byte and value, in each of a
 * byte's four positions and in both slots; programs that fail or do not
 * read back; power cuts at each program, followed by more starts; any
 * sector a torn erase can leave.
 */
#include <string.h>

#include "support/check.h"

#include "front/front.h"
#include "front/handoff.h"
#include "front/slots/slots.h"
#include "front/slots/startlog.h"

/* ---- the flash: NOR, erase to 0xFF, program clears bits, an op log ---- */

#define FLASH_SIZE 0x100000u
#define OPS_MAX    8192u

struct op {
	char kind;		/* 'r', 'e', 'p' */
	uint32_t off, len;
	uint8_t val;		/* a program's first byte */
};

static struct {
	uint8_t mem[FLASH_SIZE];
	struct op ops[OPS_MAX];
	unsigned n_ops, n_prog, n_erase;
	unsigned fail_prog;	/* this program (1-based) fails, nothing stored */
	unsigned lazy_prog;	/* this program reports success but: */
	unsigned lazy_kind;	/* 1 stores nothing, 2 clears one bit more */
	unsigned cut_prog;	/* the power is lost at this program */
	bool cut_applied;	/* after it stored its byte */
	uint32_t fail_lo, fail_hi;	/* a read starting in [lo, hi) fails */
	bool fail_after_prog;	/* every read after a program fails */
	bool dead;
} fk;

static int f_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (fk.dead || off > FLASH_SIZE || len > FLASH_SIZE - off)
		return -1;
	if (fk.n_ops < OPS_MAX)
		fk.ops[fk.n_ops++] = (struct op){ 'r', off, len, 0 };
	if (off >= fk.fail_lo && off < fk.fail_hi)
		return -1;
	if (fk.fail_after_prog && fk.n_prog != 0u)
		return -1;
	memcpy(buf, fk.mem + off, len);
	return 0;
}

static int f_erase(void *ctx, uint32_t off)
{
	(void)ctx;
	if (fk.dead || off % FR_SECTOR != 0u || off >= FLASH_SIZE)
		return -1;
	if (fk.n_ops < OPS_MAX)
		fk.ops[fk.n_ops++] = (struct op){ 'e', off, FR_SECTOR, 0 };
	fk.n_erase++;
	memset(fk.mem + off, 0xFF, FR_SECTOR);
	return 0;
}

static int f_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (fk.dead || len == 0u || off > FLASH_SIZE || len > FLASH_SIZE - off)
		return -1;
	fk.n_prog++;
	if (fk.n_ops < OPS_MAX)
		fk.ops[fk.n_ops++] = (struct op){ 'p', off, len, buf[0] };
	if (fk.n_prog == fk.fail_prog)
		return -1;
	if (fk.n_prog == fk.cut_prog) {
		if (fk.cut_applied)
			for (uint32_t i = 0; i < len; i++)
				fk.mem[off + i] &= buf[i];
		fk.dead = true;
		return -1;
	}
	for (uint32_t i = 0; i < len; i++) {
		uint8_t v = buf[i];

		if (fk.n_prog == fk.lazy_prog && fk.lazy_kind == 1u)
			v = 0xFFu;
		if (fk.n_prog == fk.lazy_prog && fk.lazy_kind == 2u) {
			/* the highest bit still set in both clears too */
			for (int b = 7; b >= 0; b--)
				if ((v >> b) & (fk.mem[off + i] >> b) & 1u) {
					v = (uint8_t)(v & ~(1u << b));
					break;
				}
		}
		fk.mem[off + i] &= v;
	}
	return 0;
}

static const struct fr_flash fl = { f_read, f_erase, f_program, NULL };

static void fk_clear(void)
{
	fk.n_ops = fk.n_prog = fk.n_erase = 0;
	fk.fail_prog = fk.lazy_prog = fk.lazy_kind = fk.cut_prog = 0;
	fk.cut_applied = false;
	fk.fail_lo = fk.fail_hi = 0;
	fk.fail_after_prog = false;
	fk.dead = false;
}

/* ---- the flash's contents ---- */

static void set_selector(uint8_t index, uint8_t verified)
{
	memset(fk.mem + FR_SELECTOR_OFF, 0xFF, FR_SECTOR);
	fk.mem[FR_SELECTOR_OFF] = index;
	fk.mem[FR_SELECTOR_OFF + 1u] = verified;
}

/* A record read with no CRC pass: the CRC is never checked here. */
static void set_record(unsigned k, uint32_t tag, uint32_t length)
{
	memset(fk.mem + FR_RECORD_OFF(k), 0xFF, FR_SECTOR);
	fr_put32(fk.mem + FR_RECORD_OFF(k), 0x12345678u);
	fr_put32(fk.mem + FR_RECORD_OFF(k) + 4u, length);
	fr_put32(fk.mem + FR_RECORD_OFF(k) + 8u, tag);
}

/* Entry e of slot k's log, its two bits (started bit low). */
static unsigned entry(unsigned k, uint32_t e)
{
	return (unsigned)(fk.mem[FR_LOG_OFF(k) + e / 4u] >> (2u * (e % 4u))) & 3u;
}

static void set_entry(unsigned k, uint32_t e, unsigned v)
{
	uint8_t *b = fk.mem + FR_LOG_OFF(k) + e / 4u;
	const unsigned sh = 2u * (e % 4u);

	*b = (uint8_t)((*b & ~(3u << sh)) | (v << sh));
}

#define F FR_LOG_E_FINISHED
#define U FR_LOG_E_UNFINISHED
#define I FR_LOG_E_INVALID
#define E FR_LOG_E_FREE

/* Slot k's sector: entries from `from`, the values in v, the rest free. */
static void set_log(unsigned k, uint32_t from, const unsigned *v, unsigned m)
{
	memset(fk.mem + FR_LOG_OFF(k), 0xFF, FR_SECTOR);
	for (uint32_t e = 0; e < from; e++)
		set_entry(k, e, F);
	for (unsigned i = 0; i < m; i++)
		set_entry(k, from + i, v[i]);
}

/* The selector names slot k (by index), both records give a log, every
 * slot erased and every log empty; the op log cleared. */
static void fresh(uint8_t index, uint8_t verified)
{
	memset(fk.mem, 0xFF, sizeof fk.mem);
	set_selector(index, verified);
	set_record(0, FR_TAG, 0x2345u);
	set_record(1, FR_TAG, 0x2345u);
	fk_clear();
}

/* ---- what the op log may hold ---- */

/* S-02.095: no erase; every program one byte in the selected slot's last
 * sector; every read the selector, the selected slot's record, or a part
 * of that sector (the record read with no CRC pass). */
static void check_ops(unsigned k)
{
	const uint32_t lo = FR_LOG_OFF(k), hi = FR_LOG_OFF(k) + FR_SECTOR;

	CHECK(fk.n_erase == 0u);
	for (unsigned i = 0; i < fk.n_ops; i++) {
		const struct op *o = &fk.ops[i];

		CHECK(o->kind != 'e');
		if (o->kind == 'p')
			CHECK(o->len == 1u && o->off >= lo && o->off < hi);
		if (o->kind == 'r')
			CHECK((o->off == FR_SELECTOR_OFF && o->len <= 2u) ||
			      (o->off == FR_RECORD_OFF(k) && o->len <= 12u) ||
			      (o->off >= lo && o->off + o->len <= hi));
	}
}

/* The one program of a step: at the byte of entry e, the value with one
 * bit clear, then a read of that byte after it. */
static void check_one_program(unsigned k, uint32_t e, uint8_t value)
{
	unsigned at = ~0u, progs = 0;

	for (unsigned i = 0; i < fk.n_ops; i++)
		if (fk.ops[i].kind == 'p') {
			progs++;
			at = i;
		}
	CHECK(progs == 1u);
	if (progs != 1u)
		return;
	CHECK(fk.ops[at].off == FR_LOG_OFF(k) + e / 4u);
	CHECK(fk.ops[at].len == 1u);
	CHECK(fk.ops[at].val == value);
	CHECK(at + 1u < fk.n_ops && fk.ops[at + 1u].kind == 'r' &&
	      fk.ops[at + 1u].off == fk.ops[at].off && fk.ops[at + 1u].len == 1u);
	check_ops(k);
}

static uint8_t started_value(uint32_t e)
{
	return (uint8_t)(0xFFu ^ (1u << (2u * (e % 4u))));
}

static uint8_t finished_value(uint32_t e)
{
	return (uint8_t)(0xFFu ^ (2u << (2u * (e % 4u))));
}

/* The main image once it is alive (S-02.095): the entry's finished bit. */
static void main_finishes(unsigned k, uint32_t e)
{
	const uint8_t v = finished_value(e);

	CHECK(f_program(NULL, FR_LOG_OFF(k) + e / 4u, &v, 1u) == 0);
}

/* ---- records that give no log ---- */

static void test_no_log(void)
{
	static const struct {
		uint32_t tag, length;
		bool log;
	} recs[] = {
		{ 0x0000B005u, 0x2345u, false },	/* the tag */
		{ 0xFFFFFFFFu, 0xFFFFFFFFu, false },	/* erased */
		{ FR_TAG, 0u, false },
		{ FR_TAG, 0x77001u, false },
		{ FR_TAG, 0x78000u, false },
		{ FR_TAG, 0x80000000u, false },
		{ FR_TAG, 0x77000u, true },
		{ FR_TAG, 1u, true },
	};
	static const uint8_t index[] = { 0x00, 0x01, 0x02, 0xFF };
	static const unsigned three[3] = { U, U, U };

	for (unsigned s = 0; s < ARRAY_LEN(index); s++)
		for (unsigned i = 0; i < ARRAY_LEN(recs); i++) {
			const unsigned k = index[s] == 0u ? 0u : 1u;	/* S-02.011 */
			struct fr_log l;

			/* verified plays no part: 00 here */
			fresh(index[s], 0x00);
			set_record(k, recs[i].tag, recs[i].length);
			/* the other slot's record gives a log: not the one read */
			set_record(1u - k, FR_TAG, 0x100u);
			fr_log_scan(&fl, &l);
			CHECK(l.slot == k);
			CHECK(l.state == (recs[i].log ? FR_LOG_OK : FR_LOG_NONE));
			check_ops(k);
			/* with a log that would open recovery: nothing written */
			set_log(k, 0, three, 3);
			fk_clear();
			if (recs[i].log) {
				CHECK(fr_log_start(&fl, &l) == FR_LOG_RECOVERY);
				check_one_program(k, 2u, finished_value(2u));
			} else {
				CHECK(fr_log_start(&fl, &l) == FR_LOG_GO);
				CHECK(fk.n_prog == 0u && fk.n_erase == 0u);
				CHECK(entry(k, 2u) == U && entry(k, 3u) == E);
			}
		}
}

/* ---- the log's states ---- */

static void expect_state(unsigned k, uint8_t state, uint16_t next, uint16_t unfinished)
{
	struct fr_log l;
	static uint8_t before[FR_SECTOR];

	memcpy(before, fk.mem + FR_LOG_OFF(k), FR_SECTOR);
	fk_clear();
	fr_log_scan(&fl, &l);
	CHECK(l.state == state);
	CHECK(l.slot == k);
	if (state == FR_LOG_OK) {
		CHECK(l.next == next);
		CHECK(l.unfinished == unfinished);
	}
	check_ops(k);
	CHECK(fk.n_prog == 0u);
	/* the step: anything not a valid log writes nothing and goes on */
	if (state != FR_LOG_OK) {
		fk_clear();
		CHECK(fr_log_start(&fl, &l) == FR_LOG_GO);
		CHECK(fk.n_prog == 0u && fk.n_erase == 0u);
		CHECK(memcmp(before, fk.mem + FR_LOG_OFF(k), FR_SECTOR) == 0);
		CHECK(!fr_log_mark(&fl, &l) && !fr_log_close(&fl, &l));
		CHECK(fk.n_prog == 0u);
	}
}

static void test_states(void)
{
	static const unsigned used[] = { F, U, F, F, U, U };
	static const unsigned bad_first[] = { I };
	static const unsigned bad_later[] = { F, U, F, I, U };
	static const unsigned gap[] = { F, E, U };		/* in one byte */
	static const unsigned gap4[] = { F, U, F, E, F };	/* the next byte */

	for (unsigned k = 0; k < 2u; k++) {
		fresh(k == 0u ? 0x00 : 0x01, 0x01);
		/* empty */
		expect_state(k, FR_LOG_OK, 0, 0);
		set_log(k, 0, used, ARRAY_LEN(used));
		expect_state(k, FR_LOG_OK, 6, 2);
		/* odd: an entry not valid, first or after used ones */
		set_log(k, 0, bad_first, 1);
		CHECK(fk.mem[FR_LOG_OFF(k)] == 0xFDu);
		expect_state(k, FR_LOG_ODD, 0, 0);
		set_log(k, 0, bad_later, ARRAY_LEN(bad_later));
		expect_state(k, FR_LOG_ODD, 0, 0);
		/* a not-valid entry after the first free one */
		set_log(k, 0, used, 2);
		set_entry(k, 3, I);
		expect_state(k, FR_LOG_ODD, 0, 0);
		/* odd: a used entry after a free one, in its byte and later */
		set_log(k, 0, gap, ARRAY_LEN(gap));
		expect_state(k, FR_LOG_ODD, 0, 0);
		set_log(k, 0, gap4, ARRAY_LEN(gap4));
		expect_state(k, FR_LOG_ODD, 0, 0);
		set_log(k, 0, used, 2);
		set_entry(k, 4u * 300u + 1u, U);		/* the next page */
		expect_state(k, FR_LOG_ODD, 0, 0);
		/* odd: a byte not FF far into the sector, also its last */
		static const uint32_t far[] = { 0x100u, 0x800u, 0xFFFu };
		static const uint8_t vals[] = { 0x7Fu, 0x00u, 0xFEu, 0xBFu };

		for (unsigned i = 0; i < ARRAY_LEN(far); i++)
			for (unsigned j = 0; j < ARRAY_LEN(vals); j++) {
				set_log(k, 0, used, ARRAY_LEN(used));
				fk.mem[FR_LOG_OFF(k) + far[i]] = vals[j];
				expect_state(k, FR_LOG_ODD, 0, 0);
			}
		/* full: no free entry, finished, unfinished or both */
		memset(fk.mem + FR_LOG_OFF(k), 0x00, FR_SECTOR);
		expect_state(k, FR_LOG_FULL, 0, 0);
		memset(fk.mem + FR_LOG_OFF(k), 0xAA, FR_SECTOR);
		expect_state(k, FR_LOG_FULL, 0, 0);
		memset(fk.mem + FR_LOG_OFF(k), 0x00, FR_SECTOR);
		fk.mem[FR_LOG_OFF(k) + FR_SECTOR - 1u] = 0xAAu;	/* four unfinished */
		expect_state(k, FR_LOG_FULL, 0, 0);
		/* one free entry left, the last: valid */
		memset(fk.mem + FR_LOG_OFF(k), 0x00, FR_SECTOR);
		fk.mem[FR_LOG_OFF(k) + FR_SECTOR - 1u] = 0xC0u;
		expect_state(k, FR_LOG_OK, FR_LOG_ENTRIES - 1u, 0);
		/* a full page, then free: the next entry starts page 1 */
		memset(fk.mem + FR_LOG_OFF(k), 0xFF, FR_SECTOR);
		memset(fk.mem + FR_LOG_OFF(k), 0x00, FR_PAGE);
		expect_state(k, FR_LOG_OK, 4u * FR_PAGE, 0);
	}
}

/* ---- reads that fail ---- */

static void test_read_failures(void)
{
	for (unsigned k = 0; k < 2u; k++) {
		struct fr_log l;
		static const unsigned two[2] = { U, U };
		const uint32_t at[] = { FR_SELECTOR_OFF, FR_RECORD_OFF(k) };

		for (unsigned i = 0; i < ARRAY_LEN(at) + FR_SECTOR / FR_PAGE; i++) {
			const uint32_t lo = i < ARRAY_LEN(at) ? at[i] :
					   FR_LOG_OFF(k) + FR_PAGE * (uint32_t)(i - ARRAY_LEN(at));

			fresh(k == 0u ? 0x00 : 0xFF, 0x01);
			set_log(k, 0, two, 2);
			fk.fail_lo = lo;
			fk.fail_hi = lo + (i < ARRAY_LEN(at) ? 1u : FR_PAGE);
			fr_log_scan(&fl, &l);
			CHECK(l.state == FR_LOG_IO);
			CHECK(fk.n_prog == 0u);
			fk.n_ops = 0;
			CHECK(fr_log_start(&fl, &l) == FR_LOG_GO);
			CHECK(fk.n_prog == 0u && fk.n_erase == 0u);
		}
	}
}

/* ---- the count, and the step it decides ---- */

static void test_count(void)
{
	/* entries before the run: one finished at least; runs across a byte
	 * (from 2 and 3), a page (from 1022) and near the sector's end */
	static const uint32_t from[] = { 1, 2, 3, 4, 7, 1021, 1022, 1023, 1024,
					 FR_LOG_ENTRIES - 5u };
	static const unsigned four[4] = { U, U, U, U };

	for (unsigned k = 0; k < 2u; k++)
		for (unsigned i = 0; i < ARRAY_LEN(from); i++)
			for (unsigned n = 0; n <= 4u; n++) {
				struct fr_log l;
				const uint32_t next = from[i] + n;

				fresh(k == 0u ? 0x00 : 0x01, 0x01);
				set_log(k, from[i], four, n);
				CHECK(entry(k, from[i] - 1u) == F);
				expect_state(k, FR_LOG_OK, (uint16_t)next, (uint16_t)n);
				fk_clear();
				if (n < FR_LOG_LIMIT) {
					/* S-02.095 step 3: the first free entry's started bit */
					CHECK(fr_log_start(&fl, &l) == FR_LOG_GO_MARKED);
					CHECK(l.slot == k && l.next == next);
					check_one_program(k, next, started_value(next));
					CHECK(entry(k, next) == U);
				} else {
					/* step 4: the newest entry's finished bit */
					CHECK(fr_log_start(&fl, &l) == FR_LOG_RECOVERY);
					check_one_program(k, next - 1u, finished_value(next - 1u));
					CHECK(entry(k, next - 1u) == F);
					CHECK(entry(k, next) == E);
					/* the next start counts none */
					expect_state(k, FR_LOG_OK, (uint16_t)next, 0);
				}
			}

	/* the run counted is the one directly before the free entry */
	{
		static const unsigned mixed[][6] = {
			{ U, F, U, U, E, E },	/* 2 */
			{ U, U, U, F, E, E },	/* 0 */
			{ U, U, F, U, U, U },	/* 3 */
			{ U, U, U, E, E, E },	/* 3 from the sector's start */
		};
		static const uint16_t want_next[] = { 4, 4, 6, 3 };
		static const uint16_t want_n[] = { 2, 0, 3, 3 };

		for (unsigned i = 0; i < ARRAY_LEN(mixed); i++) {
			fresh(0x00, 0x01);
			set_log(0, 0, mixed[i], 6);
			expect_state(0, FR_LOG_OK, want_next[i], want_n[i]);
		}
	}
}

/* ---- the program of a mark and of a close, each position, each slot ---- */

static void test_positions(void)
{
	static const uint32_t bytes[] = { 0, 1, 63, 255, 256, 257, 2048, 4095 };
	static const uint8_t index[2] = { 0x00, 0x01 };

	for (unsigned k = 0; k < 2u; k++)
		for (unsigned b = 0; b < ARRAY_LEN(bytes); b++)
			for (unsigned q = 0; q < 4u; q++) {
				const uint32_t e = 4u * bytes[b] + q;
				struct fr_log l;
				uint8_t old;

				/* mark: entries before e finished, e free */
				fresh(index[k], 0x01);
				set_log(k, e, NULL, 0);
				fr_log_scan(&fl, &l);
				CHECK(l.state == FR_LOG_OK && l.next == e && l.slot == k);
				old = fk.mem[FR_LOG_OFF(k) + bytes[b]];
				fk_clear();
				CHECK(fr_log_mark(&fl, &l));
				check_one_program(k, e, started_value(e));
				CHECK(fk.mem[FR_LOG_OFF(k) + bytes[b]] ==
				      (uint8_t)(old & started_value(e)));
				CHECK(entry(k, e) == U);
				/* close: e unfinished and newest (the last entry marked
				 * leaves a full log, which nothing closes) */
				if (e == FR_LOG_ENTRIES - 1u) {
					fr_log_scan(&fl, &l);
					CHECK(l.state == FR_LOG_FULL);
					continue;
				}
				fr_log_scan(&fl, &l);
				CHECK(l.state == FR_LOG_OK && l.next == e + 1u && l.unfinished == 1u);
				old = fk.mem[FR_LOG_OFF(k) + bytes[b]];
				fk_clear();
				CHECK(fr_log_close(&fl, &l));
				check_one_program(k, e, finished_value(e));
				CHECK(fk.mem[FR_LOG_OFF(k) + bytes[b]] ==
				      (uint8_t)(old & finished_value(e)));
				CHECK(entry(k, e) == F);
				/* nothing else in the sector changed */
				for (uint32_t x = 0; x < FR_LOG_ENTRIES; x++)
					if (entry(k, x) != (x <= e ? F : E)) {
						CHECK(0);
						break;
					}
				/* the other slot's sector untouched */
				for (uint32_t x = 0; x < FR_SECTOR; x++)
					if (fk.mem[FR_LOG_OFF(1u - k) + x] != 0xFFu) {
						CHECK(0);
						break;
					}
			}
}

/* ---- programs that fail, or do not read back ---- */

static void test_bad_programs(void)
{
	static const unsigned two[2] = { U, U }, three[3] = { U, U, U };

	for (unsigned mode = 0; mode < 4u; mode++)
		for (unsigned n = 2; n <= 3u; n++) {
			struct fr_log l;

			fresh(0x00, 0x01);
			set_log(0, 5, n == 2u ? two : three, n);
			fk_clear();
			if (mode == 0u)
				fk.fail_prog = 1;
			else if (mode == 3u)
				fk.fail_after_prog = true;	/* the read-back fails */
			else {
				fk.lazy_prog = 1;
				fk.lazy_kind = mode;		/* unchanged, or wrong */
			}
			/* S-02.095 step 5: no entry named, the main image starts */
			CHECK(fr_log_start(&fl, &l) == FR_LOG_GO);
			CHECK(fk.n_prog == 1u && fk.n_erase == 0u);
			check_ops(0);
			/* the same through mark and close themselves */
			fresh(0x00, 0x01);
			set_log(0, 5, two, 2);
			fr_log_scan(&fl, &l);
			fk_clear();
			fk.fail_prog = mode == 0u ? 1u : 0u;
			fk.fail_after_prog = mode == 3u;
			fk.lazy_prog = mode == 1u || mode == 2u ? 1u : 0u;
			fk.lazy_kind = mode;
			CHECK(!fr_log_mark(&fl, &l));
			CHECK(fk.n_prog == 1u);
			fresh(0x00, 0x01);
			set_log(0, 5, two, 2);
			fr_log_scan(&fl, &l);
			fk_clear();
			fk.fail_prog = mode == 0u ? 1u : 0u;
			fk.fail_after_prog = mode == 3u;
			fk.lazy_prog = mode == 1u || mode == 2u ? 1u : 0u;
			fk.lazy_kind = mode;
			CHECK(!fr_log_close(&fl, &l));
			CHECK(fk.n_prog == 1u);
		}
}

/* ---- a close when the newest entry is not unfinished ---- */

static void test_close_refused(void)
{
	static const unsigned done[3] = { U, U, F };
	struct fr_log l;

	fresh(0x00, 0x01);
	set_log(0, 0, done, 3);
	fr_log_scan(&fl, &l);
	CHECK(l.state == FR_LOG_OK && l.next == 3u);
	fk_clear();
	CHECK(!fr_log_close(&fl, &l));
	CHECK(fk.n_prog == 0u);
	/* no entry at all */
	fresh(0x00, 0x01);
	fr_log_scan(&fl, &l);
	CHECK(l.state == FR_LOG_OK && l.next == 0u);
	fk_clear();
	CHECK(!fr_log_close(&fl, &l));
	CHECK(fk.n_prog == 0u);
	/* a mark whose entry no longer reads free */
	fresh(0x00, 0x01);
	fr_log_scan(&fl, &l);
	set_entry(0, 0, U);
	fk_clear();
	CHECK(!fr_log_mark(&fl, &l));
	CHECK(fk.n_prog == 0u);
}

/* ---- SC-STARTLOG runs 1, 2 and 4: the exact bytes ---- */

static void test_sc_startlog(void)
{
	static const uint8_t index[2] = { 0x00, 0x01 };

	for (unsigned k = 0; k < 2u; k++) {
		const uint32_t at = FR_LOG_OFF(k);
		struct fr_log l;

		CHECK(at == (k == 0u ? 0x87000u : 0xFF000u));
		/* run 1: one start, FE, the main image's FD, the byte FC */
		fresh(index[k], 0x01);
		CHECK(fr_log_start(&fl, &l) == FR_LOG_GO_MARKED);
		CHECK(l.slot == k && l.next == 0u);
		check_one_program(k, 0, 0xFEu);
		CHECK(fk.mem[at] == 0xFEu);
		main_finishes(k, l.next);
		CHECK(fk.ops[fk.n_ops - 1u].val == 0xFDu);
		CHECK(fk.mem[at] == 0xFCu);

		/* run 2: three starts that do not finish */
		fresh(index[k], 0x01);
		for (unsigned s = 0; s < 3u; s++) {
			fk_clear();
			CHECK(fr_log_start(&fl, &l) == FR_LOG_GO_MARKED);
			CHECK(l.next == s);
			check_one_program(k, s, started_value(s));
		}
		CHECK(fk.mem[at] == 0xEAu);
		fk_clear();
		CHECK(fr_log_start(&fl, &l) == FR_LOG_RECOVERY);
		check_one_program(k, 2, 0xDFu);
		CHECK(fk.mem[at] == 0xCAu);
		fk_clear();
		CHECK(fr_log_start(&fl, &l) == FR_LOG_GO_MARKED);
		check_one_program(k, 3, 0xBFu);
		CHECK(fk.mem[at] == 0x8Au);
		CHECK(l.next == 3u);
		/* the rest of the sector, and the rest of the flash, as before */
		for (uint32_t x = 1; x < FR_SECTOR; x++)
			if (fk.mem[at + x] != 0xFFu) {
				CHECK(0);
				break;
			}
		CHECK(fk.mem[FR_LOG_OFF(1u - k)] == 0xFFu);
	}
}

/* ---- power cuts at each program, then more starts ---- */

#define DEPTH 8u

static unsigned g_paths, g_recoveries;

/*
 * One start after another on slot 0's sector, each one of: its program cut
 * and not applied, cut and applied (the start ends there), or made; a start
 * that goes on marked is finished by the main image or not. Through every
 * path: the log stays valid, nothing is erased or written outside the
 * sector, and a start that enters recovery is never followed by another
 * that does, short of a cut at that start's own program.
 */
static void walk(unsigned depth, bool after_recovery, unsigned marked_in_row)
{
	static uint8_t saved[DEPTH + 1u][FR_SECTOR];

	if (depth == DEPTH) {
		g_paths++;
		return;
	}
	memcpy(saved[depth], fk.mem + FR_LOG_OFF(0), FR_SECTOR);
	for (unsigned c = 0; c < 4u; c++) {
		struct fr_log l;
		enum fr_log_step step;

		memcpy(fk.mem + FR_LOG_OFF(0), saved[depth], FR_SECTOR);
		fk_clear();
		if (c >= 2u) {
			fk.cut_prog = 1;
			fk.cut_applied = c == 3u;
		}
		step = fr_log_start(&fl, &l);
		check_ops(0);
		CHECK(fk.n_prog == 1u);
		fk.dead = false;
		fk_clear();
		{
			struct fr_log v;

			fr_log_scan(&fl, &v);
			CHECK(v.state == FR_LOG_OK);
			CHECK(v.unfinished <= FR_LOG_LIMIT);
		}
		if (c >= 2u) {
			/* the power went at the program: neither main nor recovery;
			 * the count of the starts in a row is the log's from here.
			 * A program that took counts a start that did not finish,
			 * as a power-off before the main image was alive does. */
			walk(depth + 1u, after_recovery && c == 2u, 0);
			continue;
		}
		if (step == FR_LOG_RECOVERY) {
			CHECK(!after_recovery);
			/* three unfinished starts came before, unless cuts shifted it */
			g_recoveries++;
			/* the power switch leaves recovery; nothing is written */
			if (c == 0u)
				walk(depth + 1u, true, 0);
			continue;
		}
		CHECK(step == FR_LOG_GO_MARKED);
		if (marked_in_row >= FR_LOG_LIMIT)
			CHECK(0);	/* a fourth unfinished start in a row went on */
		if (c == 0u) {
			walk(depth + 1u, false, marked_in_row + 1u);	/* never alive */
		} else {
			main_finishes(0, l.next);
			walk(depth + 1u, false, 0);
		}
	}
	memcpy(fk.mem + FR_LOG_OFF(0), saved[depth], FR_SECTOR);
}

static void test_cuts(void)
{
	g_paths = g_recoveries = 0;
	fresh(0x00, 0x01);
	walk(0, false, 0);
	CHECK(g_paths > 1000u);
	CHECK(g_recoveries > 0u);
}

/* ---- any sector a torn erase can leave ---- */

static uint32_t rng = 0x2545F491u;

static uint32_t next_rand(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

/* From the sector as found: the main image starts, or recovery runs once
 * and the next start goes on to the main image. */
static void starts_or_recovers_once(void)
{
	struct fr_log l;
	enum fr_log_step s;

	fk_clear();
	s = fr_log_start(&fl, &l);
	check_ops(0);
	CHECK(fk.n_prog <= 1u);
	if (s != FR_LOG_RECOVERY)
		return;
	fk_clear();
	s = fr_log_start(&fl, &l);
	CHECK(s == FR_LOG_GO || s == FR_LOG_GO_MARKED);
	check_ops(0);
}

static void test_any_sector(void)
{
	for (unsigned t = 0; t < 600u; t++) {
		fresh(0x00, 0x01);
		switch (t % 6u) {
		case 0:		/* random bytes */
			for (uint32_t x = 0; x < FR_SECTOR; x++)
				fk.mem[FR_LOG_OFF(0) + x] = (uint8_t)next_rand();
			break;
		case 1:		/* a used log, a torn erase's bits raised at random */
		case 2:
			for (uint32_t x = 0; x < FR_SECTOR; x++)
				fk.mem[FR_LOG_OFF(0) + x] = x < 40u ? (uint8_t)(next_rand() & 0xAAu) : 0xFFu;
			for (uint32_t x = 0; x < 48u; x++)
				fk.mem[FR_LOG_OFF(0) + x] |= (uint8_t)(next_rand() & next_rand());
			break;
		case 3:		/* a run of unfinished entries of any length */
			memset(fk.mem + FR_LOG_OFF(0), 0xFF, FR_SECTOR);
			for (uint32_t e = 0, n = next_rand() % 40u; e < n; e++)
				set_entry(0, e, (next_rand() & 7u) == 0u ? F : U);
			break;
		case 4:		/* a full log, partly raised */
			memset(fk.mem + FR_LOG_OFF(0), (next_rand() & 1u) ? 0xAA : 0x00, FR_SECTOR);
			for (uint32_t x = next_rand() % FR_SECTOR; x < FR_SECTOR; x++)
				fk.mem[FR_LOG_OFF(0) + x] = 0xFFu;
			break;
		default:	/* free entries only at random places */
			memset(fk.mem + FR_LOG_OFF(0), 0xAA, FR_SECTOR);
			for (unsigned i = 0; i < 20u; i++)
				fk.mem[FR_LOG_OFF(0) + next_rand() % FR_SECTOR] = 0xFFu;
			break;
		}
		starts_or_recovers_once();
	}
}

/* ---- S-02.095, S-02.093: a trial pending in the other slot, and its mark ---- */

/* The gate after the scan: (pending, slot); the reads it made, in reads. */
static bool gate(unsigned *slot, unsigned *reads)
{
	struct fr_log l;
	unsigned before, r = 0;
	bool yes;

	fr_log_scan(&fl, &l);
	before = fk.n_ops;
	yes = fr_log_trial_pending(&fl, &l, slot);
	for (unsigned i = before; i < fk.n_ops; i++) {
		CHECK(fk.ops[i].kind == 'r');
		r++;
	}
	*reads = r;
	return yes;
}

static void test_trial_gate(void)
{
	for (unsigned k = 0; k < 2u; k++) {
		const unsigned o = 1u - k;	/* the slot the selector does not name */
		unsigned slot = 9, reads = 0;

		/* pending: o's byte 0 FD, the rest of its sector FF */
		fresh(k == 0u ? 0x00 : 0x01, 0x01);
		fk.mem[FR_LOG_OFF(o)] = 0xFDu;
		CHECK(gate(&slot, &reads) && slot == o);
		CHECK(fk.n_prog == 0u && fk.n_erase == 0u);
		/* no trial: one 1-byte read of o's byte 0, nothing else */
		fresh(k == 0u ? 0x00 : 0x01, 0x01);
		CHECK(!gate(&slot, &reads) && reads == 1u);
		CHECK(fk.ops[fk.n_ops - 1u].off == FR_LOG_OFF(o) && fk.ops[fk.n_ops - 1u].len == 1u);
		for (unsigned v = 0; v < 256u; v++) {
			fresh(k == 0u ? 0x00 : 0x01, 0x01);
			fk.mem[FR_LOG_OFF(o)] = (uint8_t)v;
			CHECK(gate(&slot, &reads) == (v == 0xFDu));
			if (v != 0xFDu)
				CHECK(reads == 1u);
		}
		/* the selected slot's own byte 0 FD is no trial of the other */
		fresh(k == 0u ? 0x00 : 0x01, 0x01);
		fk.mem[FR_LOG_OFF(k)] = 0xFDu;
		CHECK(!gate(&slot, &reads));
		/* o's record: another tag, a length 0 or past 0x77000 */
		{
			static const uint32_t lens[] = { 0u, 0x77001u, 0x78000u, 0x80000000u };

			fresh(k == 0u ? 0x00 : 0x01, 0x01);
			fk.mem[FR_LOG_OFF(o)] = 0xFDu;
			set_record(o, 0x0000B005u, 0x2345u);
			CHECK(!gate(&slot, &reads));
			for (unsigned i = 0; i < 4u; i++) {
				set_record(o, FR_TAG, lens[i]);
				CHECK(!gate(&slot, &reads));
			}
			set_record(o, FR_TAG, 0x77000u);
			CHECK(gate(&slot, &reads) && slot == o);
			set_record(o, FR_TAG, 1u);
			CHECK(gate(&slot, &reads) && slot == o);
		}
		/* any other byte of the sector not FF: in the first page, the
		 * last byte, another page */
		{
			static const uint32_t at[] = { 1u, 0xFFu, 0x100u, FR_SECTOR - 1u };

			for (unsigned i = 0; i < 4u; i++) {
				fresh(k == 0u ? 0x00 : 0x01, 0x01);
				fk.mem[FR_LOG_OFF(o)] = 0xFDu;
				fk.mem[FR_LOG_OFF(o) + at[i]] = 0xFEu;
				CHECK(!gate(&slot, &reads));
			}
		}
		/* a read that fails: the selector, the byte, the record, a page */
		{
			const uint32_t lo[] = { FR_SELECTOR_OFF, FR_LOG_OFF(o), FR_RECORD_OFF(o),
						FR_LOG_OFF(o) + 0x200u };

			for (unsigned i = 0; i < 4u; i++) {
				fresh(k == 0u ? 0x00 : 0x01, 0x01);
				fk.mem[FR_LOG_OFF(o)] = 0xFDu;
				fk.fail_lo = lo[i];
				fk.fail_hi = lo[i] + 1u;
				CHECK(!gate(&slot, &reads));
			}
		}

		/* the mark: one program of byte 0 to FC, read back */
		fresh(k == 0u ? 0x00 : 0x01, 0x01);
		fk.mem[FR_LOG_OFF(o)] = 0xFDu;
		CHECK(fr_log_trial_mark(&fl, o));
		CHECK(fk.mem[FR_LOG_OFF(o)] == 0xFCu);
		check_one_program(o, 0, 0xFEu);
		/* not exactly pending (a spent F9 and another pending-coded ED
		 * included): nothing written */
		{
			static const uint8_t vals[] = { 0xFFu, 0xFEu, 0xFCu, 0xEDu, 0xF9u };

			for (unsigned i = 0; i < 5u; i++) {
				fresh(k == 0u ? 0x00 : 0x01, 0x01);
				fk.mem[FR_LOG_OFF(o)] = vals[i];
				CHECK(!fr_log_trial_mark(&fl, o));
				CHECK(fk.n_prog == 0u && fk.mem[FR_LOG_OFF(o)] == vals[i]);
			}
		}
		/* a program that fails, or does not read back */
		fresh(k == 0u ? 0x00 : 0x01, 0x01);
		fk.mem[FR_LOG_OFF(o)] = 0xFDu;
		fk.fail_prog = 1;
		CHECK(!fr_log_trial_mark(&fl, o));
		fresh(k == 0u ? 0x00 : 0x01, 0x01);
		fk.mem[FR_LOG_OFF(o)] = 0xFDu;
		fk.lazy_prog = 1;
		fk.lazy_kind = 1;
		CHECK(!fr_log_trial_mark(&fl, o));
		/* the spend: one program of byte 0 with bit 2 clear (FD to F9),
		 * read back; then the gate fails at its first 1-byte read, and
		 * entry 0 reads neither pending to it nor finished */
		fresh(k == 0u ? 0x00 : 0x01, 0x01);
		fk.mem[FR_LOG_OFF(o)] = 0xFDu;
		CHECK(fr_log_trial_spend(&fl, o));
		CHECK(fk.mem[FR_LOG_OFF(o)] == 0xF9u);
		check_one_program(o, 1, 0xFBu);
		CHECK(entry(o, 0) == I && entry(o, 1) == U);
		fk_clear();
		CHECK(!gate(&slot, &reads) && reads == 1u);
		/* nor is it ever marked */
		CHECK(!fr_log_trial_mark(&fl, o));
		CHECK(fk.mem[FR_LOG_OFF(o)] == 0xF9u && fk.n_prog == 0u);
		/* not exactly FD: nothing written */
		{
			static const uint8_t vals[] = { 0xFFu, 0xFCu, 0xF9u, 0xEDu, 0xFEu };

			for (unsigned i = 0; i < 5u; i++) {
				fresh(k == 0u ? 0x00 : 0x01, 0x01);
				fk.mem[FR_LOG_OFF(o)] = vals[i];
				CHECK(!fr_log_trial_spend(&fl, o));
				CHECK(fk.n_prog == 0u && fk.mem[FR_LOG_OFF(o)] == vals[i]);
			}
		}
		/* a program that fails */
		fresh(k == 0u ? 0x00 : 0x01, 0x01);
		fk.mem[FR_LOG_OFF(o)] = 0xFDu;
		fk.fail_prog = 1;
		CHECK(!fr_log_trial_spend(&fl, o));
		/* the selected slot's log scan is as before: o's FD is not in it */
		fresh(k == 0u ? 0x00 : 0x01, 0x01);
		fk.mem[FR_LOG_OFF(o)] = 0xFDu;
		{
			struct fr_log l;

			CHECK(fr_log_start(&fl, &l) == FR_LOG_GO_MARKED);
			CHECK(l.slot == k && l.next == 0u && l.named);
			check_one_program(k, 0, started_value(0));
		}
	}
}

int main(void)
{
	RUN(test_trial_gate);
	RUN(test_no_log);
	RUN(test_states);
	RUN(test_read_failures);
	RUN(test_count);
	RUN(test_positions);
	RUN(test_bad_programs);
	RUN(test_close_refused);
	RUN(test_sc_startlog);
	RUN(test_cuts);
	RUN(test_any_sector);
	return check_summary("test_front_startlog");
}
