#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate wire_vectors.h from tests/oracle/S-11-packages/.

Run from the repository root:  python3 -I -B tests/vectors/wire/gen_wire_vectors.py --out DIR

s11_vectors.h states checks 1-15 of each package and pre-parses the zips of
zips.json. test_wire_install runs each package through s11_install() end to
end, so this header adds what s11_model.py gives for the zip each accepted
package carries: the reason of checks 16-17, the staged tree (S-11.032) and
the version a completed run records (S-11.038). The model decrypts with the
conformance package's AES and reads the zip with Python's zipfile. It also
carries the disk cases of S-11.048, for test_wire_fat the S-12 vectors of the
FAT volume's names, stamps and populated RAM disk, and for test_wire_s09 the
piece lengths of S-09's chunked reads.
"""
import calendar
import hashlib
import json
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import genout  # noqa: E402  (before anything the generator reads)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
SPEC = os.path.join(ROOT, 'tests', 'oracle', 'S-11-packages')
OUT = 'wire_vectors.h'
sys.path.insert(0, os.path.join(ROOT, 'tests', 'oracle', 'conformance'))
sys.path.insert(0, SPEC)
import gen_vectors as g  # noqa: E402
import s11_model as m  # noqa: E402


def c_str(s):
    """A C string literal; bytes outside printable ASCII as octal escapes (UTF-8)."""
    if s is None:
        return 'NULL'
    out = []
    for b in s.encode('utf-8'):
        c = chr(b)
        if c in '\\"':
            out.append('\\' + c)
        elif 0x20 <= b < 0x7F:
            out.append(c)
        else:
            out.append('\\%03o' % b)
    return '"%s"' % ''.join(out)


def reason(r):
    return 'S11_REASON_' + (r or 'NONE')


def unit_of(u):
    return m.Unit(g.AES0 if u['k0'] else None, g.AES1 if u['k1'] else None, u['option'], u['dev'],
                  q0=g.PUB if u['q0'] else None)


def installs(out):
    with open(os.path.join(SPEC, 'vectors', 'packages.json')) as f:
        d = json.load(f)
    rows, files = [], []
    for c in d['cases']:
        with open(os.path.join(SPEC, 'vectors', c['file']), 'rb') as f:
            data = f.read()
        e = c['expect']
        got = m.check_package(data, c['form'], unit_of(c['unit']), file_name=c['file_name'], link=c['link'])
        assert got['reason'] == e['reason'], c['case']
        zr, staged, pv = None, None, None
        if got['accepted']:
            zr, staged = m.check_zip(got['zip'], 1 << 30)
            assert zr == e['zip_reason'], c['case']
            if zr is None:
                pv = m.package_version(staged)
                assert (None if pv is None else list(pv)) == e['recorded_version'], c['case']
        first = len(files)
        for path in sorted(staged or {}):
            b = staged[path]
            files.append('{ %s, %d, %d, %s }' % (
                c_str(path), b is None, 0 if b is None else len(b),
                c_str(None if b is None else hashlib.sha256(b).hexdigest())))
        rows.append('{ %s, %s, %d, %d, { %s }, %d, %d }' % (
            c_str(c['case']), reason(zr), zr is None and staged is not None and m.content_only(staged),
            pv is not None, ', '.join(str(x) for x in (pv or (0, 0, 0, 0))), first, len(files) - first))
    disk = []
    for c in d['disk_cases']:
        e = c['expect']
        disk.append('{ %s, %s, %s, %s, %s, %s }' % (
            c_str(c['case']), c_str('\n'.join('%s=%s' % (n, f) for n, f in c['entries'])),
            c_str(e['package']), reason(e['reason']), c_str(e['os_sha256']), c_str(e['decoy_os_sha256'])))
    out += [
        '/* a staged path under upgrade/ (S-11.032): a directory, or a file of',
        ' * `size` bytes with this SHA-256 */',
        'struct wirev_file { const char *path; int is_dir; uint32_t size; const char *sha256; };',
        '/* packages.json in order: the reason of checks 16-17 for an accepted',
        ' * package, whether its run is content-only (S-11.056), the package version',
        ' * (S-11.038), the staged files wirev_files[first .. first + count) */',
        'struct wirev_install { const char *name; enum s11_reason zip_reason; int content_only,',
        '\thas_version; unsigned version[4]; size_t first, count; };',
        '/* packages.json disk_cases (S-11.048): the root, "name=file" lines in',
        ' * directory order; the package picked (NULL: none) and its reason; the',
        ' * SHA-256 of the os.ldr staged, and of the decoy zip\'s */',
        'struct wirev_disk { const char *name, *entries, *package; enum s11_reason reason;',
        '\tconst char *os_sha256, *decoy_os_sha256; };',
        '',
        'static const struct wirev_file wirev_files[] = {',
    ]
    out += ['\t%s,' % r for r in files] + ['};', '', 'static const struct wirev_install wirev_installs[] = {']
    out += ['\t%s,' % r for r in rows] + ['};', '', 'static const struct wirev_disk wirev_disk[] = {']
    out += ['\t%s,' % r for r in disk] + ['};', '']


def s12(name):
    with open(os.path.join(ROOT, 'tests', 'oracle', 'S-12-filesystems', 'vectors', name + '.json')) as f:
        return json.load(f)


def fat(out):
    stamps = ['{ %dull, %d, %d, %d }' % (c['epoch'], c['date'] is not None, c['date'] or 0, c['time'] or 0)
              for c in s12('stamps')['cases']]
    names = s12('names')
    forms = {'sfn': 1, 'sfn_nt': 2, 'lfn': 4, 'refused': 0}
    create = ['{ %s, %d }' % (c_str(c['name']), sum(forms[f] for f in c['forms'])) for c in names['create']]
    pop = s12('ramdisk_populated')
    inp = pop['inputs']
    assert len(inp['logs']) == 1
    log = inp['logs'][0]
    pfiles = dict(pop['files'])
    pfiles['systeminfo/licenses.txt'] = inp['licenses']
    tree = ['{ %s, %d, %d, %s }' % (c_str(p), t['type'] == 'dir', t.get('len', 0), c_str(t.get('sha256')))
            for p, t in sorted(pop['tree'].items())]
    ent = ['{ %s, %d, %d, %d, %d, %d, %d, %d, %d, %d }' % (
        c_str(e['path']), e['attr'], e['nt'], e['crt_tenths'], e['crt_time'], e['crt_date'], e['acc_date'],
        e['wrt_time'], e['wrt_date'], e['size']) for e in pop['entries']]
    out += [
        '/* stamps.json (S-12.016): seconds since 1970 and the FAT date and time;',
        ' * known 0: unspecified */',
        'struct wirev_stamp { uint64_t utc; int known; unsigned date, time; };',
        '/* names.json create (S-12.012): the entry forms allowed, bits 1 short',
        ' * name, 2 short name with the NT case bits, 4 long name; 0 refused */',
        'struct wirev_create { const char *name; unsigned forms; };',
        '/* ramdisk_populated.json (S-12.015-S-12.017): the tree and entries */',
        'struct wirev_tree { const char *path; int is_dir; uint32_t len; const char *sha256; };',
        'struct wirev_entry { const char *path; unsigned attr, nt, crt_tenths, crt_time, crt_date,',
        '\tacc_date, wrt_time, wrt_date; uint32_t size; };',
        '',
        'static const struct wirev_stamp wirev_stamps[] = {',
    ]
    out += ['\t%s,' % r for r in stamps] + ['};', '', 'static const struct wirev_create wirev_create[] = {']
    out += ['\t%s,' % r for r in create] + ['};', '']
    out += ['/* names.json display (S-12.011): raw root entries, and the names they',
            ' * show in order, one per line */',
            'static const char wirev_display_entries[] =']
    out += ['\t"%s"' % e for e in names['display']['entries']]
    out[-1] += ';'
    out += ['static const char wirev_display_names[] = %s;' % c_str('\n'.join(names['display']['names'])), '']
    out += ['/* the populated RAM disk: the RTC time of the system-information files,',
            ' * the log in yaffs2 logs/ (contents pattern(len) of gen_vectors.py) */',
            '#define WIREV_POP_RTC %dull' % calendar.timegm(tuple(inp['rtc_utc'])),
            '#define WIREV_POP_LOG %s' % c_str(log['name']),
            '#define WIREV_POP_LOG_MTIME %du' % calendar.timegm(tuple(log['mtime_utc'])),
            '#define WIREV_POP_LOG_LEN %du' % log['pattern'],
            'static const char *const wirev_pop_files[][2] = {']
    out += ['\t{ %s, %s },' % (c_str(p), c_str(t)) for p, t in sorted(pfiles.items())]
    out += ['};', 'static const struct wirev_tree wirev_pop_tree[] = {']
    out += ['\t%s,' % r for r in tree] + ['};', 'static const struct wirev_entry wirev_pop_entries[] = {']
    out += ['\t%s,' % r for r in ent] + ['};', '']


def s09(out):
    with open(os.path.join(ROOT, 'tests', 'oracle', 'S-09-bridge', 'vectors', 'file_read.json')) as f:
        cases = json.load(f)
    rows = []
    for c in cases:
        if c['fail_piece'] is not None:
            continue    # an injected failure: the slave's own test
        rs = [p['r'] for p in c['pieces']]
        assert len(rs) <= 4
        rows.append('{ %s, %d, %d, %d, { %s }, %d, %d }' % (
            c_str(c['name']), c['file_len'], c['pos'], c['n'], ', '.join(str(r) for r in rs + [0] * (4 - len(rs))),
            len(rs), c['result'][0]))
    out += ['/* file_read.json (S-09.035) without its injected failure: the file\'s',
            ' * length, the position, N, the pieces\' results and the sum */',
            'struct wirev_read { const char *name; uint32_t file_len, pos, n; int32_t r[4];',
            '\tunsigned pieces; int32_t result; };',
            'static const struct wirev_read wirev_reads[] = {']
    out += ['\t%s,' % r for r in rows] + ['};', '']


def main():
    out = [
        '/* SPDX-License-Identifier: GPL-2.0-or-later */',
        '/* Generated by tests/vectors/wire/gen_wire_vectors.py from',
        ' * tests/oracle/S-09-bridge/, tests/oracle/S-11-packages/ and tests/oracle/S-12-filesystems/.',
        ' * Do not edit. */',
        '#ifndef WIRE_VECTORS_H',
        '#define WIRE_VECTORS_H',
        '',
        '#include <stddef.h>',
        '#include <stdint.h>',
        '',
        '#include "waffle/s11_common.h"',
        '',
    ]
    installs(out)
    fat(out)
    s09(out)
    out += ['#endif /* WIRE_VECTORS_H */', '']
    genout.write(OUT, '\n'.join(out))
    genout.finish()


if __name__ == '__main__':
    main()
