/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09, the master's file client over the bridge's file and eMMC commands.
 * See s09_files.h.
 */
#include "waffle/s09_files.h"

#include <string.h>

#include "waffle/s11_run.h"

#define DLC "/yaffs2/content/dlc"
#define OS_LDR "/yaffs2/os.ldr"
#define MANIFEST "manifest.json"
#define MODE_FMT 0xF000u	/* S-09.043: the mode's type bits */
#define MODE_DIR 0x4000u
#define DT_DIR 4u		/* S-09.043: a dirent's directory type */
#define MKDIR_MODE 0755u	/* as wire_yaffs makes directories */
#define STAGING "upgrade"
#define RECORD "progress.log"	/* S11_RECORD_PATH under upgrade/ */
#define CMD38_ERASE 0u		/* S-09.040: every 0x14 carries 0 */
#define EXT_CSD_FLUSH_CACHE 32u	/* S-03.068 */

static size_t slen(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n] != '\0')
		n++;
	return n;
}

static int32_t call(struct s09_files *f, uint8_t cmd, uint32_t arg0, uint32_t arg1,
		    const uint8_t *payload, uint32_t plen, uint8_t *data,
		    uint32_t max, uint32_t *got)
{
	/* len is the payload length mod 2^16 (S-09.023, 0x13 and 0x16) */
	struct s09_req r = { cmd, 0, (uint16_t)plen, arg0, arg1 };
	uint32_t g;

	return s09_master_call(f->m, &r, payload, plen, data, max,
			       got != NULL ? got : &g, &f->err);
}

/* A request whose payload is a path (S-09.023's field rules). */
static int32_t path_call(struct s09_files *f, uint8_t cmd, uint32_t arg0,
			 const char *path, uint8_t *data, uint32_t max, uint32_t *got)
{
	size_t n = slen(path, S09_FILES_PATH_MAX);

	if (n >= S09_FILES_PATH_MAX) {
		f->err = S09_ENAMETOOLONG;
		return -1;
	}
	return call(f, cmd, arg0, 0, (const uint8_t *)path, (uint32_t)n + 1u,
		    data, max, got);
}

void s09_files_init(struct s09_files *f, struct s09_master *m)
{
	f->m = m;
	f->err = 0;
	f->mount_sent = false;
	f->mount_failed = false;
	f->mount_lost = false;
	f->mounted = false;
	f->path[0] = '\0';
	f->stage_fd = -1;
	f->stage_sent = 0;
	f->stage[0] = '\0';
	f->made_len = 0;
	f->link = 0;
}

/* ---- the requests ---- */

int32_t s09_files_mount(struct s09_files *f)
{
	int32_t r = call(f, S09_MOUNT, 0, 0, NULL, 0, NULL, 0, NULL);

	/* A negative result the slave sent (errno 0, S-09.041); a slave that
	 * is absent (ENODEV) has refused nothing. */
	f->mount_sent = true;
	f->mount_failed = r < 0 && f->err == 0;
	f->mount_lost = r < 0 && f->err == S09_ENODEV && f->m->absent;
	f->mounted = r >= 0;
	f->made_len = 0;
	return r;
}

bool s09_files_unmount(struct s09_files *f)
{
	int32_t r;

	f->made_len = 0;
	f->mounted = false;
	r = call(f, S09_UNMOUNT, 0, 0, NULL, 0, NULL, 0, NULL);
	return r >= 0 && f->err == 0;
}

int32_t s09_files_stat(struct s09_files *f, const char *path, struct s09_stat *st)
{
	uint32_t got;
	int32_t r = path_call(f, S09_STAT, 0, path, f->rec, S09_STAT_LEN, &got);

	if (r != 0 || f->err != 0)
		return -1;
	if (got != S09_STAT_LEN) {
		f->err = S09_EINVAL;
		return -1;
	}
	s09_stat_unpack(f->rec, st);
	return 0;
}

int32_t s09_files_statfs(struct s09_files *f, const char *path, struct s09_statfs *st)
{
	uint32_t got;
	int32_t r = path_call(f, S09_STATFS, 0, path, f->rec, S09_STATFS_LEN, &got);

	if (r != 0 || f->err != 0)
		return -1;
	if (got != S09_STATFS_LEN) {
		f->err = S09_EINVAL;
		return -1;
	}
	s09_statfs_unpack(f->rec, st);
	return 0;
}

int32_t s09_files_open(struct s09_files *f, const char *path, uint32_t flags)
{
	int32_t fd = path_call(f, S09_OPEN, flags, path, NULL, 0, NULL);

	return fd >= 0 && f->err == 0 ? fd : -1;
}

int32_t s09_files_close(struct s09_files *f, int32_t fd)
{
	return call(f, S09_CLOSE, (uint32_t)fd, 0, NULL, 0, NULL, 0, NULL);
}

/* 0x04: the eMMC's whole cache to its nonvolatile memory (S-03.068). */
static int32_t flush_emmc(struct s09_files *f)
{
	int32_t r = call(f, S09_FLUSH, 0, 0, NULL, 0, NULL, 0, NULL);

	return r == 0 && f->err == 0 ? 0 : -1;
}

int32_t s09_files_close_sync(struct s09_files *f, int32_t fd)
{
	if (s09_files_close(f, fd) != 0 || f->err != 0)
		return -1;
	return flush_emmc(f);
}

int32_t s09_files_read(struct s09_files *f, int32_t fd, uint8_t *buf, uint32_t n)
{
	f->err = 0;
	if (n == 0)
		return 0;
	return s09_master_read(f->m, (uint32_t)fd, buf, n, &f->err);
}

int32_t s09_files_write(struct s09_files *f, int32_t fd, const uint8_t *buf, uint32_t n)
{
	uint32_t done = 0;

	f->err = 0;
	while (done < n) {
		uint32_t k = n - done < S09_TUNNEL_MAX ? n - done : S09_TUNNEL_MAX;
		int32_t r = call(f, S09_WRITE, (uint32_t)fd, k, buf + done, k,
				 NULL, 0, NULL);

		if (r < 0 || (uint32_t)r != k) {
			if (f->err == 0)
				f->err = S09_ENOSPC;	/* written short */
			return -1;
		}
		done += k;
	}
	return (int32_t)n;
}

int32_t s09_files_unlink(struct s09_files *f, const char *path)
{
	return path_call(f, S09_UNLINK, 0, path, NULL, 0, NULL);
}

int32_t s09_files_mkdir(struct s09_files *f, const char *path)
{
	return path_call(f, S09_MKDIR, MKDIR_MODE, path, NULL, 0, NULL);
}

int32_t s09_files_rmdir(struct s09_files *f, const char *path)
{
	f->made_len = 0;
	return path_call(f, S09_RMDIR, 0, path, NULL, 0, NULL);
}

int32_t s09_files_rename(struct s09_files *f, const char *from, const char *to)
{
	size_t a = slen(from, S09_FILES_PATH_MAX), b = slen(to, S09_FILES_PATH_MAX);

	f->made_len = 0;
	if (a >= S09_FILES_PATH_MAX || b >= S09_FILES_PATH_MAX) {
		f->err = S09_ENAMETOOLONG;
		return -1;
	}
	/* S-09.023: old path and NUL, new path and NUL; arg1 the first's length */
	for (size_t i = 0; i <= a; i++)
		f->req[i] = (uint8_t)from[i];
	for (size_t i = 0; i <= b; i++)
		f->req[a + 1u + i] = (uint8_t)to[i];
	return call(f, S09_RENAME, 0, (uint32_t)a + 1u, f->req, (uint32_t)(a + b) + 2u,
		    NULL, 0, NULL);
}

/* Entry `index` of dir, counting directories only when dirs is set. */
static bool nth_entry(struct s09_files *f, const char *dir, size_t index, bool dirs,
		      char *name, size_t cap, uint8_t *type)
{
	struct s09_dirent d;
	uint32_t got, err;
	int32_t h;
	size_t k = 0;
	bool found = false;

	h = path_call(f, S09_OPENDIR, 0, dir, NULL, 0, NULL);
	if (h == 0 || f->err != 0)		/* 0: failed; -1, ENODEV: absent */
		return false;
	for (;;) {
		int32_t r = call(f, S09_READDIR, (uint32_t)h, 0, NULL, 0, f->rec,
				 S09_DIRENT_LEN, &got);
		if (r == 0 || f->err != 0)	/* the end (errno 0), or a failure */
			break;
		if (got != S09_DIRENT_LEN) {
			f->err = S09_EINVAL;
			break;
		}
		s09_dirent_unpack(f->rec, &d);
		if ((d.name[0] == '.' && d.name[1] == '\0') ||
		    (d.name[0] == '.' && d.name[1] == '.' && d.name[2] == '\0') ||
		    (dirs && d.type != DT_DIR))
			continue;
		if (k++ != index)
			continue;
		if (slen(d.name, S09_DIRENT_NAME) >= cap) {
			f->err = S09_ENAMETOOLONG;
			break;
		}
		for (size_t i = 0; i < cap; i++) {
			name[i] = d.name[i];
			if (d.name[i] == '\0')
				break;
		}
		*type = d.type;
		found = true;
		break;
	}
	err = f->err;
	(void)call(f, S09_CLOSEDIR, (uint32_t)h, 0, NULL, 0, NULL, 0, NULL);
	f->err = err;
	return found;
}

bool s09_files_entry(struct s09_files *f, const char *dir, size_t index,
		     char *name, size_t cap, uint8_t *type)
{
	return nth_entry(f, dir, index, false, name, cap, type);
}

/* ---- built on them ---- */

static bool is_dir(const struct s09_stat *st)
{
	return (st->mode & MODE_FMT) == MODE_DIR;
}

bool s09_files_dir_exists(struct s09_files *f, const char *path)
{
	struct s09_stat st;

	return s09_files_stat(f, path, &st) == 0 && is_dir(&st);
}

/* Copy src into f->path; false when it does not fit. */
static bool set_path(struct s09_files *f, const char *src)
{
	size_t n = slen(src, S09_FILES_PATH_MAX);

	if (n >= S09_FILES_PATH_MAX) {
		f->err = S09_ENAMETOOLONG;
		return false;
	}
	for (size_t i = 0; i <= n; i++)
		f->path[i] = src[i];
	return true;
}

/* Append "/" and name to f->path, of length n. */
static bool push(struct s09_files *f, size_t n, const char *name)
{
	size_t k = slen(name, S09_DIRENT_NAME);

	if (n + 1u + k >= S09_FILES_PATH_MAX) {
		f->err = S09_ENAMETOOLONG;
		return false;
	}
	f->path[n] = '/';
	for (size_t i = 0; i <= k; i++)
		f->path[n + 1u + i] = name[i];
	return true;
}

/* path's directories from its first `skip` bytes on: those name a
 * directory that is there, and path[skip] is the '/' after it. */
static bool mkdirs_from(struct s09_files *f, const char *path, size_t skip)
{
	size_t n;

	if (!set_path(f, path))
		return false;
	n = slen(f->path, S09_FILES_PATH_MAX);
	/* each prefix ending before a '/', the root's leading one left out */
	for (size_t i = skip + 1u; i < n; i++) {
		if (f->path[i] != '/')
			continue;
		f->path[i] = '\0';
		if (s09_files_mkdir(f, f->path) != 0 && !s09_files_dir_exists(f, f->path)) {
			f->path[i] = '/';
			return false;
		}
		f->path[i] = '/';
	}
	return true;
}

bool s09_files_mkdirs(struct s09_files *f, const char *path)
{
	return mkdirs_from(f, path, 0);
}

bool s09_files_remove_tree(struct s09_files *f, const char *path)
{
	struct s09_stat st;
	char name[S09_DIRENT_NAME];
	size_t root;
	uint8_t type;

	f->made_len = 0;
	if (!set_path(f, path))
		return false;
	if (s09_files_stat(f, f->path, &st) != 0)
		return f->err == S09_ENOENT;		/* missing: nothing to do */
	if (!is_dir(&st))
		return s09_files_unlink(f, f->path) == 0;
	/*
	 * f->path is always a directory here: go into its first entry, or,
	 * with none left, remove it and step back up. One path buffer, no
	 * recursion; every step removes something or descends.
	 */
	root = slen(f->path, S09_FILES_PATH_MAX);
	for (;;) {
		size_t n = slen(f->path, S09_FILES_PATH_MAX);

		if (s09_files_entry(f, f->path, 0, name, sizeof name, &type)) {
			if (!push(f, n, name))
				return false;
			if (type == DT_DIR)
				continue;
			if (s09_files_unlink(f, f->path) != 0)	/* a file or a link */
				return false;
			f->path[n] = '\0';
			continue;
		}
		if (f->err != 0 || s09_files_rmdir(f, f->path) != 0)
			return false;
		if (n <= root)
			return true;
		while (n > root && f->path[n] != '/')
			n--;
		f->path[n] = '\0';
	}
}

bool s09_files_copy(struct s09_files *f, const char *from, const char *to)
{
	int32_t in, out;
	bool ok = true;
	uint32_t err;

	if (!s09_files_mkdirs(f, to))
		return false;
	in = s09_files_open(f, from, S09_O_RDONLY);
	if (in < 0)
		return false;
	out = s09_files_open(f, to, S09_O_WRONLY | S09_O_CREAT | S09_O_TRUNC);
	if (out < 0) {
		err = f->err;
		(void)s09_files_close(f, in);
		f->err = err;
		return false;
	}
	for (;;) {
		int32_t r = s09_files_read(f, in, f->buf, S09_READ_CHUNK);

		if (r < 0 || (r > 0 && s09_files_write(f, out, f->buf, (uint32_t)r) < 0)) {
			ok = false;
			break;
		}
		if ((uint32_t)r < S09_READ_CHUNK)
			break;				/* the end of the file */
	}
	err = f->err;
	if (s09_files_close(f, out) != 0 && ok) {
		ok = false;		/* the last write may fail at the close */
		err = f->err;
	}
	(void)s09_files_close(f, in);
	f->err = ok ? 0u : err;
	return ok;
}

/* A rename replaces a file at `to` (S-12, yaffs2's rename). */
bool s09_files_move(struct s09_files *f, const char *from, const char *to)
{
	return s09_files_mkdirs(f, to) && s09_files_rename(f, from, to) == 0;
}

size_t s09_files_read_file(struct s09_files *f, const char *path, uint8_t *buf, size_t cap)
{
	uint32_t n = cap > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)cap, err;
	int32_t fd, r;

	fd = s09_files_open(f, path, S09_O_RDONLY);
	if (fd < 0)
		return 0;
	r = s09_files_read(f, fd, buf, n);
	err = f->err;
	(void)s09_files_close(f, fd);
	f->err = err;
	return r > 0 ? (size_t)r : 0u;
}

bool s09_files_read_whole(struct s09_files *f, const char *path, uint8_t *buf,
			  uint32_t cap, uint32_t *len)
{
	struct s09_stat st;
	size_t got;

	*len = 0;
	if (s09_files_stat(f, path, &st) != 0)
		return false;
	if (is_dir(&st)) {
		f->err = S09_EISDIR;
		return false;
	}
	if (st.size > cap) {
		f->err = S09_ERANGE;
		return false;
	}
	got = s09_files_read_file(f, path, buf, st.size);
	if (got != st.size || f->err != 0) {
		if (f->err == 0)
			f->err = S09_ENODATA;	/* shorter than its size */
		return false;
	}
	*len = st.size;
	return true;
}

/* ---- the staging area (S-11.080) ---- */

static bool fs_stat(void *ctx, const char *path, bool *dir, uint32_t *size)
{
	struct s09_stat st;

	if (s09_files_stat(ctx, path, &st) != 0)
		return false;
	*dir = is_dir(&st);
	*size = *dir ? 0u : st.size;
	return true;
}

static size_t fs_read(void *ctx, const char *path, uint8_t *buf, size_t cap)
{
	return s09_files_read_file(ctx, path, buf, cap);
}

static bool fs_list(void *ctx, const char *dir, size_t index, char *name, size_t cap,
		    bool *is_d)
{
	uint8_t type;

	if (!s09_files_entry(ctx, dir, index, name, cap, &type))
		return false;
	*is_d = type == DT_DIR;
	return true;
}

static bool fs_remove(void *ctx, const char *path)
{
	return s09_files_remove_tree(ctx, path);
}

static bool fs_move(void *ctx, const char *from, const char *to)
{
	return s09_files_move(ctx, from, to);
}

static bool fs_copy(void *ctx, const char *from, const char *to)
{
	return s09_files_copy(ctx, from, to);
}

void s09_files_fs_ops(struct s09_files *f, struct s11_fs_ops *ops)
{
	ops->stat = fs_stat;
	ops->read = fs_read;
	ops->list = fs_list;
	ops->remove = fs_remove;
	ops->move = fs_move;
	ops->copy = fs_copy;
	ops->ctx = f;
}

/* ---- S-11's staging writes ---- */

bool s09_files_free_space(struct s09_files *f, uint64_t *bytes)
{
	struct s09_statfs st;

	if (s09_files_statfs(f, "/yaffs2", &st) != 0)
		return false;
	*bytes = (uint64_t)st.bfree * st.bsize;
	return true;
}

/* upgrade/<rel> into f->stage, a '/' after it when dir and rel lacks one;
 * f->stage is apart from f->path, which mkdirs and remove_tree use. */
static bool stage_path(struct s09_files *f, const char *rel, bool dir)
{
	size_t n = sizeof STAGING - 1u, k = slen(rel, S09_FILES_PATH_MAX);
	bool slash = dir && (k == 0 || rel[k - 1u] != '/');

	if (n + 1u + k + (slash ? 1u : 0u) >= S09_FILES_PATH_MAX) {
		f->err = S09_ENAMETOOLONG;
		return false;
	}
	for (size_t i = 0; i < n; i++)
		f->stage[i] = STAGING[i];
	f->stage[n++] = '/';
	for (size_t i = 0; i < k; i++)
		f->stage[n++] = rel[i];
	if (slash)
		f->stage[n++] = '/';
	f->stage[n] = '\0';
	return true;
}

/* How much of f->stage names directories f->made holds: up to a '/' of
 * f->stage at which f->made ends or has a '/' too. */
static size_t stage_made(const struct s09_files *f)
{
	size_t skip = 0;

	for (size_t i = 0; i <= f->made_len && f->stage[i] != '\0'; i++) {
		if (f->stage[i] == '/' && (i == f->made_len || f->made[i] == '/'))
			skip = i;
		if (i == f->made_len || f->stage[i] != f->made[i])
			break;
	}
	return skip;
}

/* The directories of f->stage, those made before left out (skip); they
 * are remembered for the next path. */
static bool stage_mkdirs(struct s09_files *f, size_t skip)
{
	size_t last = 0;

	if (!mkdirs_from(f, f->stage, skip)) {
		f->made_len = 0;
		return false;
	}
	for (size_t i = 0; f->stage[i] != '\0'; i++) {
		if (f->stage[i] == '/')
			last = i;
	}
	for (size_t i = 0; i < last; i++)
		f->made[i] = f->stage[i];
	f->made_len = last;
	return true;
}

bool s09_files_staging_reset(struct s09_files *f)
{
	return s09_files_remove_tree(f, STAGING) && s09_files_mkdir(f, STAGING) == 0;
}

/* mkdirs makes each directory before a '/': with the '/' stage_path adds,
 * the last one too. */
bool s09_files_staging_mkdir(struct s09_files *f, const char *path)
{
	return stage_path(f, path, true) && stage_mkdirs(f, stage_made(f));
}

bool s09_files_staging_open(struct s09_files *f, const char *path)
{
	const uint32_t flags = S09_O_WRONLY | S09_O_CREAT | S09_O_TRUNC;
	size_t skip;

	if (f->stage_fd >= 0 || !stage_path(f, path, false))
		return false;
	/* nothing when the link is on its rung, or has none left to try */
	if (f->link != 0u)
		(void)s09_master_link_fast(f->m, f->link);
	skip = stage_made(f);
	if (!stage_mkdirs(f, skip))
		return false;
	f->stage_fd = s09_files_open(f, f->stage, flags);
	/* a remembered directory that is not there after all: made again */
	if (f->stage_fd < 0 && skip != 0u && f->err == S09_ENOENT && stage_mkdirs(f, 0))
		f->stage_fd = s09_files_open(f, f->stage, flags);
	return f->stage_fd >= 0;
}

/* The staging write left running, collected: false when it did not write
 * all its bytes. */
static bool stage_settle(struct s09_files *f)
{
	const uint32_t k = f->stage_sent;
	int32_t r;

	if (k == 0u)
		return true;
	f->stage_sent = 0;
	r = s09_master_write_end(f->m, &f->err);
	if (r < 0 || (uint32_t)r != k) {
		if (f->err == 0)
			f->err = S09_ENOSPC;	/* written short */
		return false;
	}
	return true;
}

/*
 * Each piece's request is left running (s09_master_write_begin()): the
 * caller prepares the next bytes while these go out and the slave writes
 * them. A piece's failure shows at the next write or at the close.
 */
bool s09_files_staging_write(struct s09_files *f, const uint8_t *p, size_t n)
{
	while (n != 0) {
		uint32_t max, k;

		if (f->stage_fd < 0 || !stage_settle(f))
			return false;
		/* S-09.023: as much as the link's rung takes in one write */
		max = s09_master_write_max(f->m);
		k = n > max ? max : (uint32_t)n;
		f->err = 0;
		if (!s09_master_write_begin(f->m, (uint32_t)f->stage_fd, p, k, &f->err))
			return false;
		f->stage_sent = k;
		p += k;
		n -= k;
	}
	return f->stage_fd >= 0;
}

/* The last write may fail at the close. */
bool s09_files_staging_close(struct s09_files *f)
{
	int32_t fd = f->stage_fd;
	const bool wrote = stage_settle(f);
	uint32_t err = f->err;

	f->stage_fd = -1;
	if (fd < 0)
		return false;
	if (s09_files_close(f, fd) != 0)
		return false;
	if (!wrote)
		f->err = err;
	return wrote;
}

bool s09_files_record_write(struct s09_files *f, uint8_t step)
{
	uint8_t rec[2];
	bool ok;

	s11_record_bytes(step, rec);
	if (!s09_files_staging_open(f, RECORD))
		return false;
	ok = s09_files_staging_write(f, rec, sizeof rec);
	return s09_files_staging_close(f) && ok;
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_member(void *ctx, size_t index, struct s11_member *m)
{
	const struct s09_files_zip *z = ctx;

	return z->reader.member(z->reader.ctx, index, m);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_zip64_end(void *ctx)
{
	const struct s09_files_zip *z = ctx;

	return z->reader.zip64_end(z->reader.ctx);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_extract(void *ctx, size_t index, s11_sink_fn sink, void *arg)
{
	const struct s09_files_zip *z = ctx;

	return z->reader.extract(z->reader.ctx, index, sink, arg);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static uint32_t zip_crc32(void *ctx, uint32_t crc, const uint8_t *p, size_t n)
{
	const struct s09_files_zip *z = ctx;

	return z->reader.crc32(z->reader.ctx, crc, p, n);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_free_space(void *ctx, uint64_t *bytes)
{
	const struct s09_files_zip *z = ctx;

	return s09_files_free_space(z->f, bytes);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_reset(void *ctx)
{
	const struct s09_files_zip *z = ctx;

	return s09_files_staging_reset(z->f);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_mkdir(void *ctx, const char *path)
{
	const struct s09_files_zip *z = ctx;

	return s09_files_staging_mkdir(z->f, path);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_open(void *ctx, const char *path)
{
	const struct s09_files_zip *z = ctx;

	return s09_files_staging_open(z->f, path);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_write(void *ctx, const uint8_t *p, size_t n)
{
	const struct s09_files_zip *z = ctx;

	return s09_files_staging_write(z->f, p, n);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_close(void *ctx)
{
	const struct s09_files_zip *z = ctx;

	return s09_files_staging_close(z->f);
}

void s09_files_zip_ops(struct s09_files_zip *z, struct s09_files *f,
		       const struct s11_zip_ops *reader, struct s11_zip_ops *ops)
{
	z->f = f;
	z->reader = *reader;
	ops->member = zip_member;
	ops->zip64_end = zip_zip64_end;
	ops->extract = zip_extract;
	ops->crc32 = zip_crc32;
	ops->free_space = zip_free_space;
	ops->staging_reset = zip_reset;
	ops->staging_mkdir = zip_mkdir;
	ops->staging_open = zip_open;
	ops->staging_write = zip_write;
	ops->staging_close = zip_close;
	ops->ctx = z;
}

/* ---- S-15 ---- */

bool s09_files_volume_refused(const struct s09_files *f)
{
	/* S-15's Terms: refused when the last 0x08 reply was negative,
	 * whatever the cause (S-12.052 or any other failed mount). */
	return f->mount_sent && f->mount_failed;
}

bool s09_files_slave_lost(const struct s09_files *f)
{
	return f->m->absent;
}

bool s09_files_pack_dir(struct s09_files *f, size_t index, char name[S15_NAME_MAX + 1u])
{
	uint8_t type;

	return nth_entry(f, DLC, index, true, name, S15_NAME_MAX + 1u, &type);
}

size_t s09_files_read_manifest(struct s09_files *f, const char *dir, uint8_t *buf, size_t max)
{
	size_t n;

	if (!set_path(f, DLC) || !push(f, sizeof DLC - 1u, dir))
		return 0;
	n = slen(f->path, S09_FILES_PATH_MAX);
	if (!push(f, n, MANIFEST))
		return 0;
	return s09_files_read_file(f, f->path, buf, max);
}

static int bridge_select(void *ctx, unsigned part)
{
	struct s09_files *f = ctx;
	int32_t r = call(f, S09_SELECT, part, 0, NULL, 0, NULL, 0, NULL);

	return r == 0 && f->err == 0 ? 0 : -1;
}

static int bridge_erase(void *ctx, uint32_t first_sector)
{
	struct s09_files *f = ctx;
	int32_t r = call(f, S09_ERASE, first_sector, CMD38_ERASE, NULL, 0, NULL, 0, NULL);

	return r == 0 && f->err == 0 ? 0 : -1;
}

bool s09_files_erase_os_fat(struct s09_files *f)
{
	/* S-03.073: the group at S03_FACTORY_RESET_GROUP of the user area */
	return bridge_select(f, S03_PART_USER) == 0 &&
	       bridge_erase(f, S03_FACTORY_RESET_GROUP * S03_GROUP_SECTORS) == 0;
}

bool s09_files_format(struct s09_files *f)
{
	bool ok;

	/*
	 * S-12.057: every block in order; a failed select erases nothing, and
	 * an erase that fails (after S-03's retries, S-16.075) does not stop
	 * the rest. Either is a failed format. The volume is mounted again
	 * after every format, and that mount alone sets the refused state
	 * (S-15.072).
	 */
	(void)s09_files_unmount(f);
	ok = bridge_select(f, S03_PART_USER) == 0;
	if (ok)
		for (uint32_t g = 0; g < S09_FILES_YAFFS2_GROUPS; g++)
			if (bridge_erase(f, S03_YAFFS2_FIRST +
					      g * S03_GROUP_SECTORS) != 0)
				ok = false;
	(void)s09_files_mount(f);
	return ok;
}

/* ---- S-15.094: the OS write, reported as it goes ---- */

#define OSW_PIECE 0x10000u	/* bytes read between two progress reports */

/* S-03's stages reported as S-15.094's steps. */
struct osw_watch {
	const struct s15_osw_report *rep;
	uint32_t needed, b_sectors;	/* S-03.051's N + 1 and B, in sectors */
	int step;			/* the step whose row is open, or -1 */
};

static void osw_step(const struct s15_osw_report *rep, enum s15_osw_step step)
{
	if (rep != NULL && rep->step != NULL)
		rep->step(rep->ctx, step);
}

static void osw_progress(const struct s15_osw_report *rep, uint32_t done, uint32_t total)
{
	if (rep != NULL && rep->progress != NULL)
		rep->progress(rep->ctx, done, total);
}

static void osw_done(const struct s15_osw_report *rep, uint32_t a, uint32_t b)
{
	if (rep != NULL && rep->done != NULL)
		rep->done(rep->ctx, a, b);
}

static void osw_stage(void *ctx, enum s03_write_stage stage, const struct s03_write *w)
{
	static const uint8_t step_of[] = {
		[S03_WS_ACCEPT] = S15_OSW_STEP_CHECK,
		[S03_WS_TARGET] = S15_OSW_STEP_TARGET,
		[S03_WS_ERASE] = S15_OSW_STEP_ERASE,
		[S03_WS_WRITE] = S15_OSW_STEP_WRITE,
		[S03_WS_VERIFY] = S15_OSW_STEP_VERIFY,
		[S03_WS_HEADER] = S15_OSW_STEP_ACTIVATE,
		[S03_WS_DEACTIVATE] = S15_OSW_STEP_ACTIVATE,
		[S03_WS_FLUSH] = S15_OSW_STEP_ACTIVATE,
		[S03_WS_DONE] = S15_OSW_STEP_COUNT,
	};
	struct osw_watch *o = ctx;
	int next = (unsigned)stage < sizeof step_of ? step_of[stage] : S15_OSW_STEP_COUNT;

	if (next == o->step)
		return;		/* step 8 is still Activate's */
	if (o->step == (int)S15_OSW_STEP_CHECK)
		osw_done(o->rep, o->needed, o->b_sectors);
	else if (o->step == (int)S15_OSW_STEP_TARGET || o->step == (int)S15_OSW_STEP_ACTIVATE)
		osw_done(o->rep, w->target, 0);
	else if (o->step >= 0)
		osw_done(o->rep, 0, 0);
	o->step = next;
	if (next < (int)S15_OSW_STEP_COUNT)
		osw_step(o->rep, (enum s15_osw_step)next);
}

static void osw_emmc_progress(void *ctx, uint32_t done, uint32_t total)
{
	const struct osw_watch *o = ctx;

	osw_progress(o->rep, done, total);
}

/* The read failed: end->why from f->err. */
static enum s15_os_write osw_unread(const struct s09_files *f, struct s15_osw_end *end)
{
	end->result = S15_OSW_UNREADABLE;
	end->why = f->err == S09_ENOENT ? S15_OSW_WHY_MISSING : S15_OSW_WHY_READ;
	end->value = f->err;
	return end->result;
}

/* /yaffs2/os.ldr, of size bytes, into image in pieces, each reported. */
static bool osw_read(struct s09_files *f, uint8_t *image, uint32_t size,
		     const struct s15_osw_report *rep)
{
	uint32_t got = 0, err;
	int32_t fd = s09_files_open(f, OS_LDR, S09_O_RDONLY);

	if (fd < 0)
		return false;
	while (got < size) {
		uint32_t k = size - got < OSW_PIECE ? size - got : OSW_PIECE;
		int32_t r = s09_files_read(f, fd, image + got, k);

		if (r <= 0 || f->err != 0)
			break;
		got += (uint32_t)r;
		osw_progress(rep, got, size);
		if ((uint32_t)r < k)
			break;			/* shorter than its size */
	}
	err = f->err;
	(void)s09_files_close(f, fd);
	f->err = err != 0u ? err : got != size ? S09_ENODATA : 0u;
	return f->err == 0u;
}

/* S-03.051's refusal, by the rule it failed. */
static void osw_refused(uint32_t len, uint32_t boot_size, struct s15_osw_end *end)
{
	if (len + 512u < 10000u) {
		end->why = S15_OSW_WHY_TOO_SMALL;
		end->limit = S03_MIN_IMAGE;
	} else if (len > S03_OS_MAX) {
		end->why = S15_OSW_WHY_TOO_LARGE;
		end->limit = S03_OS_MAX;
	} else {
		end->why = S15_OSW_WHY_NO_ROOM;
		end->limit = boot_size / S03_SECTOR;
		len = s03_sectors(len) + 1u;
	}
	end->value = len;
}

/* What a failed S-03 write left on the eMMC (S-03.056 - S-03.062, S-03.068). */
static enum s15_osw_emmc osw_emmc(const struct s03_write *w)
{
	switch (w->stage) {
	case S03_WS_ERASE:
	case S03_WS_WRITE:
	case S03_WS_VERIFY:
		return S15_OSW_EMMC_UNCHANGED;
	case S03_WS_HEADER:
		return w->recovery_failed ? S15_OSW_EMMC_BAD_HEADER : S15_OSW_EMMC_UNCHANGED;
	case S03_WS_DEACTIVATE:
		return S15_OSW_EMMC_BOTH_ACTIVE;
	case S03_WS_FLUSH:
		return S15_OSW_EMMC_UNCONFIRMED;
	case S03_WS_ACCEPT:
	case S03_WS_TARGET:
	case S03_WS_DONE:
	default:
		return S15_OSW_EMMC_UNTOUCHED;
	}
}

enum s15_os_write s09_files_os_write(struct s09_files *f, const struct s03_env *env,
				     uint8_t *image, uint32_t cap,
				     const struct s15_osw_report *rep,
				     struct s15_osw_end *end)
{
	static const enum s15_osw_why why_of[] = {
		[S03_FAIL_NONE] = S15_OSW_WHY_NONE,
		[S03_FAIL_LENGTH] = S15_OSW_WHY_NONE,
		[S03_FAIL_SELECT] = S15_OSW_WHY_SELECT,
		[S03_FAIL_READ] = S15_OSW_WHY_EMMC_READ,
		[S03_FAIL_WRITE] = S15_OSW_WHY_EMMC_WRITE,
		[S03_FAIL_ERASE] = S15_OSW_WHY_ERASE,
		[S03_FAIL_MISMATCH] = S15_OSW_WHY_MISMATCH,
		[S03_FAIL_FLUSH] = S15_OSW_WHY_FLUSH,
	};
	struct osw_watch o = { rep, 0, env->boot_size / S03_SECTOR, -1 };
	const struct s03_write_watch watch = { &o, osw_stage, osw_emmc_progress };
	struct s09_stat st;
	struct s03_write w;

	end->result = S15_OSW_FAILED;
	end->why = S15_OSW_WHY_NONE;
	end->emmc = S15_OSW_EMMC_UNTOUCHED;
	end->part = end->target = 0;
	end->value = end->limit = 0;
	end->select0_failed = false;

	osw_step(rep, S15_OSW_STEP_READ);
	/* S-12.052: what a refused volume holds is not read. */
	if (s09_files_volume_refused(f)) {
		end->result = S15_OSW_UNREADABLE;
		end->why = S15_OSW_WHY_REFUSED;
		return end->result;
	}
	if (s09_files_stat(f, OS_LDR, &st) != 0)
		return osw_unread(f, end);
	if (is_dir(&st)) {
		end->result = S15_OSW_UNREADABLE;
		end->why = S15_OSW_WHY_DIRECTORY;
		return end->result;
	}
	/* With cap at least S03_OS_MAX, S-03.051 refuses a longer file;
	 * S-15.094 adds no size check of its own. */
	if (st.size > cap) {
		end->why = S15_OSW_WHY_TOO_LARGE;
		end->value = st.size;
		end->limit = cap;
		return end->result;
	}
	if (!osw_read(f, image, st.size, rep))
		return osw_unread(f, end);
	osw_done(rep, st.size, 0);

	o.needed = s03_sectors(st.size) + 1u;
	s03_write_os_watched(env, image, st.size, &w, &watch);
	end->target = w.target;
	end->select0_failed = w.select0_failed;
	if (w.result == S03_WRITE_OK) {
		end->result = S15_OSW_OK;
		return end->result;
	}
	if (w.fail == S03_FAIL_LENGTH) {
		osw_refused(st.size, env->boot_size, end);
		return end->result;
	}
	end->why = (unsigned)w.fail < sizeof why_of / sizeof why_of[0] ?
		   why_of[w.fail] : S15_OSW_WHY_NONE;
	end->part = w.part;
	end->value = w.at;
	end->emmc = osw_emmc(&w);
	return end->result;
}

/* ---- the eMMC ---- */

static int bridge_read(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	struct s09_files *f = ctx;

	while (count > 0) {
		uint32_t k = count < S09_MAX_SECTORS ? count : S09_MAX_SECTORS, got;
		int32_t r = k == 1u ?
			call(f, S09_READ_SECTOR, sector, 0, NULL, 0, buf, S09_SECTOR, &got) :
			call(f, S09_READ_SECTORS, sector, k, NULL, 0, buf, k * S09_SECTOR, &got);

		if (r != 0 || f->err != 0 || got != k * S09_SECTOR)
			return -1;
		sector += k;
		count -= k;
		buf += k * S09_SECTOR;
	}
	return 0;
}

static int bridge_write(void *ctx, uint32_t sector, uint32_t count, const uint8_t *buf)
{
	struct s09_files *f = ctx;

	while (count > 0) {
		uint32_t k = count < S09_MAX_SECTORS ? count : S09_MAX_SECTORS;
		int32_t r = k == 1u ?
			call(f, S09_WRITE_SECTOR, sector, 0, buf, S09_SECTOR, NULL, 0, NULL) :
			call(f, S09_WRITE_SECTORS, sector, k, buf, k * S09_SECTOR, NULL, 0, NULL);

		if (r != 0 || f->err != 0)
			return -1;
		sector += k;
		count -= k;
		buf += k * S09_SECTOR;
	}
	return 0;
}

/* The flush is the one EXT_CSD byte a bridge request sets (S-09.040, S-03.068). */
static int bridge_switch(void *ctx, uint8_t index, uint8_t value)
{
	struct s09_files *f = ctx;

	if (index != EXT_CSD_FLUSH_CACHE || value != 1u)
		return -1;
	return flush_emmc(f);
}

void s09_files_emmc_ops(struct s09_files *f, struct s03_emmc_ops *ops)
{
	ops->ctx = f;
	ops->select = bridge_select;
	ops->read = bridge_read;
	ops->write = bridge_write;
	ops->erase = bridge_erase;
	ops->switch_byte = bridge_switch;
}

uint32_t s09_files_boot_size(struct s09_files *f)
{
	int32_t r = call(f, S09_BOOT_SIZE, 0, 0, NULL, 0, NULL, 0, NULL);

	return f->err == 0 ? (uint32_t)r : 0u;
}

/* ---- the settings file (S-15.172) ---- */

void s09_files_settings_read(struct s09_files *f, struct s15_settings *s)
{
	static uint8_t buf[S15_SETTINGS_MAX];
	uint32_t n = 0;

	if (s09_files_volume_refused(f) ||
	    !s09_files_read_whole(f, S15_SETTINGS_PATH, buf, sizeof buf, &n))
		n = 0;
	(void)s15_settings_read(buf, n, s);
}

bool s09_files_settings_save(struct s09_files *f, const struct s15_settings *s)
{
	uint8_t buf[S15_SETTINGS_OUT_MAX];
	size_t n = s15_settings_write(s, buf, sizeof buf);
	int32_t fd, w, c;

	if (n == 0u)
		return false;
	fd = s09_files_open(f, S15_SETTINGS_NEW_PATH,
			    S09_O_WRONLY | S09_O_CREAT | S09_O_TRUNC);
	if (fd < 0)
		return false;
	w = s09_files_write(f, fd, buf, (uint32_t)n);
	c = s09_files_close(f, fd);
	if (w != (int32_t)n || c != 0)
		return false;
	return s09_files_rename(f, S15_SETTINGS_NEW_PATH, S15_SETTINGS_PATH) == 0;
}
