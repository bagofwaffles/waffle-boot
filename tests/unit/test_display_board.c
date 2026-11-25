/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The board's display hooks (src/board/ui/display.c) over the BF70x panel
 * bus on the fake MMR, in the order the board calls them: the handshake,
 * held mode's start, the draw ops, a frame per render, the power-off that
 * sends nothing.
 * Requirements: S-13c.004, S-13c.007, S-13c.017, S-13c.021, S-13c.022,
 * S-13c.026, S-15.021.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/util/bytes.h"
#include "waffle/boot/device.h"
#include "waffle/board/boot.h"
/* opxy_display() also pulls src/board/ui/display.c out of the library
 * over the board's weak hooks */
#include "waffle/board/display.h"
#include "waffle/board/services.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_lcd.h"
#include "waffle/display/panel.h"
#include "waffle/ui/lines.h"
#include "s15_sha256.h"

/* tests/oracle/S-15-menus/vectors/handover_frame.json: the logo alone */
#define LOGO_SHA "9e50c21e72a8979051e31a3046e98383f6bf81db56a907a210847efd4455c717"

/*
 * The write hook: an enabled memory DMA destination completes at once;
 * the command bytes (D/C, PA5, low) and the frame DMA starts are counted
 * here, as the access log does not hold a frame's bytes.
 */
static uint8_t cmds[64];
static size_t ncmds;
static unsigned starts;
static int dc = -1;
static uint32_t dc_bit = 1u << 5;	/* D/C: PA5, PA11 on the OP-1F */
static bool stuck;		/* a frame's DMA never reports done */

static void clear(void)
{
	ncmds = 0;
	starts = 0;
}

static bool hook(void *ctx, enum fake_mmr_op op, unsigned width,
		 wb_mmr_addr addr, uint32_t *value)
{
	unsigned ch;

	(void)ctx;
	(void)width;
	if (op != FAKE_MMR_WRITE)
		return true;
	if (addr == PORT_DATA_CLR(WB_PORT_A) && (*value & dc_bit))
		dc = 0;
	else if (addr == PORT_DATA_SET(WB_PORT_A) && (*value & dc_bit))
		dc = 1;
	else if (addr == WB_LCD_SMC_BANK0 && dc == 0) {
		if (ncmds < sizeof cmds)
			cmds[ncmds] = (uint8_t)*value;
		ncmds++;
	}
	for (ch = 16; ch < WB_DMA_CHANNELS; ch++) {
		if (addr == DMA_STAT(ch)) {
			*value = fake_mmr_get(addr) & ~(*value & 7u);
			return true;
		}
		if (addr == DMA_CFG(ch) && (*value & DMA_CFG_EN) && (ch & 1u)) {
			if (!stuck)
				fake_mmr_set(DMA_STAT(ch), DMA_STAT_IRQDONE);
			starts++;
		}
	}
	return true;
}

static bool has(uint8_t cmd)
{
	size_t i;

	for (i = 0; i < ncmds && i < sizeof cmds; i++)
		if (cmds[i] == cmd)
			return true;
	return false;
}

static uint8_t last(void)
{
	return ncmds != 0 && ncmds <= sizeof cmds ? cmds[ncmds - 1u] : 0u;
}

static void reset(void)
{
	struct opxy_master *m = &opxy_master;

	fake_mmr_reset();
	fake_core_reset();
	fake_mmr_set_hook(hook, NULL);
	fake_core_set_cycle_step(4000);		/* 10 us a read */
	memset(m, 0, sizeof *m);
	m->hw.time.cycles_per_us = 400;
	m->strap = OPXY_STRAP_REV1;		/* PA8 high */
	dc = -1;
	dc_bit = 1u << 5;
	clear();
}

/* The OP-1F: no straps, no digital gamma, D/C on PA11 (S-18.030). */
static void reset_1f(void)
{
	reset();
	opxy_master.dev = &wb_device_op1f;
	opxy_master.strap = 0u;
	dc_bit = 1u << 11;
}

/* The record as S-13c.021 states it, with this reset time. */
static bool record_is(uint32_t reset_ms)
{
	const uint8_t *r = opxy_display_record();
	unsigned i, other = 0;

	for (i = 0; i < S13C_RECORD_SIZE; i++)
		if (i != 0x00 && i != 0x04 && i != 0x08 && i != 0x28)
			other += r[i] != 0u;
	return other == 0 && r[0x00] == 1u && r[0x28] == 1u &&
	       wb_get_le32(r + 0x04) == reset_ms && wb_get_le32(r + 0x08) == 2u;
}

static bool record_zero(void)
{
	const uint8_t *r = opxy_display_record();
	unsigned i, set = 0;

	for (i = 0; i < S13C_RECORD_SIZE; i++)
		set += r[i] != 0u;
	return set == 0;
}

static void record_stale(void)
{
	memset(opxy_display_record(), 0xA5, S13C_RECORD_SIZE);
}

/* The master's log on a sink of its own, its last lines kept as text. */
static char log_buf[512];
static size_t log_n;

static bool log_send(void *ctx, uint8_t route, uint8_t id, const uint8_t *p, size_t n)
{
	(void)ctx;
	(void)route;
	(void)id;
	for (size_t i = 0; i < n && log_n + 1u < sizeof log_buf; i++)
		log_buf[log_n++] = p[i] >= 0x20u && p[i] < 0x7Fu ? (char)p[i] : '.';
	log_buf[log_n] = '\0';
	return true;
}

static uint32_t log_now(void *ctx)
{
	(void)ctx;
	return 0u;
}

static const struct s14_link_ops log_link = { log_send, NULL };
static const struct s14_clock_ops log_clock = { log_now, NULL };

static void log_on(void)
{
	s14_log_init(&opxy_master.sv.log, S14_MASTER, &log_clock, NULL, &log_link, NULL);
	opxy_master.sv.log.cdc_on = true;
	log_n = 0;
	log_buf[0] = '\0';
}

static void test_held(void)
{
	struct opxy_master *m = &opxy_master;
	struct s15_draw_ops ops;
	struct s15_frame f;
	const struct s15_geometry *g;

	reset();
	/* as opxy_services_init() leaves it: the menu's frame reads the
	 * boot's values */
	s15_menu_init(&m->sv.menu, &wb_device_opxy, &m->sv.boot, &m->sv.env, NULL);
	m->sv.boot.version = "1.0.0";
	s15_console_append(&m->sv.menu.con, "held");

	CHECK(opxy_display() == NULL);
	opxy_display_handshake(0, 0);		/* no handshake: a pulse */
	CHECK(fake_mmr_get(SMC0_B0CTL) & SMC_CTL_EN);
	CHECK(opxy_display() != NULL &&
	      opxy_display()->panel.path == S13C_RESET_PULSE);
	g = opxy_display_geometry();
	CHECK(g != NULL && s15_geometry_ok(g));
	if (g == NULL)
		return;

	clear();
	opxy_display_held_start(m);
	CHECK(ncmds > 20 && cmds[0] == S13C_RDDID && has(S13C_SLPOUT));
	CHECK(last() == S13C_DISPON);
	CHECK(opxy_display()->panel.state == S13C_ON);
	CHECK(!has(S13C_DGR));			/* held: no gamma tables */

	opxy_display_draw_ops(m, &ops);
	CHECK(ops.fill != NULL && ops.ctx != &m->sv.drawn);
	clear();
	(void)opxy_display_geometry();		/* nothing drawn: no frame */
	CHECK(starts == 0 && ncmds == 0);
	s15_menu_frame(&m->sv.menu, &f);
	CHECK(f.version != NULL && strcmp(f.version, "1.0.0") == 0 &&
	      !f.option_on && !f.fatal);
	s15_render_console(&m->sv.menu.con, g, &ops, &f);
	CHECK(m->sv.drawn.fills == 0);		/* not the recorder */
	(void)opxy_display_geometry();		/* the getter sends nothing */
	CHECK(starts == 0);
	opxy_display_send();			/* after the render: the frame */
	CHECK(starts == 1 && ncmds == 1 && cmds[0] == S13C_RAMWR);

	/* drawn after the last frame: not sent at the power-off, and nothing
	 * else is either (S-13c.022) */
	s15_console_append(&m->sv.menu.con, "after");
	s15_render_console(&m->sv.menu.con, g, &ops, &f);
	clear();
	opxy_display_power_off(m);
	CHECK(starts == 0 && ncmds == 0);
	CHECK(opxy_display()->panel.state == S13C_OFF);
	opxy_display_send();
	CHECK(starts == 0 && ncmds == 0);
}

static void test_boot(void)
{
	struct opxy_master *m = &opxy_master;

	reset();
	fake_core_set_cycles(400u * 50000u);	/* 50 ms: after the first stage's 2 */
	opxy_display_handshake(S13C_HANDSHAKE_MAGIC, 2);
	CHECK(opxy_display()->panel.path == S13C_RESET_HANDSHAKE);
	clear();
	{	/* stale DDR where an OS keeps its frame (S-05.031, S-05.016) */
		uint8_t *os = (uint8_t *)(uintptr_t)opxy_display_os_frame();
		const size_t n = OPXY_OS_FRAME_END - OPXY_OS_FRAME_START;
		size_t i, wrong = 0, dirty = 0;
		unsigned c, r;
		struct s15_sha256 sha;
		char hex[65];

		memset(os, 0xA5, n);
		record_stale();
		opxy_display_boot_logo();
		CHECK(record_is(2));		/* S-13c.021 */
		/* the logo alone, as S-15's pixels give it, in the OS's layout:
		 * rows 512 bytes apart (S-05.016), low byte first */
		CHECK(OPXY_OS_FRAME_ROW == 512u);
		s15_sha256_init(&sha);
		for (r = 0; r < S13C_ROWS; r++) {
			const uint8_t *row = os + (size_t)r * OPXY_OS_FRAME_ROW;

			for (c = 0; c < S13C_FRAME_W; c++) {
				const uint16_t p = s15_handover_pixel(c, r);

				wrong += row[2u * c] != (uint8_t)p ||
					 row[2u * c + 1u] != (uint8_t)(p >> 8);
				s15_sha256_byte(&sha, row[2u * c + 1u]);
				s15_sha256_byte(&sha, row[2u * c]);
			}
			for (c = S13C_ROW_BYTES; c < OPXY_OS_FRAME_ROW; c++)
				dirty += row[c] != 0u;
		}
		CHECK(wrong == 0);
		s15_sha256_hex(&sha, hex);
		CHECK(strcmp(hex, LOGO_SHA) == 0);
		/* screen (59, 104), the first group's white square: row 420 */
		CHECK(os[420u * OPXY_OS_FRAME_ROW + 2u * 104u] == 0xFFu);
		for (i = (size_t)S13C_ROWS * OPXY_OS_FRAME_ROW; i < n; i++)
			dirty += os[i] != 0u;
		CHECK(dirty == 0);			/* black elsewhere */
	}
	CHECK(has(S13C_DGR) && has(S13C_DISPON));
	CHECK(last() == S13C_RAMWR && starts == 0);	/* the logo, by the CPU */
	/* a failed load: held mode finds the panel on */
	clear();
	opxy_display_held_start(m);
	CHECK(ncmds == 0);
}

/* S-04.012, S-05.016: reason 70's calibration screen, at screen (x, y) in
 * the OS's layout: rows OPXY_OS_FRAME_ROW bytes apart, low byte first. */
static const uint8_t *cal_os;

static unsigned cal_px(unsigned x, unsigned y)
{
	const uint8_t *p = cal_os + (size_t)(479u - x) * OPXY_OS_FRAME_ROW + 2u * y;

	return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static void test_calibration(void)
{
	struct opxy_master *m = &opxy_master;
	uint8_t *os;
	const size_t n = OPXY_OS_FRAME_END - OPXY_OS_FRAME_START;
	size_t i, bad = 0;
	unsigned r, c;

	reset();
	fake_core_set_cycles(400u * 50000u);
	opxy_display_handshake(S13C_HANDSHAKE_MAGIC, 2);
	os = (uint8_t *)(uintptr_t)opxy_display_os_frame();
	cal_os = os;
	memset(os, 0xA5, n);
	m->boot_reason = S04_REASON_OS_FRAME_CAL;
	opxy_display_boot_logo();
	m->boot_reason = 0u;
	/* the memory's first pixel is the top right corner's border */
	CHECK(os[0] == 0xFFu && os[1] == 0xFFu);
	/* the border on all four sides */
	CHECK(cal_px(0, 0) == 0xFFFFu && cal_px(479, 221) == 0xFFFFu);
	CHECK(cal_px(240, 0) == 0xFFFFu && cal_px(240, 221) == 0xFFFFu);
	CHECK(cal_px(0, 110) == 0xFFFFu && cal_px(479, 110) == 0xFFFFu);
	/* a tick at x 32, none at x 33 */
	CHECK(cal_px(32, 3) == 0xFFFFu && cal_px(33, 3) == 0u);
	/* the corners: red, green, blue, yellow */
	CHECK(cal_px(15, 15) == 0xF800u && cal_px(464, 15) == 0x07E0u);
	CHECK(cal_px(15, 206) == 0x001Fu && cal_px(464, 206) == 0xFFE0u);
	/* the stripes, in order */
	CHECK(cal_px(300, 47) == 0xF800u && cal_px(300, 67) == 0x07E0u);
	CHECK(cal_px(300, 87) == 0x001Fu && cal_px(300, 107) == 0xFFFFu);
	CHECK(cal_px(300, 127) == 0x5ACBu);
	CHECK(cal_px(240, 180) == 0u);			/* black between */
	for (r = 0; r < S13C_ROWS; r++) {
		const uint8_t *row = os + (size_t)r * OPXY_OS_FRAME_ROW;

		for (c = 222u; c < S13C_FRAME_W; c++)	/* not visible */
			bad += row[2u * c] != 0u || row[2u * c + 1u] != 0u;
		for (c = S13C_FRAME_W; c < OPXY_OS_FRAME_ROW / 2u; c++)
			bad += row[2u * c] != 0x1Fu || row[2u * c + 1u] != 0xF8u;
	}
	CHECK(bad == 0);				/* magenta past 224 */
	for (i = (size_t)S13C_ROWS * OPXY_OS_FRAME_ROW; i < n; i++)
		bad += os[i] != 0u;
	CHECK(bad == 0);				/* black after the rows */

	/* any other reason: the logo, with no border and no magenta */
	memset(os, 0xA5, n);
	opxy_display_boot_logo();
	CHECK(cal_px(0, 0) == 0u && cal_px(479, 221) == 0u);
	for (r = 0; r < S13C_ROWS; r++)
		for (c = S13C_ROW_BYTES; c < OPXY_OS_FRAME_ROW; c++)
			bad += os[(size_t)r * OPXY_OS_FRAME_ROW + c] != 0u;
	CHECK(bad == 0);
}

/* S-05.004: the early logo, then the hand-over frame or
 * held mode, with one initialization in the run (S-13c.013). */
static void test_early_logo(void)
{
	struct opxy_master *m = &opxy_master;

	reset();
	fake_core_set_cycles(400u * 50000u);
	opxy_display_handshake(S13C_HANDSHAKE_MAGIC, 2);
	clear();
	CHECK(opxy_display_early_logo());
	CHECK(has(S13C_SLPOUT) && has(S13C_DGR) && has(S13C_DISPON));
	CHECK(last() == S13C_RAMWR && starts == 0);	/* the logo, by the CPU */
	/* the hand-over: the early logo is the hand-over frame already, so
	 * nothing goes to the panel; the OS's frame memory is written */
	clear();
	memset((void *)(uintptr_t)opxy_display_os_frame(), 0xA5, OPXY_OS_FRAME_ROW);
	record_stale();
	CHECK(opxy_display_boot_logo());
	CHECK(ncmds == 0 && starts == 0);
	CHECK(record_is(2));
	CHECK(opxy_display_os_frame()[0] == 0u);
	/* held mode instead: the panel is on already */
	clear();
	opxy_display_held_start(m);
	CHECK(ncmds == 0);
	/* a frame of held mode's, then the hand-over: the logo again, no
	 * second initialization */
	{
		struct s15_draw_ops ops;

		opxy_display_draw_ops(m, &ops);
		ops.fill(ops.ctx, 0, 0, 8, 8, 0xFFFFu);
		opxy_display_show();
	}
	clear();
	CHECK(!opxy_display_boot_logo());
	CHECK(ncmds == 1 && cmds[0] == S13C_RAMWR && starts == 0);
}

/* S-05.004, S-13c.021, S-13c.026 on the OP-1F: no gamma load, the early
 * logo kept as the hand-over frame, and the record written. */
static void test_record_1f(void)
{
	reset_1f();
	fake_core_set_cycles(400u * 50000u);
	opxy_display_handshake(S13C_HANDSHAKE_MAGIC, 2);
	CHECK(opxy_display() != NULL && !opxy_display()->panel.digital_gamma);
	clear();
	CHECK(opxy_display_early_logo());
	CHECK(has(S13C_SLPOUT) && has(S13C_DISPON) && !has(S13C_DGR));
	CHECK(last() == S13C_RAMWR);
	clear();
	record_stale();
	CHECK(opxy_display_boot_logo());	/* kept: nothing sent */
	CHECK(ncmds == 0 && starts == 0);
	CHECK(record_is(2));

	/* no early logo: the hand-over frame goes out, then the record */
	reset_1f();
	fake_core_set_cycles(400u * 50000u);
	opxy_display_handshake(S13C_HANDSHAKE_MAGIC, 2);
	clear();
	record_stale();
	log_on();
	CHECK(!opxy_display_boot_logo());
	CHECK(has(S13C_DISPON) && !has(S13C_DGR) && last() == S13C_RAMWR);
	CHECK(record_is(2));
	CHECK(strstr(log_buf, "display record") == NULL);

	/* the hand-over frame refused (the panel powered off): no record,
	 * and a stale one cleared */
	reset_1f();
	fake_core_set_cycles(400u * 50000u);
	opxy_display_handshake(S13C_HANDSHAKE_MAGIC, 2);
	CHECK(opxy_display_early_logo());
	opxy_display_power_off(&opxy_master);
	clear();
	record_stale();
	log_on();
	CHECK(!opxy_display_boot_logo());
	CHECK(ncmds == 0 && starts == 0);
	CHECK(record_zero());
	CHECK(strstr(log_buf, "display record not written") != NULL);
}

/* S-01.023, S-05.004: the panel taken on from the front, which sent the
 * logo: this run's gamma load, then no hand-over frame of its own. */
static void test_adopted_logo(void)
{
	reset();
	fake_core_set_cycles(400u * 50000u);
	opxy_display_adopt(2u);
	CHECK(has(S13C_DGR) && !has(S13C_SLPOUT) && !has(S13C_RAMWR));
	clear();
	CHECK(opxy_display_boot_logo());
	CHECK(ncmds == 0 && starts == 0);
	/* a panel started by this run instead: the frame is sent */
	reset();
	fake_core_set_cycles(400u * 50000u);
	opxy_display_handshake(S13C_HANDSHAKE_MAGIC, 2);
	clear();
	CHECK(!opxy_display_boot_logo());
	CHECK(last() == S13C_RAMWR);
}

/* S-13c.025: the line under the logo, from the front's logo or this run's,
 * by the CPU; after it the hand-over frame is sent again (S-05.004), and no
 * line goes over a frame of held mode's. */
static void test_logo_line(void)
{
	struct opxy_master *m = &opxy_master;
	struct s15_draw_ops ops;

	reset();
	fake_core_set_cycles(400u * 50000u);
	opxy_display_adopt(2u);
	clear();
	CHECK(opxy_display_logo_line("Loading boot menu..."));
	CHECK(ncmds == 4 && cmds[0] == S13C_CASET && cmds[1] == S13C_RASET &&
	      cmds[2] == S13C_RAMWR && cmds[3] == S13C_CASET && starts == 0);
	clear();
	CHECK(!opxy_display_boot_logo());
	CHECK(ncmds == 1 && cmds[0] == S13C_RAMWR && starts == 0);

	reset();
	fake_core_set_cycles(400u * 50000u);
	opxy_display_handshake(S13C_HANDSHAKE_MAGIC, 2);
	CHECK(opxy_display_early_logo());
	clear();
	CHECK(opxy_display_logo_line("Loading boot menu..."));
	CHECK(last() == S13C_CASET && starts == 0);
	opxy_display_draw_ops(m, &ops);
	ops.fill(ops.ctx, 0, 0, 8, 8, 0xFFFFu);
	opxy_display_show();
	clear();
	CHECK(!opxy_display_logo_line("Loading boot menu..."));
	CHECK(ncmds == 0);
}

/* The restart's last frame: sent and waited for, and given up after the
 * frame's timeout when it never ends, so the caller always goes on. */
static void test_settle(void)
{
	struct opxy_master *m = &opxy_master;
	struct s15_draw_ops ops;
	struct s15_frame f;
	const struct s15_geometry *g;

	reset();
	s15_menu_init(&m->sv.menu, &wb_device_opxy, &m->sv.boot, &m->sv.env, NULL);
	m->sv.boot.version = "1.0.0";
	opxy_display_handshake(0, 0);
	g = opxy_display_geometry();
	if (g == NULL)
		return;
	opxy_display_held_start(m);
	opxy_display_draw_ops(m, &ops);
	s15_menu_frame(&m->sv.menu, &f);

	s15_console_append(&m->sv.menu.con, "Restarting...");
	s15_render_console(&m->sv.menu.con, g, &ops, &f);
	clear();
	opxy_display_settle();			/* drawn: one frame, ended */
	CHECK(starts == 1 && !opxy_display()->panel.frame);
	clear();
	opxy_display_settle();			/* nothing drawn: no frame */
	CHECK(starts == 0);

	stuck = true;
	s15_render_console(&m->sv.menu.con, g, &ops, &f);
	clear();
	opxy_display_settle();			/* returns all the same */
	CHECK(starts == 1 && opxy_display()->panel.frame);
	stuck = false;
}

/* A long call starts with its screen on the panel: rendered, sent and
 * the frame ended before the request (here a format with no slave). */
static void test_long_call_shown(void)
{
	struct opxy_master *m = &opxy_master;

	reset();
	s09_master_set_absent(&m->bridge);
	s09_files_init(&m->sv.files, &m->bridge);
	opxy_menu_env(m, &m->sv.env);
	s15_menu_init(&m->sv.menu, &wb_device_opxy, &m->sv.boot, &m->sv.env, NULL);
	m->sv.boot.version = "1.0.0";
	opxy_display_handshake(0, 0);
	opxy_display_held_start(m);
	opxy_display_draw_ops(m, &m->sv.draw);
	opxy_menu_show(m);
	CHECK(!opxy_display()->dirty);		/* drawn and sent at once */

	s15_console_append(&m->sv.menu.con, "Formatting...");
	clear();
	(void)m->sv.env.format(m->sv.env.ctx);
	CHECK(starts == 1 && !opxy_display()->panel.frame &&
	      !opxy_display()->dirty &&
	      m->sv.menu_shown == s15_menu_changes(&m->sv.menu));
}

/* The display's draw ops behind a wrapper that notes the frame starts
 * seen at each draw operation of a render. */
static struct s15_draw_ops inner;
static unsigned n_ops, first_starts, mixed;

static void seen(void)
{
	if (n_ops++ == 0u)
		first_starts = starts;
	else if (starts != first_starts)
		mixed++;
}

static void w_fill(void *ctx, uint16_t x, uint16_t y, uint16_t w,
		   uint16_t h, uint16_t rgb565)
{
	seen();
	inner.fill(ctx, x, y, w, h, rgb565);
}

static void w_text(void *ctx, uint16_t x, uint16_t y, const char *s,
		   size_t n, uint16_t fg, uint16_t bg)
{
	seen();
	inner.text(ctx, x, y, s, n, fg, bg);
}

static void w_ink(void *ctx, uint16_t x, uint16_t y, const char *s,
		  size_t n, uint16_t fg)
{
	seen();
	inner.ink(ctx, x, y, s, n, fg);
}

static void w_bold(void *ctx, uint16_t x, uint16_t y, const char *s,
		   size_t n, uint16_t fg)
{
	seen();
	inner.bold(ctx, x, y, s, n, fg);
}

/*
 * S-13c.024: a frame starts only on a whole image. Held mode's show
 * (opxy_menu_show()) draws the whole screen and then sends it: no frame
 * starts between a render's first draw operation and its last, and one
 * starts after it; so too when the frame before it never ends, which the
 * render's first draw operation stops after its timeout.
 */
static void test_render_whole(void)
{
	struct opxy_master *m = &opxy_master;
	unsigned i;

	reset();
	s09_master_set_absent(&m->bridge);
	s09_files_init(&m->sv.files, &m->bridge);
	opxy_menu_env(m, &m->sv.env);
	s15_menu_init(&m->sv.menu, &wb_device_opxy, &m->sv.boot, &m->sv.env, NULL);
	m->sv.boot.version = "1.0.0";
	opxy_display_handshake(0, 0);
	if (opxy_display_geometry() == NULL)
		return;
	opxy_display_held_start(m);
	opxy_display_draw_ops(m, &inner);
	CHECK(inner.ink != NULL && inner.bold != NULL);
	m->sv.draw = (struct s15_draw_ops){ w_fill, w_text, inner.ctx, w_ink, w_bold };
	s15_menu_start(&m->sv.menu);
	for (i = 0; i < 14u; i++) {
		if (i == 6u) {
			s15_menu_key(&m->sv.menu, S15_KEY_HELP, true);
			s15_menu_key(&m->sv.menu, S15_KEY_HELP, false);
		} else {
			s15_console_append(&m->sv.menu.con, "a line");
		}
		stuck = i >= 10u;		/* the frame sent never ends */
		n_ops = mixed = 0;
		clear();
		opxy_menu_show(m);
		CHECK(n_ops > 0u && mixed == 0u && first_starts == 0u);
		CHECK(starts == 1u);
	}
	stuck = false;
}

int main(void)
{
	RUN(test_held);
	RUN(test_long_call_shown);
	RUN(test_boot);
	RUN(test_calibration);
	RUN(test_early_logo);
	RUN(test_record_1f);
	RUN(test_adopted_logo);
	RUN(test_logo_line);
	RUN(test_settle);
	RUN(test_render_whole);
	return check_summary("test_display_board");
}
