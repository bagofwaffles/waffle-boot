# SPDX-License-Identifier: GPL-2.0-or-later
"""OP-XY bootloader conformance packages: fs, serial, usb, images and hw.

Python 3, standard library only. Each subpackage stands alone; nothing is imported here. With the
directory that holds `conformance/` on sys.path:

    from conformance import images, serial, fs, usb, hw

Importing through `conformance` avoids the clash between the top-level name `serial` and pyserial.
"""

__all__ = ['fs', 'serial', 'usb', 'images', 'hw']
