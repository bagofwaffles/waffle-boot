/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The test OS's milestone paints (enum opxy_testos_paint, waffle/testos.h):
 * the whole panel filled with one color at each milestone of the master's
 * start, written straight into the panel's frame memory through the
 * build's panel byte path (lcd.c or lcd_gpio.c), so that a unit that shows
 * no screen of ours still tells how far it got. Each paint is also an S-14
 * line, `testos paint <color> <milestone>`, when the log has its sink.
 *
 *   red      the entry shim handed over to C: before wb_start_c(), the SEC,
 *            the clocks check or any driver of ours; on the panel the
 *            loading bootloader initialized, which the display record of
 *            S-13c.021 says (byte +0x28 = 1; 0 skips the paint)
 *   orange   wb_start_c() and the register read-out done, the entry's
 *            strap read said master; nothing else of ours run yet, not
 *            even the full strap read (which may reset the panel, below)
 *   yellow   our own panel reset and initialization done, before the PSoC
 *            link is touched
 *   green    the S-07 start-up returned (whatever it returned)
 *   cyan     the panel's start returned (S-13c.013: a no-op once on, the
 *            gamma load was yellow's) and the early logo frame is next
 *   gray     the early logo frame's bytes went out (S-13c.016, by CPU
 *            writes on both byte paths)
 *   violet   0x51 {3C FF 02} sent after the frame (S-13c.019)
 *   blue     the slave found and the bridge up
 *   magenta  a fatal (opxy_fatal())
 *   olive to navy  the slave's start, the bridge's first session and
 *            held mode's start, each as enum opxy_testos_paint lists
 *            it
 *
 * The first two paints run before the display driver is up: a bus of their
 * own on the stack, over S-13b.003's pin functions and the byte path's own
 * set-up (the SMC bank's timing, or every pin a GPIO), and no static data
 * of this image, since .bss is not cleared before wb_start_c(). What the
 * entry paint found is kept in opxy_testos_entry (loaded data) for the log.
 * The later paints go through the display driver's own bus, so its D/C and
 * CS state stays right, after any frame in flight has ended (S-13c.016).
 * Every paint restores the frame window (S-13c.014) and holds the color
 * for OPXY_TESTOS_PAINT_HOLD_US, long enough to be seen.
 *
 * The monitor and the installer paint only a fatal. In their place, once
 * the early logo shows, a gray line under it names the stage that starts
 * (stage_line()), until the first frame of held mode replaces the logo.
 *
 * The slave has no panel: a paint on it does nothing, which the strap
 * sample decides (S-13b.001, read before any pin is touched). The entry
 * paint samples PA2 alone: PA8, the revision strap's other bit, is also
 * the panel's RESET (S-13c.004), and on a board whose PA8 straps low the
 * 22 us it spends as an input in a full strap read reset the panel before
 * red could be seen (S-17.003's first row). The full sample, PA0, PA2 and
 * PA8, is opxy_main()'s, after orange.
 */
#include "waffle/testos.h"

#if defined(OPXY_TEST_OS)

#include <string.h>

#include "waffle/board/opxy.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/display.h"
#include "waffle/drivers/bf70x_display.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/s01_memmap.h"
#include "waffle/s13c_panel.h"

/* The panel's whole frame memory: 320 columns by 480 rows (ST7796S). */
#define PANEL_COLS 320u
/* A frame in flight ends within this (S13C_FRAME_TIMEOUT_US's order). */
#define FRAME_END_US 100000u

/* What a paint did; the entry paint's goes to opxy_testos_entry[4]. */
#define PAINT_NONE     0u	/* not attempted */
#define PAINT_DONE     1u
#define PAINT_SLAVE    2u	/* this CPU is the slave: no panel */
#define PAINT_NORECORD 3u	/* S-13c.021's byte +0x28 is not 1 */
#define PAINT_PANELOFF 4u	/* the panel is not on (S-13c state) */

struct paint {
	const char *color, *milestone;
	uint16_t rgb565;	/* S-13c.015, as the frame buffer holds it */
};

static const struct paint paints[OPXY_PAINT_COUNT] = {
	[OPXY_PAINT_RED] = { "red", "entry", 0xF800u },
	[OPXY_PAINT_ORANGE] = { "orange", "start-c done, master", 0xFC00u },
	[OPXY_PAINT_YELLOW] = { "yellow", "panel initialized", 0xFFE0u },
	[OPXY_PAINT_GREEN] = { "green", "psoc start-up returned", 0x07E0u },
	[OPXY_PAINT_CYAN] = { "cyan", "panel start returned, early logo next", 0x07FFu },
	[OPXY_PAINT_GRAY] = { "gray", "early logo frame sent", 0x8410u },
	[OPXY_PAINT_VIOLET] = { "violet", "brightness 3c ff 02 sent", 0x801Fu },
	[OPXY_PAINT_BLUE] = { "blue", "slave found", 0x001Fu },
	[OPXY_PAINT_MAGENTA] = { "magenta", "fatal", 0xF81Fu },
	[OPXY_PAINT_OLIVE] = { "olive", "slave start entered", 0x8400u },
	[OPXY_PAINT_PINK] = { "pink", "bridge ready line low", 0xFC18u },
	[OPXY_PAINT_TEAL] = { "teal", "bridge first session answered", 0x0410u },
	[OPXY_PAINT_BROWN] = { "brown", "slave given up as absent", 0x8200u },
	[OPXY_PAINT_LIME] = { "lime", "held mode start entered", 0xAFE5u },
	[OPXY_PAINT_NAVY] = { "navy", "held mode services started", 0x0010u },
};

/* The stage lines of the monitor and the installer: the stage starting at
 * each milestone after the early logo; NULL for none. Before the logo the
 * panel is not yet initialized in these images. */
static const char *const stages[OPXY_PAINT_COUNT] = {
	[OPXY_PAINT_VIOLET] = "Starting: keys and battery",
	[OPXY_PAINT_OLIVE] = "Starting: second processor",
	[OPXY_PAINT_PINK] = "Starting: bridge",
	[OPXY_PAINT_TEAL] = "Starting: bridge check",
	[OPXY_PAINT_BLUE] = "Starting: the eMMC",
	[OPXY_PAINT_BROWN] = "No second processor, going on",
	[OPXY_PAINT_LIME] = "Starting: USB and files",
	[OPXY_PAINT_NAVY] = "Starting: installer",
};

/* The whole frame memory, then S-13c.014's frame window again: columns
 * 49-272 (s13c_panel.c's p_caset_frame) and rows 0-479. */
static const uint8_t caset_all[4] = { 0x00, 0x00, 0x01, 0x3F };
static const uint8_t raset_all[4] = { 0x00, 0x00, 0x01, 0xDF };
static const uint8_t caset_frame[4] = { 0x00, 0x31, 0x01, 0x10 };

/* Paints made before the log had its sink, in order; flushed by
 * opxy_testos_paint_log(). Not the entry paint: see opxy_testos_entry. */
static uint8_t pending[OPXY_PAINT_COUNT][2];
static unsigned n_pending;
static struct opxy_master *log_m;

static bool record_initialized(void)
{
	const volatile uint8_t *rec =
		(const volatile uint8_t *)(uintptr_t)S01_DISPLAY_RECORD_START;

	return rec[0x28] == 1u;
}

static bool is_master(void)
{
	return opxy_strap_role((uint16_t)opxy_testos_entry[5]) == OPXY_MASTER;
}

static void fill(const struct s13c_bus *b, uint16_t rgb565)
{
	const uint8_t px[2] = { (uint8_t)(rgb565 >> 8), (uint8_t)rgb565 };

	b->select(b->ctx, true);
	b->cmd(b->ctx, S13C_CASET);
	b->data(b->ctx, caset_all, sizeof caset_all);
	b->cmd(b->ctx, S13C_RASET);
	b->data(b->ctx, raset_all, sizeof raset_all);
	b->cmd(b->ctx, S13C_RAMWR);
	b->repeat(b->ctx, px, 2u, PANEL_COLS * S13C_ROWS);
	b->cmd(b->ctx, S13C_CASET);
	b->data(b->ctx, caset_frame, sizeof caset_frame);
	b->cmd(b->ctx, S13C_RASET);
	b->data(b->ctx, raset_all, sizeof raset_all);
}

/* The stage line's band: screen y STAGE_Y to STAGE_Y + 15, under the
 * logo's squares (y 104-117), across the whole width. */
#define STAGE_Y 140u

/*
 * The stage line, centered in its band, in place of the last: the band's
 * panel columns and every row, written straight into the frame memory by
 * CPU writes, one window row (one screen x, S-13c.023) at a time, then the
 * frame window again. The frame buffer is not touched, so held mode's first
 * frame draws over it.
 */
static void stage_line(const struct s13c_bus *b, const char *s)
{
	const unsigned n = (unsigned)strlen(s);
	const unsigned x0 = (WB_DISPLAY_W - n * WB_FONT_W) / 2u;
	const unsigned c0 = 0x31u + STAGE_Y, c1 = c0 + WB_FONT_H - 1u;
	const uint8_t caset[4] = { 0x00, (uint8_t)c0, 0x00, (uint8_t)c1 };
	static const uint8_t blank[WB_FONT_H];
	uint8_t px[2u * WB_FONT_H];
	unsigned r, j;

	b->select(b->ctx, true);
	b->cmd(b->ctx, S13C_CASET);
	b->data(b->ctx, caset, sizeof caset);
	b->cmd(b->ctx, S13C_RASET);
	b->data(b->ctx, raset_all, sizeof raset_all);
	b->cmd(b->ctx, S13C_RAMWR);
	for (r = 0; r < S13C_ROWS; r++) {
		const unsigned x = WB_DISPLAY_W - 1u - r;
		const uint8_t *g = blank;
		unsigned bit = 0;

		if (x >= x0 && x < x0 + n * WB_FONT_W) {
			unsigned ch = (unsigned char)s[(x - x0) / WB_FONT_W];

			if (ch < WB_FONT_FIRST || ch >= WB_FONT_FIRST + WB_FONT_COUNT)
				ch = '?';
			g = wb_display_font[ch - WB_FONT_FIRST];
			bit = WB_FONT_W - 1u - (x - x0) % WB_FONT_W;
		}
		for (j = 0; j < WB_FONT_H; j++) {
			const uint16_t c = (((unsigned)g[j] >> bit) & 1u) != 0u ?
						   S15_DIM : S15_BLACK;

			px[2u * j] = (uint8_t)(c >> 8);
			px[2u * j + 1u] = (uint8_t)c;
		}
		b->data(b->ctx, px, sizeof px);
	}
	b->cmd(b->ctx, S13C_CASET);
	b->data(b->ctx, caset_frame, sizeof caset_frame);
	b->cmd(b->ctx, S13C_RASET);
	b->data(b->ctx, raset_all, sizeof raset_all);
}

/* A stage's line, while the early logo is what the panel shows: on, the
 * logo's frame sent, no frame of held mode since (S-13c.016). */
static void stage(enum opxy_testos_paint which)
{
	struct wb_display *d = opxy_testos_display();
	uint64_t end;

	if (stages[which] == NULL || d == NULL || d->panel.state != S13C_ON ||
	    !d->panel.boot_frame)
		return;
	end = wb_time_deadline(&opxy_time, FRAME_END_US);
	while (wb_display_busy(d) && !wb_time_passed(end))
		;
	if (!wb_display_busy(d))
		stage_line(d->panel.bus, stages[which]);
}

/*
 * Before the display driver is up: S-13b.003's pin functions (the board's
 * own table, which the bring-up applies again), the byte path's set-up,
 * the fill, CS high again. Nothing static is touched.
 */
static void paint_private(uint16_t rgb565)
{
	struct wb_display_bf70x a;
	const struct s13c_bus *b;

	(void)wb_pinmux_apply(&opxy_pinmux_master);
	b = wb_display_bf70x_init(&a, &opxy_time);
	if (b == NULL)
		return;
	fill(b, rgb565);
	b->select(b->ctx, false);
}

static void hold(void)
{
	wb_delay_us(&opxy_time, OPXY_TESTOS_PAINT_HOLD_US);
}

void opxy_testos_entry_paint(void)
{
	/* S-13b.001's role strap alone (opxy_strap's timings): PA8 is the
	 * panel's RESET and is not released here, see above. */
	const struct wb_strap role = {
		.port = opxy_strap.port,
		.pins = OPXY_STRAP_ROLE,
		.settle_us = opxy_strap.settle_us,
		.inen_us = opxy_strap.inen_us,
	};
	uint16_t strap = 0;
	uint32_t outcome;

	/* S-13b.001: the role before any other pin is touched. */
	if (wb_strap_read(&role, &opxy_time, &strap) != 0)
		strap = 0;
	opxy_testos_entry[5] = strap;
	if (opxy_strap_role(strap) != OPXY_MASTER) {
		outcome = PAINT_SLAVE;
	} else if (!record_initialized()) {
		outcome = PAINT_NORECORD;
	} else {
		if (OPXY_TESTOS_STAGE_PAINTS) {
			paint_private(paints[OPXY_PAINT_RED].rgb565);
			hold();
		}
		outcome = PAINT_DONE;
	}
	opxy_testos_entry[4] = outcome;
}

static void line(struct opxy_master *m, enum opxy_testos_paint which,
		 unsigned outcome)
{
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "paint ");
	s14_text_str(&t, paints[which].color);
	s14_text_str(&t, " ");
	s14_text_str(&t, paints[which].milestone);
	switch (outcome) {
	case PAINT_DONE:
		break;
	case PAINT_SLAVE:
		s14_text_str(&t, ": not painted, this CPU is the slave");
		break;
	case PAINT_NORECORD:
		s14_text_str(&t, ": not painted, display record byte 0x28 is "
				 "not 1 (S-13c.021)");
		break;
	case PAINT_PANELOFF:
		s14_text_str(&t, ": not painted, the panel is not on");
		break;
	case PAINT_NONE:
	default:
		s14_text_str(&t, ": not attempted");
		break;
	}
	opxy_testos_emit(m, &t);
}

static void record(enum opxy_testos_paint which, unsigned outcome)
{
	if (log_m != NULL) {
		line(log_m, which, outcome);
	} else if (n_pending < OPXY_PAINT_COUNT) {
		pending[n_pending][0] = (uint8_t)which;
		pending[n_pending][1] = (uint8_t)outcome;
		n_pending++;
	}
}

void opxy_testos_paint(enum opxy_testos_paint which)
{
	struct wb_display *d = opxy_testos_display();
	unsigned outcome = PAINT_DONE;

	if ((unsigned)which >= OPXY_PAINT_COUNT)
		return;
	if (!is_master()) {
		record(which, PAINT_SLAVE);
		return;
	}
	/* The monitor's and the installer's images log their milestones and
	 * paint only a fatal, so the first thing seen is the early logo; the
	 * stages after it get a line under it. */
	if (!OPXY_TESTOS_STAGE_PAINTS && which != OPXY_PAINT_MAGENTA) {
		stage(which);
		record(which, PAINT_DONE);
		return;
	}
	if (d == NULL) {
		/* The loading bootloader's panel: as the entry paint. A fatal
		 * is painted whatever the record says. */
		if (record_initialized() || which == OPXY_PAINT_MAGENTA)
			paint_private(paints[which].rgb565);
		else
			outcome = PAINT_NORECORD;
	} else {
		uint64_t end = wb_time_deadline(&opxy_time, FRAME_END_US);

		while (wb_display_busy(d) && !wb_time_passed(end))
			;
		/* Reset and not yet initialized: the run's one initialization
		 * (S-13c.013), with the gamma load as the boot path has it. */
		if (d->panel.state == S13C_RESET)
			(void)wb_display_start(d, true);
		if (d->panel.state == S13C_ON)
			fill(d->panel.bus, paints[which].rgb565);
		else
			outcome = PAINT_PANELOFF;
	}
	record(which, outcome);
	if (outcome == PAINT_DONE && which != OPXY_PAINT_MAGENTA)
		hold();
}

void opxy_testos_display_dma_stop(void)
{
	const unsigned ch[2] = { WB_DMA_MDMA_SRC(2), WB_DMA_MDMA_DST(2) };
	unsigned i;

	for (i = 0; i < 2u; i++) {
		wb_dma_stop(ch[i]);
		wb_mmr_write32(DMA_DSCPTR_NXT(ch[i]), 0u);
		wb_mmr_write32(DMA_ADDRSTART(ch[i]), 0u);
		wb_mmr_write32(DMA_XCNT(ch[i]), 0u);
		wb_mmr_write32(DMA_XMOD(ch[i]), 0u);
		wb_mmr_write32(DMA_YCNT(ch[i]), 0u);
		wb_mmr_write32(DMA_YMOD(ch[i]), 0u);
	}
}

void opxy_testos_paint_log(struct opxy_master *m)
{
	unsigned i;

	log_m = m;
	line(m, OPXY_PAINT_RED, opxy_testos_entry[4]);
	for (i = 0; i < n_pending; i++)
		line(m, (enum opxy_testos_paint)pending[i][0], pending[i][1]);
	n_pending = 0;
}

#else

/* ISO C wants a declaration in every translation unit. */
typedef int opxy_testos_paint_not_built;

#endif /* OPXY_TEST_OS */
