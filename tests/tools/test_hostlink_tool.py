#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of tools/hostlink.py, and of testos_mon.py's dev-put and dev-get over it, against the host link's
own code (src/hostlink.c) built with volumes in memory (hl_sim.py). Python 3, standard library only;
($CC, default cc) builds the library, and without one the tests are skipped.

    test_hostlink_tool.py
"""
import hashlib
import os
import socket
import struct
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(TOP, 'tools'))
sys.path.insert(0, HERE)
import hostlink as hl  # noqa: E402
import hl_sim  # noqa: E402
import testos_mon  # noqa: E402

_dir = tempfile.TemporaryDirectory()
try:
    LIB = hl_sim.build(_dir.name)
except Exception:   # noqa: BLE001 - no compiler that builds it: skipped
    LIB = None


@unittest.skipUnless(LIB, 'no C compiler')
class Tool(unittest.TestCase):
    def client(self, **kw):
        self.unit = hl_sim.Unit(LIB, **kw)
        return hl.Client(hl_sim.Port(self.unit))

    def test_hello(self):
        c = self.client(user=True)
        h = c.hello()
        self.assertEqual((h['version'], h['oldest_client'], h['host_link'], h['session']),
                         (1, 1, True, 'none'))
        self.assertEqual(h['info']['bootloader'], '0.9.49+0')
        self.assertEqual(h['info']['device'], 'OP-XY')
        self.assertEqual(h['info']['running_slot'], 2)
        self.assertIn(hl.RUN, h['calls'])
        self.assertEqual(sorted(h['actions']), [1, 2, 3, 4, 5, 6])
        self.assertEqual([(r['name'], r['flags']) for r in h['roots']],
                         [('yaffs2', ['read', 'write', 'open list']), ('os', ['read']), ('user', ['read', 'write'])])
        # without the user drive, two roots
        self.assertEqual([r['name'] for r in self.client().hello()['roots']], ['yaffs2', 'os'])

    def test_off(self):
        c = self.client(on=False)
        self.assertEqual(c.hello(), {'version': 1, 'oldest_client': 1, 'host_link': False})
        with self.assertRaises(hl.HostLinkError) as e:
            c.ls('/yaffs2')
        self.assertEqual(e.exception.status, 1)

    def test_approval(self):
        c = self.client(answer=None)
        with self.assertRaises(hl.HostLinkError) as e:
            c.mkdir('/yaffs2/d')
        self.assertEqual(e.exception.status, 2)
        self.assertIn('hold 1 on the unit', str(e.exception))
        # listing the drive is open; the other roots are not
        self.assertEqual([x['name'] for x in c.ls('/yaffs2')], ['lost+found'])
        with self.assertRaises(hl.HostLinkError):
            c.ls('/os')
        self.assertEqual(c.approve(0.3), 'pending')
        LIB.sim_answer(3)
        self.assertEqual(c.session(), 'refused')
        self.unit.answer = 2
        self.assertEqual(c.approve(1.0), 'approved')
        c.mkdir('/yaffs2/d')
        self.assertEqual([x['name'] for x in c.ls('/os')], ['boot1', 'boot2'])
        c.end()
        self.assertEqual(c.session(), 'none')

    def test_files(self):
        c = self.client()
        c.approve(1.0)
        data = bytes((i * 13) & 0xFF for i in range(1000))
        c.put('/yaffs2/sub.bin', data)
        self.assertEqual(self.unit.file('/yaffs2/sub.bin'), data)
        self.assertIsNone(self.unit.file('/yaffs2/sub.bin.wbpart'))
        self.assertEqual(c.stat('/yaffs2/sub.bin'), {'type': 'file', 'size': 1000, 'mtime': 0})
        self.assertEqual(c.get('/yaffs2/sub.bin'), data)
        self.assertEqual(c.get('/yaffs2/sub.bin', 900), data[900:])
        self.assertEqual(c.hash('/yaffs2/sub.bin'), (hashlib.sha256(data).digest(), 1000))
        c.mkdir('/yaffs2/dir')
        c.mv('/yaffs2/sub.bin', '/yaffs2/dir/x.bin')
        self.assertEqual(c.ls('/yaffs2/dir'), [{'type': 'file', 'size': 1000, 'name': 'x.bin'}])
        c.rm('/yaffs2/dir/x.bin')
        c.rm('/yaffs2/dir')
        # a byte that arrives wrong: refused, the old file kept
        c.put('/yaffs2/os.ldr', b'old')
        LIB.sim_corrupt()
        with self.assertRaises(hl.HostLinkError) as e:
            c.put('/yaffs2/os.ldr', data)
        self.assertEqual(e.exception.status, 11)
        self.assertEqual(self.unit.file('/yaffs2/os.ldr'), b'old')
        # the OS copies: read only
        LIB.sim_os(1, b'\x03\x00\x00\x00' + bytes(508) + b'OS', 514)
        self.assertEqual(c.get('/os/boot1'), b'\x03\x00\x00\x00' + bytes(508) + b'OS')
        with self.assertRaises(hl.HostLinkError) as e:
            c.put('/os/boot1', data)
        self.assertEqual(e.exception.status, 14)
        with self.assertRaises(hl.HostLinkError) as e:
            c.get('/yaffs2/../os/boot1')
        self.assertEqual(e.exception.status, 6)

    def test_user_drive(self):
        c = self.client(user=True)
        with self.assertRaises(hl.HostLinkError) as e:
            c.ls('/user')
        self.assertEqual(e.exception.status, 2)     # even a listing needs the approval
        c.approve(1.0)
        data = bytes((i * 31) & 0xFF for i in range(5000))
        c.mkdir('/user/samples')
        c.put('/user/samples/kick.wav', data)
        self.assertEqual(c.ls('/user/samples'), [{'type': 'file', 'size': 5000, 'name': 'kick.wav'}])
        self.assertEqual(c.get('/user/samples/kick.wav'), data)
        self.assertEqual(c.hash('/user/samples/kick.wav'), (hashlib.sha256(data).digest(), 5000))
        self.assertGreater(LIB.sim_user_flushes(), 0)
        # a put over a file replaces it; a byte wrong on the way leaves the old one
        c.put('/user/samples/kick.wav', b'short')
        LIB.sim_corrupt()
        with self.assertRaises(hl.HostLinkError) as e:
            c.put('/user/samples/kick.wav', data)
        self.assertEqual(e.exception.status, 11)
        self.assertEqual(c.get('/user/samples/kick.wav'), b'short')
        self.assertEqual([x['name'] for x in c.ls('/user/samples')], ['kick.wav'])
        # a put cut short: at most its temporary file, which can be removed
        c.call(hl.PUT_OPEN, struct.pack('<I', len(data)) + hashlib.sha256(data).digest()
               + b'/user/half.wav')
        c.call(hl.PUT_DATA, struct.pack('<I', 0) + data[:224])
        self.assertIn('half.wav.wbpart', [x['name'] for x in c.ls('/user')])
        c.rm('/user/half.wav.wbpart')
        c.mv('/user/samples/kick.wav', '/user/kick.wav')
        with self.assertRaises(hl.HostLinkError) as e:
            c.mv('/user/kick.wav', '/yaffs2/kick.wav')
        self.assertEqual(e.exception.status, 6)
        c.rm('/user/kick.wav')
        c.rm('/user/samples')
        self.assertEqual(c.ls('/user'), [])

    def test_user_unreadable(self):
        c = self.client(user=True)
        c.approve(1.0)
        LIB.sim_user(1)     # no volume FatFs reads (exFAT is one)
        with self.assertRaises(hl.HostLinkError) as e:
            c.ls('/user')
        self.assertEqual(e.exception.status, 16)
        self.assertIn('not readable by this build', str(e.exception))

    def test_many_entries(self):
        c = self.client()
        for i in range(40):
            self.unit.put_file('/yaffs2/file-with-a-longer-name-%02d' % i, b'x' * i)
        names = [e['name'] for e in c.ls('/yaffs2')]
        self.assertEqual(len(names), 41)
        self.assertEqual(names[-1], 'file-with-a-longer-name-39')

    def test_resume(self):
        c = self.client()
        c.approve(1.0)
        data = bytes(range(256)) * 4
        digest = hashlib.sha256(data).digest()
        c.call(hl.PUT_OPEN, struct.pack('<I', len(data)) + digest + b'/yaffs2/big')
        c.call(hl.PUT_DATA, struct.pack('<I', 0) + data[:224])
        # the link dropped: the same put again goes on from where the unit stands
        c.put('/yaffs2/big', data)
        self.assertEqual(self.unit.file('/yaffs2/big'), data)

    def test_jobs(self):
        c = self.client()
        c.approve(1.0)
        self.unit.put_file('/yaffs2/pkg.bin', b'package')
        seen = []
        c.on_event = seen.append
        rec = c.run(hl.A_INSTALL, 0x03, '/yaffs2/pkg.bin')
        self.assertEqual((rec['state'], rec['action']), ('queued', hl.A_INSTALL))
        self.assertEqual(self.unit.last_run(), (hl.A_INSTALL, 3, '/yaffs2/pkg.bin'))
        rec = c.wait(rec, 5.0)
        self.assertEqual((rec['state'], rec['text'], rec['percent']), ('done', '[done]', 100))
        self.assertEqual([r['state'] for r in seen], ['running', 'done'])
        LIB.sim_fail_next()
        rec = c.wait(c.run(hl.A_OS_WRITE, None, '/yaffs2/pkg.bin'), 5.0)
        self.assertEqual((rec['state'], rec['error'], rec['text']), ('failed', 15, 'PRODUCT: incorrect sku'))
        with self.assertRaises(hl.HostLinkError) as e:
            c.run(0x10)
        self.assertEqual(e.exception.status, 3)
        with self.assertRaises(hl.HostLinkError) as e:
            c.run(hl.A_INSTALL, 0xFF, '/os/boot1')
        self.assertEqual(e.exception.status, 6)

    def test_host_link_off(self):
        c = self.client()
        c.approve(1.0)
        c.set_host_link(False)
        self.assertFalse(c.hello()['host_link'])


@unittest.skipUnless(LIB, 'no C compiler')
class DevPut(unittest.TestCase):
    """testos_mon.py's dev-put and dev-get, by the host link on the monitor's port."""

    def setUp(self):
        self.unit = hl_sim.Unit(LIB)
        a, self.b = socket.socketpair()
        self.mon = type('Mon', (), {'fd': a.fileno()})()
        self.keep = a
        self.t = threading.Thread(target=self.serve, daemon=True)
        self.t.start()

    def tearDown(self):
        self.keep.close()
        self.b.close()

    def serve(self):
        try:
            while True:
                d = self.b.recv(4096)
                if not d:
                    return
                self.b.sendall(self.unit.feed(d))
        except OSError:
            return

    def test_put_and_get(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b'an os image' * 50)
        try:
            self.assertEqual(testos_mon.host_file(self.mon, 'dev-put', [f.name]), 0)
            self.assertEqual(self.unit.file('/yaffs2/os.ldr'), b'an os image' * 50)
            self.assertEqual(testos_mon.host_file(self.mon, 'dev-get', []), 0)
            self.assertEqual(testos_mon.host_file(self.mon, 'dev-put', [f.name, '--as', 'x.ldr']), 0)
            self.assertIsNotNone(self.unit.file('/yaffs2/x.ldr'))
        finally:
            os.unlink(f.name)

    def test_refused(self):
        self.unit.answer = 3
        with self.assertRaises(SystemExit):
            testos_mon.host_file(self.mon, 'dev-get', [])


if __name__ == '__main__':
    unittest.main()
