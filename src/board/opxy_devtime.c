/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A developer host's timing requests (waffle/devhost.h, wiki/Developer-Host-Link.md):
 * how fast each stage of the package check (S-11.020, checks 1-15) runs on
 * the unit, where a check of the disk's package spends its time, and where
 * its extraction (checks 16-18) would. Times are core cycles. Nothing is
 * written but this file's own buffers and the screen's frame: the package,
 * the disk and the slave's volume stay as they were.
 */
#include <string.h>

#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_bridge.h"
#include "waffle/board/opxy_ops.h"
#include "waffle/board/opxy_services.h"
#include "waffle/bytes.h"
#include "waffle/devhost.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_usb.h"
#include "waffle/s09_bridge.h"
#include "waffle/s09_files.h"
#include "waffle/s09_master.h"
#include "waffle/s11_content.h"
#include "waffle/s11_staging.h"
#include "waffle/s13c_panel.h"
#include "waffle/wire_package.h"
#include "waffle/wire_zip.h"

#if !defined(OPXY_TEST_OS)

#define PIECE 0x8000u		/* the check's piece (src/s11_package.c) */

static _Alignas(4) uint8_t piece[PIECE];

/* The dispatcher is reached from inside waits too (the PSoC's service runs
 * USB): no timing request while a long operation holds the menu's keys, a
 * scan or an install runs, or the disk's volume is mounted (a host-link
 * transfer's file is open on it). */
static bool busy(const struct opxy_master *m)
{
	return s15_menu_held(&m->sv.menu) || m->sv.working || m->sv.fat.mounted;
}

/* --- OPXY_DEV_BENCH: one stage over the piece, again and again --- */

size_t opxy_dev_bench(struct opxy_master *m, const uint8_t *req, size_t n,
		      uint8_t *out)
{
	static struct wire_crypto c;
	static const uint8_t key[32] = { 0x42 };
	struct opxy_services *sv = &m->sv;
	/* the last part that holds sectors (one on the OP-1F) */
	const struct opxy_disk_part *part =
		&sv->disk.part[sv->disk.part[OPXY_DISK_PARTS - 1u].sectors != 0u ?
			       OPXY_DISK_PARTS - 1u : 0u];
	const uint8_t stage = n >= 3u ? req[0] : 0xFFu;
	const uint32_t kib = n >= 3u ? (uint32_t)req[1] | (uint32_t)req[2] << 8 : 0u;
	uint32_t bytes = kib * 1024u, done;
	const struct s15_geometry *g = NULL;
	const struct s11_crypto_ops *ops = &wire_crypto_ops;
	const bool sha = stage == OPXY_BENCH_SHA || stage == OPXY_BENCH_SHA_SOFT;
	uint8_t iv[16] = { 0 };
	uint16_t crc = 0;
	uint64_t t0;

	if (stage > OPXY_BENCH_AES_SOFT || kib == 0u || kib > OPXY_BENCH_KIB_MAX) {
		out[0] = OPXY_DEV_BAD;
		return 1u;
	}
	if (stage == OPXY_BENCH_FRAME)
		g = opxy_display_geometry();
	if (busy(m) ||
	    (stage == OPXY_BENCH_READ &&
	     (part->mem == NULL || part->sectors < bytes / WB_SECTOR_SIZE)) ||
	    (stage == OPXY_BENCH_FRAME && g == NULL)) {
		out[0] = OPXY_DEV_REFUSED;
		return 1u;
	}
	if (stage < OPXY_BENCH_SHA_SOFT)
		ops = opxy_crypto(m);
	if (stage == OPXY_BENCH_CRC)
		opxy_crc_begin(m);
	if (sha)
		ops->sha256_init(&c);
	t0 = wb_core_cycles();
	for (done = 0; done < bytes; done += PIECE) {
		const uint32_t len = bytes - done < PIECE ? bytes - done : PIECE;

		switch (stage) {
		case OPXY_BENCH_READ:
			wb_blockdev_copy(piece, part->mem + done, len);
			break;
		case OPXY_BENCH_CRC:
			crc = ops->crc16(&c, crc, piece, len);
			break;
		case OPXY_BENCH_SHA:
		case OPXY_BENCH_SHA_SOFT:
			ops->sha256_update(&c, piece, len);
			break;
		case OPXY_BENCH_AES:
		case OPXY_BENCH_AES_SOFT:
			ops->aes256_cbc_decrypt(&c, key, iv, piece, piece, len);
			break;
		default:	/* one frame drawn, whatever kib is */
			s15_menu_render(&sv->menu, g, &sv->draw);
			bytes = done = S13C_FRAME_BYTES;
			break;
		}
	}
	t0 = wb_core_cycles() - t0;
	if (sha) {
		uint8_t digest[32];

		ops->sha256_final(&c, digest);
	}
	out[1] = stage;
	wb_put_le32(out + 2, bytes);
	wb_put_le64(out + 6, t0);
	out[14] = opxy_crypto_engine();
	out[15] = opxy_crc_engine(OPXY_CRC16);
	out[16] = opxy_crc_engine(OPXY_CRC32);
	opxy_crypto_end();
	opxy_crc_end();
	return 17u;
}

/* --- OPXY_DEV_CHECK: the check itself, each stage timed --- */

static struct {
	const struct s11_crypto_ops *crypto;
	bool (*read)(void *ctx, uint32_t off, uint8_t *buf, size_t n);
	uint64_t t[OPXY_CHECK_STAGES];
	/* For OPXY_DEV_EXTRACT's reader: whether the zip is encrypted, the
	 * key, the chain value before the last AES call and the one before
	 * the zip's first piece; where the zip is and its length. */
	bool aes;
	uint8_t key[32], iv_before[16], zip_iv[16];
	uint32_t zip_at, zip_len;
} timed;

static void took(unsigned stage, uint64_t t0)
{
	timed.t[stage] += wb_core_cycles() - t0;
}

static uint16_t t_crc16(void *ctx, uint16_t crc, const uint8_t *p, size_t n)
{
	const uint64_t t0 = wb_core_cycles();

	crc = timed.crypto->crc16(ctx, crc, p, n);
	took(OPXY_CHECK_CRC, t0);
	return crc;
}

static void t_sha_init(void *ctx)
{
	timed.crypto->sha256_init(ctx);
}

static void t_sha_update(void *ctx, const uint8_t *p, size_t n)
{
	const uint64_t t0 = wb_core_cycles();

	timed.crypto->sha256_update(ctx, p, n);
	took(OPXY_CHECK_SHA, t0);
}

static void t_sha_final(void *ctx, uint8_t digest[32])
{
	timed.crypto->sha256_final(ctx, digest);
}

static void t_aes(void *ctx, const uint8_t key[32], uint8_t iv[16],
		  const uint8_t *in, uint8_t *out, size_t n)
{
	uint64_t t0;

	timed.aes = true;
	memcpy(timed.key, key, sizeof(timed.key));
	memcpy(timed.iv_before, iv, sizeof(timed.iv_before));
	t0 = wb_core_cycles();
	timed.crypto->aes256_cbc_decrypt(ctx, key, iv, in, out, n);
	took(OPXY_CHECK_AES, t0);
}

static bool t_verify(void *ctx, const uint8_t q[56], const uint8_t digest[32],
		     const uint8_t r[28], const uint8_t s[28])
{
	const uint64_t t0 = wb_core_cycles();
	const bool ok = timed.crypto->p224_verify(ctx, q, digest, r, s);

	took(OPXY_CHECK_SIG, t0);
	return ok;
}

static const struct s11_crypto_ops timed_crypto = {
	t_crc16, t_sha_init, t_sha_update, t_sha_final, t_aes, t_verify,
};

/* The read, then USB: the host keeps its answers while the check holds
 * the loop. */
static bool t_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	uint64_t t0 = wb_core_cycles();
	const bool ok = timed.read(ctx, off, buf, n);

	took(OPXY_CHECK_READ, t0);
	t0 = wb_core_cycles();
	opxy_usb_breathe();
	took(OPXY_CHECK_USB, t0);
	return ok;
}

/* The zip's bytes are dropped, so check 15 has nothing to read: a package
 * that passes here passed checks 1 to 14. Piece z of the zip is at package
 * offset at; the first was decrypted from the chain value before it. */
static bool t_discard(void *ctx, uint32_t z, uint32_t at, const uint8_t *p,
		      size_t n)
{
	(void)ctx;
	(void)p;
	if (z == 0u) {
		timed.zip_at = at;
		memcpy(timed.zip_iv, timed.iv_before, sizeof(timed.zip_iv));
	}
	timed.zip_len += (uint32_t)n;
	return true;
}

static bool t_readable(void *ctx, uint32_t len)
{
	(void)ctx;
	(void)len;
	return true;
}

/*
 * Checks 1-15 on the disk's package as a scan runs them, each stage timed,
 * the zip's bytes dropped. The disk is read as the host left it, mounted for
 * reading only and not taken from the host, so a host that is still writing
 * gives a wrong answer and nothing worse. False, with nothing mounted, when
 * the volume is mounted for someone else (a host-link transfer's file is
 * open on it) or holds no package. Otherwise the package is left open as
 * src on the mounted volume, for the caller to close and unmount.
 */
static bool check_disk(struct opxy_master *m, struct wire_source *src,
		       uint32_t *size, enum s11_reason *r)
{
	static struct wire_package pkg;
	struct opxy_services *sv = &m->sv;
	struct s11_package_ops ops;
	struct s11_check_in in;
	struct s11_check_out checked;
	char name[S11_PATH_MAX];
	uint64_t t0;

	*size = 0;
	opxy_usb_gap_begin();	/* S-10a.049: how long the bus goes unserviced */
	if (busy(m) || sv->disk.dev.n_sectors == 0u ||
	    !wire_fat_mount(&sv->fat, &sv->disk.dev))
		return false;
	if (!s11_disk_pick(wire_fat_root, &sv->fat, name) ||
	    !wire_fat_open_file(&sv->fat, name, src, size)) {
		wire_fat_unmount(&sv->fat);
		return false;
	}
	wire_package_init(&pkg, &ops, src, &opxy_unit_ops, NULL, NULL, 0u);
	memset(&timed, 0, sizeof(timed));
	timed.crypto = opxy_crypto(m);
	opxy_crc_begin(m);
	timed.read = ops.read;
	ops.crypto = &timed_crypto;
	ops.read = t_read;
	ops.zip_write = t_discard;
	ops.zip_readable = t_readable;
	in.form = S11_FORM_FILE;
	in.size = *size;
	in.option = sv->option;
	in.file_name = name;
	in.fw_command = false;
	in.dev = opxy_dev(m);
	t0 = wb_core_cycles();
	*r = s11_check_package(&ops, &in, &checked);
	took(OPXY_CHECK_TOTAL, t0);
	return true;
}

static void wipe_key(uint8_t *key, size_t n)
{
	volatile uint8_t *p = key;

	while (n-- > 0u)
		*p++ = 0u;
}

size_t opxy_dev_check(struct opxy_master *m, uint8_t *out)
{
	struct opxy_services *sv = &m->sv;
	struct wire_source src;
	uint32_t size;
	enum s11_reason r;
	unsigned i;

	if (!check_disk(m, &src, &size, &r)) {
		out[0] = OPXY_DEV_REFUSED;
		return 1u;
	}
	wire_fat_close_file(&sv->fat);
	wire_fat_unmount(&sv->fat);
	wipe_key(timed.key, sizeof(timed.key));

	out[1] = (uint8_t)r;
	wb_put_le32(out + 2, size);
	for (i = 0; i < OPXY_CHECK_STAGES; i++)
		wb_put_le64(out + 6u + 8u * i, timed.t[i]);
	out[6u + 8u * OPXY_CHECK_STAGES] = opxy_crypto_engine();
	out[7u + 8u * OPXY_CHECK_STAGES] = opxy_crc_engine(OPXY_CRC16);
	out[8u + 8u * OPXY_CHECK_STAGES] = opxy_crc_engine(OPXY_CRC32);
	opxy_crypto_end();
	opxy_crc_end();
	wb_put_le32(out + 9u + 8u * OPXY_CHECK_STAGES, opxy_usb_gap_end());
	return 13u + 8u * OPXY_CHECK_STAGES;
}

/* --- OPXY_DEV_EXTRACT: checks 16-18 on the zip in the package --- */

#define DIRS_MAX  512u
#define FNV_BASIS 0xCBF29CE484222325u
#define FNV_PRIME 0x100000001B3u

static struct {
	uint64_t t[OPXY_EXTRACT_STAGES];
	uint32_t n[OPXY_EXTRACT_COUNTS];
	uint64_t bytes;
	uint64_t sink;			/* in the extraction's sink */
	uint64_t own;			/* in the counting below, left out */
	struct s11_zip_ops zip;		/* wire_zip's own */
	s11_sink_fn to;			/* the sink of the extraction running */
	uint32_t wmax;			/* a file write's most bytes on the rung */
	uint64_t dir[DIRS_MAX];		/* the directories made, by hash */
	unsigned dirs;
	bool full;			/* dir has no room left */
} ext;

static struct wire_crypto soft;		/* the reader's AES */

static void ext_took(unsigned stage, uint64_t t0)
{
	ext.t[stage] += wb_core_cycles() - t0;
}

/* The time spent in the reader so far. */
static uint64_t in_reader(void)
{
	return ext.t[OPXY_EXTRACT_READ] + ext.t[OPXY_EXTRACT_DECRYPT] +
	       ext.t[OPXY_EXTRACT_USB];
}

static bool x_read(const struct opxy_zip_reader *x, uint32_t off,
		   uint8_t *buf, size_t n)
{
	const uint64_t t0 = wb_core_cycles();
	const bool ok = x->read(x->ctx, off, buf, n);

	ext_took(OPXY_EXTRACT_READ, t0);
	return ok;
}

/* Bytes [z, z + n) of the zip: the ciphertext blocks that cover them, a
 * piece at a time, decrypted from the block before the first (the chain
 * value iv at the zip's start), and the wanted bytes copied out. */
static bool x_decrypt(const struct opxy_zip_reader *x, uint32_t z,
		      uint8_t *buf, size_t n)
{
	const uint64_t end = ((uint64_t)z + n + 15u) & ~(uint64_t)15u;
	uint64_t b = z & ~(uint32_t)15u;
	uint8_t iv[16];

	if (b == 0u)
		memcpy(iv, x->iv, sizeof(iv));
	else if (!x_read(x, x->at + (uint32_t)b - 16u, iv, sizeof(iv)))
		return false;
	while (b < end) {
		const uint32_t k = end - b < PIECE ? (uint32_t)(end - b) : PIECE;
		const uint64_t from = z > b ? z - b : 0u;
		const uint64_t to = (uint64_t)z + n < b + k ? (uint64_t)z + n - b : k;
		uint64_t t0;

		if (!x_read(x, x->at + (uint32_t)b, piece, k))
			return false;
		t0 = wb_core_cycles();
		wire_crypto_ops.aes256_cbc_decrypt(&soft, x->key, iv, piece,
						   piece, k);
		ext_took(OPXY_EXTRACT_DECRYPT, t0);
		memcpy(buf + (size_t)(b + from - z), piece + from,
		       (size_t)(to - from));
		b += k;
	}
	return true;
}

bool opxy_zip_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	const struct opxy_zip_reader *x = ctx;
	uint64_t t0;
	bool ok;

	if (off < x->at || off - x->at > x->len || n > x->len - (off - x->at))
		return false;
	if (n == 0u)
		return true;
	ok = x->encrypted ? x_decrypt(x, off - x->at, buf, n) :
			    x_read(x, off, buf, n);
	/* USB, as the check's reads do */
	t0 = wb_core_cycles();
	opxy_usb_breathe();
	ext_took(OPXY_EXTRACT_USB, t0);
	return ok;
}

static bool t_member(void *ctx, size_t index, struct s11_member *mb)
{
	const uint64_t r0 = in_reader(), t0 = wb_core_cycles();
	const bool ok = ext.zip.member(ctx, index, mb);

	ext.t[OPXY_EXTRACT_MEMBER] += wb_core_cycles() - t0 - (in_reader() - r0);
	return ok;
}

static bool t_sink(void *arg, const uint8_t *p, size_t n)
{
	const uint64_t t0 = wb_core_cycles();
	const bool ok = ext.to(arg, p, n);

	ext.sink += wb_core_cycles() - t0;
	return ok;
}

/* Inflate: the extraction less its reads and its sink, which holds the
 * CRC-32 and the staging writes. */
static bool t_extract(void *ctx, size_t index, s11_sink_fn sink, void *arg)
{
	const uint64_t r0 = in_reader(), s0 = ext.sink, t0 = wb_core_cycles();
	bool ok;

	ext.to = sink;
	ok = ext.zip.extract(ctx, index, t_sink, arg);
	ext.t[OPXY_EXTRACT_INFLATE] += wb_core_cycles() - t0 -
				       (in_reader() - r0) - (ext.sink - s0);
	return ok;
}

static uint32_t t_crc32(void *ctx, uint32_t crc, const uint8_t *p, size_t n)
{
	const uint64_t t0 = wb_core_cycles();

	crc = ext.zip.crc32(ctx, crc, p, n);
	ext_took(OPXY_EXTRACT_CRC, t0);
	return crc;
}

static uint64_t fnv(uint64_t h, char c)
{
	return (h ^ (uint8_t)c) * FNV_PRIME;
}

/* Whether the directory of hash h was made earlier; made now if not. A
 * table with no room left takes every new one as made. */
static bool dir_made(uint64_t h)
{
	for (unsigned i = 0; i < ext.dirs; i++) {
		if (ext.dir[i] == h)
			return true;
	}
	if (ext.dirs == DIRS_MAX) {
		ext.full = true;
		return true;
	}
	ext.dir[ext.dirs++] = h;
	return false;
}

/*
 * The requests the staging area would send for stage_path()'s path,
 * upgrade/<rel> with a '/' added for a directory: a mkdir for each
 * directory of it below those the last path made (s09_files.c remembers
 * them). A mkdir of a directory that is there already is taken to fail
 * (yaffs2's EEXIST) and to be followed by one stat; a directory is there
 * when the staging area's reset made it ("upgrade") or an earlier path's
 * mkdir did.
 */
static void count_dirs(const char *rel, bool dir)
{
	static const char up[] = "upgrade/";
	static char made[sizeof(up) + S11_PATH_MAX + 1u];
	static size_t made_len;
	char path[sizeof(up) + S11_PATH_MAX + 1u];
	size_t n = 0, skip = 0, last = 0;
	uint64_t h = FNV_BASIS;

	if (ext.dirs <= 1u)		/* a reset has just been: none made */
		made_len = 0;
	for (size_t i = 0; up[i] != '\0'; i++)
		path[n++] = up[i];
	for (size_t i = 0; rel[i] != '\0' && n < sizeof(path) - 1u; i++)
		path[n++] = rel[i];
	if (dir && path[n - 1u] != '/')
		path[n++] = '/';
	for (size_t i = 0; i <= made_len && i < n; i++) {
		if (path[i] == '/' && (i == made_len || made[i] == '/'))
			skip = i;
		if (i == made_len || path[i] != made[i])
			break;
	}
	for (size_t i = 0; i < n; i++) {
		if (i > 0u && path[i] == '/') {
			last = i;
			if (i > skip) {
				ext.n[OPXY_EXTRACT_MKDIRS]++;
				if (dir_made(h))
					ext.n[OPXY_EXTRACT_STATS]++;
			}
		}
		h = fnv(h, path[i]);
	}
	memcpy(made, path, last);
	made_len = last;
}

/* The staging area as the extraction would use it: nothing is sent, the
 * calls and the requests they would make are counted. */
static bool x_reset(void *ctx)
{
	uint64_t h = FNV_BASIS;

	(void)ctx;
	for (const char *p = "upgrade"; *p != '\0'; p++)
		h = fnv(h, *p);
	ext.dirs = 0;
	(void)dir_made(h);
	return true;
}

static bool x_mkdir(void *ctx, const char *path)
{
	const uint64_t t0 = wb_core_cycles();

	(void)ctx;
	ext.n[OPXY_EXTRACT_DIRS]++;
	count_dirs(path, true);
	ext.own += wb_core_cycles() - t0;
	return true;
}

static bool x_open(void *ctx, const char *path)
{
	const uint64_t t0 = wb_core_cycles();

	(void)ctx;
	ext.n[OPXY_EXTRACT_FILES]++;
	count_dirs(path, false);
	ext.own += wb_core_cycles() - t0;
	return true;
}

/* s09_files_staging_write(): each piece in writes of at most what the
 * link's rung takes (S-09.023). */
static bool x_write(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	(void)p;
	ext.n[OPXY_EXTRACT_WRITES]++;
	ext.bytes += n;
	ext.n[OPXY_EXTRACT_WRITE_REQS] += (uint32_t)((n + ext.wmax - 1u) / ext.wmax);
	return true;
}

static bool x_close(void *ctx)
{
	(void)ctx;
	return true;
}

/* Room for any zip, and no request sent for it. */
static bool x_free(void *ctx, uint64_t *bytes)
{
	(void)ctx;
	*bytes = UINT64_MAX / 2u;
	return true;
}

/* The probes' payload: never written anywhere. */
static _Alignas(4) uint8_t zeros[S09_WRITE_MAX];

struct batch {
	uint32_t sent;
	uint64_t cycles;
	uint32_t err;			/* the first request's errno */
};

/*
 * One batch of requests, timed together; false when a write succeeded, at
 * which it stops. The writes are s09_files_write()'s request (len and arg1
 * the length, arg0 the descriptor), sent through s09_master_call() so that
 * any result of 0 or more, a short write too, shows. No descriptor is
 * OPXY_PROBE_FD, so the slave refuses each before the volume is touched
 * (EBADF). Every request is one S-09.023 allows.
 */
static bool probe(struct s09_files *f, unsigned kind, struct batch *b)
{
	const uint32_t len = s09_master_write_max(f->m);
	const struct s09_req w = { S09_WRITE, 0, (uint16_t)len, OPXY_PROBE_FD,
				   len };
	const uint64_t t0 = wb_core_cycles();

	for (uint32_t i = 0; i < OPXY_PROBE_N; i++) {
		uint32_t err = 0;
		int32_t r;

		if (kind == OPXY_PROBE_STAT) {
			struct s09_stat st;

			/* the volume's root, as the free-space query names it */
			(void)s09_files_stat(f, "/yaffs2", &st);
			err = f->err;
			r = -1;		/* a stat writes nothing */
		} else {
			uint32_t got;

			r = s09_master_call(f->m, &w, zeros, len, NULL, 0u, &got,
					    &err);
		}
		if (i == 0u)
			b->err = err;
		b->sent++;
		if (r >= 0)
			return false;
	}
	b->cycles = wb_core_cycles() - t0;
	return true;
}

/* The batches, in order; the flags they leave. */
static uint8_t probe_all(struct opxy_master *m,
			 struct batch b[OPXY_PROBE_BATCHES])
{
	struct s09_files *f = &m->sv.files;

	memset(b, 0, sizeof(b[0]) * OPXY_PROBE_BATCHES);
	if (!m->slave_present || f->m == NULL || f->m->absent)
		return OPXY_EXTRACT_NO_SLAVE;
	for (unsigned k = 0; k < OPXY_PROBE_BATCHES; k++) {
		if (!probe(f, k, &b[k])) {
			memset(b, 0, sizeof(b[0]) * OPXY_PROBE_BATCHES);
			return OPXY_EXTRACT_WROTE;
		}
		if (f->m->absent)
			return OPXY_EXTRACT_LOST;
	}
	return 0u;
}

/* Checks 16-18 on a view of the package whose reader is opxy_zip_read(),
 * every stage timed. */
static enum s11_reason extract(const struct wire_source *src)
{
	static struct opxy_zip_reader rd;
	static struct wire_package view;
	static struct wire_zip zip;
	struct s11_zip_ops zops;
	struct s11_staged_sizes sizes;
	enum s11_reason r;
	uint64_t t0;

	rd.read = src->read;
	rd.ctx = src->ctx;
	rd.encrypted = timed.aes;
	memcpy(rd.key, timed.key, sizeof(rd.key));
	memcpy(rd.iv, timed.zip_iv, sizeof(rd.iv));
	rd.at = timed.zip_at;
	rd.len = timed.zip_len;
	memset(&view, 0, sizeof(view));
	view.src.read = opxy_zip_read;
	view.src.ctx = &rd;
	view.src.write = NULL;
	view.zip = NULL;
	view.zip_at = rd.at;
	view.zip_len = rd.len;
	wire_zip_init(&zip, &zops, &view, NULL, 0, NULL);
	ext.zip = zops;
	ext.zip.crc32 = opxy_crc32;	/* an install's (opxy_scan.c) */
	zops.member = t_member;
	zops.extract = t_extract;
	zops.crc32 = t_crc32;
	zops.free_space = x_free;
	zops.staging_reset = x_reset;
	zops.staging_mkdir = x_mkdir;
	zops.staging_open = x_open;
	zops.staging_write = x_write;
	zops.staging_close = x_close;
	t0 = wb_core_cycles();
	r = s11_check_zip(&zops, &sizes);
	ext_took(OPXY_EXTRACT_TOTAL, t0);
	ext.t[OPXY_EXTRACT_TOTAL] -= ext.own;	/* the directory count's time */
	wire_zip_close(&zip);
	wipe_key(rd.key, sizeof(rd.key));
	wipe_key((uint8_t *)&soft, sizeof(soft));	/* the key schedule */
	return r;
}

size_t opxy_dev_extract(struct opxy_master *m, uint8_t *out)
{
	struct opxy_services *sv = &m->sv;
	struct batch b[OPXY_PROBE_BATCHES];
	struct wire_source src;
	uint32_t size;
	enum s11_reason r;
	uint8_t flags = 0, crc[2];
	size_t o = 3u;
	unsigned i;
	bool checked;

	if (!check_disk(m, &src, &size, &r)) {
		out[0] = OPXY_DEV_REFUSED;
		return 1u;
	}
	memset(&ext, 0, sizeof(ext));
	memset(b, 0, sizeof(b));
	checked = r == S11_REASON_NONE;
	/* S-09.075: the link as an install's extraction would have it */
	if (checked)
		opxy_bridge_fast(m);
	ext.wmax = s09_master_write_max(&m->bridge);
	if (checked) {
		ext.t[OPXY_EXTRACT_CHECK] = timed.t[OPXY_CHECK_TOTAL];
		r = extract(&src);
	}
	wire_fat_close_file(&sv->fat);
	wire_fat_unmount(&sv->fat);
	wipe_key(timed.key, sizeof(timed.key));
	crc[0] = opxy_crc_engine(OPXY_CRC16);
	crc[1] = opxy_crc_engine(OPXY_CRC32);
	opxy_crypto_end();
	opxy_crc_end();
	/* checks 1-15 refused: the reason and zeros */
	if (checked) {
		flags = probe_all(m, b);
		if (ext.full)
			flags |= OPXY_EXTRACT_DIRS_FULL;
	}

	out[1] = (uint8_t)r;
	out[2] = flags;
	for (i = 0; i < OPXY_EXTRACT_COUNTS; i++, o += 4u)
		wb_put_le32(out + o, ext.n[i]);
	wb_put_le64(out + o, ext.bytes);
	o += 8u;
	for (i = 0; i < OPXY_EXTRACT_STAGES; i++, o += 8u)
		wb_put_le64(out + o, ext.t[i]);
	for (i = 0; i < OPXY_PROBE_BATCHES; i++, o += 16u) {
		wb_put_le32(out + o, b[i].sent);
		wb_put_le64(out + o + 4u, b[i].cycles);
		wb_put_le32(out + o + 12u, b[i].err);
	}
	out[o++] = crc[0];
	out[o++] = crc[1];
	{
		const struct s09_record *rec = s09_master_record(&m->bridge);

		/* the link the extraction and the probes ran on */
		out[o++] = (uint8_t)s09_master_rung(&m->bridge);
		out[o++] = (uint8_t)(rec->falls < 0xFFu ? rec->falls : 0xFFu);
		out[o++] = (uint8_t)(ext.wmax / 1024u);
		wb_put_le32(out + o, opxy_usb_gap_end());
		o += 4u;
		out[o++] = rec->nrungs;
	}
	/* S-09.078: the reply says where the extraction ran; the link goes
	 * back to the plain mode for what comes after */
	opxy_bridge_plain(m);
	if (m->slave_present && !s09_master_settled(&m->bridge))
		out[2] |= OPXY_EXTRACT_UNSETTLED;	/* S-09.074 */
	return o;
}

/* --- OPXY_DEV_LINK_TEST: the link bench --- */

static struct s09_bench bench;
static bool bench_ran;

static void bench_between(void *ctx)
{
	(void)ctx;
	opxy_usb_breathe();
}

static size_t bench_part(unsigned part, uint8_t *out)
{
	size_t o = 4u;

	out[1] = (uint8_t)part;
	out[2] = OPXY_LINK_TEST_PARTS;
	out[3] = 0;
	for (unsigned i = part * OPXY_LINK_TEST_PER;
	     i < (part + 1u) * OPXY_LINK_TEST_PER && i < OPXY_LINK_TEST_ROWS; i++) {
		/* rungs 1-4, each length; then the plain rung */
		const unsigned rung = i < S09_RUNG_TOP * S09_BENCH_LENS ?
				      1u + i / S09_BENCH_LENS : 0u;
		const unsigned l = i % S09_BENCH_LENS;
		const struct s09_bench_cell *c = &bench.cell[rung][l];

		out[3]++;
		out[o++] = (uint8_t)rung;
		wb_put_le32(out + o, s09_bench_len[l]);
		o += 4u;
		out[o++] = (uint8_t)c->sent;
		out[o++] = (uint8_t)(c->sent >> 8);
		out[o++] = (uint8_t)c->passed;
		out[o++] = (uint8_t)(c->passed >> 8);
		wb_put_le32(out + o, c->us);
		o += 4u;
		for (unsigned k = 0; k < S09_AT_COUNT; k++) {
			out[o++] = (uint8_t)c->at[k];
			out[o++] = (uint8_t)(c->at[k] >> 8);
		}
	}
	return o;
}

size_t opxy_dev_link_test(struct opxy_master *m, const uint8_t *req, size_t n,
			  uint8_t *out)
{
	const uint8_t sub = n >= 1u ? req[0] : 0xFFu;

	if (sub == OPXY_LINK_TEST_PART) {
		const unsigned part = n >= 2u ? req[1] : OPXY_LINK_TEST_PARTS;

		if (part >= OPXY_LINK_TEST_PARTS) {
			out[0] = OPXY_DEV_BAD;
			return 1u;
		}
		if (!bench_ran) {
			out[0] = OPXY_DEV_REFUSED;
			return 1u;
		}
		return bench_part(part, out);
	}
	if (sub != OPXY_LINK_TEST_RUN) {
		out[0] = OPXY_DEV_BAD;
		return 1u;
	}
	/* as dev-extract: not inside a long operation, a scan or an install */
	if (busy(m) || !m->slave_present || m->bridge.absent) {
		out[0] = OPXY_DEV_REFUSED;
		return 1u;
	}
	{
		const unsigned count = n >= 2u ? req[1] : OPXY_LINK_TEST_N;
		const uint64_t t0 = wb_core_cycles();
		bool ok;

		if (count == 0u) {
			out[0] = OPXY_DEV_BAD;
			return 1u;
		}
		memset(&bench, 0, sizeof(bench));
		bench.n = (uint16_t)count;
		bench.limit_us = OPXY_LINK_TEST_LIMIT_S * 1000000u;
		bench.between = bench_between;
		ok = s09_master_link_bench(&m->bridge, &bench);
		m->sv.files.link = 0;		/* S-09.078: plain after it */
		bench_ran = true;
		out[1] = OPXY_LINK_TEST_PARTS;
		out[2] = (uint8_t)count;
		out[3] = (uint8_t)((ok ? 0u : OPXY_LINK_TEST_LOST) |
				   (bench.cut ? OPXY_LINK_TEST_CUT : 0u) |
				   (bench.settled ? 0u : OPXY_LINK_TEST_UNSETTLED));
		for (unsigned r = 0; r <= S09_RUNG_TOP; r++) {
			out[4u + r] = bench.entry[r];
			out[9u + r] = bench.entries[r];
		}
		wb_put_le64(out + 14, wb_core_cycles() - t0);
		out[22] = (uint8_t)(bench.fell < 0xFFFFu ? bench.fell : 0xFFFFu);
		out[23] = (uint8_t)((bench.fell < 0xFFFFu ? bench.fell : 0xFFFFu) >> 8);
		out[24] = (uint8_t)OPXY_LINK_TEST_LIMIT_S;
		out[25] = (uint8_t)(OPXY_LINK_TEST_LIMIT_S >> 8);
		out[26] = bench.stopped;
		out[27] = bench.end_rung;
		wb_put_le32(out + 28, bench.end_len);
		out[32] = (uint8_t)bench.end_req;
		out[33] = (uint8_t)(bench.end_req >> 8);
		wb_put_le32(out + 34, s09_master_record(&m->bridge)->bench_faults);
		return OPXY_LINK_TEST_REPLY;
	}
}

/* --- OPXY_DEV_QUAD_REPLY: the quad reply test, one cell --- */

static struct s09_qt_cell cell;

static size_t put16(uint8_t *out, size_t o, uint32_t v)
{
	out[o] = (uint8_t)v;
	out[o + 1u] = (uint8_t)(v >> 8);
	return o + 2u;
}

static size_t put32(uint8_t *out, size_t o, uint32_t v)
{
	wb_put_le32(out + o, v);
	return o + 4u;
}

size_t opxy_dev_quad_reply(struct opxy_master *m, const uint8_t *req, size_t n,
			   uint8_t *out)
{
	const uint32_t len = n >= 7u ? wb_get_le32(req + 3) : 0u;
	uint32_t count = n >= 9u ? (uint32_t)req[7] | (uint32_t)req[8] << 8 : 0u;
	uint64_t t0;
	size_t o = 1u;
	bool ok;

	if (count == 0u)
		count = OPXY_QUAD_N;
	if (n < 7u || req[1] > 1u || count > OPXY_QUAD_N_MAX) {
		out[0] = OPXY_DEV_BAD;
		return 1u;
	}
	/* as the link test: not inside a long operation, a scan or an install */
	if (busy(m) || !m->slave_present || m->bridge.absent) {
		out[0] = OPXY_DEV_REFUSED;
		return 1u;
	}
	memset(&cell, 0, sizeof(cell));
	cell.cand = req[0];
	cell.ren = req[1] != 0u;
	cell.clock = req[2];
	cell.len = len;
	cell.n = (uint16_t)count;
	cell.limit_us = OPXY_QUAD_LIMIT_S * 1000000u;
	cell.between = bench_between;
	t0 = wb_core_cycles();
	ok = s09_master_quad_test(&m->bridge, &cell);
	t0 = wb_core_cycles() - t0;
	m->sv.files.link = 0;		/* S-09.078: plain after it */
	out[o++] = cell.cand;
	out[o++] = cell.ren ? 1u : 0u;
	out[o++] = cell.clock;
	o = put32(out, o, cell.len);
	o = put16(out, o, cell.n);
	out[o++] = (uint8_t)((ok ? 0u : OPXY_QUAD_LOST) | (cell.cut ? OPXY_QUAD_CUT : 0u) |
			     (ok && !cell.refused && !cell.settled ? OPXY_QUAD_UNSETTLED : 0u) |
			     (cell.refused ? OPXY_QUAD_REFUSED : 0u));
	out[o++] = cell.why;
	o = put16(out, o, cell.sent);
	o = put16(out, o, cell.whole);
	o = put16(out, o, cell.flag);
	o = put16(out, o, cell.missing);
	o = put16(out, o, cell.last_wrong);
	o = put16(out, o, cell.word_wrong);
	o = put16(out, o, cell.broke);
	o = put16(out, o, cell.no_record);
	for (unsigned k = 0; k < S09_QE_COUNT; k++)
		o = put16(out, o, cell.ends[k]);
	o = put32(out, o, cell.dmin);
	o = put32(out, o, cell.dmax);
	out[o++] = cell.lines;
	o = put32(out, o, cell.m_stat_or);
	o = put32(out, o, cell.m_stat_and);
	o = put32(out, o, cell.s_stat_or);
	wb_put_le64(out + o, t0);
	o += 8u;
	out[o++] = cell.first;
	o = put16(out, o, cell.first_no);
	o = put32(out, o, cell.first_word);
	for (unsigned k = 0; k < 4u; k++)
		o = put32(out, o, cell.first_m[k]);
	for (unsigned k = 0; k < S09_QT_REC_WORDS; k++)
		o = put32(out, o, cell.first_rec[k]);
	return o;
}

#endif /* !OPXY_TEST_OS */
