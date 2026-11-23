#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A simulated unit on a Unix socket, for tests/web/test_core.mjs: units.py's Model or shim
unit serves the socket's byte stream as a unit serves its recovery port, and this process's
standard input and output carry the control channel, one JSON object a line each way.

    unit_bridge.py

It first prints {"socket": PATH, "shim": whether the front's code could be built}. Requests:

    {"op": "unit", "spec": SPEC}            a new unit on the connection the client has just
                                            made to the socket; the unit before it is closed
    {"op": "unit", "spec": SPEC, "keep": true}   a new unit on the connection that is open
    {"op": "set", "set": {...}}             units.py's set()
    {"op": "state"}                         the unit's flash and what it was asked, as its state()
    {"op": "act", "do": WHAT}               an installer's act(): hold, leave, retrial, release, restart, reset
    {"op": "reconnect"}                     the same unit on the connection the client has just
                                            made (an installer after a restart)
    {"op": "quit"}

Each is answered with {"ok": true, ...} or {"error": "..."}. Python 3, standard library only.
"""
import json
import os
import shutil
import socket
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import units  # noqa: E402


def quiet(args):
    """A unit's thread ends when the client closes the stream under it: not a failure."""
    if not issubclass(args.exc_type, OSError):
        threading.__excepthook__(args)


def main():
    threading.excepthook = quiet
    d = tempfile.mkdtemp(prefix='wbr', dir='/tmp' if os.path.isdir('/tmp') else None)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    unit = conn = None

    def say(obj):
        sys.stdout.write(json.dumps(obj) + '\n')
        sys.stdout.flush()

    def drop(with_conn):
        nonlocal unit, conn
        if unit is not None:
            unit.close()
            unit = None
        if with_conn and conn is not None:
            try:
                conn.close()
            except OSError:
                pass                # the Model closed it when its port went away
            conn = None

    try:
        path = os.path.join(d, 's')
        srv.bind(path)
        srv.listen(8)
        srv.settimeout(10)
        say({'socket': path, 'shim': units.shim() is not None})
        for line in sys.stdin:
            req = json.loads(line)
            op = req.get('op')
            try:
                if op == 'quit':
                    break
                if op == 'unit':
                    drop(not req.get('keep'))
                    if conn is None:
                        conn, _ = srv.accept()
                    unit = units.make_unit(req['spec'], conn.fileno())
                    say({'ok': unit is not None})
                elif op == 'set':
                    unit.set(req['set'])
                    say({'ok': True})
                elif op == 'act':
                    say({'ok': unit.act(req['do'])})
                elif op == 'reconnect':
                    old = conn
                    conn, _ = srv.accept()
                    unit.attach(conn.fileno())
                    if old is not None:
                        old.close()
                    say({'ok': True})
                elif op == 'state':
                    say(dict(unit.state(), ok=True))
                else:
                    say({'error': 'no such request: %r' % op})
            except Exception as e:      # noqa: BLE001 - the test reads it and fails
                say({'error': '%s: %s' % (type(e).__name__, e)})
    finally:
        drop(True)
        srv.close()
        shutil.rmtree(d, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
