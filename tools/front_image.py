#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The frozen front's build checks (src/front/, docs/front.md "Changing it").

    front_image.py pin FRONT.elf PIN [--write]
    front_image.py boundary --cc-include DIR DEPFILE...
    front_image.py init FRONT.elf
    front_image.py stack --ld FRONT.ld SU...

pin: the front's pin, SHA-256 over each loaded run of FRONT.elf (address,
length, bytes; tools/mkldr.py front_pin()), against the one kept in PIN.
A difference fails the build: the front changed. --write records the new
pin, which only the maintainer's sign-off allows.

boundary: every file the front's objects were compiled from (their make
dependency files, written with -MD so that system headers are listed) is
the front's own (src/front/), a register definition
(src/include/waffle/drivers/), or a header of the compiler's own include
directory DIR ($(CROSS_CC) -print-file-name=include); a path that leaves
the tree or any other absolute path fails. And no source under src/ outside
src/front/ includes a front header but src/front/fr_handoff.h, whatever the
include's form: quoted or angled, with or without front/, any path that
resolves into src/front/ against the including file's directory, one of its
parents or src/include, or that names a front header found nowhere else.
The front's own link, from its objects and libgcc alone and with
--orphan-handling=error, fails on any other symbol or unplaced section.

init: S-01.010, nothing outside L1 is loaded while the INIT stage runs.
From _fr_init, every relocation of the code and data it reaches in
.fr_l1code and .fr_l1data (followed through both, symbol to symbol) must
resolve into L1 (0x11800000-0x11808000, 0x11900000-0x11908000,
0x11A00000-0x11A0C000) or the MMR spaces. Needs FRONT.elf linked with
relocations kept (-q).

stack: from GCC's -fstack-usage files of the front's C objects, every frame
is static (no alloca or VLA) and at most FRAME_LIMIT bytes, and twice the
sum of all frames fits front.ld's FR_STACK. A call graph from the
relocations is not sound, since the front calls through its ops structs and
callbacks, so the bound takes every function on one chain: twice, since the
PSoC link's greeting re-enters its own wait (fr_psoc.c, held to one level by
p->greeting), so a chain may hold a function twice.

Python 3, standard library only.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elf32  # noqa: E402
import layout  # noqa: E402
import mkldr  # noqa: E402

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALLOWED = ('src/front/', 'src/include/waffle/drivers/')
FRONT_DIR = 'src/front/'
SHARED = 'src/front/fr_handoff.h'
INCLUDE_ROOTS = ('src/include',)

# S-01.012: the L1 spaces the INIT stage may use; layout.py's MMR spaces.
L1 = ((0x11800000, 0x11808000), (0x11900000, 0x11908000), (0x11A00000, 0x11A0C000))
MMR = layout.MMR
INIT_ENTRY = '_fr_init'
INIT_SECTIONS = ('.fr_l1code', '.fr_l1data')

FRAME_LIMIT = 1024

STT_OBJECT, STT_FUNC, STT_SECTION, STT_FILE = 1, 2, 3, 4


def pin_text(elf):
    runs = mkldr.front_runs(elf)
    size = sum(len(b) for _, b in runs)
    return '%s %d\n' % (mkldr.front_pin(elf).hex(), size)


def cmd_pin(args):
    write = '--write' in args
    args = [a for a in args if a != '--write']
    if len(args) != 2:
        print(__doc__)
        return 2
    now = pin_text(elf32.read(args[0]))
    if write:
        with open(args[1], 'w') as f:
            f.write(now)
        print('front pin written: %s' % now.strip())
        return 0
    try:
        with open(args[1]) as f:
            kept = f.read()
    except OSError:
        kept = ''
    if kept.split() != now.split():
        sys.stderr.write(
            'front_image: the frozen front changed.\n'
            '  pinned: %s\n  built:  %s\n'
            'The front (src/front/) runs from the image entry to the key read and\n'
            'holds recovery. It changes only with the maintainer\'s sign-off\n'
            '(docs/front.md, "Changing it"); then run make front-pin and commit\n'
            'the new pin.\n'
            % (kept.strip() or '(none)', now.strip()))
        return 1
    print('front pin ok: %s' % now.strip())
    return 0


def options(args, names):
    opts = {}
    args = list(args)
    for k in names:
        if k in args:
            i = args.index(k)
            if i + 1 >= len(args):
                raise SystemExit('front_image: %s needs a value' % k)
            opts[k] = args[i + 1]
            del args[i:i + 2]
    return opts, args


# --- boundary ---------------------------------------------------------------

def deps(path):
    with open(path) as f:
        text = f.read().replace('\\\n', ' ')
    out = []
    for line in text.splitlines():
        if ':' not in line:
            continue
        _, rhs = line.split(':', 1)
        out += rhs.split()
    return out


def under(path, root):
    return path == root or path.startswith(root.rstrip(os.sep) + os.sep)


def dep_failures(depfile, cc_include, top=TOP):
    """The dependencies of one make dependency file that are not the front's."""
    bad = []
    inc = os.path.normpath(cc_include)
    for p in deps(depfile):
        if os.path.isabs(p):
            ap = os.path.normpath(p)
            if not under(ap, inc):
                bad.append('%s: %s (absolute, not the compiler\'s include directory)' % (depfile, p))
            continue
        rel = os.path.normpath(p).replace(os.sep, '/')
        if rel == '..' or rel.startswith('../'):
            bad.append('%s: %s (leaves the tree)' % (depfile, p))
        elif not rel.startswith(ALLOWED):
            bad.append('%s: %s' % (depfile, rel))
    return bad


INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\n]*)[>"]', re.M)


def include_failures(top=TOP):
    """Sources under src/ outside src/front/ that include a front header."""
    front = os.path.join(top, FRONT_DIR)
    front_names = set(n for n in os.listdir(front) if n.endswith('.h')) if os.path.isdir(front) else set()
    shared = os.path.join(top, SHARED)
    roots = [os.path.join(top, r) for r in INCLUDE_ROOTS]
    bad = []
    for root, dirs, files in os.walk(os.path.join(top, 'src')):
        dirs.sort()
        if under(os.path.normpath(root), os.path.normpath(front)):
            continue
        for name in sorted(files):
            if not name.endswith(('.c', '.h', '.S', '.inc')):
                continue
            path = os.path.join(root, name)
            with open(path, errors='replace') as f:
                text = f.read()
            for m in INCLUDE.finditer(text):
                hit = front_target(m.group(2), path, top, roots, front, front_names)
                if hit and hit != shared:
                    bad.append('%s includes %s%s%s' % (os.path.relpath(path, top), m.group(1),
                                                       m.group(2), '>' if m.group(1) == '<' else '"'))
    return bad


def front_target(inc, path, top, roots, front, front_names):
    """The front file an include names, or None. Bases: the including file's
    directory and each parent up to the top (any -I inside the tree), and the
    include roots."""
    bases = []
    d = os.path.dirname(path)
    while under(d, top):
        bases.append(d)
        if d == top:
            break
        d = os.path.dirname(d)
    bases += roots
    elsewhere = False
    for b in bases:
        cand = os.path.normpath(os.path.join(b, inc))
        if not os.path.isfile(cand):
            continue
        if under(cand, os.path.normpath(front)):
            return cand
        elsewhere = True
    base = os.path.basename(inc)
    if not elsewhere and base in front_names:
        return os.path.join(front, base)
    return None


def cmd_boundary(args):
    opts, files = options(args, ('--cc-include',))
    if '--cc-include' not in opts or not files:
        print(__doc__)
        return 2
    bad = []
    for d in files:
        bad += dep_failures(d, opts['--cc-include'])
    bad += include_failures()
    if bad:
        sys.stderr.write('front_image: the front\'s boundary is crossed:\n  ' +
                         '\n  '.join(bad) + '\n')
        return 1
    print('front boundary ok (%d objects)' % len(files))
    return 0


# --- symbols and relocations -------------------------------------------------

def symtab(elf):
    """[(name, value, size, type, shndx)] of the first SHT_SYMTAB, in order."""
    tab = next((s for s in elf.sections if s.type == elf32.SHT_SYMTAB), None)
    out = []
    if tab is None:
        return out
    strtab = elf.sections[tab.link]
    for i in range(tab.size // 16):
        name, value, size, info, _, shndx = struct.unpack_from('<IIIBBH', elf.data, tab.offset + i * 16)
        end = elf.data.index(b'\0', strtab.offset + name)
        out.append((elf.data[strtab.offset + name:end].decode('ascii', 'replace'),
                    value, size, info & 0xF, shndx))
    return out


class Units:
    """The linked code and data cut at symbols: a unit runs from a symbol to
    the next symbol of its section (or the section's end)."""

    def __init__(self, elf):
        self.elf = elf
        self.starts = {}            # section index -> sorted [(addr, name)]
        for name, value, _, typ, shndx in symtab(elf):
            if typ in (STT_SECTION, STT_FILE) or not name or shndx in (elf32.SHN_UNDEF, elf32.SHN_ABS):
                continue
            if shndx >= len(elf.sections) or not elf.sections[shndx].alloc:
                continue
            self.starts.setdefault(shndx, []).append((value, name))
        for v in self.starts.values():
            v.sort()

    def section_at(self, addr):
        for i, s in enumerate(self.elf.sections):
            if s.alloc and s.size and s.addr <= addr < s.end:
                return i
        return None

    def unit(self, addr):
        """(name, lo, hi, section index) of the unit holding addr, or None."""
        i = self.section_at(addr)
        if i is None:
            return None
        s = self.elf.sections[i]
        name, lo, hi = s.name, s.addr, s.end
        for a, n in self.starts.get(i, []):
            if a <= addr:
                name, lo = n, a
            else:
                hi = a
                break
        return name, lo, hi, i


def relocations(elf):
    """{target section index: [(offset, type, symbol index, addend)]}."""
    out = {}
    for rel in elf.sections:
        if rel.type == elf32.SHT_RELA:
            out.setdefault(rel.info, []).extend(elf.relocations(rel))
    return out


def region_name(addr):
    if 0x08000000 <= addr < 0x08100000:
        return 'L2'
    if layout.inside(addr, (layout.DDR,)):
        return 'DDR'
    if layout.inside(addr, (layout.ROM,)):
        return 'the boot ROM'
    return 'outside L1'


def init_failures(elf, entry=INIT_ENTRY, sections=INIT_SECTIONS, allowed=L1):
    """The INIT stage's references that leave L1 (and the MMRs), by symbol."""
    syms = elf.symbols()
    units = Units(elf)
    rels = relocations(elf)
    follow = set(i for i, s in enumerate(elf.sections) if s.name in sections)
    start = elf.symbol(entry)
    if start is None:
        return ['no symbol %s' % entry]
    first = units.unit(start)
    if first is None or first[3] not in follow:
        return ['%s at 0x%08x is not in %s' % (entry, start, ' or '.join(sections))]
    if not any(rels.get(i) for i in follow):
        return ['no relocations in %s: link with --emit-relocs (-q)' % ' or '.join(sections)]
    bad, seen, todo = [], set(), [first]
    while todo:
        name, lo, hi, sec = todo.pop()
        if (sec, lo) in seen:
            continue
        seen.add((sec, lo))
        for off, rtype, si, add in rels.get(sec, []):
            if rtype == 0 or not lo <= off < hi:
                continue
            sname, value, _, shndx = syms[si]
            if shndx == elf32.SHN_UNDEF:
                bad.append('%s refers to undefined %s' % (name, sname))
                continue
            dest = (value + add) & 0xFFFFFFFF
            target = units.unit(dest)
            what = target[0] if target else (sname or '0x%08x' % dest)
            if not (layout.inside(dest, allowed) or layout.inside(dest, (MMR,))):
                bad.append('%s refers to %s at 0x%08x (%s)' % (name, what, dest, region_name(dest)))
            elif target and target[3] in follow:
                todo.append(target)
    return sorted(set(bad))


def cmd_init(args):
    if len(args) != 1:
        print(__doc__)
        return 2
    elf = elf32.read(args[0])
    bad = init_failures(elf)
    if bad:
        sys.stderr.write('front_image: the INIT stage reaches past L1 (S-01.010):\n  ' +
                         '\n  '.join(bad) + '\n')
        return 1
    print('front INIT stage ok: everything it reaches is in L1 or the MMRs')
    return 0


# --- stack -------------------------------------------------------------------

def stack_usage(path):
    """[(where, bytes, qualifiers)] of one GCC -fstack-usage file."""
    out = []
    with open(path) as f:
        for line in f:
            line = line.rstrip('\n')
            if not line:
                continue
            parts = line.split('\t')
            if len(parts) != 3 or not parts[1].isdigit():
                raise SystemExit('front_image: %s: not a stack-usage line: %r' % (path, line))
            out.append((parts[0], int(parts[1]), parts[2]))
    return out


def fr_stack(ld):
    with open(ld) as f:
        m = re.search(r'\bFR_STACK\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;', f.read())
    if not m:
        raise SystemExit('front_image: no FR_STACK in %s' % ld)
    return int(m.group(1), 0)


def stack_failures(frames, stack, frame_limit=FRAME_LIMIT):
    """frames: [(where, bytes, qualifiers)]. Returns (failures, total): total is the sum of all
    frames, and twice it is the bound held against stack."""
    bad = []
    for where, n, qual in frames:
        if qual != 'static':
            bad.append('%s: a %s frame (alloca or VLA)' % (where, qual))
        if n > frame_limit:
            bad.append('%s: a %d-byte frame, the limit is %d' % (where, n, frame_limit))
    total = sum(n for _, n, _ in frames)
    if frame_limit > stack:
        bad.append('the frame limit %d is more than FR_STACK (%d)' % (frame_limit, stack))
    if 2 * total > stack:
        bad.append('twice the sum of all frames, %d, is more than FR_STACK (%d)' % (2 * total, stack))
    return bad, total


def cmd_stack(args):
    opts, files = options(args, ('--ld',))
    if '--ld' not in opts or not files:
        print(__doc__)
        return 2
    frames = []
    for p in files:
        frames += stack_usage(p)
    stack = fr_stack(opts['--ld'])
    bad, total = stack_failures(frames, stack)
    if bad:
        sys.stderr.write('front_image: the front\'s stack:\n  ' + '\n  '.join(bad) + '\n')
        return 1
    print('front stack ok: %d functions, every frame static and at most %d bytes '
          '(largest %d); twice all frames together %d, FR_STACK %d' %
          (len(frames), FRAME_LIMIT, max((n for _, n, _ in frames), default=0), 2 * total, stack))
    return 0


def main(argv):
    cmds = {'pin': cmd_pin, 'boundary': cmd_boundary, 'init': cmd_init, 'stack': cmd_stack}
    if len(argv) < 2 or argv[1] not in cmds:
        print(__doc__)
        return 2
    return cmds[argv[1]](argv[2:])


if __name__ == '__main__':
    sys.exit(main(sys.argv))
