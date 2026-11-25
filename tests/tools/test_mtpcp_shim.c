/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * waffle-boot's MTP responder (src/core/mtp/mtp.c) over a few files held in the
 * calling process, at level R or W, so that tests/tools/test_mtpcp.py can run
 * tools/mtpcp.py against the real engine through ctypes. A host build for
 * that test only.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "waffle/mtp/mtp.h"

#define MAX_FILES 64

static struct {
	char *path;		/* "/a/b" */
	uint8_t *data;
	uint32_t size, mtime;
	bool dir, used;
} files[MAX_FILES];
static struct s10b engine;

static bool space(void *ctx, uint64_t *total, uint64_t *free_bytes)
{
	(void)ctx;
	*total = 2403870720u;
	*free_bytes = 1048576u;
	return true;
}

static int find(const char *path)
{
	int i;

	for (i = 0; i < MAX_FILES; i++)
		if (files[i].used && strcmp(files[i].path, path) == 0)
			return i;
	return -1;
}

/* An entry directly in folder `dir` ("/" the root): its name, or NULL. */
static const char *child_name(int i, const char *dir)
{
	const char *p = files[i].path;
	const char *last = strrchr(p, '/');
	const size_t n = (size_t)(last - p);

	if (strcmp(dir, "/") == 0)
		return n == 0 ? last + 1 : NULL;
	return n == strlen(dir) && memcmp(p, dir, n) == 0 ? last + 1 : NULL;
}

static bool list(void *ctx, const char *path, s10b_entry_fn fn, void *arg)
{
	int i;

	(void)ctx;
	for (i = 0; i < MAX_FILES; i++) {
		struct s10b_entry e;
		const char *name;

		if (!files[i].used || !(name = child_name(i, path)))
			continue;
		memset(&e, 0, sizeof(e));
		e.name = (const uint8_t *)name;
		e.name_len = strlen(name);
		e.kind = files[i].dir ? S10B_KIND_DIR : S10B_KIND_FILE;
		e.size = files[i].size;
		e.mtime = e.ctime = files[i].mtime;
		fn(arg, &e);
	}
	return true;
}

static long read_file(void *ctx, const char *path, uint32_t offset,
		      uint8_t *buf, size_t n)
{
	const int i = find(path);

	(void)ctx;
	if (i < 0 || files[i].dir)
		return -1;
	if (offset >= files[i].size)
		return 0;
	if (n > files[i].size - offset)
		n = files[i].size - offset;
	memcpy(buf, files[i].data + offset, n);
	return (long)n;
}

static int add(const char *path, const uint8_t *data, uint32_t size, bool dir)
{
	int i;

	for (i = 0; i < MAX_FILES; i++)
		if (!files[i].used)
			break;
	if (i == MAX_FILES)
		return -1;
	files[i].used = true;
	files[i].path = malloc(strlen(path) + 1u);
	strcpy(files[i].path, path);
	files[i].data = malloc(size ? size : 1u);
	if (size)
		memcpy(files[i].data, data, size);
	files[i].size = size;
	files[i].dir = dir;
	files[i].mtime = 1000000000u;
	return i;
}

static void drop(int i)
{
	free(files[i].path);
	free(files[i].data);
	memset(&files[i], 0, sizeof(files[i]));
}

static bool v_stat(void *ctx, const char *path, struct s10b_entry *e)
{
	const int i = find(path);

	(void)ctx;
	if (i < 0)
		return false;
	e->kind = files[i].dir ? S10B_KIND_DIR : S10B_KIND_FILE;
	e->size = files[i].size;
	e->mtime = e->ctime = files[i].mtime;
	return true;
}

static enum s10b_result v_mkdir(void *ctx, const char *path)
{
	(void)ctx;
	if (find(path) >= 0)
		return S10B_V_EXISTS;
	return add(path, NULL, 0, true) >= 0 ? S10B_V_OK : S10B_V_FULL;
}

static enum s10b_result v_create(void *ctx, const char *path, int32_t *fd)
{
	(void)ctx;
	if (find(path) >= 0)
		return S10B_V_EXISTS;
	*fd = add(path, NULL, 0, false);
	return *fd >= 0 ? S10B_V_OK : S10B_V_FULL;
}

static enum s10b_result v_write(void *ctx, int32_t fd, const uint8_t *p,
				size_t n)
{
	(void)ctx;
	files[fd].data = realloc(files[fd].data, files[fd].size + n);
	memcpy(files[fd].data + files[fd].size, p, n);
	files[fd].size += (uint32_t)n;
	return S10B_V_OK;
}

static enum s10b_result v_close(void *ctx, int32_t fd)
{
	(void)ctx;
	(void)fd;
	return S10B_V_OK;
}

static bool under(int i, const char *path)
{
	const size_t n = strlen(path);

	return files[i].used && strncmp(files[i].path, path, n) == 0 &&
	       (files[i].path[n] == '\0' || files[i].path[n] == '/');
}

static enum s10b_result v_remove(void *ctx, const char *path)
{
	int i;

	(void)ctx;
	for (i = 0; i < MAX_FILES; i++)
		if (under(i, path))
			drop(i);
	return S10B_V_OK;
}

static enum s10b_result v_rename(void *ctx, const char *from, const char *to)
{
	const size_t a = strlen(from);
	int i;

	(void)ctx;
	if (find(to) >= 0)
		return S10B_V_EXISTS;
	for (i = 0; i < MAX_FILES; i++) {
		char *np;

		if (!under(i, from))
			continue;
		np = malloc(strlen(to) + strlen(files[i].path + a) + 1u);
		strcpy(np, to);
		strcat(np, files[i].path + a);
		free(files[i].path);
		files[i].path = np;
	}
	return S10B_V_OK;
}

static enum s10b_result v_set_times(void *ctx, const char *path,
				    uint32_t atime, uint32_t mtime)
{
	const int i = find(path);

	(void)ctx;
	(void)atime;
	if (i < 0)
		return S10B_V_FAIL;
	files[i].mtime = mtime;
	return S10B_V_OK;
}

static unsigned battery(void *ctx)
{
	(void)ctx;
	return 57;
}

static uint32_t clock_read(void *ctx)
{
	(void)ctx;
	return 0;
}

static void clock_write(void *ctx, uint32_t count)
{
	(void)ctx;
	(void)count;
}

static const struct s10b_volume_ops vol_ops = {
	.space = space, .list = list, .read = read_file,
	.stat = v_stat, .mkdir = v_mkdir, .create = v_create,
	.write = v_write, .close = v_close, .remove = v_remove,
	.rename = v_rename, .set_times = v_set_times,
};
static const struct s10b_device_ops dev_ops = {
	.battery = battery, .clock_read = clock_read, .clock_write = clock_write,
};

/* A file or folder at the top of the volume. */
void shim_add(const char *name, const uint8_t *data, uint32_t size, int dir)
{
	char path[300] = "/";

	strncat(path, name, sizeof(path) - 2u);
	(void)add(path, data, size, dir != 0);
}

/* level 0: R, 1: W */
void shim_start(size_t mps, int level)
{
	static struct s10b_object objects[64];
	static uint8_t names[64u * (S10B_NAME_REC + 64u)];
	const struct s10b_config cfg = {
		.level = level ? S10B_LEVEL_W : S10B_LEVEL_R,
		.vol = &vol_ops, .dev = &dev_ops,
		.manufacturer = "m", .model = "OP-XY", .version = "1.0.0",
		.serial = "s",
		.objects = objects, .max_objects = 64,
		.names = names, .names_size = sizeof(names),
	};

	s10b_init(&engine, &cfg);
	s10b_reset(&engine, mps);
}

void shim_reset_files(void)
{
	int i;

	for (i = 0; i < MAX_FILES; i++)
		if (files[i].used)
			drop(i);
}

/* The bytes of the file at path ("/a/b"): its size, -1 when there is none,
 * -2 for a folder. */
long shim_get(const char *path, uint8_t *buf, size_t cap)
{
	const int i = find(path);

	if (i < 0)
		return -1;
	if (files[i].dir)
		return -2;
	memcpy(buf, files[i].data, files[i].size < cap ? files[i].size : cap);
	return (long)files[i].size;
}

size_t shim_rx_size(void)
{
	return s10b_rx_size(&engine);
}

void shim_rx(const uint8_t *p, size_t n)
{
	s10b_rx(&engine, p, n);
}

/* A transfer's length, -1 when the device has nothing to send. */
long shim_tx(uint8_t *buf, size_t cap)
{
	size_t n;

	return s10b_tx(&engine, buf, cap, &n) ? (long)n : -1;
}
