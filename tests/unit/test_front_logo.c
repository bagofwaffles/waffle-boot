/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The stored boot logo as the front checks and draws it (src/front/display/logo.c)
 * against the main image's code (src/core/flash/logo.c) and tools/wblogo.py's screens
 * (tests/vectors/logo/): every header field wrong in turn falls back, in
 * both; hostile data (a run past its line, data cut short, a palette index
 * past the palette, a byte left over, a CRC that does not match) falls
 * back, bounded, in both; random data with a right CRC is drawn by both or
 * by neither, pixel for pixel the same; the front's frame, the main image's
 * screen and its hand-over frame are the same bytes; and a slot's logo is
 * read from the flash and drawn only when it is that slot image's.
 */
#include <string.h>

#include "support/check.h"

#include "front/display/logo.h"
#include "front/display/panel.h"
#include "waffle/display/display.h"
#include "waffle/flash/logo.h"
#include "waffle/flash/logo_png.h"
#include "waffle/util/crc.h"

#include "vectors/logo_vectors.h"

static uint8_t g_frame[FR_PANEL_FRAME_BYTES], g_hand[FR_PANEL_FRAME_BYTES];
static uint16_t g_px[WB_LOGO_W * WB_LOGO_H];
static uint8_t g_flash[0x100000];

static const struct fr_record g_rec = { 0xA1B2C3D4u, 0x2345u, FR_TAG };
static const struct s02_record g_srec = { 0xA1B2C3D4u, 0x2345u, FR_TAG };

/* The front's frame is the main image's screen in bus order, the two
 * columns past the visible ones black. */
static bool frame_is(const uint8_t *frame, const uint16_t *px)
{
	for (unsigned r = 0; r < FR_PANEL_ROWS; r++)
		for (unsigned c = 0; c < FR_PANEL_COLS; c++) {
			const uint8_t *p = frame + 2u * (r * FR_PANEL_COLS + c);
			const uint16_t want = c < FR_PANEL_VISIBLE ? px[r * WB_LOGO_H + c] : 0u;

			if (p[0] != (uint8_t)(want >> 8) || p[1] != (uint8_t)want)
				return false;
		}
	return true;
}

static void fields(struct wb_logo *l, const struct lv_case *c)
{
	memset(l, 0, sizeof *l);
	l->width = (uint16_t)c->w;
	l->height = (uint16_t)c->h;
	l->bits = (uint8_t)c->bits;
	l->colors = (uint16_t)c->colors;
	l->bg = c->bg;
	l->len = c->len;
	l->crc = c->data_crc;
	l->image = g_rec.crc32;
}

/* Both checks and both drawings agree on header h and data d; true when
 * both draw. */
static bool both(const uint8_t *h, const uint8_t *d)
{
	struct fr_logo fl;
	struct wb_logo wl;
	const bool fh = fr_logo_head(h, &g_rec, &fl), wh = wb_logo_parse(h, &g_srec, &wl);
	bool fd, wd;

	CHECK(fh == wh);
	if (!fh || !wh)
		return false;
	fd = fr_logo_draw(&fl, d, g_frame);
	wd = wb_logo_draw(&wl, d, g_px);
	CHECK(fd == wd);
	if (fd && wd)
		CHECK(frame_is(g_frame, g_px));
	return fd && wd;
}

static void test_same_pixels(void)
{
	unsigned drawn = 0;

	for (unsigned i = 0; i < ARRAY_LEN(lv_cases); i++) {
		const struct lv_case *c = &lv_cases[i];
		struct wb_logo l;
		struct wb_display_image im = { NULL, 0, g_px };
		uint8_t h[WB_LOGO_HEAD];
		uint32_t crc = 0;

		if (!c->fits)
			continue;
		fields(&l, c);
		wb_logo_header(&l, h);
		CHECK(both(h, c->data));
		for (unsigned k = 0; k < WB_LOGO_W * WB_LOGO_H; k++) {
			const uint8_t b[2] = { (uint8_t)g_px[k], (uint8_t)(g_px[k] >> 8) };

			crc = wire_crc32(crc, b, 2u);
		}
		CHECK(crc == c->screen_crc);	/* wblogo.py's screen */
		/* the hand-over frame the main image sends: the front's frame */
		wb_display_handover_frame(&im, g_hand);
		CHECK(memcmp(g_hand, g_frame, sizeof g_hand) == 0);
		drawn++;
	}
	CHECK(drawn >= 20u);
}

/* A small logo by hand: 4 x 3, 4 colors at 2 bits, its data in d. */
static uint32_t small(uint8_t *d, struct wb_logo *l)
{
	static const uint8_t lines[] = {
		0x82, 0x01,		/* line 0: a run of 3, index 1 */
		0x02, 0xE4,		/* line 1: 3 literal, 3 2 1 */
		0x00, 0x40, 0x81, 0x02,	/* line 2: 1 literal (1), a run of 2 (2) */
		0x80, 0x03, 0x01, 0x00,	/* line 3: a run of 1 (3), 2 literal 0 0 */
	};
	static const uint16_t pal[4] = { 0x0000u, 0xF800u, 0x07E0u, 0x001Fu };
	uint32_t n = 0;

	for (unsigned i = 0; i < 4u; i++) {
		d[n++] = (uint8_t)pal[i];
		d[n++] = (uint8_t)(pal[i] >> 8);
	}
	memcpy(d + n, lines, sizeof lines);
	n += sizeof lines;
	memset(l, 0, sizeof *l);
	l->width = 4;
	l->height = 3;
	l->bits = 2;
	l->colors = 4;
	l->bg = 0x1234u;
	l->len = n;
	l->crc = wire_crc32(0u, d, n);
	l->image = g_rec.crc32;
	return n;
}

static void test_header_fields(void)
{
	struct wb_logo l, m;
	uint8_t h[WB_LOGO_HEAD], d[64];

	(void)small(d, &l);
	wb_logo_header(&l, h);
	CHECK(both(h, d));
	/* the picture's pixels where the front puts them */
	{
		const unsigned r0 = 480u - (480u - 4u) / 2u - 4u, c0 = (222u - 3u) / 2u;

		CHECK(g_px[0] == 0x1234u && g_px[(r0 + 0u) * 222u + c0] == 0xF800u &&
		      g_px[(r0 + 1u) * 222u + c0 + 2u] == 0xF800u &&
		      g_px[(r0 + 2u) * 222u + c0 + 2u] == 0x07E0u &&
		      g_px[(r0 + 3u) * 222u + c0] == 0x001Fu &&
		      g_px[(r0 + 3u) * 222u + c0 + 1u] == 0x0000u);
	}
	/* every byte of the header changed, its CRC left: falls back */
	for (unsigned i = 0; i < WB_LOGO_HEAD; i++) {
		uint8_t b[WB_LOGO_HEAD];

		memcpy(b, h, sizeof b);
		b[i] ^= 0x10u;
		CHECK(!both(b, d));
	}
	/* each field wrong in turn, the header's CRC made right */
	for (unsigned k = 0; k < 24u; k++) {
		uint8_t b[WB_LOGO_HEAD];

		m = l;
		wb_logo_header(&m, b);
		switch (k) {
		case 0: b[FR_LOGO_O_MAGIC] ^= 1u; break;
		case 1: b[FR_LOGO_O_VERSION] = 2u; break;
		case 2: b[FR_LOGO_O_SIZE] = 0x20u; break;
		case 3: fr_put16(b + FR_LOGO_O_WIDTH, 0); break;
		case 4: fr_put16(b + FR_LOGO_O_WIDTH, 481); break;
		case 5: fr_put16(b + FR_LOGO_O_HEIGHT, 0); break;
		case 6: fr_put16(b + FR_LOGO_O_HEIGHT, 223); break;
		case 7: b[FR_LOGO_O_BITS] = 3u; break;
		case 8: b[FR_LOGO_O_BITS] = 0u; break;
		case 9: b[FR_LOGO_O_BITS + 1u] = 1u; break;
		case 10: fr_put16(b + FR_LOGO_O_COLORS, 1); break;
		case 11: fr_put16(b + FR_LOGO_O_COLORS, 5); break;
		case 12: fr_put16(b + FR_LOGO_O_BG + 2u, 1); break;
		case 13: fr_put32(b + FR_LOGO_O_LEN, 7u); break;	/* under the palette */
		case 14: fr_put32(b + FR_LOGO_O_LEN, 0x74001u); break;	/* past the room */
		case 15: fr_put32(b + FR_LOGO_O_LEN, FR_LOGO_DATA_MAX + 1u); break;
		case 16: fr_put32(b + FR_LOGO_O_IMAGE, g_rec.crc32 ^ 1u); break;
		case 17: fr_put32(b + FR_LOGO_O_IMAGE, ~g_rec.crc32); break;	/* a trial mark */
		case 18: fr_put32(b + FR_LOGO_O_CRC, l.crc ^ 1u); break;
		case 19: fr_put32(b + FR_LOGO_O_LEN, l.len - 1u); break;
		case 20: fr_put32(b + FR_LOGO_O_LEN, l.len + 1u); break;
		case 21: fr_put16(b + FR_LOGO_O_WIDTH, 5); break;
		case 22: fr_put16(b + FR_LOGO_O_HEIGHT, 2); break;
		default: b[FR_LOGO_O_BITS] = 4u; break;	/* the lines read at 4 bits */
		}
		fr_put32(b + FR_LOGO_O_HCRC, wire_crc32(0u, b, FR_LOGO_O_HCRC));
		CHECK(!both(b, d));
	}
	/* the record: its tag, its length, the room it leaves */
	{
		static const struct { uint32_t len, tag; bool ok; } r[] = {
			{ 0x2345u, FR_TAG, true }, { 0x2345u, 0x0000B005u, false }, { 0u, FR_TAG, false },
			{ 0x74000u, FR_TAG, true }, { 0x74001u, FR_TAG, false }, { 0x75000u, FR_TAG, false },
			{ 0x75001u, FR_TAG, false }, { 0x76000u, FR_TAG, false },
		};
		struct fr_logo fl;
		struct wb_logo wl;

		for (unsigned i = 0; i < ARRAY_LEN(r); i++) {
			const struct fr_record fr = { g_rec.crc32, r[i].len, r[i].tag };
			const struct s02_record sr = { g_rec.crc32, r[i].len, r[i].tag };

			CHECK(fr_logo_head(h, &fr, &fl) == r[i].ok);
			CHECK(wb_logo_parse(h, &sr, &wl) == r[i].ok);
		}
	}
	/* erased: no logo */
	memset(h, 0xFF, sizeof h);
	CHECK(!both(h, d));
}

static void test_hostile(void)
{
	struct wb_logo l, m;
	uint8_t h[WB_LOGO_HEAD], d[64], e[64];
	const uint32_t n = small(d, &l);
	uint32_t seed = 12345u;
	unsigned drawn = 0;

	/* one byte at a time changed, the CRC made right: both or neither */
	for (unsigned i = 8; i < n; i++)
		for (unsigned v = 0; v < 256u; v++) {
			memcpy(e, d, n);
			e[i] = (uint8_t)v;
			m = l;
			m.crc = wire_crc32(0u, e, n);
			wb_logo_header(&m, h);
			drawn += both(h, e) ? 1u : 0u;
		}
	CHECK(drawn > 0u);
	/* named faults */
	{
		static const struct { unsigned at; uint8_t v; } f[] = {
			{ 8, 0x83 },	/* a run of 4 in a line of 3 */
			{ 10, 0x03 },	/* 4 literal in a line of 3 */
			{ 9, 0x04 },	/* a palette index past 4 colors, in a run */
			{ 18, 0xC0 },	/* a run of 65 */
		};

		for (unsigned i = 0; i < ARRAY_LEN(f); i++) {
			memcpy(e, d, n);
			e[f[i].at] = f[i].v;
			m = l;
			m.crc = wire_crc32(0u, e, n);
			wb_logo_header(&m, h);
			CHECK(!both(h, e));
		}
		/* data cut short, a byte left over */
		m = l;
		m.len = n - 1u;
		m.crc = wire_crc32(0u, d, n - 1u);
		wb_logo_header(&m, h);
		CHECK(!both(h, d));
		memcpy(e, d, n);
		e[n] = 0x80u;
		m = l;
		m.len = n + 1u;
		m.crc = wire_crc32(0u, e, n + 1u);
		wb_logo_header(&m, h);
		CHECK(!both(h, e));
		/* a literal index past the colors: 3 colors, index 3 */
		m = l;
		m.colors = 3;
		memcpy(e, d, n);
		e[0] = 0; e[1] = 0;
		memmove(e + 6, d + 8, n - 8u);
		m.len = n - 2u;
		m.crc = wire_crc32(0u, e, m.len);
		wb_logo_header(&m, h);
		CHECK(!both(h, e));
	}
	/* random lines of random pictures: both or neither, never past the data */
	for (unsigned t = 0; t < 20000u; t++) {
		static uint8_t r[600];
		uint32_t len;

		seed = seed * 1103515245u + 12345u;
		memset(&m, 0, sizeof m);
		m.width = (uint16_t)(1u + (seed >> 8) % 6u);
		m.height = (uint16_t)(1u + (seed >> 16) % 9u);
		m.bits = (uint8_t)(1u << ((seed >> 4) % 4u));
		m.colors = (uint16_t)(2u + (seed >> 20) % ((1u << m.bits) - 1u));
		len = 2u * m.colors + (seed >> 12) % 64u;
		for (uint32_t k = 0; k < len; k++) {
			seed = seed * 1103515245u + 12345u;
			r[k] = (uint8_t)(seed >> 16);
			/* lean the runs short enough to fit, sometimes */
			if (k >= 2u * m.colors && (seed & 3u) == 0u)
				r[k] &= 0x81u;
		}
		m.len = len;
		m.crc = wire_crc32(0u, r, len);
		m.image = g_rec.crc32;
		wb_logo_header(&m, h);
		drawn += both(h, r) ? 1u : 0u;
	}
	CHECK(drawn > 100u);
}

/* ---- the slot's logo on the flash ---- */

static int f_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	const uint32_t *fail_at = ctx;

	if (off > sizeof g_flash || len > sizeof g_flash - off)
		return -1;
	if (fail_at != NULL && *fail_at >= off && *fail_at < off + len)
		return -1;
	memcpy(buf, g_flash + off, len);
	return 0;
}

static int f_none(void *ctx, uint32_t off)
{
	(void)ctx;
	(void)off;
	CHECK(0);	/* nothing is erased */
	return -1;
}

static int f_noprog(void *ctx, uint32_t off, const uint8_t *b, uint32_t n)
{
	(void)ctx;
	(void)off;
	(void)b;
	(void)n;
	CHECK(0);	/* nor programmed */
	return -1;
}

static void put_logo(unsigned k, uint32_t len, const struct lv_case *c)
{
	struct wb_logo l;

	memset(g_flash, 0xFF, sizeof g_flash);
	fr_put32(g_flash + FR_RECORD_OFF(k), g_rec.crc32);
	fr_put32(g_flash + FR_RECORD_OFF(k) + 4u, len);
	fr_put32(g_flash + FR_RECORD_OFF(k) + 8u, FR_TAG);
	fields(&l, c);
	wb_logo_header(&l, g_flash + FR_LOGO_OFF(k));
	memcpy(g_flash + FR_LOGO_OFF(k) - c->len, c->data, c->len);
}

static void test_load(void)
{
	const struct lv_case *c = NULL;
	uint32_t fail_at;
	struct fr_flash f = { f_read, f_none, f_noprog, NULL };
	struct wb_logo wl;

	for (unsigned i = 0; i < ARRAY_LEN(lv_cases); i++)
		if (strcmp(lv_cases[i].name, "photo_16") == 0)
			c = &lv_cases[i];
	CHECK(c != NULL);
	if (c == NULL)
		return;
	for (unsigned k = 0; k < 2u; k++) {
		static uint8_t staging[FR_LOGO_DATA_MAX];

		put_logo(k, 0x2345u, c);
		CHECK(fr_logo_load(&f, k, staging, g_frame));
		fields(&wl, c);
		CHECK(wb_logo_draw(&wl, c->data, g_px) && frame_is(g_frame, g_px));
		CHECK(!fr_logo_load(&f, 1u - k, staging, g_frame));	/* the other slot: none */
		CHECK(!fr_logo_load(&f, 2u, staging, g_frame));
		/* a read that fails: at the header, in the data, at the record */
		{
			const uint32_t at[] = { FR_LOGO_OFF(k) + 3u, FR_LOGO_OFF(k) - c->len / 2u,
						FR_RECORD_OFF(k) };

			for (unsigned i = 0; i < ARRAY_LEN(at); i++) {
				fail_at = at[i];
				f.ctx = &fail_at;
				CHECK(!fr_logo_load(&f, k, staging, g_frame));
			}
		}
		f.ctx = NULL;
		/* a data byte changed in the flash */
		g_flash[FR_LOGO_OFF(k) - 1u] ^= 0x40u;
		CHECK(!fr_logo_load(&f, k, staging, g_frame));
		/* an image that reaches into the data, and one that ends at it */
		put_logo(k, 0x75000u - c->len + 1u, c);
		CHECK(!fr_logo_load(&f, k, staging, g_frame));
		put_logo(k, (0x75000u - c->len) / 0x1000u * 0x1000u, c);
		CHECK(fr_logo_load(&f, k, staging, g_frame));
		/* another image in the slot: its record's CRC */
		put_logo(k, 0x2345u, c);
		fr_put32(g_flash + FR_RECORD_OFF(k), g_rec.crc32 + 1u);
		CHECK(!fr_logo_load(&f, k, staging, g_frame));
		/* the main image reads the same */
		put_logo(k, 0x2345u, c);
	}
}

int main(void)
{
	RUN(test_same_pixels);
	RUN(test_header_fields);
	RUN(test_hostile);
	RUN(test_load);
	return check_summary("test_front_logo");
}
