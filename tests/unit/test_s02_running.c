/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-02 running slot: s02_running_slot() infers the slot the boot ROM loaded
 * from what memory holds (S-01 question 10), so the slave is streamed the
 * running slot and not the selected one (S-01.002, S-02.054). The cases of
 * SC-SLAVE-SRC: the running slot is the selected one; the one-boot-late run
 * under the factory first stage (the selector names the other slot, the
 * slot that runs has a bad CRC); a good slot that runs while the selector
 * names the other; byte-identical streams; read faults. Every case is
 * also run as the boot runs it (S-09.002): the slot's stream read once into
 * memory, checked and compared there, the other slot ruled out by a probe
 * or compared whole, and the answers must be the same.
 */
#include <string.h>

#include "s02_check.h"
#include "s02_fake_flash.h"
#include "waffle/boot/handoff.h"

#define MEMB 0x11A00000u	/* readable memory (L2) */
#define MEML 0x10000u
#define L1I  0xFFA00000u	/* not readable with data loads */

static struct fake_flash g_f;
static struct s02_flash_ops g_ops;
static uint8_t g_mem[MEML];
static bool g_mem_dead;
static uint8_t g_a[0x2000], g_b[0x2000];
static uint32_t g_alen, g_blen;

/* The board's map: code and constant data, not the initial data at
 * MEMB + 0x2000, which the program may have written. */
static bool mem_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (g_mem_dead || addr < MEMB || (uint64_t)addr + len > (uint64_t)MEMB + MEML)
		return false;
	if (addr < MEMB + 0x3000u && (uint64_t)addr + len > MEMB + 0x2000u)
		return false;
	memcpy(buf, g_mem + (addr - MEMB), len);
	return true;
}

static const struct s02_mem_ops g_mops = { NULL, mem_read };

/* An LDR header: signature AD, flags, and the byte that zeroes the XOR. */
static uint32_t put_block(uint8_t *s, uint32_t o, uint16_t flags, uint32_t target,
			  uint32_t count, uint32_t arg, const uint8_t *payload)
{
	uint8_t x = 0;

	ff_put32(s + o, 0xAD000000u | flags);
	ff_put32(s + o + 4, target);
	ff_put32(s + o + 8, count);
	ff_put32(s + o + 12, arg);
	for (unsigned i = 0; i < 16; i++)
		x = (uint8_t)(x ^ s[o + i]);
	s[o + 2] = x;
	o += 16;
	if (!(flags & S05_FLAG_FILL) && payload != NULL) {
		memcpy(s + o, payload, count);
		o += count;
	}
	return o;
}

/* A bootloader stream: code, data, a FILL, an IGNORE, code in L1, FINAL. */
static uint32_t make_stream(uint8_t *s, uint32_t seed)
{
	uint8_t code[3000], data[200], l1[100], ign[32];
	uint32_t o = 0;

	ff_fill_image(code, sizeof code, seed);
	ff_fill_image(data, sizeof data, 77);	/* the same data in both */
	ff_fill_image(l1, sizeof l1, seed + 1u);
	ff_fill_image(ign, sizeof ign, seed + 2u);
	o = put_block(s, o, 0, MEMB, sizeof code, 0, code);
	o = put_block(s, o, 0, MEMB + 0x2000u, sizeof data, 0, data);
	o = put_block(s, o, S05_FLAG_FILL, MEMB + 0x3000u, 0x100, 0, NULL);
	o = put_block(s, o, S05_FLAG_IGNORE, 0, sizeof ign, 0, ign);
	o = put_block(s, o, 0, L1I, sizeof l1, 0, l1);
	o = put_block(s, o, S05_FLAG_FINAL, MEMB + 0x4000u, 0, 0, NULL);
	return o;
}

/* What the ROM leaves in memory after loading s, the data since changed. */
static void load(const uint8_t *s, uint32_t len)
{
	struct s05_block b;
	uint32_t off = 0;

	memset(g_mem, 0x5A, sizeof g_mem);
	while (s05_ldr_next(s, len, &off, &b) == 0) {
		if (!(b.flags & S05_FLAG_FILL) && s05_block_has_destination(&b) &&
		    b.target >= MEMB && b.target + b.count <= MEMB + MEML)
			memcpy(g_mem + (b.target - MEMB), s + b.offset + 16, b.count);
		if (b.flags & S05_FLAG_FINAL)
			break;
	}
	memset(g_mem + 0x2000u, 0xEE, 40);	/* the program wrote its data */
}

/* The usual CRC counted, and a faster one that is right, or wrong while
 * g_lie is set. */
static unsigned g_crc_calls, g_fast_crc_calls;
static bool g_lie;

static uint32_t counted_crc32(uint32_t crc, const uint8_t *buf, uint32_t len)
{
	g_crc_calls++;
	return ref_crc32(crc, buf, len);
}

static uint32_t lying_crc32(uint32_t crc, const uint8_t *buf, uint32_t len)
{
	g_fast_crc_calls++;
	return ref_crc32(crc, buf, len) ^ (g_lie ? 0x00810000u : 0u);
}

static void put_slot(unsigned slot, const uint8_t *s, uint32_t len, bool good_crc)
{
	struct s02_record r = { ref_crc32(0, s, len) ^ (good_crc ? 0u : 1u), len,
				S02_RECORD_TAG };
	uint8_t raw[S02_RECORD_SIZE];

	memcpy(g_f.mem + s02_slot_base(slot), s, len);
	s02_record_encode(&r, raw);
	memcpy(g_f.mem + s02_record_off(slot), raw, sizeof raw);
}

/* --- the boot's way: the stream read once --- */

static uint8_t g_buf[S02_SLOT_CAPACITY];
static struct s02_image g_im;

/* A faster read: counted; it can fail, or return wrong bytes without an
 * error: one in the middle, and one in every 256 from the 0x40th on, so
 * that any chunk it is trusted for, the probe's among them, is wrong. */
static unsigned g_fast_calls, g_fast_short;
static bool g_fast_fails, g_fast_corrupts;

static int fast_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	const struct fake_flash *f = ctx;

	g_fast_calls++;
	if (len <= 2048u)		/* a chunk's, not a stream's */
		g_fast_short++;
	if (g_fast_fails || off > S02_FLASH_SIZE || len > S02_FLASH_SIZE - off)
		return -1;
	memcpy(buf, f->mem + off, len);
	if (g_fast_corrupts) {
		buf[len / 2u] ^= 0x04u;
		for (uint32_t i = 0x40u; i < len; i += 0x100u)
			buf[i] ^= 0x80u;
	}
	return 0;
}

/* Bytes of the code block: the two streams differ there. */
static const struct s02_probe g_probe = { MEMB + 0x40u, 8u };

struct boot {
	int rc;			/* s02_running_image()'s */
	unsigned first;		/* the slot read first */
	unsigned slot;		/* the running slot, 2 when not established */
	unsigned sent;		/* the slot to stream */
	bool good;		/* and S-02.016's verdict on the image of it */
};

/* The board's steps (opxy_master.c). */
static struct boot boot(unsigned selected, const struct s02_probe *probe, s02_read_fn fast)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	struct boot b;

	s02_image_init(&g_im, g_buf, sizeof g_buf, fast, &g_f);
	b.first = s02_running_first(&env, &g_mops, probe, selected, &g_im);
	(void)s02_image_read(&env, b.first, &g_im);
	(void)s02_image_check(&env, &g_im);
	b.rc = s02_running_image(&env, &g_mops, probe, &g_im, b.first, &b.slot);
	if (b.rc != S02_OK)
		CHECK_EQ(b.slot, 2u);
	b.sent = b.slot < 2u ? b.slot : selected;
	if (g_im.slot != b.sent) {
		(void)s02_image_read(&env, b.sent, &g_im);
		(void)s02_image_check(&env, &g_im);
	}
	b.good = g_im.slot == b.sent && g_im.good;
	if (b.good) {
		/* S-02.016 over the slot's own bytes, which the image holds */
		struct s02_record r;

		s02_record_decode(g_f.mem + s02_record_off(b.sent), &r);
		CHECK(s02_record_plausible(&r));
		CHECK_EQ(g_im.len, r.length);
		CHECK_EQ(ref_crc32(0, g_f.mem + s02_slot_base(b.sent), r.length), r.crc32);
		CHECK(memcmp(g_buf, g_f.mem + s02_slot_base(b.sent), g_im.len) == 0);
	}
	return b;
}

/* The old inference, and with it the boot's in each of its forms: the same
 * slot, the settled one where both slots hold the same image, or the same
 * "not established". */
static unsigned running(unsigned fallback, int *rc)
{
	struct s02_env env = ff_env(&g_f, &g_ops);
	const unsigned fail = g_f.fail_regions;
	unsigned slot = 9, plain = 9;

	*rc = s02_running_slot(&env, &g_mops, fallback, &slot);
	(void)s02_running_slot(&env, &g_mops, 2u, &plain);
	for (unsigned v = 0; v < 12u; v++) {
		struct boot b;

		/* 8 to 11: the faster read returns wrong bytes, silently */
		g_fast_corrupts = v >= 8u;
		b = boot(v & 1u, (v & 2u) ? &g_probe : NULL,
			 (v & 4u) || v >= 8u ? fast_read : NULL);
		g_fast_corrupts = false;

		if (fail == 0u) {
			CHECK_EQ(b.rc, S02_OK);
			CHECK_EQ(b.slot, plain);
		} else if (b.rc == S02_OK && b.slot < 2u) {
			/* a slot it could still establish is the right one */
			g_f.fail_regions = 0;
			(void)s02_running_slot(&env, &g_mops, 2u, &plain);
			g_f.fail_regions = fail;
			CHECK_EQ(b.slot, plain);
		}
	}
	return slot;
}

/* Reads of the flash since `from` that touch [off, off + len). */
static unsigned reads_of(unsigned from, uint32_t off, uint32_t len)
{
	unsigned n = 0;

	for (unsigned i = from; i < g_f.nlog && i < FF_LOG_MAX; i++)
		if (g_f.log[i].kind == FF_READ && g_f.log[i].off < off + len &&
		    g_f.log[i].off + g_f.log[i].len > off)
			n++;
	return n;
}

/* The same, leaving out the reads of a block header alone. */
static unsigned payload_reads_of(unsigned from, uint32_t off, uint32_t len)
{
	unsigned n = 0;

	for (unsigned i = from; i < g_f.nlog && i < FF_LOG_MAX; i++)
		if (g_f.log[i].kind == FF_READ && g_f.log[i].len != 16u &&
		    g_f.log[i].off < off + len && g_f.log[i].off + g_f.log[i].len > off)
			n++;
	return n;
}

static uint32_t bytes_read(unsigned from)
{
	uint32_t n = 0;

	for (unsigned i = from; i < g_f.nlog && i < FF_LOG_MAX; i++)
		if (g_f.log[i].kind == FF_READ)
			n += g_f.log[i].len;
	return n;
}

static void fresh(void)
{
	ff_init(&g_f);
	g_mem_dead = false;
	put_slot(0, g_a, g_alen, true);
	put_slot(1, g_b, g_blen, true);
}

int main(void)
{
	int rc;
	bool good;
	struct s02_env env;

	g_alen = make_stream(g_a, 1);
	g_blen = make_stream(g_b, 2);

	/* the running slot is the selected one (SC-SLAVE-SRC's runs) */
	fresh();
	load(g_a, g_alen);
	CHECK_EQ(running(0, &rc), 0u);
	CHECK_EQ(rc, S02_OK);
	load(g_b, g_blen);
	CHECK_EQ(running(1, &rc), 1u);

	/* one boot late: slot 0 failed its check, the selector names 1, the
	 * master runs slot 0 */
	fresh();
	put_slot(0, g_a, g_alen, false);
	env = ff_env(&g_f, &g_ops);
	CHECK_EQ(s02_check_slot(&env, 0, &good), S02_OK);
	CHECK(!good);
	load(g_a, g_alen);
	CHECK_EQ(running(1, &rc), 0u);
	CHECK_EQ(rc, S02_OK);

	/* a good slot runs while the selector names the other */
	fresh();
	load(g_b, g_blen);
	CHECK_EQ(running(0, &rc), 1u);
	load(g_a, g_alen);
	CHECK_EQ(running(1, &rc), 0u);

	/* byte-identical streams: either is this image (S-02.054). The slot
	 * is the one later starts settle on: the selected slot when it is
	 * verified or good, otherwise the other; the fallback has no say. */
	for (unsigned c = 0; c < 4u * 4u * 4u; c++) {
		static const uint8_t index[4] = { 0x00, 0x01, 0x02, 0xFF };
		static const uint8_t verified[4] = { 0x01, 0x00, 0xFE, 0xFF };
		const unsigned sel = index[c & 3u] == 0x00u ? 0u : 1u;
		const uint8_t ver = verified[c >> 2 & 3u];
		const bool good0 = (c >> 4 & 1u) == 0u, good1 = (c >> 5 & 1u) == 0u;
		const unsigned want = ver == 0x01u || (sel == 0u ? good0 : good1) ? sel : 1u - sel;

		ff_init(&g_f);
		put_slot(0, g_a, g_alen, good0);
		put_slot(1, g_a, g_alen, good1);
		ff_set_selector(&g_f, index[c & 3u], ver);
		load(g_a, g_alen);
		CHECK_EQ(running(0, &rc), want);
		CHECK_EQ(rc, S02_OK);
		CHECK_EQ(running(1, &rc), want);
		CHECK_EQ(g_f.nmut, 0);
	}
	/* a trial mark on the selected slot, not verified: the other slot */
	ff_init(&g_f);
	put_slot(0, g_a, g_alen, true);
	put_slot(1, g_a, g_alen, true);
	ff_put32(g_f.mem + s02_record_off(1), ~ref_crc32(0, g_a, g_alen));
	ff_set_selector(&g_f, 0x01, 0x00);
	load(g_a, g_alen);
	CHECK_EQ(running(1, &rc), 0u);
	/* the selector, or the slot its answer needs, cannot be read: the
	 * fallback and the error */
	ff_set_selector(&g_f, 0x00, 0x01);
	g_f.fail_regions = FF_R_SELECTOR;
	CHECK_EQ(running(1, &rc), 1u);
	CHECK_EQ(rc, S02_ERR_IO);
	g_f.fail_regions = 0;
	/* one of them cut short matches a part: the whole one, as before */
	ff_init(&g_f);
	put_slot(0, g_a, g_alen, true);
	put_slot(1, g_a, g_alen / 2u, true);
	ff_set_selector(&g_f, 0x01, 0x01);
	load(g_a, g_alen);
	CHECK_EQ(running(1, &rc), 0u);

	/* the other slot erased, or its record implausible */
	ff_init(&g_f);
	put_slot(0, g_a, g_alen, true);
	load(g_a, g_alen);
	CHECK_EQ(running(1, &rc), 0u);
	memset(g_f.mem + s02_record_off(0), 0xFF, S02_RECORD_SIZE);
	CHECK_EQ(running(1, &rc), 0u);		/* walked to its FINAL */

	/* a stream broken after its first block still tells them apart */
	fresh();
	g_f.mem[s02_slot_base(1) + 16u + 3000u + 5u] ^= 0x40u;
	load(g_b, g_blen);
	CHECK_EQ(running(0, &rc), 1u);

	/* a shorter stream that matches whole over a longer one that
	 * matches in more bytes, but not in all */
	{
		static uint8_t s1[0x3000], s2[0x3000];
		uint8_t pay[0x800];
		uint32_t n1, n2;

		ff_fill_image(pay, sizeof pay, 61);
		n1 = put_block(s1, 0, 0, MEMB, sizeof pay, 0, pay);
		n1 = put_block(s1, n1, S05_FLAG_FINAL, MEMB, 0, 0, NULL);
		n2 = put_block(s2, 0, 0, MEMB, sizeof pay, 0, pay);
		memset(pay, 0x5A, sizeof pay);
		pay[100] = 0x00;
		n2 = put_block(s2, n2, 0, MEMB + 0x4000u, sizeof pay, 0, pay);
		n2 = put_block(s2, n2, S05_FLAG_FINAL, MEMB, 0, 0, NULL);
		ff_init(&g_f);
		put_slot(0, s2, n2, true);
		put_slot(1, s1, n1, true);
		load(s1, n1);
		CHECK_EQ(running(0, &rc), 1u);
		put_slot(0, s1, n1, true);
		put_slot(1, s2, n2, true);
		CHECK_EQ(running(1, &rc), 0u);
	}

	/* the other slot a prefix of the running stream, cut short before its
	 * FINAL block: it matches every byte it has, yet the running slot is
	 * the one established */
	fresh();
	load(g_a, g_alen);
	memset(g_f.mem + s02_slot_base(1), 0xFF, 0x2000u);
	memcpy(g_f.mem + s02_slot_base(1), g_a, 16u + 3000u);	/* the code block alone */
	{
		uint8_t raw[S02_RECORD_SIZE];

		memset(raw, 0xFF, sizeof raw);
		memcpy(g_f.mem + s02_record_off(1), raw, sizeof raw);
	}
	CHECK_EQ(running(1, &rc), 0u);
	CHECK_EQ(rc, S02_OK);

	/* nothing readable: the fallback */
	fresh();
	load(g_a, g_alen);
	g_mem_dead = true;
	CHECK_EQ(running(1, &rc), 1u);
	CHECK_EQ(rc, S02_OK);

	/* a flash read fault: S02_ERR_IO and the fallback */
	fresh();
	load(g_a, g_alen);
	g_f.fail_regions = FF_R_SLOT1;
	CHECK_EQ(running(1, &rc), 1u);
	CHECK_EQ(rc, S02_ERR_IO);
	g_f.fail_regions = FF_R_RECORD0;
	CHECK_EQ(running(1, &rc), 1u);
	CHECK_EQ(rc, S02_ERR_IO);

	/* the inference writes nothing */
	fresh();
	load(g_a, g_alen);
	(void)running(1, &rc);
	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++)
		CHECK(g_f.log[i].kind == FF_READ);

	/* --- the stream read once --- */

	/* the usual boot: one read of the running slot's stream, each record
	 * read once, and of the other slot its headers and the probe's chunk */
	for (unsigned sel = 0; sel < 2u; sel++) {
		struct boot b;
		unsigned from;

		fresh();
		load(g_a, g_alen);
		from = g_f.nlog;
		b = boot(sel, &g_probe, NULL);
		CHECK_EQ(b.rc, S02_OK);
		CHECK_EQ(b.first, 0u);			/* whatever the selector names */
		CHECK_EQ(b.slot, 0u);
		CHECK(b.good);
		CHECK_EQ(g_im.len, g_alen);
		CHECK_EQ(reads_of(from, S02_RECORD0_OFF, S02_RECORD_SIZE), 1u);
		CHECK_EQ(reads_of(from, S02_RECORD1_OFF, S02_RECORD_SIZE), 1u);
		/* slot 0: the probe's chunk, then the stream, once */
		CHECK_EQ(reads_of(from, S02_SLOT0_OFF + 16u, 1u), 2u);
		CHECK_EQ(reads_of(from, S02_SLOT0_OFF + 16u + 2500u, 1u), 1u);
		CHECK_EQ(payload_reads_of(from, S02_SLOT0_OFF, g_alen), 2u);
		/* slot 1: its headers, and the one chunk that differs */
		CHECK_EQ(payload_reads_of(from, S02_SLOT1_OFF, g_blen), 1u);
		CHECK_EQ(reads_of(from, S02_SLOT1_OFF + 16u + 2048u, 3000u - 2048u), 0u);
		CHECK(bytes_read(from) < g_alen + 2u * 2048u + 400u);
		/* the slots differ: nothing is read for the tie-break */
		CHECK_EQ(reads_of(from, S02_SELECTOR_OFF, 2u), 0u);
	}

	/* no probe: the selected slot first; a wrong first slot is found out,
	 * the other compared whole, and its stream then read */
	fresh();
	load(g_a, g_alen);
	{
		struct boot b = boot(1, NULL, NULL);

		CHECK_EQ(b.first, 1u);
		CHECK_EQ(b.slot, 0u);
		CHECK(b.good);
		CHECK_EQ(g_im.slot, 0u);
	}

	/* memory that cannot be compared costs no flash read: with the same
	 * build in both slots the other is compared whole, yet its data, IGNORE
	 * and L1 payloads are not read */
	ff_init(&g_f);
	put_slot(0, g_a, g_alen, true);
	put_slot(1, g_a, g_alen, true);
	ff_set_selector(&g_f, 0x00, 0x01);
	load(g_a, g_alen);
	{
		const uint32_t data = 16u + 3000u + 16u;	/* the data block's payload */
		unsigned from = g_f.nlog;
		struct boot b = boot(0, &g_probe, NULL);

		CHECK_EQ(b.rc, S02_OK);
		CHECK_EQ(b.slot, 0u);			/* the settled slot */
		CHECK_EQ(b.sent, 0u);
		CHECK(b.good);
		/* compared whole: the code block's two chunks, the first also as the probe's */
		CHECK_EQ(payload_reads_of(from, S02_SLOT1_OFF + 16u, 3000u), 3u);
		CHECK_EQ(payload_reads_of(from, S02_SLOT1_OFF + data, g_alen - data), 0u);
	}

	/* --- the same image in both slots, as the boot finds it --- */

	/* Every selector and record state, with a probe and without, whichever
	 * slot is read first, by either read: the settled slot, as
	 * s02_running_slot() names it. Beyond what the tie compares (the other
	 * slot's record, six block headers and its code block's two chunks), the flash
	 * is read for the selector's two bytes; and for the selected slot's
	 * check only when the selector is not verified and the image holds the
	 * other slot. */
	for (unsigned c = 0; c < 2u * 3u * 3u * 2u * 2u * 2u; c++) {
		static const uint8_t verified[3] = { 0x01, 0x00, 0xFF };
		const unsigned sel = c & 1u, rs = c / 2u % 3u, first = c / 18u & 1u;
		const uint8_t ver = verified[c / 6u % 3u];
		const struct s02_probe *probe = (c / 36u & 1u) ? &g_probe : NULL;
		const s02_read_fn fast = (c / 72u & 1u) ? fast_read : NULL;
		/* the selected slot's record: good, a CRC that fails, the trial mark */
		const unsigned want = ver == 0x01u || rs == 0u ? sel : 1u - sel;
		const bool checks = ver != 0x01u && first != sel;
		/* the other slot's record, when no probe has read it yet */
		const uint32_t tie = 6u * 16u + 3000u + (probe != NULL ? 0u : S02_RECORD_SIZE);
		unsigned from, slot = 9, plain = 9;

		ff_init(&g_f);
		put_slot(0, g_a, g_alen, true);
		put_slot(1, g_a, g_alen, true);
		if (rs == 1u)
			put_slot(sel, g_a, g_alen, false);
		else if (rs == 2u)
			ff_put32(g_f.mem + s02_record_off(sel), ~ref_crc32(0, g_a, g_alen));
		ff_set_selector(&g_f, sel == 0u ? 0x00 : 0x01, ver);
		load(g_a, g_alen);
		env = ff_env(&g_f, &g_ops);
		CHECK_EQ(s02_running_slot(&env, &g_mops, 2u, &plain), S02_OK);
		CHECK_EQ(plain, want);

		s02_image_init(&g_im, g_buf, sizeof g_buf, fast, &g_f);
		/* both match the probe: the selected slot, here either */
		CHECK_EQ(s02_running_first(&env, &g_mops, probe, first, &g_im), first);
		CHECK_EQ(s02_image_read(&env, first, &g_im), S02_OK);
		CHECK_EQ(s02_image_check(&env, &g_im), S02_OK);
		from = g_f.nlog;
		g_fast_calls = 0;
		CHECK_EQ(s02_running_image(&env, &g_mops, probe, &g_im, first, &slot), S02_OK);
		CHECK_EQ(slot, want);
		CHECK_EQ(g_fast_calls, 0u);
		CHECK_EQ(reads_of(from, S02_SELECTOR_OFF, 2u), 1u);
		/* s02_check_slot(): the record, and the stream once */
		CHECK_EQ(bytes_read(from), tie + 2u + (checks ? S02_RECORD_SIZE + g_alen : 0u));
		CHECK_EQ(g_im.slot, first);		/* the image is not changed */

		/* the same before the image is checked: its bytes are judged
		 * here when the usual read gave them, else the slot is read */
		s02_image_init(&g_im, g_buf, sizeof g_buf, fast, &g_f);
		CHECK_EQ(s02_image_read(&env, first, &g_im), S02_OK);
		slot = 9;
		CHECK_EQ(s02_running_image(&env, &g_mops, probe, &g_im, first, &slot), S02_OK);
		CHECK_EQ(slot, want);
	}
	/* a record that is not plausible on the selected slot, not verified:
	 * bad with no read of its stream; the other slot */
	for (unsigned first = 0; first < 2u; first++) {
		unsigned from, slot = 9, plain = 9;

		ff_init(&g_f);
		put_slot(0, g_a, g_alen, true);
		put_slot(1, g_a, g_alen, true);
		memset(g_f.mem + s02_record_off(1), 0xFF, S02_RECORD_SIZE);
		ff_set_selector(&g_f, 0x01, 0x00);
		load(g_a, g_alen);
		env = ff_env(&g_f, &g_ops);
		CHECK_EQ(s02_running_slot(&env, &g_mops, 2u, &plain), S02_OK);
		CHECK_EQ(plain, 0u);
		s02_image_init(&g_im, g_buf, sizeof g_buf, NULL, NULL);
		CHECK_EQ(s02_image_read(&env, first, &g_im), S02_OK);
		CHECK_EQ(s02_image_check(&env, &g_im), S02_OK);
		from = g_f.nlog;
		CHECK_EQ(s02_running_image(&env, &g_mops, NULL, &g_im, first, &slot), S02_OK);
		CHECK_EQ(slot, 0u);
		CHECK_EQ(payload_reads_of(from, S02_SLOT1_OFF + 16u + 3000u, g_alen), 0u);
	}
	/* the selector cannot be read: not established; nor a slot */
	ff_init(&g_f);
	put_slot(0, g_a, g_alen, true);
	put_slot(1, g_a, g_alen, true);
	ff_set_selector(&g_f, 0x00, 0x00);
	load(g_a, g_alen);
	for (unsigned v = 0; v < 4u; v++) {
		struct boot b;

		g_f.fail_regions = FF_R_SELECTOR;
		b = boot(v & 1u, (v & 2u) ? &g_probe : NULL, NULL);
		CHECK_EQ(b.rc, S02_ERR_IO);
		CHECK_EQ(b.slot, 2u);
		CHECK_EQ(b.sent, v & 1u);
		CHECK(b.good);
		g_f.fail_regions = 0;
	}
	{
		unsigned slot = 9;

		env = ff_env(&g_f, &g_ops);
		s02_image_init(&g_im, g_buf, sizeof g_buf, NULL, NULL);
		CHECK_EQ(s02_image_read(&env, 1, &g_im), S02_OK);
		CHECK_EQ(s02_image_check(&env, &g_im), S02_OK);
		g_f.fail_regions = FF_R_SLOT0;
		CHECK_EQ(s02_running_image(&env, &g_mops, NULL, &g_im, 1, &slot), S02_ERR_IO);
		CHECK_EQ(slot, 2u);
		g_f.fail_regions = 0;
	}
	/* one read fails, whichever: made once more, the same answer; so with
	 * the selected slot's check among the reads */
	for (unsigned first = 0; first < 2u; first++) {
		unsigned n0 = g_f.nreads, n;

		(void)boot(first, &g_probe, NULL);
		n = g_f.nreads - n0;
		CHECK(n > 4u && n < 200u);
		for (unsigned k = 1; k <= n; k++) {
			struct boot b;

			g_f.fail_read = g_f.nreads + k;
			b = boot(first, &g_probe, NULL);
			CHECK_EQ(b.rc, S02_OK);
			CHECK_EQ(b.slot, 0u);
			CHECK(b.good);
		}
		g_f.fail_read = 0;
	}

	/* S-09.002: a slot whose CRC fails is established all the same, and
	 * is not good; so with a trial's mark in its record */
	fresh();
	put_slot(0, g_a, g_alen, false);
	load(g_a, g_alen);
	for (unsigned v = 0; v < 2u; v++) {
		struct boot b = boot(1, &g_probe, v ? fast_read : NULL);

		CHECK_EQ(b.slot, 0u);
		CHECK_EQ(b.sent, 0u);
		CHECK(!b.good);
	}
	/* a record that is not plausible: no read of the stream, not good */
	fresh();
	load(g_a, g_alen);
	memset(g_f.mem + s02_record_off(0), 0xFF, S02_RECORD_SIZE);
	{
		unsigned from = g_f.nlog;
		struct boot b = boot(0, &g_probe, NULL);

		CHECK_EQ(b.slot, 0u);			/* walked from flash to its FINAL */
		CHECK(!b.good);
		CHECK_EQ(g_im.len, 0u);
		CHECK_EQ(reads_of(from, S02_RECORD0_OFF, S02_RECORD_SIZE), 1u);
	}

	/* one read fails, whichever: made once more, the same answer */
	fresh();
	load(g_b, g_blen);
	{
		unsigned n0 = g_f.nreads, n;

		(void)boot(0, &g_probe, NULL);
		n = g_f.nreads - n0;
		CHECK(n > 4u && n < 200u);
		for (unsigned k = 1; k <= n; k++) {
			struct boot b;

			g_f.fail_read = g_f.nreads + k;
			b = boot(0, &g_probe, NULL);
			CHECK_EQ(b.rc, S02_OK);
			CHECK_EQ(b.slot, 1u);
			CHECK(b.good);
		}
		g_f.fail_read = 0;
	}
	/* a slot that stays unreadable: not established, nothing good to send
	 * from it; the other slot's stream is still sent when it is selected */
	fresh();
	load(g_a, g_alen);
	g_f.fail_regions = FF_R_SLOT0;
	{
		struct boot b = boot(0, &g_probe, NULL);

		CHECK_EQ(b.rc, S02_ERR_IO);
		CHECK_EQ(b.slot, 2u);
		CHECK(!b.good);
		b = boot(1, &g_probe, NULL);
		CHECK_EQ(b.slot, 2u);
		CHECK_EQ(b.sent, 1u);
		CHECK(b.good);
	}
	g_f.fail_regions = FF_R_RECORD0;
	{
		struct boot b = boot(0, &g_probe, NULL);

		CHECK_EQ(b.rc, S02_ERR_IO);
		CHECK(!b.good);
	}
	g_f.fail_regions = 0;

	/* the faster read: used for the stream, once */
	fresh();
	load(g_a, g_alen);
	{
		unsigned from = g_f.nlog;
		struct boot b;

		g_fast_calls = 0;
		b = boot(0, &g_probe, fast_read);
		CHECK_EQ(b.slot, 0u);
		CHECK(b.good);
		CHECK(g_im.by_fast);
		CHECK_EQ(g_fast_calls, 1u);
		/* by the usual read: the probe's chunk and no more */
		CHECK_EQ(payload_reads_of(from, S02_SLOT0_OFF, g_alen), 1u);
		/* it fails: the usual read is the retry */
		g_fast_fails = true;
		b = boot(0, &g_probe, fast_read);
		CHECK_EQ(b.slot, 0u);
		CHECK(b.good);
		CHECK(!g_im.by_fast);
		g_fast_fails = false;
		/* it returns a wrong byte: the CRC fails, and the slot is read at
		 * the usual rate and judged by that */
		g_fast_corrupts = true;
		from = g_f.nlog;
		b = boot(0, NULL, fast_read);
		CHECK_EQ(b.slot, 0u);
		CHECK(b.good);
		CHECK(!g_im.by_fast);
		CHECK_EQ(payload_reads_of(from, S02_SLOT0_OFF, g_alen), 1u);
		g_fast_corrupts = false;
	}

	/* A faster read that returns wrong bytes without an error decides
	 * nothing: it fills the image alone, where the CRC finds it out, and
	 * no probe or comparison reads by it. A trial start: the running slot
	 * is not the selected one, the builds differ, a probe is in use. */
	for (unsigned run = 0; run < 2u; run++) {
		struct boot b;

		fresh();
		load(run ? g_a : g_b, run ? g_alen : g_blen);
		g_fast_corrupts = true;
		g_fast_calls = g_fast_short = 0;
		b = boot(run ? 1u : 0u, &g_probe, fast_read);
		g_fast_corrupts = false;
		CHECK_EQ(b.rc, S02_OK);
		CHECK_EQ(b.first, run ? 0u : 1u);	/* the probe, by the usual read */
		CHECK_EQ(b.slot, run ? 0u : 1u);	/* established: the running slot */
		CHECK_EQ(b.sent, b.slot);
		CHECK(b.good);
		CHECK(!g_im.by_fast);			/* read again at the usual rate */
		CHECK_EQ(g_fast_calls, 1u);		/* the stream's read, nothing else */
		CHECK_EQ(g_fast_short, 0u);
	}
	/* the same with the same build in both slots, where the other slot is
	 * compared whole: by the usual read, so still both and not a verdict
	 * against one; the settled slot */
	ff_init(&g_f);
	put_slot(0, g_a, g_alen, true);
	put_slot(1, g_a, g_alen, true);
	ff_set_selector(&g_f, 0x01, 0x01);
	load(g_a, g_alen);
	g_fast_corrupts = true;
	g_fast_calls = 0;
	{
		struct boot b = boot(1, &g_probe, fast_read);

		CHECK_EQ(b.slot, 1u);
		CHECK_EQ(b.sent, 1u);
		CHECK(b.good);
		CHECK_EQ(g_fast_calls, 1u);
	}
	g_fast_corrupts = false;
	/* an image not yet checked is not compared from: its bytes may be the
	 * faster read's */
	fresh();
	load(g_a, g_alen);
	g_fast_corrupts = true;
	{
		unsigned slot = 9;

		env = ff_env(&g_f, &g_ops);
		s02_image_init(&g_im, g_buf, sizeof g_buf, fast_read, &g_f);
		CHECK_EQ(s02_image_read(&env, 0, &g_im), S02_OK);
		CHECK(g_im.by_fast);
		CHECK_EQ(s02_running_image(&env, &g_mops, &g_probe, &g_im, 0, &slot), S02_OK);
		CHECK_EQ(slot, 0u);
	}
	g_fast_corrupts = false;

	/* A trial start (S-02.016's trial mark: the record's CRC is the
	 * complement of the stream's): the tried slot runs while the selector
	 * names the other. Its stream is read once, by the faster read; the
	 * complement confirms the bytes, so nothing is read again; the tried
	 * slot is established, and it is not good: nothing is sent. */
	for (unsigned v = 0; v < 2u; v++) {
		struct s02_record r;
		uint8_t raw[S02_RECORD_SIZE];
		unsigned from;
		struct boot b;

		fresh();
		s02_record_decode(g_f.mem + s02_record_off(1), &r);
		r.crc32 = ~r.crc32;
		s02_record_encode(&r, raw);
		memcpy(g_f.mem + s02_record_off(1), raw, sizeof raw);
		load(g_b, g_blen);
		g_fast_calls = g_fast_short = 0;
		g_fast_corrupts = v != 0u;
		from = g_f.nlog;
		b = boot(0, &g_probe, fast_read);
		g_fast_corrupts = false;
		CHECK_EQ(b.rc, S02_OK);
		CHECK_EQ(b.first, 1u);
		CHECK_EQ(b.slot, 1u);			/* established: the tried slot */
		CHECK_EQ(b.sent, 1u);
		CHECK(!b.good);				/* and not to be sent */
		CHECK_EQ(g_fast_calls, 1u);
		if (v == 0u) {
			/* one read of the slot: the faster one, kept */
			CHECK(g_im.by_fast && g_im.trial);
			CHECK_EQ(payload_reads_of(from, S02_SLOT1_OFF + 16u + 2048u, g_blen), 0u);
		} else {
			/* wrong bytes: neither the record's CRC nor its complement,
			 * so the usual read, which then finds the trial mark */
			CHECK(!g_im.by_fast && g_im.trial);
			CHECK_EQ(payload_reads_of(from, S02_SLOT1_OFF + 16u + 2048u, g_blen), 1u);
			CHECK(memcmp(g_buf, g_f.mem + S02_SLOT1_OFF, g_blen) == 0);
		}
	}
	/* a CRC that is merely wrong is not a trial mark: read again */
	fresh();
	put_slot(1, g_b, g_blen, false);
	load(g_b, g_blen);
	{
		unsigned from = g_f.nlog;
		struct boot b = boot(0, &g_probe, fast_read);

		CHECK_EQ(b.slot, 1u);
		CHECK(!b.good && !g_im.trial && !g_im.by_fast);
		CHECK_EQ(payload_reads_of(from, S02_SLOT1_OFF + 16u + 2048u, g_blen), 1u);
	}

	/* S-02.016 with a faster CRC (s02_env.crc32_fast, the CRC peripheral on
	 * the board): its match with the record, or the complement, is the
	 * verdict and the usual CRC is not run; any other answer, right or
	 * wrong, is computed again by the usual CRC, whose verdict stands */
	for (unsigned v = 0; v < 6u; v++) {
		/* good, bad, the trial mark; each with a faster CRC right and wrong */
		const unsigned kind = v / 2u;
		const bool liar = (v & 1u) != 0u;

		fresh();
		put_slot(1, g_b, g_blen, kind != 1u);
		if (kind == 2u)
			ff_put32(g_f.mem + s02_record_off(1), ~ref_crc32(0, g_b, g_blen));
		env = ff_env(&g_f, &g_ops);
		env.crc32 = counted_crc32;
		env.crc32_fast = lying_crc32;
		g_lie = liar;
		g_crc_calls = g_fast_crc_calls = 0;
		s02_image_init(&g_im, g_buf, sizeof g_buf, NULL, NULL);
		CHECK_EQ(s02_image_read(&env, 1, &g_im), S02_OK);
		CHECK_EQ(s02_image_check(&env, &g_im), S02_OK);
		CHECK_EQ(g_fast_crc_calls, 1u);
		CHECK_EQ(g_crc_calls, liar || kind == 1u ? 1u : 0u);
		CHECK(g_im.good == (kind == 0u));
		CHECK(g_im.trial == (kind == 2u));
	}
	g_lie = false;

	/* the inference by the image writes nothing either */
	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++)
		CHECK(g_f.log[i].kind == FF_READ);
	return finish("test_s02_running");
}
