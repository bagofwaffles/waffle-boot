/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_MTP_VOLUME_H
#define WAFFLE_MTP_VOLUME_H

/*
 * S-10b's store on the master: struct s10b_volume_ops over the slave's
 * /yaffs2 volume, through the bridge's file commands (S-09.041) and its
 * cache flush (S-09.040's 0x04). Every path the engine gives is put under
 * /yaffs2, so nothing outside the volume is named.
 *
 * A folder is removed by walking the volume itself, so objects the listing
 * leaves out (S-10b.041, S-10b.042, S-10b.014) go with it. A rename never
 * replaces: the target is looked up first (S-10b.083, S-10b.084).
 */

#include <stdint.h>

#include "waffle/bridge/records.h"
#include "waffle/bridge/files.h"
#include "waffle/mtp/mtp.h"

#define S10B_STORE_VOLUME "/yaffs2"
/* The longest path sent: one request's payload in every link mode
 * (S-09.023), NUL included. */
#define S10B_STORE_PATH S09_TUNNEL_MAX

struct s10b_store {
	struct s09_files *f;	/* its master; its cache of made directories
				 * is forgotten on every change */
	uint32_t err;		/* the last request's errno (S-09.042) */
	char path[S10B_STORE_PATH];
	char path2[S10B_STORE_PATH];
	uint8_t rec[S09_DIRENT_LEN];
	uint8_t req[2u * (sizeof(S10B_STORE_VOLUME) + S10B_PATH_MAX)];
};

/* Fills ops (ctx st) for the volume f reaches. */
void s10b_store_ops(struct s10b_store *st, struct s09_files *f,
		    struct s10b_volume_ops *ops);

#endif /* WAFFLE_MTP_VOLUME_H */
