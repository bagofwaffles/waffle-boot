# SPDX-License-Identifier: GPL-2.0-or-later
"""S-14 logging: src/s14_line.c, s14_log.c and s14_export.c (through h_s14) against serial.logline and
tests/oracle/S-14-logging (s14_model, vectors) (part 'logic')."""

import json
import random
import unittest

from harness import requirement, run, scenario, spec_module, spec_path
from conformance.serial import logline

M = spec_module('S-14-logging', 's14_model')
NAME_CHARS = 'abcdefghijklmnopqrstuvwxyz0123456789_-'


def vec(name):
    with open(spec_path('S-14-logging', 'vectors', name), encoding='utf-8') as f:
        return json.load(f)


def h(b):
    return bytes(b).hex() or '-'


def name_field(name):
    return 'x' if name is None else h(name.encode())


def sinks(line):
    """{'psoc': (route, id, bytes), 'cdc': ..., 'screen': bytes} from an emit or relay line."""
    out = {}
    for f in line.split()[1:]:
        if f.startswith('ret:'):
            out['ret'] = int(f[4:])
            continue
        parts = f.split(':')
        data = b'' if parts[-1] == '-' else bytes.fromhex(parts[-1])
        out[parts[0]] = data if parts[0] == 'screen' else (int(parts[1], 0), int(parts[2], 0), data)
    return out


def random_text(rng):
    n = rng.choice([0, 1, 5, 40, rng.randrange(260), 230, 244, 246, 250])
    t = bytearray(rng.choice([rng.randrange(1, 256), ord('a'), ord('%')]) for _ in range(n))
    if t and rng.random() < 0.3:
        t[-1] = 0x0A
    if len(t) > 1 and rng.random() < 0.2:
        t[-2] = 0x0A
    if t and rng.random() < 0.1:
        t[rng.randrange(len(t))] = 0
    return bytes(t)


def random_name(rng):
    if rng.random() < 0.2:
        return None
    return ''.join(rng.choice(NAME_CHARS) for _ in range(rng.choice([0, 1, 7, 16])))


def model_line(ts, name, line_no, text):
    """serial.logline.build_line; a 0x00 ends the text (S-14.008)."""
    return logline.build_line(ts, text.split(b'\0', 1)[0], name, line_no or None)


class Lines(unittest.TestCase):
    @scenario('S-14/L-GRAMMAR', 'S-14/L-TIME', 'S-14/L-LONG', 'S-14/L-VERBATIM')
    def test_L_GRAMMAR_vectors(self):
        vs = vec('lines.json')['lines']
        out = run('s14', ['line %d %s %d %s' % (v['timestamp'] % (1 << 32), name_field(v['name']),
                                                v['line_no'] or 0, h(bytes.fromhex(v['text']))) for v in vs])
        for v, line in zip(vs, out):
            self.assertEqual(line, v['line'], v['note'])

    @scenario('S-14/L-GRAMMAR', 'S-14/L-TIME', 'S-14/L-LONG', 'S-14/L-VERBATIM')
    def test_L_GRAMMAR_random(self):
        rng = random.Random(14)
        cases = []
        for _ in range(1500):
            ts = rng.choice([0, 1, 0xFFFFFFFF, rng.getrandbits(32)])
            line_no = rng.choice([0, 1, 9, 10, 412, logline.LINE_NO_MAX, rng.randrange(1, 1 << 31)])
            cases.append((ts, random_name(rng), line_no, random_text(rng)))
        out = run('s14', ['line %d %s %d %s' % (ts, name_field(n), no, h(t)) for ts, n, no, t in cases])
        for c, line in zip(cases, out):
            self.assertEqual(line, model_line(*c).hex(), c)

    @scenario('S-14/L-GRAMMAR')
    def test_L_GRAMMAR_tags(self):
        vs = vec('lines.json')['tags']
        rng = random.Random(41)
        bodies = [bytes.fromhex(v['body']) for v in vs]
        for _ in range(500):
            name = random_name(rng) or ''
            num = rng.choice(['', '0', '1', '012', str(logline.LINE_NO_MAX), str(logline.LINE_NO_MAX + 1),
                              str(rng.randrange(1, 1 << 31)), '99999999999'])
            sep = rng.choice([']: ', ']:', '] ', ']: '])
            if rng.random() < 0.1:
                name += rng.choice('A.[ ')
            bodies.append(('%s[%s%s' % (name, num, sep)).encode() + rng.randbytes(rng.randrange(4)))
        out = run('s14', ['taglen %s' % h(b) for b in bodies])
        for i, (b, line) in enumerate(zip(bodies, out)):
            t = logline.parse_tag(b)
            self.assertEqual(int(line), 0 if t is None else len(b) - len(t[2]), b)
            if i < len(vs):
                self.assertEqual(t is not None, vs[i]['tagged'])

    @requirement('S-14.004')
    def test_S14_004_names(self):
        rng = random.Random(16)
        names = ['', 'a', 'a' * 16, 'a' * 17, 'A', 'x.y', 'linkmgr', '-_09']
        names += [''.join(rng.choice(NAME_CHARS + 'AZ.[]: ') for _ in range(rng.randrange(19)))
                  for _ in range(400)]
        out = run('s14', ['namevalid %s' % h(n.encode()) for n in names])
        for n, line in zip(names, out):
            try:
                logline.tag(n, None)
                want = True
            except ValueError:
                want = False
            self.assertEqual(bool(int(line)), want, n)


class Sinks(unittest.TestCase):
    @scenario('S-14/L-PSOC', 'S-14/L-CDC')
    def test_L_PSOC_frames(self):
        vs = vec('frames.json')['frames']
        cmds = []
        for v in vs:
            p = logline.parse_line(bytes.fromhex(v['line']))
            flags = {'psoc': 1, 'cdc': 2}[v['sink']]
            cmds += ['log 0', 'at %d' % p['timestamp'], 'cfg a3%02x' % flags,
                     'emit %d %s %d %s' % (v['level'], name_field(p['name']), p['line_no'] or 0,
                                           h(p['text']))]
        out = run('s14', cmds)
        for i, v in enumerate(vs):
            got = sinks(out[4 * i + 3])
            self.assertEqual(got, {v['sink']: (v['route'], v['id'], bytes.fromhex(v['line']))}, v)

    @scenario('S-14/L-LEVELS', 'S-14/L-RESET', 'S-14/L-PSOC-OFF')
    def test_L_LEVELS_config(self):
        d = vec('log_config.json')
        cmds = ['log 0', 'cfg -']
        for r in d['requests']:
            b = r['before']
            cmds += ['threshold %d' % b['threshold'], 'cfg 00%02x' % (b['psoc'] | b['cdc'] << 1),
                     'cfg %s' % (r['payload'] or '-')]
        for f in d['filter']:
            cmds += ['threshold %d' % f['threshold'], 'passes %d' % f['level']]
        out = run('s14', cmds)
        st = d['initial']
        self.assertEqual(out[1], '%d %d %d' % (st['threshold'], st['psoc'], st['cdc']))
        k = 2
        for r in d['requests']:
            a = r['after']
            self.assertEqual(out[k + 2], '%d %d %d' % (a['threshold'], a['psoc'], a['cdc']), r['note'])
            k += 3
        for f in d['filter']:
            self.assertEqual(bool(int(out[k + 1])), f['emitted'], f)
            k += 2

    @scenario('S-14/L-LEVELS', 'S-14/L-PSOC', 'S-14/L-CDC', 'S-14/L-SCREEN', 'S-14/L-PSOC-OFF',
              'S-14/L-ORDER', 'S-14/L-SLAVE')
    def test_L_LEVELS_random(self):
        """Random configuration requests, emits and the screen's start next to s14_model, on both cores."""
        rng = random.Random(1414)
        for core in (0, 1):
            cmds, expect = ['log %d' % core], []
            st, screen, now = M.initial_state(), False, 0
            for _ in range(600):
                r = rng.random()
                if r < 0.15:
                    p = bytes(rng.choice([rng.randrange(256), 0xA0, 0xA1, 0xA2, 0xA3])
                              for _ in range(rng.choice([0, 1, 2, 3])))
                    cmds.append('cfg %s' % h(p))
                    st = M.apply_log_config(st, p)
                    expect.append(('cfg', '%d %d %d' % (st['threshold'], st['psoc'], st['cdc'])))
                elif r < 0.18:
                    cmds.append('screen')
                    screen = True
                    expect.append(None)
                else:
                    now = (now + rng.randrange(1 << 20)) % (1 << 32)
                    level = rng.randrange(5)
                    name, no, text = random_name(rng), rng.randrange(1, 5000), random_text(rng)
                    cmds += ['at %d' % now, 'emit %d %s %d %s' % (level, name_field(name), no, h(text))]
                    expect.append(None)
                    want = {}
                    if core == 0 and M.emitted(level, st['threshold']):
                        line = model_line(now, name, no, text)
                        for sink in ('psoc', 'cdc'):
                            if st[sink]:
                                want[sink] = (M.ROUTE[sink], M.frame_id(level), line)
                        if level == 0 and screen:                      # S-14.025
                            want['screen'] = line[4:]
                    expect.append(('emit', want))
            out = run('s14', cmds)[1:]
            for e, line in zip(expect, out):
                if e is None:
                    continue
                if e[0] == 'cfg':
                    self.assertEqual(line, e[1])
                else:
                    self.assertEqual(sinks(line), e[1], (core, line))


class Relay(unittest.TestCase):
    cases = vec('relay.json')['cases']

    def relay(self, cases):
        cmds = ['log 0', 'cfg a302']
        cmds += ['relay %d %d %s' % (c['src'], c['id'], h(bytes.fromhex(c['payload']))) for c in cases]
        return [sinks(x) for x in run('s14', cmds)[2:]]

    def check(self, cases):
        for c, got in zip(cases, self.relay(cases)):
            self.assertEqual(got.pop('ret'), 1, c['note'])
            if not c['relayed']:
                self.assertEqual(got, {}, c['note'])
                continue
            route, fid, line = got['cdc']
            self.assertEqual((route, fid), (M.ROUTE['cdc'], M.frame_id(c['level'])), c['note'])
            text = bytes.fromhex(c['text'])
            p = logline.parse_line(line)
            if len(line) == logline.LINE_MAX and line.endswith(b'..') and p['text'] != text:
                # S-14.040: the tagged line passes 248 bytes; cut by S-14.006 (L-RELAY, L-LONG)
                self.assertEqual(p['text'][:-2], text[:len(p['text']) - 2], c['note'])
            else:
                self.assertEqual(p['text'], text, c['note'])

    @scenario('S-14/L-RELAY')
    def test_L_RELAY(self):
        self.check([c for c in self.cases if len(bytes.fromhex(c['payload'])) < 240])

    @scenario('S-14/L-RELAY')
    def test_L_RELAY_long(self):
        """The 248- and 249-byte cases: with its tag the line is 248 bytes, a prefix of the text and '..'."""
        long = [c for c in self.cases if len(bytes.fromhex(c['payload'])) >= 240 and c['relayed']]
        self.assertTrue(long)
        self.check(long)
        for got in self.relay(long):
            self.assertEqual(len(got['cdc'][2]), logline.LINE_MAX)

    @scenario('S-14/L-LONG')
    def test_L_LONG_relayed(self):
        """L-LONG: the relayed 249-byte case is a 248-byte line ending in '..', the text cut to fit."""
        c = next(c for c in self.cases if len(bytes.fromhex(c['payload'])) == 249)
        _, _, line = self.relay([c])[0]['cdc']
        self.assertEqual(len(line), 248)
        self.assertTrue(line.endswith(b'..'))
        p = logline.parse_tag(line[4:])
        self.assertIsNotNone(p)
        self.assertEqual(p[2][:-2], bytes.fromhex(c['text'])[:len(p[2]) - 2])

    @requirement('S-14.040', 'S-14.041', 'S-14.042')
    def test_S14_040_relay_random(self):
        rng = random.Random(40)
        cases = []
        for _ in range(500):
            p = rng.randbytes(rng.choice([0, 3, 4, 5, 20, 247, 248, 249, 260]))
            cases.append((rng.choice([1, 2, 3, 4, 5, 8]), rng.choice([2, 3, 4, 5]), p))
        cmds = ['log 0', 'cfg a302'] + ['relay %d %d %s' % (s, i, h(p)) for s, i, p in cases]
        for (s, i, p), line in zip(cases, run('s14', cmds)[2:]):
            got = sinks(line)
            self.assertEqual(got.pop('ret'), int(i in (3, 4)))
            want = M.relay(s, i, p)
            if want is None:
                self.assertEqual(got, {})
                continue
            line_bytes = got['cdc'][2]
            body = logline.parse_tag(line_bytes[4:])[2]
            if len(line_bytes) == logline.LINE_MAX and line_bytes.endswith(b'..'):
                self.assertEqual(body[:-2], want[1][:len(body) - 2])
            else:
                self.assertEqual(body, want[1][:-1] if want[1].endswith(b'\n') else want[1])   # S-14.007


class Export(unittest.TestCase):
    def export(self, groups):
        cmds = []
        for g in groups:
            ents = ['%s:%d:%d:%d' % (h(e['name'].encode()), e['type'] == 'file', e['mtime'], e['size'])
                    for e in g['entries']]
            fails = ['/'] + [h(n.encode()) for n in g['fails']] if g['fails'] else []
            cmds.append('export ' + ' '.join(ents + fails))
        return run('s14', cmds)

    def check(self, groups):
        for g, line in zip(groups, self.export(groups)):
            fields = line.split()[1:]
            ret = int(fields.pop()[4:])
            copies = [bytes.fromhex(f[5:]).decode() for f in fields if f.startswith('copy:')]
            discards = [bytes.fromhex(f[8:]).decode() for f in fields if f.startswith('discard:')]
            done = [n for n in copies if n not in g['fails']]
            want = M.export(g['entries'], g['fails'])
            self.assertEqual(done, want, g.get('note'))
            self.assertEqual(ret, len(want), g.get('note'))
            self.assertEqual(discards, [n for n in copies if n in g['fails']], g.get('note'))

    @scenario('S-14/L-EXPORT')
    def test_L_EXPORT_vectors(self):
        self.check(vec('export.json')['groups'])

    @scenario('S-14/L-EXPORT')
    def test_L_EXPORT_random(self):
        rng = random.Random(51)
        groups = []
        for _ in range(300):
            names = rng.sample(['a.log', 'b.log', 'c', 'B.log', 'z', 'boot-1.log', 'x y', '.', '..',
                                'dir', 'n%d' % rng.randrange(9)], rng.randrange(1, 9))
            ents = [{'name': n, 'type': 'dir' if n in ('.', '..', 'dir') or rng.random() < 0.1 else 'file',
                     'mtime': rng.choice([0, 100, 100, 200, rng.getrandbits(32)]),
                     'size': rng.choice([0, 1, 1000, M.EXPORT_LIMIT // 3, M.EXPORT_LIMIT, M.EXPORT_LIMIT + 1,
                                         rng.randrange(M.EXPORT_LIMIT)])} for n in names]
            fails = [e['name'] for e in ents if rng.random() < 0.1]
            groups.append({'entries': ents, 'fails': fails})
        self.check(groups)


if __name__ == '__main__':
    unittest.main()
