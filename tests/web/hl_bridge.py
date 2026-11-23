#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The host link's own code (src/hostlink.c) over volumes in memory (tests/tools/hl_sim.py) on this
process's standard input and output, as raw bytes: what a host sends in, the unit's frames out. For
tests/web/test_hostlink.mjs.

    hl_bridge.py [--answer N] [--user] [--off] [--seed PATH=TEXT]...

--answer is the approval's answer (2 approved, 3 refused, 0 none: left pending), --user turns the user
drive on, --off the host link off, --seed puts a file on the drive. It prints nothing until the first
request; without a C compiler it exits 3 at once. Python 3, standard library only.
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'tools'))
import hl_sim  # noqa: E402


def main(argv):
    answer, user, on, seeds = 2, False, True, []
    i = 1
    while i < len(argv):
        if argv[i] == '--answer':
            answer, i = int(argv[i + 1]), i + 1
        elif argv[i] == '--user':
            user = True
        elif argv[i] == '--off':
            on = False
        elif argv[i] == '--seed':
            seeds.append(argv[i + 1].split('=', 1))
            i += 1
        i += 1
    with tempfile.TemporaryDirectory() as d:
        try:
            lib = hl_sim.build(d)
        except Exception:   # noqa: BLE001 - no compiler that builds it
            lib = None
        if lib is None:
            return 3
        unit = hl_sim.Unit(lib, on=on, user=user, answer=answer or None)
        for path, data in seeds:
            unit.put_file(path, data.encode())
        lib.sim_os(1, b'\x03\x00\x00\x00' + bytes(508) + b'OS one', 518)
        out = sys.stdout.buffer
        while True:
            data = os.read(0, 4096)
            if not data:
                return 0
            reply = unit.feed(data)
            if reply:
                out.write(reply)
                out.flush()


if __name__ == '__main__':
    sys.exit(main(sys.argv))
