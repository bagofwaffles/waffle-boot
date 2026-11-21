#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Loopback tests for tools/testos_mon.py: the frame codec and the Monitor
transport (send, next_frame, call) over a pseudo-terminal, so the tool is
exercised without a unit. Python 3, standard library only."""
import contextlib
import io
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(TOP, 'tools')
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)
import testos_mon as m  # noqa: E402


class Codec(unittest.TestCase):
    def test_frame_round_trip(self):
        for payload in (b'', b'\x00', b'\x01\x02\x03', bytes(range(40)), b'\x00' * 10):
            wire = m.frame(payload, 7)
            self.assertEqual(wire[-1], 0)			# the delimiter
            ident, got = m.unframe(wire[:-1])
            self.assertEqual(ident, m.ID)
            self.assertEqual(got, payload)

    def test_unframe_rejects_junk(self):
        self.assertIsNone(m.unframe(b'\x01\x02\x03'))

    def test_carrier_by_device(self):
        """S-10c.020's carrier names device.midi_byte (S-18.030): 0x21 on the OP-XY, 0x02 on the OP-1F."""
        self.assertEqual(m.carrier_for('xy'), m.CARRIER)
        self.assertEqual(m.carrier_for('1f'), bytes([0xF0, 0x00, 0x20, 0x76, 0x02, 0x66]))
        self.assertTrue(m.continue_sysex().startswith(m.CARRIER))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(m.main(['testos_mon.py', '--device', 'zz', 'ping']), 2)


class Transport(unittest.TestCase):
    def setUp(self):
        self.master, slave = os.openpty()
        self.mon = m.Monitor(os.ttyname(slave))
        os.close(slave)

    def tearDown(self):
        os.close(self.mon.fd)
        os.close(self.master)

    def peer_send(self, payload, seq=1):
        os.write(self.master, m.frame(payload, seq))

    def test_send_reaches_peer(self):
        self.mon.send(bytes([m.PING]))
        wire = os.read(self.master, 256)
        ident, payload = m.unframe(wire.split(b'\x00')[0])
        self.assertEqual((ident, payload[0]), (m.ID, m.PING))

    def test_next_frame(self):
        self.peer_send(bytes([m.DEV_FLAGS, 0, 1, 2, 3, 4]))
        got = self.mon.next_frame(2.0)
        self.assertEqual(got[0], m.DEV_FLAGS)
        self.assertIsNone(self.mon.next_frame(0.2))		# nothing more

    def test_call_round_trip(self):
        # call() writes a request, then reads the first frame of our id.
        self.peer_send(bytes([m.DEV_OK if hasattr(m, 'DEV_OK') else 0]) + b'\xAB')
        got = self.mon.call(bytes([m.PING]), timeout=2.0)
        self.assertEqual(got[1], 0xAB)

    def test_nordic_info(self):
        # One info request with the host's route to the Nordic; the reply's payload comes back, and a
        # log line that arrives first is passed over.
        os.write(self.master, m.frame(b'\x00\x00\x00\x01log', 1, 0x03))
        os.write(self.master, m.frame(b'mode:application;sw_version:1.0.1', 2, m.INFO_REPLY, 0x32))
        self.assertEqual(m.nordic_info(self.mon, 2.0), b'mode:application;sw_version:1.0.1')
        raw = m.cobs_decode(os.read(self.master, 256).split(b'\x00')[0])
        self.assertEqual((raw[0], raw[2], raw[3]), (m.NORDIC_ROUTE, m.INFO, 0))
        self.assertIsNone(m.nordic_info(self.mon, 0.2))

    def test_put_frames(self):
        opn = bytes([m.DEV_PUT, m.PUT_OPEN]) + b'os.ldr'
        self.assertEqual(opn, bytes([25, 0, 111, 115, 46, 108, 100, 114]))
        dat = bytes([m.DEV_PUT, m.PUT_DATA]) + b'\x01\x02'
        self.assertEqual(dat[:2], bytes([25, 1]))
        self.assertEqual(bytes([m.DEV_PUT, m.PUT_CLOSE]), bytes([25, 2]))
        self.assertEqual(bytes([m.DEV_SHA]) + b'os.ldr', bytes([26, 111, 115, 46, 108, 100, 114]))

    def test_line_log(self):
        # [0, 0x50, count, (us:4, levels:1)...]: bit 0 PA6, bit 1 PC5.
        fr = bytes([0, 0x50, 2]) + struct.pack('<IB', 0, 0b10) + struct.pack('<IB', 1234000, 0b11)
        self.assertEqual(m.line_log(fr), '0ms PA6 low PC5 high, 1234ms PA6 high PC5 high')

    def test_mark_steps(self):
        # PA6 high at t0 (the reply), low at stop_server, then pulses of 2 and 4 ms (steps 1, 2),
        # then a high that stays (PA4 freed).
        ev = [(0, 3), (21000, 2), (30000, 3), (32000, 2), (34000, 3), (38000, 2), (900000, 3)]
        fr = bytes([0, 0x50, len(ev)]) + b''.join(struct.pack('<IB', t, lv) for t, lv in ev)
        self.assertEqual(m.mark_steps(fr), ([1, 2], 900))

    def test_handover_frame_shape(self):
        # dev-handover builds [opcode, flags:4, md:2, sd:2].
        body = bytes([m.DEV_HANDOVER]) + struct.pack('<IHH', 0x18800, 500, 0)
        self.assertEqual(len(body), 9)
        self.assertEqual(struct.unpack_from('<I', body, 1)[0], 0x18800)	# above 16 bits
        self.assertEqual(struct.unpack_from('<H', body, 5)[0], 500)


class ScreenKeys(unittest.TestCase):
    """dev-screen, dev-key and dev-shot against replies laid out as src/board/opxy_devhost.c
    builds them (waffle/devhost.h)."""

    def unit(self, rows, changes=7, bump_at=None):
        """A checked() for DEV_SCREEN and DEV_FRAME: the reply without its status byte."""
        state = {'changes': changes, 'calls': 0}

        def checked(body):
            state['calls'] += 1
            if bump_at is not None and state['calls'] == bump_at:
                state['changes'] += 1
            if body[0] == m.DEV_FRAME:
                off, n = struct.unpack_from('<IB', body, 1)
                return bytes((off + i) & 0xFF for i in range(n))
            part = body[1]
            head = bytes([1 + len(rows)]) + struct.pack('<I', state['changes'])
            if part == 0:
                return head + bytes([2, 0x05, 1, 7]) + b'system\0System\0'
            style, text = rows[part - 1]
            return head + bytes([part - 1, 0]) + style + b'\0' + text
        return checked

    def test_screen(self):
        scr = m.read_screen(self.unit([(b'head', b'Items'), (b'item', b'1 Key test\tkeys')]))
        self.assertEqual((scr['screen'], scr['title'], scr['usb'], scr['mtp']),
                         ('system', 'System', 'mtp+cdc', 'vendor (macOS)'))
        self.assertEqual(scr['flags'], ['usb up', 'armed'])
        self.assertEqual([r['text'] for r in scr['rows']], ['Items', '1 Key test\tkeys'])
        self.assertIn('1 Key test  |  keys', m.screen_text(scr))

    def test_screen_changed(self):
        # The counter moves during the first read: the screen is read again.
        scr = m.read_screen(self.unit([(b'text', b'a'), (b'text', b'b')], bump_at=2))
        self.assertEqual(len(scr['rows']), 2)

    def test_keys(self):
        self.assertEqual(m.parse_keys(['2', 'white', 'back', '0x12:down', 'shift:up']),
                         [(0x07, m.KEY_PRESS), (0x0D, m.KEY_PRESS), (0x15, m.KEY_PRESS),
                          (0x12, m.KEY_DOWN), (0x37, m.KEY_UP)])
        for bad in (['nine'], ['72'], ['1:held']):
            with self.assertRaises(SystemExit):
                m.parse_keys(bad)
        reqs = m.key_requests([(1, m.KEY_PRESS)] * 40)
        self.assertEqual([len(r) for r in reqs], [1 + 64, 1 + 16])
        self.assertEqual(reqs[0][:3], bytes([m.DEV_KEY, 1, m.KEY_PRESS]))

    def test_shot(self):
        import tempfile
        import zlib
        import ui_shots
        fb = m.read_frame(self.unit([]))
        self.assertEqual(len(fb), m.FRAME_BYTES)
        self.assertEqual(fb[:3], b'\0\1\2')
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'shot.png')
            ui_shots.frame_png(fb, path)
            with open(path, 'rb') as f:
                png = f.read()
        self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
        self.assertEqual(struct.unpack_from('>II', png, 16), (960, 444))
        self.assertGreater(len(zlib.decompress(png[41:png.index(b'IEND') - 8])), 0)


class YaffsRaw(unittest.TestCase):
    """dev-yscan over a synthetic region, its reads guarded as the unit guards
    them (src/board/opxy_dev_yaffs.c): a data chunk's bytes are refused."""

    def setUp(self):
        self.img = bytearray(4 * 128 * 4096)
        self.data_reads = 0

    def put(self, chunk, seq, obj, cw, nb, body=b''):
        o = chunk * 4096
        self.img[o:o + len(body)] = body
        struct.pack_into('<IIII', self.img, o + 0xFF0, seq, obj, cw, nb)

    def tags(self, first, stride, count):
        self.assertLessEqual(count, m.Y_TAGS_MAX)
        return b''.join(bytes(self.img[(first + stride * i) * 4096 + 0xFF0:][:16]) for i in range(count))

    def meta(self, chunk, off, n):
        self.assertLessEqual(n, m.Y_META_MAX)
        seq, obj, cw, _ = struct.unpack_from('<IIII', self.img, chunk * 4096 + 0xFF0)
        limit = 0x200 if seq and cw & 0x80000000 else 1540 if seq and obj == 0x10 and cw == 1 else 0
        if off + n > limit:
            self.data_reads += 1
            raise SystemExit('monitor: refused')
        return bytes(self.img[chunk * 4096 + off:][:n])

    def header(self, typ, parent, name, size):
        h = bytearray(b'\xff' * 0x200)
        struct.pack_into('<II', h, 0, typ, parent)
        h[0xA:0x10A] = name.encode().ljust(256, b'\0')
        struct.pack_into('<I', h, 0x124, size)
        struct.pack_into('<IIIIII', h, 0x1E8, 0, 0xFFFFFFFF, 0, 0, 0, 0)
        return bytes(h)

    def test_scan(self):
        # block 0: an older writer's os.ldr (obj 0x100) and one data chunk
        self.put(0, 0x1001, 0x10000100, 0x80000001, 5, self.header(1, 1, 'os.ldr', 5))
        self.put(1, 0x1001, 0x100, 1, 5, b'hello')
        # block 1: a checkpoint
        self.put(128, 0x21, 3, 1, 4080)
        # block 2: started by a writer of 0x1002 over chunks of 0x1001 left in it
        self.put(256, 0x1002, 0x10000101, 0x80000001, 0, self.header(1, 1, 'os.ldr', 0))
        self.put(257, 0x1001, 0x100, 2, 5)
        # block 3: full, with a valid summary whose entry 5 is the header of a directory
        ent = []
        for c in range(127):
            if c == 5:
                self.put(384 + c, 0x1003, 0x30000102, 0x80000001, 0, self.header(3, 1, 'user', 0))
                ent.append((0x30000102, 0x80000001, 0))
            else:
                self.put(384 + c, 0x1003, 0x101, c + 1, 4080)
                ent.append((0x101, c + 1, 4080))
        body = b''.join(struct.pack('<III', *e) for e in ent)
        summ = struct.pack('<IIII', 1, 4, 0x1003, sum(body) & 0xFFFFFFFF) + body
        self.put(511, 0x1003, 0x10, 1, 1540, summ)

        scan = m.yscan(self.tags, self.meta, objs=(0x100,), blocks=4)
        self.assertEqual(self.data_reads, 0)
        by = {r['b']: r for r in scan['blocks']}
        self.assertEqual(scan['empty'], 0)
        self.assertEqual(by[1]['class'], 'checkpoint')
        self.assertEqual(by[3]['summary'], 'valid')
        names = sorted((h['hdr']['name'], h['obj'], h['seq']) for h in scan['headers'] if h.get('hdr'))
        self.assertEqual(names, [('os.ldr', 0x100, 0x1001), ('os.ldr', 0x101, 0x1002), ('user', 0x102, 0x1003)])
        rep = m.yreport(scan, (0x100,))
        self.assertIn('checkpoint blocks: 1(1 chunks)', rep)
        self.assertIn('mixed block 2 (seq 00001002): 1 chunks of 00001001', rep)
        self.assertIn('data sequence numbers 00001001..00001003', rep)
        self.assertIn('chunk    2: 00001002@2.1/5(own 00001001)', rep)
        # a summary that does not add up is invalid, and the block's own tags stand
        self.img[511 * 4096 + 20] ^= 1
        self.assertEqual({r['b']: r for r in m.yscan(self.tags, self.meta, blocks=4)['blocks']}[3]['summary'],
                         'invalid')
        self.assertEqual(m.ytags_req(1, 2, 3), bytes([27, 1, 0, 0, 0, 2, 0, 0, 0, 3]))
        self.assertEqual(m.ymeta_req(5, 0x1E8, 24), bytes([28, 5, 0, 0, 0, 0xE8, 1, 24]))


class FakeBus:
    """mon_bus() (src/testos/opxy_testos.c) over a bridge that always works: each step's status 1, the
    room checks as the C code makes them, and a slave whose memory pokes write and peeks read, whose
    echo answers the complement and whose armed reply fills the q11 slots."""

    def __init__(self, sym):
        self.sym, self.mem, self.programs = sym, {}, 0
        self.seen = [0x00000801, 0x00000801, 120, 160]

    def answer(self, h):
        cmd, _, _, a0, a1 = struct.unpack('<BBHII', h)
        if cmd == m.POKE:
            self.mem[a0] = a1
            return cmd, 0
        if cmd == m.PEEK:
            return cmd, self.mem.get(a0, 0)
        q = self.sym['wb_spi_testos_q11']
        if self.mem.get(self.sym['wb_spi_testos_arm'], 0) & 0x7FFFFFFF:
            self.mem[self.sym['wb_spi_testos_arm']] -= 1
            for k, v in enumerate([self.mem.get(self.sym['wb_spi_testos_mode'], 0), 0, 1, cmd, 300, 0xFFFFFF00,
                                   900, 905, 200, 40, 20900, 0, m.NONE, m.NONE, 0x00410801, 0]):
                self.mem[q + 4 * k] = v
        return cmd, a0 ^ 0xFFFFFFFF

    def bus(self, prog, timeout=5.0):
        self.programs += 1
        p, n, i, out, last = prog, len(prog), 0, bytearray(), None
        mx = m.BUS_MAX
        while i < n and len(out) + 6 <= mx:
            op = chr(p[i])
            i += 1
            if op == 'T' and i + 9 <= n and i + 9 + p[i + 8] <= n and len(out) + 17 <= mx:
                k = p[i + 8]
                last = bytes(p[i + 9:i + 9 + k])
                i += 9 + k
                out += b'\x01' + struct.pack('<4I', *self.seen)
            elif op == 'R' and i + 9 <= n and len(out) + 17 + p[i + 8] <= mx:
                k = p[i + 8]
                i += 9
                out += b'\x01' + struct.pack('<II', *self.answer(last))[:k] + struct.pack('<4I', *self.seen)
            elif op == 's' and i < n:
                i += 1
                out += b'\x01'
            elif op == 't' and i < n and i + 1 + p[i] <= n:
                k = p[i]
                last = bytes(p[i + 1:i + 1 + k])
                i += 1 + k
                out += b'\x01'
            elif op == 'r' and i < n and len(out) + 1 + p[i] <= mx:
                k = p[i]
                i += 1
                out += b'\x01' + struct.pack('<II', *self.answer(last))[:k]
            elif op == 'w' and i + 5 <= n:
                i += 5
                out += b'\x01' + struct.pack('<I', 7)
            elif op == 'd' and i + 4 <= n:
                i += 4
                out += b'\x01'
            elif op == 'c' and i < n:
                i += 1
                out += b'\x01'
            else:
                out += b'\xff'
                break
        return bytes(out)


SYM = {n: 0x890B2000 + 0x100 * k for k, n in enumerate(m.Q11_SYMBOLS)}


class Q11(unittest.TestCase):
    def test_prog_encoding(self):
        p = m.Prog()
        p.send_last(0x00242592, 1, b'\x0a' + bytes(11))
        p.recv_last(0x00202592, 3, 8)
        self.assertEqual(bytes(p.code[:10]), b'T' + struct.pack('<IIB', 0x00242592, 1, 12))
        self.assertEqual(bytes(p.code[22:]), b'R' + struct.pack('<IIB', 0x00202592, 3, 8))
        out = (b'\x82' + struct.pack('<4I', 0x10, 0x20, 5, m.NONE) + b'\x01' + bytes(range(8)) +
               struct.pack('<4I', 1, 2, 3, 4))
        t, r = p.parse(out)
        self.assertEqual((t['op'], t['ok'], t['status'], t['seen']), ('T', False, 0x82, [0x10, 0x20, 5, m.NONE]))
        self.assertEqual((r['ok'], r['data'], r['seen']), (True, bytes(range(8)), [1, 2, 3, 4]))
        self.assertTrue(p.parse(out[:20])[1]['missing'])

    def test_programs_fit(self):
        # the largest programs q11 sends: an armed reply exchange, a batch of four quad requests
        p = m.Prog()
        for _ in range(3):
            m.poke_prog(p, 0x890B2000, 1)
        m.immediate(p, m.hdr(m.ECHO, 1), reply=m.M_QUAD, gap_us=50)
        m.poke_prog(p, 0x890B2000, 0)
        p.conf(0)
        self.assertTrue(p.fits())
        p = m.Prog()
        m.poke_prog(p, 0x890B2000, 0x403)
        for a in range(4):
            m.immediate(p, m.hdr(m.ECHO, a), request=m.M_QUAD)
        p.conf(0)
        self.assertTrue(p.fits())
        m.immediate(p, m.hdr(m.ECHO, 9))
        self.assertFalse(p.fits())

    def test_reply_exchange(self):
        bus = FakeBus(SYM)
        mon = type('Mon', (), {'bus': lambda self, prog, timeout=5.0: bus.bus(prog)})()
        mode = m.QUAD_LAST | m.REPLY_DMA | m.REPLY_TF_LATE
        ex = m.reply_exchange(mon, SYM, mode, 3)
        self.assertTrue(ex['ok'], ex)
        self.assertEqual(ex['words'], [m.ECHO, m.echo_arg(3) ^ 0xFFFFFFFF])
        q = m.q11_named(ex['q11'])
        self.assertEqual((q['mode'], q['ended'], q['tf stat']), (mode, 1, 900))
        self.assertEqual(bus.mem[SYM['wb_spi_testos_arm']], 0)      # disarmed
        self.assertEqual(ex['reply']['seen'], bus.seen)
        self.assertIn('ended tf', m.exchange_line(ex))
        got = m.request_batch(mon, SYM, m.REQ_QUAD | m.REQ_EN_LAST, [5, 6, 7, 8])
        self.assertEqual([g['ok'] for g in got], [True] * 4)
        self.assertEqual(bus.mem[SYM['wb_spi_testos_req']], 0x403)
        self.assertEqual(m.peek_words(mon, [1, 2, SYM['wb_spi_testos_req']]), [0, 0, 0x403])

    def test_reply_summary(self):
        def ex(ok, ended, tf, high, spif=0, mode=0x0FB):
            q = [mode, 0, ended, m.ECHO, 300, 0xFFFFFF00, tf, m.NONE, 200, 40, high, spif, 10, 20, 0x801, 0]
            return {'ok': ok, 'steps_ok': True, 'words': [m.ECHO, 0], 'q11': q,
                    'reply': {'status': 1, 'seen': [0, 0, 150, m.NONE]}}
        exs = [ex(True, 1, 900, 20900), ex(True, 1, 1000, 20100), ex(False, 3, m.NONE, 20000)]
        text = '\n'.join(m.reply_summary(exs))
        self.assertIn('replies right 2 of 3', text)
        self.assertIn('ended by: master 1, tf 2', text)
        self.assertIn('TF read set in SPI_STAT on 2 of 3, in SPI_ILAT on 0', text)
        self.assertIn('TF to the master\'s end (PB15 high): 19100..20000', text)
        self.assertIn('from TWC 0 to TF: 700..800; from the DMA done to TF: 600..700', text)
        self.assertIn('TF unmasked at -256..-256', text)
        self.assertIn('master R: status ok on 3 of 3, RF seen on 0', text)
        tight = [ex(True, 1, 900, 20900, spif=1 | 1 << 16, mode=0x4FB), ex(True, 4, m.NONE, m.NONE, mode=0x4FB)]
        self.assertIn('tight loop: a SPIF change on 1 of 2; falls 0..1, rises 0..1', '\n'.join(m.reply_summary(tight)))

    def test_request_summary(self):
        before = [0, 0, 0, 0, 0, 0, 1, 10]
        after = [0x103, 0, m.ECHO, 7, 7 ^ 0xFFFFFFFF, 0x10, 1, 13]
        after[4] = 0                                                # the header's third word: arg1, 0
        exs = [{'ok': True, 'request': {'status': 1, 'seen': [0, 0, 40, 90]}} for _ in range(3)]
        text = '\n'.join(m.request_summary(exs, before, after, [5, 6, 7]))
        self.assertIn('quad requests whose header came whole 3 of 3; 2 s fallbacks 0', text)
        self.assertIn('ROR set', text)
        self.assertIn('TF read set on 3, cycles to the last word 40..40, to TF 90..90', text)
        self.assertIn('0000000a 00000007 00000000: right', text)

    def test_symbols(self):
        import test_image_tools as it
        syms = [('_' + n, 0x890B2000 + 4 * k, '.bss') for k, n in enumerate(m.Q11_SYMBOLS)]
        data = it.build_elf([('.bss', 8, 3, 0x890B2000, 0x100)], syms, {}, 0)
        import elf32
        got = m.symbol_addrs(elf32.Elf(data))
        self.assertEqual(got['wb_spi_testos_q11'], 0x890B2008)
        with self.assertRaises(SystemExit):
            m.symbol_addrs(elf32.Elf(data), ['wb_spi_testos_missing'])


class Crumbs(unittest.TestCase):
    """crumbs: check.txt's breadcrumbs line decoded with no unit."""

    def test_marks(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            m.run(None, ['crumbs', '01ff0304'])
        self.assertIn('crumb  1: opxy_master_main entered', out.getvalue())
        self.assertIn('crumb  3: bring-up done', out.getvalue())

    def test_none(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            m.run(None, ['crumbs', 'ff' * 63])
        self.assertIn('breadcrumbs: none', out.getvalue())


class Link(unittest.TestCase):
    """dev-extract's estimate, the link's record and the link bench's table, read from replies laid out
    as waffle/devhost.h gives them."""

    UNIT = {'mkdir requests': 61, 'stat requests': 1, 'files': 852, 'write requests': 7035}

    def test_estimate(self):
        # by bytes: the unit's run, 129,019,167 bytes, a 32 KiB probe write 9088.0 us, a stat 144.4 us
        secs, reqs, small = m.bridge_estimate(129019167, self.UNIT, 9088.0, 144.4, 32)
        self.assertAlmostEqual(secs, 36.49, places=2)
        self.assertEqual((reqs, small), (8802, 1767))
        # the old product of write requests and a full probe write is caught
        self.assertNotAlmostEqual(secs, (7035 * 9088.0 + 1767 * 144.4) / 1e6, places=0)

    @staticmethod
    def status(told=1, fell=1, why=4, cmd=0x2B, mode=2, length=0x8000, got=0x4000, words=(0, 0, 0, 0)):
        return struct.pack('<BIBBBII4I', told, fell, why, cmd, mode, length, got, *words)

    @staticmethod
    def fault(at=0, words=(0, 0, 0, 0)):
        return struct.pack('<B4I', at, *words)

    @staticmethod
    def fault_more(at=0, words=(0, 0, 0, 0), more=(0, 0, 0, 0)):
        return struct.pack('<B8I', at, *words, *more)

    def bench_fault(self, no, kind=0, rung=1, res=0, length=12, req=0, to=0, fall_us=1500,
                    fault=None, echo=None, st=None):
        return (struct.pack('<HBBBIHBI', no, kind, rung, res, length, req, to, fall_us) +
                (fault or self.fault_more(1, (0xFFFFFFFD, 0, 8, 6), (0x8, 0x1234, 0x5, 3))) +
                (echo or self.fault_more()) + (st or self.status(why=2, cmd=0x1A, length=12, got=0)))

    def record_parts(self):
        rung = (struct.pack('<BBBI', 4, 2, 7, 0x8000) + self.fault(1, (0xFFFFFFFD, 0x60000, 4096, 7)) +
                struct.pack('<IIII', 0, 0, 0, 0) + self.status(told=0))
        kept = (struct.pack('<BBBI', 3, 0, 0, 0) + self.fault() +
                struct.pack('<IIII', 9300, 9100, 9200, 20976) + self.status(told=0))
        fall = (struct.pack('<BBBBI', 3, 2, 0x2B, 0, 0x8000) + self.fault_more(6) +
                self.status(words=(0xFFFFFFFC, 0x10, 12, 8)))
        fall2 = (struct.pack('<BBBBI', 4, 3, 0x2B, 0, 0x8000) +
                 self.fault_more(1, (0xFFFFFFFD, 0x100000, 9, 3), (0x208, 0x100421, 0x5, 9)) +
                 self.status(told=0))
        bench = [self.bench_fault(0), self.bench_fault(1, req=1),
                 self.bench_fault(17, kind=1, rung=3, res=2, length=0x8000, req=0),
                 self.bench_fault(19, rung=2, req=1, to=0xFF, fall_us=4000000,
                                  echo=self.fault_more(1, (0xFFFFFFFD, 0, 3, 2)), st=self.status(told=0))]
        self.assertEqual((len(rung), len(kept), len(fall), len(bench[0])), (72, 72, 73, m.BENCH_FAULT))
        parts = [bytes([0, 19, 0]) + struct.pack('<IIBBBIBI', 2, 2, 2, 3, 0, 3, self.settled, 20)]
        parts.append(bytes([1, 19, 2]) + rung + kept)
        parts += [bytes([p, 19, 0]) for p in range(2, 5)]
        parts.append(bytes([5, 19, 2]) + fall + fall2)
        parts += [bytes([p, 19, 0]) for p in range(6, 11)]
        parts.append(bytes([11, 19, 2]) + bench[0] + bench[1])
        parts.append(bytes([12, 19, 2]) + bench[2] + bench[3])
        parts += [bytes([p, 19, 0]) for p in range(13, 19)]
        self.assertEqual(len(parts), m.RECORD_PARTS)
        return parts

    class FakeMon:
        def __init__(self, replies):
            self.replies = replies

        def checked(self, payload, timeout=5.0):
            return self.replies(payload)

    def run_out(self, fn, *a):
        import contextlib
        import io
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            fn(*a)
        return out.getvalue()

    settled = 1

    def test_record(self):
        parts = self.record_parts()
        text = self.run_out(m.show_link_record, self.FakeMon(lambda p: parts[p[1]]))
        self.assertNotIn('restart', text)
        self.assertIn("64 KiB sessions 9300/9100/9200 us, the least against the plain wire's 20976 us "
                      "(0.434): kept", text)
        self.assertIn('tried rung 4 (quad I/O at 50 MHz, checked): the test of 32768 bytes (walking zero) '
                      'failed: the send failed [the DMA reported an error (ERRC 0); rc -3, status 0x60000, '
                      '4096 words left]', text)
        self.assertNotIn('two-halved', text)
        # the last link test's faults: the first and the last kept
        self.assertIn('the last link test had 20 fault(s); its first 12 and its last 4 are kept', text)
        self.assertIn('fault 1: rung 1 (single I/O at 25 MHz, checked), request 1 of '
                      '12 bytes: the send failed [the DMA\'s count stopped; rc -3, status 0x0, 8 words '
                      'left] [DMA idle, not done, error cause 0 (status 0x8); configuration 0x1234: disabled, '
                      'no interrupt asked for; transmit control 0x5, 3 word(s) still to send]', text)
        # a request's fall-back with the driver's second four, the slave holding the master
        self.assertIn('fell back from rung 4 (quad I/O at 50 MHz, checked) to rung 3 (quad I/O at 25 MHz, '
                      'checked): the send failed [waiting for the transmit to end; rc -3, status 0x100000, 9 '
                      'words left; the slave\'s ready line was holding the master] [DMA moving data, not done, '
                      'error cause 0 (status 0x208); configuration 0x100421: enabled, its interrupt asked for; '
                      'transmit control 0x5, 9 word(s) still to send]', text)
        self.assertIn('the link came back to rung 0 (single I/O at 25 MHz, plain) after 1.5 ms of echoes', text)
        self.assertIn('the slave: a session shorter than its request on 0x1a of 12 bytes, 0 in', text)
        self.assertIn('fault 18: entering rung 3 (quad I/O at 25 MHz, checked): the test of '
                      '32768 bytes failed', text)
        self.assertIn('fault 20: rung 2 (single I/O at 50 MHz, checked), request 2', text)
        self.assertIn('THE SLAVE WAS LOST: no recovery echo answered in 4000.0 ms; the last: the send failed '
                      '[waiting for transmit room', text)
        self.assertIn('request 0x2b of 32768 bytes (its first send) fell back from rung 3 (quad I/O at 25 MHz, '
                      "checked) to rung 2 (single I/O at 50 MHz, checked): the reply's check word was wrong", text)
        self.assertIn('the slave: its receive failed on 0x2b of 32768 bytes, 16384 in, in mode 2 '
                      '[receive overrun or mode fault; rc -4, status 0x10, 12 words left]', text)
        self.assertNotIn('FELL BACK', text)

    def test_unsettled(self):
        self.settled = 0
        parts = self.record_parts()
        text = self.run_out(m.show_link_record, self.FakeMon(lambda p: parts[p[1]]))
        self.assertIn('restart the unit', text)

    def test_bench_n(self):
        for bad in (['0'], ['256']):
            with self.assertRaises(SystemExit):
                m.run(None, ['dev-link-test'] + bad)

    @staticmethod
    def run_reply(flags=0, entry=(0, 0, 0, 0, 0), entries=(0, 1, 1, 1, 2), falls=1, end=(0, 0, 0, 0),
                  faults=0):
        return (bytes([m.LINK_TEST_PARTS, 2, flags]) + bytes(entry) + bytes(entries) +
                struct.pack('<QHH', int(4e8), falls, 90) + struct.pack('<BBIHI', *end, faults))

    def test_reply_size(self):
        self.assertEqual(len(self.run_reply()), m.LINK_TEST_REPLY)

    def test_bench_flags(self):
        def run(flags):
            reply = self.run_reply(flags, falls=3)
            return self.run_out(m.show_link_test, self.FakeMon(
                lambda p: reply if p[1] == 0 else bytes([p[2], m.LINK_TEST_PARTS, 0])), 2)
        self.assertIn('CUT SHORT', run(2))
        self.assertIn('restart the unit', run(4))
        self.assertIn('THE SLAVE STOPPED ANSWERING', run(1))
        self.assertIn('(the bound 90 s), 3 fall-back(s)', run(0))

    def test_bench_lost(self):
        """A send that never works: rung 1 not entered, then the slave lost."""
        reply = self.run_reply(1, entry=(0xFF, 2, 0xFF, 0xFF, 0xFF), entries=(0, 0, 0, 0, 0), falls=1,
                               end=(1, 1, 12, 0), faults=1)
        text = self.run_out(m.show_link_test, self.FakeMon(
            lambda p: reply if p[1] == 0 else bytes([p[2], m.LINK_TEST_PARTS, 0])), 16)
        self.assertIn('it ended on rung 1 (single I/O at 25 MHz, checked), at request 1 of 12 bytes', text)
        self.assertIn('1 fault(s) (dev-link-record has each)', text)
        self.assertIn('rung 1 (single I/O at 25 MHz, checked): could not be entered: a test failed', text)
        self.assertIn('rung 2 (single I/O at 50 MHz, checked): not tried', text)
        self.assertNotIn('phase', text)

    def test_entry_text(self):
        self.assertEqual(m.entry_text(0xFF, 0), 'not tried')
        self.assertEqual(m.entry_text(0, 3), 'entered 3 time(s)')
        self.assertEqual(m.entry_text(2, 0), 'could not be entered: a test failed')
        self.assertEqual(m.entry_text(1, 2), 'entered 2 time(s), then not again: 0x19 on trial failed')

    def test_bench(self):
        run = self.run_reply(entries=(0, 2, 1, 1, 1))
        rows = []
        for i in range(35):
            rung = 1 + i // 7 if i < 28 else 0
            length = [12, 64, 256, 0x1000, 0x4000, 0x8000, 0x10000][i % 7]
            at = [0] * 10
            if i == 3:
                at[6] = 1
            rows.append(struct.pack('<BIHHI10H', rung, length, 2, 2 - (i == 3), 1000 * (i + 1), *at))
        self.assertEqual(len(rows[0]), m.LINK_TEST_ROW)

        def replies(p):
            if p[1] == 0:
                return run
            k = p[2]
            return bytes([k, m.LINK_TEST_PARTS, 7]) + b''.join(rows[7 * k:7 * k + 7])
        text = self.run_out(m.show_link_test, self.FakeMon(replies), 2)
        self.assertIn('link test: 2 request(s) a length, 1.0 s in all', text)
        self.assertIn('rung 1 (single I/O at 25 MHz, checked): entered 2 time(s)', text)
        self.assertIn('check 1', text)
        table = [ln for ln in text.splitlines() if ln.startswith('rung ') and ln[5].isdigit()]
        self.assertEqual(len(table), 35)
        # each rung's lengths, then plain; one way of sending, no column for it
        self.assertTrue(all('plain' in ln for ln in table[28:]))
        self.assertFalse(any('words' in ln.split() or 'halves' in ln.split() for ln in table))


class QuadReply(unittest.TestCase):
    """dev-quad-reply: OPXY_DEV_QUAD_REPLY's answer laid out as waffle/devhost.h gives it, the cells asked
    for, the tables and the verdicts."""

    @staticmethod
    def answer(cand=1, ren=0, clock=2, length=12, n=16, flags=0, why=0, counts=(16, 16, 16, 0, 0, 0, 0, 0),
               ends=None, dmin=40, dmax=52, lines=0, m_or=1 << 10, m_and=1 << 10, s_or=0x800,
               first=0, first_no=0, first_word=0xFFFFFFFF, first_m=(0, 0, 0, 0), rec=None):
        ends = ends or [0, counts[2], counts[3], 0, 0, 0, 0, 0]
        rec = rec or [0] * 18
        return struct.pack(m.QUAD_LAYOUT, cand, ren, clock, length, n, flags, why, *counts, *ends, dmin, dmax,
                           lines, m_or, m_and, s_or, 400000, first, first_no, first_word, *first_m, *rec)

    def test_layout(self):
        self.assertEqual(struct.calcsize(m.QUAD_LAYOUT), m.QUAD_REPLY)
        self.assertEqual(m.QUAD_REPLY + 1, 168)         # OPXY_QUAD_REPLY, its status byte included
        d = m.quad_parse(self.answer(length=0x10000, n=4096))
        self.assertEqual((d['cand'], d['clock'], d['len'], d['n'], d['whole']), (1, 2, 0x10000, 4096, 16))
        self.assertEqual(d['first_rec']['stage'], 0)
        with self.assertRaises(SystemExit):
            m.quad_parse(b'\0' * 20)

    def test_request(self):
        r = m.quad_request(3, 0, 1, 0x4000, 4096)
        self.assertEqual(r, bytes([m.DEV_QUAD_REPLY, 3, 0, 1]) + struct.pack('<IH', 0x4000, 4096))

    def test_args(self):
        n, cells, js = m.quad_args([])
        self.assertEqual((n, js, len(cells)), (16, False, 140))       # REN only for the TF candidates
        self.assertFalse(any(c[0] == 3 and c[1] for c in cells))
        n, cells, js = m.quad_args(['4096', '--cand', 'spif', '--clock', '50f', '--len', '12,0x10000', '--json'])
        self.assertEqual((n, js, cells), (4096, True, [(3, 0, 3, 12), (3, 0, 3, 0x10000)]))
        for bad in (['0'], ['4097'], ['--cand', 'tf'], ['--len', '14'], ['--ren', '2'], ['--what']):
            with self.assertRaises(SystemExit):
                m.quad_args(bad)

    def test_verdict_good(self):
        cells = [m.quad_parse(self.answer(clock=c, length=ln)) for c in (0, 1) for ln in (12, 64)]
        self.assertEqual(m.quad_verdict(cells), 'ended every reply and every reply was whole (64 replies)')
        text = m.quad_table(cells)
        self.assertIn('TF, DMA feed; the master at 25 MHz, fast mode off', text)
        self.assertIn('TF, DMA feed; the master at 50 MHz, fast mode off', text)
        self.assertIn('40-52', text)
        self.assertIn('every reply', text)
        self.assertIn('  TF, DMA feed: ended every reply and every reply was whole', text)

    def test_verdict_failures(self):
        rec = [0] * 18
        rec[3], rec[4], rec[5], rec[7] = 64, 2 | 0x400 | 0x800 | 0x1000 | 0x2000, 0x00C40001, 1
        rec[12], rec[13], rec[14] = 400, 0xFFFFFFFF, 12000000
        missing = m.quad_parse(self.answer(cand=2, ren=1, length=64, counts=(16, 16, 0, 16, 0, 0, 0, 0),
                                           dmin=0xFFFFFFFF, dmax=0xFFFFFFFF, first=4, rec=rec))
        last = m.quad_parse(self.answer(cand=3, clock=1, length=256, counts=(8, 5, 8, 0, 3, 0, 0, 0),
                                        lines=0x1, first=2, first_no=2, first_word=63))
        self.assertEqual(m.quad_verdict([missing]),
                         'failed first at 25 MHz, fast mode on, 64 bytes: the flag did not come on 16 of 16')
        self.assertEqual(m.quad_verdict([last]), 'failed first at 50 MHz, fast mode off, 256 bytes: '
                         'the last word was wrong on 3 of 8')
        text = m.quad_table([missing, last])
        self.assertIn('TF, core feed, REN set', text)
        self.assertIn('SPIF, SPI_STAT alone', text)
        self.assertIn('trial 0: the flag did not come; the master', text)
        self.assertIn('the slave: ended by the bound; saw fed whole, the master began, the master ended, '
                      'single I/O after', text)
        self.assertIn('flag -', text)
        self.assertIn('trial 2: the last word was wrong (word 63 of 64 first)', text)
        self.assertIn(' D0 ', text)
        lost = m.quad_parse(self.answer(flags=1, counts=(3, 2, 2, 0, 0, 0, 1, 0)))
        self.assertIn('THE SLAVE STOPPED ANSWERING: restart the unit', m.quad_verdict([lost]))
        self.assertIn('the session broke on 1 of 3', m.quad_verdict([lost]))
        refused = m.quad_parse(self.answer(cand=3, ren=1, flags=8, why=1, counts=(0,) * 8))
        self.assertEqual(m.quad_verdict([refused]), 'refused: an argument out of range')

    def test_run(self):
        """The cells in order, each its own request; a lost slave stops the run; --json."""
        asked = []

        class Unit:
            def checked(unit, payload, timeout=5.0):
                asked.append(payload)
                self.assertGreaterEqual(timeout, m.QUAD_LIMIT_S)
                lost = len(asked) == 3
                return self.answer(cand=payload[1], ren=payload[2], clock=payload[3],
                                   length=struct.unpack_from('<I', payload, 4)[0], flags=1 if lost else 0)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            m.quad_reply(Unit(), ['2', '--cand', 'tf-dma', '--ren', '0', '--clock', '25,50'])
        self.assertEqual(len(asked), 3)                 # 14 cells asked for, the third lost the slave
        self.assertEqual(asked[0], m.quad_request(1, 0, 0, 12, 2))
        self.assertIn('THE SLAVE STOPPED ANSWERING', out.getvalue())
        asked.clear()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            m.quad_reply(Unit(), ['--cand', 'spif', '--clock', '25', '--len', '12,64', '--json'])
        import json
        got = json.loads(out.getvalue())
        self.assertEqual(len(got['cells']), 2)
        self.assertEqual(got['verdicts'][0]['name'], 'SPIF, SPI_STAT alone')


class Start(unittest.TestCase):
    """dev-start: OPXY_DEV_BOOT_START's report in the words the unit's screens use for it."""

    @staticmethod
    def report(**kw):
        d = dict.fromkeys(m.START_FIELDS + m.START_FILE_FIELDS, 0)
        d['attempted'] = 1
        d.update(kw)
        fault = d.pop('fault', (0, 0, 0, 0))
        return struct.pack(m.START_LAYOUT, *(d[k] for k in m.START_FIELDS), *fault,
                           *(d[k] for k in m.START_FILE_FIELDS))

    def test_layout(self):
        self.assertEqual(struct.calcsize(m.START_LAYOUT), m.START_REPORT)
        self.assertEqual(m.STATUS[3], 'the slave is not answering')

    def test_words_as_the_screens(self):
        # every case of tests/vectors/s15/ui/screens.json: the main screen's reason and Device Info's row
        import json
        with open(os.path.join(TOP, 'tests', 'vectors', 's15', 'ui', 'screens.json'),
                  encoding='utf-8') as f:
            cases = json.load(f)['start']['cases']
        self.assertGreater(len(cases), 20)
        for c in cases:
            v = dict(c['start'])
            if v['why'] != 'ENCRYPTED':
                self.assertEqual('dim:   Reason: ' + m.start_reason(v), c['main'], c['name'])
            self.assertEqual(m.start_info(v), c['info'], c['name'])

    def test_none(self):
        self.assertEqual(m.start_lines(bytes(m.START_REPORT)), [m.START_NONE])

    def test_file_start(self):
        # S-05.035: a file start's /os.ldr too large, then the slave's copy refused
        r = self.report(cause=7, file=1, file_why=7, file_length=0, file_value=8388609,
                        file_limit=8388608)
        lines = m.start_lines(r)
        self.assertEqual(lines[0], 'last plain start: /os.ldr could not be started.')
        self.assertEqual(lines[1], '  reason:   8388609 bytes, at most 8388608')
        self.assertEqual(lines[2], '  OS start: /os.ldr 8388609 bytes, at most 8388608')
        r = self.report(cause=5, replied=1, reply=1, file=1, file_length=742764, file_crc=0x1234ABCD)
        lines = m.start_lines(r)
        self.assertEqual(lines[1], '  reason:   DSP2 could not load the same /os.ldr')
        self.assertIn('  file:     /os.ldr, 742764 bytes, CRC 1234abcd of the copy read', lines)

    def test_lost_read(self):
        # the unit's case: the bridge lost on a 128-sector read of partition 2
        r = self.report(cause=3, result=8, step=3, step_part=2, part=2, sector=1281, count=128, format=3,
                        length=742764, crc=0xDEADBEEF, boot_size=0x80000, lost=1, cmd=0x12, at=4,
                        arg0=0x501, fault=(0xFFFFFFFD, 0x60000, 4096, 12))
        text = '\n'.join(m.start_lines(r))
        self.assertIn('last plain start: The OS could not be started.', text)
        self.assertIn('  reason:   DSP2 stopped answering during the image read', text)
        self.assertIn('  OS start: Sector 1281 read: 0x12 reply broke', text)
        self.assertIn('  load:     a select or read failed; cause: a failed select or read (S-05.007)', text)
        self.assertIn('  step:     an image read of partition 2, 128 sector(s) from 1281', text)
        self.assertIn('  header:   format 3, length 742764 bytes, CRC deadbeef; the partition 1024 sectors', text)
        self.assertIn('  bridge:   the slave was lost on request 0x12 (arg 0x501): the reply broke off '
                      '[PB15 rose with words to go; rc -3, status 0x60000, 4096 words left]', text)
        r = self.report(cause=3, result=8, step=1, step_part=1, lost=1, cmd=0x18, at=0x88, arg0=1)
        text = '\n'.join(m.start_lines(r))
        self.assertIn('  OS start: Select of partition 1: 0x18 slave fell', text)
        self.assertIn("on request 0x18 (arg 0x1) in a poll for it: the slave's line said it fell back", text)

    def test_causes(self):
        lines = m.start_lines(self.report(cause=1, result=7, part=1, length=742764, crc=1, image_crc=0x1234ABCD))
        self.assertIn('  OS start: CRC 1234abcd, header says 00000001', lines)
        self.assertIn("the image's CRC 1234abcd", '\n'.join(lines))
        lines = m.start_lines(self.report(cause=4, refusal=8))
        self.assertIn("  reason:   the image loads over the ROM's boot reserve", lines)
        self.assertIn("  refusal:  the image loads over the ROM's boot reserve (rule 8 of S-05.024)", lines)
        lines = m.start_lines(self.report(cause=5, replied=1, reply=1))
        self.assertIn('  OS start: DSP2 answered no OS, reply 1', lines)
        self.assertIn('  release:  reply 1', lines)
        lines = m.start_lines(self.report(cause=5))
        self.assertIn('  release:  no reply', lines)
        self.assertIn('  OS start: No reply to the release', lines)
        lines = m.start_lines(self.report(cause=2, result=6, part=1, format=3, length=742764))
        self.assertIn('  reason:   OS needs reinstalling: install a TE firmware update', lines)
        self.assertIn('  OS start: Partition 1 image is encrypted', lines)
        lines = m.start_lines(self.report(cause=1, result=4, length=742764, boot_size=0x80000))
        self.assertIn('  OS start: Needs 1452 sectors, has 1024', lines)

    def test_show(self):
        import contextlib
        import io

        class Old:
            def checked(self, payload, timeout=5.0):
                raise SystemExit('monitor: bad request')

        class New:
            def checked(self, payload, timeout=5.0):
                self.asked = payload
                return bytes(m.START_REPORT)
        out = io.StringIO()
        new = New()
        with contextlib.redirect_stdout(out):
            m.show_start(Old())
            m.show_start(new)
        self.assertEqual(new.asked, bytes([m.DEV_BOOT_START]))
        self.assertEqual(out.getvalue().splitlines(),
                         ["the failed start's report: not in this bootloader", m.START_NONE])



class Stream(unittest.TestCase):
    """dev-stream: OPXY_DEV_STREAM's report of this start's slave boot stream (S-09.009), as
    opxy_stream_report() lays it out (waffle/board/opxy_boot.h)."""

    def report(self, way=2, rc=0, end=0, fell=0, linked=1, held=0, ran=1, words=(), fault=(0, 0, 0, 0),
               fcs=(0, 0, 0, 0), version=2):
        w = list(words) + [0] * (12 - len(words))
        r = bytes([version, way, rc, end, fell, linked, held, ran]) + struct.pack('<12I', *w) + \
            struct.pack('<4I', *fault)
        if version >= 2:
            r += struct.pack('<4I', *fcs)
        self.assertEqual(len(r), m.STREAM_REPORT if version >= 2 else m.STREAM_REPORT_V1)
        return r

    def test_fcs(self):
        words = (412, 437, 421376, 0, 0, 0, 120500, 9180, 30, 12, 3, 1000)
        lines = m.stream_lines(self.report(words=words, fcs=(91000, 2400, 380, 17)))
        self.assertIn('  held:     SPI_STAT.FCS set in 2400 of 91000 samples, in 380 parts, the first at byte 16',
                      lines)
        lines = m.stream_lines(self.report(words=words, fcs=(91000, 0, 0, 0)))
        self.assertIn('  held:     SPI_STAT.FCS set in 0 of 91000 samples, in 0 parts', lines)
        lines = m.stream_lines(self.report(words=words, version=1))
        self.assertFalse(any(x.startswith('  held:') for x in lines))
        self.assertIn('  fell back: no', lines)
        self.assertIn('no report', m.stream_lines(self.report(words=words)[:80])[0])

    def test_none(self):
        self.assertIn('no report', m.stream_lines(b'')[0])
        self.assertIn('not sent', m.stream_lines(self.report(ran=0))[0])

    def test_dma(self):
        lines = m.stream_lines(self.report(words=(412, 437, 421376, 0, 0, 0, 120500, 9180, 30, 12, 3, 20)))
        self.assertEqual(lines[0], "this start's stream: by the controller under flow control, fed by DMA")
        self.assertIn('  outcome:  streamed, ended at the FINAL block; 120.500 ms from the wait for the ready '
                      'line', lines)
        self.assertIn('  bridge:   linked', lines)
        self.assertIn('  sent:     412 sessions, 437 parts by DMA, 421376 bytes by the controller, 0 one at a '
                      'time', lines)
        self.assertIn('  fell back: no', lines)

    def test_fell_back(self):
        lines = m.stream_lines(self.report(fell=3, linked=1, words=(412, 6, 6200, 415176, 7, 6200),
                                           fault=(0xFFFFFFFE, 0x10000, 25, 6)))
        text = '\n'.join(lines)
        self.assertIn('  fell back at byte 6200 of session 7: the controller stopped with the line low; the '
                      'rest one byte at a time', text)
        self.assertIn("[the DMA's count stopped; rc -2", text)
        lines = m.stream_lines(self.report(rc=4, linked=0, held=1))
        self.assertIn('  outcome:  the stream stopped part-way; 0.000 ms from the wait for the ready line', lines)
        self.assertIn('  bridge:   not linked: the slave did not answer', lines)
        self.assertIn('  the slave held the ready line past T_STREAM', lines)

    def test_core_fed(self):
        lines = m.stream_lines(self.report(way=1))
        self.assertEqual(lines[0], "this start's stream: by the controller under flow control, fed by the core")
        self.assertFalse(any(x.startswith('  sent:') for x in lines))

    def test_show(self):
        class Old:
            def checked(self, payload, timeout=5.0):
                raise SystemExit('monitor: bad request')

        class New:
            def checked(self, payload, timeout=5.0):
                self.asked = payload
                return bytes([2, 2, 0, 0, 0, 1, 0, 1]) + bytes(80)
        out = io.StringIO()
        new = New()
        with contextlib.redirect_stdout(out):
            m.show_stream(Old())
            m.show_stream(new)
        self.assertEqual(new.asked, bytes([m.DEV_STREAM]))
        self.assertIn('not in this bootloader', out.getvalue().splitlines()[0])
        self.assertIn('fed by DMA', out.getvalue())


class QuadData(unittest.TestCase):
    """dev-quad-data: OPXY_DEV_QUAD_DATA's report of this start's quad data replies (S-09.081), as
    opxy_bridge_qdata_report() lays it out (waffle/devhost.h)."""

    def report(self, state=2, why=0, rung=2, sent=8, whole=8, first=0, test_len=0, test_us=41250,
               replies=37, nbytes=2359296, failed=0, fault=(0, 0, 0, 0, 0)):
        r = struct.pack(m.QDATA_LAYOUT, state, why, rung, sent, whole, first, test_len, test_us, replies,
                        nbytes, failed, *fault)
        self.assertEqual(len(r) + 1, m.QDATA_REPLY)
        return r

    def test_taken_up(self):
        lines = m.quad_data_lines(self.report(state=4))
        self.assertEqual(lines[0], "this start's quad data replies: taken up, ended with the link")
        self.assertIn('  test:     8 of 8 test replies whole with their flag, 41.250 ms', lines)
        self.assertIn('  data:     37 sessions in quad I/O at 50 MHz, 2359296 bytes; 0 failed', lines)

    def test_not_taken_up(self):
        lines = m.quad_data_lines(self.report(state=1, why=2, whole=1, first=3, test_len=1024,
                                              replies=0, nbytes=0))
        self.assertEqual(lines[0], "this start's quad data replies: not taken up: the quad reply test failed")
        self.assertIn('first failure a word was wrong at 1024 bytes', lines[1])
        lines = m.quad_data_lines(self.report(state=1, why=2, first=0x84, test_len=1024))
        self.assertIn('refused, value 4', lines[1])
        self.assertEqual(m.quad_data_lines(self.report(state=0)), ["this start's quad data replies: not tried"])

    def test_dropped(self):
        lines = m.quad_data_lines(self.report(state=3, why=7, replies=3, nbytes=196608, failed=1,
                                              fault=(6, 0, 0, 0, 0)))
        self.assertEqual(lines[0], "this start's quad data replies: taken up, then dropped: a data session's "
                         "check word was wrong")
        self.assertTrue(lines[-1].startswith("  broke:    the reply's check word was wrong"))

    def test_show(self):
        class Old:
            def checked(self, payload, timeout=5.0):
                raise SystemExit('monitor: bad request')

        class New:
            def checked(self, payload, timeout=5.0):
                self.asked = payload
                return struct.pack(m.QDATA_LAYOUT, 2, 0, 2, 8, 8, 0, 0, 1000, 1, 65536, 0, 0, 0, 0, 0, 0)
        out = io.StringIO()
        new = New()
        with contextlib.redirect_stdout(out):
            m.show_quad_data(Old())
            m.show_quad_data(new)
        self.assertEqual(new.asked, bytes([m.DEV_QUAD_DATA]))
        self.assertIn('not in this bootloader', out.getvalue().splitlines()[0])
        self.assertIn('taken up', out.getvalue())
        self.assertIn('no report', m.quad_data_lines(b'')[0])


class CalStart(unittest.TestCase):
    """dev-cal-start: one request, OPXY_DEV_CAL_START, answered before the restart."""

    def test_request(self):
        class Unit:
            def checked(self, payload, timeout=5.0):
                self.asked = payload
                return b''
        unit = Unit()
        with contextlib.redirect_stdout(io.StringIO()):
            m.cal_start(unit)
        self.assertEqual(unit.asked, bytes([m.DEV_CAL_START]))
        self.assertEqual(m.DEV_CAL_START, 38)



class Checkpoint(unittest.TestCase):
    """dev-ckpt: OPXY_DEV_CKPT's summary and texts as src/board/opxy_dev_ckpt.c lays them out
    (waffle/devhost.h)."""

    def summary(self, outcome=0, accepted=1, unmounted=1, cut=0, alloc_used=0, blocks=3, reads=410,
                sectors=3000, mount_us=812000, refused=0, scan=1200, ckpt=1200, diffs=0, seq=0x1234,
                alloc_block=17, alloc_page=40, newer=0, disagree=0, test_us=9500000, texts=1):
        r = struct.pack('<5B14IB', outcome, accepted, unmounted, cut, alloc_used, blocks, reads, sectors,
                        mount_us, refused, scan, ckpt, diffs, seq, alloc_block & 0xFFFFFFFF, alloc_page,
                        newer, disagree, test_us, texts)
        self.assertEqual(len(r), m.CKPT_SUMMARY)
        return r

    def test_taken(self):
        lines = m.ckpt_lines(self.summary(), [''])
        self.assertEqual(lines[0], 'checkpoint test: done, 9.500 s in all')
        self.assertEqual(lines[1], 'checkpoint: 3 blocks on the volume; taken by yaffs2')
        self.assertIn('trees: 1200 objects scanned, 1200 restored; 0 differences', lines)
        self.assertTrue(any(l.endswith(': not stale') for l in lines))
        self.assertEqual(lines[-1], 'writes refused: 0; test device unmounted')

    def test_stale(self):
        lines = m.ckpt_lines(self.summary(diffs=1, newer=2, disagree=1, alloc_used=1, texts=3),
                             ['', 'later: only in the scanned mount', 'block 9: restored empty'])
        self.assertIn('  later: only in the scanned mount', lines)
        self.assertIn('  block 9: restored empty', lines)
        self.assertTrue(any('2 blocks newer than it, 1 blocks disagree' in l for l in lines))

    def test_not_taken(self):
        lines = m.ckpt_lines(self.summary(accepted=0, blocks=0, alloc_block=-1),
                             ['no checkpoint on the volume'])
        self.assertEqual(lines[1], 'checkpoint: none on the volume; not taken by yaffs2 '
                                   '(no checkpoint on the volume)')
        self.assertFalse(any(l.startswith('stale guard') for l in lines))

    def test_opcode(self):
        self.assertEqual(m.DEV_CKPT, 42)


if __name__ == '__main__':
    unittest.main(verbosity=1)
