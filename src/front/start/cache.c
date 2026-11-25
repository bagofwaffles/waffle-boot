/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The front's cache pages (see start/cache.h); the host's tests check them. */
#include "front/start/cache.h"

#include "front/handoff.h"

#define D_RW (CPLB_VALID | DCPLB_SWRITE | DCPLB_DIRTY)
#define D_RO (CPLB_VALID)

/* The main image's instruction pages (drivers/bf70x/cplb.c): L1 uncached,
 * the boot ROM, L2 (the front's code, the main image's L2 part and the
 * shared block) and DDR cached. */
const struct wb_cplb fr_icplb[FR_ICPLB_N] = {
	{ 0x11800000u, CPLB_VALID | ICPLB_NOCACHE | WB_CPLB_PSIZE(WB_CPLB_4M) },
	{ 0x04000000u, CPLB_VALID | ICPLB_CACHE | WB_CPLB_PSIZE(WB_CPLB_1M) },
	{ 0x08000000u, CPLB_VALID | ICPLB_CACHE | WB_CPLB_PSIZE(WB_CPLB_256K) },
	{ 0x80000000u, CPLB_VALID | ICPLB_CACHE | WB_CPLB_PSIZE(WB_CPLB_256M) },
};

/* The main image's data pages, with DDR in four 64 MB pages as the
 * hand-over's (board/boot/handover.c): the last, which holds FR_FRAME_*,
 * uncached. L1 uncached, the boot ROM read-only, L2 and the rest of DDR
 * write-through (the main image's DDR part and recovery's staging, which
 * the image check, the logo and the writer read), the SMC bank (the panel)
 * I/O device space, the OTP uncached and read-only. */
const struct wb_cplb fr_dcplb[FR_DCPLB_N] = {
	{ 0x11800000u, D_RW | DCPLB_NOCACHE | WB_CPLB_PSIZE(WB_CPLB_4M) },
	{ 0x04000000u, D_RO | DCPLB_WT | WB_CPLB_PSIZE(WB_CPLB_1M) },
	{ 0x08000000u, D_RW | DCPLB_WT | WB_CPLB_PSIZE(WB_CPLB_256K) },
	{ 0x80000000u, D_RW | DCPLB_WT | WB_CPLB_PSIZE(WB_CPLB_64M) },
	{ 0x84000000u, D_RW | DCPLB_WT | WB_CPLB_PSIZE(WB_CPLB_64M) },
	{ 0x88000000u, D_RW | DCPLB_WT | WB_CPLB_PSIZE(WB_CPLB_64M) },
	{ 0x8C000000u, D_RW | DCPLB_NOCACHE | WB_CPLB_PSIZE(WB_CPLB_64M) },
	{ 0x70000000u, D_RW | DCPLB_IO | WB_CPLB_PSIZE(WB_CPLB_64M) },
	{ 0x38000000u, D_RO | DCPLB_NOCACHE | WB_CPLB_PSIZE(WB_CPLB_4K) },
};

_Static_assert(FR_ICPLB_N <= FR_CPLB_PAIRS && FR_DCPLB_N <= FR_CPLB_PAIRS, "sixteen pairs a side");
_Static_assert(FR_STAGING_START >= 0x88000000u && FR_STAGING_END <= 0x8C000000u,
	       "recovery's staging lies in a cached DDR page");
_Static_assert(FR_FRAME_START >= 0x8C000000u && FR_FRAME_END <= 0x90000000u,
	       "the frame buffer lies in the uncached one");
_Static_assert(FR_L2_START >= 0x08000000u && FR_SHARED_END <= 0x08040000u,
	       "the front's L2 lies in the L2 page");
