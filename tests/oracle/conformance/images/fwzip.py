# SPDX-License-Identifier: GPL-2.0-or-later
"""The upgrade zip carried inside a .tfw: builder and parser. A version file holds
"major.minor.patch+build", optionally followed by "@<hash>" and a newline."""

import io
import json
import re
import zipfile

ROOT = 'firmware/'
EXTRACT_ROOT = 'upgrade/'
PARTIAL_MARK = 'partial_upgrade'

COMPONENTS = {
    'os': ('firmware/blackfin/bin/os.ldr', 'firmware/blackfin/bin/os.version', 0x800000),
    'bootloader': ('firmware/blackfin/bin/bootloader.ldr', 'firmware/blackfin/bin/bootloader.version', 0x78000),
    'ble': ('firmware/ble/bin/ble.bin', 'firmware/ble/bin/ble.version', 0x6FFFF),
    'backplane': ('firmware/backplane/bin/backplane.cyacd', 'firmware/backplane/bin/backplane.version', 0x30000),
}
TOP_VERSION = 'firmware/firmware.version'
NORDIC_BOOTLOADERS = ('firmware/ble/bin/nordic_bootloader_0.bin', 'firmware/ble/bin/nordic_bootloader_1.bin')
NORDIC_BOOTLOADER_VERSION = 'firmware/ble/bin/nordic_bootloader.version'
DLC_DIR = 'firmware/blackfin/content/dlc/'

_VERSION_RE = re.compile(r'^(\d+)\.(\d+)\.(\d+)\+(\d+)(?:@([0-9A-Za-z]+))?\n?$')


def format_version(v, build=0, githash=None):
    s = '%d.%d.%d+%d' % (tuple(v[:3]) + (build,))
    return s + ('@%s\n' % githash if githash else '')


def version_from_text(text):
    if isinstance(text, bytes):
        text = text.decode('ascii')
    m = _VERSION_RE.match(text)
    if not m:
        raise ValueError('bad version file %r' % text)
    return {'version': tuple(int(x) for x in m.group(1, 2, 3)), 'build': int(m.group(4)), 'hash': m.group(5)}


def build(components=None, version=(1, 0, 0), build=0, githash=None, dlc=None, nordic_bootloaders=None,
          extra=None, compression=zipfile.ZIP_DEFLATED):
    """A zip. `components`: {name: bytes or (bytes, version tuple)} for names in COMPONENTS.
    `dlc`: {pack directory: manifest dict (or bytes)}. `nordic_bootloaders`: (bytes, bytes[,
    version]). `extra`: {path: bytes} added verbatim (for malformed tests)."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', compression) as z:
        z.writestr(TOP_VERSION, format_version(version, build, githash))
        for name, val in (components or {}).items():
            data, v = (val, version) if isinstance(val, (bytes, bytearray)) else val
            path, vpath, _ = COMPONENTS[name]
            z.writestr(path, bytes(data))
            z.writestr(vpath, format_version(v, build, githash))
        if nordic_bootloaders:
            z.writestr(NORDIC_BOOTLOADERS[0], nordic_bootloaders[0])
            z.writestr(NORDIC_BOOTLOADERS[1], nordic_bootloaders[1])
            v = nordic_bootloaders[2] if len(nordic_bootloaders) > 2 else version
            z.writestr(NORDIC_BOOTLOADER_VERSION, format_version(v, build, githash))
        for pack, man in (dlc or {}).items():
            z.writestr(DLC_DIR + pack + '/manifest.json', man if isinstance(man, bytes) else json.dumps(man))
        for path, data in (extra or {}).items():
            z.writestr(path, data)
    return buf.getvalue()


def extract_path(name):
    """Where the updater puts entry `name` ("firmware/x" -> "upgrade/x"); None outside firmware/
    or for a path that would leave the extraction root."""
    if not name.startswith(ROOT) or '..' in name.split('/'):
        return None
    return EXTRACT_ROOT + name[len(ROOT):]


def parse(data):
    """{'version', 'components': {name: {'data', 'version', 'size_ok'}}, 'dlc': {pack: manifest},
    'nordic_bootloaders', 'partial', 'outside_root': [names], 'problems': [str]}."""
    out = {'version': None, 'components': {}, 'dlc': {}, 'nordic_bootloaders': None, 'partial': False,
           'outside_root': [], 'problems': []}
    with zipfile.ZipFile(io.BytesIO(bytes(data))) as z:
        names = z.namelist()
        for n in names:
            if PARTIAL_MARK in n:
                out['partial'] = True
            if extract_path(n) is None:
                out['outside_root'].append(n)
        if TOP_VERSION in names:
            out['version'] = version_from_text(z.read(TOP_VERSION))
        else:
            out['problems'].append('no firmware.version')
        for name, (path, vpath, limit) in COMPONENTS.items():
            if path not in names:
                continue
            blob = z.read(path)
            v = version_from_text(z.read(vpath)) if vpath in names else None
            if v is None:
                out['problems'].append('%s without a version file' % name)
            ok = len(blob) <= limit
            if not ok:
                out['problems'].append('%s over 0x%x bytes' % (name, limit))
            out['components'][name] = {'data': blob, 'version': v, 'size_ok': ok}
        if all(p in names for p in NORDIC_BOOTLOADERS):
            out['nordic_bootloaders'] = tuple(z.read(p) for p in NORDIC_BOOTLOADERS)
        for n in names:
            if n.startswith(DLC_DIR) and n.endswith('/manifest.json') and n.count('/') == DLC_DIR.count('/') + 1:
                pack = n[len(DLC_DIR):-len('/manifest.json')]
                try:
                    out['dlc'][pack] = json.loads(z.read(n))
                except ValueError:
                    out['problems'].append('dlc %s manifest is not JSON' % pack)
    return out
