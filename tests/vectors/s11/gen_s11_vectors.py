#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate s11_vectors.h from tests/oracle/S-11-packages/.

Run from the repository root:  python3 -I -B tests/vectors/s11/gen_s11_vectors.py --out DIR

The test reads the package files under tests/oracle/S-11-packages/vectors/pkg/
at run time; this header carries their expectations and the conformance test
keys. For each package the generator records the digest, r and s the
implementation must pass to its P-224 verifier and the verdict the
conformance package's verifier (images.p224) gives, and the signature state
(S-11.021). Zips are pre-parsed with Python's zipfile into member tables,
standing in for the zip reader (S-11.030-S-11.035).
"""
import hashlib
import io
import json
import os
import struct
import sys
import zipfile
import zlib
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import genout  # noqa: E402  (before anything the generator reads)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
SPEC = os.path.join(ROOT, 'tests', 'oracle', 'S-11-packages')
OUT = 's11_vectors.h'
sys.path.insert(0, os.path.join(ROOT, 'tests', 'oracle', 'conformance'))
sys.path.insert(0, SPEC)
from images import p224, tfw  # noqa: E402
import s11_model  # noqa: E402


def load(name):
    with open(os.path.join(SPEC, 'vectors', name)) as f:
        return json.load(f)


def tsv(name):
    with open(os.path.join(SPEC, name)) as f:
        lines = [l.rstrip('\n') for l in f if not l.startswith('#')]
    head = lines[0].split('\t')
    out = []
    for l in lines[1:]:
        if not l:
            continue
        cells = l.split('\t')
        cells += [''] * (len(head) - len(cells))
        out.append(dict(zip(head, [c[1:-1].replace('""', '"') if c.startswith('"') else c for c in cells])))
    return out


class Blob:
    def __init__(self):
        self.data = bytearray()

    def ref(self, b):
        if b is None:
            return '{ 0, 0, 0 }'
        off = len(self.data)
        self.data += bytes(b)
        return '{ 1, %d, %d }' % (off, len(b))

    def emit(self):
        out = ['static const uint8_t s11v_blob[] = {']
        for i in range(0, len(self.data), 16):
            out.append('\t' + ' '.join('0x%02x,' % b for b in self.data[i:i + 16]))
        out.append('};')
        return out


def c_str(s):
    if s is None:
        return 'NULL'
    out = []
    for b in s.encode('latin-1'):
        c = chr(b)
        out.append('\\%03o' % b if c in '"\\?' or not 0x20 <= b < 0x7F else c)
    return '"' + ''.join(out) + '"'


def c_bytes(b):
    return '{ ' + ', '.join('0x%02x' % x for x in b) + ' }'


def local_zip64(z, i):
    """A zip64 extra field (id 1) in the member's central or local header."""
    if 1 in s11_model._extra_ids(i.extra):
        return True
    o = i.header_offset
    if z[o:o + 4] != b'PK\x03\x04':
        return False
    nlen, xlen = struct.unpack_from('<HH', z, o + 26)
    return 1 in s11_model._extra_ids(z[o + 30 + nlen:o + 30 + nlen + xlen])


REASONS = ['NONE'] + s11_model.REASONS


def reason(r):
    return 'S11_REASON_' + (r or 'NONE')


def main():
    blob = Blob()
    body = []

    def table(name, ctype, rows):
        body.append('static const %s %s[] = {' % (ctype, name))
        body.extend('\t' + r + ',' for r in rows)
        body.append('};')
        body.append('')

    # ---- keys (conformance test keys only, S-11.025)
    k0, d, q0 = tfw.test_keys()
    k1 = tfw.test_keys(b'opxy-conformance-k1')[0]
    qx, qy = q0
    body.append('static const uint8_t s11v_k0[32] = %s;' % c_bytes(k0))
    body.append('static const uint8_t s11v_k1[32] = %s;' % c_bytes(k1))
    body.append('static const uint8_t s11v_q0[56] = %s;' % c_bytes(
        qx.to_bytes(28, 'big') + qy.to_bytes(28, 'big')))
    body.append('/* the test signing key whose public key is s11v_q0, for packages a test builds */')
    body.append('static const uint8_t s11v_d[28] = %s;' % c_bytes(d.to_bytes(28, 'big')))
    body.append('')

    # ---- packages.json: S-11.001-S-11.025, S-11.047, S-11.055; the OP-1F's packages_1f.json
    # (S-11.026, S-11.007)
    def package_rows(pk):
        rows = []
        for c in pk['cases']:
            rows.append(package_row(c))
        return rows

    sig_no = {None: -1, 'none': 0, 'invalid': 1, 'valid': 2}     # enum s11_sig

    def package_row(c):
        with open(os.path.join(SPEC, 'vectors', c['file']), 'rb') as f:
            data = f.read()
        e, u = c['expect'], c['unit']
        o = 0x40 if c['form'] == 'file' else 0
        man = data[o + 0x40:o + 0x40 + 0x300]
        sig = None
        # S-11.012: what a verifier is given for a signature that is present, the key index 0x00 and Q0 read;
        # a plain package (S-11.027) has none
        if (len(man) == 0x300 and u['q0'] == 'test' and not e.get('plain') and any(man[0x10:0x50])
                and man[0x89] == 0x00):
            digest = hashlib.sha256(man[:0x10] + bytes(0x40) + man[0x50:]).digest()
            r, s = man[0x14:0x30], man[0x34:0x50]
            ok = p224.verify(q0, digest, int.from_bytes(r, 'big'), int.from_bytes(s, 'big'))
            sig = (digest, r, s, ok)
        zsha = bytes.fromhex(e['zip_sha256']) if e.get('zip_sha256') else None
        return '{ %s, %s, %d, %d, %s, %d, %d, %d, %d, %s, %d, %d, %d, %s, %s, %d }' % (
            c_str(c['case']), c_str(c['file']), c['form'] == 'file', c['link'] == 'cdc',
            c_str(c.get('file_name')), u['q0'] == 'test', u['k0'] == 'test', u['k1'] == 'test_k1',
            u['option'], reason(e['reason']), sig_no[e.get('signature')], int(e.get('forced', -1)),
            sig is not None, blob.ref(sig[0] + sig[1] + sig[2]) if sig else blob.ref(None),
            blob.ref(zsha), sig[3] if sig else 0)
    table('s11v_packages', 'struct s11v_package', package_rows(load('packages.json')))
    table('s11v_packages_1f', 'struct s11v_package', package_rows(load('packages_1f.json')))

    # ---- versions.json, crc16.json, product_words.json
    v = load('versions.json')
    rows = []
    for p in v['parse']:
        b = None if p['file_hex'] is None else bytes.fromhex(p['file_hex'])
        rows.append('{ %s, { %s } }' % (blob.ref(b), ', '.join(map(str, p['version']))))
    table('s11v_version_parse', 'struct s11v_version_parse', rows)
    table('s11v_version_compare', 'struct s11v_version_compare',
          ['{ { %s }, { %s }, %d }' % (', '.join(map(str, p['a'])), ', '.join(map(str, p['b'])),
                                      p['result']) for p in v['compare']])
    table('s11v_crc16', 'struct s11v_crc16',
          ['{ %s, 0x%s }' % (blob.ref(bytes.fromhex(p['data_hex'])), p['crc16'])
           for p in load('crc16.json')])
    table('s11v_product', 'struct s11v_product',
          ['{ 0x%su, %d }' % (p['word'], p['passes']) for p in load('product_words.json')])
    table('s11v_product_1f', 'struct s11v_product',
          ['{ 0x%su, %d }' % (p['word'], p['passes']) for p in load('product_words_1f.json')])

    # ---- the acceptance table: S-11.023 (check 12)
    rows = []
    for r in s11_model.acceptance_table():
        rows.append('{ 0x%s, %s, %s, %s, %s, %s }' % (
            r['payload_key_index'], r['k0_readable'], r['k1_readable'], r['option'], r['dev_mode'],
            reason(None if r['verdict'] == 'accept' else r['verdict'])))
    table('s11v_acceptance', 'struct s11v_acceptance', rows)

    # ---- zips.json: S-11.030-S-11.035 (checks 16 and 17)
    zp = load('zips.json')
    comp_bit = {'os': 0, 'bootloader': 1, 'ble': 2, 'nordic_bl0': 3, 'nordic_bl1': 4, 'backplane': 5}
    members, cases = [], []
    for c in zp['cases']:
        z = bytes.fromhex(c['zip_hex'])
        zf = zipfile.ZipFile(io.BytesIO(z))
        infos = zf.infolist()
        first = len(members)
        for i in infos:
            o = i.header_offset
            nlen, xlen = struct.unpack_from('<HH', z, o + 26)
            start = o + 30 + nlen + xlen
            comp = z[start:start + i.compress_size]
            data = None
            if i.compress_type == 0:
                data = comp
            elif i.compress_type == 8:
                d = zlib.decompressobj(-15)
                try:
                    data = d.decompress(comp) + d.flush()
                    if not d.eof:
                        data = None
                except zlib.error:
                    data = None
            runs = b''
            if data is not None:   # run-length: (byte, u32be count)
                k = 0
                while k < len(data):
                    j = k
                    while j < len(data) and data[j] == data[k]:
                        j += 1
                    runs += bytes([data[k]]) + (j - k).to_bytes(4, 'big')
                    k = j
            members.append('{ %s, %d, 0x%04x, %du, 0x%08xu, %d, %d, %s }' % (
                c_str(i.filename), i.compress_type, i.flag_bits, i.file_size, i.CRC,
                local_zip64(z, i), data is not None, blob.ref(runs)))
        e = c['expect']
        mask = sum(1 << comp_bit[n] for n in e.get('components', []))
        staged = '\n'.join(e['staged']) if 'staged' in e else None
        tail = z[-(0x10000 + 22):]
        zip64 = b'PK\x06\x06' in tail or b'PK\x06\x07' in tail
        free = c['free_space']
        cases.append('{ %s, %d, %d, %d, %du, %s, %s, 0x%02x, %s }' % (
            c_str(c['case']), first, len(members) - first, zip64, free is not None,
            '%dull' % (free or 0), reason(e['reason']), mask, c_str(staged)))
    table('s11v_members', 'struct s11v_member', members)
    table('s11v_zips', 'struct s11v_zip', cases)
    table('s11v_space', 'struct s11v_space',
          ['{ %dull, %d, %dull, %s }' % (sum(s['sizes']), s['free'] is not None, s['free'] or 0,
                                         reason(s['reason'])) for s in zp['space_arithmetic']])

    # ---- runs.tsv: S-11.057-S-11.065
    result_code = {n: i for i, n in enumerate(s11_model.RESULT_NAMES)}

    def write_text(w):
        kind, what = w
        if kind in ('progress', 'progress_fails'):
            return '%s %d %d' % (kind, what[0], what[1])
        return '%s %s' % (kind, what)

    rows = []
    for r in tsv('runs.tsv'):
        res = [result_code[x] for x in r['results'].split(',')] if r['results'] else []
        writes = ';'.join(write_text(w) for w in json.loads(r['writes']))
        off = sum(1 << int(x) for x in r['steps_off'].split(',') if x)
        sel = sum(1 << int(x) for x in r['selected'].split(',') if x)
        rows.append('{ %s, %s, %s, 0x%02x, 0x%02x, %s, %s, %s, { %s }, %d, %s, %s }' % (
            c_str(r['case']), r['content_only'], r['nordic_images'], off, sel, r['dfu_entry'], r['nordic_dfu_ok'],
            r['version_file'], ', '.join(map(str, res)) or '0', len(res),
            c_str(r['outcome']), c_str(writes)))
    table('s11v_runs', 'struct s11v_run', rows)

    # ---- interrupted.tsv: S-11.080
    rows = []
    for r in tsv('interrupted.tsv'):
        writes = ';'.join('meta ' + w for w in json.loads(r['metadata_writes']))
        rows.append('{ %s, %d, %d, %d, %s, %s, %s, %s, %s, %s }' % (
            r['state'], r['keys'] == 'held', r['slave'] == 'present', r['meta_write_ok'] == '0',
            c_str(writes), r['notice'], r['state_after'], r['steps'], r['nordic_dfu'], r['record_access']))
    table('s11v_interrupted', 'struct s11v_interrupted', rows)

    # ---- steps.tsv: S-11.057, S-11.066-S-11.071, S-11.090, S-11.095
    step_no = {n: i for i, n in enumerate(s11_model.STEPS)}
    rel_no = {'older': -1, 'equal': 0, 'newer': 1}
    staged_no = {'ok': 0, 'bad': 1, 'read_error': 2}
    start_no = {'ok': 0, 'refused_verified': 1, 'refused_bad': 2}
    rows = []
    for r in tsv('steps.tsv'):
        c = json.loads(r['conditions'])
        rows.append('{ %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d }'
                    % (step_no[r['step']], c.get('content_dir', 0), c.get('files', 0), c.get('move_fails', 0),
                       c.get('present', 0), c.get('present0', 0), c.get('present1', 0), c.get('option', 0),
                       c.get('write_ok', 1), rel_no[c.get('rel', 'newer')], c.get('forced', 0), c.get('dev', 0),
                       c.get('in_dfu', 1), c.get('slot', 0), result_code[r['result']],
                       staged_no[c.get('staged', 'ok')], c.get('selected', 1), c.get('switched_on', 1),
                       start_no[c.get('start', 'ok')], c.get('written_here', 0), c.get('slot_matches', 0)))
    table('s11v_steps', 'struct s11v_step', rows)

    # ---- picks.tsv: S-11.096
    pick_no = {'absent': 'S11_PICK_ABSENT', 'open': 'S11_PICK_OPEN', 'switched off': 'S11_PICK_SWITCHED_OFF',
               'option off': 'S11_PICK_OPTION_OFF', 'up to date': 'S11_PICK_UP_TO_DATE'}
    rows = []
    for r in tsv('picks.tsv'):
        c = json.loads(r['conditions'])
        rows.append('{ %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %s }' % (
            step_no[r['step']], c.get('content_dir', 0), c.get('present', 0), c.get('present0', 0),
            c.get('present1', 0), c.get('option', 0), c.get('switched_on', 1), rel_no[c.get('rel', 'newer')],
            c.get('forced', 0), c.get('known', 1), pick_no[r['pick']]))
    table('s11v_picks', 'struct s11v_pick', rows)

    # ---- staged_check.json: S-11.095
    rows = []
    for c in load('staged_check.json')['cases']:
        reads = [None if r is None else bytes.fromhex(r) for r in c['reads']]
        st = c['stated']
        assert st is not None and len(reads) == 1, c['case']
        rows.append('{ %s, %su, %su, %s, %d }' % (
            c_str(c['case']), st[0], st[1], blob.ref(reads[0]), staged_no[c['expect']]))
    table('s11v_staged_checks', 'struct s11v_staged_check', rows)

    # ---- stock.tsv: S-11.091, S-11.092; progress.json: S-11.062
    table('s11v_stock', 'struct s11v_stock',
          ['{ %s, %s, %s, %s, %s, %s }' % (r['option'], r['valid'], r['bootloader_waffle'],
                                          r['bootloader_present'], r['bootloader_selected'], r['return_to_stock'])
           for r in tsv('stock.tsv') if r['os_form'] == 'plaintext'])
    # ---- bootloader_kind.json: S-11.091, a waffle-boot build or not
    table('s11v_bootloader_kinds', 'struct s11v_bootloader_kind',
          ['{ %s, %s, %d }' % (c_str(c['case']), blob.ref(bytes.fromhex(c['stream'])), c['waffle'])
           for c in load('bootloader_kind.json')['cases']])
    table('s11v_records', 'struct s11v_record',
          ['{ %d, %d, 0x%s }' % (p['step'], p['count'], p['file_hex']) for p in load('progress.json')])

    # ---- content.json: S-11.039, S-11.040, S-11.056, S-11.066, S-11.075-S-11.079
    ct = load('content.json')
    files, sets = [], [0]

    def file_set(tree, prefix=''):
        sets[0] += 1
        for path in sorted(tree):
            files.append('{ %d, %s, %s }' % (sets[0], c_str(prefix + path),
                                            blob.ref(bytes.fromhex(tree[path]))))
        return sets[0]

    def install_rows(ct):
        return ['{ %s, %d, %d, %d, %d, %d }' % (
            c_str(c['case']), file_set(c['staged'], 'upgrade/'), file_set(c['tree_before']),
            file_set(c['tree_after']), c['content_only'], result_code[c['result']]) for c in ct['installs']]
    installs = install_rows(ct)
    ct1f = load('content_1f.json')
    installs_1f = install_rows(ct1f)
    names_1f = ['{ %s, %d, %s, NULL }' % (c_str(c['case']), file_set(c['staged'], 'upgrade/'), c_str(c['name']))
                for c in ct1f['pack_names']]
    rows = []
    for c in ct['pack_names']:
        alt = c.get('also_conforming', [])      # S-11.039: over 4096 bytes
        assert len(alt) <= 1, c['case']
        rows.append('{ %s, %d, %s, %s }' % (c_str(c['case']), file_set(c['staged'], 'upgrade/'),
                                            c_str(c['name']), c_str(alt[0]) if alt else 'NULL'))
    table('s11v_files', 'struct s11v_file', files)
    table('s11v_installs', 'struct s11v_install', installs)
    table('s11v_pack_names', 'struct s11v_pack_name', rows)
    table('s11v_installs_1f', 'struct s11v_install', installs_1f)
    table('s11v_pack_names_1f', 'struct s11v_pack_name', names_1f)

    # ---- disk_search.tsv: S-11.046, S-11.047
    rows = []
    for r in tsv('disk_search.tsv'):
        entries = json.loads(r['entries_in_directory_order'])
        root = '\n'.join(('/' if d else '') + n for n, d in entries if '/' not in n)
        rows.append('{ %s, %s, %s, %s }' % (c_str(r['case']), c_str(root), c_str(r['package']),
                                            r['forced']))
    table('s11v_disk', 'struct s11v_disk', rows)

    lines = ['/* SPDX-License-Identifier: GPL-2.0-or-later */',
             '/* Generated by tests/vectors/s11/gen_s11_vectors.py from',
             ' * tests/oracle/S-11-packages/. Do not edit. */',
             '#ifndef S11_VECTORS_H', '#define S11_VECTORS_H', '',
             '#include <stddef.h>', '#include <stdint.h>', '',
             '#include "waffle/s11_common.h"', '#include "waffle/s11_run.h"', '',
             '/* present (0 for null), offset into s11v_blob, length */',
             'struct s11v_ref { int present; size_t off, len; };',
             '/* sig: digest (32), r (28), s (28) the verifier must be given; verdict. signature: the',
             ' * state (enum s11_sig), -1 when check 8 is not reached */',
             'struct s11v_package { const char *name, *file; int file_form, cdc;',
             '\tconst char *file_name; int q0, k0, k1, option; enum s11_reason reason;',
             '\tint signature, forced; /* -1: not stated */ int has_sig; struct s11v_ref sig, zip_sha256;',
             '\tint verdict; };',
             '/* file: null when absent */',
             'struct s11v_version_parse { struct s11v_ref file; unsigned v[4]; };',
             'struct s11v_version_compare { unsigned a[4], b[4]; int result; };',
             'struct s11v_crc16 { struct s11v_ref data; unsigned crc; };',
             'struct s11v_product { uint32_t word; int passes; };',
             'struct s11v_acceptance { unsigned key_index; int k0, k1, option, dev;',
             '\tenum s11_reason verdict; };',
             '/* a zip member as a zip reader reports it; data: run-length (byte, u32be count),',
             ' * absent (data_ok 0) when the member cannot be read or inflated in full */',
             'struct s11v_member { const char *name; unsigned method, flags; uint32_t size, crc;',
             '\tint zip64_extra, data_ok; struct s11v_ref runs; };',
             '/* components: bit per enum s11_component; staged: the files, one per line */',
             'struct s11v_zip { const char *name; size_t first, count; int zip64_end;',
             '\tunsigned free_known; uint64_t free; enum s11_reason reason; unsigned components;',
             '\tconst char *staged; };',
             'struct s11v_space { uint64_t total; int known; uint64_t free; enum s11_reason reason; };',
             '/* results: the codes of the steps that run, in order; writes: ";"-separated; nordic: the',
             ' * package carries Nordic images; steps_off: the switched-off steps, bit n for step n (S-11.073);',
             ' * selected: the steps selected, bit n for step n (S-11.096);',
             ' * dfu_entry: the run asks the Nordic to enter DFU (S-11.061 item 2) */',
             'struct s11v_run { const char *name; int content_only, nordic; unsigned steps_off, selected;',
             '\tint dfu_entry, nordic_dfu_ok, version_file;',
             '\tunsigned results[7]; unsigned nresults; const char *outcome, *writes; };',
             '/* S-11.080: a boot by its state; write_fails: the state-0 write fails; writes as a run\'s */',
             'struct s11v_interrupted { unsigned state; int keys_held, slave_present, write_fails;',
             '\tconst char *writes; int notice; unsigned state_after; int steps, nordic_dfu, record_access; };',
             '/* rel: the package version against the installed one: -1 older, 0 equal, 1 newer;',
             ' * staged: S-11.095\'s check of the image, 0 passes, 1 fails, 2 a read fails;',
             ' * selected: the step selected on the unit (S-11.096); switched_on: the',
             ' * step is switched on (S-11.073); start: S-02.090\'s start, 0 allowed, 1 refused for verified',
             ' * alone, 2 refused for a bad selected slot; written_here: this power-on\'s bootloader step',
             ' * wrote the selector read; slot_matches: the selected slot\'s record is the image\'s (S-11.068) */',
             'struct s11v_step { unsigned step; int content_dir, files, move_fails, present, present0,',
             '\tpresent1, option, write_ok, rel, forced, dev, in_dfu, slot; unsigned result; int staged;',
             '\tint selected, switched_on, start, written_here, slot_matches; };',
             '/* S-11.096: what the selection offers a step; known: the installed version of a Nordic or',
             ' * PSoC step is known (not 0.0.0), rel the package\'s against it */',
             'struct s11v_pick { unsigned step; int content_dir, present, present0, present1, option,',
             '\tswitched_on, rel, forced, known; enum s11_pick pick; };',
             '/* S-11.095: the read against the stated values; a read that fails is absent;',
             ' * expect 0 ok, 1 bad, 2 read_error; the bytes a step may write are the read\'s, on ok */',
             'struct s11v_staged_check { const char *name; uint32_t stated_len, stated_crc;',
             '\tstruct s11v_ref read; int expect; };',
             '/* valid: the package\'s signature state is valid (S-11.021); waffle: the bootloader image is a',
             ' * waffle-boot build (S-11.091) */',
             'struct s11v_stock { int option, valid, waffle, bootloader, selected, stock; };',
             'struct s11v_bootloader_kind { const char *name; struct s11v_ref stream; int waffle; };',
             'struct s11v_record { unsigned step, count, bytes; };',
             '/* a file of tree `set`, path relative to /yaffs2/ (staged files under upgrade/) */',
             'struct s11v_file { int set; const char *path; struct s11v_ref data; };',
             'struct s11v_install { const char *name; int staged, before, after, content_only;',
             '\tunsigned result; };',
             'struct s11v_pack_name { const char *name; int staged; const char *pack;\n'
             '\tconst char *alt; };',
             '/* root: the RAM disk root in directory order, one per line, "/" before a directory */',
             'struct s11v_disk { const char *name, *root, *package; int forced; };',
             '']
    lines += blob.emit() + [''] + body + ['#endif /* S11_VECTORS_H */', '']
    genout.write(OUT, '\n'.join(lines))
    genout.finish()


if __name__ == '__main__':
    sys.exit(main())
