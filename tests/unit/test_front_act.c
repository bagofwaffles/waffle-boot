/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The actions on the image a slot holds (src/front/slots/slots.c,
 * fr_act_decide(), fr_act_begin(), S-02.084): try, switch, keep and put
 * back, each against the RAM NOR fake. Every erase and program of each
 * case, its offset, bytes and order; what the factory first stage then
 * does (S-02.020-S-02.023); the warning's tier; and a power cut after each
 * operation, and inside each (torn), with the state it leaves and a slot
 * that still starts within two resets.
 */
#include <string.h>

#include "s02_check.h"
#include "s02_fake_flash.h"

#include "front/front.h"
#include "front/handoff.h"
#include "front/slots/slots.h"
#include "waffle/boot/handoff.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static struct fake_flash g_f, g_start;
static struct fr_writer g_w;
static struct fr_home g_home;
static struct fr_running g_run;

/* ---- the flash, with each erase and program's bytes kept ---- */

struct mop {
	char kind;		/* 'e', 'p' */
	uint32_t off, len;
	uint8_t b[12];
};
static struct mop g_mops[64];
static unsigned g_nmops;

/* S-02.103: an action keeps each slot's settings sector. */
static bool in_settings(uint32_t off)
{
	return (off >= FR_SETTINGS_OFF(0) && off < FR_SETTINGS_OFF(0) + FR_SECTOR) ||
	       (off >= FR_SETTINGS_OFF(1) && off < FR_SETTINGS_OFF(1) + FR_SECTOR);
}

static int m_erase(void *ctx, uint32_t off)
{
	CHECK(!in_settings(off));
	if (g_nmops < N(g_mops))
		g_mops[g_nmops++] = (struct mop){ 'e', off, FR_SECTOR, { 0 } };
	return ff_erase(ctx, off);
}

static int m_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	CHECK(!in_settings(off));
	if (g_nmops < N(g_mops)) {
		struct mop *m = &g_mops[g_nmops++];

		*m = (struct mop){ 'p', off, len, { 0 } };
		memcpy(m->b, buf, len < 12u ? len : 12u);
	}
	return ff_program(ctx, off, buf, len);
}

static const struct fr_flash *flash(void)
{
	static struct fr_flash ops;

	ops.read = ff_read;
	ops.erase = m_erase;
	ops.program = m_program;
	ops.ctx = &g_f;
	return &ops;
}

/* ---- images with a front (tools/mkldr.py --front's layout) ---- */

static uint32_t put_block(uint8_t *s, uint32_t o, uint32_t flags, uint32_t target, uint32_t count,
			  const uint8_t *payload)
{
	uint8_t x = 0;

	ff_put32(s + o, 0xAD000004u | flags);
	ff_put32(s + o + 4, target);
	ff_put32(s + o + 8, count);
	ff_put32(s + o + 12, 0);
	for (unsigned i = 0; i < 16u; i++)
		x = (uint8_t)(x ^ s[o + i]);
	s[o + 2] = x;
	o += 16;
	if (!(flags & S05_FLAG_FILL) && payload != NULL) {
		memcpy(s + o, payload, count);
		o += count;
	}
	return o;
}

/* An L1 code byte of the front, in every image built here. */
#define FRONT_BYTE 37u

/* n bytes: the stream with the front of fseed (its descriptor's front CRC
 * right or not), then seeded bytes. The version's minor is seed. */
static void build_image(uint8_t *dst, uint32_t n, uint32_t seed, uint32_t fseed, bool front_ok)
{
	uint8_t l1c[256], l1d[64], l2[1024], ml2[512], desc[FR_DESC_MAX];
	uint32_t off = 0, size = FR_DESC_HEAD + 8u + 4u, fcrc;

	ff_fill_image(dst, n, seed);
	ff_fill_image(l1c, sizeof l1c, fseed + 1u);
	ff_fill_image(l1d, sizeof l1d, fseed + 2u);
	ff_fill_image(l2, sizeof l2, fseed + 3u);
	ff_fill_image(ml2, sizeof ml2, seed + 4u);
	fcrc = ref_crc32(0, l1c, sizeof l1c);
	fcrc = ref_crc32(fcrc, l1d, sizeof l1d);
	fcrc = ref_crc32(fcrc, l2, sizeof l2);
	memset(desc, 0, sizeof desc);
	ff_put32(desc + FR_DESC_O_MAGIC, FR_DESC_MAGIC);
	desc[FR_DESC_O_VERSION] = (uint8_t)FR_DESC_VERSION;
	desc[FR_DESC_O_SIZE] = (uint8_t)size;
	ff_put32(desc + FR_DESC_O_MAIN_ENTRY, 0x08031000u);
	ff_put32(desc + FR_DESC_O_NREGIONS, 1);
	ff_put32(desc + FR_DESC_O_MAIN_CRC, ref_crc32(0, ml2, sizeof ml2));
	ff_put32(desc + FR_DESC_O_FRONT_CRC, front_ok ? fcrc : fcrc ^ 1u);
	ff_fill_image(desc + FR_DESC_O_FRONT_PIN, FR_SHA256_LEN, fseed + 5u);
	desc[FR_DESC_O_BL_VERSION] = 1u;
	desc[FR_DESC_O_BL_VERSION + 2u] = (uint8_t)seed;
	ff_put32(desc + FR_DESC_O_REGIONS, 0x08031000u);
	ff_put32(desc + FR_DESC_O_REGIONS + 4u, sizeof ml2);
	ff_put32(desc + size - 4u, ref_crc32(0, desc, size - 4u));

	off = put_block(dst, off, S05_FLAG_FIRST | S05_FLAG_IGNORE, FR_L2_START, 0, NULL);
	off = put_block(dst, off, 0, FR_L1_CODE_START, sizeof l1c, l1c);
	off = put_block(dst, off, 0, FR_L1_DATA_START, sizeof l1d, l1d);
	off = put_block(dst, off, S05_FLAG_INIT, FR_L1_CODE_START, 0, NULL);
	off = put_block(dst, off, 0, FR_L2_START, sizeof l2, l2);
	off = put_block(dst, off, 0, FR_DESC_ADDR, size, desc);
	off = put_block(dst, off, 0, 0x08031000u, sizeof ml2, ml2);
	off = put_block(dst, off, S05_FLAG_FINAL, FR_L2_START, 0, NULL);
	CHECK(off <= n);
	if (n <= FR_LOG_IMAGE_MAX)
		CHECK_EQ(fr_image_valid(dst, n, NULL), front_ok ? FR_IMG_OK : FR_IMG_NO_FRONT);
}

static uint8_t g_img[FR_SLOT_CAP];

/* Slot k: n bytes and the record {crc32, n, tag}, its log byte 0 log0. */
static void put_slot(unsigned k, uint32_t n, uint32_t crc32, uint8_t log0)
{
	memset(g_f.mem + FR_SLOT_BASE(k), 0xFF, FR_SLOT_CAP);
	memcpy(g_f.mem + FR_SLOT_BASE(k), g_img, n);
	memset(g_f.mem + FR_RECORD_OFF(k), 0xFF, FR_SECTOR);
	ff_put32(g_f.mem + FR_RECORD_OFF(k), crc32);
	ff_put32(g_f.mem + FR_RECORD_OFF(k) + 4u, n);
	ff_put32(g_f.mem + FR_RECORD_OFF(k) + 8u, FR_TAG);
	if (n <= FR_LOG_IMAGE_MAX)
		g_f.mem[FR_LOG_OFF(k)] = log0;
}

static uint32_t crc_of(uint32_t n)
{
	return ref_crc32(0, g_img, n);
}

/* ---- the states ---- */

#define LEN_A   0x3000u
#define LEN_B   0x2400u
#define LEN_BIG 0x77800u	/* inside the slot, past FR_LOG_IMAGE_MAX: no log */
#define F_OWN   500u		/* the running slot's front */
#define F_OTHER 900u

/* What the other slot holds. */
enum {
	BK_GOOD,		/* the running front, a log */
	BK_GOOD_NOLOG,		/* the same, longer than FR_LOG_IMAGE_MAX */
	BK_GOOD_PENDING,	/* good, its log's entry 0 pending-coded (0xFD) */
	BK_FRONT,		/* another waffle-boot front */
	BK_FOREIGN,		/* a bootable stream with no waffle-boot front */
	BK_ALTERED,		/* the running front's descriptor, a front byte changed */
	BK_TRIED,		/* the trial mark, entry 0 pending */
	BK_TRIED_NOLOG,
	BK_STARTED,		/* tried, entry 0 finished (0xFC) */
	BK_SPENT,		/* tried, entry 0 spent (0xF9) */
	BK_TRIED_FRONT,		/* tried, another front */
	BK_BAD_CRC,		/* the tag, a length, a CRC its bytes do not have */
	BK_BAD_EMPTY,		/* {0, 0, tag}: the check may pass it */
	BK_BAD_LONG,		/* the tag with a length over the slot */
	BK_ERASED,		/* nothing */
	BK_N
};

static uint32_t g_crc_b;	/* the CRC-32 of B's bytes */
static uint32_t g_len_b;

static void put_b(unsigned b, unsigned kind)
{
	const bool nolog = kind == BK_GOOD_NOLOG || kind == BK_TRIED_NOLOG;
	const uint32_t n = nolog ? LEN_BIG : LEN_B;
	const uint32_t fseed = kind == BK_FRONT || kind == BK_TRIED_FRONT ? F_OTHER : F_OWN;

	build_image(g_img, n, 7u, fseed, kind != BK_FOREIGN);
	if (kind == BK_ALTERED)
		g_img[FRONT_BYTE] ^= 0x01u;
	g_crc_b = crc_of(n);
	g_len_b = n;
	switch (kind) {
	case BK_GOOD:
	case BK_GOOD_NOLOG:
	case BK_FRONT:
	case BK_FOREIGN:
	case BK_ALTERED:
		put_slot(b, n, g_crc_b, 0xFFu);
		break;
	case BK_GOOD_PENDING:
		put_slot(b, n, g_crc_b, FR_LOG0_PENDING);
		break;
	case BK_TRIED:
	case BK_TRIED_NOLOG:
	case BK_TRIED_FRONT:
		put_slot(b, n, ~g_crc_b, FR_LOG0_PENDING);
		break;
	case BK_STARTED:
		put_slot(b, n, ~g_crc_b, FR_LOG0_MARKED);
		break;
	case BK_SPENT:
		put_slot(b, n, ~g_crc_b, 0xF9u);
		break;
	case BK_BAD_CRC:
		put_slot(b, n, g_crc_b ^ 0x10u, 0xFFu);
		break;
	case BK_BAD_EMPTY:
		put_slot(b, n, 0, 0xFFu);
		ff_put32(g_f.mem + FR_RECORD_OFF(b), 0);
		ff_put32(g_f.mem + FR_RECORD_OFF(b) + 4u, 0);
		break;
	case BK_BAD_LONG:
		put_slot(b, n, g_crc_b, 0xFFu);
		ff_put32(g_f.mem + FR_RECORD_OFF(b) + 4u, FR_SLOT_CAP + 1u);
		break;
	default:
		memset(g_f.mem + FR_SLOT_BASE(b), 0xFF, FR_SLOT_CAP);
		memset(g_f.mem + FR_RECORD_OFF(b), 0xFF, FR_SECTOR);
		break;
	}
}

static uint32_t g_crc_a;

/* A = slot a runs, holding the running front; B as kind; the selector
 * {si, sv}; this recovery has done nothing. a_tried: A carries the mark,
 * its entry 0 a_log0. */
static void setup_full(unsigned a, unsigned kind, uint8_t si, uint8_t sv, bool a_tried, uint8_t a_log0)
{
	ff_init(&g_f);
	build_image(g_img, LEN_A, 1u, F_OWN, true);
	g_crc_a = crc_of(LEN_A);
	put_slot(a, LEN_A, a_tried ? ~g_crc_a : g_crc_a, a_log0);
	put_b(1u - a, kind);
	ff_set_selector(&g_f, si, sv);
	g_run.established = true;
	g_run.slot = (uint8_t)a;
	memset(&g_home, 0, sizeof g_home);
	memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
	ff_reset(&g_f);
	g_nmops = 0;
}

static void setup(unsigned a, unsigned kind)
{
	setup_full(a, kind, (uint8_t)a, 0x01u, false, 0xFFu);
}

/* The flash as setup left it, power on, nothing logged. */
static void restore(void)
{
	memcpy(g_f.mem, g_start.mem, sizeof g_f.mem);
	ff_reset(&g_f);
	g_nmops = 0;
}

/* ---- running an action ---- */

static int act(uint8_t a, bool force, struct fr_act_plan *plan)
{
	struct fr_act_plan own;
	int rc;
	unsigned steps = 0;

	if (plan == NULL)
		plan = &own;
	rc = fr_act_begin(&g_w, flash(), &g_run, &g_home, a, force, plan);
	if (rc != FR_WR_OK) {
		CHECK(fr_writer_step(&g_w));
		CHECK_EQ(g_w.result, rc);
		CHECK_EQ(g_nmops, 0);
		return rc;
	}
	CHECK_EQ(g_nmops, 0);	/* nothing before the first step */
	while (!fr_writer_step(&g_w))
		if (++steps > 1000u)
			break;
	CHECK(steps <= 1000u);
	return g_w.result;
}

/* The expected operations: kind, offset, length, first two bytes. */
struct want {
	char kind;
	uint32_t off, len;
	uint8_t b0, b1;
};
#define E(o)        { 'e', (o), FR_SECTOR, 0, 0 }
#define P1(o, v)    { 'p', (o), 1u, (v), 0 }
#define P2(o, x, y) { 'p', (o), 2u, (x), (y) }
#define PREC(k)     { 'p', FR_RECORD_OFF(k), 12u, 0, 0 }
#define SEL FR_SELECTOR_OFF

/* The operations made were exactly want[]; a record program's 12 bytes
 * {crc, n, tag}. */
static void expect_ops(const struct want *w, unsigned n, uint32_t rec_crc, uint32_t rec_len)
{
	CHECK_EQ(g_nmops, n);
	for (unsigned i = 0; i < n && i < g_nmops; i++) {
		const struct mop *m = &g_mops[i];

		CHECK_EQ(m->kind, w[i].kind);
		CHECK_EQ(m->off, w[i].off);
		CHECK_EQ(m->len, w[i].len);
		if (m->kind == 'p' && m->len == 12u) {
			uint8_t r[12];

			ff_put32(r, rec_crc);
			ff_put32(r + 4, rec_len);
			ff_put32(r + 8, FR_TAG);
			CHECK(memcmp(m->b, r, 12) == 0);
		} else if (m->kind == 'p') {
			CHECK_EQ(m->b[0], w[i].b0);
			if (m->len == 2u)
				CHECK_EQ(m->b[1], w[i].b1);
		}
	}
}

/* The factory first stage (S-02.020-S-02.023) on the flash: the slots it
 * boots at the next starts, with no cut. */
static void first_stages(unsigned *out, unsigned n)
{
	struct s02_flash_ops ops;
	struct s02_env env = ff_env(&g_f, &ops);

	ff_reset(&g_f);
	for (unsigned i = 0; i < n; i++)
		out[i] = ff_factory_first_stage(&env);
}

static void expect_starts(unsigned s0, unsigned s1)
{
	static struct fake_flash keep;
	unsigned s[2];

	memcpy(keep.mem, g_f.mem, sizeof keep.mem);
	first_stages(s, 2);
	CHECK_EQ(s[0], s0);
	CHECK_EQ(s[1], s1);
	memcpy(g_f.mem, keep.mem, sizeof g_f.mem);
}

static void sel_is(uint8_t i, uint8_t v)
{
	CHECK_EQ(g_f.mem[SEL], i);
	CHECK_EQ(g_f.mem[SEL + 1u], v);
}

static uint32_t rec_word(unsigned k, unsigned w)
{
	const uint8_t *r = g_f.mem + FR_RECORD_OFF(k) + 4u * w;

	return (uint32_t)r[0] | (uint32_t)r[1] << 8 | (uint32_t)r[2] << 16 | (uint32_t)r[3] << 24;
}

/* ---- each action's operations, and the first stage after it ---- */

static void test_try_ops(void)
{
	struct fr_act_plan p;

	/* B good, slot 1, with a log: the log pending, the mark, the selector
	 * erased; the first stage starts slot 1 once, then slot 0 */
	{
		static const struct want w[] = {
			E(FR_LOG_OFF(1)), P1(FR_LOG_OFF(1), 0xFD), E(FR_RECORD_OFF(1)), PREC(1), E(SEL),
		};

		setup(0, BK_GOOD);
		CHECK_EQ(act(FR_ACT_TRY, false, &p), FR_WR_OK);
		CHECK_EQ(p.way, FR_WAY_TRY);
		CHECK_EQ(p.target, 1);
		expect_ops(w, N(w), ~g_crc_b, g_len_b);
		CHECK(g_home.trial && !g_home.moved && !g_home.spoiled);
		sel_is(0xFF, 0xFF);
		CHECK_EQ(g_f.mem[FR_LOG_OFF(1)], 0xFD);
		CHECK_EQ(rec_word(1, 0), ~g_crc_b);
		expect_starts(1, 0);
	}
	/* B good, slot 0: then verified 0xFE, then the index 0x00 */
	{
		static const struct want w[] = {
			E(FR_LOG_OFF(0)), P1(FR_LOG_OFF(0), 0xFD), E(FR_RECORD_OFF(0)), PREC(0), E(SEL),
			P1(SEL + 1u, 0xFE), P1(SEL, 0x00),
		};

		setup(1, BK_GOOD);
		CHECK_EQ(act(FR_ACT_TRY, false, NULL), FR_WR_OK);
		expect_ops(w, N(w), ~g_crc_b, g_len_b);
		sel_is(0x00, 0xFE);
		expect_starts(0, 1);
	}
	/* B good with no log (longer than 0x77000): no log write */
	for (unsigned a = 0; a < 2u; a++) {
		const unsigned b = 1u - a;
		const struct want w[] = {
			E(FR_RECORD_OFF(b)), PREC(b), E(SEL), P1(SEL + 1u, 0xFE), P1(SEL, 0x00),
		};

		setup(a, BK_GOOD_NOLOG);
		CHECK_EQ(act(FR_ACT_TRY, false, NULL), FR_WR_OK);
		expect_ops(w, b == 0u ? 5u : 3u, ~g_crc_b, g_len_b);
		CHECK_EQ(g_f.mem[FR_LOG_OFF(b)], g_start.mem[FR_LOG_OFF(b)]);
		expect_starts(b, a);
	}
	/* B tried: its log pending again, no record write */
	for (unsigned a = 0; a < 2u; a++) {
		const unsigned b = 1u - a;
		const struct want w[] = {
			E(FR_LOG_OFF(b)), P1(FR_LOG_OFF(b), 0xFD), E(SEL), P1(SEL + 1u, 0xFE), P1(SEL, 0x00),
		};

		setup(a, BK_STARTED);
		CHECK_EQ(act(FR_ACT_TRY, false, NULL), FR_WR_OK);
		expect_ops(w, b == 0u ? 5u : 3u, 0, 0);
		CHECK_EQ(g_f.mem[FR_LOG_OFF(b)], 0xFD);
		CHECK_EQ(rec_word(b, 0), ~g_crc_b);
		expect_starts(b, a);
		/* tried with no log: the selector alone */
		setup(a, BK_TRIED_NOLOG);
		CHECK_EQ(act(FR_ACT_TRY, false, NULL), FR_WR_OK);
		expect_ops(w + 2, b == 0u ? 3u : 1u, 0, 0);
	}
	/* B bad, not tried (forced): a record sure to fail is left; the
	 * empty record and a length over the slot are erased */
	for (unsigned a = 0; a < 2u; a++) {
		static const unsigned kinds[] = { BK_BAD_CRC, BK_ERASED, BK_BAD_EMPTY, BK_BAD_LONG };
		const unsigned b = 1u - a;

		for (unsigned i = 0; i < N(kinds); i++) {
			const bool erase = kinds[i] == BK_BAD_EMPTY || kinds[i] == BK_BAD_LONG;
			struct want w[4];
			unsigned n = 0;

			if (erase)
				w[n++] = (struct want)E(FR_RECORD_OFF(b));
			w[n++] = (struct want)E(SEL);
			if (b == 0u) {
				w[n++] = (struct want)P1(SEL + 1u, 0xFE);
				w[n++] = (struct want)P1(SEL, 0x00);
			}
			setup(a, kinds[i]);
			CHECK_EQ(act(FR_ACT_TRY, false, NULL), FR_WR_TARGET_BAD);
			CHECK_EQ(g_nmops, 0);
			CHECK_EQ(act(FR_ACT_TRY, true, &p), FR_WR_OK);
			CHECK_EQ(p.tier, FR_TIER_BAD);
			expect_ops(w, n, 0, 0);
			CHECK(fr_record_fails((const struct fr_record *)&(struct fr_record){
				rec_word(b, 0), rec_word(b, 1), rec_word(b, 2) }));
			if (!erase)
				CHECK(memcmp(g_f.mem + FR_RECORD_OFF(b), g_start.mem + FR_RECORD_OFF(b), 12) == 0);
			/* the slot is started once by the first stage, then A */
			expect_starts(b, a);
		}
	}
}

/*
 * A try of B = slot 0 whose index step reads back wrong (S-02.093): the
 * selector, {00, FE}, is settled back on A = slot 1 by an erase and
 * {01, 01} a byte at a time, the index first, read back (S-02.084). A cut
 * inside that index program leaves the verified byte erased, so it never
 * leaves a torn index with verified 0x01.
 */
static void test_try_settle_back(void)
{
	static const struct want w[] = {
		E(FR_LOG_OFF(0)), P1(FR_LOG_OFF(0), 0xFD), E(FR_RECORD_OFF(0)), PREC(0), E(SEL),
		P1(SEL + 1u, 0xFE), P1(SEL, 0x00), E(SEL), P1(SEL, 0x01), P1(SEL + 1u, 0x01),
	};
	unsigned r = 0, k = 0, n_cut = 0;

	setup(1, BK_GOOD);
	CHECK_EQ(act(FR_ACT_TRY, false, NULL), FR_WR_OK);
	/* the read of the index step's read-back: the first read after the
	 * program of index 0x00 */
	for (unsigned i = 0, reads = 0, seen = 0; i < g_f.nlog && i < FF_LOG_MAX; i++) {
		const struct ff_op *o = &g_f.log[i];

		if (o->kind == FF_READ) {
			reads++;
			if (seen && r == 0u)
				r = reads;
		} else if (o->kind == FF_PROGRAM && o->off == SEL && o->len == 1u) {
			seen = 1;
		}
	}
	CHECK(r != 0u);
	restore();
	g_f.garble_read = r;
	CHECK_EQ(act(FR_ACT_TRY, false, NULL), FR_WR_SELECTOR);
	expect_ops(w, N(w), ~g_crc_b, g_len_b);
	sel_is(0x01, 0x01);
	CHECK(g_home.trial);
	expect_starts(1, 1);
	for (unsigned i = 0; i < g_nmops; i++)
		if (g_mops[i].kind == 'p' && g_mops[i].off == SEL && g_mops[i].b[0] == 0x01u)
			k = i + 1u;	/* the settling's index program */
	CHECK_EQ(k, 9);
	for (unsigned t = 0; t < 256u + 8u; t++) {
		unsigned s[2];

		restore();
		g_f.garble_read = r;
		g_f.tear = k;
		g_f.tear_kind = t < 256u ? FF_TEAR_SEED : FF_TEAR_BIT;
		g_f.tear_a = t < 256u ? t + 1u : 0u;
		g_f.tear_b = t < 256u ? 0u : t - 256u;
		(void)act(FR_ACT_TRY, false, NULL);
		CHECK(g_f.dead);
		CHECK_EQ(g_f.mem[SEL + 1u], 0xFF);	/* never {torn, 01} */
		CHECK_EQ(fr_selected_slot(g_f.mem[SEL]), 1);
		if (g_f.mem[SEL] != 0x01u)
			n_cut++;
		first_stages(s, 2);
		CHECK(s[0] == 1u && s[1] == 1u);
	}
	CHECK(n_cut > 0u);	/* some cuts tore the index */
}

static void test_switch_ops(void)
{
	struct fr_act_plan p;

	for (unsigned a = 0; a < 2u; a++) {
		const unsigned b = 1u - a;

		/* B good: {B, 00}; the first stage checks it and trusts it */
		{
			const struct want w[] = { E(SEL), P2(SEL, (uint8_t)b, 0x00) };

			setup(a, BK_GOOD);
			CHECK_EQ(act(FR_ACT_SWITCH, false, &p), FR_WR_OK);
			CHECK_EQ(p.way, FR_WAY_SWITCH);
			CHECK_EQ(p.tier, FR_TIER_SAME);
			expect_ops(w, N(w), 0, 0);
			sel_is((uint8_t)b, 0x00);
			expect_starts(b, b);
		}
		/* B good, its log pending-coded: erased first */
		{
			const struct want w[] = { E(FR_LOG_OFF(b)), E(SEL), P2(SEL, (uint8_t)b, 0x00) };

			setup(a, BK_GOOD_PENDING);
			CHECK_EQ(act(FR_ACT_SWITCH, false, NULL), FR_WR_OK);
			expect_ops(w, N(w), 0, 0);
			CHECK_EQ(g_f.mem[FR_LOG_OFF(b)], 0xFF);
		}
		/* B tried, started: the record made right, then {B, 00} */
		{
			const struct want w[] = {
				E(FR_RECORD_OFF(b)), PREC(b), E(SEL), P2(SEL, (uint8_t)b, 0x00),
			};

			setup(a, BK_STARTED);
			CHECK_EQ(act(FR_ACT_SWITCH, false, NULL), FR_WR_OK);
			expect_ops(w, N(w), g_crc_b, g_len_b);
			CHECK_EQ(rec_word(b, 0), g_crc_b);
			CHECK_EQ(g_f.mem[FR_LOG_OFF(b)], FR_LOG0_MARKED);
			expect_starts(b, b);
		}
		/* B tried, not started (forced): its pending log erased too */
		{
			const struct want w[] = {
				E(FR_LOG_OFF(b)), E(FR_RECORD_OFF(b)), PREC(b), E(SEL),
				P2(SEL, (uint8_t)b, 0x00),
			};

			setup(a, BK_TRIED);
			CHECK_EQ(act(FR_ACT_SWITCH, false, NULL), FR_WR_NOT_STARTED);
			CHECK_EQ(act(FR_ACT_SWITCH, true, NULL), FR_WR_OK);
			expect_ops(w, N(w), g_crc_b, g_len_b);
			expect_starts(b, b);
		}
		/* B bad with a bootable stream (forced): {B, 01}, the index
		 * first, which no first stage checks */
		{
			const struct want w[] = { E(SEL), P1(SEL, (uint8_t)b), P1(SEL + 1u, 0x01) };

			setup(a, BK_BAD_CRC);
			CHECK_EQ(act(FR_ACT_SWITCH, false, NULL), FR_WR_TARGET_BAD);
			CHECK_EQ(act(FR_ACT_SWITCH, true, &p), FR_WR_OK);
			CHECK_EQ(p.tier, FR_TIER_BAD);
			expect_ops(w, N(w), 0, 0);
			sel_is((uint8_t)b, 0x01);
			expect_starts(b, b);
		}
		/* nothing bootable in B: refused, forced or not */
		setup(a, BK_ERASED);
		CHECK_EQ(act(FR_ACT_SWITCH, true, NULL), FR_WR_IMAGE);
		CHECK_EQ(act(FR_ACT_SWITCH, false, NULL), FR_WR_IMAGE);
	}
}

/*
 * The damaged state: this recovery began a write of A, and A reads bad.
 * A switch (forced) settles the selector on B instead of an erase and a
 * put: nothing where it trusts B already, programs alone for slot 0 where
 * verified has bit 0 set, else an erase and {B, 01}, the index byte first
 * and read back (selector_put()'s rule for this state).
 */
static void test_switch_damaged(void)
{
	static const struct {
		unsigned a;
		uint8_t si, sv;
	} cs[] = {
		{ 0, 0x00, 0x01 }, { 0, 0x00, 0xFF }, { 0, 0x00, 0x00 }, { 0, 0x01, 0xFF },
		{ 1, 0x01, 0x01 }, { 1, 0x01, 0xFF }, { 1, 0x01, 0x00 }, { 1, 0x00, 0xFE },
	};

	for (unsigned i = 0; i < N(cs); i++) {
		const unsigned a = cs[i].a, b = 1u - a;
		unsigned progs2 = 0;

		setup_full(a, BK_GOOD, cs[i].si, cs[i].sv, false, 0xFFu);
		/* A damaged by this recovery: its first sector erased */
		memset(g_start.mem + FR_SLOT_BASE(a), 0xFF, FR_SECTOR);
		g_home.spoiled = true;
		g_home.moved = cs[i].si == b;
		restore();
		CHECK_EQ(act(FR_ACT_SWITCH, false, NULL), FR_WR_TRIAL_STATE);
		CHECK_EQ(act(FR_ACT_SWITCH, true, NULL), FR_WR_OK);
		/* the selector settled on B, trusted */
		CHECK_EQ(fr_selected_slot(g_f.mem[SEL]), b);
		CHECK(g_f.mem[SEL + 1u] == 0x01u || b == 1u);
		for (unsigned k = 0; k < g_nmops; k++) {
			CHECK(g_mops[k].off >= SEL && g_mops[k].off < SEL + FR_SECTOR);
			if (g_mops[k].kind == 'p' && g_mops[k].len == 2u)
				progs2++;
		}
		/* no program of the index and verified at once in this state */
		CHECK_EQ(progs2, 0);
		{
			unsigned s[3];

			first_stages(s, 3);
			CHECK_EQ(s[0], b);
			CHECK_EQ(s[1], b);
		}
	}
}

/* The same settling cut inside its selector program, every way the cut
 * can leave the bits (64 seeds each): the selector never trusts the damaged
 * slot A: each settling programs the index first. */
static void test_switch_damaged_torn(void)
{
	static const struct {
		unsigned a;
		uint8_t si, sv;
	} cs[] = {
		{ 0, 0x00, 0x01 }, { 0, 0x00, 0x00 }, { 1, 0x01, 0x00 }, { 1, 0x00, 0xFE },
	};
	unsigned trusted_a = 0;

	for (unsigned i = 0; i < N(cs); i++) {
		const unsigned a = cs[i].a;
		unsigned m;

		setup_full(a, BK_GOOD, cs[i].si, cs[i].sv, false, 0xFFu);
		memset(g_start.mem + FR_SLOT_BASE(a), 0xFF, FR_SECTOR);
		g_home.spoiled = true;
		restore();
		CHECK_EQ(act(FR_ACT_SWITCH, true, NULL), FR_WR_OK);
		m = g_f.nmut;
		for (unsigned k = 1; k <= m; k++)
			for (unsigned t = 1; t <= 64u; t++) {
				restore();
				g_f.tear = k;
				g_f.tear_a = t;
				(void)act(FR_ACT_SWITCH, true, NULL);
				ff_reset(&g_f);
				if (g_f.mem[SEL + 1u] == 0x01u && fr_selected_slot(g_f.mem[SEL]) == a)
					trusted_a++;
			}
	}
	CHECK_EQ(trusted_a, 0);
	/* a put back's settling back on A, the selector naming B, tried, with
	 * verified bit 0 clear: no torn program trusts B */
	for (unsigned v = 0; v < 2u; v++) {
		unsigned m, trusted_b = 0;

		setup_full(0, BK_TRIED, 0x01, v == 0u ? 0x00 : 0xFE, false, 0xFFu);
		CHECK_EQ(act(FR_ACT_PUT_BACK, false, NULL), FR_WR_OK);
		m = g_f.nmut;
		for (unsigned k = 1; k <= m; k++)
			for (unsigned t = 1; t <= 64u; t++) {
				restore();
				g_f.tear = k;
				g_f.tear_a = t;
				(void)act(FR_ACT_PUT_BACK, false, NULL);
				ff_reset(&g_f);
				if (g_f.mem[SEL + 1u] == 0x01u && fr_selected_slot(g_f.mem[SEL]) == 1u &&
				    rec_word(1, 0) != g_crc_b)
					trusted_b++;
			}
		CHECK_EQ(trusted_b, 0);
	}
}

static void test_keep_ops(void)
{
	struct fr_act_plan p;

	for (unsigned a = 0; a < 2u; a++) {
		const unsigned b = 1u - a;

		/* B tried and started: made right, {B, 00} */
		{
			const struct want w[] = {
				E(FR_RECORD_OFF(b)), PREC(b), E(SEL), P2(SEL, (uint8_t)b, 0x00),
			};

			setup(a, BK_STARTED);
			CHECK_EQ(act(FR_ACT_KEEP, false, &p), FR_WR_OK);
			CHECK_EQ(p.way, FR_WAY_KEEP);
			CHECK_EQ(p.target, b);
			expect_ops(w, N(w), g_crc_b, g_len_b);
			expect_starts(b, b);
		}
		/* not started: 21, and forced: the pending log erased too */
		{
			const struct want w[] = {
				E(FR_LOG_OFF(b)), E(FR_RECORD_OFF(b)), PREC(b), E(SEL),
				P2(SEL, (uint8_t)b, 0x00),
			};

			setup(a, BK_TRIED);
			CHECK_EQ(act(FR_ACT_KEEP, false, NULL), FR_WR_NOT_STARTED);
			CHECK_EQ(act(FR_ACT_KEEP, true, NULL), FR_WR_OK);
			expect_ops(w, N(w), g_crc_b, g_len_b);
		}
		/* a trial start of A: A tried, B good and trusted: entry 0
		 * marked finished (0xFD to 0xFC), A made right, {A, 00} */
		{
			const struct want w[] = {
				P1(FR_LOG_OFF(a), 0xFC), E(FR_RECORD_OFF(a)), PREC(a), E(SEL),
				P2(SEL, (uint8_t)a, 0x00),
			};

			setup_full(a, BK_GOOD, (uint8_t)b, 0x01, true, FR_LOG0_PENDING);
			CHECK_EQ(act(FR_ACT_KEEP, false, &p), FR_WR_OK);
			CHECK_EQ(p.target, a);
			expect_ops(w, N(w), g_crc_a, LEN_A);
			CHECK_EQ(g_f.mem[FR_LOG_OFF(a)], FR_LOG0_MARKED);
			CHECK_EQ(rec_word(a, 0), g_crc_a);
			expect_starts(a, a);
		}
		/* A's trial spent (0xF9): 20, and forced: the log erased, not
		 * marked */
		{
			const struct want w[] = {
				E(FR_LOG_OFF(a)), E(FR_RECORD_OFF(a)), PREC(a), E(SEL),
				P2(SEL, (uint8_t)a, 0x00),
			};

			setup_full(a, BK_GOOD, (uint8_t)b, 0x01, true, 0xF9u);
			CHECK_EQ(act(FR_ACT_KEEP, false, NULL), FR_WR_NOT_TRIED);
			CHECK_EQ(act(FR_ACT_KEEP, true, NULL), FR_WR_OK);
			expect_ops(w, N(w), g_crc_a, LEN_A);
			CHECK_EQ(g_f.mem[FR_LOG_OFF(a)], 0xFF);
		}
		/* nothing tried: 20 */
		setup(a, BK_GOOD);
		CHECK_EQ(act(FR_ACT_KEEP, true, NULL), FR_WR_NOT_TRIED);
	}
}

static void test_put_back_ops(void)
{
	struct fr_act_plan p;

	for (unsigned a = 0; a < 2u; a++) {
		const unsigned b = 1u - a;

		/* B tried, the selector on A: B made right, no selector write */
		{
			const struct want w[] = { E(FR_LOG_OFF(b)), E(FR_RECORD_OFF(b)), PREC(b) };

			setup(a, BK_TRIED);
			CHECK_EQ(act(FR_ACT_PUT_BACK, false, &p), FR_WR_OK);
			CHECK_EQ(p.way, FR_WAY_PUT_BACK);
			expect_ops(w, N(w), g_crc_b, g_len_b);
			sel_is((uint8_t)a, 0x01);
			expect_starts(a, a);
			/* started: its mark stays */
			setup(a, BK_STARTED);
			CHECK_EQ(act(FR_ACT_PUT_BACK, false, NULL), FR_WR_OK);
			expect_ops(w + 1, 2, g_crc_b, g_len_b);
		}
		/* after a try not yet started (the selector names B unverified):
		 * settled back on A first */
		{
			setup(a, BK_GOOD);
			CHECK_EQ(act(FR_ACT_TRY, false, NULL), FR_WR_OK);
			memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
			memset(&g_home, 0, sizeof g_home);
			restore();
			CHECK_EQ(act(FR_ACT_PUT_BACK, false, NULL), FR_WR_OK);
			if (a == 0u) {
				/* {FF FF}: programs alone, the index first */
				const struct want w[] = {
					P1(SEL, 0x00), P1(SEL + 1u, 0x01), E(FR_LOG_OFF(b)),
					E(FR_RECORD_OFF(b)), PREC(b),
				};

				expect_ops(w, N(w), g_crc_b, g_len_b);
			} else {
				/* {00 FE}: erased, then {01 01}, the index first */
				const struct want w[] = {
					E(SEL), P1(SEL, 0x01), P1(SEL + 1u, 0x01), E(FR_LOG_OFF(b)),
					E(FR_RECORD_OFF(b)), PREC(b),
				};

				expect_ops(w, N(w), g_crc_b, g_len_b);
			}
			sel_is((uint8_t)a, 0x01);
			CHECK_EQ(rec_word(b, 0), g_crc_b);
			expect_starts(a, a);
		}
		/* nothing tried: 20, forced or not */
		setup(a, BK_GOOD);
		CHECK_EQ(act(FR_ACT_PUT_BACK, false, NULL), FR_WR_NOT_TRIED);
		CHECK_EQ(act(FR_ACT_PUT_BACK, true, NULL), FR_WR_NOT_TRIED);
	}
}

/* fr_act_decide()'s tier of B, the same for a try and a switch: the same
 * front (pin and front CRC, its bytes intact), another one, none, a bad
 * record; a tried slot by its image. */
static void test_tiers(void)
{
	static const struct {
		unsigned kind, tier;
	} ts[] = {
		{ BK_GOOD, FR_TIER_SAME }, { BK_GOOD_NOLOG, FR_TIER_SAME },
		{ BK_GOOD_PENDING, FR_TIER_SAME }, { BK_FRONT, FR_TIER_FRONT },
		{ BK_FOREIGN, FR_TIER_FOREIGN }, { BK_ALTERED, FR_TIER_FOREIGN },
		{ BK_TRIED, FR_TIER_SAME }, { BK_STARTED, FR_TIER_SAME },
		{ BK_TRIED_FRONT, FR_TIER_FRONT }, { BK_BAD_CRC, FR_TIER_BAD },
		{ BK_BAD_EMPTY, FR_TIER_BAD }, { BK_BAD_LONG, FR_TIER_BAD }, { BK_ERASED, FR_TIER_BAD },
	};

	for (unsigned a = 0; a < 2u; a++)
		for (unsigned i = 0; i < N(ts); i++)
			for (uint8_t ac = FR_ACT_TRY; ac <= FR_ACT_SWITCH; ac++) {
				struct fr_act_plan p;

				setup(a, ts[i].kind);
				(void)act(ac, true, &p);
				CHECK_EQ(p.tier, ts[i].tier);
				/* unforced, a switch goes only to the same front */
				if (ac == FR_ACT_SWITCH && (ts[i].kind == BK_FRONT || ts[i].kind == BK_FOREIGN ||
							    ts[i].kind == BK_ALTERED)) {
					setup(a, ts[i].kind);
					CHECK_EQ(act(ac, false, NULL), FR_WR_FRONT);
				}
			}
}

/* ---- power cuts ---- */

/* The state the flash is in now, read as the front reads it. */
struct state {
	bool good[2], tried[2], boot[2];
	uint8_t si, sv;
};

static struct state state_now(void)
{
	struct state s;
	struct fr_slot_info in;

	ff_reset(&g_f);
	for (unsigned k = 0; k < 2u; k++) {
		CHECK_EQ(fr_slot_info(flash(), k, &in), FR_FLASH_OK);
		s.good[k] = in.good;
		s.tried[k] = in.tried;
		s.boot[k] = in.bootable;
	}
	s.si = g_f.mem[SEL];
	s.sv = g_f.mem[SEL + 1u];
	return s;
}

/* Some slot that holds a bootable stream starts within two resets of the
 * factory first stage, and no selector trusts (verified 0x01) a slot that
 * is tried or holds no bootable stream, but where bad_ok allows a bad one. */
static unsigned g_cut_cases;

static void judge_cut(const struct state *s, bool bad_ok, unsigned a)
{
	static struct fake_flash keep;
	const unsigned n = fr_selected_slot(s->si);
	unsigned boots[2];

	g_cut_cases++;
	if (s->sv == 0x01u) {
		CHECK(!s->tried[n]);
		CHECK(s->boot[n]);
		CHECK(s->good[n] || bad_ok || n == a);
	}
	memcpy(keep.mem, g_f.mem, sizeof keep.mem);
	first_stages(boots, 2);
	CHECK(s->boot[boots[0]] || s->boot[boots[1]]);
	memcpy(g_f.mem, keep.mem, sizeof g_f.mem);
}

/* Each cut of one action: after each operation, and torn inside each (two
 * seeds); the state judged. The number of operations of the whole. */
static unsigned cuts(uint8_t ac, bool force, bool bad_ok, unsigned a)
{
	unsigned m;

	restore();
	CHECK_EQ(act(ac, force, NULL), FR_WR_OK);
	m = g_f.nmut;
	CHECK(m >= 1u);
	for (unsigned k = 1; k <= m; k++)
		for (unsigned t = 0; t < 3u; t++) {
			struct state s;
			const struct fr_home home0 = g_home;

			restore();
			memset(&g_home, 0, sizeof g_home);
			if (t == 0u) {
				g_f.cut_after = k;
			} else {
				g_f.tear = k;
				g_f.tear_a = t;
			}
			(void)act(ac, force, NULL);
			CHECK(g_w.state == FR_WS_DONE);
			s = state_now();
			judge_cut(&s, bad_ok, a);
			g_home = home0;
		}
	return m;
}

/* Every case of every action, cut. */
static void test_cuts(void)
{
	static const struct {
		uint8_t act;
		unsigned kind;
		bool force, bad_ok;
	} cs[] = {
		{ FR_ACT_TRY, BK_GOOD, false, false }, { FR_ACT_TRY, BK_GOOD_NOLOG, false, false },
		{ FR_ACT_TRY, BK_TRIED, false, false }, { FR_ACT_TRY, BK_STARTED, false, false },
		{ FR_ACT_TRY, BK_FRONT, false, false }, { FR_ACT_TRY, BK_BAD_CRC, true, false },
		{ FR_ACT_TRY, BK_BAD_EMPTY, true, false }, { FR_ACT_TRY, BK_BAD_LONG, true, false },
		{ FR_ACT_TRY, BK_ERASED, true, false },
		{ FR_ACT_SWITCH, BK_GOOD, false, false }, { FR_ACT_SWITCH, BK_GOOD_PENDING, false, false },
		{ FR_ACT_SWITCH, BK_STARTED, false, false }, { FR_ACT_SWITCH, BK_TRIED, true, false },
		{ FR_ACT_SWITCH, BK_BAD_CRC, true, true }, { FR_ACT_SWITCH, BK_FRONT, true, false },
		{ FR_ACT_KEEP, BK_STARTED, false, false }, { FR_ACT_KEEP, BK_TRIED, true, false },
		{ FR_ACT_PUT_BACK, BK_TRIED, false, false }, { FR_ACT_PUT_BACK, BK_STARTED, false, false },
	};
	unsigned total = 0;

	for (unsigned a = 0; a < 2u; a++) {
		for (unsigned i = 0; i < N(cs); i++) {
			setup(a, cs[i].kind);
			total += cuts(cs[i].act, cs[i].force, cs[i].bad_ok, a);
		}
		/* keep in a trial start of A */
		setup_full(a, BK_GOOD, (uint8_t)(1u - a), 0x01, true, FR_LOG0_PENDING);
		total += cuts(FR_ACT_KEEP, false, false, a);
		/* put back after a try, not started */
		setup(a, BK_GOOD);
		CHECK_EQ(act(FR_ACT_TRY, false, NULL), FR_WR_OK);
		memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
		memset(&g_home, 0, sizeof g_home);
		total += cuts(FR_ACT_PUT_BACK, false, false, a);
	}
	CHECK(total > 100u);
	CHECK(g_cut_cases == 3u * total);
}

/*
 * The states some cuts leave, exactly. A try of a good B cut between its
 * record's erase and program: B's record erased (bad, not tried), the
 * selector still {A, 01}: A starts. Cut after the program: B tried, the
 * selector {A, 01}. Cut after the selector's erase: {FF FF}, B started
 * once. A switch cut after its selector erase: {FF FF}, the first stage
 * checks slot 1. A keep of B cut after its record's erase: B bad, {A, 01}.
 */
static void test_cut_states(void)
{
	for (unsigned a = 0; a < 2u; a++) {
		const unsigned b = 1u - a;
		struct state s;

		/* try: E log, P log, E rec, P rec, E sel ... */
		setup(a, BK_GOOD);
		g_f.cut_after = 3;
		CHECK_EQ(act(FR_ACT_TRY, false, NULL), FR_WR_FLASH);
		s = state_now();
		CHECK(!s.good[b] && !s.tried[b] && s.good[a]);
		CHECK(rec_word(b, 0) == 0xFFFFFFFFu && rec_word(b, 1) == 0xFFFFFFFFu &&
		      rec_word(b, 2) == 0xFFFFFFFFu);
		CHECK(s.si == a && s.sv == 0x01u);
		CHECK_EQ(g_f.mem[FR_LOG_OFF(b)], 0xFD);
		CHECK(g_home.trial);
		expect_starts(a, a);

		restore();
		memset(&g_home, 0, sizeof g_home);
		g_f.cut_after = 4;
		(void)act(FR_ACT_TRY, false, NULL);
		s = state_now();
		CHECK(s.tried[b] && !s.good[b] && s.si == a && s.sv == 0x01u);
		expect_starts(a, a);

		restore();
		memset(&g_home, 0, sizeof g_home);
		g_f.cut_after = 5;
		(void)act(FR_ACT_TRY, false, NULL);
		s = state_now();
		CHECK(s.tried[b] && s.si == 0xFFu && s.sv == 0xFFu);
		/* {FF FF} names slot 1: B started once, or A checked and trusted */
		expect_starts(1, a == 0u ? 0u : 1u);

		/* switch to a good B cut after the erase */
		setup(a, BK_GOOD);
		g_f.cut_after = 1;
		CHECK_EQ(act(FR_ACT_SWITCH, false, NULL), FR_WR_SELECTOR);
		s = state_now();
		CHECK(s.si == 0xFFu && s.sv == 0xFFu && s.good[0] && s.good[1]);
		expect_starts(1, 1);

		/* keep of B cut after its record's erase: B bad, A selected */
		setup(a, BK_STARTED);
		g_f.cut_after = 1;
		CHECK_EQ(act(FR_ACT_KEEP, false, NULL), FR_WR_FLASH);
		s = state_now();
		CHECK(!s.good[b] && !s.tried[b] && s.si == a && s.sv == 0x01u);
		expect_starts(a, a);
	}
}

int main(void)
{
	test_try_ops();
	test_try_settle_back();
	test_switch_ops();
	test_switch_damaged();
	test_switch_damaged_torn();
	test_keep_ops();
	test_put_back_ops();
	test_tiers();
	test_cuts();
	test_cut_states();
	return finish("test_front_act");
}
