/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10a.049 on the master (src/board/opxy_services.c, opxy_usb.c): held
 * mode's file operations run under the host hold, and a disk offered when
 * one starts is withdrawn for it (an own write's withdrawal: no release)
 * and offered again after it, with the medium change.
 */
#include <stdlib.h>
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/s10a_msc.h"
#include "waffle/s10a_usb.h"

static struct opxy_master m;

/* While each log line is written to the PSoC: the hold and the disk. */
static unsigned lines, lines_held, lines_offered;

static size_t port_read(void *ctx, uint8_t *buf, size_t max)
{
	(void)ctx;
	(void)buf;
	(void)max;
	return 0;
}

static size_t port_room(void *ctx)
{
	(void)ctx;
	return 4096u;
}

static void port_write(void *ctx, const uint8_t *data, size_t n)
{
	(void)ctx;
	(void)data;
	(void)n;
	lines++;
	if (opxy_host_held())
		lines_held++;
	if (s10a_usb_disk()->offered)
		lines_offered++;
}

static const struct s06_port_ops port = { port_read, port_room, port_write };

static uint8_t crc8(const uint8_t *p, size_t n)
{
	uint8_t c = 0;

	while (n--)
		c = (uint8_t)(c ^ *p++);
	return c;
}

/* The RAM disk in host memory, in two parts. */
static uint8_t *disk_mem;

static void setup(void)
{
	struct opxy_disk_part p[OPXY_DISK_PARTS];

	opxy_usb_stop();
	memset(&m, 0, sizeof(m));
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
	opxy_startup_read(&m, opxy_otp_get);
	opxy_log_start(&m);
	s10c_host_init(&m.sv.host, &m.sv.host_ops, &m);
	opxy_usb_start(&m);
	if (disk_mem == NULL)
		disk_mem = calloc(OPXY_DISK_SECTORS, 512u);
	if (disk_mem == NULL)
		exit(2);
	memset(disk_mem, 0, (size_t)OPXY_DISK_SECTORS * 512u);
	p[0].mem = disk_mem;
	p[0].sectors = OPXY_DISK_SECTORS / 2u;
	p[1].mem = disk_mem + (size_t)(OPXY_DISK_SECTORS / 2u) * 512u;
	p[1].sectors = OPXY_DISK_SECTORS - OPXY_DISK_SECTORS / 2u;
	opxy_disk_init(&m.sv.disk, p);
}

/* After an operation: offered again, one more withdrawal, no release, a
 * unit attention owed (the medium change), the hold let go; its log line
 * was written under the hold with the disk withdrawn. */
static void after(const char *what, uint32_t *withdrawals)
{
	const struct s10a_lu *lu = s10a_usb_disk();
	const unsigned before = check_failures;

	*withdrawals += 1u;
	CHECK(lu->offered && lu->ua);
	CHECK(lu->withdrawals == *withdrawals);
	CHECK(lu->releases == 0u);
	CHECK(!opxy_host_held());
	CHECK(lines != 0u && lines_held == lines && lines_offered == 0u);
	lines = lines_held = lines_offered = 0;
	s15_menu_release(&m.sv.menu);
	if (check_failures != before)
		printf("  %s\n", what);
}

static void test_file_operations(void)
{
	struct s15_env_ops e;
	struct s15_osw_end end = { S15_OSW_FAILED, S15_OSW_WHY_NONE,
				   S15_OSW_EMMC_UNTOUCHED, 0, 0, 0, 0, false };
	const struct s15_settings st = { false, false, false };
	uint32_t w;

	setup();
	CHECK(opxy_usb_up());
	opxy_menu_env(&m, &e);
	opxy_usb_select(&m, S10A_STATE_UPGRADE);
	CHECK(s10a_usb_disk()->offered);
	w = s10a_usb_disk()->withdrawals;
	lines = lines_held = lines_offered = 0;

	/* each fails at once with no slave, and logs that it did */
	CHECK(!e.format(e.ctx));
	after("format", &w);
	e.erase_os_fat(e.ctx);
	after("erase", &w);

	/* these log nothing: the hold and the disk afterwards only */
	e.os_write(e.ctx, &end);
	CHECK(end.result != S15_OSW_OK);
	CHECK(!e.remove_tree(e.ctx, "upgrade"));
	CHECK(!e.settings_save(e.ctx, &st));
	CHECK(s10a_usb_disk()->offered && !opxy_host_held());
	CHECK(s10a_usb_disk()->withdrawals == w + 3u);
	s15_menu_release(&m.sv.menu);

	/* on a set with no disk nothing is withdrawn or offered */
	opxy_usb_select(&m, S10A_STATE_MAIN);
	w = s10a_usb_disk()->withdrawals;
	CHECK(!e.format(e.ctx));
	CHECK(s10a_usb_disk()->withdrawals == w && !s10a_usb_disk()->offered);
	CHECK(!opxy_host_held());
	s15_menu_release(&m.sv.menu);
	opxy_usb_stop();
}

/* The hold nests: only the outermost end lets the loop's pass run again.
 * The host links are held throughout, the hold or not: only the pass reads
 * them. */
static void test_nesting(void)
{
	setup();
	CHECK(m.link[S06_LINK_CDC].held && m.link[S06_LINK_MIDI].held);
	CHECK(m.link[S06_LINK_CDC].paced && m.link[S06_LINK_MIDI].paced);
	opxy_host_hold_begin(&m);
	opxy_host_hold_begin(&m);
	opxy_host_hold_end(&m);
	CHECK(opxy_host_held());
	opxy_host_hold_end(&m);
	CHECK(!opxy_host_held());
	opxy_host_hold_end(&m);			/* one too many: no effect */
	CHECK(!opxy_host_held());
	opxy_usb_poll(&m);
	CHECK(m.link[S06_LINK_CDC].held && m.link[S06_LINK_MIDI].held);
	opxy_usb_stop();
}

int main(void)
{
	RUN(test_file_operations);
	RUN(test_nesting);
	free(disk_mem);
	return check_summary("test_board_hold");
}
