# SPDX-License-Identifier: GPL-2.0-or-later
"""S-11 executable model: the package checks, zip checks, disk search, step results, run, content
installation, the boot after an interrupted install and the return-to-factory predicate.

Python 3, standard library plus tests/oracle/conformance (images.aes, images.p224, images.crc). Every key
used with it is a conformance test key; the model holds no key of its own.
"""
import hashlib
import io
import itertools
import json
import struct
import zipfile
import zlib

from images import aes, p224
from images.crc import crc16_xmodem

PREAMBLE_MAGIC = 0xBABECAFE
HEADER_MAGIC = 0xBEEFCAFE
PRE, HDR, MAN, NAME = 0x40, 0x40, 0x300, 0x100
PRODUCT = 0x21

# S-11.020, in check order. Reason -> result code.
REASONS = ['SHORT', 'PREAMBLE', 'TYPE', 'MAGIC', 'PRODUCT', 'DEVICE', 'CRC', 'FORMAT', 'SKU',
           'LENGTH', 'HASH', 'KEY', 'NOKEY', 'NAME', 'ZIPLEN', 'ZIP', 'MEMBER', 'SPACE', 'SPACEQUERY', 'SIZE',
           'STREAM']
REASON_CODE = {r: 3 for r in REASONS}
REASON_CODE.update(SPACE=1, SPACEQUERY=4)

# S-11.058
OK, FAILURE, NOT_FOUND, BAD_FILE, INTERNAL, CRC_ERROR, ALREADY, DOWNGRADE, SKIPPED = range(9)
RESULT_NAMES = ['OK', 'Failure', 'Not found', 'Bad file', 'Internal error', 'CRC error', 'Already installed',
                'Downgrade rejected', 'Skipped']
CONTINUE = {OK, NOT_FOUND, ALREADY, DOWNGRADE, SKIPPED}
STEPS = ['content', 'os', 'bootloader', 'ble', 'ble bootloader', 'keyboard', 'spiboot']
PROTECTED = {2, 6}
SWITCHABLE = (3, 4, 5)    # S-11.073: the steps that can be switched off
STEPS_OFF = (3, 4, 5)     # S-11.073: the steps this version has switched off
NORDIC_STEPS = (3, 4)
DONE = 7
# S-11.096: the protected components, in the order they are asked for, with their steps
CONFIRM_ORDER = (('bootloader', 2), ('spiboot', 6))
# Why a step whose component is present reports Skipped (S-11.058); the host's words (S-10c.106)
SKIP_OPTION_OFF, SKIP_NOT_CONFIRMED, SKIP_SWITCHED_OFF = 'option off', 'not confirmed', 'switched off'
SKIP_ALREADY_WRITTEN = 'already written'    # S-11.068 item 5
UPGRADING = 0xAB0D   # the metadata state of an install in progress (S-04.040)

# S-11.034
COMPONENTS = {
    'os': ('firmware/blackfin/bin/os.ldr', 'firmware/blackfin/bin/os.version', 0x800000),
    'bootloader': ('firmware/blackfin/bin/bootloader.ldr', 'firmware/blackfin/bin/bootloader.version', 0x78000),
    'spiboot': ('firmware/blackfin/bin/spiboot.ldr', 'firmware/blackfin/bin/spiboot.version', 0xB000),
    'ble': ('firmware/ble/bin/ble.bin', 'firmware/ble/bin/ble.version', 0x70000),
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
    """What the checks need of the unit: OTP Q0 (point or None), K0, K1 (32 bytes or None), the option,
    dev mode."""

    def __init__(self, q0=None, k0=None, k1=None, option=False, dev=False):
        self.q0, self.k0, self.k1, self.option, self.dev = q0, k0, k1, option, dev


# ---- S-11.007, S-11.008, S-11.036, S-11.037

def product_ok(w):
    return (w >> 14) & 0x3FF == PRODUCT and (w >> 10) & 0xF == 0


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


# ---- S-11.001-S-11.023: checks 1-15

def _ecdsa_ok(pub, man):
    digest = hashlib.sha256(man[:0x10] + bytes(0x40) + man[0x50:MAN]).digest()
    r = int.from_bytes(man[0x14:0x30], 'big')
    s = int.from_bytes(man[0x34:0x50], 'big')
    return pub is not None and p224.verify(pub, digest, r, s)


def check_package(data, form, unit, file_name=None, link=None):
    """Checks 1-15. form: 'file' (RAM disk) or 'body' (host link). Returns a dict: accepted, reason,
    te_signed, forced, version, zip, name."""
    data = bytes(data)
    out = dict(accepted=False, reason=None, te_signed=False, forced=False, version=None, zip=None, name=None)

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
    if not product_ok(hprod):
        return refuse('PRODUCT')
    if dev not in (0x01, 0x06):
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
    if man[0x84:0x88] != b'TESG' or man[0x88] != 1:
        return refuse('FORMAT')
    te_signed = man[0x89] == 0x00 and _ecdsa_ok(unit.q0, man)
    out['te_signed'] = te_signed
    if not product_ok(struct.unpack_from('<I', man, 0x8C)[0]):
        return refuse('SKU')
    payload = data[m + MAN:]
    if struct.unpack_from('<I', man, 0x98)[0] != len(payload):
        return refuse('LENGTH')
    if hashlib.sha256(payload).digest() != man[0x9C:0xBC]:
        return refuse('HASH')
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
    plain = payload[:NAME + padded] if key is None else aes.cbc_decrypt(key, iv, payload[:NAME + padded])
    z = plain[NAME:NAME + zlen]
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
    """True when the bootloader or first-stage image meets S-02.091, or the OS image S-05.024 (other
    components: True)."""
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
    elif name == 'spiboot':
        res = cs.check('first_stage', data)
    else:
        return True
    return all(ok for _, ok, _ in res)


def check_streams(staged):
    """Check 18 (S-11.035): 'STREAM' when a present bootloader or first-stage image is not a bootable stream
    of its kind, or a present OS image fails S-05.024, whether or not the run would install it; else None."""
    for name in ('os', 'bootloader', 'spiboot'):
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


# ---- S-11.066-S-11.072: step results

def switched_on(step, c):
    """S-11.073: c's `switched_on`, else this version's state."""
    return bool(c.get('switched_on', step not in STEPS_OFF))


def skip_reason(step, c):
    """S-11.058, S-11.073, S-11.090, S-11.096: why the step reports Skipped, or None when it does not."""
    if step in PROTECTED:
        if not c.get('present'):
            return None
        if not c.get('option'):
            return SKIP_OPTION_OFF
        if not c.get('confirmed'):
            return SKIP_NOT_CONFIRMED
        # S-11.068 item 5: after the image check, a start refused only for `verified`, the selector being
        # the one this power-on's own bootloader step wrote
        if (step == 2 and _staged_result(c) is None and c.get('start') == 'refused_verified'
                and c.get('written_here')):
            return SKIP_ALREADY_WRITTEN
        return None
    if step in SWITCHABLE:
        carried = c.get('present0') or c.get('present1') if step == 4 else c.get('present')
        return SKIP_SWITCHED_OFF if carried and not switched_on(step, c) else None
    return None


def step_result(step, c):
    """c: a dict of conditions (see steps.tsv). Returns a result code. `confirmed` (S-11.096) and
    `switched_on` (S-11.073) default to what nobody answered and to this version's state."""
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
    if step in (2, 6):
        if not c.get('present'):
            return NOT_FOUND
        # S-11.068: with the option on there is no version test; rel, forced and dev change nothing.
        if staged is not None:
            return staged
        if step == 2 and c.get('start', 'ok') != 'ok':
            return FAILURE                          # S-02.090 refused the start (item 5 aside)
        return OK if c.get('write_ok', True) else FAILURE
    if step == 3:
        if not c.get('present'):
            return NOT_FOUND
        if c.get('rel') == 'equal' and not forced:
            return ALREADY
        if not c.get('in_dfu', True):
            return FAILURE
        if staged is not None:
            return staged
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
        return OK if c.get('write_ok', True) else FAILURE
    if step == 5:
        if not c.get('present'):
            return NOT_FOUND
        if c.get('rel') == 'equal' and not forced:
            return ALREADY
        if staged is not None:
            return staged
        return OK if c.get('write_ok', True) else FAILURE
    raise ValueError(step)


# ---- S-11.057-S-11.065: the run

def dfu_entry(nordic_steps, steps_off=STEPS_OFF):
    """S-11.061 item 2: the DFU entry is asked when the package carries a Nordic image (nordic_steps: the
    Nordic steps, 3 and 4, whose images it carries; True for both) whose step is switched on."""
    if nordic_steps is True:
        nordic_steps = NORDIC_STEPS
    return any(s not in steps_off for s in (nordic_steps or ()))


def run(results, content_only=False, nordic_images=False, nordic_dfu_ok=True, version_file=True,
        steps_off=()):
    """results: {step: code} for the steps that would run. Every run starts at step 0 and its records
    carry count 0 (S-11.061, S-11.062). Returns a dict: outcome, ran (list of (step, code)), writes (ordered
    list of ('meta', what) / ('progress', (step, 0)) / ('nordic', what)).
    nordic_images: the Nordic steps whose images the package carries (True: both); with steps_off, the
    switched-off steps (S-11.073), they decide the DFU entry (dfu_entry).
    version_file: the staging area holds firmware/firmware.version (S-11.038); without it a completed run
    that is not content-only sets the state to 0 only. A failed run that is not content-only sets the
    state to 0 (S-11.064, S-04.045 M4)."""
    w = []
    if not content_only:
        w.append(('meta', 'state=AB0D'))
    if dfu_entry(nordic_images, steps_off):
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
            w.append(('meta', 'os_version=package,state=0' if version_file else 'state=0'))
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

def install_content(tree, staged, content_only_install):
    """tree: {path relative to /yaffs2: bytes}. staged: the staging dict. Returns (result, new tree)."""
    t = dict(tree)
    files = {p[len(CONTENT_DIR):]: b for p, b in staged.items() if p.startswith(CONTENT_DIR) and b is not None}
    if not has_content(staged):
        return NOT_FOUND, t
    partial = is_partial(staged)
    if not partial:
        t = {p: b for p, b in t.items() if not p.startswith('content/') or p.startswith('content/dlc/')}
    elif content_only_install:
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

def return_to_stock(option, te_signed, has_bootloader, bootloader_confirmed=True):
    """S-11.091: the bootloader image present and confirmed on the unit (S-11.096)."""
    return bool(option and te_signed and has_bootloader and bootloader_confirmed)


# ---- S-11.096

def confirm(components_carried, option, answer):
    """The confirmations before a run: components_carried, the package's components; answer(component) the
    user's answer on the unit. Returns (asked, confirmed): the components asked for, in order, and the
    protected steps confirmed. With the option off nothing is asked."""
    asked, confirmed = [], set()
    if not option:
        return asked, confirmed
    for comp, step in CONFIRM_ORDER:
        if comp in components_carried:
            asked.append(comp)
            if answer(comp):
                confirmed.add(step)
    return asked, confirmed


def os_form(stock):
    """S-11.092: the plaintext form in every run, a return to the factory bootloader included."""
    return 'plaintext'


# ---- S-11.061-S-11.065, S-11.080 across boots: a unit model

_STEP_COMPONENTS = {1: ('os',), 2: ('bootloader',), 3: ('ble',), 4: ('nordic_bl0', 'nordic_bl1'),
                    5: ('backplane',), 6: ('spiboot',)}


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


def _step_results(dev, option, faults, confirmed=frozenset((2, 6)), steps_off=()):
    """Results the steps would give from the staged files; faults: {step: code} overrides; confirmed: the
    protected steps confirmed (S-11.096); steps_off: the switched-off steps (S-11.073). Returns
    (results, content effect) where the effect is the tree and staging after the content step."""
    staged = dev.staged or {}
    comps = components(staged)
    res, effect = {}, None
    co = content_only(staged)
    for s in range(DONE):
        if s == 0:
            r, tree = install_content(dev.tree, staged, co)
            moved = {p for p, b in staged.items() if p.startswith(CONTENT_DIR) and b is not None}
            left = {p: b for p, b in staged.items() if p not in moved}
            for p in moved:
                left.setdefault(p.rsplit('/', 1)[0], None)
            effect = dict(tree=tree, staged=left)
        elif not any(c in comps for c in _STEP_COMPONENTS[s]):
            r = NOT_FOUND
        elif s in PROTECTED and (not option or s not in confirmed):
            r = SKIPPED
        elif s in SWITCHABLE and s in steps_off:
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
            answer=None):
    """A run, after its package and zip passed every check. steps_off: the switched-off steps (S-11.073),
    this version's by default; answer(component): the user's answer to S-11.096's confirmation (None: no
    component is confirmed). Returns the run dict, with `asked`, or None when cut."""
    extract(dev, staged)
    comps = components(dev.staged)
    asked, confirmed = confirm(comps, option, answer or (lambda c: False))
    res, effect = _step_results(dev, option, faults or {}, confirmed, tuple(steps_off))
    co = content_only(dev.staged)
    nordic = [s for s in NORDIC_STEPS if any(c in comps for c in _STEP_COMPONENTS[s])]
    r = run(res, co, nordic, nordic_dfu_ok, version_file=has_version_file(dev.staged), steps_off=tuple(steps_off))
    r['asked'] = asked
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


# ---------- S-11.021 - S-11.023: checks 8 and 12 ----------
ACCEPTANCE_COLUMNS = ('sig_key_index', 'sig_valid', 'q0_readable', 'payload_key_index', 'k0_readable',
                      'k1_readable', 'option', 'dev_mode', 'te_signed', 'verdict')


def acceptance_rows():
    """S-11.021-S-11.023 tables (checks 8 and 12 only; every other check passes)."""
    rows = []
    for sig, valid, q0, kidx, k0, k1, opt, dev in itertools.product(
            ['00', '01', 'FF', '7F'], [1, 0], [1, 0], ['00', '01', 'FE', 'FF', '02'], [1, 0], [1, 0], [0, 1], [0, 1]):
        te = sig == '00' and valid and q0
        if kidx == '00':
            verdict = 'accept' if k0 else 'NOKEY'
        elif kidx == '01':
            verdict = 'KEY' if not opt else ('accept' if k1 else 'NOKEY')
        elif kidx == 'FE':
            verdict = 'accept'
        else:
            verdict = 'KEY'
        rows.append([sig, valid, q0, kidx, k0, k1, opt, dev, int(bool(te)), verdict])
    return rows


def acceptance_table():
    """acceptance_rows() as rows of text keyed by ACCEPTANCE_COLUMNS."""
    return [dict(zip(ACCEPTANCE_COLUMNS, (str(v) for v in row))) for row in acceptance_rows()]
