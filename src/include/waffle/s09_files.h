/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09, the master's file client: the slave's /yaffs2 volume (S-12) and its
 * eMMC (S-03), reached through the bridge's file and eMMC commands
 * (S-09.040, S-09.041). On top of them: the staging area's s11_fs_ops
 * (S-11.080), the file operations S-15's menus need (packs, trees, the
 * factory reset's erase, the format, the OS write) and the settings file
 * (S-15.172).
 *
 * Every request goes through s09_master_call(); with the slave absent each
 * one fails with ENODEV (S-09.008) and the functions below report failure.
 * Paths are passed on as given: a relative one is under /yaffs2/
 * (S-09.041).
 *
 * The master's disk install (S-11 checks 16 and 17, the run)
 * stages into the slave's /yaffs2/upgrade/ through this client, so nothing
 * is staged in the master's DDR; the zip is read where checks 1 - 15
 * recovered it. Wiring, with Z a struct wire_zip over the package P:
 *
 *   wire_zip_init(&Z, &reader, &P, NULL, 0, NULL);  (fs is not used)
 *   s09_files_zip_ops(&SZ, &files, &reader, &zip);  install_ops.zip = &zip
 *   s09_files_fs_ops(&files, &fs);                  install_ops.fs
 *   run_ops.record_write -> s09_files_record_write(&files, step)
 *
 * with /yaffs2 mounted (s09_files_mount()) first. The recovered zip
 * (wire_package's zip buffer) and minizip's heap are the caller's memory:
 * while the package is on the RAM disk they must lie outside it (S-01.016,
 * RAM disk part 1 from 0x8A000000).
 */
#ifndef WAFFLE_S09_FILES_H
#define WAFFLE_S09_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s03_emmc.h"
#include "waffle/s09_bridge.h"
#include "waffle/s09_master.h"
#include "waffle/s11_staging.h"
#include "waffle/s15_menu.h"

/* The longest path sent, NUL included: /yaffs2/, a pack's directory and
 * its manifest, or an S-11 path. */
#define S09_FILES_PATH_MAX 512u

/* S-03.070: the yaffs2 region's erase groups, all erased by the format. */
#define S09_FILES_YAFFS2_GROUPS ((S03_YAFFS2_LAST + 1u) / S03_GROUP_SECTORS)

struct s09_files {
	struct s09_master *m;
	uint32_t err;		/* the last failed request's errno (S-09.042) */
	bool mount_sent;	/* a mount (0x08) has been sent */
	bool mount_failed;	/* the last one's result was negative */
	bool mount_lost;	/* the last one met an absent slave (ENODEV) */
	bool mounted;		/* the last one mounted, and no 0x09 since */
	/* S-09.041: how it was mounted (S-12.061; 0: no checkpoint read) and
	 * the objects of its tree */
	uint8_t way;
	uint32_t objects;
	char path[S09_FILES_PATH_MAX];	/* the tree walk's and pack's path */
	uint8_t req[2u * S09_FILES_PATH_MAX];	/* a rename's payload */
	uint8_t rec[S09_DIRENT_LEN];
	uint8_t buf[S09_READ_CHUNK];	/* a copy's piece */
	int32_t stage_fd;		/* the staging file open, -1: none */
	uint32_t stage_sent;		/* bytes of its write left running, 0: none */
	char stage[S09_FILES_PATH_MAX];	/* its path, upgrade/... */
	/* The directory the staging area's last open or mkdir made, with every
	 * one above it: a path under it sends no mkdir for those. Forgotten
	 * when anything is removed, renamed or mounted. */
	char made[S09_FILES_PATH_MAX];
	size_t made_len;
	/* S-09.075: the rung a staging file's open brings the link back up
	 * to after a fall-back; 0: never asked for one. */
	uint8_t link;
};

void s09_files_init(struct s09_files *f, struct s09_master *m);

/* ---- the requests (S-09.041) ---- */

/* 0x08: 0 or more when /yaffs2 is mounted (way and objects set),
 * negative when the mount failed. */
int32_t s09_files_mount(struct s09_files *f);
/* The ways of S-09.041's 0x08 result (S-12.061). */
#define S09_FILES_RESTORED 1u
#define S09_FILES_FOR_WRITE 6u
/* The volume is mounted from the OS's checkpoint, read only. */
bool s09_files_restored(const struct s09_files *f);
/*
 * S-12.062: the scan before the session's first write. On a restored
 * mount, 0x08 with arg0 1 (its reply after the scan, within T_EXEC);
 * nothing otherwise. A failed mount then counts as any failed mount. True
 * when the volume is mounted by a scan afterwards.
 */
bool s09_files_for_write(struct s09_files *f);
/* 0x09: true when the slave answered, with a result not negative. The
 * volume counts as unmounted from then on, whatever the answer. */
bool s09_files_unmount(struct s09_files *f);

/* 0x22: 0 and *st, or -1 (f->err). */
int32_t s09_files_stat(struct s09_files *f, const char *path, struct s09_stat *st);
/* 0x23: 0 and *st, or -1. */
int32_t s09_files_statfs(struct s09_files *f, const char *path, struct s09_statfs *st);
/* 0x29: a file descriptor, or -1. flags are S09_O_*. */
int32_t s09_files_open(struct s09_files *f, const char *path, uint32_t flags);
int32_t s09_files_close(struct s09_files *f, int32_t fd);
/* The close, then 0x04: 0 once the eMMC has the file in its nonvolatile
 * memory. The eMMC's cache is on (S-03.009), so a closed file alone does not
 * survive a power cut (S-03.068). */
int32_t s09_files_close_sync(struct s09_files *f, int32_t fd);
/* S-09.035: up to n bytes; the count read, or -1. */
int32_t s09_files_read(struct s09_files *f, int32_t fd, uint8_t *buf, uint32_t n);
/* 0x2B in pieces of at most 0x1000: n, or -1 when a piece failed or was
 * written short. */
int32_t s09_files_write(struct s09_files *f, int32_t fd, const uint8_t *buf, uint32_t n);
int32_t s09_files_unlink(struct s09_files *f, const char *path);
int32_t s09_files_mkdir(struct s09_files *f, const char *path);
int32_t s09_files_rmdir(struct s09_files *f, const char *path);
int32_t s09_files_rename(struct s09_files *f, const char *from, const char *to);

/*
 * Entry `index` of a directory, "." and ".." left out, by opening it
 * afresh (0x2E, 0x2F, 0x30). False past the last entry or on failure;
 * name holds cap bytes, NUL included (a longer name is a failure). *type is
 * the dirent's type (4 directory, 8 file, 10 symbolic link).
 */
bool s09_files_entry(struct s09_files *f, const char *dir, size_t index,
		     char *name, size_t cap, uint8_t *type);
/*
 * Every entry of a directory, "." and ".." left out, in one opening of it:
 * each(ctx, name) for each until it returns false. False on a failure
 * (f->err); the directory stays open while each() runs, so each() may make
 * other requests but must not list.
 */
bool s09_files_list(struct s09_files *f, const char *dir,
		    bool (*each)(void *ctx, const char *name), void *ctx);

/* ---- built on them ---- */

/* Exists, as a directory. */
bool s09_files_dir_exists(struct s09_files *f, const char *path);
/* Creates the directories `path` names before its last element. */
bool s09_files_mkdirs(struct s09_files *f, const char *path);
/* A file, or a directory with everything in it; true when it is gone,
 * also when it was missing. */
bool s09_files_remove_tree(struct s09_files *f, const char *path);
/* Replaces a file at `to`, creating the directories it implies. */
bool s09_files_copy(struct s09_files *f, const char *from, const char *to);
bool s09_files_move(struct s09_files *f, const char *from, const char *to);
/* The first cap bytes of a file; the count read, 0 when it cannot be read. */
size_t s09_files_read_file(struct s09_files *f, const char *path, uint8_t *buf, size_t cap);
/* A whole file of at most cap bytes into buf: false when it is missing,
 * longer than cap or not read whole. */
bool s09_files_read_whole(struct s09_files *f, const char *path, uint8_t *buf,
			  uint32_t cap, uint32_t *len);
/* The staging area's operations, with ctx f. */
void s09_files_fs_ops(struct s09_files *f, struct s11_fs_ops *ops);

/* ---- S-11's staging writes (checks 16 and 17, the progress record) ---- */

/*
 * S-11.033: the volume's free bytes, from 0x23 (free blocks x 512, S-09.043);
 * false when it cannot be read. Its counts are S-12.056's, as 0x31's are
 * (S-09.043, S-09.041). Not 0x31, whose errno is the volume's last error
 * (S-09.041), which an earlier request's ENOENT leaves set, so its failure
 * cannot be told from its success.
 */
bool s09_files_free_space(struct s09_files *f, uint64_t *bytes);
/* As struct s11_zip_ops's, path relative to upgrade/ (S-11.031): */
/* removes upgrade/ with everything in it, then creates it empty; */
bool s09_files_staging_reset(struct s09_files *f);
/* creates upgrade/<path> and its parents (a trailing '/' is allowed); */
bool s09_files_staging_mkdir(struct s09_files *f, const char *path);
/* creates upgrade/<path> empty, with its parents, for the writes until the
 * close; false while another is open; */
bool s09_files_staging_open(struct s09_files *f, const char *path);
bool s09_files_staging_write(struct s09_files *f, const uint8_t *p, size_t n);
/* false when nothing is open or the close fails. */
bool s09_files_staging_close(struct s09_files *f);
/* S-11.062: replaces upgrade/progress.log with {step, 0}, for
 * struct s11_run_ops's record_write. */
bool s09_files_record_write(struct s09_files *f, uint8_t step);

/*
 * struct s11_zip_ops for checks 16 and 17: member, zip64_end, extract and
 * crc32 forwarded to `reader` (a copy is kept; wire_zip's, say), the free
 * space and the staging writes above on f. ops->ctx = z; the caller keeps
 * z, f and reader's ctx alive while ops is used.
 */
struct s09_files_zip {
	struct s09_files *f;
	struct s11_zip_ops reader;
};

void s09_files_zip_ops(struct s09_files_zip *z, struct s09_files *f,
		       const struct s11_zip_ops *reader, struct s11_zip_ops *ops);

/* ---- S-15's file operations (struct s15_env_ops) ---- */

/*
 * The volume is refused (S-12.052): the last mount the master sent failed.
 * The bridge reports a refused volume and a failed mount alike (S-09.041's
 * 0x08 is negative for both), so a failed mount is taken as refused.
 */
bool s09_files_volume_refused(const struct s09_files *f);
/* The slave stopped answering (S-09.008): absent for the rest of the boot,
 * so no request reaches the volume, whatever its mount said. Not a refused
 * volume: a mount that met it refused nothing (mount_lost). */
bool s09_files_slave_lost(const struct s09_files *f);
/* Subdirectory `index` of /yaffs2/content/dlc, files and "." and ".."
 * skipped. */
bool s09_files_pack_dir(struct s09_files *f, size_t index, char name[S15_NAME_MAX + 1u]);
/* /yaffs2/content/dlc/<dir>/manifest.json, its first max bytes; 0 when
 * missing (S-15.041). */
size_t s09_files_read_manifest(struct s09_files *f, const char *dir, uint8_t *buf, size_t max);
/* S-15.052, S-03.073: the first erase group of the OS's FAT volume. */
bool s09_files_erase_os_fat(struct s09_files *f);
/* S-15.072, S-12.057: unmount, select the user area, erase every erase
 * group of the yaffs2 region in order (a failed erase does not stop the
 * rest; a failed select erases nothing), then mount again in every case.
 * False for a failed format: the select or an erase failed. */
bool s09_files_format(struct s09_files *f);
/*
 * S-15.094: read /yaffs2/os.ldr whole into image (cap bytes, at least
 * S03_OS_MAX: a longer file is one S-03.051 refuses) and write it with
 * S-03's OS write. env->ops is usually s09_files_emmc_ops()'s. Each step
 * is reported to rep as it goes (NULL: none), and *end says how it ended;
 * the result is end->result.
 */
enum s15_os_write s09_files_os_write(struct s09_files *f, const struct s03_env *env,
				     uint8_t *image, uint32_t cap,
				     const struct s15_osw_report *rep,
				     struct s15_osw_end *end);

/* ---- the eMMC (S-09.040) ---- */

/*
 * S-03's eMMC operations as bridge requests, with ctx f: select 0x18,
 * read 0x11/0x12 and write 0x15/0x16 in pieces of at most 128 sectors,
 * erase 0x14. The bridge has no switch command; switch_byte fails.
 */
void s09_files_emmc_ops(struct s09_files *f, struct s03_emmc_ops *ops);
/* 0x05: the boot-partition size B (S-03.008), 0 without a reply. */
uint32_t s09_files_boot_size(struct s09_files *f);

/* ---- the settings file (S-15.172) ---- */

/* The settings S15_SETTINGS_PATH gives; the defaults when it is missing,
 * unusable or cannot be read, or the volume is refused. */
void s09_files_settings_read(struct s09_files *f, struct s15_settings *s);
/* The whole file to S15_SETTINGS_NEW_PATH, then renamed over
 * S15_SETTINGS_PATH, so a power cut leaves the old file or the new, then the
 * eMMC's cache flushed (0x04), so the new one is kept. False when a step
 * failed. */
bool s09_files_settings_save(struct s09_files *f, const struct s15_settings *s);

#endif
