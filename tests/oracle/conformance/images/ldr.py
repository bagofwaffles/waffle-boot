# SPDX-License-Identifier: GPL-2.0-or-later
"""The ADSP-BF70x boot-stream (LDR) format from the public ADI hardware reference: block headers
{block code, target, byte count, argument}, a parser, and a generator of synthetic streams."""

import random
import struct
from dataclasses import dataclass

SIGNATURE = 0xAD
HEADER_SIZE = 16

FLAGS = {'FINAL': 1 << 15, 'FIRST': 1 << 14, 'INDIRECT': 1 << 13, 'IGNORE': 1 << 12, 'INIT': 1 << 11,
         'CALLBACK': 1 << 10, 'QUICKBOOT': 1 << 9, 'FILL': 1 << 8, 'AUX': 1 << 5, 'SAVE': 1 << 4}


@dataclass
class Block:
    flags: frozenset
    target: int
    count: int
    arg: int
    payload: bytes = b''
    dmacode: int = 4
    offset: int = 0          # file offset of the header, set by the parser

    @property
    def size(self):
        return HEADER_SIZE + (0 if 'FILL' in self.flags else self.count)


def flag_bits(flags):
    v = 0
    for f in flags:
        v |= FLAGS[f]
    return v


def flag_names(code):
    return frozenset(n for n, bit in FLAGS.items() if code & bit)


def pack_header(flags, target, count, arg, dmacode=4):
    code = (SIGNATURE << 24) | (flag_bits(flags) & 0xFFF0) | (dmacode & 0xF)
    h = bytearray(struct.pack('<IIII', code, target & 0xFFFFFFFF, count & 0xFFFFFFFF, arg & 0xFFFFFFFF))
    x = 0
    for b in h:
        x ^= b
    h[2] = x
    return bytes(h)


def header_ok(h):
    """Signature 0xAD and a zero XOR over the 16 header bytes."""
    if len(h) < HEADER_SIZE or h[3] != SIGNATURE:
        return False
    x = 0
    for b in h[:HEADER_SIZE]:
        x ^= b
    return x == 0


def pack_blocks(blocks):
    """Serialize blocks; recompute every FIRST block's argument to point at the last block of its
    application (the next block flagged FINAL or INIT, else the last block)."""
    offs, pos = [], 0
    for b in blocks:
        offs.append(pos)
        pos += b.size
    out = bytearray()
    for i, b in enumerate(blocks):
        arg = b.arg
        if 'FIRST' in b.flags:
            j = next((k for k in range(i, len(blocks)) if {'FINAL', 'INIT'} & set(blocks[k].flags)),
                     len(blocks) - 1)
            arg = offs[j] - offs[i]
        out += pack_header(b.flags, b.target, b.count, arg, b.dmacode)
        if 'FILL' not in b.flags:
            if len(b.payload) != b.count:
                raise ValueError('payload length differs from the byte count')
            out += b.payload
    return bytes(out)


def parse(data, offset=0, stop_at_final=True):
    """Parse blocks from `offset`. Raises ValueError on a bad header or a truncated payload."""
    blocks, pos = [], offset
    while pos + HEADER_SIZE <= len(data):
        h = data[pos:pos + HEADER_SIZE]
        if not header_ok(h):
            raise ValueError('bad block header at 0x%x' % pos)
        code, target, count, arg = struct.unpack('<IIII', h)
        flags = flag_names(code)
        payload = b''
        if 'FILL' not in flags:
            payload = bytes(data[pos + HEADER_SIZE:pos + HEADER_SIZE + count])
            if len(payload) != count:
                raise ValueError('truncated payload at 0x%x' % pos)
        blk = Block(flags, target, count, arg, payload, code & 0xF, pos)
        blocks.append(blk)
        pos += blk.size
        if stop_at_final and 'FINAL' in flags:
            break
    return blocks


def stream_length(data, offset=0):
    """Bytes from `offset` to the end of the FINAL block."""
    blocks = parse(data, offset)
    if not blocks or 'FINAL' not in blocks[-1].flags:
        raise ValueError('no FINAL block')
    last = blocks[-1]
    return last.offset + last.size - offset


def load(blocks):
    """The memory a boot of these blocks leaves: {address: bytes} per loaded or filled block.
    IGNORE blocks are skipped; FILL blocks store their argument as a repeated u32."""
    mem = {}
    for b in blocks:
        if 'IGNORE' in b.flags or b.count == 0:
            continue
        if 'FILL' in b.flags:
            mem[b.target] = (struct.pack('<I', b.arg) * (b.count // 4 + 1))[:b.count]
        else:
            mem[b.target] = b.payload
    return mem


def entry_point(blocks):
    first = next((b for b in blocks if 'FIRST' in b.flags), None)
    return None if first is None else first.target


def synthetic_app(seed=0, entry=0x11A00000, regions=None, fill=None, final_target=None):
    """A synthetic application stream: a FIRST|IGNORE block naming `entry`, one block per region
    (address, length) of seeded pseudo-random bytes, an optional FILL block (address, length,
    value), and a zero-length FINAL block. Returns (stream bytes, list of Block)."""
    rng = random.Random(seed)
    if regions is None:
        regions = [(entry, 0x400), (entry + 0x1000, 0x2345)]
    blocks = [Block(frozenset({'FIRST', 'IGNORE'}), entry, 0, 0)]
    for addr, n in regions:
        blocks.append(Block(frozenset(), addr, n, 0, rng.randbytes(n)))
    if fill is not None:
        addr, n, value = fill
        blocks.append(Block(frozenset({'FILL'}), addr, n, value))
    blocks.append(Block(frozenset({'FINAL'}), entry if final_target is None else final_target, 0, 0))
    data = pack_blocks(blocks)
    return data, parse(data)


def synthetic_app_of_size(size, seed=0, entry=0x11A00000):
    """A synthetic stream of exactly `size` bytes (size >= 0x30 and a multiple of 4 is not
    required): FIRST, one data block padded to fit, FINAL."""
    if size < 3 * HEADER_SIZE:
        raise ValueError('size below three headers')
    n = size - 3 * HEADER_SIZE
    data, _ = synthetic_app(seed, entry, regions=[(entry, n)])
    assert len(data) == size
    return data


# ---- the dict-based API ----

SIGN = SIGNATURE
FINAL, FIRST, INDIRECT, IGNORE = FLAGS['FINAL'], FLAGS['FIRST'], FLAGS['INDIRECT'], FLAGS['IGNORE']
INIT, CALLBACK, QUICKBOOT, FILL = FLAGS['INIT'], FLAGS['CALLBACK'], FLAGS['QUICKBOOT'], FLAGS['FILL']
AUX, SAVE = FLAGS['AUX'], FLAGS['SAVE']
DMACODE_32BIT = 4


class LdrError(ValueError):
    """A stream that does not parse: bad signature, bad checksum, truncated."""


def header(flags, target, count, argument, dmacode=DMACODE_32BIT):
    """One 16-byte block header from integer flags, HDRCHK set so the XOR of all 16 bytes is 0."""
    code = (SIGNATURE << 24) | (flags & 0xFFF0) | (dmacode & 0xF)
    h = bytearray(struct.pack('<IIII', code, target & 0xFFFFFFFF, count & 0xFFFFFFFF, argument & 0xFFFFFFFF))
    x = 0
    for b in h:
        x ^= b
    h[2] = x
    return bytes(h)


def build_ldr(blocks):
    """blocks: list of dicts {flags (int), target, payload (bytes) | count (zero payload of that size,
    or none when 0), argument (default 0), fill (a FILL block's argument), dmacode (bits 3:0 of the block
    code, the BCODE; default 4)}. The arguments are written as
    given (a FIRST block's argument is not recomputed; see `simple_os`). Returns the stream bytes."""
    out = bytearray()
    for b in blocks:
        flags = b.get('flags', 0)
        if flags & FILL:
            out += header(flags, b['target'], b['count'], b.get('fill', b.get('argument', 0)),
                          b.get('dmacode', DMACODE_32BIT))
        else:
            payload = b.get('payload', b'')
            count = len(payload) if 'payload' in b else b.get('count', 0)
            if 'payload' not in b and count:
                payload = bytes(count)
            out += header(flags, b['target'], count, b.get('argument', 0), b.get('dmacode', DMACODE_32BIT))
            out += payload
    return bytes(out)


def simple_os(entry, regions, fills=()):
    """A stream shaped like an OS image: a FIRST|IGNORE block of count 0 whose target is the entry and
    whose argument is the length of the rest of the stream; data blocks for `regions` [(target, bytes)];
    FILL blocks for `fills` [(target, count, word)]; a FINAL block of count 0 targeting the entry."""
    body = [{'target': t, 'payload': p} for t, p in regions]
    body += [{'flags': FILL, 'target': t, 'count': n, 'fill': w} for t, n, w in fills]
    tail = build_ldr(body + [{'flags': FINAL, 'target': entry, 'count': 0}])
    return header(FIRST | IGNORE, entry, 0, len(tail)) + tail


def parse_blocks(data):
    """Reads blocks up to and including the FINAL block. Returns dicts {offset, code, flags, target,
    count, argument, payload_offset (None for FILL)}. Raises LdrError on a bad signature, a bad checksum
    or a truncated header or payload."""
    blocks, off = [], 0
    while True:
        if off + HEADER_SIZE > len(data):
            raise LdrError('truncated header at 0x%x' % off)
        code, target, count, arg = struct.unpack_from('<IIII', data, off)
        if code >> 24 != SIGNATURE:
            raise LdrError('bad signature 0x%08x at 0x%x' % (code, off))
        if not header_ok(data[off:off + HEADER_SIZE]):
            raise LdrError('bad header checksum at 0x%x' % off)
        flags = code & 0xFFF0
        has_payload = not (flags & FILL)
        blk = {'offset': off, 'code': code, 'flags': flags, 'target': target, 'count': count,
               'argument': arg, 'payload_offset': off + HEADER_SIZE if has_payload else None}
        blocks.append(blk)
        off += HEADER_SIZE + (count if has_payload else 0)
        if has_payload and off > len(data):
            raise LdrError('truncated payload of block at 0x%x' % blk['offset'])
        if flags & FINAL:
            return blocks


def destinations(blocks):
    """A stream's destination: the merged [start, end) ranges of every block with a nonzero
    count that is not IGNORE (FILL blocks count). Takes parse_blocks' dicts."""
    rs = sorted((b['target'], b['target'] + b['count']) for b in blocks
                if b['count'] and not (b['flags'] & IGNORE))
    merged = []
    for s, e in rs:
        if merged and s <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], e)
        else:
            merged.append([s, e])
    return [tuple(r) for r in merged]
