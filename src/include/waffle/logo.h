/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The stored boot logo (the format in front/fr_handoff.h; tools/wblogo.py
 * is the reference): its header, the room a slot leaves it, its drawing,
 * the conversion of a picture to it, and its save and removal in the
 * running slot. A PNG file's decoding is waffle/logo_png.h's.
 *
 * SPEC-Q: the boot logo has no requirement label yet.
 */
#ifndef WAFFLE_LOGO_H
#define WAFFLE_LOGO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "front/fr_handoff.h"
#include "waffle/s02_flash.h"
#include "waffle/s02_store.h"

#define WB_LOGO_W        FR_LOGO_W_MAX
#define WB_LOGO_H        FR_LOGO_H_MAX
#define WB_LOGO_HEAD     FR_LOGO_HEAD
#define WB_LOGO_DATA_MAX FR_LOGO_DATA_MAX
/* The longest image that leaves a slot room for a logo. */
#define WB_LOGO_SLOT_OFF FR_LOGO_SLOT_OFF

/* A header's fields. */
struct wb_logo {
	uint16_t width, height;
	uint8_t bits;
	uint16_t colors;
	uint16_t bg;
	uint32_t len;		/* the data's bytes */
	uint32_t crc;		/* their CRC-32 */
	uint32_t image;		/* the slot record's CRC-32 it was saved with */
};

/* The header as the front checks it (fr_logo_head()) against a slot whose
 * record is rec: false for no logo. */
bool wb_logo_parse(const uint8_t h[WB_LOGO_HEAD], const struct s02_record *rec,
		   struct wb_logo *out);
/* The header's bytes for l, its CRC last. */
void wb_logo_header(const struct wb_logo *l, uint8_t h[WB_LOGO_HEAD]);

/* The bytes a slot with record rec leaves a logo: from the image's end,
 * its length rounded up to a sector, to the header's sector; 0 when the
 * record is not plausible or the image reaches the header's sector. */
uint32_t wb_logo_room(const struct s02_record *rec);

/* The screen as the front draws the logo: px[r * WB_LOGO_H + c] is window
 * row r (screen x 479 - r) and column c (screen y), 480 x 222. The data's
 * CRC is checked first. False at the first fault, px then undefined. */
bool wb_logo_draw(const struct wb_logo *l, const uint8_t *data, uint16_t *px);

/* How a picture was converted (wblogo.py's way). */
enum wb_logo_way {
	WB_LOGO_KEPT,		/* its own colors, 256 or fewer */
	WB_LOGO_REDUCED,	/* reduced to 256 by median cut */
	WB_LOGO_DITHERED	/* reduced to 16, dithered */
};

/*
 * A w x h picture (px row by row, RGB565) as stored data in room bytes at
 * most (and data's cap): its colors kept or reduced to 256, then, if that
 * does not fit, 16 dithered. *out gets every field but image. False when
 * nothing fits, *need then the smallest size tried.
 */
bool wb_logo_convert(const uint16_t *px, unsigned w, unsigned h, uint32_t room,
		     uint8_t *data, uint32_t cap, struct wb_logo *out,
		     enum wb_logo_way *way, uint32_t *need);

/*
 * Whether [off, off + len) may be erased or programmed for slot `slot`'s
 * logo, whose image is rec_len bytes long: wholly from the image's end,
 * rounded up to a sector, to the end of the header's sector. Never the
 * image, the settings sector, the start log, a record or the selector.
 */
bool wb_logo_range_ok(unsigned slot, uint32_t rec_len, uint32_t off, uint32_t len);

/* What a save or a removal did. */
enum wb_logo_rc {
	WB_LOGO_OK,
	WB_LOGO_UNUSABLE,	/* the running slot may not be written: nothing done */
	WB_LOGO_ROOM,		/* the data does not fit the slot's room: nothing done */
	WB_LOGO_IO,		/* an erase, program or read failed */
	WB_LOGO_VERIFY		/* a read-back differed */
};

/*
 * The save in the running slot, under the settings' rule (s02_store.h): the
 * slot established, its image good at this start, its record read again
 * unchanged and its length at most WB_LOGO_SLOT_OFF. Then the header's
 * sector erased first, so a cut leaves no logo; the data's sectors erased,
 * the data programmed and read back with its CRC; the header programmed last
 * and read back. l's image is set to the record's CRC-32. env->scratch holds
 * at least a sector.
 */
enum wb_logo_rc wb_logo_save(const struct s02_env *env, const struct s02_store_slot *run,
			     struct wb_logo *l, const uint8_t *data);
/* The removal: the header's sector erased and read back, under the same
 * rule; an erased header is left alone. */
enum wb_logo_rc wb_logo_remove(const struct s02_env *env, const struct s02_store_slot *run);
/* The slot's logo, read into data (WB_LOGO_DATA_MAX bytes) and checked
 * whole, as the front reads it: false for none. */
bool wb_logo_read(const struct s02_env *env, unsigned slot, struct wb_logo *l, uint8_t *data);

#endif /* WAFFLE_LOGO_H */
