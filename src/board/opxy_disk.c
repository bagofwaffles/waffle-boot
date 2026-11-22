/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The RAM disk of S-12.001 as one device of 512-byte sectors over its parts
 * in DDR (S-01.016, waffle/board/opxy.h): the sectors of part 0 first, then
 * those of the next part, and so on. A transfer that crosses from one part into the next is
 * split there. Memory does not fail, so neither does a transfer inside the
 * disk (S-10a.043's medium errors never arise).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "waffle/board/opxy.h"
#include "waffle/board/opxy_services.h"

/* The part holding sector *lba, with *lba made relative to it and *n cut
 * to the sectors left in it; NULL past the end of the disk. */
static uint8_t *locate(const struct opxy_disk *d, uint32_t *lba, uint32_t *n)
{
	unsigned i;

	for (i = 0; i < OPXY_DISK_PARTS; i++) {
		const struct opxy_disk_part *p = &d->part[i];

		if (*lba < p->sectors) {
			if (*n > p->sectors - *lba)
				*n = p->sectors - *lba;
			return p->mem + (size_t)*lba * WB_SECTOR_SIZE;
		}
		*lba -= p->sectors;
	}
	return NULL;
}

/* A transfer's buffer: out for a read, in for a write, neither for an
 * erase. */
struct xfer {
	uint8_t *out;
	const uint8_t *in;
};

/* Moves each part's share of count sectors from lba; nonzero when the
 * range leaves the disk (nothing is moved then). */
static int move(const struct opxy_disk *d, uint32_t lba, uint32_t count,
		const struct xfer *x)
{
	size_t off = 0;

	if (count > d->dev.n_sectors || lba > d->dev.n_sectors - count)
		return 1;
	while (count > 0u) {
		uint32_t at = lba, n = count;
		uint8_t *mem = locate(d, &at, &n);
		const size_t len = (size_t)n * WB_SECTOR_SIZE;

		if (mem == NULL)
			return 1;
		if (x->out != NULL)
			wb_blockdev_copy(x->out + off, mem, len);
		else if (x->in != NULL)
			wb_blockdev_copy(mem, x->in + off, len);
		else
			memset(mem, 0, len);
		off += len;
		lba += n;
		count -= n;
	}
	return 0;
}

/* cppcheck-suppress constParameterCallback ; wb_blockdev fixes the type */
static int disk_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	const struct xfer x = { buf, NULL };

	return move(ctx, lba, count, &x);
}

/* cppcheck-suppress constParameterCallback ; wb_blockdev fixes the type */
static int disk_write(void *ctx, uint32_t lba, const void *buf,
		      uint32_t count)
{
	const struct xfer x = { NULL, buf };

	return move(ctx, lba, count, &x);
}

/* cppcheck-suppress constParameterCallback ; wb_blockdev fixes the type */
static int disk_erase(void *ctx, uint32_t lba, uint32_t count)
{
	const struct xfer x = { NULL, NULL };

	return move(ctx, lba, count, &x);
}

void opxy_disk_init(struct opxy_disk *d,
		    const struct opxy_disk_part part[OPXY_DISK_PARTS])
{
	unsigned i;

	d->dev.ctx = d;
	d->dev.n_sectors = 0;
	for (i = 0; i < OPXY_DISK_PARTS; i++) {
		d->part[i] = part[i];
		d->dev.n_sectors += part[i].sectors;
	}
	d->dev.read = disk_read;
	d->dev.write = disk_write;
	d->dev.erase = disk_erase;
	d->dev.erase_zeroes = NULL;
}

void opxy_disk_ddr_dev(const struct wb_device *dev,
		       struct opxy_disk_part part[OPXY_DISK_PARTS])
{
	unsigned i;

	if (!dev->mem.ramdisk_one_region) {
		opxy_disk_ddr(part);
		return;
	}
	/* S-01.016, S-12.008 on the OP-1F: one region, device.ramdisk_blocks
	 * sectors; the other part holds none */
	part[0].mem = (uint8_t *)(uintptr_t)OPXY_1F_DISK_START;
	part[0].sectors = dev->mem.ramdisk_blocks;
	for (i = 1; i < OPXY_DISK_PARTS; i++) {
		part[i].mem = NULL;
		part[i].sectors = 0u;
	}
}

void opxy_disk_ddr(struct opxy_disk_part part[OPXY_DISK_PARTS])
{
	part[0].mem = (uint8_t *)(uintptr_t)OPXY_DISK0_START;
	part[0].sectors = (OPXY_DISK0_END - OPXY_DISK0_START) / WB_SECTOR_SIZE;
#if defined(OPXY_TESTOS_SHARED)
	/* the installer's image and heap where part 1 was (S-05.043) */
	part[1].mem = (uint8_t *)(uintptr_t)OPXY_TESTOS_SHARED_DISK1_START;
	part[1].sectors = (OPXY_TESTOS_SHARED_DISK1_END - OPXY_TESTOS_SHARED_DISK1_START) /
			  WB_SECTOR_SIZE;
	part[2].mem = (uint8_t *)(uintptr_t)OPXY_TESTOS_SHARED_DISK2_START;
	part[2].sectors = (OPXY_TESTOS_SHARED_DISK2_END - OPXY_TESTOS_SHARED_DISK2_START) /
			  WB_SECTOR_SIZE;
#else
	part[1].mem = (uint8_t *)(uintptr_t)OPXY_DISK1_START;
	part[1].sectors = (OPXY_DISK1_END - OPXY_DISK1_START) / WB_SECTOR_SIZE;
#endif
}
