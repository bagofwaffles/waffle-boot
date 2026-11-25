/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11's content step and pack names on a yaffs2 volume in RAM through
 * src/core/fs/yaffs.c's s11_fs_ops: every install of s11_vectors.h through
 * s11_staged_scan() and s11_install_content(), against the tree after
 * (S-11.056, S-11.066, S-11.075-S-11.079), and every pack-name case
 * through s11_pack_name() (S-11.039).
 *
 * S-11.039 takes the first pack "in the staging directory's listing
 * order", and the vectors take that order as name order. yaffs2 lists a
 * directory newest entry first, so each set is written here in descending
 * name order, which yaffs2 then lists in ascending order; test_listing
 * states the order itself, and that a remount changes it.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "waffle/boot/device.h"
#include "waffle/install/content.h"
#include "waffle/install/staging.h"
#include "waffle/fs/yaffs.h"

#include "vectors/s11_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define B(r) (s11v_blob + (r).off)
#define BLOCKS 40u

static struct wb_blockdev bd;
static struct wb_ramdisk ram;
static struct wire_yaffs Y;
static struct s11_fs_ops fs;
static uint8_t file_buf[1 << 16];

static bool put(const char *rel, const uint8_t *data, size_t len)
{
	char path[WIRE_YAFFS_PATH_MAX];
	const char *slash = strrchr(rel, '/');
	int fd;
	bool ok;

	if (slash != NULL) {
		char dir[S11_PATH_MAX];

		snprintf(dir, sizeof(dir), "%.*s", (int)(slash - rel), rel);
		if (!wire_yaffs_mkdir(&Y, dir))
			return false;
	}
	if (!wire_yaffs_full_path(&Y, rel, path))
		return false;
	fd = yaffs_open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0)
		return false;
	ok = len == 0 || yaffs_write(fd, data, (unsigned int)len) == (int)len;
	return yaffs_close(fd) == 0 && ok;
}

/* The files of `set`, in descending path order: each pass takes the
 * greatest path below the last one taken. */
static void load_set(int set)
{
	const char *last = NULL;

	for (;;) {
		const struct s11v_file *next = NULL;

		for (size_t i = 0; i < N(s11v_files); i++) {
			const struct s11v_file *f = &s11v_files[i];

			if (f->set == set &&
			    (last == NULL || strcmp(f->path, last) < 0) &&
			    (next == NULL || strcmp(f->path, next->path) > 0))
				next = f;
		}
		if (next == NULL)
			return;
		CHECK(put(next->path, B(next->data), next->data.len), "put %s",
		      next->path);
		last = next->path;
	}
}

static void fresh(void)
{
	CHECK(wire_yaffs_format(&Y), "format");
}

/* Every file outside upgrade/ under `dir` is in `set` with its contents;
 * returns how many there are, or a count past any set's on a mismatch. */
static size_t files_in(const char *dir, int set)
{
	char name[S11_PATH_MAX], path[600];
	size_t found = 0;
	bool is_dir;

	for (size_t i = 0; wire_yaffs_list(&Y, dir, i, name, sizeof(name),
					   &is_dir, NULL); i++) {
		const struct s11v_file *f = NULL;
		size_t n;

		snprintf(path, sizeof(path), "%s%s%s", dir, *dir ? "/" : "", name);
		if (!strcmp(path, "upgrade"))
			continue;
		if (is_dir) {
			found += files_in(path, set);
			continue;
		}
		for (size_t k = 0; k < N(s11v_files); k++)
			if (s11v_files[k].set == set && !strcmp(s11v_files[k].path, path))
				f = &s11v_files[k];
		n = fs.read(fs.ctx, path, file_buf, sizeof(file_buf));
		CHECK(f != NULL && n == f->data.len &&
		      !memcmp(file_buf, B(f->data), n), "%s: in the set", path);
		found++;
	}
	return found;
}

static size_t set_files(int set)
{
	size_t n = 0;

	for (size_t i = 0; i < N(s11v_files); i++)
		n += s11v_files[i].set == set &&
		     strncmp(s11v_files[i].path, "upgrade/", 8) != 0 ? 1u : 0u;
	return n;
}

/* S-11.066, S-11.075-S-11.079, S-11.056 */
static void test_installs(void)
{
	for (size_t i = 0; i < N(s11v_installs); i++) {
		const struct s11v_install *v = &s11v_installs[i];
		struct s11_staged st;
		enum s11_result r;
		bool dir;
		uint32_t size;

		fresh();
		load_set(v->staged);
		load_set(v->before);
		s11_staged_scan(&fs, &st);
		CHECK(s11_staged_content_only(&st) == (v->content_only != 0),
		      "%s: content-only from the staging area", v->name);
		r = s11_install_content(&wb_device_opxy, &fs, v->content_only != 0);
		CHECK(r == (enum s11_result)v->result, "%s: result %d", v->name,
		      (int)r);
		CHECK(files_in("", v->after) == set_files(v->after),
		      "%s: the tree after", v->name);
		if (v->result == S11_OK)
			CHECK(wire_yaffs_stat(&Y, "upgrade/firmware/blackfin/content",
					      &dir, &size) && dir,
			      "%s: the content directory stays (S-11.056)", v->name);
	}
}

/* S-11.039 */
static void test_pack_names(void)
{
	for (size_t i = 0; i < N(s11v_pack_names); i++) {
		const struct s11v_pack_name *v = &s11v_pack_names[i];
		char name[256];

		fresh();
		load_set(v->staged);
		s11_pack_name(&wb_device_opxy, &fs, name, sizeof(name));
		/* alt: S-11.039 lets a manifest over 4096 bytes be read in its
		 * first 4096 bytes only */
		CHECK(strcmp(name, v->pack) == 0 ||
		      (v->alt != NULL && strcmp(name, v->alt) == 0),
		      "%s: '%s', want '%s'", v->name, name, v->pack);
	}
}

/* yaffs2's listing order, which S-11.039 follows: newest entry first
 * until the volume is mounted again. */
static void test_listing(void)
{
	static const char *const dir = "upgrade/firmware/blackfin/content/dlc";
	char name[S11_PATH_MAX], p[200];
	bool is_dir;

	fresh();
	snprintf(p, sizeof(p), "%s/A/manifest.json", dir);
	CHECK(put(p, (const uint8_t *)"{\"name\": \"A\"}", 13), "A");
	snprintf(p, sizeof(p), "%s/B/manifest.json", dir);
	CHECK(put(p, (const uint8_t *)"{\"name\": \"B\"}", 13), "B");
	CHECK(fs.list(fs.ctx, dir, 0, name, sizeof(name), &is_dir) &&
	      !strcmp(name, "B"), "the pack written last is listed first");
	s11_pack_name(&wb_device_opxy, &fs, name, sizeof(name));
	CHECK(!strcmp(name, "B"), "S-11.039: written A then B, named '%s'", name);
	wire_yaffs_unmount(&Y);
	CHECK(wire_yaffs_mount(&Y) == WIRE_MOUNT_OK, "remount");
	/* the mount's scan builds the lists again: the order changes */
	CHECK(fs.list(fs.ctx, dir, 0, name, sizeof(name), &is_dir) &&
	      !strcmp(name, "A"), "after a remount, A is listed first");
}

int main(void)
{
	size_t len = (size_t)BLOCKS * 128u * 4096u;
	uint8_t *vol = calloc(1, len);

	if (vol == NULL)
		return 2;
	wb_ramdisk_init(&bd, &ram, vol, (uint32_t)(len / 512u));
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &bd, 0, BLOCKS - 1), "add");
	wire_yaffs_fs_ops(&Y, &fs);
	test_installs();
	test_pack_names();
	test_listing();
	wire_yaffs_detach(&Y);
	free(vol);
	return tl_done("test_wire_content");
}
