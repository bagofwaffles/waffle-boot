# SPDX-License-Identifier: GPL-2.0-or-later
"""S-04 persistent records: src/s04_records.c (through h_s04) against images.records and
tests/oracle/S-04-records (s04_model.py, tables) (part 'logic')."""

import datetime
import json
import random
import unittest

from harness import hx, run, scenario, requirement, spec_module, spec_path, tsv, unhex
from conformance.images import records

m = spec_module('S-04-records', 's04_model')

SEED = 0x504


def load(name):
    with open(spec_path('S-04-records', 'vectors', name), encoding='utf-8') as f:
        return json.load(f)


def ver(v):
    return '.'.join(str(x) for x in v)


def writer_rows():
    return {r['id']: r for r in tsv('S-04-records', 'writers.tsv')}


def expected_ops(wid, reason):
    """The operations writer `wid` makes, in order, from writers.tsv and S-04.022, S-04.023, S-04.027 and
    S-04.028: the PSoC reset request first, the shutdown reports (the slave's power-off of S-04.032 is
    inside them), the Nordic step (W6: then UART1 is no longer used), the record write, the reset. Delays
    are checked separately."""
    w = writer_rows()[wid]
    ops = []
    if w['psoc_reset'] == 'yes':
        ops.append('psoc_reset')
    if w['shutdown'] == 'yes':
        ops.append('shutdown')
    if w['nordic'] != 'none':
        ops.append(w['nordic'])
    if wid == 'W6':
        ops.append('link_stop')
    ops.append('write:%d' % reason)
    ops.append(w['reset'])
    return ops


WRITER_NUM = {'W%d' % i: i for i in range(1, 12)}


class Reasons(unittest.TestCase):

    @scenario('S-04/R-ROWS')
    def test_R_ROWS(self):
        """Every row of reason_table.tsv: the reason the boot takes, the record after the read and the
        behavior (boots, screen, cdc, transport), against the table and against images.records."""
        rows = tsv('S-04-records', 'reason_table.tsv')
        cmds = []
        for r in rows:
            cmds.append('consume ' + r['record'])
            cmds.append('effect %s' % r['reason_taken'])
        out = run('s04', cmds)
        for i, r in enumerate(rows):
            reason, after = out[2 * i].split()
            mem = bytearray(bytes.fromhex(r['record']))
            self.assertEqual(int(reason), records.consume_reboot_record(mem), r['row'])
            self.assertEqual(int(reason), int(r['reason_taken']), r['row'])
            self.assertEqual(after, r['record_after'], r['row'])
            self.assertEqual(after, mem.hex(), r['row'])
            boots, screen, cdc, transport = out[2 * i + 1].split()
            want = records.reason_behavior(int(r['reason_taken']))
            self.assertEqual((boots == '1', screen, cdc == '1', transport),
                             (want['boots'], want['screen'], want['cdc'], want['transport']), r['row'])
            self.assertEqual((boots == '1', screen, cdc == '1', transport),
                             (r['boots'] == 'yes', r['screen'], r['cdc'] == 'yes', r['transport']), r['row'])

    @requirement('S-04.006', 'S-04.011')
    def test_S04_006_every_reason(self):
        """All 65536 reason values against images.records.reason_behavior."""
        out = run('s04', ['effect %d' % r for r in range(0x10000)])
        for r, line in enumerate(out):
            boots, screen, cdc, transport = line.split()
            want = records.reason_behavior(r)
            self.assertEqual((boots == '1', screen, cdc == '1', transport),
                             (want['boots'], want['screen'], want['cdc'], want['transport']), r)

    @requirement('S-04.002', 'S-04.003', 'S-04.004', 'S-04.005')
    def test_S04_004_random_records(self):
        """Seeded random 8-byte records (half with the magic) through the read, and every write keeping
        +6..+7, against images.records."""
        rnd = random.Random(SEED)
        recs = []
        for i in range(4000):
            b = bytearray(rnd.randbytes(8))
            if i % 2:
                b[0:4] = records.REBOOT_MAGIC.to_bytes(4, 'little')
            elif i % 4 == 0:
                b[0:4] = (records.REBOOT_MAGIC ^ (1 << rnd.randrange(32))).to_bytes(4, 'little')
            recs.append(bytes(b))
        reasons = [rnd.randrange(0x10000) for _ in recs]
        cmds = ['consume ' + r.hex() for r in recs] + \
               ['write %s %d' % (r.hex(), x) for r, x in zip(recs, reasons)]
        out = run('s04', cmds)
        for r, line in zip(recs, out[:len(recs)]):
            mem = bytearray(r)
            want = records.consume_reboot_record(mem)
            self.assertEqual(line, '%d %s' % (want, mem.hex()), r.hex())
        for r, x, line in zip(recs, reasons, out[len(recs):]):
            self.assertEqual(line, records.build_reboot_record(x)[:6].hex() + r[6:].hex(), r.hex())


class Writers(unittest.TestCase):

    def check_writer(self, wid, arg, reason):
        w = writer_rows()[wid]
        n = WRITER_NUM[wid]
        rule, ops = run('s04', ['writer %d %d' % (n, arg), 'reboot %d %d' % (n, arg)])
        got = rule.split()
        self.assertEqual(int(got[0]), reason, wid)
        self.assertEqual(got[1], w['reset'], wid)
        self.assertEqual(got[2], w['nordic'], wid)
        if w['shutdown'] != 'n/a':
            self.assertEqual(got[3] == '1', w['shutdown'] == 'yes', wid)
        self.assertEqual(got[4] == '1', w['psoc_reset'] == 'yes', wid)
        self.assertEqual(int(got[5]), int(w['min_delay_ms']), wid)
        ops = ops.split()
        delays = [int(o.split(':')[1]) for o in ops if o.startswith('delay:')]
        write_at = ops.index('write:%d' % reason)
        # S-04.023 W3: at least min_delay_ms before the write; S-04.028: nothing after the write but the
        # reset.
        self.assertGreaterEqual(sum(int(o.split(':')[1]) for o in ops[:write_at] if o.startswith('delay:')),
                                int(w['min_delay_ms']), wid)
        self.assertEqual(sum(delays), sum(int(o.split(':')[1]) for o in ops[:write_at]
                                          if o.startswith('delay:')), wid)
        self.assertEqual([o for o in ops if not o.startswith('delay:')], expected_ops(wid, reason), (wid, ops))

    @scenario('S-04/R-W1')
    def test_R_W1(self):
        """host_modes.tsv, and every first byte with 0-2 more, against images.records.host_mode_reason;
        then W1's rule and sequence for each reason."""
        rows = tsv('S-04-records', 'host_modes.tsv')
        rnd = random.Random(SEED + 1)
        payloads = [unhex(r['mode_payload']) for r in rows]
        payloads += [bytes([b]) + rnd.randbytes(rnd.randrange(3)) for b in range(256)]
        out = run('s04', ['hostmode ' + hx(p) for p in payloads])
        for r in rows:
            self.assertEqual(records.host_mode_reason(unhex(r['mode_payload'])), int(r['reason']))
        for p, line in zip(payloads, out):
            self.assertEqual(int(line), records.host_mode_reason(p), p.hex())
        for reason in sorted({int(x) for x in out}):
            self.check_writer('W1', reason, records.writer_reason('W1', mode_payload=bytes([
                {30: 1, 10: 2, 50: 4, 40: 0}[reason]])))

    @scenario('S-04/R-W2')
    def test_R_W2(self):
        self.check_writer('W2', 0, 20)

    @scenario('S-04/R-W3')
    def test_R_W3(self):
        self.check_writer('W3', 0, 20)

    @scenario('S-04/R-W6')
    def test_R_W6(self):
        self.check_writer('W6', 0, 20)

    @scenario('S-04/R-W7')
    def test_R_W7(self):
        """W7 re-stores the reason taken: 20 for an absent record, 30 for record 30 (S-04.026)."""
        for rec in ('ffffffffffffffff', '0df0adde1e000000'):
            taken = records.consume_reboot_record(bytearray(bytes.fromhex(rec)))
            self.check_writer('W7', taken, records.writer_reason('W7', taken=taken))

    @scenario('S-04/R-W10')
    def test_R_W10(self):
        """W10 writes 70 whatever this boot took (S-04.012)."""
        for taken in (20, 30):
            self.check_writer('W10', taken, 70)

    @scenario('S-04/R-W11')
    def test_R_W11(self):
        """W11 writes 80 whatever this boot took (S-04.013), after one exit-DFU exchange."""
        for taken in (20, 30):
            self.check_writer('W11', taken, 80)

    @scenario('S-04/R-W9')
    def test_R_W9(self):
        self.check_writer('W9', 0, 0)

    @scenario('S-04/R-NORDIC')
    def test_R_NORDIC(self):
        """S-04.027's step for every reason, against images.records.nordic_step and s04_model."""
        out = run('s04', ['nordic %d' % r for r in range(0x10000)])
        for r, line in enumerate(out):
            self.assertEqual(line, records.nordic_step(r), r)
            self.assertEqual(line, m.nordic_step(r), r)

    @scenario('S-04/R-W4')
    def test_R_W4(self):
        """The scan's name cases of R-W4, then seeded random root listings against S-04.024's rule as
        stated in the requirement (a candidate: a regular file whose name does not begin with '.';
        reset when no candidate contains 'tfw' and one contains both 'cdc' and 'please')."""
        cases = [
            ([('cdc_please', 1)], 1), ([('please-cdc.txt', 1)], 1),
            ([('CDC_PLEASE', 1)], 0), ([('.cdc_please', 1)], 0), ([('cdc_please', 0)], 0),
            ([('cdc', 0)], 0),  # cdc/please: the file is in a directory, not in the root
            ([('cdc_please', 1), ('x.tfw', 1)], 0), ([('cdc_please', 1), ('junk.tfw', 1)], 0),
            ([('cdc_please', 1), ('notes_tfw.txt', 1)], 0), ([('cdc_please', 1), ('.x.tfw', 1)], 1),
        ]

        def rule(entries):
            cand = [n for n, reg in entries if reg and not n.startswith('.') and n]
            return int(not any('tfw' in n for n in cand) and any('cdc' in n and 'please' in n for n in cand))

        rnd = random.Random(SEED + 2)
        parts = ['cdc', 'please', 'tfw', 'CDC', 'x', '.', '_', 'pl', 'ease', 'tf', 'w']
        for _ in range(3000):
            entries = [(''.join(rnd.choice(parts) for _ in range(rnd.randrange(1, 5))), rnd.randrange(2))
                       for _ in range(rnd.randrange(1, 5))]
            cases.append((entries, rule(entries)))
        out = run('s04', ['cdc ' + ' '.join('%s:%d' % (n.encode().hex(), reg) for n, reg in e)
                          for e, _ in cases])
        for (entries, want), line in zip(cases, out):
            self.assertEqual(int(line), want, entries)
            self.assertEqual(rule(entries), want, entries)


class Metadata(unittest.TestCase):

    def meta_line(self, raw):
        r = records.read_metadata(raw)
        return ' '.join([hx(r.serial), str(r.sku), str(r.factory), ver(r.os_version), ver(r.sw_version),
                         '0x%x' % r.state, '0x%x' % r.cv_gain, '0x%x' % r.cv_zero,
                         '0x%x' % r.extra[0], '0x%x' % r.extra[1]])

    @scenario('S-04/M-INFO')
    def test_M_INFO(self):
        """The reader's view (S-04.042) of metadata.json's read records and of seeded random records
        (valid magic, a magic one bit off, erased, states 0, 0xAB0D and others)."""
        v = load('metadata.json')
        raws = [bytes.fromhex(r['bytes']) for r in v['read']]
        rnd = random.Random(SEED + 3)
        for i in range(3000):
            b = bytearray(rnd.randbytes(0x34))
            k = i % 4
            if k < 3:
                b[0:4] = records.METADATA_MAGIC.to_bytes(4, 'little')
            if k == 1:
                b[0x24:0x28] = records.METADATA_STATE_UPGRADING.to_bytes(4, 'little')
            if k == 2:
                b[4 + rnd.randrange(9)] = 0
            raws.append(bytes(b))
        raws.append(b'\xff' * 0x34)
        out = run('s04', ['metaread ' + r.hex() for r in raws])
        for r, line in zip(raws, out):
            self.assertEqual(line, self.meta_line(r), r.hex())
        for r, line in zip(v['read'], out):
            serial, sku, factory, os_v, sw_v, state = line.split()[:6]
            got = {'serial': unhex(serial).decode('latin-1'), 'sku': int(sku), 'factory': int(factory),
                   'os_version': [int(x) for x in os_v.split('.')],
                   'sw_version': [int(x) for x in sw_v.split('.')], 'state': int(state, 0)}
            for k, want in r['fields'].items():
                self.assertEqual(got[k], int(want, 0) if k == 'state' else want, (r['name'], k))

    @scenario('S-04/M-WRITES', 'S-04/M-KEEP')
    def test_M_KEEP(self):
        """The write-patch rule (S-04.043): seeded random flash bytes before (valid, invalid magic,
        erased), current values and factory mode, against images.records.write_metadata; the bytes it
        keeps are those M-KEEP checks."""
        v = load('metadata.json')
        cases = []
        for w in v['write']:
            c = w['current']
            cases.append((bytes.fromhex(w['before']), c['serial'].encode(), c['sku'], c['factory'],
                          tuple(c['os_version']), tuple(c['sw_version']), int(c['state'], 0), w['factory_mode'],
                          bytes.fromhex(w['after'])))
        rnd = random.Random(SEED + 4)
        for i in range(3000):
            before = rnd.randbytes(0x34)
            if i % 3 == 0:
                before = records.METADATA_MAGIC.to_bytes(4, 'little') + before[4:]
            elif i % 3 == 1 and i % 2:
                before = b'\xff' * 0x34
            serial = bytes(rnd.choice(b'ABCXYZ0123456789') for _ in range(rnd.randrange(9)))
            cases.append((before, serial, rnd.randrange(0x10000), rnd.randrange(0x10000),
                          tuple(rnd.randrange(0x10000) for _ in range(4)),
                          tuple(rnd.randrange(0x10000) for _ in range(4)),
                          rnd.choice((0, records.METADATA_STATE_UPGRADING)), bool(rnd.randrange(2)), None))
        out = run('s04', ['metabuild %s %s %d %d %s %s %d %d' % (b.hex(), hx(s), sku, fac, ver(o), ver(sw), st,
                                                                  fm)
                          for b, s, sku, fac, o, sw, st, fm, _ in cases])
        for (b, s, sku, fac, o, sw, st, fm, after), line in zip(cases, out):
            want = records.write_metadata(b, o, sw, st, factory_mode=fm, serial=s, sku=sku, factory=fac)
            if after is not None:
                self.assertEqual(want, after)
            self.assertEqual(line, want.hex(), (b.hex(), fm))

    @scenario('S-04/M-WRITES')
    def test_M_WRITES_m6(self):
        """M6 and N3 (metadata_writes.tsv): a failed OS load writes exactly when sw_version is not
        0.0.0.0, for metadata.json's cases and seeded random versions, against s04_model.m6_writes."""
        v = load('metadata.json')
        vs = [tuple(c['sw_version']) for c in v['m6']['cases']]
        rnd = random.Random(SEED + 5)
        for _ in range(2000):
            vs.append(tuple(rnd.choice((0, 0, 0, rnd.randrange(1, 0x10000))) for _ in range(4)))
        out = run('s04', ['m6 ' + ver(x) for x in vs])
        for x, line in zip(vs, out):
            self.assertEqual(line == '1', m.m6_writes(x), x)
        for c in v['m6']['cases']:
            self.assertEqual(m.m6_writes(c['sw_version']), c['writes'])

    @scenario('S-04/M-SETMETA')
    def test_M_SETMETA(self):
        """Every set_metadata row of metadata.json from the record `sample` changed to serial XY000123,
        sku 7, factory 1 (as M-SETMETA), against the row and s04_model.set_metadata."""
        v = load('metadata.json')
        cur = {'serial': 'XY000123', 'sku': 7, 'factory': 1}
        cmds = []

        def arg(req, key):
            return req[key].encode().hex() if req.get(key) else ('-' if key in req else 'x')

        for row in v['set_metadata']:
            req = row['request']
            cmds.append('setmeta %d %d %s %d %d %s %s %s' % (
                row['factory_mode'], row['option'], cur['serial'].encode().hex(), cur['sku'], cur['factory'],
                arg(req, 'serial'), arg(req, 'sku_variant'), arg(req, 'factory')))
        out = run('s04', cmds)
        for row, line in zip(v['set_metadata'], out):
            writes, serial, sku, factory = line.split()
            got = {'serial': unhex(serial).decode('latin-1'), 'sku': int(sku), 'factory': int(factory)}
            mw, mnew = m.set_metadata(cur, row['request'], row['factory_mode'], row['option'])
            self.assertEqual(writes == '1', row['writes'], row['name'])
            self.assertEqual(writes == '1', mw, row['name'])
            if row['writes']:
                want = dict(cur)
                want.update(row['result'])
                self.assertEqual(got, want, row['name'])
                self.assertEqual(got, {k: mnew[k] for k in got}, row['name'])


class Rtc(unittest.TestCase):

    @scenario('S-04/T-SET')
    def test_T_SET(self):
        """rtc.json's encode rows and seeded random times from 1969 to 2061, against s04_model.rtc_encode
        (the register value of T-SET's MMR log; refused outside the 15-bit day field)."""
        v = load('rtc.json')
        dts = [datetime.datetime.strptime(r['datetime'], '%Y-%m-%d %H:%M:%S') for r in v['encode']]
        rnd = random.Random(SEED + 7)
        lo, hi = datetime.datetime(1969, 1, 1), datetime.datetime(2061, 12, 31)
        span = int((hi - lo).total_seconds())
        dts += [lo + datetime.timedelta(seconds=rnd.randrange(span)) for _ in range(4000)]
        out = run('s04', ['rtcenc %d %d %d %d %d %d' % (d.year, d.month, d.day, d.hour, d.minute, d.second)
                          for d in dts])
        for d, line in zip(dts, out):
            try:
                want = 'ok 0x%x' % m.rtc_encode(d)
            except ValueError:
                want = 'refused'
            self.assertEqual(line, want, d)
        for r in v['encode']:
            d = datetime.datetime.strptime(r['datetime'], '%Y-%m-%d %H:%M:%S')
            self.assertEqual(m.rtc_encode(d), int(r['clk'], 16))

    @scenario('S-04/T-GET')
    def test_T_GET(self):
        """rtc.json's decode rows and every day count with seeded random times, against
        s04_model.rtc_decode_days; the round-trip exceptions of 2000-12-30 and 2000-12-31."""
        v = load('rtc.json')
        rnd = random.Random(SEED + 8)
        cases = [(r['days'], 0, 0, 0) for r in v['decode']]
        cases += [(n, rnd.randrange(24), rnd.randrange(60), rnd.randrange(60)) for n in range(1 << 15)]
        out = run('s04', ['rtcdec 0x%x' % (s | mi << 6 | h << 12 | n << 17) for n, h, mi, s in cases])
        for (n, h, mi, s), line in zip(cases, out):
            d = m.rtc_decode_days(n)
            self.assertEqual(line, '%d %d %d %d %d %d' % (d.year, d.month, d.day, h, mi, s), n)
        for r in v['decode']:
            self.assertEqual(m.rtc_decode_days(r['days']).isoformat(), r['date'])
        for date, back in v['round_trip_exceptions']:
            dt = datetime.datetime.fromisoformat(date)
            line = run('s04', ['rtcdec 0x%x' % m.rtc_encode(dt)])[0]
            self.assertEqual(line, '%s 0 0 0' % ' '.join(str(int(x)) for x in back.split('-')))


if __name__ == '__main__':
    unittest.main()
