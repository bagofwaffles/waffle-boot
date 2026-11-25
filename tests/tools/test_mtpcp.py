#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host tests of tools/mtpcp.py, with no USB: its protocol against waffle-boot's real responder
(src/core/mtp/mtp.c, at levels R and W, built here through tests/tools/test_mtpcp_shim.c), and against a writable fake
responder whose faults, each a departure from an S-16 rule a test turns on, show the tool stays safe with any
responder. A compiler is needed ($CC, or cc). Python 3, standard library only.

    test_mtpcp.py
"""
import ctypes
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(TOP, 'tools')
sys.path.insert(0, TOOLS)
import mtpcp  # noqa: E402

ALLOWED = {0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1007, 0x1008, 0x1009, 0x100B, 0x100C, 0x100D, 0x1019,
           0x9804}
CHANGES = {0x100B, 0x100C, 0x100D, 0x1019, 0x9804}


def packets(transport_read_queue, size, mps):
    """One bulk transfer: packets up to `size`, ended early by a short one."""
    out = b''
    while transport_read_queue and len(out) < size:
        p = transport_read_queue.pop(0)
        out += p
        if len(p) < mps:
            break
    return out


class Spy:
    """Records the operation codes a transport is given; commands are told by their container type."""

    def __init__(self):
        self.ops = []

    def note(self, data):
        if len(data) >= 12 and struct.unpack_from('<H', data, 4)[0] == mtpcp.CMD:
            self.ops.append(struct.unpack_from('<H', data, 6)[0])


# ---------------------------------------------------------------- waffle-boot's engine

class Engine(Spy):
    lib = None

    @classmethod
    def build(cls):
        if cls.lib is not None:
            return cls.lib
        cls.dir = tempfile.TemporaryDirectory()
        out = os.path.join(cls.dir.name, 'shim.so')
        subprocess.check_call(
            [os.environ.get('CC', 'cc'), '-std=c11', '-shared', '-fPIC', '-I', os.path.join(TOP, 'src', 'include'),
             '-o', out, os.path.join(HERE, 'test_mtpcp_shim.c')]
            + [os.path.join(TOP, 'src', 'core', n) for n in ('mtp/mtp.c', 'mtp/text.c', 'boot/records.c',
                                                     'host/text.c', 'util/div64.c', 'install/common.c', 'util/version.c')])
        lib = ctypes.CDLL(out)
        lib.shim_add.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_int]
        lib.shim_start.argtypes = [ctypes.c_size_t, ctypes.c_int]
        lib.shim_get.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
        lib.shim_get.restype = ctypes.c_long
        lib.shim_rx_size.restype = ctypes.c_size_t
        lib.shim_rx.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        lib.shim_tx.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        lib.shim_tx.restype = ctypes.c_long
        cls.lib = lib
        return lib

    def __init__(self, files, mps=512, level=0):
        super().__init__()
        self.mps = mps
        self.c = self.build()
        self.keep = []
        self.c.shim_reset_files()
        for name, data in files.items():
            n = name.encode()
            self.keep += [n, data]
            self.c.shim_add(n, data or b'', len(data or b''), data is None)
        self.c.shim_start(mps, level)

    def get(self, path):
        """The bytes of /path on the volume, None when absent, 'folder' for a folder."""
        buf = ctypes.create_string_buffer(1 << 20)
        n = self.c.shim_get(path.encode(), buf, len(buf))
        return None if n == -1 else 'folder' if n == -2 else buf.raw[:n]

    def write(self, data, timeout_ms=0):
        self.note(data)
        pos = 0
        while pos < len(data):
            k = self.c.shim_rx_size()
            assert k, 'the engine is not taking data'
            k = min(k, len(data) - pos)
            self.c.shim_rx(bytes(data[pos:pos + k]), k)
            pos += k

    def read(self, size, timeout_ms=0):
        buf = ctypes.create_string_buffer(self.mps)
        out = b''
        while len(out) < size:
            n = self.c.shim_tx(buf, self.mps)
            if n < 0:
                if out:
                    return out
                raise mtpcp.Timeout('nothing to read')
            out += buf.raw[:n]
            if n < self.mps:
                return out
        return out


class AgainstTheEngine(unittest.TestCase):
    def test_ls(self):
        e = Engine({'os.ldr': b'x' * 293900, 'lost+found': None, 'kick.wav': b'k'})
        out = io.StringIO()
        mtpcp.cmd_ls(mtpcp.Mtp(e), out)
        text = out.getvalue()
        self.assertIn('293900', text)
        self.assertIn('os.ldr', text)
        self.assertIn('lost+found/', text)
        self.assertIn('kick.wav', text)
        self.assertIn('3 objects, 1048576 bytes free of 2403870720', text)
        self.assertIn('OP-XY', text)
        self.assertLessEqual(set(e.ops), ALLOWED)

    def test_verify_every_framing(self):
        # 500 and 1012: the container is a multiple of the packet, so a zero-length packet ends it.
        for mps in (512, 64):
            for size in (1, 52, 116, 500, 1012, 1024, 16372, 293900):
                content = bytes(i % 251 for i in range(size))
                e = Engine({'os.ldr': content}, mps)
                out = io.StringIO()
                mtpcp.cmd_verify(mtpcp.Mtp(e), 'os.ldr', content, out)
                self.assertIn('identical', out.getvalue())
                self.assertLessEqual(set(e.ops), ALLOWED)

    def test_verify_fails_on_a_different_byte(self):
        content = bytes(1000)
        bad = content[:700] + b'\1' + content[701:]
        with self.assertRaisesRegex(mtpcp.Failure, 'offset 700'):
            mtpcp.cmd_verify(mtpcp.Mtp(Engine({'os.ldr': bad})), 'os.ldr', content, io.StringIO())

    def test_verify_fails_when_absent(self):
        with self.assertRaisesRegex(mtpcp.Failure, 'not on the unit'):
            mtpcp.cmd_verify(mtpcp.Mtp(Engine({'a': b'1'})), 'os.ldr', b'1', io.StringIO())

    def test_put_stops_at_a_read_only_responder(self):
        e = Engine({'os.ldr': b'old'})
        with tempfile.NamedTemporaryFile() as f:
            f.write(b'new')
            f.flush()
            with self.assertRaisesRegex(mtpcp.Failure, 'nothing was changed'):
                mtpcp.cmd_put(mtpcp.Mtp(e), f.name, 'os.ldr', b'new', io.StringIO())
        self.assertFalse(CHANGES & set(e.ops))

    def test_level_r_refuses_every_change(self):
        for run in (lambda m: mtpcp.cmd_rm(m, 'a.txt', io.StringIO()),
                    lambda m: mtpcp.cmd_mkdir(m, 'new', io.StringIO()),
                    lambda m: mtpcp.cmd_mv(m, 'a.txt', 'b.txt', io.StringIO())):
            e = Engine({'a.txt': b'1', 'dir': None})
            with self.assertRaisesRegex(mtpcp.Failure, 'nothing was changed'):
                run(mtpcp.Mtp(e))
            self.assertFalse(CHANGES & set(e.ops))
            self.assertEqual(e.get('/a.txt'), b'1')

    def test_put_at_level_w(self):
        new = bytes(i * 5 % 256 for i in range(70000))
        for mps in (512, 64):
            e = Engine({'os.ldr': b'old image', 'lost+found': None, 'keep.wav': b'k'}, mps, level=1)
            out = io.StringIO()
            with tempfile.NamedTemporaryFile() as f:
                f.write(new)
                f.flush()
                mtpcp.cmd_put(mtpcp.Mtp(e), f.name, 'os.ldr', new, out)
            self.assertIn('identical', out.getvalue())
            self.assertEqual(e.get('/os.ldr'), new)
            self.assertEqual(e.get('/keep.wav'), b'k')
            self.assertLessEqual(set(e.ops), ALLOWED)

    def test_rm_mkdir_mv_at_level_w(self):
        e = Engine({'a.txt': b'abc', 'dir': None, 'lost+found': None}, level=1)
        m = mtpcp.Mtp(e)
        mtpcp.cmd_mkdir(m, 'dir/sub', io.StringIO())
        self.assertEqual(e.get('/dir/sub'), 'folder')
        with self.assertRaisesRegex(mtpcp.Failure, 'already on the unit'):
            mtpcp.cmd_mkdir(m, 'dir/sub', io.StringIO())
        with self.assertRaisesRegex(mtpcp.Failure, 'not a folder'):
            mtpcp.cmd_mkdir(m, 'none/sub', io.StringIO())
        mtpcp.cmd_mv(m, 'a.txt', 'dir/sub', io.StringIO())            # into a folder
        self.assertEqual(e.get('/dir/sub/a.txt'), b'abc')
        self.assertIsNone(e.get('/a.txt'))
        mtpcp.cmd_mv(m, 'dir/sub/a.txt', 'dir/sub/b.txt', io.StringIO())   # a rename
        self.assertEqual(e.get('/dir/sub/b.txt'), b'abc')
        mtpcp.cmd_mv(m, 'dir/sub/b.txt', 'c.txt', io.StringIO())       # moved and renamed
        self.assertEqual(e.get('/c.txt'), b'abc')
        mtpcp.cmd_mv(m, 'dir', 'top', io.StringIO())                   # a folder, with what is in it
        self.assertEqual(e.get('/top/sub'), 'folder')
        with self.assertRaisesRegex(mtpcp.Failure, 'beneath itself'):
            mtpcp.cmd_mv(m, 'top', 'top/sub/x', io.StringIO())
        mtpcp.cmd_mkdir(m, 'top/c.txt', io.StringIO())
        with self.assertRaisesRegex(mtpcp.Failure, 'already on the unit'):
            mtpcp.cmd_mv(m, 'c.txt', 'top', io.StringIO())             # top/c.txt is there
        with self.assertRaisesRegex(mtpcp.Failure, 'recursive'):
            mtpcp.cmd_rm(m, 'top', io.StringIO())
        mtpcp.cmd_rm(m, 'c.txt', io.StringIO())
        self.assertIsNone(e.get('/c.txt'))
        with self.assertRaisesRegex(mtpcp.Failure, 'MoveObject answered Object_WriteProtected'):
            mtpcp.cmd_mv(m, 'lost+found', 'top', io.StringIO())
        self.assertEqual(e.get('/lost+found'), 'folder')
        self.assertLessEqual(set(e.ops), ALLOWED)


# ---------------------------------------------------------------- a writable responder, faults on request

class Factory(Spy):
    """A writable responder; each fault a test turns on departs from an S-16 rule."""

    def __init__(self, files=None, free=1 << 30, mps=512, **faults):
        super().__init__()
        self.mps = mps
        self.files = dict(files or {})     # name: bytes, or None for a folder
        self.free = free
        self.handles = {}
        self.next_handle = 0x100
        self.q = []
        self.pending = None                 # (op, tid) of a command waiting for data
        self.buf = b''
        self.announced = None
        self.session = 0
        self.faults = faults
        self.stale_tid = None
        for name in self.files:
            self.handle(name)

    def handle(self, name):
        if name not in self.handles:
            self.handles[name] = self.next_handle
            self.next_handle += 1
        return self.handles[name]

    def name_of(self, h):
        return next((n for n, v in self.handles.items() if v == h and n in self.files), None)

    def queue(self, kind, code, tid, payload=b'', length=None):
        blob = struct.pack('<IHHI', 12 + len(payload) if length is None else length, kind, code,
                           tid) + payload
        for off in range(0, len(blob), self.mps):
            self.q.append(blob[off:off + self.mps])
        if len(blob) % self.mps == 0:
            self.q.append(b'')

    def respond(self, code, tid, *params):
        self.q.append(struct.pack('<IHHI%dI' % len(params), 12 + 4 * len(params), mtpcp.RESP, code, tid,
                                  *params))

    def object_info(self, name):
        body = self.files[name]
        fmt = mtpcp.FORMAT_FOLDER if body is None else mtpcp.FORMAT_UNDEFINED
        return (struct.pack('<IHHIHIIIIIIIHII', 0x10001, fmt, 0, 0 if body is None else len(body),
                            0, 0, 0, 0, 0, 0, 0, 0, 1 if body is None else 0, 0, 0)
                + mtpcp.mtp_string(name) + mtpcp.mtp_string('20261106T000000') * 2 + b'\0')

    # the host's side
    def write(self, data, timeout_ms=0):
        self.note(data)
        data = bytes(data)
        if self.pending is None:
            if not data:
                return
            _, kind, code, tid = struct.unpack_from('<IHHI', data)
            assert kind == mtpcp.CMD
            params = struct.unpack('<%dI' % ((len(data) - 12) // 4), data[12:])
            self.params = params
            if code in (0x100C, 0x100D):
                self.pending = (code, tid)
                self.buf = b''
            else:
                self.command(code, tid, params)
            return
        self.buf += data
        total = struct.unpack_from('<I', self.buf)[0]
        # A multiple of the packet size is followed by a zero-length one; the fake takes either.
        if len(self.buf) >= total and (total % self.mps or not data):
            code, tid = self.pending
            self.pending = None
            self.data_phase(code, tid, self.buf[12:total])

    def read(self, size, timeout_ms=0):
        if not self.q:
            raise mtpcp.Timeout('nothing to read')
        return packets(self.q, size, self.mps)

    # the responder
    def command(self, code, tid, p):
        assert code in ALLOWED, 'the tool sent %#x, an operation outside its list' % code
        if self.stale_tid is not None:              # S-16.064
            self.queue(mtpcp.DATA, 0x1008, self.stale_tid, b'stale block')
            self.stale_tid = None
        if code == 0x1001:
            info = (struct.pack('<HIH', 100, 6, 100) + mtpcp.mtp_string('microsoft.com: 1.0;')
                    + struct.pack('<H', 0) + struct.pack('<I2H', 2, 0x1001, 0x1002)
                    + struct.pack('<I', 0) * 4 + mtpcp.mtp_string('teenage engineering')
                    + mtpcp.mtp_string('OP-XY') + mtpcp.mtp_string('9.9.9') + mtpcp.mtp_string('s'))
            self.queue(mtpcp.DATA, code, tid, info)
            self.respond(mtpcp.OK, tid)
        elif code == 0x1002:
            if self.session:                        # without the ID S-16.176 gives
                self.respond(mtpcp.ALREADY_OPEN, tid)
            else:
                self.session = p[0]
                self.respond(mtpcp.OK, tid)
        elif code == 0x1003:
            self.session = 0
            self.respond(mtpcp.OK, tid)
        elif code == 0x1004:
            self.queue(mtpcp.DATA, code, tid, struct.pack('<II', 1, 0x10001))
            self.respond(mtpcp.OK, tid)
        elif code == 0x1005:
            used = sum(len(v) for v in self.files.values() if v)
            self.queue(mtpcp.DATA, code, tid, struct.pack('<HHHQQI', 3, 2, 0, 2403870720,
                                                          max(self.free - used, 0), 0xFFFFFFFF)
                       + mtpcp.mtp_string('OP-XY') + b'\0')
            self.respond(mtpcp.OK, tid)
        elif code == 0x1007:
            assert p[2] == mtpcp.ROOT
            if self.faults.get('no_list_data'):
                self.respond(mtpcp.OK, tid)
                return
            hs = [self.handle(n) for n in self.files]
            self.queue(mtpcp.DATA, code, tid, struct.pack('<I%dI' % len(hs), len(hs), *hs))
            self.respond(mtpcp.OK, tid)
            if self.faults.get('stale_after_list'):
                self.stale_tid = tid
        elif code == 0x1008:
            info = self.object_info(self.name_of(p[0]))
            # info_length_off departs from S-16.056: the length does not match the bytes sent.
            wrong = len(info) + 12 + self.faults.get('info_length_off', 0)
            self.queue(mtpcp.DATA, code, tid, info, length=wrong)
            self.respond(mtpcp.OK, tid)
        elif code == 0x1009:
            body = self.files[self.name_of(p[0])]
            if self.faults.get('corrupt_read'):
                body = body[:-1] + bytes([body[-1] ^ 1])
            if self.faults.get('cannot_open'):      # departs from S-16.064: a data phase, and success
                body = b''
            self.queue(mtpcp.DATA, code, tid, body)
            self.respond(mtpcp.OK, tid)
        elif code == 0x100B:
            name = self.name_of(p[0])
            if self.faults.get('delete_refused'):
                self.respond(0x2002, tid)
            elif not self.faults.get('delete_ignored'):
                del self.files[name]
                self.respond(mtpcp.OK, tid)
            else:
                self.respond(mtpcp.OK, tid)

    def data_phase(self, code, tid, payload):
        if code == 0x100C:
            if self.faults.get('root_is_zero') and self.params[1] != 0:
                self.respond(mtpcp.INVALID_PARENT, tid)
                return
            info = mtpcp.Object(0, payload)
            self.announced = info.name
            self.files[info.name] = b''             # created empty at once, unlike S-16.184
            self.handle(info.name)
            if self.faults.get('send_info_refused'):
                del self.files[info.name]
                self.respond(0x2002, tid)
                return
            self.respond(mtpcp.OK, tid, 0x10001, 0xFFFFFFFF, self.handle(info.name))
        else:
            if self.faults.get('send_refused'):     # leaves the empty file
                self.respond(0x2002, tid)
                return
            body = payload
            if self.faults.get('truncate'):
                body = body[:self.faults['truncate']]
            if self.faults.get('corrupt_write'):
                body = body[:9] + bytes([body[9] ^ 0x40]) + body[10:]
            self.files[self.announced] = body
            self.respond(mtpcp.OK, tid)


class AgainstTheFactory(unittest.TestCase):
    NEW = bytes(i * 7 % 256 for i in range(293900))

    def put(self, device, content=None, name='os.ldr'):
        out = io.StringIO()
        with tempfile.NamedTemporaryFile() as f:
            f.write(content if content is not None else self.NEW)
            f.flush()
            mtpcp.cmd_put(mtpcp.Mtp(device), f.name, name, content if content is not None else self.NEW,
                          out)
        self.assertLessEqual(set(device.ops), ALLOWED)
        return out.getvalue()

    def test_put_replaces_and_verifies(self):
        d = Factory({'os.ldr': b'old image', 'lost+found': None, 'keep.wav': b'k'})
        text = self.put(d)
        self.assertEqual(d.files['os.ldr'], self.NEW)
        self.assertEqual(d.files['keep.wav'], b'k')
        self.assertIn('lost+found', d.files)
        self.assertIn('deleted /yaffs2/os.ldr (9 bytes)', text)
        self.assertIn('identical', text)
        self.assertIn('bytes free of 2403870720', text)
        # The read-back comes after a new session, and the upload before it.
        self.assertLess(d.ops.index(0x100D), len(d.ops) - 1 - d.ops[::-1].index(0x1009))
        self.assertLess(d.ops.index(0x100D), d.ops.index(0x1003))

    def test_put_with_nothing_to_delete(self):
        d = Factory()
        self.put(d)
        self.assertNotIn(0x100B, d.ops)
        self.assertEqual(d.files['os.ldr'], self.NEW)

    def test_container_lengths_needing_a_zero_length_packet(self):
        for size in (500, 1012, 1012 + 512, 512 * 128 - 12):
            content = bytes(size)
            d = Factory()
            self.put(d, content)
            self.assertEqual(d.files['os.ldr'], content)

    def test_full_speed(self):
        d = Factory(mps=64)
        self.put(d, self.NEW[:5000])
        self.assertEqual(d.files['os.ldr'], self.NEW[:5000])

    def test_wrong_object_info_length(self):
        for off in (40, -10):                       # S-16.056
            d = Factory({'os.ldr': b'old'}, info_length_off=off)
            self.assertIn('identical', self.put(d))

    def test_stale_data_block_is_dropped(self):     # S-16.064
        text = io.StringIO()
        d = Factory({'os.ldr': b'old'}, stale_after_list=True)
        mtp = mtpcp.Mtp(d)
        with tempfile.NamedTemporaryFile() as f:
            f.write(self.NEW)
            f.flush()
            mtpcp.cmd_put(mtp, f.name, 'os.ldr', self.NEW, text)
        self.assertGreater(mtp.stale, 0)
        self.assertEqual(d.files['os.ldr'], self.NEW)

    def test_root_as_zero(self):
        d = Factory(root_is_zero=True)
        self.put(d)
        self.assertEqual(d.files['os.ldr'], self.NEW)

    def assert_fails(self, device, pattern):
        with self.assertRaisesRegex(mtpcp.Failure, pattern):
            self.put(device)

    def test_flipped_byte_on_write_fails(self):
        self.assert_fails(Factory(corrupt_write=True), 'MISMATCH(.|\n)*offset 9(.|\n)*do NOT')

    def test_flipped_byte_on_read_fails(self):
        self.assert_fails(Factory(corrupt_read=True), 'MISMATCH(.|\n)*do NOT')

    def test_short_file_fails(self):
        self.assert_fails(Factory(truncate=65536), 'MISMATCH(.|\n)*listed 65536')

    def test_cannot_open_success_is_not_believed(self):
        self.assert_fails(Factory(cannot_open=True), 'read back 0(.|\n)*do NOT')

    def test_refused_send_warns_of_the_empty_file(self):
        d = Factory(send_refused=True)
        self.assert_fails(d, 'SendObject answered General_Error(.|\n)*do NOT')
        self.assertEqual(d.files['os.ldr'], b'')

    def test_refused_send_info(self):
        self.assert_fails(Factory(send_info_refused=True), 'SendObjectInfo answered')

    def test_delete_refused(self):
        self.assert_fails(Factory({'os.ldr': b'old'}, delete_refused=True), 'DeleteObject answered')

    def test_delete_ignored(self):
        self.assert_fails(Factory({'os.ldr': b'old'}, delete_ignored=True), 'still listed')

    def test_no_data_phase_is_not_an_empty_volume(self):
        self.assert_fails(Factory({'os.ldr': b'old'}, no_list_data=True), 'no data phase')

    def test_a_folder_is_never_deleted(self):
        d = Factory({'os.ldr': None, 'other': b'1'})
        self.assert_fails(d, 'recursive: nothing was changed')
        self.assertNotIn(0x100B, d.ops)

    def test_no_room_changes_nothing(self):
        d = Factory({'os.ldr': bytes(10)}, free=1000)
        self.assert_fails(d, 'do not fit(.|\n)*nothing was changed')
        self.assertFalse({0x100B, 0x100C, 0x100D} & set(d.ops))

    def test_room_counts_the_file_replaced(self):
        d = Factory({'os.ldr': bytes(len(self.NEW))}, free=len(self.NEW) + 5)
        self.put(d)
        self.assertEqual(d.files['os.ldr'], self.NEW)

    def test_only_the_named_file_goes(self):
        d = Factory({'os.ldr': b'old', 'os.ldr.bak': b'keep', 'OS.LDR': b'keep'})
        self.put(d)
        self.assertEqual(d.files['os.ldr.bak'], b'keep')
        self.assertEqual(d.files['OS.LDR'], b'keep')

    def test_rm(self):
        d = Factory({'os.ldr': b'old', 'wbtest.bin': b'1', 'wbtest.bin.bak': b'keep'})
        mtpcp.cmd_rm(mtpcp.Mtp(d), 'wbtest.bin', io.StringIO())
        self.assertNotIn('wbtest.bin', d.files)
        self.assertEqual(d.files['wbtest.bin.bak'], b'keep')
        self.assertLessEqual(set(d.ops), ALLOWED)
        d = Factory({'dir': None})
        with self.assertRaisesRegex(mtpcp.Failure, 'recursive'):
            mtpcp.cmd_rm(mtpcp.Mtp(d), 'dir', io.StringIO())
        self.assertNotIn(0x100B, d.ops)
        with self.assertRaisesRegex(mtpcp.Failure, 'still listed'):
            mtpcp.cmd_rm(mtpcp.Mtp(Factory({'a': b'1'}, delete_ignored=True)), 'a', io.StringIO())
        self.assertEqual(mtpcp.main(['rm', 'os.ldr']), 1)


class Parsing(unittest.TestCase):
    def test_names(self):
        for name in ('', 'a/b', '.', '..', 'x' * 255):
            with self.assertRaises(mtpcp.Failure):
                mtpcp.check_name(name)
        mtpcp.check_name('os.ldr')
        mtpcp.check_name('x' * 254)

    def test_mtp_string(self):
        self.assertEqual(mtpcp.mtp_string(''), b'\0')
        self.assertEqual(mtpcp.mtp_string('A'), b'\x02A\0\0\0')

    def test_object_info_round_trip(self):
        o = mtpcp.Object(5, mtpcp.object_info_dataset(0x10001, 'os.ldr', 293900, '20261106T000000'))
        self.assertEqual((o.name, o.size, o.format, o.modified, o.parent),
                         ('os.ldr', 293900, mtpcp.FORMAT_UNDEFINED, '20261106T000000', 0))

    def test_cut_short_dataset(self):
        o = mtpcp.Object(5, struct.pack('<IHHI', 0x10001, 0x3000, 0, 7))
        self.assertEqual((o.size, o.name, o.modified), (7, '', ''))


class Tree(unittest.TestCase):
    """tree's walk and compare, over a stub that answers list and get."""

    class Stub:
        def __init__(self, tree):
            self.objs, self.kids, self.gets = {}, {mtpcp.ROOT: []}, 0
            self.add(mtpcp.ROOT, tree)

        def add(self, parent, tree):
            for name, v in tree.items():
                h = len(self.objs) + 1
                folder = isinstance(v, dict)
                o = mtpcp.Object.__new__(mtpcp.Object)
                o.handle, o.name, o.parent, o.modified = h, name, parent, '20261110T000000'
                o.format = mtpcp.FORMAT_FOLDER if folder else mtpcp.FORMAT_UNDEFINED
                o.size = 0 if folder else len(v)
                self.objs[h] = (o, v)
                self.kids[parent].append(o)
                if folder:
                    self.kids[h] = []
                    self.add(h, v)

        def list_children(self, storage, parent):
            return list(self.kids[parent])

        def get_object(self, handle):
            self.gets += 1
            return self.objs[handle][1]

    def test_walk_and_compare(self):
        a = self.Stub({'os.ldr': b'abc', 'user': {'p1': b'1', 'deep': {'x': b'xx'}}, 'logs': {}})
        ea = mtpcp.walk_tree(a, 1, True, io.StringIO())
        self.assertEqual([e['path'] for e in ea], ['logs', 'os.ldr', 'user', 'user/deep', 'user/deep/x', 'user/p1'])
        self.assertEqual(a.gets, 3)
        x = [e for e in ea if e['path'] == 'user/deep/x'][0]
        self.assertEqual((x['size'], x['read']), (2, 2))
        b = self.Stub({'os.ldr': b'abd', 'user': {'p1': b'1', 'p2': b''}, 'logs': {}})
        eb = mtpcp.walk_tree(b, 1, True, io.StringIO())
        self.assertEqual(mtpcp.compare_trees({'entries': ea}, {'entries': eb}), [
            'differs (sha256): os.ldr', 'only in A: user/deep', 'only in A: user/deep/x', 'only in B: user/p2'])
        self.assertEqual(mtpcp.compare_trees({'entries': ea}, {'entries': ea}), [])
        n = self.Stub({'a': b'1'})
        mtpcp.walk_tree(n, 1, False, io.StringIO())
        self.assertEqual(n.gets, 0)

    def test_a_loop_stops(self):
        s = self.Stub({'d': {}})
        s.kids[1].append(s.objs[1][0])          # the folder lists itself
        with self.assertRaises(mtpcp.Failure):
            mtpcp.walk_tree(s, 1, False, io.StringIO())


if __name__ == '__main__':
    unittest.main()
