/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_INSTALL_CONTENT_H
#define WAFFLE_INSTALL_CONTENT_H

/*
 * S-11 sections 3, 4 and 6: content packs, the partial marker, the content
 * step, and which RAM-disk file is the package. The yaffs2 volume comes in
 * through struct s11_fs_ops (paths relative to /yaffs2/); the RAM disk's
 * root directory through a listing callback.
 */

#include <stdbool.h>
#include <stddef.h>

#include "waffle/install/common.h"
#include "waffle/install/staging.h"

struct wb_device;

/* The content step, step 0 (S-11.066, S-11.075-S-11.079, S-11.040; on a
 * device without content-packs S-11.097). `content_only` is S-11.056's,
 * decided from the staging area. */
enum s11_result s11_install_content(const struct wb_device *dev,
				    const struct s11_fs_ops *fs,
				    bool content_only);

/* The staging area holds a partial marker (S-11.040). */
bool s11_content_partial(const struct s11_fs_ops *fs);

/* The name of the first content pack (S-11.039) into `name` (`cap` bytes,
 * NUL included, cut to fit); "" when there is none, and always on a device
 * without content-packs (S-11.097). */
void s11_pack_name(const struct wb_device *dev, const struct s11_fs_ops *fs,
		   char *name, size_t cap);

/* Entry `index` of the RAM disk's root directory in directory order;
 * false past the last. */
typedef bool (*s11_root_fn)(void *ctx, size_t index, char *name, size_t cap,
			    bool *is_dir);

/* S-11.046: the package's file name into `name` (S11_PATH_MAX bytes);
 * false when the root holds no package. */
bool s11_disk_pick(s11_root_fn root, void *ctx, char *name);

#endif /* WAFFLE_INSTALL_CONTENT_H */
