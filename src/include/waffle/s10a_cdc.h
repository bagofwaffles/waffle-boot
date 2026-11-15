/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_S10A_CDC_H
#define WAFFLE_S10A_CDC_H

/*
 * S-10a section 5: the CDC ACM port's class requests, line coding, control
 * line state and SERIAL_STATE notification (S-10a.060 to S-10a.062).
 * Independent of the USB stack. The bytes on the data endpoints are
 * S-10c's (S-10c.030 to S-10c.033).
 */

#include <stdbool.h>
#include <stdint.h>

#define S10A_CDC_CODING_LEN	7u
#define S10A_CDC_NOTIFY_LEN	10u

struct s10a_cdc {
	uint8_t itf;			/* the communications interface n */
	uint8_t coding[S10A_CDC_CODING_LEN];
	bool dtr, rts;
	uint8_t notify_owed;		/* SERIAL_STATE not yet sent */
};

enum s10a_cdc_answer {
	S10A_CDC_STALL,
	S10A_CDC_ACCEPT,		/* status stage, no data */
	S10A_CDC_REPLY,			/* data-in of *len bytes */
	S10A_CDC_RECEIVE,		/* data-out of *len bytes, then
					 * s10a_cdc_coding_received() */
};

/* The set starts with the CDC function at interface `itf`. */
void s10a_cdc_init(struct s10a_cdc *c, uint8_t itf);
/* A configuration is selected or the bus is reset (S-10a.061): DTR and
 * RTS clear, nothing owed; the line coding is kept (S-10a.060). */
void s10a_cdc_reset(struct s10a_cdc *c);

/*
 * A class request to the CDC function's interface `itf`. For a REPLY,
 * *data points at the bytes; for a RECEIVE, at the buffer to fill.
 */
enum s10a_cdc_answer s10a_cdc_request(struct s10a_cdc *c, uint8_t bm,
				      uint8_t req, uint16_t value,
				      uint16_t itf, uint16_t length,
				      uint8_t **data, uint16_t *len);
/* The data stage of SET_LINE_CODING arrived in the buffer. */
void s10a_cdc_coding_received(struct s10a_cdc *c, const uint8_t *data);

/* S-10c.031: open while DTR or RTS is set. */
bool s10a_cdc_open(const struct s10a_cdc *c);

/* S-10a.062: the notification's bytes. */
void s10a_cdc_serial_state(const struct s10a_cdc *c,
			   uint8_t out[S10A_CDC_NOTIFY_LEN]);

#endif /* WAFFLE_S10A_CDC_H */
