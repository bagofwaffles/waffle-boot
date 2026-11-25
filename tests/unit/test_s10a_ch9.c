/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10a control endpoint and CDC port. Checks S-10a.031 to S-10a.034
 * (the request table restated row by row, in the Address and Configured
 * states) and S-10a.060 to S-10a.062.
 */
#include <stdint.h>
#include <string.h>

#include "waffle/usb/cdc.h"
#include "waffle/usb/ch9.h"

#include "support/check.h"

static struct s10a_ch9 ch9;
static uint8_t reply[2];
static uint16_t rlen;

static enum s10a_ch9_action req(uint8_t bm, uint8_t r, uint16_t value,
				uint16_t index, uint16_t length)
{
	const uint8_t s[8] = { bm, r, (uint8_t)value, (uint8_t)(value >> 8),
			       (uint8_t)index, (uint8_t)(index >> 8),
			       (uint8_t)length, (uint8_t)(length >> 8) };

	memset(reply, 0xEE, sizeof(reply));
	return s10a_ch9_setup(&ch9, s, reply, &rlen);
}

static bool replied(enum s10a_ch9_action a, uint16_t n, uint8_t b0,
		    uint8_t b1)
{
	return a == S10A_CH9_REPLY && rlen == n && reply[0] == b0 &&
	       (n < 2 || reply[1] == b1);
}

#define STALL S10A_CH9_STALL
#define FORWARD S10A_CH9_FORWARD
#define ACCEPT S10A_CH9_ACCEPT

/* Requests answered the same in every state. */
static void common_rows(void)
{
	static const uint8_t refused_types[] = { 0, 4, 5, 8, 9, 10, 11, 15,
						 0x21, 0x22, 0xFF };
	unsigned i;

	CHECK(replied(req(0x80, 0, 0, 0, 2), 2, 0x01, 0));	/* self-powered */
	CHECK(replied(req(0x82, 0, 0, 0x00, 2), 2, 0, 0));	/* endpoint 0 */
	CHECK(replied(req(0x82, 0, 0, 0x80, 2), 2, 0, 0));
	CHECK(req(0x00, 3, 1, 0, 0) == STALL);	/* DEVICE_REMOTE_WAKEUP */
	CHECK(req(0x00, 1, 1, 0, 0) == STALL);
	CHECK(req(0x00, 3, 2, 0x0400, 0) == ACCEPT);	/* TEST_MODE */
	CHECK(req(0x00, 3, 2, 0x0401, 0) == STALL);
	CHECK(req(0x00, 1, 2, 0x0400, 0) == STALL);
	CHECK(req(0x02, 3, 0, 0x00, 0) == ACCEPT);	/* EP0 HALT */
	CHECK(req(0x02, 1, 0, 0x80, 0) == ACCEPT);
	CHECK(req(0x02, 3, 1, 0x00, 0) == STALL);	/* not a halt */
	for (i = 1; i <= 7; i++)
		if (i != 4 && i != 5)
			CHECK(req(0x80, 6, (uint16_t)(i << 8), 0, 255) ==
			      FORWARD);
	for (i = 0; i < sizeof(refused_types); i++)
		CHECK(req(0x80, 6, (uint16_t)(refused_types[i] << 8), 0,
			  255) == STALL);
	CHECK(req(0x81, 6, 0x2200, 0, 255) == STALL);	/* to an interface */
	CHECK(req(0x00, 7, 0x0100, 0, 18) == STALL);	/* SET_DESCRIPTOR */
	CHECK(req(0x82, 12, 0, 0x87, 2) == STALL);	/* SYNCH_FRAME */
	CHECK(req(0x80, 13, 0, 0, 0) == STALL);
	CHECK(req(0x80, 2, 0, 0, 0) == STALL);	/* reserved request */
	CHECK(req(0x00, 5, 7, 0, 0) == FORWARD);	/* SET_ADDRESS */
	/* S-10a.034: vendor requests, class requests not to an interface */
	CHECK(req(0xC0, 0x01, 0, 0, 64) == STALL);
	CHECK(req(0x40, 0x01, 0, 0, 0) == STALL);
	CHECK(req(0xC1, 0x01, 0, 0, 64) == STALL);
	CHECK(req(0xA0, 0x01, 0, 0, 64) == STALL);
	CHECK(req(0x22, 0x01, 0x0100, 0x08, 3) == STALL);	/* audio, EP */
	CHECK(req(0xA2, 0x81, 0x0100, 0x87, 3) == STALL);
	CHECK(req(0x80, 0xEE, 0, 0, 64) == STALL);	/* MS OS descriptor */
}

static void test_address_state(void)
{
	s10a_ch9_start(&ch9, S10A_SET_MIDI);
	common_rows();
	CHECK(replied(req(0x80, 8, 0, 0, 1), 1, 0, 0));
	CHECK(req(0x81, 0, 0, 0, 2) == STALL);		/* interface */
	CHECK(req(0x81, 10, 0, 0, 1) == STALL);		/* GET_INTERFACE */
	CHECK(req(0x01, 11, 0, 0, 0) == STALL);		/* SET_INTERFACE */
	CHECK(req(0x82, 0, 0, 0x87, 2) == STALL);	/* not configured */
	CHECK(req(0x02, 1, 0, 0x87, 0) == STALL);
	CHECK(req(0x21, 0x01, 0, 0, 0) == STALL);	/* class, itf 0 */
	CHECK(req(0x00, 9, 4, 0, 0) == STALL);		/* not in the set */
	CHECK(ch9.config == 0);
	CHECK(req(0x00, 9, 0x0101, 0, 0) == STALL);
	CHECK(req(0x00, 9, 0, 0, 0) == FORWARD);	/* stays Address */
}

static void configured_rows(enum s10a_set set, unsigned n_itf,
			    uint8_t in_ep, uint8_t not_ep)
{
	CHECK(req(0x00, 9, 1, 0, 0) == FORWARD);
	CHECK(ch9.config == 1);
	common_rows();
	CHECK(replied(req(0x80, 8, 0, 0, 1), 1, 1, 0));
	CHECK(replied(req(0x81, 0, 0, (uint16_t)(n_itf - 1u), 2), 2, 0, 0));
	CHECK(req(0x81, 0, 0, (uint16_t)n_itf, 2) == STALL);
	CHECK(replied(req(0x81, 10, 0, 0, 1), 1, 0, 0));
	CHECK(req(0x81, 10, 0, (uint16_t)n_itf, 1) == STALL);
	CHECK(req(0x01, 11, 0, 0, 0) == FORWARD);	/* alternate 0 */
	CHECK(req(0x01, 11, 1, 0, 0) == STALL);
	CHECK(req(0x01, 11, 0, (uint16_t)n_itf, 0) == STALL);
	CHECK(req(0x01, 3, 0, 0, 0) == STALL);		/* interface feature */
	CHECK(req(0x01, 1, 0, 0, 0) == STALL);
	CHECK(req(0x82, 0, 0, in_ep, 2) == FORWARD);	/* halt state */
	CHECK(req(0x82, 0, 0, not_ep, 2) == STALL);
	CHECK(req(0x02, 3, 0, in_ep, 0) == FORWARD);
	CHECK(req(0x02, 1, 0, in_ep, 0) == FORWARD);
	CHECK(req(0x02, 1, 0, not_ep, 0) == STALL);
	CHECK(req(0x02, 3, 0, not_ep, 0) == STALL);
	CHECK(req(0xA1, 0x21, 0, 0, 7) == FORWARD);	/* class, itf */
	CHECK(req(0xA1, 0x81, 0x0100, 0x0300, 2) == STALL); /* entity 3 */
	CHECK(req(0xA1, 0x21, 0, (uint16_t)n_itf, 7) == STALL);
	/* S-10a.032: the current value again re-selects; others select. */
	CHECK(req(0x00, 9, 1, 0, 0) == S10A_CH9_RESELECT);
	CHECK(ch9.config == 1);
	if (s10a_desc_num_configs(set) > 1) {
		CHECK(req(0x00, 9, 2, 0, 0) == FORWARD);
		CHECK(ch9.config == 2);
	}
	CHECK(req(0x00, 9, (uint16_t)(s10a_desc_num_configs(set) + 1u), 0,
		  0) == STALL);
	CHECK(req(0x00, 9, 0, 0, 0) == FORWARD);
	CHECK(ch9.config == 0);
}

static void test_configured(void)
{
	s10a_ch9_start(&ch9, S10A_SET_MIDI);
	configured_rows(S10A_SET_MIDI, 2, 0x87, 0x81);
	s10a_ch9_start(&ch9, S10A_SET_MIDI_CDC);
	configured_rows(S10A_SET_MIDI_CDC, 4, 0x81, 0x83);
	s10a_ch9_start(&ch9, S10A_SET_DISK);
	configured_rows(S10A_SET_DISK, 1, 0x08, 0x0A);
	s10a_ch9_start(&ch9, S10A_SET_DISK_CDC);
	configured_rows(S10A_SET_DISK_CDC, 3, 0x0A, 0x83);
	s10a_ch9_start(&ch9, S10A_SET_MTP);
	configured_rows(S10A_SET_MTP, 1, 0x83, 0x87);
	s10a_ch9_start(&ch9, S10A_SET_SERIAL);
	configured_rows(S10A_SET_SERIAL, 2, 0x89, 0x08);
	s10a_ch9_start(&ch9, S10A_SET_MTP_CDC);
	configured_rows(S10A_SET_MTP_CDC, 3, 0x87, 0x07);
	s10a_ch9_start(&ch9, S10A_SET_MTP_VENDOR_CDC);
	configured_rows(S10A_SET_MTP_VENDOR_CDC, 3, 0x83, 0x84);
}

/* S-10a.033: while BOT waits for a reset, a clear leaves the halt. */
static void test_reset_owed(void)
{
	s10a_ch9_start(&ch9, S10A_SET_DISK);
	(void)req(0x00, 9, 1, 0, 0);
	ch9.reset_owed = true;
	CHECK(req(0x02, 1, 0, 0x87, 0) == ACCEPT);
	CHECK(req(0x02, 1, 0, 0x08, 0) == ACCEPT);
	CHECK(req(0x02, 3, 0, 0x87, 0) == FORWARD);
	CHECK(req(0x82, 0, 0, 0x87, 2) == FORWARD);
	ch9.reset_owed = false;
	CHECK(req(0x02, 1, 0, 0x87, 0) == FORWARD);
	/* A bus reset ends the configuration (S-10a.080). */
	s10a_ch9_reset(&ch9);
	CHECK(ch9.config == 0);
	CHECK(req(0x81, 0, 0, 0, 2) == STALL);
}

/* --- S-10a.060 to S-10a.062 ---------------------------------------------- */

static void test_line_coding(void)
{
	static const uint8_t initial[7] = { 0x00, 0xC2, 0x01, 0, 0, 0, 8 };
	static const uint8_t c9600[7] = { 0x80, 0x25, 0, 0, 0, 0, 8 };
	struct s10a_cdc c;
	uint8_t *data = NULL;
	uint16_t len;

	s10a_cdc_init(&c, 2);
	CHECK(s10a_cdc_request(&c, 0xA1, 0x21, 0, 2, 7, &data, &len) ==
	      S10A_CDC_REPLY);
	CHECK(len == 7 && memcmp(data, initial, 7) == 0);
	CHECK(s10a_cdc_request(&c, 0xA1, 0x21, 0, 2, 4, &data, &len) ==
	      S10A_CDC_REPLY && len == 4);
	CHECK(s10a_cdc_request(&c, 0x21, 0x20, 0, 2, 7, &data, &len) ==
	      S10A_CDC_RECEIVE && len == 7);
	memcpy(data, c9600, 7);
	s10a_cdc_coding_received(&c, data);
	CHECK(memcmp(c.coding, c9600, 7) == 0);
	/* wLength other than 7: STALL, coding unchanged (S-10a.060). */
	CHECK(s10a_cdc_request(&c, 0x21, 0x20, 0, 2, 6, &data, &len) ==
	      S10A_CDC_STALL);
	CHECK(s10a_cdc_request(&c, 0x21, 0x20, 0, 2, 8, &data, &len) ==
	      S10A_CDC_STALL);
	CHECK(s10a_cdc_request(&c, 0x21, 0x20, 0, 2, 0, &data, &len) ==
	      S10A_CDC_STALL);
	CHECK(memcmp(c.coding, c9600, 7) == 0);
	/* Others, and the data interface: STALL. */
	CHECK(s10a_cdc_request(&c, 0x21, 0x23, 0, 2, 0, &data, &len) ==
	      S10A_CDC_STALL);	/* SEND_BREAK */
	CHECK(s10a_cdc_request(&c, 0x21, 0x00, 0, 2, 4, &data, &len) ==
	      S10A_CDC_STALL);
	CHECK(s10a_cdc_request(&c, 0xA1, 0x21, 0, 3, 7, &data, &len) ==
	      S10A_CDC_STALL);
	CHECK(s10a_cdc_request(&c, 0x21, 0x21, 0, 2, 7, &data, &len) ==
	      S10A_CDC_STALL);
	/* A bus reset or SET_CONFIGURATION keeps the coding. */
	s10a_cdc_reset(&c);
	CHECK(memcmp(c.coding, c9600, 7) == 0);
}

static void test_control_lines(void)
{
	static const uint8_t n0[10] = { 0xA1, 0x20, 0, 0, 0, 0, 2, 0, 3, 0 };
	static const uint8_t n2[10] = { 0xA1, 0x20, 0, 0, 2, 0, 2, 0, 3, 0 };
	struct s10a_cdc c;
	uint8_t *data = NULL, note[10];
	uint16_t len;

	s10a_cdc_init(&c, 0);
	CHECK(!s10a_cdc_open(&c));
	CHECK(s10a_cdc_request(&c, 0x21, 0x22, 1, 0, 0, &data, &len) ==
	      S10A_CDC_ACCEPT);
	CHECK(c.dtr && !c.rts && s10a_cdc_open(&c));
	CHECK(c.notify_owed == 1);
	CHECK(s10a_cdc_request(&c, 0x21, 0x22, 2, 0, 0, &data, &len) ==
	      S10A_CDC_ACCEPT);
	CHECK(!c.dtr && c.rts && c.notify_owed == 2);
	CHECK(s10a_cdc_request(&c, 0x21, 0x22, 0, 0, 0, &data, &len) ==
	      S10A_CDC_ACCEPT);
	CHECK(!s10a_cdc_open(&c) && c.notify_owed == 2);
	CHECK(s10a_cdc_request(&c, 0x21, 0x22, 3, 0, 1, &data, &len) ==
	      S10A_CDC_STALL);
	s10a_cdc_serial_state(&c, note);
	CHECK(memcmp(note, n0, 10) == 0);
	s10a_cdc_init(&c, 2);
	s10a_cdc_serial_state(&c, note);
	CHECK(memcmp(note, n2, 10) == 0);
	CHECK(s10a_cdc_request(&c, 0x21, 0x22, 3, 2, 0, &data, &len) ==
	      S10A_CDC_ACCEPT);
	s10a_cdc_reset(&c);	/* S-10a.061 */
	CHECK(!c.dtr && !c.rts && c.notify_owed == 0);
}

int main(void)
{
	RUN(test_address_state);
	RUN(test_configured);
	RUN(test_reset_owed);
	RUN(test_line_coding);
	RUN(test_control_lines);
	return check_summary("test_s10a_ch9");
}
