#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of tools/layering.py on source trees written here, and on this
one. Python 3, standard library only.

    test_layering.py
"""
import os
import shutil
import sys
import tempfile
import unittest

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(TOP, 'tools')
sys.path.insert(0, TOOLS)
import layering  # noqa: E402

# A tree whose every include stays in its layer.
CLEAN = {
    'src/include/waffle/boot/core.h': '#include <stdint.h>\n#include "waffle/util/bytes.h"\n',
    'src/include/waffle/util/bytes.h': '#include <stdint.h>\n',
    'src/include/waffle/testos.h': '',
    'src/include/waffle/board/opxy.h': '#include "waffle/drivers/bf70x_x.h"\n',
    'src/include/waffle/drivers/bf70x_x.h': '#include "waffle/drivers/mmr.h"\n',
    'src/include/waffle/drivers/mmr.h': '',
    'src/core/boot/core.c': '#include "waffle/boot/core.h"\n#include "zlib.h"\n',
    'src/board/opxy_a.c': ('#include "waffle/board/opxy.h"\n#include "waffle/boot/core.h"\n'
                           '#include "waffle/testos.h"\n'),
    'src/drivers/bf70x/x.c': ('#include "waffle/drivers/bf70x_x.h"\n#include "tusb.h"\n'
                              '#include "waffle/util/bytes.h"\n'),
    'src/testos/opxy_testos_a.c': ('#include "waffle/board/opxy.h"\n#include "waffle/boot/core.h"\n'
                                   '#include "waffle/drivers/mmr.h"\n#include "waffle/testos.h"\n'),
    'src/front/fr_a.c': '#include "waffle/boot/core.h"\n',
    'src/glue/g.c': '#include "waffle/drivers/bf70x_x.h"\n',
}


class Layering(unittest.TestCase):
    def setUp(self):
        self.top = tempfile.mkdtemp()
        self.write(CLEAN)

    def tearDown(self):
        shutil.rmtree(self.top)

    def write(self, files):
        for rel, text in files.items():
            path = os.path.join(self.top, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'w') as f:
                f.write(text)

    def fails(self, allowed=frozenset()):
        return layering.failures(self.top, allowed)

    def test_clean(self):
        self.assertEqual(self.fails(), [])

    def test_core_source_includes_board(self):
        self.write({'src/core/flash/b.c': '#include "waffle/board/opxy.h"\n'})
        self.assertEqual(self.fails(), ['src/core/flash/b.c (core) includes '
                                        'src/include/waffle/board/opxy.h (board)'])

    def test_core_source_includes_driver(self):
        self.write({'src/core/main.c': '  #  include <waffle/drivers/bf70x_x.h>\n'})
        self.assertEqual(len(self.fails()), 1)

    def test_core_source_includes_testos(self):
        self.write({'src/core/flash/b.c': '#include "waffle/testos.h"\n'})
        self.assertEqual(len(self.fails()), 1)

    def test_core_source_includes_testos_code(self):
        self.write({'src/core/flash/b.c': '#include "testos/opxy_testos_a.c"\n'})
        self.assertEqual(self.fails(), ['src/core/flash/b.c (core) includes '
                                        'src/testos/opxy_testos_a.c (testos)'])

    def test_neutral_includes_testos_code(self):
        self.write({'src/include/waffle/util/bytes.h': '#include "../../../testos/opxy_testos_a.c"\n'})
        self.assertEqual(len(self.fails()), 1)

    def test_core_header_includes_driver(self):
        self.write({'src/include/waffle/flash/b.h': '#include "waffle/drivers/mmr.h"\n'})
        self.assertEqual(self.fails(), ['src/include/waffle/flash/b.h (core) includes '
                                        'src/include/waffle/drivers/mmr.h (driver)'])

    def test_relative_include(self):
        self.write({'src/core/flash/b.c': '#include "../../board/local.h"\n',
                    'src/board/local.h': ''})
        self.assertEqual(len(self.fails()), 1)

    def test_driver_includes_core(self):
        self.write({'src/drivers/bf70x/y.c': '#include "waffle/boot/core.h"\n'})
        self.assertEqual(self.fails(), ['src/drivers/bf70x/y.c (driver) includes '
                                        'src/include/waffle/boot/core.h (core)'])

    def test_driver_header_includes_board(self):
        self.write({'src/include/waffle/drivers/bf70x_y.h': '#include "waffle/board/opxy.h"\n'})
        self.assertEqual(len(self.fails()), 1)

    def test_neutral_includes_core(self):
        self.write({'src/include/waffle/util/bytes.h': '#include "waffle/boot/core.h"\n'})
        self.assertEqual(self.fails(), ['src/include/waffle/util/bytes.h (neutral) includes '
                                        'src/include/waffle/boot/core.h (core)'])

    def test_neutral_includes_driver(self):
        self.write({'src/include/waffle/util/bytes.h': '#include "waffle/drivers/mmr.h"\n'})
        self.assertEqual(len(self.fails()), 1)

    def test_not_an_include(self):
        self.write({'src/core/flash/b.c': '/* #include "waffle/board/opxy.h" */\n'
                                          '#include "waffle/board/missing.h"\n'})
        self.assertEqual(self.fails(), [])

    def test_allowed_and_stale(self):
        self.write({'src/core/flash/b.c': '#include "waffle/board/opxy.h"\n'})
        entry = ('src/core/flash/b.c', 'src/include/waffle/board/opxy.h')
        self.assertEqual(self.fails({entry}), [])
        os.remove(os.path.join(self.top, 'src/core/flash/b.c'))
        self.assertEqual(self.fails({entry}), [
            'src/core/flash/b.c no longer includes src/include/waffle/board/opxy.h: '
            'remove it from ALLOWED'])

    def test_this_tree(self):
        self.assertEqual(layering.failures(), [])


if __name__ == '__main__':
    unittest.main()
