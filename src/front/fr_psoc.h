/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The PSoC link (S-06 framing, S-07), the front's own copy of what the
 * start-up and recovery need: the greeting with its failure wait, the power
 * and battery messages and the charge wait, the brightness reports, the key
 * subscription and the one key-status request, the power-off, the reset
 * request and the USB current limit. Polled; every wait is bounded but the
 * charge wait, which only runs with the power switch off.
 *
 * The master is node 1, the PSoC node 4 (S-06.010). Plain messages go with
 * route 0x14; the one request at a time waits in reply slot 0, route 0x84,
 * for a frame to node 8 (S-06.030, S-06.032). Every message the PSoC sends
 * that the front serves is handled where the front polls the link, and
 * while a request waits it is kept and handled once the request is over;
 * a power message is always kept. The others are dropped as they come.
 */
#ifndef FR_PSOC_H
#define FR_PSOC_H

#include "fr.h"

#define FR_S06_MAX_PAYLOAD 249u	/* S-06.002 */
#define FR_S06_MAX_RAW     254u	/* S-06.001: header, payload, check */
#define FR_S06_MAX_WIRE    256u	/* S-06.005: encoded frame and delimiter */
#define FR_PSOC_INBOX      4u	/* greetings and pings kept while a request waits */
#define FR_PSOC_STATES     16u	/* power states kept while a request waits */

/* S-07.072's reports, 0x51 payloads. */
#define FR_REPORT_START    0u	/* {00 00 00} (S-04.060) */
#define FR_REPORT_HELD     1u	/* {3C FF 02}: the panel lit (S-13c.019) */
#define FR_REPORT_SHUTDOWN 3u	/* {00 00 02} */

/* Power states (S-07.050). */
#define FR_POWER_ON     0x10u
#define FR_POWER_OFF    0x14u
#define FR_POWER_CHARGE 0x16u	/* the switch off on external power */

/* Results: a power-off ran (S-07.051) and nothing more may be sent. Every
 * function that returns an int returns FR_PSOC_OFF from then on. */
#define FR_PSOC_OK  0
#define FR_PSOC_OFF (-1)

struct fr_frame {
	uint8_t route, seq, id;
	uint8_t len;
	uint8_t payload[FR_S06_MAX_PAYLOAD];
};

struct fr_psoc {
	const struct fr_bytes *io;
	const struct fr_clock *clock;
	/* S-06: the link's seq counter (S-06.020), the receiver (S-06.040) */
	uint8_t seq;
	uint8_t rx_state, rx_left;
	uint16_t rx_n;
	uint8_t rx[FR_S06_MAX_WIRE];
	/* a request pending in reply slot 0 (S-06.030), its id, and its reply */
	bool pending, replied;
	uint8_t pending_id;
	/* waited 0x30 and 0x36 requests whose reply has not come yet: one may
	 * still come late, to slot 0 (S-07.012) */
	uint8_t late_30, late_36;
	struct fr_frame reply;
	/* messages received and not yet handled: the power states of 0x31 and
	 * 0x32 in order, each differing from the one before, with how many
	 * messages in a row reported it; how many other power messages ask for
	 * the charge LED (led_any: 0x00, 0x35, 0x37; led_state: an empty 0x31
	 * or 0x32); the greetings and pings. The battery report is taken as it
	 * comes. Nothing else is kept. */
	uint8_t states[FR_PSOC_STATES], repeats[FR_PSOC_STATES];
	uint8_t n_states;
	uint8_t led_any, led_state;
	struct fr_frame inbox[FR_PSOC_INBOX];
	uint8_t in_head, in_count;
	/* S-07 state */
	bool greeted;		/* S-07.020 */
	bool greeting;		/* a greeting sequence runs */
	bool quiet;		/* S-07.021's wait: only the greeting's answer */
	bool started;		/* S-07.025: subscribed */
	bool off;		/* S-07.051: powered off, nothing more is sent */
	bool hold_off;		/* fr_psoc_hold(): a power-off waits */
	bool off_due;		/* one came while held; off_state is its state */
	uint8_t off_state;
	bool state_known;
	uint8_t state;		/* the last 0x31/0x32 state */
	uint8_t batt[16];	/* the last 0x37 payload */
	uint8_t batt_len;
	bool keys_replied;	/* S-07.032 */
	uint8_t keys[16];	/* the 0x41 reply's payload */
	uint8_t keys_len;
	/* S-07.055-S-07.057: the USB current limit */
	uint32_t applied_ma, target_ma;
	bool usb_configured;
	uint64_t last_usb_us;
	uint8_t led_rgb[3];	/* S-07.041: the last charge LED value sent */
	/* called once at a power-off, before the reports (the panel's frames stop) */
	void (*on_poweroff)(void *ctx);
	void *ctx;
};

void fr_psoc_init(struct fr_psoc *p, const struct fr_bytes *io,
		  const struct fr_clock *clock);

/* S-06.001-S-06.006: the bytes a frame is sent as (COBS, one delimiter),
 * into out[FR_S06_MAX_WIRE]; their count, or 0 for a payload over 249. */
size_t fr_s06_encode(uint8_t route, uint8_t seq, uint8_t id,
		     const uint8_t *pl, size_t n, uint8_t *out);
/* S-06.003's check byte. */
uint8_t fr_s06_crc8(const uint8_t *p, size_t n);

/* A plain message, route 0x14 (S-06.011); none after a power-off. */
void fr_psoc_send(struct fr_psoc *p, uint8_t id, const uint8_t *pl, size_t n);
/* A request (S-06.030) and its reply within timeout_us: 1 with a reply,
 * 0 without, FR_PSOC_OFF after a power-off. While a waited 0x30 or 0x36
 * of this start is still unanswered, a frame to slot 0 with its reply's id
 * (0x31, 0x32; 0x37) is that late reply, and no other request's
 * (S-07.012). Messages meanwhile are handled once the wait is over. */
int fr_psoc_request(struct fr_psoc *p, uint8_t id, const uint8_t *pl, size_t n,
		    uint32_t timeout_us, struct fr_frame *reply);
/* Takes what has arrived and handles it (S-07's table 2, S-07.076's ping). */
int fr_psoc_poll(struct fr_psoc *p);
/* Takes what has arrived and handles nothing: for fr_serve_hook, inside
 * the long loops, so that a frame the front does not serve is dropped as
 * it comes and a power message is kept. Sends nothing. */
void fr_psoc_take(struct fr_psoc *p);
/* Takes what has arrived and handles its power messages alone (S-07.050-
 * S-07.053); greetings and pings wait. */
int fr_psoc_poll_power(struct fr_psoc *p);
/* Polls for us microseconds. */
int fr_psoc_sleep(struct fr_psoc *p, uint32_t us);

/* S-07.020: up to ten greetings 10 ms apart: 1 once greeted, 0 if not. */
int fr_psoc_greet(struct fr_psoc *p);
/* S-07.021 after a greeting sequence that was not answered: the exit
 * packet as raw bytes, then a wait of at most wait_us that ends as soon as
 * the PSoC greets (S-07.024): 1 greeted, 0 not. */
int fr_psoc_greet_wait(struct fr_psoc *p, uint32_t wait_us);
/* S-07.022: 0x30 {} and 0x36 {} as plain messages. */
void fr_psoc_power_queries(struct fr_psoc *p);
/* S-07.054's first requests: 0x36, then 0x30, 100 ms each. */
int fr_psoc_power_requests(struct fr_psoc *p);
/* S-07.054's charge wait, while the state is FR_POWER_CHARGE. */
int fr_psoc_charge_wait(struct fr_psoc *p);
/* 0x51 with one of the FR_REPORT_* payloads. */
void fr_psoc_report(struct fr_psoc *p, unsigned which);
/* S-07.025: 0x43 {01 01}; the link is started. */
void fr_psoc_subscribe(struct fr_psoc *p);
/* S-07.032: 0x41, 100 ms; keys_replied and keys[] say what came. */
int fr_psoc_key_status(struct fr_psoc *p);
/* S-04.065: 0x97 {DE AD BE EF}, then the bytes out (at most 20 ms). */
void fr_psoc_reset(struct fr_psoc *p);
/* One 0x36 request, 100 ms: a reply is in batt[] (S-07.053). */
int fr_psoc_battery(struct fr_psoc *p);
/* S-07.051 during recovery's in-place write (S-02.084): while held, a
 * power-off message is kept and nothing of its sequence is sent; a later
 * report of state FR_POWER_ON drops it. Releasing the hold runs a kept one
 * and returns FR_PSOC_OFF. */
int fr_psoc_hold(struct fr_psoc *p, bool on);
/* Waits until every byte written is on the wire, at most us. */
void fr_psoc_drain(struct fr_psoc *p, uint32_t us);
/* S-07.055-S-07.057: the device configured with ma, or not configured
 * (SET_CONFIGURATION(0), a bus reset, detached: 100 mA);
 * fr_psoc_service() steps the limit towards it, on each pass of a loop. */
void fr_psoc_usb(struct fr_psoc *p, bool configured, uint32_t ma);
int fr_psoc_service(struct fr_psoc *p);

/* S-07.032's reading of the reply (NULL: no reply, held, S-16.008). */
bool fr_keys_hold(const uint8_t *pl, size_t n);

/* S-07.033's reading of a key-status reply: the one key that is down, with
 * no other key but precomp, as its number (S-07.030); or one of these. No
 * reply (NULL) is unread: an unread key never acts (S-02.080). */
#define FR_KEYS_BYTES  9u	/* the 72 keys' bytes of a reply (S-07.030) */
#define FR_KEYS_UNREAD 0xFFu
#define FR_KEYS_NONE   0xFEu	/* a reply, and no key down */
#define FR_KEYS_MANY   0xFDu	/* more than one key down */
unsigned fr_keys_one(const uint8_t *pl, size_t n);

/* The keys the front reads (S-15.011, S-15.150). */
#define FR_KEY_1 0x06u	/* M1 */
#define FR_KEY_2 0x07u	/* M2 */
#define FR_KEY_3 0x08u	/* M3 */
#define FR_KEY_4 0x09u	/* M4: Power + M4 */
#define FR_KEY_8 0x15u	/* sound 8: Back */

#endif /* FR_PSOC_H */
