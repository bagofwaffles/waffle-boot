/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The panel's pins and bus settings per device, for either panel bus
 * (lcd.c over the SMC, lcd_gpio.c bit-banged): the SMC fields are unused
 * by the gpio variant.
 */
#include "waffle/drivers/bf70x_lcd.h"

#include "waffle/drivers/bf70x_port.h"

#define PA(n) WB_PIN(WB_PORT_A, n)

/* S-13c.001's pins; SCLK0 = 100 MHz (S-13a.001). */
const struct wb_lcd_config wb_lcd_opxy = {
	.dc = PA(5),
	.rd = PA(13),
	.cs = PA(15),
	.reset = PA(8),
	.te = PA(7),
	.wr = PA(14),
	.data_port = WB_PORT_B,
	.bank = WB_LCD_SMC_BANK0,
	.b0tim = WB_LCD_B0TIM,
	.b0etim = WB_LCD_B0ETIM,
	.mdma = 2,	/* S-05.016: DMA20/DMA21; the ROM and the hand-over use MDMA0 */
	.read_strobe_us = 1,
};

const struct wb_lcd_config wb_lcd_op1f = {
	.dc = PA(11),	/* S-13c.001 on the OP-1F: device.panel_dc */
	.rd = PA(13),
	.cs = PA(15),
	.reset = PA(8),
	.te = PA(7),
	.wr = PA(14),
	.data_port = WB_PORT_B,
	.bank = WB_LCD_SMC_BANK0,
	.b0tim = WB_LCD_B0TIM,
	.b0etim = WB_LCD_B0ETIM,
	.mdma = 2,	/* S-05.016: DMA20/DMA21; the ROM and the hand-over use MDMA0 */
	.read_strobe_us = 1,
};
