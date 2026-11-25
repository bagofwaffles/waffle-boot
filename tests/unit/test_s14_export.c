/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-14 log export against tests/oracle/S-14-logging/vectors/export.json.
 * Requirements: S-14.050, S-14.051, S-14.052, S-16.082, S-16.162.
 */
#include <stdbool.h>
#include <stdint.h>

#include "waffle/log/export.h"
#include "s14_check.h"
#define S14V_EXPORT
#include "vectors/s14_vectors.h"

#define MAX_LOG 32

/* The yaffs2 logs/ directory and the USB disk's logs/, as a fake. */
struct fake_fs {
	const struct s14v_entry *entries;
	size_t n;
	size_t pos;
	bool reversed;          /* list in the other directory order */
	const char *copied[MAX_LOG];
	size_t n_copied;
	const char *on_disk[MAX_LOG];  /* files on the USB disk */
	size_t n_disk;
	unsigned discards;
	bool partial;           /* a failing copy leaves part of the file */
};

static void fs_rewind(void *ctx)
{
	((struct fake_fs *)ctx)->pos = 0;
}

static bool fs_next(void *ctx, struct s14_export_entry *e)
{
	struct fake_fs *fs = ctx;
	const struct s14v_entry *v;

	if (fs->pos >= fs->n)
		return false;
	v = &fs->entries[fs->reversed ? fs->n - 1u - fs->pos : fs->pos];
	fs->pos++;
	e->name = v->name;
	e->regular = v->regular;
	e->mtime = v->mtime;
	e->size = v->size;
	return true;
}

static const struct s14v_entry *find(const struct fake_fs *fs, const char *name)
{
	size_t i;

	for (i = 0; i < fs->n; i++)
		if (strcmp(fs->entries[i].name, name) == 0)
			return &fs->entries[i];
	return NULL;
}

static bool fs_copy(void *ctx, const char *name, uint32_t mtime)
{
	struct fake_fs *fs = ctx;
	const struct s14v_entry *v = find(fs, name);

	if (v == NULL || v->mtime != mtime || fs->n_disk == MAX_LOG)
		return false;
	if (v->fails) {
		if (fs->partial)
			fs->on_disk[fs->n_disk++] = v->name;
		return false;
	}
	fs->on_disk[fs->n_disk++] = v->name;
	fs->copied[fs->n_copied++] = v->name;
	return true;
}

static void fs_discard(void *ctx, const char *name)
{
	struct fake_fs *fs = ctx;
	size_t i, j = 0;

	fs->discards++;
	for (i = 0; i < fs->n_disk; i++)
		if (strcmp(fs->on_disk[i], name) != 0)
			fs->on_disk[j++] = fs->on_disk[i];
	fs->n_disk = j;
}

static void run(const struct s14v_export *g, bool reversed, bool partial)
{
	struct fake_fs fs;
	struct s14_export_ops ops = { fs_rewind, fs_next, fs_copy, fs_discard,
				      &fs };
	size_t i, n;

	memset(&fs, 0, sizeof fs);
	fs.entries = g->entries;
	fs.n = g->n_entries;
	fs.reversed = reversed;
	fs.partial = partial;
	n = s14_export_run(&ops);

	CHECK(n == g->n_copied && fs.n_copied == g->n_copied,
	      "S-14.051 %s (order %d): %zu copied, want %zu", g->note,
	      reversed, fs.n_copied, g->n_copied);
	for (i = 0; i < g->n_copied && i < fs.n_copied; i++)
		CHECK(strcmp(fs.copied[i], g->copied[i]) == 0,
		      "S-14.051 %s: copy %zu is %s, want %s", g->note, i,
		      fs.copied[i], g->copied[i]);
	/* S-14.052: the disk holds exactly the copied files, no partial one. */
	CHECK(fs.n_disk == g->n_copied, "S-14.052 %s: %zu files on disk",
	      g->note, fs.n_disk);
}

static void test_vectors(void)
{
	size_t i;

	CHECK(S14V_EXPORT_LIMIT == S14_EXPORT_LIMIT, "S-14.051 limit");
	for (i = 0; i < S14V_N(s14v_exports); i++) {
		/* The result does not depend on the directory order. */
		run(&s14v_exports[i], false, false);
		run(&s14v_exports[i], true, false);
		/* A copy that fails part-way leaves nothing (S-14.052). */
		run(&s14v_exports[i], false, true);
	}
}

/* No logs/ directory: nothing is copied (S-14.051). */
static void test_no_directory(void)
{
	static const struct s14v_export g = { "no logs/", NULL, 0, NULL, 0 };

	run(&g, false, false);
}

/* S-16.082: "." and ".." only are skipped; ".a" and "..x" are candidates.
 * A name longer than yaffs2's limit is not one. */
static void test_dot_names(void)
{
	static char longname[S14_EXPORT_NAME_MAX + 2u];
	static struct s14v_entry e[] = {
		{ ".", true, 9, 1, false },
		{ "..", true, 9, 1, false },
		{ "..x", true, 3, 1, false },
		{ ".a", true, 4, 1, false },
		{ NULL, true, 5, 1, false },
	};
	static const char *const want[] = { ".a", "..x" };
	struct s14v_export g = { "dot names", e, 5, want, 2 };

	memset(longname, 'n', sizeof longname - 1u);
	e[4].name = longname;
	run(&g, false, false);
}

int main(void)
{
	test_vectors();
	test_no_directory();
	test_dot_names();
	return check_done("test_s14_export");
}
