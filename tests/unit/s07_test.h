/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07 test rig: a master node with a PSoC link on a fake port, the S-07
 * module on it, a reference CRC-16/X-25 written from S-07.082's parameters,
 * and a reader of what the master wrote (frames and raw writes, in order).
 */
#ifndef S07_TEST_H
#define S07_TEST_H

#include "waffle/s07_psoc.h"

#include "s06_test.h"
#include "s06_fake.h"

/* One item the master wrote: a frame or a raw write. */
struct t_item {
	int raw;
	uint32_t t;
	uint8_t route, id, len;
	uint8_t d[256];
};

#define T_MAX_ITEMS 4096

struct t_rig {
	struct t_sim sim;
	struct t_port port;
	struct s06_link link;
	struct s06_node node;
	struct s07_ops ops;
	struct s07_psoc psoc;
	struct t_item item[T_MAX_ITEMS];
	size_t nitems;
	/* key events */
	uint8_t kev[512][2];
	size_t nkev;
	/* called after each item is recorded */
	void (*on_item)(const struct t_rig *r, const struct t_item *it);
};

static struct t_rig rig;

static inline void t_rig_tx(void *ctx, struct t_port *p, const uint8_t *d, size_t n)
{
	struct t_rig *r = ctx;
	struct t_item *it;

	(void)p;
	if (r->nitems >= T_MAX_ITEMS || n > sizeof it->d) {
		fprintf(stderr, "t_rig: too many items\n");
		exit(2);
	}
	it = &r->item[r->nitems++];
	memset(it, 0, sizeof *it);
	it->t = r->sim.now;
	/* a frame write ends with its delimiter; a raw write here never does */
	if (d[n - 1] == 0x00) {
		uint8_t raw[256];
		size_t k = t_nth_raw(d, n, 0, raw);

		if (k < 5) {
			fprintf(stderr, "t_rig: bad frame write\n");
			exit(2);
		}
		it->route = raw[0];
		it->id = raw[2];
		it->len = raw[3];
		memcpy(it->d, raw + 4, raw[3]);
	} else {
		it->raw = 1;
		it->len = (uint8_t)n;
		memcpy(it->d, d, n);
	}
	if (r->on_item)
		r->on_item(r, it);
}

static inline void t_rig_key(void *ctx, uint8_t key, bool down)
{
	struct t_rig *r = ctx;

	if (r->nkev < 512) {
		r->kev[r->nkev][0] = key;
		r->kev[r->nkev][1] = down;
		r->nkev++;
	}
}

static inline void t_rig_init(uint32_t cap_us)
{
	memset(&rig, 0, sizeof rig);
	t_sim_init(&rig.sim, cap_us);
	t_port_init(&rig.port, &rig.sim);
	rig.port.on_tx = t_rig_tx;
	rig.port.tx_ctx = &rig;
	s06_node_init(&rig.node, t_crc8, &rig.sim.clock);
	s06_link_init(&rig.link, &t_port_ops, &rig.port, S06_ADDR_MASTER,
		      S06_ADDR_PSOC);
	rig.node.link[S06_LINK_PSOC] = &rig.link;
	rig.node.routes = s06_master_routes;
	rig.node.nroutes = 6;
	rig.ops.ctx = &rig;
	rig.ops.key_event = t_rig_key;
	rig.ops.crc16 = t_crc16_x25;
	s07_init(&rig.psoc, &rig.node, S06_LINK_PSOC, &rig.ops);
}

static uint8_t t_rig_seq;

/* A frame from the PSoC arriving at time at. */
static inline void t_rig_from_psoc(uint32_t at, uint8_t route, uint8_t id,
			    const uint8_t *p, size_t n)
{
	t_port_inject_frame(&rig.port, at, route, t_rig_seq++, id, p, n);
}

#endif
