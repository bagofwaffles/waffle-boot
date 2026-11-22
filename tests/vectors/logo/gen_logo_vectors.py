#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate logo_vectors.h: PNG files made here in code, and what tools/wblogo.py, the reference, gives for
each: the decode's result and pixels, the conversion's way and stored data, and the screen it draws. The
unit tests hold src/logo_png.c, src/logo.c and the front's src/front/fr_logo.c to them.

Run from the repository root:  python3 -I -B tests/vectors/logo/gen_logo_vectors.py --out DIR
"""
import os
import struct
import sys
import zlib
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import genout  # noqa: E402  (before anything the generator reads)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import wblogo  # noqa: E402

OUT = 'logo_vectors.h'
CH = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def chunk(kind, body, crc_xor=0):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) ^ crc_xor)


def pack_row(samples, depth):
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b''.join(struct.pack('>H', s) for s in samples)
    per = 8 // depth
    out = bytearray((len(samples) + per - 1) // per)
    for k, s in enumerate(samples):
        out[k // per] |= s << (8 - depth * (k % per + 1))
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def filtered(rows, bpp):
    """Each row under filter type (row number % 5)."""
    out, prev = bytearray(), bytes(len(rows[0]))
    for y, r in enumerate(rows):
        ft = y % 5
        out.append(ft)
        for k in range(len(r)):
            a = r[k - bpp] if k >= bpp else 0
            b = prev[k]
            c = prev[k - bpp] if k >= bpp else 0
            pred = (0, a, b, (a + b) // 2, paeth(a, b, c))[ft]
            out.append((r[k] - pred) & 0xFF)
        prev = r
    return bytes(out)


def png(w, h, ctype, depth, pix, plte=None, trns=None, interlace=0, split=0, order='normal', crc_xor=0,
        cut=0, sig=b'\x89PNG\r\n\x1a\n', trailing_zlib=False):
    """pix(x, y) gives a pixel's samples."""
    ch = CH[ctype]
    rows = [pack_row([s for x in range(w) for s in pix(x, y)], depth) for y in range(h)]
    raw = filtered(rows, max(1, ch * depth // 8))
    data = zlib.compress(raw, 9)
    if trailing_zlib:
        data = data[:-4]                   # no Adler-32: an unfinished stream
    ihdr = chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, ctype, 0, 0, interlace))
    pl = chunk(b'PLTE', bytes(v for c in plte for v in c)) if plte else b''
    tr = chunk(b'tRNS', trns) if trns is not None else b''
    if split:
        idat = b''.join(chunk(b'IDAT', data[i:i + split]) for i in range(0, len(data), split))
    else:
        idat = chunk(b'IDAT', data, crc_xor)
    text = chunk(b'tEXt', b'Comment\x00made in code')
    if order == 'plte_late':
        body = ihdr + idat + pl + tr
    elif order == 'no_ihdr':
        body = pl + tr + idat
    else:
        body = ihdr + text + pl + tr + idat
    out = sig + body + (b'' if order == 'no_iend' else chunk(b'IEND', b''))
    return out[:len(out) - cut] if cut else out


def gradient(w, h):
    return lambda x, y: ((x * 255) // max(1, w - 1), (y * 255) // max(1, h - 1), ((x + y) * 127) // max(1, w + h))


def cases():
    pal = [(0, 0, 0), (255, 176, 32), (255, 255, 255), (80, 216, 112), (255, 80, 64), (30, 60, 200),
           (128, 128, 128), (10, 200, 200)]
    c = []
    # palettes at each depth, with and without alpha
    for d in (1, 2, 4):
        n = min(1 << d, len(pal))
        c.append(('palette%d' % d, png(37, 19, 3, d, lambda x, y, n=n: ((x // 3 + y // 2) % n,), plte=pal[:n]),
                  0x40000))
    many = [((i * 37) & 0xFF, (i * 91) & 0xFF, (i * 13) & 0xFF) for i in range(200)]
    c.append(('palette8', png(50, 40, 3, 8, lambda x, y: ((x * 4 + y) % 200,), plte=many), 0x40000))
    c.append(('palette4_trns', png(40, 30, 3, 4, lambda x, y: ((x * y) % 8,), plte=pal,
                                   trns=bytes([0, 128, 255, 40])), 0x40000))
    for d in (1, 2, 4, 8):
        top = (1 << d) - 1
        c.append(('gray%d' % d, png(33, 21, 0, d, lambda x, y, t=top: ((x + 2 * y) % (t + 1),)), 0x40000))
    c.append(('gray8_key', png(30, 20, 0, 8, lambda x, y: ((x * 9) & 0xF0,), trns=struct.pack('>H', 0x30)),
              0x40000))
    c.append(('gray_alpha', png(50, 25, 4, 8, lambda x, y: (200, (x * 5) & 0xFF)), 0x40000))
    c.append(('rgb_flat', png(120, 60, 2, 8, lambda x, y: pal[(x // 30 + y // 20) % 8]), 0x40000))
    c.append(('rgb_key', png(64, 32, 2, 8, lambda x, y: pal[(x // 16) % 3], trns=struct.pack('>HHH', 255, 176, 32)),
              0x40000))
    c.append(('rgba', png(60, 40, 6, 8, lambda x, y: (255, 176, 32, (x * 4 + y) & 0xFF)), 0x40000))
    c.append(('one_color', png(16, 16, 2, 8, lambda x, y: (12, 34, 56)), 0x40000))
    c.append(('full_screen', png(480, 222, 3, 2, lambda x, y: (((x // 40) ^ (y // 37)) & 3,), plte=pal[:4]),
              0x40000))
    c.append(('split_idat', png(100, 50, 2, 8, lambda x, y: pal[(x * y // 50) % 8], split=97), 0x40000))
    # more than 256 colors: reduced, then dithered, then not taken
    g = gradient(96, 64)
    c.append(('gradient_256', png(96, 64, 2, 8, g), 0x40000))
    c.append(('gradient_16', png(96, 64, 2, 8, g), 3000))
    c.append(('gradient_none', png(96, 64, 2, 8, g), 2500))
    big = gradient(240, 111)
    c.append(('photo_256', png(240, 111, 6, 8, lambda x, y: big(x, y) + ((x * y) & 0xFF,)), 0x40000))
    c.append(('photo_16', png(240, 111, 6, 8, lambda x, y: big(x, y) + ((x * y) & 0xFF,)), 16000))
    c.append(('flat_none', png(120, 60, 2, 8, lambda x, y: pal[(x + y) % 8]), 200))
    # refused
    c.append(('not_png', png(8, 8, 0, 8, lambda x, y: (1,), sig=b'\x89PNX\r\n\x1a\n'), 0x40000))
    c.append(('bad_crc', png(8, 8, 0, 8, lambda x, y: (x,), crc_xor=1), 0x40000))
    c.append(('cut_short', png(20, 20, 2, 8, lambda x, y: (x, y, 0), cut=30), 0x40000))
    c.append(('no_iend', png(8, 8, 0, 8, lambda x, y: (x,), order='no_iend'), 0x40000))
    c.append(('no_ihdr_first', png(8, 8, 3, 8, lambda x, y: (0,), plte=pal[:2], order='no_ihdr'), 0x40000))
    c.append(('plte_late', png(8, 8, 3, 8, lambda x, y: (0,), plte=pal[:2], order='plte_late'), 0x40000))
    c.append(('index_past', png(8, 8, 3, 8, lambda x, y: (x,), plte=pal[:4]), 0x40000))
    c.append(('stream_unfinished', png(8, 8, 0, 8, lambda x, y: (x,), trailing_zlib=True), 0x40000))
    c.append(('interlaced', png(8, 8, 0, 8, lambda x, y: (x,), interlace=1), 0x40000))
    c.append(('rgb16', png(8, 8, 2, 16, lambda x, y: (x * 999, y, 7)), 0x40000))
    c.append(('too_wide', png(481, 2, 0, 8, lambda x, y: (x & 0xFF,)), 0x40000))
    c.append(('too_tall', png(2, 223, 0, 8, lambda x, y: (y & 0xFF,)), 0x40000))
    return c


RCS = {'not a PNG file': 'WB_PNG_NOT_PNG', 'damaged PNG file': 'WB_PNG_DAMAGED',
       'interlaced PNG: save it without interlacing': 'WB_PNG_INTERLACED',
       'too large: at most 480 x 222 pixels': 'WB_PNG_SIZE',
       'PNG type not taken: use 8-bit RGB, RGBA, gray or a palette': 'WB_PNG_TYPE'}
WAYS = {'kept': 'WB_LOGO_KEPT', 'reduced': 'WB_LOGO_REDUCED', 'dithered': 'WB_LOGO_DITHERED'}


def le16(px):
    return b''.join(struct.pack('<H', p) for p in px)


def cbytes(name, b):
    lines = ['static const uint8_t %s[%d] = {' % (name, max(1, len(b)))]
    for i in range(0, len(b), 16):
        lines.append('\t' + ', '.join('0x%02x' % v for v in b[i:i + 16]) + ',')
    lines.append('};')
    return lines


def main():
    lines = ['/* SPDX-License-Identifier: GPL-2.0-or-later */',
             '/* Generated by tests/vectors/logo/gen_logo_vectors.py from tools/wblogo.py. Do not edit. */',
             '#ifndef LOGO_VECTORS_H', '#define LOGO_VECTORS_H', '', '#include <stdint.h>', '',
             '#include "waffle/logo.h"', '#include "waffle/logo_png.h"', '']
    rows = []
    for name, data, room in cases():
        lines += cbytes('lv_png_%s' % name, data)
        try:
            w, h, px = wblogo.read_png(data)
            rc = 'WB_PNG_OK'
        except wblogo.LogoError as e:
            rows.append('\t{"%s", lv_png_%s, %d, %s, 0, 0, 0, %du, 0, 0, 0, 0, 0, 0, 0, 0, NULL},' %
                        (name, name, len(data), RCS[str(e)], room))
            continue
        pcrc = zlib.crc32(le16(px))
        try:
            fields, stored, way = wblogo.convert(w, h, px, room)
        except wblogo.LogoError:
            rows.append('\t{"%s", lv_png_%s, %d, %s, %d, %d, 0x%08xu, %du, 0, 0, 0, 0, 0, 0, 0, 0, NULL},' %
                        (name, name, len(data), rc, w, h, pcrc, room))
            continue
        f = dict(fields, len=len(stored), crc=zlib.crc32(stored))
        scr = wblogo.draw(f, stored)
        assert scr is not None
        win = [scr[c][479 - r] for r in range(480) for c in range(222)]
        lines += cbytes('lv_data_%s' % name, stored)
        rows.append('\t{"%s", lv_png_%s, %d, %s, %d, %d, 0x%08xu, %du, 1, %s, %d, %d, 0x%04xu, %du, 0x%08xu, '
                    '0x%08xu, lv_data_%s},' %
                    (name, name, len(data), rc, w, h, pcrc, room, WAYS[way], fields['bits'], fields['colors'],
                     fields['bg'], len(stored), f['crc'], zlib.crc32(le16(win)), name))
    lines += ['', '/* A PNG case: the decode\'s result, and on WB_PNG_OK the size and the CRC-32 of the',
              ' * pixels (RGB565, little-endian, row by row); converted into room bytes: fits, the way,',
              ' * the header\'s fields, the data, and the CRC-32 of the screen drawn (window row by row). */',
              'struct lv_case {', '\tconst char *name;', '\tconst uint8_t *png;', '\tuint32_t png_len;',
              '\tint rc;', '\tunsigned w, h;', '\tuint32_t px_crc;', '\tuint32_t room;', '\tint fits;',
              '\tint way;', '\tunsigned bits, colors;', '\tuint16_t bg;', '\tuint32_t len, data_crc, screen_crc;',
              '\tconst uint8_t *data;', '};', '', 'static const struct lv_case lv_cases[] = {']
    lines += rows + ['};', '', '#endif /* LOGO_VECTORS_H */', '']
    genout.write(OUT, '\n'.join(lines))
    genout.finish()


if __name__ == '__main__':
    main()
