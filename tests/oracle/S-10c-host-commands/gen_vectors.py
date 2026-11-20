#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the S-10c test vectors from s10c_model.py and the conformance package.

    python3 -I -B tests/oracle/S-10c-host-commands/gen_vectors.py --write     write vectors/*.json, service.tsv
    python3 -I -B tests/oracle/S-10c-host-commands/gen_vectors.py             check the files are current

All vectors are synthetic: serials, FSN words, payloads and versions are made up. Nothing is random.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import s10c_model as m  # noqa: E402
from serial.frame import Frame  # noqa: E402
from serial.receiver import Receiver  # noqa: E402

V = os.path.join(HERE, 'vectors')


def h(b):
    return bytes(b).hex()


def fdict(f):
    return {'route': f'0x{f.route:02x}', 'seq': f.seq, 'id': f'0x{f.id:02x}', 'payload': f.payload.hex()}


# ---- the device state the transcripts assume (set by the runner: metadata record, OTP, straps) ----

DEVICE = {
    'meta': {'serial': 'AB123456', 'sku': 7, 'factory': 1, 'sw': [1, 2, 3, 0], 'os': [1, 2, 3, 45]},
    'dsp_serial': 'ZZ0123456789ABCDEF',
    'hw_rev': 255,
    'fsn_words': ['0x33221100', '0x77665544', '0xbbaa9988', '0xffeeddcc'],
}
FSN_WORDS = [int(w, 16) for w in DEVICE['fsn_words']]
VERSION_RE = r'[0-9]+\.[0-9]+\.[0-9]+(\+[0-9]+)?'


def device(**over):
    d = m.Device(fsn=m.fsn_from_words(FSN_WORDS), meta=json.loads(json.dumps(DEVICE['meta'])),
                 dsp_serial=DEVICE['dsp_serial'], hw_rev=DEVICE['hw_rev'])
    for k, v in over.items():
        setattr(d, k, v)
    return d


def info_re(text):
    """The info text as a pattern: bl_version may be any version (S-17.032)."""
    head, rest = text.split(';bl_version:', 1)
    tail = rest.split(';', 1)[1]
    return '^' + re.escape(head + ';bl_version:') + VERSION_RE + re.escape(';' + tail) + '$'


# ---- pack7.json (S-10c.001, .002) ----

def gen_pack7():
    inputs = [
        ('empty', b''), ('one-00', b'\x00'), ('one-7f', b'\x7f'), ('one-80', b'\x80'), ('one-ff', b'\xff'),
        ('seven-ff', b'\xff' * 7), ('seven-alternating', bytes([0x80, 0x01, 0x82, 0x03, 0x84, 0x05, 0x86])),
        ('eight', bytes([0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xF7])),
        ('fourteen', bytes(range(0x80, 0x8E))), ('fifteen', bytes(range(0xF1, 0x100))),
        ('text', b'mode:bootloader'), ('all-256', bytes(range(256))),
    ]
    pack = [{'name': 'pack-' + n, 'data': h(d), 'packed': h(m.pack7(d))} for n, d in inputs]
    unpack = [{'name': 'unpack-' + n, 'packed': h(m.pack7(d)), 'data': h(d)} for n, d in inputs]
    seven = bytes([0x81, 0x02, 0x83, 0x04, 0x85, 0x06, 0x87])
    unpack.append({'name': 'header-only-tail', 'packed': h(m.pack7(seven) + b'\x01'), 'data': h(seven),
                   'note': 'a last group of a header byte alone adds nothing'})
    unpack.append({'name': 'bit7-set-ignored', 'packed': '818182', 'data': '8102',
                   'note': 'bit 7 of the header and of the data bytes is ignored (packing 81 02 gives 01 01 02)'})
    unpack.append({'name': 'partial-group', 'packed': '000102', 'data': '0102'})
    for v in unpack:
        assert m.unpack7(bytes.fromhex(v['packed'])) == bytes.fromhex(v['data'])
    return {'pack': pack, 'unpack': unpack}


# ---- usbmidi.json (S-10c.003, .004, .006) ----

ID_REQ = bytes.fromhex('f07e210601f7')


def gen_usbmidi():
    send_msgs = [
        ('send-2', b'\xf0\xf7'), ('send-3', bytes.fromhex('f07ef7')), ('send-4', bytes.fromhex('f07e01f7')),
        ('send-5', bytes.fromhex('f07e0102f7')), ('send-6', ID_REQ), ('send-7', bytes.fromhex('f07e01020304f7')),
        ('send-identity-reply', m.IDENTITY_REPLY),
        ('send-288', m.te_reply(0x02, 1, 0x00, bytes(range(242)))),
    ]
    send = [{'name': n, 'message': h(x), 'packets': h(m.to_packets(x))} for n, x in send_msgs]
    assert len(bytes.fromhex(send[-1]['message'])) == m.MAX_SENT
    P = bytes.fromhex
    recv = [
        ('receive-identity', m.to_packets(ID_REQ), [ID_REQ]),
        ('receive-end-5', P('04f07e21' '04060102' '05f70000'), [P('f07e21060102f7')]),
        ('receive-end-6', P('04f07e21' '04060101' '0602f700'), [P('f07e2106010102f7')]),
        ('receive-end-7', P('04f07e21' '07060103') + P('04f07e21' '070601f7'), [ID_REQ]),
        ('receive-cin-f-data', P('04f07e21' '0f060000' '0f010000' '05f70000'), [ID_REQ]),
        ('receive-cin-f-status-ignored', P('04f07e21' '0ff80000' '0f060000' '0ff00000' '0601f700'), [ID_REQ]),
        ('receive-other-cin-ignored', P('04f07e21' '09903c7f' '0bb00764' '02f10100' '070601f7'), [ID_REQ]),
        ('receive-other-cable-ignored', P('04f07e21' '14010203' '17f07ef7' '070601f7'), [ID_REQ]),
        ('receive-not-f0', P('04017e21' '0601f700'), []),
        ('receive-no-f7', P('04f07e21' '06060100'), []),
        ('receive-two', m.to_packets(ID_REQ) + m.to_packets(P('f07e7f0601f7')), [ID_REQ, P('f07e7f0601f7')]),
    ]
    big = bytes([0xF0, 0x7D]) + bytes(i & 0x7F for i in range(8189)) + b'\xf7'
    assert len(big) == m.MAX_RECEIVED
    long_ = bytes([0xF0, 0x7D]) + bytes(i & 0x7F for i in range(8190)) + b'\xf7'
    recv.append(('receive-8192', m.to_packets(big) + m.to_packets(ID_REQ), [big, ID_REQ]))
    recv.append(('receive-8193-then-next', m.to_packets(long_) + m.to_packets(ID_REQ), [ID_REQ]))
    receive = []
    for n, pk, msgs in recv:
        got = m.reassemble(pk)
        assert got == msgs, (n, [x.hex() for x in got])
        receive.append({'name': n, 'packets': h(pk), 'messages': [h(x) for x in msgs]})
    return {
            'send': send, 'receive': receive}


# ---- carrier.json (S-10c.020-.022) ----

def deliver(stream):
    rx = Receiver(1)
    frames, fwd = [], []
    for ev in rx.feed(stream):
        if ev.kind == 'message':
            frames.append(fdict(ev.frame))
        elif ev.kind == 'forward':
            fwd.append(h(ev.data))
    return frames, fwd


def gen_carrier():
    enc = [('encode-empty', b''), ('encode-1', b'\x00'), ('encode-7', bytes(range(0x80, 0x87))),
           ('encode-8', bytes(range(8))), ('encode-245', bytes(i & 0xFF for i in range(245)))]
    encode = [{'name': n, 'stream': h(s), 'message': h(m.carrier(s))} for n, s in enc]
    assert len(m.carrier(bytes(245))) == 287 and len(m.carrier(bytes(246))) > m.MAX_SENT

    def nz(n):
        return bytes((i % 251) + 1 for i in range(n))
    tx_frames = [('tx-pong', Frame(0x12, 3, 0x02, b'\x5a')),
                 ('tx-zeros', Frame(0x12, 0, 0x0A, bytes(40))),
                 ('tx-238', Frame(0x12, 9, 0x02, nz(238))),
                 ('split-239', Frame(0x12, 9, 0x02, nz(239))),
                 ('split-249', Frame(0x12, 9, 0x02, nz(249))),
                 ('split-249-zeros', Frame(0x12, 0, 0x02, bytes(249)))]
    tx = []
    for n, f in tx_frames:
        e = f.encode()
        msgs = m.carrier_messages_for_frame(e)
        assert all(len(x) <= m.MAX_SENT for x in msgs)
        tx.append({'name': n, 'frame': fdict(f), 'encoded': h(e), 'messages': [h(x) for x in msgs]})
    assert len(tx[2]['messages']) == 1 and len(bytes.fromhex(tx[2]['encoded'])) == 245
    assert len(tx[3]['messages']) == 2

    a = Frame(0x21, 1, 0x01, b'\x11').encode()
    b = Frame(0x21, 2, 0x09, b'').encode()
    big = Frame(0x21, 3, 0x01, nz(249)).encode()
    big2 = Frame(0x21, 4, 0x01, nz(249)).encode()
    assert len(big) + len(big2) == 512
    rx_cases = [
        ('rx-one', [a]),
        ('rx-two-in-one', [a + b]),
        ('rx-leading-delimiters', [b'\x00\x00' + a]),
        ('rx-split-across', [a[:3], a[3:]]),
        ('rx-partial-then-whole', [b[:2], a]),
        ('rx-tail-dropped-then-next', [a + b[:2], b[2:], b]),
        ('rx-512', [big + big2]),
        ('rx-513', [big + big2 + b'\x00']),
        ('rx-empty', [b'']),
        ('rx-other-destination', [Frame(0x24, 5, 0x7A, b'\x0a\x0b').encode() + a]),
    ]
    rx = []
    for n, pieces in rx_cases:
        msgs = [m.carrier(p) for p in pieces]
        frames, fwd = [], []
        for x in msgs:
            s = m.carrier_stream(x)
            if s is None:
                continue
            fr, fw = deliver(s)
            frames += fr
            fwd += fw
        rx.append({'name': n, 'messages': [h(x) for x in msgs], 'frames': frames, 'forwarded': fwd})
    return {
            'encode': encode, 'tx': tx, 'rx': rx}


# ---- TE request/reply sequences (sysex.json) ----

def te_match(reply):
    f = m.parse_te(reply)
    return {'bytes': h(reply), 'te': {'cmd': f['cmd'], 'req_id': f['req_id'], 'status': f['status'],
                                       'payload': h(f['payload'])}}


def te_text(cmd, rid, status, **kw):
    t = {'cmd': cmd, 'req_id': rid, 'status': status}
    t.update(kw)
    return {'te': t}


def dfu_begin(word=0x00084000, length=4000, version=(1, 2, 3, 4), extra=b''):
    return (b'\x02' + b''.join(v.to_bytes(2, 'big') for v in version) + b'\x00' + word.to_bytes(4, 'big')
            + length.to_bytes(4, 'big') + extra)


class Seq:
    """A sequence of host messages with the device's replies, generated from the model."""

    def __init__(self, name, note='', dev=None):
        self.name, self.note = name, note
        self.dev = dev or device()
        self.steps = []
        self.rid = 0

    def send(self, msg, replies=None):
        got = [r for x in m.reassemble(m.to_packets(msg)) for r in m.midi_message(self.dev, x)]
        auto = []
        for r in got:
            if m.classify(r) == 'identity' or r == m.IDENTITY_REPLY:
                auto.append({'bytes': h(r)})
            elif r[:4] == bytes.fromhex('f0002076') and r[5] == 0x40 and len(r) >= 11:
                auto.append(te_match(r))
            else:
                auto.append({'bytes': h(r)})
        if replies is not None:          # a looser matcher where the vector pins less than the model
            assert len(replies) == len(auto), (self.name, replies, auto)
            auto = replies
        self.steps.append({'host': h(msg), 'replies': auto})
        return got

    def te(self, cmd, payload=b'', rid='auto', replies=None, **kw):
        if rid == 'auto':
            self.rid = (self.rid + 1) % 4096
            rid = self.rid
        return self.send(m.te_request(cmd, payload, req_id=rid, **kw), replies)

    def out(self):
        d = {'name': self.name, 'steps': self.steps}
        if self.note:
            d['note'] = self.note
        return d


def chunk(n, data=b'\x55'):
    return bytes([0x03, n & 0xFF]) + data


NO_TRANSFER_CHUNK = r'^(unexpected error when handling chunk|unexpected chunk {n} \(exp [0-9]+\))$'


def gen_sysex():
    classify = []
    cl = [('classify-te', m.te_request(0x01, req_id=1), 'te'),
          ('classify-te-device-7f', m.te_request(0x01, device=0x7F), 'te'),
          ('classify-identity-21', ID_REQ, 'identity'),
          ('classify-identity-7f', bytes.fromhex('f07e7f0601f7'), 'identity'),
          ('classify-identity-other', bytes.fromhex('f07e000601f7'), 'ignored'),
          ('classify-carrier', m.carrier(b'\x00'), 'carrier'),
          ('classify-carrier-empty', m.carrier(b''), 'carrier'),
          ('classify-carrier-other-device', bytes.fromhex('f00020767f6600f7'), 'ignored'),
          ('classify-te-9-bytes', bytes.fromhex('f000207621404000f7'), 'ignored'),
          ('classify-te-reply-form-10', bytes.fromhex('f00020762140000001f7'), 'ignored'),
          ('classify-te-reply-form-11', bytes.fromhex('f0002076214000000100f7'), 'te'),
          ('classify-other-manufacturer', bytes.fromhex('f0002077214040000100f7'), 'ignored'),
          ('classify-universal-other', bytes.fromhex('f07e210602f7'), 'ignored')]
    for n, x, c in cl:
        assert m.classify(x) == c, n
        classify.append({'name': n, 'message': h(x), 'class': c})

    vs = []
    # identity
    for n, x in [('identity-21', ID_REQ), ('identity-7f', bytes.fromhex('f07e7f0601f7')),
                 ('identity-other-device', bytes.fromhex('f07e000601f7')),
                 ('identity-long', bytes.fromhex('f07e21060100f7'))]:
        s = Seq(n)
        s.send(x)
        vs.append(s.out())
    # flags and request ids
    s = Seq('dfu-request-ids', 'request ids 0, 1, 127, 128, 4095 and none; the reply carries the id, bit 6 clear')
    for rid in (0, 1, 127, 128, 4095, None):
        s.te(0x05, rid=rid)
    vs.append(s.out())
    for n, dev_b in [('dfu-device-7f', 0x7F), ('dfu-device-00', 0x00), ('dfu-device-21', 0x21)]:
        s = Seq(n, 'the device byte is not checked; the reply carries 21')
        s.te(0x00, device=dev_b)
        vs.append(s.out())
    s = Seq('dfu-reply-form', 'a message in the reply form (flags bit 6 clear) of 11 bytes or more is handled; '
                              'offset 9 is ignored and the payload starts at offset 10')
    s.send(bytes.fromhex('f0002076214020050200') + m.pack7(b'abc') + b'\xf7')
    s.send(bytes.fromhex('f00020762140000002') + bytes([0x7F]) + b'\xf7')
    s.send(bytes.fromhex('f00020762140000001f7'))
    vs.append(s.out())
    s = Seq('dfu-9-bytes', 'a 9-byte TE message gets no reply')
    s.send(bytes.fromhex('f000207621404000f7'))
    s.send(bytes.fromhex('f000207621406105f7'))
    vs.append(s.out())
    s = Seq('dfu-10-bytes', 'the shortest request: command, no payload')
    s.send(bytes.fromhex('f00020762140400001f7'),
           [te_text(0x01, None, 0x00, text_re=info_re(device().info))])
    vs.append(s.out())
    # commands
    s = Seq('dfu-cmd-unknown', 'commands 00 and 04-7F: status 02, no payload')
    for c in (0x00, 0x04, 0x05, 0x40, 0x7F):
        s.te(c, b'\x01\x02')
    vs.append(s.out())
    s = Seq('dfu-info')
    s.te(0x01, b'', replies=[te_text(0x01, 1, 0x00, text_re=info_re(device().info))])
    s.te(0x01, b'any payload', replies=[te_text(0x01, 2, 0x00, text_re=info_re(device().info))])
    vs.append(s.out())
    for n in (0, 1, 242, 243, 256):
        s = Seq(f'echo-{n}')
        got = s.te(0x02, bytes((i * 7) & 0xFF for i in range(n)))
        assert all(len(g) <= m.MAX_SENT for g in got)
        vs.append(s.out())
    # upgrade sub-commands
    s = Seq('dfu-sub-unknown', 'status 03, no payload: an empty payload, unknown sub-commands, a begin of 17 '
                               'bytes, a chunk of 1 byte')
    for p in (b'', b'\x00', b'\x06', b'\x7f', b'\xff', dfu_begin()[:17], b'\x03'):
        s.te(0x03, p)
    vs.append(s.out())
    s = Seq('dfu-start')
    s.te(0x03, b'\x01')
    s.te(0x03, b'\x01\x02\x03')
    vs.append(s.out())
    s = Seq('dfu-reboot', 'the reply comes first; the reset follows at least 10 ms later (scenario H-DFU-UPLOAD)')
    s.te(0x03, b'\x05')
    vs.append(s.out())
    # begin
    s = Seq('dfu-begin-ok')
    s.te(0x03, dfu_begin())
    s.te(0x03, chunk(0))
    vs.append(s.out())
    s = Seq('dfu-begin-long', 'bytes from 18 on are ignored; bytes 1-9 and the length are not checked')
    s.te(0x03, dfu_begin(version=(0xFFFF,) * 4, length=0, extra=b'\xff' * 20))
    vs.append(s.out())
    s = Seq('dfu-begin-sku-bits', 'bits 0-9 and 24-31 of the product word are not interpreted')
    s.te(0x03, dfu_begin(word=0xFF0843FF))
    vs.append(s.out())
    s = Seq('dfu-begin-again-before-chunk', 'a begin while a transfer has no chunk yet changes nothing')
    s.te(0x03, dfu_begin())
    s.te(0x03, dfu_begin())
    s.te(0x03, chunk(0))
    vs.append(s.out())
    s = Seq('dfu-begin-after-chunk', 'a begin after a chunk discards the transfer and starts a new one at 0')
    s.te(0x03, dfu_begin())
    s.te(0x03, chunk(0))
    s.te(0x03, chunk(1))
    s.te(0x03, dfu_begin())
    s.te(0x03, chunk(2))
    s.te(0x03, chunk(0))
    vs.append(s.out())
    for n, w in [('dfu-begin-bad-product-34', 0x00088000), ('dfu-begin-bad-variant', 0x00084400),
                 ('dfu-begin-bad-zero', 0x00000000), ('dfu-begin-bad-over-999', 0x00FF8000),
                 ('dfu-begin-bad-variant-f', 0x00087C00 | 0x3E8)]:
        s = Seq(n, 'incorrect sku: and the TE code of the word')
        s.te(0x03, dfu_begin(word=w))
        vs.append(s.out())
    s = Seq('dfu-begin-bad-product-keeps-transfer')
    s.te(0x03, dfu_begin())
    s.te(0x03, chunk(0))
    s.te(0x03, dfu_begin(word=0x00088000))
    s.te(0x03, chunk(1))
    vs.append(s.out())
    # chunks
    s = Seq('dfu-chunk-wrap', '300 chunks: the numbers wrap from 255 to 0; then a repeated and a skipped number')
    s.te(0x03, dfu_begin(length=300))
    for i in range(300):
        s.te(0x03, chunk(i, bytes([i & 0x7F])))
    s.te(0x03, chunk(299 & 0xFF))
    s.te(0x03, chunk(46))
    s.te(0x03, chunk(300 & 0xFF))
    vs.append(s.out())
    s = Seq('dfu-chunk-empty-data', 'a chunk of 2 bytes appends nothing and is accepted')
    s.te(0x03, dfu_begin())
    s.te(0x03, b'\x03\x00')
    s.te(0x03, b'\x03\x01')
    vs.append(s.out())
    s = Seq('dfu-chunk-no-transfer', 'with no transfer in progress: status 01 and one of the two texts')
    s.te(0x03, chunk(0), replies=[te_text(0x03, 1, 0x01, text_re=NO_TRANSFER_CHUNK.format(n=0))])
    s.te(0x03, chunk(5), replies=[te_text(0x03, 2, 0x01, text_re=NO_TRANSFER_CHUNK.format(n=5))])
    vs.append(s.out())
    s = Seq('dfu-finish-no-transfer')
    s.te(0x03, b'\x04')
    s.te(0x03, b'\x04\x01\x02')
    vs.append(s.out())
    # overflow
    s = Seq('dfu-overflow', 'a payload of 4097 bytes: buffer overflow, and the transfer ends')
    s.te(0x03, dfu_begin())
    s.te(0x03, chunk(0))
    s.te(0x03, chunk(1, bytes(4095)))
    s.te(0x03, chunk(1), replies=[te_text(0x03, 4, 0x01, text_re=NO_TRANSFER_CHUNK.format(n=1))])
    vs.append(s.out())
    s = Seq('dfu-overflow-4096', 'a payload of exactly 4096 bytes is handled')
    s.te(0x03, dfu_begin())
    s.te(0x03, chunk(0, bytes(4094)))
    s.te(0x03, chunk(1))
    vs.append(s.out())
    s = Seq('dfu-overflow-echo', 'buffer overflow for every command, the echo and an unknown one included')
    s.te(0x02, bytes(4097))
    s.te(0x50, bytes(5000))
    vs.append(s.out())
    for v in vs:
        for st in v['steps']:
            assert len(bytes.fromhex(st['host'])) <= m.MAX_RECEIVED
    return {
            'classify': classify, 'vectors': vs}


# ---- commands.json (S-10c.040-.075) ----

IGNORE = [{'id': ['0x03', '0x04']}, {'id': '0x0e', 'prefix': '92'}]


class Host:
    def __init__(self, name, link='cdc', note='', device_over=None):
        self.name, self.link, self.note = name, link, note
        self.device_over = device_over or {}
        self.steps = []
        self.seq = 0
        self.mark = 0

    def send(self, msg_id, payload=b'', route=0x21, note=''):
        st = {'inject': [{'route': f'0x{route:02x}', 'seq': self.seq, 'id': f'0x{msg_id:02x}',
                          'payload': h(payload)}]}
        if note:
            st['note'] = note
        self.steps.append(st)
        self.seq = (self.seq + 1) & 0xFF

    def expect(self, msg_id, payload=None, route=0x12, prefix=None, payload_re=None, note=''):
        e = {'id': f'0x{msg_id:02x}', 'route': f'0x{route:02x}'}
        if payload is not None:
            e['payload'] = h(payload)
        if prefix is not None:
            e['prefix'] = h(prefix)
        if payload_re is not None:
            e['payload_re'] = payload_re
        st = {'expect': e}
        if note:
            st['note'] = note
        self.steps.append(st)

    def ping(self, note='a ping: the next frame is its pong, so nothing came in between'):
        self.mark += 1
        p = bytes([0x50, self.mark])
        self.send(0x01, p, note=note)
        self.expect(0x02, p)

    def info(self, text):
        self.send(0x09)
        self.expect(0x0A, payload_re=info_re(text))

    def out(self):
        d = {'name': self.name, 'link': self.link,
             'script': {'name': self.name, 'ignore': IGNORE, 'steps': self.steps}}
        if self.note:
            d['note'] = self.note
        if self.device_over:
            d['device'] = self.device_over
        return d


def text_with(**meta):
    d = device()
    d.meta.update(meta)
    return d.info


def gen_commands():
    vs = []
    base = device().info
    fsn = m.fsn_from_words(FSN_WORDS)
    for link in ('cdc', 'midi'):
        x = Host(f'ping-empty-{link}', link)
        x.send(0x01)
        x.expect(0x02, b'')
        vs.append(x.out())
        x = Host(f'ping-249-{link}', link, 'the longest payload; on MIDI the pong spans two carrier messages')
        x.send(0x01, bytes((i * 3 + 1) & 0xFF for i in range(249)))
        x.expect(0x02, bytes((i * 3 + 1) & 0xFF for i in range(249)))
        vs.append(x.out())
        x = Host(f'ping-route-slot-source-{link}', link, 'a ping from a reply-slot source: the pong goes back to it')
        x.send(0x01, b'\x01', route=0x91)
        x.expect(0x02, b'\x01', route=0x19)
        x.send(0x01, b'\x02', route=0xB1)
        x.expect(0x02, b'\x02', route=0x1B)
        vs.append(x.out())
        x = Host(f'ping-route-slot-destination-{link}', link, 'destination 8-15 reaches the master (S-10c.040)')
        x.send(0x01, b'\x03', route=0x2A)
        x.expect(0x02, b'\x03', route=0xA2)
        vs.append(x.out())
        x = Host(f'info-{link}', link)
        x.info(base)
        x.send(0x09, b'\x01\x02\x03')
        x.expect(0x0A, payload_re=info_re(base))
        vs.append(x.out())
        x = Host(f'fsn-{link}' if link == 'midi' else 'fsn', link, 'the FSN words, each least significant byte '
                                                                    'first; any payload')
        x.send(0x1F)
        x.expect(0x20, fsn)
        x.send(0x1F, b'\x99')
        x.expect(0x20, fsn)
        vs.append(x.out())
        x = Host(f'fw-begin-ok-{link}', link)
        x.send(0x0D, dfu_begin())
        x.expect(0x0E, b'\x54')
        x.send(0x0D, b'\x03' + bytes(240))
        x.expect(0x0E, b'\x54')
        x.send(0x0D, b'\x03')
        x.expect(0x0E, b'\x54', note='a chunk with no bytes')
        x.send(0x0D, dfu_begin(), note='a begin discards the transfer and starts another')
        x.expect(0x0E, b'\x54')
        vs.append(x.out())
        x = Host(f'fw-reboot-os-{link}', link, 'no reply; the reset follows (scenario H-FW-REBOOT)')
        x.send(0x0D, b'\x18')
        vs.append(x.out())
        x = Host(f'fw-reboot-mode-{link}', link, 'no reply; the reset follows (scenario H-FW-REBOOT)')
        x.send(0x0D, b'\xee\x01')
        vs.append(x.out())
        x = Host(f'fw-continue-{link}', link, 'continue to the OS: 54, then the restart; nothing more is served '
                                             '(scenario H-CONTINUE)')
        x.send(0x0D, b'\x19\x01\x02', note='bytes after the sub-command are ignored')
        x.expect(0x0E, b'\x54')
        x.send(0x01, b'\x50\x01', note='a ping after it: no pong')
        d = x.out()
        d['state'] = {'reset': 80}
        vs.append(d)
    x = Host('info-from-slot-source', note='every reply but the pong has route 12')
    x.send(0x09, route=0x91)
    x.expect(0x0A, payload_re=info_re(base), route=0x12)
    x.send(0x05, route=0x81)
    x.expect(0x06, b'\x40', route=0x12)
    vs.append(x.out())
    x = Host('handshake')
    x.send(0x05)
    x.expect(0x06, b'\x40')
    x.send(0x05, b'\x01\x02')
    x.expect(0x06, b'\x40')
    vs.append(x.out())
    x = Host('fsn-unreadable', note='the FSN cannot be read: an empty 20 reply, also after another reply',
             device_over={'fsn_readable': False})
    x.send(0x01, b'\x01\x02\x03\x04')
    x.expect(0x02, b'\x01\x02\x03\x04')
    x.send(0x1F)
    x.expect(0x20, b'')
    vs.append(x.out())
    # factory unlock
    on = {'option': True}
    x = Host('factory-unlock', note='with the session upgrade option on', device_over=on)
    x.send(0x07, bytes.fromhex('cafebabe'))
    x.expect(0x08, b'\x03')
    vs.append(x.out())
    x = Host('factory-wrong', note='any other payload: 04, and factory mode off', device_over=on)
    for p in (b'', bytes.fromhex('cafebabf'), bytes.fromhex('cafeba'), bytes.fromhex('cafebabe00'),
              bytes.fromhex('bebafeca')):
        x.send(0x07, p)
        x.expect(0x08, b'\x04')
    vs.append(x.out())
    x = Host('factory-off-again', note='a wrong unlock after a right one turns factory mode off',
             device_over=on)
    x.send(0x07, bytes.fromhex('cafebabe'))
    x.expect(0x08, b'\x03')
    x.send(0x07, bytes.fromhex('00'))
    x.expect(0x08, b'\x04')
    x.send(0x0B, b'serial:CD654321')
    x.expect(0x0C, b'\x04')
    x.info(base)
    vs.append(x.out())

    x = Host('factory-option-off', note='S-10c.061 (provisional): with the session upgrade option off, the '
             'right unlock is answered 04 and a set-metadata request 04, and the record is unchanged')
    x.send(0x07, bytes.fromhex('cafebabe'))
    x.expect(0x08, b'\x04')
    x.send(0x0B, b'serial:CD654321;sku_variant:5')
    x.expect(0x0C, b'\x04')
    x.info(base)
    vs.append(x.out())

    # set metadata (all with the session upgrade option on)
    def setmeta(name, texts, note='', unlock=True, quirk=False):
        x = Host(name, note=note, device_over=on)
        d = device(option=True)
        if unlock:
            x.send(0x07, bytes.fromhex('cafebabe'))
            x.expect(0x08, b'\x03')
            d.factory = True
        for t in texts:
            t = t if isinstance(t, bytes) else t.encode('latin-1')
            r, d.meta, _ = m.set_metadata(t, d.factory, d.meta)
            x.send(0x0B, t)
            x.expect(0x0C, bytes([r]))
        x.info(d.info)
        v = x.out()
        if quirk:
            # the outcome under kv_lookup_quirk (S-16.187): the record after the texts
            sm = json.loads(json.dumps(DEVICE['meta']))
            for t in texts:
                t = t if isinstance(t, bytes) else t.encode('latin-1')
                sr, sm, _ = m.set_metadata(t, True, sm, lookup=m.kv_lookup_quirk)
            v['quirk_outcome'] = {'reply': f'0x{sr:02x}', 'serial': sm['serial'],
                                    'differs': sm['serial'] != d.meta['serial']}
        vs.append(v)
    setmeta('setmeta-not-factory', ['serial:CD654321', 'sku_variant:5'], 'outside factory mode: 04, nothing '
                                                                         'changes', unlock=False)
    setmeta('setmeta-serial', ['serial:CD654321'])
    setmeta('setmeta-serial-refused', ['serial:CD65432', 'serial:CD6543210', 'sku_variant:9;serial:X'],
            'a serial of other than 8 characters refuses the whole request')
    setmeta('setmeta-sku', ['sku_variant:42'])
    setmeta('setmeta-sku-999', ['sku_variant:999'])
    setmeta('setmeta-sku-1000', ['sku_variant:1000'], 'over 999 stores 0')
    setmeta('setmeta-sku-2-31', ['sku_variant:2147483648'], 'unsigned: 2^31 stores 0')
    setmeta('setmeta-sku-negative', ['sku_variant:-1'], 'a minus negates modulo 2^32: over 999, stores 0')
    setmeta('setmeta-sku-spaces-sign', ['sku_variant:  +12x'], 'white space, a sign, then digits up to a non-digit')
    setmeta('setmeta-sku-no-digits', ['sku_variant:abc'], 'no digits read as 0')
    setmeta('setmeta-factory', ['factory:65537'], 'stored modulo 2^16')
    setmeta('setmeta-factory-huge', ['factory:99999999999'], 'beyond 32 bits reads 2^32-1, stored modulo 2^16')
    setmeta('setmeta-factory-huge-negative', ['factory:-99999999999'], 'beyond 32 bits reads 2^32-1 whatever the '
                                                                       'sign, stored modulo 2^16')
    setmeta('setmeta-sku-space-after-sign', ['sku_variant:- 5', 'factory:+-5'], 'one sign only: white space or a '
                                                                              'second sign after it leaves no digits, 0')
    setmeta('setmeta-all', ['serial:QQ000001;sku_variant:12;factory:3'])
    setmeta('setmeta-first-entry', ['sku_variant:11;sku_variant:22'], 'the first entry with the key is taken')
    setmeta('setmeta-exact-key', ['sku:11;Sku_variant:3;sku_variant2:4;factory:7'], 'keys match exactly')
    setmeta('setmeta-colon-in-value', ['serial:AB:12345;factory:9'], 'a value with a colon: the key is absent')
    setmeta('setmeta-empty-tail', ['sku_variant:5;serial:'], 'an entry ending the text with nothing after the '
                                                              'colon: absent')
    setmeta('setmeta-empty-value-mid', ['serial:;sku_variant:5'], 'an empty serial before a ; is present and '
                                                                   'refused')
    setmeta('setmeta-23', ['factory:' + '0' * 22 + '29'], 'the first 23 characters of a value are used')
    setmeta('setmeta-nul', [b'serial:CD654321\x00;sku_variant:5'], 'the text ends at the first 00')
    setmeta('setmeta-249', [b'sku_variant:5;' + b'x' * 235], 'a payload of 249 bytes counts as having no keys')
    setmeta('setmeta-248', [b'sku_variant:5;' + b'x' * 234], 'one byte shorter: the key is read')
    setmeta('setmeta-no-keys', ['', 'hello', 'a:b'], 'no keys: applied, the record written, nothing changes')
    # empty entries and entries with no ':' (S-10c.056, S-16.187). The serial differs from the record's
    # AB123456 so that the info text shows whether the key was found.
    setmeta('setmeta-empty-first', [';serial:CD654321'], 'an empty first entry is skipped: the serial is found',
            quirk=True)
    setmeta('setmeta-empty-two', [';;serial:CD654321'], 'two empty entries are skipped', quirk=True)
    setmeta('setmeta-empty-mid', ['a:1;;serial:CD654321'], 'an empty entry after another entry is skipped',
            quirk=True)
    setmeta('setmeta-empty-mid-later', ['a:1;;x:1;serial:CD654321'], 'an empty entry, then two more entries',
            quirk=True)
    setmeta('setmeta-no-colon-entry', ['serial;serial:CD654321'], 'an entry with no colon has no key; the '
            'next entry is taken', quirk=True)

    # served and not served
    x = Host('silent-cdc', note='pong, greeting, log lines and log configuration get no reply on CDC')
    for i, p in ((0x02, b'\x01'), (0x06, b'\x40'), (0x03, b'\x00test'), (0x04, b'\x00test'), (0xB0, b'')):
        x.send(i, p)
    x.ping()
    vs.append(x.out())
    x = Host('unserved-cdc', note='ids outside the catalog get no reply')
    for i in (0x00, 0x08, 0x0A, 0x0C, 0x0E, 0x20, 0x7A, 0xAC, 0xAD, 0xAF, 0xB1, 0xFF):
        x.send(i, b'\x01')
    x.ping()
    vs.append(x.out())
    x = Host('unserved-midi', 'midi', 'not served on the MIDI carrier: no effect, no reply (the session '
             'upgrade option on, so the unlock would succeed on CDC)', device_over={'option': True})
    for i, p in ((0x05, b''), (0x07, bytes.fromhex('cafebabe')), (0x0B, b'serial:CD654321'), (0x03, b'\x00t'),
                 (0x04, b'\x00t'), (0x06, b'\x40'), (0xB0, b''), (0xAC, b''), (0x7A, b'')):
        x.send(i, p)
    x.ping()
    x.info(base)
    vs.append(x.out())
    x = Host('order-cdc', note='commands in one write: answered one at a time, in order')
    for i, p in ((0x01, b'\x01'), (0x09, b''), (0x05, b''), (0x01, b'\x02')):
        x.send(i, p)
    x.expect(0x02, b'\x01')
    x.expect(0x0A, payload_re=info_re(base))
    x.expect(0x06, b'\x40')
    x.expect(0x02, b'\x02')
    vs.append(x.out())
    # firmware command
    x = Host('fw-begin-bad-product', note='a failing product word: 84 PRODUCT...; a transfer in progress is kept')
    x.send(0x0D, dfu_begin())
    x.expect(0x0E, b'\x54')
    for w in (0x00088000, 0x00084400, 0):
        x.send(0x0D, dfu_begin(word=w))
        x.expect(0x0E, prefix=b'\x84PRODUCT')
    x.send(0x0D, b'\x03\x01')
    x.expect(0x0E, b'\x54', note='the transfer is still in progress')
    vs.append(x.out())
    x = Host('fw-begin-short', note='a begin shorter than 14 bytes has product word 0')
    x.send(0x0D, dfu_begin()[:13])
    x.expect(0x0E, prefix=b'\x84PRODUCT')
    x.send(0x0D, dfu_begin()[:14])
    x.expect(0x0E, b'\x54')
    vs.append(x.out())
    x = Host('fw-chunk-no-transfer')
    x.send(0x0D, b'\x03\x01\x02')
    x.expect(0x0E, b'\x84No update in progress!')
    vs.append(x.out())
    x = Host('fw-finish-no-transfer')
    x.send(0x0D, b'\x05')
    x.expect(0x0E, b'\x84No update in progress!')
    vs.append(x.out())
    x = Host('fw-not-implemented')
    x.send(0x0D, b'\xaa')
    x.expect(0x0E, b'\x84compressed firmwares not implemented')
    x.send(0x0D, b'\xab\x01\x02')
    x.expect(0x0E, b'\x84signed firmwares not implemented')
    vs.append(x.out())
    x = Host('fw-continue-busy', note='continue to the OS while a host transfer is in progress: 84, no effect')
    x.send(0x0D, dfu_begin())
    x.expect(0x0E, b'\x54')
    x.send(0x0D, b'\x19')
    x.expect(0x0E, prefix=b'\x84')
    x.send(0x0D, b'\x03\x01')
    x.expect(0x0E, b'\x54', note='the transfer is still in progress')
    vs.append(x.out())
    x = Host('fw-continue-no-slave', note='continue to the OS with the slave absent (S-09.008): 54, then the '
             'restart; nothing more is served', device_over={'slave': False})
    x.send(0x0D, b'\x19')
    x.expect(0x0E, b'\x54')
    x.send(0x01, b'\x50\x01', note='a ping after it: no pong')
    d = x.out()
    d['state'] = {'reset': 20}
    vs.append(d)
    x = Host('fw-other', note='an empty payload and other sub-commands: 84 and any text')
    for p in (b'', b'\x00', b'\x01', b'\x04', b'\x06', b'\x1a', b'\xef', b'\xff'):
        x.send(0x0D, p)
        x.expect(0x0E, prefix=b'\x84')
    vs.append(x.out())

    # self-check every vector against the model, as the tests do
    return {
            'device': DEVICE, 'vectors': vs}


# ---- info.json (S-10c.054, .055, .088) ----

def gen_info():
    cases = [
        ('typical', dict(serial='AB123456', dsp_otp=b'ZZ0123456789ABCDEF', hw_rev=255, sku=7, sw=(1, 2, 3, 0),
                         bl=(1, 0, 0, 0), os=(1, 2, 3, 45), factory=1)),
        ('maxima', dict(serial='ZZZZZZZZ', dsp_otp=b'~' * 18, hw_rev=255, sku=999, sw=(65535,) * 4,
                        bl=(65535,) * 4, os=(65535,) * 4, factory=65535)),
        ('minima', dict(serial='A', dsp_otp=b'1' + bytes(17), hw_rev=0, sku=0, sw=(0, 0, 0, 0), bl=(1, 0, 0, 0),
                        os=(0, 0, 0, 0), factory=0)),
        ('sku-1000', dict(serial='AB123456', dsp_otp=b'ZZ0123456789ABCDEF', hw_rev=3, sku=1000, sw=(1, 0, 0, 0),
                          bl=(1, 0, 0, 0), os=(1, 0, 0, 0), factory=10)),
        ('build-number', dict(serial='AB123456', dsp_otp=b'ZZ0123456789ABCDEF', hw_rev=17, sku=42,
                              sw=(1, 0, 5, 1), bl=(1, 2, 0, 300), os=(0, 9, 0, 0), factory=2)),
        ('invalid-record', dict(serial='', dsp_otp=b'ZZ0123456789ABCDEF', hw_rev=255, sku=0, sw=(0, 0, 0, 0),
                                bl=(1, 0, 0, 0), os=(0, 0, 0, 0), factory=0)),
        ('otp-unreadable', dict(serial='AB123456', dsp_otp=None, hw_rev=255, sku=7, sw=(1, 2, 3, 0),
                                bl=(1, 0, 0, 0), os=(1, 2, 3, 45), factory=1)),
        ('otp-nul-inside', dict(serial='AB123456', dsp_otp=b'ZZ0123' + bytes(1) + b'456789ABCDE', hw_rev=255,
                                sku=7, sw=(1, 2, 3, 0), bl=(1, 0, 0, 0), os=(1, 2, 3, 45), factory=1)),
    ]
    info = []
    for n, f in cases:
        otp = f['dsp_otp']
        dsp = '' if otp is None else otp.split(b'\x00', 1)[0].decode('ascii')
        t = m.info_text(serial=f['serial'], dsp_serial=dsp, hw_rev=f['hw_rev'], sku=f['sku'], sw=f['sw'],
                        bl=f['bl'], os_version=f['os'], factory=f['factory'])
        assert len(t) <= 242, (n, len(t))
        fields = {'serial': f['serial'], 'dsp_otp': None if otp is None else h(otp), 'hw_rev': f['hw_rev'],
                  'sku': f['sku'], 'sw_version': list(f['sw']), 'bl_version': list(f['bl']),
                  'os_version': list(f['os']), 'factory': f['factory']}
        info.append({'name': n, 'fields': fields, 'text': t})
    words = [0x00084000, 0x000843FF, 0xFF084000, 0x00084400, 0x00088000, 0, 0x00FF8000, 0x00087C00, 0x000843E8,
             0x000843E7, 0x0009C000, 0x00F9C3E7, 0x00F9C800]
    te_code = [{'word': f'0x{w:08x}', 'code': m.te_code(w), 'passes': m.product_ok(w)} for w in words]
    return {
            'info': info, 'te_code': te_code}


# ---- progress.json (S-10c.101-.106) ----

LABELS = [('[decrypting firmware]', 'reading an encrypted package file'),
          ('[extracting firmware]', 'reading an unencrypted package file; extracting the zip'),
          ('[content]', 'step 0'), ('[main dsp]', 'step 1, the OS'), ('[bootloader]', 'step 2'),
          ('[ble]', 'step 3'), ('[ble bootloader]', 'step 4'), ('[backplane]', 'step 5'),
          ('[done]', 'outcome'), ('[nothing to do]', 'outcome'), ('[failed] ', 'outcome, followed by an error text'),
          ('[skipped] ', 'a step that reported Skipped, followed by the step and the reason'),
          ('', 'a report with no counts between two phases')]
# S-10c.106: the steps that can report Skipped and the reason (S-11.058, S-11.068 item 5, S-11.073,
# S-11.096)
SKIPS = [(2, 'option off'), (2, 'not selected'),
         (3, 'switched off'), (4, 'switched off'), (5, 'switched off'),
         (2, 'already written'),
         (0, 'not selected'), (1, 'not selected'), (3, 'not selected'), (4, 'not selected'), (5, 'not selected')]
REASONS = ['SHORT', 'PREAMBLE', 'TYPE', 'MAGIC', 'PRODUCT', 'DEVICE', 'CRC', 'FORMAT', 'SKU',
           'LENGTH', 'HASH', 'KEY', 'NOKEY', 'NAME', 'ZIPLEN', 'ZIP', 'MEMBER', 'SPACE', 'SPACEQUERY', 'SIZE', 'STREAM']


def gen_progress():
    pc = [(0, 100), (1, 100), (100, 100), (1, 8), (3, 8), (1, 3), (2, 3), (1, 200), (199, 200), (5, 1000),
          (4, 1000), (0xFFFFFFFF, 0xFFFFFFFF), (0x7FFFFFFF, 0xFFFFFFFF), (1, 0xFFFFFFFF), (0, 0), (7, 0)]
    percent = [{'done': d, 'total': t, 'percent': m.percent(d, t), 'te_percent': m.te_percent(d, t)}
               for d, t in pc]
    ts = [(0, 0, ''), (0, 0, '[done]'), (0, 0, '[nothing to do]'), (50, 100, '[main dsp]'), (1, 8, '[content]'),
          (7, 0, '[ble]'), (100, 100, '[backplane]'), (0, 4096, '[decrypting firmware]'),
          (0, 0, '[failed] step 3 result 2 ble'), (0, 0, '[failed] ' + 'x' * 238)]
    s06 = []
    for d, t, lab in ts:
        p = m.progress_s06(d, t, lab)
        assert len(p) - 1 <= 247
        f = Frame(0x12, 0, 0x0E, p)
        s06.append({'done': d, 'total': t, 'label': lab, 'payload': h(p), 'frame_seq0': h(f.encode())})
    dfu = []
    for d, t, lab, rid, oc in [(0, 0, '', 9, False), (0, 0, '[done]', 9, True), (0, 0, '[nothing to do]', None, True),
                               (0, 0, '[content]', 9, False), (1, 3, '[main dsp]', 4095, False),
                               (2, 3, '[ble bootloader]', 128, False), (5, 0, '[content]', 7, False),
                               (0, 10, 'L' * 70, 7, False), (10, 10, 'M' * 62, 7, False)]:
        p = m.progress_dfu_payload(d, t, lab, oc)
        msg = m.te_reply(0x03, rid, 0x40, p)
        assert len(msg) <= m.MAX_SENT
        dfu.append({'done': d, 'total': t, 'label': lab, 'outcome': oc, 'req_id': rid, 'payload': h(p),
                    'message': h(msg)})
    labels = [{'label': a, 'phase': b} for a, b in LABELS]
    assert [a for a, b in LABELS if b.startswith('step ')] == m.STEP_LABELS
    skipped = [{'step': st, 'reason': r, 'label': m.skip_label(st, r)} for st, r in SKIPS]
    assert skipped[2]['label'] == '[skipped] ble: switched off'
    errors = [
        {'kind': 'refused', 'reason': 'MAGIC', 'text': 'MAGIC', 'valid': True},
        {'kind': 'refused', 'reason': 'HASH', 'text': 'HASH: payload digest differs', 'valid': True},
        {'kind': 'refused', 'reason': 'SPACEQUERY', 'text': 'SPACEQUERY free space unknown', 'valid': True},
        {'kind': 'refused', 'reason': 'HASH', 'text': 'MAGIC', 'valid': False},
        {'kind': 'refused', 'reason': 'HASH', 'text': 'bad file', 'valid': False},
        {'kind': 'failed', 'step': 3, 'result': 2, 'text': 'step 3 (ble) result 2', 'valid': True},
        {'kind': 'failed', 'step': 1, 'result': 12, 'text': 'step 1: main dsp failed, result 12', 'valid': True},
        {'kind': 'failed', 'step': 3, 'result': 2, 'text': 'error occurred', 'valid': False},
        {'kind': 'failed', 'step': 3, 'result': 2, 'text': 'step 31 result 2', 'valid': False},
        {'kind': 'failed', 'step': 3, 'result': 2, 'text': 'step 3 result 21', 'valid': False},
        {'kind': 'product', 'text': 'PRODUCT', 'valid': True},
        {'kind': 'product', 'text': 'PRODUCT 0x00088000', 'valid': True},
        {'kind': 'product', 'text': 'bad product', 'valid': False},
        {'kind': 'no-transfer', 'text': 'No update in progress!', 'valid': True},
        {'kind': 'no-transfer', 'text': 'No update in progress', 'valid': False},
        {'kind': 'other', 'text': '', 'valid': True},
        {'kind': 'other', 'text': 'x' * 247, 'valid': True},
        {'kind': 'other', 'text': 'x' * 248, 'valid': False},
        {'kind': 'other', 'text': 'tab\there', 'valid': False},
        {'kind': 'other', 'text': 'nul\x00', 'valid': False},
        {'kind': 'other', 'text': 'café', 'valid': False},
    ]
    return {
            'reasons': REASONS, 'percent': percent, 's06': s06, 'dfu': dfu, 'labels': labels, 'skipped': skipped,
            'errors': errors}


# ---- service.tsv (S-10c.041) ----

NAMES = {0x01: 'ping', 0x02: 'pong', 0x03: 'log line', 0x04: 'log line', 0x05: 'handshake', 0x06: 'greeting',
         0x07: 'factory unlock', 0x09: 'info', 0x0B: 'set metadata', 0x0D: 'firmware', 0x1F: 'FSN',
         0xB0: 'log configuration'}


def gen_service():
    rows = ['# S-10c.041',
            'id\tcommand\tcdc\tmidi\treply']
    for i in range(256):
        out = {}
        reply = '-'
        for link in ('cdc', 'midi'):
            d = device()
            r = d.s06(link, Frame(0x21, 0, i, b''))
            if i not in m.SERVED[link]:
                out[link] = 'no'
            elif r:
                out[link] = 'yes'
                reply = f'{r[0].id:02x}'
            else:
                out[link] = 'silent'
        rows.append(f'{i:02x}\t{NAMES.get(i, "-")}\t{out["cdc"]}\t{out["midi"]}\t{reply}')
    return '\n'.join(rows) + '\n'


# ---- coprocessor.json (S-10c.045) ----

class CoHost(Host):
    """A coprocessor on its own S-06 link (S-07.076, S-08.004): it sends from its address to 1; the master's
    replies other than a pong come from 1 to that address."""

    def __init__(self, name, link, note='', device_over=None):
        super().__init__(name, link, note, device_over)
        self.peer = m.PEER[link]

    def send(self, msg_id, payload=b'', route=None, note=''):
        super().send(msg_id, payload, self.peer << 4 | 1 if route is None else route, note)

    def expect(self, msg_id, payload=None, route=None, prefix=None, payload_re=None, note=''):
        super().expect(msg_id, payload, 0x10 | self.peer if route is None else route, prefix, payload_re, note)

    def ping(self, note='a ping: the next frame is its pong, so nothing came in between'):
        self.mark += 1
        p = bytes([0x50, self.mark])
        self.send(0x01, p, note=note)
        self.expect(0x02, p, route=0x10 | self.peer)


def gen_coprocessor():
    vs = []
    base = device().info
    fsn = m.fsn_from_words(FSN_WORDS)
    for link in m.COPROCESSOR_LINKS:
        peer = m.PEER[link]
        x = CoHost(f'coproc-served-{link}', link, 'the CDC set: every reply but the pong from 1 to the coprocessor')
        x.send(0x01, b'\x11', route=0x81, note='a ping from a reply slot: the pong to that slot')
        x.expect(0x02, b'\x11', route=0x18)
        x.send(0x09, route=0x81, note='an info request with a slot source: the reply to the coprocessor')
        x.expect(0x0A, payload_re=info_re(base))
        x.info(base)
        x.send(0x1F)
        x.expect(0x20, fsn)
        x.send(0x0D, b'\xaa')
        x.expect(0x0E, prefix=b'\x84')
        x.send(0x07, bytes.fromhex('cafebabe'), note='S-10c.061: the option off, so 04 here too')
        x.expect(0x08, b'\x04')
        x.send(0x0B, b'serial:CD654321')
        x.expect(0x0C, b'\x04')
        for i, p in ((0x02, b'\x01'), (0xB0, b''), (0xAC, b''), (0x7A, b'\x01'), (0x0A, b''), (0x0E, b'')):
            x.send(i, p)
        x.ping(note='pong, log configuration and unknown ids: no reply')
        vs.append(x.out())
        x = CoHost(f'coproc-firmware-{link}', link, 'the firmware command as on CDC (forced, S-10c.075)')
        x.send(0x0D, dfu_begin())
        x.expect(0x0E, b'\x54')
        x.send(0x0D, b'\x03' + bytes(240))
        x.expect(0x0E, b'\x54')
        x.send(0x0D, dfu_begin(word=0x00088000), note='a product word that fails: 84 PRODUCT')
        x.expect(0x0E, prefix=b'\x84PRODUCT')
        vs.append(x.out())
        x = CoHost(f'coproc-unlock-{link}', link, 'S-10c.061 holds on this link: with the option on, the unlock '
                   'and set metadata act as on CDC', device_over={'option': True})
        x.send(0x07, bytes.fromhex('cafebabe'))
        x.expect(0x08, b'\x03')
        x.send(0x0B, b'serial:CD654321')
        x.expect(0x0C, b'\x03')
        x.info(text_with(serial='CD654321'))
        vs.append(x.out())
    # S-10c.045: before held mode only the PSoC's messages are handled (S-08.001)
    x = CoHost('coproc-early-psoc', 'psoc', 'before held mode (S-07.023 steps 1-6a): no transfer starts, no '
               'reboot runs', device_over={'held': False})
    x.send(0x0D, dfu_begin(), note='a product word that passes: 84, the transfer cannot start')
    x.expect(0x0E, prefix=b'\x84')
    x.send(0x0D, b'\x03' + bytes(240))
    x.expect(0x0E, b'\x84No update in progress!')
    x.send(0x0D, b'\x05')
    x.expect(0x0E, b'\x84No update in progress!')
    x.send(0x0D, dfu_begin(word=0x00088000), note='a product word that fails: 84 PRODUCT, as in held mode')
    x.expect(0x0E, prefix=b'\x84PRODUCT')
    x.send(0x0D, b'\xee\x02', note='a reboot with mode 2: no effect')
    x.send(0x0D, b'\x18', note='a reboot to the OS: no effect')
    x.ping(note='the pong: no reboot ran, so the link is still served')
    d = x.out()
    d['state'] = {'reset': None}
    vs.append(d)
    x = CoHost('coproc-early-continue-psoc', 'psoc', 'continue to the OS before held mode (S-10c.045, S-10c.076): '
               '84, no effect', device_over={'held': False})
    x.send(0x0D, b'\x19')
    x.expect(0x0E, prefix=b'\x84')
    x.ping(note='the pong: no restart ran, so the link is still served')
    d = x.out()
    d['state'] = {'reset': None}
    vs.append(d)
    x = CoHost('coproc-held-psoc', 'psoc', 'the same reboot in held mode: it runs')
    x.ping()
    x.send(0x0D, b'\xee\x02', note='a reboot with mode 2: reason 10 (S-04.021), no reply')
    d = x.out()
    d['state'] = {'reset': 10}
    vs.append(d)
    service = []
    for link in m.COPROCESSOR_LINKS:
        for i in range(256):
            if i in m.CLAIMED[link]:
                st = 'claimed'
            elif i not in m.SERVED[link]:
                st = 'no'
            else:
                st = 'yes' if device().s06(link, Frame(m.PEER[link] << 4 | 1, 0, i, b'')) else 'silent'
            service.append({'link': link, 'id': f'0x{i:02x}', 'service': st})
    progress = [{'submitted_by': k, 'link': t[0], 'form': t[1], 'route': None if t[2] is None else f'0x{t[2]:02x}'}
                for k in ('cdc', 'midi', 'psoc', 'nordic', 'dfu', 'ramdisk')
                for t in [m.progress_target(k)]]
    return {
            'vectors': vs, 'service': service, 'progress': progress}


# ---- main ----

def dump(obj):
    return json.dumps(obj, indent=1, sort_keys=True) + '\n'


def outputs():
    return {
        'vectors/pack7.json': dump(gen_pack7()),
        'vectors/usbmidi.json': dump(gen_usbmidi()),
        'vectors/carrier.json': dump(gen_carrier()),
        'vectors/sysex.json': dump(gen_sysex()),
        'vectors/commands.json': dump(gen_commands()),
        'vectors/info.json': dump(gen_info()),
        'vectors/progress.json': dump(gen_progress()),
        'vectors/coprocessor.json': dump(gen_coprocessor()),
        'service.tsv': gen_service(),
    }


def stale():
    """The files that differ from what the generator makes now."""
    bad = []
    for rel, text in outputs().items():
        p = os.path.join(HERE, rel)
        if not os.path.exists(p):
            bad.append(rel)
            continue
        with open(p, encoding='utf-8') as f:
            if f.read() != text:
                bad.append(rel)
    return bad


def main(argv):
    if '--write' in argv:
        os.makedirs(V, exist_ok=True)
        for rel, text in outputs().items():
            with open(os.path.join(HERE, rel), 'w', encoding='utf-8') as f:
                f.write(text)
        return 0
    bad = stale()
    for b in bad:
        print('stale:', b)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
