#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Serves the recovery page with a simulated unit behind it, for trying docs/web/ in a browser
with no OP-XY:

    tests/web/sim_server.py [PORT]        then open http://localhost:PORT/?sim=model:plain

It serves docs/ as files, and at /sim/unit a WebSocket that units.py's Model or the front's own code
(the shim) answers as a unit answers its recovery port. ?sim=KIND:CASE picks the unit: KIND is model
or shim, CASE one of CASES below. /sim/image gives an image that unit takes.

?sim=installer:CASE is the installer instead (installer_model.py; CASE one of its CASES: pass,
check-fail, trial-fail, no-os, write-fail, breadcrumb, keys-ended). It keeps its state across connections, as a
unit does across a restart, and /sim/act?unit=installer:CASE&do=hold|release|restart|reset stands
for what a person does on the unit.

?sim=mtp:CASE is the developer menu's MTP mode instead, for the Install tab's send: the responders
of tests/tools/test_mtpcp.py through mtp_bridge.py, one bulk transfer per WebSocket message. CASE is
one of MTP_CASES: good, readonly, mismatch, refused, stale, gone (the unit goes away while the file
is sent), and busy, which sim.js answers itself as an interface another program holds.

Python 3, standard library only; nothing here reaches a real
unit.
"""
import base64
import hashlib
import http.server
import os
import socket
import struct
import sys
import threading
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import units  # noqa: E402
import installer_model  # noqa: E402
import mtp_bridge  # noqa: E402

DOCS = os.path.join(HERE, '..', '..', 'docs')
OLD = {'make': {'seed': 1, 'version': [0, 9, 12, 0]}}
RUN = {'make': {'seed': 2, 'version': [0, 9, 13, 0]}}
NEW = {'make': {'seed': 3, 'version': [0, 9, 14, 0]}}
# Slot 2 runs and is selected in each; the image to write is NEW.
CASES = {
    'plain': {'slots': [OLD, RUN], 'sel': [1, 1], 'running': 1},
    'same': {'slots': [RUN, RUN], 'sel': [1, 1], 'running': 1},
    'other-bad': {'slots': [None, RUN], 'sel': [1, 1], 'running': 1},
    'unsettled': {'slots': [OLD, RUN], 'sel': [1, 0], 'running': 1},
}
OLD_OS = {'os.ldr': b'an older os.ldr', 'lost+found': None}
MTP_CASES = {
    'good': {},
    'readonly': {'kind': 'readonly'},
    'mismatch': {'faults': {'corrupt_write': True}},
    'refused': {'faults': {'send_refused': True}},
    'stale': {'faults': {'stale_after_list': True}},
    'gone': {'gone_after': 300000},
}
lock = threading.Lock()
installers = {}                         # name -> InstallerUnit, kept across connections


def installer(name):
    with lock:
        if name not in installers:
            case = name.partition(':')[2]
            installers[name] = installer_model.InstallerUnit({'kind': 'installer', 'case': case})
        return installers[name]


def spec_of(name):
    kind, _, case = name.partition(':')
    spec = dict(CASES.get(case, CASES['plain']), kind=kind if kind in ('model', 'shim') else 'model')
    if spec['kind'] == 'model':
        spec['front'] = 14
    else:
        spec['good'] = [s is not None for s in spec['slots']]
    return spec


def ws_send(sock, data):
    n = len(data)
    head = b'\x82' + (bytes([n]) if n < 126 else b'\x7e' + struct.pack('>H', n) if n < 65536
                      else b'\x7f' + struct.pack('>Q', n))
    sock.sendall(head + data)


def ws_recv(rfile):
    """The next message's payload, or None when the browser closes."""
    data = b''
    while True:
        h = rfile.read(2)
        if len(h) < 2:
            return None
        n = h[1] & 0x7F
        if n == 126:
            n = struct.unpack('>H', rfile.read(2))[0]
        elif n == 127:
            n = struct.unpack('>Q', rfile.read(8))[0]
        mask = rfile.read(4) if h[1] & 0x80 else b'\0\0\0\0'
        body = bytes(b ^ mask[i & 3] for i, b in enumerate(rfile.read(n)))
        if h[0] & 0x0F == 8:
            return None
        if h[0] & 0x0F in (0, 1, 2):
            data += body
            if h[0] & 0x80:
                return data


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=DOCS, **kw)

    def log_message(self, *a):
        pass

    def end_headers(self):
        # nothing is cached: a reload shows the tree as it is
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        unit = urllib.parse.parse_qs(url.query).get('unit', ['model:plain'])[0]
        if url.path == '/sim/image':
            data = units.image(NEW)
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        elif url.path == '/sim/act' and unit.startswith('installer:'):
            what = urllib.parse.parse_qs(url.query).get('do', [''])[0]
            ok = installer(unit).act(what)
            body = b'{"ok": true}' if ok else b'{"ok": false}'
            self.send_response(200 if ok else 400)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif url.path == '/sim/unit' and unit.startswith('installer:'):
            self.installer(installer(unit))
        elif url.path == '/sim/unit':
            self.unit(spec_of(unit))
        elif url.path == '/sim/mtp':
            self.mtp(MTP_CASES.get(unit.partition(':')[2], {}))
        else:
            super().do_GET()

    def upgrade(self):
        key = self.headers.get('Sec-WebSocket-Key', '')
        accept = base64.b64encode(hashlib.sha1(
            (key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        self.send_response(101)
        self.send_header('Upgrade', 'websocket')
        self.send_header('Connection', 'Upgrade')
        self.send_header('Sec-WebSocket-Accept', accept)
        self.end_headers()
        self.close_connection = True

    def installer(self, u):
        """The installer on this WebSocket until either side closes; the unit stays."""
        self.upgrade()
        ours, theirs = socket.socketpair()
        u.attach(theirs.fileno())
        theirs.close()                  # the unit holds its own copy
        self.bridge(ours)

    def bridge(self, ours):
        """Carries bytes between this WebSocket and ours until either side closes."""
        def to_browser():
            try:
                while True:
                    data = ours.recv(65536)
                    if not data:
                        break
                    ws_send(self.connection, data)
            except OSError:
                pass
            try:                        # the unit went away: a restart, as a port that closes
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        threading.Thread(target=to_browser, daemon=True).start()
        try:
            while True:
                data = ws_recv(self.rfile)
                if data is None:
                    break
                ours.sendall(data)
        except OSError:
            pass
        finally:
            try:
                ours.close()
            except OSError:
                pass

    def mtp(self, case):
        """One MTP responder on this WebSocket: 'W' and bytes is a bulk OUT, answered 'K'; 'R' and a
        u32 size a bulk IN, answered 'D' and the bytes, or 'T' when nothing is queued."""
        self.upgrade()
        dev = mtp_bridge.make(dict(case, files={}))
        dev.files.update(OLD_OS)
        for name in OLD_OS:
            dev.handle(name)
        sent = 0
        try:
            while True:
                msg = ws_recv(self.rfile)
                if msg is None:
                    break
                if msg[:1] == b'W':
                    sent += len(msg) - 1
                    if sent > case.get('gone_after', 1 << 62):
                        break                   # the unit went away: the socket closes
                    dev.write(msg[1:])
                    ws_send(self.connection, b'K')
                elif msg[:1] == b'R':
                    try:
                        ws_send(self.connection, b'D' + dev.read(struct.unpack('<I', msg[1:5])[0]))
                    except mtp_bridge.mtpcp.Timeout:
                        ws_send(self.connection, b'T')
        except (OSError, AssertionError):
            pass
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def unit(self, spec):
        self.upgrade()
        with lock:                      # the shim holds one flash: one unit at a time
            ours, theirs = socket.socketpair()
            u = units.make_unit(spec, theirs.fileno())

            def to_browser():
                try:
                    while True:
                        data = ours.recv(65536)
                        if not data:
                            break
                        ws_send(self.connection, data)
                except OSError:
                    pass
                try:                    # the unit went away: a restart, as a port that closes
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            threading.Thread(target=to_browser, daemon=True).start()
            try:
                while True:
                    data = ws_recv(self.rfile)
                    if data is None:
                        break
                    ours.sendall(data)
            except OSError:
                pass
            finally:
                u.close()
                for s in (ours, theirs):
                    try:
                        s.close()
                    except OSError:
                        pass


def main(argv):
    port = int(argv[1]) if len(argv) > 1 else 8767
    threading.excepthook = lambda a: None if issubclass(a.exc_type, OSError) else threading.__excepthook__(a)
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler)
    print('http://localhost:%d/?sim=model:plain' % port, flush=True)
    srv.serve_forever()


if __name__ == '__main__':
    sys.exit(main(sys.argv))
