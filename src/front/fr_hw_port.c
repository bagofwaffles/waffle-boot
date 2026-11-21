/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The main CPU's pins (S-13b.003, S-13b.004, S-13b.006), the values and
 * the order of the board's pin and GPIO set-up for the device decided
 * (fr_device.c), the flash bus before the decision (S-18.001), and the
 * time base from the core cycle counter (S-13a.001:
 * 400 MHz). Target only.
 */
#include "fr_handoff.h"
#include "fr_hw.h"

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/mmr.h"


/* Pins whose 2-bit MUX field differs between two MUX values. */
static uint16_t mux_changed(uint32_t a, uint32_t b)
{
	const uint32_t d = a ^ b;
	uint16_t pins = 0;

	for (unsigned i = 0; i < 16u; i++)
		if ((d >> (2u * i)) & 3u)
			pins = (uint16_t)(pins | (1u << i));
	return pins;
}

/* S-13b.004: port p from mux and fer, as read, to want_mux and want_fer,
 * with no FER bit set while the port's MUX differs from want_mux. */
static void port_func(uint32_t p, uint32_t mux, uint32_t fer, uint32_t want_mux, uint32_t want_fer)
{
	const uint32_t off = fer & (~want_fer | mux_changed(mux, want_mux));

	if (off != 0u)
		wb_mmr_write32(PORT_FER_CLR(p), off);
	if (mux != want_mux)
		wb_mmr_write32(PORT_MUX(p), want_mux);
	if ((fer & ~off) != want_fer)
		wb_mmr_write32(PORT_FER_SET(p), want_fer);
}

bool fr_hw_flash_bus(void)
{
	const uint32_t fields = FR_FLASH_BUS_MUX;
	const uint32_t mux = wb_mmr_read32(PORT_MUX(WB_PORT_B));
	const uint32_t fer = wb_mmr_read32(PORT_FER(WB_PORT_B));

	/* S-18.001: the bus as found, when its pins are SPI2's already (a
	 * first stage leaves them so); else PORTB alone, which S-13b.004
	 * exempts from making PB10 an input first */
	if ((mux & fields) == (FR_PORTB_MUX & fields) &&
	    (fer & FR_FLASH_BUS_PINS) == FR_FLASH_BUS_PINS)
		return false;
	port_func(WB_PORT_B, mux, fer, FR_PORTB_MUX, FR_PORTB_FER);
	wb_mmr_sync();
	return true;
}

void fr_hw_pins(const struct fr_dev *dev)
{
	unsigned i;

	/* S-13b.004: none on the OP-1F */
	for (i = 0; i < dev->inputs_first_n; i++) {
		wb_mmr_write32(PORT_DIR_CLR(WB_PIN_PORT(dev->inputs_first[i])),
			       WB_PIN_BIT(dev->inputs_first[i]));
		wb_mmr_write32(PORT_INEN_SET(WB_PIN_PORT(dev->inputs_first[i])),
			       WB_PIN_BIT(dev->inputs_first[i]));
	}
	for (uint32_t p = 0; p < WB_PORTS; p++) {
		const uint32_t mux = wb_mmr_read32(PORT_MUX(p));
		const uint32_t fer = wb_mmr_read32(PORT_FER(p));

		port_func(p, mux, fer, dev->mux[p], dev->fer[p]);
	}
	wb_mmr_sync();
	for (i = 0; i < dev->gpio_n; i++) {
		const uint32_t p = WB_PIN_PORT(dev->gpio[i].pin), b = WB_PIN_BIT(dev->gpio[i].pin);

		switch (dev->gpio[i].kind) {
		case FR_GPIO_IN:
			wb_mmr_write32(PORT_DIR_CLR(p), b);
			wb_mmr_write32(PORT_INEN_SET(p), b);
			break;
		case FR_GPIO_KEEP:
			/* S-13b.006 on the OP-1F: the level it holds now */
			wb_mmr_write32(PORT_DIR_SET(p), b);
			break;
		default:
			/* the level before the driver */
			wb_mmr_write32(dev->gpio[i].kind == FR_GPIO_HIGH ? PORT_DATA_SET(p) :
								   PORT_DATA_CLR(p), b);
			wb_mmr_write32(PORT_DIR_SET(p), b);
			break;
		}
	}
}

static uint64_t clock_now(void *ctx)
{
	(void)ctx;
	return wb_core_cycles() / FR_CCLK_MHZ;
}

static void clock_delay(void *ctx, uint32_t us)
{
	const uint64_t end = wb_core_cycles() + (uint64_t)us * FR_CCLK_MHZ;

	(void)ctx;
	while (wb_core_cycles() < end)
		;
}

const struct fr_clock fr_hw_clock = { clock_now, clock_delay, NULL };
