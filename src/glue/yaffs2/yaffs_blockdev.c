/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A yaffs2 driver for a device of 512-byte sectors: chunk n is sectors
 * 8n to 8n + 7, block b is chunks 128b to 128b + 127 (S-12.041, S-12.042).
 * With in-band tags yaffs2 hands the driver whole 4,096-byte chunks, the
 * tags already in their last 16 bytes (S-12.044).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "waffle/drivers/bf70x_msi.h"	/* WB_MSI_TIMEOUT */
#include "wb_yaffs.h"

static struct wb_blockdev *bdev(const struct yaffs_dev *dev)
{
	return dev->driver_context;
}

/* The first sector of chunk nand_chunk, or UINT32_MAX if it is off bd. */
static uint32_t chunk_lba(const struct yaffs_dev *dev, int nand_chunk)
{
	uint32_t lba;

	if (nand_chunk < 0)
		return UINT32_MAX;
	lba = (uint32_t)nand_chunk * WB_YAFFS_SECTORS_PER_CHUNK;
	if (lba / WB_YAFFS_SECTORS_PER_CHUNK != (uint32_t)nand_chunk ||
	    lba > bdev(dev)->n_sectors - WB_YAFFS_SECTORS_PER_CHUNK)
		return UINT32_MAX;
	return lba;
}

/* cppcheck-suppress constParameterCallback ; yaffs2 fixes the type */
static int drv_write_chunk(struct yaffs_dev *dev, int nand_chunk,
			   const u8 *data, int data_len, const u8 *oob,
			   int oob_len)
{
	struct wb_blockdev *bd = bdev(dev);
	uint32_t lba = chunk_lba(dev, nand_chunk);
	int rc;

	(void)oob;
	if (lba == UINT32_MAX || !data ||
	    data_len != (int)WB_YAFFS_CHUNK_BYTES || oob_len != 0)
		return YAFFS_FAIL;
	/* S-12.060: the chunk's eight sectors as one reliable write. */
	if (bd->write_reliable != NULL)
		rc = bd->write_reliable(bd->ctx, lba, data,
					WB_YAFFS_SECTORS_PER_CHUNK);
	else
		rc = bd->write(bd->ctx, lba, data, WB_YAFFS_SECTORS_PER_CHUNK);
	return rc ? YAFFS_FAIL : YAFFS_OK;
}

/* cppcheck-suppress constParameterCallback ; yaffs2 fixes the type */
static int drv_read_chunk(struct yaffs_dev *dev, int nand_chunk, u8 *data,
			  int data_len, u8 *oob, int oob_len,
			  enum yaffs_ecc_result *ecc_result)
{
	struct wb_blockdev *bd = bdev(dev);
	uint32_t lba = chunk_lba(dev, nand_chunk);

	(void)oob;
	*ecc_result = YAFFS_ECC_RESULT_NO_ERROR;
	if (lba == UINT32_MAX || !data ||
	    data_len != (int)WB_YAFFS_CHUNK_BYTES || oob_len != 0)
		return YAFFS_FAIL;
	if (bd->read(bd->ctx, lba, data, WB_YAFFS_SECTORS_PER_CHUNK))
		return YAFFS_FAIL;
	return YAFFS_OK;
}

/*
 * Erases a block so that it reads as zero (S-12.043). Without an erase
 * operation the block's sectors are written with zeros in ascending runs
 * of WB_YAFFS_ZERO_RUN sectors, one plain write each, which the eMMC takes
 * as a few multi-block writes rather than a command per sector. A run that
 * times out (WB_MSI_TIMEOUT, which the board's device passes up from the
 * eMMC driver as it is), as a run can on an eMMC too slow for the
 * driver's bound on a whole transfer (S-13b.057), is written again a
 * sector at a time, each sector with its own bound. Any other failure,
 * and a sector that fails, fails the erase and nothing after it is
 * written: a failure leaves a prefix of the block zeroed, as a sector at
 * a time did. yaffs2 retires a block whose erase fails, so a slow eMMC
 * that works must not fail here.
 * The run is not const, so it lies in .bss rather than in the loaded
 * image, on a 32-byte boundary as the eMMC driver needs (S-13b.054).
 */
#define WB_YAFFS_ZERO_RUN 128u
_Static_assert(WB_YAFFS_SECTORS_PER_BLOCK % WB_YAFFS_ZERO_RUN == 0,
	       "a block is whole runs");
static _Alignas(32) uint8_t zero_run[WB_YAFFS_ZERO_RUN * WB_SECTOR_SIZE];

static int zero_sectors(const struct wb_blockdev *bd, uint32_t lba)
{
	uint32_t i;
	int rc = bd->write(bd->ctx, lba, zero_run, WB_YAFFS_ZERO_RUN);

	if (rc != WB_MSI_TIMEOUT)
		return rc;
	for (i = 0; i < WB_YAFFS_ZERO_RUN; i++)
		if (bd->write(bd->ctx, lba + i, zero_run, 1u))
			return -1;
	return 0;
}

/* cppcheck-suppress constParameterCallback ; yaffs2 fixes the type */
static int drv_erase(struct yaffs_dev *dev, int block_no)
{
	struct wb_blockdev *bd = bdev(dev);
	uint32_t lba, i;

	if (block_no < 0 ||
	    (uint32_t)block_no >= bd->n_sectors / WB_YAFFS_SECTORS_PER_BLOCK)
		return YAFFS_FAIL;
	lba = (uint32_t)block_no * WB_YAFFS_SECTORS_PER_BLOCK;
	if (bd->erase && (bd->erase_zeroes == NULL || bd->erase_zeroes(bd->ctx)))
		return bd->erase(bd->ctx, lba, WB_YAFFS_SECTORS_PER_BLOCK) ?
			YAFFS_FAIL : YAFFS_OK;
	for (i = 0; i < WB_YAFFS_SECTORS_PER_BLOCK; i += WB_YAFFS_ZERO_RUN)
		if (zero_sectors(bd, lba + i))
			return YAFFS_FAIL;
	return YAFFS_OK;
}

/* The eMMC has no bad blocks (S-03); bad-block marking is disabled. */
static int drv_mark_bad(struct yaffs_dev *dev, int block_no)
{
	(void)dev;
	(void)block_no;
	return YAFFS_OK;
}

static int drv_check_bad(struct yaffs_dev *dev, int block_no)
{
	(void)dev;
	(void)block_no;
	return YAFFS_OK;
}

static int drv_initialize(struct yaffs_dev *dev)
{
	(void)dev;
	return YAFFS_OK;
}

int wb_yaffs_add_blockdev(struct yaffs_dev *dev, const char *name,
			  struct wb_blockdev *bd, uint32_t first_block,
			  uint32_t last_block)
{
	struct yaffs_param *p = &dev->param;
	struct yaffs_driver *drv = &dev->drv;

	if (first_block > last_block ||
	    last_block >= bd->n_sectors / WB_YAFFS_SECTORS_PER_BLOCK)
		return -1;

	memset(dev, 0, sizeof(*dev));
	p->name = name;
	/* The parameters of S-12 section 3. */
	p->total_bytes_per_chunk = WB_YAFFS_CHUNK_BYTES;
	p->chunks_per_block = WB_YAFFS_CHUNKS_PER_BLOCK;
	p->start_block = first_block;
	p->end_block = last_block;
	p->inband_tags = 1;
	p->is_yaffs2 = 1;
	p->no_tags_ecc = 1;
	p->n_reserved_blocks = WB_YAFFS_RESERVED_BLOCKS;
	p->disable_summary = 0;
	p->enable_xattr = 0;
	p->stored_endian = 0;
	p->hide_lost_n_found = 0;
	p->empty_lost_n_found = 0;
	p->disable_bad_block_marking = 1;
	/* Always scan, and never write a checkpoint (S-12.055). */
	p->skip_checkpt_rd = 1;
	p->skip_checkpt_wr = 1;
	p->n_caches = 10;

	drv->drv_write_chunk_fn = drv_write_chunk;
	drv->drv_read_chunk_fn = drv_read_chunk;
	drv->drv_erase_fn = drv_erase;
	drv->drv_mark_bad_fn = drv_mark_bad;
	drv->drv_check_bad_fn = drv_check_bad;
	drv->drv_initialise_fn = drv_initialize;
	drv->drv_deinitialise_fn = drv_initialize;
	dev->driver_context = bd;

	yaffs_add_device(dev);
	return 0;
}
