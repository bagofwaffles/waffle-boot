#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the oracle's vector generators (tests/oracle/S-*/), all at once: their vectors are tracked. No
generator reads or writes what another writes. The unit tests' vector headers are not tracked: the
Makefile builds them under the build directory from tests/vectors/*/gen_*.py.

    python3 -I -B tools/regen.py          # rewrite the generated files
    python3 -I -B tools/regen.py --check  # fail if a tracked generated file would change

--check runs the generators on a copy of the tracked files of tests/ and tools/ (every file under
them when there is no git, as in a CI copy) in a temporary directory, and compares the copy with the tree:
a file changed or removed, or a new file, is reported and fails the check. It also fails when a header the
tests' generators write is tracked again. The tree itself is not touched. Run from the repository root."""
import concurrent.futures
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (script, arguments that make it write). Order does not matter.
GENERATORS = [
    ('tests/oracle/S-02-spi-flash/gen_vectors.py', []),
    ('tests/oracle/S-03-emmc/gen_vectors.py', []),
    ('tests/oracle/S-04-records/gen_vectors_1f.py', []),
    ('tests/oracle/S-05-os-handoff/make_vectors.py', []),
    ('tests/oracle/S-05-os-handoff/gen_landing_1f.py', []),
    ('tests/oracle/S-06-framing/gen_vectors.py', ['--write']),
    ('tests/oracle/S-07-psoc/gen_s07.py', ['--write']),
    ('tests/oracle/S-08-nordic/gen_vectors.py', []),
    ('tests/oracle/S-09-bridge/make_vectors.py', []),
    ('tests/oracle/S-10a-usb-msc/gen_vectors.py', []),
    ('tests/oracle/S-10b-mtp/gen_vectors.py', []),
    ('tests/oracle/S-10c-host-commands/gen_vectors.py', ['--write']),
    ('tests/oracle/S-11-packages/gen_vectors.py', []),
    ('tests/oracle/S-12-filesystems/gen_vectors.py', []),
    ('tests/oracle/S-13b-buses-and-pins/gen_vectors.py', []),
    ('tests/oracle/S-13c-display/gen_vectors.py', []),
    ('tests/oracle/S-14-logging/gen_vectors.py', ['--write']),
    ('tests/oracle/S-15-menus/gen_vectors.py', []),
    ('tests/oracle/S-17-revisions/gen_vectors.py', []),
    ('tests/oracle/S-18-devices/gen_vectors.py', []),
]

COPIED = ('tests', 'tools')
# Never tracked; written by tests/conformance/coverage.py, which tools/conformance.sh runs alongside --check.
UNTRACKED = {os.path.join('tests', 'conformance', 'COVERAGE.md')}


def run_one(root, script, args):
    return subprocess.run([sys.executable, '-I', '-B', script] + args, cwd=root,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def run_all(root):
    """Run every generator with root as the working directory; False if one fails."""
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 1) as pool:
        results = list(pool.map(lambda g: run_one(root, *g), GENERATORS))
    failed = [(g[0], r) for g, r in zip(GENERATORS, results) if r.returncode != 0]
    for script, r in failed:
        print('regen: %s failed:\n%s' % (script, r.stdout), file=sys.stderr)
    return not failed


def tracked_test_headers(files):
    """The C headers under tests/ that a generator wrote (its banner's second line says so): the build
    makes these, so none may be tracked."""
    found = []
    for rel in files:
        if not (rel.startswith('tests/') and rel.endswith('.h')):
            continue
        try:
            with open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace') as f:
                f.readline()
                if f.readline().startswith('/* Generated'):
                    found.append(rel)
        except OSError:
            pass
    return found


def tracked(root):
    """The tracked files of COPIED; every file under them in a tree without git (a CI copy)."""
    try:
        r = subprocess.run(['git', 'ls-files', '-z', '--'] + list(COPIED), cwd=root,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    except OSError:
        r = None
    if r is None or r.returncode != 0:
        return sorted(files_under(root))
    return [p for p in r.stdout.decode('utf-8', 'surrogateescape').split('\0') if p]


def files_under(root):
    found = set()
    for top in COPIED:
        for d, dirs, names in os.walk(os.path.join(root, top)):
            dirs[:] = [x for x in dirs if x != '__pycache__']
            for n in names:
                found.add(os.path.relpath(os.path.join(d, n), root))
    return found - UNTRACKED


def check():
    files = tracked(ROOT)
    stray = tracked_test_headers(files)
    for rel in stray:
        print('regen: tracked, but the build generates it: %s' % rel)
    if stray:
        print('regen: remove it with git rm; the Makefile writes the tests\' headers under the build '
              'directory', file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix='wb-regen-') as tmp:
        for rel in files:
            src = os.path.join(ROOT, rel)
            if os.path.islink(src) or not os.path.isfile(src):
                continue
            dst = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
        before = files_under(tmp)
        if not run_all(tmp):
            return 1
        after = files_under(tmp)
        changed = sorted(rel for rel in before & after
                         if not filecmp.cmp(os.path.join(ROOT, rel), os.path.join(tmp, rel), shallow=False))
        gone = sorted(before - after)
        new = sorted(after - before)
    for label, rels in (('would change', changed), ('would be removed', gone), ('would be created', new)):
        for rel in rels:
            print('regen: %s: %s' % (label, rel))
    if changed or gone or new:
        print('regen: generated files are stale; run make regen and commit the result', file=sys.stderr)
        return 1
    print('regen: %d generators, every generated file current' % len(GENERATORS))
    return 0


def main(argv):
    if argv == ['--check']:
        return check()
    if argv:
        print(__doc__, file=sys.stderr)
        return 2
    if not run_all(ROOT):
        return 1
    print('regen: %d generators run' % len(GENERATORS))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
