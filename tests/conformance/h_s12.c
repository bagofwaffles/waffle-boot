/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for S-12's filesystems: the RAM disk on FatFs
 * (src/wire_fat.c) and the yaffs2 volume (src/wire_yaffs.c), each on a RAM
 * block device, with the scans that read the disk's root (s11_disk_pick,
 * S-04.024's CDC check) and the log export (s14_export_run). The images are
 * tests/oracle/S-12-filesystems/gen_vectors.py --images's and are read back by the
 * conformance package's readers.
 *
 * Paths and names are hex UTF-8 ("-": the root); bytes are hex ("-":
 * empty). Yaffs2 paths are relative to /yaffs2 (S-16.117). PAT is the
 * generator's pattern(): byte i is (7 i + SEED) mod 251.
 *
 * Commands (one result line each):
 *   disk SECTORS FILL      -> ok: a RAM disk, every byte FILL
 *   diskload FILE          -> ok | fail: a RAM disk holding the image FILE
 *   disksave FILE          -> ok | fail: the disk's image (sparse)
 *   disksha                -> the SHA-256 of the whole disk
 *   diskwrites             -> sectors written since the last query
 *   diskformat | diskmount -> 1 | 0: wire_fat_format(), wire_fat_mount()
 *   diskls                 -> NAME:d | NAME:f for each root entry
 *                             (wire_fat_root), or "-"
 *   diskpick               -> NAME | "-": s11_disk_pick()
 *   diskcdc                -> 1 | 0: S-04.024 over the root listing
 *   diskmkdir PATH         -> 1 | 0
 *   diskwrite PATH BYTES STAMP UTC -> 1 | 0: wire_fat_write_file()
 *   diskread PATH          -> the file's bytes, or "absent"
 *   nameok NAME            -> 1 | 0: wire_fat_name_ok()
 *   stamp UTC              -> DATE TIME | "-": wire_fat_stamp()
 *   export                 -> N: s14_export_run() from the volume's logs/
 *   yvol BLOCKS RESERVE    -> ok: an all-zero volume, added, not mounted
 *   yload FILE BLOCKS RESERVE -> ok | fail: FILE at the volume's start, the
 *                             rest zero; RESERVE the yaffs2 reserve (blocks)
 *   ysave FILE             -> ok | fail: the volume's image
 *   ybe                    -> 1 | 0: wire_yaffs_big_endian()
 *   ymount                 -> ok | refused | failed
 *   yunmount               -> ok
 *   yformat                -> 1 | 0: wire_yaffs_format()
 *   yfree                  -> BYTES | "-": wire_yaffs_free_space()
 *   ylog                   -> the volume's writes and erases since the last
 *                             query, in order: wC (chunk C), eB (block B);
 *                             or "-"
 *   clock UTC              -> ok: the time yaffs2 stamps headers with
 *   yput PATH BYTES        -> 1 | 0: a file, its directories created
 *   ypat PATH LEN SEED     -> 1 | 0: the same with PAT bytes
 *   ymkdir PATH | yrm PATH -> 1 | 0
 *   ymv FROM TO            -> 1 | 0: struct s11_fs_ops move
 *   ytree                  -> every entry the volume lists, depth first:
 *                             PATH:d, PATH:f:LEN:SHA256, PATH:l:ALIAS
 *                             (yaffs_lstat; a hard link reads as its file);
 *                             or "-"
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bearssl_hash.h"

#include "hlib.h"
#include "waffle/s04_records.h"
#include "waffle/s11_content.h"
#include "waffle/s14_export.h"
#include "waffle/wire_fat.h"
#include "waffle/wire_yaffs.h"

#define BLOCK_BYTES (WB_YAFFS_CHUNKS_PER_BLOCK * WB_YAFFS_CHUNK_BYTES)
/* A name or path argument: longer than any the filesystems take. */
#define ARG_MAX 1024u

static uint8_t line_buf[H_MAXLINE / 2];
static uint8_t file_buf[1u << 16];

static void put_ok(bool b)
{
	h_put(b ? "ok" : "fail");
	h_end();
}

static void bit(bool b)
{
	h_putu(b);
	h_end();
}

/* Field i as a NUL-terminated name or path. */
static const char *path_arg(const struct h_cmd *c, int i, char *out,
			    size_t cap)
{
	size_t n = h_hex(c, i, (uint8_t *)out, cap - 1u);

	out[n] = '\0';
	return out;
}

static void put_name(const char *name, const char *suffix)
{
	char tok[4u * S11_PATH_MAX + 128u];
	size_t n = strlen(name), k = 0;

	for (size_t i = 0; i < n && k + 3u < sizeof(tok); i++)
		k += (size_t)snprintf(tok + k, sizeof(tok) - k, "%02x",
				      (unsigned)(uint8_t)name[i]);
	if (n == 0)
		k = (size_t)snprintf(tok, sizeof(tok), "-");
	snprintf(tok + k, sizeof(tok) - k, "%s", suffix);
	h_put(tok);
}

static uint8_t *pattern(size_t n, unsigned seed)
{
	uint8_t *p = malloc(n ? n : 1u);

	for (size_t i = 0; p != NULL && i < n; i++)
		p[i] = (uint8_t)((i * 7u + seed) % 251u);
	return p;
}

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

/* Writes mem as FILE, leaving all-zero sectors as holes. */
static bool save_file(const char *path, const uint8_t *mem, size_t len)
{
	static const uint8_t zero[WB_SECTOR_SIZE];
	FILE *f = fopen(path, "wb");
	bool good = f != NULL;

	for (size_t o = 0; good && o < len; o += WB_SECTOR_SIZE) {
		if (memcmp(mem + o, zero, WB_SECTOR_SIZE) == 0)
			continue;
		good = fseek(f, (long)o, SEEK_SET) == 0 &&
		       fwrite(mem + o, 1, WB_SECTOR_SIZE, f) == WB_SECTOR_SIZE;
	}
	if (f != NULL) {
		good = fflush(f) == 0 && ftruncate(fileno(f), (off_t)len) == 0 &&
		       good;
		good = fclose(f) == 0 && good;
	}
	return good;
}

/* ---- the RAM disk ---- */

static struct wb_blockdev draw, dbd;
static struct wb_ramdisk dram;
static uint8_t *dmem;
static size_t dmem_len;
static unsigned long dwrites;
static struct wire_fat F;

static int d_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	(void)ctx;
	return draw.read(draw.ctx, lba, buf, count);
}

static int d_write(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
	(void)ctx;
	dwrites += count;
	return draw.write(draw.ctx, lba, buf, count);
}

static bool disk_set(uint8_t *mem, size_t len)
{
	if (F.mounted)
		wire_fat_unmount(&F);
	free(dmem);
	dmem = mem;
	dmem_len = len;
	if (mem == NULL)
		return false;
	wb_ramdisk_init(&draw, &dram, dmem, (uint32_t)(len / WB_SECTOR_SIZE));
	dbd = draw;
	dbd.read = d_read;
	dbd.write = d_write;
	dbd.erase = NULL;
	dwrites = 0;
	return true;
}

static void disk_sha(void)
{
	br_sha256_context sha;
	uint8_t d[32];

	br_sha256_init(&sha);
	br_sha256_update(&sha, dmem, dmem_len);
	br_sha256_out(&sha, d);
	h_puthex(d, sizeof(d));
}

static void disk_ls(void)
{
	char name[S11_PATH_MAX];
	bool is_dir, any = false;

	for (size_t i = 0; F.mounted &&
	     wire_fat_root(&F, i, name, sizeof(name), &is_dir); i++) {
		put_name(name, is_dir ? ":d" : ":f");
		any = true;
	}
	if (!any)
		h_put("-");
}

static bool disk_cdc(void)
{
	char name[S11_PATH_MAX];
	struct s04_cdc_scan s;
	bool is_dir;

	s04_cdc_scan_init(&s);
	for (size_t i = 0; F.mounted &&
	     wire_fat_root(&F, i, name, sizeof(name), &is_dir); i++)
		s04_cdc_scan_entry(&s, name, !is_dir);
	return s04_cdc_scan_result(&s);
}

static void disk_read(const char *path)
{
	struct wire_source s;
	uint32_t size = 0;

	if (!F.mounted || !wire_fat_open_file(&F, path, &s, &size) ||
	    size > sizeof(line_buf) || !s.read(s.ctx, 0, line_buf, size)) {
		if (F.open)
			wire_fat_close_file(&F);
		h_put("absent");
		return;
	}
	wire_fat_close_file(&F);
	h_puthex(line_buf, size);
}

/* ---- the yaffs2 volume ---- */

static struct wb_blockdev yraw, ybd;
static struct wb_ramdisk yram;
static uint8_t *ymem;
static size_t ymem_len;
static struct wire_yaffs Y;
static uint32_t *ylog;
static size_t ylog_n, ylog_cap;
static uint32_t clock_utc;

#define LOG_ERASE 0x80000000u

static void log_op(uint32_t v)
{
	if (ylog_n == ylog_cap) {
		size_t cap = ylog_cap ? 2u * ylog_cap : 1024u;
		uint32_t *p = realloc(ylog, cap * sizeof(*p));

		if (p == NULL)
			return;
		ylog = p;
		ylog_cap = cap;
	}
	ylog[ylog_n++] = v;
}

static int y_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	(void)ctx;
	return yraw.read(yraw.ctx, lba, buf, count);
}

/* Each chunk the write touches, once. */
static int y_write(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
	(void)ctx;
	for (uint32_t c = lba / WB_YAFFS_SECTORS_PER_CHUNK;
	     count != 0 && c <= (lba + count - 1u) / WB_YAFFS_SECTORS_PER_CHUNK;
	     c++)
		log_op(c);
	return yraw.write(yraw.ctx, lba, buf, count);
}

static int y_erase(void *ctx, uint32_t lba, uint32_t count)
{
	(void)ctx;
	for (uint32_t b = lba / WB_YAFFS_SECTORS_PER_BLOCK;
	     count != 0 && b <= (lba + count - 1u) / WB_YAFFS_SECTORS_PER_BLOCK;
	     b++)
		log_op(LOG_ERASE | b);
	return yraw.erase(yraw.ctx, lba, count);
}

static uint32_t now(void)
{
	return clock_utc;
}

/* A volume of `blocks` blocks: `img` (len bytes) at its start, zero after. */
static bool vol_set(const uint8_t *img, size_t len, uint32_t blocks,
		    uint32_t reserve)
{
	size_t want = (size_t)blocks * BLOCK_BYTES;

	wire_yaffs_detach(&Y);
	if (len > want || blocks == 0)
		return false;
	free(ymem);
	ymem = calloc(1, want);
	if (ymem == NULL)
		return false;
	ymem_len = want;
	if (len != 0)
		memcpy(ymem, img, len);
	wb_ramdisk_init(&yraw, &yram, ymem, (uint32_t)(want / WB_SECTOR_SIZE));
	ybd = yraw;
	ybd.read = y_read;
	ybd.write = y_write;
	ybd.erase = y_erase;
	ylog_n = 0;
	if (!wire_yaffs_add(&Y, "/yaffs2", &ybd, 0, blocks - 1u))
		return false;
	Y.dev.param.n_reserved_blocks = reserve;
	return true;
}

static bool y_put(const char *rel, const uint8_t *p, size_t n)
{
	char full[WIRE_YAFFS_PATH_MAX];
	const char *slash = strrchr(rel, '/');
	int fd;
	bool good = true;

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
	while (good && n != 0) {
		unsigned int k = n > 0x10000u ? 0x10000u : (unsigned int)n;

		good = yaffs_write(fd, p, k) == (int)k;
		p += k;
		n -= k;
	}
	return yaffs_close(fd) == 0 && good;
}

static void y_entry(const char *path, bool *any)
{
	char full[WIRE_YAFFS_PATH_MAX], tok[128u + 2u * S11_PATH_MAX];
	struct yaffs_stat st;
	int fd;

	*any = true;
	if (!wire_yaffs_full_path(&Y, path, full) ||
	    yaffs_lstat(full, &st) != 0) {
		put_name(path, ":?");
		return;
	}
	if (S_ISLNK(st.st_mode)) {
		char alias[S11_PATH_MAX] = { 0 };
		size_t k;

		/* 0 on success; the alias is copied NUL-terminated */
		if (yaffs_readlink(full, alias, (int)sizeof(alias) - 1) != 0)
			alias[0] = '\0';
		k = (size_t)snprintf(tok, sizeof(tok), ":l:");
		for (size_t i = 0; alias[i] != '\0' && k + 3u < sizeof(tok); i++)
			k += (size_t)snprintf(tok + k, sizeof(tok) - k, "%02x",
					      (unsigned)(uint8_t)alias[i]);
		put_name(path, tok);
		return;
	}
	if ((fd = yaffs_open(full, O_RDONLY, 0)) >= 0) {
		br_sha256_context sha;
		uint8_t d[32];
		unsigned long len = 0;
		int k;
		size_t o;

		br_sha256_init(&sha);
		while ((k = yaffs_read(fd, file_buf, sizeof(file_buf))) > 0) {
			br_sha256_update(&sha, file_buf, (size_t)k);
			len += (unsigned long)k;
		}
		(void)yaffs_close(fd);
		br_sha256_out(&sha, d);
		o = (size_t)snprintf(tok, sizeof(tok), ":f:%lu:", len);
		for (size_t i = 0; i < sizeof(d); i++)
			o += (size_t)snprintf(tok + o, sizeof(tok) - o, "%02x",
					      d[i]);
		put_name(path, tok);
		return;
	}
	put_name(path, ":?");
}

static void y_tree(const char *dir, bool *any)
{
	char name[S11_PATH_MAX], path[S11_PATH_MAX];
	bool is_dir;

	for (size_t i = 0; wire_yaffs_list(&Y, dir, i, name, sizeof(name),
					   &is_dir, NULL); i++) {
		if ((size_t)snprintf(path, sizeof(path), "%s%s%s", dir, *dir ? "/" : "", name) >=
		    sizeof(path)) {
			put_name(dir, ":long");
			continue;
		}
		if (is_dir) {
			*any = true;
			put_name(path, ":d");
			y_tree(path, any);
		} else {
			y_entry(path, any);
		}
	}
}

int main(void)
{
	struct h_cmd c;
	char p1[ARG_MAX], p2[ARG_MAX];

	wb_yaffs_set_clock(now);
	while (h_next(&c)) {
		const char *k = c.tok[0];

		if (strcmp(k, "disk") == 0 && c.n == 3) {
			size_t len = (size_t)h_num(&c, 1) * WB_SECTOR_SIZE;
			uint8_t *m = malloc(len);

			if (m != NULL)
				memset(m, (int)h_num(&c, 2), len);
			put_ok(disk_set(m, len));
		} else if (strcmp(k, "diskload") == 0 && c.n == 2) {
			size_t len = 0;
			uint8_t *m = load_file(c.tok[1], &len);

			put_ok(m != NULL && len % WB_SECTOR_SIZE == 0 &&
			   disk_set(m, len));
		} else if (strcmp(k, "disksave") == 0 && c.n == 2) {
			put_ok(dmem != NULL && save_file(c.tok[1], dmem, dmem_len));
		} else if (strcmp(k, "disksha") == 0 && c.n == 1) {
			disk_sha();
			h_end();
		} else if (strcmp(k, "diskwrites") == 0 && c.n == 1) {
			h_putu(dwrites);
			dwrites = 0;
			h_end();
		} else if (strcmp(k, "diskformat") == 0 && c.n == 1) {
			bit(dmem != NULL && wire_fat_format(&F, &dbd));
		} else if (strcmp(k, "diskmount") == 0 && c.n == 1) {
			bit(dmem != NULL && wire_fat_mount(&F, &dbd));
		} else if (strcmp(k, "diskls") == 0 && c.n == 1) {
			disk_ls();
			h_end();
		} else if (strcmp(k, "diskpick") == 0 && c.n == 1) {
			if (F.mounted && s11_disk_pick(wire_fat_root, &F, p1))
				put_name(p1, "");
			else
				h_put("-");
			h_end();
		} else if (strcmp(k, "diskcdc") == 0 && c.n == 1) {
			bit(disk_cdc());
		} else if (strcmp(k, "diskmkdir") == 0 && c.n == 2) {
			bit(F.mounted &&
			    wire_fat_mkdir(&F, path_arg(&c, 1, p1, sizeof(p1))));
		} else if (strcmp(k, "diskwrite") == 0 && c.n == 5) {
			size_t n = h_hex(&c, 2, line_buf, sizeof(line_buf));

			path_arg(&c, 1, p1, sizeof(p1));
			bit(F.mounted &&
			    wire_fat_write_file(&F, p1, line_buf, n,
						h_num(&c, 3) != 0, h_num(&c, 4)));
		} else if (strcmp(k, "diskread") == 0 && c.n == 2) {
			disk_read(path_arg(&c, 1, p1, sizeof(p1)));
			h_end();
		} else if (strcmp(k, "nameok") == 0 && c.n == 2) {
			bit(wire_fat_name_ok(path_arg(&c, 1, p1, sizeof(p1))));
		} else if (strcmp(k, "stamp") == 0 && c.n == 2) {
			uint16_t date, time;

			if (wire_fat_stamp(strtoull(c.tok[1], NULL, 0), &date,
					   &time)) {
				h_putu(date);
				h_putu(time);
			} else {
				h_put("-");
			}
			h_end();
		} else if (strcmp(k, "export") == 0 && c.n == 1) {
			struct s14_export_ops ops;

			wire_fat_export_ops(&F, &Y, &ops);
			h_putu(s14_export_run(&ops));
			h_end();
		} else if (strcmp(k, "yvol") == 0 && c.n == 3) {
			put_ok(vol_set(NULL, 0, (uint32_t)h_num(&c, 1),
				   (uint32_t)h_num(&c, 2)));
		} else if (strcmp(k, "yload") == 0 && c.n == 4) {
			size_t len = 0;
			uint8_t *m = load_file(c.tok[1], &len);

			put_ok(m != NULL && vol_set(m, len, (uint32_t)h_num(&c, 2),
						(uint32_t)h_num(&c, 3)));
			free(m);
		} else if (strcmp(k, "ysave") == 0 && c.n == 2) {
			put_ok(ymem != NULL && save_file(c.tok[1], ymem, ymem_len));
		} else if (strcmp(k, "ybe") == 0 && c.n == 1) {
			bit(Y.added && wire_yaffs_big_endian(&ybd, Y.first_block,
							     Y.last_block, Y.buf));
		} else if (strcmp(k, "ymount") == 0 && c.n == 1) {
			enum wire_mount m = wire_yaffs_mount(&Y);

			h_put(m == WIRE_MOUNT_OK ? "ok" :
			      m == WIRE_MOUNT_REFUSED ? "refused" : "failed");
			h_end();
		} else if (strcmp(k, "yunmount") == 0 && c.n == 1) {
			wire_yaffs_unmount(&Y);
			put_ok(true);
		} else if (strcmp(k, "yformat") == 0 && c.n == 1) {
			bit(wire_yaffs_format(&Y));
		} else if (strcmp(k, "yfree") == 0 && c.n == 1) {
			uint64_t bytes;

			if (wire_yaffs_free_space(&Y, &bytes))
				h_putu((unsigned long)bytes);
			else
				h_put("-");
			h_end();
		} else if (strcmp(k, "ylog") == 0 && c.n == 1) {
			char tok[16];

			for (size_t i = 0; i < ylog_n; i++) {
				snprintf(tok, sizeof(tok), "%c%lu",
					 ylog[i] & LOG_ERASE ? 'e' : 'w',
					 (unsigned long)(ylog[i] & ~LOG_ERASE));
				h_put(tok);
			}
			if (ylog_n == 0)
				h_put("-");
			ylog_n = 0;
			h_end();
		} else if (strcmp(k, "clock") == 0 && c.n == 2) {
			clock_utc = (uint32_t)h_num(&c, 1);
			put_ok(true);
		} else if (strcmp(k, "yput") == 0 && c.n == 3) {
			size_t n = h_hex(&c, 2, line_buf, sizeof(line_buf));

			bit(y_put(path_arg(&c, 1, p1, sizeof(p1)), line_buf, n));
		} else if (strcmp(k, "ypat") == 0 && c.n == 4) {
			size_t n = h_num(&c, 2);
			uint8_t *p = pattern(n, (unsigned)h_num(&c, 3));

			bit(p != NULL &&
			    y_put(path_arg(&c, 1, p1, sizeof(p1)), p, n));
			free(p);
		} else if (strcmp(k, "ymkdir") == 0 && c.n == 2) {
			bit(wire_yaffs_mkdir(&Y, path_arg(&c, 1, p1, sizeof(p1))));
		} else if (strcmp(k, "yrm") == 0 && c.n == 2) {
			bit(wire_yaffs_remove(&Y, path_arg(&c, 1, p1, sizeof(p1))));
		} else if (strcmp(k, "ymv") == 0 && c.n == 3) {
			struct s11_fs_ops fs;

			wire_yaffs_fs_ops(&Y, &fs);
			path_arg(&c, 1, p1, sizeof(p1));
			path_arg(&c, 2, p2, sizeof(p2));
			bit(fs.move(fs.ctx, p1, p2));
		} else if (strcmp(k, "ytree") == 0 && c.n == 1) {
			bool any = false;

			if (Y.mounted)
				y_tree("", &any);
			if (!any)
				h_put("-");
			h_end();
		} else {
			h_error(&c, "unknown command");
		}
	}
	return h_status();
}
