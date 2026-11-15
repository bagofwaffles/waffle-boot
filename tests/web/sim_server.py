#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Serves the recovery page with a simulated unit behind it, for trying docs/recover/ in a browser
with no OP-XY:

    tests/web/sim_server.py [PORT]        then open http://localhost:PORT/recover/?sim=model:plain

It serves docs/ as files, and at /sim/unit a WebSocket that units.py's Model or the front's own code
(the shim) answers as a unit answers its recovery port. ?sim=KIND:CASE picks the unit: KIND is model
or shim, CASE one of CASES below. /sim/image gives an image that unit takes.

?sim=installer:CASE is the installer instead (installer_model.py; CASE one of its CASES: pass,
check-fail, trial-fail, no-os, write-fail, breadcrumb). It keeps its state across connections, as a
unit does across a restart, and /sim/act?unit=installer:CASE&do=hold|release|restart|reset stands
for what a person does on the unit. Python 3, standard library only; nothing here reaches a real
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
    print('http://localhost:%d/recover/?sim=model:plain' % port, flush=True)
    srv.serve_forever()


if __name__ == '__main__':
    sys.exit(main(sys.argv))
