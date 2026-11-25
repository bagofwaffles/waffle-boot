/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_UTIL_CRC_H
#define WAFFLE_UTIL_CRC_H

/*
 * The CRCs in the forms the S-xx modules take them, from
 * src/glue/crc and zlib's crc32() (src/third_party/README.md).
 */

#include <stddef.h>
#include <stdint.h>

/* CRC-32, zlib's convention (S-02.015, S-03.025): s02_crc32_fn and
 * s03_crc32_fn. */
uint32_t wire_crc32(uint32_t crc, const uint8_t *buf, uint32_t len);

/* The S-06.003 frame check: s06_crc8_fn. */
uint8_t wire_crc8_frame(const uint8_t *data, size_t len);

/* CRC-16/X-25: s07_crc16_fn. S-07.082 swaps its bytes; s07_cy_checksum()
 * does that, not this. */
uint16_t wire_crc16_x25(const uint8_t *data, size_t len);

/* CRC-16/XMODEM (S-08.044): s08_crc16_fn. */
uint16_t wire_crc16_xmodem(const uint8_t *data, size_t len);

/* The S-11 forms, continuing `crc`; ctx is not used: s11_crypto_ops.crc16
 * (S-11.008) and s11_zip_ops.crc32 (S-11.030). */
uint16_t wire_s11_crc16(void *ctx, uint16_t crc, const uint8_t *p, size_t n);
uint32_t wire_s11_crc32(void *ctx, uint32_t crc, const uint8_t *p, size_t n);

#endif /* WAFFLE_UTIL_CRC_H */
