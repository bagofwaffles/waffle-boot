#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host tests of tools/mtpcp.py prepare-1f (S-05.042) and of its model, with no USB: the model against
tests/oracle/S-05-os-handoff/vectors/landing_1f.json, and the command against a stand-in for an OP-1F's
drive-mode MTP responder that holds a whole folder tree. Python 3, standard library only.

    test_mtpcp_prepare_1f.py
"""
import collections
import hashlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
ORACLE = os.path.join(TOP, 'tests', 'oracle', 'S-05-os-handoff')
sys.path.insert(0, os.path.join(TOP, 'tools'))
sys.path.insert(0, ORACLE)
import mtpcp  # noqa: E402
import landing_1f as L  # noqa: E402

CHANGES = {0x100B, 0x100C, 0x100D, 0x1019, 0x9804}
ALLOWED = {0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1007, 0x1008, 0x1009} | CHANGES

with open(os.path.join(ORACLE, 'vectors', 'landing_1f.json')) as f:
    VECTORS = json.load(f)


def case_listing(c):
    if 'listing' in c:
        return [list(r) for r in c['listing']]
    with open(os.path.join(ORACLE, 'vectors', c['listing_file'])) as f:
        raw = f.read()
    assert hashlib.sha256(raw.encode()).hexdigest() == c['listing_sha256']
    drop = set(c.get('drop', ()))
    return [list(r) for r in json.loads(raw)['entries'] if r[0] not in drop]


def entries(listing):
    return [{'path': p, 'folder': f, 'size': s} for p, f, s in listing]


def case(name):
    return next(c for c in VECTORS['cases'] if c['name'] == name)


# ---------------------------------------------------------------- the model against the vectors

class Model(unittest.TestCase):
    def check_verdict(self, build, listing, want):
        v = L.verdict(build, entries(listing))
        for k in ('files', 'folders', 'records', 'nodes', 'record_batches', 'node_batches',
                  'record_headroom', 'node_headroom', 'lands'):
            self.assertEqual(v[k], want[k], k)
        self.assertEqual({str(r): '0x%08X' % b for r, b in v['blocks'].items()}, want['blocks'])
        self.assertEqual({str(r): x for r, x in v['residues'].items()}, want['residues'])

    def test_every_case(self):
        self.assertGreaterEqual(len(VECTORS['cases']), 10)
        for c in VECTORS['cases']:
            with self.subTest(c['name']):
                listing = case_listing(c)
                if 'as_listed' in c:
                    self.check_verdict(c['build'], listing, c['as_listed'])
                if 'refused' in c:
                    with self.assertRaises(L.Refused) as e:
                        L.plan(c['build'], entries(listing), c['image_size'])
                    self.assertEqual(str(e.exception), c['refused'])
                    continue
                p = L.plan(c['build'], entries(listing), c['image_size'])
                self.assertEqual(p['remove'], c['plan']['remove'])
                self.assertEqual(p['pad'], c['plan']['pad'])
                self.assertEqual(p['warnings'], c['plan']['warnings'])
                self.check_verdict(c['build'], [[e['path'], e['folder'], e['size']] for e in p['listing']],
                                   c['plan']['arranged'])
                self.assertTrue(p['verdict']['lands'])

    def test_nodes(self):
        for size, chunks, n in VECTORS['nodes']:
            self.assertEqual((L.chunks(size), L.nodes(size)), (chunks, n), size)

    def test_the_landing_on_a_unit(self):
        # One write on a 1000.2.0 unit landed with this volume and a 6,200,000-byte pad.
        v = L.verdict('1000.2.0', entries(case_listing(case('run_as_written'))))
        self.assertEqual((v['files'], v['folders'], v['records'], v['nodes']), (1009, 104, 1117, 8701))
        self.assertEqual((v['record_batches'], v['node_batches']), (12, 88))
        self.assertTrue(all(b % 32 == 0 for b in v['blocks'].values()))
        p = L.plan('1000.2.0', entries(case_listing(case('run_without_its_pad'))), 2104908)
        self.assertEqual(p['pad'], 6200000)
        self.assertEqual((p['verdict']['node_headroom'], p['verdict']['record_headroom']), (99, 83))

    def test_reference_values(self):
        # S-05.042's table: the top of use and the block per rest, at the counts level.
        names = [r['name'] for r in VECTORS['reference']]
        self.assertEqual(names[:3], ['open_field', 'refused_volume', 'refused_base_with_6300000_pad'])
        for r in VECTORS['reference'][:3]:
            with self.subTest(r['name']):
                v = L.verdict_counts(r['build'], r['records'], r['nodes'])
                self.assertEqual((v['node_batches'], v['record_batches']), (r['node_batches'], r['record_batches']))
                self.assertEqual({str(k): '0x%08X' % t for k, t in v['tops'].items()}, r['tops'])
                self.assertEqual({str(k): '0x%08X' % b for k, b in v['blocks'].items()}, r['blocks'])
                self.assertEqual(v['lands'], r['lands'])
        self.assertEqual([r['residues']['96'] for r in VECTORS['reference'][:3]], [0, 8, 0])

    def test_the_refused_volume(self):
        # A 1000.2.0 unit refused the write with a 34,800,000-byte pad; a 6,300,000-byte pad landed.
        self.assertEqual(L.verdict_counts('1000.2.0', 1249, 8788 + L.nodes(34800000))['residues'],
                         {96: 8, 2768: 8})
        self.assertEqual(L.choose_pad('1000.2.0', 1248, 8788), 6300000)
        r = next(r for r in VECTORS['reference'] if r['name'] == 'refused_base_pad_chosen')
        self.assertEqual((r['pad'], r['node_headroom'], r['record_headroom'], r['lands']), (6300000, 99, 51, True))

    def test_the_allocator(self):
        heap = L.Heap(L.HEAP_BASE['1000.2.0'])
        self.assertEqual(heap.start, (0x88A22B3C + 0x23) & ~7)
        a = heap.alloc(16)
        self.assertEqual((a, heap.brk, heap.free), (heap.start, heap.start + 0x200, [[heap.start + 16, 0x200 - 16]]))
        b = heap.alloc(0x200 - 24)                  # leaves 8 bytes: the whole cell is given
        self.assertEqual((b, heap.free, heap.top), (heap.start + 16, [], heap.start + 0x200))
        c = heap.alloc(0x1000)                      # grows by need, not by whole steps
        self.assertEqual((c, heap.brk, heap.top), (heap.start + 0x200, heap.start + 0x1200, heap.start + 0x1200))
        heap = L.Heap(L.HEAP_BASE['1000.2.0'])
        for s in L.constructor_requests():
            heap.alloc(L.need(s))
        self.assertEqual([size for _, size in heap.free], [96])   # the plain start's rest
        self.assertEqual((heap.start, heap.top), (0x88A22B58, 0x88A4A010))
        self.assertEqual(L.Heap(L.HEAP_BASE['1000.1.1']).start, 0x88A1CA90)
        heap.leave_rest(96)
        top = heap.top
        heap.alloc(48)                              # a small cell from the rest: the top does not move
        self.assertEqual(heap.top, top + 48)
        heap.alloc(4096)                            # grows by 4,096 joined to the 48 left: 48 left again
        self.assertEqual(heap.free, [[heap.top, 48]])
        heap.alloc(16)                              # from that remainder, below the brk
        self.assertEqual((heap.free, heap.brk - heap.top), ([[heap.top, 32]], 32))

    def test_edges(self):
        self.assertEqual(case('lands_as_it_stands')['plan']['pad'], None)
        exact = case('nodes_exact_multiple')
        self.assertEqual(exact['plan']['unpadded']['nodes'] % 100, 0)
        self.assertTrue(exact['plan']['unpadded']['lands'])
        self.assertIsNotNone(exact['plan']['pad'])
        self.assertIn('exact multiple', exact['plan']['warnings'][0])
        self.assertIn('record margin', case('no_record_margin')['refused'])
        self.assertIn('stated only for', case('refused_build')['refused'])
        for c in VECTORS['cases']:
            if 'plan' in c and c['plan']['pad']:
                self.assertNotEqual(c['plan']['arranged']['nodes'] % 100, 0)
                self.assertGreaterEqual(c['plan']['arranged']['record_headroom'], 3)

    def test_builds(self):
        self.assertEqual(L.build_of('1000.2.0'), '1000.2.0')
        self.assertEqual(L.build_of('bootloader 1000.1.1'), '1000.1.1')
        for text in ('', None, '1000.2.1', '1000.2.0 beta', 'bootloader', '9.9.9', 'v1000.2.0'):
            self.assertIsNone(L.build_of(text), text)

    def test_removals(self):
        listing = [['logs', True, 0], ['logs/a.txt', False, 5], ['logs/sub', True, 0],
                   ['logs/sub/b.txt', False, 6], ['waffle-pad.bin', False, 7], ['waffle-pad-2.bin', False, 8],
                   ['user', True, 0], ['user/waffle-pad.bin', False, 9], ['other-pad.bin', False, 10],
                   ['os.ldr', False, 11]]
        self.assertEqual([e['path'] for e in L.removals(entries(listing))],
                         ['logs/a.txt', 'waffle-pad.bin', 'waffle-pad-2.bin'])
        rows = [(e['path'], e['size']) for e in L.arranged(entries(listing), 20000, 1000000)]
        self.assertIn(('os.ldr', 20000), rows)
        self.assertIn(('waffle-pad.bin', 1000000), rows)
        self.assertIn(('logs/sub/b.txt', 6), rows)
        self.assertNotIn(('logs/a.txt', 5), rows)


# ---------------------------------------------------------------- a stand-in for the unit's responder

class Unit:
    """A writable MTP responder over a folder tree, at the transport's level. Rows are [path, folder,
    content]: a file's content is bytes, or an int when only its size matters (the tool never reads it). Faults: log_after_upload adds a
    file to logs/ once os.ldr arrives (the unit writing its log), short_read cuts every GetObject by one
    byte, read_only reports a read-only store."""

    def __init__(self, listing=(), version='1000.2.0', model='OP-1 Field', mps=512, **faults):
        self.mps = mps
        self.version, self.model = version, model
        self.faults = faults
        self.objects = {}           # handle: [parent, name, content or None for a folder]
        self.next = 0x100
        self.out = collections.deque()
        self.pending = None
        self.buf = bytearray()
        self.session = 0
        self.log = []               # (operation, parameters)
        by_path = {'': mtpcp.ROOT}
        for path, folder, content in sorted(listing, key=lambda r: r[0].count('/')):
            by_path[path] = self.add(by_path[path.rpartition('/')[0]], path.rpartition('/')[2],
                                     None if folder else content)

    def add(self, parent, name, content):
        h = self.next
        self.next += 1
        self.objects[h] = [parent, name, content]
        return h

    def path(self, h):
        parent, name, _ = self.objects[h]
        return name if parent == mtpcp.ROOT else self.path(parent) + '/' + name

    def find(self, path):
        return next((h for h in self.objects if self.path(h) == path), None)

    def content(self, path):
        h = self.find(path)
        return None if h is None else self.objects[h][2]

    def rows(self):
        out = []
        for h, (_, _, c) in self.objects.items():
            size = 0 if c is None else c if isinstance(c, int) else len(c)
            out.append((self.path(h), c is None, size))
        return sorted(out)

    @property
    def ops(self):
        return [op for op, _ in self.log]

    # the transport
    def write(self, data, timeout_ms=0):
        data = bytes(data)
        if self.pending is None:
            if not data:
                return
            _, kind, code, tid = struct.unpack_from('<IHHI', data)
            assert kind == mtpcp.CMD
            params = struct.unpack('<%dI' % ((len(data) - 12) // 4), data[12:])
            self.log.append((code, params))
            assert code in ALLOWED, 'the tool sent %#x' % code
            if code in (0x100C, 0x100D):
                self.pending = (code, tid, params)
                self.buf = bytearray()
            else:
                self.command(code, tid, params)
            return
        self.buf += data
        total = struct.unpack_from('<I', self.buf)[0]
        if len(self.buf) >= total:
            code, tid, params = self.pending
            self.pending = None
            self.data_phase(code, tid, params, bytes(self.buf[12:total]))

    def read(self, size, timeout_ms=0):
        if not self.out:
            raise mtpcp.Timeout('nothing to read')
        head = self.out[0]
        if len(head) <= size:
            return self.out.popleft()
        self.out[0] = head[size:]
        return head[:size]

    def queue(self, code, tid, payload):
        self.out.append(struct.pack('<IHHI', 12 + len(payload), mtpcp.DATA, code, tid) + payload)

    def respond(self, code, tid, *params):
        self.out.append(struct.pack('<IHHI%dI' % len(params), 12 + 4 * len(params), mtpcp.RESP, code, tid,
                                    *params))

    def info(self, h):
        _, name, c = self.objects[h]
        size = 0 if c is None else c if isinstance(c, int) else len(c)
        parent = self.objects[h][0]
        return (struct.pack('<IHHIHIIIIIIIHII', 0x10001, mtpcp.FORMAT_FOLDER if c is None else
                            mtpcp.FORMAT_UNDEFINED, 0, size, 0, 0, 0, 0, 0, 0, 0,
                            0 if parent == mtpcp.ROOT else parent, 1 if c is None else 0, 0, 0)
                + mtpcp.mtp_string(name) + mtpcp.mtp_string('20261120T000000') * 2 + b'\0')

    def command(self, code, tid, p):
        if code == 0x1001:
            self.queue(code, tid, struct.pack('<HIH', 100, 6, 100) + mtpcp.mtp_string('microsoft.com: 1.0;')
                       + struct.pack('<H', 0) + struct.pack('<I', 0) * 5
                       + mtpcp.mtp_string('teenage engineering') + mtpcp.mtp_string(self.model)
                       + mtpcp.mtp_string(self.version) + mtpcp.mtp_string('s'))
            self.respond(mtpcp.OK, tid)
        elif code == 0x1002:
            self.session = p[0]
            self.respond(mtpcp.OK, tid)
        elif code == 0x1003:
            self.session = 0
            self.respond(mtpcp.OK, tid)
        elif code == 0x1004:
            self.queue(code, tid, struct.pack('<II', 1, 0x10001))
            self.respond(mtpcp.OK, tid)
        elif code == 0x1005:
            access = 2 if self.faults.get('read_only') else 0
            self.queue(code, tid, struct.pack('<HHHQQI', 3, 2, access, 15 << 30, 10 << 30, 0xFFFFFFFF)
                       + mtpcp.mtp_string('OP-1 Field') + b'\0')
            self.respond(mtpcp.OK, tid)
        elif code == 0x1007:
            hs = [h for h, o in self.objects.items() if o[0] == p[2]]
            self.queue(code, tid, struct.pack('<I%dI' % len(hs), len(hs), *hs))
            self.respond(mtpcp.OK, tid)
        elif code == 0x1008:
            self.queue(code, tid, self.info(p[0]))
            self.respond(mtpcp.OK, tid)
        elif code == 0x1009:
            c = self.objects[p[0]][2]
            assert isinstance(c, bytes), 'the tool read %s, a file it had no need to read' % self.path(p[0])
            self.queue(code, tid, c[:-1] if self.faults.get('short_read') else c)
            self.respond(mtpcp.OK, tid)
        elif code == 0x100B:
            assert self.objects[p[0]][2] is not None, 'a folder delete'
            del self.objects[p[0]]
            self.respond(mtpcp.OK, tid)

    def data_phase(self, code, tid, params, payload):
        if code == 0x100C:
            info = mtpcp.Object(0, payload)
            parent = params[1] if params[1] not in (0,) else mtpcp.ROOT
            self.announced = self.add(parent, info.name, b'')
            self.respond(mtpcp.OK, tid, 0x10001, parent, self.announced)
        else:
            self.objects[self.announced][2] = payload
            if self.objects[self.announced][1] == 'os.ldr' and self.faults.pop('log_after_upload', None):
                logs = next(h for h, o in self.objects.items() if o[1] == 'logs' and o[0] == mtpcp.ROOT)
                self.add(logs, 'new.log', b'written by the unit')
            self.respond(mtpcp.OK, tid)


def fill(listing, removed):
    """The listing with content for the files the tool reads (those it removes), sizes for the rest."""
    out = []
    for path, folder, size in listing:
        if not folder and path in removed:
            out.append([path, folder, bytes((i * 7 + len(path)) % 251 for i in range(size))])
        else:
            out.append([path, folder, size])
    return out


IMAGE = bytes((i * 13) % 256 for i in range(877092))


class Prepare(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.backup = os.path.join(self.tmp.name, 'backup')

    def tearDown(self):
        self.tmp.cleanup()

    def small(self):
        c = case('small_volume')
        listing = case_listing(c)
        return c, Unit(fill(listing, set(c['plan']['remove'])))

    def run_prepare(self, unit, image=IMAGE, **kw):
        out = io.StringIO()
        plan = mtpcp.cmd_prepare_1f(mtpcp.Mtp(unit), 'image.ldr', image, out, backup=self.backup, **kw)
        self.assertLessEqual(set(unit.ops), ALLOWED)
        return plan, out.getvalue()

    def test_prepare_small_volume(self):
        c, unit = self.small()
        before = {p: unit.content(p) for p in c['plan']['remove']}
        plan, text = self.run_prepare(unit)
        self.assertEqual(plan['pad'], c['plan']['pad'])
        self.assertEqual(unit.rows(), [tuple(r) for r in L.rows(plan['listing'])])
        self.assertEqual(unit.content('os.ldr'), IMAGE)
        self.assertEqual(unit.content(L.PAD_NAME), bytes(plan['pad']))
        for p in c['plan']['remove']:
            self.assertIsNone(unit.find(p))
            with open(os.path.join(self.backup, p), 'rb') as f:
                self.assertEqual(f.read(), before[p])
        with open(os.path.join(self.backup, 'SHA256SUMS')) as f:
            sums = f.read().split('\n')
        self.assertIn('%s  %s' % (hashlib.sha256(before['logs/log000.txt']).hexdigest(), 'logs/log000.txt'), sums)
        self.assertIn('READY TO WRITE', text)
        self.assertIn('OP-1 Field', text)
        a = c['plan']['arranged']
        self.assertIn('margins: %d index nodes and %d records' % (a['node_headroom'], a['record_headroom']), text)
        self.assertIn('developer menu\'s OS write', text)
        self.assertIn('Keep the computer on the cable', text)
        self.assertTrue(L.verdict('1000.2.0', [{'path': p, 'folder': f, 'size': s} for p, f, s in unit.rows()])
                        ['lands'])

    def test_backup_comes_before_every_remove(self):
        c, unit = self.small()
        handles = {p: unit.find(p) for p in c['plan']['remove']}
        self.run_prepare(unit)
        log = unit.log
        first_delete = unit.ops.index(0x100B)
        for p, h in handles.items():
            read = [i for i, (op, prm) in enumerate(log) if op == 0x1009 and prm[0] == h]
            gone = [i for i, (op, prm) in enumerate(log) if op == 0x100B and prm[0] == h]
            self.assertEqual(len(gone), 1, p)
            self.assertTrue(read and read[0] < first_delete, p)
        self.assertLess(first_delete, unit.ops.index(0x100C))

    def test_dry_run_changes_nothing(self):
        c, unit = self.small()
        rows = unit.rows()
        plan, text = self.run_prepare(unit, dry_run=True)
        self.assertEqual(plan['pad'], c['plan']['pad'])
        self.assertFalse(CHANGES & set(unit.ops))
        self.assertNotIn(0x1009, unit.ops)
        self.assertEqual(unit.rows(), rows)
        self.assertFalse(os.path.exists(self.backup))
        self.assertIn('dry run: nothing was changed', text)
        self.assertNotIn('READY TO WRITE', text)

    def test_the_landing_case(self):
        listing = case_listing(case('run_without_its_pad'))
        unit = Unit(listing)
        image = bytes(2104908)
        plan, text = self.run_prepare(unit, image, dry_run=True)
        self.assertEqual(plan['pad'], 6200000)
        self.assertIn('records 1117 (12 batches), index nodes 8701 (88 batches)', text)
        self.assertIn('margins: 99 index nodes, 83 records', text)
        self.assertFalse(CHANGES & set(unit.ops))
        # And for real: the old os.ldr is the only file removed, and is backed up first.
        unit = Unit(fill(listing, {'os.ldr'}))
        old = unit.content('os.ldr')
        plan, text = self.run_prepare(unit, image)
        self.assertIn('READY TO WRITE', text)
        self.assertEqual(unit.content(L.PAD_NAME), bytes(6200000))
        with open(os.path.join(self.backup, 'os.ldr'), 'rb') as f:
            self.assertEqual(f.read(), old)

    def test_unknown_builds_are_refused(self):
        for version in ('1000.3.0', '', '1.7.3', 'bootloader 1000.2.1', '0.9.44'):
            c, unit = self.small()
            unit.version = version
            with self.assertRaisesRegex(mtpcp.Failure, 'no verdict(.|\n)*nothing was changed'):
                self.run_prepare(unit)
            self.assertFalse(CHANGES & set(unit.ops))
            self.assertNotIn(0x1009, unit.ops)
            self.assertNotIn(0x1007, unit.ops)
            self.assertFalse(os.path.exists(self.backup))
        listing = case_listing(case('run_without_its_pad'))
        unit = Unit(listing)
        unit.version = 'bootloader 1000.1.1'
        plan, text = self.run_prepare(unit, bytes(2104908), dry_run=True)
        self.assertEqual(plan['pad'], L.plan('1000.1.1', entries(listing), 2104908)['pad'])
        self.assertIn('bootloader 1000.1.1', text)

    def test_relist_mismatch_stops(self):
        c = case('small_volume')
        unit = Unit(fill(case_listing(c), set(c['plan']['remove'])), log_after_upload=True)
        out = io.StringIO()
        with self.assertRaisesRegex(mtpcp.Failure, 'row for row(.|\n)*logs/new.log(.|\n)*do NOT run the OS '
                                                   'write(.|\n)*removed are in'):
            mtpcp.cmd_prepare_1f(mtpcp.Mtp(unit), 'image.ldr', IMAGE, out, backup=self.backup)
        self.assertNotIn('READY', out.getvalue())

    def test_a_short_backup_read_changes_nothing(self):
        c = case('small_volume')
        unit = Unit(fill(case_listing(c), set(c['plan']['remove'])), short_read=True)
        with self.assertRaisesRegex(mtpcp.Failure, 'read \\d+: nothing was changed'):
            self.run_prepare(unit)
        self.assertFalse(CHANGES & set(unit.ops))

    def test_read_only_store(self):
        _, unit = self.small()
        unit.faults['read_only'] = True
        with self.assertRaisesRegex(mtpcp.Failure, 'read-only(.|\n)*nothing was changed'):
            self.run_prepare(unit)
        self.assertFalse(CHANGES & set(unit.ops))

    def test_backup_folder(self):
        with self.assertRaisesRegex(mtpcp.Failure, 'inside the repository'):
            mtpcp.check_backup_dir(os.path.join(TOP, 'build', 'backup'))
        os.makedirs(self.backup)
        with open(os.path.join(self.backup, 'x'), 'w') as f:
            f.write('1')
        with self.assertRaisesRegex(mtpcp.Failure, 'already holds files'):
            mtpcp.check_backup_dir(self.backup)
        default = mtpcp.default_backup_dir()
        self.assertIn(os.sep + 'waffle-op1f-backup' + os.sep, default)
        self.assertFalse(os.path.realpath(default).startswith(os.path.realpath(TOP) + os.sep))

    def test_image_size_checked_before_usb(self):
        # load() refuses before any transport is opened; a transport opened anyway fails the test.
        def no_usb():
            raise AssertionError('the tool opened USB')
        saved, mtpcp.UsbTransport = mtpcp.UsbTransport, no_usb
        try:
            for size in (100, 0x800001):
                path = os.path.join(self.tmp.name, 'image.ldr')
                with open(path, 'wb') as f:
                    f.write(bytes(size))
                self.assertEqual(mtpcp.main(['prepare-1f', path, '--dry-run']), 1)
        finally:
            mtpcp.UsbTransport = saved


if __name__ == '__main__':
    unittest.main()
