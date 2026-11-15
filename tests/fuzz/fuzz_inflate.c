/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * zlib's inflate on untrusted streams: the first input byte chooses raw
 * deflate (as in a zip member), the zlib wrapper or gzip; the rest is the
 * stream, inflated in pieces up to 4 MiB of output.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "zlib.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_OUT (4u << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const int wbits[3] = { -15, 15, 31 };
	static uint8_t out[65536];
	unsigned long total = 0;
	z_stream zs;
	int ret;

	if (size < 1)
		return 0;
	memset(&zs, 0, sizeof(zs));
	if (inflateInit2(&zs, wbits[data[0] % 3]) != Z_OK)
		return 0;
	zs.next_in = (Bytef *)(uintptr_t)(data + 1);
	zs.avail_in = (uInt)(size - 1);
	do {
		zs.next_out = out;
		zs.avail_out = (uInt)(data[0] & 0x80 ? 7 : sizeof(out));
		ret = inflate(&zs, Z_NO_FLUSH);
		total += (unsigned long)(zs.next_out - out);
	} while (ret == Z_OK && total < MAX_OUT &&
		 (zs.avail_in || zs.avail_out == 0));
	inflateEnd(&zs);
	return 0;
}
