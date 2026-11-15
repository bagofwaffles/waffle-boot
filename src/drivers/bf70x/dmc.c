/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Dynamic Memory Controller, DDR2 (see waffle/drivers/bf70x_dmc.h). */
#include "waffle/drivers/bf70x_dmc.h"

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/mmr.h"

int wb_dmc_check(const struct wb_dmc_config *c)
{
	const uint32_t fixed_cfg = (2u << DMC_CFG_IFWID_SHIFT) |
				   (2u << DMC_CFG_SDRWID_SHIFT);
	const uint32_t ctl_banned = DMC_CTL_LPDDR | DMC_CTL_INIT |
				    DMC_CTL_SRREQ | DMC_CTL_PDREQ |
				    DMC_CTL_DPDREQ;

	if (c->dclk_mhz == 0 || (c->phy_ctl4 & 3u) != DMC_PHY_CTL4_DDR2 ||
	    (c->cfg & 0xF0FFu) != fixed_cfg ||
	    (c->ctl & DMC_CTL_RDTOWR_MASK) != (2u << DMC_CTL_RDTOWR_SHIFT) ||
	    (c->ctl & ctl_banned) != 0 ||
	    (c->ctl & DMC_CTL_DLLCAL) == 0 ||	/* S-13a.018 step 4 */
	    ((c->dllctl >> DMC_DLLCTL_DATACYC_SHIFT) & 0xFu) != 5u ||
	    (c->cal_padctl0 & DMC_CAL_CALSTRT) != 0)
		return WB_DMC_ERR_ARG;
	return WB_DMC_OK;
}

bool wb_dmc_aliases(uint32_t base_addr, uint32_t alias_addr)
{
	const uint32_t a = wb_mmr_read32(base_addr);
	const uint32_t b = wb_mmr_read32(alias_addr);
	bool aliased;

	wb_mmr_write32(base_addr, 0x5A5A5A5Au);
	wb_mmr_write32(alias_addr, 0xA5A5A5A5u);
	wb_mmr_sync();
	/* A cached copy of base_addr would hide the alias. */
	wb_core_flushinv_line(base_addr);
	wb_mmr_sync();
	aliased = wb_mmr_read32(base_addr) == 0xA5A5A5A5u;
	/* If they alias, the second write leaves a, which equals b. */
	wb_mmr_write32(alias_addr, b);
	wb_mmr_write32(base_addr, a);
	wb_mmr_sync();
	return aliased;
}
