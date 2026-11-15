#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the S-14 vectors from s14_model.py and the conformance package's frame codec. Synthetic: every
text, name and timestamp is made up here.

    python3 -I -B tests/oracle/S-14-logging/gen_vectors.py          # check the files are current
    python3 -I -B tests/oracle/S-14-logging/gen_vectors.py --write  # rewrite them
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'conformance'))
import s14_model as m  # noqa: E402
import serial as s06  # noqa: E402  (the conformance package)

V = os.path.join(HERE, 'vectors')


def line_case(group, note, ts, text, name=None, line_no=None):
    text = text.encode() if isinstance(text, str) else text
    line = m.build_line(ts, text, name, line_no)
    return {'group': group, 'note': note, 'timestamp': ts, 'name': name, 'line_no': line_no,
            'text': text.hex(), 'full_length': 4 + len(m.tag(name, line_no) if name is not None else b'') + len(text),
            'line': line.hex()}


def text_of_full_length(n, name, line_no, end=b''):
    """A text that makes the full line exactly n bytes long."""
    pre = 4 + len(m.tag(name, line_no))
    body = n - pre - len(end)
    alphabet = b'abcdefghijklmnopqrstuvwxyz0123456789'
    return bytes(alphabet[i % len(alphabet)] for i in range(body)) + end


def lines():
    c = []
    c.append(line_case('format', 'tagged information line', 0x0001E240, 'link up', 'linkmgr', 412))
    c.append(line_case('format', 'untagged line (no source position)', 0x00000400, 'assertion: x > 0'))
    c.append(line_case('format', 'empty line number', 0x12345678, 'no number', 'boot', None))
    c.append(line_case('format', 'empty name', 0x00ABCDEF, 'unnamed', '', 7))
    c.append(line_case('format', 'name of 16 bytes, largest line number', 0xFFFFFFFF, 'max',
                       'abcdefghijklmnop', 2147483647))
    c.append(line_case('format', 'timestamp with 0x00 bytes', 0x00000100, 'zeros in the stamp', 'clk', 1))
    c.append(line_case('format', 'timestamp of 0', 0, 'first', 'start', 3))
    c.append(line_case('format', 'timestamp taken mod 2^32 (2^32 + 5 us)', (1 << 32) + 5, 'wrapped', 'clk', 2))
    c.append(line_case('format', 'empty text', 0x00000010, '', 'quiet', 9))
    c.append(line_case('format', 'text with a percent sign', 0x00000011, '100% done %d', 'fmt', 10))
    for n in (247, 248, 249, 250, 300):
        c.append(line_case('truncate', f'full length {n}', 0x01020304,
                           text_of_full_length(n, 'trunc', 55), 'trunc', 55))
    c.append(line_case('truncate', 'full length 249 only because of the final LF: cut, LF not removed',
                       0x01020304, text_of_full_length(249, 'trunc', 56, b'\n'), 'trunc', 56))
    c.append(line_case('truncate', 'full length 248 ending in LF: LF removed, 247 bytes',
                       0x01020304, text_of_full_length(248, 'trunc', 57, b'\n'), 'trunc', 57))
    c.append(line_case('truncate', 'untagged, full length 264', 0x0A0B0C0D,
                       bytes(range(0x41, 0x41 + 26)) * 10))
    c.append(line_case('newline', 'one final LF removed', 0x00000020, 'done\n', 'nl', 1))
    c.append(line_case('newline', 'two final LFs: one removed', 0x00000021, 'done\n\n', 'nl', 2))
    c.append(line_case('newline', 'LF inside the text stays', 0x00000022, 'a\nb', 'nl', 3))
    c.append(line_case('newline', 'only an LF: removed, empty text', 0x00000023, '\n', 'nl', 4))
    c.append(line_case('newline', 'empty untagged text, timestamp ending in 0A: nothing removed',
                       0x0000010A, ''))
    c.append(line_case('newline', 'tagged, empty text: the tag keeps its space', 0x0000000A, '', 'nl', 5))
    tags = [
        {'body': b'abc[12]: x'.hex(), 'tagged': True, 'name': 'abc', 'line_no': 12, 'text': b'x'.hex()},
        {'body': b'[]: x'.hex(), 'tagged': True, 'name': '', 'line_no': None, 'text': b'x'.hex()},
        {'body': b'a_b-9[1]: '.hex(), 'tagged': True, 'name': 'a_b-9', 'line_no': 1, 'text': ''},
        {'body': b'Abc[12]: x'.hex(), 'tagged': False, 'note': 'upper case'},
        {'body': b'abc[012]: x'.hex(), 'tagged': False, 'note': 'leading zero'},
        {'body': b'abc[0]: x'.hex(), 'tagged': False, 'note': 'line 0'},
        {'body': b'abc[2147483648]: x'.hex(), 'tagged': False, 'note': 'line above 2^31-1'},
        {'body': b'abcdefghijklmnopq[1]: x'.hex(), 'tagged': False, 'note': 'name of 17 bytes'},
        {'body': b'abc[12]:x'.hex(), 'tagged': False, 'note': 'no space'},
        {'body': b'abc[12] : x'.hex(), 'tagged': False, 'note': 'space before the colon'},
        {'body': b'a.c[12]: x'.hex(), 'tagged': False, 'note': 'dot in the name'},
        {'body': b'plain text'.hex(), 'tagged': False, 'note': 'no tag'},
    ]
    return {'requirements': ['S-14.001', 'S-14.002', 'S-14.004', 'S-14.006', 'S-14.007'],
            'lines': c, 'tags': tags}


def log_config():
    reqs = []
    st = m.initial_state()
    seq = [
        ('', 'empty payload: nothing changes'),
        ('a0', 'threshold 1'),
        ('a3', 'threshold 4'),
        ('a4', 'not a level byte: unchanged'),
        ('9f', 'not a level byte: unchanged'),
        ('00', 'not a level byte: unchanged'),
        ('a1', 'threshold 2'),
        ('a2 03', 'threshold 3, both sinks on'),
        ('ff 00', 'threshold unchanged, both sinks off'),
        ('a1 01', 'threshold 2, PSoC on, CDC off'),
        ('a1 02', 'PSoC off, CDC on'),
        ('a1 fe', 'bit 0 clear, bit 1 set: PSoC off, CDC on; other bits ignored'),
        ('a3 fd 01 02', 'bit 0 set, bit 1 clear; bytes after byte 1 ignored'),
        ('a2', 'one byte: sinks keep their state'),
    ]
    for hx, note in seq:
        before = st
        st = m.apply_log_config(st, bytes.fromhex(hx))
        reqs.append({'payload': hx.replace(' ', ''), 'note': note, 'before': before, 'after': st})
    filt = [{'level': lv, 'threshold': th, 'emitted': m.emitted(lv, th)}
            for th in (1, 2, 3, 4) for lv in (0, 1, 2, 3, 4)]
    return {'requirements': ['S-14.011', 'S-14.012', 'S-14.013'], 'initial': m.initial_state(),
            'requests': reqs, 'filter': filt}


def frames():
    out = []
    seq = 0
    for sink in ('psoc', 'cdc'):
        for level in (0, 1, 2, 3, 4):
            line = m.build_line(0x00010000 + level, f'level {level} on {sink}', 'sink', 100 + level)
            route, s, fid, payload = m.log_frame(sink, level, line, seq)
            fr = s06.Frame(route, s, fid, payload)
            out.append({'sink': sink, 'level': level, 'seq': s, 'line': line.hex(),
                        'route': route, 'id': fid, 'wire': fr.encode().hex()})
            seq += 1
    longest = m.build_line(0x00000000, 'x' * 300, 'long', 1)
    fr = s06.Frame(m.ROUTE['psoc'], 255, m.ID_LOG, longest)
    out.append({'sink': 'psoc', 'level': 3, 'seq': 255, 'line': longest.hex(), 'route': m.ROUTE['psoc'],
                'id': m.ID_LOG, 'wire': fr.encode().hex(), 'note': 'longest line, timestamp all zero'})
    return {'requirements': ['S-14.010', 'S-14.021', 'S-14.023'], 'frames': out}


def relay():
    cases = []

    def add(note, src, mid, payload):
        r = m.relay(src, mid, payload)
        cases.append({'note': note, 'src': src, 'id': mid, 'payload': bytes(payload).hex(),
                      'relayed': r is not None,
                      'level': r[0] if r else None, 'text': r[1].hex() if r else None})

    stamp = bytes([0xDE, 0xAD, 0x00, 0x01])
    add('host line, id 0x03', 2, 0x03, stamp + b'hello from host')
    add('host line, id 0x04: error level', 2, 0x04, stamp + b'host error')
    add('Nordic line', 3, 0x03, stamp + b'nordic says hi')
    add('other core line', 5, 0x03, stamp + b'from the other core')
    add('psoc: no line', 4, 0x03, stamp + b'psoc line')
    add('psoc error: no line', 4, 0x04, stamp + b'psoc error')
    add('short: 0 bytes', 2, 0x03, b'')
    add('short: 3 bytes', 2, 0x03, b'\x01\x02\x03')
    add('exactly the stamp: empty text', 2, 0x03, stamp)
    add('0x00 inside the text ends it', 2, 0x03, stamp + b'abc\x00def')
    add('percent', 3, 0x04, stamp + b'%n%s%x%%')
    add('249-byte payload: byte 248 not copied', 2, 0x03, stamp + bytes((0x61 + i % 26) for i in range(245)))
    add('248-byte payload: all text copied', 2, 0x03, stamp + bytes((0x61 + i % 26) for i in range(244)))
    return {'requirements': ['S-14.008', 'S-14.040', 'S-14.041', 'S-14.042'], 'cases': cases}


def export():
    MiB = 1024 * 1024
    groups = []

    def add(note, entries, fails=()):
        groups.append({'note': note, 'entries': entries, 'fails': list(fails),
                       'copied': m.export(entries, fails)})

    def f(name, mtime, size, typ='file'):
        return {'name': name, 'type': typ, 'mtime': mtime, 'size': size}

    add('newest first, all fit', [f('a.log', 100, 10), f('b.log', 300, 20), f('c.log', 200, 30)])
    add('limit reached exactly: still copied',
        [f('new.log', 3, 3 * MiB), f('mid.log', 2, 2 * MiB), f('old.log', 1, 1)])
    add('first candidate over the limit ends the export, a smaller older one is not copied',
        [f('new.log', 3, 4 * MiB), f('big.log', 2, 2 * MiB), f('tiny.log', 1, 100)])
    add('newest alone over the limit: nothing copied', [f('huge.log', 9, 6 * MiB), f('old.log', 1, 5)])
    add('empty files are not copied and count nothing',
        [f('e1.log', 5, 0), f('x.log', 4, 7), f('e2.log', 3, 0)])
    add('directories and dot entries are not candidates; short dot names are',
        [f('.', 50, 0, 'dir'), f('..', 51, 0, 'dir'), f('sub', 40, 0, 'dir'), f('.a', 30, 9),
         f('.b.log', 20, 9), f('z.log', 10, 9)])
    add('a failed copy is skipped and counts nothing',
        [f('n.log', 3, 3 * MiB), f('bad.log', 2, 2 * MiB), f('o.log', 1, 2 * MiB)], fails=['bad.log'])
    add('a failing candidate over the limit still ends the export',
        [f('n.log', 3, 4 * MiB), f('bad.log', 2, 2 * MiB), f('o.log', 1, 10)], fails=['bad.log'])
    add('no candidates', [f('sub', 1, 0, 'dir')])
    add('equal modification times: all exported, ties in ascending name order',
        [f('b.log', 5, 10), f('d.log', 4, 10), f('a.log', 5, 10), f('c.log', 6, 10)])
    add('a tie across the limit: the name order decides which is copied',
        [f('b.log', 7, 3 * MiB), f('a.log', 7, 3 * MiB), f('old.log', 1, 10)])
    add('ties compare bytes: upper case before lower case, a prefix before a longer name',
        [f('a.log', 2, 10), f('B.log', 2, 10), f('a.log.1', 2, 10), f('_x.log', 2, 10)])
    return {'requirements': ['S-14.051', 'S-14.052'], 'limit': m.EXPORT_LIMIT, 'groups': groups}


FILES = {'lines': lines, 'log_config': log_config, 'frames': frames, 'relay': relay, 'export': export}


def render(name):
    return json.dumps(FILES[name](), indent=1, sort_keys=False) + '\n'


def main(argv):
    write = '--write' in argv
    stale = []
    os.makedirs(V, exist_ok=True)
    for name in FILES:
        path = os.path.join(V, name + '.json')
        text = render(name)
        if write:
            with open(path, 'w') as fh:
                fh.write(text)
        else:
            try:
                with open(path) as fh:
                    if fh.read() != text:
                        stale.append(name)
            except FileNotFoundError:
                stale.append(name)
    if stale:
        print('stale:', ', '.join(stale))
        return 1
    print('written' if write else 'current')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
