#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate s10c_vectors.h from tests/oracle/S-10c-host-commands/vectors/.

Run from the repository root:  python3 -I -B tests/vectors/s10c/gen_s10c_vectors.py --out DIR

Byte strings go into one blob array and are referenced as {offset, length}.
The carrier receive vectors are reduced to the stream bytes each message
feeds the MIDI link's S-06 receiver (S-10c.021), computed with s10c_model.
Info texts that commands.json matches by pattern are made exact with the
bootloader version the test's fake reports (1.0.0).
"""
import json
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import genout  # noqa: E402  (before anything the generator reads)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
SPEC = os.path.join(ROOT, 'tests', 'oracle', 'S-10c-host-commands')
OUT = 's10c_vectors.h'
sys.path.insert(0, SPEC)
import s10c_model  # noqa: E402

BL_RE = r'bl_version:[0-9]+\.[0-9]+\.[0-9]+(\+[0-9]+)?'
FAKE_BL = 'bl_version:1.0.0'


def load(name):
    with open(os.path.join(SPEC, 'vectors', name)) as f:
        return json.load(f)


class Blob:
    def __init__(self):
        self.data = bytearray()

    def ref(self, b):
        if b is None:
            return '{ 0, 0, 0 }'
        b = bytes(b)
        off = len(self.data)
        self.data += b
        return '{ 1, %d, %d }' % (off, len(b))

    def emit(self):
        out = ['static const uint8_t s10cv_blob[] = {']
        for i in range(0, len(self.data), 16):
            out.append('\t' + ' '.join('0x%02x,' % b for b in self.data[i:i + 16]))
        if not self.data:
            out.append('\t0')
        out.append('};')
        return out


def c_str(s):
    out = []
    for b in s.encode('latin-1'):
        c = chr(b)
        out.append('\\%03o' % b if c in '"\\?' or not 0x20 <= b < 0x7F else c)
    return '"' + ''.join(out) + '"'


def exact_info(pattern):
    p = pattern
    assert p.startswith('^') and p.endswith('$')
    p = p[1:-1].replace(BL_RE, FAKE_BL)
    text = re.sub(r'\\(.)', r'\1', p)
    assert not re.search(r'[\[\]()*+?|^$]', text.replace('+45', '')), text
    return text


def main():
    blob = Blob()
    body = []

    def table(name, ctype, rows):
        body.append('static const %s %s[] = {' % (ctype, name))
        body.extend('\t' + r + ',' for r in rows)
        body.append('};')
        body.append('')

    h = bytes.fromhex
    # S-10c.001, S-10c.002
    p7 = load('pack7.json')
    table('s10cv_pack', 'struct s10cv_pair',
          ['{ %s, %s, %s }' % (c_str(v['name']), blob.ref(h(v['data'])), blob.ref(h(v['packed'])))
           for v in p7['pack']])
    table('s10cv_unpack', 'struct s10cv_pair',
          ['{ %s, %s, %s }' % (c_str(v['name']), blob.ref(h(v['data'])), blob.ref(h(v['packed'])))
           for v in p7['unpack']])

    # S-10c.003, S-10c.004, S-10c.006
    um = load('usbmidi.json')
    table('s10cv_usb_send', 'struct s10cv_pair',
          ['{ %s, %s, %s }' % (c_str(v['name']), blob.ref(h(v['message'])), blob.ref(h(v['packets'])))
           for v in um['send']])
    rows = []
    for v in um['receive']:
        msgs = b''.join(len(h(m)).to_bytes(2, 'big') + h(m) for m in v['messages'])
        rows.append('{ %s, %s, %s }' % (c_str(v['name']), blob.ref(h(v['packets'])), blob.ref(msgs)))
    table('s10cv_usb_receive', 'struct s10cv_pair', rows)

    # S-10c.007
    sx = load('sysex.json')
    cls = {'ignored': 0, 'te': 1, 'identity': 2, 'carrier': 3}
    table('s10cv_classify', 'struct s10cv_class',
          ['{ %s, %s, %d }' % (c_str(v['name']), blob.ref(h(v['message'])), cls[v['class']])
           for v in sx['classify']])

    # S-10c.020-S-10c.022
    ca = load('carrier.json')
    table('s10cv_carrier_encode', 'struct s10cv_pair',
          ['{ %s, %s, %s }' % (c_str(v['name']), blob.ref(h(v['stream'])), blob.ref(h(v['message'])))
           for v in ca['encode']])
    rows = []
    for v in ca['tx']:
        msgs = b''.join(len(h(m)).to_bytes(2, 'big') + h(m) for m in v['messages'])
        rows.append('{ %s, %s, %s }' % (c_str(v['name']), blob.ref(h(v['encoded'])), blob.ref(msgs)))
    table('s10cv_carrier_tx', 'struct s10cv_pair', rows)
    rows = []
    for v in ca['rx']:
        for i, m in enumerate(v['messages']):
            s = s10c_model.carrier_stream(h(m))
            rows.append('{ %s, %s, %s }' % (c_str('%s[%d]' % (v['name'], i)), blob.ref(h(m)),
                                            blob.ref(s)))
    table('s10cv_carrier_rx', 'struct s10cv_pair', rows)

    # S-10c.080-S-10c.091 and the identity exchange: host messages, replies.
    # A reply the vector pins by fields and a text pattern only is replaced by
    # the model's reply, once that reply is checked against the pattern.
    cm = load('commands.json')
    dev = cm['device']
    rows = []
    for v in sx['vectors']:
        model = s10c_model.Device(meta=dev['meta'], dsp_serial=dev['dsp_serial'],
                                  hw_rev=dev['hw_rev'])
        for i, st in enumerate(v['steps']):
            replies = b''
            mine = s10c_model.midi_message(model, h(st['host']))
            assert len(mine) == len(st['replies']), v['name']
            for r, m in zip(st['replies'], mine):
                if 'bytes' in r:
                    b = h(r['bytes'])
                else:
                    te = s10c_model.parse_te(m)
                    assert te['cmd'] == r['te']['cmd'] and te['status'] == r['te']['status']
                    assert re.match(r['te']['text_re'], te['payload'].decode('ascii')), v['name']
                    b = m
                replies += len(b).to_bytes(2, 'big') + b
            rows.append('{ %s, %d, %s, %s }' % (c_str(v['name']), i == 0, blob.ref(h(st['host'])),
                                                blob.ref(replies)))
    table('s10cv_sysex', 'struct s10cv_step', rows)

    # S-10c.040-S-10c.075: S-06 command transcripts
    body.append('static const uint32_t s10cv_fsn_words[4] = { %s };' %
                ', '.join('%su' % w for w in dev['fsn_words']))
    body.append('')
    assert dev['meta'] == {'factory': 1, 'os': [1, 2, 3, 45], 'serial': 'AB123456', 'sku': 7,
                           'sw': [1, 2, 3, 0]}
    assert dev['dsp_serial'] == 'ZZ0123456789ABCDEF' and dev['hw_rev'] == 255
    rows = []
    co = load('coprocessor.json')
    links = {'cdc': 0, 'midi': 1, 'psoc': 2, 'nordic': 3}
    for v in cm['vectors'] + co['vectors']:
        assert v['script']['ignore'] == [{'id': ['0x03', '0x04']}, {'id': '0x0e', 'prefix': '92'}]
        d = v.get('device', {})
        assert set(d) <= {'option', 'fsn_readable', 'held', 'slave'}
        first = True
        for st in v['script']['steps']:
            for f in st.get('inject', []):
                rows.append('{ %s, %d, %d, %d, %d, %d, %d, 1, 0x%02x, 0x%02x, %s, 0 }' % (
                    c_str(v['name']), first, links[v['link']], d.get('option', False),
                    d.get('fsn_readable', True), d.get('held', True), d.get('slave', True),
                    int(f['route'], 16),
                    int(f['id'], 16),
                    blob.ref(h(f['payload']))))
                first = False
            e = st.get('expect')
            if e is None:
                continue
            if 'payload_re' in e:
                payload, prefix = exact_info(e['payload_re']).encode('ascii'), 0
            elif 'prefix' in e:
                payload, prefix = h(e['prefix']), 1
            else:
                payload, prefix = h(e['payload']), 0
            rows.append('{ %s, %d, %d, %d, %d, %d, %d, 0, 0x%02x, 0x%02x, %s, %d }' % (
                c_str(v['name']), first, links[v['link']], d.get('option', False),
                d.get('fsn_readable', True), d.get('held', True), d.get('slave', True),
                int(e['route'], 16),
                int(e['id'], 16), blob.ref(payload),
                prefix))
            first = False
    table('s10cv_commands', 'struct s10cv_cmd', rows)

    # S-10c.045: each id on each coprocessor link
    cls = {'claimed': 0, 'yes': 1, 'silent': 2, 'no': 3}
    table('s10cv_coproc_service', 'struct s10cv_service',
          ['{ %d, 0x%02x, %d }' % (links[s['link']], int(s['id'], 16), cls[s['service']])
           for s in co['service']])
    table('s10cv_coproc_progress', 'struct s10cv_service',
          ['{ %d, 0x%02x, 1 }' % (links[p['link']], int(p['route'], 16))
           for p in co['progress'] if p['submitted_by'] in ('psoc', 'nordic')])

    # S-10c.054, S-10c.055, S-10c.088
    inf = load('info.json')
    rows = []
    for v in inf['info']:
        f = v['fields']
        otp = f['dsp_otp']
        rows.append('{ %s, %s, %d, %s, %d, %d, { %s }, { %s }, { %s }, %d, %s }' % (
            c_str(v['name']), c_str(f['serial']), otp is not None, blob.ref(h(otp) if otp else b''),
            f['hw_rev'], f['sku'], ', '.join(map(str, f['sw_version'])),
            ', '.join(map(str, f['bl_version'])), ', '.join(map(str, f['os_version'])), f['factory'],
            c_str(v['text'])))
    table('s10cv_info', 'struct s10cv_info', rows)
    table('s10cv_te_code', 'struct s10cv_te_code',
          ['{ 0x%08xu, %d, %s }' % (int(v['word'], 16), v['passes'], c_str(v['code']))
           for v in inf['te_code']])

    # S-10c.101-S-10c.105
    pr = load('progress.json')
    table('s10cv_percent', 'struct s10cv_percent',
          ['{ %du, %du, %d, %d }' % (v['done'], v['total'], v['percent'], v['te_percent'])
           for v in pr['percent']])
    table('s10cv_progress_s06', 'struct s10cv_progress',
          ['{ %du, %du, %s, -1, 0, %s }' % (v['done'], v['total'], c_str(v['label']),
                                           blob.ref(h(v['payload']))) for v in pr['s06']])
    table('s10cv_progress_te', 'struct s10cv_progress',
          ['{ %du, %du, %s, %d, %d, %s }' % (v['done'], v['total'], c_str(v['label']),
                                            -1 if v['req_id'] is None else v['req_id'],
                                            int(v['outcome']),
                                            blob.ref(h(v['message']))) for v in pr['dfu']])
    body.append('static const char *const s10cv_labels[] = { %s };' %
                ', '.join(c_str(v['label']) for v in pr['labels']))
    body.append('/* S-10c.106: the label after a step\'s Skipped; reason as S-11.058 gives it */')
    body.append('struct s10cv_skip { unsigned step; const char *reason, *label; };')
    body.append('static const struct s10cv_skip s10cv_skips[] = {')
    body.extend('\t{ %du, %s, %s },' % (v['step'], c_str(v['reason']), c_str(v['label'])) for v in pr['skipped'])
    body.append('};')
    body.append('static const char *const s10cv_reasons[] = { %s };' %
                ', '.join(c_str(r) for r in pr['reasons']))
    body.append('')

    lines = ['/* SPDX-License-Identifier: GPL-2.0-or-later */',
             '/* Generated by tests/vectors/s10c/gen_s10c_vectors.py from',
             ' * tests/oracle/S-10c-host-commands/vectors/. Do not edit. */',
             '#ifndef S10C_VECTORS_H', '#define S10C_VECTORS_H', '',
             '#include <stddef.h>', '#include <stdint.h>', '',
             '/* present (0 for null), offset into s10cv_blob, length */',
             'struct s10cv_ref { int present; size_t off, len; };',
             '/* a, b: as each table says; lists are u16be length-prefixed entries */',
             'struct s10cv_pair { const char *name; struct s10cv_ref a, b; };',
             '/* class: 0 ignored, 1 TE, 2 identity, 3 carrier */',
             'struct s10cv_class { const char *name; struct s10cv_ref msg; int cls; };',
             '/* a step of a sequence from a fresh boot: the host message, the replies */',
             'struct s10cv_step { const char *name; int first; struct s10cv_ref host, replies; };',
             '/* inject 1: a frame from the host; 0: the frame expected next (prefix: the',
             ' * payload starts with these bytes). first: the vector starts here. */',
             '/* link: 0 CDC, 1 the MIDI carrier, 2 the PSoC link, 3 the Nordic link */',
             '/* held 0: before held mode (S-10c.045), where the board starts no transfer;',
             ' * slave 0: the slave absent (S-09.008) */',
             'struct s10cv_cmd { const char *name; int first, link, option, fsn_readable, held, slave,',
             '\tinject;',
             '\tunsigned route, id; struct s10cv_ref payload; int prefix; };',
             'struct s10cv_info { const char *name; const char *serial; int dsp_ok;',
             '\tstruct s10cv_ref dsp_otp; unsigned hw_rev, sku; unsigned sw[4], bl[4], os[4];',
             '\tunsigned factory; const char *text; };',
             'struct s10cv_te_code { uint32_t word; int passes; const char *code; };',
             '/* S-10c.045 service: id; cls 0 claimed by the coprocessor, 1 a reply,',
             ' * 2 served with no reply, 3 not served. Progress: id is the route. */',
             'struct s10cv_service { int link; unsigned id; int cls; };',
             '/* te_percent: the TE reply form\'s, 0 for no counts (S-10c.103) */',
             'struct s10cv_percent { uint32_t done, total; int percent, te_percent; };',
             '/* req_id -1: none (and for the S-06 form); outcome: the report of',
             ' * S-10c.106; out: the 0E payload or the TE message */',
             'struct s10cv_progress { uint32_t done, total; const char *label; int req_id;',
             '\tint outcome; struct s10cv_ref out; };', '']
    lines += blob.emit() + [''] + body + ['#endif /* S10C_VECTORS_H */', '']
    genout.write(OUT, '\n'.join(lines))
    genout.finish()


if __name__ == '__main__':
    sys.exit(main())
