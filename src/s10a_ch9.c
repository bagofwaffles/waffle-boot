/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10a.031 to S-10a.034: the control endpoint's answers
 * (waffle/s10a_ch9.h).
 */
#include "waffle/s10a_ch9.h"

enum {
	REQ_GET_STATUS = 0,
	REQ_CLEAR_FEATURE = 1,
	REQ_SET_FEATURE = 3,
	REQ_SET_ADDRESS = 5,
	REQ_GET_DESCRIPTOR = 6,
	REQ_SET_DESCRIPTOR = 7,
	REQ_GET_CONFIGURATION = 8,
	REQ_SET_CONFIGURATION = 9,
	REQ_GET_INTERFACE = 10,
	REQ_SET_INTERFACE = 11,
};

enum { RCPT_DEVICE, RCPT_INTERFACE, RCPT_ENDPOINT };
enum { TYPE_STANDARD, TYPE_CLASS };

#define FEATURE_ENDPOINT_HALT	0u
#define FEATURE_TEST_MODE	2u

void s10a_ch9_start(struct s10a_ch9 *c, enum s10a_set set)
{
	c->set = set;
	c->config = 0;
	c->reset_owed = false;
}

void s10a_ch9_reset(struct s10a_ch9 *c)
{
	c->config = 0;
	c->reset_owed = false;
}

static bool has_interface(const struct s10a_ch9 *c, unsigned itf)
{
	return c->config && itf < s10a_desc_num_interfaces(c->set);
}

static bool has_endpoint(const struct s10a_ch9 *c, uint8_t ep)
{
	return c->config && s10a_desc_has_endpoint(c->set, ep);
}

/* The set's mass-storage bulk endpoints (S-10a.013). */
static bool msc_endpoint(const struct s10a_ch9 *c, uint8_t ep)
{
	return (c->set == S10A_SET_DISK || c->set == S10A_SET_DISK_CDC) &&
	       (ep == S10A_EP_BULK_IN || ep == S10A_EP_BULK_OUT);
}

static enum s10a_ch9_action reply2(uint8_t reply[2], uint16_t *len,
				   uint8_t b0)
{
	reply[0] = b0;
	reply[1] = 0;
	*len = 2;
	return S10A_CH9_REPLY;
}

/* CLEAR_FEATURE and SET_FEATURE (S-10a.031, S-10a.033) */
static enum s10a_ch9_action feature(const struct s10a_ch9 *c,
				    unsigned rcpt, bool set, unsigned value,
				    unsigned index)
{
	const uint8_t ep = (uint8_t)index;

	switch (rcpt) {
	case RCPT_DEVICE:
		/* TEST_MODE: accepted, the test mode itself not entered. */
		if (set && value == FEATURE_TEST_MODE && (index & 0xFFu) == 0)
			return S10A_CH9_ACCEPT;
		return S10A_CH9_STALL;	/* DEVICE_REMOTE_WAKEUP included */
	case RCPT_ENDPOINT:
		if (value != FEATURE_ENDPOINT_HALT || index > 0xFFu)
			return S10A_CH9_STALL;
		if ((ep & 0x7Fu) == 0)
			return S10A_CH9_ACCEPT;	/* endpoint 0 never halts */
		if (!has_endpoint(c, ep))
			return S10A_CH9_STALL;
		if (!set && c->reset_owed && msc_endpoint(c, ep))
			return S10A_CH9_ACCEPT;	/* still halted */
		return S10A_CH9_FORWARD;
	default:
		return S10A_CH9_STALL;	/* interface features */
	}
}

static enum s10a_ch9_action standard(struct s10a_ch9 *c, const uint8_t *s,
				     uint8_t reply[2], uint16_t *len)
{
	const bool in = (s[0] & 0x80u) != 0;
	const unsigned rcpt = s[0] & 0x1Fu;
	const unsigned value = (unsigned)s[2] | (unsigned)s[3] << 8;
	const unsigned index = (unsigned)s[4] | (unsigned)s[5] << 8;
	const unsigned length = (unsigned)s[6] | (unsigned)s[7] << 8;
	const uint8_t ep = (uint8_t)index;

	switch (s[1]) {
	case REQ_GET_STATUS:
		if (!in || value != 0)
			return S10A_CH9_STALL;
		switch (rcpt) {
		case RCPT_DEVICE:
			return reply2(reply, len, 0x01);	/* self-powered */
		case RCPT_INTERFACE:
			return has_interface(c, index) ?
			       reply2(reply, len, 0) : S10A_CH9_STALL;
		case RCPT_ENDPOINT:
			if (index > 0xFFu)
				return S10A_CH9_STALL;
			if ((ep & 0x7Fu) == 0)
				return reply2(reply, len, 0);
			/* The stack holds the halt state. */
			return has_endpoint(c, ep) ?
			       S10A_CH9_FORWARD : S10A_CH9_STALL;
		default:
			return S10A_CH9_STALL;
		}
	case REQ_CLEAR_FEATURE:
	case REQ_SET_FEATURE:
		if (in || length)
			return S10A_CH9_STALL;
		return feature(c, rcpt, s[1] == REQ_SET_FEATURE, value, index);
	case REQ_SET_ADDRESS:
		return rcpt == RCPT_DEVICE && !in ?
		       S10A_CH9_FORWARD : S10A_CH9_STALL;
	case REQ_GET_DESCRIPTOR:
		if (rcpt != RCPT_DEVICE || !in)
			return S10A_CH9_STALL;
		switch (value >> 8) {
		case 1:	/* DEVICE */
		case 2:	/* CONFIGURATION */
		case 3:	/* STRING */
		case 6:	/* DEVICE_QUALIFIER */
		case 7:	/* OTHER_SPEED_CONFIGURATION */
			return S10A_CH9_FORWARD;
		default:
			return S10A_CH9_STALL;	/* OTG, debug, BOS and the rest */
		}
	case REQ_GET_CONFIGURATION:
		if (rcpt != RCPT_DEVICE || !in)
			return S10A_CH9_STALL;
		reply[0] = c->config;
		*len = 1;
		return S10A_CH9_REPLY;
	case REQ_SET_CONFIGURATION:
		if (rcpt != RCPT_DEVICE || in || length ||
		    value > s10a_desc_num_configs(c->set))
			return S10A_CH9_STALL;
		if (value != 0 && value == c->config)
			return S10A_CH9_RESELECT;	/* S-10a.032 */
		c->config = (uint8_t)value;
		c->reset_owed = false;
		return S10A_CH9_FORWARD;
	case REQ_GET_INTERFACE:
		if (rcpt != RCPT_INTERFACE || !in || !has_interface(c, index))
			return S10A_CH9_STALL;
		reply[0] = 0;
		*len = 1;
		return S10A_CH9_REPLY;
	case REQ_SET_INTERFACE:
		/* Alternate 0 resets the interface's endpoints: its driver. */
		if (rcpt != RCPT_INTERFACE || in || length || value != 0 ||
		    !has_interface(c, index))
			return S10A_CH9_STALL;
		return S10A_CH9_FORWARD;
	case REQ_SET_DESCRIPTOR:
	default:
		return S10A_CH9_STALL;	/* SYNCH_FRAME and the rest */
	}
}

enum s10a_ch9_action s10a_ch9_setup(struct s10a_ch9 *c,
				    const uint8_t setup[8],
				    uint8_t reply[2], uint16_t *len)
{
	const unsigned type = (unsigned)(setup[0] >> 5) & 3u;
	const unsigned rcpt = setup[0] & 0x1Fu;

	*len = 0;
	if (type == TYPE_STANDARD)
		return standard(c, setup, reply, len);
	/* S-10a.034: class requests go to an interface of the configuration;
	 * vendor requests, and class requests to anything else, STALL. */
	if (type == TYPE_CLASS && rcpt == RCPT_INTERFACE &&
	    has_interface(c, setup[4]) && setup[5] == 0)
		return S10A_CH9_FORWARD;
	return S10A_CH9_STALL;
}
