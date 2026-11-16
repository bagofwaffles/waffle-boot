# SPDX-License-Identifier: GPL-2.0-or-later
"""The simulated units of tests/tools/test_wbrecover.py, set up from a JSON description, for the
tests of docs/web/core.js: its Model, and the front's own protocol, slot and image code in
the shared library build_shim() makes, driven as AgainstC drives it.

A unit is made from a spec (a dict, as JSON carries it):

    {"kind": "model", "slots": [IMAGE, IMAGE], "sel": [0, 1], "running": 0, "established": true,
     "why": 1, "front": 11, "batt": "hex", "set": {attribute: value, ...}}
    {"kind": "shim", "slots": [IMAGE, IMAGE], "good": [true, true], "sel": [0, 1], "running": 0,
     "why": 1, "batt": "hex", "set": {...}}

    IMAGE: null (an empty slot), {"make": {make_image()'s arguments}}, {"foreign": seed},
           {"of": n, "make": {...}} (image_of()), or {"hex": "..."}

set() changes a unit that exists. The Model takes its own attributes (tried, started and bad as
lists; lose, late and cut with the request type as the key). The shim takes fail [lo, hi],
ignore_selector, fail_reads [at, count], erase_ns, battery (hex), drop_after (INFO requests from
now until the host is taken to have left), lost {type: replies}, flash [[offset, hex], ...] and
start [running, why], which starts recovery again over the flash as it is.

With a file descriptor the unit serves that byte stream from a thread of its own. Without one,
answer(type, payload) gives the reply to one request, as (payload, attempt): the Python tool's
commands run against it through Direct, with no port.
"""
import ctypes
import hashlib
import os
import shutil
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
import test_wbrecover as T  # noqa: E402

w = T.w
SLOT = (0x10000, 0x88000)
RECORD = (0xE000, 0xF000)
HEAD = 4096                     # bytes of each slot the shim's state reports


def image(spec):
    """The image a spec names, or None."""
    if spec is None:
        return None
    if 'hex' in spec:
        return bytes.fromhex(spec['hex'])
    if 'foreign' in spec:
        return T.foreign_image(spec['foreign'])
    kw = dict(spec.get('make', {}))
    if 'version' in kw:
        kw['version'] = tuple(kw['version'])
    if 'of' in spec:
        return T.image_of(spec['of'], **kw)
    return T.make_image(**kw)


class ModelUnit:
    def __init__(self, spec, fd=None):
        slots = [image(s) for s in spec.get('slots', [{'make': {}}, None])]
        self.m = T.Model(fd, slots, sel=tuple(spec.get('sel', (0, 1))),
                         running=spec.get('running', 0), established=spec.get('established', True),
                         why=spec.get('why', 1), front=spec.get('front', 6),
                         batt=bytes.fromhex(spec.get('batt', '')))
        self.set(spec.get('set', {}))

    def set(self, attrs):
        for k, v in attrs.items():
            if k in ('tried', 'started', 'bad', 'unanswered'):
                v = set(v)
            elif k in ('lose', 'late', 'cut'):
                v = {int(a): b for a, b in v.items()}
            elif k in ('run_says', 'begin_says'):
                v = tuple(v) if v else None
            elif k == 'slots':
                v = [image(s) for s in v]
            elif k == 'sel':
                v = list(v)
            elif k == 'batt':
                v = bytes.fromhex(v)
            setattr(self.m, k, v)

    def answer(self, typ, payload):
        m, attempt = self.m, 1
        while True:
            m.step()
            m.frames.append((typ, payload))
            reply = m.handle(typ, payload)
            if not m.lose.get(typ):
                return reply, attempt
            m.lose[typ] -= 1        # the reply is lost: the write goes on while the host waits
            attempt += 1
            for _ in range(20):
                m.step()

    def state(self):
        m = self.m
        return {'sel': list(m.sel), 'slots': [None if s is None else hashlib.sha256(s).hexdigest()
                                              for s in m.slots],
                'tried': sorted(m.tried), 'started': sorted(m.started), 'restarted': m.restarted,
                'requests': [[t, p[:40].hex()] for t, p in m.frames if t != w.DATA],
                'phases': [list(p) for p in m.phases]}

    def close(self):
        self.m.close()


_lib = None
_dir = None


def shim():
    """The front's code as a shared library, built once; None with no C compiler."""
    global _lib, _dir
    if _lib is None and shutil.which(os.environ.get('CC', 'cc')):
        _dir = tempfile.TemporaryDirectory()
        _lib = T.build_shim(_dir.name)
    return _lib


class ShimUnit:
    """One at a time: the library holds one flash and one recovery."""

    def __init__(self, spec, fd=None):
        self.lib = c = shim()
        T.AgainstC.lib = c
        self.t = t = T.AgainstC('test_scan_agrees')     # its place(), pump() and drop()
        t.master, t.lock = fd, threading.Lock()
        t.sniff, t.seen, t.frames = bytearray(), [], []
        t.drop_after, t.stop, t.free, t.lost, t.infos = None, False, True, {}, 0
        slots = [image(s) for s in spec.get('slots', [{'make': {}}, None])]
        good = spec.get('good', [True, True])
        run = spec.get('running', 0)
        batt = bytes.fromhex(spec.get('batt', ''))
        c.shim_flash(0, b'\xff' * 0x100000, 0x100000)
        for k in (0, 1):
            if slots[k] is not None:
                t.place(k, slots[k], good[k])
        c.shim_flash(0xD000, bytes(spec.get('sel', [run, 1])), 2)
        c.shim_ignore_selector(0)
        c.shim_fail(0, 0)
        c.shim_erase_ns(0)
        c.shim_battery(batt, len(batt))
        c.shim_fail_reads(0, 0)
        c.shim_start(run, spec.get('why', 1))
        self.set(spec.get('set', {}))
        self.thread = None
        if fd is not None:
            self.thread = threading.Thread(target=self.pump, daemon=True)
            self.thread.start()

    def pump(self):
        try:
            self.t.pump()
        except OSError:
            pass                    # the host closed the stream

    def set(self, attrs):
        c, t = self.lib, self.t
        with t.lock:
            for k, v in attrs.items():
                if k == 'fail':
                    c.shim_fail(*v)
                elif k == 'ignore_selector':
                    c.shim_ignore_selector(int(v))
                elif k == 'fail_reads':
                    c.shim_fail_reads(*v)
                elif k == 'erase_ns':
                    c.shim_erase_ns(v)
                elif k == 'battery':
                    b = bytes.fromhex(v)
                    c.shim_battery(b, len(b))
                elif k == 'drop_after':
                    t.drop_after = None if v is None else t.infos + v
                elif k == 'lost':
                    t.lost = {int(a): b for a, b in v.items()}
                elif k == 'flash':
                    for off, data in v:
                        b = bytes.fromhex(data)
                        c.shim_flash(off, b, len(b))
                elif k == 'start':
                    c.shim_start(*v)
                else:
                    raise ValueError('no such setting for the shim: %s' % k)

    def _poll(self, typ=None):
        """One pass of AgainstC.pump(), with no port: what the front sent, or b''."""
        c, t = self.lib, self.t
        c.shim_poll()
        if typ == w.INFO:           # the countdown runs one second for each INFO answered
            c.shim_tick(1)
            t.infos += 1
        if t.drop_after is not None and t.infos >= t.drop_after:
            c.shim_drop()
            t.drop_after = None
        buf = ctypes.create_string_buffer(4096)
        n = c.shim_tx(buf, 4096)
        return buf.raw[:n]

    def answer(self, typ, payload):
        c, t, attempt = self.lib, self.t, 1
        seq = 0
        while True:
            seq += 1
            t.frames.append((typ, payload))
            wire = w.frame(typ, seq, payload)
            c.shim_rx(wire, len(wire))
            rx = bytearray(self._poll(typ))
            for _ in range(200):
                got = w.take_frame(rx)
                if got is not None:
                    break
                rx += self._poll()
            else:
                raise w.Failure('the shim did not answer request type %d' % typ)
            for _ in range(40):     # a port that is polled while the host thinks
                self._poll()
            if not t.lost.get(typ):
                return got[2], attempt
            t.lost[typ] -= 1
            attempt += 1
            for _ in range(4000):   # the host waits out its timeout: the write goes on
                self._poll()

    def state(self):
        c, t = self.lib, self.t
        with t.lock:
            def read(off, n):
                return bytes(c.shim_byte(off + i) for i in range(n))
            return {'sel': list(read(0xD000, 2)),
                    'records': [read(RECORD[k], 12).hex() for k in (0, 1)],
                    'heads': [read(SLOT[k], HEAD).hex() for k in (0, 1)],
                    'logs': [read(SLOT[k] + 0x77000, 1).hex() for k in (0, 1)],
                    'restarted': bool(c.shim_restart()), 'batt_asks': c.shim_batt_asks(),
                    'requests': [[typ, p[:40].hex()] for typ, p in t.frames if typ != w.DATA]}

    def close(self):
        self.t.stop = True
        if self.thread is not None:
            self.thread.join(2)


def make_unit(spec, fd=None):
    """The unit a spec describes; None for a shim that cannot be built here."""
    if spec.get('kind', 'model') == 'shim':
        return ShimUnit(spec, fd) if shim() is not None else None
    if spec.get('kind') == 'installer':
        import installer_model
        return installer_model.InstallerUnit(spec, fd)
    if spec.get('kind') == 'devlink':
        import devlink_model
        return devlink_model.DevLinkUnit(spec, fd)
    return ModelUnit(spec, fd)


class Direct:
    """Stands in for wbrecover.Link: the requests go straight to a unit's answer()."""

    def __init__(self, unit):
        self.unit = unit
        self.front, self.max_data, self.sent = 0, w.MAX_DATA, 0
        self.replies = []           # (type, reply) of every request answered

    def request(self, typ, payload=b'', timeout=None, retries=None):
        self.sent = 1
        reply, attempt = self.unit.answer(typ, bytes(payload))
        self.sent = attempt
        self.replies.append((typ, reply))
        return reply, attempt

    def close(self):
        pass
