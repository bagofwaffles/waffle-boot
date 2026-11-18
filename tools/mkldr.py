#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Write the bootloader's LDR boot stream from its linked ELF file.

    mkldr.py IMAGE.elf OUT.ldr --front FRONT.elf
    mkldr.py FRONT.elf OUT.ldr --front-only
    mkldr.py IMAGE.elf OUT.ldr --no-init

One of the three modes must be given.

The stream (ADSP-BF70x HRM, "Boot Loader Stream"; S-05 Terms; S-01.010):
  1. a FIRST|IGNORE block of count 0: its target is the ELF entry, its
     argument the length of the rest of the stream (the next-application
     pointer of a single-application stream);
  2. one block per run of loaded bytes in L1;
  3. the INIT block, count 0, whose target is the front's _fr_init;
  4. one block per run of loaded bytes anywhere else (L2, DDR);
  5. a FINAL block of count 0 targeting the entry.
A block's destination is the section's load address (its LMA, from the
ELF's program headers), which the bootloader's link map makes its run
address; the test OS's map gives one section a load address apart from its
run address, and something of the image copies it over at start-up.

With --no-init there is no INIT block (step 3): the stream of an OS image
(S-05.024 refuses one with INIT; wiki/Test-OS.md), whose clocks and DDR are
up when it loads. The order of the other blocks is kept, and the FINAL
block's argument holds the entry too. The entry reaches the loading
bootloader through RCU0_SVECT0 (S-05.013), which the ROM kernel sets from
the FIRST block's target (HRM "Boot Termination and Application Execution";
with IGNORE set, the entry is still taken); kernel_model.py takes it from
the FINAL block's target. A test-OS stream
gives the same address in all three places and as the ELF entry, the first
byte of its one DDR block, where bf703_testos.ld puts the entry shim
(tools/testos_layout.py checks it). The argument of a block without FILL,
INIT or CALLBACK is not used by the kernel.

With --front, the bootloader's stream carries the frozen front
(src/front/fr_handoff.h): the front's blocks join the image's in the same
order (L1 first), the INIT block is the front's _fr_init, the entry (FIRST
and FINAL targets) is the front's _fr_entry, and one more block puts the
image descriptor at FR_DESC_ADDR: the image's entry, its runs but those in
L1 code and their CRC-32, the CRC-32 of the front's payloads, the
bootloader's version (the image's _opxy_bl_version, four u16) and the
front's pin. The front checks the runs as data with L1DM_DCTL.ENX clear,
so a load from L1 code would fault (S-01.023, S-13a.040); every listed run
must lie where the front reads (FR_READABLE). No run and no NOLOAD section
of the image may meet the front's windows or the shared block. An image built
with the slave's check of its L1 code (S-09.019) has its mark filled first
(mark_l1()). The stream
must leave the slot's last sector free, FR_LOG_IMAGE_MAX bytes at most: that
sector holds the start log (S-02.095). This build's own stream is held to
STORE_IMAGE_MAX, so that in either slot it leaves the sector below the log,
the slot's settings sector, free (S-02.099, S-02.104). --front-only streams the front alone,
a recovery image with no descriptor.

Every target and count is a multiple of 4: each run is widened to whole
words with zero bytes. Runs closer than GAP bytes are joined with zeros.
The block code's DMA code nibble is 4 (32-bit). Python 3, standard library
only.
"""
import hashlib
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elf32  # noqa: E402

SIGNATURE = 0xAD
DMACODE = 4                    # SPEC-Q: the 32-bit DMA code, as the vectors' streams use
FINAL, FIRST, IGNORE, INIT = 1 << 15, 1 << 14, 1 << 12, 1 << 11
GAP = 16

# S-01.012: the L1 spaces (data A, data B, code, scratch).
L1 = ((0x11800000, 0x11808000), (0x11900000, 0x11908000),
      (0x11A00000, 0x11A0C000), (0x11B00000, 0x11B02000))


def header(flags, target, count, argument):
    code = (SIGNATURE << 24) | (flags & 0xFFF0) | DMACODE
    h = bytearray(struct.pack('<IIII', code, target, count, argument))
    x = 0
    for b in h:
        x ^= b
    h[2] = x                   # HDRCHK: the 16 bytes XOR to zero
    return bytes(h)


def in_l1(addr):
    return any(lo <= addr < hi for lo, hi in L1)


def runs(elf):
    """The loaded bytes as [(load address, bytes)], word-aligned and joined."""
    secs = sorted((s for s in elf.sections
                   if s.alloc and s.type != elf32.SHT_NOBITS and s.size),
                  key=lambda s: s.lma)
    out = []
    for s in secs:
        data = elf.contents(s)
        if out and s.lma - (out[-1][0] + len(out[-1][1])) < GAP and \
                in_l1(s.lma) == in_l1(out[-1][0]):
            base, buf = out[-1]
            if s.lma < base + len(buf):
                raise SystemExit('mkldr: %s overlaps the section before it' % s.name)
            buf += bytes(s.lma - base - len(buf)) + data
        else:
            out.append((s.lma, bytearray(data)))
    aligned = []
    for base, buf in out:
        lead = base & 3
        buf = bytes(lead) + bytes(buf)
        buf += bytes(-len(buf) & 3)
        aligned.append((base - lead, buf))
    return aligned


def stream(elf, init_symbol):
    """The stream; init_symbol None leaves the INIT block out (--no-init)."""
    init = None
    if init_symbol is not None:
        init = elf.symbol(init_symbol)
        if init is None:
            raise SystemExit('mkldr: no symbol %s' % init_symbol)
    entry = elf.entry
    rs = runs(elf)
    body = bytearray()
    for addr, buf in rs:
        if in_l1(addr):
            body += header(0, addr, len(buf), 0) + buf
    if init is not None:
        body += header(INIT, init, 0, 0)
    for addr, buf in rs:
        if not in_l1(addr):
            body += header(0, addr, len(buf), 0) + buf
    # An OS image's FINAL block carries the entry in its argument as well (see
    # the docstring); the bootloader's keeps 0, so its stream does not change.
    body += header(FINAL, entry, 0, entry if init is None else 0)
    return header(FIRST | IGNORE, entry, 0, len(body)) + bytes(body)


# src/front/fr_handoff.h
FR_WINDOWS = ((0x11A04000, 0x11A0C000), (0x11802000, 0x11804000), (0x08002000, 0x08030000))
FR_SHARED = (0x08030000, 0x08031000)
FR_DESC_ADDR = 0x08030000
# S-13a.040: L1 instruction memory, which the front does not read as data.
L1_CODE = (0x11A00000, 0x11A0C000)
# fr_image.c's fr_mem_readable(): where the front reads the loaded image.
FR_READABLE = ((0x11800000, 0x11802000), (0x11804000, 0x11807000), (0x11900000, 0x11908000),
               (0x08030000, 0x08030800), (0x08031000, 0x0803DFD4), (0x8803D000, 0x8833D000))
FR_DESC_MAGIC = 0x4D494257
FR_DESC_VERSION = 1
FR_DESC_HEAD = 0x40
FR_DESC_REGIONS = 24
FR_LOG_IMAGE_MAX = 0x77000     # a slot less its last sector, the start log (S-02.095)
# S-09.019: the slave's L1 check's mark (waffle/board/opxy_boot.h), as compiled and as filled
L1_MARK = '_opxy_l1_mark'
L1_MARK_EMPTY, L1_MARK_FULL = 0x4C314D30, 0x4C314D31
STORE_IMAGE_MAX = 0x76000      # and less the sector below it, the slot's settings (S-02.099, S-02.104)


def in_front(addr):
    return any(lo <= addr < hi for lo, hi in FR_WINDOWS)


def meets(a, n, lo, hi):
    """[a, a + n) and [lo, hi) share a byte."""
    return n > 0 and a < hi and lo < a + n


def checked(rs):
    """The runs the descriptor lists: all but those in L1 code."""
    return [(a, b) for a, b in rs if not meets(a, len(b), *L1_CODE)]


def ordered(rs):
    """Runs in stream order: L1 first, then the rest, each by address."""
    return [r for r in rs if in_l1(r[0])] + [r for r in rs if not in_l1(r[0])]


def front_runs(front):
    rs = runs(front)
    bad = ['%08x+%x' % (a, len(b)) for a, b in rs if not in_front(a) or not in_front(a + len(b) - 1)]
    if bad:
        raise SystemExit('mkldr: front bytes outside its windows: ' + ', '.join(bad))
    return ordered(rs)


def front_pin(front):
    """The front's pin: SHA-256 over each run's address, length and bytes."""
    h = hashlib.sha256()
    for addr, buf in front_runs(front):
        h.update(struct.pack('<II', addr, len(buf)) + buf)
    return h.digest()


def descriptor(main, front):
    rs = checked(ordered(runs(main)))
    bad = ['%08x+%x' % (a, len(b)) for a, b in rs
           if not any(lo <= a and a + len(b) <= hi for lo, hi in FR_READABLE)]
    if bad:
        raise SystemExit('mkldr: runs where the front does not read: ' + ', '.join(bad))
    if len(rs) > FR_DESC_REGIONS:
        raise SystemExit('mkldr: %d runs, the descriptor holds %d' % (len(rs), FR_DESC_REGIONS))
    crc = 0
    for _, buf in rs:
        crc = zlib.crc32(buf, crc)
    fcrc = 0
    for _, buf in front_runs(front):
        fcrc = zlib.crc32(buf, fcrc)
    sym = main.symbol('_opxy_bl_version')
    sec = [] if sym is None else [
        s for s in main.sections if s.alloc and s.type != elf32.SHT_NOBITS
        and s.addr <= sym and sym + 8 <= s.addr + s.size]
    if not sec:
        raise SystemExit('mkldr: the image has no loaded _opxy_bl_version '
                         '(the descriptor\'s version)')
    data = main.contents(sec[0])
    ver = bytes(data[sym - sec[0].addr:sym - sec[0].addr + 8])
    size = FR_DESC_HEAD + 8 * len(rs) + 4
    d = struct.pack('<IHHIIII', FR_DESC_MAGIC, FR_DESC_VERSION, size, main.entry, len(rs),
                    crc & 0xFFFFFFFF, fcrc & 0xFFFFFFFF) + ver + front_pin(front)
    for addr, buf in rs:
        d += struct.pack('<II', addr, len(buf))
    return d + struct.pack('<I', zlib.crc32(d) & 0xFFFFFFFF)


def mark_l1(main):
    """S-09.019: an image built with the slave's check of its L1 code (BOOT_LINK_FC) has the 16-byte mark
    _opxy_l1_mark, four words compiled as L1_MARK_EMPTY and zeros. It is filled in the image's bytes here,
    before anything else is taken from them, with L1_MARK_FULL, the address and length of the one run the
    stream loads into L1 code and that run's CRC-32 (zlib's), so the descriptor's CRC covers the filled
    mark. An image without the symbol is left as it is."""
    sym = main.symbol(L1_MARK)
    if sym is None:
        return
    sec = [s for s in main.sections if s.alloc and s.type != elf32.SHT_NOBITS
           and s.addr <= sym and sym + 16 <= s.addr + s.size]
    if not sec:
        raise SystemExit('mkldr: %s is not in a loaded section' % L1_MARK)
    off = sec[0].offset + sym - sec[0].addr
    if struct.unpack_from('<I', main.data, off)[0] != L1_MARK_EMPTY:
        raise SystemExit('mkldr: %s holds no empty mark' % L1_MARK)
    code = [(a, b) for a, b in runs(main) if meets(a, len(b), *L1_CODE)]
    if len(code) != 1 or code[0][0] < L1_CODE[0] or code[0][0] + len(code[0][1]) > L1_CODE[1]:
        raise SystemExit('mkldr: the slave\'s L1 check needs one run inside L1 code, not %d' % len(code))
    addr, buf = code[0]
    data = bytearray(main.data)
    struct.pack_into('<IIII', data, off, L1_MARK_FULL, addr, len(buf), zlib.crc32(buf) & 0xFFFFFFFF)
    main.data = bytes(data)


def merged(main, front):
    """The bootloader's stream with the front and the descriptor."""
    mark_l1(main)
    init = front.symbol('_fr_init')
    entry = front.entry
    if init is None:
        raise SystemExit('mkldr: the front has no _fr_init')
    mr, fr = runs(main), front_runs(front)
    # S-01.023: no loaded byte and no NOLOAD section of the image in the
    # front's windows or the shared block (descriptor and hand-off record).
    spans = [(a, len(b)) for a, b in mr] + \
        [(s.addr, s.size) for s in main.sections
         if s.alloc and s.type == elf32.SHT_NOBITS and s.size]
    for a, n in spans:
        if any(meets(a, n, lo, hi) for lo, hi in FR_WINDOWS + (FR_SHARED,)):
            raise SystemExit('mkldr: the image meets the front at %08x+%x' % (a, n))
    desc = descriptor(main, front)
    desc += bytes(-len(desc) & 3)
    rs = sorted(mr + fr + [(FR_DESC_ADDR, desc)])
    body = bytearray()
    for addr, buf in rs:
        if in_l1(addr):
            body += header(0, addr, len(buf), 0) + buf
    body += header(INIT, init, 0, 0)
    for addr, buf in rs:
        if not in_l1(addr):
            body += header(0, addr, len(buf), 0) + buf
    body += header(FINAL, entry, 0, 0)
    data = header(FIRST | IGNORE, entry, 0, len(body)) + bytes(body)
    if len(data) > FR_LOG_IMAGE_MAX:
        raise SystemExit('mkldr: the stream is %d bytes; a slot image may be at most %d (0x%X): '
                         'the slot\'s last sector holds the start log (S-02.095)'
                         % (len(data), FR_LOG_IMAGE_MAX, FR_LOG_IMAGE_MAX))
    if len(data) > STORE_IMAGE_MAX:
        raise SystemExit('mkldr: the stream is %d bytes; a waffle-boot image may be at most %d '
                         '(0x%X): the slot\'s last two sectors hold its settings and its start log '
                         '(S-02.104)'
                         % (len(data), STORE_IMAGE_MAX, STORE_IMAGE_MAX))
    return data


def main(argv):
    args = argv[1:]
    modes = 0
    init_symbol = None
    if '--no-init' in args:
        args.remove('--no-init')
        modes += 1
    front = None
    if '--front' in args:
        i = args.index('--front')
        front = elf32.read(args[i + 1])
        del args[i:i + 2]
        modes += 1
    if '--front-only' in args:
        args.remove('--front-only')
        init_symbol = '_fr_init'
        modes += 1
    if len(args) != 2 or modes != 1:
        print(__doc__)
        return 2
    if front is not None:
        data = merged(elf32.read(args[0]), front)
    else:
        data = stream(elf32.read(args[0]), init_symbol)
    with open(args[1], 'wb') as f:
        f.write(data)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
