#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes the C file of a build-time image for the hand-over frame (S-15.132) from a PNG file.

    python3 -I -B tools/png2logo.py <image>.png <out>.c

`make image LOGO=<image>.png` runs it. Standard library only.

The PNG is exactly 480 x 222 pixels (the visible area, S-13c.023), not interlaced, of any color type and
bit depth. Its pixel (x, y) is shown at screen position (x, y); a translucent pixel is first blended onto
black, and each channel, as an 8-bit value, keeps its high bits (5, 6, 5), as wiki/Screen-Style-Guide.md's
palette is converted. The C file defines wb_display_boot_image
(waffle/display.h): the pixels in the frame's order, window row r = 479 - x from 0 to 479 and in each its
columns y 0 to 221, as records of a byte h, then for h < 0x80 h + 1 pixels, for h >= 0x80 one pixel
standing for h - 0x7F; a pixel is two bytes, RGB565 high byte first."""
import os
import struct
import sys
import zlib

W, H = 480, 222
MAX_BYTES = 160 * 1024          # the records' limit; a slot holds 0x78000 bytes in all (S-02.003)
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
DEPTHS = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}


class PngError(Exception):
    pass


def chunks(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise PngError('not a PNG file')
    i = 8
    while i + 12 <= len(data):
        n, kind = struct.unpack('>I4s', data[i:i + 8])
        body = data[i + 8:i + 8 + n]
        if len(body) != n or i + 12 + n > len(data):
            raise PngError('truncated %s chunk' % kind.decode('latin-1'))
        if struct.unpack('>I', data[i + 8 + n:i + 12 + n])[0] != zlib.crc32(kind + body):
            raise PngError('bad CRC in %s chunk' % kind.decode('latin-1'))
        yield kind, body
        if kind == b'IEND':
            return
        i += 12 + n
    raise PngError('no IEND chunk')


def unfilter(raw, width, height, bpp, stride):
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(height):
        if i + 1 + stride > len(raw):
            raise PngError('image data too short')
        ft, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for k in range(stride):
            a = line[k - bpp] if k >= bpp else 0
            b = prev[k]
            c = prev[k - bpp] if k >= bpp else 0
            if ft == 1:
                line[k] = (line[k] + a) & 0xFF
            elif ft == 2:
                line[k] = (line[k] + b) & 0xFF
            elif ft == 3:
                line[k] = (line[k] + (a + b) // 2) & 0xFF
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[k] = (line[k] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
            elif ft != 0:
                raise PngError('unknown filter type %d' % ft)
        rows.append(line)
        prev = line
    return rows


def samples(line, width, channels, depth):
    """The row's samples, width * channels of them."""
    if depth == 8:
        return list(line[:width * channels])
    if depth == 16:
        return [line[k] << 8 | line[k + 1] for k in range(0, 2 * width * channels, 2)]
    per, mask, out = 8 // depth, (1 << depth) - 1, []
    for k in range(width * channels):
        out.append(line[k // per] >> (8 - depth * (k % per + 1)) & mask)
    return out


def read_png(data):
    """[[(r, g, b, a, max), ...] per row]: each pixel's channels and alpha on the scale 0..max."""
    hdr = plte = trns = None
    idat = b''
    for kind, body in chunks(data):
        if kind == b'IHDR':
            hdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            plte = [tuple(body[k:k + 3]) for k in range(0, len(body) - 2, 3)]
        elif kind == b'tRNS':
            trns = body
        elif kind == b'IDAT':
            idat += body
    if hdr is None:
        raise PngError('no IHDR chunk')
    width, height, depth, ctype, comp, filt, interlace = hdr
    if ctype not in CHANNELS or depth not in DEPTHS[ctype] or comp != 0 or filt != 0:
        raise PngError('unsupported color type %d at bit depth %d' % (ctype, depth))
    if interlace != 0:
        raise PngError('interlaced PNG: save it without interlacing')
    if (width, height) != (W, H):
        raise PngError('the image is %d x %d pixels; it must be %d x %d' % (width, height, W, H))
    if ctype == 3 and not plte:
        raise PngError('palette image without a PLTE chunk')
    ch = CHANNELS[ctype]
    stride = (width * ch * depth + 7) // 8
    try:
        raw = zlib.decompress(idat)
    except zlib.error as e:
        raise PngError('bad image data: %s' % e) from None
    top = (1 << depth) - 1
    out = []
    for line in unfilter(raw, width, height, max(1, ch * depth // 8), stride):
        s = samples(line, width, ch, depth)
        row = []
        for x in range(width):
            p = s[x * ch:(x + 1) * ch]
            if ctype == 3:
                if p[0] >= len(plte):
                    raise PngError('palette index %d past the palette' % p[0])
                r, g, b = plte[p[0]]
                a = trns[p[0]] if trns is not None and p[0] < len(trns) else 255
                row.append((r, g, b, a, 255))
            elif ctype == 0:
                key = trns is not None and len(trns) >= 2 and p[0] == struct.unpack('>H', trns[:2])[0]
                row.append((p[0], p[0], p[0], 0 if key else top, top))
            elif ctype == 2:
                key = trns is not None and len(trns) >= 6 and tuple(p) == struct.unpack('>HHH', trns[:6])
                row.append((p[0], p[1], p[2], 0 if key else top, top))
            elif ctype == 4:
                row.append((p[0], p[0], p[0], p[1], top))
            else:
                row.append((p[0], p[1], p[2], p[3], top))
        out.append(row)
    return out


def level(v, a, top, bits):
    """v * a / top**2 as an 8-bit value (rounded to the nearest), then its high bits."""
    return (2 * v * a * 255 + top * top) // (2 * top * top) >> (8 - bits)


def rgb565(px):
    r, g, b, a, top = px
    return level(r, a, top, 5) << 11 | level(g, a, top, 6) << 5 | level(b, a, top, 5)


def frame_pixels(rows):
    """The visible pixels in the frame's order: window row r (screen x = 479 - r), its columns y 0..221."""
    return [rgb565(rows[y][W - 1 - r]) for r in range(W) for y in range(H)]


def encode(pixels):
    out, i, n = bytearray(), 0, len(pixels)
    lit = []

    def flush():
        while lit:
            part = lit[:128]
            del lit[:128]
            out.append(len(part) - 1)
            for p in part:
                out.extend(struct.pack('>H', p))

    while i < n:
        run = 1
        while i + run < n and run < 128 and pixels[i + run] == pixels[i]:
            run += 1
        if run >= 2:
            flush()
            out.append(0x7F + run)
            out += struct.pack('>H', pixels[i])
        else:
            lit.append(pixels[i])
        i += run
    flush()
    return bytes(out)


def decode(data, count):
    """The records back to count pixels; black past the data (as src/display_draw.c reads them)."""
    out, i = [], 0
    while len(out) < count and i < len(data):
        h = data[i]
        i += 1
        k = h + 1 if h < 0x80 else h - 0x7F
        if h < 0x80:
            for _ in range(k):
                if i + 2 > len(data):
                    i = len(data)
                    break
                out.append(data[i] << 8 | data[i + 1])
                i += 2
        else:
            if i + 2 > len(data):
                break
            out += [data[i] << 8 | data[i + 1]] * k
            i += 2
    out = out[:count]
    return out + [0] * (count - len(out))


def c_source(data, name):
    lines = ['/* SPDX-License-Identifier: GPL-2.0-or-later */',
             '/* Generated by tools/png2logo.py from %s: the hand-over frame\'s' % name,
             ' * build-time image (S-15.132). Do not edit. */',
             '#include "waffle/display.h"', '',
             'static const uint8_t data[%d] = {' % len(data)]
    for k in range(0, len(data), 12):
        lines.append('\t' + ' '.join('0x%02X,' % b for b in data[k:k + 12]))
    lines += ['};', '',
              'const struct wb_display_image wb_display_boot_image = {',
              '\tdata, sizeof data', '};', '']
    return '\n'.join(lines)


def convert(png, name):
    data = encode(frame_pixels(read_png(png)))
    if len(data) > MAX_BYTES:
        raise PngError('the image takes %d bytes in the bootloader, over the %d allowed: use fewer colors or '
                       'larger flat areas (no gradients, noise or dithering)' % (len(data), MAX_BYTES))
    return c_source(data, name)


def main(argv):
    if len(argv) != 3:
        print(__doc__.split('\n\n')[1], file=sys.stderr)
        return 2
    try:
        with open(argv[1], 'rb') as f:
            src = convert(f.read(), os.path.basename(argv[1]))
    except (OSError, PngError) as e:
        print('png2logo: %s: %s' % (argv[1], e), file=sys.stderr)
        return 1
    with open(argv[2] + '.tmp', 'w') as f:
        f.write(src)
    os.replace(argv[2] + '.tmp', argv[2])
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
