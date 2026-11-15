# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared helpers for the conformance harness's Python drivers (README.md).

A driver builds its inputs with tests/oracle/conformance, sends them to one of the
host programs h_<name> as command lines (hlib.h), and checks every result
line against the oracle. Each test method names the scenario or requirement
it checks with @scenario or @requirement; coverage.py reads those marks.

Python 3, standard library only.
"""

import importlib.util
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
ORACLE = os.path.join(TOP, 'tests', 'oracle')
BUILD = os.environ.get('WB_CONFORMANCE_BUILD', os.path.join(TOP, 'build', 'conformance'))

if ORACLE not in sys.path:
    sys.path.insert(0, ORACLE)


# --- The host programs -------------------------------------------------------

def run(name, commands):
    """Sends `commands` (strings, one per line) to h_<name> and returns its output lines, one per
    command. Fails on a nonzero exit, an "error" line or a line count that does not match."""
    path = os.path.join(BUILD, 'h_' + name)
    if not os.path.exists(path):
        raise AssertionError(path + ' is not built (make -f tests/conformance/Makefile)')
    p = subprocess.run([path], input='\n'.join(commands) + '\n', capture_output=True, text=True,
                       check=False)
    out = p.stdout.splitlines()
    errors = [x for x in out if x.startswith('error ')]
    if p.returncode != 0 or errors:
        raise AssertionError('h_%s exited %d: %s %s' % (name, p.returncode, errors[:3], p.stderr[:200]))
    if len(out) != len(commands):
        raise AssertionError('h_%s gave %d lines for %d commands' % (name, len(out), len(commands)))
    return out


def hx(b):
    """Bytes in the programs' notation: hex, '-' when empty."""
    return bytes(b).hex() if b else '-'


def unhex(s):
    return b'' if s == '-' else bytes.fromhex(s)


def spec_module(spec_dir, module):
    """Loads tests/oracle/<spec_dir>/<module>.py (a model or generator) under a unique name."""
    path = os.path.join(ORACLE, spec_dir, module + '.py')
    name = 'spec_%s_%s' % (spec_dir.split('-')[1], module)
    if name in sys.modules:
        return sys.modules[name]
    s = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(s)
    sys.modules[name] = m
    s.loader.exec_module(m)
    return m


def spec_path(*parts):
    return os.path.join(ORACLE, *parts)


def tsv(*parts):
    """Rows of a table under tests/oracle/ (tab-separated, '#' comments, a header line) as dicts. A table
    whose header is a comment ('# a<TAB>b') has it taken from the last tabbed comment before the data."""
    rows, header, commented = [], None, None
    with open(spec_path(*parts), encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('#'):
                if header is None and '\t' in line:
                    commented = line.lstrip('#').strip().split('\t')
                continue
            if not line:
                continue
            cells = line.split('\t')
            if header is None and commented is not None and len(commented) == len(cells):
                header = commented
            if header is None:
                header = cells
            else:
                rows.append(dict(zip(header, cells)))
    return rows


# --- Marks read by coverage.py ----------------------------------------------

def _mark(attr, ids, part):
    def wrap(f):
        marks = getattr(f, attr, [])
        marks.extend((i, part) for i in ids)
        setattr(f, attr, marks)
        return f
    return wrap


def scenario(*ids, part='logic'):
    """The test checks scenario `LABEL/ID` (for example 'S-04/R-ROWS'). part 'logic': the host-testable
    slice of the scenario's assertions (the rule the scenario observes, without the emulator run around
    it); 'all': every assertion of the scenario."""
    return _mark('wb_scenarios', ids, part)


def requirement(*ids):
    """The test checks requirements `ids` (for example 'S-04.070') outside any scenario."""
    return _mark('wb_requirements', ids, 'logic')


EXPECTED = []


def expected_failure(sid, note):
    """The implementation disagrees with the oracle here; recorded, not fixed (README.md)."""
    def wrap(f):
        EXPECTED.append((sid, note))
        f = unittest.expectedFailure(f)
        f.wb_expected = (sid, note)
        return f
    return wrap
