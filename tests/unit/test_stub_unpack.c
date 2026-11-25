/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The stub's unpack (src/stub/unpack.c, waffle/boot/pack.h) against blocks
 * tools/mkldr.py packed: a good one gives its runs back; a corrupted, a
 * truncated or a spoiled one, a run where nothing may be written and a
 * stream that does not fill its run are refused, and nothing is written
 * before every check that needs no inflate has passed.
 */
#include <stdint.h>
#include <string.h>

#include "zlib.h"

#include "stub/unpack.h"
#include "support/check.h"
#include "vectors/pack_vectors.h"

#define BUF_MAX 0x10000u

/* Each run's place on the host: a buffer per run address. */
struct map {
	uint32_t addr[WB_PACK_RUNS];
	uint8_t buf[WB_PACK_RUNS][BUF_MAX];
	unsigned n, refuse_at, asked;
};

static struct map m;

static uint8_t *dest(void *ctx, uint32_t addr, uint32_t len)
{
	struct map *p = ctx;

	p->asked++;
	if (p->asked == p->refuse_at || !wb_unpack_writable(addr, len) || len > BUF_MAX ||
	    p->n == WB_PACK_RUNS)
		return NULL;
	p->addr[p->n] = addr;
	return p->buf[p->n++];
}

static uint64_t arena[WB_UNPACK_ARENA / 8u];
static uint8_t blk[sizeof packv_good + 64u];

static void reset(unsigned refuse_at)
{
	memset(&m, 0xA5, sizeof m);
	m.n = 0;
	m.asked = 0;
	m.refuse_at = refuse_at;
}

static enum wb_unpack_rc run(const uint8_t *pk, uint32_t n, uint32_t arena_len, uint32_t *entry)
{
	const struct wb_unpack_ops ops = { dest, &m, arena, arena_len };

	return wb_unpack(pk, n, &ops, entry);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t head_of(const uint8_t *p)
{
	return (uint32_t)p[WB_PACK_O_HEAD] | (uint32_t)p[WB_PACK_O_HEAD + 1u] << 8;
}

/* The streams' CRC and then the head's CRC made right again, after a change. */
static void reseal(uint8_t *p)
{
	const uint32_t head = head_of(p), size = get32(p + WB_PACK_O_SIZE);

	put32(p + WB_PACK_O_ZCRC, (uint32_t)crc32(0, p + head, size - head));
	put32(p + head - 4u, (uint32_t)crc32(0, p, head - 4u));
}

static int untouched(unsigned k)
{
	for (uint32_t i = 0; i < BUF_MAX; i++)
		if (m.buf[k][i] != 0xA5u)
			return 0;
	return 1;
}

static void test_good(void)
{
	uint32_t entry = 0;

	reset(0);
	CHECK(run(packv_good, sizeof packv_good, sizeof arena, &entry) == WB_UNPACK_OK);
	CHECK(entry == PACKV_ENTRY);
	CHECK(m.n == PACKV_RUNS);
	CHECK(m.addr[0] == PACKV_RUN0_ADDR && m.addr[1] == PACKV_RUN1_ADDR);
	CHECK(memcmp(m.buf[0], packv_run0, PACKV_RUN0_LEN) == 0);
	CHECK(memcmp(m.buf[1], packv_run1, PACKV_RUN1_LEN) == 0);
	/* not a byte past either run */
	CHECK(m.buf[0][PACKV_RUN0_LEN] == 0xA5u && m.buf[1][PACKV_RUN1_LEN] == 0xA5u);
	/* more loaded than the block: the block's own size counts */
	reset(0);
	memcpy(blk, packv_good, sizeof packv_good);
	memset(blk + sizeof packv_good, 0x5A, sizeof blk - sizeof packv_good);
	CHECK(run(blk, sizeof blk, sizeof arena, &entry) == WB_UNPACK_OK);
	reset(0);
	CHECK(run(packv_zeros, sizeof packv_zeros, sizeof arena, &entry) == WB_UNPACK_OK);
	CHECK(m.n == 1u && m.addr[0] == PACKV_ZEROS_ADDR);
	for (uint32_t i = 0; i < PACKV_ZEROS_LEN; i += 4093u)
		CHECK(m.buf[0][i] == 0u);
	CHECK(m.buf[0][PACKV_ZEROS_LEN - 1u] == 0u);
}

static void test_truncated(void)
{
	uint32_t entry = 0xDEADu;

	for (uint32_t cut = 0; cut < sizeof packv_good; cut += cut < 200u ? 1u : 997u) {
		reset(0);
		CHECK(run(packv_good, cut, sizeof arena, &entry) != WB_UNPACK_OK);
		CHECK(m.n == 0u && untouched(0));
	}
	reset(0);
	CHECK(run(packv_good, sizeof packv_good - 1u, sizeof arena, &entry) == WB_UNPACK_SIZE);
	CHECK(run(NULL, 0, sizeof arena, &entry) == WB_UNPACK_HEAD);
	CHECK(entry == 0xDEADu);
}

static void test_corrupted(void)
{
	const uint32_t head = head_of(packv_good);
	uint32_t entry = 0;

	/* any byte of the head, or of a stream, with the CRCs as they are */
	for (uint32_t i = 0; i < sizeof packv_good; i += i < head ? 1u : 101u) {
		memcpy(blk, packv_good, sizeof packv_good);
		blk[i] ^= 0x10u;
		reset(0);
		CHECK(run(blk, sizeof packv_good, sizeof arena, &entry) ==
		      (i < head ? WB_UNPACK_HEAD : WB_UNPACK_STREAMS_CRC));
		CHECK(m.n == 0u && untouched(0));
	}
	/* a stream changed with both CRCs made right: inflate or the CRC of
	 * what it wrote refuses it, unless the change gives the same bytes (a
	 * padding bit after a stream's last code) */
	for (uint32_t i = head; i < sizeof packv_good; i += 37u) {
		enum wb_unpack_rc rc;

		memcpy(blk, packv_good, sizeof packv_good);
		blk[i] ^= 0x01u;
		reseal(blk);
		reset(0);
		rc = run(blk, sizeof packv_good, sizeof arena, &entry);
		CHECK(rc == WB_UNPACK_INFLATE || rc == WB_UNPACK_OUT_CRC ||
		      (rc == WB_UNPACK_OK && memcmp(m.buf[0], packv_run0, PACKV_RUN0_LEN) == 0 &&
		       memcmp(m.buf[1], packv_run1, PACKV_RUN1_LEN) == 0));
	}
	/* a version, a count or a head size other than the format's */
	memcpy(blk, packv_good, sizeof packv_good);
	blk[WB_PACK_O_VERSION] = 2u;
	put32(blk + head - 4u, (uint32_t)crc32(0, blk, head - 4u));
	reset(0);
	CHECK(run(blk, sizeof packv_good, sizeof arena, &entry) == WB_UNPACK_HEAD);
}

static void test_spoiled(void)
{
	uint32_t entry = 0xBEEFu;

	reset(0);
	CHECK(run(packv_spoiled, sizeof packv_spoiled, sizeof arena, &entry) == WB_UNPACK_OUT_CRC);
	CHECK(entry == 0xBEEFu);
}

static void test_places(void)
{
	const uint32_t r1 = WB_PACK_O_RUNS + 12u;
	uint32_t entry = 0;

	/* the map's refusal of the second run: nothing written to the first */
	reset(2);
	CHECK(run(packv_good, sizeof packv_good, sizeof arena, &entry) == WB_UNPACK_DEST);
	CHECK(untouched(0));
	/* where the stub writes */
	CHECK(wb_unpack_writable(0x0803C000u, 0x1FD4u));
	CHECK(!wb_unpack_writable(0x0803C000u, 0x1FD5u));
	CHECK(wb_unpack_writable(WB_IMAGE_DDR_START, WB_IMAGE_DDR_END - WB_IMAGE_DDR_START));
	CHECK(!wb_unpack_writable(WB_IMAGE_DDR_END - 4u, 8u));
	CHECK(!wb_unpack_writable(WB_PACK_START, 4u));
	CHECK(!wb_unpack_writable(WB_STUB_START, 4u));
	CHECK(!wb_unpack_writable(0x08030000u, 4u));	/* the shared block */
	CHECK(!wb_unpack_writable(0x11A00000u, 4u));	/* L1 code */
	CHECK(!wb_unpack_writable(0xFFFFFFFCu, 8u));
	/* a run moved into the packed block, or below the run before it */
	memcpy(blk, packv_good, sizeof packv_good);
	put32(blk + r1, WB_PACK_START);
	reseal(blk);
	reset(0);
	CHECK(run(blk, sizeof packv_good, sizeof arena, &entry) == WB_UNPACK_DEST);
	CHECK(m.n == 1u && untouched(0));
	memcpy(blk, packv_good, sizeof packv_good);
	put32(blk + r1, PACKV_RUN0_ADDR);
	reseal(blk);
	reset(0);
	CHECK(run(blk, sizeof packv_good, sizeof arena, &entry) == WB_UNPACK_SIZE);
}

static void test_lengths(void)
{
	const uint32_t r0 = WB_PACK_O_RUNS;
	uint32_t entry = 0;

	/* a run one byte longer or shorter than its stream gives */
	for (int d = -1; d <= 1; d += 2) {
		memcpy(blk, packv_good, sizeof packv_good);
		put32(blk + r0 + 4u, PACKV_RUN0_LEN + (uint32_t)d);
		put32(blk + WB_PACK_O_OUTLEN, get32(blk + WB_PACK_O_OUTLEN) + (uint32_t)d);
		reseal(blk);
		reset(0);
		CHECK(run(blk, sizeof packv_good, sizeof arena, &entry) == WB_UNPACK_INFLATE);
	}
	/* out_len that is not the runs' sum */
	memcpy(blk, packv_good, sizeof packv_good);
	put32(blk + WB_PACK_O_OUTLEN, get32(blk + WB_PACK_O_OUTLEN) + 1u);
	reseal(blk);
	reset(0);
	CHECK(run(blk, sizeof packv_good, sizeof arena, &entry) == WB_UNPACK_SIZE);
	/* no room for inflate's state */
	reset(0);
	CHECK(run(packv_good, sizeof packv_good, 1024u, &entry) == WB_UNPACK_INFLATE);
}

int main(void)
{
	RUN(test_good);
	RUN(test_truncated);
	RUN(test_corrupted);
	RUN(test_spoiled);
	RUN(test_places);
	RUN(test_lengths);
	return check_summary("test_stub_unpack");
}
