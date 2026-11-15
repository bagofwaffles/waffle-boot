# SPDX-License-Identifier: GPL-2.0-or-later
"""S-10b MTP: pending. Skips until its host program (h_s10b) exists; no test here carries a mark, so
COVERAGE.md shows S-10b as status.tsv gives it."""

import glob
import os
import unittest

from harness import BUILD, TOP, spec_path
from conformance.usb import mtp


def sources():
    return sorted(glob.glob(os.path.join(TOP, 'src', 's10b_*.c')))


class Mtp(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        if not sources():
            raise unittest.SkipTest('pending: no S-10b code in src/')
        if not os.path.exists(os.path.join(BUILD, 'h_s10b')):
            raise unittest.SkipTest('pending: S-10b code is in src/; h_s10b.c is not written yet')

    def test_vectors_present(self):
        """The inputs the drivers will replay are where they are expected."""
        self.assertTrue(glob.glob(spec_path('S-10b-mtp', 'vectors', '*.json')))
        self.assertTrue(callable(mtp.decode_data_reply))
