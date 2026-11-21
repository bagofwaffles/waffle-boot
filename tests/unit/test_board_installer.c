/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The installer's pure parts (src/testos/installer_logic.c, wiki/Installer-Reference.md):
 * its screens against tests/vectors/s15/ui/installer.json, the lock, the
 * hold, the carried image's check, the USB protocol byte for byte, and the
 * install part of check.txt.
 */
#define OPXY_TESTOS_CHECK 1
#define OPXY_TESTOS_INSTALLER 1
#include "../../src/testos/check_logic.c"
#include "../../src/testos/installer_logic.c"

#include <stdio.h>
#include <string.h>

#include "waffle/device.h"
#include "waffle/s02_flash.h"
#include "waffle/wire_crc.h"
#include "s15_sha256.h"
#include "support/check.h"

#define S15V_INSTALLER
#include "vectors/s15_vectors.h"

static uint32_t crc_of(uint32_t crc, const uint8_t *p, size_t n)
{
	return wire_crc32(crc, p, (uint32_t)n);
}

/* --- the screens --- */

static void test_screens(void)
{
	for (size_t k = 0; k < S15V_N(s15v_insts); k++) {
		const struct s15v_inst *v = &s15v_insts[k];
		struct inst_survey sv;
		struct inst_screen s;
		struct tchk_line l[S15_ROWS];
		unsigned n;

		memset(&s, 0, sizeof s);
		s.phase = (uint8_t)v->phase;
		s.version = v->version;
		s.target = (uint8_t)v->target;
		s.lock = (uint8_t)v->lock;
		s.last_rc = (uint8_t)v->last_rc;
		s.os = (uint8_t)v->os;
		s.trial = (uint8_t)v->trial;
		s.hold_digit = v->hold_digit;
		s.hold_fill = v->hold_fill;
		s.hold_left_s = v->hold_left_s;
		s.keys_seen = (uint8_t)v->keys_seen;
		s.device = (uint8_t)v->device;
		s.last_key = (uint8_t)v->last_key;
		s.fs_n = (uint32_t)v->fs_n;
		s.fs_c = (uint32_t)v->fs_c;
		if (v->survey != NULL) {
			const struct s15v_inst_survey *x = v->survey;

			memset(&sv, 0, sizeof sv);
			sv.device = (uint8_t)x->device;
			sv.how = (uint8_t)x->how;
			sv.fs_n = (uint32_t)x->fs_n;
			sv.fs_c = (uint32_t)x->fs_c;
			sv.sel_read = x->sel_read != 0;
			sv.sel_index = (uint8_t)x->sel_index;
			sv.sel_verified = (uint8_t)x->sel_verified;
			sv.slot[0].bits = (uint8_t)x->slot_bits[0];
			sv.slot[1].bits = (uint8_t)x->slot_bits[1];
			sv.ext_read = x->ext_read != 0;
			sv.part_config = (uint8_t)x->part_config;
			sv.boot_kib = x->boot_kib;
			s.survey = &sv;
		}
		n = inst_screen_lines(&s, l);
		CHECK(n == v->n);
		for (unsigned i = 0; i < n && i < v->n; i++) {
			char got[S15_LINE + 16];

			if (l[i].style == S15_ST_TEXT)
				snprintf(got, sizeof got, "%s", l[i].s);
			else
				snprintf(got, sizeof got, "%s:%s", s15_style_names[l[i].style], l[i].s);
			if (strcmp(got, v->lines[i]) != 0) {
				printf("  %s line %u: got \"%s\", want \"%s\"\n", v->name, i, got,
				       v->lines[i]);
				CHECK(false);
			}
			/* wiki/Screen-Style-Guide.md: at most 56 columns, tabs aside */
			CHECK(strlen(l[i].s) <= S15_COLS + 8u);
		}
	}
}

/* --- the lock --- */

static void all_pass(struct tchk_table *t, struct tchk_switch *sw)
{
	memset(t, 0, sizeof(*t));
	memset(sw, 0, sizeof(*sw));
	for (unsigned i = 0; i < TCHK_COUNT; i++)
		t->r[i].state = TCHK_PASS;
	t->r[TCHK_OS_PARTITION].state = TCHK_FAIL;	/* not part of the lock */
	sw->result = TCHK_SW_READY;
}

/* The lock with the device known, the image its own, nothing else in the way. */
static enum inst_lock lock(const struct tchk_table *t, const struct tchk_switch *sw, bool image_ok,
			   bool selected_ok)
{
	const struct inst_lock_in in = { image_ok, selected_ok, WB_DEV_BY_TABLE, true, true, false };

	return inst_lock_of(t, sw, &in);
}

static void test_lock(void)
{
	static struct tchk_table t;
	static struct tchk_switch sw;
	struct inst_lock_in in = { true, true, WB_DEV_BY_TABLE, true, true, false };

	all_pass(&t, &sw);
	CHECK(lock(&t, &sw, true, true) == INST_LOCK_NONE);
	CHECK(lock(&t, &sw, false, true) == INST_LOCK_IMAGE);
	CHECK(lock(&t, &sw, true, false) == INST_LOCK_SELECTED);
	t.r[TCHK_FLASH_LAYOUT].state = TCHK_FAIL;
	CHECK(lock(&t, &sw, true, true) == INST_LOCK_FAILED);
	all_pass(&t, &sw);
	t.r[TCHK_RTC].state = TCHK_NOT_TESTED;
	CHECK(lock(&t, &sw, true, true) == INST_LOCK_NOT_TESTED);
	all_pass(&t, &sw);
	t.r[TCHK_KEYS].state = TCHK_NOT_TESTED;		/* skipped */
	CHECK(lock(&t, &sw, true, true) == INST_LOCK_GUIDED);
	all_pass(&t, &sw);
	t.r[TCHK_KEYS].state = TCHK_FAIL;
	CHECK(lock(&t, &sw, true, true) == INST_LOCK_GUIDED);
	/* whatever the other boot partition holds, the install is offered */
	all_pass(&t, &sw);
	for (sw.result = TCHK_SW_NOT_RUN; sw.result <= TCHK_SW_READY; sw.result++)
		CHECK(sw.result == TCHK_SW_NO_SLAVE ||
		      lock(&t, &sw, true, true) == INST_LOCK_NONE);
	sw.unverified = true;
	sw.result = TCHK_SW_READY;
	CHECK(lock(&t, &sw, true, true) == INST_LOCK_NONE);
	sw.unverified = false;
	/* an eMMC out of reach still locks */
	sw.result = TCHK_SW_NO_SLAVE;
	CHECK(lock(&t, &sw, true, true) == INST_LOCK_EMMC);
	/* a failed check outranks a missing image */
	all_pass(&t, &sw);
	t.r[TCHK_DDR].state = TCHK_FAIL;
	CHECK(lock(&t, &sw, false, false) == INST_LOCK_FAILED);
	for (unsigned l = 0; l <= INST_LOCK_SURVEY; l++)
		CHECK(strlen(inst_lock_words((enum inst_lock)l)) <= S15_COLS - 3u);

	/* S-18: only a first stage of the table installs, and it comes first */
	all_pass(&t, &sw);
	CHECK(inst_lock_of(&t, &sw, &in) == INST_LOCK_NONE);
	in.how = WB_DEV_UNKNOWN;
	t.r[TCHK_DDR].state = TCHK_FAIL;
	CHECK(inst_lock_of(&t, &sw, &in) == INST_LOCK_DEVICE);
	in.how = WB_DEV_SECOND_CPU;
	CHECK(inst_lock_of(&t, &sw, &in) == INST_LOCK_DEVICE);
	in.how = WB_DEV_BY_TABLE;
	in.device_mine = false;
	CHECK(inst_lock_of(&t, &sw, &in) == INST_LOCK_OTHER_DEVICE);
	in.device_mine = true;
	CHECK(inst_lock_of(&t, &sw, &in) == INST_LOCK_FAILED);
	/* a test not on this device counts as passed */
	all_pass(&t, &sw);
	t.r[TCHK_SLAVE].state = TCHK_NOT_ON_DEVICE;
	t.r[TCHK_BRIDGE].state = TCHK_NOT_ON_DEVICE;
	CHECK(inst_lock_of(&t, &sw, &in) == INST_LOCK_NONE);
	CHECK(tchk_verdict(&t) == TCHK_VERDICT_PASS);
	/* the OP-1F's own bootloader kept, after the selector */
	in.keeps_factory = false;
	CHECK(inst_lock_of(&t, &sw, &in) == INST_LOCK_FACTORY);
	in.selected_ok = false;
	CHECK(inst_lock_of(&t, &sw, &in) == INST_LOCK_SELECTED);
	in.selected_ok = true;
	/* the survey last: it says whether an install would be offered */
	in.survey = true;
	CHECK(inst_lock_of(&t, &sw, &in) == INST_LOCK_FACTORY);
	in.keeps_factory = true;
	CHECK(inst_lock_of(&t, &sw, &in) == INST_LOCK_SURVEY);
	in.image_ok = false;
	CHECK(inst_lock_of(&t, &sw, &in) == INST_LOCK_IMAGE);
}

/* S-02.108: on the OP-1F a trial keeps a slot that is not waffle-boot's. */
static void test_keeps_factory(void)
{
	unsigned t;

	for (t = 0; t < 2u; t++) {
		const uint8_t mine = (uint8_t)(1u << t), other = (uint8_t)(1u << (t ^ 1u));

		/* the OP-XY keeps its rule: any target */
		CHECK(inst_keeps_factory(&wb_device_opxy, t, other));
		CHECK(inst_keeps_factory(NULL, t, 0u));
		/* the OP-1F: two of the unit's own, one stays */
		CHECK(inst_keeps_factory(&wb_device_op1f, t, 0u));
		/* the target waffle-boot already: nothing of the unit's own goes */
		CHECK(inst_keeps_factory(&wb_device_op1f, t, mine));
		CHECK(inst_keeps_factory(&wb_device_op1f, t, (uint8_t)(mine | other)));
		/* the target the last of the unit's own: refused */
		CHECK(!inst_keeps_factory(&wb_device_op1f, t, other));
	}
	CHECK(!inst_keeps_factory(&wb_device_op1f, 0xFFu, 0u));
}

/* --- whether a slot holds a waffle-boot stream --- */

static uint8_t ram[0x2000];

static int ram_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if ((uint64_t)off + len > sizeof ram)
		return -1;
	memcpy(buf, ram + off, len);
	return 0;
}

static size_t put_block(uint8_t *p, uint32_t flags, uint32_t target, const uint8_t *pl, uint32_t n)
{
	uint8_t x = 0;

	wb_put_le32(p, 0xAD000004u | flags);
	wb_put_le32(p + 4, target);
	wb_put_le32(p + 8, n);
	wb_put_le32(p + 12, 0u);
	for (unsigned i = 0; i < 16u; i++)
		x ^= p[i];
	p[2] = x;
	if (pl != NULL && (flags & S05_FLAG_FILL) == 0u)
		memcpy(p + 16, pl, n);
	return 16u + ((flags & S05_FLAG_FILL) == 0u ? n : 0u);
}

/* A stream with a descriptor of one region at FR_DESC_ADDR, or none. */
static void stream(bool desc, bool bad_crc)
{
	uint8_t d[FR_DESC_HEAD + 8u + 4u], pl[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	size_t at = 0;

	memset(ram, 0xFF, sizeof ram);
	memset(d, 0, sizeof d);
	wb_put_le32(d + FR_DESC_O_MAGIC, FR_DESC_MAGIC);
	wb_put_le16(d + FR_DESC_O_VERSION, FR_DESC_VERSION);
	wb_put_le16(d + FR_DESC_O_SIZE, (uint16_t)sizeof d);
	wb_put_le32(d + FR_DESC_O_NREGIONS, 1u);
	wb_put_le32(d + sizeof d - 4u, wire_crc32(0u, d, (uint32_t)sizeof d - 4u) ^ (bad_crc ? 1u : 0u));
	at += put_block(ram + at, S05_FLAG_FIRST | S05_FLAG_IGNORE, 0x11A00000u, NULL, 0u);
	at += put_block(ram + at, 0u, 0x11A04000u, pl, sizeof pl);
	at += put_block(ram + at, S05_FLAG_FILL, 0x08002000u, NULL, 0x100u);
	if (desc)
		at += put_block(ram + at, 0u, FR_DESC_ADDR, d, sizeof d);
	at += put_block(ram + at, 0u, 0x8803D000u, pl, sizeof pl);
	(void)put_block(ram + at, S05_FLAG_FINAL, 0x11A00000u, NULL, 0u);
}

static void test_stream_is_waffle(void)
{
	stream(true, false);
	CHECK(s02_stream_is_waffle(ram_read, NULL, 0u, sizeof ram, crc_of));
	/* its descriptor's CRC wrong, none, or past what may be read */
	stream(true, true);
	CHECK(!s02_stream_is_waffle(ram_read, NULL, 0u, sizeof ram, crc_of));
	stream(false, false);
	CHECK(!s02_stream_is_waffle(ram_read, NULL, 0u, sizeof ram, crc_of));
	stream(true, false);
	CHECK(!s02_stream_is_waffle(ram_read, NULL, 0u, 64u, crc_of));
	/* an erased slot, a header that is no LDR header */
	memset(ram, 0xFF, sizeof ram);
	CHECK(!s02_stream_is_waffle(ram_read, NULL, 0u, sizeof ram, crc_of));
	stream(true, false);
	ram[5] ^= 1u;
	CHECK(!s02_stream_is_waffle(ram_read, NULL, 0u, sizeof ram, crc_of));
	/* a read that fails */
	stream(true, false);
	CHECK(!s02_stream_is_waffle(ram_read, NULL, sizeof ram - 8u, 0x100u, crc_of));
}

static void test_os_and_trial(void)
{
	struct tchk_switch sw;

	memset(&sw, 0, sizeof sw);
	sw.result = TCHK_SW_READY;
	CHECK(inst_os_state(&sw) == INST_OS_CHECKED);
	sw.unverified = true;
	CHECK(inst_os_state(&sw) == INST_OS_ENCRYPTED);
	sw.result = TCHK_SW_RESTORED;
	CHECK(inst_os_state(&sw) == INST_OS_ACTIVE);
	sw.result = TCHK_SW_WRITE_FAILED;
	CHECK(inst_os_state(&sw) == INST_OS_WRITE_FAILED);
	sw.result = TCHK_SW_NOT_RUN;
	CHECK(inst_os_state(&sw) == INST_OS_NOT_LOOKED);

	CHECK(inst_trial_of(OPXY_INSTALL_SLOT_GOOD) == INST_TRIAL_NONE);
	CHECK(inst_trial_of(OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_PENDING) == INST_TRIAL_WAITING);
	CHECK(inst_trial_of(OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_STARTED) ==
	      INST_TRIAL_NOT_REACHED);
	CHECK(inst_trial_of(OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_STARTED |
			    OPXY_INSTALL_SLOT_REACHED) == INST_TRIAL_REACHED);
	CHECK(inst_trial_of(OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_SPENT) == INST_TRIAL_SPENT);
	CHECK(inst_trial_of(OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_ODD) == INST_TRIAL_SPENT);
	CHECK(inst_trial_of(OPXY_INSTALL_SLOT_TRIAL) == INST_TRIAL_NONE);
}

/* --- the hold --- */

#define KEY_1 0x06u	/* gives digit 1 */
#define KEY_8 0x15u	/* gives digit 8 */
#define KNOB_1 0x0Au	/* a knob click that gives digit 1 */

static void key(struct inst_hold *h, unsigned k, bool down, uint32_t t)
{
	inst_hold_key(h, k, s15_digit(&wb_device_opxy, k), down, t);
}

static void test_hold(void)
{
	struct inst_hold h;

	CHECK(s15_digit(&wb_device_opxy, KEY_1) == 1u && s15_digit(&wb_device_opxy, KEY_8) == 8u && s15_digit(&wb_device_opxy, KNOB_1) == 1u);
	memset(&h, 0, sizeof h);
	inst_hold_offer(&h, 1u << 1 | 1u << 8);
	key(&h, KEY_1, true, 1000u);
	CHECK(h.holding && h.digit == 1u);
	CHECK(inst_hold_tick(&h, 5999u) == 0u);
	CHECK(inst_hold_fill(&h, 3500u) == 500u);
	CHECK(inst_hold_tick(&h, 6000u) == 1u);
	CHECK(!h.holding && inst_hold_tick(&h, 7000u) == 0u);	/* once */
	key(&h, KEY_1, false, 7100u);				/* its release: nothing */
	CHECK(!h.holding);

	/* an early release cancels */
	inst_hold_offer(&h, 1u << 1);
	key(&h, KEY_1, true, 0u);
	key(&h, KEY_1, false, 4000u);
	CHECK(inst_hold_tick(&h, 9000u) == 0u && inst_hold_fill(&h, 9000u) == 0u);

	/* another key down cancels; the precomp key does not */
	key(&h, KEY_1, true, 10000u);
	key(&h, 0x17u, true, 11000u);
	CHECK(h.holding);
	key(&h, 0x17u, false, 11100u);
	key(&h, KEY_8, true, 12000u);
	CHECK(!h.holding);
	key(&h, KEY_8, false, 12100u);
	key(&h, KEY_1, false, 12200u);

	/* a digit not offered, a knob click, a key with another down: no hold */
	inst_hold_offer(&h, 1u << 1);
	key(&h, KEY_8, true, 0u);
	CHECK(!h.holding);
	key(&h, KEY_8, false, 10u);
	key(&h, KNOB_1, true, 20u);
	CHECK(!h.holding);
	key(&h, 0x30u, true, 30u);
	key(&h, KEY_1, true, 40u);
	CHECK(!h.holding);
	key(&h, KEY_1, false, 50u);
	key(&h, 0x30u, false, 60u);
	key(&h, KNOB_1, false, 70u);

	/* a key down when the hold was offered acts after an up and a down */
	key(&h, KEY_1, true, 100u);
	inst_hold_offer(&h, 1u << 1);
	CHECK(!h.holding);
	key(&h, KEY_1, true, 200u);
	CHECK(!h.holding);
	key(&h, KEY_1, false, 300u);
	key(&h, KEY_1, true, 400u);
	CHECK(h.holding);
	CHECK(inst_hold_tick(&h, 5400u) == 1u);
}

/* --- the carried image --- */

static void sha_init(void *ctx)
{
	s15_sha256_init(ctx);
}

static void sha_update(void *ctx, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		s15_sha256_byte(ctx, p[i]);
}

static void sha_final(void *ctx, uint8_t out[32])
{
	char hex[65];

	s15_sha256_hex(ctx, hex);
	for (unsigned i = 0; i < 32u; i++) {
		unsigned v = 0;

		(void)sscanf(hex + 2u * i, "%2x", &v);
		out[i] = (uint8_t)v;
	}
}

static uint8_t img[4096];
static uint32_t img_len;

static void block(uint32_t flags, uint32_t target, const uint8_t *p, uint32_t n)
{
	uint8_t *h = img + img_len;
	uint8_t x = 0;

	wb_put_le32(h, flags | 0xAD000000u);
	wb_put_le32(h + 4, target);
	wb_put_le32(h + 8, n);
	wb_put_le32(h + 12, 0u);
	for (unsigned i = 0; i < 16u; i++)
		x = (uint8_t)(x ^ h[i]);
	h[2] = x;		/* the header's XOR byte makes the whole zero */
	img_len += 16u;
	if (p != NULL && (flags & S05_FLAG_FILL) == 0u) {
		memcpy(img + img_len, p, n);
		img_len += n;
	}
}

/* A stream with one front block in the L2 window, the descriptor, and FINAL;
 * pin gets the front's pin. bad: 1 the descriptor's pin wrong, 2 a FILL
 * block in a front window. */
static void make_image(uint8_t pin[32], unsigned bad)
{
	static const uint8_t front[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	struct s15_sha256 sh;
	uint8_t d[FR_DESC_HEAD + 4u], head[8];

	img_len = 0;
	memset(img, 0, sizeof img);
	s15_sha256_init(&sh);
	wb_put_le32(head, FR_L2_START);
	wb_put_le32(head + 4, sizeof front);
	sha_update(&sh, head, 8);
	sha_update(&sh, front, sizeof front);
	sha_final(&sh, pin);
	block(0x0010u, FR_L2_START, front, sizeof front);
	if (bad == 2u)
		block(S05_FLAG_FILL, FR_L1_DATA_START, NULL, 16u);
	memset(d, 0, sizeof d);
	wb_put_le32(d + FR_DESC_O_MAGIC, FR_DESC_MAGIC);
	wb_put_le16(d + FR_DESC_O_VERSION, FR_DESC_VERSION);
	wb_put_le16(d + FR_DESC_O_SIZE, FR_DESC_HEAD + 4u);
	wb_put_le32(d + FR_DESC_O_NREGIONS, 0u);
	wb_put_le32(d + FR_DESC_O_FRONT_CRC, crc_of(0u, front, sizeof front));
	wb_put_le16(d + FR_DESC_O_BL_VERSION, 1u);
	wb_put_le16(d + FR_DESC_O_BL_VERSION + 2u, 0u);
	wb_put_le16(d + FR_DESC_O_BL_VERSION + 4u, 3u);
	memcpy(d + FR_DESC_O_FRONT_PIN, pin, 32);
	if (bad == 1u)
		d[FR_DESC_O_FRONT_PIN] ^= 1u;
	wb_put_le32(d + FR_DESC_HEAD, crc_of(0u, d, FR_DESC_HEAD));
	block(0x0010u, FR_DESC_ADDR, d, sizeof d);
	block(S05_FLAG_FINAL | 0x0010u, 0x11A00000u, NULL, 0u);
}

static void test_image_check(void)
{
	struct s15_sha256 sh;
	const struct inst_sha_ops ops = { &sh, sha_init, sha_update, sha_final };
	struct inst_image r;
	uint8_t pin[32], other[32];

	make_image(pin, 0u);
	CHECK(inst_image_check(img, img_len, crc_of(0u, img, img_len), pin, crc_of, &ops, &r));
	CHECK(r.crc_ok && r.stream_ok && r.desc_ok && r.front_crc_ok && r.pin_ok);
	CHECK(r.version[0] == 1u && r.version[1] == 0u && r.version[2] == 3u);
	CHECK(memcmp(r.pin, pin, 32) == 0);
	/* the build's CRC differs */
	CHECK(!inst_image_check(img, img_len, crc_of(0u, img, img_len) ^ 1u, pin, crc_of, &ops,
				&r));
	CHECK(!r.crc_ok && r.pin_ok);
	/* the pinned front is another */
	memcpy(other, pin, 32);
	other[31] ^= 0x80u;
	CHECK(!inst_image_check(img, img_len, crc_of(0u, img, img_len), other, crc_of, &ops, &r));
	CHECK(!r.pin_ok && r.front_crc_ok);
	/* the descriptor's pin is not the front's own */
	make_image(pin, 1u);
	CHECK(!inst_image_check(img, img_len, crc_of(0u, img, img_len), pin, crc_of, &ops, &r));
	CHECK(r.desc_ok && !r.pin_ok);
	/* a FILL block in a front window */
	make_image(pin, 2u);
	CHECK(!inst_image_check(img, img_len, crc_of(0u, img, img_len), pin, crc_of, &ops, &r));
	CHECK(!r.front_crc_ok && !r.pin_ok);
	/* cut short: no FINAL */
	make_image(pin, 0u);
	CHECK(!inst_image_check(img, img_len - 16u, crc_of(0u, img, img_len - 16u), pin, crc_of,
				&ops, &r));
	CHECK(!r.stream_ok);
}

/* --- the protocol --- */

static struct inst_proto pr;
static struct inst_state st;
static struct inst_hello he;
static struct tchk_table tab;
static const char rep[] = "waffle-boot check\nverdict: PASS\n";

static const struct inst_survey *survey_p;

static struct inst_view view(void)
{
	const struct inst_view v = { &he, &st, &tab, rep, sizeof rep - 1u, survey_p };

	return v;
}

/* Feeds a whole request; the reply's bytes into out, their count. */
static size_t ask(const uint8_t *req, size_t n, uint8_t *out)
{
	const struct inst_view v = view();
	size_t k, got = 0;
	const uint8_t *p;

	CHECK(inst_proto_feed(&pr, req, n, &v) == n);
	p = inst_proto_out(&pr, &k);
	memcpy(out, p, k);
	got = k;
	inst_proto_sent(&pr, k);
	return got;
}

static void test_proto(void)
{
	/* HELLO, seq 7: 57 52 40 07 00 00, then the CRC-32 of 40 07 00 00 */
	uint8_t req[64], out[INST_FRAME_MAX];
	size_t n;

	inst_proto_init(&pr, crc_of);
	memset(&st, 0, sizeof st);
	memset(&he, 0, sizeof he);
	memset(&tab, 0, sizeof tab);
	st.phase = INST_READY;
	st.flags = INST_F_CHECKS_DONE | INST_F_UNLOCKED | INST_F_IMAGE_OK;
	st.target = 1u;
	st.slot_state[0] = OPXY_INSTALL_SLOT_GOOD;
	st.os = INST_OS_CHECKED;
	st.hold_fill = 0x1234u;
	st.next_digit = 1u;
	he.version[0] = 1u;
	he.version[2] = 3u;
	he.len = 0x00012345u;
	he.crc = 0xCAFEF00Du;
	for (unsigned i = 0; i < 32u; i++)
		he.pin[i] = (uint8_t)i;
	tab.r[TCHK_FLASH_ID].state = TCHK_PASS;
	tab.r[TCHK_FLASH_LAYOUT].state = TCHK_FAIL;
	tchk_text_init(&tab.r[TCHK_FLASH_LAYOUT].detail, TCHK_DETAIL_COLS);
	tchk_put_str(&tab.r[TCHK_FLASH_LAYOUT].detail, "slot 1 bad");

	req[0] = 0x57;
	req[1] = 0x52;
	req[2] = 0x40;
	req[3] = 0x07;
	req[4] = 0;
	req[5] = 0;
	wb_put_le32(req + 6, crc_of(0u, req + 2, 4u));
	n = ask(req, 10u, out);
	CHECK(n == INST_FRAME_HEAD + INST_HELLO_LEN + 4u);
	CHECK(out[0] == 0x57 && out[1] == 0x52 && out[2] == 0xC0 && out[3] == 0x07);
	CHECK(wb_get_le16(out + 4) == INST_HELLO_LEN);
	CHECK(memcmp(out + 6, "WBIN", 4) == 0 && out[10] == 1u);
	CHECK(out[11] == INST_READY && out[12] == st.flags && out[13] == TCHK_COUNT);
	CHECK(wb_get_le16(out + 14) == 1u && wb_get_le16(out + 18) == 3u);
	CHECK(wb_get_le32(out + 22) == 0x00012345u && wb_get_le32(out + 26) == 0xCAFEF00Du);
	CHECK(out[30] == 0u && out[61] == 31u);
	CHECK(wb_get_le32(out + n - 4u) == crc_of(0u, out + 2, (uint32_t)(n - 6u)));

	/* junk, a frame with a bad CRC, then STATE (seq 9): one reply */
	{
		static const uint8_t junk[] = { 0x00, 0x57, 0x57, 0x52, 0x41, 0x01, 0x00, 0x00, 1, 2, 3,
						4 };
		uint8_t buf[64];
		size_t k = sizeof junk;

		memcpy(buf, junk, k);
		buf[k++] = 0x57;
		buf[k++] = 0x52;
		buf[k++] = 0x41;
		buf[k++] = 0x09;
		buf[k++] = 0;
		buf[k++] = 0;
		wb_put_le32(buf + k, crc_of(0u, buf + k - 4u, 4u));
		k += 4u;
		n = ask(buf, k, out);
	}
	CHECK(n == INST_FRAME_HEAD + INST_STATE_HEAD + TCHK_COUNT + 4u);
	CHECK(out[2] == 0xC1 && out[3] == 0x09);
	CHECK(out[6] == INST_READY && out[6 + 9] == 1u && out[6 + 11] == INST_OS_CHECKED);
	CHECK(wb_get_le16(out + 6 + 12) == 0x1234u && out[6 + 16] == 1u);
	CHECK(out[6 + 32 + TCHK_FLASH_ID] == TCHK_PASS);
	CHECK(out[6 + 32 + TCHK_FLASH_LAYOUT] == TCHK_FAIL);
	CHECK(pr.dropped != 0u);

	/* TEST of flash-layout: its strings */
	n = inst_frame(req, INST_T_TEST, 3u, (const uint8_t[]){ TCHK_FLASH_LAYOUT }, 1u, crc_of);
	n = ask(req, n, out);
	CHECK(out[2] == 0xC2 && out[6] == INST_ST_OK && out[7] == TCHK_FLASH_LAYOUT);
	CHECK(out[8] == TCHK_FAIL && out[9] == TCHK_KIND_AUTO);
	CHECK(strcmp((const char *)out + 10, tchk_tests[TCHK_FLASH_LAYOUT].id) == 0);
	{
		const char *s = (const char *)out + 10;

		s += strlen(s) + 1u;
		CHECK(strcmp(s, tchk_tests[TCHK_FLASH_LAYOUT].name) == 0);
		s += strlen(s) + 1u;
		CHECK(strcmp(s, "slot 1 bad") == 0);
		s += strlen(s) + 1u;
		CHECK(*s == '\0');
	}
	n = inst_frame(req, INST_T_TEST, 4u, (const uint8_t[]){ TCHK_COUNT }, 1u, crc_of);
	n = ask(req, n, out);
	CHECK(wb_get_le16(out + 4) == 1u && out[6] == INST_ST_NO_TEST);

	/* REPORT from offset 9 */
	{
		uint8_t off[4];

		wb_put_le32(off, 9u);
		n = inst_frame(req, INST_T_REPORT, 5u, off, 4u, crc_of);
	}
	n = ask(req, n, out);
	CHECK(out[2] == 0xC3 && out[6] == INST_ST_OK);
	CHECK(wb_get_le32(out + 7) == sizeof rep - 1u);
	CHECK(wb_get_le16(out + 4) == 5u + sizeof rep - 1u - 9u);
	CHECK(memcmp(out + 11, rep + 9, sizeof rep - 1u - 9u) == 0);

	/* recovery's HELLO: unknown here */
	n = inst_frame(req, 0x01u, 6u, NULL, 0u, crc_of);
	n = ask(req, n, out);
	CHECK(out[2] == 0x81 && out[3] == 6u && wb_get_le16(out + 4) == 1u && out[6] == 0xFFu);

	/* a length over 1032 is not a frame */
	req[0] = 0x57;
	req[1] = 0x52;
	req[2] = 0x40;
	req[3] = 1;
	wb_put_le16(req + 4, 1033u);
	{
		const struct inst_view v = view();

		(void)inst_proto_feed(&pr, req, 6u, &v);
		(void)inst_proto_out(&pr, &n);
		CHECK(n == 0u && pr.rx_n == 0u);
	}

	/* no request is read while a reply waits */
	{
		const struct inst_view v = view();
		uint8_t two[20];
		const size_t a = inst_frame(two, INST_T_STATE, 1u, NULL, 0u, crc_of);
		size_t k;

		(void)inst_frame(two + a, INST_T_STATE, 2u, NULL, 0u, crc_of);
		CHECK(inst_proto_feed(&pr, two, 2u * a, &v) == a);
		(void)inst_proto_out(&pr, &k);
		inst_proto_sent(&pr, k - 1u);	/* one byte still to go */
		CHECK(inst_proto_feed(&pr, two + a, a, &v) == 0u);
		inst_proto_sent(&pr, 1u);
		CHECK(inst_proto_feed(&pr, two + a, a, &v) == a);
		CHECK(inst_proto_out(&pr, &k)[3] == 2u);
		inst_proto_sent(&pr, k);
	}
}

/* --- check.txt's install part --- */

static void test_report(void)
{
	static char buf[2048];
	uint8_t eng[OPXY_INSTALL_REPLY];
	struct inst_image img_r;
	size_t n;

	memset(&img_r, 0, sizeof img_r);
	img_r.crc_ok = img_r.stream_ok = img_r.desc_ok = img_r.front_crc_ok = img_r.pin_ok = true;
	memset(eng, 0xFF, sizeof eng);
	eng[8] = OPXY_INSTALL_MIRROR_SAME;
	eng[9] = 0xF0u;				/* the tried slot's log byte: no crumbs */
	eng[9 + OPXY_INSTALL_CRUMBS] = 1u;
	eng[9 + OPXY_INSTALL_CRUMBS + 3] = 5u;
	eng[9 + OPXY_INSTALL_CRUMBS + 4] = 0u;
	eng[9 + OPXY_INSTALL_CRUMBS + 5] = 0u;
	eng[9 + OPXY_INSTALL_CRUMBS + 6] = 0u;
	st.phase = INST_COMMIT_READY;
	st.trial = INST_TRIAL_REACHED;
	st.last_rc = 0xFFu;
	st.lock = INST_LOCK_NONE;
	strcpy(buf, "head\n");
	n = inst_report_append(buf, 5u, sizeof buf, &he, &st, &img_r, eng);
	CHECK(n == strlen(buf));
	CHECK(strstr(buf, "\ninstall\ndevice: OP-XY, identified by its first stage\n"
		     "image built for: OP-XY\nimage: waffle-boot v1.0.3, 74565 bytes, crc32 cafef00d\n") != NULL);
	CHECK(strstr(buf, "front pin: 000102") != NULL);
	CHECK(strstr(buf, "image check: pass\n") != NULL);
	CHECK(strstr(buf, "phase: commit ready\n") != NULL);
	CHECK(strstr(buf, "trial: reached the boot menu\n") != NULL);
	CHECK(strstr(buf, "target: Slot 2\n") != NULL);
	CHECK(strstr(buf, "start log: 5 used, 0 unfinished at the end\n") != NULL);
	CHECK(strstr(buf, "breadcrumbs") == NULL);
	/* the other boot partition in plain words, never a failure */
	st.os = INST_OS_NONE;
	CHECK(inst_report_append(buf, 5u, sizeof buf, &he, &st, &img_r, eng) == strlen(buf));
	CHECK(strstr(buf, "\nother boot partition: no OS header\n") != NULL);
	CHECK(strstr(buf, "FAIL") == NULL && strstr(buf, "OS: ") == NULL);
	st.os = INST_OS_CHECKED;
	eng[10] = 0x01u;
	n = inst_report_append(buf, 5u, sizeof buf, &he, &st, &img_r, eng);
	CHECK(n == strlen(buf));
	CHECK(strstr(buf, "breadcrumbs: 01 ff") != NULL);
	CHECK(strstr(buf, "not read yet") == NULL);
	/* during the checks the engine has not read the flash: no selector */
	st.phase = INST_CHECKING;
	n = inst_report_append(buf, 5u, sizeof buf, &he, &st, &img_r, NULL);
	CHECK(n == strlen(buf));
	CHECK(strstr(buf, "selector and slots: not read yet\n") != NULL);
	CHECK(strstr(buf, "selector: ") == NULL && strstr(buf, "target: ") == NULL);
	/* never past the buffer */
	n = inst_report_append(buf, 5u, 40u, &he, &st, &img_r, eng);
	CHECK(n == 39u && buf[39] == '\0');
}

/* S-18 in the protocol: HELLO's device bytes, STATE's streams, SURVEY. */
static void test_proto_device(void)
{
	static struct inst_survey sv;
	uint8_t req[16], out[INST_FRAME_MAX];
	size_t n;

	inst_proto_init(&pr, crc_of);
	memset(&he, 0, sizeof he);
	memset(&st, 0, sizeof st);
	he.device = WB_DEV_OP1F;
	he.how = WB_DEV_BY_TABLE;
	he.image_device = WB_DEV_OP1F;
	he.survey = 1u;
	st.streams = 0x02u;
	n = inst_frame(req, INST_T_HELLO, 1u, NULL, 0u, crc_of);
	n = ask(req, n, out);
	CHECK(n == INST_FRAME_HEAD + INST_HELLO_LEN + 4u);
	CHECK(out[6 + 56] == WB_DEV_OP1F && out[6 + 57] == WB_DEV_BY_TABLE);
	CHECK(out[6 + 58] == WB_DEV_OP1F && out[6 + 59] == 1u);
	CHECK(out[6 + 60] == 0u && out[6 + 63] == 0u);
	n = inst_frame(req, INST_T_STATE, 2u, NULL, 0u, crc_of);
	n = ask(req, n, out);
	CHECK(n == INST_FRAME_HEAD + INST_STATE_HEAD + TCHK_COUNT + 4u && out[6 + 17] == 0x02u);
	/* SURVEY outside a survey build: not a request it knows */
	survey_p = NULL;
	n = inst_frame(req, INST_T_SURVEY, 3u, NULL, 0u, crc_of);
	n = ask(req, n, out);
	CHECK(n == INST_FRAME_HEAD + 1u + 4u && out[2] == (INST_T_SURVEY | 0x80u) &&
	      wb_get_le16(out + 4) == 1u && out[6] == INST_ST_UNKNOWN);
	memset(&sv, 0, sizeof sv);
	sv.device = WB_DEV_OP1F;
	sv.fs_n = 0x8600u;
	sv.fs_c = 0x49267EB8u;
	sv.slot[1].bits = INST_SV_RECORD | INST_SV_WAFFLE;
	sv.slot[1].log[7] = 0xC3u;
	survey_p = &sv;
	n = inst_frame(req, INST_T_SURVEY, 4u, NULL, 0u, crc_of);
	n = ask(req, n, out);
	CHECK(n == INST_FRAME_HEAD + INST_SURVEY_LEN + 4u);
	CHECK(out[6] == INST_ST_OK && out[7] == WB_DEV_OP1F);
	CHECK(wb_get_le32(out + 6 + 4) == 0x8600u && wb_get_le32(out + 6 + 8) == 0x49267EB8u);
	CHECK(out[6 + 52] == (INST_SV_RECORD | INST_SV_WAFFLE) && out[6 + 52 + 23] == 0xC3u);
	survey_p = NULL;
	memset(&he, 0, sizeof he);
	memset(&st, 0, sizeof st);
}

/* The hold on the OP-1F: no precomp key, its own knob clicks and digits. */
static void test_hold_op1f(void)
{
	const struct wb_device *d = &wb_device_op1f;
	struct inst_hold h;

	memset(&h, 0, sizeof h);
	h.dev = d;
	inst_hold_offer(&h, 1u << 1 | 1u << 8);
	/* 0x17 is an ordinary key there: down during a hold, it ends it */
	inst_hold_key(&h, 0x06u, s15_digit(d, 0x06u), true, 1000u);
	CHECK(h.holding && h.digit == 1u);
	inst_hold_key(&h, 0x17u, s15_digit(d, 0x17u), true, 2000u);
	CHECK(!h.holding);
	inst_hold_key(&h, 0x17u, s15_digit(d, 0x17u), false, 2100u);
	inst_hold_key(&h, 0x06u, s15_digit(d, 0x06u), false, 2200u);
	/* the knob's click of digit 1 starts no hold; digit 8 is 0x14 */
	CHECK(s15_digit(d, 0x36u) == 1u && s15_digit(d, 0x14u) == 8u);
	inst_hold_key(&h, 0x36u, 1u, true, 3000u);
	CHECK(!h.holding);
	inst_hold_key(&h, 0x36u, 1u, false, 3100u);
	inst_hold_key(&h, 0x14u, s15_digit(d, 0x14u), true, 4000u);
	CHECK(h.holding && h.digit == 8u);
	CHECK(inst_hold_tick(&h, 4000u + S15_HOLD_MS) == 8u);
}

/* check.txt names the device, and a device not identified by n and c. */
static void test_report_device(void)
{
	static char buf[2048];
	struct inst_image img_r;
	struct inst_hello h;
	struct inst_state s;

	memset(&img_r, 0, sizeof img_r);
	memset(&h, 0, sizeof h);
	memset(&s, 0, sizeof s);
	s.last_rc = 0xFFu;
	h.how = WB_DEV_UNKNOWN;
	h.fs_n = 34000u;
	h.fs_c = 0x12345678u;
	s.lock = INST_LOCK_DEVICE;
	(void)inst_report_append(buf, 0u, sizeof buf, &h, &s, &img_r, NULL);
	CHECK(strstr(buf, "\ninstall\ndevice: not identified, first stage n 34000, c 12345678\n"
		     "image built for: OP-XY\n") != NULL);
	CHECK(strstr(buf, "locked: The device is not identified by its first stage.\n") != NULL);
	h.how = WB_DEV_BY_TABLE;
	h.device = WB_DEV_OP1F;
	h.image_device = WB_DEV_OP1F;
	h.survey = 1u;
	(void)inst_report_append(buf, 0u, sizeof buf, &h, &s, &img_r, NULL);
	CHECK(strstr(buf, "device: OP-1F, identified by its first stage\nimage built for: OP-1F, a "
		     "survey build: no SPI-flash write\n") != NULL);
	CHECK(strstr(buf, "34000") == NULL);
	CHECK(strstr(inst_rc_words(OPXY_INSTALL_REFUSED_FACTORY), "own bootloader") != NULL);
}

/* --- the test OS's decision at its entry (S-18.001 to S-18.004, as fr_decide()) --- */

static uint8_t fs_img[0x8600];
static unsigned fs_reads, fs_fail_tries, fs_busy;
static uint32_t fs_tries_seen;

/* The first stage; the first fs_fail_tries whole tries fail at their
 * second read, with WB_FS_READ_BUSY when fs_busy. */
static int fs_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	fs_reads++;
	if (off == 0u)
		fs_tries_seen++;
	if (off != 0u && fs_tries_seen <= fs_fail_tries)
		return fs_busy ? WB_FS_READ_BUSY : -1;
	if ((uint64_t)off + len > sizeof fs_img)
		return -1;
	memcpy(buf, fs_img + off, len);
	return 0;
}

static uint16_t strap_val;
static unsigned strap_calls;

static uint16_t fs_strap(void *ctx)
{
	(void)ctx;
	strap_calls++;
	return strap_val;
}

/* One decision, the counts cleared first. */
static void decide(unsigned bmode, struct wb_dev_role *r)
{
	fs_reads = 0;
	fs_tries_seen = 0;
	strap_calls = 0;
	wb_device_decide_role(bmode, fs_strap, NULL, fs_read, NULL, r);
}

static void test_device_role(void)
{
	struct wb_dev_role r;
	uint32_t n, c;

	memset(fs_img, 0x5A, sizeof fs_img);
	wb_put_le32(fs_img + WB_FS_ARG_OFF, (uint32_t)sizeof fs_img - WB_FS_HEAD);
	/* boot mode 1: a main CPU, no sample; a first stage in no row is
	 * read three times (S-18.003) */
	fs_fail_tries = 0;
	decide(WB_BOOT_MODE_MAIN, &r);
	CHECK(r.main && !r.fallback && strap_calls == 0u);
	CHECK(r.n == sizeof fs_img && r.c == wire_crc32(0u, fs_img, sizeof fs_img));
	CHECK(r.d.how == WB_DEV_UNKNOWN && r.d.id == WB_DEV_DEFAULT);	/* in no row */
	CHECK(fs_tries_seen == WB_FS_TRIES);
	/* another boot mode: the fallback sample; PA2 = 0 the second CPU, no
	 * flash read */
	strap_val = 0x0001u;
	decide(0u, &r);
	CHECK(!r.main && strap_calls == 1u && r.d.how == WB_DEV_SECOND_CPU && fs_reads == 0u && r.n == 0u);
	/* PA2 = 1: a main CPU all the same, flagged */
	strap_val = WB_STRAP_PA2;
	decide(2u, &r);
	CHECK(r.main && r.fallback && strap_calls == 1u && fs_tries_seen == WB_FS_TRIES);
	/* a read error: the whole read again, up to three tries */
	fs_fail_tries = 2u;
	fs_busy = 0u;
	decide(WB_BOOT_MODE_MAIN, &r);
	CHECK(fs_tries_seen == 3u && r.c == wire_crc32(0u, fs_img, sizeof fs_img));
	fs_fail_tries = 3u;
	decide(WB_BOOT_MODE_MAIN, &r);
	CHECK(fs_tries_seen == 3u && r.d.how == WB_DEV_UNKNOWN && r.c == 0u && r.n == sizeof fs_img);
	/* a busy flash is not read again */
	fs_busy = 1u;
	decide(WB_BOOT_MODE_MAIN, &r);
	CHECK(fs_tries_seen == 1u && r.d.how == WB_DEV_UNKNOWN && r.c == 0u);
	fs_busy = 0u;
	fs_fail_tries = 0u;
	/* the one-try form keeps its one try */
	fs_fail_tries = 1u;
	fs_tries_seen = 0u;
	(void)wb_first_stage_identify(fs_read, NULL, &n, &c);
	CHECK(fs_tries_seen == 1u && c == 0u);
	fs_fail_tries = 0u;
	/* the table's rows name their devices through the tries too */
	CHECK(wb_first_stage_lookup(wb_device_op1f.first_stage_n, wb_device_op1f.first_stage_crc, &r.d.id) &&
	      r.d.id == WB_DEV_OP1F);
}

int main(void)
{
	RUN(test_screens);
	RUN(test_lock);
	RUN(test_keeps_factory);
	RUN(test_stream_is_waffle);
	RUN(test_os_and_trial);
	RUN(test_hold);
	RUN(test_image_check);
	RUN(test_proto);
	RUN(test_report);
	RUN(test_proto_device);
	RUN(test_hold_op1f);
	RUN(test_report_device);
	RUN(test_device_role);
	return check_summary("test_board_installer");
}
