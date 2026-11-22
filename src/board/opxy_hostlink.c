/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The host link on the master (waffle/hostlink.h): its roots, the approval
 * screen, and its actions through the code the unit's own menus run.
 *
 *   yaffs2  the drive, as MTP offers it: read, write, list and stat open
 *   os      the installed OS's two copies on the eMMC's boot partitions,
 *           boot1 and boot2: the header sector and the image, read only
 *   user    the OS's FAT volume on the eMMC's user area, read only, with
 *           FatFs on the sectors; fat-region devices alone (the OP-XY)
 *
 * No file call reaches the SPI flash, memory or a raw sector of the drive.
 * SPEC-Q: reading the FAT region and the boot partitions as files is new;
 * S-03.075's footprint names the yaffs2 region and the reset's group only.
 */
#include "waffle/board/opxy_boot.h"

#include <string.h>

#include "waffle/bytes.h"
#include "waffle/drivers/bf70x_usb.h"
#include "waffle/hostlink.h"
#include "waffle/s03_emmc.h"
#include "waffle/s15_lines.h"
#include "bearssl_hash.h"
#include "wb_fatfs.h"

static struct opxy_master *me;	/* the ops' ctx */

/* ---- errors ---- */

static int err_of(const struct s09_files *f)
{
	if (s09_files_slave_lost(f))
		return HL_E_UNREACHABLE;
	switch (f->err) {
	case S09_ENOENT:
		return HL_E_NOT_FOUND;
	case S09_EEXIST:
		return HL_E_EXISTS;
	case S09_ENAMETOOLONG:
		return HL_E_PATH;
	case S09_ENOTEMPTY:
	case S09_EBUSY:
	case S09_EROFS:
		return HL_E_REFUSED;
	default:
		return HL_E_IO;
	}
}

static int drive_ok(struct opxy_master *m)
{
	if (s09_files_slave_lost(&m->sv.files))
		return HL_E_UNREACHABLE;
	return s09_files_volume_refused(&m->sv.files) ? HL_E_REFUSED : HL_OK;
}

/* lost+found keeps its place, as over MTP */
static bool kept(const struct hl_path *p)
{
	return p->root == HL_ROOT_DRIVE && strcmp(p->rest, "lost+found") == 0;
}

/* Each change on the eMMC, not in its cache, before the host is answered. */
static int synced(struct opxy_master *m)
{
	return s09_files_flush(&m->sv.files) == 0 ? HL_OK : HL_E_IO;
}

static void from_stat(const struct s09_stat *s, struct hl_stat *st)
{
	st->type = (s->mode & 0xF000u) == 0x4000u ? 2u : 1u;
	st->size = st->type == 2u ? 0u : s->size;
	st->mtime = s->mtime;
}

/* ---- the installed OS's copies (root os) ---- */

static _Alignas(32) uint8_t os_sector[S03_SECTOR];
static int os_part = -1;
static uint32_t os_at;

/* Sector n of boot partition part, the user area selected again after. */
static int os_read(struct opxy_master *m, unsigned part, uint32_t n)
{
	const struct s03_emmc_ops *e = m->emmc.ops;
	int rc;

	if ((int)part == os_part && n == os_at)
		return HL_OK;
	os_part = -1;
	if (e == NULL || s09_files_slave_lost(&m->sv.files))
		return HL_E_UNREACHABLE;
	rc = e->select(e->ctx, part) == 0 && e->read(e->ctx, n, 1u, os_sector) == 0 ?
	     HL_OK : HL_E_IO;
	if (e->select(e->ctx, S03_PART_USER) != 0)
		rc = HL_E_IO;
	if (rc == HL_OK) {
		os_part = (int)part;
		os_at = n;
	}
	return rc;
}

static unsigned os_which(const struct hl_path *p)
{
	return strcmp(p->rest, "boot1") == 0 ? S03_PART_BP1 :
	       strcmp(p->rest, "boot2") == 0 ? S03_PART_BP2 : 0u;
}

/* The header sector and the image it states; the header alone when it
 * states none this bootloader could load. */
static int os_size(struct opxy_master *m, unsigned part, uint32_t *size)
{
	struct s03_header h;
	int rc = os_read(m, part, 0u);

	if (rc != HL_OK)
		return rc;
	s03_header_decode(os_sector, &h);
	*size = S03_SECTOR;
	if (s03_header_mode(&h) != S03_MODE_REFUSED && h.length != 0u &&
	    h.length <= S03_OS_MAX)
		*size += h.length;
	return HL_OK;
}

/* ---- the OS's FAT volume (root user) ---- */

static FATFS ufs;
static FIL ufil;

/* Volume sector s at its user-area sector (S-03.072), read only. */
static int user_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	const struct s03_emmc_ops *e = me->emmc.ops;
	uint8_t *b = buf;

	(void)ctx;
	if (e == NULL)
		return -1;
	while (count > 0u) {
		const uint32_t to = lba < S03_FAT_FIRST ? S03_FAT_FIRST : S03_FAT_SECTORS;
		uint32_t k = to - lba < count ? to - lba : count;

		if (lba >= S03_FAT_SECTORS ||
		    e->read(e->ctx, s03_fat_volume_to_user(lba), k, b) != 0)
			return -1;
		lba += k;
		b += k * S03_SECTOR;
		count -= k;
	}
	return 0;
}

static int user_write(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
	(void)ctx;
	(void)lba;
	(void)buf;
	(void)count;
	return -1;
}

static const struct wb_blockdev user_dev = {
	.ctx = NULL, .n_sectors = S03_FAT_SECTORS, .read = user_read, .write = user_write,
};

/* FatFs's one drive, while the RAM disk does not hold it. */
static int user_mount(struct opxy_master *m)
{
	if (m->sv.fat.mounted)
		return HL_E_BUSY;
	if (s09_files_slave_lost(&m->sv.files))
		return HL_E_UNREACHABLE;
	wb_fatfs_attach(&user_dev);
	if (f_mount(&ufs, "", 1) == FR_OK)
		return HL_OK;
	wb_fatfs_attach(NULL);
	return HL_E_REFUSED;		/* no FAT volume FatFs reads */
}

static void user_unmount(void)
{
	(void)f_mount(NULL, "", 0);
	wb_fatfs_attach(NULL);
}

static int fr_err(FRESULT r)
{
	return r == FR_OK ? HL_OK :
	       r == FR_NO_FILE || r == FR_NO_PATH ? HL_E_NOT_FOUND :
	       r == FR_INVALID_NAME ? HL_E_PATH : HL_E_IO;
}

static void from_fno(const FILINFO *f, struct hl_stat *st)
{
	st->type = (f->fattrib & AM_DIR) != 0u ? 2u : 1u;
	st->size = st->type == 2u ? 0u : (uint32_t)f->fsize;
	st->mtime = (uint32_t)f->fdate << 16 | f->ftime;	/* FAT's date and time */
}

/* ---- the ops ---- */

static bool link_on(void *ctx)
{
	const struct opxy_master *m = ctx;

	return m->sv.menu.host_link;
}

static bool approve_open(void *ctx)
{
	struct opxy_master *m = ctx;

	if (m->sv.working || s15_menu_held(&m->sv.menu) ||
	    !s15_menu_approve_open(&m->sv.menu))
		return false;
	opxy_menu_show(m);
	return true;
}

/* bl_version[4] front:2 device:1 slot:1 reason:2 flags:2 os_version[4] */
static void info(void *ctx, uint8_t o[HL_INFO_LEN])
{
	const struct opxy_master *m = ctx;
	const struct s15_menu *mn = &m->sv.menu;
	const uint16_t bl[4] = { OPXY_BL_VERSION_MAJOR, OPXY_BL_VERSION_MINOR,
				 OPXY_BL_VERSION_PATCH, OPXY_BL_VERSION_BUILD };
	unsigned f = 0;

	for (unsigned i = 0; i < 4u; i++) {
		wb_put_le16(o + 2u * i, bl[i]);
		wb_put_le16(o + 16u + 2u * i, m->meta.os_version[i]);
	}
	wb_put_le16(o + 8, OPXY_FRONT_VERSION);
	o[10] = (uint8_t)opxy_dev(m)->id;
	o[11] = m->sv.boot.running_known ? (uint8_t)m->sv.boot.running_slot : 0xFFu;
	wb_put_le16(o + 12, m->boot_reason);
	f |= mn->option_on ? 0x01u : 0u;
	f |= mn->dev_mode ? 0x02u : 0u;
	f |= mn->color ? 0x04u : 0u;
	f |= mn->mtp_macos ? 0x08u : 0u;
	f |= mn->start_menu ? 0x10u : 0u;
	f |= mn->host_link ? 0x20u : 0u;
	f |= m->slave_present && !s09_files_slave_lost(&m->sv.files) ? 0x40u : 0u;
	f |= m->sv.files.mounted ? 0x80u : 0u;
	wb_put_le16(o + 14, (uint16_t)f);
}

static bool root_ok(void *ctx, unsigned root)
{
	const struct opxy_master *m = ctx;

	return root == HL_ROOT_DRIVE || root == HL_ROOT_OS ||
	       (root == HL_ROOT_USER && wb_has(opxy_dev(m), WB_F_FAT_REGION));
}

static int stat_(void *ctx, const struct hl_path *p, struct hl_stat *st)
{
	struct opxy_master *m = ctx;
	struct s09_stat s;
	FILINFO fno;
	int rc;

	st->type = 2u;
	st->size = st->mtime = 0u;
	if (p->root == HL_ROOT_DRIVE) {
		rc = drive_ok(m);
		if (rc == HL_OK && s09_files_stat(&m->sv.files, p->full, &s) != 0)
			rc = err_of(&m->sv.files);
		if (rc == HL_OK)
			from_stat(&s, st);
		return rc;
	}
	if (*p->rest == '\0')
		return HL_OK;
	if (p->root == HL_ROOT_OS) {
		st->type = 1u;
		return os_which(p) == 0u ? HL_E_NOT_FOUND : os_size(m, os_which(p), &st->size);
	}
	rc = user_mount(m);
	if (rc != HL_OK)
		return rc;
	rc = fr_err(f_stat(p->rest, &fno));
	if (rc == HL_OK)
		from_fno(&fno, st);
	user_unmount();
	return rc;
}

/* The drive's listing: entries from skip on, each one stat'ed. */
struct drive_list {
	struct opxy_master *m;
	const char *dir;
	uint32_t skip;
	hl_each_fn each;
	void *arg;
	bool full;
	int rc;
};

static bool drive_each(void *ctx, const char *name)
{
	static char path[HL_PATH_MAX + 260u];
	struct drive_list *l = ctx;
	struct s09_stat s;
	struct hl_stat st;
	const size_t a = strlen(l->dir), b = strlen(name);

	if (l->skip > 0u) {
		l->skip--;
		return true;
	}
	if (a + b + 2u > sizeof(path))
		return true;			/* past any path this link names */
	memcpy(path, l->dir, a);
	path[a] = '/';
	memcpy(path + a + 1u, name, b + 1u);
	if (s09_files_stat(&l->m->sv.files, path, &s) != 0) {
		l->rc = err_of(&l->m->sv.files);
		return false;
	}
	from_stat(&s, &st);
	if (!l->each(l->arg, name, &st)) {
		l->full = true;
		return false;
	}
	return true;
}

static int list(void *ctx, const struct hl_path *p, uint32_t start, hl_each_fn each,
		void *arg, bool *end)
{
	static const char *const boot[] = { "boot1", "boot2" };
	struct opxy_master *m = ctx;
	struct hl_stat st;
	int rc;

	*end = false;
	if (p->root == HL_ROOT_DRIVE) {
		struct drive_list l = { m, p->full, start, each, arg, false, HL_OK };

		rc = drive_ok(m);
		if (rc != HL_OK)
			return rc;
		if (!s09_files_list(&m->sv.files, p->full, drive_each, &l) && !l.full &&
		    l.rc == HL_OK)
			return err_of(&m->sv.files);
		*end = !l.full && l.rc == HL_OK;
		return l.rc;
	}
	if (p->root == HL_ROOT_OS) {
		if (*p->rest != '\0')
			return HL_E_NOT_FOUND;
		for (uint32_t i = start; i < 2u; i++) {
			st.type = 1u;
			st.mtime = 0u;
			rc = os_size(m, S03_PART_BP1 + i, &st.size);
			if (rc != HL_OK)
				return rc;
			if (!each(arg, boot[i], &st))
				return HL_OK;
		}
		*end = true;
		return HL_OK;
	}
	rc = user_mount(m);
	if (rc == HL_OK) {
		static DIR dir;
		static FILINFO fno;
		uint32_t i = 0;

		rc = fr_err(f_opendir(&dir, p->rest));
		while (rc == HL_OK) {
			rc = fr_err(f_readdir(&dir, &fno));
			if (rc != HL_OK || fno.fname[0] == '\0') {
				*end = rc == HL_OK;
				break;
			}
			from_fno(&fno, &st);
			if (i++ >= start && !each(arg, fno.fname, &st))
				break;
		}
		(void)f_closedir(&dir);
		user_unmount();
	}
	return rc;
}

static int read_(void *ctx, const struct hl_path *p, uint32_t off, uint8_t *buf,
		 uint32_t n, uint32_t *got)
{
	struct opxy_master *m = ctx;
	struct s09_files *f = &m->sv.files;
	uint32_t size;
	int32_t fd, r;
	int rc;

	*got = 0;
	if (p->root == HL_ROOT_DRIVE) {
		rc = drive_ok(m);
		if (rc != HL_OK)
			return rc;
		fd = s09_files_open(f, p->full, S09_O_RDONLY);
		if (fd < 0)
			return err_of(f);
		r = s09_files_seek(f, fd, off) == 0 ? s09_files_read(f, fd, buf, n) : -1;
		rc = r < 0 ? err_of(f) : HL_OK;
		(void)s09_files_close(f, fd);
		*got = r > 0 ? (uint32_t)r : 0u;
		return rc;
	}
	if (p->root == HL_ROOT_OS) {
		const unsigned part = os_which(p);

		if (part == 0u)
			return HL_E_NOT_FOUND;
		rc = os_size(m, part, &size);
		while (rc == HL_OK && *got < n && off < size) {
			const uint32_t in = off % S03_SECTOR;
			uint32_t k = S03_SECTOR - in;

			k = k < n - *got ? k : n - *got;
			k = k < size - off ? k : size - off;
			rc = os_read(m, part, off / S03_SECTOR);
			if (rc == HL_OK) {
				memcpy(buf + *got, os_sector + in, k);
				*got += k;
				off += k;
			}
		}
		return rc;
	}
	rc = user_mount(m);
	if (rc == HL_OK) {
		UINT br = 0;

		rc = fr_err(f_open(&ufil, p->rest, FA_READ));
		if (rc == HL_OK) {
			rc = fr_err(f_lseek(&ufil, off));
			if (rc == HL_OK)
				rc = fr_err(f_read(&ufil, buf, n, &br));
			*got = (uint32_t)br;
			(void)f_close(&ufil);
		}
		user_unmount();
	}
	return rc;
}

/* The whole file's SHA-256 and size, read as GET reads it. */
static int hash(void *ctx, const struct hl_path *p, uint8_t sha[32], uint32_t *size)
{
	static uint8_t buf[4096];
	br_sha256_context c;
	uint32_t got;
	int rc;

	br_sha256_init(&c);
	*size = 0;
	do {
		rc = read_(ctx, p, *size, buf, sizeof(buf), &got);
		br_sha256_update(&c, buf, got);
		*size += got;
		opxy_usb_breathe();
	} while (rc == HL_OK && got == sizeof(buf));
	br_sha256_out(&c, sha);
	return rc;
}

static int32_t put_fd = -1;

static int create(void *ctx, const struct hl_path *p)
{
	struct opxy_master *m = ctx;
	int rc = drive_ok(m);

	if (rc != HL_OK)
		return rc;
	s15_check_files(&m->sv.menu);	/* S-12.062: the scan first, the host waiting */
	put_fd = s09_files_open(&m->sv.files, p->full,
				S09_O_WRONLY | S09_O_CREAT | S09_O_TRUNC);
	return put_fd < 0 ? err_of(&m->sv.files) : HL_OK;
}

static int write_(void *ctx, const uint8_t *b, uint32_t n)
{
	struct opxy_master *m = ctx;

	if (put_fd < 0)
		return HL_E_BAD;
	return s09_files_write(&m->sv.files, put_fd, b, n) == (int32_t)n ? HL_OK :
	       err_of(&m->sv.files);
}

/* On the eMMC, not in its cache, before the check reads it. */
static int finish(void *ctx)
{
	struct opxy_master *m = ctx;
	const int32_t fd = put_fd;

	put_fd = -1;
	if (fd < 0)
		return HL_E_BAD;
	return s09_files_close_sync(&m->sv.files, fd) == 0 ? HL_OK : err_of(&m->sv.files);
}

static void abandon(void *ctx)
{
	struct opxy_master *m = ctx;

	if (put_fd >= 0)
		(void)s09_files_close(&m->sv.files, put_fd);
	put_fd = -1;
}

static int remove_(void *ctx, const struct hl_path *p)
{
	struct opxy_master *m = ctx;
	struct s09_stat s;
	int rc = drive_ok(m);

	if (rc != HL_OK)
		return rc;
	if (kept(p))
		return HL_E_REFUSED;
	s15_check_files(&m->sv.menu);
	if (s09_files_stat(&m->sv.files, p->full, &s) != 0)
		return err_of(&m->sv.files);
	if ((s.mode & 0xF000u) == 0x4000u ? s09_files_rmdir(&m->sv.files, p->full) != 0 :
	    s09_files_unlink(&m->sv.files, p->full) != 0)
		return err_of(&m->sv.files);
	return synced(m);
}

static int mkdir_(void *ctx, const struct hl_path *p)
{
	struct opxy_master *m = ctx;
	int rc = drive_ok(m);

	if (rc != HL_OK)
		return rc;
	s15_check_files(&m->sv.menu);
	return s09_files_mkdir(&m->sv.files, p->full) == 0 ? synced(m) : err_of(&m->sv.files);
}

/* A file in the way is replaced, as a put's last step needs. */
static int rename_(void *ctx, const struct hl_path *from, const struct hl_path *to)
{
	struct opxy_master *m = ctx;
	struct s09_files *f = &m->sv.files;
	struct s09_stat s;
	int rc = drive_ok(m);

	if (rc != HL_OK)
		return rc;
	if (kept(from) || kept(to))
		return HL_E_REFUSED;
	s15_check_files(&m->sv.menu);
	if (s09_files_rename(f, from->full, to->full) == 0)
		return synced(m);
	if (s09_files_stat(f, to->full, &s) != 0 || (s.mode & 0xF000u) == 0x4000u ||
	    s09_files_unlink(f, to->full) != 0 ||
	    s09_files_rename(f, from->full, to->full) != 0)
		return err_of(f);
	return synced(m);
}

/* ---- actions ---- */

static struct hl_path job_path;
static uint8_t job_arg;

static bool unit_busy(const struct opxy_master *m)
{
	const struct s15_menu *mn = &m->sv.menu;

	return m->sv.working || s15_menu_held(mn) || mn->busy ||
	       mn->activity != S15_ACT_IDLE || mn->screen == S15_SCR_STARTING ||
	       mn->screen == S15_SCR_APPROVE || mn->screen == S15_SCR_NONE;
}

static int run(void *ctx, uint8_t action, uint8_t arg, const struct hl_path *p)
{
	struct opxy_master *m = ctx;
	struct hl_stat st;
	int rc;

	if (unit_busy(m))
		return HL_E_BUSY;
	if (p != NULL) {
		rc = stat_(ctx, p, &st);
		if (rc != HL_OK)
			return rc;
		if (st.type != 1u)
			return HL_E_PATH;
		job_path = *p;
		job_path.rest = job_path.full + (p->rest - p->full);
	}
	job_arg = arg;
	switch (action) {
	case HL_A_CONTINUE_OS:
		return s15_menu_continue_check(&m->sv.menu) == S15_CONT_BUSY ? HL_E_BUSY : HL_OK;
	case HL_A_OS_ONCE: {
		/* S-15.194's check first: a start that cannot be taken is no job */
		struct s15_os_file file;

		if (m->sv.env.os_once == NULL || m->sv.env.os_once_check == NULL)
			return HL_E_UNKNOWN;
		return m->sv.env.os_once_check(m, &file) == S15_ONCE_STARTED ? HL_OK :
		       HL_E_REFUSED;
	}
	default:
		return HL_OK;
	}
}

static int set(void *ctx, uint8_t setting, uint8_t value)
{
	struct opxy_master *m = ctx;

	/* a host's change lasts for this start, as a host's key does */
	if (setting == HL_SET_HOST_LINK && value == 0u) {
		m->sv.menu.host_link = false;
		hl_end(&m->sv.hl);
	}
	return HL_OK;
}

static void push(struct opxy_master *m)
{
	uint8_t out[HL_PAYLOAD];

	(void)s06_send(&m->node, m->sv.hl_link, m->sv.hl_route, HL_FRAME_ID, out,
		       hl_event(&m->sv.hl, out));
}

static void job(struct opxy_master *m, uint8_t state, uint8_t step, uint8_t pct,
		uint8_t err, const char *text)
{
	if (hl_job_set(&m->sv.hl, state, step, pct, err, text))
		push(m);
}

/* The record out before a restart that does not return. */
static void restarting(struct opxy_master *m)
{
	job(m, HL_J_DONE, 0, 100u, HL_OK, "restarting");
	for (unsigned i = 0; i < 200u; i++)
		wb_usb_task();
}

void opxy_hostlink_progress(struct opxy_master *m, uint32_t done, uint32_t total,
			    const char *label)
{
	const struct hl_job *j = &m->sv.hl.job;

	if (j->state != HL_J_RUNNING)
		return;
	job(m, HL_J_RUNNING, j->step,
	    total == 0u ? HL_PERCENT_NONE : (uint8_t)wb_percent32(done, total), HL_OK,
	    label != NULL ? label : j->text);
}

/*
 * As the firmware command's begin, chunks and finish (S-10c.071-S-10c.073),
 * the bytes copied from the drive: the upgrade screen, the transfer's file,
 * then the install of the package file with the host's parts.
 */
static void job_install(struct opxy_master *m)
{
	static uint8_t buf[4096];
	struct s09_files *f = &m->sv.files;
	struct hl_stat st;
	struct s11_outcome o;
	char text[S10C_TEXT_MAX];
	const char *name = strrchr(job_path.full, '/') + 1;
	uint32_t at = 0;
	int32_t fd, r = 0;

	if (stat_(m, &job_path, &st) != HL_OK || st.type != 1u) {
		job(m, HL_J_FAILED, 0, HL_PERCENT_NONE, HL_E_NOT_FOUND, "no package");
		return;
	}
	s15_up_transfer_begin(&m->sv.menu, st.size);
	opxy_menu_show(m);
	if (!opxy_transfer_start(m)) {
		s15_up_no_transfer(&m->sv.menu);
		opxy_menu_show(m);
		job(m, HL_J_FAILED, 0, HL_PERCENT_NONE, HL_E_IO, "transfer did not start");
		return;
	}
	s15_up_transfer_started(&m->sv.menu);
	fd = s09_files_open(f, job_path.full, S09_O_RDONLY);
	while (fd >= 0 && at < st.size) {
		r = s09_files_read(f, fd, buf, sizeof(buf));
		if (r <= 0 || !opxy_transfer_append(m, buf, (size_t)r))
			break;
		at += (uint32_t)r;
		job(m, HL_J_RUNNING, 0, (uint8_t)wb_percent32(at, st.size), HL_OK, "copying");
		opxy_menu_show(m);
		opxy_usb_breathe();
	}
	if (fd >= 0)
		(void)s09_files_close(f, fd);
	if (at != st.size) {
		opxy_transfer_discard(m);
		s15_up_no_transfer(&m->sv.menu);
		opxy_menu_show(m);
		job(m, HL_J_FAILED, 0, HL_PERCENT_NONE, HL_E_IO, "copy failed");
		return;
	}
	job(m, HL_J_RUNNING, 1, HL_PERCENT_NONE, HL_OK, "installing");
	opxy_install_host_parts(job_arg);
	opxy_transfer_submit_file(m, name, &o);
	opxy_install_host_parts(-1);
	if (m->sv.host_ops.ramdisk_offer != NULL)
		m->sv.host_ops.ramdisk_offer(m);
	opxy_menu_show(m);
	if (o.kind == S11_OUTCOME_DONE || o.kind == S11_OUTCOME_NOTHING_TO_DO) {
		job(m, HL_J_DONE, o.step, 100u, HL_OK,
		    o.kind == S11_OUTCOME_DONE ? "[done]" : "[nothing to do]");
		return;
	}
	text[s10c_error_text(&o, text) < sizeof(text) ? s10c_error_text(&o, text) :
	     sizeof(text) - 1u] = '\0';
	job(m, HL_J_FAILED, o.step, HL_PERCENT_NONE, HL_E_REFUSED, text);
}

static void run_job(struct opxy_master *m)
{
	struct s15_osw_end e;
	uint8_t mode = job_arg;

	job(m, HL_J_RUNNING, 0, HL_PERCENT_NONE, HL_OK, "");
	switch (m->sv.hl.job.action) {
	case HL_A_INSTALL:
		job_install(m);
		break;
	case HL_A_OS_WRITE:
		opxy_osw_path = job_path.full;
		if (!s15_menu_os_write(&m->sv.menu, &e)) {
			e.result = S15_OSW_FAILED;
			e.why = S15_OSW_WHY_NONE;
		}
		opxy_osw_path = "/yaffs2/os.ldr";
		opxy_menu_show(m);
		if (e.result == S15_OSW_OK)
			job(m, HL_J_DONE, 0, 100u, HL_OK, "written");
		else
			job(m, HL_J_FAILED, (uint8_t)e.why, HL_PERCENT_NONE,
			    HL_E_REFUSED, "not written");
		break;
	case HL_A_OS_ONCE:
		restarting(m);
		s15_menu_os_once(&m->sv.menu);	/* returns when not started */
		opxy_menu_show(m);
		job(m, HL_J_FAILED, 0, HL_PERCENT_NONE, HL_E_REFUSED, "not started");
		break;
	case HL_A_CONTINUE_OS:
		restarting(m);
		(void)s15_menu_continue(&m->sv.menu);
		break;
	case HL_A_RESTART:
		restarting(m);
		opxy_reboot(m, S04_W2, 0u);
		break;
	case HL_A_RESTART_MODE:
		restarting(m);
		opxy_reboot(m, S04_W1, s04_host_mode_reason(&mode, 1u));
		break;
	default:
		job(m, HL_J_FAILED, 0, HL_PERCENT_NONE, HL_E_UNKNOWN, "");
		break;
	}
}

/* ---- the board's side ---- */

void opxy_hostlink_start(struct opxy_master *m)
{
	struct hl_ops *o = &m->sv.hl_ops;

	me = m;
	o->ctx = m;
	o->link_on = link_on;
	o->approve_open = approve_open;
	o->info = info;
	o->root_ok = root_ok;
	o->stat = stat_;
	o->list = list;
	o->read = read_;
	o->hash = hash;
	o->create = create;
	o->write = write_;
	o->finish = finish;
	o->abandon = abandon;
	o->remove = remove_;
	o->mkdir = mkdir_;
	o->rename = rename_;
	o->run = run;
	o->set = set;
	hl_init(&m->sv.hl, o);
	m->sv.hl_link = S06_LINK_CDC;
	m->sv.hl_route = s06_route(S06_ADDR_MASTER, S06_ADDR_HOST);
}

bool opxy_hostlink_approved(const struct opxy_master *m)
{
	return m->sv.menu.host_link && m->sv.hl.session == HL_S_APPROVED;
}

bool opxy_hostlink_frame(struct opxy_master *m, unsigned link, const struct s06_frame *f)
{
	static uint8_t out[HL_PAYLOAD];
	const uint8_t route = s06_swap_route(f->route);
	size_t n;

	if (f->id != HL_FRAME_ID)
		return false;
	if (m->sv.hl.ops == NULL)
		return true;			/* before held mode: not served */
	if (f->len >= 1u && f->payload[0] == HL_RUN) {
		m->sv.hl_link = (uint8_t)link;
		m->sv.hl_route = route;
	}
	n = hl_request(&m->sv.hl, f->payload, f->len, out);
	(void)s06_send(&m->node, link, route, HL_FRAME_ID, out, n);
	return true;
}

static void approval_log(struct opxy_master *m, const char *how)
{
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "host link: ");
	s14_text_str(&t, how);
	s14_log_text(&m->sv.log, S14_INFO, S14_TAG("host"), &t);
}

void opxy_hostlink_poll(struct opxy_master *m)
{
	struct hostlink *h = &m->sv.hl;

	if (h->ops == NULL)
		return;
	if (h->session == HL_S_PENDING) {
		switch (s15_menu_approve_result(&m->sv.menu)) {
		case S15_APPROVE_PENDING:
			break;
		case S15_APPROVE_ALLOWED:
			hl_answer(h, HL_S_APPROVED);
			approval_log(m, "allowed");
			break;
		case S15_APPROVE_REFUSED:
			hl_answer(h, HL_S_REFUSED);
			approval_log(m, "refused");
			break;
		case S15_APPROVE_TIMEOUT:
			hl_answer(h, HL_S_TIMEOUT);
			approval_log(m, "not answered");
			break;
		case S15_APPROVE_NONE:
		default:
			h->session = HL_S_NONE;
			break;
		}
		opxy_menu_show(m);
	}
	hl_poll(h);
	if (h->job.state == HL_J_QUEUED)
		run_job(m);
}

enum s10c_gate opxy_hostlink_gate(void *ctx, enum s10c_link link)
{
	struct opxy_master *m = ctx;
	struct hostlink *h = &m->sv.hl;

	(void)link;
	if (!m->sv.menu.host_link)
		return S10C_GATE_OFF;
	if (opxy_hostlink_approved(m))
		return S10C_GATE_OK;
	/* not approved: the unit asks now, and the host asks again after */
	if (h->ops != NULL && h->session != HL_S_PENDING && approve_open(m))
		h->session = HL_S_PENDING;
	return S10C_GATE_ASK;
}
