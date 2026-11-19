/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "waffle/wire_s09.h"

#define CHUNK_DATA 4080u		/* S-12.041 */

void wire_s09_init(struct wire_s09 *s, struct wire_yaffs *y)
{
	memset(s, 0, sizeof(*s));
	s->y = y;
}

void wire_s09_release(struct wire_s09 *s)
{
	for (size_t i = 0; i < WIRE_S09_DIRS; i++) {
		if (s->dir[i] != NULL)
			(void)yaffs_closedir(s->dir[i]);
		s->dir[i] = NULL;
	}
}

uint32_t wire_s09_errno(int yaffs_error)
{
	int e = yaffs_error < 0 ? -yaffs_error : yaffs_error;

	/* the platform's numbers, which yaffsfs uses, to S-09.042's */
	switch (e) {
	case 0:
		return 0;
	case ENOENT:
		return S09_ENOENT;
	case EBADF:
		return S09_EBADF;
	case ENOMEM:
		return S09_ENOMEM;
	case EACCES:
		return S09_EACCES;
	case EFAULT:
		return S09_EFAULT;
	case EBUSY:
		return S09_EBUSY;
	case EEXIST:
		return S09_EEXIST;
	case EXDEV:
		return S09_EXDEV;
	case ENODEV:
		return S09_ENODEV;
	case ENOTDIR:
		return S09_ENOTDIR;
	case EISDIR:
		return S09_EISDIR;
	case ENFILE:
		return S09_ENFILE;
	case ENOSPC:
		return S09_ENOSPC;
	case EROFS:
		return S09_EROFS;
	case ERANGE:
		return S09_ERANGE;
	case ELOOP:
		return S09_ELOOP;
	case ENODATA:
		return S09_ENODATA;
	case ENAMETOOLONG:
		return S09_ENAMETOOLONG;
	case ENOTEMPTY:
		return S09_ENOTEMPTY;
	default:
		return S09_EINVAL;
	}
}

/* The table's failure result (S-09.041). */
static int32_t failure(uint8_t cmd)
{
	switch (cmd) {
	case S09_OPENDIR:
	case S09_READDIR:
	case S09_FREE_SPACE:
	case S09_TOTAL_SPACE:
		return 0;
	default:
		return -1;
	}
}

/* `path` under /fat32: a mount point with no volume behind it. */
static bool on_fat32(const char *path)
{
	return path != NULL && !strncmp(path, "/fat32", 6) &&
	       (path[6] == '\0' || path[6] == '/');
}

/* A yaffsfs call's int result: itself, or failure() with its errno. */
static int32_t done(uint8_t cmd, int res, uint32_t *err)
{
	if (res >= 0) {
		*err = 0;
		return (int32_t)res;
	}
	*err = wire_s09_errno(yaffsfs_GetLastError());
	return failure(cmd);
}

static int open_flags(uint32_t f)
{
	int o = (f & 3u) == S09_O_WRONLY ? O_WRONLY :
		(f & 3u) == S09_O_RDWR ? O_RDWR : O_RDONLY;

	if (f & S09_O_CREAT)
		o |= O_CREAT;
	if (f & S09_O_EXCL)
		o |= O_EXCL;
	if (f & S09_O_TRUNC)
		o |= O_TRUNC;
	if (f & S09_O_APPEND)
		o |= O_APPEND;
	return o;
}

static int32_t stat_cmd(const char *path, uint8_t *out, uint32_t *err)
{
	struct yaffs_stat st;
	struct s09_stat r;

	if (yaffs_stat(path, &st) != 0)
		return done(S09_STAT, -1, err);
	/* st_dev is yaffsfs's device pointer: device 0 */
	r.dev = 0;
	r.ino = (uint32_t)st.st_ino;
	r.mode = st.st_mode;
	r.nlink = (uint32_t)st.st_nlink;
	r.uid = (uint32_t)st.st_uid;
	r.gid = (uint32_t)st.st_gid;
	r.rdev = st.st_rdev;
	r.size = st.st_size > 0 ? (uint32_t)st.st_size : 0u;	/* 32 bits */
	r.blksize = (uint32_t)st.st_blksize;
	r.blocks = (uint32_t)st.st_blocks;
	r.atime = (uint32_t)st.yst_atime;
	r.mtime = (uint32_t)st.yst_mtime;
	r.ctime = (uint32_t)st.yst_ctime;
	s09_stat_pack(&r, out);
	*err = 0;
	return 0;
}

/* S-12.056: the total, (blocks - reserve) x 128 x 4080, and the free. */
static uint64_t total_bytes(const struct wire_yaffs *y)
{
	uint32_t blocks = y->last_block - y->first_block + 1u;
	uint32_t reserve = y->dev.param.n_reserved_blocks > 0 ?
			   (uint32_t)y->dev.param.n_reserved_blocks : 0u;

	return blocks > reserve ?
	       (uint64_t)(blocks - reserve) * 128u * CHUNK_DATA : 0u;
}

static int32_t space_cmd(struct wire_s09 *s, uint8_t cmd, uint8_t *out,
			 uint32_t *err)
{
	uint64_t bytes = 0;

	if (cmd == S09_TOTAL_SPACE)
		bytes = total_bytes(s->y);
	else if (!wire_yaffs_free_space(s->y, &bytes))
		bytes = 0;
	for (unsigned i = 0; i < 8; i++)
		out[i] = (uint8_t)(bytes >> (8u * i));
	/* the volume's last error, 0 if none */
	*err = wire_s09_errno(yaffsfs_GetLastError());
	return 0;
}

static int32_t statfs_cmd(struct wire_s09 *s, uint8_t *out, uint32_t *err)
{
	struct s09_statfs r;
	uint64_t free_bytes = 0;

	if (!wire_yaffs_free_space(s->y, &free_bytes))
		return done(S09_STATFS, -1, err);
	r.bsize = 512;
	r.blocks = (uint32_t)(total_bytes(s->y) / 512u);
	r.bfree = (uint32_t)(free_bytes / 512u);
	s09_statfs_pack(&r, out);
	*err = 0;
	return 0;
}

static int32_t opendir_cmd(struct wire_s09 *s, const char *path,
			   uint32_t *err)
{
	size_t i = 0, n = strlen(path);

	while (i < WIRE_S09_DIRS && s->dir[i] != NULL)
		i++;
	if (i == WIRE_S09_DIRS) {
		*err = S09_ENFILE;
		return 0;
	}
	if (n >= sizeof(s->dir_path[i])) {
		*err = S09_ENAMETOOLONG;
		return 0;
	}
	s->dir[i] = yaffs_opendir(path);
	if (s->dir[i] == NULL)
		return done(S09_OPENDIR, -1, err);
	memcpy(s->dir_path[i], path, n + 1);
	*err = 0;
	return (int32_t)(i + 1u);
}

/* S-09.043: the type byte is always filled in; yaffsfs leaves d_type 0. */
static uint8_t entry_type(struct wire_s09 *s, uint32_t handle,
			  const char *name)
{
	const char *dir = s->dir_path[handle - 1u];
	size_t d = strlen(dir), n = strlen(name);
	struct yaffs_stat st;
	uint32_t fmt;

	if (d + 1 + n >= sizeof(s->path))
		return 0;
	memcpy(s->path, dir, d);
	s->path[d] = '/';
	memcpy(s->path + d + 1, name, n + 1);
	if (yaffs_lstat(s->path, &st) != 0)
		return 0;
	fmt = (uint32_t)st.st_mode & (uint32_t)S_IFMT;
	if (fmt == (uint32_t)S_IFDIR)
		return (uint8_t)DT_DIR;
	if (fmt == (uint32_t)S_IFLNK)
		return (uint8_t)DT_LNK;
	if (fmt == (uint32_t)S_IFREG)
		return (uint8_t)DT_REG;
	return 0;
}

static yaffs_DIR *dir_of(const struct wire_s09 *s, uint32_t handle)
{
	return handle >= 1u && handle <= WIRE_S09_DIRS ? s->dir[handle - 1u] :
	       NULL;
}

static int32_t readdir_cmd(struct wire_s09 *s, uint32_t handle,
			   uint8_t *out, uint32_t *err)
{
	yaffs_DIR *d = dir_of(s, handle);
	const struct yaffs_dirent *e;
	struct s09_dirent r;
	size_t n;

	if (d == NULL) {
		*err = S09_EBADF;
		return 0;
	}
	yaffsfs_SetError(0);
	e = yaffs_readdir(d);
	if (e == NULL)		/* the end, or a failure with its error */
		return done(S09_READDIR, yaffsfs_GetLastError() != 0 ? -1 : 0,
			    err);
	memset(&r, 0, sizeof(r));
	r.ino = (uint32_t)e->d_ino;
	r.off = (uint32_t)e->d_off;
	r.type = e->d_type;
	if (r.type == 0)
		r.type = entry_type(s, handle, e->d_name);
	n = strlen(e->d_name);
	memcpy(r.name, e->d_name, n < S09_DIRENT_NAME ? n : S09_DIRENT_NAME - 1u);
	s09_dirent_pack(&r, out);
	*err = 0;
	return 1;
}

static int32_t closedir_cmd(struct wire_s09 *s, uint32_t handle,
			    uint32_t *err)
{
	yaffs_DIR *d = dir_of(s, handle);

	if (d == NULL) {
		*err = S09_EBADF;
		return -1;
	}
	s->dir[handle - 1u] = NULL;
	return done(S09_CLOSEDIR, yaffs_closedir(d), err);
}

static int32_t lseek_cmd(const struct s09_req *r, uint32_t *err)
{
	int whence = r->len == 0 ? SEEK_SET : r->len == 1 ? SEEK_CUR :
		     r->len == 2 ? SEEK_END : -1;
	Y_LOFF_T pos;

	if (whence < 0) {
		*err = S09_EINVAL;
		return -1;
	}
	pos = yaffs_lseek((int)r->arg0, (Y_LOFF_T)(int32_t)r->arg1, whence);
	if (pos > 0x7FFFFFFF) {
		*err = S09_ERANGE;	/* past what the result holds */
		return -1;
	}
	return done(S09_LSEEK, (int)pos, err);
}

static int32_t write_cmd(const struct s09_req *r, const uint8_t *data,
			 uint32_t n, uint32_t *err)
{
	int fd = (int)r->arg0, w;
	Y_LOFF_T pos;

	if (r->arg1 < 1u || r->arg1 > S09_WRITE_MAX || r->arg1 > n) {
		*err = S09_EINVAL;
		return -1;
	}
	w = yaffs_write(fd, data, r->arg1);
	if (w >= 0 || yaffsfs_GetLastError() != -EINVAL)
		return done(r->cmd, w, err);
	/* yaffsfs_do_write() replaces a write()'s ENOSPC with EINVAL; its
	 * pwrite() keeps the cause, so the write is tried again as one */
	pos = yaffs_lseek(fd, 0, SEEK_CUR);
	w = pos < 0 ? -1 : yaffs_pwrite(fd, data, r->arg1, pos);
	if (w > 0 && yaffs_lseek(fd, pos + w, SEEK_SET) < 0)
		w = -1;
	return done(r->cmd, w, err);
}

static int32_t utime_cmd(const struct s09_req *r, const char *path,
			 uint32_t *err)
{
	struct yaffs_utimbuf t;

	t.actime = r->cmd == S09_UTIME ? r->arg0 : r->arg1;
	/* 0x36 carries the low 16 bits of the modification time (S-09.041) */
	t.modtime = r->cmd == S09_UTIME ? r->arg1 : r->len;
	return done(r->cmd, r->cmd == S09_UTIME ? yaffs_utime(path, &t) :
			    yaffs_futime((int)r->arg0, &t), err);
}

/* 0x08's result when mounted (S-09.041): 0 when no checkpoint was read,
 * else the way and the objects. */
static int32_t mounted(const struct wire_yaffs *y)
{
	const uint32_t n = y->rep.objects < S09_MOUNT_OBJECTS_MAX ? y->rep.objects :
								 S09_MOUNT_OBJECTS_MAX;

	if (y->rep.way == WIRE_WAY_NONE)
		return 0;
	return (int32_t)(n << 8 | (uint32_t)y->rep.way);
}

_Static_assert(WIRE_WAY_RESTORED == 1 && WIRE_WAY_FOR_WRITE == 6,
	       "the ways are S-09.041's");

/* Files or directories held open: the volume is not mounted again under
 * them. */
static bool busy(const struct wire_s09 *s)
{
	for (size_t i = 0; i < WIRE_S09_DIRS; i++)
		if (s->dir[i] != NULL)
			return true;
	return false;
}

/* 0x08: arg0 1 is the scan before a write (S-12.062): a restored mount
 * is scanned again, an unmounted volume mounted by the scan; a mount with
 * handles open stays restored, and the result says so. */
static int32_t mount_cmd(struct wire_s09 *s, uint32_t arg0)
{
	if (arg0 == S09_MOUNT_FOR_WRITE) {
		if (s->y->mounted)
			(void)wire_yaffs_for_write(s->y, busy(s));
		else
			s->y->write_scanned = true;
	}
	if (wire_yaffs_mount(s->y) != WIRE_MOUNT_OK)
		return -1;
	return mounted(s->y);
}

/* The commands that may write the volume (S-12.062). */
static bool writes(const struct s09_req *r)
{
	switch (r->cmd) {
	case S09_RENAME:
	case S09_UNLINK:
	case S09_MKDIR:
	case S09_RMDIR:
	case S09_WRITE:
	case S09_UTIME:
	case S09_FUTIMENS:
		return true;
	case S09_OPEN:
		return (r->arg0 & 3u) != S09_O_RDONLY ||
		       (r->arg0 & (S09_O_CREAT | S09_O_TRUNC | S09_O_APPEND)) != 0u;
	default:
		return false;
	}
}

int32_t wire_s09_file(void *ctx, const struct s09_req *r, const char *path,
		      const char *path2, const uint8_t *data, uint32_t n,
		      uint8_t *out, uint32_t *err)
{
	struct wire_s09 *s = ctx;

	switch (r->cmd) {
	case S09_MOUNT:
		*err = 0;
		return mount_cmd(s, r->arg0);
	case S09_UNMOUNT:
		wire_s09_release(s);
		wire_yaffs_unmount(s->y);
		*err = 0;
		return 1;
	default:
		break;
	}
	/* a volume not mounted or refused, and /fat32: ENODEV */
	if (!s->y->mounted || wire_yaffs_volume_refused(s->y) ||
	    on_fat32(path) || on_fat32(path2)) {
		*err = S09_ENODEV;
		return failure(r->cmd);
	}
	/* S-12.062: a write that finds the mount restored has the scan made
	 * first, unless handles are open; the restored mount is read only,
	 * so the write then fails with EROFS and nothing reaches the eMMC */
	if (s->y->restored && writes(r)) {
		s->implicit_scans++;
		(void)wire_yaffs_for_write(s->y, busy(s));
		if (!s->y->mounted) {
			*err = S09_ENODEV;
			return failure(r->cmd);
		}
	}
	switch (r->cmd) {
	case S09_ACCESS:
		return done(r->cmd, yaffs_access(path, (int)r->arg0), err);
	case S09_STAT:
		return stat_cmd(path, out, err);
	case S09_STATFS:
		return statfs_cmd(s, out, err);
	case S09_RENAME:
		return done(r->cmd, yaffs_rename(path, path2), err);
	case S09_UNLINK:
		return done(r->cmd, yaffs_unlink(path), err);
	case S09_MKDIR:
		return done(r->cmd, yaffs_mkdir(path, (mode_t)r->arg0), err);
	case S09_RMDIR:
		return done(r->cmd, yaffs_rmdir(path), err);
	case S09_SYNC:
		return done(r->cmd, yaffs_sync(path), err);
	case S09_OPEN:
		return done(r->cmd, yaffs_open(path, open_flags(r->arg0), 0666),
			    err);
	case S09_WRITE:
		return write_cmd(r, data, n, err);
	case S09_LSEEK:
		return lseek_cmd(r, err);
	case S09_CLOSE:
		return done(r->cmd, yaffs_close((int)r->arg0), err);
	case S09_OPENDIR:
		return opendir_cmd(s, path, err);
	case S09_READDIR:
		return readdir_cmd(s, r->arg0, out, err);
	case S09_CLOSEDIR:
		return closedir_cmd(s, r->arg0, err);
	case S09_FREE_SPACE:
	case S09_TOTAL_SPACE:
		return space_cmd(s, r->cmd, out, err);
	case S09_FSYNC:
		return done(r->cmd, yaffs_fsync((int)r->arg0), err);
	case S09_UTIME:
	case S09_FUTIMENS:
		return utime_cmd(r, path, err);
	default:
		*err = S09_EINVAL;
		return -1;
	}
}

/* cppcheck-suppress constParameterPointer ; s09_store_ops fixes the type */
int32_t wire_s09_read(void *ctx, uint32_t fd, uint8_t *buf, uint32_t n,
		      uint32_t *err)
{
	const struct wire_s09 *s = ctx;
	uint32_t got = 0;

	if (!s->y->mounted || wire_yaffs_volume_refused(s->y)) {
		*err = S09_ENODEV;
		return -1;
	}
	while (got < n) {
		int r = yaffs_read((int)fd, buf + got, n - got);

		if (r < 0) {
			*err = wire_s09_errno(yaffsfs_GetLastError());
			return -1;
		}
		if (r == 0)
			break;
		got += (uint32_t)r;
	}
	*err = 0;
	return (int32_t)got;
}

enum wire_os_file wire_s09_os_file(struct wire_s09 *s, s03_crc32_fn crc32,
				   s03_crc32_fn crc32_fast, uint8_t *buf,
				   uint32_t cap, uint32_t len, uint32_t crc)
{
	struct yaffs_stat st;
	uint32_t got = 0, sum;
	int fd;

	if (!s->y->mounted || wire_yaffs_volume_refused(s->y) ||
	    !wire_yaffs_full_path(s->y, "os.ldr", s->path) ||
	    yaffs_stat(s->path, &st) != 0 || (st.st_mode & S_IFMT) == S_IFDIR)
		return WIRE_OS_FILE_UNREAD;
	if (st.st_size < 0 || (uint64_t)st.st_size != len || len > cap)
		return WIRE_OS_FILE_LENGTH;
	fd = yaffs_open(s->path, O_RDONLY, 0);
	if (fd < 0)
		return WIRE_OS_FILE_UNREAD;
	while (got < len) {
		const int r = yaffs_read(fd, buf + got, len - got);

		if (r <= 0)
			break;
		got += (uint32_t)r;
	}
	(void)yaffs_close(fd);
	if (got != len)
		return WIRE_OS_FILE_UNREAD;
	sum = crc32_fast != NULL ? crc32_fast(0u, buf, len) : ~crc;
	if (sum != crc)
		sum = crc32(0u, buf, len);
	return sum == crc ? WIRE_OS_FILE_OK : WIRE_OS_FILE_CRC;
}
