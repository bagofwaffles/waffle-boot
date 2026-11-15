/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The install steps' read-back check (src/board/opxy_install.c,
 * S-11.095) on the host: the master's step ops over an S-09 loopback to
 * s09_slave serving wire_s09 on a yaffs2 volume in RAM, with the SPI flash
 * in RAM. The loopback can damage one byte of a chosen read reply, the
 * failure mode of S-09.010 (a reply the master receives wrong, which the
 * bridge does not detect).
 *
 * Covered (S-11.095): the image read back must have the member's stated
 * size and CRC-32 (S-11.030). A damaged read is refused with Bad file
 * (S-11.058, S-11.068) and nothing reaches the S-02 flash; a read that
 * fails (the slave gone mid-read, S-09.013) is Internal error; an intact
 * one is written by S-02.042-S-02.053 with S-02.047's read-back, from the
 * bytes that were checked with no further read. The OS and first-stage steps
 * (S-11.067, S-11.072) take the same check.
 */
#include <stdlib.h>
#include <string.h>

#include "s06_test.h"
#include "s06_fake.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_bridge.h"
#include "waffle/s06_frame.h"
#include "waffle/s09_slave.h"
#include "waffle/s11_run.h"
#include "waffle/wire_crc.h"
#include "waffle/wire_package.h"
#include "waffle/wire_s09.h"
#include "waffle/wire_yaffs.h"
#include "vectors/s02_vectors.h"
#include "s11_ref.h"

#define BLOCKS 40u			/* one yaffs2 block is one erase group */
#define VOL_LEN ((size_t)BLOCKS * 128u * 4096u)

#define BOOTLOADER "upgrade/firmware/blackfin/bin/bootloader.ldr"
#define OS "upgrade/firmware/blackfin/bin/os.ldr"
#define SPIBOOT "upgrade/firmware/blackfin/bin/spiboot.ldr"

static struct opxy_master m;

/* ---- the PSoC port: everything written, nothing received ---- */

static uint8_t wire[1u << 18];
static size_t wire_n;

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
	return sizeof(wire) - wire_n;
}

static void port_write(void *ctx, const uint8_t *data, size_t n)
{
	(void)ctx;
	if (wire_n + n > sizeof(wire))
		wire_n = 0;
	memcpy(wire + wire_n, data, n);
	wire_n += n;
}

static const struct s06_port_ops port = { port_read, port_room, port_write };

/* The CDC link's port, as a host with the port open would take its bytes. */
static uint8_t cdc_wire[1u << 18];
static size_t cdc_n;

static size_t cdc_read(void *ctx, uint8_t *buf, size_t max)
{
	(void)ctx;
	(void)buf;
	(void)max;
	return 0;
}

static size_t cdc_room(void *ctx)
{
	(void)ctx;
	return sizeof(cdc_wire) - cdc_n;
}

static void cdc_write(void *ctx, const uint8_t *data, size_t n)
{
	(void)ctx;
	memcpy(cdc_wire + cdc_n, data, n);
	cdc_n += n;
}

static const struct s06_port_ops cdc_port = { cdc_read, cdc_room, cdc_write };

/* cppcheck-suppress constParameterCallback ; s10c_host_ops fixes the type */
static bool cdc_is_open(void *ctx)
{
	(void)ctx;
	return true;
}

static bool cdc_has(const char *text)
{
	const size_t n = strlen(text);

	for (size_t i = 0; i + n <= cdc_n; i++) {
		if (memcmp(cdc_wire + i, text, n) == 0)
			return true;
	}
	return false;
}

static uint8_t crc8(const uint8_t *p, size_t n)
{
	uint8_t c = 0;

	while (n--)
		c = (uint8_t)(c ^ *p++);
	return c;
}

/* ---- the slave's store: wire_s09 on a yaffs2 volume; no eMMC ---- */

static struct wb_blockdev bd;
static struct wb_ramdisk ram;
static struct wire_yaffs Y;
static struct wire_s09 S;
static uint8_t *vol;

static unsigned st_flushes;	/* S-09.040's 0x04 */

static int32_t st_emmc(void *c, uint8_t cmd, uint32_t a0, uint32_t a1,
		       uint8_t *data, uint32_t n)
{
	(void)c;
	(void)a0;
	(void)a1;
	(void)data;
	(void)n;
	if (cmd == S09_FLUSH) {
		st_flushes++;
		return 0;
	}
	return 1;
}

static uint32_t st_erase_group(void *c) { (void)c; return S03_GROUP_SECTORS; }
static uint32_t st_boot_size(void *c) { (void)c; return 0x40000u; }
static uint32_t st_os(void *c) { (void)c; return 1; }
static uint64_t st_cycles(void *c) { (void)c; return 0; }
static void st_adjust(void *c, int64_t d) { (void)c; (void)d; }

static int32_t st_file(void *c, const struct s09_req *r, const char *p1,
		       const char *p2, const uint8_t *d, uint32_t n, uint8_t *out,
		       uint32_t *err)
{
	(void)c;
	return wire_s09_file(&S, r, p1, p2, d, n, out, err);
}

static int32_t st_read(void *c, uint32_t fd, uint8_t *buf, uint32_t n, uint32_t *err)
{
	(void)c;
	return wire_s09_read(&S, fd, buf, n, err);
}

static const struct s09_store_ops store = {
	0, st_erase_group, st_boot_size, NULL /* dev_cmd */, st_emmc, st_file, st_read, st_os,
	st_cycles, st_adjust,
};

static struct s09_slave sl;

/* ---- the loopback bus (test_s09_files's), with one damaged reply ---- */

static struct t_sim sim;

static struct {
	bool sel, reply_session, done, dead;
	unsigned sessions, die_after;	/* die_after: slave dies after this session; 0 never */
	uint8_t req[70000];
	size_t req_n;
	uint8_t last_req;		/* the request the reply answers */
	const uint8_t *rep;
	size_t rep_n, rep_at;
	unsigned reads;			/* 0x2A request sessions so far */
	/* The reply to READ request number `corrupt_read` (1-based) has one
	 * byte of its data flipped on the way to the master; 0: none. */
	unsigned corrupt_read;
	unsigned corrupted;		/* bytes flipped */
	/* When the READ request numbered `fail_read` (1-based) is seen, the
	 * slave is made to die from the next session on (S-09.013): its
	 * reply never comes, the master's wait runs out, the operation fails
	 * with ENODEV and the slave is absent from then on (S-09.008).
	 * 0: never. */
	unsigned fail_read;
	/* every link test outside the plain mode arrives with a bit wrong */
	bool bad_tests;
} lb;

static bool lb_ready(void *c)
{
	(void)c;
	if (lb.dead)
		return false;
	return lb.sel ? !lb.done : true;
}

static bool lb_attention(void *c)
{
	(void)c;
	return !lb.dead && s09_slave_attention(&sl);
}

static void lb_select(void *c, bool on)
{
	(void)c;
	if (on && !lb.sel) {
		lb.sessions++;
		if (lb.die_after != 0 && lb.sessions > lb.die_after)
			lb.dead = true;
		lb.reply_session = sl.reply_len != 0;
		lb.done = false;
		lb.req_n = 0;
		lb.rep_at = 0;
		if (lb.reply_session && !lb.dead)
			(void)s09_slave_reply(&sl, &lb.rep, &lb.rep_n);
	} else if (!on && lb.sel && !lb.dead) {
		if (!lb.reply_session && lb.req_n >= 12) {
			size_t k = (size_t)s09_slave_rx_len(&sl, lb.req);

			/* the polls and pulls that fetch a reply (S-09.031,
			 * S-09.045) keep the request the reply answers */
			if (lb.req[0] != S09_PULL && lb.req[0] != S09_POLL)
				lb.last_req = lb.req[0];
			if (lb.req[0] == S09_READ &&
			    ++lb.reads == lb.fail_read)
				lb.die_after = lb.sessions; /* die next session */
			if (lb.bad_tests && lb.req[0] == S09_LINK_TEST &&
			    s09_slave_mode(&sl) != S09_MODE_PLAIN)
				lb.req[5] ^= 0x10u;

			s09_slave_request(&sl, lb.req, k < lb.req_n ? k : lb.req_n);
		}
	}
	lb.sel = on;
}

static bool lb_send(void *c, const uint8_t *d, size_t n)
{
	(void)c;
	if (lb.dead || lb.reply_session || lb.req_n + n > sizeof lb.req)
		return false;
	memcpy(lb.req + lb.req_n, d, n);
	lb.req_n += n;
	lb.done = true;
	return true;
}

static bool lb_recv(void *c, uint8_t *d, size_t n)
{
	(void)c;
	if (lb.dead || !lb.reply_session || lb.rep_at + n > lb.rep_n)
		return false;
	memcpy(d, lb.rep + lb.rep_at, n);
	/* the damage: a byte in the data half of the reply, past its header */
	if (lb.corrupt_read != 0 && lb.last_req == S09_READ &&
	    lb.reads == lb.corrupt_read && lb.rep_n > S09_HDR + 8u) {
		size_t at = S09_HDR + (lb.rep_n - S09_HDR) / 2u;

		if (at >= lb.rep_at && at < lb.rep_at + n) {
			d[at - lb.rep_at] ^= 0x10u;
			lb.corrupted++;
		}
	}
	lb.rep_at += n;
	lb.done = lb.rep_at == lb.rep_n;
	return true;
}

static void lb_configure(void *c, int phase)
{
	(void)c;
	(void)phase;
}

static const struct s09_bus_ops bus = {
	0, lb_ready, lb_attention, lb_select, lb_send, lb_recv, lb_configure, NULL,
};

/* ---- S-02's flash in RAM, counting every operation ---- */

static uint8_t flash_mem[S02_FLASH_SIZE];
static uint8_t flash_scratch[S02_SECTOR_SIZE];
static unsigned flash_ops_n;		/* reads, erases and programs */
static unsigned flash_writes;		/* erases and programs */
static unsigned flash_one_try;		/* operations made with one try */

/* S-13b.017: the driver's tries while the operation runs. */
static void fl_tries(void)
{
	if (m.hw.flash.tries != OPXY_FLASH_WRITER_TRIES)
		flash_one_try++;
}

static int fl_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	fl_tries();
	flash_ops_n++;
	memcpy(buf, flash_mem + off, len);
	return 0;
}

static int fl_erase(void *ctx, uint32_t off)
{
	(void)ctx;
	fl_tries();
	flash_ops_n++;
	flash_writes++;
	memset(flash_mem + off, 0xFF, S02_SECTOR_SIZE);
	return 0;
}

static int fl_program(void *ctx, uint32_t off, const uint8_t *buf,
		      uint32_t len)
{
	(void)ctx;
	fl_tries();
	flash_ops_n++;
	flash_writes++;
	for (uint32_t i = 0; i < len; i++)
		flash_mem[off + i] &= buf[i];
	return 0;
}

static const struct s02_flash_ops flash_ops = { NULL, fl_read, fl_erase,
						fl_program };

/* A slot of 0x100 bytes and its record, the CRC right. */
static void slot_put(unsigned k)
{
	struct s02_record r;
	uint8_t raw[S02_RECORD_SIZE];

	memset(flash_mem + s02_slot_base(k), 0x30 + (int)k, 0x100);
	r.length = 0x100u;
	r.tag = S02_RECORD_TAG;
	CHECK(s02_flash_crc(&m.flash, s02_slot_base(k), r.length, &r.crc32) ==
	      S02_OK);
	s02_record_encode(&r, raw);
	memcpy(flash_mem + (k == 0u ? S02_RECORD0_OFF : S02_RECORD1_OFF), raw,
	       sizeof(raw));
}

/* S-02.090's start condition: selector {0, 0x01} and slot 0 good. */
static void flash_setup(void)
{
	memset(flash_mem, 0xFF, sizeof(flash_mem));
	m.flash.flash = &flash_ops;
	m.flash.crc32 = wire_crc32;
	m.flash.scratch = flash_scratch;
	m.flash.scratch_len = sizeof(flash_scratch);
	slot_put(0u);
	flash_mem[S02_SELECTOR_OFF] = 0x00u;
	flash_mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	flash_ops_n = flash_writes = flash_one_try = 0;
	opxy_flash_writer(&m.hw, false);
}

/* ---- S-03's eMMC for the OS step: every operation refused ---- */

static int cd_select(void *ctx, unsigned part)
{
	(void)ctx;
	(void)part;
	return -1;
}

static int cd_read(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	(void)ctx;
	(void)sector;
	(void)count;
	(void)buf;
	return -1;
}

static int cd_write(void *ctx, uint32_t sector, uint32_t count,
		    const uint8_t *buf)
{
	(void)ctx;
	(void)sector;
	(void)count;
	(void)buf;
	return -1;
}

static int cd_erase(void *ctx, uint32_t first_sector)
{
	(void)ctx;
	(void)first_sector;
	return -1;
}

static int cd_switch(void *ctx, uint8_t index, uint8_t value)
{
	(void)ctx;
	(void)index;
	(void)value;
	return -1;
}

static const struct s03_emmc_ops emmc_ops = { NULL, cd_select, cd_read,
					      cd_write, cd_erase, cd_switch };
static uint8_t emmc_scratch[4096];

/* ---- the fixture ---- */

static struct s11_run_ops run;
static struct s11_step_ops step;
static uint8_t os_image[0x900];		/* within one S-09.035 piece */

/* The master as test_board_services sets it up, its bridge over the
 * loopback instead of absent, the volume fresh and mounted, the staged
 * images in place. */
static void fresh(void)
{
	wire_s09_release(&S);
	wire_yaffs_detach(&Y);
	memset(vol, 0, VOL_LEN);
	memset(&lb, 0, sizeof lb);
	wb_ramdisk_init(&bd, &ram, vol, (uint32_t)(VOL_LEN / 512u));
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &bd, 0, BLOCKS - 1));
	wire_s09_init(&S, &Y);
	t_sim_init(&sim, S06_MS(2000000));	/* under 2^31 us, as the clock compares */
	s09_slave_init(&sl, &store);

	memset(&m, 0, sizeof(m));
	wire_n = 0;
	fake_mmr_reset();
	fake_core_reset();
	m.hw.time = opxy_time;
	m.clock = opxy_clock(&m.hw);
	s09_master_init(&m.bridge, &bus, &sim.clock);
	CHECK(s09_master_start(&m.bridge));
	m.slave_present = true;
	s09_files_init(&m.sv.files, &m.bridge);
	s09_files_fs_ops(&m.sv.files, &m.sv.staging);
	s06_node_init(&m.node, crc8, &m.clock);
	s06_link_init(&m.link[S06_LINK_PSOC], &port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_PSOC);
	m.node.link[S06_LINK_PSOC] = &m.link[S06_LINK_PSOC];
	opxy_usb_host_ops(&m, &m.sv.host_ops);
	opxy_startup_read(&m, opxy_otp_get);
	opxy_log_start(&m);
	CHECK_EQ(s09_files_mount(&m.sv.files), 0);
	flash_setup();
	m.emmc.ops = &emmc_ops;
	m.emmc.crc32 = wire_crc32;
	m.emmc.boot_size = 0x40000u;
	m.emmc.scratch = emmc_scratch;
	m.emmc.scratch_len = sizeof emmc_scratch;
	opxy_install_ops(&m, &run, &step);
	/* The run's values with the option on and both protected steps
	 * confirmed, so that they write (S-11.090, S-11.096); the stated
	 * values come with extracted(). */
	opxy_install_begin(&m, true, false,
			   S11_STEP_BIT(S11_STEP_BOOTLOADER) |
			   S11_STEP_BIT(S11_STEP_SPIBOOT));
	m.sv.extracted = false;
}

/* A file with its directories. */
static bool put(const char *path, const uint8_t *d, uint32_t n)
{
	struct s09_files *f = &m.sv.files;
	int32_t fd;
	bool ok;

	if (!s09_files_mkdirs(f, path))
		return false;
	fd = s09_files_open(f, path, S09_O_WRONLY | S09_O_CREAT | S09_O_TRUNC);
	ok = fd >= 0 && s09_files_write(f, fd, d, n) == (int32_t)n;
	return s09_files_close(f, fd) == 0 && ok;
}

static void stage_all(void)
{
	for (size_t i = 0; i < sizeof os_image; i++)
		os_image[i] = (uint8_t)(i * 7u + 3u);
	CHECK(put(BOOTLOADER, v_boot_good, sizeof v_boot_good));
	CHECK(put(OS, os_image, sizeof os_image));
	CHECK(put(SPIBOOT, v_boot_good, sizeof v_boot_good));
}

/* What this boot's extraction would have kept (S-11.030). */
static void extracted(void)
{
	memset(&m.sv.extracted_sizes, 0, sizeof m.sv.extracted_sizes);
	m.sv.extracted_sizes.size[S11_C_BOOTLOADER] = sizeof v_boot_good;
	m.sv.extracted_sizes.crc32[S11_C_BOOTLOADER] =
		wire_crc32(0u, v_boot_good, sizeof v_boot_good);
	m.sv.extracted_sizes.size[S11_C_OS] = sizeof os_image;
	m.sv.extracted_sizes.crc32[S11_C_OS] =
		wire_crc32(0u, os_image, sizeof os_image);
	m.sv.extracted = true;
}

static bool slot1_holds_image(void)
{
	return memcmp(flash_mem + s02_slot_base(1u), v_boot_good,
		      sizeof v_boot_good) == 0 &&
	       flash_mem[S02_SELECTOR_OFF] == 0x01u &&
	       flash_mem[S02_SELECTOR_OFF + 1u] == 0x00u;
}

/* The step through S-11's result (S-11.068): Bad file reaches the run. */
static enum s11_result result_of(unsigned which)
{
	struct s11_staged st;
	struct s11_step_env env;
	struct s17_copro copro;

	s11_staged_scan(&m.sv.staging, &st);
	s17_copro_init(&copro);
	memset(&env, 0, sizeof env);
	env.staged = &st;
	env.option = true;
	env.confirmed = S11_STEP_BIT(S11_STEP_BOOTLOADER) |
			S11_STEP_BIT(S11_STEP_SPIBOOT);
	env.copro = &copro;
	return s11_step_result(which, &env, &step);
}

/* ---- the cases ---- */

/* No stated values (no extraction came before): nothing to check the
 * image against, so nothing is written. */
static void test_no_stated_values(void)
{
	t_case = "no stated values";
	fresh();
	stage_all();
	CHECK(!step.bootloader_write(step.ctx));
	CHECK(step.write_failure(step.ctx) == S11_INTERNAL);
	CHECK_EQ(flash_ops_n, 0u);
}

/* The boot that extracted the package: one read, checked against the
 * member's size and CRC-32 (S-11.030). */
static void test_extracted_intact(void)
{
	static uint8_t before[S02_FLASH_SIZE];

	t_case = "extracted, intact";
	fresh();
	stage_all();
	extracted();
	memcpy(before, flash_mem, sizeof before);
	CHECK(step.bootloader_write(step.ctx));
	CHECK_EQ(lb.reads, 1u);			/* read once */
	CHECK_EQ(lb.corrupted, 0u);
	CHECK(flash_writes > 0u);
	/* S-13b.017: the writer's every operation with three tries, then
	 * one again. */
	CHECK_EQ(flash_one_try, 0u);
	CHECK(m.hw.flash.tries == 1u);
	CHECK(slot1_holds_image());
	CHECK(memcmp(before, flash_mem, sizeof before) != 0);
	CHECK(step.write_failure(step.ctx) == S11_OK);
	CHECK(result_of(S11_STEP_BOOTLOADER) == S11_FAILURE); /* S-02.090: {1, 0} */
}

static void test_extracted_damaged(void)
{
	static uint8_t before[S02_FLASH_SIZE];

	t_case = "extracted, read damaged";
	fresh();
	stage_all();
	extracted();
	memcpy(before, flash_mem, sizeof before);
	lb.corrupt_read = 1u;
	CHECK(!step.bootloader_write(step.ctx));
	CHECK_EQ(lb.reads, 1u);
	CHECK_EQ(lb.corrupted, 1u);
	CHECK(step.write_failure(step.ctx) == S11_BAD_FILE);
	CHECK_EQ(flash_ops_n, 0u);
	CHECK(memcmp(before, flash_mem, sizeof before) == 0);
	/* and through S-11's step: Bad file, the run stops (S-11.059) */
	lb.corrupt_read = lb.reads + 1u;
	CHECK(result_of(S11_STEP_BOOTLOADER) == S11_BAD_FILE);
	CHECK(!s11_result_continues(S11_BAD_FILE));
	CHECK_EQ(flash_ops_n, 0u);
}

/* The staging area does not hold what the extraction staged (a length or
 * CRC-32 other than the stated one, S-11.095): refused too. */
static void test_extracted_stale(void)
{
	t_case = "extracted, stale";
	fresh();
	stage_all();
	extracted();
	m.sv.extracted_sizes.crc32[S11_C_BOOTLOADER] ^= 0x80000000u;
	CHECK(!step.bootloader_write(step.ctx));
	CHECK(step.write_failure(step.ctx) == S11_BAD_FILE);
	CHECK_EQ(flash_ops_n, 0u);
	extracted();
	m.sv.extracted_sizes.size[S11_C_BOOTLOADER] -= 1u;
	CHECK(!step.bootloader_write(step.ctx));
	CHECK(step.write_failure(step.ctx) == S11_BAD_FILE);
	CHECK_EQ(flash_ops_n, 0u);
}

/* The OS step (S-11.067): a damaged read is Bad file before S-03 is asked
 * anything; an intact one reaches S-03's writer, which the eMMC refuses
 * here (Internal error through the step's own result, not Bad file). */
static void test_os_step(void)
{
	t_case = "OS step";
	fresh();
	stage_all();
	extracted();
	lb.corrupt_read = 1u;
	CHECK(!step.os_write(step.ctx));
	CHECK_EQ(lb.corrupted, 1u);
	CHECK(step.write_failure(step.ctx) == S11_BAD_FILE);
	CHECK(result_of(S11_STEP_OS) == S11_INTERNAL);	/* intact: refused by the eMMC */
	CHECK(step.write_failure(step.ctx) != S11_BAD_FILE);
	lb.corrupt_read = lb.reads + 1u;
	CHECK(result_of(S11_STEP_OS) == S11_BAD_FILE);
	lb.corrupt_read = 0;
	CHECK(result_of(S11_STEP_OS) == S11_INTERNAL);
}

/* S-03.068: S-03's flush reaches the slave as the bridge's 0x04, and no other
 * EXT_CSD byte has a request (S-03.009). */
static void test_bridge_flush(void)
{
	struct s03_emmc_ops ops;

	t_case = "bridge flush";
	fresh();
	ops = opxy_bridge_emmc(&m.bridge);
	st_flushes = 0;
	CHECK(ops.switch_byte(ops.ctx, 33, 1) != 0);
	CHECK(ops.switch_byte(ops.ctx, 32, 0) != 0);
	CHECK_EQ(st_flushes, 0u);
	CHECK(ops.switch_byte(ops.ctx, 32, 1) == 0);
	CHECK_EQ(st_flushes, 1u);
}

/* A read that fails (S-11.095): the slave gone during the image's read;
 * Internal error, not Bad file, and nothing written. The slave is absent
 * from then on (S-09.008). */
static void test_read_fails(void)
{
	t_case = "a read fails";
	fresh();
	stage_all();
	extracted();
	lb.fail_read = 1u;
	CHECK(!step.os_write(step.ctx));
	CHECK(lb.dead);
	CHECK(step.write_failure(step.ctx) == S11_INTERNAL);
	CHECK_EQ(flash_ops_n, 0u);

	fresh();
	stage_all();
	extracted();
	lb.fail_read = 1u;
	CHECK(!step.bootloader_write(step.ctx));
	CHECK(lb.dead);
	CHECK(step.write_failure(step.ctx) == S11_INTERNAL);
	CHECK_EQ(flash_ops_n, 0u);
}

/* The first-stage step (S-11.072): the same check, before any erase. */
static void test_spiboot_step(void)
{
	static uint8_t before[S02_FLASH_SIZE];

	t_case = "first-stage step";
	fresh();
	stage_all();
	extracted();
	m.sv.extracted_sizes.size[S11_C_SPIBOOT] = sizeof v_boot_good;
	m.sv.extracted_sizes.crc32[S11_C_SPIBOOT] =
		wire_crc32(0u, v_boot_good, sizeof v_boot_good);
	memcpy(before, flash_mem, sizeof before);
	lb.corrupt_read = 1u;
	CHECK(!step.spiboot_write(step.ctx));
	CHECK_EQ(lb.corrupted, 1u);
	CHECK(step.write_failure(step.ctx) == S11_BAD_FILE);
	CHECK_EQ(flash_ops_n, 0u);
	CHECK(memcmp(before, flash_mem, sizeof before) == 0);
	CHECK(result_of(S11_STEP_SPIBOOT) == S11_BAD_FILE);
	CHECK_EQ(flash_ops_n, 0u);
}

/* S-09.075: the link is brought up to speed before an extraction; the
 * run's steps then work on its rung, and the log says where it is. */
static void test_link_fast(void)
{
	t_case = "link";
	fresh();
	CHECK_EQ(s09_master_rung(&m.bridge), S09_RUNG_PLAIN);
	opxy_bridge_fast(&m);
	CHECK_EQ(s09_master_rung(&m.bridge), S09_RUNG_TOP);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_QUAD);
	CHECK_EQ(m.sv.files.link, S09_RUNG_TOP);
	stage_all();
	extracted();
	CHECK(step.bootloader_write(step.ctx));
	CHECK(slot1_holds_image());
	CHECK_EQ(s09_master_fell(&m.bridge), 0u);
	/* S-09.078: lowered with the handshake when the run ends; a staging
	 * file's open does not raise it again, and the release is plain */
	opxy_bridge_plain(&m);
	CHECK_EQ(s09_master_rung(&m.bridge), S09_RUNG_PLAIN);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_PLAIN);
	CHECK(m.sv.files.link == 0u && s09_master_fell(&m.bridge) == 0u);
	CHECK(s09_files_staging_open(&m.sv.files, "l/after"));
	CHECK(s09_files_staging_write(&m.sv.files, (const uint8_t *)"abc", 3));
	CHECK(s09_files_staging_close(&m.sv.files));
	CHECK_EQ(s09_master_rung(&m.bridge), S09_RUNG_PLAIN);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_PLAIN);
	CHECK_EQ(s09_master_release(&m.bridge), 1);		/* no OS */
	CHECK(lb.last_req == S09_RELEASE && !m.bridge.absent);
	/* raised again, the release lowers it first */
	opxy_bridge_fast(&m);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_QUAD);
	CHECK_EQ(s09_master_release(&m.bridge), 1);
	CHECK(s09_slave_mode(&sl) == S09_MODE_PLAIN && !m.bridge.absent);
	CHECK_EQ(s09_master_rung(&m.bridge), S09_RUNG_PLAIN);
	/* with no slave nothing is asked */
	fresh();
	m.slave_present = false;
	opxy_bridge_fast(&m);
	CHECK_EQ(s09_master_rung(&m.bridge), S09_RUNG_PLAIN);
	CHECK_EQ(m.sv.files.link, 0u);
	opxy_bridge_plain(&m);
	CHECK_EQ(m.sv.files.link, 0u);
}

/* S-13b.083, S-09.079 cause 9: the slave's change to the count end comes
 * with the fall-back its reply brings; the log tells it once, and not again
 * at the next fall-back, whose own cause the status then shows. */
static void test_link_log_end(void)
{
	struct s09_stat st;

	t_case = "link log end";
	fresh();
	s06_link_init(&m.link[S06_LINK_CDC], &cdc_port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	m.node.link[S06_LINK_CDC] = &m.link[S06_LINK_CDC];
	m.sv.host_ops.cdc_open = cdc_is_open;
	m.sv.log.cdc_on = true;
	opxy_bridge_fast(&m);
	cdc_n = 0;
	s09_slave_reply_end_changed(&sl, S09_READ, 4108u);
	s09_slave_link_fault(&sl);
	CHECK_EQ(s09_files_stat(&m.sv.files, "/yaffs2", &st), 0);
	opxy_bridge_report(&m);
	CHECK(cdc_has("; the slave: a reply it saw no end of on 0x2a"));
	CHECK(cdc_has("bridge: the slave saw no end to a reply; it now ends its "
		      "single-I/O replies by their word count for the rest of the run"));
	cdc_n = 0;
	opxy_bridge_report(&m);
	opxy_bridge_end_note(&m);
	CHECK(!cdc_has("word count"));
	s09_slave_link_fault(&sl);
	CHECK_EQ(s09_files_stat(&m.sv.files, "/yaffs2", &st), 0);
	opxy_bridge_report(&m);
	CHECK(cdc_has("; the slave: its receive failed") && !cdc_has("word count"));
}

/* The link's record in the log, in plain words: a line for each rung the
 * negotiation tried, where requests run and how, each fall-back of a
 * request from where to where and why, where requests ran and, apart, that
 * the link was lowered when the run ended. */
static void test_link_log(void)
{
	struct s09_stat st;

	t_case = "link log";
	fresh();
	s06_link_init(&m.link[S06_LINK_CDC], &cdc_port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	m.node.link[S06_LINK_CDC] = &m.link[S06_LINK_CDC];
	m.sv.host_ops.cdc_open = cdc_is_open;
	m.sv.log.cdc_on = true;
	cdc_n = 0;
	opxy_bridge_fast(&m);
	CHECK(cdc_has("bridge: tried rung 4 (quad I/O at 50 MHz, checked): tests passed"));
	CHECK(cdc_has(" us: kept"));
	CHECK(cdc_has("bridge: requests run in quad I/O at 50 MHz, checked"));
	/* the slave falls back alone: the request finds out */
	cdc_n = 0;
	s09_slave_link_fault(&sl);
	CHECK_EQ(s09_files_stat(&m.sv.files, "/yaffs2", &st), 0);
	opxy_bridge_report(&m);
	CHECK(cdc_has("bytes (its first send) fell back from rung 4 to rung 3: "));
	CHECK(cdc_has("; the slave: its receive failed"));
	CHECK(cdc_has("bridge: tried rung 3 (quad I/O at 25 MHz, checked)"));
	/* nothing logged twice */
	cdc_n = 0;
	opxy_bridge_report(&m);
	CHECK(!cdc_has("fell back") && !cdc_has("tried rung"));
	opxy_bridge_plain(&m);
	CHECK(cdc_has("bridge: requests ran in quad I/O at 25 MHz, checked"));
	CHECK(cdc_has("bridge: the link was lowered to plain when it ended"));
	CHECK(!cdc_has("FELL BACK") && !cdc_has("fell back"));

	/* S-09.075: no rung, rung 1 included, passes its tests: the run is
	 * plain, logged, and the slave settled */
	fresh();
	s06_link_init(&m.link[S06_LINK_CDC], &cdc_port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	m.node.link[S06_LINK_CDC] = &m.link[S06_LINK_CDC];
	m.sv.host_ops.cdc_open = cdc_is_open;
	m.sv.log.cdc_on = true;
	cdc_n = 0;
	lb.bad_tests = true;
	opxy_bridge_fast(&m);
	CHECK_EQ(s09_master_rung(&m.bridge), S09_RUNG_PLAIN);
	CHECK(s09_master_settled(&m.bridge) && s09_slave_mode(&sl) == S09_MODE_PLAIN);
	CHECK(cdc_has("bridge: tried rung 1 (single I/O at 25 MHz, checked): the test of 4 bytes"));
	CHECK(cdc_has("bridge: requests run in single I/O at 25 MHz, plain"));
	m.sv.log.cdc_on = false;
}

/* S-10a.049: USB is serviced from inside a run. The service is rate-limited
 * and metered; the bridge's waits, a long CRC and the flash's busy wait all
 * come to it. */
static void test_usb_service(void)
{
	static uint8_t image[0x9000];
	/* the service's interval and the meter both count the board clock */
	const uint64_t ms = 1000u * (uint64_t)opxy_time.cycles_per_us;
	struct s06_clock c;
	uint64_t t = 50u * ms;

	t_case = "usb service";
	fresh();
	c = opxy_clock(&m.hw);
	fake_core_set_cycle_step(0);
	fake_core_set_cycles(t);
	opxy_usb_gap_begin();
	fake_core_set_cycles(t += 5u * ms);
	c.idle(c.ctx, 0);			/* a wait on the bridge */
	fake_core_set_cycles(t += ms / 2u);
	opxy_usb_breathe();			/* too soon: not a service */
	fake_core_set_cycles(t += 3u * ms);
	opxy_usb_breathe();
	CHECK_EQ(opxy_usb_gap_end(), 5000u);
	/* 5 ms, then 3.5 ms: the call that came too soon did not count */
	opxy_usb_gap_begin();
	fake_core_set_cycles(t + 7u * ms);
	CHECK_EQ(opxy_usb_gap_end(), 0u);	/* nothing serviced in between */

	/* a long CRC is the software's, with a service after each piece */
	for (size_t i = 0; i < sizeof(image); i++)
		image[i] = (uint8_t)(i * 13u + 5u);
	fake_core_set_cycle_step(ms);		/* a millisecond a clock read */
	opxy_usb_gap_begin();
	CHECK_EQ(opxy_crc32_long(0u, image, sizeof(image)),
		 wire_crc32(0u, image, sizeof(image)));
	CHECK(opxy_usb_gap_end() != 0u);
	/* the flash's busy wait and S-02's CRCs are wired to it */
	CHECK(m.flash.crc32 == opxy_crc32_long || m.flash.crc32 == wire_crc32);
	fake_core_set_cycle_step(1);
}

/* The log's lines reach a host that has the CDC port open with the sink on,
 * a step's error among them; USB is serviced through a run, so they are
 * taken as they are written. */
static void test_log_through_a_run(void)
{
	t_case = "log on CDC";
	fresh();
	s06_link_init(&m.link[S06_LINK_CDC], &cdc_port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	m.node.link[S06_LINK_CDC] = &m.link[S06_LINK_CDC];
	m.sv.host_ops.cdc_open = cdc_is_open;
	m.sv.log.cdc_on = true;
	cdc_n = 0;
	s14_log_str(&m.sv.log, S14_ERROR, S14_NO_TAG,
		    "install: step 1 (OS) bad file: staged image: check failed");
	CHECK(cdc_has("staged image: check failed"));
	/* with the port closed nothing is queued for it */
	m.sv.host_ops.cdc_open = NULL;
	cdc_n = 0;
	s14_log_str(&m.sv.log, S14_ERROR, S14_NO_TAG, "not taken");
	CHECK_EQ(cdc_n, 0u);
}

/* ---- the host hold through an install (S-10a.049) ---- */

/* What the host sends on CDC: read by the link, oldest first. */
static uint8_t host_rx[1024];
static size_t host_rx_n, host_rx_r;

static size_t host_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t n = host_rx_n - host_rx_r;

	(void)ctx;
	if (n > max)
		n = max;
	memcpy(buf, host_rx + host_rx_r, n);
	host_rx_r += n;
	return n;
}

static const struct s06_port_ops host_port = { host_read, cdc_room,
					       cdc_write };

static void host_sends(uint8_t route, uint8_t seq, uint8_t id,
		       const uint8_t *p, size_t n)
{
	host_rx_n += s06_frame_encode(route, seq, id, p, n, crc8,
				      host_rx + host_rx_n);
}

/* The Nordic's port: everything written, nothing received. */
static uint8_t nordic_wire[1u << 16];
static size_t nordic_n;

static void nordic_write(void *ctx, const uint8_t *data, size_t n)
{
	(void)ctx;
	if (nordic_n + n <= sizeof(nordic_wire)) {
		memcpy(nordic_wire + nordic_n, data, n);
		nordic_n += n;
	}
}

static const struct s06_port_ops nordic_port = { port_read, port_room,
						 nordic_write };

static bool holds(const uint8_t *s, size_t n, const uint8_t *f, size_t fn)
{
	for (size_t i = 0; i + fn <= n; i++)
		if (memcmp(s + i, f, fn) == 0)
			return true;
	return false;
}

static unsigned pongs_in_order(void);

/* The frames for the PSoC and the Nordic the host sends during the run. */
static uint8_t to_psoc[S06_MAX_WIRE], to_nordic[S06_MAX_WIRE];
static size_t to_psoc_n, to_nordic_n;

static struct {
	unsigned reads;
	bool held, forwarded, taken, read_any, logged;
} during;

/*
 * The package's reads: the first stands for the whole run. The host sends
 * a frame for the PSoC, one for the Nordic and nine pings for the master;
 * then the run waits as S-07 and S-08 do (the whole node polled, S-07's
 * service, the clock's idle) and writes a log line. Then a read error ends
 * the install at check 1.
 */
static bool run_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	(void)ctx;
	(void)off;
	memset(buf, 0, n);
	if (during.reads++ != 0u)
		return false;
	during.held = opxy_host_held();
	to_psoc_n = s06_frame_encode(s06_route(S06_ADDR_HOST, S06_ADDR_PSOC),
				     40, 0x7A, NULL, 0, crc8, to_psoc);
	to_nordic_n = s06_frame_encode(s06_route(S06_ADDR_HOST,
						 S06_ADDR_NORDIC),
				       41, 0x7B, NULL, 0, crc8, to_nordic);
	memcpy(host_rx + host_rx_n, to_psoc, to_psoc_n);
	host_rx_n += to_psoc_n;
	memcpy(host_rx + host_rx_n, to_nordic, to_nordic_n);
	host_rx_n += to_nordic_n;
	for (uint8_t i = 0; i < 9u; i++)
		host_sends(s06_route(S06_ADDR_HOST, S06_ADDR_MASTER), i, 0x01,
			   &i, 1);
	for (unsigned k = 0; k < 3u; k++) {
		s06_node_poll(&m.node);
		opxy_usb_poll(&m);
		m.clock.idle(m.clock.ctx, 0);
	}
	during.read_any = host_rx_r != 0u;
	during.forwarded = holds(wire, wire_n, to_psoc, to_psoc_n) ||
			   nordic_n != 0u;
	during.taken = m.link[S06_LINK_CDC].in_count != 0u ||
		       pongs_in_order() != 0u;
	s14_log_str(&m.sv.log, S14_INFO, S14_NO_TAG, "a line during the run");
	during.logged = cdc_has("a line during the run");
	cdc_n = 0;
	return false;
}

/* The pongs on CDC, in order: payload i of the k-th is i == k. */
static unsigned pongs_in_order(void)
{
	uint8_t raw[256];
	unsigned n = 0;
	size_t len;

	for (size_t k = 0; (len = t_nth_raw(cdc_wire, cdc_n, k, raw)) != 0; k++)
		if (len == 6u && raw[2] == 0x02u && raw[3] == 1u &&
		    raw[4] == n)
			n++;
	return n;
}

/* The master with USB up, the host on a test CDC port with the port open,
 * the Nordic's link and S-06.050's table. */
static void host_setup(void)
{
	fresh();
	s10c_host_init(&m.sv.host, &m.sv.host_ops, &m);
	opxy_usb_start(&m);
	CHECK(opxy_usb_up());
	/* the host's side of the CDC port; the link as the board set it up */
	m.link[S06_LINK_CDC].ops = &host_port;
	host_rx_n = host_rx_r = 0;
	m.sv.host_ops.cdc_open = cdc_is_open;
	m.sv.log.cdc_on = true;
	cdc_n = 0;
	s06_link_init(&m.link[S06_LINK_NORDIC], &nordic_port, NULL,
		      S06_ADDR_MASTER, S06_ADDR_NORDIC);
	m.node.link[S06_LINK_NORDIC] = &m.link[S06_LINK_NORDIC];
	nordic_n = 0;
	m.node.routes = s06_master_routes;
	m.node.nroutes = 6u;
}

/*
 * S-10a.049: during an install nothing the host sends is read: a frame for
 * the PSoC or the Nordic is not forwarded, nothing reaches S-10c, and log
 * lines still go out. Once it has ended, both frames are forwarded and all
 * nine pings are answered, in order: none was lost to the 8-entry inbox.
 */
static void test_host_hold_through_a_run(void)
{
	static const struct s11_zip_ops no_zip;
	const struct s11_package_ops pkg = { .read = run_read };
	const struct s11_check_in in = { .form = S11_FORM_BODY,
					 .size = 0x10000u, .option = true };
	const uint8_t crc_off = opxy_crc_off;
	struct s11_install_result res;

	t_case = "host hold";
	host_setup();
	memset(&during, 0, sizeof(during));
	opxy_crc_off = (uint8_t)((1u << OPXY_CRCS) - 1u);	/* no search */

	opxy_install_package(&m, &pkg, &no_zip, &in, &res);
	CHECK(res.reason != S11_REASON_NONE);	/* ended at check 1 */
	CHECK(during.reads != 0u);
	CHECK(during.held);
	CHECK(!during.read_any);
	CHECK(!during.forwarded);
	CHECK(!during.taken);
	CHECK(during.logged);
	CHECK(!opxy_host_held());		/* let go on the refusal */

	/* the loop's passes */
	for (unsigned k = 0; k < 3u; k++) {
		s06_node_poll(&m.node);
		opxy_usb_poll(&m);
	}
	CHECK(holds(wire, wire_n, to_psoc, to_psoc_n));
	CHECK(holds(nordic_wire, nordic_n, to_nordic, to_nordic_n));
	CHECK_EQ(pongs_in_order(), 9u);
	CHECK_EQ(m.link[S06_LINK_CDC].inbox_lost, 0u);
	opxy_crc_off = crc_off;
	opxy_usb_stop();
}

/* ---- the loop's pass alone takes the host's input (S-10a.049) ---- */

/* S-07's service hook as the board has it: the stack's own work only. */
static void psoc_wait_service(void *ctx)
{
	(void)ctx;
	opxy_usb_breathe();
}

/* The host sends a frame for the PSoC (its seq marks it) and a ping. */
static size_t host_sends_psoc_and_ping(uint8_t seq, uint8_t ping,
				       uint8_t *frame)
{
	const size_t n = s06_frame_encode(s06_route(S06_ADDR_HOST,
						    S06_ADDR_PSOC),
					  seq, 0x7A, NULL, 0, crc8, frame);

	memcpy(host_rx + host_rx_n, frame, n);
	host_rx_n += n;
	host_sends(s06_route(S06_ADDR_HOST, S06_ADDR_MASTER), seq, 0x01,
		   &ping, 1);
	return n;
}

/* Where on CDC the reply with this id (and first payload byte, for a
 * pong) stands; -1 if none. */
static int reply_at(uint8_t id, int first)
{
	uint8_t raw[256];
	size_t len;

	for (size_t k = 0; (len = t_nth_raw(cdc_wire, cdc_n, k, raw)) != 0; k++)
		if (len >= 5u && raw[2] == id &&
		    (first < 0 || (raw[3] != 0u && raw[4] == first)))
			return (int)k;
	return -1;
}

static uint8_t frame_c[S06_MAX_WIRE];
static size_t frame_c_n;
static struct {
	unsigned calls;
	bool forwarded, ponged, read_any;
} handler;

/*
 * An S-10c handler whose board op waits on the PSoC (the info request's,
 * here): the host sends a frame for the PSoC and a ping meanwhile, and the
 * handler also tries the loop's pass from inside itself.
 */
static void read_info_waiting(void *ctx, struct s10c_info *v)
{
	const size_t before = host_rx_r;

	(void)ctx;
	memset(v, 0, sizeof(*v));
	handler.calls++;
	frame_c_n = host_sends_psoc_and_ping(52, 2, frame_c);
	s07_sleep(&m.psoc, 2000u);
	opxy_usb_poll(&m);
	handler.read_any = host_rx_r != before;
	handler.forwarded = holds(wire, wire_n, frame_c, frame_c_n);
	handler.ponged = pongs_in_order() > 2u;
}

/*
 * Only the loop's own pass reads the host links. Outside any wait the pass
 * forwards and answers what it reads; an S-07 wait at the loop's level, and
 * an S-10c handler's PSoC wait inside the pass, read nothing of the host's,
 * and what came meanwhile is taken by the pass's next read, in order.
 */
static void test_loop_pass_alone(void)
{
	uint8_t a[S06_MAX_WIRE], b[S06_MAX_WIRE];
	const uint8_t info = 0;
	size_t an, bn, read_before;

	t_case = "loop pass alone";
	host_setup();
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	m.psoc_ops.service = psoc_wait_service;
	fake_core_set_cycle_step(4000u);	/* 10 us a clock read */

	/* the loop: the held loop's node poll reads no host link; its USB
	 * pass forwards the frame and answers the ping */
	an = host_sends_psoc_and_ping(50, 0, a);
	s06_node_poll(&m.node);
	CHECK_EQ(host_rx_r, 0u);
	CHECK(!holds(wire, wire_n, a, an));
	opxy_usb_poll(&m);
	CHECK(holds(wire, wire_n, a, an));
	CHECK_EQ(pongs_in_order(), 1u);

	/* an S-07 wait at the loop's level (its service hook the board's) */
	read_before = host_rx_r;
	bn = host_sends_psoc_and_ping(51, 1, b);
	s07_sleep(&m.psoc, 2000u);
	CHECK_EQ(host_rx_r, read_before);
	CHECK(!holds(wire, wire_n, b, bn));
	CHECK_EQ(pongs_in_order(), 1u);
	opxy_usb_poll(&m);
	CHECK(holds(wire, wire_n, b, bn));
	CHECK_EQ(pongs_in_order(), 2u);

	/* an S-10c handler's PSoC wait, inside the pass */
	m.sv.host_ops.read_info = read_info_waiting;
	memset(&handler, 0, sizeof(handler));
	host_sends(s06_route(S06_ADDR_HOST, S06_ADDR_MASTER), 60, 0x09,
		   &info, 0);
	opxy_usb_poll(&m);
	CHECK_EQ(handler.calls, 1u);
	CHECK(!handler.read_any);
	CHECK(!handler.forwarded);
	CHECK(!handler.ponged);
	/* the pass's next read took them: forwarded, answered, in order */
	CHECK(holds(wire, wire_n, frame_c, frame_c_n));
	CHECK_EQ(pongs_in_order(), 3u);
	CHECK(reply_at(0x0A, -1) >= 0 &&
	      reply_at(0x0A, -1) < reply_at(0x02, 2));
	CHECK_EQ(m.link[S06_LINK_CDC].inbox_lost, 0u);
	CHECK(m.link[S06_LINK_CDC].held && m.link[S06_LINK_MIDI].held);
	fake_core_set_cycle_step(1);
	opxy_usb_stop();
}

/* ---- a whole install through opxy_install_source() (S-11.073, S-11.096,
 * S-15.116, S-10c.106) ---- */

/*
 * A package of the body form (as a host transfer gives it, S-11.050): the
 * TE header, a manifest with a plaintext payload (key 0xFE), the name
 * block and a stored zip of the bootloader, the first stage and the three
 * images whose steps are switched off. No OS image and no content, so the
 * run's other steps find nothing.
 */
static const uint8_t ble_image[64] = { 0x42 };
static const uint8_t nordic_bl_image[32] = { 0x4E };
static const char backplane_text[] = ":0000000000\n";

static uint8_t pkg_buf[0x40u + 0x300u + 0x100u + 0x2000u];
static uint32_t pkg_len;

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static size_t zput16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	return 2;
}

static size_t zput32(uint8_t *p, uint32_t v)
{
	return zput16(p, v & 0xFFFFu) + zput16(p + 2, v >> 16);
}

struct zmember {
	const char *name;
	const uint8_t *data;
	uint32_t len;
};

/* A stored zip (APPNOTE 4.3): the local headers and data, the central
 * directory, its end record. */
static size_t zip_make(uint8_t *out, const struct zmember *mb, size_t n)
{
	uint32_t at[8];
	size_t o = 0, cd;

	for (size_t i = 0; i < n; i++) {
		const size_t nl = strlen(mb[i].name);

		at[i] = (uint32_t)o;
		o += zput32(out + o, 0x04034B50u);
		o += zput16(out + o, 20);
		o += zput16(out + o, 0);		/* flags */
		o += zput16(out + o, 0);		/* stored */
		o += zput32(out + o, 0);		/* time, date */
		o += zput32(out + o, wire_crc32(0u, mb[i].data, mb[i].len));
		o += zput32(out + o, mb[i].len);
		o += zput32(out + o, mb[i].len);
		o += zput16(out + o, (unsigned)nl);
		o += zput16(out + o, 0);
		memcpy(out + o, mb[i].name, nl);
		o += nl;
		memcpy(out + o, mb[i].data, mb[i].len);
		o += mb[i].len;
	}
	cd = o;
	for (size_t i = 0; i < n; i++) {
		const size_t nl = strlen(mb[i].name);

		o += zput32(out + o, 0x02014B50u);
		o += zput16(out + o, 20);
		o += zput16(out + o, 20);
		o += zput16(out + o, 0);
		o += zput16(out + o, 0);
		o += zput32(out + o, 0);
		o += zput32(out + o, wire_crc32(0u, mb[i].data, mb[i].len));
		o += zput32(out + o, mb[i].len);
		o += zput32(out + o, mb[i].len);
		o += zput16(out + o, (unsigned)nl);
		o += zput32(out + o, 0);		/* extra, comment */
		o += zput32(out + o, 0);		/* disk, internal */
		o += zput32(out + o, 0);		/* external */
		o += zput32(out + o, at[i]);
		memcpy(out + o, mb[i].name, nl);
		o += nl;
	}
	o += zput32(out + o, 0x06054B50u);
	o += zput32(out + o, 0);
	o += zput16(out + o, (unsigned)n);
	o += zput16(out + o, (unsigned)n);
	o += zput32(out + o, (uint32_t)(o - cd - 12u));
	o += zput32(out + o, (uint32_t)cd);
	o += zput16(out + o, 0);
	return o;
}

static void package_make(void)
{
	const struct zmember mb[] = {
		{ "firmware/blackfin/bin/bootloader.ldr", v_boot_good,
		  sizeof v_boot_good },
		{ "firmware/blackfin/bin/spiboot.ldr", v_boot_fs_good,
		  sizeof v_boot_fs_good },
		{ "firmware/ble/bin/ble.bin", ble_image, sizeof ble_image },
		{ "firmware/ble/bin/nordic_bootloader_0.bin", nordic_bl_image,
		  sizeof nordic_bl_image },
		{ "firmware/backplane/bin/backplane.cyacd",
		  (const uint8_t *)backplane_text, sizeof backplane_text - 1u },
	};
	uint8_t *h = pkg_buf, *man = pkg_buf + 0x40u, *pl = pkg_buf + 0x340u;
	size_t zip_len;
	uint16_t crc;

	memset(pkg_buf, 0, sizeof pkg_buf);
	zip_len = zip_make(pl + 0x100u, mb, sizeof mb / sizeof mb[0]);
	pkg_len = (uint32_t)(0x340u + 0x100u + zip_len);
	put_be32(h, 0xBEEFCAFEu);
	h[4] = 0x01;					/* firmware */
	put_be32(h + 0x17, 0x00084000u);
	memcpy(pl, "firmware.zip", 12);			/* the name block */
	man[0x04] = 0xFE;				/* plaintext */
	zput32(man + 0x80, (uint32_t)zip_len);
	memcpy(man + 0x84, "TESG", 4);
	man[0x88] = 0x01;
	man[0x89] = 0x01;				/* not TE-signed */
	zput32(man + 0x8C, 0x00084000u);
	zput32(man + 0x98, (uint32_t)(0x100u + zip_len));
	ref_sha256(pl, 0x100u + zip_len, man + 0x9C);
	crc = ref_crc16(0, pkg_buf + 0x40u, pkg_len - 0x40u);
	h[9] = (uint8_t)(crc >> 8);
	h[10] = (uint8_t)crc;
}

static bool pkg_src_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	(void)ctx;
	if (off > pkg_len || n > pkg_len - off)
		return false;
	memcpy(buf, pkg_buf + off, n);
	return true;
}

static bool pkg_src_write(void *ctx, uint32_t off, const uint8_t *buf, size_t n)
{
	(void)ctx;
	if (off > pkg_len || n > pkg_len - off)
		return false;
	memcpy(pkg_buf + off, buf, n);
	return true;
}

/*
 * The PSoC's key events, by script: each confirmation's own, in ms from
 * when its keys are taken (the board's key hook is the confirmation's
 * then: the menus' is NULL in these tests). One frame per poll of the
 * link, as the board's ring gives them.
 */
#define K_DIGIT1 0x06u
#define K_DIGIT8 0x15u
struct kev {
	uint32_t ms;
	uint8_t key;
	bool down;
};

static struct {
	const struct kev *ev[4];	/* per confirmation, ending at ms 0 */
	unsigned asks;			/* confirmations seen */
	bool in_ask;
	uint64_t start_ms[4], end_ms[4];
	size_t next;
	uint8_t bits[S07_BITMAP];
	bool gap;
	bool host_sent;		/* the host sends during the next one */
} ks;

/* The board's ms; the confirmation's 60 s count from its screen, a
 * little before the first poll these tests see (so 59900, not 60000). */
static uint64_t now_ms(void)
{
	return wb_time_ms(&m.hw.time);
}

static size_t psoc_read(void *ctx, uint8_t *buf, size_t max)
{
	const struct kev *e;
	uint8_t frame[S06_MAX_WIRE];
	size_t n;

	(void)ctx;
	if (m.psoc_ops.key_event == NULL) {
		if (ks.in_ask) {
			ks.end_ms[ks.asks - 1u] = now_ms();
			ks.in_ask = false;
		}
		return 0;
	}
	if (!ks.in_ask) {
		ks.in_ask = true;
		ks.start_ms[ks.asks < 4u ? ks.asks : 3u] = now_ms();
		ks.asks++;
		ks.next = 0;
	}
	if (ks.host_sent) {
		/* a host that sends a key bitmap as if from the PSoC, and a
		 * ping, during the confirmation: never read (S-10a.049) */
		const uint8_t forged[S07_BITMAP] = { 0x40 };
		const uint8_t ping = 7;

		ks.host_sent = false;
		host_sends(0x41u, 90, 0x42u, forged, sizeof forged);
		host_sends(s06_route(S06_ADDR_HOST, S06_ADDR_MASTER), 91, 0x01,
			   &ping, 1);
	}
	ks.gap = !ks.gap;
	e = ks.asks <= 4u ? ks.ev[ks.asks - 1u] : NULL;
	if (!ks.gap || e == NULL || e[ks.next].ms == 0u ||
	    now_ms() < ks.start_ms[ks.asks - 1u] + e[ks.next].ms)
		return 0;
	e += ks.next++;
	if (e->down)
		ks.bits[e->key / 8u] = (uint8_t)(ks.bits[e->key / 8u] | (1u << (e->key % 8u)));
	else
		ks.bits[e->key / 8u] = (uint8_t)(ks.bits[e->key / 8u] & ~(1u << (e->key % 8u)));
	n = s06_frame_encode(0x41u, (uint8_t)ks.next, 0x42u, ks.bits, sizeof ks.bits,
			     crc8, frame);
	if (n > max)
		return 0;
	memcpy(buf, frame, n);
	return n;
}

static const struct s06_port_ops psoc_port = { psoc_read, port_room, port_write };

/* The master for a whole install: the host on CDC, the Nordic's link with
 * S-08 on it, S-07 on the scripted PSoC, the run's ops as held mode sets
 * them, the clock fast (50 us a read), the package built. */
static void whole_setup(void)
{
	static const uint8_t no_keys[S07_BITMAP];

	host_setup();
	opxy_install_ops(&m, &m.sv.run, &m.sv.step);
	m.link[S06_LINK_PSOC].ops = &psoc_port;
	m.psoc_ops.ctx = &m;
	m.psoc_ops.key_event = NULL;
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	opxy_nordic_ops(&m, &m.nordic_ops);
	s08_init(&m.nordic, &m.node, S06_LINK_NORDIC, &m.nordic_ops,
		 S08_STATE_IDLE);
	m.key_status.replied = true;
	memset(&ks, 0, sizeof ks);
	memcpy(ks.bits, no_keys, sizeof ks.bits);
	wire_n = 0;
	nordic_n = 0;
	cdc_n = 0;
	fake_core_set_cycle_step(50u * OPXY_CCLK_MHZ);
	package_make();
}

static void whole_end(void)
{
	fake_core_set_cycle_step(1);
	opxy_usb_stop();
}

/* One install of the package (option on or off; fw_command: over the
 * firmware command, as a host's transfer submits it). */
static void whole_install(bool option, bool fw_command,
			  struct s11_install_result *res)
{
	const struct wire_source src = { pkg_src_read, NULL, pkg_src_write };
	const struct s11_check_in in = { .form = S11_FORM_BODY,
					 .size = pkg_len, .option = option,
					 .fw_command = fw_command };
	const uint8_t crc_off = opxy_crc_off;

	opxy_crc_off = (uint8_t)((1u << OPXY_CRCS) - 1u);	/* software */
	ks.asks = 0;
	ks.in_ask = false;
	memset(ks.start_ms, 0, sizeof ks.start_ms);
	memset(ks.end_ms, 0, sizeof ks.end_ms);
	opxy_install_source(&m, &src, &in, res);
	opxy_crc_off = crc_off;
	/* the end of the last confirmation, if the run never polled after */
	if (ks.in_ask) {
		ks.end_ms[ks.asks - 1u] = now_ms();
		ks.in_ask = false;
	}
	package_make();		/* the zip was recovered in place */
}

/* Where text first stands on CDC, or cdc_n. */
static size_t cdc_at(const char *text)
{
	const size_t n = strlen(text);

	for (size_t i = 0; i + n <= cdc_n; i++)
		if (memcmp(cdc_wire + i, text, n) == 0)
			return i;
	return cdc_n;
}

/* A frame with this id (and first payload byte, unless < 0) on a wire. */
static bool frame_on(const uint8_t *w, size_t n, uint8_t id, int first)
{
	uint8_t raw[256];
	size_t len;

	for (size_t k = 0; (len = t_nth_raw(w, n, k, raw)) != 0; k++)
		if (len >= 3u && raw[2] == id &&
		    (first < 0 || (len >= 5u && raw[4] == (uint8_t)first)))
			return true;
	return false;
}

static bool first_stage_is(const uint8_t *img, size_t len)
{
	return memcmp(flash_mem + S02_FIRST_STAGE_OFF, img, len) == 0;
}

static uint8_t flash_before[S02_FLASH_SIZE];

/* The flash's protected regions as before the install: both slots, their
 * records and the selector, and the first stage. */
static bool slots_untouched(void)
{
	return memcmp(flash_mem + S02_SELECTOR_OFF,
		      flash_before + S02_SELECTOR_OFF, 2) == 0 &&
	       memcmp(flash_mem + S02_SLOT1_OFF, flash_before + S02_SLOT1_OFF,
		      S02_SLOT_CAPACITY) == 0 &&
	       memcmp(flash_mem + S02_SLOT0_OFF, flash_before + S02_SLOT0_OFF,
		      S02_SLOT_CAPACITY) == 0;
}

static bool first_stage_untouched(void)
{
	return memcmp(flash_mem + S02_FIRST_STAGE_OFF,
		      flash_before + S02_FIRST_STAGE_OFF, S02_FIRST_STAGE_MAX) == 0;
}

static const struct kev hold_5s[] = {
	{ 1000, K_DIGIT1, true }, { 6500, K_DIGIT1, false }, { 0, 0, false },
};
/* the same, the key still down from the last confirmation: its up first */
static const struct kev rehold_5s[] = {
	{ 200, K_DIGIT1, false }, { 1000, K_DIGIT1, true },
	{ 6500, K_DIGIT1, false }, { 0, 0, false },
};
static const struct kev release_early[] = {
	{ 1000, K_DIGIT1, true }, { 3000, K_DIGIT1, false }, { 0, 0, false },
};
static const struct kev key8[] = {
	{ 500, K_DIGIT8, true }, { 600, K_DIGIT8, false }, { 0, 0, false },
};
static const struct kev nothing[] = { { 0, 0, false } };

/*
 * S-11.073: steps 3, 4 and 5 are this build's switched-off steps. With
 * their images in the package each reports Skipped, sends nothing to the
 * Nordic or the PSoC (no DFU entry is asked, S-11.061 item 2; the PSoC's
 * bootloader is never asked for), and the run completes; the host gets a
 * `[skipped]` label after each step's report (S-10c.106), and the screen
 * names them before the outcome. S-11.096: both protected components
 * confirmed by a 5 s hold of digit 1, each asked once, the bootloader
 * first, and each written only then.
 */
static void test_whole_hold(void)
{
	struct s11_install_result res;

	t_case = "whole: hold";
	CHECK_EQ(opxy_steps_off(), (uint8_t)(S11_STEP_BIT(S11_STEP_BLE) |
					     S11_STEP_BIT(S11_STEP_BLE_BOOTLOADER) |
					     S11_STEP_BIT(S11_STEP_KEYBOARD)));
	whole_setup();
	ks.ev[0] = hold_5s;
	ks.ev[1] = rehold_5s;
	memcpy(flash_before, flash_mem, sizeof flash_before);
	whole_install(true, false, &res);
	CHECK(res.reason == S11_REASON_NONE);
	CHECK_EQ(ks.asks, 2u);
	CHECK(res.run.end == S11_RUN_DONE);
	CHECK(slot1_holds_image());
	CHECK(first_stage_is(v_boot_fs_good, sizeof v_boot_fs_good));
	/* each confirmed when its hold reached 5 s, not before; the key
	 * still down from the first acted for the second only after its up
	 * and a down (S-07.031) */
	CHECK(ks.end_ms[0] - ks.start_ms[0] >= 6000u &&
	      ks.end_ms[0] - ks.start_ms[0] < 6500u);
	CHECK(ks.end_ms[1] - ks.start_ms[1] >= 6000u &&
	      ks.end_ms[1] - ks.start_ms[1] < 6500u);
	CHECK(cdc_has("Install the bootloader?") &&
	      cdc_has("Install the first stage?"));
	CHECK(cdc_at("Install the bootloader?") < cdc_at("Install the first stage?"));
	CHECK(cdc_has("install: bootloader confirmed by a 5 s hold") &&
	      cdc_has("install: first stage confirmed by a 5 s hold"));
	/* the switched-off steps: Skipped, each after its own report */
	CHECK(cdc_has("[skipped] ble: switched off") &&
	      cdc_has("[skipped] ble bootloader: switched off") &&
	      cdc_has("[skipped] backplane: switched off"));
	CHECK(cdc_at("[ble]") < cdc_at("[skipped] ble: switched off") &&
	      cdc_at("[skipped] ble: switched off") < cdc_at("[ble bootloader]") &&
	      cdc_at("[backplane]") < cdc_at("[skipped] backplane: switched off") &&
	      cdc_at("[skipped] backplane: switched off") < cdc_at("[spiboot]"));
	CHECK(!cdc_has("[skipped] bootloader") && !cdc_has("[skipped] spiboot"));
	CHECK(!cdc_has("no PSoC unlock") && !cdc_has("not in update mode"));
	CHECK(cdc_has("Not updated: BLE, BLE bootloader, Backplane"));
	CHECK(cdc_has("Done!"));
	/* nothing to the Nordic but the run's queries: no DFU entry (0x0D
	 * 0xEE), no transfer; nothing for the PSoC's bootloader (0xEE) */
	CHECK(!frame_on(nordic_wire, nordic_n, 0x0Du, 0xEE));
	CHECK(!frame_on(wire, wire_n, 0xEEu, -1));
	whole_end();
}

/* An early release empties the bar and does not confirm; the 60 s run out
 * and the bootloader is skipped, while the first stage, held in full, is
 * written: each protected step writes after its own confirmation alone. */
static void test_whole_release_timeout(void)
{
	struct s11_install_result res;

	t_case = "whole: release, then the time runs out";
	whole_setup();
	ks.ev[0] = release_early;
	ks.ev[1] = hold_5s;
	memcpy(flash_before, flash_mem, sizeof flash_before);
	whole_install(true, false, &res);
	CHECK_EQ(ks.asks, 2u);
	CHECK(ks.end_ms[0] - ks.start_ms[0] >= 59900u &&
	      ks.end_ms[0] - ks.start_ms[0] < 61000u);
	CHECK(cdc_has("install: bootloader not confirmed in 60 s"));
	CHECK(slots_untouched());
	CHECK(first_stage_is(v_boot_fs_good, sizeof v_boot_fs_good));
	CHECK(res.run.end == S11_RUN_DONE);
	CHECK(cdc_has("[skipped] bootloader: not confirmed"));
	CHECK(cdc_at("[bootloader]") < cdc_at("[skipped] bootloader: not confirmed"));
	CHECK(!cdc_has("[skipped] spiboot"));
	CHECK(cdc_has("Not updated: Bootloader, BLE, BLE bootloader,") &&
	      cdc_has("   Backplane"));
	whole_end();
}

/* Key 8 skips at once: here the first stage, after the bootloader was
 * confirmed and written. */
static void test_whole_key8(void)
{
	struct s11_install_result res;

	t_case = "whole: key 8";
	whole_setup();
	ks.ev[0] = hold_5s;
	ks.ev[1] = key8;
	memcpy(flash_before, flash_mem, sizeof flash_before);
	whole_install(true, false, &res);
	CHECK_EQ(ks.asks, 2u);
	CHECK(ks.end_ms[1] - ks.start_ms[1] < 1000u);
	CHECK(cdc_has("install: first stage skipped with key 8"));
	CHECK(slot1_holds_image());
	CHECK(first_stage_untouched());
	CHECK(res.run.end == S11_RUN_DONE);
	CHECK(cdc_has("[skipped] spiboot: not confirmed"));
	CHECK(!cdc_has("[skipped] bootloader"));
	whole_end();
}

/* A key down when the confirmation is shown (still held from the last)
 * does not count: held through the whole 60 s, the first stage is not
 * confirmed. */
static void test_whole_stale(void)
{
	struct s11_install_result res;

	t_case = "whole: a key held from before";
	whole_setup();
	ks.ev[0] = hold_5s;
	ks.ev[1] = nothing;
	memcpy(flash_before, flash_mem, sizeof flash_before);
	whole_install(true, false, &res);
	CHECK_EQ(ks.asks, 2u);
	CHECK((m.psoc.keys[K_DIGIT1 / 8u] & (1u << (K_DIGIT1 % 8u))) != 0u);
	CHECK(ks.end_ms[1] - ks.start_ms[1] >= 59900u);
	CHECK(slot1_holds_image() && first_stage_untouched());
	CHECK(cdc_has("install: first stage not confirmed in 60 s"));
	whole_end();
}

/* No key: each times out after 60 s, nothing protected is written, and
 * with every other step skipped or not found nothing was installed. */
static void test_whole_timeout(void)
{
	struct s11_install_result res;

	t_case = "whole: no key";
	whole_setup();
	ks.ev[0] = nothing;
	ks.ev[1] = nothing;
	memcpy(flash_before, flash_mem, sizeof flash_before);
	whole_install(true, false, &res);
	CHECK_EQ(ks.asks, 2u);
	CHECK(ks.end_ms[0] - ks.start_ms[0] >= 59900u &&
	      ks.end_ms[1] - ks.start_ms[1] >= 59900u);
	CHECK(slots_untouched() && first_stage_untouched());
	CHECK(res.run.end == S11_RUN_NOTHING_TO_DO);
	CHECK(cdc_has("[skipped] bootloader: not confirmed") &&
	      cdc_has("[skipped] spiboot: not confirmed"));
	CHECK(cdc_has("Nothing was installed."));
	CHECK(!res.stock);
	whole_end();
}

/* Keys that cannot be read (S-07.032 had no reply): both skipped at once,
 * with no wait. */
static void test_whole_unreadable(void)
{
	struct s11_install_result res;
	uint64_t t0;

	t_case = "whole: keys unreadable";
	whole_setup();
	m.key_status.replied = false;
	ks.ev[0] = hold_5s;
	ks.ev[1] = hold_5s;
	memcpy(flash_before, flash_mem, sizeof flash_before);
	t0 = now_ms();
	whole_install(true, false, &res);
	CHECK(cdc_has("install: bootloader not confirmed: keys not readable") &&
	      cdc_has("install: first stage not confirmed: keys not readable"));
	CHECK(slots_untouched() && first_stage_untouched());
	CHECK(cdc_has("[skipped] bootloader: not confirmed") &&
	      cdc_has("[skipped] spiboot: not confirmed"));
	CHECK(ks.asks == 0u);		/* no key was read for it */
	CHECK(now_ms() - t0 < 30000u);	/* the run's own waits, no 60 s */
	whole_end();
}

/* The option off: nothing is asked, and both protected steps report the
 * option's reason. */
static void test_whole_option_off(void)
{
	struct s11_install_result res;

	t_case = "whole: option off";
	whole_setup();
	ks.ev[0] = hold_5s;
	memcpy(flash_before, flash_mem, sizeof flash_before);
	whole_install(false, false, &res);
	CHECK_EQ(ks.asks, 0u);
	CHECK(!cdc_has("Install the bootloader?") &&
	      !cdc_has("Install the first stage?"));
	CHECK(slots_untouched() && first_stage_untouched());
	CHECK(cdc_has("[skipped] bootloader: option off") &&
	      cdc_has("[skipped] spiboot: option off"));
	CHECK(res.run.end == S11_RUN_NOTHING_TO_DO);
	whole_end();
}

/*
 * An install submitted over a host link (the firmware command's form,
 * forced) asks the same: nothing the host sends during it is read, a key
 * bitmap forged as the PSoC's included, so the host cannot answer; the
 * bootloader's time runs out. Then a second install of the same package
 * asks again: nothing of the first's answers is kept.
 */
static void test_whole_host_and_again(void)
{
	struct s11_install_result res;

	t_case = "whole: from a host link, then again";
	whole_setup();
	ks.ev[0] = nothing;
	ks.ev[1] = hold_5s;
	ks.host_sent = true;
	memcpy(flash_before, flash_mem, sizeof flash_before);
	whole_install(true, true, &res);
	CHECK(res.reason == S11_REASON_NONE && res.forced);
	CHECK_EQ(ks.asks, 2u);
	CHECK(host_rx_n != 0u && host_rx_r == 0u);	/* sent, never read */
	CHECK(slots_untouched());
	CHECK(first_stage_is(v_boot_fs_good, sizeof v_boot_fs_good));
	CHECK(cdc_has("[skipped] bootloader: not confirmed"));

	/* the same package again: asked again, both this time */
	cdc_n = 0;
	ks.ev[0] = rehold_5s;
	ks.ev[1] = key8;
	memcpy(flash_before, flash_mem, sizeof flash_before);
	whole_install(true, true, &res);
	CHECK_EQ(ks.asks, 2u);
	CHECK(cdc_has("Install the bootloader?") &&
	      cdc_has("Install the first stage?"));
	CHECK(slot1_holds_image());
	CHECK(cdc_has("[skipped] spiboot: not confirmed"));
	whole_end();
}

int main(void)
{
	vol = calloc(1, VOL_LEN);
	if (vol == NULL)
		return 2;
	test_no_stated_values();
	test_extracted_intact();
	test_extracted_damaged();
	test_extracted_stale();
	test_os_step();
	test_bridge_flush();
	test_read_fails();
	test_spiboot_step();
	test_link_fast();
	test_link_log();
	test_link_log_end();
	test_usb_service();
	test_log_through_a_run();
	test_host_hold_through_a_run();
	test_loop_pass_alone();
	test_whole_hold();
	test_whole_release_timeout();
	test_whole_key8();
	test_whole_stale();
	test_whole_timeout();
	test_whole_unreadable();
	test_whole_option_off();
	test_whole_host_and_again();
	wire_s09_release(&S);
	wire_yaffs_detach(&Y);
	free(vol);
	return t_finish("test_board_install");
}
