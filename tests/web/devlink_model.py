#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A simulated boot menu's developer link (wiki/Developer-Host-Link.md) for the recovery page's Install tab: it
answers the file requests tools/testos_mon.py dev-put sends (PUT open, data and close, then SHA),
with that tool's own frame codec, and keeps every byte it was sent.

    devlink_model.py cases       what testos_mon.py dev-put sends and says in each case, as JSON

A spec (a dict) picks the case: {"kind": "devlink", "case": CASE}. CASES:

    good        the file is stored as sent; SHA answers its digest and size
    mismatch    one byte of the third data request is stored flipped: the SHA differs
    drop        the fifth data request gets no answer, nor does anything after it
    dev-off     developer mode off: every request is refused (status 1)

A log frame (id 0x03) goes out before the open's and the SHA's replies, as the log shares the port.
attach(fd) serves the byte stream on fd. Python 3, standard library only; nothing here reaches a
real unit.
"""
import contextlib
import hashlib
import io
import json
import os
import socket
import struct
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))
import testos_mon as M  # noqa: E402

REPLY_ROUTE = 0x12              # S-06.031: the request's route with its nibbles exchanged
LOG_ID = 0x03                   # S-14.021: a log line
CASES = {
    'good': {},
    'mismatch': {'flip': 3},
    'drop': {'drop': 5},
    'dev-off': {'refuse': True},
}


def sample(size=3000, seed=1):
    """A file that passes the page's check: an LDR block header (signature 0xAD, XOR zero), then
    bytes of a fixed pattern."""
    head = bytearray(struct.pack('<IIII', 0xAD000000 | 0x8000 | 4, 0x11A00000, size - 16, 0))
    for k in range(16):
        if k != 2:
            head[2] ^= head[k]
    body = bytes((seed * 31 + 7 * k + (k >> 8)) & 0xFF for k in range(size - 16))
    return bytes(head) + body


class DevLinkUnit:
    def __init__(self, spec, fd=None):
        self.case = spec.get('case', 'good')
        self.cfg = CASES.get(self.case, CASES['good'])
        self.lock = threading.Lock()
        self.sock = None
        self.rx = bytearray()       # every byte sent to the unit
        self.asked = []             # each request's payload, as hex
        self.files = {}
        self.open = None            # (name, bytearray) while a PUT is open
        self.datas = 0
        self.silent = False
        if fd is not None:
            self.attach(fd)

    def answer(self, pl):
        """The reply's payload to one request, or None for no reply."""
        self.asked.append(pl.hex())
        if self.silent:
            return None
        if self.cfg.get('refuse'):
            return b'\x01'
        if not pl:
            return b'\x02'
        if pl[0] == M.DEV_PUT and len(pl) >= 2:
            sub = pl[1]
            if sub == M.PUT_OPEN:
                if pl[2:] != b'os.ldr':
                    return b'\x01'
                self.open = ('os.ldr', bytearray())
                return b'\x00'
            if self.open is None:
                return b'\x02'
            if sub == M.PUT_DATA:
                self.datas += 1
                if self.datas == self.cfg.get('drop'):
                    self.silent = True
                    return None
                chunk = bytearray(pl[2:])
                if self.datas == self.cfg.get('flip') and chunk:
                    chunk[0] ^= 0x01
                self.open[1].extend(chunk)
                return b'\x00'
            if sub == M.PUT_CLOSE:
                self.files[self.open[0]] = bytes(self.open[1])
                self.open = None
                return b'\x00'
            return b'\x02'
        if pl[0] == M.DEV_SHA:
            data = self.files.get(pl[1:].decode('ascii', 'replace'))
            if data is None:
                return b'\x01'
            return b'\x00' + hashlib.sha256(data).digest() + struct.pack('<I', len(data))
        return b'\x02'

    def serve(self, s):
        buf = b''
        try:
            while True:
                data = s.recv(65536)
                if not data:
                    break
                with self.lock:
                    self.rx += data
                buf += data
                while b'\0' in buf:
                    wire, _, buf = buf.partition(b'\0')
                    raw = M.cobs_decode(wire) if wire else b''
                    got = M.unframe(wire)
                    if not got or got[0] != M.ID:
                        continue
                    with self.lock:
                        reply = self.answer(got[1])
                    if reply is None:
                        continue
                    pl = got[1]
                    out = b''
                    if pl[:1] == bytes([M.DEV_SHA]) or pl[:2] == bytes([M.DEV_PUT, M.PUT_OPEN]):
                        out += M.frame(b'\x00\x00\x00\x01dev', 0, LOG_ID, REPLY_ROUTE)
                    out += M.frame(reply, raw[1], M.ID, REPLY_ROUTE)
                    s.sendall(out)
        except OSError:
            pass
        finally:
            s.close()

    def attach(self, fd):
        s = socket.socket(fileno=os.dup(fd))
        self.detach()
        self.sock = s
        threading.Thread(target=self.serve, args=(s,), daemon=True).start()

    def detach(self):
        s, self.sock = self.sock, None
        if s is not None:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def act(self, what):
        return False

    def set(self, attrs):
        with self.lock:
            for k, v in attrs.items():
                setattr(self, k, v)

    def state(self):
        with self.lock:
            return {'rx': bytes(self.rx).hex(), 'asked': list(self.asked),
                    'files': {k: [len(v), hashlib.sha256(v).hexdigest()] for k, v in self.files.items()}}

    def close(self):
        self.detach()


class _Mon(M.Monitor):
    """testos_mon's Monitor on a socket in place of a tty, its waits scaled down."""

    def __init__(self, fd, scale):
        self.fd, self.seq, self.rx, self.scale = fd, 0, bytearray(), scale

    def call(self, payload, timeout=5.0):
        return super().call(payload, timeout * self.scale)


def tool_run(case, data, scale=0.05):
    """testos_mon.py dev-put of data against a unit of this case: what the unit was sent, what the
    tool printed, and its exit (0, 1, or the text of its SystemExit)."""
    unit = DevLinkUnit({'case': case})
    ours, theirs = socket.socketpair()
    unit.attach(theirs.fileno())
    theirs.close()
    fd, path = tempfile.mkstemp(suffix='.ldr')
    try:
        with os.fdopen(fd, 'wb') as fh:
            fh.write(data)
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                status = M.run(_Mon(ours.fileno(), scale), ['dev-put', path])
        except SystemExit as e:
            status = str(e.code)
    finally:
        os.unlink(path)
        ours.close()
        unit.close()
    st = unit.state()
    return {'rx': st['rx'], 'printed': out.getvalue(), 'exit': status, 'files': st['files']}


def cases():
    files = {'small': sample(1000, 1), 'even': sample(240 * 9, 2), 'large': sample(70000, 3)}
    out = []
    for case, fname in (('good', 'small'), ('good', 'even'), ('good', 'large'), ('mismatch', 'large'),
                        ('drop', 'large'), ('dev-off', 'small')):
        data = files[fname]
        r = tool_run(case, data)
        r.update(case=case, file=data.hex(), sha256=hashlib.sha256(data).hexdigest())
        out.append(r)
    return out


def main(argv):
    if argv[1:] == ['cases']:
        json.dump(cases(), sys.stdout)
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
