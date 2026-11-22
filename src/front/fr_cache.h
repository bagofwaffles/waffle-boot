/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's L1 caches (Blackfin+ Programming Reference, chapter 7), on
 * the master from the stream's entry, its INIT stage done, to its one exit,
 * fr_main()'s return to the jump in fr_entry.S.
 *
 * The CPLB default descriptors take every address no page below matches,
 * with no exception on a miss (EOM), supervisor writes allowed and nothing
 * cached, data in system memory as I/O device space: no address raises a
 * CPLB exception, and one left out is read uncached, never speculatively. The pages
 * name only what is cached: L2 for code and data, and the DDR 64 MiB that
 * hold the main image's DDR part and recovery's staging for data,
 * write-through; any other system address, the panel's SMC bank among
 * them, is I/O device space by default. L1 is never cached. The
 * panel's frame and line buffers, the one buffer a DMA reads, lie in DDR
 * outside the cached page; no DMA writes memory.
 *
 * fr_cache_off() puts every register it changed back as fr_cache_on()
 * found it, the CPLBs and both caches off: the off state the stream's entry
 * leaves, which the main image's start and any later loader expect.
 */
#ifndef FR_CACHE_H
#define FR_CACHE_H

#include "fr.h"

#include "waffle/drivers/bf70x_cplb.h"

/* The default descriptors' bits (PRM Tables 7-14, 7-22). */
#define FR_ICPLB_DFLT_L1EOM     (1u << 8)
#define FR_ICPLB_DFLT_SYSEOM    (1u << 4)
#define FR_DCPLB_DFLT_L1SWRITE  (1u << 11)
#define FR_DCPLB_DFLT_L1EOM     (1u << 8)
#define FR_DCPLB_DFLT_SYSSWRITE (1u << 7)
#define FR_DCPLB_DFLT_SYSEOM    (1u << 4)
#define FR_DCPLB_DFLT_SYSIO     (4u << 0)	/* SYSCPROPS: I/O device space */
/* Instruction fetches: SYSCPROPS 0, not cacheable. Data: system memory as
 * I/O device space, read without speculation. */
#define FR_ICPLB_DFLT (FR_ICPLB_DFLT_L1EOM | FR_ICPLB_DFLT_SYSEOM)
#define FR_DCPLB_DFLT (FR_DCPLB_DFLT_L1SWRITE | FR_DCPLB_DFLT_L1EOM | \
		       FR_DCPLB_DFLT_SYSSWRITE | FR_DCPLB_DFLT_SYSEOM | FR_DCPLB_DFLT_SYSIO)

#define FR_ICPLB_N 1u
#define FR_DCPLB_N 2u
/* The pages, in wb_cplb's layout (addr, data). */
extern const struct wb_cplb fr_icplb[FR_ICPLB_N];
extern const struct wb_cplb fr_dcplb[FR_DCPLB_N];

/* Target only. The CPLBs on with the table and the defaults above, then
 * the instruction cache and the data cache in L1 data A's upper 16 KB, in
 * the PRM's order for each side; nothing between the calls may start a DMA
 * that writes cached memory. */
void fr_cache_on(void);
/* Both sides off in the PRM's order, data first (write-through: nothing to
 * write back), and the default descriptors and the pages' registers as
 * fr_cache_on() found them. Nothing when fr_cache_on() did not run. */
void fr_cache_off(void);

#endif /* FR_CACHE_H */
