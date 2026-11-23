# SPDX-License-Identifier: GPL-2.0-or-later
"""S-02 SPI flash: src/s02_flash.c and src/s02_write.c (through h_s02) on images built by
images.spiflash, against its outcome checker and tests/oracle/S-02-spi-flash/vectors (part 'logic')."""

import csv
import hashlib
import json
import os
import random
import struct
import unittest
import zlib

from harness import BUILD, hx, requirement, run, scenario, spec_module, spec_path
from conformance.images import crc, ldr, spiflash, records

WORK = os.path.join(BUILD, 'work')


def vectors(name):
    return spec_path('S-02-spi-flash', 'vectors', name)


def table(name):
    with open(vectors(name), encoding='utf-8') as f:
        return list(csv.DictReader(f))


def meta_vectors():
    with open(vectors('metadata_writes.json'), encoding='utf-8') as f:
        return json.load(f)


def b(s):
    return bytes.fromhex(s.replace(' ', ''))


def save(name, data):
    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, name)
    with open(path, 'wb') as f:
        f.write(data)
    return path


def split(line):
    """'RESULT... | OPS...' -> (result fields, ops)."""
    res, _, ops = line.partition(' |')
    return res.split(), ops.split()


def writes_of(ops):
    """A reference sequence's erases and programs."""
    return [o for o in ops if o['op'] != 'read']


def writes(ops):
    return [o for o in ops if o[0] in 'ep']


def bootables():
    with open(vectors('bootable_images.json'), encoding='utf-8') as f:
        return json.load(f)['cases']


def bootable(n, seed):
    """An n-byte image that S-02.091 accepts: bootable_images.json's good slot stream, then random bytes
    after its FINAL block. Shorter than the stream it is that stream's first n bytes, which S-02.091
    refuses."""
    good = {c['name']: b(c['image']) for c in bootables()}['good']
    return (good + random.Random(seed).randbytes(max(n - len(good), 0)))[:n]


def padded(name, n):
    """bootable_images.json's case `name` followed by its `padding` byte up to n bytes (S-02.044: an image
    named only by its size is a bootable stream padded after its FINAL block); n = 0 is the empty image."""
    with open(vectors('bootable_images.json'), encoding='utf-8') as f:
        pad = int(json.load(f)['padding']['byte'], 16)
    img = {c['name']: b(c['image']) for c in bootables()}[name]
    return (img + bytes([pad]) * max(n - len(img), 0))[:n]


class FirstStage(unittest.TestCase):

    @scenario('S-02/SC-FS-WRITES', 'S-01/M2')
    def test_SC_FS_WRITES(self):
        """Every row of first_stage_table.csv (every selector x record state), two resets each, against the
        conforming columns: the slot booted and its boot address (S-01 M2: 0x40000000 + the slot's offset),
        the selector after, the footprint (at most one erase at 0xD000 and one 2-byte program there, only
        when `writes` is 1), the slots checked in order, and the second reset. Where
        images.spiflash.expected_boot_slot(profile='conforming') gives a definite outcome for an unverified
        selector, the result must also be one it allows."""
        rows = table('first_stage_table.csv')
        paths, cmds = {}, []
        for r in rows:
            key = (r['slot0'], r['slot1'])
            if key not in paths:
                paths[key] = save('fs_%s_%s.bin' % key, spiflash.build_flash(selector=(0, 0), slots=key))
            cmds += ['load ' + paths[key], 'poke 0xd000 ' + r['selector'].replace(' ', ''),
                     'fs 0', 'peek 0xd000 2', 'fs 0', 'peek 0xd000 2']
        out = run('s02', cmds)
        images = {k: bytearray(spiflash.build_flash(selector=(0, 0), slots=k)) for k in paths}
        for i, r in enumerate(rows):
            what = (r['selector'], r['slot0'], r['slot1'])
            res, ops = split(out[6 * i + 2])
            boot, base, wrote, _sel, _checked, _good, status = res
            self.assertEqual(status, '0', what)
            self.assertEqual(boot, r['conforming_boot'], what)
            self.assertEqual(int(base, 16), spiflash.BOOT_BASE + spiflash.SLOT_OFF[int(boot)], what)
            self.assertEqual(out[6 * i + 3], r['conforming_sel_after'].replace(' ', ''), what)
            self.assertEqual(wrote, r['conforming_writes'], what)
            self.assertEqual(writes(ops), ['e:d000', 'p:d000:2'] if r['conforming_writes'] == '1' else [], what)
            recs = [{'r:e000:c': 'slot0', 'r:f000:c': 'slot1'}[o] for o in ops if o in ('r:e000:c', 'r:f000:c')]
            self.assertEqual('+'.join(recs), r['conforming_checks'], what)
            res2, _ = split(out[6 * i + 4])
            self.assertEqual(res2[0], r['conforming_reset2_boot'], what)
            self.assertEqual(out[6 * i + 5], r['conforming_reset2_sel_after'].replace(' ', ''), what)
            sel = bytes.fromhex(r['selector'])
            if sel[1] != 1:
                img = images[(r['slot0'], r['slot1'])]
                img[0xD000:0xD002] = sel
                want = spiflash.expected_boot_slot(bytes(img), 'conforming')
                if want['boot'] != 'open':
                    self.assertEqual(int(boot), want['boot'], what)
                    self.assertIn(bytes.fromhex(out[6 * i + 3]), want['selector_after'], what)

    @requirement('S-02.036')
    def test_S02_036_read_faults(self):
        """Every row of first_stage_read_faults.csv: the reads the row names fail (the selector's sector; a
        slot's record sector and contents), and the reset boots, checks, writes and leaves the selector as
        the row says; fault_seen is whether a failing read was made."""
        areas = {'selector': [(records.SELECTOR_OFF, records.SECTOR)]}
        for i in (0, 1):
            areas['slot%d' % i] = [(records.SLOT_RECORD_OFF[i], records.SECTOR),
                                   (records.SLOT_OFF[i], records.SLOT_MAX)]
        rows = table('first_stage_read_faults.csv')
        self.assertTrue(rows)
        paths, cmds = {}, []
        for r in rows:
            key = (r['slot0'], r['slot1'])
            if key not in paths:
                paths[key] = save('fs_%s_%s.bin' % key, spiflash.build_flash(selector=(0, 0), slots=key))
            ranges = [x for name in r['faults'].split('+') for a in areas[name] for x in a]
            cmds += ['load ' + paths[key], 'poke 0xd000 ' + r['selector'].replace(' ', ''),
                     'readfail ' + ' '.join('0x%x' % x for x in ranges), 'fs 0', 'readfail', 'readfail -',
                     'peek 0xd000 2']
        out = run('s02', cmds)
        for i, r in enumerate(rows):
            what = (r['selector'], r['slot0'], r['slot1'], r['faults'])
            o = out[7 * i:7 * i + 7]
            res, ops = split(o[3])
            boot, _base, wrote, *_ = res
            with self.subTest(row=what):
                self.assertEqual(int(int(o[4]) > 0), int(r['fault_seen']))
                self.assertEqual(boot, r['boot'])
                self.assertEqual(wrote, r['writes'])
                self.assertEqual(writes(ops), ['e:d000', 'p:d000:2'] if r['writes'] == '1' else [])
                recs = [{'r:e000:c': 'slot0', 'r:f000:c': 'slot1'}[x] for x in ops if x in ('r:e000:c', 'r:f000:c')]
                self.assertEqual('+'.join(recs), r['checks'])
                self.assertEqual(o[6], r['sel_after'].replace(' ', ''))


class Metadata(unittest.TestCase):

    def start(self, state):
        """The start states of metadata_cuts.csv: the old record with the tail bytes at 0xC000, the mirror
        sector erased (mirror_absent), a mirror of the old record (mirror_old, stale), of the new one
        (pending_old, pending_erased: then 0xC000 holds the old record, or is erased); written_then_other: the
        mirror sector erased, as a completed write leaves it, and another writer's record at 0xC000."""
        v = meta_vectors()
        old, new, other = b(v['old_record']), b(v['new_record']), b(v['other_writer_record'])
        img = bytearray(b'\xff' * spiflash.FLASH_SIZE)
        sec = bytearray(b'\xff' * spiflash.SECTOR)
        sec[:0x34] = other if state in ('stale', 'written_then_other') else old
        for t in v['tail']:
            o = int(t['offset'], 16)
            tb = b(t['bytes'])
            sec[o:o + len(tb)] = tb
        if state == 'pending_erased':
            sec = bytearray(b'\xff' * spiflash.SECTOR)
        img[0xC000:0xD000] = sec
        if state in ('mirror_old', 'stale'):
            img[0xB000:0xB068] = spiflash.mirror_encode(old)
        elif state in ('pending_old', 'pending_erased'):
            img[0xB000:0xB068] = spiflash.mirror_encode(new)
        return bytes(img)

    def classify(self, rec):
        v = meta_vectors()
        for name in ('old_record', 'new_record', 'other_writer_record'):
            if rec == b(v[name]):
                return {'old_record': 'old', 'new_record': 'new', 'other_writer_record': 'other_writer'}[name]
        return 'absent' if rec[:4] != records.METADATA_MAGIC.to_bytes(4, 'little') else 'corrupt'

    @scenario('S-02/SC-META-WRITE')
    def test_SC_META_WRITE(self):
        """From both start states: the erases and programs are S-02.064's (write_sequences 'conforming'),
        the mirror is read back over 0xB000-0xB068 before 0xC000 is erased and 0xC000-0xD000 is read back
        after its last program; the flash after equals images.spiflash.write_metadata_record's on the same
        start; only sw_version (+0x1A..+0x21) changes in 0xC000-0xD000; the 0xC000 sector is read twice
        before the first erase; the mirror sector is erased last (S-02.064 step 7)."""
        v = meta_vectors()
        new = b(v['new_record'])
        ref = [o for o in v['write_sequences']['conforming'] if o['op'] != 'read']
        want_ops = ['e:%x' % int(o['offset'], 16) if o['op'] == 'erase' else
                    'p:%x:%x' % (int(o['offset'], 16), o['length']) for o in ref]
        for state in ('mirror_absent', 'mirror_old'):
            start = self.start(state)
            out = run('s02', ['load ' + save('meta.bin', start), 'metawrite ' + new.hex(),
                              'save ' + os.path.join(WORK, 'meta_after.bin')])
            res, ops = split(out[1])
            self.assertEqual(res, ['0'], state)
            self.assertEqual(writes(ops), want_ops, state)
            erase_c = ops.index('e:c000')

            def covered(seg, lo, hi):
                got = set()
                for o in seg:
                    if o.startswith('r:'):
                        _, off, ln = o.split(':')
                        got.update(range(int(off, 16), int(off, 16) + int(ln, 16)))
                return set(range(lo, hi)) <= got

            self.assertTrue(covered(ops[ops.index('p:b000:68'):erase_c], 0xB000, 0xB068), ops)
            last_p = max(i for i, o in enumerate(ops) if o.startswith('p:c'))
            self.assertTrue(covered(ops[last_p:], 0xC000, 0xD000), ops)
            with open(os.path.join(WORK, 'meta_after.bin'), 'rb') as f:
                after = f.read()
            fl = spiflash.Flash(start)
            self.assertTrue(spiflash.write_metadata_record(fl, new))
            self.assertEqual(after, fl.bytes(), state)
            for i in range(0xC000, 0xD000):
                if not 0xC01A <= i < 0xC022:
                    self.assertEqual(after[i], start[i], (state, hex(i)))
            self.assertEqual(after[0xB000:0xC000], b'\xff' * 0x1000)
            first_erase = next(i for i, o in enumerate(ops) if o.startswith('e:'))
            self.assertTrue(covered(ops[1:first_erase], 0xC000, 0xD000), ops)
            self.assertEqual(writes(ops)[-1], 'e:b000', state)

    def mask(self, pattern, n):
        kind, arg = pattern.split(':')
        if kind == 'seed':
            rnd = random.Random(int(arg))
            return bytes(rnd.getrandbits(8) for _ in range(n))
        if kind == 'bit':
            i0, k = (int(x) for x in arg.split('.'))
            return bytes((1 << k) if i == i0 else 0 for i in range(n))
        lo, hi = (int(x) for x in arg.split('-'))
        inside = 0xFF if kind == 'ones' else 0
        return bytes(inside if lo <= i < hi else 0xFF - inside for i in range(n))

    @scenario('S-02/SC-META-CUT')
    def test_SC_META_CUT(self):
        """Every row of metadata_cuts.csv with rule `conforming`: the write (or the first boot's restore)
        cut after op n or torn inside op n by the row's pattern, then a boot's restore. The restore happens
        exactly when the row says, and the record at 0xC000 after it is the row's `result` (old, new), with
        the tail bytes kept as the row says. The reference sequence's op n maps to the implementation's
        write op count, reads aside (a read changes nothing)."""
        v = meta_vectors()
        new = b(v['new_record'])
        rows = [r for r in table('metadata_cuts.csv') if r['rule'] == 'conforming']
        self.assertTrue(rows)
        for r in rows:
            what = (r['start'], r['phase'], r['op_n'], r['kind'], r['pattern'])
            start = self.start(r['start'])
            if r['phase'] == 'write':
                seq = v['write_sequences']['conforming']
                cmd = 'metawrite ' + new.hex()
            else:
                seq = v['restore_sequences'].get(r['start'], [])
                cmd = 'restore'
            n = int(r['op_n'])
            nwrites = sum(1 for o in seq[:n] if o['op'] != 'read')
            cmds = ['load ' + save('cut.bin', start)]
            if r['kind'] == 'after':
                cmds += ['fault after %d' % nwrites, cmd]
            elif r['kind'] == 'torn':
                op = seq[n - 1]
                cmds += ['fault tear %d %s' % (nwrites, self.mask(r['pattern'], op['length']).hex()), cmd]
            cmds += ['restore', 'peek 0xc000 0x1000']
            out = run('s02', cmds)
            res, ops = split(out[-2])
            self.assertEqual(res[0], '0', what)
            self.assertEqual(res[1], '1' if r['restored'] == 'yes' else '0', what)
            self.assertFalse([o for o in writes(ops) if o.startswith(('e:b', 'p:b'))], what)
            sec = bytes.fromhex(out[-1])
            self.assertEqual(self.classify(sec[:0x34]), r['result'], what)
            tail = all(sec[int(t['offset'], 16):int(t['offset'], 16) + len(b(t['bytes']))] == b(t['bytes'])
                       for t in v['tail'])
            self.assertEqual(tail, r['tail_kept'] == 'yes', what)


def cut_sequences():
    with open(vectors('powercut_sequences.json'), encoding='utf-8') as f:
        return json.load(f)


def ref_writes(ops):
    """A reference sequence's erases and programs in h_s02's notation."""
    out = []
    for o in ops:
        off = int(o['offset'], 16)
        if o['op'] == 'erase':
            out.append('e:%x' % off)
        elif o['op'] == 'program':
            out.append('p:%x:%x' % (off, o['length']))
    return out


def covered(ops, start, end):
    """True if the reads among ops cover [start, end)."""
    got = sorted((int(o.split(':')[1], 16), int(o.split(':')[1], 16) + int(o.split(':')[2], 16))
                 for o in ops if o.startswith('r:'))
    at = start
    for s, e in got:
        if s <= at < e:
            at = e
    return at >= end


class Upgrade(unittest.TestCase):
    """The bootloader-slot writer (s02_slot_upgrade) from the start states of powercut_sequences.json, with
    the conforming rule: its erases and programs against the reference sequence, then the first stage."""

    seqs = {s['id']: s for s in cut_sequences()['sequences'] if s['writer_rule'] == 'conforming'}

    def start(self, seq):
        st = seq['start']
        sel = tuple(bytes.fromhex(st['selector']))
        return spiflash.build_flash(selector=sel, slots=(st['slot0'], st['slot1']))

    def image(self, n, seed=2):
        return bootable(n, seed)

    @scenario('S-02/SC-UPG-OK')
    def test_SC_UPG_OK(self):
        img = save('upg_b.bin', self.image(cut_sequences()['image_length']))
        ids = [i for i, s in self.seqs.items() if s['start_allowed']]
        self.assertEqual(ids, ['C-01', 'C-02', 'C-03', 'C-04', 'C-08', 'C-09'])
        for i in ids:
            seq = self.seqs[i]
            t = seq['target']
            out = run('s02', ['load ' + save('upg_start.bin', self.start(seq)), 'upgrade 1 ' + img,
                              'fs 0', 'peek 0xd000 2'])
            res, ops = split(out[1])
            self.assertEqual(res, ['0', str(t)], i)
            self.assertEqual(writes(ops), ref_writes(seq['ops']), i)
            # 4: read-back of the record and the image between the record program and the selector erase
            rec = spiflash.SLOT_RECORD_OFF[t]
            p = ops.index('p:%x:c' % rec)
            e = ops.index('e:d000')
            self.assertTrue(covered(ops[p + 1:e], rec, rec + 12), i)
            self.assertTrue(covered(ops[p + 1:e], spiflash.SLOT_OFF[t], spiflash.SLOT_OFF[t] + seq['image_length']), i)
            self.assertEqual(split(out[2])[0][0], str(t), i)
            self.assertEqual(out[3], '%02x01' % t, i)

    @scenario('S-02/SC-UPG-START')
    def test_SC_UPG_START(self):
        img = save('upg_b.bin', self.image(cut_sequences()['image_length']))
        refused = [i for i, s in self.seqs.items() if not s['start_allowed']]
        self.assertEqual(refused, ['C-05', 'C-06', 'C-07', 'C-10', 'C-11', 'C-12', 'C-13'])
        for i in refused:
            out = run('s02', ['load ' + save('upg_start.bin', self.start(self.seqs[i])), 'upgrade 1 ' + img,
                              'upgrade 1 ' + img])
            for line in out[1:]:
                res, ops = split(line)
                self.assertEqual(res[0], '5', i)                      # S02_ERR_START
                self.assertEqual(writes(ops), [], i)
        # S-02.055, S-02.057: the option off skips the step and touches nothing
        out = run('s02', ['load ' + save('upg_start.bin', self.start(self.seqs['C-02'])), 'upgrade 0 ' + img])
        self.assertEqual(split(out[1]), (['1', '2'], []))

    @scenario('S-02/SC-UPG-SIZE')
    def test_SC_UPG_SIZE(self):
        start = save('upg_start.bin', self.start(self.seqs['C-02']))
        small = next(c['length'] for c in bootables() if c['name'] == 'small_slot')
        images = ((0, b''), (1, padded('one_byte', 1)), (small, padded('small_slot', small)),
                  (spiflash.SLOT_MAX, padded('small_slot', spiflash.SLOT_MAX)),
                  (spiflash.SLOT_MAX + 1, padded('small_slot', spiflash.SLOT_MAX + 1)))
        for n, img in images:
            out = run('s02', ['load ' + start, 'upgrade 1 ' + save('upg_n.bin', img), 'fs 0'])
            res, ops = split(out[1])
            if n in (0, spiflash.SLOT_MAX + 1):
                self.assertEqual((res[0], writes(ops)), ('3', []), n)    # S02_ERR_SIZE
            elif n == 1:
                # S-02.044, S-02.091: no block header fits, so Bad file before any erase or program
                self.assertEqual((res[0], writes(ops)), ('7', []), n)    # S02_ERR_STREAM
            else:
                self.assertEqual(res, ['0', '0'], n)
                sector_erases = [o for o in ops if o.startswith('e:') and
                                 spiflash.SLOT_OFF[0] <= int(o[2:], 16) < spiflash.SLOT_OFF[0] + spiflash.SLOT_MAX]
                # S-02.045: the image's sectors, and the last sector when the image leaves it free; then the
                # boot logo's header sector (src/front/fr_handoff.h) and S-02.103's settings sector when the
                # image leaves them free too
                last = n <= spiflash.SLOT_MAX - 0x1000
                settings = n <= spiflash.SLOT_MAX - 0x2000
                logo = n <= spiflash.SLOT_MAX - 0x3000
                self.assertEqual(len(sector_erases), -(-n // 0x1000) + last + logo + settings, n)
                tail = ['e:%x' % (spiflash.SLOT_OFF[0] + spiflash.SLOT_MAX - 0x1000)] if last else []
                tail += ['e:%x' % (spiflash.SLOT_OFF[0] + spiflash.SLOT_MAX - 0x3000)] if logo else []
                tail += ['e:%x' % (spiflash.SLOT_OFF[0] + spiflash.SLOT_MAX - 0x2000)] if settings else []
                if tail:
                    self.assertEqual(sector_erases[-len(tail):], tail, n)
                self.assertEqual(split(out[2])[0][0], '0', n)

    @scenario('S-02/SC-UPG-STREAM')
    @requirement('S-02.091')
    def test_SC_UPG_STREAM(self):
        """Every case of bootable_images.json (each a slot image) offered to the slot writer from selector
        01 01 with both slots good; refused with no erase or program and Bad file (S02_ERR_STREAM), or
        installed."""
        slot = save('upg_start.bin', self.start(self.seqs['C-02']))
        cases = bootables()
        cmds = []
        for c in cases:
            self.assertEqual(c['kind'], 'slot', c['name'])
            img = save('boot_%s.bin' % c['name'], b(c['image']))
            cmds += ['load ' + slot, 'upgrade 1 ' + img, 'fs 0']
        out = run('s02', cmds)
        for k, c in enumerate(cases):
            res, ops = split(out[3 * k + 1])
            if not c['accepted']:
                self.assertEqual((res[0], writes(ops)), ('7', []), c['name'])
            else:
                self.assertEqual(res, ['0', '0'], c['name'])
                self.assertEqual(split(out[3 * k + 2])[0][0], '0', c['name'])

    @scenario('S-02/SC-UPG-FAULT')
    def test_SC_UPG_FAULT_fail_op(self):
        """Cases 1, 2 and 4 (fail_op on the first slot erase, a mid-slot program, the record program), and the
        erase of the slot's last sector (S-02.045); the corrupt_program cases 3 and 5 need a program that stores
        other bits and report success, which h_s02's faults do not model."""
        seq = self.seqs['C-02']
        img = save('upg_b.bin', self.image(seq['image_length']))
        ref = ref_writes(seq['ops'])
        cases = {'first slot erase': 1, 'mid-slot program': ref.index('p:%x:100' % (spiflash.SLOT_OFF[0] + 0x800)) + 1,
                 'record program': ref.index('p:%x:c' % spiflash.SLOT_RECORD_OFF[0]) + 1,
                 'last sector erase': ref.index('e:%x' % (spiflash.SLOT_OFF[0] + spiflash.SLOT_MAX - 0x1000)) + 1}
        for what, w in cases.items():
            out = run('s02', ['load ' + save('upg_start.bin', self.start(seq)), 'fault fail %d' % w,
                              'upgrade 1 ' + img, 'peek 0xd000 2', 'fs 0'])
            res, ops = split(out[2])
            self.assertNotEqual(res[0], '0', what)
            self.assertFalse([o for o in writes(ops) if o.startswith(('e:d000', 'p:d000'))], what)
            self.assertFalse([o for o in writes(ops) if int(o.split(':')[1], 16) < 0xD000 and
                              not int(o.split(':')[1], 16) >> 12 in (0xB, 0xC)], what)
            self.assertFalse([o for o in writes(ops) if spiflash.SLOT_OFF[1] <= int(o.split(':')[1], 16) <
                              spiflash.SLOT_OFF[1] + spiflash.SLOT_MAX or
                              int(o.split(':')[1], 16) == spiflash.SLOT_RECORD_OFF[1]], what)
            self.assertEqual(out[3], '0101', what)
            self.assertEqual(split(out[4])[0][0], '1', what)

    @scenario('S-02/SC-UPG-CUT')
    def test_SC_UPG_CUT(self):
        """powercut_cuts.csv for the conforming sequences: every 'none' (a cut before op 1) and 'after' row, and the torn rows of
        slot erases and programs (three seeds), each followed by three resets whose slots must be the row's
        conforming_boots. The torn record, readback and selector rows are test_SC_UPG_CUT_torn_records. The
        reference op n maps to the implementation's write op count, reads aside."""
        rows = [r for r in table('powercut_cuts.csv') if r['sequence'] in self.seqs
                and self.seqs[r['sequence']]['start_allowed']
                and (r['kind'] in ('none', 'after') or r['op_class'] in ('slot_erase', 'slot_program'))]
        img = save('upg_b.bin', self.image(cut_sequences()['image_length']))
        starts = {i: save('upg_%s.bin' % i, self.start(s)) for i, s in self.seqs.items()}
        cmds, keys = [], []
        for r in rows:
            seq = self.seqs[r['sequence']]
            n = int(r['op_n'])
            w = sum(1 for o in seq['ops'][:n] if o['op'] != 'read')
            if r['kind'] == 'none':                   # op 0: cut before the first op, the start state
                faults = [None]
            elif r['kind'] == 'after':
                faults = ['fault after %d' % w]
            else:
                length = seq['ops'][n - 1]['length']
                faults = ['fault tear %d %s' % (w, random.Random(k).randbytes(length).hex()) for k in range(3)]
            for f in faults:
                run_it = [f, 'upgrade 1 ' + img] if f else ['fault none', 'fault none']
                cmds += ['load ' + starts[r['sequence']]] + run_it + ['fs 0', 'fs 0', 'fs 0']
                keys.append(r)
        self.assertGreater(len(keys), 300)
        out = run('s02', cmds)
        for k, r in enumerate(keys):
            what = (r['sequence'], r['op_n'], r['kind'])
            seq = self.seqs[r['sequence']]
            # a cut after the last write, the selector's read-back (S-02.048) aside: the upgrade is done
            last = all(o['op'] == 'read' for o in seq['ops'][int(r['op_n']):])
            if r['kind'] == 'after' and last:
                self.assertEqual(split(out[6 * k + 2])[0], ['0', str(seq['target'])], what)
            elif r['kind'] != 'none':
                self.assertEqual(split(out[6 * k + 2])[0], ['cut'], what)
            boots = ' '.join(split(out[6 * k + 3 + j])[0][0] for j in range(3))
            self.assertEqual(boots, r['conforming_boots'], what)


    TORN_OPS = ('record_erase', 'record_program', 'readback', 'selector_erase', 'selector_program')

    @scenario('S-02/SC-UPG-CUT')
    def test_SC_UPG_CUT_torn_records(self):
        """powercut_cuts.csv's torn record, readback and selector rows, which the table states by an
        assumption (a torn record is badtag, a torn selector a superset_of value) or as 'varies'. A torn
        readback is the cut after the op before it: the row's conforming_boots. A torn erase or program
        is run with three seeds and the masks 00 and FF; the selector and the target's record it leaves are
        read back and the slots classified (images.spiflash.slot_state, the image as the finished upgrade
        leaves it). The three resets then boot what S-02's conforming profile boots on that state
        (gen_vectors.run_resets); a row whose conforming_safe is True is safe on it (gen_vectors.outcome:
        every reset boots a good slot, or with S-02.052's precondition false, every reset that booted one
        without the upgrade); a torn selector holds one of the row's superset_of values, a torn record
        leaves the row's selector; and a torn record classified badtag boots the row's conforming_boots."""
        G = spec_module('S-02-spi-flash', 'gen_vectors')
        rows = [r for r in table('powercut_cuts.csv') if r['sequence'] in self.seqs
                and self.seqs[r['sequence']]['start_allowed'] and r['kind'] == 'torn'
                and r['op_class'] in self.TORN_OPS]
        image = self.image(cut_sequences()['image_length'])
        img = save('upg_b.bin', image)
        starts = {i: save('upg_%s.bin' % i, self.start(s)) for i, s in self.seqs.items()}
        sel_off = spiflash.SELECTOR_OFF
        runs = []
        for r in rows:
            seq = self.seqs[r['sequence']]
            n = int(r['op_n'])
            w = sum(1 for o in seq['ops'][:n] if o['op'] != 'read')
            rec = spiflash.SLOT_RECORD_OFF[seq['target']]
            if r['op_class'] == 'readback':
                faults = ['fault after %d' % w]
            else:
                length = seq['ops'][n - 1]['length']
                faults = ['fault tear %d %s' % (w, m.hex()) for m in
                          [random.Random(k).randbytes(length) for k in range(3)] +
                          [bytes(length), b'\xff' * length]]
            for f in faults:
                runs.append((r, ['load ' + starts[r['sequence']], f, 'upgrade 1 ' + img,
                                 'peek 0x%x 2' % sel_off, 'peek 0x%x 12' % rec, 'fs 0', 'fs 0', 'fs 0']))
        self.assertGreater(len(runs), 100)
        out = run('s02', [x for _, c in runs for x in c])
        done = {}
        for k, (r, _) in enumerate(runs):
            seq = self.seqs[r['sequence']]
            t = seq['target']
            what = (r['sequence'], r['op_n'], r['op_class'], k)
            o = out[8 * k:8 * k + 8]
            self.assertEqual(split(o[2])[0], ['cut'], what)
            sel = bytes.fromhex(o[3])
            boots = [int(split(x)[0][0]) for x in o[5:]]
            if r['op_class'] == 'readback':
                self.assertEqual(' '.join(map(str, boots)), r['conforming_boots'], what)
                continue
            if r['sequence'] not in done:                 # the flash the finished upgrade leaves
                d = bytearray(spiflash.build_flash(
                    selector=tuple(bytes.fromhex(seq['start']['selector'])),
                    slots=(seq['start']['slot0'], seq['start']['slot1'])))
                d[spiflash.SLOT_OFF[t]:spiflash.SLOT_OFF[t] + len(image)] = image
                done[r['sequence']] = d
            flash = bytearray(done[r['sequence']])
            flash[spiflash.SLOT_RECORD_OFF[t]:spiflash.SLOT_RECORD_OFF[t] + 12] = bytes.fromhex(o[4])
            slots = tuple(spiflash.slot_state(flash, s) for s in (0, 1))
            self.assertEqual(boots, G.run_resets('conforming', tuple(sel), slots), (what, sel.hex(), slots))
            if r['conforming_safe'] == 'True':
                start_sel = tuple(b(seq['start']['selector']))
                start_slots = (seq['start']['slot0'], seq['start']['slot1'])
                base = G.boots_good(G.run_resets('conforming', start_sel, start_slots), start_slots)
                self.assertIs(G.outcome('conforming', tuple(sel), slots, base, seq['precondition'])[0], True,
                              (what, sel.hex(), slots))
            if r['op_class'].startswith('selector_'):
                self.assertIn(tuple(sel), G.superset_values(tuple(b(r['selector'].split(' ', 1)[1]))), what)
            else:
                self.assertEqual(sel, b(r['selector']), what)
                if slots[t] == 'badtag':
                    self.assertEqual(' '.join(map(str, boots)), r['conforming_boots'], what)


# ---- recovery (S-02.084, S-02.093) ----

HELLO, INFO, BEGIN, DATA, END, RESTART = 1, 2, 3, 4, 5, 6
PH_RECEIVING, PH_DONE, PH_FAILED = 1, 3, 4


def wr(t, seq, payload=b''):
    """One S-02.093 frame."""
    body = bytes([t, seq & 0xFF]) + struct.pack('<H', len(payload)) + payload
    return b'WR' + body + struct.pack('<I', crc.crc32(body))


def wr_parse(data):
    """The frames in data: [(type, seq, payload)], each checked whole."""
    out, at = [], 0
    while at < len(data):
        assert data[at:at + 2] == b'WR', data[at:at + 8].hex()
        t, seq, n = data[at + 2], data[at + 3], struct.unpack_from('<H', data, at + 4)[0]
        body = data[at + 2:at + 6 + n]
        assert struct.unpack_from('<I', data, at + 6 + n)[0] == crc.crc32(body)
        out.append((t, seq, data[at + 6:at + 6 + n]))
        at += 10 + n
    return out


def front_image(n, seed=11, version=(1, 2, 3, 4), front_seed=5):
    """A slot image with a waffle-boot front (S-01.023): an INIT block in the front's L1 code, a front block in
    its L2, the descriptor block, one main region, FIRST and FINAL at the front's entry; random bytes after
    FINAL up to n (S-02.091 rule 1). front_seed gives the front's bytes, seed the rest."""
    f = random.Random(front_seed)
    init, front = f.randbytes(16), f.randbytes(64)
    r = random.Random(seed)
    main = r.randbytes(64)
    regions = [(0x08031000, len(main))]
    size = 0x40 + 8 * len(regions) + 4
    head = (struct.pack('<IHHIIII', 0x4D494257, 1, size, 0x08031000, len(regions), crc.crc32(main),
                        crc.crc32(init + front))
            + struct.pack('<4H', *version) + hashlib.sha256(init + front).digest())
    desc = head + b''.join(struct.pack('<II', a, k) for a, k in regions)
    desc += struct.pack('<I', crc.crc32(desc))
    B = ldr.Block
    blocks = [B(frozenset({'FIRST'}), 0x08002000, 0, 0), B(frozenset({'INIT'}), 0x11A04000, len(init), 0, init),
              B(frozenset(), 0x08002000, len(front), 0, front), B(frozenset(), 0x08030000, len(desc), 0, desc),
              B(frozenset(), 0x08031000, len(main), 0, main), B(frozenset({'FINAL'}), 0x08002000, 0, 0)]
    img = ldr.pack_blocks(blocks)
    assert len(img) <= n
    return img + r.randbytes(n - len(img))


def begin(img, sha=None, length=None, seq=2, flags=None):
    """BEGIN for img; flags None sends none (S-02.093: the byte is optional)."""
    sha = hashlib.sha256(img).digest() if sha is None else sha
    payload = struct.pack('<I', len(img) if length is None else length) + sha
    return wr(BEGIN, seq, payload + (bytes([flags]) if flags is not None else b''))


def transfer(img, sha=None, length=None, seq=2, flags=None):
    """BEGIN, DATA in 1024-byte chunks, END, then INFO: the host's bytes."""
    out = begin(img, sha, length, seq, flags)
    for k, off in enumerate(range(0, len(img), 1024)):
        out += wr(DATA, (seq + 1 + k) & 0xFF, struct.pack('<I', off) + img[off:off + 1024])
    return out + wr(END, 0xE0) + wr(INFO, 0xE1)


INFO_LEN = 242     # front 20: S-02.093's 232 bytes and the device, how and n, c (S-18.005)
WAY_REFUSED, WAY_PLAIN, WAY_SELECT_FIRST, WAY_IN_PLACE = 0, 1, 2, 3


def info_fields(p):
    """INFO's fields by S-02.093's offsets."""
    assert len(p) == INFO_LEN, len(p)
    self_slots = [p[93 + 53 * k:93 + 53 * (k + 1)] for k in range(2)]
    return {'why': p[0], 'flags': p[1], 'running': p[2], 'sel': p[3:5].hex(), 'target': p[5], 'refusal': p[6],
            'phase': p[7], 'len': struct.unpack_from('<I', p, 8)[0], 'got': struct.unpack_from('<I', p, 12)[0],
            'progress': struct.unpack_from('<H', p, 16)[0], 'result': p[18], 'image_rc': p[19], 'written': p[20],
            'readback': p[61:93], 'slot_flags': [q[0] for q in self_slots],
            'run_way': p[199], 'run_refusal': p[200], 'way': p[201], 'batt_len': p[202], 'batt': p[203:219],
            'damaged': bool(p[1] & 0x10), 'held': bool(p[1] & 0x20), 'read_error': bool(p[1] & 0x40),
            'unsettled': bool(p[1] & 0x80)}


# Image A, the running slot's: the front of every front_image(), another main part (S-02.084).
IMAGE_A = front_image(0x1800, seed=12, version=(1, 2, 3, 3))


class Recovery(unittest.TestCase):
    """SC-RECOVERY runs 1-4 and 8-12, 14 and 15 on the logic: recovery's writer and protocol (src/front/) through
    h_s02's flash, from the allowed starts of powercut_sequences.json and the sequences of
    recovery_running.json, with the running slot the selected one, which holds image A."""

    seqs = {s['id']: s for s in cut_sequences()['sequences'] if s['writer_rule'] == 'conforming'}

    def start(self, seq):
        st = seq['start']
        sel = tuple(bytes.fromhex(st['selector']))
        images = tuple(IMAGE_A if k == (0 if sel[0] == 0 else 1) else None for k in (0, 1))
        return spiflash.build_flash(selector=sel, slots=(st['slot0'], st['slot1']), images=images)

    def recover(self, flash_path, running, host, extra=()):
        out = run('s02', ['load ' + flash_path, 'recover %d %s' % (running, save('rec_host.bin', host))]
                  + list(extra))
        res, ops = split(out[1])
        return wr_parse(bytes.fromhex(res[0])) if res[0] != '-' else [], int(res[1]), ops, out

    def check_seqs(self, frames, host):
        sent = wr_parse(host)
        self.assertEqual([(t | 0x80, s) for t, s, _ in sent], [(t, s) for t, s, _ in frames])

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.093')
    def test_SC_RECOVERY_write(self):
        """Run 1 from each allowed start: the slot the selector does not name, S-02.049's order with the
        read-back, the selector {target, 00}, INFO's done state; the next reset boots the target."""
        n = cut_sequences()['image_length']
        img = front_image(n)
        host = wr(HELLO, 0) + wr(INFO, 1) + transfer(img)
        ids = [i for i, s in self.seqs.items() if s['start_allowed']]
        self.assertEqual(ids, ['C-01', 'C-02', 'C-03', 'C-04', 'C-08', 'C-09'])
        for i in ids:
            seq = self.seqs[i]
            t = seq['target']
            sel = 1 - t                                     # the running slot is the selected one
            frames, phase, ops, out = self.recover(save('rec_start.bin', self.start(seq)), sel, host,
                                                   ['peek 0xd000 2', 'fs 0'])
            self.check_seqs(frames, host)
            hello, before, after = frames[0][2], info_fields(frames[1][2]), info_fields(frames[-1][2])
            self.assertEqual((hello[0], hello[5], len(hello)), (2, 1, 47), i)   # protocol 2
            self.assertEqual((before['why'], before['flags'] & 3, before['running'], before['target'],
                              before['refusal']), (1, 3, sel, t, 0), i)
            self.assertTrue(all(f & 1 for f in before['slot_flags'][sel:sel + 1]), i)
            statuses = [p[0] for tt, _, p in frames[2:-1]]
            self.assertEqual(statuses, [0] * len(statuses), i)          # BEGIN, every DATA, END
            self.assertEqual(frames[2][2], bytes([0, WAY_PLAIN, t]), i)  # BEGIN: the way and the target
            self.assertEqual((phase, after['phase'], after['written'], after['progress'], after['result']),
                             (PH_DONE, PH_DONE, t, 1000, 0), i)
            self.assertEqual(after['readback'], hashlib.sha256(img).digest(), i)
            self.assertEqual((after['sel'], after['target']), ('%02x00' % t, 0xFF), i)
            # S-02.049, S-02.042, S-02.103: the same erases and programs as the reference upgrade, the
            # settings sector's erase included, nothing else
            self.assertEqual(writes(ops), ref_writes(seq['ops']), i)
            rec = spiflash.SLOT_RECORD_OFF[t]
            p = ops.index('p:%x:c' % rec)
            e = ops.index('e:d000')
            self.assertTrue(covered(ops[p + 1:e], rec, rec + 12), i)                         # S-02.047
            self.assertTrue(covered(ops[p + 1:e], spiflash.SLOT_OFF[t], spiflash.SLOT_OFF[t] + n), i)
            self.assertEqual(out[2], '%02x00' % t, i)                                         # S-02.048
            self.assertEqual(split(out[3])[0][0], str(t), i)          # the next reset checks and boots it

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.090')
    def test_SC_RECOVERY_refused_start(self):
        """Run 2: the running slot not established, not the selected one, verified not 0x01, the selected slot
        bad: INFO gives no target and the refusal, BEGIN answers it, nothing is erased or programmed."""
        img = front_image(cut_sequences()['image_length'])
        host = wr(INFO, 1) + wr(BEGIN, 2, struct.pack('<I', len(img)) + hashlib.sha256(img).digest())
        cases = [((0, 1), ('good', 'good'), 2, 2), ((0, 1), ('good', 'good'), 1, 3),
                 ((0, 0), ('good', 'good'), 0, 4), ((0, 1), ('badcrc', 'good'), 0, 5)]
        for sel, slots, running, why in cases:
            frames, phase, ops, _ = self.recover(save('rec_start.bin', spiflash.build_flash(selector=sel, slots=slots)),
                                                 running, host)
            inf = info_fields(frames[0][2])
            self.assertEqual((inf['target'], inf['refusal']), (0xFF, why), (sel, slots, running))
            self.assertEqual((frames[1][2], phase), (bytes([why, WAY_REFUSED, 0xFF]), PH_FAILED),
                             (sel, slots, running))
            self.assertEqual(writes(ops), [], (sel, slots, running))

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.091')
    def test_SC_RECOVERY_refused_image(self):
        """Run 3: the lengths settings_sector.json's `recovery` refuses at BEGIN with status 6, every refused
        slot case of bootable_images.json, a bootable stream with no front, an image that is not the one
        announced and one with another front: refused at END, nothing erased or programmed. An image of
        0x76000 bytes, the most BEGIN takes, is written. Image A itself is written to slot 1 as any other."""
        start = save('rec_start.bin', spiflash.build_flash(selector=(0, 1), slots=('good', 'good'),
                                                           images=(IMAGE_A, None)))
        lengths = set_vectors()['recovery']['lengths']
        self.assertEqual(set_vectors()['recovery']['image_max'], '0x76000')
        refused = [int(r['length'], 16) for r in lengths if r['begin'] == 'refused']
        self.assertTrue({0, 0x76001, 0x77000, 0x77001, spiflash.SLOT_MAX + 1} <= set(refused))
        for length in refused:                                                      # S-02.084: 1 to 0x76000
            self.assertEqual(next(r['status'] for r in lengths if int(r['length'], 16) == length), 6)
            frames, phase, ops, _ = self.recover(start, 0, wr(BEGIN, 2, struct.pack('<I', length) + bytes(32)))
            self.assertEqual((frames[0][2], phase, writes(ops)), (bytes([6, WAY_REFUSED, 0xFF]), PH_FAILED, []),
                             length)
        img = front_image(0x76000, seed=31)
        frames, phase, ops, out = self.recover(start, 0, transfer(img), ['peek 0xd000 2', 'fs 0'])
        end = [p for t, _, p in frames if t == END | 0x80][0]
        self.assertEqual((frames[0][2], end[0], phase), (bytes([0, WAY_PLAIN, 1]), 0, PH_DONE))
        sec = spiflash.SLOT_OFF[1] + spiflash.SLOT_MAX - 0x2000
        self.assertIn('p:%x:100' % (sec - 0x100), writes(ops))                     # the image's last page
        self.assertEqual([o for o in writes(ops) if o.startswith('e:') and
                          int(o[2:], 16) >= sec], ['e:%x' % (sec + 0x1000), 'e:%x' % sec])
        self.assertEqual((out[2], split(out[3])[0][0]), ('0100', '1'))
        cases = [(c['name'], b(c['image']), 7) for c in bootables() if c['kind'] == 'slot' and not c['accepted']]
        cases.append(('good, no front', b(next(c['image'] for c in bootables() if c['name'] == 'good')), 7))
        img = front_image(cut_sequences()['image_length'])
        cases.append(('changed', img[:-1] + bytes([img[-1] ^ 1]), 8))   # after FINAL: still a valid stream
        cases.append(('another front', front_image(len(img), front_seed=6), 12))           # S-02.084
        for name, im, want in cases:
            sha = hashlib.sha256(img).digest() if name == 'changed' else None
            frames, phase, ops, _ = self.recover(start, 0, transfer(im, sha=sha))
            end = [p for t, _, p in frames if t == END | 0x80][0]
            self.assertEqual((end[0], phase, writes(ops)), (want, PH_FAILED, []), name)
            if want == 7:
                self.assertNotEqual(end[1], 0, name)      # the image check's result says which rule
        # S-02.054, S-02.084: two slots may hold the same image
        frames, phase, ops, out = self.recover(start, 0, transfer(IMAGE_A), ['peek 0xd000 2', 'fs 0'])
        end = [p for t, _, p in frames if t == END | 0x80][0]
        self.assertEqual((end[0], phase), (0, PH_DONE))
        self.assertIn('e:%x' % spiflash.SLOT_OFF[1], writes(ops))
        self.assertFalse([o for o in writes(ops) if o.startswith('e:%x' % spiflash.SLOT_OFF[0])])
        self.assertEqual((out[2], split(out[3])[0][0]), ('0100', '1'))     # checked and booted at the next reset

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.052')
    def test_SC_RECOVERY_cuts(self):
        """Run 4: powercut_cuts.csv's 'none' and 'after' rows of the allowed starts, the write made by recovery;
        each followed by three resets whose slots must be the row's conforming_boots."""
        rows = [r for r in table('powercut_cuts.csv') if r['sequence'] in self.seqs
                and self.seqs[r['sequence']]['start_allowed'] and r['kind'] in ('none', 'after')]
        n = cut_sequences()['image_length']
        host = save('rec_host.bin', transfer(front_image(n)))
        starts = {i: save('rec_%s.bin' % i, self.start(s)) for i, s in self.seqs.items()}
        cmds, keys = [], []
        for r in rows:
            seq = self.seqs[r['sequence']]
            w = sum(1 for o in seq['ops'][:int(r['op_n'])] if o['op'] != 'read')
            sel = 1 - seq['target']
            if r['kind'] == 'none':
                cmds += ['load ' + starts[r['sequence']], 'fault none', 'fault none']
            else:
                cmds += ['load ' + starts[r['sequence']], 'fault after %d' % w, 'recover %d %s' % (sel, host)]
            cmds += ['fs 0', 'fs 0', 'fs 0']
            keys.append(r)
        self.assertGreater(len(keys), 50)
        out = run('s02', cmds)
        for k, r in enumerate(keys):
            what = (r['sequence'], r['op_n'], r['kind'])
            seq = self.seqs[r['sequence']]
            # the package's selector read-back (S-02.048) is not recovery's: a cut after the selector's
            # program is no cut of recovery's write
            if r['kind'] == 'after' and any(o['op'] != 'read' for o in seq['ops'][int(r['op_n']):]):
                self.assertEqual(split(out[6 * k + 2])[0], ['cut'], what)
            boots = ' '.join(split(out[6 * k + 3 + j])[0][0] for j in range(3))
            self.assertEqual(boots, r['conforming_boots'], what)

    # ---- runs 8-12: a bad selected slot, the writes of the running slot, the retry (S-02.084) ----

    def running_seqs(self, rescue=False):
        """recovery_running.json's sequences of runs 9, 10 and 14, or with rescue those of run 16."""
        with open(vectors('recovery_running.json'), encoding='utf-8') as f:
            seqs = json.load(f)['sequences']
        return {s['id']: s for s in (seqs[10:] if rescue else seqs[:10])}

    def running_start(self, seq):
        """recovery_running.json's start: image A in the running slot, the selected one."""
        st = seq['start']
        a = seq['running']
        images = tuple(IMAGE_A if k == a else None for k in (0, 1))
        return spiflash.build_flash(selector=tuple(b(st['selector'])), slots=(st['slot0'], st['slot1']),
                                    images=images)

    @staticmethod
    def flash_writes(ops):
        """The erases and programs of ops, with their offsets."""
        return [(o, int(o.split(':')[1], 16)) for o in writes(ops)]

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_bad_selected(self):
        """Run 8: slot 0 bad with its front intact (a byte of image A's main part changed in the flash): target
        1, no refusal, and run 1 holds. With a byte of a front block changed: refusal 5 in INFO and at BEGIN,
        nothing erased or programmed. With record 0 erased, the front intact in the stream: target 1, no
        refusal, slot 0's front flag set, and BEGIN accepted."""
        n = cut_sequences()['image_length']
        img = front_image(n)
        r = random.Random(12)
        main_at = IMAGE_A.index(r.randbytes(64))
        f = random.Random(5)
        f.randbytes(16)
        front_at = IMAGE_A.index(f.randbytes(64))
        good = spiflash.build_flash(selector=(0, 1), slots=('good', 'good'), images=(IMAGE_A, None))

        def changed(at, data=None, name='rec_run8.bin'):
            fl = bytearray(good)
            if data is None:
                fl[spiflash.SLOT_OFF[0] + at] ^= 0x01
            else:
                fl[at:at + len(data)] = data
            return save(name, bytes(fl))

        c01 = self.seqs['C-01']
        host = wr(INFO, 1) + transfer(img)
        frames, phase, ops, out = self.recover(changed(main_at), 0, host, ['peek 0xd000 2', 'fs 0'])
        before, after = info_fields(frames[0][2]), info_fields(frames[-1][2])
        self.assertEqual((before['slot_flags'][0] & 5, before['target'], before['refusal'], before['run_way']),
                         (4, 1, 0, WAY_SELECT_FIRST))
        self.assertEqual((phase, after['written'], after['result']), (PH_DONE, 1, 0))
        self.assertEqual(writes(ops), ref_writes(c01['ops']))
        self.assertEqual((out[2], split(out[3])[0][0]), ('0100', '1'))
        host = wr(INFO, 1) + begin(img)
        frames, phase, ops, _ = self.recover(changed(front_at), 0, host)
        inf = info_fields(frames[0][2])
        self.assertEqual((inf['slot_flags'][0] & 5, inf['target'], inf['refusal']), (0, 0xFF, 5))
        self.assertEqual((frames[1][2][0], phase, writes(ops)), (5, PH_FAILED, []))
        frames, phase, ops, _ = self.recover(
            changed(spiflash.SLOT_RECORD_OFF[0], b'\xff' * 12, 'rec_run8_rec.bin'), 0, host)
        inf = info_fields(frames[0][2])
        self.assertEqual((inf['slot_flags'][0] & 5, inf['target'], inf['refusal']), (4, 1, 0))
        self.assertEqual((frames[1][2][0], phase, writes(ops)), (0, PH_RECEIVING, []))

    @staticmethod
    def prelude(seq, img):
        """The writes recovery_running.json's `prior` lists, each failed at its first erase, which takes
        effect: their host bytes, the faults, and the write ops they make (one each)."""
        host, faults = b'', []
        for k, p in enumerate(seq['prior']):
            host += transfer(img, seq=0x30 + 0x10 * k, flags=1 if p['write'] == 'in_place' else None)
            faults.append('fault late %d' % (k + 1))
        return host, faults, len(seq['prior'])

    def main_flags(self, seq):
        return None if seq['way'] == 'plain' else 1

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_running(self):
        """Runs 9, 10 and 14 from every sequence of recovery_running.json, after the failed writes its `prior`
        lists: BEGIN's reply, INFO's way before and after, the erases and programs of the sequence and no
        other, the selector read back after {B, 01}, the read-back before the end, the selector at the end,
        and the slot the next reset boots."""
        n = cut_sequences()['image_length']
        img = front_image(n)
        for sid, seq in self.running_seqs().items():
            a, way, t = seq['running'], seq['way'], seq['target']
            pre, faults, _ = self.prelude(seq, img)
            main = wr(INFO, 1) + transfer(img, flags=self.main_flags(seq))
            out = run('s02', ['load ' + save('rec_run.bin', self.running_start(seq))] + faults +
                      ['recover %d %s' % (a, save('rec_host.bin', pre + main)), 'peek 0xd000 2', 'fs 0'])
            k = 1 + len(faults)
            res, ops = split(out[k])
            frames = wr_parse(bytes.fromhex(res[0]))
            self.check_seqs(frames, pre + main)
            i = [j for j, (tt, s, _) in enumerate(frames) if (tt, s) == (INFO | 0x80, 1)][0]
            before, after = info_fields(frames[i][2]), info_fields(frames[-1][2])
            want_way = {'select_first': WAY_SELECT_FIRST, 'in_place': WAY_IN_PLACE, 'plain': WAY_PLAIN}[way]
            if way == 'plain':
                self.assertEqual((before['target'], before['refusal'], before['flags'] & 0x10), (t, 0, 0x10), sid)
            else:
                self.assertEqual((before['target'], before['refusal'], before['run_way'], before['run_refusal']),
                                 (1 - a, 0, want_way, 0), sid)
            self.assertEqual(frames[i + 1][2], bytes([0, want_way, t]), sid)              # BEGIN's reply
            self.assertEqual([p[0] for _, _, p in frames[i + 1:-1]], [0] * (len(frames) - i - 2), sid)
            self.assertEqual((int(res[1]), after['phase'], after['written'], after['result'], after['way']),
                             (PH_DONE, PH_DONE, t, 0, want_way), sid)
            # a good slot selected: no hold; bit 4 stays while the running slot, once begun, is bad
            self.assertEqual((after['flags'] & 0x18, res[3]), (0x10 if way == 'plain' else 0, '0'), sid)
            prior = [w for p in seq['prior'] for w in ref_writes(p['ops'])]
            self.assertEqual(writes(ops), prior + ref_writes(seq['ops']), sid)
            ops = ops[[j for j, o in enumerate(ops) if o[0] in 'ep'][len(prior)]:]       # the write's own ops
            if way == 'select_first':
                self.assertEqual(ops[:3], ['e:d000', 'p:d000:2', 'r:d000:2'], sid)
            elif way == 'in_place':
                self.assertFalse([o for o in writes(ops) if o.startswith(('e:d000', 'p:d000'))], sid)
            rec = spiflash.SLOT_RECORD_OFF[t]
            p = ops.index('p:%x:c' % rec)
            self.assertTrue(covered(ops[p + 1:], rec, rec + 12), sid)                       # S-02.047
            self.assertTrue(covered(ops[p + 1:], spiflash.SLOT_OFF[t], spiflash.SLOT_OFF[t] + n), sid)
            k_ = 1 - t
            self.assertFalse([o for o, off in self.flash_writes(ops) if off == spiflash.SLOT_RECORD_OFF[k_] or
                              spiflash.SLOT_OFF[k_] <= off < spiflash.SLOT_OFF[k_] + spiflash.SLOT_MAX], sid)
            self.assertEqual(out[k + 1], seq['selector_end'].replace(' ', ''), sid)
            self.assertEqual(after['sel'], seq['selector_end'].replace(' ', ''), sid)
            self.assertEqual(split(out[k + 2])[0][0], str(t), sid)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_select_first_fails(self):
        """Run 9: a flash model that fails the first selector program: status 14, and no erase or program
        outside the selector sector."""
        seq = self.running_seqs()['W-01']
        img = front_image(cut_sequences()['image_length'])
        host = wr(INFO, 1) + transfer(img, flags=1)
        out = run('s02', ['load ' + save('rec_run.bin', self.running_start(seq)), 'fault fail 2',
                          'recover 0 ' + save('rec_host.bin', host), 'peek 0xd000 2'])
        res, ops = split(out[2])
        frames = wr_parse(bytes.fromhex(res[0]))
        after = info_fields(frames[-1][2])
        self.assertEqual((int(res[1]), after['phase'], after['result']), (PH_FAILED, PH_FAILED, 14))
        self.assertEqual(writes(ops), ['e:d000', 'p:d000:2'])
        self.assertEqual(after['written'], 0xFF)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.052')
    def test_SC_RECOVERY_running_cuts(self):
        """Runs 9, 10 and 14 with a cut after each operation in turn, after the failed writes `prior` lists:
        recovery_running.json's 'none' and 'after' rows, each followed by three resets whose slots must be
        the row's conforming_boots (with A slot 0 and B good: slot 1 until the last selector program; in
        place: slot 0, bad within the window; after a failed in-place write: the damaged slot until B is
        good)."""
        img = front_image(cut_sequences()['image_length'])
        cmds, keys = [], []
        for sid, seq in self.running_seqs().items():
            start = save('rec_%s.bin' % sid, self.running_start(seq))
            pre, faults, nw = self.prelude(seq, img)
            host = save('rec_host_%s.bin' % sid, pre + transfer(img, flags=self.main_flags(seq)))
            for c in seq['cuts']:
                if c['kind'] == 'torn':
                    continue
                w = sum(1 for o in seq['ops'][:c['op_n']] if o['op'] != 'read')
                cmds += ['load ' + start] + faults + ['fault after %d' % (nw + w),
                                                      'recover %d %s' % (seq['running'], host),
                                                      'fs 0', 'fs 0', 'fs 0']
                keys.append((sid, seq, c, len(faults)))
        self.assertGreater(len(keys), 250)
        out = run('s02', cmds)
        at = 0
        for sid, seq, c, nf in keys:
            what = (sid, c['op_n'], c['kind'])
            o = out[at:at + 6 + nf]
            at += 6 + nf
            if any(x['op'] != 'read' for x in seq['ops'][c['op_n']:]):
                self.assertEqual(split(o[2 + nf])[0], ['cut'], what)
            boots = ' '.join(split(o[3 + nf + j])[0][0] for j in range(3))
            self.assertEqual(boots, c['conforming_boots'], what)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.093')
    def test_SC_RECOVERY_in_place_busy(self):
        """Run 10: while the in-place write runs, BEGIN, DATA, END and RESTART are answered with status 1, and
        HELLO and INFO are served (phase writing, way 3). The host here does not wait for the write to end."""
        seq = self.running_seqs()['W-03']
        img = front_image(cut_sequences()['image_length'])
        pre, faults, _ = self.prelude(seq, img)
        during = (wr(HELLO, 0x50) + wr(INFO, 0x51) + begin(img, seq=0x52, flags=1)
                  + wr(DATA, 0x53, struct.pack('<I', 0) + img[:16]) + wr(END, 0x54) + wr(RESTART, 0x55))
        host = pre + transfer(img, flags=1) + during
        out = run('s02', ['load ' + save('rec_run.bin', self.running_start(seq))] + faults +
                  ['recover 0 %s eager' % save('rec_host.bin', host), 'peek 0xd000 2'])
        out = out[len(faults):]
        res, ops = split(out[1])
        frames = wr_parse(bytes.fromhex(res[0]))
        self.check_seqs(frames, host)
        by_seq = {s: (t, p) for t, s, p in frames}
        end = [p for t, s, p in frames if t == END | 0x80][0]
        self.assertEqual(end[0], 0)
        writing = info_fields(by_seq[0xE1][1])            # the transfer's own INFO, after END
        self.assertEqual((writing['phase'], writing['way']), (2, WAY_IN_PLACE))
        self.assertEqual(len(by_seq[0x50][1]), 47)
        inf = info_fields(by_seq[0x51][1])
        self.assertEqual((inf['phase'], inf['way']), (2, WAY_IN_PLACE))
        for s in (0x52, 0x53, 0x54, 0x55):
            self.assertEqual(by_seq[s][1][0], 1, hex(s))
        self.assertEqual(int(res[1]), PH_DONE)
        self.assertEqual(writes(ops), ref_writes(seq['prior'][0]['ops']) + ref_writes(seq['ops']))
        self.assertEqual(out[2], '0001')

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_retry(self):
        """Run 11: run 9 with a flash model whose first erase of slot 0 takes effect and reports a failure:
        phase failed, the selector reads 01 01, INFO's flag bit 3 is set and its target is 0. The same request
        again, and again without the flag: no selector erase before slot 0's first erase, the write goes to
        slot 0 (way 1), and the selector reads 00 01 at the end: slot 0 bad by this recovery's write, it is
        settled from 01 01 by the index programmed to 00 alone, no erase (S-02.084, S-02.048). Run 10
        likewise: the retry is accepted with slot
        0's front gone from the flash, and it ends as run 10."""
        n = cut_sequences()['image_length']
        img = front_image(n)
        seqs = self.running_seqs()
        slot0 = 'e:%x' % spiflash.SLOT_OFF[0]
        for sid, again, fault, sel_end in (('W-01', 1, 3, '0001'), ('W-01', None, 3, '0001'),
                                          ('W-03', 1, 2, '0001')):
            seq = seqs[sid]
            what = (sid, again)
            pre, faults, _ = self.prelude(seq, img)
            host = pre + transfer(img, flags=1) + transfer(img, seq=0x40, flags=again)
            cmds = ['load ' + save('rec_run.bin', self.running_start(seq))] + faults + ['fault late %d' % fault]
            out = run('s02', cmds + ['recover 0 ' + save('rec_host.bin', host), 'peek 0xd000 2', 'fs 0'])
            out = out[len(cmds) - 2:]                  # load, fault, recover, peek, fs as without a prelude
            res, ops = split(out[2])
            frames = wr_parse(bytes.fromhex(res[0]))
            infos = [info_fields(p) for t, _, p in frames if t == INFO | 0x80]
            self.assertEqual(len(infos), 2 + len(faults), what)
            failed, done = infos[-2:]
            self.assertEqual((failed['phase'], failed['result']), (PH_FAILED, 9), what)
            self.assertEqual(failed['slot_flags'][0] & 4, 0, what)            # slot 0's front is gone
            begin2 = [p for t, s_, p in frames if t == BEGIN | 0x80][-1]
            if sid == 'W-01':      # either request writes slot 0: the selector reads {1, 01}, slot 1 good
                self.assertEqual((failed['sel'], failed['flags'] & 0x18, failed['target'], failed['run_way']),
                                 ('0101', 0x18, 0, WAY_PLAIN), what)
                self.assertEqual(begin2, bytes([0, WAY_PLAIN, 0]), what)
            else:                  # the default still the other slot; the flag still in place; slot 0 damaged
                self.assertEqual((failed['sel'], failed['flags'] & 0x18, failed['target'], failed['run_way']),
                                 ('0001', 0x10, 1, WAY_IN_PLACE), what)
            self.assertEqual((done['phase'], done['written'], done['sel']), (PH_DONE, 0, sel_end), what)
            w = writes(ops)
            k = w.index(slot0)
            self.assertEqual(w[k + 1], slot0, what)        # the retry's first write: slot 0, no selector first
            # slot 0's classes 1-4, then the damaged state's settling on slot 0 from the selector it reads:
            # from {1, 01} the index programmed to 00 alone (S-02.084); from {0, 01} (run 10) nothing
            want = ref_writes([o for o in seq['ops'] if o['class'] not in
                               ('select_erase', 'select_program', 'selector_erase', 'selector_program')])
            want += ['p:d000:1'] if sid == 'W-01' else []
            self.assertEqual(w[k + 1:], want, what)
            self.assertNotIn('e:d000', w[k + 1:], what)
            self.assertEqual((out[3], split(out[4])[0][0]), (sel_end, '0'), what)

    def w03(self):
        seq = self.running_seqs()['W-03']
        return seq, save('rec_w03.bin', spiflash.build_flash(selector=(0, 1), slots=('good', 'badcrc'),
                                                             images=(IMAGE_A, None)))

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_in_place_order(self):
        """Run 10: with slot 1 bad, BEGIN with flags 01 is refused 15 (INFO's 199 reads 0, 200 reads 15) and
        nothing is written; a write without the flag whose erase of slot 1 fails ends with result 9; then
        BEGIN with flags 01 is answered 00 03 00 (INFO's 199 reads 3 before it), and the write is in place:
        slot 0's ops only, the selector still 00 01, INFO done with way 3. Then an image with another front is
        refused 12 at END, and INFO still reports the in-place write."""
        seq, start = self.w03()
        img = front_image(cut_sequences()['image_length'])
        host = (wr(INFO, 1) + begin(img, seq=2, flags=1) + wr(INFO, 3) + transfer(img, seq=0x10)
                + wr(INFO, 0x20) + transfer(img, seq=0x21, flags=1)
                + transfer(front_image(len(img), front_seed=6), seq=0x60, flags=1))
        out = run('s02', ['load ' + start, 'fault fail 1', 'recover 0 ' + save('rec_host.bin', host),
                          'peek 0xd000 2', 'fs 0'])
        res, ops = split(out[2])
        frames = wr_parse(bytes.fromhex(res[0]))
        self.check_seqs(frames, host)
        by = {}
        for t, s, p in frames:
            by.setdefault((t & 0x7F, s), []).append(p)
        first = info_fields(by[(INFO, 1)][0])
        self.assertEqual((first['run_way'], first['run_refusal'], first['target']), (WAY_REFUSED, 15, 1))
        self.assertEqual(by[(BEGIN, 2)][0], bytes([15, WAY_REFUSED, 0xFF]))
        infos = [info_fields(p) for p in by[(INFO, 0xE1)]]
        self.assertEqual((infos[0]['phase'], infos[0]['result'], infos[0]['way']), (PH_FAILED, 9, WAY_PLAIN))
        self.assertEqual(info_fields(by[(INFO, 0x20)][0])['run_way'], WAY_IN_PLACE)
        self.assertEqual(by[(BEGIN, 0x21)][0], bytes([0, WAY_IN_PLACE, 0]))
        self.assertEqual((infos[1]['phase'], infos[1]['written'], infos[1]['way'], infos[1]['sel']),
                         (PH_DONE, 0, WAY_IN_PLACE, '0001'))
        self.assertEqual(by[(BEGIN, 0x60)][0], bytes([0, WAY_IN_PLACE, 0]))
        self.assertEqual((by[(END, 0xE0)][-1][0], infos[2]['result'], infos[2]['phase'], infos[2]['way']),
                         (12, 0, PH_DONE, WAY_IN_PLACE))      # another front
        self.assertEqual(writes(ops), ['e:%x' % spiflash.SLOT_OFF[1]] + ref_writes(seq['ops']))
        self.assertEqual((out[3], split(out[4])[0][0]), ('0001', '0'))

    def failed_in_place(self, then=b''):
        """Run 14's start: run 10 whose first erase of slot 0 takes effect and reports a failure, then
        RESTART; `then` follows. The replies by (type, seq), RESTART HOLD, ops, and the later outputs."""
        seq, start = self.w03()
        img = front_image(cut_sequences()['image_length'])
        host = (transfer(img, seq=0x10) + transfer(img, seq=0x20, flags=1) + wr(INFO, 0x30)
                + wr(RESTART, 0x31) + then)
        out = run('s02', ['load ' + start, 'fault fail 1', 'fault late 2',
                          'recover 0 ' + save('rec_host.bin', host), 'peek 0xd000 2', 'fs 0'])
        res, ops = split(out[3])
        frames = wr_parse(bytes.fromhex(res[0]))
        self.check_seqs(frames, host)
        by = {}
        for t, s, p in frames:
            by.setdefault((t & 0x7F, s), []).append(p)
        return by, (res[2], res[3]), ops, out[4:]

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_in_place_failed(self):
        """Run 14 on the logic: after a failed in-place write INFO's flag bit 4 is set, RESTART is answered 0x44
        and no reset is asked for, and the power-off stays held (fr_proto_hold()). Then a write with flags 01
        ends as run 10, or a write without the flag ends with slot 1 good and the selector 01 01; either way
        the hold ends and RESTART is answered 0 and asks for the reset. The PSoC's 0x32 is the unit tests'."""
        img = front_image(cut_sequences()['image_length'])
        by, (restart, hold), ops, _ = self.failed_in_place()
        inf = info_fields(by[(INFO, 0x30)][0])
        self.assertEqual((inf['phase'], inf['result'], inf['flags'] & 0x18), (PH_FAILED, 9, 0x10))
        self.assertEqual(by[(RESTART, 0x31)][0], bytes([0x44]))
        self.assertEqual((restart, hold), ('0', '1'))
        self.assertEqual(writes(ops), ['e:%x' % spiflash.SLOT_OFF[1], 'e:%x' % spiflash.SLOT_OFF[0]])
        for flags, written, sel in ((1, 0, '0001'), (None, 1, '0101')):
            then = transfer(img, seq=0x40, flags=flags) + wr(RESTART, 0x50)
            by, (restart, hold), ops, out = self.failed_in_place(then)
            done = info_fields(by[(INFO, 0xE1)][-1])
            self.assertEqual((done['phase'], done['written'], done['sel'], done['flags'] & 0x18),
                             (PH_DONE, written, sel, 0 if written == 0 else 0x10), flags)
            self.assertEqual(by[(RESTART, 0x31)][0], bytes([0x44]), flags)
            self.assertEqual(by[(RESTART, 0x50)][0], bytes([0]), flags)
            self.assertEqual((restart, hold), ('1', '0'), flags)
            self.assertEqual((out[0], split(out[1])[0][0]), (sel, str(written)), flags)
            if flags is None:          # the damaged state: {1, 00} a byte at a time, the index read back
                self.assertEqual(writes(ops)[-3:], ['e:d000', 'p:d000:1', 'p:d001:1'])
                k = len(ops) - 1 - ops[::-1].index('p:d000:1')
                self.assertEqual((ops[k + 1], ops[k + 2], ops[k + 3]), ('r:d000:1', 'p:d001:1', 'r:d000:2'))

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_reach_records(self):
        """S-02.084's reach after run 14's failed in-place write: the selector {0, FF} names the damaged slot 0
        untrusted and slot 1 is good. Slot 0's record decides: one the factory check is sure to fail (the
        tag with a CRC that differs, another tag) counts as the fallback, bit 5 clear and RESTART 0; an empty
        record or a length over 0x78000 does not: bit 5 set, RESTART 0x44, the power-off held."""
        seq, start = self.w03()
        img = front_image(cut_sequences()['image_length'])
        damage = transfer(img, seq=0x10) + transfer(img, seq=0x20, flags=1)
        faults = ['fault fail 1', 'fault late 2']
        path = os.path.join(WORK, 'rec_after.bin')
        run('s02', ['load ' + start] + faults + ['recover 0 ' + save('rec_host.bin', damage), 'save ' + path])
        with open(path, 'rb') as f:
            state = bytearray(f.read())
        state[spiflash.SELECTOR_OFF:spiflash.SELECTOR_OFF + 2] = b'\x00\xff'
        good1 = spiflash.build_flash(selector=(0, 1), slots=('good', 'good'), images=(IMAGE_A, None))
        lo, hi = spiflash.SLOT_OFF[1], spiflash.SLOT_OFF[1] + spiflash.SLOT_MAX
        state[lo:hi] = good1[lo:hi]
        state[spiflash.SLOT_RECORD_OFF[1]:spiflash.SLOT_RECORD_OFF[1] + 12] = \
            good1[spiflash.SLOT_RECORD_OFF[1]:spiflash.SLOT_RECORD_OFF[1] + 12]
        rec0 = struct.unpack_from('<III', state, spiflash.SLOT_RECORD_OFF[0])
        cases = [('crc differs', (rec0[0] ^ 1, rec0[1], 0xB004), False),
                 ('another tag', (rec0[0], rec0[1], 0xB005), False),
                 ('empty', (0, 0, 0xB004), True),
                 ('length 0x78001', (rec0[0], 0x78001, 0xB004), True),
                 ('length 0x80000000', (rec0[0], 0x80000000, 0xB004), True)]
        host = damage + wr(INFO, 0x30) + wr(RESTART, 0x31)
        for what, rec, held in cases:
            fl = bytearray(state)
            fl[spiflash.SLOT_RECORD_OFF[0]:spiflash.SLOT_RECORD_OFF[0] + 12] = struct.pack('<III', *rec)
            out = run('s02', ['load ' + start] + faults +
                      ['recover 0 %s swap %d %s' % (save('rec_host.bin', host), len(damage),
                                                    save('rec_swap.bin', bytes(fl)))])
            res, _ = split(out[3])
            frames = wr_parse(bytes.fromhex(res[0]))
            by = {(t & 0x7F, s): p for t, s, p in frames}
            inf = info_fields(by[(INFO, 0x30)])
            self.assertEqual((inf['sel'], inf['slot_flags'][1] & 1, inf['damaged'], inf['held']),
                             ('00ff', 1, True, held), what)
            self.assertEqual((by[(RESTART, 0x31)], res[2], res[3]),
                             (bytes([0x44]), '0', '1') if held else (bytes([0]), '1', '0'), what)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.093')
    def test_SC_RECOVERY_changed(self):
        """Run 15: run 10 up to its second BEGIN, answered way 3; slot 1 is then made good in the flash before
        END: END answers 16, nothing is erased or programmed, and INFO keeps the failed write's result 9."""
        _, start = self.w03()
        img = front_image(cut_sequences()['image_length'])
        first = transfer(img, seq=0x10)
        second = transfer(img, seq=0x20, flags=1)
        host = first + second
        at = len(first) + second.index(wr(END, 0xE0))
        good = save('rec_good.bin', spiflash.build_flash(selector=(0, 1), slots=('good', 'good'),
                                                         images=(IMAGE_A, None)))
        out = run('s02', ['load ' + start, 'fault fail 1',
                          'recover 0 %s swap %d %s' % (save('rec_host.bin', host), at, good)])
        res, ops = split(out[2])
        frames = wr_parse(bytes.fromhex(res[0]))
        begins = [p for t, s, p in frames if t == BEGIN | 0x80]
        ends = [p for t, s, p in frames if t == END | 0x80]
        self.assertEqual(begins[1], bytes([0, WAY_IN_PLACE, 0]))
        self.assertEqual(ends[1][0], 16)
        self.assertEqual((info_fields(frames[-1][2])['result'], int(res[1])), (9, PH_FAILED))
        self.assertEqual(writes(ops), ['e:%x' % spiflash.SLOT_OFF[1]])        # the failed erase only

    # ---- runs 16 and 17: after a write of the running slot has left it bad (S-02.084) ----

    def rescue_run(self, start, faults, host, extra=(), swap=None, before=(), running=1):
        """One recover from start with faults; swap: (host offset, flash image). The replies by
        (type, seq) in order, the recover fields, the ops and the outputs after it."""
        opt = ' swap %d %s' % (swap[0], save('rec_swap.bin', swap[1])) if swap else ''
        cmds = ['load ' + save('rec_res.bin', start)] + list(before) + list(faults)
        out = run('s02', cmds + ['recover %d %s%s' % (running, save('rec_host.bin', host), opt)] + list(extra))
        res, ops = split(out[len(cmds)])
        frames = wr_parse(bytes.fromhex(res[0]))
        self.check_seqs(frames, host)
        by = {}
        for t, s, p in frames:
            by.setdefault((t & 0x7F, s), []).append(p)
        return by, res, ops, out[len(cmds) + 1:]

    def flash_after(self, start, faults, host, running=1):
        """The flash as a recover from start with faults leaves it."""
        path = os.path.join(WORK, 'rec_after.bin')
        run('s02', ['load ' + save('rec_res.bin', start)] + list(faults) +
            ['recover %d %s' % (running, save('rec_host.bin', host)), 'save ' + path])
        with open(path, 'rb') as f:
            return f.read()

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_any_selector(self):
        """Run 16: slot 1 running and selected, slot 0 bad; run 14's failed in-place write of slot 1; then the
        flash set (swap) to hold a good slot 0 and the selector FF FF, as an earlier write cut short could
        leave them: no failing write produces FF FF any more (run 20). INFO: slot 0 good, flag bits 4 and 5
        set (a fallback to slot 0 does not count), RESTART 0x44, the power-off held. From that state: slot
        0's own image without the flag erases nothing at all, makes the read-back and settles the selector
        on slot 0 by programs alone, 00 01 (recovery_running.json W-11); another image is refused 18 and the
        hold stays; the flag is way 2, the same settling first, then slot 1, then the erase and {1, 01}
        (W-12). After each write bit 5 is clear and RESTART answered 0. The same with the selector torn to
        01 3F (W-14, W-15); and with slot 0 bad an in-place write of slot 1, which the selector naming slot
        1 leaves settled: no selector write (W-13, W-16)."""
        img = front_image(cut_sequences()['image_length'])
        other = front_image(len(img), seed=21)
        seqs = self.running_seqs(rescue=True)
        start = self.running_start(self.running_seqs()['W-07'])           # 01 01, slot 0 bad, A slot 1
        damage = transfer(img, seq=0x10) + transfer(img, seq=0x20, flags=1)
        faults = ['fault late 1', 'fault late 2']
        state = bytearray(self.flash_after(start, faults, damage))
        good0 = spiflash.build_flash(selector=(0, 1), slots=('good', 'good'), images=(img, None))
        lo, hi = spiflash.SLOT_OFF[0], spiflash.SLOT_OFF[0] + spiflash.SLOT_MAX
        state[lo:hi] = good0[lo:hi]
        r0 = spiflash.SLOT_RECORD_OFF[0]
        state[r0:r0 + 12] = good0[r0:r0 + 12]
        tail = wr(INFO, 0x70) + wr(RESTART, 0x71)
        for sel, own, first in (('ff ff', 'W-11', 'W-12'), ('01 3f', 'W-14', 'W-15')):
            what = sel
            fl = bytearray(state)
            fl[spiflash.SELECTOR_OFF:spiflash.SELECTOR_OFF + 2] = b(sel)
            swap = (len(damage), bytes(fl))
            by, res, ops, _ = self.rescue_run(start, faults, damage + wr(INFO, 0x3E) + wr(RESTART, 0x3F), (), swap)
            inf = info_fields(by[(INFO, 0x3E)][0])
            self.assertEqual((inf['sel'], inf['slot_flags'][0] & 1, inf['damaged'], inf['held'], inf['target']),
                             (sel.replace(' ', ''), 1, True, True, 0), what)
            self.assertEqual((by[(RESTART, 0x3F)][0], res[2], res[3]), (bytes([0x44]), '0', '1'), what)
            # (i) slot 0's own image: no erase at all, no op on slot 0 or record 0, then 00 01 by programs
            by, res, ops, out = self.rescue_run(start, faults, damage + transfer(img, seq=0x41) + tail,
                                                ['peek 0xd000 2'], swap)
            self.assertEqual(by[(BEGIN, 0x41)][0], bytes([0, WAY_PLAIN, 0]), what)
            w = writes(ops)[2:]
            self.assertEqual(w, ref_writes(seqs[own]['ops']), what)
            self.assertEqual(w, ['p:d000:1', 'p:d001:1'], what)          # the index to 00, verified to 01
            k = ops.index('p:d000:1')
            self.assertTrue(covered(ops[:k], spiflash.SLOT_OFF[0], spiflash.SLOT_OFF[0] + len(img)), what)
            self.assertEqual((ops[k + 1], ops[k + 2], ops[k + 3]), ('r:d000:1', 'p:d001:1', 'r:d000:2'), what)
            self.assertEqual((info_fields(by[(INFO, 0xE1)][-1])['result'], out[0]), (0, '0001'), what)
            self.assertEqual((info_fields(by[(INFO, 0x70)][0])['held'], by[(RESTART, 0x71)][0], res[2], res[3]),
                             (False, bytes([0]), '1', '0'), what)
            # another image: 18, nothing written, still held
            by, res, ops, out = self.rescue_run(start, faults, damage + transfer(other, seq=0x41) + tail,
                                                ['peek 0xd000 2'], swap)
            self.assertEqual((by[(END, 0xE0)][-1][0], writes(ops)[2:], out[0]), (18, [], sel.replace(' ', '')),
                             what)
            self.assertEqual((by[(RESTART, 0x71)][0], res[3]), (bytes([0x44]), '1'), what)
            # the flag: way 2, 00 01 by programs first, then slot 1, then the erase and {1, 01}; another image
            # than slot 0's
            by, res, ops, out = self.rescue_run(start, faults, damage + transfer(other, seq=0x41, flags=1) + tail,
                                                ['peek 0xd000 2', 'fs 0'], swap)
            self.assertEqual(by[(BEGIN, 0x41)][0], bytes([0, WAY_SELECT_FIRST, 1]), what)
            w = writes(ops)[2:]
            self.assertEqual(w, ref_writes(seqs[first]['ops']), what)
            self.assertEqual(w[:2], ['p:d000:1', 'p:d001:1'], what)       # no erase before slot 1's write
            self.assertEqual((out[0], split(out[1])[0][0]), ('0101', '1'), what)
            self.assertEqual((info_fields(by[(INFO, 0x70)][0])['held'], by[(RESTART, 0x71)][0], res[2], res[3]),
                             (False, bytes([0]), '1', '0'), what)
        # (iii) slot 0 bad: in place; the selector names slot 1, the target, and is left as it is
        start8 = self.running_start(self.running_seqs()['W-08'])          # 01 01, both bad, A slot 1
        state8 = self.flash_after(start8, faults, damage)
        for sel, sid in (('ff ff', 'W-13'), ('01 3f', 'W-16')):
            fl = bytearray(state8)
            fl[spiflash.SELECTOR_OFF:spiflash.SELECTOR_OFF + 2] = b(sel)
            host = damage + wr(INFO, 0x40) + transfer(img, seq=0x41, flags=1)
            by, res, ops, out = self.rescue_run(start8, faults, host, ['peek 0xd000 2', 'fs 0'],
                                                (len(damage), bytes(fl)))
            inf = info_fields(by[(INFO, 0x40)][0])
            self.assertEqual((inf['sel'], inf['held'], inf['run_way']),
                             (sel.replace(' ', ''), True, WAY_IN_PLACE), sel)
            self.assertEqual(by[(BEGIN, 0x41)][0], bytes([0, WAY_IN_PLACE, 1]), sel)
            self.assertEqual(writes(ops)[2:], ref_writes(seqs[sid]['ops']), sel)
            self.assertFalse([o for o in writes(ops)[2:] if o.startswith(('e:d000', 'p:d00'))], sel)
            self.assertEqual((out[0], split(out[1])[0][0], res[3]), (sel.replace(' ', ''), '1', '0'), sel)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_settle_not_stored(self):
        """Run 20: run 14's way out without the flag, with slot 0 the other slot and the selector 01 01: the
        write of slot 0 settles the selector on slot 0 by programs alone; with its index program not stored,
        the index reads back 01: result 11, no erase was made, and the selector still reads 01 01."""
        img = front_image(cut_sequences()['image_length'])
        seqs = self.running_seqs(rescue=True)
        start = self.running_start(self.running_seqs()['W-07'])           # 01 01, slot 0 bad, A slot 1
        damage = transfer(img, seq=0x10) + transfer(img, seq=0x20, flags=1)
        b_writes = [o for o in seqs['W-12']['prior'][2]['ops'] if o['op'] != 'read']
        n_b = [o['class'] for o in b_writes].index('selector_zero_index') + 1
        faults = ['fault late 1', 'fault late 2', 'fault stuck %d' % (2 + n_b)]
        by, res, ops, out = self.rescue_run(start, faults, damage + transfer(img, seq=0x30) + wr(INFO, 0x3E),
                                            ['peek 0xd000 2'])
        result = info_fields(by[(INFO, 0xE1)][-1])
        inf = info_fields(by[(INFO, 0x3E)][0])
        self.assertEqual((result['phase'], result['result']), (PH_FAILED, 11))
        self.assertEqual((inf['sel'], inf['slot_flags'][0] & 1, out[0]), ('0101', 1, '0101'))
        w = writes(ops)[2:]
        self.assertEqual(w[-1], 'p:d000:1')
        self.assertNotIn('e:d000', w)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_verified_00(self):
        """Run 21: the running slot damaged by run 14's failed in-place write, its record whole (a CRC that
        differs), the other slot good, the selector naming the damaged slot with verified 00: a first stage
        falls back to the good slot, so the selector leads to it. Both requests are announced as way 1 to
        the running slot; no selector erase or program comes before its last program; INFO's bit 5 is set (RESTART 0x44) until the write has ended. Slot 1 running
        with 01 00, the write then settled (it names slot 1, the target); mirrored, slot 0 running with
        00 00, settled after by the erase and {0, 01} (recovery_running.json W-19, W-20)."""
        n = cut_sequences()['image_length']
        img = front_image(n)
        b_img = front_image(n, seed=13)
        mine = front_image(n, seed=22)
        damage = transfer(img, seq=0x10) + transfer(img, seq=0x20, flags=1)
        seqs = self.running_seqs(rescue=True)
        w03 = spiflash.build_flash(selector=(0, 1), slots=('good', 'badcrc'), images=(IMAGE_A, None))
        mirrors = ((1, b'\x01\x00', 'W-19', self.running_start(self.running_seqs()['W-08'])),
                   (0, b'\x00\x00', 'W-20', w03))
        faults = ['fault late 1', 'fault late 2']
        for a, sel, sid, start in mirrors:
            bslot = 1 - a
            state = bytearray(self.flash_after(start, faults, damage, running=a))
            good = spiflash.build_flash(selector=(0, 1), slots=('good', 'good'),
                                        images=tuple(b_img if k == bslot else None for k in (0, 1)))
            lo, hi = spiflash.SLOT_OFF[bslot], spiflash.SLOT_OFF[bslot] + spiflash.SLOT_MAX
            state[lo:hi] = good[lo:hi]
            r = spiflash.SLOT_RECORD_OFF[bslot]
            state[r:r + 12] = good[r:r + 12]
            state[spiflash.SELECTOR_OFF:spiflash.SELECTOR_OFF + 2] = sel
            ra = struct.unpack_from('<III', state, spiflash.SLOT_RECORD_OFF[a])
            self.assertEqual((ra[2], 1 <= ra[1] <= spiflash.SLOT_MAX), (0xB004, True), sid)  # sure to fail
            swap = (len(damage), bytes(state))
            host = (damage + wr(INFO, 0x30) + wr(RESTART, 0x31) + begin(b_img, seq=0x32)
                    + transfer(mine, seq=0x50, flags=1) + wr(INFO, 0x60)
                    + wr(RESTART, 0x61))
            by, res, ops, out = self.rescue_run(start, faults, host, ['peek 0xd000 2', 'fs 0'], swap, running=a)
            inf = info_fields(by[(INFO, 0x30)][0])
            self.assertEqual((inf['sel'], inf['damaged'], inf['held'], inf['target'], inf['run_way']),
                             (sel.hex(), True, True, a, WAY_PLAIN), sid)
            self.assertEqual(by[(RESTART, 0x31)][0], bytes([0x44]), sid)
            self.assertEqual(by[(BEGIN, 0x32)][0], bytes([0, WAY_PLAIN, a]), sid)      # without the flag: way 1
            self.assertEqual(by[(BEGIN, 0x50)][0], bytes([0, WAY_PLAIN, a]), sid)
            w = writes(ops)[2:]
            self.assertEqual(w, ref_writes(seqs[sid]['ops']), sid)
            # no selector op before the record program, which follows the slot's last program
            last = max(j for j, o in enumerate(w) if o.startswith('p:') and not o.startswith('p:d00'))
            self.assertFalse([o for o in w[:last] if o.startswith(('e:d000', 'p:d00'))], sid)
            inf = info_fields(by[(INFO, 0x60)][0])
            end = '0100' if a == 1 else '0001'
            self.assertEqual((inf['sel'], inf['held'], by[(RESTART, 0x61)][0], res[2], res[3]),
                             (end, False, bytes([0]), '1', '0'), sid)
            self.assertEqual((out[0], split(out[1])[0][0]), (end, str(a)), sid)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_no_erase_leading(self):
        """Run 19: the running slot damaged by run 14's failed in-place write, the other slot good and named
        with verified 00. A write with the flag is announced as way 1 to the running slot and makes no
        selector op before its last program; the
        write ends with {A, 01} a byte at a time (recovery_running.json W-17, W-18). With 00 00 (slot 1
        damaged) INFO's bit 5 is set and RESTART answered 0x44 until the write has ended; mirrored, 01 00
        (slot 0 damaged), bit 5 is clear from the start and RESTART answered 0."""
        n = cut_sequences()['image_length']
        img = front_image(n)
        b_img = front_image(n, seed=13)
        mine = front_image(n, seed=22)
        damage = transfer(img, seq=0x10) + transfer(img, seq=0x20, flags=1)
        seqs = self.running_seqs(rescue=True)
        w03 = spiflash.build_flash(selector=(0, 1), slots=('good', 'badcrc'), images=(IMAGE_A, None))
        mirrors = ((1, b'\x00\x00', 'W-17', self.running_start(self.running_seqs()['W-08'])),
                   (0, b'\x01\x00', 'W-18', w03))
        faults = ['fault late 1', 'fault late 2']
        for a, sel, sid, start in mirrors:
            bslot = 1 - a
            state = bytearray(self.flash_after(start, faults, damage, running=a))
            good = spiflash.build_flash(selector=(0, 1), slots=('good', 'good'),
                                        images=tuple(b_img if k == bslot else None for k in (0, 1)))
            lo, hi = spiflash.SLOT_OFF[bslot], spiflash.SLOT_OFF[bslot] + spiflash.SLOT_MAX
            state[lo:hi] = good[lo:hi]
            r = spiflash.SLOT_RECORD_OFF[bslot]
            state[r:r + 12] = good[r:r + 12]
            state[spiflash.SELECTOR_OFF:spiflash.SELECTOR_OFF + 2] = sel
            swap = (len(damage), bytes(state))
            held = a == 1
            # the start, then RESTART: 0x44 while held; answered 0 (and the reset asked for) otherwise
            by, res, ops, _ = self.rescue_run(start, faults, damage + wr(INFO, 0x30) + wr(RESTART, 0x31), (),
                                              swap, running=a)
            inf = info_fields(by[(INFO, 0x30)][0])
            self.assertEqual((inf['sel'], inf['damaged'], inf['held'], inf['target'], inf['run_way']),
                             (sel.hex(), True, held, a, WAY_PLAIN), sid)
            self.assertEqual((by[(RESTART, 0x31)][0], res[3]),
                             (bytes([0x44 if held else 0]), '1' if held else '0'), sid)
            # the flag: way 1 to A, the selector only after A's last program
            host = (damage + transfer(mine, seq=0x50, flags=1) + wr(INFO, 0x60)
                    + wr(RESTART, 0x61))
            by, res, ops, out = self.rescue_run(start, faults, host, ['peek 0xd000 2', 'fs 0'], swap, running=a)
            self.assertEqual(by[(BEGIN, 0x50)][0], bytes([0, WAY_PLAIN, a]), sid)
            w = writes(ops)[2:]
            self.assertEqual(w, ref_writes(seqs[sid]['ops']), sid)
            self.assertEqual(w[-3:], ['e:d000', 'p:d000:1', 'p:d001:1'], sid)
            self.assertFalse([o for o in w[:-3] if o.startswith(('e:d000', 'p:d00'))], sid)
            inf = info_fields(by[(INFO, 0x60)][0])
            self.assertEqual((inf['sel'], inf['held'], by[(RESTART, 0x61)][0], res[2], res[3]),
                             ('%02x01' % a, False, bytes([0]), '1', '0'), sid)
            self.assertEqual((out[0], split(out[1])[0][0]), ('%02x01' % a, str(a)), sid)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.093')
    def test_SC_RECOVERY_read_fails_once(self):
        """Run 17: after run 14's failed in-place write, a read of the selector that fails once: that INFO has
        flag bit 6 set, the next reads again, bit 6 clear, and the writes are offered as in run 14. A write
        of slot 1 that succeeds while the read of slot 1 fails once in the pass after it: the next INFO
        reports slot 1 good and selected, bits 5 and 6 clear, RESTART is answered 0. A BEGIN whose start
        condition cannot be read is answered 17, not 9. The hold printed by the harness is bit 5."""
        seq, start = self.w03()
        img = front_image(cut_sequences()['image_length'])
        damage = transfer(img, seq=0x10) + transfer(img, seq=0x20, flags=1)
        faults = ['fault fail 1', 'fault late 2']

        def go(host, once=None):
            cmds = ['load ' + start] + (['readfail once %d %s' % once] if once else []) + faults
            out = run('s02', cmds + ['recover 0 ' + save('rec_host.bin', host), 'readfail'])
            res, ops = split(out[len(cmds)])
            frames = wr_parse(bytes.fromhex(res[0]))
            by = {}
            for t, s, p in frames:
                by.setdefault((t & 0x7F, s), []).append(p)
            return by, res, ops, int(out[len(cmds) + 1])

        host = damage + wr(INFO, 0x30) + wr(INFO, 0x31)
        _, _, ops, _ = go(host)
        reads = [o for o in ops if o == 'r:d000:2']
        by, res, ops, failed = go(host, (len(reads) - 1, '0xd000 2'))
        one, two = info_fields(by[(INFO, 0x30)][0]), info_fields(by[(INFO, 0x31)][0])
        self.assertEqual((failed, one['read_error'], one['flags'] & 2), (1, True, 0))
        self.assertEqual((two['read_error'], two['flags'] & 2, two['held'], two['target'], two['run_way']),
                         (False, 2, True, 1, WAY_IN_PLACE))
        self.assertEqual(res[3], '1')                                    # held, as bit 5 says
        host = damage + transfer(img, seq=0x40) + wr(INFO, 0x50) + wr(RESTART, 0x51)
        _, _, ops, _ = go(host)
        last = max(j for j, o in enumerate(ops) if o == 'p:d001:1')      # the damaged state's verified byte
        rec1 = 'r:%x:c' % spiflash.SLOT_RECORD_OFF[1]
        nth = sum(1 for o in ops[:last] if o == rec1) + 1
        by, res, ops, failed = go(host, (nth, '0x%x 12' % spiflash.SLOT_RECORD_OFF[1]))
        inf = info_fields(by[(INFO, 0x50)][0])
        self.assertEqual((failed, info_fields(by[(INFO, 0xE1)][-1])['phase']), (1, PH_DONE))
        self.assertEqual((inf['slot_flags'][1] & 1, inf['sel'], inf['held'], inf['read_error']),
                         (1, '0101', False, False))
        self.assertEqual((by[(RESTART, 0x51)][0], res[2], res[3]), (bytes([0]), '1', '0'))
        # a start condition that cannot be read: 17
        good = save('rec_c01.bin', self.start(self.seqs['C-01']))
        out = run('s02', ['load ' + good, 'readfail 0xd000 2',
                          'recover 0 ' + save('rec_host.bin', wr(INFO, 1) + begin(img, seq=2))])
        frames = wr_parse(bytes.fromhex(split(out[2])[0][0]))
        inf = info_fields(frames[0][2])
        self.assertEqual((inf['target'], inf['refusal'], inf['read_error'], frames[1][2]),
                         (0xFF, 17, True, bytes([17, WAY_REFUSED, 0xFF])))

    def swapped(self, start, faults, damage, running, slot=None, image=None, sel=None):
        """The flash after damage, with slot `slot` made good holding image and the selector sel: the
        start a run sets by a swap at len(damage)."""
        state = bytearray(self.flash_after(start, faults, damage, running=running))
        if slot is not None:
            good = spiflash.build_flash(selector=(0, 1), slots=('good', 'good'),
                                        images=tuple(image if k == slot else None for k in (0, 1)))
            lo, hi = spiflash.SLOT_OFF[slot], spiflash.SLOT_OFF[slot] + spiflash.SLOT_MAX
            state[lo:hi] = good[lo:hi]
            r = spiflash.SLOT_RECORD_OFF[slot]
            state[r:r + 12] = good[r:r + 12]
        if sel is not None:
            state[spiflash.SELECTOR_OFF:spiflash.SELECTOR_OFF + 2] = sel
        return bytes(state)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_unsettled(self):
        """Run 22: slot 0 running, slot 1 bad; run 14's failed in-place write; the selector then set to 00 00
        (swap); the write with flags 01 again, whose settling erase is taken and index program not stored:
        result 11, slot 0 good, the selector FF FF. INFO's bits 5 and 7 set and bit 4 clear, RESTART 0x44.
        BEGIN with flags 01 is way 1 to slot 0: with slot 0's own image no op in slot 0 or record 0, the
        settling by programs alone (recovery_running.json W-21), the selector 00 01, then bit 5 clear and
        RESTART 0; with another image END 18. BEGIN without the flag is a write of slot 1."""
        n = cut_sequences()['image_length']
        img = front_image(n)
        other = front_image(n, seed=24)
        seq = self.running_seqs()['W-03']
        start = spiflash.build_flash(selector=(0, 1), slots=('good', 'badcrc'), images=(IMAGE_A, None))
        seqs = self.running_seqs(rescue=True)
        damage = transfer(img, seq=0x10) + transfer(img, seq=0x20, flags=1)
        swap = (len(damage), self.swapped(start, ['fault fail 1', 'fault late 2'], damage, 0, sel=b'\x00\x00'))
        n14 = len(writes_of(seq['ops']))                       # the in-place write's slot and record ops
        faults = ['fault fail 1', 'fault late 2', 'fault stuck %d' % (2 + n14 + 2)]   # erase taken, index not
        base = damage + transfer(img, seq=0x30, flags=1) + wr(INFO, 0x3E)
        by, res, ops, out = self.rescue_run(start, faults, base + wr(RESTART, 0x3F), ['peek 0xd000 2'], swap,
                                            running=0)
        self.assertEqual(info_fields(by[(INFO, 0xE1)][-1])['result'], 11)
        inf = info_fields(by[(INFO, 0x3E)][0])
        self.assertEqual((inf['sel'], inf['slot_flags'][0] & 1, inf['damaged'], inf['held'], inf['unsettled']),
                         ('ffff', 1, False, True, True))
        self.assertEqual((by[(RESTART, 0x3F)][0], res[2], res[3], out[0]), (bytes([0x44]), '0', '1', 'ffff'))
        k0 = len(writes(ops))
        # the flag with slot 0's own image: the read-back and the settling alone
        tail = wr(INFO, 0x70) + wr(RESTART, 0x71)
        by, res, ops, out = self.rescue_run(start, faults, base + transfer(img, seq=0x41, flags=1) + tail,
                                            ['peek 0xd000 2'], swap, running=0)
        self.assertEqual(by[(BEGIN, 0x41)][0], bytes([0, WAY_PLAIN, 0]))
        w = writes(ops)[k0:]
        self.assertEqual(w, ref_writes(seqs['W-21']['ops']))
        self.assertEqual(w, ['p:d000:1', 'p:d001:1'])
        inf = info_fields(by[(INFO, 0x70)][0])
        self.assertEqual((out[0], inf['held'], inf['unsettled'], by[(RESTART, 0x71)][0], res[2], res[3]),
                         ('0001', False, False, bytes([0]), '1', '0'))
        # another image: 18; without the flag: a write of slot 1
        by, res, ops, out = self.rescue_run(start, faults, base + transfer(other, seq=0x41, flags=1)
                                            + begin(img, seq=0x50), ['peek 0xd000 2'], swap, running=0)
        self.assertEqual((by[(END, 0xE0)][-1][0], writes(ops)[k0:], out[0]), (18, [], 'ffff'))
        self.assertEqual(by[(BEGIN, 0x50)][0], bytes([0, WAY_PLAIN, 1]))

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_settle_by_programs(self):
        """Run 23: slot 1 running and damaged, slot 0 good, the selector 00 03 (swap). Without the flag slot
        0's own image is accepted and the write is the read-back and one program, verified to 01, with no
        erase (W-25); another image is refused 18. With the flag the write goes to slot 1 with no selector
        op before its last program (W-26)."""
        n = cut_sequences()['image_length']
        img = front_image(n)
        b_img = front_image(n, seed=13)
        mine = front_image(n, seed=22)
        seqs = self.running_seqs(rescue=True)
        start = self.running_start(self.running_seqs()['W-08'])          # 01 01, both bad, A slot 1
        damage = transfer(img, seq=0x10) + transfer(img, seq=0x20, flags=1)
        faults = ['fault late 1', 'fault late 2']
        swap = (len(damage), self.swapped(start, faults, damage, 1, slot=0, image=b_img, sel=b'\x00\x03'))
        by, res, ops, out = self.rescue_run(start, faults, damage + wr(INFO, 0x30) + transfer(b_img, seq=0x40),
                                            ['peek 0xd000 2'], swap)
        self.assertEqual((info_fields(by[(INFO, 0x30)][0])['sel'], by[(BEGIN, 0x40)][0]),
                         ('0003', bytes([0, WAY_PLAIN, 0])))
        self.assertEqual((writes(ops)[2:], out[0]), (['p:d001:1'], '0001'))
        self.assertEqual(writes(ops)[2:], ref_writes(seqs['W-25']['ops']))
        by, res, ops, out = self.rescue_run(start, faults, damage + transfer(mine, seq=0x40), ['peek 0xd000 2'],
                                            swap)
        self.assertEqual((by[(END, 0xE0)][-1][0], writes(ops)[2:], out[0]), (18, [], '0003'))
        by, res, ops, out = self.rescue_run(start, faults, damage + transfer(mine, seq=0x40, flags=1),
                                            ['peek 0xd000 2'], swap)
        self.assertEqual(by[(BEGIN, 0x40)][0], bytes([0, WAY_PLAIN, 1]))
        w = writes(ops)[2:]
        self.assertEqual(w, ref_writes(seqs['W-26']['ops']))
        self.assertFalse([o for o in w[:-3] if o.startswith(('e:d000', 'p:d00'))])
        self.assertEqual(out[0], '0101')

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_fallback_selects_first(self):
        """Run 24: slot 0 running and damaged, its record whole, slot 1 good, the selector 00 FE (swap): bit 5
        clear and RESTART 0. A write with the flag is announced as way 2: the selector is erased and written
        01 01 before slot 0's first erase; the write ends with 00 01 (W-27)."""
        n = cut_sequences()['image_length']
        img = front_image(n)
        b_img = front_image(n, seed=13)
        mine = front_image(n, seed=22)
        seqs = self.running_seqs(rescue=True)
        start = spiflash.build_flash(selector=(0, 1), slots=('good', 'badcrc'), images=(IMAGE_A, None))
        damage = transfer(img, seq=0x10) + transfer(img, seq=0x20, flags=1)
        faults = ['fault late 1', 'fault late 2']
        swap = (len(damage), self.swapped(start, faults, damage, 0, slot=1, image=b_img, sel=b'\x00\xfe'))
        by, res, ops, out = self.rescue_run(start, faults, damage + wr(INFO, 0x30) + wr(RESTART, 0x31), (), swap,
                                            running=0)
        inf = info_fields(by[(INFO, 0x30)][0])
        self.assertEqual((inf['sel'], inf['damaged'], inf['held'], by[(RESTART, 0x31)][0], res[3]),
                         ('00fe', True, False, bytes([0]), '0'))
        by, res, ops, out = self.rescue_run(start, faults, damage + transfer(mine, seq=0x40, flags=1),
                                            ['peek 0xd000 2'], swap, running=0)
        self.assertEqual(by[(BEGIN, 0x40)][0], bytes([0, WAY_SELECT_FIRST, 0]))
        w = writes(ops)[2:]
        self.assertEqual(w, ref_writes(seqs['W-27']['ops']))
        self.assertEqual(w[:3], ['e:d000', 'p:d000:1', 'p:d001:1'])
        self.assertLess(w.index('e:d000'), w.index('e:%x' % spiflash.SLOT_OFF[0]))
        self.assertEqual(out[0], '0001')

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_kept_image(self):
        """Run 12: run 9 with the image slot 1 holds, a waffle-boot image with image A's front: written as any
        other, slot 1 selected first, and both slots then hold it and are good."""
        n = cut_sequences()['image_length']
        other = front_image(n, seed=13)
        start = save('rec_run12.bin', spiflash.build_flash(selector=(0, 1), slots=('good', 'good'),
                                                           images=(IMAGE_A, other)))
        frames, phase, ops, out = self.recover(start, 0, transfer(other, flags=1), ['peek 0xd000 2', 'fs 0'])
        begin_reply = [p for t, _, p in frames if t == BEGIN | 0x80][0]
        end = [p for t, _, p in frames if t == END | 0x80][0]
        self.assertEqual((begin_reply, end[0], phase), (bytes([0, WAY_SELECT_FIRST, 0]), 0, PH_DONE))
        self.assertEqual(writes(ops)[0], 'e:d000')                  # slot 1 selected before slot 0's first erase
        self.assertFalse([o for o in writes(ops) if o.startswith('e:%x' % spiflash.SLOT_OFF[1])])
        self.assertEqual((out[2], split(out[3])[0][0]), ('0000', '0'))


F_TRIAL, F_SELECT = 0x02, 0x04
WAY_TRIAL, WAY_SELECT = 4, 5
SLOT_TRIED = 0x08                     # INFO's per-slot flag bit 3
SLOT_STARTED = 0x10                   # INFO's per-slot flag bit 4 (S-02.095)
LOG_AT = spiflash.SLOT_MAX - 0x1000   # a slot's last sector, the start log's (S-02.095)


class RecoveryTrial(unittest.TestCase):
    """SC-RECOVERY runs 25 to 30 on the logic: another front by trial (S-02.084, S-02.093), recovery's
    writer and protocol through h_s02's flash, against recovery_running.json's trial_sequences. Image C
    carries another front. The trial start's first stage is the factory profile's, applied here to the
    flash (spiflash.expected_boot_slot); h_s02's own first stage is the conforming one."""

    @classmethod
    def setUpClass(cls):
        with open(vectors('recovery_running.json'), encoding='utf-8') as f:
            d = json.load(f)
        cls.n = d['image_length']
        cls.seqs = {s['id']: s for s in d['trial_sequences']}
        cls.C = front_image(cls.n, seed=21, front_seed=6)
        cls.B = front_image(cls.n, seed=22)                 # image A's front, another main part

    @staticmethod
    def sel_of(a):
        """The start selector with A selected: 00 01 for slot 0, 01 01 for slot 1."""
        return (0, 1) if a == 0 else (1, 1)

    def start(self, a):
        """A (IMAGE_A) running and selected, the other slot good."""
        images = tuple(IMAGE_A if k == a else None for k in (0, 1))
        return spiflash.build_flash(selector=self.sel_of(a), slots=('good', 'good'), images=images)

    def tried(self, b, sel=None, log=0xFC):
        """After the trial start: A selected and trusted, B holding image C under the trial mark, byte 0 of
        its last sector `log`: FC started (the trial start's mark, S-02.095), FD pending."""
        a = 1 - b
        images = [None, None]
        images[a], images[b] = IMAGE_A, self.C
        recs = [None, None]
        recs[b] = records.SlotRecord(crc.crc32(self.C) ^ 0xFFFFFFFF, len(self.C), records.SLOT_TAG)
        fl = bytearray(spiflash.build_flash(selector=sel or self.sel_of(a), slots=('good', 'good'),
                                            images=tuple(images), records=tuple(recs)))
        fl[spiflash.SLOT_OFF[b] + LOG_AT:spiflash.SLOT_OFF[b] + LOG_AT + 0x1000] = b'\xff' * 0x1000
        fl[spiflash.SLOT_OFF[b] + LOG_AT] = log
        return bytes(fl)

    def recover(self, flash, running, host, name):
        out = run('s02', ['load ' + save(name + '.bin', flash), 'recover %d %s' % (running, save(name + '_host.bin', host)),
                          'save ' + os.path.join(WORK, name + '_after.bin')])
        res, ops = split(out[1])
        frames = wr_parse(bytes.fromhex(res[0]))
        by = {}
        for t, s, p in frames:
            by.setdefault((t & 0x7F, s), []).append(p)
        with open(os.path.join(WORK, name + '_after.bin'), 'rb') as f:
            after = f.read()
        return by, int(res[1]), ops, after

    @staticmethod
    def first_status(by, seq):
        """The first refusal among BEGIN's (seq) and the last END's replies, and BEGIN's whole reply."""
        bg = by[(BEGIN, seq)][0]
        end = by.get((END, 0xE0), [b'\x00'])[-1]
        return bg[0] or end[0], bg

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084', 'S-02.048', 'S-02.093')
    def test_SC_RECOVERY_trial_write(self):
        """Run 25: flags 02 and image C: BEGIN answers way 4 and the other slot; the op log is W-29's or
        W-30's (the log byte FD after the last sector's erase), the selector read back after each step; the
        record is the trial mark; INFO reports the slot tried and not started; a further BEGIN is refused 3,
        a select 19. Run 26 in Python: the factory first stage writes {A, 01}, and the tried slot's front
        makes the mark (FD to FC)."""
        for sid, b, sel_end in (('W-29', 0, '00fe'), ('W-30', 1, 'ffff')):
            a = 1 - b
            seq = self.seqs[sid]
            host = (wr(HELLO, 0) + transfer(self.C, flags=F_TRIAL) + begin(self.C, seq=0x70)
                    + begin(self.C, seq=0x71, flags=F_TRIAL) + begin(self.C, seq=0x72, flags=F_SELECT)
                    + wr(INFO, 0x73))
            by, phase, ops, after = self.recover(self.start(a), a, host, 'trial_%d' % b)
            hello = by[(HELLO, 0)][0]
            self.assertGreaterEqual(struct.unpack_from('<H', hello, 1)[0], 7, sid)        # the front's version
            self.assertEqual(by[(BEGIN, 2)][0], bytes([0, WAY_TRIAL, b]), sid)
            done = info_fields(by[(INFO, 0xE1)][0])
            self.assertEqual((done['phase'], done['written'], done['way'], done['result']),
                             (PH_DONE, b, WAY_TRIAL, 0), sid)
            self.assertEqual(done['slot_flags'][b] & (SLOT_TRIED | SLOT_STARTED | 1), SLOT_TRIED, sid)
            self.assertEqual(after[spiflash.SLOT_OFF[b] + LOG_AT], 0xFD, sid)                  # S-02.095
            self.assertEqual(done['readback'], hashlib.sha256(self.C).digest(), sid)
            # the write's erases and programs are the vector's, nothing else
            self.assertEqual(writes(ops), ref_writes(seq['ops']), sid)
            # S-02.047: the read-back after the record program, before the selector erase
            rec = spiflash.SLOT_RECORD_OFF[b]
            p, e = ops.index('p:%x:c' % rec), ops.index('e:d000')
            self.assertTrue(covered(ops[p + 1:e], rec, rec + 12), sid)
            self.assertTrue(covered(ops[p + 1:e], spiflash.SLOT_OFF[b], spiflash.SLOT_OFF[b] + self.n), sid)
            # S-02.048: each selector step read back before the next
            w = [k for k, o in enumerate(ops) if o[0] in 'ep' and int(o.split(':')[1], 16) in (0xD000, 0xD001)]
            for k, nxt in zip(w, w[1:] + [len(ops)]):
                at = int(ops[k].split(':')[1], 16)
                self.assertTrue(covered(ops[k + 1:nxt], at, at + 1), (sid, ops[k]))
            self.assertEqual(after[0xD000:0xD002].hex(), sel_end, sid)
            self.assertEqual(after[rec:rec + 12], records.SlotRecord(crc.crc32(self.C) ^ 0xFFFFFFFF, self.n,
                                                                     records.SLOT_TAG).pack(), sid)
            lo, hi = spiflash.SLOT_OFF[a], spiflash.SLOT_OFF[a] + spiflash.SLOT_MAX
            self.assertEqual(after[lo:hi], self.start(a)[lo:hi], sid)                        # nothing of A
            self.assertEqual((by[(BEGIN, 0x70)][0][0], by[(BEGIN, 0x71)][0][0], by[(BEGIN, 0x72)][0][0]),
                             (3, 3, 19), sid)
            self.assertEqual(info_fields(by[(INFO, 0x73)][0])['sel'], sel_end, sid)
            # the trial start: the factory first stage writes {A, 01} and boots B once; then A, trusted
            r = spiflash.expected_boot_slot(after, 'factory')
            self.assertEqual((r['boot'], r['result'], r['selector_after']), (b, 'fail', [bytes([1 if b == 0 else 0, 1])]))
            fl = bytearray(after)
            fl[0xD000:0xD002] = r['selector_after'][0]
            self.assertEqual(spiflash.expected_boot_slot(bytes(fl), 'factory')['boot'], a, sid)
            # before the mark a select from the old front is refused 21; the mark, FD to FC, makes B started
            host = wr(INFO, 1) + transfer(self.C, flags=F_SELECT)
            by2, _, ops2, _ = self.recover(bytes(fl), a, host, 'trial_nomark_%d' % b)
            self.assertEqual(info_fields(by2[(INFO, 1)][0])['slot_flags'][b] & (SLOT_TRIED | SLOT_STARTED),
                             SLOT_TRIED, sid)
            self.assertEqual((self.first_status(by2, 2)[0], writes(ops2)), (21, []), sid)
            fl[spiflash.SLOT_OFF[b] + LOG_AT] = 0xFC
            by2, _, _, _ = self.recover(bytes(fl), a, wr(INFO, 1), 'trial_mark_%d' % b)
            self.assertEqual(info_fields(by2[(INFO, 1)][0])['slot_flags'][b] & (SLOT_TRIED | SLOT_STARTED),
                             SLOT_TRIED | SLOT_STARTED, sid)
            self.assertEqual(spiflash.expected_boot_slot(after, 'conforming')['boot'], a, sid)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084', 'S-02.046', 'S-02.093')
    def test_SC_RECOVERY_select(self):
        """Runs 27 and 28: after the trial start, flags 04 and image C, from the old slot's recovery and
        from the tried slot's own: BEGIN answers way 5 and the tried slot; the op log is W-33's to W-36's
        (from the old front, B started, the record sector and the selector; from B's own front, B's entry 0
        pending, that byte programmed FC first); the record is right, the selector {B, 00}; the next reset
        checks the slot and boots it."""
        for b in (0, 1):
            a = 1 - b
            for running, sid in ((a, 'W-%d' % (33 + b)), (b, 'W-%d' % (35 + b))):
                seq = self.seqs[sid]
                host = wr(INFO, 1) + transfer(self.C, flags=F_SELECT)
                log = 0xFC if running == a else 0xFD
                by, phase, ops, after = self.recover(self.tried(b, log=log), running, host,
                                                     'select_%d_%d' % (b, running))
                before = info_fields(by[(INFO, 1)][0])
                self.assertEqual(before['slot_flags'][b] & (SLOT_TRIED | SLOT_STARTED | 1),
                                 SLOT_TRIED | (SLOT_STARTED if log == 0xFC else 0), sid)
                self.assertEqual(by[(BEGIN, 2)][0], bytes([0, WAY_SELECT, b]), sid)
                done = info_fields(by[(INFO, 0xE1)][0])
                self.assertEqual((done['phase'], done['written'], done['way'], done['sel']),
                                 (PH_DONE, b, WAY_SELECT, '%02x00' % b), sid)
                self.assertEqual(done['slot_flags'][b] & (SLOT_TRIED | 1), 1, sid)
                self.assertEqual(writes(ops), ref_writes(seq['ops']), sid)
                rec = spiflash.SLOT_RECORD_OFF[b]
                p, e = ops.index('p:%x:c' % rec), ops.index('e:d000')
                self.assertTrue(covered(ops[p + 1:e], rec, rec + 12), sid)
                self.assertTrue(covered(ops[p + 1:e], spiflash.SLOT_OFF[b], spiflash.SLOT_OFF[b] + self.n), sid)
                self.assertEqual(after[rec:rec + 12], records.SlotRecord.for_image(self.C).pack(), sid)
                self.assertEqual(after[spiflash.SLOT_OFF[b] + LOG_AT], 0xFC, sid)
                r = spiflash.expected_boot_slot(after, 'factory')
                self.assertEqual((r['boot'], r['result']), (b, 'pass'), sid)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084', 'S-02.093')
    def test_SC_RECOVERY_trial_refusals(self):
        """Run 29, each refusal erasing and programming nothing: image C with no flag or with flags 01: 12;
        flags 03, 06, 08: 0x40; flags 02 with the running slot bad (its front intact): 19; flags 02 on
        selector 00 00: 4; a select with no tried slot: 20; after the trial start a select of image C changed
        after FINAL, or of image B: 20. The running front with flags 02: BEGIN announces the trial (way 4), and END
        refuses with 16, writing nothing."""
        start = self.start(0)
        r = random.Random(12)
        main_at = IMAGE_A.index(r.randbytes(64))
        bad = bytearray(start)
        bad[spiflash.SLOT_OFF[0] + main_at] ^= 0x01
        unverified = bytearray(start)
        unverified[0xD001] = 0x00
        changed = self.C[:-1] + bytes([self.C[-1] ^ 1])
        cases = [('no flag', start, 0, transfer(self.C), 12),
                 ('flag 01', start, 0, transfer(self.C, flags=1), 12),
                 ('flags 03', start, 0, begin(self.C, flags=3), 0x40),
                 ('flags 06', start, 0, begin(self.C, flags=6), 0x40),
                 ('flags 08', start, 0, begin(self.C, flags=8), 0x40),
                 ('running slot bad', bytes(bad), 0, transfer(self.C, flags=F_TRIAL), 19),
                 ('verified 00', bytes(unverified), 0, transfer(self.C, flags=F_TRIAL), 4),
                 ('select, none tried', start, 0, transfer(self.C, flags=F_SELECT), 20),
                 ('select, changed', self.tried(1), 0, transfer(changed, flags=F_SELECT), 20),
                 ('select, image B', self.tried(1), 0, transfer(self.B, flags=F_SELECT), 20),
                 ('select, not started', self.tried(1, log=0xFD), 0, transfer(self.C, flags=F_SELECT), 21),
                 ('select, not started, slot 0', self.tried(0, log=0xFD), 1, transfer(self.C, flags=F_SELECT), 21)]
        for name, fl, running, host, want in cases:
            by, phase, ops, _ = self.recover(fl, running, host, 'trial_refused')
            got, _ = self.first_status(by, 2)
            self.assertEqual((got, writes(ops)), (want, []), name)
        by, phase, ops, _ = self.recover(start, 0, transfer(self.B, flags=F_TRIAL), 'trial_same_front')
        self.assertEqual(by[(BEGIN, 2)][0], bytes([0, WAY_TRIAL, 1]))
        self.assertEqual((self.first_status(by, 2)[0], writes(ops)), (16, []))

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_trial_after_write_of_running(self):
        """Run 29: after run 14's failed in-place write image C without a flag is refused 12, and flags 02 and
        04 are refused 19; after run 11's selector write to name the other slot (the first erase of slot 0
        fails late) flags 02 is refused 19. Nothing is written by them."""
        start = spiflash.build_flash(selector=(0, 1), slots=('good', 'badcrc'), images=(IMAGE_A, None))
        img = front_image(self.n)
        pre = transfer(img, seq=0x10) + transfer(img, seq=0x20, flags=1)
        for flags, want in ((None, 12), (F_TRIAL, 19), (F_SELECT, 19)):
            host = pre + transfer(self.C, seq=0x40, flags=flags)
            out = run('s02', ['load ' + save('trial_r14.bin', start), 'fault fail 1', 'fault late 2',
                              'recover 0 ' + save('trial_r14_host.bin', host)])
            res, ops = split(out[3])
            frames = wr_parse(bytes.fromhex(res[0]))
            by = {}
            for t, s, p in frames:
                by.setdefault((t & 0x7F, s), []).append(p)
            got, _ = self.first_status(by, 0x40)
            self.assertEqual(got, want, flags)
            self.assertEqual(writes(ops), ['e:%x' % spiflash.SLOT_OFF[1], 'e:%x' % spiflash.SLOT_OFF[0]], flags)
        start9 = spiflash.build_flash(selector=(0, 1), slots=('good', 'good'), images=(IMAGE_A, None))
        host = transfer(img, flags=1) + transfer(self.C, seq=0x40, flags=F_TRIAL)
        out = run('s02', ['load ' + save('trial_r11.bin', start9), 'fault late 3',
                          'recover 0 ' + save('trial_r11_host.bin', host)])
        res, ops = split(out[2])
        frames = wr_parse(bytes.fromhex(res[0]))
        by = {}
        for t, s, p in frames:
            by.setdefault((t & 0x7F, s), []).append(p)
        got, _ = self.first_status(by, 0x40)
        self.assertEqual(got, 19)
        self.assertEqual(writes(ops), ['e:d000', 'p:d000:2', 'e:%x' % spiflash.SLOT_OFF[0]])

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084')
    def test_SC_RECOVERY_trial_faults(self):
        """Run 25's faults: the log byte's program fails: result 9, no record or selector op. A selector step
        fails (the erase; for slot 0 also each program): the selector is settled on A, {A, 01} or, where it
        already names slot 1 as A, left to name it, and the result is 11; the next reset boots A, and a
        select from the old front is then refused 21, nothing written."""
        for sid, b in (('W-29', 0), ('W-30', 1)):
            a = 1 - b
            seq = self.seqs[sid]
            w = [o for o in seq['ops'] if o['op'] != 'read']
            k_log = next(i for i, o in enumerate(w, 1) if o['class'] == 'trial_log')
            host = transfer(self.C, flags=F_TRIAL)
            out = run('s02', ['load ' + save('trial_f.bin', self.start(a)), 'fault fail %d' % k_log,
                              'recover %d %s' % (a, save('trial_f_host.bin', host))])
            res, ops = split(out[2])
            frames = wr_parse(bytes.fromhex(res[0]))
            done = info_fields([p for t, _, p in frames if t == INFO | 0x80][-1])
            self.assertEqual(done['result'], 9, sid)
            self.assertFalse([o for o in writes(ops) if o.startswith(('e:%x' % spiflash.SLOT_RECORD_OFF[b],
                                                                      'p:%x' % spiflash.SLOT_RECORD_OFF[b],
                                                                      'e:d000', 'p:d00'))], sid)
            for k in [i for i, o in enumerate(w, 1) if o['class'] in ('trial_erase', 'trial_verified', 'trial_index')]:
                out = run('s02', ['load ' + save('trial_f.bin', self.start(a)), 'fault fail %d' % k,
                                  'recover %d %s' % (a, save('trial_f_host.bin', host)),
                                  'save ' + os.path.join(WORK, 'trial_f_after.bin')])
                res, ops = split(out[2])
                frames = wr_parse(bytes.fromhex(res[0]))
                done = info_fields([p for t, _, p in frames if t == INFO | 0x80][-1])
                self.assertEqual(done['result'], 11, (sid, k))
                with open(os.path.join(WORK, 'trial_f_after.bin'), 'rb') as f:
                    after = bytearray(f.read())
                self.assertEqual(after[0xD000:0xD002].hex(), done['sel'], (sid, k))
                if a == 0:
                    self.assertEqual(bytes(after[0xD000:0xD002]), bytes([0, 1]), (sid, k))
                else:
                    self.assertNotEqual(after[0xD000], 0, (sid, k))        # names slot 1, which is A
                # the reset after it: the first stage boots A and leaves it trusted
                r = spiflash.expected_boot_slot(bytes(after), 'factory')
                self.assertEqual((r['boot'], len(r['selector_after'])), (a, 1), (sid, k))
                after[0xD000:0xD002] = r['selector_after'][0]
                self.assertEqual(after[0xD001], 1, (sid, k))
                after = bytes(after)
                by, _, ops2, _ = self.recover(after, a, transfer(self.C, flags=F_SELECT), 'trial_f_sel')
                self.assertEqual((self.first_status(by, 2)[0], writes(ops2)), (21, []), (sid, k))

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.052')
    def test_SC_RECOVERY_trial_cuts(self):
        """Run 30 on the logic: the trial write and the select with a cut after each write op in turn
        (W-29, W-30, W-33, W-34); the flash left is classified by spiflash's slot states and the selector,
        and three factory resets from it boot the vector row's factory_boots (a select's rows, B tried at
        the start, likewise)."""
        cases = [('W-29', self.start(1), 1, transfer(self.C, flags=F_TRIAL)),
                 ('W-30', self.start(0), 0, transfer(self.C, flags=F_TRIAL)),
                 ('W-33', self.tried(0), 1, transfer(self.C, flags=F_SELECT)),
                 ('W-34', self.tried(1), 0, transfer(self.C, flags=F_SELECT))]
        n_rows = 0
        for sid, fl, running, host in cases:
            seq = self.seqs[sid]
            path, hpath = save('trial_cut.bin', fl), save('trial_cut_host.bin', host)
            rows = [c for c in seq['cuts'] if c['kind'] == 'after']
            cmds = []
            for c in rows:
                w = sum(1 for o in seq['ops'][:c['op_n']] if o['op'] != 'read')
                cmds += ['load ' + path, 'fault after %d' % w, 'recover %d %s' % (running, hpath),
                         'save ' + os.path.join(WORK, 'trial_cut_%d.bin' % c['op_n'])]
            run('s02', cmds)
            for c in rows:
                with open(os.path.join(WORK, 'trial_cut_%d.bin' % c['op_n']), 'rb') as f:
                    data = f.read()
                boots = []
                for _ in range(3):
                    r = spiflash.expected_boot_slot(data, 'factory')
                    boots.append(str(r['boot']))
                    data = bytearray(data)
                    data[0xD000:0xD002] = r['selector_after'][0]
                    data = bytes(data)
                self.assertEqual(' '.join(boots), c['factory_boots'], (sid, c['op_n']))
                n_rows += 1
            if seq['way'] != 'trial':
                continue
            # S-02.084: no cut of the trial write leaves the tried slot started; a select from the old front
            # after a reset that put {A, 01} back is refused, and writes nothing
            sel_host = save('trial_cut_sel.bin', transfer(self.C, flags=F_SELECT))
            cmds = []
            for c in rows:
                with open(os.path.join(WORK, 'trial_cut_%d.bin' % c['op_n']), 'rb') as f:
                    data = bytearray(f.read())
                data[0xD000:0xD002] = bytes(self.sel_of(running))
                cmds += ['load ' + save('trial_cut_r_%d.bin' % c['op_n'], bytes(data)),
                         'recover %d %s' % (running, sel_host)]
            out = run('s02', cmds)
            for c, line in zip(rows, out[1::2]):
                res, ops = split(line)
                frames = wr_parse(bytes.fromhex(res[0]))
                bg = [p for t, _, p in frames if t == BEGIN | 0x80][0]
                self.assertIn(bg[0], (20, 21), (sid, c['op_n']))
                self.assertEqual(writes(ops), [], (sid, c['op_n']))
        self.assertGreater(n_rows, 40)


ACT = 0x07
F_FORCE = 0x80
PH_COUNTDOWN = 5
WAY_TRY, WAY_SWITCH, WAY_KEEP, WAY_PUT_BACK = 6, 7, 8, 9
ACT_NUM = {'try': 1, 'switch': 2, 'keep': 3, 'put_back': 4}


def act(n, seq, flags=0):
    """ACT {action, flags} (S-02.093)."""
    return wr(ACT, seq, bytes([n, flags]))


def info_more(p):
    """INFO's fields from 219 (S-02.093)."""
    return {'count': p[219], 'act': p[220], 'tier': p[221], 'acts': list(p[222:226]),
            'forced': list(p[226:230]), 'keep': p[230], 'refused': p[231]}


class RecoveryActions(unittest.TestCase):
    """SC-RECOVERY runs 32 to 41: the actions on a slot's image from the host (ACT), force, the countdown,
    INFO's fields from 219 and HELLO, through recovery's protocol (src/front/) on h_s02's flash, against
    S-02's vectors/recovery_actions.json."""

    @classmethod
    def setUpClass(cls):
        with open(vectors('recovery_actions.json'), encoding='utf-8') as f:
            cls.V = json.load(f)
        cls.B = front_image(0x1800, seed=22)                    # image A's front, another main part
        cls.BIG = front_image(0x77800, seed=23)                 # over 0x77000: its record gives no log
        cls.C = front_image(0x1800, seed=21, front_seed=6)      # another front
        cls.D = front_image(0x1800, seed=24)                    # image A's front, held by neither slot

    def flash(self, a, kind, start='at_home', image=None):
        """A running (IMAGE_A, good or, in its trial start, tried with its entry pending), B as kind
        (recovery_actions.json's b_kind) holding image B, the selector as start says."""
        b = 1 - a
        img_b = image or (self.BIG if kind == 'good_nolog' else self.B)
        images = [None, None]
        images[a], images[b] = IMAGE_A, img_b
        recs = [None, None]
        states = ['good', 'good']
        if kind in ('tried', 'started'):
            recs[b] = records.SlotRecord(crc.crc32(img_b) ^ 0xFFFFFFFF, len(img_b), records.SLOT_TAG)
        elif kind in ('badcrc', 'empty', 'badlen', 'erased'):
            states[b] = kind
        if start == 'trial_start':
            recs[a] = records.SlotRecord(crc.crc32(IMAGE_A) ^ 0xFFFFFFFF, len(IMAGE_A), records.SLOT_TAG)
            sel = (b, 1) if b == 1 else (0, 1)
        elif start == 'after_try':
            sel = None if b == 1 else (0x00, 0xFE)
        else:
            sel = (a, 1) if a == 1 else (0, 1)
        fl = bytearray(spiflash.build_flash(selector=sel, slots=tuple(states), images=tuple(images),
                                            records=tuple(recs)))
        log = {'good_pending': 0xFD, 'tried': 0xFD, 'started': 0xFC}.get(kind)
        if log is not None:
            fl[spiflash.SLOT_OFF[b] + LOG_AT:spiflash.SLOT_OFF[b] + LOG_AT + 0x1000] = b'\xff' * 0x1000
            fl[spiflash.SLOT_OFF[b] + LOG_AT] = log
        if start == 'trial_start':
            fl[spiflash.SLOT_OFF[a] + LOG_AT:spiflash.SLOT_OFF[a] + LOG_AT + 0x1000] = b'\xff' * 0x1000
            fl[spiflash.SLOT_OFF[a] + LOG_AT] = 0xFD
        return bytes(fl)

    def recover(self, flash, running, host, name, opts=''):
        out = run('s02', ['load ' + save(name + '.bin', flash),
                          ('recover %d %s %s' % (running, save(name + '_host.bin', host), opts)).strip(),
                          'save ' + os.path.join(WORK, name + '_after.bin')])
        res, ops = split(out[1])
        frames = wr_parse(bytes.fromhex(res[0])) if res[0] != '-' else []
        by = {}
        for t, sq, pl in frames:
            by.setdefault((t & 0x7F, sq), []).append(pl)
        with open(os.path.join(WORK, name + '_after.bin'), 'rb') as f:
            after = f.read()
        accept = None if res[4] == '-' else int(res[4])
        first = None if res[5] == '-' else int(res[5])
        return by, int(res[1]), ops, after, accept, first

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084', 'S-02.093', 'S-02.048', 'S-02.050')
    def test_SC_RECOVERY_actions(self):
        """Runs 32 to 35: every row of recovery_actions.json from the host: unforced, the row's status; where
        that refuses and force accepts, forced; ACT's reply (status, way, slot, tier), exactly the row's erases
        and programs in order, INFO's done state, act and way; a refusal writes nothing."""
        ways = {'try': WAY_TRY, 'switch': WAY_SWITCH, 'keep': WAY_KEEP, 'put_back': WAY_PUT_BACK}
        n = 0
        for c in self.V['cases']:
            a, kind, start, name = c['a'], c['b_kind'], c['start'], c['act']
            what = (name, a, kind, start)
            fl = self.flash(a, kind, start)
            host = wr(HELLO, 0) + act(ACT_NUM[name], 0x10) + wr(INFO, 0x11)
            by, phase, ops, after, accept, first = self.recover(fl, a, host, 'act')
            reply = by[(ACT, 0x10)][0]
            self.assertEqual(reply[0], c['status'], what)
            if c['status'] != 0:
                self.assertEqual(writes(ops), [], what)
                if c['status_forced'] != 0:
                    continue
                host = wr(HELLO, 0) + act(ACT_NUM[name], 0x10, F_FORCE) + wr(INFO, 0x11)
                by, phase, ops, after, accept, first = self.recover(fl, a, host, 'act_forced')
                reply = by[(ACT, 0x10)][0]
            self.assertEqual(tuple(reply[:3]), (0, ways[name], c['target']), what)
            self.assertEqual([o for o in writes(ops)], ref_writes(c['ops']), what)
            done = info_fields(by[(INFO, 0x11)][0])
            more = info_more(by[(INFO, 0x11)][0])
            self.assertEqual((done['phase'], done['way'], done['written'], more['act']),
                             (PH_DONE, ways[name], c['target'], ACT_NUM[name]), what)
            n += 1
        self.assertGreater(n, 40)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084', 'S-02.093')
    def test_SC_RECOVERY_tiers(self):
        """Run 40: the tier a try reports: 1 the running front, 2 another front, 3 no waffle-boot front, 4 a bad
        record; a switch to tier 2 or 3 is refused 12 without force."""
        nofront = ldr.pack_blocks([ldr.Block(frozenset({'FIRST'}), 0x08002000, 0, 0),
                                   ldr.Block(frozenset({'INIT'}), 0x11A04000, 16, 0, bytes(16)),
                                   ldr.Block(frozenset({'FINAL'}), 0x08002000, 0, 0)]) + bytes(0x200)
        for a in (0, 1):
            for kind, image, tier in (('good', None, 1), ('good', self.C, 2), ('good', nofront, 3),
                                      ('badcrc', None, 4)):
                fl = self.flash(a, kind, image=image)
                by, *_ = self.recover(fl, a, act(1, 0x10, F_FORCE if tier == 4 else 0), 'tier')
                self.assertEqual(by[(ACT, 0x10)][0][0], 0, (a, tier))
                self.assertEqual(by[(ACT, 0x10)][0][3], tier, (a, tier))
                if tier in (2, 3):
                    fl = self.flash(a, kind, image=image)
                    by, *_ = self.recover(fl, a, act(2, 0x20), 'tier_sw')
                    self.assertEqual(by[(ACT, 0x20)][0][:1], bytes([12]), (a, tier))

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084', 'S-02.093')
    def test_SC_RECOVERY_countdown(self):
        """Run 37 and 41: an accepted ACT or END waits 15 s of the harness's clock before its first erase or
        program; INFO during it reports phase 5 and the seconds left, and every other request is answered 1;
        the host leaving during it stops it with status 24 and nothing written."""
        for a in (0, 1):
            fl = self.flash(a, 'good')
            host = (act(1, 0x10) + wr(INFO, 0x11) + begin(self.B, seq=0x12) + act(2, 0x13)
                    + wr(RESTART, 0x14) + wr(HELLO, 0x15))
            by, phase, ops, after, accept, first = self.recover(fl, a, host, 'count', opts='during')
            self.assertEqual(by[(ACT, 0x10)][0][0], 0)
            mid = by[(INFO, 0x11)][0]
            self.assertEqual(mid[7], PH_COUNTDOWN)
            self.assertTrue(1 <= info_more(mid)['count'] <= 15, info_more(mid))
            for sq in (0x12, 0x13, 0x14):
                self.assertEqual(by[(BEGIN if sq == 0x12 else ACT if sq == 0x13 else RESTART, sq)][0][0], 1)
            self.assertEqual(by[(HELLO, 0x15)][0][0], 2)
            self.assertIsNotNone(first)
            self.assertGreaterEqual(first - accept, 15000, (accept, first))
            self.assertEqual(phase, PH_DONE)
            # the default write too
            host = transfer(self.B, flags=None)
            by, phase, ops, after, accept, first = self.recover(self.flash(a, 'good'), a, host, 'count_w')
            self.assertGreaterEqual(first - accept, 15000, (accept, first))
            # the host leaves in the countdown: nothing written, status 24
            by, phase, ops, after, accept, first = self.recover(fl, a, act(1, 0x10) + wr(INFO, 0x11),
                                                                'count_leave', opts='leave')
            self.assertEqual(writes(ops), [])
            self.assertIsNone(first)
            self.assertEqual(after, fl)
            inf = info_fields(by[(INFO, 0x11)][0])
            self.assertEqual((inf['phase'], inf['result']), (PH_FAILED, 24))
            # the host leaves in the middle of a transfer: the same; the transfer begun again writes
            host = transfer(self.D) + transfer(self.D, seq=0x40)
            by, phase, ops, after, accept, first = self.recover(fl, a, host, 'count_leave_rx',
                                                                opts='leaverx eager')
            inf = info_fields(by[(INFO, 0xE1)][0])
            self.assertEqual((inf['phase'], inf['result']), (PH_FAILED, 24))
            self.assertEqual(by[(END, 0xE0)][0][0], 0x41)
            self.assertEqual((by[(BEGIN, 0x40)][0][0], by[(END, 0xE0)][-1][0], phase), (0, 0, PH_DONE))
            self.assertEqual(after[spiflash.SLOT_OFF[1 - a]:spiflash.SLOT_OFF[1 - a] + len(self.D)], self.D)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.084', 'S-02.093')
    def test_SC_RECOVERY_force(self):
        """Run 36: force lifts 3 (with the selected slot good) and 4, 12 (another front, no trial), 7 with
        result 7 (no waffle-boot front) and 15 (in place first); keeps 5 under a selector that names a bad
        slot, 6, 22 and 0x40 for an undefined flag bit; the kept slot's own image is written."""
        nofront = ldr.pack_blocks([ldr.Block(frozenset({'FIRST'}), 0x08002000, 0, 0),
                                   ldr.Block(frozenset({'INIT'}), 0x11A04000, 16, 0, bytes(16)),
                                   ldr.Block(frozenset({'FINAL'}), 0x08002000, 0, 0)]) + bytes(0x200)
        for a in (0, 1):
            b = 1 - a
            images = [None, None]
            images[a], images[b] = IMAGE_A, self.B
            for sel, refusal, target in (((b, 1) if b else (0, 1), 3, a), ((a, 0), 4, b)):
                fl = spiflash.build_flash(selector=sel, slots=('good', 'good'), images=tuple(images))
                by, *_ = self.recover(fl, a, begin(self.D, seq=0x10), 'force_sel')
                self.assertEqual(by[(BEGIN, 0x10)][0][0], refusal, (a, sel))
                by, phase, ops, after, *_ = self.recover(fl, a, transfer(self.D, flags=F_FORCE), 'force_sel_w')
                self.assertEqual(by[(BEGIN, 2)][0][0], 0, (a, sel))
                self.assertEqual(by[(BEGIN, 2)][0][2], target, (a, sel))
                self.assertEqual(phase, PH_DONE, (a, sel))
            # the selector naming a bad B: 3 without force, 5 with it, nothing written
            sel = (b, 1) if b else (0, 1)
            fl = spiflash.build_flash(selector=sel, slots=tuple('badcrc' if k == b else 'good' for k in (0, 1)),
                                      images=tuple(images))
            for flags, st in ((None, 3), (F_FORCE, 5), (0x81, 5)):
                by, phase, ops, after, *_ = self.recover(fl, a, transfer(self.D, flags=flags), 'force_5')
                self.assertEqual((by[(BEGIN, 2)][0][0], writes(ops)), (st, []), (a, flags))
            fl = self.flash(a, 'good')
            for img, st, irc in ((self.C, 12, 0), (nofront, 7, 7)):
                by, *_ = self.recover(fl, a, transfer(img), 'force_img')
                got = by[(BEGIN, 2)][0][0] or by[(END, 0xE0)][-1][0]
                self.assertEqual(got, st, (a, st))
                if (END, 0xE0) in by and by[(END, 0xE0)][-1][0] == st:
                    self.assertEqual(by[(END, 0xE0)][-1][1], irc, (a, st))
                by, phase, *_ = self.recover(fl, a, transfer(img, flags=F_FORCE), 'force_img_w')
                self.assertEqual(phase, PH_DONE, (a, st))
            bad = self.flash(a, 'badcrc')                       # B bad, no write of B failed
            by, *_ = self.recover(bad, a, begin(self.B, seq=0x10, flags=0x01), 'force_15')
            self.assertEqual(by[(BEGIN, 0x10)][0][0], 15)
            by, *_ = self.recover(bad, a, begin(self.B, seq=0x10, flags=0x81), 'force_15_w')
            self.assertEqual(tuple(by[(BEGIN, 0x10)][0]), (0, WAY_IN_PLACE, a))
            # kept under force
            by, *_ = self.recover(fl, a, begin(self.B, seq=0x10, length=0x77001, flags=F_FORCE), 'force_6')
            self.assertEqual(by[(BEGIN, 0x10)][0][0], 6)
            by, *_ = self.recover(fl, a, transfer(IMAGE_A, flags=F_FORCE), 'force_same')  # the kept slot's own
            self.assertEqual((by[(BEGIN, 2)][0][0], by[(END, 0xE0)][-1][0]), (0, 0))
            trust = self.flash(a, 'good', start='trial_start')
            by, *_ = self.recover(trust, a, act(1, 0x10, F_FORCE) + act(2, 0x11, F_FORCE), 'force_22',
                                  opts='eager')
            self.assertEqual((by[(ACT, 0x10)][0][0], by[(ACT, 0x11)][0][0]), (22, 22))
            by, *_ = self.recover(fl, a, act(1, 0x10, 0x40) + wr(RESTART, 0x11, b'\x01'), 'force_40',
                                  opts='eager')
            self.assertEqual((by[(ACT, 0x10)][0][0], by[(RESTART, 0x11)][0][0]), (0x40, 0x40))
            by, phase, ops, after, accept, first = self.recover(fl, a, wr(RESTART, 0x11, b'\x80'), 'force_rs')
            self.assertEqual(by[(RESTART, 0x11)][0][0], 0)

    @scenario('S-02/SC-RECOVERY')
    @requirement('S-02.093')
    def test_SC_RECOVERY_info_and_hello(self):
        """Runs 38 and 39: HELLO says protocol 2 and front 23; INFO is 242 bytes, its fields from 219 the status
        each action would get without and with force (as recovery_actions.json's rows), the tier of B, the
        slot a keep would select, and the last refusal since a write."""
        rows = {(c['act'], c['a'], c['b_kind'], c['start']): c for c in self.V['cases']}
        for c in self.V['cases']:
            if c['act'] != 'try':
                continue
            a, kind, start = c['a'], c['b_kind'], c['start']
            by, *_ = self.recover(self.flash(a, kind, start), a, wr(HELLO, 0) + wr(INFO, 1), 'info')
            hello = by[(HELLO, 0)][0]
            self.assertEqual((hello[0], struct.unpack_from('<H', hello, 1)[0], len(hello)), (2, 23, 47))
            p = by[(INFO, 1)][0]
            self.assertEqual(len(p), INFO_LEN)
            more = info_more(p)
            want = [rows[(n, a, kind, start)]['status'] for n in ('try', 'switch', 'keep', 'put_back')]
            wantf = [rows[(n, a, kind, start)]['status_forced'] for n in ('try', 'switch', 'keep', 'put_back')]
            self.assertEqual((more['acts'], more['forced']), (want, wantf), (a, kind))
            keep = rows[('keep', a, kind, start)]
            self.assertEqual(more['keep'], keep['target'] if keep['status_forced'] == 0 else 0xFF, (a, kind))
            self.assertEqual((more['count'], more['act'], more['refused']), (0, 0, 0))
        # the last refusal since a write
        fl = self.flash(0, 'good')
        by, *_ = self.recover(fl, 0, act(2, 0x10) + act(1, 0x11) + wr(INFO, 0x12), 'info_ref')
        self.assertEqual(by[(ACT, 0x11)][0][0], 3)                   # the switch left {B, 00}: no longer at home
        self.assertEqual(info_more(by[(INFO, 0x12)][0])['refused'], 3)


class StartLog(unittest.TestCase):
    """SC-STARTLOG on the logic: the front's step for the start log (src/front/fr_startlog.c, fr_log_start())
    through h_s02's flash, against vectors/start_log.csv. The hand-off record, the main image's program and
    the ways in that skip the step (run 5) are the unit tests' (test_front_startlog, test_front_start)."""

    STEP = {'main_entry': 'marked', 'recovery': 'recovery', 'main': 'go'}

    @staticmethod
    def log_base(slot):
        return spiflash.SLOT_OFF[slot] + spiflash.SLOT_MAX - 0x1000

    def flash(self, sel, tag, length, sector, slot=None):
        """The selected slot's record {crc 0, length, tag} and log sector; the other slot's record erased and
        its last sector a full log, so that reading the wrong slot shows."""
        s = (0 if sel[0] == 0 else 1) if slot is None else slot
        fl = bytearray(spiflash.build_flash(selector=sel, slots=('good', 'good')))
        o = 1 - s
        fl[spiflash.SLOT_RECORD_OFF[s]:spiflash.SLOT_RECORD_OFF[s] + 12] = struct.pack('<III', 0, length, tag)
        fl[spiflash.SLOT_RECORD_OFF[o]:spiflash.SLOT_RECORD_OFF[o] + 12] = b'\xff' * 12
        fl[self.log_base(s):self.log_base(s) + 0x1000] = sector
        fl[self.log_base(o):self.log_base(o) + 0x1000] = bytes(0x1000)
        return bytes(fl)

    def rows(self, which=None):
        G = spec_module('S-02-spi-flash', 'gen_vectors')
        out = []
        for r in table('start_log.csv'):
            if which is not None and not which(r):
                continue
            sel = tuple(b(r['selector']))
            s = 0 if sel[0] == 0 else 1
            fl = self.flash(sel, int(r['record_tag'], 16), int(r['record_length'], 16),
                            G.log_sector(r['log'], r['patch']))
            cmds = ['load ' + save('log_%d.bin' % len(out), fl)]
            f = r['fault']
            if f == 'read_selector':
                cmds.append('readfail 0xd000 2')
            elif f == 'read_record':
                cmds.append('readfail 0x%x 12' % spiflash.SLOT_RECORD_OFF[s])
            elif f == 'read_log':
                cmds.append('readfail 0x%x 0x1000' % self.log_base(s))
            elif f in ('program', 'stuck'):
                cmds.append('fault %s 1' % ('fail' if f == 'program' else 'stuck'))
            else:
                cmds.append('fault none')
            cmds.append('startlog')
            at = int(r['program'].split(':')[0], 16) if r['program'] != '-' else self.log_base(s)
            cmds += ['readfail -', 'peek 0x%x 1' % at]
            out.append((r, cmds))
        return out

    def check(self, rows):
        out = run('s02', [c for _, cmds in rows for c in cmds])
        for k, (r, _) in enumerate(rows):
            what = r['case']
            o = out[5 * k:5 * k + 5]
            res, ops = split(o[2])
            self.assertEqual(res[0], self.STEP[r['starts']], what)
            if r['program'] == '-':
                self.assertEqual(writes(ops), [], what)
                continue
            off = int(r['program'].split(':')[0], 16)
            self.assertEqual(writes(ops), ['p:%x:1' % off], what)              # one 1-byte program, no erase
            self.assertEqual(o[4], r['byte_after'], what)
            if r['fault'] == 'none':
                p = ops.index('p:%x:1' % off)
                self.assertIn('r:%x:1' % off, ops[p + 1:], what)                # read back
            if r['state'] == 'ok':
                self.assertEqual((res[1], res[2], res[3], res[4]),
                                 ('ok', r['slot'], r['next'], r['unfinished']), what)
        return out

    @scenario('S-02/SC-STARTLOG')
    @requirement('S-02.095')
    def test_SC_STARTLOG_rows(self):
        """Run 3: every row of start_log.csv: the program, or none, and what starts."""
        self.check(self.rows())

    @scenario('S-02/SC-STARTLOG')
    @requirement('S-02.095')
    def test_SC_STARTLOG_faults(self):
        """Run 6: a program that fails and one whose byte reads back unchanged, from an empty log and from
        three unfinished entries: the main image starts with no entry named."""
        rows = self.rows(lambda r: r['fault'] in ('program', 'stuck'))
        self.assertEqual(len(rows), 4)
        out = self.check(rows)
        for k in range(len(rows)):
            self.assertEqual(split(out[5 * k + 2])[0][0], 'go')

    def sequence(self, sel, n):
        """n starts, none of them finished, from an erased log: each start's result, ops and the byte."""
        s = 0 if sel[0] == 0 else 1
        base = self.log_base(s)
        fl = spiflash.build_flash(selector=sel, slots=('good', 'good'))
        cmds = ['load ' + save('log_seq.bin', fl)]
        for _ in range(n):
            cmds += ['startlog', 'peek 0x%x 1' % base]
        out = run('s02', cmds)
        return [(split(out[1 + 2 * k]), out[2 + 2 * k]) for k in range(n)]

    @scenario('S-02/SC-STARTLOG')
    @requirement('S-02.095')
    def test_SC_STARTLOG_one_start(self):
        """Run 1, the front's half: one 1-byte program, FE at 0x87000, then a read of that byte; the step names
        slot 0, entry 0."""
        (res, ops), byte = self.sequence((0, 1), 1)[0]
        self.assertEqual(res[:5], ['marked', 'ok', '0', '0', '0'])
        self.assertEqual(writes(ops), ['p:87000:1'])
        self.assertIn('r:87000:1', ops[ops.index('p:87000:1') + 1:])
        self.assertEqual(byte, 'fe')

    @scenario('S-02/SC-STARTLOG')
    @requirement('S-02.095')
    def test_SC_STARTLOG_three_starts(self):
        """Run 2: three starts that do not finish leave EA; the fourth programs DF (CA) and enters recovery; the
        fifth programs BF (8A) and starts the main image."""
        got = [(res[0], writes(ops), byte) for (res, ops), byte in self.sequence((0, 1), 5)]
        self.assertEqual(got, [('marked', ['p:87000:1'], 'fe'), ('marked', ['p:87000:1'], 'fa'),
                               ('marked', ['p:87000:1'], 'ea'), ('recovery', ['p:87000:1'], 'ca'),
                               ('marked', ['p:87000:1'], '8a')])

    @scenario('S-02/SC-STARTLOG')
    @requirement('S-02.095')
    def test_SC_STARTLOG_slot_1(self):
        """Run 4: selector 01 01: run 1 at 0xFF000, and no erase or program below it."""
        seq = self.sequence((1, 1), 4)
        (res, ops), byte = seq[0]
        self.assertEqual(res[:5], ['marked', 'ok', '1', '0', '0'])
        self.assertEqual((writes(ops), byte), (['p:ff000:1'], 'fe'))
        self.assertEqual([x[0][0][0] for x in seq], ['marked', 'marked', 'marked', 'recovery'])
        for (res, ops), _ in seq:
            self.assertFalse([o for o in writes(ops) if int(o.split(':')[1], 16) < 0xFF000], res)

    @scenario('S-02/SC-STARTLOG')
    @requirement('S-02.045')
    def test_SC_STARTLOG_writers_erase(self):
        """Run 7: after SC-UPG-OK's upgrade and after SC-RECOVERY's run 1 the target slot's last sector reads
        erased, where it held a log before too."""
        n = cut_sequences()['image_length']
        log = bytes.fromhex('eaca8a') + bytes(0x20)
        fl = bytearray(spiflash.build_flash(selector=(1, 1), slots=('good', 'good')))
        fl[self.log_base(0):self.log_base(0) + len(log)] = log
        out = run('s02', ['load ' + save('log_upg.bin', bytes(fl)), 'upgrade 1 ' + save('upg_b.bin', bootable(n, 2)),
                          'peek 0x%x 0x1000' % self.log_base(0)])
        self.assertEqual(split(out[1])[0], ['0', '0'])
        self.assertEqual(out[2], 'ff' * 0x1000)
        fl = bytearray(spiflash.build_flash(selector=(0, 1), slots=('good', 'good'), images=(IMAGE_A, None)))
        fl[self.log_base(1):self.log_base(1) + len(log)] = log
        out = run('s02', ['load ' + save('log_rec.bin', bytes(fl)),
                          'recover 0 ' + save('rec_host.bin', transfer(front_image(n))),
                          'peek 0x%x 0x1000' % self.log_base(1)])
        self.assertEqual(split(out[1])[0][1], str(PH_DONE))
        self.assertEqual(out[2], 'ff' * 0x1000)


def set_vectors():
    with open(vectors('settings_sector.json'), encoding='utf-8') as f:
        return json.load(f)


SET_SECTOR = (0x86000, 0xFE000)
SET_RECORD = (0xE000, 0xF000)
# enum s02_store_rc
STORE_OK, STORE_UNUSABLE, STORE_IO, STORE_VERIFY = 0, 1, 2, 3


class SettingsSector(unittest.TestCase):
    """SC-SETTINGS on the logic: src/s02_store.c (s02_store_read(), s02_store_write(), s02_store_clear())
    through h_s02's flash, against vectors/settings_sector.json, for both slots. The running slot's
    establishment and its image's check are the start's (S-01.002, S-02.016) and are given to each command; what
    a start does with Start in boot menu is S-05's (test_s05.py StartInMenu); the Settings screen and the factory
    reset's screen are S-15's and are not driven here."""

    @staticmethod
    def rec(stamp, length=0x2345, tag=0x0000B004):
        return struct.pack('<III', stamp, length, tag)

    def flash(self, slot, stamp, sector=None, pattern=None, rec=None):
        """A flash with slot `slot`'s record holding `stamp`, its settings sector holding `sector` (a {page:
        hex} map, erased elsewhere) or the vectors' pattern bytes, and something in every other byte of both
        slots' last two sectors to see them untouched."""
        fl = bytearray(b'\xff' * 0x100000)
        fl[SET_RECORD[slot]:SET_RECORD[slot] + 12] = self.rec(stamp) if rec is None else rec
        for k in (0, 1):
            fl[SET_SECTOR[k]:SET_SECTOR[k] + 0x2000] = random.Random(k).randbytes(0x2000)
        sec = bytearray(b'\xff' * 0x1000)
        if pattern is not None:
            sec[:] = bytes(((i * 7 + pattern) ^ (i >> 8)) & 0xFF for i in range(0x1000))
        for pg, hexb in (sector or {}).items():
            d = b(hexb)
            sec[int(pg) * 0x100:int(pg) * 0x100 + len(d)] = d
        fl[SET_SECTOR[slot]:SET_SECTOR[slot] + 0x1000] = sec
        return bytes(fl)

    @staticmethod
    def in_sector(op, slot):
        f = op.split(':')
        lo = int(f[1], 16)
        hi = lo + (int(f[2], 16) if len(f) > 2 else 0x1000)
        return SET_SECTOR[slot] <= lo and hi <= SET_SECTOR[slot] + 0x1000

    @staticmethod
    def want_ops(ops):
        out = []
        for o in ops:
            if o[0] == 'read':
                out.append('r:%x:%x' % (int(o[1], 16), o[2]))
            elif o[0] == 'erase':
                out.append('e:%x' % int(o[1], 16))
            else:
                out.append('p:%x:%x' % (int(o[1], 16), len(b(o[2]))))
        return out

    @scenario('S-02/SC-SETTINGS')
    @requirement('S-02.096', 'S-02.097')
    def test_SC_SETTINGS_reads(self):
        """Every row of `reads` in each slot's sector with the row's stamp in that slot's record: the flags
        and validity the row gives; the reads are the slot's record and one read of the sector; nothing is
        erased or programmed. `failed_read`: the sector's read fails and the defaults are read."""
        v = set_vectors()
        cmds, rows = [], []
        for slot in (0, 1):
            for r in v['reads']:
                st = int(r['stamp'], 16)
                if r['pages'] is None:
                    cmds += ['load ' + save('set_r_%d.bin' % slot, self.flash(slot, st)),
                             'readfail 0x%x 0x1000' % SET_SECTOR[slot], 'storeread %d 1 1' % slot, 'readfail -']
                else:
                    pat = 9 if r['name'] == 'image_bytes' else None
                    zero = r['name'] == 'zeros'
                    fl = bytearray(self.flash(slot, st, sector=r['pages'], pattern=pat))
                    if zero:
                        fl[SET_SECTOR[slot]:SET_SECTOR[slot] + 0x1000] = bytes(0x1000)
                    cmds += ['load ' + save('set_r_%d_%s.bin' % (slot, r['name']), bytes(fl)), 'fault none',
                             'storeread %d 1 1' % slot, 'readfail -']
                rows.append((slot, r))
        out = run('s02', cmds)
        for k, (slot, r) in enumerate(rows):
            res, ops = split(out[4 * k + 2])
            what = (slot, r['name'])
            self.assertEqual(res, ['1' if r['page'] is not None else '0', hex(r['flags'])], what)
            self.assertEqual(writes(ops), [], what)
            self.assertEqual(ops[0], 'r:%x:c' % SET_RECORD[slot], what)
            self.assertEqual(ops[1:], ['r:%x:1000' % SET_SECTOR[slot]], what)

    @scenario('S-02/SC-SETTINGS')
    @requirement('S-02.097', 'S-02.098', 'S-02.099', 'S-02.105')
    def test_SC_SETTINGS_guards(self):
        """Every row of `guards`, for both slots, a valid record with the stamp in the sector: a read gives the
        record only for the usable rows and reads no byte of the sector otherwise; a save and a clear write
        only for them, and are refused with nothing written otherwise, the flash unchanged."""
        v = set_vectors()
        cmds, rows = [], []
        for slot in (0, 1):
            for g in v['guards']:
                rec = b(g['record'])
                st = struct.unpack('<I', rec[:4])[0]
                page = {'0': next(x for x in v['reads'] if x['name'] == 'one_record')['pages']['0']}
                # the vectors' records are stamped 0x1d0c2b3a; the guard rows keep that stamp unless trial
                now = rec if g['record_now'] in (None, 'unreadable') else b(g['record_now'])
                fl = self.flash(slot, st, sector=page, rec=now)
                est, good = int(g['established']), int(g['good'])
                fail = 'readfail 0x%x 12' % SET_RECORD[slot] if g['record_now'] == 'unreadable' else 'readfail -'
                path = save('set_g_%d_%s.bin' % (slot, g['name']), fl)
                cmds += ['load ' + path, fail, 'storeread %d %d %d %s' % (slot, est, good, rec.hex()),
                         'storewrite %d %d %d 0x7 %s' % (slot, est, good, rec.hex()), 'peek 0x%x 0x2000' % SET_SECTOR[slot],
                         'load ' + path, fail, 'storeclear %d %d %d %s' % (slot, est, good, rec.hex()),
                         'peek 0x%x 0x2000' % SET_SECTOR[slot]]
                rows.append((slot, g, fl))
        out = run('s02', cmds)
        names = {g['name'] for g in v['guards']}
        self.assertTrue({'length_0x76000', 'length_0x76001', 'not_established', 'not_good', 'trial_mark',
                         'record_changed', 'record_unreadable', 'bad_tag'} <= names)
        for k, (slot, g, fl) in enumerate(rows):
            o = out[9 * k:9 * k + 9]
            what = (slot, g['name'])
            before = fl[SET_SECTOR[slot]:SET_SECTOR[slot] + 0x2000]
            rres, rops = split(o[2])
            wres, wops = split(o[3])
            cres, cops = split(o[7])
            if g['usable']:
                self.assertEqual(rres[0], '1', what)
                self.assertEqual(wres, [str(STORE_OK)], what)
                self.assertTrue(all(self.in_sector(x, slot) for x in writes(wops)), what)
                self.assertEqual(cres, [str(STORE_OK)], what)
                self.assertEqual(bytes.fromhex(o[8])[:0x1000], b'\xff' * 0x1000, what)
                self.assertEqual(bytes.fromhex(o[8])[0x1000:], before[0x1000:], what)    # the start log's sector
            else:
                self.assertEqual(rres, ['0', hex(v['defaults'])], what)
                self.assertFalse([x for x in rops if self.in_sector(x, slot)], what)
                self.assertEqual((wres, writes(wops)), ([str(STORE_UNUSABLE)], []), what)
                self.assertEqual((cres, writes(cops)), ([str(STORE_UNUSABLE)], []), what)
                self.assertEqual(bytes.fromhex(o[4]), before, what)
                self.assertEqual(bytes.fromhex(o[8]), before, what)

    @scenario('S-02/SC-SETTINGS')
    @requirement('S-02.098', 'S-02.099')
    def test_SC_SETTINGS_saves(self):
        """Every row of `saves`: from the row's sector a save of its flags makes the row's operations in
        order (the record read, the sector's read, the erase and its read-back where the sector is full or not
        valid, one program at the first free page and its read-back, or nothing for the value it already
        gives), leaves the row's sector, writes nothing outside it, and the next read gives the flags."""
        v = set_vectors()
        cmds = []
        for r in v['saves']:
            slot, st = r['slot'], int(r['stamp'], 16)
            fl = self.flash(slot, st, sector=r['before'], pattern=r['before_pattern'])
            cmds += ['load ' + save('set_s_%d_%s.bin' % (slot, r['name']), fl),
                     'storewrite %d 1 1 %d' % (slot, r['flags']), 'peek 0 0x100000', 'storeread %d 1 1' % slot]
            r['_before'] = fl
        out = run('s02', cmds)
        names = {r['name'] for r in v['saves']}
        self.assertTrue({'from_erased', 'append', 'same_value', 'full_wraps', 'not_valid_gap', 'image_bytes',
                         'append_after_torn'} <= names)
        for k, r in enumerate(v['saves']):
            slot, what = r['slot'], (r['slot'], r['name'])
            res, ops = split(out[4 * k + 1])
            self.assertEqual(res, [str(STORE_OK)], what)
            self.assertEqual(ops, self.want_ops(r['ops']), what)
            after = bytes.fromhex(out[4 * k + 2])
            want = bytearray(b'\xff' * 0x1000)
            for pg, hexb in r['after'].items():
                d = b(hexb)
                want[int(pg) * 0x100:int(pg) * 0x100 + len(d)] = d
            self.assertEqual(after[SET_SECTOR[slot]:SET_SECTOR[slot] + 0x1000], bytes(want), what)
            lo, hi = SET_SECTOR[slot], SET_SECTOR[slot] + 0x1000
            self.assertEqual(after[:lo] + after[hi:], r['_before'][:lo] + r['_before'][hi:], what)
            self.assertEqual(split(out[4 * k + 3])[0], ['1', hex(r['flags'])], what)

    @scenario('S-02/SC-SETTINGS')
    @requirement('S-02.105')
    def test_SC_SETTINGS_clears(self):
        """Every row of `clears`: an erased sector is read and left alone; a used one is erased and read back
        erased; nothing outside the sector changes."""
        v = set_vectors()
        cmds = []
        for r in v['clears']:
            fl = self.flash(r['slot'], 0x1D0C2B3A, sector=r['before'])
            cmds += ['load ' + save('set_c_%d_%s.bin' % (r['slot'], r['name']), fl),
                     'storeclear %d 1 1' % r['slot'], 'peek 0 0x100000']
            r['_before'] = fl
        out = run('s02', cmds)
        for k, r in enumerate(v['clears']):
            slot, what = r['slot'], (r['slot'], r['name'])
            res, ops = split(out[3 * k + 1])
            self.assertEqual(res, [str(STORE_OK)], what)
            self.assertEqual(ops, self.want_ops(r['ops']), what)
            after = bytes.fromhex(out[3 * k + 2])
            lo, hi = SET_SECTOR[slot], SET_SECTOR[slot] + 0x1000
            self.assertEqual(after[lo:hi], b'\xff' * 0x1000, what)
            self.assertEqual(after[:lo] + after[hi:], r['_before'][:lo] + r['_before'][hi:], what)

    @scenario('S-02/SC-SETTINGS')
    @requirement('S-02.098', 'S-02.105')
    def test_SC_SETTINGS_cuts(self):
        """`cuts`, for both slots: a save that appends (page 1 after the defaults on page 0) cut before, inside
        (mask 00 and three seeds) and after its program; a save into a full sector cut before, inside (masks 00,
        FF and three seeds over the sector) and after its erase; a clear torn and cut after its erase. The next
        read gives the row's value: the old flags, the new, an earlier saved value or the defaults."""
        v = set_vectors()
        full = next(r for r in v['saves'] if r['name'] == 'full_wraps' and r['slot'] == 0)['before']
        app = next(r for r in v['saves'] if r['name'] == 'append' and r['slot'] == 0)['before']
        earlier = {int(b(x)[12]) & 0xF for x in full.values()} | {v['defaults']}
        cases = []
        for slot in (0, 1):
            for c in v['cuts']:
                if c['op'] == 'program':
                    start, cmd, old = app, 'storewrite %d 1 1 0x7' % slot, {v['defaults']}
                    n = 17
                    faults = {'before': ['fault after 0'], 'after': ['fault after 1'],
                              'torn': ['fault tear 1 ' + m.hex() for m in
                                       [bytes(n)] + [random.Random(sd).randbytes(n) for sd in (1, 2, 3)]]}[c['at']]
                    allowed = {'old': old, 'new': {0x7}}[c['reads']] if c['at'] != 'torn' else old | {0x7}
                elif c['op'] == 'erase':
                    start, cmd = full, 'storewrite %d 1 1 0x7' % slot
                    masks = [bytes(0x1000), b'\xff' * 0x1000] + [random.Random(sd).randbytes(0x1000) for sd in (1, 2, 3)]
                    faults = {'before': ['fault after 0'], 'after': ['fault after 1'],
                              'torn': ['fault tear 1 ' + m.hex() for m in masks]}[c['at']]
                    allowed = {'before': {0xF}, 'after': {v['defaults']}, 'torn': earlier}[c['at']]
                else:
                    start, cmd = full, 'storeclear %d 1 1' % slot
                    masks = [bytes(0x1000)] + [random.Random(sd).randbytes(0x1000) for sd in (1, 2)]
                    faults = {'after': ['fault after 1'],
                              'torn': ['fault tear 1 ' + m.hex() for m in masks]}[c['at']]
                    allowed = {'after': {v['defaults']}, 'torn': earlier}[c['at']]
                for f in faults:
                    cases.append((slot, c, f, start, cmd, allowed))
        cmds = []
        for k, (slot, c, f, start, cmd, _) in enumerate(cases):
            cmds += ['load ' + save('set_cut_%d.bin' % k, self.flash(slot, 0x1D0C2B3A, sector=start)), f, cmd,
                     'storeread %d 1 1' % slot]
        out = run('s02', cmds)
        for k, (slot, c, f, _, _, allowed) in enumerate(cases):
            res, _ = split(out[4 * k + 3])
            self.assertIn(int(res[1], 16), allowed, (slot, c, f[:14]))

    @scenario('S-02/SC-SETTINGS')
    @requirement('S-02.098')
    def test_SC_SETTINGS_failures(self):
        """A program that fails, one that changes nothing and reports success, and an erase that fails: the
        save reports the failure, makes no second erase or program, and the next read gives the old value."""
        v = set_vectors()
        app = next(r for r in v['saves'] if r['name'] == 'append' and r['slot'] == 1)['before']
        full = next(r for r in v['saves'] if r['name'] == 'full_wraps' and r['slot'] == 1)['before']
        cases = [(app, 'fault fail 1', STORE_IO, v['defaults']), (app, 'fault stuck 1', STORE_VERIFY, v['defaults']),
                 (app, 'fault late 1', STORE_IO, 0x7), (full, 'fault fail 1', STORE_IO, 0xF)]
        cmds = []
        for k, (sec, f, _, _) in enumerate(cases):
            cmds += ['load ' + save('set_e_%d.bin' % k, self.flash(1, 0x1D0C2B3A, sector=sec)), f,
                     'storewrite 1 1 1 0x7', 'storeread 1 1 1']
        out = run('s02', cmds)
        for k, (_, f, rc, reads) in enumerate(cases):
            res, ops = split(out[4 * k + 2])
            self.assertEqual(res, [str(rc)], f)
            self.assertEqual(len(writes(ops)), 1, (f, ops))
            self.assertEqual(split(out[4 * k + 3])[0][1], hex(reads), f)

    @scenario('S-02/SC-SETTINGS')
    @requirement('S-02.099', 'S-02.100', 'S-02.103')
    def test_SC_SETTINGS_footprint(self):
        """With selector 00 01 and 01 01, saves and a clear of the running slot's sector (the slot each selector
        names, its record's CRC the stamp) lie in that sector, change no other byte, the start log's sector
        included, and the first stage's decision and checks read the same before and after (S-02.100). The
        front's start-log step and recovery's survey read no byte of either settings sector."""
        empty = save('set_host.bin', b'')
        for sel in ((0, 1), (1, 1)):
            slot = sel[0]
            start = spiflash.build_flash(selector=sel, slots=('good', 'good'),
                                         images=(spiflash.slot_image(0, size=0x2345),
                                                 spiflash.slot_image(1, size=0x2345)))
            after = os.path.join(WORK, 'set_f1.bin')
            path = save('set_f.bin', start)
            out = run('s02', ['load ' + path, 'fs 1', 'storewrite %d 1 1 0xf' % slot,
                              'storewrite %d 1 1 0x6' % slot, 'storeread %d 1 1' % slot,
                              'storeclear %d 1 1' % slot, 'storewrite %d 1 1 0x8' % slot, 'save ' + after, 'fs 1',
                              'load ' + path, 'startlog', 'load ' + path, 'recover %d %s' % (slot, empty)])
            for line in (out[2], out[3], out[5], out[6]):
                res, ops = split(line)
                self.assertEqual(res, [str(STORE_OK)], sel)
                self.assertTrue(writes(ops) and all(self.in_sector(o, slot) for o in writes(ops)), (sel, ops))
            self.assertEqual(split(out[4])[0], ['1', '0x6'], sel)
            fs0, fs1 = split(out[1]), split(out[8])
            self.assertEqual(fs0, fs1, sel)
            with open(after, 'rb') as f:
                a = f.read()
            lo = SET_SECTOR[slot]
            self.assertEqual(a[:lo] + a[lo + 0x1000:], start[:lo] + start[lo + 0x1000:], sel)
            for line in (out[1], out[10], out[12]):
                _, ops = split(line)
                self.assertFalse([o for o in ops if self.in_sector(o, 0) or self.in_sector(o, 1)], (sel, ops))

    @scenario('S-02/SC-SETTINGS')
    @requirement('S-02.103')
    def test_SC_SETTINGS_upgrade(self):
        """`upgrade`: the package's bootloader step (s02_slot_upgrade) erases the target's settings sector,
        right after its start-log sector, exactly for an image of at most 0x76000 bytes; a longer one leaves
        it to the image's own bytes. The settings recorded there before are gone: a read with the new record
        gives the defaults."""
        v = set_vectors()
        for row in v['upgrade']:
            n = int(row['length'], 16)
            img = padded('good', n)
            fl = bytearray(spiflash.build_flash(selector=(0, 1), slots=('good', 'good'),
                                                images=(spiflash.slot_image(0, size=0x2345), None)))
            t = 1
            fl[SET_SECTOR[t]:SET_SECTOR[t] + 0x100] = b(next(r for r in v['records'] if r['name'] == 'all_on')['bytes']).ljust(0x100, b'\xff')
            out = run('s02', ['load ' + save('set_u.bin', bytes(fl)), 'upgrade 1 ' + save('set_u.ldr', img)])
            res, ops = split(out[1])
            self.assertEqual(res, ['0', str(t)], row)
            last = max(i for i, o in enumerate(ops) if o.startswith('p:') and int(o.split(':')[1], 16) >= 0x88000)
            tail = [o for o in ops[last + 1:ops.index('e:f000')] if o.startswith('e:')]   # after the image
            log, sec = 'e:%x' % (0x88000 + 0x77000), 'e:%x' % SET_SECTOR[t]
            logo = 'e:%x' % (0x88000 + 0x75000)     # the boot logo's header, where the image leaves it free
            want = ([log] if row['erases_start_log'] else []) + ([logo] if n <= 0x75000 else []) + \
                ([sec] if row['erases_settings'] else [])
            self.assertEqual(tail, want, row)

    @scenario('S-02/SC-SETTINGS')
    @requirement('S-02.084', 'S-02.103')
    def test_SC_SETTINGS_recovery(self):
        """`recovery`: each of recovery's image writes, plain, select-first, trial and in place, from either
        running slot, makes the vector's erases and programs, the target's settings sector erased right after
        its log sector and before the trial mark or the record erase; the target's sector reads erased after,
        the other slot's is untouched. An in-place write follows a failed write of the other slot. Recovery's
        actions erase neither sector."""
        v = set_vectors()
        rv = v['recovery']
        allon = b(next(r for r in v['records'] if r['name'] == 'all_on')['bytes']).ljust(0x100, b'\xff')
        n = cut_sequences()['image_length']
        img, other = front_image(n), front_image(n, seed=21, front_seed=6)
        seen = set()
        for w in rv['writes']:
            a, t, way = w['running'], w['target'], w['way']
            self.assertEqual(int(w['length'], 16), n, w['way'])
            self.assertEqual(int(w['settings_sector'], 16), SET_SECTOR[t], way)
            seen.add((way, a))
            slots = ['good', 'good']
            if way == 'in_place':
                slots[1 - a] = 'badcrc'
            images = tuple(IMAGE_A if k == a else None for k in (0, 1))
            fl = bytearray(spiflash.build_flash(selector=(a, 1), slots=tuple(slots), images=images))
            for k in (0, 1):
                fl[SET_SECTOR[k]:SET_SECTOR[k] + 0x100] = allon
            host, faults, prior = b'', [], []
            if way == 'in_place':                       # S-02.084: the failed write of B first
                host = transfer(img, seq=0x30)
                faults, prior = ['fault late 1'], ['e:%x' % spiflash.SLOT_OFF[1 - a]]
            flags = {'plain': None, 'select_first': 1, 'in_place': 1, 'trial': 0x02}[way]
            host += transfer(other if way == 'trial' else img, flags=flags)
            after = os.path.join(WORK, 'set_rec_after.bin')
            out = run('s02', ['load ' + save('set_rec.bin', bytes(fl))] + faults +
                      ['recover %d %s' % (a, save('set_rec_host.bin', host)), 'save ' + after])
            res, ops = split(out[1 + len(faults)])
            self.assertEqual(int(res[1]), PH_DONE, (way, a))
            want = []
            for o in w['ops']:
                if o['op'] == 'erase':
                    want.append('e:%x' % int(o['offset'], 16))
                elif o['op'] == 'program':
                    want.append('p:%x:%x' % (int(o['offset'], 16), o['length']))
            self.assertEqual(writes(ops), prior + want, (way, a))
            wr_ = writes(ops)[len(prior):]
            k = wr_.index('e:%x' % SET_SECTOR[t])
            # after the log's erase and the boot logo's header's (the image leaves both free)
            self.assertEqual(wr_[k - 2:k], ['e:%x' % (spiflash.SLOT_OFF[t] + spiflash.SLOT_MAX - 0x1000),
                                           'e:%x' % (spiflash.SLOT_OFF[t] + spiflash.SLOT_MAX - 0x3000)], (way, a))
            nxt = 'p:%x:1' % (spiflash.SLOT_OFF[t] + spiflash.SLOT_MAX - 0x1000) if way == 'trial' \
                else 'e:%x' % spiflash.SLOT_RECORD_OFF[t]
            self.assertEqual(wr_[k + 1], nxt, (way, a))
            self.assertEqual(w['settings_erase']['before'], 'the trial mark' if way == 'trial' else 'the record erase')
            with open(after, 'rb') as f:
                got = f.read()
            self.assertEqual(got[SET_SECTOR[t]:SET_SECTOR[t] + 0x1000], b'\xff' * 0x1000, (way, a))
            self.assertEqual(got[SET_SECTOR[1 - t]:SET_SECTOR[1 - t] + 0x100], allon, (way, a))
        self.assertEqual(seen, {(x, a) for x in ('plain', 'select_first', 'trial', 'in_place') for a in (0, 1)})
        sets = {'e:%x' % x for x in SET_SECTOR}
        with open(vectors('recovery_actions.json'), encoding='utf-8') as f:
            acts = json.load(f)['cases']
        for c in acts:
            self.assertFalse(sets & set(ref_writes(c['ops'])), c['act'])
        self.assertFalse([x for x in rv['actions'] if x['erases_settings']])


def logo_vectors():
    with open(vectors('boot_logo.json'), encoding='utf-8') as f:
        return json.load(f)


LOGO_SLOT = (0x10000, 0x88000)
LOGO_AT = tuple(s + 0x75000 for s in LOGO_SLOT)     # each slot's header sector
# enum wb_logo_rc
LOGO_OK = 0


def logo_pattern(n, p):
    return bytes(((i * 7 + p) ^ (i >> 8)) & 0xFF for i in range(n))


class BootLogo(unittest.TestCase):
    """The stored boot logo: src/logo.c (wb_logo_parse(), wb_logo_room(), wb_logo_read(), wb_logo_save(),
    wb_logo_remove()) and the front's check and read (src/front/fr_logo.c) through h_s02's flash, against
    vectors/boot_logo.json, for both slots; the slot upgrade's erase of the header through s02_slot_upgrade().
    System's Boot logo screen, the PNG's conversion and the factory reset's screen are not driven here."""

    @staticmethod
    def flash(slot, rec, place=None, fill=True):
        """A flash with slot `slot`'s record `rec` (12 bytes), the slot's bytes from its image's end to its
        settings sector holding a pattern (fill), then `place` ({offset: hex}) written over them, and
        something in both slots' settings and start-log sectors to see them untouched."""
        fl = bytearray(b'\xff' * 0x100000)
        fl[SET_RECORD[slot]:SET_RECORD[slot] + 12] = rec
        for k in (0, 1):
            fl[SET_SECTOR[k]:SET_SECTOR[k] + 0x2000] = random.Random(k).randbytes(0x2000)
        ln = struct.unpack('<I', rec[4:8])[0]
        if fill and 1 <= ln <= 0x75000:
            lo = LOGO_SLOT[slot] + (ln + 0xFFF) // 0x1000 * 0x1000
            fl[lo:LOGO_AT[slot] + 0x1000] = logo_pattern(LOGO_AT[slot] + 0x1000 - lo, 11)
        for off, hexb in (place or {}).items():
            d = bytes.fromhex(hexb)
            fl[int(off, 16):int(off, 16) + len(d)] = d
        return bytes(fl)

    @staticmethod
    def want(ops):
        return ['e:%x' % int(o[1], 16) if o[0] == 'erase' else 'p:%x:%x' % (int(o[1], 16), o[2]) for o in ops]

    @requirement('S-02.109', 'S-02.110', 'S-02.111')
    def test_headers(self):
        """Every row of `headers`: the main image's check and the front's take the header for a slot with
        the row's record exactly when the row says, with the row's data length."""
        v = logo_vectors()
        out = run('s02', ['logohead %s %s' % (b(h['record']).hex(), b(h['header']).hex()) for h in v['headers']])
        names = {h['name'] for h in v['headers']}
        self.assertTrue({'good', 'hcrc', 'other_image', 'record_room_exact', 'record_room_over',
                         'record_0x75001'} <= names)
        for line, h in zip(out, v['headers']):
            res, _ = split(line)
            n = h['length'] or 0
            want = ['1', hex(n)] if h['usable'] else ['0', '0x0']
            self.assertEqual(res, want + want, h['name'])

    @requirement('S-02.110')
    def test_room(self):
        """Every row of `room`: the bytes a slot with that record leaves a logo."""
        v = logo_vectors()
        recs = [struct.pack('<III', 0x1D0C2B3A, int(r['length'], 16), int(r['tag'], 16)) for r in v['room']]
        out = run('s02', ['logoroom ' + r.hex() for r in recs])
        for line, r in zip(out, v['room']):
            self.assertEqual(split(line)[0], [hex(int(r['room'], 16))], r)

    @requirement('S-02.111')
    def test_reads(self):
        """Every row of `reads`: the main image and the front read a logo, its data checked whole, exactly
        when the row says; nothing is erased or programmed."""
        v = logo_vectors()
        cmds = []
        for r in v['reads']:
            fl = self.flash(r['slot'], b(r['record']), r['bytes'])
            cmds += ['load ' + save('logo_r_%d_%s.bin' % (r['slot'], r['name']), fl), 'logoread %d' % r['slot']]
        out = run('s02', cmds)
        for k, r in enumerate(v['reads']):
            res, ops = split(out[2 * k + 1])
            what = (r['slot'], r['name'])
            self.assertEqual(writes(ops), [], what)
            if r['gives']:
                data = next(bytes.fromhex(x) for off, x in r['bytes'].items() if int(off, 16) != LOGO_AT[r['slot']])
                self.assertEqual(res, ['1', hex(r['length']), hex(zlib.crc32(data)), '1'], what)
            else:
                self.assertEqual(res, ['0', '0x0', '0x0', '0'], what)

    @requirement('S-02.112')
    def test_saves(self):
        """Every row of `saves`: the save makes the row's erases and programs in order, the header's sector
        first and the header last; the slot then holds the data where the row says and the header, every
        other byte unchanged, and the next read gives it."""
        v = logo_vectors()
        for r in v['saves']:
            slot, rec = r['slot'], b(r['record'])
            fl = self.flash(slot, rec)
            data = logo_pattern(r['length'], r['data_pattern'])
            cmds = ['load ' + save('logo_s_%d.bin' % slot, fl),
                     'logosave %d 1 1 %d %d %d %d %s %s %s' % (slot, r['bits'], r['colors'], r['width'], r['height'],
                                                            r['bg'], save('logo_s.dat', data), rec.hex()),
                     'peek 0 0x100000', 'logoread %d' % slot]
            # one run a row: the data file is read when its command runs
            out = run('s02', cmds)
            res, ops = split(out[1])
            what = (slot, r['name'])
            self.assertEqual(res, [str(LOGO_OK)], what)
            self.assertEqual(writes(ops), self.want(r['ops']), what)
            want = bytearray(fl)
            for o in r['ops']:
                if o[0] == 'erase':
                    want[int(o[1], 16):int(o[1], 16) + 0x1000] = b'\xff' * 0x1000
            at = int(r['data_at'], 16)
            want[at:at + len(data)] = data
            head = b(r['header'])
            want[LOGO_AT[slot]:LOGO_AT[slot] + len(head)] = head
            self.assertEqual(bytes.fromhex(out[2]), bytes(want), what)
            self.assertEqual(split(out[3])[0][:3], ['1', hex(len(data)), hex(zlib.crc32(data))], what)

    @requirement('S-02.112')
    def test_removes(self):
        """Every row of `removes`: a stored or a not valid header's sector is erased, alone; an erased one is
        left; nothing else changes, and no logo is read after."""
        v = logo_vectors()
        cmds, rows = [], []
        for r in v['removes']:
            fl = self.flash(r['slot'], b(r['record']), r['bytes'], fill=False)
            cmds += ['load ' + save('logo_rm_%d_%s.bin' % (r['slot'], r['name']), fl),
                     'logoremove %d 1 1' % r['slot'], 'peek 0 0x100000', 'logoread %d' % r['slot']]
            rows.append(fl)
        out = run('s02', cmds)
        for k, (r, fl) in enumerate(zip(v['removes'], rows)):
            what = (r['slot'], r['name'])
            res, ops = split(out[4 * k + 1])
            self.assertEqual(res, [str(LOGO_OK)], what)
            self.assertEqual(writes(ops), self.want(r['ops']), what)
            want = bytearray(fl)
            if r['ops']:
                want[LOGO_AT[r['slot']]:LOGO_AT[r['slot']] + 0x1000] = b'\xff' * 0x1000
            self.assertEqual(bytes.fromhex(out[4 * k + 2]), bytes(want), what)
            self.assertEqual(split(out[4 * k + 3])[0][0], '0', what)

    @requirement('S-02.112')
    def test_guards(self):
        """Every row of `guards`, for both slots: a save writes only when the running slot may be written and
        the data fits its room, and is refused with nothing written otherwise; a removal under the same rule."""
        v = logo_vectors()
        names = {g['name'] for g in v['guards']}
        self.assertTrue({'usable', 'length_0x74000', 'length_0x74001', 'length_0x75001', 'not_established',
                         'not_good', 'record_changed', 'record_unreadable', 'over_room', 'over_max'} <= names)
        for slot in (0, 1):
            for g in v['guards']:
                rec = b(g['record'])
                now = rec if g['record_now'] in (None, 'unreadable') else b(g['record_now'])
                fl = self.flash(slot, now)
                fail = 'readfail 0x%x 12' % SET_RECORD[slot] if g['record_now'] == 'unreadable' else 'readfail -'
                path = save('logo_g.bin', fl)
                data = logo_pattern(g['length'], 7)
                est, good = int(g['established']), int(g['good'])
                out = run('s02', ['load ' + path, fail,
                                  'logosave %d %d %d %d %d 100 50 0x18e3 %s %s' % (
                                      slot, est, good, g['bits'], g['colors'], save('logo_g.dat', data), rec.hex()),
                                  'peek 0 0x100000', 'load ' + path, fail,
                                  'logoremove %d %d %d %s' % (slot, est, good, rec.hex()), 'peek 0 0x100000'])
                what = (slot, g['name'])
                sres, sops = split(out[2])
                rres, rops = split(out[6])
                if g['saved']:
                    self.assertEqual(sres, [str(LOGO_OK)], what)
                    self.assertTrue(writes(sops), what)
                else:
                    self.assertNotEqual(sres, [str(LOGO_OK)], what)
                    self.assertEqual(writes(sops), [], what)
                    self.assertEqual(bytes.fromhex(out[3]), fl, what)
                if g['removal_allowed']:
                    self.assertEqual(rres, [str(LOGO_OK)], what)
                else:
                    self.assertNotEqual(rres, [str(LOGO_OK)], what)
                    self.assertEqual(writes(rops), [], what)
                    self.assertEqual(bytes.fromhex(out[7]), fl, what)

    @requirement('S-02.112')
    def test_cuts(self):
        """`cuts`: a save over a stored logo cut after each of its writes, or torn inside one (masks 00, FF
        and two seeds), and a removal cut and torn: the next read gives the row's logo, the old one, none or
        the new; the front never reads one the main image does not."""
        v = logo_vectors()
        c = v['save_cut_case']
        slot, rec = c['slot'], b(c['record'])
        start = self.flash(slot, rec, c['old'])
        data = logo_pattern(c['length'], c['data_pattern'])
        dpath = save('logo_c.dat', data)
        path = save('logo_c.bin', start)
        gives = {'old': c['old_length'], 'new': c['length'], 'none': None}
        cmds, rows = [], []
        for r in v['cuts']:
            masks = [None] if r['at'] == 'after' else \
                [bytes(0x1000), b'\xff' * 0x1000] + [random.Random(sd).randbytes(0x1000) for sd in (1, 2)]
            for m in masks:
                f = 'fault after %d' % r['write'] if m is None else 'fault tear %d %s' % (r['write'], m.hex())
                op = 'logosave %d 1 1 %d %d 100 50 0x18e3 %s %s' % (slot, c['bits'], c['colors'], dpath, rec.hex()) \
                    if r['op'] == 'save' else 'logoremove %d 1 1 %s' % (slot, rec.hex())
                cmds += ['load ' + path, f, op, 'logoread %d' % slot]
                rows.append(r)
        out = run('s02', cmds)
        seen = set()
        for k, r in enumerate(rows):
            res, _ = split(out[4 * k + 3])
            got = int(res[1], 16) if res[0] == '1' else None
            self.assertIn(got, [gives[x] for x in r['gives']], r)
            self.assertFalse(res[3] == '1' and res[0] != '1', r)
            seen.add(r['op'])
        self.assertEqual(seen, {'save', 'remove'})

    @requirement('S-02.113')
    def test_upgrade(self):
        """`upgrade`: the package's bootloader step erases the target's logo header, after its start log and
        before its settings sector, exactly for an image of at most 0x75000 bytes; a logo stored for the old
        image is not read after, whatever the length."""
        gen = spec_module('S-02-spi-flash', 'gen_vectors')
        v = logo_vectors()
        t = 1
        for row in v['upgrade']:
            n = int(row['length'], 16)
            fl = bytearray(spiflash.build_flash(selector=(0, 1), slots=('good', 'good'),
                                                images=(spiflash.slot_image(0, size=0x2345),
                                                        spiflash.slot_image(1, size=0x2345))))
            f, data = gen.logo_picture(20, 10, 2, 1)
            crc_old = struct.unpack('<I', bytes(fl[SET_RECORD[t]:SET_RECORD[t] + 4]))[0]
            head = gen.logo_header(20, 10, 1, 2, f['bg'], data, crc_old)
            for off, hexb in gen.logo_place(t, head, data).items():
                d = bytes.fromhex(hexb)
                fl[int(off, 16):int(off, 16) + len(d)] = d
            out = run('s02', ['load ' + save('logo_u.bin', bytes(fl)), 'logoread %d' % t,
                              'upgrade 1 ' + save('logo_u.ldr', padded('good', n)), 'logoread %d' % t])
            self.assertEqual(split(out[1])[0][0], '1', row)
            res, ops = split(out[2])
            self.assertEqual(res, ['0', str(t)], row)
            last = max(i for i, o in enumerate(ops) if o.startswith('p:') and int(o.split(':')[1], 16) >= LOGO_SLOT[t])
            er = [o for o in ops[last + 1:ops.index('e:f000')] if o.startswith('e:')]   # after the image
            logo, log, sec = 'e:%x' % LOGO_AT[t], 'e:%x' % (LOGO_SLOT[t] + 0x77000), 'e:%x' % SET_SECTOR[t]
            self.assertEqual(logo in er, row['erases_logo_header'], row)
            if row['erases_logo_header']:
                self.assertLess(er.index(log), er.index(logo), row)
                if sec in er:
                    self.assertLess(er.index(logo), er.index(sec), row)
            self.assertEqual(split(out[3])[0][0], '0', row)


if __name__ == '__main__':
    unittest.main()
