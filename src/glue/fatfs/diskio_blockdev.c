/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>

#include "ff.h"
#include "diskio.h"
#include "wb_fatfs.h"

static const struct wb_blockdev *drive;

void wb_fatfs_attach(const struct wb_blockdev *dev)
{
	drive = dev;
}

static const struct wb_blockdev *get_drive(BYTE pdrv)
{
	return pdrv == 0 ? drive : NULL;
}

DSTATUS disk_status(BYTE pdrv)
{
	return get_drive(pdrv) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
	return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	const struct wb_blockdev *dev = get_drive(pdrv);

	if (!dev)
		return RES_NOTRDY;
	return dev->read(dev->ctx, sector, buff, count) ? RES_ERROR : RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	const struct wb_blockdev *dev = get_drive(pdrv);

	if (!dev)
		return RES_NOTRDY;
	return dev->write(dev->ctx, sector, buff, count) ? RES_ERROR : RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	const struct wb_blockdev *dev = get_drive(pdrv);

	if (!dev)
		return RES_NOTRDY;
	switch (cmd) {
	case CTRL_SYNC:
		return RES_OK;
	case GET_SECTOR_COUNT:
		*(LBA_t *)buff = dev->n_sectors;
		return RES_OK;
	case GET_SECTOR_SIZE:
		*(WORD *)buff = WB_SECTOR_SIZE;
		return RES_OK;
	case GET_BLOCK_SIZE:
		/* One sector: no alignment beyond it (S-12 section 3). */
		*(DWORD *)buff = 1;
		return RES_OK;
	default:
		return RES_PARERR;
	}
}

/* Every stamp written without an explicit time is the fixed clock (S-12.015). */
DWORD get_fattime(void)
{
	return WB_FAT_FIXED_CLOCK;
}

/* --- Fast seek on a chain that ends ------------------------------------- */

/* FAT entry `cl` as FatFs's get_fat() reads it; false on a read error. */
static bool fat_entry(FATFS *fs, DWORD cl, DWORD *v, BYTE *sec, LBA_t *held)
{
	const DWORD at = fs->fs_type == FS_FAT12 ? cl + cl / 2u :
			 fs->fs_type == FS_FAT16 ? cl * 2u : cl * 4u;
	BYTE b[4];
	unsigned n = fs->fs_type == FS_FAT12 ? 2u :
		     fs->fs_type == FS_FAT16 ? 2u : 4u;

	for (unsigned i = 0; i < n; i++) {	/* a FAT12 entry may straddle */
		const LBA_t s = fs->fatbase + (at + i) / FF_MAX_SS;

		if (s != *held) {
			if (disk_read(fs->pdrv, sec, s, 1) != RES_OK)
				return false;
			*held = s;
		}
		b[i] = sec[(at + i) % FF_MAX_SS];
	}
	if (fs->fs_type == FS_FAT12) {
		const DWORD w = (DWORD)b[0] | (DWORD)b[1] << 8;

		*v = (cl & 1u) != 0u ? w >> 4 : w & 0xFFFu;
	} else if (fs->fs_type == FS_FAT16) {
		*v = (DWORD)b[0] | (DWORD)b[1] << 8;
	} else {
		*v = ((DWORD)b[0] | (DWORD)b[1] << 8 | (DWORD)b[2] << 16 |
		      (DWORD)b[3] << 24) & 0x0FFFFFFFu;
	}
	return true;
}

/*
 * FatFs builds a link map by following the chain to its end, with no bound,
 * so a chain a host left in a loop holds it forever. The chain is walked
 * here first, no further than the file's size needs (one link for an empty
 * file): only one that ends within that gets the map.
 */
bool wb_fatfs_link_map(FIL *fp, DWORD *tbl, DWORD n)
{
	FATFS *fs = fp->obj.fs;
	BYTE sec[FF_MAX_SS];
	LBA_t held = (LBA_t)-1;
	DWORD cl = fp->obj.sclust, bpc, left;

	if (fs == NULL || fs->csize == 0u || fs->fs_type == 0u)
		return false;
	bpc = (DWORD)fs->csize * FF_MAX_SS;
	left = (DWORD)((fp->obj.objsize + bpc - 1u) / bpc);
	if (left == 0u)
		left = 1u;
	for (; cl != 0u; left--) {
		if (left == 0u || cl < 2u || cl >= fs->n_fatent ||
		    !fat_entry(fs, cl, &cl, sec, &held) || cl < 2u)
			return false;
		if (cl >= fs->n_fatent)
			break;			/* the end of the chain */
	}
	tbl[0] = n;
	fp->cltbl = tbl;
	if (f_lseek(fp, CREATE_LINKMAP) == FR_OK)
		return true;
	fp->cltbl = NULL;
	return false;
}
