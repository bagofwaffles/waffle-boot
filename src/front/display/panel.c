/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The panel (S-13c), the front's copy of the full bootloader's
 * core/display/panel.c, trimmed: the same transactions in the same order as its
 * s13c_begin(), s13c_start() with the gamma load, and a power-off that
 * sends nothing, with one difference: no color is written before DISPON;
 * the first frame is sent with the display off and DISPON follows its last
 * byte (S-13c.010). Frames go as held mode's do (wb_lcd_te_wait(),
 * wb_lcd_frame_start()): RAMWR on a TE rising edge, then one memory DMA of
 * the frame buffer (S-13c.017, S-13c.018).
 * Left out: the identification reads, which S-13c.011 permits and on
 * which nothing depends.
 */
#include "front/display/panel.h"

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
	p->gamma = true;
	p->on = false;
	p->started = false;
	p->cpu = false;
	p->out = FR_OUT_NONE;
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

/* ---- the initialization (core/display/panel.c's order) ---- */

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
	/* S-13c.009, S-13c.012: the gamma load in one window; S-13c.026:
	 * none without digital gamma. */
	if (p->gamma) {
		send(p, C_CSCON, p_unlock1, 1);
		send(p, C_CSCON, p_unlock2, 1);
		gamma_table(p, C_DGR);
		gamma_table(p, C_DGB);
		send(p, C_B9, p_b9_on, 2);
		send(p, C_CSCON, p_lock1, 1);
		send(p, C_CSCON, p_lock2, 1);
	}
	/* S-13c.014: the frame window, kept from here on. The display stays
	 * off: the first frame writes every pixel of the visible window, and
	 * DISPON follows its last byte (S-13c.010). */
	send(p, C_CASET, p_caset_frame, sizeof p_caset_frame);
	send(p, C_RASET, p_raset, sizeof p_raset);
	p->ready = true;
}

/* How often the waits look at TE and at the DMA. */
#define POLL_TE_US  10u
#define POLL_DMA_US 50u

/* S-13c.025: columns 189-204. */
static const uint8_t p_caset_line[] = { 0x00, FR_PANEL_LINE_COL, 0x00,
					FR_PANEL_LINE_COL + FR_PANEL_LINE_COLS - 1u };

/* S-13c.017: a TE rising edge, at most FR_PANEL_TE_US; a wait that saw
 * none is counted. The link is served on every pass. */
static void te_wait(struct fr_panel *p)
{
	const struct fr_panel_bus *b = p->bus;
	const uint64_t start = now_us(p);
	bool low = false;

	if (b->te == NULL)
		return;		/* no TE line */
	for (;;) {
		fr_serve();
		if (!b->te(b->ctx))
			low = true;
		else if (low)
			return;
		if (now_us(p) - start >= FR_PANEL_TE_US) {
			if (p->te_misses < 255u)
				p->te_misses++;
			return;
		}
		delay_us(p, POLL_TE_US);
	}
}

/* The CPU's send of what (enum fr_panel_out) from buf: every byte by
 * data(), a window row at a time, the link served between rows. */
static void cpu_send(const struct fr_panel *p, const uint8_t *buf, uint8_t what)
{
	const struct fr_panel_bus *b = p->bus;
	const size_t row = what == FR_OUT_LINE ? 2u * FR_PANEL_LINE_COLS : FR_PANEL_ROW_BYTES;
	unsigned r;

	for (r = 0; r < FR_PANEL_ROWS; r++) {
		b->data(b->ctx, buf + (size_t)r * row, row);
		fr_serve();
	}
}

/* After a line's last byte: the frame window's columns again (S-13c.025,
 * S-13c.014). */
static void sent(const struct fr_panel *p, uint8_t what)
{
	if (what == FR_OUT_LINE)
		send(p, C_CASET, p_caset_frame, sizeof p_caset_frame);
}

/* S-13c.016: RAMWR and the bytes, by one DMA, or by the CPU when there is
 * none or one was stopped; the CPU's has ended when this returns. */
static void go(struct fr_panel *p, const uint8_t *buf, uint8_t what)
{
	const struct fr_panel_bus *b = p->bus;

	b->cmd(b->ctx, C_RAMWR);
	if (p->cpu || b->dma_start == NULL) {
		cpu_send(p, buf, what);
		sent(p, what);
		return;
	}
	b->dma_start(b->ctx, buf, what == FR_OUT_LINE ? FR_PANEL_LINE_BYTES : FR_PANEL_FRAME_BYTES);
	p->out = what;
	p->out_buf = buf;
	p->out_at = now_us(p);
}

/* S-13c.017's "A send that does not end": a DMA send that failed, or has
 * not ended FR_PANEL_DMA_US after its start, is stopped and sent again by
 * the CPU, whole, paced unless it is the early logo frame; so is every
 * later one. A send that ended leaves both channels disabled. */
void fr_panel_watch(struct fr_panel *p)
{
	const struct fr_panel_bus *b = p->bus;

	if (p->out == FR_OUT_NONE || p->stopped)
		return;
	if (b->dma_state(b->ctx) == FR_DMA_BUSY && now_us(p) - p->out_at < FR_PANEL_DMA_US)
		return;
	if (b->dma_state(b->ctx) == FR_DMA_IDLE)
		return;		/* ended: settle() finishes it */
	b->dma_stop(b->ctx);
	p->stopped = true;
}

static void settle(struct fr_panel *p, bool paced)
{
	const struct fr_panel_bus *b = p->bus;
	const uint8_t what = p->out;
	int st;

	if (what == FR_OUT_NONE)
		return;
	st = p->stopped ? FR_DMA_ERROR : b->dma_state(b->ctx);
	if (st == FR_DMA_BUSY && now_us(p) - p->out_at < FR_PANEL_DMA_US)
		return;
	p->out = FR_OUT_NONE;
	p->stopped = false;
	b->dma_stop(b->ctx);
	if (st == FR_DMA_IDLE) {
		sent(p, what);
		return;
	}
	p->cpu = true;
	if (paced)
		te_wait(p);
	go(p, p->out_buf, what);
}

static void wait_end(struct fr_panel *p, bool paced)
{
	while (p->out != FR_OUT_NONE) {
		settle(p, paced);
		if (p->out == FR_OUT_NONE)
			break;
		fr_serve();
		delay_us(p, POLL_DMA_US);
	}
}

void fr_panel_frame(struct fr_panel *p, const uint8_t *buf)
{
	if (!p->ready)
		return;
	if (!p->on) {
		/* S-13c.010, S-13c.017: the early logo frame, not paced; the
		 * panel's memory is the frame's when the display comes on. */
		go(p, buf, FR_OUT_FRAME);
		wait_end(p, false);
		send(p, C_DISPON, 0, 0);
		p->on = true;
		return;
	}
	wait_end(p, true);
	te_wait(p);
	go(p, buf, FR_OUT_FRAME);
}

void fr_panel_line(struct fr_panel *p, const uint8_t *band)
{
	if (!p->ready || !p->on)
		return;
	wait_end(p, true);
	te_wait(p);
	send(p, C_CASET, p_caset_line, sizeof p_caset_line);
	send(p, C_RASET, p_raset, sizeof p_raset);
	go(p, band, FR_OUT_LINE);
}

bool fr_panel_busy(struct fr_panel *p)
{
	settle(p, true);
	return p->out != FR_OUT_NONE;
}

void fr_panel_idle(struct fr_panel *p)
{
	wait_end(p, true);
}

/* S-13c.022: a send going out is left to end, at most FR_PANEL_OFF_US,
 * then stopped where it is; no frame after this, and nothing is sent: the
 * panel keeps its last frame, in display-on. The link is not served here:
 * this runs inside the power-off's own handling. */
void fr_panel_off(struct fr_panel *p)
{
	const struct fr_panel_bus *b = p->bus;

	if (p->out != FR_OUT_NONE) {
		const uint64_t start = now_us(p);

		/* the last poll ends no later than FR_PANEL_OFF_US */
		while (!p->stopped && b->dma_state(b->ctx) == FR_DMA_BUSY &&
		       now_us(p) - start + POLL_DMA_US <= FR_PANEL_OFF_US)
			delay_us(p, POLL_DMA_US);
		b->dma_stop(b->ctx);
		p->out = FR_OUT_NONE;
	}
	p->ready = false;	/* no frame after it */
}

/* The main image's held mode starts its first frame only on channels that
 * are not enabled (wb_lcd_frame_start()). */
void fr_panel_end(struct fr_panel *p)
{
	const struct fr_panel_bus *b = p->bus;

	if (b == NULL)
		return;
	wait_end(p, true);
	if (b->dma_stop != NULL)
		b->dma_stop(b->ctx);
}
