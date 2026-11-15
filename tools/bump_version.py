#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Raise the bootloader's patch version, so that builds on a unit tell apart.

    bump_version.py              raise OPXY_BL_VERSION_PATCH by one, print the new version
    bump_version.py --show       print the version, change nothing
    bump_version.py --set N      set the patch to N, print the new version
    bump_version.py --header H   work on H instead of the board header

Only the patch's #define line changes; every other byte of the header stays.
The version comes from the tree, so the same tree still gives the same image.
Python 3, standard library only.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(os.path.dirname(HERE), 'src', 'include', 'waffle', 'board', 'opxy_services.h')
PLACES = ('MAJOR', 'MINOR', 'PATCH')
MAX = 0xFFFF            # a field of the descriptor's version


class Failure(Exception):
    pass


def find(text, place):
    """(line match, value match, value) of the one '#define OPXY_BL_VERSION_<place> <n>u' line."""
    name = 'OPXY_BL_VERSION_' + place
    lines = list(re.finditer(r'^[ \t]*#[ \t]*define[ \t]+%s\b.*$' % name, text, re.M))
    if len(lines) != 1:
        raise Failure('found %d #define lines for %s, not exactly one' % (len(lines), name))
    m = re.match(r'^#define[ \t]+%s[ \t]+(0|[1-9][0-9]*)u[ \t]*\r?$' % name, lines[0].group(0))
    if not m:
        raise Failure('%s is not a plain decimal with the u suffix: %r' % (name, lines[0].group(0).rstrip('\r')))
    return lines[0], m, int(m.group(1))


def current(text):
    return tuple(find(text, p)[2] for p in PLACES)


def set_patch(text, n):
    """text with the patch's value replaced by n, nothing else changed."""
    line, m, _ = find(text, 'PATCH')
    new = line.group(0)[:m.start(1)] + str(n) + line.group(0)[m.end(1):]
    return text[:line.start()] + new + text[line.end():]


def main(argv=None):
    ap = argparse.ArgumentParser(description='Raise or show the bootloader version.')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--show', action='store_true', help='print the version and change nothing')
    g.add_argument('--set', type=int, metavar='N', help='set the patch to N')
    ap.add_argument('--header', default=HEADER, help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    try:
        with open(a.header, 'rb') as f:
            raw = f.read()
        text = raw.decode('utf-8')
        ver = current(text)
        if a.show:
            print('%d.%d.%d' % ver)
            return 0
        n = ver[2] + 1 if a.set is None else a.set
        if not 0 <= n <= MAX:
            raise Failure('the patch must be 0 to %d, not %d' % (MAX, n))
        out = set_patch(text, n).encode('utf-8')
        with open(a.header, 'wb') as f:
            f.write(out)
        print('%d.%d.%d' % current(out.decode('utf-8')))
        return 0
    except (Failure, OSError, UnicodeDecodeError) as e:
        print('bump_version: %s: %s' % (a.header, e), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
