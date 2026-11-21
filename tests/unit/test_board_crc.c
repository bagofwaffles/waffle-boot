/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The package's CRCs on the CRC peripheral (src/board/opxy_crc.c,
 * src/drivers/bf70x/crc.c) against a model of CRC0 in memory scan mode fed
 * by DMA18. The model is the test's, not a unit's: what it assumes is
 * named in `struct convention` and in hook() below.
 *
 * The model computes a real bitwise 32-bit CRC of the words DMA18 reads,
 * after CRC_CTL's data mirroring as Table 14-3 gives it, with CRC_POLY and
 * the result registers mirrored as Table 14-6 says, and is set to one of
 * several conventions a unit could have: the engine shifting left or right,
 * the word on the bus little- or big-endian, the result registers holding
 * the running value or its complement. The search must find a working
 * option set in each; a convention that no option set and conversion can
 * match (a CRC that adds each word after the shift) must leave the
 * peripheral unused.
 *
 * Covered: the results equal the software's over many lengths, alignments
 * and running values; a timeout, a DMA error or a wrong answer on a compared
 * piece turns the peripheral off for that CRC for the boot with the result
 * still right; the pieces compared are the first, every 64th, the first of
 * each DMA transfer size and, for the CRC-16, each one whose length is not
 * the last's, so a fault on a new transfer size, or on the CRC-16's last,
 * shorter piece, is caught there; a wrong answer on a piece that is not
 * compared comes back wrong; a deflated member's pieces, whose lengths
 * vary, mostly stay on the peripheral. Then the
 * package's checks on top (opxy_crc.c, s11_engine.h): a member whose
 * CRC-32 the peripheral got wrong is extracted again in software and
 * accepted, its staged file whole, the peripheral off after; a truly bad
 * member is refused MEMBER as the software refuses it, extracted at most
 * twice; check 6 refused because of the peripheral turns it off for CRC-16
 * with a line in the log, and the same package then gets the software's
 * verdict; a truly bad CRC-16 is refused CRC every time. An absent
 * peripheral and a DMA18 in use leave the CRCs to the software; the end
 * leaves the registers at their reset values; nothing touches CRC1 or
 * MDMA2's channels.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_services.h"
#include "waffle/drivers/bf70x_crc.h"
#include "waffle/s11_engine.h"
#include "waffle/s11_package.h"
#include "waffle/s11_staging.h"
#include "waffle/wire_crc.h"
#include "waffle/wire_package.h"

#include "zip_build.h"
#include "zlib.h"

#define BIG (WB_CRC_MAX + 0x3000u)
#define LEAST 0x400u		/* opxy_crc.c's */

static struct opxy_master m;
static _Alignas(32) uint8_t data[BIG + 64u];

/* What a unit's CRC0 might do beyond the manual's words. */
struct convention {
	bool right;		/* the engine shifts right (LSB first) */
	bool big;		/* the bus word has byte 0 in bits 31:24 */
	bool inverted;		/* the result registers hold the complement */
	bool added;		/* each word is shifted in after the state:
				 * no conversion matches the software */
};

static struct {
	struct convention c;
	bool present;		/* CRC_STAT.LUTDONE follows a CRC_POLY write */
	uint32_t poly, stat;
	bool lut, dma_on, crc_on;
	unsigned ops, bad, foreign;
	unsigned silent_at, error_at, wrong_at;	/* the DMA start, from 1 */
	uint32_t good_len;	/* not 0: every other length answered wrong */
	unsigned wrong_msize;	/* not 0: this transfer size answered wrong */
} hw;

static uint32_t rev32(uint32_t x)
{
	uint32_t r = 0;

	for (unsigned i = 0; i < 32u; i++, x >>= 1)
		r = r << 1 | (x & 1u);
	return r;
}

static uint32_t rev8s(uint32_t x)
{
	uint32_t r = 0;

	for (unsigned b = 0; b < 4u; b++)
		r |= rev32((x >> (8u * b)) & 0xFFu) >> 24 << (8u * b);
	return r;
}

/* Table 14-3: bytes mirrored, then halves swapped, then each byte's bits. */
static uint32_t mirror(uint32_t w, uint32_t ctl)
{
	if ((ctl & CRC_CTL_BYTMIRR) != 0u)
		w = w >> 24 | (w >> 8 & 0xFF00u) | (w << 8 & 0xFF0000u) | w << 24;
	if ((ctl & CRC_CTL_W16SWP) != 0u)
		w = w >> 16 | w << 16;
	return (ctl & CRC_CTL_BITMIRR) != 0u ? rev8s(w) : w;
}

static uint32_t step(uint32_t s, uint32_t w)
{
	if (hw.c.added) {
		for (int i = 31; i >= 0; i--) {
			const uint32_t top = s >> 31;

			s = s << 1 | (w >> i & 1u);
			if (top != 0u)
				s ^= hw.poly;
		}
		return s;
	}
	s ^= w;
	for (unsigned i = 0; i < 32u; i++) {
		if (hw.c.right)
			s = (s & 1u) != 0u ? s >> 1 ^ hw.poly : s >> 1;
		else
			s = (s & 0x80000000u) != 0u ? s << 1 ^ hw.poly : s << 1;
	}
	return s;
}

/* A result register's value from the engine's state and back. */
static uint32_t to_reg(uint32_t s, uint32_t ctl)
{
	if (hw.c.inverted)
		s = ~s;
	return (ctl & CRC_CTL_RSLTMIRR) != 0u ? rev32(s) : s;
}

static uint32_t from_reg(uint32_t r, uint32_t ctl)
{
	if ((ctl & CRC_CTL_RSLTMIRR) != 0u)
		r = rev32(r);
	return hw.c.inverted ? ~r : r;
}

static uint32_t low(const void *p)
{
	return (uint32_t)(uintptr_t)p;
}

/* DMA18 started: the scan, all at once. */
static void scan(uint32_t cfg)
{
	const uint32_t ctl = fake_mmr_get(CRC0_CTL);
	const unsigned ms = cfg >> DMA_CFG_MSIZE_SHIFT & 7u;
	const uint8_t *p = opxy_crc_dev.src;
	const uint32_t len = opxy_crc_dev.len;
	const uint32_t at = fake_mmr_get(DMA_ADDRSTART(WB_CRC_DMA));
	uint32_t s;

	hw.ops++;
	if (!hw.crc_on || (ctl & CRC_CTL_OPMODE_MASK) != CRC_CTL_OPMODE_SCAN ||
	    !hw.lut || (cfg & DMA_CFG_WNR) != 0u ||
	    (cfg >> DMA_CFG_PSIZE_SHIFT & 7u) != WB_DMA_4B || ms < 2u ||
	    ms > 5u || fake_mmr_get(DMA_XMOD(WB_CRC_DMA)) != 1u << ms ||
	    at != low(p) || (at & ((1u << ms) - 1u)) != 0u ||
	    fake_mmr_get(DMA_XCNT(WB_CRC_DMA)) << ms != len ||
	    fake_mmr_get(CRC0_DCNT) * 4u != len)
		hw.bad++;
	if (hw.ops == hw.silent_at)
		return;
	if (hw.ops == hw.error_at) {
		fake_mmr_set(DMA_STAT(WB_CRC_DMA), DMA_STAT_IRQERR);
		return;
	}
	s = from_reg(fake_mmr_get(CRC0_RESULT_CUR), ctl);
	for (uint32_t i = 0; i < len; i += 4u) {
		const uint32_t w = hw.c.big ?
			(uint32_t)p[i] << 24 | (uint32_t)p[i + 1u] << 16 |
			(uint32_t)p[i + 2u] << 8 | p[i + 3u] :
			(uint32_t)p[i] | (uint32_t)p[i + 1u] << 8 |
			(uint32_t)p[i + 2u] << 16 | (uint32_t)p[i + 3u] << 24;

		s = step(s, mirror(w, ctl));
	}
	if (hw.ops == hw.wrong_at || (hw.good_len != 0u && len != hw.good_len) ||
	    (hw.wrong_msize != 0u && ms == hw.wrong_msize))
		s ^= 0x00810000u;	/* in both CRCs' bits */
	fake_mmr_set(CRC0_RESULT_CUR, to_reg(s, ctl));
	fake_mmr_set(CRC0_RESULT_FIN, to_reg(s, ctl));
	hw.stat |= CRC_STAT_DCNTEXP;
	if (to_reg(s, ctl) != fake_mmr_get(CRC0_COMP))
		hw.stat |= CRC_STAT_CMPERR;
	fake_mmr_set(DMA_STAT(WB_CRC_DMA), DMA_STAT_IRQDONE);
}

static bool hook(void *ctx, enum fake_mmr_op op, unsigned w, wb_mmr_addr addr,
		 uint32_t *v)
{
	(void)ctx;
	(void)w;
	if ((addr >= 0x200B1000u && addr < 0x200B2000u) ||
	    (addr >= DMA_BASE(19u) && addr < DMA_BASE(21u) + 0x1000u))
		hw.foreign++;		/* CRC1, DMA19, MDMA2 */
	if (op == FAKE_MMR_READ && addr == CRC0_STAT)
		*v = hw.stat;
	if (op != FAKE_MMR_WRITE)
		return true;
	switch (addr) {
	case CRC0_STAT:
		hw.stat &= ~(*v & CRC_STAT_W1C);
		break;
	case CRC0_POLY:
		hw.poly = (fake_mmr_get(CRC0_CTL) & CRC_CTL_POLYMIRR) != 0u ?
			  rev32(*v) : *v;
		hw.lut = hw.present;
		hw.stat = (hw.stat & ~CRC_STAT_LUTDONE) |
			  (hw.present ? CRC_STAT_LUTDONE : 0u);
		break;
	case CRC0_CTL:
		/* restriction 2: the channel off before the block */
		if ((*v & CRC_CTL_BLKEN) == 0u && hw.crc_on && hw.dma_on)
			hw.bad++;
		hw.crc_on = (*v & CRC_CTL_BLKEN) != 0u;
		break;
	case DMA_STAT(WB_CRC_DMA):
		fake_mmr_set(addr, fake_mmr_get(addr) &
				   ~(*v & (DMA_STAT_IRQDONE | DMA_STAT_IRQERR |
					   DMA_STAT_PIRQ)));
		return false;
	case DMA_CFG(WB_CRC_DMA):
		hw.dma_on = (*v & DMA_CFG_EN) != 0u;
		if (hw.dma_on) {
			fake_mmr_set(addr, *v);
			scan(*v);
			return false;
		}
		break;
	default:
		break;
	}
	return true;
}

static void start(struct convention c, bool present)
{
	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(1000000u);	/* a silent unit times out soon */
	fake_mmr_set_hook(hook, NULL);
	memset(&hw, 0, sizeof(hw));
	hw.c = c;
	hw.present = present;
	memset(&m, 0, sizeof(m));
	m.hw.time.cycles_per_us = 400u;
	opxy_crc_off = 0;
	opxy_crc_begin(&m);
}

static const struct convention plain = { false, false, false, false };
static const struct convention conventions[] = {
	{ false, false, false, false },		/* left, little-endian bus */
	{ true, false, false, false },		/* right */
	{ false, true, true, false },		/* big-endian bus, inverted */
	{ true, true, true, false },
	{ false, false, true, false },
};

static uint32_t ref(unsigned which, uint32_t v, const uint8_t *p, size_t n)
{
	return which == OPXY_CRC16 ? wire_s11_crc16(NULL, (uint16_t)v, p, n) :
				     wire_s11_crc32(NULL, v, p, n);
}

static uint32_t board(unsigned which, uint32_t v, const uint8_t *p, size_t n)
{
	return which == OPXY_CRC16 ? opxy_crc16(NULL, (uint16_t)v, p, n) :
				     opxy_crc32(NULL, v, p, n);
}

/* CRC-16/XMODEM is the top half of the 32-bit CRC with 0x1021 shifted up,
 * from the running value shifted up likewise: what opxy_crc.c relies on. */
static void test_crc16_on_32(void)
{
	static const size_t lens[] = { 0, 1, 2, 3, 9, 100, 1021, 4096 };

	for (size_t i = 0; i < ARRAY_LEN(lens); i++) {
		for (uint32_t v = 0; v < 0x10000u; v += 0x1357u) {
			uint32_t s = v << 16;

			for (size_t k = 0; k < lens[i]; k++) {
				s ^= (uint32_t)data[k] << 24;
				for (unsigned b = 0; b < 8u; b++)
					s = (s & 0x80000000u) != 0u ?
					    s << 1 ^ 0x10210000u : s << 1;
			}
			CHECK((s & 0xFFFFu) == 0u &&
			      s >> 16 == ref(OPXY_CRC16, v, data, lens[i]));
		}
	}
}

/* Each convention: both CRCs found, then the results the software's. */
static void test_conventions(void)
{
	static const size_t lens[] = { 0, 1, 3, 4, 0x3FF, 0x400, 0x401,
				       0x1003, 0x8000, 0x8000 + 7u, 100000 };

	for (size_t c = 0; c < ARRAY_LEN(conventions); c++) {
		start(conventions[c], true);
		CHECK(opxy_crc_engine(OPXY_CRC16) && opxy_crc_engine(OPXY_CRC32));
		CHECK(hw.bad == 0u && opxy_crc_off == 0u);
		for (unsigned which = 0; which < OPXY_CRCS; which++) {
			const unsigned before = hw.ops;

			for (size_t i = 0; i < ARRAY_LEN(lens); i++) {
				for (size_t off = 0; off < 4u; off++) {
					const uint32_t v = (uint32_t)(i * 0x9E3779B9u + off) &
							   (which == OPXY_CRC16 ? 0xFFFFu : ~0u);

					CHECK(board(which, v, data + off, lens[i]) ==
					      ref(which, v, data + off, lens[i]));
				}
			}
			CHECK(hw.ops > before);
			/* more than one operation's worth */
			CHECK(board(which, 7u, data + 1, BIG) ==
			      ref(which, 7u, data + 1, BIG));
		}
		CHECK(opxy_crc_engine(OPXY_CRC16) && opxy_crc_engine(OPXY_CRC32));
		CHECK(hw.bad == 0u && hw.foreign == 0u);
		opxy_crc_end();
	}
}

/* Pieces the peripheral takes: whole aligned ones of LEAST or more, an
 * unaligned head in software first. */
static void test_pieces(void)
{
	unsigned before;

	start(plain, true);
	before = hw.ops;
	(void)opxy_crc32(NULL, 0, data, LEAST - 4u);
	(void)opxy_crc32(NULL, 0, data + 2, LEAST + 1u);	/* LEAST - 1 after the head */
	CHECK(hw.ops == before);
	(void)opxy_crc32(NULL, 0, data, LEAST);
	CHECK(hw.ops == before + 1u);
	(void)opxy_crc32(NULL, 0, data + 3, LEAST + 3u);	/* head 1, LEAST + 2 */
	CHECK(hw.ops == before + 2u);
	(void)opxy_crc16(NULL, 0, data, WB_CRC_MAX + LEAST);	/* two */
	CHECK(hw.ops == before + 4u && hw.bad == 0u);
	opxy_crc_end();
}

/* No option set and conversion matches, or no peripheral answers: the
 * software's, and the CRCs stay off for the boot. */
static void test_never(void)
{
	static const struct convention added = { false, false, false, true };
	unsigned ops;

	start(added, true);
	CHECK(!opxy_crc_engine(OPXY_CRC16) && !opxy_crc_engine(OPXY_CRC32));
	/* the whole search, every run but the first ruled out at once */
	CHECK(hw.ops == 2u * 16u * 4u && opxy_crc_off == 3u && hw.bad == 0u);
	ops = hw.ops;
	CHECK(opxy_crc32(NULL, 5u, data, 0x8000) == ref(OPXY_CRC32, 5u, data, 0x8000));
	CHECK(opxy_crc16(NULL, 5u, data, 0x8000) == ref(OPXY_CRC16, 5u, data, 0x8000));
	CHECK(hw.ops == ops);
	opxy_crc_end();
	/* the next use does not search again */
	opxy_crc_begin(&m);
	CHECK(hw.ops == ops && !opxy_crc_engine(OPXY_CRC32));
	opxy_crc_end();

	start(plain, false);		/* no LUTDONE: absent */
	CHECK(!opxy_crc_engine(OPXY_CRC16) && !opxy_crc_engine(OPXY_CRC32));
	CHECK(hw.ops == 0u && opxy_crc_off == 3u);
	CHECK(opxy_crc32(NULL, 0, data, 0x8000) == ref(OPXY_CRC32, 0, data, 0x8000));
	opxy_crc_end();
	CHECK(fake_mmr_get(CRC0_CTL) == 0u && fake_mmr_get(CRC0_POLY) == 0u);
}

/* A timeout, a DMA error or a wrong answer in the search: that CRC off. */
static void test_search_fails(void)
{
	for (unsigned k = 0; k < 3u; k++) {
		fake_mmr_reset();
		fake_core_reset();
		fake_core_set_cycle_step(1000000u);
		fake_mmr_set_hook(hook, NULL);
		memset(&hw, 0, sizeof(hw));
		hw.present = true;
		/* plain finds CRC-16 at the third option set (BYTMIRR): its
		 * first operation fails */
		*(k == 0u ? &hw.silent_at : k == 1u ? &hw.error_at : &hw.wrong_at) = 1u;
		opxy_crc_off = 0;
		opxy_crc_begin(&m);
		CHECK(opxy_crc_engine(OPXY_CRC16) == (k == 2u));
		CHECK(opxy_crc_engine(OPXY_CRC32));
		CHECK(opxy_crc_off == (k == 2u ? 0u : 1u) && hw.bad == 0u);
		CHECK(opxy_crc16(NULL, 1u, data, 0x8000) == ref(OPXY_CRC16, 1u, data, 0x8000));
		CHECK(!wb_dma_enabled(WB_CRC_DMA) && fake_mmr_get(CRC0_CTL) == 0u);
		opxy_crc_end();
	}
}

/* In use: a failing operation is redone in software and ends the use; a
 * wrong answer is caught on a compared piece only. */
static void test_in_use(void)
{
	for (unsigned k = 0; k < 2u; k++) {
		for (unsigned which = 0; which < OPXY_CRCS; which++) {
			start(plain, true);
			*(k == 0u ? &hw.silent_at : &hw.error_at) = hw.ops + 2u;
			CHECK(board(which, 9u, data, 0x8000) == ref(which, 9u, data, 0x8000));
			CHECK(board(which, 9u, data, 0x8000) == ref(which, 9u, data, 0x8000));
			CHECK(!opxy_crc_engine(which) && opxy_crc_engine(1u - which));
			CHECK(opxy_crc_off == 1u << which && hw.bad == 0u);
			CHECK(!wb_dma_enabled(WB_CRC_DMA) && fake_mmr_get(CRC0_CTL) == 0u);
			opxy_crc_end();
		}
	}

	/* piece 1 is compared: the software's answer, the CRC off */
	start(plain, true);
	hw.wrong_at = hw.ops + 1u;
	CHECK(opxy_crc32(NULL, 0, data, 0x8000) == ref(OPXY_CRC32, 0, data, 0x8000));
	CHECK(!opxy_crc_engine(OPXY_CRC32) && opxy_crc_engine(OPXY_CRC16));
	opxy_crc_end();

	/* pieces 2 to 64 are not: a wrong one comes back wrong, and piece 65
	 * is compared again */
	start(plain, true);
	hw.wrong_at = hw.ops + 2u;
	CHECK(opxy_crc32(NULL, 0, data, 0x8000) == ref(OPXY_CRC32, 0, data, 0x8000));
	CHECK(opxy_crc32(NULL, 0, data, 0x8000) != ref(OPXY_CRC32, 0, data, 0x8000));
	for (unsigned i = 2; i < 64u; i++)
		CHECK(opxy_crc32(NULL, 0, data, LEAST) == ref(OPXY_CRC32, 0, data, LEAST));
	CHECK(opxy_crc_engine(OPXY_CRC32));
	hw.wrong_at = hw.ops + 1u;
	CHECK(opxy_crc32(NULL, 0, data, LEAST) == ref(OPXY_CRC32, 0, data, LEAST));
	CHECK(!opxy_crc_engine(OPXY_CRC32) && hw.bad == 0u);
	opxy_crc_end();
}

/* A fault tied to a shape is met where it shows: a last, shorter piece and
 * the first piece of a new transfer size are compared, so the result is
 * still the software's and the peripheral is off after. Pieces of one
 * shape between every 64th are not compared. */
static void test_shapes(void)
{
	for (unsigned which = 0; which < OPXY_CRCS; which++) {
		uint32_t v = 3u, want = 3u;
		unsigned ops;

		/* every length but 0x8000 answered wrong: the last, shorter
		 * piece, of the same transfer size (32 bytes), is compared for
		 * the CRC-16 and not for the CRC-32 (a member's retry stands
		 * behind it) */
		start(plain, true);
		hw.good_len = 0x8000u;
		for (uint32_t k = 0; k < 5u; k++) {
			v = board(which, v, data + k * 0x8000u, 0x8000u);
			want = ref(which, want, data + k * 0x8000u, 0x8000u);
		}
		CHECK(v == want && opxy_crc_engine(which));
		ops = hw.ops;
		CHECK(wb_crc_msize(low(data + 5u * 0x8000u), 0x1220u) ==
		      wb_crc_msize(low(data), 0x8000u));
		v = board(which, v, data + 5u * 0x8000u, 0x1220u);
		want = ref(which, want, data + 5u * 0x8000u, 0x1220u);
		CHECK(hw.ops == ops + 1u);
		if (which == OPXY_CRC16)
			CHECK(v == want && !opxy_crc_engine(which) &&
			      opxy_crc_off == 1u << which);
		else
			CHECK(v != want && opxy_crc_engine(which));
		opxy_crc_end();

		/* a last piece of a new transfer size is compared for both */
		start(plain, true);
		hw.good_len = 0x8000u;
		v = want = 3u;
		v = board(which, v, data, 0x8000u);
		want = ref(which, want, data, 0x8000u);
		v = board(which, v, data + 0x8000u, 0x1234u);
		want = ref(which, want, data + 0x8000u, 0x1234u);
		CHECK(v == want && !opxy_crc_engine(which));
		opxy_crc_end();

		/* 4-byte transfers answered wrong: the first one */
		start(plain, true);
		hw.wrong_msize = WB_DMA_4B;
		v = want = 3u;
		for (uint32_t k = 0; k < 3u; k++) {
			v = board(which, v, data + k * 0x8000u, 0x8000u);
			want = ref(which, want, data + k * 0x8000u, 0x8000u);
		}
		CHECK(v == want && opxy_crc_engine(which));
		CHECK(wb_crc_msize(low(data + 4), 0x8000u) == WB_DMA_4B);
		v = board(which, v, data + 4, 0x8000u);
		want = ref(which, want, data + 4, 0x8000u);
		CHECK(v == want && !opxy_crc_engine(which));
		opxy_crc_end();

		/* one shape throughout: the third piece is not compared */
		start(plain, true);
		hw.wrong_at = hw.ops + 3u;
		v = want = 3u;
		for (uint32_t k = 0; k < 4u; k++) {
			v = board(which, v, data, 0x8000u);
			want = ref(which, want, data, 0x8000u);
		}
		CHECK(v != want && opxy_crc_engine(which));
		opxy_crc_end();
	}
}

/* ---- a deflated member's pieces ---- */

#define DEFLATED 400000u

static _Alignas(32) uint8_t inflated[0x8000];

/*
 * The CRC-32 of a deflated member as wire_zip.c's extraction feeds it:
 * inflate() given 16 KiB of input at a time and up to 32 KiB of output, each
 * piece of output through opxy_crc32(). *pieces counts them, *changes those
 * whose length is not the one before's.
 */
static uint32_t inflate_crc32(const uint8_t *comp, size_t clen, unsigned *pieces,
			      unsigned *changes)
{
	z_stream s;
	uint32_t crc = 0;
	size_t at = 0, left = DEFLATED, before = 0;
	int ret = Z_OK;

	memset(&s, 0, sizeof(s));
	*pieces = *changes = 0;
	if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
		return 0;
	while (left != 0u && ret == Z_OK) {
		if (s.avail_in == 0u && at < clen) {
			const size_t n = clen - at < 0x4000u ? clen - at : 0x4000u;

			s.next_in = (Bytef *)(uintptr_t)(comp + at);
			s.avail_in = (uInt)n;
			at += n;
		}
		s.next_out = inflated;
		s.avail_out = left < sizeof(inflated) ? (uInt)left : sizeof(inflated);
		ret = inflate(&s, Z_SYNC_FLUSH);
		if (ret == Z_OK || ret == Z_STREAM_END) {
			const size_t got = (size_t)(s.next_out - inflated);

			left -= got;
			crc = opxy_crc32(NULL, crc, inflated, got);
			*pieces += 1u;
			*changes += got != before ? 1u : 0u;
			before = got;
		}
	}
	(void)inflateEnd(&s);
	return crc;
}

/* A deflated member's pieces change length at nearly every call: the
 * CRC-32 compares only every 64th and the first of each transfer size, so
 * most of them stay on the peripheral. Each piece is made wrong in turn: a
 * wrong one that is compared is caught (the CRC-32 still right). */
static void test_deflated_pieces(void)
{
	uint8_t *raw = malloc(DEFLATED), *comp = malloc(DEFLATED * 9u / 8u + 16u);
	size_t clen;
	uint32_t want;
	unsigned pieces = 0, changes = 0, ops, caught = 0;

	CHECK(raw != NULL && comp != NULL);
	if (raw == NULL || comp == NULL) {
		free(raw);
		free(comp);
		return;
	}
	fill(raw, DEFLATED, 41u);
	clen = deflate_fixed(raw, DEFLATED, comp);
	want = ref(OPXY_CRC32, 0, raw, DEFLATED);
	start(plain, true);
	ops = hw.ops;
	CHECK(inflate_crc32(comp, clen, &pieces, &changes) == want);
	ops = hw.ops - ops;
	CHECK(opxy_crc_engine(OPXY_CRC32) && ops >= 16u);
	opxy_crc_end();
	for (unsigned k = 1; k <= ops; k++) {
		unsigned n, c;

		start(plain, true);
		hw.wrong_at = hw.ops + k;
		if (inflate_crc32(comp, clen, &n, &c) == want)
			caught++;
		CHECK(n == pieces);
		opxy_crc_end();
	}
	printf("  deflated member: %u pieces in %u lengths, %u on the peripheral, %u of them compared in software\n",
	       pieces, changes, ops, caught);
	/* the lengths vary, and most pieces stay on the peripheral */
	CHECK(changes * 2u >= pieces && caught * 4u <= ops);
	free(raw);
	free(comp);
}

/* The log's lines that name the CRC peripheral. */
static unsigned crc_lines;

static bool log_send(void *ctx, uint8_t route, uint8_t id, const uint8_t *p,
		     size_t n)
{
	(void)ctx;
	(void)route;
	(void)id;
	for (size_t i = 0; i + 4u <= n; i++)
		crc_lines += memcmp(p + i, "crc:", 4) == 0 ? 1u : 0u;
	return true;
}

static const struct s14_link_ops log_link = { log_send, NULL };

static void start_logged(void)
{
	start(plain, true);
	m.sv.log.threshold = S14_INFO;
	m.sv.log.cdc = &log_link;
	m.sv.log.cdc_on = true;
	crc_lines = 0;
}

/* ---- a member's CRC-32 (S-11.030) ---- */

#define MEMBER (5u * 0x8000u + 0x1234u)

static struct {
	uint32_t crc;			/* stated */
	unsigned extracts, opens;
	size_t len;			/* of the staged file */
	uint8_t file[MEMBER];
} Z;

static bool z_member(void *ctx, size_t i, struct s11_member *mb)
{
	(void)ctx;
	if (i != 0u)
		return false;
	memset(mb, 0, sizeof(*mb));
	memcpy(mb->name, "a.bin", 6);
	mb->size = MEMBER;
	mb->crc32 = Z.crc;
	return true;
}

static bool z_zip64(void *ctx)
{
	(void)ctx;
	return false;
}

/* data[0, MEMBER) in pieces of 0x8000 bytes */
static bool z_extract(void *ctx, size_t i, s11_sink_fn sink, void *arg)
{
	(void)ctx;
	(void)i;
	Z.extracts++;
	for (uint32_t at = 0; at < MEMBER; at += 0x8000u) {
		if (!sink(arg, data + at, MEMBER - at < 0x8000u ? MEMBER - at : 0x8000u))
			return false;
	}
	return true;
}

static bool z_free(void *ctx, uint64_t *bytes)
{
	(void)ctx;
	*bytes = 1u << 30;
	return true;
}

static bool z_yes(void *ctx)
{
	(void)ctx;
	return true;
}

static bool z_mkdir(void *ctx, const char *path)
{
	(void)ctx;
	(void)path;
	return true;
}

/* created empty, as the volume's staging files are */
static bool z_open(void *ctx, const char *path)
{
	(void)ctx;
	(void)path;
	Z.opens++;
	Z.len = 0;
	return true;
}

static bool z_write(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	if (n > MEMBER - Z.len)
		return false;
	memcpy(Z.file + Z.len, p, n);
	Z.len += n;
	return true;
}

static const struct s11_zip_ops zops = {
	z_member, z_zip64, z_extract, opxy_crc32, z_free, z_yes, z_mkdir,
	z_open, z_write, z_yes, NULL,
};

static enum s11_reason check_member(uint32_t crc)
{
	struct s11_staged_sizes st;

	Z.crc = crc;
	Z.extracts = Z.opens = 0;
	Z.len = 0;
	return s11_check_zip(&zops, &st);
}

/* A wrong answer on a piece between compared ones: the member is extracted
 * again with the software's CRC-32 and gets the software's verdict, its
 * staged file whole; the peripheral is off after, and a later member is
 * extracted once. */
static void test_member(void)
{
	const uint32_t good = ref(OPXY_CRC32, 0, data, MEMBER);
	enum s11_reason r;

	for (unsigned bad = 0; bad < 2u; bad++) {
		const enum s11_reason want = bad ? S11_REASON_MEMBER : S11_REASON_NONE;
		unsigned ops;

		start(plain, true);
		CHECK(s11_crc_engine != NULL);
		hw.wrong_at = hw.ops + 2u;
		ops = hw.ops;
		r = check_member(good ^ bad);
		CHECK(r == want && Z.extracts == 2u && Z.opens == 2u);
		CHECK(Z.len == MEMBER && !memcmp(Z.file, data, MEMBER));
		/* the first extraction's six pieces only */
		CHECK(hw.ops == ops + 6u && hw.bad == 0u);
		CHECK(!opxy_crc_engine(OPXY_CRC32) && opxy_crc_engine(OPXY_CRC16) &&
		      opxy_crc_off == 2u);
		opxy_crc_end();
		CHECK(s11_crc_engine == NULL);

		/* the next install: software, one extraction */
		opxy_crc_begin(&m);
		ops = hw.ops;
		r = check_member(good ^ bad);
		CHECK(r == want && Z.extracts == 1u && hw.ops == ops);
		opxy_crc_end();
	}

	/* a peripheral that answers right: one extraction, still in use */
	start(plain, true);
	r = check_member(good);
	CHECK(r == S11_REASON_NONE && Z.extracts == 1u && opxy_crc_engine(OPXY_CRC32));
	/* a truly bad member it took part in: extracted again, refused as the
	 * software refuses it, the peripheral off for CRC-32 */
	r = check_member(good ^ 0x100u);
	CHECK(r == S11_REASON_MEMBER && Z.extracts == 2u && !opxy_crc_engine(OPXY_CRC32));
	r = check_member(good ^ 0x100u);
	CHECK(r == S11_REASON_MEMBER && Z.extracts == 1u);
	opxy_crc_end();
}

/* ---- check 6 (S-11.008) ---- */

#define PKG_BODY (0x80u + 0x300u + 0x100u)
#define PKG_LEN (PKG_BODY + 5u * 0x8000u + 0x1234u)

static uint8_t pkg[PKG_LEN];

static bool no_q0(void *ctx, uint8_t q[56])
{
	(void)ctx;
	(void)q;
	return false;
}

static bool no_key(void *ctx, uint8_t key[32])
{
	(void)ctx;
	(void)key;
	return false;
}

static const struct s11_unit_ops no_otp = { no_q0, no_key, no_key };

/* A package whose checks 1-6 pass (with its CRC-16 right) and check 7
 * refuses (FORMAT): a payload of 5 pieces of 32 KiB and a shorter one. */
static void make_package(bool bad_crc)
{
	static const uint8_t pre[5] = { 0xBA, 0xBE, 0xCA, 0xFE, 0x00 };
	static const uint8_t hdr[5] = { 0xBE, 0xEF, 0xCA, 0xFE, 0x01 };
	static const uint8_t product[4] = { 0x00, 0x08, 0x40, 0x00 };
	uint16_t crc;

	memset(pkg, 0, PKG_BODY);
	memcpy(pkg, pre, sizeof(pre));
	memcpy(pkg + 0x40, hdr, sizeof(hdr));
	memcpy(pkg + 0x40 + 0x17, product, sizeof(product));
	memcpy(pkg + 0x80, data, PKG_LEN - 0x80u);
	crc = wire_s11_crc16(NULL, 0, pkg + 0x80, PKG_LEN - 0x80u);
	crc ^= bad_crc ? 1u : 0u;
	pkg[0x49] = (uint8_t)(crc >> 8);
	pkg[0x4A] = (uint8_t)crc;
}

static enum s11_reason check_package(const struct s11_crypto_ops *crypto)
{
	static struct wire_package p;
	static struct wire_mem mem;
	static uint8_t zip[64];
	const struct wire_source src = { wire_mem_read, &mem, NULL };
	struct s11_package_ops ops;
	struct s11_check_in in = { .form = S11_FORM_FILE, .size = PKG_LEN,
				   .dev = &wb_device_opxy };
	struct s11_check_out out;

	mem.buf = pkg;
	mem.len = PKG_LEN;
	wire_package_init(&p, &ops, &src, &no_otp, NULL, zip, sizeof(zip));
	ops.crypto = crypto;
	return s11_check_package(&ops, &in, &out);
}

/*
 * A wrong answer on a piece between compared ones refuses check 6 once:
 * the peripheral is off for CRC-16 after, with a line in the log, and the
 * same package then gets the software's verdict. A wrong answer on the
 * last, shorter piece refuses nothing. A truly bad CRC-16 is refused CRC
 * every time.
 */
static void test_check6(void)
{
	struct s11_crypto_ops crypto = wire_crypto_ops;

	crypto.crc16 = opxy_crc16;
	for (unsigned bad = 0; bad < 2u; bad++) {
		enum s11_reason soft, r;
		unsigned ops;

		make_package(bad != 0u);
		soft = check_package(&wire_crypto_ops);
		CHECK(soft == (bad ? S11_REASON_CRC : S11_REASON_FORMAT));

		start_logged();
		hw.wrong_at = hw.ops + 2u;
		r = check_package(&crypto);
		CHECK(r == S11_REASON_CRC && crc_lines == 1u);
		CHECK(!opxy_crc_engine(OPXY_CRC16) && opxy_crc_engine(OPXY_CRC32) &&
		      opxy_crc_off == 1u && hw.bad == 0u);
		opxy_crc_end();
		opxy_crc_begin(&m);
		ops = hw.ops;
		r = check_package(&crypto);
		CHECK(r == soft && hw.ops == ops && crc_lines == 1u);
		opxy_crc_end();

		start_logged();
		hw.good_len = 0x8000u;
		r = check_package(&crypto);
		CHECK(r == soft && !opxy_crc_engine(OPXY_CRC16));
		CHECK(crc_lines == bad);
		opxy_crc_end();
	}
}

/* The end: the registers the driver writes as reset leaves them, and the
 * software from then on; nothing written without a use, or with DMA18 in
 * another's use. */
static void test_end(void)
{
	static const wb_mmr_addr zero[] = {
		CRC0_CTL, CRC0_DCNT, CRC0_DCNTRLD, CRC0_COMP, CRC0_INEN,
		CRC0_POLY, CRC0_RESULT_CUR, CRC0_RESULT_FIN,
		DMA_CFG(WB_CRC_DMA), DMA_ADDRSTART(WB_CRC_DMA),
		DMA_XCNT(WB_CRC_DMA), DMA_XMOD(WB_CRC_DMA), DMA_STAT(WB_CRC_DMA),
	};
	unsigned ops;

	start(plain, true);
	CHECK(opxy_crc32(NULL, 0, data, 0x8000) == ref(OPXY_CRC32, 0, data, 0x8000));
	opxy_crc_end();
	for (size_t i = 0; i < ARRAY_LEN(zero); i++)
		CHECK(fake_mmr_get(zero[i]) == 0u);
	CHECK((hw.stat & CRC_STAT_W1C) == 0u && !hw.dma_on && !hw.crc_on);
	CHECK(!opxy_crc_engine(OPXY_CRC16) && !opxy_crc_engine(OPXY_CRC32));
	ops = hw.ops;
	CHECK(opxy_crc32(NULL, 0, data, 0x8000) == ref(OPXY_CRC32, 0, data, 0x8000));
	CHECK(hw.ops == ops && hw.foreign == 0u);

	fake_mmr_clear_log();
	opxy_crc_end();
	CHECK(fake_mmr_count() == 0u);

	/* DMA18 enabled by someone else: read, never written */
	fake_mmr_reset();
	fake_mmr_set_hook(hook, NULL);
	memset(&hw, 0, sizeof(hw));
	hw.present = true;
	fake_mmr_set(DMA_CFG(WB_CRC_DMA), DMA_CFG_EN);
	opxy_crc_off = 0;
	opxy_crc_begin(&m);
	CHECK(!opxy_crc_engine(OPXY_CRC16) && !opxy_crc_engine(OPXY_CRC32));
	opxy_crc_end();
	for (size_t i = 0; i < fake_mmr_count(); i++)
		CHECK(fake_mmr_log()[i].op != FAKE_MMR_WRITE);
	CHECK(opxy_crc_off == 0u && hw.ops == 0u);
}

/* A check's CRC-32 (opxy_crc32_fast()), outside any use. */
static unsigned idles;

static void idle_count(void)
{
	idles++;
}

static void bare(struct convention c, bool present)
{
	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(1000000u);
	fake_mmr_set_hook(hook, NULL);
	memset(&hw, 0, sizeof(hw));
	hw.c = c;
	hw.present = present;
	m.hw.time.cycles_per_us = 400u;
	opxy_crc_off = 0;
	idles = 0;
}

static uint32_t fast(uint32_t v, const uint8_t *p, uint32_t n)
{
	return opxy_crc32_fast(v, p, n, &m.hw.time, idle_count);
}

/* Registers as reset leaves them, after the call. */
static bool left_reset(void)
{
	return fake_mmr_get(CRC0_CTL) == 0u && fake_mmr_get(CRC0_POLY) == 0u &&
	       fake_mmr_get(CRC0_RESULT_FIN) == 0u && !wb_dma_enabled(WB_CRC_DMA) &&
	       !hw.dma_on && !hw.crc_on;
}

/*
 * S-02.016's and S-03.044's CRC-32 on the peripheral, each result against
 * the software's: every convention, lengths and alignments, an image's
 * length in pieces with the idle hook between them, the registers reset
 * after each call, the conventions found last tried first. The software's
 * alone in another use, below LEAST, with the peripheral absent or DMA18
 * taken; a failed operation leaves a right result and the peripheral off.
 * A wrong answer with a good status comes back wrong: the caller's miss,
 * computed again in software (s02_image_check(), s03_load_os()).
 */
static void test_fast(void)
{
	static const size_t lens[] = { LEAST, LEAST + 1u, 0x1003, 0x10000, 0x10003, 100000, BIG };
	unsigned ops;

	for (size_t c = 0; c < ARRAY_LEN(conventions); c++) {
		bare(conventions[c], true);
		for (size_t i = 0; i < ARRAY_LEN(lens); i++) {
			for (size_t off = 0; off < 4u; off++) {
				const uint32_t v = (uint32_t)(i * 0x9E3779B9u + off);

				ops = hw.ops;
				CHECK(fast(v, data + off, (uint32_t)lens[i]) ==
				      ref(OPXY_CRC32, v, data + off, lens[i]));
				CHECK(opxy_crc_fast_used && hw.ops > ops);
				CHECK(left_reset() && hw.bad == 0u && hw.foreign == 0u);
			}
		}
		CHECK(opxy_crc_off == 0u);
	}
	/* the last call's conventions answered at once: three runs, then the
	 * pieces; the idle hook after each piece */
	bare(plain, true);
	(void)fast(0, data, LEAST);
	ops = hw.ops;
	idles = 0;
	CHECK(fast(1u, data, 0x30000u) == ref(OPXY_CRC32, 1u, data, 0x30000u));
	CHECK(hw.ops == ops + 3u + 3u && idles == 3u);

	/* the software's: short, in another use, DMA18 taken, absent */
	bare(plain, true);
	CHECK(fast(2u, data, LEAST - 1u) == ref(OPXY_CRC32, 2u, data, LEAST - 1u));
	CHECK(hw.ops == 0u && !opxy_crc_fast_used);
	start(plain, true);
	ops = hw.ops;
	CHECK(fast(2u, data, 0x8000) == ref(OPXY_CRC32, 2u, data, 0x8000));
	CHECK(hw.ops == ops && !opxy_crc_fast_used && opxy_crc_engine(OPXY_CRC32));
	opxy_crc_end();
	bare(plain, true);
	fake_mmr_set(DMA_CFG(WB_CRC_DMA), DMA_CFG_EN);
	CHECK(fast(2u, data, 0x8000) == ref(OPXY_CRC32, 2u, data, 0x8000));
	CHECK(hw.ops == 0u && !opxy_crc_fast_used && opxy_crc_off == 0u);
	bare(plain, false);
	CHECK(fast(2u, data, 0x8000) == ref(OPXY_CRC32, 2u, data, 0x8000));
	CHECK(!opxy_crc_fast_used && opxy_crc_off == 1u << OPXY_CRC32 && left_reset());
	ops = hw.ops;
	CHECK(fast(2u, data, 0x8000) == ref(OPXY_CRC32, 2u, data, 0x8000));
	CHECK(hw.ops == ops);		/* off for the boot */

	/* an operation that fails: the rest in software, right, and off */
	for (unsigned k = 0; k < 2u; k++) {
		bare(plain, true);
		(void)fast(0, data, LEAST);
		*(k == 0u ? &hw.silent_at : &hw.error_at) = hw.ops + 3u + 2u;
		CHECK(fast(3u, data + 1, BIG) == ref(OPXY_CRC32, 3u, data + 1, BIG));
		CHECK(!opxy_crc_fast_used && opxy_crc_off == 1u << OPXY_CRC32 && left_reset());
	}

	/* a wrong answer on a piece: a wrong result, for the caller to miss */
	bare(plain, true);
	(void)fast(0, data, LEAST);
	hw.wrong_at = hw.ops + 3u + 2u;
	CHECK(fast(4u, data, BIG) != ref(OPXY_CRC32, 4u, data, BIG));
	CHECK(opxy_crc_fast_used && left_reset());

	/* conventions that changed since the last call are searched again */
	bare(conventions[1], true);
	(void)fast(0, data, LEAST);
	bare(conventions[2], true);
	CHECK(fast(5u, data, 0x8000) == ref(OPXY_CRC32, 5u, data, 0x8000));
	CHECK(opxy_crc_fast_used && hw.ops > 3u + 1u);
}

/* The driver: its stream is MDMA1's source, not the display's MDMA2, and
 * it refuses what it cannot run without a register written. */
static void test_driver(void)
{
	struct wb_crc *c = &opxy_crc_dev;	/* the record the model reads */
	uint32_t s = 0;

	CHECK(WB_CRC_DMA == 18u && WB_CRC_DMA != WB_DMA_MDMA_SRC(2u) &&
	      WB_CRC_DMA != WB_DMA_MDMA_DST(2u));
	start(plain, true);
	opxy_crc_end();
	fake_mmr_clear_log();
	wb_crc_init(c, &m.hw.time);
	CHECK(wb_crc_run(c, 0x04C11DB7u, 0, &s, data + 2, 8) == WB_CRC_ERR_ARG);
	CHECK(wb_crc_run(c, 0x04C11DB7u, 0, &s, data, 6) == WB_CRC_ERR_ARG);
	CHECK(wb_crc_run(c, 0x04C11DB7u, 0, &s, data, 0) == WB_CRC_ERR_ARG);
	CHECK(wb_crc_run(c, 0x04C11DB7u, 0, &s, data, WB_CRC_MAX + 4u) ==
	      WB_CRC_ERR_ARG);
	CHECK(wb_crc_run(c, 0x04C11DB7u, CRC_CTL_BLKEN, &s, data, 8) ==
	      WB_CRC_ERR_ARG);
	CHECK(fake_mmr_count() == 0u);
	fake_mmr_set(DMA_CFG(WB_CRC_DMA), DMA_CFG_EN);
	CHECK(wb_crc_run(c, 0x04C11DB7u, 0, &s, data, 8) == WB_CRC_BUSY);
	for (size_t i = 0; i < fake_mmr_count(); i++)
		CHECK(fake_mmr_log()[i].op != FAKE_MMR_WRITE);
	fake_mmr_set(DMA_CFG(WB_CRC_DMA), 0u);

	/* the raw CRC over one word with no option: a left shift's */
	{
		static _Alignas(4) const uint8_t one[4] = { 0x01, 0, 0, 0 };
		uint32_t want = 1u;

		for (unsigned b = 0; b < 32u; b++)
			want = (want & 0x80000000u) != 0u ? want << 1 ^ 0x04C11DB7u :
							    want << 1;
		s = 0;
		CHECK(wb_crc_run(c, 0x04C11DB7u, 0, &s, one, 4) == WB_CRC_OK &&
		      s == want);
		/* POLYMIRR: the reflected polynomial reversed back */
		s = 0;
		CHECK(wb_crc_run(c, 0xEDB88320u, CRC_CTL_POLYMIRR, &s, one, 4) ==
		      WB_CRC_OK && s == want);
	}
	CHECK(!wb_dma_enabled(WB_CRC_DMA) && fake_mmr_get(CRC0_CTL) == 0u &&
	      hw.bad == 0u && hw.foreign == 0u);
}

int main(void)
{
	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 29u + (i >> 9) + (i >> 3) * 5u + 11u);
	RUN(test_crc16_on_32);
	RUN(test_conventions);
	RUN(test_pieces);
	RUN(test_never);
	RUN(test_search_fails);
	RUN(test_in_use);
	RUN(test_shapes);
	RUN(test_deflated_pieces);
	RUN(test_member);
	RUN(test_check6);
	RUN(test_end);
	RUN(test_fast);
	RUN(test_driver);
	return check_summary("test_board_crc");
}
