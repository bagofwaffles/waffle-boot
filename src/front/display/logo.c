/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The stored boot logo (see display/logo.h). */
#include "front/display/logo.h"

#include "front/display/panel.h"

_Static_assert(FR_LOGO_OFF(0) == FR_SLOT_BASE(0) + FR_LOGO_SLOT_OFF &&
	       FR_LOGO_OFF(1) == FR_SLOT_BASE(1) + FR_LOGO_SLOT_OFF &&
	       FR_LOGO_SLOT_OFF + FR_SECTOR == FR_IMAGE_MAX,
	       "the logo's header sector lies below the settings sector");
_Static_assert(FR_LOGO_W_MAX == FR_PANEL_ROWS && FR_LOGO_H_MAX == FR_PANEL_VISIBLE,
	       "a picture is at most the visible screen");

bool fr_logo_head(const uint8_t h[FR_LOGO_HEAD], const struct fr_record *rec, struct fr_logo *l)
{
	uint32_t end;

	if (fr_le32(h + FR_LOGO_O_MAGIC) != FR_LOGO_MAGIC ||
	    fr_le16(h + FR_LOGO_O_VERSION) != FR_LOGO_VERSION ||
	    fr_le16(h + FR_LOGO_O_SIZE) != FR_LOGO_HEAD ||
	    fr_crc32(0, h, FR_LOGO_O_HCRC) != fr_le32(h + FR_LOGO_O_HCRC))
		return false;
	l->width = fr_le16(h + FR_LOGO_O_WIDTH);
	l->height = fr_le16(h + FR_LOGO_O_HEIGHT);
	l->bits = h[FR_LOGO_O_BITS];
	l->colors = fr_le16(h + FR_LOGO_O_COLORS);
	l->bg = fr_le16(h + FR_LOGO_O_BG);
	l->len = fr_le32(h + FR_LOGO_O_LEN);
	l->crc = fr_le32(h + FR_LOGO_O_CRC);
	if (h[FR_LOGO_O_BITS + 1u] != 0u || fr_le16(h + FR_LOGO_O_BG + 2u) != 0u ||
	    l->width < 1u || l->width > FR_LOGO_W_MAX || l->height < 1u || l->height > FR_LOGO_H_MAX ||
	    (l->bits != 1u && l->bits != 2u && l->bits != 4u && l->bits != 8u) ||
	    l->colors < 2u || l->colors > (1u << l->bits))
		return false;
	/* the logo of this image, in the room it leaves */
	if (rec->tag != FR_TAG || rec->length < 1u || rec->length > FR_LOGO_SLOT_OFF ||
	    fr_le32(h + FR_LOGO_O_IMAGE) != rec->crc32)
		return false;
	end = (rec->length + FR_SECTOR - 1u) & ~(FR_SECTOR - 1u);
	return l->len >= 2u * l->colors && l->len <= FR_LOGO_DATA_MAX &&
	       l->len <= FR_LOGO_SLOT_OFF - end;
}

static void put(uint8_t *frame, uint32_t r, uint32_t c, uint16_t v)
{
	uint8_t *p = frame + 2u * ((size_t)r * FR_PANEL_COLS + c);

	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

bool fr_logo_draw(const struct fr_logo *l, const uint8_t *data, uint8_t *frame)
{
	const uint8_t *p = data + 2u * l->colors;
	const uint32_t n = l->len - 2u * l->colors, mask = (1u << l->bits) - 1u;
	/* line j is screen x x0 + width - 1 - j, window row 479 - x */
	const uint32_t r0 = FR_PANEL_ROWS - (FR_PANEL_ROWS - l->width) / 2u - l->width;
	const uint32_t c0 = (FR_PANEL_VISIBLE - l->height) / 2u;
	uint32_t pos = 0, r, c;

	if (fr_crc32(0, data, l->len) != l->crc)
		return false;
	/* S-13c.014: the window's columns past the visible 222 black */
	for (r = 0; r < FR_PANEL_ROWS; r++) {
		for (c = 0; c < FR_PANEL_COLS; c++)
			put(frame, r, c, c < FR_PANEL_VISIBLE ? l->bg : 0u);
		fr_serve();
	}
	for (uint32_t j = 0; j < l->width; j++) {
		for (uint32_t y = 0; y < l->height;) {
			uint32_t k, cnt, v;

			if (pos >= n)
				return false;
			cnt = (p[pos] & 0x7Fu) + 1u;
			if (cnt > l->height - y)
				return false;
			if ((p[pos++] & 0x80u) != 0u) {
				if (pos >= n || p[pos] >= l->colors)
					return false;
				v = fr_le16(data + 2u * p[pos++]);
				for (k = 0; k < cnt; k++)
					put(frame, r0 + j, c0 + y + k, (uint16_t)v);
			} else {
				const uint32_t nb = (cnt * l->bits + 7u) / 8u;

				if (nb > n - pos)
					return false;
				for (k = 0; k < cnt; k++) {
					const uint32_t b = k * l->bits;

					v = (uint32_t)p[pos + b / 8u] >> (8u - l->bits - b % 8u) & mask;
					if (v >= l->colors)
						return false;
					put(frame, r0 + j, c0 + y + k, fr_le16(data + 2u * v));
				}
				pos += nb;
			}
			y += cnt;
		}
		fr_serve();
	}
	return pos == n;
}

bool fr_logo_load(const struct fr_flash *f, unsigned k, uint8_t *data, uint8_t *frame)
{
	struct fr_record rec;
	struct fr_logo l;
	uint8_t h[FR_LOGO_HEAD];
	uint32_t at;

	if (k > 1u || fr_read_record(f, k, &rec) != FR_FLASH_OK)
		return false;
	fr_serve();
	if (f->read(f->ctx, FR_LOGO_OFF(k), h, FR_LOGO_HEAD) != 0 || !fr_logo_head(h, &rec, &l))
		return false;
	/* forward through one range, a page a read, the link served between */
	at = FR_LOGO_OFF(k) - l.len;
	for (uint32_t off = 0; off < l.len;) {
		const uint32_t m = l.len - off < FR_PAGE ? l.len - off : FR_PAGE;

		fr_serve();
		if (f->read(f->ctx, at + off, data + off, m) != 0)
			return false;
		off += m;
	}
	return fr_logo_draw(&l, data, frame);
}
