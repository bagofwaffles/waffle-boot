#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the S-13c synthetic panel-bus traces and the frame-format vector.

Every trace is generated from init_reference.tsv (one conforming order), init_registers.tsv and timing.tsv.
The gamma tables are an identity ramp (S-13c.012); the hand-over frame is black with a white 109 x 36
block standing in for the logo.

    python3 gen_vectors.py            writes vectors/*.tsv, vectors/index.json, vectors/frame_pattern.json

Python 3, standard library only.
"""
import hashlib
import json
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
V = os.path.join(HERE, 'vectors')
US, MS = 1000, 1000000

GAMMA = bytes(4 * i for i in range(64))      # identity ramp (S-13c.012)
FRAME_W, FRAME_H = 224, 480                   # columns 49..272, rows 0..479
FILL_PIXELS = 222 * 480
LOGO_W, LOGO_H = 109, 36                      # a stand-in block of the logo's size
LOGO_C, LOGO_R = (FRAME_W - LOGO_W) // 2, (FRAME_H - LOGO_H) // 2
VISIBLE_FILL = ('29', '30', '31')             # init_reference.tsv steps filling the visible window


def load_tsv(name):
    rows, header = [], None
    with open(os.path.join(HERE, name)) as f:
        for line in f:
            line = line.rstrip('\n')
            if not line or line.startswith('#'):
                continue
            cells = line.split('\t')
            if header is None:
                header = cells
                continue
            rows.append(dict(zip(header, cells + [''] * (len(header) - len(cells)))))
    return rows


REFERENCE_ORDER = load_tsv('init_reference.tsv')


class Builder:
    """Accumulates trace lines with a time cursor in ns. Bytes on the GPIO bus take 1 us each."""

    def __init__(self):
        self.lines = []
        self.t = 0

    def ev(self, *f):
        self.lines.append('\t'.join(str(x) for x in (self.t,) + f))

    def wait(self, ns):
        self.t += ns

    def at(self, ns):
        self.t = max(self.t, ns)

    def reset_pulse(self, width=10 * US + 500):
        self.ev('RESET', 0)
        self.wait(width)
        self.ev('RESET', 1)
        self.wait(1 * US)

    def cs(self, lvl):
        self.ev('CS', lvl)
        self.wait(200)

    def cmd(self, c, params=b'', suffix=''):
        self.ev('CMD' + suffix, '%02X' % c)
        self.wait(1 * US)
        if params:
            self.ev('DATA' + suffix, params.hex().upper(), 'end=%d' % (self.t + (len(params) - 1) * US))
            self.wait(len(params) * US)

    def run(self, count, group, byte_ns):
        n = count * len(group)
        self.ev('RUN', count, group.hex().upper(), 'end=%d' % (self.t + (n - 1) * byte_ns))
        self.wait(n * byte_ns)

    def data(self, payload, byte_ns):
        self.ev('DATA', payload.hex().upper(), 'end=%d' % (self.t + (len(payload) - 1) * byte_ns))
        self.wait(len(payload) * byte_ns)

    def read_reg(self, c, n):
        self.cs(0)
        self.cmd(c)
        for v in [0x00] + [0x77, 0x96, 0x00][:n]:
            self.ev('READ', '%02X' % v)
            self.wait(2 * US)
        self.cs(1)

    def te_at(self, t):
        """A TE edge at an absolute time, independent of the cursor (the panel's own clock)."""
        self.lines.append('%d\tTE\t1' % t)

    def text(self):
        lines = sorted(self.lines, key=lambda l: int(l.split('\t', 1)[0]))   # stable: same-time order kept
        return '\n'.join(lines) + '\n'


def hx(s):
    return bytes.fromhex(s.replace(' ', ''))


def init_sequence(b, gamma, fill=b'\xff\xff', opts=None):
    """The reference order (S-13c.008-010) with optional deviations for negative vectors."""
    o = opts or {}
    b.cs(0)
    for row in REFERENCE_ORDER:
        if row['when'] == 'gamma' and not gamma:
            continue
        c = int(row['cmd'], 16)
        if c == 0x29 and o.get('dispon_early'):
            continue
        if o.get('logo_first') and (c == 0x29 or row['step'] in VISIBLE_FILL):
            continue                           # S-13c.010's logo form: the early logo frame, then DISPON
        if row['params'] in ('GAMMA_R', 'GAMMA_B'):
            p = GAMMA[:o.get('gamma_len', 64)]
            if o.get('gamma_no_tables'):
                continue
        elif row['params'] == 'FILL':
            if o.get('dispon_early'):
                b.cmd(0x29)
            b.cmd(0x2C)
            pixels = FILL_PIXELS - (480 * 10 if o.get('prefill_partial') else 0)
            b.run(pixels, fill, o.get('fill_byte_ns', 70))
            b.wait(1 * MS)
            continue
        else:
            p = hx(row['params'])
        if c == 0xE8 and o.get('missing_e8'):
            continue
        if c == 0xB9 and o.get('b9_42'):
            p = b'\x42\xe0'                   # digital gamma on with no table (S-13c.026's negative)
        if c == 0xF0 and row['step'] == '3' and o.get('set2_before_unlock'):
            b.cmd(0xC5, b'\x18')
        if c == 0xC5 and o.get('set2_before_unlock'):
            continue
        if c == 0xC5 and o.get('vcom'):
            p = bytes([o['vcom']])
        if o.get('unlocked') and c == 0xF0 and row['params'] in ('3C', '69'):
            continue
        if c == 0x2A and o.get('window') and row['step'] == '32':
            p = o['window']
        b.cmd(c, p)
        if c == 0x11:
            b.wait(o.get('slpout_gap', int(row['gap_us']) * US))
    if o.get('unlocked'):
        b.cmd(0xF0, b'\x3c')
        b.cmd(0xF0, b'\x69')
    if o.get('double_init'):
        b.cmd(0x11)
        b.wait(5 * MS)
        b.cmd(0x3A, b'\x55')


def logo_runs():
    """The hand-over frame as RUN groups: black, with a white block at the center (S-15.132: the logo alone)."""
    blk, wht = b'\x00\x00', b'\xff\xff'
    row_logo = blk * LOGO_C + wht * LOGO_W + blk * (FRAME_W - LOGO_C - LOGO_W)
    return [(FRAME_W * LOGO_R, blk), (LOGO_H, row_logo), (FRAME_W * (FRAME_H - LOGO_R - LOGO_H), blk)]


def logo_before_dispon(b, how):
    """S-13c.010's logo form: the early logo frame with the display off, then DISPON after its last byte.
    how: True; 'twice' (a second frame before DISPON); 'other' (another image before DISPON, the logo after
    it); 'dispon_early' (DISPON among the frame's bytes); 'lit_before_dispon' (0x51 3C FF 02 before DISPON)."""
    runs = logo_runs()
    if how == 'dispon_early':
        b.cmd(0x2C)
        b.run(runs[0][0], runs[0][1], 70)
        b.cmd(0x29)                            # the rows after it reach DISPON, not the panel's memory
        for count, group in runs[1:]:
            b.run(count, group, 70)
        return
    if how == 'other':
        frame(b, runs=[(FRAME_W * FRAME_H // 2, b'\xff\xff'), (FRAME_W * FRAME_H // 2, b'\x00\x00')])
        b.cmd(0x29)
        b.wait(1 * MS)
        frame(b, runs=runs)
        return
    frame(b, runs=runs)
    if how == 'twice':
        frame(b, runs=runs)
    if how == 'lit_before_dispon':
        b.wait(100 * US)
        b.ev('MSG51', '3C FF 02')
        b.wait(100 * US)
    b.cmd(0x29)


def frame(b, pixels=None, byte_ns=70, short=0, runs=None):
    """One frame of the window 49..272 x 0..479 (224 x 480): black unless pixels (bytes) or runs
    ([(count, group)]) is given."""
    b.cmd(0x2C)
    if runs is not None:
        for count, group in runs:
            b.run(count, group, byte_ns)
    elif pixels is None:
        b.run(FRAME_W * FRAME_H - short, b'\x00\x00', byte_ns)
    else:
        b.data(pixels, byte_ns)


def logo_line(b, how=True):
    """S-13c.025: the line under the logo, screen y 140-155 (panel columns 189-204) over all 480 rows, then
    S-13c.014's window again. how: True; 'short' (a pixel short); 'keep_window' (the band's window left for the
    next frame)."""
    b.cmd(0x2A, hx('00 BD 00 CC'))
    b.cmd(0x2B, hx('00 00 01 DF'))
    b.cmd(0x2C)
    b.run(16 * 480 - (1 if how == 'short' else 0), b'\x8c\x71', 70)
    if how != 'keep_window':
        b.cmd(0x2A, hx('00 31 01 10'))


def record(reset_ms):
    r = bytearray(44)
    r[0] = 1
    r[4:8] = struct.pack('<I', reset_ms)
    r[8:12] = struct.pack('<I', 2)
    r[0x28] = 1
    return bytes(r)


def build(path='boot', handshake=True, reads=True, **o):
    """path: boot | held | fail | off (a power-off during the start) | recovery (the front's, S-02.084: way
    is chord, check or count, MARK recovery_<way> when the front decides). device '1f': a device without
    digital-gamma and straps (S-13c.026, S-13c.027): no MARK strap, no gamma load, and MARK no_handshake when
    the bootloader finds no valid handshake (`future`: the first stage pulsed, its time not before the
    bootloader's clock). Returns (trace text, expect list)."""
    b = Builder()
    dev = o.get('device', 'xy')
    # first stage (S-01.006): its pulse at 2 ms, handshake left for the bootloader
    b.at(2 * MS)
    if handshake:
        b.reset_pulse()
        reset_ms = 2
    b.at(40 * MS)
    b.ev('MARK', 'bootloader')
    b.wait(100 * US)
    if dev == 'xy' or o.get('strap_mark'):
        b.ev('MARK', 'strap')                  # PA8 an input for the strap sample (S-13b.001)
    if dev == '1f':
        # S-13c.027: the values the reset decision reads (S-01.006); a time not before the clock is no
        # valid handshake
        b.ev('DMA9', '0x12345678' if handshake else '0x00000000',
             0x7FFFFFFF if o.get('future') else (2 if handshake else 0))
    if dev == '1f' and (not handshake or o.get('future')) and not o.get('no_mark'):
        b.ev('MARK', 'no_handshake')           # S-13c.027: DMA9_XMOD/YMOD give no valid handshake
    if o.get('strap_low'):
        b.wait(1 * US)
        b.ev('RESET', 0)                       # the RESET level follows PA8's strap level
    b.at(50 * MS)                              # the display bus set-up drives PA8 high
    if o.get('strap_low') or not handshake:
        b.ev('RESET', 1)
    if o.get('strap_low'):
        reset_ms = b.t // MS                   # the panel's last release (S-13c.004)
    if not handshake:
        b.wait(1 * US)
        b.reset_pulse(o.get('reset_width', 10 * US + 500))
        reset_ms = b.t // MS
        if o.get('second_reset'):
            b.reset_pulse()
            reset_ms = b.t // MS
    elif o.get('reset_width'):
        b.reset_pulse(o['reset_width'])
        reset_ms = b.t // MS
    if o.get('trust_handshake'):
        reset_ms = 2                           # the first stage's time, though PA8 strapped low
    if o.get('byte_in_reset'):
        b.ev('RESET', 0)
        b.ev('CMD!', '00')
        b.wait(20 * US)
        b.ev('RESET', 1)
    if not o.get('msg51_missing_off'):
        b.ev('MSG51', '00 00 00')
    if o.get('regreet'):
        b.wait(1 * MS)
        b.ev('GREETING')
        b.wait(200 * US)
        b.ev('MSG51', '3C FF 02')
    if o.get('msg51_early'):
        b.wait(1 * MS)
        b.ev('MSG51', '3C FF 32')
    rises, lvl = [], 1                         # rising edges of the RESET level
    for l in sorted(b.lines, key=lambda l: int(l.split('\t', 1)[0])):
        f = l.split('\t')
        if f[1] == 'RESET':
            if f[2] == '1' and lvl == 0:
                rises.append(int(f[0]))
            lvl = int(f[2])
    last_rise = rises[0] if o.get('trust_handshake') else rises[-1]
    b.at(last_rise + o.get('reset_wait', 120 * MS + 500 * US))
    if reads:
        for _ in range(5):
            b.read_reg(0x04, 3)
            b.read_reg(0xD3, 2)
    if o.get('read_with_write'):
        b.cs(0)
        b.cmd(0x04)
        b.ev('READ', '00')
        b.wait(US)
        b.ev('DATA', '00')
        b.wait(US)
        b.cs(1)
    # S-05.004: the run's one initialization loads the gamma tables on every path
    early = o.get('early', 'lit')
    gamma = early != 'none' or path in ('boot', 'fail')
    if dev == '1f':
        gamma = o.get('gamma_load', False)     # S-13c.026: no gamma load on any path
    init_sequence(b, gamma, fill=o.get('fill', b'\xff\xff'), opts=o)
    if o.get('reset_after_init'):
        b.cs(1)
        b.reset_pulse()
        b.cs(0)
    if o.get('cs_high_cmd'):
        b.cs(1)
        b.ev('CMD!', '00')
        b.wait(US)
        b.cs(0)
    expect = [path]
    lf = o.get('logo_first')
    if early != 'none':
        # S-05.004, S-13c.019: after the key status, the early logo frame, lit by
        # 0x51 3C FF 02 after its last byte; then the slave's stream and start-up.
        b.wait(1 * MS)
        if early == 'first':
            b.ev('MSG51', '3C FF 02')
            b.wait(100 * US)
        if lf:
            logo_before_dispon(b, lf)
        else:
            frame(b, runs=None if early == 'image' else logo_runs())
        b.wait(100 * US)
        if early in ('lit', 'image') and o.get('prompt') != 'dark':
            b.ev('MSG51', '3C FF 02')
        if o.get('extra'):
            # a frame of its own between the early logo frame and held mode (S-13c.019)
            b.wait(1 * MS)
            frame(b, runs=logo_runs())
        if o.get('line') == 'boot':
            # a line under the logo on the boot path (S-13c.025)
            b.wait(1 * MS)
            logo_line(b)
        b.wait((RECOVERY_DECIDE[o['way']] if path == 'recovery' else 500) * MS)
    if path == 'recovery':
        return recovery(b, o, expect)
    if o.get('prompt'):
        # S-05.025, S-13c.019: a boot prompt, lit by the early logo's 3C FF 02, before
        # the hand-over frame or held mode; waits for a key
        b.wait(1 * MS)
        b.ev('MARK', 'prompt')
        b.wait(100 * US)
        if o['prompt'] == 'bright_first':
            b.ev('MSG51', '3C FF 32')
            b.wait(1 * MS)
        frame(b)
        b.wait(3000 * MS)
    if path == 'off':
        # S-07.051 during the start: nothing more to the panel (S-13c.022), then 0x51 00 00 02 (S-13c.019).
        # dispoff: a DISPOFF at the power-off (S-13c.022)
        b.wait(1 * MS)
        b.ev('MARK', 'poweroff')
        if o.get('dispoff'):
            b.cmd(0x28)
        if o.get('frame_after_off'):
            b.wait(1 * US)
            frame(b)
        b.wait(1 * MS)
        b.ev('MSG51', '00 00 02')
        return b.text(), ['poweroff']
    if path in ('boot', 'fail') and o.get('prompt') == 'last':
        b.wait(100 * US)
        b.ev('MSG51', '3C FF 32')
        b.wait(10 * MS)
        rec = o.get('record', record(reset_ms))
        b.ev('HANDOVER', 'now_ms=%d' % (b.t // MS), 'record=%s' % rec.hex().upper())
        return b.text(), expect
    kept = o.get('kept')
    if path in ('boot', 'fail') and kept:
        # S-05.004: the early logo frame kept as the hand-over frame; 'gamma' a run that took the panel on from
        # the front loads the gamma after it (S-13c.012), 'cmd' another transaction after it
        if kept == 'gamma':
            b.wait(1 * MS)
            b.cmd(0xF0, b'\xc3')
            b.cmd(0xF0, b'\x96')
            b.cmd(0xE2, GAMMA)
            b.cmd(0xE3, GAMMA)
            b.cmd(0xB9, b'\x42\xe0')
            b.cmd(0xF0, b'\x3c')
            b.cmd(0xF0, b'\x69')
        if kept == 'cmd':
            b.wait(1 * MS)
            b.cmd(0x2A, hx('00 31 01 10'))
    elif path in ('boot', 'fail'):
        runs = None if o.get('pixels') or o.get('short') else logo_runs()
        frame(b, o.get('pixels'), short=o.get('short', 0), runs=runs)
    if path in ('boot', 'fail'):
        if o.get('cmd_after_frame'):
            b.wait(10 * US)
            b.cmd(0x2A, hx('00 31 01 10'))
        b.wait(100 * US)
        b.ev('MSG51', '3C FF 32')
        b.wait(10 * MS)
        if path == 'boot':
            now_ms = b.t // MS
            rec = o.get('record', record(reset_ms))
            b.ev('HANDOVER', 'now_ms=%d' % now_ms, 'record=%s' % rec.hex().upper())
            return b.text(), expect
        b.wait(200 * MS)                       # the load fails (S-05.006)
    b.wait(20 * MS)
    line = o.get('line')
    if line and line not in ('boot', 'late'):
        # S-13c.025: held mode's start: the line under the logo, then the mount and the rest before its
        # first frame
        logo_line(b, line)
        b.wait(4000 * MS)
    if not o.get('held_dark'):
        b.ev('MSG51', '3C FF 02')
    b.ev('MARK', 'held')
    if o.get('held_plan') is not None:
        # S-13c.017 TE-miss vectors: (kind, ns after MARK held); 'te' is a TE rising edge, 'frame' a frame start
        t0 = b.t
        for kind, dt in o['held_plan']:
            if kind == 'te':
                b.te_at(t0 + dt)
            elif kind == 'change':                     # a menu change (CHANGE), independent of the cursor
                b.lines.append('%d\tCHANGE' % (t0 + dt))
            else:
                b.at(t0 + dt)
                frame(b)
        return b.text(), expect
    te0 = b.t + 3 * MS
    period = 16667 * US
    for k in range(5):
        if not o.get('no_te'):
            b.te_at(te0 + k * period)
    for k in range(4):
        b.at(te0 + k * period)
        b.wait(20 * US)
        frame(b, byte_ns=o.get('frame_byte_ns', 70))
        if k == 1 and o.get('two_frames_one_te'):
            frame(b)
        if k == 1 and o.get('line') == 'late':
            b.wait(1 * MS)
            logo_line(b)                       # a line over held mode's screen (S-13c.025)
        if k == 1 and o.get('fatal'):
            b.at(te0 + k * period + 8 * MS)    # mid-period: a fatal error is shown at once
            b.ev('MARK', 'fatal')
            b.wait(5 * US)
            frame(b)
            break
    if o.get('poweroff'):
        # S-13c.022: the power-off; no frame starts after it and no command is sent, the panel keeps its last
        # frame; then 0x51 00 00 02 (S-13c.019). poweroff: True; 'dispoff' (a DISPOFF at the power-off);
        # 'frame' (a frame after it); 'in_frame' (the power-off comes 5 ms into a frame, which ends; the
        # message follows its last byte); 'msg_in_frame' (as in_frame, the message 1 ms after the power-off,
        # among the frame's bytes); 'handover' (a restart, S-05.033, that still sends the hand-over frame,
        # 3C FF 32 and the kernel call after it)
        how = o['poweroff']
        expect.append('poweroff')
        b.at(te0 + 4 * period)
        if how in ('in_frame', 'msg_in_frame'):
            b.wait(20 * US)
            b.lines.append('%d\tMARK\tpoweroff' % (b.t + 5 * MS))
            if how == 'msg_in_frame':
                b.lines.append('%d\tMSG51\t00 00 02' % (b.t + 6 * MS))
            frame(b)
            if how == 'msg_in_frame':
                return b.text(), expect
        else:
            b.ev('MARK', 'poweroff')
            if how == 'dispoff':
                b.cmd(0x28)
            elif how == 'frame':
                b.wait(US)
                frame(b)
        b.wait(1 * MS)
        b.ev('MSG51', '00 00 02')
        if how == 'handover':
            b.wait(30 * MS)
            frame(b, runs=logo_runs())
            b.wait(100 * US)
            b.ev('MSG51', '3C FF 32')
            b.wait(10 * MS)
            b.ev('HANDOVER', 'now_ms=%d' % (b.t // MS), 'record=%s' % record(reset_ms).hex().upper())
    return b.text(), expect


# S-02.084: when the front decides on recovery after the early logo frame is lit, in ms: at the key read
# (Power + M4), after the start log's read (three unfinished starts), after the image check (it failed).
RECOVERY_DECIDE = {'chord': 1, 'count': 30, 'check': 400}


def recovery(b, o, expect):
    """S-13c.025's front's line under the logo, then recovery's frames from MARK recovery (S-13c.017), by
    plan (as held_plan, with the sends of play()) or four frames at TE's 60 Hz. line: True, or 'after_menu' (the
    line after recovery's first frame)."""
    b.ev('MARK', 'recovery_' + o['way'])
    line = o.get('line', True)
    if line is True:
        b.wait(100 * US)
        logo_line(b)
    b.wait(300 * MS)                           # the front's start-up and its slot reads
    b.ev('MARK', 'recovery')
    t0 = b.t
    plan = o.get('plan') or [('te', 3 * MS + k * 16667 * US) for k in range(5)] + \
        [('frame', 3 * MS + 20 * US + k * 16667 * US) for k in range(4)]
    shown = False
    for kind, dt in plan:
        play(b, t0, kind, dt)
        if line == 'after_menu' and kind not in ('te', 'change') and not shown:
            shown = True
            b.wait(1 * MS)
            logo_line(b)
    if any(kind == 'offcut' for kind, _ in plan):
        expect.append('poweroff')
    return b.text(), expect


def play(b, t0, kind, dt):
    """One step of a plan at t0 + dt: 'te' a TE rising edge; 'change' a CHANGE; 'frame' a frame by DMA at
    70 ns a byte; 'cut' a DMA send that stalls after 50 000 bytes and is stopped 40 ms after its first byte;
    'cpu' a frame sent by the CPU, 200 ns a byte; 'late' a DMA send that runs 60 ms; 'offcut' a slow send,
    400 ns a byte, with the power-off 10 ms after its first byte, stopped 60 ms after the power-off, then
    0x51 00 00 02."""
    if kind == 'te':
        b.te_at(t0 + dt)
        return
    if kind == 'change':
        b.lines.append('%d\tCHANGE' % (t0 + dt))
        return
    b.at(t0 + dt)
    if kind == 'frame':
        frame(b)
    elif kind == 'cut':
        b.cmd(0x2C)
        b.run(25000, b'\x00\x00', 70)
    elif kind == 'cpu':
        frame(b, byte_ns=200)
    elif kind == 'late':
        frame(b, byte_ns=279)
    elif kind == 'offcut':
        b.cmd(0x2C)
        b.lines.append('%d\tMARK\tpoweroff' % (b.t + 10 * MS))
        b.run(87500, b'\x00\x00', 400)
        b.wait(1 * MS)
        b.ev('MSG51', '00 00 02')


def te_every(n, start=3 * MS):
    return [('te', start + k * 16667 * US) for k in range(n)]


# S-13c.017 in recovery: a send stopped at 40 ms, then the CPU's frames at TE; a send running 60 ms; a slow
# send at the power-off, stopped 60 ms after it.
FRONT_DMA_STOPPED = te_every(10) + [('frame', 3 * MS + 20 * US), ('cut', 19687 * US),
                                    ('cpu', 69687 * US), ('cpu', 119687 * US)]
FRONT_DMA_LATE = te_every(6) + [('frame', 3 * MS + 20 * US), ('late', 19687 * US)]
FRONT_POWEROFF_CUT = te_every(7) + [('frame', 3 * MS + 20 * US), ('offcut', 19687 * US)]


# Held-mode plans for the TE-miss vectors (S-13c.017): (kind, ns after MARK held). A frame takes 215 040 x
# 70 ns, about 15.05 ms; FRAME_NS is that time rounded up.
FRAME_NS = 15100 * US
TE_STOPS = [('te', 3 * MS), ('frame', 3 * MS + 20 * US),
            ('te', 19667 * US), ('frame', 19687 * US), ('change', 25 * MS), ('change', 90 * MS),
            ('frame', 19687 * US + FRAME_NS + 50 * MS),
            ('frame', 19687 * US + 2 * FRAME_NS + 100 * MS),
            ('te', 200 * MS), ('frame', 200 * MS + 20 * US),
            ('te', 216667 * US), ('frame', 216687 * US),
            ('te', 233333 * US)]
TE_MISS_SHORT = [(k, (19687 * US + FRAME_NS + 30 * MS) if (k, t) == ('frame', 19687 * US + FRAME_NS + 50 * MS)
                  else t) for k, t in TE_STOPS]
TE_MISS_LONG = [('te', 3 * MS), ('frame', 3 * MS + 20 * US),
                ('te', 19667 * US), ('frame', 19687 * US), ('change', 40 * MS),
                ('frame', 19687 * US + FRAME_NS + 400 * MS),
                ('te', 600 * MS), ('frame', 600 * MS + 20 * US)]
TE_FROZEN = [('te', 300 * MS), ('frame', 300 * MS + 20 * US),
             ('te', 316667 * US), ('frame', 316687 * US),
             ('te', 333333 * US)]


# name -> (build kwargs, rule expected to fail or None for a pass, requirement)
VECTORS = {
    'good_boot_handshake':   (dict(path='boot'), None, 'S-13c.004-021'),
    'good_boot_own_reset':   (dict(path='boot', handshake=False), None, 'S-13c.004'),
    'good_boot_no_reads':    (dict(path='boot', reads=False, fill=b'\x00\x00'), None, 'S-13c.010, S-13c.011'),
    'good_boot_regreet':     (dict(path='boot', regreet=True), None, 'S-13c.019'),
    'good_held':             (dict(path='held'), None, 'S-13c.012, S-13c.017'),
    'good_held_poweroff':    (dict(path='held', poweroff=True), None, 'S-13c.022'),
    'good_fail_then_held':   (dict(path='fail'), None, 'S-13c.013'),
    'bad_cs_high':           (dict(path='boot', cs_high_cmd=True), 'bus', 'S-13c.002'),
    'bad_byte_in_reset':     (dict(path='boot', byte_in_reset=True), 'bus', 'S-13c.002'),
    'bad_reset_9us':         (dict(path='boot', handshake=False, reset_width=9 * US), 'reset', 'S-13c.005'),
    'bad_reset_wait':        (dict(path='boot', reset_wait=119 * MS), 'reset_wait', 'S-13c.006'),
    'bad_reset_wait_1us_short': (dict(path='boot', handshake=False, reset_wait=120 * MS - 1 * US), 'reset_wait',
                                 'S-13c.006'),
    'good_reset_wait_exact': (dict(path='boot', handshake=False, reset_wait=120 * MS), None, 'S-13c.006'),
    'bad_slpout_4ms':        (dict(path='boot', slpout_gap=4 * MS), 'slpout', 'S-13c.007'),
    'bad_missing_e8':        (dict(path='boot', missing_e8=True), 'registers', 'S-13c.008'),
    'bad_set2_before_unlock': (dict(path='boot', set2_before_unlock=True), 'cscon', 'S-13c.009'),
    'bad_held_no_te':        (dict(path='held', no_te=True), 'te', 'S-13c.017'),
    'bad_vcom':              (dict(path='boot', vcom=0x19), 'registers', 'S-13c.008'),
    'bad_unlocked':          (dict(path='boot', unlocked=True), 'cscon', 'S-13c.009'),
    'bad_dispon_early':      (dict(path='boot', dispon_early=True), 'prefill', 'S-13c.010'),
    'bad_prefill_partial':   (dict(path='boot', prefill_partial=True), 'prefill', 'S-13c.010'),
    'bad_read_with_write':   (dict(path='boot', read_with_write=True), 'reads', 'S-13c.011'),
    'bad_gamma_short':       (dict(path='boot', gamma_len=63), 'gamma', 'S-13c.012'),
    'bad_gamma_no_tables':   (dict(path='boot', gamma_no_tables=True), 'gamma', 'S-13c.012'),
    'bad_double_init':       (dict(path='boot', double_init=True), 'once', 'S-13c.013'),
    'bad_window':            (dict(path='boot', window=hx('00 32 01 11')), 'frames', 'S-13c.014'),
    'bad_short_frame':       (dict(path='boot', short=1), 'frames', 'S-13c.016'),
    'bad_two_frames_one_te': (dict(path='held', two_frames_one_te=True), 'te', 'S-13c.017'),
    'bad_frame_overruns_te': (dict(path='held', frame_byte_ns=80), 'te', 'S-13c.018'),
    'bad_msg51_early':       (dict(path='boot', msg51_early=True), 'msg51', 'S-13c.019'),
    'bad_cmd_after_frame':   (dict(path='boot', cmd_after_frame=True), 'handover', 'S-13c.020'),
    # S-05.004: the early logo frame kept as the hand-over frame, with the gamma's load after it or not
    'good_boot_logo_kept':   (dict(path='boot', kept=True), None, 'S-05.004, S-13c.020'),
    'good_boot_logo_kept_gamma': (dict(path='boot', kept='gamma'), None, 'S-05.004, S-13c.012, S-13c.020'),
    'good_logo_first_kept_gamma': (dict(path='boot', logo_first=True, kept='gamma'), None,
                                   'S-05.004, S-13c.012, S-13c.020'),
    'bad_logo_kept_cmd':     (dict(path='boot', kept='cmd'), 'handover', 'S-13c.020'),
    # S-13c.025: the line under the logo in held mode's start, over the early logo or the hand-over frame
    'good_held_logo_line':   (dict(path='held', line=True), None, 'S-13c.025'),
    'good_fail_logo_line':   (dict(path='fail', line=True), None, 'S-13c.025'),
    'good_fail_kept_logo_line': (dict(path='fail', kept=True, line=True), None, 'S-05.004, S-13c.025'),
    'bad_logo_line_short':   (dict(path='held', line='short'), 'lines', 'S-13c.025'),
    'bad_logo_line_window':  (dict(path='held', line='keep_window'), 'frames', 'S-13c.014, S-13c.025'),
    'bad_logo_line_late':    (dict(path='held', line='late'), 'lines', 'S-13c.025'),
    'bad_logo_line_boot':    (dict(path='boot', line='boot'), 'lines', 'S-13c.025'),
    'bad_record':            (dict(path='boot', record=record(0)), 'record', 'S-13c.021'),
    'bad_record_stale':      (dict(path='boot', handshake=False, record=record(1)), 'record', 'S-13c.021'),
    'bad_record_future':     (dict(path='boot', record=record(0x7FFFFFF0)), 'record', 'S-13c.021'),
    'good_strap_low_handshake': (dict(path='boot', strap_low=True), None, 'S-13c.004, S-13c.006, S-13c.021'),
    'good_strap_low_own_reset': (dict(path='boot', handshake=False, strap_low=True), None, 'S-13c.004, S-13c.005'),
    'bad_strap_handshake':   (dict(path='boot', strap_low=True, trust_handshake=True), 'reset_wait',
                              'S-13c.004, S-13c.006, S-13c.021'),
    'bad_second_reset':      (dict(path='boot', handshake=False, second_reset=True), 'reset', 'S-13c.005'),
    'bad_reset_after_init':  (dict(path='boot', reset_after_init=True), 'reset', 'S-13c.005'),
    'bad_frame_byte_rate':   (dict(path='held', frame_byte_ns=60), 'byte_rate', 'S-13c.018'),
    'bad_prefill_byte_rate': (dict(path='boot', fill_byte_ns=60), 'byte_rate', 'S-13c.003'),
    'good_held_fatal':       (dict(path='held', fatal=True), None, 'S-13c.017'),
    # S-13c.019 the boot prompt: lit, on to each path; dark, after the bright report, or
    # with no hand-over frame after it
    'good_boot_prompt':      (dict(path='boot', prompt='lit'), None, 'S-05.025, S-13c.019'),
    'good_prompt_then_held': (dict(path='held', prompt='lit'), None, 'S-05.025, S-13c.019'),
    'bad_boot_prompt_dark':  (dict(path='boot', prompt='dark'), 'msg51', 'S-13c.019'),
    'bad_boot_prompt_bright_first': (dict(path='boot', prompt='bright_first'), 'msg51', 'S-13c.019'),
    'bad_boot_prompt_last':  (dict(path='boot', prompt='last'), 'msg51', 'S-13c.019'),
    'bad_prompt_then_held_dark': (dict(path='held', prompt='lit', held_dark=True), 'msg51', 'S-13c.019'),
    # S-13c.017 when TE edges stop: TE at 60 Hz for two frames, then none; frames
    # without TE start 50 ms after the previous frame's end; TE comes back at 200 ms and pacing resumes.
    'good_held_te_stops':    (dict(path='held', held_plan=TE_STOPS), None, 'S-13c.017'),
    # as good_held_te_stops, but the first frame without TE starts 30 ms after the previous frame's end:
    # past the longest TE period, short of T_TE_MISS's 40 ms floor
    'bad_held_te_miss_short': (dict(path='held', held_plan=TE_MISS_SHORT), 'te', 'S-13c.017'),
    # TE stops after two frames and the fallback waits 400 ms: held mode's first frame came within 100 ms, but
    # the menu change at 40 ms is not shown until 435 ms (S-13c.017: within 100 ms plus one frame time)
    'bad_held_te_miss_long': (dict(path='held', held_plan=TE_MISS_LONG), 'te', 'S-13c.017'),
    # no TE edge for 300 ms after held mode starts and no frame until TE comes back: the frozen screen
    'bad_held_te_frozen':    (dict(path='held', held_plan=TE_FROZEN), 'te', 'S-13c.017'),
    # S-13c.022 the power-off: a frame started after it; a DISPOFF at it, in held mode and during the start;
    # a frame in progress at it ends, and 0x51 00 00 02 follows that frame's last byte (S-13c.019), not before
    'bad_frame_after_poweroff': (dict(path='held', poweroff='frame'), 'poweroff', 'S-13c.022'),
    'bad_dispoff_at_poweroff': (dict(path='held', poweroff='dispoff'), 'poweroff', 'S-13c.022'),
    'bad_off_in_start_dispoff': (dict(path='off', dispoff=True), 'poweroff', 'S-13c.022'),
    'good_poweroff_in_frame': (dict(path='held', poweroff='in_frame'), None, 'S-13c.019, S-13c.022'),
    'bad_poweroff_msg_in_frame': (dict(path='held', poweroff='msg_in_frame'), 'msg51', 'S-13c.019'),
    # S-05.033 Continue to the OS is a restart: after its shutdown begins no hand-over frame is sent
    'bad_restart_handover':  (dict(path='held', poweroff='handover'), 'poweroff', 'S-05.033, S-13c.022'),
    # S-05.004, S-13c.019 the early logo frame: missing (the logo only at the
    # hand-over), never lit, lit before its frame, or not the hand-over frame's image
    'bad_early_logo_missing': (dict(path='boot', early='none'), 'msg51', 'S-05.004, S-13c.019'),
    'bad_early_logo_dark':   (dict(path='boot', early='dark'), 'msg51', 'S-13c.019'),
    'bad_early_logo_lit_first': (dict(path='held', early='first'), 'msg51', 'S-13c.019'),
    'bad_early_logo_image':  (dict(path='boot', early='image'), 'msg51', 'S-05.004'),
    # a power-off during the start, before the first frame (one-color form), and after the early logo is lit
    'good_prefill_off_before_frame': (dict(path='off', early='none'), None, 'S-13c.010, S-13c.022'),
    'good_off_in_start':     (dict(path='off'), None, 'S-13c.019, S-13c.022'),
    # S-13c.010's logo form: no one-color fill; the early logo frame is sent with the display off and
    # DISPON follows its last byte (S-13c.019), on each path
    'good_logo_first_boot':  (dict(path='boot', logo_first=True), None, 'S-13c.010, S-13c.019'),
    'good_logo_first_regreet': (dict(path='boot', logo_first=True, regreet=True), None, 'S-13c.019'),
    'good_logo_first_held':  (dict(path='held', logo_first=True), None, 'S-13c.010, S-13c.019'),
    'good_logo_first_fail_then_held': (dict(path='fail', logo_first=True), None, 'S-13c.010, S-13c.013'),
    'good_logo_first_prompt': (dict(path='boot', logo_first=True, prompt='lit'), None, 'S-05.025, S-13c.019'),
    'good_logo_first_prompt_then_held': (dict(path='held', logo_first=True, prompt='lit'), None,
                                         'S-05.025, S-13c.019'),
    'good_logo_first_held_poweroff': (dict(path='held', logo_first=True, poweroff=True), None, 'S-13c.022'),
    # a power-off during the start after the first frame, before the logo is lit
    'good_logo_first_off_after_frame': (dict(path='off', logo_first=True, early='dark'), None,
                                        'S-13c.010, S-13c.022'),
    # the logo form's failures: a first frame that leaves visible column 49 unwritten; DISPON among its
    # bytes; another image before DISPON with the logo after it; a second frame before DISPON; 0x51
    # 3C FF 02 before DISPON, or before the frame; never lit; a frame after the power-off
    'bad_logo_first_window': (dict(path='boot', logo_first=True, window=hx('00 32 01 11')), 'prefill',
                              'S-13c.010'),
    'bad_logo_first_dispon_early': (dict(path='boot', logo_first='dispon_early'), 'prefill', 'S-13c.010'),
    'bad_logo_first_other_frame': (dict(path='boot', logo_first='other'), 'msg51', 'S-05.004, S-13c.019'),
    'bad_logo_first_twice':  (dict(path='boot', logo_first='twice'), 'prefill', 'S-13c.010, S-13c.019'),
    'bad_logo_first_lit_before_dispon': (dict(path='boot', logo_first='lit_before_dispon'), 'msg51',
                                         'S-13c.019'),
    'bad_logo_first_lit_first': (dict(path='held', logo_first=True, early='first'), 'msg51', 'S-13c.019'),
    'bad_logo_first_dark':   (dict(path='boot', logo_first=True, early='dark'), 'msg51', 'S-13c.019'),
    'bad_logo_first_frame_after_off': (dict(path='off', logo_first=True, early='dark', frame_after_off=True),
                                       'poweroff', 'S-13c.022'),
    # S-13c.019 a held start: no frame of its own between the early logo frame and held mode
    'bad_held_extra_frame':  (dict(path='held', extra=True), 'held_start', 'S-13c.019'),
    # S-13c.017 in recovery (the front's frames): paced by TE in the logo form, TE stopping and coming back,
    # a send stopped at 40 ms then whole frames by the CPU, a frame stopped 60 ms into a power-off; a send that
    # runs 60 ms with no power-off
    'good_front_te':         (dict(path='recovery', way='chord', logo_first=True), None, 'S-13c.017'),
    'good_front_te_stops':   (dict(path='recovery', way='chord', plan=TE_STOPS), None, 'S-13c.017'),
    'good_front_dma_stopped': (dict(path='recovery', way='chord', plan=FRONT_DMA_STOPPED), None, 'S-13c.017'),
    'good_front_poweroff_cut': (dict(path='recovery', way='chord', plan=FRONT_POWEROFF_CUT), None,
                                'S-13c.017, S-13c.022'),
    'bad_front_dma_late':    (dict(path='recovery', way='chord', plan=FRONT_DMA_LATE), 'te', 'S-13c.017'),
    # S-13c.025 the front's line under the logo, on each way into recovery; not after recovery's first frame
    'good_front_recovery_line_chord': (dict(path='recovery', way='chord'), None, 'S-13c.025, S-15.151'),
    'good_front_recovery_line_check': (dict(path='recovery', way='check'), None, 'S-13c.025, S-15.151'),
    'good_front_recovery_line_count': (dict(path='recovery', way='count'), None, 'S-13c.025, S-15.151'),
    'bad_front_line_after_menu': (dict(path='recovery', way='chord', line='after_menu'), 'lines', 'S-13c.025'),
}


def pattern_pixel(r, c):
    """The frame-format test pattern (S-13c.015): corner markers, else a value mixing row and column."""
    corners = {(0, 0): 0xF800, (0, FRAME_W - 1): 0x07E0, (FRAME_H - 1, 0): 0x001F,
               (FRAME_H - 1, FRAME_W - 1): 0xFFFF}
    if (r, c) in corners:
        return corners[(r, c)]
    return (r * 0x0133 + c * 0x2B5D + ((r ^ c) & 0xFF)) & 0xFFFF


def pattern_bytes(swap=False):
    out = bytearray()
    for r in range(FRAME_H):
        for c in range(FRAME_W):
            v = pattern_pixel(r, c)
            out += struct.pack('<H' if swap else '>H', v)
    return bytes(out)


def orientation_mark():
    """S-13c.023: the screen positions (x, y) of an F, white on black: a stroke down the left side and two
    bars to the right of it, so the mark is not symmetric under a left-right or top-bottom mirror."""
    pts = {(10, y) for y in range(10, 50)}
    pts |= {(x, 10) for x in range(10, 40)}
    pts |= {(x, 30) for x in range(10, 30)}
    return pts


def orientation_bytes(mirrored=False):
    """The frame of orientation_mark(): screen (x, y) at window row 479 - x, column index y (S-13c.023);
    mirrored puts it at row x instead, the left-right mirror."""
    on = {((x if mirrored else 479 - x), y) for x, y in orientation_mark()}
    out = bytearray()
    for r in range(480):
        for c in range(224):
            out += b'\xff\xff' if (r, c) in on else b'\x00\x00'
    return bytes(out)


def orientation_vector():
    b = orientation_bytes()
    return {
        'requirement': 'S-13c.023',
        'mark': 'screen positions (x, y), x 0..479 from the left, y 0..221 from the top: x = 10 for y = 10..49; '
                'y = 10 for x = 10..39; y = 30 for x = 10..29; white FFFF, every other pixel 0000',
        'map': 'screen (x, y) = window row 479 - x, window column index y (panel column 49 + y)',
        'window_samples': {'row 469 col 10': 'FFFF', 'row 469 col 49': 'FFFF', 'row 440 col 10': 'FFFF',
                           'row 450 col 30': 'FFFF', 'row 10 col 10': '0000'},
        'white_pixels': len(orientation_mark()),
        'sha256': hashlib.sha256(b).hexdigest(),
        'sha256_mirrored': hashlib.sha256(orientation_bytes(mirrored=True)).hexdigest(),
    }


# The OP-1F's (no digital-gamma, no straps), checked with --device 1f: vectors/index_1f.json.
VECTORS_1F = {
    'good_boot_1f':            (dict(path='boot', device='1f'), None, 'S-13c.026, S-13c.027'),
    'good_held_1f':            (dict(path='held', device='1f'), None, 'S-13c.026, S-13c.013'),
    'good_boot_logo_kept_1f':  (dict(path='boot', kept=True, device='1f'), None, 'S-13c.026, S-13c.020'),
    'bad_gamma_load_1f':       (dict(path='boot', gamma_load=True, device='1f'), 'no_gamma_bytes', 'S-13c.026'),
    'bad_b9_42_1f':            (dict(path='boot', b9_42=True, device='1f'), 'no_gamma_bytes', 'S-13c.026'),
    'bad_gamma_window_after_frame_1f': (dict(path='boot', kept='gamma', device='1f'), 'no_gamma_bytes',
                                        'S-13c.026'),
    'good_handshake_1f':       (dict(path='boot', device='1f'), None, 'S-13c.027, S-13c.006, S-13c.021'),
    'good_own_reset_1f':       (dict(path='boot', handshake=False, device='1f'), None,
                                'S-13c.027, S-13c.005, S-13c.006'),
    'bad_future_handshake_1f': (dict(path='boot', future=True, device='1f'), 'reset', 'S-13c.027'),
    'bad_strap_mark_1f':       (dict(path='boot', strap_mark=True, device='1f'), 'reset', 'S-13c.027'),
    'bad_two_pulses_1f':       (dict(path='boot', handshake=False, second_reset=True, device='1f'), 'reset',
                                'S-13c.027, S-13c.005'),
    # judged on the DMA9 values alone: a pulse with a valid handshake, none behind a future one
    'bad_pulse_with_handshake_1f': (dict(path='boot', reset_width=10 * US + 500, device='1f'), 'reset',
                                    'S-13c.027'),
    'bad_future_handshake_dma9_1f': (dict(path='boot', future=True, no_mark=True, device='1f'), 'reset',
                                     'S-13c.027'),
}


def main():
    os.makedirs(V, exist_ok=True)
    for vectors, index_name in ((VECTORS, 'index.json'), (VECTORS_1F, 'index_1f.json')):
        index = {}
        for name, (kw, rule, req) in vectors.items():
            text, expect = build(**kw)
            with open(os.path.join(V, name + '.tsv'), 'w') as f:
                f.write('# %s\n' % req)
                f.write(text)
            index[name] = {'expect': expect, 'fails_rule': rule, 'requirement': req}
            if vectors is VECTORS_1F:
                index[name]['device'] = '1f'
        with open(os.path.join(V, index_name), 'w') as f:
            json.dump(index, f, indent=1, sort_keys=True)
            f.write('\n')
    p = pattern_bytes()
    fp = {
        'requirement': 'S-13c.015, S-13c.016',
        'window': {'columns': [49, 272], 'rows': [0, 479]},
        'pixel': 'value(r, c) for row r in 0..479 and column index c in 0..223 of the window: '
                 '(0,0)=F800, (0,223)=07E0, (479,0)=001F, (479,223)=FFFF; '
                 'otherwise (r*0x0133 + c*0x2B5D + ((r XOR c) AND 0xFF)) AND 0xFFFF',
        'stream': 'rows 0..479 in order; within a row columns 0..223; each pixel high byte then low byte',
        'length': len(p),
        'sha256': hashlib.sha256(p).hexdigest(),
        'head_hex': p[:16].hex(),
        'tail_hex': p[-16:].hex(),
        'sha256_bytes_swapped': hashlib.sha256(pattern_bytes(swap=True)).hexdigest(),
    }
    with open(os.path.join(V, 'frame_pattern.json'), 'w') as f:
        json.dump(fp, f, indent=1)
        f.write('\n')
    ov = orientation_vector()
    with open(os.path.join(V, 'orientation.json'), 'w') as f:
        json.dump(ov, f, indent=1)
        f.write('\n')
    print('%d traces, frame pattern %s' % (len(VECTORS) + len(VECTORS_1F), fp['sha256'][:16]))


if __name__ == '__main__':
    main()
