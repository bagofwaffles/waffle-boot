# SPDX-License-Identifier: GPL-2.0-or-later
"""Consistent Overhead Byte Stuffing (Cheshire and Baker, 1999).

A group is a code byte ``c`` (1..255) and ``c - 1`` non-zero data bytes; every group but a 0xFF group
and the last stands for its data followed by one zero byte. When the input ends right after a full
254-byte group, the encoder writes no empty final group (0x01) unless asked to.
"""

DELIMITER = 0x00


class CobsError(ValueError):
    """Malformed encoded data. ``reason`` is one of 'empty', 'zero-in-data', 'truncated-group'."""

    def __init__(self, reason, detail=''):
        super().__init__(f'{reason}: {detail}' if detail else reason)
        self.reason = reason


def encode(data, trailing_group_after_ff=False):
    """COBS-encode ``data``; the result holds no zero byte and no delimiter."""
    out = bytearray()
    group = bytearray()
    ended_on_ff = False
    for b in data:
        ended_on_ff = False
        if b == 0:
            out.append(len(group) + 1)
            out += group
            group.clear()
            continue
        group.append(b)
        if len(group) == 254:
            out.append(0xFF)
            out += group
            group.clear()
            ended_on_ff = True
    if not ended_on_ff or trailing_group_after_ff:
        out.append(len(group) + 1)
        out += group
    return bytes(out)


def decode(enc):
    """Decode one encoded frame (without its delimiter). Raises CobsError."""
    if not enc:
        raise CobsError('empty')
    out = bytearray()
    i, n = 0, len(enc)
    while i < n:
        c = enc[i]
        if c == 0:
            raise CobsError('zero-in-data', f'offset {i}')
        if i + c > n:
            raise CobsError('truncated-group', f'code {c:#x} at offset {i}, {n - i - 1} bytes left')
        seg = enc[i + 1:i + c]
        if 0 in seg:
            raise CobsError('zero-in-data', f'offset {i + 1 + seg.index(0)}')
        out += seg
        i += c
        if i < n and c != 0xFF:
            out.append(0)
    return bytes(out)


def split_stream(stream):
    """Split a byte stream at delimiters: (complete encoded frames, trailing bytes with no delimiter yet).
    Empty pieces (back-to-back delimiters) are kept as b'' so a caller can see them."""
    parts = bytes(stream).split(b'\x00')
    return parts[:-1], parts[-1]
