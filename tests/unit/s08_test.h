/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-08 test rig: a master node with the Nordic link (and the CDC link, for
 * forwarding) on fake ports, the S-08 module on it, and a reference
 * CRC-16/XMODEM written from S-08.044's parameters.
 */
#ifndef S08_TEST_H
#define S08_TEST_H

#include "waffle/nordic/nordic.h"
#include "waffle/link/bytes_compat.h"

#include "s06_test.h"
#include "s06_fake.h"

/* S-08.044: polynomial 0x1021, init 0, not reflected, no final XOR. */
static inline uint16_t t_crc16_xmodem(const uint8_t *d, size_t n)
{
	uint16_t c = 0;

	for (size_t i = 0; i < n; i++) {
		c = (uint16_t)(c ^ (uint16_t)(d[i] << 8));
		for (int k = 0; k < 8; k++)
			c = (uint16_t)((c & 0x8000u) ? ((unsigned)c << 1) ^ 0x1021u : (unsigned)c << 1);
	}
	return c;
}

struct t_nrig {
	struct t_sim sim;
	struct t_port port, cdc;
	struct s06_link link, cdc_link;
	struct s06_node node;
	struct s08_ops ops;
	struct s08_nordic nordic;
	int revision;
	int answers;		/* messages the module sent in answer */
};

static struct t_nrig nrig;

static inline void t_nrig_revision(void *ctx, uint8_t rev)
{
	((struct t_nrig *)ctx)->revision = rev;
}

static inline void t_nrig_init(int state, uint32_t cap_us)
{
	memset(&nrig, 0, sizeof nrig);
	t_sim_init(&nrig.sim, cap_us);
	t_port_init(&nrig.port, &nrig.sim);
	t_port_init(&nrig.cdc, &nrig.sim);
	s06_node_init(&nrig.node, t_crc8, &nrig.sim.clock);
	s06_link_init(&nrig.link, &t_port_ops, &nrig.port, S06_ADDR_MASTER,
		      S06_ADDR_NORDIC);
	s06_link_init(&nrig.cdc_link, &t_port_ops, &nrig.cdc, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	nrig.node.link[S06_LINK_NORDIC] = &nrig.link;
	nrig.node.link[S06_LINK_CDC] = &nrig.cdc_link;
	nrig.node.routes = s06_master_routes;
	nrig.node.nroutes = 6;
	nrig.revision = -1;
	nrig.ops.ctx = &nrig;
	nrig.ops.revision = t_nrig_revision;
	nrig.ops.crc16 = t_crc16_xmodem;
	s08_init(&nrig.nordic, &nrig.node, S06_LINK_NORDIC, &nrig.ops, state);
}

#endif
