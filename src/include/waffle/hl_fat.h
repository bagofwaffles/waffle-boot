/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_HL_FAT_H
#define WAFFLE_HL_FAT_H

/*
 * The host link's /user root: a FAT volume on a block device, read and
 * written with FatFs (src/third_party/fatfs). Paths are the root's rest
 * ("" the volume's root). The volume stays mounted from its first use
 * until FatFs's one drive is taken for another volume (the RAM disk);
 * a put under way is then lost, and its temporary file stays. Each
 * completed change (a put's end, a removal, a directory, a rename) is
 * written through and the medium's cache flushed before the call returns.
 * Nothing formats, and FatFs's writes reach only the block device, whose
 * own bounds hold them to the volume. Every int is an enum hl_status.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "blockdev.h"
#include "ff.h"
#include "waffle/hostlink.h"

struct hl_fat {
	const struct wb_blockdev *dev;
	/* the medium's cache to its nonvolatile memory: 0 on success */
	int (*flush)(void *ctx);
	/* FatFs's drive is in use for another volume now */
	bool (*busy)(void *ctx);
	void *ctx;
	bool mounted, writing, reading;
	char rd_path[HL_PATH_MAX + 8u];
	uint32_t rd_pos;
	FATFS fs;
	FIL wr, rd;
};

/* HL_OK mounted; HL_E_FS when the device holds no FAT volume FatFs reads
 * (an exFAT one included); HL_E_BUSY while the drive is another's. */
int hlfat_mount(struct hl_fat *f);
int hlfat_stat(struct hl_fat *f, const char *rest, struct hl_stat *st);
int hlfat_list(struct hl_fat *f, const char *rest, uint32_t start, hl_each_fn each,
	       void *arg, bool *end);
int hlfat_read(struct hl_fat *f, const char *rest, uint32_t off, uint8_t *buf,
	       uint32_t n, uint32_t *got);
int hlfat_create(struct hl_fat *f, const char *rest);
int hlfat_write(struct hl_fat *f, const uint8_t *b, uint32_t n);
int hlfat_finish(struct hl_fat *f);
void hlfat_abandon(struct hl_fat *f);
/* A file, or an empty directory. */
int hlfat_remove(struct hl_fat *f, const char *rest);
int hlfat_mkdir(struct hl_fat *f, const char *rest);
/* A file in the way of `to` is replaced; a directory there refuses. */
int hlfat_rename(struct hl_fat *f, const char *from, const char *to);

#endif /* WAFFLE_HL_FAT_H */
