/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The USB disk's FAT volume on FatFs (src/wire_fat.c), against the S-12
 * vectors in wire_vectors.h and fat.txt:
 * - S-12.016's stamps (stamps.json), S-12.012's names (names.json create:
 *   the entry forms FatFs gives, and every refused name creates nothing),
 *   S-12.011's displayed names through the S-11.046 root listing;
 * - the populated RAM disk (ramdisk_populated.json) rebuilt through the
 *   adapter: the system-information files written with the RTC stamp and
 *   the log exported from a yaffs2 volume by s14_export_run(); its tree
 *   and each entry's attributes, case bits and stamps (S-12.015, S-12.016);
 * - the export's refusals (S-12.012, S-14.052): a name FAT cannot hold and
 *   a copy that fills the disk leave nothing behind;
 * - every volume of fat.txt read through the root listing, the package
 *   pick and the file source without a sector changing (S-12.020-S-12.022);
 * - a fragmented package read and written in place in pieces, as the
 *   install's recovery does (S-11.018), with FatFs's fast seek and, past
 *   the cluster map's fragments, without it;
 * - a package whose cluster chain a host left in a loop, or longer than its
 *   size, opened without fast seek and read.
 * Disk images are read from WB_VECTORS/s12.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include <unistd.h>

#include "wb_fatfs.h"

#include "waffle/s11_content.h"
#include "waffle/wire_fat.h"

#include "vectors/wire_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define DISK_SECTORS 454656u		/* S-12.001 */
#define YAFFS_BLOCKS 40u

static uint8_t *disk;
static struct wb_blockdev bd;
static struct wb_ramdisk ram;
static struct wire_fat F;
static uint8_t file_buf[1 << 16];

static uint8_t *yvol;
static struct wb_blockdev ybd;
static struct wb_ramdisk yram;
static struct wire_yaffs Y;

static uint32_t le16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
	return le16(p) | le16(p + 2) << 16;
}

/* ---- the RAM disk's layout, read raw (FAT16 behind an MBR) ---- */

static struct {
	uint32_t fat, root, root_entries, data, spc;
} G;

static void geometry(void)
{
	const uint8_t *b = disk + (size_t)le32(disk + 0x1C6) * 512u;

	G.fat = le32(disk + 0x1C6) + le16(b + 14);
	G.root = G.fat + b[16] * le16(b + 22);
	G.root_entries = le16(b + 17);
	G.data = G.root + G.root_entries * 32u / 512u;
	G.spc = b[13];
}

/* Entry i of the directory at `cluster` (0: the root), or NULL past it. */
static uint8_t *dir_entry(uint32_t cluster, size_t i)
{
	size_t per = (size_t)G.spc * 16u;

	if (cluster == 0)
		return i < G.root_entries ?
		       disk + (size_t)G.root * 512u + i * 32u : NULL;
	for (; i >= per; i -= per) {
		cluster = le16(disk + (size_t)G.fat * 512u + cluster * 2u);
		if (cluster < 2 || cluster >= 0xFFF8u)
			return NULL;
	}
	return disk + ((size_t)G.data + (size_t)(cluster - 2) * G.spc) * 512u +
	       i * 32u;
}

/* The UTF-8 form of a BMP code unit. */
static size_t put_utf8(char *out, uint32_t u)
{
	if (u < 0x80u) {
		out[0] = (char)u;
		return 1;
	}
	if (u < 0x800u) {
		out[0] = (char)(0xC0u | u >> 6);
		out[1] = (char)(0x80u | (u & 0x3Fu));
		return 2;
	}
	out[0] = (char)(0xE0u | u >> 12);
	out[1] = (char)(0x80u | (u >> 6 & 0x3Fu));
	out[2] = (char)(0x80u | (u & 0x3Fu));
	return 3;
}

/* The short entry of `path`; *lfn: it has long-name entries. Names are
 * the long name, else the short one with the NT case bits (S-12.011). */
static const uint8_t *find_entry(const char *path, bool *lfn)
{
	static const uint8_t offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22,
					  24, 28, 30 };
	uint32_t cluster = 0;
	const char *p = path;

	for (;;) {
		size_t n = strcspn(p, "/");
		uint16_t units[260];
		size_t nunits = 0;
		const uint8_t *e = NULL;

		*lfn = false;
		for (size_t i = 0; (e = dir_entry(cluster, i)) != NULL; i++) {
			char name[800];
			size_t len = 0;

			if (e[0] == 0)
				return NULL;
			if (e[0] == 0xE5) {
				nunits = 0;
				continue;
			}
			if (e[11] == 0x0F) {
				size_t at = ((size_t)(e[0] & 0x1Fu) - 1u) * 13u;

				if (e[0] & 0x40u)
					nunits = 0;
				for (size_t k = 0; k < 13 && at + k < 260; k++) {
					units[at + k] = (uint16_t)le16(e + offs[k]);
					if (units[at + k] == 0 && nunits <= at + k)
						break;
					if (nunits <= at + k)
						nunits = at + k + 1;
				}
				continue;
			}
			if (nunits != 0) {
				for (size_t k = 0; k < nunits && units[k] != 0; k++)
					len += put_utf8(name + len, units[k]);
			} else {
				for (size_t k = 0; k < 8 && e[k] != ' '; k++)
					name[len++] = (char)((e[12] & 0x08u) &&
						e[k] >= 'A' && e[k] <= 'Z' ? e[k] + 32 : e[k]);
				if (e[8] != ' ')
					name[len++] = '.';
				for (size_t k = 8; k < 11 && e[k] != ' '; k++)
					name[len++] = (char)((e[12] & 0x10u) &&
						e[k] >= 'A' && e[k] <= 'Z' ? e[k] + 32 : e[k]);
			}
			*lfn = nunits != 0;
			nunits = 0;
			if (len == n && !memcmp(name, p, n))
				break;
		}
		if (e == NULL)
			return NULL;
		if (p[n] == '\0')
			return e;
		cluster = le16(e + 26);
		p += n + 1;
	}
}

static void format(void)
{
	CHECK(wire_fat_format(&F, &bd), "format");
	geometry();
}

/* ---- S-12.016 ---- */

static void test_stamps(void)
{
	for (size_t i = 0; i < N(wirev_stamps); i++) {
		const struct wirev_stamp *s = &wirev_stamps[i];
		uint16_t d, t;
		bool known = wire_fat_stamp(s->utc, &d, &t);

		CHECK(known == (s->known != 0) &&
		      (!known || (d == s->date && t == s->time)),
		      "stamp %llu: %04x %04x, want %04x %04x",
		      (unsigned long long)s->utc, d, t, s->date, s->time);
	}
}

/* ---- S-12.011, S-12.012 ---- */

static void test_create(void)
{
	for (size_t i = 0; i < N(wirev_create); i++) {
		const struct wirev_create *c = &wirev_create[i];
		char name[S11_PATH_MAX];
		const uint8_t *e;
		bool dir, lfn, made;
		unsigned form;

		format();
		CHECK(wire_fat_name_ok(c->name) == (c->forms != 0), "%s: name rule",
		      c->name);
		made = wire_fat_write_file(&F, c->name, (const uint8_t *)"x", 1,
					   false, 0);
		CHECK(made == (c->forms != 0), "%s: created", c->name);
		if (!made) {
			/* not under that name or any other */
			CHECK(!wire_fat_root(&F, 0, name, sizeof(name), &dir) &&
			      !wire_fat_mkdir(&F, c->name), "%s: nothing created",
			      c->name);
			continue;
		}
		CHECK(wire_fat_root(&F, 0, name, sizeof(name), &dir) &&
		      !strcmp(name, c->name) && !dir &&
		      !wire_fat_root(&F, 1, name, sizeof(name), &dir),
		      "%s: listed as given", c->name);
		e = find_entry(c->name, &lfn);
		CHECK(e != NULL, "%s: entry", c->name);
		if (e == NULL)
			continue;
		form = lfn ? 4u : (e[12] & 0x18u) ? 2u : 1u;
		CHECK((c->forms & form) != 0, "%s: form %u, allowed %u", c->name,
		      form, c->forms);
	}
	format();
	CHECK(!wire_fat_mkdir(&F, "a/b.") && !wire_fat_mkdir(&F, ""),
	      "a path with a refused name is not created");
}

static void test_display(void)
{
	size_t len = 0;
	uint8_t *raw = tl_unhex(wirev_display_entries, &len);
	char got[512] = "", name[S11_PATH_MAX];
	bool dir;

	format();
	wire_fat_unmount(&F);
	memcpy(disk + (size_t)G.root * 512u, raw, len);
	free(raw);
	CHECK(wire_fat_mount(&F, &bd), "mount");
	for (size_t i = 0; wire_fat_root(&F, i, name, sizeof(name), &dir); i++)
		snprintf(got + strlen(got), sizeof(got) - strlen(got), "%s%s",
			 i != 0 ? "\n" : "", name);
	CHECK(!strcmp(got, wirev_display_names), "displayed names:\n%s\nwant\n%s",
	      got, wirev_display_names);
	/* S-11.046: the first file holding "tfw", case-sensitive */
	CHECK(s11_disk_pick(wire_fat_root, &F, name) && !strcmp(name, "OPXY.tfw"),
	      "package picked: %s", name);
}

/* ---- the populated RAM disk ---- */

static bool fat_list(const char *path, size_t index, FILINFO *fno)
{
	DIR d;
	bool found = false;

	if (f_opendir(&d, path) != FR_OK)
		return false;
	for (size_t i = 0; i <= index; i++) {
		found = f_readdir(&d, fno) == FR_OK && fno->fname[0] != '\0';
		if (!found)
			break;
	}
	(void)f_closedir(&d);
	return found;
}

static size_t file_sha(const char *path, char sha[65])
{
	struct wire_source src;
	uint32_t size = 0;
	bool ok;

	if (!wire_fat_open_file(&F, path, &src, &size))
		return (size_t)-1;
	ok = size <= sizeof(file_buf) && src.read(src.ctx, 0, file_buf, size);
	wire_fat_close_file(&F);
	tl_sha256_hex(sha, file_buf, size);
	return ok ? size : (size_t)-1;
}

/* Every path under `dir` is in the vector's tree; returns how many. */
static size_t check_tree(const char *dir)
{
	FILINFO fno;
	char path[600], sha[65];
	size_t found = 0;

	for (size_t i = 0; fat_list(dir, i, &fno); i++) {
		const struct wirev_tree *t = NULL;
		bool is_dir = (fno.fattrib & AM_DIR) != 0;

		snprintf(path, sizeof(path), "%s%s%s", dir, *dir ? "/" : "",
			 fno.fname);
		for (size_t k = 0; k < N(wirev_pop_tree); k++)
			if (!strcmp(wirev_pop_tree[k].path, path))
				t = &wirev_pop_tree[k];
		found++;
		CHECK(t != NULL && t->is_dir == is_dir, "populated: %s", path);
		if (t == NULL)
			continue;
		if (is_dir)
			found += check_tree(path);
		else
			CHECK(file_sha(path, sha) == t->len && !strcmp(sha, t->sha256),
			      "populated: %s contents", path);
	}
	return found;
}

static int yaffs_put(const char *rel, const uint8_t *data, size_t len,
		     uint32_t mtime)
{
	char path[WIRE_YAFFS_PATH_MAX];
	struct yaffs_utimbuf t = { mtime, mtime };
	int fd;

	if (!wire_yaffs_full_path(&Y, rel, path))
		return -1;
	fd = yaffs_open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0)
		return -1;
	while (len != 0) {
		unsigned int n = len > 0x10000u ? 0x10000u : (unsigned int)len;

		if (yaffs_write(fd, data, n) != (int)n)
			return -1;
		data += n;
		len -= n;
	}
	(void)yaffs_close(fd);
	return yaffs_utime(path, &t);
}

static uint8_t *pattern(size_t n, unsigned seed)
{
	uint8_t *p = malloc(n != 0 ? n : 1);

	for (size_t i = 0; p != NULL && i < n; i++)
		p[i] = (uint8_t)((i * 7u + seed) % 251u);
	return p;
}

static void yaffs_fresh(void)
{
	CHECK(wire_yaffs_format(&Y) && wire_yaffs_mkdir(&Y, "logs"),
	      "yaffs2 volume");
}

static void test_populated(void)
{
	struct s14_export_ops ops;
	uint8_t *log = pattern(WIREV_POP_LOG_LEN, 1);
	size_t found;

	format();
	yaffs_fresh();
	CHECK(log != NULL && yaffs_put("logs/" WIREV_POP_LOG, log,
				       WIREV_POP_LOG_LEN,
				       WIREV_POP_LOG_MTIME) == 0, "yaffs2 log");
	free(log);
	CHECK(wire_fat_mkdir(&F, "systeminfo"), "systeminfo/");
	for (size_t i = 0; i < N(wirev_pop_files); i++) {
		const char *text = wirev_pop_files[i][1];

		CHECK(wire_fat_write_file(&F, wirev_pop_files[i][0],
					  (const uint8_t *)text, strlen(text),
					  true, WIREV_POP_RTC),
		      "write %s", wirev_pop_files[i][0]);
	}
	wire_fat_export_ops(&F, &Y, &ops);
	CHECK(s14_export_run(&ops) == 1, "the log exported");
	found = check_tree("");
	CHECK(found == N(wirev_pop_tree), "populated: %zu paths, want %zu", found,
	      N(wirev_pop_tree));
	wire_fat_unmount(&F);

	for (size_t i = 0; i < N(wirev_pop_entries); i++) {
		const struct wirev_entry *w = &wirev_pop_entries[i];
		bool lfn;
		const uint8_t *e = find_entry(w->path, &lfn);
		uint32_t crt_time, crt_date;

		CHECK(e != NULL, "%s: entry", w->path);
		if (e == NULL)
			continue;
		crt_time = le16(e + 14);
		crt_date = le16(e + 16);
		CHECK(e[11] == w->attr && le32(e + 28) == w->size, "%s: attr, size",
		      w->path);
		CHECK(e[13] == 0 && le16(e + 18) == 0,
		      "%s: creation tenths and access date 0", w->path);
		CHECK(le16(e + 22) == w->wrt_time && le16(e + 24) == w->wrt_date,
		      "%s: write stamp %04x %04x, want %04x %04x", w->path,
		      le16(e + 24), le16(e + 22), w->wrt_date, w->wrt_time);
		if (w->attr & 0x10u)	/* 0 or the fixed clock (S-12.015) */
			CHECK(crt_time == 0 && (crt_date == 0 || crt_date == 0x21u),
			      "%s: directory creation stamp", w->path);
		else
			CHECK(crt_time == w->crt_time && crt_date == w->crt_date,
			      "%s: creation stamp", w->path);
		if (!lfn)	/* the short name shows the name (S-12.012) */
			CHECK((e[12] & 0x18u) == w->nt, "%s: case bits", w->path);
	}
	CHECK(wire_fat_mount(&F, &bd), "mount");
}

/* ---- S-12.012, S-14.052 through the export ---- */

static void test_export_refused(void)
{
	struct s14_export_ops ops;
	FILINFO fno;
	char sha[65], want[65];

	format();
	yaffs_fresh();
	CHECK(wire_fat_mkdir(&F, "logs") &&
	      wire_fat_write_file(&F, "logs/bad", (const uint8_t *)"keep", 4,
				  false, 0), "logs/bad on the disk");
	CHECK(yaffs_put("logs/bad.", (const uint8_t *)"lost", 4, 2000) == 0 &&
	      yaffs_put("logs/ok.log", (const uint8_t *)"ok", 2, 1000) == 0 &&
	      wire_yaffs_mkdir(&Y, "logs/sub") &&
	      yaffs_put("logs/sub/x.log", (const uint8_t *)"x", 1, 3000) == 0,
	      "yaffs2 logs");
	wire_fat_export_ops(&F, &Y, &ops);
	CHECK(s14_export_run(&ops) == 1, "only ok.log exported");
	tl_sha256_hex(want, "keep", 4);
	CHECK(file_sha("logs/bad", sha) == 4 && !strcmp(sha, want),
	      "logs/bad kept: the refused bad. maps to nothing");
	CHECK(fat_list("logs", 0, &fno) && fat_list("logs", 1, &fno) &&
	      !fat_list("logs", 2, &fno), "logs/ holds bad and ok.log");
	CHECK(file_sha("logs/ok.log", sha) == 2, "ok.log");
}

static void test_export_full(void)
{
	static uint8_t small[4096 * 512];
	struct wb_blockdev sbd;
	struct wb_ramdisk sram;
	struct s14_export_ops ops;
	MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 0, 0, 0 };
	FATFS *fs = NULL;
	DWORD before = 0, after = 0;
	FILINFO fno;
	uint8_t *big = pattern(3u << 20, 3);

	memset(small, 0, sizeof(small));
	wire_fat_unmount(&F);
	wb_ramdisk_init(&sbd, &sram, small, (uint32_t)(sizeof(small) / 512u));
	wb_fatfs_attach(&sbd);
	CHECK(f_mkfs("", &opt, F.buf, (UINT)sizeof(F.buf)) == FR_OK, "mkfs");
	CHECK(wire_fat_mount(&F, &sbd) && f_getfree("", &before, &fs) == FR_OK,
	      "small volume");
	yaffs_fresh();
	CHECK(big != NULL && yaffs_put("logs/big.log", big, 3u << 20, 2000) == 0 &&
	      yaffs_put("logs/small.log", big, 100, 1000) == 0, "yaffs2 logs");
	free(big);
	wire_fat_export_ops(&F, &Y, &ops);
	CHECK(s14_export_run(&ops) == 1, "the copy that fills the disk fails");
	CHECK(fat_list("logs", 0, &fno) && !strcmp(fno.fname, "small.log") &&
	      !fat_list("logs", 1, &fno), "logs/ holds small.log only");
	/* every cluster of the failed copy is free again: logs/ and small.log
	 * hold one cluster each */
	CHECK(f_getfree("", &after, &fs) == FR_OK && before - after == 2u,
	      "free clusters %lu -> %lu", (unsigned long)before,
	      (unsigned long)after);
	wire_fat_unmount(&F);
	CHECK(wire_fat_mount(&F, &bd), "mount");
}

/* ---- S-12.020-S-12.022: reads leave every sector as it was ---- */

/* S-11.018: a package file opened for the install is written in place,
 * never past its end, a read-only attribute notwithstanding; a file opened
 * for reading cannot be written. */
static void test_open_package(void)
{
	static const uint8_t data[] = "0123456789abcdef";
	struct wire_source src;
	uint8_t back[sizeof(data)];
	uint32_t size = 0;

	format();
	CHECK(wire_fat_write_file(&F, "P.TFW", data, sizeof(data), false, 0) &&
	      f_chmod("P.TFW", AM_RDO, AM_RDO) == FR_OK, "package: created");
	CHECK(wire_fat_open_file(&F, "P.TFW", &src, &size) &&
	      src.write == NULL, "package: read-only open has no write");
	wire_fat_close_file(&F);
	CHECK(wire_fat_open_package(&F, "P.TFW", &src, &size) &&
	      size == sizeof(data) && src.write != NULL, "package: opened");
	if (src.write == NULL)
		return;
	CHECK(src.write(src.ctx, 4, (const uint8_t *)"WXYZ", 4) &&
	      !src.write(src.ctx, (uint32_t)sizeof(data) - 2u,
			 (const uint8_t *)"..", 3) &&
	      src.read(src.ctx, 0, back, sizeof(back)) &&
	      !memcmp(back, "0123WXYZ89abcdef", 16), "package: in place");
	wire_fat_close_file(&F);
	CHECK(file_sha("P.TFW", (char[65]){ 0 }) == sizeof(data),
	      "package: never grown");
}

/* A package in about `holes` + 1 fragments: one-cluster files, every other
 * one removed, and the package written over the holes after a remount
 * (FatFs then allocates from the volume's start). */
static void fragmented(unsigned holes, bool fast)
{
	const uint32_t cl = 8192u, size = (holes + 2u) * cl + 1000u;
	uint8_t *want = malloc(size), *got = malloc(size);
	struct wire_source src;
	uint32_t n = 0, off;
	char name[16];

	if (want == NULL || got == NULL) {
		CHECK(0, "memory");
		goto out;
	}
	format();
	memset(file_buf, 0xEE, cl);
	for (unsigned i = 0; i < 2u * holes; i++) {
		snprintf(name, sizeof(name), "F%u", i);
		CHECK(wire_fat_write_file(&F, name, file_buf, cl, false, 0),
		      "fragments: %s", name);
	}
	for (unsigned i = 0; i < 2u * holes; i += 2u) {
		snprintf(name, sizeof(name), "F%u", i);
		CHECK(f_unlink(name) == FR_OK, "fragments: %s removed", name);
	}
	wire_fat_unmount(&F);
	CHECK(wire_fat_mount(&F, &bd), "fragments: remount");
	for (off = 0; off < size; off++)
		want[off] = (uint8_t)(off * 7u + off / 251u);
	CHECK(wire_fat_write_file(&F, "P.TFW", want, size, false, 0) &&
	      wire_fat_open_package(&F, "P.TFW", &src, &n) && n == size,
	      "fragments %u: package", holes);
	CHECK((F.fil.cltbl != NULL) == fast, "fragments %u: fast seek %s",
	      holes, fast ? "on" : "off");
	/* each piece read, changed and written back, from offset 100 on */
	for (off = 100u; off < size; off += 3000u) {
		const uint32_t k = size - off < 3000u ? size - off : 3000u;

		CHECK(src.read(src.ctx, off, got, k), "fragments: read %u", off);
		for (uint32_t i = 0; i < k; i++)
			got[i] ^= 0x5Au;
		CHECK(src.write(src.ctx, off, got, k), "fragments: write %u", off);
		for (uint32_t i = 0; i < k; i++)
			want[off + i] ^= 0x5Au;
	}
	wire_fat_close_file(&F);
	CHECK(wire_fat_open_file(&F, "P.TFW", &src, &n) && n == size &&
	      src.read(src.ctx, 0, got, size) && !memcmp(got, want, size),
	      "fragments %u: the bytes in place", holes);
	wire_fat_close_file(&F);
out:
	free(want);
	free(got);
}

static void test_fragmented(void)
{
	fragmented(20u, true);
	fragmented(200u, false);	/* over WIRE_FAT_CLMT's 127 */
}

static void test_read_only(void)
{
	FILE *f = tl_open_vectors("fat.txt");
	char *line = NULL, *fld[4];
	size_t cap = 0, len = (size_t)DISK_SECTORS * 512u;
	uint8_t *orig = malloc(len);
	int n;

	CHECK(orig != NULL, "memory");
	wire_fat_unmount(&F);
	while (orig != NULL && (n = tl_fields(f, &line, &cap, fld, 4)) >= 0) {
		char path[300];
		FILE *img;
		bool mounted;

		if (n != 4 || strcmp(fld[0], "volume"))
			continue;
		snprintf(path, sizeof(path), "s12/%s", fld[2]);
		img = fopen(tl_vector_path(path), "rb");
		CHECK(img != NULL && fread(disk, 1, len, img) == len, "%s", path);
		if (img != NULL)
			fclose(img);
		memcpy(orig, disk, len);
		mounted = wire_fat_mount(&F, &bd);
		CHECK(mounted == (atoi(fld[3]) != 0), "%s: read %d", fld[1],
		      (int)mounted);
		if (mounted) {
			char name[S11_PATH_MAX];
			struct wire_source src;
			uint32_t size;
			bool dir, picked;

			for (size_t i = 0; wire_fat_root(&F, i, name, sizeof(name),
							 &dir); i++)
				;
			picked = s11_disk_pick(wire_fat_root, &F, name);
			if (picked && wire_fat_open_file(&F, name, &src, &size)) {
				for (uint32_t off = 0; off < size; off += 4096u)
					CHECK(src.read(src.ctx, off, file_buf,
						       size - off < 4096u ?
						       size - off : 4096u),
					      "%s: %s read", fld[1], name);
				wire_fat_close_file(&F);
			}
			wire_fat_unmount(&F);
		}
		CHECK(!memcmp(orig, disk, len), "%s: no sector written", fld[1]);
	}
	free(line);
	free(orig);
	fclose(f);
	memset(disk, 0, len);
}

/* FAT16 entry `cl` of the RAM disk, written raw. */
static void fat_set(uint32_t cl, uint16_t v)
{
	uint8_t *e = disk + (size_t)G.fat * 512u + cl * 2u;

	e[0] = (uint8_t)v;
	e[1] = (uint8_t)(v >> 8);
}

/* A three-cluster package whose chain the host bent: the open must
 * return, with no fast seek, and the file's bytes still read. */
static void bent_chain(bool loop)
{
	const uint32_t size = 3u * 8192u - 100u;
	uint8_t *want = malloc(size), *got = malloc(size);
	struct wire_source src;
	const uint8_t *e;
	uint32_t n = 0, c;
	bool lfn;

	if (want == NULL || got == NULL) {
		CHECK(0, "memory");
		goto out;
	}
	format();
	for (uint32_t i = 0; i < size; i++)
		want[i] = (uint8_t)(i * 13u + 5u);
	CHECK(wire_fat_write_file(&F, "P.TFW", want, size, false, 0),
	      "bent chain: written");
	wire_fat_unmount(&F);
	e = find_entry("P.TFW", &lfn);
	CHECK(e != NULL, "bent chain: entry");
	if (e == NULL)
		goto out;
	c = le16(e + 26);
	CHECK(le16(disk + (size_t)G.fat * 512u + (c + 2u) * 2u) >= 0xFFF8u,
	      "bent chain: three clusters in a row");
	if (loop) {
		fat_set(c + 2u, (uint16_t)c);		/* back to the start */
	} else {
		fat_set(c + 2u, (uint16_t)(c + 3u));	/* one cluster more */
		fat_set(c + 3u, 0xFFFFu);
	}
	CHECK(wire_fat_mount(&F, &bd), "bent chain: mount");
	alarm(30);		/* a chain followed for ever fails the test */
	CHECK(wire_fat_open_file(&F, "P.TFW", &src, &n) && n == size,
	      "bent chain %s: opened", loop ? "loop" : "long");
	CHECK(F.fil.cltbl == NULL, "bent chain %s: no fast seek",
	      loop ? "loop" : "long");
	CHECK(src.read(src.ctx, 8000u, got, size - 8000u) &&
	      src.read(src.ctx, 0, got, size) && !memcmp(got, want, size),
	      "bent chain %s: read", loop ? "loop" : "long");
	alarm(0);
	wire_fat_close_file(&F);
out:
	free(want);
	free(got);
}

static void test_bent_chain(void)
{
	bent_chain(true);
	bent_chain(false);
}

int main(void)
{
	size_t ylen = (size_t)YAFFS_BLOCKS * 128u * 4096u;

	disk = calloc(DISK_SECTORS, 512u);
	yvol = calloc(1, ylen);
	if (disk == NULL || yvol == NULL)
		return 2;
	wb_ramdisk_init(&bd, &ram, disk, DISK_SECTORS);
	wb_ramdisk_init(&ybd, &yram, yvol, (uint32_t)(ylen / 512u));
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &ybd, 0, YAFFS_BLOCKS - 1),
	      "yaffs2 device");
	test_stamps();
	test_create();
	test_display();
	test_populated();
	test_export_refused();
	test_export_full();
	test_open_package();
	test_fragmented();
	test_bent_chain();
	test_read_only();
	wire_fat_unmount(&F);
	wire_yaffs_detach(&Y);
	free(yvol);
	free(disk);
	return tl_done("test_wire_fat");
}
