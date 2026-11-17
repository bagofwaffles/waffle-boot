/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FatFs on a disk a host left behind (S-12.020): the input is the start of
 * a RAM disk of S-12.001's size; the rest reads as zero. The volume is
 * mounted, walked, read, and written to as the bootloader does
 * (S-12.017's files, S-14.051's log export, a scan's removals), each file
 * opened with fast seek as a package is.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "wb_fatfs.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define DISK_SECTORS 454656u
#define BACKED_SECTORS 16384u   /* 8 MiB; writes beyond are dropped */
#define MAX_ENTRIES 256
#define MAX_DEPTH 8
#define MAX_READ 65536u

static uint8_t backing[BACKED_SECTORS * 512u];
static size_t dirty;            /* bytes of backing that may be nonzero */
static unsigned int entries;

static int fz_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	uint8_t *out = buf;

	(void)ctx;
	if (lba >= DISK_SECTORS || count > DISK_SECTORS - lba)
		return -1;
	for (; count; count--, lba++, out += 512) {
		if (lba < BACKED_SECTORS)
			memcpy(out, backing + (size_t)lba * 512u, 512);
		else
			memset(out, 0, 512);
	}
	return 0;
}

static int fz_write(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
	const uint8_t *in = buf;

	(void)ctx;
	if (lba >= DISK_SECTORS || count > DISK_SECTORS - lba)
		return -1;
	for (; count; count--, lba++, in += 512) {
		if (lba >= BACKED_SECTORS)
			continue;
		memcpy(backing + (size_t)lba * 512u, in, 512);
		if ((size_t)(lba + 1) * 512u > dirty)
			dirty = (size_t)(lba + 1) * 512u;
	}
	return 0;
}

static const struct wb_blockdev disk = {
	NULL, DISK_SECTORS, fz_read, fz_write, NULL, NULL, NULL,
};

static void read_file(const char *path)
{
	static uint8_t buf[4096];
	FIL fil;
	UINT got;
	size_t total = 0;

	static DWORD map[32];

	if (f_open(&fil, path, FA_READ) != FR_OK)
		return;
	/* the bootloader's open (wire_fat.c): a link map when the chain ends,
	 * then seeks back as the in-place recovery makes them */
	(void)wb_fatfs_link_map(&fil, map, sizeof(map) / sizeof(map[0]));
	while (total < MAX_READ &&
	       f_read(&fil, buf, sizeof(buf), &got) == FR_OK && got)
		total += got;
	if (f_lseek(&fil, f_size(&fil) / 2u) == FR_OK)
		(void)f_read(&fil, buf, sizeof(buf), &got);
	if (f_lseek(&fil, 0) == FR_OK)
		(void)f_read(&fil, buf, sizeof(buf), &got);
	f_close(&fil);
}

static void walk(const char *dir, int depth)
{
	char path[2048];
	FILINFO fno;
	DIR d;

	if (depth > MAX_DEPTH || f_opendir(&d, dir) != FR_OK)
		return;
	while (entries < MAX_ENTRIES && f_readdir(&d, &fno) == FR_OK &&
	       fno.fname[0]) {
		entries++;
		snprintf(path, sizeof(path), "%s/%s", dir, fno.fname);
		if (fno.fattrib & AM_DIR)
			walk(path, depth + 1);
		else
			read_file(path);
	}
	f_closedir(&d);
}

static void write_some(void)
{
	static uint8_t buf[5000];
	FIL fil;
	UINT done;
	FILINFO fno;

	memset(buf, 'x', sizeof(buf));
	f_mkdir("systeminfo");
	if (f_open(&fil, "systeminfo/version.txt",
		   FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
		f_write(&fil, buf, sizeof(buf), &done);
		f_close(&fil);
	}
	fno.fdate = 0x5B5C;
	fno.ftime = 0x6000;
	f_utime("systeminfo/version.txt", &fno);
	f_mkdir("logs");
	if (f_open(&fil, "logs/a log with a long name.log",
		   FA_WRITE | FA_OPEN_APPEND) == FR_OK) {
		f_write(&fil, buf, 700, &done);
		f_close(&fil);
	}
	f_rename("logs/a log with a long name.log", "logs/boot.log");
	f_unlink("logs/boot.log");
	f_unlink("op-xy.tfw");
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FATFS fs;

	if (size > sizeof(backing))
		size = sizeof(backing);
	memcpy(backing, data, size);
	if (dirty > size)
		memset(backing + size, 0, dirty - size);
	dirty = size;
	entries = 0;

	wb_fatfs_attach(&disk);
	if (f_mount(&fs, "", 1) == FR_OK) {
		DWORD free_clusters, vsn;
		FATFS *pfs;
		char label[64];

		f_getlabel("", label, &vsn);
		f_getfree("", &free_clusters, &pfs);
		walk("", 0);
		write_some();
		entries = 0;
		walk("", 0);
	}
	/* f_mount registers fs even when the mount fails. */
	f_mount(NULL, "", 0);
	return 0;
}
