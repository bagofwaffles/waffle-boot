#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate s06_vectors.h from tests/oracle/S-06-framing (vectors, links.tsv, routing.tsv).

Run from the repository root:  python3 -I -B tests/vectors/s06/gen_s06.py --out DIR
"""
import json
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import genout  # noqa: E402  (before anything the generator reads)

TOP = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
SPEC = os.path.join(TOP, 'tests', 'oracle', 'S-06-framing')
sys.path.insert(0, os.path.dirname(__file__))
from cgen import CHeader  # noqa: E402

LINK_IDX = {'midi': 0, 'cdc': 1, 'psoc': 2, 'nordic': 3, 'cpu': 4}


def load(name):
    with open(os.path.join(SPEC, name)) as f:
        return json.load(f)


def raw_of(fr):
    p = bytes.fromhex(fr['payload'])
    return bytes([int(fr['route'], 16), fr['seq'], int(fr['id'], 16), len(p)]) + p


def main():
    h = CHeader('S06_VECTORS_H', 'tests/oracle/S-06-framing (vectors, links.tsv, routing.tsv)', 'tests/vectors/s06/gen_s06.py')

    rows = []
    for v in load('vectors/crc.json')['vectors']:
        rows.append('%s, %s, %s' % (h.cstr(v['name']), h.arr('crc', bytes.fromhex(v['data'])), v['crc']))
    h.table('struct s06v_crc', 's06v_crc', rows,
            'struct s06v_crc { const char *name; const uint8_t *data; size_t len; uint8_t crc; };')

    rows = []
    for v in load('vectors/encode.json')['vectors']:
        fr = v['frame']
        rows.append('%s, %s, %d, %s, %s, %s, %s' % (
            h.cstr(v['name']), fr['route'], fr['seq'], fr['id'], h.arr('encp', bytes.fromhex(fr['payload'])),
            h.arr('encr', bytes.fromhex(v['raw'])), h.arr('encw', bytes.fromhex(v['wire']))))
    h.table('struct s06v_encode', 's06v_encode', rows,
            'struct s06v_encode { const char *name; uint8_t route, seq, id; const uint8_t *payload; '
            'size_t len; const uint8_t *raw; size_t raw_len; const uint8_t *wire; size_t wire_len; };')

    # receive.json: events 1 deliver (raw frame without the check byte), 2 forward (wire bytes), 3 drop
    ev_rows, rows = [], []
    for v in load('vectors/receive.json')['vectors']:
        first = len(ev_rows)
        for e in v['required']:
            if e[0] == 'deliver':
                ev_rows.append('1, ' + h.arr('rxe', raw_of(e[1])))
            elif e[0] == 'forward':
                ev_rows.append('2, ' + h.arr('rxe', bytes.fromhex(e[1])))
            elif e[0] == 'drop':
                ev_rows.append('3, 0, 0')
            else:
                raise ValueError(e)
        rows.append('%s, %d, %s, %d, %d' % (h.cstr(v['name']), v['address'], h.arr('rxw', bytes.fromhex(v['wire'])),
                                            first, len(ev_rows) - first))
    h.table('struct s06v_rx_event', 's06v_rx_events', ev_rows,
            'struct s06v_rx_event { int kind; const uint8_t *data; size_t len; };')
    h.table('struct s06v_rx', 's06v_rx', rows,
            'struct s06v_rx { const char *name; uint8_t address; const uint8_t *wire; size_t len; '
            'size_t first_event; size_t n_events; };')

    rows = []
    with open(os.path.join(SPEC, 'links.tsv')) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            core, link, own, peer, route = line.split()
            rows.append('%d, %d, %s, %s, %s' % (core == 'slave', LINK_IDX[link], own, peer, route))
    h.table('struct s06v_link', 's06v_links', rows,
            'struct s06v_link { int slave; int link; uint8_t own, peer, route; };')

    # verdict: -1 drop, -2 local, else the outgoing link index
    rows = []
    with open(os.path.join(SPEC, 'routing.tsv')) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            core, link, dst, verdict = line.split()
            if verdict == 'drop':
                vv = -1
            elif verdict == 'local':
                vv = -2
            else:
                vv = LINK_IDX[verdict.split(':')[1]]
            rows.append('%d, %d, %s, %d' % (core == 'slave', LINK_IDX[link], dst, vv))
    h.table('struct s06v_route', 's06v_routes', rows,
            'struct s06v_route { int slave; int link; uint8_t dst; int verdict; };')

    genout.write('s06_vectors.h', h.text())
    genout.finish()


if __name__ == '__main__':
    main()
