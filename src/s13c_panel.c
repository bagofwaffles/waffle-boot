/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-13c the display panel: reset, initialization, frames, power-off and
 * the hand-over.
 */
#include "waffle/s13c_panel.h"
#include "waffle/testos.h"

#include "waffle/bytes.h"
#include "waffle/s17_revisions.h"

/* S-13c.004 */
enum s13c_reset_path s13c_reset_decide(uint32_t xmod, uint32_t ymod,
				       unsigned pa8, uint32_t now_ms,
				       uint32_t *handshake_ms)
{
	uint32_t t = ymod != 0u ? ymod : 1u;

	/* S-17.003: a PA8 strapped low reset the panel again. */
	if (s17_panel_reset_path(pa8) == S17_PANEL_RESET_AT_RELEASE)
		return S13C_RESET_STRAP;
	if (xmod != S13C_HANDSHAKE_MAGIC || t >= now_ms)
		return S13C_RESET_PULSE;
	*handshake_ms = t;
	return S13C_RESET_HANDSHAKE;
}

/* S-13c.021 */
void s13c_record(uint32_t reset_ms, uint8_t rec[S13C_RECORD_SIZE])
{
	unsigned i;

	for (i = 0; i < S13C_RECORD_SIZE; i++)
		rec[i] = 0;
	rec[0x00] = 1;			/* the panel has been reset */
	wb_put_le32(&rec[0x04], reset_ms);
	wb_put_le32(&rec[0x08], S13C_RECORD_VERSION);
	rec[0x28] = 1;			/* the registers are initialized */
}

static uint64_t now_us(const struct s13c_panel *p)
{
	return p->bus->now_us(p->bus->ctx);
}

static uint32_t ms_of(uint64_t us)
{
	uint64_t ms = us / 1000u;

	/* S-13c.021: at least 1. */
	return ms == 0u ? 1u : (uint32_t)ms;
}

/* The ms time of a release at us: its truncation, at least 1. */
static void released(struct s13c_panel *p)
{
	uint64_t t = now_us(p);

	p->reset_ms = ms_of(t);
	p->ready_us = t + S13C_RESET_WAIT_US;
}

void s13c_begin(struct s13c_panel *p, const struct s13c_bus *bus,
		uint32_t xmod, uint32_t ymod, unsigned pa8)
{
	const struct s13c_bus *b = bus;
	uint32_t hs = 0;

	p->bus = bus;
	p->gamma_loaded = false;
	p->gamma_on = false;
	p->frame = false;
	p->boot_frame = false;
	p->lined = false;
	p->select_due = false;
	p->id_valid = false;
	p->te_misses = 0;
	p->path = s13c_reset_decide(xmod, ymod, pa8,
				    (uint32_t)(now_us(p) / 1000u), &hs);

	/* S-13c.001: the first drive of RESET; on a board strapped low it
	 * ends the strap interval's reset. */
	b->reset(b->ctx, true);
	switch (p->path) {
	case S13C_RESET_HANDSHAKE:
		/* The release came no later than the end of the first
		 * stage's millisecond. */
		p->reset_ms = hs;
		p->ready_us = ((uint64_t)hs + 1u) * 1000u + S13C_RESET_WAIT_US;
		break;
	case S13C_RESET_PULSE:
		/* S-13c.005: one pulse of at least 10 us. */
		b->reset(b->ctx, false);
		b->delay_us(b->ctx, S13C_RESET_LOW_US);
		b->reset(b->ctx, true);
		released(p);
		break;
	case S13C_RESET_STRAP:
	default:
		/* The pulse is optional (S-13c.004); none is made. */
		released(p);
		break;
	}
	p->state = S13C_RESET;
}

void s13c_adopt(struct s13c_panel *p, const struct s13c_bus *bus,
		uint32_t reset_ms)
{
	p->bus = bus;
	p->path = S13C_RESET_HANDSHAKE;
	p->reset_ms = reset_ms;
	p->ready_us = 0;
	/* What the front's gamma load left is not trusted: the caller loads the
	 * tables again (s13c_gamma()) before anything is drawn. */
	p->gamma_loaded = false;
	p->gamma_on = false;
	p->frame = false;
	p->boot_frame = true;
	p->lined = false;
	p->select_due = true;
	p->id_valid = false;
	p->te_misses = 0;
	p->state = S13C_ON;
}

/* ---- the initialization (init_reference.tsv's order) ---- */

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
/* S-13c.010: columns 49-270; S-13c.014: columns 49-272; rows 0-479. */
static const uint8_t p_caset_vis[] = { 0x00, 0x31, 0x01, 0x0E };
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
	{ S13C_DISPOFF, 0, 0 },
	T(S13C_CSCON, p_unlock1), T(S13C_CSCON, p_unlock2),
	T(S13C_MADCTL, p_madctl), T(S13C_COLMOD, p_colmod),
	T(0xB4, p_dic), T(0xB6, p_dfc), T(0xB7, p_em), T(S13C_B9, p_b9_off),
	T(0xC0, p_pwr1), T(0xC1, p_pwr2), T(0xC2, p_pwr3), T(0xC5, p_vcom),
	T(0xE8, p_doca), T(0xE0, p_pgc), T(0xE1, p_ngc),
	T(S13C_CSCON, p_lock1), T(S13C_CSCON, p_lock2),
	T(S13C_TEON, p_teon),
	{ S13C_INVON, 0, 0 },
};

static void send(const struct s13c_panel *p, uint8_t cmd, const uint8_t *a,
		 size_t n)
{
	const struct s13c_bus *b = p->bus;

	b->cmd(b->ctx, cmd);
	if (n != 0u)
		b->data(b->ctx, a, n);
}

/* After s13c_adopt(): the panel selected again before its first byte. */
static void claim(struct s13c_panel *p)
{
	if (p->select_due) {
		p->bus->select(p->bus->ctx, true);
		p->select_due = false;
	}
}

static void send_txn(const struct s13c_panel *p, const struct txn *t)
{
	send(p, t->cmd, t->p, t->len);
}

/* S-13c.012: the tables are free; a ramp. */
static void gamma_table(const struct s13c_panel *p, uint8_t cmd)
{
	uint8_t t[64];
	unsigned i;

	for (i = 0; i < sizeof t; i++)
		t[i] = (uint8_t)(4u * i);
	send(p, cmd, t, sizeof t);
}

/* S-13c.009, S-13c.012: one window, closed again. */
static void gamma_window(struct s13c_panel *p, bool on)
{
	send(p, S13C_CSCON, p_unlock1, 1);
	send(p, S13C_CSCON, p_unlock2, 1);
	if (on && !p->gamma_loaded) {
		gamma_table(p, S13C_DGR);
		gamma_table(p, S13C_DGB);
		p->gamma_loaded = true;
	}
	send(p, S13C_B9, on ? p_b9_on : p_b9_off, 2);
	p->gamma_on = on;
	send(p, S13C_CSCON, p_lock1, 1);
	send(p, S13C_CSCON, p_lock2, 1);
}

static void wait_until(const struct s13c_panel *p, uint64_t t)
{
	const struct s13c_bus *b = p->bus;
	uint64_t now = now_us(p);

	while (now < t) {
		uint64_t left = t - now;

		b->delay_us(b->ctx, left > 1000000u ? 1000000u : (uint32_t)left);
		now = now_us(p);
	}
}

/* S-13c.011: values kept, nothing depends on them; S-16.169: RDID4 with
 * all four strobes. */
static void id_reads(struct s13c_panel *p)
{
	const struct s13c_bus *b = p->bus;
	uint8_t v[4];
	unsigned i;

	p->id_valid = true;
	if (b->read(b->ctx, S13C_RDDID, v, 4) != 0)
		p->id_valid = false;
	for (i = 0; i < 3u; i++)
		p->rddid[i] = v[i + 1u];
	if (b->read(b->ctx, S13C_RDID4, v, 4) != 0)
		p->id_valid = false;
	for (i = 0; i < 3u; i++)
		p->rdid4[i] = v[i + 1u];
}

int s13c_start(struct s13c_panel *p, bool gamma)
{
	static const uint8_t black[2] = { 0x00, 0x00 };
	const struct s13c_bus *b = p->bus;
	unsigned i;

	if (p->state == S13C_ON)
		return S13C_OK;		/* S-13c.013: once per run */
	if (p->state != S13C_RESET)
		return S13C_ERR_STATE;
	wait_until(p, p->ready_us);	/* S-13c.006 */
	if (b->read != 0)
		id_reads(p);
	b->select(b->ctx, true);
	/* S-13c.007 */
	send(p, S13C_SLPOUT, 0, 0);
	b->delay_us(b->ctx, S13C_SLPOUT_WAIT_US);
	for (i = 0; i < sizeof init_seq / sizeof init_seq[0]; i++)
		send_txn(p, &init_seq[i]);
	if (gamma)
		gamma_window(p, true);
	/* S-13c.010: the visible window in one color, then DISPON. */
	send(p, S13C_CASET, p_caset_vis, sizeof p_caset_vis);
	send(p, S13C_RASET, p_raset, sizeof p_raset);
	b->cmd(b->ctx, S13C_RAMWR);
	b->repeat(b->ctx, black, 2, S13C_VIS_W * S13C_ROWS);
	/* S-13c.014: the frame window, kept from here on. */
	send(p, S13C_CASET, p_caset_frame, sizeof p_caset_frame);
	send(p, S13C_RASET, p_raset, sizeof p_raset);
	send(p, S13C_DISPON, 0, 0);
	p->state = S13C_ON;
	return S13C_OK;
}

int s13c_gamma(struct s13c_panel *p, bool on)
{
	int rc;

	if (p->state != S13C_ON)
		return S13C_ERR_STATE;
	rc = s13c_frame_wait(p);	/* S-13c.016: not inside a frame */
	if (rc != S13C_OK)
		return rc;
	claim(p);
	gamma_window(p, on);
	return S13C_OK;
}

bool s13c_frame_busy(struct s13c_panel *p)
{
	if (p->frame && !p->bus->frame_busy(p->bus->ctx))
		p->frame = false;
	return p->frame;
}

static int frame_wait(struct s13c_panel *p, uint32_t bound_us)
{
	const struct s13c_bus *b = p->bus;
	uint64_t end = now_us(p) + bound_us;

	while (s13c_frame_busy(p)) {
		if (now_us(p) >= end)
			{ OPXY_TESTOS(opxy_testos_display_timeout("frame end (S-13c.016)", bound_us);) return S13C_ERR_BUS; }	/* the test OS logs it */
		b->delay_us(b->ctx, 10);
	}
	return S13C_OK;
}

int s13c_frame_wait(struct s13c_panel *p)
{
	return frame_wait(p, S13C_FRAME_TIMEOUT_US);
}

bool s13c_frame_stop(struct s13c_panel *p)
{
	const struct s13c_bus *b = p->bus;

	if (s13c_frame_busy(p) && b->frame_stop != NULL)
		b->frame_stop(b->ctx);
	return !s13c_frame_busy(p);
}

int s13c_frame(struct s13c_panel *p, const uint8_t *buf, bool paced)
{
	const struct s13c_bus *b = p->bus;
	int rc;

	if (p->state != S13C_ON)
		return S13C_ERR_STATE;
	/* S-13c.017: a frame starts only once the previous one has ended,
	 * and a paced one after a TE edge seen since then. */
	rc = s13c_frame_wait(p);
	if (rc != S13C_OK)
		return rc;
	if (paced && b->te_wait(b->ctx, S13C_TE_TIMEOUT_US) != 0)
		{ p->te_misses++; OPXY_TESTOS(opxy_testos_display_timeout("te rising edge (S-13c.017)", S13C_TE_TIMEOUT_US);) }	/* S-13c.017: sent without TE; the test OS logs it */
	claim(p);
	b->cmd(b->ctx, S13C_RAMWR);
	p->boot_frame = false;
	if (b->frame_start(b->ctx, buf, S13C_FRAME_BYTES) != 0)
		return S13C_ERR_BUS;
	p->frame = true;
	return S13C_OK;
}

int s13c_boot_frame(struct s13c_panel *p,
		    void (*row)(void *ctx, unsigned r, uint8_t *out),
		    void *ctx)
{
	const struct s13c_bus *b = p->bus;
	uint8_t line[S13C_ROW_BYTES];
	unsigned r;
	int rc;

	if (p->state != S13C_ON)
		return S13C_ERR_STATE;
	rc = s13c_frame_wait(p);
	if (rc != S13C_OK)
		return rc;
	claim(p);
	/* S-13c.012: the load precedes the hand-over frame. */
	if (!p->gamma_on)
		gamma_window(p, true);
	/* S-13c.016, S-13c.020: one frame, sent at once and ended. */
	b->cmd(b->ctx, S13C_RAMWR);
	for (r = 0; r < S13C_ROWS; r++) {
		row(ctx, r, line);
		b->data(b->ctx, line, sizeof line);
	}
	p->boot_frame = true;
	p->lined = false;
	return S13C_OK;
}

int s13c_logo_line(struct s13c_panel *p, unsigned y, unsigned h,
		   void (*row)(void *ctx, unsigned r, uint8_t *out), void *ctx)
{
	const struct s13c_bus *b = p->bus;
	const unsigned c0 = S13C_VIS_X0 + y, c1 = c0 + h - 1u;
	const uint8_t caset[4] = { (uint8_t)(c0 >> 8), (uint8_t)c0,
				   (uint8_t)(c1 >> 8), (uint8_t)c1 };
	uint8_t line[2u * S13C_LINE_MAX_H];
	unsigned r;
	int rc;

	if (h == 0u || h > S13C_LINE_MAX_H || y + h > S13C_VIS_W)
		return S13C_ERR_STATE;
	if (p->state != S13C_ON || !p->boot_frame)
		return S13C_ERR_STATE;
	rc = s13c_frame_wait(p);
	if (rc != S13C_OK)
		return rc;
	claim(p);
	/* S-13c.025: the band's columns and every row, then S-13c.014's
	 * columns again; its rows are the band's. */
	send(p, S13C_CASET, caset, sizeof caset);
	send(p, S13C_RASET, p_raset, sizeof p_raset);
	b->cmd(b->ctx, S13C_RAMWR);
	for (r = 0; r < S13C_ROWS; r++) {
		row(ctx, r, line);
		b->data(b->ctx, line, 2u * h);
	}
	send(p, S13C_CASET, p_caset_frame, sizeof p_caset_frame);
	p->lined = true;
	return S13C_OK;
}

int s13c_handover(struct s13c_panel *p, uint8_t rec[S13C_RECORD_SIZE])
{
	if (p->state != S13C_ON || !p->boot_frame || p->lined || !p->gamma_on ||
	    s13c_frame_busy(p))
		return S13C_ERR_STATE;
	p->state = S13C_HANDED;
	/* S-13c.021: written once the state it describes holds. */
	s13c_record(p->reset_ms, rec);
	return S13C_OK;
}

int s13c_poweroff(struct s13c_panel *p)
{
	if (p->state == S13C_IDLE || p->state == S13C_RESET) {
		p->state = S13C_OFF;	/* never on: it stays off */
		return S13C_OK;
	}
	if (p->state != S13C_ON)
		return p->state == S13C_OFF ? S13C_OK : S13C_ERR_STATE;
	p->state = S13C_OFF;		/* no frame starts from here */
	/* The frame in progress ends, or is stopped where it is when it has
	 * not within its bound, so it does not hold the power-off; then
	 * nothing more is sent: no DISPOFF, the panel keeps its last frame
	 * in display-on. */
	if (frame_wait(p, S13C_POWEROFF_FRAME_US) == S13C_OK)
		return S13C_OK;
	return s13c_frame_stop(p) ? S13C_OK : S13C_ERR_BUS;
}
