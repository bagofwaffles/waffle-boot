# SPDX-License-Identifier: GPL-2.0-or-later
"""S-10c codecs, texts and the command server's Continue transcripts: src/s10c_sysex.c, s10c_text.c and
s10c_host.c (through h_s10c) against tests/oracle/S-10c-host-commands/vectors."""

import json
import re
import unittest

from harness import hx, requirement, run, scenario, spec_path, unhex


def vectors(name):
    with open(spec_path('S-10c-host-commands', 'vectors', name), encoding='utf-8') as f:
        return json.load(f)


class Packing(unittest.TestCase):

    @requirement('S-10c.001')
    def test_S10c_001_pack(self):
        cases = vectors('pack7.json')['pack']
        out = run('s10c', ['pack ' + (c['data'] or '-') for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(name=c['name']):
                self.assertEqual(unhex(o).hex(), c['packed'])

    @requirement('S-10c.002')
    def test_S10c_002_unpack(self):
        cases = vectors('pack7.json')['unpack']
        out = run('s10c', ['unpack ' + (c['packed'] or '-') for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(name=c['name']):
                self.assertEqual(unhex(o).hex(), c['data'])


class UsbMidi(unittest.TestCase):

    @requirement('S-10c.003')
    def test_S10c_003_send(self):
        cases = vectors('usbmidi.json')['send']
        out = run('s10c', ['send ' + c['message'] for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(name=c['name']):
                self.assertEqual(unhex(o).hex(), c['packets'])

    @requirement('S-10c.004', 'S-10c.006')
    def test_S10c_004_receive(self):
        cases = vectors('usbmidi.json')['receive']
        out = run('s10c', ['recv ' + (c['packets'] or '-') for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(name=c['name']):
                self.assertEqual([] if o == '-' else o.split(), c['messages'])


class Messages(unittest.TestCase):

    @requirement('S-10c.007', 'S-10c.010', 'S-10c.081')
    def test_S10c_007_classify(self):
        cases = vectors('sysex.json')['classify']
        out = run('s10c', ['classify ' + c['message'] for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(name=c['name']):
                self.assertEqual(o, c['class'])

    @requirement('S-10c.020')
    def test_S10c_020_carrier(self):
        cases = vectors('carrier.json')['encode']
        out = run('s10c', ['carrier ' + (c['stream'] or '-') for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(name=c['name']):
                self.assertEqual(unhex(o).hex(), c['message'])


class Texts(unittest.TestCase):

    @requirement('S-10c.103')
    def test_S10c_103_percent(self):
        cases = vectors('progress.json')['percent']
        out = run('s10c', ['percent %d %d' % (c['done'], c['total']) for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(done=c['done'], total=c['total']):
                self.assertEqual(int(o), c['percent'])

    @requirement('S-10c.101')
    def test_S10c_101_progress_s06(self):
        cases = vectors('progress.json')['s06']
        out = run('s10c', ['progress %d %d %s' % (c['done'], c['total'], hx(c['label'].encode()))
                           for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(case=c):
                self.assertEqual(unhex(o).hex(), c['payload'])

    @requirement('S-10c.102', 'S-10c.103')
    def test_S10c_102_progress_te(self):
        """progress.json's dfu: the TE progress reply's payload (the percent, the label cut to 62 bytes),
        the outcome report's included."""
        cases = vectors('progress.json')['dfu']
        out = run('s10c', ['progress_te %d %d %d %s' % (c['done'], c['total'], c['outcome'],
                                                        hx(c['label'].encode())) for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(case=c['label'][:20], done=c['done'], total=c['total']):
                self.assertEqual(unhex(o).hex(), c['payload'])

    @requirement('S-10c.104')
    def test_S10c_104_step_labels(self):
        steps = [(int(m.group(1)), c['label']) for c in vectors('progress.json')['labels']
                 if (m := re.match(r'step (\d+)', c['phase']))]
        self.assertTrue(steps)
        out = run('s10c', ['label %d' % s for s, _ in steps] + ['label 7'])
        for (s, label), o in zip(steps, out):
            with self.subTest(step=s):
                self.assertEqual(unhex(o).decode(), label)
        self.assertEqual(out[-1], '-')

    @requirement('S-10c.106')
    def test_S10c_106_skip_labels(self):
        """progress.json's skipped: the label of the report after a step's Skipped, for each step and reason."""
        cases = vectors('progress.json')['skipped']
        out = run('s10c', ['skiplabel %d %s' % (c['step'], c['reason'].replace(' ', '_')) for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(step=c['step'], reason=c['reason']):
                self.assertEqual(unhex(o).decode(), c['label'])

    @requirement('S-10c.088')
    def test_S10c_088_te_code(self):
        cases = vectors('info.json')['te_code']
        out = run('s10c', ['tecode ' + c['word'] for c in cases])
        for c, o in zip(cases, out):
            with self.subTest(word=c['word']):
                self.assertEqual(unhex(o).decode(), c['code'])


LINK = {'cdc': 0, 'midi': 1, 'psoc': 2, 'nordic': 3}


def ignored(script, frame):
    """The script's ignore rules: log lines and progress reports."""
    route, mid, payload = frame
    for rule in script.get('ignore', []):
        ids = rule['id'] if isinstance(rule['id'], list) else [rule['id']]
        if mid in [int(x, 16) for x in ids] and payload.hex().startswith(rule.get('prefix', '')):
            return True
    return False


class HostCommands(unittest.TestCase):
    """src/s10c_host.c through h_s10c's dev and frame commands: a transcript's inject frames delivered to the
    server one by one on the vector's link, its expect frames matched in order against what the server sends.
    What the server does with frames after Continue to the OS has asked for the restart is not checked: the
    board restarts (src/board/opxy_services.c), which is not driven here."""

    def play(self, v):
        dev = v.get('device', {})
        link = LINK[v['link']]
        script = v['script']
        cmds = ['dev %d %d 0' % (dev.get('held', True), dev.get('slave', True))]
        injects = []
        for step in script['steps']:
            for f in step.get('inject', []):
                cmds.append('frame %d %s %s %s' % (link, f['route'], f['id'], f['payload'] or '-'))
                injects.append(f)
        out = run('s10c', cmds)
        self.assertEqual(out[0], 'ok')
        sent, cont, after = [], None, []
        for f, line in zip(injects, out[1:]):
            for item in ([] if line == '-' else line.split()):
                if item.startswith('restart:'):
                    self.assertIsNone(cont, v['name'])
                    _, lk, writer = item.split(':')
                    cont = (int(lk), writer)
                    continue
                lk, route, mid, payload = item.split(':')
                frame = (int(route, 16), int(mid, 16), unhex(payload))
                self.assertEqual(int(lk), link, (v['name'], item))
                if cont is not None:
                    after.append(frame)
                elif not ignored(script, frame):
                    sent.append(frame)
            if cont is not None:
                break
        expects = [s['expect'] for s in script['steps'] if 'expect' in s]
        self.assertEqual(len(sent), len(expects), (v['name'], sent))
        for e, (route, mid, payload) in zip(expects, sent):
            self.assertEqual((route, mid), (int(e['route'], 16), int(e['id'], 16)), v['name'])
            if 'payload' in e:
                self.assertEqual(payload.hex(), e['payload'], v['name'])
            else:
                self.assertTrue(payload.hex().startswith(e['prefix']), (v['name'], payload))
        self.assertEqual(after, [], v['name'])
        return sent, cont

    @scenario('S-10c/H-CONTINUE')
    @requirement('S-10c.076', 'S-10c.042', 'S-10c.045', 'S-10c.070')
    def test_H_CONTINUE(self):
        """fw-continue-cdc and -midi: the reply 54 on route 12 and only then the restart (W11) asked on that
        link (the reply leaves before the detach, S-04.063); bytes after the sub-command ignored.
        fw-continue-no-slave: the same, by W2 (the writer is the harness board's choice, as the board's).
        fw-continue-busy (a host transfer in progress): 84 and a text, no restart, and the transfer goes on.
        coproc-early-continue-psoc (before held mode): 84, no restart, and the link is still served."""
        cmd = {v['name']: v for v in vectors('commands.json')['vectors']}
        co = {v['name']: v for v in vectors('coprocessor.json')['vectors']}
        names = ['fw-continue-cdc', 'fw-continue-midi', 'fw-continue-busy', 'fw-continue-no-slave']
        self.assertTrue(set(names) <= set(cmd))
        for name in names:
            with self.subTest(vector=name):
                sent, cont = self.play(cmd[name])
                if name == 'fw-continue-busy':
                    self.assertIsNone(cont)
                else:
                    writer = 'W11' if cmd[name].get('device', {}).get('slave', True) else 'W2'
                    self.assertEqual(cont, (LINK[cmd[name]['link']], writer))
                    self.assertEqual(sent[-1], (0x12, 0x0E, b'\x54'))
        with self.subTest(vector='coproc-early-continue-psoc'):
            sent, cont = self.play(co['coproc-early-continue-psoc'])
            self.assertIsNone(cont)
            self.assertEqual(sent[0][:2], (0x14, 0x0E))

    @requirement('S-10c.070', 'S-10c.076')
    def test_fw_other(self):
        """fw-other: an empty payload and the sub-commands that are none of S-10c.070's (1A, not 19) are
        answered 84 with a text; none asks for the restart."""
        v = {v['name']: v for v in vectors('commands.json')['vectors']}['fw-other']
        _, cont = self.play(v)
        self.assertIsNone(cont)


if __name__ == '__main__':
    unittest.main()
