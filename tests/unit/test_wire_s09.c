/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The slave's file layer of S-09.041 on yaffs2 (src/wire_s09.c): each file
 * command against a yaffs2 volume in RAM, with the results and errno
 * numbers of S-09.041 and S-09.042 (errno.json's table; S-CMD's fs_err_*
 * cases: a non-empty rmdir 93, a 300-character name 78, a full volume 28),
 * the records of S-09.043, the pieces of S-09.035's chunked read
 * (file_read.json), /fat32, and ENODEV on a volume not mounted or refused
 * (a big-endian image of S-12.052).
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "waffle/wire_crc.h"
#include "waffle/wire_s09.h"

#include "vectors/wire_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define BLOCKS 40u

static struct wb_blockdev bd;
static struct wb_ramdisk ram;
static struct wire_yaffs Y;
static struct wire_s09 S;
static uint8_t rec[S09_DIRENT_LEN];
static uint8_t data_buf[0x3000];

/* One file command as s09_slave hands it over: relative paths under
 * /yaffs2/. Returns the result; *err the errno word. */
static int32_t cmd(uint8_t c, uint32_t arg0, uint32_t arg1, uint16_t len,
		   const char *path, const char *path2, const uint8_t *data,
		   uint32_t n, uint32_t *err)
{
	static char p1[700], p2[700];
	struct s09_req r = { c, 0, len, arg0, arg1 };

	if (path != NULL)
		snprintf(p1, sizeof(p1), "%s%s", *path == '/' ? "" : "/yaffs2/",
			 path);
	if (path2 != NULL)
		snprintf(p2, sizeof(p2), "%s%s", *path2 == '/' ? "" : "/yaffs2/",
			 path2);
	memset(rec, 0, sizeof(rec));
	*err = 0xFFFFFFFFu;
	return wire_s09_file(&S, &r, path != NULL ? p1 : NULL,
			     path2 != NULL ? p2 : NULL, data, n, rec, err);
}

static int32_t path_cmd(uint8_t c, uint32_t arg0, const char *path,
			uint32_t *err)
{
	return cmd(c, arg0, 0, 0, path, NULL, NULL, 0, err);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static void test_errno(void)
{
	static const int map[][2] = {
		{ ENOENT, 2 }, { EBADF, 9 }, { ENOMEM, 12 }, { EACCES, 13 },
		{ EFAULT, 14 }, { EBUSY, 16 }, { EEXIST, 17 }, { EXDEV, 18 },
		{ ENODEV, 19 }, { ENOTDIR, 20 }, { EISDIR, 21 }, { EINVAL, 22 },
		{ ENFILE, 23 }, { ENOSPC, 28 }, { EROFS, 30 }, { ERANGE, 34 },
		{ ELOOP, 40 }, { ENODATA, 61 }, { ENAMETOOLONG, 78 },
		{ ENOTEMPTY, 93 },
	};

	for (size_t i = 0; i < N(map); i++)
		CHECK(wire_s09_errno(-map[i][0]) == (uint32_t)map[i][1],
		      "errno %d -> %u", map[i][0], wire_s09_errno(-map[i][0]));
	CHECK(wire_s09_errno(0) == 0, "no error");
}

static void test_unmounted(void)
{
	uint32_t err;

	CHECK(path_cmd(S09_STAT, 0, "x", &err) == -1 && err == S09_ENODEV,
	      "stat before the mount: ENODEV");
	CHECK(path_cmd(S09_OPENDIR, 0, "", &err) == 0 && err == S09_ENODEV,
	      "opendir before the mount: 0, ENODEV");
	CHECK(path_cmd(S09_FREE_SPACE, 0, "", &err) == 0 && err == S09_ENODEV,
	      "free space before the mount: 0, ENODEV");
	CHECK(wire_s09_read(&S, 0, data_buf, 1, &err) == -1 && err == S09_ENODEV,
	      "read before the mount: ENODEV");
	CHECK(cmd(S09_MOUNT, 0, 0, 0, NULL, NULL, NULL, 0, &err) == 0 &&
	      err == 0 && cmd(S09_MOUNT, 0, 0, 0, NULL, NULL, NULL, 0, &err) == 0,
	      "mount, and again");
}

static void test_files(void)
{
	static const uint8_t hello[5] = { 'h', 'e', 'l', 'l', 'o' };
	struct s09_stat st;
	uint32_t err;
	int32_t fd;

	CHECK(path_cmd(S09_MKDIR, 0755, "d", &err) == 0 && err == 0, "mkdir");
	CHECK(path_cmd(S09_MKDIR, 0755, "d", &err) == -1 && err == S09_EEXIST,
	      "mkdir again: EEXIST");
	fd = path_cmd(S09_OPEN, S09_O_WRONLY | S09_O_CREAT, "d/f", &err);
	CHECK(fd >= 0 && err == 0, "open to create");
	CHECK(cmd(S09_WRITE, (uint32_t)fd, 5, 5, NULL, NULL, hello, 5, &err) == 5,
	      "write: the count");
	CHECK(cmd(S09_WRITE, (uint32_t)fd, 0, 0, NULL, NULL, hello, 5, &err) == -1 &&
	      err == S09_EINVAL &&
	      cmd(S09_WRITE, (uint32_t)fd, 6, 6, NULL, NULL, hello, 5, &err) == -1 &&
	      err == S09_EINVAL, "write of 0 or past the payload: EINVAL");
	CHECK(cmd(S09_FSYNC, (uint32_t)fd, 0, 0, NULL, NULL, NULL, 0, &err) == 0,
	      "fsync");
	CHECK(cmd(S09_CLOSE, (uint32_t)fd, 0, 0, NULL, NULL, NULL, 0, &err) == 0 &&
	      cmd(S09_CLOSE, (uint32_t)fd, 0, 0, NULL, NULL, NULL, 0, &err) == -1 &&
	      err == S09_EBADF, "close, and again: EBADF");
	CHECK(path_cmd(S09_OPEN, S09_O_WRONLY | S09_O_CREAT | S09_O_EXCL, "d/f",
		       &err) == -1 && err == S09_EEXIST, "exclusive create: EEXIST");

	/* S-09.043: the file status record */
	CHECK(path_cmd(S09_STAT, 0, "d/f", &err) == 0 && err == 0, "stat");
	s09_stat_unpack(rec, &st);
	CHECK((st.mode & 0xF000u) == 0x8000u && st.size == 5 && st.dev == 0 &&
	      st.nlink == 1, "stat: a file of 5 bytes, mode %x", st.mode);
	CHECK(path_cmd(S09_STAT, 0, "d", &err) == 0, "stat d");
	s09_stat_unpack(rec, &st);
	CHECK((st.mode & 0xF000u) == 0x4000u, "stat: a directory");
	CHECK(path_cmd(S09_STAT, 0, "none", &err) == -1 && err == S09_ENOENT,
	      "stat of nothing: ENOENT");

	/* lseek and the read pieces */
	fd = path_cmd(S09_OPEN, S09_O_RDONLY, "d/f", &err);
	CHECK(fd >= 0, "open to read");
	CHECK(cmd(S09_LSEEK, (uint32_t)fd, 1, 0, NULL, NULL, NULL, 0, &err) == 1 &&
	      cmd(S09_LSEEK, (uint32_t)fd, 0, 2, NULL, NULL, NULL, 0, &err) == 5 &&
	      cmd(S09_LSEEK, (uint32_t)fd, (uint32_t)-2, 1, NULL, NULL, NULL, 0,
		  &err) == 3, "lseek: set, end, current back");
	CHECK(cmd(S09_LSEEK, (uint32_t)fd, 0, 3, NULL, NULL, NULL, 0, &err) == -1 &&
	      err == S09_EINVAL, "lseek: whence 3 is EINVAL");
	CHECK(wire_s09_read(&S, (uint32_t)fd, data_buf, 10, &err) == 2 && err == 0 &&
	      data_buf[0] == 'l' && data_buf[1] == 'o', "read to the end");
	CHECK(wire_s09_read(&S, (uint32_t)fd, data_buf, 10, &err) == 0, "read at the end");
	CHECK(wire_s09_read(&S, 999, data_buf, 1, &err) == -1 && err == S09_EBADF,
	      "read of a bad descriptor: EBADF");

	/* utime and futimens (16 bits of the modification time) */
	CHECK(cmd(S09_UTIME, 100, 200, 0, "d/f", NULL, NULL, 0, &err) == 0, "utime");
	(void)path_cmd(S09_STAT, 0, "d/f", &err);
	s09_stat_unpack(rec, &st);
	CHECK(st.atime == 100 && st.mtime == 200, "utime: %u %u", st.atime,
	      st.mtime);
	CHECK(cmd(S09_FUTIMENS, (uint32_t)fd, 300, 0x1234, NULL, NULL, NULL, 0,
		  &err) == 0, "futimens");
	(void)path_cmd(S09_STAT, 0, "d/f", &err);
	s09_stat_unpack(rec, &st);
	CHECK(st.atime == 300 && st.mtime == 0x1234, "futimens: %u %u", st.atime,
	      st.mtime);
	(void)cmd(S09_CLOSE, (uint32_t)fd, 0, 0, NULL, NULL, NULL, 0, &err);

	CHECK(cmd(S09_RENAME, 0, 0, 0, "d/f", "d/g", NULL, 0, &err) == 0 &&
	      path_cmd(S09_ACCESS, 0, "d/g", &err) == 0 &&
	      path_cmd(S09_ACCESS, 0, "d/f", &err) == -1 && err == S09_ENOENT,
	      "rename, access");
	CHECK(path_cmd(S09_SYNC, 0, "", &err) == 0, "sync");
	/* S-CMD fs_err_*: 93 and 78 as S-CMD gives them */
	CHECK(path_cmd(S09_RMDIR, 0, "d", &err) == -1 && err == S09_ENOTEMPTY,
	      "rmdir of a non-empty directory: 93, got %u", err);
	CHECK(path_cmd(S09_UNLINK, 0, "d/g", &err) == 0 &&
	      path_cmd(S09_RMDIR, 0, "d", &err) == 0, "unlink, rmdir");
	memset(data_buf, 'n', 300);
	data_buf[300] = 0;
	CHECK(path_cmd(S09_OPEN, S09_O_WRONLY | S09_O_CREAT, (const char *)data_buf,
		       &err) == -1 && err == S09_ENAMETOOLONG,
	      "a 300-character name: 78, got %u", err);
}

static void test_dirs(void)
{
	struct s09_dirent d;
	uint32_t err, h[WIRE_S09_DIRS];
	int32_t fd;

	CHECK(path_cmd(S09_MKDIR, 0755, "e", &err) == 0, "mkdir e");
	fd = path_cmd(S09_OPEN, S09_O_WRONLY | S09_O_CREAT, "e/progress.log", &err);
	(void)cmd(S09_CLOSE, (uint32_t)fd, 0, 0, NULL, NULL, NULL, 0, &err);
	h[0] = (uint32_t)path_cmd(S09_OPENDIR, 0, "e", &err);
	CHECK(h[0] != 0 && err == 0, "opendir: a nonzero handle");
	CHECK(cmd(S09_READDIR, h[0], 0, 0, NULL, NULL, NULL, 0, &err) == 1 &&
	      err == 0, "readdir: an entry");
	s09_dirent_unpack(rec, &d);
	CHECK(!strcmp(d.name, "progress.log") && d.type == 8 &&
	      (rec[8] | rec[9] << 8) == 0x10C, "the directory entry");
	CHECK(cmd(S09_READDIR, h[0], 0, 0, NULL, NULL, NULL, 0, &err) == 0 &&
	      err == 0, "readdir: the end, errno 0");
	CHECK(cmd(S09_CLOSEDIR, h[0], 0, 0, NULL, NULL, NULL, 0, &err) == 0, "closedir");
	CHECK(cmd(S09_READDIR, h[0], 0, 0, NULL, NULL, NULL, 0, &err) == 0 &&
	      err == S09_EBADF &&
	      cmd(S09_CLOSEDIR, h[0], 0, 0, NULL, NULL, NULL, 0, &err) == -1 &&
	      err == S09_EBADF, "a closed handle: EBADF");
	CHECK(path_cmd(S09_OPENDIR, 0, "none", &err) == 0 && err == S09_ENOENT,
	      "opendir of nothing: 0, ENOENT");
	for (size_t i = 0; i < WIRE_S09_DIRS; i++)
		h[i] = (uint32_t)path_cmd(S09_OPENDIR, 0, "e", &err);
	CHECK(h[WIRE_S09_DIRS - 1] != 0 &&
	      path_cmd(S09_OPENDIR, 0, "e", &err) == 0 && err == S09_ENFILE,
	      "handles run out: ENFILE");
	wire_s09_release(&S);
	CHECK(path_cmd(S09_OPENDIR, 0, "e", &err) != 0, "the release frees them");
	wire_s09_release(&S);
	CHECK(path_cmd(S09_STAT, 0, "/fat32/x", &err) == -1 && err == S09_ENODEV &&
	      path_cmd(S09_OPENDIR, 0, "/fat32", &err) == 0 && err == S09_ENODEV,
	      "/fat32: ENODEV");
}

static void test_space(void)
{
	struct s09_statfs fs;
	uint64_t total = (uint64_t)(BLOCKS - 5u) * 128u * 4080u, free_now = 0;
	uint32_t err;
	int32_t fd, r = 0;

	CHECK(path_cmd(S09_TOTAL_SPACE, 0, "", &err) == 0 && le64(rec) == total,
	      "total space %llu", (unsigned long long)le64(rec));
	CHECK(path_cmd(S09_FREE_SPACE, 0, "", &err) == 0 &&
	      wire_yaffs_free_space(&Y, &free_now) && le64(rec) == free_now,
	      "free space");
	CHECK(path_cmd(S09_STATFS, 0, "", &err) == 0 && err == 0, "statfs");
	s09_statfs_unpack(rec, &fs);
	CHECK(fs.bsize == 512 && fs.blocks == total / 512u &&
	      fs.bfree == free_now / 512u, "statfs: %u %u %u", fs.bsize,
	      fs.blocks, fs.bfree);
	/* S-CMD fs_err_*: a full volume gives 28 */
	memset(data_buf, 0xA5, 0x1000);
	fd = path_cmd(S09_OPEN, S09_O_WRONLY | S09_O_CREAT, "full", &err);
	for (unsigned i = 0; fd >= 0 && i < 10000 && r >= 0; i++)
		r = cmd(S09_WRITE, (uint32_t)fd, 0x1000, 0x1000, NULL, NULL, data_buf,
			0x1000, &err);
	CHECK(r == -1 && err == S09_ENOSPC, "a full volume: 28, got %u", err);
	(void)cmd(S09_CLOSE, (uint32_t)fd, 0, 0, NULL, NULL, NULL, 0, &err);
	CHECK(path_cmd(S09_UNLINK, 0, "full", &err) == 0, "unlink full");
}

/* S-09.035: the pieces s09_slave asks for, served by wire_s09_read() */
static void test_reads(void)
{
	for (size_t i = 0; i < N(wirev_reads); i++) {
		const struct wirev_read *v = &wirev_reads[i];
		uint32_t err, received = 0, k = 0;
		int32_t fd, sum = 0;
		bool ok = true;

		for (uint32_t b = 0; b < v->file_len; b++)
			data_buf[b] = (uint8_t)(b * 13u + i);
		fd = path_cmd(S09_OPEN, S09_O_RDWR | S09_O_CREAT | S09_O_TRUNC,
			      "r.bin", &err);
		CHECK(fd >= 0 && cmd(S09_WRITE, (uint32_t)fd, 0, 0, NULL, NULL, data_buf,
				     0, &err) == -1, "%s: file", v->name);
		for (uint32_t off = 0; off < v->file_len; off += 0x1000u) {
			uint32_t m = v->file_len - off < 0x1000u ?
				     v->file_len - off : 0x1000u;

			ok &= cmd(S09_WRITE, (uint32_t)fd, m, (uint16_t)m, NULL, NULL,
				  data_buf + off, m, &err) == (int32_t)m;
		}
		ok &= cmd(S09_LSEEK, (uint32_t)fd, v->pos, 0, NULL, NULL, NULL, 0,
			  &err) == (int32_t)v->pos;
		CHECK(ok, "%s: written", v->name);
		do {
			static uint8_t piece[0x1000];
			uint32_t before = received;
			int32_t r;

			r = wire_s09_read(&S, (uint32_t)fd, piece,
					  s09_read_ask(v->n, received, 0x1000u), &err);
			CHECK(k < v->pieces && r == v->r[k] && err == 0 &&
			      (r <= 0 || !memcmp(piece, data_buf + v->pos + received,
						 (size_t)r)),
			      "%s: piece %u: %d", v->name, k, r);
			received += r > 0 ? (uint32_t)r : 0u;
			sum += r;
			k++;
			if (!s09_read_continues(r, 0x1000u, before, v->n))
				break;
		} while (k <= v->pieces);
		CHECK(k == v->pieces && sum == v->result, "%s: %u pieces, %d", v->name,
		      k, sum);
		(void)cmd(S09_CLOSE, (uint32_t)fd, 0, 0, NULL, NULL, NULL, 0, &err);
	}
}

/* A CRC that is always wrong, for the fast path that must be checked. */
static uint32_t bad_crc(uint32_t crc, const uint8_t *p, uint32_t n)
{
	return ~wire_crc32(crc, p, n);
}

/* A file written whole through the bridge's commands. */
static void put_file(const char *path, const uint8_t *data, uint32_t n)
{
	uint32_t err;
	int32_t fd = path_cmd(S09_OPEN, S09_O_RDWR | S09_O_CREAT | S09_O_TRUNC, path, &err);

	for (uint32_t off = 0; fd >= 0 && off < n; off += 0x1000u) {
		uint32_t k = n - off < 0x1000u ? n - off : 0x1000u;

		(void)cmd(S09_WRITE, (uint32_t)fd, k, (uint16_t)k, NULL, NULL, data + off, k, &err);
	}
	CHECK(fd >= 0 && cmd(S09_CLOSE, (uint32_t)fd, 0, 0, NULL, NULL, NULL, 0, &err) == 0,
	      "%s written", path);
}

/* S-09.082: the slave's copy of a file start's /os.ldr, checked against
 * the release's length and CRC-32. */
static void test_os_file(void)
{
	static uint8_t img[20000], got[20000];
	uint32_t err, crc;
	int32_t fd;

	for (size_t i = 0; i < sizeof img; i++)
		img[i] = (uint8_t)(i * 7u + 3u);
	crc = wire_crc32(0u, img, sizeof img);
	CHECK(wire_s09_os_file(&S, wire_crc32, NULL, got, sizeof got, sizeof img, crc) ==
	      WIRE_OS_FILE_UNREAD, "os.ldr: missing");
	CHECK(path_cmd(S09_MKDIR, 0755, "os.ldr", &err) == 0, "a directory");
	CHECK(wire_s09_os_file(&S, wire_crc32, NULL, got, sizeof got, sizeof img, crc) ==
	      WIRE_OS_FILE_UNREAD, "os.ldr: a directory");
	CHECK(path_cmd(S09_RMDIR, 0, "os.ldr", &err) == 0, "removed");
	fd = path_cmd(S09_OPEN, S09_O_RDWR | S09_O_CREAT | S09_O_TRUNC, "os.ldr", &err);
	for (uint32_t off = 0; fd >= 0 && off < sizeof img; off += 0x1000u) {
		uint32_t k = (uint32_t)sizeof img - off < 0x1000u ? (uint32_t)sizeof img - off : 0x1000u;

		(void)cmd(S09_WRITE, (uint32_t)fd, k, (uint16_t)k, NULL, NULL, img + off, k, &err);
	}
	CHECK(fd >= 0 && cmd(S09_CLOSE, (uint32_t)fd, 0, 0, NULL, NULL, NULL, 0, &err) == 0,
	      "os.ldr written");
	memset(got, 0, sizeof got);
	CHECK(wire_s09_os_file(&S, wire_crc32, NULL, got, sizeof got, sizeof img, crc) ==
	      WIRE_OS_FILE_OK && !memcmp(got, img, sizeof img), "os.ldr: read whole");
	CHECK(wire_s09_os_file(&S, wire_crc32, bad_crc, got, sizeof got, sizeof img, crc) ==
	      WIRE_OS_FILE_OK, "a fast CRC that differs is computed again");
	CHECK(wire_s09_os_file(&S, wire_crc32, NULL, got, sizeof got, sizeof img, crc ^ 1u) ==
	      WIRE_OS_FILE_CRC, "another CRC");
	CHECK(wire_s09_os_file(&S, wire_crc32, NULL, got, sizeof got, sizeof img - 1u, crc) ==
	      WIRE_OS_FILE_LENGTH, "another length");
	CHECK(wire_s09_os_file(&S, wire_crc32, NULL, got, sizeof img - 1u, sizeof img, crc) ==
	      WIRE_OS_FILE_LENGTH, "over the buffer");
	CHECK(path_cmd(S09_UNLINK, 0, "os.ldr", &err) == 0, "removed");

	/* S-09.083: no /os.ldr, the release's copy among the files of /os,
	 * another of the same length first */
	CHECK(path_cmd(S09_MKDIR, 0755, "os", &err) == 0, "/os");
	img[0] ^= 0xFFu;
	put_file("os/a.ldr", img, sizeof img);
	img[0] ^= 0xFFu;
	put_file("os/b.ldr", img, 100u);
	put_file("os/c.ldr", img, sizeof img);
	memset(got, 0, sizeof got);
	CHECK(wire_s09_os_file(&S, wire_crc32, NULL, got, sizeof got, sizeof img, crc) ==
	      WIRE_OS_FILE_OK && !memcmp(got, img, sizeof img), "S-09.083 found in /os");
	CHECK(wire_s09_os_file(&S, wire_crc32, NULL, got, sizeof got, sizeof img, crc ^ 1u) ==
	      WIRE_OS_FILE_UNREAD, "S-09.083 none of /os: /os.ldr's result");
	CHECK(path_cmd(S09_UNLINK, 0, "os/a.ldr", &err) == 0 &&
	      path_cmd(S09_UNLINK, 0, "os/b.ldr", &err) == 0 &&
	      path_cmd(S09_UNLINK, 0, "os/c.ldr", &err) == 0 &&
	      path_cmd(S09_RMDIR, 0, "os", &err) == 0, "/os removed");
}

static void test_unmount(void)
{
	uint32_t err;

	CHECK(cmd(S09_UNMOUNT, 0, 0, 0, NULL, NULL, NULL, 0, &err) == 1 && err == 0,
	      "unmount: 1");
	CHECK(path_cmd(S09_STAT, 0, "e", &err) == -1 && err == S09_ENODEV,
	      "after the unmount: ENODEV");
	CHECK(cmd(S09_MOUNT, 0, 0, 0, NULL, NULL, NULL, 0, &err) == 0 &&
	      path_cmd(S09_STAT, 0, "e", &err) == 0, "mounted again");
}

/* S-12.052, S-09.041: a refused volume fails every file command */
static void test_refused(void)
{
	size_t len = 8u * WB_YAFFS_SECTORS_PER_BLOCK * WB_SECTOR_SIZE;
	uint8_t *img = tl_xmalloc(len);
	struct wb_blockdev rbd;
	struct wb_ramdisk rram;
	struct wire_yaffs ry;
	struct wire_s09 rs;
	struct s09_req r = { S09_STAT, 0, 0, 0, 0 };
	FILE *f = fopen(tl_vector_path("s12/yaffs_big_endian.img"), "rb");
	uint32_t err = 0;

	CHECK(f != NULL && fread(img, 1, len, f) == len, "big-endian image");
	if (f != NULL)
		fclose(f);
	wb_ramdisk_init(&rbd, &rram, img, (uint32_t)(len / 512u));
	CHECK(wire_yaffs_add(&ry, "/refused", &rbd, 0, 7), "add");
	ry.dev.param.n_reserved_blocks = 2;
	wire_s09_init(&rs, &ry);
	r.cmd = S09_MOUNT;
	CHECK(wire_s09_file(&rs, &r, NULL, NULL, NULL, 0, rec, &err) < 0,
	      "mount of a refused volume: negative");
	r.cmd = S09_STAT;
	CHECK(wire_s09_file(&rs, &r, "/refused/x", NULL, NULL, 0, rec, &err) == -1 &&
	      err == S09_ENODEV, "a refused volume: ENODEV");
	wire_yaffs_detach(&ry);
	free(img);
}

int main(void)
{
	size_t len = (size_t)BLOCKS * 128u * 4096u;
	uint8_t *vol = calloc(1, len);

	if (vol == NULL)
		return 2;
	wb_ramdisk_init(&bd, &ram, vol, (uint32_t)(len / 512u));
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &bd, 0, BLOCKS - 1), "add");
	wire_s09_init(&S, &Y);
	test_errno();
	test_unmounted();
	test_files();
	test_dirs();
	test_space();
	test_reads();
	test_os_file();
	test_unmount();
	test_refused();
	wire_s09_release(&S);
	wire_yaffs_detach(&Y);
	free(vol);
	return tl_done("test_wire_s09");
}
