/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-06 receiver state machine (S-06.040-S-06.049).
 */
#include "waffle/s06_rx.h"

#define ST_IDLE 0	/* where a frame would begin */
#define ST_DECODE 1	/* decoding a frame for this node */
#define ST_FORWARD 2	/* collecting a frame for another node */

void s06_rx_init(struct s06_rx *rx, uint8_t own)
{
	rx->own = (uint8_t)(own & 0x0Fu);
	s06_rx_reset(rx);
}

void s06_rx_reset(struct s06_rx *rx)
{
	rx->state = ST_IDLE;
	rx->code = 0;
	rx->left = 0;
	rx->n = 0;
}

/* S-06.013: the own address, or bit 3 set (reply slots and 12-15). */
bool s06_rx_for_node(uint8_t own, uint8_t dst)
{
	dst &= 0x0Fu;
	return dst == (own & 0x0Fu) || (dst & 0x08u) != 0;
}

static int forward_byte(struct s06_rx *rx, uint8_t b)
{
	rx->buf[rx->n++] = b;
	if (b == 0) {			/* S-06.046: up to and including it */
		rx->state = ST_IDLE;
		return S06_RX_FORWARD;
	}
	if (rx->n >= S06_MAX_WIRE) {	/* S-06.048 */
		rx->state = ST_IDLE;
		return S06_RX_DROP_FWD_LONG;
	}
	return S06_RX_NONE;
}

int s06_rx_byte(struct s06_rx *rx, uint8_t b, s06_crc8_fn crc)
{
	uint8_t value;

	if (rx->state == ST_FORWARD)
		return forward_byte(rx, b);

	if (b == 0) {
		/* S-06.040: ignored where a frame would begin */
		if (rx->state == ST_IDLE)
			return S06_RX_NONE;
		/* S-06.044: a delimiter ends an incomplete frame */
		rx->state = ST_IDLE;
		return S06_RX_DROP_PARTIAL;
	}

	if (rx->state == ST_IDLE) {	/* S-06.040: the first code byte */
		rx->state = ST_DECODE;
		rx->code = b;
		rx->left = (uint8_t)(b - 1u);
		rx->n = 0;
		return S06_RX_NONE;
	}

	/* S-06.004: a data byte, or a code byte standing for a 00 */
	if (rx->left) {
		value = b;
		rx->left--;
	} else {
		value = 0;
		rx->left = (uint8_t)(b - 1u);
	}

	/*
	 * S-06.047: decided on the route byte, the first decoded byte, when
	 * the frame's second byte arrives; a first code byte 01 makes it 00.
	 */
	if (rx->n == 0 && !s06_rx_for_node(rx->own, value)) {
		rx->state = ST_FORWARD;
		rx->fwd_dst = (uint8_t)(value & 0x0Fu);
		rx->buf[0] = rx->code;
		rx->buf[1] = b;
		rx->n = 2;
		return S06_RX_NONE;
	}

	rx->buf[rx->n++] = value;
	/* S-06.041: complete at len + 5 decoded bytes, no delimiter needed */
	if (rx->n >= S06_HEADER && rx->n == (uint16_t)(rx->buf[3] + 5u)) {
		rx->state = ST_IDLE;
		/* S-06.042 */
		if (crc(rx->buf, (size_t)rx->n - 1u) == rx->buf[rx->n - 1u])
			return S06_RX_DELIVER;
		return S06_RX_DROP_CRC;
	}
	/* S-06.043: 254 decoded bytes and not complete */
	if (rx->n >= S06_MAX_RAW) {
		rx->state = ST_IDLE;
		return S06_RX_DROP_LONG;
	}
	return S06_RX_NONE;
}
