/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for the S-06 framing logic (src/core/link/frame.c, link/rx.c,
 * link/link.c).
 *
 * Commands (one result line each):
 *   crc HEX                        -> the glue CRC-8 (below)
 *   cobs HEX                       -> s06_cobs_encode
 *   raw ROUTE SEQ ID HEX           -> s06_frame_raw
 *   encode ROUTE SEQ ID HEX        -> s06_frame_encode
 *   rx OWN HEX                     -> KIND:HEX per event of a fresh s06_rx
 *   fornode OWN DST                -> s06_rx_for_node
 * A master node on five ports (S06_LINK_*), each with a receive queue and a
 * transmit buffer of ROOM bytes (0: unlimited):
 *   node ROOM [slave]              -> ok (slave: no forwarding routes); then per link:
 *   link LINK OWN PEER             -> ok
 *   routes                         -> IN:DST:OUT per row of s06_master_routes
 *   send LINK ROUTE ID HEX         -> 1|0
 *   log LINK ID HEX                -> 1|0
 *   req LINK ID HEX TIMEOUT        -> the slot, or -1
 *   at NOW                         -> the clock; expires requests on every link
 *   in LINK HEX                    -> drops WHY:HEX... | msgs ROUTE:SEQ:ID:HEX... | replies SLOT:ROUTE:SEQ:ID:HEX...
 *   out LINK                       -> the bytes written since the last out, hex
 *   rawmode LINK 0|1               -> ok
 *   rawread LINK                   -> hex
 *   rawwrite LINK HEX              -> 1|0
 *   rawin LINK HEX                 -> ok (bytes queued for rawread)
 *   seq LINK                       -> the link's next seq
 * "in" polls the link once, then takes every inbox message and collects
 * every completed request, so the inbox never fills.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hlib.h"
#include "waffle/link/clock.h"
#include "waffle/link/frame.h"
#include "waffle/link/link.h"
#include "waffle/link/rx.h"

/*
 * The S-06 frame check (tests/oracle/conformance/serial/crc8.py): poly 0x4D
 * LSB first, whitening 0xA5 per byte.  Test glue: src/ takes the CRC as an
 * s06_crc8_fn and does not define one.
 */
static uint8_t crc_table[256];

static void crc_init(void)
{
	for (unsigned i = 0; i < 256; i++) {
		uint8_t c = (uint8_t)(i ^ 0xA5u);

		for (unsigned k = 0; k < 8; k++)
			c = (uint8_t)((c & 1u) ? (c >> 1) ^ 0xB2u : c >> 1);
		crc_table[i] = c;
	}
}

static uint8_t crc8(const uint8_t *data, size_t len)
{
	uint8_t c = 0;

	for (size_t i = 0; i < len; i++)
		c = crc_table[c ^ data[i]];
	return c;
}

static uint8_t buf[H_MAXLINE / 2];

static void put_frame(const struct s06_frame *f, int slot)
{
	char head[40];

	if (slot >= 0)
		snprintf(head, sizeof head, "%d:0x%x:%u:0x%x:", slot, f->route, f->seq, f->id);
	else
		snprintf(head, sizeof head, "0x%x:%u:0x%x:", f->route, f->seq, f->id);
	h_put(head);
	if (f->len)
		for (unsigned i = 0; i < f->len; i++)
			printf("%02x", f->payload[i]);
	else
		fputs("-", stdout);
}

/* --- the ports --- */

struct port {
	uint8_t in[H_MAXLINE / 2];
	size_t in_n, in_pos;
	uint8_t out[1u << 16];
	size_t out_n;
	size_t cap;
};

static struct port port[S06_MAX_LINKS];

static size_t p_read(void *ctx, uint8_t *b, size_t max)
{
	struct port *p = ctx;
	size_t n = p->in_n - p->in_pos;

	if (n > max)
		n = max;
	memcpy(b, p->in + p->in_pos, n);
	p->in_pos += n;
	return n;
}

/* cppcheck-suppress constParameterCallback ; s06_port_ops fixes the type */
static size_t p_room(void *ctx)
{
	const struct port *p = ctx;

	return p->cap - p->out_n;
}

static void p_write(void *ctx, const uint8_t *data, size_t n)
{
	struct port *p = ctx;

	memcpy(p->out + p->out_n, data, n);
	p->out_n += n;
}

static const struct s06_port_ops port_ops = { p_read, p_room, p_write };

static uint32_t clock_us;

static uint32_t c_now(void *ctx)
{
	(void)ctx;
	return clock_us;
}

static void c_idle(void *ctx, uint32_t until)
{
	(void)ctx;
	clock_us = until;
}

static const struct s06_clock clk = { c_now, c_idle, NULL };

static struct s06_node node;
static struct s06_link links[S06_MAX_LINKS];

/* The drops of the current "in", as WHY:HEX fields. */
static char drops[H_MAXLINE * 2];
static size_t drops_n;

static void dropped(void *ctx, unsigned link, int why, const uint8_t *data, size_t n)
{
	(void)ctx;
	(void)link;
	drops_n += (size_t)snprintf(drops + drops_n, sizeof drops - drops_n, " %d:", why);
	if (n == 0 || data == NULL)
		drops_n += (size_t)snprintf(drops + drops_n, sizeof drops - drops_n, "-");
	for (size_t i = 0; i < n && data; i++)
		drops_n += (size_t)snprintf(drops + drops_n, sizeof drops - drops_n, "%02x", data[i]);
}

static unsigned link_arg(const struct h_cmd *c, int i)
{
	unsigned long l = h_num(c, i);

	if (l >= S06_MAX_LINKS || node.link[l] == NULL) {
		h_error(c, "link");
		return S06_MAX_LINKS;
	}
	return (unsigned)l;
}

static void do_in(const struct h_cmd *c, unsigned l)
{
	struct port *p = &port[l];
	struct s06_frame f;

	p->in_n = h_hex(c, 2, p->in, sizeof p->in);
	p->in_pos = 0;
	drops_n = 0;
	drops[0] = 0;
	s06_node_poll_link(&node, l);
	h_put("drops");
	fputs(drops, stdout);
	h_put("| msgs");
	while (s06_link_take(&links[l], &f))
		put_frame(&f, -1);
	h_put("| replies");
	for (int s = 0; s < S06_SLOTS; s++) {
		if (links[l].slot[s].state == S06_SLOT_DONE) {
			s06_request_collect(&node, l, s, &f);
			put_frame(&f, s);
		}
	}
	h_end();
}

static void node_cmd(const struct h_cmd *c, const char *k)
{
	unsigned l = S06_MAX_LINKS;
	size_t n;

	if (strcmp(k, "node") == 0) {
		size_t cap = (size_t)h_num(c, 1);

		s06_node_init(&node, crc8, &clk);
		if (c->n < 3) {
			node.routes = s06_master_routes;
			node.nroutes = (uint8_t)(sizeof s06_master_routes / sizeof s06_master_routes[0]);
		}
		node.obs.dropped = dropped;
		memset(port, 0, sizeof port);
		for (unsigned i = 0; i < S06_MAX_LINKS; i++)
			port[i].cap = cap ? cap : sizeof port[i].out;
		clock_us = 0;
		h_put("ok");
		h_end();
		return;
	}
	if (strcmp(k, "link") == 0) {
		unsigned long i = h_num(c, 1);

		if (i >= S06_MAX_LINKS) {
			h_error(c, "link");
			return;
		}
		s06_link_init(&links[i], &port_ops, &port[i], (uint8_t)h_num(c, 2), (uint8_t)h_num(c, 3));
		node.link[i] = &links[i];
		h_put("ok");
		h_end();
		return;
	}
	if (strcmp(k, "routes") == 0) {
		for (unsigned i = 0; i < node.nroutes; i++) {
			char s[32];

			snprintf(s, sizeof s, "%u:%u:%u", node.routes[i].in, node.routes[i].dst,
				 node.routes[i].out);
			h_put(s);
		}
		h_end();
		return;
	}
	if (strcmp(k, "at") == 0) {
		clock_us = (uint32_t)h_num(c, 1);
		for (unsigned i = 0; i < S06_MAX_LINKS; i++)
			if (node.link[i])
				s06_link_expire(node.link[i], clock_us);
		h_put("ok");
		h_end();
		return;
	}
	if (c->n > 1)
		l = link_arg(c, 1);
	if (l >= S06_MAX_LINKS)
		return;
	if (strcmp(k, "send") == 0 && c->n == 5) {
		n = h_hex(c, 4, buf, sizeof buf);
		h_putu(s06_send(&node, l, (uint8_t)h_num(c, 2), (uint8_t)h_num(c, 3), buf, n));
	} else if (strcmp(k, "log") == 0 && c->n == 4) {
		n = h_hex(c, 3, buf, sizeof buf);
		h_putu(s06_send_log(&node, l, (uint8_t)h_num(c, 2), buf, n));
	} else if (strcmp(k, "req") == 0 && c->n == 5) {
		char s[16];

		n = h_hex(c, 3, buf, sizeof buf);
		snprintf(s, sizeof s, "%d", s06_request(&node, l, (uint8_t)h_num(c, 2), buf, n,
							 (uint32_t)h_num(c, 4)));
		h_put(s);
	} else if (strcmp(k, "in") == 0 && c->n == 3) {
		do_in(c, l);
		return;
	} else if (strcmp(k, "out") == 0) {
		h_puthex(port[l].out, port[l].out_n);
		port[l].out_n = 0;
	} else if (strcmp(k, "rawmode") == 0 && c->n == 3) {
		s06_set_raw(&node, l, h_num(c, 2) != 0u);
		h_put("ok");
	} else if (strcmp(k, "rawread") == 0) {
		n = s06_raw_read(&node, l, buf, sizeof buf);
		h_puthex(buf, n);
	} else if (strcmp(k, "rawwrite") == 0 && c->n == 3) {
		n = h_hex(c, 2, buf, sizeof buf);
		h_putu(s06_raw_write(&node, l, buf, n));
	} else if (strcmp(k, "rawin") == 0 && c->n == 3) {
		/* bytes arriving in raw mode: queued, read by rawread */
		struct port *p = &port[l];

		n = h_hex(c, 2, p->in + p->in_n, sizeof p->in - p->in_n);
		p->in_n += n;
		h_put("ok");
	} else if (strcmp(k, "seq") == 0) {
		h_putu(links[l].seq);
	} else {
		h_error(c, "unknown command");
		return;
	}
	h_end();
}

static void do_rx(const struct h_cmd *c)
{
	struct s06_rx rx;
	size_t n = h_hex(c, 2, buf, sizeof buf);
	static const char *const kind[] = { "", "deliver", "forward", "partial", "crc", "long", "fwdlong" };

	s06_rx_init(&rx, (uint8_t)h_num(c, 1));
	for (size_t i = 0; i < n; i++) {
		int ev = s06_rx_byte(&rx, buf[i], crc8);

		if (ev <= 0 || ev > 6)
			continue;
		h_put(kind[ev]);
		putchar(':');
		if (rx.n == 0)
			putchar('-');
		for (unsigned j = 0; j < rx.n; j++)
			printf("%02x", rx.buf[j]);
	}
	h_put("end");
	h_end();
}

int main(void)
{
	struct h_cmd c;
	uint8_t out[2 * S06_MAX_WIRE];

	crc_init();
	while (h_next(&c)) {
		const char *k = c.tok[0];
		size_t n;

		if (strcmp(k, "crc") == 0 && c.n == 2) {
			n = h_hex(&c, 1, buf, sizeof buf);
			h_putx(crc8(buf, n));
			h_end();
		} else if (strcmp(k, "cobs") == 0 && c.n == 2) {
			n = h_hex(&c, 1, buf, S06_MAX_RAW);
			h_puthex(out, s06_cobs_encode(buf, n, out));
			h_end();
		} else if ((strcmp(k, "raw") == 0 || strcmp(k, "encode") == 0) && c.n == 5) {
			n = h_hex(&c, 4, buf, S06_MAX_PAYLOAD);
			if (k[0] == 'r')
				n = s06_frame_raw((uint8_t)h_num(&c, 1), (uint8_t)h_num(&c, 2),
						  (uint8_t)h_num(&c, 3), buf, n, crc8, out);
			else
				n = s06_frame_encode((uint8_t)h_num(&c, 1), (uint8_t)h_num(&c, 2),
						     (uint8_t)h_num(&c, 3), buf, n, crc8, out);
			h_puthex(out, n);
			h_end();
		} else if (strcmp(k, "rx") == 0 && c.n == 3) {
			do_rx(&c);
		} else if (strcmp(k, "fornode") == 0 && c.n == 3) {
			h_putu(s06_rx_for_node((uint8_t)h_num(&c, 1), (uint8_t)h_num(&c, 2)));
			h_end();
		} else {
			node_cmd(&c, k);
		}
	}
	return h_status();
}
