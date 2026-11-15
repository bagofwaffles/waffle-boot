/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_WIRE_ZIP_H
#define WAFFLE_WIRE_ZIP_H

/*
 * S-11 checks 16 and 17 on minizip (src/glue/minizip/unzip_mem) and zlib: struct
 * s11_zip_ops over the zip in RAM, staging into the yaffs2 volume through
 * src/wire_yaffs.c.
 *
 * s11_install() runs checks 16 and 17 right after check 15 has recovered the
 * zip, so the archive is opened on the first call and its length taken from
 * the package then. minizip allocates while it is open: call
 * wire_zip_close() when the install is over.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unzip_mem.h"

#include "waffle/s11_staging.h"
#include "waffle/wire_package.h"
#include "waffle/wire_yaffs.h"

#define WIRE_ZIP_EXTRA_MAX 1024u	/* extra field read in place; longer is malloc()ed */

struct wire_zip {
	const struct wire_package *pkg;	/* the zip checks 1-15 recovered, */
	const uint8_t *buf;		/* or, without a package, this one */
	size_t len;
	struct wire_yaffs *fs;		/* the staging area, the free space */
	struct wb_unzip_mem mem;
	unzFile uf;
	bool opened;			/* the open was tried */
	bool at_valid;			/* minizip is at member `at` */
	size_t at;
	uint8_t extra[WIRE_ZIP_EXTRA_MAX];
	uint8_t data[4096];		/* the end of the zip, searched */
};

/*
 * Sets up z and fills ops (ops->ctx = z) for the zip pkg recovers or, with
 * pkg NULL, the len bytes at buf. The caller keeps pkg or buf, and fs,
 * alive while ops is used.
 */
void wire_zip_init(struct wire_zip *z, struct s11_zip_ops *ops,
		   const struct wire_package *pkg, const uint8_t *buf,
		   size_t len, struct wire_yaffs *fs);

/* Closes the archive; ops may be used again (it reopens). */
void wire_zip_close(struct wire_zip *z);

#endif /* WAFFLE_WIRE_ZIP_H */
