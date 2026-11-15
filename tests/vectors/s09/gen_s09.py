#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate s09_vectors.h from tests/oracle/S-09-bridge/vectors.
Run from the repository root:  python3 -I -B tests/vectors/s09/gen_s09.py --out DIR

Vectors that carry only a SHA-256 are rebuilt here and checked against it: the multi-sector
write sessions from their data rule, and the slave stream sessions as slices of the slot
images (.bin). The file-read vectors carry the piece lengths and results; their file bytes
come from Python's random module, so the C test fills its file with its own pattern and checks the
pieces' lengths, shapes and order (S-09.035), not the vectors' contents.
"""
import hashlib
import json
import os
import struct
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import genout  # noqa: E402  (before anything the generator reads)

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
VEC = os.path.join(TOP, 'tests', 'oracle', 'S-09-bridge', 'vectors')
sys.path.insert(0, os.path.join(HERE, '..', 's06'))
from cgen import CHeader  # noqa: E402


def load(name):
    with open(os.path.join(VEC, name)) as f:
        return json.load(f)


def sha(b):
    return hashlib.sha256(bytes(b)).hexdigest()


def main():
    h = CHeader('S09_VECTORS_H', 'tests/oracle/S-09-bridge/vectors', 'tests/vectors/s09/gen_s09.py')

    rows = []
    for r in load('requests.json'):
        q = r['req']
        if 'session_hex' in r:
            sess = bytes.fromhex(r['session_hex'])
        else:
            n = q['count'] * 512
            data = bytes((i * 7) & 0xFF for i in range(n))
            hdr = struct.pack('<III', q['cmd'] | q['slot'] << 8 | (n & 0xFFFF) << 16, q.get('arg0', 0),
                              q['count'])
            sess = hdr + data
            assert sha(sess) == r['session_sha256'] and sess.hex().startswith(r['session_head_hex']), r['name']
        assert len(sess) == r['session_len'], r['name']
        rows.append('%s, %s, %d' % (h.cstr(r['name']), h.arr('rq', sess), r['slave_rx_len']))
    h.table('struct s09v_req', 's09v_requests', rows,
            'struct s09v_req { const char *name; const uint8_t *session; size_t len; size_t slave_rx_len; };')

    rows = []
    for r in load('replies.json'):
        b = bytes.fromhex(r['reply_hex'])
        assert len(b) == r['reply_len'], r['name']
        rows.append('%s, %s, %s' % (h.cstr(r['name']), h.cstr(r['kind']), h.arr('rp', b)))
    h.table('struct s09v_reply', 's09v_replies', rows,
            'struct s09v_reply { const char *name; const char *kind; const uint8_t *reply; size_t len; };')

    # A long item publishes its head and a fill (S-09.037: arg1 x 512 payload bytes): the request is
    # rebuilt and checked here, and the C table carries the head with the fill byte and length.
    rows = []
    for r in load('malformed.json'):
        if 'request_hex' in r:
            req, fill, fill_len = bytes.fromhex(r['request_hex']), 0, 0
        else:
            req = bytes.fromhex(r['request_head_hex'])
            fill, fill_len = int(r['payload_fill']['byte'], 16), r['payload_fill']['len']
            whole = req + bytes([fill]) * fill_len
            assert len(whole) == r['request_len'] and sha(whole) == r['request_sha256'], r['name']
        rep = bytes.fromhex(r['reply_hex'])
        assert len(rep) == r['reply_len'], r['name']
        rows.append('%s, %s, %d, %s, 0x%02x, %d' % (h.cstr(r['name']), h.arr('mq', req), r['slave_rx_len'],
                                                   h.arr('mr', rep), fill, fill_len))
    h.table('struct s09v_malformed', 's09v_malformed', rows,
            'struct s09v_malformed { const char *name; const uint8_t *req; size_t len; size_t slave_rx_len; '
            'const uint8_t *reply; size_t reply_len; uint8_t fill; size_t fill_len; };')

    rows, prow = [], []
    for c in load('file_read.json'):
        p0 = len(prow)
        for p in c['pieces']:
            prow.append('%d, %d, %d' % (p['r'], p['reply_len'], p.get('errno', 0)))
        rows.append('%s, %d, %d, %d, %d, %d, %d, %d, %d' % (
            h.cstr(c['name']), c['file_len'], c['pos'], c['n'], c['chunk'],
            -1 if c['fail_piece'] is None else c['fail_piece'], c['result'][0], c['result'][1], p0))
        rows[-1] += ', %d' % (len(prow) - p0)
    h.table('struct s09v_piece', 's09v_pieces', prow, 'struct s09v_piece { int32_t r; uint32_t reply_len, err; };')
    h.table('struct s09v_fread', 's09v_freads', rows,
            'struct s09v_fread { const char *name; uint32_t file_len, pos, n, chunk; int fail_piece; '
            'int32_t result; uint32_t err; size_t p0, pn; };')

    rows = []
    for r in load('records.json'):
        f = r['fields']
        b = bytes.fromhex(r['hex'])
        if r['kind'] == 'stat':
            v = [f[k] for k in ('dev', 'ino', 'mode', 'nlink', 'uid', 'gid', 'rdev', 'size', 'blksize', 'blocks',
                                'atime', 'mtime', 'ctime')]
            rows.append('0, {%s}, 0, %s' % (', '.join(str(x) for x in v), h.arr('rc', b)))
        elif r['kind'] == 'statfs':
            rows.append('1, {%d, %d, %d}, 0, %s' % (f['bsize'], f['blocks'], f['bfree'], h.arr('rc', b)))
        else:
            rows.append('2, {%d, %d, %d}, %s, %s' % (f['ino'], f['off'], f['type'], h.cstr(f['name']), h.arr('rc', b)))
    h.table('struct s09v_record', 's09v_records', rows,
            'struct s09v_record { int kind; uint32_t v[13]; const char *name; const uint8_t *b; size_t len; };')

    rows = ['%s, %d' % (h.cstr(k), v) for k, v in sorted(load('errno.json').items())]
    h.table('struct s09v_errno', 's09v_errno', rows, 'struct s09v_errno { const char *name; uint32_t v; };')

    rows, srow = [], []
    for name in ('final', 'badsig', 'truncated'):
        v = load('slave_stream_%s.json' % name)
        with open(os.path.join(VEC, 'slave_stream_%s.bin' % name), 'rb') as f:
            slot = f.read()
        assert sha(slot) == v['slot_sha256'], name
        s0, off = len(srow), 0
        for i, s in enumerate(v['sessions']):
            if i == 0:
                data, at = b'\x03', -1
            else:
                data, at = slot[off:off + s['len']], off
                off += s['len']
            assert sha(data) == s['sha256'] and data.hex().startswith(s['head_hex']), (name, i)
            srow.append('%d, %d' % (at, s['len']))
        rows.append('%s, %s, %d, %d, %d, %s' % (h.cstr(name), h.arr('slot', slot), {'final': 0, 'badsig': 1,
                                                                                      'slot_end': 2}[v['end']],
                                              s0, len(srow) - s0, h.arr('no', bytes.fromhex(v['no_open_session_hex']))))
    h.table('struct s09v_sess', 's09v_sessions', srow, 'struct s09v_sess { long at; size_t len; };')
    h.table('struct s09v_stream', 's09v_streams', rows,
            'struct s09v_stream { const char *name; const uint8_t *slot; size_t len; int end; size_t s0, sn; '
            'const uint8_t *no_open; size_t no_open_len; };')

    genout.write('s09_vectors.h', h.text())
    genout.finish()


if __name__ == '__main__':
    main()
