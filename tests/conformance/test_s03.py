# SPDX-License-Identifier: GPL-2.0-or-later
"""S-03 eMMC: src/core/emmc/emmc.c (through h_s03) on boot partitions built with images.emmc, against its
load rule and writer and tests/oracle/S-03-emmc (model, vectors) (part 'logic')."""

import hashlib
import json
import os
import re
import unittest

from harness import BUILD, run, scenario, spec_module, spec_path

G = spec_module('S-03-emmc', 'gen_vectors')
M = G.M
from images import emmc  # noqa: E402  (the generator's copy, as it built the vectors)
from images.crc import crc32  # noqa: E402

WORK = os.path.join(BUILD, 'work')
REASONS = ['', 'no_active', 'zero_length', 'too_long', 'past_partition', 'format', 'encrypted',
           'crc_mismatch', 'io']


def vectors(name):
    with open(spec_path('S-03-emmc', 'vectors', name), encoding='utf-8') as f:
        return json.load(f)


_saved = {}


def save(key, data):
    if key not in _saved:
        os.makedirs(WORK, exist_ok=True)
        path = os.path.join(WORK, 's03_%s.bin' % key)
        with open(path, 'wb') as f:
            f.write(data)
        _saved[key] = path
    return _saved[key]


def split(line):
    res, _, ops = line.partition(' |')
    return res.split(), ops.split()


def ops_of(ref):
    """write_sequences.json / model ops -> the program's notation."""
    out = []
    for o in ref:
        if o[0] == 'flush':                     # S-03.068: EXT_CSD[32] <- 1
            out.append('x:32:1' + (':error' if o[-1] == 'error' else ''))
            continue
        err = ':error' if o[-1] == 'error' else ''
        o = [x for x in o if x != 'error']
        out.append({'select': 's', 'read': 'r', 'erase': 'e', 'write': 'w'}[o[0]] + ':' +
                   ':'.join(str(x) for x in o[1:]) + err)
    return out


def merged(ops):
    """Reads and writes of contiguous sectors of one partition, one after the other, as one: S-03.043 and
    S-03.055 leave the split into transfers free."""
    out = []
    for o in ops:
        f = o.split(':')
        if out and f[0] in 'rw' and len(f) == 4:
            p = out[-1].split(':')
            if p[0] == f[0] and len(p) == 4 and p[1] == f[1] and int(p[2]) + int(p[3]) == int(f[2]):
                out[-1] = '%s:%s:%s:%d' % (f[0], f[1], p[2], int(p[3]) + int(f[3]))
                continue
        out.append(o)
    return out


def emmc_cmds(device, key):
    return ['bsize %d' % device.B, 'load 1 ' + save(key + '_1', bytes(device.boot[M.BP1])),
            'load 2 ' + save(key + '_2', bytes(device.boot[M.BP2]))]


def outcome(res, old, new):
    """boot's result line -> 'old', 'new' or 'none' by the CRC of the image loaded."""
    if res[0] != '0':
        return 'none'
    crc = int(res[4], 16)
    return 'new' if crc == crc32(new) and int(res[2]) == len(new) else \
        'old' if crc == crc32(old) and int(res[2]) == len(old) else 'other'


S03_RETRIES = 3                  # S-03.010: attempts of a failing operation (src/include/waffle/emmc/emmc.h)


class Load(unittest.TestCase):

    def row_emmc(self, row):
        c = M.Emmc(G.B)
        G.build(row['p1'], M.BP1, c, 1)
        G.build(row['p2'], M.BP2, c, 2)
        self.assertEqual(hashlib.sha256(bytes(c.boot[M.BP1])).hexdigest(), row['p1_sha256'], row['name'])
        self.assertEqual(hashlib.sha256(bytes(c.boot[M.BP2])).hexdigest(), row['p2_sha256'], row['name'])
        return c

    def check_row(self, row, res, ops):
        what = row['name']
        want = 'no_os' if res[0] != '0' else 'BP%s' % res[1]
        self.assertEqual(want, row['expect'], what)
        if row['expect'] == 'no_os':
            self.assertEqual(REASONS[int(res[0])], row['reason'], what)
        else:
            self.assertEqual(int(res[2]), row['loaded_length'], what)
            self.assertEqual('%08x' % int(res[4], 16), row['loaded_crc32'], what)
        self.assertEqual([int(o[2:]) for o in ops if o.startswith('s:')], row['selects'], what)
        self.assertEqual([int(o.split(':')[1]) for o in ops if o.startswith('r:') and o.split(':')[2] == '0'],
                         row['header_reads'], what)
        self.assertFalse([o for o in ops if o[0] in 'ew'], what)
        reads = [o.split(':') for o in ops if o.startswith('r:') and o.split(':')[2] != '0']
        covered = set()
        for _, p, first, count in reads:
            covered.update(range(int(first), int(first) + int(count)))
        if row['extent']:
            lo, hi = row['extent']
            self.assertTrue(set(range(lo, hi + 1)) <= covered, what)
            self.assertLessEqual(max(covered), hi + 1, what)       # none past N + 1
        elif row['reason'] != 'crc_mismatch':
            self.assertFalse(reads, what)                          # S-03.042, S-03.048: no image read

    @scenario('S-03/SC-03-LOAD-TABLE', 'S-03/SC-03-BOOT-P1', 'S-03/SC-03-BOOT-P2', 'S-03/SC-03-NO-OS',
              'S-03/SC-03-BOOT-ENC', 'S-03/SC-03-BAD-CRC')
    def test_SC_03_LOAD_TABLE(self):
        """Every row of load_table.json: the outcome (partition, length, CRC-32, or no OS and why), the
        selects in order ending on the user area, the header reads, image reads covering the extent and
        none past N + 1, and no erase or write; also against images.emmc.load_os on the same eMMC."""
        rows = vectors('load_table.json')['rows']
        cmds = []
        devices = []
        for row in rows:
            c = self.row_emmc(row)
            devices.append(c)
            cmds += emmc_cmds(c, '%s_%s' % (row['p1'], row['p2'])) + ['boot']
        out = run('s03', cmds)
        for i, (row, c) in enumerate(zip(rows, devices)):
            res, ops = split(out[4 * i + 3])
            self.check_row(row, res, ops)
            want = emmc.load_os(c)
            self.assertEqual(res[0] == '0', want.ok, row['name'])
            if want.ok:
                self.assertEqual(int(res[1]), want.partition, row['name'])

    @scenario('S-03/SC-03-SELECT-ERR', 'S-03/SC-03-READ-ERR', 'S-03/SC-03-FINAL-SELECT-ERR')
    def test_SC_03_LOAD_FAULTS(self):
        """The failing selects and reads of SC-03-SELECT-ERR, SC-03-READ-ERR and SC-03-FINAL-SELECT-ERR:
        no OS, a select 0 attempted after the failure, the final select 0's failure reported; against
        S-03's model.load with the same failure."""
        rows = {r['name']: r for r in vectors('load_table.json')['rows']}
        cases = [('p1_good__p2_good', 'fail s 1 0 1 0', ('select1',)),
                 ('p1_inactive_good__p2_good', 'fail s 2 0 1 0', ('select2',)),
                 ('p1_good__p2_erased', 'readfail 1 5', ('read_image',)),
                 ('p1_good__p2_erased', 'readfail 1 0', ('read_header1',)),
                 ('p1_inactive_good__p2_good', 'readfail 2 0', ('read_header2',)),
                 ('p1_bad_crc__p2_erased', 'fail s 0 0 1 0', ('select0',)),
                 ('p1_good__p2_erased', 'fail s 0 0 1 0', ('select0',))]
        for name, fault, fail in cases:
            row = rows[name]
            c = self.row_emmc(row)
            out = run('s03', emmc_cmds(c, '%s_%s' % (row['p1'], row['p2'])) + ['fault ' + fault, 'boot'])
            res, ops = split(out[-1])
            want = M.load(c, fail=fail)
            self.assertEqual(want['outcome'], 'no_os')
            self.assertNotEqual(res[0], '0', (name, fault))
            self.assertTrue(ops[-1].startswith('s:0'), (name, fault, ops))
            self.assertEqual(res[3], '1' if fail == ('select0',) else '0', (name, fault))
            self.assertEqual([o.split(':error')[0] for o in ops if o.startswith('s:')][:-1],
                             ['s:%d' % s for s in want['selects'][:-1]], (name, fault, ops))

    @scenario('S-03/SC-03-ERASED-FF')
    def test_SC_03_ERASED_FF(self):
        """BP1's header 512 x 0xFF: active, fails its length check; BP2 is not tried."""
        row = {r['name']: r for r in vectors('load_table.json')['rows']}['p1_good__p2_good']
        c = self.row_emmc(row)
        out = run('s03', emmc_cmds(c, 'good_good') + ['poke 1 0 ' + 'ff' * 512, 'boot'])
        res, ops = split(out[-1])
        self.assertEqual(REASONS[int(res[0])], 'too_long')
        self.assertNotIn('s:2', ops)


class Write(unittest.TestCase):

    def faults(self, case):
        """The case's injected failure, corrupt sector or corrupt header, as fault commands."""
        cmds = []
        if case['fail']:
            ops = case['ops']
            j = next(i for i, o in enumerate(ops) if o[-1] == 'error')
            o = ops[j]
            nth = sum(1 for p in ops[:j] if p == o[:-1]) + 1
            kind = 'x' if o[0] == 'flush' else o[0][0]
            part = 0 if kind == 'x' else o[1]
            first = 32 if kind == 'x' else o[2] if kind in 'rew' else 0
            store = int(case['fail'] in ('write_header', 'write_header_active'))
            cmds.append('fault fail %s %d %d %d %d' % (kind, part, first, nth, store))
        if case['corrupt_sector'] is not None:
            cmds.append('fault corrupt %d %d 0 1' % (case['target'], case['corrupt_sector']))
        if case['corrupt_header']:
            cmds.append('fault corrupt %d 0 4 %d' % (case['target'], 1 if case['corrupt_header'] == 'inactive'
                                                     else 2))
        return cmds

    def setup_case(self, case):
        old = G.img(3, 16384)
        new = G.img(4, case['length']) if case['length'] >= 0x30 else bytes(case['length'])
        device = G.start_state(case['start'], old)
        return old, new, emmc_cmds(device, case['start']) + ['fault none'], save('new_%d' % case['length'], new)

    def supported(self, case):
        return case['deactivate'] == 'rewrite'

    @scenario('S-03/SC-03-WRITE-TABLE', 'S-03/SC-03-WRITE-P1', 'S-03/SC-03-WRITE-P2',
              'S-03/SC-03-WRITE-RECOVER', 'S-03/SC-03-WRITE-CORRUPT', 'S-03/SC-03-WRITE-HDR-CORRUPT')
    def test_SC_03_WRITE_TABLE(self):
        """Every case of write_sequences.json that the implementation's deactivation (the rewrite of
        S-03.058) applies to: the result, the target and the eMMC operations in order (selects, reads,
        erases and writes, a failed one marked), then the next boot's outcome (old, new or none)."""
        for case in vectors('write_sequences.json')['cases']:
            if not self.supported(case):
                continue
            old, new, cmds, path = self.setup_case(case)
            out = run('s03', cmds + self.faults(case) + ['write ' + path, 'fault none', 'boot'])
            res, ops = split(out[-3])
            what = case['name']
            self.assertEqual(res[0], {'ok': '0', 'refused': '1', 'failed': '2'}[case['result']], what)
            if case['result'] != 'refused':
                self.assertEqual(int(res[1]), case['target'] or 0, what)
            # A failed transfer may be a part of the reference's single op: compare it without its count.
            self.assertEqual([re.sub(r':\d+:error$', ':error', o) for o in merged(ops)],
                             [re.sub(r':\d+:error$', ':error', o) for o in ops_of(case['ops'])], what)
            if case['result'] != 'refused':
                bres, _ = split(out[-1])
                self.assertEqual(outcome(bres, old, new), case['final']['boots'], what)

    @scenario('S-03/SC-03-WRITE-CUT')
    def test_SC_03_WRITE_CUT(self):
        """Every cut of every write case (the rewrite deactivation): power cut after the k-th erase or
        write, then a boot; the outcome is the case's cuts[k-1].boots."""
        for case in vectors('write_sequences.json')['cases']:
            if not self.supported(case) or case['result'] == 'refused':
                continue
            old, new, cmds, path = self.setup_case(case)
            for cut in case['cuts']:
                out = run('s03', cmds + self.faults(case) + ['fault cut %d' % cut['after'], 'write ' + path,
                                                             'fault none', 'boot'])
                res, _ = split(out[-3])
                what = (case['name'], cut['after'])
                if cut['after'] < len([o for o in case['ops'] if o[0] in ('erase', 'write')]):
                    self.assertEqual(res, ['cut'], what)
                bres, _ = split(out[-1])
                self.assertEqual(outcome(bres, old, new), cut['boots'], what)

    @scenario('S-03/SC-03-WRITE-TORN')
    def test_SC_03_WRITE_TORN(self):
        """Every torn op of every write case (the rewrite deactivation): the k-th erase or write's range is
        stored as each of model.tear_variants of the bytes before it (the eMMC after a cut after op k-1)
        and after it (after a cut after op k), then a boot; the outcome is one of torn[k-1].boots, and old
        or new (new or none without an old image) for every case but corrupt_header_active."""
        for case in vectors('write_sequences.json')['cases']:
            if not self.supported(case) or case['result'] == 'refused' or not case.get('torn'):
                continue
            old, new, cmds, path = self.setup_case(case)
            start = G.start_state(case['start'], old)

            def written(cut):
                return cmds + self.faults(case) + ['fault cut %d' % cut, 'write ' + path, 'fault none']

            ranges = {}
            for t in case['torn']:
                op = t['op']
                if op[0] == 'erase':
                    a = op[2] * emmc.ERASE_GROUP * emmc.SECTOR
                    ranges[t['op_index']] = (op[1], a, min(emmc.ERASE_GROUP * emmc.SECTOR, start.B - a))
                else:
                    ranges[t['op_index']] = (op[1], op[2] * emmc.SECTOR, op[3] * emmc.SECTOR)
            probes = [(k, cut) for k in ranges for cut in (k - 1, k) if cut]
            probe = [written(cut) + ['peek %d %d %d' % ranges[k]] for k, cut in probes]
            out = run('s03', [x for c in probe for x in c])
            peeked, at = {}, 0
            for (k, cut), c in zip(probes, probe):
                at += len(c)
                peeked[k, cut] = bytes.fromhex(out[at - 1])
            boots = []
            for t in case['torn']:
                k = t['op_index']
                part, a, n = ranges[k]
                before = peeked[k, k - 1] if k > 1 else bytes(start.boot[part][a:a + n])
                for v in M.tear_variants(before, peeked[k, k]):
                    c = written(k)
                    if len(v) > emmc.SECTOR:
                        rest = v[emmc.SECTOR:]
                        c.append('pokefile %d %d %s' % (part, a + emmc.SECTOR,
                                                        save('torn_' + hashlib.sha256(rest).hexdigest()[:16],
                                                             rest)))
                    boots.append((t, c + ['poke %d %d %s' % (part, a, v[:emmc.SECTOR].hex()), 'boot']))
            out = run('s03', [x for _, c in boots for x in c])
            at = 0
            for t, c in boots:
                at += len(c)
                got = outcome(split(out[at - 1])[0], old, new)
                what = (case['name'], t['op_index'])
                self.assertIn(got, t['boots'], what)
                if case['name'] != 'corrupt_header_active':
                    self.assertIn(got, ('old', 'new') if case['boots_before'] == 'old' else ('new', 'none'),
                                  what)

    @scenario('S-03/SC-03-BOOT-SIZE')
    def test_SC_03_BOOT_SIZE(self):
        """An image of 0x400000 bytes: refused with no eMMC operation when B = 4 MiB, written and booted
        when B = 8 MiB (S-03.008, S-03.051); against emmc.acceptable."""
        new = G.img(5, 0x400000)
        path = save('new_400000', new)
        for b, ok in ((4 << 20, False), (8 << 20, True)):
            self.assertEqual(emmc.acceptable(len(new), b), ok)
            out = run('s03', ['bsize %d' % b, 'zero 1', 'zero 2', 'write ' + path, 'boot'])
            res, ops = split(out[3])
            if ok:
                self.assertEqual(res[0], '0')
                bres, _ = split(out[4])
                self.assertEqual(outcome(bres, b'', new), 'new')
            else:
                self.assertEqual((res[0], ops), ('1', []))


class Emmc(unittest.TestCase):

    @scenario('S-03/SC-03-EMMC-OTHER', 'S-03/SC-03-FACTORY-RESET')
    def test_SC_03_USER_AREA(self):
        """user_area.json: the refused and accepted eMMCs (S-03.002), the region map, the FAT volume map
        and the factory reset group, against images.emmc."""
        v = vectors('user_area.json')
        device = v['emmc']
        cmds = ['supported %d %d %d' % (r['sectors'], r['erase_group_sectors'], r['opt_write']) for r in device['refused']]
        cmds += ['supported %d %d %d' % (device['sectors'], device['erase_group_sectors'], w)
                 for w in device['accepted_opt_write']]
        edges = sorted({x for _, f, l in emmc.USER_LAYOUT for x in (f, l, l + 1, max(f - 1, 0))})
        cmds += ['region %d' % s for s in edges]
        cmds += ['fatmap %d' % m['volume_sector'] for m in v['fat_map']]
        out = run('s03', cmds)
        n = len(device['refused'])
        self.assertEqual(out[:n], ['0'] * n)
        self.assertEqual(out[n:n + len(device['accepted_opt_write'])], ['1'] * len(device['accepted_opt_write']))
        k = n + len(device['accepted_opt_write'])
        for s, line in zip(edges, out[k:k + len(edges)]):
            want = next((name for name, f, l in emmc.USER_LAYOUT if f <= s <= l), 'none')
            self.assertEqual(line, want, hex(s))
        for m, line in zip(v['fat_map'], out[k + len(edges):]):
            self.assertEqual(int(line, 16), m['user_sector'], m)
            self.assertEqual(emmc.fat_to_physical(m['volume_sector']), m['user_sector'])

    @scenario('S-03/SC-03-FEATURES', 'S-03/SC-03-POWER-OFF')
    def test_SC_03_SWITCHES(self):
        """The feature switches before the first user-area read (EXT_CSD[34] = 1, [33] = 1, [163] = 2) and
        the shutdown's order (FLUSH_CACHE 1, CACHE_CTRL 0, the unmount, BKOPS_EN 0, POWER_OFF_NOTIFICATION
        3, nothing after), also with the unmount failing."""
        out = run('s03', ['features', 'shutdown 0', 'shutdown 1'])
        _, ops = split(out[0])
        self.assertEqual(sorted(ops), sorted(['x:34:1', 'x:33:1', 'x:163:2']))
        for line in out[1:]:
            _, ops = split(line)
            self.assertEqual([o.split(':error')[0] for o in ops], ['x:32:1', 'x:33:0', 'u', 'x:163:0', 'x:34:3'])


    @scenario('S-03/SC-03-POWER-OFF-ERR')
    def test_SC_03_POWER_OFF_ERR(self):
        """One step answered with an error on every attempt, in turn each of the five: every later step is
        still made in SC-03-POWER-OFF's order, the failing switch is tried a bounded number of times
        (S-03.010's retries), and nothing follows the POWER_OFF_NOTIFICATION switch."""
        order = ['x:32:1', 'x:33:0', 'u', 'x:163:0', 'x:34:3']
        steps = [('x:32:1', 'shutdown 0 32'), ('x:33:0', 'shutdown 0 33'), ('u', 'shutdown 1'),
                 ('x:163:0', 'shutdown 0 163'), ('x:34:3', 'shutdown 0 34')]
        out = run('s03', [cmd for _, cmd in steps])
        for (step, _), line in zip(steps, out):
            res, ops = split(line)
            with self.subTest(step=step):
                self.assertEqual([o.split(':error')[0] for o in ops], order)
                self.assertEqual([o for o in ops if o.endswith(':error')], [step + ':error'])
                self.assertEqual(res[0], '1')
                if step != 'u':
                    self.assertTrue(1 <= int(res[1]) <= S03_RETRIES, res)


if __name__ == '__main__':
    unittest.main()
