/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The stored boot logo in the main image (src/logo.c, src/logo_png.c)
 * against tools/wblogo.py, the reference (tests/vectors/logo/): every PNG
 * case decoded to the same pixels or refused for the same reason, converted
 * to the same bytes by the same way, and drawn to the same screen; the room
 * a slot leaves at its edges; the one range check every erase and program
 * goes through, against every area it must never reach; and the save and
 * the removal on the flash fake: their footprint, their order (the header's
 * sector erased first, the header programmed last), a fault at every
 * operation leaving no logo, and the settings' rule.
 */
#include <string.h>

#include "support/check.h"
#include "s02_fake_flash.h"
#include "waffle/logo.h"
#include "waffle/logo_png.h"

#include "vectors/logo_vectors.h"

/* the conversion's working memory */
static struct wb_logo_work g_work;
static struct fake_flash g_f;
static struct s02_flash_ops g_ops;
static struct s02_env g_env;
static uint8_t g_before[S02_FLASH_SIZE];
static struct s02_store_slot g_run;
static uint16_t g_px[WB_LOGO_W * WB_LOGO_H];
static uint16_t g_screen[WB_LOGO_W * WB_LOGO_H];
static uint8_t g_data[WB_LOGO_DATA_MAX];

/* ---- the reference's cases ---- */

struct mem {
	const uint8_t *p;
	uint32_t n;
	uint32_t fail_at;	/* a read reaching this offset fails; 0: none */
};

static bool mem_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	const struct mem *m = ctx;

	if (off > m->n || n > m->n - off || (m->fail_at != 0u && off + n > m->fail_at))
		return false;
	memcpy(buf, m->p + off, n);
	return true;
}

static uint32_t crc16s(const uint16_t *px, uint32_t n)
{
	uint32_t c = 0;

	for (uint32_t i = 0; i < n; i++) {
		const uint8_t b[2] = { (uint8_t)px[i], (uint8_t)(px[i] >> 8) };

		c = ref_crc32(c, b, 2u);
	}
	return c;
}

static void test_cases(void)
{
	for (unsigned i = 0; i < ARRAY_LEN(lv_cases); i++) {
		const struct lv_case *c = &lv_cases[i];
		struct mem m = { c->png, c->png_len, 0u };
		struct wb_logo l;
		enum wb_logo_way way;
		uint32_t need;
		unsigned w = 0, h = 0;
		enum wb_png_rc rc = wb_png_decode(mem_read, &m, c->png_len, g_px, &w, &h);
		bool fits;

		if ((int)rc != c->rc)
			printf("  %s: decode gave %d, the reference %d\n", c->name, (int)rc, c->rc);
		CHECK((int)rc == c->rc);
		CHECK(wb_png_why(rc) != NULL && (rc == WB_PNG_OK) == (wb_png_why(rc)[0] == '\0'));
		if (rc != WB_PNG_OK || c->rc != WB_PNG_OK)
			continue;
		CHECK(w == c->w && h == c->h && crc16s(g_px, w * h) == c->px_crc);
		/* a read that fails anywhere refuses it */
		m.fail_at = c->png_len / 2u;
		CHECK(wb_png_decode(mem_read, &m, c->png_len, g_px, &w, &h) == WB_PNG_READ);
		m.fail_at = 0;
		(void)wb_png_decode(mem_read, &m, c->png_len, g_px, &w, &h);
		fits = wb_logo_convert(&g_work, g_px, w, h, c->room, g_data, sizeof g_data, &l, &way, &need);
		if (fits != (c->fits != 0))
			printf("  %s: fits %d, the reference %d (need %u)\n", c->name, fits, c->fits,
			       (unsigned)need);
		CHECK(fits == (c->fits != 0));
		if (!fits) {
			CHECK(need > c->room);
			continue;
		}
		CHECK((int)way == c->way && l.bits == c->bits && l.colors == c->colors &&
		      l.bg == c->bg && l.width == w && l.height == h);
		CHECK(l.len == c->len && l.crc == c->data_crc && need == c->len);
		CHECK(l.len == c->len && memcmp(g_data, c->data, c->len) == 0);
		if (memcmp(g_data, c->data, c->len) != 0)
			printf("  %s: data differs from the reference\n", c->name);
		CHECK(wb_logo_draw(&l, g_data, g_screen));
		CHECK(crc16s(g_screen, WB_LOGO_W * WB_LOGO_H) == c->screen_crc);
		/* a room of a byte less takes the next way, or none */
		if (wb_logo_convert(&g_work, g_px, w, h, c->len - 1u, g_data, sizeof g_data, &l, &way, &need))
			CHECK(l.len < c->len && way == WB_LOGO_DITHERED);
		/* the data's cap counts as the room */
		CHECK(!wb_logo_convert(&g_work, g_px, w, h, c->room, g_data, c->len - 1u, &l, &way, &need) ||
		      l.len < c->len);
	}
	/* sizes out of range are not converted */
	{
		struct wb_logo l;
		enum wb_logo_way way;
		uint32_t need;

		CHECK(!wb_logo_convert(&g_work, g_px, 0, 1, 0x40000u, g_data, sizeof g_data, &l, &way, &need));
		CHECK(!wb_logo_convert(&g_work, g_px, 481, 1, 0x40000u, g_data, sizeof g_data, &l, &way, &need));
		CHECK(!wb_logo_convert(&g_work, g_px, 1, 223, 0x40000u, g_data, sizeof g_data, &l, &way, &need));
	}
	/* an empty file, and one shorter than a signature */
	{
		struct mem m = { lv_cases[0].png, 0u, 0u };
		unsigned w, h;

		CHECK(wb_png_decode(mem_read, &m, 0, g_px, &w, &h) == WB_PNG_NOT_PNG);
		m.n = 7u;
		CHECK(wb_png_decode(mem_read, &m, 7u, g_px, &w, &h) == WB_PNG_NOT_PNG);
	}
}

/* ---- the room and the range ---- */

static struct s02_record rec(uint32_t len)
{
	struct s02_record r = { 0x12345678u, len, S02_RECORD_TAG };

	return r;
}

static void test_room(void)
{
	static const struct { uint32_t len, room; } t[] = {
		{ 0u, 0u }, { 1u, 0x74000u }, { 0x1000u, 0x74000u }, { 0x1001u, 0x73000u },
		{ 0x6BD54u, 0x9000u }, { 0x6C000u, 0x9000u }, { 0x74000u, 0x1000u },
		{ 0x74001u, 0u }, { 0x75000u, 0u }, { 0x75001u, 0u }, { 0x76000u, 0u }, { 0x78000u, 0u },
		{ 0x80000000u, 0u },
	};
	struct s02_record r;

	for (unsigned i = 0; i < ARRAY_LEN(t); i++) {
		r = rec(t[i].len);
		CHECK(wb_logo_room(&r) == t[i].room);
	}
	r = rec(0x1000u);
	r.tag = 0x0000B005u;
	CHECK(wb_logo_room(&r) == 0u);
}

/* The areas a logo write must never reach, for slot k whose image is len. */
static bool forbidden(unsigned k, uint32_t len, uint32_t off, uint32_t n)
{
	const uint32_t base = s02_slot_base(k);
	const uint64_t end = (uint64_t)off + n;
	const uint32_t img_end = (len + S02_SECTOR_SIZE - 1u) / S02_SECTOR_SIZE * S02_SECTOR_SIZE;

	if (off < base || end > (uint64_t)base + S02_SLOT_CAPACITY)
		return true;		/* the other slot, records, selector, first stage */
	if (off < base + img_end)
		return true;		/* the image's sectors */
	return end > base + S02_STORE_SLOT_MAX;	/* the settings sector, the start log */
}

static void test_range(void)
{
	static const uint32_t lens[] = { 0u, 1u, 0x1000u, 0x1001u, 0x6BD54u, 0x74000u, 0x74001u,
					 0x75000u, 0x75001u, 0x76000u, 0x78000u };
	static const uint32_t sizes[] = { 1u, 36u, 0x100u, 0x1000u, 0x1001u, 0x9000u };
	unsigned allowed = 0;

	for (unsigned k = 0; k < 3u; k++)
		for (unsigned i = 0; i < ARRAY_LEN(lens); i++)
			for (uint32_t off = 0; off < S02_FLASH_SIZE; off += 0x80u)
				for (unsigned j = 0; j < ARRAY_LEN(sizes); j++) {
					const bool ok = wb_logo_range_ok(k, lens[i], off, sizes[j]);
					const bool plausible = lens[i] >= 1u && lens[i] <= 0x75000u;

					if (ok) {
						allowed++;
						CHECK(k <= 1u && plausible &&
						      !forbidden(k, lens[i], off, sizes[j]));
					} else if (k <= 1u && plausible) {
						CHECK(forbidden(k, lens[i], off, sizes[j]));
					}
				}
	CHECK(allowed > 0u);
	CHECK(!wb_logo_range_ok(0, 0x2000u, 0x12000u, 0u));
	CHECK(!wb_logo_range_ok(0, 0x2000u, 0xFFFFFFF0u, 0x20u));
	CHECK(wb_logo_range_ok(0, 0x2000u, 0x85000u, 0x1000u));
	CHECK(!wb_logo_range_ok(0, 0x2000u, 0x85000u, 0x1001u));
	CHECK(wb_logo_range_ok(0, 0x75000u, 0x85000u, 0x1000u));	/* the image ends at it */
	CHECK(!wb_logo_range_ok(0, 0x75001u, 0x85000u, 1u));	/* the image's own bytes there */
}

/* ---- the save and the removal ---- */

static uint32_t slot_image(unsigned s, uint32_t len, bool trial)
{
	const uint32_t base = s02_slot_base(s), r = s02_record_off(s);
	const uint32_t n = len < S02_SLOT_CAPACITY ? len : S02_SLOT_CAPACITY;
	uint32_t crc;

	memset(g_f.mem + base, 0xFF, S02_SLOT_CAPACITY);
	memset(g_f.mem + r, 0xFF, S02_SECTOR_SIZE);
	ff_fill_image(g_f.mem + base, n, 7u + s);
	crc = ref_crc32(0, g_f.mem + base, n);
	ff_put32(g_f.mem + r, trial ? ~crc : crc);
	ff_put32(g_f.mem + r + 4u, len);
	ff_put32(g_f.mem + r + 8u, S02_RECORD_TAG);
	return crc;
}

static void running(unsigned s, bool good)
{
	g_run.established = true;
	g_run.slot = s;
	g_run.good = good;
	(void)s02_read_record(&g_env, s, &g_run.rec);
}

/* A unit running slot s, an image of len there, the other slot's too, and
 * old bytes in the room and in the settings sector to see what changes. */
static void setup(unsigned s, uint32_t len)
{
	ff_init(&g_f);
	g_env = ff_env(&g_f, &g_ops);
	ff_set_selector(&g_f, (uint8_t)s, 0x01u);
	(void)slot_image(s, len, false);
	(void)slot_image(1u - s, 0x40000u, false);
	for (unsigned k = 0; k < 2u; k++) {
		const uint32_t base = s02_slot_base(k);

		if (len <= 0x70000u || k != s)
			ff_fill_image(g_f.mem + base + 0x70000u, 0x5000u, 30u + k);
		ff_fill_image(g_f.mem + base + 0x76000u, 0x100u, 40u + k);
	}
	running(s, true);
	memcpy(g_before, g_f.mem, sizeof g_before);
	ff_reset(&g_f);
}

/* A picture from the cases, converted for the slot's room. */
static void picture(const char *name, struct wb_logo *l)
{
	for (unsigned i = 0; i < ARRAY_LEN(lv_cases); i++)
		if (strcmp(lv_cases[i].name, name) == 0) {
			const struct lv_case *c = &lv_cases[i];

			memset(l, 0, sizeof *l);
			l->width = (uint16_t)c->w;
			l->height = (uint16_t)c->h;
			l->bits = (uint8_t)c->bits;
			l->colors = (uint16_t)c->colors;
			l->bg = c->bg;
			l->len = c->len;
			l->crc = c->data_crc;
			memcpy(g_data, c->data, c->len);
			return;
		}
	CHECK(0);
}

/* Every byte outside [lo, hi) of the flash as it was. */
static bool same_outside(uint32_t lo, uint32_t hi)
{
	return memcmp(g_f.mem, g_before, lo) == 0 &&
	       memcmp(g_f.mem + hi, g_before + hi, S02_FLASH_SIZE - hi) == 0;
}

static bool stored(unsigned s, struct wb_logo *got)
{
	static uint8_t buf[WB_LOGO_DATA_MAX];

	return wb_logo_read(&g_env, s, got, buf);
}

static void test_save(void)
{
	static const char *const names[] = { "full_screen", "photo_256", "one_color", "gray1" };

	for (unsigned s = 0; s < 2u; s++)
		for (unsigned i = 0; i < ARRAY_LEN(names); i++) {
			const uint32_t hdr = s02_slot_base(s) + WB_LOGO_SLOT_OFF;
			struct wb_logo l, got;
			unsigned first = ~0u, last = 0;

			setup(s, 0x2345u);
			picture(names[i], &l);
			CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_OK);
			CHECK(!g_f.bad_call);
			CHECK(l.image == g_run.rec.crc32);
			CHECK(stored(s, &got) && got.len == l.len && got.crc == l.crc &&
			      got.width == l.width && got.image == l.image);
			CHECK(memcmp(g_f.mem + hdr - l.len, g_data, l.len) == 0);
			CHECK(same_outside((hdr - l.len) / S02_SECTOR_SIZE * S02_SECTOR_SIZE,
					   hdr + S02_SECTOR_SIZE));
			/* the order: the header's sector erased first, the header
			 * programmed last, everything in range */
			for (unsigned k = 0; k < g_f.nlog; k++) {
				const struct ff_op *o = &g_f.log[k];

				if (o->kind == FF_READ)
					continue;
				CHECK(wb_logo_range_ok(s, g_run.rec.length, o->off, o->len));
				if (first == ~0u)
					first = k;
				last = k;
			}
			CHECK(first != ~0u && g_f.log[first].kind == FF_ERASE && g_f.log[first].off == hdr);
			CHECK(g_f.log[last].kind == FF_PROGRAM && g_f.log[last].off == hdr &&
			      g_f.log[last].len == WB_LOGO_HEAD);
			/* a fault at each operation: no logo is left */
			for (unsigned f = 1, total = g_f.nmut; f <= total; f++) {
				setup(s, 0x2345u);
				g_f.fail = f;
				CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_IO);
				ff_reset(&g_f);
				CHECK(!stored(s, &got));
			}
			/* a program that does not hold: verified, no logo */
			setup(s, 0x2345u);
			g_f.corrupt = 3u;
			g_f.corrupt_mask = 0xFE;
			g_f.corrupt_on = true;
			CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_VERIFY);
			ff_reset(&g_f);
			CHECK(!stored(s, &got));
		}
}

static void test_rule(void)
{
	struct wb_logo l, got;

	picture("gray1", &l);
	/* the settings' rule: nothing written */
	setup(0, 0x2345u);
	g_run.established = false;
	CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_UNUSABLE && g_f.nmut == 0u);
	CHECK(wb_logo_remove(&g_env, &g_run) == WB_LOGO_UNUSABLE && g_f.nmut == 0u);
	setup(0, 0x2345u);
	g_run.good = false;		/* a trial start, or a check that failed */
	CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_UNUSABLE && g_f.nmut == 0u);
	setup(0, 0x2345u);
	g_run.slot = 2u;
	CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_UNUSABLE && g_f.nmut == 0u);
	setup(0, 0x2345u);
	ff_put32(g_f.mem + s02_record_off(0) + 4u, 0x2346u);	/* the record changed */
	memcpy(g_before, g_f.mem, sizeof g_before);
	CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_UNUSABLE && g_f.nmut == 0u);
	/* an image that reaches the header's sector, or leaves no room */
	setup(0, 0x75000u);
	CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_UNUSABLE && g_f.nmut == 0u);
	CHECK(wb_logo_remove(&g_env, &g_run) == WB_LOGO_UNUSABLE && g_f.nmut == 0u);
	setup(0, 0x74001u);
	CHECK(wb_logo_remove(&g_env, &g_run) == WB_LOGO_UNUSABLE && g_f.nmut == 0u);
	setup(0, 0x76000u);
	CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_UNUSABLE && g_f.nmut == 0u);
	/* data larger than the room */
	setup(0, 0x74000u);
	picture("full_screen", &l);
	CHECK(l.len > 0x1000u);
	CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_ROOM && g_f.nmut == 0u);
	/* exactly the room: data from the image's end */
	picture("gray1", &l);
	l.len = 0x1000u;
	memset(g_data + 70u, 0, 0x1000u - 70u);
	l.crc = ref_crc32(0, g_data, l.len);
	CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_OK);
	CHECK(same_outside(0x84000u, 0x86000u));
	CHECK(memcmp(g_f.mem, g_before, 0x84000u) == 0);	/* the image, to its last byte */
	/* the removal: the header's sector alone, once */
	setup(1, 0x2345u);
	picture("gray4", &l);
	CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_OK);
	memcpy(g_before, g_f.mem, sizeof g_before);
	ff_reset(&g_f);
	CHECK(wb_logo_remove(&g_env, &g_run) == WB_LOGO_OK);
	CHECK(g_f.nmut == 1u && g_f.nlog >= 3u && g_f.log[2].kind == FF_ERASE &&
	      g_f.log[2].off == s02_slot_base(1) + WB_LOGO_SLOT_OFF);
	CHECK(same_outside(s02_slot_base(1) + WB_LOGO_SLOT_OFF,
			   s02_slot_base(1) + WB_LOGO_SLOT_OFF + S02_SECTOR_SIZE));
	CHECK(!stored(1, &got));
	ff_reset(&g_f);
	CHECK(wb_logo_remove(&g_env, &g_run) == WB_LOGO_OK && g_f.nmut == 0u);	/* left alone */
	g_f.fail = 1;
	g_f.mem[s02_slot_base(1) + WB_LOGO_SLOT_OFF + 4000u] = 0u;
	CHECK(wb_logo_remove(&g_env, &g_run) == WB_LOGO_IO);
	/* a logo saved for another image does not count */
	setup(0, 0x2345u);
	picture("gray4", &l);
	CHECK(wb_logo_save(&g_env, &g_run, &l, g_data) == WB_LOGO_OK);
	CHECK(stored(0, &got));
	g_f.mem[s02_slot_base(0) + 5u] ^= 1u;
	ff_put32(g_f.mem + s02_record_off(0), ref_crc32(0, g_f.mem + s02_slot_base(0), 0x2345u));
	CHECK(!stored(0, &got));
}

int main(void)
{
	RUN(test_cases);
	RUN(test_room);
	RUN(test_range);
	RUN(test_save);
	RUN(test_rule);
	return check_summary("test_logo");
}
