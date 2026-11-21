# SPDX-License-Identifier: GPL-2.0-or-later
"""S-18.002 to S-18.005: the device a CPU is on, and the synthetic first stages of the vectors.

fingerprint() gives S-18.003's (n, c) from the flash's bytes, device() the row that holds it, tries()
the read's tries with a fault per try, and identify() the whole decision of a CPU: its boot mode and the
fallback sample (S-18.002), the table, the default (S-18.004), and what the hand-off record then carries
(S-18.005). A vector's first stage is a fill pattern with the
argument set at 0x0C and four bytes patched to force the CRC-32, so no vector holds a byte of a first
stage; build() makes a case's bytes again.
"""
import importlib.util
import os
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def _slot_kind():
    """S-02.106's model, loaded from its file so that nothing of S-02 joins sys.path."""
    path = os.path.join(HERE, '..', 'S-02-spi-flash', 'slot_kind.py')
    spec = importlib.util.spec_from_file_location('_s02_slot_kind', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


force = _slot_kind().force   # S-02.106's CRC-forcing patch

DEVICES = ('OP-XY', 'OP-1F')            # the record's offset 152
HOWS = ('table', 'second_cpu', 'unknown')   # and its offset 153
DEVICE_ID = {'OP-XY': 'xy', 'OP-1F': '1f'}
UNKNOWN_ID = 'unknown'                  # S-18.031
DEFAULT = 'OP-XY'                       # S-18.004
MAIN_BOOT_MODE = 1                      # S-18.002: SPI master boot

# S-18.003
ARG_OFF = 0x0C
HEAD = 16
N_MAX = 0xC000
N_SATURATED = 0xFFFFFFFF                # n when the argument plus 16 leaves 32 bits
TABLE = {
    (0x86C0, 0xF3C93780): 'OP-XY',
    (0x8600, 0x49267EB8): 'OP-1F',
}

# S-18.020
FEATURES = (
    ('second-cpu', True, False),
    ('straps', True, False),
    ('emmc-on-main', False, True),
    ('pmic-pass-through', True, False),
    ('digital-gamma', True, False),
    ('key-leds', True, False),
    ('content-packs', True, False),
    ('fat-region', True, False),
    ('factory-word', True, False),
    ('cv-calibration', True, False),
    ('mtp-two-items', True, False),
)


def has(device, feature):
    for name, xy, f1 in FEATURES:
        if name == feature:
            return xy if device == 'OP-XY' else f1
    raise KeyError(feature)


def crc32(data, crc=0):
    return zlib.crc32(data, crc) & 0xFFFFFFFF


def read(flash, off, k):
    """k bytes of the flash at off; past the bytes given it reads erased."""
    b = bytes(flash[off:off + k])
    return b + b'\xff' * (k - len(b))


def fingerprint(flash):
    """S-18.003: (n, c), c None when n is out of range."""
    head = read(flash, 0, HEAD)
    n = min(int.from_bytes(head[ARG_OFF:ARG_OFF + 4], 'little') + HEAD, N_SATURATED)
    if not HEAD < n <= N_MAX:
        return n, None
    return n, crc32(head + read(flash, HEAD, n - HEAD))


def device(n, c):
    """The device whose row holds (n, c), None for none."""
    return None if c is None else TABLE.get((n, c))


TRIES = 3                               # S-18.003: the most tries of the whole read


def try_read(flash, fault=None):
    """One try of S-18.003's read, (n, c, outcome), c None when none is computed. fault, for this try:
    None a whole read; 'head' the first 16 bytes' read fails (n 0); 'later' a later read fails (the
    computed n); 'busy_head' and 'busy_later' the same with a busy wait that ran its whole bound;
    'flip' every read returns, with one byte past the header changed."""
    if fault in ('head', 'busy_head'):
        return 0, None, 'busy' if fault == 'busy_head' else 'failed'
    head = read(flash, 0, HEAD)
    n = min(int.from_bytes(head[ARG_OFF:ARG_OFF + 4], 'little') + HEAD, N_SATURATED)
    if not HEAD < n <= N_MAX:
        return n, None, 'read'
    if fault in ('later', 'busy_later'):
        return n, None, 'busy' if fault == 'busy_later' else 'failed'
    rest = bytearray(read(flash, HEAD, n - HEAD))
    if fault == 'flip':
        rest[0] ^= 0x01
    return n, crc32(head + bytes(rest)), 'read'


def tries(flash, faults=()):
    """S-18.003's tries: faults[i] is try i's fault (try_read()), none past the list. A further try
    follows a failed try or one whose (n, c) is in no row, never a busy one; the first row found decides;
    after the last try its n and c stand. (device, how, n, c, tries made)."""
    for t in range(TRIES):
        n, c, outcome = try_read(flash, faults[t] if t < len(faults) else None)
        found = device(n, c) if outcome == 'read' else None
        if found:
            return found, 'table', n, c, t + 1
        if outcome == 'busy':
            break
    return DEFAULT, 'unknown', n, c, t + 1


def identify(boot_mode, pa2, flash):
    """A CPU's decision (S-18.002 to S-18.005) and what follows from it. A boot mode other than 1 takes
    the fallback sample, PA2 alone: 0 the second CPU, 1 a main CPU all the same, flagged in the record's
    flags bit 12 and logged."""
    fallback = boot_mode != MAIN_BOOT_MODE
    if fallback and pa2 == 0:
        dev, how, n, c = 'OP-XY', 'second_cpu', None, None
        cpu = 'second'
    else:
        n, c = fingerprint(flash)
        found = device(n, c)
        dev, how = (found, 'table') if found else (DEFAULT, 'unknown')
        cpu = 'main'
    straps = has(dev, 'straps')
    return {
        'cpu': cpu,
        'device': dev,
        'how': how,
        'record': {'device': DEVICES.index(dev), 'how': HOWS.index(how)},
        'reads_flash': cpu == 'main',
        'n': n,
        'c': None if c is None else '0x%08X' % c,
        'strap_sample': straps,
        # S-18.002: a main CPU of the OP-XY that reads PA2 = 0 stays the main CPU and logs it
        'role_disagreement_logged': cpu == 'main' and straps and pa2 == 0,
        # S-18.002's fallback sample (PA2's INEN alone) before any flash read, and its main CPU's flag
        'fallback_sample': fallback,
        'role_flag': fallback and cpu == 'main',
        'device_id': UNKNOWN_ID if how == 'unknown' else DEVICE_ID[dev],
    }


# ---- synthetic first stages ----

def fill(length, seed, argument):
    """Byte i is (i * 37 + seed) & 0xFF, the argument at 0x0C; seed None is an erased flash."""
    if seed is None:
        return bytearray(b'\xff' * length)
    data = bytearray(((i * 37 + seed) & 0xFF) for i in range(length))
    if argument is not None:
        data[ARG_OFF:ARG_OFF + 4] = argument.to_bytes(4, 'little')
    return data


def build(case):
    """A case's flash bytes: its fill, then its patches in order."""
    data = fill(case['length'], case['seed'], case['argument'])
    for p in case['patches']:
        off = p['offset']
        data[off:off + 4] = bytes.fromhex(p['bytes'])
    return bytes(data)


def _case(name, length, seed, argument, target=None):
    """target: the CRC-32 forced over [0, argument + 16), patched at its middle."""
    data = fill(length, seed, argument)
    patches = []
    if target is not None:
        n = argument + HEAD
        off = n // 2
        patches.append({'offset': off, 'bytes': force(data, off, target, n).hex()})
    n, c = fingerprint(bytes(data))
    dev = device(n, c)
    return {
        'name': name, 'length': length, 'seed': seed, 'argument': argument, 'patches': patches,
        'n': n, 'c': None if c is None else '0x%08X' % c, 'in_range': c is not None,
        'device': dev, 'how': 'table' if dev else 'unknown',
    }


def cases():
    by_dev = {d: r for r, d in TABLE.items()}
    (nx, cx), (nf, cf) = by_dev['OP-XY'], by_dev['OP-1F']
    return [
        # each row
        _case('xy_row', nx, 1, nx - HEAD, cx),
        _case('1f_row', nf, 2, nf - HEAD, cf),
        # the bytes past n count for nothing
        _case('xy_row_with_tail', nx + 0x400, 3, nx - HEAD, cx),
        # a row's CRC at another length, and its length with another CRC
        _case('xy_crc_at_1f_length', nf, 4, nf - HEAD, cx),
        _case('1f_crc_at_xy_length', nx, 5, nx - HEAD, cf),
        _case('xy_crc_one_longer', nx + 1, 6, nx + 1 - HEAD, cx),
        _case('xy_length_other_crc', nx, 7, nx - HEAD),
        _case('1f_length_other_crc', nf, 8, nf - HEAD),
        # the range's ends
        _case('n_16', 0x100, 9, 0),
        _case('n_17', 0x100, 10, 1),
        _case('n_c000', N_MAX, 11, N_MAX - HEAD),
        _case('n_c001', N_MAX + 1, 12, N_MAX + 1 - HEAD),
        # an argument whose n leaves 32 bits, and an erased flash
        _case('n_past_32_bits', 0x100, 13, 0xFFFFFFF8),
        _case('erased', 0, None, None),
    ]
