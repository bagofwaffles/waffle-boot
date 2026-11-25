/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The byte and string helpers of waffle/util/bytes.h: values, byte order, every
 * offset of a buffer (aligned or not), the bytes around a write left alone,
 * and the S-06 names giving the same results.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "waffle/util/bytes.h"
#include "waffle/link/bytes_compat.h"

#define GUARD 0xA5u

static void check_reads(void)
{
	static const uint8_t src[16] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87,
	};

	CHECK(wb_get_le16(src) == 0x0201u, "le16");
	CHECK(wb_get_be16(src) == 0x0102u, "be16");
	CHECK(wb_get_le32(src) == 0x04030201u, "le32");
	CHECK(wb_get_be32(src) == 0x01020304u, "be32");
	CHECK(wb_get_le16(src + 8) == 0xE1F0u, "le16 high bits");
	CHECK(wb_get_be16(src + 8) == 0xF0E1u, "be16 high bits");
	CHECK(wb_get_le32(src + 8) == 0xC3D2E1F0u, "le32 high bits");
	CHECK(wb_get_be32(src + 8) == 0xF0E1D2C3u, "be32 high bits");

	for (size_t off = 0; off + 4 <= sizeof(src); off++) {
		const uint8_t *p = src + off;
		uint32_t le = 0, be = 0;

		for (int i = 3; i >= 0; i--)
			le = le << 8 | p[i];
		for (int i = 0; i < 4; i++)
			be = be << 8 | p[i];
		CHECK(wb_get_le32(p) == le, "le32 at offset %zu", off);
		CHECK(wb_get_be32(p) == be, "be32 at offset %zu", off);
		CHECK(wb_get_le16(p) == (uint16_t)le, "le16 at offset %zu", off);
		CHECK(wb_get_be16(p) == (uint16_t)(be >> 16), "be16 at offset %zu", off);
		CHECK(s06_rd_le32(p) == le && s06_rd_le16(p) == wb_get_le16(p) &&
		      s06_rd_be16(p) == wb_get_be16(p), "s06 reads at offset %zu", off);
	}
}

/* buf[off..off+n) holds want, and every other byte is still GUARD. */
static int only(const uint8_t *buf, size_t size, size_t off, const uint8_t *want, size_t n)
{
	for (size_t i = 0; i < size; i++) {
		uint8_t b = i >= off && i < off + n ? want[i - off] : GUARD;

		if (buf[i] != b)
			return 0;
	}
	return 1;
}

static void check_writes(void)
{
	static const uint8_t le16[] = { 0xCD, 0xAB };
	static const uint8_t be16[] = { 0xAB, 0xCD };
	static const uint8_t le32[] = { 0x78, 0x56, 0x34, 0x12 };
	static const uint8_t be32[] = { 0x12, 0x34, 0x56, 0x78 };
	static const uint8_t le64[] = { 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01 };
	uint8_t buf[24];

	for (size_t off = 0; off < 8; off++) {
		memset(buf, GUARD, sizeof(buf));
		wb_put_le16(buf + off, 0xABCDu);
		CHECK(only(buf, sizeof(buf), off, le16, 2), "put le16 at offset %zu", off);
		memset(buf, GUARD, sizeof(buf));
		wb_put_be16(buf + off, 0xABCDu);
		CHECK(only(buf, sizeof(buf), off, be16, 2), "put be16 at offset %zu", off);
		memset(buf, GUARD, sizeof(buf));
		wb_put_le32(buf + off, 0x12345678u);
		CHECK(only(buf, sizeof(buf), off, le32, 4), "put le32 at offset %zu", off);
		memset(buf, GUARD, sizeof(buf));
		wb_put_be32(buf + off, 0x12345678u);
		CHECK(only(buf, sizeof(buf), off, be32, 4), "put be32 at offset %zu", off);
		memset(buf, GUARD, sizeof(buf));
		wb_put_le64(buf + off, 0x0123456789ABCDEFull);
		CHECK(only(buf, sizeof(buf), off, le64, 8), "put le64 at offset %zu", off);

		memset(buf, GUARD, sizeof(buf));
		s06_wr_le16(buf + off, 0xABCDu);
		CHECK(only(buf, sizeof(buf), off, le16, 2), "s06 le16 at offset %zu", off);
		memset(buf, GUARD, sizeof(buf));
		s06_wr_be16(buf + off, 0xABCDu);
		CHECK(only(buf, sizeof(buf), off, be16, 2), "s06 be16 at offset %zu", off);
		memset(buf, GUARD, sizeof(buf));
		s06_wr_le32(buf + off, 0x12345678u);
		CHECK(only(buf, sizeof(buf), off, le32, 4), "s06 le32 at offset %zu", off);
		memset(buf, GUARD, sizeof(buf));
		s06_wr_be32(buf + off, 0x12345678u);
		CHECK(only(buf, sizeof(buf), off, be32, 4), "s06 be32 at offset %zu", off);
	}

	/* A write read back, at the extremes of each width. */
	static const uint32_t v32[] = { 0u, 1u, 0x80u, 0xFFu, 0x8000u, 0x80000000u, 0xFFFFFFFFu };

	for (size_t i = 0; i < sizeof(v32) / sizeof(v32[0]); i++) {
		wb_put_le32(buf + 3, v32[i]);
		CHECK(wb_get_le32(buf + 3) == v32[i], "le32 round trip %08x", v32[i]);
		wb_put_be32(buf + 3, v32[i]);
		CHECK(wb_get_be32(buf + 3) == v32[i], "be32 round trip %08x", v32[i]);
		wb_put_le16(buf + 1, (uint16_t)v32[i]);
		CHECK(wb_get_le16(buf + 1) == (uint16_t)v32[i], "le16 round trip %08x", v32[i]);
		wb_put_be16(buf + 1, (uint16_t)v32[i]);
		CHECK(wb_get_be16(buf + 1) == (uint16_t)v32[i], "be16 round trip %08x", v32[i]);
	}
}

static void check_blocks(void)
{
	static const uint8_t src[7] = { 1, 2, 3, 4, 5, 6, 7 };
	static const uint8_t fives[3] = { 5, 5, 5 };
	uint8_t buf[16];

	memset(buf, GUARD, sizeof(buf));
	wb_copy(buf + 3, src, sizeof(src));
	CHECK(only(buf, sizeof(buf), 3, src, sizeof(src)), "copy");
	wb_copy(buf + 3, src, 0);
	CHECK(only(buf, sizeof(buf), 3, src, sizeof(src)), "copy of nothing");
	memset(buf, GUARD, sizeof(buf));
	wb_fill(buf + 5, 5, 3);
	CHECK(only(buf, sizeof(buf), 5, fives, 3), "fill");
	memset(buf, GUARD, sizeof(buf));
	s06_copy(buf + 1, src, 7);
	s06_fill(buf + 1, 5, 3);
	CHECK(buf[0] == GUARD && buf[1] == 5 && buf[3] == 5 && buf[4] == 4 && buf[7] == 7 &&
	      buf[8] == GUARD, "s06 copy and fill");

	CHECK(wb_equal(src, src, sizeof(src)) == 1, "equal");
	CHECK(wb_equal(src, fives, 0) == 1, "equal over nothing");
	CHECK(wb_equal(src + 4, fives, 1) == 1, "equal one byte");
	CHECK(wb_equal(src + 4, fives, 2) == 0, "unequal second byte");
	CHECK(s06_equal(src, fives, 1) == 0, "s06 unequal");
}

static void check_strings(void)
{
	static const char nine[] = "123456789";
	static const char raw[4] = { 'a', 'b', 'c', 'd' };

	CHECK(wb_strlen("") == 0, "strlen empty");
	CHECK(wb_strlen(nine) == 9, "strlen");
	CHECK(wb_strlen(nine + 3) == 6, "strlen from an offset");
	CHECK(wb_strnlen(nine, 20) == 9, "strnlen short of max");
	CHECK(wb_strnlen(nine, 9) == 9, "strnlen at max");
	CHECK(wb_strnlen(nine, 4) == 4, "strnlen cut at max");
	CHECK(wb_strnlen(nine, 0) == 0, "strnlen max 0");
	CHECK(wb_strnlen(raw, sizeof(raw)) == 4, "strnlen with no terminator");
	CHECK(wb_strnlen("", 5) == 0, "strnlen empty");

	CHECK(wb_streq("", ""), "streq empty");
	CHECK(wb_streq("abc", "abc"), "streq");
	CHECK(!wb_streq("abc", "abd"), "streq last differs");
	CHECK(!wb_streq("abc", "ab"), "streq longer");
	CHECK(!wb_streq("ab", "abc"), "streq shorter");
	CHECK(!wb_streq("", "a"), "streq empty against one");
	CHECK(!wb_streq("\xC3\xA9", "\xC3\xA8"), "streq high bytes");
}

int main(void)
{
	check_reads();
	check_writes();
	check_blocks();
	check_strings();
	return tl_done("bytes");
}
