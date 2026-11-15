# SPDX-License-Identifier: GPL-2.0-or-later
"""USB host-side conformance oracles: USB descriptors, MTP 1.1 datasets and containers, and MSC
Bulk-Only / SCSI replies."""
from . import descriptors, expected, mtp, scsi  # noqa: F401

__all__ = ['descriptors', 'expected', 'mtp', 'scsi']
