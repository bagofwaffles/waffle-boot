/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Recovery's protocol (src/front/recovery/proto.c) over an in-memory port and a
 * fake SPI flash, with the front's own writer: a whole write, every
 * refusal, DATA out of order and DATA's size, damaged frames, replies that
 * wait for the port, and RESTART; BEGIN's flags byte, INFO's fields for a
 * write of the running slot, the target and refusals for every state the
 * screen and INFO show (S-02.084, S-02.093), the battery request before an
 * in-place write, the requests served during one, the first erase's place
 * after END, and the retries after a failed write of the running slot;
 * protocol version 2: the countdown before a host's write or action, its
 * stop when the host leaves, ACT, the force flag on BEGIN, ACT and RESTART,
 * and INFO's new fields; the device fields of front version 20 (S-18.004).
 */
#include <string.h>

#include "support/check.h"
#include "vectors/s02_vectors.h"

#include "front/front.h"
#include "front/handoff.h"
#include "front/recovery/proto.h"
#include "front/slots/slots.h"

/* --- the fake flash: NOR, erase to 0xFF, program clears bits --- */

#define FLASH_SIZE 0x100000u
static uint8_t flash[FLASH_SIZE];
static uint32_t fail_program_at = UINT32_MAX;	/* a program touching it fails */
static uint32_t fail_erase_at = UINT32_MAX;	/* the erase of this sector fails */
static bool erase_then_fail;			/* ... after it has erased */
static bool sel_prog_lazy;	/* a selector program reports success, stores nothing */
static uint32_t fail_read_once_at = UINT32_MAX;	/* the next read starting here fails */
static unsigned fail_read_skip;	/* ... after this many reads starting there */
static bool ver_prog_lazy;	/* the 1-byte verified program reports success,
				 * stores nothing */
static uint32_t fail_read_always_at = UINT32_MAX;	/* every read starting here fails */
static unsigned n_reads;	/* reads made */
static unsigned page_faults;

/* every erase and program, in order */
struct fop {
	char kind;
	uint32_t off;
};
static struct fop fops[8192];
static unsigned n_fops, n_erase;

static void fop_log(char kind, uint32_t off)
{
	if (n_fops < ARRAY_LEN(fops))
		fops[n_fops++] = (struct fop){ kind, off };
}

static int f_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (off > FLASH_SIZE || len > FLASH_SIZE - off)
		return FR_FLASH_IO;
	n_reads++;
	if (off == fail_read_once_at) {
		if (fail_read_skip == 0u) {
			fail_read_once_at = UINT32_MAX;
			return FR_FLASH_IO;
		}
		fail_read_skip--;
	}
	if (off == fail_read_always_at)
		return FR_FLASH_IO;
	memcpy(buf, flash + off, len);
	return FR_FLASH_OK;
}

static int f_erase(void *ctx, uint32_t off)
{
	(void)ctx;
	if (off % FR_SECTOR || off >= FLASH_SIZE)
		return FR_FLASH_IO;
	fop_log('e', off);
	n_erase++;
	if (off == fail_erase_at) {
		if (erase_then_fail)
			memset(flash + off, 0xFF, FR_SECTOR);
		return FR_FLASH_IO;
	}
	memset(flash + off, 0xFF, FR_SECTOR);
	return FR_FLASH_OK;
}

static int f_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	uint32_t i;

	(void)ctx;
	if (len == 0 || off / FR_PAGE != (off + len - 1u) / FR_PAGE)
		page_faults++;
	if (off > FLASH_SIZE || len > FLASH_SIZE - off)
		return FR_FLASH_IO;
	fop_log('p', off);
	if (fail_program_at >= off && fail_program_at < off + len)
		return FR_FLASH_IO;
	if (sel_prog_lazy && off == FR_SELECTOR_OFF)
		return FR_FLASH_OK;
	if (ver_prog_lazy && off == FR_SELECTOR_OFF + 1u && len == 1u)
		return FR_FLASH_OK;
	for (i = 0; i < len; i++)
		flash[off + i] &= buf[i];
	return FR_FLASH_OK;
}

static const struct fr_flash fl = { f_read, f_erase, f_program, NULL };

/* --- the port: host to device, device to host, with room for
 * write_limit bytes a poll --- */

static uint8_t h2d[8192], d2h[8192];
static size_t h2d_n, h2d_at, d2h_n;
static size_t write_limit = SIZE_MAX, room;

static size_t p_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t k = h2d_n - h2d_at < max ? h2d_n - h2d_at : max;

	(void)ctx;
	memcpy(buf, h2d + h2d_at, k);
	h2d_at += k;
	if (h2d_at == h2d_n)
		h2d_at = h2d_n = 0;
	return k;
}

static size_t p_write(void *ctx, const uint8_t *buf, size_t n)
{
	size_t k = n < room ? n : room;

	(void)ctx;
	room -= k;
	if (k > sizeof d2h - d2h_n)
		k = sizeof d2h - d2h_n;
	memcpy(d2h + d2h_n, buf, k);
	d2h_n += k;
	return k;
}

static bool p_idle(void *ctx)
{
	(void)ctx;
	return true;
}

static const struct fr_bytes port = { p_read, p_write, p_idle, NULL };

static void raw_send(const uint8_t *b, size_t n)
{
	memcpy(h2d + h2d_n, b, n);
	h2d_n += n;
}

static size_t frame(uint8_t *o, uint8_t type, uint8_t seq, const uint8_t *pl, size_t n)
{
	o[0] = 'W';
	o[1] = 'R';
	o[2] = type;
	o[3] = seq;
	fr_put16(o + 4, (uint16_t)n);
	if (n)
		memcpy(o + 6, pl, n);
	fr_put32(o + 6 + n, fr_crc32(0, o + 2, 4 + n));
	return 6 + n + 4;
}

static void send(uint8_t type, uint8_t seq, const uint8_t *pl, size_t n)
{
	uint8_t f[FR_PROTO_HEAD + FR_PROTO_MAX + 4];
	const size_t k = frame(f, type, seq, pl, n);

	raw_send(f, k);
}

/* The first reply waiting: its payload, or -1 when none is whole. */
static int take(uint8_t want_type, uint8_t want_seq, uint8_t *pl)
{
	size_t n;
	int len;

	if (d2h_n < 6)
		return -1;
	n = fr_le16(d2h + 4);
	if (d2h_n < 6 + n + 4)
		return -1;
	CHECK(d2h[0] == 'W' && d2h[1] == 'R');
	CHECK(d2h[2] == (want_type | FR_T_REPLY));
	CHECK(d2h[3] == want_seq);
	CHECK(fr_crc32(0, d2h + 2, 4 + n) == fr_le32(d2h + 6 + n));
	memcpy(pl, d2h + 6, n);
	len = (int)n;
	memmove(d2h, d2h + 6 + n + 4, d2h_n - (6 + n + 4));
	d2h_n -= 6 + n + 4;
	return len;
}

/* --- a slot image with a front, as tools/mkldr.py's merged() makes one --- */

#define FRONT_ENTRY 0x08002000u
#define FRONT_INIT  0x11A04000u
#define MAIN_ADDR   0x08031000u

static size_t hdr(uint8_t *o, uint16_t flags, uint32_t target, uint32_t count, uint32_t arg)
{
	uint8_t x = 0;
	unsigned i;

	fr_put32(o, 0xAD000000u | (flags & 0xFFF0u) | 4u);
	fr_put32(o + 4, target);
	fr_put32(o + 8, count);
	fr_put32(o + 12, arg);
	for (i = 0; i < 16; i++)
		x ^= o[i];
	o[2] = x;	/* HDRCHK: the 16 bytes XOR to zero */
	return 16;
}

static size_t block(uint8_t *o, uint32_t target, const uint8_t *p, uint32_t n)
{
	hdr(o, 0, target, n, 0);
	memcpy(o + 16, p, n);
	return 16 + n;
}

static uint8_t img_a[0x4000], img_b[0x14000];
static uint8_t g_fseed = 1;	/* the front's bytes: the same in every image */

/* pad: zero bytes of main in DDR (a multiple of 4); seed varies main's
 * bytes and the version, g_fseed the front's. */
static size_t make_image(uint8_t *img, uint32_t pad, uint8_t seed, bool front_ok)
{
	static uint8_t fl1[32], fl2[64], main_run[64], desc[FR_DESC_MAX];
	static const uint8_t zero[0x10000];
	struct fr_sha256 s;
	uint32_t fcrc, mcrc, dsize, nreg = pad ? 2u : 1u;
	size_t n = 16, i;

	for (i = 0; i < sizeof fl1; i++)
		fl1[i] = (uint8_t)(g_fseed + i);
	for (i = 0; i < sizeof fl2; i++)
		fl2[i] = (uint8_t)(g_fseed * 3u + i);
	for (i = 0; i < sizeof main_run; i++)
		main_run[i] = (uint8_t)(seed * 5u + i);
	fcrc = fr_crc32(fr_crc32(0, fl1, sizeof fl1), fl2, sizeof fl2);
	mcrc = fr_crc32(fr_crc32(0, main_run, sizeof main_run), zero, pad);

	memset(desc, 0, sizeof desc);
	dsize = FR_DESC_HEAD + 8u * nreg + 4u;
	fr_put32(desc + FR_DESC_O_MAGIC, FR_DESC_MAGIC);
	fr_put16(desc + FR_DESC_O_VERSION, FR_DESC_VERSION);
	fr_put16(desc + FR_DESC_O_SIZE, (uint16_t)dsize);
	fr_put32(desc + FR_DESC_O_MAIN_ENTRY, MAIN_ADDR);
	fr_put32(desc + FR_DESC_O_NREGIONS, nreg);
	fr_put32(desc + FR_DESC_O_MAIN_CRC, mcrc);
	fr_put32(desc + FR_DESC_O_FRONT_CRC, front_ok ? fcrc : fcrc ^ 1u);
	for (i = 0; i < 4; i++)
		fr_put16(desc + FR_DESC_O_BL_VERSION + 2 * i, (uint16_t)(seed + i));
	fr_sha256_init(&s);
	{
		uint8_t ad[8];

		fr_put32(ad, FRONT_INIT);
		fr_put32(ad + 4, sizeof fl1);
		fr_sha256_add(&s, ad, 8);
		fr_sha256_add(&s, fl1, sizeof fl1);
		fr_put32(ad, FRONT_ENTRY);
		fr_put32(ad + 4, sizeof fl2);
		fr_sha256_add(&s, ad, 8);
		fr_sha256_add(&s, fl2, sizeof fl2);
	}
	fr_sha256_end(&s, desc + FR_DESC_O_FRONT_PIN);
	fr_put32(desc + FR_DESC_O_REGIONS, MAIN_ADDR);
	fr_put32(desc + FR_DESC_O_REGIONS + 4, sizeof main_run);
	if (pad) {
		fr_put32(desc + FR_DESC_O_REGIONS + 8, 0x88000000u);
		fr_put32(desc + FR_DESC_O_REGIONS + 12, pad);
	}
	fr_put32(desc + dsize - 4, fr_crc32(0, desc, dsize - 4));

	/* L1 first, INIT, then the rest by address (mkldr.merged()). */
	n += block(img + n, FRONT_INIT, fl1, sizeof fl1);
	n += hdr(img + n, 0x0800u, FRONT_INIT, 0, 0);
	n += block(img + n, FRONT_ENTRY, fl2, sizeof fl2);
	n += block(img + n, FR_DESC_ADDR, desc, dsize);
	n += block(img + n, MAIN_ADDR, main_run, sizeof main_run);
	if (pad)
		n += block(img + n, 0x88000000u, zero, pad);
	n += hdr(img + n, 0x8000u, FRONT_ENTRY, 0, 0);
	hdr(img, 0x5000u, FRONT_ENTRY, 0, (uint32_t)(n - 16));
	return n;
}

static void sha(const uint8_t *p, size_t n, uint8_t out[FR_SHA256_LEN])
{
	struct fr_sha256 s;

	fr_sha256_init(&s);
	fr_sha256_add(&s, p, n);
	fr_sha256_end(&s, out);
}

/* --- the unit's start-up state --- */

static struct fr_rec_status st;
static struct fr_proto proto;
static uint8_t staging[FR_SLOT_CAP];
static size_t len_a;
static uint8_t seq;
/* The clock recovery's loop gives fr_proto_clock() after each poll: g_tick
 * a poll. */
static uint64_t g_now;
static uint64_t g_tick = 1000000u;

static void put_slot(unsigned k, const uint8_t *img, size_t n)
{
	uint8_t rec[12];

	memset(flash + FR_SLOT_BASE(k), 0xFF, FR_SLOT_CAP);
	memcpy(flash + FR_SLOT_BASE(k), img, n);
	fr_put32(rec, fr_crc32(0, img, n));
	fr_put32(rec + 4, (uint32_t)n);
	fr_put32(rec + 8, FR_TAG);
	memset(flash + FR_RECORD_OFF(k), 0xFF, FR_SECTOR);
	memcpy(flash + FR_RECORD_OFF(k), rec, sizeof rec);
}

/* The battery hook (S-02.084): counts its calls and leaves a report. */
static const uint8_t batt7[7] = { 0x02, 0x50, 0x19, 0x01, 0x2C, 0x0F, 0xA0 };
static unsigned battery_calls;

static void battery(void *ctx)
{
	(void)ctx;
	battery_calls++;
	memcpy(st.batt, batt7, sizeof batt7);
	st.batt_len = sizeof batt7;
}

/* What recovery's start-up reads, from the flash as it is now. */
static void survey(void)
{
	unsigned k;

	st.selector_ok = fr_read_selector(&fl, &st.sel) == FR_FLASH_OK;
	for (k = 0; k < 2; k++) {
		CHECK(fr_slot_info(&fl, k, &st.slot[k]) == FR_FLASH_OK);
		CHECK(fr_slot_kind(&fl, k, &st.slot[k]) == FR_FLASH_OK);
	}
	fr_rec_target(&st);
}

/* Slot 0 holds image A and runs; the selector reads sel; slot 1 is erased
 * (bad); the caller's start-up as recovery's would fill it. */
static void start(uint8_t index, uint8_t verified, bool established)
{
	memset(flash, 0xFF, sizeof flash);
	len_a = make_image(img_a, 0x400, 1, true);
	put_slot(0, img_a, len_a);
	flash[FR_SELECTOR_OFF] = index;
	flash[FR_SELECTOR_OFF + 1] = verified;
	fail_program_at = UINT32_MAX;
	fail_erase_at = UINT32_MAX;
	erase_then_fail = false;
	sel_prog_lazy = false;
	ver_prog_lazy = false;
	fail_read_once_at = fail_read_always_at = UINT32_MAX;
	fail_read_skip = 0;
	page_faults = 0;
	n_fops = n_erase = 0;
	h2d_n = h2d_at = d2h_n = 0;
	write_limit = SIZE_MAX;
	battery_calls = 0;

	memset(&st, 0, sizeof st);
	st.why = FR_WHY_KEYS;
	st.running.established = established;
	st.running.slot = 0;
	survey();
	st.own_desc = st.slot[0].has_desc;
	st.desc = st.slot[0].desc;
	fr_proto_init(&proto, &port, &st, &fl, staging);
	proto.battery = battery;
	proto.battery_ctx = NULL;
	g_now = 1000u;
	g_tick = 1000000u;
	fr_proto_clock(&proto, g_now);
}

/* Slot 1 good too, holding image C (the same front, another main part). */
static uint8_t img_c[0x4000];
static size_t len_c;

static void start_b_good(uint8_t index, uint8_t verified)
{
	start(index, verified, true);
	len_c = make_image(img_c, 0x200, 7, true);
	put_slot(1, img_c, len_c);
	survey();
}

static void poll1(void)
{
	room = write_limit;
	fr_proto_poll(&proto);
	g_now += g_tick;
	fr_proto_clock(&proto, g_now);
}

static void poll_n(unsigned n)
{
	while (n--)
		poll1();
}

/* A write or an action accepted: counted down to, or under way. */
static bool busy_ph(uint8_t ph)
{
	return ph == FR_PH_WRITING || ph == FR_PH_COUNTDOWN;
}

/* Polled until the countdown is over (S-02.084): the phase is writing,
 * and the first step comes in the next poll. */
static void through_countdown(void)
{
	for (unsigned i = 0; i < 100u && proto.phase == FR_PH_COUNTDOWN; i++)
		poll1();
	CHECK(proto.phase != FR_PH_COUNTDOWN);
}

/* One request and its reply's payload length (-1: none). */
static int ask(uint8_t type, const uint8_t *pl, size_t n, uint8_t *reply)
{
	seq++;
	send(type, seq, pl, n);
	poll_n(4);
	return take(type, seq, reply);
}

static uint8_t status_of(uint8_t type, const uint8_t *pl, size_t n)
{
	uint8_t r[300] = { 0 };

	return ask(type, pl, n, r) >= 1 ? r[0] : 0xEEu;
}

static uint8_t begin(uint32_t len, const uint8_t *image_sha)
{
	uint8_t pl[4 + FR_SHA256_LEN];

	fr_put32(pl, len);
	memcpy(pl + 4, image_sha, FR_SHA256_LEN);
	return status_of(FR_T_BEGIN, pl, sizeof pl);
}

static uint8_t data(uint32_t off, const uint8_t *p, size_t n, uint32_t *next)
{
	uint8_t pl[4 + FR_PROTO_MAX_DATA], r[16] = { 0 };
	int k;

	fr_put32(pl, off);
	memcpy(pl + 4, p, n);
	k = ask(FR_T_DATA, pl, 4 + n, r);
	CHECK(k == (int)FR_DATA_REPLY_LEN);
	if (k != (int)FR_DATA_REPLY_LEN)
		return 0xEEu;
	*next = fr_le32(r + 1);
	return r[0];
}

static uint8_t end_rc(uint8_t *image_rc)
{
	uint8_t r[16] = { 0 };
	int k = ask(FR_T_END, NULL, 0, r);

	CHECK(k == (int)FR_END_REPLY_LEN);
	*image_rc = r[1];
	return k >= 1 ? r[0] : 0xEEu;
}

static void info(uint8_t *r)
{
	CHECK(ask(FR_T_INFO, NULL, 0, r) == (int)FR_INFO_LEN);
}

static bool send_all(const uint8_t *img, size_t n)
{
	uint32_t off = 0, next = 0;
	bool ok = true;

	while (off < n) {
		size_t k = n - off < 1024 ? n - off : 1024;

		ok &= data(off, img + off, k, &next) == FR_WR_OK;
		ok &= next == off + k;
		off = next;
		if (!ok)
			break;
	}
	return ok;
}

/* --- the tests --- */

static void test_hello(void)
{
	uint8_t r[300] = { 0 };

	start(0, 1, true);
	CHECK(ask(FR_T_HELLO, NULL, 0, r) == (int)FR_HELLO_LEN);
	CHECK(r[FR_HELLO_O_PROTO] == FR_PROTO_VERSION);
	CHECK(fr_le16(r + FR_HELLO_O_FRONT) == FR_FRONT_VERSION && FR_FRONT_VERSION == 23u);
	CHECK(r[FR_HELLO_O_PROTO] == 2u);
	CHECK(fr_le16(r + FR_HELLO_O_MAX_DATA) == FR_PROTO_MAX_DATA);
	CHECK(r[FR_HELLO_O_WHY] == FR_WHY_KEYS);
	CHECK(r[FR_HELLO_O_FLAGS] == (FR_INFO_F_ESTABLISHED | FR_INFO_F_SELECTOR | FR_INFO_F_OWN_DESC));
	CHECK(fr_le16(r + FR_HELLO_O_VERSION) == 1u && fr_le16(r + FR_HELLO_O_VERSION + 6) == 4u);
	CHECK(memcmp(r + FR_HELLO_O_PIN, st.desc.pin, FR_SHA256_LEN) == 0);
}

static void test_write(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;
	size_t n;
	unsigned i, last = 0;
	bool monotonic = true;

	start(0, 1, true);
	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);

	info(r);
	CHECK(r[FR_INFO_O_TARGET] == 1u && r[FR_INFO_O_REFUSAL] == FR_WR_OK);
	CHECK(r[FR_INFO_O_RUNNING] == 0u && r[FR_INFO_O_SEL_INDEX] == 0u &&
	      r[FR_INFO_O_SEL_VERIFIED] == 1u);
	CHECK(r[FR_INFO_O_SLOT(0) + FR_INFO_S_FLAGS] ==
	      (FR_SLOT_F_GOOD | FR_SLOT_F_HAS_DESC | FR_SLOT_F_HAS_FRONT));
	CHECK(fr_le32(r + FR_INFO_O_SLOT(0) + FR_INFO_S_LENGTH) == len_a);
	CHECK(fr_le32(r + FR_INFO_O_SLOT(0) + FR_INFO_S_CRC) == fr_crc32(0, img_a, len_a));
	CHECK(fr_le16(r + FR_INFO_O_SLOT(0) + FR_INFO_S_VERSION) == 1u);
	CHECK(r[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] == 0u);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_IDLE);

	CHECK(begin((uint32_t)n, h) == FR_WR_OK);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_RECEIVING && fr_le32(r + FR_INFO_O_LEN) == n);
	CHECK(send_all(img_b, n));
	info(r);
	CHECK(fr_le32(r + FR_INFO_O_GOT) == n);
	CHECK(end_rc(&rc_img) == FR_WR_OK && rc_img == FR_IMG_OK);

	/* bounded: the writer's steps for a slot this size, with room */
	for (i = 0; i < 10000; i++) {
		info(r);
		if (!busy_ph(r[FR_INFO_O_PHASE]))
			break;
		monotonic &= fr_le16(r + FR_INFO_O_PROGRESS) >= last;
		last = fr_le16(r + FR_INFO_O_PROGRESS);
		CHECK(begin((uint32_t)n, h) == FR_WR_BUSY);
	}
	CHECK(monotonic);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE);
	CHECK(r[FR_INFO_O_RESULT] == FR_WR_OK);
	CHECK(fr_le16(r + FR_INFO_O_PROGRESS) == 1000u);
	CHECK(r[FR_INFO_O_WRITTEN] == 1u);
	CHECK(memcmp(r + FR_INFO_O_READBACK, h, FR_SHA256_LEN) == 0);
	/* the slot as INFO now reads it: the record of this image */
	CHECK(r[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] ==
	      (FR_SLOT_F_GOOD | FR_SLOT_F_HAS_DESC | FR_SLOT_F_HAS_FRONT));
	CHECK(fr_le32(r + FR_INFO_O_SLOT(1) + FR_INFO_S_LENGTH) == n);
	CHECK(fr_le32(r + FR_INFO_O_SLOT(1) + FR_INFO_S_CRC) == fr_crc32(0, img_b, n));
	CHECK(fr_le16(r + FR_INFO_O_SLOT(1) + FR_INFO_S_VERSION) == 9u);
	/* the flash: the slot, its record, the selector {1, 00} (S-02.048) */
	CHECK(memcmp(flash + FR_SLOT_BASE(1), img_b, n) == 0);
	CHECK(fr_le32(flash + FR_RECORD_OFF(1)) == fr_crc32(0, img_b, n));
	CHECK(fr_le32(flash + FR_RECORD_OFF(1) + 4) == n);
	CHECK(fr_le32(flash + FR_RECORD_OFF(1) + 8) == FR_TAG);
	CHECK(flash[FR_SELECTOR_OFF] == 1u && flash[FR_SELECTOR_OFF + 1] == 0u);
	CHECK(memcmp(flash + FR_SLOT_BASE(0), img_a, len_a) == 0);
	CHECK(page_faults == 0);
	/* The selector names the slot just written: a second write is
	 * refused until a reset. */
	CHECK(r[FR_INFO_O_TARGET] == FR_NO_SLOT && r[FR_INFO_O_REFUSAL] == FR_WR_NOT_SELECTED);
	CHECK(begin((uint32_t)n, h) == FR_WR_NOT_SELECTED);
}

static void refused_start(uint8_t index, uint8_t verified, bool established, bool bad0,
			  uint8_t want)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN];

	start(index, verified, established);
	if (bad0) {
		flash[FR_SLOT_BASE(0) + 100] ^= 1u;
		CHECK(fr_slot_info(&fl, 0, &st.slot[0]) == FR_FLASH_OK);
		fr_rec_target(&st);
	}
	info(r);
	CHECK(r[FR_INFO_O_TARGET] == FR_NO_SLOT);
	CHECK(r[FR_INFO_O_REFUSAL] == want);
	sha(img_a, len_a, h);
	CHECK(begin((uint32_t)len_a, h) == want);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == want);
}

static void test_refusals(void)
{
	uint8_t h[FR_SHA256_LEN], rc_img, bad[256];
	uint32_t next;
	size_t n;

	refused_start(0, 1, false, false, FR_WR_NOT_RUNNING);
	refused_start(1, 1, true, false, FR_WR_NOT_SELECTED);
	refused_start(0xFF, 1, true, false, FR_WR_NOT_SELECTED);
	refused_start(0, 0, true, false, FR_WR_UNVERIFIED);
	refused_start(0, 0xFF, true, false, FR_WR_UNVERIFIED);
	refused_start(0, 1, true, true, FR_WR_SELECTED_BAD);

	/* sizes (S-02.044) */
	start(0, 1, true);
	memset(h, 0, sizeof h);
	CHECK(begin(0, h) == FR_WR_SIZE);
	CHECK(begin(FR_SLOT_CAP + 1u, h) == FR_WR_SIZE);
	CHECK(status_of(FR_T_BEGIN, h, 8) == FR_ST_BAD_REQUEST);

	/* not a stream (S-02.091) */
	memset(bad, 0x5A, sizeof bad);
	sha(bad, sizeof bad, h);
	CHECK(begin(sizeof bad, h) == FR_WR_OK);
	CHECK(send_all(bad, sizeof bad));
	CHECK(end_rc(&rc_img) == FR_WR_IMAGE && rc_img == FR_IMG_STREAM);

	/* a stream without a front */
	n = make_image(img_b, 0x100, 3, false);
	sha(img_b, n, h);
	CHECK(begin((uint32_t)n, h) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_IMAGE && rc_img == FR_IMG_NO_FRONT);

	/* not the image announced */
	n = make_image(img_b, 0x100, 3, true);
	sha(img_b, n, h);
	h[0] ^= 1u;
	CHECK(begin((uint32_t)n, h) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_HASH);

	/* the order of the requests */
	CHECK(end_rc(&rc_img) == FR_ST_STATE);
	CHECK(data(0, img_b, 16, &next) == FR_ST_STATE);
	sha(img_b, n, h);
	CHECK(begin((uint32_t)n, h) == FR_WR_OK);
	CHECK(data(0, img_b, 16, &next) == FR_WR_OK && next == 16u);
	CHECK(end_rc(&rc_img) == FR_ST_LENGTH);
	CHECK(data(16, img_b + 16, n, &next) == FR_ST_LENGTH && next == 16u);
	CHECK(status_of(0x7Fu, NULL, 0) == FR_ST_BAD_REQUEST);

	/* S-02.084: a front other than the running one, refused at END with
	 * nothing written */
	g_fseed = 2;
	n = make_image(img_b, 0x100, 3, true);
	g_fseed = 1;
	sha(img_b, n, h);
	CHECK(begin((uint32_t)n, h) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_FRONT && rc_img == FR_IMG_OK);
	{
		uint8_t r[300] = { 0 };

		info(r);
		CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_FRONT);
		CHECK(r[FR_INFO_O_TARGET] == 1u);	/* another image may still go */
	}
	CHECK(memcmp(flash + FR_SLOT_BASE(1), "\xFF\xFF\xFF\xFF", 4) == 0);
	CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 1u);
	n = make_image(img_b, 0x100, 3, true);
	sha(img_b, n, h);

	/* The writer reads the start condition again: the selector changed
	 * under the protocol's feet is still refused. */
	CHECK(begin((uint32_t)n, h) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	f_erase(NULL, FR_SELECTOR_OFF);
	flash[FR_SELECTOR_OFF] = 0;
	flash[FR_SELECTOR_OFF + 1] = 0;
	CHECK(end_rc(&rc_img) == FR_WR_UNVERIFIED);
	CHECK(memcmp(flash + FR_SLOT_BASE(1), "\xFF\xFF\xFF\xFF", 4) == 0);
}

static void test_flash_failure(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;
	size_t n;
	unsigned i;

	start(0, 1, true);
	n = make_image(img_b, 0x10000, 4, true);
	sha(img_b, n, h);
	fail_program_at = FR_SLOT_BASE(1) + 0xC100u;	/* sector 12 of 17 */
	CHECK(begin((uint32_t)n, h) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_BUSY);
	for (i = 0; i < 10000; i++) {
		info(r);
		if (!busy_ph(r[FR_INFO_O_PHASE]))
			break;
	}
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_FLASH);
	CHECK(r[FR_INFO_O_WRITTEN] == FR_NO_SLOT);
	/* the selector untouched: the unit starts as before, and a retry may go */
	CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 1u);
	CHECK(r[FR_INFO_O_TARGET] == 1u);
	CHECK(!proto.restart);
}

static void test_data_order(void)
{
	uint8_t h[FR_SHA256_LEN], other[64];
	uint32_t next = 0;
	size_t n;

	start(0, 1, true);
	n = make_image(img_b, 0x800, 5, true);
	sha(img_b, n, h);
	CHECK(begin((uint32_t)n, h) == FR_WR_OK);
	CHECK(data(0, img_b, 1024, &next) == FR_WR_OK && next == 1024u);
	/* ahead of the next offset */
	CHECK(data(2048, img_b + 2048, 1024, &next) == FR_ST_OFFSET && next == 1024u);
	/* a host retry of a chunk already taken */
	CHECK(data(0, img_b, 1024, &next) == FR_WR_OK && next == 1024u);
	CHECK(data(512, img_b + 512, 256, &next) == FR_WR_OK && next == 1024u);
	/* an earlier offset with other bytes */
	memset(other, 0x33, sizeof other);
	CHECK(data(0, other, sizeof other, &next) == FR_ST_OFFSET && next == 1024u);
	/* straddling the next offset */
	CHECK(data(1000, img_b + 1000, 100, &next) == FR_ST_OFFSET && next == 1024u);
	CHECK(data(1024, img_b + 1024, 1024, &next) == FR_WR_OK && next == 2048u);
	CHECK(status_of(FR_T_DATA, img_b, 4) == FR_ST_BAD_REQUEST);
	/* S-02.093: at most 1024 image bytes a DATA, in a payload of at most
	 * 1032: 1024 taken, 1025 and 1028 malformed, nothing taken */
	{
		uint8_t pl[FR_PROTO_MAX], r[16] = { 0 };

		CHECK(begin((uint32_t)n, h) == FR_WR_OK);
		fr_put32(pl, 0);
		memcpy(pl + 4, img_b, FR_PROTO_MAX - 4u);
		CHECK(ask(FR_T_DATA, pl, 4u + 1025u, r) == (int)FR_DATA_REPLY_LEN);
		CHECK(r[0] == FR_ST_BAD_REQUEST && fr_le32(r + 1) == 0u);
		CHECK(ask(FR_T_DATA, pl, FR_PROTO_MAX, r) == (int)FR_DATA_REPLY_LEN);
		CHECK(r[0] == FR_ST_BAD_REQUEST && fr_le32(r + 1) == 0u);
		CHECK(ask(FR_T_DATA, pl, 4u + 1024u, r) == (int)FR_DATA_REPLY_LEN);
		CHECK(r[0] == FR_WR_OK && fr_le32(r + 1) == 1024u);
		CHECK(proto.got == 1024u);
	}
	/* A new BEGIN starts over. */
	CHECK(begin((uint32_t)n, h) == FR_WR_OK);
	CHECK(data(1024, img_b + 1024, 1024, &next) == FR_ST_OFFSET && next == 0u);
}

static void test_damaged_frames(void)
{
	uint8_t f[64], r[300] = { 0 }, big[6] = { 'W', 'R', FR_T_INFO, 1, 0xFF, 0xFF };
	size_t n;

	start(0, 1, true);
	/* junk, a frame with a bad CRC, a header claiming too much, then INFO */
	raw_send((const uint8_t *)"xxWWRjunk", 9);
	n = frame(f, FR_T_HELLO, 40, NULL, 0);
	f[n - 1] ^= 0x10u;
	raw_send(f, n);
	raw_send(big, sizeof big);
	send(FR_T_INFO, 41, NULL, 0);
	poll_n(6);
	CHECK(take(FR_T_INFO, 41, r) == (int)FR_INFO_LEN);
	CHECK(d2h_n == 0);
	/* a frame split across polls */
	n = frame(f, FR_T_HELLO, 42, NULL, 0);
	raw_send(f, 3);
	poll_n(2);
	CHECK(d2h_n == 0);
	raw_send(f + 3, n - 3);
	poll_n(2);
	CHECK(take(FR_T_HELLO, 42, r) == (int)FR_HELLO_LEN);
	/* a reply-typed frame is not answered */
	send(FR_T_INFO | FR_T_REPLY, 43, NULL, 0);
	poll_n(2);
	CHECK(d2h_n == 0);
}

static void test_slow_port(void)
{
	uint8_t r[300] = { 0 };
	unsigned i;

	start(0, 1, true);
	write_limit = 7;
	send(FR_T_INFO, 50, NULL, 0);
	send(FR_T_HELLO, 51, NULL, 0);
	/* one request at a time: the HELLO waits for INFO's reply to go out */
	for (i = 0; i < 100 && d2h_n < FR_PROTO_HEAD + FR_INFO_LEN + 4u; i++)
		poll1();
	CHECK(i > 20u);			/* it took many polls */
	CHECK(take(FR_T_INFO, 50, r) == (int)FR_INFO_LEN);
	poll_n(20);
	CHECK(take(FR_T_HELLO, 51, r) == (int)FR_HELLO_LEN);
}

static void test_restart(void)
{
	uint8_t r[16] = { 0 };

	start(0, 1, true);
	write_limit = 3;
	send(FR_T_RESTART, 60, NULL, 0);
	poll1();
	CHECK(!proto.restart);		/* its reply is not out yet */
	poll_n(5);
	CHECK(proto.restart);
	CHECK(take(FR_T_RESTART, 60, r) == 1 && r[0] == FR_WR_OK);
}

static void test_target(void)
{
	struct fr_rec_status s;

	memset(&s, 0, sizeof s);
	s.running.established = true;
	s.running.slot = 1;
	s.selector_ok = true;
	s.sel.index = 0x07;	/* S-02.011: slot 1 */
	s.sel.verified = 1;
	s.slot[1].good = true;
	s.slot[1].has_desc = true;
	s.slot[1].has_front = true;
	fr_rec_target(&s);
	CHECK(s.target == 0u && s.refusal == FR_WR_OK);
	/* slot 0 bad: in place, but only once a write of it has failed */
	CHECK(s.run_way == FR_WAY_NONE && s.run_refusal == FR_WR_TRY_OTHER);
	s.home.other_failed = true;
	fr_rec_target(&s);
	CHECK(s.run_way == FR_WAY_IN_PLACE && s.run_refusal == FR_WR_OK);
	s.selector_ok = false;
	fr_rec_target(&s);
	CHECK(s.target == FR_NO_SLOT && s.refusal == FR_WR_READ);
	CHECK(s.run_way == FR_WAY_NONE && s.run_refusal == FR_WR_READ);
}

/*
 * S-02.084's rules over every state recovery's start-up can find, written
 * from the text: A the running slot, B the other. Refused unless A is
 * established and selected, or B is selected because this recovery moved
 * the selector there; verified 01; A's front intact in the flash, or a write
 * of A begun in this recovery; and where B is selected, B good (status 5).
 * Then the default goes to the slot the selector does not name, and a
 * write of A goes with B selected first when B is good and in place when it
 * is bad; with B selected by this recovery, either request writes A as a
 * plain write. *pinned is false where the text names no status: A good
 * and its front not intact.
 */
/*
 * S-02.084: which slot a start reaches, for the factory first stage with
 * its own selector write cut short in any way: the named slot when good
 * with verified 01; slot 1 named and good with any verified; slot 0 named
 * and good with verified not 01 only while slot 1 is good too; slot 1 by
 * fallback when slot 0 is named, bad by a record sure to fail (another
 * tag, or the tag with a length of 1 to 0x78000), verified has a bit
 * besides bit 0, and slot 1 is good. Else none. rec is the named slot's.
 */
static bool spec_sure_to_fail(const struct fr_record *r)
{
	return r->tag != FR_TAG || (r->length >= 1u && r->length <= 0x78000u);
}

static unsigned spec_reach(uint8_t index, uint8_t verified, const bool good[2],
			   const struct fr_record *rec)
{
	const unsigned n = index == 0u ? 0u : 1u;

	if (good[n] && verified == 0x01u)
		return FR_REACH_NAMED;
	if (good[n] && n == 1u)
		return FR_REACH_NAMED;
	if (good[n] && n == 0u && good[1])
		return FR_REACH_NAMED;
	if (!good[n] && n == 0u && spec_sure_to_fail(rec) && (verified & 0xFEu) != 0u && good[1])
		return FR_REACH_FALLBACK;
	return FR_REACH_NONE;
}

static void spec_target(const struct fr_rec_status *s, uint8_t *target, uint8_t *refusal,
			uint8_t *run_way, uint8_t *run_refusal, bool *pinned)
{
	unsigned a, sel;

	*run_refusal = 0xEEu;	/* as the default's, unless set below */

	*target = FR_NO_SLOT;
	*run_way = FR_WAY_NONE;
	*pinned = true;
	if (!s->running.established || s->running.slot > 1u) {
		*refusal = FR_WR_NOT_RUNNING;
		return;
	}
	if (!s->selector_ok) {
		*refusal = FR_WR_READ;		/* status 17 */
		return;
	}
	a = s->running.slot;
	sel = s->sel.index == 0u ? 0u : 1u;
	/* S-02.084's damaged state: a write of A began in this recovery and
	 * A is bad; or A was written and read back good, the selector not
	 * settled after it, and B is bad. The selector's conditions are
	 * waived. */
	if (s->home.spoiled &&
	    (!s->slot[a].good || (s->home.unsettled && !s->slot[1u - a].good))) {
		const unsigned b = 1u - a;
		const uint8_t v = s->sel.verified;
		/* B good and the selector leading to it: it names B, or names A
		 * with verified exactly 00 and A's record sure to fail */
		const bool leads = sel == b ||
				   (v == 0x00u && spec_sure_to_fail(&s->slot[a].rec));
		/* the exception: B is slot 0, named, verified odd and not 01 */
		const bool by_programs = sel == b && b == 0u && (v & 0x01u) != 0u && v != 0x01u;

		*refusal = FR_WR_OK;
		*target = (uint8_t)b;
		if (s->slot[a].good) {
			/* A good, B bad: the flag's own image settles on A;
			 * without it B */
			*run_way = FR_WAY_PLAIN;
		} else if (!s->slot[b].good) {
			*run_way = FR_WAY_IN_PLACE;
		} else if (leads) {
			/* either request writes A with no selector write first,
			 * but B's own image settled by programs alone */
			if (!by_programs)
				*target = (uint8_t)a;
			*run_way = FR_WAY_PLAIN;
		} else {
			*run_way = FR_WAY_SELECT_FIRST;
		}
		return;
	}
	if (sel != a && !s->home.moved) {
		*refusal = FR_WR_NOT_SELECTED;
		return;
	}
	if (s->sel.verified != 0x01u) {
		*refusal = FR_WR_UNVERIFIED;
		return;
	}
	if (!s->slot[a].has_front && !s->home.spoiled) {
		*refusal = FR_WR_SELECTED_BAD;
		*pinned = !s->slot[a].good;
		return;
	}
	if (sel != a) {
		if (!s->slot[sel].good) {
			*refusal = FR_WR_SELECTED_BAD;
			return;
		}
		*refusal = FR_WR_OK;
		*target = (uint8_t)a;
		*run_way = FR_WAY_PLAIN;
		return;
	}
	*refusal = FR_WR_OK;
	*target = (uint8_t)(1u - a);
	if (s->slot[1u - a].good)
		*run_way = FR_WAY_SELECT_FIRST;
	else if (s->home.other_failed)
		*run_way = FR_WAY_IN_PLACE;
	else
		*run_refusal = FR_WR_TRY_OTHER;	/* in place before a failed write of B */
}

static void test_target_all(void)
{
	static const uint8_t index[] = { 0x00, 0x01, 0x02, 0xFF };
	static const uint8_t verified[] = { 0x00, 0x01, 0x02, 0xFF };
	unsigned n = 0;

	for (unsigned c = 0; c < 2u * 3u * 2u * 4u * 4u * 4u * 4u * 4u * 2u * 2u; c++) {
		struct fr_rec_status s;
		uint8_t target, refusal, way, run_refusal;
		bool pinned;
		unsigned x = c;

		memset(&s, 0, sizeof s);
		s.selector_ok = (x % 2u) != 0u;
		x /= 2u;
		s.running.established = x % 3u != 0u;
		s.running.slot = (uint8_t)(x % 3u == 2u ? 1u : 0u);
		x /= 3u;
		if (x % 2u)
			s.running.slot = 2u;		/* not a slot */
		x /= 2u;
		s.sel.index = index[x % 4u];
		x /= 4u;
		s.sel.verified = verified[x % 4u];
		x /= 4u;
		for (unsigned k = 0; k < 2u; k++) {
			/* good, and a front intact, each or both */
			s.slot[k].good = (x & 1u) != 0u;
			s.slot[k].has_front = (x & 2u) != 0u;
			s.slot[k].has_desc = s.slot[k].has_front;
			x /= 4u;
		}
		s.home.moved = (x & 1u) != 0u;
		s.home.spoiled = (x & 2u) != 0u;
		s.home.other_failed = (x & 4u) != 0u;
		s.home.unsettled = (x & 8u) != 0u && s.home.spoiled;
		spec_target(&s, &target, &refusal, &way, &run_refusal, &pinned);
		if (run_refusal == 0xEEu)
			run_refusal = refusal;
		fr_rec_target(&s);
		/* the default request */
		CHECK(s.target == target);
		if (pinned)
			CHECK(s.refusal == refusal);
		else
			CHECK(s.refusal != FR_WR_OK);
		/* the running slot's request: refused alike, or its way */
		CHECK(s.run_way == way);
		if (pinned)
			CHECK(s.run_refusal == run_refusal);
		else
			CHECK(s.run_refusal != FR_WR_OK);
		n++;
	}
	CHECK(n > 3000u);
}

/* --- BEGIN's flags byte (S-02.093) --- */

static uint8_t begin_n(uint32_t len, const uint8_t *image_sha, size_t n, uint8_t flags)
{
	uint8_t pl[4 + FR_SHA256_LEN + 2];

	memset(pl, 0, sizeof pl);
	fr_put32(pl, len);
	memcpy(pl + 4, image_sha, FR_SHA256_LEN);
	pl[4 + FR_SHA256_LEN] = flags;
	return status_of(FR_T_BEGIN, pl, n);
}

static uint8_t begin_f(uint32_t len, const uint8_t *image_sha, uint8_t flags)
{
	return begin_n(len, image_sha, 4 + FR_SHA256_LEN + 1, flags);
}

/* --- S-02.084: a plain write of slot 1 that fails (status 9), after which
 * an in-place write may go --- */

static uint8_t img_f[0x1000];

static void run_to_end(uint8_t *r);

static void fail_other(void)
{
	uint8_t h[FR_SHA256_LEN], r[300] = { 0 }, rc_img;
	const unsigned calls = battery_calls;
	size_t n = make_image(img_f, 0x100, 11, true);

	sha(img_f, n, h);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	CHECK(send_all(img_f, n));
	fail_erase_at = FR_SLOT_BASE(1);
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_FLASH);
	CHECK(st.home.other_failed);
	fail_erase_at = UINT32_MAX;
	n_fops = n_erase = 0;
	CHECK(battery_calls == calls);
}

static void test_begin_flags(void)
{
	static const uint8_t other[] = {
		0x03, 0x05, 0x06, 0x07, 0x08, 0x10, 0x20, 0x40, 0x83, 0x85, 0x86, 0x87, 0x88, 0x90,
		0xA0, 0xC0, 0xFE, 0xFF,
	};
	uint8_t h[FR_SHA256_LEN], r[300];
	size_t n;

	start(0, 1, true);
	n = make_image(img_b, 0x100, 3, true);
	sha(img_b, n, h);
	/* 36 bytes: no flags; 37 with 00: the same */
	CHECK(begin_n((uint32_t)n, h, 36, 0) == FR_WR_OK);
	CHECK(proto.flags == 0u && proto.phase == FR_PH_RECEIVING && proto.len == n);
	CHECK(begin_n((uint32_t)n, h, 37, 0) == FR_WR_OK);
	CHECK(proto.flags == 0u);
	CHECK(battery_calls == 0u);
	/* 37 with 01: the running slot (here in place, slot 1 is bad, once a
	 * write of it has failed) */
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_TRY_OTHER);
	CHECK(battery_calls == 0u);
	fail_other();
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(proto.flags == FR_BEGIN_F_RUNNING);
	CHECK(battery_calls == 1u);
	CHECK(data(0, img_b, 16, &(uint32_t){ 0 }) == FR_WR_OK);
	/* any other bit, and any other length: malformed, nothing changed */
	for (unsigned i = 0; i < ARRAY_LEN(other) + 3u; i++) {
		const uint8_t phase = proto.phase, flags = proto.flags, result = proto.result;
		const uint32_t len = proto.len, got = proto.got, changes = proto.changes;
		uint8_t rc;

		if (i < ARRAY_LEN(other))
			rc = begin_f((uint32_t)n, h, other[i]);
		else
			rc = begin_n((uint32_t)n, h, i == ARRAY_LEN(other) ? 35u :
					 i == ARRAY_LEN(other) + 1u ? 38u : 4u, 0);
		CHECK(rc == FR_ST_BAD_REQUEST);
		CHECK(proto.phase == phase && proto.flags == flags && proto.result == result);
		CHECK(proto.len == len && proto.got == got && proto.changes == changes);
		CHECK(battery_calls == 1u);
	}
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_RECEIVING && fr_le32(r + FR_INFO_O_GOT) == 16u);
	CHECK(n_fops == 0u);
}

/* --- The battery request before an in-place write (S-02.084) --- */

static void test_battery(void)
{
	uint8_t h[FR_SHA256_LEN], r[300];
	size_t n;

	/* in place: once per accepted BEGIN, and INFO carries the report */
	start(0, 1, true);
	n = make_image(img_b, 0x100, 3, true);
	sha(img_b, n, h);
	info(r);
	CHECK(r[FR_INFO_O_BATT_LEN] == 0u);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_NONE && r[FR_INFO_O_RUN_REFUSAL] == FR_WR_TRY_OTHER);
	/* refused 15 before a write of slot 1 has failed: no request */
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_TRY_OTHER);
	CHECK(battery_calls == 0u);
	fail_other();
	info(r);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_IN_PLACE);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(battery_calls == 1u);
	info(r);
	CHECK(r[FR_INFO_O_BATT_LEN] == sizeof batt7);
	CHECK(memcmp(r + FR_INFO_O_BATT, batt7, sizeof batt7) == 0);
	for (unsigned i = sizeof batt7; i < FR_BATT_MAX; i++)
		CHECK(r[FR_INFO_O_BATT + i] == 0u);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(battery_calls == 2u);
	/* not for the default write, nor for a malformed BEGIN */
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	CHECK(begin_n((uint32_t)n, h, 36, 0) == FR_WR_OK);
	CHECK(begin_f((uint32_t)n, h, 0x03) == FR_ST_BAD_REQUEST);
	CHECK(battery_calls == 2u);
	/* not for a write of the running slot that selects the other first */
	start_b_good(0, 1);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(battery_calls == 0u);
	/* not for a refused BEGIN */
	start(0, 0, true);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_UNVERIFIED);
	CHECK(battery_calls == 0u);
	start(0, 1, false);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_NOT_RUNNING);
	CHECK(battery_calls == 0u);
	/* refused for its size: no request */
	start(0, 1, true);
	fail_other();
	CHECK(begin_f(0x76001u, h, FR_BEGIN_F_RUNNING) == FR_WR_SIZE);
	CHECK(begin_f(0, h, FR_BEGIN_F_RUNNING) == FR_WR_SIZE);
	CHECK(battery_calls == 0u);
	/* no hook: accepted all the same */
	proto.battery = NULL;
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
}

/* --- A whole write through the protocol, polled to its end --- */

static void run_to_end(uint8_t *r)
{
	for (unsigned i = 0; i < 10000; i++) {
		info(r);
		if (!busy_ph(r[FR_INFO_O_PHASE]))
			break;
	}
}

static unsigned first_fop(char kind, uint32_t lo, uint32_t hi)
{
	for (unsigned i = 0; i < n_fops; i++)
		if (fops[i].kind == kind && fops[i].off >= lo && fops[i].off < hi)
			return i;
	return ~0u;
}

static bool any_fop(uint32_t lo, uint32_t hi)
{
	return first_fop('e', lo, hi) != ~0u || first_fop('p', lo, hi) != ~0u;
}

#define SEL_LO FR_SELECTOR_OFF
#define SEL_HI (FR_SELECTOR_OFF + FR_SECTOR)
#define SLOT_LO(k) FR_SLOT_BASE(k)
#define SLOT_HI(k) (FR_SLOT_BASE(k) + FR_SLOT_CAP)
#define REC_LO(k) FR_RECORD_OFF(k)
#define REC_HI(k) (FR_RECORD_OFF(k) + FR_SECTOR)

/* SC-RECOVERY run 9: the running slot, the other slot good */
static void test_select_first(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;
	size_t n;

	start_b_good(0, 1);
	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);
	info(r);
	CHECK(r[FR_INFO_O_TARGET] == 1u && r[FR_INFO_O_REFUSAL] == FR_WR_OK);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_SELECT_FIRST && r[FR_INFO_O_RUN_REFUSAL] == FR_WR_OK);
	CHECK(r[FR_INFO_O_WAY] == FR_WAY_NONE);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_MOVED) == 0u);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_RESULT] == FR_WR_OK);
	CHECK(r[FR_INFO_O_WRITTEN] == 0u);
	CHECK(r[FR_INFO_O_WAY] == FR_WAY_SELECT_FIRST);
	/* S-02.093: bit 3 only while the selector names the other slot; it
	 * reads {00 00} again, and slot 0 is good, so bit 4 is clear too */
	CHECK((r[FR_INFO_O_FLAGS] & (FR_INFO_F_MOVED | FR_INFO_F_SPOILED)) == 0u);
	CHECK(memcmp(r + FR_INFO_O_READBACK, h, FR_SHA256_LEN) == 0);
	/* the selector erase and {01 01} first, then slot 0, then {00 00} */
	CHECK(n_fops >= 4u && fops[0].kind == 'e' && fops[0].off == FR_SELECTOR_OFF);
	CHECK(fops[1].kind == 'p' && fops[1].off == FR_SELECTOR_OFF);
	CHECK(first_fop('e', SLOT_LO(0), SLOT_HI(0)) == 2u);
	CHECK(fops[n_fops - 1u].kind == 'p' && fops[n_fops - 1u].off == FR_SELECTOR_OFF);
	CHECK(!any_fop(SLOT_LO(1), SLOT_HI(1)) && !any_fop(REC_LO(1), REC_HI(1)));
	CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 0u);
	CHECK(memcmp(flash + FR_SLOT_BASE(0), img_b, n) == 0);
	CHECK(memcmp(flash + FR_SLOT_BASE(1), img_c, len_c) == 0);
	CHECK(page_faults == 0);
	/* verified 00 now: nothing more until a reset */
	CHECK(r[FR_INFO_O_TARGET] == FR_NO_SLOT && r[FR_INFO_O_REFUSAL] == FR_WR_UNVERIFIED);
}

/* SC-RECOVERY run 10: the running slot, the other slot bad: in place */
static void test_in_place(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;
	uint32_t next = 0;
	size_t n;
	int k;

	start(0, 1, true);
	fail_other();
	n = make_image(img_b, 0x10000, 9, true);
	sha(img_b, n, h);
	info(r);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_IN_PLACE);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	CHECK(proto.phase == FR_PH_COUNTDOWN && !fr_proto_in_place(&proto));
	through_countdown();
	CHECK(fr_proto_in_place(&proto));
	/* while it runs: every request but HELLO and INFO answered busy, with
	 * its own reply's length (S-02.093's table) */
	CHECK(proto.phase == FR_PH_WRITING);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_BUSY);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_BUSY);
	{
		uint8_t pl[8] = { 0 };

		k = ask(FR_T_DATA, pl, sizeof pl, r);
		CHECK(k >= 1 && r[0] == FR_WR_BUSY);
		CHECK(k == (int)FR_DATA_REPLY_LEN);
	}
	k = ask(FR_T_END, NULL, 0, r);
	CHECK(k >= 1 && r[0] == FR_WR_BUSY);
	CHECK(k == (int)FR_END_REPLY_LEN);
	CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_BUSY);
	CHECK(!proto.restart && !proto.restart_due);
	CHECK(proto.phase == FR_PH_WRITING);
	CHECK(ask(FR_T_HELLO, NULL, 0, r) == (int)FR_HELLO_LEN);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_WRITING && r[FR_INFO_O_WAY] == FR_WAY_IN_PLACE);
	CHECK(proto.len == n && proto.got == n);	/* nothing restarted */
	run_to_end(r);
	CHECK(!fr_proto_in_place(&proto));
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_RESULT] == FR_WR_OK);
	CHECK(r[FR_INFO_O_WRITTEN] == 0u && r[FR_INFO_O_WAY] == FR_WAY_IN_PLACE);
	CHECK(memcmp(r + FR_INFO_O_READBACK, h, FR_SHA256_LEN) == 0);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_MOVED) == 0u);
	/* no selector erase or program; the selector still reads 00 01 */
	CHECK(!any_fop(SEL_LO, SEL_HI));
	CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 1u);
	CHECK(r[FR_INFO_O_SEL_INDEX] == 0u && r[FR_INFO_O_SEL_VERIFIED] == 1u);
	CHECK(memcmp(flash + FR_SLOT_BASE(0), img_b, n) == 0);
	CHECK(fr_le32(flash + FR_RECORD_OFF(0) + 4) == n);
	CHECK(!any_fop(SLOT_LO(1), SLOT_HI(1)) && !any_fop(REC_LO(1), REC_HI(1)));
	/* after it the requests are served again */
	CHECK(data(0, img_b, 16, &next) == FR_ST_STATE);
	CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_OK);
}

static int begin_reply(uint32_t len, const uint8_t *image_sha, uint8_t flags, uint8_t *r);

static void first_stage(void);

/* Slot 0's record not plausible (S-02.016 items 1 and 2), its stream as
 * written: erased; length 0; one past the slot; the tag with a bit left
 * set; then the two a factory check is unspecified for (S-02.026): the tag
 * alone, length 0xFFFFFFFF, and length 0x80000000. */
#define SPOIL_KINDS   6u
#define SPOIL_ACCEPTS 4u	/* the kinds below it are written over */
static void spoil_record0(unsigned kind)
{
	uint8_t *r = flash + FR_RECORD_OFF(0);

	switch (kind) {
	case 0:
		memset(r, 0xFF, 12);
		break;
	case 1:
		fr_put32(r + 4, 0u);
		break;
	case 2:
		fr_put32(r + 4, FR_SLOT_CAP + 1u);
		break;
	case 3:
		fr_put32(r + 8, FR_TAG | 0x00400000u);
		break;
	case 4:
		memset(r, 0xFF, 8);
		break;
	default:
		fr_put32(r + 4, 0x80000000u);
		break;
	}
}

/*
 * S-02.084: the selector {A, 01} trusts A, which runs, but A's record is
 * erased or torn. A's front is read from its stream within the slot, so
 * the start condition holds: the default write goes to B, and a write of
 * A with B selected first when B is good, or refused (15) when B is bad;
 * a trial is refused, A being bad. Each write runs as announced, and a
 * selector its erase could leave, naming A unchecked, has a first stage
 * check A's record cleanly. A's front damaged as well, verified not 01, or
 * the tag with a length of 2^31 or more (SPEC-Q), still refuses.
 */
static void test_record_gone(void)
{
	for (unsigned kind = 0; kind < SPOIL_ACCEPTS; kind++)
		for (unsigned b_good = 0; b_good < 2u; b_good++)
			for (unsigned req = 0; req < 2u; req++) {
				uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], br[16], rc_img;
				uint8_t rec0[12];
				size_t n;

				if (b_good)
					start_b_good(0, 1);
				else
					start(0, 1, true);
				spoil_record0(kind);
				memcpy(rec0, flash + FR_RECORD_OFF(0), sizeof rec0);
				survey();
				CHECK(!st.slot[0].good && st.slot[0].has_front);
				CHECK(st.target == 1u && st.refusal == FR_WR_OK);
				CHECK(!st.trial);
				if (b_good) {
					CHECK(st.run_way == FR_WAY_SELECT_FIRST);
				} else {
					CHECK(st.run_way == FR_WAY_NONE && st.run_refusal == FR_WR_TRY_OTHER);
				}
				info(r);
				CHECK(r[FR_INFO_O_SLOT(0) + FR_INFO_S_FLAGS] ==
				      (FR_SLOT_F_HAS_DESC | FR_SLOT_F_HAS_FRONT));
				if (req && !b_good) {
					CHECK(begin_f(0x100u, h, FR_BEGIN_F_RUNNING) == FR_WR_TRY_OTHER);
					continue;
				}
				n = make_image(img_b, 0x2400, (uint8_t)(20u + kind), true);
				sha(img_b, n, h);
				CHECK(begin_reply((uint32_t)n, h, req ? FR_BEGIN_F_RUNNING : 0u, br) ==
				      (int)FR_BEGIN_REPLY_LEN);
				CHECK(br[0] == FR_WR_OK && br[2] == (req ? 0u : 1u));
				CHECK(br[1] == (req ? FR_WAY_SELECT_FIRST : FR_WAY_PLAIN));
				CHECK(send_all(img_b, n));
				CHECK(end_rc(&rc_img) == FR_WR_OK);
				run_to_end(r);
				CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_RESULT] == FR_WR_OK);
				CHECK(r[FR_INFO_O_WRITTEN] == (req ? 0u : 1u));
				CHECK(flash[FR_SELECTOR_OFF] == (req ? 0u : 1u) && flash[FR_SELECTOR_OFF + 1] == 0u);
				CHECK(memcmp(flash + FR_SLOT_BASE(req ? 0u : 1u), img_b, n) == 0);
				if (!req) {
					/* A, its record and its stream, untouched */
					CHECK(memcmp(flash + FR_RECORD_OFF(0), rec0, sizeof rec0) == 0);
					CHECK(memcmp(flash + FR_SLOT_BASE(0), img_a, len_a) == 0);
				} else {
					/* B was selected first and kept */
					CHECK(memcmp(flash + FR_SLOT_BASE(1), img_c, len_c) == 0);
				}
				/* the selector erase, torn: A named, checked */
				memcpy(flash + FR_RECORD_OFF(0), rec0, sizeof rec0);
				flash[FR_SELECTOR_OFF] = 0x00u;
				flash[FR_SELECTOR_OFF + 1] = 0xFFu;
				first_stage();
			}

	/* the tag with a length of 2^31 or more: refused as a front not
	 * intact (5), nothing written, the selector left trusting A */
	for (unsigned kind = SPOIL_ACCEPTS; kind < SPOIL_KINDS; kind++)
		for (unsigned b_good = 0; b_good < 2u; b_good++) {
			uint8_t h[FR_SHA256_LEN];

			if (b_good)
				start_b_good(0, 1);
			else
				start(0, 1, true);
			spoil_record0(kind);
			survey();
			CHECK(!st.slot[0].has_front);
			CHECK(st.target == FR_NO_SLOT && st.refusal == FR_WR_SELECTED_BAD);
			CHECK(st.run_way == FR_WAY_NONE);
			memset(h, 0, sizeof h);
			CHECK(begin_f(0x100u, h, 0) == FR_WR_SELECTED_BAD);
			CHECK(begin_f(0x100u, h, FR_BEGIN_F_RUNNING) == FR_WR_SELECTED_BAD);
			CHECK(n_fops == 0u);
		}

	/* a trial: refused, A bad (19) */
	start_b_good(0, 1);
	spoil_record0(0);
	survey();
	{
		uint8_t h[FR_SHA256_LEN], rc_img;
		size_t n;

		g_fseed = 2;
		n = make_image(img_b, 0x400, 30, true);
		g_fseed = 1;
		sha(img_b, n, h);
		CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_TRIAL) == FR_WR_OK);
		CHECK(send_all(img_b, n));
		CHECK(end_rc(&rc_img) == FR_WR_TRIAL_STATE);
		CHECK(n_fops == 0u);
	}
	/* A's front damaged too: refused (5); verified 00: refused (4) */
	for (unsigned kind = 0; kind < SPOIL_KINDS; kind++) {
		start(0, 1, true);
		spoil_record0(kind);
		flash[FR_SLOT_BASE(0) + 32u] ^= 0x01u;
		survey();
		CHECK(!st.slot[0].has_front);
		CHECK(st.target == FR_NO_SLOT && st.refusal == FR_WR_SELECTED_BAD);
		start(0, 0, true);
		spoil_record0(kind);
		survey();
		CHECK(st.target == FR_NO_SLOT && st.refusal == FR_WR_UNVERIFIED);
	}
}

/* S-02.093: a transfer that leads to no write leaves the phase, status,
 * slot written, read-back and way of the write this recovery made: a
 * refused BEGIN after a plain write; an accepted BEGIN whose END is refused,
 * after an in-place write, the refusal and the image check's result in
 * END's reply alone. */
static void test_after_write(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], hb[FR_SHA256_LEN], rc_img;
	size_t n, nb;

	start(0, 1, true);
	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);
	CHECK(begin((uint32_t)n, h) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 1u);
	CHECK(begin((uint32_t)n, h) == FR_WR_NOT_SELECTED);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_SELECT) == FR_WR_NOT_TRIED);
	CHECK(begin(0, h) == FR_WR_NOT_SELECTED);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_RESULT] == FR_WR_OK);
	CHECK(r[FR_INFO_O_IMAGE_RC] == FR_IMG_OK && r[FR_INFO_O_WRITTEN] == 1u);
	CHECK(r[FR_INFO_O_WAY] == FR_WAY_PLAIN && fr_le16(r + FR_INFO_O_PROGRESS) == 1000u);
	CHECK(memcmp(r + FR_INFO_O_READBACK, h, FR_SHA256_LEN) == 0);
	CHECK(r[FR_INFO_O_REFUSAL] == FR_WR_NOT_SELECTED);

	start(0, 1, true);
	fail_other();
	n = make_image(img_b, 0x2000, 9, true);
	sha(img_b, n, h);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WAY] == FR_WAY_IN_PLACE);
	/* an image with another front for slot 1: accepted at BEGIN,
	 * refused at END */
	g_fseed = 2;
	nb = make_image(img_c, 0x200, 5, true);
	g_fseed = 1;
	sha(img_c, nb, hb);
	CHECK(begin((uint32_t)nb, hb) == FR_WR_OK);
	CHECK(send_all(img_c, nb));
	CHECK(end_rc(&rc_img) == FR_WR_FRONT && rc_img == FR_IMG_OK);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_RESULT] == FR_WR_OK);
	CHECK(r[FR_INFO_O_WRITTEN] == 0u && r[FR_INFO_O_WAY] == FR_WAY_IN_PLACE);
	CHECK(memcmp(r + FR_INFO_O_READBACK, h, FR_SHA256_LEN) == 0);
	/* an image with no front: its result in END's reply */
	nb = make_image(img_c, 0x200, 5, false);
	sha(img_c, nb, hb);
	CHECK(begin((uint32_t)nb, hb) == FR_WR_OK);
	CHECK(send_all(img_c, nb));
	CHECK(end_rc(&rc_img) == FR_WR_IMAGE && rc_img == FR_IMG_NO_FRONT);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_IMAGE_RC] == FR_IMG_OK);
	CHECK(r[FR_INFO_O_WRITTEN] == 0u && r[FR_INFO_O_WAY] == FR_WAY_IN_PLACE);
	/* with no write made, a refusal is the phase's */
	start(0, 0, true);
	CHECK(begin((uint32_t)n, h) == FR_WR_UNVERIFIED);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_UNVERIFIED);
	CHECK(r[FR_INFO_O_WRITTEN] == FR_NO_SLOT);
}

/* S-02.093: the first erase comes after END's reply is handed to the port,
 * not in the poll that accepted END. */
static void test_first_erase(void)
{
	uint8_t h[FR_SHA256_LEN], r[16];
	size_t n;

	for (unsigned way = 0; way < 3u; way++) {
		if (way == 1u)
			start_b_good(0, 1);
		else
			start(0, 1, true);
		if (way == 2u)
			fail_other();	/* in place may go */
		n = make_image(img_b, 0x100, 3, true);
		sha(img_b, n, h);
		CHECK(begin_f((uint32_t)n, h, way ? FR_BEGIN_F_RUNNING : 0) == FR_WR_OK);
		CHECK(send_all(img_b, n));
		seq++;
		send(FR_T_END, seq, NULL, 0);
		poll1();
		CHECK(proto.phase == FR_PH_COUNTDOWN);
		CHECK(n_erase == 0u);
		CHECK(take(FR_T_END, seq, r) == (int)FR_END_REPLY_LEN && r[0] == FR_WR_OK);
		/* nothing until the countdown is over; the first erase in the
		 * poll after the one that ended it */
		through_countdown();
		CHECK(proto.phase == FR_PH_WRITING && n_erase == 0u);
		poll1();
		CHECK(n_erase == 1u);
	}
	/* a port that takes nothing: END's reply waits, and so does the erase */
	start(0, 1, true);
	n = make_image(img_b, 0x100, 3, true);
	sha(img_b, n, h);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	seq++;
	send(FR_T_END, seq, NULL, 0);
	write_limit = 0;
	poll_n(3);
	CHECK(proto.phase == FR_PH_COUNTDOWN);
	CHECK(d2h_n == 0u);
	CHECK(n_erase == 0u);
	/* the countdown over, the reply still waiting: no erase */
	poll_n(20);
	CHECK(proto.phase == FR_PH_WRITING);
	CHECK(d2h_n == 0u);
	CHECK(n_erase == 0u);
	write_limit = SIZE_MAX;
	poll1();
	CHECK(take(FR_T_END, seq, r) == (int)FR_END_REPLY_LEN);
}

/* SC-RECOVERY run 11: the retries */
static void test_retry(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;
	size_t n;

	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);
	for (unsigned req = 0; req < 2u; req++) {
		/* run 9 with an erase of slot 0 that fails, after erasing */
		start_b_good(0, 1);
		fail_erase_at = FR_SLOT_BASE(0);
		erase_then_fail = true;
		CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
		CHECK(send_all(img_b, n));
		CHECK(end_rc(&rc_img) == FR_WR_OK);
		run_to_end(r);
		CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_FLASH);
		CHECK(r[FR_INFO_O_WAY] == FR_WAY_SELECT_FIRST);
		CHECK(flash[FR_SELECTOR_OFF] == 1u && flash[FR_SELECTOR_OFF + 1] == 1u);
		CHECK(r[FR_INFO_O_SEL_INDEX] == 1u && r[FR_INFO_O_SEL_VERIFIED] == 1u);
		CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_MOVED) != 0u);
		/* SC-RECOVERY run 11, S-02.084's retry rule: the selector reads
		 * {B, 01} and B is good, so either request writes slot 0. */
		CHECK(r[FR_INFO_O_TARGET] == 0u && r[FR_INFO_O_REFUSAL] == FR_WR_OK);
		CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_PLAIN);
		CHECK((r[FR_INFO_O_SLOT(0) + FR_INFO_S_FLAGS] & FR_SLOT_F_HAS_FRONT) == 0u);
		if (!req)
			continue;
		/* the same request again: slot 0, with no selector erase
		 * before its first erase */
		fail_erase_at = UINT32_MAX;
		n_fops = n_erase = 0;
		CHECK(begin_f((uint32_t)n, h, req ? FR_BEGIN_F_RUNNING : 0) == FR_WR_OK);
		CHECK(battery_calls == 0u);
		CHECK(send_all(img_b, n));
		CHECK(end_rc(&rc_img) == FR_WR_OK);
		run_to_end(r);
		CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 0u);
		CHECK(r[FR_INFO_O_WAY] == FR_WAY_PLAIN);
		CHECK(first_fop('e', SLOT_LO(0), SLOT_HI(0)) == 0u);
		CHECK(first_fop('e', SLOT_LO(0), SLOT_HI(0)) < first_fop('e', SEL_LO, SEL_HI));
		CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 1u);
		CHECK(memcmp(flash + FR_SLOT_BASE(0), img_b, n) == 0);
		CHECK(memcmp(flash + FR_SLOT_BASE(1), img_c, len_c) == 0);
	}

	/* run 10 with an erase of slot 0 that fails: accepted again, though
	 * slot 0's front is gone, and it ends as run 10 */
	start(0, 1, true);
	fail_other();
	fail_erase_at = FR_SLOT_BASE(0);
	erase_then_fail = true;
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_FLASH);
	CHECK(r[FR_INFO_O_WAY] == FR_WAY_IN_PLACE);
	CHECK((r[FR_INFO_O_SLOT(0) + FR_INFO_S_FLAGS] & FR_SLOT_F_HAS_FRONT) == 0u);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_MOVED) == 0u);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_IN_PLACE && r[FR_INFO_O_RUN_REFUSAL] == FR_WR_OK);
	fail_erase_at = UINT32_MAX;
	n_fops = 0;
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(battery_calls == 2u);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 0u);
	CHECK(r[FR_INFO_O_WAY] == FR_WAY_IN_PLACE);
	CHECK(!any_fop(SEL_LO, SEL_HI));
	CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 1u);
	CHECK(memcmp(flash + FR_SLOT_BASE(0), img_b, n) == 0);
}

/* INFO's flag bits 3 and 4 (S-02.093) after each failed write of the
 * running slot, and the retry state behind them. */
static uint8_t moved_spoiled(const uint8_t *r)
{
	return (uint8_t)(r[FR_INFO_O_FLAGS] & (FR_INFO_F_MOVED | FR_INFO_F_SPOILED));
}

static void test_retry_flags(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;
	size_t n;

	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);

	/* status 14 with the selector untouched (its erase fails, erasing
	 * nothing): neither bit; the running slot not begun */
	start_b_good(0, 1);
	fail_erase_at = FR_SELECTOR_OFF;
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_SELECT_FIRST);
	CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 1u);
	CHECK(moved_spoiled(r) == 0u);
	CHECK(!st.home.spoiled);
	CHECK(!any_fop(SLOT_LO(0), SLOT_HI(0)) && !any_fop(REC_LO(0), REC_HI(0)));
	/* the default request still goes to slot 1, the way of a running-slot
	 * write as at the start */
	CHECK(r[FR_INFO_O_TARGET] == 1u && r[FR_INFO_O_RUN_WAY] == FR_WAY_SELECT_FIRST);

	/* the selector written {01 01}, then slot 0's first erase fails without
	 * erasing: bit 3, and bit 4 clear while slot 0 is still good */
	start_b_good(0, 1);
	fail_erase_at = FR_SLOT_BASE(0);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_FLASH);
	CHECK(flash[FR_SELECTOR_OFF] == 1u && flash[FR_SELECTOR_OFF + 1] == 1u);
	CHECK((r[FR_INFO_O_SLOT(0) + FR_INFO_S_FLAGS] & FR_SLOT_F_GOOD) != 0u);
	CHECK(moved_spoiled(r) == FR_INFO_F_MOVED);
	CHECK(st.home.moved && st.home.spoiled);

	/* ... and failing after erasing: both bits */
	start_b_good(0, 1);
	fail_erase_at = FR_SLOT_BASE(0);
	erase_then_fail = true;
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED);
	CHECK((r[FR_INFO_O_SLOT(0) + FR_INFO_S_FLAGS] & FR_SLOT_F_GOOD) == 0u);
	CHECK(moved_spoiled(r) == (FR_INFO_F_MOVED | FR_INFO_F_SPOILED));
	CHECK(st.home.moved && st.home.spoiled);
	/* the retry: both bits clear once it is done */
	fail_erase_at = UINT32_MAX;
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE);
	CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 1u);
	CHECK(moved_spoiled(r) == 0u);

	/* a failed in-place write: bit 4 alone, and clear after the retry */
	start(0, 1, true);
	fail_other();
	fail_erase_at = FR_SLOT_BASE(0);
	erase_then_fail = true;
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED);
	CHECK(moved_spoiled(r) == FR_INFO_F_SPOILED);
	CHECK(!st.home.moved && st.home.spoiled);
	fail_erase_at = UINT32_MAX;
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE);
	CHECK(moved_spoiled(r) == 0u);

	/* an accepted END whose write has made no step marks nothing: in the
	 * poll that accepts it, and while its reply waits for the port */
	for (unsigned way = 0; way < 2u; way++) {
		if (way)
			start_b_good(0, 1);
		else
			start(0, 1, true);
		if (!way)
			fail_other();	/* in place may go */
		CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
		CHECK(send_all(img_b, n));
		seq++;
		send(FR_T_END, seq, NULL, 0);
		write_limit = 0;
		poll_n(20);	/* the countdown over, the reply waiting */
		CHECK(proto.phase == FR_PH_WRITING);
		CHECK(n_fops == 0u);
		CHECK(!st.home.moved && !st.home.spoiled);
		/* the reply handed to the port, then (in the same poll) the first
		 * step: the selector's erase, or slot 0's */
		write_limit = SIZE_MAX;
		poll1();
		CHECK(take(FR_T_END, seq, r) == (int)FR_END_REPLY_LEN && r[0] == FR_WR_OK);
		CHECK(n_fops >= 1u);
		CHECK(way ? st.home.moved && !st.home.spoiled : st.home.spoiled && !st.home.moved);
	}
}

/* INFO's ways and refusals in each state (S-02.093 offsets 199 to 218) */
/* BEGIN's whole reply: its length, and status, way, target in r. */
static int begin_reply(uint32_t len, const uint8_t *image_sha, uint8_t flags, uint8_t *r)
{
	uint8_t pl[4 + FR_SHA256_LEN + 1];

	fr_put32(pl, len);
	memcpy(pl + 4, image_sha, FR_SHA256_LEN);
	pl[4 + FR_SHA256_LEN] = flags;
	return ask(FR_T_BEGIN, pl, sizeof pl, r);
}

/* S-02.093: BEGIN answers its status, the way and the target; a refusal
 * way 0 and 0xFF. The size is 1 to 0x76000 bytes (S-02.084). */
static void test_begin_reply(void)
{
	uint8_t r[16], h[FR_SHA256_LEN];

	memset(h, 0x11, sizeof h);
	/* the default: plain, slot 1 */
	start(0, 1, true);
	CHECK(begin_reply(0x76000u, h, 0, r) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(r[0] == FR_WR_OK && r[1] == FR_WAY_PLAIN && r[2] == 1u);
	CHECK(proto.len == 0x76000u);
	/* sizes over 0x76000, and 0: status 6 */
	{
		static const uint32_t big[] = { 0x76001u, 0x77000u, 0x77001u, 0x78000u, 0x78001u, 0 };

		for (unsigned i = 0; i < ARRAY_LEN(big); i++) {
			CHECK(begin_reply(big[i], h, 0, r) == (int)FR_BEGIN_REPLY_LEN);
			CHECK(r[0] == FR_WR_SIZE && r[1] == FR_WAY_NONE && r[2] == FR_NO_SLOT);
		}
	}
	/* the running slot, slot 1 bad: refused 15 */
	CHECK(begin_reply(0x1000u, h, FR_BEGIN_F_RUNNING, r) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(r[0] == FR_WR_TRY_OTHER && r[1] == FR_WAY_NONE && r[2] == FR_NO_SLOT);
	/* after a failed write of slot 1: in place, slot 0 */
	fail_other();
	CHECK(begin_reply(0x1000u, h, FR_BEGIN_F_RUNNING, r) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(r[0] == FR_WR_OK && r[1] == FR_WAY_IN_PLACE && r[2] == 0u);
	CHECK(begin_reply(0x76000u, h, FR_BEGIN_F_RUNNING, r) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(r[0] == FR_WR_OK);
	/* slot 1 good: selected first, slot 0 */
	start_b_good(0, 1);
	CHECK(begin_reply(0x1000u, h, FR_BEGIN_F_RUNNING, r) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(r[0] == FR_WR_OK && r[1] == FR_WAY_SELECT_FIRST && r[2] == 0u);
	/* a refused start */
	start(0, 0, true);
	CHECK(begin_reply(0x1000u, h, 0, r) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(r[0] == FR_WR_UNVERIFIED && r[1] == FR_WAY_NONE && r[2] == FR_NO_SLOT);
	/* malformed: a status, nothing announced */
	start(0, 1, true);
	CHECK(begin_reply(0x1000u, h, 0x06, r) >= 1 && r[0] == FR_ST_BAD_REQUEST);
	CHECK(n_fops == 0u);
}

/* S-02.084: the in-place gate through the protocol */
static void test_gate(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;
	size_t n;

	n = make_image(img_b, 0x400, 9, true);
	sha(img_b, n, h);
	/* before any write: 15 in INFO and at BEGIN, nothing written */
	start(0, 1, true);
	info(r);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_NONE && r[FR_INFO_O_RUN_REFUSAL] == FR_WR_TRY_OTHER);
	CHECK(r[FR_INFO_O_TARGET] == 1u && r[FR_INFO_O_REFUSAL] == FR_WR_OK);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_TRY_OTHER);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_TRY_OTHER);
	CHECK(n_fops == 0u && !st.home.other_failed);
	/* a refused write of slot 1 (not the image announced): still 15 */
	{
		uint8_t h2[FR_SHA256_LEN];

		memcpy(h2, h, sizeof h2);
		h2[0] ^= 0x01u;
		CHECK(begin_f((uint32_t)n, h2, 0) == FR_WR_OK);
		CHECK(send_all(img_b, n));
		CHECK(end_rc(&rc_img) == FR_WR_HASH);
	}
	CHECK(!st.home.other_failed);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_TRY_OTHER);
	/* a write of slot 1 failing with 9: in place goes */
	fail_other();
	info(r);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_IN_PLACE && r[FR_INFO_O_RUN_REFUSAL] == FR_WR_OK);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	/* a write of slot 1 failing with 11 (its selector program): not set;
	 * that write leaves slot 1 good, so in place does not apply */
	start(0, 1, true);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	fail_program_at = FR_SELECTOR_OFF;
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_RESULT] == FR_WR_SELECTOR);
	CHECK(!st.home.other_failed);
}

/*
 * RESTART while a write of this recovery left the running slot damaged and
 * selected: answered 0x44, no reset request; again 0 once a good slot is
 * selected (S-02.084, S-02.093). The hold (fr_proto_hold()) for the same
 * time, from the in-place write's acceptance.
 */
static void test_restart_damaged(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img, hb[FR_SHA256_LEN];
	size_t n, nb;

	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);
	for (unsigned then = 0; then < 2u; then++) {
		start(0, 1, true);
		fail_other();
		CHECK(!fr_proto_hold(&proto));
		CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
		CHECK(send_all(img_b, n));
		fail_erase_at = FR_SLOT_BASE(0) + FR_SECTOR;	/* the second erase */
		erase_then_fail = true;
		CHECK(end_rc(&rc_img) == FR_WR_OK);
		/* S-02.084: held from the pass that ends the countdown, before
		 * the first erase */
		CHECK(proto.phase == FR_PH_COUNTDOWN && !fr_proto_hold(&proto));
		through_countdown();
		CHECK(fr_proto_hold(&proto) && n_erase == 0u);
		run_to_end(r);
		CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_FLASH);
		CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_SPOILED) != 0u);
		CHECK(fr_proto_hold(&proto));
		CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_ST_DAMAGED);
		poll_n(4);
		CHECK(!proto.restart && !proto.restart_due);
		fail_erase_at = UINT32_MAX;
		if (then == 0u) {
			/* (i) a retry in place that succeeds */
			CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
			CHECK(send_all(img_b, n));
			CHECK(end_rc(&rc_img) == FR_WR_OK);
			run_to_end(r);
			CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE);
		} else {
			/* (ii) a plain write of slot 1 that succeeds: held throughout,
			 * then slot 1 good and selected {01 00} */
			nb = make_image(img_c, 0x200, 13, true);
			sha(img_c, nb, hb);
			CHECK(begin_f((uint32_t)nb, hb, 0) == FR_WR_OK);
			CHECK(send_all(img_c, nb));
			CHECK(end_rc(&rc_img) == FR_WR_OK);
			for (unsigned i = 0; i < 10000; i++) {
				info(r);
				if (!busy_ph(r[FR_INFO_O_PHASE]))
					break;
				if (proto.phase == FR_PH_WRITING)
					CHECK(fr_proto_hold(&proto));
			}
			CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 1u);
			CHECK(flash[FR_SELECTOR_OFF] == 1u && flash[FR_SELECTOR_OFF + 1] == 1u);
		}
		CHECK(!fr_proto_hold(&proto));
		CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_OK);
		poll_n(2);
		CHECK(proto.restart);
	}
	/* a failed select-first write leaves the selector naming the good
	 * other slot: RESTART is answered 0 */
	start_b_good(0, 1);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	fail_erase_at = FR_SLOT_BASE(0);
	erase_then_fail = true;
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	CHECK(!fr_proto_hold(&proto));
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED);
	CHECK(flash[FR_SELECTOR_OFF] == 1u && flash[FR_SELECTOR_OFF + 1] == 1u);
	CHECK(!fr_proto_hold(&proto));
	CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_OK);
}

/* S-02.093 status 16: the write END would make is not the one BEGIN
 * announced: nothing written, home unchanged, and a new BEGIN works. */
static void test_end_changed(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img, br[16];
	size_t n;

	n = make_image(img_b, 0x400, 9, true);
	sha(img_b, n, h);
	for (unsigned dir = 0; dir < 2u; dir++) {
		struct fr_home home;

		if (dir == 0u) {
			/* announced in place; slot 1 made good before END */
			start(0, 1, true);
			fail_other();
		} else {
			/* announced select first, with other_failed; slot 1 made
			 * bad before END */
			start_b_good(0, 1);
			fail_other();		/* fails without erasing: slot 1 still good */
		}
		CHECK(begin_reply((uint32_t)n, h, FR_BEGIN_F_RUNNING, br) == (int)FR_BEGIN_REPLY_LEN);
		CHECK(br[0] == FR_WR_OK && br[1] == (dir ? FR_WAY_SELECT_FIRST : FR_WAY_IN_PLACE));
		CHECK(send_all(img_b, n));
		if (dir == 0u) {
			len_c = make_image(img_c, 0x200, 7, true);
			put_slot(1, img_c, len_c);
		} else {
			flash[FR_SLOT_BASE(1) + 0x40] ^= 0x01u;
		}
		home = st.home;
		n_fops = 0;
		CHECK(end_rc(&rc_img) == FR_WR_CHANGED);
		poll_n(4);
		CHECK(n_fops == 0u);
		CHECK(memcmp(&home, &st.home, sizeof home) == 0);
		/* INFO keeps the failed write of slot 1 this recovery made */
		info(r);
		CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_FLASH);
		/* a new BEGIN announces what END will do now, and the write goes */
		CHECK(begin_reply((uint32_t)n, h, FR_BEGIN_F_RUNNING, br) == (int)FR_BEGIN_REPLY_LEN);
		CHECK(br[0] == FR_WR_OK && br[1] == (dir ? FR_WAY_IN_PLACE : FR_WAY_SELECT_FIRST));
		CHECK(send_all(img_b, n));
		CHECK(end_rc(&rc_img) == FR_WR_OK);
		run_to_end(r);
		CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 0u);
	}
}

/* --- S-02.084: after a write of A has left A bad (the rescue state) --- */

/* Slot 1 (index 1) holds image A, runs and is selected {01 01}; slot 0 is
 * erased (bad). */
static void start_run1(void)
{
	start(1, 1, true);
	memset(flash + FR_SLOT_BASE(0), 0xFF, FR_SLOT_CAP);
	memset(flash + FR_RECORD_OFF(0), 0xFF, FR_SECTOR);
	put_slot(1, img_a, len_a);
	memset(&st, 0, sizeof st);
	st.why = FR_WHY_KEYS;
	st.running.established = true;
	st.running.slot = 1;
	survey();
	st.own_desc = st.slot[1].has_desc;
	st.desc = st.slot[1].desc;
	fr_proto_init(&proto, &port, &st, &fl, staging);
	proto.battery = battery;
	fr_proto_clock(&proto, g_now);
}

/* A whole write: BEGIN (status), DATA, END, then polled to its end; the
 * END status, or BEGIN's when refused. INFO's reply in r. */
static uint8_t write_img(const uint8_t *img, size_t n, uint8_t flags, uint8_t *r)
{
	uint8_t h[FR_SHA256_LEN], rc_img, s;

	sha(img, n, h);
	s = begin_f((uint32_t)n, h, flags);
	if (s != FR_WR_OK)
		return s;
	CHECK(send_all(img, n));
	s = end_rc(&rc_img);
	if (s == FR_WR_OK)
		run_to_end(r);
	return s;
}

/* SC-RECOVERY run 16's state: the gate opened by a failed write of slot 0,
 * the in-place write of slot 1 failed with its first erase taken, then a
 * write of slot 0 whose selector program is not stored: result 11, the
 * selector FF FF, slot 0 good, slot 1 damaged. Slot 0 holds img_c. */
static void case_a_state(void)
{
	uint8_t r[300] = { 0 };
	size_t n;

	start_run1();
	len_c = make_image(img_c, 0x200, 7, true);
	fail_erase_at = FR_SLOT_BASE(0);
	CHECK(write_img(img_c, len_c, 0, r) == FR_WR_OK);
	CHECK(r[FR_INFO_O_RESULT] == FR_WR_FLASH && st.home.other_failed);
	n = make_image(img_b, 0x400, 9, true);
	fail_erase_at = FR_SLOT_BASE(1);
	erase_then_fail = true;
	CHECK(write_img(img_b, n, FR_BEGIN_F_RUNNING, r) == FR_WR_OK);
	CHECK(r[FR_INFO_O_RESULT] == FR_WR_FLASH && r[FR_INFO_O_WAY] == FR_WAY_IN_PLACE);
	fail_erase_at = UINT32_MAX;
	erase_then_fail = false;
	sel_prog_lazy = true;
	CHECK(write_img(img_c, len_c, 0, r) == FR_WR_OK);
	CHECK(r[FR_INFO_O_RESULT] == FR_WR_SELECTOR);
	sel_prog_lazy = false;
	/* the settling on slot 0 from {01, 01} is a program of the index
	 * alone (S-02.084), not stored: the selector stays {01, 01}. Run 16's
	 * FF FF is written to the flash model directly. */
	CHECK(flash[FR_SELECTOR_OFF] == 0x01u && flash[FR_SELECTOR_OFF + 1] == 0x01u);
	flash[FR_SELECTOR_OFF] = 0xFFu;
	flash[FR_SELECTOR_OFF + 1] = 0xFFu;
	n_fops = 0;
}

static bool fops_in(uint32_t lo, uint32_t hi)
{
	return any_fop(lo, hi);
}

static uint8_t img_y[0x4000];

/* Review case 1(a), SC-RECOVERY run 16 */
static void test_rescue_case_a(void)
{
	uint8_t r[300] = { 0 }, br[16], h[FR_SHA256_LEN];
	size_t n;

	/* FF FF names slot 1, damaged: a fallback to slot 0 does not count
	 * (the first stage's own {00, 01} could be cut to {01, 01}) */
	case_a_state();
	info(r);
	CHECK((r[FR_INFO_O_SLOT(0) + FR_INFO_S_FLAGS] & FR_SLOT_F_GOOD) != 0u);
	CHECK((r[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] & FR_SLOT_F_GOOD) == 0u);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_SPOILED) != 0u);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_HELD) != 0u);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_READ_ERROR) == 0u);
	CHECK(fr_rec_reach(&st) == FR_REACH_NONE);
	CHECK(fr_proto_hold(&proto));
	/* nothing refused for the selector's conditions */
	CHECK(r[FR_INFO_O_TARGET] == 0u && r[FR_INFO_O_REFUSAL] == FR_WR_OK);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_SELECT_FIRST && r[FR_INFO_O_RUN_REFUSAL] == FR_WR_OK);
	CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_ST_DAMAGED);

	/* slot 0's own image without the flag: nothing of slot 0 or record 0
	 * written, then the selector 00 01, a byte at a time */
	case_a_state();
	sha(img_c, len_c, h);
	CHECK(begin_reply((uint32_t)len_c, h, 0, br) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_PLAIN && br[2] == 0u);
	CHECK(send_all(img_c, len_c));
	CHECK(end_rc(&(uint8_t){ 0 }) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 0u);
	CHECK(!fops_in(SLOT_LO(0), SLOT_HI(0)) && !fops_in(REC_LO(0), REC_HI(0)));
	CHECK(!fops_in(SLOT_LO(1), SLOT_HI(1)) && !fops_in(REC_LO(1), REC_HI(1)));
	/* from FF FF: programs alone, the index then verified, no erase */
	CHECK(n_fops == 2u && fops[0].kind == 'p' && fops[0].off == FR_SELECTOR_OFF &&
	      fops[1].kind == 'p' && fops[1].off == FR_SELECTOR_OFF + 1u);
	CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 1u);
	CHECK(memcmp(flash + FR_SLOT_BASE(0), img_c, len_c) == 0);
	info(r);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_HELD) == 0u);
	CHECK(!fr_proto_hold(&proto));
	CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_OK);

	/* another image without the flag: 18, nothing written */
	case_a_state();
	n = make_image(img_y, 0x300, 21, true);
	CHECK(write_img(img_y, n, 0, r) == FR_WR_ONLY_GOOD);
	CHECK(n_fops == 0u);
	CHECK(flash[FR_SELECTOR_OFF] == 0xFFu && flash[FR_SELECTOR_OFF + 1] == 0xFFu);

	/* with the flag: way 2, slot 1; 00 01 first, then slot 1, then 01 01 */
	case_a_state();
	sha(img_y, n, h);
	CHECK(begin_reply((uint32_t)n, h, FR_BEGIN_F_RUNNING, br) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_SELECT_FIRST && br[2] == 1u);
	CHECK(send_all(img_y, n));
	CHECK(end_rc(&(uint8_t){ 0 }) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 1u);
	/* slot 1 damaged: the settling on slot 0 first, from FF FF by programs
	 * alone, the index then verified (S-02.084) */
	CHECK(n_fops >= 6u && fops[0].kind == 'p' && fops[0].off == FR_SELECTOR_OFF);
	CHECK(fops[1].kind == 'p' && fops[1].off == FR_SELECTOR_OFF + 1u);
	CHECK(first_fop('e', SLOT_LO(1), SLOT_HI(1)) == 2u);
	CHECK(!fops_in(SLOT_LO(0), SLOT_HI(0)) && !fops_in(REC_LO(0), REC_HI(0)));
	/* settled on slot 1 at the end: erase, 01, 01 */
	CHECK(fops[n_fops - 3u].kind == 'e' && fops[n_fops - 3u].off == FR_SELECTOR_OFF);
	CHECK(flash[FR_SELECTOR_OFF] == 1u && flash[FR_SELECTOR_OFF + 1] == 1u);
	CHECK(memcmp(flash + FR_SLOT_BASE(1), img_y, n) == 0);
	info(r);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_HELD) == 0u);
	CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_OK);
}

/* Slot 0 runs, damaged by a select-first write that failed at its first
 * erase (taken), slot 1 good; then the selector set to {00, v}. */
static void damaged_sel(uint8_t v, uint8_t *r)
{
	size_t n = make_image(img_b, 0x400, 9, true);

	start_b_good(0, 1);
	fail_erase_at = FR_SLOT_BASE(0);
	erase_then_fail = true;
	CHECK(write_img(img_b, n, FR_BEGIN_F_RUNNING, r) == FR_WR_OK);
	CHECK(r[FR_INFO_O_RESULT] == FR_WR_FLASH);
	fail_erase_at = UINT32_MAX;
	erase_then_fail = false;
	flash[FR_SELECTOR_OFF] = 0x00;
	flash[FR_SELECTOR_OFF + 1] = v;
	n_fops = 0;
}

/* SC-RECOVERY run 24, the fallback case: slot 0 running and damaged, its
 * record whole (sure to fail), slot 1 good, the selector {00, v} with a
 * verified bit set besides bit 0 (torn to 3F, or FE). A start reaches slot
 * 1 by the fallback, so bit 5 is clear and RESTART answered 0. The flag
 * selects slot 1 first (way 2): the selector erased and written {01, 01}
 * before slot 0's first erase, so that no first stage reads slot 0's record
 * while it is written (S-02.084); slot 1's own image settles on slot 1;
 * another image without the flag is 18. */
static void test_rescue_torn_verified(void)
{
	static const uint8_t vs[] = { 0x3F, 0xFE };
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;

	for (unsigned i = 0; i < ARRAY_LEN(vs); i++)
		for (unsigned v = 0; v < 3u; v++) {
			size_t n;

			damaged_sel(vs[i], r);
			info(r);
			CHECK(r[FR_INFO_O_SEL_INDEX] == 0u && r[FR_INFO_O_SEL_VERIFIED] == vs[i]);
			CHECK(r[FR_INFO_O_REFUSAL] == FR_WR_OK && r[FR_INFO_O_TARGET] == 1u);
			CHECK(r[FR_INFO_O_RUN_REFUSAL] == FR_WR_OK);
			CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_SELECT_FIRST);
			CHECK((r[FR_INFO_O_FLAGS] & (FR_INFO_F_SPOILED | FR_INFO_F_HELD |
						     FR_INFO_F_UNSETTLED)) == FR_INFO_F_SPOILED);
			CHECK(fr_rec_reach(&st) == FR_REACH_FALLBACK);
			CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_OK);
			damaged_sel(vs[i], r);	/* RESTART left a restart due: again */
			n = make_image(img_y, 0x300, 23, true);
			if (v == 0u) {
				/* slot 1's own image: the selector alone, on slot 1 */
				CHECK(write_img(img_c, len_c, 0, r) == FR_WR_OK);
				CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 1u);
				CHECK(!any_fop(0, SEL_LO) && !any_fop(SEL_LO + FR_SECTOR, FLASH_SIZE));
				CHECK(flash[FR_SELECTOR_OFF] == 1u && flash[FR_SELECTOR_OFF + 1] == 1u);
			} else if (v == 1u) {
				/* another image without the flag: 18, nothing written */
				CHECK(write_img(img_y, n, 0, r) == FR_WR_ONLY_GOOD);
				CHECK(n_fops == 0u);
			} else {
				/* the flag: way 2, {01, 01} before slot 0's first erase */
				sha(img_y, n, h);
				CHECK(begin_reply((uint32_t)n, h, FR_BEGIN_F_RUNNING, r) ==
				      (int)FR_BEGIN_REPLY_LEN);
				CHECK(r[0] == FR_WR_OK && r[1] == FR_WAY_SELECT_FIRST && r[2] == 0u);
				CHECK(send_all(img_y, n));
				CHECK(end_rc(&rc_img) == FR_WR_OK);
				run_to_end(r);
				CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 0u);
				CHECK(r[FR_INFO_O_WAY] == FR_WAY_SELECT_FIRST);
				CHECK(n_fops >= 3u && fops[0].kind == 'e' && fops[0].off == SEL_LO);
				CHECK(fops[1].kind == 'p' && fops[1].off == SEL_LO);
				CHECK(fops[2].kind == 'p' && fops[2].off == SEL_LO + 1u);
				CHECK(first_fop('e', SLOT_LO(0), SLOT_HI(0)) == 3u);
				CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 1u);
			}
		}
}

/* An in-place retry settles the selector on slot 0 after the read-back,
 * a byte at a time, unless it reads {00, 01} already (S-02.084). */
static void test_rescue_in_place_close(void)
{
	uint8_t r[300] = { 0 };

	for (unsigned torn = 0; torn < 3u; torn++) {
		size_t n = make_image(img_b, 0x400, 9, true);

		start(0, 1, true);
		fail_other();
		fail_erase_at = FR_SLOT_BASE(0);
		erase_then_fail = true;
		CHECK(write_img(img_b, n, FR_BEGIN_F_RUNNING, r) == FR_WR_OK);
		CHECK(r[FR_INFO_O_RESULT] == FR_WR_FLASH);
		fail_erase_at = UINT32_MAX;
		erase_then_fail = false;
		if (torn == 1u) {		/* names slot 1, bad */
			flash[FR_SELECTOR_OFF] = 0xFF;
			flash[FR_SELECTOR_OFF + 1] = 0xFF;
		} else if (torn == 2u) {	/* names slot 0 with verified 00 */
			flash[FR_SELECTOR_OFF] = 0x00;
			flash[FR_SELECTOR_OFF + 1] = 0x00;
		}
		n_fops = 0;
		info(r);
		CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_HELD) != 0u);
		CHECK(fr_proto_hold(&proto));
		CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_IN_PLACE);
		CHECK(write_img(img_b, n, FR_BEGIN_F_RUNNING, r) == FR_WR_OK);
		CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 0u);
		CHECK(!fops_in(SLOT_LO(1), SLOT_HI(1)) && !fops_in(REC_LO(1), REC_HI(1)));
		if (torn == 1u) {
			/* from FF FF on slot 0: the programs alone, last, no erase */
			CHECK(first_fop('e', SEL_LO, SEL_HI) == ~0u);
			CHECK(n_fops >= 2u && fops[n_fops - 2u].kind == 'p' &&
			      fops[n_fops - 2u].off == FR_SELECTOR_OFF);
			CHECK(fops[n_fops - 1u].kind == 'p' &&
			      fops[n_fops - 1u].off == FR_SELECTOR_OFF + 1u);
			CHECK(first_fop('p', SEL_LO, SEL_HI) > first_fop('p', REC_LO(0), REC_HI(0)));
		} else if (torn == 2u) {
			/* from {00, 00}: verified must rise, so the erase and its two
			 * 1-byte programs last */
			CHECK(n_fops >= 3u && fops[n_fops - 3u].kind == 'e' &&
			      fops[n_fops - 3u].off == FR_SELECTOR_OFF);
			CHECK(fops[n_fops - 2u].kind == 'p' && fops[n_fops - 2u].off == FR_SELECTOR_OFF);
			CHECK(fops[n_fops - 1u].kind == 'p' &&
			      fops[n_fops - 1u].off == FR_SELECTOR_OFF + 1u);
			CHECK(first_fop('e', SEL_LO, SEL_HI) > first_fop('p', REC_LO(0), REC_HI(0)));
		} else {
			CHECK(!fops_in(SEL_LO, SEL_HI));
		}
		CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 1u);
		CHECK(!fr_proto_hold(&proto));
		CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_OK);
	}
}

/* Run 19's state: A runs, damaged by a failed in-place write after a
 * failed write of B (the gate); then B made good with img_c and the
 * selector {B, 00}. */
static void run19_state(unsigned a, size_t n)
{
	const unsigned b = 1u - a;
	uint8_t r[300] = { 0 };

	if (a == 1u)
		start_run1();
	else
		start(0, 1, true);
	len_c = make_image(img_c, 0x200, 7, true);
	fail_erase_at = FR_SLOT_BASE(b);
	CHECK(write_img(img_c, len_c, 0, r) == FR_WR_OK);
	CHECK(st.home.other_failed);
	fail_erase_at = FR_SLOT_BASE(a);
	erase_then_fail = true;
	CHECK(write_img(img_y, n, FR_BEGIN_F_RUNNING, r) == FR_WR_OK);
	CHECK(r[FR_INFO_O_RESULT] == FR_WR_FLASH);
	fail_erase_at = UINT32_MAX;
	erase_then_fail = false;
	put_slot(b, img_c, len_c);
	flash[FR_SELECTOR_OFF] = (uint8_t)b;
	flash[FR_SELECTOR_OFF + 1] = 0x00;
	n_fops = 0;
}

/*
 * SC-RECOVERY run 19: no erase of a selector that leads to a good slot.
 * Slot 1 runs, damaged by a failed in-place write; slot 0 good; the
 * selector {00, 00}. The flag: way 1 to slot 1, no selector erase before
 * slot 1's last program; slot 0's own image without the flag: 13; bit 5
 * set and RESTART 0x44 until the write of slot 1 has ended, which ends
 * with {01, 01}. Mirrored: slot 0 running and damaged, slot 1 good, the
 * selector {01, 00}: bit 5 clear and RESTART 0 from the start.
 */
static void test_run19(void)
{
	uint8_t r[300] = { 0 }, br[16], h[FR_SHA256_LEN];

	for (unsigned mirror = 0; mirror < 2u; mirror++) {
		const unsigned a = mirror ? 0u : 1u, b = 1u - a;
		const size_t n = make_image(img_y, 0x300, 25, true);

		run19_state(a, n);
		info(r);
		CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_SPOILED) != 0u);
		CHECK(((r[FR_INFO_O_FLAGS] & FR_INFO_F_HELD) != 0u) == !mirror);
		CHECK(r[FR_INFO_O_TARGET] == a && r[FR_INFO_O_RUN_WAY] == FR_WAY_PLAIN);
		CHECK(fr_proto_hold(&proto) == !mirror);
		CHECK(status_of(FR_T_RESTART, NULL, 0) == (mirror ? FR_WR_OK : FR_ST_DAMAGED));
		if (mirror)
			run19_state(a, n);	/* the restart was accepted: again */
		/* the flag: way 1 to A; no selector erase before A's last program */
		sha(img_y, n, h);
		CHECK(begin_reply((uint32_t)n, h, FR_BEGIN_F_RUNNING, br) == (int)FR_BEGIN_REPLY_LEN);
		CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_PLAIN && br[2] == a);
		CHECK(send_all(img_y, n));
		CHECK(end_rc(&(uint8_t){ 0 }) == FR_WR_OK);
		run_to_end(r);
		CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == a);
		CHECK(first_fop('e', SEL_LO, SEL_HI) > first_fop('p', REC_LO(a), REC_HI(a)));
		CHECK(!fops_in(SLOT_LO(b), SLOT_HI(b)) && !fops_in(REC_LO(b), REC_HI(b)));
		/* settled: {A, 01} (S-02.084) */
		CHECK(flash[FR_SELECTOR_OFF] == a && flash[FR_SELECTOR_OFF + 1] == 1u);
		info(r);
		CHECK((r[FR_INFO_O_FLAGS] & (FR_INFO_F_SPOILED | FR_INFO_F_HELD)) == 0u);
		CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_OK);
	}
}

/* Polls until the write has ended, with no request: what the re-read after
 * the write left in st, before any refresh. */
static void poll_to_end(void)
{
	for (unsigned i = 0; i < 10000u && busy_ph(proto.phase); i++)
		poll1();
}

/*
 * SC-RECOVERY run 22 and its mirror (S-02.084): the running slot A damaged by run 14's failed
 * in-place write, B bad, the selector then set to {00, 00} (or {01, 03}
 * and {00, 03} for the programs alone); the write of A with the flag reads
 * A back good, then its settling fails: 0 the erase taken and the index
 * program not stored; 1 the index stored, its read-back failing; 2 the
 * verified program not stored; 3 the index program failing (programs
 * alone); 4 the verified program failing (programs alone). Result 11, A
 * good; bit 7 set and bit 4 clear; held (bit 5, RESTART 0x44) where no
 * start reaches a good slot, as for slot 0 every time. BEGIN with the flag
 * is way 1 to A; another image is 18 at END; A's own image erases and
 * programs nothing in A or its record and settles the selector, after
 * which bits 5 and 7 are clear and RESTART is answered 0. BEGIN without the
 * flag is a write of B.
 */
static void unsettled_state(unsigned a, unsigned m, const uint8_t *img, size_t n)
{
	static const uint16_t from[] = { 0x0000u, 0x0000u, 0x0000u, 0x0301u, 0x0300u };
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;

	if (a == 0u) {
		start(0, 1, true);
		fail_other();
	} else {
		start_run1();
		len_c = make_image(img_c, 0x200, 7, true);
		fail_erase_at = FR_SLOT_BASE(0);
		CHECK(write_img(img_c, len_c, 0, r) == FR_WR_OK);
		CHECK(r[FR_INFO_O_RESULT] == FR_WR_FLASH && st.home.other_failed);
	}
	fail_erase_at = FR_SLOT_BASE(a);
	erase_then_fail = true;
	CHECK(write_img(img, n, FR_BEGIN_F_RUNNING, r) == FR_WR_OK);
	CHECK(r[FR_INFO_O_RESULT] == FR_WR_FLASH && r[FR_INFO_O_WAY] == FR_WAY_IN_PLACE);
	fail_erase_at = UINT32_MAX;
	erase_then_fail = false;
	flash[FR_SELECTOR_OFF] = (uint8_t)from[m];
	flash[FR_SELECTOR_OFF + 1] = (uint8_t)(from[m] >> 8);
	/* the write again, its settling failing as m says */
	sha(img, n, h);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	for (unsigned i = 0; i < 10000u && busy_ph(proto.phase) &&
	     proto.w.state != FR_WS_SELECTOR_ERASE; i++)
		poll1();
	CHECK(proto.w.state == FR_WS_SELECTOR_ERASE);
	sel_prog_lazy = m == 0u;
	if (m == 1u) {
		fail_read_once_at = FR_SELECTOR_OFF;	/* the settling's read, then */
		fail_read_skip = 1;			/* the index's read-back */
	}
	ver_prog_lazy = m == 2u;
	fail_program_at = m == 3u ? FR_SELECTOR_OFF : m == 4u ? FR_SELECTOR_OFF + 1u : UINT32_MAX;
	poll_to_end();
	sel_prog_lazy = ver_prog_lazy = false;
	fail_program_at = UINT32_MAX;
	fail_read_once_at = UINT32_MAX;
	fail_read_skip = 0;
	n_fops = 0;
}

static void test_run22(void)
{
	uint8_t r[300] = { 0 }, br[16], h[FR_SHA256_LEN];

	for (unsigned a = 0; a < 2u; a++)
		for (unsigned m = 0; m < 5u; m++) {
			const unsigned b = 1u - a;
			size_t n = make_image(img_b, 0x400, 9, true), ny;
			bool held;

			if (a == 1u && m >= 3u)
				continue;	/* slot 1 is settled by no program */
			unsettled_state(a, m, img_b, n);
			CHECK(proto.phase == FR_PH_FAILED && proto.result == FR_WR_SELECTOR);
			CHECK(st.slot[a].good && !st.slot[b].good);
			CHECK(st.home.spoiled && st.home.unsettled);
			if (a == 0u && m == 0u)
				CHECK(flash[FR_SELECTOR_OFF] == 0xFFu && flash[FR_SELECTOR_OFF + 1] == 0xFFu);
			held = fr_rec_reach(&st) == FR_REACH_NONE;
			if (a == 0u)
				CHECK(held);
			info(r);
			CHECK(r[FR_INFO_O_RESULT] == FR_WR_SELECTOR);
			CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_UNSETTLED) != 0u);
			CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_SPOILED) == 0u);
			CHECK(((r[FR_INFO_O_FLAGS] & FR_INFO_F_HELD) != 0u) == held);
			CHECK(fr_rec_rescue(&st) && fr_rec_held(&st) == held);
			CHECK(fr_proto_hold(&proto) == held);
			CHECK(r[FR_INFO_O_TARGET] == b && r[FR_INFO_O_RUN_WAY] == FR_WAY_PLAIN);
			CHECK(status_of(FR_T_RESTART, NULL, 0) == (held ? FR_ST_DAMAGED : FR_WR_OK));
			if (!held)
				unsettled_state(a, m, img_b, n);	/* the restart was due: again */
			/* without the flag: a write of B */
			sha(img_b, n, h);
			CHECK(begin_reply((uint32_t)n, h, 0, br) == (int)FR_BEGIN_REPLY_LEN);
			CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_PLAIN && br[2] == b);
			/* the flag: way 1 to A; another image is 18 at END */
			ny = make_image(img_y, 0x300, 23, true);
			sha(img_y, ny, h);
			CHECK(begin_reply((uint32_t)ny, h, FR_BEGIN_F_RUNNING, br) ==
			      (int)FR_BEGIN_REPLY_LEN);
			CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_PLAIN && br[2] == a);
			CHECK(send_all(img_y, ny));
			CHECK(end_rc(&(uint8_t){ 0 }) == FR_WR_ONLY_GOOD);
			CHECK(n_fops == 0u);
			CHECK(st.home.unsettled && fr_proto_hold(&proto) == held);
			/* A's own image: the selector alone, settled on A */
			CHECK(write_img(img_b, n, FR_BEGIN_F_RUNNING, r) == FR_WR_OK);
			CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == a);
			CHECK(!fops_in(SLOT_LO(a), SLOT_HI(a)) && !fops_in(REC_LO(a), REC_HI(a)));
			CHECK(!fops_in(SLOT_LO(b), SLOT_HI(b)) && !fops_in(REC_LO(b), REC_HI(b)));
			if (a == 0u)
				CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 1u);
			else
				CHECK(fr_selected_slot(flash[FR_SELECTOR_OFF]) == 1u);
			CHECK(!st.home.unsettled);
			info(r);
			CHECK((r[FR_INFO_O_FLAGS] & (FR_INFO_F_HELD | FR_INFO_F_UNSETTLED |
						     FR_INFO_F_SPOILED)) == 0u);
			CHECK(!fr_proto_hold(&proto));
			CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_OK);
		}
}

/* SC-RECOVERY run 23: slot 1 runs, damaged; slot 0 good; the selector
 * {00, 03}. No start reaches a good slot (held). Without the flag slot 0's
 * own image is the read-back and one program, verified to 01, with no
 * erase, and the hold ends; another image is 18. With the flag the write
 * goes to slot 1 with no selector write first. */
static void test_run23(void)
{
	uint8_t r[300] = { 0 }, br[16], h[FR_SHA256_LEN];

	for (unsigned v = 0; v < 3u; v++) {
		const size_t ny = make_image(img_y, 0x300, 23, true);
		const size_t n = make_image(img_b, 0x400, 9, true);

		run19_state(1, ny);
		flash[FR_SELECTOR_OFF] = 0x00u;
		flash[FR_SELECTOR_OFF + 1] = 0x03u;
		info(r);
		CHECK(st.slot[0].good && !st.slot[1].good && st.home.spoiled);
		CHECK((r[FR_INFO_O_FLAGS] & (FR_INFO_F_SPOILED | FR_INFO_F_HELD |
					     FR_INFO_F_UNSETTLED)) ==
		      (FR_INFO_F_SPOILED | FR_INFO_F_HELD));
		CHECK(r[FR_INFO_O_TARGET] == 0u && r[FR_INFO_O_RUN_WAY] == FR_WAY_PLAIN);
		CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_ST_DAMAGED);
		n_fops = 0;
		if (v == 0u) {
			CHECK(write_img(img_c, len_c, 0, r) == FR_WR_OK);
			CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 0u);
			CHECK(n_fops == 1u && fops[0].kind == 'p' && fops[0].off == SEL_LO + 1u);
			CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 1u);
			info(r);
			CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_HELD) == 0u);
			CHECK(!fr_proto_hold(&proto));
		} else if (v == 1u) {
			CHECK(write_img(img_b, n, 0, r) == FR_WR_ONLY_GOOD);
			CHECK(n_fops == 0u);
		} else {
			sha(img_b, n, h);
			CHECK(begin_reply((uint32_t)n, h, FR_BEGIN_F_RUNNING, br) ==
			      (int)FR_BEGIN_REPLY_LEN);
			CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_PLAIN && br[2] == 1u);
			CHECK(send_all(img_b, n));
			CHECK(end_rc(&(uint8_t){ 0 }) == FR_WR_OK);
			run_to_end(r);
			CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 1u);
			CHECK(first_fop('e', SEL_LO, SEL_HI) > first_fop('p', REC_LO(1), REC_HI(1)));
			CHECK(first_fop('p', SEL_LO, SEL_HI) > first_fop('p', REC_LO(1), REC_HI(1)));
		}
	}
}

/* Review case 1(b), SC-RECOVERY run 17: a read that failed does not stick */
static void test_rescue_read_errors(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], hello_r[64], rc_img;
	size_t n;

	n = make_image(img_b, 0x10000, 9, true);
	sha(img_b, n, h);
	/* the selector read fails once, in the re-read after a failed
	 * in-place write */
	start(0, 1, true);
	fail_other();
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	fail_erase_at = FR_SLOT_BASE(0) + 10u * FR_SECTOR;
	erase_then_fail = true;
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	through_countdown();
	CHECK(proto.phase == FR_PH_WRITING);
	fail_read_once_at = FR_SELECTOR_OFF;
	poll_to_end();
	CHECK(proto.phase == FR_PH_FAILED);
	CHECK(st.read_error && !st.selector_ok);
	CHECK(ask(FR_T_HELLO, NULL, 0, hello_r) == (int)FR_HELLO_LEN);
	CHECK((hello_r[FR_HELLO_O_FLAGS] & FR_INFO_F_READ_ERROR) != 0u);
	CHECK(fr_proto_hold(&proto));	/* the selector unknown: held */
	fail_erase_at = UINT32_MAX;
	info(r);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_READ_ERROR) == 0u);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_SELECTOR) != 0u);
	CHECK(r[FR_INFO_O_REFUSAL] == FR_WR_OK && r[FR_INFO_O_RUN_REFUSAL] == FR_WR_OK);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_IN_PLACE);
	/* a read error that persists: 17, bit 6 stays, nothing written, held */
	fail_read_always_at = FR_SELECTOR_OFF;
	n_fops = 0;
	info(r);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_READ_ERROR) != 0u);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_READ);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_READ);
	info(r);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_READ_ERROR) != 0u);
	CHECK(r[FR_INFO_O_REFUSAL] == FR_WR_READ);
	CHECK(n_fops == 0u);
	CHECK(fr_proto_hold(&proto));
	CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_ST_DAMAGED);
	/* the read works again: the writes are offered */
	fail_read_always_at = UINT32_MAX;
	info(r);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_READ_ERROR) == 0u);
	CHECK(r[FR_INFO_O_RUN_REFUSAL] == FR_WR_OK);

	/* then a plain write of slot 1 that succeeds, with slot 1's CRC pass
	 * failing once in the re-read after it */
	{
		const size_t nc = make_image(img_c, 0x200, 13, true);
		uint8_t hc[FR_SHA256_LEN];

		sha(img_c, nc, hc);
		CHECK(begin_f((uint32_t)nc, hc, 0) == FR_WR_OK);
		CHECK(send_all(img_c, nc));
		CHECK(end_rc(&rc_img) == FR_WR_OK);
		for (unsigned i = 0; i < 10000u && busy_ph(proto.phase) &&
		     proto.w.state != FR_WS_SELECTOR_ERASE; i++)
			poll1();
		fail_read_once_at = FR_SLOT_BASE(1);
		poll_to_end();
		CHECK(proto.phase == FR_PH_DONE);
		CHECK(flash[FR_SELECTOR_OFF] == 1u && flash[FR_SELECTOR_OFF + 1] == 1u);
		CHECK(st.read_error && !st.slot[1].good);
		CHECK(fr_proto_hold(&proto));
		info(r);
		CHECK((r[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] & FR_SLOT_F_GOOD) != 0u);
		CHECK((r[FR_INFO_O_FLAGS] & (FR_INFO_F_HELD | FR_INFO_F_READ_ERROR)) == 0u);
		CHECK(!fr_proto_hold(&proto));
		CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_OK);
	}
}

/* fr_rec_reach() over every selector and slot state, against the first
 * stage's rules (S-02.020-S-02.023, S-02.033): the named slot when good;
 * else, with verified not 01, the other slot when good, after the named
 * slot's check fails; else none. An unreadable selector reaches nothing
 * known. */
static void test_reach(void)
{
	static const uint8_t index[] = { 0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF };
	static const uint8_t verified[] = { 0x00, 0x01, 0x02, 0x3F, 0xFE, 0xFF };
	/* the named slot's record when that slot is bad: its tag and length */
	static const struct {
		uint32_t tag, length;
		bool sure_to_fail;	/* the factory check cannot pass it */
	} recs[] = {
		{ FR_TAG, 0x1000u, true },	/* the CRC differs */
		{ FR_TAG, 1u, true },
		{ FR_TAG, 0x78000u, true },
		{ 0x0000B005u, 0x1000u, true },	/* another tag */
		{ 0xFFFFFFFFu, 0xFFFFFFFFu, true },	/* erased */
		{ FR_TAG, 0u, false },		/* empty: S-02.021 passes it */
		{ FR_TAG, 0x78001u, false },	/* S-02.026: left to chance */
		{ FR_TAG, 0x80000000u, false },
	};

	for (unsigned ok = 0; ok < 2u; ok++)
		for (unsigned re = 0; re < 2u; re++)
			for (unsigned i = 0; i < ARRAY_LEN(index); i++)
				for (unsigned v = 0; v < ARRAY_LEN(verified); v++)
					for (unsigned g = 0; g < 4u; g++)
						for (unsigned c = 0; c < ARRAY_LEN(recs); c++) {
							struct fr_rec_status s;
							const unsigned named = index[i] == 0u ? 0u : 1u;
							unsigned want;

							memset(&s, 0, sizeof s);
							s.selector_ok = ok != 0u;
							s.read_error = re != 0u;
							s.sel.index = index[i];
							s.sel.verified = verified[v];
							s.slot[0].good = (g & 1u) != 0u;
							s.slot[1].good = (g & 2u) != 0u;
							s.slot[named].rec.tag = recs[c].tag;
							s.slot[named].rec.length = recs[c].length;
							const bool good[2] = { s.slot[0].good,
									       s.slot[1].good };

							CHECK(spec_sure_to_fail(&s.slot[named].rec) ==
							      recs[c].sure_to_fail);
							want = !ok || re ? FR_REACH_NONE :
							       spec_reach(index[i], verified[v], good,
									  &s.slot[named].rec);
							CHECK(fr_rec_reach(&s) == want);
						}
}

/* In the damaged state with the selector naming slot 0 (bad) with verified
 * 00 and slot 1 good: the hold, bit 5 and RESTART's answer follow the
 * reach for each kind of slot 0 record, and a read that failed. */
static void test_reach_hold(void)
{
	static const struct {
		uint32_t tag, length;
		bool reached;
	} recs[] = {
		{ FR_TAG, 0x400u, true },
		{ 0x0000B005u, 0x400u, true },
		{ FR_TAG, 0u, false },
		{ FR_TAG, 0x78001u, false },
		{ FR_TAG, 0x80000000u, false },
	};
	uint8_t r[300] = { 0 };
	size_t n = make_image(img_b, 0x400, 9, true);

	for (unsigned vi = 0; vi < 3u; vi++)
	for (unsigned c = 0; c <= ARRAY_LEN(recs); c++) {
		/* verified 00 and 01: no fallback (no bit besides bit 0); 3F: as
		 * the record says (S-02.084) */
		static const uint8_t vsel[3] = { 0x00, 0x01, 0x3F };
		const bool read_err = c == ARRAY_LEN(recs);
		bool reached;

		start_b_good(0, 1);
		fail_erase_at = FR_SLOT_BASE(0);
		erase_then_fail = true;
		CHECK(write_img(img_b, n, FR_BEGIN_F_RUNNING, r) == FR_WR_OK);
		CHECK(r[FR_INFO_O_RESULT] == FR_WR_FLASH && st.home.spoiled);
		fail_erase_at = UINT32_MAX;
		erase_then_fail = false;
		flash[FR_SELECTOR_OFF] = 0x00;
		flash[FR_SELECTOR_OFF + 1] = vsel[vi];
		if (read_err) {
			/* the record as at the failure (sure to fail), but a read of
			 * slot 1 that keeps failing */
			fail_read_always_at = FR_SLOT_BASE(1);
			reached = false;
		} else {
			fr_put32(flash + FR_RECORD_OFF(0) + 4, recs[c].length);
			fr_put32(flash + FR_RECORD_OFF(0) + 8, recs[c].tag);
			reached = recs[c].reached && vi == 2u;
		}
		info(r);
		CHECK((r[FR_INFO_O_SLOT(0) + FR_INFO_S_FLAGS] & FR_SLOT_F_GOOD) == 0u);
		CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_SPOILED) != 0u);
		CHECK(((r[FR_INFO_O_FLAGS] & FR_INFO_F_HELD) != 0u) == !reached);
		CHECK(((r[FR_INFO_O_FLAGS] & FR_INFO_F_READ_ERROR) != 0u) == read_err);
		CHECK(fr_rec_reach(&st) == (reached ? FR_REACH_FALLBACK : FR_REACH_NONE));
		CHECK(fr_proto_hold(&proto) == !reached);
		CHECK(status_of(FR_T_RESTART, NULL, 0) == (reached ? FR_WR_OK : FR_ST_DAMAGED));
		fail_read_always_at = UINT32_MAX;
	}
}

/* refresh(): reads only in the rescue and read-error states, never during
 * a write, and a bounded number per INFO */
/* The idle hook: called between a survey's passes (S-02.084: the links
 * served meanwhile). */
static unsigned idle_calls, idle_reads_at[8];

static void idle(void *ctx)
{
	(void)ctx;
	if (idle_calls < ARRAY_LEN(idle_reads_at))
		idle_reads_at[idle_calls] = n_reads;
	idle_calls++;
}

/*
 * fr_proto_tick(): after a read that failed, and not during a write, the
 * selector and both slots are read again, with the idle hook before each
 * slot's passes and after them; with no read error, or during a write,
 * nothing is read. A request's survey calls the hook the same way.
 */
static void test_tick(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;
	size_t n = make_image(img_b, 0x10000, 9, true);
	unsigned before;

	sha(img_b, n, h);
	/* the damaged state with the selector unreadable once in the re-read */
	start(0, 1, true);
	fail_other();
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	fail_erase_at = FR_SLOT_BASE(0) + 10u * FR_SECTOR;
	erase_then_fail = true;
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	fail_read_once_at = FR_SELECTOR_OFF;
	poll_to_end();
	CHECK(proto.phase == FR_PH_FAILED && st.read_error && fr_proto_hold(&proto));
	fail_erase_at = UINT32_MAX;
	proto.idle = idle;
	proto.idle_ctx = NULL;
	/* the tick reads again, the hook between the passes */
	idle_calls = 0;
	before = n_reads;
	fr_proto_tick(&proto);
	CHECK(n_reads > before);
	CHECK(!st.read_error && st.selector_ok);
	CHECK(idle_calls == 3u);
	CHECK(idle_reads_at[0] > before && idle_reads_at[1] > idle_reads_at[0] &&
	      idle_reads_at[2] > idle_reads_at[1]);
	/* no read error: nothing read */
	before = n_reads;
	fr_proto_tick(&proto);
	CHECK(n_reads == before && idle_calls == 3u);
	/* a request's survey in the damaged state: the hook three times */
	idle_calls = 0;
	info(r);
	CHECK(idle_calls == 3u);
	/* during a write: nothing read, whatever the state says */
	n = make_image(img_b, 0x10000, 13, true);
	sha(img_b, n, h);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	/* the damaged state holds the power-off: no countdown */
	CHECK(proto.phase == FR_PH_WRITING);
	st.read_error = true;
	idle_calls = 0;
	before = n_reads;
	fr_proto_tick(&proto);
	CHECK(n_reads == before && idle_calls == 0u);
	CHECK(proto.phase == FR_PH_WRITING);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE);
}

static void test_refresh_reads(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;
	size_t n;
	unsigned before;

	/* outside them: INFO, BEGIN, RESTART read nothing */
	start(0, 1, true);
	n = make_image(img_b, 0x10000, 9, true);
	sha(img_b, n, h);
	before = n_reads;
	info(r);
	CHECK(n_reads == before);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	CHECK(n_reads == before);
	CHECK(send_all(img_b, n));
	CHECK(n_reads == before);
	/* during a write: INFO reads nothing beyond the write's own reads */
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	{
		struct fr_rec_status copy = st;

		CHECK(ask(FR_T_INFO, NULL, 0, r) == (int)FR_INFO_LEN);
		/* only the writer's steps read; st untouched by a refresh */
		CHECK(memcmp(&copy.slot, &st.slot, sizeof copy.slot) == 0);
	}
	run_to_end(r);
	/* in the rescue state: each INFO re-reads, within a bound (the
	 * selector, two records, two CRC passes and two stream scans) */
	start(0, 1, true);
	fail_other();
	n = make_image(img_b, 0x400, 9, true);
	fail_erase_at = FR_SLOT_BASE(0);
	erase_then_fail = true;
	CHECK(write_img(img_b, n, FR_BEGIN_F_RUNNING, r) == FR_WR_OK);
	fail_erase_at = UINT32_MAX;
	before = n_reads;
	info(r);
	CHECK(n_reads > before);
	CHECK(n_reads - before <= 2u * (1u + 1u + FR_SLOT_CAP / FR_PAGE + 64u) + 1u);
	before = n_reads;
	CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_ST_DAMAGED);
	CHECK(n_reads > before);
}

static void test_info_ways(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;
	size_t n;

	n = make_image(img_b, 0x100, 3, true);
	sha(img_b, n, h);
	/* refused starts: way 0 and the refusal, for both requests */
	start(0, 0, true);
	info(r);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_NONE && r[FR_INFO_O_RUN_REFUSAL] == FR_WR_UNVERIFIED);
	CHECK(r[FR_INFO_O_TARGET] == FR_NO_SLOT && r[FR_INFO_O_REFUSAL] == FR_WR_UNVERIFIED);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_UNVERIFIED);
	start(0, 1, false);
	info(r);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_NONE && r[FR_INFO_O_RUN_REFUSAL] == FR_WR_NOT_RUNNING);
	/* slot 0 bad with its front intact (a main byte changed), slot 1 good:
	 * both requests go; with its front changed: status 5 for both */
	start_b_good(0, 1);
	flash[FR_SLOT_BASE(0) + len_a - 40u] ^= 0x01u;
	survey();
	info(r);
	CHECK((r[FR_INFO_O_SLOT(0) + FR_INFO_S_FLAGS] & FR_SLOT_F_GOOD) == 0u);
	CHECK(r[FR_INFO_O_TARGET] == 1u && r[FR_INFO_O_REFUSAL] == FR_WR_OK);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_SELECT_FIRST);
	start_b_good(0, 1);
	flash[FR_SLOT_BASE(0) + 100] ^= 0x01u;		/* a front block */
	survey();
	info(r);
	CHECK(r[FR_INFO_O_TARGET] == FR_NO_SLOT && r[FR_INFO_O_REFUSAL] == FR_WR_SELECTED_BAD);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_NONE && r[FR_INFO_O_RUN_REFUSAL] == FR_WR_SELECTED_BAD);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_SELECTED_BAD);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_SELECTED_BAD);
	CHECK(n_fops == 0u);
	/* the way of the write under way, then of the last one; a refused END
	 * leaves none */
	start(0, 1, true);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	info(r);
	CHECK(r[FR_INFO_O_WAY] == FR_WAY_NONE);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] != FR_PH_WRITING || r[FR_INFO_O_WAY] == FR_WAY_PLAIN);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WAY] == FR_WAY_PLAIN);
	start(0, 1, true);
	sha(img_a, len_a, h);
	CHECK(begin_f((uint32_t)len_a, h, 0) == FR_WR_OK);
	CHECK(send_all(img_a, len_a));
	/* the running slot's own image goes to the other slot as any other:
	 * two slots may hold the same image (S-02.084) */
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WAY] == FR_WAY_PLAIN);
	CHECK(r[FR_INFO_O_WRITTEN] == 1u);
	CHECK(memcmp(flash + FR_SLOT_BASE(1), img_a, len_a) == 0);
	CHECK(memcmp(flash + FR_SLOT_BASE(0), img_a, len_a) == 0);
	CHECK(flash[FR_SELECTOR_OFF] == 1u && flash[FR_SELECTOR_OFF + 1] == 0u);
}

/* --- S-02.093: the trial (BEGIN flag bit 1) and the select (bit 2) --- */

/* Every flags byte with more than one of the three bits, or another bit
 * but bit 7 (force): malformed, nothing changed, nothing written. */
static void test_trial_flags(void)
{
	uint8_t h[FR_SHA256_LEN], r[300];
	size_t n;
	unsigned bad = 0;

	start(0, 1, true);
	n = make_image(img_b, 0x100, 3, true);
	sha(img_b, n, h);
	for (unsigned v = 0; v < 256u; v++) {
		const uint32_t changes = proto.changes;

		const unsigned m = v & ~(unsigned)FR_BEGIN_F_FORCE;

		if (m == 0u || m == FR_BEGIN_F_RUNNING || m == FR_BEGIN_F_TRIAL || m == FR_BEGIN_F_SELECT)
			continue;
		CHECK(begin_f((uint32_t)n, h, (uint8_t)v) == FR_ST_BAD_REQUEST);
		CHECK(proto.phase == FR_PH_IDLE && proto.changes == changes && proto.len == 0u);
		bad++;
	}
	CHECK(bad == 248u);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_IDLE);
	CHECK(n_fops == 0u);
}

/* The factory first stage's start on the flash (S-02.020-S-02.023): with
 * verified not 0x01 the named slot is checked, the selector erased and
 * programmed {index, 01} or, when the check fails, {other, 01}. */
static void first_stage(void)
{
	const uint8_t idx = flash[FR_SELECTOR_OFF];
	const unsigned k = fr_selected_slot(idx);
	struct fr_record rec;
	bool pass = false;

	if (flash[FR_SELECTOR_OFF + 1] == 0x01u)
		return;
	CHECK(fr_read_record(&fl, k, &rec) == FR_FLASH_OK);
	/* S-02.026, S-16.126: the factory check of the tag with a length of
	 * 2^31 or more is unspecified; no write may lead a start there */
	CHECK(!(rec.tag == FR_TAG && rec.length >= 0x80000000u));
	if (rec.tag == FR_TAG && rec.length <= FR_SLOT_CAP)
		pass = fr_crc32(0, flash + FR_SLOT_BASE(k), rec.length) == rec.crc32;
	memset(flash + FR_SELECTOR_OFF, 0xFF, FR_SECTOR);
	flash[FR_SELECTOR_OFF] = pass ? idx : (idx == 0x00u ? 0x01u : 0x00u);
	flash[FR_SELECTOR_OFF + 1] = 0x01u;
}

/* A new start of recovery, from slot k: home cleared, the flash surveyed. */
static void restart_as(unsigned k)
{
	memset(&st, 0, sizeof st);
	st.why = FR_WHY_KEYS;
	st.running.established = true;
	st.running.slot = (uint8_t)k;
	survey();
	st.own_desc = st.slot[k].has_desc;
	st.desc = st.slot[k].desc;
	fr_proto_init(&proto, &port, &st, &fl, staging);
	proto.battery = battery;
	fr_proto_clock(&proto, g_now);
	n_fops = n_erase = 0;
}

/* img_t: an image whose front is not the running one (front seed 2). */
static uint8_t img_t[0x2000];
static size_t len_t;

/* The running slot a (A, slot 0 from start(), slot 1 from start_run1()),
 * the other slot bad; the trial of img_t through the protocol: BEGIN
 * announces it, END takes it, it ends done. */
static void trial_of(unsigned a)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], br[16], rc_img;
	const unsigned b = 1u - a;
	unsigned e;

	if (a == 0u)
		start(0, 1, true);
	else
		start_run1();
	g_fseed = 2;
	len_t = make_image(img_t, 0x300, 13, true);
	g_fseed = 1;
	sha(img_t, len_t, h);
	CHECK(begin_reply((uint32_t)len_t, h, FR_BEGIN_F_TRIAL, br) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_TRIAL && br[2] == b);
	CHECK(send_all(img_t, len_t));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_RESULT] == FR_WR_OK);
	CHECK(r[FR_INFO_O_WAY] == FR_WAY_TRIAL && r[FR_INFO_O_WRITTEN] == b);
	CHECK(memcmp(r + FR_INFO_O_READBACK, h, FR_SHA256_LEN) == 0);
	/* B: the image, its record with the trial mark; INFO says it is tried */
	CHECK(memcmp(flash + FR_SLOT_BASE(b), img_t, len_t) == 0);
	CHECK(fr_le32(flash + FR_RECORD_OFF(b)) == ~fr_crc32(0, img_t, len_t));
	CHECK(fr_le32(flash + FR_RECORD_OFF(b) + 4) == len_t);
	CHECK(fr_le32(flash + FR_RECORD_OFF(b) + 8) == FR_TAG);
	CHECK(r[FR_INFO_O_SLOT(b) + FR_INFO_S_FLAGS] ==
	      (FR_SLOT_F_TRIED | FR_SLOT_F_HAS_DESC | FR_SLOT_F_HAS_FRONT));
	CHECK((r[FR_INFO_O_SLOT(a) + FR_INFO_S_FLAGS] & FR_SLOT_F_TRIED) == 0u);
	/* nothing of A, its record or its log */
	CHECK(!any_fop(SLOT_LO(a), SLOT_HI(a)) && !any_fop(REC_LO(a), REC_HI(a)));
	/* B's log: erased, then entry 0 pending, before B's record */
	e = first_fop('p', FR_LOG_OFF(b), FR_LOG_OFF(b) + FR_SECTOR);
	CHECK(e != ~0u && fops[e].off == FR_LOG_OFF(b));
	CHECK(e > first_fop('e', FR_LOG_OFF(b), FR_LOG_OFF(b) + FR_SECTOR));
	CHECK(e < first_fop('e', REC_LO(b), REC_HI(b)));
	CHECK(flash[FR_LOG_OFF(b)] == 0xFDu);
	for (unsigned j = 1; j < FR_SECTOR; j++)
		if (flash[FR_LOG_OFF(b) + j] != 0xFFu) {
			CHECK(0);
			break;
		}
	CHECK(st.home.trial);
	/* the selector after the read-back: erased, then for slot 0 verified
	 * 0xFE and the index 0x00, a byte at a time */
	e = first_fop('e', SEL_LO, SEL_HI);
	CHECK(e != ~0u && e > first_fop('p', REC_LO(b), REC_HI(b)));
	if (b == 1u) {
		CHECK(e == n_fops - 1u);
		CHECK(flash[FR_SELECTOR_OFF] == 0xFFu && flash[FR_SELECTOR_OFF + 1] == 0xFFu);
	} else {
		CHECK(e + 3u == n_fops);
		CHECK(fops[e + 1u].kind == 'p' && fops[e + 1u].off == FR_SELECTOR_OFF + 1u);
		CHECK(fops[e + 2u].kind == 'p' && fops[e + 2u].off == FR_SELECTOR_OFF);
		CHECK(flash[FR_SELECTOR_OFF] == 0x00u && flash[FR_SELECTOR_OFF + 1] == 0xFEu);
	}
	CHECK(page_faults == 0u);
	/* the selector no longer names A with verified 01: refused by the
	 * start condition; the select, after a trial in this recovery, 19 */
	CHECK(r[FR_INFO_O_TARGET] == FR_NO_SLOT && r[FR_INFO_O_REFUSAL] == FR_WR_NOT_SELECTED);
	n_fops = 0;
	CHECK(begin_f((uint32_t)len_t, h, FR_BEGIN_F_TRIAL) == FR_WR_NOT_SELECTED);
	CHECK(begin_f((uint32_t)len_t, h, FR_BEGIN_F_SELECT) == FR_WR_TRIAL_STATE);
	CHECK(begin_f((uint32_t)len_t, h, 0) == FR_WR_NOT_SELECTED);
	CHECK(n_fops == 0u);
}

/* The trial for each slot order, then the select from the old front after
 * the trial start has left its mark (21 before it), and from the tried
 * slot's own front during it, which makes the mark itself. */
static void test_trial_select(void)
{
	for (unsigned a = 0; a < 2u; a++)
		for (unsigned from_b = 0; from_b < 2u; from_b++) {
			uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], br[16], rc_img;
			const unsigned b = 1u - a;
			const unsigned sel = from_b ? b : a;	/* the recovery that selects */
			unsigned i;

			trial_of(a);
			/* the trial start: B's check fails, {A, 01}, B boots once */
			first_stage();
			CHECK(fr_selected_slot(flash[FR_SELECTOR_OFF]) == a &&
			      flash[FR_SELECTOR_OFF + 1] == 0x01u);
			restart_as(sel);
			info(r);
			CHECK((r[FR_INFO_O_SLOT(b) + FR_INFO_S_FLAGS] & (FR_SLOT_F_TRIED | FR_SLOT_F_STARTED)) ==
			      FR_SLOT_F_TRIED);
			if (!from_b) {
				/* no mark yet: 21, nothing written */
				CHECK(st.select_target == FR_NO_SLOT &&
				      st.select_refusal == FR_WR_NOT_STARTED);
				sha(img_t, len_t, h);
				CHECK(begin_reply((uint32_t)len_t, h, FR_BEGIN_F_SELECT, br) ==
				      (int)FR_BEGIN_REPLY_LEN);
				CHECK(br[0] == FR_WR_NOT_STARTED && br[1] == FR_WAY_NONE &&
				      br[2] == FR_NO_SLOT);
				CHECK(n_fops == 0u);
				/* the tried slot's front at its trial start
				 * (fr_log_trial_mark()): entry 0 finished */
				flash[FR_LOG_OFF(b)] &= 0xFEu;
				restart_as(sel);
				info(r);
				CHECK((r[FR_INFO_O_SLOT(b) + FR_INFO_S_FLAGS] &
				       (FR_SLOT_F_TRIED | FR_SLOT_F_STARTED)) ==
				      (FR_SLOT_F_TRIED | FR_SLOT_F_STARTED));
			}
			if (from_b) {
				/* the trial start: no write of any other kind */
				CHECK(r[FR_INFO_O_TARGET] == FR_NO_SLOT &&
				      r[FR_INFO_O_REFUSAL] == FR_WR_NOT_SELECTED);
				CHECK(st.select_target == b && st.select_refusal == FR_WR_OK);
				sha(img_t, len_t, h);
				CHECK(begin_f((uint32_t)len_t, h, 0) == FR_WR_NOT_SELECTED);
				CHECK(begin_f((uint32_t)len_t, h, FR_BEGIN_F_RUNNING) == FR_WR_NOT_SELECTED);
				CHECK(begin_f((uint32_t)len_t, h, FR_BEGIN_F_TRIAL) == FR_WR_NOT_SELECTED);
			} else {
				CHECK(r[FR_INFO_O_TARGET] == b && r[FR_INFO_O_REFUSAL] == FR_WR_OK);
				CHECK(st.select_target == b && st.trial);
			}
			/* another image announced: refused at END, 20, nothing written */
			len_c = make_image(img_c, 0x200, 7, true);
			sha(img_c, len_c, h);
			CHECK(begin_reply((uint32_t)len_c, h, FR_BEGIN_F_SELECT, br) == (int)FR_BEGIN_REPLY_LEN);
			CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_SELECT && br[2] == b);
			CHECK(send_all(img_c, len_c));
			CHECK(end_rc(&rc_img) == FR_WR_NOT_TRIED && rc_img == FR_IMG_OK);
			CHECK(n_fops == 0u);
			/* the image the slot holds: its record made right, then the
			 * selector {B, 00} */
			sha(img_t, len_t, h);
			CHECK(begin_reply((uint32_t)len_t, h, FR_BEGIN_F_SELECT, br) == (int)FR_BEGIN_REPLY_LEN);
			CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_SELECT && br[2] == b);
			CHECK(send_all(img_t, len_t));
			CHECK(end_rc(&rc_img) == FR_WR_OK);
			run_to_end(r);
			CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_RESULT] == FR_WR_OK);
			CHECK(r[FR_INFO_O_WAY] == FR_WAY_SELECT && r[FR_INFO_O_WRITTEN] == b);
			CHECK(memcmp(r + FR_INFO_O_READBACK, h, FR_SHA256_LEN) == 0);
			CHECK(r[FR_INFO_O_SLOT(b) + FR_INFO_S_FLAGS] ==
			      (FR_SLOT_F_GOOD | FR_SLOT_F_HAS_DESC | FR_SLOT_F_HAS_FRONT));
			CHECK(fr_le32(flash + FR_RECORD_OFF(b)) == fr_crc32(0, img_t, len_t));
			CHECK(memcmp(flash + FR_SLOT_BASE(b), img_t, len_t) == 0);
			CHECK(flash[FR_SELECTOR_OFF] == b && flash[FR_SELECTOR_OFF + 1] == 0x00u);
			/* the operations: from B's own front its log's entry 0
			 * marked; B's record erase and program, the selector erase
			 * and program; nothing else in a slot or log sector */
			i = from_b ? 1u : 0u;
			CHECK(n_fops == 4u + i);
			if (from_b)
				CHECK(fops[0].kind == 'p' && fops[0].off == FR_LOG_OFF(b));
			CHECK(fops[i].kind == 'e' && fops[i].off == FR_RECORD_OFF(b));
			CHECK(fops[i + 1u].kind == 'p' && fops[i + 1u].off == FR_RECORD_OFF(b));
			CHECK(fops[i + 2u].kind == 'e' && fops[i + 2u].off == FR_SELECTOR_OFF);
			CHECK(fops[i + 3u].kind == 'p' && fops[i + 3u].off == FR_SELECTOR_OFF);
			CHECK(!any_fop(SLOT_LO(a), SLOT_HI(a)));
			CHECK(!any_fop(SLOT_LO(b), FR_LOG_OFF(b)));
			CHECK(flash[FR_LOG_OFF(b)] == 0xFCu);
			CHECK(page_faults == 0u);
			/* the next start checks B and trusts it */
			first_stage();
			CHECK(flash[FR_SELECTOR_OFF] == b && flash[FR_SELECTOR_OFF + 1] == 0x01u);
		}
}

/* Statuses 12, 19, 20 and 16 with the new flags; every refusal writes
 * nothing. */
static void test_trial_refusals(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], br[16], rc_img;
	unsigned main_byte;

	g_fseed = 2;
	len_t = make_image(img_t, 0x300, 13, true);
	g_fseed = 1;
	sha(img_t, len_t, h);

	/* 12: another front with bit 0, or with no flag */
	start_b_good(0, 1);
	CHECK(write_img(img_t, len_t, FR_BEGIN_F_RUNNING, r) == FR_WR_FRONT);
	CHECK(write_img(img_t, len_t, 0, r) == FR_WR_FRONT);
	CHECK(n_fops == 0u);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_FRONT);

	/* 19: the running slot bad (a main byte, its front intact): the
	 * trial is not announced, and END refuses it */
	start_b_good(0, 1);
	main_byte = (unsigned)len_a - 40u;
	flash[FR_SLOT_BASE(0) + main_byte] ^= 0x01u;
	survey();
	CHECK(!st.slot[0].good && st.slot[0].has_front && !st.trial);
	CHECK(begin_reply((uint32_t)len_t, h, FR_BEGIN_F_TRIAL, br) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_PLAIN && br[2] == 1u);
	CHECK(send_all(img_t, len_t));
	CHECK(end_rc(&rc_img) == FR_WR_TRIAL_STATE);
	CHECK(n_fops == 0u);
	/* 16: the slot made good between BEGIN and END: END would make a
	 * trial where BEGIN announced a plain write */
	CHECK(begin_reply((uint32_t)len_t, h, FR_BEGIN_F_TRIAL, br) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_PLAIN);
	CHECK(send_all(img_t, len_t));
	flash[FR_SLOT_BASE(0) + main_byte] ^= 0x01u;
	CHECK(end_rc(&rc_img) == FR_WR_CHANGED);
	CHECK(n_fops == 0u);
	/* the state read again: the trial announced now, and taken */
	info(r);
	CHECK(st.trial);
	CHECK(begin_reply((uint32_t)len_t, h, FR_BEGIN_F_TRIAL, br) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_TRIAL && br[2] == 1u);

	/* 19 in the damaged state: a write of A selected first, its slot
	 * erase taken and failed: the selector {01 01}, A bad. Another front:
	 * 12 without the flag, 19 with it; a select 19 at BEGIN. */
	start_b_good(0, 1);
	fail_erase_at = FR_SLOT_BASE(0);
	erase_then_fail = true;
	{
		const size_t nb = make_image(img_b, 0x400, 9, true);

		CHECK(write_img(img_b, nb, FR_BEGIN_F_RUNNING, r) == FR_WR_OK);
	}
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && st.home.moved && st.home.spoiled);
	fail_erase_at = UINT32_MAX;
	erase_then_fail = false;
	n_fops = 0;
	CHECK(write_img(img_t, len_t, 0, r) == FR_WR_FRONT);
	CHECK(write_img(img_t, len_t, FR_BEGIN_F_TRIAL, r) == FR_WR_TRIAL_STATE);
	CHECK(begin_f((uint32_t)len_t, h, FR_BEGIN_F_SELECT) == FR_WR_TRIAL_STATE);
	CHECK(n_fops == 0u);

	/* 20 at BEGIN: no slot tried */
	start_b_good(0, 1);
	CHECK(st.select_refusal == FR_WR_NOT_TRIED);
	CHECK(begin_reply((uint32_t)len_t, h, FR_BEGIN_F_SELECT, br) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(br[0] == FR_WR_NOT_TRIED && br[1] == FR_WAY_NONE && br[2] == FR_NO_SLOT);
	/* 2: the running slot not known */
	start(0, 1, false);
	CHECK(begin_f((uint32_t)len_t, h, FR_BEGIN_F_SELECT) == FR_WR_NOT_RUNNING);
	CHECK(begin_f((uint32_t)len_t, h, FR_BEGIN_F_TRIAL) == FR_WR_NOT_RUNNING);
	CHECK(n_fops == 0u);

	/* the flag with the running front: a trial was announced, and the
	 * image would be written and selected at once: END refuses, and
	 * nothing is written */
	start(0, 1, true);
	len_c = make_image(img_c, 0x200, 7, true);
	sha(img_c, len_c, h);
	CHECK(begin_reply((uint32_t)len_c, h, FR_BEGIN_F_TRIAL, br) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_TRIAL && br[2] == 1u);
	CHECK(send_all(img_c, len_c));
	n_fops = 0;
	CHECK(end_rc(&rc_img) == FR_WR_CHANGED);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_CHANGED);
	CHECK(n_fops == 0u);
	CHECK(flash[FR_SELECTOR_OFF] == 0u && flash[FR_SELECTOR_OFF + 1] == 0x01u);
}

/*
 * Revision 2 of S-02.093: a selector step of the trial that fails settles
 * the selector back on A ({00 01}, A = slot 0) and ends 11, home->trial
 * stands (a select is 19); after a reset into A's recovery the select is
 * 21, and the tried slot's own front's mark makes it go. The order of a
 * select's refusals at END: another image is 20 even where B has not
 * started.
 */
static void test_trial_settle_back(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN];

	start(0, 1, true);
	g_fseed = 2;
	len_t = make_image(img_t, 0x300, 13, true);
	g_fseed = 1;
	sha(img_t, len_t, h);
	fail_erase_at = FR_SELECTOR_OFF;
	erase_then_fail = true;
	CHECK(write_img(img_t, len_t, FR_BEGIN_F_TRIAL, r) == FR_WR_OK);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_SELECTOR);
	fail_erase_at = UINT32_MAX;
	erase_then_fail = false;
	/* erased to FF FF, then the index and verified programmed alone */
	CHECK(flash[FR_SELECTOR_OFF] == 0x00u && flash[FR_SELECTOR_OFF + 1] == 0x01u);
	CHECK(fops[n_fops - 2u].kind == 'p' && fops[n_fops - 2u].off == FR_SELECTOR_OFF);
	CHECK(fops[n_fops - 1u].kind == 'p' && fops[n_fops - 1u].off == FR_SELECTOR_OFF + 1u);
	CHECK(st.home.trial && flash[FR_LOG_OFF(1)] == 0xFDu);
	CHECK((r[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] & (FR_SLOT_F_TRIED | FR_SLOT_F_STARTED)) ==
	      FR_SLOT_F_TRIED);
	n_fops = 0;
	CHECK(begin_f((uint32_t)len_t, h, FR_BEGIN_F_SELECT) == FR_WR_TRIAL_STATE);
	/* a reset into A's recovery: no trial start ran, 21 */
	restart_as(0);
	CHECK(st.select_refusal == FR_WR_NOT_STARTED);
	CHECK(begin_f((uint32_t)len_t, h, FR_BEGIN_F_SELECT) == FR_WR_NOT_STARTED);
	CHECK(n_fops == 0u);
	/* the mark, then a select of another image: 20 at END */
	flash[FR_LOG_OFF(1)] &= 0xFEu;
	restart_as(0);
	info(r);
	CHECK((r[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] & FR_SLOT_F_STARTED) != 0u);
	len_c = make_image(img_c, 0x200, 7, true);
	CHECK(write_img(img_c, len_c, FR_BEGIN_F_SELECT, r) == FR_WR_NOT_TRIED);
	CHECK(write_img(img_t, len_t, FR_BEGIN_F_SELECT, r) == FR_WR_OK);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WAY] == FR_WAY_SELECT);
	CHECK(flash[FR_SELECTOR_OFF] == 0x01u && flash[FR_SELECTOR_OFF + 1] == 0x00u);
}

/* A trial a start of the old slot spent (byte 0 F9): from the old front
 * 21; from the tried slot's own front 20, its log untouched. */
static void test_trial_spent(void)
{
	for (unsigned a = 0; a < 2u; a++) {
		uint8_t r[300] = { 0 }, h[FR_SHA256_LEN];
		const unsigned b = 1u - a;

		trial_of(a);
		first_stage();
		flash[FR_LOG_OFF(b)] = 0xF9u;
		sha(img_t, len_t, h);
		restart_as(a);
		info(r);
		CHECK((r[FR_INFO_O_SLOT(b) + FR_INFO_S_FLAGS] & (FR_SLOT_F_TRIED | FR_SLOT_F_STARTED)) ==
		      FR_SLOT_F_TRIED);
		CHECK(st.slot[b].spent && st.select_refusal == FR_WR_NOT_STARTED);
		CHECK(begin_f((uint32_t)len_t, h, FR_BEGIN_F_SELECT) == FR_WR_NOT_STARTED);
		restart_as(b);
		CHECK(st.select_target == FR_NO_SLOT && st.select_refusal == FR_WR_NOT_TRIED);
		CHECK(begin_f((uint32_t)len_t, h, FR_BEGIN_F_SELECT) == FR_WR_NOT_TRIED);
		CHECK(n_fops == 0u && flash[FR_LOG_OFF(b)] == 0xF9u);
	}
}

/* --- What a survey announces is what the writer plans --- */

/* A slot's states: good with a front or without; bad by its CRC, with a
 * front or without; erased; the image with no record, an empty record or
 * one longer than the slot; tried, its log's byte 0 erased, pending,
 * marked or spent. */
enum { SK_GOOD, SK_GOOD_NOFRONT, SK_BAD, SK_BAD_NOFRONT, SK_ERASED, SK_TORN, SK_EMPTY,
       SK_LONG, SK_TRIED, SK_TRIED_PENDING, SK_TRIED_MARKED, SK_TRIED_SPENT, SK_N };

static uint8_t img_pf[0x2000], img_pn[0x2000], img_pt[0x2000];
static size_t len_pf, len_pn, len_pt;

static void put_record(unsigned k, uint32_t crc, uint32_t length)
{
	memset(flash + FR_RECORD_OFF(k), 0xFF, FR_SECTOR);
	fr_put32(flash + FR_RECORD_OFF(k), crc);
	fr_put32(flash + FR_RECORD_OFF(k) + 4, length);
	fr_put32(flash + FR_RECORD_OFF(k) + 8, FR_TAG);
}

static void put_kind(unsigned k, unsigned kind)
{
	static const uint8_t log0[4] = { 0xFF, FR_LOG0_PENDING, FR_LOG0_MARKED, 0xF9 };
	const bool nofront = kind == SK_GOOD_NOFRONT || kind == SK_BAD_NOFRONT;
	const uint8_t *img = kind >= SK_TRIED ? img_pt : nofront ? img_pn : img_pf;
	const size_t n = kind >= SK_TRIED ? len_pt : nofront ? len_pn : len_pf;

	put_slot(k, img, n);
	switch (kind) {
	case SK_BAD:
	case SK_BAD_NOFRONT:
		flash[FR_SLOT_BASE(k) + n - 40u] ^= 0x01u;	/* main's zero bytes */
		break;
	case SK_ERASED:
		memset(flash + FR_SLOT_BASE(k), 0xFF, FR_SLOT_CAP);
		memset(flash + FR_RECORD_OFF(k), 0xFF, FR_SECTOR);
		break;
	case SK_TORN:
		memset(flash + FR_RECORD_OFF(k), 0xFF, FR_SECTOR);
		break;
	case SK_EMPTY:
		put_record(k, fr_crc32(0, img, n), 0);
		break;
	case SK_LONG:
		put_record(k, fr_crc32(0, img, n), FR_SLOT_CAP + 1u);
		break;
	case SK_TRIED:
	case SK_TRIED_PENDING:
	case SK_TRIED_MARKED:
	case SK_TRIED_SPENT:
		put_record(k, ~fr_crc32(0, img, n), (uint32_t)n);
		flash[FR_LOG_OFF(k)] = log0[kind - SK_TRIED];
		break;
	default:
		break;
	}
}

/* The writer's plan for a BEGIN with these flags against what the survey
 * in st announced for it. A read that failed may refuse the writer alone. */
static void check_plan(uint8_t flags, bool reads_fail)
{
	struct fr_plan p;
	struct fr_desc front;
	uint8_t refusal, way, target;
	const int rc = fr_writer_plan(&fl, &st.running, &st.home, flags, &p, &front);

	if (flags & FR_BEGIN_F_RUNNING) {
		refusal = st.run_refusal;
		way = st.run_way;
		target = way != FR_WAY_NONE ? st.running.slot : (uint8_t)FR_NO_SLOT;
	} else if (flags & FR_BEGIN_F_SELECT) {
		refusal = st.select_refusal;
		target = st.select_target;
		way = target != FR_NO_SLOT ? (uint8_t)FR_WAY_SELECT : (uint8_t)FR_WAY_NONE;
	} else {
		refusal = st.refusal;
		target = st.target;
		way = target != FR_NO_SLOT ? (uint8_t)FR_WAY_PLAIN : (uint8_t)FR_WAY_NONE;
	}
	if (reads_fail && rc == FR_WR_READ)
		return;
	/* A log byte that cannot be read makes the survey read its slot as
	 * bad, with read_error, while the writer reads no log byte of a slot
	 * that is not tried: the plans may differ, and END then refuses 16
	 * (test_log_read_fails()). */
	if (reads_fail && st.read_error &&
	    (fail_read_always_at == FR_LOG_OFF(0) || fail_read_always_at == FR_LOG_OFF(1)))
		return;
	CHECK(rc != FR_WR_READ || refusal == FR_WR_READ);
	CHECK(rc == refusal && p.refusal == refusal);
	CHECK(p.way == way && p.target == target);
	CHECK((refusal == FR_WR_OK) == (way != FR_WAY_NONE));
	if (!(flags & FR_BEGIN_F_SELECT))
		CHECK(p.rescue == fr_rec_rescue(&st));
	if (!(flags & (FR_BEGIN_F_RUNNING | FR_BEGIN_F_SELECT)))
		CHECK(p.trial_ok == st.trial);
}

/* Every flash state of the kinds above, both slots running and none, the
 * selector naming either slot with several verified bytes, and home's
 * bits; with the reads of fail failing (or none). */
static void plan_states(uint32_t fail, const uint8_t *vs, unsigned nv, const uint8_t *homes,
			unsigned nh)
{
	static const uint8_t flagset[4] = {
		0, FR_BEGIN_F_RUNNING, FR_BEGIN_F_TRIAL, FR_BEGIN_F_SELECT
	};

	for (unsigned r = 0; r < 3u; r++)
		for (unsigned k0 = 0; k0 < SK_N; k0++)
			for (unsigned k1 = 0; k1 < SK_N; k1++) {
				memset(flash, 0xFF, sizeof flash);
				put_kind(0, k0);
				put_kind(1, k1);
				for (unsigned c = 0; c < 2u * nv; c++) {
					flash[FR_SELECTOR_OFF] = (uint8_t)(c & 1u);
					flash[FR_SELECTOR_OFF + 1] = vs[c >> 1];
					memset(&st, 0, sizeof st);
					st.running.established = r < 2u;
					st.running.slot = r < 2u ? (uint8_t)r : (uint8_t)FR_NO_SLOT;
					fail_read_always_at = fail;
					fr_rec_survey(&st, &fl);
					CHECK(fail != UINT32_MAX || !st.read_error);
					for (unsigned h = 0; h < nh; h++) {
						st.home.moved = (homes[h] & 1u) != 0u;
						st.home.spoiled = (homes[h] & 2u) != 0u;
						st.home.other_failed = (homes[h] & 4u) != 0u;
						st.home.unsettled = (homes[h] & 8u) != 0u;
						st.home.trial = (homes[h] & 16u) != 0u;
						memcpy(st.home.pin, st.slot[r & 1u].desc.pin,
						       FR_SHA256_LEN);
						st.home.front_crc = st.slot[r & 1u].desc.front_crc;
						fr_rec_target(&st);
						for (unsigned f = 0; f < 4u; f++)
							check_plan(flagset[f], fail != UINT32_MAX);
					}
				}
			}
	fail_read_always_at = UINT32_MAX;
}

/*
 * S-02.093: BEGIN announces what END's writer does. Over every state
 * above, what a survey announces for each request (the target or refusal,
 * the way of a write of the running slot, a trial, the select) is what
 * fr_writer_plan() decides from the same flash; with a read failing, the
 * writer may refuse 17 where the survey reads that slot as bad.
 */
static void test_plan_agrees(void)
{
	static const uint8_t vs[] = { 0x00, 0x01, 0x02, 0x03, 0xFE, 0xFF };
	static const uint8_t vs_fail[] = { 0x00, 0x01, 0xFF };
	static const uint8_t homes_fail[] = { 0, 1, 2, 4, 2 | 4, 2 | 8, 16, 1 | 2 };
	static uint8_t homes[32];
	const uint32_t fails[] = {
		FR_SELECTOR_OFF, FR_RECORD_OFF(0), FR_RECORD_OFF(1), FR_SLOT_BASE(0),
		FR_SLOT_BASE(1), FR_LOG_OFF(0), FR_LOG_OFF(1)
	};

	for (unsigned h = 0; h < 32u; h++)
		homes[h] = (uint8_t)h;
	len_pf = make_image(img_pf, 0x400, 3, true);
	len_pn = make_image(img_pn, 0x400, 4, false);
	g_fseed = 2;
	len_pt = make_image(img_pt, 0x400, 5, true);
	g_fseed = 1;
	plan_states(UINT32_MAX, vs, sizeof vs, homes, sizeof homes);
	for (unsigned i = 0; i < sizeof fails / sizeof fails[0]; i++)
		plan_states(fails[i], vs_fail, sizeof vs_fail, homes_fail, sizeof homes_fail);
}

/* --- version 2: the countdown, ACT, the force flag, INFO's new fields --- */

/* ACT {act, flags}: the reply's length, and {status, way, slot, tier} in r. */
static int act_req(const uint8_t *pl, size_t n, uint8_t *r)
{
	return ask(FR_T_ACT, pl, n, r);
}

static uint8_t act_st(uint8_t act, uint8_t flags, uint8_t *r)
{
	const uint8_t pl[2] = { act, flags };

	memset(r, 0xEE, 4);
	CHECK(act_req(pl, 2, r) == (int)FR_ACT_REPLY_LEN);
	return r[0];
}

/* Slot 1's record made the trial mark of its bytes, its log byte 0 v. */
static void mark_b(unsigned b, uint8_t log0)
{
	const uint32_t n = fr_le32(flash + FR_RECORD_OFF(b) + 4);

	fr_put32(flash + FR_RECORD_OFF(b), ~fr_crc32(0, flash + FR_SLOT_BASE(b), n));
	flash[FR_LOG_OFF(b)] = log0;
}

/* S-02.084: the countdown. An accepted END gives phase 5, and INFO's
 * seconds count 15 down to 1; nothing is erased or programmed before
 * fr_proto_clock() has seen the 15 s pass, however many polls run and
 * whatever the host asks (status 1, but HELLO and INFO); the phase is
 * writing from the pass that ends it, the first erase in the next. */
static void test_countdown(void)
{
	static const uint8_t restart_force[1] = { FR_BEGIN_F_FORCE };
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;
	uint64_t t0;
	size_t n;

	start(0, 1, true);
	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	g_tick = 0;			/* the clock stands still */
	CHECK(end_rc(&rc_img) == FR_WR_OK && rc_img == FR_IMG_OK);
	t0 = g_now;
	CHECK(proto.phase == FR_PH_COUNTDOWN);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_COUNTDOWN && r[FR_INFO_O_COUNT] == 15u);
	CHECK(r[FR_INFO_O_WAY] == FR_WAY_PLAIN && r[FR_INFO_O_ACT] == FR_ACT_NONE);
	/* polls with no time: nothing */
	poll_n(5000);
	CHECK(proto.phase == FR_PH_COUNTDOWN && n_fops == 0u);
	/* every request but HELLO and INFO is busy, and none shortens it */
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_BUSY);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_FORCE) == FR_WR_BUSY);
	{
		uint8_t pl[8] = { 0 };

		CHECK(ask(FR_T_DATA, pl, sizeof pl, r) == (int)FR_DATA_REPLY_LEN && r[0] == FR_WR_BUSY);
	}
	CHECK(ask(FR_T_END, NULL, 0, r) == (int)FR_END_REPLY_LEN && r[0] == FR_WR_BUSY);
	CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_WR_BUSY);
	CHECK(status_of(FR_T_RESTART, restart_force, 1) == FR_WR_BUSY);
	for (uint8_t a = FR_ACT_TRY; a <= FR_ACT_PUT_BACK; a++) {
		CHECK(act_st(a, 0, r) == FR_WR_BUSY);
		CHECK(act_st(a, FR_BEGIN_F_FORCE, r) == FR_WR_BUSY);
	}
	CHECK(fr_proto_local(&proto, FR_ACT_SWITCH) == FR_WR_BUSY);
	CHECK(ask(FR_T_HELLO, NULL, 0, r) == (int)FR_HELLO_LEN);
	CHECK(!proto.restart && !proto.restart_due);
	CHECK(proto.phase == FR_PH_COUNTDOWN && n_fops == 0u);
	/* the seconds left, a second at a time */
	for (unsigned left = 15; left >= 1u; left--) {
		g_now = t0 + (uint64_t)(15u - left) * 1000000u;
		fr_proto_clock(&proto, g_now);
		info(r);
		CHECK(r[FR_INFO_O_PHASE] == FR_PH_COUNTDOWN && r[FR_INFO_O_COUNT] == left);
		g_now += 999999u;
		fr_proto_clock(&proto, g_now);
		info(r);
		CHECK(r[FR_INFO_O_COUNT] == (left == 1u ? 1u : left - 1u) || r[FR_INFO_O_COUNT] == left);
		CHECK(n_fops == 0u);
	}
	CHECK(proto.phase == FR_PH_COUNTDOWN && n_fops == 0u);
	/* the 15 s: writing, nothing yet; the first erase in the next poll */
	fr_proto_clock(&proto, t0 + FR_COUNTDOWN_US);
	CHECK(proto.phase == FR_PH_WRITING && n_fops == 0u);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_WRITING && r[FR_INFO_O_COUNT] == 0u);
	CHECK(n_erase >= 1u);	/* INFO's own polls made the first steps */
	g_tick = 1000000u;
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_RESULT] == FR_WR_OK);
	CHECK(r[FR_INFO_O_COUNT] == 0u);
	CHECK(memcmp(flash + FR_SLOT_BASE(1), img_b, n) == 0);
}

/* S-02.084: the host leaves in the middle of a transfer or during the
 * countdown (fr_proto_drop()): status 24 with nothing written; the next
 * BEGIN works. Before any write of this recovery the phase is failed with
 * 24; after one, that write's phase stays and 24 is the request's refusal
 * (shown as what to do). A drop with neither under way changes no phase. */
static void test_countdown_drop(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img, ar[4];
	size_t n;

	start(0, 1, true);
	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);
	fr_proto_drop(&proto);		/* idle: the phase stays */
	CHECK(proto.phase == FR_PH_IDLE);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	fr_proto_drop(&proto);		/* receiving: the transfer ends */
	CHECK(proto.phase == FR_PH_FAILED && proto.result == FR_WR_STOPPED);
	CHECK(end_rc(&rc_img) == FR_ST_STATE && n_fops == 0u);
	/* begun again */
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	CHECK(proto.phase == FR_PH_COUNTDOWN);
	poll_n(5);
	fr_proto_drop(&proto);
	CHECK(proto.phase == FR_PH_FAILED && proto.result == FR_WR_STOPPED);
	poll_n(30);
	CHECK(n_fops == 0u && !st.home.moved && !st.home.spoiled && !st.home.trial);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_STOPPED);
	CHECK(r[FR_INFO_O_WRITTEN] == FR_NO_SLOT);
	/* the next BEGIN works, and its write ends */
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 1u);
	CHECK(proto.refused == FR_WR_OK);
	/* after that write: a forced try of slot 0 (the selector {01 00}
	 * names slot 1, the one written), stopped */
	n_fops = 0;
	CHECK(act_st(FR_ACT_TRY, 0, ar) == FR_WR_NOT_SELECTED);
	CHECK(proto.phase == FR_PH_DONE && proto.refused == FR_WR_NOT_SELECTED);
	CHECK(act_st(FR_ACT_TRY, FR_BEGIN_F_FORCE, ar) == FR_WR_OK);
	CHECK(ar[1] == FR_WAY_TRY && ar[2] == 1u);
	CHECK(proto.phase == FR_PH_COUNTDOWN && proto.refused == FR_WR_OK);
	fr_proto_drop(&proto);
	poll_n(30);
	CHECK(n_fops == 0u);
	CHECK(proto.phase == FR_PH_DONE && proto.refused == FR_WR_STOPPED);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 1u);
	CHECK(r[FR_INFO_O_WAY] == FR_WAY_PLAIN && r[FR_INFO_O_RESULT] == FR_WR_OK);
	CHECK(!st.home.trial);
	/* after that write: a transfer left half sent, the write's state kept */
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_FORCE) == FR_WR_OK);
	CHECK(proto.phase == FR_PH_RECEIVING);
	fr_proto_drop(&proto);
	CHECK(proto.phase == FR_PH_DONE && proto.refused == FR_WR_STOPPED);
	CHECK(n_fops == 0u);
}

/* INFO byte 231: the status of a request refused, or of a countdown
 * stopped, since this recovery's last write, with that write's phase, way
 * and action kept; cleared when a write or an action is accepted. */
static void test_info_refused(void)
{
	uint8_t r[300] = { 0 }, ar[4], h[FR_SHA256_LEN], rc_img;
	size_t n;

	/* an earlier action: a switch to slot 1 */
	start_b_good(0, 1);
	info(r);
	CHECK(r[FR_INFO_O_REFUSED] == 0u);
	CHECK(act_st(FR_ACT_SWITCH, 0, ar) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WAY] == FR_WAY_SWITCH);
	CHECK(r[FR_INFO_O_ACT] == FR_ACT_SWITCH && r[FR_INFO_O_REFUSED] == 0u);
	/* a refused ACT: its status, the switch's state kept */
	CHECK(act_st(FR_ACT_TRY, 0, ar) == FR_WR_NOT_SELECTED);
	info(r);
	CHECK(r[FR_INFO_O_REFUSED] == FR_WR_NOT_SELECTED);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WAY] == FR_WAY_SWITCH);
	CHECK(r[FR_INFO_O_ACT] == FR_ACT_SWITCH && r[FR_INFO_O_WRITTEN] == 1u);
	CHECK(r[FR_INFO_O_RESULT] == FR_WR_OK);
	/* a refused BEGIN: its status */
	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_NOT_SELECTED);
	CHECK(begin_f(FR_IMAGE_MAX + 1u, h, FR_BEGIN_F_FORCE) == FR_WR_SIZE);
	info(r);
	CHECK(r[FR_INFO_O_REFUSED] == FR_WR_SIZE);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_ACT] == FR_ACT_SWITCH);
	/* an accepted ACT clears it; its countdown stopped: 24, the switch's
	 * state again */
	CHECK(act_st(FR_ACT_KEEP, FR_BEGIN_F_FORCE, ar) == FR_WR_NOT_TRIED);
	n_fops = 0;
	CHECK(act_st(FR_ACT_TRY, FR_BEGIN_F_FORCE, ar) == FR_WR_OK);
	info(r);
	CHECK(r[FR_INFO_O_REFUSED] == 0u && r[FR_INFO_O_PHASE] == FR_PH_COUNTDOWN);
	CHECK(r[FR_INFO_O_ACT] == FR_ACT_TRY);
	fr_proto_drop(&proto);
	info(r);
	CHECK(r[FR_INFO_O_REFUSED] == FR_WR_STOPPED);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WAY] == FR_WAY_SWITCH);
	CHECK(r[FR_INFO_O_ACT] == FR_ACT_SWITCH && r[FR_INFO_O_WRITTEN] == 1u);
	poll_n(30);
	CHECK(n_fops == 0u && flash[FR_SELECTOR_OFF] == 1u);
	/* an accepted END clears it */
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_FORCE) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	info(r);
	CHECK(r[FR_INFO_O_REFUSED] == 0u && r[FR_INFO_O_ACT] == FR_ACT_NONE);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_REFUSED] == 0u);
	/* before any write: a refusal is the phase failed, byte 231 0 */
	start(0, 0x00, true);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_UNVERIFIED);
	info(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && r[FR_INFO_O_RESULT] == FR_WR_UNVERIFIED);
	CHECK(r[FR_INFO_O_REFUSED] == 0u);
}

/* An ACT's countdown, the damaged state's exception, and the unit's own
 * actions (fr_proto_local()): no countdown, busy while a transfer, a
 * countdown or a write is under way. */
static void test_countdown_kinds(void)
{
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img, ar[4];
	size_t n;

	/* ACT: a try of slot 1, good, counted down to */
	start_b_good(0, 1);
	CHECK(act_st(FR_ACT_TRY, 0, ar) == FR_WR_OK);
	CHECK(ar[1] == FR_WAY_TRY && ar[2] == 1u && ar[3] == FR_TIER_SAME);
	CHECK(proto.phase == FR_PH_COUNTDOWN && n_fops == 0u && !st.home.trial);
	info(r);
	CHECK(r[FR_INFO_O_COUNT] >= 11u && r[FR_INFO_O_ACT] == FR_ACT_TRY);
	CHECK(r[FR_INFO_O_WAY] == FR_WAY_TRY);
	CHECK(n_fops == 0u);
	through_countdown();
	CHECK(proto.phase == FR_PH_WRITING && n_fops == 0u);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WAY] == FR_WAY_TRY);
	CHECK(r[FR_INFO_O_ACT] == FR_ACT_TRY && r[FR_INFO_O_WRITTEN] == 1u);
	CHECK(flash[FR_SELECTOR_OFF] == 0xFFu && flash[FR_SELECTOR_OFF + 1] == 0xFFu);
	CHECK(st.home.trial);

	/* the unit's own: started at once, forced */
	start_b_good(0, 1);
	CHECK(fr_proto_local(&proto, FR_ACT_SWITCH) == FR_WR_OK);
	CHECK(proto.phase == FR_PH_WRITING && proto.local && n_fops == 0u);
	CHECK((proto.flags & FR_BEGIN_F_FORCE) != 0u);
	CHECK(fr_proto_local(&proto, FR_ACT_SWITCH) == FR_WR_BUSY);
	poll1();
	CHECK(n_fops != 0u);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WAY] == FR_WAY_SWITCH);
	CHECK(flash[FR_SELECTOR_OFF] == 1u && flash[FR_SELECTOR_OFF + 1] == 0u);
	/* busy while receiving, and while counting down */
	start_b_good(0, 1);
	n = make_image(img_b, 0x100, 3, true);
	sha(img_b, n, h);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	CHECK(fr_proto_local(&proto, FR_ACT_TRY) == FR_WR_BUSY);
	CHECK(proto.phase == FR_PH_RECEIVING);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	CHECK(fr_proto_local(&proto, FR_ACT_TRY) == FR_WR_BUSY);
	CHECK(proto.phase == FR_PH_COUNTDOWN && n_fops == 0u);
	/* refused: the status, nothing written */
	start(0, 1, true);
	CHECK(fr_proto_local(&proto, FR_ACT_KEEP) == FR_WR_NOT_TRIED);
	CHECK(proto.phase == FR_PH_IDLE && n_fops == 0u);

	/* the damaged state: a repair write with the power-off held starts
	 * at once */
	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);
	start(0, 1, true);
	fail_other();
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	fail_erase_at = FR_SLOT_BASE(0) + FR_SECTOR;
	erase_then_fail = true;
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	CHECK(proto.phase == FR_PH_COUNTDOWN && !fr_proto_hold(&proto));
	through_countdown();
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_FAILED && fr_proto_hold(&proto));
	fail_erase_at = UINT32_MAX;
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	g_tick = 0;
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	CHECK(proto.phase == FR_PH_WRITING && fr_proto_hold(&proto));
	g_tick = 1000000u;
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && !fr_proto_hold(&proto));
}

/* RESTART's flags byte: bit 7 alone lifts 0x44 in the damaged state;
 * another bit or a longer payload is 0x40. */
static void test_restart_force(void)
{
	static const uint8_t f80[1] = { 0x80 }, f01[1] = { 0x01 }, fc0[1] = { 0xC0 },
			     two[2] = { 0x80, 0x00 };
	uint8_t r[300] = { 0 }, h[FR_SHA256_LEN], rc_img;
	size_t n;

	/* outside it: {} and {80} both restart */
	for (unsigned i = 0; i < 2u; i++) {
		start(0, 1, true);
		CHECK(status_of(FR_T_RESTART, f80, i) == FR_WR_OK);
		poll_n(2);
		CHECK(proto.restart);
	}
	start(0, 1, true);
	CHECK(status_of(FR_T_RESTART, f01, 1) == FR_ST_BAD_REQUEST);
	CHECK(status_of(FR_T_RESTART, fc0, 1) == FR_ST_BAD_REQUEST);
	CHECK(status_of(FR_T_RESTART, two, 2) == FR_ST_BAD_REQUEST);
	poll_n(2);
	CHECK(!proto.restart && !proto.restart_due);
	/* the damaged state */
	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);
	start(0, 1, true);
	fail_other();
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	fail_erase_at = FR_SLOT_BASE(0) + FR_SECTOR;
	erase_then_fail = true;
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(fr_proto_hold(&proto));
	CHECK(status_of(FR_T_RESTART, NULL, 0) == FR_ST_DAMAGED);
	CHECK(status_of(FR_T_RESTART, f01, 1) == FR_ST_BAD_REQUEST);
	CHECK(status_of(FR_T_RESTART, two, 2) == FR_ST_BAD_REQUEST);
	poll_n(2);
	CHECK(!proto.restart && !proto.restart_due);
	CHECK(status_of(FR_T_RESTART, f80, 1) == FR_WR_OK);
	poll_n(2);
	CHECK(proto.restart);
}

/* ACT's payload: two bytes, an action 1 to 4, only bit 7 of the flags. */
static void test_act_malformed(void)
{
	static const uint8_t bad[][3] = {
		{ 0, 0 }, { 5, 0 }, { 0xFF, 0 }, { 1, 0x01 }, { 1, 0x40 }, { 2, 0xC0 }, { 4, 0xFF },
	};
	uint8_t r[16];

	start_b_good(0, 1);
	for (unsigned i = 0; i < ARRAY_LEN(bad); i++) {
		CHECK(act_req(bad[i], 2, r) == (int)FR_ACT_REPLY_LEN);
		CHECK(r[0] == FR_ST_BAD_REQUEST && r[1] == FR_WAY_NONE && r[2] == FR_NO_SLOT);
	}
	CHECK(act_req(bad[0], 0, r) == (int)FR_ACT_REPLY_LEN && r[0] == FR_ST_BAD_REQUEST);
	CHECK(act_req((const uint8_t[]){ 1 }, 1, r) == (int)FR_ACT_REPLY_LEN && r[0] == FR_ST_BAD_REQUEST);
	CHECK(act_req((const uint8_t[]){ 1, 0, 0 }, 3, r) == (int)FR_ACT_REPLY_LEN &&
	      r[0] == FR_ST_BAD_REQUEST);
	CHECK(proto.phase == FR_PH_IDLE && n_fops == 0u);
}

/* One ACT, without force and with: the statuses, nothing written where
 * refused. A state builder runs before each. */
static void act_both(void (*state)(void), uint8_t act, uint8_t want, uint8_t want_forced)
{
	uint8_t ar[4], r[300];

	state();
	info(r);
	CHECK(r[FR_INFO_O_ACTS + act - 1u] == want);
	CHECK(r[FR_INFO_O_FORCED + act - 1u] == want_forced);
	n_fops = 0;
	CHECK(act_st(act, 0, ar) == want);
	if (want != FR_WR_OK) {
		CHECK(ar[1] == FR_WAY_NONE && ar[2] == FR_NO_SLOT);
		poll_n(30);
		CHECK(n_fops == 0u && proto.phase != FR_PH_COUNTDOWN && proto.phase != FR_PH_WRITING);
	}
	state();
	n_fops = 0;
	CHECK(act_st(act, FR_BEGIN_F_FORCE, ar) == want_forced);
	if (want_forced != FR_WR_OK) {
		poll_n(30);
		CHECK(n_fops == 0u && proto.phase != FR_PH_COUNTDOWN && proto.phase != FR_PH_WRITING);
	} else {
		CHECK(proto.phase == FR_PH_COUNTDOWN && (proto.flags & FR_BEGIN_F_FORCE) != 0u);
		run_to_end(r);
		CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE);
	}
}

static void st_good(void) { start_b_good(0, 1); }
static void st_trusted(void) { start_b_good(0, 1); flash[FR_SELECTOR_OFF] = 1; survey(); }
static void st_unverified(void) { start_b_good(0, 0x00); }
static void st_names_b(void) { start_b_good(1, 0xFF); }
static void st_b_bad(void)
{
	start_b_good(0, 1);
	flash[FR_SLOT_BASE(1) + len_c - 40u] ^= 0x01u;	/* main's zero bytes: still bootable */
	survey();
}
static void st_b_erased(void) { start(0, 1, true); }
static void st_tried(void) { start_b_good(0, 1); mark_b(1, FR_LOG0_PENDING); survey(); }
static void st_started(void) { start_b_good(0, 1); mark_b(1, FR_LOG0_MARKED); survey(); }
static void st_front(void)
{
	start_b_good(0, 1);
	g_fseed = 2;
	len_c = make_image(img_c, 0x200, 7, true);
	g_fseed = 1;
	put_slot(1, img_c, len_c);
	survey();
}
static void st_foreign(void)
{
	start_b_good(0, 1);
	len_c = make_image(img_c, 0x200, 7, false);
	put_slot(1, img_c, len_c);
	survey();
}
static void st_a_nofront(void)
{
	start_b_good(0, 1);
	len_a = make_image(img_a, 0x400, 1, false);
	put_slot(0, img_a, len_a);
	survey();
}
static void st_moved(void) { start_b_good(0, 1); st.home.moved = true; }
static void st_unknown(void) { start_b_good(0, 1); st.running.established = false; fr_rec_target(&st); }
/* a trial start: slot 1 tried and running, slot 0 trusted */
static void st_trial_start(void)
{
	start_b_good(0, 1);
	mark_b(1, FR_LOG0_PENDING);
	restart_as(1);
}
static void st_trial_start_spent(void)
{
	start_b_good(0, 1);
	mark_b(1, 0xF9);
	restart_as(1);
}

/*
 * S-02.093's force flag on ACT: each refusal that protects the user is
 * lifted by it, and each that states a fact is kept. INFO's statuses for
 * each action, unforced and forced, are the ones ACT gets.
 */
static void test_act_force(void)
{
	/* kept with force */
	act_both(st_trusted, FR_ACT_TRY, FR_WR_ALREADY, FR_WR_ALREADY);
	act_both(st_trusted, FR_ACT_SWITCH, FR_WR_ALREADY, FR_WR_ALREADY);
	act_both(st_b_erased, FR_ACT_SWITCH, FR_WR_IMAGE, FR_WR_IMAGE);
	act_both(st_moved, FR_ACT_TRY, FR_WR_TRIAL_STATE, FR_WR_TRIAL_STATE);
	act_both(st_good, FR_ACT_KEEP, FR_WR_NOT_TRIED, FR_WR_NOT_TRIED);
	act_both(st_good, FR_ACT_PUT_BACK, FR_WR_NOT_TRIED, FR_WR_NOT_TRIED);
	act_both(st_unknown, FR_ACT_TRY, FR_WR_NOT_RUNNING, FR_WR_NOT_RUNNING);
	act_both(st_unknown, FR_ACT_SWITCH, FR_WR_NOT_RUNNING, FR_WR_NOT_RUNNING);
	/* lifted by force */
	act_both(st_unverified, FR_ACT_TRY, FR_WR_UNVERIFIED, FR_WR_OK);
	act_both(st_unverified, FR_ACT_SWITCH, FR_WR_UNVERIFIED, FR_WR_OK);
	act_both(st_names_b, FR_ACT_TRY, FR_WR_NOT_SELECTED, FR_WR_OK);
	act_both(st_names_b, FR_ACT_SWITCH, FR_WR_NOT_SELECTED, FR_WR_OK);
	act_both(st_b_bad, FR_ACT_TRY, FR_WR_TARGET_BAD, FR_WR_OK);
	act_both(st_b_bad, FR_ACT_SWITCH, FR_WR_TARGET_BAD, FR_WR_OK);
	act_both(st_b_erased, FR_ACT_TRY, FR_WR_TARGET_BAD, FR_WR_OK);
	act_both(st_tried, FR_ACT_SWITCH, FR_WR_NOT_STARTED, FR_WR_OK);
	act_both(st_tried, FR_ACT_KEEP, FR_WR_NOT_STARTED, FR_WR_OK);
	act_both(st_front, FR_ACT_SWITCH, FR_WR_FRONT, FR_WR_OK);
	act_both(st_foreign, FR_ACT_SWITCH, FR_WR_FRONT, FR_WR_OK);
	act_both(st_a_nofront, FR_ACT_TRY, FR_WR_TRIAL_STATE, FR_WR_OK);
	act_both(st_moved, FR_ACT_SWITCH, FR_WR_TRIAL_STATE, FR_WR_OK);
	act_both(st_trial_start_spent, FR_ACT_KEEP, FR_WR_NOT_TRIED, FR_WR_OK);
	/* allowed either way */
	act_both(st_good, FR_ACT_TRY, FR_WR_OK, FR_WR_OK);
	act_both(st_good, FR_ACT_SWITCH, FR_WR_OK, FR_WR_OK);
	act_both(st_tried, FR_ACT_TRY, FR_WR_OK, FR_WR_OK);
	act_both(st_tried, FR_ACT_PUT_BACK, FR_WR_OK, FR_WR_OK);
	act_both(st_started, FR_ACT_KEEP, FR_WR_OK, FR_WR_OK);
	act_both(st_started, FR_ACT_SWITCH, FR_WR_OK, FR_WR_OK);
	act_both(st_front, FR_ACT_TRY, FR_WR_OK, FR_WR_OK);
	act_both(st_trial_start, FR_ACT_KEEP, FR_WR_OK, FR_WR_OK);
}

/*
 * The force flag on BEGIN: the selector not naming A (3), verified not 01
 * (4) and an in-place write before a write of B failed (15) are lifted, and
 * the write goes where the notes say; a size over 0x76000 (6), the running
 * slot unknown (2) and S-02.091's rules (7) are kept. Another front without
 * the trial flag (12), and a bootable stream with no front (7 with image
 * result 7), are written and selected under force.
 */
static void test_begin_force(void)
{
	uint8_t r[300] = { 0 }, br[16], h[FR_SHA256_LEN], rc_img;
	size_t n;

	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);
	/* 4: {00 00} */
	start(0, 0x00, true);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_UNVERIFIED && n_fops == 0u);
	CHECK(begin_reply((uint32_t)n, h, FR_BEGIN_F_FORCE, br) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_PLAIN && br[2] == 1u);
	CHECK(send_all(img_b, n) && end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 1u);
	CHECK(flash[FR_SELECTOR_OFF] == 1u && flash[FR_SELECTOR_OFF + 1] == 0u);
	/* 3: {01 01} with slot 1 good: the write goes to slot 0, not named */
	start_b_good(1, 0x01);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_NOT_SELECTED && n_fops == 0u);
	CHECK(begin_reply((uint32_t)n, h, FR_BEGIN_F_FORCE, br) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_PLAIN && br[2] == 0u);
	/* kept: 5, {01 01} with slot 1 bad: no write of slot 0 under it */
	start(1, 0x01, true);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_NOT_SELECTED);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_FORCE) == FR_WR_SELECTED_BAD);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING | FR_BEGIN_F_FORCE) == FR_WR_SELECTED_BAD);
	CHECK(n_fops == 0u);
	/* 15: in place before a write of slot 1 failed */
	start(0, 1, true);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_RUNNING) == FR_WR_TRY_OTHER && n_fops == 0u);
	CHECK(begin_reply((uint32_t)n, h, FR_BEGIN_F_RUNNING | FR_BEGIN_F_FORCE, br) ==
	      (int)FR_BEGIN_REPLY_LEN);
	CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_IN_PLACE && br[2] == 0u);
	CHECK(battery_calls == 1u);
	/* kept: 2, 6 */
	start(0, 1, false);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_FORCE) == FR_WR_NOT_RUNNING);
	start(0, 1, true);
	CHECK(begin_f(FR_IMAGE_MAX + 1u, h, FR_BEGIN_F_FORCE) == FR_WR_SIZE);
	CHECK(begin_f(0, h, FR_BEGIN_F_FORCE) == FR_WR_SIZE);
	CHECK(n_fops == 0u);
	/* kept: 7 for a stream that fails S-02.091's rules */
	{
		static uint8_t junk[0x400];

		memset(junk, 0x5A, sizeof junk);
		sha(junk, sizeof junk, h);
		start(0, 1, true);
		CHECK(begin_f(sizeof junk, h, FR_BEGIN_F_FORCE) == FR_WR_OK);
		CHECK(send_all(junk, sizeof junk));
		CHECK(end_rc(&rc_img) == FR_WR_IMAGE && rc_img == FR_IMG_STREAM);
		poll_n(30);
		CHECK(n_fops == 0u);
	}
	/* 12: another front, forced without the trial flag: written and
	 * selected */
	g_fseed = 2;
	n = make_image(img_b, 0x2400, 9, true);
	g_fseed = 1;
	sha(img_b, n, h);
	start(0, 1, true);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_FRONT);
	poll_n(30);
	CHECK(n_fops == 0u);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_FORCE) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 1u);
	CHECK(flash[FR_SELECTOR_OFF] == 1u && flash[FR_SELECTOR_OFF + 1] == 0u);
	CHECK(fr_le32(flash + FR_RECORD_OFF(1)) == fr_crc32(0, img_b, n));
	/* 7 with image result 7: no waffle-boot front, forced */
	n = make_image(img_b, 0x2400, 9, false);
	sha(img_b, n, h);
	start(0, 1, true);
	CHECK(begin_f((uint32_t)n, h, 0) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_IMAGE && rc_img == FR_IMG_NO_FRONT);
	poll_n(30);
	CHECK(n_fops == 0u);
	CHECK(begin_f((uint32_t)n, h, FR_BEGIN_F_FORCE) == FR_WR_OK);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 1u);
	CHECK((r[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] & (FR_SLOT_F_GOOD | FR_SLOT_F_HAS_FRONT)) ==
	      FR_SLOT_F_GOOD);
	/* the stream the slot kept holds: written, forced or not */
	start_b_good(0, 1);
	sha(img_a, len_a, h);
	CHECK(begin_f((uint32_t)len_a, h, FR_BEGIN_F_FORCE) == FR_WR_OK);
	CHECK(send_all(img_a, len_a));
	CHECK(end_rc(&rc_img) == FR_WR_OK);
	run_to_end(r);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_DONE && r[FR_INFO_O_WRITTEN] == 1u);
	CHECK(memcmp(flash + FR_SLOT_BASE(1), img_a, len_a) == 0);
}

/* INFO's fields of version 2 against fr_rec_act() over the same state, in
 * the states above, and its length. */
static void info_acts(void (*state)(void))
{
	uint8_t r[300] = { 0 };

	state();
	CHECK(ask(FR_T_INFO, NULL, 0, r) == (int)FR_INFO_LEN && FR_INFO_LEN == 242u);
	CHECK(r[FR_INFO_O_PHASE] == FR_PH_IDLE && r[FR_INFO_O_COUNT] == 0u);
	CHECK(r[FR_INFO_O_ACT] == FR_ACT_NONE);
	for (uint8_t a = FR_ACT_TRY; a <= FR_ACT_PUT_BACK; a++) {
		struct fr_act_plan p, pf;

		fr_rec_act(&st, a, false, &p);
		fr_rec_act(&st, a, true, &pf);
		CHECK(r[FR_INFO_O_ACTS + a - 1u] == p.status);
		CHECK(r[FR_INFO_O_FORCED + a - 1u] == pf.status);
		if (a == FR_ACT_TRY)
			CHECK(r[FR_INFO_O_TIER] == pf.tier);
		if (a == FR_ACT_KEEP)
			CHECK(r[FR_INFO_O_KEEP] == (pf.status == FR_WR_OK ? pf.target : FR_NO_SLOT));
	}
}

static void test_info_v2(void)
{
	uint8_t r[300] = { 0 };

	info_acts(st_good);
	CHECK(proto.st == &st);
	info(r);
	CHECK(r[FR_INFO_O_TIER] == FR_TIER_SAME && r[FR_INFO_O_KEEP] == FR_NO_SLOT);
	CHECK(r[FR_INFO_O_ACTS + 0] == FR_WR_OK && r[FR_INFO_O_ACTS + 2] == FR_WR_NOT_TRIED);
	info_acts(st_front);
	info(r);
	CHECK(r[FR_INFO_O_TIER] == FR_TIER_FRONT);
	info_acts(st_foreign);
	info(r);
	CHECK(r[FR_INFO_O_TIER] == FR_TIER_FOREIGN);
	info_acts(st_b_erased);
	info(r);
	CHECK(r[FR_INFO_O_TIER] == FR_TIER_BAD);
	info_acts(st_started);
	info(r);
	CHECK(r[FR_INFO_O_KEEP] == 1u);
	CHECK((r[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] & (FR_SLOT_F_TRIED | FR_SLOT_F_STARTED)) ==
	      (FR_SLOT_F_TRIED | FR_SLOT_F_STARTED));
	info_acts(st_trial_start);
	info(r);
	CHECK(r[FR_INFO_O_KEEP] == 1u && r[FR_INFO_O_RUNNING] == 1u);
	info_acts(st_trusted);
	info_acts(st_unverified);
	info_acts(st_names_b);
	info_acts(st_tried);
	info_acts(st_moved);
	info_acts(st_unknown);
}

/* A log byte that cannot be read: the survey reads that slot as bad, with
 * the read error (INFO bit 6); the writer, reading no log byte of a slot
 * that is not tried, may plan otherwise, and END then refuses 16 with
 * nothing written. */
static void test_log_read_fails(void)
{
	uint8_t r[300] = { 0 }, br[16], h[FR_SHA256_LEN], rc_img;
	size_t n;

	n = make_image(img_b, 0x2400, 9, true);
	sha(img_b, n, h);
	start_b_good(0, 1);
	st.home.other_failed = true;
	fail_read_always_at = FR_LOG_OFF(1);
	fr_rec_survey(&st, &fl);
	CHECK(st.read_error && !st.slot[1].good);
	info(r);
	CHECK((r[FR_INFO_O_FLAGS] & FR_INFO_F_READ_ERROR) != 0u);
	CHECK(r[FR_INFO_O_RUN_WAY] == FR_WAY_IN_PLACE);
	CHECK(begin_reply((uint32_t)n, h, FR_BEGIN_F_RUNNING, br) == (int)FR_BEGIN_REPLY_LEN);
	CHECK(br[0] == FR_WR_OK && br[1] == FR_WAY_IN_PLACE);
	CHECK(send_all(img_b, n));
	CHECK(end_rc(&rc_img) == FR_WR_CHANGED);
	poll_n(30);
	CHECK(n_fops == 0u && !st.home.spoiled && !st.home.moved);
	fail_read_always_at = UINT32_MAX;
}

/* Slot 1 holding slot_kinds.json's case `name` (S-02.106), its record good
 * or tried. */
static void put_kind_case(const char *name, bool tried)
{
	const struct v_skind *c = NULL;
	uint8_t *d = flash + FR_SLOT_BASE(1);

	for (unsigned i = 0; i < ARRAY_LEN(v_slot_kinds); i++)
		if (strcmp(v_slot_kinds[i].name, name) == 0)
			c = &v_slot_kinds[i];
	CHECK(c != NULL);
	if (c == NULL)
		return;
	memset(d, 0xFF, FR_SLOT_CAP);
	for (uint32_t i = 0; i < c->length; i++)
		d[i] = (uint8_t)(i * 37u + c->seed);
	for (unsigned j = 0; j < c->npatch; j++)
		memcpy(d + c->patch[j].off, c->patch[j].b, 4);
	CHECK(fr_crc32(0, d, c->length) == c->crc32);
	memset(flash + FR_RECORD_OFF(1), 0xFF, FR_SECTOR);
	fr_put32(flash + FR_RECORD_OFF(1), tried ? ~c->crc32 : c->crc32);
	fr_put32(flash + FR_RECORD_OFF(1) + 4u, c->length);
	fr_put32(flash + FR_RECORD_OFF(1) + 8u, FR_TAG);
}

/* INFO's slot flag bit 5 (S-02.093): set for a good or tried slot whose
 * kind S-02.106's rule 3 decides, clear where rule 1 or 2 decides first or
 * no rule holds, or the slot is neither good nor tried; a write of the slot
 * reads its kind again. */
static void test_info_te_prefix(void)
{
	static const struct {
		const char *name;
		bool bit5;
	} cases[] = {
		{ "te_prefix_only", true }, { "te_prefix_shortest", true },
		{ "te_row_and_prefix", false }, { "te_row_1000_0_20", false },
		{ "prefix_span_short", false }, { "prefix_too_short", false }, { "unknown", false },
	};
	uint8_t r[300] = { 0 };
	size_t n;

	for (unsigned i = 0; i < ARRAY_LEN(cases); i++)
		for (unsigned t = 0; t < 2u; t++) {
			const uint8_t f = t ? FR_SLOT_F_TRIED : FR_SLOT_F_GOOD;

			start(0, 1, true);
			put_kind_case(cases[i].name, t != 0u);
			survey();
			info(r);
			CHECK(r[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] ==
			      (uint8_t)(f | (cases[i].bit5 ? FR_SLOT_F_TE_PREFIX : 0u)));
			/* slot 0's image has a descriptor: rule 1 */
			CHECK((r[FR_INFO_O_SLOT(0) + FR_INFO_S_FLAGS] & FR_SLOT_F_TE_PREFIX) == 0u);
		}
	/* neither good nor tried: no kind */
	start(0, 1, true);
	put_kind_case("te_prefix_only", false);
	flash[FR_RECORD_OFF(1)] ^= 0x01u;
	survey();
	info(r);
	CHECK(r[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] == 0u);
	/* recovery's own survey reads the kind too */
	start(0, 1, true);
	put_kind_case("te_prefix_only", false);
	fr_rec_survey(&st, &fl);
	info(r);
	CHECK(r[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] == (FR_SLOT_F_GOOD | FR_SLOT_F_TE_PREFIX));
	/* the slot written is read again with its kind: a waffle-boot image
	 * over the TE one clears bit 5 */
	start(0, 1, true);
	put_kind_case("te_prefix_only", false);
	survey();
	n = make_image(img_b, 0x2400, 9, true);
	CHECK(write_img(img_b, n, 0, r) == FR_WR_OK);
	info(r);
	CHECK(r[FR_INFO_O_RESULT] == FR_WR_OK && r[FR_INFO_O_WRITTEN] == 1u);
	CHECK(r[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] ==
	      (FR_SLOT_F_GOOD | FR_SLOT_F_HAS_DESC | FR_SLOT_F_HAS_FRONT));
	CHECK(st.slot[1].kind == FR_KIND_WB);
}

/* S-18.004, S-18.005: INFO's device fields from 232, what recovery was
 * given; the fields before them as they were. */
static void test_info_device(void)
{
	uint8_t r[300] = { 0 };

	CHECK(FR_INFO_O_DEVICE == 232u && FR_INFO_O_DEV_HOW == 233u && FR_INFO_O_FS_N == 234u &&
	      FR_INFO_O_FS_C == 238u && FR_INFO_LEN == 242u);
	CHECK(FR_INFO_O_REFUSED + 1u == FR_INFO_O_DEVICE);
	start(0, 1, true);
	info(r);
	CHECK(r[FR_INFO_O_DEVICE] == FR_HO_DEV_OPXY && r[FR_INFO_O_DEV_HOW] == FR_HO_HOW_TABLE);
	CHECK(fr_le32(r + FR_INFO_O_FS_N) == 0u && fr_le32(r + FR_INFO_O_FS_C) == 0u);
	start(0, 1, true);
	st.dev_id = FR_HO_DEV_OP1F;
	st.dev_how = FR_HO_HOW_TABLE;
	st.fs_n = 0x8600u;
	st.fs_c = 0x49267EB8u;
	info(r);
	CHECK(r[FR_INFO_O_DEVICE] == FR_HO_DEV_OP1F && r[FR_INFO_O_DEV_HOW] == FR_HO_HOW_TABLE);
	CHECK(fr_le32(r + FR_INFO_O_FS_N) == 0x8600u && fr_le32(r + FR_INFO_O_FS_C) == 0x49267EB8u);
	/* not identified: the default device, with n and c as read */
	start(0, 1, true);
	st.dev_id = FR_HO_DEV_OPXY;
	st.dev_how = FR_HO_HOW_UNKNOWN;
	st.fs_n = 0x86C0u;
	st.fs_c = 0xB151F65Du;
	info(r);
	CHECK(r[FR_INFO_O_DEVICE] == FR_HO_DEV_OPXY && r[FR_INFO_O_DEV_HOW] == FR_HO_HOW_UNKNOWN);
	CHECK(r[FR_INFO_O_FS_N] == 0xC0u && r[FR_INFO_O_FS_N + 1] == 0x86u &&
	      r[FR_INFO_O_FS_N + 2] == 0u && r[FR_INFO_O_FS_N + 3] == 0u);
	CHECK(fr_le32(r + FR_INFO_O_FS_C) == 0xB151F65Du);
	CHECK(r[FR_INFO_O_WHY] == FR_WHY_KEYS && r[FR_INFO_O_PHASE] == FR_PH_IDLE);
}

int main(void)
{
	RUN(test_info_device);
	RUN(test_trial_spent);
	RUN(test_target);
	RUN(test_target_all);
	RUN(test_plan_agrees);
	RUN(test_begin_flags);
	RUN(test_battery);
	RUN(test_select_first);
	RUN(test_in_place);
	RUN(test_first_erase);
	RUN(test_retry);
	RUN(test_info_ways);
	RUN(test_retry_flags);
	RUN(test_begin_reply);
	RUN(test_gate);
	RUN(test_restart_damaged);
	RUN(test_end_changed);
	RUN(test_rescue_case_a);
	RUN(test_rescue_torn_verified);
	RUN(test_rescue_in_place_close);
	RUN(test_run19);
	RUN(test_tick);
	RUN(test_rescue_read_errors);
	RUN(test_run22);
	RUN(test_run23);
	RUN(test_reach);
	RUN(test_reach_hold);
	RUN(test_refresh_reads);
	RUN(test_hello);
	RUN(test_write);
	RUN(test_refusals);
	RUN(test_flash_failure);
	RUN(test_data_order);
	RUN(test_damaged_frames);
	RUN(test_slow_port);
	RUN(test_restart);
	RUN(test_record_gone);
	RUN(test_after_write);
	RUN(test_trial_flags);
	RUN(test_trial_select);
	RUN(test_trial_refusals);
	RUN(test_trial_settle_back);
	RUN(test_countdown);
	RUN(test_countdown_drop);
	RUN(test_countdown_kinds);
	RUN(test_info_refused);
	RUN(test_restart_force);
	RUN(test_act_malformed);
	RUN(test_act_force);
	RUN(test_begin_force);
	RUN(test_info_v2);
	RUN(test_log_read_fails);
	RUN(test_info_te_prefix);
	return check_summary("front_proto");
}
