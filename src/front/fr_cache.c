/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The front's cache pages (see fr_cache.h); the host's tests check them. */
#include "fr_cache.h"

#include "fr_handoff.h"

#define D_RW (CPLB_VALID | DCPLB_SWRITE | DCPLB_DIRTY)

/* The front's code in L2 (fr_handoff.h's FR_L2_*), the main image's L2
 * part and the shared block. */
const struct wb_cplb fr_icplb[FR_ICPLB_N] = {
	{ 0x08000000u, CPLB_VALID | ICPLB_CACHE | WB_CPLB_PSIZE(WB_CPLB_256K) },
};

/* L2 as above; DDR 0x88000000-0x8C000000: the main image's DDR part
 * (from 0x8803D000) the image check reads, and recovery's staging
 * (FR_STAGING_*) the logo and the writer read. FR_FRAME_* lies outside
 * every page. */
const struct wb_cplb fr_dcplb[FR_DCPLB_N] = {
	{ 0x08000000u, D_RW | DCPLB_WT | WB_CPLB_PSIZE(WB_CPLB_256K) },
	{ 0x88000000u, D_RW | DCPLB_WT | WB_CPLB_PSIZE(WB_CPLB_64M) },
};

_Static_assert(FR_STAGING_START >= 0x88000000u && FR_STAGING_END <= 0x8C000000u,
	       "recovery's staging lies in the cached DDR page");
_Static_assert(FR_FRAME_START >= 0x8C000000u, "the frame buffer lies outside it");
_Static_assert(FR_L2_START >= 0x08000000u && FR_SHARED_END <= 0x08040000u,
	       "the front's L2 lies in the L2 page");
