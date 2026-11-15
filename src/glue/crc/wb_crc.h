/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The CRCs of the S-xx modules other than CRC-32. CRC-32 (S-02.015,
 * S-03.025, the zip members of S-11.030) is zlib's crc32().
 *
 * Every function continues a CRC: pass 0 for the first piece of data and
 * the previous result for the next, so that a CRC over several pieces is
 * the CRC of their concatenation. The result for no data is 0.
 */
#ifndef WAFFLE_WB_CRC_H
#define WAFFLE_WB_CRC_H

#include <stddef.h>
#include <stdint.h>

/*
 * CRC-16/XMODEM: polynomial 0x1021, initial value 0, not reflected, no
 * final XOR; "123456789" gives 0x31C3. The TE header's CRC of a package
 * (S-11.008) and of a Nordic image (S-08).
 */
uint16_t wb_crc16_xmodem(uint16_t crc, const void *buf, size_t len);

/*
 * The S-06 frame check (S-06.003): CRC-8, polynomial 0x4D processed least
 * significant bit first (0xB2 reflected), initial value 0, every input
 * byte XORed with 0xA5, no final XOR; "123456789" gives 0xD8.
 */
uint8_t wb_crc8_frame(uint8_t crc, const void *buf, size_t len);

/*
 * CRC-16/X-25 (also IBM-SDLC): polynomial 0x1021 reflected (0x8408),
 * initial value 0xFFFF, final XOR 0xFFFF; "123456789" gives 0x906E.
 */
uint16_t wb_crc16_x25(uint16_t crc, const void *buf, size_t len);

#endif /* WAFFLE_WB_CRC_H */
