/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's CRC-32 and SHA-256, its image descriptor and the loaded
 * image's check (src/front/fr_image.c), and the hand-off record's seal
 * as the full bootloader reads it.
 */
#include <string.h>

#include "support/check.h"
#include "fr_map_rule.h"

#include "front/fr.h"
#include "front/fr_handoff.h"
#include "front/fr_image.h"
#include "waffle/wire_crc.h"

static void test_crc32(void)
{
	static const uint8_t check[] = "123456789";
	uint8_t buf[300];
	unsigned i;

	CHECK(fr_crc32(0, check, 9) == 0xCBF43926u);	/* S-02.015 */
	CHECK(fr_crc32(0, check, 0) == 0u);
	for (i = 0; i < sizeof buf; i++)
		buf[i] = (uint8_t)(i * 7u + 3u);
	CHECK(fr_crc32(0, buf, sizeof buf) == wire_crc32(0, buf, sizeof buf));
	CHECK(fr_crc32(fr_crc32(0, buf, 100), buf + 100, 200) ==
	      wire_crc32(0, buf, sizeof buf));
}

static void hex(const uint8_t *d, char *out)
{
	static const char x[] = "0123456789abcdef";
	unsigned i;

	for (i = 0; i < FR_SHA256_LEN; i++) {
		out[2 * i] = x[d[i] >> 4];
		out[2 * i + 1] = x[d[i] & 15u];
	}
	out[64] = '\0';
}

static void sha(const char *msg, size_t n, size_t step, char out[65])
{
	struct fr_sha256 s;
	uint8_t d[FR_SHA256_LEN];
	size_t i;

	fr_sha256_init(&s);
	for (i = 0; i < n; i += step)
		fr_sha256_add(&s, (const uint8_t *)msg + i, n - i < step ? n - i : step);
	fr_sha256_end(&s, d);
	hex(d, out);
}

static void test_sha256(void)
{
	static const char two[] =
		"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	char out[65];

	sha("", 0, 1, out);
	CHECK(strcmp(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
	sha("abc", 3, 1, out);
	CHECK(strcmp(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
	sha(two, strlen(two), 5, out);
	CHECK(strcmp(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
	sha(two, strlen(two), 64, out);
	CHECK(strcmp(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
}

/* A fake memory: the descriptor at FR_DESC_ADDR and two regions, main's
 * L1 data and DDR. A request the front must never make fails the test. */
static uint8_t desc_mem[FR_DESC_MAX];
static uint8_t region_a[64], region_b[100];
#define A_ADDR 0x11800000u
#define B_ADDR 0x8803D000u
static unsigned forbidden_maps;

static const uint8_t *map(void *ctx, uint32_t addr, uint32_t len)
{
	(void)ctx;
	if (fr_map_forbidden(addr, len)) {
		forbidden_maps++;
		return NULL;
	}
	if (addr == FR_DESC_ADDR && len <= sizeof desc_mem)
		return desc_mem;
	if (addr == A_ADDR && len <= sizeof region_a)
		return region_a;
	if (addr == B_ADDR && len <= sizeof region_b)
		return region_b;
	return NULL;
}

static size_t build_desc_at(uint32_t crc, uint32_t a_addr)
{
	size_t n = FR_DESC_HEAD + 2u * 8u + 4u;

	memset(desc_mem, 0, sizeof desc_mem);
	fr_put32(desc_mem + FR_DESC_O_MAGIC, FR_DESC_MAGIC);
	fr_put16(desc_mem + FR_DESC_O_VERSION, FR_DESC_VERSION);
	fr_put16(desc_mem + FR_DESC_O_SIZE, (uint16_t)n);
	fr_put32(desc_mem + FR_DESC_O_MAIN_ENTRY, 0x11A00040u);
	fr_put32(desc_mem + FR_DESC_O_NREGIONS, 2u);
	fr_put32(desc_mem + FR_DESC_O_MAIN_CRC, crc);
	fr_put16(desc_mem + FR_DESC_O_BL_VERSION, 1u);
	fr_put32(desc_mem + FR_DESC_O_REGIONS, a_addr);
	fr_put32(desc_mem + FR_DESC_O_REGIONS + 4u, sizeof region_a);
	fr_put32(desc_mem + FR_DESC_O_REGIONS + 8u, B_ADDR);
	fr_put32(desc_mem + FR_DESC_O_REGIONS + 12u, sizeof region_b);
	fr_put32(desc_mem + n - 4u, fr_crc32(0, desc_mem, n - 4u));
	return n;
}

static size_t build_desc(uint32_t crc)
{
	return build_desc_at(crc, A_ADDR);
}

/* The board's map: the backing memory where fr_mem_readable() allows. */
static const uint8_t *board_map(void *ctx, uint32_t addr, uint32_t len)
{
	if (!fr_mem_readable(addr, len))
		return NULL;
	if (addr == 0x11A00000u && len <= sizeof region_a)
		return region_a;	/* reached only if the rule let L1 code through */
	return map(ctx, addr, len);
}

static void test_image_check(void)
{
	const struct fr_mem mem = { map, NULL };
	struct fr_desc d;
	uint32_t crc;
	size_t n;

	memset(region_a, 0x5A, sizeof region_a);
	memset(region_b, 0xA5, sizeof region_b);
	crc = fr_crc32(fr_crc32(0, region_a, sizeof region_a), region_b, sizeof region_b);
	(void)build_desc(crc);
	CHECK(fr_image_check(&mem, &d));
	CHECK(d.main_entry == 0x11A00040u && d.n_regions == 2u);
	CHECK(d.bl_version[0] == 1u);

	/* One byte of the image changed. */
	region_b[50] ^= 1u;
	CHECK(!fr_image_check(&mem, &d));
	region_b[50] ^= 1u;
	CHECK(fr_image_check(&mem, &d));

	/* The descriptor's own CRC, its magic, a region past the memory. */
	desc_mem[FR_DESC_O_MAIN_ENTRY] ^= 1u;
	CHECK(!fr_image_check(&mem, &d));
	n = build_desc(crc);
	desc_mem[FR_DESC_O_MAGIC] ^= 1u;
	fr_put32(desc_mem + n - 4u, fr_crc32(0, desc_mem, n - 4u));
	CHECK(!fr_image_check(&mem, &d));
	n = build_desc(crc);
	fr_put32(desc_mem + FR_DESC_O_REGIONS + 12u, sizeof region_b + 1u);
	fr_put32(desc_mem + n - 4u, fr_crc32(0, desc_mem, n - 4u));
	CHECK(!fr_image_check(&mem, &d));
	/* No descriptor at all: an erased block. */
	memset(desc_mem, 0xFF, sizeof desc_mem);
	CHECK(!fr_image_check(&mem, &d));
	CHECK(forbidden_maps == 0u);
}

/* S-01.023, S-13a.040: a descriptor that lists L1 code fails the check on
 * the board's map without a load from it. */
static void test_l1_code_not_read(void)
{
	const struct fr_mem mem = { board_map, NULL };
	struct fr_desc d;
	uint32_t crc;

	forbidden_maps = 0;
	memset(region_a, 0x5A, sizeof region_a);
	memset(region_b, 0xA5, sizeof region_b);
	crc = fr_crc32(fr_crc32(0, region_a, sizeof region_a), region_b, sizeof region_b);
	(void)build_desc_at(crc, A_ADDR);
	CHECK(fr_image_check(&mem, &d));
	(void)build_desc_at(crc, 0x11A00000u);
	CHECK(!fr_image_check(&mem, &d));
	CHECK(forbidden_maps == 0u);
}

/* fr_mem_readable(), the board's map, against the rule and S-01.023's
 * memory: nothing forbidden, and what the image check needs. */
static void test_mem_readable(void)
{
	static const struct {
		uint32_t lo, hi, step;
	} sweep[] = {
		{ 0x117FF000u, 0x11C00000u, 0x40u },	/* L1 */
		{ 0x08000000u, 0x08040000u, 0x40u },	/* L2 */
		{ 0x88000000u, 0x88500000u, 0x400u },	/* DDR */
	};
	unsigned i, lens[] = { 1u, 4u, 0x100u, 0x1000u };

	for (i = 0; i < ARRAY_LEN(sweep); i++)
		for (uint32_t a = sweep[i].lo; a < sweep[i].hi; a += sweep[i].step)
			for (unsigned j = 0; j < ARRAY_LEN(lens); j++)
				if (fr_mem_readable(a, lens[j]) && fr_map_forbidden(a, lens[j])) {
					CHECK(false);
					return;
				}
	/* What a main image loads. */
	CHECK(fr_mem_readable(FR_DESC_ADDR, FR_DESC_MAX));
	CHECK(fr_mem_readable(0x11800000u, 0x2000u));
	CHECK(fr_mem_readable(0x11900000u, 0x8000u));
	CHECK(fr_mem_readable(0x08031000u, 0x0803DFD4u - 0x08031000u));
	CHECK(fr_mem_readable(0x8803D000u, 0x300000u));
	/* Never L1 code, a hole, the front's own memory, the record, the
	 * staging buffer, nor a range that runs out of a readable one. */
	CHECK(!fr_mem_readable(0x11A00000u, 4u));
	CHECK(!fr_mem_readable(0x11A08000u, 4u));
	CHECK(!fr_mem_readable(0x11808000u, 4u));
	CHECK(!fr_mem_readable(0x11908000u, 4u));
	CHECK(!fr_mem_readable(0x11802000u, 4u));
	CHECK(!fr_mem_readable(0x11807000u, 4u));
	CHECK(!fr_mem_readable(0x08002000u, 4u));
	CHECK(!fr_mem_readable(FR_HANDOFF_ADDR, 4u));
	CHECK(!fr_mem_readable(FR_STAGING_START, 4u));
	CHECK(!fr_mem_readable(0x11801FFCu, 8u));
	CHECK(!fr_mem_readable(0x8833CFFCu, 8u));
	CHECK(!fr_mem_readable(0xFFFFFFFCu, 8u));
}

static void test_handoff_seal(void)
{
	struct fr_handoff ho;

	memset(&ho, 0, sizeof ho);
	ho.strap = 0x0104u;
	fr_handoff_seal(&ho);
	CHECK(ho.magic == FR_HO_MAGIC && ho.version == FR_HO_VERSION);
	CHECK(ho.size == sizeof ho);
	/* The full bootloader's own CRC agrees (opxy_front_take()). */
	CHECK(wire_crc32(0, (const uint8_t *)&ho, offsetof(struct fr_handoff, crc32)) ==
	      ho.crc32);
	/* The layout tools/mkldr.py writes. */
	CHECK(offsetof(struct fr_handoff, stamps) % 8u == 0u);
	CHECK(FR_HANDOFF_ADDR + sizeof ho <= FR_SHARED_END);
	CHECK(FR_DESC_ADDR + FR_DESC_MAX <= FR_HANDOFF_ADDR);
}

int main(void)
{
	RUN(test_crc32);
	RUN(test_sha256);
	RUN(test_image_check);
	RUN(test_l1_code_not_read);
	RUN(test_mem_readable);
	RUN(test_handoff_seal);
	return check_summary("test_front_image");
}
