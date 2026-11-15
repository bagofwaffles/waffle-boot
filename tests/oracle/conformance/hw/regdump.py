# SPDX-License-Identifier: GPL-2.0-or-later
"""Register-dump checker with field rules.

A rule table is a tab-separated file with a header row and at least these columns:

    req       the requirement the row checks (printed in failures)
    points    comma-separated observation points the row applies at (names are the table's own)
    register  the register's name (a dump may key values by it)
    address   the register's address (hex with 0x, or decimal)
    field     the field's name
    bits      msb:lsb, or a single bit number
    rule      eq | ne | ge | le | in | any
    value     V for eq/ne/ge/le, A..B for in, ignored for any

Rules, applied to the field's value (bits msb:lsb of the register):
    eq V      the field equals V
    ne V      the field differs from V
    ge V      the field is at least V
    le V      the field is at most V
    in A..B   A <= field <= B
    any       not checked (named so that the table documents the field)

A dump is text with one register per line, `ADDRESS VALUE` or `ADDRESS=VALUE` (hex, `0x` optional; `#`
starts a comment), or a JSON object mapping addresses or register names to values. A register the table
names at a point must be in the dump, unless every rule on it is `any`.

    python3 -I regdump.py TABLE POINT DUMP

Exit status 0 when every rule holds, 1 otherwise. Python 3, standard library only.
"""
import csv
import json
import sys


def num(s):
    s = s.strip()
    return int(s, 16) if s.lower().startswith('0x') else int(s, 10)


def load_table(path):
    with open(path, newline='') as f:
        rows = list(csv.DictReader(f, delimiter='\t'))
    for r in rows:
        r['points'] = r['points'].split(',')
        bits = r['bits'].split(':')
        r['msb'] = int(bits[0])
        r['lsb'] = int(bits[-1])
        r['addr'] = num(r['address'])
    return rows


def points_of(rows):
    """Every observation point the table names, in first-seen order."""
    out = []
    for r in rows:
        for p in r['points']:
            if p not in out:
                out.append(p)
    return out


def rows_at(point, rows):
    return [r for r in rows if point in r['points']]


def field(value, r):
    width = r['msb'] - r['lsb'] + 1
    return (value >> r['lsb']) & ((1 << width) - 1)


def holds(r, v):
    rule, val = r['rule'], r['value']
    if rule == 'any':
        return True
    if rule == 'in':
        lo, hi = val.split('..')
        return num(lo) <= v <= num(hi)
    x = num(val)
    return {'eq': v == x, 'ne': v != x, 'ge': v >= x, 'le': v <= x}[rule]


def parse_dump(text):
    text = text.strip()
    out = {}
    if text.startswith('{'):
        for k, v in json.loads(text).items():
            out[k] = v if isinstance(v, int) else num(str(v))
        return out
    for line in text.splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        k, v = line.replace('=', ' ').split()[:2]
        out[k] = num(v)
    return out


def resolve(dump, r):
    """The dump's value for r's register: by address (any spelling) or by register name."""
    for k, v in dump.items():
        try:
            if num(k if k.lower().startswith('0x') else '0x' + k) == r['addr']:
                return v
        except ValueError:
            pass
        if k == r['register']:
            return v
    return None


def check(point, dump, rows, points=None):
    """Return a list of failure strings (empty when the dump conforms). With `points`, a point outside it
    is an error."""
    if points is not None and point not in points:
        raise ValueError('unknown point %r' % point)
    fails = []
    for r in rows_at(point, rows):
        v = resolve(dump, r)
        if v is None:
            if r['rule'] != 'any':
                fails.append('%s %s.%s: register missing from dump (%s)' % (r['req'], r['register'], r['field'], point))
            continue
        fv = field(v, r)
        if not holds(r, fv):
            fails.append('%s %s.%s [%s] = %#x, rule %s %s (%s)' % (
                r['req'], r['register'], r['field'], r['bits'], fv, r['rule'], r['value'], point))
    return fails


def report(point, dump_path, rows, points=None):
    """The command-line run: print failures and a summary line; return the exit status."""
    with open(dump_path) as f:
        dump = parse_dump(f.read())
    fails = check(point, dump, rows, points)
    for x in fails:
        print('FAIL', x)
    print('%s: %d field rules, %d failed' % (point, len(rows_at(point, rows)), len(fails)))
    return 1 if fails else 0


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    rows = load_table(argv[1])
    return report(argv[2], argv[3], rows, points_of(rows))


if __name__ == '__main__':
    sys.exit(main(sys.argv))
