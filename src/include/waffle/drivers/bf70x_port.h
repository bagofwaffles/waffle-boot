/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_PORT_H
#define WAFFLE_DRIVERS_BF70X_PORT_H

/*
 * General-purpose ports, pin multiplexing and pad configuration
 * (ADSP-BF70x Hardware Reference chapter "General-Purpose Ports (PORT)").
 *
 * Which signal each 2-bit MUX field selects is in the data sheet's
 * signal-muxing table, which is not available here: the board passes
 * whole MUX and FER values (S-13b.003 gives this board's). Every
 * single-pin change goes through the SET and CLR registers, so the other
 * pins of the port keep their DIR, INEN, DATA and FER bits (S-13b.006).
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/drivers/bf70x_time.h"

#define WB_PORT_A 0u
#define WB_PORT_B 1u
#define WB_PORT_C 2u
#define WB_PORTS  3u

#define PORT_BASE(p)       (0x20040000u + 0x80u * (p))
#define PORT_FER(p)        (PORT_BASE(p) + 0x00u)
#define PORT_FER_SET(p)    (PORT_BASE(p) + 0x04u)
#define PORT_FER_CLR(p)    (PORT_BASE(p) + 0x08u)
#define PORT_DATA(p)       (PORT_BASE(p) + 0x0Cu)
#define PORT_DATA_SET(p)   (PORT_BASE(p) + 0x10u)
#define PORT_DATA_CLR(p)   (PORT_BASE(p) + 0x14u)
#define PORT_DIR(p)        (PORT_BASE(p) + 0x18u)
#define PORT_DIR_SET(p)    (PORT_BASE(p) + 0x1Cu)
#define PORT_DIR_CLR(p)    (PORT_BASE(p) + 0x20u)
#define PORT_INEN(p)       (PORT_BASE(p) + 0x24u)
#define PORT_INEN_SET(p)   (PORT_BASE(p) + 0x28u)
#define PORT_INEN_CLR(p)   (PORT_BASE(p) + 0x2Cu)
#define PORT_MUX(p)        (PORT_BASE(p) + 0x30u)
#define PORT_DATA_TGL(p)   (PORT_BASE(p) + 0x34u)
#define PORT_POL(p)        (PORT_BASE(p) + 0x38u)
#define PORT_LOCK(p)       (PORT_BASE(p) + 0x44u)

#define PADS0_PCFG0        0x20040404u

/* Pin interrupt blocks (HRM, PORT chapter; Table A-40). The latch is set
 * whatever the mask; in level mode, while the pin is high. */
#define PINT_BASE(n)       (0x20041000u + 0x100u * (n))
#define PINT_MSK_SET(n)    (PINT_BASE(n) + 0x00u)
#define PINT_MSK_CLR(n)    (PINT_BASE(n) + 0x04u)
#define PINT_REQ(n)        (PINT_BASE(n) + 0x08u)
#define PINT_ASSIGN(n)     (PINT_BASE(n) + 0x0Cu)
#define PINT_EDGE_SET(n)   (PINT_BASE(n) + 0x10u)
#define PINT_EDGE_CLR(n)   (PINT_BASE(n) + 0x14u)
#define PINT_INV_SET(n)    (PINT_BASE(n) + 0x18u)
#define PINT_INV_CLR(n)    (PINT_BASE(n) + 0x1Cu)
#define PINT_PINSTATE(n)   (PINT_BASE(n) + 0x20u)
#define PINT_LATCH(n)      (PINT_BASE(n) + 0x24u)
#define PINT_ASSIGN_B2MAP  (0xFFu << 16)	/* byte 2: 0 is PA.L, 1 PB.L */

/* A pin: port * 16 + bit, e.g. WB_PIN(WB_PORT_C, 11) for PC11. */
#define WB_PIN(port, bit)  ((uint8_t)((port) * 16u + (bit)))
#define WB_PIN_PORT(pin)   ((uint32_t)(pin) / 16u)
#define WB_PIN_BIT(pin)    (1u << ((uint32_t)(pin) % 16u))
#define WB_PIN_COUNT       (WB_PORTS * 16u)

/* One port's function registers, as S-13b.003 tabulates them. */
struct wb_port_func {
	uint32_t mux;
	uint16_t fer;
};

struct wb_pinmux {
	struct wb_port_func port[WB_PORTS];
	/* Made inputs before the first MUX write (S-13b.004). */
	const uint8_t *inputs_first;
	unsigned n_inputs_first;
};

/* A strap sample (S-13b.001, .002, .008). */
struct wb_strap {
	uint32_t port;
	uint16_t pins;
	uint32_t settle_us;	/* drivers released to sample, >= */
	uint32_t inen_us;	/* input enable to sample, >= */
};

#define WB_PORT_OK        0
#define WB_PORT_ERR_ARG  (-1)

/*
 * Single pins. output() sets the level before it turns the driver on;
 * input() turns the driver off and enables the input buffer. gpio() hands
 * a pin from its peripheral back to the port (FER clear).
 */
int wb_gpio_output(uint8_t pin, bool high);
/* An output driving the level its DATA bit holds: DIR set, DATA not
 * written (S-13b.006 on the OP-1F). */
int wb_gpio_output_keep(uint8_t pin);
int wb_gpio_input(uint8_t pin);
int wb_gpio_set(uint8_t pin, bool high);
bool wb_gpio_get(uint8_t pin);
int wb_gpio_gpio(uint8_t pin);
int wb_gpio_peripheral(uint8_t pin);

/*
 * Sample strap pins before any MUX or FER write: DIR clear, wait
 * settle_us, INEN set, wait inen_us, read DATA, INEN clear. Returns the
 * strap pins' bits of PORTx_DATA.
 */
int wb_strap_read(const struct wb_strap *s, const struct wb_time *t,
		  uint16_t *value);
/*
 * Sample pins by their input enables alone (S-18.002's fallback sample):
 * INEN set, wait settle_us, read DATA. No DIR bit is written and INEN stays
 * set. Returns the pins' bits of PORTx_DATA.
 */
int wb_strap_inen_read(const struct wb_strap *s, const struct wb_time *t,
		       uint16_t *value);

/*
 * Apply a pin table: the inputs_first pins become inputs (wb_gpio_input():
 * DIR cleared, then INEN set, S-13b.004); then per port,
 * FER is cleared for every pin whose MUX field changes or that the table
 * does not enable, MUX is written, and FER is set to the table's value. No
 * FER bit is set while the port's MUX differs from the table (S-13b.004).
 */
int wb_pinmux_apply(const struct wb_pinmux *m);

#endif /* WAFFLE_DRIVERS_BF70X_PORT_H */
