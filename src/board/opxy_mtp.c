/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The MTP sets' store (S-10b) on the master: the slave's /yaffs2 volume
 * read through the bridge's file commands (S-09.041), and the device's
 * battery (S-07) and clock (S-04.070). S-10a drives the engine from
 * s10a_usb_poll(), so every request below runs from opxy_usb_poll().
 */
#include "waffle/board/opxy_services.h"

#include <stdlib.h>
#include <string.h>

#include "waffle/board/opxy_boot.h"
#include "waffle/s09_bridge.h"
#include "waffle/s10a_usb.h"

/* The session's listing (S-10b.013), from the heap once per boot, sized
 * to S-10b.014's floor. */
#define MTP_OBJECTS S10B_LIST_OBJECTS
#define MTP_NAMES   S10B_LIST_NAMES

#define VOLUME "/yaffs2"
#define MODE_FMT 0xF000u	/* S-09.043 */
#define MODE_DIR 0x4000u
#define MODE_REG 0x8000u

static struct s10b_object *objects;
static uint8_t *names;

/* A request of S-09.041 whose payload is the path in vpath. */
static char vpath[sizeof(VOLUME) + S10B_PATH_MAX];
static uint8_t rec[S09_DIRENT_LEN];
static uint32_t err;		/* the last request's errno (S-09.042) */

/* The result as S-09.041 gives it; err says whether it failed. 0x2C's
 * whence is the length field: 0, from the start, as no payload sends. */
static int32_t call(struct opxy_master *m, uint8_t cmd, uint32_t arg0,
		    uint32_t arg1, const uint8_t *payload, uint32_t plen,
		    uint8_t *data, uint32_t max, uint32_t *got)
{
	struct s09_req r = { cmd, 0, (uint16_t)plen, arg0, arg1 };
	uint32_t g;

	return s09_master_call(&m->bridge, &r, payload, plen, data, max,
			       got != NULL ? got : &g, &err);
}

static int32_t path_call(struct opxy_master *m, uint8_t cmd, uint32_t arg0,
			 uint8_t *data, uint32_t max, uint32_t *got)
{
	return call(m, cmd, arg0, 0, (const uint8_t *)vpath,
		    (uint32_t)strlen(vpath) + 1u, data, max, got);
}

/* The engine's "/" or "/a/b" as the volume's path; false when too long. */
static bool to_volume(const char *path)
{
	const size_t n = strlen(path);

	if (n + sizeof(VOLUME) > sizeof(vpath))
		return false;
	memcpy(vpath, VOLUME, sizeof(VOLUME) - 1u);
	memcpy(vpath + sizeof(VOLUME) - 1u, path, n + 1u);
	if (n == 1u)
		vpath[sizeof(VOLUME) - 1u] = '\0';	/* the root */
	return true;
}

/* 0x31, 0x32: an unsigned 64-bit little-endian count (S-09.041). */
static bool space_of(struct opxy_master *m, uint8_t cmd, uint64_t *v)
{
	uint8_t b[8];
	uint32_t got;
	unsigned i;

	/* S-09.041: the errno of these two is the volume's last error, from any
	 * earlier call, so it does not say whether this one worked. A slave that
	 * has gone answers no bytes. */
	if (path_call(m, cmd, 0, b, sizeof(b), &got) != 0 || got != 8u)
		return false;
	*v = 0;
	for (i = 8u; i > 0u; i--)
		*v = *v << 8 | b[i - 1u];
	return true;
}

static bool vol_space(void *ctx, uint64_t *total, uint64_t *free_bytes)
{
	struct opxy_master *m = ctx;

	(void)to_volume("/");
	return space_of(m, S09_TOTAL_SPACE, total) &&
	       space_of(m, S09_FREE_SPACE, free_bytes);
}

/* One pass over the directory (0x2E, 0x2F, 0x30), each entry's kind,
 * size and times from its stat record (0x22). */
static bool vol_list(void *ctx, const char *path, s10b_entry_fn fn, void *arg)
{
	struct opxy_master *m = ctx;
	struct s09_dirent d;
	struct s09_stat st;
	struct s10b_entry e;
	uint32_t got;
	size_t base;
	int32_t h;
	bool ok = true;

	if (!to_volume(path))
		return false;
	/* A handle is opaque: 0 is the failure (S-09.041). */
	h = path_call(m, S09_OPENDIR, 0, NULL, 0, NULL);
	if (h == 0 || err != 0u)
		return false;
	base = strlen(vpath);
	for (;;) {
		int32_t r = call(m, S09_READDIR, (uint32_t)h, 0, NULL, 0, rec,
				 S09_DIRENT_LEN, &got);

		if (err != 0u || (r != 0 && got != S09_DIRENT_LEN)) {
			ok = false;
			break;
		}
		if (r == 0)
			break;			/* the end */
		s09_dirent_unpack(rec, &d);
		d.name[S09_DIRENT_NAME - 1u] = '\0';
		e.name_len = strlen(d.name);
		if ((e.name_len == 1u && d.name[0] == '.') ||
		    (e.name_len == 2u && d.name[0] == '.' && d.name[1] == '.'))
			continue;
		if (base + 1u + e.name_len >= sizeof(vpath)) {
			ok = false;
			break;
		}
		vpath[base] = '/';
		memcpy(vpath + base + 1u, d.name, e.name_len + 1u);
		e.name = (const uint8_t *)d.name;
		e.kind = S10B_KIND_OTHER;
		e.size = e.mtime = e.ctime = 0;
		if (path_call(m, S09_STAT, 0, rec, S09_STAT_LEN, &got) == 0 &&
		    err == 0u && got == S09_STAT_LEN) {
			s09_stat_unpack(rec, &st);
			if ((st.mode & MODE_FMT) == MODE_DIR)
				e.kind = S10B_KIND_DIR;
			else if ((st.mode & MODE_FMT) == MODE_REG)
				e.kind = S10B_KIND_FILE;
			e.size = st.size;
			e.mtime = st.mtime;
			e.ctime = st.ctime;
		}
		vpath[base] = '\0';
		fn(arg, &e);
	}
	(void)call(m, S09_CLOSEDIR, (uint32_t)h, 0, NULL, 0, NULL, 0, NULL);
	return ok;
}

/* Open, seek, read, close: no handle outlives the request. */
static long vol_read(void *ctx, const char *path, uint32_t offset,
		     uint8_t *buf, size_t n)
{
	struct opxy_master *m = ctx;
	int32_t fd, r = -1;

	if (!to_volume(path))
		return -1;
	fd = path_call(m, S09_OPEN, S09_O_RDONLY, NULL, 0, NULL);
	if (fd < 0 || err != 0u)
		return -1;
	if (call(m, S09_LSEEK, (uint32_t)fd, offset, NULL, 0, NULL, 0, NULL) ==
	    (int32_t)offset && err == 0u) {
		r = 0;
		if (n > 0u)
			r = s09_master_read(&m->bridge, (uint32_t)fd, buf,
					    (uint32_t)n, &err);
		if (err != 0u)
			r = -1;
	}
	(void)call(m, S09_CLOSE, (uint32_t)fd, 0, NULL, 0, NULL, 0, NULL);
	return r;
}

/* --- the device --- */

/* cppcheck-suppress constParameterCallback ; s10b_device_ops fixes it */
static unsigned dev_battery(void *ctx)
{
	const struct opxy_master *m = ctx;

	return m->psoc.batt.charge;
}

/* An unreadable RTC reads 0, 1970-01-01. */
static uint32_t dev_clock_read(void *ctx)
{
	uint32_t clk = 0;

	(void)ctx;
	(void)opxy_rtc_read(&clk);
	return clk;
}

/* S-10b.063: the slave is told the new time at once, from the register
 * read back. */
static void dev_clock_write(void *ctx, uint32_t count)
{
	opxy_rtc_write(count);
	opxy_clock_to_slave(ctx);
}

static const struct s10b_volume_ops volume = { NULL, vol_space, vol_list,
					       vol_read };
static const struct s10b_device_ops device = { NULL, dev_battery,
					       dev_clock_read,
					       dev_clock_write };

void opxy_mtp_start(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;
	struct s10b_config cfg;
	struct s11_version v;

	if (objects == NULL) {
		objects = malloc(MTP_OBJECTS * sizeof(*objects));
		names = malloc(MTP_NAMES);
	}
	if (objects == NULL || names == NULL) {
		s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG, "mtp: no memory");
		s10a_usb_mtp(NULL);
		return;
	}
	sv->mtp_volume = volume;
	sv->mtp_volume.ctx = m;
	sv->mtp_device = device;
	sv->mtp_device.ctx = m;
	memset(&cfg, 0, sizeof(cfg));
	cfg.vol = &sv->mtp_volume;
	cfg.dev = &sv->mtp_device;
	/* S-10b.020: S-10a's strings replace these (s10b_identity()). */
	cfg.manufacturer = "";
	cfg.model = "";
	cfg.serial = "";
	/* S-17.032: the bootloader's own version, as the screen shows it */
	v.major = OPXY_BL_VERSION_MAJOR;
	v.minor = OPXY_BL_VERSION_MINOR;
	v.patch = OPXY_BL_VERSION_PATCH;
	v.build = OPXY_BL_VERSION_BUILD;
	sv->bl_text[s11_version_text(&v, sv->bl_text, S11_VERSION_TEXT_MAX)] =
		'\0';
	cfg.version = sv->bl_text;
	cfg.objects = objects;
	cfg.max_objects = MTP_OBJECTS;
	cfg.names = names;
	cfg.names_size = MTP_NAMES;
	s10b_init(&sv->mtp, &cfg);
	s10a_usb_mtp(&sv->mtp);
}
