# SPDX-License-Identifier: GPL-2.0-or-later
"""S-05 OS load model: the ROM kernel's load-function calls (S-05.008, S-05.009) and the pre-hand-over
compatibility check (S-05.024).

    load(image, header_buffer) -> {'calls': [(pSource, pDestination, count)], 'memory': {addr: bytes},
                                   'svect0': entry, 'final_psource': n}

Block kinds covered: data, FILL, zero-count, FIRST|IGNORE with count 0, FINAL.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'S-01-boot-chain'))     # memory map (S-01.012-S-01.014)
import ldr  # noqa: E402
import check_stream  # noqa: E402

STAGING = 0x8A000000
STAGING_END = 0x8A800000
REFUSED_FLAGS = ldr.INIT | ldr.CALLBACK | ldr.INDIRECT | ldr.QUICKBOOT | ldr.AUX | ldr.SAVE   # S-05.024


class Refused(Exception):
    pass


def load(image, header_buffer):
    """What the load function is asked to do, and what memory holds afterwards."""
    calls, memory = [], {}
    psource = 0

    def hook(dest, count):                        # S-05.009: copy, then advance pSource by count
        nonlocal psource
        calls.append((psource, dest, count))
        if count == 0:
            return b''
        data = image[psource:psource + count]
        psource += count
        return data

    while True:
        hdr = hook(header_buffer, 16)
        code, target, count, arg = ldr.struct.unpack('<IIII', hdr)
        if code >> 24 != ldr.SIGN:
            raise ldr.LdrError('bad signature at stream offset 0x%x' % (psource - 16))
        flags = code & 0xFFF0
        if flags & ldr.FILL:
            if count:
                w = ldr.struct.pack('<I', arg)
                memory[target] = (w * (count // 4 + 1))[:count]
        elif flags & ldr.IGNORE:
            if count:
                raise NotImplementedError('IGNORE with nonzero count')
        elif count:
            memory[target] = hook(target, count)
        if flags & ldr.FINAL:
            return {'calls': calls, 'memory': memory, 'svect0': target, 'final_psource': psource}


def compatibility_check(image):
    """S-05.024: accept the staged image only if it fits the staging area, parses to a FINAL block, and
    no block carries INIT, CALLBACK, INDIRECT, QUICKBOOT, AUX or SAVE, and S-01.017 calls it compatible:
    every destination inside L1, L2 or DDR (S-01.012) and none meeting the handover reserve (S-01.013),
    the staging area (S-01.014), the ROM's boot reserve in L2 or the record and the L1 scratch above it
    (both S-01.008). Raises Refused with the reason."""
    m = check_stream.load_map()
    if len(image) > STAGING_END - STAGING:
        raise Refused('image longer than the staging area')
    try:
        blocks = ldr.parse(image)
    except ldr.LdrError as e:
        raise Refused('stream does not parse: %s' % e)
    for b in blocks:                              # S-05.024
        bad = b['flags'] & REFUSED_FLAGS
        if bad:
            raise Refused('block at %08x carries flags 0x%04x outside FIRST, IGNORE, FILL, FINAL (flags)'
                          % (b['target'], bad))
    spaces = [m['spaces'][n] for n in ('l2', 'l1_data_a', 'l1_data_b', 'l1_code', 'l1_scratch', 'ddr')]
    for r in ldr.destinations(blocks):
        s, e = r
        if not check_stream.inside_any(r, spaces):
            raise Refused('destination %08x-%08x outside L1, L2 and DDR (space)' % (s, e))
        if check_stream.meets(r, m['staging']):
            raise Refused('destination %08x-%08x meets the staging area' % (s, e))
        for z in m['reserve']:
            if check_stream.meets(r, z):
                raise Refused('destination %08x-%08x meets the handover reserve %08x-%08x' % ((s, e) + z))
        if check_stream.meets(r, m['rom_boot_reserve']):
            raise Refused('destination %08x-%08x meets the ROM\'s boot reserve (boot reserve)' % (s, e))
        if check_stream.meets(r, m['record_and_rom_stack']):
            raise Refused('destination %08x-%08x meets the reboot record and the scratch above it (record)'
                          % (s, e))
    return blocks
