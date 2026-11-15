# SPDX-License-Identifier: GPL-2.0-or-later
"""Self-tests of the integration additions: receiver switches, responder raw mode, forwarded frames,
cross-link and delayed replies, stalls, order checks, the log-line codec, rule peers and the Cypress
bootloader model. Synthetic ids and payloads throughout."""
import unittest

from . import cypress, logline
from .frame import Frame, decode_stream
from .link import Node
from .peers import RulePeer
from .receiver import Receiver, FACTORY
from .transcript import (Delayed, Expect, ForwardLedger, Inject, Match, OnLink, Peers, Raw, RawBytes,
                         Script, S06Responder, TranscriptError, assert_order, in_order,
                         occurs_before, reply_to)


def frames(data, me=1):
    return [e.frame for e in Receiver(me).feed(data) if e.kind == 'message']


class Switches(unittest.TestCase):
    def test_route0(self):
        w = Frame(0x00, 0, 0x71).encode()
        self.assertEqual([e.kind for e in Receiver(1).feed(w)], ['forward'])
        self.assertEqual([e.kind for e in Receiver.factory(1).feed(w)], ['message'])
        self.assertEqual([e.kind for e in Receiver(1, route0='code-byte').feed(w)], ['message'])

    def test_early_delim(self):
        w = b'\x03\x00' + Frame(0x21, 0, 0x71).encode()
        self.assertEqual([e.kind for e in Receiver(1).feed(w)], ['partial', 'message'])
        self.assertEqual([e.kind for e in Receiver(1, **FACTORY).feed(w)], ['forward'])
        self.assertEqual([e.kind for e in Receiver(1, early_delim='collect').feed(w)], ['forward'])

    def test_early_done_off(self):
        from . import cobs
        raw = Frame(0x21, 0, 0x71, b'\x07').raw()
        self.assertEqual([e.kind for e in Receiver(1).feed(cobs.encode(raw + b'\x33\x41') + b'\x00')],
                         ['message', 'partial'])
        self.assertEqual([e.kind for e in Receiver(1, early_done=False).feed(cobs.encode(raw + b'\x33\x41')
                                                                              + b'\x00')], ['length'])
        self.assertEqual([e.kind for e in Receiver(1, early_done=False).feed(cobs.encode(raw) + b'\x00')],
                         ['message'])

    def test_bad_option(self):
        with self.assertRaises(ValueError):
            Receiver(1, route0='x')


class Responder(unittest.TestCase):
    def test_forwarded_left_out_of_seq_check(self):
        led = ForwardLedger()
        fwd = Frame(0x24, 200, 0x7e, b'\x01')
        led.put(fwd.encode())
        sc = Script([Expect(Match(id=0x7a)), Expect(Match(id=0x7c))])
        peer = S06Responder(sc, check_seq=True, forwarded=led)
        peer.feed(Frame(0x14, 5, 0x7a).encode() + fwd.encode() + Frame(0x14, 6, 0x7c).encode())
        sc.check()
        self.assertEqual(peer.forwarded_frames, [fwd])
        # without the ledger the copy breaks the check
        sc = Script([Expect(Match(id=0x7a)), Expect(Match(id=0x7c))], ignore=Match(id=0x7e))
        peer = S06Responder(sc, check_seq=True)
        peer.feed(Frame(0x14, 5, 0x7a).encode() + fwd.encode() + Frame(0x14, 6, 0x7c).encode())
        self.assertRaises(TranscriptError, sc.check)

    def test_raw_mode(self):
        # a Raw step after the expect: the bytes after that frame are raw, in the same feed
        sc = Script([Expect(Match(id=0x7a), reply_to(0x7b)), Raw(True), Expect(Match(id=0x7c))])
        peer = S06Responder(sc, raw_handler=lambda b: b'\xaa' if b.endswith(b'\x17') else b'')
        out = peer.feed(Frame(0x14, 0, 0x7a).encode() + b'\x01\x02')
        self.assertEqual(frames(out), [Frame(0x41, 0, 0x7b)])
        self.assertTrue(peer.raw)
        self.assertEqual(peer.feed(b'\x00\x17'), b'\xaa')
        self.assertEqual(bytes(peer.raw_rx), b'\x01\x02\x00\x17')
        peer.set_raw(False)
        peer.feed(Frame(0x14, 1, 0x7c).encode())
        sc.check()

    def test_raw_handler_leaves_raw(self):
        sc = Script([Raw(True), Expect(Match(id=0x7c))])
        peer = S06Responder(sc, raw_handler=lambda b: [RawBytes(b'ok'), Raw(False)] if b'\x17' in b else [])
        peer.start()
        self.assertTrue(peer.raw)
        self.assertEqual(peer.feed(b'\x05\x17'), b'ok')
        self.assertFalse(peer.raw)
        peer.feed(Frame(0x14, 1, 0x7c).encode())
        sc.check()

    def test_cross_link_and_other_slot(self):
        psoc = S06Responder(Script([Expect(Match(id=0x7a), [OnLink('cdc', Frame(0x12, 0, 0x7d)),
                                                               reply_to(0x7b), reply_to(0x7b, slot=3)])]))
        cdc = S06Responder(Script([]))
        peers = Peers({'psoc': psoc, 'cdc': cdc})
        out = peers.feed('psoc', Frame(0x94, 0, 0x7a).encode())
        self.assertEqual(frames(out['psoc']), [Frame(0x49, 0, 0x7b), Frame(0x4b, 0, 0x7b)])
        self.assertEqual(frames(out['cdc'], me=2), [Frame(0x12, 0, 0x7d)])
        peers.check()

    def test_delayed_and_stall(self):
        sc = Script([Expect(Match(id=0x7a), Delayed(Frame(0x41, 0, 0x7b), 0.25))])
        peer = S06Responder(sc)
        self.assertEqual(peer.feed(Frame(0x14, 0, 0x7a).encode(), now=1.0), b'')
        self.assertEqual(peer.poll(1.2), b'')
        self.assertEqual(frames(peer.poll(1.25)), [Frame(0x41, 0, 0x7b)])
        sc = Script([Expect(Match(id=0x7a), reply_to(0x7b))])
        peer = S06Responder(sc)
        peer.stall()
        self.assertEqual(peer.feed(Frame(0x14, 0, 0x7a).encode()), b'')
        self.assertFalse(sc.done)
        self.assertEqual(frames(peer.resume()), [Frame(0x41, 0, 0x7b)])
        sc.check()

    def test_inject_bytes_from_json(self):
        from .transcript import load_s06_script
        sc = load_s06_script({'steps': [{'inject': [{'bytes': '0300'}]}, {'raw': True},
                                             {'expect': {'id': '0x7a'}, 'reply': [{'id': '0x7b', 'slot': 1,
                                                                                    'delay': 0}]}]})
        peer = S06Responder(sc)
        self.assertEqual(peer.start(), b'\x03\x00')
        self.assertTrue(peer.raw)


class Order(unittest.TestCase):
    def test_order(self):
        s = [Frame(0x14, i, x) for i, x in enumerate([0x70, 0x71, 0x72, 0x71])]
        a, b, c = Match(id=0x70), Match(id=0x71), Match(id=0x72)
        self.assertTrue(occurs_before(s, a, b))
        self.assertFalse(occurs_before(s, c, b))
        self.assertFalse(occurs_before(s, a, Match(id=0x7f)))
        self.assertEqual(in_order(s, a, c, b), [0, 2, 3])
        self.assertIsNone(in_order(s, c, a))
        assert_order(b''.join(f.encode() for f in s), a, c)
        with self.assertRaises(TranscriptError) as e:
            assert_order(s, c, a, name='psoc')
        self.assertIn('matches 0', str(e.exception))

    def test_decode_stream_skip(self):
        a, b = Frame(0x14, 0, 0x70).encode(), Frame(0x14, 1, 0x71).encode()
        stream = a + b[:3] + b'\x01\x02\x00\x17' + b
        got, tail = decode_stream(stream, skip=[(len(a) + 3, len(a) + 7)])
        self.assertEqual([f for f, _ in got], [Frame(0x14, 0, 0x70), Frame(0x14, 1, 0x71)])


class LogLine(unittest.TestCase):
    def test_round_trip_and_filter(self):
        line = logline.build_line(2 ** 32 + 5, b'hello\n', 'boot', 12)
        self.assertEqual(line, b'\x00\x00\x00\x05boot[12]: hello')
        p = logline.parse_line(line)
        self.assertEqual((p['timestamp'], p['name'], p['line_no'], p['text']), (5, 'boot', 12, b'hello'))
        cut = logline.build_line(0, b'x' * 300)
        self.assertEqual((len(cut), cut[-2:]), (248, b'..'))
        n = Node({'psoc': (1, 4)})
        n.log('psoc', line)
        n.send('psoc', 0x70)
        n.log('psoc', line, id=0x04)
        logs, other = logline.split_logs(bytes(n.out['psoc']))
        self.assertEqual([f.id for f in logs], [0x03, 0x04])
        self.assertEqual([f.id for f in other], [0x70])
        self.assertEqual(logline.log_lines(logs)[0][1]['text'], b'hello')
        self.assertTrue(logline.log_matcher()(logs[0]))


class Rules(unittest.TestCase):
    def test_rule_peer(self):
        peer = RulePeer(name='nordic')
        info = peer.on(Match(id=0x70), reply_to(0x70, b'v1'), delay=0.5, required=True)
        peer.greet(Frame(0x31, 0, 0x75, b'\x00\x9c'), poll=Match(id=0x71), at_poll=2, repeat=2)
        r = S06Responder(peer)
        self.assertEqual(r.feed(Frame(0x13, 0, 0x70).encode(), now=0.0), b'')
        self.assertEqual(frames(r.poll(0.5)), [Frame(0x31, 0, 0x70, b'v1')])
        got = [frames(r.feed(Frame(0x13, i, 0x71).encode())) for i in range(5)]
        self.assertEqual([len(g) for g in got], [0, 0, 1, 1, 0])
        self.assertEqual(info.fired, 1)
        peer.check()
        strict = RulePeer(strict=True)
        S06Responder(strict).feed(Frame(0x13, 0, 0x7f).encode())
        self.assertRaises(TranscriptError, strict.check)


class Cypress(unittest.TestCase):
    def test_packets(self):
        p = cypress.packet(0x38)
        self.assertEqual(p, bytes([0x01, 0x38, 0, 0, 0xC7, 0xFF, 0x17]))
        self.assertEqual(cypress.parse(p), (0x38, b''))
        self.assertEqual(cypress.parse(cypress.packet(0x39, b'\x00\x01', 'crc'), 'crc'), (0x39, b'\x00\x01'))
        with self.assertRaises(cypress.PacketError) as e:
            cypress.parse(p[:4] + b'\x00\x00\x17')
        self.assertEqual(e.exception.reason, 'checksum')

    def test_bootloader_session(self):
        bl = cypress.Bootloader(silicon_id=0x11223344, row_size=8, arrays={0: (0, 3)})
        st, data = cypress.parse(bl.feed(cypress.packet(0x38)))
        self.assertEqual((st, data), (0, bytes([0x44, 0x33, 0x22, 0x11, 0]) + bytes([1, 30, 1])))
        self.assertEqual(cypress.parse(bl.feed(cypress.packet(0x32, b'\x00'))), (0, b'\x00\x00\x03\x00'))
        self.assertEqual(cypress.parse(bl.feed(cypress.packet(0x32, b'\x01')))[0], cypress.ERR_ARRAY)
        row = bytes(range(1, 9))
        self.assertEqual(cypress.parse(bl.feed(cypress.packet(0x37, row[:4])))[0], 0)
        # a packet split across feeds
        pr = cypress.packet(0x39, b'\x00\x02\x00' + row[4:])
        self.assertEqual(bl.feed(pr[:5]), b'')
        self.assertEqual(cypress.parse(bl.feed(pr[5:]))[0], 0)
        self.assertEqual(bl.rows[(0, 2)], row)
        self.assertEqual(cypress.parse(bl.feed(cypress.packet(0x3A, b'\x00\x02\x00'))),
                         (0, bytes([cypress.row_checksum(row)])))
        self.assertEqual(cypress.parse(bl.feed(cypress.packet(0x39, b'\x00\x09\x00' + row)))[0], cypress.ERR_ROW)
        self.assertEqual(cypress.parse(bl.feed(cypress.packet(0x39, b'\x00\x01\x00' + row[:3])))[0],
                         cypress.ERR_LENGTH)
        self.assertEqual(cypress.parse(bl.feed(cypress.packet(0x31))), (0, b'\x01'))
        bad = bytearray(cypress.packet(0x31))
        bad[4] ^= 1
        self.assertEqual(cypress.parse(bl.feed(bytes(bad)))[0], cypress.ERR_CHECKSUM)
        self.assertEqual(cypress.parse(bl.feed(cypress.packet(0x3F)))[0], cypress.ERR_CMD)
        self.assertEqual(bl.feed(cypress.packet(0x3B)), b'')
        self.assertTrue(bl.exited)

    def test_hook_and_raw_handler(self):
        long_reply = b'\x01' + b'\x00' * 20
        bl = cypress.Bootloader(hook=lambda cmd, data: long_reply if cmd == 0x38 else
                                (cypress.NO_REPLY if cmd == 0x31 else None))
        self.assertEqual(bl.feed(cypress.packet(0x38)), long_reply)
        self.assertEqual(bl.feed(cypress.packet(0x31)), b'')
        sc = Script([Raw(True)])
        peer = S06Responder(sc, raw_handler=cypress.Bootloader())
        peer.start()
        self.assertEqual(cypress.parse(peer.feed(cypress.packet(0x38)))[0], 0)
        sc.check()

    def test_cyacd(self):
        rows = [(0, 1, bytes(range(16))), (0, 2, b'\xff' * 16)]
        t = cypress.build_cyacd(0x1E0A0069, 0x11, rows)
        p = cypress.parse_cyacd(t)
        self.assertEqual((p['silicon_id'], p['silicon_rev'], p['checksum_type']), (0x1E0A0069, 0x11, 0))
        for (a, r, d, ck), (_, _, want) in zip(p['rows'], rows):
            self.assertEqual(d, want)
            self.assertEqual(cypress.verify_row_expected(a, r, d, ck), cypress.row_checksum(d))
        with self.assertRaises(ValueError):
            cypress.parse_cyacd(t.replace(':0000', ':0100', 1))


if __name__ == '__main__':
    unittest.main()
