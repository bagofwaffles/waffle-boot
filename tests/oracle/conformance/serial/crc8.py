# SPDX-License-Identifier: GPL-2.0-or-later
"""The frame check of an S-06 frame: an 8-bit CRC with a per-byte whitening constant.

    width      8
    poly       0x4D, processed LSB first (the reflected form is 0xB2)
    init       0x00
    whiten     0xA5, XORed into every input byte before it enters the register
    xorout     0x00
    coverage   the 4 header bytes and the payload, in wire order (not the check byte itself)

One step is ``crc = R(crc ^ byte ^ WHITEN)``, ``R`` being eight LSB-first shift steps. Equivalent to the
Rocksoft-model CRC-8 with refin/refout, poly 0x4D, init 0xFF, xorout 0xFF (check of "123456789": 0xD8).
"""

POLY_REFLECTED = 0xB2
POLY_NORMAL = 0x4D
INIT = 0x00
WHITEN = 0xA5
XOROUT = 0x00


def _register_step(c):
    for _ in range(8):
        c = (c >> 1) ^ POLY_REFLECTED if c & 1 else c >> 1
    return c


def make_table():
    """The 256-entry lookup table ``t`` with ``crc = t[crc ^ byte]`` (whitening folded in)."""
    return bytes(_register_step(i ^ WHITEN) for i in range(256))


TABLE = make_table()


def crc8_bitwise(data, crc=INIT):
    """The check computed bit by bit from the parameters."""
    for b in data:
        crc = _register_step(crc ^ b ^ WHITEN)
    return crc ^ XOROUT


def crc8(data, crc=INIT):
    """The check of ``data`` (bytes-like), table-driven."""
    t = TABLE
    for b in data:
        crc = t[crc ^ b]
    return crc ^ XOROUT
