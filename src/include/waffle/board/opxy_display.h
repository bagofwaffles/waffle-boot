/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The OP-XY master's display behind the board's hooks (opxy_display.c;
 * the hooks are declared in waffle/board/opxy_boot.h and opxy_services.h).
 */
#ifndef WAFFLE_BOARD_OPXY_DISPLAY_H
#define WAFFLE_BOARD_OPXY_DISPLAY_H

#include "waffle/display.h"

/* The display the hooks drive, or NULL while its bus is not up. */
const struct wb_display *opxy_display(void);

#endif /* WAFFLE_BOARD_OPXY_DISPLAY_H */
