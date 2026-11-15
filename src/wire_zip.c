/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "waffle/wire_crc.h"
#include "waffle/wire_zip.h"

#define EOCD_LEN 22u
#define LOCATOR_LEN 20u

static unzFile open_zip(struct wire_zip *z)
{
	if (!z->opened) {
		z->opened = true;
		if (z->pkg != NULL) {
			z->buf = z->pkg->zip;	/* NULL: in place (S-11.018) */
			z->len = z->pkg->zip_len;
			z->uf = wire_package_unzip(z->pkg, &z->mem);
		} else {
			z->uf = wb_unzip_open_mem(&z->mem, z->buf, z->len);
		}
		z->at_valid = false;
	}
	return z->uf;
}

/* Puts minizip at member `index`, in archive order. */
static bool go_to(struct wire_zip *z, size_t index)
{
	unzFile uf = open_zip(z);

	if (uf == NULL)
		return false;
	if (!z->at_valid || z->at > index) {
		z->at_valid = unzGoToFirstFile(uf) == UNZ_OK;
		z->at = 0;
	}
	while (z->at_valid && z->at < index) {
		z->at_valid = unzGoToNextFile(uf) == UNZ_OK;
		z->at++;
	}
	return z->at_valid;
}

/* The ids of an extra field (APPNOTE 4.5), fed in pieces; a header cut
 * short at the end is ignored. */
struct extra_scan {
	uint32_t skip;
	uint8_t head[4];
	unsigned have;
	bool zip64;			/* an id 1 (zip64) was seen */
};

static void extra_feed(struct extra_scan *x, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (x->skip != 0) {
			x->skip--;
			continue;
		}
		x->head[x->have++] = p[i];
		if (x->have == 4) {
			if (x->head[0] == 1 && x->head[1] == 0)
				x->zip64 = true;
			x->skip = (uint32_t)x->head[2] | (uint32_t)x->head[3] << 8;
			x->have = 0;
		}
	}
}

/* Whether the n-byte extra field p names a zip64 record (id 1). */
static bool extra_zip64(const uint8_t *p, size_t n)
{
	struct extra_scan x = { 0, { 0, 0, 0, 0 }, 0, false };

	extra_feed(&x, p, n);
	return x.zip64;
}

/* A field longer than z->extra is read whole into memory of its own
 * (at most 0xFFFF bytes, APPNOTE 4.4.11); NULL when that cannot be had. */
static uint8_t *extra_buf(struct wire_zip *z, size_t n)
{
	return n <= sizeof(z->extra) ? z->extra : malloc(n);
}

static void extra_done(const struct wire_zip *z, uint8_t *p)
{
	if (p != z->extra)
		free(p);
}

/* A zip64 extra field in the local header of the member minizip is at,
 * inspected in full (S-11.030: only zip64 records refuse a member). A
 * member whose local header cannot be read, or whose field has no room in
 * memory, is taken as zip64 and refused: it is one the reader cannot
 * vouch for. */
static bool local_zip64(struct wire_zip *z)
{
	int method, level, size, n = -1;
	uint8_t *p;
	bool zip64;

	if (unzOpenCurrentFile2(z->uf, &method, &level, 1) != UNZ_OK)
		return false;
	size = unzGetLocalExtrafield(z->uf, NULL, 0);
	if (size == 0)
		n = 0;
	p = size > 0 ? extra_buf(z, (size_t)size) : NULL;
	if (p != NULL)
		n = unzGetLocalExtrafield(z->uf, p, (unsigned)size);
	zip64 = size < 0 || n != size ||
		(n > 0 && extra_zip64(p, (size_t)n));
	if (p != NULL)
		extra_done(z, p);
	(void)unzCloseCurrentFile(z->uf);
	return zip64;
}

/* The central extra field of the member minizip is at, when it is longer
 * than z->extra; with no room for it the member is taken as zip64. */
static bool central_long_zip64(struct wire_zip *z, uLong n)
{
	uint8_t *p = extra_buf(z, (size_t)n);
	bool zip64 = true;

	if (p == NULL)
		return true;
	if (unzGetCurrentFileInfo64(z->uf, NULL, NULL, 0, p, n, NULL, 0) ==
	    UNZ_OK)
		zip64 = extra_zip64(p, (size_t)n);
	extra_done(z, p);
	return zip64;
}

static bool zip_member(void *ctx, size_t index, struct s11_member *m)
{
	struct wire_zip *z = ctx;
	unz_file_info64 info;

	if (!go_to(z, index) ||
	    unzGetCurrentFileInfo64(z->uf, &info, m->name, sizeof(m->name),
				    z->extra, sizeof(z->extra), NULL,
				    0) != UNZ_OK)
		return false;
	/* a name that does not fit is refused as an empty one (S-11.031) */
	if (info.size_filename >= sizeof(m->name))
		m->name[0] = '\0';
	else
		m->name[info.size_filename] = '\0';
	m->method = (uint16_t)info.compression_method;
	m->flags = (uint16_t)info.flag;
	m->size = (uint32_t)info.uncompressed_size;
	m->crc32 = (uint32_t)info.crc;
	/* the central extra field, inspected in full (S-11.030) */
	m->zip64 = info.size_file_extra > sizeof(z->extra) ?
		   central_long_zip64(z, info.size_file_extra) :
		   extra_zip64(z->extra, info.size_file_extra);
	m->zip64 = m->zip64 || info.uncompressed_size > 0xFFFFFFFFu ||
		   info.compressed_size > 0xFFFFFFFFu || local_zip64(z);
	return true;
}

/* n bytes of the zip from its offset off, in RAM or in the package. */
static bool zip_bytes(const struct wire_zip *z, size_t off, uint8_t *buf,
		      size_t n)
{
	if (z->buf != NULL) {
		memcpy(buf, z->buf + off, n);
		return true;
	}
	return z->pkg->src.read(z->pkg->src.ctx, z->pkg->zip_at + (uint32_t)off,
				buf, n);
}

/* The zip64 end-of-central-directory locator just before the end record
 * minizip reads (the last one, APPNOTE 4.3.15). An archive that cannot be
 * opened is refused here too. */
static bool zip_zip64_end(void *ctx)
{
	struct wire_zip *z = ctx;
	size_t lowest, e, ws = 0, wn = 0;	/* z->data holds [ws, ws + wn) */
	uint8_t loc[4];

	if (open_zip(z) == NULL || z->len < EOCD_LEN)
		return true;
	lowest = z->len > 0xFFFFu + EOCD_LEN ? z->len - 0xFFFFu - EOCD_LEN : 0;
	for (e = z->len - EOCD_LEN;; e--) {
		const uint8_t *p;

		if (wn == 0 || e < ws) {
			ws = e + 4 - lowest > sizeof(z->data) ?
				     e + 4 - sizeof(z->data) : lowest;
			wn = e + 4 - ws;
			if (!zip_bytes(z, ws, z->data, wn))
				return true;
		}
		p = z->data + (e - ws);
		if (p[0] == 'P' && p[1] == 'K' && p[2] == 5 && p[3] == 6)
			break;
		if (e == lowest)
			return true;
	}
	if (e < LOCATOR_LEN)
		return false;
	if (!zip_bytes(z, e - LOCATOR_LEN, loc, sizeof(loc)))
		return true;
	return loc[0] == 'P' && loc[1] == 'K' && loc[2] == 6 && loc[3] == 7;
}

/* The pieces minizip read a member's data in (its UNZ_BUFSIZE). */
#define ZIP_PIECE 16384u

/* One extraction runs at a time, so every struct wire_zip shares these:
 * inflate()'s output and the pieces of compressed data read from a zip
 * that is not in RAM. */
static _Alignas(4) uint8_t inflate_out[1u << MAX_WBITS];
static _Alignas(4) uint8_t zip_piece[ZIP_PIECE];

/* A member's data on its way to the sink. */
struct member_data {
	const struct wire_zip *z;
	uint64_t pos;			/* the next compressed byte, in the zip */
	uint64_t rest;			/* compressed bytes not yet read */
	uint64_t left;			/* bytes the member states, not yet given */
	s11_sink_fn sink;
	void *arg;
};

/*
 * The next piece of compressed data, as minizip read it: ZIP_PIECE bytes or
 * the rest, and a piece that runs past the zip's end fails. In place when
 * the zip is in RAM. Returns its length, 0 when there is none.
 */
static size_t next_piece(struct member_data *d, const uint8_t **p)
{
	const struct wire_zip *z = d->z;
	const size_t n = d->rest < ZIP_PIECE ? (size_t)d->rest : ZIP_PIECE;

	if (n == 0 || d->pos > z->len || n > z->len - d->pos)
		return 0;
	if (z->buf != NULL) {
		*p = z->buf + d->pos;
	} else {
		if (!zip_bytes(z, (size_t)d->pos, zip_piece, n))
			return 0;
		*p = zip_piece;
	}
	d->pos += n;
	d->rest -= n;
	return n;
}

/* As minizip gave a stored member: its first `left` bytes, read in pieces;
 * compressed bytes beyond them are not looked at. */
static bool extract_stored(struct member_data *d)
{
	while (d->left != 0) {
		const uint8_t *p = NULL;
		size_t n = next_piece(d, &p);

		if (n == 0)
			return false;
		if (n > d->left)
			n = (size_t)d->left;
		d->left -= n;
		if (!d->sink(d->arg, p, n))
			return false;
	}
	return true;
}

/*
 * As minizip read a deflated member, step for step, so that every verdict
 * is its: inflate() with its output capped at the bytes still stated, the
 * next piece read when the input is used up and output is still wanted.
 * Nothing past the stated size is decoded to the sink, and once it is
 * reached nothing more is read; a call that fails fails the member, even
 * one that reached it. A member stated empty is not inflated.
 */
static bool extract_deflated(struct member_data *d)
{
	z_stream s;
	int ret = Z_OK;

	if (d->left == 0)
		return true;
	memset(&s, 0, sizeof(s));
	if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
		return false;
	while (d->left != 0 && ret == Z_OK) {
		size_t got;

		if (s.avail_in == 0 && d->rest != 0) {
			const uint8_t *p = NULL;
			const size_t n = next_piece(d, &p);

			if (n == 0) {
				ret = Z_ERRNO;
				break;
			}
			/* inflate() only reads its input */
			s.next_in = (Bytef *)(uintptr_t)p;
			s.avail_in = (uInt)n;
		}
		s.next_out = inflate_out;
		s.avail_out = d->left < sizeof(inflate_out) ? (uInt)d->left :
							      sizeof(inflate_out);
		ret = inflate(&s, Z_SYNC_FLUSH);
		if (ret >= 0 && s.msg != NULL)
			ret = Z_DATA_ERROR;
		if (ret != Z_OK && ret != Z_STREAM_END)
			break;
		got = (size_t)(s.next_out - inflate_out);
		d->left -= got;
		if (got != 0 && !d->sink(d->arg, inflate_out, got))
			ret = Z_ERRNO;
	}
	(void)inflateEnd(&s);
	return (ret == Z_OK || ret == Z_STREAM_END) && d->left == 0;
}

/*
 * Stored, or inflated by zlib (window 32 KiB), to the sink in pieces of at
 * most 32 KiB. minizip checks the local header and finds the data; its
 * bytes are read here, so minizip computes no CRC-32: the CRC-32 and the
 * length are s11_check_zip()'s to compare.
 */
static bool zip_extract(void *ctx, size_t index, s11_sink_fn sink, void *arg)
{
	struct wire_zip *z = ctx;
	unz_file_info64 info;
	struct member_data d;

	if (!go_to(z, index) ||
	    unzGetCurrentFileInfo64(z->uf, &info, NULL, 0, NULL, 0, NULL, 0) !=
		    UNZ_OK ||
	    unzOpenCurrentFile2(z->uf, NULL, NULL, 1) != UNZ_OK)
		return false;
	d.z = z;
	d.pos = unzGetCurrentFileZStreamPos64(z->uf);
	d.rest = info.compressed_size;
	d.left = info.uncompressed_size;
	d.sink = sink;
	d.arg = arg;
	/* raw: minizip compares no CRC-32 at the close, so UNZ_OK */
	if (unzCloseCurrentFile(z->uf) != UNZ_OK)
		return false;
	if (info.compression_method == 0)
		return extract_stored(&d);
	if (info.compression_method == Z_DEFLATED)
		return extract_deflated(&d);
	return false;
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_free_space(void *ctx, uint64_t *bytes)
{
	const struct wire_zip *z = ctx;

	return wire_yaffs_free_space(z->fs, bytes);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_staging_reset(void *ctx)
{
	const struct wire_zip *z = ctx;

	return wire_yaffs_staging_reset(z->fs);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_staging_mkdir(void *ctx, const char *path)
{
	const struct wire_zip *z = ctx;

	return wire_yaffs_staging_mkdir(z->fs, path);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_staging_open(void *ctx, const char *path)
{
	const struct wire_zip *z = ctx;

	return wire_yaffs_staging_open(z->fs, path);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_staging_write(void *ctx, const uint8_t *p, size_t n)
{
	const struct wire_zip *z = ctx;

	return wire_yaffs_staging_write(z->fs, p, n);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_staging_close(void *ctx)
{
	const struct wire_zip *z = ctx;

	return wire_yaffs_staging_close(z->fs);
}

void wire_zip_init(struct wire_zip *z, struct s11_zip_ops *ops,
		   const struct wire_package *pkg, const uint8_t *buf,
		   size_t len, struct wire_yaffs *fs)
{
	memset(z, 0, sizeof(*z));
	z->pkg = pkg;
	z->buf = buf;
	z->len = len;
	z->fs = fs;
	ops->member = zip_member;
	ops->zip64_end = zip_zip64_end;
	ops->extract = zip_extract;
	ops->crc32 = wire_s11_crc32;
	ops->free_space = zip_free_space;
	ops->staging_reset = zip_staging_reset;
	ops->staging_mkdir = zip_staging_mkdir;
	ops->staging_open = zip_staging_open;
	ops->staging_write = zip_staging_write;
	ops->staging_close = zip_staging_close;
	ops->ctx = z;
}

void wire_zip_close(struct wire_zip *z)
{
	if (z->uf != NULL)
		(void)unzClose(z->uf);
	z->uf = NULL;
	z->opened = false;
	z->at_valid = false;
}
