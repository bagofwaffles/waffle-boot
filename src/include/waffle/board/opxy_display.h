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

#if !defined(__bfin__)
/* The host's stand-in for the display record's 44 bytes (S-13c.021). */
uint8_t *opxy_display_record(void);
#endif

#endif /* WAFFLE_BOARD_OPXY_DISPLAY_H */
