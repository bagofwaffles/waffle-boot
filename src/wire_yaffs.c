/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "waffle/bytes.h"
#include "waffle/wire_yaffs.h"

/* yaffsfs.c defines it; yaffsfs.h declares only yaffs_add_device(). */
void yaffs_remove_device(struct yaffs_dev *dev);

#define SECTOR 512u
#define TAGS 0xFF0u			/* the tags in a chunk (S-12.044) */
#define SUMMARY_LEN 1540u		/* S-12.049 */
#define SEQ_FIRST 0x1000u		/* the data range (S-12.047) */
#define SEQ_LAST 0xEFFFFEFFu
#define ID_MAX 0x3FFFFu			/* S-12.046, S-12.050 rule 2 */
#define ID_SUMMARY 0x10u
#define DATA_BYTES 4080u

/* ---- paths ---- */

/* out = <mount>/<rel>, or <mount> for ""; false when too long. */
static bool full(const struct wire_yaffs *y, char *out, const char *rel)
{
	size_t m = wb_strlen(y->mount), r = wb_strlen(rel);

	if (m + 1 + r >= WIRE_YAFFS_PATH_MAX)
		return false;
	memcpy(out, y->mount, m);
	if (r != 0) {
		out[m++] = '/';
		memcpy(out + m, rel, r);
	}
	out[m + r] = '\0';
	return true;
}

/* Appends "/name" to the path of length len; returns the new length, or 0
 * when it does not fit. */
static size_t push(char *path, size_t len, const char *name)
{
	size_t n = wb_strlen(name);

	if (len + 1 + n >= WIRE_YAFFS_PATH_MAX)
		return 0;
	path[len] = '/';
	memcpy(path + len + 1, name, n + 1);
	return len + 1 + n;
}

static bool lstat_kind(const char *path, bool *is_dir, uint32_t *size)
{
	struct yaffs_stat st;

	if (yaffs_lstat(path, &st) != 0)
		return false;
	*is_dir = (st.st_mode & S_IFMT) == S_IFDIR;
	*size = st.st_size > 0 ? (uint32_t)st.st_size : 0u;
	return true;
}

/* Creates every directory of path (a full path) from the mount point on;
 * the last component too when `last`. */
static bool mkdirs(const struct wire_yaffs *y, char *path, bool last)
{
	size_t m = wb_strlen(y->mount), n = wb_strlen(path);
	bool dir = false;
	uint32_t size;

	for (size_t i = m + 1; i <= n; i++) {
		if (path[i] != '/' && (path[i] != '\0' || !last))
			continue;
		path[i] = '\0';
		if (yaffs_mkdir(path, 0755) != 0 &&
		    (!lstat_kind(path, &dir, &size) || !dir)) {
			path[i] = i == n ? '\0' : '/';
			return false;
		}
		path[i] = i == n ? '\0' : '/';
	}
	return true;
}

/* Removes the tree at path (full, of length len), which is changed while
 * the function runs and restored. */
static bool remove_at(char *path, size_t len)
{
	bool dir = false;
	uint32_t size;

	if (!lstat_kind(path, &dir, &size))
		return true;
	if (!dir)
		return yaffs_unlink(path) == 0;
	for (;;) {
		yaffs_DIR *d = yaffs_opendir(path);
		const struct yaffs_dirent *e;
		size_t n = 0;

		if (d == NULL)
			return false;
		e = yaffs_readdir(d);
		if (e != NULL)
			n = push(path, len, e->d_name);
		yaffs_closedir(d);
		if (e == NULL)
			break;
		if (n == 0)
			return false;
		if (!remove_at(path, n))
			return false;
		path[len] = '\0';
	}
	return yaffs_rmdir(path) == 0;
}

/* ---- the guard (S-12.062) ---- */

static int g_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	struct wire_yaffs *y = ctx;
	const int rc = y->bd->read(y->bd->ctx, lba, buf, count);

	y->reads++;
	if (rc != 0)
		y->read_failed++;
	return rc;
}

static bool refuse(struct wire_yaffs *y)
{
	if (y->guarded)
		y->rep.writes_refused++;
	return y->guarded;
}

static int g_write(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
	struct wire_yaffs *y = ctx;

	if (refuse(y))
		return -1;
	return y->bd->write(y->bd->ctx, lba, buf, count);
}

/* bd's reliable write, or its plain write where it has none, as the glue
 * would choose (src/glue/yaffs2/yaffs_blockdev.c) */
static int g_write_reliable(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
	struct wire_yaffs *y = ctx;

	if (refuse(y))
		return -1;
	if (y->bd->write_reliable != NULL)
		return y->bd->write_reliable(y->bd->ctx, lba, buf, count);
	return y->bd->write(y->bd->ctx, lba, buf, count);
}

static int g_erase(void *ctx, uint32_t lba, uint32_t count)
{
	struct wire_yaffs *y = ctx;

	if (refuse(y))
		return -1;
	return y->bd->erase(y->bd->ctx, lba, count);
}

/* bd has an erase that empties a block to zeros; without one the glue
 * writes the zeros itself */
static bool g_erase_zeroes(void *ctx)
{
	const struct wire_yaffs *y = ctx;

	if (y->bd->erase == NULL)
		return false;
	return y->bd->erase_zeroes == NULL || y->bd->erase_zeroes(y->bd->ctx);
}

/* ---- mounting ---- */

bool wire_yaffs_add(struct wire_yaffs *y, const char *mount,
		    struct wb_blockdev *bd, uint32_t first, uint32_t last)
{
	size_t n = wb_strlen(mount);

	if (n == 0 || n >= sizeof(y->mount))
		return false;
	memset(y, 0, sizeof(*y));
	memcpy(y->mount, mount, n + 1);
	y->bd = bd;
	y->first_block = first;
	y->last_block = last;
	y->fd = -1;
	y->guard.ctx = y;
	y->guard.n_sectors = bd->n_sectors;
	y->guard.read = g_read;
	y->guard.write = g_write;
	y->guard.erase = g_erase;
	y->guard.erase_zeroes = g_erase_zeroes;
	y->guard.write_reliable = g_write_reliable;
	if (wb_yaffs_add_blockdev(&y->dev, y->mount, &y->guard, first, last) != 0)
		return false;
	y->added = true;
	return true;
}

static uint32_t since(const struct wire_yaffs *y, uint64_t t0)
{
	uint64_t d;

	if (y->now_us == NULL)
		return 0;
	d = y->now_us() - t0;
	return d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
}

static uint64_t now(const struct wire_yaffs *y)
{
	return y->now_us != NULL ? y->now_us() : 0u;
}

static uint32_t children(const struct yaffs_obj *dir)
{
	const struct list_head *i;
	uint32_t n = 0;

	if (dir == NULL)
		return 0;
	for (i = dir->variant.dir_variant.children.next;
	     i != &dir->variant.dir_variant.children; i = i->next)
		n++;
	return n;
}

/* The objects a walk of the tree meets: yaffs2's, but its unlinked and
 * deleted directories and what they hold. */
static uint32_t objects(const struct yaffs_dev *d)
{
	const uint32_t hidden = 2u + children(d->unlinked_dir) + children(d->del_dir);

	return d->n_obj > (int)hidden ? (uint32_t)d->n_obj - hidden : 0u;
}

/* A mount by the scan, read-write (S-12.050), as without a checkpoint;
 * its time and reads into *us and *reads. */
static bool scan(struct wire_yaffs *y, uint32_t *us, uint32_t *reads)
{
	const uint64_t t0 = now(y);
	const uint32_t r0 = y->reads;
	bool ok;

	y->guarded = false;
	y->restored = false;
	y->dev.param.skip_checkpt_rd = 1;
	ok = yaffs_mount(y->mount) == 0;
	*us = since(y, t0);
	*reads = y->reads - r0;
	return ok;
}

/*
 * S-12.061: the checkpoint restored, then the stale check; on any doubt
 * the mount is undone and the volume scanned. The guard refuses every
 * write throughout, though none is made: a mount writes nothing
 * (S-12.054), and the unmount of a restored mount saves no checkpoint
 * (skip_checkpt_wr) and flushes nothing (nothing was changed).
 */
static bool restore(struct wire_yaffs *y)
{
	struct wire_yaffs_report *rep = &y->rep;
	uint64_t t0 = now(y);
	uint32_t r0 = y->reads, f0 = y->read_failed;

	y->guarded = true;
	y->dev.param.skip_checkpt_rd = 0;
	if (yaffs_mount(y->mount) != 0) {
		y->dev.param.skip_checkpt_rd = 1;
		y->guarded = false;
		return false;
	}
	y->dev.param.skip_checkpt_rd = 1;
	rep->restore_us = since(y, t0);
	rep->restore_reads = y->reads - r0;
	if (!y->dev.is_checkpointed) {
		/* yaffs2 took no checkpoint and scanned on its own */
		rep->way = y->dev.blocks_in_checkpt == 0 ? WIRE_WAY_NO_CKPT :
			   y->read_failed != f0 ? WIRE_WAY_CKPT_UNREADABLE :
						  WIRE_WAY_CKPT_INVALID;
		y->guarded = false;
		return true;
	}
	t0 = now(y);
	r0 = y->reads;
	f0 = y->read_failed;
	if (!wire_ckpt_stale(&y->dev, &rep->stale, NULL, NULL) &&
	    yaffs_remount(y->mount, 0, 1) == 0) {
		rep->check_us = since(y, t0);
		rep->check_reads = y->reads - r0;
		rep->way = WIRE_WAY_RESTORED;
		y->restored = true;
		return true;
	}
	rep->check_us = since(y, t0);
	rep->check_reads = y->reads - r0;
	rep->way = rep->stale.failed != 0u || y->read_failed != f0 ?
		   WIRE_WAY_CKPT_UNREADABLE :
		   rep->stale.newer != 0u || rep->stale.disagree != 0u ||
		   rep->stale.alloc_used != 0u ? WIRE_WAY_CKPT_STALE : WIRE_WAY_CKPT_INVALID;
	(void)yaffs_unmount2(y->mount, 1);
	return scan(y, &rep->scan_us, &rep->scan_reads);
}

enum wire_mount wire_yaffs_mount(struct wire_yaffs *y)
{
	bool ok;

	if (!y->added)
		return WIRE_MOUNT_FAILED;
	if (y->mounted)
		return WIRE_MOUNT_OK;
	y->refused = wire_yaffs_big_endian(y->bd, y->first_block,
					   y->last_block, y->buf);
	if (y->refused)
		return WIRE_MOUNT_REFUSED;
	memset(&y->rep, 0, sizeof y->rep);
	if (y->may_restore && !y->write_scanned) {
		ok = restore(y);
	} else {
		y->rep.way = y->write_scanned ? WIRE_WAY_FOR_WRITE : WIRE_WAY_NONE;
		ok = scan(y, &y->rep.scan_us, &y->rep.scan_reads);
	}
	if (!ok)
		return WIRE_MOUNT_FAILED;
	y->rep.objects = objects(&y->dev);
	y->mounted = true;
	return WIRE_MOUNT_OK;
}

bool wire_yaffs_for_write(struct wire_yaffs *y, bool busy)
{
	struct wire_yaffs_report *rep = &y->rep;
	bool ok;

	if (!y->mounted || !y->restored) {
		y->write_scanned = true;
		return y->mounted;
	}
	/* files open on the restored mount: it stays, read only */
	if (busy || yaffs_unmount2(y->mount, 0) != 0)
		return false;
	y->write_scanned = true;
	y->mounted = false;
	ok = scan(y, &rep->write_scan_us, &rep->write_scan_reads);
	rep->way = WIRE_WAY_FOR_WRITE;
	if (!ok)
		return false;
	rep->objects = objects(&y->dev);
	y->mounted = true;
	return true;
}

void wire_yaffs_unmount(struct wire_yaffs *y)
{
	if (y->fd >= 0)
		(void)wire_yaffs_staging_close(y);
	if (y->mounted)
		(void)yaffs_unmount(y->mount);
	y->mounted = false;
	y->restored = false;
	y->guarded = false;
}

void wire_yaffs_detach(struct wire_yaffs *y)
{
	wire_yaffs_unmount(y);
	if (y->added)
		yaffs_remove_device(&y->dev);
	y->added = false;
}

bool wire_yaffs_format(struct wire_yaffs *y)
{
	const struct wb_blockdev *bd = y->bd;

	if (!y->added)
		return false;
	wire_yaffs_unmount(y);
	memset(y->buf, 0, sizeof(y->buf));
	for (uint32_t b = y->first_block; b <= y->last_block; b++) {
		uint32_t lba = b * WB_YAFFS_SECTORS_PER_BLOCK;

		if (bd->erase != NULL) {
			if (bd->erase(bd->ctx, lba, WB_YAFFS_SECTORS_PER_BLOCK))
				return false;
			continue;
		}
		for (uint32_t s = 0; s < WB_YAFFS_SECTORS_PER_BLOCK;
		     s += WB_YAFFS_SECTORS_PER_CHUNK) {
			if (bd->write(bd->ctx, lba + s, y->buf,
				      WB_YAFFS_SECTORS_PER_CHUNK))
				return false;
		}
	}
	return wire_yaffs_mount(y) == WIRE_MOUNT_OK;
}

/* ---- S-12.052 ---- */

static bool data_seq(uint32_t seq)
{
	return seq >= SEQ_FIRST && seq <= SEQ_LAST;
}

struct tags {
	uint32_t seq, obj, chunk, bytes;
};

/* Reads sector `s` of chunk `c` of block `b` into its place in buf. */
static bool read_sector(const struct wb_blockdev *bd, uint32_t b, uint32_t c,
			uint32_t s, uint8_t *buf)
{
	uint32_t lba = b * WB_YAFFS_SECTORS_PER_BLOCK +
		       c * WB_YAFFS_SECTORS_PER_CHUNK + s;

	return bd->read(bd->ctx, lba, buf + s * SECTOR, 1) == 0;
}

static bool read_tags(const struct wb_blockdev *bd, uint32_t b, uint32_t c,
		      uint8_t *buf, uint32_t (*word)(const uint8_t *),
		      struct tags *t)
{
	if (!read_sector(bd, b, c, WB_YAFFS_SECTORS_PER_CHUNK - 1u, buf))
		return false;
	t->seq = word(buf + TAGS);
	t->obj = word(buf + TAGS + 4);
	t->chunk = word(buf + TAGS + 8);
	t->bytes = word(buf + TAGS + 12);
	return true;
}

/* A consistent header (S-12.052): its header's type and parent words,
 * read with `word`, equal its tags' (sector 0 of the chunk is read). */
static bool consistent(const struct wb_blockdev *bd, uint32_t b, uint32_t c,
		       uint8_t *buf, uint32_t (*word)(const uint8_t *),
		       const struct tags *t)
{
	uint32_t type = t->obj >> 28;

	if (t->seq == 0 || (t->chunk & 0x80000000u) == 0 || type < 1 ||
	    type > 5 || !read_sector(bd, b, c, 0, buf))
		return false;
	return word(buf) == type && word(buf + 4) == (t->chunk & 0x0FFFFFFFu);
}

/* S-12.050 rule 2 */
static bool ignored(const struct tags *t, uint32_t block_seq)
{
	bool header = (t->chunk & 0x80000000u) != 0;
	uint32_t id = header ? t->obj & 0x0FFFFFFFu : t->obj;

	if (id > ID_MAX || id == ID_SUMMARY || t->seq != block_seq)
		return true;
	return !header && (t->chunk > 0x0FFFFFFFu || t->bytes > DATA_BYTES);
}

/*
 * Chunk 127's summary (S-12.049) into sum (the 127 entries); false when it
 * is not valid. buf takes the whole chunk.
 */
static bool summary(const struct wb_blockdev *bd, uint32_t b, uint32_t seq,
		    uint8_t *buf, uint8_t *sum)
{
	uint32_t lba = b * WB_YAFFS_SECTORS_PER_BLOCK +
		       (WB_YAFFS_CHUNKS_PER_BLOCK - 1u) *
			       WB_YAFFS_SECTORS_PER_CHUNK;
	uint32_t total = 0;

	if (bd->read(bd->ctx, lba, buf, WB_YAFFS_SECTORS_PER_CHUNK) != 0 ||
	    wb_get_le32(buf + TAGS) == 0 || wb_get_le32(buf + TAGS + 4) != ID_SUMMARY ||
	    wb_get_le32(buf + TAGS + 8) != 1 || wb_get_le32(buf + TAGS + 12) != SUMMARY_LEN ||
	    wb_get_le32(buf) != 1 || wb_get_le32(buf + 8) != seq)
		return false;
	for (uint32_t i = 16; i < SUMMARY_LEN; i++)
		total += (uint32_t)buf[i];
	if (wb_get_le32(buf + 12) != total)
		return false;
	memcpy(sum, buf + 16, SUMMARY_LEN - 16);
	return true;
}

/* Condition 1's negation: some chunk of a data block, its tags taken as
 * S-12.049 says and not ignored by rule 2, is a consistent header. */
static bool le_header(const struct wb_blockdev *bd, uint32_t first,
		      uint32_t last, uint8_t *buf)
{
	uint8_t *sum = buf + WB_YAFFS_CHUNK_BYTES;

	for (uint32_t b = first; b <= last; b++) {
		struct tags t0;
		bool has_sum;

		if (!read_tags(bd, b, 0, buf, wb_get_le32, &t0) || !data_seq(t0.seq))
			continue;
		has_sum = summary(bd, b, t0.seq, buf, sum);
		for (uint32_t c = 0; c + 1 < WB_YAFFS_CHUNKS_PER_BLOCK; c++) {
			const uint8_t *e = sum + 12u * c;
			struct tags t;

			if (!read_tags(bd, b, c, buf, wb_get_le32, &t))
				continue;
			if (has_sum && wb_get_le32(e) != 0) {
				t.seq = t0.seq;
				t.obj = wb_get_le32(e);
				t.chunk = wb_get_le32(e + 4);
				t.bytes = wb_get_le32(e + 8);
			}
			if (t.seq != 0 && !ignored(&t, t0.seq) &&
			    consistent(bd, b, c, buf, wb_get_le32, &t))
				return true;
		}
	}
	return false;
}

/* Condition 2: some used chunk is a consistent header read big-endian. */
static bool be_header(const struct wb_blockdev *bd, uint32_t first,
		      uint32_t last, uint8_t *buf)
{
	for (uint32_t b = first; b <= last; b++) {
		for (uint32_t c = 0; c < WB_YAFFS_CHUNKS_PER_BLOCK; c++) {
			struct tags t;
			uint32_t id, parent;

			if (!read_tags(bd, b, c, buf, wb_get_be32, &t))
				continue;
			if (t.seq == 0) {
				if (c == 0)
					break;	/* an empty block (S-12.043) */
				continue;
			}
			id = t.obj & 0x0FFFFFFFu;
			parent = t.chunk & 0x0FFFFFFFu;
			if (data_seq(t.seq) && id >= 1 && id <= ID_MAX &&
			    id != ID_SUMMARY && parent >= 1 && parent <= ID_MAX &&
			    consistent(bd, b, c, buf, wb_get_be32, &t))
				return true;
		}
	}
	return false;
}

bool wire_yaffs_big_endian(const struct wb_blockdev *bd, uint32_t first,
			   uint32_t last, uint8_t *buf)
{
	return !le_header(bd, first, last, buf) &&
	       be_header(bd, first, last, buf);
}

/* ---- single operations ---- */

bool wire_yaffs_full_path(const struct wire_yaffs *y, const char *rel,
			  char *out)
{
	return full(y, out, rel);
}

bool wire_yaffs_stat(struct wire_yaffs *y, const char *rel, bool *is_dir,
		     uint32_t *size)
{
	return y->mounted && full(y, y->path, rel) &&
	       lstat_kind(y->path, is_dir, size);
}

bool wire_yaffs_remove(struct wire_yaffs *y, const char *rel)
{
	return y->mounted && full(y, y->path, rel) &&
	       remove_at(y->path, wb_strlen(y->path));
}

bool wire_yaffs_mkdir(struct wire_yaffs *y, const char *rel)
{
	return y->mounted && full(y, y->path, rel) && mkdirs(y, y->path, true);
}

bool wire_yaffs_list(struct wire_yaffs *y, const char *rel, size_t index,
		     char *name, size_t cap, bool *is_dir, uint32_t *mtime)
{
	yaffs_DIR *d;
	const struct yaffs_dirent *e = NULL;
	struct yaffs_stat st;
	size_t len, n;

	if (!y->mounted || cap == 0 || !full(y, y->path, rel))
		return false;
	d = yaffs_opendir(y->path);
	if (d == NULL)
		return false;
	for (size_t i = 0; i <= index; i++) {
		e = yaffs_readdir(d);
		if (e == NULL)
			break;
	}
	if (e == NULL) {
		yaffs_closedir(d);
		return false;
	}
	n = wb_strlen(e->d_name);
	len = push(y->path, wb_strlen(y->path), e->d_name);
	memcpy(name, e->d_name, n < cap ? n : cap - 1);
	name[n < cap ? n : cap - 1] = '\0';
	yaffs_closedir(d);
	if (len == 0 || yaffs_lstat(y->path, &st) != 0)
		return false;
	*is_dir = (st.st_mode & S_IFMT) == S_IFDIR;
	if (mtime != NULL)
		*mtime = (uint32_t)st.yst_mtime;
	return true;
}

/* S-12.056: yaffs2 counts free chunks less one block more than the
 * reserve and the checkpoint's blocks, so with the reserve of 5 its figure
 * is within (empty-or-reclaimable chunks - 6 x 128) x 4080. */
bool wire_yaffs_free_space(struct wire_yaffs *y, uint64_t *bytes)
{
	Y_LOFF_T n;

	if (!y->mounted)
		return false;
	n = yaffs_freespace(y->mount);
	if (n < 0)
		return false;
	*bytes = (uint64_t)n;
	return true;
}

/* ---- staging (S-11.031, S-11.032) ---- */

static bool staging_path(struct wire_yaffs *y, const char *path)
{
	size_t n;

	if (!y->mounted || !full(y, y->path, "upgrade"))
		return false;
	n = push(y->path, wb_strlen(y->path), path);
	return n != 0;
}

bool wire_yaffs_staging_reset(struct wire_yaffs *y)
{
	if (!wire_yaffs_remove(y, "upgrade"))
		return false;
	return yaffs_mkdir(y->path, 0755) == 0;
}

bool wire_yaffs_staging_mkdir(struct wire_yaffs *y, const char *path)
{
	return staging_path(y, path) && mkdirs(y, y->path, true);
}

bool wire_yaffs_staging_open(struct wire_yaffs *y, const char *path)
{
	if (y->fd >= 0 || !staging_path(y, path) || !mkdirs(y, y->path, false))
		return false;
	y->fd = yaffs_open(y->path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	return y->fd >= 0;
}

bool wire_yaffs_staging_write(struct wire_yaffs *y, const uint8_t *p,
			      size_t n)
{
	while (n != 0) {
		unsigned int k = n > 0x10000u ? 0x10000u : (unsigned int)n;

		if (y->fd < 0 || yaffs_write(y->fd, p, k) != (int)k)
			return false;
		p += k;
		n -= k;
	}
	return y->fd >= 0;
}

bool wire_yaffs_staging_close(struct wire_yaffs *y)
{
	int fd = y->fd;

	y->fd = -1;
	return fd >= 0 && yaffs_close(fd) == 0;
}

/* ---- struct s11_fs_ops ---- */

static bool fs_stat(void *ctx, const char *path, bool *is_dir, uint32_t *size)
{
	return wire_yaffs_stat(ctx, path, is_dir, size);
}

static size_t fs_read(void *ctx, const char *path, uint8_t *buf, size_t cap)
{
	struct wire_yaffs *y = ctx;
	size_t got = 0;
	int fd;

	if (!y->mounted || !full(y, y->path, path))
		return 0;
	fd = yaffs_open(y->path, O_RDONLY, 0);
	if (fd < 0)
		return 0;
	while (got < cap) {
		size_t want = cap - got > 0x10000u ? 0x10000u : cap - got;
		int n = yaffs_read(fd, buf + got, (unsigned int)want);

		if (n <= 0)
			break;
		got += (size_t)n;
	}
	yaffs_close(fd);
	return got;
}

static bool fs_list(void *ctx, const char *dir, size_t index, char *name,
		    size_t cap, bool *is_dir)
{
	return wire_yaffs_list(ctx, dir, index, name, cap, is_dir, NULL);
}

static bool fs_remove(void *ctx, const char *path)
{
	return wire_yaffs_remove(ctx, path);
}

/* yaffs2's rename replaces a file at `to` (S-12.050 rule 5's shadowing). */
static bool fs_move(void *ctx, const char *from, const char *to)
{
	struct wire_yaffs *y = ctx;

	return y->mounted && full(y, y->path, from) && full(y, y->path2, to) &&
	       mkdirs(y, y->path2, false) &&
	       yaffs_rename(y->path, y->path2) == 0;
}

static bool fs_copy(void *ctx, const char *from, const char *to)
{
	struct wire_yaffs *y = ctx;
	bool ok = true;
	int in, out, n;

	if (!y->mounted || !full(y, y->path, from) || !full(y, y->path2, to) ||
	    !mkdirs(y, y->path2, false))
		return false;
	in = yaffs_open(y->path, O_RDONLY, 0);
	if (in < 0)
		return false;
	out = yaffs_open(y->path2, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (out < 0) {
		yaffs_close(in);
		return false;
	}
	while (ok && (n = yaffs_read(in, y->buf, sizeof(y->buf))) > 0)
		ok = yaffs_write(out, y->buf, (unsigned int)n) == n;
	ok = n == 0 && ok;
	yaffs_close(in);
	return yaffs_close(out) == 0 && ok;
}

void wire_yaffs_fs_ops(struct wire_yaffs *y, struct s11_fs_ops *ops)
{
	ops->stat = fs_stat;
	ops->read = fs_read;
	ops->list = fs_list;
	ops->remove = fs_remove;
	ops->move = fs_move;
	ops->copy = fs_copy;
	ops->ctx = y;
}

/* ---- S-15 ---- */

bool wire_yaffs_volume_refused(const struct wire_yaffs *y)
{
	return y->refused;
}

bool wire_yaffs_dir_exists(struct wire_yaffs *y, const char *rel)
{
	bool dir = false;
	uint32_t size;

	return wire_yaffs_stat(y, rel, &dir, &size) && dir;
}

bool wire_yaffs_pack_dir(struct wire_yaffs *y, size_t index, char *name,
			 size_t cap)
{
	bool dir = false;

	for (size_t i = 0; wire_yaffs_list(y, "content/dlc", i, name, cap, &dir,
					   NULL); i++) {
		if (dir && index-- == 0)
			return true;
	}
	return false;
}

size_t wire_yaffs_read_manifest(struct wire_yaffs *y, const char *dir,
				uint8_t *buf, size_t max)
{
	if (!full(y, y->path2, "content/dlc") ||
	    push(y->path2, wb_strlen(y->path2), dir) == 0 ||
	    push(y->path2, wb_strlen(y->path2), "manifest.json") == 0)
		return 0;
	/* fs_read takes a relative path; y->path2 is a full one */
	return fs_read(y, y->path2 + wb_strlen(y->mount) + 1, buf, max);
}
