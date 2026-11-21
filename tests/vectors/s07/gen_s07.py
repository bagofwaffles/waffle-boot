#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate s07_vectors.h and s07_transcripts.h from tests/oracle/S-07-psoc (vectors, transcripts).
Run from the repository root:  python3 -I -B tests/vectors/s07/gen_s07.py --out DIR

Transcripts are flattened for replay: the master's expected items (log lines left out, as
timed_check does), and the PSoC's events, each scheduled relative to the master item before it;
a reply addressed to a reply slot names the master request it answers, so that the replay can
address it to the slot the implementation chose. The Cypress runs are recorded against the
conformance package's bootloader model (serial.cypress.Bootloader): each packet and its reply.
"""
import json
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import genout  # noqa: E402  (before anything the generator reads)

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
SPEC = os.path.join(TOP, 'tests', 'oracle', 'S-07-psoc')
sys.path.insert(0, os.path.join(HERE, '..', 's06'))
sys.path.insert(0, os.path.join(TOP, 'tests', 'oracle', 'conformance'))
sys.path.insert(0, SPEC)
from cgen import CHeader  # noqa: E402
from serial import cypress  # noqa: E402  (tests/oracle/conformance/serial, not pyserial)
import s07_model as m  # noqa: E402
import gen_s07 as specgen  # noqa: E402

LOG_IDS = {0x03, 0x04}
KIND = {'plain': 0, 'request': 1, 'raw': 2}


def load(name):
    with open(os.path.join(SPEC, name)) as f:
        return json.load(f)


def h2i(s):
    return int(s, 16) if isinstance(s, str) else s


def rules(h):
    k = load('vectors/keys.json')
    bm_rows, ev_rows = [], []
    for case in k['bitmaps']:
        first_ev = len(ev_rows)
        counts = []
        for evs in case['events']:
            counts.append(len(evs))
            for key, down in evs:
                ev_rows.append('%d, %d' % (key, down))
        cnt = h.arr('bmc', bytes(counts))
        lens = h.arr('bml', bytes(len(bytes.fromhex(b)) for b in case['bitmaps']))
        bm_rows.append('%s, %s, %s, %d' % (h.arr('bm', b''.join(bytes.fromhex(b) for b in case['bitmaps'])),
                                           cnt, lens, first_ev))
    h.table('struct s07v_key_ev', 's07v_key_events', ev_rows,
            'struct s07v_key_ev { uint8_t key, down; };')
    h.table('struct s07v_bitmaps', 's07v_bitmaps', bm_rows,
            'struct s07v_bitmaps { const uint8_t *maps; size_t maps_len; const uint8_t *counts; '
            'size_t n; const uint8_t *lens; size_t nlens; size_t first_event; };')
    # after_status: the key-status reply first (S-07.032), then the bitmaps
    as_rows, as_ev = [], []
    for case in k['after_status']:
        first_ev = len(as_ev)
        counts = []
        for evs in case['events']:
            counts.append(len(evs))
            for key, down in evs:
                as_ev.append('%d, %d' % (key, down))
        none = case['status'] == 'none'
        maps = b''.join(bytes.fromhex(b) for b in case['bitmaps'])
        as_rows.append('%d, %s, %s, %s, %d' % (none, h.arr('ass', b'' if none else bytes.fromhex(case['status'])),
                                               h.arr('asm', maps), h.arr('asc', bytes(counts)), first_ev))
    h.table('struct s07v_key_ev', 's07v_after_events', as_ev)
    h.table('struct s07v_after', 's07v_after_status', as_rows,
            'struct s07v_after { int none; const uint8_t *status; size_t status_len; const uint8_t *maps; '
            'size_t maps_len; const uint8_t *counts; size_t n; size_t first_event; };')
    rows = []
    for s in k['status']:
        none = s['payload'] == 'none'
        rows.append('%d, %s, %d, %d, %d' % (none, h.arr('ks', b'' if none else bytes.fromhex(s['payload'])),
                                            s['hold'], s['debug'], s['precomp']))
    h.table('struct s07v_status', 's07v_status', rows,
            'struct s07v_status { int none; const uint8_t *p; size_t len; int hold, debug, precomp; };')

    # chord_status.json (S-07.033): chord 0 none, 4; -1 unreadable
    rows = []
    for c in load('vectors/chord_status.json')['cases']:
        none = c['payload'] == 'none'
        chord = {'none': 0, 'unreadable': -1}.get(c['chord'])
        rows.append('"%s", %d, %s, %d' % (c['name'], none, h.arr('cs', b'' if none else bytes.fromhex(c['payload'])),
                                         int(c['chord']) if chord is None else chord))
    h.table('struct s07v_chord', 's07v_chords', rows,
            'struct s07v_chord { const char *name; int none; const uint8_t *p; size_t len; int chord; };')

    # LED requests: op 0 bar(n), 1 key(k, a, b), 2 rgb(r, g, b, forced)
    leds = load('vectors/leds.json')
    rows = []
    for c in leds['cases']:
        ops = bytearray()
        for r in c['requests']:
            if r[0] == 'bar':
                ops += bytes([0, r[1], 0, 0, 0])
            elif r[0] == 'key':
                ab = bytes.fromhex(r[2])
                ops += bytes([1, r[1], ab[0], ab[1], 0])
            else:
                v = bytes.fromhex(r[1])
                ops += bytes([2]) + v + bytes([1 if len(r) > 2 and r[2] == 'forced' else 0])
        sent = b''.join(bytes([len(bytes.fromhex(s))]) + bytes.fromhex(s) for s in c['sent'])
        rows.append('%s, %s, %d' % (h.arr('ledop', bytes(ops)), h.arr('ledsent', sent), len(c['sent'])))
    h.table('struct s07v_leds', 's07v_leds', rows,
            'struct s07v_leds { const uint8_t *ops; size_t ops_len; const uint8_t *sent; size_t sent_len; '
            'size_t n_sent; };')

    pw = load('vectors/power.json')
    rows = ['%s, %d' % (s['state'], {'on': 0, 'off': 1, 'charge': 2, 'other': 3}[s['kind']]) for s in pw['states']]
    h.table('struct s07v_state', 's07v_states', rows, 'struct s07v_state { unsigned state; int kind; };')
    rows = ['%s, %s, %d' % ('0x100' if t['prev'] == 'none' else t['prev'], t['state'], t['power_off'])
            for t in pw['transitions']]
    h.table('struct s07v_trans', 's07v_trans', rows, 'struct s07v_trans { unsigned prev, state; int off; };')
    rows = ['%s, %d, %d, %d, %d, %d, %d, %d' % (h.arr('batt', bytes.fromhex(b['payload'])), b['present'],
                                                b['charging'], b['charge'], b['temp'], b['ma'], b['mv'],
                                                b['usable']) for b in pw['battery']]
    h.table('struct s07v_batt', 's07v_batt', rows,
            'struct s07v_batt { const uint8_t *p; size_t len; int present, charging, charge, temp, ma, mv, '
            'usable; };')
    rows = []
    for v in pw['vbus']:
        r = bytes.fromhex(v['reply']) + bytes(2)      # S-07.079: missing bytes read as 00
        rows.append('%d, %d, %d, %d, %d, %d' % (v['applied'], v['target'], r[0], r[1], v['applied_after'],
                                                -1 if v['send_34'] is None else bytes.fromhex(v['send_34'])[1]))
    h.table('struct s07v_vbus', 's07v_vbus', rows,
            'struct s07v_vbus { uint32_t applied, target; uint8_t st, n; uint32_t after; int send_t; };')

    rows = []
    with open(os.path.join(SPEC, 'charge_led.tsv')) as f:
        for line in f:
            if line.startswith('#') or line.startswith('state') or not line.strip():
                continue
            st, ch, rgb, forced = line.split()
            rows.append('%s, %s, %s, %s' % ('0x100' if st == 'none' else st, ch,
                                            ', '.join('0x' + rgb[i:i + 2] for i in (2, 4, 6)), forced))
    h.table('struct s07v_charge', 's07v_charge', rows,
            'struct s07v_charge { unsigned state; uint8_t charge; uint8_t r, g, b; int forced; };')

    pm = load('vectors/pmic.json')
    rows = []
    for s in pm['sequence']:
        fr = b''.join(bytes([h2i(f[0]), KIND[f[1]], len(bytes.fromhex(f[2]))]) + bytes.fromhex(f[2])
                      for f in s['frames'])
        rows.append('%d, %s, %d' % (-1 if s['read'] == 'none' else h2i(s['read']), h.arr('pmic', fr),
                                    len(s['frames'])))
    h.table('struct s07v_pmic', 's07v_pmic', rows,
            'struct s07v_pmic { int read; const uint8_t *frames; size_t len; size_t n; };')


def cypress_vectors(h):
    c = load('vectors/cypress.json')
    h.lines.append('#define S07V_ENTER_UNLOCK_HEX "%s"' % c['enter']['unlock'])
    rows = ['%s, %s' % (h.arr('eu', bytes.fromhex(c['enter']['unlock'])), h.arr('ep', bytes.fromhex(c['enter']['packet'])))]
    h.table('struct s07v_enter', 's07v_enter', rows,
            'struct s07v_enter { const uint8_t *unlock; size_t unlock_len; const uint8_t *packet; size_t len; };')
    rows = ['%s, %d' % (h.arr('probe', bytes.fromhex(p['reply'])), {1: 1, 0: 0, 'either': -1}[p['ok']])
            for p in c['probe']]
    h.table('struct s07v_probe', 's07v_probe', rows,
            'struct s07v_probe { const uint8_t *reply; size_t len; int ok; };')
    rows = ['%s, %s, %s, %s' % (p['code'], h.arr('pd', bytes.fromhex(p['data'])), h.arr('ps', bytes.fromhex(p['sum'])),
                                h.arr('pc', bytes.fromhex(p['crc']))) for p in c['packets']]
    h.table('struct s07v_packet', 's07v_packets', rows,
            'struct s07v_packet { uint8_t code; const uint8_t *data; size_t len; const uint8_t *sum; size_t sum_len; '
            'const uint8_t *crc; size_t crc_len; };')
    rows = ['%s, %s, %s' % (h.arr('cs', bytes.fromhex(x['bytes'])), x['sum'], x['crc']) for x in c['checksum']]
    h.table('struct s07v_checksum', 's07v_checksum', rows,
            'struct s07v_checksum { const uint8_t *d; size_t len; uint16_t sum, crc; };')

    # runs: the vectors' runs (their packets checked), the failure cases and the reply cases (the
    # command carried out, its reply replaced), each recorded as (packet, reply or none) against the model.
    unlock = bytes.fromhex(c['runs'][0]['unlock'])
    cases = [('published', {}, c['runs'][0]['cyacd'], 0)]
    for i, r in enumerate(c['runs'][1:], 1):
        kw = {'make': specgen.eeprom_bootloader} if r.get('eeprom') else {}
        cases.append(('published-%d' % i, kw, r['cyacd'], 0))
    for i, r in enumerate(c['replies']):
        if r['ok'] == 'either':
            continue
        code = int(r['cmd'], 16)
        cases.append(('reply-%d' % i, {'swap': (code, bytes.fromhex(r['reply']))}, None, 0 if r['ok'] else code))
    cases += [('wrong-silicon-id', {'silicon_id': 0x11111111}, None, 0x38),
             ('version-02', {'version': (1, 30, 2)}, None, 0x38),
             ('row-outside-range', {'arrays': {0: (0, 1)}}, None, 0x32),
             ('eeprom-outside-range', {'arrays': {0: (0, 0xFF), 0x40: (0, 0)}},
              cypress.build_cyacd(0x2E123069, 0, [(0x40, 5, bytes(8))]), 0x39),
             ('wrong-row-check', {}, specgen.cyacd_text(bad_line=1), 0x3A),
             ('status-05', {'hook': lambda cc, d: cypress.ERR_CMD if cc == 0x39 else None}, None, 0x39),
             ('silent', {'hook': lambda cc, d: cypress.NO_REPLY if cc == 0x38 else None}, None, 0x38),
             ('app-invalid', {'app_valid': False}, None, 0x31),
             # S-07.081, S-07.083: a bootloader that checks by the CRC variant is not entered
             ('crc-bootloader', {'kind': 'crc'}, None, 0x38)]
    xrows, rrows = [], []
    for name, kw, text, at in cases:
        text = text or specgen.cyacd_text()
        kw = dict(kw)
        make, swap = kw.pop('make', None), kw.pop('swap', None)
        bl = make() if make else cypress.Bootloader(row_size=None, **kw)
        if swap:
            inner = bl._packet
            bl._packet = lambda p, inner=inner, swap=swap: (inner(p), swap[1])[1] if p[1] == swap[0] else inner(p)
        sent = []

        def xfer(p, n):
            r = bl.feed(p)
            sent.append((p, r[:n] if len(r) >= n else None))
            return r[:n] if len(r) >= n else None

        def write(p):
            bl.feed(p)
            sent.append((p, None))

        try:
            m.Host(xfer, write, unlock).run(text)
            ok = True
        except m.RunFailed as e:
            ok = False
            assert e.at == at, (name, e)
        assert ok == (at == 0), name
        if name.startswith('published'):
            i = int(name.split('-')[1]) if '-' in name else 0
            assert [p.hex() for p, _ in sent] == c['runs'][i]['packets'], name
        first = len(xrows)
        for p, r in sent:
            xrows.append('%s, %d, %s' % (h.arr('xp', p), r is not None, h.arr('xr', r or b'')))
        tb = text.encode()
        rrows.append('%s, %s, %d, %s, %d, %d, %s' % (h.cstr(name), h.arr('cy', tb), at, '0',
                                                 first, len(xrows) - first, h.arr('ru', unlock)))
    h.table('struct s07v_xchg', 's07v_xchg', xrows,
            'struct s07v_xchg { const uint8_t *p; size_t len; int has_reply; const uint8_t *r; size_t rlen; };')
    h.table('struct s07v_run', 's07v_runs', rrows,
            'struct s07v_run { const char *name; const uint8_t *cyacd; size_t cyacd_len; int fail_at; int crc; '
            'size_t first; size_t n; const uint8_t *unlock; size_t unlock_len; };')

    # images: S-07.083's bad files ('either': a master may refuse it or not, -1)
    rows = ['%s, %d, %s' % (h.arr('im', x['cyacd'].encode()), {1: 1, 0: 0, 'either': -1}[x['bad']], h.cstr(x['why']))
            for x in c['images']]
    h.table('struct s07v_image', 's07v_images', rows,
            'struct s07v_image { const uint8_t *cyacd; size_t len; int bad; const char *why; };')

    cat = load('vectors/catalog.json')
    rows = []
    for e in cat['table1']:
        for pat in e['payloads']:
            kinds = e['kind'].split(' or ')
            rows.append('%s, %d, %d, %s' % (e['id'], 'plain' in kinds, 'request' in kinds, h.cstr(pat)))
    h.table('struct s07v_cat', 's07v_catalog', rows,
            'struct s07v_cat { uint8_t id; int plain, request; const char *pattern; };')
    h.lines.append('static const uint8_t s07v_psoc_accepts[] = { %s };' % ', '.join(cat['psoc_accepts']))


def transcripts(h):
    names = sorted(f[:-5] for f in os.listdir(os.path.join(SPEC, 'transcripts')) if f.endswith('.json'))
    mrows, prows, crows, trows, arows = [], [], [], [], []
    for name in names:
        t = load('transcripts/%s.json' % name)
        ev = t['events']
        setup = t.get('setup', {})
        mfirst, pfirst, cfirst, afirst = len(mrows), len(prows), len(crows), len(arows)
        m_ord, p_ord = {}, {}
        last_m = -1          # event index of the latest master event
        last_req = {}        # reply slot -> master ordinal of the latest request from it
        for i, e in enumerate(ev):
            if e[1] == 'M':
                if e[2] == 'frame' and h2i(e[4]) in LOG_IDS:
                    raise ValueError('log line in a transcript')
                m_ord[i] = len(mrows) - mfirst
                if e[2] == 'raw':
                    mrows.append('2, 0, %s' % h.arr('mr', bytes.fromhex(e[3])))
                else:
                    route = h2i(e[3])
                    kind = 1 if route >> 4 >= 8 and route & 0x0F == 4 else 0
                    if kind:
                        last_req[route >> 4] = m_ord[i]
                    mrows.append('%d, %s, %s' % (kind, e[4], h.arr('mp', bytes.fromhex(e[5]))))
                last_m = i
            else:
                p_ord[i] = len(prows) - pfirst
                anchor = m_ord[last_m] if last_m >= 0 else -1
                at = e[0] - (ev[last_m][0] if last_m >= 0 else 0)
                us = int(round(at * 1000))
                if e[2] == 'raw':
                    prows.append('%d, %d, 1, 0, 0, -1, %s' % (anchor, us, h.arr('pr', bytes.fromhex(e[3]))))
                else:
                    route = h2i(e[3])
                    dst = route & 0x0F
                    req = last_req.get(dst, -1) if dst >= 8 else -1
                    if dst >= 8 and req < 0:
                        raise ValueError('%s: reply to no request' % name)
                    prows.append('%d, %d, 0, %s, %s, %d, %s' % (anchor, us, e[3], e[4], req,
                                                               h.arr('pp', bytes.fromhex(e[5]))))
        for c in t.get('checks', []):
            def ref(i):
                return (0, m_ord[i]) if i in m_ord else (1, p_ord[i])
            a, b = ref(c['from']), ref(c['to'])
            crows.append('%d, %d, %d, %d, %d, %d' % (a[0], a[1], b[0], b[1],
                                                     int(round(c.get('min', -1e9) * 1000)) if 'min' in c else -2000000000,
                                                     int(round(c.get('max', 1e9) * 1000)) if 'max' in c else 2000000000))
        for a in setup.get('actions', []):
            arows.append('%s, %d, %d' % (h.cstr(a['do']), a['t_ms'], a.get('ma', 0)))
        cy = setup.get('cyacd', '')
        trows.append('%s, %d, %d, %d, %d, %d, %s, %d, %d, %d, %d, %d, %d, %d, %d' % (
            h.cstr(name), setup.get('boot_allowed', True), setup.get('os_loads', True),
            setup.get('boot_prompt', False),
            setup.get('end_ms', -1), setup.get('tail_ms', 50), h.arr('tcy', cy.encode()),
            mfirst, len(mrows) - mfirst, pfirst, len(prows) - pfirst, cfirst, len(crows) - cfirst,
            afirst, len(arows) - afirst))
    h.table('struct s07t_m', 's07t_m', mrows,
            'struct s07t_m { int kind; uint8_t id; const uint8_t *d; size_t len; };')
    h.table('struct s07t_p', 's07t_p', prows,
            'struct s07t_p { int anchor; int32_t delta_us; int raw; uint8_t route, id; int req; '
            'const uint8_t *d; size_t len; };')
    h.table('struct s07t_check', 's07t_checks', crows,
            'struct s07t_check { int from_p, from; int to_p, to; int32_t min_us, max_us; };')
    h.table('struct s07t_action', 's07t_actions', arows,
            'struct s07t_action { const char *what; int t_ms; uint32_t ma; };')
    h.table('struct s07t', 's07t', trows,
            'struct s07t { const char *name; int boot_allowed, os_loads, boot_prompt; int end_ms, tail_ms; '
            'const uint8_t *cyacd; size_t cyacd_len; size_t m0, mn, p0, pn, c0, cn, a0, an; };')


def one_f(h):
    """leds-1f.json and charge_led-1f.json (S-07.046-.049): payloads as length-prefixed runs."""
    def runs(hexes):
        return b''.join(bytes([len(bytes.fromhex(s))]) + bytes.fromhex(s) for s in hexes)

    v = load('vectors/leds-1f.json')
    rows = ['%d, %s' % (f['n'], h.arr('bar1f', bytes.fromhex(f['payload']))) for f in v['forms']['bar']]
    h.table('struct s07v1f_bar', 's07v1f_bars', rows,
            'struct s07v1f_bar { uint8_t n; const uint8_t *p; size_t len; };')
    rows = ['{%s}, %s' % (', '.join('0x' + f['rgb'][i:i + 2] for i in (0, 2, 4)),
                          h.arr('rgb1f', bytes.fromhex(f['payload']))) for f in v['forms']['rgb']]
    h.table('struct s07v1f_rgb', 's07v1f_rgbs', rows,
            'struct s07v1f_rgb { uint8_t rgb[3]; const uint8_t *p; size_t len; };')
    # requests: op 0 bar(n), 1 clear, 2 rgb(r, g, b); 4 bytes each
    rows = []
    for c in v['change_only']:
        ops = bytearray()
        for r in c['requests']:
            if r[0] == 'bar':
                ops += bytes([0, r[1], 0, 0])
            elif r[0] == 'clear':
                ops += bytes([1, 0, 0, 0])
            else:
                ops += bytes([2]) + bytes.fromhex(r[1])
        rows.append('%s, %s, %d' % (h.arr('led1fop', bytes(ops)), h.arr('led1fsent', runs(c['sent'])),
                                    len(c['sent'])))
    h.table('struct s07v_leds', 's07v1f_leds', rows)
    h.lines.append('static const uint8_t s07v1f_bar_clear[] = {%s};' % ', '.join(
        '0x%02x' % b for b in bytes.fromhex(v['bar_clear']['payload'])))
    rows = []
    for k in v['key_test']:
        ev = b''.join(bytes([e[0], e[1]]) for e in k['events'])
        rows.append('%s, %s, %d, %s, %d' % (h.arr('kt1fev', ev), k['state'], k['charge'],
                                            h.arr('kt1fsent', runs(k['sent'])), len(k['sent'])))
    h.table('struct s07v1f_kt', 's07v1f_kts', rows,
            'struct s07v1f_kt { const uint8_t *ev; size_t ev_len; unsigned state; uint8_t charge; '
            'const uint8_t *sent; size_t sent_len; size_t n_sent; };')
    c = load('vectors/charge_led-1f.json')
    rows = ['%s, %d, %s' % ('0x100' if s['state'] == 'none' else s['state'], s['charge'],
                            h.arr('chg1f', bytes.fromhex(s['payload']))) for s in c['steady']]
    h.table('struct s07v1f_steady', 's07v1f_steady', rows,
            'struct s07v1f_steady { unsigned state; uint8_t charge; const uint8_t *p; size_t len; };')
    rows = []
    for b in c['breathing']:
        ts = b''.join(r[0].to_bytes(4, 'little') for r in b['updates_ms_levels'])
        lv = b''.join(bytes(r[1]) for r in b['updates_ms_levels'])
        rows.append('%d, %s, %s' % (b['charge'], h.arr('br1ft', ts), h.arr('br1fl', lv)))
    h.table('struct s07v1f_breathe', 's07v1f_breathe', rows,
            'struct s07v1f_breathe { uint8_t charge; const uint8_t *t_le32; size_t t_len; const uint8_t *levels; '
            'size_t l_len; };')
    h.lines.append('#define S07V1F_TOLERANCE %d' % c['tolerance']['level'])


def trace_rules(h):
    """trace_rules-1f.json (S-18.020, SC-18-FEATURES): the ids no_pmic_ids forbids, and the 0x40 first
    bytes no_key_led_ids forbids."""
    r = load('vectors/trace_rules-1f.json')['rules']
    ids = [h2i(i) for i in r['no_pmic_ids']['ids']]
    firsts = [int(b, 16) for b in r['no_key_led_ids']['first_bytes']]
    h.lines.append('static const uint8_t s07v_no_pmic_ids[] = {%s};' % ', '.join('0x%02X' % i for i in ids))
    h.lines.append('#define S07V_LED_ID 0x%02X' % h2i(r['no_key_led_ids']['id']))
    h.lines.append('static const uint8_t s07v_no_key_led_firsts[] = {%s};'
                   % ', '.join('0x%02X' % b for b in firsts))


def main():
    h = CHeader('S07_VECTORS_H', 'tests/oracle/S-07-psoc (vectors, charge_led.tsv)', 'tests/vectors/s07/gen_s07.py')
    rules(h)
    cypress_vectors(h)
    one_f(h)
    trace_rules(h)
    genout.write('s07_vectors.h', h.text())
    h = CHeader('S07_TRANSCRIPTS_H', 'tests/oracle/S-07-psoc/transcripts', 'tests/vectors/s07/gen_s07.py')
    transcripts(h)
    genout.write('s07_transcripts.h', h.text())
    genout.finish()


if __name__ == '__main__':
    main()
