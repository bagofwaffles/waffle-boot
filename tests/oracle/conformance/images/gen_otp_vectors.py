#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes vectors/otp_items.json (S-16.095): OTP items as 32-bit words for units built from the test
keys (tfw.test_key_set), with the serial, keys and provisioned flag expected from them.

Run: python3 -I tests/oracle/conformance/images/gen_otp_vectors.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from images import p224, tfw  # noqa: E402

OUT = os.path.join(HERE, "vectors", "otp_items.json")


def words(b):
    """The little-endian 32-bit words of b, zero-padded."""
    b = bytes(b) + bytes(-len(b) % 4)
    return ["0x%08x" % int.from_bytes(b[i:i + 4], "little") for i in range(0, len(b), 4)]


def point(pub):
    return {"x": "%056x" % pub[0], "y": "%056x" % pub[1]}


def unit(name, note, serial_item, gp1_item, tail9=b"", tailA=b"", keys=None):
    ks = keys or tfw.test_key_set()
    q1 = ks.sign_for(tfw.SIG_DEVICE)[1]
    items = {
        "0x1": words(serial_item),
        "0x3": words(ks.k0[:16]), "0x4": words(ks.k0[16:]),
        "0x5": words(ks.k1[:16]), "0x6": words(ks.k1[16:]),
        "0x9": words(p224.pub_bytes(ks.q0) + tail9),
        "0xa": words(p224.pub_bytes(q1) + tailA),
        "0xe": words(gp1_item),
    }
    serial = serial_item[:18].split(b"\0", 1)[0]
    return {
        "name": name, "note": note, "items": items,
        "expect": {
            "serial_hex": serial.hex(),
            "k0": ks.k0.hex(), "k1": ks.k1.hex(),
            "q0": point(ks.q0), "q1": point(q1),
            "provisioned": sum(1 for x in gp1_item[:28] if x) > 14,
        },
    }


def vectors():
    gp1_15 = bytes([0x5A] * 15 + [0] * 17)
    gp1_14 = bytes([0x5A] * 14 + [0] * 18)
    return {
        "requirement": "S-16.095",
        "units": [
            unit("slots_64", "items 9 and 0xA are 16 words; the key is the first 0x38 bytes and the last 8 are "
                 "nonzero filler", b"TESTSERIAL00000001".ljust(32, b"\0"), gp1_15,
                 tail9=bytes(range(0xA0, 0xA8)), tailA=bytes(range(0xB0, 0xB8))),
            unit("slots_56", "items 9 and 0xA are exactly 14 words", b"TESTSERIAL00000002".ljust(32, b"\0"),
                 gp1_14),
            unit("serial_full", "item 1 has no 00 byte in its first 18; bytes 18 on are not part of the serial",
                 b"TESTSERIAL00000003" + b"EXTRA-BYTES-XX", gp1_15, tail9=b"\xff" * 8),
            unit("serial_short", "item 1's serial ends at its first 00 byte", b"SHORT".ljust(32, b"\0"), gp1_15,
                 tail9=b"\x01" * 8),
        ],
    }


def text():
    return json.dumps(vectors(), indent=1) + "\n"


if __name__ == "__main__":
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text())
