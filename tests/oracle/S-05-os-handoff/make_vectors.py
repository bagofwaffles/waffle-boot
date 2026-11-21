#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the S-05 vectors in vectors/: synthetic OS images, the load-function call sequence and the
memory each must leave, the S-05.024 refusal cases and the S-05.027-.029 cases. Deterministic. Expected
calls and memory are counted here from the block list, independently of kernel_model.py.
"""
import hashlib
import json
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ldr  # noqa: E402
import kernel_model  # noqa: E402

ENTRY = 0x11A00000
HEADER_BUFFER = 0x88A34D5C     # a header buffer address inside the DDR handover reserve (S-01.013)
OUT = os.path.join(HERE, 'vectors')


def payload(rng, n):
    return bytes(rng.getrandbits(8) for _ in range(n))


def os_blocks(rng):
    """synthetic_os_a: every block kind, every memory the OS may load, and counts
    that are not multiples of 2, 4 or 8 (a load function that rounds the count fails)."""
    return [
        ('data', 0x08002000, payload(rng, 0x4e0)),
        ('data', 0x0800F4E0, payload(rng, 0xf94)),          # count = 4 mod 8
        ('data', 0x08010474, payload(rng, 0x3)),            # count 3
        ('fill', 0x08010478, 0x1f88, 0x00000000),
        ('data', 0x0803F000, payload(rng, 0xff8)),
        ('data', 0x11800000, payload(rng, 0x810)),
        ('fill', 0x11800810, 0x7f0, 0xdeadbeef),
        ('data', 0x11900000, payload(rng, 0x5)),            # count 5
        ('data', 0x11900005, payload(rng, 0x7ffb)),
        ('data', 0x11A00000, payload(rng, 0xd)),            # count 13, at the entry
        ('data', 0x11A0000D, payload(rng, 0xbff3)),
        ('data', 0x11B00000, payload(rng, 0x1f00)),         # L1 scratch up to the record
        ('empty', 0x84000000, 0),                           # zero-count data block
        ('data', 0x84000000, payload(rng, 0x10000)),
        ('fill', 0x84010000, 0x30000, 0x01020304),
        ('data', 0x8C03D000, payload(rng, 0x1001)),
        ('data', 0x8FED0000, payload(rng, 0x8dc8)),
    ]


def build(blocks):
    """Stream bytes, expected calls and expected memory, counted directly."""
    body, calls, memory = bytearray(), [], []
    psource = 16                                          # after the FIRST|IGNORE header
    calls.append([0, HEADER_BUFFER, 16])
    for kind, target, *rest in blocks:
        if kind == 'fill':
            count, word = rest
            body += ldr.header(ldr.FILL, target, count, word)
            calls.append([psource, HEADER_BUFFER, 16]); psource += 16
            memory.append((target, (struct.pack('<I', word) * (count // 4 + 1))[:count]))
        elif kind == 'flagged':                           # a data block with extra flags (S-05.024)
            flags, data = rest
            body += ldr.header(flags, target, len(data), 0) + data
            calls.append([psource, HEADER_BUFFER, 16]); psource += 16
            calls.append([psource, target, len(data)]); psource += len(data)
            memory.append((target, data))
        elif kind == 'empty':
            body += ldr.header(0, target, 0, 0)
            calls.append([psource, HEADER_BUFFER, 16]); psource += 16
        else:
            data = rest[0]
            body += ldr.header(0, target, len(data), 0) + data
            calls.append([psource, HEADER_BUFFER, 16]); psource += 16
            calls.append([psource, target, len(data)]); psource += len(data)
            memory.append((target, data))
    body += ldr.header(ldr.FINAL, ENTRY, 0, 0)
    calls.append([psource, HEADER_BUFFER, 16]); psource += 16
    image = ldr.header(ldr.FIRST | ldr.IGNORE, ENTRY, 0, len(body)) + bytes(body)
    assert psource == len(image)
    return image, calls, memory


def write(name, image, calls, memory, extra=None):
    with open(os.path.join(OUT, name + '.ldr'), 'wb') as f:
        f.write(image)
    exp = {
        'image': name + '.ldr',
        'length': len(image),
        'sha256': hashlib.sha256(image).hexdigest(),
        'entry': '0x%08x' % ENTRY,
        'header_buffer': '0x%08x' % HEADER_BUFFER,
        'calls': [['0x%x' % s, '0x%08x' % d, '0x%x' % n] for s, d, n in calls],
        'final_psource': '0x%x' % len(image),
        'memory': [{'start': '0x%08x' % a, 'length': '0x%x' % len(b),
                    'sha256': hashlib.sha256(b).hexdigest()} for a, b in memory],
    }
    if extra:
        exp.update(extra)
    with open(os.path.join(OUT, name + '.json'), 'w') as f:
        json.dump(exp, f, indent=1)


def main():
    os.makedirs(OUT, exist_ok=True)
    rng = random.Random(0x5005)
    write('synthetic_os_a', *build(os_blocks(rng)))
    write('synthetic_os_b', *build([('data', ENTRY, payload(rng, 0x40))]))

    # S-05.024 refusal cases: {name: (blocks or raw bytes, expected reason keyword)}
    refusals = {
        'refuse_l2_reserve': ([('data', 0x0803D000, payload(rng, 0x20))], 'reserve'),
        'refuse_ddr_reserve': ([('data', 0x88A34D00, payload(rng, 0x100))], 'reserve'),
        'refuse_staging': ([('fill', 0x8A100000, 0x100, 0)], 'staging'),
    }
    # S-01.017 cases: fixed payloads, so the shared rng sequence, and every vector built before, is unchanged.
    fixed = bytes(range(0x10))
    later = {
        'refuse_record': ([('data', 0x11B01F00, fixed)], 'record'),       # reboot record (S-01.008)
        'refuse_rom_stack': ([('fill', 0x11B01FE0, 0x20, 0)], 'record'),   # L1 scratch above it (S-01.008)
        'refuse_mmr': ([('data', 0x20000020, fixed[:4])], 'space'),       # a system MMR (RCU0_SVECT0)
        'refuse_rom': ([('data', 0x04000000, fixed)], 'space'),            # the boot ROM
        'refuse_unmapped': ([('data', 0x11808000, fixed)], 'space'),       # between L1 data A and B
    }
    # Files written below: refuse_l2_reserve.ldr, refuse_ddr_reserve.ldr, refuse_staging.ldr,
    # refuse_checksum.ldr, refuse_no_final.ldr, refuse_record.ldr, refuse_rom_stack.ldr, refuse_mmr.ldr,
    # refuse_rom.ldr, refuse_unmapped.ldr, refuse_flag_init.ldr, refuse_flag_callback.ldr,
    # refuse_flag_indirect.ldr, refuse_flag_quickboot.ldr, refuse_flag_aux.ldr, refuse_flag_save.ldr.
    cases = []
    for name, (blocks, why) in refusals.items():
        image, _, _ = build(blocks)
        with open(os.path.join(OUT, name + '.ldr'), 'wb') as f:
            f.write(image)
        cases.append({'image': name + '.ldr', 'refuse': True, 'reason': why})
    image, _, _ = build([('data', ENTRY, payload(rng, 0x40))])
    bad = bytearray(image); bad[16 + 2] ^= 0x01                     # second header's checksum byte
    with open(os.path.join(OUT, 'refuse_checksum.ldr'), 'wb') as f:
        f.write(bytes(bad))
    cases.append({'image': 'refuse_checksum.ldr', 'refuse': True, 'reason': 'parse'})
    trunc = image[:-16]                                              # no FINAL block
    with open(os.path.join(OUT, 'refuse_no_final.ldr'), 'wb') as f:
        f.write(trunc)
    cases.append({'image': 'refuse_no_final.ldr', 'refuse': True, 'reason': 'parse'})
    for name, (blocks, why) in later.items():
        image, _, _ = build(blocks)
        with open(os.path.join(OUT, name + '.ldr'), 'wb') as f:
            f.write(image)
        cases.append({'image': name + '.ldr', 'refuse': True, 'reason': why})
    # One data block at the entry carrying a refused flag; destinations are legal, so only the flag
    # criterion refuses these. Fixed payloads.
    flagged = [('init', ldr.INIT), ('callback', ldr.CALLBACK), ('indirect', ldr.INDIRECT),
               ('quickboot', ldr.QUICKBOOT), ('aux', ldr.AUX), ('save', ldr.SAVE)]
    for fname, flag in flagged:
        name = 'refuse_flag_' + fname
        image, _, _ = build([('flagged', ENTRY, flag, fixed)])
        with open(os.path.join(OUT, name + '.ldr'), 'wb') as f:
            f.write(image)
        cases.append({'image': name + '.ldr', 'refuse': True, 'reason': 'flags'})
    # S-01.008: one data block in the ROM's boot reserve in L2, a legal L2 destination otherwise. Fixed
    # payload, appended after the flag cases. Written: refuse_rom_boot_reserve.ldr.
    image, _, _ = build([('data', 0x08001F00, fixed)])
    with open(os.path.join(OUT, 'refuse_rom_boot_reserve.ldr'), 'wb') as f:
        f.write(image)
    cases.append({'image': 'refuse_rom_boot_reserve.ldr', 'refuse': True, 'reason': 'boot reserve'})
    cases.append({'image': 'synthetic_os_a.ldr', 'refuse': False})
    cases.append({'image': 'synthetic_os_b.ldr', 'refuse': False})
    with open(os.path.join(OUT, 'compatibility_cases.json'), 'w') as f:
        json.dump({
                   'cases': cases}, f, indent=1)

    one_cpu_vectors()

    # S-05.027-.029 cases: fixed payloads, no use of the shared rng. Written: factory_good.ldr, factory_ddr_at_reserve_end.ldr, factory_ddr_stack_top.ldr,
    # factory_ddr_just_clear.ldr, factory_l2_above_reserve.ldr, factory_l2_just_clear.ldr,
    # factory_too_short.ldr, factory_first_not_final.ldr, factory_final_argument.ldr,
    # factory_l1_code_not_by_8.ldr, factory_entry_in_ddr.ldr, factory_cases.json.
    factory_cases(fixed)


# ---- the OP-1F (S-05.036-S-05.040) ----

# S-05.040: OS 1.7.3's destinations, start included and end excluded, and the FILL range among them
OS_1F_RANGES = [
    ('l1_code', 0x11A00000, 0x11A0C000, False),
    ('l1_data_a', 0x11800000, 0x11803000, False),
    ('l1_data_b', 0x11900000, 0x11908000, False),
    ('l1_scratch', 0x11B00000, 0x11B01F00, False),
    ('l2', 0x08002000, 0x08002480, False),
    ('l2', 0x08011480, 0x08030000, True),
    ('l2', 0x0803F000, 0x08040000, False),
    ('ddr', 0x84000000, 0x84D262A0, False),
    ('ddr', 0x8C03D002, 0x8FCCDD38, False),
]


def os_1f_stream():
    """A stream with OS 1.7.3's shape (S-05.040): FIRST|IGNORE and FINAL at 0x11A00000 with argument 0, and
    one FILL block over each destination, so that S-05.024's model judges exactly those ranges."""
    body = bytearray()
    for _, s, e, _fill in OS_1F_RANGES:
        body += ldr.header(ldr.FILL, s, e - s, 0)
    body += ldr.header(ldr.FINAL, ENTRY, 0, 0)
    return ldr.header(ldr.FIRST | ldr.IGNORE, ENTRY, 0, len(body)) + bytes(body)


def one_cpu_vectors():
    """os_destinations_1f.json (S-05.040), teardown_1f.json (S-05.037), entry_state_1f.json (S-05.038)."""
    image = os_1f_stream()
    try:
        kernel_model.compatibility_check(image)
        verdict = {'accepted': True}
    except kernel_model.Refused as e:
        verdict = {'accepted': False, 'reason': str(e)}
    assert verdict['accepted'], verdict
    dest = {'document': 'S-05', 'requirement': 'S-05.040',
            'os': 'OP-1F OS 1.7.3', 'blocks': 313, 'bytes': 2079016,
            'flags': ['FIRST', 'IGNORE', 'FILL', 'FINAL'], 'first_target': '0x%08X' % ENTRY,
            'final_target': '0x%08X' % ENTRY, 'final_argument': 0,
            'ranges': [{'memory': m, 'start': '0x%08X' % s, 'end': '0x%08X' % e, 'fill': f}
                       for m, s, e, f in OS_1F_RANGES],
            'display_rows_end': '0x8C03C000',
            'stream': image.hex(),
            'note': 'stream: one FILL block per range in the shape of S-05.040, for S-05.024\'s check',
            'verdict': verdict}
    with open(os.path.join(OUT, 'os_destinations_1f.json'), 'w') as f:
        json.dump(dest, f, indent=1)
        f.write('\n')
    teardown = {
        'document': 'S-05', 'requirement': 'S-05.037',
        'at': 'the kernel call, after the last eMMC command',
        'msi': [{'register': 'MSI_CTL', 'mask': '0x00000010', 'value': '0x00000000',
                 'note': 'bit 4, the interrupt enable, clear'},
                {'register': 'MSI_IMSK', 'mask': '0xFFFFFFFF', 'value': '0x00000000'},
                {'register': 'MSI_ISTAT', 'mask': '0xFFFFFFFF', 'value': '0x00000000',
                 'note': 'the close writes 0xFFFFFFFF (W1C)'},
                {'register': 'MSI_CMD', 'field': 'START', 'value': 0},
                {'item': 'data transfer', 'value': 'none in progress'}],
        'emmc': {'selected': True, 'last_command_complete': True, 'partition': 'user',
                 'bus_width': 'device.emmc_width (4 on the OP-1F)'},
        'sec': {'source_72_msi0': {'pending': False, 'ien': False, 'sen': False, 'provisional': True},
                'enabled_sources': [1], 'enabled_provisional': True,
                'others': {'ien': False, 'sen': False}, 'pending': 'none'},
        'other_registers': 'not fixed',
    }
    with open(os.path.join(OUT, 'teardown_1f.json'), 'w') as f:
        json.dump(teardown, f, indent=1)
        f.write('\n')
    evt = {'EVT2': 0x11A00C18, 'EVT3': 0x11A00B78, 'EVT11': 0x11A01438, 'EVT15': 0x11A000CA}
    for n in list(range(4, 11)) + [12, 13, 14]:
        evt['EVT%d' % n] = 0x11A00664
    entry = {
        'document': 'S-05', 'requirement': 'S-05.038',
        'evt': {k: '0x%08X' % evt[k] for k in sorted(evt, key=lambda x: int(x[3:]))},
        'ipend': '0x8000', 'imask': '0x8800', 'imask_read': '0x881F',
        'ilat_clear_bits': '5-14', 'l1im_ictl': '0x00000207', 'l1dm_dctl': '0x00000207',
        'sp': None, 'fp': None, 'note': 'SP and FP are not fixed (S-05.038, open question 15): recorded, '
                                       'not checked; the rest is S-05.014\'s',
        'entry': 'RCU0_SVECT0 as the kernel left it (S-05.013)',
    }
    with open(os.path.join(OUT, 'entry_state_1f.json'), 'w') as f:
        json.dump(entry, f, indent=1)
        f.write('\n')


FACTORY_PAD = bytes(0x2800)                           # past the OS write's minimum (S-05.029)


def factory_stream(blocks, first=ENTRY, final=ENTRY, final_arg=0):
    """A stream with its FIRST and FINAL targets and its FINAL argument chosen. blocks: (target, payload)."""
    body = bytearray()
    for target, data in blocks:
        body += ldr.header(0, target, len(data), 0) + data
    body += ldr.header(ldr.FINAL, final, 0, final_arg)
    return ldr.header(ldr.FIRST | ldr.IGNORE, first, 0, len(body)) + bytes(body)


def factory_cases(fixed):
    code = bytes(range(8)) * 8                        # 64 bytes: a multiple of 8, at the entry
    data = fixed * 0x100                              # 4 KiB
    pad = (0x11900000, FACTORY_PAD)
    base = [pad, (ENTRY, code)]
    table = [
        # name, stream, os (S-01.017), os_factory, os_factory_seen, the rule that fails it
        ('factory_good', factory_stream(base + [(0x84000000, data)]), True, True, True, None),
        ('factory_ddr_at_reserve_end', factory_stream(base + [(0x88A65000, data)]),
         True, False, False, 'factory_keep_out'),
        ('factory_ddr_stack_top', factory_stream(base + [(0x88A65BF0, fixed)]),
         True, False, False, 'factory_keep_out'),
        ('factory_ddr_just_clear', factory_stream(base + [(0x88A66000, data)]),
         True, True, False, 'destinations_seen'),
        ('factory_l2_above_reserve', factory_stream([pad, (0x0803E000, data[:0x800]), (ENTRY, code)]),
         True, False, False, 'factory_keep_out'),
        ('factory_l2_just_clear', factory_stream([pad, (0x0803F000, data[:0x800]), (ENTRY, code)]),
         True, True, True, None),
        ('factory_too_short', factory_stream([(ENTRY, code)]), True, False, False, 'factory_writer_length'),
        ('factory_first_not_final', factory_stream(base, final=ENTRY + 8),
         True, False, False, 'entry_first_is_final'),
        ('factory_final_argument', factory_stream(base, final_arg=ENTRY),
         True, True, False, 'final_argument_zero'),
        ('factory_l1_code_not_by_8', factory_stream([pad, (ENTRY, code + fixed[:4])]),
         True, True, False, 'l1_code_blocks_by_8'),
        ('factory_entry_in_ddr', factory_stream([pad, (0x84000000, data)], first=0x84000000, final=0x84000000),
         True, True, False, 'entry_seen'),
    ]
    cases = []
    for name, image, os_ok, factory_ok, seen_ok, rule in table:
        with open(os.path.join(OUT, name + '.ldr'), 'wb') as f:
            f.write(image)
        cases.append({'image': name + '.ldr', 'os': os_ok, 'os_factory': factory_ok, 'os_factory_seen': seen_ok,
                      'fails_on': rule})
    with open(os.path.join(OUT, 'factory_cases.json'), 'w') as f:
        json.dump({
                   'cases': cases}, f, indent=1)


if __name__ == '__main__':
    main()
