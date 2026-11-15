#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-01 layout checks on an implementation's declared memory layout (S-01.011, .013-.016, .019; S-05.008).

    check_layout.py LAYOUT.json

LAYOUT.json comes from the implementation's build (its link map):

    {"ranges": [{"name": "...", "start": "0x...", "end": "0x...",
                 "kind": "code" | "data" | "stack" | "heap" | "held_buffer",
                 "handover": true | false,
                 "role": "kernel_stack" | "config_block" | "header_buffer"}]}

`handover` marks every range the handover path uses. `role` is optional; each role appears once. Prints
PASS/FAIL per rule and exits 1 on any failure. Rule `heap_clear` (every heap range ends below the staging
area and meets no held-mode buffer) is a self-check: it prints INFO-PASS/INFO-FAIL and never sets the exit
status. Also importable: check(layout) -> list of (rule, ok, detail).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from check_stream import load_map, meets, inside_any, fmt  # noqa: E402

OWN = ('code', 'data', 'stack', 'heap')
INFORMATIVE = ('heap_clear',)


def _h(x):
    return int(x, 16) if isinstance(x, str) else x


DISK_BYTES = 454656 * 512        # S-12.001: the RAM disk, held in held-mode buffers (S-01.016)
KERNEL_STACK_MIN = 4096          # S-05.008


def handover_stack(rs, reserve):
    """S-05.008: problems with the one kernel stack (size, and its path from its top down to the bottom of
    its reserve range meeting the configuration block or header buffer). Empty with no handover range."""
    if not any(r.get('handover') for r in rs):
        return []
    role = lambda n: [r for r in rs if r.get('role') == n]
    stacks, blocks = role('kernel_stack'), role('config_block') + role('header_buffer')
    if len(stacks) != 1 or len(role('config_block')) != 1 or len(role('header_buffer')) != 1:
        return ['one kernel_stack, config_block and header_buffer range each']
    st = stacks[0]
    bad = []
    if st['end'] - st['start'] < KERNEL_STACK_MIN:
        bad.append('%s is %d bytes' % (st['name'], st['end'] - st['start']))
    part = [p for p in reserve if p[0] <= st['start'] and st['end'] <= p[1]]
    if not part:
        return bad + ['%s outside the reserve' % st['name']]
    path = (part[0][0], st['end'])
    bad += ['%s in the stack\'s path' % b['name'] for b in blocks if meets((b['start'], b['end']), path)]
    return bad


def check(layout):
    m = load_map()
    rs = [dict(r, start=_h(r['start']), end=_h(r['end'])) for r in layout['ranges']]
    rng = lambda r: (r['start'], r['end'])
    own = [r for r in rs if r['kind'] in OWN]
    held = [r for r in rs if r['kind'] == 'held_buffer']
    with open(os.path.join(HERE, 'memory_map.json')) as f:
        mm = json.load(f)
    rec, dr = mm['reboot_record'], mm['display_record']
    record = (int(rec['start'], 16), int(rec['end'], 16))
    out = []

    bad = [r['name'] for r in rs if r.get('handover') and not inside_any(rng(r), m['reserve'])]
    out.append(('handover_in_reserve', not bad, ', '.join(bad)))                       # S-01.013
    bad = [r['name'] for r in rs if r.get('handover') and meets(rng(r), m['staging'])]
    out.append(('handover_not_staging', not bad, ', '.join(bad)))                      # S-01.013
    bad = [r['name'] for r in rs if r.get('handover') and r['kind'] == 'heap']
    out.append(('handover_no_heap', not bad, ', '.join(bad)))                          # S-01.013
    bad = handover_stack(rs, m['reserve'])
    out.append(('handover_stack', not bad, ', '.join(bad)))                            # S-05.008
    bad = [r['name'] for r in own if meets(rng(r), m['staging'])]
    out.append(('staging_clear', not bad, ', '.join(bad)))                             # S-01.014
    heaps = [r for r in rs if r['kind'] == 'heap']
    bad = [r['name'] for r in heaps if r['end'] > m['staging'][0]
           or any(meets(rng(r), rng(h)) for h in held)]
    out.append(('heap_clear', not bad, ', '.join(bad)))       # self-check, INFO only
    bad = ['%s in %s' % (r['name'], h['name']) for r in own for h in held if meets(rng(r), rng(h))]
    out.append(('held_buffers_clear', not bad, ', '.join(bad)))                        # S-01.016
    cover = sorted(rng(h) for h in held)
    total, top = 0, 0
    for a, b in cover:                     # bytes covered by the union of the held-mode buffers
        a = max(a, top)
        if b > a:
            total, top = total + b - a, b
    out.append(('held_room', total >= DISK_BYTES, '%d of %d bytes' % (total, DISK_BYTES)))  # S-01.016
    bad = [r['name'] for r in rs if meets(rng(r), record)]
    out.append(('record_clear', not bad, ', '.join(bad)))                              # S-01.007
    display_record = (int(dr['start'], 16), int(dr['end'], 16))
    bad = [r['name'] for r in own if meets(rng(r), display_record)]
    out.append(('display_record_clear', not bad, ', '.join(bad)))                      # S-01.019
    bad = [r['name'] for r in rs if not inside_any(rng(r), list(m['spaces'].values()))]
    out.append(('inside_spaces', not bad, ', '.join(bad)))                             # S-01.012
    return out


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1]) as f:
        res = check(json.load(f))
    for rule, ok, detail in res:
        verdict = 'PASS' if ok else 'FAIL'
        if rule in INFORMATIVE:
            verdict = 'INFO-' + verdict
        print('%s %s %s' % (verdict, rule, detail))
    return 0 if all(ok for rule, ok, _ in res if rule not in INFORMATIVE) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
