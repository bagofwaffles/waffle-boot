# SPDX-License-Identifier: GPL-2.0-or-later
"""S-11 executable model: the package checks, zip checks, disk search, step results, run, content
installation, the boot after an interrupted install and the return-to-factory predicate.

Python 3, standard library plus tests/oracle/conformance (images.aes, images.p224, images.crc, images.ldr).
Every key used with it is a conformance test key; the model holds no key of its own. The signature is checked
but never required (S-11.012, S-11.022): it sets the signature state (S-11.021), valid, invalid or none.
"""
import hashlib
import io
import itertools
import json
import os
import struct
import zipfile
import zlib

from images import aes, ldr, p224
from images.crc import crc16_xmodem, crc32

PREAMBLE_MAGIC = 0xBABECAFE
HEADER_MAGIC = 0xBEEFCAFE
PRE, HDR, MAN, NAME = 0x40, 0x40, 0x300, 0x100
PRODUCT = 0x21


def _s18(name):
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'S-18-devices', 'vectors',
                           name + '.json'), encoding='utf-8') as f:
        return json.load(f)


def _profile(name, device):
    for rows in _s18('profile')['tables'].values():
        for r in rows:
            if r['name'] == name:
                return r[device]
    raise KeyError(name)


def has_feature(name, device):
    return next(r[device] for r in _s18('features')['features'] if r['name'] == name)


def product_number(device='OP-XY'):
    """device.product_number (S-18.030): 0x21 on the OP-XY, 0x02 on the OP-1F."""
    return _profile('device.product_number', device)


def product_base(device='OP-XY'):
    """device.product_base, the word a writer stores."""
    return _profile('device.product_base', device)


def device_ids(device='OP-XY'):
    """S-11.006, S-11.026: the device ids that pass the third check; 0x06 is the OP-XY's alone."""
    return (0x01, 0x06) if device == 'OP-XY' else (0x01,)

# S-11.020, in check order. Reason -> result code.
# Check 8 refuses nothing (S-11.022) and check 9 is not made (S-11.013 withdrawn): no reason of their own.
REASONS = ['SHORT', 'PREAMBLE', 'TYPE', 'MAGIC', 'PRODUCT', 'DEVICE', 'CRC', 'FORMAT',
           'LENGTH', 'HASH', 'KEY', 'NOKEY', 'NAME', 'ZIPLEN', 'ZIP', 'MEMBER', 'SPACE', 'SPACEQUERY', 'SIZE',
           'STREAM']
REASON_CODE = {r: 3 for r in REASONS}
REASON_CODE.update(SPACE=1, SPACEQUERY=4)

# S-11.058
OK, FAILURE, NOT_FOUND, BAD_FILE, INTERNAL, CRC_ERROR, ALREADY, DOWNGRADE, SKIPPED = range(9)
RESULT_NAMES = ['OK', 'Failure', 'Not found', 'Bad file', 'Internal error', 'CRC error', 'Already installed',
                'Downgrade rejected', 'Skipped']
CONTINUE = {OK, NOT_FOUND, ALREADY, DOWNGRADE, SKIPPED}
STEPS = ['content', 'os', 'bootloader', 'ble', 'ble bootloader', 'keyboard', 'reserved']
RESERVED = 6                 # S-11.057: step 6 does nothing and reports Not found
PROTECTED = {2}
SWITCHABLE = (3, 4, 5)       # S-11.073: the steps that can be switched off
STEPS_OFF = ()               # S-11.073: the steps this version has switched off: none
NORDIC_STEPS = (3, 4)
DONE = 7
# S-11.096: the steps a selection can hold, and the protected ones it holds only when asked
SELECTABLE = (0, 1, 2, 3, 4, 5)
# Why a step whose component is present reports Skipped (S-11.058); the host's words (S-10c.106)
SKIP_OPTION_OFF, SKIP_NOT_SELECTED, SKIP_SWITCHED_OFF = 'option off', 'not selected', 'switched off'
SKIP_ALREADY_WRITTEN = 'already written'    # S-11.068 item 5
UPGRADING = 0xAB0D   # the metadata state of an install in progress (S-04.040)

# S-11.034
COMPONENTS = {
    'os': ('firmware/blackfin/bin/os.ldr', 'firmware/blackfin/bin/os.version', 0x800000),
    'bootloader': ('firmware/blackfin/bin/bootloader.ldr', 'firmware/blackfin/bin/bootloader.version', 0x78000),
    'ble': ('firmware/ble/bin/ble.bin', 'firmware/ble/bin/ble.version', 0x6FFFF),
    'nordic_bl0': ('firmware/ble/bin/nordic_bootloader_0.bin', 'firmware/ble/bin/nordic_bootloader.version', 0x3000),
    'nordic_bl1': ('firmware/ble/bin/nordic_bootloader_1.bin', 'firmware/ble/bin/nordic_bootloader.version', 0x3000),
    'backplane': ('firmware/backplane/bin/backplane.cyacd', 'firmware/backplane/bin/backplane.version', 0x30000),
}
CONTENT_DIR = 'firmware/blackfin/content/'
DLC_DIR = CONTENT_DIR + 'dlc/'
FW_VERSION = 'firmware/firmware.version'
SPACE_RESERVE = 0x400000
UNKNOWN_VERSION = (1234, 0, 0, 0)


class Unit:
    """What the checks need of the unit: K0, K1 (32 bytes or None), the option, dev mode, and the cloud
    public key Q0 (a P-224 point, or None when it cannot be read), which only sets the signature state."""

    def __init__(self, k0=None, k1=None, option=False, dev=False, q0=None):
        self.k0, self.k1, self.option, self.dev, self.q0 = k0, k1, option, dev, q0


# ---- S-11.007, S-11.008, S-11.036, S-11.037

def product_ok(w, device='OP-XY'):
    return (w >> 14) & 0x3FF == product_number(device) and (w >> 10) & 0xF == 0


def crc16(data):
    return crc16_xmodem(data)


def _num(s, i):
    while i < len(s) and s[i] in b' \t':
        i += 1
    v = 0
    while i < len(s) and 0x30 <= s[i] <= 0x39:
        v = v * 10 + s[i] - 0x30
        i += 1
    return v % 65536, i


def version_from_text(data):
    """S-11.036. `data`: the file's bytes, or None when absent."""
    if data is None:
        return UNKNOWN_VERSION
    s = bytes(data[:32])
    d1 = s.find(b'.')
    if d1 < 0:
        return UNKNOWN_VERSION
    d2 = s.find(b'.', d1 + 1)
    if d2 < 0:
        return UNKNOWN_VERSION
    major, _ = _num(s, 0)
    minor, _ = _num(s, d1 + 1)
    patch, _ = _num(s, d2 + 1)
    plus = s.find(b'+', d2 + 1)
    build = _num(s, plus + 1)[0] if plus >= 0 else 0
    return (major, minor, patch, build)


def vcmp(a, b):
    a, b = tuple(a), tuple(b)
    return (a > b) - (a < b)


def is_0_1_0(v):
    return tuple(v[:3]) == (0, 1, 0)


# ---- S-11.012, S-11.021: the signature and its state

SIG_VALID, SIG_INVALID, SIG_NONE = 'valid', 'invalid', 'none'


def signature_present(man):
    """S-11.012: a signature is present when sig_r (+0x10) and sig_s (+0x30) are not both zero."""
    return any(man[0x10:0x50])


def signature_verifies(pub, man):
    """S-11.012: ECDSA P-224 over SHA-256 of the manifest with +0x010-+0x04F zeroed; r and s from the last 28
    bytes of their fields; the key the signature key index selects, Q0 for 0x00 and none for any other."""
    if pub is None or man[0x89] != 0x00:
        return False
    digest = hashlib.sha256(man[:0x10] + bytes(0x40) + man[0x50:MAN]).digest()
    r = int.from_bytes(man[0x14:0x30], 'big')
    s = int.from_bytes(man[0x34:0x50], 'big')
    return p224.verify(pub, digest, r, s)


def signature_state(man, q0, plain=False):
    """S-11.021: 'valid' (a present signature that verifies against Q0), 'invalid' (a present one that does
    not, or Q0 None: not readable) or 'none' (a plain package, S-11.027, or a blank signature). It never
    refuses a package (S-11.022)."""
    if plain or not signature_present(man):
        return SIG_NONE
    return SIG_VALID if signature_verifies(q0, man) else SIG_INVALID


# ---- S-11.001-S-11.023: checks 1-15

def check_package(data, form, unit, file_name=None, link=None, device='OP-XY'):
    """Checks 1-15. form: 'file' (RAM disk) or 'body' (host link). Returns a dict: accepted, reason,
    signature (S-11.021's state once check 8 is reached, else None), forced, version, zip, name, plain (the
    OP-1F's plain format, S-11.027)."""
    data = bytes(data)
    out = dict(accepted=False, reason=None, signature=None, forced=False, version=None, zip=None, name=None,
               plain=False)

    def refuse(r):
        out['reason'] = r
        return out

    forced = False
    o = 0
    if form == 'file':
        if len(data) < PRE:
            return refuse('SHORT')
        magic, typ = struct.unpack_from('>IB', data)
        if magic != PREAMBLE_MAGIC:
            return refuse('PREAMBLE')
        if typ != 0:
            return refuse('TYPE')
        if is_0_1_0(struct.unpack_from('>4H', data, 7)):
            forced = True
        if file_name is not None and 'force' in file_name:
            forced = True
        o = PRE
    elif link == 'cdc':
        forced = True
    if len(data) < o + HDR:
        return refuse('SHORT')
    magic, dev = struct.unpack_from('>IB', data, o)
    if magic != HEADER_MAGIC:
        return refuse('MAGIC')
    hcrc = struct.unpack_from('>H', data, o + 9)[0]
    version = struct.unpack_from('>4H', data, o + 0x0B)
    hprod = struct.unpack_from('>I', data, o + 0x17)[0]
    if not product_ok(hprod, device):
        return refuse('PRODUCT')
    if dev not in device_ids(device):
        return refuse('DEVICE')
    if dev == 0x06 or is_0_1_0(version):
        forced = True
    out['version'] = version
    out['forced'] = forced
    m = o + HDR
    if crc16(data[m:]) != hcrc:
        return refuse('CRC')
    if len(data) < m + MAN:
        return refuse('SHORT')
    man = data[m:m + MAN]
    payload = data[m + MAN:]
    # S-11.027: the tag tells the format; on the OP-1F a block without TESG is the plain format's crypto
    # header, not a refusal. A TESG block's version must be 0x01 (S-11.011).
    plain = man[0x84:0x88] != b'TESG'
    if (plain and device == 'OP-XY') or (not plain and man[0x88] != 1):
        return refuse('FORMAT')
    out['plain'] = plain
    # check 8 sets the signature state and refuses nothing (S-11.022); check 9 is not made: the manifest
    # product word is not read (S-11.013 withdrawn)
    out['signature'] = signature_state(man, unit.q0, plain)
    if not plain:
        if struct.unpack_from('<I', man, 0x98)[0] != len(payload):
            return refuse('LENGTH')
        if hashlib.sha256(payload).digest() != man[0x9C:0xBC]:
            return refuse('HASH')
    # checks 10 and 11 are integrity checks only (S-11.014). plain: checks 10 and 11 are not made; the zip length is enforced at check 14. The key rule is the same for
    # both formats.
    kidx = man[0x04]
    key = None
    if kidx == 0x00:
        if unit.k0 is None:
            return refuse('NOKEY')
        key = unit.k0
    elif kidx == 0x01:
        if not unit.option:
            return refuse('KEY')
        if unit.k1 is None:
            return refuse('NOKEY')
        key = unit.k1
    elif kidx != 0xFE:
        return refuse('KEY')
    zlen = struct.unpack_from('<I', man, 0x80)[0]
    iv = man[0x70:0x80]
    if len(payload) < NAME:
        return refuse('NAME')
    name_block = payload[:NAME] if key is None else aes.cbc_decrypt(key, iv, payload[:NAME])
    nm = name_block[:0x7C].split(b'\0', 1)[0]
    if b'.zip' not in nm:
        return refuse('NAME')
    out['name'] = nm
    padded = -(-zlen // 16) * 16 if key is not None else zlen
    if len(payload) < NAME + padded:
        return refuse('ZIPLEN')
    clear = payload[:NAME + padded] if key is None else aes.cbc_decrypt(key, iv, payload[:NAME + padded])
    z = clear[NAME:NAME + zlen]
    try:
        zipfile.ZipFile(io.BytesIO(z)).infolist()
    except Exception:
        return refuse('ZIP')
    out.update(accepted=True, zip=z)
    return out


# ---- S-11.030-S-11.035: checks 16-17

def _extra_ids(extra):
    ids, i = [], 0
    while i + 4 <= len(extra):
        hid, ln = struct.unpack_from('<HH', extra, i)
        ids.append(hid)
        i += 4 + ln
    return ids


def _has_zip64(z, infos):
    """zip64 end records, or a zip64 extra field (id 1) in a central or local header."""
    tail = z[-(0x10000 + 22):]
    if b'PK\x06\x06' in tail or b'PK\x06\x07' in tail:
        return True
    for i in infos:
        if 1 in _extra_ids(i.extra):
            return True
        o = i.header_offset
        if z[o:o + 4] == b'PK\x03\x04':
            nlen, xlen = struct.unpack_from('<HH', z, o + 26)
            if 1 in _extra_ids(z[o + 30 + nlen:o + 30 + nlen + xlen]):
                return True
    return False


def _name_ok(n):
    if not n or '\\' in n:
        return False
    parts = n.split('/')
    if parts[-1] == '':        # a directory member's trailing '/'
        parts = parts[:-1]
    return all(p not in ('', '.', '..') for p in parts)


def space_verdict(sizes, free_space):
    """S-11.033: None when the members fit, else the reason. Sizes are summed without overflow."""
    if free_space is None:
        return 'SPACEQUERY'
    return 'SPACE' if free_space < sum(sizes) + SPACE_RESERVE else None


def check_zip(z, free_space):
    """Checks 16-17. free_space: bytes free on the volume, or None when it cannot be determined.
    Returns (reason or None, staging dict {path under upgrade/: bytes, or None for a directory})."""
    try:
        zf = zipfile.ZipFile(io.BytesIO(z))
        infos = zf.infolist()
    except Exception:
        return 'MEMBER', None
    if _has_zip64(z, infos):
        return 'MEMBER', None
    for i in infos:
        if i.compress_type not in (0, 8) or i.flag_bits & 1 or not _name_ok(i.filename):
            return 'MEMBER', None
    v = space_verdict([i.file_size for i in infos], free_space)
    if v:
        return v, None
    staged = {}
    for i in infos:
        if i.filename.endswith('/'):
            staged[i.filename.rstrip('/')] = None
            continue
        try:
            data = zf.read(i)
        except (zipfile.BadZipFile, zlib.error, EOFError, ValueError):
            return 'MEMBER', None
        if len(data) != i.file_size:
            return 'MEMBER', None
        staged[i.filename] = data
    for name, (img, _, limit) in COMPONENTS.items():
        if staged.get(img) and len(staged[img]) > limit:
            return 'SIZE', staged
    return None, staged


# S-11.035 check 18: the protected components are bootable streams (S-02.091, S-01 stream rules).
FACTORY_FS_STACK = (0x11807000, 0x11808000)   # S-02.091 rule 4


def _check_stream():
    import os
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'S-01-boot-chain'))
    import check_stream
    return check_stream


def _kernel_model():
    import os
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'S-05-os-handoff'))
    import kernel_model
    return kernel_model


def stream_verdict(name, data):
    """True when the bootloader image meets S-02.091, or the OS image S-05.024 (other components: True)."""
    cs = _check_stream()
    if name == 'os':
        km = _kernel_model()
        try:
            km.compatibility_check(data)
        except km.Refused:
            return False
        return True
    if name == 'bootloader':
        res = cs.check('bootloader', data, FACTORY_FS_STACK)
    else:
        return True
    return all(ok for _, ok, _ in res)


def check_streams(staged):
    """Check 18 (S-11.035): 'STREAM' when a present bootloader image is not a bootable slot stream, or a
    present OS image fails S-05.024, whether or not the run would install it; else None."""
    for name in ('os', 'bootloader'):
        img = COMPONENTS[name][0]
        if staged.get(img) and not stream_verdict(name, staged[img]):
            return 'STREAM'
    return None


def check_members(z, free_space):
    """Checks 16-18: check_zip, then check_streams on what it staged."""
    reason, staged = check_zip(z, free_space)
    if reason is None:
        reason = check_streams(staged)
    return reason, staged


def components(staged):
    """S-11.034: {name: (bytes, version)} of the present components."""
    out = {}
    for name, (img, ver, _) in COMPONENTS.items():
        b = staged.get(img)
        if b:
            out[name] = (b, version_from_text(staged.get(ver)))
    return out


def has_content(staged):
    """The staging area has the directory firmware/blackfin/content/ (a file under it, or the directory
    itself, which stays after a content step has moved its files out)."""
    return any(p.startswith(CONTENT_DIR) or p == CONTENT_DIR.rstrip('/') for p in staged)


def content_only(staged):
    """S-11.056: decided from the staging area."""
    return not components(staged) and has_content(staged)


def package_version(staged):
    """S-11.038: the version recorded and reported is the staged firmware/firmware.version's (S-11.036).
    The TE header version only decides S-11.055's 0.1.0 marker. None when the file is absent. A present
    file is read by S-11.036, so one that lacks a '.' gives 1234.0.0.0."""
    if staged.get(FW_VERSION) is None:
        return None
    return version_from_text(staged.get(FW_VERSION))


def has_version_file(staged):
    return staged.get(FW_VERSION) is not None


def is_partial(staged):
    return any(p.startswith(CONTENT_DIR) and 'partial_upgrade' in p[len(CONTENT_DIR):] and staged[p] is not None
               for p in staged)


def pack_name(staged):
    """S-11.039: listing order as the sorted order of the pack directories."""
    packs = sorted({p[len(DLC_DIR):].split('/')[0] for p in staged if p.startswith(DLC_DIR) and
                    '/' in p[len(DLC_DIR):]})
    for pk in packs:
        mf = staged.get(DLC_DIR + pk + '/manifest.json')
        if mf:
            try:
                name = json.loads(mf.decode('utf-8', 'replace')).get('name', '')
            except ValueError:
                name = ''
            return name if isinstance(name, str) else ''
    return ''


# ---- S-11.046-S-11.048: the disk search

def disk_search(entries):
    """entries: [(name, is_dir)] in directory order. Returns the package's name or None."""
    for name, is_dir in entries:
        if is_dir or name.startswith('.'):
            continue
        if 'tfw' in name:
            return name
    return None


def disk_install(files, unit):
    """S-11.046-S-11.048, the disk path end to end. files: [(name, is_dir, bytes)] of the root directory in
    directory order. Returns (package name or None, check_package result or None). The zip an accepted
    package installs is that result's 'zip', recovered from the package itself; no other file is read."""
    name = disk_search([(n, d) for n, d, _ in files])
    if name is None:
        return None, None
    data = next(b for n, d, b in files if n == name and not d)
    return name, check_package(data, 'file', unit, file_name=name)


def cdc_please(entries):
    return any(not d and not n.startswith('.') and 'cdc' in n and 'please' in n for n, d in entries)


# ---- S-11.095: the check of a staged image

def staged_check(stated, reads):
    """stated: (length, crc32) kept from the zip for the run. reads: the bytes of the file's read, as a
    one-element list, None for a read that fails. Returns 'ok', 'bad' or 'read_error'."""
    first = reads[0]
    if first is None:
        return 'read_error'
    return 'ok' if (len(first), zlib.crc32(first) & 0xFFFFFFFF) == tuple(stated) else 'bad'


def _staged_result(c):
    """The step's result when the check of S-11.095 does not pass, or None."""
    return {'ok': None, 'bad': BAD_FILE, 'read_error': INTERNAL}[c.get('staged', 'ok')]


# ---- S-11.066-S-11.071: step results

def switched_on(step, c):
    """S-11.073: c's `switched_on`, else this version's state."""
    return bool(c.get('switched_on', step not in STEPS_OFF))


def _already(step, c):
    """S-11.069-S-11.071: the installed version is the package's and the package is not forced."""
    return step in SWITCHABLE and c.get('rel') == 'equal' and not c.get('forced', False)


def skip_reason(step, c):
    """S-11.058, S-11.073, S-11.090, S-11.096: why the step reports Skipped, or None when it does not.
    c's `selected`: the step selected on the unit (S-11.096), selected when not given. The signature state
    (S-11.021) plays no part."""
    selected = c.get('selected', 1)
    if step == 0:
        return SKIP_NOT_SELECTED if c.get('content_dir') and not selected else None
    if step == 1:
        return SKIP_NOT_SELECTED if c.get('present') and not selected else None
    if step in PROTECTED:
        if not c.get('present'):
            return None
        if step in SWITCHABLE and not switched_on(step, c):
            return SKIP_SWITCHED_OFF        # S-11.073: before the option and the selection
        if not c.get('option'):
            return SKIP_OPTION_OFF
        if not selected:
            return SKIP_NOT_SELECTED
        # S-11.068 item 5: after the image check, a start refused only for `verified`, the selector being
        # the one this power-on's own bootloader step wrote, and the selected slot's record that of the
        # checked image
        if (step == 2 and _staged_result(c) is None and c.get('start') == 'refused_verified'
                and c.get('written_here') and c.get('slot_matches')):
            return SKIP_ALREADY_WRITTEN
        return None
    if step in SWITCHABLE:
        carried = c.get('present0') or c.get('present1') if step == 4 else c.get('present')
        if not carried:
            return None
        if not switched_on(step, c):
            return SKIP_SWITCHED_OFF
        if _already(step, c):
            return None                     # Already installed, before the selection
        return None if selected else SKIP_NOT_SELECTED
    return None


def step_result(step, c):
    """c: a dict of conditions (see steps.tsv). Returns a result code. `selected` (S-11.096) and
    `switched_on` (S-11.073) default to selected and to this version's state."""
    forced = c.get('forced', False)
    staged = _staged_result(c)
    if skip_reason(step, c) is not None:
        return SKIPPED
    if step == 0:
        if not c.get('content_dir'):
            return NOT_FOUND
        if c.get('move_fails'):
            return FAILURE
        return OK if c.get('files', 0) > 0 else NOT_FOUND
    if step == 1:
        if not c.get('present'):
            return NOT_FOUND
        if staged is not None:
            return staged
        return OK if c.get('write_ok', True) else INTERNAL
    if step == 2:
        if not c.get('present'):
            return NOT_FOUND
        # S-11.068: with the option on there is no version test; rel, forced and dev change nothing.
        if staged is not None:
            return staged
        if c.get('start', 'ok') != 'ok':
            return FAILURE                          # S-02.090 refused the start (item 5 aside)
        return OK if c.get('write_ok', True) else FAILURE
    if step == 3:
        if not c.get('present'):
            return NOT_FOUND
        if c.get('rel') == 'equal' and not forced:
            return ALREADY
        if staged is not None:
            return staged
        if not c.get('in_dfu', True):
            return FAILURE                          # S-11.069 item 5: the step's own queries, after the check
        return OK if c.get('write_ok', True) else FAILURE
    if step == 4:
        if not c.get('present0') and not c.get('present1'):
            return NOT_FOUND
        if c.get('rel') == 'equal' and not forced:
            return ALREADY
        slot = c.get('slot', 0)
        if slot not in (0, 1):
            return FAILURE
        if not c.get('present%d' % slot):
            return NOT_FOUND
        if staged is not None:
            return staged
        if not c.get('in_dfu', True):
            return FAILURE                          # S-11.070 item 7: as S-11.069 item 5
        return OK if c.get('write_ok', True) else FAILURE
    if step == 5:
        if not c.get('present'):
            return NOT_FOUND
        if c.get('rel') == 'equal' and not forced:
            return ALREADY
        if staged is not None:
            return staged
        return OK if c.get('write_ok', True) else FAILURE
    if step == RESERVED:
        return NOT_FOUND                            # S-11.057: nothing is looked for
    raise ValueError(step)


def end_nordic(step, c):
    """S-11.064 item 3 after a step with conditions c stopped or ended the run: 'no_exit' when its result is a
    Failure of the Nordic application step after its prepare was sent (S-08.048; item 5's queries answered
    "in DFU", so the transfer began with the prepare), otherwise 'exit' (the query, and the exit when in
    DFU)."""
    if step == 3 and step_result(step, c) == FAILURE and c.get('in_dfu', True):
        return 'no_exit'
    return 'exit'


# ---- S-11.057-S-11.065: the run

def dfu_entry(nordic_steps, steps_off=STEPS_OFF, selected=None):
    """S-11.061 item 2: the DFU entry is asked when the package carries a Nordic image (nordic_steps: the
    Nordic steps, 3 and 4, whose images it carries; True for both) whose step is switched on and selected
    (S-11.096; selected None: every step)."""
    if nordic_steps is True:
        nordic_steps = NORDIC_STEPS
    return any(s not in steps_off and (selected is None or s in selected) for s in (nordic_steps or ()))


def run(results, content_only=False, nordic_images=False, nordic_dfu_ok=True, version_file=True,
        steps_off=(), selected=None):
    """results: {step: code} for the steps that would run. Every run starts at step 0 and its records
    carry count 0 (S-11.061, S-11.062). Returns a dict: outcome, ran (list of (step, code)), writes (ordered
    list of ('meta', what) / ('progress', (step, 0)) / ('nordic', what)).
    nordic_images: the Nordic steps whose images the package carries (True: both); with steps_off, the
    switched-off steps (S-11.073), and selected, the steps selected (S-11.096; None: every step), they
    decide the DFU entry (dfu_entry).
    version_file: the staging area holds firmware/firmware.version (S-11.038). A completed run that is not
    content-only records the package version with state 0 only when its step 1 installed the OS (S-11.064);
    otherwise, and without the file, it sets the state to 0 only. A failed run that is not content-only
    sets the state to 0 (S-11.064, S-04.045 M4)."""
    w = []
    if not content_only:
        w.append(('meta', 'state=AB0D'))
    if dfu_entry(nordic_images, steps_off, selected):
        w.append(('nordic', 'enter_dfu'))
        if not nordic_dfu_ok:
            if not content_only:
                w.append(('meta', 'state=0'))
            return dict(outcome='failed', ran=[], writes=w)
    w.append(('nordic', 'read_versions'))
    w.append(('progress', (0, 0)))
    ran, step = [], 0
    while step != DONE:
        r = results.get(step, NOT_FOUND)
        ran.append((step, r))
        if r not in CONTINUE:
            break
        step += 1
        w.append(('progress', (step, 0)))
    if step == DONE:
        if not content_only:
            os_ok = (1, OK) in ran
            w.append(('meta', 'os_version=package,state=0' if version_file and os_ok else 'state=0'))
        w.append(('progress', (0, 0)))
        nothing = all(r in (NOT_FOUND, ALREADY, SKIPPED) for _, r in ran)
        outcome = 'nothing to do' if nothing else 'done'
    else:
        if not content_only:
            w.append(('meta', 'state=0'))
        outcome = 'failed'
    w.append(('nordic', 'exit_dfu_if_in_dfu'))
    return dict(outcome=outcome, ran=ran, writes=w)


# ---- S-11.080: the boot that finds an interrupted install

def interrupted_boot(state, keys_held=False, slave_present=True, meta_write_ok=True):
    """One boot's handling of the upgrade state. Returns a dict: decision ('os', 'held'), writes (the
    metadata writes attempted, in order), notice (S-15.162), state_after, and the steps, Nordic DFU
    exchanges, record and staging-area accesses made for the install (always none, S-11.080). The keys and
    the slave change nothing when the state is 0xAB0D. With state 0 the keys decide (S-05.001; the reason
    is not modeled here)."""
    if state != UPGRADING:
        return dict(decision='held' if keys_held else 'os', writes=[], notice=False, state_after=state,
                    steps=0, nordic_dfu=0, record_access=0)
    return dict(decision='held', writes=['state=0'], notice=True, state_after=0 if meta_write_ok else UPGRADING,
                steps=0, nordic_dfu=0, record_access=0)


# ---- S-11.075-S-11.079: content

def install_content(tree, staged, content_only_install, device='OP-XY'):
    """tree: {path relative to /yaffs2: bytes}. staged: the staging dict. Returns (result, new tree).
    Without content-packs no directory is DLC (S-11.097): a full install removes all of content/."""
    t = dict(tree)
    packs_feature = has_feature('content-packs', device)
    files = {p[len(CONTENT_DIR):]: b for p, b in staged.items() if p.startswith(CONTENT_DIR) and b is not None}
    if not has_content(staged):
        return NOT_FOUND, t
    partial = is_partial(staged)
    if not partial and not packs_feature:
        t = {p: b for p, b in t.items() if not p.startswith('content/')}
    elif not partial:
        t = {p: b for p, b in t.items() if not p.startswith('content/') or p.startswith('content/dlc/')}
    elif content_only_install and packs_feature:
        packs = {p[len(DLC_DIR):].split('/')[0] for p in staged if p.startswith(DLC_DIR)}
        for pk in packs:
            if pk and not pk.startswith('.'):
                t = {p: b for p, b in t.items() if not p.startswith('content/dlc/%s/' % pk)}
    n = 0
    for rel, b in sorted(files.items()):
        if 'partial_upgrade' in rel:
            continue
        t['content/' + rel] = b
        n += 1
    if not content_only_install and FW_VERSION in staged and staged[FW_VERSION] is not None:
        t['firmware.version'] = staged[FW_VERSION]
    return (OK if n else NOT_FOUND), t


# ---- S-11.091, S-11.092

def return_to_stock(option, bootloader_waffle, has_bootloader, bootloader_selected=True, valid=True):
    """S-11.091: the option on, the package valid (S-11.021), the bootloader image present and selected on the
    unit (S-11.096), and that image not a waffle-boot build (bootloader_is_waffle). valid: the package's
    signature state is 'valid'."""
    return bool(option and valid and has_bootloader and bootloader_selected and not bootloader_waffle)


DESC_ADDR, DESC_MAGIC, DESC_VERSION, DESC_HEAD, DESC_REGIONS = 0x08030000, 0x4D494257, 1, 0x40, 24
DESC_MAX = DESC_HEAD + 8 * DESC_REGIONS + 4


def bootloader_is_waffle(stream):
    """S-11.091: the bootloader image is a waffle-boot build when its stream carries a valid image
    descriptor (S-01.023), as recovery's WB kind (S-02.106 rule 1) and the installer read it: walking the
    LDR blocks (S-05) up to FINAL, the first data block loaded at the descriptor's address decides; its
    magic, version, size (0x40 + 8 n + 4, n at most 24) and final CRC-32 must hold. FILL blocks carry no
    payload, IGNORE blocks carry one and load nothing. Anything else, a factory bootloader included, is not."""
    data, pos = bytes(stream), 0
    while pos + ldr.HEADER_SIZE <= len(data):
        h = data[pos:pos + ldr.HEADER_SIZE]
        if not ldr.header_ok(h):
            return False
        code, target, count, _ = struct.unpack('<IIII', h)
        fill = bool(code & ldr.FLAGS['FILL'])
        if not fill and not code & ldr.FLAGS['IGNORE'] and target == DESC_ADDR and count >= DESC_HEAD + 4:
            d = data[pos + ldr.HEADER_SIZE:pos + ldr.HEADER_SIZE + min(count, DESC_MAX)]
            if len(d) < min(count, DESC_MAX):
                return False
            magic, ver, size = struct.unpack_from('<IHH', d)
            n = struct.unpack_from('<I', d, 0x0C)[0]
            return (magic == DESC_MAGIC and ver == DESC_VERSION and n <= DESC_REGIONS
                    and size == DESC_HEAD + 8 * n + 4 and size <= len(d)
                    and crc32(d[:size - 4]) == struct.unpack_from('<I', d, size - 4)[0])
        if code & ldr.FLAGS['FINAL']:
            return False
        pos += ldr.HEADER_SIZE + (0 if fill else count)
    return False


# ---- S-11.096: the selection

PICK_ABSENT, PICK_OPEN, PICK_SWITCHED_OFF, PICK_OPTION_OFF, PICK_UP_TO_DATE = (
    'absent', 'open', 'switched off', 'option off', 'up to date')


def pick(step, c):
    """What the selection offers for a step under steps.tsv's conditions, with `known`: the installed
    version of a Nordic or PSoC step is known (not 0.0.0). A row for each step carried; one that would report
    Already installed, its installed version known, is up to date. The signature state (S-11.021) plays no
    part (S-11.090)."""
    if step == 0:
        return PICK_OPEN if c.get('content_dir') else PICK_ABSENT
    if step == 1:
        return PICK_OPEN if c.get('present') else PICK_ABSENT
    carried = c.get('present0') or c.get('present1') if step == 4 else c.get('present')
    if step == RESERVED or not carried:
        return PICK_ABSENT
    if step in SWITCHABLE and not switched_on(step, c):
        return PICK_SWITCHED_OFF
    if step in PROTECTED and not c.get('option'):
        return PICK_OPTION_OFF
    if _already(step, c) and c.get('known', 1):
        return PICK_UP_TO_DATE
    return PICK_OPEN


def select(pickable, answer):
    """The selection before a run: pickable, the steps that can be picked; answer(pickable) the user's answer
    on the unit: ('install', steps), 'cancel', or None when the keys cannot be read and nothing is asked.
    Returns the steps selected, or None when canceled. Unasked: every step that can be picked but the
    protected ones."""
    a = answer(frozenset(pickable)) if answer else None
    if a is None:
        return frozenset(s for s in pickable if s not in PROTECTED)
    if a == 'cancel':
        return None
    return frozenset(a[1]) & frozenset(pickable)


def os_form(stock):
    """S-11.092: the plaintext form in every run, a return to the factory bootloader included."""
    return 'plaintext'


# ---- S-11.061-S-11.065, S-11.080 across boots: a unit model

_STEP_COMPONENTS = {1: ('os',), 2: ('bootloader',), 3: ('ble',), 4: ('nordic_bl0', 'nordic_bl1'),
                    5: ('backplane',), 6: ()}


class Device:
    """What survives a reset: the metadata state and os_version (S-04), the staging area (None when it does
    not exist) with its progress record, and the yaffs2 tree. The option never survives (S-02.055).
    upgrade_access counts the boots' reads and writes of the record and the staging area (S-11.080: none)."""

    def __init__(self, tree=None, os_version=(1, 0, 0, 0)):
        self.state, self.os_version = 0, tuple(os_version)
        self.staged, self.record, self.tree = None, None, dict(tree or {})
        self.records_written = []    # every progress record written, in order
        self.meta_writes = []        # every metadata write made, in order
        self.upgrade_access = 0

    def _write(self, w, ctx):
        kind, what = w
        if kind == 'progress':
            self.record = bytes(what)
            self.records_written.append(tuple(what))
        elif kind == 'meta':
            self.meta_writes.append(what)
            if what == 'state=AB0D':
                self.state = UPGRADING
            elif what == 'state=0':
                self.state = 0
            elif what == 'os_version=package,state=0':
                self.os_version, self.state = package_version(self.staged or {}), 0
        elif kind == 'content':
            self.tree = ctx['tree']
            self.staged = ctx['staged']


def _step_results(dev, option, faults, selected=frozenset(SELECTABLE), steps_off=()):
    """Results the steps would give from the staged files; faults: {step: code} overrides; selected: the
    steps selected (S-11.096); steps_off: the switched-off steps (S-11.073). Returns (results, content
    effect) where the effect is the tree and staging after the content step (None when it does not run)."""
    staged = dev.staged or {}
    comps = components(staged)
    res, effect = {}, None
    co = content_only(staged)
    for s in range(DONE):
        if s == 0 and has_content(staged) and s not in selected:
            r = SKIPPED
        elif s == 0:
            r, tree = install_content(dev.tree, staged, co)
            moved = {p for p, b in staged.items() if p.startswith(CONTENT_DIR) and b is not None}
            left = {p: b for p, b in staged.items() if p not in moved}
            for p in moved:
                left.setdefault(p.rsplit('/', 1)[0], None)
            effect = dict(tree=tree, staged=left)
        elif not any(c in comps for c in _STEP_COMPONENTS[s]):
            r = NOT_FOUND
        elif s in SWITCHABLE and s in steps_off:
            r = SKIPPED
        elif s in PROTECTED and not option:
            r = SKIPPED
        elif s not in selected:
            r = SKIPPED
        else:
            r = OK
        res[s] = faults.get(s, r)
    return res, effect


def _execute(dev, r, effect, cut):
    """Apply a run's writes in order; the content step's effect lands just before the record that follows
    it. cut: the number of writes made before a reset (None: no reset). Returns False when cut."""
    for i, w in enumerate(r['writes']):
        if cut is not None and i >= cut:
            return False
        if effect is not None and w[0] == 'progress' and w[1][0] == 1 and r['ran'] and r['ran'][0][0] == 0:
            dev._write(('content', None), effect)
            effect = None
        dev._write(w, None)
    return True


def extract(dev, staged):
    """S-11.031: the staging area and its record are removed, then the zip's members staged."""
    dev.staged, dev.record = dict(staged), None


def install(dev, staged, option=False, faults=None, nordic_dfu_ok=True, cut=None, steps_off=STEPS_OFF,
            answer=None, signature=SIG_VALID, prompt=None):
    """A run, after its package and zip passed every check. steps_off: the switched-off steps (S-11.073),
    this version's by default; answer(pickable): the user's answer to S-11.096's selection (see select();
    None: the keys cannot be read). signature: the package's state (S-11.021), which never gates a step
    (S-11.090); an invalid one is asked prompt() first (S-15.197): 'install' goes on to
    the selection, 'cancel' (or prompt None: the keys cannot be read, so nobody can confirm) ends it.
    Returns the run dict, with `pickable` and `selected`, or None when cut. A canceled selection or prompt
    starts no run and writes nothing: outcome 'nothing to do', `canceled` true."""
    extract(dev, staged)
    comps = components(dev.staged)
    pickable = set()
    if has_content(dev.staged):
        pickable.add(0)
    for st, names in _STEP_COMPONENTS.items():
        if st == RESERVED or not any(c in comps for c in names):
            continue
        if st in SWITCHABLE and st in steps_off:
            continue
        if st in PROTECTED and not option:
            continue
        pickable.add(st)
    confirmed = signature != SIG_INVALID or (prompt is not None and prompt() == 'install')
    selected = select(pickable, answer) if confirmed else None
    if selected is None:
        return dict(outcome='nothing to do', ran=[], writes=[], canceled=True, pickable=sorted(pickable),
                    selected=[])
    res, effect = _step_results(dev, option, faults or {}, selected, tuple(steps_off))
    co = content_only(dev.staged)
    nordic = [s for s in NORDIC_STEPS if any(c in comps for c in _STEP_COMPONENTS[s])]
    r = run(res, co, nordic, nordic_dfu_ok, version_file=has_version_file(dev.staged), steps_off=tuple(steps_off),
            selected=selected)
    r.update(canceled=False, pickable=sorted(pickable), selected=sorted(selected))
    return r if _execute(dev, r, effect, cut) else None


def boot(dev, keys_held=False, slave_present=True, meta_write_ok=True):
    """One boot. Returns 'os', 'held' (keys held, state 0) or 'interrupted' (S-11.080: the state was
    0xAB0D; held mode with the notice). The record and the staging area are never touched."""
    b = interrupted_boot(dev.state, keys_held, slave_present, meta_write_ok)
    if not b['notice']:
        return b['decision']
    if meta_write_ok:
        dev._write(('meta', 'state=0'), None)
    return 'interrupted'


# ---------- S-11.021 - S-11.023: check 8 sets the state, check 12 refuses ----------
ACCEPTANCE_COLUMNS = ('payload_key_index', 'k0_readable', 'k1_readable', 'option', 'dev_mode', 'verdict')


def acceptance_rows():
    """S-11.023's table (check 12 only; every other check passes). Acceptance is independent of the signature
    state (S-11.022): no signature, signature key index or Q0 dimension; signature_state() decides only the
    prompt, the bootloader step and the return to stock. Dev mode has no effect."""
    rows = []
    for kidx, k0, k1, opt, dev in itertools.product(['00', '01', 'FE', 'FF', '02'], [1, 0], [1, 0], [0, 1],
                                                    [0, 1]):
        if kidx == '00':
            verdict = 'accept' if k0 else 'NOKEY'
        elif kidx == '01':
            verdict = 'KEY' if not opt else ('accept' if k1 else 'NOKEY')
        elif kidx == 'FE':
            verdict = 'accept'
        else:
            verdict = 'KEY'
        rows.append([kidx, k0, k1, opt, dev, verdict])
    return rows


def acceptance_table():
    """acceptance_rows() as rows of text keyed by ACCEPTANCE_COLUMNS."""
    return [dict(zip(ACCEPTANCE_COLUMNS, (str(v) for v in row))) for row in acceptance_rows()]
