/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The panel (S-13c), the front's copy of the full bootloader's
 * s13c_panel.c, trimmed: the same transactions in the same order as its
 * s13c_begin(), s13c_start() with the gamma load, the hand-over frame's
 * RAMWR and rows (s13c_boot_frame()) and a power-off that sends nothing,
 * with one difference: no color is written before DISPON; the first frame
 * is sent with the display off and DISPON follows its last byte
 * (S-13c.010).
 * Left out: the identification reads, which S-13c.011 permits and on
 * which nothing depends, and TE pacing (the front's frames are not held
 * mode's, S-13c.017).
 */
#include "fr_panel.h"

/* ST7796S commands. */
#define C_SLPOUT  0x11u
#define C_INVON   0x21u
#define C_DISPOFF 0x28u
#define C_DISPON  0x29u
#define C_CASET   0x2Au
#define C_RASET   0x2Bu
#define C_RAMWR   0x2Cu
#define C_TEON    0x35u
#define C_MADCTL  0x36u
#define C_COLMOD  0x3Au
#define C_B9      0xB9u
#define C_DGR     0xE2u
#define C_DGB     0xE3u
#define C_CSCON   0xF0u

/* S-01.006: the first stage's handshake value in DMA9_XMOD. */
#define HANDSHAKE_MAGIC 0x12345678u

#define RESET_LOW_US   20u	/* S-13c.005: >= 10 us */
#define RESET_WAIT_US  120000u	/* S-13c.006: release to the first transaction */
#define SLPOUT_WAIT_US 5100u	/* S-13c.007: >= 5 ms */

static uint64_t now_us(const struct fr_panel *p)
{
	return p->clock->now_us(p->clock->ctx);
}

static void delay_us(const struct fr_panel *p, uint32_t us)
{
	p->clock->delay_us(p->clock->ctx, us);
}

/* S-13c.021: a release at us, in ms time (truncated, at least 1); the
 * first transaction 120 ms later. */
static void released(struct fr_panel *p)
{
	uint64_t t = now_us(p), ms = t / 1000u;

	p->reset_ms = ms == 0u ? 1u : (uint32_t)ms;
	p->ready_us = t + RESET_WAIT_US;
}

void fr_panel_begin(struct fr_panel *p, const struct fr_panel_bus *bus,
		    const struct fr_clock *clock, uint32_t xmod, uint32_t ymod,
		    unsigned pa8)
{
	uint32_t now_ms, t = ymod != 0u ? ymod : 1u;

	p->bus = bus;
	p->clock = clock;
	p->ready = false;
	p->on = false;
	p->started = false;
	now_ms = (uint32_t)(now_us(p) / 1000u);
	/* S-13c.004, S-17.003: PA8 strapped low reset the panel again. */
	if ((pa8 & 1u) == 0u)
		p->path = FR_PANEL_STRAP;
	else if (xmod != HANDSHAKE_MAGIC || t >= now_ms)
		p->path = FR_PANEL_PULSE;
	else
		p->path = FR_PANEL_HANDSHAKE;

	/* S-13c.001: the first drive of RESET; on a board strapped low it
	 * ends the strap interval's reset. */
	bus->reset(bus->ctx, true);
	switch (p->path) {
	case FR_PANEL_HANDSHAKE:
		/* The release came no later than the end of the first stage's
		 * millisecond. t < now_ms, so ready_us is at most the time
		 * now plus 120 ms. */
		p->reset_ms = t;
		p->ready_us = ((uint64_t)t + 1u) * 1000u + RESET_WAIT_US;
		break;
	case FR_PANEL_PULSE:
		/* S-13c.005: one pulse of at least 10 us. */
		bus->reset(bus->ctx, false);
		delay_us(p, RESET_LOW_US);
		bus->reset(bus->ctx, true);
		released(p);
		break;
	default:
		/* S-13c.004: the pulse is optional after a low strap; none. */
		released(p);
		break;
	}
}

/* ---- the initialization (s13c_panel.c's order) ---- */

static const uint8_t p_unlock1[] = { 0xC3 }, p_unlock2[] = { 0x96 };
static const uint8_t p_lock1[] = { 0x3C }, p_lock2[] = { 0x69 };
static const uint8_t p_madctl[] = { 0x98 }, p_colmod[] = { 0x55 };
static const uint8_t p_dic[] = { 0x01 }, p_dfc[] = { 0x8A, 0x07, 0x3B };
static const uint8_t p_em[] = { 0xC6 }, p_b9_off[] = { 0x02, 0xE0 };
static const uint8_t p_b9_on[] = { 0x42, 0xE0 };
static const uint8_t p_pwr1[] = { 0xC0, 0x64 }, p_pwr2[] = { 0x1D };
static const uint8_t p_pwr3[] = { 0xA7 }, p_vcom[] = { 0x18 };
static const uint8_t p_doca[] = { 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5,
				  0x33 };
static const uint8_t p_pgc[] = { 0xF0, 0x0B, 0x12, 0x09, 0x0A, 0x26, 0x39,
				 0x54, 0x4E, 0x38, 0x13, 0x13, 0x2E, 0x34 };
static const uint8_t p_ngc[] = { 0xF0, 0x10, 0x15, 0x0D, 0x0C, 0x07, 0x38,
				 0x43, 0x4D, 0x3A, 0x16, 0x15, 0x30, 0x35 };
static const uint8_t p_teon[] = { 0x00 };
/* S-13c.014: columns 49-272, which cover the visible 49-270; rows 0-479. */
static const uint8_t p_caset_frame[] = { 0x00, 0x31, 0x01, 0x10 };
static const uint8_t p_raset[] = { 0x00, 0x00, 0x01, 0xDF };

struct txn {
	uint8_t cmd, len;
	const uint8_t *p;
};

#define T(c, a) { (c), (uint8_t)sizeof(a), (a) }

/* S-13c.008 inside one command-set-2 window (S-13c.009), then TEON and
 * INVON. DISPOFF first: the panel is off after a reset either way. */
static const struct txn init_seq[] = {
	{ C_DISPOFF, 0, 0 },
	T(C_CSCON, p_unlock1), T(C_CSCON, p_unlock2),
	T(C_MADCTL, p_madctl), T(C_COLMOD, p_colmod),
	T(0xB4, p_dic), T(0xB6, p_dfc), T(0xB7, p_em), T(C_B9, p_b9_off),
	T(0xC0, p_pwr1), T(0xC1, p_pwr2), T(0xC2, p_pwr3), T(0xC5, p_vcom),
	T(0xE8, p_doca), T(0xE0, p_pgc), T(0xE1, p_ngc),
	T(C_CSCON, p_lock1), T(C_CSCON, p_lock2),
	T(C_TEON, p_teon),
	{ C_INVON, 0, 0 },
};

static void send(const struct fr_panel *p, uint8_t cmd, const uint8_t *a, size_t n)
{
	const struct fr_panel_bus *b = p->bus;

	b->cmd(b->ctx, cmd);
	if (n != 0u)
		b->data(b->ctx, a, n);
}

/* S-13c.012: the tables are free; a ramp. */
static void gamma_table(const struct fr_panel *p, uint8_t cmd)
{
	uint8_t t[64];
	unsigned i;

	for (i = 0; i < sizeof t; i++)
		t[i] = (uint8_t)(4u * i);
	send(p, cmd, t, sizeof t);
}

void fr_panel_start(struct fr_panel *p)
{
	const struct fr_panel_bus *b = p->bus;
	uint64_t now;
	unsigned i;

	if (b == NULL || p->started)
		return;		/* S-13c.013: once per run */
	p->started = true;
	/* S-13c.006: one wait, at most the 120 ms from the release (begin
	 * set ready_us no later than that); bounded again in case the clock
	 * was not. */
	now = now_us(p);
	if (now < p->ready_us) {
		uint64_t left = p->ready_us - now;

		delay_us(p, left > RESET_WAIT_US + 1000u ? RESET_WAIT_US + 1000u
							 : (uint32_t)left);
	}
	b->select(b->ctx, true);
	/* S-13c.007 */
	send(p, C_SLPOUT, 0, 0);
	delay_us(p, SLPOUT_WAIT_US);
	for (i = 0; i < sizeof init_seq / sizeof init_seq[0]; i++)
		send(p, init_seq[i].cmd, init_seq[i].p, init_seq[i].len);
	/* S-13c.009, S-13c.012: the gamma load in one window. */
	send(p, C_CSCON, p_unlock1, 1);
	send(p, C_CSCON, p_unlock2, 1);
	gamma_table(p, C_DGR);
	gamma_table(p, C_DGB);
	send(p, C_B9, p_b9_on, 2);
	send(p, C_CSCON, p_lock1, 1);
	send(p, C_CSCON, p_lock2, 1);
	/* S-13c.014: the frame window, kept from here on. The display stays
	 * off: the first frame writes every pixel of the visible window, and
	 * DISPON follows its last byte (S-13c.010). */
	send(p, C_CASET, p_caset_frame, sizeof p_caset_frame);
	send(p, C_RASET, p_raset, sizeof p_raset);
	p->ready = true;
}

/* A window row, in L1 data on the target. */
static uint8_t line[FR_PANEL_ROW_BYTES] FR_FAST_BSS;

/* S-13c.015, S-13c.016: RAMWR and the 480 rows, by the CPU, with nothing
 * between them; the frame has ended when this returns. */
void fr_panel_frame(struct fr_panel *p,
		    void (*row)(void *ctx, unsigned r, uint8_t *out), void *ctx)
{
	const struct fr_panel_bus *b = p->bus;
	unsigned r;

	if (!p->ready)
		return;
	b->cmd(b->ctx, C_RAMWR);
	for (r = 0; r < FR_PANEL_ROWS; r++) {
		row(ctx, r, line);
		b->data(b->ctx, line, sizeof line);
		fr_serve();
	}
	/* S-13c.010: the first frame has written the visible window, so the
	 * panel's memory is the frame's when the display comes on. */
	if (!p->on) {
		send(p, C_DISPON, 0, 0);
		p->on = true;
	}
}

/* S-13c.022: no frame is in progress (they end before fr_panel_frame()
 * returns); no frame after this, and nothing is sent: the panel keeps its
 * last frame, in display-on. */
void fr_panel_off(struct fr_panel *p)
{
	p->ready = false;	/* no frame after it */
}
