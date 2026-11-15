/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's drivers on the OP-XY (fr_hw_*.c, target only), each the
 * front's own copy of the board's, polled, with every wait bounded.
 */
#ifndef FR_HW_H
#define FR_HW_H

#include "fr.h"

/* The time base: the core cycle counter at 400 MHz (S-13a.001). */
extern const struct fr_clock fr_hw_clock;

/* S-13b.003, S-13b.004, S-13b.006: the master's pin functions and GPIOs, as
 * the full bootloader sets them (opxy_pinmux_master, opxy_gpio_master).
 * After the strap read (S-13b.001). */
void fr_hw_pins(void);

/* S-13b.070: UART0 at 1 Mbaud, 8N1, polled; the PSoC's link. */
void fr_hw_uart_start(void);
extern const struct fr_bytes fr_hw_psoc;
/* What the controller holds into memory, for fr_serve_hook. */
void fr_hw_uart_take(void);

/* S-13c.001: the panel bus on SMC0 bank 0, D/C, RD, CS, RESET and TE. */
const struct fr_panel_bus *fr_hw_lcd_start(void);

/* S-13b.010, S-13b.011: the flash on SPI2, mode 3, SCLK0 / 4, with its
 * busy waits bounded (S-02's NOR semantics). */
void fr_hw_flash_start(void);
extern const struct fr_flash fr_hw_flash;

#endif /* FR_HW_H */
