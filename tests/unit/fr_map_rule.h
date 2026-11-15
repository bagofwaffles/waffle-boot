/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The rule a fake struct fr_mem keeps for the front's tests, stated apart
 * from the front's own table (fr_mem_readable()): the front never reads as
 * data L1 code (L1DM_DCTL.ENX is clear, S-13a.040), the L1 holes and
 * scratch, the first stage's stack while the ROM loads (S-02.091 rule 4),
 * the ROM's boot reserve, the front's own windows, the hand-off record's
 * half of the shared block, or recovery's staging buffer (S-01.002,
 * S-01.023).
 *
 * A fake map for the image check fails the test on any such request
 * (CHECK(!fr_map_forbidden(addr, len))); one for the running slot, whose
 * streams have blocks there, refuses it (returns NULL).
 */
#ifndef FR_MAP_RULE_H
#define FR_MAP_RULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool fr_map_forbidden(uint32_t addr, uint32_t len)
{
	static const struct {
		uint32_t lo, hi;
	} never[] = {
		{ 0x11A00000u, 0x11A0C000u },	/* L1 code */
		{ 0x11808000u, 0x11900000u },	/* holes: L1 data A's aliases */
		{ 0x11908000u, 0x11A00000u },	/* L1 data B's */
		{ 0x11A0C000u, 0x11B00000u },	/* past L1 code */
		{ 0x11B00000u, 0x11B02000u },	/* L1 scratch */
		{ 0x11807000u, 0x11808000u },	/* the first stage's stack */
		{ 0x11802000u, 0x11804000u },	/* the front's L1 data */
		{ 0x08000000u, 0x08002000u },	/* the ROM's boot reserve */
		{ 0x08002000u, 0x08030000u },	/* the front's L2 */
		{ 0x08030800u, 0x08031000u },	/* the hand-off record */
		{ 0x88400000u, 0x88478000u },	/* recovery's staging buffer */
	};
	const uint64_t end = (uint64_t)addr + (len ? len : 1u);
	size_t i;

	for (i = 0; i < sizeof never / sizeof never[0]; i++)
		if (addr < never[i].hi && never[i].lo < end)
			return true;
	return false;
}

#endif /* FR_MAP_RULE_H */
