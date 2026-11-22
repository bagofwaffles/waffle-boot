#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The host link's protocol (src/hostlink.c) over volumes in memory (hl_sim.c), built as a shared library,
with the unit's side of the framing: tests/tools/test_hostlink.py and tests/web/hl_bridge.py drive it.

    lib = build(tmpdir)        # None with no C compiler
    u = Unit(lib)              # fresh: Host link on, no user drive, the drive holding lost+found
    wire = u.feed(bytes)       # the host's bytes in, the unit's frames out

feed() answers each frame of id 0x7C; with `answer` set, an approval the unit asks for is answered so,
and with `jobs`, a job queued is moved on to its end, each change an HL_EVENT frame after the reply.
Python 3, standard library only.
"""
import ctypes
import os
import shutil
import subprocess
import sys

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(TOP, 'tools'))
from testos_mon import frame, unframe  # noqa: E402

ID = 0x7C
REPLY_ROUTE = 0x12
BEARSSL = os.path.join(TOP, 'src', 'third_party', 'bearssl')


def build(d):
    cc = os.environ.get('CC', 'cc')
    if not shutil.which(cc):
        return None
    out = os.path.join(d, 'hl_sim.so')
    srcs = [os.path.join(TOP, 'tests', 'tools', 'hl_sim.c'), os.path.join(TOP, 'src', 'hostlink.c'),
            os.path.join(BEARSSL, 'src', 'hash', 'sha2small.c'),
            os.path.join(BEARSSL, 'src', 'codec', 'dec32be.c'),
            os.path.join(BEARSSL, 'src', 'codec', 'enc32be.c')]
    subprocess.check_call([cc, '-std=c11', '-O1', '-shared', '-fPIC', '-I', os.path.join(TOP, 'src', 'include'),
                           '-I', os.path.join(BEARSSL, 'inc'), '-I', os.path.join(BEARSSL, 'src')]
                          + srcs + ['-o', out])
    lib = ctypes.CDLL(out)
    lib.sim_request.restype = ctypes.c_size_t
    lib.sim_request.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]
    lib.sim_step.restype = ctypes.c_size_t
    lib.sim_step.argtypes = [ctypes.c_char_p]
    lib.sim_put.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
    lib.sim_get.restype = ctypes.c_long
    lib.sim_get.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
    lib.sim_os.argtypes = [ctypes.c_uint, ctypes.c_char_p, ctypes.c_size_t]
    lib.sim_last_run.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_uint8),
                                 ctypes.c_char_p, ctypes.c_size_t]
    return lib


class Unit:
    """One unit at a time: the library holds one."""

    def __init__(self, lib, on=True, user=False, answer=2, jobs=True):
        self.lib, self.answer, self.jobs = lib, answer, jobs
        self.rx = bytearray()
        self.seq = 0
        self.requests = []
        lib.sim_reset(1 if on else 0, 1 if user else 0)

    def request(self, payload):
        out = ctypes.create_string_buffer(256)
        n = self.lib.sim_request(bytes(payload), len(payload), out)
        return out.raw[:n]

    def _frame(self, payload):
        self.seq = (self.seq + 1) & 0xFF
        return frame(payload, self.seq, ident=ID, route=REPLY_ROUTE)

    def feed(self, data):
        self.rx += data
        out = bytearray()
        while b'\0' in self.rx:
            wire, _, rest = bytes(self.rx).partition(b'\0')
            self.rx = bytearray(rest)
            got = unframe(wire)
            if not got or got[0] != ID:
                continue
            self.requests.append(got[1])
            out += self._frame(self.request(got[1]))
            if self.answer is not None and self.lib.sim_pending():
                self.lib.sim_answer(self.answer)
            while self.jobs:
                ev = ctypes.create_string_buffer(256)
                n = self.lib.sim_step(ev)
                if n == 0:
                    break
                out += self._frame(ev.raw[:n])
        return bytes(out)

    def put_file(self, path, data, d=False):
        self.lib.sim_put(path.encode(), bytes(data), len(data), 1 if d else 0)

    def file(self, path):
        buf = ctypes.create_string_buffer(1 << 20)
        n = self.lib.sim_get(path.encode(), buf, len(buf))
        return None if n < 0 else buf.raw[:n]

    def last_run(self):
        a, g = ctypes.c_uint8(), ctypes.c_uint8()
        p = ctypes.create_string_buffer(256)
        self.lib.sim_last_run(ctypes.byref(a), ctypes.byref(g), p, len(p))
        return a.value, g.value, p.value.decode()


class Port:
    """A Client's transport straight onto a Unit."""

    def __init__(self, unit):
        self.unit, self.pending = unit, bytearray()

    def write(self, data):
        self.pending += self.unit.feed(data)

    def read(self, timeout):
        out, self.pending = bytes(self.pending), bytearray()
        return out
