/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The installer's pure parts (src/testos/installer_logic.c, docs/install.md):
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
			/* docs/ui-style-guide.md: at most 56 columns, tabs aside */
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
	t->r[TCHK_OS_PARTITION].state = TCHK_NOT_PASSED;	/* not part of the lock */
	sw->result = TCHK_SW_READY;
}

static void test_lock(void)
{
	static struct tchk_table t;
	static struct tchk_switch sw;

	all_pass(&t, &sw);
	CHECK(inst_lock_of(&t, &sw, true, true) == INST_LOCK_NONE);
	CHECK(inst_lock_of(&t, &sw, false, true) == INST_LOCK_IMAGE);
	CHECK(inst_lock_of(&t, &sw, true, false) == INST_LOCK_SELECTED);
	t.r[TCHK_FLASH_LAYOUT].state = TCHK_FAIL;
	CHECK(inst_lock_of(&t, &sw, true, true) == INST_LOCK_FAILED);
	all_pass(&t, &sw);
	t.r[TCHK_RTC].state = TCHK_NOT_TESTED;
	CHECK(inst_lock_of(&t, &sw, true, true) == INST_LOCK_NOT_TESTED);
	all_pass(&t, &sw);
	t.r[TCHK_KEYS].state = TCHK_NOT_TESTED;		/* skipped */
	CHECK(inst_lock_of(&t, &sw, true, true) == INST_LOCK_GUIDED);
	all_pass(&t, &sw);
	t.r[TCHK_KEYS].state = TCHK_FAIL;
	CHECK(inst_lock_of(&t, &sw, true, true) == INST_LOCK_GUIDED);
	all_pass(&t, &sw);
	sw.result = TCHK_SW_NO_OTHER_OS;
	CHECK(inst_lock_of(&t, &sw, true, true) == INST_LOCK_NO_OS);
	sw.result = TCHK_SW_OTHER_BAD;
	CHECK(inst_lock_of(&t, &sw, true, true) == INST_LOCK_NO_OS);
	sw.result = TCHK_SW_NO_SLAVE;
	CHECK(inst_lock_of(&t, &sw, true, true) == INST_LOCK_EMMC);
	/* a failed check outranks a missing image */
	all_pass(&t, &sw);
	t.r[TCHK_DDR].state = TCHK_FAIL;
	CHECK(inst_lock_of(&t, &sw, false, false) == INST_LOCK_FAILED);
	for (unsigned l = 0; l <= INST_LOCK_EMMC; l++)
		CHECK(strlen(inst_lock_words((enum inst_lock)l)) <= S15_COLS - 3u);
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
	inst_hold_key(h, k, s15_digit(k), down, t);
}

static void test_hold(void)
{
	struct inst_hold h;

	CHECK(s15_digit(KEY_1) == 1u && s15_digit(KEY_8) == 8u && s15_digit(KNOB_1) == 1u);
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

static struct inst_view view(void)
{
	const struct inst_view v = { &he, &st, &tab, rep, sizeof rep - 1u };

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
	CHECK(strstr(buf, "\ninstall\nimage: waffle-boot v1.0.3, 74565 bytes, crc32 cafef00d\n") != NULL);
	CHECK(strstr(buf, "front pin: 000102") != NULL);
	CHECK(strstr(buf, "image check: pass\n") != NULL);
	CHECK(strstr(buf, "phase: commit ready\n") != NULL);
	CHECK(strstr(buf, "trial: reached the boot menu\n") != NULL);
	CHECK(strstr(buf, "target: Slot 2\n") != NULL);
	CHECK(strstr(buf, "start log: 5 used, 0 unfinished at the end\n") != NULL);
	CHECK(strstr(buf, "breadcrumbs") == NULL);
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

int main(void)
{
	RUN(test_screens);
	RUN(test_lock);
	RUN(test_os_and_trial);
	RUN(test_hold);
	RUN(test_image_check);
	RUN(test_proto);
	RUN(test_report);
	return check_summary("test_board_installer");
}
