/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * yaffs2 on a volume of 8 blocks whose chunks come from the input, as
 * records of
 *     chunk number (2 bytes, little-endian, modulo the volume's 1,024 chunks)
 *     n (2 bytes, little-endian, modulo 4,081)
 *     n bytes of the chunk's data area
 *     16 bytes of tags
 * and every other chunk erased (zero). The volume is mounted, walked,
 * read, and written to.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wb_yaffs.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define BLOCKS 8u
#define CHUNKS (BLOCKS * WB_YAFFS_CHUNKS_PER_BLOCK)
#define DATA_BYTES (WB_YAFFS_CHUNK_BYTES - 16u)
#define MAX_ENTRIES 256
#define MAX_DEPTH 8
#define MAX_READ 65536

static uint8_t image[CHUNKS * WB_YAFFS_CHUNK_BYTES];
static struct wb_blockdev bdev;
static struct wb_ramdisk ram;
static struct yaffs_dev dev;
static unsigned int entries;

static unsigned int le16(const uint8_t *p)
{
	return (unsigned int)p[0] | (unsigned int)p[1] << 8;
}

static void load(const uint8_t *data, size_t size)
{
	memset(image, 0, sizeof(image));
	while (size >= 4) {
		uint8_t *chunk = image + (le16(data) % CHUNKS) *
						 WB_YAFFS_CHUNK_BYTES;
		size_t n = le16(data + 2) % (DATA_BYTES + 1);
		size_t t;

		data += 4;
		size -= 4;
		if (n > size)
			n = size;
		memcpy(chunk, data, n);
		data += n;
		size -= n;
		t = size < 16 ? size : 16;
		memcpy(chunk + DATA_BYTES, data, t);
		data += t;
		size -= t;
	}
}

static void read_file(const char *path)
{
	static uint8_t buf[4096];
	int fd = yaffs_open(path, O_RDONLY, 0), n, total = 0;

	if (fd < 0)
		return;
	while (total < MAX_READ && (n = yaffs_read(fd, buf, sizeof(buf))) > 0)
		total += n;
	yaffs_close(fd);
}

static void walk(const char *dir, int depth)
{
	char path[2048], link[512];
	const struct yaffs_dirent *de;
	struct yaffs_stat st;
	yaffs_DIR *d;

	if (depth > MAX_DEPTH || !(d = yaffs_opendir(dir)))
		return;
	while (entries < MAX_ENTRIES && (de = yaffs_readdir(d)) != NULL) {
		entries++;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (yaffs_lstat(path, &st))
			continue;
		switch (st.st_mode & S_IFMT) {
		case S_IFDIR:
			walk(path, depth + 1);
			break;
		case S_IFREG:
			read_file(path);
			break;
		case S_IFLNK:
			yaffs_readlink(path, link, sizeof(link) - 1);
			break;
		default:
			break;
		}
	}
	yaffs_closedir(d);
}

static void write_some(void)
{
	static uint8_t buf[9000];
	int fd;

	memset(buf, 'y', sizeof(buf));
	yaffs_mkdir("/f/logs", 0755);
	fd = yaffs_open("/f/logs/boot.log", O_CREAT | O_RDWR | O_APPEND, 0644);
	if (fd >= 0) {
		yaffs_write(fd, buf, sizeof(buf));
		yaffs_ftruncate(fd, 100);
		yaffs_close(fd);
	}
	yaffs_rename("/f/logs/boot.log", "/f/logs/old.log");
	yaffs_unlink("/f/logs/old.log");
	yaffs_rmdir("/f/logs");
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static int added;

	if (!added) {
		wb_ramdisk_init(&bdev, &ram, image,
				(uint32_t)(sizeof(image) / WB_SECTOR_SIZE));
		wb_yaffs_add_blockdev(&dev, "/f", &bdev, 0, BLOCKS - 1);
		/* yaffs2 needs more blocks than the reserve plus 2. */
		dev.param.n_reserved_blocks = 2;
		added = 1;
	}
	load(data, size);
	entries = 0;
	if (yaffs_mount("/f") == 0) {
		walk("/f", 0);
		write_some();
		yaffs_unmount2("/f", 1);
	}
	return 0;
}
