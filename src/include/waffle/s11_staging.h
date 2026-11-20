/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_S11_STAGING_H
#define WAFFLE_S11_STAGING_H

/*
 * S-11 section 3: the upgrade zip's members, the staging area and the
 * components it holds. The zip reader and inflate (vendored) and the yaffs2
 * volume come in through struct s11_zip_ops and struct s11_fs_ops.
 *
 * Paths given to struct s11_fs_ops are relative to /yaffs2/ (S-16.117);
 * staging paths are under "upgrade/" (S-11 terms).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s11_common.h"
#include "waffle/s11_version.h"

#define S11_PATH_MAX 256u	/* longest path handled, NUL included */

/* The components of S-11.034. */
enum s11_component {
	S11_C_OS,
	S11_C_BOOTLOADER,
	S11_C_BLE,
	S11_C_NORDIC_BL0,
	S11_C_NORDIC_BL1,
	S11_C_BACKPLANE,
	S11_COMPONENTS,
};

struct s11_component_info {
	const char *image;	/* path in the zip */
	const char *version;	/* its version file */
	uint32_t limit;		/* S-11.035 */
};

extern const struct s11_component_info s11_components[S11_COMPONENTS];

#define S11_CONTENT_DIR "firmware/blackfin/content/"
#define S11_FW_VERSION "firmware/firmware.version"

/* ---- checks 16 and 17 (S-11.030-S-11.035) ---- */

struct s11_member {
	char name[S11_PATH_MAX];	/* NUL-terminated */
	uint16_t method;
	uint16_t flags;			/* general-purpose flags */
	uint32_t size;			/* stated uncompressed size */
	uint32_t crc32;			/* stated CRC-32 */
	bool zip64;			/* a zip64 extra field */
};

typedef bool (*s11_sink_fn)(void *arg, const uint8_t *p, size_t n);

struct s11_zip_ops {
	/* Member `index` in archive order; false past the last. */
	bool (*member)(void *ctx, size_t index, struct s11_member *m);
	/* The archive has zip64 end records. */
	bool (*zip64_end)(void *ctx);
	/* The member's data, stored or inflated (window at most 32 KiB), to
	 * `sink` in pieces; false when it cannot be read or inflated in
	 * full, or when `sink` fails. */
	bool (*extract)(void *ctx, size_t index, s11_sink_fn sink, void *arg);
	/* CRC-32 (ISO 3309) continued from `crc` (0 to start). */
	uint32_t (*crc32)(void *ctx, uint32_t crc, const uint8_t *p, size_t n);
	/* The yaffs2 volume's free bytes; false when unknown. */
	bool (*free_space)(void *ctx, uint64_t *bytes);
	/* Removes upgrade/ with everything in it, then creates it empty. */
	bool (*staging_reset)(void *ctx);
	/* Creates the directory upgrade/<path> and its parents. */
	bool (*staging_mkdir)(void *ctx, const char *path);
	/* Creates upgrade/<path> empty, with the directories it implies,
	 * then appends to it until staging_close. */
	bool (*staging_open)(void *ctx, const char *path);
	bool (*staging_write)(void *ctx, const uint8_t *p, size_t n);
	bool (*staging_close)(void *ctx);
	void *ctx;
};

/*
 * The sizes of the component images staged (0: absent, S-11.034) and
 * their CRC-32s as the zip states them (S-11.030): the stated values the
 * bootloader keeps for the run that staged the package, for S-11.095's
 * check of each image a step reads back.
 */
struct s11_staged_sizes {
	uint32_t size[S11_COMPONENTS];
	uint32_t crc32[S11_COMPONENTS];
};

/* S-11.033: the free space against the members' total plus 0x400000. */
enum s11_reason s11_space_rule(uint64_t total, bool known, uint64_t free);

/* S-11.031: a member name the staging area may take. */
bool s11_member_name_ok(const char *name);

/* Checks 16 and 17: refuses or stages the zip's members. */
enum s11_reason s11_check_zip(const struct s11_zip_ops *ops,
			      struct s11_staged_sizes *staged);

/* ---- S-11.095: a staged image is checked before a step uses it ---- */

/* One read of a staged image: `ok` false when the read reported an error
 * or its session did not complete (S-09.013); then len and crc32 are not
 * used. Also the member's stated values (S-11.030), with ok true. */
struct s11_staged_read {
	bool ok;
	uint32_t len;
	uint32_t crc32;
};

enum s11_staged_check {
	S11_STAGED_OK,		/* the bytes of the first read may be used */
	S11_STAGED_BAD,		/* the check failed: Bad file */
	S11_STAGED_READ_ERROR,	/* a read failed: Internal error */
};

/*
 * S-11.095. `stated`: the member's size and CRC-32 (S-11.030), kept for the
 * run from the extraction. The bytes a step writes or sends are the read's,
 * and only on S11_STAGED_OK.
 */
enum s11_staged_check s11_staged_check(const struct s11_staged_read *stated,
				       const struct s11_staged_read *read);
/* The step's result for a check that did not pass: Bad file or Internal
 * error (S-11.058, S-11.067-S-11.071); S11_OK for S11_STAGED_OK. */
enum s11_result s11_staged_result(enum s11_staged_check c);

/* ---- the staging area as a run reads it ---- */

struct s11_fs_ops {
	/* Whether `path` exists; its kind and, for a file, its size. */
	bool (*stat)(void *ctx, const char *path, bool *is_dir, uint32_t *size);
	/* The first `cap` bytes of a file; returns how many were read. */
	size_t (*read)(void *ctx, const char *path, uint8_t *buf, size_t cap);
	/* Entry `index` of a directory in listing order; false past the
	 * last. `name` holds `cap` bytes, NUL included. */
	bool (*list)(void *ctx, const char *dir, size_t index, char *name,
		     size_t cap, bool *is_dir);
	/* Removes a file, or a directory with everything in it. */
	bool (*remove)(void *ctx, const char *path);
	/* Moves a file to `to`, replacing a file there and creating the
	 * directories `to` implies. */
	bool (*move)(void *ctx, const char *from, const char *to);
	/* Copies a file to `to`, replacing it. */
	bool (*copy)(void *ctx, const char *from, const char *to);
	void *ctx;
};

/* What the staging area holds (S-11.034, S-11.036, S-11.038, S-11.056). */
struct s11_staged {
	bool exists;				/* upgrade/ exists */
	uint32_t size[S11_COMPONENTS];		/* 0: absent */
	struct s11_version version[S11_COMPONENTS];
	bool content_dir;			/* upgrade/firmware/blackfin/content/ */
	bool has_fw_version;			/* upgrade/firmware/firmware.version */
	struct s11_version fw_version;
};

void s11_staged_scan(const struct s11_fs_ops *fs, struct s11_staged *st);

bool s11_staged_present(const struct s11_staged *st, enum s11_component c);
/* No component present and a content directory (S-11.056). */
bool s11_staged_content_only(const struct s11_staged *st);
/* The Nordic application or either Nordic bootloader (S-11.061). */
bool s11_staged_nordic(const struct s11_staged *st);

#endif /* WAFFLE_S11_STAGING_H */
