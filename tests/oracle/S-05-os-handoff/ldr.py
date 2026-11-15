# SPDX-License-Identifier: GPL-2.0-or-later
"""BF70x LDR block format: re-exports tests/oracle/conformance/images/ldr.py's builder and parser under the
names the S-01 and S-05 checks use.
"""
import os
import struct  # noqa: F401  (callers use ldr.struct)
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'conformance'))
from images.ldr import (SIGN, FINAL, FIRST, INDIRECT, IGNORE, INIT, CALLBACK, QUICKBOOT, FILL,  # noqa: E402,F401
                        AUX, SAVE, DMACODE_32BIT, LdrError, header, simple_os, destinations)
from images.ldr import build_ldr as build, parse_blocks as parse  # noqa: E402,F401
