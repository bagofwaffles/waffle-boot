# SPDX-License-Identifier: GPL-2.0-or-later
"""S-10b MTP: src/s10b_mtp.c (through h_s10b, over an in-memory volume) against tests/oracle/S-10b-mtp: the
level-W scripts of vectors/writes.json (SC-10b-WRITE's and SC-10b-READONLY's logic slice), the operation sweep
and DeviceInfo at both levels. Handles are bound by path through the device's own listing, never by value."""

import hashlib
import json
import struct
import unittest
import zlib

from harness import hx, requirement, run, scenario, spec_module, spec_path, unhex

S, ALL = 0x00010001, 0xFFFFFFFF


def vectors(name):
    with open(spec_path('S-10b-mtp', 'vectors', name + '.json'), encoding='utf-8') as f:
        return json.load(f)


def gen():
    return spec_module('S-10b-mtp', 'gen_vectors')


def volume_commands(entries, level, clock, free):
    """begin, then the volume's objects, parents first; then start and OpenSession."""
    out = ['begin %d 0x%x %d' % (1 if level == 'W' else 0, clock, free)]
    for path in sorted(entries, key=lambda p: (p.count('/'), p)):
        e = entries[path]
        p = hx(path.encode('utf-8'))
        if e['type'] == 'dir':
            out.append('dir %s %d %d' % (p, e['mtime'], e['ctime']))
        else:
            out.append('file %s %s %d %d' % (p, hx(e['data']), e['mtime'], e['ctime']))
    return out + ['start', 'op 0x1002 1 1 none']


def reply(line):
    """(code, params, data or None) of an op's result line."""
    code, params, data = line.split(' ')
    return (int(code, 16), [] if params == '-' else [int(x, 16) for x in params.split(',')],
            None if data == 'none' else unhex(data))


class Device:
    """A script's commands so far, replayed whole for each look at the device; and the map path -> handle,
    bound through the device's listing (GetObjectHandles, then each object's name and parent)."""

    def __init__(self, entries, level, clock, free):
        self.cmds = volume_commands(entries, level, clock, free)
        self.by_path = {}

    def results(self, extra=()):
        out = run('s10b', self.cmds + list(extra))
        return out[len(self.cmds):]

    def last(self):
        """The result of the last command."""
        return run('s10b', self.cmds)[-1]

    def bind(self):
        o = self.results(['op 0x1007 3 0x%x 0 0 none' % S])
        code, _, data = reply(o[0])
        assert code == 0x2001
        n = struct.unpack_from('<I', data)[0]
        hs = struct.unpack_from('<%dI' % n, data, 4)
        q = []
        for h in hs:
            q += ['op 0x9803 2 0x%x 0xdc07 none' % h, 'op 0x9803 2 0x%x 0xdc0b none' % h]
        o = self.results(q)
        name, parent = {}, {}
        for i, h in enumerate(hs):
            raw = reply(o[2 * i])[2]
            name[h] = raw[1:-2].decode('utf-16-le') if raw[0] else ''
            parent[h] = struct.unpack_from('<I', reply(o[2 * i + 1])[2])[0]

        def path(h):
            return (path(parent[h]) if parent[h] else '') + '/' + name[h]
        self.by_path = {path(h): h for h in hs}


def value(dev, v, first):
    if isinstance(v, dict):
        (k, p), = v.items()
        src = first if k == 'was' else dev.by_path
        if p not in src:
            dev.bind()
        return (first if k == 'was' else dev.by_path)[p]
    return v


def command(st, params):
    d = st.get('data')
    head = '0x%x %d %s' % (st['op'], len(params), ' '.join('0x%x' % p for p in params))
    if isinstance(d, dict):
        return 'opp %s %d %d' % (head, d['len'], d['seed'])
    return 'op %s %s' % (head, 'none' if d is None else hx(unhex(d) if d else b''))


class Writes(unittest.TestCase):
    """Every script of vectors/writes.json, each step's response, parameters and data, and the volume after."""

    def replay(self, sc, w):
        dev = Device(w.r.v.__class__.build(gen().TREES[sc['volume']](), free=sc['free']).entries, sc['level'],
                     sc['clock'], sc['free'])
        dev.bind()
        first = dict(dev.by_path)
        made = set()
        for i, st in enumerate(sc['steps']):
            params = [value(dev, v, first) for v in st['params']]
            dev.cmds.append(command(st, params))
            code, rp, data = reply(dev.last())
            where = '%s step %d (op 0x%04x)' % (sc['name'], i, st['op'])
            self.assertEqual(code, st['code'], where)
            self.assertEqual(len(rp), len(st['reply']), where)
            for got, want in zip(rp, st['reply']):
                if isinstance(want, dict) and 'new' in want:
                    self.assertNotIn(got, set(first.values()) | made, where)          # S-10b.012
                    self.assertTrue(got not in (0, ALL) and got >> 16 != 1, where)
                    made.add(got)
                    dev.by_path[want['new']] = got
                else:
                    self.assertEqual(got, value(dev, want, first), where)
            if 'same' in st:                                                              # the handle stays
                old, new = st['same']
                before = dev.by_path[old]
                dev.bind()
                self.assertEqual(dev.by_path.get(new), before, where)
            elif st['op'] in (0x100B, 0x1019, 0x9804) and code == 0x2001:
                dev.bind()
            if 'paths' in st:
                n = struct.unpack_from('<I', data)[0]
                hs = struct.unpack_from('<%dI' % n, data, 4)
                inv = {h: p for p, h in dev.by_path.items()}
                if any(h not in inv for h in hs):
                    dev.bind()
                    inv = {h: p for p, h in dev.by_path.items()}
                self.assertEqual(sorted(inv.get(h) or '' for h in hs), st['paths'], where)
            elif 'out' in st:
                want = bytearray(unhex(st['out']))
                if st['op'] == 0x1008 and st.get('parent'):
                    struct.pack_into('<I', want, 38, dev.by_path[st['parent']])
                if st['op'] == 0x9803 and st.get('parent_path'):
                    struct.pack_into('<I', want, 0, dev.by_path[st['parent_path']])
                self.assertEqual(data, bytes(want), where)
            elif 'sha256' in st:
                self.assertEqual((len(data), hashlib.sha256(data).hexdigest()), (st['length'], st['sha256']),
                                 where)
            else:
                self.assertEqual(data is not None, st['op'] == 0x1001 and code == 0x2001, where)
        return dev

    def check_tree(self, sc, dev, w):
        out = dev.results(['tree'])[-1]
        got = {}
        for item in ([] if out == '-' else out.split(' ')):
            p, kind, size, mt, ct, at, crc = item.split(':')
            got[bytes.fromhex(p).decode('utf-8')] = (int(kind), int(size), int(mt), int(ct), int(at), crc)
        want = {}
        for p, e in w.r.v.entries.items():
            d = e['data'] or b''
            want[p] = (int(e['type'] == 'dir'), len(d), e['mtime'], e['ctime'], e.get('atime', 0),
                       '%08x' % zlib.crc32(d))
        self.assertEqual(got, want, sc['name'])
        self.assertEqual(set(got), set(sc['tree']), sc['name'])

    @scenario('S-10b/SC-10b-WRITE')
    def test_write_scripts(self):
        g = gen()
        fns = {f.__name__[2:]: f for f in (g.w_send, g.w_send_refused, g.w_delete, g.w_delete_handles,
                                            g.w_move, g.w_rename, g.w_modified, g.w_deep, g.w_long)}
        for sc in vectors('writes')['scripts']:
            if sc['level'] != 'W':
                continue
            with self.subTest(script=sc['name']):
                w = fns[sc['name']]()
                self.check_tree(sc, self.replay(sc, w), w)

    @scenario('S-10b/SC-10b-READONLY')
    def test_readonly(self):
        sc = [s for s in vectors('writes')['scripts'] if s['level'] == 'R'][0]
        w = gen().w_readonly()
        self.check_tree(sc, self.replay(sc, w), w)


class Sweep(unittest.TestCase):

    @requirement('S-10b.006', 'S-10b.011', 'S-10b.021', 'S-10b.086', 'S-10b.090', 'S-10b.091')
    def test_op_sweep(self):
        g = gen()
        entries = g.vol('tree_basic').entries
        v = vectors('op_sweep')
        for lv in ('R', 'W'):
            base = volume_commands(entries, lv, 0, 1 << 30)[:-2]
            cmds = list(base)
            want = []
            for code, in_session, listed, response, host_data, device_data in v[lv]:
                cmds.append('start')
                if in_session:
                    cmds.append('op 0x1002 1 1 none')
                cmds.append('op 0x%x 0 %s' % (code, '00' if host_data else 'none'))
                want.append((len(cmds) - 1, code, response, device_data))
            out = run('s10b', cmds)
            for at, code, response, device_data in want:
                got = reply(out[at])
                with self.subTest(level=lv, op=hex(code)):
                    self.assertEqual((got[0], got[2] is not None), (response, bool(device_data)))


class DeviceInfo(unittest.TestCase):

    @requirement('S-10b.020', 'S-10b.021', 'S-10b.031')
    def test_both_levels(self):
        v = vectors('device_info')
        si = {(c['level'], c['free']): c for c in vectors('storage_info')['cases'] if c['total'] is not None}
        for lv in ('R', 'W'):
            out = run('s10b', ['begin %d 0 1024' % (lv == 'W'), 'start', 'op 0x1001 0 none',
                               'op 0x1002 1 1 none', 'op 0x1005 1 0x%x none' % S])
            self.assertEqual(reply(out[2])[2], unhex(v[lv]['data']), lv)
            self.assertEqual(reply(out[4])[2], unhex(si[(lv, 1024)]['data']), lv)
