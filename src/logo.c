/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The stored boot logo (waffle/logo.h); tools/wblogo.py is the reference. */
#include "waffle/logo.h"

#include <string.h>

#include "waffle/bytes.h"
#include "waffle/wire_crc.h"

#define SECTOR S02_SECTOR_SIZE
#define PAGE   S02_PAGE_SIZE
#define NCOLOR 65536u

_Static_assert(S02_STORE_SLOT_MAX == WB_LOGO_SLOT_OFF + SECTOR,
	       "the logo's header sector lies right below the settings sector");

static uint32_t up(uint32_t n)
{
	return (n + SECTOR - 1u) / SECTOR * SECTOR;
}

/* ---- the header ---- */

bool wb_logo_parse(const uint8_t h[WB_LOGO_HEAD], const struct s02_record *rec,
		   struct wb_logo *l)
{
	const uint32_t bits = h[FR_LOGO_O_BITS];

	if (wb_get_le32(h + FR_LOGO_O_MAGIC) != FR_LOGO_MAGIC ||
	    wb_get_le16(h + FR_LOGO_O_VERSION) != FR_LOGO_VERSION ||
	    wb_get_le16(h + FR_LOGO_O_SIZE) != WB_LOGO_HEAD ||
	    wire_crc32(0u, h, FR_LOGO_O_HCRC) != wb_get_le32(h + FR_LOGO_O_HCRC))
		return false;
	l->width = wb_get_le16(h + FR_LOGO_O_WIDTH);
	l->height = wb_get_le16(h + FR_LOGO_O_HEIGHT);
	l->bits = (uint8_t)bits;
	l->colors = wb_get_le16(h + FR_LOGO_O_COLORS);
	l->bg = wb_get_le16(h + FR_LOGO_O_BG);
	l->len = wb_get_le32(h + FR_LOGO_O_LEN);
	l->crc = wb_get_le32(h + FR_LOGO_O_CRC);
	l->image = wb_get_le32(h + FR_LOGO_O_IMAGE);
	if (h[FR_LOGO_O_BITS + 1u] != 0u || wb_get_le16(h + FR_LOGO_O_BG + 2u) != 0u ||
	    l->width < 1u || l->width > WB_LOGO_W || l->height < 1u || l->height > WB_LOGO_H ||
	    (bits != 1u && bits != 2u && bits != 4u && bits != 8u) ||
	    l->colors < 2u || l->colors > (1u << bits))
		return false;
	if (l->image != rec->crc32 || wb_logo_room(rec) == 0u)
		return false;
	return l->len >= 2u * l->colors && l->len <= WB_LOGO_DATA_MAX &&
	       l->len <= wb_logo_room(rec);
}

void wb_logo_header(const struct wb_logo *l, uint8_t h[WB_LOGO_HEAD])
{
	memset(h, 0, WB_LOGO_HEAD);
	wb_put_le32(h + FR_LOGO_O_MAGIC, FR_LOGO_MAGIC);
	wb_put_le16(h + FR_LOGO_O_VERSION, FR_LOGO_VERSION);
	wb_put_le16(h + FR_LOGO_O_SIZE, WB_LOGO_HEAD);
	wb_put_le16(h + FR_LOGO_O_WIDTH, l->width);
	wb_put_le16(h + FR_LOGO_O_HEIGHT, l->height);
	h[FR_LOGO_O_BITS] = l->bits;
	wb_put_le16(h + FR_LOGO_O_COLORS, l->colors);
	wb_put_le16(h + FR_LOGO_O_BG, l->bg);
	wb_put_le32(h + FR_LOGO_O_LEN, l->len);
	wb_put_le32(h + FR_LOGO_O_CRC, l->crc);
	wb_put_le32(h + FR_LOGO_O_IMAGE, l->image);
	wb_put_le32(h + FR_LOGO_O_HCRC, wire_crc32(0u, h, FR_LOGO_O_HCRC));
}

uint32_t wb_logo_room(const struct s02_record *rec)
{
	if (!s02_tag_ok(rec->tag) || rec->length < 1u || rec->length > WB_LOGO_SLOT_OFF)
		return 0u;
	return WB_LOGO_SLOT_OFF - up(rec->length);
}

/* ---- drawing, as fr_logo_draw() ---- */

bool wb_logo_draw(const struct wb_logo *l, const uint8_t *data, uint16_t *px)
{
	const uint8_t *p = data + 2u * l->colors;
	const uint32_t n = l->len - 2u * l->colors, mask = (1u << l->bits) - 1u;
	const uint32_t r0 = WB_LOGO_W - (WB_LOGO_W - l->width) / 2u - l->width;
	const uint32_t c0 = (WB_LOGO_H - l->height) / 2u;
	uint32_t pos = 0;

	if (l->len < 2u * l->colors || wire_crc32(0u, data, l->len) != l->crc)
		return false;
	for (uint32_t i = 0; i < WB_LOGO_W * WB_LOGO_H; i++)
		px[i] = l->bg;
	for (uint32_t j = 0; j < l->width; j++) {
		uint16_t *row = px + (size_t)(r0 + j) * WB_LOGO_H + c0;

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
				v = wb_get_le16(data + 2u * p[pos++]);
				for (k = 0; k < cnt; k++)
					row[y + k] = (uint16_t)v;
			} else {
				const uint32_t nb = (cnt * l->bits + 7u) / 8u;

				if (nb > n - pos)
					return false;
				for (k = 0; k < cnt; k++) {
					const uint32_t b = k * l->bits;

					v = (uint32_t)p[pos + b / 8u] >> (8u - l->bits - b % 8u) & mask;
					if (v >= l->colors)
						return false;
					row[y + k] = wb_get_le16(data + 2u * v);
				}
				pos += nb;
			}
			y += cnt;
		}
	}
	return pos == n;
}

/* ---- conversion ---- */

/* The working memory of the conversion under way (wb_logo_convert()'s k). */
#define ERR_N (2u * (WB_LOGO_W + 2u) * 3u)
static uint32_t *hist;		/* each RGB565 color's count */
static uint16_t *cols;		/* the colors, grouped by box */
static uint8_t *lut;		/* a color's palette entry */
static uint8_t *idx;		/* each pixel's */
static int32_t *err;		/* two rows of the dithering's errors */
static struct wb_logo_box *boxes;
_Static_assert(sizeof(((struct wb_logo_work *)0)->err) == ERR_N * sizeof(int32_t),
	       "two rows of errors");

static unsigned comp(uint16_t c, unsigned k)
{
	return k == 0u ? (unsigned)(c >> 11) & 31u : k == 1u ? (unsigned)(c >> 5) & 63u : c & 31u;
}

/* median cut's scale: red and blue doubled */
static unsigned scaled(uint16_t c, unsigned k)
{
	return k == 1u ? comp(c, k) : 2u * comp(c, k);
}

static void widest(struct wb_logo_box *b)
{
	unsigned lo[3] = { 255u, 255u, 255u }, hi[3] = { 0u, 0u, 0u }, rr, gr, br;

	for (uint32_t i = b->lo; i < b->hi; i++)
		for (unsigned k = 0; k < 3u; k++) {
			const unsigned v = scaled(cols[i], k);

			lo[k] = v < lo[k] ? v : lo[k];
			hi[k] = v > hi[k] ? v : hi[k];
		}
	rr = hi[0] - lo[0];
	gr = hi[1] - lo[1];
	br = hi[2] - lo[2];
	if (gr >= rr && gr >= br) {
		b->range = gr;
		b->ch = 1u;
	} else if (rr >= br) {
		b->range = rr;
		b->ch = 0u;
	} else {
		b->range = br;
		b->ch = 2u;
	}
}

/* The sort key: the component, then the color; every key differs. */
static uint32_t key(uint16_t c, unsigned ch)
{
	return (uint32_t)scaled(c, ch) << 16 | c;
}

static void sift(uint16_t *a, uint32_t i, uint32_t n, unsigned ch)
{
	for (;;) {
		uint32_t m = i, l = 2u * i + 1u, r = l + 1u;
		uint16_t t;

		if (l < n && key(a[l], ch) > key(a[m], ch))
			m = l;
		if (r < n && key(a[r], ch) > key(a[m], ch))
			m = r;
		if (m == i)
			return;
		t = a[i];
		a[i] = a[m];
		a[m] = t;
		i = m;
	}
}

/* Heapsort by key(): the keys differ, so the order is the one sorted() gives. */
static void sort(uint16_t *a, uint32_t n, unsigned ch)
{
	for (uint32_t i = n / 2u; i-- > 0u;)
		sift(a, i, n, ch);
	for (uint32_t e = n; e-- > 1u;) {
		const uint16_t t = a[0];

		a[0] = a[e];
		a[e] = t;
		sift(a, 0, e, ch);
	}
}

/* Median cut of the ncols colors in cols to at most target boxes; the
 * palette's colors, and lut for each color. */
static unsigned median_cut(uint32_t ncols, unsigned target, uint16_t *pal)
{
	unsigned nb = 1u;

	boxes[0].lo = 0;
	boxes[0].hi = ncols;
	widest(&boxes[0]);
	while (nb < target) {
		unsigned bi = nb;
		uint32_t best = 0, total = 0, cum = 0, k;
		struct wb_logo_box *b;

		for (unsigned i = 0; i < nb; i++)
			if (boxes[i].hi - boxes[i].lo >= 2u && boxes[i].range > best) {
				bi = i;
				best = boxes[i].range;
			}
		if (bi == nb)
			break;
		b = &boxes[bi];
		sort(cols + b->lo, b->hi - b->lo, b->ch);
		for (uint32_t i = b->lo; i < b->hi; i++)
			total += hist[cols[i]];
		k = b->hi - b->lo - 1u;
		for (uint32_t i = b->lo; i < b->hi; i++) {
			cum += hist[cols[i]];
			if (2u * cum >= total) {
				k = i - b->lo + 1u < k ? i - b->lo + 1u : k;
				break;
			}
		}
		boxes[nb].lo = b->lo + k;
		boxes[nb].hi = b->hi;
		b->hi = b->lo + k;
		widest(b);
		widest(&boxes[nb]);
		nb++;
	}
	for (unsigned i = 0; i < nb; i++) {
		uint32_t t = 0, s[3] = { 0u, 0u, 0u }, v[3];

		for (uint32_t j = boxes[i].lo; j < boxes[i].hi; j++) {
			const uint16_t c = cols[j];

			t += hist[c];
			for (unsigned k = 0; k < 3u; k++)
				s[k] += hist[c] * comp(c, k);
			lut[c] = (uint8_t)i;
		}
		/* t >= 1: every color in a box has a count; 1 against no box */
		if (t == 0u)
			t = 1u;
		for (unsigned k = 0; k < 3u; k++)
			v[k] = (2u * s[k] + t) / (2u * t);
		pal[i] = (uint16_t)(v[0] << 11 | v[1] << 5 | v[2]);
	}
	return nb;
}

/* Floor division by 16, as Python's //. */
static int32_t fdiv16(int32_t v)
{
	return v >= 0 ? v / 16 : -((-v + 15) / 16);
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* Floyd-Steinberg onto pal, the error in sixteenths of a component. */
static void dither(const uint16_t *px, unsigned w, unsigned h, const uint16_t *pal, unsigned n)
{
	static const int32_t top[3] = { 31, 63, 31 };
	int32_t *cur = err, *nxt = err + ERR_N / 2u, *t;

	memset(err, 0, ERR_N * sizeof err[0]);
	for (unsigned y = 0; y < h; y++) {
		memset(nxt, 0, ERR_N / 2u * sizeof err[0]);
		for (unsigned x = 0; x < w; x++) {
			const uint16_t c = px[y * w + x];
			int32_t want[3], q[3];
			unsigned best = 0;
			uint32_t bd = 0;

			for (unsigned k = 0; k < 3u; k++) {
				want[k] = (int32_t)comp(c, k) * 16 + cur[(x + 1u) * 3u + k];
				q[k] = clamp(fdiv16(want[k] + 8), 0, top[k]);
			}
			for (unsigned i = 0; i < n; i++) {
				const int32_t dr = (int32_t)comp(pal[i], 0) - q[0];
				const int32_t dg = (int32_t)comp(pal[i], 1) - q[1];
				const int32_t db = (int32_t)comp(pal[i], 2) - q[2];
				const uint32_t d = (uint32_t)(4 * dr * dr + dg * dg + 4 * db * db);

				if (i == 0u || d < bd) {
					best = i;
					bd = d;
				}
			}
			idx[y * w + x] = (uint8_t)best;
			for (unsigned k = 0; k < 3u; k++) {
				const int32_t e = clamp(want[k], 0, top[k] * 16) -
						  (int32_t)comp(pal[best], k) * 16;
				const int32_t e7 = fdiv16(e * 7), e3 = fdiv16(e * 3), e5 = fdiv16(e * 5);

				cur[(x + 2u) * 3u + k] += e7;
				nxt[x * 3u + k] += e3;
				nxt[(x + 1u) * 3u + k] += e5;
				nxt[(x + 2u) * 3u + k] += e - e7 - e3 - e5;
			}
		}
		t = cur;
		cur = nxt;
		nxt = t;
	}
}

static unsigned bits_for(unsigned n)
{
	return n <= 2u ? 1u : n <= 4u ? 2u : n <= 16u ? 4u : 8u;
}

struct out {
	uint8_t *p;
	uint32_t n, cap;
	bool full;
};

static void emit(struct out *o, uint8_t b)
{
	if (o->n >= o->cap) {
		o->full = true;
		return;
	}
	o->p[o->n++] = b;
}

static void flush(struct out *o, const uint8_t *lit, unsigned n, unsigned bits)
{
	uint32_t at;

	if (n == 0u)
		return;
	emit(o, (uint8_t)(n - 1u));
	at = o->n;
	for (unsigned k = 0; k < (n * bits + 7u) / 8u; k++)
		emit(o, 0u);
	if (o->full)
		return;
	for (unsigned k = 0; k < n; k++) {
		const unsigned b = k * bits;

		o->p[at + b / 8u] = (uint8_t)(o->p[at + b / 8u] | lit[k] << (8u - bits - b % 8u));
	}
}

/* The lines in the panel's order (wblogo.py's pack()). */
static void pack(struct out *o, unsigned w, unsigned h, unsigned bits)
{
	const unsigned minrun = 16u / bits + 1u;
	uint8_t lit[128];

	for (unsigned j = 0; j < w; j++) {
		const unsigned x = w - 1u - j;
		unsigned nl = 0, i = 0;

		while (i < h) {
			const uint8_t v = idx[i * w + x];
			unsigned n = 1;

			while (i + n < h && n < 128u && idx[(i + n) * w + x] == v)
				n++;
			if (n >= minrun) {
				flush(o, lit, nl, bits);
				nl = 0;
				emit(o, (uint8_t)(0x80u | (n - 1u)));
				emit(o, v);
				i += n;
			} else {
				lit[nl++] = v;
				i++;
				if (nl == 128u) {
					flush(o, lit, nl, bits);
					nl = 0;
				}
			}
		}
		flush(o, lit, nl, bits);
	}
}

/* The data for pal and idx; its length, or cap + 1 when it does not fit. */
static uint32_t stored(unsigned w, unsigned h, const uint16_t *pal, unsigned n, uint8_t *data,
		       uint32_t cap, struct wb_logo *l)
{
	struct out o = { data, 0u, cap, false };

	for (unsigned i = 0; i < n; i++) {
		emit(&o, (uint8_t)pal[i]);
		emit(&o, (uint8_t)(pal[i] >> 8));
	}
	l->bits = (uint8_t)bits_for(n);
	l->colors = (uint16_t)n;
	pack(&o, w, h, l->bits);
	return o.full ? cap + 1u : o.n;
}

bool wb_logo_convert(struct wb_logo_work *k, const uint16_t *px, unsigned w, unsigned h,
		     uint32_t room, uint8_t *data, uint32_t cap, struct wb_logo *out,
		     enum wb_logo_way *way, uint32_t *need)
{
	uint16_t pal[256];
	uint32_t ncols = 0, n;
	unsigned np;
	const uint32_t lim = room < cap ? room : cap;

	*need = 0;
	if (w < 1u || w > WB_LOGO_W || h < 1u || h > WB_LOGO_H)
		return false;
	hist = k->hist;
	cols = k->cols;
	lut = k->lut;
	idx = k->idx;
	err = &k->err[0][0];
	boxes = k->boxes;
	memset(hist, 0, sizeof k->hist);
	for (uint32_t i = 0; i < w * h; i++)
		hist[px[i]]++;
	for (uint32_t c = 0; c < NCOLOR; c++)
		if (hist[c] != 0u)
			cols[ncols++] = (uint16_t)c;
	out->width = (uint16_t)w;
	out->height = (uint16_t)h;
	out->bg = px[0];
	if (ncols <= 256u) {
		for (uint32_t i = 0; i < ncols; i++) {
			pal[i] = cols[i];
			lut[cols[i]] = (uint8_t)i;
		}
		np = ncols;
		if (np == 1u)
			pal[np++] = (uint16_t)(cols[0] ^ 0xFFFFu);
		*way = WB_LOGO_KEPT;
	} else {
		np = median_cut(ncols, 256u, pal);
		*way = WB_LOGO_REDUCED;
	}
	for (uint32_t i = 0; i < w * h; i++)
		idx[i] = lut[px[i]];
	n = stored(w, h, pal, np, data, lim, out);
	*need = n;
	if (n > lim && ncols > 16u) {
		for (uint32_t c = 0, i = 0; c < NCOLOR; c++)
			if (hist[c] != 0u)
				cols[i++] = (uint16_t)c;
		np = median_cut(ncols, 16u, pal);
		dither(px, w, h, pal, np);
		*way = WB_LOGO_DITHERED;
		n = stored(w, h, pal, np, data, lim, out);
		*need = n;
	}
	if (n > lim)
		return false;
	out->len = n;
	out->crc = wire_crc32(0u, data, n);
	out->image = 0u;
	return true;
}

/* ---- the flash ---- */

bool wb_logo_range_ok(unsigned slot, uint32_t rec_len, uint32_t off, uint32_t len)
{
	uint32_t lo, hi;

	if (slot > 1u || len == 0u || rec_len < 1u || rec_len > WB_LOGO_SLOT_OFF)
		return false;
	lo = s02_slot_base(slot) + up(rec_len);
	hi = s02_slot_base(slot) + WB_LOGO_SLOT_OFF + SECTOR;
	return off >= lo && off <= hi && len <= hi - off;
}

/* The settings' rule (s02_store.h's usable()), with the logo's length. */
static bool usable(const struct s02_env *env, const struct s02_store_slot *run)
{
	struct s02_record now;

	if (!run->established || !run->good || run->slot > 1u ||
	    env->scratch_len < SECTOR || wb_logo_room(&run->rec) == 0u)
		return false;
	if (s02_read_record(env, run->slot, &now) != S02_OK)
		return false;
	return now.crc32 == run->rec.crc32 && now.length == run->rec.length &&
	       now.tag == run->rec.tag;
}

/* Every erase and program goes through these two. */
static bool erase(const struct s02_env *env, const struct s02_store_slot *run, uint32_t off)
{
	return off % SECTOR == 0u && wb_logo_range_ok(run->slot, run->rec.length, off, SECTOR) &&
	       env->flash->erase(env->flash->ctx, off) == 0;
}

static bool program(const struct s02_env *env, const struct s02_store_slot *run, uint32_t off,
		    const uint8_t *p, uint32_t n)
{
	return n >= 1u && off % PAGE + n <= PAGE &&
	       wb_logo_range_ok(run->slot, run->rec.length, off, n) &&
	       env->flash->program(env->flash->ctx, off, p, n) == 0;
}

static uint32_t header_off(unsigned slot)
{
	return s02_slot_base(slot) + WB_LOGO_SLOT_OFF;
}

/* The header's sector erased and read back erased. */
static enum wb_logo_rc clear_header(const struct s02_env *env, const struct s02_store_slot *run)
{
	const uint32_t at = header_off(run->slot);

	if (!erase(env, run, at) || env->flash->read(env->flash->ctx, at, env->scratch, SECTOR) != 0)
		return WB_LOGO_IO;
	for (uint32_t i = 0; i < SECTOR; i++)
		if (env->scratch[i] != S02_ERASED)
			return WB_LOGO_VERIFY;
	return WB_LOGO_OK;
}

enum wb_logo_rc wb_logo_save(const struct s02_env *env, const struct s02_store_slot *run,
			     struct wb_logo *l, const uint8_t *data)
{
	uint8_t h[WB_LOGO_HEAD];
	uint32_t at, start, crc = 0;
	enum wb_logo_rc rc;

	if (!usable(env, run))
		return WB_LOGO_UNUSABLE;
	if (l->len < 2u * l->colors || l->len > wb_logo_room(&run->rec) ||
	    l->len > WB_LOGO_DATA_MAX)
		return WB_LOGO_ROOM;
	l->image = run->rec.crc32;
	wb_logo_header(l, h);
	at = header_off(run->slot);
	start = at - l->len;
	/* a cut from here on leaves no logo */
	rc = clear_header(env, run);
	if (rc != WB_LOGO_OK)
		return rc;
	for (uint32_t s = start / SECTOR * SECTOR; s < at; s += SECTOR)
		if (!erase(env, run, s))
			return WB_LOGO_IO;
	for (uint32_t off = start; off < at;) {
		const uint32_t n = at - off < PAGE - off % PAGE ? at - off : PAGE - off % PAGE;

		if (!program(env, run, off, data + (off - start), n))
			return WB_LOGO_IO;
		off += n;
	}
	for (uint32_t off = start; off < at;) {
		const uint32_t n = at - off < SECTOR ? at - off : SECTOR;

		if (env->flash->read(env->flash->ctx, off, env->scratch, n) != 0)
			return WB_LOGO_IO;
		if (!wb_equal(env->scratch, data + (off - start), n))
			return WB_LOGO_VERIFY;
		crc = wire_crc32(crc, env->scratch, n);
		off += n;
	}
	if (crc != l->crc)
		return WB_LOGO_VERIFY;
	/* the header last */
	if (!program(env, run, at, h, WB_LOGO_HEAD) ||
	    env->flash->read(env->flash->ctx, at, env->scratch, WB_LOGO_HEAD) != 0)
		return WB_LOGO_IO;
	return wb_equal(env->scratch, h, WB_LOGO_HEAD) ? WB_LOGO_OK : WB_LOGO_VERIFY;
}

enum wb_logo_rc wb_logo_remove(const struct s02_env *env, const struct s02_store_slot *run)
{
	bool erased = true;

	if (!usable(env, run))
		return WB_LOGO_UNUSABLE;
	if (env->flash->read(env->flash->ctx, header_off(run->slot), env->scratch, SECTOR) != 0)
		return WB_LOGO_IO;
	for (uint32_t i = 0; i < SECTOR; i++)
		erased = erased && env->scratch[i] == S02_ERASED;
	return erased ? WB_LOGO_OK : clear_header(env, run);
}

bool wb_logo_read(const struct s02_env *env, unsigned slot, struct wb_logo *l, uint8_t *data)
{
	struct s02_record rec;
	uint8_t h[WB_LOGO_HEAD];

	if (slot > 1u || s02_read_record(env, slot, &rec) != S02_OK ||
	    env->flash->read(env->flash->ctx, header_off(slot), h, WB_LOGO_HEAD) != 0 ||
	    !wb_logo_parse(h, &rec, l))
		return false;
	return env->flash->read(env->flash->ctx, header_off(slot) - l->len, data, l->len) == 0 &&
	       wire_crc32(0u, data, l->len) == l->crc;
}
