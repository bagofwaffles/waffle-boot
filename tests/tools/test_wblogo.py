#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""tools/wblogo.py, the boot logo's reference: its command line from a PNG to the stored format and back,
the header's checks against a slot's record, the room a slot leaves, and the stored format's own rules
(runs inside their line, every pixel drawn, the background around a smaller picture). The C code is held
to this tool by tests/vectors/logo/ and the unit tests."""
import io
import contextlib
import os
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tools'))
import wblogo  # noqa: E402


def png_rgb(w, h, pix):
    raw = b''.join(b'\0' + bytes(v for x in range(w) for v in pix(x, y)) for y in range(h))

    def chunk(k, b):
        return struct.pack('>I', len(b)) + k + b + struct.pack('>I', zlib.crc32(k + b))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


class Tool(unittest.TestCase):
    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = wblogo.main(['wblogo.py'] + list(args))
        return rc, out.getvalue() + err.getvalue()

    def test_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            src, blob, back = (os.path.join(d, n) for n in ('a.png', 'a.bin', 'b.png'))
            with open(src, 'wb') as f:
                f.write(png_rgb(60, 30, lambda x, y: (255, 176, 32) if (x // 6 + y // 6) % 2 else (0, 0, 0)))
            rc, out = self.run_main('encode', src, blob, '--image-crc', '0x1234')
            self.assertEqual(rc, 0, out)
            self.assertIn('60 x 30, 2 colors (kept)', out)
            rc, out = self.run_main('info', blob)
            self.assertEqual(rc, 0, out)
            self.assertIn('60 x 30, 2 colors at 1 bits', out)
            rc, out = self.run_main('decode', blob, back)
            self.assertEqual(rc, 0, out)
            with open(back, 'rb') as f:
                w, h, px = wblogo.read_png(f.read())
            self.assertEqual((w, h), (480, 222))
            self.assertEqual(px[0], 0)                      # the background: the top-left pixel's
            self.assertEqual(px[96 * 480 + 210], 0)         # (210, 96): the picture's (0, 0)
            self.assertEqual(px[102 * 480 + 210], 0xFD84)   # its (0, 6)
            # too little room: nothing written
            rc, out = self.run_main('encode', src, os.path.join(d, 'c.bin'), '--room', '20')
            self.assertEqual(rc, 1)
            self.assertIn('does not fit', out)
            self.assertFalse(os.path.exists(os.path.join(d, 'c.bin')))
            with open(src, 'wb') as f:
                f.write(b'not a png')
            rc, out = self.run_main('encode', src, blob)
            self.assertEqual((rc, 'not a PNG file' in out), (1, True))


class Format(unittest.TestCase):
    def stored(self, room=0x10000):
        px = [((x * 7 + y * 3) % 5) * 0x1111 for y in range(9) for x in range(13)]
        fields, data, way = wblogo.convert(13, 9, px, room)
        return px, fields, data, way

    def test_header(self):
        _, fields, data, _ = self.stored()
        head = wblogo.header(fields, data, 0xCAFE)
        self.assertEqual(len(head), wblogo.HEAD)
        self.assertIsNotNone(wblogo.parse(head, 0x2345, 0xCAFE))
        self.assertIsNone(wblogo.parse(head, 0x2345, 0xCAFF))          # another image
        self.assertIsNone(wblogo.parse(head, 0x2345, 0xCAFE, 0xB005))  # no tag
        self.assertIsNone(wblogo.parse(head, 0x74001, 0xCAFE))         # no room
        self.assertIsNone(wblogo.parse(b'\xff' * wblogo.HEAD, 0x2345, 0xCAFE))
        for i in range(wblogo.HEAD):
            bad = bytearray(head)
            bad[i] ^= 4
            self.assertIsNone(wblogo.parse(bytes(bad), 0x2345, 0xCAFE), i)

    def test_room(self):
        for n, room in ((0, 0), (1, 0x74000), (0x1000, 0x74000), (0x1001, 0x73000), (0x74000, 0x1000),
                        (0x74001, 0), (0x75000, 0), (0x76000, 0)):
            self.assertEqual(wblogo.room(n), room, hex(n))

    def test_drawn(self):
        px, fields, data, _ = self.stored()
        f = dict(fields, len=len(data), crc=zlib.crc32(data))
        scr = wblogo.draw(f, data)
        x0, y0 = (480 - 13) // 2, (222 - 9) // 2
        for y in range(9):
            for x in range(13):
                self.assertEqual(scr[y0 + y][x0 + x], px[y * 13 + x])
        self.assertEqual(scr[0][0], px[0])
        # a run past its line, a byte left over, data cut short: no picture
        for cut in (data[:-1], data + b'\x00'):
            g = dict(f, len=len(cut), crc=zlib.crc32(cut))
            self.assertIsNone(wblogo.draw(g, cut))
        bad = bytearray(data)
        bad[2 * fields['colors']] = 0xFF
        g = dict(f, crc=zlib.crc32(bytes(bad)))
        self.assertIsNone(wblogo.draw(g, bytes(bad)))

    def test_ways(self):
        px = [(x * 2) << 11 | (y * 3) << 5 | ((x + y) % 32) for y in range(20) for x in range(16)]
        _, data256, way = wblogo.convert(16, 20, px, 0x10000)
        self.assertEqual(way, 'reduced')
        _, data16, way = wblogo.convert(16, 20, px, len(data256) - 1)
        self.assertEqual(way, 'dithered')
        self.assertLess(len(data16), len(data256))
        with self.assertRaises(wblogo.LogoError):
            wblogo.convert(16, 20, px, len(data16) - 1)
        # the same input, the same bytes
        self.assertEqual(wblogo.convert(16, 20, px, len(data256) - 1)[1], data16)


if __name__ == '__main__':
    unittest.main()
