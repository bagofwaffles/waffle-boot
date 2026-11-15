/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Start-up steps between reset and main() (see waffle/drivers/bf70x_start.h).
 */
#include "waffle/drivers/bf70x_start.h"

#include <string.h>

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_coremmr.h"
#include "waffle/drivers/bf70x_rcu.h"
#include "waffle/drivers/mmr.h"

const struct wb_l1_range wb_l1_bf703[4] = {
	{ 0x11800000u, 0x11808000u },	/* data A, SRAM and cache halves */
	{ 0x11900000u, 0x11908000u },	/* data B */
	{ 0x11A00000u, 0x11A10000u },	/* code, SRAM and cache */
	{ 0x11B00000u, 0x11B02000u },	/* scratch, record and ROM stack */
};

void wb_crt_sections(const struct wb_crt_section *t, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		if (t[i].len == 0 || t[i].dst == t[i].src)
			continue;
		if (t[i].src)
			memcpy(t[i].dst, t[i].src, t[i].len);
		else
			memset(t[i].dst, 0, t[i].len);
	}
}

#if defined(__bfin__)
/*
 * S-13a.040, PRM "Extended Data Access": loads and stores to the
 * restricted L1 spaces (code, cache) go only through DAG0; one through
 * DAG1 is a protection violation. Each word is read from its own address
 * and stored back there, never through another address. Each load and
 * store is a single-issue instruction on P registers, never a multi-issue
 * instruction's second slot (DAG1), and the word stays in a register
 * between its read and its write, so the stack in L1 scratch is not
 * written in between. A CSYNC follows the writes, before any L1 code is
 * fetched. No range is cache while this runs: wb_start_init() has
 * cleared L1IM_ICTL.CFG and L1DM_DCTL.CFG (wb_cplb_disable()), and the
 * pass runs from DDR.
 */
static void l1_rewrite(uint32_t start, uint32_t end)
{
	uint32_t a = start, w;

	if (start >= end)
		return;
	__asm__ __volatile__(".Lwb_l1_%=:\n\t"
			     "%1 = [%0];\n\t"
			     "[%0++] = %1;\n\t"
			     "cc = %0 < %2 (iu);\n\t"
			     "if cc jump .Lwb_l1_%=;\n\t"
			     "csync;"
			     : "+a"(a), "=&d"(w)
			     : "a"(end)
			     : "CC", "memory");
}
#else
static void l1_rewrite(uint32_t start, uint32_t end)
{
	uint32_t a;

	for (a = start; a < end; a += 4u)
		wb_mmr_write32(a, wb_mmr_read32(a));
}
#endif

void wb_l1_parity_init(const struct wb_l1_range *r, unsigned n)
{
	unsigned i;

	/*
	 * S-13a.040: read parity checking off on both sides before any word
	 * is read (start.S has already cleared it; this keeps the pass safe
	 * on its own), and ENX on.
	 */
	wb_mmr_clear32(L1IM_ICTL, L1IM_ICTL_RDCHK);
	wb_mmr_write32(L1DM_DCTL, (wb_mmr_read32(L1DM_DCTL) &
				   ~(uint32_t)L1DM_DCTL_RDCHK) | L1DM_DCTL_ENX);
	wb_core_csync();
	for (i = 0; i < n; i++)
		l1_rewrite(r[i].start, r[i].end);
	wb_mmr_sync();
	wb_mmr_clear32(L1DM_DCTL, L1DM_DCTL_ENX);
	wb_core_csync();
}

void wb_l1_parity_on(void)
{
	wb_mmr_set32(L1IM_ICTL, L1IM_ICTL_RDCHK);
	wb_mmr_set32(L1DM_DCTL, L1DM_DCTL_RDCHK);
	wb_mmr_sync();
	wb_core_csync();
}

int wb_start_init(const struct wb_start_config *cfg)
{
	wb_cplb_disable();
	if ((wb_mmr_read32(RCU0_MSG) & RCU_MSG_C0L1INIT) == 0) {
		wb_l1_parity_init(cfg->l1, cfg->n_l1);
		wb_mmr_write32(RCU0_MSG_SET, RCU_MSG_C0L1INIT);
		wb_mmr_sync();
	}
	/*
	 * S-13a.040: read parity checking on, straight after the pass, or
	 * where it would have been on a warm start; nothing turns it off
	 * again (wb_cplb_setup(), wb_cplb_disable() and the hand-over keep it).
	 */
	wb_l1_parity_on();
	wb_crt_sections(cfg->sections, cfg->n_sections);
	if (cfg->cplb == NULL)
		return WB_CPLB_OK;
	return wb_cplb_setup(cfg->cplb);
}
