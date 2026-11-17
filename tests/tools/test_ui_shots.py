#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""tools/ui_shots.py: each PNG carries the version it was drawn at, and --check fails on a picture drawn at
another version than the tree's, or carrying none."""
import contextlib
import io
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tools'))
import ui_shots  # noqa: E402

FRAME = bytes(2 * ui_shots.COLS * ui_shots.W)


class Version(unittest.TestCase):
    def check(self, versions):
        with tempfile.TemporaryDirectory() as d:
            for i, v in enumerate(versions):
                ui_shots.frame_png(FRAME, os.path.join(d, 'shot%d.png' % i), v)
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = ui_shots.check(d)
        return rc, err.getvalue()

    def test_carried(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'a.png')
            ui_shots.frame_png(FRAME, path, '1.2.3')
            self.assertEqual(ui_shots.png_version(path), '1.2.3')
            ui_shots.frame_png(FRAME, path)
            self.assertIsNone(ui_shots.png_version(path))

    def test_check(self):
        tree = ui_shots.tree_version()
        self.assertEqual(self.check([tree, tree]), (0, ''))
        rc, err = self.check([tree, '0.0.1'])
        self.assertEqual(rc, 1)
        self.assertIn('shot1.png (0.0.1)', err)
        rc, err = self.check([None])
        self.assertEqual(rc, 1)
        self.assertIn('no version', err)


if __name__ == '__main__':
    unittest.main()
