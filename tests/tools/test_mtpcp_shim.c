/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * waffle-boot's MTP responder (src/s10b_mtp.c) over a few files held in the
 * calling process, so that tests/tools/test_mtpcp.py can run tools/mtpcp.py against
 * the real engine through ctypes. A host build for that test only.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "waffle/s10b_mtp.h"

#define MAX_FILES 16

static struct {
	const char *name;
	const uint8_t *data;
	uint32_t size;
	bool dir;
} files[MAX_FILES];
static size_t n_files;
static struct s10b engine;

static bool space(void *ctx, uint64_t *total, uint64_t *free_bytes)
{
	(void)ctx;
	*total = 2403870720u;
	*free_bytes = 1048576u;
	return true;
}

static bool list(void *ctx, const char *path, s10b_entry_fn fn, void *arg)
{
	size_t i;

	(void)ctx;
	if (strcmp(path, "/") != 0)
		return true;
	for (i = 0; i < n_files; i++) {
		struct s10b_entry e;

		memset(&e, 0, sizeof(e));
		e.name = (const uint8_t *)files[i].name;
		e.name_len = strlen(files[i].name);
		e.kind = files[i].dir ? S10B_KIND_DIR : S10B_KIND_FILE;
		e.size = files[i].size;
		e.mtime = e.ctime = 1000000000u;
		fn(arg, &e);
	}
	return true;
}

static long read_file(void *ctx, const char *path, uint32_t offset,
		      uint8_t *buf, size_t n)
{
	size_t i;

	(void)ctx;
	for (i = 0; i < n_files; i++) {
		if (files[i].dir || strcmp(path + 1, files[i].name) != 0)
			continue;
		if (offset >= files[i].size)
			return 0;
		if (n > files[i].size - offset)
			n = files[i].size - offset;
		memcpy(buf, files[i].data + offset, n);
		return (long)n;
	}
	return -1;
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
};
static const struct s10b_device_ops dev_ops = {
	.battery = battery, .clock_read = clock_read, .clock_write = clock_write,
};

void shim_add(const char *name, const uint8_t *data, uint32_t size, int dir)
{
	files[n_files].name = name;
	files[n_files].data = data;
	files[n_files].size = size;
	files[n_files].dir = dir != 0;
	n_files++;
}

void shim_start(size_t mps)
{
	static struct s10b_object objects[64];
	static uint8_t names[4096];
	const struct s10b_config cfg = {
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
	n_files = 0;
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
