# SPDX-License-Identifier: GPL-2.0-or-later
"""Checksums used by the on-disk formats.

- CRC-32: the ISO-HDLC / zlib CRC-32 (reflected polynomial 0xEDB88320, initial value and final XOR
  0xFFFFFFFF), as `zlib.crc32` computes it.
- CRC-16: CRC-16/XMODEM (polynomial 0x1021, not reflected, initial value 0, no final XOR).
"""

import zlib


def crc32(data, value=0):
    """CRC-32 (zlib) of `data`, continuing from `value`."""
    return zlib.crc32(bytes(data), value) & 0xFFFFFFFF


def _crc16_table():
    t = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else (c << 1)
        t.append(c & 0xFFFF)
    return t


_T16 = _crc16_table()


def crc16_xmodem(data, value=0):
    """CRC-16/XMODEM of `data`, continuing from `value`."""
    c = value & 0xFFFF
    for b in bytes(data):
        c = ((c << 8) & 0xFFFF) ^ _T16[((c >> 8) ^ b) & 0xFF]
    return c
