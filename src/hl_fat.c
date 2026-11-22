/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The host link's FAT root (waffle/hl_fat.h). */
#include "waffle/hl_fat.h"

#include "waffle/bytes.h"
#include "wb_fatfs.h"

static int fr(FRESULT r)
{
	if (r == FR_OK)
		return HL_OK;
	if (r == FR_NO_FILE || r == FR_NO_PATH)
		return HL_E_NOT_FOUND;
	if (r == FR_EXIST)
		return HL_E_EXISTS;
	if (r == FR_INVALID_NAME)
		return HL_E_PATH;
	if (r == FR_DENIED || r == FR_WRITE_PROTECTED)
		return HL_E_REFUSED;
	return r == FR_NO_FILESYSTEM ? HL_E_FS : HL_E_IO;
}

/* FatFs no longer holds this volume: what was open on it is gone. */
static void lost(struct hl_fat *f)
{
	f->mounted = f->writing = f->reading = false;
}

int hlfat_mount(struct hl_fat *f)
{
	FRESULT r;

	if (f->mounted && wb_fatfs_drive() == f->dev)
		return HL_OK;
	lost(f);
	if (f->busy != NULL && f->busy(f->ctx))
		return HL_E_BUSY;
	wb_fatfs_attach(f->dev);
	r = f_mount(&f->fs, "", 1);
	if (r != FR_OK) {
		wb_fatfs_attach(NULL);
		return r == FR_NO_FILESYSTEM ? HL_E_FS : HL_E_IO;
	}
	f->mounted = true;
	return HL_OK;
}

/* The change on the medium, its cache flushed. */
static int settle(struct hl_fat *f, int rc)
{
	if (rc == HL_OK && f->flush != NULL && f->flush(f->ctx) != 0)
		rc = HL_E_IO;
	return rc;
}

static void drop_reader(struct hl_fat *f)
{
	if (f->reading)
		(void)f_close(&f->rd);
	f->reading = false;
}

static void from_fno(const FILINFO *i, struct hl_stat *st)
{
	st->type = (i->fattrib & AM_DIR) != 0u ? 2u : 1u;
	st->size = st->type == 2u ? 0u : (uint32_t)i->fsize;
	st->mtime = (uint32_t)i->fdate << 16 | i->ftime;	/* FAT's date and time */
}

int hlfat_stat(struct hl_fat *f, const char *rest, struct hl_stat *st)
{
	FILINFO i;
	int rc = hlfat_mount(f);

	st->type = 2u;
	st->size = st->mtime = 0u;
	if (rc != HL_OK || *rest == '\0')
		return rc;
	rc = fr(f_stat(rest, &i));
	if (rc == HL_OK)
		from_fno(&i, st);
	return rc;
}

int hlfat_list(struct hl_fat *f, const char *rest, uint32_t start, hl_each_fn each,
	       void *arg, bool *end)
{
	static DIR dir;
	static FILINFO i;
	struct hl_stat st;
	uint32_t k = 0;
	int rc = hlfat_mount(f);

	*end = false;
	if (rc != HL_OK)
		return rc;
	rc = fr(f_opendir(&dir, rest));
	while (rc == HL_OK) {
		rc = fr(f_readdir(&dir, &i));
		if (rc != HL_OK || i.fname[0] == '\0') {
			*end = rc == HL_OK;
			break;
		}
		from_fno(&i, &st);
		if (k++ >= start && !each(arg, i.fname, &st))
			break;
	}
	(void)f_closedir(&dir);
	return rc;
}

/* Reads go on from where the last one ended without opening the file
 * again, so a whole file read in chunks walks its clusters once. */
int hlfat_read(struct hl_fat *f, const char *rest, uint32_t off, uint8_t *buf,
	       uint32_t n, uint32_t *got)
{
	const size_t len = wb_strlen(rest);
	UINT br = 0;
	int rc = hlfat_mount(f);

	*got = 0;
	if (rc != HL_OK)
		return rc;
	if (!f->reading || len >= sizeof f->rd_path || off != f->rd_pos ||
	    !wb_equal((const uint8_t *)f->rd_path, (const uint8_t *)rest, len + 1u)) {
		drop_reader(f);
		if (len >= sizeof f->rd_path)
			return HL_E_PATH;
		rc = fr(f_open(&f->rd, rest, FA_READ));
		if (rc == HL_OK)
			rc = fr(f_lseek(&f->rd, off));
		if (rc != HL_OK) {
			if (rc != HL_E_NOT_FOUND && rc != HL_E_PATH)
				(void)f_close(&f->rd);
			return rc;
		}
		f->reading = true;
		wb_copy((uint8_t *)f->rd_path, (const uint8_t *)rest, len + 1u);
	}
	rc = fr(f_read(&f->rd, buf, n, &br));
	*got = (uint32_t)br;
	f->rd_pos = off + *got;
	if (rc != HL_OK)
		drop_reader(f);
	return rc;
}

int hlfat_create(struct hl_fat *f, const char *rest)
{
	int rc = hlfat_mount(f);

	if (rc != HL_OK)
		return rc;
	drop_reader(f);
	hlfat_abandon(f);
	/* the entry on the medium at once: a put cut short leaves this file */
	rc = fr(f_open(&f->wr, rest, FA_WRITE | FA_CREATE_ALWAYS));
	if (rc == HL_OK && f_sync(&f->wr) != FR_OK) {
		(void)f_close(&f->wr);
		rc = HL_E_IO;
	}
	f->writing = rc == HL_OK;
	return rc;
}

int hlfat_write(struct hl_fat *f, const uint8_t *b, uint32_t n)
{
	UINT bw = 0;
	int rc = hlfat_mount(f);

	if (rc != HL_OK)
		return rc;
	if (!f->writing)
		return HL_E_IO;		/* lost with its mount */
	rc = fr(f_write(&f->wr, b, n, &bw));
	return rc == HL_OK && bw != n ? HL_E_IO : rc;	/* short: the volume full */
}

int hlfat_finish(struct hl_fat *f)
{
	int rc = hlfat_mount(f);

	if (rc != HL_OK)
		return rc;
	if (!f->writing)
		return HL_E_IO;
	f->writing = false;
	return settle(f, fr(f_close(&f->wr)));
}

void hlfat_abandon(struct hl_fat *f)
{
	if (f->writing && f->mounted && wb_fatfs_drive() == f->dev)
		(void)f_close(&f->wr);
	f->writing = false;
}

int hlfat_remove(struct hl_fat *f, const char *rest)
{
	int rc = hlfat_mount(f);

	if (rc != HL_OK)
		return rc;
	drop_reader(f);
	return settle(f, fr(f_unlink(rest)));
}

int hlfat_mkdir(struct hl_fat *f, const char *rest)
{
	int rc = hlfat_mount(f);

	if (rc != HL_OK)
		return rc;
	return settle(f, fr(f_mkdir(rest)));
}

int hlfat_rename(struct hl_fat *f, const char *from, const char *to)
{
	FILINFO i;
	int rc = hlfat_mount(f);

	if (rc != HL_OK)
		return rc;
	drop_reader(f);
	rc = fr(f_rename(from, to));
	if (rc == HL_E_EXISTS && f_stat(to, &i) == FR_OK && (i.fattrib & AM_DIR) == 0u &&
	    f_unlink(to) == FR_OK)
		rc = fr(f_rename(from, to));
	return settle(f, rc);
}
