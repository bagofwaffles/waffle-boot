#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate s08_vectors.h and s08_transcripts.h from tests/oracle/S-08-nordic (vectors, transcripts).
Run from the repository root:  python3 -I -B tests/vectors/s08/gen_s08.py --out DIR

The transcripts are a scripted Nordic (rules, greetings) and expectation steps; both are emitted as
tables for the C rule peer and checker.
"""
import json
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import genout  # noqa: E402  (before anything the generator reads)

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
SPEC = os.path.join(TOP, 'tests', 'oracle', 'S-08-nordic')
sys.path.insert(0, os.path.join(HERE, '..', 's06'))
from cgen import CHeader  # noqa: E402

NONE = -2147483647
PROCS = ['link_greeting', 'enter_dfu', 'exit_dfu', 'transfer', 'idle', 'run_start', 'run_end', 'run']


def load(name):
    with open(os.path.join(SPEC, name)) as f:
        return json.load(f)


def h2i(s):
    return int(s, 16) if isinstance(s, str) else s


def state_of(s):
    return -1 if s is None else h2i(s)


def us(ms):
    return int(round(float(ms) * 1000))


def rng(r):
    return (NONE, NONE) if r is None else (us(r[0]), us(r[1]))


def payload_of(spec):
    if 'text' in spec:
        return spec['text'].encode('latin-1')
    return bytes.fromhex(spec.get('payload', ''))


def vectors(h):
    g = load('vectors/greeting.json')
    rows = []
    for r in g['rows'] + g['sequence']:
        a = r['answer']
        rows.append('%d, %s, %d, %d, %d, %d' % (
            state_of(r['state']), h.arr('gp', bytes.fromhex(r['payload'])), a is not None,
            h2i(a['id']) if a else 0, bytes.fromhex(a['payload'])[0] if a else 0,
            -1 if r.get('revision') is None else r['revision']))
    h.table('struct s08v_greet', 's08v_greet', rows,
            'struct s08v_greet { int state; const uint8_t *p; size_t len; int answered; uint8_t id, byte; '
            'int revision; };')
    h.lines.append('#define S08V_SEQUENCE_FIRST %d' % len(g['rows']))
    rows = ['%s, %s, %d' % (r['id'], h.arr('ga', bytes.fromhex(r['payload'])), r['counts']) for r in g['accept']]
    h.table('struct s08v_accept', 's08v_accept', rows,
            'struct s08v_accept { uint8_t id; const uint8_t *p; size_t len; int counts; };')

    q = load('vectors/queries.json')
    rows = []
    for r in q['mode']:
        none = r['reply'] is None
        rows.append('%d, %s, %d' % (none, h.arr('qm', b'' if none else r['reply'].encode('latin-1')),
                                    {None: 0, 'dfu': 1, 'not': 2}[r['expected']]))
    h.table('struct s08v_mode', 's08v_mode', rows,
            'struct s08v_mode { int none; const uint8_t *p; size_t len; int answer; };')
    rows = []
    for r in q['boot_slot']:
        none = r['reply'] is None
        # expected: a slot, None (no slot: -1), or 'other' (any slot but 0 and 1, or none: -2)
        exp = -1 if r['expected'] is None else -2 if r['expected'] == 'other' else r['expected']
        rows.append('%d, %s, %d' % (none, h.arr('qs', b'' if none else r['reply'].encode('latin-1')), exp))
    h.table('struct s08v_slot', 's08v_slot', rows,
            'struct s08v_slot { int none; const uint8_t *p; size_t len; long slot; };')

    d = load('vectors/dfu_replies.json')
    rows = []
    for r in d['rows']:
        et = r['error_text']
        rows.append('%d, %s, %s, %d, %d, %s' % (r['command'], r['reply_id'], h.arr('dr', bytes.fromhex(r['payload'])),
                                                {'fail': 0, 'ok': 1, 'busy': 2}[r['class']], et is not None,
                                                h.arr('de', bytes.fromhex(et or ''))))
    h.table('struct s08v_dfu', 's08v_dfu', rows,
            'struct s08v_dfu { uint8_t cmd, id; const uint8_t *p; size_t len; int cls; int has_text; '
            'const uint8_t *text; size_t text_len; };')

    msg = load('vectors/messages.json')['frames']
    rows = []
    for k in sorted(msg):
        f = msg[k]
        rows.append('%s, %s, %s, %s' % (h.cstr(k), f['route'], f['id'], h.arr('mw', bytes.fromhex(f['wire']))))
    h.table('struct s08v_msg', 's08v_msgs', rows,
            'struct s08v_msg { const char *name; uint8_t route, id; const uint8_t *wire; size_t len; };')

    t = load('vectors/transfers.json')
    rows = ['{%s}, %s, %s, %s' % (', '.join(str(v) for v in p['version']), p['type'], p['product_word'],
                                  h.arr('tp', bytes.fromhex(p['payload']))) for p in t['prepare']]
    h.table('struct s08v_prepare', 's08v_prepare', rows,
            'struct s08v_prepare { uint16_t version[4]; uint8_t type; uint32_t product; const uint8_t *p; '
            'size_t len; };')
    rows = ['%d, {%s}, %s, %s, %s' % (p['image_len'], ', '.join(str(v) for v in p['version']), p['product_word'],
                                      p['crc16'], h.arr('th', bytes.fromhex(p['header']))) for p in t['header']]
    h.table('struct s08v_header', 's08v_header', rows,
            'struct s08v_header { uint32_t image_len; uint16_t version[4]; uint32_t product; uint16_t crc; '
            'const uint8_t *h; size_t len; };')
    rows = ['%d, %d, %s' % (p['sku'], p['read_ok'], p['word']) for p in t['product_word']]
    h.table('struct s08v_pw', 's08v_pw', rows, 'struct s08v_pw { uint32_t sku; int read_ok; uint32_t word; };')
    rows = ['%d, %d, %d, %s, %s' % (p['power_on'], p['boot_sku'],
                                    -1 if p['set_sku_during'] is None else p['set_sku_during'],
                                    p['word'], h.arr('tpo', bytes.fromhex(p['prepare'])))
            for p in t['product_word_power_ons']]
    h.table('struct s08v_pw_on', 's08v_pw_on', rows,
            'struct s08v_pw_on { int power_on; uint32_t boot_sku; int set_sku; uint32_t word; '
            'const uint8_t *prepare; size_t len; };')
    rows = ['%d, %s' % (c['image_len'], h.arr('cl', b''.join(n.to_bytes(2, 'big') for n in c['chunk_payload_lens'])))
            for c in t['chunks']]
    h.table('struct s08v_chunks', 's08v_chunks', rows,
            'struct s08v_chunks { uint32_t image_len; const uint8_t *lens_be16; size_t len; };')

    # transfers-1f.json (S-08.045 with device.product_base 0x00008000): the same tables, s08v1f_*
    t = load('vectors/transfers-1f.json')
    h.lines.append('#define S08V1F_PRODUCT_BASE %su' % t['product_base'])
    rows = ['{%s}, %s, %s, %s' % (', '.join(str(v) for v in p['version']), p['type'], p['product_word'],
                                  h.arr('tp1f', bytes.fromhex(p['payload']))) for p in t['prepare']]
    h.table('struct s08v_prepare', 's08v1f_prepare', rows)
    rows = ['%d, {%s}, %s, %s, %s' % (p['image_len'], ', '.join(str(v) for v in p['version']), p['product_word'],
                                      p['crc16'], h.arr('th1f', bytes.fromhex(p['header']))) for p in t['header']]
    h.table('struct s08v_header', 's08v1f_header', rows)
    rows = ['%d, %d, %s' % (p['sku'], p['read_ok'], p['word']) for p in t['product_word']]
    h.table('struct s08v_pw', 's08v1f_pw', rows)
    rows = ['%d, %d, %d, %s, %s' % (p['power_on'], p['boot_sku'],
                                    -1 if p['set_sku_during'] is None else p['set_sku_during'],
                                    p['word'], h.arr('tpo1f', bytes.fromhex(p['prepare'])))
            for p in t['product_word_power_ons']]
    h.table('struct s08v_pw_on', 's08v1f_pw_on', rows)


def peer_rules(h, peer, rules, sends):
    """A scripted Nordic's rules (and what each sends) appended to the tables."""
    for r in peer.get('rules', []):
        on = r.get('on', {})
        s0 = len(sends)
        for s in r.get('send', []):
            sends.append('%d, %s, %s, %d, %s' % (s.get('kind', 'reply') == 'reply', s.get('id', '0x05'),
                                                 s.get('route', '0x31'), us(s.get('delay_ms', 1.0)),
                                                 h.arr('ps', payload_of(s))))
        rules.append('%d, %d, %d, %d, %s, %d, %d, %d, %d, %d, %d' % (
            h2i(on['id']) if 'id' in on else -1,
            {'request': 1, 'message': 0}.get(on.get('kind'), -1),
            h2i(on['first']) if 'first' in on else -1,
            'payload' in on, h.arr('po', bytes.fromhex(on.get('payload', ''))),
            us(r['from_ms']) if 'from_ms' in r else NONE, us(r['until_ms']) if 'until_ms' in r else -NONE,
            r.get('skip', 0), r['times'] if r.get('times') is not None else -1, s0, len(sends) - s0))


def no_app(h, rules, sends):
    """vectors/no_app.json (SC-08-NO-APP) and writers.tsv's nordic and nordic_no_app columns (S-04's R-NORDIC,
    variant no_app)."""
    v = load('vectors/no_app.json')
    runs, phases = [], []
    for r in v['runs']:
        r0, p0 = len(rules), len(phases)
        peer_rules(h, r['peer'], rules, sends)
        for p in r['sequence']:
            report = {None: 0, 'no_app': 1, 'may_not_start': 2}[p.get('report')]
            phases.append('%d, %s, %d, %d, %d, %d, %d, %d' % (
                p['do'] == 'reboot', p.get('type', '0'), p.get('reason', 0), int(p.get('transfer', True)),
                int(p['enter']), p['exit'], int(p['no_app']), report))
        runs.append('%s, %d, %d, %d, %d' % (h.cstr(r['name']), r0, len(rules) - r0, p0, len(phases) - p0))
    h.lines.append('#define S08N_STATE %d' % h2i(v['state']))
    h.lines.append('#define S08N_IMAGE_LEN %du' % v['image_len'])
    h.lines.append('static const uint16_t s08n_version[4] = { %s };' % ', '.join(str(x) for x in v['version']))
    h.lines.append('#define S08N_PRODUCT %su' % v['product_word'])
    h.table('struct s08n_phase', 's08n_phases', phases,
            '/* report: 0 none, 1 no application, 2 may not start */\n'
            'struct s08n_phase { int reboot; uint8_t type; int reason; int transfer, enter, exits, no_app, '
            'report; };')
    h.table('struct s08n', 's08n', runs,
            'struct s08n { const char *name; int r0, rn, p0, pn; };')
    rows = []
    with open(os.path.join(TOP, 'tests', 'oracle', 'S-04-records', 'writers.tsv')) as f:
        lines = [x for x in f.read().splitlines() if x and not x.startswith('#')]
    head = lines[0].split('\t')
    steps = ['none', 'enter', 'exit']
    for x in lines[1:]:
        w = dict(zip(head, x.split('\t')))
        rows.append('%s, %d, %d' % (w['id'][1:], steps.index(w['nordic']), steps.index(w['nordic_no_app'])))
    h.table('struct s08n_writer', 's08n_writers', rows,
            '/* S-04 writers.tsv: the Nordic step, 0 none 1 enter 2 exit, and after S-08.048 */\n'
            'struct s08n_writer { int id; int nordic, no_app; };')


def transcripts(h):
    names = sorted(f[:-5] for f in os.listdir(os.path.join(SPEC, 'transcripts')) if f.endswith('.json'))
    rules, sends, greets, steps, trs = [], [], [], [], []
    for name in names:
        t = load('transcripts/%s.json' % name)
        peer = t.get('peer', {})
        r0, g0, e0 = len(rules), len(greets), len(steps)
        peer_rules(h, peer, rules, sends)
        for g in peer.get('greet', []):
            greets.append('%d, %s, %s, %s' % (us(g['at_ms']), g.get('route', '0x31'), g.get('id', '0x05'),
                                              h.arr('pg', payload_of(g))))
        for e in t['expect']:
            m = e['dut']
            c = e.get('count', 1)
            lo, hi = (c, c) if isinstance(c, int) else c
            steps.append('%s, %d, %d, %s, %d, %s, %d, %d, %d, %d, %d, %d, %d, %d, %d' % (
                m.get('id', '-1'), {'request': 1, 'message': 0}.get(m.get('kind'), -1),
                'payload' in m, h.arr('ep', bytes.fromhex(m.get('payload', ''))),
                'prefix' in m, h.arr('ex', bytes.fromhex(m.get('prefix', ''))), m.get('len', -1),
                lo, hi, *rng(e.get('t_ms')), *rng(e.get('after_ms')), *rng(e.get('gap_ms'))))
        a = t.get('args', {})
        if 'image_len' in a:
            img_len = a['image_len']
        else:
            img_len = len(bytes.fromhex(a.get('image', '')))
            assert 'image' not in a or bytes.fromhex(a['image']) == bytes((i * 7 + 3) & 0xFF for i in range(img_len))
        v = a.get('version', [1, 0, 0, 0])
        trs.append('%s, %d, %d, %d, %d, %d, %d, %d, {%s}, %s, %s, %d, %d, %d, %d, %d, %d, %d' % (
            h.cstr(name), PROCS.index(t['procedure']), state_of(t.get('state', 0)),
            t['result'] if 'result' in t else -1, *rng(t.get('end_ms')), img_len,
            us(a.get('ms', 100)), ', '.join(str(x) for x in v), a.get('type', '0xb0'),
            a.get('product_word', '0x00084000'), a.get('nordic_image', True),
            r0, len(rules) - r0, g0, len(greets) - g0, e0, len(steps) - e0))
    no_app(h, rules, sends)
    h.table('struct s08t_send', 's08t_sends', sends,
            'struct s08t_send { int reply; uint8_t id, route; int32_t delay_us; const uint8_t *p; size_t len; };')
    h.table('struct s08t_rule', 's08t_rules', rules,
            'struct s08t_rule { int id, kind, first, has_payload; const uint8_t *p; size_t len; '
            'int32_t from_us, until_us; int skip, times; int s0, sn; };')
    h.table('struct s08t_greet', 's08t_greets', greets or ['0, 0, 0, 0, 0'],
            'struct s08t_greet { int32_t at_us; uint8_t route, id; const uint8_t *p; size_t len; };')
    h.table('struct s08t_step', 's08t_steps', steps,
            'struct s08t_step { int id, kind, has_payload; const uint8_t *p; size_t len; int has_prefix; '
            'const uint8_t *x; size_t xlen; int plen; int lo, hi; int32_t t_lo, t_hi, a_lo, a_hi, g_lo, g_hi; };')
    h.lines.append('#define S08T_NONE (%d)' % NONE)
    h.table('struct s08t', 's08t', trs,
            'struct s08t { const char *name; int proc; int state; int result; int32_t end_lo, end_hi; '
            'uint32_t image_len; int32_t idle_us; uint16_t version[4]; uint8_t type; uint32_t product; '
            'int nordic_image; int r0, rn, g0, gn, e0, en; };')


def main():
    h = CHeader('S08_VECTORS_H', 'tests/oracle/S-08-nordic/vectors', 'tests/vectors/s08/gen_s08.py')
    vectors(h)
    genout.write('s08_vectors.h', h.text())
    h = CHeader('S08_TRANSCRIPTS_H', 'tests/oracle/S-08-nordic/transcripts', 'tests/vectors/s08/gen_s08.py')
    transcripts(h)
    genout.write('s08_transcripts.h', h.text())
    genout.finish()


if __name__ == '__main__':
    main()
