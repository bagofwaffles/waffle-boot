/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's recovery writer (src/front/fr_slots.c) against the RAM NOR
 * fake: S-02.049's order, S-02.042's footprint, S-02.045's extent with the
 * erase of the slot's last sector, the record and selector bytes
 * (S-02.046, S-02.048), the read-back's place (S-02.047); the four cells of
 * S-02.052's table for recovery's two requests (S-02.084): the default
 * write, the write of the running slot with the other slot selected first,
 * and the one in place; a power cut after every erase and program of each
 * kind of write, and a torn version of each, with the boot rule judged
 * after it under the factory first stage and a conforming one
 * (s02_first_stage()), further cuts included (S-02.051, S-02.052); every
 * refusal with no erase or program and the retry state unchanged
 * (S-02.044, S-02.090, S-02.091, S-01.002, S-02.084); faults that stop the
 * write before the selector (S-02.047); the selector write before a write
 * of the running slot failing; the retries after a failed write of the
 * running slot.
 */
#include <string.h>

#include "s02_check.h"
#include "s02_fake_flash.h"
#include "vectors/s02_vectors.h"

#include "bearssl_hash.h"
#include "front/fr.h"
#include "front/fr_handoff.h"
#include "front/fr_proto.h"
#include "front/fr_slots.h"
#include "front/fr_startlog.h"
#include "waffle/s05_handoff.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static struct fake_flash g_f;
static struct fake_flash g_start, g_cut;
static struct fr_writer g_w;
static struct fr_home g_home;
static uint8_t g_flags;		/* BEGIN's flags for write_all() */
static uint8_t g_img[FR_SLOT_CAP + 1u];
static uint8_t g_sha[32];

/* ---- the flash ops: the fake, with a few faults of the selector's own ---- */

static unsigned g_sel_mode;	/* 1 ignore its program, 2 store its second byte
				 * flipped, 3 fail the reads of it after */
static unsigned g_sel_nth;	/* the selector program it applies to, 0 every one */
static unsigned g_nsel;		/* selector programs seen */
static uint8_t g_selprog[8][2];	/* what each one programmed */
static unsigned g_erase_then_fail;	/* this erase (by nmut) is made, then fails */
/* every selector program: the log index of its op and the two bytes it
 * stands for ({index, verified}: a 1-byte index program and the verified
 * byte after it are joined) */
static uint8_t g_selb[16][2];
static unsigned g_selb_at[16], g_nselb;
static bool g_split;		/* the running slot damaged: split programs */
static unsigned g_idx_mode;	/* the 1-byte index program: 1 stores nothing,
				 * 2 fails its read-back, 3 fails */
static unsigned g_ver_mode;	/* the 1-byte verified program: 1 stores
				 * nothing, 2 fails */

/* A selector operation that fails (by mutation number, 0 none), leaving
 * the selector at g_fail_val (index | verified << 8), once. */
static unsigned g_fail_mut, g_fail_val;
/* what each program wrote, by mutation number: its first two bytes */
#define PV_MAX 512u
static unsigned g_pv[PV_MAX];

static bool inject(void *ctx, char kind, uint32_t off, uint32_t len)
{
	if (g_fail_mut == 0u || g_f.nmut + 1u != g_fail_mut)
		return false;
	ff_log(ctx, kind, off, len);
	g_f.nmut++;
	g_f.nall++;
	g_f.mem[FR_SELECTOR_OFF] = (uint8_t)g_fail_val;
	g_f.mem[FR_SELECTOR_OFF + 1u] = (uint8_t)(g_fail_val >> 8);
	g_fail_mut = 0;
	return true;
}

static bool sel_applies(void)
{
	return g_sel_mode != 0u && g_nsel != 0u && (g_sel_nth == 0u || g_nsel == g_sel_nth);
}

static int w_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	if (g_sel_mode == 3u && off == FR_SELECTOR_OFF && sel_applies())
		return -1;
	if (g_idx_mode == 2u && off == FR_SELECTOR_OFF && len == 1u)
		return -1;
	return ff_read(ctx, off, buf, len);
}

static int w_erase(void *ctx, uint32_t off)
{
	int rc;

	if (inject(ctx, FF_ERASE, off, FR_SECTOR))
		return -1;
	rc = ff_erase(ctx, off);

	if (rc == 0 && g_erase_then_fail != 0u && g_f.nmut == g_erase_then_fail)
		return -1;
	return rc;
}

static int w_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	uint8_t v[2];

	if (g_f.nmut + 1u < PV_MAX)
		g_pv[g_f.nmut + 1u] = buf[0] | (len > 1u ? (unsigned)buf[1] << 8 : 0xFF00u);
	if (inject(ctx, FF_PROGRAM, off, len))
		return -1;
	if (off == FR_SELECTOR_OFF && len == 1u) {
		if (g_nselb < N(g_selb)) {
			g_selb_at[g_nselb] = g_f.nlog;
			g_selb[g_nselb][0] = buf[0];
			g_selb[g_nselb][1] = 0xEEu;
			g_nselb++;
		}
		if (g_idx_mode == 1u || g_idx_mode == 3u) {
			ff_log(ctx, FF_PROGRAM, off, len);
			return g_idx_mode == 1u ? 0 : -1;	/* stores nothing */
		}
		return ff_program(ctx, off, buf, len);
	}
	if (off == FR_SELECTOR_OFF + 1u && len == 1u) {
		if (g_nselb > 0u)
			g_selb[g_nselb - 1u][1] = buf[0];
		if (g_ver_mode != 0u) {
			ff_log(ctx, FF_PROGRAM, off, len);
			return g_ver_mode == 1u ? 0 : -1;	/* stores nothing */
		}
		return ff_program(ctx, off, buf, len);
	}
	if (off != FR_SELECTOR_OFF || len != 2u)
		return ff_program(ctx, off, buf, len);
	if (g_nselb < N(g_selb)) {
		g_selb_at[g_nselb] = g_f.nlog;
		memcpy(g_selb[g_nselb], buf, 2);
		g_nselb++;
	}
	if (g_nsel < N(g_selprog))
		memcpy(g_selprog[g_nsel], buf, 2);
	g_nsel++;
	if (!sel_applies() || g_sel_mode == 3u)
		return ff_program(ctx, off, buf, len);
	if (g_sel_mode == 1u) {
		ff_log(ctx, FF_PROGRAM, off, len);
		return 0;		/* reports success, stores nothing */
	}
	v[0] = buf[0];
	v[1] = (uint8_t)(buf[1] ^ 0x01u);
	return ff_program(ctx, off, v, 2u);
}

static const struct fr_flash *flash(void)
{
	static struct fr_flash ops;

	ops.read = w_read;
	ops.erase = w_erase;
	ops.program = w_program;
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
	if (!(flags & S05_FLAG_FILL) && payload != NULL) {
		memcpy(s + o, payload, count);
		o += count;
	}
	return o;
}

/* The front's bytes and pin come from g_fseed, the rest from seed. */
#define FRONT_SEED 500u
static uint32_t g_fseed = FRONT_SEED;
static bool g_pin_flip;		/* the pin's first byte changed, nothing else */

/* Offsets in a built image: a byte of the front's L1 code block, and one
 * of the main image's payload, outside every front window. */
#define FRONT_BYTE 37u
#define MAIN_BYTE  1600u

/* n bytes into dst: the stream, then seeded bytes after its FINAL block;
 * the stream's length. */
static uint32_t build_image(uint8_t *dst, uint32_t n, uint32_t seed)
{
	uint8_t l1c[256], l1d[64], l2[1024], ml2[512], desc[FR_DESC_MAX];
	uint32_t off = 0, size = FR_DESC_HEAD + 8u + 4u, fcrc;

	ff_fill_image(dst, n, seed);
	ff_fill_image(l1c, sizeof l1c, g_fseed + 1u);
	ff_fill_image(l1d, sizeof l1d, g_fseed + 2u);
	ff_fill_image(l2, sizeof l2, g_fseed + 3u);
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
	ff_fill_image(desc + FR_DESC_O_FRONT_PIN, FR_SHA256_LEN, g_fseed + 5u);
	desc[FR_DESC_O_FRONT_PIN] ^= g_pin_flip ? 0x80u : 0x00u;
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
	CHECK(FRONT_BYTE >= 32u && FRONT_BYTE < 32u + 256u);	/* l1c's payload */
	CHECK(MAIN_BYTE >= off - 16u - 512u && MAIN_BYTE < off - 16u);	/* ml2's */
	/* S-02.084: at most 0x77000 bytes */
	CHECK_EQ(fr_image_valid(dst, n, NULL), n <= 0x77000u ? FR_IMG_OK : FR_IMG_SIZE);
	return off;
}

static uint32_t make_image(uint32_t n, uint32_t seed)
{
	uint32_t e = build_image(g_img, n, seed);

	sha256(g_sha, g_img, n);
	return e;
}

/* Slot k holds img[0..n) with its record (S-02.016). */
static void put_slot(unsigned k, const uint8_t *img, uint32_t n)
{
	memset(g_f.mem + FR_SLOT_BASE(k), 0xFF, FR_SLOT_CAP);
	memcpy(g_f.mem + FR_SLOT_BASE(k), img, n);
	memset(g_f.mem + FR_RECORD_OFF(k), 0xFF, FR_SECTOR);
	ff_put32(g_f.mem + FR_RECORD_OFF(k), ref_crc32(0, img, n));
	ff_put32(g_f.mem + FR_RECORD_OFF(k) + 4u, n);
	ff_put32(g_f.mem + FR_RECORD_OFF(k) + 8u, FR_TAG);
}

/* A start log in slot k's last sector (S-02.095): three entries used. */
static void put_log(unsigned k)
{
	g_f.mem[FR_SLOT_BASE(k) + FR_SLOT_CAP - FR_SECTOR] = 0xEAu;
}

/* --- Starts --- */

static const uint8_t starts[][2] = {{0x00, 0x01}, {0x01, 0x01}, {0x02, 0x01}, {0xFF, 0x01}};

#define N_IMAGE 0x2345u
#define CUR_LEN 0x1345u
#define OLD_TARGET_LEN 0x3456u	/* a longer predecessor in the target */
/* A write of N_IMAGE bytes: 3 sector erases, 36 page programs, the last
 * sector's erase, the record's erase and program. */
#define SLOT_OPS (3u + 36u + 1u + 2u)

static void snapshot(void)
{
	memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
	ff_reset(&g_f);
}

/* Both slots good, the selected one holding an image with the front of
 * FRONT_SEED (S-02.084), each with a start log in its last sector, the
 * first-stage area and metadata a known pattern. Nothing done yet in this
 * recovery: home cleared, no faults. */
static void setup(uint8_t index, uint8_t verified)
{
	static uint8_t cur[CUR_LEN];
	unsigned sel = fr_selected_slot(index);
	uint32_t fseed = g_fseed;
	bool flip = g_pin_flip;

	ff_init(&g_f);
	ff_fill_image(g_f.mem, FR_SELECTOR_OFF, 55u);
	g_fseed = FRONT_SEED;
	g_pin_flip = false;
	(void)build_image(cur, sizeof cur, 1u);
	g_fseed = fseed;
	g_pin_flip = flip;
	put_slot(sel, cur, sizeof cur);
	ff_set_slot(&g_f, 1u - sel, "good", OLD_TARGET_LEN, 7u);
	put_log(0);
	put_log(1);
	ff_set_selector(&g_f, index, verified);
	memset(&g_home, 0, sizeof g_home);
	g_flags = 0;
	g_sel_mode = g_sel_nth = g_nsel = 0;
	g_erase_then_fail = 0;
	g_nselb = 0;
	g_idx_mode = 0;
	g_ver_mode = 0;
	g_split = false;
	snapshot();
}

static struct fr_running running_of(uint8_t index)
{
	struct fr_running r;

	r.established = true;
	r.slot = (uint8_t)fr_selected_slot(index);
	return r;
}

/* Begin, then step until done, checking each step's size: at most one
 * sector erase and its programs, one selector operation, or 4 KiB of
 * read-back. */
static int write_all(uint32_t n, const uint8_t *sha, const struct fr_running *run)
{
	int rc;
	unsigned steps = 0, last = 0;

	g_nsel = 0;
	g_nselb = 0;
	rc = fr_writer_begin(&g_w, flash(), run, &g_home, g_img, n, sha, g_flags);
	if (rc != FR_WR_OK) {
		CHECK(fr_writer_step(&g_w));
		CHECK_EQ(g_w.result, rc);
		return rc;
	}
	for (;;) {
		unsigned from = g_f.nlog, erases = 0, progs = 0, i;
		uint32_t rbytes = 0, sector = 0;
		bool done = fr_writer_step(&g_w);
		unsigned p = fr_writer_progress(&g_w);

		for (i = from; i < g_f.nlog && i < FF_LOG_MAX; i++) {
			const struct ff_op *o = &g_f.log[i];

			if (o->kind == FF_ERASE) {
				erases++;
				sector = o->off;
			} else if (o->kind == FF_PROGRAM) {
				progs++;
				if (erases != 0u)
					CHECK(o->off >= sector && o->off < sector + FR_SECTOR);
			} else if (o->off != FR_SELECTOR_OFF || o->len > 2u) {
				rbytes += o->len;	/* the settling's selector read aside */
			}
		}
		CHECK(erases <= 1u);
		CHECK(progs <= FR_SECTOR / FR_PAGE);
		CHECK(erases == 0u || rbytes == 0u);
		CHECK(rbytes <= FR_SECTOR + 12u + 1u);	/* and a trial's log byte */
		if (!done || g_w.result == FR_WR_OK)
			CHECK(p >= last);	/* a failed write shows 0 */
		last = p;
		if (done)
			break;
		CHECK(++steps < 2000u);
		if (steps >= 2000u)
			break;
	}
	if (g_w.result == FR_WR_OK)
		CHECK_EQ(fr_writer_progress(&g_w), 1000);
	return g_w.result;
}

/* --- The op log --- */

static unsigned find_op(char kind, uint32_t off, unsigned from)
{
	for (unsigned i = from; i < g_f.nlog; i++)
		if (g_f.log[i].kind == kind && g_f.log[i].off == off)
			return i;
	return ~0u;
}

static bool reads_cover(uint32_t off, uint32_t len, unsigned lo, unsigned hi)
{
	static uint8_t seen[0x100000];
	uint32_t j;

	memset(seen + off, 0, len);
	for (unsigned i = lo + 1u; i < hi && i < g_f.nlog; i++) {
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

/* S-02.042 */
static bool in_footprint(unsigned t, uint32_t off, uint32_t len)
{
	uint32_t base = FR_SLOT_BASE(t), rec = FR_RECORD_OFF(t);

	return (off >= base && off + len <= base + FR_SLOT_CAP) ||
	       (off >= rec && off + len <= rec + FR_SECTOR) ||
	       (off >= FR_SELECTOR_OFF && off + len <= FR_SELECTOR_OFF + FR_SECTOR);
}

static unsigned mutations(void)
{
	return g_f.nmut;
}

/*
 * S-02.045-S-02.049 on a whole write's log, for each way of S-02.084:
 * - plain: class 1, the last sector's erase when the image leaves it free,
 *   the record, the read-back, then the selector erase and its program,
 *   read back;
 * - select first: the selector erase, its program {other, 01} and a read of
 *   it, then all of plain;
 * - in place: plain to the read-back, and no selector erase or program.
 */
/*
 * A selector write in the op log from index i, its erase: S-02.048's one
 * 2-byte program and its 2-byte read-back; or, with the running slot
 * damaged by this recovery (g_split, S-02.084), the index byte at 0xD000,
 * its 1-byte read-back, the verified byte at 0xD001 and the 2-byte
 * read-back. The bytes programmed are idx and ver. The index after it.
 */
static unsigned check_sel_write(unsigned i, uint8_t idx, uint8_t ver)
{
	const struct ff_op *o = g_f.log + i;
	const unsigned n = g_split ? 5u : 3u;

	CHECK(i + n <= g_f.nlog);
	if (i + n > g_f.nlog)
		return g_f.nlog;
	CHECK(o[0].kind == FF_ERASE && o[0].off == FR_SELECTOR_OFF);
	if (g_split) {
		CHECK(o[1].kind == FF_PROGRAM && o[1].off == FR_SELECTOR_OFF && o[1].len == 1u);
		CHECK(o[2].kind == FF_READ && o[2].off == FR_SELECTOR_OFF && o[2].len == 1u);
		CHECK(o[3].kind == FF_PROGRAM && o[3].off == FR_SELECTOR_OFF + 1u && o[3].len == 1u);
		CHECK(o[4].kind == FF_READ && o[4].off == FR_SELECTOR_OFF && o[4].len == 2u);
	} else {
		CHECK(o[1].kind == FF_PROGRAM && o[1].off == FR_SELECTOR_OFF && o[1].len == 2u);
		CHECK(o[2].kind == FF_READ && o[2].off == FR_SELECTOR_OFF && o[2].len == 2u);
	}
	/* the bytes, as the hooks saw them programmed */
	{
		bool found = false;

		for (unsigned k = 0; k < g_nselb; k++)
			if (g_selb_at[k] == i + 1u)
				found = g_selb[k][0] == idx && g_selb[k][1] == ver;
		CHECK(found);
	}
	return i + n;
}

/*
 * S-02.084's settling on slot t with the running slot damaged, from the
 * selector value p (index | verified << 8): 0 nothing, where it names t
 * with verified 01 or names slot 1 as t; 1 programs alone, for slot 0
 * with bit 0 of verified set; 2 an erase and {t, 01}, the index first.
 */
static unsigned settle_kind(unsigned p, unsigned t)
{
	const unsigned n = (p & 0xFFu) == 0u ? 0u : 1u, v = p >> 8;

	if (n == t && (v == 0x01u || t == 1u))
		return 0;
	if (t == 0u && (v & 0x01u) != 0u)
		return 1;
	return 2;
}

/* The settling's operations in the op log from i, its first erase or
 * program of the selector: the programs alone (the index to 00 when it is
 * not, its 1-byte read-back, verified to 01 when it is not, the 2-byte
 * read), or the erase and the split programs. The index after them. */
static unsigned check_settle(unsigned i, unsigned p, unsigned t)
{
	const struct ff_op *o = g_f.log;

	if (settle_kind(p, t) == 2u)
		return check_sel_write(i, (uint8_t)t, 0x01u);
	if (settle_kind(p, t) == 0u)
		return i;
	if ((p & 0xFFu) != 0u) {
		CHECK(i + 1u < g_f.nlog);
		if (i + 1u >= g_f.nlog)
			return g_f.nlog;
		CHECK(o[i].kind == FF_PROGRAM && o[i].off == FR_SELECTOR_OFF && o[i].len == 1u);
		CHECK(o[i + 1u].kind == FF_READ && o[i + 1u].off == FR_SELECTOR_OFF &&
		      o[i + 1u].len == 1u);
		i += 2u;
	}
	if ((p >> 8) != 0x01u) {
		CHECK(i < g_f.nlog);
		if (i >= g_f.nlog)
			return g_f.nlog;
		CHECK(o[i].kind == FF_PROGRAM && o[i].off == FR_SELECTOR_OFF + 1u && o[i].len == 1u);
		i++;
	}
	CHECK(i < g_f.nlog);
	if (i >= g_f.nlog)
		return g_f.nlog;
	CHECK(o[i].kind == FF_READ && o[i].off == FR_SELECTOR_OFF && o[i].len == 2u);
	return i + 1u;
}

/* The first erase or program of the selector sector from index i, or ~0u. */
static unsigned first_sel_mut(unsigned i)
{
	for (; i < g_f.nlog; i++)
		if (g_f.log[i].kind != FF_READ && g_f.log[i].off >= FR_SELECTOR_OFF &&
		    g_f.log[i].off < FR_SELECTOR_OFF + FR_SECTOR)
			return i;
	return ~0u;
}

static void check_log(unsigned t, uint32_t n, unsigned way)
{
	static uint8_t programmed[FR_SLOT_CAP];
	const uint32_t base = FR_SLOT_BASE(t), rec = FR_RECORD_OFF(t);
	const uint32_t last = base + FR_SLOT_CAP - FR_SECTOR;
	const uint32_t sectors = (n + FR_SECTOR - 1u) / FR_SECTOR;
	const bool log_erase = true;	/* every image leaves the last sector */
	unsigned i, from = 0, rec_erase, rec_prog, sel_erase = ~0u, sel_prog = ~0u, end;
	unsigned erases = 0, last_prog = 0, log_at = ~0u, log_n = 0, kind_end = 0;
	/* the selector before the write, and before its final settling */
	unsigned pre = g_start.mem[FR_SELECTOR_OFF] | (unsigned)g_start.mem[FR_SELECTOR_OFF + 1u] << 8;

	CHECK(g_f.nlog <= FF_LOG_MAX);
	CHECK(!g_f.bad_call);
	if (way == FR_WAY_SELECT_FIRST) {
		/* the first erase or program of all, after the start's reads */
		unsigned s = 0;

		while (s < g_f.nlog && g_f.log[s].kind == FF_READ)
			s++;
		if (g_split) {
			/* S-02.084: the same settling, on B */
			if (settle_kind(pre, 1u - t) != 0u) {
				from = check_settle(s, pre, 1u - t);
				pre = (1u - t) | 0x0100u;
			}
		} else {
			from = check_sel_write(s, (uint8_t)(1u - t), 0x01u);
		}
	}
	rec_erase = find_op(FF_ERASE, rec, from);
	rec_prog = find_op(FF_PROGRAM, rec, from);
	CHECK(rec_erase != ~0u && rec_prog == rec_erase + 1u);
	if (rec_erase == ~0u || rec_prog == ~0u)
		return;
	CHECK_EQ(g_f.log[rec_prog].len, 12);
	if (g_split) {
		/* S-02.084: the write ends by settling the selector on t, after
		 * the read-back */
		const unsigned m = first_sel_mut(rec_prog + 1u);

		kind_end = settle_kind(pre, t);
		if (kind_end == 0u) {
			CHECK_EQ(m, ~0u);
			end = g_f.nlog;
		} else {
			CHECK(m != ~0u);
			end = m == ~0u ? g_f.nlog : m;
			if (m != ~0u)
				CHECK_EQ(check_settle(m, pre, t), g_f.nlog);
		}
		CHECK_EQ(g_nsel, 0);	/* no 2-byte program at all */
	} else if (way == FR_WAY_IN_PLACE) {
		/* S-02.048: no selector write at all; the read-back ends it */
		for (i = 0; i < g_f.nlog; i++)
			CHECK(g_f.log[i].kind == FF_READ || g_f.log[i].off < FR_SELECTOR_OFF ||
			      g_f.log[i].off >= FR_SELECTOR_OFF + FR_SECTOR);
		CHECK_EQ(g_nsel, 0);
		CHECK_EQ(g_f.log[g_f.nlog - 1u].kind, FF_READ);
		end = g_f.nlog;
	} else {
		sel_erase = find_op(FF_ERASE, FR_SELECTOR_OFF, from);
		sel_prog = find_op(FF_PROGRAM, FR_SELECTOR_OFF, from);
		CHECK(rec_prog < sel_erase && sel_erase != ~0u);
		CHECK_EQ(sel_prog, sel_erase + 1u);
		/* class 6 last, then its read-back */
		CHECK_EQ(check_sel_write(sel_erase, (uint8_t)t, 0x00u), g_f.nlog);
		CHECK_EQ(g_nsel, way == FR_WAY_SELECT_FIRST ? 2u : 1u);
		end = sel_erase;
	}
	memset(programmed, 0, sizeof programmed);
	for (i = from; i < g_f.nlog; i++) {
		const struct ff_op *o = &g_f.log[i];

		if (o->kind == FF_READ)
			continue;
		CHECK(in_footprint(t, o->off, o->len));
		if (o->kind == FF_ERASE)
			erases++;
		if (i > rec_prog && i < end)
			CHECK(0);		/* only the read-back between them */
		if (o->off == rec || (o->off >= FR_SELECTOR_OFF && o->off < FR_SELECTOR_OFF + 2u))
			continue;
		CHECK(o->off >= base && o->off < base + FR_SLOT_CAP);
		CHECK(i < rec_erase);		/* class 1 first */
		if (o->kind == FF_ERASE) {
			if (log_erase && o->off == last) {
				log_at = i;		/* the last sector, emptied */
				log_n++;
				continue;
			}
			CHECK(o->off < base + sectors * FR_SECTOR);	/* nothing wholly beyond */
			continue;
		}
		CHECK(o->len >= 1u && o->len <= FR_PAGE);
		CHECK((o->off % FR_PAGE) + o->len <= FR_PAGE);
		/* its sector erased before it, each byte once, none past n */
		CHECK(find_op(FF_ERASE, o->off - (o->off % FR_SECTOR), 0) < i);
		last_prog = i;
		for (uint32_t j = o->off - base; j < o->off - base + o->len; j++) {
			CHECK(j < n);
			if (j < FR_SLOT_CAP) {
				CHECK(programmed[j] == 0);
				programmed[j] = 1;
			}
		}
	}
	for (i = 0; i < n; i++)
		if (!programmed[i])
			break;
	CHECK_EQ(i, n);
	/* S-02.045: the last sector erased after the image's last program and
	 * before the record erase, when the image leaves it free; else no
	 * erase of its own */
	if (log_erase) {
		CHECK_EQ(log_n, 1);
		CHECK(log_at != ~0u && log_at > last_prog && log_at < rec_erase);
	} else {
		CHECK_EQ(log_n, 0);
	}
	CHECK_EQ(erases, sectors + (log_erase ? 1u : 0u) + 1u +
			 (g_split ? (kind_end == 2u ? 1u : 0u) : way == FR_WAY_IN_PLACE ? 0u : 1u));
	/* S-02.047: the read-back strictly between the record program and the
	 * selector erase. */
	CHECK(reads_cover(rec, 12, rec_prog, end));
	CHECK(reads_cover(base, n, rec_prog, end));
}

/* The flash after a whole write of target t, the other slot kept. */
static void check_final(unsigned t, uint32_t n, unsigned way)
{
	const unsigned keep = 1u - t;
	const uint32_t base = FR_SLOT_BASE(t), end = (n + FR_SECTOR - 1u) / FR_SECTOR * FR_SECTOR;
	const uint32_t keep_to = n <= FR_LOG_IMAGE_MAX ? FR_LOG_IMAGE_MAX : FR_SLOT_CAP;
	uint32_t j;
	uint8_t want[32];

	CHECK(memcmp(g_f.mem + base, g_img, n) == 0);
	for (j = n; j < end; j++)
		CHECK_EQ(g_f.mem[base + j], 0xFFu);
	if (end < keep_to)
		CHECK(memcmp(g_f.mem + base + end, g_start.mem + base + end, keep_to - end) == 0);
	/* S-02.045, S-02.095: an empty start log */
	for (j = keep_to; j < FR_SLOT_CAP; j++)
		if (g_f.mem[base + j] != 0xFFu) {
			CHECK(0);
			break;
		}
	CHECK_EQ(fr_le32(g_f.mem + FR_RECORD_OFF(t)), ref_crc32(0, g_img, n));	/* S-02.046 */
	CHECK_EQ(fr_le32(g_f.mem + FR_RECORD_OFF(t) + 4u), n);
	CHECK_EQ(fr_le32(g_f.mem + FR_RECORD_OFF(t) + 8u), 0x0000B004u);
	for (j = 12; j < FR_SECTOR; j++)
		CHECK_EQ(g_f.mem[FR_RECORD_OFF(t) + j], 0xFFu);
	if (way == FR_WAY_IN_PLACE) {
		/* S-02.048: the selector as it was, naming t with verified 01 */
		CHECK(memcmp(g_f.mem + FR_SELECTOR_OFF, g_start.mem + FR_SELECTOR_OFF, FR_SECTOR) == 0);
		CHECK_EQ(fr_selected_slot(g_f.mem[FR_SELECTOR_OFF]), t);
		CHECK_EQ(g_f.mem[FR_SELECTOR_OFF + 1u], 0x01u);
	} else {
		CHECK_EQ(g_f.mem[FR_SELECTOR_OFF], t);			/* S-02.048 */
		/* S-02.084: settled with verified 01 in the damaged state */
		CHECK_EQ(g_f.mem[FR_SELECTOR_OFF + 1u], g_split ? 0x01u : 0x00u);
		for (j = 2; j < FR_SECTOR; j++)
			CHECK_EQ(g_f.mem[FR_SELECTOR_OFF + j], 0xFFu);
	}
	CHECK(memcmp(g_f.mem + FR_SLOT_BASE(keep), g_start.mem + FR_SLOT_BASE(keep), FR_SLOT_CAP) == 0);
	CHECK(memcmp(g_f.mem + FR_RECORD_OFF(keep), g_start.mem + FR_RECORD_OFF(keep), FR_SECTOR) == 0);
	CHECK(memcmp(g_f.mem, g_start.mem, FR_SELECTOR_OFF) == 0);	/* S-02.041 */
	CHECK(ff_slot_good(&g_f, t));
	sha256(want, g_f.mem + base, n);
	CHECK(memcmp(g_w.readback_sha, want, 32) == 0);
	CHECK(memcmp(g_w.readback_sha, g_sha, 32) == 0);
}

/* Nothing outside the footprint changed, the kept slot untouched. */
static void check_untouched(unsigned t)
{
	unsigned keep = 1u - t;

	CHECK(memcmp(g_f.mem, g_start.mem, FR_SELECTOR_OFF) == 0);
	CHECK(memcmp(g_f.mem + FR_SLOT_BASE(keep), g_start.mem + FR_SLOT_BASE(keep), FR_SLOT_CAP) == 0);
	CHECK(memcmp(g_f.mem + FR_RECORD_OFF(keep), g_start.mem + FR_RECORD_OFF(keep), FR_SECTOR) == 0);
}

/* One reset under a profile: the slot it boots (its selector write made). */
static unsigned boot_once(int conforming)
{
	struct s02_flash_ops ops;
	struct s02_env env = ff_env(&g_f, &ops);

	ff_reset(&g_f);
	if (conforming) {
		struct s02_fs_outcome o;

		s02_first_stage(&env, false, &o);
		return o.boot_slot;
	}
	return ff_factory_first_stage(&env);
}

/* `resets` resets under a profile; true when each boots a good slot and
 * leaves the selector naming one. tear_first tears the first reset's own
 * erase or program of the selector. */
static bool run_resets(int conforming, unsigned resets, unsigned tear_first, uint32_t seed)
{
	struct s02_flash_ops ops;
	struct s02_env env = ff_env(&g_f, &ops);
	bool ok = true;

	for (unsigned r = 0; r < resets; r++) {
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
		if (!ff_slot_good(&g_f, b))
			ok = false;
		if (!ff_slot_good(&g_f, fr_selected_slot(g_f.mem[FR_SELECTOR_OFF])))
			ok = false;
	}
	return ok;
}

/* --- A whole write, every start --- */

static unsigned g_total_ops[4];

static void test_write_ok(void)
{
	unsigned i;

	for (i = 0; i < N(starts); i++) {
		struct fr_running run = running_of(starts[i][0]);
		unsigned t = starts[i][0] == 0x00u ? 1u : 0u;	/* S-02.043 */

		setup(starts[i][0], starts[i][1]);
		make_image(N_IMAGE, 40u + i);
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
		CHECK_EQ(g_w.target, t);
		CHECK_EQ(g_w.way, FR_WAY_PLAIN);
		check_log(t, N_IMAGE, FR_WAY_PLAIN);
		check_final(t, N_IMAGE, FR_WAY_PLAIN);
		g_total_ops[i] = mutations();
		CHECK_EQ(mutations(), SLOT_OPS + 2u);
		/* the default write leaves the running slot as it is: no retry
		 * state */
		CHECK(!g_home.moved && !g_home.spoiled);
		memcpy(g_cut.mem, g_f.mem, sizeof g_f.mem);
		/* a second write in the same boot: the selector names the new
		 * slot, with verified 0x00 */
		ff_reset(&g_f);
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_NOT_SELECTED);
		run.slot = (uint8_t)t;
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_UNVERIFIED);
		CHECK_EQ(g_f.nmut, 0);
		/* the next reset boots the new slot under either profile */
		CHECK(run_resets(1, 1, 0, 0));
		CHECK_EQ(g_f.mem[FR_SELECTOR_OFF], t);
		CHECK_EQ(g_f.mem[FR_SELECTOR_OFF + 1u], 0x01u);
		memcpy(g_f.mem, g_cut.mem, sizeof g_f.mem);
		ff_reset(&g_f);
		{
			struct s02_flash_ops ops;
			struct s02_env env = ff_env(&g_f, &ops);

			CHECK_EQ(ff_factory_first_stage(&env), t);
		}
	}

	/* The largest image, 0x77000 bytes: 119 sectors, the last one left
	 * to the start log and erased (S-02.084, S-02.045). */
	{
		struct fr_running run = running_of(0x01);

		setup(0x01, 0x01);
		make_image(0x77000u, 99u);
		CHECK_EQ(write_all(0x77000u, g_sha, &run), FR_WR_OK);
		check_log(0, 0x77000u, FR_WAY_PLAIN);
		check_final(0, 0x77000u, FR_WAY_PLAIN);
	}
	/* A one-sector image shorter than a page at its end. */
	{
		struct fr_running run = running_of(0x00);

		setup(0x00, 0x01);
		make_image(0x1F81u, 5u);
		CHECK_EQ(write_all(0x1F81u, g_sha, &run), FR_WR_OK);
		check_log(1, 0x1F81u, FR_WAY_PLAIN);
		check_final(1, 0x1F81u, FR_WAY_PLAIN);
	}
}

static void refused(int want, struct fr_running run, uint32_t n, const uint8_t *sha);

/* --- S-02.045: the last sector, about 0x77000 --- */

static void test_extent(void)
{
	static const uint32_t sizes[] = { 0x76FFFu, 0x77000u, 0x77001u, 0x77100u, 0x78000u };

	for (unsigned i = 0; i < N(sizes); i++)
		for (unsigned way = 0; way < 2u; way++) {
			struct fr_running run = running_of(0x00);

			setup(0x00, 0x01);
			g_flags = way ? FR_BEGIN_F_RUNNING : 0u;
			make_image(sizes[i], 70u + i);
			if (sizes[i] > 0x77000u) {
				/* S-02.084: refused as a size, nothing written */
				refused(FR_WR_SIZE, run, sizes[i], g_sha);
				CHECK_EQ(g_w.image_rc, FR_IMG_SIZE);
				continue;
			}
			CHECK_EQ(write_all(sizes[i], g_sha, &run), FR_WR_OK);
			check_log(way ? 0u : 1u, sizes[i], way ? FR_WAY_SELECT_FIRST : FR_WAY_PLAIN);
			check_final(way ? 0u : 1u, sizes[i], way ? FR_WAY_SELECT_FIRST : FR_WAY_PLAIN);
		}
}

/* --- Power cuts (S-02.051, S-02.052) --- */

static void after_cut(unsigned t, unsigned k, unsigned m_total, uint32_t seed)
{
	unsigned sel_erase = find_op(FF_ERASE, FR_SELECTOR_OFF, 0);
	unsigned prof, m;

	CHECK(g_f.dead || k == m_total);
	CHECK(!g_f.bad_call);
	check_untouched(t);
	/* no selector operation without the read-back before it */
	if (sel_erase != ~0u)
		CHECK(reads_cover(FR_SLOT_BASE(t), N_IMAGE, find_op(FF_PROGRAM, FR_RECORD_OFF(t), 0),
				  sel_erase));
	/* the boot rule: the slot the selector selects is good */
	CHECK(ff_slot_good(&g_f, fr_selected_slot(g_f.mem[FR_SELECTOR_OFF])));
	memcpy(g_cut.mem, g_f.mem, sizeof g_f.mem);
	for (prof = 0; prof < 2u; prof++) {
		memcpy(g_f.mem, g_cut.mem, sizeof g_f.mem);
		CHECK(run_resets((int)prof, 3u, 0u, 0u));
		/* further cuts: the first reset's own selector erase or program torn */
		for (m = 1; m <= 2u; m++) {
			memcpy(g_f.mem, g_cut.mem, sizeof g_f.mem);
			CHECK(run_resets((int)prof, 1u, m, seed * 7u + m));
			CHECK(run_resets((int)prof, 3u, 0u, 0u));
		}
	}
}

static void tear(unsigned k, unsigned v)
{
	g_f.tear = k;
	if (v < 3u) {
		g_f.tear_kind = FF_TEAR_SEED;
		g_f.tear_a = 1000u * k + v;
	} else {
		g_f.tear_kind = v == 3u ? FF_TEAR_ZEROS : FF_TEAR_ONES;
		g_f.tear_a = 0;
		g_f.tear_b = FR_SECTOR;
	}
}

static void test_power_cuts(void)
{
	unsigned i, k, v, cuts = 0;

	for (i = 0; i < N(starts); i++) {
		struct fr_running run = running_of(starts[i][0]);
		unsigned t = starts[i][0] == 0x00u ? 1u : 0u;
		unsigned m_total = g_total_ops[i];

		CHECK(m_total > 0u);
		make_image(N_IMAGE, 40u + i);
		for (k = 1; k <= m_total; k++) {
			/* at the boundary after op k */
			setup(starts[i][0], starts[i][1]);
			g_f.cut_after = k;
			(void)write_all(N_IMAGE, g_sha, &run);
			CHECK_EQ(g_f.nmut, k);
			if (k < m_total)
				CHECK(g_w.result != FR_WR_OK);
			after_cut(t, k, m_total, k);
			cuts++;
			/* torn inside op k: seeded bits, none reached, all reached */
			for (v = 0; v < 5u; v++) {
				setup(starts[i][0], starts[i][1]);
				tear(k, v);
				CHECK(write_all(N_IMAGE, g_sha, &run) != FR_WR_OK);
				CHECK(g_f.dead);
				CHECK_EQ(g_f.nmut, k);
				after_cut(t, 0, m_total, 1000u * k + v);
				cuts++;
			}
		}
	}
	CHECK_EQ(cuts, 4u * 6u * g_total_ops[0]);
}

/* --- S-02.052's table for recovery's two requests (S-02.084) --- */

/* The selected and running slot a, made bad with its front intact (one
 * byte of the main image changed in the flash), or with the front itself
 * changed; the other slot b bad. */
static void spoil_main(unsigned a)
{
	g_f.mem[FR_SLOT_BASE(a) + MAIN_BYTE] ^= 0x01u;
}

static void spoil_front(unsigned a)
{
	g_f.mem[FR_SLOT_BASE(a) + FRONT_BYTE] ^= 0x01u;
}

static void test_cells(void)
{
	for (unsigned a = 0; a < 2u; a++)
		for (unsigned cell = 0; cell < 4u; cell++)
			for (unsigned req = 0; req < 2u; req++) {
				const bool a_bad = (cell & 1u) != 0u, b_bad = (cell & 2u) != 0u;
				const unsigned b = 1u - a;
				const uint8_t flags = req ? FR_BEGIN_F_RUNNING : 0u;
				const unsigned way = !req ? FR_WAY_PLAIN :
						     b_bad ? FR_WAY_IN_PLACE : FR_WAY_SELECT_FIRST;
				const unsigned t = req ? a : b;
				struct fr_running run = running_of(a == 0u ? 0x00 : 0x01);
				struct fr_plan plan;
				struct fr_desc front;
				struct fr_slot_info ai;

				setup(a == 0u ? 0x00 : 0x01, 0x01);
				if (a_bad)
					spoil_main(a);
				if (b_bad)
					ff_set_slot(&g_f, b, "badcrc", OLD_TARGET_LEN, 7u);
				put_log(b);
				snapshot();
				CHECK(ff_slot_good(&g_f, a) == !a_bad);
				CHECK(ff_slot_good(&g_f, b) == !b_bad);
				CHECK_EQ(fr_slot_info(flash(), a, &ai), FR_FLASH_OK);
				CHECK(ai.has_front);

				/* S-02.084: in place only once a write of B has failed
				 * in this recovery; before that, status 15 */
				if (way == FR_WAY_IN_PLACE) {
					CHECK_EQ(fr_writer_plan(flash(), &run, &g_home, flags, &plan,
								&front), FR_WR_TRY_OTHER);
					CHECK_EQ(plan.way, FR_WAY_NONE);
					CHECK_EQ(plan.refusal, FR_WR_TRY_OTHER);
					g_home.other_failed = true;
				}
				/* what the start condition says, before anything */
				CHECK_EQ(fr_writer_plan(flash(), &run, &g_home, flags, &plan, &front),
					 FR_WR_OK);
				CHECK_EQ(plan.way, way);
				CHECK_EQ(plan.target, t);
				CHECK_EQ(plan.keep, 1u - t);
				CHECK_EQ(plan.refusal, FR_WR_OK);
				CHECK(memcmp(front.pin, ai.desc.pin, FR_SHA256_LEN) == 0);
				CHECK_EQ(front.front_crc, ai.desc.front_crc);
				CHECK_EQ(g_f.nmut, 0);

				g_flags = flags;
				make_image(N_IMAGE, 80u + 8u * a + 2u * cell + req);
				CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
				CHECK_EQ(g_w.target, t);
				CHECK_EQ(g_w.way, way);
				check_log(t, N_IMAGE, way);
				check_final(t, N_IMAGE, way);
				CHECK_EQ(mutations(), SLOT_OPS + (way == FR_WAY_IN_PLACE ? 0u :
								  way == FR_WAY_PLAIN ? 2u : 4u));
				if (way == FR_WAY_SELECT_FIRST) {
					/* B named first, A named last */
					CHECK_EQ(g_nsel, 2);
					CHECK(g_selprog[0][0] == b && g_selprog[0][1] == 0x01u);
					CHECK(g_selprog[1][0] == a && g_selprog[1][1] == 0x00u);
				}
				/* what a retry would need: set only for a write of A */
				CHECK(g_home.spoiled == (req != 0u));
				CHECK(g_home.moved == (way == FR_WAY_SELECT_FIRST));
				if (req) {
					CHECK(memcmp(g_home.pin, ai.desc.pin, FR_SHA256_LEN) == 0);
					CHECK_EQ(g_home.front_crc, ai.desc.front_crc);
				}
				/* the next reset boots the slot written */
				CHECK_EQ(boot_once(0), t);
			}
}

/* --- Refusals: no erase or program at all, the retry state unchanged --- */

static void refused(int want, struct fr_running run, uint32_t n, const uint8_t *sha)
{
	struct fr_home before = g_home;

	CHECK_EQ(write_all(n, sha, &run), want);
	CHECK_EQ(g_f.nmut, 0);
	CHECK(memcmp(g_f.mem, g_start.mem, sizeof g_f.mem) == 0);
	CHECK(fr_writer_step(&g_w));
	CHECK_EQ(g_f.nmut, 0);
	CHECK(memcmp(&before, &g_home, sizeof before) == 0);
	CHECK_EQ(g_w.target, FR_NO_SLOT);
}

static void refusals(uint8_t flags)
{
	struct fr_running run = running_of(0x01);
	uint8_t bad_sha[32];

	make_image(N_IMAGE, 3u);
	memcpy(bad_sha, g_sha, 32);
	bad_sha[31] ^= 1u;

	/* S-01.002: the running slot not established */
	setup(0x01, 0x01);
	g_flags = flags;
	run.established = false;
	refused(FR_WR_NOT_RUNNING, run, N_IMAGE, g_sha);
	setup(0x01, 0x01);
	g_flags = flags;
	run = running_of(0x01);
	run.slot = 2;
	refused(FR_WR_NOT_RUNNING, run, N_IMAGE, g_sha);
	/* the running slot not the selected one */
	setup(0x01, 0x01);
	g_flags = flags;
	run = running_of(0x00);
	refused(FR_WR_NOT_SELECTED, run, N_IMAGE, g_sha);
	setup(0x00, 0x01);
	g_flags = flags;
	run = running_of(0x01);
	refused(FR_WR_NOT_SELECTED, run, N_IMAGE, g_sha);
	/* S-02.090: verified 0x00 and 0xFF, and 0x02 */
	setup(0x01, 0x00);
	g_flags = flags;
	run = running_of(0x01);
	refused(FR_WR_UNVERIFIED, run, N_IMAGE, g_sha);
	setup(0x01, 0xFF);
	g_flags = flags;
	refused(FR_WR_UNVERIFIED, run, N_IMAGE, g_sha);
	setup(0x00, 0x00);
	g_flags = flags;
	run = running_of(0x00);
	refused(FR_WR_UNVERIFIED, run, N_IMAGE, g_sha);
	setup(0x00, 0x02);
	g_flags = flags;
	refused(FR_WR_UNVERIFIED, run, N_IMAGE, g_sha);
	/* S-02.084, status 5: the selected slot bad and its front not intact:
	 * another image in it, its record erased or empty, a front byte
	 * changed; whatever the other slot */
	for (unsigned b_bad = 0; b_bad < 2u; b_bad++) {
		static const char *const states[] = { "badcrc", "erased", "empty", "badtag" };

		for (unsigned s = 0; s < N(states) + 1u; s++) {
			setup(0x01, 0x01);
			g_flags = flags;
			if (s < N(states))
				ff_set_slot(&g_f, 1, states[s], CUR_LEN, 1u);
			else
				spoil_front(1);
			if (s == N(states))
				spoil_main(1);	/* bad as well */
			if (b_bad)
				ff_set_slot(&g_f, 0, "badcrc", OLD_TARGET_LEN, 7u);
			snapshot();
			CHECK(!ff_slot_good(&g_f, 1));
			run = running_of(0x01);
			refused(FR_WR_SELECTED_BAD, run, N_IMAGE, g_sha);
		}
	}
	/* status 5: the slot this recovery selected went bad */
	setup(0x01, 0x01);
	g_flags = flags;
	ff_set_selector(&g_f, 0x00, 0x01);
	ff_set_slot(&g_f, 0, "badcrc", OLD_TARGET_LEN, 7u);
	snapshot();
	g_home.moved = true;
	g_home.spoiled = true;
	{
		struct fr_slot_info ai;

		CHECK_EQ(fr_slot_info(flash(), 1, &ai), FR_FLASH_OK);
		memcpy(g_home.pin, ai.desc.pin, FR_SHA256_LEN);
		g_home.front_crc = ai.desc.front_crc;
	}
	ff_reset(&g_f);
	run = running_of(0x01);
	refused(FR_WR_SELECTED_BAD, run, N_IMAGE, g_sha);
	/* S-02.044 */
	setup(0x01, 0x01);
	g_flags = flags;
	run = running_of(0x01);
	refused(FR_WR_SIZE, run, 0, g_sha);
	CHECK_EQ(g_w.image_rc, FR_IMG_SIZE);
	memset(g_img, 0xFF, FR_SLOT_CAP + 1u);
	make_image(N_IMAGE, 3u);
	refused(FR_WR_SIZE, run, FR_SLOT_CAP + 1u, g_sha);
	/* S-02.091: a stream that does not parse, and one cut short */
	memcpy(g_img, v_boot_bad_signature, sizeof v_boot_bad_signature);
	sha256(g_sha, g_img, sizeof v_boot_bad_signature);
	refused(FR_WR_IMAGE, run, sizeof v_boot_bad_signature, g_sha);
	CHECK_EQ(g_w.image_rc, FR_IMG_STREAM);
	make_image(N_IMAGE, 3u);
	sha256(g_sha, g_img, 0x200u);
	refused(FR_WR_IMAGE, run, 0x200u, g_sha);
	CHECK_EQ(g_w.image_rc, FR_IMG_STREAM);
	/* a bootable stream without a front */
	memcpy(g_img, v_boot_good, sizeof v_boot_good);
	sha256(g_sha, g_img, sizeof v_boot_good);
	refused(FR_WR_IMAGE, run, sizeof v_boot_good, g_sha);
	CHECK_EQ(g_w.image_rc, FR_IMG_NO_FRONT);
	/* not the image announced */
	make_image(N_IMAGE, 3u);
	refused(FR_WR_HASH, run, N_IMAGE, bad_sha);
	/* S-02.091: the image is judged before the start condition is read */
	setup(0x01, 0x00);
	g_flags = flags;
	memcpy(g_img, v_boot_good, sizeof v_boot_good);
	sha256(g_sha, g_img, sizeof v_boot_good);
	refused(FR_WR_IMAGE, run, sizeof v_boot_good, g_sha);
	CHECK_EQ(g_f.nlog, 0);
	/* reads that fail: the selector, the selected slot, the other one */
	make_image(N_IMAGE, 3u);
	setup(0x01, 0x01);
	g_flags = flags;
	g_f.fail_regions = FF_R_SELECTOR;
	refused(FR_WR_READ, run, N_IMAGE, g_sha);	/* status 17 */
	setup(0x01, 0x01);
	g_flags = flags;
	g_f.fail_regions = FF_R_SLOT1;
	refused(FR_WR_READ, run, N_IMAGE, g_sha);
	if (flags & FR_BEGIN_F_RUNNING) {
		/* the other slot, read good or bad for the way */
		setup(0x01, 0x01);
		g_flags = flags;
		g_f.fail_regions = FF_R_SLOT0 | FF_R_RECORD0;
		refused(FR_WR_READ, run, N_IMAGE, g_sha);
	}
	/* a write under way is busy, and left as it was */
	setup(0x01, 0x01);
	g_flags = flags;
	CHECK_EQ(fr_writer_begin(&g_w, flash(), &run, &g_home, g_img, N_IMAGE, g_sha, flags),
		 FR_WR_OK);
	CHECK(!fr_writer_step(&g_w));
	{
		struct fr_writer copy = g_w;
		struct fr_home home = g_home;

		CHECK_EQ(fr_writer_begin(&g_w, flash(), &run, &g_home, g_img, N_IMAGE, g_sha, flags),
			 FR_WR_BUSY);
		CHECK(memcmp(&copy, &g_w, sizeof copy) == 0);
		CHECK(memcmp(&home, &g_home, sizeof home) == 0);
	}
	/* an idle writer is done at once */
	memset(&g_w, 0, sizeof g_w);
	CHECK(fr_writer_step(&g_w));
}

static void test_refusals(void)
{
	refusals(0);
	refusals(FR_BEGIN_F_RUNNING);
}

/* --- Faults during the write (S-02.047) --- */

static void no_selector_op(void)
{
	CHECK_EQ(find_op(FF_ERASE, FR_SELECTOR_OFF, 0), ~0u);
	CHECK_EQ(find_op(FF_PROGRAM, FR_SELECTOR_OFF, 0), ~0u);
	CHECK(memcmp(g_f.mem + FR_SELECTOR_OFF, g_start.mem + FR_SELECTOR_OFF, FR_SECTOR) == 0);
}

static void test_faults(void)
{
	struct fr_running run = running_of(0x01);
	unsigned m = g_total_ops[1], rec_prog = m - 2u;	/* start 01 01: target 0 */

	make_image(N_IMAGE, 41u);
	/* a corrupted slot program: the read-back refuses the slot */
	setup(0x01, 0x01);
	g_f.corrupt = 5;
	g_f.corrupt_mask = 0xFE;
	CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_READBACK);
	no_selector_op();
	check_untouched(0);
	CHECK(find_op(FF_PROGRAM, FR_RECORD_OFF(0), 0) != ~0u);
	CHECK(run_resets(0, 2, 0, 0));
	/* a corrupted record program */
	setup(0x01, 0x01);
	g_f.corrupt = rec_prog;
	g_f.corrupt_mask = 0xFE;
	CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_READBACK);
	no_selector_op();
	/* a read-back read that fails */
	setup(0x01, 0x01);
	CHECK_EQ(fr_writer_begin(&g_w, flash(), &run, &g_home, g_img, N_IMAGE, g_sha, 0), FR_WR_OK);
	g_f.fail_regions = FF_R_SLOT0;
	while (!fr_writer_step(&g_w))
		;
	CHECK_EQ(g_w.result, FR_WR_FLASH);
	no_selector_op();
	CHECK_EQ(g_f.nmut, m - 2u);
	/* an erase that fails stops at once: the first, a later one, the last
	 * sector's, the record's */
	{
		static const unsigned at[] = {1u, 18u, 35u};
		unsigned i;

		for (i = 0; i < N(at); i++) {
			setup(0x01, 0x01);
			g_f.fail = at[i];
			CHECK_EQ(g_f.nmut, 0);
			CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
			CHECK_EQ(g_f.nmut, at[i]);
			CHECK_EQ(g_f.log[g_f.nlog - 1u].kind, FF_ERASE);
			no_selector_op();
			check_untouched(0);
		}
		setup(0x01, 0x01);
		g_f.fail = m - 4u;	/* the last sector's */
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
		CHECK_EQ(g_f.nmut, m - 4u);
		CHECK_EQ(g_f.log[g_f.nlog - 1u].off, FR_SLOT_BASE(0) + FR_LOG_IMAGE_MAX);
		CHECK_EQ(find_op(FF_ERASE, FR_RECORD_OFF(0), 0), ~0u);
		no_selector_op();
		setup(0x01, 0x01);
		g_f.fail = m - 3u;	/* the record erase */
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
		CHECK_EQ(g_f.nmut, m - 3u);
		CHECK_EQ(g_f.log[g_f.nlog - 1u].off, FR_RECORD_OFF(0));
		no_selector_op();
	}
	/* a program that fails, the record's */
	setup(0x01, 0x01);
	g_f.fail = 7;
	CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
	CHECK_EQ(g_f.nmut, 7);
	no_selector_op();
	setup(0x01, 0x01);
	g_f.fail = rec_prog;
	CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
	CHECK_EQ(g_f.nmut, rec_prog);
	no_selector_op();
	/* the selector's own erase or program failing: no later operation */
	setup(0x01, 0x01);
	g_f.fail = m - 1u;
	CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_SELECTOR);
	CHECK_EQ(g_f.nmut, m - 1u);
	CHECK_EQ(find_op(FF_PROGRAM, FR_SELECTOR_OFF, 0), ~0u);
	setup(0x01, 0x01);
	g_f.fail = m;
	CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_SELECTOR);
	CHECK_EQ(g_f.nmut, m);
	/* a retry after a fault starts again and succeeds */
	setup(0x01, 0x01);
	g_f.fail = 20;
	CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
	memcpy(g_cut.mem, g_f.mem, sizeof g_f.mem);
	ff_reset(&g_f);
	CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
	check_log(0, N_IMAGE, FR_WAY_PLAIN);
}

/* --- S-02.084: only the running front, and not the kept slot's own stream --- */

static void test_front_and_same(void)
{
	struct fr_running run = running_of(0x01);	/* selected slot 1, target 0 */
	static uint8_t cur[0x3000], other[0x3000];
	uint32_t e;

	/* another front: its bytes (and so its front CRC) and its pin */
	g_fseed = FRONT_SEED + 1u;
	make_image(N_IMAGE, 3u);
	setup(0x01, 0x01);
	refused(FR_WR_FRONT, run, N_IMAGE, g_sha);
	CHECK_EQ(g_w.image_rc, FR_IMG_OK);
	g_fseed = FRONT_SEED;
	/* the same front's bytes, a pin that differs */
	g_pin_flip = true;
	make_image(N_IMAGE, 3u);
	setup(0x01, 0x01);
	refused(FR_WR_FRONT, run, N_IMAGE, g_sha);
	g_flags = FR_BEGIN_F_RUNNING;
	refused(FR_WR_FRONT, run, N_IMAGE, g_sha);
	g_pin_flip = false;
	/* the selected slot good but its front not intact (its record made to
	 * match), or good with no waffle-boot image: refused, nothing written
	 * (S-02.093 gives this case no status of its own) */
	make_image(N_IMAGE, 3u);
	setup(0x01, 0x01);
	spoil_front(1);
	ff_put32(g_f.mem + FR_RECORD_OFF(1), ref_crc32(0, g_f.mem + FR_SLOT_BASE(1), CUR_LEN));
	snapshot();
	CHECK(ff_slot_good(&g_f, 1));
	CHECK(write_all(N_IMAGE, g_sha, &run) != FR_WR_OK);
	CHECK_EQ(g_f.nmut, 0);
	setup(0x01, 0x01);
	ff_set_slot(&g_f, 1, "good", CUR_LEN, 1u);
	snapshot();
	CHECK(write_all(N_IMAGE, g_sha, &run) != FR_WR_OK);
	CHECK_EQ(g_f.nmut, 0);
	/* the start condition is still read first */
	setup(0x01, 0x00);
	ff_set_slot(&g_f, 1, "good", CUR_LEN, 1u);
	snapshot();
	refused(FR_WR_UNVERIFIED, run, N_IMAGE, g_sha);

	/* The default write: the selected slot's own image, byte for byte,
	 * is written as any other: two slots may hold the same image */
	setup(0x01, 0x01);
	memcpy(g_img, g_f.mem + FR_SLOT_BASE(1), CUR_LEN);
	sha256(g_sha, g_img, CUR_LEN);
	CHECK_EQ(write_all(CUR_LEN, g_sha, &run), FR_WR_OK);
	check_log(0, CUR_LEN, FR_WAY_PLAIN);
	CHECK(memcmp(g_f.mem + FR_SLOT_BASE(0), g_f.mem + FR_SLOT_BASE(1), CUR_LEN) == 0);
	CHECK(ff_slot_good(&g_f, 0) && ff_slot_good(&g_f, 1));
	setup(0x01, 0x01);
	e = build_image(cur, sizeof cur, 1u);
	CHECK(e < 0x1000u);
	memcpy(g_img, cur, sizeof cur);
	/* the slot's record ending inside the stream: its own stream has no
	 * FINAL, so no front, and the front's rule refuses first */
	ff_put32(g_f.mem + FR_RECORD_OFF(1), ref_crc32(0, g_f.mem + FR_SLOT_BASE(1), e - 1u));
	ff_put32(g_f.mem + FR_RECORD_OFF(1) + 4u, e - 1u);
	memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
	sha256(g_sha, g_img, sizeof cur);
	CHECK(write_all(sizeof cur, g_sha, &run) != FR_WR_OK);
	CHECK_EQ(g_f.nmut, 0);
	/* one byte of main's payload differs: written */
	setup(0x01, 0x01);
	e = build_image(g_img, CUR_LEN, 1u);
	g_img[e - 16u - 1u] ^= 0x01u;		/* the last byte before FINAL's header */
	sha256(g_sha, g_img, CUR_LEN);
	CHECK_EQ(fr_image_valid(g_img, CUR_LEN, NULL), FR_IMG_OK);
	CHECK_EQ(write_all(CUR_LEN, g_sha, &run), FR_WR_OK);
	check_log(0, CUR_LEN, FR_WAY_PLAIN);
	/* A write of the running slot: the kept slot is the other one. Slot 0
	 * holds image X with the running front; X is written to slot 1 all the
	 * same, whether slot 0 is good (select first) or bad (in place), and
	 * whether its record is plausible or not. */
	for (unsigned v = 0; v < 4u; v++) {
		setup(0x01, 0x01);
		g_flags = FR_BEGIN_F_RUNNING;
		e = build_image(other, sizeof other, 21u);
		put_slot(0, other, sizeof other);
		if (v == 1u)
			g_f.mem[FR_SLOT_BASE(0) + e + 100u] ^= 0x01u;	/* bad, after FINAL */
		if (v == 2u)
			memset(g_f.mem + FR_RECORD_OFF(0), 0xFF, 12);	/* bad, no record */
		if (v == 3u) {
			/* bad, a record that ends before the stream does */
			ff_put32(g_f.mem + FR_RECORD_OFF(0), 0);
			ff_put32(g_f.mem + FR_RECORD_OFF(0) + 4u, e - 1u);
		}
		snapshot();
		if (v != 0u)
			g_home.other_failed = true;	/* in place may go (S-02.084) */
		CHECK(ff_slot_good(&g_f, 0) == (v == 0u));
		memcpy(g_img, other, sizeof other);
		sha256(g_sha, g_img, sizeof other);
		{
			const unsigned way = v == 0u ? FR_WAY_SELECT_FIRST : FR_WAY_IN_PLACE;

			CHECK_EQ(write_all(sizeof other, g_sha, &run), FR_WR_OK);
			CHECK_EQ(g_w.way, way);
			check_log(1, sizeof other, way);
		}
		/* the same image to the slot that holds it, by default: the kept
		 * slot is then the running one, which differs */
		if (v == 0u) {
			setup(0x01, 0x01);
			put_slot(0, other, sizeof other);
			snapshot();
			CHECK_EQ(write_all(sizeof other, g_sha, &run), FR_WR_OK);
			CHECK_EQ(g_w.target, 0);
		}
	}
}

/* --- S-02.093: the read-back's SHA-256 before the selector --- */

static void test_readback_sha(void)
{
	/* zlib's CRC-32 polynomial, reflected, as 33 bits from the LSB: XORed
	 * into the data at any byte, the CRC of the whole is unchanged */
	static const uint8_t poly[5] = {0x41, 0x06, 0x71, 0xDB, 0x01};
	struct fr_running run = running_of(0x01);
	uint32_t crc;

	/* The stored SHA-256 differs from the read-back's: no selector op. */
	make_image(N_IMAGE, 7u);
	setup(0x01, 0x01);
	CHECK_EQ(fr_writer_begin(&g_w, flash(), &run, &g_home, g_img, N_IMAGE, g_sha, 0), FR_WR_OK);
	CHECK(memcmp(g_w.want_sha, g_sha, 32) == 0);
	g_w.want_sha[17] ^= 0x04u;
	while (!fr_writer_step(&g_w))
		;
	CHECK_EQ(g_w.result, FR_WR_READBACK);
	CHECK(memcmp(g_w.readback_sha, g_sha, 32) == 0);
	no_selector_op();
	CHECK(find_op(FF_PROGRAM, FR_RECORD_OFF(0), 0) != ~0u);

	/* The staged image changed after BEGIN's check so that its CRC-32 is
	 * the same: the slot, its record and the read-back's CRC all agree,
	 * and only the SHA-256 tells. */
	make_image(N_IMAGE, 7u);
	setup(0x01, 0x01);
	crc = ref_crc32(0, g_img, N_IMAGE);
	CHECK_EQ(fr_writer_begin(&g_w, flash(), &run, &g_home, g_img, N_IMAGE, g_sha, 0), FR_WR_OK);
	for (unsigned i = 0; i < sizeof poly; i++)
		g_img[N_IMAGE - 64u + i] ^= poly[i];	/* after FINAL */
	CHECK_EQ(ref_crc32(0, g_img, N_IMAGE), crc);
	while (!fr_writer_step(&g_w))
		;
	CHECK_EQ(g_w.result, FR_WR_READBACK);
	no_selector_op();
	CHECK(memcmp(g_f.mem + FR_SLOT_BASE(0), g_img, N_IMAGE) == 0);
	CHECK_EQ(fr_le32(g_f.mem + FR_RECORD_OFF(0)), crc);
	CHECK(run_resets(1, 2, 0, 0));

	/* In place: the SHA-256 is the last word as well, and no selector
	 * write follows either way */
	make_image(N_IMAGE, 7u);
	setup(0x01, 0x01);
	ff_set_slot(&g_f, 0, "badcrc", OLD_TARGET_LEN, 7u);
	snapshot();
	g_home.other_failed = true;
	CHECK_EQ(fr_writer_begin(&g_w, flash(), &run, &g_home, g_img, N_IMAGE, g_sha,
				 FR_BEGIN_F_RUNNING), FR_WR_OK);
	CHECK_EQ(g_w.way, FR_WAY_IN_PLACE);
	g_w.want_sha[0] ^= 0x01u;
	while (!fr_writer_step(&g_w))
		;
	CHECK_EQ(g_w.result, FR_WR_READBACK);
	no_selector_op();
}

/* --- S-02.048: the selector read back after its program --- */

static void test_selector_readback(void)
{
	struct fr_running run = running_of(0x01);

	make_image(N_IMAGE, 8u);
	for (unsigned mode = 1; mode <= 3u; mode++) {
		setup(0x01, 0x01);
		g_sel_mode = mode;
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_SELECTOR);
		CHECK(find_op(FF_PROGRAM, FR_SELECTOR_OFF, 0) != ~0u);
		/* the slot is written and read back; the selector still names a
		 * good slot ({FF FF}: slot 1, the old one; {00 01}: the new one) */
		CHECK(ff_slot_good(&g_f, 0));
		CHECK(ff_slot_good(&g_f, fr_selected_slot(g_f.mem[FR_SELECTOR_OFF])));
	}
	CHECK_EQ(g_f.mem[FR_SELECTOR_OFF], 0x00u);	/* mode 3 stored it right */
	setup(0x01, 0x01);
	CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
	/* the read after the program: the last operation */
	CHECK_EQ(g_f.log[g_f.nlog - 1u].kind, FF_READ);
	CHECK_EQ(g_f.log[g_f.nlog - 1u].off, FR_SELECTOR_OFF);
	CHECK_EQ(g_f.log[g_f.nlog - 2u].kind, FF_PROGRAM);
	CHECK_EQ(g_f.log[g_f.nlog - 2u].off, FR_SELECTOR_OFF);
}

/* --- S-02.084: the selector write before a write of the running slot --- */

/* Nothing but the selector sector touched, the slots and records as they
 * were (S-02.093 status 14). */
static void only_selector(void)
{
	for (unsigned i = 0; i < g_f.nlog; i++)
		if (g_f.log[i].kind != FF_READ)
			CHECK_EQ(g_f.log[i].off, FR_SELECTOR_OFF);
	CHECK(memcmp(g_f.mem, g_start.mem, FR_SELECTOR_OFF) == 0);
	CHECK(memcmp(g_f.mem + FR_SELECTOR_OFF + FR_SECTOR, g_start.mem + FR_SELECTOR_OFF + FR_SECTOR,
		     sizeof g_f.mem - FR_SELECTOR_OFF - FR_SECTOR) == 0);
}

static void test_select_first_fails(void)
{
	for (unsigned a = 0; a < 2u; a++) {
		struct fr_running run = running_of(a == 0u ? 0x00 : 0x01);

		make_image(N_IMAGE, 90u + a);
		/* its erase fails */
		setup(a == 0u ? 0x00 : 0x01, 0x01);
		g_flags = FR_BEGIN_F_RUNNING;
		g_f.fail = 1;
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_SELECT_FIRST);
		CHECK_EQ(g_f.nmut, 1);
		only_selector();
		/* its program fails */
		setup(a == 0u ? 0x00 : 0x01, 0x01);
		g_flags = FR_BEGIN_F_RUNNING;
		g_f.fail = 2;
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_SELECT_FIRST);
		CHECK_EQ(g_f.nmut, 2);
		only_selector();
		/* it does not read back: nothing stored, the wrong bytes, a read
		 * that fails */
		for (unsigned mode = 1; mode <= 3u; mode++) {
			setup(a == 0u ? 0x00 : 0x01, 0x01);
			g_flags = FR_BEGIN_F_RUNNING;
			g_sel_mode = mode;
			g_sel_nth = 1;
			CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_SELECT_FIRST);
			CHECK_EQ(g_nsel, 1);
			only_selector();
			CHECK_EQ(g_w.target, a);
		}
	}
}

/* --- Cuts during a write of the running slot --- */

/* A, the running slot, as this boot started it, or as the write leaves it
 * complete. */
static bool a_as_started(unsigned a)
{
	return memcmp(g_f.mem + FR_SLOT_BASE(a), g_start.mem + FR_SLOT_BASE(a), FR_SLOT_CAP) == 0 &&
	       memcmp(g_f.mem + FR_RECORD_OFF(a), g_start.mem + FR_RECORD_OFF(a), 12) == 0;
}

static bool a_complete(unsigned a)
{
	return ff_slot_good(&g_f, a) && memcmp(g_f.mem + FR_SLOT_BASE(a), g_img, N_IMAGE) == 0;
}

/*
 * The other slot good, selected first (S-02.052's table, S-02.051): after a
 * cut at any instant every reset boots a good slot. With A = slot 0 that is
 * slot 1 (B) until the last selector program, then either; with A = slot 1
 * an erased selector names slot 1, so a reset boots B, or A as this boot
 * started it, or A complete.
 */
static void sf_after_cut(unsigned a, bool boundary, unsigned k, unsigned m_total, uint32_t seed)
{
	const unsigned b = 1u - a;

	CHECK(!g_f.bad_call);
	check_untouched(a);
	memcpy(g_cut.mem, g_f.mem, sizeof g_f.mem);
	for (int prof = 0; prof < 2; prof++) {
		unsigned s;

		memcpy(g_f.mem, g_cut.mem, sizeof g_f.mem);
		s = boot_once(prof);
		CHECK(ff_slot_good(&g_f, s));
		if (a == 0u && boundary && k < m_total)
			CHECK_EQ(s, b);
		else
			CHECK(s == b || a_as_started(a) || a_complete(a));
		memcpy(g_f.mem, g_cut.mem, sizeof g_f.mem);
		CHECK(run_resets(prof, 3u, 0u, 0u));
		/* further cuts, after the cuts at a boundary */
		for (unsigned m = 1; boundary && m <= 2u; m++) {
			memcpy(g_f.mem, g_cut.mem, sizeof g_f.mem);
			CHECK(run_resets(prof, 1u, m, seed * 7u + m));
			CHECK(run_resets(prof, 3u, 0u, 0u));
		}
	}
}

static void test_cuts_select_first(void)
{
	for (unsigned a = 0; a < 2u; a++) {
		const uint8_t index = a == 0u ? 0x00 : 0x01;
		struct fr_running run = running_of(index);
		const unsigned m_total = SLOT_OPS + 4u;

		make_image(N_IMAGE, 100u + a);
		for (unsigned k = 1; k <= m_total; k++) {
			setup(index, 0x01);
			g_flags = FR_BEGIN_F_RUNNING;
			g_f.cut_after = k;
			(void)write_all(N_IMAGE, g_sha, &run);
			CHECK_EQ(g_f.nmut, k);
			CHECK_EQ(g_w.way, FR_WAY_SELECT_FIRST);
			sf_after_cut(a, true, k, m_total, k);
			for (unsigned v = 0; v < 5u; v++) {
				setup(index, 0x01);
				g_flags = FR_BEGIN_F_RUNNING;
				tear(k, v);
				CHECK(write_all(N_IMAGE, g_sha, &run) != FR_WR_OK);
				CHECK_EQ(g_f.nmut, k);
				sf_after_cut(a, false, k, m_total, 1000u * k + v);
			}
		}
	}
}

/*
 * The other slot bad: the write in place (S-02.052's table, S-02.084).
 * THE BRICK WINDOW: from the first erase of the running slot until its
 * record's program has completed, the selector names that slot with
 * verified 01 and the slot is not good, and the other slot is bad too: a
 * cut anywhere in it leaves a unit on which no first stage finds a slot
 * that starts. Only an external programmer recovers it. After the record's
 * program the slot is good and every reset boots it; the read-back that
 * follows writes nothing. Nothing narrows this window here; the test
 * states it, and that the selector is never touched.
 */
static void test_cuts_in_place(void)
{
	for (unsigned a = 0; a < 2u; a++) {
		const uint8_t index = a == 0u ? 0x00 : 0x01;
		struct fr_running run = running_of(index);
		const unsigned m_total = SLOT_OPS;

		make_image(N_IMAGE, 110u + a);
		for (unsigned k = 1; k <= m_total; k++)
			for (unsigned v = 0; v < 6u; v++) {
				setup(index, 0x01);
				ff_set_slot(&g_f, 1u - a, "badcrc", OLD_TARGET_LEN, 7u);
				snapshot();
				g_home.other_failed = true;	/* a write of B failed before */
				g_flags = FR_BEGIN_F_RUNNING;
				if (v == 5u)
					g_f.cut_after = k;
				else
					tear(k, v);
				(void)write_all(N_IMAGE, g_sha, &run);
				CHECK_EQ(g_f.nmut, k);
				CHECK_EQ(g_w.way, FR_WAY_IN_PLACE);
				CHECK(!g_f.bad_call);
				check_untouched(a);
				CHECK(memcmp(g_f.mem + FR_SELECTOR_OFF, g_start.mem + FR_SELECTOR_OFF,
					     FR_SECTOR) == 0);
				memcpy(g_cut.mem, g_f.mem, sizeof g_f.mem);
				for (int prof = 0; prof < 2; prof++) {
					const bool as_started = a_as_started(a);

					memcpy(g_f.mem, g_cut.mem, sizeof g_f.mem);
					/* in the window nothing starts, unless a torn first
					 * erase reached no bit of the slot */
					if (k < m_total)
						CHECK(run_resets(prof, 1u, 0u, 0u) == as_started);
					else if (v == 5u)
						CHECK(run_resets(prof, 3u, 0u, 0u));
				}
			}
	}
}

/* --- Retries after a failed write of the running slot (S-02.084) --- */

static void test_retry_select_first(void)
{
	for (unsigned a = 0; a < 2u; a++)
		for (unsigned req = 0; req < 2u; req++) {
			const uint8_t index = a == 0u ? 0x00 : 0x01;
			const unsigned b = 1u - a;
			struct fr_running run = running_of(index);
			struct fr_plan plan;
			struct fr_desc front;
			struct fr_slot_info ai;
			struct fr_home fresh;

			make_image(N_IMAGE, 120u + a);
			setup(index, 0x01);
			g_flags = FR_BEGIN_F_RUNNING;
			/* the first erase of A is made, then reported failed: A's
			 * front is gone from the flash */
			g_erase_then_fail = 3;
			CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
			CHECK(g_f.mem[FR_SELECTOR_OFF] == b && g_f.mem[FR_SELECTOR_OFF + 1u] == 0x01u);
			CHECK(g_home.moved && g_home.spoiled);
			CHECK_EQ(fr_slot_info(flash(), a, &ai), FR_FLASH_OK);
			CHECK(!ai.has_front && !ai.good);
			/* a recovery that had done nothing would refuse now */
			memset(&fresh, 0, sizeof fresh);
			CHECK_EQ(fr_writer_plan(flash(), &run, &fresh, FR_BEGIN_F_RUNNING, &plan, &front),
				 FR_WR_NOT_SELECTED);
			/* A is bad by this write, the selector reads {B, 01} and B
			 * is good: S-02.084's retry rule. Either request writes A,
			 * the slot the selector does not name, with no selector
			 * write before it. */
			g_flags = req ? FR_BEGIN_F_RUNNING : 0u;
			CHECK_EQ(fr_writer_plan(flash(), &run, &g_home, g_flags, &plan, &front), FR_WR_OK);
			CHECK_EQ(plan.way, FR_WAY_PLAIN);
			CHECK_EQ(plan.target, a);
			CHECK_EQ(plan.keep, b);
			CHECK(!plan.only_same);
			memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
			ff_reset(&g_f);
			g_erase_then_fail = 0;
			CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
			CHECK_EQ(g_w.target, a);
			CHECK_EQ(g_w.way, FR_WAY_PLAIN);
			CHECK(g_f.log[0].kind == FF_READ || g_f.log[0].off != FR_SELECTOR_OFF);
			CHECK(find_op(FF_ERASE, FR_SLOT_BASE(a), 0) < find_op(FF_ERASE, FR_SELECTOR_OFF, 0));
			g_split = true;		/* A is damaged: S-02.084 */
			check_log(a, N_IMAGE, FR_WAY_PLAIN);
			check_final(a, N_IMAGE, FR_WAY_PLAIN);
			/* only the running front, still: another front is refused */
			setup(index, 0x01);
			g_flags = FR_BEGIN_F_RUNNING;
			g_erase_then_fail = 3;
			make_image(N_IMAGE, 120u + a);
			CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
			g_fseed = FRONT_SEED + 1u;
			make_image(N_IMAGE, 121u + a);
			g_fseed = FRONT_SEED;
			memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
			ff_reset(&g_f);
			g_erase_then_fail = 0;
			refused(FR_WR_FRONT, run, N_IMAGE, g_sha);
		}
}

static void test_retry_in_place(void)
{
	for (unsigned a = 0; a < 2u; a++) {
		const uint8_t index = a == 0u ? 0x00 : 0x01;
		struct fr_running run = running_of(index);
		struct fr_plan plan;
		struct fr_desc front;
		struct fr_slot_info ai;
		struct fr_home fresh;

		make_image(N_IMAGE, 130u + a);
		setup(index, 0x01);
		ff_set_slot(&g_f, 1u - a, "badcrc", OLD_TARGET_LEN, 7u);
		snapshot();
		g_home.other_failed = true;	/* a write of B failed before */
		g_flags = FR_BEGIN_F_RUNNING;
		g_erase_then_fail = 1;
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
		CHECK(memcmp(g_f.mem + FR_SELECTOR_OFF, g_start.mem + FR_SELECTOR_OFF, FR_SECTOR) == 0);
		CHECK(g_home.spoiled && !g_home.moved);
		CHECK_EQ(fr_slot_info(flash(), a, &ai), FR_FLASH_OK);
		CHECK(!ai.has_front && !ai.good);
		/* a recovery that had done nothing would refuse it, status 5 */
		memset(&fresh, 0, sizeof fresh);
		CHECK_EQ(fr_writer_plan(flash(), &run, &fresh, FR_BEGIN_F_RUNNING, &plan, &front),
			 FR_WR_SELECTED_BAD);
		/* accepted, though A's front is gone, and it ends as the first */
		CHECK_EQ(fr_writer_plan(flash(), &run, &g_home, FR_BEGIN_F_RUNNING, &plan, &front),
			 FR_WR_OK);
		CHECK_EQ(plan.way, FR_WAY_IN_PLACE);
		CHECK_EQ(plan.target, a);
		CHECK(memcmp(front.pin, g_home.pin, FR_SHA256_LEN) == 0);
		ff_reset(&g_f);
		g_erase_then_fail = 0;
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
		CHECK_EQ(g_w.way, FR_WAY_IN_PLACE);
		check_log(a, N_IMAGE, FR_WAY_IN_PLACE);
		check_final(a, N_IMAGE, FR_WAY_IN_PLACE);
		/* the default request then targets B, as ever */
		CHECK_EQ(fr_writer_plan(flash(), &run, &g_home, 0, &plan, &front), FR_WR_OK);
		CHECK_EQ(plan.target, 1u - a);
	}
}

/* --- S-02.084: in place only after a write of the other slot failed --- */

static void test_in_place_gate(void)
{
	for (unsigned a = 0; a < 2u; a++) {
		const uint8_t index = a == 0u ? 0x00 : 0x01;
		const unsigned b = 1u - a;
		struct fr_running run = running_of(index);
		struct fr_plan plan;
		struct fr_desc front;
		const unsigned m = SLOT_OPS + 2u;	/* a plain write's mutations */

		make_image(N_IMAGE, 140u + a);
		/* before any write: status 15, nothing written, home as it was */
		setup(index, 0x01);
		ff_set_slot(&g_f, b, "badcrc", OLD_TARGET_LEN, 7u);
		snapshot();
		CHECK_EQ(fr_writer_plan(flash(), &run, &g_home, FR_BEGIN_F_RUNNING, &plan, &front),
			 FR_WR_TRY_OTHER);
		CHECK_EQ(plan.way, FR_WAY_NONE);
		CHECK_EQ(plan.target, FR_NO_SLOT);
		g_flags = FR_BEGIN_F_RUNNING;
		refused(FR_WR_TRY_OTHER, run, N_IMAGE, g_sha);
		CHECK(!g_home.other_failed);

		/* a plain write of B that fails: 9 (an erase, a program), 10 (a
		 * program stored wrong): in place goes from then on */
		for (unsigned f = 0; f < 3u; f++) {
			setup(index, 0x01);
			ff_set_slot(&g_f, b, "badcrc", OLD_TARGET_LEN, 7u);
			snapshot();
			if (f == 0u)
				g_f.fail = 1;
			else if (f == 1u)
				g_f.fail = 5;
			else {
				g_f.corrupt = 5;
				g_f.corrupt_mask = 0xFE;
			}
			CHECK_EQ(write_all(N_IMAGE, g_sha, &run), f == 2u ? FR_WR_READBACK : FR_WR_FLASH);
			CHECK(g_home.other_failed);
			CHECK(!g_home.spoiled && !g_home.moved);
			ff_reset(&g_f);
			CHECK_EQ(fr_writer_plan(flash(), &run, &g_home, FR_BEGIN_F_RUNNING, &plan,
						&front), FR_WR_OK);
			CHECK_EQ(plan.way, FR_WAY_IN_PLACE);
			CHECK_EQ(plan.target, a);
		}

		/* a plain write of B that fails at the selector (11), or is
		 * refused: still 15 */
		for (unsigned f = 0; f < 3u; f++) {
			setup(index, 0x01);
			ff_set_slot(&g_f, b, "badcrc", OLD_TARGET_LEN, 7u);
			snapshot();
			if (f == 0u) {
				g_f.fail = m - 1u;
				CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_SELECTOR);
			} else if (f == 1u) {
				g_f.fail = m;
				CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_SELECTOR);
			} else {
				uint8_t bad[32];

				memcpy(bad, g_sha, sizeof bad);
				bad[0] ^= 1u;
				CHECK_EQ(write_all(N_IMAGE, bad, &run), FR_WR_HASH);
			}
			CHECK(!g_home.other_failed);
			/* B bad and A selected again (a selector failure leaves B
			 * good, or the selector torn): the gate is still shut */
			ff_set_slot(&g_f, b, "badcrc", OLD_TARGET_LEN, 7u);
			ff_set_selector(&g_f, index, 0x01);
			ff_reset(&g_f);
			CHECK_EQ(fr_writer_plan(flash(), &run, &g_home, FR_BEGIN_F_RUNNING, &plan,
						&front), FR_WR_TRY_OTHER);
		}

		/* a failed write of the running slot does not count: select first
		 * (B good) failing at A's first erase, then its retry, a plain
		 * write of A, failing too */
		setup(index, 0x01);
		g_flags = FR_BEGIN_F_RUNNING;
		g_f.fail = 3;
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
		CHECK(!g_home.other_failed && g_home.moved);
		ff_reset(&g_f);
		g_f.fail = 1;
		g_flags = 0;
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
		CHECK_EQ(g_w.target, a);
		CHECK(!g_home.other_failed);
		/* nor a select-first write failing at the selector (14) */
		setup(index, 0x01);
		g_flags = FR_BEGIN_F_RUNNING;
		g_f.fail = 1;
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_SELECT_FIRST);
		CHECK(!g_home.other_failed);
		/* nor a successful plain write */
		setup(index, 0x01);
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
		CHECK(!g_home.other_failed);
	}
}

/* --- S-02.084: a good running slot is not rewritten with its own image --- */

static void test_own_image(void)
{
	static uint8_t x[0x3000];

	for (unsigned a = 0; a < 2u; a++)
		for (unsigned cell = 0; cell < 4u; cell++) {
			const bool a_bad = (cell & 1u) != 0u, b_bad = (cell & 2u) != 0u;
			const uint8_t index = a == 0u ? 0x00 : 0x01;
			struct fr_running run = running_of(index);
			uint32_t e;

			setup(index, 0x01);
			e = build_image(x, sizeof x, 31u);
			put_slot(a, x, sizeof x);
			if (a_bad)
				g_f.mem[FR_SLOT_BASE(a) + e + 64u] ^= 0x01u;	/* after FINAL */
			if (b_bad)
				ff_set_slot(&g_f, 1u - a, "badcrc", OLD_TARGET_LEN, 7u);
			snapshot();
			g_home.other_failed = true;
			CHECK(ff_slot_good(&g_f, a) == !a_bad);
			g_flags = FR_BEGIN_F_RUNNING;
			memcpy(g_img, x, sizeof x);
			sha256(g_sha, g_img, sizeof x);
			{
				/* a good running slot's own image too: written again */
				const unsigned way = b_bad ? FR_WAY_IN_PLACE : FR_WAY_SELECT_FIRST;

				CHECK_EQ(write_all(sizeof x, g_sha, &run), FR_WR_OK);
				CHECK_EQ(g_w.way, way);
				CHECK_EQ(g_w.target, a);
				check_log(a, sizeof x, way);
				CHECK(ff_slot_good(&g_f, a));
			}
		}
}

/*
 * A plain write of B with the running slot A damaged by a failed in-place
 * write (S-02.084): accepted, target B, then the selector settled on B
 * with verified 01. Cuts after each of its operations: A is already bad
 * and selected, so until B is good AND selected no reset of the factory
 * first stage starts anything. That is every cut before the settling (for
 * B = slot 1 the erase's FF FF names B already; for B = slot 0 the index
 * program alone settles it). A conforming first stage falls back from the bad
 * selected slot, so it starts B from the record's program on.
 */
static void test_damaged_plain(void)
{
	for (unsigned a = 0; a < 2u; a++) {
		const uint8_t index = a == 0u ? 0x00 : 0x01;
		const unsigned b = 1u - a;
		struct fr_running run = running_of(index);
		/* the slot's ops, then the settling from {A, 01} on B: the index
		 * program alone for B = slot 0, an erase and two programs for B =
		 * slot 1 */
		const unsigned m_total = SLOT_OPS + (settle_kind((unsigned)index | 0x0100u, b) == 1u ?
						     1u : 3u);
		static struct fake_flash damaged;
		struct fr_home home;

		make_image(N_IMAGE, 150u + a);
		for (unsigned k = 0; k <= m_total; k++) {
			setup(index, 0x01);
			ff_set_slot(&g_f, b, "badcrc", OLD_TARGET_LEN, 7u);
			snapshot();
			g_home.other_failed = true;
			g_flags = FR_BEGIN_F_RUNNING;
			g_erase_then_fail = 1;
			CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
			CHECK(g_home.spoiled && !ff_slot_good(&g_f, a));
			memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
			memcpy(damaged.mem, g_f.mem, sizeof g_f.mem);
			ff_reset(&g_f);
			g_erase_then_fail = 0;
			g_flags = 0;
			home = g_home;
			g_f.cut_after = k;	/* 0: no cut */
			if (k == 0u) {
				CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
				CHECK_EQ(g_w.target, b);
				CHECK_EQ(g_w.way, FR_WAY_PLAIN);
				g_split = true;		/* A is damaged: S-02.084 */
				check_log(b, N_IMAGE, FR_WAY_PLAIN);
				CHECK(ff_slot_good(&g_f, b));
				CHECK(g_f.mem[FR_SELECTOR_OFF] == b && g_f.mem[FR_SELECTOR_OFF + 1u] == 1u);
				/* A untouched by it */
				CHECK(memcmp(g_f.mem + FR_SLOT_BASE(a), damaged.mem + FR_SLOT_BASE(a),
					     FR_SLOT_CAP) == 0);
				/* the write ended well: the selector settled */
				home.unsettled = false;
				CHECK(memcmp(&home, &g_home, sizeof home) == 0);
				CHECK_EQ(boot_once(0), b);
				continue;
			}
			(void)write_all(N_IMAGE, g_sha, &run);
			CHECK_EQ(g_f.nmut, k);
			memcpy(g_cut.mem, g_f.mem, sizeof g_f.mem);
			{
				const bool b_good = ff_slot_good(&g_f, b);
				const bool b_sel = fr_selected_slot(g_f.mem[FR_SELECTOR_OFF]) == b;

				CHECK(run_resets(0, 1u, 0u, 0u) == (b_good && b_sel));
				if (k < SLOT_OPS)
					CHECK(!(b_good && b_sel));	/* nothing boots */
				memcpy(g_f.mem, g_cut.mem, sizeof g_f.mem);
				CHECK(run_resets(1, 1u, 0u, 0u) == b_good);
			}
		}
	}
}

/* --- S-02.084: with the running slot damaged, the selector a byte at a time --- */

/* The rescue writes that program the selector. */
enum { K_PLAIN_B, K_ONLY_SAME, K_SELECT_FIRST, K_CLOSE, K_RETRY, K_KINDS };

struct kind_want {
	unsigned target, way;
	unsigned nsel;			/* selector writes */
	uint8_t sel[2][2];		/* their {index, verified}, in order */
};

static uint8_t g_x[CUR_LEN];		/* the image slot B holds */

/* A damaged by an in-place write whose first erase took and failed, with
 * B bad: the selector still {A's index, 01}. */
static void damage_in_place(uint8_t index, unsigned a)
{
	struct fr_running run = running_of(index);

	setup(index, 0x01);
	ff_set_slot(&g_f, 1u - a, "badcrc", OLD_TARGET_LEN, 7u);
	snapshot();
	g_home.other_failed = true;
	g_flags = FR_BEGIN_F_RUNNING;
	g_erase_then_fail = 1;
	make_image(N_IMAGE, 160u + a);
	CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_FLASH);
	g_erase_then_fail = 0;
}

/* The state before kind k's write with A = slot a, and what it must do;
 * g_img, g_sha and g_flags set for it, the op log cleared. */
static struct kind_want prepare(unsigned k, unsigned a)
{
	const uint8_t index = a == 0u ? 0x00 : 0x01;
	const unsigned b = 1u - a;
	struct kind_want want;

	memset(&want, 0, sizeof want);
	/* S-02.084: every write in this state settles the selector with
	 * verified 01 */
	want.sel[0][1] = want.sel[1][1] = 0x01u;
	switch (k) {
	case K_PLAIN_B:		/* B bad: a plain write of B, then {B, 01} */
		damage_in_place(index, a);
		g_flags = 0;
		make_image(N_IMAGE, 170u + a);
		want.target = b;
		want.way = FR_WAY_PLAIN;
		want.nsel = 1;
		want.sel[0][0] = (uint8_t)b;
		break;
	case K_ONLY_SAME:	/* B good with its image: only {B, 01} */
	case K_SELECT_FIRST:	/* the same state, the flag: {B, 01}, A, {A, 01} */
		damage_in_place(index, a);
		(void)build_image(g_x, sizeof g_x, 180u + a);
		put_slot(b, g_x, sizeof g_x);
		if (k == K_ONLY_SAME) {
			g_flags = 0;
			memcpy(g_img, g_x, sizeof g_x);
			sha256(g_sha, g_img, sizeof g_x);
			want.target = b;
			want.way = FR_WAY_PLAIN;
			want.nsel = 1;
			want.sel[0][0] = (uint8_t)b;
		} else {
			g_flags = FR_BEGIN_F_RUNNING;
			make_image(N_IMAGE, 190u + a);
			want.target = a;
			want.way = FR_WAY_SELECT_FIRST;
			want.nsel = 2;
			want.sel[0][0] = (uint8_t)b;
			want.sel[0][1] = 0x01u;
			want.sel[1][0] = (uint8_t)a;
		}
		break;
	case K_CLOSE:		/* B bad, the selector {B, 00}: in place, then {A, 01} */
		damage_in_place(index, a);
		g_f.mem[FR_SELECTOR_OFF] = (uint8_t)b;
		g_f.mem[FR_SELECTOR_OFF + 1u] = 0x00u;
		g_flags = FR_BEGIN_F_RUNNING;
		make_image(N_IMAGE, 200u + a);
		want.target = a;
		want.way = FR_WAY_IN_PLACE;
		want.nsel = 1;
		want.sel[0][0] = (uint8_t)a;
		break;
	default:		/* K_RETRY: {B, 01}, B good, A damaged: A, then {A, 01} */
		setup(index, 0x01);
		g_flags = FR_BEGIN_F_RUNNING;
		g_erase_then_fail = 3;
		make_image(N_IMAGE, 210u + a);
		CHECK_EQ(write_all(N_IMAGE, g_sha, &(struct fr_running){ true, (uint8_t)a }),
			 FR_WR_FLASH);
		g_erase_then_fail = 0;
		CHECK(g_f.mem[FR_SELECTOR_OFF] == b && g_f.mem[FR_SELECTOR_OFF + 1u] == 0x01u);
		make_image(N_IMAGE, 211u + a);
		want.target = a;
		want.way = FR_WAY_PLAIN;
		want.nsel = 1;
		want.sel[0][0] = (uint8_t)a;
		break;
	}
	CHECK(g_home.spoiled && !ff_slot_good(&g_f, a));
	memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
	ff_reset(&g_f);
	g_nselb = 0;
	return want;
}

/* The split programs in each rescue write's op log; one 2-byte program
 * outside the rescue state. */
static void test_split_programs(void)
{
	for (unsigned a = 0; a < 2u; a++)
		for (unsigned k = 0; k < K_KINDS; k++) {
			struct fr_running run = running_of(a == 0u ? 0x00 : 0x01);
			const struct kind_want want = prepare(k, a);
			unsigned at = 0, seen = 0;

			CHECK_EQ(write_all(k == K_ONLY_SAME ? sizeof g_x : N_IMAGE, g_sha, &run), FR_WR_OK);
			CHECK_EQ(g_w.target, want.target);
			CHECK_EQ(g_w.way, want.way);
			g_split = true;
			(void)at;
			(void)seen;
			for (unsigned i = 0; i < g_f.nlog; i++)
				if (g_f.log[i].kind == FF_PROGRAM && g_f.log[i].off >= FR_SELECTOR_OFF &&
				    g_f.log[i].off < FR_SELECTOR_OFF + FR_SECTOR)
					CHECK_EQ(g_f.log[i].len, 1);
			if (k == K_ONLY_SAME) {
				/* nothing of B or its record: the selector alone,
				 * settled on B */
				const unsigned pre = g_start.mem[FR_SELECTOR_OFF] |
						     (unsigned)g_start.mem[FR_SELECTOR_OFF + 1u] << 8;
				const unsigned m = first_sel_mut(0);

				for (unsigned i = 0; i < g_f.nlog; i++)
					if (g_f.log[i].kind != FF_READ)
						CHECK(g_f.log[i].off >= FR_SELECTOR_OFF &&
						      g_f.log[i].off < FR_SELECTOR_OFF + 2u);
				CHECK(m != ~0u);
				if (m != ~0u)
					CHECK_EQ(check_settle(m, pre, want.target), g_f.nlog);
			} else {
				check_log(want.target, N_IMAGE, want.way);
			}
			CHECK(g_f.mem[FR_SELECTOR_OFF] == want.sel[want.nsel - 1u][0] &&
			      g_f.mem[FR_SELECTOR_OFF + 1u] == 0x01u);
			CHECK(ff_slot_good(&g_f, want.target));
			CHECK_EQ(boot_once(0), want.target);
		}

	/* outside the rescue state: one 2-byte program each */
	for (unsigned way = 0; way < 2u; way++) {
		struct fr_running run = running_of(0x01);

		setup(0x01, 0x01);
		g_flags = way ? FR_BEGIN_F_RUNNING : 0u;
		make_image(N_IMAGE, 220u + way);
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
		for (unsigned i = 0; i < g_f.nlog; i++)
			if (g_f.log[i].kind == FF_PROGRAM && g_f.log[i].off >= FR_SELECTOR_OFF &&
			    g_f.log[i].off < FR_SELECTOR_OFF + FR_SECTOR)
				CHECK(g_f.log[i].off == FR_SELECTOR_OFF && g_f.log[i].len == 2u);
		CHECK_EQ(g_nsel, way ? 2u : 1u);
	}

	/* the index read back wrong, or not at all: the write stops before the
	 * verified byte, 11, or 14 for the first selector write */
	for (unsigned a = 0; a < 2u; a++)
		for (unsigned mode = 1; mode <= 2u; mode++)
			for (unsigned k = K_PLAIN_B; k <= K_SELECT_FIRST; k += 2u) {
				struct fr_running run = running_of(a == 0u ? 0x00 : 0x01);

				(void)prepare(k, a);
				g_idx_mode = mode;
				CHECK_EQ(write_all(N_IMAGE, g_sha, &run),
					 k == K_SELECT_FIRST ? FR_WR_SELECT_FIRST : FR_WR_SELECTOR);
				CHECK_EQ(find_op(FF_PROGRAM, FR_SELECTOR_OFF + 1u, 0), ~0u);
				if (k == K_SELECT_FIRST)	/* nothing of A written */
					CHECK_EQ(find_op(FF_ERASE, FR_SLOT_BASE(a), 0), ~0u);
				g_idx_mode = 0;
			}
}

/*
 * The proof by cuts, from every start (S-02.084, S-02.020 to S-02.026).
 *
 * A selector value is s = index | verified << 8. The factory first stage
 * (S-02.020 to S-02.026), as a model: verified 01 is trusted, the named
 * slot booted with no check and no write. Else the named slot is checked
 * (its tag, then the CRC over its length), the selector erased and
 * programmed {index, 01} when the check passes or {other, 01} when it
 * fails, and the named slot booted (one start late). Its own write can be
 * cut: the erase torn to any superset of the old bits, the 2-byte program
 * torn to any value that keeps the bits to stay set (any superset of the
 * value written). A check of a bad slot whose record is not sure to fail
 * (a length beyond the slot, S-02.026) may pass or fail; an empty record
 * passes (S-02.021).
 *
 * For a slot state: U(s), a start from s reaches a good slot with the
 * first stage's own writes uncut; G(s), it does however they are cut, at
 * every later start too (no path of cuts reaches a trusted bad slot).
 * The slots do not change in this phase, so G is a fixed point over the
 * 65,536 values, computed with a superset-OR transform.
 */
#define SEL_N 0x10000u

/*
 * Record kinds: good (S-02.016); an old record over another image; erased;
 * a length of 0x80000000 or more (unspecified, S-02.026: passes or fails);
 * empty (the tag, length 0, CRC 0: the check passes it, S-02.021); a CRC
 * that differs; a length beyond the slot below 0x80000000 (judged by its
 * CRC up to the model's limit: passes or fails); and a whole image under a
 * record that may pass or fail (a cut of the record's own operations, the
 * image's bytes all programmed: booting it boots a whole image).
 */
enum { RK_GOOD, RK_OLD, RK_ERASED, RK_TORN, RK_EMPTY, RK_BADCRC, RK_LONG, RK_WHOLE, RK_KINDS };

/* the factory check's outcomes for a slot's record kind */
static bool rk_can_pass(unsigned k)
{
	return k == RK_GOOD || k == RK_TORN || k == RK_EMPTY || k == RK_LONG || k == RK_WHOLE;
}

static bool rk_can_fail(unsigned k)
{
	return k != RK_GOOD && k != RK_EMPTY;
}

/* booting the slot boots a whole image */
static bool rk_boots(unsigned k)
{
	return k == RK_GOOD || k == RK_WHOLE;
}

static struct fr_record rk_record(unsigned k)
{
	struct fr_record r;

	r.crc32 = 0x12345678u;
	r.length = 0x40u;
	r.tag = FR_TAG;
	if (k == RK_ERASED)
		r.crc32 = r.length = r.tag = 0xFFFFFFFFu;
	else if (k == RK_TORN)
		r.length = 0x80000000u;		/* beyond the slot */
	else if (k == RK_LONG)
		r.length = 0x7FFFFFFFu;
	else if (k == RK_EMPTY)
		r.crc32 = r.length = 0;
	return r;
}

struct model {
	unsigned kind[2];		/* RK_* of each slot */
	bool good[2];
	uint8_t bad[SEL_N];		/* 1: a path of cuts reaches a trusted bad slot */
	uint8_t lost_sup[SEL_N];	/* some superset t: the uncut start reaches none */
	uint32_t dbl_sup[SEL_N];	/* supersets t: uncut reaches, cut may not */
};

static bool model_u(const struct model *m, unsigned s);

static uint8_t sup[SEL_N];

static void model_build(struct model *m, unsigned k0, unsigned k1)
{
	bool changed = true;

	m->kind[0] = k0;
	m->kind[1] = k1;
	m->good[0] = rk_boots(k0);
	m->good[1] = rk_boots(k1);
	for (unsigned s = 0; s < SEL_N; s++) {
		const unsigned n = (s & 0xFFu) == 0u ? 0u : 1u;

		m->bad[s] = (uint8_t)((s >> 8) == 0x01u && !m->good[n]);
	}
	while (changed) {
		changed = false;
		memcpy(sup, m->bad, sizeof sup);
		for (unsigned bit = 1; bit < SEL_N; bit <<= 1)
			for (unsigned s = 0; s < SEL_N; s++)
				if (!(s & bit))
					sup[s] |= sup[s | bit];
		for (unsigned s = 0; s < SEL_N; s++) {
			const unsigned n = (s & 0xFFu) == 0u ? 0u : 1u;
			uint8_t v;

			if ((s >> 8) == 0x01u || m->bad[s])
				continue;
			/* a torn erase: any superset of s, s itself included */
			v = sup[s];
			/* the program from FF FF: any superset of what it writes */
			if (rk_can_pass(m->kind[n]))
				v |= sup[(s & 0xFFu) | 0x0100u];
			if (rk_can_fail(m->kind[n]))
				v |= sup[(n == 0u ? 0x01u : 0x00u) | 0x0100u];
			if (v) {
				m->bad[s] = 1;
				changed = true;
			}
		}
	}
	/* what a torn erase of p can reach: over every superset of p */
	for (unsigned s = 0; s < SEL_N; s++) {
		m->lost_sup[s] = (uint8_t)!model_u(m, s);
		m->dbl_sup[s] = model_u(m, s) && m->bad[s] ? 1u : 0u;
	}
	for (unsigned bit = 1; bit < SEL_N; bit <<= 1)
		for (unsigned s = 0; s < SEL_N; s++)
			if (!(s & bit)) {
				m->lost_sup[s] |= m->lost_sup[s | bit];
				m->dbl_sup[s] += m->dbl_sup[s | bit];
			}
}

static bool model_u(const struct model *m, unsigned s)
{
	const unsigned n = (s & 0xFFu) == 0u ? 0u : 1u;

	if ((s >> 8) == 0x01u)
		return m->good[n];
	return (!rk_can_pass(m->kind[n]) || m->good[n]) &&
	       (!rk_can_fail(m->kind[n]) || m->good[1u - n]);
}

static bool model_g(const struct model *m, unsigned s)
{
	return !m->bad[s];
}

/* A cache of models by the two kinds. */
static struct model g_models[RK_KINDS][RK_KINDS];
static bool g_model_built[RK_KINDS][RK_KINDS];

static const struct model *model_of(unsigned k0, unsigned k1)
{
	if (!g_model_built[k0][k1]) {
		model_build(&g_models[k0][k1], k0, k1);
		g_model_built[k0][k1] = true;
	}
	return &g_models[k0][k1];
}

/* S-02.084: which slot a start reaches. rec: the named slot's record. */
static unsigned rule_reach(unsigned s, const bool good[2], const struct fr_record *rec)
{
	const unsigned n = (s & 0xFFu) == 0u ? 0u : 1u, v = s >> 8;
	const bool sure = rec->tag != FR_TAG || (rec->length >= 1u && rec->length <= 0x78000u);

	if (good[n] && (v == 0x01u || n == 1u || good[1]))
		return FR_REACH_NAMED;
	if (!good[n] && n == 0u && sure && (v & 0xFEu) != 0u && good[1])
		return FR_REACH_FALLBACK;
	return FR_REACH_NONE;
}

/* S-02.021: a record whose check the factory first stage is sure to fail. */
static bool rule_fails(const struct fr_record *r)
{
	return r->tag != FR_TAG || (r->length >= 1u && r->length <= 0x78000u);
}

/* S-02.084: B good and the selector leading to it: it names B, whatever
 * verified reads; or it names A with verified exactly 00 and A's record is
 * sure to fail (a first stage falls back to B, and an erase of that value,
 * cut short, could read 01 over A's index). The fallback (verified with a
 * bit besides bit 0) is not counted: there B is selected first. rec_a: A's
 * record; B is taken good. */
static bool rule_leads_to_b(unsigned s, unsigned a, const struct fr_record *rec_a)
{
	const unsigned b = 1u - a, n = (s & 0xFFu) == 0u ? 0u : 1u;

	return n == b || ((s >> 8) == 0x00u && rule_fails(rec_a));
}

/* A simpler rule, the control: slot 0 named with verified not 01 counted
 * with "good[0]", always true there. */
static unsigned old_reach(unsigned s, const bool good[2], const struct fr_record *rec)
{
	const unsigned n = (s & 0xFFu) == 0u ? 0u : 1u, v = s >> 8;
	const bool sure = rec->tag != FR_TAG || (rec->length >= 1u && rec->length <= 0x78000u);

	if (good[n] && (v == 0x01u || n == 1u || good[0]))
		return FR_REACH_NAMED;
	if (!good[n] && n == 0u && sure && (v & 0xFEu) != 0u && good[1])
		return FR_REACH_FALLBACK;
	return FR_REACH_NONE;
}

static unsigned code_reach(unsigned s, const bool good[2], const struct fr_record *rec)
{
	struct fr_selector sel;

	sel.index = (uint8_t)s;
	sel.verified = (uint8_t)(s >> 8);
	return fr_reach(&sel, good, rec);
}

static unsigned g_conservative;
static bool settled(unsigned p, unsigned t);	/* states the model reaches that fr_reach() calls none */

/* (iii) fr_reach() against the model, over all values, every slot state */
static void test_reach_proof(void)
{
	static const unsigned kinds[] = { RK_GOOD, RK_OLD, RK_ERASED, RK_TORN, RK_EMPTY, RK_BADCRC,
					  RK_LONG };
	unsigned old_wrong = 0;

	g_conservative = 0;
	for (unsigned i = 0; i < N(kinds); i++)
		for (unsigned j = 0; j < N(kinds); j++) {
			const struct model *m = model_of(kinds[i], kinds[j]);
			const struct fr_record rec[2] = { rk_record(kinds[i]), rk_record(kinds[j]) };

			for (unsigned s = 0; s < SEL_N; s++) {
				const unsigned n = (s & 0xFFu) == 0u ? 0u : 1u;
				const unsigned got = code_reach(s, m->good, &rec[n]);

				CHECK_EQ(got, rule_reach(s, m->good, &rec[n]));
				if (got != FR_REACH_NONE) {
					CHECK(model_g(m, s));		/* the proof */
					CHECK(model_u(m, s));
				} else if (model_g(m, s) && (m->good[0] || m->good[1])) {
					g_conservative++;
				}
				if (old_reach(s, m->good, &rec[n]) != FR_REACH_NONE && !model_g(m, s))
					old_wrong++;
			}
		}
	/* the control: the simpler rule is not safe */
	CHECK(old_wrong > 0u);
	printf("  reach proof: %u states reached that fr_reach() leaves unclaimed; "
	       "the old rule claimed %u it should not\n", g_conservative, old_wrong);
}

/* --- The front's selector operations, from every start --- */

/* What a selector operation of the front can leave: every value it may
 * reach when torn, the uncut one included. */
enum { OP_ERASE, OP_INDEX, OP_VERIFIED, OP_PAIR, OP_PIDX, OP_PVER };

enum { PH_FIRST, PH_SETTLE, PH_CONTROL, PH_RETRY, PHS };

struct op_count {
	unsigned ops;			/* judged */
	unsigned long long values;
	unsigned u_loss[PHS];		/* by phase: a single cut of the front's operation
					 * after which the uncut first stage reaches no good
					 * slot, from a pre-state where it reached one */
	unsigned g_loss;		/* the same from a pre-state reached however the
					 * first stage's own write is cut */
	unsigned doubles;		/* robust lost, the uncut first stage still reaches */
	unsigned by[PHS][2][RK_KINDS][RK_KINDS];	/* losses by phase, A, A's kind, B's */
	unsigned settle_class[4];	/* settling losses: the value erased trusted a
					 * bad slot; no program could settle it; else; a
					 * program alone (no erase) */
};

static struct op_count g_cnt;
static bool g_quiet;
static bool g_shown[PHS], g_else_shown;
static const struct model *g_jm;	/* the model of the operation judged */
static unsigned g_settle_tgt;		/* the settling's target */

/*
 * A failed step of a settling (an erase, a program or a read-back that
 * fails, or a program not stored) leaves the selector at one of the values
 * the step can leave torn, the old one included, and the slots as the
 * write left them: A good with the selector open and B bad (the state
 * goes on until a write has ended well, S-02.084), or
 * A bad and B good. Each such state is enumerated over every selector
 * value below; these record which values a failure leaves in which state,
 * and which values of a state failed a check.
 */
#define X1_STATES 8u			/* A good and unsettled, B bad: a, B's kind */
#define X_STATES (X1_STATES + 10u)	/* then A bad, B good: a, A's kind */
#define NO_STATE (~0u)

static unsigned g_fstate = NO_STATE;	/* the state a failure of the step judged leaves */
static uint8_t g_left[X_STATES][SEL_N];	/* a failed program or read leaves this value */
static uint8_t g_efrom[X_STATES][SEL_N];	/* a failed erase of this value: any superset */
static uint8_t g_notok[X_STATES][SEL_N];	/* the state at this value failed a check */
static bool g_done[X_STATES];

static const unsigned g_bbad[] = { RK_BADCRC, RK_EMPTY, RK_TORN, RK_LONG };
static const unsigned g_akinds[] = { RK_OLD, RK_ERASED, RK_TORN, RK_EMPTY, RK_LONG };

static unsigned x1_state(unsigned a, unsigned kbi)
{
	return a * (unsigned)N(g_bbad) + kbi;
}

static unsigned x2_state(unsigned a, unsigned kai)
{
	return X1_STATES + a * (unsigned)N(g_akinds) + kai;
}

static void loss(unsigned ph, bool g0, unsigned a, unsigned kb, unsigned ka, unsigned s,
		 unsigned req, unsigned op, unsigned p, unsigned t)
{
	static const char *const names[] = { "erase", "index", "verified", "pair",
					     "index program alone", "verified program alone" };
	static const char *const phases[] = { "before the slot write", "at the settling",
					      "control", "at the retry's settling" };

	g_cnt.u_loss[ph]++;
	g_cnt.by[ph][a][ka][kb]++;
	if ((ph == PH_SETTLE || ph == PH_RETRY) && op != OP_ERASE) {
		g_cnt.settle_class[3]++;
		printf("  SETTLE PROGRAM LOSS: A = slot %u, start %02X %02X, target %u, %s of "
		       "%02X %02X torn to %02X %02X\n", a, s & 0xFFu, s >> 8, g_settle_tgt, names[op],
		       p & 0xFFu, p >> 8, t & 0xFFu, t >> 8);
	} else if (ph == PH_SETTLE || ph == PH_RETRY) {
		const unsigned idx = p & 0xFFu, v = p >> 8, n = idx == 0u ? 0u : 1u;
		const unsigned tg = g_settle_tgt;

		if (v == 0x01u && !g_jm->good[n]) {
			g_cnt.settle_class[0]++;	/* trusted a bad slot */
		} else if ((idx & tg) != tg || (v & 0x01u) == 0u) {
			g_cnt.settle_class[1]++;	/* no program reaches {tg, 01} */
		} else {
			g_cnt.settle_class[2]++;
			if (!g_else_shown) {
				g_else_shown = true;
				printf("  SETTLE ELSE: A = slot %u, start %02X %02X, target %u, "
				       "erase of %02X %02X torn to %02X %02X\n", a, s & 0xFFu, s >> 8,
				       tg, p & 0xFFu, p >> 8, t & 0xFFu, t >> 8);
			}
		}
	}
	if (g0)
		g_cnt.g_loss++;
	if (!g_quiet && !g_shown[ph]) {
		g_shown[ph] = true;
		printf("  LOST (%s): A = slot %u, B kind %u, A kind %u, start %02X %02X, "
		       "request %u, %s of %02X %02X torn to %02X %02X%s\n", phases[ph], a, kb, ka,
		       s & 0xFFu, s >> 8, req, names[op], p & 0xFFu, p >> 8, t & 0xFFu, t >> 8,
		       g0 ? " (from a robust state)" : "");
	}
}

/*
 * One operation of the front from pre p, toward idx/ver, under model m:
 * every torn result t, the uncut one included. (ii): if the uncut first
 * stage reaches a good slot from p, it does from every t (else a loss);
 * where every cut of the first stage's own writes reaches one from p but
 * not from some t that the uncut first stage still follows, a double
 * fault.
 */
static void judge_op(const struct model *m, unsigned ph, unsigned op, unsigned p, uint8_t idx,
		     uint8_t ver, unsigned a, unsigned kb, unsigned ka, unsigned s, unsigned req)
{
	const bool u0 = model_u(m, p), g0 = model_g(m, p);

	g_jm = m;
	g_cnt.ops++;
	if (op == OP_ERASE) {
		/* any superset of the old bits: through the transforms */
		unsigned zeros = 0;

		for (unsigned bit = 1; bit < SEL_N; bit <<= 1)
			zeros += (p & bit) ? 0u : 1u;
		g_cnt.values += 1ull << zeros;
		if (g_fstate != NO_STATE)
			g_efrom[g_fstate][p] = 1;
		if (u0 && m->lost_sup[p]) {
			unsigned t = p;

			for (unsigned x = 0; x < SEL_N; x++)
				if ((x & p) == p && !model_u(m, x)) {
					t = x;
					break;
				}
			loss(ph, g0, a, kb, ka, s, req, op, p, t);
		}
		if (g0)
			g_cnt.doubles += m->dbl_sup[p];
		return;
	}
	for (unsigned x = 0; x < (op == OP_PAIR ? SEL_N : 0x100u); x++) {
		unsigned t;

		if (op == OP_PIDX) {
			/* a program alone, the index toward 00 from p's: every value
			 * between them, verified as it was */
			if ((x & ~(p & 0xFFu)) != 0u)
				continue;
			t = x | (p & 0xFF00u);
		} else if (op == OP_PVER) {
			/* verified toward 01 from p's, under index 00 */
			if ((x & ~(p >> 8)) != 0u || (x & 0x01u) == 0u || (p & 0xFFu) != 0u)
				continue;
			t = x << 8;
		} else if (op == OP_INDEX) {	/* from FF FF: the index keeps the target's bits */
			t = x | 0xFF00u;
		} else if (op == OP_VERIFIED) {	/* under the whole index */
			t = idx | x << 8;
		} else {			/* one 2-byte program from FF FF */
			t = x;
		}
		if (op != OP_PIDX && op != OP_PVER &&
		    (((t & 0xFFu) & idx) != idx || ((t >> 8) & ver) != ver))
			continue;
		g_cnt.values++;
		if (g_fstate != NO_STATE)
			g_left[g_fstate][t] = 1;
		if (u0 && !model_u(m, t))
			loss(ph, g0, a, kb, ka, s, req, op, p, t);
		else if (g0 && !model_g(m, t))
			g_cnt.doubles++;
	}
}

/* The flash for the enumeration: A = slot a with a record of kind ka over
 * 64 bytes (good with its image for RK_GOOD), B with 64 bytes and a record
 * of kind kb; home spoiled with the front A had, and unsettled. */
static struct fr_home g_ehome;

static void put_kind(unsigned k, unsigned kind, uint32_t seed)
{
	const struct fr_record r = rk_record(kind);

	ff_set_slot(&g_f, k, "good", 0x40u, seed);
	if (kind == RK_GOOD)
		return;
	if (kind == RK_BADCRC) {
		g_f.mem[FR_SLOT_BASE(k) + 0x10u] ^= 0x01u;	/* its CRC differs */
		return;
	}
	memset(g_f.mem + FR_RECORD_OFF(k), 0xFF, FR_SECTOR);
	ff_put32(g_f.mem + FR_RECORD_OFF(k), r.crc32);
	ff_put32(g_f.mem + FR_RECORD_OFF(k) + 4u, r.length);
	ff_put32(g_f.mem + FR_RECORD_OFF(k) + 8u, r.tag);
}

static void enum_flash(unsigned a, unsigned ka, unsigned kb)
{
	const unsigned b = 1u - a;
	uint8_t pin[FR_SHA256_LEN];
	uint32_t fcrc;

	/* the front A had, from setup()'s image */
	setup(a == 0u ? 0x00 : 0x01, 0x01);
	{
		struct fr_slot_info ai;

		CHECK_EQ(fr_slot_info(flash(), a, &ai), FR_FLASH_OK);
		memcpy(pin, ai.desc.pin, sizeof pin);
		fcrc = ai.desc.front_crc;
	}
	if (ka == RK_OLD) {
		/* an old record over another image: its CRC differs */
		put_kind(a, RK_BADCRC, 301u);
	} else if (ka == RK_ERASED) {
		memset(g_f.mem + FR_SLOT_BASE(a), 0xFF, FR_SLOT_CAP);
		ff_fill_image(g_f.mem + FR_SLOT_BASE(a), 0x40u, 301u);
		memset(g_f.mem + FR_RECORD_OFF(a), 0xFF, FR_SECTOR);
	} else {
		put_kind(a, ka, 301u);
	}
	put_kind(b, kb, 302u);
	memset(&g_ehome, 0, sizeof g_ehome);
	g_ehome.spoiled = true;
	g_ehome.unsettled = true;
	g_ehome.other_failed = true;
	memcpy(g_ehome.pin, pin, sizeof pin);
	g_ehome.front_crc = fcrc;
	CHECK(ff_slot_good(&g_f, a) == (ka == RK_GOOD));
	CHECK(ff_slot_good(&g_f, b) == (kb == RK_GOOD));
	ff_reset(&g_f);
}

/* What INFO and the hold are decided from (fr_proto.c), over the flash as
 * enum_flash() left it; the selector set per value. */
static struct fr_rec_status g_est;

static void enum_status(unsigned a)
{
	memset(&g_est, 0, sizeof g_est);
	g_est.running.established = true;
	g_est.running.slot = (uint8_t)a;
	g_est.home = g_ehome;
	fr_rec_survey(&g_est, flash());
	CHECK(g_est.selector_ok && !g_est.read_error);
	ff_reset(&g_f);
}

/* S-02.084: a selector the settling leaves as it is */
static bool settled(unsigned p, unsigned t)
{
	return ((p & 0xFFu) == 0u ? 0u : 1u) == t && ((p >> 8) == 0x01u || t == 1u);
}

enum { REQ_OWN, REQ_OTHER, REQ_FLAG, REQS };

/* S-02.084's settling on t from p, every operation judged: nothing; the
 * programs alone (the index to 00, then verified to 01); or the erase and
 * the split programs. The value it leaves. */
static unsigned judge_settle(const struct model *m, unsigned ph, unsigned p, unsigned t,
			     unsigned a, unsigned kb, unsigned ka, unsigned s, unsigned req)
{
	switch (settle_kind(p, t)) {
	case 0:
		/* a failed read of the selector leaves it as it is */
		if (g_fstate != NO_STATE)
			g_left[g_fstate][p] = 1;
		return p;
	case 1:
		if ((p & 0xFFu) != 0u) {
			judge_op(m, ph, OP_PIDX, p, 0, 0, a, kb, ka, s, req);
			p &= 0xFF00u;
		}
		if ((p >> 8) != 0x01u)
			judge_op(m, ph, OP_PVER, p, 0, 0, a, kb, ka, s, req);
		return 0x0100u;
	default:
		judge_op(m, ph, OP_ERASE, p, 0, 0, a, kb, ka, s, req);
		judge_op(m, ph, OP_INDEX, 0xFFFFu, (uint8_t)t, 0, a, kb, ka, s, req);
		judge_op(m, ph, OP_VERIFIED, 0xFFFFu, (uint8_t)t, 0x01u, a, kb, ka, s, req);
		return t | 0x0100u;
	}
}

/* (iv): settled on t under model m: reached, no first-stage write left or
 * naming slot 1, and robust */
static void check_settled(const struct model *m, unsigned p)
{
	const unsigned nf = (p & 0xFFu) == 0u ? 0u : 1u;
	const struct fr_record rf = rk_record(RK_GOOD);

	CHECK(rule_reach(p, m->good, &rf) == FR_REACH_NAMED);
	CHECK((p >> 8) == 0x01u || nf == 1u);
	CHECK(model_g(m, p));
}

/* The status at selector value q: the damaged state, and the hold where no
 * start reaches a good slot (S-02.084, S-02.093's bits 5 and 7). */
static void check_hold(unsigned q, unsigned reach)
{
	g_est.sel.index = (uint8_t)q;
	g_est.sel.verified = (uint8_t)(q >> 8);
	CHECK(fr_rec_rescue(&g_est));
	CHECK(fr_rec_held(&g_est) == (reach == FR_REACH_NONE));
	CHECK_EQ(fr_rec_reach(&g_est), reach);
	fr_rec_target(&g_est);
}

static unsigned g_by_programs, g_fallback_first;

/*
 * The state a write of A leaves when it read A back good and its settling
 * failed while B is bad: A good, the selector open at any value q. The
 * state stands (plan.rescue) and holds where no start reaches a good slot;
 * the flag with A's own image settles on A erasing nothing of A; without
 * it the write goes to B. Every value, and the retry's settling judged.
 */
static void enum_unsettled(void)
{
	for (unsigned a = 0; a < 2u; a++)
		for (unsigned kbi = 0; kbi < N(g_bbad); kbi++) {
			const unsigned b = 1u - a, kb = g_bbad[kbi], xs = x1_state(a, kbi);
			const struct fr_running run = running_of(a == 0u ? 0x00 : 0x01);
			unsigned k_a[2];
			const struct model *m_now, *m_both = model_of(RK_GOOD, RK_GOOD);
			const struct fr_record rec_a = rk_record(RK_GOOD), rec_b = rk_record(kb);

			k_a[a] = RK_GOOD;
			k_a[b] = kb;
			m_now = model_of(k_a[0], k_a[1]);
			enum_flash(a, RK_GOOD, kb);
			enum_status(a);
			for (unsigned q = 0; q < SEL_N; q++) {
				const unsigned n = (q & 0xFFu) == 0u ? 0u : 1u;
				const int f0 = g_failures;
				const unsigned reach = rule_reach(q, m_now->good, n == a ? &rec_a : &rec_b);
				struct fr_plan plans[2];
				struct fr_desc front;
				unsigned p;

				g_f.mem[FR_SELECTOR_OFF] = (uint8_t)q;
				g_f.mem[FR_SELECTOR_OFF + 1u] = (uint8_t)(q >> 8);
				CHECK_EQ(fr_writer_plan(flash(), &run, &g_ehome, 0, &plans[0], &front),
					 FR_WR_OK);
				CHECK_EQ(fr_writer_plan(flash(), &run, &g_ehome, FR_BEGIN_F_RUNNING,
							&plans[1], &front), FR_WR_OK);
				/* without the flag: B, plainly */
				CHECK(plans[0].rescue && plans[0].way == FR_WAY_PLAIN);
				CHECK(plans[0].target == b && !plans[0].only_same);
				/* with it: A's own image alone, settled on A */
				CHECK(plans[1].rescue && plans[1].way == FR_WAY_PLAIN);
				CHECK(plans[1].target == a && plans[1].only_same);
				check_hold(q, reach);
				CHECK(g_est.target == b && g_est.run_way == FR_WAY_PLAIN);
				/* the retry with the flag: nothing of A, the settling on A;
				 * a failure of it leaves this state again */
				g_settle_tgt = a;
				g_fstate = xs;
				p = judge_settle(m_now, PH_RETRY, q, a, a, kb, RK_GOOD, q, REQ_FLAG);
				g_fstate = NO_STATE;
				check_settled(m_now, p);
				/* without the flag: B written, both good, the settling on B */
				g_settle_tgt = b;
				p = judge_settle(m_both, PH_RETRY, q, b, a, kb, RK_GOOD, q, REQ_OTHER);
				check_settled(m_both, p);
				g_notok[xs][q] = (uint8_t)(g_failures != f0);
			}
			g_done[xs] = true;
		}
}

static void test_front_every_start(void)
{
	static const unsigned bkinds[] = { RK_GOOD, RK_BADCRC, RK_EMPTY, RK_TORN, RK_LONG };
	unsigned left = 0, efrom = 0;

	memset(&g_cnt, 0, sizeof g_cnt);
	memset(g_left, 0, sizeof g_left);
	memset(g_efrom, 0, sizeof g_efrom);
	memset(g_notok, 0, sizeof g_notok);
	memset(g_done, 0, sizeof g_done);
	g_by_programs = g_fallback_first = 0;
	enum_unsettled();
	for (unsigned a = 0; a < 2u; a++)
		for (unsigned kbi = 0; kbi < N(bkinds); kbi++)
			for (unsigned ki = 0; ki < N(g_akinds); ki++) {
				const unsigned b = 1u - a, ka = g_akinds[ki], kb = bkinds[kbi];
				const bool bg = kb == RK_GOOD;
				const unsigned xs = bg ? x2_state(a, ki) : NO_STATE;
				struct fr_running run = running_of(a == 0u ? 0x00 : 0x01);
				/* the slots: before (A damaged), after a write of A, of B */
				unsigned k_now[2], k_a[2];
				const struct model *m_now, *m_a, *m_b;
				const struct fr_record rec_a = rk_record(ka);
				/* a failed settling after a write of A: A good, B as it
				 * was (the state goes on where B is bad); after a write of
				 * B, or before the slot write: A bad, B good */
				unsigned x_after_a = NO_STATE;

				if (!bg)
					for (unsigned j = 0; j < N(g_bbad); j++)
						if (g_bbad[j] == kb)
							x_after_a = x1_state(a, j);
				k_now[a] = ka;
				k_now[b] = kb;
				k_a[a] = RK_GOOD;
				k_a[b] = kb;
				m_now = model_of(k_now[0], k_now[1]);
				m_a = model_of(k_a[0], k_a[1]);
				m_b = model_of(a == 0u ? ka : RK_GOOD, a == 0u ? RK_GOOD : ka);
				enum_flash(a, ka, kb);
				enum_status(a);
				for (unsigned s = 0; s < SEL_N; s++) {
					const unsigned n = (s & 0xFFu) == 0u ? 0u : 1u, v = s >> 8;
					const struct fr_record rn = n == a ? rec_a : rk_record(kb);
					/* S-02.084: B good and the selector leading to it */
					const bool leading = bg && rule_leads_to_b(s, a, &rec_a);
					/* the exception: B is slot 0, named, verified odd and
					 * not 01; programs alone settle it */
					const bool by_prog = bg && n == b && b == 0u && (v & 0x01u) != 0u &&
							     v != 0x01u;
					const unsigned reach0 = rule_reach(s, m_now->good, &rn);
					const int f0 = g_failures;
					struct fr_plan plans[2];
					struct fr_desc front;

					g_f.mem[FR_SELECTOR_OFF] = (uint8_t)s;
					g_f.mem[FR_SELECTOR_OFF + 1u] = (uint8_t)(s >> 8);
					CHECK_EQ(fr_writer_plan(flash(), &run, &g_ehome, 0, &plans[0], &front),
						 FR_WR_OK);
					CHECK_EQ(fr_writer_plan(flash(), &run, &g_ehome, FR_BEGIN_F_RUNNING,
								&plans[1], &front), FR_WR_OK);
					check_hold(s, reach0);
					CHECK_EQ(g_est.target, plans[0].target);
					CHECK_EQ(g_est.run_way, plans[1].way);
					if (by_prog) {
						/* held before: B named unverified, A bad */
						CHECK(reach0 == FR_REACH_NONE);
						g_by_programs++;
					}
					for (unsigned req = 0; req < REQS; req++) {
						const struct fr_plan plan = plans[req == REQ_FLAG ? 1 : 0];
						unsigned way, tgt, p = s;
						bool turned_down = false;
						const struct model *m_end;

						if (req == REQ_OWN && !bg)
							continue;	/* B holds no image */
						/* the expected plan */
						if (req == REQ_FLAG) {
							way = leading ? FR_WAY_PLAIN : bg ? FR_WAY_SELECT_FIRST :
							      FR_WAY_IN_PLACE;
							tgt = a;
						} else {
							way = FR_WAY_PLAIN;
							tgt = leading && !by_prog ? a : b;
						}
						CHECK_EQ(plan.way, way);
						CHECK_EQ(plan.target, tgt);
						CHECK(plan.rescue);
						CHECK(plan.only_same == (req != REQ_FLAG && bg && tgt == b));
						/* the requests' outcome: B's own image refused 13
						 * where the write goes to A (the kept slot's), taken
						 * alone where B is the target and good; another image
						 * refused 18 there */
						if (req == REQ_OWN && tgt == a)
							turned_down = true;
						if (req == REQ_OTHER && plan.only_same)
							turned_down = true;
						/* (i): from a start that reaches a good slot, no
						 * selector write while A is damaged, but the settling
						 * on B (select first, or B's own image) where the
						 * fallback reaches B: that erase cannot read verified
						 * 01, verified having a bit set besides bit 0 */
						if (reach0 != FR_REACH_NONE && !turned_down && tgt != a)
							CHECK(reach0 == FR_REACH_FALLBACK && (v & 0xFEu) != 0u);
						if (reach0 != FR_REACH_NONE && plan.way == FR_WAY_SELECT_FIRST)
							CHECK(reach0 == FR_REACH_FALLBACK && (v & 0xFEu) != 0u);
						if (turned_down)
							continue;
						if (plan.way == FR_WAY_SELECT_FIRST) {
							/* the same settling, on B, before the slot write;
							 * a failure leaves A bad and B good */
							if (reach0 == FR_REACH_FALLBACK)
								g_fallback_first++;
							g_settle_tgt = b;
							g_fstate = x2_state(a, ki);
							p = judge_settle(m_now, PH_FIRST, p, b, a, kb, ka, s, req);
							CHECK(p == (b | 0x0100u));
						}
						/* the settling, the target read back good */
						m_end = tgt == a ? m_a : m_b;
						g_settle_tgt = tgt;
						g_fstate = tgt == a ? x_after_a : x2_state(a, ki);
						p = judge_settle(m_end, PH_SETTLE, p, tgt, a, kb, ka, s, req);
						g_fstate = NO_STATE;
						check_settled(m_end, p);
					}
					if (xs != NO_STATE)
						g_notok[xs][s] = (uint8_t)(g_failures != f0);
				}
				if (xs != NO_STATE)
					g_done[xs] = true;
			}
	/*
	 * Every value a failed step leaves, in the state it leaves: enumerated
	 * there, and passing every check of that state. An erase that fails
	 * leaves any superset of the value it erased.
	 */
	for (unsigned x = 0; x < X_STATES; x++) {
		CHECK(g_done[x]);
		memcpy(sup, g_notok[x], sizeof sup);
		for (unsigned bit = 1; bit < SEL_N; bit <<= 1)
			for (unsigned q = 0; q < SEL_N; q++)
				if (!(q & bit))
					sup[q] |= sup[q | bit];
		for (unsigned q = 0; q < SEL_N; q++) {
			if (g_left[x][q]) {
				left++;
				CHECK(!g_notok[x][q]);
			}
			if (g_efrom[x][q]) {
				efrom++;
				CHECK(!sup[q]);
			}
		}
	}
	printf("  every start: %u operations, %llu torn values judged; single-cut losses: %u "
	       "before the slot write, %u at the settling, %u at the retry's settling (%u of them "
	       "from a robust state); %u double faults\n", g_cnt.ops, g_cnt.values,
	       g_cnt.u_loss[PH_FIRST], g_cnt.u_loss[PH_SETTLE], g_cnt.u_loss[PH_RETRY],
	       g_cnt.g_loss, g_cnt.doubles);
	printf("    failed settling steps: %u values left by a program or a read, %u erased values "
	       "(any superset left), each in the state it leaves; B's own image settled by "
	       "programs from %u starts; select first under the fallback from %u\n", left, efrom,
	       g_by_programs, g_fallback_first);
	for (unsigned ph = 0; ph < PHS; ph++)
		for (unsigned a = 0; a < 2u; a++)
			for (unsigned k = 0; k < RK_KINDS; k++)
				for (unsigned kb = 0; kb < RK_KINDS; kb++)
					if (g_cnt.by[ph][a][k][kb] != 0u)
						printf("    %s: A = slot %u, A kind %u, B kind %u: %u\n",
						       ph == PH_FIRST ? "before" :
						       ph == PH_SETTLE ? "settling" : "retry",
						       a, k, kb, g_cnt.by[ph][a][k][kb]);
	printf("    settling losses: %u from a value that trusted a bad slot, %u from one no "
	       "program could settle, %u else, %u from a program alone\n", g_cnt.settle_class[0],
	       g_cnt.settle_class[1], g_cnt.settle_class[2], g_cnt.settle_class[3]);
	/* by construction: an erase is made only where no program settles the
	 * value, and a value that trusts a bad slot leaves nothing to lose */
	CHECK_EQ(g_cnt.settle_class[0], 0);
	CHECK_EQ(g_cnt.settle_class[2], 0);
	CHECK_EQ(g_cnt.settle_class[3], 0);
	CHECK(left > 0u && efrom > 0u);
	CHECK(g_by_programs > 0u && g_fallback_first > 0u);
	/*
	 * No single cut of the front loses a good slot from a state that every
	 * cut of the first stage's own write reaches one from (the reach of
	 * fr_reach()), and none adds a double fault there.
	 */
	CHECK_EQ(g_cnt.g_loss, 0);
	CHECK_EQ(g_cnt.doubles, 0);
	/*
	 * From a state that only the uncut first stage follows to a good slot
	 * (for instance {00, 00} with slot 0 damaged by a sure-to-fail record
	 * and slot 1 good: the first stage fails slot 0 and writes {01, 01}),
	 * the front's erase can be torn to {00, 01}, which trusts the damaged
	 * slot. The first stage's own erase of the same value at the next start
	 * has the same exposure, which is why fr_reach() does not count it.
	 * Before the slot write the front leaves such a selector alone
	 * (S-02.084: it leads to B), or erases one whose verified byte keeps a
	 * bit besides bit 0 (the fallback), so no single cut there loses a slot
	 * the uncut first stage reached. At the settling the erase cannot be
	 * avoided: no program raises verified to 01, and a selector left as it
	 * is leaves the same erase to the first stage at the next start. Those
	 * are counted by class and printed; a class other than "no program
	 * could settle it" fails above.
	 */
	CHECK_EQ(g_cnt.u_loss[PH_FIRST], 0);
}

/*
 * The real writer against the predicted operations, for a representative of
 * each class of starts (the plan and the settling decide by the selector
 * alone, checked above over every value): its op log holds exactly the
 * predicted selector writes, a byte at a time, and ends settled.
 */
static void test_front_representatives(void)
{
	static const unsigned reps[] = {
		0x0000u, 0x0001u, 0x0100u, 0x0101u, 0x3F00u, 0x3F01u, 0xFFFFu, 0xFF00u, 0x01FFu,
		0xFE00u, 0x0200u, 0x0300u,
	};

	for (unsigned a = 0; a < 2u; a++)
		for (unsigned bg = 0; bg < 2u; bg++)
			for (unsigned i = 0; i < N(reps); i++)
				for (unsigned req = 0; req < REQS; req++) {
					const unsigned b = 1u - a, s = reps[i];
					const uint8_t index = a == 0u ? 0x00 : 0x01;
					struct fr_running run = running_of(index);
					bool leading, by_prog;
					unsigned tgt;
					uint32_t n = N_IMAGE;
					int rc;

					if (req == REQ_OWN && !bg)
						continue;
					damage_in_place(index, a);	/* A's old record; B bad */
					if (bg) {
						(void)build_image(g_x, sizeof g_x, 400u + a);
						put_slot(b, g_x, sizeof g_x);
					}
					g_f.mem[FR_SELECTOR_OFF] = (uint8_t)s;
					g_f.mem[FR_SELECTOR_OFF + 1u] = (uint8_t)(s >> 8);
					memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
					ff_reset(&g_f);
					{
						struct fr_record ra;
						const unsigned v = s >> 8;

						ra.crc32 = 0;
						ra.length = fr_le32(g_f.mem + FR_RECORD_OFF(a) + 4u);
						ra.tag = fr_le32(g_f.mem + FR_RECORD_OFF(a) + 8u);
						leading = bg && rule_leads_to_b(s, a, &ra);
						by_prog = bg && (s & 0xFFu) == 0u && b == 0u &&
							  (v & 0x01u) != 0u && v != 0x01u;
					}
					g_flags = req == REQ_FLAG ? FR_BEGIN_F_RUNNING : 0u;
					if (req == REQ_OWN) {
						memcpy(g_img, g_x, sizeof g_x);
						sha256(g_sha, g_img, sizeof g_x);
						n = sizeof g_x;
					} else {
						make_image(N_IMAGE, 410u + i);
					}
					rc = write_all(n, g_sha, &run);
					tgt = req == REQ_FLAG || (leading && !by_prog) ? a : b;
					if (req == REQ_OTHER && bg && tgt == b) {
						CHECK_EQ(rc, FR_WR_ONLY_GOOD);
						CHECK_EQ(g_f.nmut, 0);
						continue;
					}
					CHECK_EQ(rc, FR_WR_OK);
					CHECK_EQ(g_w.target, tgt);
					/* the selector writes: select first's settling on B,
					 * then the settling on the target, each nothing,
					 * programs alone, or an erase and programs */
					{
						const unsigned k1 = g_w.way == FR_WAY_SELECT_FIRST ?
								    settle_kind(s, b) : 0u;
						const unsigned pre2 = k1 != 0u ? (b | 0x0100u) : s;
						const unsigned k2 = settle_kind(pre2, tgt);
						const unsigned rp = find_op(FF_PROGRAM, FR_RECORD_OFF(tgt), 0);
						unsigned erases = 0, at = 0, m;

						for (unsigned k = 0; k < g_f.nlog; k++)
							if (g_f.log[k].kind == FF_ERASE &&
							    g_f.log[k].off == FR_SELECTOR_OFF)
								erases++;
						CHECK_EQ(erases, (k1 == 2u ? 1u : 0u) + (k2 == 2u ? 1u : 0u));
						g_split = true;
						if (k1 != 0u) {
							m = first_sel_mut(0);
							CHECK(m != ~0u);
							if (m != ~0u)
								at = check_settle(m, s, b);
						}
						/* the final settling: after the record's program, or
						 * the whole write for B's image alone */
						m = first_sel_mut(rp == ~0u ? at : rp + 1u);
						if (k2 == 0u) {
							CHECK_EQ(m, ~0u);
						} else {
							CHECK(m != ~0u);
							if (m != ~0u)
								CHECK_EQ(check_settle(m, pre2, tgt), g_f.nlog);
						}
						/* no selector op before A is whole, from a start
						 * that leads to B */
						if (leading && tgt == a)
							CHECK(first_sel_mut(0) == ~0u ||
							      first_sel_mut(0) > rp);
					}
					CHECK(ff_slot_good(&g_f, tgt));
					CHECK(settle_kind(g_f.mem[FR_SELECTOR_OFF] |
							  (unsigned)g_f.mem[FR_SELECTOR_OFF + 1u] << 8, tgt) == 0u);
				}
}

/*
 * The controls: what the fix removed fails the same assertions.
 * 1. One 2-byte program (both bytes torn at once) settling on slot 0 with
 *    A = slot 1 damaged: from FF FF, which the uncut first stage follows to
 *    slot 0, it can leave {01, 01}, trusting the damaged slot 1.
 * 2. An erase of {00, 00} with A = slot 1 damaged and slot 0 good: every
 *    reset boots slot 0 before; a torn erase can leave {01, 01}.
 * 3. Ending with {target, 00}, target slot 0, slot 1 bad: the first stage
 *    then writes {00, 01} itself, and its erase can leave {01, 01}: not
 *    settled, and not reached however the first stage's write is cut.
 */
static void test_controls(void)
{
	const struct model *m = model_of(RK_GOOD, RK_OLD);	/* slot 0 good, slot 1 damaged */
	const struct op_count saved = g_cnt;
	const struct fr_record rf = rk_record(RK_GOOD);

	g_quiet = true;
	/* 1: one 2-byte program from FF FF toward {00, 01} */
	memset(&g_cnt, 0, sizeof g_cnt);
	CHECK(model_u(m, 0xFFFFu));
	judge_op(m, PH_CONTROL, OP_PAIR, 0xFFFFu, 0x00u, 0x01u, 1, RK_GOOD, RK_OLD, 0xFFFFu, 9);
	CHECK(g_cnt.u_loss[PH_CONTROL] > 0u);
	/* the split way from the same state loses nothing */
	memset(&g_cnt, 0, sizeof g_cnt);
	judge_op(m, PH_CONTROL, OP_INDEX, 0xFFFFu, 0x00u, 0, 1, RK_GOOD, RK_OLD, 0xFFFFu, 9);
	judge_op(m, PH_CONTROL, OP_VERIFIED, 0xFFFFu, 0x00u, 0x01u, 1, RK_GOOD, RK_OLD, 0xFFFFu, 9);
	CHECK_EQ(g_cnt.u_loss[PH_CONTROL], 0);
	/* 2: an erase of {00, 00}, which every reset follows to slot 0 */
	memset(&g_cnt, 0, sizeof g_cnt);
	CHECK(model_u(m, 0x0000u));
	judge_op(m, PH_CONTROL, OP_ERASE, 0x0000u, 0, 0, 1, RK_GOOD, RK_OLD, 0x0000u, 9);
	CHECK(g_cnt.u_loss[PH_CONTROL] > 0u);
	CHECK(!model_u(m, 0x0101u));
	/* 4: select first's erase of {A, 00}, A's record sure to fail, B good:
	 * the uncut first stage falls back to B; a torn erase reads 01 over
	 * A's index. Both mirrors. */
	{
		const struct model *m0 = model_of(RK_OLD, RK_GOOD);	/* A = slot 0 */

		memset(&g_cnt, 0, sizeof g_cnt);
		CHECK(model_u(m0, 0x0000u));
		judge_op(m0, PH_CONTROL, OP_ERASE, 0x0000u, 0, 0, 0, RK_GOOD, RK_OLD, 0x0000u, 9);
		CHECK(g_cnt.u_loss[PH_CONTROL] > 0u);
		memset(&g_cnt, 0, sizeof g_cnt);
		CHECK(model_u(m, 0x0001u));			/* A = slot 1: {01, 00} */
		judge_op(m, PH_CONTROL, OP_ERASE, 0x0001u, 0, 0, 1, RK_GOOD, RK_OLD, 0x0001u, 9);
		CHECK(g_cnt.u_loss[PH_CONTROL] > 0u);
	}
	/* 5: an erase in place of the programs alone: {00, 03} settled on slot
	 * 0 with A = slot 1 damaged (a record its check may pass) and slot 0
	 * good: the uncut first stage passes slot 0; a torn erase can read
	 * {01, 03}, and the first stage may then pass slot 1 and trust it. The
	 * programs alone lose nothing there. */
	{
		const struct model *mt = model_of(RK_GOOD, RK_TORN);

		memset(&g_cnt, 0, sizeof g_cnt);
		CHECK(model_u(mt, 0x0300u));
		judge_op(mt, PH_CONTROL, OP_ERASE, 0x0300u, 0, 0, 1, RK_GOOD, RK_TORN, 0x0300u, 9);
		CHECK(g_cnt.u_loss[PH_CONTROL] > 0u);
		CHECK(!model_u(mt, 0x0301u));
		memset(&g_cnt, 0, sizeof g_cnt);
		judge_op(mt, PH_CONTROL, OP_PVER, 0x0300u, 0, 0, 1, RK_GOOD, RK_TORN, 0x0300u, 9);
		CHECK_EQ(g_cnt.u_loss[PH_CONTROL], 0);
	}
	/* 3: ending {00, 00} with slot 1 bad: not settled, not reached by
	 * fr_reach(), and a cut of the first stage's own write loses it */
	CHECK(rule_reach(0x0000u, m->good, &rf) == FR_REACH_NONE);
	CHECK(!model_g(m, 0x0000u));
	CHECK(!settled(0x0000u, 0));
	g_quiet = false;
	g_cnt = saved;
}

/* The settling in the real writer, for every selector value and both
 * targets: nothing, the programs alone, or the erase, as settle_kind()
 * says from S-02.084's text. */
static void test_settle_every_value(void)
{
	for (unsigned a = 0; a < 2u; a++) {
		const uint8_t index = a == 0u ? 0x00 : 0x01;
		struct fr_running run = running_of(index);
		struct fr_writer saved;

		/* a write of A in place in the damaged state, up to the settling */
		damage_in_place(index, a);
		memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
		ff_reset(&g_f);
		g_flags = FR_BEGIN_F_RUNNING;
		make_image(N_IMAGE, 420u + a);
		CHECK_EQ(fr_writer_begin(&g_w, flash(), &run, &g_home, g_img, N_IMAGE, g_sha,
					 g_flags), FR_WR_OK);
		CHECK_EQ(g_w.way, FR_WAY_IN_PLACE);
		for (unsigned i = 0; i < 4000u && g_w.state != FR_WS_SELECTOR_ERASE &&
		     g_w.state != FR_WS_DONE; i++)
			(void)fr_writer_step(&g_w);
		CHECK_EQ(g_w.state, FR_WS_SELECTOR_ERASE);
		saved = g_w;
		for (unsigned s = 0; s < SEL_N; s++) {
			bool done;

			g_w = saved;
			memset(g_f.mem + FR_SELECTOR_OFF, 0xFF, FR_SECTOR);
			g_f.mem[FR_SELECTOR_OFF] = (uint8_t)s;
			g_f.mem[FR_SELECTOR_OFF + 1u] = (uint8_t)(s >> 8);
			ff_reset(&g_f);
			done = fr_writer_step(&g_w);
			CHECK(done);
			CHECK_EQ(g_w.result, FR_WR_OK);
			switch (settle_kind(s, a)) {
			case 0:		/* nothing */
				CHECK_EQ(g_f.nmut, 0);
				CHECK(g_f.mem[FR_SELECTOR_OFF] == (uint8_t)s &&
				      g_f.mem[FR_SELECTOR_OFF + 1u] == (uint8_t)(s >> 8));
				break;
			case 1:		/* programs alone: no erase */
				CHECK_EQ(g_f.nmut, ((s & 0xFFu) != 0u ? 1u : 0u) + ((s >> 8) != 0x01u ? 1u : 0u));
				CHECK_EQ(find_op(FF_ERASE, FR_SELECTOR_OFF, 0), ~0u);
				CHECK(g_f.mem[FR_SELECTOR_OFF] == 0x00u &&
				      g_f.mem[FR_SELECTOR_OFF + 1u] == 0x01u);
				break;
			default:	/* an erase and the split programs */
				CHECK_EQ(g_f.nmut, 3);
				CHECK(g_f.mem[FR_SELECTOR_OFF] == a && g_f.mem[FR_SELECTOR_OFF + 1u] == 0x01u);
				break;
			}
		}
		memset(&g_w, 0, sizeof g_w);	/* idle again for the next write */
	}
}

/*
 * What stays unsafe by design, and the double faults:
 * - States with no good slot: nothing to lose.
 * - Starts that already trusted a bad slot (verified 01 naming the damaged
 *   slot): no start boots a good slot before the write either; the uncut
 *   first stage reaches none (model_u false), so no cut can lose one.
 * - Double faults: a cut of the front's operation that the uncut first
 *   stage still follows to a good slot, followed by a cut inside the first
 *   stage's own selector write on the next start (its erase torn, or its
 *   2-byte program {index, 01} torn to {01, 01}). test_front_every_start
 *   counts them from the states fr_reach() claims: none.
 * - Starts the uncut first stage follows to a good slot but a cut of its
 *   own write may not (fr_reach() none, model_g false): a single cut of
 *   the front's erase there can lose the slot as the first stage's own
 *   erase can. test_front_every_start prints the counts, before the slot
 *   write (checked: it fails while select first erases such a selector)
 *   and at the settling (unavoidable, counted).
 */

/* The status recovery decides INFO, the hold and RESTART from, over the
 * flash as it is (fr_proto.c). */
static struct fr_rec_status status_now(unsigned a)
{
	struct fr_rec_status st;

	memset(&st, 0, sizeof st);
	st.running.established = true;
	st.running.slot = (uint8_t)a;
	st.home = g_home;
	fr_rec_survey(&st, flash());
	ff_reset(&g_f);
	return st;
}

static unsigned sel_now(void)
{
	return g_f.mem[FR_SELECTOR_OFF] | (unsigned)g_f.mem[FR_SELECTOR_OFF + 1u] << 8;
}

/*
 * S-02.084, the state until a write has ended well: A = slot a, B
 * bad; a write of A in place reads A back good, then its settling fails:
 * 0 the erase taken and the index program not stored; 1 the index stored
 * and its read-back failing; 2 the verified program not stored; 3 and 4
 * the programs alone (slot 0 only), the index program or the verified
 * program failing. The write ends 11 with A good; the state stands (bit
 * 7), and holds where no start reaches a good slot. Then the flag with A's
 * own image settles the selector with no erase or program in A or its
 * record, and the state ends; another image is 18; without the flag the
 * write goes to B.
 */
static void test_unsettled(void)
{
	static const uint16_t from[] = { 0x0000u, 0x0000u, 0x0000u, 0x0301u, 0x0300u };
	static uint8_t mine[N_IMAGE];
	uint8_t mine_sha[32];

	for (unsigned a = 0; a < 2u; a++)
		for (unsigned m = 0; m < N(from); m++) {
			const uint8_t index = a == 0u ? 0x00 : 0x01;
			const unsigned b = 1u - a;
			struct fr_running run = running_of(index);
			struct fr_rec_status st;
			struct fr_plan plan;
			struct fr_desc front;
			bool good[2];
			unsigned q;
			int rc;

			if (a == 1u && m >= 3u)
				continue;	/* slot 1 is settled by no program */
			damage_in_place(index, a);
			g_f.mem[FR_SELECTOR_OFF] = (uint8_t)from[m];
			g_f.mem[FR_SELECTOR_OFF + 1u] = (uint8_t)(from[m] >> 8);
			ff_reset(&g_f);
			CHECK_EQ(settle_kind(from[m], a), m >= 3u ? 1u : 2u);
			g_flags = FR_BEGIN_F_RUNNING;
			make_image(N_IMAGE, 230u + a);
			memcpy(mine, g_img, N_IMAGE);
			memcpy(mine_sha, g_sha, sizeof mine_sha);
			g_idx_mode = m == 0u ? 1u : m == 1u ? 2u : m == 3u ? 3u : 0u;
			g_ver_mode = m == 2u ? 1u : m == 4u ? 2u : 0u;
			rc = write_all(N_IMAGE, g_sha, &run);
			g_idx_mode = g_ver_mode = 0;
			CHECK_EQ(rc, FR_WR_SELECTOR);
			CHECK_EQ(g_w.way, FR_WAY_IN_PLACE);
			CHECK(ff_slot_good(&g_f, a) && !ff_slot_good(&g_f, b));
			CHECK(g_home.spoiled && g_home.unsettled);
			q = sel_now();
			good[a] = true;
			good[b] = false;
			st = status_now(a);
			CHECK(fr_rec_rescue(&st));
			CHECK(fr_rec_held(&st) == (code_reach(q, good, &st.slot[(q & 0xFFu) != 0u ? 1u : 0u].rec) ==
						   FR_REACH_NONE));
			if (a == 0u)
				CHECK(fr_rec_held(&st));	/* no start reaches slot 0 */
			else
				CHECK(!fr_rec_held(&st));	/* the index left names slot 1 */
			CHECK(st.target == b && st.run_way == FR_WAY_PLAIN);
			/* the plans: the flag A's own image alone; without it, B */
			CHECK_EQ(fr_writer_plan(flash(), &run, &g_home, FR_BEGIN_F_RUNNING, &plan, &front),
				 FR_WR_OK);
			CHECK(plan.rescue && plan.way == FR_WAY_PLAIN && plan.target == a && plan.only_same);
			CHECK_EQ(fr_writer_plan(flash(), &run, &g_home, 0, &plan, &front), FR_WR_OK);
			CHECK(plan.rescue && plan.way == FR_WAY_PLAIN && plan.target == b &&
			      !plan.only_same);
			ff_reset(&g_f);
			/* another image with the flag: 18, nothing written */
			make_image(N_IMAGE, 240u + a);
			CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_ONLY_GOOD);
			CHECK_EQ(g_f.nmut, 0);
			CHECK(g_home.unsettled);
			/* A's own image with the flag: the selector alone */
			memcpy(g_img, mine, N_IMAGE);
			memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
			ff_reset(&g_f);
			CHECK_EQ(write_all(N_IMAGE, mine_sha, &run), FR_WR_OK);
			CHECK_EQ(g_w.target, a);
			for (unsigned i = 0; i < g_f.nlog; i++)
				if (g_f.log[i].kind != FF_READ)
					CHECK(g_f.log[i].off >= FR_SELECTOR_OFF &&
					      g_f.log[i].off < FR_SELECTOR_OFF + FR_SECTOR);
			{
				const unsigned msel = first_sel_mut(0);

				if (settle_kind(q, a) == 0u) {
					CHECK_EQ(msel, ~0u);
				} else {
					g_split = true;
					CHECK(msel != ~0u);
					if (msel != ~0u)
						CHECK_EQ(check_settle(msel, q, a), g_f.nlog);
				}
			}
			CHECK_EQ(settle_kind(sel_now(), a), 0);
			CHECK(!g_home.unsettled);
			st = status_now(a);
			CHECK(!fr_rec_rescue(&st) && !fr_rec_held(&st));
			CHECK_EQ(fr_rec_reach(&st), FR_REACH_NAMED);
		}
}

/*
 * S-02.084's exception: B = slot 0 good and named, verified odd and not
 * 01, A = slot 1 damaged. No start reaches a good slot ({00, 03}: the
 * first stage's own write could be cut to {01, 01}): the hold is on. B's
 * own image without the flag settles it by one program and ends the hold;
 * refusing that image with 13 (the old choice) lost nothing but kept the
 * hold. Another image is 18; the flag writes A with no selector write
 * first.
 */
static void test_by_programs(void)
{
	struct fr_running run = running_of(0x01);
	struct fr_rec_status st;
	const bool good[2] = { true, false };
	const struct fr_record rf = rk_record(RK_GOOD);

	for (unsigned req = 0; req < REQS; req++) {
		damage_in_place(0x01, 1);
		(void)build_image(g_x, sizeof g_x, 250u);
		put_slot(0, g_x, sizeof g_x);
		g_f.mem[FR_SELECTOR_OFF] = 0x00u;
		g_f.mem[FR_SELECTOR_OFF + 1u] = 0x03u;
		ff_reset(&g_f);
		st = status_now(1);
		CHECK(fr_rec_held(&st));
		CHECK(rule_reach(0x0300u, good, &rf) == FR_REACH_NONE);
		CHECK(st.target == 0u && st.run_way == FR_WAY_PLAIN);
		memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
		g_flags = req == REQ_FLAG ? FR_BEGIN_F_RUNNING : 0u;
		if (req == REQ_OWN) {
			memcpy(g_img, g_x, sizeof g_x);
			sha256(g_sha, g_img, sizeof g_x);
			CHECK_EQ(write_all(sizeof g_x, g_sha, &run), FR_WR_OK);
			CHECK_EQ(g_w.target, 0);
			/* one program, verified to 01; no erase */
			CHECK_EQ(g_f.nmut, 1);
			CHECK_EQ(find_op(FF_ERASE, FR_SELECTOR_OFF, 0), ~0u);
			CHECK(find_op(FF_PROGRAM, FR_SELECTOR_OFF + 1u, 0) != ~0u);
			CHECK(sel_now() == 0x0100u);
			st = status_now(1);
			CHECK(!fr_rec_held(&st));
			CHECK_EQ(fr_rec_reach(&st), FR_REACH_NAMED);
		} else if (req == REQ_OTHER) {
			make_image(N_IMAGE, 251u);
			CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_ONLY_GOOD);
			CHECK_EQ(g_f.nmut, 0);
		} else {
			make_image(N_IMAGE, 252u);
			CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
			CHECK_EQ(g_w.target, 1);
			CHECK_EQ(g_w.way, FR_WAY_PLAIN);
			/* no selector op before A's record is programmed */
			CHECK(first_sel_mut(0) > find_op(FF_PROGRAM, FR_RECORD_OFF(1), 0));
		}
	}
}

/*
 * The one write of A made under a selector naming A unverified (S-02.052,
 * the paragraph after "What a torn record leaves"): A damaged with a
 * record sure to fail, B good, the selector {A, 00}; the flag writes A
 * plainly. Its record erase and record program come after every byte of
 * A's image, and the selector is not written before them. A cut there
 * leaves A's record at any superset of the old record's bits (the erase)
 * or of the new one's (the program). By class: the tag not matching (the
 * check fails: B); a length of 0, inside the slot, beyond it below
 * 0x80000000 (judged by the CRC), or 0x80000000 and more (both outcomes):
 * A boots a whole image or the check fails and B boots. The factory
 * model with A's image whole and its check passing or failing, every cut
 * of the first stage's own write included, reaches a good slot from {A,
 * 00}. A length shorter than the image whose CRC field matches the
 * image's prefix would boot that prefix: counted over the bits the cut
 * leaves free, for the images here.
 */
struct rec_classes {
	bool tag_other, zero, in_slot, shorter, beyond, huge;
};

static struct rec_classes record_classes(const struct fr_record *r)
{
	struct rec_classes c;
	const bool tag = (FR_TAG & r->tag) == r->tag;

	c.tag_other = true;	/* the tag itself or a bit of FR_TAG's set */
	c.zero = tag && r->length == 0u;
	c.in_slot = tag && r->length <= FR_SLOT_CAP;
	c.shorter = tag && r->length < N_IMAGE;
	c.beyond = tag && r->length < 0x80000000u;
	c.huge = tag;
	return c;
}

/* lengths x over len's bits below N_IMAGE whose prefix CRC keeps crc's bits */
static unsigned prefix_matches(const uint8_t *img, uint32_t len, uint32_t crc)
{
	unsigned hits = 0;
	uint32_t c = 0;

	for (uint32_t x = 1; x < N_IMAGE; x++) {
		c = fr_crc32(c, img + x - 1u, 1u);
		if ((x & len) == len && (c & crc) == crc)
			hits++;
	}
	return hits;
}

static void test_record_cuts(void)
{
	unsigned exposed = 0, shorter = 0;

	for (unsigned a = 0; a < 2u; a++) {
		const uint8_t index = a == 0u ? 0x00 : 0x01;
		const unsigned b = 1u - a, s = index;	/* {A, 00} */
		struct fr_running run = running_of(index);
		struct fr_record r_old, r_new;
		unsigned re, rp, last_img = 0;
		struct rec_classes c[2];
		const struct model *m = model_of(a == 0u ? RK_WHOLE : RK_GOOD,
						 a == 0u ? RK_GOOD : RK_WHOLE);

		damage_in_place(index, a);
		(void)build_image(g_x, sizeof g_x, 260u + a);
		put_slot(b, g_x, sizeof g_x);
		g_f.mem[FR_SELECTOR_OFF] = index;
		g_f.mem[FR_SELECTOR_OFF + 1u] = 0x00u;
		CHECK_EQ(fr_read_record(flash(), a, &r_old), FR_FLASH_OK);
		CHECK(rule_fails(&r_old));
		ff_reset(&g_f);
		memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
		g_flags = FR_BEGIN_F_RUNNING;
		make_image(N_IMAGE, 270u + a);
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
		CHECK_EQ(g_w.way, FR_WAY_PLAIN);
		CHECK_EQ(g_w.target, a);
		re = find_op(FF_ERASE, FR_RECORD_OFF(a), 0);
		rp = find_op(FF_PROGRAM, FR_RECORD_OFF(a), 0);
		CHECK(re != ~0u && rp != ~0u && re < rp);
		for (unsigned i = 0; i < g_f.nlog; i++)
			if (g_f.log[i].kind != FF_READ && g_f.log[i].off >= FR_SLOT_BASE(a) &&
			    g_f.log[i].off < FR_SLOT_BASE(a) + FR_SLOT_CAP)
				last_img = i;
		/* A's image whole (its last sector erased too) before the record */
		CHECK(last_img < re);
		/* the selector names A unverified throughout: not written before */
		CHECK(first_sel_mut(0) > rp);
		CHECK_EQ(fr_read_record(flash(), a, &r_new), FR_FLASH_OK);
		ff_reset(&g_f);
		c[0] = record_classes(&r_old);
		c[1] = record_classes(&r_new);
		/* the classes a cut can leave: the program's lengths keep the new
		 * length's bits, so none is shorter than the image */
		CHECK(!c[1].shorter && c[1].huge && c[1].beyond && c[1].in_slot);
		CHECK(c[0].huge);	/* the erase: 0x80000000 and more, unspecified */
		if (c[0].shorter)
			shorter += prefix_matches(g_img, r_old.length, r_old.crc32);
		/* a first stage reads A's record from {A, 00} */
		exposed += (s >> 8) != 0x01u ? 1u : 0u;
		/* both outcomes of the check, every cut of the first stage's own
		 * write: a good slot (A whole, or B) */
		CHECK(model_u(m, s) && model_g(m, s));
		/* and with the outcome fixed either way, a subset of those paths */
		CHECK(model_g(model_of(RK_GOOD, RK_GOOD), s));
	}
	CHECK_EQ(exposed, 2);
	/* a CRC field over the old record's bits matching a prefix of the new
	 * image: none for these images */
	CHECK_EQ(shorter, 0);
	printf("  record cuts in the verified-00 write: both outcomes reach a good slot; "
	       "prefix lengths with a matching CRC: %u\n", shorter);
}

/*
 * The fallback case (the selector naming A = slot 0 with a verified bit
 * besides bit 0, A's record sure to fail, B good): the flag selects B
 * first, so a first stage reads B's record, not A's, while A's record is
 * erased and programmed. The old order wrote A with the selector naming
 * A: a cut of A's record erase could leave the tag with a length of
 * 0x80000000 or more, read by the first stage there. The control shows
 * that exposure; the real writer shows the new order.
 */
static void test_fallback_order(void)
{
	struct fr_running run = running_of(0x00);
	struct fr_record r_old;
	unsigned rp, last;

	damage_in_place(0x00, 0);
	(void)build_image(g_x, sizeof g_x, 280u);
	put_slot(1, g_x, sizeof g_x);
	g_f.mem[FR_SELECTOR_OFF] = 0x00u;
	g_f.mem[FR_SELECTOR_OFF + 1u] = 0xFEu;
	CHECK_EQ(fr_read_record(flash(), 0, &r_old), FR_FLASH_OK);
	CHECK(rule_fails(&r_old));
	ff_reset(&g_f);
	memcpy(g_start.mem, g_f.mem, sizeof g_f.mem);
	/* the control: under {00, FE} a first stage reads A's record, and a
	 * cut of its erase can leave a length the check may pass */
	CHECK(record_classes(&r_old).huge);
	CHECK(fr_selected_slot(0x00) == 0u && 0xFEu != 0x01u);
	g_flags = FR_BEGIN_F_RUNNING;
	make_image(N_IMAGE, 281u);
	CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
	CHECK_EQ(g_w.way, FR_WAY_SELECT_FIRST);
	g_split = true;
	/* the settling on B before A's first erase: {01, 01} */
	{
		const unsigned m0 = first_sel_mut(0);
		const unsigned e0 = find_op(FF_ERASE, FR_SLOT_BASE(0), 0);

		CHECK(m0 != ~0u && e0 != ~0u && m0 < e0);
		if (m0 != ~0u)
			last = check_settle(m0, 0xFE00u, 1);
		else
			last = 0;
		CHECK(last <= e0);
	}
	/* no selector op from there to A's record program: the first stage
	 * reads {01, 01} and boots B, whatever A's record holds */
	rp = find_op(FF_PROGRAM, FR_RECORD_OFF(0), 0);
	CHECK(rp != ~0u && first_sel_mut(last) > rp);
	CHECK(ff_slot_good(&g_f, 0));
	CHECK(sel_now() == 0x0100u);
}

/*
 * --- S-02.093: the trial (BEGIN flag bit 1) and the select (bit 2) ---
 *
 * The proof by cuts, with the model above. The new front is taken as dead:
 * the slot written for the trial never boots, so its kind in the model is
 * one that does not boot, by what the factory check makes of its record
 * as the flash holds it after a cut: sure to fail (the tag, or a CRC that
 * differs), sure to pass (a CRC that matches: a start would trust it), or
 * either (a length beyond the slot, S-02.026). A loss is a state from
 * which the factory first stage comes to trust that slot: with its own
 * writes uncut (model_u false: the cut alone does it), or under some cut
 * of its own selector write at some later start (model_g false). A select
 * has made its choice once the tried slot's record is right: from then on
 * the slot counts as good, as the target of any write does.
 */

enum { TL_SINGLE, TL_FS, TL_KINDS };

struct trial_tally {
	unsigned long long states;		/* selector values judged */
	unsigned long long loss[TL_KINDS];
	unsigned long long erase_loss[TL_KINDS];	/* inside recovery's selector erase */
	unsigned long long other_loss[TL_KINDS];	/* anywhere else */
};

static struct trial_tally g_tt[2];	/* by B's index */

/* The model's kind of slot k, as the flash holds it: good where alive and
 * good by S-02.016; else by what the factory check makes of its record. */
static unsigned flash_kind(unsigned k, bool alive)
{
	const uint8_t *r = g_f.mem + FR_RECORD_OFF(k);
	const uint32_t crc = fr_le32(r), len = fr_le32(r + 4), tag = fr_le32(r + 8);

	if (alive && ff_slot_good(&g_f, k))
		return RK_GOOD;
	if (tag != FR_TAG)
		return RK_BADCRC;
	if (len > FR_SLOT_CAP)
		return RK_TORN;			/* passes or fails (S-02.026) */
	return ref_crc32(0, g_f.mem + FR_SLOT_BASE(k), len) == crc ? RK_EMPTY : RK_BADCRC;
}

/* 0 no loss, else 1 + TL_SINGLE or 1 + TL_FS */
static unsigned judge_value(const struct model *m, unsigned s)
{
	if (!model_u(m, s))
		return 1u + TL_SINGLE;
	return model_g(m, s) ? 0u : 1u + TL_FS;
}

/* What one operation of the writer can leave at the selector, torn or
 * whole, from value p: an erase any superset of p; a 1-byte program of w
 * at 0xD000 or 0xD001 its byte between the old one and the old one AND w,
 * the other byte as it was; the 2-byte program the same for both bytes. */
static bool torn_to(const struct ff_op *op, unsigned p, unsigned w, unsigned x)
{
	unsigned mask, keep;

	if (op->kind == FF_ERASE)
		return (x & p) == p;
	if (op->off == FR_SELECTOR_OFF && op->len == 1u)
		mask = 0x00FFu;
	else if (op->off == FR_SELECTOR_OFF + 1u && op->len == 1u)
		mask = 0xFF00u;
	else
		mask = 0xFFFFu;
	keep = p & ~mask;
	return (x & ~mask) == keep && (x & mask & ~p) == 0u && (x & mask & p & w) == (p & w & mask);
}

static unsigned sel_value(void)
{
	return g_f.mem[FR_SELECTOR_OFF] | (unsigned)g_f.mem[FR_SELECTOR_OFF + 1u] << 8;
}

/* The flash as op k of the uncut write found it (k from 1), the write
 * redone up to the cut after op k - 1. */
typedef void (*prep_fn)(void);
static prep_fn g_prep;		/* the state before the write, g_img and g_flags set */
static struct fr_running g_trun;

static void redo_to(unsigned k)
{
	g_prep();
	if (k > 1u) {
		g_f.cut_after = k - 1u;
		(void)write_all(N_IMAGE, g_sha, &g_trun);
		CHECK_EQ(g_f.nmut, k - 1u);
	}
	ff_reset(&g_f);
}

/*
 * The memory a start of slot k loaded: its stream's blocks as the boot ROM
 * loads them (to FINAL, within the record's length, or the slot where the
 * record is not plausible), read where the front may read it
 * (fr_mem_readable()). The flash's own bytes: the start's writes are the
 * selector and the log sectors, outside every stream here.
 */
struct ld_block {
	uint32_t target, count, off;
};

static struct ld_block g_ldb[32];
static unsigned g_nldb, g_ld_slot;

static void load_slot(unsigned k)
{
	const uint8_t *s = g_f.mem + FR_SLOT_BASE(k), *r = g_f.mem + FR_RECORD_OFF(k);
	const uint32_t rl = fr_le32(r + 4);
	const uint32_t len = fr_le32(r + 8) == FR_TAG && rl >= 1u && rl <= FR_SLOT_CAP ? rl : FR_SLOT_CAP;
	uint32_t off = 0;

	g_ld_slot = k;
	g_nldb = 0;
	while ((uint64_t)off + 16u <= len) {
		uint32_t flags, count;
		uint8_t x = 0;

		for (unsigned i = 0; i < 16u; i++)
			x = (uint8_t)(x ^ s[off + i]);
		if (s[off + 3] != 0xADu || x != 0u)
			break;
		flags = fr_le32(s + off) & 0xFFF0u;
		count = fr_le32(s + off + 8);
		off += 16u;
		if (!(flags & S05_FLAG_FILL)) {
			if ((uint64_t)off + count > len)
				break;
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
			return g_f.mem + FR_SLOT_BASE(g_ld_slot) + g_ldb[i].off + (addr - g_ldb[i].target);
	return NULL;
}

/*
 * One start from the flash as it is: the factory first stage's step, its
 * own write uncut (S-02.020-S-02.023), then, where the slot it boots runs
 * to its start log's step (B's front dead or alive; A's always runs), the
 * front's own step, the one fr_start.c makes (fr_log_step()), from that
 * slot's loaded memory. The slot booted; *trial what the step did of a
 * pending trial (FR_LOG_TRIAL_NONE where no step ran).
 */
static unsigned start_once(unsigned b, bool b_dead, unsigned *trial)
{
	const struct fr_mem mem = { ld_map, NULL };
	struct fr_log l;
	const unsigned k = boot_once(0);

	*trial = FR_LOG_TRIAL_NONE;
	if (b_dead && k == b)
		return k;
	load_slot(k);
	(void)fr_log_step(flash(), &mem, &l);
	*trial = l.trial;
	return k;
}

/*
 * S-02.093: a select from the old front (A running), with the selector read
 * as s. In the same recovery (home->trial): refused 19. After a reset: one
 * start first (start_once()), with B's front dead and with it alive, then
 * the select. An acceptance is a loss unless that start ran from B's bytes
 * past its log step, or B's bytes are those of an earlier trial whose own
 * front marked its start (g_old_ok, counted apart). Every mark the step
 * makes is made by a start from B; every spend by a start from A. The plan
 * and the first stage decide by the selector's slot and whether verified is
 * 0x01, so each class runs once in a state (cls_done). The flash is left
 * as it was.
 */
struct sel_tally {
	unsigned long long states, refused[FR_WR_NOT_STARTED + 1], lost, old_own, after_b;
	unsigned long long marked, spent;		/* the step's attempts (l->trial) */
	unsigned long long marked_prog, spent_prog;	/* what the flash shows it took */
	unsigned long long further;			/* starts after a spend */
};

static struct sel_tally g_st;
static bool (*g_old_ok)(unsigned b);	/* the bytes an earlier mark was made for */
static unsigned cls_done;		/* classes run in this state, a bit each */

static void print_sel(const char *what)
{
	const unsigned long long other = g_st.states - g_st.lost - g_st.old_own - g_st.after_b -
					 g_st.refused[FR_WR_NOT_STARTED] - g_st.refused[FR_WR_NOT_TRIED] -
					 g_st.refused[FR_WR_TRIAL_STATE];

	printf("  one start (B's front dead, then alive) and a select from the old front, %s: "
	       "%llu checks; the start's own log step programmed the mark %llu times (%llu "
	       "attempts), every one running from the tried slot, and programmed the spend %llu "
	       "times (%llu attempts), every one running from the old slot; one further start "
	       "after each spend (%llu): none booted the tried slot or marked it; the select "
	       "accepted %llu times after a start ran from the tried slot past its log step, %llu "
	       "with no such start; refused %llu with 21, %llu with 20, %llu with 19, %llu "
	       "otherwise; %llu accepted over an earlier trial's own bytes and mark\n", what,
	       g_st.states, g_st.marked_prog, g_st.marked, g_st.spent_prog, g_st.spent,
	       g_st.further, g_st.after_b, g_st.lost, g_st.refused[FR_WR_NOT_STARTED],
	       g_st.refused[FR_WR_NOT_TRIED], g_st.refused[FR_WR_TRIAL_STATE], other, g_st.old_own);
}

static int select_plan_a(unsigned a, bool same)
{
	struct fr_running run;
	struct fr_home home;
	struct fr_plan plan;
	struct fr_desc front;
	const unsigned nmut = g_f.nmut;
	int rc;

	run.established = true;
	run.slot = (uint8_t)a;
	memset(&home, 0, sizeof home);
	home.trial = same;
	g_f.dead = false;
	rc = fr_writer_plan(flash(), &run, &home, FR_BEGIN_F_SELECT, &plan, &front);
	CHECK_EQ(g_f.nmut, nmut);
	return rc;
}

static void select_check(unsigned a, bool same, unsigned s)
{
	static uint8_t keep[3][FR_SECTOR];
	const unsigned b = 1u - a, cls = fr_selected_slot((uint8_t)s) * 2u + ((s >> 8) == 0x01u ? 1u : 0u);
	const uint32_t where[3] = { FR_SELECTOR_OFF, FR_LOG_OFF(0), FR_LOG_OFF(1) };

	if (cls_done & (1u << (cls + (same ? 4u : 0u))))
		return;
	cls_done |= 1u << (cls + (same ? 4u : 0u));
	for (unsigned i = 0; i < 3u; i++)
		memcpy(keep[i], g_f.mem + where[i], FR_SECTOR);
	g_f.mem[FR_SELECTOR_OFF] = (uint8_t)s;
	g_f.mem[FR_SELECTOR_OFF + 1u] = (uint8_t)(s >> 8);
	if (same) {
		g_st.states++;
		CHECK_EQ(select_plan_a(a, true), FR_WR_TRIAL_STATE);
		g_st.refused[FR_WR_TRIAL_STATE]++;
	}
	for (unsigned dead = 0; !same && dead < 2u; dead++) {
		unsigned trial, k;
		uint8_t before, after;
		bool ran_b, marked, spent;
		int rc;

		g_f.mem[FR_SELECTOR_OFF] = (uint8_t)s;
		g_f.mem[FR_SELECTOR_OFF + 1u] = (uint8_t)(s >> 8);
		memcpy(g_f.mem + FR_LOG_OFF(0), keep[1], FR_SECTOR);
		memcpy(g_f.mem + FR_LOG_OFF(1), keep[2], FR_SECTOR);
		before = g_f.mem[FR_LOG_OFF(b)];
		k = start_once(b, dead != 0u, &trial);
		after = g_f.mem[FR_LOG_OFF(b)];
		ran_b = k == b && dead == 0u;
		/* the step's attempts, and what the flash shows it programmed */
		marked = before == FR_LOG0_PENDING && after == FR_LOG0_MARKED;
		spent = before == FR_LOG0_PENDING && after == 0xF9u;
		if (trial == FR_LOG_TRIAL_MARKED) {
			g_st.marked++;
			CHECK(ran_b);
		} else if (trial == FR_LOG_TRIAL_SPENT) {
			g_st.spent++;
			CHECK_EQ(k, a);
		}
		CHECK_EQ(marked, trial == FR_LOG_TRIAL_MARKED);
		CHECK_EQ(spent, trial == FR_LOG_TRIAL_SPENT);
		/* byte 0 changes by the mark, the spend, or the start log of a
		 * selected B whose log was free */
		CHECK(after == before || marked || spent ||
		      (before == 0xFFu && after == 0xFEu && k == b));
		g_st.marked_prog += marked ? 1u : 0u;
		g_st.spent_prog += spent ? 1u : 0u;
		rc = select_plan_a(a, false);
		g_st.states++;
		if (rc == FR_WR_OK) {
			if (g_f.mem[FR_LOG_OFF(b)] == FR_LOG0_MARKED && g_old_ok != NULL && g_old_ok(b))
				g_st.old_own++;
			else if (ran_b)
				g_st.after_b++;
			else
				g_st.lost++;
		} else if (rc >= 0 && rc <= FR_WR_NOT_STARTED) {
			g_st.refused[rc]++;
		}
		/* a spend is never followed by a trial start: one more start
		 * from the selector as it stands boots A, and its step makes
		 * no mark (nor a second spend) */
		if (spent) {
			unsigned t2;

			g_st.further++;
			CHECK_EQ(start_once(b, false, &t2), a);
			CHECK_EQ(t2, FR_LOG_TRIAL_NONE);
			CHECK_EQ(g_f.mem[FR_LOG_OFF(b)], 0xF9u);
		}
	}
	for (unsigned i = 0; i < 3u; i++)
		memcpy(g_f.mem + where[i], keep[i], FR_SECTOR);
}

/* What op (mutation k) wrote, as a selector value's bits (index | verified
 * << 8), the bytes it does not write 0xFF. */
static unsigned written(const struct ff_op *op, unsigned k)
{
	const unsigned v = k < PV_MAX ? g_pv[k] : 0xFFFFu;

	if (op->kind == FF_ERASE)
		return 0xFFFFu;
	if (op->off == FR_SELECTOR_OFF + 1u && op->len == 1u)
		return (v & 0xFFu) << 8 | 0xFFu;
	if (op->len == 1u)
		return v | 0xFF00u;
	return v;
}

static unsigned g_sel_a = 2;	/* with A = slot g_sel_a, every state's select checked */

/*
 * Every cut of the write g_prep() sets up: after each operation, and
 * inside each one. A selector operation is judged over every value it can
 * leave; any other over the fake's torn results (seeded bits, none, all).
 * b: the tried slot; alive: whether a right record counts as good (the
 * select). The whole write's own end state too. Returns the operations.
 */
static unsigned judge_write_cuts(unsigned b, bool alive, struct trial_tally *tt)
{
	static struct ff_op ops[256];
	static unsigned wv[256];
	unsigned m_total = 0, k;

	g_prep();
	CHECK_EQ(write_all(N_IMAGE, g_sha, &g_trun), FR_WR_OK);
	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++)
		if (g_f.log[i].kind != FF_READ && m_total < N(ops)) {
			ops[m_total] = g_f.log[i];
			wv[m_total] = written(&g_f.log[i], m_total + 1u);
			m_total++;
		}
	CHECK_EQ(m_total, g_f.nmut);
	for (k = 1; k <= m_total; k++) {
		const struct ff_op *op = &ops[k - 1u];
		const bool sel = op->off >= FR_SELECTOR_OFF && op->off < FR_SELECTOR_OFF + FR_SECTOR;

		if (sel) {
			const struct model *m;
			unsigned p;

			redo_to(k);
			p = sel_value();
			m = b == 0u ? model_of(flash_kind(0, alive), flash_kind(1, true)) :
				      model_of(flash_kind(0, true), flash_kind(1, alive));
			cls_done = 0;
			for (unsigned x = 0; x < SEL_N; x++) {
				unsigned j;

				if (!torn_to(op, p, wv[k - 1u], x))
					continue;
				tt->states++;
				if (g_sel_a < 2u)
					select_check(g_sel_a, false, x);
				j = judge_value(m, x);
				if (j == 0u)
					continue;
				tt->loss[j - 1u]++;
				if (op->kind == FF_ERASE)
					tt->erase_loss[j - 1u]++;
				else
					tt->other_loss[j - 1u]++;
			}
		} else {
			for (unsigned v = 0; v < 6u; v++) {
				const struct model *m;
				unsigned j;

				g_prep();
				if (v == 5u)
					g_f.cut_after = k;
				else
					tear(k, v);
				CHECK(write_all(N_IMAGE, g_sha, &g_trun) != FR_WR_OK || k == m_total);
				CHECK_EQ(g_f.nmut, k);
				m = model_of(flash_kind(0, b == 0u ? alive : true),
					     flash_kind(1, b == 1u ? alive : true));
				tt->states++;
				cls_done = 0;
				if (g_sel_a < 2u)
					select_check(g_sel_a, false, sel_value());
				j = judge_value(m, sel_value());
				if (j != 0u) {
					tt->loss[j - 1u]++;
					tt->other_loss[j - 1u]++;
				}
			}
		}
	}
	/* the whole write */
	g_prep();
	CHECK_EQ(write_all(N_IMAGE, g_sha, &g_trun), FR_WR_OK);
	{
		const struct model *m = model_of(flash_kind(0, b == 0u ? alive : true),
						 flash_kind(1, b == 1u ? alive : true));
		const unsigned j = judge_value(m, sel_value());

		cls_done = 0;
		if (g_sel_a < 2u)
			select_check(g_sel_a, false, sel_value());
		tt->states++;
		if (j != 0u) {
			tt->loss[j - 1u]++;
			tt->other_loss[j - 1u]++;
		}
	}
	return m_total;
}

/* The trial's start: A = slot a, running and selected by index idx with
 * verified 01; an image with another front (its front seed FRONT_SEED + 1),
 * BEGIN's flag bit 1. */
static uint8_t g_tidx;

static void prep_trial(void)
{
	setup(g_tidx, 0x01);
	g_flags = FR_BEGIN_F_TRIAL;
}

static void make_trial_image(uint32_t seed)
{
	g_fseed = FRONT_SEED + 1u;
	make_image(N_IMAGE, seed);
	g_fseed = FRONT_SEED;
}

/* The flash after a whole trial of B: B the image under the trial mark,
 * the selector {00, FE} for slot 0 or {FF, FF} for slot 1, A and its
 * record and log as they were. */
static void check_trial_final(unsigned b)
{
	const unsigned a = 1u - b;

	CHECK(memcmp(g_f.mem + FR_SLOT_BASE(b), g_img, N_IMAGE) == 0);
	CHECK_EQ(fr_le32(g_f.mem + FR_RECORD_OFF(b)), ~ref_crc32(0, g_img, N_IMAGE));
	CHECK_EQ(fr_le32(g_f.mem + FR_RECORD_OFF(b) + 4u), N_IMAGE);
	CHECK_EQ(fr_le32(g_f.mem + FR_RECORD_OFF(b) + 8u), FR_TAG);
	CHECK(!ff_slot_good(&g_f, b));
	CHECK_EQ(sel_value(), b == 0u ? 0xFE00u : 0xFFFFu);
	for (unsigned j = 2; j < FR_SECTOR; j++)
		CHECK_EQ(g_f.mem[FR_SELECTOR_OFF + j], 0xFFu);
	/* the start log of the slot written holds entry 0 pending, the rest
	 * free (S-02.095) */
	CHECK_EQ(g_f.mem[FR_LOG_OFF(b)], 0xFDu);
	for (uint32_t j = FR_LOG_IMAGE_MAX + 1u; j < FR_SLOT_CAP; j++)
		if (g_f.mem[FR_SLOT_BASE(b) + j] != 0xFFu) {
			CHECK(0);
			break;
		}
	CHECK(memcmp(g_f.mem + FR_SLOT_BASE(a), g_start.mem + FR_SLOT_BASE(a), FR_SLOT_CAP) == 0);
	CHECK(memcmp(g_f.mem + FR_RECORD_OFF(a), g_start.mem + FR_RECORD_OFF(a), FR_SECTOR) == 0);
	CHECK(memcmp(g_f.mem, g_start.mem, FR_SELECTOR_OFF) == 0);
	CHECK(ff_slot_good(&g_f, a));
}

/* The order of the trial's operations: B's slot, its log sector erased and
 * entry 0 programmed pending (0xFD, read back), its record (erased,
 * programmed with the trial mark), the read-back, then the selector: its
 * erase and, for slot 0, verified then the index, a byte at a time, each
 * read back. */
static void check_trial_log(unsigned b)
{
	const unsigned rec_p = find_op(FF_PROGRAM, FR_RECORD_OFF(b), 0);
	const unsigned e = find_op(FF_ERASE, FR_SELECTOR_OFF, 0);
	const unsigned le = find_op(FF_ERASE, FR_LOG_OFF(b), 0);
	const unsigned lp = find_op(FF_PROGRAM, FR_LOG_OFF(b), 0);
	const struct ff_op *o = g_f.log;

	CHECK(le != ~0u && lp == le + 1u && lp + 2u == find_op(FF_ERASE, FR_RECORD_OFF(b), 0));
	if (lp != ~0u && lp + 1u < g_f.nlog) {
		CHECK_EQ(o[lp].len, 1);
		CHECK(o[lp + 1u].kind == FF_READ && o[lp + 1u].off == FR_LOG_OFF(b) && o[lp + 1u].len == 1u);
	}
	CHECK(reads_cover(FR_LOG_OFF(b), 1u, find_op(FF_PROGRAM, FR_RECORD_OFF(b), 0), e));
	CHECK(rec_p != ~0u && e != ~0u && rec_p < e);
	CHECK(reads_cover(FR_SLOT_BASE(b), N_IMAGE, rec_p, e));
	CHECK(reads_cover(FR_RECORD_OFF(b), 12u, rec_p, e));
	CHECK_EQ(first_sel_mut(0), e);
	CHECK(e + 1u < g_f.nlog && o[e + 1u].kind == FF_READ && o[e + 1u].off == FR_SELECTOR_OFF);
	if (b == 1u) {
		CHECK_EQ(e + 2u, g_f.nlog);
		return;
	}
	CHECK_EQ(e + 6u, g_f.nlog);
	if (e + 6u != g_f.nlog)
		return;
	CHECK(o[e + 2u].kind == FF_PROGRAM && o[e + 2u].off == FR_SELECTOR_OFF + 1u && o[e + 2u].len == 1u);
	CHECK(o[e + 3u].kind == FF_READ && o[e + 3u].off == FR_SELECTOR_OFF);
	CHECK(o[e + 4u].kind == FF_PROGRAM && o[e + 4u].off == FR_SELECTOR_OFF && o[e + 4u].len == 1u);
	CHECK(o[e + 5u].kind == FF_READ && o[e + 5u].off == FR_SELECTOR_OFF);
	for (unsigned i = 0; i < g_nselb; i++)
		CHECK(g_selb[i][0] == 0x00u && g_selb[i][1] == 0xEEu);	/* the index alone */
}

/* The state after a whole trial and the trial start: B tried, the first
 * stage's own write made ({A, 01}), B booted once; with g_tried_mark, B's
 * front ran to its start log's step and marked entry 0 finished
 * (fr_log_trial_mark(): 0xFD to 0xFC). */
static bool g_tried_mark;

static void prep_tried(void)
{
	struct fr_running run;

	setup(g_tidx, 0x01);
	g_flags = FR_BEGIN_F_TRIAL;
	run = running_of(g_tidx);
	CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
	CHECK_EQ(g_w.way, FR_WAY_TRIAL);
	/* S-02.021-S-02.023: B's check fails, {A, 01}, B booted once */
	CHECK_EQ(boot_once(0), 1u - run.slot);
	CHECK_EQ(fr_selected_slot(g_f.mem[FR_SELECTOR_OFF]), run.slot);
	CHECK_EQ(g_f.mem[FR_SELECTOR_OFF + 1u], 0x01u);
	CHECK_EQ(g_f.mem[FR_LOG_OFF(1u - run.slot)], FR_LOG0_PENDING);
	if (g_tried_mark)
		g_f.mem[FR_LOG_OFF(1u - run.slot)] = FR_LOG0_MARKED;
	memset(&g_home, 0, sizeof g_home);	/* a new start of recovery */
	snapshot();
}

static unsigned g_sel_from_b;	/* the select made from the tried slot's own front */

/* From the old front only after the mark; from B's own front with none */
static void prep_select(void)
{
	g_tried_mark = g_sel_from_b == 0u;
	prep_tried();
	g_flags = FR_BEGIN_F_SELECT;
}

/* An earlier trial in B, of another image, whose own front marked its
 * start: B tried and started before this trial. */
static uint8_t g_old[N_IMAGE];

static bool old_bytes(unsigned b)
{
	return memcmp(g_f.mem + FR_SLOT_BASE(b), g_old, N_IMAGE) == 0 &&
	       fr_le32(g_f.mem + FR_RECORD_OFF(b)) == ~ref_crc32(0, g_old, N_IMAGE) &&
	       fr_le32(g_f.mem + FR_RECORD_OFF(b) + 4u) == N_IMAGE &&
	       fr_le32(g_f.mem + FR_RECORD_OFF(b) + 8u) == FR_TAG;
}

static void prep_trial_old(void)
{
	const unsigned b = 1u - fr_selected_slot(g_tidx);

	setup(g_tidx, 0x01);
	put_slot(b, g_old, N_IMAGE);
	ff_put32(g_f.mem + FR_RECORD_OFF(b), ~ref_crc32(0, g_old, N_IMAGE));
	g_f.mem[FR_LOG_OFF(b)] = FR_LOG0_MARKED;
	snapshot();
	g_flags = FR_BEGIN_F_TRIAL;
}

static void test_trial_cuts(void)
{
	static const uint8_t idx[] = { 0x00, 0x01, 0x02, 0xFF };
	static struct trial_tally old_tt[2];
	struct trial_tally sel_tt;
	unsigned ops_trial[2] = { 0, 0 }, ops_sel = 0;

	memset(g_tt, 0, sizeof g_tt);
	memset(old_tt, 0, sizeof old_tt);
	memset(&sel_tt, 0, sizeof sel_tt);
	memset(&g_st, 0, sizeof g_st);
	g_fseed = FRONT_SEED + 2u;
	(void)build_image(g_old, N_IMAGE, 64u);
	g_fseed = FRONT_SEED;
	make_trial_image(61u);
	for (unsigned i = 0; i < N(idx); i++) {
		const unsigned a = fr_selected_slot(idx[i]), b = 1u - a;
		struct trial_tally *tt = &g_tt[b];

		g_tidx = idx[i];
		g_trun = running_of(idx[i]);
		/* the whole trial, and its log */
		prep_trial();
		CHECK_EQ(write_all(N_IMAGE, g_sha, &g_trun), FR_WR_OK);
		CHECK_EQ(g_w.way, FR_WAY_TRIAL);
		CHECK_EQ(g_w.target, b);
		CHECK(!g_home.moved && !g_home.spoiled && !g_home.other_failed);
		check_trial_log(b);
		check_trial_final(b);
		CHECK_EQ(mutations(), SLOT_OPS + 1u + (b == 0u ? 3u : 1u));
		/* every cut, and from every state a select from the old front
		 * after a reset; then the same over an earlier trial in B whose
		 * front had marked its start */
		g_prep = prep_trial;
		g_sel_a = a;
		g_old_ok = NULL;
		ops_trial[b] = judge_write_cuts(b, false, tt);
		g_prep = prep_trial_old;
		g_old_ok = old_bytes;
		(void)judge_write_cuts(b, false, &old_tt[b]);
		g_old_ok = NULL;
		g_sel_a = 2;

		/* the select from both places, every cut: no loss */
		for (g_sel_from_b = 0; g_sel_from_b < 2u; g_sel_from_b++) {
			g_prep = prep_select;
			g_trun.established = true;
			g_trun.slot = (uint8_t)(g_sel_from_b ? b : a);
			prep_select();
			CHECK_EQ(write_all(N_IMAGE, g_sha, &g_trun), FR_WR_OK);
			CHECK_EQ(g_w.way, FR_WAY_SELECT);
			CHECK_EQ(g_w.target, b);
			ops_sel = judge_write_cuts(b, true, &sel_tt);
		}
	}
	/* the earlier trial changes nothing of the windows */
	for (unsigned b = 0; b < 2u; b++) {
		CHECK_EQ(old_tt[b].erase_loss[TL_SINGLE], g_tt[b].erase_loss[TL_SINGLE]);
		CHECK_EQ(old_tt[b].erase_loss[TL_FS], g_tt[b].erase_loss[TL_FS]);
		CHECK_EQ(old_tt[b].other_loss[TL_SINGLE], g_tt[b].other_loss[TL_SINGLE]);
		CHECK_EQ(old_tt[b].other_loss[TL_FS], g_tt[b].other_loss[TL_FS]);
	}
	/* no select from the old front accepted unless a start ran from B's
	 * bytes past its log step; marks made, spends made, both seen */
	CHECK_EQ(g_st.lost, 0);
	CHECK(g_st.refused[FR_WR_NOT_STARTED] > 0u && g_st.marked_prog > 0u && g_st.spent_prog > 0u);
	/* the model flash takes every program: each attempt took, and each
	 * spend had its further start */
	CHECK_EQ(g_st.marked_prog, g_st.marked);
	CHECK_EQ(g_st.spent_prog, g_st.spent);
	CHECK_EQ(g_st.further, g_st.spent_prog);
	CHECK(g_st.after_b > 0u && g_st.old_own > 0u);
	/* B as slot 0: no loss at all */
	CHECK_EQ(g_tt[0].loss[TL_SINGLE], 0);
	CHECK_EQ(g_tt[0].loss[TL_FS], 0);
	/* B as slot 1: only inside the two windows. Recovery's selector erase
	 * from {00, 01}, cut short: {nonzero, 01} trusts B at once (255
	 * values); any other value it can leave makes the first stage write
	 * {00, 01}, which a cut can tear to {nonzero, 01}. Then the trial
	 * start's own program of {00, 01} from {FF, FF}: the whole trial's
	 * state, one value, lost only under a cut of that program, as the
	 * next lines show. Nothing else. */
	CHECK_EQ(g_tt[1].erase_loss[TL_SINGLE], 255);
	CHECK_EQ(g_tt[1].other_loss[TL_SINGLE], 0);
	CHECK_EQ(g_tt[1].erase_loss[TL_FS], 256u * 127u);
	CHECK_EQ(g_tt[1].other_loss[TL_FS], 1);	/* the whole trial's {FF, FF} */
	{
		/* {FF, FF}: an erase leaves FF FF whatever its cut, and the
		 * program {00, 01} whole trusts A, good; torn it can leave
		 * {01, 01}, trusting B */
		const struct model *m = model_of(RK_GOOD, RK_BADCRC);

		CHECK(model_u(m, 0xFFFFu) && !model_g(m, 0xFFFFu));
		CHECK(model_g(m, 0x0100u) && !model_u(m, 0x0101u));
	}
	/* the selects: no loss in either place, either order */
	CHECK_EQ(sel_tt.loss[TL_SINGLE], 0);
	CHECK_EQ(sel_tt.loss[TL_FS], 0);
	printf("  trial, B = slot 1 (index 0): %u operations, %llu states, %llu lost\n", ops_trial[0],
	       g_tt[0].states, g_tt[0].loss[TL_SINGLE] + g_tt[0].loss[TL_FS]);
	printf("  trial, B = slot 2 (index 1): %u operations, %llu states; lost: %llu inside "
	       "recovery's selector erase from {00, 01} (%llu by the cut alone, %llu with the "
	       "first stage's next write cut too), %llu at {FF, FF} by a cut of the trial "
	       "start's program of {00, 01}, %llu elsewhere\n", ops_trial[1], g_tt[1].states,
	       g_tt[1].erase_loss[TL_SINGLE] + g_tt[1].erase_loss[TL_FS],
	       g_tt[1].erase_loss[TL_SINGLE], g_tt[1].erase_loss[TL_FS],
	       g_tt[1].other_loss[TL_FS], g_tt[1].other_loss[TL_SINGLE]);
	printf("  select, both places, both orders: %u operations from the old front, %u from the "
	       "tried slot's own (its log's mark first), %llu states, %llu lost\n", ops_sel - 1u,
	       ops_sel, sel_tt.states, sel_tt.loss[TL_SINGLE] + sel_tt.loss[TL_FS]);
	printf("  each remaining loss needs a second fault, a dead front: the new front failing "
	       "before its key read on the start it gets (the %llu also need the first stage's "
	       "next write cut)\n", g_tt[1].erase_loss[TL_FS]);
	print_sel("from every state a cut of the trial leaves (also over an earlier, started trial)");
}

/* A model's states from which a path of cuts of the first stage's own
 * writes trusts a bad slot, OR-ed over the supersets: what an erase from a
 * value can leave. */
static uint8_t g_gsup[2][SEL_N];

static void build_gsup(unsigned b, const struct model *m)
{
	for (unsigned s = 0; s < SEL_N; s++)
		g_gsup[b][s] = (uint8_t)(model_u(m, s) && !model_g(m, s));
	for (unsigned bit = 1; bit < SEL_N; bit <<= 1)
		for (unsigned s = 0; s < SEL_N; s++)
			if (!(s & bit))
				g_gsup[b][s] |= g_gsup[b][s | bit];
}

/*
 * A selector step of the trial that fails (S-02.093): the erase, the 0xFE
 * program or the index program, leaving any value the step can leave, or
 * reading back otherwise. The writer settles the selector back on A, a
 * byte at a time, and ends 11. Every operation of that settling is judged
 * as a cut over every value it can leave (B's front dead); it ends with A
 * trusted, or for A = slot 1 named; and a select from the old front, in
 * this recovery (19) and after a reset, is refused and writes nothing.
 */
static void test_trial_settle_back(void)
{
	static const uint8_t idx[] = { 0x00, 0x01, 0x02, 0xFF };
	static struct ff_op ops[256];
	static unsigned wv[256];
	unsigned long long fails[2] = { 0, 0 }, sops[2] = { 0, 0 };
	unsigned long long lost[2][TL_KINDS], lost_other[2] = { 0, 0 }, quiet[2] = { 0, 0 };
	const struct model *ms[2];

	memset(lost, 0, sizeof lost);
	memset(&g_st, 0, sizeof g_st);
	g_old_ok = NULL;
	ms[0] = model_of(RK_BADCRC, RK_GOOD);	/* B = slot 0 tried */
	ms[1] = model_of(RK_GOOD, RK_BADCRC);
	build_gsup(0, ms[0]);
	build_gsup(1, ms[1]);
	make_trial_image(61u);
	for (unsigned i = 0; i < N(idx); i++) {
		const unsigned a = fr_selected_slot(idx[i]), b = 1u - a;
		const struct fr_running run = running_of(idx[i]);
		unsigned m_total = 0;

		g_tidx = idx[i];
		prep_trial();
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
		for (unsigned j = 0; j < g_f.nlog && j < FF_LOG_MAX; j++)
			if (g_f.log[j].kind != FF_READ && m_total < N(ops)) {
				ops[m_total] = g_f.log[j];
				wv[m_total] = written(&g_f.log[j], m_total + 1u);
				m_total++;
			}
		for (unsigned k = 1; k <= m_total; k++) {
			const struct ff_op *op = &ops[k - 1u];
			struct fr_writer w0;
			struct fr_home h0;
			unsigned p, steps = 0;

			if (op->off < FR_SELECTOR_OFF || op->off >= FR_SELECTOR_OFF + FR_SECTOR)
				continue;
			prep_trial();
			CHECK_EQ(fr_writer_begin(&g_w, flash(), &run, &g_home, g_img, N_IMAGE, g_sha,
						 g_flags), FR_WR_OK);
			while (g_f.nmut < k - 1u && steps++ < 400u)
				CHECK(!fr_writer_step(&g_w));
			CHECK_EQ(g_f.nmut, k - 1u);
			w0 = g_w;
			h0 = g_home;
			CHECK(h0.trial);
			p = sel_value();
			cls_done = 0;
			for (unsigned x = 0; x < SEL_N; x++) {
				unsigned v = x, n = 0;

				if (!torn_to(op, p, wv[k - 1u], x))
					continue;
				g_w = w0;
				g_home = h0;
				g_f.mem[FR_SELECTOR_OFF] = (uint8_t)p;
				g_f.mem[FR_SELECTOR_OFF + 1u] = (uint8_t)(p >> 8);
				g_f.nmut = k - 1u;
				g_f.nlog = 0;
				g_f.dead = false;
				g_fail_mut = k;
				g_fail_val = x;
				for (steps = 0; !fr_writer_step(&g_w) && steps < 16u; steps++)
					;
				CHECK_EQ(g_w.result, FR_WR_SELECTOR);
				CHECK(g_home.trial);
				fails[b]++;
				/* the settling's operations after the failed one (the
				 * first erase or program in the log) */
				for (unsigned j = 0, seen = 0; j < g_f.nlog; j++) {
					const struct ff_op *s = &g_f.log[j];
					const unsigned ws = written(s, k + 1u + n);
					bool single = false, fs = false;

					if (s->kind == FF_READ)
						continue;
					if (!seen) {
						seen = 1;
						continue;
					}
					n++;
					sops[b]++;
					CHECK(s->off >= FR_SELECTOR_OFF && s->off < FR_SELECTOR_OFF + 2u);
					if (s->kind == FF_ERASE) {
						single = ms[b]->lost_sup[v] != 0u;
						fs = !single && g_gsup[b][v] != 0u;
						v = 0xFFFFu;
					} else {
						CHECK_EQ(s->len, 1);	/* a byte at a time */
						for (unsigned y = 0; y < 256u; y++) {
							const unsigned t = s->off == FR_SELECTOR_OFF ?
									   (v & 0xFF00u) | y : (v & 0x00FFu) | y << 8;
							const unsigned jv = torn_to(s, v, ws, t) ? judge_value(ms[b], t) : 0u;

							single |= jv == 1u + TL_SINGLE;
							fs |= jv == 1u + TL_FS;
						}
						v &= ws;
					}
					if (single || fs) {
						lost[b][single ? TL_SINGLE : TL_FS]++;
						if (op->kind != FF_ERASE)
							lost_other[b]++;
					}
				}
				if (n == 0u)
					quiet[b]++;
				CHECK_EQ(v, sel_value());
				CHECK_EQ(judge_value(ms[b], v), 0);
				/* settled on A: trusted, or for slot 1 named */
				CHECK_EQ(fr_selected_slot((uint8_t)v), a);
				CHECK((v >> 8) == 0x01u || a == 1u);
				select_check(a, true, v);
				select_check(a, false, v);
			}
		}
	}
	g_fail_mut = 0;
	CHECK_EQ(lost[0][TL_SINGLE] + lost[0][TL_FS], 0);
	CHECK_EQ(lost_other[1], 0);
	CHECK_EQ(g_st.lost, 0);
	CHECK(g_st.spent_prog > 0u && g_st.marked_prog == 0u);	/* only A starts after it */
	CHECK_EQ(g_st.spent_prog, g_st.spent);
	CHECK_EQ(g_st.further, g_st.spent_prog);
	printf("  trial selector step failed: B = slot 1: %llu failures, %llu settling operations "
	       "(%llu failures needed none), %llu lost; B = slot 2: %llu failures, %llu settling "
	       "operations (%llu needed none), lost under a cut of the settling %llu by the cut "
	       "alone and %llu with the first stage's next write cut too, all after recovery's "
	       "erase from {00, 01} failed\n", fails[0], sops[0], quiet[0],
	       lost[0][TL_SINGLE] + lost[0][TL_FS], fails[1], sops[1], quiet[1],
	       lost[1][TL_SINGLE], lost[1][TL_FS]);
	print_sel("after every failed step (19 in the same recovery, then after a reset)");
}

/* A cut inside the select's record rewrite leaves A selected and trusted,
 * and a later trial goes; the select from both places ends right. */
static void test_select_ends(void)
{
	static const uint8_t idx[] = { 0x00, 0x01 };

	make_trial_image(62u);
	for (unsigned i = 0; i < N(idx); i++)
		for (unsigned from_b = 0; from_b < 2u; from_b++) {
			const unsigned a = fr_selected_slot(idx[i]), b = 1u - a;
			struct fr_running run;
			unsigned rec_ops = 0;

			g_tidx = idx[i];
			g_sel_from_b = from_b;
			run.established = true;
			run.slot = (uint8_t)(from_b ? b : a);
			/* whole: from B's own front its log's entry 0 marked first;
			 * nothing else in B's slot or log sector, its record right,
			 * the selector {B, 00} */
			prep_select();
			CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
			CHECK_EQ(g_w.way, FR_WAY_SELECT);
			CHECK_EQ(g_w.target, b);
			CHECK_EQ(mutations(), 4u + from_b);
			CHECK_EQ(g_f.mem[FR_LOG_OFF(b)], FR_LOG0_MARKED);
			if (from_b) {
				CHECK(find_op(FF_PROGRAM, FR_LOG_OFF(b), 0) <
				      find_op(FF_ERASE, FR_RECORD_OFF(b), 0));
				g_start.mem[FR_LOG_OFF(b)] = FR_LOG0_MARKED;
			}
			CHECK(memcmp(g_f.mem + FR_SLOT_BASE(b), g_start.mem + FR_SLOT_BASE(b), FR_SLOT_CAP) == 0);
			CHECK(memcmp(g_f.mem + FR_SLOT_BASE(a), g_start.mem + FR_SLOT_BASE(a), FR_SLOT_CAP) == 0);
			CHECK(memcmp(g_f.mem + FR_RECORD_OFF(a), g_start.mem + FR_RECORD_OFF(a), FR_SECTOR) == 0);
			CHECK(ff_slot_good(&g_f, b) && ff_slot_good(&g_f, a));
			CHECK_EQ(sel_value(), b);
			CHECK(memcmp(g_w.readback_sha, g_sha, 32) == 0);
			for (unsigned k = 0; k < g_f.nlog; k++)
				if (g_f.log[k].kind != FF_READ && g_f.log[k].off == FR_RECORD_OFF(b))
					rec_ops++;
			CHECK_EQ(rec_ops, 2);
			/* the next start checks B and trusts it */
			CHECK_EQ(boot_once(0), b);
			CHECK_EQ(sel_value(), b | 0x0100u);
			/* cuts in the log's mark and the record's erase and
			 * program: A still selected and trusted, and a later trial
			 * goes */
			for (unsigned k = 1; k <= 2u + from_b; k++)
				for (unsigned v = 0; v < 6u; v++) {
					struct fr_running ra = { true, (uint8_t)a };

					prep_select();
					if (v == 5u)
						g_f.cut_after = k;
					else
						tear(k, v);
					CHECK(write_all(N_IMAGE, g_sha, &run) != FR_WR_OK);
					CHECK_EQ(fr_selected_slot(g_f.mem[FR_SELECTOR_OFF]), a);
					CHECK_EQ(g_f.mem[FR_SELECTOR_OFF + 1u], 0x01u);
					CHECK(ff_slot_good(&g_f, a));
					ff_reset(&g_f);
					memset(&g_home, 0, sizeof g_home);
					g_flags = FR_BEGIN_F_TRIAL;
					CHECK_EQ(write_all(N_IMAGE, g_sha, &ra), FR_WR_OK);
					CHECK_EQ(g_w.way, FR_WAY_TRIAL);
					CHECK_EQ(g_w.target, b);
				}
		}
}

/* Every refusal of the trial and the select: nothing erased or programmed,
 * home as it was. */
static void test_trial_refusals(void)
{
	for (unsigned i = 0; i < 2u; i++) {
		const uint8_t index = i ? 0x01 : 0x00;
		const unsigned a = fr_selected_slot(index), b = 1u - a;
		struct fr_running run = running_of(index), rb = { true, (uint8_t)b };

		make_trial_image(63u);
		/* 12: another front with bit 0, or with no flag */
		setup(index, 0x01);
		refused(FR_WR_FRONT, run, N_IMAGE, g_sha);
		g_flags = FR_BEGIN_F_RUNNING;
		refused(FR_WR_FRONT, run, N_IMAGE, g_sha);
		/* 19: A bad, its front intact */
		setup(index, 0x01);
		spoil_main(a);
		snapshot();
		g_flags = FR_BEGIN_F_TRIAL;
		refused(FR_WR_TRIAL_STATE, run, N_IMAGE, g_sha);
		/* 19: this recovery wrote the selector to B ({B, 01}, B good) */
		setup(index, 0x01);
		ff_set_selector(&g_f, (uint8_t)b, 0x01);
		snapshot();
		g_home.moved = true;
		g_flags = FR_BEGIN_F_TRIAL;
		refused(FR_WR_TRIAL_STATE, run, N_IMAGE, g_sha);
		g_flags = FR_BEGIN_F_SELECT;
		refused(FR_WR_TRIAL_STATE, run, N_IMAGE, g_sha);
		/* 19: a write of A was begun, A good again with the selector
		 * settled */
		setup(index, 0x01);
		g_home.spoiled = true;
		g_flags = FR_BEGIN_F_TRIAL;
		refused(FR_WR_TRIAL_STATE, run, N_IMAGE, g_sha);
		g_flags = FR_BEGIN_F_SELECT;
		refused(FR_WR_TRIAL_STATE, run, N_IMAGE, g_sha);
		/* the start condition first: verified 00, not running */
		setup(index, 0x00);
		g_flags = FR_BEGIN_F_TRIAL;
		refused(FR_WR_UNVERIFIED, run, N_IMAGE, g_sha);
		run.established = false;
		refused(FR_WR_NOT_RUNNING, run, N_IMAGE, g_sha);
		g_flags = FR_BEGIN_F_SELECT;
		refused(FR_WR_NOT_RUNNING, run, N_IMAGE, g_sha);
		run.established = true;
		/* 20: no slot tried */
		setup(index, 0x01);
		g_flags = FR_BEGIN_F_SELECT;
		refused(FR_WR_NOT_TRIED, run, N_IMAGE, g_sha);
		refused(FR_WR_NOT_TRIED, rb, N_IMAGE, g_sha);
		/* after the trial: 3 for a trial or a write, 20 for a select
		 * (B is tried but nothing has put the selector back) */
		g_tidx = index;
		prep_trial();
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
		snapshot();
		memset(&g_home, 0, sizeof g_home);
		refused(FR_WR_NOT_SELECTED, run, N_IMAGE, g_sha);
		g_flags = 0;
		refused(FR_WR_NOT_SELECTED, run, N_IMAGE, g_sha);
		g_flags = FR_BEGIN_F_SELECT;
		refused(FR_WR_NOT_TRIED, run, N_IMAGE, g_sha);
		/* B's own front in that state, before any first stage ran: the
		 * selector names B, not A with verified 01 */
		refused(FR_WR_NOT_TRIED, rb, N_IMAGE, g_sha);
		/* 19: a trial begun in this recovery */
		setup(index, 0x01);
		g_home.trial = true;
		g_flags = FR_BEGIN_F_SELECT;
		refused(FR_WR_TRIAL_STATE, run, N_IMAGE, g_sha);
		refused(FR_WR_TRIAL_STATE, rb, N_IMAGE, g_sha);
		/* after the trial start with no mark of it: 21 from the old
		 * front, and from B's own front the select goes */
		g_tried_mark = false;
		prep_tried();
		g_flags = FR_BEGIN_F_SELECT;
		refused(FR_WR_NOT_STARTED, run, N_IMAGE, g_sha);
		/* a trial a start of A spent (F9): 21 from the old front; from
		 * B's own front 20, its log left as it is */
		prep_tried();
		g_f.mem[FR_LOG_OFF(b)] = 0xF9u;
		snapshot();
		g_flags = FR_BEGIN_F_SELECT;
		refused(FR_WR_NOT_STARTED, run, N_IMAGE, g_sha);
		refused(FR_WR_NOT_TRIED, rb, N_IMAGE, g_sha);
		/* after the trial start: another image, or the image one byte
		 * changed: 20 from either place, before 21 */
		prep_tried();
		g_flags = FR_BEGIN_F_SELECT;
		{
			uint8_t sha[32];

			g_img[MAIN_BYTE] ^= 0x01u;
			sha256(sha, g_img, N_IMAGE);
			refused(FR_WR_NOT_TRIED, run, N_IMAGE, sha);
			refused(FR_WR_NOT_TRIED, rb, N_IMAGE, sha);
			g_img[MAIN_BYTE] ^= 0x01u;
			/* another length */
			sha256(sha, g_img, N_IMAGE - 16u);
			refused(FR_WR_NOT_TRIED, run, N_IMAGE - 16u, sha);
		}
		/* in the trial start, nothing but the select */
		g_flags = 0;
		refused(FR_WR_NOT_SELECTED, rb, N_IMAGE, g_sha);
		g_flags = FR_BEGIN_F_TRIAL;
		refused(FR_WR_NOT_SELECTED, rb, N_IMAGE, g_sha);
		g_flags = FR_BEGIN_F_RUNNING;
		refused(FR_WR_NOT_SELECTED, rb, N_IMAGE, g_sha);
		/* the slot not selected holds the image too, and is good: the
		 * select goes all the same */
		g_tried_mark = true;
		prep_tried();
		memcpy(g_f.mem + FR_SLOT_BASE(a), g_f.mem + FR_SLOT_BASE(b), FR_SLOT_CAP);
		ff_put32(g_f.mem + FR_RECORD_OFF(a), ref_crc32(0, g_img, N_IMAGE));
		ff_put32(g_f.mem + FR_RECORD_OFF(a) + 4u, N_IMAGE);
		snapshot();
		CHECK(ff_slot_good(&g_f, a));
		g_flags = FR_BEGIN_F_SELECT;
		CHECK_EQ(write_all(N_IMAGE, g_sha, &run), FR_WR_OK);
		CHECK_EQ(g_w.way, FR_WAY_SELECT);
		CHECK(ff_slot_good(&g_f, a) && ff_slot_good(&g_f, b));
	}
}

int main(void)
{
	test_split_programs();
	test_reach_proof();
	test_controls();
	test_settle_every_value();
	test_front_every_start();
	test_front_representatives();
	test_unsettled();
	test_by_programs();
	test_record_cuts();
	test_fallback_order();
	test_write_ok();
	test_extent();
	test_power_cuts();
	test_cells();
	test_refusals();
	test_faults();
	test_front_and_same();
	test_readback_sha();
	test_selector_readback();
	test_select_first_fails();
	test_cuts_select_first();
	test_cuts_in_place();
	test_retry_select_first();
	test_retry_in_place();
	test_in_place_gate();
	test_own_image();
	test_damaged_plain();
	test_trial_cuts();
	test_select_ends();
	test_trial_refusals();
	test_trial_settle_back();
	return finish("test_front_writer");
}
