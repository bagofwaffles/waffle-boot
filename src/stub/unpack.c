/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The packed block's check and inflate (see unpack.h). */
#include "unpack.h"

#include <stddef.h>

/* next_in as const: a type only, the library as built either way */
#define ZLIB_CONST
#include "zlib.h"

static uint32_t le16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static const struct {
	uint32_t lo, hi;
} writable[] = {
	{ 0x0803C000u, 0x0803DFD4u },
	{ WB_IMAGE_DDR_START, WB_IMAGE_DDR_END },
};

bool wb_unpack_writable(uint32_t addr, uint32_t len)
{
	const uint64_t end = (uint64_t)addr + len;
	size_t i;

	for (i = 0; i < sizeof writable / sizeof writable[0]; i++)
		if (addr >= writable[i].lo && end <= writable[i].hi)
			return true;
	return false;
}

static uint32_t crc(uint32_t c, const uint8_t *p, uint32_t n)
{
	return (uint32_t)crc32_z(c, p, n);
}

/* inflate's allocations, from the arena: its state only, once per run (a
 * window would be asked for only by a stream that does not end in its
 * run, which the arena has no room for and which fails anyway). */
struct arena {
	uint8_t *base;
	uint32_t len, used;
};

static voidpf take(voidpf opaque, uInt items, uInt size)
{
	struct arena *a = opaque;
	const uint32_t room = a->len - a->used;
	uint32_t n;

	if (size == 0u || items > room / size)
		return Z_NULL;
	n = ((uint32_t)items * (uint32_t)size + 7u) & ~7u;
	if (n > room)
		return Z_NULL;
	a->used += n;
	return a->base + a->used - n;
}

static void give(voidpf opaque, voidpf p)
{
	(void)opaque;
	(void)p;
}

/* One run's stream inflated whole into out, every input byte used. */
static int inflate_run(struct arena *a, const uint8_t *in, uint32_t zlen, uint8_t *out,
		       uint32_t len)
{
	z_stream z;
	int rc;

	a->used = 0;
	z.zalloc = take;
	z.zfree = give;
	z.opaque = a;
	z.next_in = in;
	z.avail_in = zlen;
	if (inflateInit2(&z, -15) != Z_OK)
		return -1;
	z.next_out = out;
	z.avail_out = len;
	rc = inflate(&z, Z_FINISH);
	(void)inflateEnd(&z);
	return rc == Z_STREAM_END && z.avail_in == 0u && z.avail_out == 0u ? 0 : -1;
}

enum wb_unpack_rc wb_unpack(const uint8_t *pk, uint32_t avail, const struct wb_unpack_ops *ops,
			    uint32_t *entry)
{
	struct arena a = { ops->arena, ops->arena_len, 0 };
	uint8_t *dst[WB_PACK_RUNS];
	uint32_t head, n, size, i, off, out_len = 0, c = 0;
	uint64_t prev_end = 0;

	if (pk == NULL || avail < WB_PACK_HEAD(1u) || le32(pk + WB_PACK_O_MAGIC) != WB_PACK_MAGIC ||
	    le16(pk + WB_PACK_O_VERSION) != WB_PACK_VERSION)
		return WB_UNPACK_HEAD;
	head = le16(pk + WB_PACK_O_HEAD);
	n = le32(pk + WB_PACK_O_NRUNS);
	if (n == 0u || n > WB_PACK_RUNS || head != WB_PACK_HEAD(n) || head > avail ||
	    crc(0, pk, head - 4u) != le32(pk + head - 4u))
		return WB_UNPACK_HEAD;
	size = le32(pk + WB_PACK_O_SIZE);
	if (size < head || size > avail)
		return WB_UNPACK_SIZE;
	if (crc(0, pk + head, size - head) != le32(pk + WB_PACK_O_ZCRC))
		return WB_UNPACK_STREAMS_CRC;
	/* every run's place and stream before the first write */
	off = head;
	for (i = 0; i < n; i++) {
		const uint8_t *r = pk + WB_PACK_O_RUNS + 12u * i;
		const uint32_t addr = le32(r), len = le32(r + 4), zlen = le32(r + 8);

		if (len == 0u || (uint64_t)addr < prev_end || (uint64_t)addr + len > 0x100000000ull ||
		    zlen > size - off)
			return WB_UNPACK_SIZE;
		dst[i] = ops->dest(ops->ctx, addr, len);
		if (dst[i] == NULL)
			return WB_UNPACK_DEST;
		prev_end = (uint64_t)addr + len;
		out_len += len;
		off += zlen;
	}
	if (off != size || out_len != le32(pk + WB_PACK_O_OUTLEN))
		return WB_UNPACK_SIZE;
	off = head;
	for (i = 0; i < n; i++) {
		const uint8_t *r = pk + WB_PACK_O_RUNS + 12u * i;
		const uint32_t len = le32(r + 4), zlen = le32(r + 8);

		if (inflate_run(&a, pk + off, zlen, dst[i], len) != 0)
			return WB_UNPACK_INFLATE;
		off += zlen;
	}
	/* what was written, read back */
	for (i = 0; i < n; i++)
		c = crc(c, dst[i], le32(pk + WB_PACK_O_RUNS + 12u * i + 4u));
	if (c != le32(pk + WB_PACK_O_OUTCRC))
		return WB_UNPACK_OUT_CRC;
	*entry = le32(pk + WB_PACK_O_ENTRY);
	return WB_UNPACK_OK;
}
