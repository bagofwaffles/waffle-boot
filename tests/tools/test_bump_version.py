#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of tools/bump_version.py on copies of the board header. Python 3,
standard library only.

    test_bump_version.py
"""
import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(TOP, 'tools')
sys.path.insert(0, TOOLS)
import bump_version as b  # noqa: E402


class Bump(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, 'opxy_services.h')
        shutil.copyfile(b.HEADER, self.path)
        with open(self.path, 'rb') as f:
            self.orig = f.read()
        self.ver = b.current(self.orig.decode('utf-8'))

    def tearDown(self):
        shutil.rmtree(self.dir)

    def run_tool(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = b.main(['--header', self.path] + list(args))
        return rc, out.getvalue(), err.getvalue()

    def read(self):
        with open(self.path, 'rb') as f:
            return f.read()

    def write(self, data):
        with open(self.path, 'wb') as f:
            f.write(data)

    def only_patch_changed(self, n):
        """The file is the original with the patch's value, and nothing else, changed."""
        old = b'#define OPXY_BL_VERSION_PATCH %du' % self.ver[2]
        self.assertEqual(self.orig.count(old), 1)
        self.assertEqual(self.read(), self.orig.replace(old, b'#define OPXY_BL_VERSION_PATCH %du' % n))

    def test_show(self):
        rc, out, _ = self.run_tool('--show')
        self.assertEqual((rc, out), (0, '%d.%d.%d\n' % self.ver))
        self.assertEqual(self.read(), self.orig)

    def test_bump(self):
        rc, out, _ = self.run_tool()
        self.assertEqual((rc, out), (0, '%d.%d.%d\n' % (self.ver[0], self.ver[1], self.ver[2] + 1)))
        self.only_patch_changed(self.ver[2] + 1)
        rc, out, _ = self.run_tool()
        self.assertEqual(out, '%d.%d.%d\n' % (self.ver[0], self.ver[1], self.ver[2] + 2))
        self.only_patch_changed(self.ver[2] + 2)

    def test_set(self):
        rc, out, _ = self.run_tool('--set', '33')
        self.assertEqual((rc, out), (0, '%d.%d.33\n' % self.ver[:2]))
        self.only_patch_changed(33)
        rc, out, _ = self.run_tool('--set', '0')
        self.assertEqual(rc, 0)
        self.only_patch_changed(0)

    def test_set_out_of_range(self):
        for n in ('-1', '65536'):
            rc, _, err = self.run_tool('--set', n)
            self.assertEqual(rc, 1)
            self.assertIn('the patch must be', err)
            self.assertEqual(self.read(), self.orig)

    def test_crlf_kept(self):
        self.orig = self.orig.replace(b'\n', b'\r\n')
        self.write(self.orig)
        rc, _, err = self.run_tool('--set', '7')
        self.assertEqual(rc, 0, err)
        self.only_patch_changed(7)

    def refused(self, data, why):
        self.write(data)
        rc, out, err = self.run_tool()
        self.assertEqual((rc, out), (1, ''))
        self.assertIn(why, err)
        self.assertEqual(self.read(), data)

    def test_refuses_missing(self):
        line = b'#define OPXY_BL_VERSION_PATCH %du\n' % self.ver[2]
        self.refused(self.orig.replace(line, b''), 'found 0 #define lines for OPXY_BL_VERSION_PATCH')

    def test_refuses_twice(self):
        line = b'#define OPXY_BL_VERSION_PATCH %du\n' % self.ver[2]
        self.refused(self.orig.replace(line, line + line), 'found 2 #define lines')

    def test_refuses_other_values(self):
        line = b'#define OPXY_BL_VERSION_PATCH %du' % self.ver[2]
        for bad in (b'0x1u', b'1', b'01u', b'1U', b'(1u)', b'1u /* x */'):
            self.refused(self.orig.replace(line, b'#define OPXY_BL_VERSION_PATCH ' + bad),
                         'not a plain decimal with the u suffix')


if __name__ == '__main__':
    unittest.main()
