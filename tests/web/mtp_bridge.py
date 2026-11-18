#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The MTP responders of tests/tools/test_mtpcp.py for docs/web/mtp.js: its writable fake (Factory,
with its faults), a read-only one, and waffle-boot's own responder (Engine, built with a C compiler).

    mtp_bridge.py          JSON lines on stdin, one answer line each on stdout

Requests: {"op": "new", "kind": "factory" | "readonly" | "engine", "files": {name: base64 | null},
"mps": 512, "free": n, "level": 0 | 1, "faults": {...}}; {"op": "w", "b": base64} (one bulk OUT);
{"op": "r", "n": size} (one bulk IN: {"b": base64}, or {"t": true} when nothing is queued);
{"op": "state", "get": [paths]} (the files and the operation codes sent); {"op": "python_put", the
"new" fields, "content": base64, "name": "os.ldr", "mtime": seconds} runs tools/mtpcp.py's put on a
new responder and gives every bulk OUT it sent, what it printed and its failure.

sim_server.py uses the responders too. Python 3, standard library only.
"""
import base64
import datetime
import io
import json
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
import test_mtpcp  # noqa: E402
from test_mtpcp import mtpcp  # noqa: E402

B = lambda b: base64.b64encode(b).decode()      # noqa: E731
U = lambda s: base64.b64decode(s)               # noqa: E731


class ReadOnly(test_mtpcp.Factory):
    """The fake with a read-only store (AccessCapability 1)."""

    def command(self, code, tid, p):
        if code == mtpcp.OP_STORAGE_INFO:
            used = sum(len(v) for v in self.files.values() if v)
            self.queue(mtpcp.DATA, code, tid, struct.pack('<HHHQQI', 3, 2, 1, 2403870720,
                                                          max(self.free - used, 0), 0xFFFFFFFF)
                       + mtpcp.mtp_string('OP-XY') + b'\0')
            self.respond(mtpcp.OK, tid)
            return
        super().command(code, tid, p)


def files_of(spec):
    return {n: (None if v is None else U(v)) for n, v in (spec.get('files') or {}).items()}


def make(spec):
    """A responder for a "new" request; None when the engine cannot be built here."""
    kind = spec.get('kind', 'factory')
    mps = spec.get('mps', 512)
    if kind == 'engine':
        try:
            return test_mtpcp.Engine(files_of(spec), mps, spec.get('level', 0))
        except Exception:                  # no compiler, or the build failed
            return None
    cls = ReadOnly if kind == 'readonly' else test_mtpcp.Factory
    return cls(files_of(spec), free=spec.get('free', 1 << 30), mps=mps, **(spec.get('faults') or {}))


class Recording:
    """A responder whose bulk OUT transfers are kept, in order."""

    def __init__(self, dev):
        self.dev = dev
        self.mps = dev.mps
        self.writes = []

    def write(self, data, timeout_ms=0):
        self.writes.append(bytes(data))
        self.dev.write(data, timeout_ms)

    def read(self, size, timeout_ms=0):
        return self.dev.read(size, timeout_ms)


def state(dev, req):
    out = {'ops': list(dev.ops)}
    if isinstance(dev, test_mtpcp.Engine):
        got = {}
        for path in req.get('get', []):
            v = dev.get(path)
            got[path] = v if v is None or v == 'folder' else B(v)
        out['get'] = got
    else:
        out['files'] = {n: (None if v is None else B(v)) for n, v in dev.files.items()}
    return out


def python_put(req):
    dev = make(req)
    if dev is None:
        return {'skip': 'no C compiler'}
    rec = Recording(dev)
    content = U(req['content'])
    out = io.StringIO()
    error = None
    with tempfile.NamedTemporaryFile() as f:
        f.write(content)
        f.flush()
        os.utime(f.name, (req['mtime'], req['mtime']))
        try:
            mtpcp.cmd_put(mtpcp.Mtp(rec), f.name, req.get('name', 'os.ldr'), content, out)
        except mtpcp.Failure as e:
            error = str(e)
    modified = datetime.datetime.fromtimestamp(req['mtime'], datetime.timezone.utc)
    r = {'writes': [B(w) for w in rec.writes], 'out': out.getvalue(), 'error': error,
         'modified': modified.strftime('%Y%m%dT%H%M%S')}
    r.update(state(dev, req))
    return r


def main():
    dev = None
    for line in sys.stdin:
        req = json.loads(line)
        op = req['op']
        try:
            if op == 'new':
                dev = make(req)
                ans = {'ok': True} if dev is not None else {'skip': 'no C compiler'}
            elif op == 'w':
                dev.write(U(req['b']))
                ans = {'ok': True}
            elif op == 'r':
                try:
                    ans = {'b': B(dev.read(req['n']))}
                except mtpcp.Timeout:
                    ans = {'t': True}
            elif op == 'state':
                ans = state(dev, req)
            elif op == 'python_put':
                ans = python_put(req)
            else:
                ans = {'err': 'unknown op %r' % op}
        except AssertionError as e:         # the responder saw something it does not allow
            ans = {'err': 'responder: %s' % e}
        sys.stdout.write(json.dumps(ans) + '\n')
        sys.stdout.flush()


if __name__ == '__main__':
    main()
