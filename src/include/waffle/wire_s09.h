/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_WIRE_S09_H
#define WAFFLE_WIRE_S09_H

/*
 * The slave's file layer of S-09.041 on yaffs2: the `file` and `read`
 * members of struct s09_store_ops (src/s09_slave.c) over a wire_yaffs
 * volume mounted at /yaffs2. The other members (the eMMC, the cycle
 * counter, the slave's OS status) are the board's.
 *
 * s09_store_ops has one ctx for all its members, so a board's own file()
 * and read() forward to these with its struct wire_s09; with no other
 * member to serve, ctx may be the struct wire_s09 itself.
 *
 * File descriptors are yaffsfs's; directory handles are 1 to
 * WIRE_S09_DIRS. Both stay valid until closed or wire_s09_release().
 */

#include <stdint.h>

#include "waffle/s03_emmc.h"
#include "waffle/s09_bridge.h"
#include "waffle/wire_yaffs.h"

#define WIRE_S09_DIRS 8u

struct wire_s09 {
	struct wire_yaffs *y;
	yaffs_DIR *dir[WIRE_S09_DIRS];
	char dir_path[WIRE_S09_DIRS][WIRE_YAFFS_PATH_MAX];
	char path[WIRE_YAFFS_PATH_MAX];
	/* writes that found the mount restored and had the scan made first
	 * (S-12.062); the master's own scan request comes before them */
	uint32_t implicit_scans;
};

void wire_s09_init(struct wire_s09 *s, struct wire_yaffs *y);

/* s09_store_ops.file: a file command (0x08, 0x09, 0x21-0x36 but 0x2A),
 * with the results, errno numbers (S-09.042) and records (S-09.043) of
 * S-09.041. path and path2 are full paths (s09_slave adds /yaffs2/). */
int32_t wire_s09_file(void *ctx, const struct s09_req *r, const char *path,
		      const char *path2, const uint8_t *data, uint32_t n,
		      uint8_t *out, uint32_t *err);

/* s09_store_ops.read: one piece of S-09.035, read until n bytes, the end
 * of the file or a failure. */
int32_t wire_s09_read(void *ctx, uint32_t fd, uint8_t *buf, uint32_t n,
		      uint32_t *err);

/* S-09.082: how the slave's copy of a file start's image came out. */
enum wire_os_file {
	WIRE_OS_FILE_OK,	/* read whole, its length and CRC-32 the release's */
	WIRE_OS_FILE_UNREAD,	/* not mounted, missing, a directory, or not read whole */
	WIRE_OS_FILE_LENGTH,	/* another length than the release's, or over cap */
	WIRE_OS_FILE_CRC	/* the CRC-32 of the bytes read is not the release's */
};

/*
 * S-09.082: /os.ldr of the mounted volume read whole into buf, at most cap
 * bytes, and checked against the release's len and crc: the CRC-32 by
 * crc32_fast first when it is not NULL, and by crc32 when that differs
 * (S-03.044's rule). S-09.083: when /os.ldr is not that copy, the files of
 * /os are tried in directory order, each of length len read and checked
 * the same way, and the first that passes is taken; the result is then
 * WIRE_OS_FILE_OK, else /os.ldr's. Nothing is written.
 */
enum wire_os_file wire_s09_os_file(struct wire_s09 *s, s03_crc32_fn crc32,
				   s03_crc32_fn crc32_fast, uint8_t *buf,
				   uint32_t cap, uint32_t len, uint32_t crc);

/* Closes the directory handles left open (the release, S-09.052). */
void wire_s09_release(struct wire_s09 *s);

/* A yaffsfs error (negative errno) as S-09.042 numbers it; 0 for 0. */
uint32_t wire_s09_errno(int yaffs_error);

#endif /* WAFFLE_WIRE_S09_H */
