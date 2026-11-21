/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_WIRE_FAT_H
#define WAFFLE_WIRE_FAT_H

/*
 * The USB disk's FAT volume (S-12 section 1) on FatFs over src/glue/blockdev:
 * the RAM disk's format and mount, the names and stamps the bootloader
 * writes (S-12.012, S-12.015, S-12.016), the root listing and package file
 * of S-11.046 (s11_root_fn, struct wire_source), and the log export's
 * copies from the yaffs2 volume (struct s14_export_ops).
 *
 * FatFs has one physical drive (src/glue/fatfs): one struct wire_fat is mounted
 * at a time. Paths are UTF-8, relative to the volume's root, '/'-separated.
 * Reads alone never write the disk (S-12.021, S-12.022).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff.h"
#include "blockdev.h"

#include "waffle/s14_export.h"
#include "waffle/wire_package.h"
#include "waffle/wire_yaffs.h"

struct wb_device;

#define WIRE_FAT_NAME_MAX 255u		/* UTF-16 units of a long name */
/* "logs/" and a name of S14_EXPORT_NAME_MAX bytes */
#define WIRE_FAT_PATH_MAX (5u + S14_EXPORT_NAME_MAX + 1u)
/* The open file's cluster map for FatFs's fast seek: 127 fragments */
#define WIRE_FAT_CLMT 256u

struct wire_fat {
	FATFS fs;
	FIL fil;
	DWORD clmt[WIRE_FAT_CLMT];
	bool mounted;
	bool open;			/* fil is open */
	/* the log export */
	struct wire_yaffs *yaffs;
	yaffs_DIR *logs;
	char name[S14_EXPORT_NAME_MAX + 1u];
	char path[WIRE_FAT_PATH_MAX];
	char ypath[WIRE_YAFFS_PATH_MAX];
	uint8_t buf[4096];
};

/* Mounts the FAT volume of bd (S-12.020); false when there is none FatFs
 * reads. Mounting reads only. */
bool wire_fat_mount(struct wire_fat *f, const struct wb_blockdev *bd);
void wire_fat_unmount(struct wire_fat *f);

/* Formats bd as the RAM disk of dev (S-12.003-S-12.008) and mounts it. */
bool wire_fat_format(struct wire_fat *f, const struct wb_blockdev *bd,
		     const struct wb_device *dev);

/* A name S-12.012 lets the bootloader create: not empty, at most 255
 * UTF-16 units of valid UTF-8, none of U+0000-U+001F "*:<>?\|/ U+007F,
 * not ending with a space or '.'. FatFs would strip the trailing ones. */
bool wire_fat_name_ok(const char *name);

/* S-12.016: seconds since 1970 (UTC) as a FAT date and time; false for a
 * year after 2107, where the stamp is unspecified (date and time are then
 * the last stamp FAT holds). */
bool wire_fat_stamp(uint64_t utc, uint16_t *date, uint16_t *time);

/* Creates the directory `path`; true when it exists already. Each name
 * must pass wire_fat_name_ok(). */
bool wire_fat_mkdir(const struct wire_fat *f, const char *path);

/* Creates or replaces the file `path` with len bytes, stamped with the
 * fixed clock (S-12.015) or, when `stamp`, with `utc` (S-12.016). A file
 * that is not written in full is removed. */
bool wire_fat_write_file(struct wire_fat *f, const char *path,
			 const uint8_t *data, size_t len, bool stamp,
			 uint64_t utc);

/* s11_root_fn on the volume's root (S-11.046), ctx = f: names by S-12.011
 * as FatFs gives them, in directory order. */
bool wire_fat_root(void *ctx, size_t index, char *name, size_t cap,
		   bool *is_dir);

/* Opens the file `path` for reading and sets src to read it; *size is its
 * length. Close it with wire_fat_close_file(). */
bool wire_fat_open_file(struct wire_fat *f, const char *path,
			struct wire_source *src, uint32_t *size);

/*
 * The same for a package the install takes (S-11.018): src can also write
 * the file's bytes in place, so the recovered zip stays in the package's own
 * clusters. A read-only attribute is cleared first: the disk is formatted
 * afterwards (S-11.049), or the file is the bootloader's own (S-11.050).
 */
bool wire_fat_open_package(struct wire_fat *f, const char *path,
			   struct wire_source *src, uint32_t *size);
void wire_fat_close_file(struct wire_fat *f);

/* Fills ops (ops->ctx = f) with the log export from y's logs/ to this
 * volume's logs/ (S-14.051, S-14.052), which is created on the first copy.
 * y stays mounted while ops is used. */
void wire_fat_export_ops(struct wire_fat *f, struct wire_yaffs *y,
			 struct s14_export_ops *ops);

#endif /* WAFFLE_WIRE_FAT_H */
