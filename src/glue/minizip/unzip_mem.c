/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unzip_mem.h"

static voidpf ZCALLBACK mem_open(voidpf opaque, const void *filename, int mode)
{
	struct wb_unzip_mem *m = opaque;

	(void)filename;
	if ((mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) != ZLIB_FILEFUNC_MODE_READ)
		return NULL;
	m->pos = 0;
	return m;
}

static uLong ZCALLBACK mem_read(voidpf opaque, voidpf stream, void *buf,
				uLong size)
{
	struct wb_unzip_mem *m = stream;
	size_t n = m->len - m->pos;

	(void)opaque;
	if (n > size)
		n = size;
	if (m->buf == NULL) {
		if (n != 0 && !m->read(m->ctx, m->base + (uint32_t)m->pos, buf, n))
			return 0;
	} else {
		memcpy(buf, m->buf + m->pos, n);
	}
	m->pos += n;
	return (uLong)n;
}

static uLong ZCALLBACK mem_write(voidpf opaque, voidpf stream, const void *buf,
				 uLong size)
{
	(void)opaque;
	(void)stream;
	(void)buf;
	(void)size;
	return 0;
}

static ZPOS64_T ZCALLBACK mem_tell(voidpf opaque, voidpf stream)
{
	const struct wb_unzip_mem *m = stream;

	(void)opaque;
	return m->pos;
}

static long ZCALLBACK mem_seek(voidpf opaque, voidpf stream, ZPOS64_T offset,
			       int origin)
{
	struct wb_unzip_mem *m = stream;
	ZPOS64_T base;

	(void)opaque;
	switch (origin) {
	case ZLIB_FILEFUNC_SEEK_SET:
		base = 0;
		break;
	case ZLIB_FILEFUNC_SEEK_CUR:
		base = m->pos;
		break;
	case ZLIB_FILEFUNC_SEEK_END:
		base = m->len;
		break;
	default:
		return -1;
	}
	/*
	 * minizip seeks back over a central extra field it has read with a
	 * negative SEEK_CUR offset cast to ZPOS64_T (unz64local_GetCurrentFile-
	 * InfoInternal, "move back"), which stdio's fseeko takes as signed:
	 * an offset above INT64_MAX from the current position is that step
	 * back. Positions before the start are refused.
	 */
	if (origin == ZLIB_FILEFUNC_SEEK_CUR && offset > (ZPOS64_T)INT64_MAX) {
		const ZPOS64_T back = (ZPOS64_T)0 - offset;

		if (back > (ZPOS64_T)m->pos)
			return -1;
		m->pos -= (size_t)back;
		return 0;
	}
	/* Positions past the end are refused, like a wrapped one. */
	if (offset > (ZPOS64_T)m->len - base)
		return -1;
	m->pos = (size_t)(base + offset);
	return 0;
}

static int ZCALLBACK mem_close(voidpf opaque, voidpf stream)
{
	(void)opaque;
	(void)stream;
	return 0;
}

static int ZCALLBACK mem_error(voidpf opaque, voidpf stream)
{
	(void)opaque;
	(void)stream;
	return 0;
}

static unzFile open_def(struct wb_unzip_mem *m)
{
	zlib_filefunc64_def def;

	m->pos = 0;
	def.zopen64_file = mem_open;
	def.zread_file = mem_read;
	def.zwrite_file = mem_write;
	def.ztell64_file = mem_tell;
	def.zseek64_file = mem_seek;
	def.zclose_file = mem_close;
	def.zerror_file = mem_error;
	def.opaque = m;
	return unzOpen2_64("memory", &def);
}

unzFile wb_unzip_open_mem(struct wb_unzip_mem *m, const void *buf, size_t len)
{
	m->buf = buf;
	m->len = len;
	m->read = NULL;
	m->ctx = NULL;
	m->base = 0;
	return open_def(m);
}

unzFile wb_unzip_open_read(struct wb_unzip_mem *m,
			   bool (*read)(void *ctx, uint32_t off, uint8_t *buf,
					size_t n),
			   void *ctx, uint32_t base, size_t len)
{
	m->buf = NULL;
	m->len = len;
	m->read = read;
	m->ctx = ctx;
	m->base = base;
	return open_def(m);
}
