/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The developer host's timing requests (src/board/dev/devtime.c): each
 * stage of OPXY_DEV_BENCH answers its byte count and a time, OPXY_DEV_CHECK
 * runs checks 1-14 on the RAM disk's package (S-11.020, the key_fe vector
 * of packages.txt) without changing a sector of the disk, and
 * OPXY_DEV_EXTRACT runs checks 16-18 on it as well, staging nothing, and
 * times bridge requests that the slave (s09_slave serving wire_s09 on a
 * yaffs2 volume in RAM, over test_board_install's loopback) refuses
 * without touching its volume.
 *
 * The board cannot read a key on the host, so an encrypted package is
 * refused before its zip: the zip reader's decryption is tested on its own,
 * on the key_00 vector and on synthetic ciphertext.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/boot/device.h"
#include "waffle/board/boot.h"
#include "waffle/board/emmc_bridge.h"
#include "waffle/board/services.h"
#include "waffle/util/bytes.h"
#include "waffle/host/devhost.h"
#include "waffle/bridge/slave.h"
#include "waffle/install/common.h"
#include "waffle/fs/fat.h"
#include "waffle/install/crypto.h"
#include "waffle/bridge/slave_files.h"
#include "waffle/fs/yaffs.h"
#include "waffle/install/zip.h"

#define DISK_SECTORS 454656u		/* S-12.001 */
#define SPLIT 100u
#define BLOCKS 40u			/* the slave's yaffs2 volume */
#define VOL_LEN ((size_t)BLOCKS * 128u * 4096u)

static struct opxy_master m;
static uint8_t *disk, *before;
#define CHECK_REPLY (2u + 7u + 8u * OPXY_CHECK_STAGES + 4u)

static uint8_t reply[OPXY_EXTRACT_REPLY];

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static size_t bench(uint8_t stage, uint16_t kib)
{
	const uint8_t req[3] = { stage, (uint8_t)kib, (uint8_t)(kib >> 8) };

	memset(reply, 0, sizeof(reply));
	return opxy_dev_bench(&m, req, sizeof(req), reply);
}

static void test_bench(void)
{
	static const uint8_t stages[] = { OPXY_BENCH_READ, OPXY_BENCH_CRC,
					  OPXY_BENCH_SHA, OPXY_BENCH_AES,
					  OPXY_BENCH_SHA_SOFT,
					  OPXY_BENCH_AES_SOFT };

	for (size_t i = 0; i < sizeof(stages); i++) {
		CHECK(bench(stages[i], 100) == 17u && reply[0] == OPXY_DEV_OK &&
		      reply[1] == stages[i] && le32(reply + 2) == 100u * 1024u &&
		      le64(reply + 6) != 0u && reply[14] == 0u &&
		      reply[15] == 0u && reply[16] == 0u,
		      "bench: stage %u", stages[i]);
	}
	CHECK(bench(OPXY_BENCH_FRAME, 1) == 1u && reply[0] == OPXY_DEV_REFUSED,
	      "bench: no panel, no frame");
	CHECK(bench(OPXY_BENCH_AES_SOFT + 1u, 1) == 1u && reply[0] == OPXY_DEV_BAD,
	      "bench: an unknown stage");
	CHECK(bench(OPXY_BENCH_CRC, 0) == 1u && reply[0] == OPXY_DEV_BAD,
	      "bench: no bytes");
	CHECK(opxy_dev_bench(&m, reply, 2, reply) == 1u && reply[0] == OPXY_DEV_BAD,
	      "bench: a short request");
}

static void test_check(void)
{
	FILE *f = tl_open_vectors("packages.txt");
	struct tl_package p;
	bool found = false;
	uint64_t sum = 0;
	size_t n;

	/* no package on the disk */
	CHECK(wire_fat_format(&m.sv.fat, &m.sv.disk.dev, &wb_device_opxy), "check: format");
	wire_fat_unmount(&m.sv.fat);
	memset(reply, 0, sizeof(reply));
	CHECK(opxy_dev_check(&m, reply) == 1u && reply[0] == OPXY_DEV_REFUSED,
	      "check: no package");

	while (!found && !tl_next_package(f, &p, NULL)) {
		found = !strcmp(p.name, "key_fe");
		if (!found)
			free(p.file);
	}
	fclose(f);
	CHECK(found, "check: the key_fe vector");
	if (!found)
		return;
	CHECK(wire_fat_mount(&m.sv.fat, &m.sv.disk.dev) &&
	      wire_fat_write_file(&m.sv.fat, "fw.tfw", p.file, p.file_len,
				  false, 0), "check: package written");
	wire_fat_unmount(&m.sv.fat);
	memcpy(before, disk, (size_t)DISK_SECTORS * 512u);

	memset(reply, 0, sizeof(reply));
	n = opxy_dev_check(&m, reply);
	CHECK(n == CHECK_REPLY && reply[0] == OPXY_DEV_OK &&
	      reply[1] == S11_REASON_NONE && le32(reply + 2) == p.file_len,
	      "check: checks 1-14 pass (reason %u)", reply[1]);
	for (unsigned i = 0; i < OPXY_CHECK_TOTAL; i++)
		sum += le64(reply + 6u + 8u * i);
	CHECK(le64(reply + 6u + 8u * OPXY_CHECK_READ) != 0u &&
	      le64(reply + 6u + 8u * OPXY_CHECK_CRC) != 0u &&
	      le64(reply + 6u + 8u * OPXY_CHECK_SHA) != 0u &&
	      sum <= le64(reply + 6u + 8u * OPXY_CHECK_TOTAL),
	      "check: the stages' times");
	CHECK(!memcmp(before, disk, (size_t)DISK_SECTORS * 512u),
	      "check: the disk is as it was");

	/* a volume someone else has mounted is left alone */
	m.sv.fat.mounted = true;
	CHECK(opxy_dev_check(&m, reply) == 1u && reply[0] == OPXY_DEV_REFUSED &&
	      m.sv.fat.mounted, "check: refused while the volume is mounted");
	m.sv.fat.mounted = false;

	/* a damaged package is refused as a scan refuses it */
	p.file[p.file_len - 1u] ^= 1u;
	CHECK(wire_fat_mount(&m.sv.fat, &m.sv.disk.dev) &&
	      wire_fat_write_file(&m.sv.fat, "fw.tfw", p.file, p.file_len,
				  false, 0), "check: damaged package written");
	wire_fat_unmount(&m.sv.fat);
	CHECK(opxy_dev_check(&m, reply) == CHECK_REPLY &&
	      reply[1] == S11_REASON_CRC, "check: a flipped bit is CRC (%u)",
	      reply[1]);
	free(p.file);
}

/* ---- the slave: wire_s09 on a yaffs2 volume in RAM, no eMMC ---- */

static struct wb_blockdev bd;
static struct wb_ramdisk ram;
static struct wire_yaffs Y;
static struct wire_s09 S;
static struct s09_slave sl;
static uint8_t *vol, *vol_before;
static unsigned reqs[256];		/* requests by command */
static unsigned wrote;			/* writes the volume took */

static int32_t st_emmc(void *c, uint8_t cmd, uint32_t a0, uint32_t a1,
		       uint8_t *data, uint32_t n)
{
	(void)c;
	(void)cmd;
	(void)a0;
	(void)a1;
	(void)data;
	(void)n;
	return -1;
}

static uint32_t st_erase_group(void *c) { (void)c; return S03_GROUP_SECTORS; }
static uint32_t st_boot_size(void *c) { (void)c; return 0x40000u; }
static uint32_t st_os(void *c, uint32_t len, uint32_t crc)
{
	(void)c;
	(void)len;
	(void)crc;
	return 1;
}
static uint64_t st_cycles(void *c) { (void)c; return 0; }
static void st_adjust(void *c, int64_t d) { (void)c; (void)d; }

static int32_t st_file(void *c, const struct s09_req *r, const char *p1,
		       const char *p2, const uint8_t *d, uint32_t n, uint8_t *o,
		       uint32_t *err)
{
	const int32_t res = wire_s09_file(&S, r, p1, p2, d, n, o, err);

	(void)c;
	if (r->cmd == S09_WRITE && res >= 0)
		wrote++;
	return res;
}

static int32_t st_read(void *c, uint32_t fd, uint8_t *buf, uint32_t n,
		       uint32_t *err)
{
	(void)c;
	return wire_s09_read(&S, fd, buf, n, err);
}

/* S-09.080: the quad reply test's slave end, as the board's keeps it: the
 * arm, the test reply, the record (the end the flag unless no_flag). */
static struct {
	bool armed, due, no_flag;
	uint32_t arg0, len;
	uint32_t rec[S09_QT_REC_WORDS];
	uint8_t small[8], out[S09_QT_REC_LEN];
	_Alignas(4) uint8_t buf[S09_QT_MAX];
} qs;

static size_t st_qtest(void *c, const struct s09_req *r, const uint8_t **out)
{
	const bool armed = qs.armed;

	(void)c;
	qs.armed = qs.due = false;
	*out = qs.small;
	wb_put_le32(qs.small, s09_hdr_word(S09_QUAD_TEST, r->slot));
	wb_put_le32(qs.small + 4, S09_QT_BAD);
	if (S09_QT_OP(r->arg0) == S09_QT_ARM && s09_qt_args_ok(r->arg0, r->arg1) &&
	    s09_check_ready()) {
		qs.armed = true;
		qs.arg0 = r->arg0;
		qs.len = r->arg1;
		wb_put_le32(qs.small + 4, S09_QT_ARMED);
	} else if (r->arg0 == S09_QT_SEND && armed) {
		s09_qt_build(qs.buf, qs.len, r->slot, qs.arg0);
		qs.due = true;
		*out = qs.buf;
		return qs.len;
	} else if (r->arg0 == S09_QT_RECORD) {
		qs.rec[S09_QR_HDR] = s09_hdr_word(S09_QUAD_TEST, r->slot);
		for (unsigned k = 0; k < S09_QT_REC_WORDS; k++)
			wb_put_le32(qs.out + 4u * k, qs.rec[k]);
		*out = qs.out;
		return S09_QT_REC_LEN;
	}
	return 8u;
}

static const struct s09_store_ops store = {
	0, st_erase_group, st_boot_size, NULL /* dev_cmd */, st_emmc, st_file,
	st_read, st_os, st_cycles, st_adjust, NULL /* stage_write */
};

/* ---- the loopback bus (test_board_install's, without its damage) ---- */

/* A virtual clock: a wait moves it on to its end. */
static uint32_t now_us;

static uint32_t clk_now(void *c)
{
	(void)c;
	return now_us;
}

static void clk_idle(void *c, uint32_t until)
{
	(void)c;
	now_us = s06_time_reached(now_us, until) ? now_us + 1u : until;
}

static const struct s06_clock clk = { clk_now, clk_idle, NULL };

static struct {
	bool sel, reply_session, done;
	uint8_t req[70000];
	size_t req_n;
	const uint8_t *rep;
	size_t rep_n, rep_at;
	int phase;			/* the bus's last configure() */
	unsigned quad_sends;		/* request sessions sent on rung 3 or 4 */
} lb;

static bool lb_ready(void *c)
{
	(void)c;
	return lb.sel ? !lb.done : true;
}

static bool lb_attention(void *c)
{
	(void)c;
	return s09_slave_attention(&sl);
}

static void lb_select(void *c, bool on)
{
	(void)c;
	if (on && !lb.sel) {
		lb.reply_session = sl.reply_len != 0;
		lb.done = false;
		lb.req_n = 0;
		lb.rep_at = 0;
		if (lb.reply_session)
			(void)s09_slave_reply(&sl, &lb.rep, &lb.rep_n);
	} else if (!on && lb.sel && !lb.reply_session && lb.req_n >= 12) {
		size_t k = (size_t)s09_slave_rx_len(&sl, lb.req);

		if (lb.req[0] != S09_PULL && lb.req[0] != S09_POLL)
			reqs[lb.req[0]]++;
		s09_slave_request(&sl, lb.req, k < lb.req_n ? k : lb.req_n);
	}
	lb.sel = on;
}

static bool lb_send(void *c, const uint8_t *d, size_t n)
{
	(void)c;
	if (lb.reply_session || lb.req_n + n > sizeof lb.req)
		return false;
	if (lb.phase >= S09_PHASE_RUNG(S09_RUNG_QUAD))
		lb.quad_sends++;
	memcpy(lb.req + lb.req_n, d, n);
	lb.req_n += n;
	lb.done = true;
	return true;
}

static bool lb_recv(void *c, uint8_t *d, size_t n)
{
	(void)c;
	if (!lb.reply_session || lb.rep_at + n > lb.rep_n)
		return false;
	memcpy(d, lb.rep + lb.rep_at, n);
	lb.rep_at += n;
	lb.done = lb.rep_at == lb.rep_n;
	return true;
}

static void lb_configure(void *c, int phase)
{
	(void)c;
	lb.phase = phase;
}

/* S-13b.084 on the master: the test reply's session, whole. */
static unsigned quads;

static bool lb_quad(void *c, uint8_t *d, size_t n, unsigned clock, uint32_t out[4])
{
	(void)c;
	(void)clock;
	quads++;
	if (!lb.reply_session || !qs.due || lb.rep != qs.buf || n != lb.rep_n)
		return false;
	qs.due = false;
	memcpy(d, lb.rep, n);
	lb.rep_at = n;
	lb.done = true;
	memset(qs.rec, 0, sizeof qs.rec);
	qs.rec[S09_QR_ARM] = qs.arg0;
	qs.rec[S09_QR_LEN] = qs.len;
	qs.rec[S09_QR_END] = qs.no_flag ? S09_QE_BOUND | S09_QF_FED : S09_QE_FLAG | S09_QF_TF_ILAT;
	qs.rec[S09_QR_T_FEED] = 100u;
	qs.rec[S09_QR_T_FLAG] = qs.no_flag ? S09_QT_NONE : 160u;
	qs.rec[S09_QR_STAT] = 0x00C40801u;
	out[0] = 1u << 10;
	out[1] = 0u;
	out[2] = 1234u;
	out[3] = 1u;
	return true;
}

static const struct s09_bus_ops bus = {
	0, lb_ready, lb_attention, lb_select, lb_send, lb_recv, lb_configure, NULL,
};

/* The master's bridge over the loopback, the slave's volume mounted. */
static bool bridge_up(void)
{
	vol = calloc(1, VOL_LEN);
	vol_before = malloc(VOL_LEN);
	if (vol == NULL || vol_before == NULL)
		return false;
	wb_ramdisk_init(&bd, &ram, vol, (uint32_t)(VOL_LEN / 512u));
	if (!wire_yaffs_add(&Y, "/yaffs2", &bd, 0, BLOCKS - 1))
		return false;
	wire_s09_init(&S, &Y);
	s09_slave_init(&sl, &store);
	s09_slave_qtest_source(&sl, st_qtest, NULL);
	s09_master_init(&m.bridge, &bus, &clk);
	s09_master_quad_recv(&m.bridge, lb_quad, NULL);
	m.link_ceiling = OPXY_LINK_CEILING;	/* as the bridge's start sets it */
	if (!s09_master_start(&m.bridge))
		return false;
	m.slave_present = true;
	s09_files_init(&m.sv.files, &m.bridge);
	return s09_files_mount(&m.sv.files) == 0;
}

/* ---- OPXY_DEV_EXTRACT ---- */

static bool vector(const char *name, struct tl_package *p)
{
	FILE *f = tl_open_vectors("packages.txt");
	bool found = false;

	while (!found && !tl_next_package(f, p, NULL)) {
		found = !strcmp(p->name, name);
		if (!found)
			free(p->file);
	}
	fclose(f);
	return found;
}

static bool on_disk(const struct tl_package *p)
{
	bool ok = wire_fat_format(&m.sv.fat, &m.sv.disk.dev, &wb_device_opxy);

	wire_fat_unmount(&m.sv.fat);
	ok = ok && wire_fat_mount(&m.sv.fat, &m.sv.disk.dev) &&
	     wire_fat_write_file(&m.sv.fat, "fw.tfw", p->file, p->file_len,
				 false, 0);
	wire_fat_unmount(&m.sv.fat);
	return ok;
}

/* What a zip holds, read straight from the package's bytes. */
struct held {
	uint32_t files, dirs, min_reqs;
	uint64_t bytes;
};

static struct held zip_holds(const uint8_t *zip, size_t len)
{
	static struct wire_zip z;
	struct s11_zip_ops ops;
	struct s11_member mb;
	struct held h = { 0, 0, 0, 0 };

	wire_zip_init(&z, &ops, NULL, zip, len, NULL);
	for (size_t i = 0; ops.member(ops.ctx, i, &mb); i++) {
		if (mb.name[strlen(mb.name) - 1u] == '/') {
			h.dirs++;
		} else {
			h.files++;
			h.bytes += mb.size;
			/* on a rung a write takes S09_WRITE_MAX bytes */
			h.min_reqs += (mb.size + S09_WRITE_MAX - 1u) / S09_WRITE_MAX;
		}
	}
	wire_zip_close(&z);
	return h;
}

static uint32_t count(unsigned i)
{
	return le32(reply + 3u + 4u * i);
}

static uint64_t stage(unsigned i)
{
	return le64(reply + 3u + 4u * OPXY_EXTRACT_COUNTS + 8u + 8u * i);
}

static const uint8_t *batch(unsigned i)
{
	return reply + 3u + 4u * OPXY_EXTRACT_COUNTS + 8u +
	       8u * OPXY_EXTRACT_STAGES + 16u * i;
}

/* Zeros from there to the link's four bytes, the gap's four and the link's
 * two after, which say where the link is and how USB fared whatever the
 * request did. */
static bool zeros_after(size_t from)
{
	for (size_t i = from; i < sizeof(reply) - 10u; i++) {
		if (reply[i] != 0u)
			return false;
	}
	return true;
}

/* Every request but the link's own (S-09.070, S-09.071). */
static unsigned all_reqs(void)
{
	unsigned n = 0;

	for (unsigned i = 0; i < 256u; i++) {
		if (i != S09_LINK_MODE && i != S09_LINK_TEST)
			n += reqs[i];
	}
	return n;
}

static void test_extract(void)
{
	struct tl_package p;
	struct held h;
	uint64_t sum = 0;
	size_t n;

	const bool found = vector("key_fe", &p);

	CHECK(found, "extract: the key_fe vector");
	if (!found)
		return;
	h = zip_holds(p.payload + S11_NAME_BLOCK_LEN, p.zip_len);
	CHECK(h.files == 3u && h.dirs == 0u && h.bytes > 0u,
	      "extract: the vector's zip (%u files)", h.files);
	CHECK(on_disk(&p), "extract: package written");
	memcpy(before, disk, (size_t)DISK_SECTORS * 512u);
	memcpy(vol_before, vol, VOL_LEN);
	memset(reqs, 0, sizeof(reqs));
	wrote = 0;
	lb.quad_sends = 0;

	memset(reply, 0xEE, sizeof(reply));
	reply[0] = OPXY_DEV_OK;		/* as opxy_dev_host() sets it */
	n = opxy_dev_extract(&m, reply);
	CHECK(n == OPXY_EXTRACT_REPLY && reply[0] == OPXY_DEV_OK &&
	      reply[1] == S11_REASON_NONE && reply[2] == 0u,
	      "extract: checks 16-18 pass (reason %u, flags %02x)", reply[1],
	      reply[2]);
	CHECK(count(OPXY_EXTRACT_FILES) == h.files &&
	      count(OPXY_EXTRACT_DIRS) == h.dirs &&
	      le64(reply + 3u + 4u * OPXY_EXTRACT_COUNTS) == h.bytes,
	      "extract: the members and bytes the zip holds");
	CHECK(count(OPXY_EXTRACT_WRITES) >= h.files &&
	      count(OPXY_EXTRACT_WRITE_REQS) >= h.min_reqs &&
	      count(OPXY_EXTRACT_WRITE_REQS) >= count(OPXY_EXTRACT_WRITES),
	      "extract: the staging writes (%u in %u requests)",
	      count(OPXY_EXTRACT_WRITES), count(OPXY_EXTRACT_WRITE_REQS));
	/* firmware/firmware.version: upgrade (there), upgrade/firmware (new);
	 * firmware/blackfin/bin/os.ldr: two remembered, two new; os.version:
	 * all four remembered. Four mkdirs, one of them followed by a stat. */
	CHECK(count(OPXY_EXTRACT_MKDIRS) == 4u && count(OPXY_EXTRACT_STATS) == 1u,
	      "extract: the directory requests (%u mkdir, %u stat)",
	      count(OPXY_EXTRACT_MKDIRS), count(OPXY_EXTRACT_STATS));
	for (unsigned i = 0; i < OPXY_EXTRACT_TOTAL; i++)
		sum += stage(i);
	CHECK(stage(OPXY_EXTRACT_READ) != 0u && stage(OPXY_EXTRACT_INFLATE) != 0u &&
	      stage(OPXY_EXTRACT_CRC) != 0u && stage(OPXY_EXTRACT_MEMBER) != 0u &&
	      stage(OPXY_EXTRACT_DECRYPT) == 0u && stage(OPXY_EXTRACT_CHECK) != 0u &&
	      sum <= stage(OPXY_EXTRACT_TOTAL),
	      "extract: the stages' times (plaintext: no decryption)");
	CHECK(le32(batch(OPXY_PROBE_WRITE)) == OPXY_PROBE_N &&
	      le64(batch(OPXY_PROBE_WRITE) + 4) != 0u &&
	      le32(batch(OPXY_PROBE_WRITE) + 12) == S09_EBADF,
	      "extract: the writes, refused EBADF (%u)",
	      le32(batch(OPXY_PROBE_WRITE) + 12));
	CHECK(le32(batch(OPXY_PROBE_STAT)) == OPXY_PROBE_N &&
	      le64(batch(OPXY_PROBE_STAT) + 4) != 0u &&
	      le32(batch(OPXY_PROBE_STAT) + 12) == 0u,
	      "extract: the stats answered (%u)", le32(batch(OPXY_PROBE_STAT) + 12));
	CHECK(reqs[S09_WRITE] == OPXY_PROBE_N && reqs[S09_STAT] == OPXY_PROBE_N &&
	      all_reqs() == 2u * OPXY_PROBE_N && wrote == 0u,
	      "extract: no request but the batches, no write taken (%u)",
	      all_reqs());
	CHECK(!memcmp(before, disk, (size_t)DISK_SECTORS * 512u),
	      "extract: the disk is as it was");
	CHECK(!memcmp(vol_before, vol, VOL_LEN), "extract: the volume is as it was");
	/* the link was brought up as an install's extraction would have it:
	 * to the board's ceiling, single I/O at 50 MHz, no quad request */
	{
		const uint8_t *link = batch(OPXY_PROBE_BATCHES) + 2;

		CHECK(lb.quad_sends == 0u, "extract: no quad request (%u)", lb.quad_sends);
		CHECK(link[0] == S09_RUNG_CHECKED_FAST && link[1] == 0u &&
		      link[2] == S09_WRITE_MAX / 1024u && reqs[S09_LINK_TEST] != 0u,
		      "extract: the link's rung %u, fell %u, %u KiB a write",
		      link[0], link[1], link[2]);
		/* the rungs the negotiation tried */
		CHECK(link[7] == 1u, "extract: tried %u", link[7]);
	}
	/* ... and lowered when it ended (S-09.078) */
	CHECK(s09_master_rung(&m.bridge) == S09_RUNG_PLAIN &&
	      s09_slave_mode(&sl) == S09_MODE_PLAIN && m.sv.files.link == 0u &&
	      s09_master_fell(&m.bridge) == 0u, "extract: the link plain after");

	/* the slave absent: the extraction, no batch */
	m.slave_present = false;
	memset(reqs, 0, sizeof(reqs));
	CHECK(opxy_dev_extract(&m, reply) == OPXY_EXTRACT_REPLY &&
	      reply[1] == S11_REASON_NONE && reply[2] == OPXY_EXTRACT_NO_SLAVE &&
	      count(OPXY_EXTRACT_FILES) == h.files &&
	      zeros_after((size_t)(batch(0) - reply)) && all_reqs() == 0u,
	      "extract: no slave, no batch");
	m.slave_present = true;

	/* a volume someone else has mounted is left alone */
	m.sv.fat.mounted = true;
	CHECK(opxy_dev_extract(&m, reply) == 1u && reply[0] == OPXY_DEV_REFUSED &&
	      m.sv.fat.mounted, "extract: refused while the volume is mounted");
	m.sv.fat.mounted = false;

	/* a long operation holds the keys, or a scan or an install runs: every
	 * timing request is refused, and nothing is sent or mounted */
	memset(reqs, 0, sizeof(reqs));
	for (unsigned held = 0; held < 2u; held++) {
		if (held != 0u)
			s15_menu_hold(&m.sv.menu);
		else
			m.sv.working = true;
		CHECK(opxy_dev_extract(&m, reply) == 1u && reply[0] == OPXY_DEV_REFUSED &&
		      opxy_dev_check(&m, reply) == 1u && reply[0] == OPXY_DEV_REFUSED &&
		      bench(OPXY_BENCH_CRC, 1) == 1u && reply[0] == OPXY_DEV_REFUSED &&
		      !m.sv.fat.mounted && all_reqs() == 0u,
		      "timing requests refused during a long operation (%u)", held);
		s15_menu_release(&m.sv.menu);
		m.sv.working = false;
	}

	/* a damaged package: its reason and zeros, no request */
	p.file[p.file_len - 1u] ^= 1u;
	CHECK(on_disk(&p), "extract: damaged package written");
	memset(reqs, 0, sizeof(reqs));
	memset(reply, 0xEE, sizeof(reply));
	CHECK(opxy_dev_extract(&m, reply) == OPXY_EXTRACT_REPLY &&
	      reply[1] == S11_REASON_CRC && zeros_after(2u) && all_reqs() == 0u,
	      "extract: a flipped bit is CRC and zeros (%u)", reply[1]);
	free(p.file);

	/* no package */
	CHECK(wire_fat_format(&m.sv.fat, &m.sv.disk.dev, &wb_device_opxy), "extract: format");
	wire_fat_unmount(&m.sv.fat);
	CHECK(opxy_dev_extract(&m, reply) == 1u && reply[0] == OPXY_DEV_REFUSED,
	      "extract: no package");
}

/* ---- the link's record and the link bench ---- */

static uint8_t big_reply[249];

static size_t link_test(uint8_t sub, uint8_t arg)
{
	const uint8_t req[2] = { sub, arg };

	memset(big_reply, 0xEE, sizeof(big_reply));
	big_reply[0] = OPXY_DEV_OK;		/* as opxy_dev_host() sets it */
	return opxy_dev_link_test(&m, req, sizeof(req), big_reply);
}

/* OPXY_DEV_LINK_RECORD's parts: the summary, the rungs the last
 * negotiation tried, the fall-backs; each within an S-06 payload. */
static void test_link_record(void)
{
	const struct s09_record *r = s09_master_record(&m.bridge);
	size_t n;

	n = opxy_bridge_record(&m.bridge, 0, big_reply);
	CHECK(n == 23u && big_reply[0] == 0u &&
	      big_reply[1] == OPXY_RECORD_PARTS && le32(big_reply + 3) == r->falls &&
	      le32(big_reply + 7) == r->negotiations && big_reply[11] == r->nrungs &&
	      big_reply[12] == r->kept && big_reply[13] == S09_RUNG_PLAIN &&
	      big_reply[18] == 1u && le32(big_reply + 19) == r->bench_faults,
	      "record: its summary, settled (%zu bytes)", n);
	n = opxy_bridge_record(&m.bridge, 1, big_reply);
	CHECK(big_reply[2] == (r->nrungs < 2u ? r->nrungs : 2u) &&
	      n == 3u + 72u * big_reply[2] && big_reply[3] == r->rung[0].rung &&
	      big_reply[4] == r->rung[0].result &&
	      le32(big_reply + 3 + 24) == r->rung[0].big_us[0] &&
	      le32(big_reply + 3 + 32) == r->rung[0].big_us[2] &&
	      le32(big_reply + 3 + 36) == r->rung[0].plain_us,
	      "record: the first rung tried (%zu bytes)", n);
	for (unsigned p = 0; p < OPXY_RECORD_PARTS; p++)
		CHECK(1u + opxy_bridge_record(&m.bridge, p, big_reply) <= 249u,
		      "record: part %u fits", p);
	/* a fall-back from rung 4, then its part: the ceiling at the top */
	m.link_ceiling = S09_RUNG_TOP;
	opxy_bridge_fast(&m);
	s09_slave_link_fault(&sl);
	{
		struct s09_stat st;

		CHECK(s09_files_stat(&m.sv.files, "/yaffs2", &st) == 0, "record: a request");
	}
	CHECK(r->falls == 1u, "record: one fall-back (%u)", r->falls);
	n = opxy_bridge_record(&m.bridge, 1u + OPXY_RECORD_RUNG_PARTS, big_reply);
	CHECK(big_reply[2] == 1u && n == 3u + 73u && big_reply[3] == S09_RUNG_TOP &&
	      big_reply[4] == S09_RUNG_QUAD && big_reply[11] != 0u && big_reply[44] == 1u &&
	      big_reply[49] == S09_WHY_RECEIVE,
	      "record: the fall-back's part (%zu bytes, at %u)", n, big_reply[11]);
	opxy_bridge_plain(&m);
}

static void test_link_bench(void)
{
	const struct s09_record *r = s09_master_record(&m.bridge);
	const uint32_t negs = r->negotiations, falls = r->falls;
	const uint8_t ceiling = m.bridge.ceiling;
	size_t n;

	CHECK(link_test(OPXY_LINK_TEST_PART, 0) == 1u && big_reply[0] == OPXY_DEV_REFUSED,
	      "link test: no table before a run");
	CHECK(link_test(7, 0) == 1u && big_reply[0] == OPXY_DEV_BAD, "link test: no such sub-op");
	s15_menu_hold(&m.sv.menu);
	CHECK(link_test(OPXY_LINK_TEST_RUN, 1) == 1u && big_reply[0] == OPXY_DEV_REFUSED,
	      "link test: refused during a long operation");
	s15_menu_release(&m.sv.menu);
	CHECK(link_test(OPXY_LINK_TEST_RUN, 0) == 1u && big_reply[0] == OPXY_DEV_BAD,
	      "link test: no requests is refused");
	memset(reqs, 0, sizeof(reqs));
	n = link_test(OPXY_LINK_TEST_RUN, 2);
	CHECK(n == OPXY_LINK_TEST_REPLY && big_reply[0] == OPXY_DEV_OK &&
	      big_reply[1] == OPXY_LINK_TEST_PARTS && big_reply[2] == 2u && big_reply[3] == 0u &&
	      big_reply[22] == 0u && big_reply[23] == 0u &&
	      big_reply[24] == (uint8_t)OPXY_LINK_TEST_LIMIT_S && big_reply[25] == 0u &&
	      big_reply[26] == 0u && le32(big_reply + 34) == 0u,
	      "link test: run (%zu bytes, flags %02x)", n, big_reply[3]);
	CHECK(s09_master_settled(&m.bridge), "link test: the slave settled");
	/* one pass: each rung entered once */
	for (unsigned rung = 1; rung <= S09_RUNG_TOP; rung++)
		CHECK(big_reply[4u + rung] == S09_RR_KEPT && big_reply[9u + rung] == 1u,
		      "link test: rung %u entered once", rung);
	/* nothing sent but the link's own requests */
	{
		unsigned others = 0;

		for (unsigned c = 0; c < 256u; c++)
			others += c != S09_ECHO && c != S09_LINK_MODE && c != S09_LINK_TEST &&
				  c != S09_LINK_STATUS ? reqs[c] : 0u;
		CHECK(others == 0u && wrote == 0u, "link test: no other request (%u)", others);
	}
	/* plain after it, the boot's state as it was */
	CHECK(s09_master_rung(&m.bridge) == S09_RUNG_PLAIN &&
	      s09_slave_mode(&sl) == S09_MODE_PLAIN && m.sv.files.link == 0u &&
	      r->negotiations == negs && r->falls == falls && m.bridge.ceiling == ceiling,
	      "link test: plain after, the record untouched");
	/* the table: 35 rows in 5 parts, each row's sends and passes */
	{
		unsigned rows = 0, good = 0;

		for (unsigned p = 0; p < OPXY_LINK_TEST_PARTS; p++) {
			n = link_test(OPXY_LINK_TEST_PART, (uint8_t)p);
			CHECK(big_reply[0] == OPXY_DEV_OK && big_reply[1] == p &&
			      n == 4u + 33u * big_reply[3] && n <= 249u,
			      "link test: part %u (%zu bytes)", p, n);
			for (unsigned i = 0; i < big_reply[3]; i++) {
				const uint8_t *row = big_reply + 4u + 33u * i;

				rows++;
				good += row[5] == 2u && row[7] == 2u;
			}
		}
		CHECK(rows == OPXY_LINK_TEST_ROWS && good == OPXY_LINK_TEST_ROWS,
		      "link test: %u rows, %u good", rows, good);
		n = link_test(OPXY_LINK_TEST_PART, 0);
		CHECK(n >= 4u + 33u && big_reply[4] == 1u && le32(big_reply + 5) == 12u,
		      "link test: the first row, rung 1, 12 bytes");
	}
	CHECK(link_test(OPXY_LINK_TEST_PART, OPXY_LINK_TEST_PARTS) == 1u &&
	      big_reply[0] == OPXY_DEV_BAD, "link test: no such part");
	/* the record's part for its faults: none in this run; one written in
	 * as a run keeps it, read back field by field */
	CHECK(r->bench_faults == 0u && opxy_bridge_record(&m.bridge, 11, big_reply) == 3u &&
	      big_reply[2] == 0u, "link test: no fault in the record");
	{
		struct s09_rec_bench *e = &m.bridge.rec.bench[0];

		memset(e, 0, sizeof *e);
		e->no = 0;
		e->kind = S09_BF_REQUEST;
		e->rung = 2;
		e->len = 12;
		e->req = 1;
		e->to = S09_REC_ABSENT;
		e->fall_us = 0x01020304u;
		e->fault.at = S09_AT_SEND;
		e->fault.words[3] = 6u;
		e->fault.more[0] = 0xA5A5A5A5u;
		e->echo.at = S09_AT_SEND;
		e->echo.more[3] = 0x5A5A5A5Au;
		e->st.told = true;
		e->st.why = S09_WHY_SHORT;
		m.bridge.rec.bench_faults = 1;
		n = opxy_bridge_record(&m.bridge, 11, big_reply);
		CHECK(n == 3u + 114u && big_reply[2] == 1u && big_reply[5] == S09_BF_REQUEST &&
		      big_reply[6] == 2u && le32(big_reply + 8) == 12u &&
		      big_reply[12] == 1u && big_reply[14] == S09_REC_ABSENT &&
		      le32(big_reply + 15) == 0x01020304u && big_reply[19] == S09_AT_SEND &&
		      le32(big_reply + 32) == 6u && le32(big_reply + 36) == 0xA5A5A5A5u &&
		      big_reply[52] == S09_AT_SEND && le32(big_reply + 81) == 0x5A5A5A5Au &&
		      big_reply[85] == 1u && big_reply[90] == S09_WHY_SHORT,
		      "link test: a fault's bytes in the record (%zu)", n);
		CHECK(opxy_bridge_record(&m.bridge, 12, big_reply) == 3u,
		      "link test: the next part empty");
		m.bridge.rec.bench_faults = 0;
	}
	/* an install after it negotiates as it would have: below the rung the
	 * record's fall-back struck */
	opxy_bridge_fast(&m);
	CHECK(s09_master_rung(&m.bridge) == S09_RUNG_QUAD && ceiling == S09_RUNG_QUAD,
	      "link test: raised again after");
	opxy_bridge_plain(&m);
	/* the board's own ceiling */
	m.link_ceiling = OPXY_LINK_CEILING;
	lb.quad_sends = 0;
	opxy_bridge_fast(&m);
	CHECK(s09_master_rung(&m.bridge) == S09_RUNG_CHECKED_FAST && lb.quad_sends == 0u,
	      "link test: an install after it at the board's ceiling");
	opxy_bridge_plain(&m);
}

/* ---- the zip reader ---- */

static uint32_t crc_of;

static bool crc_sink(void *arg, const uint8_t *p, size_t n)
{
	const struct s11_zip_ops *ops = arg;

	crc_of = ops->crc32(ops->ctx, crc_of, p, n);
	return true;
}

/* key_00's zip read through the reader in odd pieces has the digest of the
 * decrypted zip, and minizip reads every member to its stated CRC-32. */
static void test_reader_vector(void)
{
	static const size_t sizes[] = { 1, 15, 16, 17, 33, 100, 255, 4097 };
	static struct wire_zip z;
	static struct wire_package view;
	struct tl_package p;
	struct opxy_zip_reader rd;
	struct wire_mem mem;
	struct wire_crypto c;
	struct s11_zip_ops ops;
	struct s11_member mb;
	uint8_t buf[4097], digest[32];
	uint32_t at, off = 0;
	unsigned members = 0, good = 0;
	bool ok = true;

	const bool found = vector("key_00", &p);

	CHECK(found, "reader: the key_00 vector");
	if (!found)
		return;
	CHECK(p.has_key && p.has_zip_sha && p.key_index == 0u,
	      "reader: key_00 is encrypted");
	mem.buf = p.file;
	mem.len = p.file_len;
	at = (uint32_t)(p.payload - p.file) + S11_NAME_BLOCK_LEN;
	rd.read = wire_mem_read;
	rd.ctx = &mem;
	rd.encrypted = true;
	memcpy(rd.key, p.key, sizeof(rd.key));
	memcpy(rd.iv, p.file + at - 16u, sizeof(rd.iv));	/* the name block's last */
	rd.at = at;
	rd.len = p.zip_len;

	wire_crypto_ops.sha256_init(&c);
	for (unsigned i = 0; off < p.zip_len; i++) {
		size_t k = sizes[i % (sizeof(sizes) / sizeof(sizes[0]))];

		if (k > p.zip_len - off)
			k = p.zip_len - off;
		ok = opxy_zip_read(&rd, at + off, buf, k) && ok;
		wire_crypto_ops.sha256_update(&c, buf, k);
		off += (uint32_t)k;
	}
	wire_crypto_ops.sha256_final(&c, digest);
	CHECK(ok && !memcmp(digest, p.zip_sha, 32),
	      "reader: the zip decrypted in odd pieces");
	CHECK(opxy_zip_read(&rd, at + p.zip_len - 1u, buf, 1) &&
	      !opxy_zip_read(&rd, at + p.zip_len - 1u, buf, 2) &&
	      !opxy_zip_read(&rd, at - 1u, buf, 1),
	      "reader: nothing outside the zip");

	memset(&view, 0, sizeof(view));
	view.src.read = opxy_zip_read;
	view.src.ctx = &rd;
	view.zip_at = at;
	view.zip_len = p.zip_len;
	wire_zip_init(&z, &ops, &view, NULL, 0, NULL);
	for (size_t i = 0; ops.member(ops.ctx, i, &mb); i++) {
		members++;
		crc_of = 0;
		if (ops.extract(ops.ctx, i, crc_sink, &ops) && crc_of == mb.crc32)
			good++;
	}
	wire_zip_close(&z);
	CHECK(members > 0u && good == members,
	      "reader: minizip extracts every member (%u of %u)", good, members);
	free(p.file);
}

/* Synthetic ciphertext decrypted in one pass is what the reader gives at
 * any offset, across its 32 KiB pieces too. */
static void test_reader_synthetic(void)
{
	enum { LEN = 100001, AT = 0x480 };
	static uint8_t pkg[AT + LEN + 15], plain[LEN + 15], got[LEN];
	static const uint32_t from[] = { 0, 1, 15, 16, 17, 4095, 32768, 32769,
					 50000 };
	struct opxy_zip_reader rd;
	struct wire_mem mem;
	struct wire_crypto c;
	uint8_t iv[16];
	unsigned good = 0;
	uint32_t x = 1;

	for (size_t i = 0; i < sizeof(pkg); i++) {
		x = x * 1103515245u + 12345u;
		pkg[i] = (uint8_t)(x >> 16);
	}
	mem.buf = pkg;
	mem.len = sizeof(pkg);
	rd.read = wire_mem_read;
	rd.ctx = &mem;
	rd.encrypted = true;
	for (unsigned i = 0; i < 32u; i++)
		rd.key[i] = (uint8_t)(i * 7u + 3u);
	for (unsigned i = 0; i < 16u; i++)
		rd.iv[i] = (uint8_t)(0xA0u + i);
	rd.at = AT;
	rd.len = LEN;
	memcpy(iv, rd.iv, sizeof(iv));
	wire_crypto_ops.aes256_cbc_decrypt(&c, rd.key, iv, pkg + AT, plain,
					   sizeof(plain));
	for (size_t i = 0; i < sizeof(from) / sizeof(from[0]); i++) {
		const size_t n = LEN - from[i] < 70000u ? LEN - from[i] : 70000u;

		memset(got, 0, n);
		if (opxy_zip_read(&rd, AT + from[i], got, n) &&
		    !memcmp(got, plain + from[i], n))
			good++;
	}
	CHECK(good == sizeof(from) / sizeof(from[0]),
	      "reader: synthetic reads match (%u)", good);
	rd.encrypted = false;
	CHECK(opxy_zip_read(&rd, AT + 5u, got, 1000) &&
	      !memcmp(got, pkg + AT + 5u, 1000),
	      "reader: a plaintext zip passes through");
}

/* ---- OPXY_DEV_QUAD_REPLY: the quad reply test, one cell ---- */

static size_t quad_req(uint8_t cand, uint8_t ren, uint8_t clock, uint32_t len,
		       uint16_t n, size_t req_n)
{
	uint8_t req[9];

	req[0] = cand;
	req[1] = ren;
	req[2] = clock;
	wb_put_le32(req + 3, len);
	req[7] = (uint8_t)n;
	req[8] = (uint8_t)(n >> 8);
	memset(big_reply, 0xEE, sizeof(big_reply));
	big_reply[0] = OPXY_DEV_OK;
	return opxy_dev_quad_reply(&m, req, req_n, big_reply);
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

/* The reply's layout (waffle/host/devhost.h), its counts, refusals while busy
 * and for bad arguments, and nothing written: no storage request, the
 * volume as it was, the link plain after. */
static void test_quad_reply(void)
{
	const struct s09_record *r = s09_master_record(&m.bridge);
	const uint32_t falls = r->falls, negs = r->negotiations;
	size_t n;

	CHECK(OPXY_QUAD_REPLY == 168u && OPXY_QUAD_REPLY <= 249u, "quad: the reply fits");
	CHECK(quad_req(1, 2, 0, 12, 1, 9) == 1u && big_reply[0] == OPXY_DEV_BAD,
	      "quad: REN is 0 or 1");
	CHECK(quad_req(1, 0, 0, 12, 4097, 9) == 1u && big_reply[0] == OPXY_DEV_BAD,
	      "quad: at most 4096 trials");
	CHECK(quad_req(1, 0, 0, 12, 1, 6) == 1u && big_reply[0] == OPXY_DEV_BAD,
	      "quad: a short request");
	s15_menu_hold(&m.sv.menu);
	CHECK(quad_req(1, 0, 0, 12, 1, 9) == 1u && big_reply[0] == OPXY_DEV_REFUSED,
	      "quad: refused during a long operation");
	s15_menu_release(&m.sv.menu);
	memset(reqs, 0, sizeof(reqs));
	wrote = 0;
	memcpy(vol_before, vol, VOL_LEN);
	opxy_bridge_fast(&m);			/* lowered first */
	quads = 0;
	n = quad_req(S09_QT_TF_DMA, 0, S09_QT_FMODE, 0x1000u, 0, 7);	/* n left out */
	CHECK(n == OPXY_QUAD_REPLY && big_reply[0] == OPXY_DEV_OK, "quad: a cell (%zu)", n);
	CHECK(big_reply[1] == S09_QT_TF_DMA && big_reply[2] == 0u && big_reply[3] == S09_QT_FMODE &&
	      le32(big_reply + 4) == 0x1000u && le16(big_reply + 8) == OPXY_QUAD_N &&
	      big_reply[10] == 0u, "quad: as asked, flags %02x", big_reply[10]);
	CHECK(le16(big_reply + 12) == OPXY_QUAD_N && le16(big_reply + 14) == OPXY_QUAD_N &&
	      le16(big_reply + 16) == OPXY_QUAD_N && le16(big_reply + 18) == 0u &&
	      le16(big_reply + 24) == 0u && le16(big_reply + 26) == 0u &&
	      le16(big_reply + 28 + 2u * S09_QE_FLAG) == OPXY_QUAD_N,
	      "quad: every reply whole, its flag each");
	CHECK(le32(big_reply + 44) == 60u && le32(big_reply + 48) == 60u && big_reply[52] == 0u &&
	      le32(big_reply + 53) == (1u << 10) && le32(big_reply + 57) == (1u << 10) &&
	      le64(big_reply + 65) != 0u && big_reply[73] == S09_QX_OK,
	      "quad: the delay, the master's RF, no failure");
	CHECK(quads == OPXY_QUAD_N, "quad: %u quad receives", quads);
	{
		unsigned others = 0;

		for (unsigned c = 0; c < 256u; c++)
			others += c != S09_QUAD_TEST && c != S09_ECHO && c != S09_LINK_MODE &&
				  c != S09_LINK_TEST && c != S09_LINK_STATUS ? reqs[c] : 0u;
		CHECK(others == 0u && wrote == 0u && reqs[S09_QUAD_TEST] == 3u * OPXY_QUAD_N,
		      "quad: nothing but 0x1C and the link's own (%u others)", others);
	}
	CHECK(!memcmp(vol, vol_before, VOL_LEN), "quad: the volume as it was");
	CHECK(s09_master_rung(&m.bridge) == S09_RUNG_PLAIN && s09_slave_mode(&sl) == S09_MODE_PLAIN &&
	      m.sv.files.link == 0u && s09_master_settled(&m.bridge) && r->falls == falls,
	      "quad: plain after, settled");
	(void)negs;
	/* no flag: the first failure and the slave's record of it */
	qs.no_flag = true;
	n = quad_req(S09_QT_SPIF, 0, 0, 64u, 3, 9);
	qs.no_flag = false;
	CHECK(n == OPXY_QUAD_REPLY && le16(big_reply + 12) == 3u && le16(big_reply + 14) == 3u &&
	      le16(big_reply + 16) == 0u && le16(big_reply + 18) == 3u &&
	      le16(big_reply + 28 + 2u * S09_QE_BOUND) == 3u &&
	      le32(big_reply + 44) == S09_QT_NONE && big_reply[73] == S09_QX_NO_FLAG &&
	      le16(big_reply + 74) == 0u && le32(big_reply + 76) == S09_QT_NONE &&
	      le32(big_reply + 80) == (1u << 10) && le32(big_reply + 88) == 1234u &&
	      le32(big_reply + 96 + 4u * S09_QR_LEN) == 64u &&
	      (le32(big_reply + 96 + 4u * S09_QR_END) & 0xFFu) == S09_QE_BOUND &&
	      le32(big_reply + 96 + 4u * S09_QR_T_FLAG) == S09_QT_NONE,
	      "quad: the flag missing, the record of the first");
	/* refused by the slave's arm: REN with SPIF, refused before anything */
	n = quad_req(S09_QT_SPIF, 1, 0, 64u, 3, 9);
	CHECK(n == OPXY_QUAD_REPLY && (big_reply[10] & OPXY_QUAD_REFUSED) &&
	      big_reply[11] == S09_QT_BAD && le16(big_reply + 12) == 0u,
	      "quad: REN with SPIF refused");
}

int main(void)
{
	struct opxy_disk_part part[OPXY_DISK_PARTS];

	disk = calloc(DISK_SECTORS, 512u);
	before = malloc((size_t)DISK_SECTORS * 512u);
	if (disk == NULL || before == NULL)
		return 2;
	fake_mmr_reset();
	fake_core_reset();
	memset(&m, 0, sizeof(m));

	/* no disk yet */
	CHECK(opxy_dev_check(&m, reply) == 1u && reply[0] == OPXY_DEV_REFUSED,
	      "check: no disk");
	CHECK(bench(OPXY_BENCH_READ, 1) == 1u && reply[0] == OPXY_DEV_REFUSED,
	      "bench: no disk");

	part[0].mem = disk;
	part[0].sectors = SPLIT;
	part[1].mem = disk + SPLIT * 512u;
	part[1].sectors = DISK_SECTORS - SPLIT;
	opxy_disk_init(&m.sv.disk, part);
	test_bench();
	test_check();
	CHECK(bridge_up(), "the bridge over the loopback");
	test_extract();
	test_link_record();
	test_link_bench();
	test_quad_reply();
	test_reader_vector();
	test_reader_synthetic();
	free(vol_before);
	free(vol);
	free(before);
	free(disk);
	return tl_done("test_board_devtime");
}
