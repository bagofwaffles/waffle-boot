/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front: fr_main(), which start/entry.S calls after the stream has
 * loaded. It reads what the first stage left and decides the device before
 * any pin is changed but the flash bus's (S-18.001, fr_decide()), then sets
 * the device's pins. It runs the main CPU's start-up, or hands the OP-XY's
 * second CPU straight on. Target only.
 */
#include "front/front.h"
#include "front/start/device.h"
#include "front/handoff.h"
#include "front/start/cache.h"
#include "front/hw/hw.h"
#include "front/slots/image.h"
#include "front/start/start.h"
#include "front/recovery/usb.h"

#include "waffle/drivers/bf70x/core.h"
#include "waffle/drivers/bf70x/dma.h"
#include "waffle/drivers/bf70x/port.h"
#include "waffle/drivers/bf70x/rcu.h"
#include "waffle/drivers/mmr.h"

/* S-13b.001: PA0, PA2 and PA8. PA2, the role strap, is recorded; the role
 * is the boot mode's (S-18.002), or the fallback sample's where the boot
 * mode is not 1 (fr_decide()). */
#define STRAP_PINS  (1u << 0 | 1u << 2 | 1u << 8)
#define STRAP_SETTLE_US 20u
#define STRAP_INEN_US   2u
/* S-01.006: the first stage's panel handshake is in DMA9. */
#define HANDSHAKE_CH 9u

_Static_assert(FR_BOOT_MODE_MAIN == RCU_BMODE_SPI_MASTER, "S-18.002: boot mode 1");

static struct fr_start start;
static struct fr_role role;

/* fr_serve_hook: the PSoC link's frames taken in, once fr_start_run() has
 * started the link; before that the UART's bytes into memory. */
static void serve_link(void)
{
	if (start.panel_up)
		fr_panel_watch(&start.panel);
	if (start.psoc.io != NULL)
		fr_psoc_take(&start.psoc);
	else
		fr_hw_uart_take();
}

extern char fr_bss_start[], fr_bss_end[];
extern uint32_t fr_init_sp, fr_init_dctl, fr_init_dflt;
extern uint64_t fr_init_cycles[3];

static void delay_us(uint32_t us)
{
	const uint64_t end = wb_core_cycles() + (uint64_t)us * FR_CCLK_MHZ;

	while (wb_core_cycles() < end)
		;
}

/* S-13b.001, when fr_decide() asks for it. */
static uint16_t strap_read(void *ctx)
{
	uint16_t v;

	(void)ctx;
	wb_mmr_write32(PORT_DIR_CLR(WB_PORT_A), STRAP_PINS);
	wb_mmr_sync();
	delay_us(STRAP_SETTLE_US);
	wb_mmr_write32(PORT_INEN_SET(WB_PORT_A), STRAP_PINS);
	wb_mmr_sync();
	delay_us(STRAP_INEN_US);
	v = (uint16_t)(wb_mmr_read32(PORT_DATA(WB_PORT_A)) & STRAP_PINS);
	wb_mmr_write32(PORT_INEN_CLR(WB_PORT_A), STRAP_PINS);
	return v;
}

/* S-18.002's fallback sample: PA2's INEN set and no other port bit, the
 * settling time, PA2 read. PA0 and PA8 are not released. */
static uint16_t pa2_read(void *ctx)
{
	(void)ctx;
	wb_mmr_write32(PORT_INEN_SET(WB_PORT_A), FR_STRAP_PA2);
	wb_mmr_sync();
	delay_us(STRAP_SETTLE_US);
	return (uint16_t)(wb_mmr_read32(PORT_DATA(WB_PORT_A)) & FR_STRAP_PA2);
}

static unsigned boot_mode(void *ctx)
{
	(void)ctx;
	return (wb_mmr_read32(RCU0_STAT) & RCU_STAT_BMODE_MASK) >> RCU_STAT_BMODE_SHIFT;
}

/* S-18.001, S-18.003: PORTB alone where the bus is not SPI2's, then
 * SPI2's registers */
static void flash_bus(void *ctx)
{
	(void)ctx;
	(void)fr_hw_flash_bus();
	fr_hw_flash_start();
}

/* The loaded image, where it may be read as data (fr_mem_readable()). The
 * image check and the running slot's comparison read only through this. */
static const uint8_t *mem_map(void *ctx, uint32_t addr, uint32_t len)
{
	(void)ctx;
	return fr_mem_readable(addr, len) ? (const uint8_t *)(uintptr_t)addr : NULL;
}

uint32_t fr_main(void);
uint32_t fr_main(void)
{
	struct fr_handoff *ho = (struct fr_handoff *)(uintptr_t)FR_HANDOFF_ADDR;
	static const struct fr_mem mem = { mem_map, NULL };
	struct fr_desc d;
	static const struct fr_decide_hw dhw = {
		boot_mode, pa2_read, strap_read, flash_bus, &fr_hw_flash, NULL,
	};
	uint32_t xmod, ymod;

	/* S-01.006, S-13b.081: before anything can touch DMA9. */
	xmod = wb_mmr_read32(DMA_XMOD(HANDSHAKE_CH));
	ymod = wb_mmr_read32(DMA_YMOD(HANDSHAKE_CH));
	fr_fill(fr_bss_start, 0, (size_t)((uintptr_t)fr_bss_end - (uintptr_t)fr_bss_start));
	fr_fill(ho, 0, sizeof *ho);
	ho->stamps[FR_STAMP_INIT_ENTRY] = fr_init_cycles[0];
	ho->stamps[FR_STAMP_INIT_CLOCKS] = fr_init_cycles[1];
	ho->stamps[FR_STAMP_INIT_DDR] = fr_init_cycles[2];
	ho->stamps[FR_STAMP_ENTRY] = wb_core_cycles();
	ho->init_sp = fr_init_sp;
	ho->init_dctl = fr_init_dctl;
	ho->init_dflt = fr_init_dflt;
	ho->dma9_xmod = xmod;
	ho->dma9_ymod = ymod;
	/* S-18.001 to S-18.004 */
	fr_decide(&dhw, &role);
	ho->strap = role.strap;		/* 0 where no sample was taken */
	if (role.main) {
		static const struct fr_start_hw hw = {
			&fr_hw_clock, &fr_hw_psoc, NULL, &fr_hw_flash, &mem,
			fr_usb_start, fr_usb_poll, fr_usb_configured, fr_usb_ma,
			&fr_usb_serial, (uint8_t *)(uintptr_t)FR_STAGING_START,
			(uint8_t *)(uintptr_t)FR_FRAME_START, &role.ident,
		};
		struct fr_start_hw h = hw;
		enum fr_next next;

		/* the caches on, the INIT stage's clocks and DDR up; off again at
		 * the one exit below */
		fr_cache_on();
		ho->role_master = 1u;
		if (role.by_strap)
			ho->flags |= FR_HO_ROLE_BY_STRAP;
		fr_hw_pins(role.ident.dev);
		h.panel = fr_hw_lcd_start(role.ident.dev);
		fr_hw_uart_start();
		fr_usb_device(role.ident.dev);
		fr_serve_hook = serve_link;	/* the long loops read the link */
		next = fr_start_run(&start, &h, ho);
		if (next == FR_NEXT_RECOVERY)
			next = fr_recovery_run(&start);
		if (next != FR_NEXT_MAIN)
			for (;;)
				fr_halt(&start, 1000000u);	/* the PSoC ends it */
		d = start.desc;
		/* S-01.023's reports */
		ho->te_misses = start.panel.te_misses;
		fr_hw_uart_report(&ho->link_gap_max_us, &ho->link_overruns);
	} else {
		ho->role_master = 0u;
		ho->device = FR_HO_DEV_OPXY;
		ho->device_how = FR_HO_HOW_SECOND;
		if (!fr_image_check(&mem, &d)) {
			/* The slave: its stream came from the master, which
			 * checked it. It has no PSoC link to serve. */
			for (;;)
				;
		}
	}
	ho->stamps[FR_STAMP_HANDOFF] = wb_core_cycles();
	fr_handoff_seal(ho);
	/* the state the stream's entry left: the CPLBs and caches off, so that
	 * no line of the image just read as data survives the jump */
	fr_cache_off();
	return d.main_entry;
}
