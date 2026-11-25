/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_USB_CH9_H
#define WAFFLE_USB_CH9_H

/*
 * S-10a section 3: the answers of the control endpoint (S-10a.031 to
 * S-10a.034), decided from the presented set and its state alone. The
 * device stack (TinyUSB) is left the requests it answers as S-10a wants;
 * every other answer is given here, before the stack sees the request:
 *
 * - a STALL for each request S-10a refuses (vendor and device- or endpoint-
 *   addressed class requests, unknown descriptor types, SET_DESCRIPTOR,
 *   SYNCH_FRAME, a configuration value not in the set, an interface or
 *   endpoint not in the configuration, interface features,
 *   DEVICE_REMOTE_WAKEUP, SET_INTERFACE with an alternate other than 0);
 * - the replies of GET_STATUS (device, interface, endpoint 0),
 *   GET_CONFIGURATION and GET_INTERFACE;
 * - SET_FEATURE(TEST_MODE) and the halt requests of endpoint 0 accepted;
 * - CLEAR_FEATURE(ENDPOINT_HALT) of a bulk endpoint accepted with the halt
 *   kept while the mass-storage function waits for a reset (S-10a.033);
 * - a SET_CONFIGURATION of the current value passed on as a re-selection
 *   (the stack skips one otherwise).
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/usb/desc.h"

enum s10a_ch9_action {
	S10A_CH9_FORWARD,	/* the stack answers */
	S10A_CH9_STALL,
	S10A_CH9_ACCEPT,	/* the status stage completes */
	S10A_CH9_REPLY,		/* reply[0..len) is the data stage */
	S10A_CH9_RESELECT,	/* SET_CONFIGURATION(0), then the request */
};

struct s10a_ch9 {
	enum s10a_set set;
	uint8_t config;		/* bConfigurationValue, 0 when none */
	bool reset_owed;	/* the mass-storage function waits for a reset */
};

/* The set starts: Default state, nothing owed. */
void s10a_ch9_start(struct s10a_ch9 *c, enum s10a_set set);
/* A bus reset or the loss of VBUS (S-10a.080, S-10a.081). */
void s10a_ch9_reset(struct s10a_ch9 *c);

/* Decides one SETUP packet. A reply is at most 2 bytes. */
enum s10a_ch9_action s10a_ch9_setup(struct s10a_ch9 *c,
				    const uint8_t setup[8],
				    uint8_t reply[2], uint16_t *len);

#endif /* WAFFLE_USB_CH9_H */
