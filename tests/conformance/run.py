# SPDX-License-Identifier: GPL-2.0-or-later
"""Runs the conformance harness: every tests/conformance/test_*.py.

    make -f tests/conformance/Makefile
    python3 -I -B tests/conformance/run.py [-v] [pattern]

Each test module runs in a process of its own, all at once; the report is
the modules' output in order, then the totals. WB_CONFORMANCE_BUILD names
the directory of the host programs (default build/conformance). Exits
nonzero on a failure or an unexpected success.
"""

import concurrent.futures
import fnmatch
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import harness  # noqa: E402,F401  (puts tests/oracle/ on sys.path)


class Stream:
    """What unittest.TextTestResult writes to, kept as text."""

    def __init__(self):
        self.parts = []

    def write(self, s):
        self.parts.append(s)

    def writeln(self, s=''):
        self.parts.append(s + '\n')

    def flush(self):
        pass

    def text(self):
        return ''.join(self.parts)


def one(module, verbosity, out):
    """Run one test module; write its results to the file out as JSON."""
    suite = unittest.TestLoader().loadTestsFromName(module)
    progress = Stream()
    result = unittest.TextTestResult(progress, True, verbosity)
    suite.run(result)
    details = Stream()
    result.stream = details
    result.printErrors()
    # printErrors() first ends the progress line; main() does that once for all.
    details = details.text()
    if details.startswith('\n'):
        details = details[1:]
    expected = []
    for test, _ in result.expectedFailures:
        sid, note = getattr(getattr(test, test._testMethodName), 'wb_expected', ('?', ''))
        expected.append((test.id(), sid, note))
    with open(out, 'w') as f:
        json.dump({'run': result.testsRun, 'failures': len(result.failures),
                   'errors': len(result.errors),
                   'unexpected': len(result.unexpectedSuccesses),
                   'expected': expected,
                   'skipped': [(t.id(), why) for t, why in result.skipped],
                   'progress': progress.text(), 'details': details}, f)
    return 0


def spawn(module, verbosity, tmp):
    out = os.path.join(tmp, module + '.json')
    r = subprocess.run([sys.executable, '-I', '-B', os.path.abspath(__file__),
                        '--one', module, str(verbosity), out],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        with open(out) as f:
            res = json.load(f)
    except (OSError, ValueError):
        # The module did not load or the process died: one error.
        res = {'run': 0, 'failures': 0, 'errors': 1, 'unexpected': 0, 'expected': [],
               'skipped': [], 'progress': 'E' if verbosity < 2 else module + ' ... ERROR\n',
               'details': '%s\nERROR: %s (exit status %d)\n%s\n' % ('=' * 70, module, r.returncode,
                                                                    '-' * 70)}
    res['output'] = r.stdout
    return res


def main(argv):
    if argv[:1] == ['--one']:
        return one(argv[1], int(argv[2]), argv[3])
    verbosity = 2 if '-v' in argv else 1
    args = [a for a in argv if a != '-v']
    pattern = args[0] if args else 'test_*.py'
    modules = sorted(f[:-3] for f in os.listdir(HERE)
                     if f.endswith('.py') and fnmatch.fnmatch(f, pattern))
    start = time.perf_counter()
    with tempfile.TemporaryDirectory(prefix='wb-conformance-') as tmp:
        with concurrent.futures.ThreadPoolExecutor(max(1, len(modules))) as pool:
            results = list(pool.map(lambda m: spawn(m, verbosity, tmp), modules))
    took = time.perf_counter() - start

    out = sys.stdout
    for r in results:
        out.write(r['output'])
        out.write(r['progress'])
    out.write('\n')
    for r in results:
        out.write(r['details'])
    run = sum(r['run'] for r in results)
    failures = sum(r['failures'] for r in results)
    errors = sum(r['errors'] for r in results)
    unexpected = sum(r['unexpected'] for r in results)
    expected = [e for r in results for e in r['expected']]
    skipped = [s for r in results for s in r['skipped']]
    out.write('-' * 70 + '\n')
    out.write('Ran %d test%s in %.3fs\n\n' % (run, '' if run == 1 else 's', took))
    ok = not (failures or errors or unexpected)
    infos = ['%s=%d' % (k, v) for k, v in (('failures', failures), ('errors', errors),
                                          ('skipped', len(skipped)),
                                          ('expected failures', len(expected)),
                                          ('unexpected successes', unexpected)) if v]
    out.write('%s%s\n' % ('OK' if ok else 'FAILED', ' (%s)' % ', '.join(infos) if infos else ''))
    if expected:
        print('expected failures (the implementation disagrees with the oracle):')
        for tid, sid, note in expected:
            print('  %s  %s: %s' % (tid, sid, note))
    if skipped:
        print('skipped:')
        for tid, why in skipped:
            print('  %s: %s' % (tid, why))
    print('harness: %d run, %d failed, %d errors, %d expected failures, %d skipped'
          % (run, failures, errors, len(expected), len(skipped)))
    return 0 if ok and run else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
