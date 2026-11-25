/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09.019: the slave's check of the L1 code the stream loaded
 * (src/board/boot/l1check.c), against the mark tools/mkldr.py fills: the
 * image as loaded passes; a one-bit error at any bit of it fails, and so do
 * a mark left as compiled, a wrong length or CRC, and a run outside L1 code.
 * The bring-up spins without serving the bridge on a failure
 * (src/board/boot/slave.c); that the check runs before the server starts is
 * that function's order.
 */
#include <string.h>

#include "support/check.h"

#include "waffle/board/boot.h"
#include "waffle/boot/memmap.h"
#include "waffle/util/crc.h"

#define BASE S01_L1_CODE_START
#define LEN 0x400u

static uint8_t mem[LEN];
static unsigned reads;

static uint32_t word(uint32_t addr)
{
	const uint32_t at = addr - BASE;

	reads++;
	if (addr < BASE || at + 4u > LEN || (addr & 3u) != 0u)
		return 0xDEADBEEFu;
	return (uint32_t)mem[at] | (uint32_t)mem[at + 1u] << 8 | (uint32_t)mem[at + 2u] << 16 |
	       (uint32_t)mem[at + 3u] << 24;
}

static struct opxy_l1_mark filled(void)
{
	struct opxy_l1_mark mk = { OPXY_L1_MARK_FULL, BASE, LEN, 0u };

	mk.crc = wire_crc32(0u, mem, LEN);
	return mk;
}

static void test_good(void)
{
	struct opxy_l1_mark mk;

	for (uint32_t i = 0; i < LEN; i++)
		mem[i] = (uint8_t)(i * 37u + 11u);
	mk = filled();
	reads = 0;
	CHECK(opxy_l1_check(&mk, word));
	CHECK(reads == LEN / 4u);	/* every word read once */
	/* the compiled mark is the empty one, which never passes */
	CHECK(opxy_l1_mark.magic == OPXY_L1_MARK_EMPTY);
	CHECK(!opxy_l1_check(&opxy_l1_mark, word));
}

static void test_every_bit(void)
{
	const struct opxy_l1_mark mk = filled();
	unsigned passed = 0;

	for (uint32_t i = 0; i < LEN; i++)
		for (unsigned b = 0; b < 8u; b++) {
			mem[i] ^= (uint8_t)(1u << b);
			passed += opxy_l1_check(&mk, word) ? 1u : 0u;
			mem[i] ^= (uint8_t)(1u << b);
		}
	CHECK(passed == 0u);
	CHECK(opxy_l1_check(&mk, word));
}

static void test_bad_marks(void)
{
	struct opxy_l1_mark mk = filled();

	mk.magic = OPXY_L1_MARK_EMPTY;
	CHECK(!opxy_l1_check(&mk, word));
	mk = filled();
	mk.crc ^= 1u;
	CHECK(!opxy_l1_check(&mk, word));
	mk = filled();
	mk.len = LEN - 4u;			/* a shorter run is another CRC */
	CHECK(!opxy_l1_check(&mk, word));
	mk = filled();
	mk.len = 0u;
	CHECK(!opxy_l1_check(&mk, word));
	mk = filled();
	mk.len = LEN + 2u;			/* not whole words */
	CHECK(!opxy_l1_check(&mk, word));
	mk = filled();
	mk.addr = BASE - 4u;			/* outside L1 code */
	reads = 0;
	CHECK(!opxy_l1_check(&mk, word));
	CHECK(reads == 0u);
	mk = filled();
	mk.addr = S01_L1_CODE_END - 4u;		/* past its end */
	reads = 0;
	CHECK(!opxy_l1_check(&mk, word));
	CHECK(reads == 0u);
}

int main(void)
{
	RUN(test_good);
	RUN(test_every_bit);
	RUN(test_bad_marks);
	return check_summary("board_l1check");
}
