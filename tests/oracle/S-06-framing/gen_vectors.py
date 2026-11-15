#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates the synthetic S-06 vectors from s06_model.py and the conformance package.

    gen_vectors.py --write    write vectors/*.json, links.tsv, routing.tsv
    gen_vectors.py            check the files are current
"""
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import s06_model as m  # noqa: E402
from serial import cobs  # noqa: E402
from serial.crc8 import crc8  # noqa: E402
from serial.frame import Frame, FrameError, decode  # noqa: E402

V = os.path.join(HERE, 'vectors')


def fdict(f):
    return {'route': f'0x{f.route:02x}', 'seq': f.seq, 'id': f'0x{f.id:02x}', 'payload': f.payload.hex()}


def ev_json(evs):
    out = []
    for kind, v in evs:
        if kind == 'deliver':
            out.append(['deliver', fdict(v)])
        elif kind == 'forward':
            out.append(['forward', v.hex()])
        else:
            out.append(['drop'])
    return out


# ---- CRC-8 (S-06.003) ----

def crc_vectors():
    cases = [('check-123456789', b'123456789'), ('empty', b''), ('byte-00', b'\x00'), ('byte-01', b'\x01'),
             ('byte-a5', b'\xa5'), ('byte-ff', b'\xff'), ('header-12-00-01-00', b'\x12\x00\x01\x00'),
             ('header-14-07-03-02-payload', b'\x14\x07\x03\x02\x41\x42'),
             ('all-256', bytes(range(256)))]
    return [{'name': n, 'data': d.hex(), 'crc': f'0x{crc8(d):02x}'} for n, d in cases]


# ---- encoder (S-06.001..S-06.006) ----

def encode_vectors():
    vs = []

    def add(name, f, note=''):
        v = {'name': name, 'frame': fdict(f), 'raw': f.raw().hex(), 'wire': f.encode().hex()}
        if note:
            v['note'] = note
        vs.append(v)

    add('empty-payload', Frame(0x14, 0, 0x01))
    add('route-12-seq-ff', Frame(0x12, 0xFF, 0x02, b'\x01\x02\x03'))
    add('zero-seq-and-id', Frame(0x14, 0, 0x00, b''), 'zero header bytes become code bytes')
    add('payload-zeros', Frame(0x13, 9, 0x40, b'\x00\x01\x00\x00\x02\x00'))
    add('request-slot-2', Frame(0xA4, 5, 0x7A, b'\x0a\x0b'), 'a request: source nibble 8 + 2')
    add('reply-to-slot-2', Frame(0x4A, 0, 0x7B, b'\x01'), 'the reply: destination nibble 8 + 2')
    add('max-payload-zeros', Frame(0x15, 0x12, 0x55, bytes(249)))
    add('max-payload-nonzero', Frame(0x15, 0x11, 0x55, bytes(range(1, 250))))
    for s in range(256):
        f = Frame(0x14, s, 0x09, b'\x10')
        if f.raw()[-1] == 0:
            add('check-byte-zero', f, 'the check byte is 0x00: the last group is a lone code byte 0x01')
            break
    for s in range(1, 256):
        f = Frame(0x15, s, 0x55, bytes(range(1, 250)))
        if 0 not in f.raw():
            add('ff-group-no-final-code', f, '254 raw bytes, none zero: one 0xFF group and no final group')
            break
    rng = random.Random(0x5e06)
    for i in range(10):
        n = rng.choice([0, 1, 3, 17, 64, 128, 200, 248, 249])
        pl = bytes(0 if rng.random() < 0.25 else rng.randrange(1, 256) for _ in range(n))
        add(f'random-{i}', Frame(rng.choice([0x12, 0x13, 0x14, 0x15, 0x51, 0x21, 0x41, 0x31, 0x84, 0xB3]),
                                 rng.randrange(256), rng.randrange(256), pl))
    return vs


# ---- receiver (S-06.040..S-06.049) ----

ME = 1      # the receiving node: the bootloader on the master


def receive_vectors():
    vs = []

    def add(name, wire, quirk=None, note=''):
        wire = bytes(wire)
        req = m.RequiredReceiver(ME).feed(wire)
        factory = m.factory_events(wire, ME)
        v = {'name': name, 'wire': wire.hex(), 'address': ME, 'required': ev_json(req)}
        if quirk:
            v['quirk'] = quirk          # 'factory': the outcome under the package's compatibility preset
            v['factory'] = ev_json(factory)
        elif ev_json(req) != ev_json(factory):
            raise AssertionError(f'{name}: required and factory differ without a quirk')
        if note:
            v['note'] = note
        vs.append(v)

    F = Frame
    # delivered
    add('deliver-empty-payload', F(0x41, 0, 0x01).encode())
    add('deliver-own-address', F(0x21, 7, 0x09, b'abc').encode())
    add('deliver-reply-slot-8', F(0x48, 1, 0x02, b'\x01').encode())
    add('deliver-reply-slot-11', F(0x4B, 2, 0x02).encode())
    add('deliver-dst-12', F(0x4C, 3, 0x7E, b'\xff').encode(), note='12..15 are local (S-06.013)')
    add('deliver-dst-15', F(0x4F, 255, 0x7F, b'\x00').encode())
    add('deliver-max-payload', F(0x41, 0x10, 0x55, bytes(range(1, 250))).encode())
    add('deliver-max-payload-zeros', F(0x41, 0x11, 0x55, bytes(249)).encode())
    add('deliver-any-seq', F(0x41, 200, 0x01).encode() + F(0x41, 3, 0x01).encode() + F(0x41, 3, 0x01).encode(),
        note='seq gaps and repeats do not matter (S-06.021)')
    add('leading-delimiters', b'\x00\x00' + F(0x41, 5, 0x01, b'x').encode())
    add('back-to-back', F(0x41, 3, 0x01, b'ab').encode() + F(0x41, 4, 0x01, b'cd').encode())
    # quirk Q-EARLYDONE: completion before the delimiter
    add('no-delimiter-between-frames', F(0x41, 0, 0x01, b'\x07').encode()[:-1] + F(0x41, 1, 0x01).encode(),
        quirk='Q-EARLYDONE', note='both frames are delivered (S-06.041)')
    z = next(F(0x41, s, 0x09, b'\x10') for s in range(256) if F(0x41, s, 0x09, b'\x10').raw()[-1] == 0)
    add('no-delimiter-after-zero-check', z.encode()[:-1] + F(0x41, 1, 0x01).encode(), quirk='Q-EARLYDONE',
        note='the first frame ends in a lone 01 group (its check byte is 00); both frames are delivered')
    raw = F(0x41, 0, 0x01, b'\x07').raw() + b'\x33\x44'
    add('extra-bytes-for-other-node', cobs.encode(raw) + b'\x00', quirk='Q-EARLYDONE',
        note='the frame is delivered; the extra bytes start a frame whose route byte 0x44 sends it on')
    raw = F(0x41, 0, 0x01, b'\x07').raw() + b'\x33\x31'
    add('extra-bytes-local', cobs.encode(raw) + b'\x00', quirk='Q-EARLYDONE',
        note='the frame is delivered; the extra bytes are a partial frame the delimiter drops')
    # dropped
    f = F(0x41, 0, 0x7A, b'\x0a\x0b')
    add('bad-crc', f.encode(bad_crc=True))
    add('bad-crc-then-good', f.encode(bad_crc=True) + F(0x41, 1, 0x01).encode())
    add('truncated', f.encode()[:-3] + b'\x00')
    add('short-raw', cobs.encode(b'\x41\x00\x05') + b'\x00')
    raw = bytes([0x41, 0, 0x05, 3]) + b'\x01\x02'
    add('len-field-longer-than-data', cobs.encode(raw + bytes([crc8(raw)])) + b'\x00')
    raw = bytes([0x41, 0, 0x05, 255]) + b'\x01' * 10
    add('len-field-255-short', cobs.encode(raw + bytes([crc8(raw)])) + b'\x00')
    raw = bytes([0x41, 0, 0x05, 250]) + bytes(range(1, 251))
    add('len-field-250', cobs.encode(raw + bytes([crc8(raw)])) + b'\x00' + F(0x41, 9, 0x01).encode(),
        quirk='Q-EARLYDELIM',
        note='overflow at 254 bytes; the leftover byte and the delimiter end a partial frame; the next frame '
             'is delivered (compatibility preset: the next frame is dropped)')
    add('cobs-group-past-end', b'\x05\x41\x01\x00')
    add('lone-delimiter', b'\x00')
    add('stray-byte-then-frame', b'\x03\x00' + F(0x41, 0, 0x01).encode(), quirk='Q-EARLYDELIM',
        note='a stray byte and a delimiter are dropped; the next frame is delivered (compatibility preset: the '
             'next frame is dropped)')
    add('bad-crc-zero-len', F(0x41, 0, 0x01).encode(bad_crc=True))
    # forwarded
    f = F(0x14, 1, 0x7E, b'\x01\x02')
    add('forward-dst-4', f.encode())
    add('forward-bad-crc-unchecked', f.encode(bad_crc=True), note='forwarded frames are not checked')
    add('forward-max', F(0x24, 2, 0x55, bytes(range(1, 250))).encode(), note='a 256-byte forward')
    add('forward-max-ff-group', next(F(0x24, s, 0x55, bytes(range(1, 250))).encode()
                                     for s in range(1, 256) if 0 not in F(0x24, s, 0x55, bytes(range(1, 250))).raw()))
    add('forward-dst-2-then-local', F(0x12, 1, 0x7E).encode() + F(0x21, 2, 0x7E).encode())
    add('forward-overflow', b'\xff\x24' + b'\x01' * 300 + b'\x00', quirk='Q-ROUTE0',
        note='256 bytes without a delimiter are dropped; the rest starts a new frame whose route byte is 0 '
             '(code bytes 01 01), so it is collected for forwarding (compatibility preset: the rest is dropped)')
    f = F(0x00, 0, 0x01)
    add('route-00', f.encode(), quirk='Q-ROUTE0',
        note='destination 0 is not local and no link takes it (S-06.047, S-06.050). Route 00 '
             'with seq 00 always encodes as 01 01 ...; the compatibility preset (address 1) delivers such a '
             'frame (S-16.142, design; S-06 question 2)')
    f = F(0x00, 0x21, 0x01)
    add('route-00-seq-21', f.encode(), note='route 0x00 again; here the compatibility preset reaches the same verdict')
    add('route-10', F(0x10, 0, 0x01).encode(), note='destination 0 with a non-zero source: not local')
    return vs


# ---- links and routing (S-06.011, S-06.050..S-06.053) ----

def links_rows():
    rows = [('# core', 'link', 'own', 'peer', 'route of an originated message')]
    for core, links in (('master', m.LINKS_MASTER), ('slave', m.LINKS_SLAVE)):
        for k, (own, peer) in links.items():
            rows.append((core, k, str(own), str(peer), f'0x{own << 4 | peer:02x}'))
    return rows


def routing_rows():
    rows = [('# core', 'incoming link', 'dst', 'verdict')]
    for core, links, fwd in (('master', m.LINKS_MASTER, m.FORWARD_MASTER), ('slave', m.LINKS_SLAVE, {})):
        for k, (own, _) in links.items():
            for dst in range(16):
                if m.is_local(dst, own):
                    v = 'local'
                elif (k, dst) in fwd:
                    v = 'forward:' + fwd[(k, dst)]
                else:
                    v = 'drop'
                rows.append((core, k, str(dst), v))
    return rows


def tsv(rows, head):
    return head + ''.join('\t'.join(r) + '\n' for r in rows)


def outputs():
    return {
        os.path.join(V, 'crc.json'): json.dumps({'vectors': crc_vectors()}, indent=1) + '\n',
        os.path.join(V, 'encode.json'): json.dumps({'vectors': encode_vectors()}, indent=1) + '\n',
        os.path.join(V, 'receive.json'): json.dumps({'vectors': receive_vectors()}, indent=1) + '\n',
        os.path.join(HERE, 'links.tsv'): tsv(links_rows(), '# S-06.011\n'),
        os.path.join(HERE, 'routing.tsv'): tsv(routing_rows(), '# S-06.013, S-06.050, S-06.051\n'),
    }


def main(argv):
    bad = 0
    for path, text in outputs().items():
        if '--write' in argv:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'w') as f:
                f.write(text)
            print('wrote', os.path.relpath(path, HERE))
        else:
            same = os.path.exists(path) and open(path).read() == text
            print(os.path.relpath(path, HERE), 'matches' if same else 'DIFFERS')
            bad += not same
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
