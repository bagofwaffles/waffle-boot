# SPDX-License-Identifier: GPL-2.0-or-later
"""Masked comparison of line-oriented traces (panel-bus traces, MMR traces, serial transcripts).

Two runs are compared after masking:

    drop      regexes: a line matching one is left out
    windows   (t0, t1) pairs: a line whose leading time field lies in [t0, t1) is left out
    subs      (regex, replacement) pairs applied to every line kept
    times     False (default): the leading time field is removed before the comparison, so only order and
              content count; True: times must match as well

`diff` returns a list of human-readable differences (empty when the masked traces are equal), at most
`limit` of them.
"""
import difflib
import re


def _time(line):
    head = line.split(None, 1)[0] if line.strip() else ''
    try:
        return int(head)
    except ValueError:
        return None


def masked(lines, drop=(), windows=(), subs=(), times=False):
    drop = [re.compile(p) for p in drop]
    subs = [(re.compile(p), r) for p, r in subs]
    out = []
    for line in lines:
        line = line.rstrip('\n')
        if not line.strip() or any(p.search(line) for p in drop):
            continue
        t = _time(line)
        if t is not None and any(a <= t < b for a, b in windows):
            continue
        for p, r in subs:
            line = p.sub(r, line)
        if not times and t is not None:
            line = line.split(None, 1)[1] if len(line.split(None, 1)) > 1 else ''
        out.append(line)
    return out


def diff(a, b, drop=(), windows=(), subs=(), times=False, limit=10):
    """Differences between traces a and b (lists of lines) after masking. Windows apply to both traces;
    pass `windows=(wa, wb)` as a dict {'a': [...], 'b': [...]} to mask each run in its own time base."""
    if isinstance(windows, dict):
        wa, wb = windows.get('a', ()), windows.get('b', ())
    else:
        wa = wb = windows
    ma = masked(a, drop, wa, subs, times)
    mb = masked(b, drop, wb, subs, times)
    out = []
    sm = difflib.SequenceMatcher(None, ma, mb, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            continue
        out.append('%s a[%d:%d] %r -> b[%d:%d] %r' % (tag, i1, i2, ma[i1:i1 + 2], j1, j2, mb[j1:j1 + 2]))
        if len(out) >= limit:
            break
    return out
