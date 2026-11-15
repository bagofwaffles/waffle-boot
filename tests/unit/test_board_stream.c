/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The slave's stream source on the board (src/board/opxy_master.c): the two
 * steps the slave start takes, called as it calls them, on a fake flash.
 * A normal start; a trial start (the tried slot runs, is established, and
 * is not sent: S-09.002); byte-identical slots (established: the slot
 * later starts settle on, with no log line), a trial of the image the kept
 * slot holds among them; a selector that cannot be read (not established:
 * the log line); a slot that runs while the selector names the other; a slot that is not the one read first; and a faster read
 * that returns wrong bytes.
 */
#include <string.h>

#include "s02_check.h"
#include "s02_fake_flash.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/s05_handoff.h"

#define MEMB 0x88040000u	/* the image's code and constant data */
#define MEML 0x4000u

static struct opxy_master m;
static struct fake_flash g_f;
static struct s02_flash_ops g_ops;
static uint8_t g_mem[MEML];
static uint8_t g_a[0x2000], g_b[0x2000];
static uint32_t g_alen, g_blen;

static bool mem_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (addr < MEMB || (uint64_t)addr + len > (uint64_t)MEMB + MEML)
		return false;
	memcpy(buf, g_mem + (addr - MEMB), len);
	return true;
}

static const struct s02_mem_ops g_mops = { NULL, mem_read };

/* The faster read: counted; told to, it returns wrong bytes silently. */
static unsigned g_fast;
static bool g_fast_wrong;

static int fast_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	const struct fake_flash *f = ctx;

	g_fast++;
	memcpy(buf, f->mem + off, len);
	if (g_fast_wrong)
		for (uint32_t i = 0x40u; i < len; i += 0x100u)
			buf[i] ^= 0x80u;
	return 0;
}

static const struct opxy_stream_src g_src = {
	&g_mops, { MEMB + 0x40u, 8u }, fast_read, &g_f,
};

/* The log: the lines that say the running slot was not established. */
static unsigned g_warned;

static bool log_send(void *ctx, uint8_t route, uint8_t id, const uint8_t *p, size_t n)
{
	static const char text[] = "running slot not established";

	(void)ctx;
	(void)route;
	(void)id;
	for (size_t i = 0; i + sizeof text - 1u <= n; i++)
		g_warned += memcmp(p + i, text, sizeof text - 1u) == 0 ? 1u : 0u;
	return true;
}

static uint32_t log_now(void *ctx)
{
	(void)ctx;
	return 0;
}

static const struct s14_link_ops log_link = { log_send, NULL };
static const struct s14_clock_ops log_clock = { log_now, NULL };

/* A stream: a block of code, FINAL. */
static uint32_t make_stream(uint8_t *s, uint32_t seed)
{
	static uint8_t code[3000];
	uint8_t x = 0;
	uint32_t o;

	ff_fill_image(code, sizeof code, seed);
	ff_put32(s, 0xAD000000u);
	ff_put32(s + 4, MEMB);
	ff_put32(s + 8, sizeof code);
	ff_put32(s + 12, 0);
	for (unsigned i = 0; i < 16; i++)
		x = (uint8_t)(x ^ s[i]);
	s[2] = x;
	memcpy(s + 16, code, sizeof code);
	o = 16u + (uint32_t)sizeof code;
	ff_put32(s + o, 0xAD000000u | S05_FLAG_FINAL);
	ff_put32(s + o + 4, MEMB);
	ff_put32(s + o + 8, 0);
	ff_put32(s + o + 12, 0);
	x = 0;
	for (unsigned i = 0; i < 16; i++)
		x = (uint8_t)(x ^ s[o + i]);
	s[o + 2] = x;
	return o + 16u;
}

/* crc: 0 good, 1 wrong, 2 the trial mark (S-02.016) */
static void put_slot(unsigned slot, const uint8_t *s, uint32_t len, unsigned crc)
{
	const uint32_t c = ref_crc32(0, s, len);
	struct s02_record r = { crc == 0u ? c : crc == 1u ? c ^ 1u : ~c, len, S02_RECORD_TAG };
	uint8_t raw[S02_RECORD_SIZE];

	memcpy(g_f.mem + s02_slot_base(slot), s, len);
	s02_record_encode(&r, raw);
	memcpy(g_f.mem + s02_record_off(slot), raw, sizeof raw);
}

static void set_up(unsigned selected, const uint8_t *loaded)
{
	ff_init(&g_f);
	g_f.mem[S02_SELECTOR_OFF] = (uint8_t)selected;
	g_f.mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	memset(&m, 0, sizeof m);
	m.flash = ff_env(&g_f, &g_ops);
	s14_log_init(&m.sv.log, S14_MASTER, &log_clock, NULL, &log_link, NULL);
	m.sv.log.cdc_on = true;
	memset(g_mem, 0x5A, sizeof g_mem);
	memcpy(g_mem, loaded + 16, 3000);
	g_warned = 0;
	g_fast = 0;
	g_fast_wrong = false;
}

int main(void)
{
	const uint8_t *stream = NULL;
	bool established = false;
	uint32_t len = 9;
	unsigned slot;

	g_alen = make_stream(g_a, 1);
	g_blen = make_stream(g_b, 2);

	/* a normal start: the selected slot runs; its stream, read once */
	for (unsigned sel = 0; sel < 2u; sel++) {
		set_up(sel, sel ? g_b : g_a);
		put_slot(0, g_a, g_alen, 0);
		put_slot(1, g_b, g_blen, 0);
		slot = opxy_stream_slot(&m, &g_src, &established);
		CHECK(established);
		CHECK_EQ(slot, sel);
		CHECK(opxy_stream_checked(&m, slot, &stream, &len));
		CHECK_EQ(len, sel ? g_blen : g_alen);
		CHECK(memcmp(stream, sel ? g_b : g_a, len) == 0);
		CHECK_EQ(g_fast, 1u);
		CHECK_EQ(g_warned, 0u);
	}

	/* a trial start: the tried slot runs while the selector names the
	 * other; it is established, and with its trial mark it is not sent */
	set_up(0, g_b);
	put_slot(0, g_a, g_alen, 0);
	put_slot(1, g_b, g_blen, 2);
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK(established);
	CHECK_EQ(slot, 1u);
	CHECK(!opxy_stream_checked(&m, slot, &stream, &len));
	CHECK_EQ(len, 0u);
	CHECK_EQ(g_warned, 0u);
	/* its stream read once, by the faster read: the trial mark's CRC
	 * confirms the bytes, and no read of the whole slot follows */
	CHECK_EQ(g_fast, 1u);
	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++)
		CHECK(g_f.log[i].len <= 2048u);
	/* the same one boot late (S-02.023): a bad CRC */
	set_up(1, g_a);
	put_slot(0, g_a, g_alen, 1);
	put_slot(1, g_b, g_blen, 0);
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK(established);
	CHECK_EQ(slot, 0u);
	CHECK(!opxy_stream_checked(&m, slot, &stream, &len));
	CHECK_EQ(len, 0u);

	/* byte-identical slots: established, the slot later starts settle on
	 * (S-02.054's tie-break): the selected slot when the selector is
	 * verified or that slot is good, otherwise the other; no log line. The
	 * selected slot's record good, with a CRC that fails, and with the
	 * trial mark: the last is a trial of the image the kept slot holds,
	 * before and after the first stage has gone back to the kept slot. */
	for (unsigned c = 0; c < 2u * 2u * 3u; c++) {
		const unsigned sel = c & 1u, rs = c / 4u;
		const bool ver = (c & 2u) == 0u;
		const unsigned want = ver || rs == 0u ? sel : 1u - sel;

		set_up(sel, g_a);
		g_f.mem[S02_SELECTOR_OFF + 1u] = ver ? 0x01u : 0x00u;
		put_slot(sel, g_a, g_alen, rs);
		put_slot(1u - sel, g_a, g_alen, 0);
		slot = opxy_stream_slot(&m, &g_src, &established);
		CHECK(established);
		CHECK_EQ(slot, want);
		CHECK_EQ(g_warned, 0u);
		/* the stream's one read is the selected slot's: its check is
		 * the image's, and nothing reads a whole slot again */
		CHECK_EQ(g_fast, 1u);
		for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++)
			CHECK(g_f.log[i].len <= 2048u || rs == 1u);
		/* S-09.002: sent when good; a selected slot that is not good is
		 * the answer only under a verified selector, and is not sent */
		if (want == sel && rs != 0u) {
			CHECK(!opxy_stream_checked(&m, slot, &stream, &len));
			CHECK_EQ(len, 0u);
		} else {
			CHECK(opxy_stream_checked(&m, slot, &stream, &len));
			CHECK_EQ(len, g_alen);
			CHECK(memcmp(stream, g_a, len) == 0);
		}
	}
	/* the selector cannot be read for it: not established; slot 0, its
	 * stream, and the log line */
	set_up(1, g_a);
	put_slot(0, g_a, g_alen, 0);
	put_slot(1, g_a, g_alen, 0);
	g_f.fail_regions = FF_R_SELECTOR;
	slot = opxy_stream_slot(&m, &g_src, &established);
	g_f.fail_regions = 0;
	CHECK(!established);
	CHECK_EQ(slot, 0u);
	CHECK_EQ(g_warned, 1u);
	CHECK(opxy_stream_checked(&m, slot, &stream, &len));
	CHECK_EQ(len, g_alen);

	/* slot 1 runs while the selector names slot 0: slot 1 is established
	 * and its stream sent (S-01.002) */
	set_up(0, g_b);
	put_slot(0, g_a, g_alen, 0);
	put_slot(1, g_b, g_blen, 0);
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK(established);
	CHECK_EQ(slot, 1u);
	CHECK(opxy_stream_checked(&m, slot, &stream, &len));
	CHECK_EQ(len, g_blen);
	CHECK(memcmp(stream, g_b, len) == 0);

	/* a slot other than the one read first: read then, and checked */
	set_up(0, g_a);
	put_slot(0, g_a, g_alen, 0);
	put_slot(1, g_b, g_blen, 0);
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK_EQ(slot, 0u);
	CHECK(opxy_stream_checked(&m, 1u, &stream, &len));
	CHECK_EQ(len, g_blen);
	CHECK(memcmp(stream, g_b, len) == 0);
	CHECK_EQ(g_fast, 2u);

	/* a faster read that returns wrong bytes: the same slot, the right
	 * bytes, in a trial-like start with the builds differing */
	set_up(0, g_b);
	put_slot(0, g_a, g_alen, 0);
	put_slot(1, g_b, g_blen, 0);
	g_fast_wrong = true;
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK(established);
	CHECK_EQ(slot, 1u);
	CHECK(opxy_stream_checked(&m, slot, &stream, &len));
	CHECK_EQ(len, g_blen);
	CHECK(memcmp(stream, g_b, len) == 0);
	CHECK_EQ(g_fast, 1u);
	CHECK_EQ(g_warned, 0u);

	/* the two steps write nothing */
	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++)
		CHECK(g_f.log[i].kind == FF_READ);
	return finish("test_board_stream");
}
