/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The yaffs2 build configuration. Included first by every yaffs2 source
 * (the build passes -include) and by wb_yaffs.h, so that the library and
 * its users see the same structures.
 */
#ifndef WAFFLE_YAFFS_PORT_H
#define WAFFLE_YAFFS_PORT_H

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* yaffs direct, yaffs2 only: every device is set up with is_yaffs2
 * (yaffs_blockdev.c, S-12.040), so the yaffs1 scan is left out. */
#define CONFIG_YAFFS_DIRECT 1
#define CONFIG_YAFFS_YAFFS2 1
#define CONFIG_YAFFS_NO_YAFFS1 1
#define CONFIG_YAFFS_DEFINES_TYPES 1
#define CONFIG_YAFFS_PROVIDE_DEFS 1
#define CONFIG_YAFFSFS_PROVIDE_VALUES 1

/* Erased media reads as zero (S-12.043; a local yaffs2 change). */
#define CONFIG_YAFFS_ERASED_ZERO 1

/*
 * A mount erases no checkpoint block; the first write of the session does
 * (S-12.054, S-12.055; a local yaffs2 change).
 */
#define CONFIG_YAFFS_LAZY_CHECKPT_ERASE 1

/*
 * A checkpoint block that fails to erase stays a checkpoint, and while one
 * is left no chunk is written and no other block erased (S-12.055; a local
 * yaffs2 change).
 */
#define CONFIG_YAFFS_CHECKPT_ERASE_FIRST 1

/*
 * A mount deletes and erases nothing; the first write of the session
 * finishes the deletions the scan found (S-12.054; a local yaffs2 change).
 */
#define CONFIG_YAFFS_LAZY_MOUNT_CLEANUP 1

/* No trace output: the target has no printf. */
#define CONFIG_YAFFS_NO_TRACE 1

/*
 * yaffs2 uses the Linux type loff_t for file offsets. picolibc has none,
 * and glibc's needs _GNU_SOURCE; give yaffs2 one of its own, after the
 * C library headers above so that none of them sees the macro.
 */
typedef long long wb_yaffs_loff_t;
#define loff_t wb_yaffs_loff_t

/*
 * n / d as C rounds it, for a d above 1, without the compiler's 64-bit
 * signed division (__divdi3 on the BF70x): yaffsfs_DoStat()'s st_blocks,
 * a local yaffs2 change.
 */
#include "waffle/util/div64.h"

static inline long long wb_yaffs_div(long long n, unsigned long d)
{
	uint64_t mag = n < 0 ? 0u - (uint64_t)n : (uint64_t)n;
	uint64_t q = wb_udiv64_32(mag, (uint32_t)d);

	return n < 0 ? -(long long)q : (long long)q;
}

#endif /* WAFFLE_YAFFS_PORT_H */
