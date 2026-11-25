/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The check suite's runner on the master (wiki/Installer-Reference.md;
 * waffle/board/testos_check.h), in the installer variant alone: held mode's
 * loop in place of the menus, so the boot menu (with its format and factory
 * reset) is never reachable. It looks at the unit's OS partition first,
 * then runs each automatic test, writes check.txt to the RAM disk, offers
 * the guided tests, and hands over to the installer
 * (src/testos/opxy_testos_installer.c).
 *
 * What the suite may change: one temporary file on /yaffs2, the RAM disk.
 * The installer's own writes are its file's to list. No OS write, no PSoC
 * 0xEE, 0x97, 0x48, 0x71 or 0x34, and on a device without
 * pmic-pass-through no 0x01 either (S-07 Devices).
 *
 * The suite is the device's (opxy_dev(), decided at the entry, S-18): on
 * the OP-1F there is no slave and no bridge to test, and the eMMC is the
 * main CPU's own (S-13b.060), reached through the same bridge client. The
 * survey build (OPXY_TESTOS_SURVEY) makes the OS active again as soon as
 * it has looked, so that a later fault leaves the unit's OS to start. The Nordic link passes only the frames of
 * its gate. USB offers the RAM disk and the installer's serial port, without
 * the unit's serial; neither S-10c's host commands nor the developer host
 * can be reached through it.
 */
#include "waffle/testos.h"

#if defined(OPXY_TESTOS_CHECK)

#include <string.h>

#include "waffle/board/opxy_boot.h"
#include "waffle/util/bytes.h"
#include "waffle/display/display.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_dmc.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/bf70x_spi.h"
#include "waffle/drivers/bf70x_spiflash.h"
#include "waffle/drivers/bf70x_usb.h"
#include "waffle/drivers/mmr.h"
#include "waffle/usb/device.h"
#include "waffle/display/panel.h"
#include "waffle/ui/lines.h"
#include "waffle/boot/revisions.h"
#include "waffle/board/installer.h"
#include "waffle/board/testos_check.h"
#include "waffle/fs/fat.h"

#define REQUEST_US 100000u		/* S-07.011, S-08.020 */
#define LINK_TOTAL 0x800000u		/* the bridge's soak: 8 MiB */
#define RTC_WAIT_US 1500000u
#define TE_WAIT_US 50000u
#define PSOC_PING 0x01u
#define PSOC_BATTERY_QUERY 0x36u	/* S-07.053 */
#define PSOC_KEY_STATUS 0x41u		/* S-07.032 */
#define NORDIC_GREETING 0x05u		/* S-08.002 */
#define NORDIC_TRIES 200u
#define NORDIC_LEAVE_US 10000000u	/* its bootloader told to leave: the application by then */
#define NORDIC_ASK_US 500000u

static struct {
	struct tchk_facts f;
	struct tchk_table t;
	struct tchk_guard guard;
	struct s03_env env;
	struct s15_console con;
	char version[24];
	bool gray;		/* S-15.173's gray rendering */
	char report[TCHK_REPORT_MAX];
	size_t report_len;
	/* key events from the PSoC (S-07.031), for the loop */
	uint8_t key[32];
	bool down[32];
	unsigned kin, kout;
} ck;

/* S-13b.054: on a cache line, as the eMMC driver takes it on the OP-1F */
static _Alignas(32) uint8_t scratch[S09_MAX_SECTORS * S03_SECTOR];
static _Alignas(4) uint8_t link_pl[S09_TEST_MAX];
static const struct s06_port_ops *nordic_port;

static uint64_t now_us(void)
{
	return wb_time_us(&opxy_time);
}

/* --- the Nordic link's gate (S-08.030 to S-08.046 never sent) --- */

static size_t gate_read(void *ctx, uint8_t *buf, size_t max)
{
	return nordic_port->read(ctx, buf, max);
}

static size_t gate_room(void *ctx)
{
	return nordic_port->room(ctx);
}

static void gate_write(void *ctx, const uint8_t *data, size_t n)
{
	unsigned id;

	if (tchk_nordic_allowed(data, n, &id)) {
		if (id == 0x0Du && ck.f.nordic_left < 0xFFu)
			ck.f.nordic_left++;
		nordic_port->write(ctx, data, n);
	}
	else
		ck.f.nordic_refused++;
}

static const struct s06_port_ops gate_port = { gate_read, gate_room, gate_write };

/* --- keys, the loop's pass, the screen --- */

static void key_event(void *ctx, uint8_t key, bool down)
{
	(void)ctx;
	if (ck.kin - ck.kout < sizeof ck.key) {
		ck.key[ck.kin % sizeof ck.key] = key;
		ck.down[ck.kin % sizeof ck.key] = down;
		ck.kin++;
	}
}

/* The PSoC's log events, counted on their way to the usual log. */
static void (*psoc_log_next)(void *ctx, int event, uint32_t a, uint32_t b);

static void psoc_log(void *ctx, int event, uint32_t a, uint32_t b)
{
	if (event == S07_LOG_GREET_FAILED && ck.f.psoc_greet_failed < 0xFFu)
		ck.f.psoc_greet_failed++;
	if (psoc_log_next != NULL)
		psoc_log_next(ctx, event, a, b);
}

static bool next_key(unsigned *key, bool *down)
{
	if (ck.kin == ck.kout)
		return false;
	*key = ck.key[ck.kout % sizeof ck.key];
	*down = ck.down[ck.kout % sizeof ck.key];
	ck.kout++;
	return true;
}

/* S-07.051: the PSoC removes the power; nothing more. */
static void powered_off(void)
{
	for (;;)
		wb_core_idle();
}

/* One pass of held mode's loop, without the menus (opxy_held_service()). */
static void pass(struct opxy_master *m)
{
	s06_node_poll(&m->node);
	if (s07_service_pass(&m->psoc) == S07_POWEROFF || s08_poll(&m->nordic) != 0)
		powered_off();
	if (m->slave_present)
		s09_master_service(&m->bridge);
	opxy_clock_to_slave(m);
	opxy_usb_poll(m);
	opxy_display_send();
}

/* Inside a long operation: the USB stack and the links' bytes alone. */
static void breathe(void *ctx)
{
	struct opxy_master *m = ctx;

	s06_node_poll(&m->node);
	opxy_usb_breathe();
}

static const struct s15_frame *frame(void)
{
	static struct s15_frame f;

	f.title = "Installer";
	f.version = ck.version;
	f.status = 0;
	f.slot = S15_SLOT_UNKNOWN;
	f.option_on = false;
	f.fatal = false;
	f.gray = ck.gray;
	return &f;
}

static void show(struct opxy_master *m)
{
	const struct s15_geometry *g = opxy_display_geometry();

	/* cppcheck-suppress knownConditionTrueFalse ; a weak default */
	if (g == NULL)
		return;
	s15_render_console(&ck.con, g, &m->sv.draw, frame());
	opxy_display_send();
}

static void lines(enum s15_style style, const char *const *l, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++)
		s15_console_add(&ck.con, style, l[i]);
}

static void render(void);

/* The first screen while the automatic tests run: what loading cost, and
 * the test under way. */
static void progress(struct opxy_master *m, enum tchk_id id)
{
	char s[S15_LINE];
	const size_t n = strlen(tchk_tests[id].name);

	s15_console_clear(&ck.con);
	s15_console_add(&ck.con, S15_ST_HEAD, tchk_first_lines[0]);
	lines(S15_ST_TEXT, &tchk_first_lines[1], TCHK_FIRST_LINES - 1u);
	memcpy(s, tchk_tests[id].name, n);
	memcpy(s + n, "\ttesting", sizeof "\ttesting");
	s15_console_add(&ck.con, S15_ST_STEP_RUN, s);
	show(m);
	/* the results so far, for the USB port's protocol */
	render();
	opxy_installer_table(&ck.t, ck.report, ck.report_len);
	opxy_installer_note(INST_CHECKING, id);
	pass(m);
}

/* --- check.txt --- */

static void render(void)
{
	tchk_table_init(&ck.t);
	tchk_judge(&ck.f, &ck.t);
	ck.report_len = tchk_report(&ck.t, &ck.f.sw, ck.version, ck.report, sizeof ck.report);
	ck.report_len = opxy_installer_report(ck.report, ck.report_len, sizeof ck.report);
}

static bool write_file(struct opxy_master *m)
{
	uint64_t utc = 0;
	const bool stamp = opxy_rtc_utc(m, &utc);

	return wire_fat_write_file(&m->sv.fat, "check.txt", (const uint8_t *)ck.report,
				   tchk_report_pad(ck.report, ck.report_len, sizeof ck.report), stamp,
				   utc);
}

/* The RAM disk just formatted and still mounted (opxy_disk_ready()): the
 * report in place of the test OS's flash.bin and testos.txt. */
void opxy_testos_check_disk(struct opxy_master *m)
{
	if (!write_file(m))
		s14_log_str(&m->sv.log, S14_WARNING, S14_TAG("check"), "check.txt not written");
}

/* Written anew, as opxy_system_info() writes its files: withdrawn, written,
 * offered again (S-12.018, S-10a.049). */
static void report(struct opxy_master *m)
{
	const bool offered = opxy_usb_up() && s10a_usb_disk()->offered;

	render();
	opxy_installer_table(&ck.t, ck.report, ck.report_len);
	if (!m->sv.disk_formatted) {
		if (opxy_disk_ready(m) && opxy_usb_up())
			s10a_usb_disk_offer();
		return;
	}
	if (offered && !opxy_disk_take(m, S10A_WITHDRAW_OWN_WRITE)) {
		s14_log_str(&m->sv.log, S14_WARNING, S14_TAG("check"), "check.txt: disk busy");
	} else if (wire_fat_mount(&m->sv.fat, &m->sv.disk.dev)) {
		opxy_testos_check_disk(m);
		wire_fat_unmount(&m->sv.fat);
	}
	if (opxy_usb_up())
		s10a_usb_disk_offer();
}

/* --- the automatic tests --- */

static void mark(enum tchk_id id)
{
	ck.f.ran |= 1u << id;
}

static bool two_cpus(const struct opxy_master *m)
{
	return wb_has(opxy_dev(m), WB_F_SECOND_CPU);
}

/* The eMMC in reach: over the bridge to the slave, or the main CPU's own
 * storage up (S-13b.060). */
static bool slave_up(const struct opxy_master *m)
{
	return m->slave_present && !m->bridge.absent && (two_cpus(m) || m->st.emmc_up);
}

static void t_switch_back(struct opxy_master *m)
{
	progress(m, TCHK_OS_PARTITION);
	mark(TCHK_OS_PARTITION);
	if (!slave_up(m)) {
		ck.f.sw.result = TCHK_SW_NO_SLAVE;
		return;
	}
	/* a look alone: an OS found there is made active when the installer
	 * ends; the survey, which writes nothing else, makes it active now */
	if (OPXY_TESTOS_SURVEY_ON)
		tchk_switch_back(&ck.env, &ck.f.sw);
	else
		tchk_switch_look(&ck.env, &ck.f.sw);
}

static void t_registers(struct opxy_master *m)
{
	const uint32_t *regs;
	unsigned n;
	bool replied;
	uint32_t te, fr, mask;

	progress(m, TCHK_CLOCKS);
	opxy_testos_check_kept(&regs, &n, &replied, &te, &fr);
	tchk_regs_judge(opxy_testos_regs, n, regs, ck.f.regs);
	/* S-13a.021, again: the words are put back */
	mask = wb_core_cli();
	ck.f.ddr_aliased = wb_dmc_aliases(OPXY_DDR_BASE, OPXY_DDR_ALIAS);
	wb_core_sti(mask);
	mark(TCHK_CLOCKS);
	mark(TCHK_DDR);
	mark(TCHK_CORE_STATE);
}

/*
 * S-13b.013: RDID as the driver frames every other command (spiflash.c's
 * frame()): the opcode, then the reply, both full-duplex transfers. A counted
 * send and receive is a pair nothing else uses on this controller; after its
 * counted receive one more word can reach the receive FIFO, and every
 * full-duplex read that follows then returns one byte late.
 */
static bool read_id(const struct wb_spi *spi, uint8_t id[3])
{
	static const uint8_t rdid = WB_SPIFLASH_RDID;
	bool ok;

	wb_spi_select(spi, true);
	ok = wb_spi_xfer(spi, &rdid, NULL, 1u) == WB_SPI_OK &&
	     wb_spi_xfer(spi, NULL, id, 3u) == WB_SPI_OK;
	wb_spi_select(spi, false);
	return ok;
}

/*
 * The flash as the slot checks read it: each read, then a pass of held
 * mode's services. A slot read whole is seconds at this rate; the links are
 * answered between the reads, not held up for all of it (the Nordic's and
 * the PSoC's messages included).
 */
static struct opxy_master *flash_m;
static const struct s02_flash_ops *flash_inner;

static int flash_served(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	const int rc = flash_inner->read(ctx, off, buf, len);

	pass(flash_m);
	return rc;
}

static void t_flash(struct opxy_master *m)
{
	const struct s02_flash_ops served = {
		m->flash_ops.ctx, flash_served, m->flash_ops.erase, m->flash_ops.program,
	};
	struct s02_env env = m->flash;
	const struct wb_spi *spi = m->hw.flash.spi;
	struct s02_selector sel;
	uint8_t magic[5];

	progress(m, TCHK_FLASH_ID);
	/* S-13b.015: the stage's first frames are the read's RDSR until WIP = 0;
	 * then RDID, informative (S-13b.018) */
	ck.f.flash_id_read = m->flash_ops.read(m->flash_ops.ctx, S02_META_OFF, magic, 1u) == 0 &&
			     read_id(spi, ck.f.jedec);
	/* the status register, read only: its block-protect bits, under which a
	 * program or erase is ignored with no error */
	ck.f.flash_sr_read = wb_spiflash_status(&m->hw.flash, &ck.f.flash_sr) == WB_SPIFLASH_OK;
	mark(TCHK_FLASH_ID);

	progress(m, TCHK_FLASH_LAYOUT);
	flash_m = m;
	flash_inner = &m->flash_ops;
	env.flash = &served;
	ck.f.flash_read = m->flash_ops.read(m->flash_ops.ctx, S02_META_OFF, magic, 5u) == 0 &&
			  s02_read_selector(&env, &sel) == 0;
	if (ck.f.flash_read) {
		ck.f.meta_magic_ok = wb_get_le32(magic) == S02_META_MAGIC;
		/* the magic one byte on: reads come back one byte late */
		ck.f.flash_late = !ck.f.meta_magic_ok && wb_get_le32(magic + 1) == S02_META_MAGIC;
		ck.f.sel_index = sel.index;
		ck.f.sel_verified = sel.verified;
		const uint64_t t0 = now_us();

		for (unsigned slot = 0; slot < 2u; slot++) {
			struct s02_record rec;

			pass(m);
			if (s02_read_record(&env, slot, &rec) == 0) {
				ck.f.slot_crc[slot] = rec.crc32;
				ck.f.slot_len[slot] = rec.length;
				if (s02_record_plausible(&rec))
					ck.f.flash_bytes += rec.length;
			}
			if (s02_check_slot(&env, slot, &ck.f.slot_good[slot]) != 0)
				ck.f.slot_good[slot] = false;
		}
		ck.f.flash_us = now_us() - t0;
		tchk_first_stage(flash_served, m->flash_ops.ctx, &ck.f.fs);
	}
	mark(TCHK_FLASH_LAYOUT);
}

static void t_psoc(struct opxy_master *m)
{
	struct s06_frame r;
	int status = 0;
	int len;

	progress(m, TCHK_PSOC);
	ck.f.psoc_greeted = m->psoc.greeted;
	if (!ck.f.psoc_greeted) {
		bool g = false;

		/* S-07.020: a greeting of the test's own; S-07.025's subscription
		 * after it, as after any greeting held mode answers */
		if (s07_greet(&m->psoc, &g) == S07_OK && g) {
			ck.f.psoc_asked = true;
			s07_subscribe(&m->psoc);
		}
	}
	/* the ping 0x01 is pmic-pass-through's: none on a device without it */
	ck.f.psoc_ping = wb_has(opxy_dev(m), WB_F_PMIC_PASS_THROUGH) &&
			 s06_transact(&m->node, S06_LINK_PSOC, PSOC_PING, NULL, 0, REQUEST_US,
				      &r) == 1;
	ck.f.psoc_keys = s06_transact(&m->node, S06_LINK_PSOC, PSOC_KEY_STATUS, NULL, 0,
				      REQUEST_US, &r) == 1;
	ck.f.psoc_battery = s06_transact(&m->node, S06_LINK_PSOC, PSOC_BATTERY_QUERY, NULL, 0,
					 REQUEST_US, &r) == 1;
	len = s07_info_request(&m->psoc, ck.f.psoc_info, sizeof ck.f.psoc_info, &status);
	ck.f.psoc_info_len = len > 0 ? (uint16_t)len : 0u;
	mark(TCHK_PSOC);
}

static void t_slave(struct opxy_master *m)
{
	unsigned i;

	ck.f.slave_failed_row = -1;
	for (i = 0; i < OPXY_TESTOS_STEP_COUNT; i++)
		ck.f.slave_ms[i] = 0xFFFFu;
	if (!two_cpus(m))
		return;		/* not on this device */
	progress(m, TCHK_SLAVE);
	if (!slave_up(m))
		return;
	{
		/* S-09.015's echo */
		struct s09_req r = { S09_ECHO, 0, 0, 0x12345678u, 0 };
		uint32_t err = 0;
		const int32_t v = s09_master_call(&m->bridge, &r, NULL, 0, NULL, 0, NULL, &err);

		ck.f.slave_echo = err == 0u && (uint32_t)v == 0xEDCBA987u;
	}
	/* the slave's own start-up steps (waffle/testos.h) */
	for (i = 0; i < OPXY_TESTOS_STEP_REQUEST; i++) {
		struct s09_req r = { OPXY_TESTOS_ROW_REQ, 0, 0, i, 0 };
		uint32_t err = 0, code;
		const uint32_t v = (uint32_t)s09_master_call(&m->bridge, &r, NULL, 0, NULL, 0,
							     NULL, &err);

		if (err != 0u) {
			ck.f.slave_echo = false;
			break;
		}
		code = v >> 16 & 0xFFu;
		if (code != OPXY_TESTOS_ROW_NOT_RUN)
			ck.f.slave_ms[i] = (uint16_t)(v & 0xFFFFu);
		if (code != 0u && code != OPXY_TESTOS_ROW_NOT_RUN && ck.f.slave_failed_row < 0) {
			ck.f.slave_failed_row = (int)i;
			ck.f.slave_failed_code = (uint8_t)code;
		}
	}
	mark(TCHK_SLAVE);
}

/* S-09.071's link test at its largest payload, to 8 MiB, each reply's value
 * checked. */
static void t_bridge(struct opxy_master *m)
{
	uint32_t seed = 0x1234567u, i;

	if (!two_cpus(m))
		return;		/* not on this device */
	progress(m, TCHK_BRIDGE);
	if (!slave_up(m))
		return;
	while (ck.f.link_bytes < LINK_TOTAL && ck.f.link_err < 4u) {
		/* len is n mod 2^16 (S-09.071) */
		struct s09_req r = { S09_LINK_TEST, 0, (uint16_t)(S09_TEST_MAX & 0xFFFFu), 0,
				     S09_TEST_MAX };
		uint32_t err = 0, want;
		int32_t v;

		for (i = 0; i < S09_TEST_MAX; i++) {
			seed = seed * 1664525u + 1013904223u;
			link_pl[i] = (uint8_t)(seed >> 24);
		}
		want = s09_test_value(link_pl, S09_TEST_MAX);
		r.arg0 = seed;
		const uint64_t t = now_us();

		v = s09_master_call(&m->bridge, &r, link_pl, S09_TEST_MAX, NULL, 0, NULL, &err);
		ck.f.link_us += now_us() - t;
		if (err != 0u || m->bridge.absent) {
			ck.f.link_err++;
			if (m->bridge.absent)
				break;
		} else if ((uint32_t)v != want) {
			ck.f.link_bad++;
		}
		ck.f.link_bytes += S09_TEST_MAX;
		pass(m);
	}
	mark(TCHK_BRIDGE);
}

/* EXT_CSD as the slave read it at its bring-up, through the test OS's peek:
 * the same image on both CPUs, so the slave's copy is at the same address. */
static bool peek_byte(struct opxy_master *m, const void *at, uint8_t *out)
{
	const uint32_t a = (uint32_t)(uintptr_t)at;
	struct s09_req r = { OPXY_TESTOS_PEEK_REQ, 0, 0, a & ~3u, 0 };
	uint32_t err = 0;
	const uint32_t v = (uint32_t)s09_master_call(&m->bridge, &r, NULL, 0, NULL, 0, NULL, &err);

	*out = (uint8_t)(v >> (8u * (a & 3u)));
	return err == 0u && !m->bridge.absent;
}

/* On the OP-1F the bring-up's EXT_CSD is the main CPU's own (S-13b.060),
 * refreshed by S-03.012's reads. */
static bool ext_byte(struct opxy_master *m, unsigned i, uint8_t *out)
{
	if (!two_cpus(m)) {
		*out = m->st.ext_csd[i];
		return true;
	}
	return peek_byte(m, &opxy_slave.st.ext_csd[i], out);
}

static bool features_made(struct opxy_master *m, uint8_t *out)
{
	if (!two_cpus(m)) {
		*out = m->st.features ? 1u : 0u;
		return true;
	}
	return peek_byte(m, &opxy_slave.st.features, out);
}

static void t_ext_csd(struct opxy_master *m)
{
	uint8_t c[4], features = 0;
	unsigned i;
	bool ok = true;

	progress(m, TCHK_EMMC_CONFIG);
	if (!slave_up(m))
		return;
	for (i = 0; i < 4u; i++)
		ok = ext_byte(m, 212u + i, &c[i]) && ok;
	ck.f.sec_count = wb_get_le32(c);
	ok = ext_byte(m, 33u, &ck.f.ext33) && ok;
	ok = ext_byte(m, 34u, &ck.f.ext34) && ok;
	ok = ext_byte(m, 163u, &ck.f.ext163) && ok;
	ok = ext_byte(m, 179u, &ck.f.ext179) && ok;
	ok = ext_byte(m, 183u, &ck.f.ext183) && ok;
	ok = ext_byte(m, 226u, &ck.f.ext226) && ok;
	ok = ext_byte(m, 265u, &ck.f.ext265) && ok;
	/* S-03.009's switches came after that read: the record of them */
	ok = features_made(m, &features) && ok;
	ck.f.features = features != 0u;
	{
		struct s09_req r = { S09_ERASE_GROUP, 0, 0, 0, 0 };
		uint32_t err = 0;

		ck.f.erase_group = (uint32_t)s09_master_call(&m->bridge, &r, NULL, 0, NULL, 0, NULL,
							     &err);
		ok = ok && err == 0u;
	}
	ck.f.ext_read = ok;
	mark(TCHK_EMMC_CONFIG);
}

/* A store's note, on this CPU or peeked word by word from the slave's. */
static bool note_of(struct opxy_master *m, const struct opxy_emmc_note *mine,
		    const struct opxy_emmc_note *slave, struct opxy_emmc_note *out)
{
	uint32_t w[(sizeof(*out) + 3u) / 4u];
	unsigned i;

	memset(out, 0, sizeof(*out));
	if (!two_cpus(m)) {
		*out = *mine;
		return out->seen != 0u;
	}
	if (!peek_byte(m, &slave->seen, &out->seen) || out->seen == 0u)
		return false;
	for (i = 0; i < sizeof(w) / 4u; i++) {
		const uint32_t a = (uint32_t)(uintptr_t)slave + 4u * i;
		struct s09_req r = { OPXY_TESTOS_PEEK_REQ, 0, 0, a, 0 };
		uint32_t err = 0;

		w[i] = (uint32_t)s09_master_call(&m->bridge, &r, NULL, 0, NULL, 0, NULL, &err);
		if (err != 0u || m->bridge.absent)
			return false;
	}
	memcpy(out, w, sizeof(*out));
	return out->seen != 0u;
}

/* emmc-boot's fault lines: the first failing select and read (the store's). */
static void fault_facts(struct opxy_master *m)
{
	const struct opxy_emmc_note *mine[2] = { &m->st.note_select, &m->st.note_read };
	const struct opxy_emmc_note *slave[2] = { &opxy_slave.st.note_select,
						  &opxy_slave.st.note_read };
	unsigned k;

	for (k = 0; k < 2u; k++) {
		struct tchk_emmc_fault *e = &ck.f.emmc_fault[k];
		struct opxy_emmc_note n;

		memset(e, 0, sizeof(*e));
		if (!note_of(m, mine[k], slave[k], &n))
			continue;
		e->seen = true;
		e->part = n.part;
		e->why = n.why;
		e->pc_flags = n.pc_flags;
		e->pc_before = n.pc_before;
		e->pc_after = n.pc_after;
		e->sector = n.sector;
		e->count = n.count;
		e->step = n.f.step;
		e->cmd = n.f.cmd;
		e->at = n.f.at;
		e->state = n.f.state;
		e->rc = n.f.rc;
		e->arg = n.f.arg;
		e->r1 = n.f.r1;
		e->istat = n.f.istat;
		e->idsts = n.f.idsts;
		e->busy_us = n.f.busy_us;
	}
}

_Static_assert((int)TCHK_FW_DRIVER == (int)OPXY_NOTE_DRIVER &&
	       (int)TCHK_FW_DOWN == (int)OPXY_NOTE_DOWN &&
	       (int)TCHK_FW_UNKNOWN == (int)OPXY_NOTE_UNKNOWN &&
	       (int)TCHK_FW_REFUSED == (int)OPXY_NOTE_REFUSED, "the note's words are the store's");
_Static_assert((int)TCHK_FS_COUNT == (int)WB_MSI_STEP_COUNT &&
	       (int)TCHK_FS_SWITCH == (int)WB_MSI_STEP_SWITCH &&
	       (int)TCHK_FS_DATA_TIME == (int)WB_MSI_STEP_DATA_TIME, "the steps are the driver's");
_Static_assert(TCHK_PC_BEFORE == OPXY_NOTE_PC_BEFORE && TCHK_PC_AFTER == OPXY_NOTE_PC_AFTER,
	       "the flags are the store's");

static void t_emmc(struct opxy_master *m)
{
	progress(m, TCHK_EMMC_BOOT);
	if (!slave_up(m))
		return;
	tchk_emmc_boot(&ck.env, &ck.f.sw, now_us, &ck.f.boot);
	fault_facts(m);
	mark(TCHK_EMMC_BOOT);

	progress(m, TCHK_EMMC_VOLUME);
	ck.f.mounted = m->sv.files.mount_sent && !m->sv.files.mount_failed &&
		       !m->sv.files.mount_lost;
	ck.f.space_read = ck.f.mounted && s09_files_free_space(&m->sv.files, &ck.f.free_bytes);
	mark(TCHK_EMMC_VOLUME);
}

/* The file layer over S-09's file requests (S-09.041). */
static int32_t vol_open(void *ctx, const char *path, uint32_t flags)
{
	return s09_files_open(ctx, path, flags);
}

static int32_t vol_write(void *ctx, int32_t fd, const uint8_t *p, uint32_t n)
{
	return s09_files_write(ctx, fd, p, n);
}

static int32_t vol_read(void *ctx, int32_t fd, uint8_t *p, uint32_t n)
{
	return s09_files_read(ctx, fd, p, n);
}

static int32_t vol_close(void *ctx, int32_t fd)
{
	return s09_files_close_sync(ctx, fd);
}

static int32_t vol_unlink(void *ctx, const char *path)
{
	return s09_files_unlink(ctx, path);
}

static int32_t vol_exists(void *ctx, const char *path)
{
	struct s09_files *f = ctx;
	struct s09_stat st;

	if (s09_files_stat(f, path, &st) == 0)
		return 1;
	return f->err == S09_ENOENT ? 0 : -1;
}

static void t_file(struct opxy_master *m)
{
	const struct tchk_file_ops ops = { &m->sv.files, vol_open, vol_write, vol_read, vol_close,
					   vol_unlink, vol_exists };

	progress(m, TCHK_EMMC_FILE);
	if (!slave_up(m) || !ck.f.mounted)
		return;
	tchk_file_test(&ops, scratch, &ck.f.file);
	mark(TCHK_EMMC_FILE);
}

/* Five blocks side by side in the window, red, green, blue, white, gray:
 * in color when the rest is gray, since they check that the panel shows
 * colors. */
static void bars(struct opxy_master *m, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	static const uint16_t c[5] = { 0xF800u, 0x07E0u, 0x001Fu, 0xFFFFu, 0x8410u };
	unsigned i;

	for (i = 0; i < 5u; i++) {
		const uint16_t x0 = (uint16_t)(x + w * i / 5u);
		const uint16_t x1 = (uint16_t)(x + w * (i + 1u) / 5u);

		m->sv.draw.fill(m->sv.draw.ctx, x0, y, (uint16_t)(x1 - x0), h, c[i]);
	}
}

#define BARS_W 90u
#define BARS_H 10u

static void t_panel(struct opxy_master *m)
{
	struct wb_display *d = opxy_testos_display();
	const struct s15_geometry *g = opxy_display_geometry();
	const uint32_t *regs;
	unsigned n, i;
	bool replied;
	uint32_t te0, fr0, te1, fr1;

	progress(m, TCHK_PANEL);
	mark(TCHK_PANEL);
	/* cppcheck-suppress knownConditionTrueFalse ; a weak default */
	ck.f.panel_up = d != NULL && g != NULL && d->panel.state == S13C_ON;
	if (!ck.f.panel_up)
		return;
	/* S-13c.011: the identification reads the start made, if it made them */
	ck.f.panel_id = d->panel.id_valid;
	memcpy(ck.f.rddid, d->panel.rddid, 3);
	memcpy(ck.f.rdid4, d->panel.rdid4, 3);
	opxy_testos_check_kept(&regs, &n, &replied, &te0, &fr0);
	/* three whole frames of the pattern, each waited for */
	for (i = 0; i < 3u; i++) {
		/* a strip in the body's last row, empty while the checks run */
		bars(m, g->margin, s15_row_y(g, S15_ROWS - 1u), BARS_W, BARS_H);
		opxy_display_settle();
		pass(m);
	}
	opxy_testos_check_kept(&regs, &n, &replied, &te1, &fr1);
	ck.f.te_misses = te1 - te0;
	ck.f.frame_timeouts = fr1 - fr0;
	ck.f.pattern_drawn = true;
	/* S-13c.018: the time between two rising edges of TE */
	if (wb_display_wait(d) == 0 && d->panel.bus->te_wait(d->panel.bus->ctx, TE_WAIT_US) == 0) {
		const uint64_t t0 = now_us();

		if (d->panel.bus->te_wait(d->panel.bus->ctx, TE_WAIT_US) == 0) {
			ck.f.te_seen = true;
			ck.f.te_us = (uint32_t)(now_us() - t0);
		}
	}
}

/* RTC0: one change of the seconds, then the time to the next. */
static bool rtc_change(struct opxy_master *m, uint32_t *clk)
{
	const uint64_t end = wb_time_deadline(&m->hw.time, RTC_WAIT_US);
	uint32_t c;

	do {
		if (!opxy_rtc_read(&c))
			return false;
		if (c != *clk) {
			*clk = c;
			return true;
		}
		pass(m);
	} while (!wb_time_passed(end));
	return false;
}

static void t_rtc(struct opxy_master *m)
{
	uint32_t clk;
	uint64_t t0;

	progress(m, TCHK_RTC);
	mark(TCHK_RTC);
	ck.f.rtc_read = opxy_rtc_read(&clk);
	if (!ck.f.rtc_read || !rtc_change(m, &clk))
		return;
	t0 = now_us();
	ck.f.rtc_ticked = rtc_change(m, &clk);
	ck.f.rtc_us = (uint32_t)(now_us() - t0);
}

/* The Nordic's mode before anything long runs (S-08.021): whether a
 * bootloader the nordic test finds was there from the start. */
static void nordic_mode_at_start(struct opxy_master *m)
{
	uint8_t reply[TCHK_INFO_MAX];
	const long len = s08_info(&m->nordic, reply, sizeof reply);

	if (len >= 0)
		ck.f.nordic_start_mode = s17_reply_in_bootloader(reply, (size_t)len) ? 2u : 1u;
}

static void t_nordic(struct opxy_master *m)
{
	static const uint8_t hello[2] = { 0x01, 0x03 };
	uint64_t end;
	unsigned i;
	long len;

	progress(m, TCHK_NORDIC);
	/* S-08.002's greeting request, read-only, once more; any reply ends it */
	for (i = 0; i < NORDIC_TRIES && !ck.f.nordic_greeting; i++)
		ck.f.nordic_greeting = s06_transact(&m->node, S06_LINK_NORDIC, NORDIC_GREETING,
						    hello, sizeof hello, 5000u, NULL) == 1;
	len = s08_info(&m->nordic, ck.f.nordic_reply, sizeof ck.f.nordic_reply);
	/* still in its bootloader: its greetings are being answered "leave";
	 * asked again until its application answers, within a bound */
	end = wb_time_deadline(&m->hw.time, NORDIC_LEAVE_US);
	while (len >= 0 && s17_reply_in_bootloader(ck.f.nordic_reply, (size_t)len) &&
	       !wb_time_passed(end)) {
		const uint64_t next = wb_time_deadline(&m->hw.time, NORDIC_ASK_US);

		while (!wb_time_passed(next))
			pass(m);
		len = s08_info(&m->nordic, ck.f.nordic_reply, sizeof ck.f.nordic_reply);
	}
	ck.f.nordic_info = len >= 0;
	ck.f.nordic_reply_len = len > 0 ? (uint16_t)len : 0u;
	mark(TCHK_NORDIC);
}

/* --- the guided tests --- */

/*
 * The one guided test: com and the recovery key, in either order
 * (device.key.com and device.key.recovery; on the OP-XY com and M4). A
 * person who can read this screen has seen the panel work. Where an OS was
 * found to make active again, 8 held returns to it, and the install is
 * then not offered.
 */
static void key_line(struct opxy_master *m, unsigned last, const struct inst_hold *h)
{
	const uint32_t now = (uint32_t)(now_us() / 1000u);
	const uint32_t held = h->holding ? now - h->t0 : 0u;
	struct inst_screen sc;
	struct tchk_line l[S15_ROWS];
	unsigned n, i;

	memset(&sc, 0, sizeof sc);
	sc.phase = INST_GUIDED;
	sc.os = (uint8_t)inst_os_state(&ck.f.sw);
	sc.keys_seen = ck.f.keys_seen;
	sc.device = (uint8_t)opxy_dev(m)->id;
	sc.last_key = (uint8_t)last;
	sc.hold_digit = h->holding ? h->digit : 0u;
	sc.hold_fill = inst_hold_fill(h, now);
	sc.hold_left_s = h->holding ?
		(S15_HOLD_MS - (held < S15_HOLD_MS ? held : S15_HOLD_MS) + 999u) / 1000u : 0u;
	opxy_installer_note_hold(sc.os, sc.hold_digit, sc.hold_fill);
	n = inst_screen_lines(&sc, l);
	s15_console_clear(&ck.con);
	for (i = 0; i < n; i++)
		s15_console_add(&ck.con, (enum s15_style)l[i].style, l[i].s);
	show(m);
}

/* True when 8 was held: return to the old OS. */
static bool g_keys(struct opxy_master *m)
{
	const struct wb_device *dev = opxy_dev(m);
	const bool os_ok = ck.f.sw.result == TCHK_SW_READY;
	struct inst_hold h;
	unsigned key, last = 0xFFu;
	uint32_t drawn = 0;
	bool down;

	memset(&h, 0, sizeof h);
	h.dev = dev;
	inst_hold_offer(&h, inst_holds_for(INST_GUIDED, os_ok));
	opxy_installer_note(INST_GUIDED, TCHK_COUNT);
	key_line(m, last, &h);
	while (ck.f.keys_seen != 3u) {
		const bool was = h.holding;
		uint32_t now;
		bool changed = false;

		pass(m);
		while (next_key(&key, &down)) {
			inst_hold_key(&h, key, s15_digit(dev, key), down, (uint32_t)(now_us() / 1000u));
			if (!down)
				continue;
			if (key == tchk_key_com(dev))
				ck.f.keys_seen |= 1u;
			else if (key == tchk_key_recovery(dev))
				ck.f.keys_seen |= 2u;
			else
				last = key;
			changed = true;
		}
		now = (uint32_t)(now_us() / 1000u);
		if (inst_act_of(INST_GUIDED, inst_hold_tick(&h, now)) == INST_ACT_LEAVE) {
			ck.f.keys = TCHK_G_ENDED;
			return true;
		}
		if (changed || was != h.holding || (h.holding && now - drawn >= 100u)) {
			key_line(m, last, &h);
			drawn = now;
		}
	}
	ck.f.keys = TCHK_G_PASSED;
	return false;
}

/* PA2 as the survey reports it (tchk_facts.pa2): the strap where the
 * device has straps; else its level where the device's own pins have
 * made it an input (S-13b.006), so that nothing is driven to read it. */
static uint8_t pa2_level(const struct opxy_master *m)
{
	const struct wb_device *dev = opxy_dev(m);
	unsigned i;

	if (wb_has(dev, WB_F_STRAPS))
		return (m->strap & OPXY_STRAP_ROLE) != 0u ? 1u : 0u;
	for (i = 0; i < dev->pins.gpio_n; i++)
		if (dev->pins.gpio[i].pin == WB_DEV_PA(2u) && dev->pins.gpio[i].kind == WB_GPIO_IN)
			return wb_gpio_get(WB_PIN(WB_PORT_A, 2)) ? 1u : 0u;
	return 0xFFu;
}

/* --- the entry points --- */

static void version(void)
{
	struct tchk_text t;

	tchk_text_init(&t, sizeof ck.version - 1u);
	tchk_put_dec(&t, OPXY_BL_VERSION_MAJOR);
	tchk_put_str(&t, ".");
	tchk_put_dec(&t, OPXY_BL_VERSION_MINOR);
	tchk_put_str(&t, ".");
	tchk_put_dec(&t, OPXY_BL_VERSION_PATCH);
	memcpy(ck.version, t.s, (size_t)t.n + 1u);
}

/* Before held mode's entry steps: the Nordic link's gate, the keys, USB. */
void opxy_testos_check_start(struct opxy_master *m)
{
	ck.f.dev = opxy_dev(m);
	ck.f.held_ms = (uint32_t)wb_time_ms(&m->hw.time);
	version();
	/* The color setting as held mode's services read it: the test OS keeps
	 * no settings, so the device's default (S-15.173), gray on the OP-XY */
	ck.gray = !m->sv.settings.color;
	/* S-08.011 in state 0: a bootloader's greeting is told to start the
	 * application, any other answered 0x06 {40}; the gate holds back every
	 * other frame, every other 0x0D form above all */
	nordic_port = m->link[S06_LINK_NORDIC].ops;
	m->link[S06_LINK_NORDIC].ops = &gate_port;
	s08_set_state(&m->nordic, TCHK_NORDIC_STATE);
	m->psoc_ops.key_event = key_event;
	ck.f.psoc_start_greeted = m->psoc.greeted;
	psoc_log_next = m->psoc_ops.log;
	m->psoc_ops.log = psoc_log;
	/* the carried image checked; the serial port's bytes to the installer */
	opxy_installer_start(m);
	/* S-10a.010's disk set with the serial port, with no serial number:
	 * the RAM disk and the installer's protocol; the medium follows with
	 * check.txt on it (S-10a.049) */
	if (opxy_usb_up()) {
		if (wb_usb_attached())
			wb_usb_detach();
		s10a_usb_select(S10A_SET_DISK_CDC, "", &m->sv.disk.dev);
		(void)wb_usb_attach();
	}
	/* the eMMC through the guard, with the suite's own scratch */
	tchk_guard_init(&ck.guard, &m->emmc_ops);
	ck.guard.breathe = breathe;
	ck.guard.breathe_ctx = m;
	ck.env = m->emmc;
	ck.env.ops = &ck.guard.ops;
	ck.env.scratch = scratch;
	ck.env.scratch_len = sizeof scratch;
	ck.env.max_xfer = S09_MAX_SECTORS;
	s15_console_init(&ck.con, &m->sv.log);
}

/* Held mode's loop in this variant: boot_held_enter() hands over here. */
void opxy_testos_check_service(void *ctx, const struct boot_held *h)
{
	struct opxy_master *m = ctx;
	bool returned = false;

	(void)h;
	ck.f.slave = slave_up(m);
	ck.f.pa2 = pa2_level(m);
	opxy_display_held_start(m);
	nordic_mode_at_start(m);
	/* the OS partition first, as soon as the eMMC answers */
	t_switch_back(m);
	t_registers(m);
	t_flash(m);
	t_psoc(m);
	t_slave(m);
	t_bridge(m);
	t_ext_csd(m);
	t_emmc(m);
	t_file(m);
	t_panel(m);
	t_rtc(m);
	t_nordic(m);
	mark(TCHK_TIMING);
	report(m);

	/* back after a trial start: the keys passed at the start that wrote it */
	if (opxy_installer_trial_back(m))
		ck.f.keys = TCHK_G_EARLIER;
	else
		returned = g_keys(m);
	report(m);

	/* the install, offered only when every check passed; after 8 held in
	 * the key check, the OS made active again and nothing offered */
	{
		const struct opxy_installer_ctx ic = {
			m, &ck.t, &ck.f.sw, &ck.env, &ck.con, pass, next_key, show, report, &ck.f,
			returned,
		};

		opxy_installer_run(&ic);
	}
}

#else

/* ISO C wants a declaration in every translation unit. */
typedef int opxy_testos_check_not_built;

#endif /* OPXY_TESTOS_CHECK */
