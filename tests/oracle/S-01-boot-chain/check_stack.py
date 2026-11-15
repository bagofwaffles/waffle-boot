#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-01 stack check: the stack a stage uses on the SP it was entered with (S-01.007).

    check_stack.py DECLARATION.json

The declaration describes the code that runs on the entry stack, before the stage moves SP to a stack of its
own or returns: one entry, and for each routine of that code the bytes its frame takes at its deepest point
and the routines it calls. JSON:

    {"entry": "a", "budget": "0xe8",                  # budget optional: default memory_map.json's
     "frames": {"a": 24, "b": 40},                    # bytes below the routine's own entry SP, saved
                                                      # registers and saved return address included
     "calls": {"a": ["b"]},                           # direct calls; a routine with none may be omitted
     "indirect": ["b"]}                               # routines that make a call whose target is not
                                                      # known statically (optional)

Rules: every routine reachable from the entry has a frame (`frames_known`); none is reachable from itself
(`no_recursion`); none makes an indirect call (`no_indirect`); the deepest path, the largest sum of frames
along any call path from the entry, is at most the budget (`within_budget`). Prints one line per rule, PASS
or FAIL, and exits 1 on any failure. Also importable: check(decl) -> list of (rule, ok, detail).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def default_budget():
    with open(os.path.join(HERE, 'memory_map.json')) as f:
        return int(json.load(f)['rom']['entry_stack_budget'], 16)


def boot_call_sp(sp):
    """S-01.022: the SP at a bootloader's own boot-entry call. Returns the failed conditions (empty: passes)."""
    with open(os.path.join(HERE, 'memory_map.json')) as f:
        b = json.load(f)['boot_call_stack']
    lo, floor, hi = (int(b[k], 16) for k in ('start', 'sp_floor', 'end'))
    bad = []
    if sp % 4:
        bad.append('word_aligned')
    if not floor <= sp <= hi:
        bad.append('at_or_above_floor' if sp < floor else 'within_spared_range')
    return bad


def _int(v):
    return int(v, 0) if isinstance(v, str) else int(v)


def check(decl):
    entry = decl['entry']
    frames = {k: _int(v) for k, v in decl.get('frames', {}).items()}
    calls = decl.get('calls', {})
    indirect = set(decl.get('indirect', []))
    budget = _int(decl['budget']) if 'budget' in decl else default_budget()

    reach, todo = set(), [entry]
    while todo:
        f = todo.pop()
        if f not in reach:
            reach.add(f)
            todo.extend(calls.get(f, []))
    out = []
    missing = sorted(f for f in reach if f not in frames)
    out.append(('frames_known', not missing, ', '.join(missing)))

    cyc = []
    state = {}

    def visit(f, path):
        state[f] = 1
        for g in calls.get(f, []):
            if state.get(g) == 1:
                cyc.append(' -> '.join(path + [g]))
            elif g not in state:
                visit(g, path + [g])
        state[f] = 2
    visit(entry, [entry])
    out.append(('no_recursion', not cyc, '; '.join(cyc)))

    ind = sorted(reach & indirect)
    out.append(('no_indirect', not ind, ', '.join(ind)))

    if missing or cyc:
        out.append(('within_budget', False, 'depth not computable'))
        return out
    memo = {}

    def depth(f):
        if f not in memo:
            memo[f] = frames[f] + max([depth(g) for g in calls.get(f, [])] or [0])
        return memo[f]
    d = depth(entry)
    out.append(('within_budget', d <= budget, 'depth 0x%x, budget 0x%x' % (d, budget)))
    return out


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1]) as f:
        res = check(json.load(f))
    for rule, ok, detail in res:
        print('%s %s %s' % ('PASS' if ok else 'FAIL', rule, detail))
    return 0 if all(ok for _, ok, _ in res) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
