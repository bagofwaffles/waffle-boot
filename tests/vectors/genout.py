# SPDX-License-Identifier: GPL-2.0-or-later
"""Output for the unit tests' vector generators (tests/vectors/*/gen_*.py), which the Makefile runs to
write their C headers under the build directory; nothing they write is tracked.

    gen_<x>.py --out DIR [--deps FILE]

Each header goes to DIR, and is replaced only when its text changes: make runs a generator once for each
header it writes, possibly at the same time (-j), and the runs must not touch each other's files. --deps
writes a make dependency file: every header written depends on each file under the repository that the
generator read or imported and each directory it listed, so a changed, added or removed input rebuilds it.
Import this module before anything the generator reads, so that it sees every read."""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
USAGE = 'usage: %s --out DIR [--deps FILE]'

_read = set()
_written = []
_out = None
_deps = None


def _path(p):
    if isinstance(p, int) or p is None:
        return None
    return os.path.abspath(os.fsdecode(p))


def _audit(event, args):
    if event == 'open':
        path, mode, flags = args
        if mode is None:
            writes = flags & (os.O_WRONLY | os.O_RDWR)
        else:
            writes = any(c in mode for c in 'wax+')
        if not writes and _path(path):
            _read.add(_path(path))
    elif event in ('os.listdir', 'os.scandir'):
        if args and _path(args[0]):
            _read.add(_path(args[0]))


sys.addaudithook(_audit)


def _options(argv):
    global _out, _deps
    it = iter(argv)
    for a in it:
        if a in ('--out', '--deps'):
            v = next(it, None)
            if v is None:
                break
            if a == '--out':
                _out = v
            else:
                _deps = v
        else:
            break
    else:
        if _out is not None:
            return
    sys.exit(USAGE % os.path.relpath(os.path.abspath(sys.argv[0]), ROOT))


def write(name, text):
    """Write the header name into the output directory, unless it already holds text."""
    if _out is None:
        _options(sys.argv[1:])
    os.makedirs(_out, exist_ok=True)
    path = os.path.join(_out, name)
    _written.append(path)
    try:
        with open(path, encoding='utf-8', newline='') as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    _replace(path, text)


def _replace(path, text):
    tmp = '%s.%d.tmp' % (path, os.getpid())
    with open(tmp, 'w', encoding='utf-8', newline='') as f:
        f.write(text)
    os.replace(tmp, path)


def _inputs():
    out = os.path.abspath(_out) + os.sep
    found = set(_read)
    for m in list(sys.modules.values()):
        f = getattr(m, '__file__', None)
        if f:
            found.add(os.path.abspath(f))
    keep = set()
    for p in found:
        if not p.startswith(ROOT + os.sep) or p.startswith(out) or '__pycache__' in p.split(os.sep):
            continue
        keep.add(os.path.relpath(p, ROOT))
    return sorted(keep)


def finish():
    """Write the dependency file, when --deps asked for one. Call once every header is written."""
    if _deps is None:
        return
    inputs = _inputs()
    lines = ['%s: \\' % ' '.join(_written)]
    lines += [' %s \\' % p for p in inputs[:-1]] + [' ' + inputs[-1], '']
    lines += ['%s:' % p for p in inputs]
    os.makedirs(os.path.dirname(_deps) or '.', exist_ok=True)
    _replace(_deps, '\n'.join(lines) + '\n')

