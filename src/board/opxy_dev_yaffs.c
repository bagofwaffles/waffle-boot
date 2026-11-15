/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The developer requests' raw view of the yaffs2 region (S-12 section 2,
 * waffle/devhost.h): chunk tags, and the first bytes of object headers and
 * block summaries. Read only. A data chunk's bytes are never returned: the
 * meta read answers only for a chunk whose own tags make it a header or a
 * summary.
 */
#include <string.h>

#include "waffle/bytes.h"
#include "waffle/devhost.h"

#define SECTOR 512u
#define SECTORS_PER_CHUNK 8u				/* S-12.041 */
#define REGION_CHUNKS (0x1200u * 128u)			/* S-12.042 */
#define TAGS_IN_SECTOR (SECTOR - OPXY_YTAGS_BYTES)	/* chunk bytes 0xFF0-0xFFF */
#define HEADER_BYTES 0x200u				/* S-12.045 */
#define SUMMARY_ID 0x10u				/* S-12.046, S-12.049 */
#define SUMMARY_BYTES 1540u				/* S-12.049 */

static _Alignas(4) uint8_t sector[2u * SECTOR];

static bool read_tags(const struct opxy_ydump_ops *io, uint32_t chunk,
		      uint8_t *out)
{
	if (io->read(io->ctx, chunk * SECTORS_PER_CHUNK + SECTORS_PER_CHUNK - 1u,
		     1u, sector) != 0)
		return false;
	memcpy(out, sector + TAGS_IN_SECTOR, OPXY_YTAGS_BYTES);
	return true;
}

uint8_t opxy_ydump_tags(const struct opxy_ydump_ops *io, uint32_t first,
			uint32_t stride, uint32_t count, uint8_t *out)
{
	uint32_t i;

	if (count == 0u || count > OPXY_YTAGS_MAX || first >= REGION_CHUNKS ||
	    (count > 1u && (stride == 0u ||
			    (uint64_t)first + (uint64_t)stride * (count - 1u) >=
				    REGION_CHUNKS)))
		return OPXY_DEV_BAD;
	for (i = 0; i < count; i++)
		if (!read_tags(io, first + stride * i, out + OPXY_YTAGS_BYTES * i))
			return OPXY_DEV_REFUSED;
	return OPXY_DEV_OK;
}

uint8_t opxy_ydump_meta(const struct opxy_ydump_ops *io, uint32_t chunk,
			uint32_t off, uint32_t len, uint8_t *out)
{
	uint8_t t[OPXY_YTAGS_BYTES];
	uint32_t limit, s0, s1;

	if (chunk >= REGION_CHUNKS || len == 0u || len > OPXY_YMETA_MAX ||
	    off > SUMMARY_BYTES)
		return OPXY_DEV_BAD;
	if (!read_tags(io, chunk, t))
		return OPXY_DEV_REFUSED;
	/* S-12.043: unused. S-12.044: a header's chunk word has bit 31 set. */
	if (wb_get_le32(t) == 0u)
		return OPXY_DEV_REFUSED;
	if ((wb_get_le32(t + 8) & 0x80000000u) != 0u)
		limit = HEADER_BYTES;
	else if (wb_get_le32(t + 4) == SUMMARY_ID && wb_get_le32(t + 8) == 1u)
		limit = SUMMARY_BYTES;
	else
		return OPXY_DEV_REFUSED;		/* a data chunk */
	if (off + len > limit)
		return OPXY_DEV_REFUSED;
	s0 = off / SECTOR;
	s1 = (off + len - 1u) / SECTOR;
	if (io->read(io->ctx, chunk * SECTORS_PER_CHUNK + s0, s1 - s0 + 1u,
		     sector) != 0)
		return OPXY_DEV_REFUSED;
	memcpy(out, sector + (off - s0 * SECTOR), len);
	return OPXY_DEV_OK;
}
