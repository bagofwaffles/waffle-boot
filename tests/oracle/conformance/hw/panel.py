# SPDX-License-Identifier: GPL-2.0-or-later
"""The panel-bus trace format and its checker (ST7796-class 8080 parallel panel on a BF70x board).

One event per line, tab- or space-separated, `time_ns event args...`, in time order; `#` starts a comment.
Events:

    RESET 0|1          the panel RESET pin level          CS 0|1     the chip-select level
    CMD XX             a byte latched with D/C low        DATA XX..  parameter bytes (D/C high)
    RUN N XX..         N repeats of the bytes XX..        READ XX    a read strobe and the byte returned
    TE 1               a tearing-effect rising edge       MSG51 XX XX XX  PSoC message 0x51 and its payload
    GREETING           a PSoC greeting after the hello    MARK name  a named instant (bootloader, strap,
                                                                     held, fatal, prompt, poweroff)
    CHANGE             a change of what held mode shows (a menu change, a key's effect)
    HANDOVER now_ms=N record=HEX                          the kernel call, with the persistent record
An event name ending in `!` was latched with CS high or RESET low. `end=T` as the last argument gives the
time of the last byte of a DATA or RUN event.

`Trace` reduces the lines to transactions (command, parameter bytes, read bytes, times) and side events.
`Checker` applies the S-13c rules; `load_tables(dir)` reads `init_registers.tsv` and `timing.tsv` (under
tests/oracle/S-13c-display/). Command numbers and the GRAM geometry are the panel datasheet's (ST7796S:
CASET 2Ah, RASET 2Bh, RAMWR 2Ch, CSCON F0h, ID reads 04h and D3h).

    python3 -I panel.py TRACE.tsv --tables DIR --expect boot [--frame-sha256 HEX] [--no-te]

Exit status 0 when every rule passes. Python 3, standard library only.
"""
import argparse
import hashlib
import math
import os
import sys

COLS, ROWS = 320, 480                     # ST7796 GRAM
VIS_X0, VIS_X1, VIS_Y0, VIS_Y1 = 49, 270, 0, 479   # the board's visible window (S-13c)
SET2 = {0xB4, 0xB6, 0xB7, 0xB9, 0xC0, 0xC1, 0xC2, 0xC5, 0xE0, 0xE1, 0xE2, 0xE3, 0xE8}
READ_CMDS = {0x04, 0xD3}
SLPOUT, DISPOFF, DISPON, RAMWR, CASET, RASET, CSCON = 0x11, 0x28, 0x29, 0x2C, 0x2A, 0x2B, 0xF0
STRAP_SETTLE_NS = 10000       # S-13b.002: RESET fall after MARK strap, at most
TE_MISS_MAX_NS = 100000000    # S-13c.017: T_TE_MISS upper bound (lower bound: timing.tsv te_miss_min)


def load_tsv(path):
    """Rows of a tab-separated table with a header line; `#` lines and blank lines are skipped."""
    rows = []
    with open(path) as f:
        header = None
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


def hexbytes(s):
    return bytes.fromhex(s.replace(' ', '')) if s else b''


def load_tables(directory):
    """(registers, timing) from init_registers.tsv and timing.tsv in `directory`."""
    registers = {int(r['cmd'], 16): r for r in load_tsv(os.path.join(directory, 'init_registers.tsv'))}
    timing = {r['name']: int(r['min']) for r in load_tsv(os.path.join(directory, 'timing.tsv'))}
    return registers, timing


class Txn:
    __slots__ = ('t', 't_end', 't_data', 'cmd', 'params', 'reads', 'index')

    def __init__(self, t, cmd, index):
        self.t = self.t_end = t
        self.t_data = None
        self.cmd = cmd
        self.params = bytearray()
        self.reads = []
        self.index = index


class Trace:
    """The trace reduced to transactions plus side events."""

    def __init__(self, lines):
        self.txns = []
        self.pulses = []          # (t_fall, t_rise)
        self.te = []
        self.msg51 = []           # (t, payload bytes)
        self.marks = {}           # name -> t (first)
        self.mark_txn = {}        # name -> the number of transactions begun before the mark (first)
        self.greetings = []       # t of each PSoC greeting after the start-up hello
        self.changes = []         # t of each CHANGE
        self.handover = None      # (t, now_ms, record bytes)
        self.bus_errors = []
        self.read_errors = []
        reset, cs = 1, 1
        fall = None
        cur = None
        for n, raw in enumerate(lines, 1):
            line = raw.split('#', 1)[0].strip()
            if not line:
                continue
            f = line.split()
            t, ev, args = int(f[0]), f[1], f[2:]
            end = t
            if args and args[-1].startswith('end='):
                end = int(args[-1][4:])
                args = args[:-1]
            if ev.endswith('!'):
                self.bus_errors.append('line %d: %s latched with CS high or RESET low' % (n, ev[:-1]))
                ev = ev[:-1]
            if ev == 'RESET':
                lvl = int(args[0])
                if lvl == 0 and reset == 1:
                    fall = t
                elif lvl == 1 and reset == 0 and fall is not None:
                    self.pulses.append((fall, t))
                reset = lvl
                cur = None
            elif ev == 'CS':
                cs = int(args[0])
                if cs == 1:
                    cur = None
            elif ev == 'CMD':
                if cs or not reset:
                    self.bus_errors.append('line %d: command byte with CS high or RESET low' % n)
                cur = Txn(t, int(args[0], 16), len(self.txns))
                self.txns.append(cur)
            elif ev in ('DATA', 'RUN'):
                if ev == 'DATA':
                    data = hexbytes(''.join(args))
                else:
                    data = hexbytes(''.join(args[1:])) * int(args[0])
                if cs or not reset:
                    self.bus_errors.append('line %d: parameter bytes with CS high or RESET low' % n)
                if cur is None:
                    self.bus_errors.append('line %d: parameter bytes without a command' % n)
                    continue
                if cur.reads:
                    self.read_errors.append('line %d: write strobe inside a read of %02X' % (n, cur.cmd))
                if cur.t_data is None:
                    cur.t_data = t
                cur.params += data
                cur.t_end = end
            elif ev == 'READ':
                if cs or cur is None:
                    self.read_errors.append('line %d: read strobe outside a CS-framed read' % n)
                    continue
                if cur.params:
                    self.read_errors.append('line %d: read strobe after parameter bytes' % n)
                cur.reads.append(int(args[0], 16))
                cur.t_end = t
            elif ev == 'TE':
                self.te.append(t)
            elif ev == 'MSG51':
                self.msg51.append((t, hexbytes(''.join(args))))
            elif ev == 'GREETING':
                self.greetings.append(t)
            elif ev == 'MARK':
                self.marks.setdefault(args[0], t)
                self.mark_txn.setdefault(args[0], len(self.txns))
            elif ev == 'CHANGE':
                self.changes.append(t)
            elif ev == 'HANDOVER':
                kv = dict(a.split('=', 1) for a in args)
                self.handover = (t, int(kv['now_ms']), hexbytes(kv['record']))
            else:
                self.bus_errors.append('line %d: unknown event %s' % (n, ev))


class Checker:
    def __init__(self, trace, expect, registers, timing, frame_sha=None, no_te=False):
        """registers: {cmd: row of the init-register table}; timing: {name: minimum ns} (load_tables)."""
        self.registers = registers
        self.timing = timing
        self.no_te = no_te
        self.tr = trace
        self.expect = set(expect)
        self.frame_sha = frame_sha
        self.fail = []
        self.frames = []          # (txn, window): the early logo frame if before DISPON, then each RAMWR after
        self.logo_before = None   # the early logo frame sent before DISPON, or None
        self.notes = []

    def err(self, rule, req, msg):
        self.fail.append((rule, req, msg))

    # ---- helpers ----
    def boot_t(self):
        return self.tr.marks.get('bootloader', 0)

    def bl_txns(self):
        b = self.boot_t()
        return [x for x in self.tr.txns if x.t >= b]

    def first(self, cmd, after=None):
        for x in self.tr.txns:
            if x.cmd == cmd and (after is None or x.t >= after):
                return x
        return None

    # ---- rules ----
    def rule_bus(self):
        for m in self.tr.bus_errors:
            self.err('bus', 'S-13c.002', m)

    def strap_pulse(self):
        """The RESET low period of the strap interval (MARK strap), or None."""
        s = self.tr.marks.get('strap')
        if s is None:
            return None
        for p in self.tr.pulses:
            if s <= p[0] <= s + STRAP_SETTLE_NS:
                return p
        return None

    def rule_reset(self):
        b = self.boot_t()
        for fall, rise in self.tr.pulses:
            if rise - fall < self.timing['reset_low']:
                self.err('reset', 'S-13c.005', 'reset pulse %d ns < %d ns' % (rise - fall, self.timing['reset_low']))
        strap = self.strap_pulse()
        mine = [p for p in self.tr.pulses if p[0] >= b and p != strap]
        if 'bootloader' not in self.tr.marks:
            self.notes.append('reset: pulse count not evaluated (no MARK bootloader)')
        else:
            if len(mine) > 1:
                self.err('reset', 'S-13c.005', '%d reset pulses driven by the bootloader' % len(mine))
            first = self.bl_txns()
            if first and any(p[0] >= first[0].t for p in self.tr.pulses):
                self.err('reset', 'S-13c.005', 'reset pulse after the first panel transaction')
        if not self.tr.pulses and self.tr.txns:
            self.err('reset', 'S-13c.004', 'panel commands without any reset pulse in the trace')

    def rule_reset_wait(self):
        if not self.tr.pulses:
            return
        rise = self.tr.pulses[-1][1]
        after = [x for x in self.tr.txns if x.t >= rise]
        if after and after[0].t - rise < self.timing['reset_to_first_transaction']:
            self.err('reset_wait', 'S-13c.006', 'first transaction %d ns after reset release' % (after[0].t - rise))

    def rule_slpout(self):
        txns = [x for x in self.bl_txns() if x.cmd not in READ_CMDS]
        if not txns:
            return
        if txns[0].cmd != SLPOUT:
            self.err('slpout', 'S-13c.007', 'first command is %02X, not SLPOUT' % txns[0].cmd)
            return
        nxt = [x for x in self.tr.txns if x.t > txns[0].t]
        if nxt and nxt[0].t - txns[0].t_end < self.timing['slpout_to_next_command']:
            self.err('slpout', 'S-13c.007', 'next command %d ns after SLPOUT' % (nxt[0].t - txns[0].t_end))

    def rule_registers(self):
        slp = self.first(SLPOUT, self.boot_t())
        don = self.first(DISPON, self.boot_t())
        if slp is None or don is None:
            if 'boot' in self.expect or 'held' in self.expect or 'fail' in self.expect:
                self.err('registers', 'S-13c.008', 'SLPOUT or DISPON missing')
            return
        seen = {}
        for x in self.tr.txns:
            if x.cmd in self.registers:
                r = self.registers[x.cmd]
                ok = [hexbytes(r['params'])] + ([hexbytes(r['alt'])] if r['alt'] else [])
                if bytes(x.params) not in ok:
                    self.err('registers', 'S-13c.008', '%02X %s: expected %s' % (x.cmd, x.params.hex(' '), r['params']))
                if slp.t < x.t < don.t:
                    seen[x.cmd] = True
        for c in self.registers:
            if c not in seen:
                self.err('registers', 'S-13c.008', '%02X not written between SLPOUT and DISPON' % c)

    def rule_cscon(self):
        unlocked = False
        last_f0 = None
        for x in self.tr.txns:
            if x.cmd == CSCON:
                p = bytes(x.params)
                if p == b'\x96' and last_f0 == b'\xc3':
                    unlocked = True
                elif p == b'\x69' and last_f0 == b'\x3c':
                    unlocked = False
                last_f0 = p
                continue
            if x.cmd in SET2 and not unlocked:
                self.err('cscon', 'S-13c.009', '%02X outside a command-set-2 window' % x.cmd)
            if x.cmd in (DISPON, RAMWR) and unlocked:
                self.err('cscon', 'S-13c.009', '%02X while a command-set-2 window is open' % x.cmd)
        if self.tr.handover and unlocked:
            self.err('cscon', 'S-13c.009', 'window open at the hand-over')

    def gram_walk(self):
        """Apply every RAMWR to a GRAM model, check the visible window at the first DISPON (S-13c.010) and
        collect the frames. A one-color window at DISPON means the RAMWRs before it are a fill, not frames;
        otherwise the single RAMWR before DISPON is the early logo frame and the first frame."""
        gram = [bytearray(COLS * 2) for _ in range(ROWS)]
        wr = [bytearray(COLS) for _ in range(ROWS)]
        xs, xe, ys, ye = 0, COLS - 1, 0, ROWS - 1
        dispon_seen = False
        before = []               # (txn, window) of each RAMWR before the first DISPON
        for x in self.tr.txns:
            if x.cmd == CASET and len(x.params) == 4:
                xs, xe = x.params[0] << 8 | x.params[1], x.params[2] << 8 | x.params[3]
            elif x.cmd == RASET and len(x.params) == 4:
                ys, ye = x.params[0] << 8 | x.params[1], x.params[2] << 8 | x.params[3]
            elif x.cmd == RAMWR:
                w = xe - xs + 1
                (self.frames if dispon_seen else before).append((x, (xs, xe, ys, ye)))
                data = bytes(x.params)
                pos, y = 0, ys
                while pos + 1 < len(data) and y <= ye and y < ROWS:
                    n = min(w, (len(data) - pos) // 2)
                    if n <= 0:
                        break
                    gram[y][xs * 2:(xs + n) * 2] = data[pos:pos + n * 2]
                    wr[y][xs:xs + n] = b'\x01' * n
                    pos += n * 2
                    y += 1
            elif x.cmd == DISPON and not dispon_seen:
                dispon_seen = True
                self.visible_at_dispon(gram, wr, before, x)
        return dispon_seen

    def visible_at_dispon(self, gram, wr, before, don):
        """S-13c.010, S-13c.019 at the first DISPON."""
        vals = set()
        for yy in range(VIS_Y0, VIS_Y1 + 1):
            if wr[yy][VIS_X0:VIS_X1 + 1].count(1) != VIS_X1 - VIS_X0 + 1:
                self.err('prefill', 'S-13c.010', 'visible window not fully written at DISPON (row %d)' % yy)
                return
            row = gram[yy][VIS_X0 * 2:(VIS_X1 + 1) * 2]
            vals.update(bytes(row[i:i + 2]) for i in range(0, len(row), 2))
        if len(vals) == 1:
            return
        if len(before) != 1:
            self.err('prefill', 'S-13c.010', 'visible window not one color at DISPON, and %d RAMWRs before it, '
                     'not the early logo frame alone' % len(before))
            return
        x = before[0][0]
        if x.t_end > don.t:
            self.err('prefill', 'S-13c.010', 'DISPON before the early logo frame\'s last byte')
        self.logo_before = x
        self.frames.append(before[0])

    def rule_frames(self):
        for x, (xs, xe, ys, ye) in self.frames:
            if not (xs <= VIS_X0 and xe >= VIS_X1 and ys == 0 and ye == ROWS - 1):
                self.err('frames', 'S-13c.014', 'frame window %d-%d x %d-%d' % (xs, xe, ys, ye))
            need = 2 * (xe - xs + 1) * (ye - ys + 1)
            if len(x.params) != need or x.reads:
                self.err('frames', 'S-13c.016', 'frame of %d bytes, expected %d' % (len(x.params), need))
        if self.frame_sha and self.frames:
            got = hashlib.sha256(bytes(self.frames[-1][0].params)).hexdigest()
            if got != self.frame_sha:
                self.err('frames', 'S-13c.015', 'last frame sha256 %s != expected' % got[:16])

    def rule_byte_rate(self):
        """Mean write-cycle time of each multi-byte DATA/RUN span with end= (S-13c.003, S-13c.018)."""
        frames = {id(x) for x, _ in self.frames}
        for x in self.tr.txns:
            n = len(x.params)
            if n < 2 or x.t_data is None or x.t_end == x.t_data:
                continue
            frame = id(x) in frames
            lim = self.timing['frame_byte_period' if frame else 'wr_cycle']
            if x.t_end - x.t_data < (n - 1) * lim:
                self.err('byte_rate', 'S-13c.018' if frame else 'S-13c.003',
                         '%02X at %d ns: %d bytes in %d ns, under %d ns per byte'
                         % (x.cmd, x.t, n, x.t_end - x.t_data, lim))

    def rule_reads(self):
        for m in self.tr.read_errors:
            self.err('reads', 'S-13c.011', m)
        for x in self.tr.txns:
            if x.reads and x.cmd not in READ_CMDS:
                self.err('reads', 'S-13c.011', 'read strobes after %02X' % x.cmd)

    def rule_gamma(self):
        window = []
        loaded_at = None
        for x in self.tr.txns:
            if x.cmd == CSCON and bytes(x.params) == b'\x96':
                window = []
            if x.cmd in (0xE2, 0xE3):
                if len(x.params) != 64:
                    self.err('gamma', 'S-13c.012', '%02X with %d parameter bytes' % (x.cmd, len(x.params)))
                window.append(x.cmd)
            if x.cmd == 0xB9 and bytes(x.params) == b'\x42\xe0':
                if not (0xE2 in window and 0xE3 in window):
                    self.err('gamma', 'S-13c.012', 'B9 42 E0 without both tables in its window')
                loaded_at = x
        if 'boot' in self.expect:
            last_b9 = [x for x in self.tr.txns if x.cmd == 0xB9]
            if not last_b9 or bytes(last_b9[-1].params) != b'\x42\xe0':
                self.err('gamma', 'S-13c.012', 'digital gamma not on at the hand-over')
            elif self.frames and loaded_at.t > self.frames[-1][0].t:
                self.err('gamma', 'S-13c.012', 'gamma load after the hand-over frame started')

    def rule_once(self):
        if 'bootloader' not in self.tr.marks:
            self.notes.append('once: not evaluated (no MARK bootloader)')
            return
        b = self.boot_t()
        counts = {}
        for x in self.tr.txns:
            if x.t >= b and (x.cmd == SLPOUT or (x.cmd in self.registers and x.cmd != 0xB9)):
                counts[x.cmd] = counts.get(x.cmd, 0) + 1
        for c, n in counts.items():
            if n > 1:
                self.err('once', 'S-13c.013', '%02X sent %d times in one run' % (c, n))

    def held_t(self):
        """Held mode's start: MARK held, or else the first 0x51 3C FF 02 after DISPON that is not the early
        logo's; None without one."""
        if 'held' in self.tr.marks:
            return self.tr.marks['held']
        don = self.first(DISPON, self.boot_t())
        lit = [t for t, p in self.tr.msg51 if p == bytes.fromhex('3cff02') and don is not None and t > don.t]
        early = self.early_logo()
        if early is not None and len(lit) > 1 and lit[0] > early.t_end:
            return lit[1]
        return lit[0] if lit else None

    def early_logo(self):
        """The early logo frame: the one sent before DISPON, or else the first frame after the bootloader's
        DISPON; None without one."""
        if self.logo_before is not None:
            return self.logo_before
        don = self.first(DISPON, self.boot_t())
        if don is None:
            return None
        after = [x for x, _ in self.frames if x.t > don.t]
        return after[0] if after else None

    def rule_te(self):
        """S-13c.017, S-13c.018: held-mode frame pacing against TE edges and CHANGE events. --no-te reports
        a trace without TE edges as not evaluated; the frame after MARK fatal is skipped."""
        if not ('held' in self.expect or 'fail' in self.expect):
            return
        held = self.held_t()
        frames = [x for x, _ in self.frames if held is not None and x.t >= held]
        te = self.tr.te
        if held is None or not frames or (not te and self.no_te):
            what = 'no held-mode start' if held is None else 'no held-mode frames' if not frames else 'no TE edges'
            if self.no_te:
                self.notes.append('te: not evaluated (%s; --no-te)' % what)
            else:
                self.err('te', 'S-13c.017', 'held mode not checkable: %s' % what)
            return
        miss_min = self.timing['te_miss_min']
        if not [t for t in te if held <= t <= held + TE_MISS_MAX_NS] and frames[0].t > held + TE_MISS_MAX_NS:
            self.err('te', 'S-13c.017', 'no TE edge within %d ns of held mode\'s start, and no frame until %d ns'
                     % (TE_MISS_MAX_NS, frames[0].t))
        prev_end = held
        fatal = self.tr.marks.get('fatal')
        for x in frames:
            if fatal is not None and x.t >= fatal:
                fatal = None              # the fatal-error frame: not paced
                prev_end = x.t_end
                continue
            edges = [t for t in te if prev_end <= t <= x.t]
            if edges:
                later = [t for t in te if t > x.t]
                if later and x.t_end >= later[0]:
                    self.err('te', 'S-13c.018', 'frame at %d ns ends after the next TE edge' % x.t)
            else:
                ref = max([prev_end] + [t for t in te if t < x.t][-1:])
                if x.t - ref < miss_min:
                    self.err('te', 'S-13c.017', 'frame at %d ns has no TE edge since the last frame and starts '
                             '%d ns after %d ns (T_TE_MISS is at least %d ns)' % (x.t, x.t - ref, ref, miss_min))
            prev_end = x.t_end
        # CHANGE without TE edges: a frame within TE_MISS_MAX_NS plus the longest frame time
        frame_ns = max(x.t_end - x.t for x in frames)
        for c in self.tr.changes:
            if c < held:
                continue
            lim = c + TE_MISS_MAX_NS + frame_ns
            if [t for t in te if c <= t <= lim]:
                continue                  # TE pacing applies
            if not [x for x in frames if c < x.t <= lim]:
                self.err('te', 'S-13c.017', 'change at %d ns with no TE edge and no frame start within %d ns '
                         '(T_TE_MISS at most %d ns plus one frame time)' % (c, lim - c, TE_MISS_MAX_NS))

    def rule_msg51(self):
        b = self.boot_t()
        m = [(t, p) for t, p in self.tr.msg51 if t >= b]
        cmds = self.bl_txns()
        don = self.first(DISPON, b)
        if cmds and ('boot' in self.expect or 'held' in self.expect or 'fail' in self.expect):
            first = [t for t, p in m if p == b'\0\0\0']
            if not first:
                self.err('msg51', 'S-13c.019', 'no 0x51 00 00 00 at start-up')
            elif any(p != b'\0\0\0' and t < first[0] for t, p in m):
                self.err('msg51', 'S-13c.019', '0x51 00 00 00 after another 0x51')
        greet = sorted(self.tr.greetings)
        for t, p in m:
            if p == bytes.fromhex('3cff02') and greet and greet[0] <= t:
                greet.pop(0)              # the answer to a PSoC greeting
                continue
            if p != b'\0\0\0' and (don is None or t < don.t):
                self.err('msg51', 'S-13c.019', '0x51 %s before DISPON' % p.hex(' '))
        if 'boot' in self.expect and self.frames:
            last = self.frames[-1][0]
            ho = self.tr.handover[0] if self.tr.handover else None
            if not any(p == bytes.fromhex('3cff32') and t > last.t_end and (ho is None or t < ho) for t, p in m):
                self.err('msg51', 'S-13c.019', 'no 0x51 3C FF 32 between the hand-over frame and the kernel call')
        if 'held' in self.expect or 'fail' in self.expect:
            if not any(p == bytes.fromhex('3cff02') and don is not None and t > don.t for t, p in m):
                self.err('msg51', 'S-13c.019', 'no 0x51 3C FF 02 after DISPON on entering held mode')
        self.check_early_logo(m, don)
        self.check_prompt(don)
        if 'poweroff' in self.expect:
            # 00 00 02 from MARK poweroff on (when present), outside any frame
            at = self.tr.marks.get('poweroff')
            dark = [t for t, p in m if p == bytes.fromhex('000002') and (at is None or t >= at)]
            if not dark:
                self.err('msg51', 'S-13c.019', 'no 0x51 00 00 02 at power-off' if at is None else
                         'no 0x51 00 00 02 after the power-off (MARK poweroff at %d ns)' % at)
            elif any(x.t <= dark[0] < x.t_end for x, _ in self.frames):
                self.err('msg51', 'S-13c.019', '0x51 00 00 02 at %d ns, before the last frame has ended'
                         % dark[0])

    def check_prompt(self, don):
        """S-05.025, S-13c.019: the frame after MARK prompt and the 0x51 messages around it. Not evaluated
        without MARK prompt."""
        mark = self.tr.marks.get('prompt')
        if mark is None:
            return
        shown = [x for x, _ in self.frames if x.t >= mark]
        if not shown:
            self.err('msg51', 'S-13c.019', 'no frame after MARK prompt (the boot prompt)')
            return
        first = shown[0]
        if not any(p == bytes.fromhex('3cff02') and don is not None and don.t < t < first.t
                   for t, p in self.tr.msg51):
            self.err('msg51', 'S-13c.019', 'no 0x51 3C FF 02 between DISPON and the boot prompt\'s frame at %d ns'
                     % first.t)
        if any(p == bytes.fromhex('3cff32') and t < first.t_end for t, p in self.tr.msg51):
            self.err('msg51', 'S-13c.019', '0x51 3C FF 32 before the end of the boot prompt\'s frame')
        if 'boot' in self.expect and self.frames[-1][0] is first:
            self.err('msg51', 'S-13c.019', 'the boot prompt is the last frame: no hand-over frame follows it')
        if ('held' in self.expect or 'fail' in self.expect) and not any(
                p == bytes.fromhex('3cff02') and t > first.t_end for t, p in self.tr.msg51):
            self.err('msg51', 'S-13c.019', 'no 0x51 3C FF 02 for held mode after the boot prompt\'s frame')

    def check_early_logo(self, m, don):
        """S-05.004, S-13c.019: the early logo frame (early_logo()) and the 0x51 messages around it."""
        if not ('boot' in self.expect or 'held' in self.expect or 'fail' in self.expect):
            return
        early = self.early_logo()
        if early is None:
            self.err('msg51', 'S-13c.019', 'no early logo frame: no frame before DISPON as S-13c.010\'s, none '
                     'after it')
            return
        later = [x for x, _ in self.frames if x.t > early.t]
        nxt = later[0].t if later else None
        lit = max(early.t_end, don.t if don is not None else early.t_end)
        if not any(p == bytes.fromhex('3cff02') and t > lit and (nxt is None or t < nxt) for t, p in m):
            self.err('msg51', 'S-13c.019', 'no 0x51 3C FF 02 between the early logo frame\'s end (and DISPON) '
                     'and the next frame')
        greet = sorted(self.tr.greetings)
        for t, p in m:
            if p == bytes.fromhex('3cff02') and greet and greet[0] <= t:
                greet.pop(0)
                continue
            if p != b'\0\0\0' and t <= early.t_end:
                self.err('msg51', 'S-13c.019', '0x51 %s before the early logo frame ended' % p.hex(' '))
        if 'boot' in self.expect and later and self.frames[-1][0].params != early.params:
            self.err('msg51', 'S-05.004', 'the early logo frame is not the hand-over frame\'s image')

    def poweroff_t(self):
        """The power-off: MARK poweroff, or else the first 0x51 00 00 02 (an upper bound); None without
        either."""
        if 'poweroff' in self.tr.marks:
            return self.tr.marks['poweroff']
        b = self.boot_t()
        return next((t for t, p in self.tr.msg51 if t >= b and p == bytes.fromhex('000002')), None)

    def rule_held_start(self):
        """S-13c.019 for --expect held without boot or fail: frames between the early logo frame and held
        mode's start or MARK prompt. Without MARK bootloader such a frame gives a note, not a failure."""
        if 'held' not in self.expect or self.expect & {'boot', 'fail'}:
            return
        early = self.early_logo()
        if early is None:
            return                        # rule msg51
        ends = [t for t in (self.tr.marks.get('prompt'), self.held_t()) if t is not None]
        end = min(ends) if ends else None
        extra = [x for x, _ in self.frames if x.t > early.t and (end is None or x.t < end)]
        if not extra:
            return
        if 'bootloader' not in self.tr.marks:
            self.notes.append('held_start: not evaluated (no MARK bootloader: the frame at %d ns before held mode '
                              'may be a boot prompt)' % extra[0].t)
            return
        self.err('held_start', 'S-13c.019', 'the frame at %d ns comes between the early logo frame and held '
                 'mode\'s start' % extra[0].t)

    def rule_handover(self):
        if 'boot' not in self.expect:
            return
        if self.tr.handover is None:
            self.err('handover', 'S-13c.020', 'no HANDOVER event')
            return
        t = self.tr.handover[0]
        before = [x for x in self.tr.txns if x.t < t]
        onoff = [x.cmd for x in before if x.cmd in (DISPON, DISPOFF)]
        if not onoff or onoff[-1] != DISPON:
            self.err('handover', 'S-13c.020', 'display not on at the hand-over')
        if not self.frames:
            self.err('handover', 'S-13c.020', 'no frame before the hand-over')
            return
        last = self.frames[-1][0]
        if before and before[-1] is not last:
            self.err('handover', 'S-13c.020', 'transaction %02X after the hand-over frame' % before[-1].cmd)
        if last.t_end > t:
            self.err('handover', 'S-13c.020', 'hand-over frame still in progress at the kernel call')
        # S-15.132: --frame-sha256 checks the frame's content (rule frames)

    def rule_record(self):
        if 'boot' not in self.expect or self.tr.handover is None:
            return
        _, now, rec = self.tr.handover
        if len(rec) != 44:
            self.err('record', 'S-13c.021', 'record is %d bytes' % len(rec))
            return
        le = lambda o: int.from_bytes(rec[o:o + 4], 'little')
        if rec[0] != 1 or rec[0x28] != 1 or le(8) != 2:
            self.err('record', 'S-13c.021', 'flags/version %d %d %d' % (rec[0], rec[0x28], le(8)))
        if any(rec[1:4]) or any(rec[0x0C:0x28]) or any(rec[0x29:0x2C]):
            self.err('record', 'S-13c.021', 'reserved bytes not zero')
        rt = le(4)
        if not (1 <= rt <= now - 120):
            self.err('record', 'S-13c.021', 'reset time %d not in 1..%d' % (rt, now - 120))
        t_ho = self.tr.handover[0]
        rises = [r for _, r in self.tr.pulses if r <= t_ho]
        if not rises:
            self.notes.append('record: reset release not in the trace; reset time not tied to it')
            return
        # ms time of the last release, on the call's clock: now_ms - (t_handover - t_release)
        rel_ms = now - (t_ho - rises[-1]) / 1e6
        if rt < math.floor(rel_ms) - 1:   # 1 ms for the truncation of both ms times
            self.err('record', 'S-13c.021', 'reset time %d ms earlier than the last reset release (%.3f ms)'
                     % (rt, rel_ms))

    def rule_poweroff(self):
        """S-13c.022: panel transactions after MARK poweroff, or, without it, after the 0x51 00 00 02 (with a
        note for what is not checked)."""
        if 'poweroff' not in self.expect:
            return
        b = self.boot_t()
        at = self.tr.marks.get('poweroff')
        if at is not None:
            after = [x for x in self.tr.txns if x.index >= self.tr.mark_txn['poweroff']]
            where = 'the power-off'
        else:
            dark = self.poweroff_t()
            if dark is None:
                self.err('poweroff', 'S-13c.022', 'no power-off in the trace: no MARK poweroff and no '
                         '0x51 00 00 02')
                return
            after = [x for x in self.tr.txns if x.t > dark]
            where = 'the power-off\'s 0x51 00 00 02'
            don = self.first(DISPON, b)
            off = [x for x in self.tr.txns if x.cmd == DISPOFF and don is not None and don.t < x.t <= dark]
            self.notes.append('poweroff: no MARK poweroff: frames and commands between the power-off and its '
                              '0x51 00 00 02 at %d ns not evaluated%s'
                              % (dark, '; the DISPOFF at %d ns cannot be placed before or after the power-off'
                                 % off[-1].t if off else ''))
        for x in after:
            if x.cmd == RAMWR:
                self.err('poweroff', 'S-13c.022', 'frame at %d ns, after %s' % (x.t, where))
            elif x.cmd == DISPOFF:
                self.err('poweroff', 'S-13c.022', 'DISPOFF at %d ns, after %s: the panel is sent no command '
                         'there' % (x.t, where))
            else:
                self.err('poweroff', 'S-13c.022', 'command %02X at %d ns, after %s' % (x.cmd, x.t, where))

    def run(self):
        self.rule_bus()
        self.rule_reset()
        self.rule_reset_wait()
        self.rule_slpout()
        self.rule_registers()
        self.rule_cscon()
        self.gram_walk()
        self.rule_frames()
        self.rule_byte_rate()
        self.rule_reads()
        self.rule_gamma()
        self.rule_once()
        self.rule_te()
        self.rule_msg51()
        self.rule_held_start()
        self.rule_handover()
        self.rule_record()
        self.rule_poweroff()
        return self.fail


def check(lines, expect, registers, timing, frame_sha=None, no_te=False):
    c = Checker(Trace(lines), expect, registers, timing, frame_sha, no_te)
    return c.run(), c


def report(trace_path, expect, registers, timing, frame_sha=None, no_te=False):
    """The command-line run: print notes, failures and a summary line; return the exit status."""
    with open(trace_path) as f:
        fails, c = check(f.readlines(), expect, registers, timing, frame_sha, no_te)
    for n in c.notes:
        print('note:', n)
    for rule, req, msg in fails:
        print('FAIL %-10s %-10s %s' % (rule, req, msg))
    print('%d frames; %s' % (len(c.frames), 'PASS' if not fails else '%d failures' % len(fails)))
    return 1 if fails else 0


def arg_parser(doc):
    ap = argparse.ArgumentParser(description=doc, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace')
    ap.add_argument('--expect', default='boot')
    ap.add_argument('--frame-sha256')
    ap.add_argument('--no-te', action='store_true',
                    help='the emulator has no TE or frame-path model: report held-mode pacing as not evaluated')
    return ap


def main():
    ap = arg_parser(__doc__)
    ap.add_argument('--tables', required=True, help='directory holding init_registers.tsv and timing.tsv')
    a = ap.parse_args()
    registers, timing = load_tables(a.tables)
    sys.exit(report(a.trace, a.expect.split(','), registers, timing, a.frame_sha256, a.no_te))


if __name__ == '__main__':
    main()
