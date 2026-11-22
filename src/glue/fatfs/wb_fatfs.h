/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FatFs on a block device: the disk I/O layer FatFs calls (diskio.h) and
 * its time source.
 */
#ifndef WAFFLE_WB_FATFS_H
#define WAFFLE_WB_FATFS_H

#include <stdbool.h>

#include "blockdev.h"
#include "ff.h"

/*
 * The fixed clock of S-12.015, 1980-01-01 00:00:00, as get_fattime()
 * returns it (date in the high half). It is also the volume ID a format
 * writes (S-12.004).
 */
#define WB_FAT_FIXED_CLOCK 0x00210000u

/*
 * Makes dev FatFs physical drive 0, the only one. NULL detaches it: every
 * access then fails as "not ready". The caller keeps dev alive.
 */
void wb_fatfs_attach(const struct wb_blockdev *dev);
/* The device drive 0 is attached to now, or NULL. */
const struct wb_blockdev *wb_fatfs_drive(void);

/*
 * Formats drive 0 as the RAM disk of S-12.003 to S-12.006 (S-12.008 on the
 * OP-1F's disk): a master boot record with one FAT16 partition from sector
 * 63 to the end of the disk, one FAT, 512 root entries, clusters by the
 * disk's size (16 sectors on the OP-XY's, 8 on the OP-1F's), the volume ID
 * of the fixed clock and the label (device.product_text, at most 11
 * characters). It writes the sectors S-12.007 names and no other (0, 63
 * and 64 to 206 on the OP-XY's disk). work is a scratch buffer of at least
 * FF_MAX_SS bytes. Returns a FatFs FRESULT, FR_OK (0) on success. Drive 0
 * is not mounted afterwards.
 */
int wb_fatfs_format(void *work, unsigned int work_len, const char *label);

/*
 * Fast seek for an open file: a link map in tbl, n entries, when its
 * cluster chain ends within what its size needs; false, and fp left to seek
 * the slow way, when it does not (a chain in a loop, say) or the map is too
 * small.
 */
bool wb_fatfs_link_map(FIL *fp, DWORD *tbl, DWORD n);

#endif /* WAFFLE_WB_FATFS_H */
