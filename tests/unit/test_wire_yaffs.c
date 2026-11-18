/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The yaffs2 volume through src/wire_yaffs.c, against yaffs.txt and S-12
 * section 2:
 * - every image of yaffs_trees.json through wire_yaffs_mount(): the
 *   big-endian ones refused without a sector written (S-12.052), every
 *   other mounted and its tree read through struct s11_fs_ops;
 * - a refused volume formatted (S-12.057): the root and lost+found only;
 * - the free space reported within S-12.056's bound;
 * - the s11_fs_ops contract (remove, move, copy, read) and the S-15 env
 *   helpers (pack_dir, read_manifest, dir_exists).
 * test_yaffs2 checks the library's own reading of the same images.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "waffle/s11_staging.h"
#include "waffle/wire_yaffs.h"
#include "wb_yaffs.h"

#define MAX_ENTRIES 64
#define BLOCKS 40u

static uint8_t *image;
static size_t image_len;
static struct wb_blockdev bd;
static struct wb_ramdisk ram;
static struct wire_yaffs Y;
static struct s11_fs_ops fs;
static uint8_t file_buf[1 << 20];

struct entry {
	char path[600];
	char type[16];
	unsigned long len;
	char sha[65];
	bool seen;
};

static void load_image(const char *file)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "s12/%s", file);
	f = fopen(tl_vector_path(path), "rb");
	CHECK(f != NULL && fread(image, 1, image_len, f) == image_len, "%s",
	      path);
	if (f != NULL)
		fclose(f);
}

/* Every path under `dir` is an entry of the vector, files with their
 * contents; lost+found's objects are counted in *lost. */
static void walk(const char *dir, struct entry *want, int n_want,
		 char lost[][65], int n_lost, int *found_lost)
{
	char name[S11_PATH_MAX], path[600], sha[65];
	bool is_dir;

	for (size_t i = 0; wire_yaffs_list(&Y, dir, i, name, sizeof(name),
					   &is_dir, NULL); i++) {
		struct entry *e = NULL;
		bool d;
		uint32_t size = 0;

		snprintf(path, sizeof(path), "%s%s%s", dir, *dir ? "/" : "", name);
		if (!strcmp(dir, "lost+found")) {
			size_t n = fs.read(fs.ctx, path, file_buf, sizeof(file_buf));
			bool known = false;

			tl_sha256_hex(sha, file_buf, n);
			for (int k = 0; k < n_lost; k++)
				known |= !strcmp(lost[k], sha);
			CHECK(known, "lost+found/%s: an orphan of the vector", name);
			(*found_lost)++;
			continue;
		}
		for (int k = 0; k < n_want; k++)
			if (!strcmp(want[k].path, path))
				e = &want[k];
		CHECK(e != NULL && !e->seen, "%s: in the tree", path);
		if (e == NULL)
			continue;
		e->seen = true;
		CHECK(fs.stat(fs.ctx, path, &d, &size), "%s: stat", path);
		if (!strcmp(e->type, "dir")) {
			CHECK(is_dir && d, "%s: a directory", path);
			walk(path, want, n_want, lost, n_lost, found_lost);
		} else if (!strcmp(e->type, "file")) {
			size_t n = fs.read(fs.ctx, path, file_buf, sizeof(file_buf));

			tl_sha256_hex(sha, file_buf, n);
			CHECK(!is_dir && size == e->len && n == e->len &&
			      !strcmp(sha, e->sha), "%s: contents", path);
		} else {
			CHECK(!is_dir, "%s: a %s", path, e->type);
		}
	}
}

/*
 * The eMMC driver takes only 4-byte aligned buffers: so does this device,
 * so a read that the S-12.052 scan or yaffs2 makes from another buffer
 * fails here as it does on the unit.
 */
static int (*ram_read)(void *, uint32_t, void *, uint32_t);
static int (*ram_write)(void *, uint32_t, const void *, uint32_t);
static unsigned long unaligned;

/* A read of fewer sectors than a chunk fails when short_fail is set, and
 * is counted (the tags-only reads' fall-back; the S-12.052 check's reads
 * are short too). */
static bool short_fail;
static unsigned long short_failed;

static int emmc_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	if ((uintptr_t)buf & 3u) {
		unaligned++;
		return -1;
	}
	if (short_fail && count < WB_YAFFS_SECTORS_PER_CHUNK) {
		short_failed++;
		return -1;
	}
	return ram_read(ctx, lba, buf, count);
}

static int emmc_write(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
	if ((uintptr_t)buf & 3u) {
		unaligned++;
		return -1;
	}
	return ram_write(ctx, lba, buf, count);
}

static void check_volume(const char *name, bool refused, struct entry *want,
			 int n_want, char lost[][65], int n_lost)
{
	uint8_t *before = tl_xmalloc(image_len);

	memcpy(before, image, image_len);
	/*
	 * S-12.044: the same tree from tags read alone (the glue's default),
	 * from whole chunks only, and from tags-alone reads that all fail and
	 * fall back to the whole chunk.
	 */
	for (int mode = 0; mode < (refused ? 1 : 3); mode++) {
		struct wb_yaffs_read_counts c;
		enum wire_mount m;
		int found_lost = 0;

		for (int k = 0; k < n_want; k++)
			want[k].seen = false;
		wb_yaffs_tags_reads(mode != 1);
		short_failed = 0;
		CHECK(wire_yaffs_add(&Y, "/yaffs2", &bd, 0,
				     (uint32_t)(image_len / (WB_YAFFS_SECTORS_PER_BLOCK *
							     WB_SECTOR_SIZE)) - 1),
		      "%s: add", name);
		/* 8 blocks: below the reserve of 5 plus yaffs2's own 2 (test_yaffs2) */
		Y.dev.param.n_reserved_blocks = 2;
		CHECK(wire_yaffs_big_endian(&bd, Y.first_block, Y.last_block, Y.buf) ==
		      refused, "%s: big-endian", name);
		wb_yaffs_read_counts(&c, true);
		short_fail = mode == 2;
		m = wire_yaffs_mount(&Y);
		short_fail = false;
		CHECK(m == (refused ? WIRE_MOUNT_REFUSED : WIRE_MOUNT_OK) &&
		      wire_yaffs_volume_refused(&Y) == refused, "%s: mount %d", name,
		      (int)m);
		wb_yaffs_read_counts(&c, true);
		if (m == WIRE_MOUNT_OK) {
			walk("", want, n_want, lost, n_lost, &found_lost);
			for (int k = 0; k < n_want; k++)
				CHECK(want[k].seen, "%s: %s listed", name, want[k].path);
			CHECK(found_lost == n_lost, "%s: %d orphans, want %d", name,
			      found_lost, n_lost);
			CHECK(mode == 0 ? c.tags_only != 0u && c.fell_back == 0u :
			      mode == 1 ? c.tags_only == 0u && c.fell_back == 0u :
			      c.tags_only == 0u && c.fell_back != 0u &&
			      c.fell_back <= short_failed, "%s: mode %d, %lu tags only, "
			      "%lu whole, %lu fell back, %lu short reads failed", name, mode,
			      (unsigned long)c.tags_only, (unsigned long)c.whole,
			      (unsigned long)c.fell_back, short_failed);
		}
		wire_yaffs_detach(&Y);
		wb_yaffs_tags_reads(true);
		short_fail = false;
		/* S-12.054: a session that changes nothing writes nothing */
		CHECK(!memcmp(before, image, image_len), "%s: nothing written", name);
	}
	if (refused) {
		/* S-12.057: a confirmed format erases a refused volume */
		bool dir;
		uint32_t size;
		char n0[S11_PATH_MAX];

		CHECK(wire_yaffs_add(&Y, "/yaffs2", &bd, 0,
				     (uint32_t)(image_len /
						(WB_YAFFS_SECTORS_PER_BLOCK *
						 WB_SECTOR_SIZE)) - 1), "add");
		Y.dev.param.n_reserved_blocks = 2;
		CHECK(wire_yaffs_mount(&Y) == WIRE_MOUNT_REFUSED &&
		      wire_yaffs_format(&Y) && !wire_yaffs_volume_refused(&Y),
		      "%s: formatted", name);
		CHECK(wire_yaffs_list(&Y, "", 0, n0, sizeof(n0), &dir, NULL) &&
		      !strcmp(n0, "lost+found") && dir &&
		      !wire_yaffs_list(&Y, "", 1, n0, sizeof(n0), &dir, NULL) &&
		      !wire_yaffs_stat(&Y, "lost+found/x", &dir, &size),
		      "%s: the root and lost+found only", name);
		wire_yaffs_detach(&Y);
	}
	free(before);
}

static void check_volumes(FILE *f)
{
	static struct entry want[MAX_ENTRIES];
	static char lost[8][65];
	char *line = NULL, *fld[6], name[64] = "";
	size_t cap = 0;
	int n, n_want = 0, n_lost = 0, volumes = 0;
	bool refused = false;

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
			}
		} else if (n == 3 && !strcmp(fld[0], "lostfound") && n_lost < 8) {
			snprintf(lost[n_lost++], 65, "%s", fld[2]);
		} else if (n == 1 && !strcmp(fld[0], "end") && *name) {
			check_volume(name, refused, want, n_want, lost, n_lost);
			volumes++;
			*name = '\0';
		}
	}
	free(line);
	CHECK(volumes >= 17, "only %d volumes", volumes);
}

/* ---- a volume of its own ---- */

static bool put(const char *rel, const char *text)
{
	char p[S11_PATH_MAX];
	const char *slash = strrchr(rel, '/');

	snprintf(p, sizeof(p), "%.*s", slash != NULL ? (int)(slash - rel) : 0,
		 rel);
	return (*p == '\0' || wire_yaffs_mkdir(&Y, p)) &&
	       wire_yaffs_staging_open(&Y, "t") &&
	       wire_yaffs_staging_write(&Y, (const uint8_t *)text, strlen(text)) &&
	       wire_yaffs_staging_close(&Y) && fs.move(fs.ctx, "upgrade/t", rel);
}

static bool text_is(const char *rel, const char *text)
{
	size_t n = fs.read(fs.ctx, rel, file_buf, sizeof(file_buf));

	return n == strlen(text) && !memcmp(file_buf, text, n);
}

static void test_ops(void)
{
	uint8_t *vol = calloc(BLOCKS, 128u * 4096u);
	struct wb_blockdev vbd;
	struct wb_ramdisk vram;
	uint64_t free0 = 0, free1 = 0;
	uint8_t buf[8];
	bool dir;
	uint32_t size;

	if (vol == NULL)
		return;
	wb_ramdisk_init(&vbd, &vram, vol, BLOCKS * 128u * 8u);
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &vbd, 0, BLOCKS - 1) &&
	      wire_yaffs_format(&Y) && wire_yaffs_staging_reset(&Y), "volume");

	/* S-12.056: within (empty-or-reclaimable chunks - 6 x 128) x 4080 */
	CHECK(wire_yaffs_free_space(&Y, &free0) &&
	      free0 <= (uint64_t)(BLOCKS - 6u) * 128u * 4080u && free0 != 0,
	      "free space %llu", (unsigned long long)free0);
	memset(file_buf, 0x5A, sizeof(file_buf));
	CHECK(wire_yaffs_staging_open(&Y, "big") &&
	      wire_yaffs_staging_write(&Y, file_buf, sizeof(file_buf)) &&
	      wire_yaffs_staging_close(&Y) && wire_yaffs_free_space(&Y, &free1) &&
	      free0 - free1 >= sizeof(file_buf), "free space after 1 MiB: %llu",
	      (unsigned long long)free1);

	/* move: replaces a file and creates the directories `to` implies */
	CHECK(put("a/b/c.txt", "one") && put("a/b/c.txt", "two") &&
	      text_is("a/b/c.txt", "two"), "move replaces");
	CHECK(fs.copy(fs.ctx, "a/b/c.txt", "d/e.txt") &&
	      text_is("d/e.txt", "two") && text_is("a/b/c.txt", "two"),
	      "copy creates the parents and keeps the source");
	CHECK(put("d/e.txt", "three") && fs.copy(fs.ctx, "a/b/c.txt", "d/e.txt") &&
	      text_is("d/e.txt", "two"), "copy replaces");
	CHECK(fs.read(fs.ctx, "a/b/c.txt", buf, 2) == 2 && buf[0] == 't' &&
	      buf[1] == 'w', "read takes the first cap bytes");
	CHECK(fs.read(fs.ctx, "a/b", buf, sizeof(buf)) == 0 &&
	      fs.read(fs.ctx, "none", buf, sizeof(buf)) == 0,
	      "read: a directory or a missing file gives 0");
	CHECK(!fs.move(fs.ctx, "none", "x") && !fs.copy(fs.ctx, "none", "x") &&
	      !wire_yaffs_stat(&Y, "x", &dir, &size), "a missing source fails");

	/* remove: a whole tree; an absent path is removed already */
	CHECK(fs.remove(fs.ctx, "a") && !wire_yaffs_stat(&Y, "a", &dir, &size) &&
	      wire_yaffs_remove(&Y, "a") && wire_yaffs_remove(&Y, "q/r"),
	      "remove");

	/* S-15.041: pack directories, files skipped */
	CHECK(wire_yaffs_mkdir(&Y, "content/dlc/P1") &&
	      put("content/dlc/file", "f") &&
	      put("content/dlc/P2/manifest.json", "{\"name\":\"two\"}"),
	      "packs");
	{
		char name[S11_PATH_MAX];
		int dirs = 0;
		bool p1 = false, p2 = false;

		for (size_t i = 0; wire_yaffs_pack_dir(&Y, i, name, sizeof(name));
		     i++) {
			dirs++;
			p1 |= !strcmp(name, "P1");
			p2 |= !strcmp(name, "P2");
		}
		CHECK(dirs == 2 && p1 && p2, "pack_dir: the two directories");
	}
	CHECK(wire_yaffs_read_manifest(&Y, "P2", file_buf, 6) == 6 &&
	      !memcmp(file_buf, "{\"name", 6) &&
	      wire_yaffs_read_manifest(&Y, "P1", file_buf, 6) == 0,
	      "read_manifest");
	CHECK(wire_yaffs_dir_exists(&Y, "content/dlc") &&
	      !wire_yaffs_dir_exists(&Y, "content/dlc/file") &&
	      !wire_yaffs_dir_exists(&Y, "none"), "dir_exists");
	wire_yaffs_detach(&Y);
	free(vol);
}

int main(void)
{
	FILE *f = tl_open_vectors("yaffs.txt");
	char *line = NULL, *fld[2];
	size_t cap = 0;
	unsigned long n_blocks;

	if (tl_fields(f, &line, &cap, fld, 2) != 2 || strcmp(fld[0], "blocks")) {
		free(line);
		fclose(f);
		return 2;
	}
	n_blocks = strtoul(fld[1], NULL, 10);
	free(line);
	image_len = n_blocks * WB_YAFFS_SECTORS_PER_BLOCK * WB_SECTOR_SIZE;
	image = tl_xmalloc(image_len);
	wb_ramdisk_init(&bd, &ram, image, (uint32_t)(image_len / 512));
	ram_read = bd.read;
	ram_write = bd.write;
	bd.read = emmc_read;
	bd.write = emmc_write;
	bd.erase = NULL;	/* the eMMC's case: the glue writes zeros */
	wire_yaffs_fs_ops(&Y, &fs);	/* ctx &Y, whichever volume it holds */
	check_volumes(f);
	CHECK(unaligned == 0, "%lu unaligned buffers reached the device", unaligned);
	fclose(f);
	free(image);
	test_ops();
	return tl_done("test_wire_yaffs");
}
