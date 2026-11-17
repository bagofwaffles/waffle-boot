/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09.019, with BOOT_LINK_FC=1 or 2: the slave's check of the L1 code the
 * stream loaded, the one part of the main image the front's check does not
 * read (waffle/board/opxy_boot.h).
 */
#include "waffle/board/opxy_boot.h"

#if OPXY_BOOT_LINK_FC && !defined(OPXY_TEST_OS)
#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_coremmr.h"
#include "waffle/drivers/bf70x_cplb.h"
#include "waffle/drivers/mmr.h"
#include "waffle/s01_memmap.h"
#include "waffle/wire_crc.h"

/* Filled by tools/mkldr.py in the stream's bytes; volatile, so that the
 * compiled values are never taken for the loaded ones. */
const volatile struct opxy_l1_mark opxy_l1_mark = { OPXY_L1_MARK_EMPTY, 0u, 0u, 0u };

/* Words read at a time into a buffer the CRC then reads. */
#define CHUNK_WORDS 64u

bool opxy_l1_check(const volatile struct opxy_l1_mark *mk, uint32_t (*word)(uint32_t addr))
{
	const uint32_t addr = mk->addr, len = mk->len, want = mk->crc;
	uint8_t buf[4u * CHUNK_WORDS];
	uint32_t crc = 0u, at = 0u;

	if (mk->magic != OPXY_L1_MARK_FULL || len == 0u || ((addr | len) & 3u) != 0u ||
	    addr < S01_L1_CODE_START || addr >= S01_L1_CODE_END ||
	    len > S01_L1_CODE_END - addr)
		return false;
	while (at < len) {
		const uint32_t k = len - at < sizeof buf ? len - at : (uint32_t)sizeof buf;
		uint32_t i;

		/* a word's bytes in memory order: this core is little-endian */
		for (i = 0; i < k; i += 4u) {
			const uint32_t v = word(addr + at + i);

			buf[i] = (uint8_t)v;
			buf[i + 1u] = (uint8_t)(v >> 8);
			buf[i + 2u] = (uint8_t)(v >> 16);
			buf[i + 3u] = (uint8_t)(v >> 24);
		}
		crc = wire_crc32(crc, buf, k);
		at += k;
	}
	return crc == want;
}

/* PRM, Extended Data Access: with ENX set, loads may read L1 instruction
 * memory. */
static uint32_t l1_word(uint32_t addr)
{
	return *(const volatile uint32_t *)(uintptr_t)addr;
}

bool opxy_l1_code_ok(void)
{
	const uint32_t saved = wb_mmr_read32(L1DM_DCTL);
	const uint32_t mask = wb_core_cli();
	bool ok;

	wb_mmr_write32(L1DM_DCTL, saved | L1DM_DCTL_ENX);
	wb_core_csync();
	ok = opxy_l1_check(&opxy_l1_mark, l1_word);
	wb_mmr_write32(L1DM_DCTL, saved);
	wb_core_csync();
	wb_core_sti(mask);
	return ok;
}
#else
typedef int opxy_l1check_absent;	/* a translation unit is not empty */
#endif
