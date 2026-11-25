/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-14.051, S-14.052: the log export, the copy of the yaffs2 directory
 * logs/ to the USB disk's logs/. The bootloader writes no log file of its
 * own (S-14.050); this is the only file the log reaches.
 *
 * The file systems come in through s14_export_ops; the selection, the
 * 5 MiB limit and the clean-up of a failed copy are here.
 */
#ifndef WAFFLE_LOG_EXPORT_H
#define WAFFLE_LOG_EXPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define S14_EXPORT_LIMIT    (5u * 1024u * 1024u)
/* Longest name kept: yaffs2's limit. A longer name is not a candidate. */
#define S14_EXPORT_NAME_MAX 255u

struct s14_export_entry {
	const char *name;  /* NUL-terminated; valid until the next call */
	bool regular;      /* a regular file (not a directory or other) */
	uint32_t mtime;    /* modification time, seconds */
	uint32_t size;     /* bytes */
};

struct s14_export_ops {
	/* Start listing the yaffs2 directory logs/ from its first entry. */
	void (*rewind)(void *ctx);
	/* The next entry directly in logs/, in any order; false at the end,
	 * or at once when logs/ does not exist. */
	bool (*next)(void *ctx, struct s14_export_entry *e);
	/* Copy yaffs2 logs/<name> to the USB disk's logs/<name> with the
	 * same contents and modification time (S-12's FAT time stamp).
	 * Returns false on any failure, including a name the FAT volume
	 * refuses (S-16.162). */
	bool (*copy)(void *ctx, const char *name, uint32_t mtime);
	/* Remove the USB disk's logs/<name>, if any, after a failed copy. */
	void (*discard)(void *ctx, const char *name);
	void *ctx;
};

/* Run the export. Returns the number of files copied. */
size_t s14_export_run(const struct s14_export_ops *ops);

#endif /* WAFFLE_LOG_EXPORT_H */
