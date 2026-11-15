/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_WIRE_YAFFS_H
#define WAFFLE_WIRE_YAFFS_H

/*
 * The yaffs2 volume (S-12 section 2) on src/glue/yaffs2, as the S-xx modules use
 * it: struct s11_fs_ops, the staging writes of struct s11_zip_ops (with
 * src/wire_zip.c), and the yaffs2 calls of S-14's log export and S-15's
 * env_ops. Paths are relative to the mount point, as S-16.117 gives them
 * ("upgrade/progress.log" is <mount>/upgrade/progress.log).
 *
 * The mount refuses a big-endian volume (S-12.052) before yaffs2 sees it.
 * Not done here (they need yaffs2 changes, src/third_party/README.md): the
 * trusted-header rule (S-12.051) and the one header a mount writes after a
 * shadowing rename (S-12.054).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wb_yaffs.h"

#include "waffle/s11_staging.h"

#define WIRE_YAFFS_MOUNT_MAX 16u	/* mount point, NUL included */
#define WIRE_YAFFS_PATH_MAX (WIRE_YAFFS_MOUNT_MAX + 2u * S11_PATH_MAX)
/* The S-12.052 scan's buffer: a chunk and a summary's 127 entries. */
#define WIRE_YAFFS_SCAN_BYTES (WB_YAFFS_CHUNK_BYTES + 127u * 12u)

struct wire_yaffs {
	struct yaffs_dev dev;
	struct wb_blockdev *bd;
	uint32_t first_block, last_block;
	char mount[WIRE_YAFFS_MOUNT_MAX];
	bool added, mounted, refused;
	int fd;				/* the staging file being written, or -1 */
	char path[WIRE_YAFFS_PATH_MAX];	/* scratch: <mount>/<rel> */
	char path2[WIRE_YAFFS_PATH_MAX];
	/* copies; the S-12.052 scan, which reads the eMMC into it */
	_Alignas(32) uint8_t buf[WIRE_YAFFS_SCAN_BYTES];	/* the eMMC's DMA */
};

/*
 * Adds the volume on blocks first..last of bd under mount (for example
 * "/yaffs2"); nothing is read yet. The caller may adjust y->dev.param
 * before wire_yaffs_mount() (the tests lower the reserve on small volumes).
 * Returns false when the blocks are not on bd or the name is too long.
 */
bool wire_yaffs_add(struct wire_yaffs *y, const char *mount,
		    struct wb_blockdev *bd, uint32_t first, uint32_t last);

enum wire_mount {
	WIRE_MOUNT_OK,
	WIRE_MOUNT_REFUSED,	/* big-endian (S-12.052): not mounted */
	WIRE_MOUNT_FAILED,
};

/*
 * Mounts the volume read-write (S-12 section 3: the lost+found fix-up),
 * unless S-12.052 refuses it. Every operation below fails on a volume that
 * is not mounted.
 */
enum wire_mount wire_yaffs_mount(struct wire_yaffs *y);
void wire_yaffs_unmount(struct wire_yaffs *y);
/* Unmounts y and removes its device from yaffsfs; wire_yaffs_add() may
 * then be called on y again. */
void wire_yaffs_detach(struct wire_yaffs *y);

/*
 * S-12.052: the volume on blocks first..last of bd is big-endian. buf holds
 * WIRE_YAFFS_SCAN_BYTES. A read error counts as no evidence.
 */
bool wire_yaffs_big_endian(const struct wb_blockdev *bd, uint32_t first,
			   uint32_t last, uint8_t *buf);

/*
 * S-12.057: erases every block, the refused big-endian volume's included
 * (S-12.052: only once the user has confirmed it, S-15), then mounts the
 * empty volume. False when an erase or the mount fails.
 */
bool wire_yaffs_format(struct wire_yaffs *y);

/* Fills ops with the S-11 staging-area operations on y (ops->ctx = y). */
void wire_yaffs_fs_ops(struct wire_yaffs *y, struct s11_fs_ops *ops);

/* ---- single operations, for the ops structs that mix in hardware ---- */

/* <mount>/<rel> into out (WIRE_YAFFS_PATH_MAX bytes), for yaffsfs calls
 * made directly; false when it does not fit. */
bool wire_yaffs_full_path(const struct wire_yaffs *y, const char *rel,
			  char *out);
/* `rel` exists; its kind, and for a file its size. */
bool wire_yaffs_stat(struct wire_yaffs *y, const char *rel, bool *is_dir,
		     uint32_t *size);
/* Removes a file, or a directory with everything in it; true when `rel`
 * is absent (S-12.058). */
bool wire_yaffs_remove(struct wire_yaffs *y, const char *rel);
/* Creates the directory `rel` and its parents. */
bool wire_yaffs_mkdir(struct wire_yaffs *y, const char *rel);
/* Entry `index` of directory `rel` in yaffs2's listing order; false past
 * the last. name holds cap bytes, NUL included; a longer name is cut.
 * mtime may be NULL. */
bool wire_yaffs_list(struct wire_yaffs *y, const char *rel, size_t index,
		     char *name, size_t cap, bool *is_dir, uint32_t *mtime);
/* The volume's free bytes as S-12.056 bounds them; false when unknown. */
bool wire_yaffs_free_space(struct wire_yaffs *y, uint64_t *bytes);

/* The staging writes of struct s11_zip_ops (S-11.031, S-11.032). */
bool wire_yaffs_staging_reset(struct wire_yaffs *y);
bool wire_yaffs_staging_mkdir(struct wire_yaffs *y, const char *path);
bool wire_yaffs_staging_open(struct wire_yaffs *y, const char *path);
bool wire_yaffs_staging_write(struct wire_yaffs *y, const uint8_t *p,
			      size_t n);
bool wire_yaffs_staging_close(struct wire_yaffs *y);

/* For S-15's env_ops (struct s15_env_ops), paths relative as above. */
bool wire_yaffs_volume_refused(const struct wire_yaffs *y);
bool wire_yaffs_dir_exists(struct wire_yaffs *y, const char *rel);
/* Subdirectory `index` of content/dlc, files skipped. */
bool wire_yaffs_pack_dir(struct wire_yaffs *y, size_t index, char *name,
			 size_t cap);
/* S-15.041: up to max bytes of content/dlc/<dir>/manifest.json, 0 when
 * it is missing. */
size_t wire_yaffs_read_manifest(struct wire_yaffs *y, const char *dir,
				uint8_t *buf, size_t max);

#endif /* WAFFLE_WIRE_YAFFS_H */
