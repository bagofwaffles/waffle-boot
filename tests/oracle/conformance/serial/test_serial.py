# SPDX-License-Identifier: GPL-2.0-or-later
"""Self-test of the serial conformance package (unittest)."""
import json
import os
import random
import unittest

from . import cobs, vectors
from .bridge import BridgeReply, BridgeRequest
from .crc8 import TABLE, crc8, crc8_bitwise
from .frame import Frame, FrameError, MAX_PAYLOAD, decode, decode_stream, swap_route
from .link import Node, Sender, reply_slot
from .receiver import Receiver
from .transcript import (BridgeResponder, Expect, Inject, Match, Script, S06Responder, TranscriptError,
                         load_s06_script, reply_to)

HERE = os.path.dirname(os.path.abspath(__file__))


class Crc(unittest.TestCase):
    def test_table_matches_bitwise(self):
        rng = random.Random(1)
        for _ in range(500):
            d = rng.randbytes(rng.randrange(0, 260))
            self.assertEqual(crc8(d), crc8_bitwise(d))

    def test_table_is_a_permutation(self):
        self.assertEqual(len(set(TABLE)), 256)

    def test_whitening_is_per_byte(self):
        # with whitening 0xA5 the check of n zero bytes is the plain CRC of n 0xA5 bytes
        from . import crc8 as m
        for n in range(1, 8):
            c = 0
            for _ in range(n):
                c = m._register_step(c ^ 0xA5)
            self.assertEqual(crc8(bytes(n)), c)

    def test_chaining(self):
        d = bytes(range(40))
        self.assertEqual(crc8(d), crc8(d[20:], crc8(d[:20])))


class Cobs(unittest.TestCase):
    # the examples of the published COBS description (input -> encoded, without the delimiter)
    KNOWN = [
        ('00', '0101'), ('0000', '010101'), ('001100', '01021101'),
        ('11220033', '0311220233'), ('11223344', '0511223344'), ('11000000', '0211010101'),
    ]

    def test_known(self):
        for raw, enc in self.KNOWN:
            self.assertEqual(cobs.encode(bytes.fromhex(raw)).hex(), enc, raw)
            self.assertEqual(cobs.decode(bytes.fromhex(enc)).hex(), raw, enc)

    def test_long_runs(self):
        a = bytes(range(1, 255))                    # 254 non-zero bytes
        self.assertEqual(cobs.encode(a), b'\xff' + a)
        self.assertEqual(cobs.encode(b'\x00' + a), b'\x01\xff' + a)
        b = bytes(range(1, 256))                    # 255 non-zero bytes
        self.assertEqual(cobs.encode(b), b'\xff' + b[:254] + b'\x02\xff')
        for x in (a, b'\x00' + a, b, a + b'\x00'):
            self.assertEqual(cobs.decode(cobs.encode(x)), x)

    def test_roundtrip_random(self):
        rng = random.Random(2)
        for _ in range(2000):
            d = bytes(0 if rng.random() < 0.2 else rng.randrange(1, 256) for _ in range(rng.randrange(0, 600)))
            e = cobs.encode(d)
            self.assertNotIn(0, e)
            self.assertEqual(cobs.decode(e), d)

    def test_errors(self):
        for enc, why in ((b'', 'empty'), (b'\x05\x01\x02', 'truncated-group'), (b'\x03\x01\x00', 'zero-in-data')):
            with self.assertRaises(cobs.CobsError) as c:
                cobs.decode(enc)
            self.assertEqual(c.exception.reason, why)


class Frames(unittest.TestCase):
    def test_roundtrip_random(self):
        rng = random.Random(3)
        for _ in range(3000):
            n = rng.choice([0, 1, 2, 5, 60, 248, 249, rng.randrange(250)])
            f = Frame(rng.randrange(256), rng.randrange(256), rng.randrange(256),
                      bytes(0 if rng.random() < 0.15 else rng.randrange(1, 256) for _ in range(n)))
            w = f.encode()
            self.assertEqual(w.count(0), 1)
            self.assertEqual(w[-1], 0)
            self.assertEqual(decode(w), f)
            with self.assertRaises(FrameError) as c:
                decode(f.encode(bad_crc=True))
            self.assertEqual(c.exception.reason, 'crc')

    def test_limits(self):
        with self.assertRaises(ValueError):
            Frame(0x12, 0, 1, bytes(MAX_PAYLOAD + 1)).encode()
        self.assertEqual(len(Frame(0x12, 0, 1, bytes(MAX_PAYLOAD)).raw()), 254)

    def test_stream(self):
        a, b = Frame(0x12, 0, 1, b'a'), Frame(0x12, 1, 2, b'')
        got, tail = decode_stream(a.encode() + b'\x00' + b.encode() + b'\x02\x12')
        self.assertEqual([g for g, _ in got], [a, b])
        self.assertEqual(tail, b'\x02\x12')


class Vectors(unittest.TestCase):
    def setUp(self):
        self.doc = json.load(open(os.path.join(HERE, 'vectors', 'frames.json')))

    def test_file_is_current(self):
        self.assertEqual(vectors.main([]), 0)

    def test_each_vector(self):
        n_ok = n_rej = 0
        for v in self.doc['vectors']:
            wire = bytes.fromhex(v['wire'])
            first = vectors.first_piece(wire)
            if v['frame'] is not None and v['decode'] == 'ok':
                f = v['frame']
                fr = Frame(f['route'], f['seq'], f['id'], bytes.fromhex(f['payload']))
                self.assertEqual(decode(first), fr, v['name'])
                self.assertTrue(wire.lstrip(b'\x00').startswith(fr.encode()), v['name'])
                n_ok += 1
            elif v['decode'] != 'ok':
                with self.assertRaises(FrameError, msg=v['name']) as c:
                    decode(first)
                self.assertEqual(c.exception.reason, v['decode'], v['name'])
                n_rej += 1
            for key, rx in (('receiver', Receiver), ('factory', Receiver.factory)):
                self._events(v, key, rx(v['address'], v['reply_mask']).feed(wire), wire)
            if 'quirk' in v and v['quirk'] != 'Q-EARLYDONE':
                self.assertNotEqual(v['receiver'], v['factory'], v['name'])
            else:
                self.assertEqual(v['receiver'], v['factory'], v['name'])
        self.assertGreater(n_ok, 15)
        self.assertGreater(n_rej, 5)

    def _events(self, v, key, evs, wire):
        self.assertEqual([e.kind for e in evs], [e[0] for e in v[key]], (v['name'], key))
        for e, want in zip(evs, v[key]):
            if e.kind == 'message':
                w = want[1]
                self.assertEqual(e.frame, Frame(w['route'], w['seq'], w['id'], bytes.fromhex(w['payload'])))
            if e.kind == 'forward' and v['name'].startswith('forward-') and v['name'] != 'forward-overflow':
                self.assertEqual(e.data, wire, v['name'])


class ReceiverModel(unittest.TestCase):
    def test_agrees_with_strict_decoder_on_well_formed_streams(self):
        """For streams of well-formed frames (route != 0), the byte-wise receiver and split-and-decode
        agree: local frames delivered, others forwarded verbatim, bad checks dropped."""
        rng = random.Random(4)
        me = 2
        for _ in range(300):
            stream, want = bytearray(), []
            for _ in range(rng.randrange(1, 8)):
                dst = rng.choice([me, me, 8, 11, 15, 1, 3, 4])
                f = Frame(rng.randrange(1, 16) << 4 | dst, rng.randrange(256), rng.randrange(256),
                          bytes(0 if rng.random() < 0.2 else rng.randrange(1, 256)
                                for _ in range(rng.choice([0, 1, 9, 249, rng.randrange(250)]))))
                bad = rng.random() < 0.2
                w = f.encode(bad_crc=bad)
                stream += b'\x00' * rng.choice([0, 0, 1]) + w
                if dst == me or dst & 8:
                    want.append(('crc', None) if bad else ('message', f))
                else:
                    want.append(('forward', w))
            got = [(e.kind, e.frame if e.kind == 'message' else (e.data if e.kind == 'forward' else None))
                   for e in Receiver(me).feed(stream)]
            self.assertEqual(got, want)

    def test_split_feeding(self):
        f = Frame(0x12, 9, 0x41, bytes(range(30)))
        r = Receiver(2)
        evs = []
        for b in f.encode():
            evs += r.feed(bytes([b]))
        self.assertEqual([e.frame for e in evs], [f])


class Links(unittest.TestCase):
    def test_sender(self):
        s = Sender(1, 4, seq=254)
        a, b, c = s.message(0x7e, b'\x01'), s.request(0x7a, b'\x0a\x0b', slot=2), s.message(0x7c, dst=5)
        self.assertEqual((a.route, a.seq), (0x14, 254))
        self.assertEqual((b.route, b.seq), (0xa4, 255))
        self.assertEqual((c.route, c.seq), (0x15, 0))
        self.assertEqual(swap_route(b.route), 0x4a)
        self.assertEqual(reply_slot(swap_route(b.route) & 0xF), 2)
        self.assertIsNone(reply_slot(12))                # S-06.034
        self.assertEqual(reply_slot(12, slot12=True), 0)
        self.assertIsNone(reply_slot(13, slot12=True))

    def test_node_forwarding(self):
        n = Node({'midi': (1, 2), 'cpu': (1, 5), 'cdc': (1, 2), 'psoc': (1, 4)})
        n.set_forward('cdc', 4, 'psoc')
        to_psoc = Frame(0x24, 0, 0x7e, b'\x01\x02').encode()
        self.assertEqual(n.receive('cdc', to_psoc), [('forward', ('psoc', to_psoc))])
        to_nowhere = Frame(0x23, 0, 0x7c).encode()
        self.assertEqual(n.receive('cdc', to_nowhere)[0][0], 'no-route')
        mine = Frame(0x41, 0, 0x7d, b'\x00')
        self.assertEqual(n.receive('psoc', mine.encode()), [('message', mine)])
        self.assertEqual(n.inbox['psoc'], [mine])

    def test_node_requests_and_timeouts(self):
        n = Node({'psoc': (1, 4)})
        s0 = n.request('psoc', 0x7a, timeout=0.5, now=10.0)
        s1 = n.request('psoc', 0x7a, timeout=2.0, now=10.0)
        self.assertEqual((s0, s1), (0, 1))
        self.assertEqual(n.expire(10.6), [('psoc', 0)])
        late = Frame(0x48, 0, 0x7b)                       # the late reply to slot 0
        self.assertEqual(n.receive('psoc', late.encode()), [('message', late)])
        rep = Frame(0x49, 0, 0x7b)
        self.assertEqual(n.receive('psoc', rep.encode()), [('reply', (1, rep))])
        self.assertEqual(n.expire(100.0), [])
        # destination 12: an ordinary message by default, slot 0 with factory=True
        for factory, want in ((False, 'message'), (True, 'reply')):
            n = Node({'psoc': (1, 4)}, factory=factory)
            n.request('psoc', 0x7a)
            self.assertEqual(n.receive('psoc', Frame(0x4c, 0, 0x7b).encode())[0][0], want)

    def test_node_log_and_raw(self):
        n = Node({'psoc': (1, 4)}, room=12)
        self.assertEqual(n.send('psoc', 0x7c).seq, 0)        # 6 bytes
        self.assertIsNone(n.log('psoc', b'\x00' * 10))      # no room: no number (S-14.027)
        self.assertEqual(n.send('psoc', 0x7c).seq, 1)
        self.assertEqual(n.send('psoc', 0x7c).seq, 2)        # not sent: the number is used
        n.out['psoc'].clear()
        n.set_raw('psoc', True)
        self.assertIsNone(n.log('psoc', b'x'))
        self.assertEqual(n.receive('psoc', b'\x01\x02\x00'), [('raw', b'\x01\x02\x00')])
        n.set_raw('psoc', False)
        self.assertEqual(n.log('psoc', b'x').seq, 3)


class Transcripts(unittest.TestCase):
    def _decode(self, data, me=1):
        return [e.frame for e in Receiver(me).feed(data) if e.kind == 'message']

    def test_psoc_example(self):
        sc = load_s06_script(open(os.path.join(HERE, 'examples', 'psoc_greeting.json')).read())
        resp = S06Responder(sc, check_seq=True)
        dut = Sender(1, 4)
        self.assertEqual(resp.start(), b'')
        log = dut.message(0x70, b'\x00\x00\x00\x00text\n')            # ignored
        out = resp.feed(log.encode() + dut.request(0x7a, b'\x0a\x0b', slot=0).encode())
        (rep,) = self._decode(out)
        self.assertEqual((rep.route, rep.id, rep.payload), (0x48, 0x7b, b'\x01'))
        out = resp.feed(dut.message(0x7c, b'\x01\x02').encode())
        (inj,) = self._decode(out)
        self.assertEqual((inj.route, inj.id), (0x41, 0x7d))
        sc.check()
        self.assertIn('ignored', sc.render())

    def test_nordic_example_peer_first(self):
        sc = load_s06_script(open(os.path.join(HERE, 'examples', 'nordic_handshake.json')).read())
        resp = S06Responder(sc)
        (hs,) = self._decode(resp.start())
        self.assertEqual(hs.id, 0x7a)
        resp.feed(Sender(1, 3).message(0x7b, b'\xdd').encode())
        sc.check()

    def test_failures_are_reported(self):
        sc = Script([Expect(Match(id=0x7a), reply_to(0x7b))], name='psoc')
        resp = S06Responder(sc, check_seq=True)
        dut = Sender(1, 4)
        resp.feed(dut.message(0x7c).encode())                           # wrong id
        resp.feed(b'\x05\x12\x01\x00')                                  # malformed
        with self.assertRaises(TranscriptError) as c:
            sc.check()
        msg = str(c.exception)
        self.assertIn('unexpected', msg)
        self.assertIn('malformed', msg)
        self.assertIn('stopped at step 0', msg)
        sc2 = Script([Expect(Match(id=1)), Expect(Match(id=1))])
        r2 = S06Responder(sc2, check_seq=True)
        r2.feed(Frame(0x14, 5, 1).encode() + Frame(0x14, 7, 1).encode())  # seq skips
        with self.assertRaises(TranscriptError):
            sc2.check()

    def test_bridge_with_tunnel(self):
        # synthetic ids throughout: a command answered with a word, then an echo through the serial tunnel
        INIT, PUSH, PULL, STATUS = 0x71, 0x72, 0x73, 0x74
        inner = Script([Expect(Match(id=0x7a), reply_to(0x7b, b'hi'))], name='cpu-tunnel')
        tunnel = S06Responder(inner)
        sc = Script([Expect(Match(cmd=INIT), lambda q: BridgeReply.word(q, 0x0badcafe)),
                     Expect(Match(cmd=STATUS), BridgeReply.header_only(STATUS, 0xff))], name='bridge')
        br = BridgeResponder(sc, tunnel=tunnel, push_cmds=(PUSH,), pull_cmd=PULL)
        rep = BridgeReply.decode(br.feed_record(BridgeRequest(INIT, slot=1).encode()))
        self.assertEqual((rep.cmd, rep.slot, rep.body), (INIT, 1, (0x0badcafe).to_bytes(4, 'little')))
        ping = Sender(1, 2).message(0x7a, b'hi')
        self.assertEqual(br.feed_record(BridgeRequest(PUSH, 0, payload=ping.encode()).encode()), b'')
        pull = BridgeReply.decode(br.feed_record(BridgeRequest(PULL, 2, length=0x100).encode()))
        n = int.from_bytes(pull.body[:4], 'little')
        (pong,) = self._decode(pull.body[4:4 + n])
        self.assertEqual((pong.route, pong.id, pong.payload), (0x21, 0x7b, b'hi'))
        self.assertEqual(len(pull.body) % 4, 0)
        self.assertEqual(br.feed_record(BridgeRequest(STATUS).encode()), bytes([STATUS, 0xff, 0, 0]))
        sc.check()
        inner.check()

    def test_bridge_record_codec(self):
        q = BridgeRequest(0x75, 3, 0x1, 0x2, b'/a/b\x00')
        e = q.encode()
        self.assertEqual(len(e), 12 + 8)
        self.assertEqual(e[:4], bytes([0x75, 3, 5, 0]))
        d = BridgeRequest.decode(e)
        self.assertEqual((d.cmd, d.slot, d.len_field, d.arg0, d.arg1, d.payload[:5]), (0x75, 3, 5, 1, 2, b'/a/b\x00'))
        r = BridgeReply.io(q, result=0xffffffff, errno=2, data=b'xy')
        self.assertEqual(r.encode(), bytes([0x75, 3, 0, 0]) + b'\xff' * 4 + b'\x02\x00\x00\x00xy')

    def test_inject_mid_script(self):
        sc = Script([Expect(Match(id=1)), Inject(Frame(0x41, 0, 0x7d, b'\x01')), Expect(Match(id=2))])
        r = S06Responder(sc)
        out = r.feed(Frame(0x14, 0, 1).encode())
        self.assertEqual(self._decode(out)[0].id, 0x7d)
        r.feed(Frame(0x14, 1, 2).encode())
        sc.check()


if __name__ == '__main__':
    unittest.main()
