#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turns the S-15 vectors into s15_vectors.h for the C unit tests.

    python3 -I -B tests/vectors/s15/gen_s15_vectors.py --out DIR  # write DIR/s15_vectors.h

Run from the repository root. Standard library only. The header holds the cases of the vectors as C data,
in sections a test selects by defining S15V_<NAME>: the screens' wording and the console's behavior from
tests/vectors/s15/ui/, and the other vectors from tests/oracle/S-15-menus/vectors/. Strings of the JSON
files are taken as Latin-1 bytes, as s15_model.py reads them."""
import csv
import json
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import genout  # noqa: E402  (before anything the generator reads)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
SPEC = os.path.join(ROOT, 'tests', 'oracle', 'S-15-menus')
UI = os.path.join(ROOT, 'tests', 'vectors', 's15', 'ui')
OUT = 's15_vectors.h'

# The screens' wording and the console's behavior, which this repository owns.
UI_FILES = ('console.json', 'screens.json', 'upgrade_lines.json', 'os_write.json', 'installer.json')


def load(name):
    path = os.path.join(UI, name) if name in UI_FILES else os.path.join(SPEC, 'vectors', name)
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def tsv(path):
    with open(path, encoding='utf-8') as f:
        return list(csv.DictReader((l for l in f if not l.startswith('#')), delimiter='\t'))


def cbytes(b):
    out = []
    for c in b:
        ch = chr(c)
        if ch in '"\\?':
            out.append('\\' + ch)
        elif 0x20 <= c <= 0x7E:
            out.append(ch)
        else:
            out.append('\\%03o' % c)
    return '"' + ''.join(out) + '"'


def cstr(s):
    if s is None:
        return 'NULL'
    return cbytes(s.encode('latin-1'))


def cbool(v):
    return 'true' if v else 'false'


def clist(name, items, ctype='const char *const'):
    return 'static %s %s[] = { %s };' % (ctype, name, ', '.join([cstr(x) for x in items] + ['NULL']))


class Out:
    def __init__(self):
        self.lines = []
        self.n = 0

    def w(self, s=''):
        self.lines.append(s)

    def uid(self, prefix):
        self.n += 1
        return '%s_%d' % (prefix, self.n)


def console(o):
    v = load('console.json')
    o.w('#ifdef S15V_CONSOLE')
    o.w('/* Ops applied in order to an empty console; shown rows, top first, as "style:text". */')
    o.w('enum s15v_op_kind { S15V_APPEND, S15V_APPEND_N, S15V_REPLACE, S15V_CLEAR };')
    o.w('struct s15v_op { enum s15v_op_kind kind; const char *text; size_t len; unsigned n;'
        ' const char *style; };')
    o.w('struct s15v_console { const char *name; const struct s15v_op *ops; size_t n_ops;'
        ' const char *const *shown; };')
    names = []
    for c in v['cases']:
        ops = o.uid('s15v_ops')
        shown = o.uid('s15v_shown')
        o.w('static const struct s15v_op %s[] = {' % ops)
        for op in c['ops']:
            kind = op[0]
            if kind in ('append', 'replace'):
                b = op[1].encode('latin-1')
                o.w('\t{ %s, %s, %du, 0, %s },' % ('S15V_APPEND' if kind == 'append' else 'S15V_REPLACE',
                                                  cbytes(b), len(b), cstr(op[2] if len(op) > 2 else 'text')))
            elif kind == 'append_n':
                o.w('\t{ S15V_APPEND_N, NULL, 0, %du, "text" },' % op[1])
            elif kind == 'clear':
                o.w('\t{ S15V_CLEAR, NULL, 0, 0, "text" },')
            else:
                raise ValueError(kind)
        o.w('};')
        o.w(clist(shown, c['shown']))
        names.append((c['name'], ops, len(c['ops']), shown))
    o.w('static const struct s15v_console s15v_console[] = {')
    for name, ops, n, shown in names:
        o.w('\t{ %s, %s, %du, %s },' % (cstr(name), ops, n, shown))
    o.w('};')
    o.w('#endif')
    o.w()


def keys(o):
    o.w('#ifdef S15V_KEYS')
    o.w('/* S-15.011 digits.tsv: key -> digit. */')
    o.w('struct s15v_digit { unsigned key, digit; };')
    o.w('static const struct s15v_digit s15v_digits[] = {')
    for r in tsv(os.path.join(SPEC, 'vectors', 'digits.tsv')):
        o.w('\t{ 0x%02xu, %su },' % (int(r['key'], 16), r['digit']))
    o.w('};')
    o.w('/* the OP-1F\'s (S-15.016): digits_1f.tsv */')
    o.w('static const struct s15v_digit s15v_digits_1f[] = {')
    for r in tsv(os.path.join(SPEC, 'vectors', 'digits_1f.tsv')):
        o.w('\t{ 0x%02xu, %su },' % (int(r['key'], 16), r['digit']))
    o.w('};')
    o.w('#endif')
    o.w()
    v = load('boot_prompt_keys.json')
    o.w('#ifdef S15V_PROMPT_KEYS')
    o.w('/* S-15.180 boot_prompt_keys.json: keys down when the prompt opened, the events after, and what they')
    o.w('   choose: "start", "held" or NULL (it goes on waiting). */')
    o.w('struct s15v_prompt_keys { const char *name; const uint8_t *down_at_open; size_t n_down;'
        ' const uint8_t (*events)[2]; size_t n; const char *expect; };')
    cases = []
    for c in v['cases']:
        dn = o.uid('s15v_bp_down')
        ev = o.uid('s15v_bp_events')
        o.w('static const uint8_t %s[] = { %s };' % (dn, ', '.join(str(int(k, 16)) for k in c['down_at_open']) or '0'))
        o.w('static const uint8_t %s[][2] = { %s };' % (ev, ', '.join(
            '{ %d, %d }' % (int(k, 16), e == 'down') for k, e in c['events']) or '{ 0, 0 }'))
        cases.append((c, dn, ev))
    o.w('static const struct s15v_prompt_keys s15v_prompt_keys[] = {')
    for c, dn, ev in cases:
        o.w('\t{ %s, %s, %du, %s, %du, %s },' % (cstr(c['case']), dn, len(c['down_at_open']), ev, len(c['events']),
                                                 cstr(c['expect'])))
    o.w('};')
    o.w('#endif')
    o.w()
    v = load('select_keys.json')
    o.w('#ifdef S15V_SELECT_KEYS')
    o.w('/* S-15.116 select_keys.json: the selection shown at 0 ms, every row at No, the rows of the steps in')
    o.w('   pickable (bit n for step n) able to be picked and the keys down_at_open down; the events (ms, key, down)')
    o.w('   after it; the choice by end_ms: "install", "cancel" or NULL (it goes on waiting), at_ms when it was')
    o.w('   made (-1: none), and selected, the steps at Yes then (bit n for step n). */')
    o.w('#define S15V_SELECT_HOLD_MS %du' % v['hold_ms'])
    o.w('#define S15V_SELECT_TIMEOUT_MS %du' % v['timeout_ms'])
    o.w('/* the step of digit d\'s row, 99 for none */')
    o.w('static const unsigned s15v_select_row_step[9] = { %s };' % ', '.join(
        '%du' % v['row_steps'].get(str(d), 99) for d in range(9)))
    o.w('struct s15v_select_event { uint32_t ms; uint8_t key; bool down; };')
    o.w('struct s15v_select { const char *name; unsigned pickable; const uint8_t *down_at_open; size_t n_down;'
        ' const struct s15v_select_event *events; size_t n; uint32_t end_ms; const char *expect; long at_ms;'
        ' unsigned selected; };')
    for table, vec in (('s15v_select', v), ('s15v_select_1f', load('select_keys_1f.json'))):
        cases = []
        for c in vec['cases']:
            dn = o.uid('s15v_sel_down')
            ev = o.uid('s15v_sel_events')
            o.w('static const uint8_t %s[] = { %s };' % (dn, ', '.join(str(int(k, 16)) for k in c['down_at_open'])
                                                       or '0'))
            o.w('static const struct s15v_select_event %s[] = { %s };' % (ev, ', '.join(
                '{ %du, 0x%02x, %s }' % (t, int(k, 16), cbool(e == 'down')) for t, k, e in c['events'])
                or '{ 0u, 0, false }'))
            cases.append((c, dn, ev))
        if table.endswith('_1f'):
            o.w('/* the OP-1F\'s (S-15.016): select_keys_1f.json */')
        o.w('static const struct s15v_select %s[] = {' % table)
        for c, dn, ev in cases:
            o.w('\t{ %s, 0x%02xu, %s, %du, %s, %du, %du, %s, %d, 0x%02xu },' % (
                cstr(c['case']), sum(1 << x for x in c['pickable']), dn, len(c['down_at_open']), ev,
                len(c['events']), c['end_ms'], cstr(c['expect']), -1 if c['at_ms'] is None else c['at_ms'],
                sum(1 << x for x in c['selected'])))
        o.w('};')
    # S-15.197 sig_prompt.json: the prompt's keys and time as the selection's, with no row (pickable 0)
    sp = load('sig_prompt.json')
    assert (sp['hold_ms'], sp['timeout_ms']) == (v['hold_ms'], v['timeout_ms'])
    o.w('/* S-15.197 sig_prompt.json: per signature state (S-11.021) whether the prompt shows and whether the')
    o.w('   package is shown as TE\'s; the prompt\'s cases, opened at 0 ms, "install" going on to the selection */')
    o.w('struct s15v_sig_state { const char *state; bool prompt, sig_valid; };')
    o.w('static const struct s15v_sig_state s15v_sig_states[] = {')
    for st in sp['states']:
        o.w('\t{ %s, %s, %s },' % (cstr(st['state']), cbool(st['prompt']), cbool(st['sig_valid'])))
    o.w('};')
    cases = []
    for c in sp['cases']:
        dn = o.uid('s15v_sig_down')
        ev = o.uid('s15v_sig_events')
        o.w('static const uint8_t %s[] = { %s };' % (dn, ', '.join(str(int(k, 16)) for k in c['down_at_open'])
                                                   or '0'))
        o.w('static const struct s15v_select_event %s[] = { %s };' % (ev, ', '.join(
            '{ %du, 0x%02x, %s }' % (t, int(k, 16), cbool(e == 'down')) for t, k, e in c['events'])
            or '{ 0u, 0, false }'))
        cases.append((c, dn, ev))
    o.w('static const struct s15v_select s15v_sig_prompt[] = {')
    for c, dn, ev in cases:
        o.w('\t{ %s, 0x00u, %s, %du, %s, %du, %du, %s, %d, 0x00u },' % (
            cstr(c['case']), dn, len(c['down_at_open']), ev, len(c['events']), c['end_ms'],
            cstr(c['expect']), -1 if c['at_ms'] is None else c['at_ms']))
    o.w('};')
    o.w('#endif')
    o.w()
    v = load('keytest.json')
    o.w('#ifdef S15V_KEYTEST')
    o.w('/* S-15.064, S-15.065 keytest.json */')
    o.w('struct s15v_color { const char *state; uint16_t outline, fill; };')
    o.w('static const struct s15v_color s15v_colors[] = {')
    for state, (outline, fill) in v['colors'].items():
        o.w('\t{ %s, 0x%su, 0x%su },' % (cstr(state), outline, fill))
    o.w('};')
    o.w('struct s15v_keyseq { const char *name; const uint8_t (*events)[2]; size_t n;'
        ' const uint8_t *tested; size_t n_tested; bool end; };')
    k1f = load('keytest_1f.json')
    for table, vec in (('s15v_keyseqs', v), ('s15v_keyseqs_1f', k1f)):
        seqs = []
        for c in vec['sequences']:
            ev = o.uid('s15v_kt_events')
            te = o.uid('s15v_kt_tested')
            o.w('static const uint8_t %s[][2] = { %s };' % (ev, ', '.join('{ %d, %d }' % tuple(e)
                                                                       for e in c['events'])))
            o.w('static const uint8_t %s[] = { %s };' % (te, ', '.join(str(k) for k in c['tested']) or '0'))
            seqs.append((c, ev, te))
        if table.endswith('_1f'):
            o.w('/* the OP-1F\'s (S-15.016): keytest_1f.json, %d keys */' % k1f['nkeys'])
            o.w('#define S15V_KEYTEST_NKEYS_1F %du' % k1f['nkeys'])
        o.w('static const struct s15v_keyseq %s[] = {' % table)
        for c, ev, te in seqs:
            o.w('\t{ %s, %s, %du, %s, %du, %s },' % (cstr(c['name']), ev, len(c['events']), te, len(c['tested']),
                                                    cbool(c['end'])))
        o.w('};')
    o.w('#endif')
    o.w()
    o.w('#ifdef S15V_DEVICE_1F')
    o.w('/* S-15.052 on the OP-1F: factory_reset_1f.json\'s paths removed; S-15.196: mtp_item_1f.json */')
    fr = load('factory_reset_1f.json')
    o.w('static const char *const s15v_reset_removes_1f[] = { %s };' % ', '.join(cstr(p) for p in fr['removes']))
    mi = load('mtp_item_1f.json')
    o.w('struct s15v_mtp_item { const char *name; const uint8_t (*events)[2]; size_t n; bool vendor; };')
    items = []
    for c in mi['cases']:
        ev = o.uid('s15v_mtp_events')
        o.w('static const uint8_t %s[][2] = { %s };' % (ev, ', '.join(
            '{ %d, %d }' % (int(k, 16), e == 'down') for k, e in c['events'])))
        items.append((c, ev))
    o.w('static const struct s15v_mtp_item s15v_mtp_items_1f[] = {')
    for c, ev in items:
        o.w('\t{ %s, %s, %du, %s },' % (cstr(c['case']), ev, len(c['events']), cbool(c['interface'] == 'MTP vendor')))
    o.w('};')
    o.w('#endif')
    o.w()


def screens(o):
    v = load('screens.json')
    o.w('#ifdef S15V_SCREENS')
    o.w('/* screens.json: lines as "style:text"; "{INFO:...}" text is required information. */')
    o.w('struct s15v_screen { const char *name; const char *const *lines; };')
    items = []
    for name, val in v.items():
        if name.startswith('_') or name in ('os_reinstall', 'interrupted', 'titles', 'status', 'start'):
            continue
        lines = val['lines'] if isinstance(val, dict) else val
        ident = 's15v_screen_' + name
        o.w(clist(ident, lines))
        items.append((name, ident))
    o.w('static const struct s15v_screen s15v_screens[] = {')
    for name, ident in items:
        o.w('\t{ %s, %s },' % (cstr(name), ident))
    o.w('};')
    o.w('struct s15v_title { const char *screen, *title; };')
    o.w('static const struct s15v_title s15v_titles[] = {')
    for k, t in v['titles'].items():
        o.w('\t{ %s, %s },' % (cstr(k), cstr(t)))
    o.w('};')
    o.w('/* running: the running slot\'s index, -1 when it is not established. */')
    o.w('struct s15v_status { int running; bool usb; const char *words; };')
    o.w('static const struct s15v_status s15v_status[] = {')
    for flags, words in v['status']:
        running = flags.get('running')
        o.w('\t{ %d, %s, %s },' % (-1 if running is None else running, cbool(flags.get('usb', False)),
                                   cstr(' '.join(words))))
    o.w('};')
    o.w('struct s15v_info_values { const char *unit, *dsp, *backplane, *firmware, *blackfin, *bootloader, *ble,'
        ' *backplane_version; bool provisioned; const char *slave; };')
    for name in ('info', 'info_unprovisioned', 'info_slave_absent'):
        x = v[name]['values']
        o.w('static const struct s15v_info_values s15v_%s_values = { %s, %s, %s };' % (
            name, ', '.join(cstr(x[k]) for k in ('unit', 'dsp', 'backplane', 'firmware', 'blackfin', 'bootloader',
                                                   'ble', 'backplane_version')), cbool(x['provisioned']),
            cstr(x.get('slave') or '')))
    o.w('/* S-15.163: a failed start; main is the notice\'s second line, info Device Info\'s OS start value. */')
    o.w('struct s15v_start { const char *name; struct s15_start start; enum s15_slave slave;'
        ' const char *first, *main, *info, *dsp2; };')
    o.w('static const struct s15v_start s15v_starts[] = {')
    for c in v['start']['cases']:
        s = c['start']
        o.w('\t{ %s, { S15_START_%s, %du, %du, %du, S15_CHECK_%s, %s, 0x%02Xu, 0x%02Xu, %s, S15_OSW_WHY_%s, %s },'
            ' S15_SLAVE_%s,' % (
                cstr(c['name']), s['why'], s.get('part', 0), s.get('value', 0), s.get('limit', 0),
                s.get('check', 'LENGTH'), cbool(s.get('lost', False)), s.get('cmd', 0), s.get('at', 0),
                cbool(s.get('file', False)), s.get('file_why', 'NONE'), cbool(s.get('listed', False)),
                c['slave']))
        o.w('\t  %s, %s, %s, %s },' % (cstr(c.get('first', v['start']['first'])), cstr(c['main']),
                                       cstr(c['info']), cstr(c['dsp2'])))
    o.w('};')
    r = v['os_reinstall']
    o.w('static const char s15v_os_reinstall_text[] = %s;' % cstr(r['text']))
    o.w('struct s15v_os_load { const char *load; int shown; /* 1, 0, or -1: no main screen */ };')
    o.w('static const struct s15v_os_load s15v_os_loads[] = {')
    for ld, shown in r['shown'].items():
        o.w('\t{ %s, %d },' % (cstr(ld), -1 if shown is None else int(shown)))
    o.w('};')
    o.w('#endif')
    o.w()
    m = load('manifest_names.json')
    o.w('#ifdef S15V_MANIFEST')
    o.w('/* S-15.041 manifest_names.json; text NULL: no file; pad spaces before the text. */')
    o.w('struct s15v_manifest { const char *name; const char *text; size_t len; size_t pad; const char *dir;'
        ' const char *expect, *label; };')
    o.w('static const struct s15v_manifest s15v_manifests[] = {')
    for c in m['cases']:
        t = c['text']
        b = None if t is None else t.encode('latin-1')
        o.w('\t{ %s, %s, %du, %du, %s, %s, %s },' % (cstr(c['name']), 'NULL' if b is None else cbytes(b),
                                                    0 if b is None else len(b), c.get('text_pad', 0),
                                                    cstr(c['dir']), cstr(c['expect']), cstr(c['label'])))
    o.w('};')
    o.w('#endif')
    o.w()
    h = load('handover_frame.json')
    o.w('#ifdef S15V_HANDOVER')
    o.w('/* S-15.132 handover_frame.json */')
    o.w('#define S15V_HANDOVER_BYTES %du' % h['bytes'])
    o.w('#define S15V_HANDOVER_WHITE %du' % h['white_pixels'])
    o.w('#define S15V_HANDOVER_FAINT %du' % h['faint_pixels'])
    o.w('static const char s15v_handover_sha256[] = %s;' % cstr(h['sha256']))
    o.w('#endif')
    o.w()


def upgrade(o):
    v = load('upgrade_lines.json')
    o.w('#ifdef S15V_UPGRADE')
    o.w('/* upgrade_lines.json: the upgrade screen\'s lines */')
    o.w('struct s15v_progress { uint32_t done, total; unsigned p; };')
    o.w('static const struct s15v_progress s15v_progress[] = {')
    for d, t, p in v['progress']:
        o.w('\t{ %du, %du, %du },' % (d, t, p))
    o.w('};')
    o.w('static const char s15v_backplane_chars[] = %s;' % cstr(''.join(
        v['backplane_chars'][str(i)] for i in range(len(v['backplane_chars'])))))
    o.w('struct s15v_label { const char *step, *label; unsigned number; };')
    o.w('static const struct s15v_label s15v_labels[] = {')
    for step, label, num in v['labels']:
        o.w('\t{ %s, %s, %du },' % (cstr(step), cstr(label), num))
    o.w('};')
    o.w('struct s15v_header { const char *kind, *version, *name, *line; };')
    o.w('static const struct s15v_header s15v_headers[] = {')
    for kind, ver, name, line in v['headers']:
        o.w('\t{ %s, %s, %s, %s },' % (cstr(kind), cstr(ver), cstr(name), cstr(line)))
    o.w('};')
    o.w('struct s15v_run_header { const char *kind, *version, *name; bool stock, te_signed;'
        ' const char *const *lines; };')
    rh = []
    for head, lines in v['run_headers']:
        kind, ver, name, stock = head[:4]
        signed = head[4] if len(head) > 4 else False   # S-15.197: a valid package
        ident = o.uid('s15v_rh')
        o.w(clist(ident, lines))
        rh.append((kind, ver, name, stock, signed, ident))
    o.w('static const struct s15v_run_header s15v_run_headers[] = {')
    for kind, ver, name, stock, signed, ident in rh:
        o.w('\t{ %s, %s, %s, %s, %s, %s },' % (cstr(kind), cstr(ver), cstr(name), cbool(stock), cbool(signed),
                                              ident))
    o.w('};')
    o.w('struct s15v_result { const char *step, *result, *version; bool content_only;'
        ' const char *op, *line; };')
    o.w('static const struct s15v_result s15v_results[] = {')
    for (step, res, ver, co), (op, line) in v['results']:
        o.w('\t{ %s, %s, %s, %s, %s, %s },' % (cstr(step), cstr(res), cstr(ver), cbool(co),
                                              cstr(op), cstr(line)))
    o.w('};')
    o.w('struct s15v_progress_row { const char *step; unsigned p; const char *line; };')
    o.w('static const struct s15v_progress_row s15v_progress_rows[] = {')
    for step, p, line in v['progress_rows']:
        o.w('\t{ %s, %du, %s },' % (cstr(step), p, cstr(line)))
    o.w('};')
    o.w('struct s15v_failed_row { const char *step, *cause, *line; };')
    o.w('static const struct s15v_failed_row s15v_failed_rows[] = {')
    for step, cause, line in v['failed_rows']:
        o.w('\t{ %s, %s, %s },' % (cstr(step), cstr(cause), cstr(line)))
    o.w('};')
    o.w('/* nordic_notes: S-11.069, S-11.070 */')
    o.w('struct s15v_note { const char *note; const char *const *lines; };')
    notes = []
    for note, lines in v['nordic_notes']:
        ident = o.uid('s15v_nn')
        o.w(clist(ident, lines))
        notes.append((note, ident))
    o.w('static const struct s15v_note s15v_notes[] = {')
    for note, ident in notes:
        o.w('\t{ %s, %s },' % (cstr(note), ident))
    o.w('};')
    pr = v['prepare']
    o.w('static const char s15v_prepare_header[] = %s;' % cstr(pr['header'][0]))
    o.w('static const char s15v_not_found[] = %s;' % cstr(pr['not_found']))
    o.w('static const char s15v_no_transfer[] = %s;' % cstr(pr['no_transfer']))
    o.w('static const char s15v_reason[] = %s;' % cstr(pr['reason']))
    o.w('struct s15v_outcome { const char *outcome; bool skipped; unsigned reason; bool content_only;'
        ' const char *const *lines; };')
    oc = []
    for (outcome, results, reason, co), lines in v['outcomes']:
        ident = o.uid('s15v_oc')
        o.w(clist(ident, lines))
        oc.append((outcome, 'Skipped' in results, reason, co, ident))
    o.w('static const struct s15v_outcome s15v_outcomes[] = {')
    for outcome, sk, reason, co, ident in oc:
        o.w('\t{ %s, %s, %du, %s, %s },' % (cstr(outcome), cbool(sk), reason, cbool(co), ident))
    o.w('};')
    o.w('/* not_updated: the steps a completed run skipped, the lines before its outcome */')
    o.w('struct s15v_not_updated { const char *const *steps; const char *const *lines; };')
    nu = []
    for steps, lines in v['not_updated']:
        si, li = o.uid('s15v_nus'), o.uid('s15v_nul')
        o.w(clist(si, steps))
        o.w(clist(li, lines))
        nu.append((si, li))
    o.w('static const struct s15v_not_updated s15v_not_updated[] = {')
    for si, li in nu:
        o.w('\t{ %s, %s },' % (si, li))
    o.w('};')
    o.w('#endif')
    o.w()


def os_write(o):
    v = load('os_write.json')
    o.w('#ifdef S15V_OSW')
    o.w('/* os_write.json (S-15.094): how a write went, and every line its screen appends; stop, why, emmc and')
    o.w('   result as the vector names them (stop and why "" for none). */')
    o.w('struct s15v_osw { const char *name, *result; uint32_t length, boot_size; unsigned target; const char *stop,'
        ' *why; unsigned part; uint32_t value, limit; const char *emmc; bool select0_failed;'
        ' const char *const *lines; };')
    items = []
    for c in v['cases']:
        ident = o.uid('s15v_osw_lines')
        o.w(clist(ident, c['lines']))
        items.append((c, ident))
    o.w('static const struct s15v_osw s15v_osws[] = {')
    for c, ident in items:
        o.w('\t{ %s, %s, %du, %du, %du, %s, %s, %du, %du, %du, %s, %s, %s },' % (
            cstr(c['name']), cstr(c['result']), c['length'], c['boot_size'], c['target'], cstr(c['stop'] or ''),
            cstr(c['why'] or ''), c['part'], c['value'], c['limit'], cstr(c['emmc']), cbool(c['select0_failed']),
            ident))
    o.w('};')
    o.w('#endif')
    o.w()


def installer(o):
    v = load('installer.json')
    o.w('#ifdef S15V_INSTALLER')
    o.w('/* installer.json: the installer\'s screens, one case per state shown. */')
    o.w('struct s15v_inst_survey { unsigned device, how; unsigned long fs_n, fs_c; int sel_read;'
        ' unsigned sel_index, sel_verified, slot_bits[2]; int ext_read; unsigned part_config, boot_kib; };')
    o.w('struct s15v_inst { const char *name; unsigned phase; const char *version; unsigned target, lock, last_rc,'
        ' os, trial, hold_digit, hold_fill, hold_left_s, keys_seen, device, last_key; unsigned long fs_n, fs_c;'
        ' const struct s15v_inst_survey *survey; const char *const *lines; size_t n; };')
    items = []
    for c in v['cases']:
        ident = o.uid('s15v_inst_lines')
        o.w(clist(ident, c['lines']))
        sv = 'NULL'
        if 'survey' in c:
            x = c['survey']
            sv = o.uid('s15v_inst_survey')
            o.w('static const struct s15v_inst_survey %s = { %du, %du, %dul, %dul, %d, %du, %du, { %du, %du },'
                ' %d, %du, %du };' % (sv, x['device'], x['how'], x['fs_n'], x['fs_c'], int(x['sel_read']),
                                      x['sel_index'], x['sel_verified'], x['slot_bits'][0], x['slot_bits'][1],
                                      int(x['ext_read']), x['part_config'], x['boot_kib']))
            sv = '&' + sv
        items.append((c, ident, sv))
    o.w('static const struct s15v_inst s15v_insts[] = {')
    for c, ident, sv in items:
        o.w('\t{ %s, %du, %s, %du, %du, %du, %du, %du, %du, %du, %du, %du, %du, %du, %dul, %dul, %s, %s, %d },' % (
            cstr(c['name']), c['phase'], cstr(c['version']), c['target'], c['lock'], c['last_rc'], c['os'],
            c['trial'], c['hold_digit'], c['hold_fill'], c['hold_left_s'], c.get('keys_seen', 0),
            c.get('device', 0), c.get('last_key', 255), c.get('fs_n', 0), c.get('fs_c', 0), sv, ident,
            len(c['lines'])))
    o.w('};')
    o.w('#endif')
    o.w()


def tokens(o):
    v = load('info_tokens.json')
    o.w('#ifdef S15V_TOKENS')
    o.w('/* info_tokens.json: groups of alternatives (NULL-terminated), `not` words, `follows`. */')
    o.w('struct s15v_token { const char *name; unsigned rows; const char *follows;'
        ' const char *const *const *groups; const char *const *nots; };')
    toks = []
    for name, spec in v['tokens'].items():
        gids = []
        for g in spec['groups']:
            gid = o.uid('s15v_alt')
            o.w(clist(gid, g))
            gids.append(gid)
        groups = o.uid('s15v_groups')
        o.w('static const char *const *const %s[] = { %s };' % (groups, ', '.join(gids + ['NULL'])))
        nots = o.uid('s15v_not')
        o.w(clist(nots, spec.get('not', [])))
        toks.append((name, spec.get('rows', 1), spec.get('follows'), groups, nots))
    o.w('static const struct s15v_token s15v_tokens[] = {')
    for name, rows, follows, groups, nots in toks:
        o.w('\t{ %s, %du, %s, %s, %s },' % (cstr(name), rows, cstr(follows), groups, nots))
    o.w('};')
    o.w('/* examples: token, rows, values (name, value pairs), expected match */')
    o.w('struct s15v_example { const char *token; const char *const *rows; const char *const *values; bool expect; };')
    ex = []
    for token, rows, values, expect in v['examples']:
        r = o.uid('s15v_exrows')
        o.w(clist(r, rows))
        vals = o.uid('s15v_exvals')
        flat = []
        for k, x in values.items():
            flat += [k, str(x)]
        o.w(clist(vals, flat))
        ex.append((token, r, vals, expect))
    o.w('static const struct s15v_example s15v_examples[] = {')
    for token, r, vals, expect in ex:
        o.w('\t{ %s, %s, %s, %s },' % (cstr(token), r, vals, cbool(expect)))
    o.w('};')
    o.w('#endif')
    o.w()


def gray(o):
    v = load('gray.json')
    o.w('#ifdef S15V_GRAY')
    o.w('/* gray.json (S-15.173): RGB565 colors and their grays. */')
    o.w('struct s15v_gray { const char *name; uint16_t color, gray; };')
    o.w('static const struct s15v_gray s15v_gray[] = {')
    for c in v['cases']:
        o.w('\t{ %s, 0x%su, 0x%su },' % (cstr(c['name']), c['color'].lower(), c['gray'].lower()))
    o.w('};')
    o.w('#endif')
    o.w()


def gen():
    o = Out()
    o.w('/* SPDX-License-Identifier: GPL-2.0-or-later */')
    o.w('/* Generated by tests/vectors/s15/gen_s15_vectors.py from tests/vectors/s15/ui/ and tests/oracle/S-15-menus/. Do not edit. */')
    o.w('#ifndef S15_VECTORS_H')
    o.w('#define S15_VECTORS_H')
    o.w()
    o.w('#include <stdbool.h>')
    o.w('#include <stddef.h>')
    o.w('#include <stdint.h>')
    o.w()
    console(o)
    keys(o)
    screens(o)
    upgrade(o)
    tokens(o)
    gray(o)
    os_write(o)
    installer(o)
    o.w('#define S15V_N(a) (sizeof(a) / sizeof((a)[0]))')
    o.w()
    o.w('#endif /* S15_VECTORS_H */')
    return '\n'.join(o.lines) + '\n'


def main():
    genout.write(OUT, gen())
    genout.finish()
    return 0


if __name__ == '__main__':
    sys.exit(main())
