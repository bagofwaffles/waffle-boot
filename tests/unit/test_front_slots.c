/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's view of the SPI flash (src/front/fr_slots.c): S-02.091's
 * rules on a slot image against bootable_images.json and libwaffle's
 * s01_stream_check(), the front's own condition on an image, the selector
 * and record readers and the slot check against records.json's states
 * (S-02.010-S-02.016), slot information read from the flash, and the
 * running slot against libwaffle's s02_running_slot() (S-01.002), which
 * the front establishes only on a match of every byte its map lets it
 * compare.
 */
#include <string.h>

#include "fr_map_rule.h"
#include "s02_check.h"
#include "s02_fake_flash.h"
#include "vectors/s02_vectors.h"

#include "front/fr.h"
#include "front/fr_handoff.h"
#include "front/fr_slots.h"
#include "waffle/s01_memmap.h"
#include "waffle/s05_handoff.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static struct fake_flash g_f;
static uint8_t g_img[FR_SLOT_CAP + 1u];

static const struct fr_flash *flash(void)
{
	static struct fr_flash ops;

	ops.read = ff_read;
	ops.erase = ff_erase;
	ops.program = ff_program;
	ops.ctx = &g_f;
	return &ops;
}

/* --- LDR streams --- */

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

/* A synthetic image with a front, as tools/mkldr.py --front lays it out:
 * L1 blocks, the INIT block, the rest, the descriptor among them, FINAL. */
struct fopt {
	uint32_t entry, final, init;
	bool no_desc, two_desc, bad_fcrc, bad_desc_crc, straddle, front_fill;
};

#define ENTRY 0x08002000u
#define INIT  0x11A04000u

static uint32_t build_front(uint8_t *s, const struct fopt *o)
{
	uint8_t main_l1[96], f_l1c[128], f_l1d[64], f_l2[512], main_l2[256], ddr[300], cross[0x200];
	uint8_t desc[FR_DESC_MAX];
	uint32_t off = 0, fcrc, mcrc, size, i;
	static const uint32_t regions[3][2] = {
		{0x11A00000u, 96u}, {0x08031000u, 256u}, {0x84000000u, 300u},
	};

	ff_fill_image(main_l1, sizeof main_l1, 1);
	ff_fill_image(f_l1c, sizeof f_l1c, 2);
	ff_fill_image(f_l1d, sizeof f_l1d, 3);
	ff_fill_image(f_l2, sizeof f_l2, 4);
	ff_fill_image(main_l2, sizeof main_l2, 5);
	ff_fill_image(ddr, sizeof ddr, 6);
	ff_fill_image(cross, sizeof cross, 7);
	fcrc = ref_crc32(0, f_l1c, sizeof f_l1c);
	fcrc = ref_crc32(fcrc, f_l1d, sizeof f_l1d);
	fcrc = ref_crc32(fcrc, f_l2, sizeof f_l2);
	mcrc = ref_crc32(0, main_l1, sizeof main_l1);
	mcrc = ref_crc32(mcrc, main_l2, sizeof main_l2);
	mcrc = ref_crc32(mcrc, ddr, sizeof ddr);

	size = FR_DESC_HEAD + 8u * 3u + 4u;
	memset(desc, 0, sizeof desc);
	ff_put32(desc + FR_DESC_O_MAGIC, FR_DESC_MAGIC);
	desc[FR_DESC_O_VERSION] = (uint8_t)FR_DESC_VERSION;
	desc[FR_DESC_O_SIZE] = (uint8_t)size;
	ff_put32(desc + FR_DESC_O_MAIN_ENTRY, 0x08031000u);
	ff_put32(desc + FR_DESC_O_NREGIONS, 3);
	ff_put32(desc + FR_DESC_O_MAIN_CRC, mcrc);
	ff_put32(desc + FR_DESC_O_FRONT_CRC, fcrc ^ (o->bad_fcrc ? 1u : 0u));
	for (i = 0; i < 4u; i++)
		desc[FR_DESC_O_BL_VERSION + 2u * i] = (uint8_t)(i + 1u);
	memset(desc + FR_DESC_O_FRONT_PIN, 0x5A, FR_SHA256_LEN);
	for (i = 0; i < 3u; i++) {
		ff_put32(desc + FR_DESC_O_REGIONS + 8u * i, regions[i][0]);
		ff_put32(desc + FR_DESC_O_REGIONS + 8u * i + 4u, regions[i][1]);
	}
	ff_put32(desc + size - 4u, ref_crc32(0, desc, size - 4u) ^ (o->bad_desc_crc ? 1u : 0u));

	off = put_block(s, off, S05_FLAG_FIRST | S05_FLAG_IGNORE, o->entry, 0, NULL);
	off = put_block(s, off, 0, 0x11A00000u, sizeof main_l1, main_l1);
	off = put_block(s, off, 0, 0x11A04000u, sizeof f_l1c, f_l1c);
	off = put_block(s, off, 0, 0x11802000u, sizeof f_l1d, f_l1d);
	if (o->straddle)	/* main's L1 data A into the front's */
		off = put_block(s, off, 0, 0x11801F00u, sizeof cross, cross);
	if (o->front_fill)
		off = put_block(s, off, S05_FLAG_FILL, 0x11803000u, 0x40, NULL);
	off = put_block(s, off, S05_FLAG_INIT, o->init, 0, NULL);
	off = put_block(s, off, 0, 0x08002000u, sizeof f_l2, f_l2);
	if (!o->no_desc)
		off = put_block(s, off, 0, FR_DESC_ADDR, size, desc);
	if (o->two_desc)
		off = put_block(s, off, 0, FR_DESC_ADDR, size, desc);
	off = put_block(s, off, 0, 0x08031000u, sizeof main_l2, main_l2);
	off = put_block(s, off, 0, 0x84000000u, sizeof ddr, ddr);
	off = put_block(s, off, S05_FLAG_FINAL, o->final, 0, NULL);
	return off;
}

static const struct fopt good_opt = {ENTRY, ENTRY, INIT, false, false, false, false, false, false};

/* --- S-02.091 against the vectors and libwaffle --- */

/* The first rule of S-02.091 libwaffle's verdict fails, as fr_image_rc. */
static enum fr_image_rc expected(uint32_t fail)
{
	if (fail & S01_SRULE_PARSE)
		return FR_IMG_STREAM;
	if (fail & (S01_SRULE_ONE_INIT_BLOCK | S01_SRULE_INIT_IN_L1 | S01_SRULE_INIT_BEFORE_DDR))
		return FR_IMG_INIT;
	if (fail & (S01_SRULE_INSIDE_SPACES | S01_SRULE_ROM_BOOT_RESERVE |
		    S01_SRULE_RECORD_AND_ROM_STACK))
		return FR_IMG_SPACES;
	if (fail & S01_SRULE_SPARES_FS_STACK)
		return FR_IMG_STACK;
	if (fail & S01_SRULE_BCODE)
		return FR_IMG_BCODE;
	return FR_IMG_OK;
}

static uint32_t lib_check(const uint8_t *img, uint32_t n)
{
	static const struct s01_range st = {S01_FACTORY_FS_STACK_START, S01_FACTORY_FS_STACK_END};

	return s01_stream_check(S01_BOOTLOADER, img, n, &st);
}

static void test_bootable_vectors(void)
{
	unsigned i, slots = 0;

	for (i = 0; i < N(v_bootables); i++) {
		const struct v_bootable *c = &v_bootables[i];
		uint32_t fail;
		enum fr_image_rc rc, want;

		if (!c->slot)
			continue;	/* first-stage images: no slot writer */
		slots++;
		fail = lib_check(c->image, c->length);
		rc = fr_image_valid(c->image, c->length, NULL);
		want = expected(fail);
		if (c->accepted) {
			CHECK_EQ(fail, 0);
			/* every rule met; none of these carries a front */
			CHECK_EQ(rc, FR_IMG_NO_FRONT);
		} else {
			CHECK(fail & c->refused);
			CHECK(want != FR_IMG_OK);
			CHECK_EQ(rc, want);
			CHECK(want == expected(c->refused) || (fail & ~c->refused) != 0u);
		}
	}
	CHECK_EQ(slots, 20);

	/* S-02.084: small_slot padded with 0xFF; recovery's bound is 0x76000,
	 * which leaves the settings sector (S-02.103) and the slot's last
	 * sector, the start log (S-02.095) */
	{
		static const uint32_t sizes[] = {0x2345u, FR_IMAGE_MAX, FR_IMAGE_MAX + 1u,
						 FR_LOG_IMAGE_MAX, FR_LOG_IMAGE_MAX + 1u,
						 FR_SLOT_CAP, FR_SLOT_CAP + 1u};

		for (i = 0; i < N(sizes); i++) {
			memset(g_img, 0xFF, sizes[i]);
			memcpy(g_img, v_boot_small_slot, sizeof v_boot_small_slot);
			CHECK_EQ(fr_image_valid(g_img, sizes[i], NULL),
				 sizes[i] > 0x76000u ? FR_IMG_SIZE : FR_IMG_NO_FRONT);
		}
		CHECK_EQ(fr_image_valid(g_img, 0, NULL), FR_IMG_SIZE);
		CHECK_EQ(fr_image_valid(v_boot_one_byte, 1, NULL), FR_IMG_STREAM);
	}
}

/* Mutated streams: the verdict agrees with libwaffle's on every one. */
static void test_rules_agree_random(void)
{
	uint8_t img[4096];
	uint32_t x = 12345u, n;
	unsigned t, passed = 0;

	n = build_front(img, &good_opt);
	CHECK_EQ(lib_check(img, n), 0);
	for (t = 0; t < 20000u; t++) {
		uint8_t m[4096];
		unsigned flips, j;

		memcpy(m, img, n);
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		flips = 1u + x % 3u;
		for (j = 0; j < flips; j++) {
			uint32_t at;

			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			/* mostly the header words, which the rules read */
			at = (x >> 8) % n;
			m[at] = (uint8_t)(m[at] ^ (1u << (x % 8u)));
		}
		/* fix up the checksum of every header still signed, sometimes */
		if (x & 0x100u) {
			struct s05_block b;
			uint32_t off = 0;

			while (off + 16u <= n && m[off + 3] == 0xADu) {
				uint8_t s = 0;

				m[off + 2] = 0;
				for (j = 0; j < 16u; j++)
					s = (uint8_t)(s ^ m[off + j]);
				m[off + 2] = s;
				if (s05_ldr_next(m, n, &off, &b) != 0 || (b.flags & S05_FLAG_FINAL))
					break;
			}
		}
		{
			uint32_t fail = lib_check(m, n);
			enum fr_image_rc rc = fr_image_valid(m, n, NULL);

			if (fail != 0u) {
				CHECK_EQ(rc, expected(fail));
			} else {
				CHECK(rc == FR_IMG_OK || rc == FR_IMG_NO_FRONT);
				passed++;
			}
		}
	}
	CHECK(passed > 0u);
}

/* --- The front's condition (fr_slots.h) --- */

static void test_front_condition(void)
{
	struct fr_desc d;
	struct fopt o;
	uint32_t n;

	n = build_front(g_img, &good_opt);
	CHECK_EQ(lib_check(g_img, n), 0);
	memset(&d, 0, sizeof d);
	CHECK_EQ(fr_image_valid(g_img, n, &d), FR_IMG_OK);
	CHECK_EQ(d.main_entry, 0x08031000u);
	CHECK_EQ(d.n_regions, 3);
	CHECK_EQ(d.bl_version[0], 1);
	CHECK_EQ(d.bl_version[3], 4);
	CHECK_EQ(d.addr[2], 0x84000000u);
	/* trailing bytes after FINAL, up to 0x76000 (S-02.091 rule 1, S-02.084) */
	memset(g_img + n, 0xFF, FR_SLOT_CAP - n);
	CHECK_EQ(fr_image_valid(g_img, 0x76000u, NULL), FR_IMG_OK);
	CHECK_EQ(fr_image_valid(g_img, 0x76001u, NULL), FR_IMG_SIZE);
	CHECK_EQ(fr_image_valid(g_img, 0x77000u, NULL), FR_IMG_SIZE);
	CHECK_EQ(fr_image_valid(g_img, FR_SLOT_CAP, NULL), FR_IMG_SIZE);
	CHECK_EQ(fr_image_valid(g_img, FR_SLOT_CAP + 1u, NULL), FR_IMG_SIZE);

	/* no descriptor */
	o = good_opt;
	o.no_desc = true;
	n = build_front(g_img, &o);
	CHECK_EQ(lib_check(g_img, n), 0);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_NO_FRONT);
	/* two */
	o = good_opt;
	o.two_desc = true;
	n = build_front(g_img, &o);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_NO_FRONT);
	/* a descriptor that does not parse */
	o = good_opt;
	o.bad_desc_crc = true;
	n = build_front(g_img, &o);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_NO_FRONT);
	/* its front_crc does not match the front's bytes */
	o = good_opt;
	o.bad_fcrc = true;
	n = build_front(g_img, &o);
	CHECK_EQ(lib_check(g_img, n), 0);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_NO_FRONT);
	/* one front byte changed: the same */
	n = build_front(g_img, &good_opt);
	g_img[16u + 16u + 96u + 16u + 5u] ^= 0x01u;	/* in the front's L1 code block */
	CHECK_EQ(lib_check(g_img, n), 0);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_NO_FRONT);
	/* the entry outside the front's L2 window: FIRST, FINAL, both */
	o = good_opt;
	o.entry = 0x08031000u;
	n = build_front(g_img, &o);
	CHECK_EQ(lib_check(g_img, n), 0);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_NO_FRONT);
	o = good_opt;
	o.final = 0x08031000u;
	n = build_front(g_img, &o);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_NO_FRONT);
	o = good_opt;
	o.entry = o.final = 0x11A04000u;	/* the front, but not its L2 */
	n = build_front(g_img, &o);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_NO_FRONT);
	o = good_opt;
	o.entry = o.final = FR_L2_END - 4u;	/* the last word of the window */
	n = build_front(g_img, &o);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_OK);
	/* the INIT target in L1 code, but not the front's */
	o = good_opt;
	o.init = 0x11A00000u;
	n = build_front(g_img, &o);
	CHECK_EQ(lib_check(g_img, n), 0);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_NO_FRONT);
	/* a block half in a front window, and a FILL in one */
	o = good_opt;
	o.straddle = true;
	n = build_front(g_img, &o);
	CHECK_EQ(lib_check(g_img, n), 0);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_NO_FRONT);
	o = good_opt;
	o.front_fill = true;
	n = build_front(g_img, &o);
	CHECK_EQ(lib_check(g_img, n), 0);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_NO_FRONT);
	/* a stream rule still comes first: the good front with BCODE 3 */
	n = build_front(g_img, &good_opt);
	g_img[0] = (uint8_t)((g_img[0] & 0xF0u) | 0x3u);
	g_img[2] = (uint8_t)(g_img[2] ^ 0x7u);
	CHECK_EQ(lib_check(g_img, n), S01_SRULE_BCODE);
	CHECK_EQ(fr_image_valid(g_img, n, NULL), FR_IMG_BCODE);
}

/* --- Formats and the slot check (records.json) --- */

static void test_selector_and_records(void)
{
	const struct fr_flash *f = flash();
	struct s02_flash_ops ops;
	struct s02_env env = ff_env(&g_f, &ops);
	unsigned i, s0, s1;

	ff_init(&g_f);
	for (i = 0; i < N(v_selectors); i++) {
		struct fr_selector s;

		ff_set_selector(&g_f, v_selectors[i].index, v_selectors[i].verified);
		CHECK_EQ(fr_read_selector(f, &s), FR_FLASH_OK);
		CHECK_EQ(s.index, v_selectors[i].index);
		CHECK_EQ(s.verified, v_selectors[i].verified);
		CHECK_EQ(fr_selected_slot(s.index), v_selectors[i].slot);	/* S-02.011 */
	}
	for (i = 0; i < 256u; i++)
		CHECK_EQ(fr_selected_slot((uint8_t)i), i == 0u ? 0u : 1u);
	for (i = 0; i < N(v_records); i++) {
		struct fr_record r;
		unsigned k;

		for (k = 0; k < 2u; k++) {
			memcpy(g_f.mem + FR_RECORD_OFF(k), v_records[i].bytes, 12);
			CHECK_EQ(fr_read_record(f, k, &r), FR_FLASH_OK);
			CHECK_EQ(r.crc32, v_records[i].crc32);
			CHECK_EQ(r.length, v_records[i].length);
			CHECK_EQ(r.tag, v_records[i].tag);
		}
	}
	/* S-02.016: every pair of the vectors' states */
	for (s0 = 0; s0 < N(v_states); s0++) {
		for (s1 = 0; s1 < N(v_states); s1++) {
			bool good[2], lib;
			struct fr_record r;

			ff_init(&g_f);
			ff_set_slot(&g_f, 0, v_states[s0], 0x1345u, 1u + s0);
			ff_set_slot(&g_f, 1, v_states[s1], 0x2345u, 9u + s1);
			for (unsigned k = 0; k < 2u; k++) {
				CHECK_EQ(fr_check_slot(f, k, &good[k], &r), FR_FLASH_OK);
				CHECK_EQ(good[k], ff_slot_good(&g_f, k));
				CHECK_EQ(s02_check_slot(&env, k, &lib), S02_OK);
				CHECK_EQ(good[k], lib);
				CHECK_EQ(r.tag, fr_le32(g_f.mem + FR_RECORD_OFF(k) + 8u));
			}
			CHECK_EQ(good[0], strcmp(v_states[s0], "good") == 0);
			CHECK_EQ(good[1], strcmp(v_states[s1], "good") == 0);
		}
	}
	/* a read failure is a failure, never a judgment */
	ff_init(&g_f);
	ff_set_slot(&g_f, 0, "good", 0x1345u, 1u);
	g_f.fail_regions = FF_R_SLOT0;
	{
		bool good = true;
		struct fr_selector s;

		CHECK_EQ(fr_check_slot(f, 0, &good, NULL), FR_FLASH_IO);
		CHECK(!good);
		g_f.fail_regions = FF_R_SELECTOR;
		CHECK_EQ(fr_read_selector(f, &s), FR_FLASH_IO);
		CHECK_EQ(fr_check_slot(f, 2, &good, NULL), FR_FLASH_IO);
	}
	/* the readers write nothing */
	CHECK_EQ(g_f.nmut, 0);
}

static unsigned g_served;

static void count_serve(void)
{
	g_served++;
}

/* fr_slot_info() from the flash: the record, the descriptor, the front. */
static void test_slot_info(void)
{
	const struct fr_flash *f = flash();
	struct fr_slot_info in;
	uint32_t n;

	ff_init(&g_f);
	n = build_front(g_img, &good_opt);
	memset(g_img + n, 0xFF, 0x3000u - n);
	n = 0x3000u;
	memcpy(g_f.mem + FR_SLOT_BASE(1), g_img, n);
	ff_put32(g_f.mem + FR_RECORD_OFF(1), ref_crc32(0, g_img, n));
	ff_put32(g_f.mem + FR_RECORD_OFF(1) + 4u, n);
	ff_put32(g_f.mem + FR_RECORD_OFF(1) + 8u, FR_TAG);
	ff_set_slot(&g_f, 0, "good", 0x1345u, 3u);	/* no stream: no descriptor */

	g_served = 0;
	fr_serve_hook = count_serve;	/* the link served between pages */
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	fr_serve_hook = NULL;
	CHECK(g_served >= n / FR_PAGE);
	CHECK(in.good);
	CHECK_EQ(in.rec.length, n);
	CHECK(in.has_desc);
	CHECK(in.has_front);
	CHECK_EQ(in.desc.bl_version[1], 2);
	CHECK_EQ(fr_slot_info(f, 0, &in), FR_FLASH_OK);
	CHECK(in.good);
	CHECK(!in.has_desc);
	CHECK(!in.has_front);

	/* a front byte changed, the record made to match: good, no front */
	g_f.mem[FR_SLOT_BASE(1) + 16u + 16u + 96u + 16u + 5u] ^= 0x01u;
	ff_put32(g_f.mem + FR_RECORD_OFF(1), ref_crc32(0, g_f.mem + FR_SLOT_BASE(1), n));
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(in.good);
	CHECK(in.has_desc);
	CHECK(!in.has_front);
	/* the record shorter than the stream: nothing past it is read */
	ff_put32(g_f.mem + FR_RECORD_OFF(1) + 4u, 0x100u);
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.good);
	CHECK(!in.has_desc);
	/* S-02.084: a record that is not plausible, erased or torn: bad, and
	 * the stream read within the slot */
	g_f.mem[FR_SLOT_BASE(1) + 16u + 16u + 96u + 16u + 5u] ^= 0x01u;
	memset(g_f.mem + FR_RECORD_OFF(1), 0xFF, 12);
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.good && !in.tried);
	CHECK(in.has_desc && in.has_front);
	CHECK_EQ(in.desc.bl_version[1], 2);
	/* the tag with a length of 2^31 or more: no stream read (SPEC-Q) */
	ff_put32(g_f.mem + FR_RECORD_OFF(1) + 8u, FR_TAG);	/* length 0xFFFFFFFF */
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.good && !in.has_desc && !in.has_front);
	ff_put32(g_f.mem + FR_RECORD_OFF(1) + 4u, 0x80000000u);
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.good && !in.has_desc && !in.has_front);
	ff_put32(g_f.mem + FR_RECORD_OFF(1) + 4u, 0x7FFFFFFFu);
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.good && in.has_front);
	ff_put32(g_f.mem + FR_RECORD_OFF(1) + 4u, 0u);		/* length 0 */
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.good && in.has_front);
	ff_put32(g_f.mem + FR_RECORD_OFF(1) + 4u, FR_SLOT_CAP + 1u);
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.good && in.has_front);
	ff_put32(g_f.mem + FR_RECORD_OFF(1) + 8u, FR_TAG | 0x00010000u);
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.good && in.has_front);
	/* and with a front byte changed: no front */
	g_f.mem[FR_SLOT_BASE(1) + 16u + 16u + 96u + 16u + 5u] ^= 0x01u;
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.good && in.has_desc && !in.has_front);
	/* an erased slot: no stream */
	memset(g_f.mem + FR_SLOT_BASE(1), 0xFF, n);
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.good && !in.has_desc && !in.has_front);
	CHECK_EQ(g_f.nmut, 0);
}

/* Slot 1 holding case c's bytes, its record good or tried; the rest of
 * the slot erased. */
static void put_kind_case(const struct v_skind *c, bool tried)
{
	uint8_t *d = g_f.mem + FR_SLOT_BASE(1);

	memset(d, 0xFF, FR_SLOT_CAP);
	for (uint32_t i = 0; i < c->length; i++)
		d[i] = (uint8_t)(i * 37u + c->seed);
	for (unsigned j = 0; j < c->npatch; j++)
		memcpy(d + c->patch[j].off, c->patch[j].b, 4);
	memset(g_f.mem + FR_RECORD_OFF(1), 0xFF, FR_SECTOR);
	ff_put32(g_f.mem + FR_RECORD_OFF(1), tried ? ~c->crc32 : c->crc32);
	ff_put32(g_f.mem + FR_RECORD_OFF(1) + 4u, c->length);
	ff_put32(g_f.mem + FR_RECORD_OFF(1) + 8u, FR_TAG);
}

/* The bytes slot 1's reads covered since the last ff_reset(), all within
 * [lo, hi): their total, or ~0u when one fell outside. */
static uint32_t read_bytes(uint32_t lo, uint32_t hi)
{
	uint32_t sum = 0;

	CHECK(g_f.nlog <= FF_LOG_MAX);
	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++) {
		const struct ff_op *o = &g_f.log[i];

		if (o->kind != FF_READ)
			continue;
		if (o->off < lo || o->off + o->len > hi)
			return ~0u;
		sum += o->len;
	}
	return sum;
}

/*
 * S-02.106 over slot_kinds.json: each case in slot 1, its record good and
 * tried, read by fr_slot_info() and then fr_slot_kind(). A case with a
 * valid descriptor has it set in the slot information (no fill holds one).
 * Rules 1 and 2 read nothing; rule 3 reads the slot's first 4,468 bytes,
 * the link served between its reads, and only when 1 and 2 fail; a slot
 * neither good nor tried has no kind and reads nothing.
 */
static void test_slot_kinds(void)
{
	static const uint8_t kinds[] = { FR_KIND_WB, FR_KIND_TE_BUILD, FR_KIND_TE, FR_KIND_UNKNOWN };
	const struct fr_flash *f = flash();
	struct fr_slot_info in;
	unsigned rule3 = 0;

	CHECK_EQ(N(v_slot_kinds), 15);
	for (unsigned i = 0; i < N(v_slot_kinds); i++) {
		const struct v_skind *c = &v_slot_kinds[i];
		const uint8_t want = kinds[c->kind];

		for (unsigned t = 0; t < 2u; t++) {
			bool by3;
			uint32_t read;

			ff_init(&g_f);
			put_kind_case(c, t != 0u);
			CHECK_EQ(ref_crc32(0, g_f.mem + FR_SLOT_BASE(1), c->length), c->crc32);
			CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
			CHECK(t ? in.tried && !in.good : in.good && !in.tried);
			CHECK(!in.has_desc);
			CHECK_EQ(in.kind, FR_KIND_NONE);	/* fr_slot_info() leaves it */
			if (c->desc_valid) {
				in.has_desc = true;
				memcpy(in.desc.bl_version, c->desc_version, sizeof in.desc.bl_version);
			}
			ff_reset(&g_f);
			g_served = 0;
			fr_serve_hook = count_serve;
			CHECK_EQ(fr_slot_kind(f, 1, &in), FR_FLASH_OK);
			fr_serve_hook = NULL;
			CHECK_EQ(in.kind, want);
			if (want == FR_KIND_TE_BUILD)
				CHECK(memcmp(in.te_version, c->version, sizeof in.te_version) == 0);
			if (want == FR_KIND_WB)
				CHECK_EQ(in.desc.bl_version[1], c->desc_version[1]);
			/* rule 3's read: only past rules 1 and 2, and only at a
			 * length of 4,468 or more */
			by3 = want != FR_KIND_WB && want != FR_KIND_TE_BUILD && c->length >= 4468u;
			read = read_bytes(FR_SLOT_BASE(1), FR_SLOT_BASE(1) + 4468u);
			CHECK_EQ(read, by3 ? 4468u : 0u);
			CHECK(!by3 || g_served >= 4468u / FR_PAGE);
			rule3 += by3 ? 1u : 0u;
			CHECK_EQ(g_f.nmut, 0);
		}
	}
	CHECK_EQ(rule3, 14);	/* 7 cases, good and tried */
	/* neither good nor tried: no kind, nothing read */
	ff_init(&g_f);
	put_kind_case(&v_slot_kinds[0], false);
	g_f.mem[FR_RECORD_OFF(1)] ^= 0x01u;
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.good && !in.tried);
	ff_reset(&g_f);
	CHECK_EQ(fr_slot_kind(f, 1, &in), FR_FLASH_OK);
	CHECK_EQ(in.kind, FR_KIND_NONE);
	CHECK_EQ(g_f.nlog, 0);
	/* rule 3's read failing: an error, the kind unknown */
	for (unsigned i = 0; i < N(v_slot_kinds); i++)
		if (strcmp(v_slot_kinds[i].name, "te_prefix_only") == 0) {
			ff_init(&g_f);
			put_kind_case(&v_slot_kinds[i], false);
			CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
			ff_reset(&g_f);
			g_f.fail_read = 2;
			CHECK_EQ(fr_slot_kind(f, 1, &in), FR_FLASH_IO);
			CHECK(in.kind != FR_KIND_TE);
		}
	/* a real waffle-boot image: rule 1, from its own descriptor */
	ff_init(&g_f);
	{
		uint32_t n = build_front(g_img, &good_opt);

		memcpy(g_f.mem + FR_SLOT_BASE(1), g_img, n);
		ff_put32(g_f.mem + FR_RECORD_OFF(1), ~ref_crc32(0, g_img, n));
		ff_put32(g_f.mem + FR_RECORD_OFF(1) + 4u, n);
		ff_put32(g_f.mem + FR_RECORD_OFF(1) + 8u, FR_TAG);
		CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
		CHECK(in.tried && in.has_desc);
		ff_reset(&g_f);
		CHECK_EQ(fr_slot_kind(f, 1, &in), FR_FLASH_OK);
		CHECK_EQ(in.kind, FR_KIND_WB);
		CHECK_EQ(g_f.nlog, 0);
	}
	CHECK_EQ(fr_slot_kind(f, 2, &in), FR_FLASH_IO);
}

/* The 1-byte reads of slot k's log byte 0 in the op log. */
static unsigned log0_reads(unsigned k)
{
	unsigned n = 0;

	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++)
		if (g_f.log[i].kind == FF_READ && g_f.log[i].off == FR_LOG_OFF(k) &&
		    g_f.log[i].len == 1u)
			n++;
	return n;
}

/*
 * The trial mark (fr_tried()): the tag, a length from 1 to the slot's
 * capacity, crc32 the complement of the CRC. fr_slot_info()'s log: one more
 * 1-byte read, of byte 0 of the slot's last sector, for a record with the
 * tag and a length of 1 to 0x77000 (has_log, log0), none otherwise; a tried
 * slot's start and spend read from it, and only with a log.
 */
static void test_tried_and_log(void)
{
	static const struct {
		uint32_t len;
		bool tried;
	} lens[] = {
		{ 0u, false }, { 1u, true }, { 0x77000u, true }, { 0x77001u, true },
		{ FR_SLOT_CAP, true }, { FR_SLOT_CAP + 1u, false }, { 0x80000000u, false },
	};
	static const struct {
		uint8_t b;
		bool started, spent;
	} logs[] = {
		{ 0xFFu, false, false }, { 0xFDu, false, false }, { 0xFCu, true, false },
		{ 0xF9u, false, true }, { 0xFEu, false, false }, { 0x00u, true, false },
		{ 0xEDu, false, true },
	};
	const struct fr_flash *f = flash();
	struct fr_slot_info in;
	struct fr_record r;

	for (unsigned i = 0; i < N(lens); i++) {
		r.tag = FR_TAG;
		r.length = lens[i].len;
		r.crc32 = ~0x12345678u;
		CHECK(fr_tried(&r, 0x12345678u) == lens[i].tried);
		r.crc32 = 0x12345678u;
		CHECK(!fr_tried(&r, 0x12345678u));
		r.crc32 = ~0x12345678u;
		r.tag = 0x0000B005u;
		CHECK(!fr_tried(&r, 0x12345678u));
	}
	for (unsigned k = 0; k < 2u; k++)
		for (unsigned len = 0; len < 3u; len++)
			for (unsigned t = 0; t < 2u; t++)
				for (unsigned l = 0; l < N(logs); l++) {
					/* 0x2000, 0x77000 (a log), 0x77010 (none) */
					const uint32_t n = len == 0u ? 0x2000u : len == 1u ? 0x77000u : 0x77010u;
					const bool has_log = n <= 0x77000u;
					uint32_t crc;

					ff_init(&g_f);
					ff_fill_image(g_f.mem + FR_SLOT_BASE(k), n, 9u + k);
					crc = ref_crc32(0, g_f.mem + FR_SLOT_BASE(k), n);
					ff_put32(g_f.mem + FR_RECORD_OFF(k), t ? ~crc : crc);
					ff_put32(g_f.mem + FR_RECORD_OFF(k) + 4u, n);
					ff_put32(g_f.mem + FR_RECORD_OFF(k) + 8u, FR_TAG);
					if (has_log)
						g_f.mem[FR_LOG_OFF(k)] = logs[l].b;
					ff_reset(&g_f);
					CHECK_EQ(fr_slot_info(f, k, &in), FR_FLASH_OK);
					CHECK(in.good == !t && in.tried == (t != 0u));
					CHECK(in.has_log == has_log);
					CHECK_EQ(log0_reads(k), (has_log ? 1u : 0u));
					CHECK_EQ(in.log0, (has_log ? logs[l].b : 0u));
					CHECK(in.started == (t && has_log && logs[l].started));
					CHECK(in.spent == (t && has_log && logs[l].spent));
					CHECK_EQ(g_f.nmut, 0);
					/* the read fails: the call does */
					if (has_log && l == 0u) {
						ff_reset(&g_f);
						g_f.fail_regions = k == 0u ? FF_R_SLOT0 : FF_R_SLOT1;
						CHECK_EQ(fr_slot_info(f, k, &in), FR_FLASH_IO);
					}
				}
	/* no log for a record without the tag, or with length 0 */
	ff_init(&g_f);
	ff_set_slot(&g_f, 1, "badtag", 0x2000u, 3u);
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.has_log && log0_reads(1) == 0u);
	ff_init(&g_f);
	ff_set_slot(&g_f, 1, "empty", 0x2000u, 3u);
	CHECK_EQ(fr_slot_info(f, 1, &in), FR_FLASH_OK);
	CHECK(!in.has_log && log0_reads(1) == 0u);
}

/* --- The running slot against s02_running_slot() (S-01.002) --- */

/* The memory lies in main's DDR; the map refuses what fr_map_rule.h says
 * the front never reads, and the streams have blocks there too. */
#define MEMB 0x88000000u
#define MEML 0x40000u
static uint8_t g_mem[MEML];
static uint32_t g_map_lo, g_map_hi;	/* the readable part */

static bool readable(uint32_t addr, uint32_t len)
{
	return addr >= g_map_lo && (uint64_t)addr + len <= g_map_hi &&
	       !fr_map_forbidden(addr, len);
}

/* The loaded descriptor's half, for fr_running_slots_checked(). */
static uint8_t g_desc[FR_DESC_MAX];

static bool in_desc(uint32_t addr, uint64_t len)
{
	return addr >= FR_DESC_ADDR && addr + len <= (uint64_t)FR_DESC_ADDR + FR_DESC_MAX;
}

static const uint8_t *mem_map(void *ctx, uint32_t addr, uint32_t len)
{
	(void)ctx;
	if (in_desc(addr, len))
		return g_desc + (addr - FR_DESC_ADDR);
	return readable(addr, len) ? g_mem + (addr - MEMB) : NULL;
}

static bool mem_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (!readable(addr, len))
		return false;
	memcpy(buf, g_mem + (addr - MEMB), len);
	return true;
}

static uint32_t rnd(uint32_t *x)
{
	*x ^= *x << 13;
	*x ^= *x >> 17;
	*x ^= *x << 5;
	return *x;
}

/* Where the front never reads (fr_map_rule.h): L1 code, the front's L2,
 * the hand-off record, the staging buffer. */
static const uint32_t never_targets[] = {0x11A00000u, 0x08002000u, 0x08030800u, 0x88400000u};

/* A stream of a few blocks into MEMB.., one 0x6000-byte region each, so
 * that no two load the same byte; with FILL, IGNORE and refused ones;
 * shared makes its first block the same in every stream. */
static uint32_t random_stream(uint8_t *s, uint32_t *x, bool shared)
{
	uint8_t pay[3000];
	uint32_t off = 0, nb = 2u + rnd(x) % 6u;

	for (uint32_t b = 0; b < nb; b++) {
		uint32_t kind = rnd(x) % 8u, count = 1u + rnd(x) % sizeof pay;
		uint32_t target = MEMB + 0x2000u + 0x6000u * b + rnd(x) % 0x2000u;

		if (b == 0 && shared) {
			ff_fill_image(pay, 1000, 99);
			off = put_block(s, off, 0, MEMB + 0x3000u, 1000, pay);
			continue;
		}
		ff_fill_image(pay, count, rnd(x));
		if (kind == 0u)
			off = put_block(s, off, S05_FLAG_FILL, target, count, NULL);
		else if (kind == 1u)
			off = put_block(s, off, S05_FLAG_IGNORE, target, count, pay);
		else if (kind == 2u)
			off = put_block(s, off, 0, never_targets[rnd(x) % N(never_targets)], count, pay);
		else
			off = put_block(s, off, 0, target, count, pay);
	}
	return put_block(s, off, S05_FLAG_FINAL, MEMB + 0x2000u, 0, NULL);
}

static void load(const uint8_t *s, uint32_t len)
{
	struct s05_block b;
	uint32_t off = 0;

	while (s05_ldr_next(s, len, &off, &b) == 0) {
		if (!(b.flags & S05_FLAG_FILL) && s05_block_has_destination(&b) &&
		    b.target >= MEMB && (uint64_t)b.target + b.count <= (uint64_t)MEMB + MEML)
			memcpy(g_mem + (b.target - MEMB), s + b.offset + 16u, b.count);
		else if (!(b.flags & S05_FLAG_FILL) && in_desc(b.target, b.count))
			memcpy(g_desc + (b.target - FR_DESC_ADDR), s + b.offset + 16u, b.count);
		if (b.flags & S05_FLAG_FINAL)
			break;
	}
}

static void put_slot(unsigned k, const uint8_t *s, uint32_t len, unsigned rec)
{
	memset(g_f.mem + FR_SLOT_BASE(k), 0xFF, FR_SLOT_CAP);
	memcpy(g_f.mem + FR_SLOT_BASE(k), s, len);
	memset(g_f.mem + FR_RECORD_OFF(k), 0xFF, FR_SECTOR);
	if (rec == 0u)
		return;			/* erased record: the whole capacity is walked */
	ff_put32(g_f.mem + FR_RECORD_OFF(k), ref_crc32(0, s, len) ^ (rec == 2u ? 1u : 0u));
	ff_put32(g_f.mem + FR_RECORD_OFF(k) + 4u, rec == 3u ? len / 2u : len);
	ff_put32(g_f.mem + FR_RECORD_OFF(k) + 8u, FR_TAG);
}

/* Slot k's stream against memory, by libwaffle's parser: the bytes
 * compared (256 at a time from each block's start, where readable) and
 * the bytes equal. */
static void ref_counts(unsigned k, uint32_t *cmp, uint32_t *eq, bool *complete)
{
	const uint8_t *s = g_f.mem + FR_SLOT_BASE(k), *rec = g_f.mem + FR_RECORD_OFF(k);
	uint32_t len = FR_SLOT_CAP, off = 0;
	struct s05_block b;

	*cmp = *eq = 0;
	*complete = false;
	if (fr_le32(rec + 8u) == FR_TAG && fr_le32(rec + 4u) >= 1u && fr_le32(rec + 4u) <= FR_SLOT_CAP)
		len = fr_le32(rec + 4u);
	while (s05_ldr_next(s, len, &off, &b) == 0) {
		if (!(b.flags & S05_FLAG_FILL) && s05_block_has_destination(&b) &&
		    (uint64_t)b.target + b.count <= 0x100000000ull) {
			for (uint32_t j = 0; j < b.count; j += 256u) {
				uint32_t n = b.count - j < 256u ? b.count - j : 256u;

				if (!readable(b.target + j, n))
					continue;
				*cmp += n;
				for (uint32_t i = 0; i < n; i++)
					*eq += s[b.offset + 16u + j + i] ==
					       g_mem[b.target + j + i - MEMB] ? 1u : 0u;
			}
		}
		if (b.flags & S05_FLAG_FINAL) {
			*complete = true;
			break;
		}
	}
}

/*
 * The boot's inference (s02_running_image(): the first slot's stream read
 * once into memory, the other ruled out by a probe or compared whole,
 * either left at its first difference) against the rule itself, `want`
 * being the slot it names, the settled one where both hold the same image,
 * or 2: the same answer whichever slot is read first, with a probe and
 * without. It reads less than the two above, so a
 * read fault they meet it may not: when it answers, the answer is right.
 */
static void agree_image(const struct s02_env *env, const struct s02_mem_ops *mops, unsigned want,
			bool faults)
{
	static uint8_t buf[FR_SLOT_CAP];
	static const struct s02_probe probe = {MEMB + 0x100u, 8u};

	for (unsigned v = 0; v < 4u; v++) {
		const struct s02_probe *p = (v & 2u) ? &probe : NULL;
		struct s02_image im;
		unsigned first, got = 7;
		int rc;

		s02_image_init(&im, buf, sizeof buf, NULL, NULL);
		first = s02_running_first(env, mops, p, v & 1u, &im);
		(void)s02_image_read(env, first, &im);
		(void)s02_image_check(env, &im);
		rc = s02_running_image(env, mops, p, &im, first, &got);
		if (!faults)
			CHECK_EQ(rc, S02_OK);
		if (rc == S02_OK)
			CHECK_EQ(got, want);
		else
			CHECK_EQ(got, 2u);
	}
}

/* The slot later starts settle on, by the flash's bytes (S-02.020 to
 * S-02.023): the selected slot when verified is 0x01 or it is good, else
 * the other. */
static unsigned ref_settled(void)
{
	const unsigned s = g_f.mem[FR_SELECTOR_OFF] == 0x00u ? 0u : 1u;

	if (g_f.mem[FR_SELECTOR_OFF + 1u] == 0x01u || ff_slot_good(&g_f, s))
		return s;
	return 1u - s;
}

/* libwaffle with a 512-byte scratch compares 256 bytes at a time, as the
 * front does, so the two count the same bytes and name the same slot: the
 * one whose stream matched every byte compared, the settled slot where
 * both did to their FINAL block, or none. */
static void agree(unsigned *est_out, unsigned *slot_out)
{
	static uint8_t scratch[512];
	static const struct s02_mem_ops mops = {NULL, mem_read};
	static const struct fr_mem fmem = {mem_map, NULL};
	struct s02_flash_ops ops;
	struct s02_env env = ff_env(&g_f, &ops);
	unsigned lib = 7, slot = 5;
	bool est = true, both = true;
	int lrc, frc;

	env.scratch = scratch;
	env.scratch_len = sizeof scratch;
	lrc = s02_running_slot(&env, &mops, 7, &lib);
	frc = fr_running_slots(flash(), &fmem, &slot, &est, &both);
	CHECK_EQ(lrc == S02_OK, frc == FR_FLASH_OK);
	{
		uint32_t cmp[2], eq[2];
		bool full[2], whole[2];
		unsigned plain = 9;
		bool pest = false;

		/* the form without the flag gives the same answer */
		CHECK_EQ(fr_running_slot(flash(), &fmem, &plain, &pest), frc);
		CHECK(plain == slot && pest == est);

		for (unsigned k = 0; k < 2u; k++) {
			ref_counts(k, &cmp[k], &eq[k], &whole[k]);
			full[k] = cmp[k] != 0u && eq[k] == cmp[k];
		}
		if (full[0] && full[1] && whole[0] != whole[1])
			full[whole[0] ? 1u : 0u] = false;
		agree_image(&env, &mops,
			    full[0] && full[1] && whole[0] && whole[1] ? ref_settled() :
			    full[0] == full[1] ? 2u : full[0] ? 0u : 1u,
			    frc != FR_FLASH_OK);
		if (frc != FR_FLASH_OK) {
			CHECK(!est);
			CHECK_EQ(slot, FR_NO_SLOT);
			CHECK(!both);
		} else if (full[0] && full[1] && whole[0] && whole[1]) {
			CHECK(est && both);
			CHECK_EQ(slot, ref_settled());
			CHECK_EQ(lib, slot);
		} else if (full[0] == full[1]) {
			CHECK(!both);
			CHECK_EQ(lib, 7u);
			CHECK(!est);
			CHECK_EQ(slot, FR_NO_SLOT);
		} else {
			CHECK_EQ(lib, full[0] ? 0u : 1u);
			CHECK(est && !both);
			CHECK_EQ(slot, lib);
		}
	}
	*est_out = est ? 1u : 0u;
	*slot_out = slot;
}

static void test_running_slot(void)
{
	static uint8_t a[0x8000], b[0x8000];
	uint32_t x = 777u, alen, blen;
	unsigned est, slot, t, counts[3] = {0, 0, 0};

	/* the plain cases */
	ff_init(&g_f);
	alen = random_stream(a, &x, true);
	blen = random_stream(b, &x, true);
	put_slot(0, a, alen, 1);
	put_slot(1, b, blen, 1);
	g_map_lo = MEMB + 0x2000u;
	g_map_hi = MEMB + MEML;
	memset(g_mem, 0x5A, sizeof g_mem);
	load(a, alen);
	agree(&est, &slot);
	CHECK(est && slot == 0u);
	memset(g_mem, 0x5A, sizeof g_mem);
	load(b, blen);
	agree(&est, &slot);
	CHECK(est && slot == 1u);
	/* nothing readable: a tie, not established */
	g_map_hi = g_map_lo;
	agree(&est, &slot);
	CHECK(!est);
	/* byte-identical streams: either slot is this image, and the slot
	 * is the one later starts settle on. Every selector kind against
	 * every pair of records: good, a CRC that fails, the trial mark,
	 * erased, and one that ends before the FINAL block. */
	g_map_hi = MEMB + MEML;
	for (unsigned c = 0; c < 4u * 4u * 5u * 5u; c++) {
		static const uint8_t index[4] = { 0x00, 0x01, 0x02, 0xFF };
		static const uint8_t verified[4] = { 0x01, 0x00, 0xFE, 0xFF };
		const unsigned r0 = c / 16u % 5u, r1 = c / 80u;
		const unsigned sel = index[c & 3u] == 0x00u ? 0u : 1u;
		const uint8_t ver = verified[c >> 2 & 3u];
		unsigned want;

		ff_init(&g_f);
		for (unsigned k = 0; k < 2u; k++) {
			const unsigned r = k == 0u ? r0 : r1;

			/* put_slot()'s records: 0 erased, 1 good, 2 a bad CRC,
			 * 3 half the length; 4 the trial mark here */
			put_slot(k, a, alen, r == 4u ? 1u : r);
			if (r == 4u)
				ff_put32(g_f.mem + FR_RECORD_OFF(k), ~ref_crc32(0, a, alen));
		}
		ff_set_selector(&g_f, index[c & 3u], ver);
		memset(g_mem, 0x5A, sizeof g_mem);
		load(a, alen);
		agree(&est, &slot);
		if (r0 == 3u || r1 == 3u) {
			/* one stream cut short by its record: the whole one, as
			 * before; both cut short: none */
			CHECK(est == (r0 != r1));
			if (est)
				CHECK_EQ(slot, r0 == 3u ? 1u : 0u);
			continue;
		}
		want = ver == 0x01u || (sel == 0u ? r0 : r1) == 1u ? sel : 1u - sel;
		CHECK(est);
		CHECK_EQ(slot, want);
		CHECK_EQ(g_f.nmut, 0);
	}
	/* the selector unreadable, or the selected slot when its check is
	 * needed: not established */
	ff_init(&g_f);
	put_slot(0, a, alen, 1);
	put_slot(1, a, alen, 1);
	ff_set_selector(&g_f, 0x00, 0x00);
	memset(g_mem, 0x5A, sizeof g_mem);
	load(a, alen);
	agree(&est, &slot);
	CHECK(est && slot == 0u);
	g_f.fail_regions = FF_R_SELECTOR;
	agree(&est, &slot);
	CHECK(!est);
	g_f.fail_regions = 0;
	{
		static const struct fr_mem fmem = {mem_map, NULL};
		unsigned reads, got = 0;
		bool e = true;

		ff_reset(&g_f);
		CHECK_EQ(fr_running_slot(flash(), &fmem, &got, &e), FR_FLASH_OK);
		reads = g_f.nreads;
		ff_reset(&g_f);
		g_f.fail_read = reads;		/* the check's last read */
		CHECK_EQ(fr_running_slot(flash(), &fmem, &got, &e), FR_FLASH_IO);
		CHECK(!e && got == FR_NO_SLOT);
		ff_reset(&g_f);
	}
	/* a read failure: not established */
	put_slot(1, b, blen, 1);
	g_f.fail_regions = FF_R_SLOT1;
	agree(&est, &slot);
	CHECK(!est);
	g_f.fail_regions = FF_R_RECORD0;
	agree(&est, &slot);
	CHECK(!est);
	g_f.fail_regions = 0;

	/* random slots, records, loads and readable windows */
	for (t = 0; t < 400u; t++) {
		unsigned which = rnd(&x) % 3u;

		ff_init(&g_f);
		alen = random_stream(a, &x, (rnd(&x) & 1u) != 0u);
		blen = random_stream(b, &x, (rnd(&x) & 1u) != 0u);
		put_slot(0, a, alen, rnd(&x) % 4u);
		put_slot(1, b, blen, rnd(&x) % 4u);
		if (rnd(&x) % 8u == 0u)	/* a stream broken part way */
			g_f.mem[FR_SLOT_BASE(1) + rnd(&x) % blen] ^= 0x10u;
		memset(g_mem, 0x5A, sizeof g_mem);
		if (which != 2u)
			load(which == 0u ? a : b, which == 0u ? alen : blen);
		for (unsigned w = rnd(&x) % 4u; w > 0; w--)	/* the program wrote */
			if (rnd(&x) % 2u == 0u)
				g_mem[rnd(&x) % MEML] ^= 0xFFu;
		g_map_lo = MEMB + rnd(&x) % 0x8000u;
		g_map_hi = g_map_lo + rnd(&x) % (MEML - (g_map_lo - MEMB) + 1u);
		agree(&est, &slot);
		counts[est && slot < 2u ? slot : 2u]++;
		CHECK_EQ(g_f.nmut, 0);
	}
	CHECK(counts[0] > 0u && counts[1] > 0u && counts[2] > 0u);
}

/* S-01.002: a shorter stream that matches whole is established over a
 * longer one that matches in more bytes, but not in all. */
static void test_running_whole(void)
{
	static uint8_t a[0x2000], b[0x4000], pay[0x1000];
	uint32_t alen, blen, off;
	unsigned est, slot;

	ff_init(&g_f);
	g_map_lo = MEMB;
	g_map_hi = MEMB + MEML;
	ff_fill_image(pay, sizeof pay, 52);
	alen = put_block(a, 0, 0, MEMB + 0x1000u, 0x800u, pay);
	alen = put_block(a, alen, S05_FLAG_FINAL, MEMB + 0x1000u, 0, NULL);
	off = put_block(b, 0, 0, MEMB + 0x1000u, 0x800u, pay);
	memset(pay, 0x5A, sizeof pay);
	pay[0x7FFu] = 0x00u;
	off = put_block(b, off, 0, MEMB + 0x8000u, sizeof pay, pay);
	blen = put_block(b, off, S05_FLAG_FINAL, MEMB + 0x1000u, 0, NULL);
	put_slot(0, a, alen, 1);
	put_slot(1, b, blen, 1);
	memset(g_mem, 0x5A, sizeof g_mem);
	load(a, alen);
	agree(&est, &slot);
	CHECK(est && slot == 0u);
	/* and in the other order */
	put_slot(0, b, blen, 1);
	put_slot(1, a, alen, 1);
	agree(&est, &slot);
	CHECK(est && slot == 1u);

	/* the other slot a prefix of the running stream, cut short before its
	 * FINAL block (a write cut early): it matches every byte it has, yet
	 * the running slot is established */
	ff_fill_image(pay, sizeof pay, 52);
	off = put_block(b, 0, 0, MEMB + 0x1000u, 0x400u, pay);
	off = put_block(b, off, 0, MEMB + 0x1400u, 0x400u, pay + 0x400u);
	off = put_block(b, off, S05_FLAG_FINAL, MEMB + 0x1000u, 0, NULL);
	memset(g_mem, 0x5A, sizeof g_mem);
	load(b, off);
	put_slot(0, b, off, 0);
	put_slot(1, b, 16u + 0x400u, 0);	/* its first block alone, no FINAL */
	agree(&est, &slot);
	CHECK(est && slot == 0u);
	put_slot(1, b, off, 1);
	put_slot(0, b, 16u + 0x400u, 1);
	agree(&est, &slot);
	CHECK(est && slot == 1u);
}

/* S-01.002 with the front's map: a winner is established only on a match
 * of every byte compared. */
static void test_running_strict(void)
{
	static uint8_t a[0x2000], b[0x2000], c[0x2000], pay[0x1000];
	static const struct fr_mem fmem = {mem_map, NULL};
	uint32_t alen, blen, clen, off;
	unsigned slot;
	bool est;

	ff_init(&g_f);
	g_map_lo = MEMB;
	g_map_hi = MEMB + MEML;
	/* slot 0: blocks the map refuses, then one in main's DDR; slot 1 the
	 * same with other DDR bytes */
	ff_fill_image(pay, sizeof pay, 41);
	off = 0;
	for (unsigned i = 0; i < N(never_targets); i++)
		off = put_block(a, off, 0, never_targets[i], 0x200u, pay + 0x100u * i);
	memcpy(b, a, off);
	off = put_block(a, off, 0, MEMB + 0x1000u, 0x800u, pay);
	alen = put_block(a, off, S05_FLAG_FINAL, MEMB + 0x1000u, 0, NULL);
	ff_fill_image(pay, sizeof pay, 42);
	off = put_block(b, off, 0, MEMB + 0x1000u, 0x800u, pay);
	blen = put_block(b, off, S05_FLAG_FINAL, MEMB + 0x1000u, 0, NULL);
	put_slot(0, a, alen, 1);
	put_slot(1, b, blen, 1);

	/* loaded from slot 0: established, only the DDR block compared */
	memset(g_mem, 0x5A, sizeof g_mem);
	load(a, alen);
	CHECK_EQ(fr_running_slot(flash(), &fmem, &slot, &est), FR_FLASH_OK);
	CHECK(est && slot == 0u);
	/* one byte of it written since the load: slot 0 still scores higher,
	 * but it is not a match */
	g_mem[0x1000u + 0x7FFu] ^= 0x01u;
	CHECK_EQ(fr_running_slot(flash(), &fmem, &slot, &est), FR_FLASH_OK);
	CHECK(!est && slot == FR_NO_SLOT);
	/* a third image, close to slot 0, loaded from neither */
	memcpy(c, a, alen);
	clen = alen;
	c[alen - 16u - 0x400u] ^= 0x20u;	/* inside the DDR block's payload */
	memset(g_mem, 0x5A, sizeof g_mem);
	load(c, clen);
	CHECK_EQ(fr_running_slot(flash(), &fmem, &slot, &est), FR_FLASH_OK);
	CHECK(!est && slot == FR_NO_SLOT);
	/* the DDR block unreadable: only refused memory is left, nothing is
	 * compared, a tie */
	load(a, alen);
	g_map_hi = MEMB + 0x1000u;
	CHECK_EQ(fr_running_slot(flash(), &fmem, &slot, &est), FR_FLASH_OK);
	CHECK(!est && slot == FR_NO_SLOT);
	/* half of it readable, every compared byte equal: established */
	g_map_hi = MEMB + 0x1400u;
	CHECK_EQ(fr_running_slot(flash(), &fmem, &slot, &est), FR_FLASH_OK);
	CHECK(est && slot == 0u);
	/* a stream whose two blocks load the same bytes: the earlier one's
	 * bytes are overwritten by the load itself, so it never matches */
	ff_fill_image(pay, sizeof pay, 43);
	off = put_block(c, 0, 0, MEMB + 0x1000u, 0x100u, pay);
	off = put_block(c, off, 0, MEMB + 0x1000u, 0x100u, pay + 0x100u);
	clen = put_block(c, off, S05_FLAG_FINAL, MEMB + 0x1000u, 0, NULL);
	put_slot(0, c, clen, 1);
	g_map_hi = MEMB + MEML;
	memset(g_mem, 0x5A, sizeof g_mem);
	load(c, clen);
	CHECK_EQ(fr_running_slot(flash(), &fmem, &slot, &est), FR_FLASH_OK);
	CHECK(!est && slot == FR_NO_SLOT);
	CHECK_EQ(g_f.nmut, 0);
}

/* A descriptor of n bytes: magic, version, size, the rest from seed. */
static void make_desc(uint8_t *d, uint32_t n, unsigned seed)
{
	ff_fill_image(d, n, seed);
	ff_put32(d + FR_DESC_O_MAGIC, FR_DESC_MAGIC);
	d[FR_DESC_O_VERSION] = (uint8_t)FR_DESC_VERSION;
	d[FR_DESC_O_VERSION + 1u] = 0;
	d[FR_DESC_O_SIZE] = (uint8_t)n;
	d[FR_DESC_O_SIZE + 1u] = (uint8_t)(n >> 8);
}

/* A stream: the descriptor's block (flags dflags, n bytes of d, or none),
 * then one DDR block from seed. */
static uint32_t desc_stream(uint8_t *s, const uint8_t *d, uint32_t n, uint32_t dflags, unsigned seed)
{
	static uint8_t pay[0x3000];
	uint32_t off = 0;

	if (d != NULL)
		off = put_block(s, off, dflags, FR_DESC_ADDR, n, d);
	ff_fill_image(pay, sizeof pay, seed);
	off = put_block(s, off, 0, MEMB + 0x1000u, sizeof pay, pay);
	return put_block(s, off, S05_FLAG_FINAL, MEMB + 0x1000u, 0, NULL);
}

/* The checked form against the whole compare: the same slot, flag and
 * both; fast says it read no stream payload (a few reads, not thousands). */
static void same_as_whole(bool fast, unsigned want, bool want_est)
{
	static const struct fr_mem fmem = {mem_map, NULL};
	unsigned s1 = 5, s2 = 6, reads;
	bool e1 = false, e2 = true, b1 = false, b2 = true;

	ff_reset(&g_f);
	CHECK_EQ(fr_running_slots_checked(flash(), &fmem, &s1, &e1, &b1), FR_FLASH_OK);
	reads = g_f.nreads;
	CHECK_EQ(fr_running_slots(flash(), &fmem, &s2, &e2, &b2), FR_FLASH_OK);
	CHECK(s1 == s2 && e1 == e2 && b1 == b2);
	CHECK(e1 == want_est);
	if (want_est)
		CHECK_EQ(s1, want);
	if (fast)
		CHECK(reads <= 16u);
	else
		CHECK(reads > 16u);
}

static void test_running_checked(void)
{
	static uint8_t a[0x4000], b[0x4000], c[0x4000];
	uint8_t da[0x5C], db[0x5C];
	uint32_t alen, blen, clen;

	ff_init(&g_f);
	g_map_lo = MEMB;
	g_map_hi = MEMB + MEML;
	make_desc(da, sizeof da, 61);
	memcpy(db, da, sizeof db);
	db[FR_DESC_O_MAIN_CRC] ^= 0x5Au;	/* another build: its main CRC */
	alen = desc_stream(a, da, sizeof da, 0, 62);
	blen = desc_stream(b, db, sizeof db, 0, 63);
	put_slot(0, a, alen, 1);
	put_slot(1, b, blen, 1);
	g_f.mem[FR_SELECTOR_OFF] = 0x00u;
	g_f.mem[FR_SELECTOR_OFF + 1u] = 0x01u;

	/* two builds: the loaded one's descriptor names its slot, no stream read */
	memset(g_mem, 0x5A, sizeof g_mem);
	memset(g_desc, 0, sizeof g_desc);
	load(a, alen);
	same_as_whole(true, 0u, true);
	memset(g_mem, 0x5A, sizeof g_mem);
	load(b, blen);
	same_as_whole(true, 1u, true);

	/* the same file in both slots: the whole compare and its tie-break */
	put_slot(1, a, alen, 1);
	load(a, alen);
	same_as_whole(false, 0u, true);
	g_f.mem[FR_SELECTOR_OFF] = 0x01u;
	same_as_whole(false, 1u, true);
	g_f.mem[FR_SELECTOR_OFF] = 0x00u;

	/* the same descriptor in a copy cut after it: the whole compare */
	memcpy(c, a, alen);
	clen = 16u + sizeof da + 16u + 0x1000u;
	put_slot(1, c, clen, 0);
	same_as_whole(false, 0u, true);

	/* a whole stream with no descriptor (another bootloader): it differs,
	 * no stream payload read */
	clen = desc_stream(c, NULL, 0, 0, 63);
	put_slot(1, c, clen, 1);
	same_as_whole(true, 0u, true);
	/* the same cut before its FINAL block, or a descriptor by FILL: not
	 * told, the whole compare */
	put_slot(1, c, clen - 16u, 1);
	same_as_whole(false, 0u, true);
	clen = desc_stream(c, da, sizeof da, S05_FLAG_FILL, 63);
	put_slot(1, c, clen, 1);
	same_as_whole(false, 0u, true);

	/* no descriptor in memory: the whole compare, which the streams'
	 * descriptor blocks then fail */
	put_slot(1, b, blen, 1);
	memset(g_desc, 0, sizeof g_desc);
	same_as_whole(false, 0u, false);
	CHECK_EQ(g_f.nmut, 0);
}

int main(void)
{
	test_bootable_vectors();
	test_rules_agree_random();
	test_front_condition();
	test_selector_and_records();
	test_slot_info();
	test_slot_kinds();
	test_tried_and_log();
	test_running_slot();
	test_running_whole();
	test_running_strict();
	test_running_checked();
	return finish("test_front_slots");
}
