/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The MTP sets' store (S-10b) on the master: the slave's /yaffs2 volume
 * through the bridge's file commands (mtp/volume.c), and the device's
 * battery (S-07) and clock (S-04.070). S-10a drives the engine from
 * s10a_usb_poll(), so every request runs from opxy_usb_poll().
 *
 * MTP is offered only by the developer menu (S-15.093), so the store is
 * read-write: level W (S-10b.021).
 */
#include "waffle/board/opxy_services.h"

#include <stdlib.h>
#include <string.h>

#include "waffle/board/opxy_boot.h"
#include "waffle/usb/device.h"
#include "waffle/mtp/volume.h"

/* The session's listing (S-10b.013) and the store, from the heap once per
 * boot, the listing sized to S-10b.014's floor. */
#define MTP_OBJECTS S10B_LIST_OBJECTS
#define MTP_NAMES   S10B_LIST_POOL

static struct s10b_object *objects;
static uint8_t *names;
static struct s10b_store *store;

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
		store = malloc(sizeof(*store));
	}
	if (objects == NULL || names == NULL || store == NULL) {
		s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG, "mtp: no memory");
		s10a_usb_mtp(NULL);
		return;
	}
	s10b_store_ops(store, &sv->files, &sv->mtp_volume);
	sv->mtp_device = device;
	sv->mtp_device.ctx = m;
	memset(&cfg, 0, sizeof(cfg));
	cfg.level = S10B_LEVEL_W;
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
