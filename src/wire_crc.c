/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/wire_crc.h"

#include "wb_crc.h"
#include "zlib.h"

uint32_t wire_crc32(uint32_t crc, const uint8_t *buf, uint32_t len)
{
	return (uint32_t)crc32_z(crc, buf, len);
}

uint8_t wire_crc8_frame(const uint8_t *data, size_t len)
{
	return wb_crc8_frame(0, data, len);
}

uint16_t wire_crc16_x25(const uint8_t *data, size_t len)
{
	return wb_crc16_x25(0, data, len);
}

uint16_t wire_crc16_xmodem(const uint8_t *data, size_t len)
{
	return wb_crc16_xmodem(0, data, len);
}

uint16_t wire_s11_crc16(void *ctx, uint16_t crc, const uint8_t *p, size_t n)
{
	(void)ctx;
	return wb_crc16_xmodem(crc, p, n);
}

uint32_t wire_s11_crc32(void *ctx, uint32_t crc, const uint8_t *p, size_t n)
{
	(void)ctx;
	return (uint32_t)crc32_z(crc, p, n);
}
