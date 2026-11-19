/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The PSoC link (fr_psoc.h): S-06's framing and S-07's exchanges, trimmed
 * from the full bootloader's s06 and s07 modules to what the front runs.
 */
#include "fr_psoc.h"

#define ADDR_MASTER 1u
#define ADDR_PSOC   4u
#define ADDR_SLOT0  8u
#define ROUTE_PLAIN   (ADDR_MASTER << 4 | ADDR_PSOC)	/* 0x14 */
#define ROUTE_REQUEST (ADDR_SLOT0 << 4 | ADDR_PSOC)	/* 0x84 */
#define HEADER 4u

#define STEP_US       50u		/* a wait polls this often */
#define WRITE_US      10000u		/* a frame's bytes out: 2.6 ms at 1 Mbaud */
#define REQUEST_US    100000u		/* S-07.011 */
#define GREET_TRIES   10u		/* S-07.020 */
/* S-07.020: >= 10 ms from the last byte, ten within 200 ms; a greeting is
 * 9 bytes, 90 us on the line. */
#define GREET_GAP_US  10200u
#define ROUND_US      10000u		/* a charge-wait round */
#define QUERY_EVERY_US 1000000u		/* S-07.054: "more than 1 s" */
#define LIMIT_STEP_US 5000u
#define RESET_DRAIN_US 20000u

#define ID_PING   0x01u
#define ID_PONG   0x02u
#define ID_GREET  0x05u
#define ID_GREET_OK 0x06u
#define ID_POWER_QUERY 0x30u
#define ID_POWER_31 0x31u
#define ID_POWER_32 0x32u
#define ID_VBUS   0x33u
#define ID_LIMIT  0x34u
#define ID_POWER_35 0x35u
#define ID_BATTERY_QUERY 0x36u
#define ID_BATTERY 0x37u
#define ID_LED    0x40u
#define ID_KEY_STATUS 0x41u
#define ID_SUBSCRIBE 0x43u
#define ID_REPORT 0x51u
#define ID_RESET  0x97u

/* S-07.021 (S-07.082): the PSoC bootloader's exit command. */
static const uint8_t exit_packet[7] = { 0x01, 0x3B, 0x00, 0x00, 0xC4, 0xFF, 0x17 };

/* ---- S-06 frames ---- */

/* S-06.003: CRC-8, polynomial 0x4D reflected (0xB2), init 0, every byte
 * XORed with 0xA5, no final XOR ("123456789": 0xD8). */
uint8_t fr_s06_crc8(const uint8_t *p, size_t n)
{
	unsigned c = 0;

	while (n--) {
		c ^= (unsigned)*p++ ^ 0xA5u;
		for (unsigned k = 0; k < 8u; k++)
			c = (c & 1u) ? (c >> 1) ^ 0xB2u : c >> 1;
	}
	return (uint8_t)c;
}

/* S-06.004: COBS, no empty final group after an FF group. */
static size_t cobs(const uint8_t *in, size_t n, uint8_t *out)
{
	size_t code_at = 0, o = 1;
	uint8_t code = 1;

	for (size_t i = 0; i < n; i++) {
		if (in[i] == 0) {
			out[code_at] = code;
			code_at = o++;
			code = 1;
			continue;
		}
		out[o++] = in[i];
		code++;
		if (code == 0xFFu) {
			out[code_at] = code;
			if (i + 1u == n)
				return o;
			code_at = o++;
			code = 1;
		}
	}
	out[code_at] = code;
	return o;
}

/* S-06.001-S-06.006 */
size_t fr_s06_encode(uint8_t route, uint8_t seq, uint8_t id,
		     const uint8_t *pl, size_t n, uint8_t *out)
{
	uint8_t raw[FR_S06_MAX_RAW];
	size_t e;

	if (n > FR_S06_MAX_PAYLOAD)
		return 0;
	raw[0] = route;
	raw[1] = seq;
	raw[2] = id;
	raw[3] = (uint8_t)n;
	if (n)
		fr_copy(raw + HEADER, pl, n);
	raw[HEADER + n] = fr_s06_crc8(raw, HEADER + n);
	e = cobs(raw, HEADER + n + 1u, out);
	out[e] = 0x00;
	return e + 1u;
}

/* ---- time and bytes ---- */

static uint64_t now(const struct fr_psoc *p)
{
	return p->clock->now_us(p->clock->ctx);
}

static void step(const struct fr_psoc *p, uint64_t end)
{
	uint64_t t = now(p);
	uint64_t left = end > t ? end - t : 0u;

	p->clock->delay_us(p->clock->ctx,
			   (uint32_t)(left < STEP_US ? (left ? left : 1u) : STEP_US));
}

static void take_bytes(struct fr_psoc *p);

/* The whole of d out, within WRITE_US; reading meanwhile. */
static void put(struct fr_psoc *p, const uint8_t *d, size_t n)
{
	const uint64_t end = now(p) + WRITE_US;

	for (;;) {
		size_t k = p->io->write(p->io->ctx, d, n);

		d += k;
		n -= k;
		if (n == 0 || now(p) >= end)
			return;
		take_bytes(p);
		step(p, end);
	}
}

/* S-06.002, S-06.020: a frame not sent uses no number. */
static void send_route(struct fr_psoc *p, uint8_t route, uint8_t id,
		       const uint8_t *pl, size_t n)
{
	uint8_t wire[FR_S06_MAX_WIRE];
	size_t w;

	if (p->off)
		return;			/* S-07.051 step 3 */
	w = fr_s06_encode(route, p->seq, id, pl, n, wire);
	if (w == 0)
		return;
	p->seq = (uint8_t)(p->seq + 1u);
	put(p, wire, w);
}

void fr_psoc_send(struct fr_psoc *p, uint8_t id, const uint8_t *pl, size_t n)
{
	send_route(p, ROUTE_PLAIN, id, pl, n);
}

/* ---- S-06 receiving (S-06.040-S-06.049, S-06.032-S-06.034) ---- */

#define RX_IDLE   0u	/* where a frame would begin */
#define RX_DECODE 1u	/* a frame for this node */
#define RX_SKIP   2u	/* a frame for another node: no other link here */

/* S-06.013 */
static bool for_node(uint8_t dst)
{
	dst &= 0x0Fu;
	return dst == ADDR_MASTER || (dst & 0x08u) != 0u;
}

/* S-07.012: 0x31 and 0x32 answer the waited 0x30, and 0x37 the 0x36.
 * While one of those went unanswered in this start, a frame to slot 0 with
 * its reply's id may be that reply come late, and completes no other
 * request. Otherwise every id completes the request pending, as the reply
 * ids of the others are not known. */
static bool expected(const struct fr_psoc *p, uint8_t id)
{
	switch (id) {
	case ID_POWER_31:
	case ID_POWER_32:
		return p->pending_id == ID_POWER_QUERY || p->late_30 == 0u;
	case ID_BATTERY:
		return p->pending_id == ID_BATTERY_QUERY || p->late_36 == 0u;
	default:
		return true;
	}
}

/* A frame to slot 0 that no request took: the late reply it may be. */
static void late_came(struct fr_psoc *p, uint8_t id)
{
	if ((id == ID_POWER_31 || id == ID_POWER_32) && p->late_30 != 0u)
		p->late_30--;
	else if (id == ID_BATTERY && p->late_36 != 0u)
		p->late_36--;
}

static void count_up(uint8_t *n)
{
	if (*n != 0xFFu)
		(*n)++;
}

/* A power state kept for handle_power(). The same state twice in a row
 * acts as once (S-07.050) but for the charge LED, which each message sets
 * (S-07.042): a repeat is counted. Past FR_PSOC_STATES changes, which no
 * switch makes between two polls, the newest takes the last place. */
static void keep_state(struct fr_psoc *p, uint8_t s)
{
	if (p->n_states != 0u && p->states[p->n_states - 1u] == s) {
		count_up(&p->repeats[p->n_states - 1u]);
		return;
	}
	if (p->n_states == FR_PSOC_STATES)
		p->n_states--;
	p->states[p->n_states] = s;
	p->repeats[p->n_states++] = 0;
}

static void deliver(struct fr_psoc *p)
{
	const uint8_t *raw = p->rx;
	struct fr_frame *f;

	/* S-06.032: node 8 completes the request pending in slot 0 when it
	 * may be its reply; otherwise, and for 9-15, an ordinary message
	 * (S-06.033), kept only when handle() acts on it */
	if ((raw[0] & 0x0Fu) == ADDR_SLOT0 && p->pending && expected(p, raw[2])) {
		f = &p->reply;
		p->pending = false;
		p->replied = true;
	} else {
		if ((raw[0] & 0x0Fu) == ADDR_SLOT0)
			late_came(p, raw[2]);
		switch (raw[2]) {
		case ID_POWER_31:
		case ID_POWER_32:
			if (raw[3] != 0u)
				keep_state(p, raw[HEADER]);
			else
				count_up(&p->led_state);	/* S-07.079 */
			return;
		case ID_BATTERY:		/* S-07.053 */
			p->batt_len = (uint8_t)(raw[3] < sizeof p->batt ? raw[3] : sizeof p->batt);
			fr_copy(p->batt, raw + HEADER, p->batt_len);
			count_up(&p->led_any);
			return;
		case 0x00:			/* S-07.052 */
		case ID_POWER_35:
			count_up(&p->led_any);
			return;
		case ID_GREET:
		case ID_GREET_OK:
		case ID_PING:
			if (p->in_count >= FR_PSOC_INBOX)
				return;		/* dropped: the inbox is full */
			f = &p->inbox[((unsigned)p->in_head + p->in_count) % FR_PSOC_INBOX];
			p->in_count++;
			break;
		default:
			return;			/* not served here (handle()) */
		}
	}
	f->route = raw[0];
	f->seq = raw[1];
	f->id = raw[2];
	f->len = raw[3];
	fr_copy(f->payload, raw + HEADER, raw[3]);
}

static void rx_byte(struct fr_psoc *p, uint8_t b)
{
	uint8_t v;

	if (p->rx_state == RX_SKIP) {
		/* S-06.046, S-06.048: to the delimiter, at most 256 bytes */
		p->rx_n++;
		if (b == 0 || p->rx_n >= FR_S06_MAX_WIRE)
			p->rx_state = RX_IDLE;
		return;
	}
	if (b == 0) {
		/* S-06.040: ignored where a frame would begin; S-06.044 */
		p->rx_state = RX_IDLE;
		return;
	}
	if (p->rx_state == RX_IDLE) {
		p->rx_state = RX_DECODE;
		p->rx_left = (uint8_t)(b - 1u);
		p->rx_n = 0;
		return;
	}
	/* S-06.004: a data byte, or a code byte standing for a 00 */
	if (p->rx_left) {
		v = b;
		p->rx_left--;
	} else {
		v = 0;
		p->rx_left = (uint8_t)(b - 1u);
	}
	/* S-06.047: decided on the route byte */
	if (p->rx_n == 0 && !for_node(v)) {
		p->rx_state = RX_SKIP;
		p->rx_n = 2;
		return;
	}
	p->rx[p->rx_n++] = v;
	/* S-06.041: complete at len + 5 bytes */
	if (p->rx_n >= HEADER && p->rx_n == (uint16_t)(p->rx[3] + 5u)) {
		p->rx_state = RX_IDLE;
		/* S-06.042 */
		if (fr_s06_crc8(p->rx, (size_t)p->rx_n - 1u) == p->rx[p->rx_n - 1u])
			deliver(p);
		return;
	}
	if (p->rx_n >= FR_S06_MAX_RAW)	/* S-06.043 */
		p->rx_state = RX_IDLE;
}

/* What has arrived, at most 16 reads (so the loop ends on a busy line). */
static void take_bytes(struct fr_psoc *p)
{
	uint8_t buf[32];

	for (unsigned r = 0; r < 16u; r++) {
		size_t got = p->io->read(p->io->ctx, buf, sizeof buf);

		if (got == 0)
			return;
		for (size_t i = 0; i < got; i++)
			rx_byte(p, buf[i]);
	}
}

/* ---- S-07: table 2 ---- */

static void led_rgb(struct fr_psoc *p, const uint8_t rgb[3], bool forced)
{
	const uint8_t m[4] = { 0x03, rgb[0], rgb[1], rgb[2] };

	if (!forced) {
		if (fr_same(p->led_rgb, rgb, 3))
			return;		/* S-07.041 */
		fr_copy(p->led_rgb, rgb, 3);
	}
	/* S-07.041: a forced message is always sent and not remembered */
	fr_psoc_send(p, ID_LED, m, 4);
}

/* S-07.042 (s07_charge_led): from the state and the last charge */
static void charge_led(struct fr_psoc *p)
{
	static const uint8_t red[3] = { 0xC8, 0, 0 }, green[3] = { 0, 0xC8, 0 },
			     dark[3] = { 0, 0, 0 };
	/* S-07.053: 0 before a report and after "removed"; S-07.079 */
	const uint8_t charge = (p->batt_len > 1u && p->batt[0] != 0u) ? p->batt[1] : 0u;
	const uint8_t *v = dark;
	bool forced = false;

	if (p->state_known && p->state == FR_POWER_ON) {
		v = charge < 15u ? red : green;
	} else if (p->state_known && p->state == FR_POWER_CHARGE) {
		v = charge > 90u ? green : red;
		forced = true;
	}
	led_rgb(p, v, forced);
}

/* S-07.051's sequence for state s. */
static int power_off(struct fr_psoc *p, uint8_t s)
{
	static const uint8_t shutdown[3] = { 0x00, 0x00, 0x02 }, all_off[1] = { 0x01 };

	if (p->on_poweroff)
		p->on_poweroff(p->ctx);
	/* step 1: S-07.072's shutdown reports; step 2: the echo */
	fr_psoc_send(p, ID_REPORT, shutdown, 3);
	fr_psoc_send(p, ID_LED, all_off, 1);
	fr_psoc_send(p, ID_POWER_32, &s, 1);
	p->off = true;			/* step 3: nothing more, no reset */
	return FR_PSOC_OFF;
}

/* S-07.050, S-07.051 */
static int power_state(struct fr_psoc *p, uint8_t s)
{
	const bool from_on = p->state_known && p->state == FR_POWER_ON;

	p->state = s;
	p->state_known = true;
	/* S-07.051: the switch is on again: a kept power-off is dropped. */
	if (s == FR_POWER_ON)
		p->off_due = false;
	if (!(s == FR_POWER_OFF || (s == FR_POWER_CHARGE && from_on)))
		return FR_PSOC_OK;
	if (p->hold_off) {
		/* S-02.084: kept until the in-place write has ended. */
		p->off_due = true;
		p->off_state = s;
		return FR_PSOC_OK;
	}
	return power_off(p, s);
}

int fr_psoc_hold(struct fr_psoc *p, bool on)
{
	if (p->off)
		return FR_PSOC_OFF;
	p->hold_off = on;
	if (!on && p->off_due) {
		p->off_due = false;
		return power_off(p, p->off_state);
	}
	return FR_PSOC_OK;
}

static void report_held_and_subscribe(struct fr_psoc *p)
{
	static const uint8_t sub[2] = { 0x01, 0x01 };

	fr_psoc_report(p, FR_REPORT_HELD);	/* S-04.064 */
	fr_psoc_send(p, ID_SUBSCRIBE, sub, 2);	/* S-07.025 */
}

/* S-07.024: a greeting from the PSoC. One that comes while a greeting
 * sequence runs greets the link and is answered 0x06 {40} alone. */
static int psoc_greeting(struct fr_psoc *p)
{
	static const uint8_t ok[1] = { 0x40 };

	if (p->started && !p->greeting) {
		int rc = fr_psoc_greet(p);

		if (rc == FR_PSOC_OFF)
			return rc;
		report_held_and_subscribe(p);
	}
	fr_psoc_send(p, ID_GREET_OK, ok, 1);
	p->greeted = true;		/* S-07.020 */
	return FR_PSOC_OK;
}

static int handle(struct fr_psoc *p, const struct fr_frame *f)
{
	switch (f->id) {
	case ID_POWER_31:
	case ID_POWER_32:
		/* S-07.079: an empty one records nothing */
		if (f->len && power_state(p, f->payload[0]) == FR_PSOC_OFF)
			return FR_PSOC_OFF;
		if (!p->off_due)
			charge_led(p);
		break;
	case 0x00:			/* S-07.052 */
	case ID_POWER_35:
		charge_led(p);
		break;
	case ID_BATTERY:		/* S-07.053 */
		p->batt_len = (uint8_t)(f->len < sizeof p->batt ? f->len : sizeof p->batt);
		fr_copy(p->batt, f->payload, p->batt_len);
		charge_led(p);
		break;
	case ID_GREET:
		return psoc_greeting(p);
	case ID_GREET_OK:		/* S-07.020 */
		if (f->len && f->payload[0] == 0x40u)
			p->greeted = true;
		break;
	case ID_PING:
		/* S-07.076: 0x02 with the same payload, route swapped */
		if (!p->quiet)
			send_route(p, (uint8_t)(f->route << 4 | f->route >> 4),
				   ID_PONG, f->payload, f->len);
		break;
	default:
		/* 0x42 (the front reads no key events), 0x45, 0x4D and S-10c's
		 * common commands: not served here */
		break;
	}
	return FR_PSOC_OK;
}

/* The power messages kept, as handle() takes them, each taken off before
 * it is handled: a message sent meanwhile reads the link, and what that
 * brings waits behind. */
static int handle_power(struct fr_psoc *p)
{
	uint8_t led_state, led_any;

	while (p->n_states != 0u && !p->off) {
		const uint8_t s = p->states[0];
		unsigned n = 1u + p->repeats[0];

		p->n_states--;
		for (unsigned i = 0; i < p->n_states; i++) {
			p->states[i] = p->states[i + 1u];
			p->repeats[i] = p->repeats[i + 1u];
		}
		if (power_state(p, s) == FR_PSOC_OFF)
			return FR_PSOC_OFF;
		while (n-- && !p->off_due && !p->off)
			charge_led(p);
	}
	led_state = p->led_state;
	led_any = p->led_any;
	p->led_state = p->led_any = 0;
	while (led_state-- && !p->off && !p->off_due)
		charge_led(p);
	while (led_any-- && !p->off)
		charge_led(p);
	return p->off ? FR_PSOC_OFF : FR_PSOC_OK;
}

static int handle_inbox(struct fr_psoc *p)
{
	struct fr_frame f;

	if (handle_power(p) == FR_PSOC_OFF)
		return FR_PSOC_OFF;
	while (p->in_count && !p->off) {
		f = p->inbox[p->in_head];
		p->in_head = (uint8_t)((p->in_head + 1u) % FR_PSOC_INBOX);
		p->in_count--;
		if (handle(p, &f) == FR_PSOC_OFF)
			return FR_PSOC_OFF;
	}
	return p->off ? FR_PSOC_OFF : FR_PSOC_OK;
}

void fr_psoc_take(struct fr_psoc *p)
{
	if (!p->off)
		take_bytes(p);
}

int fr_psoc_poll(struct fr_psoc *p)
{
	if (p->off)
		return FR_PSOC_OFF;
	take_bytes(p);
	return handle_inbox(p);
}

/* Polls until end, or until the link is greeted when greet is set. */
static int wait_until(struct fr_psoc *p, uint64_t end, bool greet)
{
	for (;;) {
		if (fr_psoc_poll(p) == FR_PSOC_OFF)
			return FR_PSOC_OFF;
		if ((greet && p->greeted) || now(p) >= end)
			return FR_PSOC_OK;
		step(p, end);
	}
}

int fr_psoc_sleep(struct fr_psoc *p, uint32_t us)
{
	return wait_until(p, now(p) + us, false);
}

void fr_psoc_init(struct fr_psoc *p, const struct fr_bytes *io,
		  const struct fr_clock *clock)
{
	fr_fill(p, 0, sizeof *p);
	p->io = io;
	p->clock = clock;	/* S-06.020: seq 0; S-07.041: the LED dark */
}

/* S-06.030-S-06.035, S-07.011: one at a time, never resent. */
int fr_psoc_request(struct fr_psoc *p, uint8_t id, const uint8_t *pl, size_t n,
		    uint32_t timeout_us, struct fr_frame *reply)
{
	uint64_t end;
	int got;

	if (p->off)
		return FR_PSOC_OFF;
	p->pending = true;
	p->pending_id = id;
	p->replied = false;
	send_route(p, ROUTE_REQUEST, id, pl, n);
	end = now(p) + timeout_us;
	for (;;) {
		take_bytes(p);
		/* a reply read by the deadline is in time */
		if (p->replied || now(p) >= end)
			break;
		step(p, end);
	}
	p->pending = false;
	got = p->replied ? 1 : 0;
	/* S-07.012: its reply may still come, late */
	if (!got && id == ID_POWER_QUERY && p->late_30 != 0xFFu)
		p->late_30++;
	if (!got && id == ID_BATTERY_QUERY && p->late_36 != 0xFFu)
		p->late_36++;
	if (got && reply)
		fr_copy(reply, &p->reply, sizeof *reply);
	if (handle_inbox(p) == FR_PSOC_OFF)
		return FR_PSOC_OFF;
	return got;
}

/* ---- greeting and start-up (S-07.020-S-07.025) ---- */

int fr_psoc_greet(struct fr_psoc *p)
{
	static const uint8_t hello[2] = { 0x01, 0x04 };

	if (p->off)
		return FR_PSOC_OFF;
	p->greeted = false;
	p->greeting = true;
	for (unsigned i = 0; i < GREET_TRIES && !p->greeted; i++) {
		fr_psoc_send(p, ID_GREET, hello, 2);
		if (wait_until(p, now(p) + GREET_GAP_US, true) == FR_PSOC_OFF) {
			p->greeting = false;
			return FR_PSOC_OFF;
		}
	}
	p->greeting = false;
	return p->greeted ? 1 : 0;
}

/* S-07.021, with the wait ended by the PSoC's greeting (S-07.024): a PSoC
 * that left its bootloader greets as its application starts. Nothing is
 * sent during it but the greeting's answer. */
int fr_psoc_greet_wait(struct fr_psoc *p, uint32_t wait_us)
{
	int rc;

	if (p->off)
		return FR_PSOC_OFF;
	put(p, exit_packet, sizeof exit_packet);	/* raw, framed mode kept */
	p->quiet = true;
	rc = wait_until(p, now(p) + wait_us, true);
	p->quiet = false;
	if (rc == FR_PSOC_OFF)
		return rc;
	return p->greeted ? 1 : 0;
}

void fr_psoc_power_queries(struct fr_psoc *p)
{
	fr_psoc_send(p, ID_POWER_QUERY, NULL, 0);
	fr_psoc_send(p, ID_BATTERY_QUERY, NULL, 0);
}

/* S-07.012: a waited 0x30/0x36 reply, handled as a power message. */
static int waited(struct fr_psoc *p, int rc, const struct fr_frame *r)
{
	if (rc != 1)
		return rc == FR_PSOC_OFF ? FR_PSOC_OFF : FR_PSOC_OK;
	if (r->id == ID_POWER_31 || r->id == ID_POWER_32 || r->id == ID_BATTERY)
		return handle(p, r);
	charge_led(p);
	return FR_PSOC_OK;
}

int fr_psoc_power_requests(struct fr_psoc *p)
{
	struct fr_frame r;
	int rc;

	rc = fr_psoc_request(p, ID_BATTERY_QUERY, NULL, 0, REQUEST_US, &r);
	if (waited(p, rc, &r) == FR_PSOC_OFF)
		return FR_PSOC_OFF;
	rc = fr_psoc_request(p, ID_POWER_QUERY, NULL, 0, REQUEST_US, &r);
	return waited(p, rc, &r);
}

int fr_psoc_battery(struct fr_psoc *p)
{
	struct fr_frame r;
	int rc = fr_psoc_request(p, ID_BATTERY_QUERY, NULL, 0, REQUEST_US, &r);

	return waited(p, rc, &r);
}

/* S-07.055 (s07_vbus_update): no reply counts as {00 00} (S-07.058). */
static int vbus(struct fr_psoc *p)
{
	struct fr_frame r;
	uint8_t st = 0, n = 0;
	uint32_t t = p->target_ma / 10u;
	int rc = fr_psoc_request(p, ID_VBUS, NULL, 0, REQUEST_US, &r);

	if (rc == FR_PSOC_OFF)
		return rc;
	if (rc == 1) {			/* S-07.079: missing bytes read 00 */
		st = r.len > 0u ? r.payload[0] : 0u;
		n = r.len > 1u ? r.payload[1] : 0u;
	}
	if (st != 0u)
		return FR_PSOC_OK;
	if (n < t) {
		const uint8_t lim[2] = { 0x00, (uint8_t)(t > 0xFFu ? 0xFFu : t) };

		fr_psoc_send(p, ID_LIMIT, lim, 2);
		p->applied_ma = (uint32_t)lim[1] * 10u;
	} else {
		p->applied_ma = (uint32_t)n * 10u;
	}
	return FR_PSOC_OK;
}

/* S-07.056 */
static int limit_step(struct fr_psoc *p)
{
	if (p->applied_ma >= p->target_ma)
		return FR_PSOC_OK;
	if (vbus(p) == FR_PSOC_OFF)
		return FR_PSOC_OFF;
	return fr_psoc_sleep(p, LIMIT_STEP_US);
}

/*
 * S-07.054's charge wait: the switch is off on external power, so the unit
 * only charges, dark and with no USB function; 500 mA is asked for.
 * It ends when the switch goes on, or in the power-off a 0x14 starts. Not
 * bounded: the PSoC holds the master in this state until either.
 */
int fr_psoc_charge_wait(struct fr_psoc *p)
{
	uint64_t last = 0;
	bool first = true;

	if (p->off)
		return FR_PSOC_OFF;
	/* S-07.054, S-07.057: a fixed 500 mA, which a standard port supplies;
	 * the unit stays off the bus, so no charger rule applies. */
	p->target_ma = 500u;
	while (p->state_known && p->state == FR_POWER_CHARGE) {
		if (first || now(p) > last + QUERY_EVERY_US) {
			first = false;
			last = now(p);
			fr_psoc_send(p, ID_BATTERY_QUERY, NULL, 0);
			fr_psoc_send(p, ID_POWER_QUERY, NULL, 0);
			if (vbus(p) == FR_PSOC_OFF)
				return FR_PSOC_OFF;
		}
		if (p->applied_ma > 100u)
			charge_led(p);
		if (fr_psoc_sleep(p, ROUND_US) == FR_PSOC_OFF ||
		    limit_step(p) == FR_PSOC_OFF)
			return FR_PSOC_OFF;
	}
	p->target_ma = 0;
	p->applied_ma = 0;
	return FR_PSOC_OK;
}

/* S-07.065 */
void fr_psoc_report(struct fr_psoc *p, unsigned which)
{
	static const uint8_t pl[4][3] = {
		{ 0x00, 0x00, 0x00 }, { 0x3C, 0xFF, 0x02 },
		{ 0x3C, 0xFF, 0x32 }, { 0x00, 0x00, 0x02 },
	};

	/* the hand-over's {3C FF 32} is main's, not the front's */
	if (which == FR_REPORT_START || which == FR_REPORT_HELD ||
	    which == FR_REPORT_SHUTDOWN)
		fr_psoc_send(p, ID_REPORT, pl[which], 3);
}

void fr_psoc_subscribe(struct fr_psoc *p)
{
	static const uint8_t sub[2] = { 0x01, 0x01 };

	fr_psoc_send(p, ID_SUBSCRIBE, sub, 2);
	p->started = true;
}

/* S-07.032: the payload is kept for the reading (fr_keys_hold, _one) */
int fr_psoc_key_status(struct fr_psoc *p)
{
	struct fr_frame r;
	int rc = fr_psoc_request(p, ID_KEY_STATUS, NULL, 0, REQUEST_US, &r);

	if (rc == FR_PSOC_OFF)
		return rc;
	p->keys_replied = rc == 1;
	p->keys_len = 0;
	if (rc == 1) {
		p->keys_len = (uint8_t)(r.len < sizeof p->keys ? r.len : sizeof p->keys);
		fr_copy(p->keys, r.payload, p->keys_len);
	}
	return FR_PSOC_OK;
}

void fr_psoc_drain(struct fr_psoc *p, uint32_t us)
{
	const uint64_t end = now(p) + us;

	while (!p->io->idle(p->io->ctx) && now(p) < end) {
		take_bytes(p);
		step(p, end);
	}
}

/* S-04.065, S-07.070 */
void fr_psoc_reset(struct fr_psoc *p)
{
	static const uint8_t v[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

	fr_psoc_send(p, ID_RESET, v, 4);
	fr_psoc_drain(p, RESET_DRAIN_US);
}

/* S-07.057 (s07_usb_configured, s07_usb_reset) */
void fr_psoc_usb(struct fr_psoc *p, bool configured, uint32_t ma)
{
	p->usb_configured = configured;
	p->target_ma = configured ? ma : 100u;
	p->last_usb_us = now(p);
}

int fr_psoc_service(struct fr_psoc *p)
{
	if (fr_psoc_poll(p) == FR_PSOC_OFF)
		return FR_PSOC_OFF;
	return limit_step(p);
}

/* ---- keys (S-07.032, S-07.033) ---- */

/* S-07.032, S-16.008: hold is byte 3 bit 0 or byte 1 bit 5; no reply,
 * held. S-07.079: missing bytes read 00. */
bool fr_keys_hold(const uint8_t *pl, size_t n)
{
	uint8_t b[4] = { 0, 0, 0, 0 };

	if (!pl)
		return true;
	fr_copy(b, pl, n < 4u ? n : 4u);
	return (b[3] & 0x01u) != 0u || (b[1] & 0x20u) != 0u;
}

/* S-07.033's reading of the reply, key k as bit k & 7 of byte k >> 3, over
 * the 72 keys 0x00-0x47 but precomp (byte 2 bit 7): the one key down, none,
 * or more than one. A byte the reply does not carry reads 00 (S-07.079);
 * what follows byte 8 is not read. */
unsigned fr_keys_one(const uint8_t *pl, size_t n)
{
	uint8_t b[FR_KEYS_BYTES] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	unsigned key = FR_KEYS_NONE;

	if (!pl)
		return FR_KEYS_UNREAD;
	fr_copy(b, pl, n < FR_KEYS_BYTES ? n : FR_KEYS_BYTES);
	b[2] &= 0x7Fu;
	for (unsigned k = 0; k < 8u * FR_KEYS_BYTES; k++) {
		if ((((unsigned)b[k >> 3] >> (k & 7u)) & 1u) == 0u)
			continue;
		if (key != FR_KEYS_NONE)
			return FR_KEYS_MANY;
		key = k;
	}
	return key;
}
