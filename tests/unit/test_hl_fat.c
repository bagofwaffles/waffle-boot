/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The host link's /user root (src/hl_fat.c) on a FAT volume this test
 * builds in memory with FatFs's f_mkfs: files written, read in chunks,
 * listed, renamed over a file, removed; every completed change flushed;
 * a volume FatFs does not read (blank, or exFAT) answers HL_E_FS; a drive
 * taken for another volume loses the put under way, and the root mounts
 * again after.
 */
#include <string.h>

#include "support/check.h"

#include "waffle/hl_fat.h"
#include "wb_fatfs.h"

#define SECTORS 16384u		/* 8 MiB */

static uint8_t disk[SECTORS * 512u], other[64u * 512u];
static unsigned flushes;
static bool busy_now;

static int d_read(void *ctx, uint32_t lba, void *buf, uint32_t n)
{
	uint8_t *d = ctx;

	memcpy(buf, d + (size_t)lba * 512u, (size_t)n * 512u);
	return 0;
}

static int d_write(void *ctx, uint32_t lba, const void *buf, uint32_t n)
{
	uint8_t *d = ctx;

	memcpy(d + (size_t)lba * 512u, buf, (size_t)n * 512u);
	return 0;
}

static const struct wb_blockdev dev = { .ctx = disk, .n_sectors = SECTORS, .read = d_read,
					.write = d_write };
static const struct wb_blockdev odev = { .ctx = other, .n_sectors = 64u, .read = d_read,
					 .write = d_write };

static int t_flush(void *ctx)
{
	(void)ctx;
	flushes++;
	return 0;
}

static bool t_busy(void *ctx)
{
	(void)ctx;
	return busy_now;
}

static struct hl_fat F = { .dev = &dev, .flush = t_flush, .busy = t_busy };

static void format(void)
{
	static uint8_t work[4096];
	const MKFS_PARM opt = { FM_FAT, 1, 1, 512, 0 };

	memset(disk, 0, sizeof disk);
	wb_fatfs_attach(&dev);
	CHECK(f_mkfs("", &opt, work, sizeof work) == FR_OK);
	wb_fatfs_attach(NULL);
	memset(&F, 0, sizeof F);
	F.dev = &dev;
	F.flush = t_flush;
	F.busy = t_busy;
	flushes = 0;
}

static unsigned n_seen;
static char seen[8][32];

static bool each(void *arg, const char *name, const struct hl_stat *st)
{
	(void)arg;
	(void)st;
	if (n_seen >= 8u)
		return false;
	strncpy(seen[n_seen++], name, 31);
	return true;
}

static void test_files(void)
{
	uint8_t data[3000], back[3000];
	struct hl_stat st;
	uint32_t got, at = 0;
	bool end;

	format();
	for (unsigned i = 0; i < sizeof data; i++)
		data[i] = (uint8_t)(i * 7u);
	CHECK(hlfat_mount(&F) == HL_OK);
	CHECK(hlfat_stat(&F, "", &st) == HL_OK && st.type == 2u);
	CHECK(hlfat_create(&F, "song.wav.wbpart") == HL_OK);
	CHECK(hlfat_write(&F, data, 1000) == HL_OK && hlfat_write(&F, data + 1000, 2000) == HL_OK);
	CHECK(flushes == 0u);
	CHECK(hlfat_finish(&F) == HL_OK && flushes == 1u);
	CHECK(hlfat_rename(&F, "song.wav.wbpart", "song.wav") == HL_OK && flushes == 2u);
	CHECK(hlfat_stat(&F, "song.wav", &st) == HL_OK && st.type == 1u && st.size == 3000u);
	/* read in chunks, by offset, and from an offset afresh */
	while (at < sizeof back) {
		CHECK(hlfat_read(&F, "song.wav", at, back + at, 224, &got) == HL_OK);
		if (got == 0u)
			break;
		at += got;
	}
	CHECK(at == 3000u && memcmp(back, data, 3000) == 0);
	CHECK(hlfat_read(&F, "song.wav", 2990, back, 224, &got) == HL_OK && got == 10u &&
	      memcmp(back, data + 2990, 10) == 0);
	CHECK(hlfat_read(&F, "none.wav", 0, back, 224, &got) == HL_E_NOT_FOUND);
	/* a directory, listed; a rename over a file replaces it */
	CHECK(hlfat_mkdir(&F, "samples") == HL_OK && flushes == 3u);
	CHECK(hlfat_mkdir(&F, "samples") == HL_E_EXISTS);
	CHECK(hlfat_create(&F, "samples/a.wav") == HL_OK && hlfat_write(&F, data, 10) == HL_OK &&
	      hlfat_finish(&F) == HL_OK);
	CHECK(hlfat_rename(&F, "song.wav", "samples/a.wav") == HL_OK);
	CHECK(hlfat_stat(&F, "samples/a.wav", &st) == HL_OK && st.size == 3000u);
	CHECK(hlfat_stat(&F, "song.wav", &st) == HL_E_NOT_FOUND);
	n_seen = 0;
	CHECK(hlfat_list(&F, "samples", 0, each, NULL, &end) == HL_OK && end && n_seen == 1u &&
	      strcmp(seen[0], "a.wav") == 0);
	n_seen = 0;
	CHECK(hlfat_list(&F, "", 0, each, NULL, &end) == HL_OK && end && n_seen == 1u &&
	      strcmp(seen[0], "samples") == 0);
	/* a rename onto a directory is refused, a non-empty one is not removed */
	CHECK(hlfat_create(&F, "b.wav") == HL_OK && hlfat_finish(&F) == HL_OK);
	CHECK(hlfat_rename(&F, "b.wav", "samples") != HL_OK);
	CHECK(hlfat_remove(&F, "samples") != HL_OK);
	CHECK(hlfat_remove(&F, "samples/a.wav") == HL_OK && hlfat_remove(&F, "samples") == HL_OK);
	CHECK(hlfat_remove(&F, "samples") == HL_E_NOT_FOUND);
	/* the volume as a fresh mount reads it: everything on the medium */
	memset(&F.fs, 0, sizeof F.fs);
	F.mounted = false;
	CHECK(hlfat_stat(&F, "b.wav", &st) == HL_OK && st.size == 0u);
}

static void test_volumes(void)
{
	struct hl_stat st;

	/* blank, and exFAT (this build reads none): HL_E_FS */
	memset(disk, 0, 4096);
	memset(&F, 0, sizeof F);
	F.dev = &dev;
	CHECK(hlfat_stat(&F, "", &st) == HL_E_FS);
	memcpy(disk + 3, "EXFAT   ", 8);
	disk[510] = 0x55;
	disk[511] = 0xAA;
	CHECK(hlfat_mount(&F) == HL_E_FS);
	/* the drive another's now */
	format();
	busy_now = true;
	CHECK(hlfat_mount(&F) == HL_E_BUSY);
	busy_now = false;
	/* a put under way when the drive is taken: lost; the root comes back */
	CHECK(hlfat_create(&F, "x.wbpart") == HL_OK && hlfat_write(&F, disk, 100) == HL_OK);
	wb_fatfs_attach(&odev);
	CHECK(hlfat_write(&F, disk, 100) == HL_E_IO && hlfat_finish(&F) == HL_E_IO);
	CHECK(hlfat_stat(&F, "x.wbpart", &st) == HL_OK);	/* the temporary file stays */
	CHECK(hlfat_remove(&F, "x.wbpart") == HL_OK);
	wb_fatfs_attach(NULL);
}

int main(void)
{
	RUN(test_files);
	RUN(test_volumes);
	return check_summary("test_hl_fat");
}
