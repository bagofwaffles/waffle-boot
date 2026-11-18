/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The master's services (src/board/opxy_services.c, opxy_usb.c) on the
 * host: the log's PSoC sink and the dropped-frame line, the draw recorder
 * S-15 renders into, the USB sets and host links, the RAM disk, and the
 * weak builders' refusals.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"
#include "ff.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_bridge.h"
#include "waffle/board/opxy_rtc_board.h"
#include "waffle/drivers/bf70x_usb.h"
#include "waffle/s06_frame.h"
#include "waffle/s10a_msc.h"
#include "waffle/s11_version.h"
#include "waffle/s17_revisions.h"
#include "waffle/wire_crc.h"

#include "device/dcd.h"

/* The bootloader's own version as the screens and reports write it, so a
 * version bump (make bump-version) changes no expectation here. */
static const char *bl_text(void)
{
	static char t[24];

	snprintf(t, sizeof(t), "%u.%u.%u", OPXY_BL_VERSION_MAJOR,
		 OPXY_BL_VERSION_MINOR, OPXY_BL_VERSION_PATCH);
	return t;
}

/* S-17.032: the bootloader's own version is one of its own, never S-11's
 * forced marker. */
static void test_own_version(void)
{
	const struct s11_version v = { OPXY_BL_VERSION_MAJOR, OPXY_BL_VERSION_MINOR,
				       OPXY_BL_VERSION_PATCH, 0 };

	CHECK(s17_own_version_ok(&v) && !s11_version_is_forced_marker(&v));
}

static struct opxy_master m;

/*
 * S-02.095's finished mark as the flash fake sees it (fl_program): what
 * had happened when it came. A loop that polls the PSoC more than
 * loop_limit times leaves by mark_jmp, so a wait that never ends ends the
 * test.
 */
static struct {
	unsigned n;
	uint32_t off;
	uint8_t value;
	size_t wire, inject;	/* wire_n and inject_next then */
} lm;
static jmp_buf mark_jmp;
static unsigned loop_limit, loop_reads;

/* The PSoC port: everything written; received only what a test injects
 * (one frame per poll of the link, so that a poll between two frames is
 * a step the test controls). */
static uint8_t wire[4096];
static bool wire_usb[4096];	/* the device attached when the byte was sent */
static bool wire_phy[4096];	/* the USB PHY on when the byte was sent */
static size_t wire_mmr[4096];	/* fake_mmr_count() when the byte was sent */
static size_t wire_n;
#define INJECT_MAX 8
static uint8_t inject_wire[INJECT_MAX][S06_MAX_WIRE];
static size_t inject_len[INJECT_MAX], inject_n, inject_next, inject_calls;

static size_t port_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t n;

	(void)ctx;
	if (loop_limit != 0u && ++loop_reads > loop_limit)
		longjmp(mark_jmp, 2);
	/* a poll reads until nothing comes: the first read of a poll gives
	 * the next frame, the second ends the poll */
	if (inject_next >= inject_n || (inject_calls++ % 2u) != 0u)
		return 0;
	n = inject_len[inject_next];
	if (n > max)
		n = max;
	memcpy(buf, inject_wire[inject_next], n);
	inject_next++;
	return n;
}

static size_t port_room(void *ctx)
{
	(void)ctx;
	return sizeof(wire) - wire_n;
}

static bool usb_phy_on(void)
{
	return (fake_mmr_get(USB0_PHY_CTL) & USB_PHY_CTL_EN) != 0u;
}

static void port_write(void *ctx, const uint8_t *data, size_t n)
{
	size_t i;

	(void)ctx;
	memcpy(wire + wire_n, data, n);
	for (i = 0; i < n; i++) {
		wire_usb[wire_n + i] = wb_usb_attached();
		wire_phy[wire_n + i] = usb_phy_on();
		wire_mmr[wire_n + i] = fake_mmr_count();
	}
	wire_n += n;
}

static const struct s06_port_ops port = { port_read, port_room, port_write };

static uint8_t crc8(const uint8_t *p, size_t n)
{
	uint8_t c = 0;

	while (n--)
		c = (uint8_t)(c ^ *p++);
	return c;
}

/* A frame from the PSoC (route 0x41), received at the next poll. */
static void inject(uint8_t id, const uint8_t *payload, size_t n)
{
	if (inject_n < INJECT_MAX) {
		static uint8_t seq;

		inject_len[inject_n] = s06_frame_encode(0x41u, seq++, id, payload,
							n, crc8,
							inject_wire[inject_n]);
		inject_n++;
	}
}

/* Where s first stands in the wire, or the wire's end. */
static size_t wire_find(const char *s)
{
	size_t i, n = strlen(s);

	for (i = 0; i + n <= wire_n; i++)
		if (memcmp(wire + i, s, n) == 0)
			return i;
	return wire_n;
}

static bool wire_has_bytes(const void *b, size_t n)
{
	size_t i;

	for (i = 0; i + n <= wire_n; i++)
		if (memcmp(wire + i, b, n) == 0)
			return true;
	return false;
}

static bool wire_has(const char *s)
{
	return wire_has_bytes(s, strlen(s));
}

/* Where the wire's last frame starts: after the delimiter (00) before
 * its own. */
static size_t last_frame_at(void)
{
	size_t i = wire_n - 1u;

	while (i > 0u && wire[i - 1u] != 0u)
		i--;
	return i;
}

static int mmr_last(enum fake_mmr_op op, wb_mmr_addr addr, uint32_t value,
		    bool any_value);

static void setup(void)
{
	opxy_usb_stop();		/* a controller another test left up */
	memset(&m, 0, sizeof(m));
	s02_store_defaults(&m.sv.store);	/* as the bring-up leaves them */
	wire_n = 0;
	inject_n = inject_next = inject_calls = 0;
	fake_mmr_reset();
	fake_core_reset();
	m.hw.time = opxy_time;
	m.clock = opxy_clock(&m.hw);
	s09_master_set_absent(&m.bridge);	/* no slave: S-09.008 */
	s09_files_init(&m.sv.files, &m.bridge);
	s06_node_init(&m.node, crc8, &m.clock);
	s06_link_init(&m.link[S06_LINK_PSOC], &port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_PSOC);
	m.node.link[S06_LINK_PSOC] = &m.link[S06_LINK_PSOC];
	opxy_usb_host_ops(&m, &m.sv.host_ops);
	/* the target's order: the start-up read, then the PSoC link */
	opxy_startup_read(&m, opxy_otp_get);
	opxy_log_start(&m);
}

/* S-14.022: the PSoC sink is on from the start; the CDC sink waits for
 * the USB device port, so its line is not taken there. */
static void test_log_sinks(void)
{
	setup();
	CHECK(m.sv.log.psoc_on);
	s14_log_str(&m.sv.log, S14_ERROR, S14_NO_TAG, "hello psoc");
	CHECK(wire_n > 0);
	CHECK(wire_has("hello psoc"));
	CHECK(m.sv.log_cdc.send(m.sv.log_cdc.ctx, S14_ROUTE_CDC, 3u,
				(const uint8_t *)"x", 1u) == false);
	/* The menus' console is the screen sink, and the menus wait for the
	 * first screen (S-15.013). */
	CHECK(m.sv.menu.screen == S15_SCR_NONE);
	CHECK(m.sv.log_screen.ctx == &m.sv.menu.con);
	CHECK(m.node.obs.ctx == &m);
}

/* S-16.020: a frame with a bad check is logged with its route, seq, id. */
static void test_crc_drop_logged(void)
{
	const uint8_t raw[] = { 0x41, 0x07, 0x2A, 0x00, 0x99 };

	setup();
	m.node.obs.dropped(m.node.obs.ctx, S06_LINK_PSOC, S06_DROP_CRC, raw,
			   sizeof(raw));
	CHECK(wire_n > 0);
	wire_n = 0;
	m.node.obs.dropped(m.node.obs.ctx, S06_LINK_PSOC, S06_DROP_LONG, raw,
			   sizeof(raw));
	CHECK(wire_n == 0);
	m.node.obs.dropped(m.node.obs.ctx, S06_LINK_PSOC, S06_DROP_CRC, NULL,
			   0u);
	CHECK(wire_n == 0);
}

static void test_draw_recorder(void)
{
	struct opxy_draw_rec r;
	struct s15_draw_ops d;
	struct s15_frame f;
	const struct s15_geometry g = { 480, 222, 8, 16, 27, 19, 8, 16, 23, 202,
					 10, 17, 4 };

	setup();
	opxy_draw_record(&r, &d);
	s15_console_append(&m.sv.menu.con, "abc");
	/* the menu's frame: its boot values are the services' own */
	s15_menu_frame(&m.sv.menu, &f);
	s15_render_console(&m.sv.menu.con, &g, &d, &f);
	CHECK(r.fills >= 3u);		/* black, the title bar, the rule */
	CHECK(r.texts >= 5u);		/* the title, the status words, the rows */
	/* the last call: the body's row, after the bars */
	CHECK(r.text_len == 3u && memcmp(r.text, "abc", 3) == 0);
	CHECK(r.x == g.margin && r.y == g.top && r.color == S15_TEXT);
}

/* Without a display geometry nothing is drawn. */
static void test_poll_without_display(void)
{
	setup();
	opxy_draw_record(&m.sv.drawn, &m.sv.draw);
	s15_console_append(&m.sv.menu.con, "line");
	opxy_services_poll(&m);
	CHECK(m.sv.drawn.fills == 0u && m.sv.drawn.texts == 0u);
}

/* Held mode's USB: S-10c's host, then the links and the controller, its
 * PLL reporting stable at once. */
/*
 * The RAM disk in host memory: all of S-12.001's sectors, in two parts
 * split near the start so that the format's sectors cross the split, with
 * a gap of GAP sectors of 0xEE between them that nothing may touch.
 */
#define SPLIT 100u
#define GAP 8u
static uint8_t *disk_mem;

static uint8_t *disk_sector(uint32_t lba)
{
	return disk_mem + (size_t)(lba < SPLIT ? lba : lba + GAP) * 512u;
}

static bool gap_intact(void)
{
	size_t i;

	for (i = 0; i < GAP * 512u; i++)
		if (disk_mem[SPLIT * 512u + i] != 0xEEu)
			return false;
	return true;
}

static void host_disk(void)
{
	struct opxy_disk_part p[OPXY_DISK_PARTS];

	if (disk_mem == NULL)
		disk_mem = calloc(OPXY_DISK_SECTORS + GAP, 512u);
	CHECK(disk_mem != NULL);
	if (disk_mem == NULL)
		exit(1);
	memset(disk_mem, 0, 207u * 512u);
	memset(disk_mem + SPLIT * 512u, 0xEE, GAP * 512u);
	p[0].mem = disk_mem;
	p[0].sectors = SPLIT;
	p[1].mem = disk_sector(SPLIT);
	p[1].sectors = OPXY_DISK_SECTORS - SPLIT;
	opxy_disk_init(&m.sv.disk, p);
}

static void usb_setup(void)
{
	setup();
	s10c_host_init(&m.sv.host, &m.sv.host_ops, &m);
	opxy_usb_start(&m);
	host_disk();
}

/* S-10a.010, S-10a.011: S-15's sets as states, and what the serial flag
 * adds to them. */
static void test_usb_sets(void)
{
	enum s10a_set set;

	CHECK(opxy_usb_state(S15_USB_MAIN) == S10A_STATE_MAIN);
	CHECK(opxy_usb_state(S15_USB_DISK) == S10A_STATE_UPGRADE);
	CHECK(opxy_usb_state(S15_USB_MTP_VENDOR) ==
	      S10A_STATE_FACTORY_MTP_VENDOR);
	CHECK(opxy_usb_state(S15_USB_MTP) == S10A_STATE_FACTORY_MTP);
	CHECK(opxy_usb_state(S15_USB_SERIAL) == S10A_STATE_FACTORY_MTP_OFF);
	CHECK(s10a_set_for_state(opxy_usb_state(S15_USB_DISK), true, false,
				 &set) && set == S10A_SET_DISK_CDC);
	CHECK(s10a_set_for_state(opxy_usb_state(S15_USB_MAIN), false, false,
				 &set) && set == S10A_SET_MIDI);
	CHECK(s10a_set_for_state(S10A_STATE_BATTERY, true, true, &set) &&
	      set == S10A_SET_DISK);
	CHECK(s10a_set_for_state(opxy_usb_state(S15_USB_MTP), true, false,
				 &set) && set == S10A_SET_MTP);

	setup();
	m.boot_reason = 10u;
	CHECK(opxy_serial_flag(&m));
	m.boot_reason = 30u;
	m.sv.provisioned = true;
	CHECK(!opxy_serial_flag(&m));
	/* Developer mode as it is now (S-15.172) and the debug key add it on
	 * their own. */
	m.sv.menu.dev_mode = true;
	CHECK(opxy_serial_flag(&m));
	m.sv.menu.dev_mode = false;
	m.debug_key = true;
	CHECK(opxy_serial_flag(&m));
	m.debug_key = false;
	CHECK(!opxy_serial_flag(&m));
}

/* The controller comes up; a state that starts a set attaches, one that
 * does not leaves the device as it is. */
static void test_usb_select(void)
{
	usb_setup();
	CHECK(opxy_usb_up());
	CHECK(!wb_usb_attached());
	opxy_usb_select(&m, S10A_STATE_UPGRADE_KEEP);
	CHECK(!wb_usb_attached());
	opxy_usb_select(&m, S10A_STATE_MAIN);
	CHECK(wb_usb_attached());
	/* S-07.057: not configured, so any current S-10a reports is the
	 * unconfigured unit load. */
	m.psoc.node = &m.node;
	m.sv.usb_board.input_current(m.sv.usb_board.ctx, 500u);
	CHECK(!m.psoc.usb_configured && m.psoc.target_ma == 100u);
	/* S-07.054: USB activity restarts S-07's count and
	 * leaves the target (S-10a.081). */
	CHECK(m.sv.usb_board.suspend == NULL);
	m.psoc.last_usb_us = 0u;
	fake_core_set_cycles(400u * 5000u);
	m.sv.usb_board.activity(m.sv.usb_board.ctx);
	CHECK(m.psoc.last_usb_us != 0u && m.psoc.target_ma == 100u);
	opxy_usb_stop();
	CHECK(!wb_usb_attached());
	CHECK(!opxy_usb_up());
}

/* S-10a.011: with developer mode on every held-mode set carries the serial
 * port, the MTP sets included, and turning MTP off keeps it; with it off the
 * MTP sets are MTP alone, whatever else sets the serial flag. The battery
 * wait's set never has it. */
static void test_usb_dev_serial(void)
{
	usb_setup();
	m.sv.provisioned = true;
	m.debug_key = true;
	m.sv.menu.dev_mode = false;
	opxy_usb_select(&m, opxy_usb_state(S15_USB_MTP));
	CHECK(opxy_usb_set() == S10A_SET_MTP);
	opxy_usb_select(&m, opxy_usb_state(S15_USB_MTP_VENDOR));
	CHECK(opxy_usb_set() == S10A_SET_MTP_VENDOR);
	opxy_usb_select(&m, S10A_STATE_MAIN);
	CHECK(opxy_usb_set() == S10A_SET_MIDI_CDC);
	m.debug_key = false;
	m.sv.menu.dev_mode = true;
	opxy_usb_select(&m, opxy_usb_state(S15_USB_MTP));
	CHECK(opxy_usb_set() == S10A_SET_MTP_CDC);
	CHECK(wb_usb_attached());
	opxy_usb_select(&m, opxy_usb_state(S15_USB_SERIAL));
	CHECK(opxy_usb_set() == S10A_SET_SERIAL);
	opxy_usb_select(&m, opxy_usb_state(S15_USB_MTP_VENDOR));
	CHECK(opxy_usb_set() == S10A_SET_MTP_VENDOR_CDC);
	opxy_usb_select(&m, opxy_usb_state(S15_USB_DISK));
	CHECK(opxy_usb_set() == S10A_SET_DISK_CDC);
	opxy_usb_select(&m, S10A_STATE_BATTERY);
	CHECK(opxy_usb_set() == S10A_SET_DISK);
	opxy_usb_stop();
}

/* S-10c.021, Q-C10: a carrier message's frame for the master reaches
 * S-10c; a restart keeps the frames the last message completed. */
static void test_midi_carrier_to_host(void)
{
	uint8_t a[S06_MAX_WIRE], b[S06_MAX_WIRE];
	const uint8_t route = s06_route(S06_ADDR_HOST, S06_ADDR_MASTER);
	size_t na, nb;

	usb_setup();
	CHECK(m.node.link[S06_LINK_MIDI] == &m.link[S06_LINK_MIDI]);
	na = s06_frame_encode(route, 0u, 0x01u, NULL, 0u, crc8, a);
	nb = s06_frame_encode(route, 1u, 0x01u, NULL, 0u, crc8, b);
	m.sv.host_ops.midi_rx_restart(&m);
	m.sv.host_ops.midi_rx_feed(&m, a, na);
	m.sv.host_ops.midi_rx_restart(&m);
	m.sv.host_ops.midi_rx_feed(&m, b, nb);
	/* the restart read the first; the second waits for the loop's pass */
	s06_node_poll(&m.node);
	CHECK(m.link[S06_LINK_MIDI].in_count == 1u);
	opxy_usb_poll(&m);
	CHECK(m.link[S06_LINK_MIDI].in_count == 0u);
}

/* S-10c.031: with the port closed the CDC link takes nothing. */
static void test_cdc_closed_drops(void)
{
	const struct s06_link *l;

	usb_setup();
	l = &m.link[S06_LINK_CDC];
	CHECK(m.node.link[S06_LINK_CDC] == l);
	CHECK(!m.sv.host_ops.cdc_open(&m));
	CHECK(l->ops->room(l->ctx) == 0u);
	m.sv.host_ops.send_frame(&m, S10C_LINK_CDC, 0u, 0x01u, NULL, 0u);
	CHECK(s10a_usb_cdc_pending() == 0u);
	CHECK(!s06_send(&m.node, S06_LINK_CDC, 0u, 0x01u, NULL, 0u));
}

/* --- The host on the USB controller, as the BF70x driver sees it ---------- */

#define MIDI_IN_EP 7u		/* S-10a's MIDI set: 0x87 and 0x08 */
#define MIDI_OUT_EP 8u

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

/* One interrupt: endpoint 0, an IN or an OUT endpoint. */
static void usb_irq(uint8_t irq, uint16_t tx, uint16_t rx)
{
	fake_mmr_queue_read(USB0_IRQ, irq);
	fake_mmr_queue_read(USB0_INTRTX, tx);
	fake_mmr_queue_read(USB0_INTRRX, rx);
	dcd_int_handler(0);
}

/* A bus reset at high speed, then SET_CONFIGURATION(1) and its status
 * stage, each serviced by the device stack. */
static void usb_configure(void)
{
	static const uint8_t set_config[8] = { 0x00, 9, 1, 0, 0, 0, 0, 0 };

	fake_mmr_set(USB0_POWER, USB_POWER_HSEN | USB_POWER_HSMODE |
		     USB_POWER_SOFTCONN);
	usb_irq(USB_IRQ_RSTBABBLE, 0, 0);
	wb_usb_task();
	fake_mmr_queue_read(USB0_EP0_CSR, USB_CSR0_RXPKTRDY);
	fake_mmr_queue_read(USB0_EP0_CNT, 8u);
	fake_mmr_queue_read(USB0_FIFO(0), le32(set_config));
	fake_mmr_queue_read(USB0_FIFO(0), le32(set_config + 4));
	usb_irq(0, 1u, 0);
	wb_usb_task();
	fake_mmr_queue_read(USB0_EP0_CSR, 0);
	usb_irq(0, 1u, 0);
	wb_usb_task();
	fake_mmr_clear_log();
}

/* An OUT packet of n bytes (a multiple of 4) on endpoint epn. */
static void usb_out_packet(unsigned epn, const uint8_t *p, unsigned n)
{
	unsigned i;

	fake_mmr_queue_read(USB0_EP_RXCSR(epn), USB_RXCSR_RXPKTRDY);
	fake_mmr_queue_read(USB0_EP_RXCNT(epn), n);
	for (i = 0; i < n; i += 4u)
		fake_mmr_queue_read(USB0_FIFO(epn), le32(p + i));
	usb_irq(0, 0, (uint16_t)(1u << epn));
}

/* A host that takes the MIDI IN packets, from the cycle reads on: each
 * packet the driver has made ready is taken and its interrupt raised. */
static bool host_reading;
static unsigned host_packets;

static void midi_host(void *ctx)
{
	(void)ctx;
	fake_mmr_clear_log();
	if (!host_reading ||
	    !(fake_mmr_get(USB0_EP_TXCSR(MIDI_IN_EP)) & USB_TXCSR_TXPKTRDY))
		return;
	fake_mmr_set(USB0_EP_TXCSR(MIDI_IN_EP), 0);
	host_packets++;
	usb_irq(0, (uint16_t)(1u << MIDI_IN_EP), 0);
}

/*
 * S-10c.033 on MIDI: a SysEx message or a frame that does not fit what is
 * queued waits for the host, len / 10 + 110 ms at least, the bus serviced
 * meanwhile and nothing that arrives handed to S-10c; then it is given up
 * whole. A host that reads on gets it.
 */
static void test_midi_send_waits(void)
{
	static const uint8_t identity[8] = { 0x04, 0xF0, 0x7E, 0x7F,
					     0x07, 0x06, 0x01, 0xF7 };
	static uint8_t msg[S10C_SYSEX_MAX_SENT];
	const uint8_t route = s06_route(S06_ADDR_MASTER, S06_ADDR_HOST);
	const size_t need = s10a_usb_sysex_size(sizeof(msg));
	const uint8_t payload[200] = { 0 };
	size_t room, i;
	uint64_t t0, waited;

	usb_setup();
	m.psoc.node = &m.node;		/* S-07.057's target, on configuration */
	m.sv.provisioned = true;
	opxy_usb_select(&m, S10A_STATE_MAIN);
	CHECK(opxy_usb_set() == S10A_SET_MIDI);
	usb_configure();
	CHECK(s10a_usb_midi_open());
	for (i = 0; i < sizeof(msg); i++)
		msg[i] = (uint8_t)(i & 0x7Fu);
	msg[0] = 0xF0;
	msg[sizeof(msg) - 1] = 0xF7;
	while (s10a_usb_midi_room() >= need)
		s10a_usb_send_sysex(NULL, msg, sizeof(msg));
	room = s10a_usb_midi_room();
	/* an identity request arrives; only the loop's pass answers it */
	usb_out_packet(MIDI_OUT_EP, identity, sizeof(identity));
	fake_core_set_cycle_step(40000u);	/* 0.1 ms a read */
	host_reading = false;
	host_packets = 0;
	fake_core_set_cycle_hook(midi_host, NULL);

	/* A host that reads nothing. */
	t0 = wb_time_ms(&m.hw.time);
	m.sv.host_ops.send_sysex(&m, msg, sizeof(msg));
	waited = wb_time_ms(&m.hw.time) - t0;
	CHECK(waited >= s10c_write_timeout_ms(need));
	CHECK(waited < s10c_write_timeout_ms(need) + 5u);
	CHECK(s10a_usb_midi_room() == room);
	t0 = wb_time_ms(&m.hw.time);
	m.sv.host_ops.send_frame(&m, S10C_LINK_MIDI, route, 0x01u, payload,
				 sizeof(payload));
	waited = wb_time_ms(&m.hw.time) - t0;
	CHECK(waited >= s10c_write_timeout_ms(sizeof(payload)));
	CHECK(s10a_usb_midi_room() == room);

	/* A host that reads on: the message goes as soon as it fits. */
	host_reading = true;
	t0 = wb_time_ms(&m.hw.time);
	m.sv.host_ops.send_sysex(&m, msg, sizeof(msg));
	host_reading = false;
	waited = wb_time_ms(&m.hw.time) - t0;
	CHECK(host_packets > 0u && waited < s10c_write_timeout_ms(need));
	CHECK(s10a_usb_midi_room() + need <= 1024u);
	t0 = wb_time_ms(&m.hw.time);
	host_reading = true;
	m.sv.host_ops.send_frame(&m, S10C_LINK_MIDI, route, 0x01u, payload,
				 sizeof(payload));
	host_reading = false;
	CHECK(wb_time_ms(&m.hw.time) - t0 <
	      s10c_write_timeout_ms(sizeof(payload)));

	/* The identity request waited for the loop's pass. */
	host_reading = true;
	for (i = 0; i < 100u && (s10a_usb_midi_room() < 1024u ||
		     fake_mmr_get(USB0_EP_TXCSR(MIDI_IN_EP))); i++) {
		(void)wb_time_ms(&m.hw.time);
		wb_usb_task();
	}
	host_reading = false;
	CHECK(s10a_usb_midi_room() == 1024u);
	opxy_usb_poll(&m);
	CHECK(fake_mmr_get(USB0_EP_TXCSR(MIDI_IN_EP)) & USB_TXCSR_TXPKTRDY);
	fake_core_set_cycle_hook(NULL, NULL);
	opxy_usb_stop();
}

/* The display: none unless a test sets one (the weak default). */
static const struct s15_geometry *display_geometry;

const struct s15_geometry *opxy_display_geometry(void)
{
	return display_geometry;
}

/* Held mode: the services, then the first screen for the boot h gives. */
static void held_setup(void)
{
	setup();
	opxy_services_start(&m);
	host_disk();
}

/* The disk's parts in DDR hold S-12.001's sectors (S-01.016). */
static void test_disk_ddr(void)
{
	struct opxy_disk_part p[OPXY_DISK_PARTS];
	struct opxy_disk d;

	opxy_disk_ddr(p);
	opxy_disk_init(&d, p);
	CHECK(d.dev.n_sectors == OPXY_DISK_SECTORS);
	CHECK((uintptr_t)p[0].mem == OPXY_DISK0_START);
	CHECK((uintptr_t)p[1].mem == OPXY_DISK1_START);
}

/* Reads, writes and erases that cross from one part into the next, and
 * ranges that leave the disk. */
static void test_disk_parts(void)
{
	uint8_t mem[10 * 512];
	uint8_t buf[4 * 512], back[4 * 512];
	struct opxy_disk_part p[OPXY_DISK_PARTS];
	struct opxy_disk d;
	size_t i;

	memset(mem, 0xEE, sizeof(mem));
	p[0].mem = mem;			/* sectors 0-2 */
	p[0].sectors = 3;
	p[1].mem = mem + 5 * 512;	/* sectors 3-7, after a gap of 2 */
	p[1].sectors = 5;
	opxy_disk_init(&d, p);
	CHECK(d.dev.n_sectors == 8u && d.dev.ctx == &d);
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (uint8_t)i;
	CHECK(d.dev.write(d.dev.ctx, 1, buf, 4) == 0);	/* 1-2, then 3-4 */
	CHECK(memcmp(mem + 512, buf, 1024) == 0);
	CHECK(memcmp(mem + 5 * 512, buf + 1024, 1024) == 0);
	CHECK(mem[3 * 512] == 0xEE && mem[5 * 512 - 1] == 0xEE);
	CHECK(d.dev.read(d.dev.ctx, 1, back, 4) == 0);
	CHECK(memcmp(back, buf, sizeof(buf)) == 0);
	CHECK(d.dev.erase(d.dev.ctx, 2, 2) == 0);	/* 2, then 3 */
	CHECK(mem[2 * 512] == 0 && mem[3 * 512 - 1] == 0);
	CHECK(mem[5 * 512] == 0 && mem[6 * 512 - 1] == 0);
	CHECK(mem[6 * 512] == buf[1536] && mem[3 * 512] == 0xEE);
	CHECK(d.dev.read(d.dev.ctx, 7, back, 1) == 0);
	CHECK(d.dev.read(d.dev.ctx, 7, back, 2) != 0);
	CHECK(d.dev.write(d.dev.ctx, 8, buf, 1) != 0);
	CHECK(d.dev.erase(d.dev.ctx, 0, 9) != 0);
	CHECK(d.dev.read(d.dev.ctx, 0xFFFFFFFFu, back, 2) != 0);
}

/*
 * S-10a.010, S-12.002: the upgrade screen's set formats the disk once in
 * the boot, then offers it; entering it again keeps the volume; the
 * battery wait's set has no medium; S-11.050 offers it again.
 */
static void test_disk_offer(void)
{
	static const struct s15_geometry frame = { 480, 222, 8, 16, 27, 19, 8, 16,
						   23, 202, 10, 17, 4 };
	uint32_t fills;

	held_setup();
	CHECK(!m.sv.disk_formatted);
	opxy_usb_select(&m, S10A_STATE_BATTERY);
	CHECK(wb_usb_attached() && !s10a_usb_disk()->offered);
	CHECK(!m.sv.disk_formatted);

	/* S-15.100: the screen is drawn by the first format, not after it. */
	display_geometry = &frame;
	s15_console_append(&m.sv.menu.con, "abc");
	fills = m.sv.drawn.fills;
	opxy_usb_select(&m, S10A_STATE_UPGRADE);
	CHECK(m.sv.drawn.fills > fills);
	display_geometry = NULL;
	CHECK(m.sv.disk_formatted);
	CHECK(s10a_usb_disk()->offered);
	CHECK(s10a_usb_disk()->dev == &m.sv.disk.dev);
	/* S-12.003: the master boot record, in part 0; the format's
	 * sectors 64-206 run on into part 1 past the gap. */
	CHECK(disk_sector(0)[0x1FE] == 0x55 && disk_sector(0)[0x1FF] == 0xAA);
	CHECK(disk_sector(0)[0x1C2] == 0x06);
	CHECK(disk_sector(63)[0x1FE] == 0x55 && disk_sector(63)[0x1FF] == 0xAA);
	CHECK(gap_intact());

	disk_sector(0)[0] = 0x5A;	/* as a host might write it */
	opxy_usb_select(&m, S10A_STATE_MAIN);
	CHECK(!s10a_usb_disk()->offered);
	opxy_usb_select(&m, S10A_STATE_UPGRADE);
	CHECK(disk_sector(0)[0] == 0x5A);	/* not formatted again */
	CHECK(s10a_usb_disk()->offered);

	s10a_usb_disk_withdraw(S10A_WITHDRAW_SCAN);
	m.sv.host_ops.ramdisk_offer(&m);	/* a host-link package */
	CHECK(s10a_usb_disk()->offered);
	/* S-15.115: once a zip was read from the disk it is not offered
	 * again, after a transfer or when the screen opens again. */
	m.sv.menu.disk_read = true;
	s10a_usb_disk_withdraw(S10A_WITHDRAW_SCAN);
	m.sv.host_ops.ramdisk_offer(&m);
	CHECK(!s10a_usb_disk()->offered);
	opxy_usb_select(&m, S10A_STATE_UPGRADE);
	CHECK(!s10a_usb_disk()->offered);
	m.sv.menu.disk_read = false;
	opxy_usb_select(&m, S10A_STATE_UPGRADE);
	CHECK(s10a_usb_disk()->offered);
	opxy_usb_select(&m, S10A_STATE_MAIN);
	m.sv.host_ops.ramdisk_offer(&m);	/* no disk in the MIDI set */
	CHECK(!s10a_usb_disk()->offered);
	opxy_usb_stop();
}

/*
 * S-07.054, S-10a.010: the battery wait presents the disk set with no
 * medium and no disk, and detaches when it ends. Held mode then finds the
 * controller up and does not bring it up again.
 */
static void test_battery_wait(void)
{
	setup();
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	opxy_usb_battery(&m, true);
	CHECK(opxy_usb_up() && wb_usb_attached());
	CHECK(m.sv.usb_state == S10A_STATE_BATTERY);
	CHECK(s10a_usb_disk()->dev == NULL && !s10a_usb_disk()->offered);
	CHECK(!m.sv.disk_formatted);
	opxy_usb_poll(&m);
	opxy_usb_battery(&m, false);
	CHECK(opxy_usb_up() && !wb_usb_attached());

	opxy_services_start(&m);
	CHECK(opxy_usb_up());
	opxy_usb_stop();
}

/*
 * S-07.051, S-04.063: from held mode's start a PSoC power-off runs the full
 * shutdown (the slave's power-off first, which S-09.008 skips here with no
 * slave), then the reports, then the device detached and the PHY off
 * (S-10a.030), then 0x32, and nothing more. A byte queued for the Nordic
 * has left first (S-13b.072).
 */
static void test_held_power_off(void)
{
	static const uint8_t on[1] = { 0x10 }, off[1] = { 0x14 }, ping[1] = { 7 };
	static uint8_t rx[64], tx[64];
	static const uint8_t last = 0x7E;
	size_t sent;
	int thr, stat;

	setup();
	/* the shutdown's drain finds the UART idle (the reports go out on the
	 * fake port, test_board_report checks the UART's side) */
	fake_mmr_set(UART_STAT(OPXY_UART_PSOC), UART_STAT_THRE | UART_STAT_TEMT);
	/* S-13b.072: a byte for the Nordic leaves before the 0x32 */
	CHECK(wb_uart_init(&m.hw.uart[OPXY_UART_NORDIC], OPXY_UART_NORDIC,
			   &opxy_uart_nordic, &m.hw.time, rx, sizeof(rx), tx,
			   sizeof(tx)) == WB_UART_OK);
	m.psoc_ops.ctx = &m;		/* as psoc_link_init() sets it */
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	opxy_services_start(&m);
	CHECK(m.psoc_ops.shutdown != NULL);
	opxy_usb_select(&m, S10A_STATE_MAIN);
	CHECK(wb_usb_attached() && usb_phy_on());
	inject(0x31, on, sizeof(on));
	CHECK(s07_poll(&m.psoc) == S07_OK);
	CHECK(!m.psoc.shut);
	inject_calls = 0;		/* the fake gives a frame on a poll's first read */
	sent = wire_n;
	inject(0x31, off, sizeof(off));
	fake_mmr_set(UART_STAT(OPXY_UART_NORDIC), 0);	/* THR full */
	wb_uart_write(&m.hw.uart[OPXY_UART_NORDIC], &last, 1);
	fake_mmr_set(UART_STAT(OPXY_UART_NORDIC), UART_STAT_THRE | UART_STAT_TEMT);
	fake_mmr_clear_log();
	CHECK(s07_poll(&m.psoc) == S07_POWEROFF);
	CHECK(m.psoc.shut && m.psoc.off && m.psoc.state == 0x14u);
	/* the reports while attached; the 0x32 answer, last, after the
	 * pull-up and the PHY went off */
	CHECK(wire_n > sent + 1u && wire_usb[sent] && wire_phy[sent]);
	CHECK(!wire_usb[wire_n - 1u] && !wire_phy[wire_n - 1u]);
	CHECK(!(fake_mmr_get(USB0_POWER) & USB_POWER_SOFTCONN));
	/* UART1's last byte to THR, then TEMT read, before the 0x32's first
	 * byte; the UART left on */
	thr = mmr_last(FAKE_MMR_WRITE, UART_THR(OPXY_UART_NORDIC), last, false);
	stat = mmr_last(FAKE_MMR_READ, UART_STAT(OPXY_UART_NORDIC), 0, true);
	CHECK(thr >= 0 && stat > thr);
	CHECK((size_t)stat < wire_mmr[last_frame_at()]);
	CHECK(fake_mmr_get(UART_CTL(OPXY_UART_NORDIC)) & UART_CTL_EN);
	sent = wire_n;
	inject_calls = 0;
	inject(0x01, ping, sizeof(ping));
	CHECK(s07_poll(&m.psoc) == S07_OK);
	CHECK(wire_n == sent);		/* step 3: nothing more, not even a pong */
	opxy_usb_stop();
}

static void off_idle(void *ctx)
{
	(void)ctx;
	longjmp(mark_jmp, 3);
}

/*
 * S-07.051, S-13b.072: after the 0x32, polled (outside held mode), the
 * PSoC UART's queue goes out whole and TEMT is read before the master
 * idles; the UART is left on.
 */
static void test_powered_off(void)
{
	static uint8_t rx[64], tx[64];
	static const uint8_t frame[9] = { 3, 0x41, 1, 0x32, 2, 1, 0x14, 0x55, 0 };
	const struct fake_mmr_access *log;
	size_t i, off_thr;
	int stat;

	setup();
	CHECK(wb_uart_init(&m.hw.uart[OPXY_UART_PSOC], OPXY_UART_PSOC,
			   &opxy_uart_psoc, &m.hw.time, rx, sizeof(rx), tx,
			   sizeof(tx)) == WB_UART_OK);
	wb_uart_write(&m.hw.uart[OPXY_UART_PSOC], frame, sizeof(frame));
	CHECK(m.hw.uart[OPXY_UART_PSOC].tx_head == sizeof(frame));
	fake_mmr_set(UART_STAT(OPXY_UART_PSOC), UART_STAT_THRE | UART_STAT_TEMT);
	fake_mmr_clear_log();
	fake_core_set_idle_hook(off_idle, NULL);
	if (setjmp(mark_jmp) == 0)
		opxy_powered_off(&m);
	fake_core_set_idle_hook(NULL, NULL);
	log = fake_mmr_log();
	off_thr = 0;
	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == FAKE_MMR_WRITE &&
		    log[i].addr == UART_THR(OPXY_UART_PSOC) &&
		    log[i].value == frame[off_thr] && off_thr < sizeof(frame))
			off_thr++;
	CHECK(off_thr == sizeof(frame));
	stat = mmr_last(FAKE_MMR_READ, UART_STAT(OPXY_UART_PSOC), 0, true);
	CHECK(stat > mmr_last(FAKE_MMR_WRITE, UART_THR(OPXY_UART_PSOC), 0, true));
	CHECK(fake_core_idles() == 1u);
	CHECK(fake_mmr_get(UART_CTL(OPXY_UART_PSOC)) & UART_CTL_EN);
	opxy_usb_stop();
}

/* The restart's steps as S-04's ops see them, with the time of each. */
static char rs_ev[8];
static uint64_t rs_at[8];
static bool rs_usb[8];		/* the device attached at the step */
static bool rs_phy[8];		/* the USB PHY on at the step */
static size_t rs_mmr[8];	/* fake_mmr_count() at the step */
static size_t rs_n;
static uint16_t rs_reason;

static void rs_mark(char c)
{
	if (rs_n < sizeof(rs_ev)) {
		rs_ev[rs_n] = c;
		rs_at[rs_n] = fake_core_peek_cycles();
		rs_usb[rs_n] = wb_usb_attached();
		rs_phy[rs_n] = usb_phy_on();
		rs_mmr[rs_n] = fake_mmr_count();
		rs_n++;
	}
}

static void rs_shutdown(void *ctx) { (void)ctx; rs_mark('S'); }
static void rs_write(void *ctx, uint16_t reason) { (void)ctx; rs_reason = reason; rs_mark('R'); }
static void rs_hard(void *ctx) { (void)ctx; rs_mark('H'); }
static void rs_other(void *ctx) { (void)ctx; rs_mark('?'); }

/*
 * S-15.095, S-04.026: the restart's line stays on the screen for
 * OPXY_RESTART_HOLD_MS before the shutdown; then the record with this
 * boot's reason and the hard reset, with no other step (W7).
 */
static void test_restart_hold(void)
{
	uint64_t t0;

	held_setup();
	m.boot_reason = 30u;
	m.sv.reboot.shutdown = rs_shutdown;
	m.sv.reboot.write_record = rs_write;
	m.sv.reboot.hard_reset = rs_hard;
	m.sv.reboot.psoc_reset_request = rs_other;
	m.sv.reboot.nordic_enter_dfu = rs_other;
	m.sv.reboot.nordic_exit_dfu = rs_other;
	m.sv.reboot.wait_for_psoc = rs_other;
	rs_n = 0;
	fake_core_set_cycle_step(400u * 1000u);	/* 1 ms a read */
	t0 = fake_core_peek_cycles();
	m.sv.env.reset_w7(m.sv.env.ctx);
	CHECK(rs_n == 3u && memcmp(rs_ev, "SRH", 3) == 0);
	CHECK(rs_reason == 30u);
	CHECK(rs_at[0] - t0 >= (uint64_t)OPXY_RESTART_HOLD_MS * 1000u * 400u);
	/* bounded: the hold and a few reads, not a wait on anything */
	CHECK(rs_at[0] - t0 <= (uint64_t)(OPXY_RESTART_HOLD_MS + 20u) * 1000u * 400u);
	fake_core_set_cycle_step(1u);
	opxy_usb_stop();
}

static void rs_psoc(void *ctx) { (void)ctx; rs_mark('P'); }
static void rs_exit(void *ctx) { (void)ctx; rs_mark('X'); }
static void rs_wait(void *ctx) { (void)ctx; rs_mark('W'); }

/*
 * S-15.190, S-05.033: Continue to the OS holds its view as the restart
 * does, then W11 with the slave present (the shutdown, the Nordic's exit,
 * reason 80, the hard reset) and W2 without it (the PSoC's reset request
 * first, reason 20, the wait for the PSoC).
 */
static void test_continue_restart(void)
{
	for (int k = 0; k < 3; k++) {
		uint64_t t0;

		held_setup();
		m.boot_reason = 30u;
		m.slave_present = k != 1;
		m.bridge.absent = k == 2;
		m.sv.reboot.shutdown = rs_shutdown;
		m.sv.reboot.write_record = rs_write;
		m.sv.reboot.hard_reset = rs_hard;
		m.sv.reboot.psoc_reset_request = rs_psoc;
		m.sv.reboot.nordic_enter_dfu = rs_other;
		m.sv.reboot.nordic_exit_dfu = rs_exit;
		m.sv.reboot.wait_for_psoc = rs_wait;
		rs_n = 0;
		fake_core_set_cycle_step(400u * 1000u);	/* 1 ms a read */
		t0 = fake_core_peek_cycles();
		CHECK(m.sv.env.continue_os != NULL);
		m.sv.env.continue_os(m.sv.env.ctx);
		if (k == 0) {
			CHECK(rs_n == 4u && memcmp(rs_ev, "SXRH", 4) == 0);
			CHECK(rs_reason == S04_REASON_OS_ONCE);
		} else {
			CHECK(rs_n == 5u && memcmp(rs_ev, "PSXRW", 5) == 0);
			CHECK(rs_reason == S04_REASON_DEFAULT);
		}
		CHECK(rs_at[0] - t0 >= (uint64_t)OPXY_RESTART_HOLD_MS * 1000u * 400u);
		fake_core_set_cycle_step(1u);
		opxy_usb_stop();
	}
}

/*
 * S-04.063, S-04.028: a reboot's real shutdown (no slave, no display),
 * then OPXY_BACKLIGHT_SETTLE_MS with nothing sent, then the record and the
 * reset. A PSoC power-off answers at once (test_held_power_off).
 */
static void test_reboot_settle(void)
{
	uint64_t t0;
	size_t sent;

	held_setup();
	fake_mmr_set(UART_STAT(OPXY_UART_PSOC), UART_STAT_THRE | UART_STAT_TEMT);
	m.psoc_ops.ctx = &m;
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	m.sv.reboot.write_record = rs_write;
	m.sv.reboot.hard_reset = rs_hard;
	opxy_usb_select(&m, S10A_STATE_MAIN);
	CHECK(wb_usb_attached());
	rs_n = 0;
	sent = wire_n;
	fake_core_set_cycle_step(400u * 1000u);	/* 1 ms a read */
	t0 = fake_core_peek_cycles();
	opxy_reboot(&m, S04_W7, 20u);
	CHECK(m.psoc.shut && wire_n > sent);	/* the reports went out */
	CHECK(rs_n == 2u && memcmp(rs_ev, "RH", 2) == 0 && rs_reason == 20u);
	/* S-10a.030: detached and the PHY off before the record and the
	 * reset, after the reports */
	CHECK(!rs_usb[0] && !rs_usb[1] && wire_usb[sent]);
	CHECK(!rs_phy[0] && !rs_phy[1] && wire_phy[sent]);
	CHECK(rs_at[0] - t0 >= (uint64_t)OPXY_BACKLIGHT_SETTLE_MS * 1000u * 400u);
	CHECK(rs_at[0] - t0 <= (uint64_t)(OPXY_BACKLIGHT_SETTLE_MS + 20u) * 1000u * 400u);
	fake_core_set_cycle_step(1u);
	opxy_usb_stop();
}

/*
 * S-10a.030 on the soft reboot (W2, S-04.022): the device detached before
 * the PSoC reset request, after which the PSoC may reset the processor at
 * any time; then the shutdown and the record.
 */
static void test_reboot_os_detach(void)
{
	static const uint8_t magic[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	size_t i, sent;

	held_setup();
	fake_mmr_set(UART_STAT(OPXY_UART_PSOC), UART_STAT_THRE | UART_STAT_TEMT);
	m.psoc_ops.ctx = &m;
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	m.sv.reboot.nordic_exit_dfu = rs_other;
	m.sv.reboot.write_record = rs_write;
	m.sv.reboot.wait_for_psoc = rs_other;
	opxy_usb_select(&m, S10A_STATE_MAIN);
	CHECK(wb_usb_attached());
	rs_n = 0;
	sent = wire_n;
	fake_core_set_cycle_step(400u * 1000u);
	opxy_reboot(&m, S04_W2, 0u);
	for (i = sent; i + sizeof(magic) <= wire_n; i++)
		if (memcmp(wire + i, magic, sizeof(magic)) == 0)
			break;
	CHECK(i + sizeof(magic) <= wire_n);	/* 0x97 DE AD BE EF went out */
	if (i + sizeof(magic) <= wire_n)
		CHECK(!wire_usb[i]);
	CHECK(rs_n == 3u && memcmp(rs_ev, "?R?", 3) == 0 && rs_reason == 20u);
	CHECK(!rs_usb[0] && !rs_usb[1] && !rs_usb[2]);
	fake_core_set_cycle_step(1u);
	opxy_usb_stop();
}

/* The log index of the last access of op to addr, or -1. */
static int mmr_last(enum fake_mmr_op op, wb_mmr_addr addr, uint32_t value,
		    bool any_value)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	int at = -1;
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == op && log[i].addr == addr &&
		    (any_value || log[i].value == value))
			at = (int)i;
	return at;
}

/*
 * S-04.027, S-13b.072 on W6: the Nordic link stops after the exit, and
 * UART1 is disabled only once its last byte has left: a THR write, then a
 * STAT read with TEMT, then UART1_CTL = 0.
 */
static void test_nordic_link_stop(void)
{
	static uint8_t rx[64], tx[64];
	static const uint8_t last = 0x7E;
	int thr, stat, ctl;

	held_setup();
	fake_mmr_set(UART_STAT(OPXY_UART_PSOC), UART_STAT_THRE | UART_STAT_TEMT);
	m.psoc_ops.ctx = &m;
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	m.sv.reboot.nordic_exit_dfu = rs_other;
	m.sv.reboot.write_record = rs_write;
	m.sv.reboot.hard_reset = rs_hard;
	CHECK(wb_uart_init(&m.hw.uart[OPXY_UART_NORDIC], OPXY_UART_NORDIC,
			   &opxy_uart_nordic, &m.hw.time, rx, sizeof(rx), tx,
			   sizeof(tx)) == WB_UART_OK);
	fake_mmr_set(UART_STAT(OPXY_UART_NORDIC), UART_STAT_THRE | UART_STAT_TEMT);
	rs_n = 0;
	fake_mmr_clear_log();
	wb_uart_write(&m.hw.uart[OPXY_UART_NORDIC], &last, 1);
	fake_core_set_cycle_step(400u * 1000u);	/* 1 ms a read */
	opxy_reboot(&m, S04_W6, 0u);
	fake_core_set_cycle_step(1u);
	CHECK(rs_n == 3u && memcmp(rs_ev, "?RH", 3) == 0);
	thr = mmr_last(FAKE_MMR_WRITE, UART_THR(OPXY_UART_NORDIC), last, false);
	stat = mmr_last(FAKE_MMR_READ, UART_STAT(OPXY_UART_NORDIC), 0, true);
	ctl = mmr_last(FAKE_MMR_WRITE, UART_CTL(OPXY_UART_NORDIC), 0, false);
	CHECK(thr >= 0 && stat > thr && ctl > stat);
	CHECK(fake_mmr_get(UART_CTL(OPXY_UART_NORDIC)) == 0);
	CHECK(fake_mmr_errors() == 0);
	opxy_usb_stop();
}

/*
 * S-13b.072, S-04.028 before a hard reset (W7): each enabled UART's queue
 * and transmitter empty before the record, which precedes the reset's
 * arming; the UART left on. With CTS held off by the Nordic, the record
 * follows WB_UART_CLOSE_US.
 */
static void test_reboot_drain(void)
{
	static uint8_t rx[64], tx[64];
	static const uint8_t last = 0x7E;
	uint64_t t0, took;
	int thr, stat;

	held_setup();
	m.sv.reboot.shutdown = rs_shutdown;
	m.sv.reboot.write_record = rs_write;
	m.sv.reboot.hard_reset = rs_hard;
	CHECK(wb_uart_init(&m.hw.uart[OPXY_UART_NORDIC], OPXY_UART_NORDIC,
			   &opxy_uart_nordic, &m.hw.time, rx, sizeof(rx), tx,
			   sizeof(tx)) == WB_UART_OK);
	fake_mmr_set(UART_STAT(OPXY_UART_NORDIC), 0);	/* THR full */
	wb_uart_write(&m.hw.uart[OPXY_UART_NORDIC], &last, 1);
	fake_mmr_set(UART_STAT(OPXY_UART_NORDIC), UART_STAT_THRE | UART_STAT_TEMT);
	rs_n = 0;
	fake_mmr_clear_log();
	opxy_reboot(&m, S04_W7, 30u);
	CHECK(rs_n == 3u && memcmp(rs_ev, "SRH", 3) == 0);
	thr = mmr_last(FAKE_MMR_WRITE, UART_THR(OPXY_UART_NORDIC), last, false);
	stat = mmr_last(FAKE_MMR_READ, UART_STAT(OPXY_UART_NORDIC), 0, true);
	CHECK(thr >= 0 && stat > thr && (size_t)stat < rs_mmr[1]);
	CHECK(fake_mmr_get(UART_CTL(OPXY_UART_NORDIC)) & UART_CTL_EN);

	/* CTS held: the transmitter never empties; the record follows the
	 * bound, and only one bound. */
	wb_uart_write(&m.hw.uart[OPXY_UART_NORDIC], &last, 1);
	fake_mmr_set(UART_STAT(OPXY_UART_NORDIC), 0);
	rs_n = 0;
	fake_core_set_cycle_step(400u * 250u);		/* 250 us a read */
	t0 = fake_core_peek_cycles();
	opxy_reboot(&m, S04_W7, 30u);
	CHECK(rs_n == 3u && memcmp(rs_ev, "SRH", 3) == 0);
	took = rs_at[1] - t0;
	CHECK(took >= 400u * (uint64_t)WB_UART_CLOSE_US);
	CHECK(took <= 400u * ((uint64_t)WB_UART_CLOSE_US + 2000u));
	CHECK(fake_mmr_get(UART_CTL(OPXY_UART_NORDIC)) & UART_CTL_EN);
	CHECK(fake_mmr_errors() == 0);
	fake_core_set_cycle_step(1u);
	opxy_usb_stop();
}

/* A reboot with the controller up but no set ever presented, and the
 * detach with no controller at all: nothing to undo, nothing fails. */
static void test_reboot_never_attached(void)
{
	held_setup();
	fake_mmr_set(UART_STAT(OPXY_UART_PSOC), UART_STAT_THRE | UART_STAT_TEMT);
	m.psoc_ops.ctx = &m;
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	m.sv.reboot.write_record = rs_write;
	m.sv.reboot.hard_reset = rs_hard;
	CHECK(!wb_usb_attached());
	rs_n = 0;
	fake_core_set_cycle_step(400u * 1000u);
	opxy_reboot(&m, S04_W7, 30u);
	CHECK(rs_n == 2u && memcmp(rs_ev, "RH", 2) == 0 && !rs_usb[0]);
	fake_core_set_cycle_step(1u);
	opxy_usb_stop();
	setup();			/* no controller brought up */
	opxy_usb_stop();
	opxy_usb_stop();
	CHECK(!wb_usb_attached());
}

/* S-15.105: the host's release makes S-11.045's scan due. */
static void test_disk_released(void)
{
	struct boot_held h;

	memset(&h, 0, sizeof(h));
	h.reason = 30u;
	held_setup();
	opxy_held_screen(&m, &h);
	CHECK(m.sv.menu.screen == S15_SCR_UPGRADE);
	CHECK(s10a_usb_disk()->offered);
	CHECK(!m.sv.menu.scan_due_set);
	m.sv.disk_acquired = true;
	m.sv.usb_board.disk_released(m.sv.usb_board.ctx);
	CHECK(m.sv.menu.scan_due_set);
	CHECK(!m.sv.disk_acquired);
	opxy_usb_stop();
}

/* Whether a shown console row is `text` in `style`. */
static bool con_has(enum s15_style style, const char *text)
{
	size_t len, i;

	for (i = 0; i < m.sv.menu.con.count; i++) {
		const char *row = s15_console_row(&m.sv.menu.con, (unsigned)i,
						  &len);

		if (len == strlen(text) && memcmp(row, text, len) == 0 &&
		    s15_console_row_style(&m.sv.menu.con, (unsigned)i) == style)
			return true;
	}
	return false;
}

/*
 * S-11.045, S-11.046, S-11.049: S-15 starts the scan, which runs from the
 * next service pass, not from inside S-15's call; the empty disk holds no
 * package, and is offered again with its contents.
 */
static void test_scan_not_found(void)
{
	struct boot_held h;

	memset(&h, 0, sizeof(h));
	h.reason = 30u;
	held_setup();
	opxy_held_screen(&m, &h);
	m.sv.usb_board.disk_released(m.sv.usb_board.ctx);
	s15_up_tick(&m.sv.menu, m.sv.menu.scan_due_ms);
	CHECK(m.sv.scan_due);
	CHECK(con_has(S15_ST_HEAD, "Preparing the installation") &&
	      !con_has(S15_ST_WARN, "No valid package found"));
	opxy_services_poll(&m);
	CHECK(!m.sv.scan_due);
	CHECK(con_has(S15_ST_WARN, "No valid package found"));
	CHECK(s10a_usb_disk()->offered && s15_up_disk_offerable(&m.sv.menu));
	opxy_usb_stop();
}

/* S-15.020, S-10a.010: the main screen starts its set; the upgrade
 * screen after reason 30 the disk's, also when an install was interrupted. */
static void test_held_screen(void)
{
	struct boot_held h;

	memset(&h, 0, sizeof(h));
	held_setup();
	CHECK(!m.sv.provisioned);	/* no OTP on the host */
	/* S-17.030: what S-17.021 step 1 read, as the Info screen shows it */
	m.sv.copro.nordic_app.major = 1u;
	m.sv.copro.nordic_app.minor = 2u;
	m.sv.copro.nordic_app.patch = 3u;
	m.sv.copro.nordic_app.build = 7u;
	m.sv.copro.psoc.major = 3u;
	m.sv.copro.psoc.minor = 4u;
	memcpy(m.sv.copro.psoc_serial, "PS000001", 8u);
	m.sv.copro.psoc_serial_len = 8u;
	opxy_held_screen(&m, &h);
	CHECK(strcmp(m.sv.boot.ble_version, "1.2.3+7") == 0);
	CHECK(strcmp(m.sv.boot.psoc_version, "3.4.0") == 0);
	CHECK(strcmp(m.sv.boot.psoc_serial, "PS000001") == 0);
	CHECK(m.sv.menu.screen == S15_SCR_MAIN);
	CHECK(wb_usb_attached());
	CHECK(strcmp(m.sv.boot.version, bl_text()) == 0);
	/* Without S-13c the draw ops are the recorder's. */
	CHECK(m.sv.draw.ctx == &m.sv.drawn);
	CHECK(m.sv.boot.slave == S15_SLAVE_OK);
	/* S-15.172: no settings read at this start: the defaults, developer
	 * mode off, and nothing asked of the drive for them */
	CHECK(!m.sv.boot.settings.dev_mode && m.sv.boot.settings.color &&
	      m.sv.boot.settings.mtp_macos);
	CHECK(!m.sv.menu.dev_mode && m.sv.menu.color);
	CHECK(!m.sv.files.mount_sent);
	opxy_usb_stop();

	h.reason = 30u;
	held_setup();
	opxy_held_screen(&m, &h);
	CHECK(m.sv.menu.screen == S15_SCR_UPGRADE);
	CHECK(wb_usb_attached());
	opxy_usb_stop();

	/* S-11.080: an interrupted install keeps the reason's first screen
	 * and its disk, and the screens are told. */
	h.interrupted = true;
	h.slave_absent = true;
	held_setup();
	opxy_held_screen(&m, &h);
	CHECK(m.sv.menu.screen == S15_SCR_UPGRADE);
	CHECK(wb_usb_attached());
	CHECK(m.sv.boot.slave == S15_SLAVE_ABSENT && m.sv.boot.interrupted);
	opxy_usb_stop();

	memset(&h, 0, sizeof(h));
	h.cause = S05_FAIL_SLAVE;
	h.start.replied = true;
	h.start.reply = 1u;
	held_setup();
	opxy_held_screen(&m, &h);
	CHECK(m.sv.boot.slave == S15_SLAVE_NO_OS);
	opxy_usb_stop();

	/* No reply to the release: the slave stopped answering. */
	memset(&h, 0, sizeof(h));
	h.cause = S05_FAIL_SLAVE;
	held_setup();
	opxy_held_screen(&m, &h);
	CHECK(m.sv.boot.slave == S15_SLAVE_LOST);
	opxy_usb_stop();

	/* S-05.032: released, its OS did not start */
	memset(&h, 0, sizeof(h));
	h.cause = S05_FAIL_SLAVE_START;
	held_setup();
	opxy_held_screen(&m, &h);
	CHECK(m.sv.boot.slave == S15_SLAVE_NO_START);
	opxy_usb_stop();
}

/* A failed start's account: its s05 cause and stage result. */
static struct boot_start account(enum s03_load_result r)
{
	struct boot_start st;

	memset(&st, 0, sizeof(st));
	st.load.result = r;
	st.load.part = 2u;
	st.load.header.format = 9u;
	st.load.header.length = 742764u;
	st.load.header.crc = 0xDEADBEEFu;
	st.load.crc = 0x1234ABCDu;
	st.boot_size = 0x80000u;
	return st;
}

/*
 * S-05.006, S-05.007, S-15.163 on the board: each cause and stage result
 * in S-15's terms; a stage the bridge lost is a failed select that writes
 * no record, its error line names the request, the stage and the driver's
 * words, and held mode shows it, keeps it for a developer host and knows
 * the slave stopped answering: the mount sent to no one is not a refused
 * volume.
 */
static void test_failed_start(void)
{
	static const struct {
		enum s05_fail cause;
		enum s03_load_result r;
		enum s15_start_why why;
		uint32_t value, limit;
	} views[] = {
		{S05_FAIL_NO_OS, S03_LOAD_NO_ACTIVE, S15_START_NO_ACTIVE, 0, 0},
		{S05_FAIL_NO_OS, S03_LOAD_ZERO_LENGTH, S15_START_ZERO_LENGTH, 0, 0},
		{S05_FAIL_NO_OS, S03_LOAD_TOO_LONG, S15_START_TOO_LONG, 742764u, S03_OS_MAX},
		{S05_FAIL_NO_OS, S03_LOAD_PAST_PARTITION, S15_START_PAST_PARTITION, 1452u, 1024u},
		{S05_FAIL_NO_OS, S03_LOAD_FORMAT, S15_START_FORMAT, 9u, 0},
		{S05_FAIL_NO_OS, S03_LOAD_CRC, S15_START_CRC, 0x1234ABCDu, 0xDEADBEEFu},
		{S05_FAIL_ENCRYPTED, S03_LOAD_ENCRYPTED, S15_START_ENCRYPTED, 0, 0},
		{S05_FAIL_SLAVE_START, S03_LOAD_OK, S15_START_SLAVE_START, 0, 0},
		{S05_FAIL_NONE, S03_LOAD_OK, S15_START_OK, 0, 0},
	};
	static const enum s03_load_result results[] = {
		S03_LOAD_OK, S03_LOAD_NO_ACTIVE, S03_LOAD_ZERO_LENGTH, S03_LOAD_TOO_LONG,
		S03_LOAD_PAST_PARTITION, S03_LOAD_FORMAT, S03_LOAD_ENCRYPTED, S03_LOAD_CRC,
		S03_LOAD_IO,
	};
	static const enum s05_fail causes[] = {
		S05_FAIL_NONE, S05_FAIL_NO_OS, S05_FAIL_NO_OS, S05_FAIL_NO_OS,
		S05_FAIL_NO_OS, S05_FAIL_NO_OS, S05_FAIL_ENCRYPTED, S05_FAIL_NO_OS,
		S05_FAIL_SELECT,
	};
	static uint8_t staging[S03_SECTOR * 4];
	uint8_t emmc_scratch[S03_SECTOR];
	uint8_t rep[OPXY_START_REPORT];
	struct boot_start st;
	struct s15_start v;
	struct s03_load ld;
	struct boot_held h;
	struct s03_emmc_ops ops;
	struct s03_env env;
	size_t i;

	for (i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
		st = account(views[i].r);
		opxy_start_view(views[i].cause, &st, &v);
		CHECK(v.why == views[i].why && v.value == views[i].value &&
		      v.limit == views[i].limit && !v.lost);
	}
	st = account(S03_LOAD_OK);
	st.refusal = S05_REFUSE_BOOT_RESERVE;
	opxy_start_view(S05_FAIL_REFUSED, &st, &v);
	CHECK(v.why == S15_START_REFUSED && v.check == S15_CHECK_BOOT_RESERVE);
	st.replied = true;
	st.reply = 1u;
	opxy_start_view(S05_FAIL_SLAVE, &st, &v);
	CHECK(v.why == S15_START_SLAVE_NO_OS && v.value == 1u);
	st.replied = false;
	opxy_start_view(S05_FAIL_SLAVE, &st, &v);
	CHECK(v.why == S15_START_NO_REPLY);
	/* S-05.006, S-05.007: only a failed request is S05_FAIL_SELECT */
	for (i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
		ld.result = results[i];
		CHECK(opxy_stage_fail(&ld) == causes[i]);
	}

	/* The slave lost before the load: no request leaves, the first
	 * select fails, and S-05.007's cause writes no record. */
	setup();
	ops = opxy_bridge_emmc(&m.bridge);
	memset(&env, 0, sizeof(env));
	env.ops = &ops;
	env.crc32 = opxy_crc32_long;
	env.boot_size = 0x80000u;
	env.scratch = emmc_scratch;
	env.scratch_len = sizeof(emmc_scratch);
	env.max_xfer = 128u;
	s03_load_os(&env, staging, sizeof(staging), &ld);
	CHECK(ld.result == S03_LOAD_IO && ld.step == S03_STEP_SELECT &&
	      ld.step_part == S03_PART_BP1 && ld.select0_failed);
	CHECK(opxy_stage_fail(&ld) == S05_FAIL_SELECT);
	memset(&st, 0, sizeof(st));
	st.load = ld;
	st.lost = s09_master_lost(&m.bridge, &st.why);
	CHECK(st.lost);
	opxy_start_view(S05_FAIL_SELECT, &st, &v);
	CHECK(v.why == S15_START_SELECT && v.part == 1u && v.lost);

	/* The unit's case: the bridge lost on an image read. The log line
	 * reaches the PSoC link, the one sink on before held mode. */
	memset(&st, 0, sizeof(st));
	st.load.result = S03_LOAD_IO;
	st.load.step = S03_STEP_IMAGE;
	st.load.step_part = st.load.part = 2u;
	st.load.sector = 1281u;
	st.load.count = 128u;
	st.load.header.length = 742764u;
	st.load.header.format = 3u;
	st.lost = true;
	st.why.cmd = S09_READ_SECTORS;
	st.why.at = S09_AT_REPLY_BROKE;
	st.why.arg0 = 0x501u;
	st.why.fault[0] = 0xFFFFFFF5u;
	st.why.fault[3] = 0xA5u;
	setup();
	opxy_start_log(&m, S05_FAIL_SELECT, &st);
	CHECK(wire_has("OS not started: DSP2 stopped answering during the image read"));
	CHECK(wire_has("step 3 part 2 sector 1281+128 length 742764"));
	CHECK(wire_has("bridge lost on 12 arg 501 at 04 fault fffffff5 0 0 a5"));

	/* Held mode after it: the mount meets the lost slave, which is no
	 * refused volume; the screens and the report say so. */
	setup();
	m.slave_present = true;
	opxy_services_start(&m);
	host_disk();
	CHECK(m.sv.files.mount_sent && m.sv.files.mount_lost &&
	      !m.sv.files.mount_failed);
	CHECK(!s09_files_volume_refused(&m.sv.files) &&
	      s09_files_slave_lost(&m.sv.files));
	CHECK(m.sv.env.slave_lost(m.sv.env.ctx) &&
	      !m.sv.env.volume_refused(m.sv.env.ctx));
	memset(&h, 0, sizeof(h));
	h.load_attempted = true;
	h.cause = S05_FAIL_SELECT;
	h.start = st;
	opxy_held_screen(&m, &h);
	CHECK(m.sv.boot.slave == S15_SLAVE_LOST);
	CHECK(m.sv.boot.start.why == S15_START_IMAGE_READ &&
	      m.sv.boot.start.value == 1281u && m.sv.boot.start.lost &&
	      m.sv.boot.start.cmd == S09_READ_SECTORS);
	CHECK(con_has(S15_ST_ERROR, "The OS could not be started."));
	CHECK(con_has(S15_ST_DIM,
		    "   Reason: DSP2 stopped answering during the image read"));
	CHECK(opxy_start_report(&m, rep) == OPXY_START_REPORT);
	CHECK(rep[0] == 1u && rep[1] == S05_FAIL_SELECT &&
	      rep[2] == S03_LOAD_IO && rep[3] == S03_STEP_IMAGE &&
	      rep[4] == 2u && rep[5] == 2u && rep[6] == 0u);
	CHECK(rep[7] == 0x01u && rep[8] == 0x05u && rep[11] == 128u);	/* sector, count */
	CHECK(rep[19] == 0x6Cu && rep[20] == 0x55u && rep[21] == 0x0Bu);	/* 742764 */
	CHECK(rep[38] == 1u && rep[39] == S09_READ_SECTORS &&
	      rep[40] == S09_AT_REPLY_BROKE && rep[41] == 0x01u && rep[42] == 0x05u);
	CHECK(rep[45] == 0xF5u && rep[60] == 0x00u && rep[57] == 0xA5u);
	opxy_usb_stop();

	/* A boot that attempted no start: no notice, a report of zeros. */
	memset(&h, 0, sizeof(h));
	held_setup();
	opxy_held_screen(&m, &h);
	CHECK(m.sv.boot.start.why == S15_START_OK);
	CHECK(!con_has(S15_ST_ERROR, "The OS could not be started."));
	CHECK(opxy_start_report(&m, rep) == OPXY_START_REPORT && rep[0] == 0u &&
	      rep[39] == 0u);
	opxy_usb_stop();
}

/* A RAM disk file's whole contents, NUL-terminated, into buf. */
static bool disk_file(const char *path, char *buf, size_t cap)
{
	FIL f;
	UINT n = 0;

	if (f_open(&f, path, FA_READ) != FR_OK)
		return false;
	(void)f_read(&f, buf, (UINT)cap - 1u, &n);
	(void)f_close(&f);
	buf[n] = '\0';
	return true;
}

/*
 * S-12.017 through S-15's trigger: the three files, written while the disk
 * is withdrawn and offered again after (S-12.018); one that exists keeps
 * its contents. With no slave the export copies nothing.
 */
static void test_system_info(void)
{
	struct boot_held h;
	char buf[512];

	memset(&h, 0, sizeof(h));
	h.reason = 30u;
	held_setup();
	strcpy(m.meta.serial, "XY123456");
	strcpy(m.sv.chip_serial, "CHIPSERIAL00000042");	/* S-12.017's S */
	m.meta.os_version[0] = 1u;
	m.meta.os_version[1] = 1u;
	m.meta.os_version[2] = 2u;
	m.meta.sw_version[0] = 1u;
	m.meta.sw_version[1] = 1u;
	m.meta.sw_version[2] = 2u;
	m.meta.sw_version[3] = 4512u;
	m.sv.copro.psoc.major = 3u;
	m.sv.copro.psoc.minor = 4u;
	m.sv.copro.nordic_app.major = 1u;
	m.sv.copro.nordic_app.minor = 2u;
	m.sv.copro.nordic_app.patch = 3u;
	m.sv.copro.nordic_app.build = 7u;
	memcpy(m.sv.copro.psoc_serial, "PS000001", 8u);
	m.sv.copro.psoc_serial_len = 8u;
	opxy_held_screen(&m, &h);
	CHECK(s10a_usb_disk()->offered);
	CHECK(strcmp(m.sv.boot.chip_serial, "CHIPSERIAL00000042") == 0);

	m.sv.env.system_info(m.sv.env.ctx);
	CHECK(s10a_usb_disk()->offered);
	CHECK(s10a_usb_disk()->ua);	/* the medium changed (S-10a.049) */
	CHECK(wire_fat_mount(&m.sv.fat, &m.sv.disk.dev));
	CHECK(disk_file("systeminfo/version.txt", buf, sizeof(buf)));
	{
		char want[160];

		snprintf(want, sizeof(want),
			 "[firmware: v. 1.1.2]\nblackfin: v. 1.1.2+4512\n"
			 "bootloader: v. %s\nbackplane: v. 3.4.0\n"
			 "nordic: v. 1.2.3+7\n", bl_text());
		CHECK(strcmp(buf, want) == 0);
	}
	CHECK(disk_file("systeminfo/serial.txt", buf, sizeof(buf)));
	CHECK(strcmp(buf, "unit: XY123456\ndsp: CHIPSERIAL00000042\n"
		     "backplane: PS000001\n") == 0);
	CHECK(disk_file("systeminfo/licenses.txt", buf, sizeof(buf)));
	CHECK(strncmp(buf, "waffle-boot is free software", 28u) == 0);
	CHECK(wire_fat_write_file(&m.sv.fat, "systeminfo/serial.txt",
				  (const uint8_t *)"x", 1u, false, 0u));
	wire_fat_unmount(&m.sv.fat);

	m.sv.env.system_info(m.sv.env.ctx);
	CHECK(wire_fat_mount(&m.sv.fat, &m.sv.disk.dev));
	CHECK(disk_file("systeminfo/serial.txt", buf, sizeof(buf)));
	CHECK(strcmp(buf, "x") == 0);
	CHECK(f_stat("logs", NULL) != FR_OK);	/* nothing exported */
	wire_fat_unmount(&m.sv.fat);
	opxy_usb_stop();

	/* S-15.100 with shift: before the first offer, the format first and
	 * no offer here (the screen's set offers it next). */
	memset(&h, 0, sizeof(h));
	held_setup();
	opxy_held_screen(&m, &h);
	m.sv.env.system_info(m.sv.env.ctx);
	CHECK(m.sv.disk_formatted && !s10a_usb_disk()->offered);
	CHECK(wire_fat_mount(&m.sv.fat, &m.sv.disk.dev));
	CHECK(f_stat("systeminfo/version.txt", NULL) == FR_OK);
	wire_fat_unmount(&m.sv.fat);
	opxy_usb_stop();
}

/* S-15.104: a host transfer's begin makes the upgrade screen current. */
static void test_host_upgrade_screen(void)
{
	struct boot_held h;

	memset(&h, 0, sizeof(h));
	held_setup();
	opxy_held_screen(&m, &h);
	m.sv.host_ops.upgrade_screen(&m, 0u);
	CHECK(m.sv.menu.screen == S15_SCR_UPGRADE);
	CHECK(m.sv.menu.activity == S15_ACT_TRANSFER);
	CHECK(m.sv.host_ops.transfer_start(&m));
	m.sv.host_ops.transfer_discard(&m);
	/* S-10c.071: a begin refused for its product word, on the screen it
	 * opened; it goes idle again (S-15.103). */
	m.sv.host_ops.upgrade_screen(&m, 0u);
	m.sv.host_ops.begin_refused(&m, S11_REASON_PRODUCT);
	CHECK(m.sv.menu.activity == S15_ACT_IDLE);
	opxy_usb_stop();
}

/* Whether the RAM disk holds mark (16 bytes) at a 16-byte boundary. */
static bool disk_has(const uint8_t *mark)
{
	uint32_t lba, at;

	for (lba = 0; lba < OPXY_DISK_SECTORS; lba++)
		for (at = 0; at < 512u; at += 16u)
			if (memcmp(disk_sector(lba) + at, mark, 16u) == 0)
				return true;
	return false;
}

/* An upload as a host sends it: 300 chunks of 240 bytes, 72,000 bytes,
 * which ends inside a sector. */
static bool upload(const uint8_t *mark)
{
	uint8_t chunk[240];
	size_t i;
	bool ok = true;

	for (i = 0; i < sizeof(chunk); i++)
		chunk[i] = mark[i % 16u];
	for (i = 0; i < 300u && ok; i++)
		ok = m.sv.host_ops.transfer_append(&m, chunk, sizeof(chunk));
	return ok;
}

/*
 * S-11.050, S-12.024: a transfer's bytes go to a file of the bootloader's
 * own while the disk is withdrawn. A host's file keeps its name and
 * contents, the file's sectors read as zeros once it is gone, and the disk
 * is offered again: by the discard, or by S-10c after a submit.
 */
static void test_transfer_file(void)
{
	static const uint8_t mark[16] = { 'W', 'B', '-', 'T', 'R', 'A', 'N',
					  'S', 'F', 'E', 'R', '-', 'M', 'A',
					  'R', 'K' };
	struct boot_held h;
	struct s11_outcome out;
	char buf[32];

	memset(&h, 0, sizeof(h));
	h.reason = 30u;
	held_setup();
	opxy_held_screen(&m, &h);
	CHECK(s10a_usb_disk()->offered);
	CHECK(wire_fat_mount(&m.sv.fat, &m.sv.disk.dev));
	CHECK(wire_fat_write_file(&m.sv.fat, "firmware.zip",
				  (const uint8_t *)"host zip", 8u, false, 0));
	wire_fat_unmount(&m.sv.fat);

	m.sv.host_ops.upgrade_screen(&m, 0u);
	CHECK(m.sv.host_ops.transfer_start(&m));
	CHECK(!s10a_usb_disk()->offered);
	CHECK(upload(mark));
	CHECK(f_stat("~WBX0000.TMP", NULL) == FR_OK);
	CHECK(disk_has(mark));
	m.sv.host_ops.transfer_discard(&m);
	CHECK(!disk_has(mark));
	CHECK(s10a_usb_disk()->offered);
	CHECK(wire_fat_mount(&m.sv.fat, &m.sv.disk.dev));
	CHECK(f_stat("~WBX0000.TMP", NULL) == FR_NO_FILE);
	CHECK(disk_file("firmware.zip", buf, sizeof(buf)) &&
	      strcmp(buf, "host zip") == 0);
	wire_fat_unmount(&m.sv.fat);

	/* Submitted: the bytes are no package (MAGIC); the file is gone. */
	m.sv.host_ops.upgrade_screen(&m, 0u);
	CHECK(m.sv.host_ops.transfer_start(&m));
	CHECK(upload(mark));
	m.sv.host_ops.transfer_submit(&m, S10C_SOURCE_FW_CDC, &out);
	CHECK(out.kind == S11_OUTCOME_REFUSED &&
	      out.reason == S11_REASON_MAGIC);
	CHECK(!disk_has(mark));
	CHECK(!s10a_usb_disk()->offered);
	m.sv.host_ops.ramdisk_offer(&m);
	CHECK(s10a_usb_disk()->offered);
	CHECK(wire_fat_mount(&m.sv.fat, &m.sv.disk.dev));
	CHECK(f_stat("~WBX0000.TMP", NULL) == FR_NO_FILE);
	CHECK(disk_file("firmware.zip", buf, sizeof(buf)) &&
	      strcmp(buf, "host zip") == 0);
	wire_fat_unmount(&m.sv.fat);
	opxy_usb_stop();
}

/*
 * S-12.024, S-16.231: a package whose zip was recovered in place and which
 * is then refused leaves none of its bytes on the disk. key_fe's zip is
 * plaintext (no key on the host), body form, one zip byte flipped: checks
 * 7-14 pass, so the pass writes the zip back over itself before check 6
 * refuses it (CRC).
 */
static void test_transfer_refused_in_place(void)
{
	enum { BODY = 0x40, ZIP = 0x440 };	/* the body form's zip offset */
	uint8_t pkg[4096], window[16], chunk[240];
	struct boot_held h;
	struct s11_outcome out;
	FILE *fp = fopen("tests/oracle/S-11-packages/vectors/pkg/key_fe.bin", "rb");
	size_t len = fp != NULL ? fread(pkg, 1, sizeof(pkg), fp) : 0u;
	bool ok = true;

	if (fp != NULL)
		fclose(fp);
	/* run from the repository root */
	CHECK(len > ZIP + 0x40u && len < sizeof(pkg));
	if (len <= ZIP + 0x40u || len >= sizeof(pkg))
		return;
	len -= BODY;			/* the body form: no preamble */
	memmove(pkg, pkg + BODY, len);
	memcpy(window, pkg + ZIP + 0x20, sizeof(window));
	pkg[ZIP + 1] ^= 0x01u;

	memset(&h, 0, sizeof(h));
	h.reason = 30u;
	held_setup();
	opxy_held_screen(&m, &h);
	m.sv.host_ops.upgrade_screen(&m, 0u);
	CHECK(m.sv.host_ops.transfer_start(&m));
	for (size_t at = 0; at < len && ok; at += sizeof(chunk)) {
		const size_t n = len - at < sizeof(chunk) ? len - at :
							    sizeof(chunk);

		memcpy(chunk, pkg + at, n);
		ok = m.sv.host_ops.transfer_append(&m, chunk, n);
	}
	CHECK(ok && disk_has(window));
	m.sv.host_ops.transfer_submit(&m, S10C_SOURCE_FW_CDC, &out);
	CHECK(out.kind == S11_OUTCOME_REFUSED && out.reason == S11_REASON_CRC);
	CHECK(!disk_has(window));
	m.sv.host_ops.ramdisk_offer(&m);
	CHECK(wire_fat_mount(&m.sv.fat, &m.sv.disk.dev));
	CHECK(f_stat("~WBX0000.TMP", NULL) == FR_NO_FILE);
	wire_fat_unmount(&m.sv.fat);
	opxy_usb_stop();
}

/* S-08's events on the log; S-17.011, S-17.014: the revision byte as
 * two hex digits; S-14.040: a Nordic log line relayed. */
static void test_nordic_ops(void)
{
	struct s08_ops o;
	struct s06_frame f;

	held_setup();
	opxy_nordic_ops(&m, &o);
	CHECK(o.ctx == &m && o.power != NULL);
	wire_n = 0;
	o.log(o.ctx, S08_LOG_DFU_ERROR, (const uint8_t *)"dfu boom", 8u, 0u);
	CHECK(wire_has("dfu boom"));
	wire_n = 0;
	o.revision(o.ctx, 0x5Au);
	CHECK(wire_has("5a"));
	CHECK(s17_rev_reported(&m.sv.rev) == 0x5Au);

	memset(&f, 0, sizeof(f));
	f.route = s06_route(S06_ADDR_NORDIC, S06_ADDR_MASTER);
	f.id = 0x03u;
	f.len = 12u;				/* 4 bytes, then the text */
	memcpy(f.payload, "\0\0\0\0nrf line", 12u);
	wire_n = 0;
	o.common(o.ctx, &f);
	CHECK(wire_has("nrf line"));
	opxy_usb_stop();
}

/*
 * S-07.076, S-08.004: the coprocessors' other messages are S-10c's common
 * set, answered on their own link; an id it does not serve is logged.
 * Before held mode's services start only the log lines are served.
 */
static void test_copro_common(void)
{
	struct s06_frame f;

	setup();
	memset(&f, 0, sizeof(f));
	f.route = s06_route(S06_ADDR_PSOC, S06_ADDR_MASTER);
	f.id = 0x01u;				/* ping */
	f.len = 6u;
	memcpy(f.payload, "PINGME", 6u);
	wire_n = 0;
	/* S-07.076, S-10c.045: served from the PSoC link's start */
	opxy_copro_common(&m, S10C_LINK_PSOC, &f);
	CHECK(wire_has("PINGME") && !wire_has("not served"));
	/* S-10c.045 before held mode: no transfer starts (so a chunk is
	 * answered as with none), no reboot runs */
	{
		static const uint8_t begin[14] = { 0x02, 0, 0, 0, 0, 0, 0,
						   0, 0, 0, 0x00, 0x08, 0x40,
						   0x00 };
		static const uint8_t chunk[3] = { 0x03, 0xAA, 0xBB };
		static const uint8_t reboot[2] = { 0xEE, 0x02 };

		f.id = 0x0Du;
		f.len = sizeof(begin);
		memcpy(f.payload, begin, sizeof(begin));
		wire_n = 0;
		opxy_copro_common(&m, S10C_LINK_PSOC, &f);
		CHECK(wire_has("cannot prepare the transfer") &&
		      wire_has("transfer not taken before held mode"));
		f.len = sizeof(chunk);
		memcpy(f.payload, chunk, sizeof(chunk));
		wire_n = 0;
		opxy_copro_common(&m, S10C_LINK_PSOC, &f);
		CHECK(wire_has("No update in progress!"));
		f.len = sizeof(reboot);
		memcpy(f.payload, reboot, sizeof(reboot));
		wire_n = 0;
		opxy_copro_common(&m, S10C_LINK_PSOC, &f);
		CHECK(wire_has("reboot not taken before held mode"));
		f.id = 0xACu;
		f.len = 0u;
		wire_n = 0;
		opxy_copro_common(&m, S10C_LINK_PSOC, &f);
		CHECK(wire_has("not served"));	/* S-10c.041: AC is not served */
		f.id = 0x01u;
		f.len = 6u;
		memcpy(f.payload, "PINGME", 6u);
		wire_n = 0;
		opxy_copro_common(&m, S10C_LINK_PSOC, &f);
		CHECK(wire_has("PINGME"));	/* no reboot ran */
	}
	opxy_usb_stop();

	held_setup();
	s06_link_init(&m.link[S06_LINK_NORDIC], &port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_NORDIC);
	m.node.link[S06_LINK_NORDIC] = &m.link[S06_LINK_NORDIC];
	wire_n = 0;
	opxy_copro_common(&m, S10C_LINK_PSOC, &f);
	CHECK(wire_has("PINGME") && !wire_has("not served"));
	f.route = s06_route(S06_ADDR_NORDIC, S06_ADDR_MASTER);
	memcpy(f.payload, "NRFPNG", 6u);
	wire_n = 0;
	opxy_copro_common(&m, S10C_LINK_NORDIC, &f);
	CHECK(wire_has("NRFPNG"));
	f.id = 0x77u;
	wire_n = 0;
	opxy_copro_common(&m, S10C_LINK_NORDIC, &f);
	CHECK(!wire_has("NRFPNG") && wire_has("not served 77"));
	opxy_usb_stop();
}

/* S-16's GP1 test: more than 14 of the first 28 bytes nonzero. */
static unsigned gp1_nonzero;

static bool gp1_get(uint32_t item, uint32_t *data)
{
	unsigned i;

	if (item != WB_OTP_GP1)
		return false;
	memset(data, 0, 64u);
	for (i = 0; i < gp1_nonzero; i++)
		data[i / 4u] |= 0x5Au << (8u * (i % 4u));
	return true;
}

static bool otp_fails(uint32_t item, uint32_t *data)
{
	(void)item;
	(void)data;
	return false;
}

static void test_provisioned(void)
{
	gp1_nonzero = 15u;
	CHECK(opxy_otp_provisioned(gp1_get));
	gp1_nonzero = 14u;
	CHECK(!opxy_otp_provisioned(gp1_get));
	CHECK(!opxy_otp_provisioned(otp_fails));

	setup();
	m.sv.provisioned = true;
	m.boot_reason = 0u;
	CHECK(!opxy_serial_flag(&m));
	m.sv.provisioned = false;
	CHECK(opxy_serial_flag(&m));
}

/* S-16.095 item 1: the TE serial as NUL-padded text (NULL: no read). */
static const char *item1;

static bool item1_get(uint32_t item, uint32_t *data)
{
	unsigned i;

	if (item != WB_OTP_HUK || item1 == NULL)
		return false;
	for (i = 0; i < 32u && item1[i] != '\0'; i++)
		data[i / 4u] |= (uint32_t)(uint8_t)item1[i] << (8u * (i % 4u));
	return true;
}

/* S-10c.055, S-17.015: the chip serial from item 1, and a failed read. */
static void test_chip_serial(void)
{
	char s[OPXY_CHIP_SERIAL_MAX + 1u];
	struct s10c_info v;

	item1 = "ZZ0123456789ABCDEF";
	CHECK(opxy_otp_serial(item1_get, s) && strcmp(s, item1) == 0);
	item1 = "ZZ0123456789ABCDEFGH";		/* 18 characters at most */
	CHECK(opxy_otp_serial(item1_get, s) &&
	      strcmp(s, "ZZ0123456789ABCDEF") == 0);
	item1 = "ZZ01;23";			/* S-04.047: not text */
	CHECK(opxy_otp_serial(item1_get, s) && s[0] == '\0');
	item1 = "ZZ\x80";
	CHECK(opxy_otp_serial(item1_get, s) && s[0] == '\0');
	item1 = NULL;
	CHECK(!opxy_otp_serial(item1_get, s) && s[0] == '\0');

	/* S-04.047: read at start-up, so the boot ops' info has it too */
	setup();
	item1 = "CHIPSERIAL00000042";
	opxy_startup_read(&m, item1_get);
	CHECK(m.sv.chip_ok && m.sv.rev.meta == S17_META_READ);
	m.sv.host_ops.read_info(&m, &v);
	CHECK(v.dsp_ok && memcmp(v.dsp_otp, item1, 18u) == 0);
	opxy_services_start(&m);
	m.sv.host_ops.read_info(&m, &v);
	CHECK(v.dsp_ok && memcmp(v.dsp_otp, item1, 18u) == 0);

	setup();
	opxy_startup_read(&m, otp_fails);
	CHECK(!m.sv.chip_ok && m.sv.rev.meta == S17_META_SERIAL_FAILED);
	CHECK(s17_rev_reported(&m.sv.rev) == 0u);
	m.sv.host_ops.read_info(&m, &v);
	CHECK(!v.dsp_ok);

	/* S-04.047: no read after the record's failed; S-17.015 */
	setup();
	m.meta_failed = true;
	item1 = "CHIPSERIAL00000042";
	opxy_startup_read(&m, item1_get);
	CHECK(!m.sv.chip_ok && m.sv.chip_serial[0] == '\0' &&
	      m.sv.rev.meta == S17_META_RECORD_FAILED);
	opxy_usb_stop();
}

/* The transports are S-10a's; with no transfer begun there is nothing to
 * store or submit. */
static void test_usb_refuses(void)
{
	struct s10c_ops o;
	struct s11_outcome out;

	opxy_usb_host_ops(&m, &o);
	CHECK(o.send_sysex != NULL);
	CHECK(o.cdc_open == s10a_usb_cdc_open);
	CHECK(!o.cdc_open(&m));
	CHECK(!o.transfer_append(&m, (const uint8_t *)"x", 1u));
	o.transfer_submit(&m, S10C_SOURCE_TE, &out);
	CHECK(out.kind == S11_OUTCOME_REFUSED);
}

/* S-02's flash in RAM, for the metadata writes (S-04.043, S-02.064). */
static uint8_t flash_mem[S02_FLASH_SIZE];
static uint8_t flash_scratch[S02_SECTOR_SIZE];
static bool flash_fail;
static bool meta_read_fail;	/* reads of the metadata sector fail */

static int fl_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (meta_read_fail && off >= S02_META_OFF &&
	    off < S02_META_OFF + S02_SECTOR_SIZE)
		return -1;
	memcpy(buf, flash_mem + off, len);
	return 0;
}

static int fl_erase(void *ctx, uint32_t off)
{
	(void)ctx;
	memset(flash_mem + off, 0xFF, S02_SECTOR_SIZE);
	return 0;
}

static int fl_program(void *ctx, uint32_t off, const uint8_t *buf,
		      uint32_t len)
{
	(void)ctx;
	if ((off >= FR_LOG_OFF(0) && off < FR_LOG_OFF(0) + S02_SECTOR_SIZE) ||
	    (off >= FR_LOG_OFF(1) && off < FR_LOG_OFF(1) + S02_SECTOR_SIZE)) {
		lm.n++;
		lm.off = off;
		lm.value = buf[0];
		lm.wire = wire_n;
		lm.inject = inject_next;
	}
	if (flash_fail)
		return -1;
	for (uint32_t i = 0; i < len; i++)
		flash_mem[off + i] &= buf[i];
	return 0;
}

static const struct s02_flash_ops flash_ops = { NULL, fl_read, fl_erase,
						fl_program };

static void flash_setup(void)
{
	memset(flash_mem, 0xFF, sizeof(flash_mem));
	flash_fail = false;
	meta_read_fail = false;
	m.flash.flash = &flash_ops;
	m.flash.crc32 = wire_crc32;
	m.flash.scratch = flash_scratch;
	m.flash.scratch_len = sizeof(flash_scratch);
}

static void flash_meta(struct s04_meta *out)
{
	s04_meta_read(flash_mem + S02_META_OFF, out);
}

/* S-07's log events, for the backplane step's failure. */
static uint32_t last_why;

/* Whether a frame with this id went to the PSoC (the programmer's entry is
 * 0xEE, S-07.081). */
static bool psoc_frame_sent(uint8_t id)
{
	size_t at = 0;

	/* each frame COBS-coded up to its 0x00 (S-06): its third byte */
	while (at < wire_n) {
		size_t end = at, i = at, o = 0;
		uint8_t raw[3] = { 0, 0, 0 };

		while (end < wire_n && wire[end] != 0u)
			end++;
		while (i < end && o < 3u) {
			const uint8_t code = wire[i++];

			for (uint8_t j = 1; j < code && i < end && o < 3u; j++)
				raw[o++] = wire[i++];
			if (code != 0xFFu && i < end && o < 3u)
				raw[o++] = 0;
		}
		if (o == 3u && raw[2] == id)
			return true;
		at = end + 1u;
	}
	return false;
}

static void psoc_why(void *ctx, int event, uint32_t a, uint32_t b)
{
	(void)ctx;
	(void)a;
	if (event == S07_LOG_BACKPLANE)
		last_why = b;
}

/*
 * S-11's run ops: S-04.045's M1, M2, M3 and M4 written to the record; a
 * failed write leaves the values as the flash holds them. With no slave the
 * progress record and the staged images cannot be reached; the backplane
 * step is switched off and the board has no PSoC unlock.
 */
static void test_install_ops(void)
{
	struct s11_run_ops run;
	struct s11_step_ops step;
	const struct s11_version v = { 2u, 1u, 0u, 9u };
	struct s04_meta r;

	setup();
	flash_setup();
	opxy_install_ops(&m, &run, &step);
	CHECK(run.ctx == &m && step.ctx == &m);

	CHECK(run.meta_state(run.ctx, S11_STATE_UPGRADING));	/* M1 */
	flash_meta(&r);
	CHECK(r.state == S04_STATE_UPGRADE && m.meta.state == r.state);
	step.meta_sw_version(step.ctx, &v);			/* M2 */
	flash_meta(&r);
	CHECK(r.sw_version[0] == 2u && r.sw_version[3] == 9u);
	CHECK(r.state == S04_STATE_UPGRADE);
	CHECK(run.meta_complete(run.ctx, &v));			/* M3 */
	flash_meta(&r);
	CHECK(r.os_version[0] == 2u && r.os_version[1] == 1u &&
	      r.os_version[3] == 9u && r.state == S04_STATE_IDLE);

	flash_fail = true;
	wire_n = 0;
	CHECK(!run.meta_state(run.ctx, S11_STATE_UPGRADING));
	CHECK(m.meta.state == S04_STATE_IDLE);
	CHECK(wire_has("metadata write failed"));
	flash_fail = false;

	CHECK(!run.record_write(run.ctx, 1u));	/* no slave */
	CHECK(!step.os_write(step.ctx));
	CHECK(!step.bootloader_write(step.ctx));
	CHECK(!step.spiboot_write(step.ctx));
	/* The board supplies no PSoC unlock (S-07.081), and its backplane
	 * step is switched off (S-11.073; test_board_install runs it Skipped
	 * with its image staged). The programmer itself, were the step on,
	 * fails on the missing unlock as S-07 reports it (why 10), before the
	 * PSoC's bootloader is asked for (0xEE). */
	{
		uint8_t unlock[S07_CY_UNLOCK_LEN];

		CHECK(!opxy_psoc_unlock(unlock));
	}
	CHECK((opxy_steps_off() & S11_STEP_BIT(S11_STEP_KEYBOARD)) != 0u);
	m.psoc_ops.log = psoc_why;
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	last_why = 0;
	wire_n = 0;
	CHECK(!step.psoc_program(step.ctx));
	CHECK(last_why == S07_CY_WHY_UNLOCK && !psoc_frame_sent(0xEEu));
	CHECK(run.step(run.ctx, S11_STEP_DONE) == S11_INTERNAL);
}

/*
 * S-04.043: a metadata write keeps the factory bytes the flash holds now, not
 * a copy taken at boot, and makes no write when the record cannot be read or
 * its restore failed (S-02.065).
 */
static uint8_t factory_rec[S04_META_SIZE];

static void factory_put(bool mirror)
{
	unsigned i;

	for (i = 0; i < S04_META_SIZE; i++)
		factory_rec[i] = (uint8_t)(0x40u + i);
	factory_rec[0] = 0xBEu;			/* the magic */
	factory_rec[1] = 0xBAu;
	factory_rec[2] = 0xADu;
	factory_rec[3] = 0xDEu;
	factory_rec[0x24] = 0;			/* state 0 */
	factory_rec[0x25] = 0;
	factory_rec[0x26] = 0;
	factory_rec[0x27] = 0;
	memcpy(flash_mem + S02_META_OFF, factory_rec, S04_META_SIZE);
	flash_mem[S02_META_OFF + S04_META_SIZE] = 0x5Au;	/* beyond the record */
	if (mirror) {
		uint8_t mir[S02_MIRROR_SIZE];

		s02_mirror_build(factory_rec, mir);
		memcpy(flash_mem + S02_MIRROR_OFF, mir, sizeof(mir));
	}
}

/* Every byte S-04.043 keeps: serial, sku, factory and calibration. */
static bool factory_kept(void)
{
	const uint8_t *r = flash_mem + S02_META_OFF;

	return memcmp(r + 4, factory_rec + 4, 0x12u - 4u) == 0 &&
	       memcmp(r + 0x28, factory_rec + 0x28, S04_META_SIZE - 0x28u) == 0 &&
	       r[S04_META_SIZE] == 0x5Au;
}

static void test_meta_write_base(void)
{
	struct s11_run_ops run;
	struct s11_step_ops step;
	static uint8_t before[S02_FLASH_SIZE];
	struct s04_meta r;

	setup();
	flash_setup();
	opxy_install_ops(&m, &run, &step);

	/* The boot's copy is stale (0xFF): the write keeps the flash's bytes. */
	factory_put(true);
	memset(m.meta_raw, 0xFF, sizeof(m.meta_raw));
	s04_meta_read(factory_rec, &m.meta);
	CHECK(run.meta_state(run.ctx, S11_STATE_UPGRADING));
	flash_meta(&r);
	CHECK(r.state == S04_STATE_UPGRADE && factory_kept());

	/* 0xC000 erased, the mirror valid: the mirror's bytes are kept. */
	factory_put(true);
	memset(flash_mem + S02_META_OFF, 0xFF, S02_SECTOR_SIZE);
	CHECK(run.meta_state(run.ctx, 0u));
	flash_mem[S02_META_OFF + S04_META_SIZE] = 0x5Au;
	CHECK(factory_kept());

	/* Nothing readable: no erase, no program. */
	factory_put(false);
	meta_read_fail = true;
	memcpy(before, flash_mem, sizeof(before));
	CHECK(!run.meta_state(run.ctx, S11_STATE_UPGRADING));
	CHECK(memcmp(before, flash_mem, sizeof(before)) == 0);
	meta_read_fail = false;

	/* The record not read at boot, or its restore failed: no write. */
	factory_put(true);
	memcpy(before, flash_mem, sizeof(before));
	m.meta_failed = true;
	CHECK(!run.meta_state(run.ctx, S11_STATE_UPGRADING));
	m.meta_failed = false;
	m.meta_restore_failed = true;
	CHECK(!run.meta_state(run.ctx, S11_STATE_UPGRADING));
	CHECK(memcmp(before, flash_mem, sizeof(before)) == 0);
	m.meta_restore_failed = false;
}

/* ---- the Nordic link on a port of its own ---- */

static uint8_t nwire[1024];
static size_t nwire_n;
static uint8_t nin[4][S06_MAX_WIRE];
static size_t nin_len[4], nin_n, nin_next;
static bool nin_gap;

/* One frame a poll, then nothing, as port_read(). */
static size_t nport_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t n;

	(void)ctx;
	nin_gap = !nin_gap;
	if (nin_next >= nin_n || !nin_gap)
		return 0;
	n = nin_len[nin_next] < max ? nin_len[nin_next] : max;
	memcpy(buf, nin[nin_next++], n);
	return n;
}

static size_t nport_room(void *ctx)
{
	(void)ctx;
	return sizeof(nwire) - nwire_n;
}

static void nport_write(void *ctx, const uint8_t *data, size_t n)
{
	(void)ctx;
	memcpy(nwire + nwire_n, data, n);
	nwire_n += n;
}

static const struct s06_port_ops nport = { nport_read, nport_room, nport_write };

static void nordic_setup(void)
{
	nwire_n = 0;
	nin_n = nin_next = 0;
	nin_gap = false;
	s06_link_init(&m.link[S06_LINK_NORDIC], &nport, NULL, S06_ADDR_MASTER,
		      S06_ADDR_NORDIC);
	m.node.link[S06_LINK_NORDIC] = &m.link[S06_LINK_NORDIC];
	opxy_nordic_ops(&m, &m.nordic_ops);
	s08_init(&m.nordic, &m.node, S06_LINK_NORDIC, &m.nordic_ops,
		 S08_STATE_IDLE);
}

/* The Nordic greets with this flag byte; received at the next poll. */
static void nordic_greets(uint8_t flag)
{
	const uint8_t p[2] = { flag, 0x02 };

	if (nin_next == nin_n)
		nin_n = nin_next = 0;
	if (nin_n < 4u) {
		nin_len[nin_n] = s06_frame_encode(0x31u, (uint8_t)nin_n, 0x05u, p,
						  sizeof(p), crc8, nin[nin_n]);
		nin_n++;
	}
}

/* Whether the master sent this one-byte message as its frame number seq. */
static bool nordic_sent(uint8_t id, uint8_t byte, uint8_t seq)
{
	uint8_t w[S06_MAX_WIRE];
	const size_t n = s06_frame_encode(0x13u, seq, id, &byte, 1u, crc8, w);

	for (size_t i = 0; i + n <= nwire_n; i++)
		if (memcmp(nwire + i, w, n) == 0)
			return true;
	return false;
}

/*
 * S-08.013 through the board's run ops: the Nordic's greetings are answered
 * by the state a run has set, from M1 on, written or not, and by 0 again
 * after M3 and M4 (S-08.011). S-08.014: a greeting that arrived during a
 * step is answered when the next step begins.
 */
static void test_install_nordic_state(void)
{
	struct s11_run_ops run;
	struct s11_step_ops step;
	const struct s11_version v = { 2u, 1u, 0u, 9u };
	const struct s06_link *l = &m.link[S06_LINK_NORDIC];
	uint8_t seq;

	held_setup();
	flash_setup();
	nordic_setup();
	opxy_install_ops(&m, &run, &step);
	opxy_install_begin(&m, false, false, 0);

	/* no run: a 0xBB greeting is told to leave DFU */
	nordic_greets(0xBBu);
	seq = l->seq;
	(void)s08_poll(&m.nordic);
	CHECK(nordic_sent(0x0Du, 0x18u, seq));

	CHECK(run.meta_state(run.ctx, S11_STATE_UPGRADING));	/* M1 */
	CHECK(m.nordic.state == S08_STATE_UPGRADING);
	nordic_greets(0xBBu);
	seq = l->seq;
	(void)s08_poll(&m.nordic);
	CHECK(nordic_sent(0x0Du, 0xEEu, seq));
	nordic_greets(0xAAu);
	seq = l->seq;
	(void)s08_poll(&m.nordic);
	CHECK(nordic_sent(0x0Du, 0xEEu, seq));

	/* between steps: with no slave the content step finds nothing, and
	 * the greeting that waited is answered before it */
	nordic_greets(0xBBu);
	seq = l->seq;
	CHECK(run.step(run.ctx, S11_STEP_CONTENT) == S11_NOT_FOUND);
	CHECK(nordic_sent(0x0Du, 0xEEu, seq));
	CHECK(l->seq == (uint8_t)(seq + 1u));

	CHECK(run.meta_complete(run.ctx, &v));			/* M3 */
	CHECK(m.nordic.state == S08_STATE_IDLE);
	nordic_greets(0xBBu);
	seq = l->seq;
	(void)s08_poll(&m.nordic);
	CHECK(nordic_sent(0x0Du, 0x18u, seq));

	CHECK(run.meta_state(run.ctx, S11_STATE_UPGRADING));
	CHECK(run.meta_state(run.ctx, 0u));			/* M4 */
	CHECK(m.nordic.state == S08_STATE_IDLE);
	nordic_greets(0xAAu);
	seq = l->seq;
	(void)s08_poll(&m.nordic);
	CHECK(nordic_sent(0x06u, 0x40u, seq));

	/* a write that fails: the state still follows (S-08 Terms) */
	flash_fail = true;
	CHECK(!run.meta_state(run.ctx, S11_STATE_UPGRADING));
	flash_fail = false;
	CHECK(m.nordic.state == S08_STATE_UPGRADING);
	opxy_usb_stop();
}

/* A slot of 0x100 bytes and its record, the CRC right or wrong. */
static void slot_put(unsigned k, bool good)
{
	struct s02_record r;
	uint8_t raw[S02_RECORD_SIZE];

	memset(flash_mem + s02_slot_base(k), 0x30 + (int)k, 0x100);
	r.length = 0x100u;
	r.tag = S02_RECORD_TAG;
	CHECK(s02_flash_crc(&m.flash, s02_slot_base(k), r.length, &r.crc32) ==
	      S02_OK);
	if (!good)
		r.crc32 ^= 1u;
	s02_record_encode(&r, raw);
	memcpy(flash_mem + (k == 0u ? S02_RECORD0_OFF : S02_RECORD1_OFF), raw,
	       sizeof(raw));
}

/*
 * S-02.080: Power + 1 and Power + 2 are no chord. A key-status reply with
 * key 1 or key 2 held alone reads as no hold and no chord (S-07.032,
 * S-07.033), so the boot goes on as any boot does (S-05.001). Held mode
 * after such a start, with the front's record still naming a chord,
 * announces nothing, shows the running slot as on every boot and writes
 * nothing to the flash: the main image acts on no chord.
 */
static void test_no_slot_chords(void)
{
	static const uint8_t k1[8] = { 0x40 }, k2[8] = { 0x80 };
	static uint8_t before[S02_FLASH_SIZE];
	const uint8_t *const reply[2] = { k1, k2 };

	for (unsigned i = 0; i < 2u; i++) {
		struct s07_key_status ks;
		struct boot_held h;
		struct s15_frame f;
		struct s05_decision d;

		s07_key_status_decode(reply[i], sizeof(k1), &ks);
		CHECK(ks.replied && !ks.hold && !ks.debug);
		CHECK(s07_chord_digit(reply[i], sizeof(k1)) == 0u);
		s05_boot_decision(BOOT_REASON_NONE, 0u, S05_KEYS_NOT_HELD, true, &d);
		CHECK(d.boot_os && !d.interrupted);

		memset(&h, 0, sizeof(h));
		held_setup();
		flash_setup();
		slot_put(0u, true);
		slot_put(1u, true);
		memcpy(before, flash_mem, sizeof(before));
		m.front = true;
		m.ho.flags = FR_HO_CHORD_SHOWN;
		m.ho.chord = (uint8_t)(i + 1u);
		m.ho.chord_result = 1u;
		m.running_established = true;
		m.running_slot = 0;
		wire_n = 0;
		opxy_held_screen(&m, &h);
		CHECK(!wire_has("Power + ") && !wire_has("Forced"));
		CHECK(!wire_has("slot 2"));
		s15_menu_frame(&m.sv.menu, &f);
		CHECK(f.slot == 0u && !f.option_on);
		CHECK(m.sv.menu.screen == S15_SCR_MAIN);
		CHECK(memcmp(before, flash_mem, sizeof(before)) == 0);
		opxy_usb_stop();
	}
}

/*
 * S-05.025, S-15.180 on the board (opxy_prompt.c; the host build has the
 * prompt on): the prompt's lines are logged (S-15.004), a bitmap waiting
 * when the prompt opens counts as down then and acts only after an up and
 * a down, the first digit 1 or 2 is returned, and the key hook is
 * restored. The frame is the console's under the title Boot with the
 * running slot and the version text.
 */
static void test_boot_prompt(void)
{
	static const uint8_t k6[S07_BITMAP] = { 0x40 };		/* 0x06 */
	static const uint8_t k7[S07_BITMAP] = { 0x80 };		/* 0x07 */
	static const uint8_t up[S07_BITMAP] = { 0 };
	static const uint8_t ping[1] = { 0xAA };
	struct boot_held h;
	struct s15_frame f;

	setup();
	flash_setup();
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	m.running_established = true;
	m.running_slot = 0;
	m.debug_key = true;
	inject(0x42, k6, sizeof(k6));	/* waiting when the prompt opens */
	inject(0x42, up, sizeof(up));
	inject(0x42, k6, sizeof(k6));	/* up and down again: digit 1 */
	CHECK(opxy_boot_prompt(&m) == BOOT_PROMPT_START);
	CHECK(inject_next == 3u);	/* the first alone chose nothing */
	CHECK(!wire_has("Power + "));
	CHECK(wire_has("1 Start the OS") && wire_has("2 Bootloader menus"));
	CHECK(wire_has("Running  Slot 1  Selector  "));
	CHECK(wire_has("Verified  ") && wire_has("  Entry SP  0x"));
	CHECK(wire_has("Debug key  Held"));
	CHECK(m.psoc_ops.key_event == NULL);	/* restored */
	CHECK(m.sv.menu.screen == S15_SCR_NONE);	/* S-15.013 */
	s15_menu_frame(&m.sv.menu, &f);
	CHECK(f.title != NULL && strcmp(f.title, "Boot") == 0);
	CHECK(f.slot == 0u);
	CHECK(!(f.status & S15_STATUS_USB));
	CHECK(f.version != NULL &&
	      strncmp(f.version, bl_text(), strlen(bl_text())) == 0);
	CHECK(!f.gray && !f.option_on);

	/* Digit 2 held when shown: a ping is answered meanwhile (S-07.076),
	 * and the up and down choose. */
	setup();
	flash_setup();
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	m.running_established = false;
	inject(0x42, k7, sizeof(k7));
	inject(0x01, ping, sizeof(ping));
	inject(0x42, up, sizeof(up));
	inject(0x42, k7, sizeof(k7));
	CHECK(opxy_boot_prompt(&m) == BOOT_PROMPT_HELD);
	CHECK(inject_next == 4u);
	CHECK(wire_has("Running  Unknown  Selector  "));
	CHECK(wire_has("Debug key  Not held"));
	s15_menu_frame(&m.sv.menu, &f);
	/* The running slot is not established: the bare word, unlit. */
	CHECK(f.slot == S15_SLOT_UNKNOWN);
	/* S-15.021: held mode's first screen replaces the prompt. */
	memset(&h, 0, sizeof(h));
	h.reason = BOOT_REASON_NONE;
	opxy_services_start(&m);
	wire_n = 0;
	opxy_held_screen(&m, &h);
	CHECK(wire_has("1 Install firmware"));
	CHECK(m.sv.menu.screen == S15_SCR_MAIN);
	opxy_usb_stop();
}

/*
 * S-11.080: a boot that found the state 0xAB0D sets it to 0 and does
 * nothing else, slave or no slave: no step, no record, no run. The main
 * screen then says that the install did not finish.
 */
static void test_install_interrupted(void)
{
	struct boot_held h;
	struct s04_meta r;

	memset(&h, 0, sizeof(h));
	h.interrupted = true;
	held_setup();
	flash_setup();
	m.meta.state = S04_STATE_UPGRADE;
	s08_set_state(&m.nordic, S08_STATE_UPGRADING);
	wire_n = 0;
	opxy_install_interrupted(&m);
	flash_meta(&r);
	CHECK(r.state == S04_STATE_IDLE && m.meta.state == S04_STATE_IDLE);
	/* S-08.013: the Nordic's greetings are answered by the state 0 */
	CHECK(m.nordic.state == S08_STATE_IDLE);
	CHECK(wire_has("did not finish; state cleared"));
	opxy_held_screen(&m, &h);
	CHECK(m.sv.boot.interrupted && m.sv.menu.screen == S15_SCR_MAIN);
	opxy_usb_stop();

	/* The write fails: the Nordic still goes by 0, and the notice shows. */
	held_setup();
	flash_setup();
	m.meta.state = S04_STATE_UPGRADE;
	flash_fail = true;
	wire_n = 0;
	opxy_install_interrupted(&m);
	flash_fail = false;
	CHECK(wire_has("state not cleared"));
	CHECK(m.nordic.state == S08_STATE_IDLE);
	opxy_held_screen(&m, &h);
	CHECK(m.sv.boot.interrupted);
	opxy_usb_stop();
}

static void test_menu_env(void)
{
	struct s15_env_ops e;
	char name[S15_NAME_MAX + 1];

	setup();
	opxy_menu_env(&m, &e);
	CHECK(!m.sv.option);
	e.option(e.ctx, true);
	CHECK(m.sv.option);
	/* No slave: every file request fails with ENODEV (S-09.008), and no
	 * mount was sent, so the volume is not refused. */
	CHECK(!e.volume_refused(e.ctx));
	CHECK(!e.dir_exists(e.ctx, "/yaffs2"));
	CHECK(!e.pack_dir(e.ctx, 0u, name) && name[0] == '\0');
	CHECK(e.read_manifest(e.ctx, "pack", (uint8_t *)name, 4u) == 0u);
	/* The short calls do not hold the menu's keys; the long ones do. */
	CHECK(!s15_menu_held(&m.sv.menu));
	CHECK(!e.remove_tree(e.ctx, "upgrade"));
	CHECK(s15_menu_held(&m.sv.menu));
	s15_menu_release(&m.sv.menu);
	{
		struct s15_osw_end end = { S15_OSW_FAILED, S15_OSW_WHY_NONE,
					   S15_OSW_EMMC_UNTOUCHED, 0, 0, 0, 0, false };

		/* S-15.094: the request's errno names it; nothing written */
		e.os_write(e.ctx, &end);
		CHECK(end.result == S15_OSW_UNREADABLE && end.why == S15_OSW_WHY_READ &&
		      end.value == S09_ENODEV && end.emmc == S15_OSW_EMMC_UNTOUCHED);
		CHECK(s15_menu_held(&m.sv.menu));
		s15_menu_release(&m.sv.menu);
	}
	{
		/* S-15.172, S-02.098: the settings go to the running slot's
		 * settings sector and nowhere else; no request reaches the slave,
		 * so nothing of the eMMC is read or written */
		static const uint8_t rec[S02_RECORD_SIZE] = {
			0x44, 0x33, 0x22, 0x11, 0x00, 0x00, 0x04, 0x00, 0x04, 0xB0, 0x00, 0x00 };
		struct s15_settings st = { true, false, false, true }, now, back;

		flash_setup();
		memcpy(flash_mem + S02_RECORD1_OFF, rec, sizeof rec);
		m.store_slot.established = true;
		m.store_slot.slot = 1u;
		m.store_slot.good = true;
		s02_record_decode(rec, &m.store_slot.rec);
		m.sv.files.err = 0u;
		CHECK(e.settings_save(e.ctx, &st, &now) == S15_STORE_SAVED);
		CHECK(now.dev_mode && !now.color && !now.mtp_macos && now.start_menu);
		CHECK(flash_mem[0xFE000u] == 0x57u && flash_mem[0xFE000u + 8u] == 0x44u &&
		      flash_mem[0xFE000u + 12u] == 0x09u && flash_mem[0x86000u] == 0xFFu);
		CHECK(m.sv.files.err == 0u && !m.sv.files.mount_sent);
		CHECK(!s15_menu_held(&m.sv.menu));
		opxy_settings_read(&m, &back);
		CHECK(back.dev_mode && !back.color && !back.mtp_macos && back.start_menu);
		/* S-02.105: the factory reset's clear */
		wire_n = 0;
		CHECK(e.settings_clear(e.ctx) == S15_STORE_SAVED && flash_mem[0xFE000u] == 0xFFu);
		CHECK(wire_has("cleared") && m.sv.files.err == 0u);
		/* a running slot not established: refused, logged */
		m.store_slot.established = false;
		wire_n = 0;
		CHECK(e.settings_save(e.ctx, &st, &now) == S15_STORE_UNUSABLE);
		CHECK(!now.dev_mode && now.color && now.mtp_macos && !now.start_menu);
		CHECK(wire_has("not saved: this slot keeps no settings") && flash_mem[0xFE000u] == 0xFFu);
		CHECK(e.settings_clear(e.ctx) == S15_STORE_UNUSABLE);
		CHECK(wire_has("not cleared: this slot keeps no settings"));
		CHECK(m.sv.files.err == 0u && !s15_menu_held(&m.sv.menu));
	}
	wire_n = 0;
	CHECK(!e.format(e.ctx));
	CHECK(wire_has("yaffs2 format failed"));
	CHECK(s15_menu_held(&m.sv.menu));
	s15_menu_release(&m.sv.menu);
	e.erase_os_fat(e.ctx);
	CHECK(wire_has("os fat erase failed"));
	CHECK(s15_menu_held(&m.sv.menu));
}

/* The test's RTC, in place of src/rtc_board.c's. */
static bool rtc_ok;
static uint32_t rtc_clk;

bool opxy_rtc_read(uint32_t *clk)
{
	*clk = rtc_clk;
	return rtc_ok;
}

/* The register as it holds it: up once held mode started the RTC. */
bool opxy_rtc_peek(uint32_t *clk)
{
	*clk = rtc_clk;
	return true;
}

void opxy_rtc_write(uint32_t clk)
{
	rtc_clk = clk;
}

/* Its start too, so that a link that reaches held mode's code
 * (opxy_held.c) takes none of opxy_rtc_board.c. */
void opxy_rtc_board_start(const struct wb_time *t)
{
	(void)t;
}

/* S-12.016's stamps from S-04.070's count. */
static void test_rtc_utc(void)
{
	uint64_t utc = 0;

	rtc_ok = false;
	CHECK(!opxy_rtc_utc(&m, &utc));
	rtc_ok = true;
	CHECK(s04_rtc_encode(2026u, 11u, 1u, 12u, 34u, 56u, &rtc_clk));
	CHECK(opxy_rtc_utc(&m, &utc) && utc == 1793536496u);
	CHECK(s04_rtc_encode(2000u, 2u, 29u, 0u, 0u, 0u, &rtc_clk));
	CHECK(opxy_rtc_utc(&m, &utc) && utc == 951782400u);
	rtc_ok = false;
}

/*
 * S-10b on the MTP sets: the device's battery (S-07) and clock (S-04.070);
 * with no slave the store has no volume (S-10b.031's empty store) and
 * nothing reads.
 */
static void test_mtp_ops(void)
{
	uint64_t total, free_bytes;
	uint8_t b[4], le[4];
	uint32_t clk, secs;

	usb_setup();
	m.psoc.batt.charge = 42u;
	CHECK(m.sv.mtp_device.battery(m.sv.mtp_device.ctx) == 42u);
	rtc_ok = true;
	rtc_clk = 0x12345u;
	CHECK(m.sv.mtp_device.clock_read(m.sv.mtp_device.ctx) == 0x12345u);
	m.sv.mtp_device.clock_write(m.sv.mtp_device.ctx, 0x54321u);
	CHECK(rtc_clk == 0x54321u);
	CHECK(!m.clock_msg.sent);		/* no slave: nothing sent */

	/* S-10b.063, S-09.046: with the slave, it is told the new time at
	 * once, and a clock that has not changed is not sent again. */
	m.slave_present = true;
	s06_link_init(&m.link[S06_LINK_CPU], &port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_SLAVE);
	m.node.link[S06_LINK_CPU] = &m.link[S06_LINK_CPU];
	/* seconds with no zero byte, which COBS would change on the wire */
	CHECK(s04_rtc_encode(2026u, 11u, 1u, 12u, 34u, 56u, &clk));
	wire_n = 0;
	m.sv.mtp_device.clock_write(m.sv.mtp_device.ctx, clk);
	secs = s09_clock_seconds(clk);
	CHECK(secs == 0x6AE731F0u);
	le[0] = (uint8_t)secs;
	le[1] = (uint8_t)(secs >> 8);
	le[2] = (uint8_t)(secs >> 16);
	le[3] = (uint8_t)(secs >> 24);
	CHECK(wire_has_bytes(le, sizeof(le)));
	CHECK(m.clock_msg.sent && m.clock_msg.last == secs);
	wire_n = 0;
	opxy_clock_to_slave(&m);
	CHECK(wire_n == 0u);
	/* S-09.046: a clock that does not tick is still the register's
	 * value, sent once; it never changes, so nothing follows. */
	rtc_ok = false;
	rtc_clk = 0x11111u;
	opxy_clock_to_slave(&m);
	CHECK(m.clock_msg.last == s09_clock_seconds(0x11111u));
	CHECK(wire_n > 0u);
	wire_n = 0;
	opxy_clock_to_slave(&m);
	CHECK(wire_n == 0u);
	m.slave_present = false;
	CHECK(!m.sv.mtp_volume.space(m.sv.mtp_volume.ctx, &total,
				     &free_bytes));
	CHECK(!m.sv.mtp_volume.list(m.sv.mtp_volume.ctx, "/", NULL, NULL));
	CHECK(m.sv.mtp_volume.read(m.sv.mtp_volume.ctx, "/os.ldr", 0u, b,
				   sizeof(b)) < 0);
	CHECK(strcmp(m.sv.bl_text, bl_text()) == 0);	/* DeviceVersion */
	opxy_usb_stop();
}

static void test_fsn_unreadable(void)
{
	uint32_t w[4];

	CHECK(!opxy_read_fsn(w));
}

/* --- S-02.095: where the main image marks its start finished --- */

/* The front's record names entry `entry` of slot `slot`; no mark yet. */
static void front_logged(uint8_t slot, uint16_t entry)
{
	memset(&lm, 0, sizeof(lm));
	loop_limit = loop_reads = 0;
	m.front = true;
	m.ho.flags = FR_HO_START_LOGGED;
	m.ho.log_slot = slot;
	m.ho.log_entry = entry;
}

static void mark_done(void)
{
	loop_limit = 0;
	opxy_usb_stop();
}

/* The boot prompt: shown (its lines logged), before the wait reads a key. */
static void test_mark_prompt(void)
{
	static const uint8_t k1[S07_BITMAP] = { 0x40 };		/* 0x06: digit 1 */
	static const uint8_t up[S07_BITMAP] = { 0 };

	setup();
	flash_setup();
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	front_logged(0, 0);
	inject(0x42, up, sizeof(up));
	inject(0x42, k1, sizeof(k1));
	loop_limit = 64u;
	if (setjmp(mark_jmp) == 0)
		CHECK(opxy_boot_prompt(&m) == BOOT_PROMPT_START);
	CHECK(lm.n == 1u && lm.off == FR_LOG_OFF(0) && lm.value == 0xFDu);
	CHECK(wire_find("1 Start the OS") < lm.wire);
	CHECK(lm.inject == 0u);
	mark_done();
}

/* No front record (the test OS: opxy_front_take() is false, as on the
 * host): the prompt marks nothing, whatever the record's memory says. */
static void test_mark_prompt_no_record(void)
{
	static const uint8_t k1[S07_BITMAP] = { 0x40 };
	static const uint8_t up[S07_BITMAP] = { 0 };

	setup();
	flash_setup();
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	front_logged(0, 0);
	m.front = opxy_front_take(&m.ho);
	CHECK(!m.front);
	inject(0x42, up, sizeof(up));
	inject(0x42, k1, sizeof(k1));
	loop_limit = 64u;
	if (setjmp(mark_jmp) == 0)
		CHECK(opxy_boot_prompt(&m) == BOOT_PROMPT_START);
	CHECK(lm.n == 0u);
	mark_done();
}

int main(void)
{
	test_own_version();
	test_log_sinks();
	test_crc_drop_logged();
	test_draw_recorder();
	test_poll_without_display();
	test_usb_sets();
	test_usb_select();
	test_usb_dev_serial();
	test_midi_carrier_to_host();
	test_cdc_closed_drops();
	test_midi_send_waits();
	test_held_screen();
	test_failed_start();
	test_scan_not_found();
	test_disk_ddr();
	test_disk_parts();
	test_disk_offer();
	test_disk_released();
	test_battery_wait();
	test_held_power_off();
	test_powered_off();
	test_restart_hold();
	test_continue_restart();
	test_reboot_settle();
	test_reboot_os_detach();
	test_nordic_link_stop();
	test_reboot_drain();
	test_reboot_never_attached();
	test_host_upgrade_screen();
	test_transfer_file();
	test_transfer_refused_in_place();
	test_system_info();
	test_provisioned();
	test_chip_serial();
	test_nordic_ops();
	test_no_slot_chords();
	test_boot_prompt();
	test_copro_common();
	test_usb_refuses();
	test_install_ops();
	test_meta_write_base();
	test_install_nordic_state();
	test_install_interrupted();
	test_menu_env();
	test_fsn_unreadable();
	test_rtc_utc();
	test_mtp_ops();
	test_mark_prompt();
	test_mark_prompt_no_record();
	return check_summary("test_board_services");
}
