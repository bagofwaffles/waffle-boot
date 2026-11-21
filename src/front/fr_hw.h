/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's drivers (fr_hw_*.c, target only), each the front's own copy
 * of the board's, polled, with every wait bounded. Those that differ by
 * device take the device (fr_device.h).
 */
#ifndef FR_HW_H
#define FR_HW_H

#include "fr.h"
#include "fr_device.h"

/* The time base: the core cycle counter at 400 MHz (S-13a.001). */
extern const struct fr_clock fr_hw_clock;

/* S-13b.003, S-13b.004, S-13b.006: the main CPU's pin functions and GPIOs
 * on dev, as the full bootloader sets them. After the device is decided
 * (S-18.001) and, on a device with straps, the strap read (S-13b.001). */
void fr_hw_pins(const struct fr_dev *dev);

/* S-13b.070: UART0 at 1 Mbaud, 8N1, polled; the PSoC's link. */
void fr_hw_uart_start(void);
extern const struct fr_bytes fr_hw_psoc;
/* What the controller holds into memory, for fr_serve_hook. */
void fr_hw_uart_take(void);
/* Since fr_hw_uart_start(): the longest time between two reads of the
 * receiver, and the reads that found its overrun flag set (saturating). */
void fr_hw_uart_report(uint32_t *gap_max_us, uint16_t *overrun_reads);

/* S-13c.001: the panel bus on SMC0 bank 0, D/C (dev's), RD, CS, RESET and
 * TE, and the one memory DMA to the bank on MDMA2 (S-13c.018). */
const struct fr_panel_bus *fr_hw_lcd_start(const struct fr_dev *dev);

/* S-18.001, S-13b.003: the flash bus's pins (FR_FLASH_BUS_PINS) left as
 * found when they are SPI2's already; else PORTB_MUX and PORTB_FER brought
 * to FR_PORTB_MUX and FR_PORTB_FER, MUX before FER (S-13b.004). No other
 * port register is written. True when it wrote. */
bool fr_hw_flash_bus(void);

/* S-13b.010, S-13b.011: the flash on SPI2, mode 3, SCLK0 / 4, with its
 * busy waits bounded (S-02's NOR semantics). It writes SPI2's registers
 * alone, no port register: before the device is decided the flash is
 * read on the pins fr_hw_flash_bus() leaves (S-18.001). */
void fr_hw_flash_start(void);
extern const struct fr_flash fr_hw_flash;

#endif /* FR_HW_H */
