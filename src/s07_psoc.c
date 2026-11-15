/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07: the PSoC link as the master uses it.
 */
#include "waffle/s07_psoc.h"
#include "waffle/bytes.h"
#include "waffle/testos.h"

#define REQUEST_US S06_MS(100)		/* S-07.011 */
#define GREET_TRIES 10			/* S-07.020 */
#define GREET_GAP_US S06_MS(10)		/* >= 10 ms, ten within 200 ms */
#define GREET_FAIL_WAIT_US S06_MS(1500)	/* S-07.021 */
#define ROUND_US S06_MS(10)		/* a battery-wait round */
#define QUERY_EVERY_US S06_MS(1000)	/* S-07.054 */
#define CHARGER_US S06_MS(3000)		/* S-07.054, S-16.017 */
#define LIMIT_STEP_US S06_MS(5)
#define ENTER_TRIES 4			/* S-07.080 */
#define ENTER_WAIT_US S06_MS(250)
#define PROBE_US S06_MS(5)		/* S-07.081, judged after 5 ms ... */
#define PROBE_MARGIN_US S06_MS(2)	/* ... and a little line time */
#define RUNS 3				/* S-07.084 */
#define RETURN_EXIT_FROM_US S06_MS(1000)	/* S-07.085 */
#define RETURN_GIVE_UP_US S06_MS(2000)
#define RETURN_GAP_US S06_MS(10)	/* exit packets <= 15 ms apart */
#define RAW_WRITE_US S06_MS(1000)
/* The test OS's cap on a wait that need not end (waffle/testos.h). */
#define TESTOS_WAIT_CAP_US S06_MS(5000)

#define ID_PING 0x01u
#define ID_PONG 0x02u
#define ID_GREET 0x05u
#define ID_GREET_OK 0x06u
#define ID_INFO 0x09u
#define ID_POWER_QUERY 0x30u
#define ID_POWER_31 0x31u
#define ID_POWER_32 0x32u
#define ID_VBUS 0x33u
#define ID_LIMIT 0x34u
#define ID_POWER_35 0x35u
#define ID_BATTERY_QUERY 0x36u
#define ID_BATTERY 0x37u
#define ID_LED 0x40u
#define ID_KEY_STATUS 0x41u
#define ID_BITMAP 0x42u
#define ID_SUBSCRIBE 0x43u
#define ID_KEY_EVENT 0x45u
#define ID_VOLCAL 0x48u
#define ID_PORT 0x4Du
#define ID_REPORT 0x51u
#define ID_PMIC_PASS 0x70u
#define ID_PMIC_WRITE 0x71u
#define ID_PMIC_READ 0x73u
#define ID_RESET 0x97u
#define ID_BOOTLOADER 0xEEu

static void log_ev(struct s07_psoc *p, int ev, uint32_t a, uint32_t b)
{
	if (p->ops->log)
		p->ops->log(p->ops->ctx, ev, a, b);
}

static uint32_t now(const struct s07_psoc *p)
{
	return s06_now(p->node->clock);
}

/* A plain message (route 0x14). S-07.051: nothing after a power-off. */
static void send(struct s07_psoc *p, uint8_t id, const uint8_t *pl, size_t n)
{
	if (!p->off)
		(void)s06_send(p->node, p->link, 0, id, pl, n);
}

/* A request (S-07.011): 1 with a reply within 100 ms, else 0. */
static int request(struct s07_psoc *p, uint8_t id, const uint8_t *pl,
		   size_t n, struct s06_frame *reply)
{
	if (p->off)
		return 0;
	return s06_transact(p->node, p->link, id, pl, n, REQUEST_US, reply) == 1;
}

void s07_init(struct s07_psoc *p, struct s06_node *node, unsigned link,
	      const struct s07_ops *ops)
{
	wb_fill((uint8_t *)p, 0, sizeof *p);
	p->node = node;
	p->link = link;
	p->ops = ops;
	p->state = S07_POWER_NONE;
	p->led_bar = -1;		/* S-07.041: the first bar is sent */
	p->charge_led_on = true;
	p->kt_lone = S07_KEYS;
}

void s07_sleep(struct s07_psoc *p, uint32_t us)
{
	uint32_t end = now(p) + us;

	for (;;) {
		s06_node_poll(p->node);
		if (p->ops->service)
			p->ops->service(p->ops->ctx);
		if (s06_time_reached(now(p), end))
			return;
		s06_idle(p->node->clock, end);
	}
}

static bool raw_put(struct s07_psoc *p, const uint8_t *d, size_t n)
{
	uint32_t end = now(p) + RAW_WRITE_US;

	while (!s06_raw_write(p->node, p->link, d, n)) {
		if (s06_time_reached(now(p), end))
			return false;
		s06_idle(p->node->clock, now(p) + 100u);
	}
	return true;
}

/* ---- LEDs (S-07.040-S-07.045) ---- */

void s07_led_key(struct s07_psoc *p, uint8_t key, uint8_t a, uint8_t b)
{
	const uint8_t m[4] = { 0x00, key, a, b };

	if (key >= S07_KEYS)
		return;
	if (p->led_key[key][0] == a && p->led_key[key][1] == b)
		return;			/* S-07.041 */
	p->led_key[key][0] = a;
	p->led_key[key][1] = b;
	send(p, ID_LED, m, 4);
}

void s07_led_bar(struct s07_psoc *p, uint8_t n)
{
	const uint8_t m[2] = { 0x02, n };

	if (p->led_bar == n)
		return;			/* S-07.041 */
	p->led_bar = n;
	send(p, ID_LED, m, 2);
}

void s07_led_rgb(struct s07_psoc *p, const uint8_t rgb[3], bool forced)
{
	const uint8_t m[4] = { 0x03, rgb[0], rgb[1], rgb[2] };

	if (!forced) {
		if (wb_equal(p->led_rgb, rgb, 3))
			return;		/* S-07.041 */
		wb_copy(p->led_rgb, rgb, 3);
	}
	/* S-07.041: a forced message is always sent and not remembered */
	send(p, ID_LED, m, 4);
}

void s07_led_all_off(struct s07_psoc *p)
{
	static const uint8_t m[1] = { 0x01 };

	send(p, ID_LED, m, 1);
}

/* S-07.042, S-07.045: not while the key test runs */
void s07_update_charge_led(struct s07_psoc *p)
{
	uint8_t rgb[3];
	bool forced;

	if (!p->charge_led_on)
		return;
	s07_charge_led(p->state, (uint8_t)(p->batt.present ? p->batt.charge : 0), rgb,
		       &forced);
	s07_led_rgb(p, rgb, forced);
}

static void key_test_rgb(struct s07_psoc *p)
{
	uint8_t rgb[3] = { 0, 0, 0 };

	rgb[p->kt_presses % 3u] = 0xFF;	/* FF 00 00, 00 FF 00, 00 00 FF */
	s07_led_rgb(p, rgb, false);
}

static bool kt_tested(const struct s07_psoc *p, unsigned k)
{
	return s07_key_down(p->kt_tested, k);
}

/* S-07.045: a key shown FE 00, seen 00 FF, shown but not seen 00 00. Only a
 * press that is alone advances the RGB value, and its key is seen when it
 * is released with no other key gone down in between. */
static void key_test_event(struct s07_psoc *p, uint8_t key, bool down)
{
	if (down) {
		if (p->kt_down == 0) {
			p->kt_lone = key;
			p->kt_presses++;
		} else {
			p->kt_lone = S07_KEYS;
		}
		s07_led_key(p, key, 0xFE, 0x00);
		p->kt_down++;
	} else {
		if (p->kt_down)
			p->kt_down--;
		if (kt_tested(p, key))
			s07_led_key(p, key, 0x00, 0xFF);
		else
			s07_led_key(p, key, 0x00, 0x00);
		if (key == p->kt_lone) {
			p->kt_tested[key >> 3] =
				(uint8_t)(p->kt_tested[key >> 3] | 1u << (key & 7u));
			s07_led_key(p, key, 0x00, 0xFF);
		}
	}
	key_test_rgb(p);
}

void s07_key_test_start(struct s07_psoc *p)
{
	p->key_test = true;
	p->charge_led_on = false;
	p->kt_down = 0;
	p->kt_lone = S07_KEYS;
	p->kt_presses = 0;
	wb_fill(p->kt_tested, 0, sizeof p->kt_tested);
	s07_led_bar(p, 0x0E);
	key_test_rgb(p);
}

void s07_key_test_stop(struct s07_psoc *p)
{
	static const uint8_t off[3] = { 0, 0, 0 };

	p->key_test = false;
	s07_led_rgb(p, off, false);
	s07_led_bar(p, 0);
	p->charge_led_on = true;
	s07_update_charge_led(p);
	s07_led_all_off(p);		/* S-07.067 */
}

/* ---- table 2 ---- */

void s07_shutdown_reports(struct s07_psoc *p)
{
	if (p->shut)
		return;			/* S-07.072: once */
	p->shut = true;
	s07_report(p, S07_REPORT_SHUTDOWN);
	s07_led_all_off(p);
}

/* S-07.050, S-07.051 */
static int power_state(struct s07_psoc *p, uint8_t s)
{
	unsigned prev = p->state;

	p->state = s;
	log_ev(p, S07_LOG_POWER, s, 0);
	if (OPXY_TESTOS_OR(opxy_testos_power_state(prev, s), !s07_power_off_trigger(prev, s)))	/* the test OS: the states recorded, a power-off logged first */
		return S07_OK;
	if (p->ops->shutdown)
		p->ops->shutdown(p->ops->ctx);
	s07_shutdown_reports(p);
	send(p, ID_POWER_32, &s, 1);
	p->off = true;			/* step 3: nothing more, no reset */
	return S07_POWEROFF;
}

/* S-07.031 */
static void bitmap(struct s07_psoc *p, const uint8_t *bm)
{
	for (unsigned k = 0; k < S07_KEYS; k++) {
		bool down = s07_key_down(bm, k);

		if (down == s07_key_down(p->keys, k))
			continue;
		if (down)
			p->keys[k >> 3] = (uint8_t)(p->keys[k >> 3] | 1u << (k & 7u));
		else
			p->keys[k >> 3] = (uint8_t)(p->keys[k >> 3] & ~(1u << (k & 7u)));
		log_ev(p, S07_LOG_KEY, k, down);
		if (p->key_test)
			key_test_event(p, (uint8_t)k, down);
		if (p->ops->key_event)
			p->ops->key_event(p->ops->ctx, (uint8_t)k, down);
	}
}

static int handle(struct s07_psoc *p, const struct s06_frame *f);

/*
 * S-07.024: a greeting from the PSoC. One that arrives while a re-greeting's
 * sequence runs only ends that sequence, whose messages then answer it, so
 * the PSoC's greetings never nest sequences on the stack.
 */
static int psoc_greeting(struct s07_psoc *p)
{
	static const uint8_t ok[1] = { 0x40 }, sub[2] = { 0x01, 0x01 };
	bool greeted;

	if (p->started && p->greeting) {
		p->greeted = true;		/* S-07.020: the sequence ends */
		return S07_OK;
	}
	if (p->started) {
		int rc = s07_greet(p, &greeted);

		if (rc)
			return rc;
		if (!greeted)
			log_ev(p, S07_LOG_GREET_FAILED, 0, 0);
		s07_report(p, S07_REPORT_HELD);		/* S-04.064 */
		send(p, ID_SUBSCRIBE, sub, 2);		/* S-07.025 */
	}
	send(p, ID_GREET_OK, ok, 1);
	p->greeted = true;			/* S-07.020 */
	return S07_OK;
}

static int handle(struct s07_psoc *p, const struct s06_frame *f)
{
	switch (f->id) {
	case ID_POWER_31:
	case ID_POWER_32:
		/* S-07.079: an empty payload records no state and starts no
		 * power-off; the charge LED is re-evaluated */
		if (f->len) {
			int rc = power_state(p, f->payload[0]);

			if (rc)
				return rc;
		}
		s07_update_charge_led(p);
		break;
	case 0x00:
		log_ev(p, S07_LOG_WARNING_00, 0, 0);	/* logged */
		s07_update_charge_led(p);
		break;
	case ID_POWER_35:
		s07_update_charge_led(p);		/* S-07.052 */
		break;
	case ID_BATTERY:				/* S-07.053 */
		s07_battery_decode(f->payload, f->len, &p->batt);
		if (p->batt.present)
			log_ev(p, S07_LOG_BATTERY, p->batt.charge, p->batt.mv);
		else
			log_ev(p, S07_LOG_BATTERY_REMOVED, 0, 0);
		s07_update_charge_led(p);
		break;
	case ID_BITMAP:
		/* S-07.079: fewer than 9 bytes: ignored; bytes past the
		 * ninth are not read */
		if (f->len >= S07_BITMAP)
			bitmap(p, f->payload);
		break;
	case ID_KEY_EVENT:				/* S-07.075: ignored */
		break;
	case ID_PORT:					/* logged */
		log_ev(p, S07_LOG_PORT,
		       (uint32_t)(f->len > 0 ? f->payload[0] : 0) |
			       (uint32_t)(f->len > 1 ? f->payload[1] : 0) << 8 |
			       (uint32_t)(f->len > 2 ? f->payload[2] : 0) << 16,
		       0);
		break;
	case ID_GREET:
		return psoc_greeting(p);
	case ID_GREET_OK:
		if (f->len && f->payload[0] == 0x40)
			p->greeted = true;		/* S-07.020 */
		else
			log_ev(p, S07_LOG_GREET_REJECTED,
			       f->len ? f->payload[0] : 0x100u, 0);
		break;
	case ID_PING:
		/* S-07.076: 0x02 with the same payload, route swapped */
		if (!p->off)
			(void)s06_send(p->node, p->link,
				       s06_swap_route(f->route), ID_PONG,
				       f->payload, f->len);
		break;
	default:					/* S-07.076 */
		if (p->ops->common)
			p->ops->common(p->ops->ctx, f);
		break;
	}
	return S07_OK;
}

int s07_power_message(struct s07_psoc *p, const struct s06_frame *f)
{
	if (p->off)
		return S07_POWEROFF;
	if (f->id == 0x00 || f->id == ID_POWER_31 || f->id == ID_POWER_32 ||
	    f->id == ID_POWER_35 || f->id == ID_BATTERY)
		return handle(p, f);
	return S07_OK;
}

int s07_poll(struct s07_psoc *p)
{
	struct s06_frame f;
	int rc;

	s06_node_poll(p->node);
	while (s06_link_take(p->node->link[p->link], &f)) {
		if (p->off)
			continue;
		OPXY_TESTOS(opxy_testos_frame_seen(&f);) rc = handle(p, &f);	/* the test OS keeps the last frame for its wait report */
		if (rc)
			return rc;
	}
	return S07_OK;
}

/* S-07.012: a waited 0x30/0x36 reply, handled as a power message. */
static int waited_reply(struct s07_psoc *p, const struct s06_frame *r)
{
	if (r->id == ID_POWER_31 || r->id == ID_POWER_32 || r->id == ID_BATTERY)
		return handle(p, r);
	if (r->id != ID_POWER_35)
		log_ev(p, S07_LOG_REPLY_ID, r->id, 0);
	s07_update_charge_led(p);
	return S07_OK;
}

/* ---- greeting and start-up (S-07.020-S-07.025) ---- */

int s07_greet(struct s07_psoc *p, bool *greeted)
{
	static const uint8_t hello[2] = { 0x01, 0x04 };
	int rc = S07_OK;

	p->greeted = false;
	p->greeting = true;
	for (int i = 0; i < GREET_TRIES && !p->off; i++) {
		send(p, ID_GREET, hello, 2);
		s07_sleep(p, GREET_GAP_US);
		rc = s07_poll(p);
		if (rc || p->greeted)
			break;
	}
	p->greeting = false;
	*greeted = p->greeted;
	return rc;
}

static bool is_greeting(const struct s06_frame *f)
{
	return f->id == ID_GREET ||
	       (f->id == ID_GREET_OK && f->len && f->payload[0] == 0x40);
}

/*
 * S-07.021: at most 1.5 s, ended by the PSoC's greeting, which is answered
 * (S-07.024). The other messages that arrive meanwhile wait, and are
 * handled once it ends, so that nothing else is sent during it.
 */
static int greet_wait(struct s07_psoc *p)
{
	static struct s06_frame waiting[S06_INBOX];
	struct s06_link *l = p->node->link[p->link];
	const uint32_t end = now(p) + GREET_FAIL_WAIT_US;
	size_t n = 0, i;
	int rc = S07_OK;

	while (!p->greeted && !p->off) {
		s06_node_poll(p->node);
		if (p->ops->service)
			p->ops->service(p->ops->ctx);
		while (rc == S07_OK && !p->greeted && n < S06_INBOX &&
		       s06_link_take(l, &waiting[n])) {
			if (is_greeting(&waiting[n]))
				rc = handle(p, &waiting[n]);
			else
				n++;
		}
		if (rc || p->greeted || s06_time_reached(now(p), end))
			break;
		s06_idle(p->node->clock, end);
	}
	for (i = 0; i < n && rc == S07_OK && !p->off; i++)
		rc = handle(p, &waiting[i]);
	return rc;
}

int s07_open(struct s07_psoc *p)
{
	bool greeted;
	int rc = s07_greet(p, &greeted);

	if (rc)
		return rc;
	if (!greeted) {				/* S-07.021 */
		log_ev(p, S07_LOG_GREET_FAILED, 0, 0);
		(void)raw_put(p, s07_cy_exit_packet, sizeof s07_cy_exit_packet);
		rc = greet_wait(p);
		if (rc)
			return rc;
	}
	send(p, ID_POWER_QUERY, 0, 0);		/* S-07.022 */
	send(p, ID_BATTERY_QUERY, 0, 0);
	return S07_OK;
}

/* S-07.055 */
static void vbus(struct s07_psoc *p)
{
	struct s06_frame r;
	uint8_t st = 0, n = 0;
	int t;

	/* no reply within 100 ms: as {00 00} (S-07.058) */
	if (request(p, ID_VBUS, 0, 0, &r)) {
		/* S-07.079: missing bytes read as 00 */
		st = (uint8_t)(r.len > 0 ? r.payload[0] : 0);
		n = (uint8_t)(r.len > 1 ? r.payload[1] : 0);
	}
	if ((uint32_t)n * 10u != p->applied_ma)
		log_ev(p, S07_LOG_VBUS, st, (uint32_t)n * 10u);
	p->applied_ma = s07_vbus_update(p->applied_ma, p->target_ma, st, n, &t);
	if (t >= 0) {
		const uint8_t lim[2] = { 0x00, (uint8_t)t };

		send(p, ID_LIMIT, lim, 2);
	}
}

/* S-07.056 */
static void limit_step(struct s07_psoc *p)
{
	if (p->applied_ma < p->target_ma) {
		vbus(p);
		s07_sleep(p, LIMIT_STEP_US);
	}
}

static bool keep_waiting(const struct s07_psoc *p)
{
	return p->state == S07_POWER_CHARGE ||
	       (!s07_battery_usable(&p->batt) && p->applied_ma < 500u);
}

/*
 * S-07.054's charge wait: the switch is off on external power, so the unit
 * only charges. Dark and with no USB device; one unit load is asked for,
 * which also keeps the 3 s rule off. Ends when the switch goes on, or in
 * the power-off a 0x14 starts (S-07.051).
 */
static int charge_wait(struct s07_psoc *p)
{
	uint32_t last_round = 0;
	bool first = true;

	/* S-07.054, S-07.057: a fixed 500 mA, which a standard port supplies;
	 * the unit stays off the bus, so no charger rule applies. */
	p->target_ma = 500u;
	while (p->state == S07_POWER_CHARGE) {
		int rc;

		if (first || !s06_time_reached(last_round + QUERY_EVERY_US,
					       now(p))) {
			first = false;
			last_round = now(p);
			send(p, ID_BATTERY_QUERY, 0, 0);
			send(p, ID_POWER_QUERY, 0, 0);
			vbus(p);
		}
		if (p->applied_ma > 100u)
			s07_update_charge_led(p);
		s07_sleep(p, ROUND_US);
		rc = s07_poll(p);
		if (rc)
			return rc;
		limit_step(p);
	}
	p->target_ma = 0;
	p->applied_ma = 0;
	return S07_OK;
}

/* S-07.054's wait loop, with USB mass storage, while the battery is not
 * usable and less than 500 mA is applied. */
static int battery_loop(struct s07_psoc *p)
{
	if (keep_waiting(p)) {
		uint32_t start = now(p), last_round = 0;
		bool first = true;

		p->last_usb_us = start;
		if (p->ops->usb_msc)
			p->ops->usb_msc(p->ops->ctx, true);
		while (keep_waiting(p)) {
			/* The test OS alone: S-07.054's
			 * loop has no end while the PSoC does not answer or reports a
			 * state a cold start never shows; after TESTOS_WAIT_CAP_US the wait
			 * ends as if the battery were usable, with a report of what the link
			 * showed. Not in state 0x16: the switch is off and the unit only
			 * charges (S-07.050). The normal image waits as S-07.054 says. */
			OPXY_TESTOS(if (p->state != S07_POWER_CHARGE && s06_time_reached(now(p), start + TESTOS_WAIT_CAP_US)) { opxy_testos_wait_timeout(p, "battery wait (S-07.054)", now(p) - start); break; })
			if (first || !s06_time_reached(last_round + QUERY_EVERY_US,
						       now(p))) {
				first = false;
				last_round = now(p);
				send(p, ID_BATTERY_QUERY, 0, 0);
				send(p, ID_POWER_QUERY, 0, 0);
				if (p->applied_ma != p->target_ma ||
				    p->applied_ma < 500u)
					vbus(p);
			}
			/* S-16.017: the 3 s charger rule, and no 1 s rule here.
			 * SPEC-Q (S-07.054): the 1 s rule may also fire
			 * in the wait, and the condition may be "no host connected"
			 * rather than "not configured"; a bus reset's 100 mA target
			 * keeps a connected host out of this rule either way. */
			if (!p->usb_configured && p->target_ma == 0 &&
			    !s06_time_reached(p->last_usb_us + CHARGER_US, now(p)))
				p->target_ma = 1000u;
			s07_sleep(p, ROUND_US);
			const int rc = s07_poll(p);
			if (rc)
				return rc;
			limit_step(p);
		}
		if (p->ops->usb_msc)
			p->ops->usb_msc(p->ops->ctx, false);
		p->applied_ma = 0;
	}
	s07_update_charge_led(p);
	return S07_OK;
}

/* S-07.054: the first two requests, then any charge wait. */
int s07_battery_wait(struct s07_psoc *p)
{
	struct s06_frame r;
	int rc;

	if (request(p, ID_BATTERY_QUERY, 0, 0, &r) && (rc = waited_reply(p, &r)))
		return rc;
	if (request(p, ID_POWER_QUERY, 0, 0, &r) && (rc = waited_reply(p, &r)))
		return rc;
	if (p->state == S07_POWER_CHARGE && (rc = charge_wait(p)))
		return rc;
	return S07_OK;
}

/* S-07.025: the first key subscription; the link is started. */
void s07_subscribe(struct s07_psoc *p)
{
	static const uint8_t sub[2] = { 0x01, 0x01 };

	send(p, ID_SUBSCRIBE, sub, 2);
	p->started = true;
}

void s07_resume(struct s07_psoc *p, bool greeted, bool state_known, uint8_t state,
		const uint8_t *batt, size_t batt_len)
{
	p->greeted = greeted;
	p->started = true;
	if (state_known)
		p->state = state;
	if (batt_len != 0u)
		s07_battery_decode(batt, batt_len, &p->batt);
}

int s07_battery_rest(struct s07_psoc *p)
{
	return battery_loop(p);
}

void s07_key_status_given(struct s07_psoc *p, const uint8_t *pl, size_t n,
			  struct s07_key_status *ks)
{
	if (pl != NULL) {
		s07_key_status_decode(pl, n, ks);
		log_ev(p, S07_LOG_KEY_STATUS,
		       (uint32_t)ks->hold | (uint32_t)ks->debug << 1 |
			       (uint32_t)ks->precomp << 2, 0);
	} else {
		s07_key_status_decode(0, 0, ks);	/* S-16.008: held */
		log_ev(p, S07_LOG_KEYS_NO_REPLY, 0, 0);
	}
}

/* S-07.032 */
int s07_key_status(struct s07_psoc *p, struct s07_key_status *ks)
{
	struct s06_frame r;

	if (request(p, ID_KEY_STATUS, 0, 0, &r)) {
		OPXY_TESTOS(opxy_testos_key_raw(r.payload, r.len);)
		s07_key_status_decode(r.payload, r.len, ks);
		log_ev(p, S07_LOG_KEY_STATUS,
		       (uint32_t)ks->hold | (uint32_t)ks->debug << 1 |
			       (uint32_t)ks->precomp << 2, 0);
	} else {
		s07_key_status_decode(0, 0, ks);	/* S-16.008: held */
		log_ev(p, S07_LOG_KEYS_NO_REPLY, 0, 0);
	}
	return S07_OK;
}

/* S-07.023: the keys before the wait loop, whatever the battery's
 * charge. */
int s07_startup(struct s07_psoc *p, struct s07_key_status *ks)
{
	int rc = s07_open(p);

	if (rc)
		return rc;
	OPXY_TESTOS_OR((void)0, s07_report(p, S07_REPORT_START));	/* S-07.066; not in the test OS, whose panel is up and lit by now (S-13c.019's 00 00 00 would darken it) */
	rc = s07_battery_wait(p);
	if (rc)
		return rc;
	if (p->ops->early_logo)		/* S-05.004 */
		p->ops->early_logo(p->ops->ctx);
	s07_subscribe(p);
	(void)s07_key_status(p, ks);	/* no reply counts as held (S-16.008) */
	return s07_battery_rest(p);
}

/* ---- PMIC, reports, hand-over (S-07.060-S-07.071) ---- */

int s07_pmic(struct s07_psoc *p)
{
	static const uint8_t on[1] = { 0x01 }, off[1] = { 0x00 },
			     rd[2] = { 0x03, 0x10 };
	struct s06_frame r;
	uint8_t wr[3] = { 0x03, 0x10, 0 };

	send(p, ID_PMIC_PASS, on, 1);
	/* no reply within 100 ms: nothing more, pass-through left on */
	if (!request(p, ID_PMIC_READ, rd, 2, &r))
		return S07_OK;
	/* S-07.079: a reply of fewer than 3 bytes is handled as no reply */
	if (r.len < 3)
		return S07_OK;
	wr[2] = s07_pmic_write_value(r.payload[2]);
	send(p, ID_PMIC_WRITE, wr, 3);
	send(p, ID_PMIC_PASS, off, 1);
	(void)request(p, ID_PING, 0, 0, &r);	/* its reply is not used */
	return S07_OK;
}

void s07_report(struct s07_psoc *p, int which)
{
	static const uint8_t pl[4][3] = {
		{ 0x00, 0x00, 0x00 }, { 0x3C, 0xFF, 0x02 },
		{ 0x3C, 0xFF, 0x32 }, { 0x00, 0x00, 0x02 },
	};

	if (which >= 0 && which < 4)
		send(p, ID_REPORT, pl[which], 3);
}

int s07_handover(struct s07_psoc *p)
{
	int rc = s07_pmic(p);

	s07_report(p, S07_REPORT_BOOT);	/* not sent after a power-off */
	return rc;
}

int s07_info_request(struct s07_psoc *p, uint8_t *buf, size_t max,
		     int *status)
{
	struct s06_frame r;
	size_t n;

	*status = S07_OK;
	if (!request(p, ID_INFO, 0, 0, &r))
		return -1;
	n = r.len < max ? r.len : max;
	wb_copy(buf, r.payload, n);
	return (int)n;
}

int s07_service_pass(struct s07_psoc *p)
{
	int rc;

	if (!p->first_pass_done) {		/* S-07.043 */
		p->first_pass_done = true;
		s07_led_all_off(p);
		s07_led_bar(p, 0);
	}
	rc = s07_poll(p);
	if (rc)
		return rc;
	limit_step(p);
	return S07_OK;
}

void s07_reset_request(struct s07_psoc *p)
{
	static const uint8_t v[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

	send(p, ID_RESET, v, 4);
}

void s07_volcal(struct s07_psoc *p)
{
	static const uint8_t v[1] = { 0x01 };

	send(p, ID_VOLCAL, v, 1);
}

void s07_usb_configured(struct s07_psoc *p, uint32_t ma)
{
	p->usb_configured = true;
	p->target_ma = ma;
	p->last_usb_us = now(p);
}

/* S-07.057: 100 mA after SET_CONFIGURATION(0), a reset, VBUS loss */
void s07_usb_reset(struct s07_psoc *p)
{
	p->usb_configured = false;
	p->target_ma = 100u;
	p->last_usb_us = now(p);
}

void s07_usb_activity(struct s07_psoc *p)
{
	p->last_usb_us = now(p);
}

/* ---- backplane (S-07.080-S-07.085) ---- */

static void raw_discard(struct s07_psoc *p)
{
	uint8_t b[64];

	while (s06_raw_read(p->node, p->link, b, sizeof b))
		;
}

/* S-07.081: collect up to 15 bytes from the first 01; judge after 5 ms. */
static bool probe(struct s07_psoc *p)
{
	uint8_t got[15], b;
	size_t n = 0;
	uint32_t end = now(p) + PROBE_US + PROBE_MARGIN_US;

	for (;;) {
		while (s06_raw_read(p->node, p->link, &b, 1)) {
			if (n == 0 && b != 0x01)
				continue;	/* bytes before the first 01 */
			if (n < sizeof got)
				got[n++] = b;	/* the rest is discarded */
		}
		if (n == sizeof got || s06_time_reached(now(p), end))
			break;
		s06_idle(p->node->clock, end);
	}
	return s07_cy_probe_ok(got, n);
}

struct cy_ctx {
	struct s07_psoc *p;
};

static void cy_send(void *ctx, const uint8_t *d, size_t n)
{
	(void)raw_put(((struct cy_ctx *)ctx)->p, d, n);
}

static bool cy_recv(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_us)
{
	struct s07_psoc *p = ((struct cy_ctx *)ctx)->p;
	uint32_t end = now(p) + timeout_us;
	size_t got = 0;

	for (;;) {
		got += s06_raw_read(p->node, p->link, buf + got, n - got);
		if (got == n)
			return true;
		if (s06_time_reached(now(p), end))
			return false;
		s06_idle(p->node->clock, end);
	}
}

/*
 * S-07.085: the link framed again, wait for the application's greeting,
 * with exit packets from about 1 s, at most 2 s. The step's line goes on
 * the log first: a line on a raw link is lost (S-06.062, S-14.027), and
 * the result does not depend on the greeting, a power-off can end the
 * wait, and a frame after the exit packets may be lost by the PSoC
 * (S-07.086).
 */
static int return_wait(struct s07_psoc *p, uint32_t a, uint32_t b)
{
	uint32_t start;

	s06_set_raw(p->node, p->link, false);
	p->greeted = false;
	start = now(p);
	log_ev(p, S07_LOG_BACKPLANE, a, b);
	for (;;) {
		const int rc = s07_poll(p);
		uint32_t el;

		if (rc)
			return rc;
		if (p->greeted)
			return S07_OK;
		s07_sleep(p, RETURN_GAP_US);
		el = now(p) - start;
		if (el >= RETURN_GIVE_UP_US)
			return S07_OK;
		if (el >= RETURN_EXIT_FROM_US)		/* S-07.086 */
			(void)raw_put(p, s07_cy_exit_packet,
				      sizeof s07_cy_exit_packet);
	}
}

int s07_backplane(struct s07_psoc *p, const struct s07_cyacd *f,
		  const uint8_t unlock[S07_CY_UNLOCK_LEN], bool *ok)
{
	struct cy_ctx cc = { p };
	struct s07_cy_io io = { cy_send, cy_recv, &cc };
	struct s07_cy_result res = { false, 0, 0 };
	uint8_t enter[7 + S07_CY_UNLOCK_LEN];
	size_t elen = s07_cy_packet(S07_CY_ENTER, unlock, S07_CY_UNLOCK_LEN,
				    S07_CY_SUM, p->ops->crc16, enter);
	bool entered = false;
	int rc;

	*ok = false;
	for (int attempt = 0; attempt < ENTER_TRIES && !entered; attempt++) {
		if (attempt == 0) {			/* S-07.080 step 1 */
			rc = s07_poll(p);
			if (rc)
				return rc;
			send(p, ID_BOOTLOADER, 0, 0);
			s06_set_raw(p->node, p->link, true);
		} else {
			/* the link is raw: not transmitted (S-16.147) */
			send(p, ID_BOOTLOADER, 0, 0);
		}
		/* step 2; the bytes received during the wait are discarded
		 * too, which S-07.080 and S-07.081 allow */
		s07_sleep(p, ENTER_WAIT_US);
		raw_discard(p);
		(void)raw_put(p, enter, elen);		/* step 3 */
		entered = probe(p);
	}
	/* S-07.080: the 0xEE may have started the PSoC's bootloader even
	 * though no enter packet was answered; the return wait's exit
	 * packets bring its application back */
	if (!entered)
		return return_wait(p, 0, 0);

	/* S-07.084: no exit packet between runs or after the last failed
	 * one, so a PSoC whose application still validates stays in its
	 * bootloader; the return wait's exits end the session (S-16.043,
	 * S-16.079) */
	for (int run = 0; run < RUNS; run++) {
		raw_discard(p);
		s07_cy_run(&io, f, unlock, p->ops->crc16, &res);
		if (res.ok)
			break;
	}
	*ok = res.ok;
	return return_wait(p, res.ok, (uint32_t)res.at << 8 | res.why);
}
