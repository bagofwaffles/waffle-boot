/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The OP-XY's entry after start-up, main(): the panel handshake and the strap
 * first, then one role's boot (S-01.003); the fatal halt; the heap; and
 * the weak defaults of the board's driver hooks.
 */
#include "waffle/board/opxy_boot.h"


#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/bf70x_start.h"
#include "waffle/drivers/mmr.h"

/* S-01.006: the first stage's panel handshake is in MDMA0's DMA9 pair. */
#define HANDSHAKE_CH 9u

void opxy_main(void)
{
	struct opxy_master *m = &opxy_master;
	struct wb_time t = opxy_time;
	/* S-01.006, S-13b.081: before anything can touch DMA9. */
	uint32_t xmod = wb_mmr_read32(DMA_XMOD(HANDSHAKE_CH));
	uint32_t ymod = wb_mmr_read32(DMA_YMOD(HANDSHAKE_CH));
	uint16_t strap = 0;

	/* The test OS reads what the loading bootloader left first, then paints
	 * orange on the master (wb_start_c() and the read-out done) before the
	 * full strap read below releases PA8, the panel's RESET, which on a
	 * board whose PA8 straps low resets the panel (S-13c.004, S-17.003);
	 * the role is the entry's PA2 sample (opxy_testos_entry_paint()). */
	OPXY_TESTOS(opxy_testos_capture(); OPXY_TESTOS_TIMING(opxy_testos_timing_capture();) if (opxy_strap_role((uint16_t)opxy_testos_entry[5]) == OPXY_MASTER) opxy_testos_paint(OPXY_PAINT_ORANGE);)
	m->front = opxy_front_take(&m->ho);
	if (m->front) {
		/* The front read DMA9 and the strap before it touched a pin. */
		xmod = m->ho.dma9_xmod;
		ymod = m->ho.dma9_ymod;
		strap = m->ho.strap;
		opxy_init_sp = m->ho.init_sp;
		opxy_init_dctl = m->ho.init_dctl;
		opxy_init_dflt = m->ho.init_dflt;
		opxy_init_cycles[0] = m->ho.stamps[FR_STAMP_INIT_ENTRY];
		opxy_init_cycles[1] = m->ho.stamps[FR_STAMP_INIT_CLOCKS];
		opxy_init_cycles[2] = m->ho.stamps[FR_STAMP_INIT_DDR];
	} else if (wb_strap_read(&opxy_strap, &t, &strap) != 0) {
		/* S-13b.001: before any MUX or FER write. The read fails only on
		 * a bad pin table (a driver argument error), never on the pins;
		 * it is taken as the slave, which never drives the boot link or
		 * the PSoC. */
		strap = 0;
	}
	if (opxy_strap_role(strap) == OPXY_MASTER) {
		/* The test OS resets and initializes the panel itself
		 * (S-13c.004's pulse path): no handshake. */
		m->dma9_xmod = OPXY_TESTOS_OR(((void)xmod, 0u), xmod);
		m->dma9_ymod = ymod;
		m->strap = strap;
		opxy_master_main(m);
	} else {
		opxy_slave_main(&opxy_slave);
	}
	for (;;)
		wb_core_idle();
}

#if defined(__bfin__)
/* Entered from start.S at IVG15 after wb_start_c() (S-01.001 step 4). On
 * the host every test has its own main. */
int main(void)
{
	opxy_main();
	return 0;
}
#endif

/* S-16.104: the record in the L2 reserve, then nothing but the halt. */
void opxy_fatal(enum s04_fatal_cause cause)
{
	uint8_t rec[S04_FATAL_SIZE];
	unsigned i;

	(void)wb_core_cli();
	/* The test OS: magenta on the panel before the record (a fatal frame,
	 * where one can follow, comes from the caller's path). */
	OPXY_TESTOS(opxy_testos_paint(OPXY_PAINT_MAGENTA);)
	s04_fatal_record(cause, (uint32_t)(uintptr_t)__builtin_return_address(0),
			 rec);
	for (i = 0; i < S04_FATAL_SIZE; i++)
		wb_mmr_write8(S04_FATAL_ADDR + i, rec[i]);
	for (;;)
		wb_core_idle();
}

/* --- Hooks (see waffle/board/opxy_boot.h) --- */

/* Defaults for a build without the display driver (S-13c). */
__attribute__((weak)) bool opxy_display_boot_logo(void)
{
	return false;
}

__attribute__((weak)) void opxy_display_dma_take(void)
{
}

__attribute__((weak)) bool opxy_display_early_logo(void)
{
	return false;
}

__attribute__((weak)) void opxy_display_handshake(uint32_t xmod, uint32_t ymod)
{
	(void)xmod;
	(void)ymod;
}

__attribute__((weak)) void opxy_display_adopt(uint32_t reset_ms)
{
	(void)reset_ms;
}

#if defined(__bfin__)
#include <stddef.h>

/* picolibc's sys/unistd.h declares it only under feature macros. */
void *sbrk(ptrdiff_t incr);

/*
 * picolibc's malloc (yaffs2's yaffsfs_malloc) grows through sbrk over the
 * heap opxy.h declares (S-01.015: never into staging or a held buffer).
 */
void *sbrk(ptrdiff_t incr)
{
	static uint32_t top = OPXY_HEAP_START;
	const uint32_t old = top;

	if (incr < 0 || (uint32_t)incr > OPXY_HEAP_END - top)
		return (void *)-1;
	top += (uint32_t)incr;
	return (void *)(uintptr_t)old;
}
#endif
