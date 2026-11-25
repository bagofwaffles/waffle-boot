#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host tests of tools/front_image.py's checks of the frozen front: init on
synthetic ELF32 files laid out like front.ld, boundary on make dependency
files and a source tree written here, and stack on -fstack-usage lines.
Python 3, standard library only.

    test_front_image.py
"""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(TOP, 'tools')
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)
import elf32  # noqa: E402
import front_image  # noqa: E402
from test_image_tools import AX, WA, build_elf  # noqa: E402

L1CODE, L1DATA, L2 = 0x11A04000, 0x11802000, 0x08002000
CC_INCLUDE = '/opt/cc/lib/gcc/bfin-elf/8.0.0/include'


def front(init_relocs, data_relocs=(), other_relocs=()):
    """A front: _fr_init and _helper in L1 code, _table in L1 data, _fast in
    L1 code (not reached), and _memcpy and _l2_data in L2. Each reloc is
    (from symbol, to symbol, addend)."""
    syms = [('_fr_init', L1CODE, '.fr_l1code', 0x10), ('_helper', L1CODE + 0x10, '.fr_l1code', 0x10),
            ('_fast', L1CODE + 0x20, '.fr_l1code', 0x10), ('_table', L1DATA, '.fr_l1data', 8),
            ('_memcpy', L2, '.fr_l2', 0x10), ('_l2_data', L2 + 0x10, '.fr_l2', 0x10)]
    at = {n: v for n, v, _, _ in syms}
    sections = [('.fr_l1code', elf32.SHT_PROGBITS, AX, L1CODE, bytes(0x30)),
                ('.fr_l1data', elf32.SHT_PROGBITS, WA, L1DATA, bytes(8)),
                ('.fr_l2', elf32.SHT_PROGBITS, AX | WA, L2, bytes(0x20))]
    relocs = {}
    for sec, rs in (('.fr_l1code', list(init_relocs) + list(other_relocs)),
                    ('.fr_l1data', data_relocs)):
        for k, (src, dst, add) in enumerate(rs):
            relocs.setdefault(sec, []).append((at[src] + 4 + 2 * k, dst, add))
    return elf32.Elf(build_elf(sections, syms, relocs, L2))


class Init(unittest.TestCase):
    def test_closure_in_l1_passes(self):
        elf = front([('_fr_init', '_helper', 0), ('_helper', '_table', 0), ('_helper', '_fr_init', 4)],
                    data_relocs=[('_table', '_helper', 0)])
        self.assertEqual(front_image.init_failures(elf), [])

    def test_mmr_reference_passes(self):
        elf = front([('_fr_init', '_fr_init', 0x1FC01004 - L1CODE)])
        self.assertEqual(front_image.init_failures(elf), [])

    def test_unreached_l1_code_may_reach_l2(self):
        elf = front([('_fr_init', '_helper', 0)], other_relocs=[('_fast', '_memcpy', 0)])
        self.assertEqual(front_image.init_failures(elf), [])

    def test_call_into_l2_fails_naming_it(self):
        elf = front([('_fr_init', '_helper', 0), ('_helper', '_memcpy', 0)])
        bad = front_image.init_failures(elf)
        self.assertEqual(len(bad), 1)
        self.assertIn('_helper refers to _memcpy', bad[0])
        self.assertIn('L2', bad[0])

    def test_data_pointer_into_l2_fails(self):
        elf = front([('_fr_init', '_table', 0)], data_relocs=[('_table', '_l2_data', 0)])
        bad = front_image.init_failures(elf)
        self.assertEqual(len(bad), 1)
        self.assertIn('_table refers to _l2_data', bad[0])

    def test_ddr_reference_fails(self):
        elf = front([('_fr_init', '_fr_init', 0x80001000 - L1CODE)])
        bad = front_image.init_failures(elf)
        self.assertEqual(len(bad), 1)
        self.assertIn('DDR', bad[0])

    def test_no_relocations_fails(self):
        bad = front_image.init_failures(front([]))
        self.assertTrue(bad and '-q' in bad[0])


class Boundary(unittest.TestCase):
    def depfile(self, d, *paths):
        p = os.path.join(d, 'x.d')
        with open(p, 'w') as f:
            f.write('build/x.o: ' + ' \\\n '.join(paths) + '\n')
        return p

    def test_front_registers_and_compiler_headers_pass(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.depfile(d, 'src/front/slots/x.c', 'src/front/front.h', 'src/front/slots/slots.h',
                             'src/include/waffle/drivers/mmr.h', 'src/include/waffle/drivers/bf70x/usb.h',
                             CC_INCLUDE + '/stdint.h', CC_INCLUDE + '/../include/stddef.h')
            self.assertEqual(front_image.dep_failures(p, CC_INCLUDE), [])

    def test_bad_dependencies_fail(self):
        for path in ('../elsewhere/x.h', 'src/../../x.h', 'src/front/../../../x.h',
                     'src/include/waffle/flash/layout.h', 'src/s01.c', 'src/core/display/draw.h',
                     '/usr/include/string.h', '/opt/cc/bfin-elf/include/string.h',
                     CC_INCLUDE + '/../include-fixed/limits.h', CC_INCLUDE + '-other/x.h'):
            with tempfile.TemporaryDirectory() as d:
                p = self.depfile(d, 'src/front/slots/x.c', path)
                self.assertEqual(len(front_image.dep_failures(p, CC_INCLUDE)), 1, path)

    def tree(self, d, files):
        for rel, text in files.items():
            p = os.path.join(d, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'w') as f:
                f.write(text)

    BASE = {'src/front/front.h': '', 'src/front/handoff.h': '', 'src/front/slots/slots.h': '',
            'src/front/display/draw.h': '', 'src/front/slots/x.c': '#include "front/front.h"\n',
            'src/include/waffle/s01.h': ''}

    def test_shared_header_and_others_pass(self):
        with tempfile.TemporaryDirectory() as d:
            self.tree(d, dict(self.BASE, **{
                'src/board/a.c': '#include "front/handoff.h"\n#include "waffle/s01.h"\n'
                                 '#include <stdint.h>\n',
                'src/b.c': '#include <front/handoff.h>\n#include "handoff.h"\n',
                'src/include/c.h': '#include "front.h"\n', 'src/include/front.h': ''}))
            self.assertEqual(front_image.include_failures(d), [])

    def test_same_file_name_elsewhere_passes(self):
        """Headers outside the front that share a file name with a front header: an include
        that resolves to them is not the front's."""
        with tempfile.TemporaryDirectory() as d:
            self.tree(d, dict(self.BASE, **{
                'src/include/waffle/display/draw.h': '', 'src/core/display/draw.h': '',
                'src/core/display/draw.c': '#include "draw.h"\n#include "waffle/display/draw.h"\n',
                'src/core/a.c': '#include "display/draw.h"\n#include <waffle/display/draw.h>\n',
                'src/core/slots/slots.h': '', 'src/core/slots/slots.c': '#include "slots.h"\n'}))
            self.assertEqual(front_image.include_failures(d), [])

    def test_front_headers_fail_in_any_form(self):
        for inc in ('"front/front.h"', '<front/front.h>', '"../front/front.h"', '<../front/front.h>',
                    '"front.h"', '"src/front/front.h"', '"../../src/front/front.h"',
                    '"front/../front/front.h"', '"front/slots/x.c"', '"front/slots/slots.h"',
                    '<front/slots/slots.h>', '"../front/display/draw.h"', '"slots/slots.h"',
                    '"slots.h"', '"display/draw.h"', '"draw.h"'):
            with tempfile.TemporaryDirectory() as d:
                self.tree(d, dict(self.BASE, **{'src/board/a.c': ' #  include %s\n' % inc}))
                bad = front_image.include_failures(d)
                self.assertEqual(len(bad), 1, inc)
                self.assertIn('src/board/a.c', bad[0])

    def test_front_header_fails_beside_a_namesake(self):
        """front/slots/slots.h from outside the front fails even where a header of the same
        file name sits beside the including file."""
        with tempfile.TemporaryDirectory() as d:
            self.tree(d, dict(self.BASE, **{
                'src/core/slots/slots.h': '',
                'src/core/slots/slots.c': '#include "slots.h"\n#include "front/slots/slots.h"\n'}))
            bad = front_image.include_failures(d)
            self.assertEqual(bad, ['src/core/slots/slots.c includes "front/slots/slots.h"'])

    def test_front_may_include_itself(self):
        with tempfile.TemporaryDirectory() as d:
            self.tree(d, dict(self.BASE, **{
                'src/front/display/y.c': '#include "front/front.h"\n#include "front/slots/slots.h"\n'
                                         '#include "draw.h"\n'}))
            self.assertEqual(front_image.include_failures(d), [])


class Stack(unittest.TestCase):
    def test_parse(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'a.su')
            with open(p, 'w') as f:
                f.write('a.c:10:5:fr_a\t48\tstatic\na.c:20:13:helper\t0\tstatic\n')
            self.assertEqual(front_image.stack_usage(p),
                             [('a.c:10:5:fr_a', 48, 'static'), ('a.c:20:13:helper', 0, 'static')])

    def test_static_frames_within_limit_pass(self):
        bad, total = front_image.stack_failures([('a', 48, 'static'), ('b', 1024, 'static')], 0x4000)
        self.assertEqual((bad, total), ([], 1072))

    def test_dynamic_frame_fails(self):
        for qual in ('dynamic', 'dynamic,bounded'):
            bad, _ = front_image.stack_failures([('a', 48, qual)], 0x4000)
            self.assertEqual(len(bad), 1, qual)

    def test_large_frame_fails(self):
        bad, _ = front_image.stack_failures([('a', 1028, 'static')], 0x4000)
        self.assertEqual(len(bad), 1)
        self.assertIn('1028', bad[0])

    def test_sum_over_stack_fails(self):
        """Every frame within the limit, but twice their sum over FR_STACK: a chain could run
        out of stack."""
        frames = [('f%d' % k, 1000, 'static') for k in range(8)]
        bad, total = front_image.stack_failures(frames, 0x4000)
        self.assertEqual((bad, total), ([], 8000))
        bad, _ = front_image.stack_failures(frames + [('g', 200, 'static')], 0x4000)
        self.assertEqual(len(bad), 1)
        self.assertIn('16400', bad[0])

    def test_front_ld_stack(self):
        self.assertEqual(front_image.fr_stack(os.path.join(TOP, 'src', 'front', 'front.ld')), 0x4000)


if __name__ == '__main__':
    unittest.main(verbosity=1)
