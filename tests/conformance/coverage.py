# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes tests/conformance/COVERAGE.md: every scenario ID of the index scenarios.tsv and what checks it.

    python3 -I -B tests/conformance/coverage.py [-o FILE]   # write COVERAGE.md (or FILE)
    python3 -I -B tests/conformance/coverage.py --check     # check the marks, write nothing
    python3 -I -B tests/conformance/coverage.py --write-index   # rebuild scenarios.tsv (needs spec/)

COVERAGE.md is generated, not tracked. Both forms exit 1 when a test's mark names a scenario that is not
in the index. scenarios.tsv is tracked: per row the document label, the scenario ID, its kind and its
requirements, and a row of kind s16 per S-16 entry with the requirements it names. Where spec/ is present,
--write-index rebuilds the index from it and --check also fails when the index does not match.

For each scenario:
- logic slice: `harness` when a test here marks it (harness.scenario); otherwise `unit tests` when
  tests/unit/ names one of its requirements; otherwise the status.tsv entry (W1: waits on the src/wire_*.c
  adapters; D1: waits on the BF70x drivers), or `-`;
- full run: `harness` when a test marks it with part 'all'; `hardware` or `OS` per the index's kind,
  otherwise `emulator`; status.tsv may override.

S-16 entries have no scenarios. A section after the scenarios lists each entry a test here marks
(harness.requirement('S-16.NNN')), and each entry whose target requirements belong to a scenario a test
here marks.

Python 3, standard library only.
"""

import fnmatch
import glob
import importlib
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import harness  # noqa: E402

TOP = harness.TOP
SPEC = os.path.join(TOP, 'spec')
INDEX = os.path.join(HERE, 'scenarios.tsv')
INDEX_HEADER = ('spec', 'id', 'kind', 'requirements')
OUT = os.path.join(HERE, 'COVERAGE.md')

ID = r'[A-Z][A-Za-z0-9]*(?:-[A-Za-z0-9]+)+'
HEADING = re.compile(r'^(#{2,3})\s+(%s(?:\s+and\s+%s)*)(?=\s*$|\s+[—–(-])' % (ID, ID))
SECTION = re.compile(r'^(#{1,3})\s')
TABLE_KEYS = ('ID', 'Id', 'Scenario', 'Step')
REQ = re.compile(r'(?:(S-\d\d[a-c]?)\.|(?<=[\s(,;/])\.)(\d{3})'
                 r'(?:\s*[–-]\s*(?:(S-\d\d[a-c]?)\.|\.)?(\d{3})(?!\d))?')


def requirements(text, doc):
    """The S-xx.NNN IDs in text, with '.NNN' after an S-xx.NNN in the same document and ranges
    ('.041–.049', 'S-02.001–S-02.007') expanded. `doc` is the document a bare '.NNN' belongs to first."""
    out = []
    for mt in REQ.finditer(text):
        d1, n1, d2, n2 = mt.groups()
        doc = d1 or doc
        if doc is None:
            continue
        lo = int(n1)
        hi = int(n2) if n2 and (d2 is None or d2 == doc) else lo
        if hi < lo or hi - lo > 200:
            hi = lo
        out.extend('%s.%03d' % (doc, n) for n in range(lo, hi + 1))
    return out


def spec_of(path):
    return re.match(r'(S-\d\d[a-c]?)', os.path.basename(os.path.dirname(path))).group(1)


def spec_scenarios():
    """[(spec, id, requirements, kind)] from spec/S-*/scenario*.md: a heading `## ID` or `### ID`, or a
    table row whose first header cell is one of TABLE_KEYS; kind 'emulator', 'OS' or 'hardware'."""
    found = []
    for path in sorted(glob.glob(os.path.join(SPEC, 'S-*', 'scenario*.md'))):
        spec = spec_of(path)
        with open(path, encoding='utf-8') as f:
            lines = f.read().split('\n')
        hardware = os_run = False
        i = 0
        while i < len(lines):
            line = lines[i]
            sm = SECTION.match(line)
            if sm and len(sm.group(1)) == 2:
                hardware = 'Hardware' in line or '(HW)' in line
                os_run = 'OS running' in line
            hm = HEADING.match(line)
            if hm:
                level = len(hm.group(1))
                j = i + 1
                while j < len(lines) and not (SECTION.match(lines[j]) and
                                              len(SECTION.match(lines[j]).group(1)) <= level):
                    j += 1
                body = '\n'.join(lines[i:j])
                kind = 'hardware' if hardware or hm.group(2).startswith('HW-') else \
                    'OS' if os_run else 'emulator'
                for sid in re.split(r'\s+and\s+', hm.group(2)):
                    found.append((spec, sid, requirements(body, spec), kind))
                i += 1
                continue
            if line.startswith('|') and i + 1 < len(lines) and re.match(r'^\|\s*-{3}', lines[i + 1]):
                head = [c.strip() for c in line.strip().strip('|').split('|')]
                i += 2
                while i < len(lines) and lines[i].startswith('|'):
                    if head[0] in TABLE_KEYS:
                        cells = [c.strip() for c in lines[i].strip().strip('|').split('|')]
                        sid = cells[0].strip('`')
                        if re.fullmatch(r'[A-Za-z][A-Za-z0-9-]*', sid):
                            row = lines[i]
                            kind = 'hardware' if hardware or '(HW)' in row else \
                                'OS' if os_run or '(OS)' in row else 'emulator'
                            found.append((spec, sid, requirements(row, spec), kind))
                    i += 1
                continue
            i += 1
    return found


def unit_requirements():
    refs = set()
    for path in glob.glob(os.path.join(TOP, 'tests', 'unit', '*.[ch]')):
        with open(path, encoding='utf-8', errors='replace') as f:
            refs.update(requirements(f.read(), None))
    return refs


def harness_marks():
    """{scenario key: part}, {requirement: [test ids, an expected failure's marked so]}, {scenario key:
    [expected-failure notes]} from the test modules' marks."""
    marks, reqs, expected = {}, {}, {}
    for path in sorted(glob.glob(os.path.join(HERE, 'test_*.py'))):
        mod = importlib.import_module(os.path.basename(path)[:-3])
        for name in dir(mod):
            cls = getattr(mod, name)
            if not (isinstance(cls, type) and issubclass(cls, unittest.TestCase)):
                continue
            for attr in dir(cls):
                f = getattr(cls, attr)
                for sid, part in getattr(f, 'wb_scenarios', []):
                    if marks.get(sid) != 'all':
                        marks[sid] = part
                for rid, _ in getattr(f, 'wb_requirements', []):
                    reqs.setdefault(rid, []).append('%s.%s%s' % (cls.__name__, attr, ' (expected failure)'
                                                                 if hasattr(f, 'wb_expected') else ''))
                if hasattr(f, 'wb_expected'):
                    sid, note = f.wb_expected
                    expected.setdefault(sid, []).append(note)
    return marks, reqs, expected


def status_rules():
    """status.tsv: pattern (fnmatch on LABEL/ID), logic (W1 the wire adapters, D1 the BF70x drivers, or -),
    full (emulator, hardware, OS or -) and a note."""
    rules = []
    for r in harness.tsv(os.path.join(HERE, 'status.tsv')):
        rules.append(r)
    return rules


S16 = os.path.join(SPEC, 'S-16-quirks.md')
ENTRY = re.compile(r'^### (S-16\.\d{3}) ')


def spec_s16_entries():
    """[(id, target requirements)] of the S-16 entries in spec/: the requirements after a Test line's arrow."""
    with open(S16, encoding='utf-8') as f:
        lines = f.read().split('\n')
    out = []
    for i, line in enumerate(lines):
        m = ENTRY.match(line)
        if not m:
            continue
        targets = []
        for body in lines[i + 1:]:
            if body.startswith('#'):
                break
            if body.startswith('- Test:') and '→' in body:
                targets = requirements(body.split('→', 1)[1], None)
        out.append((m.group(1), targets))
    return out


def spec_index():
    """The index rows spec/ gives: one per scenario, then one per S-16 entry (kind s16)."""
    rows = [(s, i, k, ','.join(r)) for s, i, r, k in spec_scenarios()]
    rows += [('S-16', e, 's16', ','.join(t)) for e, t in spec_s16_entries()]
    return rows


def index_text(rows):
    return '\n'.join('\t'.join(r) for r in [INDEX_HEADER] + rows) + '\n'


def read_index():
    with open(INDEX, encoding='utf-8') as f:
        lines = f.read().rstrip('\n').split('\n')
    assert tuple(lines[0].split('\t')) == INDEX_HEADER, INDEX
    return [tuple(x.split('\t')) for x in lines[1:]]


def scenarios():
    """[(spec, id, requirements, kind)] from the index, in document order."""
    return [(s, i, [x for x in r.split(',') if x], k) for s, i, k, r in read_index() if k != 's16']


def s16_entries():
    """[(id, target requirements)] from the index."""
    return [(i, [x for x in r.split(',') if x]) for s, i, k, r in read_index() if k == 's16']


def s16_table():
    """[(id, harness tests, targets in harness-driven scenarios)] for the entries either column names."""
    marks, reqs, _ = harness_marks()
    driven = set()
    for spec, sid, rq, _ in scenarios():
        if '%s/%s' % (spec, sid) in marks:
            driven.update(rq)
    rows = []
    for eid, targets in s16_entries():
        tests = reqs.get(eid, [])
        via = sorted({t for t in targets if t in driven})
        if tests or via:
            rows.append((eid, tests, via))
    return rows


def short(reqs):
    if not reqs:
        return '-'
    out, last = [], None
    for r in dict.fromkeys(reqs):
        doc, n = r.rsplit('.', 1)
        out.append(r if doc != last else '.' + n)
        last = doc
    return ', '.join(out[:6]) + (', …' if len(out) > 6 else '')


LOGIC_TEXT = {'harness': 'harness', 'W1': 'waits on W1', 'D1': 'waits on D1', '-': '-'}


def table():
    found = scenarios()
    unit = unit_requirements()
    marks, _, expected = harness_marks()
    rules = status_rules()
    keys = ['%s/%s' % (s, i) for s, i, _, _ in found]
    unknown = sorted((set(marks) | set(expected)) - set(keys))
    if unknown:
        raise SystemExit('marks name scenarios that are not in the index: %s' % ', '.join(unknown))
    rows = []
    for (spec, sid, reqs, kind), key in zip(found, keys):
        rule = next((r for r in rules if fnmatch.fnmatchcase(key, r['pattern'])), None)
        full = {'hardware': 'hardware', 'OS': 'emulator (OS running)'}.get(kind, 'emulator')
        if rule and rule['full'] != '-':
            full = rule['full']
        note = rule['note'] if rule and rule['note'] != '-' else ''
        if key in expected:
            note = '; '.join(['expected failure: ' + n for n in expected[key]] + ([note] if note else []))
        hit = [r for r in dict.fromkeys(reqs) if r in unit]
        if key in marks:
            logic = 'harness'
            if marks[key] == 'all':
                full = 'harness'
        elif rule and rule['logic'] in ('W1', 'D1'):
            logic = LOGIC_TEXT[rule['logic']]
            if hit:
                logic += '; unit tests (%d of %d)' % (len(hit), len(set(reqs)))
        elif hit:
            logic = 'unit tests (%d of %d)' % (len(hit), len(set(reqs)))
        else:
            logic = '-'
        rows.append((spec, sid, short(reqs), logic, full, note))
    return rows


def render(rows):
    out = ['<!-- Generated by tests/conformance/coverage.py from scenarios.tsv; do not edit. -->',
           '# Conformance coverage', '',
           'Every scenario ID of `tests/conformance/scenarios.tsv`, keyed by its document label. Generated by',
           '`python3 -I -B tests/conformance/coverage.py`, not tracked; CI publishes it with each run.', '',
           '- **Logic slice**: the rule the scenario observes, applied to the S-xx logic alone. `harness`: a',
           '  test in this directory drives `src/` with the oracles of `tests/oracle/`;',
           '  `unit tests (k of n)`: only `tests/unit/` names k of the scenario\'s n requirements;',
           '  `waits on W1` / `waits on D1`:',
           '  needs the vendored-library adapters (`src/wire_*.c`) or the BF70x drivers; `-`: nothing',
           '  host-testable checks it.',
           '- **Full run**: what the whole scenario needs: `emulator`, `emulator (OS running)`, `hardware`, or',
           '  `harness` when the harness checks every assertion.',
           '- **Note**: `expected failure:` the implementation disagrees with the oracle there; the',
           '  harness test is marked so and `src/` is left as it is.', '']
    total = len(rows)
    n_h = sum(1 for r in rows if r[3] == 'harness')
    n_u = sum(1 for r in rows if r[3].startswith('unit'))
    n_w = sum(1 for r in rows if r[3].startswith('waits'))
    n_hw = sum(1 for r in rows if r[4] == 'hardware')
    out += ['| Scenarios | Logic: harness | Logic: unit tests only | Logic: waits on W1/D1 | Logic: none | '
            'Full run: hardware |', '| --- | --- | --- | --- | --- | --- |',
            '| %d | %d | %d | %d | %d | %d |' % (total, n_h, n_u, n_w, total - n_h - n_u - n_w, n_hw), '']
    spec = None
    for k, (s, sid, reqs, logic, full, note) in enumerate(rows):
        if s != spec:
            out += ['## %s' % s, '', '| Scenario | Requirements | Logic slice | Full run | Note |',
                    '| --- | --- | --- | --- | --- |']
            spec = s
        out.append('| %s | %s | %s | %s | %s |' % (sid, reqs, logic, full, note))
        if k + 1 == len(rows) or rows[k + 1][0] != s:
            out.append('')
    out += ['## S-16', '', 'No scenarios. The entries a test here marks (**Harness tests**), and the entries whose',
            'Test line names requirements of scenarios the harness drives (**Via scenarios**; the scenario\'s',
            'test need not check the entry\'s decision).', '',
            '| Entry | Harness tests | Via scenarios |', '| --- | --- | --- |']
    for eid, tests, via in s16_table():
        out.append('| %s | %s | %s |' % (eid, ', '.join(tests) or '-', short(via)))
    out.append('')
    return '\n'.join(out)


def check_index():
    """Where spec/ is present: the index must be what spec/ gives. Returns a message or None."""
    if not os.path.isdir(SPEC):
        return None
    if read_index() != spec_index():
        return 'coverage: scenarios.tsv does not match spec/; run coverage.py --write-index'
    return None


def main(argv):
    if '--write-index' in argv:
        if not os.path.isdir(SPEC):
            raise SystemExit('coverage: --write-index needs spec/')
        rows = spec_index()
        with open(INDEX, 'w', encoding='utf-8') as f:
            f.write(index_text(rows))
        print('coverage: %d rows written to %s' % (len(rows), os.path.relpath(INDEX, TOP)))
        return 0
    rows = table()
    text = render(rows)
    if '--check' in argv:
        msg = check_index()
        if msg:
            print(msg, file=sys.stderr)
            return 1
        print('coverage: %d scenarios; every mark names one in the index%s'
              % (len(rows), '' if os.path.isdir(SPEC) else ' (no spec/: the index is not compared)'))
        return 0
    out = argv[argv.index('-o') + 1] if '-o' in argv else OUT
    with open(out, 'w', encoding='utf-8') as f:
        f.write(text)
    if '-v' in argv:
        counts = {}
        for r in table():
            counts[r[0]] = counts.get(r[0], 0) + 1
        for p, n in counts.items():
            print('%4d %s' % (n, p))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
