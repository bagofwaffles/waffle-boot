/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A PNG file to the boot logo's pixels (waffle/logo_png.h). */
#include "waffle/logo_png.h"

#include <string.h>

#include "zlib.h"

#include "waffle/bytes.h"
#include "waffle/logo.h"
#include "waffle/wire_crc.h"

#define STRIDE_MAX (WB_LOGO_W * 4u)
#define PIECE      4096u

struct png {
	wb_png_read_fn read;
	void *ctx;
	uint32_t width, height, depth, ctype, ch, stride, bpp;
	uint8_t plte[768];
	uint32_t nplte;		/* entries */
	uint8_t trns[256];
	uint32_t ntrns;
	bool has_trns;
	uint8_t row[2][STRIDE_MAX + 1u];	/* the filter byte, then the row */
	unsigned cur;
	uint32_t fill, y;
	uint16_t *px;
	z_stream z;
	bool zinit, zend;
};

static struct png g;
static uint8_t piece[PIECE];
static uint8_t inflated[PIECE];

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* wblogo.py's _level(): v * a / top^2 as an 8-bit value, rounded, its high bits. */
static uint32_t level(uint32_t v, uint32_t a, uint32_t top, unsigned bits)
{
	return (2u * v * a * 255u + top * top) / (2u * top * top) >> (8u - bits);
}

static uint16_t rgb565(uint32_t r, uint32_t gr, uint32_t b, uint32_t a, uint32_t top)
{
	return (uint16_t)(level(r, a, top, 5u) << 11 | level(gr, a, top, 6u) << 5 |
			  level(b, a, top, 5u));
}

static uint32_t sample(const uint8_t *line, uint32_t k, uint32_t depth)
{
	const uint32_t per = 8u / depth;

	if (depth == 8u)
		return line[k];
	return (uint32_t)line[k / per] >> (8u - depth * (k % per + 1u)) & ((1u << depth) - 1u);
}

/* The unfiltered row y into its pixels. */
static bool convert(struct png *p, const uint8_t *line)
{
	const uint32_t top = (1u << p->depth) - 1u;
	uint16_t *out = p->px + (size_t)p->y * p->width;

	for (uint32_t x = 0; x < p->width; x++) {
		const uint32_t s0 = sample(line, x * p->ch, p->depth);
		uint32_t s1 = 0, s2 = 0, s3 = 0;

		if (p->ch > 1u)
			s1 = sample(line, x * p->ch + 1u, p->depth);
		if (p->ch > 2u)
			s2 = sample(line, x * p->ch + 2u, p->depth);
		if (p->ch > 3u)
			s3 = sample(line, x * p->ch + 3u, p->depth);
		switch (p->ctype) {
		case 3: {
			uint32_t a = 255u;

			if (s0 >= p->nplte)
				return false;
			if (p->has_trns && s0 < p->ntrns)
				a = p->trns[s0];
			out[x] = rgb565(p->plte[3u * s0], p->plte[3u * s0 + 1u], p->plte[3u * s0 + 2u],
					a, 255u);
			break;
		}
		case 0: {
			const bool key = p->has_trns && p->ntrns >= 2u &&
					 s0 == (uint32_t)(p->trns[0] << 8 | p->trns[1]);

			out[x] = rgb565(s0, s0, s0, key ? 0u : top, top);
			break;
		}
		case 2: {
			const bool key = p->has_trns && p->ntrns >= 6u &&
					 s0 == (uint32_t)(p->trns[0] << 8 | p->trns[1]) &&
					 s1 == (uint32_t)(p->trns[2] << 8 | p->trns[3]) &&
					 s2 == (uint32_t)(p->trns[4] << 8 | p->trns[5]);

			out[x] = rgb565(s0, s1, s2, key ? 0u : 255u, 255u);
			break;
		}
		case 4:
			out[x] = rgb565(s0, s0, s0, s1, 255u);
			break;
		default:
			out[x] = rgb565(s0, s1, s2, s3, 255u);
			break;
		}
	}
	return true;
}

/* A whole row arrived: unfiltered against the one before it, converted. */
static bool row_done(struct png *p)
{
	uint8_t *line = p->row[p->cur] + 1;
	const uint8_t *prev = p->row[p->cur ^ 1u] + 1;
	const unsigned ft = p->row[p->cur][0];

	if (ft > 4u)
		return false;
	for (uint32_t k = 0; k < p->stride; k++) {
		const uint32_t a = k >= p->bpp ? line[k - p->bpp] : 0u, b = prev[k];
		const uint32_t c = k >= p->bpp ? prev[k - p->bpp] : 0u;
		uint32_t v = 0;

		if (ft == 1u) {
			v = a;
		} else if (ft == 2u) {
			v = b;
		} else if (ft == 3u) {
			v = (a + b) / 2u;
		} else if (ft == 4u) {
			const int32_t q = (int32_t)(a + b) - (int32_t)c;
			const int32_t pa = q > (int32_t)a ? q - (int32_t)a : (int32_t)a - q;
			const int32_t pb = q > (int32_t)b ? q - (int32_t)b : (int32_t)b - q;
			const int32_t pc = q > (int32_t)c ? q - (int32_t)c : (int32_t)c - q;

			v = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
		}
		line[k] = (uint8_t)(line[k] + v);
	}
	if (!convert(p, line))
		return false;
	p->cur ^= 1u;
	p->y++;
	p->fill = 0;
	return true;
}

/* Inflated bytes into rows; those past the last row are dropped. */
static bool take(struct png *p, const uint8_t *b, uint32_t n)
{
	const uint32_t whole = p->stride + 1u;

	for (uint32_t i = 0; i < n && p->y < p->height;) {
		uint32_t k = whole - p->fill;

		k = k < n - i ? k : n - i;
		memcpy(p->row[p->cur] + p->fill, b + i, k);
		p->fill += k;
		i += k;
		if (p->fill == whole && !row_done(p))
			return false;
	}
	return true;
}

static bool feed(struct png *p, const uint8_t *b, uint32_t n)
{
	if (p->zend)
		return true;	/* past the stream's end: unused, as zlib leaves it */
	p->z.next_in = (Bytef *)(uintptr_t)b;	/* inflate() only reads its input */
	p->z.avail_in = n;
	for (;;) {
		int ret;

		p->z.next_out = inflated;
		p->z.avail_out = sizeof inflated;
		ret = inflate(&p->z, Z_NO_FLUSH);
		if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
			return false;
		if (!take(p, inflated, (uint32_t)(sizeof inflated - p->z.avail_out)))
			return false;
		if (ret == Z_STREAM_END) {
			p->zend = true;
			return true;
		}
		if (p->z.avail_out != 0u && (p->z.avail_in == 0u || ret == Z_BUF_ERROR))
			return true;
	}
}

static enum wb_png_rc header(struct png *p, const uint8_t *b)
{
	static const uint8_t ch[7] = { 1, 0, 3, 1, 2, 0, 4 };

	p->width = be32(b);
	p->height = be32(b + 4);
	p->depth = b[8];
	p->ctype = b[9];
	if (p->ctype > 6u || ch[p->ctype] == 0u || b[10] != 0u || b[11] != 0u ||
	    (p->depth != 8u && !((p->ctype == 0u || p->ctype == 3u) &&
				 (p->depth == 1u || p->depth == 2u || p->depth == 4u))))
		return WB_PNG_TYPE;
	if (b[12] != 0u)
		return WB_PNG_INTERLACED;
	if (p->width < 1u || p->width > WB_LOGO_W || p->height < 1u || p->height > WB_LOGO_H)
		return WB_PNG_SIZE;
	p->ch = ch[p->ctype];
	p->stride = (p->width * p->ch * p->depth + 7u) / 8u;
	p->bpp = p->ch * p->depth / 8u != 0u ? p->ch * p->depth / 8u : 1u;
	return WB_PNG_OK;
}

static enum wb_png_rc decode(struct png *p, uint32_t size)
{
	uint8_t hd[8];
	uint32_t off = 8;
	bool seen = false, have = false;

	if (size < 8u)
		return WB_PNG_NOT_PNG;
	if (!p->read(p->ctx, 0, hd, 8u))
		return WB_PNG_READ;
	if (memcmp(hd, "\x89PNG\r\n\x1a\n", 8u) != 0)
		return WB_PNG_NOT_PNG;
	for (;;) {
		uint8_t kind[4];
		uint32_t n, crc;
		bool ihdr, plte, trns, idat;
		enum wb_png_rc rc;

		if (size - off < 12u)
			return WB_PNG_DAMAGED;
		if (!p->read(p->ctx, off, hd, 8u))
			return WB_PNG_READ;
		n = be32(hd);
		memcpy(kind, hd + 4, 4u);
		ihdr = memcmp(kind, "IHDR", 4u) == 0;
		plte = memcmp(kind, "PLTE", 4u) == 0;
		trns = memcmp(kind, "tRNS", 4u) == 0;
		idat = memcmp(kind, "IDAT", 4u) == 0;
		if (n > size - off - 12u)
			return WB_PNG_DAMAGED;
		/* the header first and once; the palette and the key before the data */
		if (have == ihdr || (seen && (plte || trns)) || (ihdr && n != 13u) ||
		    (plte && n > 768u) || (trns && n > 256u))
			return WB_PNG_DAMAGED;
		if (idat && !seen) {
			seen = true;
			if (p->ctype == 3u && p->nplte == 0u)
				return WB_PNG_DAMAGED;
			if (inflateInit(&p->z) != Z_OK)
				return WB_PNG_DAMAGED;
			p->zinit = true;
		}
		crc = wire_crc32(0u, kind, 4u);
		for (uint32_t at = 0; at < n;) {
			const uint32_t k = n - at < PIECE ? n - at : PIECE;

			if (!p->read(p->ctx, off + 8u + at, piece, k))
				return WB_PNG_READ;
			crc = wire_crc32(crc, piece, k);
			if (idat && !feed(p, piece, k))
				return WB_PNG_DAMAGED;
			at += k;
		}
		if (!p->read(p->ctx, off + 8u + n, hd, 4u))
			return WB_PNG_READ;
		if (be32(hd) != crc)
			return WB_PNG_DAMAGED;
		/* a small chunk's body is whole in piece */
		if (ihdr) {
			rc = header(p, piece);
			if (rc != WB_PNG_OK)
				return rc;
			have = true;
		} else if (plte) {
			memcpy(p->plte, piece, n);
			p->nplte = n / 3u;
		} else if (trns) {
			memcpy(p->trns, piece, n);
			p->ntrns = n;
			p->has_trns = true;
		} else if (memcmp(kind, "IEND", 4u) == 0) {
			return p->zend && p->y == p->height ? WB_PNG_OK : WB_PNG_DAMAGED;
		}
		off += 12u + n;
	}
}

enum wb_png_rc wb_png_decode(wb_png_read_fn read, void *ctx, uint32_t size, uint16_t *px,
			     unsigned *w, unsigned *h)
{
	enum wb_png_rc rc;

	memset(&g, 0, sizeof g);
	g.read = read;
	g.ctx = ctx;
	g.px = px;
	rc = decode(&g, size);
	if (g.zinit)
		(void)inflateEnd(&g.z);
	*w = g.width;
	*h = g.height;
	return rc;
}

const char *wb_png_why(enum wb_png_rc rc)
{
	switch (rc) {
	case WB_PNG_OK:
		return "";
	case WB_PNG_READ:
		return "file not read";
	case WB_PNG_NOT_PNG:
		return "not a PNG file";
	case WB_PNG_TYPE:
		return "PNG type not taken: use 8-bit";
	case WB_PNG_INTERLACED:
		return "interlaced PNG";
	case WB_PNG_SIZE:
		return "over 480 x 222";
	case WB_PNG_DAMAGED:
	default:
		return "damaged PNG file";
	}
}
