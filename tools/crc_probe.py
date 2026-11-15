#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A probe of the ADSP-BF70x CRC peripheral CRC0 and memory DMA stream 1 from the host.

    crc_probe.py [--port /dev/cu.usbmodemNNN] [--force] [search] [speed]

It drives CRC0 in memory scan mode, fed by DMA18 (MDMA1's source channel), with the developer
host's register reads and writes (docs/devhost.md: held mode, developer mode on), in the order
src/drivers/bf70x/crc.c uses, so it needs no new build. It never touches CRC1 or MDMA2, the
display's stream.

search  the board's search (src/board/opxy_crc.c), every option set and conversion of the
        running value tried on three runs of a 1 KiB buffer against the software's CRC-16/XMODEM
        and CRC-32: prints each combination that answers all three
speed   one scan of 4 MiB, eight times, and the MiB/s (host clock: a few ms of error on each)
With no name, both.

It writes CRC0's and DMA18's registers and, for search, 1 KiB at 0x89C01000: inside the top
4 MiB of the bootloader's heap (0x89C00000-0x8A000000), which grows up from 0x8833D000 and in
held mode stays some 24 MiB short of it. speed only reads that range. Before it writes there it
samples the range and stops unless every word is still one of the two fill values DDR holds
where nothing has been written since power-on (0x55555555, 0xAAAAAAAA). --force skips that
check: use it only for a second run since power-on (the first run's own writes fail the check),
or on a unit whose untouched DDR reads otherwise, and then only in held mode entered straight
from power-on, with no install, transfer or MTP session since. It writes no flash and no eMMC.
Run it with no install in progress, and power the unit off and on afterwards.

Python 3, standard library only.
"""
import struct
import sys
import time
import zlib

import testos_mon as tm
from pkte_probe import HEAP, Unit, heap_free

CRC0 = 0x200B0000
CTL, DCNT, DCNTRLD, COMP, INEN, POLY, STAT, RESULT_FIN, RESULT_CUR = (
    0x00, 0x04, 0x08, 0x14, 0x20, 0x2C, 0x40, 0x4C, 0x50)
BLKEN, OPMODE_SCAN = 1 << 0, 3 << 4
BITMIRR, BYTMIRR, W16SWP, POLYMIRR = 1 << 16, 1 << 17, 1 << 18, 1 << 21
CMPERR, DCNTEXP, LUTDONE = 1 << 1, 1 << 4, 1 << 19

DMA18 = 0x200A0000         # MDMA1's source: CRC0's input (HRM Table 8-2)
ADDRSTART, CFG, XCNT, XMOD, DSTAT = 0x04, 0x08, 0x0C, 0x10, 0x30
EN, IRQDONE, IRQERR, PIRQ = 1 << 0, 1 << 0, 1 << 1, 1 << 2
PSIZE_4, MSIZE_SHIFT = 2 << 4, 8

TEST = HEAP + 0x1000
TEST_LEN = 1024
RUNS = ((0, TEST_LEN), (4, 516), (520, 68))
POLY_OF = {'crc-16': 0x10210000, 'crc-32': 0xEDB88320}
SEED = {'crc-16': 0x1D0F, 'crc-32': 0x9E3779B9}
REV, NOT = 1, 2


def crc16_xmodem(crc, data):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = (crc << 1 ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc


def soft(name, v, data):
    return crc16_xmodem(v, data) if name == 'crc-16' else zlib.crc32(data, v)


def rev32(x):
    return int('{:032b}'.format(x)[::-1], 2)


def convert(x, conv):
    if conv & NOT:
        x ^= 0xFFFFFFFF
    return rev32(x) if conv & REV else x


def options(s):
    return ((BITMIRR if s & 1 else 0) | (BYTMIRR if s & 2 else 0) | (W16SWP if s & 4 else 0)
            | (POLYMIRR if s & 8 else 0))


def option_names(opts):
    names = [n for n, b in (('BITMIRR', BITMIRR), ('BYTMIRR', BYTMIRR), ('W16SWP', W16SWP),
                            ('POLYMIRR', POLYMIRR)) if opts & b]
    return '|'.join(names) or 'none'


def conv_name(conv):
    return {0: 'as is', REV: 'reversed', NOT: 'complemented', REV | NOT: 'reversed, complemented'}[conv]


class Crc:
    def __init__(self, u):
        self.u = u
        self.foreign = False    # DMA18 found in another's use: nothing more is written

    def reg(self, off):
        return self.u.rd(CRC0 + off)[0]

    def put(self, off, v):
        self.u.wr(CRC0 + off, v)

    def stop(self):
        """DMA18 off before the block (HRM "CRC Programming Model"), then the status."""
        self.u.wr(DMA18 + CFG, 0)
        self.u.wr(DMA18 + DSTAT, IRQDONE | IRQERR | PIRQ)
        self.put(CTL, 0)
        self.put(STAT, CMPERR | DCNTEXP)

    def run(self, poly, opts, state, addr, length, timeout=2.0):
        """One scan as src/drivers/bf70x/crc.c runs it: (result or None, seconds or a reason)."""
        if self.u.rd(DMA18 + CFG)[0] & EN:
            self.foreign = True
            raise SystemExit('DMA18 is enabled: something else uses MDMA1. It was left alone.')
        self.put(CTL, opts)
        self.put(STAT, CMPERR | DCNTEXP)
        self.put(INEN, 0)
        self.put(POLY, poly)
        for _ in range(20):
            if self.reg(STAT) & LUTDONE:
                break
        else:
            self.stop()
            return None, 'no LUTDONE'
        self.put(DCNT, length // 4)
        self.put(DCNTRLD, 0)
        self.put(COMP, 0)
        self.put(RESULT_CUR, state)
        self.put(CTL, opts | OPMODE_SCAN | BLKEN)
        ms = 2
        while ms < 5 and (addr | length) & ((2 << ms) - 1) == 0:
            ms += 1
        self.u.wr(DMA18 + DSTAT, IRQDONE | IRQERR | PIRQ)
        self.u.wr(DMA18 + ADDRSTART, addr)
        self.u.wr(DMA18 + XCNT, length >> ms)
        self.u.wr(DMA18 + XMOD, 1 << ms)
        t0 = time.monotonic()
        self.u.wr(DMA18 + CFG, EN | PSIZE_4 | ms << MSIZE_SHIFT)
        why = 'timeout'
        while time.monotonic() - t0 < timeout:
            if self.reg(STAT) & DCNTEXP:
                why = None
                break
            if self.u.rd(DMA18 + DSTAT)[0] & IRQERR:
                why = 'DMA18 error, DMA_STAT %08x' % self.u.rd(DMA18 + DSTAT)[0]
                break
        t = time.monotonic() - t0
        result = self.reg(RESULT_FIN) if why is None else None
        self.stop()
        return result, why or t

    def end(self):
        """wb_crc_end(): every register the driver writes as reset leaves it."""
        if self.foreign:
            return
        self.stop()
        for off in (ADDRSTART, XCNT, XMOD):
            self.u.wr(DMA18 + off, 0)
        for off in (INEN, DCNT, DCNTRLD, COMP, RESULT_CUR, RESULT_FIN, POLY):
            self.put(off, 0)


def engine(c, name, opts, conv, v, off, length):
    s = convert(v << 16 if name == 'crc-16' else v, conv)
    r, why = c.run(POLY_OF[name], opts, s, TEST + off, length)
    if r is None:
        return None, why
    r = convert(r, conv)
    return (r >> 16 if name == 'crc-16' else r), None


def search(c, u):
    print('search: every option set and conversion on three runs of 1 KiB')
    data = bytes((i * 151 + (i >> 3) * 7 + 0x3C) & 0xFF for i in range(TEST_LEN))
    u.wr(TEST, *struct.unpack('<%dI' % (TEST_LEN // 4), data))
    for name in ('crc-16', 'crc-32'):
        want, seed = [], []
        for k, (off, length) in enumerate(RUNS):
            seed.append(0 if k == 0 else SEED[name] if k == 1 else want[0])
            want.append(soft(name, seed[k], data[off:off + length]))
        found = []
        for s in range(16):
            for conv in range(4):
                for k, (off, length) in enumerate(RUNS):
                    r, why = engine(c, name, options(s), conv, seed[k], off, length)
                    if why is not None:
                        print('%s: %s, options %s: the search stops (the board leaves %s to the software)'
                              % (name, why, option_names(options(s)), name))
                        return
                    if r != want[k]:
                        break
                else:
                    found.append((options(s), conv))
        if not found:
            print('%-7s no combination answers as the software: the board computes it in software' % name)
        for i, (opts, conv) in enumerate(found):
            print('%-7s %s options %-26s running value %s'
                  % (name, 'board uses' if i == 0 else '   also   ', option_names(opts), conv_name(conv)))


def speed(c):
    """The options and the polynomial change what is computed, not how fast."""
    print('speed (host clock: a few ms of error on each)')
    size = 0x400000
    for name in ('crc-32', 'crc-16'):
        for _ in range(8):
            r, t = c.run(POLY_OF[name], 0, 0, HEAP, size, 10.0)
            if r is None:
                print('%s: %s' % (name, t))
                break
            print('%-7s %d bytes %7.3f s %8.1f MiB/s' % (name, size, t, size / 1048576 / t))


def main(argv):
    args = argv[1:]
    port = None
    if args[:1] == ['--port']:
        port, args = args[1], args[2:]
    force = args[:1] == ['--force']
    if force:
        args = args[1:]
    if args and (set(args) - {'search', 'speed'}):
        print(__doc__)
        return 2
    u = Unit(tm.Monitor(port or tm.find_port()))
    if (not args or 'search' in args) and not force and not heap_free(u):
        print('0x%08X-0x%08X does not hold the fill of unwritten DDR: something has written there '
              '(this probe or another, if one ran since power-on). Nothing was written. See --force in '
              'the help.' % (HEAP, HEAP + 0x400000))
        return 1
    c = Crc(u)
    if u.rd(DMA18 + CFG)[0] & EN:
        print('DMA18 is enabled: something else uses MDMA1. Nothing was written.')
        return 1
    print('CRC0_CTL %08x CRC0_STAT %08x DMA18_CFG %08x DMA18_STAT %08x'
          % (c.reg(CTL), c.reg(STAT), u.rd(DMA18 + CFG)[0], u.rd(DMA18 + DSTAT)[0]))
    try:
        for name, test in (('search', lambda: search(c, u)), ('speed', lambda: speed(c))):
            if not args or name in args:
                test()
    finally:
        c.end()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
