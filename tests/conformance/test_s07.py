# SPDX-License-Identifier: GPL-2.0-or-later
"""S-07 rules and the PSoC bootloader-host protocol: src/core/psoc/rules.c and psoc/cypress.c (through h_s07),
and the backplane step (src/core/psoc/psoc.c, psoc/program.c through h_s07p), against tests/oracle/S-07-psoc."""

import json
import os
import subprocess
import unittest

from harness import BUILD, hx, requirement, run, scenario, spec_module, spec_path, tsv, unhex

gen = spec_module('S-07-psoc', 'gen_s07')
m = gen.m                         # s07_model, as gen_s07 loads it
cypress = gen.cypress


def vectors(name):
    with open(spec_path('S-07-psoc', 'vectors', name), encoding='utf-8') as f:
        return json.load(f)


NONE = 0x100                     # S07_POWER_NONE


def state(s):
    return NONE if s == 'none' else int(s, 16)


class Keys(unittest.TestCase):

    @requirement('S-07.031', 'S-07.079')
    def test_S07_031_bitmaps(self):
        """Each bitmap's keys down by s07_key_down(). The lengths of S-07.079 are the caller's (src/core/psoc/psoc.c,
        not driven): a bitmap shorter than 9 bytes is ignored (no events), and only bytes 0-8 are read."""
        cases = vectors('keys.json')['bitmaps']
        read = [b[:18] for c in cases for b in c['bitmaps'] if len(b) >= 18]
        out = iter(run('s07', ['keys ' + b for b in read]))
        for n, c in enumerate(cases):
            down = set()
            for b, want in zip(c['bitmaps'], c['events']):
                if len(b) < 18:
                    self.assertEqual(want, [])
                    continue
                line = next(out)
                now = set() if line == '-' else {int(x) for x in line.split()}
                events = sorted([[k, 0] for k in down - now] + [[k, 1] for k in now - down])
                with self.subTest(case=n, bitmap=b):
                    self.assertEqual(events, sorted(want))
                down = now

    @requirement('S-07.032', 'S-07.033')
    def test_S07_032_key_status(self):
        cases = vectors('keys.json')['status']
        out = run('s07', ['status ' + c['payload'] for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(payload=c['payload']):
                self.assertEqual(o.split(), [str(int(c['payload'] != 'none')), str(c['hold']), str(c['debug']),
                                             str(c['precomp'])])


class Power(unittest.TestCase):

    @requirement('S-07.042')
    def test_S07_042_charge_led(self):
        rows = tsv('S-07-psoc', 'charge_led.tsv')
        self.assertTrue(rows)
        out = run('s07', ['led %d %s' % (state(r['state']), r['charge']) for r in rows])
        for r, o in zip(rows, out):
            rgb, forced = o.split()
            with self.subTest(state=r['state'], charge=r['charge']):
                self.assertEqual(('03' + rgb, forced), (r['rgb'], r['forced']))

    @requirement('S-07.050', 'S-07.051')
    def test_S07_051_transitions(self):
        cases = vectors('power.json')['transitions']
        out = run('s07', ['power %d %d' % (state(c['prev']), state(c['state'])) for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(prev=c['prev'], state=c['state']):
                self.assertEqual(o, str(c['power_off']))

    @requirement('S-07.053', 'S-07.079')
    def test_S07_053_battery(self):
        cases = vectors('power.json')['battery']
        keys = ['present', 'charging', 'charge', 'temp', 'ma', 'mv', 'usable']
        out = run('s07', ['battery ' + (c['payload'] or '-') for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(payload=c['payload']):
                self.assertEqual(o.split(), [str(c[k]) for k in keys])

    @requirement('S-07.055', 'S-07.079')
    def test_S07_055_vbus(self):
        """A shorter reply's missing bytes read as 00 (S-07.079): the caller's (src/core/psoc/psoc.c), done here."""
        cases = vectors('power.json')['vbus']
        reply = [(bytes.fromhex(c['reply']) + bytes(2))[:2] for c in cases]
        out = run('s07', ['vbus %d %d %d %d' % (c['applied'], c['target'], r[0], r[1]) for c, r in zip(cases, reply)])
        for c, o in zip(cases, out):
            applied, t = o.split()
            with self.subTest(case=c):
                self.assertEqual(int(applied), c['applied_after'])
                self.assertEqual(int(t), -1 if c['send_34'] is None else int(c['send_34'], 16))


class Pmic(unittest.TestCase):

    @requirement('S-07.061')
    def test_S07_061_write_value(self):
        seqs = [s for s in vectors('pmic.json')['sequence'] if s['read'] != 'none']   # none: no write (s07_psoc)
        self.assertTrue(seqs)
        out = run('s07', ['pmic ' + s['read'] for s in seqs])
        for s, o in zip(seqs, out):
            (write,) = [p for mid, kind, p in s['frames'] if mid == '0x71']
            with self.subTest(read=s['read']):
                self.assertEqual(int(o, 16), int(write[-2:], 16))


SUM, CRC = 0, 1
WHY = {'timeout': 1, 'malformed': 2, 'device': 4, 'version': 5, 'row': 6, 'row check': 7,
       'application invalid': 8}


def model_run(text, unlock=gen.SYNTH_UNLOCK, make=None, swap=None, **kw):
    """The model's host against cypress.Bootloader(**kw), or make(): (ok, at, why, packets sent, the
    replies as the host took them, None for none). swap (code, reply): that command is carried out and
    its reply replaced."""
    bl = make() if make else cypress.Bootloader(**dict({'row_size': None}, **kw))
    if swap:
        inner = bl._packet
        bl._packet = lambda p: (inner(p), swap[1])[1] if p[1] == swap[0] else inner(p)
    sent, replies = [], []

    def xfer(p, n):
        sent.append(p)
        r = bl.feed(p)
        r = r[:n] if len(r) >= n else None
        replies.append(r)
        return r

    def write(p):
        sent.append(p)
        bl.feed(p)

    try:
        m.Host(xfer, write, unlock).run(text)
        return True, 0, 0, sent, replies
    except m.RunFailed as e:
        return False, e.at, 3 if e.why.startswith('status') else WHY[e.why], sent, replies


def c_runs(cases):
    """Each (text, unlock, replies) through h_s07's run: (ok, at, why, packets sent)."""
    out = run('s07', ['run %s %s %s' % (hx(t.encode()), hx(u),
                                        ','.join('none' if r is None else r.hex() for r in rs) or '-')
                      for t, u, rs in cases])
    res = []
    for o in out:
        f = o.split()
        res.append((f[0] == '1', int(f[1], 16), int(f[2]), [unhex(x) for x in f[3:]]))
    return res


class Cypress(unittest.TestCase):

    @requirement('S-07.081')
    def test_S07_081_enter_packet(self):
        e = vectors('cypress.json')['enter']
        (o,) = run('s07', ['cypacket 0x38 %d %s' % (SUM, e['unlock'])])
        self.assertEqual(o, e['packet'])

    @requirement('S-07.080', 'S-07.081')
    def test_S07_081_probe_verdicts(self):
        """S-07.081: a reply with a wrong checksum may be accepted or rejected; bytes received during the
        wait of S-07.080 step 2 may be judged with the reply or discarded (the caller's choice: the
        reply alone must pass, and with them the verdict must be the vector's unless it is free)."""
        cases = vectors('cypress.json')['probe']
        cmds, want = [], []
        for p in cases:
            cmds.append('probe ' + (p['reply'] or '-'))
            want.append(1 if 'wait' in p else p['ok'])
            if 'wait' in p and p['ok'] != 'either':
                cmds.append('probe ' + p['wait'] + p['reply'])
                want.append(p['ok'])
        for c, w, o in zip(cmds, want, run('s07', cmds)):
            with self.subTest(cmd=c):
                self.assertIn(o, ('0', '1'))
                if w != 'either':
                    self.assertEqual(int(o), w)

    @requirement('S-07.082')
    def test_S07_082_packets(self):
        cases = vectors('cypress.json')['packets']
        cmds = ['cypacket %s %d %s' % (p['code'], kind, p['data'] or '-') for p in cases for kind in (SUM, CRC)]
        out = iter(run('s07', cmds))
        for p in cases:
            with self.subTest(code=p['code'], data=p['data']):
                self.assertEqual(next(out), p['sum'])
                self.assertEqual(next(out), p['crc'])
                self.assertLessEqual(len(bytes.fromhex(p['sum'])), m.MAX_PACKET)

    @requirement('S-07.082')
    def test_S07_082_checksums(self):
        """Including CRC-16/X-25's published check value for "123456789", 0x906E, sent swapped."""
        cases = vectors('cypress.json')['checksum'] + [
            {'bytes': b'123456789'.hex(), 'sum': '%04x' % (-sum(b'123456789') & 0xFFFF), 'crc': '6e90'}]
        out = run('s07', ['cycheck %d %s' % (kind, c['bytes'] or '-') for c in cases for kind in (SUM, CRC)])
        for c, s_, x in zip(cases, out[0::2], out[1::2]):
            with self.subTest(bytes=c['bytes']):
                self.assertEqual(int(s_, 16), int(c['sum'], 16))
                self.assertEqual(int(x, 16), int(c['crc'], 16))

    @requirement('S-07.082')
    def test_S07_082_exit_packet(self):
        self.assertEqual(run('s07', ['exitpacket']), [vectors('cypress.json')['exit_packet']])
        self.assertEqual(bytes.fromhex(vectors('cypress.json')['exit_packet']), m.EXIT_PACKET)

    @requirement('S-07.082', 'S-07.083')
    def test_S07_083_runs(self):
        """Every run sends the model's packets, in order, and stops at the model's exchange for the
        model's reason: the cypress.json run, a CRC-variant file, two arrays, long rows, and the
        model's failure runs with a malformed reply added."""
        runs = vectors('cypress.json')['runs']
        v = runs[0]
        two = cypress.build_cyacd(0x2E123069, 0, [(0, 3, bytes(range(64))), (1, 7, bytes(range(200))),
                                                  (0, 4, bytes(64)), (1, 8, bytes(range(1, 201)))])
        bad = bytes([0x55, 0, 0, 0, 0, 0, 0x17])
        # a program row carries at most 52 bytes, a send data 55: the lengths where the split changes
        split_lens = [0, 1, 52, 53, 54, 55, 56, 107, 108, 109, 110, 111]
        cases = [
            ('runs[0]', v['cyacd'], bytes.fromhex(v['unlock']), {}),
            ('runs[1]: 108-byte rows', runs[1]['cyacd'], gen.SYNTH_UNLOCK, {}),
            ('runs[2]: 109-byte rows', runs[2]['cyacd'], gen.SYNTH_UNLOCK, {}),
            ('runs[3]: arrays 00 and 40', runs[3]['cyacd'], gen.SYNTH_UNLOCK, {'make': gen.eeprom_bootloader}),
            ('array 40 outside the range: fails at program row',
             cypress.build_cyacd(0x2E123069, 0, [(0x40, 5, bytes(8))]), gen.SYNTH_UNLOCK,
             {'arrays': {0: (0, 0xFF), 0x40: (0, 0)}}),
            ('a bootloader checking by the CRC variant: not entered', gen.CYACD, gen.SYNTH_UNLOCK, {'kind': 'crc'}),
            ('two arrays', two, gen.SYNTH_UNLOCK, {'arrays': {0: (0, 255), 1: (0, 255)}}),
            ('one row of 512', gen.cyacd_text(rows=1, row_len=512), gen.SYNTH_UNLOCK, {}),
            ('rows around the split', cypress.build_cyacd(0x2E123069, 0, [(0, n, bytes(range(n)))
                                                                          for n in split_lens]),
             gen.SYNTH_UNLOCK, {}),
            ('silicon id', gen.CYACD, gen.SYNTH_UNLOCK, {'silicon_id': 0x11111111}),
            ('silicon revision', gen.CYACD, gen.SYNTH_UNLOCK, {'silicon_rev': 0x01}),
            ('version byte 02', gen.CYACD, gen.SYNTH_UNLOCK, {'version': (1, 30, 2)}),
            ('row out of range', gen.CYACD, gen.SYNTH_UNLOCK, {'arrays': {0: (0, 1)}}),
            ('array not there', gen.CYACD, gen.SYNTH_UNLOCK, {'arrays': {1: (0, 255)}}),
            ('row check', gen.cyacd_text(bad_line=1), gen.SYNTH_UNLOCK, {}),
            ('status 05 on program row', gen.CYACD, gen.SYNTH_UNLOCK,
             {'hook': lambda c, d: cypress.ERR_CMD if c == 0x39 else None}),
            ('silent PSoC', gen.CYACD, gen.SYNTH_UNLOCK,
             {'hook': lambda c, d: cypress.NO_REPLY if c == 0x38 else None}),
            ('silent on verify row', gen.CYACD, gen.SYNTH_UNLOCK,
             {'hook': lambda c, d: cypress.NO_REPLY if c == 0x3A else None}),
            ('malformed reply to send data', gen.CYACD, gen.SYNTH_UNLOCK,
             {'hook': lambda c, d: bad if c == 0x37 else None}),
            ('application invalid', gen.CYACD, gen.SYNTH_UNLOCK, {'app_valid': False}),
        ]
        models = [model_run(t, u, **kw) for _, t, u, kw in cases]
        out = c_runs([(t, u, md[4]) for (_, t, u, _), md in zip(cases, models)])
        for (name, _, _, _), md, got in zip(cases, models, out):
            with self.subTest(case=name):
                self.assertEqual(got, md[:4])
                self.assertTrue(all(len(p) <= m.MAX_PACKET for p in got[3]))
                self.assertEqual(got[3][-1][1] == 0x3B, md[0] or md[1] == 0x31)   # exit only after 0x31
        for i in range(len(runs)):
            self.assertEqual([p.hex() for p in out[i][3]], runs[i]['packets'])
            self.assertTrue(out[i][0])

    @requirement('S-07.083')
    def test_S07_083_replies(self):
        """What a reply must be: first byte 01, status 00, the length field, 17 (cypress.json replies).
        The checksum may be checked or not: such a reply either goes on or fails at its exchange."""
        cases = vectors('cypress.json')['replies']
        models = [model_run(gen.CYACD, swap=(int(c['cmd'], 16), bytes.fromhex(c['reply']))) for c in cases]
        out = c_runs([(gen.CYACD, gen.SYNTH_UNLOCK, md[4]) for md in models])
        for c, md, got in zip(cases, models, out):
            with self.subTest(why=c['why']):
                if c['ok'] == 'either':
                    if not got[0]:
                        self.assertEqual(got[1], int(c['cmd'], 16))
                    continue
                self.assertEqual(got[0], bool(c['ok']))
                self.assertEqual(got, md[:4])

    @requirement('S-07.083')
    def test_S07_083_images(self):
        """Each published file bad or not as cypress.json says (a master may refuse more than 8 arrays or
        not), agreeing with s07_model.image_problem."""
        cases = vectors('cypress.json')['images']
        out = run('s07', ['image ' + hx(c['cyacd'].encode()) for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(why=c['why']):
                if c['bad'] == 'either':
                    self.assertIn(o, ('0', '1'))
                    continue
                self.assertEqual(int(o), c['bad'])
                self.assertEqual(int(o), int(m.image_problem(c['cyacd']) is not None))


def cosim(t, unlock=gen.SYNTH_UNLOCK):
    """Transcript t's setup run by h_s07p against s07_model.Psoc: (events in the transcript's
    form, the final time in ms, the last S-07 call's return, the backplane result or -1)."""
    psoc = m.Psoc(t.get('psoc'), unlock or bytes(6))
    setup = t.get('setup', {})
    sent = []                                                # [(t_ms, item)] the PSoC sends

    def lines(items):
        out = []
        for ta, item in items:
            sent.append((ta, item))
            us = round(ta * 1000)
            if item[0] == 'raw':
                out.append('P %d r %s' % (us, hx(item[1])))
            else:
                out.append('P %d f %#x %#x %s' % (us, item[1], item[2], hx(item[3])))
        return out

    head = ['setup %d %d %d %d %d' % (setup.get('boot_allowed', True), setup.get('os_loads', True),
                                      setup.get('tail_ms', 50), setup.get('end_ms', -1),
                                      setup.get('boot_prompt', False)),
            'cyacd ' + hx(setup.get('cyacd', '').encode())] + ([] if unlock is None else ['unlock ' + hx(unlock)])
    for a in setup.get('actions', []):
        if a['do'] not in ('backplane', 'key_test_start', 'continue_os'):
            raise AssertionError('h_s07p runs only the backplane, key_test_start and continue_os actions: %r' % a)
        head.append('action %d %s' % (a['t_ms'], a['do']))
    head += lines(psoc.initial()) + ['go']
    path = os.path.join(BUILD, 'h_s07p')
    if not os.path.exists(path):
        raise AssertionError(path + ' is not built (make -f tests/conformance/Makefile)')
    events, end = [], None
    with subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                          bufsize=1) as p:
        p.stdin.write('\n'.join(head) + '\n')
        p.stdin.flush()
        for line in p.stdout:
            f = line.split()
            if f[0] == 'M':
                tm = int(f[1]) / 1000
                if f[2] == 'r':
                    data = unhex(f[3])
                    events.append([tm, 'M', 'raw', data])
                    answer = psoc.on_raw(tm, data)
                else:
                    route, mid, payload = int(f[3], 16), int(f[4], 16), unhex(f[5])
                    events.append([tm, 'M', 'frame', route, mid, payload])
                    answer = psoc.on_frame(tm, route, mid, payload)
                p.stdin.write('\n'.join(lines(answer) + ['.']) + '\n')
                p.stdin.flush()
            elif f[0] == 'end' and len(f) == 4:
                end = (int(f[1]) / 1000, int(f[2]), int(f[3]))
            else:
                raise AssertionError('h_s07p: ' + line.strip())
        p.stdin.close()
        if p.wait() != 0 or end is None:
            raise AssertionError('h_s07p exited %d' % p.returncode)
    for ta, item in sent:                                    # what had arrived when the run ended
        if ta <= end[0]:
            events.append([ta, 'P', 'raw', item[1]] if item[0] == 'raw' else [ta, 'P', 'frame'] + list(item[1:]))
    events.sort(key=lambda e: e[0])
    return (events,) + end


def transcript(name):
    with open(spec_path('S-07-psoc', 'transcripts', name + '.json'), encoding='utf-8') as f:
        return json.load(f)


BACKPLANE = ['backplane_ok', 'backplane_enter_retry', 'backplane_enter_fail', 'backplane_enter_silent',
             'backplane_bad_file',
             'backplane_run_retry',
             'backplane_run_retry_valid', 'backplane_run_fail', 'backplane_no_greet']


class Backplane(unittest.TestCase):
    """The backplane step against s07_model.Psoc: its frames and raw writes are the transcript's, in
    order, and every timing check of the transcript holds (s07_model.timed_check)."""

    @scenario('S-07/PS-BACKPLANE')
    @requirement('S-07.080', 'S-07.081', 'S-07.083', 'S-07.084', 'S-07.085', 'S-07.086')
    def test_PS_BACKPLANE_transcripts(self):
        for name in BACKPLANE:
            t = transcript(name)
            with self.subTest(transcript=name):
                events, _, rc, ok = cosim(t)
                self.assertEqual(rc, 0)
                m.timed_check(events, t)
                want = m.run_transcript(t, unlock=gen.SYNTH_UNLOCK)      # the model's own run of it
                self.assertEqual(ok, int(name in ('backplane_ok', 'backplane_enter_retry', 'backplane_run_retry',
                                                  'backplane_run_retry_valid', 'backplane_no_greet')))
                self.assertEqual([k for _, k in m.master_items(events)], [k for _, k in m.master_items(want)])

    @requirement('S-07.081')
    def test_S07_081_no_unlock(self):
        """No PSoC unlock: the step fails before entry, no 0xEE and no raw byte; the rest as the model runs it."""
        t = transcript('backplane_ok')
        events, _, rc, ok = cosim(t, unlock=None)
        self.assertEqual((rc, ok), (0, 0))
        k = [x for _, x in m.master_items(events)]
        self.assertNotIn(('plain', 0xEE, b''), k)
        self.assertFalse([x for x in k if x[0] == 'raw'])
        self.assertEqual(k, [x for _, x in m.master_items(m.Sim(t['setup'], t['psoc'], unlock=None).run())])


class Continue(unittest.TestCase):
    """Continue to the OS on the PSoC link (S-05.033): src/core/psoc/psoc.c in h_s07p, held mode by the hold key and a key
    test, then the restart's PSoC part as the board makes it with the slave present (the key test's end, the view's
    hold, s07_shutdown_reports()), against s07_model.Psoc."""

    @requirement('S-07.045', 'S-07.067', 'S-07.071', 'S-07.072', 'S-05.033')
    def test_continue_restart(self):
        """Transcript continue_restart: its master items in order and every timing check (s07_model.timed_check);
        the key test's end, then the shutdown reports last; no PMIC sequence and no 0x51 {3C FF 32}."""
        t = transcript('continue_restart')
        events, _, rc, _ = cosim(t)
        self.assertEqual(rc, 0)
        m.timed_check(events, t)
        k = [x for _, x in m.master_items(events)]
        self.assertEqual(k, [x for _, x in m.master_items(m.run_transcript(t, unlock=gen.SYNTH_UNLOCK))])
        self.assertEqual([x[1:] for x in k[-2:]], [(0x51, b'\x00\x00\x02'), (0x40, b'\x01')])
        self.assertNotIn((0x70, b'\x01'), [x[1:] for x in k])
        self.assertNotIn((0x51, b'\x3c\xff\x32'), [x[1:] for x in k])


if __name__ == '__main__':
    unittest.main()
