/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10b's store over the bridge's file commands (waffle/mtp/volume.h).
 */
#include "waffle/mtp/volume.h"

#include <string.h>

#define MODE_FMT 0xF000u	/* S-09.043 */
#define MODE_DIR 0x4000u
#define MODE_REG 0x8000u
#define DT_DIR 4u		/* S-09.043: a dirent's directory type */
#define MKDIR_MODE 0755u
/* A bound on a folder delete's steps, each of which removes an object or
 * goes into a folder, so a slave that never empties one cannot hold the
 * engine for ever. */
#define REMOVE_STEPS 4000000u

static int32_t call(struct s10b_store *st, uint8_t cmd, uint32_t arg0,
		    uint32_t arg1, const uint8_t *payload, uint32_t plen,
		    uint8_t *data, uint32_t max, uint32_t *got)
{
	struct s09_req r = { cmd, 0, (uint16_t)plen, arg0, arg1 };
	uint32_t g;

	return s09_master_call(st->f->m, &r, payload, plen, data, max,
			       got != NULL ? got : &g, &st->err);
}

static int32_t path_call(struct s10b_store *st, uint8_t cmd, uint32_t arg0,
			 uint32_t arg1, const char *path, uint8_t *data,
			 uint32_t max, uint32_t *got)
{
	return call(st, cmd, arg0, arg1, (const uint8_t *)path,
		    (uint32_t)strlen(path) + 1u, data, max, got);
}

/* The engine's "/" or "/a/b" as the volume's path in out; false when too
 * long. */
static bool to_volume(char *out, const char *path)
{
	const size_t v = sizeof(S10B_STORE_VOLUME) - 1u, n = strlen(path);

	if (v + n + 1u > S10B_STORE_PATH)
		return false;
	memcpy(out, S10B_STORE_VOLUME, v);
	memcpy(out + v, path, n + 1u);
	if (n == 1u)
		out[v] = '\0';		/* the root */
	return true;
}

static enum s10b_result result(const struct s10b_store *st, int32_t r)
{
	if (r >= 0 && st->err == 0)
		return S10B_V_OK;
	if (st->err == S09_EEXIST)
		return S10B_V_EXISTS;
	return st->err == S09_ENOSPC ? S10B_V_FULL : S10B_V_FAIL;
}

/* 0x22: false when it fails, with the errno in st->err. */
static bool stat_of(struct s10b_store *st, const char *vpath,
		    struct s09_stat *s)
{
	uint32_t got;

	if (path_call(st, S09_STAT, 0, 0, vpath, st->rec, S09_STAT_LEN,
		      &got) != 0 || st->err != 0 || got != S09_STAT_LEN)
		return false;
	s09_stat_unpack(st->rec, s);
	return true;
}

static void fill_entry(const struct s09_stat *s, struct s10b_entry *e)
{
	e->kind = S10B_KIND_OTHER;
	if ((s->mode & MODE_FMT) == MODE_DIR)
		e->kind = S10B_KIND_DIR;
	else if ((s->mode & MODE_FMT) == MODE_REG)
		e->kind = S10B_KIND_FILE;
	e->size = s->size;
	e->mtime = s->mtime;
	e->ctime = s->ctime;
}

/* 0x31, 0x32: an unsigned 64-bit little-endian count (S-09.041). */
static bool space_of(struct s10b_store *st, uint8_t cmd, uint64_t *v)
{
	uint8_t b[8];
	uint32_t got;
	unsigned i;

	/* S-09.041: the errno of these two is the volume's last error, from any
	 * earlier call, so it does not say whether this one worked. A slave that
	 * has gone answers no bytes. */
	if (path_call(st, cmd, 0, 0, S10B_STORE_VOLUME, b, sizeof(b), &got) !=
	    0 || got != 8u)
		return false;
	*v = 0;
	for (i = 8u; i > 0u; i--)
		*v = *v << 8 | b[i - 1u];
	return true;
}

static bool vol_space(void *ctx, uint64_t *total, uint64_t *free_bytes)
{
	struct s10b_store *st = ctx;

	return space_of(st, S09_TOTAL_SPACE, total) &&
	       space_of(st, S09_FREE_SPACE, free_bytes);
}

/* One pass over the directory (0x2E, 0x2F, 0x30), each entry's kind,
 * size and times from its stat record (0x22). */
static bool vol_list(void *ctx, const char *path, s10b_entry_fn fn, void *arg)
{
	struct s10b_store *st = ctx;
	struct s09_dirent d;
	struct s09_stat s;
	struct s10b_entry e;
	uint32_t got;
	size_t base;
	int32_t h;
	bool ok = true;

	if (!to_volume(st->path, path))
		return false;
	/* A handle is opaque: 0 is the failure (S-09.041). */
	h = path_call(st, S09_OPENDIR, 0, 0, st->path, NULL, 0, NULL);
	if (h == 0 || st->err != 0u)
		return false;
	base = strlen(st->path);
	for (;;) {
		int32_t r = call(st, S09_READDIR, (uint32_t)h, 0, NULL, 0,
				 st->rec, S09_DIRENT_LEN, &got);

		if (st->err != 0u || (r != 0 && got != S09_DIRENT_LEN)) {
			ok = false;
			break;
		}
		if (r == 0)
			break;			/* the end */
		s09_dirent_unpack(st->rec, &d);
		d.name[S09_DIRENT_NAME - 1u] = '\0';
		e.name_len = strlen(d.name);
		if ((e.name_len == 1u && d.name[0] == '.') ||
		    (e.name_len == 2u && d.name[0] == '.' && d.name[1] == '.'))
			continue;
		if (base + 1u + e.name_len >= sizeof(st->path)) {
			ok = false;
			break;
		}
		st->path[base] = '/';
		memcpy(st->path + base + 1u, d.name, e.name_len + 1u);
		e.name = (const uint8_t *)d.name;
		e.kind = S10B_KIND_OTHER;
		e.size = e.mtime = e.ctime = 0;
		if (stat_of(st, st->path, &s))
			fill_entry(&s, &e);
		st->path[base] = '\0';
		fn(arg, &e);
	}
	(void)call(st, S09_CLOSEDIR, (uint32_t)h, 0, NULL, 0, NULL, 0, NULL);
	return ok;
}

/* Open, seek, read, close: no handle outlives the request. */
static long vol_read(void *ctx, const char *path, uint32_t offset,
		     uint8_t *buf, size_t n)
{
	struct s10b_store *st = ctx;
	uint32_t *err = &st->err;
	int32_t fd, r = -1;

	if (!to_volume(st->path, path))
		return -1;
	fd = path_call(st, S09_OPEN, S09_O_RDONLY, 0, st->path, NULL, 0, NULL);
	if (fd < 0 || *err != 0u)
		return -1;
	if (call(st, S09_LSEEK, (uint32_t)fd, offset, NULL, 0, NULL, 0, NULL) ==
	    (int32_t)offset && *err == 0u) {
		r = 0;
		if (n > 0u)
			r = s09_master_read(st->f->m, (uint32_t)fd, buf,
					    (uint32_t)n, err);
		if (*err != 0u)
			r = -1;
	}
	(void)call(st, S09_CLOSE, (uint32_t)fd, 0, NULL, 0, NULL, 0, NULL);
	return r;
}

static bool vol_stat(void *ctx, const char *path, struct s10b_entry *e)
{
	struct s10b_store *st = ctx;
	struct s09_stat s;

	if (!to_volume(st->path, path) || !stat_of(st, st->path, &s))
		return false;
	fill_entry(&s, e);
	return true;
}

static enum s10b_result vol_mkdir(void *ctx, const char *path)
{
	struct s10b_store *st = ctx;

	if (!to_volume(st->path, path))
		return S10B_V_FAIL;
	return result(st, path_call(st, S09_MKDIR, MKDIR_MODE, 0, st->path,
				    NULL, 0, NULL));
}

/* 0x29 with create and exclusive: never an existing file (S-10b Q-05). */
static enum s10b_result vol_create(void *ctx, const char *path, int32_t *fd)
{
	struct s10b_store *st = ctx;
	int32_t r;

	if (!to_volume(st->path, path))
		return S10B_V_FAIL;
	st->f->made_len = 0;
	r = path_call(st, S09_OPEN, S09_O_WRONLY | S09_O_CREAT | S09_O_EXCL, 0,
		      st->path, NULL, 0, NULL);
	*fd = r;
	return result(st, r);
}

/* 0x2B in pieces a plain link carries (S-09.023). A piece written short
 * found the volume full. */
static enum s10b_result vol_write(void *ctx, int32_t fd, const uint8_t *p,
				  size_t n)
{
	struct s10b_store *st = ctx;
	size_t done = 0;

	while (done < n) {
		const uint32_t k = (uint32_t)(n - done < S09_TUNNEL_MAX ?
					      n - done : S09_TUNNEL_MAX);
		const int32_t r = call(st, S09_WRITE, (uint32_t)fd, k, p + done,
				       k, NULL, 0, NULL);

		if (r < 0 || st->err != 0)
			return result(st, -1);
		if ((uint32_t)r != k)
			return S10B_V_FULL;
		done += k;
	}
	return S10B_V_OK;
}

static enum s10b_result vol_close(void *ctx, int32_t fd)
{
	struct s10b_store *st = ctx;

	return result(st, call(st, S09_CLOSE, (uint32_t)fd, 0, NULL, 0, NULL,
			       0, NULL));
}

/* The first entry of directory st->path but "." and ".."; false at none,
 * with st->err set on a failure. */
static bool first_entry(struct s10b_store *st, struct s09_dirent *d)
{
	uint32_t got;
	int32_t h;
	bool found = false;

	h = path_call(st, S09_OPENDIR, 0, 0, st->path, NULL, 0, NULL);
	if (h == 0 || st->err != 0u) {
		if (st->err == 0u)
			st->err = S09_EINVAL;
		return false;
	}
	for (;;) {
		const int32_t r = call(st, S09_READDIR, (uint32_t)h, 0, NULL, 0,
				       st->rec, S09_DIRENT_LEN, &got);

		if (r == 0 || st->err != 0u)
			break;		/* the end (errno 0), or a failure */
		if (got != S09_DIRENT_LEN) {
			st->err = S09_EINVAL;
			break;
		}
		s09_dirent_unpack(st->rec, d);
		d->name[S09_DIRENT_NAME - 1u] = '\0';
		if (strcmp(d->name, ".") != 0 && strcmp(d->name, "..") != 0) {
			found = true;
			break;
		}
	}
	(void)call(st, S09_CLOSEDIR, (uint32_t)h, 0, NULL, 0, NULL, 0, NULL);
	return found;
}

/*
 * A file, or a folder with everything in it, whatever the listing shows
 * (S-10b.082). st->path is always a folder in the walk: go into its first
 * entry, or with none left remove it and step back up. One path buffer,
 * no recursion.
 */
static enum s10b_result vol_remove(void *ctx, const char *path)
{
	struct s10b_store *st = ctx;
	struct s09_dirent d;
	struct s09_stat s;
	size_t root;
	uint32_t steps = 0;

	if (!to_volume(st->path, path))
		return S10B_V_FAIL;
	st->f->made_len = 0;
	if (!stat_of(st, st->path, &s))
		return st->err == S09_ENOENT ? S10B_V_OK : S10B_V_FAIL;
	if ((s.mode & MODE_FMT) != MODE_DIR)
		return result(st, path_call(st, S09_UNLINK, 0, 0, st->path,
					    NULL, 0, NULL));
	root = strlen(st->path);
	while (steps++ < REMOVE_STEPS) {
		size_t n = strlen(st->path);
		int32_t r;

		if (first_entry(st, &d)) {
			const size_t k = strlen(d.name);

			if (n + 1u + k >= sizeof(st->path))
				return S10B_V_FAIL;
			st->path[n] = '/';
			memcpy(st->path + n + 1u, d.name, k + 1u);
			if (d.type == DT_DIR)
				continue;
			r = path_call(st, S09_UNLINK, 0, 0, st->path, NULL, 0,
				      NULL);
			if (result(st, r) != S10B_V_OK)
				return result(st, r);
			st->path[n] = '\0';
			continue;
		}
		if (st->err != 0u)
			return S10B_V_FAIL;
		r = path_call(st, S09_RMDIR, 0, 0, st->path, NULL, 0, NULL);
		if (result(st, r) != S10B_V_OK)
			return result(st, r);
		if (n <= root)
			return S10B_V_OK;
		while (n > root && st->path[n] != '/')
			n--;
		st->path[n] = '\0';
	}
	return S10B_V_FAIL;
}

/* 0x24, once the target is known not to exist: yaffs2's rename would
 * replace it. */
static enum s10b_result vol_rename(void *ctx, const char *from, const char *to)
{
	struct s10b_store *st = ctx;
	struct s09_stat s;
	size_t a, b;

	if (!to_volume(st->path, from) || !to_volume(st->path2, to))
		return S10B_V_FAIL;
	a = strlen(st->path);
	b = strlen(st->path2);
	if (a + b + 2u > sizeof(st->req))
		return S10B_V_FAIL;
	st->f->made_len = 0;
	if (stat_of(st, st->path2, &s))
		return S10B_V_EXISTS;
	if (st->err != S09_ENOENT)
		return S10B_V_FAIL;
	/* S-09.023: old path and NUL, new path and NUL; arg1 the first's
	 * length */
	memcpy(st->req, st->path, a + 1u);
	memcpy(st->req + a + 1u, st->path2, b + 1u);
	return result(st, call(st, S09_RENAME, 0, (uint32_t)a + 1u, st->req,
			       (uint32_t)(a + b) + 2u, NULL, 0, NULL));
}

/* 0x35: access and modification times. */
static enum s10b_result vol_set_times(void *ctx, const char *path,
				      uint32_t atime, uint32_t mtime)
{
	struct s10b_store *st = ctx;

	if (!to_volume(st->path, path))
		return S10B_V_FAIL;
	return result(st, path_call(st, S09_UTIME, atime, mtime, st->path,
				    NULL, 0, NULL));
}

/* 0x04: the eMMC's cache committed (S-03.068), so a power loss keeps the
 * change. */
static void vol_commit(void *ctx)
{
	struct s10b_store *st = ctx;

	(void)call(st, S09_FLUSH, 0, 0, NULL, 0, NULL, 0, NULL);
}

void s10b_store_ops(struct s10b_store *st, struct s09_files *f,
		    struct s10b_volume_ops *ops)
{
	memset(ops, 0, sizeof(*ops));
	st->f = f;
	ops->ctx = st;
	ops->space = vol_space;
	ops->list = vol_list;
	ops->read = vol_read;
	ops->stat = vol_stat;
	ops->mkdir = vol_mkdir;
	ops->create = vol_create;
	ops->write = vol_write;
	ops->close = vol_close;
	ops->remove = vol_remove;
	ops->rename = vol_rename;
	ops->set_times = vol_set_times;
	ops->commit = vol_commit;
}
