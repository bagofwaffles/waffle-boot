/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A device of 512-byte sectors, as the filesystem glue sees it: the RAM
 * disk (S-12.001) or the eMMC user area (S-03).
 */
#ifndef WAFFLE_BLOCKDEV_H
#define WAFFLE_BLOCKDEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WB_SECTOR_SIZE 512u

/* Every function returns 0 on success, nonzero on failure. */
/* Every buffer passed to read and write starts on a 32-byte boundary, as
 * the eMMC's DMA needs (S-13b.054); the RAM disk takes any 4-byte aligned
 * one. */
struct wb_blockdev {
	void *ctx;
	uint32_t n_sectors;
	int (*read)(void *ctx, uint32_t lba, void *buf, uint32_t count);
	int (*write)(void *ctx, uint32_t lba, const void *buf, uint32_t count);
	/*
	 * Makes count sectors from lba read as zero (an eMMC erase,
	 * S-03.003). NULL: the glue writes zeros instead.
	 */
	int (*erase)(void *ctx, uint32_t lba, uint32_t count);
	/*
	 * Whether that erase leaves zeros (the eMMC's EXT_CSD ERASED_MEM_CONT,
	 * read at its bring-up); NULL: it does. When it does not, the glue
	 * writes zeros in place of the erase.
	 */
	bool (*erase_zeroes)(void *ctx);
	/*
	 * Writes count sectors from lba so that a power loss leaves each
	 * either as it was or as written (an eMMC reliable write, S-12.060);
	 * yaffs2's chunks go this way. NULL: write is used.
	 */
	int (*write_reliable)(void *ctx, uint32_t lba, const void *buf,
			      uint32_t count);
};

/*
 * Copies len bytes that do not overlap: a word at a time when both ends and
 * len are 4-byte aligned, as sectors and their buffers are, which is several
 * times faster on the target than the C library's byte loop.
 */
void wb_blockdev_copy(void *dst, const void *src, size_t len);

/* A RAM disk of n_sectors sectors at mem. */
struct wb_ramdisk {
	uint8_t *mem;
	uint32_t n_sectors;
};

/* Sets up dev to access rd, which is set to mem and n_sectors. */
void wb_ramdisk_init(struct wb_blockdev *dev, struct wb_ramdisk *rd,
		     void *mem, uint32_t n_sectors);

#endif /* WAFFLE_BLOCKDEV_H */
