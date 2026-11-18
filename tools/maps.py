#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws the README's two maps as SVG: the 1 MiB SPI flash (S-02) and the eMMC (S-03), each on a
white page and, as <name>-dark.svg, on a dark one, for a reader's light or dark theme.

    python3 -I -B tools/maps.py [output directory]     (default docs/maps)

Each map is a column of areas, not to scale, with its offsets and what each area is for, and under
it a bar that is to scale. The numbers are the tables below. Standard library only."""
import os
import sys

# The two pages' colors. The tables below name an area's color; use() picks the page.
PAGES = {
    'light': dict(BG='#ffffff', PANEL='#eaeef2', RULE='#d1d9e0', TEXT='#1f2328', DIM='#59636e',
                  AMBER='#bf8700', RED='#cf222e', GREEN='#1a7f37', BLUE='#0969da', GRAY='#8c959f'),
    'dark': dict(BG='#0b0b0c', PANEL='#151517', RULE='#2c2c30', TEXT='#ececec', DIM='#9a9aa0',
                 AMBER='#ffb000', RED='#ff5a4d', GREEN='#7bd88f', BLUE='#6cb6ff', GRAY='#5f5f66'),
}
AMBER, RED, GREEN, BLUE, GRAY = 'AMBER', 'RED', 'GREEN', 'BLUE', 'GRAY'
BG = PANEL = RULE = TEXT = DIM = ''
PAGE = {}


def use(page):
    global BG, PANEL, RULE, TEXT, DIM, PAGE
    PAGE = PAGES[page]
    BG, PANEL, RULE, TEXT, DIM = (PAGE[k] for k in ('BG', 'PANEL', 'RULE', 'TEXT', 'DIM'))
FONT = 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace'
W = 860
X_OFF, X_BOX, W_BOX, X_NOTE = 20, 122, 268, 408

SECTOR = 0x1000

# name, first byte, bytes, color, row height, what it is
FLASH = [
    ('First stage', 0x00000, 0xB000, GRAY, 46,
     ["The unit's own first loader. It reads the selector,", 'checks the slot and starts it. waffle-boot leaves it.']),
    ('Metadata mirror', 0x0B000, SECTOR, BLUE, 26, ['A second copy, kept only while the record is written.']),
    ('Metadata', 0x0C000, SECTOR, BLUE, 26, ['Serial number and calibration. Written by an OS install.']),
    ('Selector', 0x0D000, SECTOR, AMBER, 26, ['Which slot starts, and whether it is trusted.']),
    ('Slot 1 record', 0x0E000, SECTOR, AMBER, 26, ["Slot 1's length and CRC-32."]),
    ('Slot 2 record', 0x0F000, SECTOR, AMBER, 26, ["Slot 2's length and CRC-32."]),
    ('Slot 1', 0x10000, 0x76000, RED, 66,
     ['A bootloader image. Its first part is the front,', 'which holds recovery, so recovery is in every image.',
      'Written only on request, never the slot that runs.']),
    ('Slot 1 settings', 0x86000, SECTOR, BLUE, 26, ["Slot 1's image's settings, a page per change."]),
    ('Slot 1 start log', 0x87000, SECTOR, GREEN, 26, ['One bit cleared at each start. Erased every 256 or more.']),
    ('Slot 2', 0x88000, 0x76000, RED, 66,
     ['The second image. A new image goes into the slot', 'that is not running and is read back before the',
      'selector names it.']),
    ('Slot 2 settings', 0xFE000, SECTOR, BLUE, 26, ["As Slot 1's."]),
    ('Slot 2 start log', 0xFF000, SECTOR, GREEN, 26, ['As Slot 1\'s.']),
]
FLASH_END = 0x100000

GIB = 1 << 30
SEC = 512
# name, first sector, sectors, color, row height, what it is
USER = [
    ('yaffs2 volume', 0x000000, 0x480000, AMBER, 66,
     ['Shared by the bootloader and the OS: content,', 'the staging area of an install, and os.ldr',
      'for a developer OS write.']),
    ('Unused', 0x480000, 0x878000, GRAY, 40, ['Never read or written.']),
    ('FAT volume', 0xCF8000, 0x1000000, BLUE, 66,
     ["The OS's own. The bootloader never mounts it. Its", 'one write there is a factory reset erasing the',
      "volume's first 512 KiB."]),
]
USER_END = 0x1CF8000


def esc(s):
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def text(x, y, s, fill=TEXT, size=13, weight='400', anchor='start'):
    return ('<text x="%d" y="%d" fill="%s" font-size="%d" font-weight="%s" text-anchor="%s">%s</text>'
            % (x, y, fill, size, weight, anchor, esc(s)))


def size_words(n):
    if n >= GIB:
        return ('%.2f' % (n / GIB)).rstrip('0').rstrip('.') + ' GiB'
    if n >= 1 << 20:
        return ('%.2f' % (n / (1 << 20))).rstrip('0').rstrip('.') + ' MiB'
    return '%d KiB' % (n // 1024)


def column(rows, y, off_words):
    """The areas as boxes: (svg, the y after the last)."""
    out = []
    for name, first, n, color, h, notes in rows:
        color = PAGE[color]
        out.append(text(X_OFF, y + 17, off_words(first), DIM, 12))
        out.append('<rect x="%d" y="%d" width="%d" height="%d" rx="4" fill="%s" fill-opacity="0.16" '
                   'stroke="%s" stroke-width="1.5"/>' % (X_BOX, y, W_BOX, h - 4, color, color))
        out.append(text(X_BOX + 12, y + 17, name, TEXT, 13, '700'))
        if isinstance(n, str):
            out.append(text(X_BOX + 12, y + 35, n, DIM, 12))
        else:
            out.append(text(X_BOX + W_BOX - 10, y + 17, size_words(n if rows is FLASH else n * SEC), DIM, 12,
                            anchor='end'))
        for i, line in enumerate(notes):
            out.append(text(X_NOTE, y + 17 + 17 * i, line, DIM, 12))
        y += h
    return out, y


def scale_bar(rows, end, y, label):
    """The same areas to scale, in one bar."""
    x0, w = X_OFF, W - 2 * X_OFF
    out = [text(x0, y, label, DIM, 12)]
    y += 8
    out.append('<rect x="%d" y="%d" width="%d" height="22" rx="3" fill="%s"/>' % (x0, y, w, PANEL))
    for name, first, n, color, h, notes in rows:
        color = PAGE[color]
        x = x0 + w * first / end
        out.append('<rect x="%.2f" y="%d" width="%.2f" height="22" fill="%s" fill-opacity="0.85"/>'
                   % (x, y, max(w * n / end - 0.6, 0.9), color))
    return out, y + 22


def page(title, sub, body, h):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" '
            'font-family="%s" role="img" aria-label="%s">\n<rect width="%d" height="%d" rx="10" fill="%s"/>\n'
            % (W, h, W, h, FONT, esc(title), W, h, BG) +
            text(X_OFF, 34, title, TEXT, 17, '700') + '\n' + text(X_OFF, 54, sub, DIM, 12) + '\n' +
            '<line x1="%d" y1="66" x2="%d" y2="66" stroke="%s"/>\n' % (X_OFF, W - X_OFF, RULE) +
            '\n'.join(body) + '\n</svg>\n')


def flash_map():
    body, y = column(FLASH, 82, lambda b: '0x%05X' % b)
    body.append(text(X_OFF, y + 13, '0x%05X' % FLASH_END, DIM, 12))
    body.append(text(X_BOX, y + 13, 'the end of the flash', DIM, 12))
    bar, y = scale_bar(FLASH, FLASH_END, y + 46, 'To scale: the two slots are 93% of the flash')
    return page('The SPI flash, 1 MiB', 'Everything the unit needs to start. Offsets in bytes; sectors are 4 KiB.',
                body + bar, y + 20)


def emmc_map():
    y = 82
    body = []
    for k in (1, 2):
        rows = [('Boot partition %d' % k, 0, 'its size comes from the eMMC', RED, 66,
                 ['Sector 0: a header (active or not, the image\'s', 'length and CRC-32). From sector 1: one OS image.']
                 if k == 1 else
                 ['The same. A new OS goes into the inactive one', 'and is switched to last, so a cut leaves the old',
                  'OS starting.'])]
        part, y = column(rows, y, lambda s: 'BP%d' % k)
        body += part
    body.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s"/>' % (X_OFF, y + 4, W - X_OFF, y + 4, RULE))
    body.append(text(X_OFF, y + 26, 'The user area, %s' % size_words(USER_END * SEC), TEXT, 13, '700'))
    body.append(text(X_NOTE, y + 26, 'Offsets are sector numbers, 512 bytes each.', DIM, 12))
    part, y = column(USER, y + 40, lambda s: '0x%07X' % s)
    body += part
    body.append(text(X_OFF, y + 13, '0x%07X' % USER_END, DIM, 12))
    body.append(text(X_BOX, y + 13, 'the end of the user area', DIM, 12))
    bar, y = scale_bar(USER, USER_END, y + 46, 'The user area to scale. Each boot partition is a few MiB beside it')
    return page('The eMMC', 'The OS and everything it stores. The bootloader itself is not here: it is in the SPI flash.',
                body + bar, y + 20)


def main(argv):
    # An option is never taken for the output directory: -h and --help print the usage.
    if len(argv) > 2 or (len(argv) > 1 and argv[1].startswith('-')):
        print(__doc__)
        return 0 if argv[1:] in (['-h'], ['--help']) else 2
    out = argv[1] if len(argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs', 'maps')
    os.makedirs(out, exist_ok=True)
    for page, suffix in (('light', ''), ('dark', '-dark')):
        use(page)
        for name, svg in (('spi-flash', flash_map()), ('emmc', emmc_map())):
            with open(os.path.join(out, name + suffix + '.svg'), 'w') as f:
                f.write(svg)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
