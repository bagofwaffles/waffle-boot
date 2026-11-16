#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The layering of src/, checked on its includes.

    layering.py [TOP]

Three layers and the test OS, by where a file lives:

  core    the files directly under src/ and the headers directly under
          src/include/waffle/: hardware-independent, behind ops structs
  board   src/board/ and src/include/waffle/board/, and
          src/include/waffle/testos.h, which declares the board's test-OS
          hooks
  driver  src/drivers/ and src/include/waffle/drivers/
  testos  src/testos/: the test OS's own code (wiki/Test-OS.md)

and the neutral headers of NEUTRAL, which every layer may include and
which include no header of the three layers or of the test OS.

The core includes no board, driver or test OS file, and a driver includes no
core or board header. The board and the test OS may include anything. An
include is resolved as the compiler does, against the including file's
directory (quoted only), then src/include; one that resolves to neither (a
system or vendored header) is not checked. src/front/ has its own check (tools/front_image.py
boundary), and src/glue/ and src/third_party/ are not checked.

ALLOWED lists the crossings that remain, each with its reason; one that no
longer occurs fails too, so that the list only shrinks. Exit status 1 on a
failure.

Python 3, standard library only.
"""
import os
import re
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\n]*)[>"]', re.M)
SUFFIXES = ('.c', '.h', '.S', '.inc')

# (including file, included file), both from the top.
ALLOWED = {
    # The TinyUSB class adapter's state holds the USB driver's struct.
    ('src/include/waffle/s10a_usb.h', 'src/include/waffle/drivers/bf70x_usb.h'),
    # The panel driver's adapter fills the core's S-13c bus struct.
    ('src/include/waffle/drivers/bf70x_display.h', 'src/include/waffle/s13c_panel.h'),
    # The test OS's hooks, until the test OS leaves the core.
    ('src/drivers/bf70x/spi.c', 'src/include/waffle/testos.h'),
    ('src/include/waffle/boot.h', 'src/include/waffle/testos.h'),
    ('src/include/waffle/s09_slave.h', 'src/include/waffle/testos.h'),
    ('src/s07_psoc.c', 'src/include/waffle/testos.h'),
    ('src/s09_bridge.c', 'src/include/waffle/testos.h'),
    ('src/s09_master.c', 'src/include/waffle/testos.h'),
    ('src/s13c_panel.c', 'src/include/waffle/testos.h'),
}

# Headers for every layer: the shared byte and string helpers.
NEUTRAL = ('src/include/waffle/bytes.h',)

# What each layer may not include.
FORBIDDEN = {'core': ('board', 'driver', 'testos'), 'driver': ('core', 'board'),
             'neutral': ('core', 'board', 'driver', 'testos')}


def layer(rel):
    """The layer of a path from the top, or None outside the layers."""
    d, name = os.path.split(rel)
    if rel in NEUTRAL:
        return 'neutral'
    if rel == 'src/include/waffle/testos.h':
        return 'board'
    if d in ('src', 'src/include/waffle'):
        return 'core'
    if d.startswith(('src/board', 'src/include/waffle/board')):
        return 'board'
    if d.startswith(('src/drivers', 'src/include/waffle/drivers')):
        return 'driver'
    if d.startswith('src/testos'):
        return 'testos'
    return None


def resolve(inc, path, quoted, top):
    """The file an include names, from the top, or None outside the tree."""
    bases = ([os.path.dirname(path)] if quoted else []) + [os.path.join(top, 'src', 'include')]
    for b in bases:
        cand = os.path.normpath(os.path.join(b, inc))
        if os.path.isfile(cand):
            return os.path.relpath(cand, top).replace(os.sep, '/')
    return None


def crossings(top=TOP):
    """[(file, included file, file's layer, included layer)] across layers."""
    out = []
    for root, dirs, files in os.walk(os.path.join(top, 'src')):
        dirs.sort()
        for name in sorted(files):
            if not name.endswith(SUFFIXES):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, top).replace(os.sep, '/')
            mine = layer(rel)
            if mine not in FORBIDDEN:
                continue
            with open(path, errors='replace') as f:
                text = f.read()
            for m in INCLUDE.finditer(text):
                hit = resolve(m.group(2), path, m.group(1) == '"', top)
                theirs = layer(hit) if hit else None
                if theirs in FORBIDDEN[mine]:
                    out.append((rel, hit, mine, theirs))
    return out


def failures(top=TOP, allowed=None):
    allowed = ALLOWED if allowed is None else allowed
    found = crossings(top)
    bad = ['%s (%s) includes %s (%s)' % (f, fl, i, il)
           for f, i, fl, il in found if (f, i) not in allowed]
    seen = set(c[:2] for c in found)
    bad += ['%s no longer includes %s: remove it from ALLOWED' % a
            for a in sorted(allowed) if a not in seen]
    return bad


def main(argv):
    top = argv[0] if argv else TOP
    bad = failures(top)
    if bad:
        sys.stderr.write('layering: a layer is crossed:\n  ' + '\n  '.join(bad) + '\n')
        return 1
    print('layering ok (%d allowed)' % len(ALLOWED))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
