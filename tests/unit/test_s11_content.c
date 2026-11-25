/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11 content and the RAM disk. Checks S-11.034, S-11.039, S-11.040,
 * S-11.046, S-11.047, S-11.056, S-11.066, S-11.075, S-11.076, S-11.077,
 * S-11.078, S-11.079 (and SC-11-DLC's legs with a removal the volume fails).
 * The yaffs2 volume is an in-memory tree.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "waffle/boot/device.h"
#include "waffle/install/content.h"
#include "waffle/install/package.h"
#include "waffle/install/staging.h"

#include "s11_check.h"
#include "vectors/s11_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define B(r) (s11v_blob + (r).off)

/* ---- an in-memory tree ---- */

struct node {
	char path[S11_PATH_MAX];
	int dir;
	const uint8_t *data;
	size_t len;
};

struct tree {
	struct node n[96];
	size_t count;
	int move_fails, remove_fails;
	unsigned moves, failed_removes;
};

static struct tree T;

static struct node *find(const char *path)
{
	for (size_t i = 0; i < T.count; i++) {
		if (strcmp(T.n[i].path, path) == 0)
			return &T.n[i];
	}
	return NULL;
}

static void add_dirs(const char *path)
{
	char p[S11_PATH_MAX];
	char *s;

	snprintf(p, sizeof(p), "%s", path);
	while ((s = strrchr(p, '/')) != NULL) {
		*s = '\0';
		if (find(p) == NULL && T.count < N(T.n)) {
			strcpy(T.n[T.count].path, p);
			T.n[T.count].dir = 1;
			T.n[T.count].data = NULL;
			T.n[T.count++].len = 0;
		}
	}
}

static void add_file(const char *path, const uint8_t *data, size_t len)
{
	struct node *f = find(path);

	add_dirs(path);
	if (f == NULL && T.count < N(T.n))
		f = &T.n[T.count++];
	if (f == NULL)
		return;
	snprintf(f->path, sizeof(f->path), "%s", path);
	f->dir = 0;
	f->data = data;
	f->len = len;
}

static void load_set(int set)
{
	for (size_t i = 0; i < N(s11v_files); i++) {
		if (s11v_files[i].set == set)
			add_file(s11v_files[i].path, B(s11v_files[i].data),
				 s11v_files[i].data.len);
	}
}

static bool t_stat(void *ctx, const char *path, bool *is_dir, uint32_t *size)
{
	const struct node *f = find(path);

	(void)ctx;
	if (f == NULL)
		return false;
	*is_dir = f->dir != 0;
	*size = (uint32_t)f->len;
	return true;
}

static size_t t_read(void *ctx, const char *path, uint8_t *buf, size_t cap)
{
	const struct node *f = find(path);
	size_t n;

	(void)ctx;
	if (f == NULL || f->dir)
		return 0;
	n = f->len < cap ? f->len : cap;
	memcpy(buf, f->data, n);
	return n;
}

static int by_name(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* listing order: names in sorted order */
static bool t_list(void *ctx, const char *dir, size_t index, char *name,
		   size_t cap, bool *is_dir)
{
	const char *names[96];
	size_t k = 0, dl = strlen(dir);

	(void)ctx;
	for (size_t i = 0; i < T.count; i++) {
		const char *p = T.n[i].path;

		if (strncmp(p, dir, dl) == 0 && p[dl] == '/' &&
		    strchr(p + dl + 1, '/') == NULL)
			names[k++] = p;
	}
	if (index >= k)
		return false;
	qsort(names, k, sizeof(names[0]), by_name);
	snprintf(name, cap, "%s", names[index] + dl + 1);
	*is_dir = find(names[index])->dir != 0;
	return true;
}

static void drop(const char *path)
{
	size_t pl = strlen(path), j = 0;

	for (size_t i = 0; i < T.count; i++) {
		const char *p = T.n[i].path;

		if (strcmp(p, path) == 0 || (strncmp(p, path, pl) == 0 && p[pl] == '/'))
			continue;
		T.n[j++] = T.n[i];
	}
	T.count = j;
}

static bool t_remove(void *ctx, const char *path)
{
	(void)ctx;
	if (T.remove_fails) {
		T.failed_removes++;
		return false;
	}
	drop(path);
	return true;
}

static bool t_move(void *ctx, const char *from, const char *to)
{
	const struct node *f = find(from);
	const uint8_t *data;
	size_t len;

	(void)ctx;
	if (T.move_fails || f == NULL || f->dir)
		return false;
	data = f->data;
	len = f->len;
	drop(from);
	add_file(to, data, len);
	T.moves++;
	return true;
}

static bool t_copy(void *ctx, const char *from, const char *to)
{
	const struct node *f = find(from);

	(void)ctx;
	if (f == NULL || f->dir)
		return false;
	add_file(to, f->data, f->len);
	return true;
}

static const struct s11_fs_ops fs = {
	t_stat, t_read, t_list, t_remove, t_move, t_copy, NULL,
};

/* The tree outside upgrade/ holds exactly the files of `set`. */
static int tree_is(int set)
{
	size_t want = 0, have = 0;

	for (size_t i = 0; i < N(s11v_files); i++) {
		const struct node *f;

		if (s11v_files[i].set != set)
			continue;
		want++;
		f = find(s11v_files[i].path);
		if (f == NULL || f->dir || f->len != s11v_files[i].data.len ||
		    memcmp(f->data, B(s11v_files[i].data), f->len) != 0)
			return 0;
	}
	for (size_t i = 0; i < T.count; i++)
		have += !T.n[i].dir && strncmp(T.n[i].path, "upgrade/", 8) != 0 ?
			1u : 0u;
	return want == have;
}

/* S-11.066, S-11.075-S-11.079, S-11.056 */
static void check_installs(const struct wb_device *dev,
			   const struct s11v_install *cases, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		const struct s11v_install *v = &cases[i];
		struct s11_staged st;
		enum s11_result r;

		memset(&T, 0, sizeof(T));
		load_set(v->staged);
		load_set(v->before);
		s11_staged_scan(&fs, &st);
		CHECK(s11_staged_content_only(&st) == (v->content_only != 0),
		      "%s: content-only from the staging area", v->name);
		r = s11_install_content(dev, &fs, v->content_only != 0);
		CHECK(r == (enum s11_result)v->result, "%s %s: result %d", dev->name,
		      v->name, (int)r);
		CHECK(tree_is(v->after), "%s %s: the tree after", dev->name, v->name);
		if (v->result == S11_OK)
			CHECK(find("upgrade/firmware/blackfin/content") != NULL,
			      "%s: the content directory stays (S-11.056)", v->name);
	}
}

/* S-11.075 to S-11.079; on the OP-1F S-11.097 (content_1f.json) */
static void test_installs(void)
{
	check_installs(&wb_device_opxy, s11v_installs, N(s11v_installs));
	check_installs(&wb_device_op1f, s11v_installs_1f, N(s11v_installs_1f));
}

/* Every staged content file of `set` but the markers is at content/R. */
static int staged_installed(int set)
{
	static const char pre[] = "upgrade/firmware/blackfin/content/";

	for (size_t i = 0; i < N(s11v_files); i++) {
		const char *p = s11v_files[i].path;
		const struct node *f;
		char dst[S11_PATH_MAX];

		if (s11v_files[i].set != set || strncmp(p, pre, sizeof(pre) - 1) != 0 ||
		    strstr(p, "partial_upgrade") != NULL)
			continue;
		snprintf(dst, sizeof(dst), "content/%s", p + sizeof(pre) - 1);
		f = find(dst);
		if (f == NULL || f->dir || f->len != s11v_files[i].data.len ||
		    memcmp(f->data, B(s11v_files[i].data), f->len) != 0 || find(p) != NULL)
			return 0;
	}
	return 1;
}

/* S-11.075, S-11.077: a removal the volume fails does not stop the step */
static void test_removal_fails(void)
{
	static const char *const cases[] = { "full_keeps_dlc", "pack_replace_partial" };

	for (size_t k = 0; k < N(cases); k++) {
		const struct s11v_install *v = NULL;
		enum s11_result r;

		for (size_t i = 0; i < N(s11v_installs); i++) {
			if (strcmp(s11v_installs[i].name, cases[k]) == 0)
				v = &s11v_installs[i];
		}
		if (v == NULL) {
			CHECK(0, "%s not found", cases[k]);
			continue;
		}
		memset(&T, 0, sizeof(T));
		load_set(v->staged);
		load_set(v->before);
		T.remove_fails = 1;
		r = s11_install_content(&wb_device_opxy, &fs, v->content_only != 0);
		CHECK(T.failed_removes != 0, "%s: a removal was tried", v->name);
		CHECK(r == S11_OK && staged_installed(v->staged),
		      "%s: every staged file installed, OK (%d)", v->name, (int)r);
	}
}

/* steps.tsv's content rows (S-11.066); those not selected (S-11.096)
 * never reach s11_install_content(): test_s11_run */
static void test_content_rows(void)
{
	static const uint8_t one[1] = { '1' };

	for (size_t i = 0; i < N(s11v_steps); i++) {
		const struct s11v_step *v = &s11v_steps[i];
		enum s11_result r;

		if (v->step != 0 || !v->selected)
			continue;
		memset(&T, 0, sizeof(T));
		if (v->content_dir) {
			add_dirs("upgrade/firmware/blackfin/content/x");
			for (int k = 0; k < v->files; k++) {
				char p[64];

				snprintf(p, sizeof(p),
					 "upgrade/firmware/blackfin/content/f%d", k);
				add_file(p, one, 1);
			}
		}
		T.move_fails = v->move_fails;
		r = s11_install_content(&wb_device_opxy, &fs, false);
		CHECK(r == (enum s11_result)v->result, "content row %zu: %d", i, (int)r);
	}
}

/* S-11.079: a move that fails stops the step; nothing after it moves */
static void test_move_failure(void)
{
	static const uint8_t one[1] = { '1' };

	memset(&T, 0, sizeof(T));
	add_file("upgrade/firmware/blackfin/content/a", one, 1);
	add_file("upgrade/firmware/blackfin/content/b", one, 1);
	add_file("content/dlc/P/p1", one, 1);
	T.move_fails = 1;
	CHECK(s11_install_content(&wb_device_opxy, &fs, false) == S11_FAILURE && T.moves == 0,
	      "Failure, nothing moved");
	CHECK(find("content/dlc/P/p1") != NULL, "the DLC is untouched");
	CHECK(find("firmware.version") == NULL, "no version file after Failure");
}

/* S-11.039 */
static void test_pack_names(void)
{
	for (size_t i = 0; i < N(s11v_pack_names); i++) {
		const struct s11v_pack_name *v = &s11v_pack_names[i];
		char name[256];

		memset(&T, 0, sizeof(T));
		load_set(v->staged);
		s11_pack_name(&wb_device_opxy, &fs, name, sizeof(name));
		CHECK(strcmp(name, v->pack) == 0 ||
		      (v->alt != NULL && strcmp(name, v->alt) == 0),
		      "%s: '%s'", v->name, name);
	}
	/* S-11.097: no pack on the OP-1F, whatever is staged */
	for (size_t i = 0; i < N(s11v_pack_names_1f); i++) {
		const struct s11v_pack_name *v = &s11v_pack_names_1f[i];
		char name[256];

		memset(&T, 0, sizeof(T));
		load_set(v->staged);
		s11_pack_name(&wb_device_op1f, &fs, name, sizeof(name));
		CHECK(strcmp(name, v->pack) == 0, "OP-1F %s: '%s'", v->name, name);
	}
	for (size_t i = 0; i < N(s11v_pack_names); i++) {
		char name[256] = "x";

		memset(&T, 0, sizeof(T));
		load_set(s11v_pack_names[i].staged);
		s11_pack_name(&wb_device_op1f, &fs, name, sizeof(name));
		CHECK(name[0] == '\0', "OP-1F %s: no name", s11v_pack_names[i].name);
	}
	{
		static const char json[] = "{\"a\": {\"name\": \"x\"}, \"b\": [1, \"]\"],"
					   " \"name\": \"A\\u00e9\\\"\"}";
		char name[16];

		memset(&T, 0, sizeof(T));
		add_file("upgrade/firmware/blackfin/content/dlc/A/manifest.json",
			 (const uint8_t *)json, strlen(json));
		s11_pack_name(&wb_device_opxy, &fs, name, sizeof(name));
		CHECK(strcmp(name, "A\xc3\xa9\"") == 0, "nested values skipped: '%s'",
		      name);
		s11_pack_name(&wb_device_opxy, &fs, name, 3);
		CHECK(strcmp(name, "A\xc3") == 0, "cut to fit");
	}
}

/* S-11.046, S-11.047 */
/* The root listing being played (the callbacks reach the fakes' state
 * through file-scope variables, as the ops signatures fix their ctx). */
static const char *root_text;

static bool root_entry(void *ctx, size_t index, char *name, size_t cap,
		       bool *is_dir)
{
	const char *p = root_text;
	size_t n;

	(void)ctx;
	for (; index != 0 && p != NULL; index--) {
		p = strchr(p, '\n');
		if (p != NULL)
			p++;
	}
	if (p == NULL || *p == '\0')
		return false;
	*is_dir = *p == '/';
	p += *is_dir;
	n = strcspn(p, "\n");
	if (n >= cap)
		n = cap - 1;
	memcpy(name, p, n);
	name[n] = '\0';
	return true;
}

static void test_disk(void)
{
	for (size_t i = 0; i < N(s11v_disk); i++) {
		const struct s11v_disk *v = &s11v_disk[i];
		char name[S11_PATH_MAX];
		bool found;

		root_text = v->root;
		found = s11_disk_pick(root_entry, NULL, name);

		CHECK(found ? strcmp(name, v->package) == 0 : v->package[0] == '\0',
		      "%s: package", v->name);
		CHECK(!found || s11_name_forces(name) == (v->forced != 0),
		      "%s: forced by name", v->name);
	}
}

int main(void)
{
	test_installs();
	test_content_rows();
	test_move_failure();
	test_removal_fails();
	test_pack_names();
	test_disk();
	return s11_check_report("test_s11_content");
}
