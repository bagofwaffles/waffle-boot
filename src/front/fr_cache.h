/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's L1 caches (Blackfin+ Programming Reference, chapter 7), on
 * the master from the stream's entry, its INIT stage done, to its one exit,
 * fr_main()'s return to the jump in fr_entry.S.
 *
 * The sequence is the main image's wb_cplb_setup() and the stub's: all
 * sixteen descriptor pairs of each side written, the table's entries
 * first and invalid ones after them, so that no pair the boot ROM or the
 * first stage left valid (HRM, Preboot Operations, Cache Configuration:
 * ten instruction and twelve data pairs, among them L2 and DDR pages) can
 * match an address a second time or with other properties. The default
 * descriptors are left as found, as both of those leave them: every
 * address the front touches lies in a page of the table, L1 included in
 * the main image's uncached 4 MB page. Data pages are write-through; the
 * panel's frame and line buffers, the one buffer a DMA reads, lie in an
 * uncached DDR page; no DMA writes memory.
 *
 * fr_cache_off() puts every pair back as fr_cache_on() found it, the CPLBs
 * and both caches off: the off state the stream's entry leaves, which the
 * main image's start and any later loader expect.
 */
#ifndef FR_CACHE_H
#define FR_CACHE_H

#include "fr.h"

#include "waffle/drivers/bf70x_cplb.h"

#define FR_CPLB_PAIRS 16u
#define FR_ICPLB_N 4u
#define FR_DCPLB_N 9u
/* The pages, in wb_cplb's layout (addr, data). */
extern const struct wb_cplb fr_icplb[FR_ICPLB_N];
extern const struct wb_cplb fr_dcplb[FR_DCPLB_N];

/* Target only (built for the host's test from its source). The CPLBs on
 * with the table, then the instruction cache and the data cache in L1 data
 * A's upper 16 KB, in the PRM's order for each side; nothing between the
 * calls may start a DMA that writes cached memory. */
void fr_cache_on(void);
/* Both sides off in the PRM's order, data first (write-through: nothing to
 * write back), and every descriptor pair as fr_cache_on() found it.
 * Nothing when fr_cache_on() did not run. */
void fr_cache_off(void);

#endif /* FR_CACHE_H */
