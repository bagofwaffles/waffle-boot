/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "wb_fatfs.h"

#include "waffle/bytes.h"
#include "waffle/wire_fat.h"

#define LOGS "logs"

bool wire_fat_mount(struct wire_fat *f, const struct wb_blockdev *bd)
{
	memset(f, 0, sizeof(*f));
	wb_fatfs_attach(bd);
	f->mounted = f_mount(&f->fs, "", 1) == FR_OK;
	if (!f->mounted)
		wb_fatfs_attach(NULL);
	return f->mounted;
}

void wire_fat_unmount(struct wire_fat *f)
{
	wire_fat_close_file(f);
	if (f->logs != NULL)
		(void)yaffs_closedir(f->logs);
	f->logs = NULL;
	if (f->mounted)
		(void)f_mount(NULL, "", 0);
	f->mounted = false;
	wb_fatfs_attach(NULL);
}

bool wire_fat_format(struct wire_fat *f, const struct wb_blockdev *bd)
{
	memset(f, 0, sizeof(*f));
	wb_fatfs_attach(bd);
	if (wb_fatfs_format(f->buf, (unsigned int)sizeof(f->buf)) != FR_OK)
		return false;
	return wire_fat_mount(f, bd);
}

/* ---- names and stamps (S-12.012, S-12.016) ---- */

/* The UTF-16 units of a UTF-8 name; 0 when it is not valid UTF-8. */
static size_t utf16_units(const char *s, size_t len)
{
	size_t units = 0, i = 0;

	while (i < len) {
		uint8_t c = (uint8_t)s[i];
		size_t n = c < 0x80u ? 1u : (c & 0xE0u) == 0xC0u ? 2u :
			   (c & 0xF0u) == 0xE0u ? 3u : (c & 0xF8u) == 0xF0u ? 4u : 0u;
		uint32_t cp = n == 1u ? c : n == 2u ? (c & 0x1Fu) :
			      n == 3u ? (c & 0x0Fu) : (c & 0x07u);

		if (n == 0u || i + n > len)
			return 0;
		for (size_t k = 1; k < n; k++) {
			uint8_t t = (uint8_t)s[i + k];

			if ((t & 0xC0u) != 0x80u)
				return 0;
			cp = cp << 6 | (t & 0x3Fu);
		}
		/* overlong forms, surrogates and values past U+10FFFF */
		if ((n == 2u && cp < 0x80u) || (n == 3u && cp < 0x800u) ||
		    (n == 4u && (cp < 0x10000u || cp > 0x10FFFFu)) ||
		    (cp >= 0xD800u && cp <= 0xDFFFu))
			return 0;
		units += n == 4u ? 2u : 1u;
		i += n;
	}
	return units;
}

static bool name_ok(const char *name, size_t len)
{
	size_t units;

	if (len == 0)
		return false;
	for (size_t i = 0; i < len; i++) {
		uint8_t c = (uint8_t)name[i];

		if (c < 0x20u || c == 0x7Fu || c == '"' || c == '*' ||
		    c == '/' || c == ':' || c == '<' || c == '>' ||
		    c == '?' || c == '\\' || c == '|')
			return false;
	}
	if (name[len - 1] == ' ' || name[len - 1] == '.')
		return false;
	units = utf16_units(name, len);
	return units != 0 && units <= WIRE_FAT_NAME_MAX;
}

bool wire_fat_name_ok(const char *name)
{
	return name_ok(name, wb_strlen(name));
}

/* Every '/'-separated component of path passes S-12.012. */
static bool path_ok(const char *path)
{
	const char *p = path;

	for (;;) {
		size_t n = 0;

		while (p[n] != '\0' && p[n] != '/')
			n++;
		if (!name_ok(p, n))
			return false;
		if (p[n] == '\0')
			return true;
		p += n + 1;
	}
}

bool wire_fat_stamp(uint64_t utc, uint16_t *date, uint16_t *time)
{
	uint64_t days = utc / 86400u;
	uint32_t sec = (uint32_t)(utc % 86400u);
	/* the civil date of a day count (proleptic Gregorian, from 0000-03-01) */
	uint64_t z = days + 719468u;
	uint64_t era = z / 146097u;
	uint32_t doe = (uint32_t)(z - era * 146097u);
	uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
	uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
	uint32_t mp = (5u * doy + 2u) / 153u;
	uint32_t day = doy - (153u * mp + 2u) / 5u + 1u;
	uint32_t month = mp < 10u ? mp + 3u : mp - 9u;
	uint64_t year = (uint64_t)yoe + era * 400u + (month <= 2u ? 1u : 0u);

	if (year > 2107u) {
		*date = (uint16_t)(127u << 9 | 12u << 5 | 31u);
		*time = (uint16_t)(23u << 11 | 59u << 5 | 29u);
		return false;
	}
	if (year < 1980u)
		year = 1980u;
	*date = (uint16_t)((uint32_t)(year - 1980u) << 9 | month << 5 | day);
	*time = (uint16_t)((sec / 3600u) << 11 | (sec / 60u % 60u) << 5 |
			   (sec % 60u) / 2u);
	return true;
}

/* ---- writes ---- */

bool wire_fat_mkdir(const struct wire_fat *f, const char *path)
{
	FRESULT r;

	if (!f->mounted || !path_ok(path))
		return false;
	r = f_mkdir(path);
	return r == FR_OK || r == FR_EXIST;
}

static bool set_stamp(const char *path, uint64_t utc)
{
	FILINFO fno;

	memset(&fno, 0, sizeof(fno));
	(void)wire_fat_stamp(utc, &fno.fdate, &fno.ftime);
	return f_utime(path, &fno) == FR_OK;
}

bool wire_fat_write_file(struct wire_fat *f, const char *path,
			 const uint8_t *data, size_t len, bool stamp,
			 uint64_t utc)
{
	bool ok;

	if (!f->mounted || f->open || !path_ok(path) ||
	    f_open(&f->fil, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
		return false;
	ok = true;
	while (ok && len != 0) {
		UINT n = len > 0x8000u ? 0x8000u : (UINT)len, done = 0;

		ok = f_write(&f->fil, data, n, &done) == FR_OK && done == n;
		data += n;
		len -= n;
	}
	/* the close stamps the fixed clock; an explicit time comes after */
	ok = f_close(&f->fil) == FR_OK && ok;
	if (ok && stamp)
		ok = set_stamp(path, utc);
	if (!ok)
		(void)f_unlink(path);
	return ok;
}

/* ---- reads (S-11.046) ---- */

/* cppcheck-suppress constParameterPointer ; s11_root_fn fixes the type */
bool wire_fat_root(void *ctx, size_t index, char *name, size_t cap,
		   bool *is_dir)
{
	const struct wire_fat *f = ctx;
	DIR dir;
	FILINFO fno;
	bool found = false;
	size_t n;

	if (!f->mounted || cap == 0 || f_opendir(&dir, "") != FR_OK)
		return false;
	for (size_t i = 0; i <= index; i++) {
		found = f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0';
		if (!found)
			break;
	}
	(void)f_closedir(&dir);
	if (!found)
		return false;
	n = wb_strlen(fno.fname);
	if (n >= cap)
		n = cap - 1;
	memcpy(name, fno.fname, n);
	name[n] = '\0';
	*is_dir = (fno.fattrib & AM_DIR) != 0;
	return true;
}

static bool fat_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	struct wire_fat *f = ctx;
	UINT done = 0;

	if (!f->open || n > 0xFFFFFFFFu || f_lseek(&f->fil, off) != FR_OK)
		return false;
	return f_read(&f->fil, buf, (UINT)n, &done) == FR_OK && done == n;
}

static bool fat_write(void *ctx, uint32_t off, const uint8_t *buf, size_t n)
{
	struct wire_fat *f = ctx;
	UINT done = 0;

	/* in place only: the file never grows */
	if (!f->open || off > f_size(&f->fil) || n > f_size(&f->fil) - off ||
	    f_lseek(&f->fil, off) != FR_OK)
		return false;
	return f_write(&f->fil, buf, (UINT)n, &done) == FR_OK && done == n;
}

static bool open_file(struct wire_fat *f, const char *path, BYTE mode,
		      struct wire_source *src, uint32_t *size)
{
	if (!f->mounted || f->open || f_open(&f->fil, path, mode) != FR_OK)
		return false;
	f->open = true;
	/* Fast seek: a seek back, as the in-place recovery makes one for
	 * every piece it writes, would otherwise follow the cluster chain
	 * from the file's start. A file in more fragments than the map holds,
	 * or whose chain does not end where its size says, seeks the slow
	 * way. */
	(void)wb_fatfs_link_map(&f->fil, f->clmt, WIRE_FAT_CLMT);
	*size = (uint32_t)f_size(&f->fil);
	src->read = fat_read;
	src->ctx = f;
	src->write = (mode & FA_WRITE) != 0 ? fat_write : NULL;
	return true;
}

bool wire_fat_open_file(struct wire_fat *f, const char *path,
			struct wire_source *src, uint32_t *size)
{
	return open_file(f, path, FA_READ, src, size);
}

bool wire_fat_open_package(struct wire_fat *f, const char *path,
			   struct wire_source *src, uint32_t *size)
{
	if (f->mounted)
		(void)f_chmod(path, 0, AM_RDO);
	return open_file(f, path, FA_READ | FA_WRITE, src, size);
}

void wire_fat_close_file(struct wire_fat *f)
{
	if (f->open)
		(void)f_close(&f->fil);
	f->open = false;
}

/* ---- struct s14_export_ops (S-14.051, S-14.052) ---- */

static void ex_rewind(void *ctx)
{
	struct wire_fat *f = ctx;

	if (f->logs != NULL)
		(void)yaffs_closedir(f->logs);
	f->logs = NULL;
	if (wire_yaffs_full_path(f->yaffs, LOGS, f->ypath))
		f->logs = yaffs_opendir(f->ypath);
}

static bool ex_next(void *ctx, struct s14_export_entry *e)
{
	struct wire_fat *f = ctx;
	const struct yaffs_dirent *d;
	struct yaffs_stat st;
	size_t n, m;

	if (f->logs == NULL)
		return false;
	d = yaffs_readdir(f->logs);
	if (d == NULL)
		return false;
	n = wb_strlen(d->d_name);
	if (n > S14_EXPORT_NAME_MAX)
		n = S14_EXPORT_NAME_MAX;	/* yaffs2 holds no longer one */
	memcpy(f->name, d->d_name, n);
	f->name[n] = '\0';
	e->name = f->name;
	e->regular = false;
	e->mtime = 0;
	e->size = 0;
	if (!wire_yaffs_full_path(f->yaffs, LOGS, f->ypath))
		return true;
	m = wb_strlen(f->ypath);
	if (m + 1 + n >= sizeof(f->ypath))
		return true;
	f->ypath[m] = '/';
	memcpy(f->ypath + m + 1, f->name, n + 1);
	if (yaffs_lstat(f->ypath, &st) == 0) {
		e->regular = (st.st_mode & S_IFMT) == S_IFREG;
		e->mtime = (uint32_t)st.yst_mtime;
		e->size = st.st_size > 0 ? (uint32_t)st.st_size : 0u;
	}
	return true;
}

/* f->path = "logs/<name>"; false for a name S-12.012 refuses. */
static bool export_path(struct wire_fat *f, const char *name)
{
	size_t n = wb_strlen(name);

	if (!wire_fat_name_ok(name) || n > S14_EXPORT_NAME_MAX)
		return false;
	memcpy(f->path, LOGS "/", 5);
	memcpy(f->path + 5, name, n + 1);
	return true;
}

static bool ex_copy(void *ctx, const char *name, uint32_t mtime)
{
	struct wire_fat *f = ctx;
	bool ok = true;
	int fd, n;
	size_t m;

	if (!f->mounted || f->open || !export_path(f, name) ||
	    !wire_fat_mkdir(f, LOGS) ||
	    !wire_yaffs_full_path(f->yaffs, LOGS, f->ypath))
		return false;
	m = wb_strlen(f->ypath);
	if (m + 1 + wb_strlen(name) >= sizeof(f->ypath))
		return false;
	f->ypath[m] = '/';
	memcpy(f->ypath + m + 1, name, wb_strlen(name) + 1);
	fd = yaffs_open(f->ypath, O_RDONLY, 0);
	if (fd < 0)
		return false;
	if (f_open(&f->fil, f->path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
		(void)yaffs_close(fd);
		return false;
	}
	while (ok && (n = yaffs_read(fd, f->buf, (unsigned int)sizeof(f->buf))) != 0) {
		UINT done = 0;

		ok = n > 0 && f_write(&f->fil, f->buf, (UINT)n, &done) == FR_OK &&
		     done == (UINT)n;
	}
	(void)yaffs_close(fd);
	/* closed before anything else: FatFs has no lock against unlinking
	 * an open file (FF_FS_LOCK 0) */
	ok = f_close(&f->fil) == FR_OK && ok;
	return ok && set_stamp(f->path, mtime);
}

static void ex_discard(void *ctx, const char *name)
{
	struct wire_fat *f = ctx;

	/* a refused name was not created; FatFs would map it to another */
	if (f->mounted && !f->open && export_path(f, name))
		(void)f_unlink(f->path);
}

void wire_fat_export_ops(struct wire_fat *f, struct wire_yaffs *y,
			 struct s14_export_ops *ops)
{
	f->yaffs = y;
	ops->rewind = ex_rewind;
	ops->next = ex_next;
	ops->copy = ex_copy;
	ops->discard = ex_discard;
	ops->ctx = f;
}
