# SPDX-License-Identifier: GPL-2.0-or-later
"""S-13c display panel: src/s13c_panel.c (through h_s13c, over a model of the panel bus) and the front's
panel on its way into recovery (src/front/, through h_s13cf) against
tests/oracle/S-13c-display/check_trace.py (part 'logic')."""

import hashlib
import json
import os
import unittest

from harness import BUILD, requirement, run, scenario, spec_module, spec_path

C = spec_module('S-13c-display', 'check_trace')
T15 = spec_module('S-15-menus', 's15_model')

WORK = os.path.join(BUILD, 'work')
MS = 1000000


def vec_json(spec, name):
    with open(spec_path(spec, 'vectors', name), encoding='utf-8') as f:
        return json.load(f)


PATTERN_SHA = vec_json('S-13c-display', 'frame_pattern.json')['sha256']
ORIENT = vec_json('S-13c-display', 'orientation.json')
INFO_TOKENS = vec_json('S-15-menus', 'info_tokens.json')['tokens']


def trace(name, path, **keys):
    """Runs h_s13c; returns (the trace's lines, frames sent by DMA, TE misses)."""
    os.makedirs(WORK, exist_ok=True)
    out = os.path.join(WORK, 's13c_%s.tsv' % name)
    args = ' '.join('%s=%s' % kv for kv in sorted(keys.items()))
    res = run('s13c', ['run %s %s %s' % (path, out, args)])[0].split()
    with open(out, encoding='utf-8') as f:
        return f.readlines(), int(res[1]), int(res[2])


class Base(unittest.TestCase):
    def check(self, lines, expect, frame_sha=None):
        fails, c = C.check(lines, expect, frame_sha)
        self.assertEqual(fails, [], '\n'.join('%s %s %s' % f for f in fails))
        return c

    @staticmethod
    def pulses_after_bootloader(c):
        b = c.tr.marks['bootloader']
        return [(f, r) for f, r in c.tr.pulses if f >= b]

    @staticmethod
    def first_txn(c):
        return min(x.t for x in c.tr.txns)


class Reset(Base):
    @scenario('S-13c/D-HS')
    def test_handshake(self):
        lines, _, _ = trace('hs', 'boot', hs=1)
        c = self.check(lines, ['boot'], handover_sha())
        self.assertEqual(self.pulses_after_bootloader(c), [])
        # the first stage's release at 2 ms (+10.5 us), its ms time 2: 122 ms at the earliest
        self.assertGreaterEqual(self.first_txn(c), 122 * MS)
        self.assertEqual(c.tr.handover[2][4:8], (2).to_bytes(4, 'little'))

    @scenario('S-13c/D-NOHS')
    def test_no_handshake(self):
        lines, _, _ = trace('nohs', 'boot', hs=0)
        c = self.check(lines, ['boot'], handover_sha())
        p = self.pulses_after_bootloader(c)
        self.assertEqual(len(p), 1)
        self.assertGreaterEqual(p[0][1] - p[0][0], 10000)
        self.assertGreaterEqual(self.first_txn(c) - p[0][1], 120 * MS)

    @scenario('S-13c/D-FUTURE')
    def test_future(self):
        lines, _, _ = trace('future', 'boot', hs=1, ymod=0xFFFFFFF0)
        c = self.check(lines, ['boot'], handover_sha())
        self.assertEqual(len(self.pulses_after_bootloader(c)), 1)

    @requirement('S-13c.004')
    def test_wrong_magic(self):
        lines, _, _ = trace('magic', 'boot', hs=1, xmod=0x12345679)
        c = self.check(lines, ['boot'], handover_sha())
        self.assertEqual(len(self.pulses_after_bootloader(c)), 1)

    @scenario('S-13c/D-STRAP0')
    def test_strap_low(self):
        for hs in (1, 0):
            with self.subTest(hs=hs):
                lines, _, _ = trace('strap0_%d' % hs, 'boot', hs=hs, strap=0)
                c = self.check(lines, ['boot'], handover_sha())
                # no driven pulse: the strap interval's release is the reset
                self.assertEqual(len(self.pulses_after_bootloader(c)), 1)
                fall, rise = self.pulses_after_bootloader(c)[0]
                self.assertLess(fall - c.tr.marks['strap'], C.STRAP_SETTLE_NS)
                self.assertGreaterEqual(self.first_txn(c) - rise, 120 * MS)
                self.assertEqual(c.tr.handover[2][4:8], (rise // MS).to_bytes(4, 'little'))


class Boot(Base):
    @scenario('S-13c/D-BOOT')
    def test_boot(self):
        lines, frames, _ = trace('boot', 'boot')
        c = self.check(lines, ['boot'], handover_sha())
        self.assertEqual(frames, 0)          # the hand-over frame is written by the CPU
        ho = c.tr.handover[0]
        boot_msgs = [t for t, m in c.tr.msg51 if m == bytes([0x3C, 0xFF, 0x32])]
        self.assertEqual(len(boot_msgs), 1)
        self.assertGreater(boot_msgs[0], max(x.t_end or x.t for x in c.tr.txns))
        self.assertLess(boot_msgs[0], ho)

    @scenario('S-13c/D-REC')
    def test_record(self):
        for hs in (1, 0):
            with self.subTest(hs=hs):
                lines, _, _ = trace('rec_%d' % hs, 'boot', hs=hs)
                c = self.check(lines, ['boot'], handover_sha())
                rec = c.tr.handover[2]
                self.assertEqual(len(rec), 44)
                self.assertEqual((rec[0], rec[0x28]), (1, 1))
                self.assertEqual(int.from_bytes(rec[8:12], 'little'), 2)

    @scenario('S-13c/D-REGREET')
    def test_regreet(self):
        lines, _, _ = trace('regreet', 'boot', regreet=1)
        c = self.check(lines, ['boot'], handover_sha())
        self.assertEqual(len(c.tr.greetings), 1)

    @scenario('S-13c/D-PROMPT')
    def test_prompt(self):
        """S-05.025, S-13c.019: the boot prompt's frame after MARK prompt, lit by the early logo's 3C FF 02,
        no 3C FF 32 before its end; with digit 1 the hand-over frame follows it (rule msg51, vector
        good_boot_prompt), with digit 2 held mode's 3C FF 02 (good_prompt_then_held)."""
        lines, frames, _ = trace('prompt_boot', 'boot', prompt=1)
        c = self.check(lines, ['boot'], handover_sha())
        self.assertEqual(frames, 1)                          # the prompt's; the logo frames are the CPU's
        mark = c.tr.marks['prompt']
        shown = [x for x, _ in c.frames if x.t >= mark]
        self.assertEqual(len(shown), 2)                      # the prompt's frame, then the hand-over frame
        self.assertLess(shown[0].t_end, shown[1].t)
        self.assertLess(shown[1].t_end, c.tr.handover[0])
        self.assertEqual(sum(1 for x in c.tr.txns if x.cmd == C.SLPOUT), 1)
        lines, frames, _ = trace('prompt_held', 'held', prompt=1, frames=2)
        c = self.check(lines, ['held'])
        self.assertEqual(frames, 3)
        self.assertLess(c.tr.marks['prompt'], c.tr.marks['held'])


class Held(Base):
    @scenario('S-13c/D-HELD')
    def test_held(self):
        lines, frames, misses = trace('held', 'held', frames=6)
        c = self.check(lines, ['held'])
        self.assertEqual((frames, misses), (6, 0))
        # S-13c.012: the early logo loads the gamma tables on every path (S-13c.012 allows it in
        # held mode); still one initialization in the run
        self.assertEqual(sum(1 for x in c.tr.txns if x.cmd == 0xE2), 1)
        self.assertEqual(sum(1 for x in c.tr.txns if x.cmd == C.SLPOUT), 1)

    @requirement('S-13c.015', 'S-13c.016')
    def test_pattern(self):
        lines, _, _ = trace('pattern', 'held', pattern=1)
        self.check(lines, ['held'], PATTERN_SHA)

    @scenario('S-13c/D-FAIL')
    def test_fail(self):
        lines, frames, _ = trace('fail', 'fail')
        c = self.check(lines, ['fail'])
        self.assertEqual(frames, 4)
        self.assertEqual(sum(1 for x in c.tr.txns if x.cmd == C.SLPOUT), 1)
        self.assertEqual(sum(1 for x in c.tr.txns if x.cmd == 0xE2), 1)

    @requirement('S-13c.025')
    def test_logo_line(self):
        """S-13c.025: the line under the logo at held mode's entry, over the early logo frame (held) or the
        hand-over frame (fail): one band of 16 panel columns under the logo, its text in dim gray, then held
        mode's frames in S-13c.014's window."""
        for path in ('held', 'fail'):
            with self.subTest(path=path):
                lines, frames, _ = trace('line_' + path, path, line=1)
                c = self.check(lines, [path])
                self.assertEqual(frames, 4)
                self.assertEqual(len(c.lines), 1)
                x, window = c.lines[0]
                self.assertEqual(window, (49 + 140, 49 + 155, 0, 479))
                self.assertIn(b'\x8c\x71', bytes(x.params))
                self.assertTrue(all(f.t > x.t_end for f, _ in c.frames[-4:]))

    @scenario('S-13c/D-TE')
    def test_te_rates(self):
        # one run per rate: 60 Hz, 50 Hz and 65 Hz, a menu change each frame
        for us in (16667, 20000, 15385):
            with self.subTest(te_us=us):
                lines, frames, misses = trace('te_%d' % us, 'held', te_us=us, frames=8)
                self.check(lines, ['held'])
                self.assertEqual((frames, misses), (8, 0))

    @requirement('S-13c.017')
    def test_no_te(self):
        # TE never comes: frames still go out, each T_TE_MISS after the previous one's end, and the
        # rule checks it (S-13c.017)
        lines, frames, misses = trace('note', 'held', te_us=0, frames=2)
        self.check(lines, ['held'])
        self.assertEqual((frames, misses), (2, 2))

    @requirement('S-13c.017')
    def test_fatal(self):
        lines, frames, _ = trace('fatal', 'held', fatal=1)
        c = self.check(lines, ['held'])
        self.assertEqual(frames, 3)
        fatal = c.tr.marks['fatal']
        after = [x for x in c.tr.txns if x.cmd == C.RAMWR and x.t >= fatal]
        before = [x for x in c.tr.txns if x.cmd == C.RAMWR and x.t < fatal]
        self.assertEqual(len(after), 1)
        # at once: when the frame in flight has ended, no TE wait
        self.assertLess(after[0].t - max(fatal, before[-1].t_end), 1 * MS)

    @scenario('S-13c/D-CONTINUE')
    @requirement('S-05.033', 'S-13c.019', 'S-13c.022')
    def test_continue(self):
        """D-CONTINUE: Continue to the OS is a restart. Held mode, the restarting view sent whole, the 750 ms
        hold, then the restart's shutdown (MARK poweroff); check_trace --expect held,poweroff passes. The view
        is the last frame and the last thing sent, and ends at least 750 ms before the mark; no hand-over
        frame, no 0x51 3C FF 32 and no kernel call in the run."""
        for frames in (1, 4):
            with self.subTest(frames=frames):
                lines, sent, _ = trace('cont_%d' % frames, 'held', frames=frames, cont=1)
                c = self.check(lines, ['held', 'poweroff'])
                self.assertEqual(sent, frames + 1)                   # the held frames and the restarting view
                mark = c.tr.marks['poweroff']
                last = c.frames[-1][0]
                self.assertIs(c.tr.txns[-1], last)
                self.assertGreaterEqual(mark - last.t_end, 750 * MS)
                self.assertEqual([m for _, m in c.tr.msg51 if m == bytes([0x3C, 0xFF, 0x32])], [])
                self.assertIsNone(c.tr.handover)

    def off_state(self, c):
        """No transaction starts after MARK poweroff; the DISPON and DISPOFF commands of the run, in order."""
        mark = c.tr.marks['poweroff']
        self.assertTrue(all(x.t < mark for x in c.tr.txns))
        return [x.cmd for x in c.tr.txns if x.cmd in (C.DISPON, C.DISPOFF)]

    @scenario('S-13c/D-OFF')
    def test_poweroff(self):
        """S-13c.022 in its four variants: (a) between two held-mode frames, (b) while one goes out, (c) in
        the start after the early logo frame is lit, (d) in the start before the first frame. Nothing reaches
        the panel after MARK poweroff, no DISPOFF; a frame in progress ends whole (rule frames); 0x51 00 00 02
        follows the last frame's last byte (rule msg51). The panel stays in display-on, its last frame the
        last thing sent; in (d) the display was never on."""
        for variant, n, expect in (('a', 2, ['held', 'poweroff']), ('b', 1, ['held', 'poweroff']),
                                   ('c', 3, ['poweroff'])):
            with self.subTest(variant=variant):
                lines, _, _ = trace('off_%s' % variant, 'held', poweroff=n)
                c = self.check(lines, expect)
                mark = c.tr.marks['poweroff']
                onoff = self.off_state(c)
                self.assertEqual(onoff[-1], C.DISPON)             # display-on at the end
                last = c.frames[-1][0]
                self.assertIs(c.tr.txns[-1], last)                # the last frame is the last thing sent
                self.assertLess(last.t, mark)
                if variant == 'a':
                    self.assertLess(last.t_end, mark)
                if variant == 'b':
                    self.assertGreater(last.t_end, mark)          # in progress at the power-off, ended
        with self.subTest(variant='d'):
            lines, _, _ = trace('off_d', 'held', poweroff=4)
            fails, c = C.check(lines, ['poweroff'])
            # S-13c.019 as it stands: the 00 00 02 precedes DISPON, which never comes (scenario D-OFF (d))
            self.assertEqual([f[0] for f in fails], ['msg51'], fails)
            self.assertIn('00 00 02 before DISPON', fails[0][2])
            self.assertEqual(self.off_state(c), [])
            self.assertEqual(c.frames, [])


def front_trace(name, way, **keys):
    """Runs h_s13cf; returns (the trace's lines, DMA sends from MARK recovery, sends stopped, TE misses)."""
    os.makedirs(WORK, exist_ok=True)
    out = os.path.join(WORK, 's13cf_%s.tsv' % name)
    args = ' '.join('%s=%s' % kv for kv in sorted(keys.items()))
    res = run('s13cf', ['run %s %s %s' % (way, out, args)])[0].split()
    with open(out, encoding='utf-8') as f:
        return f.readlines(), int(res[1]), int(res[2]), int(res[3])


def t_of(line):
    return -1 if line.startswith('#') else int(line.split('\t', 1)[0])


def line_block(lines):
    """The indexes [i, j) of the line under the logo: its CASET to the frame window's CASET after it."""
    i = next(k for k, x in enumerate(lines) if '\tCMD\t2A' in x and '\tDATA\t00BD00CC' in lines[k + 1])
    j = next(k for k in range(i + 2, len(lines)) if '\tCMD\t2A' in lines[k])
    return i, j + 2


class Front(Base):
    """The front's panel (src/front/fr_panel.c) from its entry into recovery: fr_start_run() and
    fr_recovery_run() whole, over h_s13cf's bus, PSoC, flash and image models; check_trace --expect
    recovery."""

    LINE_WINDOW = (189, 204, 0, 479)

    def front(self, name, way='chord', expect=('recovery',), **keys):
        lines, sends, stops, misses = front_trace(name, way, **keys)
        return self.check(lines, list(expect)), lines, sends, stops, misses

    @requirement('S-13c.025', 'S-15.151', 'S-02.084')
    def test_line_each_way(self):
        """On each way in (Power + M4, a failed image check, three unfinished starts): the early logo frame
        with the display off, DISPON, one line under the logo in its band, then recovery's frames from MARK
        recovery, each by one DMA send."""
        for way in ('chord', 'check', 'count'):
            with self.subTest(way=way):
                c, _, sends, stops, misses = self.front('way_' + way, way)
                self.assertIn('recovery_' + way, c.tr.marks)
                self.assertEqual(len(c.lines), 1)
                x, window = c.lines[0]
                self.assertEqual(window, self.LINE_WINDOW)
                don = c.first(C.DISPON, c.boot_t())
                shown = c.mode_frames()
                self.assertLess(don.t, x.t)
                self.assertLess(x.t_end, shown[0].t)
                self.assertLess(c.tr.marks['recovery_' + way], x.t)
                self.assertEqual((len(shown), sends, stops, misses), (4, 4, 0, 0))
                self.assertTrue(all(len(f.params) == 215040 for f in shown))

    @scenario('S-13c/D-TE')
    @requirement('S-13c.017')
    def test_te(self):
        """Recovery's frames start on a TE rising edge, one frame time apart at most, none crossing the next
        edge."""
        c, _, sends, _, misses = self.front('te', frames=6)
        self.assertEqual((sends, misses), (6, 0))
        for f in c.mode_frames():
            edge = max(t for t in c.tr.te if t <= f.t)
            self.assertLess(f.t - edge, 100000)

    @scenario('S-13c/D-TEMISS')
    @requirement('S-13c.017')
    def test_te_stops(self):
        """TE stops 150 ms after MARK recovery and comes back at 450 ms: the frames meanwhile go T_TE_MISS after
        the wait began, counted, and are paced by TE again after it."""
        c, _, sends, _, misses = self.front('te_stops', frames=6, te_stop=150, te_back=450, end=700)
        self.assertEqual(sends, 6)
        self.assertGreater(misses, 0)
        rec = c.tr.marks['recovery']
        self.assertTrue(any(rec + 450 * MS < f.t for f in c.mode_frames()))

    @requirement('S-13c.017')
    def test_no_te(self):
        _, _, sends, _, misses = self.front('no_te', te_us=0)
        self.assertEqual(misses, sends + 1)          # the line's wait too

    @requirement('S-13c.017')
    def test_dma_stopped(self):
        """Recovery's second send stops taking bytes and never ends: it is stopped no earlier than 40 ms after
        its first byte (here at the next change of screen), that frame is sent again whole by the CPU, and so
        is every later one."""
        c, _, sends, stops, _ = self.front('dma_stopped', stall=2, frames=5)
        self.assertEqual((sends, stops), (2, 1))
        shown = c.mode_frames()
        cut = shown[1]
        self.assertLess(len(cut.params), 215040)
        self.assertGreaterEqual(shown[2].t, cut.t_data + 40 * MS)
        self.assertEqual(len(shown), 6)
        self.assertTrue(all(len(f.params) == 215040 for f in shown[2:]))
        self.assertEqual(c.cpu_sent, {id(f) for f in shown[2:]})

    @scenario('S-13c/D-OFF')
    @requirement('S-13c.022')
    def test_poweroff_frame(self):
        """A power-off 10 ms into recovery's second frame: the frame ends whole, nothing follows it on the
        bus, then 0x51 00 00 02."""
        c, _, _, stops, _ = self.front('off', expect=('recovery', 'poweroff'), off=2)
        mark = c.tr.marks['poweroff']
        last = c.frames[-1][0]
        self.assertEqual(stops, 0)
        self.assertIs(c.tr.txns[-1], last)
        self.assertLess(last.t, mark)
        self.assertGreater(last.t_end, mark)
        self.assertEqual(len(last.params), 215040)

    @requirement('S-13c.017', 'S-13c.022')
    def test_poweroff_cut(self):
        """A slow second send (400 ns a byte) with the power-off 10 ms into it: stopped within 60 ms of the
        power-off, nothing after it."""
        c, _, _, stops, _ = self.front('off_cut', expect=('recovery', 'poweroff'), slow=2, off=2)
        self.assertEqual(stops, 1)
        self.assertLessEqual(c.frames[-1][0].t_end, c.tr.marks['poweroff'] + 60 * MS)

    @requirement('S-13c.017')
    def test_dma_stopped_last(self):
        """Recovery's second and last send never ends: stopped at 40 ms and sent again whole by the CPU."""
        c, _, sends, stops, _ = self.front('dma_stopped_last', stall=2, frames=2)
        self.assertEqual((sends, stops), (2, 1))
        self.assertEqual(len(c.mode_frames()), 3)

    @requirement('S-13c.025')
    def test_line_after_menu_fails(self):
        """The checker sees the front's line moved to after recovery's first frame (rule lines)."""
        c, lines, _, _, _ = self.front('line_moved')
        i, j = line_block(lines)
        block, rest = lines[i:j], lines[:i] + lines[j:]
        at = c.mode_frames()[0].t_end + 1 * MS - t_of(block[0])
        moved = ['%d\t%s' % (t_of(x) + at, x.split('\t', 1)[1]) for x in block]
        mutated = sorted(rest + moved, key=t_of)
        fails, _ = C.check(mutated, ['recovery'])
        self.assertIn('lines', [f[0] for f in fails], fails)
        fails, _ = C.check(rest, ['recovery'])
        self.assertIn('lines', [f[0] for f in fails], fails)

    @requirement('S-13c.017')
    def test_stopped_not_sent_fails(self):
        """The checker sees a stopped send that is not sent again (the trace cut at the stop)."""
        c, lines, _, _, _ = self.front('stop_cut', stall=2, frames=5)
        cut = c.mode_frames()[1]
        fails, _ = C.check([x for x in lines if t_of(x) <= cut.t_end], ['recovery'])
        self.assertIn('frames', [f[0] for f in fails], fails)


def declared_font():
    """The font h_s13c declares (S-15.003), in test_s15.check_font's form, plus its width."""
    f = run('s13c', ['font'])[0].split()
    h, w, pitch, margin, top, title, status = (int(x) for x in f[1:8])

    raw = bytes.fromhex(f[8])
    th, tw = int(f[9]), int(f[10])
    traw = bytes.fromhex(f[11])
    return {'height': h, 'width': w, 'pitch': pitch, 'margin': margin, 'top': top,
            'title_row': title, 'status_row': status,
            'glyphs': {chr(0x20 + i): list(raw[i * h:(i + 1) * h]) for i in range(95)},
            'title_height': th, 'title_width': tw,
            'title_glyphs': {chr(0x20 + i): [traw[2 * (i * th + r)] << 8 | traw[2 * (i * th + r) + 1] for r in range(th)]
                             for i in range(95)},
            'bit_order': 'bit width-1 is the leftmost pixel'}


_HANDOVER = {}


def handover_sha():
    """S-15.132's whole hand-over frame, the logo alone: handover_frame.json's (test_s15 Handover)."""
    if 'sha' not in _HANDOVER:
        _HANDOVER['sha'] = hashlib.sha256(T15.handover_frame()).hexdigest()
    return _HANDOVER['sha']


ROWS = 9            # body rows
COLS = 56           # body columns
EDGE = 8            # x of the bars' text (wiki/Screen-Style-Guide.md)
BAR_COLS = 58       # the bars' text cells, x 8 to 471
TITLE = 'Main Menu'
STATUS = 'SLOT  USB'
VERSION = 'waffle-boot v1.0.0'
INDICATOR = 'Protected writes on'
TITLE_COLS = 17     # title bar cells clear of the indicator, x 8 to 177
AMBER, RED, BLACK = 0xFD84, 0xFA88, 0x0000
# the title bar solid from side to side, its text left of x 383 (BAR_END), with four squares of 14 from
# x 392, 22 apart, at y 5 to 18, in its dark color: the first filled, the others outlined; the status bar
# one color
BAR_END = 383
MARKS = [392 + 22 * i for i in range(4)]
AMBER_DARK, RED_DARK = 0xC423, 0xC1E6
STATUS_BG = 0x2124


def line_text(k):
    return ''.join(chr(0x20 + (k * 13 + i) % 95) for i in range(COLS))


def bar(left, right=''):
    """A bar's text row as screen_text decodes it: left from the bar's edge, right ending at its other
    edge (x 472)."""
    return (left.ljust(BAR_COLS - len(right)) + right).rstrip(' ')


def pixel(frame, x, y):
    """Screen (x, y) of a frame (S-13c.023: window row 479 - x, column y; S-13c.015: high byte first)."""
    i = 2 * ((479 - x) * 224 + y)
    return frame[i] << 8 | frame[i + 1]


def screen_text(frame, font, y, x0, cols, bg=frozenset({BLACK})):
    """A text row of a frame (bytes in S-13c.015's order) at screen y, cols glyph cells from screen x0,
    decoded with the font in screen positions (S-15.003). A pixel is ink when it is none of the row's
    background colors bg (black in the body, the status bar's color under it, the title bar's
    amber or red in it); '\0' for a cell that is no glyph, trailing spaces dropped."""
    h, w = font['height'], font['width']
    by_bits = {tuple(v): c for c, v in font['glyphs'].items()}
    s = ''
    for k in range(cols):
        x = x0 + k * w
        bits = tuple(sum(1 << (w - 1 - i) for i in range(w) if pixel(frame, x + i, y + j) not in bg)
                     for j in range(h))
        s += by_bits.get(bits, '\0')
    return s.rstrip(' ')


def body(frame, font):
    """The 9 body rows, on the glyph grid from the margin."""
    return [screen_text(frame, font, font['top'] + i * font['pitch'], font['margin'], COLS)
            for i in range(ROWS)]


def title_bar(left, right=''):
    """The title bar's text as title_row decodes it."""
    return (left.ljust(TITLE_COLS) + right).rstrip(' ')


def title_row(frame, font):
    """The title from the bar's edge, then what ends at its other edge, each in the title bar's glyphs, on
    a grid of its own."""
    w = font['title_width']
    tf = {'height': font['title_height'], 'width': w, 'glyphs': font['title_glyphs']}
    bg = {AMBER, RED}
    y = font['title_row']
    return title_bar(screen_text(frame, tf, y, EDGE, TITLE_COLS, bg),
                     screen_text(frame, tf, y, BAR_END - EDGE - len(INDICATOR) * w, len(INDICATOR), bg))


def status_row(frame, font):
    return screen_text(frame, font, font['status_row'], EDGE, BAR_COLS, {STATUS_BG})


class Screen(Base):
    """The frames the display layer draws (src/display_draw.c) through S-15's console renderer, in the
    frame: the title bar, 9 body rows, the status bar."""

    @classmethod
    def setUpClass(cls):
        cls.font = declared_font()
        os.makedirs(WORK, exist_ok=True)
        # the declared font as a test artifact (S-15.003)
        with open(os.path.join(WORK, 'display_font.json'), 'w', encoding='utf-8') as f:
            json.dump(cls.font, f, indent=1, sort_keys=True)

    @staticmethod
    def frames(c):
        return [x for x in c.tr.txns if x.cmd == C.RAMWR and len(x.params) == 215040]

    @scenario('S-13c/D-ORIENT')
    @requirement('S-13c.023')
    def test_orientation(self):
        # the mark of vectors/orientation.json drawn in screen positions through the draw interface
        lines, _, _ = trace('orient', 'held', orient=1)
        c = self.check(lines, ['held'], ORIENT['sha256'])
        sha = hashlib.sha256(self.frames(c)[-1].params).hexdigest()
        self.assertEqual(sha, ORIENT['sha256'])
        self.assertNotEqual(sha, ORIENT['sha256_mirrored'])

    @requirement('S-15.003', 'S-15.002')
    def test_font(self):
        f = self.font
        self.assertEqual(T15.check_font(f, extent=222), 'ok')
        # Spleen 8x16 on the frame of wiki/Screen-Style-Guide.md
        self.assertEqual((f['height'], f['width'], f['margin'], f['top'], f['pitch']), (16, 8, 16, 33, 19))
        self.assertEqual((f['title_row'], f['status_row']), (4, 205))
        self.assertLessEqual(f['margin'] + COLS * f['width'], 480 - EDGE)
        self.assertLessEqual(EDGE + BAR_COLS * f['width'], 480 - EDGE)

    def check_title_bar(self, frame, color, dark):
        for y in (0, 3, 23):
            self.assertEqual({pixel(frame, c, y) for c in range(0, BAR_END, 7)} | {pixel(frame, BAR_END - 1, y)},
                             {color})
        self.assertEqual({pixel(frame, c, y) for c in range(BAR_END, 480) for y in (0, 4, 19, 23)}, {color})
        self.assertEqual({pixel(frame, c, 12) for c in (BAR_END, MARKS[0] - 1, MARKS[1] - 1, 472, 479)}, {color})
        self.assertEqual({pixel(frame, c, 24) for c in range(0, 480, 10)}, {BLACK})
        self.assertEqual(pixel(frame, MARKS[0] + 7, 12), dark)
        for x0 in MARKS[1:]:
            self.assertEqual([pixel(frame, x0, 5), pixel(frame, x0 + 13, 18), pixel(frame, x0 + 1, 12),
                              pixel(frame, x0 + 2, 12), pixel(frame, x0 + 7, 7)], [dark, dark, dark, color, color])

    def check_bars(self, frame, title=TITLE, right='', status=STATUS):
        self.assertEqual(title_row(frame, self.font), title_bar(title, right))
        self.assertEqual(status_row(frame, self.font), bar(status, VERSION))

    @requirement('S-15.002', 'S-15.003')
    def test_console_frames(self):
        lines, _, _ = trace('screen', 'held', frames=11)
        c = self.check(lines, ['held'])
        held = [x for x in self.frames(c) if x.t > c.tr.marks['held']]
        self.assertEqual(len(held), 11)
        for k, x in enumerate(held):
            shown = [line_text(i) for i in range(max(0, k - ROWS + 1), k + 1)]
            want = [t.rstrip(' ') for t in shown] + [''] * (ROWS - len(shown))
            self.assertEqual(body(x.params, self.font), want, 'frame %d' % k)
            self.check_bars(x.params)
            # the title bar solid amber with its squares and no line under it, the status bar one
            # color from its first row with no rule, the body black
            self.check_title_bar(x.params, AMBER, AMBER_DARK)
            self.assertEqual({pixel(x.params, c, r) for r in (202, 203, 204, 221) for c in range(0, 480, 10)},
                             {STATUS_BG})

    @requirement('S-15.002')
    def test_status_words(self):
        lines, _, _ = trace('status', 'held', frames=1, status=0x01, slot=1)
        c = self.check(lines, ['held'])
        last = self.frames(c)[-1].params
        self.check_bars(last, status='SLOT2  USB')
        self.assertEqual(body(last, self.font), [line_text(0)] + [''] * (ROWS - 1))

    @requirement('S-15.140')
    def test_indicator(self):
        lines, _, _ = trace('option', 'held', frames=16, option=1)
        c = self.check(lines, ['held'])
        for x in self.frames(c)[-16:]:
            # every held frame: the red bar, its title and the indicator at its text part's right end,
            # the status bar's first row its one color
            self.check_bars(x.params, right=INDICATOR)
            self.check_title_bar(x.params, RED, RED_DARK)
            self.assertEqual(pixel(x.params, 240, 202), STATUS_BG)
        last = self.frames(c)[-1].params
        # the `indicator` token on the title bar's text row (info_tokens.json)
        self.assertTrue(T15.info_match(INFO_TOKENS['indicator'], [title_row(last, self.font)], {}))
        # all 9 body rows decode with the indicator shown
        self.assertEqual(body(last, self.font), [line_text(i).rstrip(' ') for i in range(7, 16)])

    @scenario('S-13c/D-IMAGE')
    @requirement('S-13c.024')
    def test_image(self):
        """A change that moves every console line, made 0 to 15 ms after a held-mode frame's first byte, with
        TE and without: every held-mode frame is, byte for byte, one of the run's settled screens (the last
        frame before each change, and the run's last frame), never rows of two screens."""
        for te in (16667, 0):
            with self.subTest(te_us=te):
                lines, _, _ = trace('image_%d' % te, 'held', image=1, te_us=te)
                c = self.check(lines, ['held'])
                held = [x for x in self.frames(c) if x.t > c.tr.marks['held']]
                changes = c.tr.changes
                self.assertEqual(len(changes), 16)
                before = [[x for x in held if x.t < ch][-1] for ch in changes]
                settled = [bytes(x.params) for x in before] + [bytes(held[-1].params)]
                self.assertEqual(len(set(settled)), 17)          # every change moves every line
                for k, (ch, x) in enumerate(zip(changes, before)):
                    # the change k ms after the first byte of a frame of the screen as it stands, while that
                    # frame goes out
                    self.assertEqual((ch - x.t) // MS, k)
                    self.assertLess(ch, x.t_end)
                    self.assertEqual(bytes(x.params), settled[k])
                    after = [y for y in held if y.t > ch][0]
                    self.assertEqual(bytes(after.params), settled[k + 1])
                for x in held:
                    self.assertIn(bytes(x.params), settled, 'frame at %d ns' % x.t)

    @requirement('S-14.060')
    def test_fatal_frame(self):
        lines, _, _ = trace('fatal_screen', 'held', fatal=1)
        c = self.check(lines, ['held'])
        last = self.frames(c)[-1]
        self.assertGreater(last.t, c.tr.marks['fatal'])
        self.assertEqual(body(last.params, self.font)[:3],
                         [line_text(0).rstrip(' '), line_text(1).rstrip(' '), 'fatal: test'])
        self.check_bars(last.params, title='Fatal Error')
        self.check_title_bar(last.params, RED, RED_DARK)

    @requirement('S-13c.022')
    def test_shutdown_frame(self):
        """No frame at the power-off: the last frame is the last held-mode screen, the console as it stood,
        sent before MARK poweroff; nothing is drawn and sent for the power-off."""
        for n in (1, 2):
            with self.subTest(poweroff=n):
                lines, _, _ = trace('off_screen_%d' % n, 'held', poweroff=n)
                c = self.check(lines, ['held', 'poweroff'])
                frames = self.frames(c)
                self.assertLess(frames[-1].t, c.tr.marks['poweroff'])
                self.assertEqual(body(frames[-1].params, self.font),
                                 [line_text(i).rstrip(' ') for i in range(4)] + [''] * (ROWS - 4))
                self.check_bars(frames[-1].params)
                self.assertNotEqual(frames[-1].params, frames[-2].params)

    @requirement('S-15.190')
    def test_continue_frames(self):
        """S-15.190 with the lines of src/s15_lines.c, as drawn: the restarting view, the last frame before
        MARK poweroff, carries `continue:restarting` (info_tokens.json). Every line fits its row."""
        lines, _, _ = trace('cont_screen', 'held', frames=2, cont=1)
        c = self.check(lines, ['held', 'poweroff'])
        view = self.frames(c)[-1]
        self.assertLess(view.t_end, c.tr.marks['poweroff'])
        rows = body(view.params, self.font)
        self.assertTrue(T15.info_match(INFO_TOKENS['continue:restarting'], rows, {}), rows)
        self.assertNotIn('\0', ''.join(rows))


if __name__ == '__main__':
    unittest.main()
