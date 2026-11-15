/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-06 receiver: what a node does with each byte of a framed link
 * (S-06.040-S-06.049). One instance per link.
 *
 * There is no receive timeout: the outcome depends only on the bytes, not
 * on their timing (S-06.040). Resynchronization is by the delimiter
 * (S-06.044, S-06.049).
 */
#ifndef WAFFLE_S06_RX_H
#define WAFFLE_S06_RX_H

#include <stdbool.h>
#include <stdint.h>

#include "waffle/s06_frame.h"

/* Events returned by s06_rx_byte(). */
#define S06_RX_NONE 0		/* nothing completed */
#define S06_RX_DELIVER 1	/* a frame for this node with a good check */
#define S06_RX_FORWARD 2	/* a frame for another node, whole */
#define S06_RX_DROP_PARTIAL 3	/* a delimiter before completion (S-06.044) */
#define S06_RX_DROP_CRC 4	/* a completed frame with a bad check */
#define S06_RX_DROP_LONG 5	/* 254 decoded bytes, not complete (S-06.043) */
#define S06_RX_DROP_FWD_LONG 6	/* 256 bytes for another node, no delimiter */

struct s06_rx {
	uint8_t own;		/* the node's address on this link */
	uint8_t state;
	uint8_t code;		/* the frame's first code byte */
	uint8_t left;		/* data bytes left in the current COBS group */
	uint8_t fwd_dst;	/* the destination a forward was decided on */
	uint16_t n;		/* bytes in buf */
	/*
	 * After DELIVER and DROP_CRC: the raw frame (n bytes). After FORWARD:
	 * the encoded frame with its delimiter (n bytes). After the other
	 * drops: what had been collected. Valid until the next byte.
	 */
	uint8_t buf[S06_MAX_WIRE];
};

void s06_rx_init(struct s06_rx *rx, uint8_t own);

/* Discard a partial frame: the next byte begins a frame (S-06.060). */
void s06_rx_reset(struct s06_rx *rx);

/* S-06.013: is a frame with this destination nibble for the node? */
bool s06_rx_for_node(uint8_t own, uint8_t dst);

/* Feed one received byte; returns an S06_RX_* event. */
int s06_rx_byte(struct s06_rx *rx, uint8_t b, s06_crc8_fn crc);

#endif
