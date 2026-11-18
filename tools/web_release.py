#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Puts a release's files where the page loads them from: docs/web/release/.

    tools/web_release.py VERSION waffle-install.ldr waffle-boot.ldr

It copies the installer file and the bootloader image and writes latest.json beside them: the
version, and each file's name, size and SHA-256, which the page holds the file against. Run it for
each release, with the release's own files.
"""

import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'docs', 'web', 'release')
INSTALLER, BOOT = 'waffle-install.ldr', 'waffle-boot.ldr'


def place(path, name):
    with open(path, 'rb') as f:
        data = f.read()
    with open(os.path.join(OUT, name), 'wb') as f:
        f.write(data)
    print('docs/web/release: %s, %d bytes' % (name, len(data)))
    return {'file': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def main(argv):
    if len(argv) != 4:
        sys.exit(__doc__)
    os.makedirs(OUT, exist_ok=True)
    meta = {'version': argv[1].lstrip('v')}
    meta.update(place(argv[2], INSTALLER))
    meta['boot'] = place(argv[3], BOOT)
    with open(os.path.join(OUT, 'latest.json'), 'w') as f:
        json.dump(meta, f, indent=2)
        f.write('\n')


if __name__ == '__main__':
    main(sys.argv)
