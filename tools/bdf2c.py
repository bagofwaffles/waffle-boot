#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes src/display_font.c, the display's font (S-15.003), from the vendored Spleen 8x16 BDF file.

    python3 -I -B tools/bdf2c.py                  # check src/display_font.c is current
    python3 -I -B tools/bdf2c.py --write          # rewrite it
    python3 -I -B tools/bdf2c.py --front [--write]  # the same for the front's copy, src/front/fr_font.c

Run from the repository root. Standard library only. wb_display_font holds the glyphs 0x20..0x7E, each 16 rows
of one byte, bit 7 the leftmost pixel, row 0 the top of the 16-pixel cell (12 above the baseline, 4 below).
Spleen has no bold, so the title bar's font is derived from it: each glyph widened to 9 columns (side stems 3
wide, a column clear after it) and heightened to 17 rows (capitals 14 tall), each row of 16 bits, bit 9 the
leftmost pixel. TITLE_FIXES draws by hand the glyphs that rule does not serve."""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDF = os.path.join(ROOT, 'src', 'third_party', 'spleen', 'spleen-8x16.bdf')
OUT = os.path.join(ROOT, 'src', 'display_font.c')
FRONT_OUT = os.path.join(ROOT, 'src', 'front', 'fr_font.c')
W, H, FIRST, LAST = 8, 16, 0x20, 0x7E
TITLE_W, TITLE_H = 10, 17
# A title glyph's rows drawn by hand, in the widened 16-row cell, from the given row.
TITLE_FIXES = {
    'I': (2, ['.#######.'] + ['...###...'] * 8 + ['.#######.']),
    'T': (2, ['#########'] + ['...###...'] * 9),
}


def glyphs(path):
    """{code: [16 row bytes]} for the codes FIRST..LAST, placed in the font's cell."""
    with open(path, encoding='latin-1') as f:
        lines = f.read().splitlines()
    ascent = descent = None
    out = {}
    i = 0
    while i < len(lines):
        p = lines[i].split()
        i += 1
        if not p:
            continue
        if p[0] == 'FONT_ASCENT':
            ascent = int(p[1])
        elif p[0] == 'FONT_DESCENT':
            descent = int(p[1])
        elif p[0] == 'STARTCHAR':
            code = bbx = None
            while True:
                q = lines[i].split()
                i += 1
                if q[0] == 'ENCODING':
                    code = int(q[1])
                elif q[0] == 'BBX':
                    bbx = [int(v) for v in q[1:]]
                elif q[0] == 'BITMAP':
                    bw, bh, bx, by = bbx
                    rows = [int(lines[i + k], 16) for k in range(bh)]
                    nbits = len(lines[i]) * 4
                    i += bh
                    if FIRST <= code <= LAST:
                        cell = [0] * H
                        top = ascent - (bh + by)
                        for k, r in enumerate(rows):
                            # leftmost pixel of the BBX first, shifted by its x offset
                            v = (r >> (nbits - bw)) << (W - bw) >> bx if bw <= W else 0
                            if 0 <= top + k < H:
                                cell[top + k] = v & 0xFF
                        out[code] = cell
                elif q[0] == 'ENDCHAR':
                    break
    if ascent is None or descent is None or ascent + descent != H:
        sys.exit('bdf2c: the font is not %d pixels tall' % H)
    missing = [c for c in range(FIRST, LAST + 1) if c not in out]
    if missing:
        sys.exit('bdf2c: no glyph for %s' % ', '.join('0x%02X' % c for c in missing))
    return out


def widen(row):
    """A row of 8 pixels to 9 bits: columns 1 and 5 repeated, a stem of 2 elsewhere widened to 3."""
    b = [(row >> (W - 1 - i)) & 1 for i in range(7)]
    n = b[:2] + [b[1]] + b[2:6] + [b[5]] + b[6:]
    out = n[:]
    i = 0
    while i < 9:
        if not n[i]:
            i += 1
            continue
        j = i
        while j + 1 < 9 and n[j + 1]:
            j += 1
        if j - i == 1 and j + 2 < 9 and not n[j + 2] and (j + 3 >= 9 or not n[j + 3]):
            out[j + 1] = 1
        i = j + 1
    return tuple(out)


def has_bar(row):
    """Whether a row holds a horizontal stroke."""
    return '11111' in ''.join(map(str, row))


def grow(rows, want):
    """rows to want rows: each horizontal stroke repeated once, then the longest stem."""
    rows = list(rows)
    i = 0
    while len(rows) < want and i < len(rows):
        i += 2 if has_bar(rows[i]) and not rows.insert(i, rows[i]) else 1
    while len(rows) < want:
        runs = []
        i = 0
        while i < len(rows):
            j = i
            while j + 1 < len(rows) and rows[j + 1] == rows[i]:
                j += 1
            if any(rows[i]):
                runs.append((j - i + 1, i))
            i = j + 1
        at = max(runs)[1] if runs else 0
        rows.insert(at, rows[at])
    return rows


def title_font(g):
    """{code: [TITLE_H rows]} of the glyphs g: rows 2-4 to 4 rows, the x-height's 5-11 to 10, rows 12-14 kept."""
    out = {}
    for c, cell in g.items():
        rows = [widen(r) for r in cell]
        fix = TITLE_FIXES.get(chr(c))
        if fix:
            for k, r in enumerate(fix[1]):
                rows[fix[0] + k] = tuple(1 if v == '#' else 0 for v in r)
        tall = grow(rows[2:5], 4) + grow(rows[5:12], 10) + rows[12:15]
        out[c] = [int(''.join(map(str, r)), 2) << 1 for r in tall]
    return out


def name(c):
    ch = chr(c)
    if ch == "'":
        return 'quote'
    if ch == '\\':
        return 'backslash'
    return "'%s'" % ch


def source(g):
    return text(g, ['/* SPDX-License-Identifier: GPL-2.0-or-later */',
                    '/*',
                    ' * The display font (S-15.003): Spleen 8x16 2.2.0, Copyright (c) 2018-2026',
                    ' * Frederic Cambus, BSD-2-Clause (src/third_party/spleen/LICENSE). Generated',
                    ' * by tools/bdf2c.py from src/third_party/spleen/spleen-8x16.bdf; do not edit.',
                    ' * Each glyph is 16 rows of 8 pixels, bit 7 the leftmost; rows 12 to 15',
                    ' * are below the baseline. wb_display_title_font, the title bar\'s, is derived',
                    ' * from it by the same tool: each glyph 17 rows of 10 pixels, bit 9 the leftmost.',
                    ' */',
                    '#include "waffle/display.h"'],
                'const uint8_t wb_display_font[WB_FONT_COUNT][WB_FONT_H]',
                'const uint16_t wb_display_title_font[WB_FONT_COUNT][WB_TITLE_H]')


def front_source(g):
    """The front's own copy of the same tables (src/front/fr_font.h)."""
    return text(g, ['/* SPDX-License-Identifier: GPL-2.0-or-later */',
                    '/*',
                    ' * The front\'s copy of the display font (S-15.003): Spleen 8x16 2.2.0,',
                    ' * Copyright (c) 2018-2026 Frederic Cambus, BSD-2-Clause',
                    ' * (src/third_party/spleen/LICENSE). Generated by tools/bdf2c.py --front',
                    ' * from src/third_party/spleen/spleen-8x16.bdf; do not edit. The same',
                    ' * tables as src/display_font.c: fr_font 16 rows of 8 pixels, bit 7 the',
                    ' * leftmost; fr_title_font 17 rows of 10 pixels, bit 9 the leftmost.',
                    ' */',
                    '#include "fr_font.h"'],
                'const uint8_t fr_font[FR_FONT_COUNT][FR_FONT_H]',
                'const uint16_t fr_title_font[FR_FONT_COUNT][FR_TITLE_H]')


def text(g, head, font_decl, title_decl):
    out = list(head)

    def table(decl, t, form):
        out.extend(['', decl + ' = {'])
        for c in range(FIRST, LAST + 1):
            row = ', '.join(form % v for v in t[c])
            out.append('\t{ %s },\t/* %s */' % (row, name(c)))
        out.append('};')

    table(font_decl, g, '0x%02X')
    table(title_decl, title_font(g), '0x%03X')
    return '\n'.join(out) + '\n'


def main():
    args = sys.argv[1:]
    front = '--front' in args
    path = FRONT_OUT if front else OUT
    text = (front_source if front else source)(glyphs(BDF))
    if '--write' in args:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(text)
        return 0
    with open(path, encoding='utf-8') as f:
        if f.read() != text:
            print('bdf2c: %s is not current; run tools/bdf2c.py%s --write' %
                  (os.path.relpath(path, ROOT), ' --front' if front else ''))
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
