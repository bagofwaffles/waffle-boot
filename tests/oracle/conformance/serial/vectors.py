# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic test vectors for the S-06 frame (vectors/frames.json).

Each vector:
    name       short identifier
    wire       hex of the bytes on the link (encoded frame(s) with delimiters)
    frame      the frame the bytes encode, if any: {route, seq, id, payload}
    decode     the strict single-frame decoder's verdict on the first frame: "ok" or a FrameError reason
    address, reply_mask    the receiving node
    receiver   the receiver's events on ``wire`` with the default switches: [kind, ...] (with the frame
               for "message")
    factory    the same with ``receiver.FACTORY``
    quirk      set when the receiver's behavior differs from a plain delimiter-based decoder

Write the file:  python3 -I -m <pkg>.vectors --write      Check it:  ... --check
"""
import json
import os
import random
import sys

from . import cobs
from .crc8 import crc8
from .frame import Frame, FrameError, decode
from .receiver import Receiver

HERE = os.path.dirname(os.path.abspath(__file__))
PATH = os.path.join(HERE, 'vectors', 'frames.json')


def _fdict(f):
    return {'route': f.route, 'seq': f.seq, 'id': f.id, 'payload': f.payload.hex()}


def first_piece(wire):
    """The first non-empty delimiter-separated piece of a wire (b'' if none)."""
    return next((p for p in wire.split(b'\x00') if p), b'')


def _verdict(wire):
    first = first_piece(bytes.fromhex(wire))
    try:
        decode(first)
        return 'ok'
    except FrameError as e:
        return e.reason


def _events(wire, address, mask):
    """The events with the ``FACTORY`` switches."""
    out = []
    for ev in Receiver.factory(address, mask).feed(bytes.fromhex(wire)):
        out.append(['message', _fdict(ev.frame)] if ev.kind == 'message' else [ev.kind])
    return out


def build():
    vs = []
    me = 0x2           # the receiving node's address in these vectors

    def add(name, wire, frame=None, receiver=None, quirk=None, address=me, mask=0x08, required=None):
        """``receiver``: the ``FACTORY`` events; ``required``: the default receiver's, when they differ."""
        v = {'name': name, 'wire': wire.hex() if isinstance(wire, bytes) else wire,
             'frame': _fdict(frame) if frame else None, 'address': address, 'reply_mask': mask}
        v['decode'] = _verdict(v['wire'])
        v['receiver'] = receiver if required is None else required
        v['factory'] = receiver
        if quirk:
            v['quirk'] = quirk
        vs.append(v)

    def ok(name, f, **kw):
        add(name, f.encode(), f, [['message', _fdict(f)]], **kw)

    # ---- valid frames for this node ----
    ok('empty-payload', Frame(0x12, 0, 0x01))
    ok('one-zero-byte', Frame(0x12, 1, 0x01, b'\x00'))
    ok('zeros-in-header', Frame(0x42, 0, 0x00, b''))
    ok('reply-slot-dst', Frame(0x48, 7, 0x02, b'\x01\x02\x03'))
    ok('reply-mask-dst-15', Frame(0x4f, 255, 0x7f, b'\xff'))
    ok('payload-zeros', Frame(0x32, 9, 0x40, b'\x00\x01\x00\x00\x02\x00'))
    ok('max-payload-nonzero', Frame(0x12, 0x11, 0x55, bytes(range(1, 250))))   # one 0xFF group
    ok('max-payload-zero', Frame(0x12, 0x12, 0x55, bytes(249)))
    rng = random.Random(0x5e71a1)
    for i in range(12):
        n = rng.choice([0, 1, 2, 7, 31, 100, 200, 248, 249])
        pl = bytes(0 if rng.random() < 0.2 else rng.randrange(1, 256) for _ in range(n))
        ok(f'random-{i}', Frame(rng.randrange(16) << 4 | rng.choice([me, 8, 9, 10, 11]),
                                rng.randrange(256), rng.randrange(256), pl))
    # a frame whose check byte is 0x00 (the last group then stands for the implied zero)
    for s in range(256):
        f = Frame(0x12, s, 0x09, b'\x10')
        if f.raw()[-1] == 0:
            ok('check-byte-zero', f)
            break
    # a frame whose raw bytes are all non-zero and 254 long: the 0xFF group with no final group
    for s in range(1, 256):
        f = Frame(0x12, s, 0x55, bytes(range(1, 250)))
        if 0 not in f.raw():
            ok('ff-group-no-final-code', f)
            break
    ok('two-frames', Frame(0x12, 3, 0x01, b'ab'))
    vs[-1]['wire'] = (Frame(0x12, 3, 0x01, b'ab').encode() + Frame(0x12, 4, 0x01, b'cd').encode()).hex()
    vs[-1]['receiver'] = vs[-1]['factory'] = [['message', _fdict(Frame(0x12, 3, 0x01, b'ab'))],
                                              ['message', _fdict(Frame(0x12, 4, 0x01, b'cd'))]]
    ok('leading-delimiters', Frame(0x12, 5, 0x01, b'x'))
    vs[-1]['wire'] = '0000' + vs[-1]['wire']
    for v in vs:
        v['decode'] = _verdict(v['wire'])

    # ---- rejected ----
    f = Frame(0x12, 0, 0x7a, b'\x0a\x0b')
    add('bad-crc', f.encode(bad_crc=True), f, [['crc']])
    add('truncated', f.encode()[:-3] + b'\x00', None, [['partial']])
    add('short-raw', cobs.encode(b'\x12\x00\x05') + b'\x00', None, [['partial']])
    raw = bytes([0x12, 0, 0x05, 250]) + bytes(range(1, 251))
    # overflow at 254 decoded bytes; the leftover check byte starts a frame whose second byte is the
    # delimiter: 'collect' swallows it, 'end' gives 'partial'
    add('len-field-250', cobs.encode(raw + bytes([crc8(raw)])) + b'\x00', None, [['overflow']],
        quirk='Q-EARLYDELIM', required=[['overflow'], ['partial']])
    raw = bytes([0x12, 0, 0x05, 255]) + b'\x01' * 10
    add('len-field-255-short', cobs.encode(raw + bytes([crc8(raw)])) + b'\x00', None, [['partial']])
    raw = bytes([0x12, 0, 0x05, 3]) + b'\x01\x02'
    add('len-field-longer-than-data', cobs.encode(raw + bytes([crc8(raw)])) + b'\x00', None, [['partial']])
    add('cobs-group-past-end', b'\x05\x12\x01\x00', None, [['partial']])
    add('empty-frame', b'\x00', None, [])
    # bytes after a complete frame and before the delimiter: the frame is delivered; the extra bytes start
    # a new frame, and here its second byte (0x44, dst 4) sends it to forwarding
    raw = Frame(0x12, 0, 0x01, b'\x07').raw() + b'\x33\x44'
    add('extra-bytes-before-delimiter', cobs.encode(raw) + b'\x00', None,
        [['message', _fdict(Frame(0x12, 0, 0x01, b'\x07'))], ['forward']], quirk='Q-EARLYDONE')
    raw = Frame(0x12, 0, 0x01, b'\x07').raw() + b'\x33\x32'
    add('extra-bytes-local-dst', cobs.encode(raw) + b'\x00', None,
        [['message', _fdict(Frame(0x12, 0, 0x01, b'\x07'))], ['partial']], quirk='Q-EARLYDONE')

    # ---- forwarding ----
    f = Frame(0x24, 1, 0x7e, b'\x01\x02')
    add('forward-other-dst', f.encode(), f, [['forward']])
    f = Frame(0x24, 1, 0x7e, b'\x01\x02')
    add('forward-bad-crc-unchecked', f.encode(bad_crc=True), f, [['forward']])
    long_fwd = b'\xff\x24' + b'\x01' * 300 + b'\x00'
    # 256 bytes collected; the rest (0x01 code bytes, second byte 0x01 = dst 1) is forwarded as a new frame
    add('forward-overflow', long_fwd, None, [['route-overflow'], ['forward']])
    f = Frame(0x00, 0, 0x01)    # route 0x00: the decision is taken on the next code byte (0x01 -> dst 1)
    # node 1: 'code-byte' delivers it; 'route-byte' (S-06.047) sees destination 0 and forwards it
    add('route-zero', f.encode(), f, _events(f.encode().hex(), 1, 0x08), quirk='Q-ROUTE0', address=1,
        required=[['forward']])
    add('delimiter-after-first-code', b'\x03\x00' + Frame(0x12, 0, 0x01).encode(), None,
        _events((b'\x03\x00' + Frame(0x12, 0, 0x01).encode()).hex(), me, 0x08), quirk='Q-EARLYDELIM',
        required=[['partial'], ['message', _fdict(Frame(0x12, 0, 0x01))]])
    return vs


def main(argv):
    vs = build()
    text = json.dumps({'vectors': vs}, indent=1) + '\n'
    if '--write' in argv:
        os.makedirs(os.path.dirname(PATH), exist_ok=True)
        open(PATH, 'w').write(text)
        print(f'wrote {len(vs)} vectors to {PATH}')
        return 0
    same = os.path.exists(PATH) and open(PATH).read() == text
    print(f'{len(vs)} vectors; file {"matches" if same else "DIFFERS"}')
    return 0 if same else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
