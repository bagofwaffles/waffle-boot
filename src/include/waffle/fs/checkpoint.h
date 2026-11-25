/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_FS_CHECKPOINT_H
#define WAFFLE_FS_CHECKPOINT_H

/*
 * A read-only test of the volume's yaffs2 checkpoint (the bootloader's own
 * mount restores it, S-12.061, with the same stale check, wire_ckpt_stale()
 * below; the test compares the trees too): a second device on the volume's
 * blocks, mounted read only with the checkpoint restored, its tree compared
 * with the scanned mount's, then every block's chunk-0 tags read against
 * the block states and sequence number the checkpoint records. The test
 * device sits on a block device that passes reads through and refuses, and
 * counts, every write, reliable write and erase, so nothing is written
 * whatever yaffs2 tries; read only, yaffs2 tries nothing (fs/checkpoint.c).
 */

#include <stdbool.h>
#include <stdint.h>

#include "blockdev.h"

#define WIRE_CKPT_KEPT 4u	/* differences and disagreements told in full */
#define WIRE_CKPT_TEXT 96u
#define WIRE_CKPT_OBJS 65536u	/* objects compared at most, per tree */

/* An object as a tree walk sees it: its type as a dirent's (4 directory,
 * 8 file, 10 symbolic link, 0 other), its id, size and parent's id. */
struct wire_ckpt_obj {
	uint32_t ino, type, size, parent;
};

/*
 * The scanned mount's tree, paths relative to the volume ("" its root,
 * "a/b" below it). list() calls each() once per entry of dir, "." and ".."
 * left out, and returns false when dir cannot be read; stat() fills o
 * (parent left to the walk).
 */
struct wire_ckpt_tree_ops {
	void *ctx;
	bool (*list)(void *ctx, const char *dir,
		     bool (*each)(void *ectx, const char *name), void *ectx);
	bool (*stat)(void *ctx, const char *path, struct wire_ckpt_obj *o);
};

enum wire_ckpt_outcome {
	WIRE_CKPT_DONE,
	WIRE_CKPT_NO_MEMORY,	/* the test device's memory is not there */
	WIRE_CKPT_NOT_MOUNTED,	/* the test mount failed */
	WIRE_CKPT_BAD_VOLUME,	/* the blocks are not on the device */
};

struct wire_ckpt_report {
	enum wire_ckpt_outcome outcome;
	/* the checkpoint: its blocks on the volume (chunk 0 tagged as
	 * checkpoint data), whether yaffs2 restored the mount from it, and
	 * when not why, as far as its first chunk tells */
	uint32_t ckpt_blocks;
	bool accepted;
	char why[WIRE_CKPT_TEXT];
	/* the test mount: its reads (sectors) and time */
	uint32_t mount_reads, mount_sectors, mount_us;
	/* every write, reliable write and erase the test device was given,
	 * all refused; and whether its unmount returned */
	uint32_t writes_refused;
	bool unmounted;
	/* the trees: their objects, the differences, the first in full; a
	 * tree that did not fit WIRE_CKPT_OBJS, or whose paths were too long,
	 * is cut (*_cut) */
	uint32_t objs_scan, objs_ckpt, diffs;
	bool scan_cut, ckpt_cut;
	char diff[WIRE_CKPT_KEPT][WIRE_CKPT_TEXT];
	/* the stale guard, over every block's chunk-0 tags: the checkpoint's
	 * sequence number, allocating block and page; the blocks whose
	 * sequence number is above it, whether the allocating block's next
	 * page is in use, and the blocks whose tags disagree with the state
	 * the checkpoint records, the first in full */
	uint32_t ckpt_seq;
	int32_t alloc_block;
	uint32_t alloc_page;
	uint32_t newer;
	bool alloc_used;
	uint32_t disagree;
	char disagreement[WIRE_CKPT_KEPT][WIRE_CKPT_TEXT];
	/* the guard's tags reads that failed (not in the summary) */
	uint32_t read_failed;
	/* the whole test's time */
	uint32_t test_us;
};

/*
 * The test on blocks first..last of bd, against the scanned mount's tree.
 * now_us gives the time (for the report). The test device is named
 * "/ckpt"; it is added, mounted, unmounted and removed here, and no other
 * device is touched. Returns the outcome, also in r.
 */
enum wire_ckpt_outcome wire_ckpt_run(const struct wb_blockdev *bd, uint32_t first,
				     uint32_t last, const struct wire_ckpt_tree_ops *scan,
				     uint64_t (*now_us)(void), struct wire_ckpt_report *r);

/* The test device's write side, refusing (for the tests): returns -1 and
 * counts the attempt into the report of the run under way, or of the
 * last. */
int wire_ckpt_refused_write(void *ctx, uint32_t lba, const void *buf, uint32_t count);

/*
 * The stale check of a mount restored from a checkpoint (S-12.061), the
 * test's guard and the bootloader's mount alike: chunk 0's tags of every
 * block read against the state and sequence number the restored mount
 * records for it, and every page of the allocating block from the
 * allocating page on, each of which must be unused. A block agrees when
 *   - its chunk 0 is checkpoint data and the mount records a checkpoint
 *     block, or
 *   - the mount records it empty and chunk 0 is unused, or
 *   - the mount records it allocating, full, dirty or being collected and
 *     chunk 0 is in use with the sequence number recorded;
 * any other pairing disagrees (a dead block among them: the eMMC has
 * none). Counted apart: data blocks newer than the checkpoint's sequence
 * number, used pages at or after the allocating page, and tags reads
 * that failed. d is mounted; nothing is written.
 */
struct yaffs_dev;
struct yaffs_block_info;
struct wire_ckpt_stale {
	uint32_t ckpt_seq;	/* the restored mount's sequence number */
	int32_t alloc_block;	/* the volume's numbering; -1: none */
	uint32_t alloc_page;
	uint32_t blocks;	/* blocks whose chunk 0 was read */
	uint32_t ckpt_blocks;	/* of them, checkpoint data */
	uint32_t newer;
	uint32_t disagree;
	uint32_t alloc_used;	/* pages of the allocating block in use */
	uint32_t failed;	/* tags reads that failed */
};
/* Each disagreeing block (the volume's numbering), with what the mount
 * records and what its chunk 0 holds; NULL: not told. */
typedef void (*wire_ckpt_disagree_fn)(void *ctx, uint32_t block,
				      const struct yaffs_block_info *bi, bool used,
				      uint32_t seq);
/* True when the checkpoint cannot be trusted: d not restored from one, or
 * any disagreement, newer block, used page or failed read. */
bool wire_ckpt_stale(struct yaffs_dev *d, struct wire_ckpt_stale *s,
		     wire_ckpt_disagree_fn each, void *ctx);

#endif /* WAFFLE_FS_CHECKPOINT_H */
