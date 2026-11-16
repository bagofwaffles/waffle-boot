/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "blockdev.h"

typedef uint32_t __attribute__((may_alias)) word;

void wb_blockdev_copy(void *dst, const void *src, size_t len)
{
	word *d = dst;
	const word *s = src;
	size_t n;

	if ((((uintptr_t)dst | (uintptr_t)src | len) & 3u) != 0) {
		memcpy(dst, src, len);
		return;
	}
	for (n = len / 16u; n != 0; n--) {
		d[0] = s[0];
		d[1] = s[1];
		d[2] = s[2];
		d[3] = s[3];
		d += 4;
		s += 4;
	}
	for (n = len % 16u / 4u; n != 0; n--)
		*d++ = *s++;
}

static int in_range(const struct wb_ramdisk *rd, uint32_t lba, uint32_t count)
{
	return lba <= rd->n_sectors && count <= rd->n_sectors - lba;
}

static size_t offset(uint32_t lba)
{
	return (size_t)lba * WB_SECTOR_SIZE;
}

static int ramdisk_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	const struct wb_ramdisk *rd = ctx;

	if (!in_range(rd, lba, count))
		return -1;
	wb_blockdev_copy(buf, rd->mem + offset(lba), offset(count));
	return 0;
}

static int ramdisk_write(void *ctx, uint32_t lba, const void *buf,
			 uint32_t count)
{
	struct wb_ramdisk *rd = ctx;

	if (!in_range(rd, lba, count))
		return -1;
	wb_blockdev_copy(rd->mem + offset(lba), buf, offset(count));
	return 0;
}

static int ramdisk_erase(void *ctx, uint32_t lba, uint32_t count)
{
	struct wb_ramdisk *rd = ctx;

	if (!in_range(rd, lba, count))
		return -1;
	memset(rd->mem + offset(lba), 0, offset(count));
	return 0;
}

void wb_ramdisk_init(struct wb_blockdev *dev, struct wb_ramdisk *rd,
		     void *mem, uint32_t n_sectors)
{
	rd->mem = mem;
	rd->n_sectors = n_sectors;
	dev->ctx = rd;
	dev->n_sectors = n_sectors;
	dev->read = ramdisk_read;
	dev->write = ramdisk_write;
	dev->erase = ramdisk_erase;
	dev->erase_zeroes = NULL;
	dev->write_reliable = NULL;
}
