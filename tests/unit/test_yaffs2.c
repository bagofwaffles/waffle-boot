/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * yaffs2 configured as S-12 section 2 states (in-band tags, little-endian,
 * zero-erased media, on a block device), against yaffs.txt:
 * - every volume image of yaffs_trees.json mounts to its tree, compared
 *   exactly outside lost+found and as a multiset of file contents beneath
 *   it (S-12.050), and a session that changes nothing writes nothing
 *   (S-12.054), no write or erase reaching the block device;
 * - a rename over a file, made by yaffs2 or as the shadows image holds it
 *   (a shadowing header and no deletion record), and the remounts after
 *   it: a mount writes and erases nothing, the deletion record is written
 *   by the first change, and the shadowed file never comes back;
 * - files written on an empty volume read back after a remount, and the
 *   volume then holds no checkpoint and only data-range sequence numbers
 *   (S-12.047, S-12.055);
 * - a reader that keeps checkpoints sees what our session wrote, and a
 *   checkpoint block that will not erase stops the session's writes
 *   (S-12.055);
 * - on a device with no erase operation a block is erased by zero writes
 *   of 128 sectors, leaving the same bytes as a sector at a time;
 * - on a device with a reliable write every chunk goes through it, and
 *   the zero runs through the plain write (S-12.060).
 *
 * The volumes S-12.052 refuses (big-endian) are skipped: that refusal is
 * the bootloader's check before it mounts, not yaffs2's.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "waffle/drivers/bf70x_msi.h"
#include "wb_yaffs.h"

#define MAX_ENTRIES 64
#define MOUNT "/y"

struct entry {
	char path[600];
	char type[16];
	unsigned long len;
	char sha[65];      /* files */
	char target[600];  /* symbolic links: the alias; hard links: the path */
	int ino;
};

static uint8_t *image;
static size_t image_len;
static struct wb_blockdev bdev;
static struct wb_ramdisk ram;
static struct yaffs_dev vol_dev;

/* Sector writes and erases that reached the vector volume's device. */
static unsigned long n_writes, n_erases;
static int (*ram_write)(void *, uint32_t, const void *, uint32_t);
static int (*ram_erase)(void *, uint32_t, uint32_t);

static int count_write(void *ctx, uint32_t lba, const void *buf,
		       uint32_t count)
{
	n_writes += count;
	return ram_write(ctx, lba, buf, count);
}

static int count_erase(void *ctx, uint32_t lba, uint32_t count)
{
	n_erases += count;
	return ram_erase(ctx, lba, count);
}

static void hash_file(const char *path, struct entry *e)
{
	br_sha256_context ctx;
	uint8_t buf[4096], d[32];
	int fd = yaffs_open(path, O_RDONLY, 0), n;

	CHECK(fd >= 0, "open %s", path);
	br_sha256_init(&ctx);
	e->len = 0;
	while (fd >= 0 && (n = yaffs_read(fd, buf, sizeof(buf))) > 0) {
		br_sha256_update(&ctx, buf, (size_t)n);
		e->len += (unsigned int)n;
	}
	if (fd >= 0)
		yaffs_close(fd);
	br_sha256_out(&ctx, d);
	tl_hex(e->sha, d, 32);
}

/* Lists the directory rel (relative to the mount, "" for the root). */
static void walk(const char *rel, struct entry *e, int *n)
{
	char dir[700];
	yaffs_DIR *d;
	const struct yaffs_dirent *de;

	snprintf(dir, sizeof(dir), "%s%s%s", MOUNT, *rel ? "/" : "", rel);
	d = yaffs_opendir(dir);
	CHECK(d != NULL, "opendir %s", dir);
	while (d && (de = yaffs_readdir(d)) != NULL) {
		struct yaffs_stat st;
		char full[1400];
		struct entry *x;

		if (*n >= MAX_ENTRIES) {
			CHECK(0, "too many entries");
			break;
		}
		x = &e[(*n)++];
		memset(x, 0, sizeof(*x));
		snprintf(x->path, sizeof(x->path), "%s%s%s", rel,
			 *rel ? "/" : "", de->d_name);
		snprintf(full, sizeof(full), "%s/%s", MOUNT, x->path);
		CHECK(yaffs_lstat(full, &st) == 0, "lstat %s", full);
		x->ino = st.st_ino;
		switch (st.st_mode & S_IFMT) {
		case S_IFDIR:
			snprintf(x->type, sizeof(x->type), "dir");
			walk(x->path, e, n);
			break;
		case S_IFLNK:
			snprintf(x->type, sizeof(x->type), "symlink");
			CHECK(yaffs_readlink(full, x->target,
					     sizeof(x->target) - 1) == 0,
			      "readlink %s", full);
			break;
		case S_IFREG:
			snprintf(x->type, sizeof(x->type), "file");
			hash_file(full, x);
			break;
		default:
			snprintf(x->type, sizeof(x->type), "other");
			break;
		}
	}
	if (d)
		yaffs_closedir(d);
}

static const struct entry *find(const struct entry *e, int n, const char *path)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp(e[i].path, path))
			return &e[i];
	return NULL;
}

static int beneath_lost_found(const char *path)
{
	return !strncmp(path, "lost+found/", 11);
}

static void check_tree(const char *name, const struct entry *want, int n_want,
		       char lost[][65], int n_lost)
{
	struct entry got[MAX_ENTRIES];
	int n = 0, i, k, n_got_lost = 0;

	walk("", got, &n);
	for (i = 0; i < n_want; i++) {
		const struct entry *g = find(got, n, want[i].path);

		CHECK(g != NULL, "%s: %s missing", name, want[i].path);
		if (!g)
			continue;
		if (!strcmp(want[i].type, "hardlink")) {
			const struct entry *t = find(got, n, want[i].target);

			/* A hard link is the object it names (rule 7). */
			CHECK(t && t->ino == g->ino && !strcmp(g->type, "file"),
			      "%s: %s is not a link to %s", name, want[i].path,
			      want[i].target);
			continue;
		}
		CHECK(!strcmp(g->type, want[i].type), "%s: %s is a %s, want %s",
		      name, want[i].path, g->type, want[i].type);
		if (!strcmp(want[i].type, "file"))
			CHECK(g->len == want[i].len && !strcmp(g->sha, want[i].sha),
			      "%s: contents of %s (%lu bytes, want %lu)", name,
			      want[i].path, g->len, want[i].len);
		if (!strcmp(want[i].type, "symlink"))
			CHECK(!strcmp(g->target, want[i].target),
			      "%s: %s -> %s, want %s", name, want[i].path,
			      g->target, want[i].target);
	}
	/* Nothing else outside lost+found; its files as a multiset. */
	for (i = 0; i < n; i++) {
		if (beneath_lost_found(got[i].path)) {
			if (strcmp(got[i].type, "file"))
				continue;
			n_got_lost++;
			for (k = 0; k < n_lost; k++) {
				if (!strcmp(lost[k], got[i].sha)) {
					lost[k][0] = '\0';
					break;
				}
			}
			CHECK(k < n_lost, "%s: unexpected %s", name, got[i].path);
			continue;
		}
		CHECK(find(want, n_want, got[i].path) != NULL,
		      "%s: unexpected %s", name, got[i].path);
	}
	CHECK(n_got_lost == n_lost, "%s: %d files in lost+found, want %d",
	      name, n_got_lost, n_lost);
}

static void load_image(const char *file)
{
	char path[256];
	FILE *img;

	snprintf(path, sizeof(path), "s12/%s", file);
	img = fopen(tl_vector_path(path), "rb");
	if (!img || fread(image, 1, image_len, img) != image_len) {
		printf("cannot read %s\n", tl_vector_path(path));
		exit(2);
	}
	fclose(img);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

/* S-12.054: every chunk reads as it did before the session. */
static void check_unchanged(const char *name, const uint8_t *before)
{
	size_t c;

	for (c = 0; c < image_len; c += WB_YAFFS_CHUNK_BYTES)
		CHECK(!memcmp(before + c, image + c, WB_YAFFS_CHUNK_BYTES),
		      "%s: a session that changed nothing wrote chunk %zu "
		      "(sequence 0x%08lx, was 0x%08lx)", name,
		      c / WB_YAFFS_CHUNK_BYTES,
		      (unsigned long)le32(image + c + 0xFF0),
		      (unsigned long)le32(before + c + 0xFF0));
}

/* Mounts and unmounts the loaded image; S-12.054 for that session. */
static void mount_only(const char *name, uint8_t *before)
{
	memcpy(before, image, image_len);
	n_writes = n_erases = 0;
	CHECK(yaffs_mount(MOUNT) == 0, "%s: mount", name);
	CHECK(yaffs_unmount(MOUNT) == 0, "%s: unmount", name);
	CHECK(n_writes == 0 && n_erases == 0,
	      "%s: a mount wrote %lu and erased %lu sectors", name, n_writes,
	      n_erases);
	check_unchanged(name, before);
}

/* Mounts the loaded image, checks its tree, and that nothing was written. */
static void check_volume(const char *name, uint8_t *before,
			 const struct entry *want, int n_want, char lost[][65],
			 int n_lost)
{
	memcpy(before, image, image_len);
	n_writes = n_erases = 0;
	CHECK(yaffs_mount(MOUNT) == 0, "%s: mount (error %d)", name,
	      yaffsfs_GetLastError());
	check_tree(name, want, n_want, lost, n_lost);
	CHECK(yaffs_unmount(MOUNT) == 0, "%s: unmount", name);
	CHECK(n_writes == 0 && n_erases == 0,
	      "%s: a session that changed nothing wrote %lu and erased %lu "
	      "sectors", name, n_writes, n_erases);
	check_unchanged(name, before);
	/* And the next one (a mount clean-up left over would show here). */
	mount_only(name, before);
}

static void check_volumes(FILE *f)
{
	static struct entry want[MAX_ENTRIES];
	static char lost[8][65];
	char *line = NULL, *fld[6], name[64] = "";
	size_t cap = 0;
	uint8_t *before = tl_xmalloc(image_len);
	int n, n_want = 0, n_lost = 0, refused = 0, volumes = 0;

	while ((n = tl_fields(f, &line, &cap, fld, 6)) >= 0) {
		if (n == 4 && !strcmp(fld[0], "volume")) {
			snprintf(name, sizeof(name), "%s", fld[1]);
			refused = !strcmp(fld[3], "1");
			load_image(fld[2]);
			n_want = n_lost = 0;
		} else if (n >= 3 && !strcmp(fld[0], "entry") &&
			   n_want < MAX_ENTRIES) {
			struct entry *e = &want[n_want++];
			size_t len;
			uint8_t *p = tl_unhex(fld[1], &len);

			memset(e, 0, sizeof(*e));
			snprintf(e->path, sizeof(e->path), "%.*s", (int)len,
				 (const char *)p);
			free(p);
			snprintf(e->type, sizeof(e->type), "%s", fld[2]);
			if (n == 5) {
				e->len = strtoul(fld[3], NULL, 10);
				snprintf(e->sha, sizeof(e->sha), "%s", fld[4]);
			} else if (n == 4) {
				p = tl_unhex(fld[3], &len);
				snprintf(e->target, sizeof(e->target), "%.*s",
					 (int)len, (const char *)p);
				free(p);
			}
		} else if (n == 3 && !strcmp(fld[0], "lostfound") &&
			   n_lost < 8) {
			snprintf(lost[n_lost++], 65, "%s", fld[2]);
		} else if (n == 1 && !strcmp(fld[0], "end") && *name) {
			if (!refused) {
				check_volume(name, before, want, n_want, lost,
					     n_lost);
				volumes++;
			}
			*name = '\0';
		}
	}
	free(before);
	free(line);
	CHECK(volumes >= 14, "only %d volumes", volumes);
}

static uint32_t fixed_clock(void)
{
	return 1793190896u; /* 2026-10-28 12:34:56 UTC */
}

/* Writes, rewrites and removes files on an empty volume of 16 blocks. */
static void check_write(void)
{
	static const char *const files[] = {
		MOUNT "w/user/a.txt", MOUNT "w/user/empty", MOUNT "w/logs/boot.log",
		MOUNT "w/content/dlc/pack.bin", MOUNT "w/upgrade/progress.log",
	};
	static const size_t sizes[] = { 5, 0, 10000, 600000, 4080 };
	static struct wb_blockdev wbdev;
	static struct wb_ramdisk wram;
	static struct yaffs_dev wdev;
	const size_t n_blocks = 16, len = n_blocks * 128 * 4096;
	uint8_t *vol = calloc(1, len), *data = tl_xmalloc(600000);
	uint8_t *back = tl_xmalloc(600000);
	struct yaffs_stat st;
	size_t i, k, used = 0;
	int fd;

	for (i = 0; i < 600000; i++)
		data[i] = (uint8_t)(i * 13 + (i >> 11));
	wb_ramdisk_init(&wbdev, &wram, vol, (uint32_t)(len / 512));
	CHECK(wb_yaffs_add_blockdev(&wdev, MOUNT "w", &wbdev, 0,
				    (uint32_t)n_blocks - 1) == 0, "add");
	wb_yaffs_set_clock(fixed_clock);

	/* S-12.057: mounting an all-empty volume writes nothing. */
	CHECK(yaffs_mount(MOUNT "w") == 0, "mount empty");
	CHECK(yaffs_unmount(MOUNT "w") == 0, "unmount empty");
	for (i = 0; i < len && !vol[i]; i++)
		;
	CHECK(i == len, "mounting an empty volume wrote byte %zu", i);

	CHECK(yaffs_mount(MOUNT "w") == 0, "mount");
	CHECK(yaffs_mkdir(MOUNT "w/user", 0755) == 0, "mkdir user");
	CHECK(yaffs_mkdir(MOUNT "w/logs", 0755) == 0, "mkdir logs");
	CHECK(yaffs_mkdir(MOUNT "w/content", 0755) == 0, "mkdir content");
	CHECK(yaffs_mkdir(MOUNT "w/content/dlc", 0755) == 0, "mkdir dlc");
	CHECK(yaffs_mkdir(MOUNT "w/upgrade", 0755) == 0, "mkdir upgrade");
	for (k = 0; k < sizeof(files) / sizeof(files[0]); k++) {
		fd = yaffs_open(files[k], O_CREAT | O_RDWR | O_TRUNC, 0644);
		CHECK(fd >= 0, "create %s", files[k]);
		CHECK(yaffs_write(fd, data + k, (unsigned int)sizes[k]) ==
			      (int)sizes[k], "write %s", files[k]);
		yaffs_close(fd);
	}
	/* Rewrite one, shrink one, rename one, remove one. */
	fd = yaffs_open(files[2], O_RDWR | O_TRUNC, 0);
	CHECK(fd >= 0 && yaffs_write(fd, data + 7, 9000) == 9000, "rewrite");
	yaffs_close(fd);
	fd = yaffs_open(files[3], O_RDWR, 0);
	CHECK(fd >= 0 && yaffs_ftruncate(fd, 5000) == 0, "truncate");
	yaffs_close(fd);
	CHECK(yaffs_rename(files[0], MOUNT "w/user/b.txt") == 0, "rename");
	CHECK(yaffs_unlink(files[4]) == 0, "unlink");
	CHECK(yaffs_unmount(MOUNT "w") == 0, "unmount");

	CHECK(yaffs_mount(MOUNT "w") == 0, "remount");
	CHECK(yaffs_lstat(files[0], &st) != 0, "renamed file still there");
	CHECK(yaffs_lstat(files[4], &st) != 0, "removed file still there");
	CHECK(yaffs_lstat(MOUNT "w/lost+found", &st) == 0 &&
		      (st.st_mode & S_IFMT) == S_IFDIR, "lost+found");
	{
		static const char *const paths[] = {
			MOUNT "w/user/b.txt", MOUNT "w/user/empty",
			MOUNT "w/logs/boot.log", MOUNT "w/content/dlc/pack.bin",
		};
		static const size_t lens[] = { 5, 0, 9000, 5000 };
		static const size_t offs[] = { 0, 1, 7, 3 };

		for (k = 0; k < 4; k++) {
			int got;

			fd = yaffs_open(paths[k], O_RDONLY, 0);
			got = fd >= 0 ? yaffs_read(fd, back, 600000) : -1;
			CHECK(got == (int)lens[k] &&
				      !memcmp(back, data + offs[k], lens[k]),
			      "read back %s (%d bytes)", paths[k], got);
			if (fd >= 0)
				yaffs_close(fd);
		}
	}
	CHECK(yaffs_lstat(MOUNT "w/logs/boot.log", &st) == 0 &&
		      st.yst_mtime == fixed_clock(), "header time (S-12.059)");
	CHECK(yaffs_unmount(MOUNT "w") == 0, "unmount");

	/* Every used chunk carries a data-range sequence number (S-12.043,
	 * S-12.047): no checkpoint (S-12.055), no 0xFF erased marks. */
	for (i = 0; i < len; i += 4096) {
		uint32_t seq = le32(vol + i + 0xFF0);

		if (!seq)
			continue;
		used++;
		CHECK(seq >= 0x1000 && seq <= 0xEFFFFEFF,
		      "chunk %zu has sequence number 0x%08lx", i / 4096,
		      (unsigned long)seq);
	}
	CHECK(used > 150, "only %zu chunks written", used);
	CHECK(wb_yaffs_bug_count == 0, "yaffs2 internal error at %s:%d",
	      wb_yaffs_bug_file ? wb_yaffs_bug_file : "?", wb_yaffs_bug_line);
	free(vol);
	free(data);
	free(back);
}

/*
 * S-12.055: the first write of a session erases every checkpoint block
 * (seq_classes has one, block 3); no chunk written carries 0x21.
 */
static void check_checkpoint_erase(void)
{
	size_t c;
	int fd;

	load_image("yaffs_seq_classes.img");
	CHECK(le32(image + 3 * 128 * 4096 + 0xFF0) == 0x21,
	      "seq_classes has no checkpoint block");
	CHECK(yaffs_mount(MOUNT) == 0, "mount seq_classes");
	fd = yaffs_open(MOUNT "/new", O_CREAT | O_RDWR, 0644);
	CHECK(fd >= 0 && yaffs_write(fd, "x", 1) == 1, "write");
	if (fd >= 0)
		yaffs_close(fd);
	CHECK(yaffs_unmount(MOUNT) == 0, "unmount");
	for (c = 0; c < image_len; c += WB_YAFFS_CHUNK_BYTES)
		CHECK(le32(image + c + 0xFF0) != 0x21,
		      "chunk %zu still has the checkpoint sequence number",
		      c / WB_YAFFS_CHUNK_BYTES);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Writes an object header (S-12.044, S-12.045) as chunk n of the image. */
static void put_header(uint32_t n, uint32_t seq, uint32_t obj_id,
		       uint32_t type, uint32_t parent, const char *name)
{
	uint8_t *c = image + (size_t)n * WB_YAFFS_CHUNK_BYTES;

	memset(c, 0xFF, 0x200);
	put_le32(c + 0x000, type);
	put_le32(c + 0x004, parent);
	memset(c + 0x00A, 0, 256);
	memcpy(c + 0x00A, name, strlen(name));
	put_le32(c + 0x10C, type == 3 ? 040755u : 0100644u);
	put_le32(c + 0x124, 0);
	put_le32(c + 0x128, 0);
	memset(c + 0x12C, 0, 160);
	put_le32(c + 0x1E8, 0);
	put_le32(c + 0x1F0, 0);
	put_le32(c + 0x1F8, 0);
	put_le32(c + 0x1FC, 0);
	put_le32(c + 0xFF0, seq);
	put_le32(c + 0xFF4, type << 28 | obj_id);
	put_le32(c + 0xFF8, 0x80000000u | parent);
	put_le32(c + 0xFFC, 0);
}

/*
 * Volumes that upstream yaffs2 mishandled (the local fixes):
 * - a header that makes the deleted directory (object 4) a file: the
 *   fixed directories keep their type (S-12.046), and the mount survives;
 * - a hard link whose object does not exist: listing it does not crash;
 * - blocks on both sides of sequence number 0x80000000: the newer block's
 *   header is the current one (S-12.047, S-12.050 rules 1 and 3).
 */
static void check_regressions(void)
{
	struct yaffs_stat st;

	memset(image, 0, image_len);
	put_header(0, 0x1001, 4, 1, 1, "deleted");
	put_header(1, 0x1001, 0x100, 1, 1, "kept");
	CHECK(yaffs_mount(MOUNT) == 0, "mount with a retyped object 4");
	CHECK(yaffs_lstat(MOUNT "/kept", &st) == 0, "kept is there");
	CHECK(yaffs_unmount(MOUNT) == 0, "unmount");

	/* A hard link to an object that does not exist. */
	memset(image, 0, image_len);
	put_header(0, 0x1001, 0x100, 4, 1, "dangling");
	put_le32(image + 0x128, 0x999);
	put_le32(image + 0xFFC, 0x999);
	CHECK(yaffs_mount(MOUNT) == 0, "mount with a dangling hard link");
	{
		yaffs_DIR *d = yaffs_opendir(MOUNT);
		int n = 0;

		while (d && yaffs_readdir(d))
			n++;
		if (d)
			yaffs_closedir(d);
		CHECK(n == 2, "%d entries beside the dangling link", n);
	}
	CHECK(yaffs_unmount(MOUNT) == 0, "unmount");

	memset(image, 0, image_len);
	put_header(0, 0x7FFFFFFF, 0x100, 1, 1, "older");
	put_header(128, 0x80000000, 0x100, 1, 1, "newer");
	CHECK(yaffs_mount(MOUNT) == 0, "mount across 0x80000000");
	CHECK(yaffs_lstat(MOUNT "/newer", &st) == 0 &&
		      yaffs_lstat(MOUNT "/older", &st) != 0,
	      "the header of sequence number 0x80000000 is not the current one");
	CHECK(yaffs_unmount(MOUNT) == 0, "unmount");
}

static void put_file(const char *path, const uint8_t *data, size_t len)
{
	int fd = yaffs_open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);

	CHECK(fd >= 0 && yaffs_write(fd, data, (unsigned int)len) == (int)len,
	      "write %s", path);
	if (fd >= 0)
		yaffs_close(fd);
}

/* The number of entries in the root, lost+found included. */
static int root_entries(void)
{
	yaffs_DIR *d = yaffs_opendir(MOUNT);
	int n = 0;

	while (d && yaffs_readdir(d))
		n++;
	if (d)
		yaffs_closedir(d);
	return n;
}

static long file_len(const char *path)
{
	struct yaffs_stat st;

	return yaffs_lstat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* The chunk of the image holding the header that shadows another, or -1. */
static long shadowing_chunk(void)
{
	size_t c;

	for (c = 0; c < image_len; c += WB_YAFFS_CHUNK_BYTES)
		if (le32(image + c + 0xFF0) && le32(image + c + 0xFF8) >> 31 &&
		    le32(image + c + 0x1E8))
			return (long)(c / WB_YAFFS_CHUNK_BYTES);
	return -1;
}

/* Whether chunk c is a header of obj_id in the deleted directory. */
static int is_deletion_record(size_t c, uint32_t obj_id)
{
	const uint8_t *p = image + c * WB_YAFFS_CHUNK_BYTES;

	return le32(p + 0xFF0) && (le32(p + 0xFF4) & 0x3FFFFu) == obj_id &&
	       (le32(p + 0xFF8) & 0x8FFFFFFFu) == 0x80000004u;
}

/*
 * S-12.054 after a rename over a file. yaffs2's own rename writes the
 * deletion record of the file it replaces; the shadows image holds only
 * the shadowing header (S-12.050 rule 5), which a power cut between the
 * two writes also leaves. Either way a mount writes and erases nothing.
 * The deletion record is the first chunk the next change writes, and
 * the collection that later clears the shadowing header's flag does not
 * bring the replaced file back.
 */
static void check_rename_over(void)
{
	uint8_t *before = tl_xmalloc(image_len), data[300];
	size_t c, first = 0;
	uint32_t first_seq = 0, shadowed;
	long h;
	int k, moved = 0;

	for (k = 0; k < (int)sizeof(data); k++)
		data[k] = (uint8_t)(k * 7 + 1);

	/* By yaffs2's rename. */
	memset(image, 0, image_len);
	CHECK(yaffs_mount(MOUNT) == 0, "mount empty");
	put_file(MOUNT "/a", data, 100);
	put_file(MOUNT "/b", data + 1, 200);
	CHECK(yaffs_rename(MOUNT "/a", MOUNT "/b") == 0, "rename a over b");
	CHECK(yaffs_unmount(MOUNT) == 0, "unmount");
	mount_only("after a rename over", before);
	mount_only("after a rename over (again)", before);
	CHECK(yaffs_mount(MOUNT) == 0, "mount");
	CHECK(file_len(MOUNT "/b") == 100 && file_len(MOUNT "/a") < 0 &&
	      root_entries() == 2, "rename over: the tree");
	CHECK(yaffs_unmount(MOUNT) == 0, "unmount");

	/* As the shadows image holds it: no deletion record. */
	load_image("yaffs_shadows.img");
	h = shadowing_chunk();
	CHECK(h >= 0, "shadows has no shadowing header");
	if (h < 0)
		goto out;
	shadowed = le32(image + (size_t)h * WB_YAFFS_CHUNK_BYTES + 0x1E8);
	for (c = 0; c < image_len / WB_YAFFS_CHUNK_BYTES; c++)
		CHECK(!is_deletion_record(c, shadowed),
		      "shadows already has a deletion record");
	mount_only("shadows", before);
	mount_only("shadows (again)", before);

	/* The first change writes the deletion record first. */
	memcpy(before, image, image_len);
	CHECK(yaffs_mount(MOUNT) == 0, "mount shadows");
	put_file(MOUNT "/c", data, 1);
	CHECK(yaffs_unmount(MOUNT) == 0, "unmount");
	for (c = 0; c < image_len / WB_YAFFS_CHUNK_BYTES; c++) {
		size_t at = c * WB_YAFFS_CHUNK_BYTES;
		uint32_t seq = le32(image + at + 0xFF0);

		if (!seq || !memcmp(before + at, image + at,
				    WB_YAFFS_CHUNK_BYTES))
			continue;
		if (!first_seq || seq < first_seq) {
			first_seq = seq;
			first = c;
		}
	}
	CHECK(first_seq && is_deletion_record(first, shadowed),
	      "the first chunk of the first change (%zu) is not the deletion "
	      "record of object 0x%lx", first, (unsigned long)shadowed);
	mount_only("shadows after a change", before);
	CHECK(yaffs_mount(MOUNT) == 0, "mount");
	CHECK(file_len(MOUNT "/b") == 100 && file_len(MOUNT "/c") == 1 &&
	      root_entries() == 3, "shadows after a change: the tree");
	CHECK(yaffs_unmount(MOUNT) == 0, "unmount");

	/*
	 * A collection that moves the shadowing header (block 1) clears its
	 * shadows flag; the replaced file, whose header is in block 0 with
	 * 127 other files, stays deleted. (yaffs2 collects block 0 first here
	 * too, so this checks the outcome, not the order.)
	 */
	memset(image, 0, image_len);
	put_header(0, 0x1001, 0x101, 1, 1, "b");
	for (k = 1; k < 128; k++) {
		char name[8];

		snprintf(name, sizeof(name), "k%d", k);
		put_header((uint32_t)k, 0x1001, 0x200u + (uint32_t)k, 1, 1, name);
	}
	put_header(128, 0x1002, 0x100, 1, 1, "b");
	put_le32(image + 128 * WB_YAFFS_CHUNK_BYTES + 0x1E8, 0x101);
	put_le32(image + 128 * WB_YAFFS_CHUNK_BYTES + 0x1F8, 0x101);
	put_le32(image + 128 * WB_YAFFS_CHUNK_BYTES + 0xFF8,
		 0x80000000u | 1u << 29 | 1u);
	mount_only("shadowing header in a newer block", before);
	memcpy(before, image, image_len);
	CHECK(yaffs_mount(MOUNT) == 0 && root_entries() == 129,
	      "mount: the tree");
	{
		/* A few chunks at a time, until a collection moves it. */
		static uint8_t some[4 * 4080];
		int fd = yaffs_open(MOUNT "/fill", O_CREAT | O_RDWR, 0644);

		CHECK(fd >= 0, "create fill");
		for (k = 0; fd >= 0 && k < 200 && !moved; k++) {
			CHECK(yaffs_write(fd, some, sizeof(some)) ==
				      (int)sizeof(some), "write fill");
			yaffs_flush(fd);
			moved = memcmp(before + 128 * WB_YAFFS_CHUNK_BYTES,
				       image + 128 * WB_YAFFS_CHUNK_BYTES,
				       WB_YAFFS_CHUNK_BYTES) != 0;
		}
		if (fd >= 0)
			yaffs_close(fd);
	}
	CHECK(yaffs_unmount(MOUNT) == 0, "unmount");
	CHECK(moved, "no collection reached the shadowing header");
	CHECK(yaffs_mount(MOUNT) == 0, "mount");
	CHECK(root_entries() == 130 && file_len(MOUNT "/b") == 0,
	      "the shadowed file came back");
	CHECK(yaffs_unmount(MOUNT) == 0, "unmount");
out:
	free(before);
}

/*
 * The rest of the clean-up a mount leaves (S-12.054): a block the scan
 * finds with nothing in use, and an object in the unlinked directory
 * (object 3), are erased and deleted by the first change, not the mount.
 */
static void check_deferred_cleanup(void)
{
	uint8_t *before = tl_xmalloc(image_len);
	size_t c;
	int found = 0;
	uint32_t k;

	memset(image, 0, image_len);
	for (k = 0; k < 128; k++)
		put_header(k, 0x1001, 0x100, 1, 1, "old");
	put_header(128, 0x1002, 0x100, 1, 1, "kept");
	put_header(129, 0x1002, 0x101, 1, 3, "unlinked");
	mount_only("dead block", before);
	mount_only("dead block (again)", before);

	n_erases = 0;
	CHECK(yaffs_mount(MOUNT) == 0, "mount dead block");
	put_file(MOUNT "/new", (const uint8_t *)"x", 1);
	CHECK(yaffs_unmount(MOUNT) == 0, "unmount");
	CHECK(n_erases >= WB_YAFFS_SECTORS_PER_BLOCK, "the dead block stayed");
	for (c = 0; c < 128; c++)
		CHECK(le32(image + c * WB_YAFFS_CHUNK_BYTES + 0xFF0) != 0x1001,
		      "chunk %zu of the dead block is still there", c);
	for (c = 0; c < image_len / WB_YAFFS_CHUNK_BYTES; c++)
		found |= is_deletion_record(c, 0x101);
	CHECK(found, "no deletion record for the unlinked object");
	mount_only("dead block after a change", before);
	CHECK(yaffs_mount(MOUNT) == 0, "mount");
	CHECK(file_len(MOUNT "/kept") == 0 && file_len(MOUNT "/new") == 1 &&
	      root_entries() == 3, "dead block after a change: the tree");
	CHECK(yaffs_unmount(MOUNT) == 0, "unmount");
	free(before);
}

/*
 * S-12.055 against a reader that keeps checkpoints (upstream's default; TE
 * restores one): it writes a file and leaves a checkpoint at its unmount;
 * our session rewrites the file; the reader's next mount must not restore
 * the old state, so it reads our file. The device is the eMMC's: no erase
 * operation (the glue writes zeros), and a buffer that is not 4-byte
 * aligned is refused, as the eMMC driver refuses it. A checkpoint block
 * that will not erase stops our session before it writes anything.
 */
static struct wb_ramdisk emmc_ram;
static unsigned long emmc_unaligned;
static uint32_t emmc_stuck_first, emmc_stuck_end;

static int emmc_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	const struct wb_ramdisk *rd = ctx;

	if ((uintptr_t)buf & 3u) {
		emmc_unaligned++;
		return -1;
	}
	memcpy(buf, rd->mem + (size_t)lba * 512u, (size_t)count * 512u);
	return 0;
}

static int emmc_write(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
	struct wb_ramdisk *rd = ctx;

	if ((uintptr_t)buf & 3u) {
		emmc_unaligned++;
		return -1;
	}
	if (lba < emmc_stuck_end && lba + count > emmc_stuck_first)
		return -1;
	memcpy(rd->mem + (size_t)lba * 512u, buf, (size_t)count * 512u);
	return 0;
}

static int checkpoint_block(const uint8_t *vol, size_t len)
{
	size_t c;

	for (c = 0; c < len; c += 128u * WB_YAFFS_CHUNK_BYTES)
		if (le32(vol + c + 0xFF0) == 0x21)
			return (int)(c / (128u * WB_YAFFS_CHUNK_BYTES));
	return -1;
}

static void check_checkpoint_reader(void)
{
	static struct wb_blockdev cbdev;
	static struct yaffs_dev te, wb;
	const size_t n_blocks = 64, len = n_blocks * 128 * 4096;
	uint8_t *vol = calloc(1, len), *a = tl_xmalloc(700000);
	uint8_t *b = tl_xmalloc(700000), *back = tl_xmalloc(700000);
	uint8_t *before = tl_xmalloc(len);
	size_t i;
	int fd, got, ck;

	for (i = 0; i < 700000; i++) {
		a[i] = (uint8_t)(i * 7);
		b[i] = (uint8_t)(i * 11 + 3);
	}
	emmc_ram.mem = vol;
	emmc_ram.n_sectors = (uint32_t)(len / 512);
	cbdev.ctx = &emmc_ram;
	cbdev.n_sectors = emmc_ram.n_sectors;
	cbdev.read = emmc_read;
	cbdev.write = emmc_write;
	cbdev.erase = NULL;
	CHECK(wb_yaffs_add_blockdev(&te, MOUNT "t", &cbdev, 0,
				    (uint32_t)n_blocks - 1) == 0, "add t");
	CHECK(wb_yaffs_add_blockdev(&wb, MOUNT "b", &cbdev, 0,
				    (uint32_t)n_blocks - 1) == 0, "add b");
	/* yaffs2 checkpoints only volumes of 60 blocks or more */
	te.param.skip_checkpt_rd = 0;
	te.param.skip_checkpt_wr = 0;
	te.param.n_reserved_blocks = 2;
	wb.param.n_reserved_blocks = 2;

	/* The checkpointing reader writes os.ldr and checkpoints. */
	CHECK(yaffs_mount(MOUNT "t") == 0, "t: mount");
	fd = yaffs_open(MOUNT "t/os.ldr", O_CREAT | O_RDWR | O_TRUNC, 0644);
	CHECK(fd >= 0 && yaffs_write(fd, a, 300000) == 300000, "t: write");
	yaffs_close(fd);
	CHECK(yaffs_unmount(MOUNT "t") == 0, "t: unmount");
	ck = checkpoint_block(vol, len);
	CHECK(ck >= 0, "t: left no checkpoint");

	/* The checkpoint block will not erase: our session writes nothing. */
	emmc_stuck_first = (uint32_t)ck * WB_YAFFS_SECTORS_PER_BLOCK;
	emmc_stuck_end = emmc_stuck_first + WB_YAFFS_SECTORS_PER_BLOCK;
	memcpy(before, vol, len);
	CHECK(yaffs_mount(MOUNT "b") == 0, "b: mount");
	/* yaffs2's cache takes the bytes; their chunks are refused. */
	fd = yaffs_open(MOUNT "b/os.ldr", O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd >= 0) {
		(void)yaffs_write(fd, b, 700000);
		yaffs_close(fd);
	}
	fd = yaffs_open(MOUNT "b/new", O_CREAT | O_RDWR, 0644);
	CHECK(fd < 0, "b: created a file with a checkpoint left");
	if (fd >= 0)
		yaffs_close(fd);
	CHECK(yaffs_unmount(MOUNT "b") == 0, "b: unmount (stuck)");
	CHECK(!memcmp(before, vol, len), "b: changed the volume with a checkpoint left");

	/* It erases now: the checkpoint goes before the first chunk. */
	emmc_stuck_first = emmc_stuck_end = 0;
	CHECK(yaffs_mount(MOUNT "b") == 0, "b: mount");
	fd = yaffs_open(MOUNT "b/os.ldr", O_CREAT | O_RDWR | O_TRUNC, 0644);
	CHECK(fd >= 0 && yaffs_write(fd, b, 700000) == 700000, "b: write");
	yaffs_close(fd);
	CHECK(yaffs_unmount(MOUNT "b") == 0, "b: unmount");
	CHECK(checkpoint_block(vol, len) < 0, "b: checkpoint block %d left",
	      checkpoint_block(vol, len));

	/* The reader's next mount scans and sees our file. */
	CHECK(yaffs_mount(MOUNT "t") == 0, "t: remount");
	fd = yaffs_open(MOUNT "t/os.ldr", O_RDONLY, 0);
	got = fd >= 0 ? yaffs_read(fd, back, 700000) : -1;
	if (fd >= 0)
		yaffs_close(fd);
	CHECK(got == 700000 && !memcmp(back, b, 700000),
	      "t: reads %d bytes of os.ldr, not ours", got);
	CHECK(yaffs_unmount(MOUNT "t") == 0, "t: unmount 2");
	CHECK(emmc_unaligned == 0, "%lu unaligned buffers reached the eMMC",
	      emmc_unaligned);
	CHECK(wb_yaffs_bug_count == 0, "yaffs2 internal error at %s:%d",
	      wb_yaffs_bug_file ? wb_yaffs_bug_file : "?", wb_yaffs_bug_line);
	free(vol);
	free(a);
	free(b);
	free(back);
	free(before);
}

/*
 * S-12.043 on a device with no erase operation (the eMMC's): the glue's
 * erase writes the block's sectors with zeros in 8 runs of 128, ascending,
 * and leaves the same image as 1,024 single-sector writes of zeros; the
 * first run that fails fails the erase and nothing after it is written,
 * but a run that times out is written again a sector at a time; the last
 * block stays inside the device and one past it is refused.
 */
#define ERASE_BLOCKS 4u
#define ERASE_SECTORS (ERASE_BLOCKS * WB_YAFFS_SECTORS_PER_BLOCK)
static struct wb_ramdisk erase_ram;
static uint32_t erase_lba[64], erase_count[64];
static unsigned erase_n;
static uint32_t erase_fail_lba = UINT32_MAX;
static uint32_t erase_slow_lba = UINT32_MAX;	/* a run over it times out */

static int erase_write(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
	struct wb_ramdisk *rd = ctx;

	if (erase_n < sizeof(erase_lba) / sizeof(erase_lba[0])) {
		erase_lba[erase_n] = lba;
		erase_count[erase_n] = count;
	}
	erase_n++;
	if (count > 1u && erase_slow_lba >= lba && erase_slow_lba - lba < count)
		return WB_MSI_TIMEOUT;
	if (((uintptr_t)buf & 3u) || lba > rd->n_sectors ||
	    count > rd->n_sectors - lba ||
	    (erase_fail_lba >= lba && erase_fail_lba - lba < count))
		return -1;
	memcpy(rd->mem + (size_t)lba * 512u, buf, (size_t)count * 512u);
	return 0;
}

static void erase_fill(uint8_t *mem)
{
	size_t i;

	for (i = 0; i < (size_t)ERASE_SECTORS * 512u; i++)
		mem[i] = (uint8_t)(i * 29u + (i >> 9) + 1u);
}

static void check_erase_runs(void)
{
	static struct wb_blockdev ebd;
	static struct yaffs_dev edev;
	static const uint8_t zero[512];
	const size_t len = (size_t)ERASE_SECTORS * 512u;
	uint8_t *mem = tl_xmalloc(len), *want = tl_xmalloc(len);
	uint32_t b, i;

	erase_ram.mem = mem;
	erase_ram.n_sectors = ERASE_SECTORS;
	ebd.ctx = &erase_ram;
	ebd.n_sectors = ERASE_SECTORS;
	ebd.read = NULL;
	ebd.write = erase_write;
	ebd.erase = NULL;
	CHECK(wb_yaffs_add_blockdev(&edev, MOUNT "e", &ebd, 0,
				    ERASE_BLOCKS - 1u) == 0, "erase: add");

	for (b = 0; b < ERASE_BLOCKS; b++) {
		/* the one-sector path, on the same starting image */
		erase_fill(want);
		for (i = 0; i < WB_YAFFS_SECTORS_PER_BLOCK; i++)
			memcpy(want + ((size_t)b * WB_YAFFS_SECTORS_PER_BLOCK + i) * 512u,
			       zero, 512u);
		erase_fill(mem);
		erase_n = 0;
		CHECK(edev.drv.drv_erase_fn(&edev, (int)b) == YAFFS_OK,
		      "erase: block %u failed", (unsigned)b);
		CHECK(!memcmp(mem, want, len),
		      "erase: block %u's image differs from a sector at a time",
		      (unsigned)b);
		CHECK(erase_n == 8u, "erase: %u writes for block %u, not 8",
		      erase_n, (unsigned)b);
		for (i = 0; i < 8u && i < erase_n; i++)
			CHECK(erase_lba[i] == b * WB_YAFFS_SECTORS_PER_BLOCK + 128u * i &&
			      erase_count[i] == 128u,
			      "erase: write %u of block %u at %lu, %lu sectors",
			      (unsigned)i, (unsigned)b, (unsigned long)erase_lba[i],
			      (unsigned long)erase_count[i]);
	}

	/* a run that fails: the erase fails, the runs before it are zero and
	 * nothing after it is written */
	erase_fill(mem);
	erase_fill(want);
	memset(want + (size_t)WB_YAFFS_SECTORS_PER_BLOCK * 512u, 0, 256u * 512u);
	erase_fail_lba = WB_YAFFS_SECTORS_PER_BLOCK + 300u;
	erase_n = 0;
	CHECK(edev.drv.drv_erase_fn(&edev, 1) == YAFFS_FAIL,
	      "erase: a failed run did not fail the erase");
	CHECK(erase_n == 3u, "erase: %u writes after a failed third run", erase_n);
	CHECK(!memcmp(mem, want, len), "erase: a failed run left other bytes");

	/* a run that times out is written a sector at a time: the erase
	 * succeeds and leaves the block zero */
	erase_fail_lba = UINT32_MAX;
	erase_slow_lba = WB_YAFFS_SECTORS_PER_BLOCK + 300u;
	erase_fill(mem);
	erase_fill(want);
	memset(want + (size_t)WB_YAFFS_SECTORS_PER_BLOCK * 512u, 0,
	       (size_t)WB_YAFFS_SECTORS_PER_BLOCK * 512u);
	erase_n = 0;
	CHECK(edev.drv.drv_erase_fn(&edev, 1) == YAFFS_OK,
	      "erase: a run that timed out failed the erase");
	CHECK(!memcmp(mem, want, len), "erase: the retried run left other bytes");
	CHECK(erase_n == 8u + 128u, "erase: %u writes with one run retried",
	      erase_n);
	/* the log holds 64 writes: the three runs before, then 61 of the retry */
	for (i = 0; i < 64u - 3u; i++)
		CHECK(erase_lba[3u + i] == WB_YAFFS_SECTORS_PER_BLOCK + 256u + i &&
		      erase_count[3u + i] == 1u,
		      "erase: retry write %u at %lu, %lu sectors", (unsigned)i,
		      (unsigned long)erase_lba[3u + i],
		      (unsigned long)erase_count[3u + i]);

	/* a sector of the retry that fails fails the erase, nothing after */
	erase_fail_lba = erase_slow_lba;
	erase_fill(mem);
	erase_fill(want);
	memset(want + (size_t)WB_YAFFS_SECTORS_PER_BLOCK * 512u, 0, 300u * 512u);
	erase_n = 0;
	CHECK(edev.drv.drv_erase_fn(&edev, 1) == YAFFS_FAIL,
	      "erase: a failed sector of a retried run did not fail the erase");
	CHECK(erase_n == 3u + 45u, "erase: %u writes, a retry failing at 300",
	      erase_n);
	CHECK(!memcmp(mem, want, len), "erase: a failed retry left other bytes");
	erase_slow_lba = UINT32_MAX;
	erase_fail_lba = UINT32_MAX;

	/* one past the last block: refused, nothing written */
	erase_n = 0;
	CHECK(edev.drv.drv_erase_fn(&edev, (int)ERASE_BLOCKS) == YAFFS_FAIL &&
	      erase_n == 0u, "erase: a block past the device");
	CHECK(edev.drv.drv_erase_fn(&edev, -1) == YAFFS_FAIL && erase_n == 0u,
	      "erase: block -1");
	free(mem);
	free(want);
}

/*
 * S-12.060: on a device with a reliable write, every chunk goes through
 * it, eight sectors at the chunk's first; the plain write carries only the
 * zero runs of a device with no erase.
 */
static unsigned rel_n, plain_n;
static uint32_t rel_lba, rel_count;

static int route_reliable(void *ctx, uint32_t lba, const void *buf,
			  uint32_t count)
{
	rel_n++;
	rel_lba = lba;
	rel_count = count;
	return erase_write(ctx, lba, buf, count);
}

static int route_plain(void *ctx, uint32_t lba, const void *buf,
		       uint32_t count)
{
	plain_n++;
	return erase_write(ctx, lba, buf, count);
}

static void check_chunk_route(void)
{
	static struct wb_blockdev rbd;
	static struct yaffs_dev rdev;
	static uint8_t chunk[WB_YAFFS_CHUNK_BYTES];
	const size_t len = (size_t)ERASE_SECTORS * 512u;
	uint8_t *mem = tl_xmalloc(len);

	memset(mem, 0, len);
	memset(chunk, 0xC3, sizeof(chunk));
	erase_ram.mem = mem;
	erase_ram.n_sectors = ERASE_SECTORS;
	rbd.ctx = &erase_ram;
	rbd.n_sectors = ERASE_SECTORS;
	rbd.write = route_plain;
	rbd.erase = NULL;
	rbd.write_reliable = route_reliable;
	CHECK(wb_yaffs_add_blockdev(&rdev, MOUNT "r", &rbd, 0,
				    ERASE_BLOCKS - 1u) == 0, "route: add");

	CHECK(rdev.drv.drv_write_chunk_fn(&rdev, 130, chunk, (int)sizeof(chunk),
					  NULL, 0) == YAFFS_OK, "route: write");
	CHECK(rel_n == 1u && plain_n == 0u && rel_lba == 130u * 8u &&
	      rel_count == 8u, "route: %u reliable at %lu (%lu), %u plain",
	      rel_n, (unsigned long)rel_lba, (unsigned long)rel_count, plain_n);
	CHECK(!memcmp(mem + (size_t)130u * 4096u, chunk, sizeof(chunk)),
	      "route: the chunk's bytes");

	rel_n = plain_n = 0;
	erase_n = 0;
	CHECK(rdev.drv.drv_erase_fn(&rdev, 1) == YAFFS_OK, "route: erase");
	CHECK(rel_n == 0u && plain_n == 8u,
	      "route: a zero-written erase took %u reliable, %u plain writes",
	      rel_n, plain_n);
	free(mem);
}

int main(void)
{
	FILE *f = tl_open_vectors("yaffs.txt");
	char *line = NULL, *fld[2];
	size_t cap = 0;
	unsigned long n_blocks;

	if (tl_fields(f, &line, &cap, fld, 2) != 2 || strcmp(fld[0], "blocks"))
		return 2;
	n_blocks = strtoul(fld[1], NULL, 10);
	free(line);
	image_len = n_blocks * WB_YAFFS_SECTORS_PER_BLOCK * WB_SECTOR_SIZE;
	image = tl_xmalloc(image_len);
	wb_ramdisk_init(&bdev, &ram, image, (uint32_t)(image_len / 512));
	ram_write = bdev.write;
	ram_erase = bdev.erase;
	bdev.write = count_write;
	if (ram_erase)
		bdev.erase = count_erase;
	CHECK(wb_yaffs_add_blockdev(&vol_dev, MOUNT, &bdev, 0,
				    (uint32_t)n_blocks - 1) == 0, "add");
	/*
	 * yaffs2 needs more blocks than the reserve plus 2; the vector images
	 * have 8. The reserve (S-12.056) only limits writing.
	 */
	vol_dev.param.n_reserved_blocks = 2;

	check_volumes(f);
	check_regressions();
	check_rename_over();
	check_deferred_cleanup();
	check_checkpoint_erase();
	check_checkpoint_reader();
	check_write();
	check_erase_runs();
	check_chunk_route();

	fclose(f);
	free(image);
	return tl_done("test_yaffs2");
}
