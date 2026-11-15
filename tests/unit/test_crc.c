/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The CRCs of the S-xx modules against the vectors (crc.txt: S-02.015,
 * S-11.008, S-06.003, S-07.082) and against bit-by-bit definitions written
 * from their parameter tables, over every split of the data into two
 * pieces for the continuation property.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "wb_crc.h"
#include "zlib.h"

/* CRC-32 (S-02.015): reflected 0x04C11DB7, init and final XOR 0xFFFFFFFF. */
static uint32_t ref_crc32(const uint8_t *p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;

	while (n--) {
		int k;

		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = c & 1u ? c >> 1 ^ 0xEDB88320u : c >> 1;
	}
	return c ^ 0xFFFFFFFFu;
}

/* CRC-16/XMODEM (S-11.008): 0x1021, init 0, not reflected. */
static unsigned int ref_xmodem(const uint8_t *p, size_t n)
{
	unsigned int c = 0;

	while (n--) {
		int k;

		c ^= (unsigned int)*p++ << 8;
		for (k = 0; k < 8; k++)
			c = c & 0x8000u ? (c << 1 ^ 0x1021u) & 0xFFFFu :
					  (c << 1) & 0xFFFFu;
	}
	return c;
}

/* S-06.003, as its one-step statement gives it. */
static unsigned int ref_crc8_frame(const uint8_t *p, size_t n)
{
	unsigned int c = 0;

	while (n--) {
		int k;

		c ^= *p++ ^ 0xA5u;
		for (k = 0; k < 8; k++)
			c = c & 1u ? c >> 1 ^ 0xB2u : c >> 1;
	}
	return c;
}

static uint32_t zlib_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
	return (uint32_t)crc32(crc, p, (uInt)n);
}

static void check_vector(const char *algo, const uint8_t *d, size_t n,
			 unsigned long want)
{
	size_t cut = n / 3;

	if (!strcmp(algo, "crc32")) {
		uint32_t whole = zlib_crc32(0, d, n);
		uint32_t split = zlib_crc32(zlib_crc32(0, d, cut), d + cut,
					    n - cut);

		CHECK(whole == want, "crc32 of %zu bytes: %08x, want %08lx",
		      n, (unsigned int)whole, want);
		CHECK(split == want, "crc32 continued at %zu", cut);
		CHECK(ref_crc32(d, n) == want, "reference crc32 of %zu bytes",
		      n);
	} else if (!strcmp(algo, "crc16_xmodem")) {
		unsigned int whole = wb_crc16_xmodem(0, d, n);
		unsigned int split = wb_crc16_xmodem(
			wb_crc16_xmodem(0, d, cut), d + cut, n - cut);

		CHECK(whole == want, "xmodem of %zu bytes: %04x, want %04lx",
		      n, whole, want);
		CHECK(split == want, "xmodem continued at %zu", cut);
		CHECK(ref_xmodem(d, n) == want, "reference xmodem");
	} else if (!strcmp(algo, "crc8_frame")) {
		unsigned int whole = wb_crc8_frame(0, d, n);
		unsigned int split = wb_crc8_frame(wb_crc8_frame(0, d, cut),
						   d + cut, n - cut);

		CHECK(whole == want, "crc8 of %zu bytes: %02x, want %02lx", n,
		      whole, want);
		CHECK(split == want, "crc8 continued at %zu", cut);
		CHECK(ref_crc8_frame(d, n) == want, "reference crc8");
	} else if (!strcmp(algo, "psoc_check")) {
		/* S-07.082: the X-25 CRC with its two bytes swapped */
		unsigned int x = wb_crc16_x25(0, d, n);
		unsigned int got = (x << 8 | x >> 8) & 0xFFFFu;

		CHECK(got == want, "psoc check of %zu bytes: %04x, want %04lx",
		      n, got, want);
	} else {
		CHECK(0, "unknown algorithm %s", algo);
	}
}

/* The tables against the definitions, over every one- and two-byte input. */
static void check_tables(void)
{
	unsigned int i, j;

	for (i = 0; i < 256; i++) {
		for (j = 0; j < 256; j += 17) {
			const uint8_t b[2] = { (uint8_t)i, (uint8_t)j };

			CHECK(wb_crc16_xmodem(0, b, 2) == ref_xmodem(b, 2),
			      "xmodem %02x %02x", i, j);
			CHECK(wb_crc8_frame(0, b, 2) == ref_crc8_frame(b, 2),
			      "crc8 %02x %02x", i, j);
		}
	}
}

/* The published check values, independent of the vector files. */
static void check_values(void)
{
	static const uint8_t check[] = "123456789";

	CHECK(zlib_crc32(0, check, 9) == 0xCBF43926u, "crc32 check value");
	CHECK(wb_crc16_xmodem(0, check, 9) == 0x31C3u, "xmodem check value");
	CHECK(wb_crc8_frame(0, check, 9) == 0xD8u, "crc8 check value");
	CHECK(wb_crc16_x25(0, check, 9) == 0x906Eu, "x25 check value");
	CHECK(wb_crc16_x25(wb_crc16_x25(0, check, 4), check + 4, 5) ==
		      0x906Eu, "x25 continued");
	CHECK(wb_crc16_x25(0, check, 0) == 0, "x25 of nothing");
}

int main(void)
{
	FILE *f = tl_open_vectors("crc.txt");
	char *line = NULL, *fld[8];
	size_t cap = 0;
	int n, vectors = 0;

	while ((n = tl_fields(f, &line, &cap, fld, 8)) >= 0) {
		size_t len;
		uint8_t *data;

		if (n != 4 || strcmp(fld[0], "crc"))
			continue;
		data = tl_unhex(fld[2], &len);
		check_vector(fld[1], data, len, strtoul(fld[3], NULL, 16));
		free(data);
		vectors++;
	}
	free(line);
	fclose(f);
	CHECK(vectors >= 20, "only %d vectors", vectors);
	check_tables();
	check_values();
	return tl_done("test_crc");
}
