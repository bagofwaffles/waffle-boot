#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-01 stream checks on a stage's LDR stream: load destinations and block coding (S-01.005, .008, .009,
.010, .017, .020; S-05.027-.029).

    check_stream.py PROFILE STREAM.ldr [--first-stage-stack START:END]

PROFILE is first_stage, bootloader, os, os_factory or os_factory_seen. os_factory adds S-05.027-.029;
os_factory_seen adds the destination, entry and block shapes in memory_map.json's seen_on_a_unit. Prints
one line per rule, PASS or FAIL, and exits 1 on any failure. Also importable:
check(profile, data, first_stage_stack=None) -> list of (rule, ok, detail).
"""
import json
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'conformance'))
from images import ldr as _ldr  # noqa: E402

# The LDR block format, under the names the S-01 checks and tests use.
ldr = types.SimpleNamespace(FINAL=_ldr.FINAL, FIRST=_ldr.FIRST, IGNORE=_ldr.IGNORE, INIT=_ldr.INIT,
                            FILL=_ldr.FILL, LdrError=_ldr.LdrError, build=_ldr.build_ldr,
                            parse=_ldr.parse_blocks, simple_os=_ldr.simple_os,
                            destinations=_ldr.destinations)


def _h(x):
    return int(x, 16)


def load_map():
    with open(os.path.join(HERE, 'memory_map.json')) as f:
        m = json.load(f)
    spaces = {s['name']: (_h(s['start']), _h(s['end'])) for s in m['spaces']}
    return {
        'spaces': spaces,
        'on_chip': [spaces[n] for n in spaces if n != 'ddr'],
        'l1': [spaces[n] for n in ('l1_data_a', 'l1_data_b', 'l1_code', 'l1_scratch')],
        'l1_code': spaces['l1_code'],
        'ddr': spaces['ddr'],
        'rom_boot_reserve': (_h(m['rom_boot_reserve']['start']), _h(m['rom_boot_reserve']['end'])),
        'record_and_rom_stack': (_h(m['record_and_rom_stack']['start']), _h(m['record_and_rom_stack']['end'])),
        'reserve': [(_h(r['start']), _h(r['end'])) for r in m['handover_reserve']],
        'staging': (_h(m['staging']['start']), _h(m['staging']['end'])),
        'bcode': _h(m['spi_master_bcode']['value']),
        'factory_keep_out': [(_h(r['start']), _h(r['end'])) for r in m['factory_load']['keep_out']],
        'factory_seen': [(_h(r['start']), _h(r['end'])) for r in m['factory_load']['seen_on_a_unit']],
        'factory_entry_seen': _h(m['factory_load']['entry_seen_on_a_unit']),
        'factory_min_length': m['factory_load']['writer_min_length'],
    }


def meets(a, b):
    return a[0] < b[1] and b[0] < a[1]


def inside_any(r, spaces):
    return any(s[0] <= r[0] and r[1] <= s[1] for s in spaces)


def fmt(r):
    return '%08x-%08x' % r


def check(profile, data, first_stage_stack=None):
    m = load_map()
    out = []
    try:
        blocks = ldr.parse(data)
    except ldr.LdrError as e:
        return [('parse', False, str(e))]
    dest = ldr.destinations(blocks)

    bad = [fmt(r) for r in dest if not inside_any(r, list(m['spaces'].values()))]
    out.append(('inside_spaces', not bad, ', '.join(bad)))

    bad = [fmt(r) for r in dest if meets(r, m['rom_boot_reserve'])]
    out.append(('rom_boot_reserve', not bad, ', '.join(bad)))               # S-01.008, S-01.017

    bad = [fmt(r) for r in dest if meets(r, m['record_and_rom_stack'])]
    out.append(('record_and_rom_stack', not bad, ', '.join(bad)))           # S-01.008

    if profile in ('first_stage', 'bootloader'):                             # S-01.020
        bad = ['block %d: %x' % (i, b['code'] & 0xF) for i, b in enumerate(blocks)
               if b['code'] & 0xF != m['bcode']]
        out.append(('bcode', not bad, ', '.join(bad)))

    if profile == 'first_stage':                                             # S-01.005
        bad = [fmt(r) for r in dest if not inside_any(r, m['on_chip'])]
        out.append(('on_chip_only', not bad, ', '.join(bad)))

    if profile == 'bootloader':                                              # S-01.010, S-01.009
        inits = [i for i, b in enumerate(blocks) if b['flags'] & ldr.INIT]
        out.append(('one_init_block', len(inits) == 1, 'INIT blocks at %s' % inits))
        if len(inits) == 1:
            k = inits[0]
            pre = ldr.destinations(blocks[:k + 1])
            bad = [fmt(r) for r in pre if not inside_any(r, m['l1'])]
            t = blocks[k]['target']
            ok_t = m['l1_code'][0] <= t < m['l1_code'][1]
            out.append(('init_in_l1', not bad and ok_t,
                        ', '.join(bad) + ('' if ok_t else ' target %08x not in L1 code' % t)))
            bad = [fmt(r) for r in pre if meets(r, m['ddr'])]
            out.append(('init_before_ddr', not bad, ', '.join(bad)))
        if first_stage_stack is not None:
            bad = [fmt(r) for r in dest if meets(r, first_stage_stack)]
            out.append(('spares_first_stage_stack', not bad, ', '.join(bad)))

    if profile in ('os', 'os_factory', 'os_factory_seen'):                   # S-01.017
        bad = [fmt(r) for r in dest for z in m['reserve'] if meets(r, z)]
        out.append(('outside_reserve', not bad, ', '.join(bad)))
        bad = [fmt(r) for r in dest if meets(r, m['staging'])]
        out.append(('outside_staging', not bad, ', '.join(bad)))
        out.append(('fits_staging', len(data) <= m['staging'][1] - m['staging'][0], 'length 0x%x' % len(data)))

    if profile in ('os_factory', 'os_factory_seen'):                         # S-05.027-.029
        bad = [fmt(r) for r in dest for z in m['factory_keep_out'] if meets(r, z)]
        out.append(('factory_keep_out', not bad, ', '.join(bad)))
        first = [b for b in blocks if b['flags'] & ldr.FIRST]
        final = [b for b in blocks if b['flags'] & ldr.FINAL]
        same = bool(first) and bool(final) and first[0]['target'] == final[-1]['target']
        out.append(('entry_first_is_final', same, '' if same else 'FIRST and FINAL targets differ, or one is missing'))
        out.append(('factory_writer_length', len(data) >= m['factory_min_length'], 'length 0x%x' % len(data)))

    if profile == 'os_factory_seen':                                         # S-05.028
        bad = [fmt(r) for r in dest if not inside_any(r, m['factory_seen'])]
        out.append(('destinations_seen', not bad, ', '.join(bad)))
        ok = bool(first) and first[0]['target'] == m['factory_entry_seen']
        out.append(('entry_seen', ok, '' if ok else 'entry is not 0x%08x' % m['factory_entry_seen']))
        bad = ['%08x' % b['argument'] for b in final if b['argument'] != 0]
        out.append(('final_argument_zero', not bad, ', '.join(bad)))
        lo, hi = m['l1_code']
        bad = ['%08x+%x' % (b['target'], b['count']) for b in blocks
               if b['count'] and not b['flags'] & (ldr.IGNORE | ldr.FILL) and lo <= b['target'] < hi
               and (b['target'] % 8 or b['count'] % 8)]
        out.append(('l1_code_blocks_by_8', not bad, ', '.join(bad)))
    return out


def main(argv):
    if len(argv) < 3 or argv[1] not in ('first_stage', 'bootloader', 'os', 'os_factory', 'os_factory_seen'):
        print(__doc__)
        return 2
    fss = None
    if '--first-stage-stack' in argv:
        a, b = argv[argv.index('--first-stage-stack') + 1].split(':')
        fss = (int(a, 16), int(b, 16))
    with open(argv[2], 'rb') as f:
        res = check(argv[1], f.read(), fss)
    for rule, ok, detail in res:
        print('%s %s %s' % ('PASS' if ok else 'FAIL', rule, detail))
    return 0 if all(ok for _, ok, _ in res) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
