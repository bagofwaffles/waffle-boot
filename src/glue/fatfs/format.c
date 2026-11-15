/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The RAM disk's format (S-12.003 to S-12.007).
 *
 * FatFs R0.16's f_mkfs, asked for these parameters, writes every byte
 * S-12.003 to S-12.006 give except two fields, in which it differs from
 * R0.13c, the release S-12 section 3 describes:
 * - the partition's end CHS: R0.16 derives a head count from the disk size
 *   (8 here); S-12.003 has R0.13c's 255 heads of 63 sectors, the end head
 *   always 254 and the end sector 63;
 * - the volume ID: R0.16 adds the volume's size to the time; S-12.004 has
 *   the time alone.
 * Both are set here after f_mkfs, in sectors it has just written, so the
 * format still writes only the sectors S-12.007 names.
 */
#include <stdint.h>
#include <string.h>

#include "ff.h"
#include "diskio.h"
#include "wb_fatfs.h"

#define PART_START 63u
#define MBR_PART1 0x1BE
#define VBR_VOLUME_ID 0x27

static int patch_sector(LBA_t lba, BYTE *buf, unsigned int offset,
			const uint8_t *bytes, unsigned int n)
{
	if (disk_read(0, buf, lba, 1) != RES_OK)
		return -1;
	memcpy(buf + offset, bytes, n);
	return disk_write(0, buf, lba, 1) == RES_OK ? 0 : -1;
}

int wb_fatfs_format(void *work, unsigned int work_len)
{
	/* S-12.004: one FAT, 512 root entries, clusters of 16 sectors. */
	const MKFS_PARM opt = { FM_FAT, 1, 1, 512, 16 * 512 };
	BYTE *buf = work;
	LBA_t n_sectors;
	unsigned int cyl;
	uint8_t end_chs[3], vol_id[4];
	FATFS fs;
	FRESULT fr;

	if (work_len < FF_MAX_SS ||
	    disk_ioctl(0, GET_SECTOR_COUNT, &n_sectors) != RES_OK)
		return (int)FR_NOT_READY;
	fr = f_mkfs("", &opt, work, work_len);
	if (fr != FR_OK)
		return (int)fr;

	/* The last cylinder of 255 heads by 63 sectors, at most 1023. */
	cyl = (unsigned int)(n_sectors / (255u * 63u));
	if (cyl > 1023)
		cyl = 1023;
	end_chs[0] = 254;
	end_chs[1] = (uint8_t)((cyl >> 2 & 0xC0u) | 63u);
	end_chs[2] = (uint8_t)cyl;
	vol_id[0] = (uint8_t)WB_FAT_FIXED_CLOCK;
	vol_id[1] = (uint8_t)(WB_FAT_FIXED_CLOCK >> 8);
	vol_id[2] = (uint8_t)(WB_FAT_FIXED_CLOCK >> 16);
	vol_id[3] = (uint8_t)(WB_FAT_FIXED_CLOCK >> 24);
	if (patch_sector(0, buf, MBR_PART1 + 5, end_chs, sizeof(end_chs)) ||
	    patch_sector(PART_START, buf, VBR_VOLUME_ID, vol_id,
			 sizeof(vol_id)))
		return (int)FR_DISK_ERR;

	/* S-12.006: the label entry; the boot sector keeps NO NAME. */
	fr = f_mount(&fs, "", 1);
	if (fr == FR_OK)
		fr = f_setlabel("OP-XY");
	f_mount(NULL, "", 0);
	return (int)fr;
}
