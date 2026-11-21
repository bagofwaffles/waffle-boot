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
#include "waffle/drivers/bf70x_rcu.h"
#include "waffle/drivers/bf70x_spiflash.h"
#include "waffle/drivers/bf70x_start.h"
#include "waffle/drivers/mmr.h"

/* S-01.006: the first stage's panel handshake is in MDMA0's DMA9 pair. */
#define HANDSHAKE_CH 9u

/*
 * S-18.003 without a front: SPI2's registers alone, no port register
 * written, so the first stage is read on the pins as the boot ROM left
 * them (S-18.001).
 */
struct first_stage_bus {
	struct opxy_master *m;
	const struct wb_time *t;
	bool up;
};

/* A wb_flash_read_fn: the bus set up on the first read. */
static int first_stage_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	struct first_stage_bus *b = ctx;
	struct opxy_master *m = b->m;

	if (!b->up) {
		(void)wb_spi_init(&m->hw.flash_spi, OPXY_SPI_FLASH, &opxy_spi_flash, b->t,
				  OPXY_SPI_WORD_US);
		m->hw.flash.spi = &m->hw.flash_spi;
		m->hw.flash.size = OPXY_FLASH_SIZE;
		m->hw.flash.busy_timeout_us = OPXY_FLASH_BUSY_US;
		m->hw.flash.busy = true;
		b->up = true;
	}
	return wb_spiflash_read(&m->hw.flash, off, buf, len);
}

/* S-13b.001's sample (PA0, PA2, PA8). The read fails only on a bad pin
 * table (a driver argument error), never on the pins: 0 then. */
static uint16_t board_strap(void *ctx)
{
	uint16_t strap = 0;

	(void)ctx;
	if (wb_strap_read(&opxy_strap, &opxy_time, &strap) != 0)
		strap = 0;
	return strap;
}

/* S-18.002's fallback sample, when opxy_device_pick() asks for it: PA2's
 * INEN alone, at S-13b.001's settling time; PA0 and PA8 not released. */
static OPXY_TESTOS_UNUSED uint16_t board_pa2(void *ctx)
{
	(void)ctx;
	wb_mmr_write32(PORT_INEN_SET(WB_PORT_A), OPXY_STRAP_ROLE);
	wb_mmr_sync();
	wb_delay_us(&opxy_time, 20u);	/* opxy_strap.settle_us */
	return (uint16_t)(wb_mmr_read32(PORT_DATA(WB_PORT_A)) & OPXY_STRAP_ROLE);
}

void opxy_main(void)
{
	struct opxy_master *m = &opxy_master;
	struct wb_time t = opxy_time;
	/* S-01.006, S-13b.081: before anything can touch DMA9. */
	uint32_t xmod = wb_mmr_read32(DMA_XMOD(HANDSHAKE_CH));
	uint32_t ymod = wb_mmr_read32(DMA_YMOD(HANDSHAKE_CH));
	const unsigned bmode = (wb_mmr_read32(RCU0_STAT) & RCU_STAT_BMODE_MASK) >>
			       RCU_STAT_BMODE_SHIFT;
	struct first_stage_bus bus = { m, &t, false };
	struct opxy_device_pick pick;
	uint16_t strap = 0;

	/* The test OS reads what the loading bootloader left first, then paints
	 * orange on the master (wb_start_c() and the read-out done) before the
	 * full strap read below releases PA8, the panel's RESET, which on a
	 * board whose PA8 straps low resets the panel (S-13c.004, S-17.003);
	 * the role and device are the entry's (opxy_testos_entry_paint()). */
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
	}
	/* S-18.005: the record's device; without one, S-18.002 to S-18.004
	 * here, before any pin, UART, SPI0 or MSI0 access (S-18.001) */
	OPXY_TESTOS_OR(((void)bmode, (void)bus, (void)first_stage_read, opxy_testos_device(&pick)), opxy_device_pick(m->front ? &m->ho : NULL, bmode, board_pa2, NULL, first_stage_read, &bus, &pick));	/* the test OS: the entry's decision, the first stage read once */
	/* S-13b.001 where the device has straps, the second CPU included;
	 * after the first stage's read (S-18.001) */
	if (!m->front && (!pick.master || wb_has(pick.dev, WB_F_STRAPS)))
		strap = board_strap(NULL);
	if (pick.master) {
		/* The test OS resets and initializes the panel itself
		 * (S-13c.004's pulse path): no handshake. */
		m->dma9_xmod = OPXY_TESTOS_OR(((void)xmod, 0u), xmod);
		m->dma9_ymod = ymod;
		m->strap = strap;
		m->dev = pick.dev;
		m->dev_how = (uint8_t)pick.how;
		m->fs_n = pick.n;
		m->fs_c = pick.c;
		/* S-18.002: a main CPU stays the master; PA2 = 0 is logged */
		m->role_disagrees = wb_has(pick.dev, WB_F_STRAPS) &&
				    opxy_strap_role(strap) != OPXY_MASTER;
		m->role_fallback = pick.fallback;
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
