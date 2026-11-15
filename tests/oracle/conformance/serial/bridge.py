# SPDX-License-Identifier: GPL-2.0-or-later
"""Records of the master-slave SPI bridge, and the serial tunnel it carries.

Record framing only. All words are 32-bit little-endian.

Request (master -> slave), one SPI transfer:

    +0  u32  cmd | slot << 8 | len << 16     cmd 8 bits, slot 8 bits (0..3), len 16 bits
    +4  u32  arg0
    +8  u32  arg1
    +12      payload, zero-padded to a multiple of 4

``len`` is usually the payload length but may carry another quantity (a pull's capacity), so a
record is never split by ``len``.

Reply (slave -> master): a u32 header ``cmd | slot << 8`` followed by a command-specific body.
An io request's body is ``u32 result, u32 errno, data``. Some replies are a header alone (``header_only``).

Serial tunnel: push commands carry raw S-06 bytes (COBS frames with their delimiters) as payload;
the pull command's reply body is ``u32 n`` and ``n`` tunnel bytes, zero-padded to a multiple of 4.
"""
import struct
from dataclasses import dataclass, field

def pad4(b):
    b = bytes(b)
    return b + bytes(-len(b) % 4)


@dataclass(frozen=True)
class BridgeRequest:
    cmd: int
    slot: int = 0
    arg0: int = 0
    arg1: int = 0
    payload: bytes = field(default=b'')
    length: int = None      # the len field; None = len(payload)

    def __post_init__(self):
        object.__setattr__(self, 'payload', bytes(self.payload))

    @property
    def len_field(self):
        return len(self.payload) if self.length is None else self.length

    def encode(self):
        w0 = (self.cmd & 0xFF) | (self.slot & 0xFF) << 8 | (self.len_field & 0xFFFF) << 16
        return struct.pack('<III', w0, self.arg0 & 0xFFFFFFFF, self.arg1 & 0xFFFFFFFF) + pad4(self.payload)

    @classmethod
    def decode(cls, rec):
        rec = bytes(rec)
        if len(rec) < 12:
            raise ValueError(f'bridge request of {len(rec)} bytes, header is 12')
        w0, a0, a1 = struct.unpack_from('<III', rec)
        return cls(w0 & 0xFF, w0 >> 8 & 0xFF, a0, a1, rec[12:], w0 >> 16)

    def __repr__(self):
        return (f'BridgeRequest(cmd={self.cmd:#04x}, slot={self.slot}, len={self.len_field:#x}, '
                f'arg0={self.arg0:#x}, arg1={self.arg1:#x}, payload={self.payload.hex()!r})')


@dataclass(frozen=True)
class BridgeReply:
    cmd: int
    slot: int = 0
    body: bytes = field(default=b'')

    def __post_init__(self):
        object.__setattr__(self, 'body', bytes(self.body))

    def encode(self):
        return struct.pack('<I', (self.cmd & 0xFF) | (self.slot & 0xFF) << 8) + self.body

    @classmethod
    def decode(cls, rec):
        rec = bytes(rec)
        (h,) = struct.unpack_from('<I', rec)
        return cls(h & 0xFF, h >> 8 & 0xFF, rec[4:])

    @classmethod
    def word(cls, req, value):
        """A reply whose body is one u32."""
        return cls(req.cmd, req.slot, struct.pack('<I', value & 0xFFFFFFFF))

    @classmethod
    def io(cls, req, result=0, errno=0, data=b''):
        """An io request's reply: result, errno, data."""
        return cls(req.cmd, req.slot, struct.pack('<Ii', result & 0xFFFFFFFF, errno) + bytes(data))

    @classmethod
    def header_only(cls, cmd, slot):
        """A reply that is a header word alone (``slot`` may carry a status instead of a slot number)."""
        return cls(cmd, slot)

    @classmethod
    def pull(cls, req, tunnel_bytes):
        return cls(req.cmd, req.slot, struct.pack('<I', len(tunnel_bytes)) + pad4(tunnel_bytes))
