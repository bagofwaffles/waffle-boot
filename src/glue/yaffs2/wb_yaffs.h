/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * yaffs2 on a block device, configured as S-12 section 2 states (the
 * parameters of S-12 section 3).
 */
#ifndef WAFFLE_WB_YAFFS_H
#define WAFFLE_WB_YAFFS_H

#include <stdint.h>

#include "yaffs_port.h"
#include "yaffsfs.h"
#include "yaffs_guts.h"

#include "blockdev.h"

/* Geometry (S-12.041, S-12.042). */
#define WB_YAFFS_CHUNK_BYTES 4096u
#define WB_YAFFS_SECTORS_PER_CHUNK 8u
#define WB_YAFFS_CHUNKS_PER_BLOCK 128u
#define WB_YAFFS_SECTORS_PER_BLOCK \
	(WB_YAFFS_SECTORS_PER_CHUNK * WB_YAFFS_CHUNKS_PER_BLOCK)
#define WB_YAFFS_LAST_BLOCK 0x11FFu
#define WB_YAFFS_RESERVED_BLOCKS 5u

/*
 * Sets up dev for a yaffs2 volume on blocks first_block..last_block of bd
 * (block b is sectors 0x400 * b to 0x400 * b + 0x3FF) and adds it to yaffs
 * direct under name, the path prefix it is mounted at (for example
 * "/yaffs2"). dev is zeroed first, so it must not be added already
 * (yaffs_remove_device() it before reusing it). The caller keeps dev, bd and
 * name alive. Returns 0, or -1 when the blocks are not on bd.
 */
int wb_yaffs_add_blockdev(struct yaffs_dev *dev, const char *name,
			  struct wb_blockdev *bd, uint32_t first_block,
			  uint32_t last_block);

/*
 * The clock that yaffs2 stamps object headers with (S-12.059), in seconds
 * since 1970-01-01 00:00:00 UTC. Without one, the time is 0.
 */
void wb_yaffs_set_clock(uint32_t (*now)(void));

/*
 * yaffs2's internal-error handler counts its calls here, with the place
 * of the last one.
 */
extern unsigned int wb_yaffs_bug_count;
extern const char *wb_yaffs_bug_file;
extern int wb_yaffs_bug_line;

#endif /* WAFFLE_WB_YAFFS_H */
