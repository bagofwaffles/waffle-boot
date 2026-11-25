/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-06 links and node: S-06.002, S-06.010-S-06.013, S-06.020-S-06.022,
 * S-06.030-S-06.035, S-06.046, S-06.050-S-06.052, S-06.060-S-06.065, against
 * tests/oracle/S-06-framing/links.tsv, routing.tsv and the transcripts
 * request_reply.json and forward_host_psoc.json; without second-cpu
 * FR-NODE5-1F (S-06.010, S-06.011, S-06.050, S-06.051).
 */
#include "waffle/link/link.h"

#include "s06_test.h"
#include "s06_fake.h"
#include "vectors/s06_vectors.h"

static struct t_sim sim;
static struct t_port port[S06_MAX_LINKS];
static struct s06_link links[S06_MAX_LINKS];
static struct s06_node node;
static int drops[32];

static void on_drop(void *ctx, unsigned link, int why, const uint8_t *d,
		    size_t n)
{
	(void)ctx;
	(void)link;
	(void)d;
	(void)n;
	drops[why]++;
}

/* The master (or, slave = 1, the slave with its one link at index CPU). */
static void setup(int slave)
{
	static const uint8_t peer[S06_MAX_LINKS] = { 2, 2, 4, 3, 5 };

	t_sim_init(&sim, S06_MS(600000));
	s06_node_init(&node, t_crc8, &sim.clock);
	memset(drops, 0, sizeof drops);
	for (unsigned i = 0; i < S06_MAX_LINKS; i++) {
		t_port_init(&port[i], &sim);
		if (slave && i != S06_LINK_CPU)
			continue;
		s06_link_init(&links[i], &t_port_ops, &port[i],
			      slave ? 5 : 1, slave ? 1 : peer[i]);
		node.link[i] = &links[i];
	}
	if (!slave) {
		node.routes = s06_master_routes;
		node.nroutes = 6;
	}
	node.obs.dropped = on_drop;
}

static size_t tx_total(void)
{
	size_t n = 0;

	for (unsigned i = 0; i < S06_MAX_LINKS; i++)
		n += port[i].ntx;
	return n;
}

/* Decode the skip-th frame a port has written: 1, or 0 if none. */
static int first_tx(unsigned link, size_t skip, struct s06_frame *f)
{
	uint8_t raw[256];
	size_t n = t_nth_raw(port[link].tx, port[link].ntx, skip, raw);

	memset(f, 0, sizeof *f);
	if (n < 5)
		return 0;
	f->route = raw[0];
	f->seq = raw[1];
	f->id = raw[2];
	f->len = raw[3];
	memcpy(f->payload, raw + 4, raw[3]);
	return 1;
}

/* S-06.011, S-06.012: own and peer address, the route of a message. */
static void test_links(void)
{
	for (size_t i = 0; i < s06v_links_count; i++) {
		const struct s06v_link *v = &s06v_links[i];
		struct s06_frame f;

		t_case = "links.tsv";
		setup(v->slave);
		CHECK_EQ(links[v->link].own, v->own);
		CHECK_EQ(links[v->link].peer, v->peer);
		CHECK(s06_send(&node, (unsigned)v->link, 0, 0x7C, 0, 0));
		CHECK_EQ(first_tx((unsigned)v->link, 0, &f), 1);
		CHECK_EQ(f.route, v->route);
		/* a log line has the same route */
		CHECK(s06_send_log(&node, (unsigned)v->link, 0x03,
				   (const uint8_t *)"x", 1));
		CHECK_EQ(first_tx((unsigned)v->link, 1, &f), 1);
		CHECK_EQ(f.route, v->route);
	}
	/* S-06.012: a non-zero nibble is sent as it is */
	t_case = "defaults";
	setup(0);
	CHECK(s06_send(&node, S06_LINK_PSOC, 0x30, 0x7C, 0, 0));
	CHECK(s06_send(&node, S06_LINK_PSOC, 0x07, 0x7C, 0, 0));
	{
		struct s06_frame f;

		CHECK_EQ(first_tx(S06_LINK_PSOC, 0, &f), 1);
		CHECK_EQ(f.route, 0x34);
		CHECK_EQ(first_tx(S06_LINK_PSOC, 1, &f), 1);
		CHECK_EQ(f.route, 0x17);
	}
}

/* S-06.013, S-06.050, S-06.051: the verdict per arrival link and dst. */
static void test_routing(void)
{
	for (size_t i = 0; i < s06v_routes_count; i++) {
		const struct s06v_route *v = &s06v_routes[i];
		uint8_t wire[S06_MAX_WIRE];
		const uint8_t pl[2] = { 0xA1, 0x00 };
		uint8_t route = s06_route(9, v->dst);
		size_t w = s06_frame_encode(route, 0x21, 0x7A, pl, 2, t_crc8, wire);
		struct s06_frame f;
		char name[48];

		snprintf(name, sizeof name, "routing %s link %d dst %u",
			 v->slave ? "slave" : "master", v->link, v->dst);
		t_case = name;
		setup(v->slave);
		t_port_inject(&port[v->link], 0, wire, w);
		s06_node_poll(&node);
		if (v->verdict == -2) {
			CHECK(s06_link_take(&links[v->link], &f));
			CHECK_EQ(f.route, route);
			CHECK_EQ(tx_total(), 0);
		} else if (v->verdict == -1) {
			CHECK_EQ(tx_total(), 0);
			CHECK_EQ(drops[S06_DROP_NO_ROUTE], 1);
		} else {
			CHECK_MEM(port[v->verdict].tx, port[v->verdict].ntx, wire, w);
			CHECK_EQ(tx_total(), w);
		}
	}
}

/* S-06.046: forwarded unchanged and unchecked, whole or absent. */
static void test_forward_whole(void)
{
	uint8_t wire[S06_MAX_WIRE];
	size_t w = s06_frame_encode(0x24, 3, 0x41, 0, 0, t_crc8, wire);

	t_case = "forward-bad-check";
	setup(0);
	wire[w - 2] ^= 0x01;			/* the check byte */
	t_port_inject(&port[S06_LINK_CDC], 0, wire, w);
	s06_node_poll(&node);
	CHECK_MEM(port[S06_LINK_PSOC].tx, port[S06_LINK_PSOC].ntx, wire, w);

	t_case = "forward-no-room";
	setup(0);
	port[S06_LINK_PSOC].room = w - 1;
	t_port_inject(&port[S06_LINK_CDC], 0, wire, w);
	s06_node_poll(&node);
	CHECK_EQ(port[S06_LINK_PSOC].ntx, 0);
	CHECK_EQ(drops[S06_DROP_NO_ROOM], 1);
}

/* The node's forward_ok, when set, decides each forward: refused, the frame
 * is dropped as such and nothing is sent; with it unset every frame goes. */
static int fw_calls;
static unsigned fw_link;
static uint8_t fw_dst;

static bool fw_refuse(void *ctx, unsigned link, uint8_t dst, const uint8_t *wire, size_t n)
{
	(void)ctx;
	(void)wire;
	(void)n;
	fw_calls++;
	fw_link = link;
	fw_dst = dst;
	return false;
}

static void test_forward_refused(void)
{
	uint8_t wire[S06_MAX_WIRE];
	size_t w = s06_frame_encode(0x24, 3, 0x41, 0, 0, t_crc8, wire);

	t_case = "forward-refused";
	setup(0);
	fw_calls = 0;
	node.obs.forward_ok = fw_refuse;
	t_port_inject(&port[S06_LINK_CDC], 0, wire, w);
	s06_node_poll(&node);
	CHECK_EQ(port[S06_LINK_PSOC].ntx, 0);
	CHECK_EQ(drops[S06_DROP_REFUSED], 1);
	CHECK_EQ(fw_calls, 1);
	CHECK_EQ(fw_link, S06_LINK_CDC);
	CHECK_EQ(fw_dst, 4);
	node.obs.forward_ok = NULL;
}

/* S-06.020-S-06.022: one counter per link; forwarding uses none. */
static void test_seq(void)
{
	struct s06_frame f;
	uint8_t wire[S06_MAX_WIRE];
	size_t w;

	t_case = "seq-counter";
	setup(0);
	for (int i = 0; i < 300; i++)
		(void)s06_send(&node, S06_LINK_PSOC, 0, 0x40, 0, 0);
	CHECK_EQ(links[S06_LINK_PSOC].seq, 300 % 256);
	CHECK_EQ(first_tx(S06_LINK_PSOC, 255, &f), 1);
	CHECK_EQ(f.seq, 255);
	CHECK_EQ(first_tx(S06_LINK_PSOC, 256, &f), 1);
	CHECK_EQ(f.seq, 0);
	CHECK_EQ(links[S06_LINK_NORDIC].seq, 0);	/* per link */
	/* requests and log lines count too */
	CHECK(s06_request(&node, S06_LINK_PSOC, 0x41, 0, 0, 100) >= 0);
	CHECK(s06_send_log(&node, S06_LINK_PSOC, 0x03, 0, 0));
	CHECK_EQ(links[S06_LINK_PSOC].seq, (300 + 2) % 256);

	t_case = "seq-forward";
	setup(0);
	w = s06_frame_encode(0x24, 0x77, 0x41, 0, 0, t_crc8, wire);
	t_port_inject(&port[S06_LINK_CDC], 0, wire, w);
	s06_node_poll(&node);
	CHECK_EQ(links[S06_LINK_PSOC].seq, 0);
	CHECK(s06_send(&node, S06_LINK_PSOC, 0, 0x40, 0, 0));
	CHECK_EQ(first_tx(S06_LINK_PSOC, 1, &f), 1);
	CHECK_EQ(f.seq, 0);

	/* S-14.027 via S-06.020: a log line not sent uses no number */
	t_case = "seq-log-full";
	setup(0);
	port[S06_LINK_PSOC].room = 3;
	CHECK(!s06_send_log(&node, S06_LINK_PSOC, 0x03, 0, 0));
	CHECK_EQ(links[S06_LINK_PSOC].seq, 0);
	/* S-16.147: a message not sent may use one */
	CHECK(!s06_send(&node, S06_LINK_PSOC, 0, 0x40, 0, 0));
	CHECK_EQ(links[S06_LINK_PSOC].seq, 1);
	t_case = "seq-log-raw";
	setup(0);
	s06_set_raw(&node, S06_LINK_PSOC, true);
	CHECK(!s06_send_log(&node, S06_LINK_PSOC, 0x03, 0, 0));
	CHECK_EQ(links[S06_LINK_PSOC].seq, 0);

	/* S-06.002: a payload over 249 bytes is not sent at all */
	t_case = "payload-limit";
	setup(0);
	{
		static const uint8_t big[250];

		CHECK(!s06_send(&node, S06_LINK_PSOC, 0, 0x40, big, 250));
		CHECK_EQ(port[S06_LINK_PSOC].ntx, 0);
		CHECK(s06_send(&node, S06_LINK_PSOC, 0, 0x40, big, 249));
		CHECK_EQ(port[S06_LINK_PSOC].ntx, 256);
	}
}

static void reply_in(unsigned link, uint32_t at, uint8_t route, uint8_t id)
{
	t_port_inject_frame(&port[link], at, route, 0x5A, id, 0, 0);
}

/* S-06.030-S-06.035 */
static void test_requests(void)
{
	struct s06_frame f;
	int slot, s2;

	t_case = "slot-and-reply";
	setup(0);
	slot = s06_request(&node, S06_LINK_PSOC, 0x7A, 0, 0, S06_MS(100));
	CHECK(slot >= 0 && slot < 4);
	CHECK_EQ(first_tx(S06_LINK_PSOC, 0, &f), 1);
	CHECK_EQ(f.route, (uint8_t)((8 + slot) << 4 | 4));
	/* S-06.032: id, seq and source not compared */
	reply_in(S06_LINK_PSOC, 0, (uint8_t)(0x70 | (8 + slot)), 0x55);
	s06_node_poll(&node);
	CHECK_EQ(s06_request_state(&node, S06_LINK_PSOC, slot), S06_SLOT_DONE);
	s06_request_collect(&node, S06_LINK_PSOC, slot, &f);
	CHECK_EQ(f.id, 0x55);
	CHECK(!s06_link_take(&links[S06_LINK_PSOC], &f));

	t_case = "slots-in-use";
	setup(0);
	for (int i = 0; i < 4; i++)
		CHECK(s06_request(&node, S06_LINK_PSOC, 0x7A, 0, 0, 1000) >= 0);
	{
		size_t before = port[S06_LINK_PSOC].ntx;

		CHECK_EQ(s06_request(&node, S06_LINK_PSOC, 0x7A, 0, 0, 1000), -1);
		CHECK_EQ(port[S06_LINK_PSOC].ntx, before);
	}
	/* a request on another link has its own four */
	CHECK(s06_request(&node, S06_LINK_NORDIC, 0x7A, 0, 0, 1000) >= 0);

	t_case = "per-link";
	setup(0);
	slot = s06_request(&node, S06_LINK_PSOC, 0x7A, 0, 0, S06_MS(100));
	reply_in(S06_LINK_NORDIC, 0, (uint8_t)(0x30 | (8 + slot)), 0x7B);
	s06_node_poll(&node);
	CHECK_EQ(s06_request_state(&node, S06_LINK_PSOC, slot), S06_SLOT_PENDING);
	CHECK(s06_link_take(&links[S06_LINK_NORDIC], &f));	/* ordinary */

	/* S-06.033, S-06.034 */
	t_case = "no-pending-and-dst-12-15";
	setup(0);
	reply_in(S06_LINK_PSOC, 0, 0x48, 0x7B);
	reply_in(S06_LINK_PSOC, 0, 0x4C, 0x7B);
	reply_in(S06_LINK_PSOC, 0, 0x4F, 0x7B);
	s06_node_poll(&node);
	CHECK(s06_link_take(&links[S06_LINK_PSOC], &f));
	CHECK(s06_link_take(&links[S06_LINK_PSOC], &f));
	CHECK(s06_link_take(&links[S06_LINK_PSOC], &f));
	slot = s06_request(&node, S06_LINK_PSOC, 0x7A, 0, 0, S06_MS(100));
	CHECK_EQ(slot, 0);
	reply_in(S06_LINK_PSOC, 0, 0x4C, 0x7B);		/* S-16.144: not slot 0 */
	s06_node_poll(&node);
	CHECK_EQ(s06_request_state(&node, S06_LINK_PSOC, 0), S06_SLOT_PENDING);
	CHECK(s06_link_take(&links[S06_LINK_PSOC], &f));

	/* S-06.035: timeout frees the slot; a late reply is ordinary, or
	 * completes a later request in the same slot (S-16.145) */
	t_case = "late-reply";
	setup(0);
	CHECK_EQ(s06_transact(&node, S06_LINK_PSOC, 0x7A, 0, 0, S06_MS(100), &f), 0);
	CHECK(sim.now >= S06_MS(100));
	reply_in(S06_LINK_PSOC, sim.now + 5, 0x48, 0x7B);
	sim.now += 10;
	s06_node_poll(&node);
	CHECK(s06_link_take(&links[S06_LINK_PSOC], &f));
	setup(0);
	slot = s06_request(&node, S06_LINK_PSOC, 0x7A, 0, 0, S06_MS(100));
	sim.now += S06_MS(101);
	CHECK_EQ(s06_request_state(&node, S06_LINK_PSOC, slot), S06_SLOT_FREE);
	for (int i = 0; i < 3; i++)	/* walk round to the same slot */
		s06_request_collect(&node, S06_LINK_PSOC,
				    s06_request(&node, S06_LINK_PSOC, 1, 0, 0, 0), 0);
	s2 = s06_request(&node, S06_LINK_PSOC, 0x7C, 0, 0, S06_MS(100));
	CHECK_EQ(s2, slot);
	reply_in(S06_LINK_PSOC, sim.now, (uint8_t)(0x40 | (8 + slot)), 0x7B);
	s06_node_poll(&node);
	CHECK_EQ(s06_request_state(&node, S06_LINK_PSOC, s2), S06_SLOT_DONE);

	/* the reply within the wait is used (transact) */
	t_case = "transact";
	setup(0);
	reply_in(S06_LINK_PSOC, S06_MS(80), 0x48, 0x41);
	CHECK_EQ(s06_transact(&node, S06_LINK_PSOC, 0x41, 0, 0, S06_MS(100), &f), 1);
	CHECK_EQ(f.id, 0x41);
	CHECK_EQ(sim.now, S06_MS(80));
}

/* S-06.052: a reply-slot destination stays local; source 2 round-trips. */
static void test_host_paths(void)
{
	uint8_t wire[S06_MAX_WIRE];
	size_t w;
	struct s06_frame f;

	t_case = "host-reply-slot-stays-local";
	setup(0);
	w = s06_frame_encode(0x94, 1, 0x7A, 0, 0, t_crc8, wire);
	t_port_inject(&port[S06_LINK_CDC], 0, wire, w);
	s06_node_poll(&node);
	CHECK_MEM(port[S06_LINK_PSOC].tx, port[S06_LINK_PSOC].ntx, wire, w);
	reply_in(S06_LINK_PSOC, 0, 0x49, 0x7B);
	s06_node_poll(&node);
	CHECK_EQ(port[S06_LINK_CDC].ntx, 0);
	CHECK(s06_link_take(&links[S06_LINK_PSOC], &f));

	/* forward_host_psoc.json: 0x24 seq 33 id 7a {0a 0b}, answer 0x42 seq 5 */
	t_case = "forward_host_psoc.json";
	setup(0);
	{
		const uint8_t p1[2] = { 0x0A, 0x0B }, p2[1] = { 0x01 };
		uint8_t w2[S06_MAX_WIRE];
		size_t n2;

		w = s06_frame_encode(0x24, 33, 0x7A, p1, 2, t_crc8, wire);
		n2 = s06_frame_encode(0x42, 5, 0x7B, p2, 1, t_crc8, w2);
		t_port_inject(&port[S06_LINK_CDC], 0, wire, w);
		s06_node_poll(&node);
		CHECK_MEM(port[S06_LINK_PSOC].tx, port[S06_LINK_PSOC].ntx, wire, w);
		t_port_inject(&port[S06_LINK_PSOC], 0, w2, n2);
		s06_node_poll(&node);
		CHECK_MEM(port[S06_LINK_CDC].tx, port[S06_LINK_CDC].ntx, w2, n2);
	}
}

/* A held link is not read: a frame for the PSoC waits in the port, not
 * forwarded and not dropped, and goes on when the link is let go. */
static void test_hold(void)
{
	uint8_t wire[S06_MAX_WIRE], raw[8];
	size_t w;

	t_case = "held link";
	setup(0);
	w = s06_frame_encode(0x24, 1, 0x7A, 0, 0, t_crc8, wire);
	t_port_inject(&port[S06_LINK_CDC], 0, wire, w);
	s06_link_hold(&links[S06_LINK_CDC], true);
	s06_node_poll(&node);
	s06_node_poll_link(&node, S06_LINK_CDC);
	CHECK_EQ(port[S06_LINK_PSOC].ntx, 0);
	CHECK_EQ(port[S06_LINK_CDC].rx_r, 0);	/* nothing read */
	/* raw mode reads nothing of it either */
	s06_set_raw(&node, S06_LINK_CDC, true);
	CHECK_EQ(s06_raw_read(&node, S06_LINK_CDC, raw, sizeof raw), 0);
	s06_set_raw(&node, S06_LINK_CDC, false);
	CHECK_EQ(port[S06_LINK_CDC].rx_r, 0);
	/* sending is not affected */
	CHECK(s06_send(&node, S06_LINK_CDC, 0, 0x30, 0, 0));
	s06_link_hold(&links[S06_LINK_CDC], false);
	s06_node_poll(&node);
	CHECK_MEM(port[S06_LINK_PSOC].tx, port[S06_LINK_PSOC].ntx, wire, w);
	CHECK_EQ(drops[S06_DROP_NO_ROUTE] + drops[S06_DROP_INBOX], 0);
}

/* A paced link reads only as far as its inbox has room: ten messages sent
 * back to back with no delimiter between them (S-06.041, the fewest bytes
 * each) are all taken, in order, none dropped. An unpaced link drops what
 * does not fit. */
static void test_paced(void)
{
	uint8_t wire[S06_MAX_WIRE];
	struct s06_frame f;
	unsigned i, next = 0;

	for (int paced = 1; paced >= 0; paced--) {
		t_case = paced ? "paced link" : "unpaced link";
		setup(0);
		links[S06_LINK_CDC].paced = paced != 0;
		for (i = 0; i < 10u; i++) {
			const size_t w = s06_frame_encode(0x21, (uint8_t)i, 0x01,
							  0, 0, t_crc8, wire);

			CHECK_EQ(w, 7);
			t_port_inject(&port[S06_LINK_CDC], 0, wire, w - 1u);
		}
		s06_node_poll(&node);
		CHECK_EQ(links[S06_LINK_CDC].in_count, 8);
		if (!paced) {
			CHECK_EQ(links[S06_LINK_CDC].inbox_lost, 2);
			continue;
		}
		CHECK_EQ(links[S06_LINK_CDC].inbox_lost, 0);
		CHECK(port[S06_LINK_CDC].rx_r < port[S06_LINK_CDC].rx_w);
		next = 0;
		for (unsigned round = 0; round < 4u; round++) {
			while (s06_link_take(&links[S06_LINK_CDC], &f)) {
				CHECK_EQ(f.seq, next);
				next++;
			}
			s06_node_poll(&node);
		}
		CHECK_EQ(next, 10);
		CHECK_EQ(links[S06_LINK_CDC].inbox_lost, 0);
		CHECK_EQ(drops[S06_DROP_INBOX], 0);
	}
}

/* A delivered frame carries the start of its link's last poll that read the
 * port empty: its delimiter arrived after that. A paced poll that stops
 * for a full inbox has not read the port empty and moves nothing. */
static void test_arrival(void)
{
	uint8_t wire[S06_MAX_WIRE];
	struct s06_frame f;
	const size_t w = s06_frame_encode(0x31, 0, 0x05, 0, 0, t_crc8, wire);

	t_case = "arrival time";
	setup(0);
	sim.now = 1000;
	s06_node_poll(&node);			/* empty at 1 ms */
	CHECK_EQ(links[S06_LINK_NORDIC].read_at, 1000);
	t_port_inject(&port[S06_LINK_NORDIC], 5000, wire, w);
	sim.now = 9000;
	s06_node_poll(&node);
	CHECK(s06_link_take(&links[S06_LINK_NORDIC], &f));
	CHECK_EQ(f.at, 1000);
	CHECK_EQ(links[S06_LINK_NORDIC].read_at, 9000);
	t_port_inject(&port[S06_LINK_NORDIC], 9500, wire, w);
	sim.now = 9500;
	s06_node_poll(&node);
	CHECK(s06_link_take(&links[S06_LINK_NORDIC], &f));
	CHECK_EQ(f.at, 9000);
	/* a reply carries it too */
	CHECK_EQ(s06_request(&node, S06_LINK_NORDIC, 0x09, 0, 0, S06_MS(100)), 0);
	t_port_inject_frame(&port[S06_LINK_NORDIC], 9700, 0x38, 0, 0x0A, 0, 0);
	sim.now = 9800;
	s06_node_poll(&node);
	CHECK_EQ(links[S06_LINK_NORDIC].slot[0].state, S06_SLOT_DONE);
	CHECK_EQ(links[S06_LINK_NORDIC].slot[0].reply.at, 9500);

	t_case = "arrival time, paced";
	setup(0);
	links[S06_LINK_CDC].paced = true;
	for (unsigned i = 0; i < 9u; i++)
		t_port_inject_frame(&port[S06_LINK_CDC], 0, 0x21, (uint8_t)i, 0x01, 0, 0);
	sim.now = 2000;
	s06_node_poll(&node);			/* stops with the inbox full */
	CHECK_EQ(links[S06_LINK_CDC].in_count, 8);
	CHECK_EQ(links[S06_LINK_CDC].read_at, 0);
	while (s06_link_take(&links[S06_LINK_CDC], &f))
		;
	sim.now = 3000;
	s06_node_poll(&node);
	CHECK(s06_link_take(&links[S06_LINK_CDC], &f));
	CHECK_EQ(f.seq, 8);
	CHECK_EQ(f.at, 0);
	CHECK_EQ(links[S06_LINK_CDC].read_at, 3000);
}

/* request_reply.json: request from slot source, reply, plain, inject, plain */
static void test_transcript_request_reply(void)
{
	const uint8_t p[2] = { 0x0A, 0x0B }, r[1] = { 0x01 }, inj[3] = { 0, 0xFF, 0 };
	struct s06_frame f;
	int slot;

	t_case = "request_reply.json";
	setup(0);
	slot = s06_request(&node, S06_LINK_PSOC, 0x7A, p, 2, S06_MS(100));
	CHECK_EQ(first_tx(S06_LINK_PSOC, 0, &f), 1);
	CHECK(s06_src(f.route) >= 8 && s06_src(f.route) <= 11);
	CHECK_EQ(s06_dst(f.route), 4);
	CHECK_EQ(f.seq, 0);
	t_port_inject_frame(&port[S06_LINK_PSOC], 1, s06_swap_route(f.route), 0,
			    0x7B, r, 1);
	sim.now = 1;
	s06_node_poll(&node);
	CHECK_EQ(s06_request_state(&node, S06_LINK_PSOC, slot), S06_SLOT_DONE);
	s06_request_collect(&node, S06_LINK_PSOC, slot, 0);
	CHECK(s06_send(&node, S06_LINK_PSOC, 0, 0x7C, p, 2));
	t_port_inject_frame(&port[S06_LINK_PSOC], 2, 0x41, 17, 0x7D, inj, 3);
	sim.now = 2;
	s06_node_poll(&node);
	CHECK(s06_link_take(&links[S06_LINK_PSOC], &f));
	CHECK_EQ(f.id, 0x7D);
	CHECK(s06_send(&node, S06_LINK_PSOC, 0, 0x7E, 0, 0));
	CHECK_EQ(first_tx(S06_LINK_PSOC, 1, &f), 1);
	CHECK_EQ(f.route, 0x14);
	CHECK_EQ(f.seq, 1);
	CHECK_EQ(first_tx(S06_LINK_PSOC, 2, &f), 1);
	CHECK_EQ(f.route, 0x14);
	CHECK_EQ(f.seq, 2);
}

/* S-06.060-S-06.065 */
static void test_raw(void)
{
	uint8_t wire[S06_MAX_WIRE];
	const uint8_t exitp[7] = { 0x01, 0x3B, 0x00, 0x00, 0xC4, 0xFF, 0x17 };
	struct s06_frame f;
	size_t w;

	t_case = "raw-entry-discards-partial";
	setup(0);
	w = s06_frame_encode(0x41, 0, 0x42, 0, 0, t_crc8, wire);
	t_port_inject(&port[S06_LINK_PSOC], 0, wire, 3);
	s06_node_poll(&node);
	s06_set_raw(&node, S06_LINK_PSOC, true);
	s06_set_raw(&node, S06_LINK_PSOC, false);
	t_port_inject(&port[S06_LINK_PSOC], 0, wire, w);
	s06_node_poll(&node);
	CHECK(s06_link_take(&links[S06_LINK_PSOC], &f));
	CHECK(!s06_link_take(&links[S06_LINK_PSOC], &f));

	t_case = "raw-bytes-uninterpreted";
	setup(0);
	s06_set_raw(&node, S06_LINK_PSOC, true);
	t_port_inject(&port[S06_LINK_PSOC], 0, wire, w);
	s06_node_poll(&node);
	CHECK(!s06_link_take(&links[S06_LINK_PSOC], &f));
	{
		uint8_t buf[64];
		size_t got = s06_raw_read(&node, S06_LINK_PSOC, buf, sizeof buf);

		CHECK_MEM(buf, got, wire, w);
	}

	t_case = "raw-no-frames-sent";
	CHECK(!s06_send(&node, S06_LINK_PSOC, 0, 0x40, 0, 0));
	CHECK(s06_raw_write(&node, S06_LINK_PSOC, exitp, 7));
	CHECK_MEM(port[S06_LINK_PSOC].tx, port[S06_LINK_PSOC].ntx, exitp, 7);

	t_case = "raw-after-exit";
	t_port_inject(&port[S06_LINK_PSOC], 0, exitp, 7);	/* unread */
	s06_set_raw(&node, S06_LINK_PSOC, false);
	t_port_inject(&port[S06_LINK_PSOC], 0, wire, w);
	s06_node_poll(&node);
	CHECK(s06_link_take(&links[S06_LINK_PSOC], &f));
	CHECK(!s06_link_take(&links[S06_LINK_PSOC], &f));

	t_case = "forward-into-raw-link";
	setup(0);
	s06_set_raw(&node, S06_LINK_PSOC, true);
	w = s06_frame_encode(0x24, 0, 0x42, 0, 0, t_crc8, wire);
	t_port_inject(&port[S06_LINK_CDC], 0, wire, w);
	s06_node_poll(&node);
	CHECK_EQ(port[S06_LINK_PSOC].ntx, 0);
	CHECK_EQ(drops[S06_DROP_NO_ROUTE], 1);

	/* S-06.065: whole, between frames, no number */
	t_case = "raw-bytes-on-framed-link";
	setup(0);
	CHECK(s06_send(&node, S06_LINK_PSOC, 0, 0x05, 0, 0));
	w = port[S06_LINK_PSOC].ntx;
	CHECK(s06_raw_write(&node, S06_LINK_PSOC, exitp, 7));
	CHECK(s06_send(&node, S06_LINK_PSOC, 0, 0x30, 0, 0));
	CHECK_MEM(port[S06_LINK_PSOC].tx + w, 7, exitp, 7);
	CHECK_EQ(port[S06_LINK_PSOC].tx[w - 1], 0);
	CHECK_EQ(links[S06_LINK_PSOC].seq, 2);
	{
		/* the second frame starts right after the string */
		struct s06_rx rx;
		int delivered = 0;

		s06_rx_init(&rx, 4);
		for (size_t i = w + 7; i < port[S06_LINK_PSOC].ntx; i++)
			if (s06_rx_byte(&rx, port[S06_LINK_PSOC].tx[i], t_crc8) ==
			    S06_RX_DELIVER) {
				delivered++;
				CHECK_EQ(rx.buf[1], 1);
			}
		CHECK_EQ(delivered, 1);
	}
}

/* FR-NODE5-1F: the OP-1F's main CPU, no link to a slave; address 5 is in
 * no row on any link, the other rows as on the OP-XY. */
static void test_node5_1f(void)
{
	static const struct { unsigned in; uint8_t dst; int out; } rows[] = {
		{ S06_LINK_MIDI, 5, -1 }, { S06_LINK_MIDI, 4, -1 }, { S06_LINK_MIDI, 3, -1 },
		{ S06_LINK_CDC, 5, -1 }, { S06_LINK_CDC, 4, S06_LINK_PSOC },
		{ S06_LINK_CDC, 3, S06_LINK_NORDIC }, { S06_LINK_PSOC, 5, -1 },
		{ S06_LINK_NORDIC, 5, -1 }, { S06_LINK_PSOC, 2, S06_LINK_CDC },
	};

	for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
		uint8_t wire[S06_MAX_WIRE];
		const uint8_t pl[2] = { 0x0A, 0x0B };
		size_t w = s06_frame_encode(s06_route(2, rows[i].dst), 0x21, 0x7A, pl, 2, t_crc8,
					    wire);

		t_case = "node5-1f";
		setup(0);
		node.link[S06_LINK_CPU] = NULL;
		s06_node_master_routes(&node, &wb_device_op1f);
		t_port_inject(&port[rows[i].in], 0, wire, w);
		s06_node_poll(&node);
		if (rows[i].out < 0) {
			CHECK_EQ(tx_total(), 0);
			CHECK_EQ(drops[S06_DROP_NO_ROUTE], 1);
		} else {
			CHECK_MEM(port[rows[i].out].tx, port[rows[i].out].ntx, wire, w);
			CHECK_EQ(tx_total(), w);
		}
	}
	/* the OP-XY: the same call puts every row in force */
	setup(0);
	s06_node_master_routes(&node, &wb_device_opxy);
	CHECK_EQ(node.routes_off, 0u);
	CHECK_EQ(node.nroutes, 6u);
}

int main(void)
{
	test_links();
	test_routing();
	test_forward_whole();
	test_forward_refused();
	test_seq();
	test_requests();
	test_host_paths();
	test_hold();
	test_paced();
	test_arrival();
	test_transcript_request_reply();
	test_raw();
	test_node5_1f();
	return t_finish("test_s06_link");
}
