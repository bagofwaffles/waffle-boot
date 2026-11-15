/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Two slots that hold the same image, a supported state (S-02.084,
 * S-01.002), against the RAM NOR fake and the first stage's rules
 * (S-02.020 to S-02.023): the running slot each start establishes, from
 * the memory that start loaded; recovery's plans and writes; try, switch,
 * keep and put back; a pending trial of the image the kept slot holds; and
 * a power cut after every erase and program of each of them, and a torn
 * version of each. After every cut the unit starts, on each of the next
 * resets, a slot whose stream is whole, the front establishes its running
 * slot, and a write from recovery goes through and starts.
 */
#include <string.h>

#include "s02_check.h"
#include "s02_fake_flash.h"

#include "bearssl_hash.h"
#include "front/fr.h"
#include "front/fr_handoff.h"
#include "front/fr_proto.h"
#include "front/fr_slots.h"
#include "front/fr_startlog.h"
#include "waffle/s05_handoff.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static struct fake_flash g_f, g_base, g_keep;
static struct fr_writer g_w;
static struct fr_home g_home;

static const struct fr_flash *flash(void)
{
	static struct fr_flash ops;

	ops.read = ff_read;
	ops.erase = ff_erase;
	ops.program = ff_program;
	ops.ctx = &g_f;
	return &ops;
}

static void sha256(uint8_t out[32], const uint8_t *p, size_t n)
{
	br_sha256_context c;

	br_sha256_init(&c);
	br_sha256_update(&c, p, n);
	br_sha256_out(&c, out);
}

/* --- An image with a front (tools/mkldr.py --front's layout) --- */

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
	if (payload != NULL) {
		memcpy(s + o, payload, count);
		o += count;
	}
	return o;
}

#define N_IMAGE 0x2345u
#define FRONT_SEED 500u

/* N_IMAGE bytes into dst: one front for every image, the main part from
 * seed; the stream's length. */
static uint32_t build_image(uint8_t *dst, uint32_t seed)
{
	uint8_t l1c[256], l1d[64], l2[1024], ml2[512], desc[FR_DESC_MAX];
	uint32_t off = 0, size = FR_DESC_HEAD + 8u + 4u, fcrc;

	ff_fill_image(dst, N_IMAGE, seed);
	ff_fill_image(l1c, sizeof l1c, FRONT_SEED + 1u);
	ff_fill_image(l1d, sizeof l1d, FRONT_SEED + 2u);
	ff_fill_image(l2, sizeof l2, FRONT_SEED + 3u);
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
	ff_put32(desc + FR_DESC_O_FRONT_CRC, fcrc);
	ff_fill_image(desc + FR_DESC_O_FRONT_PIN, FR_SHA256_LEN, FRONT_SEED + 5u);
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
	CHECK(off <= N_IMAGE);
	CHECK_EQ(fr_image_valid(dst, N_IMAGE, NULL), FR_IMG_OK);
	return off;
}

/* X is the image both slots hold; Y another build with the same front. */
static uint8_t g_x[N_IMAGE], g_y[N_IMAGE];
static uint8_t g_xsha[32], g_ysha[32];
static uint32_t g_xend, g_yend;

static void put_slot(unsigned k, const uint8_t *img)
{
	memset(g_f.mem + FR_SLOT_BASE(k), 0xFF, FR_SLOT_CAP);
	memcpy(g_f.mem + FR_SLOT_BASE(k), img, N_IMAGE);
	memset(g_f.mem + FR_RECORD_OFF(k), 0xFF, FR_SECTOR);
	ff_put32(g_f.mem + FR_RECORD_OFF(k), ref_crc32(0, img, N_IMAGE));
	ff_put32(g_f.mem + FR_RECORD_OFF(k) + 4u, N_IMAGE);
	ff_put32(g_f.mem + FR_RECORD_OFF(k) + 8u, FR_TAG);
}

/* Both slots hold X and are good, each with a few starts in its log; the
 * selector names slot a, verified. Nothing done yet in this recovery. */
static void same(unsigned a)
{
	ff_init(&g_f);
	ff_fill_image(g_f.mem, FR_SELECTOR_OFF, 55u);
	put_slot(0, g_x);
	put_slot(1, g_x);
	g_f.mem[FR_LOG_OFF(0)] = 0xEAu;
	g_f.mem[FR_LOG_OFF(1)] = 0xFAu;
	ff_set_selector(&g_f, (uint8_t)a, 0x01);
	memset(&g_home, 0, sizeof g_home);
	ff_reset(&g_f);
}

/* Slot k's stream is whole: the boot ROM loads X or Y from it, whatever
 * its record says. */
static bool whole(unsigned k)
{
	const uint8_t *s = g_f.mem + FR_SLOT_BASE(k);

	return memcmp(s, g_x, g_xend) == 0 || memcmp(s, g_y, g_yend) == 0;
}

/* --- The memory a start loaded (as fr_mem_readable() lets it be read) --- */

struct ld_block {
	uint32_t target, count, off;
};

static struct ld_block g_ldb[16];
static unsigned g_nldb, g_ld_slot;
static uint8_t g_loaded[N_IMAGE];	/* the stream as it was loaded */

static void load_slot(unsigned k)
{
	const uint8_t *s = g_loaded;
	uint32_t off = 0;

	memcpy(g_loaded, g_f.mem + FR_SLOT_BASE(k), N_IMAGE);
	g_ld_slot = k;
	g_nldb = 0;
	while (off + 16u <= N_IMAGE) {
		const uint32_t flags = fr_le32(s + off) & 0xFFF0u, count = fr_le32(s + off + 8);

		off += 16u;
		if (!(flags & S05_FLAG_FILL)) {
			if (count != 0u && !(flags & S05_FLAG_IGNORE) && g_nldb < N(g_ldb)) {
				g_ldb[g_nldb].target = fr_le32(s + off - 12u);
				g_ldb[g_nldb].count = count;
				g_ldb[g_nldb].off = off;
				g_nldb++;
			}
			off += count;
		}
		if (flags & S05_FLAG_FINAL)
			break;
	}
}

static const uint8_t *ld_map(void *ctx, uint32_t addr, uint32_t len)
{
	(void)ctx;
	if (!fr_mem_readable(addr, len))
		return NULL;
	for (unsigned i = 0; i < g_nldb; i++)
		if (addr >= g_ldb[i].target &&
		    (uint64_t)addr + len <= (uint64_t)g_ldb[i].target + g_ldb[i].count)
			return g_loaded + g_ldb[i].off + (addr - g_ldb[i].target);
	return NULL;
}

static const struct fr_mem g_mem = { ld_map, NULL };

/*
 * One start from the flash as it is: the first stage's step with its own
 * selector write (S-02.020 to S-02.023), the slot it boots loaded, then
 * either the front's start-log step on the way to the main image (*trial
 * what it did of a pending trial), or recovery, which makes no such step;
 * and the running slot that front establishes. The slot booted.
 */
static unsigned start(bool to_main, struct fr_running *run, unsigned *trial)
{
	struct s02_flash_ops ops;
	struct s02_env env = ff_env(&g_f, &ops);
	struct fr_log l;
	unsigned k, slot = FR_NO_SLOT, t = FR_LOG_TRIAL_NONE;
	bool est = false;

	ff_reset(&g_f);
	k = ff_factory_first_stage(&env);
	CHECK(whole(k));		/* the unit starts */
	load_slot(k);
	if (to_main) {
		(void)fr_log_step(flash(), &g_mem, &l);
		t = l.trial;
	}
	CHECK_EQ(fr_running_slot(flash(), &g_mem, &slot, &est), FR_FLASH_OK);
	run->established = est;
	run->slot = (uint8_t)slot;
	if (trial != NULL)
		*trial = t;
	ff_reset(&g_f);
	return k;
}

static int steps(void)
{
	unsigned n = 0;

	while (!fr_writer_step(&g_w))
		if (++n > 4000u)
			break;
	CHECK(n <= 4000u);
	return g_w.result;
}

static int write_img(const struct fr_running *run, const uint8_t *img, const uint8_t *sha,
		     uint8_t flags)
{
	const int rc = fr_writer_begin(&g_w, flash(), run, &g_home, img, N_IMAGE, sha, flags);

	return rc != FR_WR_OK ? rc : steps();
}

static int act(const struct fr_running *run, uint8_t a, bool force, struct fr_act_plan *plan)
{
	const int rc = fr_act_begin(&g_w, flash(), run, &g_home, a, force, plan);

	return rc != FR_WR_OK ? rc : steps();
}

static struct fr_slot_info info(unsigned k)
{
	struct fr_slot_info i;

	CHECK_EQ(fr_slot_info(flash(), k, &i), FR_FLASH_OK);
	return i;
}

static bool sel_is(uint8_t index, uint8_t verified)
{
	return g_f.mem[FR_SELECTOR_OFF] == index && g_f.mem[FR_SELECTOR_OFF + 1u] == verified;
}

/* --- The running slot each start establishes --- */

static void set_record(unsigned k, unsigned kind)
{
	uint8_t *r = g_f.mem + FR_RECORD_OFF(k);

	if (kind == 1u)				/* the trial mark */
		ff_put32(r, ~ref_crc32(0, g_x, N_IMAGE));
	else if (kind == 2u)			/* erased */
		memset(r, 0xFF, 12);
	else if (kind == 3u)			/* a CRC that fails */
		ff_put32(r, ref_crc32(0, g_x, N_IMAGE) ^ 0x10u);
}

/*
 * Every selector kind against every pair of records. The running slot is
 * always established, it is the slot the selector names once the first
 * stage has made its write (verified reads 0x01 then), and the next start
 * boots that same slot: a write that keeps it keeps what the unit starts.
 */
static void test_running(void)
{
	static const uint8_t index[] = { 0x00, 0x01, 0x02, 0xFF };
	static const uint8_t verified[] = { 0x01, 0x00, 0xFE, 0xFF };

	for (unsigned i = 0; i < N(index); i++)
		for (unsigned v = 0; v < N(verified); v++)
			for (unsigned r = 0; r < 16u; r++) {
				struct fr_running run, again;
				unsigned k, k2;

				same(0);
				set_record(0, r & 3u);
				set_record(1, r >> 2);
				ff_set_selector(&g_f, index[i], verified[v]);
				k = start(true, &run, NULL);
				CHECK(run.established);
				CHECK_EQ(g_f.mem[FR_SELECTOR_OFF + 1u], 0x01);
				CHECK_EQ(run.slot, fr_selected_slot(g_f.mem[FR_SELECTOR_OFF]));
				/* with the selector verified on the way in, that is
				 * the slot the ROM read as well */
				if (verified[v] == 0x01u)
					CHECK_EQ(run.slot, k);
				k2 = start(true, &again, NULL);
				CHECK_EQ(k2, run.slot);
				CHECK(again.established && again.slot == run.slot);
			}

	/* The selector as a first stage that wrote nothing left it (a write
	 * that did not take): not verified. The slot is the selected one
	 * when it is good, else the other, where the next start settles. */
	for (unsigned s = 0; s < 2u; s++)
		for (unsigned r = 0; r < 4u; r++) {
			unsigned slot = FR_NO_SLOT;
			bool est = false, both = false;

			same(s);
			set_record(s, r);
			ff_set_selector(&g_f, (uint8_t)s, 0x00);
			load_slot(s);
			CHECK_EQ(fr_running_slots(flash(), &g_mem, &slot, &est, &both), FR_FLASH_OK);
			CHECK(est && both);
			CHECK_EQ(slot, r == 0u ? s : 1u - s);
			CHECK_EQ(g_f.nmut, 0);
		}
}

/* --- Recovery's plans and writes --- */

/* The flash starts, now and on the next reset, and recovery opened in
 * either of them, or after one more restart, writes Y to the slot it does
 * not keep; the unit then starts Y. The flash is left as it was. */
static void starts_and_repairs(void)
{
	struct fr_home home = g_home;
	bool repaired = false;

	memcpy(&g_keep, &g_f, sizeof g_f);
	for (unsigned n = 0; n < 3u && !repaired; n++) {
		struct fr_running run;
		int rc;

		(void)start(n != 1u, &run, NULL);
		CHECK(run.established);
		memset(&g_home, 0, sizeof g_home);
		rc = write_img(&run, g_y, g_ysha, 0);
		if (rc == FR_WR_OK) {
			struct fr_running after;
			const unsigned t = g_w.target;
			const unsigned k = start(true, &after, NULL);

			CHECK_EQ(t, 1u - run.slot);
			CHECK_EQ(k, t);
			CHECK(memcmp(g_f.mem + FR_SLOT_BASE(k), g_y, N_IMAGE) == 0);
			CHECK(ff_slot_good(&g_f, k));
			CHECK(after.established && after.slot == k);
			repaired = true;
		} else {
			/* only the refusals a restart lifts, and nothing written */
			CHECK(rc == FR_WR_NOT_SELECTED || rc == FR_WR_UNVERIFIED);
			CHECK_EQ(g_f.nmut, 0);
		}
	}
	CHECK(repaired);
	memcpy(&g_f, &g_keep, sizeof g_f);
	g_home = home;
}

static void test_plans(void)
{
	for (unsigned a = 0; a < 2u; a++) {
		const unsigned b = 1u - a;
		struct fr_running run;
		struct fr_plan plan;
		struct fr_desc front;

		/* the plain plan: the other slot, a trial allowed */
		same(a);
		CHECK_EQ(start(false, &run, NULL), a);
		CHECK(run.established && run.slot == a);
		CHECK_EQ(fr_writer_plan(flash(), &run, &g_home, 0, &plan, &front), FR_WR_OK);
		CHECK(plan.way == FR_WAY_PLAIN && plan.target == b && plan.keep == a);
		CHECK(plan.own_good && plan.trial_ok);
		CHECK_EQ(fr_writer_plan(flash(), &run, &g_home, FR_BEGIN_F_RUNNING, &plan, &front),
			 FR_WR_OK);
		CHECK(plan.way == FR_WAY_SELECT_FIRST && plan.target == a && plan.keep == b);
		CHECK_EQ(g_f.nmut, 0);
		starts_and_repairs();

		/* another image to the other slot */
		CHECK_EQ(write_img(&run, g_y, g_ysha, 0), FR_WR_OK);
		CHECK(g_w.target == b && sel_is((uint8_t)b, 0x00));
		CHECK(memcmp(g_f.mem + FR_SLOT_BASE(a), g_x, N_IMAGE) == 0);
		CHECK_EQ(start(true, &run, NULL), b);
		CHECK(run.established && run.slot == b);
		/* and the kept slot's own image back over it: the same again */
		memset(&g_home, 0, sizeof g_home);
		CHECK_EQ(start(false, &run, NULL), b);
		CHECK_EQ(write_img(&run, g_y, g_ysha, 0), FR_WR_OK);
		CHECK(g_w.target == a && sel_is((uint8_t)a, 0x00));
		CHECK(memcmp(g_f.mem + FR_SLOT_BASE(0), g_f.mem + FR_SLOT_BASE(1), N_IMAGE) == 0);
		CHECK(ff_slot_good(&g_f, 0) && ff_slot_good(&g_f, 1));
		CHECK_EQ(start(true, &run, NULL), a);
		CHECK(run.established && run.slot == a);

		/* the image both hold, to the other slot and to the running one */
		same(a);
		(void)start(false, &run, NULL);
		CHECK_EQ(write_img(&run, g_x, g_xsha, 0), FR_WR_OK);
		CHECK(g_w.target == b && sel_is((uint8_t)b, 0x00));
		CHECK_EQ(start(true, &run, NULL), b);
		CHECK(run.established && run.slot == b);
		same(a);
		(void)start(false, &run, NULL);
		CHECK_EQ(write_img(&run, g_x, g_xsha, FR_BEGIN_F_RUNNING), FR_WR_OK);
		CHECK(g_w.way == FR_WAY_SELECT_FIRST && g_w.target == a);
		CHECK(sel_is((uint8_t)a, 0x00));
		CHECK(ff_slot_good(&g_f, 0) && ff_slot_good(&g_f, 1));
		CHECK_EQ(start(true, &run, NULL), a);
		starts_and_repairs();
	}
}

/* --- The actions, and a trial of the image the kept slot holds --- */

static void test_actions(void)
{
	for (unsigned a = 0; a < 2u; a++) {
		const unsigned b = 1u - a;
		struct fr_running run;
		struct fr_act_plan plan;
		struct fr_slot_info ib;
		unsigned trial;

		/* try: B marked tried with a pending entry, the selector written
		 * to start it once */
		same(a);
		(void)start(false, &run, NULL);
		CHECK_EQ(act(&run, FR_ACT_TRY, false, &plan), FR_WR_OK);
		CHECK(plan.target == b && plan.tier == FR_TIER_SAME);
		ib = info(b);
		CHECK(ib.tried && !ib.good && ib.log0 == FR_LOG0_PENDING);
		CHECK(ff_slot_good(&g_f, a));
		/* the trial start: the first stage boots B once and names A
		 * again; the front marks the trial started and logs no start */
		CHECK_EQ(start(true, &run, &trial), b);
		CHECK(sel_is((uint8_t)a, 0x01));
		CHECK_EQ(trial, FR_LOG_TRIAL_MARKED);
		CHECK(run.established && run.slot == a);
		ib = info(b);
		CHECK(ib.tried && ib.started && !ib.spent && ib.log0 == FR_LOG0_MARKED);
		CHECK_EQ(g_f.mem[FR_LOG_OFF(a)], a == 0u ? 0xEAu : 0xFAu);
		starts_and_repairs();
		/* every later start runs A, and marks nothing more */
		CHECK_EQ(start(true, &run, &trial), a);
		CHECK_EQ(trial, FR_LOG_TRIAL_NONE);
		CHECK(run.established && run.slot == a);
		/* keep: B's record made right, B selected */
		memset(&g_home, 0, sizeof g_home);
		CHECK_EQ(act(&run, FR_ACT_KEEP, false, &plan), FR_WR_OK);
		CHECK(plan.target == b && sel_is((uint8_t)b, 0x00));
		CHECK(ff_slot_good(&g_f, 0) && ff_slot_good(&g_f, 1));
		CHECK_EQ(start(true, &run, NULL), b);
		CHECK(run.established && run.slot == b && sel_is((uint8_t)b, 0x01));
		starts_and_repairs();

		/* the trial start held into recovery: no log step, so no mark.
		 * A is the running slot there; a keep waits for a started
		 * trial, a put back makes B good again */
		same(a);
		(void)start(false, &run, NULL);
		CHECK_EQ(act(&run, FR_ACT_TRY, false, NULL), FR_WR_OK);
		memset(&g_home, 0, sizeof g_home);
		CHECK_EQ(start(false, &run, NULL), b);
		CHECK(run.established && run.slot == a);
		CHECK_EQ(act(&run, FR_ACT_KEEP, false, NULL), FR_WR_NOT_STARTED);
		CHECK_EQ(g_f.nmut, 0);
		CHECK(info(b).log0 == FR_LOG0_PENDING);
		/* the next start, of A: the entry is still pending, both slots
		 * hold this image, and the mark is made then */
		CHECK_EQ(start(true, &run, &trial), a);
		CHECK_EQ(trial, FR_LOG_TRIAL_MARKED);
		CHECK(info(b).started);
		CHECK_EQ(act(&run, FR_ACT_PUT_BACK, false, &plan), FR_WR_OK);
		CHECK(plan.target == b && sel_is((uint8_t)a, 0x01));
		CHECK(ff_slot_good(&g_f, 0) && ff_slot_good(&g_f, 1));
		starts_and_repairs();

		/* put back before the trial start: the selector settled back */
		same(a);
		(void)start(false, &run, NULL);
		CHECK_EQ(act(&run, FR_ACT_TRY, false, NULL), FR_WR_OK);
		CHECK_EQ(act(&run, FR_ACT_PUT_BACK, true, NULL), FR_WR_OK);
		CHECK(sel_is((uint8_t)a, 0x01));
		CHECK(ff_slot_good(&g_f, 0) && ff_slot_good(&g_f, 1));
		CHECK_EQ(start(true, &run, &trial), a);
		CHECK_EQ(trial, FR_LOG_TRIAL_NONE);
		CHECK(run.established && run.slot == a);

		/* switch: the selector alone */
		same(a);
		(void)start(false, &run, NULL);
		CHECK_EQ(act(&run, FR_ACT_SWITCH, false, &plan), FR_WR_OK);
		CHECK(plan.target == b && plan.tier == FR_TIER_SAME && sel_is((uint8_t)b, 0x00));
		CHECK_EQ(start(true, &run, NULL), b);
		CHECK(run.established && run.slot == b && sel_is((uint8_t)b, 0x01));
		starts_and_repairs();
	}
}

/* A tried slot that matches the loaded memory whole and still differs in
 * the flash, in bytes the front does not read from memory (its own L1
 * code): not the same image. A start of the other slot spends its trial,
 * as with any two images, and its front is never taken as started. */
static void test_not_the_same(void)
{
	for (unsigned a = 0; a < 2u; a++) {
		const unsigned b = 1u - a;
		struct fr_running run;
		unsigned trial, slot = FR_NO_SLOT;
		bool est = false, both = false, eq = true;

		same(a);
		CHECK_EQ(fr_slots_same(flash(), &eq), FR_FLASH_OK);
		CHECK(eq);
		g_f.mem[FR_SLOT_BASE(b) + 16u + 16u + 5u] ^= 0x40u;	/* a byte of the L1 code block */
		ff_put32(g_f.mem + FR_RECORD_OFF(b),
			 ~ref_crc32(0, g_f.mem + FR_SLOT_BASE(b), N_IMAGE));
		memset(g_f.mem + FR_LOG_OFF(b), 0xFF, FR_SECTOR);
		g_f.mem[FR_LOG_OFF(b)] = FR_LOG0_PENDING;
		CHECK_EQ(fr_slots_same(flash(), &eq), FR_FLASH_OK);
		CHECK(!eq);
		load_slot(a);
		CHECK_EQ(fr_running_slots(flash(), &g_mem, &slot, &est, &both), FR_FLASH_OK);
		CHECK(est && both && slot == a);
		CHECK_EQ(start(true, &run, &trial), a);
		CHECK_EQ(trial, FR_LOG_TRIAL_SPENT);
		CHECK(info(b).spent && !info(b).started);
		CHECK_EQ(act(&run, FR_ACT_KEEP, false, NULL), FR_WR_NOT_STARTED);
		CHECK_EQ(g_f.nmut, 0);

		/* a record of another length, or one that is not plausible: not
		 * the same; a read that fails: the error */
		same(a);
		ff_put32(g_f.mem + FR_RECORD_OFF(b) + 4u, N_IMAGE - 1u);
		CHECK_EQ(fr_slots_same(flash(), &eq), FR_FLASH_OK);
		CHECK(!eq);
		same(a);
		memset(g_f.mem + FR_RECORD_OFF(a), 0xFF, 12);
		CHECK_EQ(fr_slots_same(flash(), &eq), FR_FLASH_OK);
		CHECK(!eq);
		same(a);
		g_f.fail_regions = b == 0u ? FF_R_SLOT0 : FF_R_SLOT1;
		CHECK_EQ(fr_slots_same(flash(), &eq), FR_FLASH_IO);
		CHECK(!eq);
		g_f.fail_regions = 0;
	}
}

/* --- Power cuts --- */

enum { K_WRITE_SAME, K_WRITE_OWN, K_WRITE_OTHER, K_TRY, K_SWITCH, K_KEEP, K_PUT_BACK,
       K_PUT_BACK_EARLY, K_KINDS };

static const char *const kind_name[K_KINDS] = {
	"a write of the same image", "a write of the running slot with the same image",
	"a write of another image", "try", "switch", "keep", "put back",
	"put back before the trial start",
};

static struct fr_running g_run;

/* The state a kind starts from, in g_f and g_run. */
static void prepare(unsigned kind, unsigned a)
{
	unsigned trial;

	same(a);
	(void)start(false, &g_run, NULL);
	if (kind == K_KEEP || kind == K_PUT_BACK || kind == K_PUT_BACK_EARLY) {
		CHECK_EQ(act(&g_run, FR_ACT_TRY, false, NULL), FR_WR_OK);
		if (kind != K_PUT_BACK_EARLY) {
			CHECK_EQ(start(true, &g_run, &trial), 1u - a);
			CHECK_EQ(trial, FR_LOG_TRIAL_MARKED);
			(void)start(false, &g_run, NULL);
		}
	}
	memset(&g_home, 0, sizeof g_home);
	CHECK(g_run.established && g_run.slot == a);
	ff_reset(&g_f);
}

static int run_kind(unsigned kind)
{
	switch (kind) {
	case K_WRITE_SAME:
		return write_img(&g_run, g_x, g_xsha, 0);
	case K_WRITE_OWN:
		return write_img(&g_run, g_x, g_xsha, FR_BEGIN_F_RUNNING);
	case K_WRITE_OTHER:
		return write_img(&g_run, g_y, g_ysha, 0);
	case K_TRY:
		return act(&g_run, FR_ACT_TRY, false, NULL);
	case K_SWITCH:
		return act(&g_run, FR_ACT_SWITCH, false, NULL);
	case K_KEEP:
		return act(&g_run, FR_ACT_KEEP, false, NULL);
	case K_PUT_BACK:
		return act(&g_run, FR_ACT_PUT_BACK, false, NULL);
	default:
		return act(&g_run, FR_ACT_PUT_BACK, true, NULL);
	}
}

/* After a cut: three starts, each of a whole stream with its running slot
 * established, a trial start among them where the cut left one; then the
 * repair. */
static void after_cut(void)
{
	struct fr_running run;

	g_f.dead = false;
	memcpy(&g_keep, &g_f, sizeof g_f);
	for (unsigned n = 0; n < 3u; n++) {
		(void)start(true, &run, NULL);
		CHECK(run.established);
		CHECK_EQ(g_f.mem[FR_SELECTOR_OFF + 1u], 0x01);
		CHECK_EQ(run.slot, fr_selected_slot(g_f.mem[FR_SELECTOR_OFF]));
	}
	memcpy(&g_f, &g_keep, sizeof g_f);
	memset(&g_home, 0, sizeof g_home);
	starts_and_repairs();
}

static void test_cuts(void)
{
	static const enum ff_tear tears[] = { FF_TEAR_ZEROS, FF_TEAR_ONES, FF_TEAR_SEED,
					      FF_TEAR_SEED, FF_TEAR_SEED };

	for (unsigned kind = 0; kind < K_KINDS; kind++)
		for (unsigned a = 0; a < 2u; a++) {
			unsigned total, cuts = 0;

			prepare(kind, a);
			memcpy(&g_base, &g_f, sizeof g_f);
			CHECK_EQ(run_kind(kind), FR_WR_OK);
			total = g_f.nmut;
			CHECK(total != 0u);
			for (unsigned k = 1; k <= total; k++) {
				/* a cut after operation k */
				memcpy(&g_f, &g_base, sizeof g_f);
				memset(&g_home, 0, sizeof g_home);
				g_f.cut_after = k;
				(void)run_kind(kind);
				after_cut();
				cuts++;
				/* operation k torn */
				for (unsigned t = 0; t < N(tears); t++) {
					memcpy(&g_f, &g_base, sizeof g_f);
					memset(&g_home, 0, sizeof g_home);
					g_f.tear = k;
					g_f.tear_kind = tears[t];
					g_f.tear_a = 7u * k + t;
					(void)run_kind(kind);
					after_cut();
					cuts++;
				}
			}
			printf("  %s, running slot %u: %u operations, %u cuts, each followed by "
			       "three starts and a repair\n", kind_name[kind], a, total, cuts);
		}
}

int main(void)
{
	g_xend = build_image(g_x, 1u);
	g_yend = build_image(g_y, 9u);
	sha256(g_xsha, g_x, N_IMAGE);
	sha256(g_ysha, g_y, N_IMAGE);
	CHECK(g_xend == g_yend && memcmp(g_x, g_y, g_xend) != 0);

	test_running();
	test_plans();
	test_actions();
	test_not_the_same();
	test_cuts();
	return finish("test_front_same");
}
