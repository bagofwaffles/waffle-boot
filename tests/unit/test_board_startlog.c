/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-02.095: the main image's finished mark in the start log, from the
 * front's hand-off record (opxy_front.c), and where the boot makes it:
 * held mode's first pass (opxy_held.c), the battery wait's loop and the
 * boot path's release (opxy_master.c). The boot prompt's is
 * test_board_services.c's.
 */
#include "support/check.h"

#include <setjmp.h>
#include <string.h>

#include "support/fake_core.h"
#include "support/fake_mmr.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_display.h"
#include "waffle/drivers/bf70x_lcd.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/s13c_panel.h"
#include "waffle/wire_crc.h"

static struct opxy_master m;
static unsigned programs;
static uint32_t prog_off, prog_len;
static uint8_t prog_byte;
static int prog_rc;

/*
 * A loop under test leaves by loop_jmp: at the mark when jump is set (1),
 * or once the PSoC link has been read read_limit times (2), so a mark that
 * never comes ends the test. What had happened by the mark is kept in at.
 */
static jmp_buf loop_jmp;
static bool jump;
static unsigned reads, read_limit, stages;
static size_t writes;
static struct {
	unsigned reads;		/* PSoC link reads */
	size_t writes;		/* bytes sent to the PSoC */
	bool usb;		/* USB up */
	bool bridge_absent;	/* the bridge marked absent */
	unsigned stages;	/* stage() calls */
} at;

static int fake_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	(void)off;
	memset(buf, 0xFF, len);
	return 0;
}

static int fake_erase(void *ctx, uint32_t off)
{
	(void)ctx;
	(void)off;
	CHECK(0);	/* the mark never erases */
	return -1;
}

static int fake_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	(void)ctx;
	programs++;
	prog_off = off;
	prog_len = len;
	prog_byte = buf[0];
	at.reads = reads;
	at.writes = writes;
	at.usb = opxy_usb_up();
	at.bridge_absent = m.bridge.absent;
	at.stages = stages;
	if (jump)
		longjmp(loop_jmp, 1);
	return prog_rc;
}

static const struct s02_flash_ops fake = { NULL, fake_read, fake_erase, fake_program };

static void reset(bool front, uint32_t flags, uint8_t slot, uint16_t entry)
{
	memset(&m, 0, sizeof m);
	m.flash.flash = &fake;
	m.front = front;
	m.ho.flags = flags;
	m.ho.log_slot = slot;
	m.ho.log_entry = entry;
	programs = 0;
	prog_off = prog_len = 0;
	prog_byte = 0;
	prog_rc = 0;
	memset(&at, 0, sizeof at);
	jump = false;
	reads = read_limit = stages = 0;
	writes = 0;
}

static void test_finish_at(void)
{
	struct fr_handoff ho;
	uint32_t off = 0;
	uint8_t v = 0;
	static const uint8_t want[4] = { 0xFDu, 0xF7u, 0xDFu, 0x7Fu };

	memset(&ho, 0, sizeof ho);
	/* no flag: nothing named */
	ho.log_slot = 0;
	ho.log_entry = 5;
	CHECK(!opxy_log_finish_at(&ho, &off, &v));
	ho.flags = FR_HO_START_LOGGED;
	/* each entry position within a byte, both slots */
	for (unsigned slot = 0; slot < 2u; slot++)
		for (unsigned e = 40; e < 44u; e++) {
			ho.log_slot = (uint8_t)slot;
			ho.log_entry = (uint16_t)e;
			CHECK(opxy_log_finish_at(&ho, &off, &v));
			CHECK(off == (slot == 0u ? 0x87000u : 0xFF000u) + 10u);
			CHECK(v == want[e & 3u]);
		}
	/* the first and last entries */
	ho.log_slot = 1;
	ho.log_entry = 0;
	CHECK(opxy_log_finish_at(&ho, &off, &v) && off == 0xFF000u && v == 0xFDu);
	ho.log_entry = (uint16_t)(FR_LOG_ENTRIES - 1u);
	CHECK(opxy_log_finish_at(&ho, &off, &v) && off == 0xFFFFFu && v == 0x7Fu);
	/* a bad entry or slot */
	ho.log_entry = (uint16_t)FR_LOG_ENTRIES;
	CHECK(!opxy_log_finish_at(&ho, &off, &v));
	ho.log_entry = 0xFFFFu;
	CHECK(!opxy_log_finish_at(&ho, &off, &v));
	ho.log_entry = 3;
	ho.log_slot = 2;
	CHECK(!opxy_log_finish_at(&ho, &off, &v));
	ho.log_slot = 0xFF;
	CHECK(!opxy_log_finish_at(&ho, &off, &v));
}

static void test_once(void)
{
	/* one 1-byte program, then none however often it is called */
	reset(true, FR_HO_START_LOGGED | FR_HO_GREETED, 1, 6);
	opxy_start_finished(&m);
	CHECK(programs == 1u && prog_len == 1u);
	CHECK(prog_off == 0xFF001u && prog_byte == 0xDFu);
	opxy_start_finished(&m);
	opxy_start_finished(&m);
	CHECK(programs == 1u);

	/* a failed program is not tried again, and returns */
	reset(true, FR_HO_START_LOGGED, 0, 0);
	prog_rc = -1;
	opxy_start_finished(&m);
	opxy_start_finished(&m);
	CHECK(programs == 1u && prog_off == 0x87000u && prog_byte == 0xFDu);
}

static void test_nothing_named(void)
{
	/* no front record (the test OS, an old front) */
	reset(false, FR_HO_START_LOGGED, 0, 1);
	opxy_start_finished(&m);
	CHECK(programs == 0u);
	/* the flag clear */
	reset(true, FR_HO_GREETED, 0, 1);
	opxy_start_finished(&m);
	CHECK(programs == 0u);
	/* a slot or entry out of range */
	reset(true, FR_HO_START_LOGGED, 2, 1);
	opxy_start_finished(&m);
	CHECK(programs == 0u);
	reset(true, FR_HO_START_LOGGED, 0, (uint16_t)FR_LOG_ENTRIES);
	opxy_start_finished(&m);
	CHECK(programs == 0u);
}

/* --- the boot's points, with the board's own code --- */

/* The PSoC's port: what is set to arrive, counted reads and writes. */
static const uint8_t *psoc_in;
static size_t psoc_in_n;

static size_t psoc_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t n = psoc_in_n < max ? psoc_in_n : max;

	(void)ctx;
	if (read_limit != 0u && ++reads > read_limit)
		longjmp(loop_jmp, 2);
	memcpy(buf, psoc_in, n);
	psoc_in += n;
	psoc_in_n -= n;
	return n;
}

static size_t any_room(void *ctx)
{
	(void)ctx;
	return 4096u;
}

static void psoc_write(void *ctx, const uint8_t *data, size_t n)
{
	(void)ctx;
	(void)data;
	writes += n;
}

static size_t none_read(void *ctx, uint8_t *buf, size_t max)
{
	(void)ctx;
	(void)buf;
	(void)max;
	return 0;
}

static void drop_write(void *ctx, const uint8_t *data, size_t n)
{
	(void)ctx;
	(void)data;
	(void)n;
}

static const struct s06_port_ops psoc_port = { psoc_read, any_room, psoc_write };
static const struct s06_port_ops quiet_port = { none_read, any_room, drop_write };

/* The master as the boot leaves it before held mode or the battery wait:
 * the PSoC and Nordic links, the log, USB's host ops, no slave. */
static void board(bool front)
{
	reset(front, FR_HO_START_LOGGED, 0, 0);
	fake_mmr_reset();
	fake_core_reset();
	psoc_in_n = 0;
	m.hw.time = opxy_time;
	m.clock = opxy_clock(&m.hw);
	s09_master_set_absent(&m.bridge);
	s09_files_init(&m.sv.files, &m.bridge);
	s06_node_init(&m.node, wire_crc8_frame, &m.clock);
	s06_link_init(&m.link[S06_LINK_PSOC], &psoc_port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_PSOC);
	m.node.link[S06_LINK_PSOC] = &m.link[S06_LINK_PSOC];
	s06_link_init(&m.link[S06_LINK_NORDIC], &quiet_port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_NORDIC);
	m.node.link[S06_LINK_NORDIC] = &m.link[S06_LINK_NORDIC];
	opxy_usb_host_ops(&m, &m.sv.host_ops);
	opxy_startup_read(&m, opxy_otp_get);
	opxy_log_start(&m);
	m.psoc_ops.ctx = &m;
	m.psoc_ops.usb_msc = opxy_battery_usb;	/* as psoc_link_init() sets it */
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
}

/*
 * Held mode marks at the start of its first pass: its set-up returned (the
 * first screen chosen) and nothing of the pass done. The PSoC's waiting
 * frame was read before the first screen (S-15.013), once.
 */
static void test_held(void)
{
	static const uint8_t ping[1] = { 0xAA };
	static uint8_t wire[S06_MAX_WIRE];
	static struct boot_held h;
	int r;

	board(true);
	m.ho.log_entry = 9;
	opxy_nordic_ops(&m, &m.nordic_ops);
	m.nordic_ops.crc16 = wire_crc16_xmodem;
	s08_init(&m.nordic, &m.node, S06_LINK_NORDIC, &m.nordic_ops, 0);
	opxy_services_start(&m);
	memset(&h, 0, sizeof(h));
	h.reason = BOOT_REASON_NONE;
	psoc_in_n = s06_frame_encode(s06_route(S06_ADDR_PSOC, S06_ADDR_MASTER), 0, 0x01,
				     ping, sizeof(ping), wire_crc8_frame, wire);
	psoc_in = wire;
	read_limit = 64u;
	jump = true;
	r = setjmp(loop_jmp);
	if (r == 0)
		opxy_held_service(&m, &h);
	jump = false;
	CHECK(r == 1 && programs == 1u);
	CHECK(prog_off == FR_LOG_OFF(0) + 2u && prog_byte == 0xF7u);
	CHECK(at.reads != 0u && psoc_in_n == 0u);
	CHECK(at.writes != 0u);		/* the ping answered then */
	CHECK(m.sv.menu.screen == S15_SCR_MAIN);
	opxy_usb_stop();
}

/* S-13c.025: the line under the logo, seen on the panel bus as its first
 * CASET (a command byte: PA5, D/C, low, on SMC bank 0) after the front's
 * logo was taken on; what had been done by then is kept. */
static unsigned casets;
static int dc = -1;
static struct {
	bool mount_sent;	/* the mount requested */
	bool usb;		/* USB up */
	bool greeted;		/* the Nordic greeted */
} at_line;

static bool panel_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		       wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op != FAKE_MMR_WRITE)
		return true;
	if (addr == PORT_DATA_CLR(WB_PORT_A) && (*value & (1u << 5)))
		dc = 0;
	else if (addr == PORT_DATA_SET(WB_PORT_A) && (*value & (1u << 5)))
		dc = 1;
	else if (addr == WB_LCD_SMC_BANK0 && dc == 0 && (uint8_t)*value == S13C_CASET &&
		 casets++ == 0u) {
		at_line.mount_sent = m.sv.files.mount_sent;
		at_line.usb = opxy_usb_up();
		at_line.greeted = m.nordic.greeted;
	}
	return true;
}

/* Held mode's entry, whole, to its first pass: the line is sent first,
 * before the mount is requested (S-09.018), USB starts and the Nordic is
 * greeted, and once only; the first screen still waits for them. */
static void test_held_line(void)
{
	static struct boot_held h;
	int r;

	board(true);
	m.slave_present = true;		/* the bridge absent: the mount goes to no one */
	casets = 0;
	memset(&at_line, 0, sizeof at_line);
	fake_mmr_set_hook(panel_hook, NULL);
	/* the panel as the front leaves it: on, its logo the last frame */
	opxy_master.hw.time = opxy_time;
	opxy_display_adopt(2u);
	CHECK(opxy_display() != NULL && casets == 0u);
	memset(&h, 0, sizeof(h));
	h.reason = BOOT_REASON_NONE;
	/* 10 us per counter read: the Nordic's greeting and the info
	 * exchanges time out on the silent links in a bounded loop */
	fake_core_set_cycle_step(4000u);
	jump = true;
	r = setjmp(loop_jmp);
	if (r == 0)
		opxy_held(&m, &h);
	jump = false;
	fake_core_set_cycle_step(1u);
	CHECK(r == 1 && programs == 1u);
	/* the line's CASET and the frame window's after it, once */
	CHECK(casets == 2u);
	CHECK(!at_line.mount_sent && !at_line.usb && !at_line.greeted);
	CHECK(m.sv.files.mount_sent && m.nordic.greeted);
	CHECK(m.sv.menu.screen == S15_SCR_MAIN);
	fake_mmr_set_hook(NULL, NULL);
	opxy_usb_stop();
}

/* The battery wait marks at its loop's start: USB up, and before the loop's
 * first battery query. */
static void test_battery(void)
{
	int r;

	board(true);
	m.ho.log_slot = 1;
	m.ho.log_entry = 6;
	CHECK(!s07_battery_usable(&m.psoc.batt));	/* the loop runs */
	read_limit = 64u;
	jump = true;
	r = setjmp(loop_jmp);
	if (r == 0)
		(void)s07_battery_rest(&m.psoc);
	jump = false;
	CHECK(r == 1 && programs == 1u);
	CHECK(prog_off == FR_LOG_OFF(1) + 1u && prog_byte == 0xDFu);
	CHECK(at.usb && m.sv.usb_state == S10A_STATE_BATTERY);
	CHECK(at.writes == 0u && at.reads == 0u);
	opxy_battery_usb(&m, false);
	opxy_usb_stop();
}

/* The boot path: the board's release op, in S-05.018's sequence with the
 * other ops stood in for. */
static uint8_t staged[16];
static enum s05_fail stage_result;
static unsigned held_cause;

static void no_pre_load(void *ctx)
{
	(void)ctx;
}

static enum s05_fail fake_stage(void *ctx, uint32_t *length)
{
	(void)ctx;
	stages++;
	*length = sizeof(staged);
	return stage_result;
}

static void fake_held(void *ctx, enum s05_fail cause)
{
	(void)ctx;
	held_cause = (unsigned)cause;
}

static void no_select(void *ctx, bool on)
{
	(void)ctx;
	(void)on;
}

/* One FINAL block of count 0, which S-05.024 accepts; with good false its
 * header fails its check. */
static void stage_image(bool good)
{
	uint8_t x = 0;

	memset(staged, 0, sizeof(staged));
	staged[1] = (uint8_t)(S05_FLAG_FINAL >> 8);
	staged[3] = S05_LDR_SIGNATURE;
	for (unsigned i = 0; i < sizeof(staged); i++)
		x = (uint8_t)(x ^ staged[i]);
	staged[2] = good ? x : (uint8_t)(x ^ 1u);
}

static void boot_path(bool front, bool good, enum s05_fail result)
{
	static struct s09_bus_ops bus;
	struct s05_master_ops ops;
	const struct s05_master sm = { staged, 0u, false };

	reset(front, FR_HO_START_LOGGED, 1, 7);
	opxy_master_handoff(&m);
	/* A bridge not yet failed: s09_master_release() makes no session
	 * (released) and then marks it absent. */
	memset(&bus, 0, sizeof(bus));
	bus.select = no_select;
	m.bridge.bus = &bus;
	m.bridge.absent = false;
	m.bridge.released = true;
	stage_image(good);
	stage_result = result;
	held_cause = 99u;
	ops = m.handoff;
	ops.pre_load = no_pre_load;
	ops.stage = fake_stage;
	ops.held = fake_held;
	s05_master_handoff(&ops, &sm);
}

/* After the OS image is staged and checked, before s09_master_release(). */
static void test_boot_path(void)
{
	boot_path(true, true, S05_FAIL_NONE);
	CHECK(programs == 1u && at.stages == 1u);
	CHECK(prog_off == FR_LOG_OFF(1) + 1u && prog_byte == 0x7Fu);
	CHECK(!at.bridge_absent && m.bridge.absent);
	CHECK(held_cause == S05_FAIL_SLAVE);
	/* An image S-05.024 refuses, or none staged: no mark. */
	boot_path(true, false, S05_FAIL_NONE);
	CHECK(programs == 0u && held_cause == S05_FAIL_REFUSED);
	boot_path(true, true, S05_FAIL_NO_OS);
	CHECK(programs == 0u && held_cause == S05_FAIL_NO_OS);
}

/* No front record (the test OS: opxy_front_take() is false there, as on
 * the host): neither point marks, whatever the record's memory says. */
static void test_no_record(void)
{
	boot_path(false, true, S05_FAIL_NONE);
	CHECK(!opxy_front_take(&m.ho));
	CHECK(programs == 0u && held_cause == S05_FAIL_SLAVE);
	board(opxy_front_take(&m.ho));
	m.ho.flags = FR_HO_START_LOGGED;
	opxy_battery_usb(&m, true);
	CHECK(programs == 0u && opxy_usb_up());
	opxy_battery_usb(&m, false);
	opxy_usb_stop();
}

int main(void)
{
	RUN(test_finish_at);
	RUN(test_once);
	RUN(test_nothing_named);
	RUN(test_held);
	RUN(test_held_line);
	RUN(test_battery);
	RUN(test_boot_path);
	RUN(test_no_record);
	return check_summary("test_board_startlog");
}
