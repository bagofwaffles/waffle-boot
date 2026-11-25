# SPDX-License-Identifier: GPL-2.0-or-later
"""S-12 filesystems: the RAM disk on FatFs (src/core/fs/fat.c) and the yaffs2 volume (src/core/fs/yaffs.c),
through h_s12 on the images of tests/oracle/S-12-filesystems, read back with conformance/fs."""

import hashlib
import json
import os
import struct
import subprocess
import sys
import unittest
from datetime import datetime, timezone

from harness import BUILD, expected_failure, requirement, run, scenario, spec_module, spec_path
from conformance.fs import fat, fatread, yaffs2, yaffs2read
from conformance.fs.disk import assert_unmodified

WORK = os.path.join(BUILD, 's12')
IMAGES = os.path.join(WORK, 'img')
DISK_SECTORS = 454656                       # S-12.001
BLOCKS = 8                                  # the vectors' yaffs2 volumes
RESERVE = 2                                 # below S-12.056's 5 on 8 blocks (as tests/unit/test_wire_yaffs.c)
P = yaffs2.op_xy_params(n_blocks=BLOCKS)
FIXED = (fat.fat_date(1980, 1, 1), 0)       # S-12.015

_DONE = []
_S11 = []


def images():
    """gen_vectors.py --images, once per run, in its own process: it imports the conformance package under
    another name."""
    if not _DONE:
        subprocess.run([sys.executable, '-I', '-B', spec_path('S-12-filesystems', 'gen_vectors.py'), '--images',
                        IMAGES], check=True)
        _DONE.append(True)
    return IMAGES


def image(name):
    return os.path.join(images(), name)


def vec(name):
    with open(spec_path('S-12-filesystems', 'vectors', name + '.json'), encoding='utf-8') as f:
        return json.load(f)


def s11_model():
    """S-11's model (disk_search). Its generator sets up the import path the model needs; the path is put
    back once it is loaded."""
    if not _S11:
        saved = list(sys.path)
        try:
            _S11.append(spec_module('S-11-packages', 'gen_vectors').m)
        finally:
            sys.path[:] = saved
    return _S11[0]


def out(name):
    os.makedirs(WORK, exist_ok=True)
    return os.path.join(WORK, name)


def h(s):
    """A name or path in h_s12's notation: hex UTF-8, '-' for the root."""
    return s.encode().hex() or '-'


def unh(s):
    return '' if s == '-' else bytes.fromhex(s).decode()


def pattern(n, seed=1):
    return bytes((i * 7 + seed) % 251 for i in range(n))


def digest(b):
    return {'len': len(b), 'sha256': hashlib.sha256(bytes(b)).hexdigest()}


def utc(*t):
    return int(datetime(*t, tzinfo=timezone.utc).timestamp())


class Session:
    """Commands for one h_s12 run; named results."""

    def __init__(self):
        self.cmds, self.names = [], {}

    def add(self, *cmds, name=None):
        self.cmds.extend(cmds)
        if name is not None:
            self.names[name] = len(self.cmds) - 1

    def run(self):
        lines = run('s12', self.cmds)
        return {k: lines[i] for k, i in self.names.items()}


def zero_file(path):
    with open(path, 'rb') as f:
        while True:
            x = f.read(1 << 20)
            if not x:
                return True
            if x.count(0) != len(x):
                return False


# --- the yaffs2 listings -------------------------------------------------------------------------------------

def listing(line):
    """ytree's output as {path: entry}: dir, file (len, sha256) or symlink (alias)."""
    tree = {}
    if line == '-':
        return tree
    for tok in line.split():
        f = tok.split(':')
        path = unh(f[0])
        if f[1] == 'd':
            tree[path] = {'type': 'dir'}
        elif f[1] == 'f':
            tree[path] = {'type': 'file', 'len': int(f[2]), 'sha256': f[3]}
        elif f[1] == 'l':
            tree[path] = {'type': 'symlink', 'alias': unh(f[2] or '-')}
        else:
            tree[path] = {'type': '?'}
    return tree


def expected_listing(case_tree):
    """A case's tree as the implementation lists it: a hard link reads as the file it links to."""
    want = {}
    for path, e in case_tree.items():
        if e['type'] == 'hardlink':
            want[path] = dict(case_tree[e['target']])
        else:
            want[path] = dict(e)
    return want


def split_lost(tree):
    """(entries outside lost+found, sorted digests of the files beneath it) (S-12.050 rule 8)."""
    outside = {p: e for p, e in tree.items() if not p.startswith('lost+found/')}
    lost = sorted(({'len': e['len'], 'sha256': e['sha256']} for p, e in tree.items()
                   if p.startswith('lost+found/') and e['type'] == 'file'), key=lambda d: (d['sha256'], d['len']))
    return outside, lost


def reader_tree(r):
    """Yaffs2Reader's tree with file contents as digests (gen_vectors.tree_json, outside lost+found)."""
    out = {}
    for path, e in sorted(r.tree().items()):
        if path.startswith('lost+found/'):
            continue
        if e['type'] == 'file':
            out[path] = {'type': 'file', **digest(e['data'])}
        elif e['type'] == 'symlink':
            out[path] = {'type': 'symlink', 'alias': e['alias']}
        elif e['type'] == 'hardlink':
            out[path] = {'type': 'hardlink', 'target': e['target']}
        else:
            out[path] = {'type': e['type']}
    return out


def ylog(line):
    """ylog's output as [(kind, index)]: ('w', chunk) or ('e', block)."""
    return [] if line == '-' else [(t[0], int(t[1:])) for t in line.split()]


def checkpoint_chunks_written(before, after, p):
    """S-12.055: the chunks of `after` that differ from `before` and whose tags carry the checkpoint
    sequence number 0x21."""
    a = yaffs2read.Yaffs2Reader(after, p)
    b = yaffs2read.Yaffs2Reader(before, p)
    found = []
    for idx in range(p.n_blocks * p.chunks_per_block):
        c = a.read_chunk(idx)
        if c == b.read_chunk(idx):
            continue
        t = yaffs2.unpack_tags(c[-16:], p.erased_word)
        if t and t['seq'] == yaffs2.SEQ_CHECKPOINT:
            found.append(idx)
    return found


def cases():
    return {c['name']: c for c in vec('yaffs_trees')['cases']}


# --- the RAM disk ----------------------------------------------------------------------------------------------

def sector(path, n):
    with open(path, 'rb') as f:
        f.seek(n * 512)
        return f.read(512)


def raw_entry(r, path):
    """The 32-byte short entry of `path` on the volume FatReader r reads."""
    head, _, name = path.rpartition('/')
    cluster = 0
    if head:
        cluster = next(e['cluster'] for p, e in r.walk() if p == head)
    e = next(e for e in r.listdir(cluster) if e['name'] == name)
    return r._dir_bytes(cluster)[e['pos']:e['pos'] + 32]


class Format(unittest.TestCase):

    @requirement('S-16.081')
    @scenario('S-12/SC-12-FORMAT')
    def test_SC_12_FORMAT(self):
        """wire_fat_format() of a RAM disk filled with 0xA5: sectors 0, 63 and 64-206 are the fresh image's,
        FatReader finds a consistent, empty FAT16 volume labeled OP-XY (code page 850, long names)."""
        f = out('format.img')
        o = Session()
        o.add('disk %d 0xA5' % DISK_SECTORS, 'diskformat', name='format')
        o.add('diskls', name='ls')
        o.add('disksave ' + f)
        r = o.run()
        self.assertEqual(r['format'], '1')
        self.assertEqual(r['ls'], '-')
        fresh = image('ramdisk_fresh.img')
        for s in [0, 63] + list(range(64, 207)):
            self.assertEqual(sector(f, s), sector(fresh, s), 'sector %d' % s)
        rd = fatread.FatReader(f)
        self.assertEqual((rd.fat_type, rd.check(), rd.tree(), rd.label), (16, [], {}, 'OP-XY'))
        os.remove(f)

    @scenario('S-12/SC-12-SCAN-NO-PACKAGE')
    def test_SC_12_SCAN_NO_PACKAGE(self):
        """A disk holding notes.txt and a directory cdc/: neither scan takes anything, and the scans write no
        sector."""
        o = Session()
        o.add('disk %d 0' % DISK_SECTORS, 'diskformat',
              'diskwrite %s %s 0 0' % (h('notes.txt'), b'some notes\n'.hex()), 'diskmkdir ' + h('cdc'))
        o.add('disksha', name='before')
        o.add('diskwrites')
        o.add('diskpick', name='pick')
        o.add('diskcdc', name='cdc')
        o.add('diskwrites', name='writes')
        o.add('disksha', name='after')
        r = o.run()
        self.assertEqual((r['pick'], r['cdc'], r['writes']), ('-', '0', '0'))
        self.assertEqual(r['after'], r['before'])


def cdc_rule(entries):
    """S-04.024 restated: candidates are regular root files whose name does not begin with '.'."""
    cand = [n for n, d in entries if not d and not n.startswith('.')]
    return not any('tfw' in n for n in cand) and any('cdc' in n and 'please' in n for n in cand)


class Names(unittest.TestCase):

    def roots(self):
        """The display entries of names.json in the root of a RAM-disk volume, and a variant without the
        lowercase 'tfw' names, with a regular file cdc_please."""
        ents = [bytes.fromhex(x) for x in vec('names')['display']['entries']]
        lfn = fat.lfn_entries('op-xy firmware.tfw', b'OP-XYF~1TFW')
        drop = set(lfn) | {fat.dirent(b'OP-XYF~1TFW', fat.ATTR_ARC, 0, 0),
                           fat.dirent(b'OPXY    TFW', fat.ATTR_ARC, 0, 0, nt=0x10)}
        cdc = fat.lfn_entries('cdc_please', b'CDC_PL~2   ') + [fat.dirent(b'CDC_PL~2   ', fat.ATTR_ARC, 0, 0)]
        return {'display': ents, 'no_tfw': [e for e in ents if e not in drop] + cdc}

    @scenario('S-12/SC-12-NAMES')
    def test_SC_12_NAMES(self):
        """The root as wire_fat_root() lists it holds the names S-12.011 gives (FatReader's), and the package
        and CDC decisions are S-11.046's and S-04.024's for those names."""
        o, kinds = Session(), {}
        for k, ents in self.roots().items():
            v = fat.FatVolume(fat.ramdisk_params())
            v.add_label('OP-XY')
            for e in ents:
                v.add_raw_entry('', e)
            path = out('names_%s.img' % k)
            v.build().save(path)
            kinds[k] = path
            o.add('diskload ' + path, 'diskmount', name=k + '/mount')
            o.add('diskls', name=k + '/ls')
            o.add('diskpick', name=k + '/pick')
            o.add('diskcdc', name=k + '/cdc')
        r = o.run()
        for k, path in kinds.items():
            with self.subTest(root=k):
                rd = fatread.FatReader(path)
                want = [(e['name'], bool(e['attr'] & fat.ATTR_DIR)) for e in rd.listdir(0)]
                got = [(unh(t.rsplit(':', 1)[0]), t.endswith(':d')) for t in r[k + '/ls'].split()]
                self.assertEqual(r[k + '/mount'], '1')
                self.assertEqual(got, want)
                if k == 'display':
                    self.assertEqual([n for n, _ in got], vec('names')['display']['names'])
                pick = s11_model().disk_search(got)
                self.assertEqual(None if r[k + '/pick'] == '-' else unh(r[k + '/pick']), pick)
                self.assertEqual(r[k + '/cdc'] == '1', cdc_rule(got))
            os.remove(path)

    @requirement('S-12.012', 'S-16.081', 'S-16.162')
    def test_S12_012_create(self):
        """names.json create: wire_fat_name_ok() refuses exactly the refused names; each other name is
        created as itself, in one of the forms the vector allows."""
        o = Session()
        rows = vec('names')['create']
        for c in rows:
            o.add('nameok ' + h(c['name']), name='ok/' + c['name'])
        o.add('disk %d 0' % DISK_SECTORS, 'diskformat')
        for k, c in enumerate(rows):                       # one directory each: FAT names ignore case
            if c['forms'] != ['refused']:
                o.add('diskmkdir ' + h('d%02d' % k))
                o.add('diskwrite %s 78 0 0' % h('d%02d/%s' % (k, c['name'])), name='w/' + c['name'])
        f = out('create.img')
        o.add('disksave ' + f)
        r = o.run()
        rd = fatread.FatReader(f)
        dirs = {e['name']: e['cluster'] for e in rd.listdir(0)}
        for k, c in enumerate(rows):
            with self.subTest(name=c['name']):
                refused = c['forms'] == ['refused']
                self.assertEqual(r['ok/' + c['name']], '0' if refused else '1')
                if refused:
                    continue
                self.assertEqual(r['w/' + c['name']], '1')
                ents = [e for e in rd.listdir(dirs['d%02d' % k]) if not e['dot']]
                self.assertEqual([e['name'] for e in ents], [c['name']])
                e = ents[0]
                form = 'lfn' if e['lfn'] else ('sfn_nt' if e['nt'] & 0x18 else 'sfn')
                self.assertIn(form, c['forms'])
        self.assertEqual(rd.check(), [])
        os.remove(f)

    @requirement('S-12.015', 'S-12.016')
    def test_S12_016_stamps(self):
        """stamps.json: wire_fat_stamp() of each time; a year after 2107 is unspecified."""
        o = Session()
        rows = vec('stamps')['cases']
        for c in rows:
            o.add('stamp %d' % c['epoch'], name=c['utc'])
        r = o.run()
        for c in rows:
            with self.subTest(utc=c['utc']):
                if c['date'] is None:
                    continue
                self.assertEqual(r[c['utc']], '%d %d' % (c['date'], c['time']))


class Host(unittest.TestCase):

    @scenario('S-12/SC-12-HOST-FAT32')
    def test_SC_12_HOST_FAT32(self):
        """Every readable host_*.img: (a) the scan sees op-xy_1.1.2.tfw and cdc_please and takes the
        package; (b) after a write, FatReader's check() is empty (both FATs equal, FSInfo)."""
        rows = vec('host_volumes')['read']
        o = Session()
        for row in rows:
            k = row['kind']
            o.add('diskload ' + image('host_%s.img' % k), 'diskmount', name=k + '/mount')
            o.add('diskls', name=k + '/ls')
            o.add('diskpick', name=k + '/pick')
            o.add('diskcdc', name=k + '/cdc')
            o.add('diskmkdir ' + h('systeminfo'), name=k + '/mkdir')
            o.add('diskwrite %s %s 1 %d' % (h('systeminfo/version.txt'), b'v\n'.hex(), utc(2026, 10, 26)),
                  name=k + '/write')
            o.add('disksave ' + out('host_%s.img' % k))
        r = o.run()
        for row in rows:
            k = row['kind']
            with self.subTest(kind=k):
                ls = [(unh(t.rsplit(':', 1)[0]), t.endswith(':d')) for t in r[k + '/ls'].split()]
                self.assertEqual(r[k + '/mount'], '1')
                self.assertIn(('op-xy_1.1.2.tfw', False), ls)
                self.assertIn(('cdc_please', False), ls)
                self.assertEqual(unh(r[k + '/pick']), 'op-xy_1.1.2.tfw')
                self.assertEqual(r[k + '/cdc'], '0')                       # a tfw name stops the CDC reset
                self.assertEqual((r[k + '/mkdir'], r[k + '/write']), ('1', '1'))
                f = out('host_%s.img' % k)
                rd = fatread.FatReader(f, volume_start=row['volume_start'])
                self.assertEqual(rd.check(), [])
                t = rd.tree()
                self.assertEqual(t['systeminfo/version.txt']['data'], b'v\n')
                for path, e in row['tree'].items():
                    if e['type'] == 'file':
                        self.assertEqual(digest(t[path]['data']), {k2: e[k2] for k2 in ('len', 'sha256')})
                os.remove(f)

    @scenario('S-12/SC-12-HOST-OTHER')
    def test_SC_12_HOST_OTHER(self):
        """Every unreadable host_*.img: no package is taken, no CDC reset, and the disk is unchanged."""
        rows = vec('host_volumes')['unreadable']
        o = Session()
        for row in rows:
            k = row['kind']
            o.add('diskload ' + image('host_%s.img' % k), name=k + '/load')
            o.add('disksha', name=k + '/before')
            o.add('diskmount', name=k + '/mount')
            o.add('diskpick', name=k + '/pick')
            o.add('diskcdc', name=k + '/cdc')
            o.add('diskwrites', name=k + '/writes')
            o.add('disksha', name=k + '/after')
        r = o.run()
        for row in rows:
            k = row['kind']
            with self.subTest(kind=k):
                self.assertEqual(r[k + '/load'], 'ok')
                self.assertEqual((r[k + '/pick'], r[k + '/cdc'], r[k + '/writes']), ('-', '0', '0'))
                self.assertEqual(r[k + '/after'], r[k + '/before'])


class SystemInfo(unittest.TestCase):
    """The writes SC-12-SYSINFO observes, made through wire_fat_mkdir() and wire_fat_write_file() with the
    vector's contents (src/ has no writer of systeminfo/ yet), and the log export (s14_export_run over
    wire_fat_export_ops) from a yaffs2 volume holding logs/boot.log."""

    def volume(self, o, extra=()):
        si = vec('ramdisk_populated')['inputs']
        lg = si['logs'][0]
        o.add('yvol %d %d' % (BLOCKS, RESERVE), 'ymount', 'ymkdir ' + h('logs'))
        for name, n, t in list(extra):
            o.add('clock %d' % utc(*t), 'ypat %s %d 2' % (h('logs/' + name), n))
        o.add('clock %d' % utc(*lg['mtime_utc']), 'ypat %s %d 1' % (h('logs/' + lg['name']), lg['pattern']))

    def write(self, o, tag):
        pop = vec('ramdisk_populated')
        rtc = utc(*pop['inputs']['rtc_utc'])
        o.add('diskmkdir ' + h('systeminfo'), name=tag + '/mkdir')
        for path in ('systeminfo/version.txt', 'systeminfo/serial.txt'):
            o.add('diskwrite %s %s 1 %d' % (h(path), pop['files'][path].encode().hex(), rtc), name=tag + path)
        o.add('diskwrite %s %s 1 %d' % (h('systeminfo/licenses.txt'), pop['inputs']['licenses'].encode().hex(),
                                        rtc), name=tag + '/licenses')
        o.add('export', name=tag + '/export')

    def check_disk(self, f):
        pop = vec('ramdisk_populated')
        rd = fatread.FatReader(f)
        self.assertEqual(rd.check(), [])
        t = rd.tree()
        got = {p: ({'type': 'dir'} if e['type'] == 'dir' else {'type': 'file', **digest(e['data'])})
               for p, e in t.items()}
        want = dict(pop['tree'])
        got.pop('systeminfo/licenses.txt')                                   # its content is free
        want.pop('systeminfo/licenses.txt')
        self.assertEqual(got, want)
        for path in ('systeminfo/version.txt', 'systeminfo/serial.txt'):
            self.assertEqual(t[path]['data'].decode(), pop['files'][path])
        for e in pop['entries']:
            raw = raw_entry(rd, e['path'])
            wrt_time, wrt_date = struct.unpack_from('<HH', raw, 22)
            crt = struct.unpack_from('<HH', raw, 14)
            self.assertEqual((raw[11], raw[13], struct.unpack_from('<H', raw, 18)[0]),
                             (e['attr'], e['crt_tenths'], e['acc_date']), e['path'])
            self.assertEqual((wrt_time, wrt_date), (e['wrt_time'], e['wrt_date']), e['path'])
            if e['attr'] & fat.ATTR_DIR:
                self.assertIn(crt, [(0, 0), (FIXED[1], FIXED[0])], e['path'])
            else:
                self.assertEqual(crt, (e['crt_time'], e['crt_date']), e['path'])
        return t

    @scenario('S-12/SC-12-SYSINFO')
    def test_SC_12_SYSINFO(self):
        """The tree, contents, entries (attributes, the fixed clock, the RTC and log stamps) and check() of
        the vector; variant existing: a second run leaves the contents as the first wrote them."""
        o = Session()
        self.volume(o)
        o.add('disk %d 0' % DISK_SECTORS, 'diskformat')
        self.write(o, 'first')
        f1, f2 = out('sysinfo1.img'), out('sysinfo2.img')
        o.add('disksave ' + f1)
        self.write(o, 'second')
        o.add('disksave ' + f2)
        r = o.run()
        for tag in ('first', 'second'):
            self.assertEqual(r[tag + '/mkdir'], '1')
            self.assertEqual(r[tag + '/export'], '1')
        t1 = self.check_disk(f1)
        t2 = self.check_disk(f2)
        self.assertEqual(t2, t1)
        os.remove(f1)
        os.remove(f2)

    @requirement('S-16.162')
    @scenario('S-12/SC-12-SYSINFO')
    def test_SC_12_SYSINFO_trailing(self):
        """Variant trailing: logs/old.log. and logs/x (names S-12.012 refuses) leave no entry on the disk;
        logs/boot.log is copied as before."""
        o = Session()
        self.volume(o, [('old.log.', 100, (2026, 10, 24, 1, 2, 4)), ('x ', 100, (2026, 10, 25, 1, 2, 4))])
        o.add('disk %d 0' % DISK_SECTORS, 'diskformat')
        self.write(o, 'run')
        f = out('trailing.img')
        o.add('disksave ' + f)
        r = o.run()
        self.assertEqual(r['run/export'], '1')
        t = self.check_disk(f)
        self.assertEqual(sorted(p for p in t if p.startswith('logs/')), ['logs/boot.log'])
        os.remove(f)

    @requirement('S-16.082', 'S-14.051')
    def test_S16_082_dot_names(self):
        """logs/.a is exported: the walk skips exactly '.' and '..'."""
        o = Session()
        self.volume(o, [('.a', 10, (2026, 10, 24, 1, 2, 4))])
        o.add('disk %d 0' % DISK_SECTORS, 'diskformat', 'export', name='export')
        o.add('diskread ' + h('logs/.a'), name='a')
        r = o.run()
        self.assertEqual(r['export'], '2')
        self.assertEqual(r['a'], pattern(10, 2).hex())


# --- yaffs2 ------------------------------------------------------------------------------------------------------

def mount_session(o, name):
    """Loads yaffs_<name>.img, mounts it, lists it, unmounts it and saves it."""
    o.add('yload %s %d %d' % (image('yaffs_%s.img' % name), BLOCKS, RESERVE), name=name + '/load')
    o.add('ybe', name=name + '/be')
    o.add('ymount', name=name + '/mount')
    o.add('ytree', name=name + '/tree')
    o.add('yunmount', 'ylog', name=name + '/log')
    o.add('ysave ' + out('y_%s.img' % name))


class YaffsMount(unittest.TestCase):

    def run_cases(self):
        o = Session()
        rows = [c for c in vec('yaffs_trees')['cases'] if not c['refused']]
        for c in rows:
            mount_session(o, c['name'])
        return rows, o.run()

    @requirement('S-16.069', 'S-16.070')
    @scenario('S-12/SC-12-YAFFS-MOUNT')
    def test_SC_12_YAFFS_MOUNT(self):
        """Every case that is not refused mounts; the listing, link targets and file contents are the case's
        tree (outside lost+found exactly, file contents beneath it)."""
        rows, r = self.run_cases()
        for c in rows:
            n = c['name']
            with self.subTest(case=n):
                self.assertEqual((r[n + '/load'], r[n + '/be'], r[n + '/mount']), ('ok', '0', 'ok'))
                outside, lost = split_lost(listing(r[n + '/tree']))
                self.assertEqual(outside, expected_listing(c['tree']))
                self.assertEqual(lost, c['lost_found_files'])
        for c in rows:
            os.remove(out('y_%s.img' % c['name']))

    @scenario('S-12/SC-12-YAFFS-MOUNT')
    def test_SC_12_YAFFS_MOUNT_no_write(self):
        """No write or erase reaches the volume, and every chunk reads as before (S-12.054)."""
        rows, r = self.run_cases()
        for c in rows:
            n = c['name']
            with self.subTest(case=n):
                self.assertEqual(ylog(r[n + '/log']), [])
                assert_unmodified(image('yaffs_%s.img' % n), out('y_%s.img' % n), 4096)
        for c in rows:
            os.remove(out('y_%s.img' % c['name']))

    @requirement('S-12.054', 'S-16.072')
    @scenario('S-12/SC-12-YAFFS-READONLY')
    def test_SC_12_YAFFS_READONLY(self):
        """yaffs_basic and yaffs_seq_classes (a checkpoint block): two sessions of listings and reads write
        and erase nothing; the region is byte-identical and the tree unchanged."""
        o = Session()
        for n in ('basic', 'seq_classes'):
            o.add('yload %s %d %d' % (image('yaffs_%s.img' % n), BLOCKS, RESERVE), 'ymount')
            o.add('ytree', name=n + '/tree')
            o.add('yunmount', 'ymount')
            o.add('ytree', name=n + '/again')
            o.add('yunmount', 'ylog', name=n + '/log')
            o.add('ysave ' + out('y_%s.img' % n))
        r = o.run()
        for n in ('basic', 'seq_classes'):
            with self.subTest(case=n):
                c = cases()[n]
                self.assertEqual(ylog(r[n + '/log']), [])
                assert_unmodified(image('yaffs_%s.img' % n), out('y_%s.img' % n), 4096)
                self.assertEqual(r[n + '/again'], r[n + '/tree'])
                self.assertEqual(reader_tree(yaffs2read.Yaffs2Reader(out('y_%s.img' % n), P)), c['tree'])
                os.remove(out('y_%s.img' % n))

    def write_session(self, t, fill):
        """SC-12-YAFFS-WRITE's changes on yaffs_basic; (results, the image before, the image after)."""
        o = Session()
        o.add('yload %s %d %d' % (image('yaffs_basic.img'), BLOCKS, RESERVE), 'ymount', 'clock %d' % t)
        o.add('ypat %s 10000 3' % h('user/new.bin'), name='new')
        o.add('ymv %s %s' % (h('user/a.txt'), h('user/b.txt')), name='mv')
        o.add('yrm ' + h('logs/boot.log'), name='rm')
        o.add('ymkdir ' + h('user/d'), name='mkdir')
        o.add('ypat %s %d 4' % (h('user/fill'), fill), name='fill')
        o.add('yunmount', 'ylog', name='log')
        after = out('y_write.img')
        o.add('ysave ' + after)
        r = o.run()
        self.assertEqual([r[k] for k in ('new', 'mv', 'rm', 'mkdir', 'fill')], ['1'] * 5)
        return r, image('yaffs_basic.img'), after

    @requirement('S-16.069', 'S-16.070', 'S-16.072')
    @scenario('S-12/SC-12-YAFFS-WRITE')
    def test_SC_12_YAFFS_WRITE(self):
        """yaffs_basic: create user/new.bin, rename user/a.txt to user/b.txt, delete logs/boot.log, create
        user/d, then a file that fills more than a block. The conformance reader sees those changes and
        nothing else; check() is empty; no checkpoint chunk; new blocks have higher sequence numbers, every
        filled block ends with a summary, and what is not written reads as zero (S-16.069)."""
        fill = 130 * P.data_bytes
        r, before, after = self.write_session(utc(2026, 10, 26, 10, 0, 0), fill)
        rb, ra = yaffs2read.Yaffs2Reader(before, P), yaffs2read.Yaffs2Reader(after, P)
        want = dict(cases()['basic']['tree'])
        want['user/b.txt'] = want.pop('user/a.txt')
        want['user/hl'] = {'type': 'hardlink', 'target': 'user/b.txt'}
        del want['logs/boot.log']
        want['user/new.bin'] = {'type': 'file', **digest(pattern(10000, 3))}
        want['user/d'] = {'type': 'dir'}
        want['user/fill'] = {'type': 'file', **digest(pattern(fill, 4))}
        self.assertEqual(reader_tree(ra), dict(sorted(want.items())))
        self.assertEqual(ra.check(), [])
        self.assertEqual(checkpoint_chunks_written(before, after, P), [])
        old = {b: i['seq'] for b, i in rb.blocks.items() if i['state'] == 'data'}
        new = {b: i['seq'] for b, i in ra.blocks.items() if i['state'] == 'data' and old.get(b) != i['seq']}
        self.assertTrue(new)
        self.assertGreater(min(new.values()), max(old.values()))
        filled = [b for b in new if all(ra.tags(ra.chunk_index(b, k)) for k in range(P.chunks_per_block))]
        self.assertTrue(filled)
        for b in filled:
            self.assertTrue(ra.blocks[b]['summary'], 'block %d: no summary' % b)
        for b, i in ra.blocks.items():                                   # S-16.069: what is not written is zero
            if i['state'] == 'empty':
                for k in range(P.chunks_per_block):
                    self.assertEqual(ra.read_chunk(ra.chunk_index(b, k)), bytes(P.chunk_size))
        os.remove(after)

    @expected_failure('S-12/SC-12-YAFFS-WRITE', 'the headers lack S-12.059\'s mode type bits (yaffs2 stores '
                      'the mode its caller gives, 0644 or 0755, and src/core/fs/yaffs.c gives no S_IFREG or '
                      'S_IFDIR), and a rename rewrites the header with the old times, not the clock\'s')
    @requirement('S-12.059')
    def test_SC_12_YAFFS_WRITE_headers(self):
        """The headers SC-12-YAFFS-WRITE's changes write: name, type, the mode's type bits, modification and
        change times of the clock, 0x1E8 and 0x1F8 zero (no object replaced)."""
        t = utc(2026, 10, 26, 10, 0, 0)
        r, before, after = self.write_session(t, 130 * P.data_bytes)
        ra = yaffs2read.Yaffs2Reader(after, P)
        by_path = {ra.path_of(oid): oid for oid in ra.live}
        for path, kind in (('user/new.bin', 0o100000), ('user/b.txt', 0o100000), ('user/d', 0o040000),
                           ('user/fill', 0o100000)):
            with self.subTest(header=path):
                oid = by_path[path]
                a = ra.attributes(oid)
                self.assertIsNotNone(a)
                self.assertEqual(a['name'], path.rsplit('/', 1)[1])
                self.assertEqual(a['mode'] & 0o170000, kind)
                self.assertEqual(ra.live[oid]['type'], yaffs2.FILE if kind == 0o100000 else yaffs2.DIR)
                self.assertEqual(a['times'][1:], (t, t))
                data = ra.read_chunk(ra.live[oid]['hdr'])
                self.assertEqual(struct.unpack_from('<I', data, 0x1E8)[0], 0)
                self.assertEqual(struct.unpack_from('<i', data, 0x1F8)[0], 0)
        os.remove(after)

    @expected_failure('S-12/SC-12-YAFFS-TRUST', 'the volume mounts but the root lists no entry: object 0x300 '
                      'is named from chunk 1\'s data (the S-12.051 trusted-header rule is not done, '
                      'waffle/fs/yaffs.h) and wire_yaffs_list() ends at the entry it cannot stat')
    @requirement('S-16.071')
    @scenario('S-12/SC-12-YAFFS-TRUST')
    def test_SC_12_YAFFS_TRUST(self):
        """yaffs_trust: the volume mounts; real and f003-f126 are listed with their names, real reads as 8,160
        bytes with 4,080-8,159 zero; object 0x300 is listed under a name that is not chunk 1's bytes, or not
        at all; after a rename its header carries the name 'renamed'."""
        trust = vec('yaffs_trust')
        bad = bytes.fromhex(trust['expect']['chunk_1_name_bytes']).split(b'\0')[0]
        o = Session()
        o.add('yload %s %d %d' % (image('yaffs_trust.img'), BLOCKS, RESERVE))
        o.add('ymount', name='mount')
        o.add('ytree', name='tree')
        r = o.run()
        self.assertEqual(r['mount'], 'ok')
        outside, _ = split_lost(listing(r['tree']))
        known = {'real', 'lost+found'} | set(trust['expect']['others_unchanged'])
        for n in trust['expect']['others_unchanged']:
            self.assertEqual(outside.get(n), {'type': 'file', **digest(b'')}, n)
        self.assertEqual(outside.get('real'), {'type': 'file', **digest(pattern(4080, 12) + bytes(4080))})
        rest = sorted(set(outside) - known)
        self.assertLessEqual(len(rest), 1, rest)
        for n in rest:
            self.assertNotEqual(n.encode(), bad)
        names = [n.encode() for n in listing(r['tree'])]
        self.assertNotIn(bad, [n.rsplit(b'/', 1)[-1] for n in names])
        if not rest:
            return
        o = Session()
        o.add('yload %s %d %d' % (image('yaffs_trust.img'), BLOCKS, RESERVE), 'ymount')
        o.add('ymv %s %s' % (h(rest[0]), h('renamed')), name='mv')
        o.add('yunmount')
        f = out('y_trust.img')
        o.add('ysave ' + f)
        r = o.run()
        self.assertEqual(r['mv'], '1')
        ra = yaffs2read.Yaffs2Reader(f, P)
        o3 = ra.live.get(0x300)
        self.assertIsNotNone(o3)
        self.assertEqual(ra.attributes(0x300)['name'], 'renamed')
        os.remove(f)

    @requirement('S-16.078')
    @scenario('S-12/SC-12-YAFFS-BE')
    def test_SC_12_YAFFS_BE(self):
        """The refused cases: S-12.052's test holds, the mount is refused, no directory is listed, a create
        fails, and nothing is written or erased; the region is byte-identical."""
        rows = [c for c in vec('yaffs_trees')['cases'] if c['refused']]
        self.assertEqual(sorted(c['name'] for c in rows), ['big_endian', 'big_endian_filled'])
        o = Session()
        for c in rows:
            n = c['name']
            o.add('yload %s %d %d' % (image('yaffs_%s.img' % n), BLOCKS, RESERVE))
            o.add('ybe', name=n + '/be')
            o.add('ymount', name=n + '/mount')
            o.add('ytree', name=n + '/tree')
            o.add('yput %s %s' % (h('user/new.bin'), b'x'.hex()), name=n + '/create')
            o.add('yunmount', 'ylog', name=n + '/log')
            o.add('ysave ' + out('y_%s.img' % n))
        r = o.run()
        for c in rows:
            n = c['name']
            with self.subTest(case=n):
                self.assertEqual((r[n + '/be'], r[n + '/mount'], r[n + '/tree'], r[n + '/create']),
                                 ('1', 'refused', '-', '0'))
                self.assertEqual(ylog(r[n + '/log']), [])
                assert_unmodified(image('yaffs_%s.img' % n), out('y_%s.img' % n), 4096)
                os.remove(out('y_%s.img' % n))

    @requirement('S-16.072')
    @scenario('S-12/SC-12-YAFFS-CHECKPOINT')
    def test_SC_12_YAFFS_CHECKPOINT(self):
        """yaffs_seq_classes (block 3 holds sequence 0x21) and yaffs_basic: one change. Every checkpoint block
        is erased before the change's first chunk is written; no chunk carrying 0x21 is written; the tree is
        the change applied."""
        o = Session()
        for n in ('seq_classes', 'basic'):
            o.add('yload %s %d %d' % (image('yaffs_%s.img' % n), BLOCKS, RESERVE), 'ymount',
                  'clock %d' % utc(2026, 10, 26))
            o.add('ypat %s 10 5' % h('new'), name=n + '/new')
            o.add('yunmount', 'ylog', name=n + '/log')
            o.add('ysave ' + out('y_cp_%s.img' % n))
        r = o.run()
        for n in ('seq_classes', 'basic'):
            with self.subTest(case=n):
                before, after = image('yaffs_%s.img' % n), out('y_cp_%s.img' % n)
                rb = yaffs2read.Yaffs2Reader(before, P)
                checkpoint = [b for b, i in rb.blocks.items() if i['seq'] == yaffs2.SEQ_CHECKPOINT]
                self.assertEqual(checkpoint, [3] if n == 'seq_classes' else [])
                log = ylog(r[n + '/log'])
                first = next(i for i, (k, _) in enumerate(log) if k == 'w')
                for b in checkpoint:
                    self.assertIn(('e', b), log[:first])
                ra = yaffs2read.Yaffs2Reader(after, P)
                for k, idx in log:
                    if k == 'w':
                        t = ra.tags(idx)
                        self.assertTrue(t is None or t['seq'] != yaffs2.SEQ_CHECKPOINT, idx)
                self.assertEqual(checkpoint_chunks_written(before, after, P), [])
                want = dict(cases()[n]['tree'], new={'type': 'file', **digest(pattern(10, 5))})
                self.assertEqual(reader_tree(ra), dict(sorted(want.items())))
                os.remove(after)

    @scenario('S-12/SC-12-YAFFS-FORMAT')
    def test_SC_12_YAFFS_FORMAT(self):
        """(a) wire_yaffs_format() of yaffs_basic erases every block and writes nothing: the volume reads as
        zero; (b) an all-zero volume mounts as the root and lost+found, and the mount writes nothing."""
        o = Session()
        o.add('yload %s %d %d' % (image('yaffs_basic.img'), BLOCKS, RESERVE))
        o.add('yformat', name='format')
        o.add('ytree', name='formatted')
        o.add('yunmount', 'ylog', name='log')
        fa = out('y_format.img')
        o.add('ysave ' + fa)
        o.add('yvol %d %d' % (BLOCKS, RESERVE), 'ymount', name='zero')
        o.add('ytree', name='tree')
        o.add('yunmount', 'ylog', name='zlog')
        r = o.run()
        self.assertEqual(r['format'], '1')
        log = ylog(r['log'])
        self.assertEqual(sorted(i for k, i in log if k == 'e'), list(range(BLOCKS)))
        self.assertEqual([x for x in log if x[0] == 'w'], [])
        self.assertTrue(zero_file(fa))
        self.assertEqual(listing(r['formatted']), {'lost+found': {'type': 'dir'}})
        self.assertEqual(r['zero'], 'ok')
        self.assertEqual(listing(r['tree']), {'lost+found': {'type': 'dir'}})
        self.assertEqual(ylog(r['zlog']), [])
        os.remove(fa)

    @scenario('S-12/SC-12-YAFFS-BE-FORMAT')
    def test_SC_12_YAFFS_BE_FORMAT(self):
        """(a) without a format, the refused volume is untouched (as SC-12-YAFFS-BE); (b) a confirmed format
        erases every block, the volume then mounts as the root and lost+found, and user/new.bin can be
        created."""
        o = Session()
        for n in ('big_endian', 'big_endian_filled'):
            o.add('yload %s %d %d' % (image('yaffs_%s.img' % n), BLOCKS, RESERVE))
            o.add('ymount', name=n + '/refused')
            o.add('ylog', name=n + '/untouched')
            o.add('yformat', name=n + '/format')
            o.add('ylog', name=n + '/log')
            o.add('ysave ' + out('y_bef_%s.img' % n))
            o.add('ytree', name=n + '/tree')
            o.add('yput %s %s' % (h('user/new.bin'), pattern(100).hex()), name=n + '/create')
            o.add('yunmount')
            o.add('ysave ' + out('y_bef2_%s.img' % n))
        r = o.run()
        for n in ('big_endian', 'big_endian_filled'):
            with self.subTest(case=n):
                self.assertEqual((r[n + '/refused'], r[n + '/untouched']), ('refused', '-'))
                self.assertEqual(r[n + '/format'], '1')
                log = ylog(r[n + '/log'])
                self.assertEqual(sorted(i for k, i in log if k == 'e'), list(range(BLOCKS)))
                self.assertTrue(zero_file(out('y_bef_%s.img' % n)))
                self.assertFalse(yaffs2read.Yaffs2Reader(out('y_bef_%s.img' % n), P).refused)
                self.assertEqual(listing(r[n + '/tree']), {'lost+found': {'type': 'dir'}})
                self.assertEqual(r[n + '/create'], '1')
                ra = yaffs2read.Yaffs2Reader(out('y_bef2_%s.img' % n), P)
                self.assertEqual(reader_tree(ra), {'lost+found': {'type': 'dir'}, 'user': {'type': 'dir'},
                                                   'user/new.bin': {'type': 'file', **digest(pattern(100))}})
                os.remove(out('y_bef_%s.img' % n))
                os.remove(out('y_bef2_%s.img' % n))

    FULL_BLOCKS, FULL_FILE = 64, 300000

    def fill(self):
        """SC-12-YAFFS-FULL on a 64-block volume with the reserve of 5: files of 300,000 bytes are written until
        one is refused, with the free space after each; then a second run stops at the refusal, unmounts and
        saves the volume. (results, files written, the saved image, its parameters)."""
        n_blocks, size = self.FULL_BLOCKS, self.FULL_FILE
        o = Session()
        o.add('yvol %d 5' % n_blocks, 'ymount')
        o.add('yfree', name='free/0')
        for k in range(1, 120):
            o.add('ypat %s %d %d' % (h('f%03d' % k), size, k % 200), name='w/%d' % k)
            o.add('yfree', name='free/%d' % k)
        r = o.run()
        written = next((k - 1 for k in range(1, 120) if r['w/%d' % k] != '1'), None)
        self.assertIsNotNone(written, 'no write was refused')
        o = Session()
        o.add('yvol %d 5' % n_blocks, 'ymount')
        for k in range(1, written + 2):
            o.add('ypat %s %d %d' % (h('f%03d' % k), size, k % 200), name='w/%d' % k)
        o.add('yunmount')
        f = out('y_full.img')
        o.add('ysave ' + f)
        r2 = o.run()
        self.assertEqual([r2['w/%d' % k] for k in range(1, written + 2)], ['1'] * written + ['0'])
        return r, written, f, yaffs2.op_xy_params(n_blocks=n_blocks)

    @scenario('S-12/SC-12-YAFFS-FULL')
    def test_SC_12_YAFFS_FULL(self):
        """The free space wire_yaffs_free_space() reports is never negative and never more than (empty or
        reclaimable chunks - 768) x 4,080 (no file is removed, so every chunk not holding a file's data or a
        header is counted as reclaimable); a write is refused before the volume is full."""
        r, written, f, p = self.fill()
        total = p.n_blocks * p.chunks_per_block
        for j in range(0, written + 1):
            free = r['free/%d' % j]
            self.assertNotEqual(free, '-')
            live = j * (-(-self.FULL_FILE // p.data_bytes) + 1)
            self.assertGreaterEqual(int(free), 0)
            self.assertLessEqual(int(free), max(0, total - live - 6 * p.chunks_per_block) * p.data_bytes, j)
        self.assertLess(written * self.FULL_FILE, total * p.data_bytes)
        os.remove(f)

    @expected_failure('S-12/SC-12-YAFFS-FULL', 'at the refusal 4 blocks are empty, not 5: yaffs2 refuses a '
                      'write only when its free chunks, the allocating block\'s included, reach the reserve '
                      '(S-12.056: a change never leaves fewer than 5 empty blocks)')
    @requirement('S-12.056')
    def test_SC_12_YAFFS_FULL_reserve(self):
        """At the refusal at least 5 blocks of the volume are empty, and they are still empty after the
        unmount."""
        _, _, f, p = self.fill()
        ra = yaffs2read.Yaffs2Reader(f, p)
        empty = sum(1 for i in ra.blocks.values() if i['state'] == 'empty')
        os.remove(f)
        self.assertGreaterEqual(empty, 5)

if __name__ == '__main__':
    unittest.main()
