#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Placement and stream checks of the linked test OS (wiki/Test-OS.md).

    testos_layout.py IMAGE.elf --ld LDSCRIPT --board OPXY_H --memmap S01_H
                     [--stream STREAM.ldr] [--json LAYOUT.json] [--device 1f]

The test OS is an OS image another bootloader loads (S-05.008 to S-05.014), so it is checked against
S-01.017 and S-05.024 rather than layout.py's rules. Prints the use of each MEMORY region of LDSCRIPT
and fails (exit 1) when:
  - a region is overrun, or a section lies in no region;
  - there is an INIT stage (.init.* sections) (S-13a.007, S-13a.020);
  - a section, loaded or not, lies outside L1, L2 and DDR (S-01.012) or meets the handover reserve
    (S-01.013), the staging area (S-01.014), the ROM's boot reserve or the reboot record and the
    scratch above it (S-01.008), or the display record (S-01.019);
  - a DDR section lies outside the test OS's image range (OPXY_TESTOS_IMAGE_START-END in OPXY_H), or
    its heap (OPXY_TESTOS_HEAP_START-END) or a held-mode buffer (the RAM disk's parts, the frame
    buffers) meets the image, or the heap meets a held-mode buffer, the reserve, the staging area or
    the display record (S-01.015, S-01.016);
  - a loaded section's load address (its LMA, from the ELF's program headers) is outside the image
    range: the test OS loads nothing into L1 or L2, and a section that runs in L1 (.l1.text) is
    copied there by the entry shim from its load address in the image;
  - with --stream, S-05.024 would refuse the stream: it does not parse to a FINAL block, is longer
    than 0x800000 bytes, has a block with INIT, CALLBACK, INDIRECT, QUICKBOOT, AUX or SAVE, or a
    destination kernel_model.py's check calls incompatible (S-01.017);
  - with --stream, the stream is not FIRST|IGNORE, one plain data block whose destination is the DDR
    image range, FINAL: a destination in L1 or L2, a second data block, a FILL block, all fail;
  - the entry is not the same by every reading of S-05.013: the ELF entry must be the first byte of
    the DDR image, OPXY_TESTOS_IMAGE_START, where the lowest loaded byte is, and with --stream the
    FIRST block's target (what the ROM kernel loads into RCU0_SVECT0), the FINAL block's target
    (kernel_model.py) and the FINAL block's argument (mkldr.py --no-init) must all be that entry.
    The ROM's default application entry 0x11A00000 (HRM Table 36-10) is in L1, which the image
    cannot load, so that reading is not absorbed.
With --device 1f, the OP-1F's installer images (bf703_testos_1f.ld), laid out like the OP-1F's OS
1.7.3 (S-05.040) for its own loader: the image range is OPXY_TESTOS_1F_IMAGE_START-END, the held-mode
buffers are the OP-1F's (its one RAM disk region, the OS frame, the frame buffers), the stream is
FIRST|IGNORE, one L1 code block at 0x11A00000 holding _wb_testos_entry_1f (a whole number of 8-byte
words), the DDR image as consecutive blocks from the image range's start, FINAL; the first block's
argument the stream's length less 16, every count a multiple of 4, no block longer than 0xC000 bytes
in L1 or 0xACAB8 in DDR (S-05.041); the ELF entry, the FIRST and FINAL targets are
0x11A00000 and the FINAL argument 0; the entry shim _wb_testos_entry is the DDR image's first byte;
and every destination lies inside one of OS 1.7.3's ranges
(tests/oracle/S-05-os-handoff/vectors/os_destinations_1f.json). S-01.016's room is then the OP-1F's RAM
disk (S-12.008, OPXY_1F_DISK_SECTORS), checked here against the board header, and by check_layout.py
--device 1f against S-18's device.ramdisk_blocks and device.ramdisk_regions.
With --json, writes the layout in tests/oracle/S-01-boot-chain/check_layout.py's form: one range per
section, the heap and the held-mode buffers, none of them a handover range. Python 3, standard library
only.
"""
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(TOP, 'tests', 'oracle', 'S-05-os-handoff'))
import elf32  # noqa: E402
import layout  # noqa: E402
import kernel_model  # noqa: E402

L2 = (0x08000000, 0x08040000)      # S-01.012
L1_CODE = (0x11A00000, 0x11A0C000)
ENTRY_1F = 0x11A00000              # S-05.040: OS 1.7.3's FIRST and FINAL target
L1_BLOCK_MAX_1F, DDR_BLOCK_MAX_1F = 0xC000, 0xACAB8   # S-05.041
OS_1F = os.path.join(TOP, 'tests', 'oracle', 'S-05-os-handoff', 'vectors', 'os_destinations_1f.json')


def os_1f_ranges():
    """S-05.040: OS 1.7.3's destinations, [(start, end)]."""
    with open(OS_1F) as f:
        v = json.load(f)
    return [(int(r['start'], 16), int(r['end'], 16)) for r in v['ranges']]
SPACES = layout.L1 + (L2, layout.DDR)
STAGING_MAX = 0x800000             # S-01.014
FLAG_FINAL, FLAG_FIRST, FLAG_IGNORE, FLAG_FILL = 0x8000, 0x4000, 0x1000, 0x0100  # LDR flags


def forbidden(s01):
    """[(name, (start, end))]: the ranges no section of an OS image may meet, from S01_H."""
    return [
        ('the L2 handover reserve (S-01.013)', (s01['S01_RESERVE_L2_START'], s01['S01_RESERVE_L2_END'])),
        ('the DDR handover reserve (S-01.013)', (s01['S01_RESERVE_DDR_START'], s01['S01_RESERVE_DDR_END'])),
        ('the staging area (S-01.014)', (s01['S01_STAGING_START'], s01['S01_STAGING_END'])),
        ("the ROM's boot reserve (S-01.008)", (s01['S01_BOOT_RESERVE_START'], s01['S01_BOOT_RESERVE_END'])),
        ('the reboot record and the scratch above it (S-01.008)', (s01['S01_RECORD_START'], s01['S01_RECORD_END'])),
        ('the display record (S-01.019)', (s01['S01_DISPLAY_RECORD_START'], s01['S01_DISPLAY_RECORD_END'])),
    ]


def ranges_of(board, device):
    """The image, the heap and the held-mode buffers of an image for device ('xy' or '1f')."""
    if device == '1f':
        image = (board['OPXY_TESTOS_1F_IMAGE_START'], board['OPXY_TESTOS_1F_IMAGE_END'])
        heap = (board['OPXY_TESTOS_1F_HEAP_START'], board['OPXY_TESTOS_1F_HEAP_END'])
        held = [('ramdisk', board['OPXY_1F_DISK_START'], board['OPXY_1F_DISK_END']),
                ('os_frame', board['OPXY_OS_FRAME_START'], board['OPXY_OS_FRAME_END']),
                ('frame_buffers', board['OPXY_FRAMES_START'], board['OPXY_FRAMES_END'])]
        return image, heap, held
    image = (board['OPXY_TESTOS_IMAGE_START'], board['OPXY_TESTOS_IMAGE_END'])
    heap = (board['OPXY_TESTOS_HEAP_START'], board['OPXY_TESTOS_HEAP_END'])
    held = [('ramdisk_0', board['OPXY_DISK0_START'], board['OPXY_DISK0_END']),
            ('ramdisk_1', board['OPXY_DISK1_START'], board['OPXY_DISK1_END']),
            ('frame_buffers', board['OPXY_FRAMES_START'], board['OPXY_FRAMES_END'])]
    return image, heap, held


def entry_block(s, device):
    """The OP-1F image's L1 block: the entry stub, loaded where it runs."""
    return device == '1f' and s.name == '.l1.entry' and s.lma == s.addr and \
        L1_CODE[0] <= s.addr and s.end <= L1_CODE[1]


def check_sections(elf, regions, board, s01, device='xy'):
    """(failures, printed lines) of the ELF's sections against the rules above."""
    fails, out = [], []
    secs = [s for s in elf.sections if s.alloc and s.size]
    out.append('%-10s %-10s %-10s %10s %10s' % ('region', 'start', 'end', 'used', 'limit'))
    for name, lo, hi in regions:
        mine = [s for s in secs if lo <= s.addr < hi]
        used = sum(s.size for s in mine)
        top = max((s.end for s in mine), default=lo)
        out.append('%-10s 0x%08x 0x%08x %10d %10d' % (name, lo, hi, used, hi - lo))
        if top > hi:
            fails.append('%s overrun: ends at 0x%08x, limit 0x%08x' % (name, top, hi))
        for s in mine:
            out.append('  %-20s 0x%08x %8d%s' % (s.name, s.addr, s.size,
                                                 ' (not loaded)' if s.type == elf32.SHT_NOBITS else
                                                 ' (loaded at 0x%08x)' % s.lma if s.lma != s.addr else ''))
    stray = [s.name for s in secs if not any(lo <= s.addr < hi for _, lo, hi in regions)]
    if stray:
        fails.append('sections outside every region: %s' % ', '.join(stray))
    inits = [s.name for s in secs if s.name.startswith('.init.')]
    if inits:
        fails.append('an INIT stage in an OS image: %s' % ', '.join(inits))

    image, heap, held = ranges_of(board, device)
    for s in secs:
        if not layout.inside(s.addr, SPACES) or not any(lo < s.end <= hi for lo, hi in SPACES
                                                        if lo <= s.addr < hi):
            fails.append('%s at 0x%08x-0x%08x is outside L1, L2 and DDR' % (s.name, s.addr, s.end))
        for what, r in forbidden(s01):
            if layout.meets(s.addr, s.end, r):
                fails.append('%s at 0x%08x-0x%08x meets %s' % (s.name, s.addr, s.end, what))
        if s.addr >= layout.DDR[0] and not (image[0] <= s.addr and s.end <= image[1]):
            fails.append('%s at 0x%08x-0x%08x is outside the test OS image range 0x%08x-0x%08x' %
                         (s.name, s.addr, s.end, image[0], image[1]))
        if s.type != elf32.SHT_NOBITS and not (image[0] <= s.lma and s.lma_end <= image[1]) and \
                not entry_block(s, device):
            fails.append('%s loads at 0x%08x-0x%08x, outside the DDR image range 0x%08x-0x%08x: the test OS '
                         'loads nothing into L1 or L2' %
                         (s.name, s.lma, s.lma_end, image[0], image[1]))
    if layout.meets(heap[0], heap[1], image):
        fails.append('the heap meets the image')
    for what, r in forbidden(s01):
        if layout.meets(heap[0], heap[1], r):
            fails.append('the heap meets %s' % what)
    for name, lo, hi in held:
        for what, r in (('the image', image), ('the heap', heap)):
            if layout.meets(lo, hi, r):
                fails.append('%s meets %s' % (name, what))
    return fails, out, secs, heap, held


def check_stream_file(path):
    """S-05.024 on the stream: [] or the reason it would be refused."""
    with open(path, 'rb') as f:
        data = f.read()
    try:
        kernel_model.compatibility_check(data)
    except kernel_model.Refused as e:
        return ['the stream would be refused by S-05.024: %s' % e]
    return []


def stream_blocks(data):
    """[(flags, target, count, argument)] of a stream's blocks up to and including FINAL, by
    their headers alone; None for a stream that does not parse to a FINAL block."""
    out = []
    pos = 0
    while pos + 16 <= len(data):
        code, target, count, arg = struct.unpack('<IIII', data[pos:pos + 16])
        if code >> 24 != 0xAD:
            return None
        flags = code & 0xFFF0
        out.append((flags, target, count, arg))
        pos += 16
        if not flags & (FLAG_FILL | FLAG_IGNORE):
            pos += count
        if flags & FLAG_FINAL:
            return out
    return None


def stream_entries(data):
    """(FIRST target, FINAL target, FINAL argument) of a stream; None for one that does not
    parse to a FINAL block."""
    hs = stream_blocks(data)
    if hs is None:
        return None
    return hs[0][1], hs[-1][1], hs[-1][3]


def check_blocks(data, image):
    """The stream's shape the test OS keeps for any loading bootloader: FIRST,
    exactly one plain data block inside the DDR image range, FINAL. No destination in L1 or L2."""
    hs = stream_blocks(data)
    if hs is None:
        return ['the stream does not parse to a FINAL block']
    fails = []
    first, final, blocks = hs[0], hs[-1], hs[1:-1]
    if first[0] != FLAG_FIRST | FLAG_IGNORE or first[2] != 0:
        fails.append('the first block is not FIRST|IGNORE of count 0')
    if final[0] != FLAG_FINAL or final[2] != 0:
        fails.append('the last block is not FINAL of count 0')
    for fl, t, c, _ in blocks:
        end = t + c
        if layout.inside(t, layout.L1) or L2[0] <= t < L2[1]:
            fails.append('block destination 0x%08x-0x%08x is in L1 or L2: the loader\'s hand-over '
                         'runs there' % (t, end))
        elif not (image[0] <= t and end <= image[1]):
            fails.append('block destination 0x%08x-0x%08x is outside the DDR image range 0x%08x-0x%08x' %
                         (t, end, image[0], image[1]))
        if fl != 0:
            fails.append('block at 0x%08x has flags 0x%04x: the test OS\'s one data block is plain' % (t, fl))
    if len(blocks) != 1:
        fails.append('%d data blocks, wanted one: the DDR image (FIRST, the block, FINAL)' % len(blocks))
    return fails


def check_blocks_1f(data, image):
    """The OP-1F images' shape (S-05.040, S-05.041): FIRST|IGNORE of count 0 whose argument is the
    stream's length less 16, one L1 code block at 0x11A00000 of whole 8-byte words, the DDR image as
    consecutive blocks from the image range's start inside it, FINAL of count 0 with argument 0; the
    flags FIRST, IGNORE, FILL and FINAL alone; every count a multiple of 4, no block longer than 0xC000
    bytes in L1 or 0xACAB8 in DDR; every destination inside OS 1.7.3's ranges."""
    hs = stream_blocks(data)
    if hs is None:
        return ['the stream does not parse to a FINAL block']
    fails = []
    first, final, blocks = hs[0], hs[-1], hs[1:-1]
    if first[0] != FLAG_FIRST | FLAG_IGNORE or first[2] != 0:
        fails.append('the first block is not FIRST|IGNORE of count 0')
    if first[3] != len(data) - 16:
        fails.append('the first block\'s argument is 0x%x, not the stream\'s length less 16 (0x%x)' %
                     (first[3], len(data) - 16))
    if final[0] != FLAG_FINAL or final[2] != 0 or final[3] != 0:
        fails.append('the last block is not FINAL of count 0 and argument 0')
    allowed = FLAG_FIRST | FLAG_IGNORE | FLAG_FILL | FLAG_FINAL
    for fl, t, c, _ in hs:
        if fl & ~allowed:
            fails.append('block at 0x%08x has flags 0x%04x: OS 1.7.3 uses FIRST, IGNORE, FILL, FINAL' %
                         (t, fl))
        if c % 4:
            fails.append('block at 0x%08x: count 0x%x is not a multiple of 4 (S-05.041)' % (t, c))
    l1 = [(t, c) for fl, t, c, _ in blocks if L1_CODE[0] <= t < L1_CODE[1]]
    ddr = [(t, c) for fl, t, c, _ in blocks if t >= layout.DDR[0]]
    if len(l1) != 1 or l1[0][0] != ENTRY_1F or l1[0][1] % 8 or not l1[0][1]:
        fails.append('wanted one L1 code block at 0x%08x of whole 8-byte words, got %s' %
                     (ENTRY_1F, ', '.join('0x%08x+%x' % b for b in l1) or 'none'))
    for t, c in l1:
        if c > L1_BLOCK_MAX_1F:
            fails.append('L1 block at 0x%08x is 0x%x bytes, over 0x%x (S-05.041)' % (t, c, L1_BLOCK_MAX_1F))
    for t, c in ddr:
        if c > DDR_BLOCK_MAX_1F:
            fails.append('DDR block at 0x%08x is 0x%x bytes, over 0x%x (S-05.041)' % (t, c, DDR_BLOCK_MAX_1F))
    joined = not ddr or ddr[0][0] == image[0]
    for (t0, c0), (t1, _) in zip(ddr, ddr[1:]):
        joined = joined and t1 == t0 + c0
    if not ddr or not joined or ddr[-1][0] + ddr[-1][1] > image[1]:
        fails.append('wanted the DDR image as consecutive blocks from 0x%08x inside 0x%08x-0x%08x, got %s' %
                     (image[0], image[0], image[1], ', '.join('0x%08x+%x' % b for b in ddr) or 'none'))
    if len(l1) + len(ddr) != len(blocks):
        fails.append('%d blocks besides FIRST and FINAL, wanted the L1 block and the DDR blocks' % len(blocks))
    ranges = os_1f_ranges()
    for fl, t, c, _ in blocks:
        if c and not any(lo <= t and t + c <= hi for lo, hi in ranges):
            fails.append('block destination 0x%08x-0x%08x is in none of OS 1.7.3\'s ranges (S-05.040)' %
                         (t, t + c))
    return fails


def check_entry_1f(elf, stream_path, image):
    """The OP-1F images' entry: 0x11A00000 by every reading (the ELF, the FIRST and FINAL targets), the
    FINAL argument 0, and the entry shim the DDR image's first byte."""
    fails = []
    if elf.entry != ENTRY_1F:
        fails.append('the ELF entry 0x%08x is not 0x%08x' % (elf.entry, ENTRY_1F))
    shim = elf.symbol('_wb_testos_entry')
    if shim != image[0]:
        fails.append('the entry shim is not the first byte of the DDR image 0x%08x' % image[0])
    if stream_path is None:
        return fails
    with open(stream_path, 'rb') as f:
        data = f.read()
    ends = stream_entries(data)
    if ends is None:
        return fails + ['the stream does not parse to a FINAL block']
    for what, got, want in zip(('FIRST block target', 'FINAL block target', 'FINAL block argument'), ends,
                               (ENTRY_1F, ENTRY_1F, 0)):
        if got != want:
            fails.append('the stream\'s %s 0x%08x is not 0x%08x' % (what, got, want))
    return fails + check_blocks_1f(data, image)


def check_entry(elf, stream_path, image):
    """The entry the loading bootloader will take (S-05.013), the same by every reading the
    image can absorb: the ELF entry, which must be the first byte of the DDR image (the lowest
    loaded byte, OPXY_TESTOS_IMAGE_START, where bf703_testos.ld puts _wb_testos_entry);
    RCU0_SVECT0 as the ROM kernel sets it from the FIRST block's target (HRM "Boot Termination
    and Application Execution"); the FINAL block's target (kernel_model.py);
    and the FINAL block's argument (mkldr.py --no-init)."""
    fails = []
    if elf.entry != image[0]:
        fails.append('the ELF entry 0x%08x is not the first byte of the DDR image 0x%08x' %
                     (elf.entry, image[0]))
    loaded = [s.lma for s in elf.sections if s.alloc and s.size and s.type != elf32.SHT_NOBITS]
    if loaded and min(loaded) != elf.entry:
        fails.append('the lowest loaded byte 0x%08x is not the entry 0x%08x: the entry shim must come first' %
                     (min(loaded), elf.entry))
    if stream_path is None:
        return fails
    with open(stream_path, 'rb') as f:
        data = f.read()
    ends = stream_entries(data)
    if ends is None:
        return fails + ['the stream does not parse to a FINAL block']
    for what, got in zip(('FIRST block target', 'FINAL block target', 'FINAL block argument'), ends):
        if got != elf.entry:
            fails.append('the stream\'s %s 0x%08x is not the entry 0x%08x' % (what, got, elf.entry))
    return fails + check_blocks(data, image)


def layout_json(secs, heap, held):
    ranges = []
    for s in secs:
        kind = 'code' if s.flags & elf32.SHF_EXECINSTR else 'stack' if s.name.endswith('stack') else 'data'
        ranges.append({'name': s.name, 'start': '0x%08x' % s.addr, 'end': '0x%08x' % s.end,
                       'kind': kind, 'handover': False})
    ranges.append({'name': 'heap', 'start': '0x%08x' % heap[0], 'end': '0x%08x' % heap[1],
                   'kind': 'heap', 'handover': False})
    for name, lo, hi in held:
        ranges.append({'name': name, 'start': '0x%08x' % lo, 'end': '0x%08x' % hi,
                       'kind': 'held_buffer', 'handover': False})
    return {'ranges': ranges}


def main(argv):
    args = argv[1:]
    opts = {}
    for k in ('--ld', '--board', '--memmap', '--stream', '--json', '--device'):
        if k in args:
            i = args.index(k)
            opts[k] = args[i + 1]
            del args[i:i + 2]
    if len(args) != 1 or not all(k in opts for k in ('--ld', '--board', '--memmap')):
        print(__doc__)
        return 2
    elf = elf32.read(args[0])
    regions = layout.memory_regions(opts['--ld'])
    board = layout.defines(opts['--board'])
    s01 = layout.defines(opts['--memmap'])
    device = opts.get('--device', 'xy')
    if device not in ('xy', '1f'):
        print(__doc__)
        return 2
    fails, out, secs, heap, held = check_sections(elf, regions, board, s01, device)
    for line in out:
        print(line)
    print('entry      0x%08x' % elf.entry)
    if '--stream' in opts:
        n = os.path.getsize(opts['--stream'])
        print('%-10s %32s %10d %10d' % ('stream', '', n, STAGING_MAX))
        fails += check_stream_file(opts['--stream'])
    image = ranges_of(board, device)[0]
    if device == '1f':
        fails += check_entry_1f(elf, opts.get('--stream'), image)
        room = board['OPXY_1F_DISK_END'] - board['OPXY_1F_DISK_START']
        with open(opts['--board']) as f:
            m = re.search(r'#define\s+OPXY_1F_DISK_SECTORS\s+(\d+)u?\b', f.read())
        need = int(m.group(1)) * 512 if m else None
        if need is None or room < need:
            fails.append('the RAM disk region holds %d bytes, short of S-12.008\'s %s' % (room, need))
        if not any(lo <= image[0] and image[1] <= hi for lo, hi in os_1f_ranges()):
            fails.append('the image range 0x%08x-0x%08x is in none of OS 1.7.3\'s ranges' % image)
    else:
        fails += check_entry(elf, opts.get('--stream'), image)
    if '--json' in opts:
        with open(opts['--json'], 'w') as f:
            json.dump(layout_json(secs, heap, held), f, indent=1, sort_keys=True)
            f.write('\n')
    for f in fails:
        print('FAIL ' + f)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
