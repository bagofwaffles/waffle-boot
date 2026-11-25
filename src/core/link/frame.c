/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-06 frame codec: raw frame, check byte (through the caller's CRC) and
 * COBS encoding.
 */
#include "waffle/link/frame.h"
#include "waffle/util/bytes.h"

/* S-06.004 */
size_t s06_cobs_encode(const uint8_t *in, size_t n, uint8_t *out)
{
	size_t code_at = 0, o = 1;
	uint8_t code = 1;

	for (size_t i = 0; i < n; i++) {
		if (in[i] == 0) {
			out[code_at] = code;
			code_at = o++;
			code = 1;
			continue;
		}
		out[o++] = in[i];
		code++;
		if (code == 0xFF) {
			out[code_at] = code;
			/* S-06.004: no empty final group after an FF group */
			if (i + 1 == n)
				return o;
			code_at = o++;
			code = 1;
		}
	}
	out[code_at] = code;
	return o;
}

/* S-06.001, S-06.002, S-06.003 */
size_t s06_frame_raw(uint8_t route, uint8_t seq, uint8_t id,
		     const uint8_t *payload, size_t len, s06_crc8_fn crc,
		     uint8_t *raw)
{
	if (len > S06_MAX_PAYLOAD)
		return 0;
	raw[0] = route;
	raw[1] = seq;
	raw[2] = id;
	raw[3] = (uint8_t)len;
	if (len)
		wb_copy(raw + S06_HEADER, payload, len);
	raw[S06_HEADER + len] = crc(raw, S06_HEADER + len);
	return S06_HEADER + len + 1;
}

/* S-06.004, S-06.005, S-06.006 */
size_t s06_frame_encode(uint8_t route, uint8_t seq, uint8_t id,
			const uint8_t *payload, size_t len, s06_crc8_fn crc,
			uint8_t *out)
{
	uint8_t raw[S06_MAX_RAW];
	size_t n = s06_frame_raw(route, seq, id, payload, len, crc, raw);
	size_t e;

	if (n == 0)
		return 0;
	e = s06_cobs_encode(raw, n, out);
	out[e] = 0x00;
	return e + 1;
}
