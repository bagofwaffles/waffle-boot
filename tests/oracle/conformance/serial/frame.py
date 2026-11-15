# SPDX-License-Identifier: GPL-2.0-or-later
"""The S-06 frame: header, payload, check byte, COBS-encoded, one 0x00 delimiter.

Raw (decoded) layout, all single bytes:

    +0  route   source address in the high nibble, destination address in the low nibble
    +1  seq     the sender's sequence number for this link (mod 256)
    +2  id      message id
    +3  len     payload length, 0..249
    +4  payload len bytes
    +4+len crc  crc8 (see crc8.py) of bytes +0 .. +3+len

So a raw frame is 5..254 bytes. On the wire: ``cobs.encode(raw) + b'\\x00'``.
"""
from dataclasses import dataclass, field

from . import cobs
from .crc8 import crc8

MAX_PAYLOAD = 249
HEADER_LEN = 4
MIN_RAW = HEADER_LEN + 1
MAX_RAW = HEADER_LEN + MAX_PAYLOAD + 1     # 254


def route(src, dst):
    if not (0 <= src <= 15 and 0 <= dst <= 15):
        raise ValueError('address nibbles are 0..15')
    return src << 4 | dst


def route_src(r):
    return r >> 4


def route_dst(r):
    return r & 0x0F


def swap_route(r):
    """The route of a reply: source and destination exchanged."""
    return (r & 0x0F) << 4 | r >> 4


@dataclass(frozen=True)
class Frame:
    route: int
    seq: int
    id: int
    payload: bytes = field(default=b'')

    def __post_init__(self):
        object.__setattr__(self, 'payload', bytes(self.payload))
        for name in ('route', 'seq', 'id'):
            v = getattr(self, name)
            if not 0 <= v <= 255:
                raise ValueError(f'{name} {v} out of range')

    @property
    def src(self):
        return self.route >> 4

    @property
    def dst(self):
        return self.route & 0x0F

    def raw(self, bad_crc=False):
        """Header, payload and check byte (not COBS-encoded). ``bad_crc`` flips the check's low bit."""
        if len(self.payload) > MAX_PAYLOAD:
            raise ValueError(f'payload {len(self.payload)} > {MAX_PAYLOAD}')
        body = bytes([self.route, self.seq, self.id, len(self.payload)]) + self.payload
        return body + bytes([crc8(body) ^ (1 if bad_crc else 0)])

    def encode(self, bad_crc=False):
        """The frame as sent: COBS-encoded raw frame plus the 0x00 delimiter."""
        return cobs.encode(self.raw(bad_crc)) + b'\x00'

    def __repr__(self):
        return f'Frame(route={self.route:#04x}, seq={self.seq}, id={self.id:#04x}, payload={self.payload.hex()!r})'


class FrameError(ValueError):
    """A frame a receiver must not deliver. ``reason`` is one of:
    'cobs-empty', 'cobs-zero-in-data', 'cobs-truncated-group', 'short', 'too-long', 'length', 'crc'."""

    def __init__(self, reason, detail=''):
        super().__init__(f'{reason}: {detail}' if detail else reason)
        self.reason = reason


def parse_raw(raw):
    """A decoded raw frame -> Frame. Raises FrameError."""
    raw = bytes(raw)
    if len(raw) < MIN_RAW:
        raise FrameError('short', f'{len(raw)} bytes')
    if len(raw) > MAX_RAW:
        raise FrameError('too-long', f'{len(raw)} bytes')
    n = raw[3]
    if n > MAX_PAYLOAD:
        raise FrameError('too-long', f'len field {n}')
    if len(raw) != n + MIN_RAW:
        raise FrameError('length', f'len field {n}, {len(raw)} raw bytes')
    want = crc8(raw[:-1])
    if raw[-1] != want:
        raise FrameError('crc', f'got {raw[-1]:#04x}, computed {want:#04x}')
    return Frame(raw[0], raw[1], raw[2], raw[4:-1])


def decode(enc):
    """One encoded frame (a trailing delimiter is allowed) -> Frame. Raises FrameError."""
    enc = bytes(enc)
    if enc.endswith(b'\x00'):
        enc = enc[:-1]
    try:
        raw = cobs.decode(enc)
    except cobs.CobsError as e:
        raise FrameError('cobs-' + e.reason, str(e)) from None
    return parse_raw(raw)


def decode_stream(stream, skip=()):
    """Split a byte stream at delimiters and decode each frame: [(Frame or FrameError, encoded bytes)].
    Empty pieces between back-to-back delimiters are skipped; bytes after the last delimiter are
    returned separately as the incomplete tail.

    ``skip``: (start, end) byte offsets of raw-mode periods (S-06.060..S-06.063). Their bytes are not
    framed; a partial frame before a period is discarded."""
    stream = bytes(stream)
    if skip:
        out, pos = [], 0
        for a, b in sorted(skip):
            seg_out, _ = decode_stream(stream[pos:a])
            out += seg_out
            pos = max(pos, b)
        rest, tail = decode_stream(stream[pos:])
        return out + rest, tail
    parts, tail = cobs.split_stream(stream)
    out = []
    for p in parts:
        if not p:
            continue
        try:
            out.append((decode(p), p))
        except FrameError as e:
            out.append((e, p))
    return out, tail
