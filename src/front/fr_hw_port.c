/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The master's pins (S-13b.003, S-13b.004, S-13b.006), the values and the
 * order of the board's opxy_pinmux_master and opxy_gpio_master, and the
 * time base from the core cycle counter (S-13a.001: 400 MHz). Target only.
 */
#include "fr_handoff.h"
#include "fr_hw.h"

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/mmr.h"


/* S-13b.003: MUX and FER per port. */
static const struct { uint32_t mux; uint16_t fer; } func[WB_PORTS] = {
	{ 0x30002000u, 0x4040u },	/* A */
	{ 0x1400FFFFu, 0xFFFFu },	/* B */
	{ 0x00855100u, 0x0BD3u },	/* C */
};

/* S-13b.004: PB10 and PC4 inputs before the first MUX write: DIR cleared,
 * then INEN set, which stays set. */
static const uint8_t inputs_first[] = {
	WB_PIN(WB_PORT_B, 10), WB_PIN(WB_PORT_C, 4),
};

/* S-13b.006: PA3 low; PA7 (the display's TE) before PA6 (the boot link's
 * ready); PC2. The bridge sets its own pins later. */
#define G_IN  0u
#define G_LOW 1u
static const struct { uint8_t pin, dir; } gpio[] = {
	{ WB_PIN(WB_PORT_A, 3), G_LOW },
	{ WB_PIN(WB_PORT_A, 7), G_IN },
	{ WB_PIN(WB_PORT_A, 6), G_IN },
	{ WB_PIN(WB_PORT_C, 2), G_IN },
};

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

void fr_hw_pins(void)
{
	unsigned i;

	for (i = 0; i < sizeof inputs_first; i++) {
		wb_mmr_write32(PORT_DIR_CLR(WB_PIN_PORT(inputs_first[i])),
			       WB_PIN_BIT(inputs_first[i]));
		wb_mmr_write32(PORT_INEN_SET(WB_PIN_PORT(inputs_first[i])),
			       WB_PIN_BIT(inputs_first[i]));
	}
	/* no FER bit set while the port's MUX differs from the table */
	for (uint32_t p = 0; p < WB_PORTS; p++) {
		const uint32_t mux = wb_mmr_read32(PORT_MUX(p));
		const uint32_t fer = wb_mmr_read32(PORT_FER(p));
		const uint32_t off = fer & (~(uint32_t)func[p].fer |
					    mux_changed(mux, func[p].mux));

		if (off != 0u)
			wb_mmr_write32(PORT_FER_CLR(p), off);
		if (mux != func[p].mux)
			wb_mmr_write32(PORT_MUX(p), func[p].mux);
		if ((fer & ~off) != func[p].fer)
			wb_mmr_write32(PORT_FER_SET(p), func[p].fer);
	}
	wb_mmr_sync();
	for (i = 0; i < sizeof gpio / sizeof gpio[0]; i++) {
		const uint32_t p = WB_PIN_PORT(gpio[i].pin), b = WB_PIN_BIT(gpio[i].pin);

		if (gpio[i].dir == G_IN) {
			wb_mmr_write32(PORT_DIR_CLR(p), b);
			wb_mmr_write32(PORT_INEN_SET(p), b);
		} else {
			/* the level before the driver */
			wb_mmr_write32(PORT_DATA_CLR(p), b);
			wb_mmr_write32(PORT_DIR_SET(p), b);
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
