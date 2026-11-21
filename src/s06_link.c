/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-06 links: sequence numbers, defaults, request slots, forwarding and raw
 * mode (S-06.010-S-06.065).
 */
#include "waffle/s06_link.h"
#include "waffle/bytes.h"

/* S-06.050 */
const struct s06_route_entry s06_master_routes[6] = {
	{ S06_LINK_MIDI, S06_ADDR_SLAVE, S06_LINK_CPU },
	{ S06_LINK_CPU, S06_ADDR_HOST, S06_LINK_MIDI },
	{ S06_LINK_CDC, S06_ADDR_PSOC, S06_LINK_PSOC },
	{ S06_LINK_PSOC, S06_ADDR_HOST, S06_LINK_CDC },
	{ S06_LINK_CDC, S06_ADDR_NORDIC, S06_LINK_NORDIC },
	{ S06_LINK_NORDIC, S06_ADDR_HOST, S06_LINK_CDC },
};

void s06_node_master_routes(struct s06_node *n, const struct wb_device *dev)
{
	n->routes = s06_master_routes;
	n->nroutes = 6u;
	n->routes_off = wb_has(dev, WB_F_SECOND_CPU) ? 0u : S06_ROUTES_CPU;
}

void s06_link_init(struct s06_link *l, const struct s06_port_ops *ops,
		   void *ctx, uint8_t own, uint8_t peer)
{
	l->ops = ops;
	l->ctx = ctx;
	l->own = (uint8_t)(own & 0x0Fu);
	l->peer = (uint8_t)(peer & 0x0Fu);
	l->seq = 0;			/* S-06.020: 0 when the bootloader starts */
	l->next_slot = 0;
	l->raw = false;
	l->held = false;
	l->paced = false;
	s06_rx_init(&l->rx, own);
	for (unsigned i = 0; i < S06_SLOTS; i++)
		l->slot[i].state = S06_SLOT_FREE;
	l->in_head = 0;
	l->in_count = 0;
	l->inbox_lost = 0;
	l->read_at = 0;
}

void s06_node_init(struct s06_node *n, s06_crc8_fn crc,
		   const struct s06_clock *clock)
{
	for (unsigned i = 0; i < S06_MAX_LINKS; i++)
		n->link[i] = 0;
	n->crc = crc;
	n->clock = clock;
	n->routes = 0;
	n->nroutes = 0;
	n->routes_off = 0;
	n->obs.dropped = 0;
	n->obs.ctx = 0;
}

static void drop(struct s06_node *n, unsigned link, int why,
		 const uint8_t *data, size_t len)
{
	if (n->obs.dropped)
		n->obs.dropped(n->obs.ctx, link, why, data, len);
}

/* ---- sending ---- */

/* S-06.012: zero nibbles become the defaults. */
static uint8_t defaults(const struct s06_link *l, uint8_t route)
{
	uint8_t src = s06_src(route), dst = s06_dst(route);

	return s06_route((uint8_t)(src ? src : l->own),
			 (uint8_t)(dst ? dst : l->peer));
}

static bool put(struct s06_link *l, const uint8_t *data, size_t len)
{
	if (l->ops->room(l->ctx) < len)
		return false;
	l->ops->write(l->ctx, data, len);
	return true;
}

/* S-06.002, S-06.012, S-06.020, S-06.062 */
bool s06_send(struct s06_node *n, unsigned link, uint8_t route, uint8_t id,
	      const uint8_t *payload, size_t len)
{
	struct s06_link *l = n->link[link];
	uint8_t wire[S06_MAX_WIRE];
	size_t w;

	if (len > S06_MAX_PAYLOAD)
		return false;			/* S-06.002: not sent at all */
	w = s06_frame_encode(defaults(l, route), l->seq, id, payload, len,
			     n->crc, wire);
	l->seq = (uint8_t)(l->seq + 1u);	/* S-16.147: used even if not sent */
	if (l->raw)
		return false;			/* S-06.062: not kept for later */
	return put(l, wire, w);
}

/* S-06.020, S-14.027: a log line not sent uses no number. */
bool s06_send_log(struct s06_node *n, unsigned link, uint8_t id,
		  const uint8_t *payload, size_t len)
{
	struct s06_link *l = n->link[link];
	uint8_t wire[S06_MAX_WIRE];
	size_t w;

	if (l->raw || len > S06_MAX_PAYLOAD)
		return false;
	w = s06_frame_encode(s06_route(l->own, l->peer), l->seq, id, payload,
			     len, n->crc, wire);
	if (!put(l, wire, w))
		return false;
	l->seq = (uint8_t)(l->seq + 1u);
	return true;
}

/* ---- requests (S-06.030-S-06.035) ---- */

int s06_request(struct s06_node *n, unsigned link, uint8_t id,
		const uint8_t *payload, size_t len, uint32_t timeout_us)
{
	struct s06_link *l = n->link[link];
	uint32_t now = s06_now(n->clock);

	s06_link_expire(l, now);
	for (unsigned i = 0; i < S06_SLOTS; i++) {
		unsigned s = (l->next_slot + i) % S06_SLOTS;

		if (l->slot[s].state != S06_SLOT_FREE)
			continue;
		/* S-06.030: source 8 + n, destination the peer */
		l->next_slot = (uint8_t)((s + 1u) % S06_SLOTS);
		l->slot[s].state = S06_SLOT_PENDING;
		l->slot[s].deadline = now + timeout_us;
		(void)s06_send(n, link,
			       s06_route((uint8_t)(S06_ADDR_SLOT0 + s), l->peer),
			       id, payload, len);
		return (int)s;
	}
	return -1;	/* S-06.030: no free slot, fails at once */
}

/* S-06.035: a request whose time is up fails; its slot is free again. */
void s06_link_expire(struct s06_link *l, uint32_t now)
{
	for (unsigned i = 0; i < S06_SLOTS; i++)
		if (l->slot[i].state == S06_SLOT_PENDING &&
		    s06_time_reached(now, l->slot[i].deadline))
			l->slot[i].state = S06_SLOT_FREE;
}

int s06_request_state(struct s06_node *n, unsigned link, int slot)
{
	struct s06_link *l = n->link[link];

	s06_link_expire(l, s06_now(n->clock));
	return l->slot[slot].state;
}

void s06_request_collect(struct s06_node *n, unsigned link, int slot,
			 struct s06_frame *reply)
{
	struct s06_slot *s = &n->link[link]->slot[slot];

	if (reply && s->state == S06_SLOT_DONE)
		*reply = s->reply;
	s->state = S06_SLOT_FREE;
}

int s06_transact(struct s06_node *n, unsigned link, uint8_t id,
		 const uint8_t *payload, size_t len, uint32_t timeout_us,
		 struct s06_frame *reply)
{
	int slot = s06_request(n, link, id, payload, len, timeout_us);
	uint32_t deadline;

	if (slot < 0)
		return -1;
	deadline = n->link[link]->slot[slot].deadline;
	for (;;) {
		s06_node_poll(n);
		switch (s06_request_state(n, link, slot)) {
		case S06_SLOT_DONE:
			s06_request_collect(n, link, slot, reply);
			return 1;
		case S06_SLOT_FREE:
			return 0;
		default:
			break;
		}
		s06_idle(n->clock, deadline);
	}
}

/* ---- receiving ---- */

static void deliver(struct s06_node *n, unsigned link, const uint8_t *raw)
{
	struct s06_link *l = n->link[link];
	uint8_t dst = s06_dst(raw[0]);
	struct s06_frame *f;

	/* S-06.032: destination 8 + n completes a request pending in slot n;
	 * S-06.033, S-06.034: otherwise, and for 12-15, an ordinary message. */
	if (dst >= S06_ADDR_SLOT0 && dst < S06_ADDR_SLOT0 + S06_SLOTS &&
	    l->slot[dst - S06_ADDR_SLOT0].state == S06_SLOT_PENDING) {
		f = &l->slot[dst - S06_ADDR_SLOT0].reply;
		l->slot[dst - S06_ADDR_SLOT0].state = S06_SLOT_DONE;
	} else if (l->in_count >= S06_INBOX) {
		l->inbox_lost++;
		drop(n, link, S06_DROP_INBOX, raw, (size_t)raw[3] + 5u);
		return;
	} else {
		f = &l->inbox[(l->in_head + l->in_count) % S06_INBOX];
		l->in_count++;
	}
	f->route = raw[0];
	f->seq = raw[1];
	f->id = raw[2];
	f->len = raw[3];
	wb_copy(f->payload, raw + S06_HEADER, raw[3]);
	f->at = l->read_at;
}

/* S-06.046, S-06.050, S-06.051, S-06.064 */
static void forward(struct s06_node *n, unsigned link, uint8_t dst,
		    const uint8_t *wire, size_t len)
{
	for (unsigned i = 0; i < n->nroutes; i++) {
		const struct s06_route_entry *r = &n->routes[i];
		struct s06_link *out;

		if (r->in != link || r->dst != dst || (n->routes_off >> i & 1u))
			continue;
		out = n->link[r->out];
		if (!out || out->raw)
			break;		/* S-06.064 */
		/* S-06.022: unchanged, no number used */
		if (!put(out, wire, len))
			drop(n, link, S06_DROP_NO_ROOM, wire, len);
		return;
	}
	drop(n, link, S06_DROP_NO_ROUTE, wire, len);
}

/*
 * The bytes a paced link may read now. A message is complete at len + 5
 * decoded bytes (S-06.041); COBS gives at most one per wire byte and none
 * for a frame's first, so k bytes complete at most 1 + (k - 1) / 6
 * messages, the one already begun included.
 */
#define FRAME_WIRE_MIN (S06_HEADER + 2u)

static size_t read_max(const struct s06_link *l, size_t max)
{
	size_t room, k;

	if (!l->paced)
		return max;
	room = S06_INBOX - (size_t)l->in_count;
	if (room == 0u)
		return 0;
	k = 1u + (room - 1u) * FRAME_WIRE_MIN;
	return k < max ? k : max;
}

void s06_node_poll_link(struct s06_node *n, unsigned link)
{
	struct s06_link *l = n->link[link];
	uint8_t buf[64];
	size_t got = 1, max;
	uint32_t start;

	if (!l || l->raw || l->held)
		return;		/* S-06.061: raw bytes are not decoded */
	start = s06_now(n->clock);
	while ((max = read_max(l, sizeof buf)) != 0 &&
	       (got = l->ops->read(l->ctx, buf, max)) != 0) {
		for (size_t i = 0; i < got; i++) {
			int ev = s06_rx_byte(&l->rx, buf[i], n->crc);

			if (ev == S06_RX_NONE)
				continue;
			if (ev == S06_RX_DELIVER)
				deliver(n, link, l->rx.buf);
			else if (ev == S06_RX_FORWARD)
				forward(n, link, l->rx.fwd_dst, l->rx.buf,
					l->rx.n);
			else
				drop(n, link, ev, l->rx.buf, l->rx.n);
		}
	}
	/* read empty: what arrived before the start has all been read */
	if (got == 0)
		l->read_at = start;
	/* after the bytes: a reply that arrived by the deadline is in time */
	s06_link_expire(l, s06_now(n->clock));
}

void s06_link_hold(struct s06_link *l, bool on)
{
	l->held = on;
}

void s06_node_poll(struct s06_node *n)
{
	for (unsigned i = 0; i < S06_MAX_LINKS; i++)
		s06_node_poll_link(n, i);
}

bool s06_link_take(struct s06_link *l, struct s06_frame *out)
{
	if (l->in_count == 0)
		return false;
	*out = l->inbox[l->in_head];
	l->in_head = (uint8_t)((l->in_head + 1u) % S06_INBOX);
	l->in_count--;
	return true;
}

void s06_link_put(struct s06_link *l, const struct s06_frame *f)
{
	if (l->in_count >= S06_INBOX)
		return;		/* cannot be: the caller just took one */
	l->inbox[(l->in_head + l->in_count) % S06_INBOX] = *f;
	l->in_count++;
}

/* ---- raw mode (S-06.060-S-06.065) ---- */

static void discard_input(struct s06_link *l)
{
	uint8_t buf[64];

	while (!l->held && l->ops->read(l->ctx, buf, sizeof buf) != 0)
		;
}

/*
 * S-06.060. Entering: what has arrived in framed mode is read as frames
 * first, then a partial frame is discarded. Leaving: received raw bytes not
 * yet consumed are discarded. Bytes already handed to the port are sent.
 */
void s06_set_raw(struct s06_node *n, unsigned link, bool on)
{
	struct s06_link *l = n->link[link];

	if (on && !l->raw)
		s06_node_poll_link(n, link);
	else if (!on && l->raw)
		discard_input(l);
	l->raw = on;
	s06_rx_reset(&l->rx);
}

size_t s06_raw_read(struct s06_node *n, unsigned link, uint8_t *buf,
		    size_t max)
{
	struct s06_link *l = n->link[link];

	if (!l->raw || l->held)
		return 0;
	return l->ops->read(l->ctx, buf, max);	/* S-06.061: unchanged */
}

bool s06_raw_write(struct s06_node *n, unsigned link, const uint8_t *data,
		   size_t len)
{
	return put(n->link[link], data, len);
}
