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


def check(lines, expect, frame_sha=None, no_te=False):
    c = Checker(Trace(lines), expect, frame_sha, no_te)
    return c.run(), c


def main():
    a = _P.arg_parser(__doc__).parse_args()
    sys.exit(_P.report(a.trace, a.expect.split(','), REGISTERS, TIMING, a.frame_sha256, a.no_te))


if __name__ == '__main__':
    main()
