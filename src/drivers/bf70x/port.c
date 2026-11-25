/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Ports, pin multiplexing and pads (see waffle/drivers/bf70x/port.h). */
#include "waffle/drivers/bf70x/port.h"

#include "waffle/drivers/mmr.h"

static bool pin_ok(uint8_t pin)
{
	return pin < WB_PIN_COUNT;
}

int wb_gpio_output(uint8_t pin, bool high)
{
	uint32_t p = WB_PIN_PORT(pin), b = WB_PIN_BIT(pin);

	if (!pin_ok(pin))
		return WB_PORT_ERR_ARG;
	wb_mmr_write32(high ? PORT_DATA_SET(p) : PORT_DATA_CLR(p), b);
	wb_mmr_write32(PORT_DIR_SET(p), b);
	return WB_PORT_OK;
}

int wb_gpio_output_keep(uint8_t pin)
{
	uint32_t p = WB_PIN_PORT(pin), b = WB_PIN_BIT(pin);

	if (!pin_ok(pin))
		return WB_PORT_ERR_ARG;
	wb_mmr_write32(PORT_DIR_SET(p), b);
	return WB_PORT_OK;
}

int wb_gpio_input(uint8_t pin)
{
	uint32_t p = WB_PIN_PORT(pin), b = WB_PIN_BIT(pin);

	if (!pin_ok(pin))
		return WB_PORT_ERR_ARG;
	wb_mmr_write32(PORT_DIR_CLR(p), b);
	wb_mmr_write32(PORT_INEN_SET(p), b);
	return WB_PORT_OK;
}

int wb_gpio_set(uint8_t pin, bool high)
{
	uint32_t p = WB_PIN_PORT(pin), b = WB_PIN_BIT(pin);

	if (!pin_ok(pin))
		return WB_PORT_ERR_ARG;
	wb_mmr_write32(high ? PORT_DATA_SET(p) : PORT_DATA_CLR(p), b);
	return WB_PORT_OK;
}

bool wb_gpio_get(uint8_t pin)
{
	return pin_ok(pin) &&
	       (wb_mmr_read32(PORT_DATA(WB_PIN_PORT(pin))) & WB_PIN_BIT(pin));
}

int wb_gpio_gpio(uint8_t pin)
{
	if (!pin_ok(pin))
		return WB_PORT_ERR_ARG;
	wb_mmr_write32(PORT_FER_CLR(WB_PIN_PORT(pin)), WB_PIN_BIT(pin));
	return WB_PORT_OK;
}

int wb_gpio_peripheral(uint8_t pin)
{
	if (!pin_ok(pin))
		return WB_PORT_ERR_ARG;
	wb_mmr_write32(PORT_FER_SET(WB_PIN_PORT(pin)), WB_PIN_BIT(pin));
	return WB_PORT_OK;
}

int wb_strap_read(const struct wb_strap *s, const struct wb_time *t,
		  uint16_t *value)
{
	if (s->port >= WB_PORTS || s->pins == 0)
		return WB_PORT_ERR_ARG;
	wb_mmr_write32(PORT_DIR_CLR(s->port), s->pins);
	wb_mmr_sync();
	wb_delay_us(t, s->settle_us);
	wb_mmr_write32(PORT_INEN_SET(s->port), s->pins);
	wb_mmr_sync();
	wb_delay_us(t, s->inen_us);
	*value = (uint16_t)(wb_mmr_read32(PORT_DATA(s->port)) & s->pins);
	wb_mmr_write32(PORT_INEN_CLR(s->port), s->pins);
	return WB_PORT_OK;
}

int wb_strap_inen_read(const struct wb_strap *s, const struct wb_time *t,
		       uint16_t *value)
{
	if (s->port >= WB_PORTS || s->pins == 0)
		return WB_PORT_ERR_ARG;
	wb_mmr_write32(PORT_INEN_SET(s->port), s->pins);
	wb_mmr_sync();
	wb_delay_us(t, s->settle_us);
	*value = (uint16_t)(wb_mmr_read32(PORT_DATA(s->port)) & s->pins);
	return WB_PORT_OK;
}

/* Pins whose 2-bit MUX field differs between two MUX values. */
static uint16_t mux_changed(uint32_t a, uint32_t b)
{
	uint32_t d = a ^ b;
	uint16_t pins = 0;
	unsigned i;

	for (i = 0; i < 16; i++)
		if ((d >> (2u * i)) & 3u)
			pins = (uint16_t)(pins | (1u << i));
	return pins;
}

int wb_pinmux_apply(const struct wb_pinmux *m)
{
	unsigned i;
	uint32_t p;

	for (i = 0; i < m->n_inputs_first; i++)
		if (!pin_ok(m->inputs_first[i]))
			return WB_PORT_ERR_ARG;
	/* S-13b.004: DIR cleared, then INEN set, which stays set. */
	for (i = 0; i < m->n_inputs_first; i++)
		(void)wb_gpio_input(m->inputs_first[i]);
	for (p = 0; p < WB_PORTS; p++) {
		const struct wb_port_func *f = &m->port[p];
		uint32_t mux = wb_mmr_read32(PORT_MUX(p));
		uint32_t fer = wb_mmr_read32(PORT_FER(p));
		uint32_t off = fer & (~(uint32_t)f->fer |
				      mux_changed(mux, f->mux));

		if (off != 0)
			wb_mmr_write32(PORT_FER_CLR(p), off);
		if (mux != f->mux)
			wb_mmr_write32(PORT_MUX(p), f->mux);
		if ((fer & ~off) != f->fer)
			wb_mmr_write32(PORT_FER_SET(p), f->fer);
	}
	wb_mmr_sync();
	return WB_PORT_OK;
}
