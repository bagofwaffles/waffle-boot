/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_START_H
#define WAFFLE_DRIVERS_BF70X_START_H

/*
 * Start-up between the front's jump and main().
 *
 * src/drivers/bf70x/start.S is the entry point the front jumps to. It
 * moves the stack to the main stack in DDR, points the fault vectors at
 * wb_core_fault(), calls
 * wb_start_c() and then main() at the lowest interrupt level (IVG15), with
 * interrupts enabled at the core and nothing enabled at the SEC.
 *
 * wb_start_c() (src/drivers/bf70x/start_bf703.c) fills a wb_start_config
 * from the linker script (src/drivers/bf70x/bf703.ld) and the board's
 * CPLB table, and runs wb_start_init(). The steps are separate functions so
 * that the host tests can check each one.
 */

#include <stdint.h>

#include "waffle/drivers/bf70x/cplb.h"

/* Copy len bytes from src to dst, or zero them when src is NULL. */
struct wb_crt_section {
	void *dst;
	const void *src;
	uint32_t len;
};

/* A word-aligned range of L1 memory, [start, end). */
struct wb_l1_range {
	uint32_t start;
	uint32_t end;
};

struct wb_start_config {
	const struct wb_l1_range *l1;	/* the parity pass */
	unsigned n_l1;
	const struct wb_crt_section *sections;
	unsigned n_sections;
	const struct wb_cplb_config *cplb; /* NULL: CPLBs and caches off */
};

/* All of the ADSP-BF703's L1: data A and B, code, scratch (S-01.012). */
extern const struct wb_l1_range wb_l1_bf703[4];

void wb_crt_sections(const struct wb_crt_section *t, unsigned n);

/*
 * Write every word of the ranges back with its own value, which sets its
 * parity bits (S-13a.040): with read parity checking off on both sides
 * (L1IM_ICTL.RDCHK and L1DM_DCTL.RDCHK cleared first) and L1DM_DCTL.ENX
 * set, each word is read from its own address and stored back there; no
 * other address is read. Contents, the stack included, are kept. Read
 * checking stays off; turning it on is the caller's, after the pass.
 * Interrupts must be off.
 */
void wb_l1_parity_init(const struct wb_l1_range *r, unsigned n);

/* Read parity checking on, L1IM_ICTL.RDCHK and L1DM_DCTL.RDCHK, by
 * read-modify-write (S-13a.040). */
void wb_l1_parity_on(void);

/*
 * Turn CPLBs and caches off; on the first start since power-on
 * (RCU0_MSG.C0L1INIT clear) run the parity pass and set C0L1INIT; turn
 * read parity checking on, on a warm start too; set up the sections; then
 * install the CPLB table if there is one. Returns
 * wb_cplb_setup()'s result, or WB_CPLB_OK.
 */
int wb_start_init(const struct wb_start_config *cfg);

/* Called by start.S before main(). */
void wb_start_c(void);

/*
 * The CPLB table wb_start_c() installs. The default (a weak definition)
 * returns &wb_cplb_bf703_default; a board defines its own to change it.
 * It runs before .bss is cleared.
 */
const struct wb_cplb_config *wb_board_cplb(void);

/*
 * Where exceptions, NMI and unexpected interrupts end up. seqstat is
 * SEQSTAT, ret the return address of the event (RETX). The default (a
 * weak definition) idles forever; a board can record and reset instead.
 */
void wb_core_fault(uint32_t seqstat, uint32_t ret);

int main(void);

#endif /* WAFFLE_DRIVERS_BF70X_START_H */
