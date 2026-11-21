#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-13c panel-bus trace checker.

Checks a panel-bus trace against the S-13c rules, using init_registers.tsv and timing.tsv beside this file.
Each rule names its requirement.

    python3 check_trace.py TRACE.tsv --expect boot
    python3 check_trace.py TRACE.tsv --expect held,poweroff
    python3 check_trace.py TRACE.tsv --expect fail --frame-sha256 HEX

--expect lists the paths the trace covers:
    boot      the boot path up to the kernel call (HANDOVER event required)
    held      held mode entered without a boot attempt
    fail      the boot path's preparation, the load failing, then held mode (no HANDOVER)
    recovery  the front's recovery (S-02.084): its line under the logo, then recovery's frames from MARK
              recovery, paced as held mode's, each sent by one memory DMA until a send is stopped (S-13c.017)
    poweroff  held mode ends with the power-off: a shutdown, a power-off the PSoC reports or a restart
              (MARK poweroff; nothing reaches the panel after it, S-13c.022); given alone, the start ends with
              it, before held mode or the boot
--frame-sha256 checks the sha256 of the last frame's parameter bytes (on the boot path, the hand-over
frame: S-13c.015, S-13c.020).
--no-te reports held-mode frame pacing as not evaluated instead of failing when the trace has no TE edges
or no held-mode frames (a trace source without TE or frame-path models).
--device 1f checks a device without `digital-gamma` and `straps` (S-18.020, the OP-1F): rule
`no_gamma_bytes` (S-13c.026) in place of `gamma`: no E2, no E3, every B9 is 02 E0, no command-set-2 window
after the initialization's, and after a kept logo frame only DISPON; and S-13c.027 for the reset: no
`MARK strap`; with a `DMA9` event (the values the bootloader's reset decision read) no pulse of its own when
they are a valid handshake (XMOD 0x12345678, YMOD, 0 read as 1, less than the ms time of the event) and
exactly one when they are not; with `MARK no_handshake` (the bootloader found no valid handshake) exactly one
pulse of its own, and the mark agrees with the DMA9 values where both are given. The vectors of that device
are listed in vectors/index_1f.json.

Exit status 0 when every rule passes. Python 3, standard library only.
The trace reader and the checker are the conformance package's `hw.panel`; this file binds them to the
S-13c tables.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'conformance'))
from hw import panel as _P  # noqa: E402
from hw.panel import (COLS, ROWS, VIS_X0, VIS_X1, VIS_Y0, VIS_Y1, SET2, READ_CMDS,  # noqa: E402,F401
                      SLPOUT, DISPOFF, DISPON, RAMWR, CASET, RASET, CSCON, STRAP_SETTLE_NS,
                      TE_MISS_MAX_NS, hexbytes, Txn, Trace)


def load_tsv(name):
    return _P.load_tsv(os.path.join(HERE, name))


REGISTERS, TIMING = _P.load_tables(HERE)


class Checker(_P.Checker):
    def __init__(self, trace, expect, frame_sha=None, no_te=False):
        super().__init__(trace, expect, REGISTERS, TIMING, frame_sha, no_te)


UNLOCK, LOCK = b'\xc3', b'\x3c'
HANDSHAKE = 0x12345678        # S-01.006: DMA9_XMOD when the first stage leaves the handshake


class Checker1F(Checker):
    """S-13c on a device without digital-gamma or straps (S-13c.026, S-13c.027)."""
    GAMMA_CMDS = ()                       # after a kept logo frame only DISPON (S-13c.026)

    def strap_pulse(self):
        return None                       # PA8 is no strap: every bootloader pulse counts (S-13c.027)

    def rule_gamma(self):
        """Rule no_gamma_bytes (S-13c.026)."""
        windows = 0
        for x in self.tr.txns:
            if x.cmd in (0xE2, 0xE3):
                self.err('no_gamma_bytes', 'S-13c.026', '%02X at %d ns' % (x.cmd, x.t))
            if x.cmd == 0xB9 and bytes(x.params) != b'\x02\xe0':
                self.err('no_gamma_bytes', 'S-13c.026', 'B9 %s at %d ns' % (bytes(x.params).hex(' '), x.t))
            if x.cmd == CSCON and bytes(x.params) == UNLOCK:
                windows += 1
                if windows > 1:
                    self.err('no_gamma_bytes', 'S-13c.026',
                             'a command-set-2 window after the initialization\'s at %d ns' % x.t)
        if 'boot' in self.expect:
            b9 = [x for x in self.tr.txns if x.cmd == 0xB9]
            if not b9 or bytes(b9[-1].params) != b'\x02\xe0':
                self.err('no_gamma_bytes', 'S-13c.026', 'the last B9 at the hand-over is not 02 E0')

    def rule_reset(self):
        super().rule_reset()
        if 'strap' in self.tr.marks:
            self.err('reset', 'S-13c.027', 'MARK strap: PA8 is never an input here')
        if 'bootloader' not in self.tr.marks:
            return
        b = self.boot_t()
        mine = [p for p in self.tr.pulses if p[0] >= b]
        valid = None
        if self.tr.dma9 is not None:
            t, xmod, ymod = self.tr.dma9
            valid = xmod == HANDSHAKE and max(ymod, 1) < t // 1000000
            if valid and mine:
                self.err('reset', 'S-13c.027', '%d reset pulses of the bootloader with a valid handshake'
                         % len(mine))
            if 'no_handshake' in self.tr.marks and valid:
                self.err('reset', 'S-13c.027', 'MARK no_handshake with a valid handshake in DMA9')
        if ('no_handshake' in self.tr.marks or valid is False) and len(mine) != 1:
            self.err('reset', 'S-13c.027', '%d reset pulses of the bootloader with no valid handshake'
                     % len(mine))


DEVICES = {'xy': Checker, '1f': Checker1F}


def check(lines, expect, frame_sha=None, no_te=False, device='xy'):
    c = DEVICES[device](Trace(lines), expect, frame_sha, no_te)
    return c.run(), c


def main():
    ap = _P.arg_parser(__doc__)
    ap.add_argument('--device', default='xy', choices=sorted(DEVICES))
    a = ap.parse_args()
    with open(a.trace) as f:
        fails, c = check(f.readlines(), a.expect.split(','), a.frame_sha256, a.no_te, a.device)
    for n in c.notes:
        print('note:', n)
    for rule, req, msg in fails:
        print('FAIL %-10s %-10s %s' % (rule, req, msg))
    print('%d frames; %s' % (len(c.frames), 'PASS' if not fails else '%d failures' % len(fails)))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
