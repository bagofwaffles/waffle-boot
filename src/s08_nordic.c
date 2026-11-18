/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-08 exchanges on the Nordic link.
 */
#include "waffle/s08_nordic.h"
#include "waffle/bytes.h"

#define ID_GREET 0x05u
#define ID_INFO 0x09u
#define ID_DFU 0x0Du
#define ID_DFU_REPLY 0x0Eu
#define DFU_ENTER 0xEEu
#define DFU_EXIT 0x18u

#define GREET_US S06_MS(5)		/* S-08.002 */
#define GREET_TRIES 1000
#define INFO_US S06_MS(100)		/* S-08.020 */
#define ENTER_WAIT_US S06_MS(250)	/* S-08.031 step 2 */
#define ENTER_QUERIES 20
#define ENTER_GREET_US S06_MS(500)	/* step 4 */
#define AFTER_GREET_US S06_MS(5)	/* step 5, S-08.015 */
#define STEP_US S06_MS(1)
#define EXIT_PHASE1 2500		/* S-08.033 */
#define EXIT_SILENCE 100
#define EXIT_PHASE2 20
#define PREPARE_US S06_MS(1500)		/* S-08.041 */
#define CHUNK_US S06_MS(1000)
#define BUSY_US S06_MS(5000)
#define BUSY_TOTAL_US S06_MS(60000)	/* a request's busy replies, from the first */
#define TRANSFER_QUERIES 3		/* S-08.042 */

static uint32_t now(const struct s08_nordic *n)
{
	return s06_now(n->node->clock);
}

static void log_ev(struct s08_nordic *n, int ev, const uint8_t *d, size_t len,
		   uint32_t a)
{
	if (n->ops->log)
		n->ops->log(n->ops->ctx, ev, d, len, a);
}

static void message(struct s08_nordic *n, uint8_t id, uint8_t byte)
{
	(void)s06_send(n->node, n->link, 0, id, &byte, 1);	/* route 0x13 */
}

static void routes(struct s08_nordic *n, bool on)
{
	uint32_t bits = 1u << S06_ROUTE_CDC_NORDIC | 1u << S06_ROUTE_NORDIC_CDC;

	if (n->node->routes != s06_master_routes)
		return;
	if (on)
		n->node->routes_off &= ~bits;
	else
		n->node->routes_off |= bits;
}

void s08_init(struct s08_nordic *n, struct s06_node *node, unsigned link,
	      const struct s08_ops *ops, int state)
{
	n->node = node;
	n->link = link;
	n->ops = ops;
	n->state = state;
	n->greeted = false;
	n->greet_tries = 0;
	n->connected = false;
	n->replied = false;
	n->power_status = 0;
	n->sku_read = false;
	n->product = 0;
	n->entry_failed = false;
	n->failure.at = S08_AT_NONE;
	routes(n, false);	/* S-08.004: nothing forwarded before the greeting */
}

void s08_set_state(struct s08_nordic *n, int state)
{
	n->state = state;
}

void s08_set_sku(struct s08_nordic *n, uint32_t sku, bool read_ok)
{
	if (n->sku_read)
		return;		/* S-08.045: once per power-on */
	n->sku_read = true;
	n->product = s08_product_word(sku, read_ok);
}

uint32_t s08_product(const struct s08_nordic *n)
{
	return n->product;
}

/* S-08.004, S-08.010-S-08.013 */
static void handle(struct s08_nordic *n, const struct s06_frame *f)
{
	uint8_t id, byte;

	if (f->id == ID_GREET) {
		if (!s08_answer_greeting(n->state, f->payload, f->len, &id, &byte))
			return;			/* S-08.012: ignored as a whole */
		if (f->len >= 2 && n->ops->revision)
			n->ops->revision(n->ops->ctx, f->payload[1]);
		message(n, id, byte);		/* one answer per greeting */
		n->connected = true;
		return;
	}
	if (s08_counts_as_greeting(f))
		n->connected = true;
	if (f->id == 0x00 || f->id == 0x31 || f->id == 0x35 || f->id == 0x37) {
		if (n->ops->power) {
			int rc = n->ops->power(n->ops->ctx, f);

			if (rc && !n->power_status)
				n->power_status = rc;
		}
	} else if (n->ops->common) {
		n->ops->common(n->ops->ctx, f);
	}
}

int s08_poll(struct s08_nordic *n)
{
	struct s06_frame f;

	s06_node_poll(n->node);
	while (s06_link_take(n->node->link[n->link], &f))
		handle(n, &f);
	return n->power_status;
}

/* S-08.014: the greetings delivered so far are answered; every other
 * frame keeps its place for held mode's loop. */
void s08_answer_greetings(struct s08_nordic *n)
{
	struct s06_link *l = n->node->link[n->link];
	struct s06_frame f;

	s06_node_poll(n->node);
	for (unsigned left = l->in_count; left > 0 && s06_link_take(l, &f);
	     left--) {
		if (f.id == ID_GREET)
			handle(n, &f);
		else
			s06_link_put(l, &f);
	}
}

/* S-08.015: the frames delivered so far, discarded unhandled. */
static void discard(struct s08_nordic *n)
{
	struct s06_frame f;

	s06_node_poll(n->node);
	while (s06_link_take(n->node->link[n->link], &f))
		log_ev(n, S08_LOG_DROPPED, f.payload, f.len, f.id);
}

/* Wait, receiving (frames queue) but handling nothing. */
static void wait(struct s08_nordic *n, uint32_t us)
{
	uint32_t end = now(n) + us;

	for (;;) {
		s06_node_poll(n->node);
		if (s06_time_reached(now(n), end))
			return;
		s06_idle(n->node->clock, end);
	}
}

void s08_idle(struct s08_nordic *n, uint32_t us)
{
	uint32_t end = now(n) + us;

	while (!s06_time_reached(now(n), end)) {
		(void)s08_poll(n);
		wait(n, STEP_US);
	}
	(void)s08_poll(n);
}

/* S-08.002 */
bool s08_link_greeting(struct s08_nordic *n)
{
	static const uint8_t hello[2] = { 0x01, 0x03 };
	bool answered = false;
	int i;

	if (n->greeted)
		return false;		/* once per power-on */
	n->greeted = true;
	for (i = 0; i < GREET_TRIES && !answered; i++)
		answered = s06_transact(n->node, n->link, ID_GREET, hello, 2,
					GREET_US, 0) == 1;
	n->greet_tries = (unsigned)i;
	if (!answered)
		log_ev(n, S08_LOG_GREETING_UNANSWERED, 0, 0, 0);
	routes(n, true);	/* S-08.004: forwarding from the end on */
	return answered;
}

/* ---- queries (S-08.020-S-08.022) ---- */

static long info(struct s08_nordic *n, struct s06_frame *r)
{
	n->replied = s06_transact(n->node, n->link, ID_INFO, 0, 0, INFO_US, r) == 1;
	return n->replied ? r->len : -1;
}

long s08_info(struct s08_nordic *n, uint8_t *buf, size_t max)
{
	struct s06_frame r;
	long len = info(n, &r);

	if (len < 0)
		return -1;
	if ((size_t)len > max)
		len = (long)max;
	wb_copy(buf, r.payload, (size_t)len);
	return len;
}

/* S-08.020: an answer only from this query's own reply. */
int s08_mode_query(struct s08_nordic *n)
{
	struct s06_frame r;

	if (info(n, &r) < 0)
		return S08_NO_ANSWER;
	return s08_mode_answer(r.payload, r.len);
}

long s08_boot_slot_query(struct s08_nordic *n)
{
	struct s06_frame r;
	long slot;

	if (info(n, &r) < 0) {
		log_ev(n, S08_LOG_SLOT_UNREAD, 0, 0, 0);
		return -1;
	}
	slot = s08_boot_slot(r.payload, r.len);
	if (slot < 0)
		log_ev(n, S08_LOG_SLOT_UNREAD, r.payload, r.len, 1);
	return slot;
}

/* ---- entering and leaving DFU (S-08.030-S-08.034) ---- */

bool s08_enter_dfu(struct s08_nordic *n)
{
	uint32_t t_dfu, deadline;
	int q;

	n->connected = false;
	message(n, ID_DFU, DFU_ENTER);			/* step 1 */
	wait(n, ENTER_WAIT_US);				/* step 2 */
	discard(n);
	for (q = 0; q < ENTER_QUERIES; q++)		/* step 3 */
		if (s08_mode_query(n) == S08_IN_DFU)
			break;
	if (q == ENTER_QUERIES) {
		log_ev(n, S08_LOG_ENTER_FAILED, 0, 0, 1);
		return false;
	}
	/* step 4: the first greeting handled after the first query */
	t_dfu = now(n);
	deadline = t_dfu + ENTER_GREET_US;
	n->connected = false;
	for (;;) {
		(void)s08_poll(n);
		if (n->connected)
			break;
		if (s06_time_reached(now(n), deadline)) {
			log_ev(n, S08_LOG_ENTER_FAILED, 0, 0, 4);
			return false;
		}
		wait(n, STEP_US);
	}
	wait(n, AFTER_GREET_US);			/* step 5 */
	discard(n);
	return true;
}

bool s08_exit_dfu(struct s08_nordic *n)
{
	int silent = 0;

	message(n, ID_DFU, DFU_EXIT);			/* step 1 */
	n->connected = false;
	for (int q = 0; q < EXIT_PHASE1; q++) {		/* phase 1 */
		if (s08_mode_query(n) == S08_NOT_IN_DFU)
			return true;
		/* any reply restarts the count of queries without one */
		silent = n->replied ? 0 : silent + 1;
		if (silent >= EXIT_SILENCE)
			break;
		(void)s08_poll(n);
		if (n->connected)
			break;
		wait(n, STEP_US);
	}
	for (int q = 0; q < EXIT_PHASE2; q++)		/* phase 2 */
		if (s08_mode_query(n) == S08_NOT_IN_DFU)
			return true;
	log_ev(n, S08_LOG_EXIT_FAILED, 0, 0, 0);
	return false;
}

/* ---- DFU requests and the transfer (S-08.040-S-08.047) ---- */

/* S-08.041: after a busy reply, the next frame with id 0x0E, or after a busy
 * perform 0x0E or 0x0D, within 5 s and not past end; other frames meanwhile
 * may be discarded (S-08.015). */
static int busy_wait(struct s08_nordic *n, struct s06_frame *r, uint32_t end, bool take_0d)
{
	uint32_t deadline = now(n) + BUSY_US;

	if (s06_time_reached(deadline, end))
		deadline = end;
	for (;;) {
		s06_node_poll(n->node);
		while (s06_link_take(n->node->link[n->link], r)) {
			if (r->id == ID_DFU_REPLY || (take_0d && r->id == ID_DFU))
				return 1;
			log_ev(n, S08_LOG_DROPPED, r->payload, r->len, r->id);
		}
		if (s06_time_reached(now(n), deadline))
			return 0;
		s06_idle(n->node->clock, deadline);
	}
}

int s08_dfu_request(struct s08_nordic *n, const uint8_t *p, size_t len)
{
	struct s06_frame r;
	uint8_t cmd = p[0];
	uint32_t t = cmd == S08_DFU_PREPARE ? PREPARE_US : CHUNK_US;
	const bool perform = cmd == S08_DFU_PERFORM;
	uint8_t id;
	uint32_t end;
	int c;

	n->failure.reply = S08_REPLY_NONE;
	if (s06_transact(n->node, n->link, ID_DFU, p, len, t, &r) != 1)
		return S08_DFU_FAIL;		/* no reply within the timeout */
	id = r.id;
	c = s08_classify_dfu(cmd, id, r.payload, r.len);
	end = now(n) + BUSY_TOTAL_US;		/* S-08.041 */
	while (c == S08_DFU_BUSY) {
		n->failure.reply = S08_REPLY_BUSY;
		if (s06_time_reached(now(n), end) || !busy_wait(n, &r, end, perform))
			return S08_DFU_FAIL;
		/* a 0x0D that ends a busy perform's wait reads as its reply */
		id = perform && r.id == ID_DFU ? ID_DFU_REPLY : r.id;
		c = s08_classify_dfu(cmd, id, r.payload, r.len);
	}
	n->failure.reply = S08_REPLY_REFUSED;	/* for a failure below */
	if (c == S08_DFU_FAIL && id == ID_DFU_REPLY && r.len > 1)
		/* S-16.099: the error text is data, logged verbatim */
		log_ev(n, S08_LOG_DFU_ERROR, r.payload + 1, (size_t)r.len - 1u, cmd);
	return c;
}

bool s08_transfer(struct s08_nordic *n, const uint8_t *image, uint32_t len,
		  const uint16_t version[4], uint8_t type, uint32_t product)
{
	uint8_t p[1 + S08_HEADER_LEN];
	struct s08_failure *f = &n->failure;
	int a = S08_NO_ANSWER;

	f->at = S08_AT_QUERY;
	f->chunk = 0;
	f->chunks = (uint16_t)((len + S08_CHUNK_MAX - 1u) / S08_CHUNK_MAX);
	/* S-08.042: up to 3 queries until one has an answer */
	for (int q = 0; q < TRANSFER_QUERIES && a == S08_NO_ANSWER; q++)
		a = s08_mode_query(n);
	if (a != S08_IN_DFU) {
		f->reply = a == S08_NO_ANSWER ? S08_REPLY_NONE : S08_REPLY_REFUSED;
		log_ev(n, S08_LOG_TRANSFER_FAILED, 0, 0, 0);
		return false;
	}
	/* S-08.047: each request only after the previous one succeeded */
	f->at = S08_AT_PREPARE;
	s08_prepare_payload(version, type, product, p);
	if (s08_dfu_request(n, p, 14) != S08_DFU_OK)
		goto fail;
	f->at = S08_AT_HEADER;
	p[0] = S08_DFU_CHUNK;
	s08_te_header(image, len, version, product, n->ops->crc16, p + 1);
	if (s08_dfu_request(n, p, sizeof p) != S08_DFU_OK)
		goto fail;
	f->at = S08_AT_CHUNK;
	for (uint32_t off = 0; off < len; off += S08_CHUNK_MAX) {	/* S-08.046 */
		static uint8_t c[1 + S08_CHUNK_MAX];
		uint32_t k = len - off < S08_CHUNK_MAX ? len - off : S08_CHUNK_MAX;

		f->chunk++;
		c[0] = S08_DFU_CHUNK;
		wb_copy(c + 1, image + off, k);
		if (s08_dfu_request(n, c, 1u + k) != S08_DFU_OK)
			goto fail;
	}
	f->at = S08_AT_PERFORM;
	p[0] = S08_DFU_PERFORM;
	if (s08_dfu_request(n, p, 1) != S08_DFU_OK)
		goto fail;
	f->at = S08_AT_NONE;
	return true;
fail:
	/* a: the DFU command, then the image chunk it was (0: none) */
	log_ev(n, S08_LOG_TRANSFER_FAILED, 0, 0,
	       p[0] | (f->at == S08_AT_CHUNK ? (uint32_t)f->chunk << 8 : 0u));
	return false;
}

bool s08_run_start(struct s08_nordic *n, bool nordic_image, uint8_t *buf,
		   size_t max, long *info_len)
{
	*info_len = -1;
	n->entry_failed = false;
	if (nordic_image && !s08_enter_dfu(n)) {
		n->entry_failed = true;		/* S-08.035: no end-of-run query */
		return false;
	}
	if (s08_mode_query(n) == S08_IN_DFU)
		*info_len = s08_info(n, buf, max);
	return true;
}

bool s08_run_end(struct s08_nordic *n)
{
	if (n->entry_failed) {
		n->entry_failed = false;
		return true;
	}
	if (s08_mode_query(n) == S08_IN_DFU)
		return s08_exit_dfu(n);
	return true;
}
