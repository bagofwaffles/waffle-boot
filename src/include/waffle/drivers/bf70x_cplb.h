/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_CPLB_H
#define WAFFLE_DRIVERS_BF70X_CPLB_H

/*
 * L1 cache and memory protection (CPLB) set-up, from a table
 * (Blackfin+ Programming Reference, chapter 7, Memory Protection and
 * Properties; L1IM and L1DM register descriptions).
 *
 * The table gives up to 16 instruction and 16 data page descriptors. Every
 * entry is installed locked; there is no CPLB miss handler, so every
 * address the program touches outside the MMR space must be covered.
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/drivers/bf70x_core.h"

/* L1IM_ICTL (PRM Table 7-16). */
#define L1IM_ICTL_ENCPLB    (1u << 1)
#define L1IM_ICTL_CFG       (1u << 2)	/* upper 16 KB of L1 code is cache */
#define L1IM_ICTL_CBYPASS   (1u << 8)
#define L1IM_ICTL_RDCHK     (1u << 9)
#define L1IM_ICTL_CPRIORST  (1u << 13)

/* L1DM_DCTL (PRM Table 7-24). */
#define L1DM_DCTL_ENCPLB    (1u << 1)
#define L1DM_DCTL_CFG_SHIFT 2u
#define L1DM_DCTL_CFG_MASK  (3u << 2)
#define L1DM_DCTL_CFG_A     (1u << 2)	/* upper 16 KB of bank A is cache */
#define L1DM_DCTL_CFG_AB    (3u << 2)	/* and of bank B */
#define L1DM_DCTL_DCBS      (1u << 4)
#define L1DM_DCTL_CBYPASS   (1u << 8)
#define L1DM_DCTL_RDCHK     (1u << 9)
#define L1DM_DCTL_ENX       (1u << 16)	/* extended data access */

/* L1IM_ICPLB_DATA[n] and L1DM_DCPLB_DATA[n] (PRM Tables 7-13, 7-21). */
#define CPLB_VALID          (1u << 0)
#define CPLB_LOCK           (1u << 1)
#define CPLB_UREAD          (1u << 2)
#define DCPLB_UWRITE        (1u << 3)
#define DCPLB_SWRITE        (1u << 4)
#define DCPLB_DIRTY         (1u << 7)
#define ICPLB_CPRIO         (1u << 8)
#define CPLB_CPROPS_SHIFT   12u
#define ICPLB_CPROPS_MASK   (3u << 12)
#define DCPLB_CPROPS_MASK   (7u << 12)
#define ICPLB_NOCACHE       (0u << 12)
#define ICPLB_CACHE         (1u << 12)
#define DCPLB_NOCACHE       (0u << 12)
#define DCPLB_WB            (1u << 12)	/* write-back cacheable in L1 */
#define DCPLB_IO            (4u << 12)	/* I/O device space */
#define DCPLB_WT            (5u << 12)	/* write-through cacheable in L1 */
#define CPLB_PSIZE_SHIFT    16u
#define CPLB_PSIZE_MASK     (0xFu << 16)

/* Page sizes: PSIZE n is 1 KB x 4^n (PRM). */
enum wb_cplb_size {
	WB_CPLB_1K = 0,
	WB_CPLB_4K,
	WB_CPLB_16K,
	WB_CPLB_64K,
	WB_CPLB_256K,
	WB_CPLB_1M,
	WB_CPLB_4M,
	WB_CPLB_16M,
	WB_CPLB_64M,
	WB_CPLB_256M,
	WB_CPLB_1G,
};

#define WB_CPLB_PSIZE(sz) ((uint32_t)(sz) << CPLB_PSIZE_SHIFT)

/* One page descriptor: the page's first address and its data word. */
struct wb_cplb {
	uint32_t addr;
	uint32_t data;
};

struct wb_cplb_config {
	const struct wb_cplb *icplb;
	unsigned n_icplb;
	const struct wb_cplb *dcplb;
	unsigned n_dcplb;
	bool icache;		/* L1IM_ICTL.CFG */
	uint32_t dcache;	/* L1DM_DCTL.CFG field: 0, L1DM_DCTL_CFG_A or _AB */
};

#define WB_CPLB_OK          0
#define WB_CPLB_ERR_COUNT  (-1)	/* more than 16 entries on one side */
#define WB_CPLB_ERR_ENTRY  (-2)	/* not valid, bad size or misaligned */
#define WB_CPLB_ERR_CFG    (-3)	/* a dcache value other than the three */

/* Bytes in the page an entry describes; 0 for a reserved size. */
uint32_t wb_cplb_page_bytes(uint32_t data);

/*
 * Check one side's table without touching hardware. In DDR, called from
 * wb_cplb_setup() in L1, hence WB_FAR_CALL here and on the two below.
 */
WB_FAR_CALL int wb_cplb_check(const struct wb_cplb *t, unsigned n,
			      bool data);

/*
 * Turn the CPLBs and caches off, then install the table and turn on what
 * the configuration asks for, in the PRM's order for each side: CPLBs off,
 * SSYNC, cache configuration (which initializes the tags), descriptors,
 * CSYNC, CPLBs on, SSYNC. Unused entries are written invalid. Extended data
 * access (L1DM_DCTL.ENX) ends off; read parity checking (RDCHK) is kept as
 * it is (S-13a.040). Nothing is written if the table fails wb_cplb_check().
 *
 * Call with interrupts off. On the target the register sequence runs from
 * L1 code (.l1.text), which is never cacheable, as the PRM requires.
 */
WB_FAR_CALL int wb_cplb_setup(const struct wb_cplb_config *cfg);

/*
 * Turn both sides' CPLBs and caches off, and ENX; RDCHK is kept as it is
 * (S-13a.040). With a write-back data cache the caller writes back what it
 * needs first (wb_dcache_flush_range).
 */
WB_FAR_CALL void wb_cplb_disable(void);

/* Data cache maintenance by address range, one line at a time. */
void wb_dcache_flush_range(uint32_t addr, uint32_t len);
void wb_dcache_invalidate_range(uint32_t addr, uint32_t len);

/*
 * A generic ADSP-BF703 map: L1 (non-cached), boot ROM, L2 and DDR cached
 * (data write-through, so turning the cache off needs no write-back), the
 * SMC bank as I/O device space. Boards with other needs pass their own.
 */
extern const struct wb_cplb_config wb_cplb_bf703_default;

#endif /* WAFFLE_DRIVERS_BF70X_CPLB_H */
