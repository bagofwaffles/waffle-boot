# SPDX-License-Identifier: GPL-2.0-or-later
"""S-11 installs: s11_install() and the interrupted-install boot (through h_s11) against
tests/oracle/S-11-packages (vectors, tables, s11_model); peers and writers are h_s11's logging fakes."""

import copy
import io
import json
import os
import sys
import unittest
import zipfile
import zlib

from harness import BUILD, expected_failure, hx, requirement, run, scenario, spec_module, spec_path, tsv
from conformance.images import fwzip, tfw

WORK = os.path.join(BUILD, 's11')
BIG = 1 << 30                               # free space that never refuses
OLD = '1.0.0.0'

_GV = []


def gv():
    """S-11's generator. It puts its own directory first on sys.path: the path is put back once it is
    loaded."""
    if not _GV:
        saved = list(sys.path)
        try:
            _GV.append(spec_module('S-11-packages', 'gen_vectors'))
        finally:
            sys.path[:] = saved
    return _GV[0]


def model():
    return gv().m


def vectors(name):
    with open(spec_path('S-11-packages', 'vectors', name), encoding='utf-8') as f:
        return json.load(f)


def keys():
    aes0, _, pub = tfw.test_keys()
    aes1 = tfw.test_keys(b'opxy-conformance-k1')[0]
    return (pub[0].to_bytes(28, 'big') + pub[1].to_bytes(28, 'big')).hex(), aes0.hex(), aes1.hex()


def otp(q0=True, k0=True, k1=True):
    q, a, b = keys()
    return 'otp %s %s %s' % (q if q0 else 'x', a if k0 else 'x', b if k1 else 'x')


def otp_unit(u):
    return otp(u['q0'] is not None, u['k0'] is not None, u['k1'] is not None)


def write(name, data):
    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, name)
    with open(path, 'wb') as f:
        f.write(bytes(data))
    return path


def package(name, zip_bytes, **kw):
    """A package of zip_bytes (gen_vectors.assemble's knobs), written to WORK; its path."""
    return write(name + '.tfw', gv().assemble(zip_bytes, **kw))


def vtext(v):
    return '.'.join(str(x) for x in tuple(v) + (0,) * (4 - len(v)))


class Result:
    """install's fields."""

    def __init__(self, line):
        f = line.split()
        self.reason = None if f[0] == 'none' else f[0]
        self.te, self.forced, self.stock = f[1] == '1', f[2] == '1', f[3] == '1'
        self.end, self.outcome, self.step, self.result = f[4], f[5], int(f[6]), int(f[7])


def tree(line):
    """tree's entries: {path: None for a directory, (size, crc32) for a file}, lost+found left out."""
    out = {}
    for e in [] if line == '-' else line.split():
        if e.startswith('lost+found/'):
            continue
        if e.endswith('/'):
            out[e[:-1]] = None
        else:
            p, size, crc = e.rsplit(':', 2)
            out[p] = (int(size), int(crc, 16))
    return out


def files(t, under=''):
    return {p: v for p, v in t.items() if v is not None and p.startswith(under)}


def outside(t, d='upgrade'):
    return {p: v for p, v in t.items() if p != d and not p.startswith(d + '/')}


def expect_tree(fs):
    """The tree entries of {path: bytes, or None for a directory}, the directories files imply included."""
    out = {}
    for p, b in fs.items():
        parts = p.split('/')
        for i in range(1, len(parts)):
            out.setdefault('/'.join(parts[:i]), None)
        out[p] = None if b is None else (len(b), zlib.crc32(b))
    return out


def unquote(cell):
    """A table cell as csv quotes it ("[[""a"", 0]]")."""
    return cell[1:-1].replace('""', '"') if cell.startswith('"') else cell


def content_code(code):
    return 'content=%d' % code


def put(path, data):
    return 'put %s %s' % (path, hx(data))


BOOT = 'boot 0 0 1'                         # a boot: reason 0, keys not held, the slave present
# The log's notes, which are no change (no cut falls on them): the content step's result, S-11.096's
# selection, the reason of each Skipped (S-11.058)
NOTES = ('content=', 'select:', 'skip:')
# setup()'s steps and answers: every step switched on (S-11.073) and every step that can be picked selected
# (S-11.096), so that the scenarios written for the Nordic and PSoC steps run them
ALL_ON = dict(steps_off=(), answer=lambda pickable: ('install', pickable))


WRITES = ('meta', 'meta_fails', 'progress', 'progress_fails', 'nordic')
STEP_OF = {'os_write': 1, 'sw_version': 1, 'bootloader_write': 2, 'nordic_send:ble': 3, 'nordic_slot': 4,
           'nordic_send:bl0': 4, 'nordic_send:bl1': 4, 'psoc_program': 5,
           'fs_remove': 0, 'fs_move': 0, 'fs_copy': 0}


def log(line):
    return [] if line == '-' else line.split()


def writes(lg):
    """The log's metadata, progress-record and Nordic entries, as s11_model.run() lists its writes."""
    return [t for t in lg if t.split(':')[0] in WRITES]


def model_writes(w, version=None):
    out = []
    for kind, what in w:
        if kind in ('progress', 'progress_fails'):
            out.append('%s:%d,%d' % (kind, what[0], what[1]))
        elif kind == 'meta' and what == 'os_version=package,state=0':
            out.append('meta:os_version=%s,state=0' % vtext(version))
        elif kind == 'meta':
            out.append('meta:' + what)
        else:
            out.append('%s:%s' % (kind, what))
    return out


def step_effects(lg):
    """{step: [effects]} from a run's log: each effect falls between the step's records."""
    out, step = {}, None
    for t in lg:
        k = t.split(':')[0]
        if k in ('progress', 'progress_fails'):
            step = int(t.split(':')[1].split(',')[0])
        name = t if t in STEP_OF else t.split(':')[0]
        if name in STEP_OF:
            out.setdefault(step, []).append(name)
    return out


def setup(meta='0 ' + OLD, tree_files=None):
    cmds = ['vol', 'meta ' + meta, otp(), 'copro 0.0.0.0 0.0.0.0 0.0.0.0']
    for k in ('dfu', 'indfu', 'send', 'sendbl', 'psoc', 'os', 'bootloader'):
        cmds.append('set %s 1' % k)
    cmds += ['set slot 0', 'set space real', 'set readfail -', 'set stagefail 0', 'set removefail 0',
             'set movefail 0', 'set metafail 0', 'set recordfail 0', 'set cut -', 'set cutat -', 'set staged -',
             'set off -', 'set select install all']
    for p, b in (tree_files or {}).items():
        cmds.append(put(p, b))
    return cmds


class Session:
    """Commands for one h_s11 run, with a name on the lines whose results the test reads."""

    def __init__(self):
        self.cmds, self.names = [], []

    def add(self, *cmds, name=None):
        for c in cmds:
            self.cmds.append(c)
            self.names.append(None)
        if name is not None:
            self.names[-1] = name

    def run(self):
        out = run('s11', self.cmds)
        res = {}
        for n, o in zip(self.names, out):
            if n is not None:
                res[n] = o
        return res


def staged_of(zip_bytes):
    reason, staged = model().check_zip(zip_bytes, BIG)
    assert reason is None, reason
    return staged


def img(n, seed):
    return bytes((i * 7 + seed) & 0xFF for i in range(n))


def boot_img(n, seed):
    """A bootloader image that check 18 accepts (S-11.035, S-02.091): the generator's bootable slot
    stream, then img(n, seed) after its FINAL block."""
    return gv().BOOT_STREAM + img(n, seed)


def os_img(n, seed):
    """An OS image that check 18 accepts (S-11.035, S-05.024): a stream with no destination, then img(n,
    seed) after its FINAL block."""
    return gv().os_image(32) + img(n, seed)


def comp_img(c, n, seed):
    if c == 'os':
        return os_img(n, seed)
    return boot_img(n, seed) if c == 'bootloader' else img(n, seed)


# S-11.034: a first-stage image and its version file, staged as plain members
FIRST_STAGE_FILE = {'firmware/blackfin/bin/spiboot.ldr': img(0xC001, 13),
                    'firmware/blackfin/bin/spiboot.version': b'1.2.0+0\n'}


def full_zip(version=(1, 2, 0), content=True, fw_version=True, **comp_versions):
    """Every component, both Nordic bootloaders, a first-stage file (S-11.034: no component, never looked
    for) and (content) a content file."""
    comps = {c: (comp_img(c, 100 + i, i), comp_versions.get(c, version))
             for i, c in enumerate(('os', 'bootloader', 'ble', 'backplane'))}
    extra = dict(FIRST_STAGE_FILE)
    if content:
        extra['firmware/blackfin/content/op1.json'] = b'{}'
    z = fwzip.build(comps, version=version, nordic_bootloaders=(img(64, 9), img(64, 11),
                                                                comp_versions.get('nordic_bl', version)),
                    extra=extra)
    return z if fw_version else without(z, fwzip.TOP_VERSION)


def without(z, name):
    src = zipfile.ZipFile(io.BytesIO(z))
    b = io.BytesIO()
    with zipfile.ZipFile(b, 'w', zipfile.ZIP_DEFLATED) as out:
        for i in src.infolist():
            if i.filename != name:
                out.writestr(i, src.read(i))
    return b.getvalue()


def zip_of(fs):
    return gv().zipb({p: b for p, b in fs.items()})


# --- SC-11-REFUSE, SC-11-ACCEPT ------------------------------------------------------------------------

PRESET = {'user/song.xy': b'u', 'upgrade/progress.log': b'\x03\x00', 'upgrade/old.bin': b'\x01'}


class Refusals(unittest.TestCase):

    @scenario('S-11/SC-11-REFUSE')
    def test_SC_11_REFUSE_packages(self):
        """Every case of packages.json on its path: the reason; a refusal of checks 1-15 changes nothing on
        the volume and writes nothing; one of checks 16-17 only the staging area."""
        cases = vectors('packages.json')['cases']
        s = Session()
        for c in cases:
            u = c['unit']
            s.add(*setup(tree_files=PRESET))
            s.add(otp_unit(u), 'load ' + spec_path('S-11-packages', 'vectors', c['file']), 'tree', name=c['case'] + '/before')
            s.add('install %s %d %d %s' % (c['form'], c['link'] == 'cdc', u['option'], c['file_name'] or '-'),
                  name=c['case'])
            s.add('log', name=c['case'] + '/log')
            s.add('tree', name=c['case'] + '/after')
            s.add('meta', name=c['case'] + '/meta')
        out = s.run()
        for c in cases:
            e, name = c['expect'], c['case']
            with self.subTest(case=name):
                r = Result(out[name])
                want = e['reason'] or e.get('zip_reason')
                self.assertEqual(r.reason, want)
                if 'te_signed' in e:
                    self.assertEqual(r.te, e['te_signed'])
                if 'forced' in e:
                    self.assertEqual(r.forced, e['forced'])
                before, after = tree(out[name + '/before']), tree(out[name + '/after'])
                lg = log(out[name + '/log'])
                if e['reason']:                         # checks 1-15 (S-11.020)
                    self.assertEqual(r.outcome, 'refused')
                    self.assertEqual(lg, [])
                    self.assertEqual(after, before)
                elif want:                              # checks 16-17
                    self.assertEqual(r.outcome, 'refused')
                    self.assertEqual(lg, [])
                    self.assertEqual(outside(after), outside(before))
                    self.assertNotIn('upgrade/old.bin', after)
                    self.assertNotIn('upgrade/progress.log', after)
                else:
                    self.assertIn(r.outcome, ('done', 'nothing'))
                    st, ver = out[name + '/meta'].split()
                    self.assertEqual(int(st, 16), 0)
                    rv = e.get('recorded_version')
                    self.assertEqual(ver, vtext(rv) if rv else OLD)

    @scenario('S-11/SC-11-STREAM')
    def test_SC_11_STREAM(self):
        """Each stream_* zip of zips.json, option on and off: stream_bootloader_ok installs (option on: the
        bootloader step reports OK), and so does stream_os_ok; the others are refused STREAM, or SIZE for
        stream_size_first, before any step: no write, no metadata write, nothing outside the staging area
        changed."""
        cases = [c for c in vectors('zips.json')['cases'] if c['case'].startswith('stream_')]
        self.assertEqual(len(cases), 13)
        s = Session()
        for c in cases:
            path = package('zip_' + c['case'], bytes.fromhex(c['zip_hex']))
            for opt in (0, 1):
                name = '%s_%d' % (c['case'], opt)
                s.add(*setup(tree_files=PRESET))
                s.add('load ' + path, 'tree', name=name + '/before')
                s.add('install file 0 %d op-xy.tfw' % opt, name=name)
                s.add('log', name=name + '/log')
                s.add('tree', name=name + '/after')
        out = s.run()
        for c in cases:
            for opt in (0, 1):
                name = '%s_%d' % (c['case'], opt)
                with self.subTest(case=name):
                    r = Result(out[name])
                    want = c['expect']['reason']
                    self.assertEqual(r.reason, want)
                    lg = log(out[name + '/log'])
                    if want:
                        self.assertIn(want, ('STREAM', 'SIZE'))
                        self.assertEqual(r.result, model().REASON_CODE[want])
                        self.assertEqual(model().REASON_CODE[want], model().BAD_FILE)
                        self.assertEqual(lg, [])
                        self.assertEqual(outside(tree(out[name + '/after'])), outside(tree(out[name + '/before'])))
                    else:
                        self.assertIn(r.outcome, ('done', 'nothing'))
                        self.assertEqual('bootloader_write' in lg,
                                         opt == 1 and c['case'] == 'stream_bootloader_ok')

    @scenario('S-11/SC-11-REFUSE')
    def test_SC_11_REFUSE_zips(self):
        """Every case of zips.json in an otherwise valid package: its reason; a refusal leaves everything but
        /yaffs2/upgrade/ as it was and makes no metadata write; an accepted zip stages its components."""
        cases = vectors('zips.json')['cases']
        s = Session()
        for c in cases:
            path = package('zip_' + c['case'], bytes.fromhex(c['zip_hex']))
            s.add(*setup(tree_files=PRESET))
            s.add('set space ' + ('fail' if c['free_space'] is None else str(c['free_space'])))
            s.add('load ' + path, 'tree', name=c['case'] + '/before')
            s.add('install file 0 1 op-xy.tfw', name=c['case'])
            s.add('log', name=c['case'] + '/log')
            s.add('tree', name=c['case'] + '/after')
        out = s.run()
        for c in cases:
            e, name = c['expect'], c['case']
            with self.subTest(case=name):
                r = Result(out[name])
                self.assertEqual(r.reason, e['reason'])
                before, after = tree(out[name + '/before']), tree(out[name + '/after'])
                self.assertNotIn('upgrade/old.bin', after)
                if e['reason']:
                    self.assertEqual(log(out[name + '/log']), [])
                    self.assertEqual(outside(after), outside(before))
                    self.assertEqual(r.result, model().REASON_CODE[e['reason']])
                    continue
                for comp in e['components']:
                    self.assertIn('upgrade/' + model().COMPONENTS[comp][0], after, comp)
                for p in e.get('staged', []):
                    self.assertIn('upgrade/' + p, after)

    @scenario('S-11/SC-11-REFUSE')
    def test_SC_11_REFUSE_read_errors(self):
        """The base package with the medium's reads failing inside the TE header (SHORT) and inside the zip
        (ZIP): refused, nothing written."""
        base = spec_path('S-11-packages', 'vectors', 'pkg', 'form_file_ok.bin')
        zip_at = 0x40 + 0x40 + 0x300 + 0x100        # preamble, TE header, manifest, name block
        s = Session()
        for name, off, want in (('header', 0x48, 'SHORT'), ('zip', zip_at + 0x20, 'ZIP')):
            s.add(*setup(tree_files=PRESET))
            s.add('load ' + base, 'tree', name=name + '/before')
            s.add('set readfail %d 4' % off)
            s.add('install file 0 0 op-xy.tfw', name=name)
            s.add('log', name=name + '/log')
            s.add('tree', name=name + '/after')
        out = s.run()
        for name, _, want in (('header', 0, 'SHORT'), ('zip', 0, 'ZIP')):
            with self.subTest(read=name):
                self.assertEqual(Result(out[name]).reason, want)
                self.assertEqual(log(out[name + '/log']), [])
                self.assertEqual(tree(out[name + '/after']), tree(out[name + '/before']))

    @scenario('S-11/SC-11-REFUSE-REPEAT')
    def test_SC_11_REFUSE_REPEAT(self):
        """64 refused packages, one case per reason of checks 1-15 in turn on the disk path and two on each
        host link, then the base package: it installs."""
        cases = vectors('packages.json')['cases']
        per = {}
        for c in cases:
            if c['expect']['reason'] and c['form'] == 'file' and not c['unit']['option']:
                per.setdefault(c['expect']['reason'], c)
        body = [c for c in cases if c['expect']['reason'] and c['form'] == 'body']
        order = list(per.values())
        s = Session()
        s.add(*setup())
        n = 0
        while n < 60:
            c = order[n % len(order)]
            s.add(otp_unit(c['unit']), 'load ' + spec_path('S-11-packages', 'vectors', c['file']))
            s.add('install file 0 0 %s' % c['file_name'], name='r%d' % n)
            n += 1
        for link in (1, 0):
            for c in body:
                s.add('load ' + spec_path('S-11-packages', 'vectors', c['file']))
                s.add('install body %d 0 -' % link, name='r%d' % n)
                n += 1
        s.add(otp(), 'load ' + spec_path('S-11-packages', 'vectors', 'pkg', 'form_file_ok.bin'))
        s.add('install file 0 0 op-xy.tfw', name='base')
        out = s.run()
        self.assertGreaterEqual(n, 64)
        for i in range(n):
            self.assertIsNotNone(Result(out['r%d' % i]).reason, i)
        r = Result(out['base'])
        self.assertEqual((r.reason, r.outcome), (None, 'done'))


class Acceptance(unittest.TestCase):

    @requirement('S-16.039')
    @scenario('S-11/SC-11-ACCEPT')
    def test_SC_11_ACCEPT(self):
        """The acceptance table (S-11's acceptance_table()): checks 8 and 12 under each unit. One package per signature key index, signature
        validity and payload key index; the OTP and the option vary in h_s11. Dev mode has no input: developer mode
        (S-15.172) has no effect on it, and the rows with it on must read as those with it off."""
        g = gv()
        rows = g.m.acceptance_table()
        built, s = {}, Session()
        rows.sort(key=lambda r: (r['sig_key_index'], r['sig_valid'], r['payload_key_index']))
        for i, r in enumerate(rows):
            key = (r['sig_key_index'], r['sig_valid'], r['payload_key_index'])
            if key not in built:
                ki = int(key[2], 16)
                built[key] = package('acc_%s_%s_%s' % key, g.BASE_ZIP, sig_index=int(key[0], 16),
                                     sign_d=g.D if key[1] == '1' else g.D_OTHER, key_index=ki,
                                     key=g.AES0 if ki in (0xFF, 0x02) else None)
                s.add('load ' + built[key])
            s.add(otp(r['q0_readable'] == '1', r['k0_readable'] == '1', r['k1_readable'] == '1'))
            s.add('install file 0 %s p.tfw' % r['option'], name=i)
        out = s.run()
        for i, r in enumerate(rows):
            with self.subTest(row=r):
                got = Result(out[i])
                self.assertEqual(got.reason or 'accept', r['verdict'])
                if r['verdict'] in ('accept', 'KEY', 'NOKEY'):
                    self.assertEqual(got.te, r['te_signed'] == '1')
                if r['verdict'] == 'accept':
                    self.assertEqual(got.outcome, 'done')


# --- SC-11-STAGE, SC-11-SPACE, SC-11-SIZE ---------------------------------------------------------------

def zip_case(name):
    return bytes.fromhex(next(c for c in vectors('zips.json')['cases'] if c['case'] == name)['zip_hex'])


def members(z):
    zf = zipfile.ZipFile(io.BytesIO(z))
    return {('upgrade/' + i.filename.rstrip('/')): (None if i.filename.endswith('/') else zf.read(i))
            for i in zf.infolist()}


class Staging(unittest.TestCase):

    @scenario('S-11/SC-11-STAGE')
    def test_SC_11_STAGE(self):
        """The staging area holds exactly the zip's members (and the run's record); what was there before is
        gone, and nothing outside it changed; a refused member name or a failing member write leaves it
        empty."""
        old = {'upgrade/progress.log': b'\x03\x00', 'upgrade/old.bin': b'old', 'user/song.xy': b'u'}
        s = Session()
        for name in ('with_dir_entries', 'outside_firmware', 'name_dotdot', 'stagefail'):
            z = zip_case('with_dir_entries' if name == 'stagefail' else name)
            s.add(*setup(tree_files=old))
            if name == 'stagefail':
                s.add('set stagefail 1')
            s.add('load ' + package('stage_' + name, z), 'tree', name=name + '/before')
            s.add('install file 0 0 op-xy.tfw', name=name)
            s.add('tree', name=name + '/after')
            s.add('get upgrade/progress.log', name=name + '/record')
        out = s.run()
        for name in ('with_dir_entries', 'outside_firmware'):
            with self.subTest(zip=name):
                r = Result(out[name])
                self.assertEqual((r.reason, r.outcome), (None, 'done'))
                after = tree(out[name + '/after'])
                want = expect_tree(members(zip_case(name)))
                want['upgrade/progress.log'] = (2, zlib.crc32(b'\x00\x00'))
                got = {p: v for p, v in after.items() if p.startswith('upgrade/')}
                self.assertEqual(got, {p: v for p, v in want.items() if p.startswith('upgrade/')})
                self.assertEqual(outside(after), outside(tree(out[name + '/before'])))
        for name in ('name_dotdot', 'stagefail'):
            with self.subTest(zip=name):
                self.assertEqual(Result(out[name]).reason, 'MEMBER')
                after = tree(out[name + '/after'])
                self.assertEqual(files(after, 'upgrade/'), {})
                self.assertEqual(out[name + '/record'], 'absent')

    @scenario('S-11/SC-11-SPACE')
    def test_SC_11_SPACE(self):
        """An OS of 12000 bytes: the members' total plus 0x400000 free installs, one byte less refuses SPACE
        (Failure), a failing query SPACEQUERY (Internal error); a refused run writes no member and leaves
        /yaffs2/upgrade/ existing and empty."""
        z = fwzip.build({'os': os_img(12000, 3)}, version=(1, 2, 0))
        total = sum(i.file_size for i in zipfile.ZipFile(io.BytesIO(z)).infolist())
        path = package('space', z)
        s = Session()
        for name, space in (('fits', total + 0x400000), ('short', total + 0x400000 - 1), ('query', 'fail')):
            s.add(*setup(tree_files={'upgrade/progress.log': b'\x03\x00'}))
            s.add('set space %s' % space, 'load ' + path)
            s.add('install file 0 0 op-xy.tfw', name=name)
            s.add('log', name=name + '/log')
            s.add('tree', name=name + '/tree')
        out = s.run()
        self.assertEqual(Result(out['fits']).outcome, 'done')
        for name, reason, code in (('short', 'SPACE', 1), ('query', 'SPACEQUERY', 4)):
            with self.subTest(case=name):
                r = Result(out[name])
                self.assertEqual((r.reason, r.result), (reason, code))
                self.assertEqual(log(out[name + '/log']), [])
                t = tree(out[name + '/tree'])
                self.assertIn('upgrade', t)
                self.assertIsNone(t['upgrade'])
                self.assertEqual([p for p in t if p.startswith('upgrade/')], [])

    @requirement('S-16.150')
    @scenario('S-11/SC-11-SIZE')
    def test_SC_11_SIZE(self):
        """Each limit_*_over zip, with the option on and off: refused SIZE before any step, no write; each
        limit_*_at zip installs."""
        cases = [c for c in vectors('zips.json')['cases'] if c['case'].startswith('limit_')]
        s = Session()
        for c in cases:
            path = package('size_' + c['case'], bytes.fromhex(c['zip_hex']))
            for option in (0, 1):
                s.add(*setup(), 'load ' + path)
                s.add('install file 0 %d op-xy.tfw' % option, name=(c['case'], option))
                s.add('log', name=(c['case'], option, 'log'))
        out = s.run()
        for c in cases:
            for option in (0, 1):
                with self.subTest(case=c['case'], option=option):
                    r = Result(out[(c['case'], option)])
                    lg = log(out[(c['case'], option, 'log')])
                    if c['expect']['reason'] == 'SIZE':
                        self.assertEqual(r.reason, 'SIZE')
                        self.assertEqual(lg, [])
                    else:
                        self.assertIsNone(r.reason)
                        self.assertIn(r.outcome, ('done', 'nothing'))


# --- SC-11-DLC -------------------------------------------------------------------------------------------

def content_tree(t):
    return {p: v for p, v in files(t).items() if not p.startswith('upgrade/')}


class Content(unittest.TestCase):

    def run_case(self, c, s, tag, fault=None):
        before = {p: bytes.fromhex(b) for p, b in c['tree_before'].items()}
        staged = {p: bytes.fromhex(b) for p, b in c['staged'].items()}
        s.add(*setup(tree_files=before))
        if fault:
            s.add(fault)
        s.add('load ' + package('dlc_' + c['case'], zip_of(staged)), 'install file 0 0 op-xy.tfw', name=tag)
        s.add('log', name=tag + '/log')
        s.add('tree', name=tag + '/tree')

    @scenario('S-11/SC-11-DLC')
    def test_SC_11_DLC(self):
        """content.json: the tree after the content step and its result; a content-only install makes no
        metadata write."""
        cases = vectors('content.json')['installs']
        s = Session()
        for c in cases:
            self.run_case(c, s, c['case'])
        out = s.run()
        for c in cases:
            with self.subTest(case=c['case']):
                lg = log(out[c['case'] + '/log'])
                self.assertIn(content_code(model().RESULT_NAMES.index(c['result'])), lg)
                want = {p: (len(bytes.fromhex(b)), zlib.crc32(bytes.fromhex(b))) for p, b in c['tree_after'].items()}
                self.assertEqual(content_tree(tree(out[c['case'] + '/tree'])), want)
                if c['content_only']:
                    self.assertEqual([t for t in lg if t.startswith('meta')], [])

    @scenario('S-11/SC-11-DLC')
    def test_SC_11_DLC_removal_fails(self):
        """full_keeps_dlc and pack_replace_partial with the removal of S-11.075 or S-11.077 failing: every
        staged file is still installed and the step reports OK."""
        cases = [c for c in vectors('content.json')['installs'] if c['case'] in ('full_keeps_dlc', 'pack_replace_partial')]
        s = Session()
        for c in cases:
            self.run_case(c, s, c['case'], fault='set removefail 1')
        out = s.run()
        cdir = model().CONTENT_DIR
        for c in cases:
            with self.subTest(case=c['case']):
                lg = log(out[c['case'] + '/log'])
                self.assertIn('fs_remove', lg)
                self.assertIn(content_code(model().OK), lg)
                got = content_tree(tree(out[c['case'] + '/tree']))
                for p, b in c['staged'].items():
                    if p.startswith(cdir) and 'partial_upgrade' not in p:
                        b = bytes.fromhex(b)
                        self.assertEqual(got.get('content/' + p[len(cdir):]), (len(b), zlib.crc32(b)), p)


# --- SC-11-RUN-OK, SC-11-RUN-FAIL, the Nordic and PSoC steps ---------------------------------------------

class Runs(unittest.TestCase):

    def check_run(self, out, tag, staged, option, faults=None, dfu=True, tree0=None, os_version=OLD,
                  version=None, steps_off=(), answer=ALL_ON['answer']):
        """The run's writes and step effects against s11_model.install() on the same staging area, with the
        steps switched off and the answers h_s11 was given (setup()'s by default)."""
        m = model()
        dev = m.Device(tree0, tuple(int(x) for x in os_version.split('.')))
        want = m.install(dev, staged, option=option, faults=faults or {}, nordic_dfu_ok=dfu, steps_off=steps_off,
                         answer=answer)
        lg = log(out[tag + '/log'])
        pv = m.package_version(staged)
        self.assertEqual(writes(lg), model_writes(want['writes'], pv), tag)
        eff = step_effects(lg)
        for step, code in want['ran']:
            ran = eff.get(step, [])
            if step == 0:
                # S-11.096: a content step not selected moves nothing and reports no result of its own
                if code == m.SKIPPED:
                    self.assertFalse([t for t in lg if t.startswith('content=')], tag)
                    self.assertEqual(ran, [], tag)
                else:
                    self.assertIn(content_code(code), lg)
                continue
            if code == m.OK:
                self.assertTrue(ran, (tag, step))
            elif code in (m.NOT_FOUND, m.SKIPPED, m.ALREADY):
                writes_ = [e for e in ran if e not in ('nordic_slot',)]
                self.assertEqual(writes_, [], (tag, step, m.RESULT_NAMES[code]))
        r = Result(out[tag])
        self.assertEqual(r.outcome, {'nothing to do': 'nothing'}.get(want['outcome'], want['outcome']), tag)
        # S-11.096: asked once, offered the steps that can be picked; the run given the selection, or none
        def steps(x):
            return ','.join(map(str, x)) or '-'
        self.assertEqual([t for t in lg if t.startswith('select:offered=')],
                         ['select:offered=' + steps(want['pickable'])], tag)
        if want['canceled']:
            self.assertIn('select:canceled', lg, tag)
        else:
            self.assertIn('select:selected=' + steps(want['selected']), lg, tag)
        # S-11.058: each Skipped with its reason
        reasons = {m.SKIP_OPTION_OFF: 'option_off', m.SKIP_NOT_SELECTED: 'not_selected',
                   m.SKIP_SWITCHED_OFF: 'switched_off'}
        skips = []
        for step, code in want['ran']:
            if code == m.SKIPPED:
                if step in m.SWITCHABLE and step in steps_off:
                    why = m.SKIP_SWITCHED_OFF
                elif step in m.PROTECTED and not option:
                    why = m.SKIP_OPTION_OFF
                else:
                    why = m.SKIP_NOT_SELECTED
                    self.assertNotIn(step, want['selected'], tag)
                skips.append('skip:%d=%s' % (step, reasons[why]))
        self.assertEqual([t for t in lg if t.startswith('skip:')], skips, tag)
        st, ver = out[tag + '/meta'].split()
        self.assertEqual(int(st, 16), dev.state, tag)
        self.assertEqual(ver, vtext(dev.os_version), tag)
        rec = out[tag + '/record']
        self.assertEqual(None if rec == 'absent' else bytes.fromhex(rec), dev.record, tag)
        return lg, r

    def install(self, s, tag, z, option=1, extra=(), fwcmd=0, form='file', **kw):
        s.add(*setup())
        s.add(*extra)
        s.add('load ' + package(tag, z, **kw), 'install %s %d %d op-xy.tfw' % (form, fwcmd, option), name=tag)
        s.add('log', name=tag + '/log')
        s.add('meta', name=tag + '/meta')
        s.add('get upgrade/progress.log', name=tag + '/record')

    @scenario('S-11/SC-11-SWITCHED-OFF')
    def test_SC_11_SWITCHED_OFF(self):
        """Steps 3-5 switched off (S-11.073), as a build may have them. The SC-11-RUN-OK package, option off: no DFU entry,
        no boot-slot query, nothing sent to the Nordic or the PSoC; the version read and the exit query made;
        steps 2-5 Skipped with their reasons, step 6 Not found; done, os_version the package's, the coprocessor values unchanged;
        as the model's run. Variants: the Nordic refusing DFU (the same, done); a package of only Nordic and
        PSoC images (nothing to do, nothing sent); a package of only the OS (steps 3-5 Not found)."""
        z = full_zip(version=(1, 2, 0))
        only = gv().zipb({'firmware/ble/bin/ble.bin': img(200, 2), 'firmware/ble/bin/ble.version': b'1.2.0+0\n',
                          'firmware/ble/bin/nordic_bootloader_0.bin': img(64, 3),
                          'firmware/ble/bin/nordic_bootloader.version': b'1.2.0+0\n',
                          'firmware/backplane/bin/backplane.cyacd': img(300, 5),
                          'firmware/backplane/bin/backplane.version': b'1.2.0+0\n',
                          'firmware/firmware.version': b'1.2.0+0\n'})
        os_only = fwzip.build({'os': os_img(300, 1)}, version=(1, 2, 0))
        off = ['set off 3,4,5', 'set select unasked']
        s = Session()
        for tag, zz, extra in (('off', z, off), ('nodfu', z, off + ['set dfu 0']), ('only', only, off),
                               ('os_only', os_only, off)):
            self.install(s, tag, zz, option=0, extra=extra)
            s.add('copro', name=tag + '/copro')
        out = s.run()
        m = model()
        for tag, zz, dfu in (('off', z, True), ('nodfu', z, False), ('only', only, True), ('os_only', os_only, True)):
            with self.subTest(variant=tag):
                lg, r = self.check_run(out, tag, staged_of(zz), False, dfu=dfu, steps_off=(3, 4, 5), answer=None)
                for e in ('nordic:enter_dfu', 'nordic_slot', 'nordic_send:ble', 'nordic_send:bl0', 'nordic_send:bl1',
                          'psoc_program', 'bootloader_write'):
                    self.assertNotIn(e, lg)
                self.assertIn('nordic:read_versions', lg)
                self.assertIn('nordic:exit_dfu_if_in_dfu', lg)
                self.assertEqual(out[tag + '/copro'], '0.0.0.0 0.0.0.0 0.0.0.0')
        self.assertEqual([t for t in log(out['off/log']) if t.startswith('skip:')],
                         ['skip:2=option_off', 'skip:3=switched_off', 'skip:4=switched_off', 'skip:5=switched_off'])
        for tag in ('off', 'nodfu'):
            self.assertEqual(Result(out[tag]).outcome, 'done', tag)
            self.assertEqual(out[tag + '/meta'], '0x0 1.2.0.0', tag)
        self.assertEqual(Result(out['only']).outcome, 'nothing')
        self.assertEqual([t for t in log(out['only/log']) if t.startswith('skip:')],
                         ['skip:3=switched_off', 'skip:4=switched_off', 'skip:5=switched_off'])
        self.assertEqual([t for t in log(out['os_only/log']) if t.startswith('skip:')], [])

    @scenario('S-11/SC-11-CONFIRM')
    def test_SC_11_CONFIRM(self):
        """S-11.096, the selection, with steps 3-5 switched off (S-11.073). The SC-11-RUN-OK package
        (TE-signed, a first-stage file carried), option on: asked once before the run's first write, offered
        content, the OS and the bootloader (never a first-stage file, nothing writes it); every step offered
        selected: all written, a return to the factory bootloader; the bootloader left out: Skipped, not
        selected, no return to the factory bootloader; the bootloader alone: the OS Skipped, not selected,
        and no package version recorded (S-11.064); a cancel: no run, nothing written (no metadata state, no
        progress record, no Nordic exchange), nothing to do; the keys unreadable: every step offered but the
        bootloader. The option off: the bootloader not offered, Skipped, option off. A package without the
        bootloader: not offered. The same package over the CDC firmware command and the MIDI transfer: the
        same ask. The same package installed again: asked again."""
        z = full_zip(version=(1, 2, 0))
        os_only = fwzip.build({'os': os_img(300, 1)}, version=(1, 2, 0))
        body = package('confirm_body', z, pre=False)
        s = Session()
        every = ('install', None)
        legs = [('all', z, 1, every), ('no_bl', z, 1, ('install', (0, 1))), ('only_bl', z, 1, ('install', (2,))),
                ('cancel', z, 1, ('cancel',)), ('unasked', z, 1, None), ('option_off', z, 0, every),
                ('no_protected', os_only, 1, every)]

        def cmd(a):
            if a is None:
                return 'set select unasked'
            if a[0] == 'cancel':
                return 'set select cancel'
            return 'set select install ' + ('all' if a[1] is None else ','.join(map(str, a[1])))

        def answer(a):
            if a is None:
                return None
            if a[0] == 'cancel':
                return lambda p: 'cancel'
            return lambda p: ('install', p if a[1] is None else a[1])
        for tag, zz, opt, a in legs:
            self.install(s, tag, zz, option=opt, extra=['set off 3,4,5', cmd(a)])
        for tag, fwcmd in (('cdc', 1), ('midi', 0)):
            s.add(*setup(), 'set off 3,4,5', 'set select install all', 'load ' + body)
            s.add('install body %d 1 -' % fwcmd, name=tag)
            s.add('log', name=tag + '/log')
        s.add('set select install 0,1', 'install body 0 1 -', name='again')
        s.add('log', name='again/log')
        out = s.run()
        m = model()
        self.assertEqual(m.STEPS_OFF, ())
        for tag, zz, opt, a in legs:
            with self.subTest(leg=tag):
                lg, r = self.check_run(out, tag, staged_of(zz), bool(opt), steps_off=(3, 4, 5), answer=answer(a))
                asks = [i for i, t in enumerate(lg) if t.startswith('select:offered=')]
                self.assertEqual(len(asks), 1, tag)
                written = [i for i, t in enumerate(lg) if t.split(':')[0] in WRITES or t in STEP_OF]
                if written:
                    self.assertLess(asks[0], min(written), 'asked before the run\'s first write')
                bl = a is not None and a[0] == 'install' and (a[1] is None or 2 in a[1])
                self.assertEqual('bootloader_write' in lg, bool(opt and bl and zz is z), tag)
                self.assertFalse([t for t in lg if 'spiboot' in t], tag)
                if zz is z:
                    self.assertTrue(r.te)
                    self.assertEqual(r.stock, bool(opt and bl),
                                     'S-11.091: a return to the factory bootloader only when selected')
        self.assertEqual([t for t in log(out['all/log']) if t.startswith('select:')],
                         ['select:offered=0,1,2', 'select:selected=0,1,2'])
        self.assertEqual(Result(out['all']).outcome, 'done')
        self.assertEqual([t for t in log(out['no_bl/log']) if t.startswith('skip:')],
                         ['skip:2=not_selected', 'skip:3=switched_off', 'skip:4=switched_off', 'skip:5=switched_off'])
        lg = log(out['only_bl/log'])
        self.assertIn('skip:1=not_selected', lg)
        self.assertNotIn('os_write', lg)
        self.assertIn('bootloader_write', lg)
        self.assertEqual([t for t in lg if t.startswith('meta:')], ['meta:state=AB0D', 'meta:state=0'])
        self.assertEqual(out['only_bl/meta'], '0x0 ' + OLD)
        lg = log(out['cancel/log'])
        self.assertEqual([t for t in lg if t.startswith('select:')], ['select:offered=0,1,2', 'select:canceled'])
        self.assertEqual(writes(lg), [])
        self.assertEqual([t for t in lg if t in STEP_OF or t.split(':')[0] in STEP_OF], [])
        self.assertEqual(Result(out['cancel']).outcome, 'nothing')
        self.assertEqual(out['cancel/record'], 'absent')
        self.assertEqual([t for t in log(out['unasked/log']) if t.startswith('select:')],
                         ['select:offered=0,1,2', 'select:selected=0,1'])
        self.assertEqual([t for t in log(out['option_off/log']) if t.startswith('select:offered=')],
                         ['select:offered=0,1'])
        self.assertIn('skip:2=option_off', log(out['option_off/log']))
        self.assertEqual([t for t in log(out['no_protected/log']) if t.startswith('select:offered=')],
                         ['select:offered=1'])
        for tag in ('cdc', 'midi'):
            lg = log(out[tag + '/log'])
            self.assertEqual([t for t in lg if t.startswith('select:')],
                             ['select:offered=0,1,2', 'select:selected=0,1,2'], tag)
            self.assertIn('bootloader_write', lg, tag)
        lg = log(out['again/log'])
        self.assertEqual([t for t in lg if t.startswith('select:')], ['select:offered=0,1,2', 'select:selected=0,1'])
        self.assertNotIn('bootloader_write', lg)

    @scenario('S-11/SC-11-STAGED')
    def test_SC_11_STAGED(self):
        """S-11.095 through s11_run's step results: the OS image's check failing reports Bad file, a read of it
        failing Internal error; the step writes nothing (no os_write, no sw_version), the run stops there
        (S-11.059) and ends as SC-11-RUN-FAIL (state 0), as the model's run with that step's result
        overridden. The same package installed again with the check passing: the step's normal result."""
        z = full_zip(version=(1, 2, 0))
        s = Session()
        self.install(s, 'bad', z, extra=['set staged bad'])
        s.add('set staged -', 'install file 0 1 op-xy.tfw', name='again')
        s.add('log', name='again/log')
        s.add('meta', name='again/meta')
        s.add('get upgrade/progress.log', name='again/record')
        self.install(s, 'read_error', z, extra=['set staged read_error'])
        out = s.run()
        m = model()
        for tag, code, token in (('bad', m.BAD_FILE, 'staged:bad'),
                                 ('read_error', m.INTERNAL, 'staged:read_error')):
            lg, r = self.check_run(out, tag, staged_of(z), True, faults={1: code})
            self.assertEqual(r.outcome, 'failed', tag)
            self.assertEqual((r.step, r.result), (1, code), tag)
            self.assertIn(token, lg, tag)
            self.assertNotIn('os_write', lg, tag)
            self.assertNotIn('sw_version:1.2.0.0', lg, tag)
            self.assertEqual([k for k in step_effects(lg) if k], [], tag)
            self.assertEqual(out[tag + '/meta'].split()[0], '0x0', tag)
        lg, r = self.check_run(out, 'again', staged_of(z), True)
        self.assertEqual(r.outcome, 'done')
        self.assertEqual(step_effects(lg)[1], ['os_write', 'sw_version'])

    @scenario('S-11/SC-11-RUN-OK')
    def test_SC_11_RUN_OK(self):
        """Every component and content, option on: the writes in order, each step's effect between its
        records, os_version the staged firmware.version's; again with the TE header's version 0.1.0.0
        (forced; os_version still firmware.version's), without the version file (the state only), and with
        the first metadata write or the record after step 2 failing (the run goes on, done)."""
        z = full_zip(version=(1, 2, 0))
        s = Session()
        self.install(s, 'ok', z)
        self.install(s, 'tampered', z, version=(0, 1, 0, 0))
        self.install(s, 'no_version', full_zip(fw_version=False))
        self.install(s, 'meta_fails', z, extra=['set metafail 1'])
        self.install(s, 'record_fails', z, extra=['set recordfail 4'])
        out = s.run()
        lg, r = self.check_run(out, 'ok', staged_of(z), True)
        self.assertTrue(r.te and not r.forced)
        eff = step_effects(lg)
        self.assertEqual([k for k in sorted(eff) if k], [1, 2, 3, 4, 5])    # step 6 reserved: no effect
        self.assertEqual(eff[1], ['os_write', 'sw_version'])
        self.assertIn('nordic_send:bl0', lg)
        _, r = self.check_run(out, 'tampered', staged_of(z), True)
        self.assertTrue(r.forced)
        self.check_run(out, 'no_version', staged_of(full_zip(fw_version=False)), True)
        for tag, fail in (('meta_fails', 'meta_fails:state=AB0D'), ('record_fails', 'progress_fails:3,0')):
            with self.subTest(variant=tag):
                lg = log(out[tag + '/log'])
                self.assertIn(fail, lg)
                self.assertEqual(Result(out[tag]).outcome, 'done')
                self.assertEqual(sorted(k for k in step_effects(lg) if k), [1, 2, 3, 4, 5])

    @scenario('S-11/SC-11-RUN-FAIL')
    def test_SC_11_RUN_FAIL(self):
        """The Nordic failing the application transfer: steps 0-3 run, step 3 Failure, record 03 00; after it
        one metadata write setting the state to 0 with os_version as it was, then the Nordic's exit; the next
        boot, keys not held, boots the OS and writes nothing. Variant content-only: the content step failing
        on a move, no metadata write at all."""
        z = fwzip.build({'os': os_img(300, 1), 'ble': img(200, 2)}, version=(1, 2, 0))
        pack = next(c for c in vectors('content.json')['installs'] if c['case'] == 'pack_replace_partial')
        zc = zip_of({p: bytes.fromhex(b) for p, b in pack['staged'].items()})
        s = Session()
        self.install(s, 'fail', z, option=0, extra=['set send 0'])
        s.add('tree', name='fail/tree')
        s.add('set send 1', 'reboot', BOOT, name='boot')
        s.add('log', name='boot/log')
        s.add('tree', name='boot/tree')
        self.install(s, 'content_only', zc, extra=['set movefail 1'])
        out = s.run()
        m = model()
        lg, r = self.check_run(out, 'fail', staged_of(z), False, faults={3: m.FAILURE})
        self.assertEqual((r.outcome, r.step, r.result), ('failed', 3, m.FAILURE))
        self.assertEqual(out['fail/record'], '0300')
        w = writes(lg)
        self.assertEqual(w[w.index('progress:3,0'):], ['progress:3,0', 'meta:state=0', 'nordic:exit_dfu_if_in_dfu'])
        self.assertEqual(out['fail/meta'], '0x0 ' + OLD)
        self.assertEqual(out['boot'], 'os 0x0')
        self.assertEqual(log(out['boot/log']), [])
        self.assertEqual(tree(out['boot/tree']), tree(out['fail/tree']))
        self.assertTrue(m.content_only(staged_of(zc)))
        lg, r = self.check_run(out, 'content_only', staged_of(zc), True, faults={0: m.FAILURE})
        self.assertEqual((r.outcome, r.step, r.result), ('failed', 0, m.FAILURE))
        self.assertIn('fs_move', lg)
        self.assertEqual([t for t in lg if t.startswith('meta')], [])

    @scenario('S-11/SC-11-NORDIC-NODFU')
    def test_SC_11_NORDIC_NODFU(self):
        """The Nordic refusing DFU entry: no step, no record; the metadata write of 0xAB0D, then one of 0; no
        exchange after the refused entry; the next boot boots the OS and writes nothing."""
        z = fwzip.build({'os': os_img(300, 1), 'ble': img(200, 2)}, version=(1, 2, 0))
        s = Session()
        self.install(s, 'nodfu', z, extra=['set dfu 0'])
        s.add('set dfu 1', 'reboot', BOOT, name='boot')
        s.add('log', name='boot/log')
        out = s.run()
        lg, r = self.check_run(out, 'nodfu', staged_of(z), True, dfu=False)
        self.assertEqual([t for t in lg if not t.startswith('select:')],
                         ['meta:state=AB0D', 'nordic:enter_dfu', 'meta:state=0'])
        self.assertEqual(out['nodfu/record'], 'absent')
        self.assertEqual(out['boot'], 'os 0x0')
        self.assertEqual(log(out['boot/log']), [])

    @scenario('S-11/SC-11-NORDIC')
    def test_SC_11_NORDIC(self):
        """Only ble.bin and nordic_bootloader_0.bin: slot 0 sends the bootloader (OK); slot 1 Not found; slot
        7 Failure; a failing bootloader transfer leaves the reported version unchanged."""
        z = gv().zipb({'firmware/ble/bin/ble.bin': img(200, 2), 'firmware/ble/bin/ble.version': b'1.2.0+0\n',
                       'firmware/ble/bin/nordic_bootloader_0.bin': img(64, 3),
                       'firmware/ble/bin/nordic_bootloader.version': b'1.2.0+0\n'})
        s = Session()
        for tag, extra in (('slot0', ['set slot 0']), ('slot1', ['set slot 1']), ('slot7', ['set slot 7']),
                           ('blfails', ['set slot 0', 'set sendbl 0'])):
            self.install(s, tag, z, extra=extra)
            s.add('copro', name=tag + '/copro')
        out = s.run()
        lg = log(out['slot0/log'])
        self.assertIn('nordic_send:bl0', lg)
        self.assertEqual(Result(out['slot0']).outcome, 'done')
        self.assertEqual(out['slot0/copro'].split()[1], '1.2.0.0')
        lg = log(out['slot1/log'])
        self.assertNotIn('nordic_send:bl0', lg)
        self.assertNotIn('nordic_send:bl1', lg)
        self.assertEqual(Result(out['slot1']).outcome, 'done')
        r = Result(out['slot7'])
        self.assertEqual((r.outcome, r.step, r.result), ('failed', 4, 1))
        r = Result(out['blfails'])
        self.assertEqual((r.outcome, r.step, r.result), ('failed', 4, 1))
        self.assertEqual(out['blfails/copro'].split()[1], '0.0.0.0')

    @scenario('S-11/SC-11-PSOC')
    def test_SC_11_PSOC(self):
        """backplane.cyacd: the PSoC at the package's version (Already installed: no programming), older
        (OK), failing (Failure; the run stops)."""
        z = fwzip.build({'backplane': img(300, 5)}, version=(1, 2, 0))
        s = Session()
        self.install(s, 'equal', z, extra=['copro 0.0.0.0 0.0.0.0 1.2.0.0'])
        self.install(s, 'older', z, extra=['copro 0.0.0.0 0.0.0.0 1.1.0.0'])
        self.install(s, 'fails', z, extra=['set psoc 0'])
        out = s.run()
        self.assertNotIn('psoc_program', log(out['equal/log']))
        self.assertEqual(Result(out['equal']).outcome, 'nothing')
        self.assertIn('psoc_program', log(out['older/log']))
        self.assertEqual(Result(out['older']).outcome, 'done')
        r = Result(out['fails'])
        self.assertEqual((r.outcome, r.step, r.result), ('failed', 5, 1))
        self.assertNotIn('progress:6,0', log(out['fails/log']))

    @scenario('S-11/SC-11-FORCED')
    def test_SC_11_FORCED(self):
        """1. Option on: a bootloader of 1.1.0, 1.0.0 or 0.9.0, unforced and forced, is written every time;
        0.9.0 unforced and TE-signed is a return to the factory bootloader. 2. Option off (TE-signed packages):
        Skipped, not written. 3. The Nordic application, Nordic bootloader and backplane at the peers' versions:
        unforced Already installed (nothing sent), forced each sent."""
        s = Session()
        for v in ((1, 1, 0), (1, 0, 0), (0, 9, 0)):
            z = fwzip.build({'bootloader': (boot_img(300, 7), v)}, version=(1, 2, 0))
            for forced in (0, 1):
                hv = (0, 1, 0, 0) if forced else (1, 2, 0, 0)
                self.install(s, 'on_%s_%d' % (vtext(v), forced), z, sign_d=None, version=hv)
                self.install(s, 'off_%s_%d' % (vtext(v), forced), z, option=0, version=hv)
            if v == (0, 9, 0):
                self.install(s, 'stock', z)
        z = full_zip(version=(1, 2, 0), content=False)
        for forced in (0, 1):
            self.install(s, 'peers_%d' % forced, z, extra=['copro 1.2.0.0 1.2.0.0 1.2.0.0'],
                         version=(0, 1, 0, 0) if forced else (1, 2, 0, 0))
        out = s.run()
        for v in ('1.1.0.0', '1.0.0.0', '0.9.0.0'):
            for forced in (0, 1):
                with self.subTest(bootloader=v, forced=forced):
                    r = Result(out['on_%s_%d' % (v, forced)])
                    self.assertEqual((r.reason, r.forced, r.outcome), (None, bool(forced), 'done'))
                    self.assertIn('bootloader_write', log(out['on_%s_%d/log' % (v, forced)]))
                    r = Result(out['off_%s_%d' % (v, forced)])
                    self.assertEqual((r.reason, r.outcome), (None, 'nothing'))
                    self.assertNotIn('bootloader_write', log(out['off_%s_%d/log' % (v, forced)]))
        r = Result(out['stock'])
        self.assertTrue(r.te and r.stock)
        self.assertIn('bootloader_write', log(out['stock/log']))
        lg = log(out['peers_0/log'])
        for e in ('nordic_send:ble', 'nordic_send:bl0', 'psoc_program'):
            self.assertNotIn(e, lg)
        lg = log(out['peers_1/log'])
        for e in ('nordic_send:ble', 'nordic_send:bl0', 'psoc_program'):
            self.assertIn(e, lg)

    @scenario('S-11/SC-11-CDC', 'S-11/SC-11-MIDI')
    def test_SC_11_CDC_MIDI(self):
        """A body-form package with a Nordic application at the version the Nordic reports: over the
        firmware command it is forced and the application is sent (OK); over the MIDI transfer it is not,
        and nothing is sent (Already installed). form_file_ok.bin (file form) over the command: refused
        MAGIC. (cdc_forced.bin and midi_not_forced.bin carry no Nordic application.)"""
        z = fwzip.build({'os': os_img(300, 1), 'ble': img(200, 2)}, version=(1, 2, 0))
        path = package('body_ble', z, pre=False)
        s = Session()
        for tag, fwcmd in (('cdc', 1), ('midi', 0)):
            s.add(*setup(), 'copro 1.2.0.0 0.0.0.0 0.0.0.0', 'load ' + path)
            s.add('install body %d 0 -' % fwcmd, name=tag)
            s.add('log', name=tag + '/log')
        for f in ('cdc_forced.bin', 'midi_not_forced.bin'):
            s.add(*setup(), 'load ' + spec_path('S-11-packages', 'vectors', 'pkg', f))
            s.add('install body %d 0 -' % (f == 'cdc_forced.bin'), name=f)
        s.add('load ' + spec_path('S-11-packages', 'vectors', 'pkg', 'form_file_ok.bin'))
        s.add('install body 1 0 -', name='file')
        out = s.run()
        r = Result(out['cdc'])
        self.assertEqual((r.reason, r.forced, r.outcome), (None, True, 'done'))
        self.assertIn('nordic_send:ble', log(out['cdc/log']))
        r = Result(out['midi'])
        self.assertEqual((r.reason, r.forced, r.outcome), (None, False, 'done'))
        self.assertNotIn('nordic_send:ble', log(out['midi/log']))
        self.assertEqual(Result(out['cdc_forced.bin']).forced, True)
        self.assertEqual(Result(out['midi_not_forced.bin']).forced, False)
        self.assertEqual(Result(out['file']).reason, 'MAGIC')


# --- SC-11-CUT, SC-11-CONTENT-CUT, SC-11-INTERRUPTED -------------------------------------------------------

class Interrupted(unittest.TestCase):

    def state(self, s, tag):
        s.add('meta', name=tag + '/meta')
        s.add('get upgrade/progress.log', name=tag + '/record')
        s.add('tree', name=tag + '/tree')

    def assert_state(self, out, tag, dev):
        st, ver = out[tag + '/meta'].split()
        self.assertEqual(int(st, 16), dev.state, tag)
        self.assertEqual(ver, vtext(dev.os_version), tag)
        rec = out[tag + '/record']
        self.assertEqual(None if rec == 'absent' else bytes.fromhex(rec), dev.record, tag)

    def assert_interrupted_boot(self, out, tag, state_after=0):
        """SC-11-INTERRUPTED's boot: one metadata write of state 0, no record write, no Nordic exchange, no
        step, the volume unchanged."""
        self.assertEqual(out[tag], 'interrupted 0x%x' % state_after, tag)
        lg = log(out[tag + '/log'])
        self.assertEqual(lg, ['meta:state=0' if state_after == 0 else 'meta_fails:state=0'], tag)
        self.assertEqual(step_effects(lg), {}, tag)

    @scenario('S-11/SC-11-CUT')
    def test_SC_11_CUT(self):
        """The SC-11-RUN-OK package cut before each change the run makes (its metadata, record and Nordic
        writes and each step's effects), then booted until the OS boots: every record written carries 0 as
        its second byte; a cut after the write of 0xAB0D and before the run's last metadata write is followed
        by SC-11-INTERRUPTED's boot, then an OS boot; any other cut by an OS boot. After the cut and after
        each boot the state, os_version and record are the model's (s11_model.install, boot), and no boot
        changes the volume."""
        m = model()
        z = full_zip(version=(1, 2, 0), content=False)
        staged = staged_of(z)
        path = package('cut', z)
        s = Session()
        s.add(*setup(), 'load ' + path, 'install file 0 1 op-xy.tfw', 'log', name='uncut')
        changes = [t for t in log(s.run()['uncut']) if not t.startswith(NOTES)]
        records_ = [t for t in changes if t.startswith('progress')]
        self.assertTrue(records_)
        self.assertEqual([t for t in records_ if not t.endswith(',0')], [])           # S-11.062
        metas = [i for i, t in enumerate(changes) if t.startswith('meta')]
        first, last = metas[0], metas[-1]
        s = Session()
        plan = []
        for i in range(len(changes) + 1):
            k = len(writes(changes[:i]))         # the model's writes made before the cut
            dev = m.Device(None, (1, 0, 0, 0))
            cut = i < len(changes)
            if cut:
                self.assertIsNone(m.install(dev, staged, option=True, cut=k, **ALL_ON))
            else:
                self.assertIsNotNone(m.install(dev, staged, option=True, **ALL_ON))
            s.add(*setup(), 'load ' + path, 'set cut %d' % i if cut else 'set cut -')
            s.add('install file 0 1 op-xy.tfw', name=(i, 'cut'))
            s.add('set cut -', 'reboot')
            self.state(s, '%d/0' % i)
            boots = [(0, None, copy.deepcopy(dev))]
            for b in range(1, 4):
                end = m.boot(dev)
                s.add(BOOT, name='%d/%d' % (i, b))
                s.add('log', name='%d/%d/log' % (i, b))
                s.add('reboot')
                self.state(s, '%d/%d' % (i, b))
                boots.append((b, end, copy.deepcopy(dev)))
                if end == 'os':
                    break
            plan.append((i, cut, boots))
        out = s.run()
        for i, cut, boots in plan:
            with self.subTest(cut_before=changes[i] if cut else '-'):
                self.assertEqual(Result(out[(i, 'cut')]).end, 'cut' if cut else 'done')
                ends = [end for _, end, _ in boots[1:]]
                self.assertEqual(ends, ['interrupted', 'os'] if first < i <= last else ['os'])
                for b, end, after in boots:
                    tag = '%d/%d' % (i, b)
                    self.assert_state(out, tag, after)
                    if b == 0:
                        continue
                    self.assertEqual(tree(out[tag + '/tree']), tree(out['%d/0/tree' % i]), tag)
                    if end == 'interrupted':
                        self.assert_interrupted_boot(out, tag)
                    else:
                        self.assertEqual(out[tag], 'os 0x0', tag)
                        self.assertEqual(log(out[tag + '/log']), [], tag)

    @scenario('S-11/SC-11-CONTENT-CUT')
    def test_SC_11_CONTENT_CUT(self):
        """Packs A and B installed; a full (unmarked) package with content, cut before each volume change of
        its content step and before the record that follows it; the next boot is SC-11-INTERRUPTED's (no
        step), the one after it boots the OS: every file of packs A and B is in content/dlc/ with its
        contents, and none of them is anywhere else."""
        packs = {'content/dlc/A/a1': b'1', 'content/dlc/A/manifest.json': b'{"name":"Pack A"}',
                 'content/dlc/B/b1': b'b', 'content/dlc/B/sub/b2': b'bb', 'content/op1.json': b'old',
                 'content/wavetables/a.mip': b'w'}
        z = fwzip.build({'os': os_img(300, 1)}, version=(1, 2, 0),
                        extra={'firmware/blackfin/content/op1.json': b'new', 'firmware/blackfin/content/x/y': b'y'})
        path = package('content_cut', z)
        s = Session()
        s.add(*setup(tree_files=packs), 'load ' + path, 'install file 0 0 op-xy.tfw', 'log', name='uncut')
        changes = [t for t in log(s.run()['uncut']) if not t.startswith(NOTES)]
        cuts = [i for i, t in enumerate(changes) if t.startswith('fs_')]
        self.assertTrue(cuts)
        cuts.append(cuts[-1] + 1)
        s = Session()
        for i in cuts:
            s.add(*setup(tree_files=packs), 'load ' + path, 'set cut %d' % i)
            s.add('install file 0 0 op-xy.tfw', name=(i, 'cut'))
            s.add('set cut -', 'reboot', BOOT, name='%d/boot' % i)
            s.add('log', name='%d/boot/log' % i)
            s.add('reboot', BOOT, name=(i, 'next'))
            s.add('tree', name=(i, 'tree'))
        out = s.run()
        dlc = {p: b for p, b in packs.items() if p.startswith('content/dlc/')}
        want = expect_tree(dlc)
        for i in cuts:
            with self.subTest(cut_before=changes[i]):
                self.assertEqual(Result(out[(i, 'cut')]).end, 'cut')
                self.assert_interrupted_boot(out, '%d/boot' % i)
                self.assertEqual(out[(i, 'next')], 'os 0x0')
                t = tree(out[(i, 'tree')])
                self.assertEqual({p: v for p, v in t.items() if p.startswith('content/dlc/')},
                                 {p: v for p, v in want.items() if p.startswith('content/dlc/')})
                for p in dlc:
                    rel = p[len('content/dlc/'):]
                    self.assertEqual([q for q in files(t) if q.endswith('/' + rel) and q != p], [], p)

    @requirement('S-16.041')
    @scenario('S-11/SC-11-INTERRUPTED')
    def test_SC_11_INTERRUPTED_cut_os(self):
        """1. The SC-11-RUN-OK install cut during the OS step (state 0xAB0D, record 01 00); a boot with the
        keys not held: SC-11-INTERRUPTED's boot, os_version kept, the record and the staging area unchanged;
        then a boot with reason 20 boots the OS and writes nothing."""
        s = Session()
        s.add(*setup(), 'load ' + package('interrupted_os', full_zip(version=(1, 2, 0))), 'set cutat os_write')
        s.add('install file 0 1 op-xy.tfw', name='cut')
        s.add('set cutat -', 'reboot')
        self.state(s, 'before')
        s.add(BOOT, name='boot')
        s.add('log', name='boot/log')
        self.state(s, 'boot')
        s.add('reboot', 'boot 20 0 1', name='next')
        s.add('log', name='next/log')
        out = s.run()
        self.assertEqual(Result(out['cut']).end, 'cut')
        self.assertEqual((out['before/meta'], out['before/record']), ('0xab0d ' + OLD, '0100'))
        self.assert_interrupted_boot(out, 'boot')
        self.assertEqual((out['boot/meta'], out['boot/record']), ('0x0 ' + OLD, '0100'))
        self.assertEqual(tree(out['boot/tree']), tree(out['before/tree']))
        self.assertEqual(out['next'], 'os 0x0')
        self.assertEqual(log(out['next/log']), [])

    @scenario('S-11/SC-11-INTERRUPTED')
    def test_SC_11_INTERRUPTED_table(self):
        """2. interrupted.tsv, every row, against s11_model.interrupted_boot: the keys held, a silent PSoC
        (counted as held), the slave absent, the reboot reasons 0, 10, 30, 40 and 60, and (3.) the metadata
        write failing: with the state 0xAB0D the same single write of state 0 every time, and no step, Nordic
        exchange, record write or volume change; a staging area with record 03 00 unchanged afterwards."""
        m = model()
        rows = tsv('S-11-packages', 'interrupted.tsv')
        s, plan = Session(), []
        for r in rows:
            state = int(r['state'], 16)
            keys = (0,) if r['keys'] == 'not_held' else (1, 2)
            for reason in (0, 10, 30, 40, 60) if state == m.UPGRADING else (0,):
                for k in keys:
                    tag = '%s/%s/%s/%s/%d/%d' % (r['state'], r['keys'], r['slave'], r['meta_write_ok'], reason, k)
                    s.add(*setup(meta='%s %s' % (r['state'], OLD)))
                    s.add(put('upgrade/firmware/blackfin/bin/os.ldr', b'o'), put('upgrade/progress.log', b'\x03\x00'))
                    if r['meta_write_ok'] == '0':
                        s.add('set metafail 1')
                    s.add('tree', name=tag + '/tree0')
                    s.add('boot %d %d %d' % (reason, k, r['slave'] == 'present'), name=tag)
                    s.add('log', name=tag + '/log')
                    self.state(s, tag)
                    plan.append((r, reason, tag))
        out = s.run()
        for r, reason, tag in plan:
            with self.subTest(case=tag):
                ok = r['meta_write_ok'] != '0'
                want = m.interrupted_boot(int(r['state'], 16), r['keys'] == 'held', r['slave'] == 'present', ok)
                self.assertEqual(want['decision'], r['decision'])
                self.assertEqual(want['writes'], json.loads(unquote(r['metadata_writes'])))
                self.assertEqual(want['state_after'], int(r['state_after'], 16))
                self.assertEqual((int(want['notice']), want['steps'], want['nordic_dfu'], want['record_access']),
                                 (int(r['notice']), int(r['steps']), int(r['nordic_dfu']), int(r['record_access'])))
                decision, st = out[tag].split()
                self.assertEqual(decision, 'interrupted' if want['notice'] else want['decision'])
                self.assertEqual(int(st, 16), want['state_after'])
                lg = log(out[tag + '/log'])
                self.assertEqual([t.split(':', 1)[1] for t in lg if t.split(':')[0] in ('meta', 'meta_fails')],
                                 want['writes'])
                self.assertEqual(len(step_effects(lg)), want['steps'])
                self.assertEqual(len([t for t in lg if t.startswith('nordic')]), want['nordic_dfu'])
                self.assertEqual(len([t for t in lg if t.startswith('progress')]), want['record_access'])
                self.assertEqual(out[tag + '/record'], '0300')
                self.assertEqual(tree(out[tag + '/tree']), tree(out[tag + '/tree0']))
                self.assertEqual(out[tag + '/meta'].split()[1], OLD)

    @scenario('S-11/SC-11-INTERRUPTED')
    def test_SC_11_INTERRUPTED_meta_fails(self):
        """3. The metadata write failing: the boot is SC-11-INTERRUPTED's all the same and the state stays
        0xAB0D; the next boot does the same again; with the write working again, the next boot clears the
        state and the one after it boots the OS. As s11_model.boot over the same boots."""
        m = model()
        dev = m.Device(None, (1, 0, 0, 0))
        dev.state = m.UPGRADING
        ends = [m.boot(dev, meta_write_ok=False), m.boot(dev, meta_write_ok=False), m.boot(dev), m.boot(dev)]
        self.assertEqual(ends, ['interrupted', 'interrupted', 'interrupted', 'os'])
        s = Session()
        s.add(*setup(meta='0xAB0D ' + OLD), 'set metafail 1')
        for b in (1, 2):
            s.add('reboot', BOOT, name='fail%d' % b)
            s.add('log', name='fail%d/log' % b)
        s.add('set metafail 0', 'reboot', BOOT, name='ok')
        s.add('log', name='ok/log')
        s.add('reboot', BOOT, name='next')
        s.add('log', name='next/log')
        s.add('meta', name='meta')
        out = s.run()
        for b in (1, 2):
            self.assert_interrupted_boot(out, 'fail%d' % b, state_after=m.UPGRADING)
        self.assert_interrupted_boot(out, 'ok')
        self.assertEqual(out['next'], 'os 0x0')
        self.assertEqual(log(out['next/log']), [])
        self.assertEqual(out['meta'], '0x0 ' + OLD)

    @scenario('S-11/SC-11-INTERRUPTED')
    def test_SC_11_INTERRUPTED_te_state(self):
        """4. A state 0xAB0D left by another bootloader's install, with a staging area and record 03 01: as 1,
        and the record and the staging area unchanged afterwards."""
        s = Session()
        s.add(*setup(meta='0xAB0D ' + OLD))
        s.add(put('upgrade/firmware/blackfin/bin/os.ldr', b'os'),
              put('upgrade/firmware/firmware.version', b'1.4.0+0\n'),
              put('upgrade/firmware/blackfin/content/op1.json', b'{}'), put('upgrade/progress.log', b'\x03\x01'))
        self.state(s, 'before')
        s.add(BOOT, name='boot')
        s.add('log', name='boot/log')
        self.state(s, 'boot')
        out = s.run()
        self.assert_interrupted_boot(out, 'boot')
        self.assertEqual((out['boot/meta'], out['boot/record']), ('0x0 ' + OLD, '0301'))
        self.assertEqual(tree(out['boot/tree']), tree(out['before/tree']))

    @scenario('S-11/SC-11-INTERRUPTED')
    def test_SC_11_INTERRUPTED_content_only(self):
        """5. A content-only install cut after its content step's first move: no state was written, so the next
        boot boots the OS and writes nothing; each staged file is either installed under content/ or still
        staged, not both, and some of each. As s11_model.install with the cut, then s11_model.boot."""
        m = model()
        fs = {m.DLC_DIR + 'A/a%d' % n: bytes([n]) * (n + 1) for n in range(1, 5)}
        fs[m.DLC_DIR + 'A/manifest.json'] = b'{"name":"Pack A"}'
        z = zip_of(fs)
        staged = staged_of(z)
        self.assertTrue(m.content_only(staged))
        path = package('interrupted_content', z)
        s = Session()
        s.add(*setup(), 'load ' + path, 'install file 0 0 op-xy.tfw', 'log', name='uncut')
        changes = [t for t in log(s.run()['uncut']) if not t.startswith(NOTES)]
        moves = [i for i, t in enumerate(changes) if t == 'fs_move']
        self.assertGreater(len(moves), 2)
        i = moves[1]
        dev = m.Device(None, (1, 0, 0, 0))
        self.assertIsNone(m.install(dev, staged, cut=len(writes(changes[:i])), **ALL_ON))
        self.assertEqual(m.boot(dev), 'os')
        s = Session()
        s.add(*setup(), 'load ' + path, 'set cut %d' % i)
        s.add('install file 0 0 op-xy.tfw', name='cut')
        s.add('set cut -', 'reboot', 'meta', name='cut/meta')
        s.add(BOOT, name='boot')
        s.add('log', name='boot/log')
        s.add('tree', name='tree')
        out = s.run()
        self.assertEqual(Result(out['cut']).end, 'cut')
        self.assertEqual(out['cut/meta'], '0x0 ' + OLD)
        self.assertEqual(out['boot'], 'os 0x0')
        self.assertEqual(log(out['boot/log']), [])
        t = files(tree(out['tree']))
        where = []
        for p, b in fs.items():
            rel = p[len(m.CONTENT_DIR):]
            want = (len(b), zlib.crc32(b))
            at = [q for q in ('content/' + rel, 'upgrade/' + p) if t.get(q) == want]
            self.assertEqual(len(at), 1, (p, at))
            where.append(at[0].startswith('content/'))
        self.assertEqual(sum(where), 1)


# --- SC-11-DECOY, the disk search ------------------------------------------------------------------------

DISK_SECTORS = 454656                       # S-12.001


class Disk(unittest.TestCase):

    @requirement('S-16.148')
    @scenario('S-11/SC-11-DECOY')
    def test_SC_11_DECOY(self):
        """The four disks of packages.json disk_cases on a FAT RAM disk: the file s11_disk_pick() takes, the
        reason, and the os.ldr staged is the package's, never the decoy's."""
        cases = vectors('packages.json')['disk_cases']
        s = Session()
        for c in cases:
            s.add(*setup(), 'disk %d' % DISK_SECTORS)
            for name, f in c['entries']:
                s.add('diskput %s %s' % (name, spec_path('S-11-packages', 'vectors', f)))
            s.add('diskinstall 0', name=c['case'])
            s.add('sha upgrade/firmware/blackfin/bin/os.ldr', name=c['case'] + '/os')
        out = s.run()
        for c in cases:
            e = c['expect']
            with self.subTest(case=c['case']):
                f = out[c['case']].split()
                self.assertEqual(None if f[0] == '-' else f[0], e['package'])
                if e['package'] is None:
                    self.assertEqual(out[c['case'] + '/os'], 'absent')
                    continue
                self.assertEqual(Result(' '.join(f[1:])).reason, e['reason'])
                os_sha = out[c['case'] + '/os']
                self.assertNotEqual(os_sha, e['decoy_os_sha256'])
                self.assertEqual(None if os_sha == 'absent' else os_sha, e['os_sha256'])

    @scenario('S-11/SC-11-DECOY')
    def test_S11_046_disk_search(self):
        """disk_search.tsv: the root in directory order -> the file taken, and forced by its name."""
        rows = tsv('S-11-packages', 'disk_search.tsv')
        pkg = spec_path('S-11-packages', 'vectors', 'pkg', 'form_file_ok.bin')
        decoy = spec_path('S-11-packages', 'vectors', 'pkg', 'decoy_firmware.zip')
        other = write('other.bin', b'x' * 10)
        s = Session()
        for r in rows:
            s.add(*setup(), 'disk %d' % DISK_SECTORS)
            for name, is_dir in json.loads(unquote(r['entries_in_directory_order'])):
                if is_dir:
                    s.add('diskmkdir ' + name)
                else:
                    s.add('diskput %s %s' % (name, pkg if 'tfw' in name else decoy if name.endswith('.zip') else other))
            s.add('diskinstall 0', name=r['case'])
        out = s.run()
        for r in rows:
            with self.subTest(case=r['case']):
                f = out[r['case']].split()
                self.assertEqual('' if f[0] == '-' else f[0], r['package'])
                if r['package']:
                    self.assertEqual(Result(' '.join(f[1:])).forced, r['forced'] == '1')


# --- The master's way: the same installs staged over S-09 ------------------------------------------------

class _OverS09:
    """The class's tests with h_s11 in its S-09 mode (WB_S11_VIA=s09): checks 16-17's staging and free space,
    the run's and the content step's view of the volume and the progress record go through the master's file
    client (s09_files) and S-09's file commands to wire_s09 on the same volume, as on the master, which holds no
    yaffs2 of its own."""

    @classmethod
    def setUpClass(cls):
        os.environ['WB_S11_VIA'] = 's09'
        super().setUpClass()

    @classmethod
    def tearDownClass(cls):
        os.environ.pop('WB_S11_VIA', None)
        super().tearDownClass()


class RefusalsOverS09(_OverS09, Refusals):
    pass


class AcceptanceOverS09(_OverS09, Acceptance):
    pass


class StagingOverS09(_OverS09, Staging):
    pass


class ContentOverS09(_OverS09, Content):
    pass


class RunsOverS09(_OverS09, Runs):
    pass


class InterruptedOverS09(_OverS09, Interrupted):
    pass


class DiskOverS09(_OverS09, Disk):
    pass


if __name__ == '__main__':
    unittest.main()
