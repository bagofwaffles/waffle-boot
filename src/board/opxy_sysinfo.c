/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15's system-information trigger on the master: S-12.017's files on the
 * RAM disk, then S-14.051's log export from the slave's /yaffs2 logs/,
 * read through the S-09 file client. The disk is not offered while they
 * are written (S-12.018): a disk the host has is withdrawn first, as a
 * medium change, and offered again after the last sector.
 */
#include "waffle/board/opxy_services.h"

#include <string.h>

#include "ff.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/s10a_msc.h"
#include "waffle/s14_export.h"

#define LOGS "logs"

/* S-12.017: the third-party notices; src/third_party/README.md holds their
 * sources and versions. */
static const char licenses[] =
	"waffle-boot is free software under the GNU General Public License,\n"
	"version 2 or later; this image, which contains yaffs2, is distributed\n"
	"under GPL-2.0. It contains:\n"
	"\n"
	"yaffs2 - Aleph One Ltd. - GPL-2.0 (direct/ headers LGPL-2.1)\n"
	"FatFs - ChaN - FatFs license (BSD-style, 1 clause)\n"
	"TinyUSB - Ha Thach (tinyusb.org) - MIT\n"
	"zlib, minizip - Jean-loup Gailly, Mark Adler, Gilles Vollant - Zlib\n"
	"micro-ecc - Kenneth MacKay - BSD-2-Clause\n"
	"BearSSL - Thomas Pornin - MIT\n"
	"picolibc - Keith Packard and the newlib authors - BSD-3-Clause and\n"
	"  the other permissive notices of newlib\n";

/* --- text --- */

struct text {
	char buf[160];
	size_t len;
};

static void put(struct text *t, const char *s, size_t n)
{
	if (n > sizeof(t->buf) - t->len)
		n = sizeof(t->buf) - t->len;
	memcpy(t->buf + t->len, s, n);
	t->len += n;
}

static void put_str(struct text *t, const char *s)
{
	put(t, s, strlen(s));
}

/* "<key>v. M.m.p[+b]" and LF (S-11's version text). */
static void put_version(struct text *t, const char *key,
			const struct s11_version *v)
{
	char s[S11_VERSION_TEXT_MAX];
	const size_t n = s11_version_text(v, s, sizeof(s));

	put_str(t, key);
	put_str(t, "v. ");
	put(t, s, n);
	put_str(t, "\n");
}

static void version_of(const uint16_t w[4], struct s11_version *v)
{
	v->major = w[0];
	v->minor = w[1];
	v->patch = w[2];
	v->build = w[3];
}

/* --- S-12.017's files --- */

/* A file of one of these names that exists keeps its contents. */
static bool write_new(struct opxy_master *m, const char *path,
		      const char *data, size_t len, bool stamp, uint64_t utc)
{
	if (f_stat(path, NULL) == FR_OK)
		return true;
	return wire_fat_write_file(&m->sv.fat, path, (const uint8_t *)data, len,
				   stamp, utc);
}

static bool system_files(struct opxy_master *m, bool stamp, uint64_t utc)
{
	struct text t;
	struct s11_version v;
	bool ok;

	t.len = 0;
	version_of(m->meta.os_version, &v);
	put_str(&t, "[");
	put_version(&t, "firmware: ", &v);
	t.len--;		/* the bracket goes before the LF */
	put_str(&t, "]\n");
	version_of(m->meta.sw_version, &v);
	put_version(&t, "blackfin: ", &v);
	v.major = OPXY_BL_VERSION_MAJOR;
	v.minor = OPXY_BL_VERSION_MINOR;
	v.patch = OPXY_BL_VERSION_PATCH;
	v.build = OPXY_BL_VERSION_BUILD;
	put_version(&t, "bootloader: ", &v);
	put_version(&t, "backplane: ", &m->sv.copro.psoc);
	put_version(&t, "nordic: ", &m->sv.copro.nordic_app);
	ok = wire_fat_mkdir(&m->sv.fat, "systeminfo") &&
	     write_new(m, "systeminfo/version.txt", t.buf, t.len, stamp, utc);

	/* S-12.017: S the chip serial, OTP item 1 (S-16.095, S-10c.055) */
	t.len = 0;
	put_str(&t, "unit: ");
	put_str(&t, m->meta.serial);
	put_str(&t, "\ndsp: ");
	put_str(&t, m->sv.chip_serial);
	put_str(&t, "\nbackplane: ");
	put(&t, m->sv.copro.psoc_serial, m->sv.copro.psoc_serial_len);
	put_str(&t, "\n");
	ok = ok && write_new(m, "systeminfo/serial.txt", t.buf, t.len, stamp,
			     utc);
	return ok && write_new(m, "systeminfo/licenses.txt", licenses,
			       sizeof(licenses) - 1u, stamp, utc);
}

/* --- S-14.051: the export, from /yaffs2/logs/ over S-09 --- */

struct export {
	struct opxy_master *m;
	size_t index;
	char name[S14_EXPORT_NAME_MAX + 1u];
	char path[sizeof(LOGS) + S14_EXPORT_NAME_MAX + 1u];
	uint8_t buf[S09_READ_CHUNK];
};

static struct export ex;

static void logs_path(const char *name)
{
	memcpy(ex.path, LOGS "/", sizeof(LOGS));
	strcpy(ex.path + sizeof(LOGS), name);
}

static void ex_rewind(void *ctx)
{
	(void)ctx;
	ex.index = 0;
}

static bool ex_next(void *ctx, struct s14_export_entry *e)
{
	struct s09_files *f = &ex.m->sv.files;
	struct s09_stat st;
	uint8_t type;

	(void)ctx;
	if (!s09_files_entry(f, LOGS, ex.index, ex.name, sizeof(ex.name),
			     &type))
		return false;
	ex.index++;
	e->name = ex.name;
	e->regular = type == 8u;
	e->mtime = 0;
	e->size = 0;
	if (e->regular) {
		logs_path(ex.name);
		if (s09_files_stat(f, ex.path, &st) != 0)
			e->regular = false;	/* gone: not a candidate */
		else {
			e->mtime = st.mtime;
			e->size = st.size;
		}
	}
	return true;
}

static bool ex_copy(void *ctx, const char *name, uint32_t mtime)
{
	struct s09_files *f = &ex.m->sv.files;
	FIL fil;
	FILINFO fno;
	UINT w;
	int32_t fd, n;
	bool ok = true;

	(void)ctx;
	if (!wire_fat_name_ok(name) || !wire_fat_mkdir(&ex.m->sv.fat, LOGS))
		return false;
	logs_path(name);
	fd = s09_files_open(f, ex.path, S09_O_RDONLY);
	if (fd < 0)
		return false;
	if (f_open(&fil, ex.path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
		(void)s09_files_close(f, fd);
		return false;
	}
	do {
		n = s09_files_read(f, fd, ex.buf, sizeof(ex.buf));
		if (n < 0 ||
		    (n > 0 && (f_write(&fil, ex.buf, (UINT)n, &w) != FR_OK ||
			       w != (UINT)n)))
			ok = false;
	} while (ok && n > 0);
	ok = f_close(&fil) == FR_OK && ok;
	ok = s09_files_close(f, fd) == 0 && ok;
	/* S-12.016: the yaffs2 file's modification time */
	memset(&fno, 0, sizeof(fno));
	(void)wire_fat_stamp(mtime, &fno.fdate, &fno.ftime);
	return ok && f_utime(ex.path, &fno) == FR_OK;
}

static void ex_discard(void *ctx, const char *name)
{
	(void)ctx;
	logs_path(name);
	(void)f_unlink(ex.path);
}

/* --- the trigger --- */

void opxy_system_info(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;
	struct s14_export_ops ops;
	const bool offered = opxy_usb_up() && s10a_usb_disk()->offered;
	uint64_t utc = 0;
	const bool stamp = opxy_rtc_utc(m, &utc);

	if (!opxy_disk_ready(m))
		return;
	/* S-12.018: withdrawn as a medium change (S-10a.049) */
	if (offered && !opxy_disk_take(m, S10A_WITHDRAW_OWN_WRITE)) {
		s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG,
			    "systeminfo: disk busy");
	} else if (!wire_fat_mount(&sv->fat, &sv->disk.dev)) {
		s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG,
			    "systeminfo: no volume");
	} else {
		if (!system_files(m, stamp, utc))
			s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG,
				    "systeminfo write failed");
		ex.m = m;
		ops.rewind = ex_rewind;
		ops.next = ex_next;
		ops.copy = ex_copy;
		ops.discard = ex_discard;
		ops.ctx = &ex;
		(void)s14_export_run(&ops);
		wire_fat_unmount(&sv->fat);
	}
	/* After the last sector (S-12.018); FatFs keeps nothing back once
	 * each file is closed. */
	if (offered)
		s10a_usb_disk_offer();
}
