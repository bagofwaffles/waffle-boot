#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The stored boot logo (src/front/fr_handoff.h) and its conversion from a PNG file: the reference for the
main image's code (src/logo.c, src/logo_png.c), which must give the same bytes. Standard library only.

    python3 -I -B tools/wblogo.py encode IMAGE.png OUT.bin [--room BYTES] [--image-crc CRC]
    python3 -I -B tools/wblogo.py decode IN.bin OUT.png
    python3 -I -B tools/wblogo.py info IN.bin

OUT.bin holds the 36-byte header and then the data; in a slot the data ends where the header's sector starts.
decode draws the whole screen as the front does: the picture centered on its background.

The PNG is not interlaced, at most 480 x 222 pixels, and gray (1, 2, 4 or 8 bits), gray with alpha, RGB or
RGBA (8 bits), or a palette (1, 2, 4 or 8 bits). A translucent pixel is blended onto black. Each channel, as
an 8-bit value, keeps its high bits (5, 6, 5). A picture of 256 colors or fewer keeps them; one with more is
reduced to 256 by median cut. If that does not fit the room, it is reduced to 16, with Floyd-Steinberg
dithering. If that does not fit either, nothing is stored."""
import struct
import sys
import zlib

W, H = 480, 222
MAGIC, VERSION, HEAD = 0x474C4257, 1, 0x24
DATA_MAX = 2 * 256 + 2 * W * H
SLOT_OFF = 0x75000                 # the header's sector in a slot
SECTOR = 0x1000
TAG = 0x0000B004
# color type: (channels, bit depths taken)
TYPES = {0: (1, (1, 2, 4, 8)), 2: (3, (8,)), 3: (1, (1, 2, 4, 8)), 4: (2, (8,)), 6: (4, (8,))}


class LogoError(Exception):
    pass


# --- PNG -------------------------------------------------------------------------------------------------

def _chunks(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise LogoError('not a PNG file')
    i = 8
    while True:
        if i + 12 > len(data):
            raise LogoError('damaged PNG file')
        n, kind = struct.unpack('>I4s', data[i:i + 8])
        if n > len(data) - i - 12:
            raise LogoError('damaged PNG file')
        body = data[i + 8:i + 8 + n]
        if struct.unpack('>I', data[i + 8 + n:i + 12 + n])[0] != zlib.crc32(kind + body):
            raise LogoError('damaged PNG file')
        yield kind, body
        if kind == b'IEND':
            return
        i += 12 + n


def _unfilter(raw, height, bpp, stride):
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(height):
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
                raise LogoError('damaged PNG file')
        rows.append(line)
        prev = line
    return rows


def _samples(line, n, depth):
    if depth == 8:
        return list(line[:n])
    per, mask = 8 // depth, (1 << depth) - 1
    return [line[k // per] >> (8 - depth * (k % per + 1)) & mask for k in range(n)]


def _level(v, a, top, bits):
    """v * a / top**2 as an 8-bit value, rounded, then its high bits (tools/png2logo.py's rule)."""
    return (2 * v * a * 255 + top * top) // (2 * top * top) >> (8 - bits)


def _rgb565(r, g, b, a, top):
    return _level(r, a, top, 5) << 11 | _level(g, a, top, 6) << 5 | _level(b, a, top, 5)


def read_png(data):
    """(width, height, pixels): the RGB565 pixels row by row."""
    hdr = plte = trns = None
    idat = b''
    seen = False                   # an IDAT chunk came: the header, palette and key come before it
    for kind, body in _chunks(data):
        if (hdr is None) != (kind == b'IHDR') or (seen and kind in (b'PLTE', b'tRNS')):
            raise LogoError('damaged PNG file')
        if kind == b'IHDR':
            if len(body) != 13:
                raise LogoError('damaged PNG file')
            hdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            if len(body) > 768:
                raise LogoError('damaged PNG file')
            plte = [tuple(body[k:k + 3]) for k in range(0, len(body) - 2, 3)]
        elif kind == b'tRNS':
            if len(body) > 256:
                raise LogoError('damaged PNG file')
            trns = body
        elif kind == b'IDAT':
            seen = True
            idat += body
    if hdr is None:
        raise LogoError('damaged PNG file')
    width, height, depth, ctype, comp, filt, interlace = hdr
    if ctype not in TYPES or depth not in TYPES[ctype][1] or comp != 0 or filt != 0:
        raise LogoError('PNG type not taken: use 8-bit RGB, RGBA, gray or a palette')
    if interlace != 0:
        raise LogoError('interlaced PNG: save it without interlacing')
    if not 1 <= width <= W or not 1 <= height <= H:
        raise LogoError('too large: at most 480 x 222 pixels')
    if ctype == 3 and not plte:
        raise LogoError('damaged PNG file')
    ch = TYPES[ctype][0]
    stride = (width * ch * depth + 7) // 8
    try:
        d = zlib.decompressobj()
        raw = d.decompress(idat)
        if not d.eof:
            raise LogoError('damaged PNG file')
    except zlib.error:
        raise LogoError('damaged PNG file') from None
    if len(raw) < height * (stride + 1):
        raise LogoError('damaged PNG file')
    top = (1 << depth) - 1
    px = []
    for line in _unfilter(raw, height, max(1, ch * depth // 8), stride):
        s = _samples(line, width * ch, depth)
        for x in range(width):
            p = s[x * ch:(x + 1) * ch]
            if ctype == 3:
                if p[0] >= len(plte):
                    raise LogoError('damaged PNG file')
                r, g, b = plte[p[0]]
                a = trns[p[0]] if trns is not None and p[0] < len(trns) else 255
                px.append(_rgb565(r, g, b, a, 255))
            elif ctype == 0:
                key = trns is not None and len(trns) >= 2 and p[0] == struct.unpack('>H', trns[:2])[0]
                px.append(_rgb565(p[0], p[0], p[0], 0 if key else top, top))
            elif ctype == 2:
                key = trns is not None and len(trns) >= 6 and tuple(p) == struct.unpack('>HHH', trns[:6])
                px.append(_rgb565(p[0], p[1], p[2], 0 if key else 255, 255))
            elif ctype == 4:
                px.append(_rgb565(p[0], p[0], p[0], p[1], 255))
            else:
                px.append(_rgb565(p[0], p[1], p[2], p[3], 255))
    return width, height, px


# --- Colors ----------------------------------------------------------------------------------------------

def _comps(c):
    return c >> 11 & 31, c >> 5 & 63, c & 31


def _scaled(c):
    """The components on one scale for median cut's ranges: red and blue doubled."""
    r, g, b = _comps(c)
    return 2 * r, g, 2 * b


def _widest(box):
    lo = [min(_scaled(c)[k] for c in box) for k in range(3)]
    hi = [max(_scaled(c)[k] for c in box) for k in range(3)]
    rr, gr, br = (hi[k] - lo[k] for k in range(3))
    if gr >= rr and gr >= br:
        return gr, 1
    if rr >= br:
        return rr, 0
    return br, 2


def median_cut(counts, target):
    """(palette, box): at most target colors from {color: count}; box maps each color to its entry."""
    boxes = [sorted(counts)]
    wide = [_widest(boxes[0])]
    while len(boxes) < target:
        bi, best = -1, 0
        for i, b in enumerate(boxes):
            if len(b) >= 2 and wide[i][0] > best:
                bi, best = i, wide[i][0]
        if bi < 0:
            break
        ch = wide[bi][1]
        b = sorted(boxes[bi], key=lambda c: (_scaled(c)[ch], c))
        total = sum(counts[c] for c in b)
        cum, k = 0, len(b) - 1
        for i, c in enumerate(b):
            cum += counts[c]
            if 2 * cum >= total:
                k = min(i + 1, len(b) - 1)
                break
        boxes[bi], new = b[:k], b[k:]
        wide[bi] = _widest(boxes[bi])
        boxes.append(new)
        wide.append(_widest(new))
    palette, box = [], {}
    for i, b in enumerate(boxes):
        t = sum(counts[c] for c in b)
        s = [sum(counts[c] * _comps(c)[k] for c in b) for k in range(3)]
        r, g, bl = ((2 * s[k] + t) // (2 * t) for k in range(3))
        palette.append(r << 11 | g << 5 | bl)
        for c in b:
            box[c] = i
    return palette, box


def _fdiv16(v):
    return v // 16


def dither(w, h, px, palette):
    """Floyd-Steinberg onto palette, in the components' own units, the error kept in sixteenths."""
    pc = [_comps(c) for c in palette]
    top = (31, 63, 31)
    cur = [[0, 0, 0] for _ in range(w + 2)]
    out = []
    for y in range(h):
        nxt = [[0, 0, 0] for _ in range(w + 2)]
        for x in range(w):
            c = _comps(px[y * w + x])
            want = [c[k] * 16 + cur[x + 1][k] for k in range(3)]
            t = [min(max(_fdiv16(want[k] + 8), 0), top[k]) for k in range(3)]
            best, bd = 0, None
            for i, p in enumerate(pc):
                d = 4 * (p[0] - t[0]) ** 2 + (p[1] - t[1]) ** 2 + 4 * (p[2] - t[2]) ** 2
                if bd is None or d < bd:
                    best, bd = i, d
            out.append(best)
            for k in range(3):
                e = min(max(want[k], 0), top[k] * 16) - pc[best][k] * 16
                e7, e3, e5 = _fdiv16(e * 7), _fdiv16(e * 3), _fdiv16(e * 5)
                cur[x + 2][k] += e7
                nxt[x][k] += e3
                nxt[x + 1][k] += e5
                nxt[x + 2][k] += e - e7 - e3 - e5
        cur = nxt
    return out


# --- The stored format -----------------------------------------------------------------------------------

def bits_for(n):
    return 1 if n <= 2 else 2 if n <= 4 else 4 if n <= 16 else 8


def pack(idx, w, h, bits):
    """The lines in the panel's order: line j is column w - 1 - j, top down."""
    minrun = 16 // bits + 1
    out = bytearray()

    def flush(lit):
        if lit:
            out.append(len(lit) - 1)
            acc = bytearray((len(lit) * bits + 7) // 8)
            for k, v in enumerate(lit):
                b = k * bits
                acc[b // 8] |= v << (8 - bits - b % 8)
            out.extend(acc)
            lit.clear()

    for j in range(w):
        x = w - 1 - j
        line = [idx[y * w + x] for y in range(h)]
        lit, i = [], 0
        while i < h:
            n = 1
            while i + n < h and n < 128 and line[i + n] == line[i]:
                n += 1
            if n >= minrun:
                flush(lit)
                out += bytes((0x80 | (n - 1), line[i]))
                i += n
            else:
                lit.append(line[i])
                i += 1
                if len(lit) == 128:
                    flush(lit)
        flush(lit)
    return bytes(out)


def _stored(w, h, idx, palette, bg):
    bits = bits_for(len(palette))
    data = b''.join(struct.pack('<H', c) for c in palette) + pack(idx, w, h, bits)
    return dict(width=w, height=h, bits=bits, colors=len(palette), bg=bg), data


def convert(w, h, px, room):
    """(fields, data, way): way is 'kept', 'reduced' (to 256) or 'dithered' (to 16)."""
    counts = {}
    for c in px:
        counts[c] = counts.get(c, 0) + 1
    bg = px[0]
    if len(counts) <= 256:
        palette = sorted(counts)
        if len(palette) == 1:
            palette.append(palette[0] ^ 0xFFFF)
        at = {c: i for i, c in enumerate(palette)}
        idx, way = [at[c] for c in px], 'kept'
    else:
        palette, box = median_cut(counts, 256)
        idx, way = [box[c] for c in px], 'reduced'
    fields, data = _stored(w, h, idx, palette, bg)
    if len(data) <= room:
        return fields, data, way
    if len(counts) > 16:
        palette, _ = median_cut(counts, 16)
        fields, data = _stored(w, h, dither(w, h, px, palette), palette, bg)
        if len(data) <= room:
            return fields, data, 'dithered'
    raise LogoError('does not fit: %d bytes, %d free' % (len(data), room))


def header(fields, data, image_crc):
    h = struct.pack('<IHHHHBBHHHIII', MAGIC, VERSION, HEAD, fields['width'], fields['height'], fields['bits'],
                    0, fields['colors'], fields['bg'], 0, len(data), zlib.crc32(data), image_crc)
    return h + struct.pack('<I', zlib.crc32(h))


def parse(head, rec_len, rec_crc, rec_tag=TAG):
    """The header's fields as the front checks them; None for no logo."""
    if len(head) < HEAD:
        return None
    (magic, version, size, w, h, bits, z1, colors, bg, z2, n, crc, image, hcrc) = struct.unpack(
        '<IHHHHBBHHHIIII', head[:HEAD])
    if magic != MAGIC or version != VERSION or size != HEAD or zlib.crc32(head[:HEAD - 4]) != hcrc:
        return None
    if z1 or z2 or not 1 <= w <= W or not 1 <= h <= H or bits not in (1, 2, 4, 8) or \
            not 2 <= colors <= 1 << bits:
        return None
    if rec_tag != TAG or not 1 <= rec_len <= SLOT_OFF or image != rec_crc:
        return None
    end = (rec_len + SECTOR - 1) // SECTOR * SECTOR
    if not 2 * colors <= n <= min(DATA_MAX, SLOT_OFF - end):
        return None
    return dict(width=w, height=h, bits=bits, colors=colors, bg=bg, len=n, crc=crc)


def room(rec_len, rec_tag=TAG):
    """The bytes a slot whose record is this leaves to a logo."""
    if rec_tag != TAG or not 1 <= rec_len <= SLOT_OFF:
        return 0
    return SLOT_OFF - (rec_len + SECTOR - 1) // SECTOR * SECTOR


def draw(f, data):
    """The screen as the front draws it, [y][x] RGB565; None at the first fault."""
    if zlib.crc32(data) != f['crc'] or len(data) != f['len']:
        return None
    w, h, bits, colors = f['width'], f['height'], f['bits'], f['colors']
    pal = [struct.unpack('<H', data[2 * i:2 * i + 2])[0] for i in range(colors)]
    p = data[2 * colors:]
    x0, y0 = (W - w) // 2, (H - h) // 2
    scr = [[f['bg']] * W for _ in range(H)]
    pos = 0
    for j in range(w):
        x, y = x0 + w - 1 - j, 0
        while y < h:
            if pos >= len(p):
                return None
            c = p[pos]
            pos += 1
            n = (c & 0x7F) + 1
            if n > h - y:
                return None
            if c & 0x80:
                if pos >= len(p) or p[pos] >= colors:
                    return None
                for k in range(n):
                    scr[y0 + y + k][x] = pal[p[pos]]
                pos += 1
            else:
                nb = (n * bits + 7) // 8
                if nb > len(p) - pos:
                    return None
                for k in range(n):
                    b = k * bits
                    v = p[pos + b // 8] >> (8 - bits - b % 8) & ((1 << bits) - 1)
                    if v >= colors:
                        return None
                    scr[y0 + y + k][x] = pal[v]
                pos += nb
            y += n
    return scr if pos == len(p) else None


def write_png(scr):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = bytearray()
    for row in scr:
        raw.append(0)
        for c in row:
            r, g, b = _comps(c)
            raw += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', len(scr[0]), len(scr), 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def main(argv):
    args = argv[1:]
    opts = {'--room': str(SLOT_OFF), '--image-crc': '0'}
    pos = []
    while args:
        a = args.pop(0)
        if a in opts and args:
            opts[a] = args.pop(0)
        else:
            pos.append(a)
    try:
        if len(pos) == 3 and pos[0] == 'encode':
            with open(pos[1], 'rb') as f:
                w, h, px = read_png(f.read())
            fields, data, way = convert(w, h, px, int(opts['--room'], 0))
            with open(pos[2], 'wb') as f:
                f.write(header(fields, data, int(opts['--image-crc'], 0)) + data)
            print('%d x %d, %d colors (%s), %d bytes' % (w, h, fields['colors'], way, len(data)))
            return 0
        if len(pos) in (2, 3) and pos[0] in ('decode', 'info'):
            with open(pos[1], 'rb') as f:
                blob = f.read()
            image = struct.unpack('<I', blob[0x1C:0x20])[0] if len(blob) >= HEAD else 0
            f = parse(blob[:HEAD], 1, image)
            scr = draw(f, blob[HEAD:]) if f else None
            if scr is None:
                raise LogoError('not a valid stored logo')
            if pos[0] == 'info' and len(pos) == 2:
                print('%d x %d, %d colors at %d bits, background %04X, %d bytes' %
                      (f['width'], f['height'], f['colors'], f['bits'], f['bg'], f['len']))
                return 0
            if pos[0] == 'decode' and len(pos) == 3:
                with open(pos[2], 'wb') as out:
                    out.write(write_png(scr))
                return 0
    except (OSError, ValueError, LogoError) as e:
        print('wblogo: %s' % e, file=sys.stderr)
        return 1
    print(__doc__.split('\n\n')[1], file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
