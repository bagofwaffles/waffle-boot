/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Fakes for the serial protocol tests: a virtual clock and byte ports whose
 * received bytes carry arrival times. idle() moves the clock to the next
 * arrival or to its deadline, so a wait loop never spins; a cap on virtual
 * time fails the test instead of hanging it.
 */
#ifndef S06_FAKE_H
#define S06_FAKE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "waffle/link/clock.h"
#include "waffle/link/link.h"

#define T_PORT_RX 32768
#define T_PORT_TX 65536
#define T_MAX_PORTS 6

struct t_sim;

struct t_port {
	struct t_sim *sim;
	uint8_t rx[T_PORT_RX];
	uint32_t rxt[T_PORT_RX];
	size_t rx_r, rx_w;
	uint8_t tx[T_PORT_TX];
	size_t ntx;
	size_t room;		/* bytes the transmit side takes; SIZE_MAX: any */
	/* called for every write, with the time it happened */
	void (*on_tx)(void *ctx, struct t_port *p, const uint8_t *d, size_t n);
	void *tx_ctx;
};

struct t_sim {
	uint32_t now;
	uint32_t cap;		/* virtual-time limit of a test */
	struct t_port *port[T_MAX_PORTS];
	int nports;
	void (*tick)(void *ctx);	/* called on every idle, after time moved */
	void *tick_ctx;
	struct s06_clock clock;
};

static inline uint32_t t_sim_now(void *ctx)
{
	return ((struct t_sim *)ctx)->now;
}

static inline void t_sim_idle(void *ctx, uint32_t until)
{
	struct t_sim *s = ctx;
	uint32_t target = until;

	for (int i = 0; i < s->nports; i++) {
		struct t_port *p = s->port[i];

		if (p->rx_r == p->rx_w)
			continue;
		if (s06_time_reached(s->now, p->rxt[p->rx_r]))
			return;		/* input is waiting */
		if ((int32_t)(p->rxt[p->rx_r] - target) < 0)
			target = p->rxt[p->rx_r];
	}
	if (s06_time_reached(s->now, target))
		target = s->now + 1u;
	s->now = target;
	if (s06_time_reached(s->now, s->cap)) {
		fprintf(stderr, "virtual time cap %u us reached\n", s->cap);
		exit(2);
	}
	if (s->tick)
		s->tick(s->tick_ctx);
}

static inline void t_sim_init(struct t_sim *s, uint32_t cap_us)
{
	memset(s, 0, sizeof *s);
	s->cap = cap_us;
	s->clock.now_us = t_sim_now;
	s->clock.idle = t_sim_idle;
	s->clock.ctx = s;
}

static inline size_t t_port_read(void *ctx, uint8_t *buf, size_t max)
{
	struct t_port *p = ctx;
	size_t n = 0;

	while (n < max && p->rx_r != p->rx_w &&
	       s06_time_reached(p->sim->now, p->rxt[p->rx_r]))
		buf[n++] = p->rx[p->rx_r++];
	return n;
}

/* cppcheck-suppress constParameterCallback ; s06_port_ops fixes the type */
static inline size_t t_port_room(void *ctx)
{
	const struct t_port *p = ctx;
	size_t left = T_PORT_TX - p->ntx;

	return p->room < left ? p->room : left;
}

static inline void t_port_write(void *ctx, const uint8_t *d, size_t n)
{
	struct t_port *p = ctx;

	memcpy(p->tx + p->ntx, d, n);
	p->ntx += n;
	if (p->room != SIZE_MAX)
		p->room -= n;
	if (p->on_tx)
		p->on_tx(p->tx_ctx, p, d, n);
}

static const struct s06_port_ops t_port_ops = {
	t_port_read, t_port_room, t_port_write,
};

static inline void t_port_init(struct t_port *p, struct t_sim *s)
{
	memset(p, 0, sizeof *p);
	p->sim = s;
	p->room = SIZE_MAX;
	s->port[s->nports++] = p;
}

/* Bytes that arrive at time at, kept in arrival order (after any bytes
 * already queued for the same time). */
static inline void t_port_inject(struct t_port *p, uint32_t at, const uint8_t *d,
				 size_t n)
{
	size_t pos;

	if (p->rx_r) {			/* drop what has been read */
		memmove(p->rx, p->rx + p->rx_r, p->rx_w - p->rx_r);
		memmove(p->rxt, p->rxt + p->rx_r,
			(p->rx_w - p->rx_r) * sizeof p->rxt[0]);
		p->rx_w -= p->rx_r;
		p->rx_r = 0;
	}
	pos = p->rx_w;
	if (p->rx_w + n > T_PORT_RX) {
		fprintf(stderr, "t_port rx overflow\n");
		exit(2);
	}
	while (pos > p->rx_r && (int32_t)(p->rxt[pos - 1] - at) > 0)
		pos--;
	memmove(p->rx + pos + n, p->rx + pos, p->rx_w - pos);
	memmove(p->rxt + pos + n, p->rxt + pos, (p->rx_w - pos) * sizeof p->rxt[0]);
	for (size_t i = 0; i < n; i++) {
		p->rx[pos + i] = d[i];
		p->rxt[pos + i] = at;
	}
	p->rx_w += n;
}

/* Encode and inject a frame. */
static inline void t_port_inject_frame(struct t_port *p, uint32_t at, uint8_t route,
				uint8_t seq, uint8_t id, const uint8_t *pl,
				size_t n)
{
	uint8_t w[S06_MAX_WIRE];
	size_t len = s06_frame_encode(route, seq, id, pl, n, t_crc8, w);

	t_port_inject(p, at, w, len);
}

#endif
