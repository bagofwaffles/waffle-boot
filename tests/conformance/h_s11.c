/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for S-11's installs: s11_install() and s11_interrupted() with
 * checks 1-15 on BearSSL and micro-ecc (src/wire_package.c), checks 16-17 on
 * minizip (src/wire_zip.c), the staging area and the content step
 * (s11_install_content) on a yaffs2 volume in RAM (src/wire_yaffs.c), and
 * the RAM disk on FatFs (src/wire_fat.c). The metadata record, the Nordic and
 * PSoC peers and the image writers are fakes that log what they are asked.
 * The volume, the metadata and the installed versions persist from command
 * to command, as they do across boots.
 *
 * With WB_S11_VIA=s09 in the environment, the master's way: checks 16-17's
 * staging and free space (s09_files_zip_ops() around minizip's reader), the
 * run's and the content step's view of the volume (s09_files_fs_ops()) and
 * the progress record (s09_files_record_write()) go over S-09's file
 * commands: s09_files and s09_master, a loopback of the bus, s09_slave and
 * wire_s09 on the same yaffs2 volume. The bridge starts afresh at every
 * install and boot.
 *
 * Paths are relative to /yaffs2 (S-16.117); versions are M.m.p.b; bytes are
 * hex ("-": empty).
 *
 * Commands (one result line each):
 *   vol                   -> ok: a fresh, empty volume
 *   reboot                -> ok: the volume unmounted and mounted again
 *   put PATH BYTES        -> ok | fail: a file, its directories created
 *   fill PATH SIZE BYTE   -> ok | fail: SIZE bytes of BYTE
 *   mkdir PATH | rm PATH  -> ok | fail
 *   get PATH              -> the file's bytes, or "absent"
 *   sha PATH              -> the file's SHA-256, or "absent"
 *   tree                  -> every entry: PATH/ for a directory, PATH:SIZE:CRC32
 *                            for a file, or "-"
 *   otp Q0 K0 K1          -> ok: the OTP items, "x" for one that cannot be read
 *   copro [APP BL PSOC]   -> APP BL PSOC: the installed Nordic application,
 *                            Nordic bootloader and PSoC versions (S-17.020),
 *                            set first
 *   meta [STATE VERSION]  -> STATE VERSION: the metadata record's (set first)
 *   set KEY VALUE         -> ok: a peer's answer or a fault, below
 *   load FILE             -> the package's length: FILE read into memory
 *   install FORM FWCMD OPTION NAME -> REASON TE FORCED STOCK END OUTCOME STEP
 *                            RESULT: s11_install() of the loaded package; FORM
 *                            file | body, NAME "-" for none, REASON "none"
 *                            for an accepted package, END "-" for a refused
 *                            one and "cut" when a cut ended the run; STEP and
 *                            RESULT are the outcome's (S-11.051)
 *   boot REASON KEYS SLAVE -> os | held | interrupted, then STATE: a boot's
 *                            decision (s05_boot_decision() on the metadata
 *                            state; KEYS 0 not held, 1 held, 2 silent; SLAVE
 *                            0 absent), s11_interrupted() when it finds the
 *                            install interrupted, and the state after; "cut"
 *                            when a cut ended it
 *   log                   -> what the last install or boot did, in order:
 *                            meta:state=X | meta:os_version=V,state=0 |
 *                            meta_fails:..., progress:B0,B1 |
 *                            progress_fails:B0,B1 (the record's bytes),
 *                            nordic:enter_dfu | nordic:read_versions |
 *                            nordic:exit_dfu_if_in_dfu | nordic_send:C |
 *                            nordic_slot, os_write, sw_version:V,
 *                            bootloader_write, spiboot_write, psoc_program,
 *                            fs_remove | fs_move | fs_copy (the content step's
 *                            volume changes) and content=RESULT (its code,
 *                            S-11.058), confirm:C=A (S-11.096: component C,
 *                            bootloader or spiboot, asked for and answered
 *                            A, 1 or 0) and skip:STEP=WHY (s11_step_skip()
 *                            of each step that reported Skipped, after the
 *                            run: option_off | not_confirmed | switched_off);
 *                            or "-"
 *   disk SECTORS          -> ok | fail: a RAM disk, formatted (S-12.003)
 *   diskput NAME FILE     -> ok | fail: a file on the RAM disk's root
 *   diskmkdir NAME        -> ok | fail: a directory there
 *   bwrite N RUNG         -> RESULT ERR SIZE MODE: over S-09's loopback (the
 *                            bridge started afresh and raised to RUNG, 0 for
 *                            the plain mode, S-09.075), one file write (0x2B)
 *                            of N bytes (i*7)&0xff to a new file bw.bin,
 *                            through s09_master_call() (S-09.023); SIZE is the
 *                            file's size after it, MODE the slave's link mode
 *   diskinstall OPTION    -> NAME and install's fields: the package
 *                            s11_disk_pick() takes, installed as FatFs reads it;
 *                            "-" when there is none
 *
 * set KEY VALUE (each 0 or 1 unless stated; the default first):
 *   off 3,4,5 | STEPS | -  the switched-off steps (S-11.073), comma-separated;
 *                         "-" for none. The default is this version's
 *   confirm 0 0 | BL FS   S-11.096: the answers on the unit for the
 *                         bootloader and the first stage; 0 for none, as when
 *                         nobody answers
 *   dfu 1 | indfu 1       the Nordic enters DFU; reports bootloader mode
 *   send 1 | sendbl 1     the Nordic application and bootloader transfers
 *                         succeed
 *   slot 0                the boot slot the Nordic answers
 *   psoc 1 | os 1 | bootloader 1 | spiboot 1  the writes succeed
 *   staged - | bad | read_error  S-11.095's check of each image a step
 *                         would write or send: passes, fails (Bad file),
 *                         a read of the image fails (Internal error); the
 *                         step's write or transfer is then not made
 *   space real|fail|N     the free space checks 16-17 see
 *   readfail OFF LEN | -  package reads touching OFF..OFF+LEN-1 fail
 *   stagefail N           the Nth staging open or write fails (0: none)
 *   removefail N | movefail N  the content step's Nth removal or move fails
 *                         (0: none)
 *   metafail N | recordfail N  the Nth metadata or record write fails
 *   cut N                 power is cut before the logged change N+1 (a
 *                         content= entry is not one); "-": no cut
 *   cutat TOKEN           power is cut before the first logged change that
 *                         begins with TOKEN; "-": no cut
 * The counts start again at each install or boot.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bearssl_hash.h"

#include "hlib.h"
#include "waffle/s05_handoff.h"
#include "waffle/s11_content.h"
#include "waffle/s11_package.h"
#include "waffle/s11_run.h"
#include "waffle/s09_files.h"
#include "waffle/s09_slave.h"
#include "waffle/s17_revisions.h"
#include "waffle/wire_fat.h"
#include "waffle/wire_package.h"
#include "waffle/wire_s09.h"
#include "waffle/wire_yaffs.h"
#include "waffle/wire_zip.h"

#define BLOCKS 64u			/* a volume of 32 MiB */
#define LOG_MAX 512u
#define LOG_TOKEN 64u

static uint8_t line_buf[H_MAXLINE / 2];
static uint8_t file_buf[1u << 16];
static uint8_t zip_ram[4u << 20];

/* ---- the yaffs2 volume ---- */

static struct wb_blockdev ybd;
static struct wb_ramdisk yram;
static uint8_t *yvol;
static struct wire_yaffs Y;

static bool vol_fresh(void)
{
	size_t len = (size_t)BLOCKS * WB_YAFFS_CHUNKS_PER_BLOCK *
		     WB_YAFFS_CHUNK_BYTES;

	wire_yaffs_detach(&Y);
	if (yvol == NULL)
		yvol = malloc(len);
	if (yvol == NULL)
		return false;
	memset(yvol, 0, len);
	wb_ramdisk_init(&ybd, &yram, yvol, (uint32_t)(len / WB_SECTOR_SIZE));
	return wire_yaffs_add(&Y, "/yaffs2", &ybd, 0, BLOCKS - 1) &&
	       wire_yaffs_mount(&Y) == WIRE_MOUNT_OK;
}

/* ---- the master's way to it: S-09 over a loopback (WB_S11_VIA=s09) ---- */

static bool via_s09;
static struct wire_s09 S;
static struct s09_slave sl;
static struct s09_master ms;
static struct s09_files Fs;
static uint32_t clock_us;

static int32_t st_emmc(void *c, uint8_t cmd, uint32_t a0, uint32_t a1,
		       uint8_t *data, uint32_t n)
{
	(void)c;
	(void)cmd;
	(void)a0;
	(void)a1;
	(void)data;
	(void)n;
	return 1;
}

static uint32_t st_group(void *c) { (void)c; return 1024u; }
static uint32_t st_boot(void *c) { (void)c; return 0; }
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

static int32_t st_read(void *c, uint32_t fd, uint8_t *buf, uint32_t n,
		       uint32_t *err)
{
	(void)c;
	return wire_s09_read(&S, fd, buf, n, err);
}

static const struct s09_store_ops store = {
	0, st_group, st_boot, NULL /* dev_cmd */, st_emmc, st_file, st_read, st_os, st_cycles,
	st_adjust,
};

/* The bus: a request session ends at the deselect, a reply session gives
 * the slave's reply whole. */
static struct {
	bool sel, reply_session, done;
	uint8_t req[S09_HDR + S09_TEST_MAX + S09_CHECK_LEN];	/* a link test's, the largest */
	size_t req_n;
	const uint8_t *rep;
	size_t rep_n, rep_at;
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
	} else if (!on && lb.sel && !lb.reply_session && lb.req_n >= S09_HDR) {
		size_t k = (size_t)s09_slave_rx_len(&sl, lb.req);

		s09_slave_request(&sl, lb.req, k < lb.req_n ? k : lb.req_n);
	}
	lb.sel = on;
}

static bool lb_send(void *c, const uint8_t *d, size_t n)
{
	(void)c;
	if (lb.reply_session || lb.req_n + n > sizeof lb.req)
		return false;
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
	(void)phase;
}

static const struct s09_bus_ops bus = {
	0, lb_ready, lb_attention, lb_select, lb_send, lb_recv, lb_configure, NULL,
};

static uint32_t c_now(void *c)
{
	(void)c;
	return clock_us++;
}

static void c_idle(void *c, uint32_t until)
{
	(void)c;
	if ((int32_t)(until - clock_us) > 0)
		clock_us = until;
}

static const struct s06_clock clk = { c_now, c_idle, NULL };

/* What a cut left open on the slave's side is closed before the volume is
 * unmounted. */
static void bridge_down(void)
{
	if (Fs.m != NULL && Fs.stage_fd >= 0)
		(void)yaffs_close(Fs.stage_fd);
	Fs.stage_fd = -1;
	wire_s09_release(&S);
}

static void bridge_up(void)
{
	bridge_down();
	memset(&lb, 0, sizeof lb);
	wire_s09_init(&S, &Y);
	s09_slave_init(&sl, &store);
	s09_master_init(&ms, &bus, &clk);
	if (!s09_master_start(&ms)) {
		fprintf(stderr, "h_s11: the bridge did not start\n");
		exit(1);
	}
	s09_files_init(&Fs, &ms);
	if (s09_files_mount(&Fs) != 0) {
		fprintf(stderr, "h_s11: /yaffs2 not mounted over the bridge\n");
		exit(1);
	}
}

static bool put_file(const char *rel, const uint8_t *p, size_t n)
{
	char full[WIRE_YAFFS_PATH_MAX];
	const char *slash = strrchr(rel, '/');
	int fd;
	bool ok;

	if (slash != NULL) {
		char dir[S11_PATH_MAX];

		snprintf(dir, sizeof(dir), "%.*s", (int)(slash - rel), rel);
		if (!wire_yaffs_mkdir(&Y, dir))
			return false;
	}
	if (!wire_yaffs_full_path(&Y, rel, full))
		return false;
	fd = yaffs_open(full, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0)
		return false;
	ok = true;
	while (ok && n != 0) {
		unsigned int k = n > 0x10000u ? 0x10000u : (unsigned int)n;

		ok = yaffs_write(fd, p, k) == (int)k;
		p += k;
		n -= k;
	}
	return yaffs_close(fd) == 0 && ok;
}

static bool fill_file(const char *rel, uint32_t size, uint8_t byte)
{
	uint8_t *p = malloc(size ? size : 1u);
	bool ok;

	if (p == NULL)
		return false;
	memset(p, byte, size);
	ok = put_file(rel, p, size);
	free(p);
	return ok;
}

static size_t read_file(const char *rel, uint8_t *buf, size_t cap)
{
	struct s11_fs_ops fs;

	wire_yaffs_fs_ops(&Y, &fs);
	return fs.read(fs.ctx, rel, buf, cap);
}

static void put_tree(const char *dir, bool *any)
{
	char name[S11_PATH_MAX], path[2 * S11_PATH_MAX], tok[2 * S11_PATH_MAX + 32];
	bool is_dir;

	for (size_t i = 0; wire_yaffs_list(&Y, dir, i, name, sizeof(name),
					   &is_dir, NULL); i++) {
		uint32_t size = 0, crc = 0, off = 0;
		bool d;
		char full[WIRE_YAFFS_PATH_MAX];
		int fd;

		snprintf(path, sizeof(path), "%s%s%s", dir, *dir ? "/" : "", name);
		if (is_dir) {
			snprintf(tok, sizeof(tok), "%s/", path);
			h_put(tok);
			*any = true;
			put_tree(path, any);
			continue;
		}
		(void)wire_yaffs_stat(&Y, path, &d, &size);
		if (wire_yaffs_full_path(&Y, path, full) &&
		    (fd = yaffs_open(full, O_RDONLY, 0)) >= 0) {
			int k;

			while ((k = yaffs_read(fd, file_buf, sizeof(file_buf))) > 0) {
				crc = h_crc32(crc, file_buf, (uint32_t)k);
				off += (uint32_t)k;
			}
			(void)yaffs_close(fd);
		}
		snprintf(tok, sizeof(tok), "%s:%lu:%08lx", path,
			 (unsigned long)off, (unsigned long)crc);
		h_put(tok);
		*any = true;
	}
}

static void put_sha(const char *rel)
{
	char full[WIRE_YAFFS_PATH_MAX];
	br_sha256_context sha;
	uint8_t d[32];
	int fd;

	br_sha256_init(&sha);
	if (wire_yaffs_full_path(&Y, rel, full) &&
	    (fd = yaffs_open(full, O_RDONLY, 0)) >= 0) {
		int k;

		while ((k = yaffs_read(fd, file_buf, sizeof(file_buf))) > 0)
			br_sha256_update(&sha, file_buf, (size_t)k);
		(void)yaffs_close(fd);
	}
	br_sha256_out(&sha, d);
	h_puthex(d, sizeof(d));
}

/* ---- the unit: OTP, metadata, installed versions ---- */

static struct {
	uint8_t q0[56], k0[32], k1[32];
	bool q0_ok, k0_ok, k1_ok;
} otp;

/* cppcheck-suppress [constParameterCallback, constParameterPointer] ; s11_unit_ops fixes the type */
static bool otp_q0(void *ctx, uint8_t q[56])
{
	(void)ctx;
	memcpy(q, otp.q0, 56);
	return otp.q0_ok;
}

/* cppcheck-suppress [constParameterCallback, constParameterPointer] ; s11_unit_ops fixes the type */
static bool otp_k0(void *ctx, uint8_t k[32])
{
	(void)ctx;
	memcpy(k, otp.k0, 32);
	return otp.k0_ok;
}

/* cppcheck-suppress [constParameterCallback, constParameterPointer] ; s11_unit_ops fixes the type */
static bool otp_k1(void *ctx, uint8_t k[32])
{
	(void)ctx;
	memcpy(k, otp.k1, 32);
	return otp.k1_ok;
}

static const struct s11_unit_ops unit_ops = { otp_q0, otp_k0, otp_k1 };

static struct {
	uint16_t state;
	struct s11_version os_version;
} meta;

static struct s17_copro copro;

/* ---- peers, writers, faults ---- */

static struct {
	bool dfu, indfu, send, sendbl, psoc, os, bootloader, spiboot;
	int slot;
	int space;			/* -1 real, -2 fails, else below */
	uint64_t space_bytes;
	bool readfail;
	uint32_t rf_off, rf_len;
	unsigned stagefail, removefail, movefail, metafail, recordfail;
	long cut;			/* -1: none */
	char cut_at[LOG_TOKEN];		/* "": none */
	int staged;			/* S-11.095: 0 passes, 1 bad, 2 read error */
	uint8_t off;			/* S-11.073: S11_STEP_BIT() each */
	bool confirm_bl, confirm_fs;	/* S-11.096: the answers */
} cfg = { true, true, true, true, true, true, true, true, 0, -1, 0, false, 0,
	  0, 0, 0, 0, 0, 0, -1, "", 0, S11_STEPS_SWITCHABLE, false, false };

static struct {
	unsigned stage, remove, move, meta, record;
	long changes;
	bool in_dfu;
} cnt;

static char log_tok[LOG_MAX][LOG_TOKEN];
static size_t log_n;
static jmp_buf cut_jmp;

static void note(const char *s)
{
	if (log_n < LOG_MAX)
		snprintf(log_tok[log_n++], LOG_TOKEN, "%s", s);
}

/* A change to the unit's state: the point where a cut may land. */
static void change(const char *s)
{
	if ((cfg.cut >= 0 && cnt.changes >= cfg.cut) ||
	    (cfg.cut_at[0] != '\0' &&
	     strncmp(s, cfg.cut_at, strlen(cfg.cut_at)) == 0))
		longjmp(cut_jmp, 1);
	cnt.changes++;
	note(s);
}

static void version_text(const struct s11_version *v, char *out, size_t cap)
{
	snprintf(out, cap, "%u.%u.%u.%u", (unsigned)v->major,
		 (unsigned)v->minor, (unsigned)v->patch, (unsigned)v->build);
}

static bool parse_version(const char *s, struct s11_version *v)
{
	unsigned a, b, c, d;

	if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 0xFFFFu ||
	    b > 0xFFFFu || c > 0xFFFFu || d > 0xFFFFu)
		return false;
	v->major = (uint16_t)a;
	v->minor = (uint16_t)b;
	v->patch = (uint16_t)c;
	v->build = (uint16_t)d;
	return true;
}

static bool r_meta(void *ctx, uint16_t state)
{
	char t[LOG_TOKEN];
	bool ok = ++cnt.meta != cfg.metafail;

	(void)ctx;
	snprintf(t, sizeof(t), "meta%s:state=%X", ok ? "" : "_fails",
		 (unsigned)state);
	change(t);
	if (ok)
		meta.state = state;
	return ok;
}

static bool r_complete(void *ctx, const struct s11_version *v)
{
	char t[LOG_TOKEN], vt[32];
	bool ok = ++cnt.meta != cfg.metafail;

	(void)ctx;
	version_text(v, vt, sizeof(vt));
	snprintf(t, sizeof(t), "meta%s:os_version=%s,state=0", ok ? "" : "_fails",
		 vt);
	change(t);
	if (ok) {
		meta.state = 0;
		meta.os_version = *v;
	}
	return ok;
}

static bool r_record(void *ctx, uint8_t s)
{
	char t[LOG_TOKEN];
	uint8_t rec[2];
	bool ok = ++cnt.record != cfg.recordfail;

	(void)ctx;
	s11_record_bytes(s, rec);
	snprintf(t, sizeof(t), "progress%s:%u,%u", ok ? "" : "_fails",
		 (unsigned)rec[0], (unsigned)rec[1]);
	change(t);
	if (!ok)
		return false;
	if (via_s09)
		return s09_files_record_write(&Fs, s);
	if (!wire_yaffs_staging_open(&Y, "progress.log"))
		return false;
	ok = wire_yaffs_staging_write(&Y, rec, sizeof(rec));
	return wire_yaffs_staging_close(&Y) && ok;
}

static bool r_enter(void *ctx)
{
	(void)ctx;
	change("nordic:enter_dfu");
	return cfg.dfu;
}

static bool r_versions(void *ctx)
{
	(void)ctx;
	change("nordic:read_versions");
	cnt.in_dfu = cfg.indfu;
	return cfg.indfu;
}

static void r_exit(void *ctx)
{
	(void)ctx;
	change("nordic:exit_dfu_if_in_dfu");
}

static enum s11_result r_step(void *ctx, unsigned s)
{
	(void)ctx;
	(void)s;
	return S11_INTERNAL;	/* not used: the step ops are given */
}

static const struct s11_run_ops run_ops = {
	r_meta, r_complete, r_record, r_enter, r_versions, r_exit, r_step, NULL,
};

/* The content step's volume: the yaffs2 one, its changes logged. */
static struct s11_fs_ops yfs;

static bool c_stat(void *ctx, const char *path, bool *is_dir, uint32_t *size)
{
	(void)ctx;
	return yfs.stat(yfs.ctx, path, is_dir, size);
}

static size_t c_read(void *ctx, const char *path, uint8_t *buf, size_t cap)
{
	(void)ctx;
	return yfs.read(yfs.ctx, path, buf, cap);
}

static bool c_list(void *ctx, const char *dir, size_t index, char *name,
		   size_t cap, bool *is_dir)
{
	(void)ctx;
	return yfs.list(yfs.ctx, dir, index, name, cap, is_dir);
}

static bool c_remove(void *ctx, const char *path)
{
	(void)ctx;
	change("fs_remove");
	if (++cnt.remove == cfg.removefail)
		return false;
	return yfs.remove(yfs.ctx, path);
}

static bool c_move(void *ctx, const char *from, const char *to)
{
	(void)ctx;
	change("fs_move");
	if (++cnt.move == cfg.movefail)
		return false;
	return yfs.move(yfs.ctx, from, to);
}

static bool c_copy(void *ctx, const char *from, const char *to)
{
	(void)ctx;
	change("fs_copy");
	return yfs.copy(yfs.ctx, from, to);
}

static const struct s11_fs_ops content_fs = {
	c_stat, c_read, c_list, c_remove, c_move, c_copy, NULL,
};

static enum s11_result s_content(void *ctx)
{
	struct s11_staged st;
	enum s11_result r;
	char t[LOG_TOKEN];

	(void)ctx;
	s11_staged_scan(&yfs, &st);
	r = s11_install_content(&content_fs, s11_staged_content_only(&st));
	snprintf(t, sizeof(t), "content=%u", (unsigned)r);
	note(t);
	return r;
}

/* S-11.095: an image whose check or read fails is neither written nor
 * sent; the step's op returns false and s_failure says why. */
static bool staged_refused(void)
{
	if (cfg.staged == 0)
		return false;
	note(cfg.staged == 1 ? "staged:bad" : "staged:read_error");
	return true;
}

static enum s11_result s_failure(void *ctx)
{
	(void)ctx;
	return cfg.staged == 1 ? S11_BAD_FILE :
	       cfg.staged == 2 ? S11_INTERNAL : S11_FAILURE;
}

static bool s_os(void *ctx)
{
	(void)ctx;
	if (staged_refused())
		return false;
	change("os_write");
	return cfg.os;
}

static void s_sw(void *ctx, const struct s11_version *v)
{
	char t[LOG_TOKEN], vt[32];

	(void)ctx;
	version_text(v, vt, sizeof(vt));
	snprintf(t, sizeof(t), "sw_version:%s", vt);
	change(t);
}

static bool s_bootloader(void *ctx)
{
	(void)ctx;
	if (staged_refused())
		return false;
	change("bootloader_write");
	return cfg.bootloader;
}

static bool s_spiboot(void *ctx)
{
	(void)ctx;
	if (staged_refused())
		return false;
	change("spiboot_write");
	return cfg.spiboot;
}

static bool s_send(void *ctx, enum s11_component c)
{
	(void)ctx;
	if (staged_refused())
		return false;
	change(c == S11_C_BLE ? "nordic_send:ble" :
	       c == S11_C_NORDIC_BL0 ? "nordic_send:bl0" :
	       c == S11_C_NORDIC_BL1 ? "nordic_send:bl1" : "nordic_send:?");
	return c == S11_C_BLE ? cfg.send : cfg.sendbl;
}

static int s_slot(void *ctx)
{
	(void)ctx;
	change("nordic_slot");
	return cfg.slot;
}

static bool s_psoc(void *ctx)
{
	(void)ctx;
	if (staged_refused())
		return false;
	change("psoc_program");
	return cfg.psoc;
}

static const struct s11_step_ops step_ops = {
	s_content, s_os, s_sw, s_bootloader, s_spiboot, s_failure, s_send, s_slot,
	s_psoc, NULL,
};

/* S-11.096: the answer on the unit, logged; not a change, so no cut falls
 * on it */
static bool u_confirm(void *ctx, enum s11_component c)
{
	const bool yes = c == S11_C_BOOTLOADER ? cfg.confirm_bl :
			 c == S11_C_SPIBOOT ? cfg.confirm_fs : false;

	(void)ctx;
	note(c == S11_C_BOOTLOADER ? (yes ? "confirm:bootloader=1" :
					    "confirm:bootloader=0") :
	     c == S11_C_SPIBOOT ? (yes ? "confirm:spiboot=1" :
					 "confirm:spiboot=0") : "confirm:?");
	return yes;
}

/* ---- the package's source and the zip's staging, with faults ---- */

static struct wire_source src_inner;

static bool src_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	(void)ctx;
	if (cfg.readfail && off < cfg.rf_off + cfg.rf_len &&
	    cfg.rf_off < off + n)
		return false;
	return src_inner.read(src_inner.ctx, off, buf, n);
}

static struct s11_zip_ops zip_inner;

static bool z_member(void *ctx, size_t index, struct s11_member *m)
{
	(void)ctx;
	return zip_inner.member(zip_inner.ctx, index, m);
}

static bool z_zip64(void *ctx)
{
	(void)ctx;
	return zip_inner.zip64_end(zip_inner.ctx);
}

static bool z_extract(void *ctx, size_t index, s11_sink_fn sink, void *arg)
{
	(void)ctx;
	return zip_inner.extract(zip_inner.ctx, index, sink, arg);
}

static uint32_t z_crc32(void *ctx, uint32_t crc, const uint8_t *p, size_t n)
{
	(void)ctx;
	return zip_inner.crc32(zip_inner.ctx, crc, p, n);
}

static bool z_free(void *ctx, uint64_t *bytes)
{
	(void)ctx;
	if (cfg.space == -2)
		return false;
	if (cfg.space >= 0) {
		*bytes = cfg.space_bytes;
		return true;
	}
	return zip_inner.free_space(zip_inner.ctx, bytes);
}

static bool z_reset(void *ctx)
{
	(void)ctx;
	return zip_inner.staging_reset(zip_inner.ctx);
}

static bool z_mkdir(void *ctx, const char *path)
{
	(void)ctx;
	return zip_inner.staging_mkdir(zip_inner.ctx, path);
}

static bool z_open(void *ctx, const char *path)
{
	(void)ctx;
	if (++cnt.stage == cfg.stagefail)
		return false;
	return zip_inner.staging_open(zip_inner.ctx, path);
}

static bool z_write(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	if (++cnt.stage == cfg.stagefail)
		return false;
	return zip_inner.staging_write(zip_inner.ctx, p, n);
}

static bool z_close(void *ctx)
{
	(void)ctx;
	return zip_inner.staging_close(zip_inner.ctx);
}

static const struct s11_zip_ops zip_ops = {
	z_member, z_zip64, z_extract, z_crc32, z_free, z_reset, z_mkdir, z_open,
	z_write, z_close, NULL,
};

/* ---- installs ---- */

static uint8_t *pkg;
static size_t pkg_len;
static struct wire_package P;
static struct wire_zip Z;
static struct s11_zip_ops reader;
static struct s09_files_zip SZ;
static struct s11_install_result R;

static void start(void)
{
	memset(&cnt, 0, sizeof(cnt));
	log_n = 0;
	if (via_s09) {
		bridge_up();
		s09_files_fs_ops(&Fs, &yfs);
	} else {
		wire_yaffs_fs_ops(&Y, &yfs);
	}
}

static const char *end_name(enum s11_run_end e)
{
	switch (e) {
	case S11_RUN_DONE:
		return "done";
	case S11_RUN_NOTHING_TO_DO:
		return "nothing";
	case S11_RUN_FAILED:
		return "failed";
	default:
		return "?";
	}
}

static const char *outcome_name(enum s11_outcome_kind k)
{
	switch (k) {
	case S11_OUTCOME_DONE:
		return "done";
	case S11_OUTCOME_NOTHING_TO_DO:
		return "nothing";
	case S11_OUTCOME_REFUSED:
		return "refused";
	case S11_OUTCOME_FAILED:
		return "failed";
	default:
		return "?";
	}
}

/* S-11.058: why each step that reported Skipped did, from the staging area
 * the run used and the answers it was given. */
static void note_skips(const struct s11_fs_ops *fs, bool option)
{
	static const char *const why[] = {
		[S11_SKIP_NONE] = NULL,
		[S11_SKIP_OPTION_OFF] = "option_off",
		[S11_SKIP_NOT_CONFIRMED] = "not_confirmed",
		[S11_SKIP_SWITCHED_OFF] = "switched_off",
		[S11_SKIP_ALREADY_WRITTEN] = "already_written",
	};
	struct s11_staged st;
	struct s11_step_env env;
	char t[LOG_TOKEN];

	if (R.reason != S11_REASON_NONE)
		return;
	s11_staged_scan(fs, &st);
	memset(&env, 0, sizeof(env));
	env.staged = &st;
	env.option = option;
	env.off = cfg.off;
	env.confirmed = (uint8_t)((option && cfg.confirm_bl ?
				   S11_STEP_BIT(S11_STEP_BOOTLOADER) : 0u) |
				  (option && cfg.confirm_fs ?
				   S11_STEP_BIT(S11_STEP_SPIBOOT) : 0u));
	/* the steps that ran: all of a completed run, those before the step
	 * that stopped a failed one */
	for (unsigned step = 0; step < S11_STEP_DONE; step++) {
		const enum s11_skip w = s11_step_skip(step, &env);

		if (R.run.end == S11_RUN_FAILED &&
		    (R.run.step >= S11_STEP_DONE || step >= R.run.step))
			break;
		if (w == S11_SKIP_NONE)
			continue;
		snprintf(t, sizeof(t), "skip:%u=%s", step, why[w]);
		note(t);
	}
}

/* The package from `src` (size bytes), installed; prints install's fields. */
static void install(const struct wire_source *src, uint32_t size,
		    enum s11_form form, bool fw_command, bool option,
		    const char *name)
{
	static struct s11_package_ops pkg_ops;
	static struct s11_fs_ops fs;
	static struct s11_install_ops ops;
	static struct s11_check_in in;
	struct wire_source w = { src_read, NULL, NULL };

	src_inner = *src;
	start();
	wire_package_init(&P, &pkg_ops, &w, &unit_ops, NULL, zip_ram,
			  sizeof(zip_ram));
	if (via_s09) {
		wire_zip_init(&Z, &reader, &P, NULL, 0, NULL);
		s09_files_zip_ops(&SZ, &Fs, &reader, &zip_inner);
		fs = yfs;
	} else {
		wire_zip_init(&Z, &zip_inner, &P, NULL, 0, &Y);
		wire_yaffs_fs_ops(&Y, &fs);
	}
	ops.pkg = &pkg_ops;
	ops.zip = &zip_ops;
	ops.fs = &fs;
	ops.run = &run_ops;
	ops.steps = &step_ops;
	ops.confirm = u_confirm;
	ops.confirm_ctx = NULL;
	ops.off = cfg.off;
	in.form = form;
	in.size = size;
	in.option = option;
	in.file_name = name;
	in.fw_command = fw_command;
	memset(&R, 0, sizeof(R));
	if (setjmp(cut_jmp) == 0) {
		s11_install(&ops, &in, &copro, &R);
		wire_zip_close(&Z);
		note_skips(&fs, in.option);
		h_put(R.reason == S11_REASON_NONE ? "none" :
		      s11_reason_name(R.reason));
		h_putu(R.te_signed);
		h_putu(R.forced);
		h_putu(R.stock);
		h_put(R.reason == S11_REASON_NONE ? end_name(R.run.end) : "-");
		h_put(outcome_name(R.outcome.kind));
		h_putu(R.outcome.step);
		h_putu(R.outcome.result);
	} else {
		wire_zip_close(&Z);
		h_put("none");
		h_putu(R.te_signed);
		h_putu(R.forced);
		h_putu(0);
		h_put("cut");
		h_put("-");
		h_putu(0xFF);
		h_putu(0);
	}
}

/* A boot (S-05.001): the OS, held mode, or held mode after S-11.080's
 * state-0 write, which is s11_interrupted()'s alone. */
static void boot(uint16_t reason, enum s05_keys keys, bool slave)
{
	struct s05_decision d;

	start();
	s05_boot_decision(reason, meta.state, keys, slave, &d);
	if (setjmp(cut_jmp) != 0) {
		h_put("cut");
		h_putx(meta.state);
		return;
	}
	if (d.interrupted)
		(void)s11_interrupted(&run_ops);
	h_put(d.interrupted ? "interrupted" : d.boot_os ? "os" : "held");
	h_putx(meta.state);
}

/* ---- the RAM disk ---- */

static struct wb_blockdev dbd;
static struct wb_ramdisk dram;
static uint8_t *dmem;
static size_t dmem_len;
static struct wire_fat F;

static uint8_t *load_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *p = NULL;
	long n;

	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
	    fseek(f, 0, SEEK_SET) == 0 &&
	    (p = malloc((size_t)n + 1u)) != NULL &&
	    fread(p, 1, (size_t)n, f) != (size_t)n) {
		free(p);
		p = NULL;
	} else if (p != NULL) {
		*len = (size_t)n;
	}
	fclose(f);
	return p;
}

static bool disk_new(uint32_t sectors)
{
	if (F.mounted)
		wire_fat_unmount(&F);
	free(dmem);
	dmem_len = (size_t)sectors * WB_SECTOR_SIZE;
	dmem = calloc(sectors, WB_SECTOR_SIZE);
	if (dmem == NULL)
		return false;
	wb_ramdisk_init(&dbd, &dram, dmem, sectors);
	return wire_fat_format(&F, &dbd);
}

static void put_ok(bool b)
{
	h_put(b ? "ok" : "fail");
	h_end();
}

static bool set(const struct h_cmd *c)
{
	const char *k = c->tok[1], *v = c->tok[2];
	bool b = strcmp(v, "1") == 0;

	if (strcmp(k, "dfu") == 0)
		cfg.dfu = b;
	else if (strcmp(k, "indfu") == 0)
		cfg.indfu = b;
	else if (strcmp(k, "send") == 0)
		cfg.send = b;
	else if (strcmp(k, "sendbl") == 0)
		cfg.sendbl = b;
	else if (strcmp(k, "psoc") == 0)
		cfg.psoc = b;
	else if (strcmp(k, "os") == 0)
		cfg.os = b;
	else if (strcmp(k, "bootloader") == 0)
		cfg.bootloader = b;
	else if (strcmp(k, "spiboot") == 0)
		cfg.spiboot = b;
	else if (strcmp(k, "slot") == 0)
		cfg.slot = (int)h_num(c, 2);
	else if (strcmp(k, "off") == 0) {
		cfg.off = 0;
		for (const char *p = v; strcmp(v, "-") != 0 && *p != '\0'; p++) {
			if (*p >= '0' && *p <= '6')
				cfg.off = (uint8_t)(cfg.off |
						    S11_STEP_BIT((unsigned)(*p - '0')));
		}
	} else if (strcmp(k, "confirm") == 0 && c->n == 4) {
		cfg.confirm_bl = b;
		cfg.confirm_fs = strcmp(c->tok[3], "1") == 0;
	}
	else if (strcmp(k, "staged") == 0)
		cfg.staged = strcmp(v, "bad") == 0 ? 1 :
			     strcmp(v, "read_error") == 0 ? 2 : 0;
	else if (strcmp(k, "space") == 0) {
		cfg.space = strcmp(v, "real") == 0 ? -1 :
			    strcmp(v, "fail") == 0 ? -2 : 0;
		if (cfg.space == 0)
			cfg.space_bytes = strtoull(v, NULL, 0);
	} else if (strcmp(k, "readfail") == 0) {
		cfg.readfail = strcmp(v, "-") != 0;
		if (cfg.readfail && c->n == 4) {
			cfg.rf_off = (uint32_t)h_num(c, 2);
			cfg.rf_len = (uint32_t)h_num(c, 3);
		}
	} else if (strcmp(k, "stagefail") == 0)
		cfg.stagefail = (unsigned)h_num(c, 2);
	else if (strcmp(k, "removefail") == 0)
		cfg.removefail = (unsigned)h_num(c, 2);
	else if (strcmp(k, "movefail") == 0)
		cfg.movefail = (unsigned)h_num(c, 2);
	else if (strcmp(k, "metafail") == 0)
		cfg.metafail = (unsigned)h_num(c, 2);
	else if (strcmp(k, "recordfail") == 0)
		cfg.recordfail = (unsigned)h_num(c, 2);
	else if (strcmp(k, "cut") == 0)
		cfg.cut = strcmp(v, "-") == 0 ? -1 : (long)h_num(c, 2);
	else if (strcmp(k, "cutat") == 0)
		snprintf(cfg.cut_at, sizeof(cfg.cut_at), "%s",
			 strcmp(v, "-") == 0 ? "" : v);
	else
		return false;
	return true;
}

static bool hex_item(const struct h_cmd *c, int i, uint8_t *out, size_t n)
{
	if (strcmp(c->tok[i], "x") == 0)
		return false;
	return h_hex(c, i, out, n) == n;
}

/* S-09.023, S-09.037: one 0x2B of n bytes over the loopback to wire_s09. */
static void bwrite(uint32_t n, unsigned rung)
{
	struct s09_req r = { S09_WRITE, 0, (uint16_t)n, 0, n };
	uint32_t err = 0, size = 0;
	int32_t fd, res;
	bool d;
	char out[48];

	bridge_up();
	fd = s09_files_open(&Fs, "bw.bin", S09_O_WRONLY | S09_O_CREAT | S09_O_TRUNC);
	if (fd < 0 || n > sizeof(file_buf) || !s09_master_link_fast(&ms, rung) ||
	    s09_master_rung(&ms) != rung) {
		h_put("fail");
		h_end();
		return;
	}
	for (uint32_t i = 0; i < n; i++)
		file_buf[i] = (uint8_t)(i * 7u);
	r.arg0 = (uint32_t)fd;
	res = s09_master_call(&ms, &r, file_buf, n, NULL, 0, NULL, &err);
	(void)s09_files_close(&Fs, fd);
	(void)wire_yaffs_stat(&Y, "bw.bin", &d, &size);
	snprintf(out, sizeof(out), "%ld %lu %lu %u", (long)res, (unsigned long)err, (unsigned long)size,
		 s09_slave_mode(&sl));
	h_put(out);
	h_end();
	bridge_down();
}

int main(void)
{
	struct h_cmd c;

	const char *via = getenv("WB_S11_VIA");

	via_s09 = via != NULL && strcmp(via, "s09") == 0;
	s17_copro_init(&copro);
	if (!vol_fresh())
		return 2;
	while (h_next(&c)) {
		const char *k = c.tok[0];

		if (strcmp(k, "bwrite") == 0 && c.n == 3) {
			bwrite((uint32_t)h_num(&c, 1), (unsigned)h_num(&c, 2));
		} else if (strcmp(k, "vol") == 0 && c.n == 1) {
			bridge_down();
			put_ok(vol_fresh());
		} else if (strcmp(k, "reboot") == 0 && c.n == 1) {
			bridge_down();
			wire_yaffs_unmount(&Y);
			put_ok(wire_yaffs_mount(&Y) == WIRE_MOUNT_OK);
		} else if (strcmp(k, "put") == 0 && c.n == 3) {
			size_t n = h_hex(&c, 2, line_buf, sizeof(line_buf));

			put_ok(put_file(c.tok[1], line_buf, n));
		} else if (strcmp(k, "fill") == 0 && c.n == 4) {
			put_ok(fill_file(c.tok[1], (uint32_t)h_num(&c, 2),
				     (uint8_t)h_num(&c, 3)));
		} else if (strcmp(k, "mkdir") == 0 && c.n == 2) {
			put_ok(wire_yaffs_mkdir(&Y, c.tok[1]));
		} else if (strcmp(k, "rm") == 0 && c.n == 2) {
			put_ok(wire_yaffs_remove(&Y, c.tok[1]));
		} else if (strcmp(k, "get") == 0 && c.n == 2) {
			bool d;
			uint32_t size;

			if (!wire_yaffs_stat(&Y, c.tok[1], &d, &size) || d)
				h_put("absent");
			else
				h_puthex(file_buf, read_file(c.tok[1], file_buf,
							     sizeof(file_buf)));
			h_end();
		} else if (strcmp(k, "sha") == 0 && c.n == 2) {
			bool d;
			uint32_t size;

			if (!wire_yaffs_stat(&Y, c.tok[1], &d, &size) || d)
				h_put("absent");
			else
				put_sha(c.tok[1]);
			h_end();
		} else if (strcmp(k, "tree") == 0 && c.n == 1) {
			bool any = false;

			put_tree("", &any);
			if (!any)
				h_put("-");
			h_end();
		} else if (strcmp(k, "otp") == 0 && c.n == 4) {
			otp.q0_ok = hex_item(&c, 1, otp.q0, sizeof(otp.q0));
			otp.k0_ok = hex_item(&c, 2, otp.k0, sizeof(otp.k0));
			otp.k1_ok = hex_item(&c, 3, otp.k1, sizeof(otp.k1));
			put_ok(true);
		} else if (strcmp(k, "copro") == 0 && (c.n == 1 || c.n == 4)) {
			char vt[32];

			if (c.n == 4 &&
			    (!parse_version(c.tok[1], &copro.nordic_app) ||
			     !parse_version(c.tok[2], &copro.nordic_bl) ||
			     !parse_version(c.tok[3], &copro.psoc))) {
				h_error(&c, "bad version");
				continue;
			}
			version_text(&copro.nordic_app, vt, sizeof(vt));
			h_put(vt);
			version_text(&copro.nordic_bl, vt, sizeof(vt));
			h_put(vt);
			version_text(&copro.psoc, vt, sizeof(vt));
			h_put(vt);
			h_end();
		} else if (strcmp(k, "meta") == 0 && (c.n == 1 || c.n == 3)) {
			char vt[32];

			if (c.n == 3) {
				meta.state = (uint16_t)h_num(&c, 1);
				if (!parse_version(c.tok[2], &meta.os_version)) {
					h_error(&c, "bad version");
					continue;
				}
			}
			h_putx(meta.state);
			version_text(&meta.os_version, vt, sizeof(vt));
			h_put(vt);
			h_end();
		} else if (strcmp(k, "set") == 0 && (c.n == 3 || c.n == 4)) {
			if (!set(&c)) {
				h_error(&c, "unknown setting");
				continue;
			}
			put_ok(true);
		} else if (strcmp(k, "load") == 0 && c.n == 2) {
			free(pkg);
			pkg_len = 0;
			pkg = load_file(c.tok[1], &pkg_len);
			if (pkg == NULL) {
				h_error(&c, "cannot read the file");
				continue;
			}
			h_putu(pkg_len);
			h_end();
		} else if (strcmp(k, "install") == 0 && c.n == 5) {
			static struct wire_mem mem;
			struct wire_source s = { wire_mem_read, &mem, NULL };
			const char *name = strcmp(c.tok[4], "-") ? c.tok[4] : NULL;

			mem.buf = pkg;
			mem.len = pkg_len;
			install(&s, (uint32_t)pkg_len,
				strcmp(c.tok[1], "file") == 0 ? S11_FORM_FILE :
				S11_FORM_BODY, h_num(&c, 2) != 0,
				h_num(&c, 3) != 0, name);
			h_end();
		} else if (strcmp(k, "boot") == 0 && c.n == 4) {
			boot((uint16_t)h_num(&c, 1), (enum s05_keys)h_num(&c, 2),
			     h_num(&c, 3) != 0);
			h_end();
		} else if (strcmp(k, "log") == 0 && c.n == 1) {
			for (size_t i = 0; i < log_n; i++)
				h_put(log_tok[i]);
			if (log_n == 0)
				h_put("-");
			h_end();
		} else if (strcmp(k, "disk") == 0 && c.n == 2) {
			put_ok(disk_new((uint32_t)h_num(&c, 1)));
		} else if (strcmp(k, "diskput") == 0 && c.n == 3) {
			size_t n = 0;
			uint8_t *p = load_file(c.tok[2], &n);

			put_ok(p != NULL && F.mounted &&
			   wire_fat_write_file(&F, c.tok[1], p, n, false, 0));
			free(p);
		} else if (strcmp(k, "diskmkdir") == 0 && c.n == 2) {
			put_ok(F.mounted && wire_fat_mkdir(&F, c.tok[1]));
		} else if (strcmp(k, "diskinstall") == 0 && c.n == 2) {
			static char name[S11_PATH_MAX];
			struct wire_source s;
			uint32_t size = 0;

			if (!F.mounted || !s11_disk_pick(wire_fat_root, &F, name)) {
				h_put("-");
			} else if (!wire_fat_open_file(&F, name, &s, &size)) {
				h_error(&c, "cannot open the package");
				continue;
			} else {
				h_put(name);
				install(&s, size, S11_FORM_FILE, false,
					h_num(&c, 1) != 0, name);
				wire_fat_close_file(&F);
			}
			h_end();
		} else {
			h_error(&c, "unknown command");
		}
	}
	return h_status();
}
