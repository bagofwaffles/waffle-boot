# SPDX-License-Identifier: GPL-2.0-or-later
"""S-05 hand-off: src/s05_handoff.c (through h_s05) against tests/oracle/S-05-os-handoff (decision
tables, kernel_model, vectors) and streams built with images.ldr; outside steps are logged stubs."""

import json
import os
import random
import struct
import unittest
import zlib

from harness import BUILD, requirement, run, scenario, spec_module, spec_path, tsv

K = spec_module('S-05-os-handoff', 'kernel_model')
from images import ldr  # noqa: E402

WORK = os.path.join(BUILD, 'work')
KEYS = {'not_held': 0, 'held': 1, 'silent': 2}
# enum s05_refusal, and the keyword of kernel_model's Refused message for each.
REFUSAL = ['accept', 'length', 'parse', 'flags', 'space', 'staging', 'reserve', 'record', 'boot reserve']
KEYWORD = {'longer than the staging area': 'length', 'does not parse': 'parse', '(flags)': 'flags',
           '(space)': 'space', 'meets the staging area': 'staging', 'handover reserve': 'reserve',
           '(record)': 'record', '(boot reserve)': 'boot reserve'}
# enum s05_fail
NO_OS, ENCRYPTED, SELECT, REFUSED, SLAVE = 1, 2, 3, 4, 5
HB = 0x88A34D5C
CONFIG_ADDR = 0x88A34A00


def vec(name):
    return spec_path('S-05-os-handoff', 'vectors', name)


def vec_json(name):
    with open(vec(name), encoding='utf-8') as f:
        return json.load(f)


def save(name, data):
    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, 's05_%s.ldr' % name)
    with open(path, 'wb') as f:
        f.write(data)
    return path


def model_verdict(image):
    """'accept', or the reason keyword of kernel_model.compatibility_check's refusal."""
    try:
        K.compatibility_check(image)
    except K.Refused as e:
        for k, v in KEYWORD.items():
            if k in str(e):
                return v
        raise AssertionError('unclassified refusal: %s' % e)
    return 'accept'


# Addresses on and around the edges S-01.017 and S-05.024 name.
_MAP = K.check_stream.load_map()
EDGES = sorted({a for s, e in _MAP['spaces'].values() for a in (s, e)} |
               {a for s, e in _MAP['reserve'] for a in (s, e)} |
               set(_MAP['staging']) | set(_MAP['record_and_rom_stack']) |
               {0x04000000, 0x20000000, 0xFFFFFF00})


# Inside a space and clear of the reserve, the staging area and the record.
CLEAR = [_MAP['spaces']['l1_data_a'], (_MAP['spaces']['l2'][0], _MAP['reserve'][0][0]), (0x80000000, 0x88000000)]


def random_target(rng, count):
    """A target whose range starts or ends near an edge, or lies well inside a space."""
    e = rng.choice(EDGES)
    if rng.random() < 0.3:
        s, end = rng.choice(list(_MAP['spaces'].values()))
        return rng.randrange(s, max(s + 1, end - count)) & ~3
    if rng.random() < 0.5:
        return (e + rng.choice([-8, -4, 0, 4])) & 0xFFFFFFFF
    return (e - count + rng.choice([-4, 0, 4])) & 0xFFFFFFFF


def random_stream(rng, bad=1):
    """A stream shaped like an OS image with up to `bad` destinations chosen near edges (the rest well
    inside a space), sometimes a refused flag, sometimes broken. Returns (bytes, whether it may have two
    causes of refusal: two edge destinations, or a refused flag and a break)."""
    entry = 0x11A00000
    blocks = []
    nbad = 0
    flagged = False
    for _ in range(rng.randrange(1, 6)):
        count = rng.choice([0, 1, 3, 4, 0x40, 0x123])
        if nbad < bad and rng.random() < 0.5:
            target = random_target(rng, count)
            nbad += 1
        else:
            s, end = rng.choice(CLEAR)
            target = rng.randrange(s, end - 0x200) & ~3
        flags = 0
        r = rng.random()
        if r < 0.15:
            flags |= ldr.FILL
        elif r < 0.25:
            flags |= ldr.IGNORE
        if rng.random() < 0.08:
            flagged = True
            flags |= rng.choice([ldr.INIT, ldr.CALLBACK, ldr.INDIRECT, ldr.QUICKBOOT, ldr.AUX, ldr.SAVE])
        if flags & ldr.FILL:
            blocks.append({'flags': flags, 'target': target, 'count': count, 'fill': rng.getrandbits(32)})
        else:
            blocks.append({'flags': flags, 'target': target, 'payload': rng.randbytes(count)})
    tail = ldr.build_ldr(blocks + [{'flags': ldr.FINAL, 'target': entry, 'count': 0}])
    data = bytearray(ldr.header(ldr.FIRST | ldr.IGNORE, entry, 0, len(tail)) + tail)
    r = rng.random()
    if r < 0.05:
        data = data[:rng.randrange(len(data))]                          # truncated
    elif r < 0.1:
        data[rng.randrange(len(data))] ^= 1 << rng.randrange(8)         # a flipped bit
    return bytes(data), nbad > 1 or (flagged and r < 0.1)


def model_load(image, hb):
    """kernel_model.load as the h_s05 'load' line: calls, then sorted ranges (target, count, crc32)."""
    out = K.load(image, hb)
    calls = ['0x%x:0x%x:0x%x' % c for c in out['calls']]
    ranges = sorted((t, len(b), zlib.crc32(b)) for t, b in out['memory'].items())
    return calls, ranges, out['svect0']


def parse_load(line):
    calls, ranges, entry = (p.split() for p in line.split('|'))
    rs = sorted(tuple(int(x, 0) for x in r.split(':')) for r in ranges)
    return calls, rs, int(entry[1], 0) if entry[0] == 'entry' else None


def random_os(rng):
    """A loadable stream: data blocks of odd and even lengths, FILL blocks, zero-count blocks, at
    distinct targets."""
    regions, fills, used = [], [], set()
    for _ in range(rng.randrange(1, 8)):
        t = rng.randrange(0x80000000, 0x88000000, 0x10000) + rng.randrange(0, 0x100)
        if t in used:
            continue
        used.add(t)
        n = rng.choice([1, 2, 3, 5, 16, 0x101, 0x1000 + rng.randrange(64)])
        if rng.random() < 0.25:
            fills.append((t, n, rng.getrandbits(32)))
        else:
            regions.append((t, rng.randbytes(n)))
    data = ldr.simple_os(0x11A00000, regions, fills)
    if rng.random() < 0.5:                       # a zero-count data block before FINAL
        data = data[:-16] + ldr.header(0, 0x80001000, 0, 0) + data[-16:]
    return data


class Decision(unittest.TestCase):
    rows = tsv('S-05-os-handoff', 'boot_decision.tsv')

    def decide(self, slave=1):
        cmds = []
        for r in self.rows:
            reason = 20 if r['reason'] == 'none' else int(r['reason'])     # S-05.001: missing reads as 20
            cmds.append('decide %d %s %d %d' % (reason, r['state'], KEYS[r['keys']], slave))
        return [tuple(int(x) for x in line.split()) for line in run('s05', cmds)]

    @scenario('S-05/H-DEC')
    def test_H_DEC(self):
        """Every row: the OS boot exactly when the row says "boot"; and state_after, as far as the decision
        gives it: "written" (0) exactly for the rows the decision marks interrupted, whose one state-0 write
        is s11_interrupted()'s (test_s11.py, SC-11-INTERRUPTED), and "unchanged" for every other row, which
        the decision writes nothing for. Both against s11_model.interrupted_boot."""
        self.assertEqual(len(self.rows), 96)
        m11 = spec_module('S-11-packages', 's11_model')
        for r, (boot, interrupted) in zip(self.rows, self.decide()):
            self.assertEqual(bool(boot), r['decision'] == 'boot', r)
            value, how = r['state_after'].split()
            want = m11.interrupted_boot(int(r['state'], 16), r['keys'] != 'not_held')
            self.assertEqual(how, '(written)' if interrupted else '(unchanged)', r)
            self.assertEqual(int(value, 16), 0 if interrupted else int(r['state'], 16), r)
            self.assertEqual(int(value, 16), want['state_after'], r)
            self.assertEqual(bool(interrupted), want['notice'], r)

    @requirement('S-11.080')
    @scenario('S-05/H-INTERRUPTED')
    def test_H_INTERRUPTED(self):
        """The rows with state 0xAB0D, every reason and key case, with the slave present and absent: no OS
        boot, interrupted; no other row is. Then the same boot with the state written to 0, keys not held and
        reason 20: the OS boot. What the boot then writes and leaves (one metadata write, the record and the
        staging area unchanged) is s11_interrupted()'s, checked in test_s11.py; the notice is S-15's."""
        rows = [r for r in self.rows if int(r['state'], 16) == 0xAB0D]
        self.assertEqual(len(rows), 24)
        present = self.decide()
        for slave, got in ((1, present), (0, self.decide(slave=0))):
            for r, (boot, interrupted), (boot1, _) in zip(self.rows, got, present):
                want = int(r['state'], 16) == 0xAB0D
                self.assertEqual(interrupted, int(want), (r, slave))
                self.assertEqual(boot, 0 if want else boot1, (r, slave))   # S-09.008 makes the load
        self.assertEqual(run('s05', ['decide 20 0x0000 0 1']), ['1 0'])


class ImageCheck(unittest.TestCase):
    @scenario('S-05/F-REFUSE')
    def test_F_REFUSE_cases(self):
        cases = vec_json('compatibility_cases.json')['cases']
        out = run('s05', ['check %s' % vec(c['image']) for c in cases])
        for c, line in zip(cases, out):
            got = REFUSAL[int(line)]
            self.assertEqual(got != 'accept', c['refuse'], c)
            if c['refuse']:
                self.assertEqual(got, c['reason'], c)

    @scenario('S-05/F-REFUSE', 'S-05/F-LONG')
    def test_F_REFUSE_random_streams(self):
        rng = random.Random(5024)
        streams = [random_stream(rng, bad=rng.choice([0, 1, 1, 2])) for _ in range(600)]
        out = run('s05', ['check %s' % save('rand%d' % i, im) for i, (im, _) in enumerate(streams)])
        for i, ((im, two_causes), line) in enumerate(zip(streams, out)):
            got, want = REFUSAL[int(line)], model_verdict(im)
            self.assertEqual(got == 'accept', want == 'accept', (i, got, want))
            # S-05.024 orders no causes: a stream with two (two edge destinations, or a refused flag and a
            # break) may be refused for either.
            if not two_causes:
                self.assertEqual(got, want, (i, got, want))

    @scenario('S-05/F-LONG')
    def test_F_LONG(self):
        size = 0x800000
        fits = ldr.synthetic_app_of_size(size, entry=0x80000000)
        self.assertEqual(model_verdict(fits), 'accept')
        out = run('s05', ['check %s' % save('fits', fits), 'check zero:0x800001',
                          'check %s' % save('long', fits + b'\0')])
        self.assertEqual([REFUSAL[int(x)] for x in out], ['accept', 'length', 'length'])

    @requirement('S-05.024')
    def test_S05_024_walk(self):
        """s05_ldr_next against images.ldr.parse_blocks on the vectors and random streams."""
        rng = random.Random(5)
        images = [open(vec(n), 'rb').read() for n in ('synthetic_os_a.ldr', 'synthetic_os_b.ldr')]
        images += [random_stream(rng)[0] for _ in range(200)]
        out = run('s05', ['walk %s' % save('walk%d' % i, im) for i, im in enumerate(images)])
        for im, line in zip(images, out):
            try:
                want = ['0x%x:0x%x:0x%x:0x%x:0x%x' % (b['offset'], b['code'], b['target'], b['count'],
                                                     b['argument']) for b in ldr.parse_blocks(im)]
                want.append('end')
            except ldr.LdrError:
                want = None
            got = line.split()
            if want is None:
                self.assertEqual(got[-1], 'fail')
            else:
                self.assertEqual(got, want)


class KernelCall(unittest.TestCase):
    @scenario('S-05/H-CFG')
    def test_H_CFG(self):
        cfgv = vec_json('boot_config.json')
        size = int(cfgv['config_size'], 0)
        ph = int(cfgv['config_nonzero_fields']['pHeader'], 0)
        pl = int(cfgv['config_nonzero_fields']['bootRegistry.pLoadFunction'], 0)
        rng = random.Random(8)
        pairs = [(HB, 0x88A34E00)] + [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(20)]
        out = run('s05', ['cfg 0x%x 0x%x' % p for p in pairs])
        for (hb, lf), line in zip(pairs, out):
            want = bytearray(size)
            want[ph:ph + 4] = struct.pack('<I', hb)
            want[pl:pl + 4] = struct.pack('<I', lf)
            self.assertEqual(bytes.fromhex(line), bytes(want))

    @scenario('S-05/H-CALLS', 'S-05/H-MEM')
    def test_H_CALLS_vectors(self):
        for name in ('synthetic_os_a', 'synthetic_os_b'):
            v = vec_json(name + '.json')
            hb = int(v['header_buffer'], 0)
            line = run('s05', ['load %s 0x%x' % (vec(v['image']), hb)])[0]
            calls, ranges, entry = parse_load(line)
            want_calls = ['0x%x:0x%x:0x%x' % tuple(int(x, 0) for x in c) for c in v['calls']]
            self.assertEqual(calls, want_calls, name)
            self.assertEqual(entry, int(v['entry'], 0))
            image = open(vec(v['image']), 'rb').read()
            self.assertEqual(ranges, model_load(image, hb)[1])
            self.assertEqual(sorted((int(m['start'], 0), int(m['length'], 0)) for m in v['memory']),
                             [r[:2] for r in ranges])

    @scenario('S-05/H-CALLS', 'S-05/H-MEM', 'S-05/H-ENTRY')
    def test_H_CALLS_random(self):
        rng = random.Random(509)
        images = [random_os(rng) for _ in range(150)]
        hbs = [rng.choice([HB, 0x08034000 + 4 * rng.randrange(64)]) for _ in images]
        out = run('s05', ['load %s 0x%x' % (save('os%d' % i, im), hb)
                          for i, (im, hb) in enumerate(zip(images, hbs))])
        for im, hb, line in zip(images, hbs, out):
            self.assertEqual(parse_load(line), model_load(im, hb))


def ops(line):
    return line.split()[1:]


class Master(unittest.TestCase):
    os_a = vec('synthetic_os_a.ldr')

    @scenario('S-05/H-ORDER', 'S-05/H-ENTRY')
    def test_H_ORDER(self):
        n = len(K.load(open(self.os_a, 'rb').read(), HB)['calls'])
        out = run('s05', ['master 0 %s 0 %d' % (self.os_a, nz) for nz in (0, 1)])
        for line in out:
            self.assertEqual(ops(line), ['pre', 'stage', 'release', 'teardown',
                                         'kernel:0x%x:%d' % (CONFIG_ADDR, n), 'svect', 'jump:0x11a00000'])

    @scenario('S-05/F-NOOS', 'S-05/F-BADCRC', 'S-05/F-ENC', 'S-05/F-SELECT')
    def test_F_NOOS_stage_failed(self):
        cmds = ['master %d - 0 %d' % (cause, nz) for cause in (NO_OS, ENCRYPTED, SELECT) for nz in (0, 1)]
        out = run('s05', cmds)
        i = 0
        for cause in (NO_OS, ENCRYPTED, SELECT):
            for nz in (0, 1):
                # S-05.007: a failed select or read keeps the software version.
                want = ['pre', 'stage'] + (['clear'] if nz and cause != SELECT else []) + ['held:%d' % cause]
                self.assertEqual(ops(out[i]), want, cmds[i])
                i += 1

    @scenario('S-05/F-REFUSE', 'S-05/F-LONG')
    def test_F_REFUSE_master(self):
        cases = [c for c in vec_json('compatibility_cases.json')['cases'] if c['refuse']]
        images = [vec(c['image']) for c in cases] + ['zero:0x800001']
        out = run('s05', ['master 0 %s 0 1' % p for p in images])
        for p, line in zip(images, out):
            self.assertEqual(ops(line), ['pre', 'stage', 'clear', 'held:%d' % REFUSED], p)

    @scenario('S-05/SL-NOOS')
    def test_SL_NOOS_master(self):
        """S-05.021: a reply other than 0, or none, leaves the master in held mode with no teardown and no
        kernel call; S-05.006 lists no metadata write for this cause."""
        out = run('s05', ['master 0 %s %s 1' % (self.os_a, r) for r in ('1', '2', 'none')])
        for line in out:
            self.assertEqual(ops(line), ['pre', 'stage', 'release', 'held:%d' % SLAVE])


class Slave(unittest.TestCase):
    os_a = vec('synthetic_os_a.ldr')

    @scenario('S-05/SL-TEAR', 'S-05/SL-LOAD')
    def test_SL_TEAR(self):
        n = len(K.load(open(self.os_a, 'rb').read(), HB)['calls'])
        line = run('s05', ['slave 0 %s' % self.os_a])[0]
        self.assertEqual(ops(line), ['stage', 'reply:0', 'stop', 'teardown',
                                     'kernel:0x%x:%d' % (CONFIG_ADDR, n), 'svect', 'jump:0x11a00000',
                                     'ret:1'])

    @scenario('S-05/SL-NOOS', 'S-05/SL-LOAD')
    def test_SL_NOOS(self):
        refused = [vec(c['image']) for c in vec_json('compatibility_cases.json')['cases'] if c['refuse']]
        cmds = ['slave %d -' % c for c in (NO_OS, ENCRYPTED, SELECT)] + ['slave 0 %s' % p for p in refused]
        for cmd, line in zip(cmds, run('s05', cmds)):
            self.assertEqual(ops(line), ['stage', 'reply:1', 'ret:0'], cmd)


STORE = {'on': 1, 'off': 0, 'erased': 0}       # an erased store gives its default, off (S-02.097)


def boot_cmd(reason, state, keys, slave, store, prompt=0, stage=0, image=None, release='0', nz=0):
    """h_s05's boot line: boot_master (src/boot_flow.c) with its ops logged."""
    return 'boot %d %s %d %d %s %d %d %s %s %d' % (reason, state, keys, slave, store, prompt, stage,
                                                   image or Master.os_a, release, nz)


def handover_ops():
    """S-05.018's order on the master, as H-ORDER checks it."""
    n = len(K.load(open(Master.os_a, 'rb').read(), HB)['calls'])
    return ['pre', 'stage', 'release', 'teardown', 'kernel:0x%x:%d' % (CONFIG_ADDR, n), 'svect', 'jump:0x11a00000']


class StartInMenu(unittest.TestCase):
    rows = tsv('S-05-os-handoff', 'start_in_menu.tsv')

    @scenario('S-05/H-DEC')
    @requirement('S-05.001', 'S-15.175', 'S-05.025')
    def test_H_DEC_start_in_menu(self):
        """H-DEC's variant start_in_menu, the master's part: every row of start_in_menu.tsv that is a start,
        through boot_master with the setting as the running slot's settings sector gave it (an erased one gives off), the
        prompt rows with the prompt's op (digit 1). `boot`: the hand-off runs; `prompt`: the prompt, then the
        hand-off; `held_no_load`: held mode as a boot that attempted no load, with no prompt and no hand-off
        op; `held`: as with the setting off. The recovery rows are the front's, which never reads the store
        (test_s02.py SC-SETTINGS footprint)."""
        starts = [r for r in self.rows if r['case'] == 'start']
        self.assertEqual(len(starts) + 7, len(self.rows))
        cmds = []
        for r in starts:
            reason = 20 if r['reason'] == 'none' else int(r['reason'], 0)     # S-05.001: missing reads as 20
            cmds.append(boot_cmd(reason, r['state'], KEYS[r['keys']], r['slave'] == 'present', STORE[r['store']],
                                 prompt=1 if r['build'] == 'prompt' else 0))
        for r, line in zip(starts, run('s05', cmds)):
            got = ops(line)
            held = [x for x in got if x.startswith('held:')]
            what = r
            if r['outcome'] in ('boot', 'prompt'):
                self.assertEqual(got[got.index('pre'):], handover_ops(), what)
                self.assertEqual(held, [], what)
                self.assertEqual('prompt' in got, r['outcome'] == 'prompt', what)
                if r['reason'] == '80':                    # S-04.013: the setting is not asked
                    self.assertNotIn('store', got, what)
                else:
                    self.assertLess(got.index('store'), got.index('pre'), what)
            elif r['outcome'] == 'held_no_load':
                self.assertEqual(got, ['store', 'held:0:0:0:0'], what)
            else:
                self.assertEqual(r['outcome'], 'held', what)
                interrupted = int(r['state'], 16) == 0xAB0D
                self.assertEqual(got, ['held:0:0:%d:%d' % (interrupted, r['slave'] == 'absent')], what)


class ContinueToOS(unittest.TestCase):
    """S-05.033: the start after Continue to the OS's restart, through boot_master with reason 80. The
    restart itself is S-04's R-W11 (test_s04.py); the screen, the hold and the shutdown are the board's
    (status.tsv)."""

    @scenario('S-05/H-CONTINUE-NEXT')
    @requirement('S-05.033', 'S-04.013', 'S-05.001', 'S-05.006')
    def test_H_CONTINUE_NEXT(self):
        """With the setting on: reason 80 takes the boot path without asking the setting, where reason 20
        enters held mode with no load attempt; keys held, the state 0xAB0D and an absent slave still end in
        held mode; a load that fails ends in held mode with its cause, as on any start (`noos`); after a power
        cycle the record is absent, so reason 20 (`power_cycle`)."""
        cmds = [boot_cmd(80, '0x0000', 0, 1, 1), boot_cmd(20, '0x0000', 0, 1, 1),
                boot_cmd(80, '0x0000', 1, 1, 1), boot_cmd(80, '0xab0d', 0, 1, 1), boot_cmd(80, '0x0000', 0, 0, 1),
                boot_cmd(80, '0x0000', 0, 1, 1, stage=NO_OS, image='-', nz=1)]
        out = [ops(line) for line in run('s05', cmds)]
        self.assertEqual(out[0], handover_ops())
        self.assertEqual(out[1], ['store', 'held:0:0:0:0'])
        self.assertEqual(out[2], ['held:0:0:0:0'])
        self.assertEqual(out[3], ['held:0:0:1:0'])
        self.assertEqual(out[4], ['held:0:0:0:1'])
        self.assertEqual(out[5], ['pre', 'stage', 'clear', 'held:1:%d:0:0' % NO_OS])


if __name__ == '__main__':
    unittest.main()
