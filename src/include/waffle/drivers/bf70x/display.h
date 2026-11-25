/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The S-13c panel bus on the OP-XY's BF70x master: struct s13c_bus over
 * the ST7796S driver (src/include/waffle/drivers/bf70x/lcd.h, wb_lcd_opxy).
 */
#ifndef WAFFLE_DRIVERS_BF70X_DISPLAY_H
#define WAFFLE_DRIVERS_BF70X_DISPLAY_H

#include "waffle/drivers/bf70x/lcd.h"
#include "waffle/drivers/bf70x/time.h"
#include "waffle/display/panel.h"

struct wb_display_bf70x {
	struct wb_lcd lcd;
	struct s13c_bus bus;
};

/*
 * Sets up the SMC bank and the panel pins (not RESET: S-13c.004 drives it
 * first, in wb_display_begin()) and returns the bus, or NULL when the
 * driver refuses. S-13b's pin multiplexing must be in place.
 */
const struct s13c_bus *wb_display_bf70x_init(struct wb_display_bf70x *a,
					     const struct wb_time *t);
/* The same on the pins of cfg (wb_lcd_opxy, wb_lcd_op1f). */
const struct s13c_bus *wb_display_bf70x_init_cfg(struct wb_display_bf70x *a,
						 const struct wb_lcd_config *cfg,
						 const struct wb_time *t);

#endif /* WAFFLE_DRIVERS_BF70X_DISPLAY_H */
