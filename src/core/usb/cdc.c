/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10a.060 to S-10a.062: the CDC ACM port (waffle/usb/cdc.h).
 *
 * Of the answers S-10a.060 allows, SET_LINE_CODING is accepted with
 * wLength 7 only (the coding then is the 7 bytes, whatever the rate) and
 * gets a STALL with any other wLength, the coding unchanged.
 */
#include "waffle/usb/cdc.h"

#include <string.h>

enum {
	SET_LINE_CODING = 0x20,
	GET_LINE_CODING = 0x21,
	SET_CONTROL_LINE_STATE = 0x22,
};

/* 115200 bit/s, 1 stop bit, no parity, 8 data bits */
static const uint8_t default_coding[S10A_CDC_CODING_LEN] = {
	0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08,
};

void s10a_cdc_init(struct s10a_cdc *c, uint8_t itf)
{
	memset(c, 0, sizeof(*c));
	c->itf = itf;
	memcpy(c->coding, default_coding, sizeof(c->coding));
}

void s10a_cdc_reset(struct s10a_cdc *c)
{
	c->dtr = false;
	c->rts = false;
	c->notify_owed = 0;
}

enum s10a_cdc_answer s10a_cdc_request(struct s10a_cdc *c, uint8_t bm,
				      uint8_t req, uint16_t value,
				      uint16_t itf, uint16_t length,
				      uint8_t **data, uint16_t *len)
{
	*len = 0;
	if (itf != c->itf)
		return S10A_CDC_STALL;	/* the data interface has none */
	switch (req) {
	case SET_LINE_CODING:
		if (bm != 0x21u || length != S10A_CDC_CODING_LEN)
			return S10A_CDC_STALL;
		*data = c->coding;
		*len = S10A_CDC_CODING_LEN;
		return S10A_CDC_RECEIVE;
	case GET_LINE_CODING:
		if (bm != 0xA1u)
			return S10A_CDC_STALL;
		*data = c->coding;
		*len = (uint16_t)(length < S10A_CDC_CODING_LEN ?
				  length : S10A_CDC_CODING_LEN);
		return S10A_CDC_REPLY;
	case SET_CONTROL_LINE_STATE:
		if (bm != 0x21u || length != 0)
			return S10A_CDC_STALL;
		c->dtr = (value & 1u) != 0;
		c->rts = (value & 2u) != 0;
		if ((c->dtr || c->rts) && c->notify_owed < UINT8_MAX)
			c->notify_owed++;
		return S10A_CDC_ACCEPT;
	default:
		return S10A_CDC_STALL;
	}
}

void s10a_cdc_coding_received(struct s10a_cdc *c, const uint8_t *data)
{
	if (data != c->coding)
		memcpy(c->coding, data, sizeof(c->coding));
}

bool s10a_cdc_open(const struct s10a_cdc *c)
{
	return c->dtr || c->rts;
}

void s10a_cdc_serial_state(const struct s10a_cdc *c,
			   uint8_t out[S10A_CDC_NOTIFY_LEN])
{
	static const uint8_t head[] = { 0xA1, 0x20, 0x00, 0x00 };

	memcpy(out, head, sizeof(head));
	out[4] = c->itf;
	out[5] = 0x00;
	out[6] = 0x02;	/* wLength */
	out[7] = 0x00;
	out[8] = 0x03;	/* bRxCarrier, bTxCarrier */
	out[9] = 0x00;
}
