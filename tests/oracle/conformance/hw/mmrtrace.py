# SPDX-License-Identifier: GPL-2.0-or-later
"""The MMR-trace text format of this package, and order primitives over it.

One event per line, in program order, for one CPU:

    [@CYCLE] w ADDRESS VALUE      MMR write   (also accepted: `w ADDRESS/4=VALUE`, `w ADDRESS=VALUE`)
    [@CYCLE] r ADDRESS VALUE      MMR read and the value returned
    [@CYCLE] m ADDRESS            a data access to memory
    [@CYCLE] ret                  the traced stage returns to its caller (for example to the boot ROM)

ADDRESS and VALUE are hex (`0x` optional). @CYCLE is the core-clock cycle count at the event; it is optional
and a rule that needs it reports "not checked" when it is missing. A `/N` after the address (the access
width in bytes) is accepted and ignored. Other lines are ignored, so a log with interleaved text can be
read as it is.

An event is a dict {'cyc': int | None, 'k': 'w' | 'r' | 'm' | 'ret', 'a': int | None, 'v': int | None}.
"""
import re

LINE = re.compile(r'^(?:@(\d+)\s+)?([rwm]|ret)\b\s*([0-9a-fA-Fx]+)?(?:/\d+)?(?:[=\s]+([0-9a-fA-Fx]+))?')


def parse_line(line):
    """One event, or None for a line that is not an event."""
    m = LINE.match(line.strip())
    if not m:
        return None
    cyc, kind, a, v = m.groups()
    return {'cyc': int(cyc) if cyc else None, 'k': kind,
            'a': int(a, 16) if a else None, 'v': int(v, 16) if v else None}


def iter_events(lines):
    """Events from an iterable of lines (a file object works; nothing is held in memory)."""
    for line in lines:
        e = parse_line(line)
        if e is not None:
            yield e


def parse(text):
    """All events of a trace given as one string."""
    return list(iter_events(text.splitlines()))


def format_event(e):
    """The canonical text line of one event."""
    pre = '@%d ' % e['cyc'] if e.get('cyc') is not None else ''
    if e['k'] == 'ret':
        return pre + 'ret'
    if e['k'] == 'm':
        return pre + 'm %08x' % e['a']
    return pre + '%s %08x %x' % (e['k'], e['a'], e['v'])


def first(ev, pred, start=0):
    """Index of the first event at or after `start` for which pred holds, or None."""
    for i in range(start, len(ev)):
        if pred(ev[i]):
            return i
    return None


def last(ev, pred):
    """Index of the last event for which pred holds, or None."""
    for i in range(len(ev) - 1, -1, -1):
        if pred(ev[i]):
            return i
    return None


def w(addr, mask=None, zero=False):
    """Predicate: a write to addr; with mask, one that sets every mask bit (zero=True: clears them all)."""
    def p(e):
        if e['k'] != 'w' or e['a'] != addr:
            return False
        if mask is None:
            return True
        return (e['v'] & mask) == 0 if zero else (e['v'] & mask) == mask
    return p


def duration(ev, i, j, cycles, label, out):
    """Require at least `cycles` core-clock cycles from event i to event j. Appends to out['fail'] or, when
    either event has no cycle count, to out['unchecked']."""
    a, b = ev[i]['cyc'], ev[j]['cyc']
    if a is None or b is None:
        out['unchecked'].append(label)
    elif b - a < cycles:
        out['fail'].append('%s: %d CCLK cycles, at least %d required' % (label, b - a, cycles))
