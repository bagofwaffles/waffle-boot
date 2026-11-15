/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * minizip on a zip archive in memory, or read in place through a callback
 * (the upgrade zip on the RAM disk, S-11.018, S-11.030).
 */
#ifndef WAFFLE_UNZIP_MEM_H
#define WAFFLE_UNZIP_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unzip.h"

/*
 * The state of one open archive; the caller keeps it while it is open. The
 * archive is the len bytes at buf or, with buf NULL, the len bytes that
 * read() gives from offset base on (a file on the RAM disk, S-11.018).
 */
struct wb_unzip_mem {
	const uint8_t *buf;
	size_t len;
	size_t pos;
	bool (*read)(void *ctx, uint32_t off, uint8_t *buf, size_t n);
	void *ctx;
	uint32_t base;
};

/*
 * Opens the len-byte archive at buf for reading with minizip's unz*()
 * functions. Returns NULL when minizip cannot read it as a zip archive.
 * Close it with unzClose().
 */
unzFile wb_unzip_open_mem(struct wb_unzip_mem *m, const void *buf, size_t len);

/*
 * The same for the len bytes read(ctx, base + k, ...) gives, k from 0; a
 * read that fails is a short read to minizip. base + len must not pass
 * 2^32.
 */
unzFile wb_unzip_open_read(struct wb_unzip_mem *m,
			   bool (*read)(void *ctx, uint32_t off, uint8_t *buf,
					size_t n),
			   void *ctx, uint32_t base, size_t len);

#endif /* WAFFLE_UNZIP_MEM_H */
