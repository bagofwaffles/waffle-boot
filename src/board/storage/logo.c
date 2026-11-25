/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The boot logo on the master (waffle/flash/logo.h): the picture the front drew
 * at this start, read again for the hand-over frame, and Display's Boot
 * logo: a blank RAM disk for one PNG file, its conversion, and its save or
 * removal in the running slot under the settings' rule, the factory reset's
 * among them.
 *
 * S-02.109 - S-02.112.
 */
#include "waffle/board/services.h"

#include <stdlib.h>
#include <string.h>

#include "waffle/board/boot.h"
#include "waffle/board/ops.h"
#include "waffle/display/display.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/flash/logo.h"
#include "waffle/flash/logo_png.h"
#include "waffle/usb/msc.h"
#include "waffle/install/content.h"

#define PIXELS (WB_LOGO_W * WB_LOGO_H)

/* the hand-over frame's picture, as the front drew it; the big buffers are
 * written before they are read, so start-up does not clear them */
static WB_NOINIT uint16_t shown[PIXELS];
static struct wb_display_image shown_image;
static bool shown_read, shown_ok;
/* the stored data: the front's logo read again, or a dropped picture's */
static WB_NOINIT uint8_t data[WB_LOGO_DATA_MAX];
#if !defined(OPXY_TEST_OS)
/* a dropped picture: the file's pixels, then, once it is converted, the
 * screen it gives */
static WB_NOINIT uint16_t picture[PIXELS];
static struct wb_logo pending;
static bool pending_ok;
#endif

const struct wb_display_image *opxy_logo_handover(struct opxy_master *m)
{
	struct wb_logo l;

	if (!shown_read) {
		shown_read = true;
		if (m->front && (m->ho.flags & FR_HO_LOGO) != 0u) {
			const unsigned k = (m->ho.flags & FR_HO_LOGO_SLOT1) != 0u ? 1u : 0u;

			shown_ok = wb_logo_read(&m->flash, k, &l, data) && wb_logo_draw(&l, data, shown);
			if (!shown_ok)
				s14_log_str(&m->sv.log, S14_ERROR, S14_TAG("logo"),
					    "the front's logo not read again");
		}
		shown_image.data = NULL;
		shown_image.len = 0;
		shown_image.logo = shown;
	}
	return shown_ok ? &shown_image : NULL;
}

bool opxy_logo_as_front(const struct opxy_master *m, const struct wb_display_image *im)
{
	if ((m->ho.flags & FR_HO_LOGO) != 0u && m->front)
		return im->logo != NULL;
	return im->logo == NULL && im->data == NULL;
}

#if !defined(OPXY_TEST_OS)

/* The settings' rule, as wb_logo_save() applies it, without a read of the
 * record: what Display shows. */
static bool usable(const struct opxy_master *m)
{
	return m->store_slot.established && m->store_slot.good && m->store_slot.slot <= 1u;
}

static void env_logo_state(void *ctx, struct s15_logo *out)
{
	struct opxy_master *m = ctx;
	const unsigned k = m->store_slot.slot;
	uint8_t h[WB_LOGO_HEAD];
	struct wb_logo l;

	memset(out, 0, sizeof *out);
	out->room = wb_logo_room(&m->store_slot.rec);
	if (!usable(m)) {
		out->state = S15_LOGO_UNAVAILABLE;
	} else if (out->room == 0u) {
		out->state = S15_LOGO_NO_ROOM;
	} else if (m->flash.flash->read(m->flash.flash->ctx, s02_slot_base(k) + WB_LOGO_SLOT_OFF,
					h, WB_LOGO_HEAD) == 0 &&
		   wb_logo_parse(h, &m->store_slot.rec, &l)) {
		out->state = S15_LOGO_CUSTOM;
		out->stored = l.len;
	} else {
		out->state = S15_LOGO_BUILTIN;
	}
}

/* A blank disk: whatever it held formatted away while no host holds it
 * (S-12.018). */
static bool blank_disk(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;

	if (opxy_usb_up() && s10a_usb_disk()->offered &&
	    !opxy_disk_take(m, S10A_WITHDRAW_OWN_WRITE)) {
		s14_log_str(&sv->log, S14_ERROR, S14_TAG("logo"), "disk busy: not formatted");
		return false;
	}
	if (!wire_fat_format(&sv->fat, &sv->disk.dev, opxy_dev(m))) {
		s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG, "ramdisk format failed");
		return false;
	}
	wire_fat_unmount(&sv->fat);
	sv->disk_formatted = true;
	return true;
}

/* The disk offered, also after a package was read from it in this boot. */
static void offer(void)
{
	if (opxy_usb_up() && !s10a_usb_disk()->offered)
		s10a_usb_disk_offer();
}

/* A blank disk first, then the disk's set started and the disk offered. */
static void env_logo_disk(void *ctx)
{
	struct opxy_master *m = ctx;
	bool blank;

	pending_ok = false;
	opxy_host_hold_begin(m);
	blank = blank_disk(m);
	opxy_usb_select(m, S10A_STATE_UPGRADE);
	if (blank)
		offer();
	opxy_host_hold_end(m);
}

/* A name to take: a file whose name ends in .png, not hidden (the files a
 * computer adds start with a dot). */
static bool png_name(const char *s)
{
	const size_t n = strlen(s);

	return n > 4u && s[0] != '.' && s[n - 4u] == '.' &&
	       (s[n - 3u] | 0x20) == 'p' && (s[n - 2u] | 0x20) == 'n' && (s[n - 1u] | 0x20) == 'g';
}

static void load(struct opxy_master *m, struct s15_logo_pic *out)
{
	struct opxy_services *sv = &m->sv;
	char name[S11_PATH_MAX], got[S11_PATH_MAX];
	struct wire_source src;
	enum wb_logo_way way;
	enum wb_png_rc rc;
	unsigned w = 0, h = 0, found = 0;
	uint32_t size = 0, need = 0;
	struct wb_logo_work *work;
	bool dir, fits;

	out->room = wb_logo_room(&m->store_slot.rec);
	out->result = S15_LOGO_REFUSED;
	out->why = "disk not read";
	if ((opxy_usb_up() && s10a_usb_disk()->offered && !opxy_disk_take(m, S10A_WITHDRAW_SCAN)) ||
	    !wire_fat_mount(&sv->fat, &sv->disk.dev))
		return;
	for (size_t i = 0; wire_fat_root(&sv->fat, i, name, sizeof name, &dir); i++)
		if (!dir && png_name(name) && found++ == 0u)
			memcpy(got, name, sizeof got);
	if (found != 1u) {
		out->result = found == 0u ? S15_LOGO_NONE : S15_LOGO_MANY;
		wire_fat_unmount(&sv->fat);
		return;
	}
	if (!wire_fat_open_file(&sv->fat, got, &src, &size)) {
		wire_fat_unmount(&sv->fat);
		return;
	}
	rc = wb_png_decode(src.read, src.ctx, size, picture, &w, &h);
	wire_fat_close_file(&sv->fat);
	wire_fat_unmount(&sv->fat);
	if (rc != WB_PNG_OK) {
		out->why = wb_png_why(rc);
		return;
	}
	/* the conversion's working memory from the heap for its length, as
	 * zlib's window is: the image's own region has no room for it */
	work = malloc(sizeof *work);
	if (work == NULL) {
		out->why = "no memory";
		return;
	}
	fits = wb_logo_convert(work, picture, w, h, out->room, data, sizeof data, &pending, &way,
			       &need);
	free(work);
	if (!fits) {
		out->result = S15_LOGO_BIG;
		out->size = need;
		return;
	}
	if (!wb_logo_draw(&pending, data, picture)) {
		out->why = "not drawn";
		return;
	}
	pending_ok = true;
	out->result = S15_LOGO_READY;
	out->size = pending.len;
	out->px = picture;
	s14_log_str(&sv->log, S14_INFO, S14_TAG("logo"),
		    way == WB_LOGO_KEPT ? "picture: its own colors" :
		    way == WB_LOGO_REDUCED ? "picture: reduced to 256 colors" :
					     "picture: reduced to 16 colors, dithered");
}

/* The released disk read; a picture that is not taken leaves a blank disk
 * offered again. */
static void env_logo_load(void *ctx, struct s15_logo_pic *out)
{
	struct opxy_master *m = ctx;

	pending_ok = false;
	s15_menu_hold(&m->sv.menu);
	opxy_menu_show_now(m);
	opxy_host_hold_begin(m);
	load(m, out);
	if (out->result != S15_LOGO_READY && blank_disk(m))
		offer();
	opxy_host_hold_end(m);
}

static enum s15_store_save outcome(struct opxy_master *m, enum wb_logo_rc rc, const char *what)
{
	switch (rc) {
	case WB_LOGO_OK:
		s14_log_str(&m->sv.log, S14_INFO, S14_TAG("logo"), what);
		return S15_STORE_SAVED;
	case WB_LOGO_UNUSABLE:
	case WB_LOGO_ROOM:
		s14_log_str(&m->sv.log, S14_WARNING, S14_TAG("logo"),
			    "not written: this slot can't keep it now");
		return S15_STORE_UNUSABLE;
	case WB_LOGO_IO:
	case WB_LOGO_VERIFY:
	default:
		s14_log_str(&m->sv.log, S14_ERROR, S14_TAG("logo"), "flash write failed");
		return S15_STORE_FAILED;
	}
}

/* The writes go as a settings save's: the driver's writer mode on for
 * them alone (opxy_boot_store_save()), with no protected-writes gate. */
static enum s15_store_save env_logo_save(void *ctx)
{
	struct opxy_master *m = ctx;
	enum wb_logo_rc rc = WB_LOGO_UNUSABLE;

	s15_menu_hold(&m->sv.menu);
	if (pending_ok) {
		opxy_flash_writer(&m->hw, true);
		rc = wb_logo_save(&m->flash, &m->store_slot, &pending, data);
		opxy_flash_writer(&m->hw, false);
	}
	pending_ok = false;
	return outcome(m, rc, "saved");
}

static enum s15_store_save env_logo_remove(void *ctx)
{
	struct opxy_master *m = ctx;
	enum wb_logo_rc rc;

	s15_menu_hold(&m->sv.menu);
	opxy_flash_writer(&m->hw, true);
	rc = wb_logo_remove(&m->flash, &m->store_slot);
	opxy_flash_writer(&m->hw, false);
	return outcome(m, rc, "removed");
}
#endif

void opxy_logo_env(struct opxy_master *m, struct s15_env_ops *env)
{
	(void)m;
	/* the test OS (waffle/testos.h) keeps no settings, nor a logo */
	env->logo_state = OPXY_TESTOS_OR(NULL, env_logo_state);
	env->logo_disk = OPXY_TESTOS_OR(NULL, env_logo_disk);
	env->logo_load = OPXY_TESTOS_OR(NULL, env_logo_load);
	env->logo_save = OPXY_TESTOS_OR(NULL, env_logo_save);
	env->logo_remove = OPXY_TESTOS_OR(NULL, env_logo_remove);
}
