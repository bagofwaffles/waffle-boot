/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-06 frame codec: S-06.001-S-06.006 against tests/oracle/S-06-framing/vectors
 * (crc.json, encode.json).
 */
#include "waffle/link/frame.h"

#include "s06_test.h"
#include "vectors/s06_vectors.h"

/* S-06.003: the reference CRC against crc.json (the CRC is the caller's). */
static void test_crc(void)
{
	for (size_t i = 0; i < s06v_crc_count; i++) {
		t_case = s06v_crc[i].name;
		CHECK_EQ(t_crc8(s06v_crc[i].data, s06v_crc[i].len), s06v_crc[i].crc);
	}
}

/* S-06.001, S-06.004, S-06.005, S-06.006: byte-exact raw and wire bytes. */
static void test_encode(void)
{
	for (size_t i = 0; i < s06v_encode_count; i++) {
		const struct s06v_encode *v = &s06v_encode[i];
		uint8_t raw[S06_MAX_RAW], wire[S06_MAX_WIRE];
		size_t n;

		t_case = v->name;
		n = s06_frame_raw(v->route, v->seq, v->id, v->payload, v->len,
				  t_crc8, raw);
		CHECK_MEM(raw, n, v->raw, v->raw_len);
		n = s06_frame_encode(v->route, v->seq, v->id, v->payload, v->len,
				     t_crc8, wire);
		CHECK_MEM(wire, n, v->wire, v->wire_len);
		/* S-06.004/005: no 00 but the delimiter, 7-256 bytes */
		CHECK(n >= 7 && n <= S06_MAX_WIRE);
		for (size_t k = 0; k + 1 < n; k++)
			CHECK(wire[k] != 0);
		CHECK_EQ(wire[n - 1], 0);
	}
}

/* S-06.002: every length 0-249 encodes to len + 6 bytes; 250 is refused. */
static void test_lengths(void)
{
	uint8_t p[S06_MAX_PAYLOAD + 1] = { 0 }, wire[S06_MAX_WIRE];
	uint32_t seed = 6;

	t_case = "lengths";
	for (size_t len = 0; len <= S06_MAX_PAYLOAD; len++) {
		for (size_t i = 0; i < len; i++)
			p[i] = (uint8_t)(t_rand(&seed) & 3u ? t_rand(&seed) : 0);
		CHECK_EQ(s06_frame_encode(0x14, (uint8_t)len, 0x40, p, len,
					  t_crc8, wire), len + 7);
	}
	CHECK_EQ(s06_frame_encode(0x14, 0, 0x40, p, S06_MAX_PAYLOAD + 1, t_crc8,
				  wire), 0);
}

/* S-06.004: the FF group with no final code (a 254-byte frame, no 00). */
static void test_ff_group(void)
{
	uint8_t raw[254], out[256];

	t_case = "ff-group";
	for (size_t i = 0; i < sizeof raw; i++)
		raw[i] = (uint8_t)(i % 255u + 1u);
	CHECK_EQ(s06_cobs_encode(raw, sizeof raw, out), 255);
	CHECK_EQ(out[0], 0xFF);
	CHECK_MEM(out + 1, 254, raw, 254);
	/* 254 non-zero bytes then a zero: FF group, then 01 for the zero */
	raw[253] = 0;
	CHECK_EQ(s06_cobs_encode(raw, sizeof raw, out), 255);
	CHECK_EQ(out[0], 254);
}

int main(void)
{
	test_crc();
	test_encode();
	test_lengths();
	test_ff_group();
	return t_finish("test_s06_frame");
}
