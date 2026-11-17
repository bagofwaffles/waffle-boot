#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turns the raw frames of tools/ui_shots.c into PNG files: screen positions (S-13c.023: screen (x, y) is window
row 479 - x, column y), the visible 480 x 222 area, each pixel doubled so that an image shown 480 wide
is sharp on a high-density screen. It also writes tour.gif, a few
of the screens in turn, for the README. Each PNG carries the version it was drawn at, in a tEXt chunk;
--check fails when a picture in the directory carries another version than the tree's
(tools/bump_version.py), or none. Standard library only.

    python3 -I -B tools/ui_shots.py <raw directory> <output directory> <version>
    python3 -I -B tools/ui_shots.py --check [directory]      (default docs/ui)"""
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION_KEY = b'waffle-boot version'

W, H, COLS, SCALE = 480, 222, 224, 2


def png(path, rows, version=None):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b''.join(b'\0' + r for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', W * SCALE, H * SCALE, 8, 2, 0, 0, 0)))
        if version is not None:
            f.write(chunk(b'tEXt', VERSION_KEY + b'\0' + version.encode('ascii')))
        f.write(chunk(b'IDAT', zlib.compress(raw, 9)))
        f.write(chunk(b'IEND', b''))


def convert(src, dst, version):
    with open(src, 'rb') as f:
        frame_png(f.read(), dst, version)


def frame_png(fb, dst, version=None):
    """A frame buffer (S-13c.015's order, as tools/ui_shots.c writes it) as a PNG at dst."""
    png(dst, frame_rows(fb), version)


def png_version(path):
    """The version a PNG of this tool carries, or None."""
    with open(path, 'rb') as f:
        data = f.read()
    at = 8
    while at + 8 <= len(data):
        n, kind = struct.unpack_from('>I4s', data, at)
        if kind == b'tEXt':
            key, _, value = data[at + 8:at + 8 + n].partition(b'\0')
            if key == VERSION_KEY:
                return value.decode('ascii', 'replace')
        at += 12 + n
    return None


def tree_version():
    out = subprocess.run([sys.executable, '-I', '-B', os.path.join(HERE, 'bump_version.py'), '--show'],
                         check=True, capture_output=True, text=True)
    return out.stdout.strip()


def check(directory):
    """0 when every PNG in directory carries the tree's version."""
    want, bad = tree_version(), []
    for name in sorted(os.listdir(directory)):
        if name.endswith('.png'):
            got = png_version(os.path.join(directory, name))
            if got != want:
                bad.append('%s (%s)' % (name, got or 'no version'))
    if bad:
        print('ui_shots: pictures not drawn at %s: %s; redraw them (wiki/Screen-Style-Guide.md)'
              % (want, ', '.join(bad)), file=sys.stderr)
        return 1
    return 0


def frame_rows(fb):
    """The frame buffer's visible area as rows of RGB bytes."""
    rows = []
    for y in range(H):
        line = bytearray()
        for x in range(W):
            i = 2 * ((W - 1 - x) * COLS + y)
            v = fb[i] << 8 | fb[i + 1]
            r, g, b = v >> 11, (v >> 5) & 0x3F, v & 0x1F
            px = bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
            line += px * SCALE
        rows += [bytes(line)] * SCALE
    return rows


# The README's tour: each screen and how long it stays, in hundredths of a second.
TOUR = (('handover', 150), ('main', 300), ('settings', 250), ('developer', 250), ('system', 250),
        ('info', 250), ('upgrade-run', 250), ('recovery', 300))


def lzw(indexes, bits):
    """GIF's LZW over color indexes, with `bits` the minimum code size."""
    clear, end = 1 << bits, (1 << bits) + 1
    out, acc, nacc = bytearray(), 0, 0

    def put(code, size):
        nonlocal acc, nacc
        acc |= code << nacc
        nacc += size
        while nacc >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nacc -= 8
    table, size, prefix = {}, bits + 1, None
    put(clear, size)
    for i in indexes:
        if prefix is None:
            prefix = i
        elif (prefix, i) in table:
            prefix = table[prefix, i]
        else:
            put(prefix, size)
            code = end + 1 + len(table)
            if code == 4096:
                put(clear, size)
                table, size = {}, bits + 1
            else:
                table[prefix, i] = code
                if code == 1 << size:
                    size += 1
            prefix = i
    put(prefix, size)
    put(end, size)
    if nacc:
        out.append(acc & 0xFF)
    return bytes(out)


def gif(path, frames):
    """An endless GIF of (rows, hundredths of a second) frames, with one palette of up to 256 colors."""
    colors = {}
    for rows, _ in frames:
        for r in rows:
            for i in range(0, len(r), 3):
                colors.setdefault(r[i:i + 3], len(colors))
    if len(colors) > 256:
        raise SystemExit('ui_shots: the tour has %d colors, over a GIF palette' % len(colors))
    bits = max(2, (len(colors) - 1).bit_length())
    w, h = len(frames[0][0][0]) // 3, len(frames[0][0])
    with open(path, 'wb') as f:
        f.write(b'GIF89a' + struct.pack('<HHBBB', w, h, 0xF0 | (bits - 1), 0, 0))
        f.write(b''.join(colors) + b'\0\0\0' * ((1 << bits) - len(colors)))
        f.write(b'\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00')
        for rows, delay in frames:
            data = lzw([colors[r[i:i + 3]] for r in rows for i in range(0, len(r), 3)], bits)
            f.write(b'\x21\xF9\x04\x00' + struct.pack('<H', delay) + b'\x00\x00')
            f.write(b'\x2C' + struct.pack('<HHHHB', 0, 0, w, h, 0) + bytes((bits,)))
            for i in range(0, len(data), 255):
                f.write(bytes((len(data[i:i + 255]),)) + data[i:i + 255])
            f.write(b'\x00')
        f.write(b'\x3B')


def main(argv):
    if len(argv) >= 2 and argv[1] == '--check':
        return check(argv[2] if len(argv) > 2 else os.path.join(os.path.dirname(HERE), 'docs', 'ui'))
    src, out, version = argv[1], argv[2], argv[3]
    for name in sorted(os.listdir(src)):
        if name.endswith('.raw'):
            convert(os.path.join(src, name), os.path.join(out, name[:-4] + '.png'), version)
    frames = []
    for name, delay in TOUR:
        with open(os.path.join(src, name + '.raw'), 'rb') as f:
            frames.append((frame_rows(f.read()), delay))
    gif(os.path.join(out, 'tour.gif'), frames)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
