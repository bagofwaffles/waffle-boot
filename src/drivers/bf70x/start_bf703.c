/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The target half of start-up: the linker script's symbols and the weak
 * board hooks (see waffle/drivers/bf70x/start.h). Host builds have none of
 * these symbols, so the file is empty there.
 */
#include "waffle/drivers/bf70x/start.h"

#if defined(__bfin__)

#include <stddef.h>

#include "waffle/drivers/bf70x/core.h"

/* From src/drivers/bf70x/bf703.ld. */
extern char wb_bss_start[], wb_bss_end[];
extern char wb_l1_bss_start[], wb_l1_bss_end[];

__attribute__((weak)) const struct wb_cplb_config *wb_board_cplb(void)
{
	return &wb_cplb_bf703_default;
}

__attribute__((weak)) void wb_core_fault(uint32_t seqstat, uint32_t ret)
{
	(void)seqstat;
	(void)ret;
	for (;;)
		wb_core_idle();
}

void wb_start_c(void)
{
	/*
	 * The loader stream puts initialized data at its run address, so
	 * only the zero-initialized sections need work.
	 */
	const struct wb_crt_section sections[] = {
		{ wb_bss_start, NULL,
		  (uint32_t)((uintptr_t)wb_bss_end - (uintptr_t)wb_bss_start) },
		{ wb_l1_bss_start, NULL,
		  (uint32_t)((uintptr_t)wb_l1_bss_end -
			     (uintptr_t)wb_l1_bss_start) },
	};
	struct wb_start_config cfg = {
		.l1 = wb_l1_bf703,
		.n_l1 = 4,
		.sections = sections,
		.n_sections = 2,
		.cplb = NULL,
	};

	cfg.cplb = wb_board_cplb();
	if (wb_start_init(&cfg) != WB_CPLB_OK) {
		/* A bad board table: run uncached rather than not at all. */
		wb_cplb_disable();
	}
}

#else

typedef int wb_start_bf703_host_empty;

#endif
