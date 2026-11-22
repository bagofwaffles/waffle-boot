#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Size, placement and layout checks of the linked bootloader.

    layout.py IMAGE.elf --ld LDSCRIPT --board OPXY_H --flash S02_H
              --memmap S01_H [--stream STREAM.ldr] [--stub STUB.elf]
              [--json LAYOUT.json]

Prints the use of each MEMORY region of LDSCRIPT and fails (exit 1) when:
  - a region is overrun, a DDR section lies outside the image's range
    (OPXY_IMAGE_START-END), or a held-mode buffer (the RAM disk's parts,
    the frame buffers) meets the image or the heap;
  - the stream does not leave a flash slot's last sector free
    (S02_SLOT_CAPACITY less S02_SECTOR_SIZE): it holds the start log (S-02.095);
    or it reaches the sector below it, the slot's settings (less two sectors,
    0x76000, S-02.104);
  - a loaded section meets the front's frame (S-01.023, FR_FRAME in
    tools/mkldr.py), which the front paints before the image is checked;
  - the handover path (.handover.*) refers to anything outside the handover
    reserve (S-01.013, S01_RESERVE_* in S01_H), apart from the boot ROM
    and MMRs; L1, which the OS may load over, fails;
  - a section meets the ROM's L2 boot reserve (S-01.008, S01_BOOT_RESERVE_*
    in S01_H), loaded or not: the ROM is running on it while the INIT
    stage runs. The stream's blocks are check_stream.py's rule
    rom_boot_reserve;
  - with --stub, a section of the stub (src/stub/stub.ld) lies outside its
    region, below its record (STUB_AT, STUB_RECORD in tools/mkldr.py), or a
    section of the image meets it, and with --stream too, the stream loads
    no packed block, or one longer than its place (OPXY_PACK_START-END);
  - a section of the image meets the packed block's place.
The relocation checks need the image linked with --emit-relocs (-q).

With --json, writes the layout in tests/oracle/S-01-boot-chain/
check_layout.py's form: one range per loaded or reserved section, the heap
and the held-mode buffers (S-01.011, S-01.013-S-01.016), and the kernel
call's stack, configuration block and header buffer by role (S-05.008, from
the symbols of HO_ROLES). Python 3, standard library only.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elf32  # noqa: E402
import mkldr  # noqa: E402
from mkldr import FR_FRAME  # noqa: E402

# S-01.012: the L1 spaces; the boot ROM; core and system MMRs.
L1 = ((0x11800000, 0x11808000), (0x11900000, 0x11908000),
      (0x11A00000, 0x11A0C000), (0x11B00000, 0x11B02000))
ROM = (0x04000000, 0x08000000)
MMR = (0x1FC00000, 0x30000000)
DDR = (0x80000000, 0x90000000)
# S-05.008: src/board/opxy_handover.c's symbols for the kernel call.
HO_ROLES = (('ho_stack', 'kernel_stack', 'stack'), ('config_block', 'config_block', 'data'),
            ('header_buffer', 'header_buffer', 'data'))


def memory_regions(path):
    with open(path) as f:
        text = f.read()
    m = re.search(r'MEMORY\s*\{(.*?)\}', text, re.S)
    if not m:
        raise SystemExit('layout: no MEMORY block in %s' % path)
    out = []
    for name, org, length in re.findall(
            r'(\w+)\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*'
            r'LENGTH\s*=\s*(0x[0-9A-Fa-f]+)', m.group(1)):
        out.append((name, int(org, 16), int(org, 16) + int(length, 16)))
    return out


def defines(path):
    with open(path) as f:
        return {k: int(v, 16) for k, v in
                re.findall(r'#define\s+(\w+)\s+(0x[0-9A-Fa-f]+)u?\b', f.read())}


def inside(addr, ranges):
    return any(lo <= addr < hi for lo, hi in ranges)


def meets(lo, hi, r):
    return lo < r[1] and r[0] < hi


def check_stream_blocks(data):
    """The stream's blocks, by S-01's own parser (tests/oracle/S-01-boot-chain)."""
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, os.path.join(here, '..', 'tests', 'oracle', 'S-01-boot-chain'))
    import check_stream  # noqa: E402
    return check_stream.ldr.parse(data)


def reloc_check(elf, prefix, allowed):
    """Every relocation in an output section named PREFIX* resolves into
    ALLOWED (or the ROM or MMRs). Returns a list of failures."""
    syms = elf.symbols()
    bad = []
    for rel in elf.sections:
        if rel.type != elf32.SHT_RELA:
            continue
        target = elf.sections[rel.info]
        if not target.name.startswith(prefix):
            continue
        for off, rtype, si, add in elf.relocations(rel):
            name, value, _, shndx = syms[si]
            if rtype == 0:
                continue
            if shndx == elf32.SHN_UNDEF:
                bad.append('%s+0x%x: undefined %s' % (target.name, off - target.addr, name))
                continue
            dest = (value + add) & 0xFFFFFFFF
            if not (inside(dest, allowed) or inside(dest, (ROM, MMR))):
                bad.append('%s+0x%x -> 0x%08x (%s%+d)' %
                           (target.name, off - target.addr, dest, name or '<section>', add))
    return bad


def main(argv):
    args = argv[1:]
    opts = {}
    for k in ('--ld', '--board', '--flash', '--memmap', '--stream', '--stub', '--json'):
        if k in args:
            i = args.index(k)
            opts[k] = args[i + 1]
            del args[i:i + 2]
    if len(args) != 1 or not all(k in opts for k in ('--ld', '--board', '--flash', '--memmap')):
        print(__doc__)
        return 2
    elf = elf32.read(args[0])
    regions = memory_regions(opts['--ld'])
    board = defines(opts['--board'])
    flash = defines(opts['--flash'])
    s01 = defines(opts['--memmap'])
    boot_reserve = (s01['S01_BOOT_RESERVE_START'], s01['S01_BOOT_RESERVE_END'])
    fails = []

    secs = [s for s in elf.sections if s.alloc and s.size]
    print('%-10s %-10s %-10s %10s %10s' % ('region', 'start', 'end', 'used', 'limit'))
    for name, lo, hi in regions:
        mine = [s for s in secs if lo <= s.addr < hi]
        used = sum(s.size for s in mine)
        top = max((s.end for s in mine), default=lo)
        print('%-10s 0x%08x 0x%08x %10d %10d' % (name, lo, hi, used, hi - lo))
        if top > hi:
            fails.append('%s overrun: ends at 0x%08x, limit 0x%08x' % (name, top, hi))
        for s in mine:
            print('  %-20s 0x%08x %8d%s' % (s.name, s.addr, s.size,
                                            '' if s.type != elf32.SHT_NOBITS else ' (not loaded)'))
    stray = [s.name for s in secs if not any(lo <= s.addr < hi for _, lo, hi in regions)]
    if stray:
        fails.append('sections outside every region: %s' % ', '.join(stray))

    image = (board['OPXY_IMAGE_START'], board['OPXY_IMAGE_END'])
    heap = (board['OPXY_HEAP_START'], board['OPXY_HEAP_END'])
    held = [('ramdisk_0', board['OPXY_DISK0_START'], board['OPXY_DISK0_END']),
            ('ramdisk_1', board['OPXY_DISK1_START'], board['OPXY_DISK1_END']),
            ('frame_buffers', board['OPXY_FRAMES_START'], board['OPXY_FRAMES_END'])]
    for s in secs:
        if s.addr >= DDR[0] and not (image[0] <= s.addr and s.end <= image[1]):
            fails.append('%s at 0x%08x-0x%08x is outside the DDR image range' %
                         (s.name, s.addr, s.end))
    for name, lo, hi in held:
        for what, r in (('the image', image), ('the heap', heap)):
            if lo < r[1] and r[0] < hi:
                fails.append('%s meets %s' % (name, what))

    for s in secs:
        if s.type != elf32.SHT_NOBITS and meets(s.addr, s.end, FR_FRAME):
            fails.append('%s at 0x%08x-0x%08x meets the front\'s frame (S-01.023)' %
                         (s.name, s.addr, s.end))

    for s in secs:
        if meets(s.addr, s.end, boot_reserve):
            fails.append('%s at 0x%08x-0x%08x meets the ROM boot reserve' %
                         (s.name, s.addr, s.end))

    if '--stream' in opts:
        n = os.path.getsize(opts['--stream'])
        log = flash['S02_SLOT_CAPACITY'] - flash['S02_SECTOR_SIZE']
        cap = log - flash['S02_SECTOR_SIZE']
        print('%-10s %32s %10d %10d' % ('stream', '', n, cap))
        if n > log:
            fails.append('the stream is %d bytes, a slot holds %d before its start log (S-02.095)'
                         % (n, log))
        elif n > cap:
            fails.append('the stream is %d bytes, a slot holds %d before its settings sector '
                         '(S-02.104)' % (n, cap))

    pack = (board['OPXY_PACK_START'], board['OPXY_PACK_END'])
    for s in secs:
        if meets(s.addr, s.end, pack):
            fails.append('%s at 0x%08x-0x%08x meets the packed block\'s place' %
                         (s.name, s.addr, s.end))
    packed = None
    if '--stream' in opts and '--stub' in opts:
        with open(opts['--stream'], 'rb') as f:
            blocks = check_stream_blocks(f.read())
        hits = [b for b in blocks if b['target'] == pack[0] and b['count']]
        if len(hits) != 1:
            fails.append('%d stream blocks load the packed block' % len(hits))
        else:
            packed = (pack[0], pack[0] + hits[0]['count'])
            print('%-10s 0x%08x 0x%08x %10d %10d' % ('packed', pack[0], pack[1],
                                                     hits[0]['count'], pack[1] - pack[0]))
            if packed[1] > pack[1]:
                fails.append('the packed block is %d bytes, its place holds %d'
                             % (hits[0]['count'], pack[1] - pack[0]))
    stub_secs = []
    if '--stub' in opts:
        stub = elf32.read(opts['--stub'])
        stub_secs = [s for s in stub.sections if s.alloc and s.size]
        used = sum(s.size for s in stub_secs)
        print('%-10s 0x%08x 0x%08x %10d %10d' % ('stub', mkldr.STUB_AT[0], mkldr.STUB_RECORD,
                                                 used, mkldr.STUB_RECORD - mkldr.STUB_AT[0]))
        for s in stub_secs:
            print('  %-20s 0x%08x %8d%s' % (s.name, s.addr, s.size,
                                            '' if s.type != elf32.SHT_NOBITS else ' (not loaded)'))
            if not (mkldr.STUB_AT[0] <= s.addr and s.end <= mkldr.STUB_RECORD):
                fails.append('the stub\'s %s at 0x%08x-0x%08x is outside its region' %
                             (s.name, s.addr, s.end))
        for s in secs:
            if meets(s.addr, s.end, mkldr.STUB_AT):
                fails.append('%s at 0x%08x-0x%08x meets the stub\'s region' %
                             (s.name, s.addr, s.end))

    reserve = [(lo, hi) for name, lo, hi in regions if name == 'handover']
    reserve.append((s01['S01_RESERVE_DDR_START'], s01['S01_RESERVE_DDR_END']))
    fails += ['handover: ' + b for b in reloc_check(elf, '.handover.', reserve)]
    if not any(s.type == elf32.SHT_RELA for s in elf.sections):
        fails.append('no relocations: link with --emit-relocs (-q)')

    if '--json' in opts:
        ranges = []
        for s in secs:
            kind = 'code' if s.flags & elf32.SHF_EXECINSTR else \
                'stack' if s.name.endswith('stack') else 'data'
            ranges.append({'name': s.name, 'start': '0x%08x' % s.addr, 'end': '0x%08x' % s.end,
                           'kind': kind, 'handover': s.name.startswith('.handover.')})
        for sym, role, kind in HO_ROLES:
            hits = [(v, n) for name, v, n, shndx in elf.symbols()
                    if name in (sym, '_' + sym) and shndx != elf32.SHN_UNDEF and n]
            if len(hits) != 1:
                fails.append('%d symbols %s (S-05.008)' % (len(hits), sym))
                continue
            v, n = hits[0]
            ranges.append({'name': sym, 'start': '0x%08x' % v, 'end': '0x%08x' % (v + n),
                           'kind': kind, 'handover': True, 'role': role})
        for s in stub_secs:
            kind = 'code' if s.flags & elf32.SHF_EXECINSTR else \
                'stack' if s.name.endswith('stack') else 'data'
            ranges.append({'name': 'stub' + s.name, 'start': '0x%08x' % s.addr,
                           'end': '0x%08x' % s.end, 'kind': kind, 'handover': False})
        if packed is not None:
            ranges.append({'name': 'packed_block', 'start': '0x%08x' % packed[0],
                           'end': '0x%08x' % packed[1], 'kind': 'data', 'handover': False})
        ranges.append({'name': 'heap', 'start': '0x%08x' % heap[0], 'end': '0x%08x' % heap[1],
                       'kind': 'heap', 'handover': False})
        for name, lo, hi in held:
            ranges.append({'name': name, 'start': '0x%08x' % lo, 'end': '0x%08x' % hi,
                           'kind': 'held_buffer', 'handover': False})
        with open(opts['--json'], 'w') as f:
            json.dump({'ranges': ranges}, f, indent=1, sort_keys=True)
            f.write('\n')

    for f in fails:
        print('FAIL ' + f)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
