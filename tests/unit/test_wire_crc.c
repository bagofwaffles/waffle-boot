/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The CRC adapters of src/core/util/crc.c in the forms the S-xx modules take
 * them, against the vectors (crc.txt: S-02.015, S-11.008,
 * S-06.003, S-07.082): CRC-32 for S-02 and S-03, the S-06 CRC-8, the X-25
 * CRC through S-07's own check (which swaps its bytes), CRC-16/XMODEM for
 * S-08 and S-11, and the continuation of the S-02 and S-11 forms across a
 * split of the data into two pieces: every split of a short vector, and a
 * fixed sample of the splits of a long one (split_cut()).
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "waffle/flash/layout.h"
#include "waffle/emmc/emmc.h"
#include "waffle/link/frame.h"
#include "waffle/psoc/cypress.h"
#include "waffle/nordic/nordic.h"
#include "waffle/install/package.h"
#include "waffle/install/staging.h"
#include "waffle/util/crc.h"

/* The adapters must have the modules' types exactly. */
static const s02_crc32_fn crc_s02 = wire_crc32;
static const s03_crc32_fn crc_s03 = wire_crc32;
static const s06_crc8_fn crc_s06 = wire_crc8_frame;
static const s07_crc16_fn crc_s07 = wire_crc16_x25;
static const s08_crc16_fn crc_s08 = wire_crc16_xmodem;
static const struct s11_crypto_ops crypto = { .crc16 = wire_s11_crc16 };
static const struct s11_zip_ops zip = { .crc32 = wire_s11_crc32 };

/* Every split of a vector up to this long is tried. */
#define SPLIT_ALL 16384u
/* On a longer one: the cuts this close to either end, ... */
#define SPLIT_EDGE 64u
/* ... the powers of two and their neighbors, and every n/SPLIT_STEPS bytes
 * (an odd stride, so the sampled cuts fall on every alignment). */
#define SPLIT_STEPS 127u

static int pow2(size_t x)
{
	return x && !(x & (x - 1));
}

/* Whether the split at cut of n bytes is tried. */
static int split_cut(size_t cut, size_t n)
{
	size_t stride = (n / SPLIT_STEPS) | 1u;

	return n <= SPLIT_ALL || cut < SPLIT_EDGE || n - cut < SPLIT_EDGE ||
	       pow2(cut) || pow2(cut + 1) || pow2(cut - 1) || cut % stride == 0;
}

static void check_crc32(const uint8_t *p, size_t n, unsigned long want)
{
	CHECK(crc_s02(0, p, (uint32_t)n) == want, "S-02 CRC-32 of %zu bytes", n);
	CHECK(crc_s03(0, p, (uint32_t)n) == want, "S-03 CRC-32 of %zu bytes", n);
	CHECK(zip.crc32(NULL, 0, p, n) == want, "S-11 CRC-32 of %zu bytes", n);
	for (size_t cut = 0; cut <= n; cut++) {
		uint32_t a, b;

		if (!split_cut(cut, n))
			continue;
		a = crc_s02(crc_s02(0, p, (uint32_t)cut), p + cut,
			    (uint32_t)(n - cut));
		b = zip.crc32(NULL, zip.crc32(NULL, 0, p, cut), p + cut, n - cut);
		CHECK(a == want && b == want, "CRC-32 split at %zu of %zu", cut,
		      n);
	}
}

static void check_xmodem(const uint8_t *p, size_t n, unsigned long want)
{
	CHECK(crc_s08(p, n) == want, "S-08 CRC-16 of %zu bytes", n);
	for (size_t cut = 0; cut <= n; cut++) {
		uint16_t c;

		if (!split_cut(cut, n))
			continue;
		c = crypto.crc16(NULL, crypto.crc16(NULL, 0, p, cut), p + cut,
				 n - cut);
		CHECK(c == want, "S-11 CRC-16 split at %zu of %zu", cut, n);
	}
}

int main(void)
{
	FILE *f = tl_open_vectors("crc.txt");
	char *line = NULL, *fld[4];
	size_t cap = 0;
	int n, seen[4] = { 0, 0, 0, 0 };

	while ((n = tl_fields(f, &line, &cap, fld, 4)) >= 0) {
		size_t len;
		uint8_t *data;
		unsigned long want;

		if (n != 4 || strcmp(fld[0], "crc"))
			continue;
		data = tl_unhex(fld[2], &len);
		want = strtoul(fld[3], NULL, 16);
		if (!strcmp(fld[1], "crc32")) {
			check_crc32(data, len, want);
			seen[0]++;
		} else if (!strcmp(fld[1], "crc16_xmodem")) {
			check_xmodem(data, len, want);
			seen[1]++;
		} else if (!strcmp(fld[1], "crc8_frame")) {
			CHECK(crc_s06(data, len) == want, "S-06 CRC-8 of %zu bytes",
			      len);
			seen[2]++;
		} else if (!strcmp(fld[1], "psoc_check")) {
			/* S-07.082: the packet's check, S-07's swap included */
			CHECK(s07_cy_checksum(data, len, S07_CY_CRC, crc_s07) ==
				      want, "S-07 check of %zu bytes", len);
			seen[3]++;
		}
		free(data);
	}
	free(line);
	fclose(f);
	/* The check values of the parameter tables. */
	CHECK(wire_crc16_x25((const uint8_t *)"123456789", 9) == 0x906Eu,
	      "X-25 check value");
	CHECK(wire_crc8_frame((const uint8_t *)"123456789", 9) == 0xD8u,
	      "CRC-8 check value");
	CHECK(seen[0] >= 5 && seen[1] >= 3 && seen[2] >= 5 && seen[3] >= 2,
	      "vectors: %d CRC-32, %d CRC-16, %d CRC-8, %d PSoC", seen[0],
	      seen[1], seen[2], seen[3]);
	return tl_done("test_wire_crc");
}
