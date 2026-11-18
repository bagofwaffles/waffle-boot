# SPDX-License-Identifier: GPL-2.0-or-later
"""A synthetic eMMC (boot partitions with OS images behind a 512-byte header, the user-area layout), with
models of the OS loader (S-03.040-S-03.048) and the OS writer (S-03.050-S-03.068)."""

import struct
from dataclasses import dataclass

from . import ldr
from .crc import crc32

SECTOR = 512
HEADER_FORMAT = 3
OS_MAX = 0x800000                 # largest OS image (8 MB)
WRITE_MIN = 10000                 # the writer's minimum for length + 512
BOOT_PARTITION_SIZE = 4 << 20     # B, a test choice

# Partition numbers as EXT_CSD[179] PARTITION_CONFIG selects them
USER, BOOT1, BOOT2, RPMB = 0, 1, 2, 3

# The user area (sectors)
USER_SECTORS = 0x1CF8000
ERASE_GROUP = 0x400
YAFFS_FIRST, YAFFS_LAST = 0, 0x47FFFF
UNUSED_FIRST, UNUSED_LAST = 0x480000, 0xCF7FFF
FAT_FIRST, FAT_LAST = 0xCF8000, 0x1CF7FFF
FAT_SECTORS = 0x1000000
YAFFS_CHUNK_SECTORS = 8
YAFFS_CHUNK_DATA = 0xFF0
YAFFS_TAGS_BYTES = 16
YAFFS_CHUNKS_PER_BLOCK = 128
YAFFS_BLOCKS = (YAFFS_LAST + 1) // (YAFFS_CHUNK_SECTORS * YAFFS_CHUNKS_PER_BLOCK)   # 0x1200
YAFFS_RESERVED_BLOCKS = 5
FACTORY_RESET_ERASE = (0x1000000, 0x10003FF)

USER_LAYOUT = [('yaffs2', YAFFS_FIRST, YAFFS_LAST), ('unused', UNUSED_FIRST, UNUSED_LAST),
               ('fat', FAT_FIRST, FAT_LAST)]


def fat_to_physical(s):
    """The OS's rotated FAT volume: FatFs sector s (0 <= s < 0x1000000) to an eMMC sector."""
    if not 0 <= s < FAT_SECTORS:
        raise ValueError('outside the FAT volume')
    return s + 0x1000000 if s < 0xCF8000 else s


def physical_to_fat(p):
    if not FAT_FIRST <= p <= FAT_LAST:
        raise ValueError('outside the FAT volume')
    return p % FAT_SECTORS


@dataclass
class OsHeader:
    """Sector 0 of a boot partition, little-endian: u32 format, u32 length, u32 crc (CRC-32 of the
    plaintext image), u8 active, u8 encrypted, zeros to 512 bytes. The image follows from sector 1."""
    format: int = HEADER_FORMAT
    length: int = 0
    crc: int = 0
    active: int = 1
    encrypted: int = 0

    def pack(self):
        return struct.pack('<IIIBB', self.format, self.length, self.crc, self.active & 0xFF,
                           self.encrypted & 0xFF).ljust(SECTOR, b'\0')

    @classmethod
    def unpack(cls, b):
        return cls(*struct.unpack_from('<IIIBB', bytes(b[:SECTOR])))

    @classmethod
    def for_image(cls, image, active=1, encrypted=0):
        return cls(HEADER_FORMAT, len(image), crc32(image), active, encrypted)


def synthetic_os(seed=0x05, entry=0x11A00000, size=None):
    """A synthetic OS payload (LDR application) for handoff tests: blocks into L1, L2 and DDR, a
    FILL block, FINAL. With `size`, a single-data-block stream of exactly that many bytes."""
    if size is not None:
        return ldr.synthetic_app_of_size(size, seed=seed, entry=entry)
    data, _ = ldr.synthetic_app(seed, entry, regions=[(entry, 0x800), (0x08000000, 0x3000), (0x80000000, 0x2400)],
                                fill=(0x80100000, 0x400, 0))
    return data


def n_sectors(length):
    """N: the stored region's sector count."""
    return (length + SECTOR - 1) // SECTOR


def pack_header(format=HEADER_FORMAT, length=0, crc=0, active=1, encrypted=0, reserved=0):
    """A 512-byte header; `reserved` fills bytes 0x0E-0x1FF."""
    h = struct.pack('<IIIBB', format, length, crc, active & 0xFF, encrypted & 0xFF)
    return h + bytes([reserved]) * (SECTOR - len(h))


def parse_header(b):
    """The reader's view of a header: mode 'plain' (format < 2, or format 2/3 with encrypted 0),
    'encrypted' (format 2/3 with a nonzero encrypted byte), or 'open' (format > 3)."""
    fmt, length, crc, active, enc = struct.unpack_from('<IIIBB', bytes(b))
    if fmt > 3:
        mode = 'open'
    elif fmt >= 2 and enc:
        mode = 'encrypted'
    else:
        mode = 'plain'
    return {'format': fmt, 'length': length, 'crc': crc, 'active': active != 0, 'mode': mode}


def acceptable(length, B=BOOT_PARTITION_SIZE):
    """The writer's acceptance (S-03.051): length + 512 >= 10000, length <= 8 MB, 512 x (N + 1) <= B."""
    return length + SECTOR >= WRITE_MIN and length <= OS_MAX and SECTOR * (n_sectors(length) + 1) <= B


def last_writable_sector(length, B=BOOT_PARTITION_SIZE):
    """S-03.027, S-03.054: the last sector a write of `length` bytes may change: N + 1 if it lies inside
    the partition and the erased groups, else N."""
    n = n_sectors(length)
    if n + 1 <= B // SECTOR - 1 and (n + 1) // ERASE_GROUP <= n // ERASE_GROUP:
        return n + 1
    return n


class Emmc:
    """Boot partitions as bytearrays of B bytes, the user area sparse ({sector: 512 bytes}, zero
    elsewhere). An erase sets one group of 0x400 sectors to `erased` (0x00 by default, S-03.003)."""

    def __init__(self, boot_size=BOOT_PARTITION_SIZE, erased=0x00):
        self.B = boot_size
        self.erased = erased      # what an erased byte reads as (EXT_CSD ERASED_MEM_CONT)
        self.boot = {BOOT1: bytearray(boot_size), BOOT2: bytearray(boot_size)}
        self.user = {}

    def copy(self):
        c = Emmc(self.B, self.erased)
        c.boot = {k: bytearray(v) for k, v in self.boot.items()}
        c.user = dict(self.user)
        return c

    def erase_group(self, part, g):
        a = g * ERASE_GROUP * SECTOR
        b = min(a + ERASE_GROUP * SECTOR, self.B)
        self.boot[part][a:b] = bytes([self.erased]) * (b - a)

    def write(self, part, sector, data):
        a = sector * SECTOR
        if a + len(data) > self.B:
            raise ValueError('write past the partition')
        self.boot[part][a:a + len(data)] = data

    def read(self, part, sector, count):
        return bytes(self.boot[part][sector * SECTOR:(sector + count) * SECTOR])

    def install(self, part, image, active=1, encrypted=0, header=None, format=HEADER_FORMAT, pad=0,
                reserved=0):
        """Place `image` at sector 1 of boot partition `part` behind its header, padded with `pad` to N
        sectors. `encrypted` sets the header byte only; the body stays plaintext. `header`: an OsHeader or
        512 bytes, replacing the computed one."""
        p = self.boot[part]
        n = n_sectors(len(image))
        if SECTOR * (n + 1) > len(p):
            raise ValueError('image larger than the boot partition')
        stored = bytes(image) + bytes([pad]) * (n * SECTOR - len(image))
        if header is None:
            h = pack_header(format, len(image), crc32(image), active, encrypted, reserved)
        else:
            h = header.pack() if isinstance(header, OsHeader) else bytes(header)
        p[0:SECTOR] = h
        p[SECTOR:SECTOR + len(stored)] = stored

    def header(self, part):
        return OsHeader.unpack(self.boot[part][:SECTOR])

    def write_user(self, sector, data):
        for i in range(0, len(data), SECTOR):
            self.user[sector + i // SECTOR] = bytes(data[i:i + SECTOR]).ljust(SECTOR, b'\0')

    def read_user(self, sector, count=1):
        return b''.join(self.user.get(s, bytes(SECTOR)) for s in range(sector, sector + count))

    def save(self, prefix):
        """Write <prefix>.boot1, <prefix>.boot2 and a sparse <prefix>.user of the full user-area size."""
        for part, name in ((BOOT1, 'boot1'), (BOOT2, 'boot2')):
            with open('%s.%s' % (prefix, name), 'wb') as f:
                f.write(self.boot[part])
        with open(prefix + '.user', 'wb') as f:
            for s in sorted(self.user):
                f.seek(s * SECTOR)
                f.write(self.user[s])
            f.truncate(USER_SECTORS * SECTOR)


@dataclass
class LoadResult:
    ok: bool
    partition: object = None      # the partition whose header was used
    length: int = 0
    reason: str = ''              # on failure: 'no_active', 'zero_length', 'too_long', 'past_partition',
                                  # 'format', 'encrypted', 'crc_mismatch'
    selects: tuple = ()           # partition selects in order, ending on the user area
    reads: tuple = ()             # (first sector, count) per read
    image: bytes = None           # the plaintext image on success


def load_os(emmc, max_len=OS_MAX):
    """The OS loader (S-03.040-S-03.044, S-03.048): boot partition 1 if active, else 2 if active, else
    'no_active'. Checks length, size, format and encrypted marking, reads sectors 1 to N as one range,
    then checks the CRC. No fallback to the other partition."""
    selects = [BOOT1]
    part = None
    if emmc.header(BOOT1).active:
        part = BOOT1
    else:
        selects.append(BOOT2)
        if emmc.header(BOOT2).active:
            part = BOOT2
    selects.append(USER)
    sel = tuple(selects)
    if part is None:
        return LoadResult(False, None, 0, 'no_active', sel)
    h = parse_header(emmc.boot[part][:SECTOR])
    ln = h['length']
    if ln == 0:
        return LoadResult(False, part, ln, 'zero_length', sel)
    if ln > max_len:
        return LoadResult(False, part, ln, 'too_long', sel)
    n = n_sectors(ln)
    if SECTOR * (n + 1) > len(emmc.boot[part]):
        return LoadResult(False, part, ln, 'past_partition', sel)
    if h['mode'] == 'open':
        return LoadResult(False, part, ln, 'format', sel)
    if h['mode'] == 'encrypted':
        return LoadResult(False, part, ln, 'encrypted', sel)
    reads = ((1, n),)             # one range (S-03.043)
    img = bytes(emmc.boot[part][SECTOR:SECTOR + ln])
    if crc32(img) != h['crc']:
        return LoadResult(False, part, ln, 'crc_mismatch', sel, reads)
    return LoadResult(True, part, ln, '', sel, reads, img)


def write_target(emmc):
    """The writer's target (S-03.052): boot partition 2 if partition 1 is active, else 1."""
    return BOOT2 if emmc.header(BOOT1).active else BOOT1


def os_image_acceptable(image, boot_size=BOOT_PARTITION_SIZE):
    """The writer's size rule (S-03.051) on an image: `acceptable(len(image), boot_size)`."""
    return acceptable(len(image), boot_size)


WRITE_FAILS = ('select1', 'read_header1', 'select2', 'erase', 'write_image', 'read_image',
               'write_header', 'read_header', 'write_header_active', 'read_header_active',
               'select_other', 'read_other', 'deactivate', 'select0',
               'flush_image', 'flush_header', 'flush_final')
DEACTIVATIONS = ('rewrite',)    # S-03.058: the rewrite, and no erase on the other partition
PROFILES = ('two_step', 'factory')


def write_os(emmc, image, corrupt_sector=None, corrupt_header=None, deactivate='rewrite', cut_after=None,
             fail=None, profile='two_step', tear=None):
    """The OS writer (S-03.050-S-03.068) on `emmc` (mutated). Returns (result, ops, target), result 'ok',
    'refused', 'failed' or 'cut'.

    profile 'two_step' (S-03.055): zero the target header if it reads active, and read it back; erase the
    target's groups from 1 on (group 0, the header's, is written over and never erased); write and read
    back the image; write and verify the header inactive, then active; rewrite the other partition's header
    inactive, and erase nothing there (S-03.058). A header that does not verify is
    rewritten inactive. Flushes follow the inactive header, the active header and the deactivation; a
    failed write that changed anything flushes before its select 0.
    profile 'factory': a single active header write, a failed header and the other partition undone by
    erasing group 0; the comparison case for tear tests. `deactivate` is ignored.

    The image is stored as format 3, encrypted byte 0.
    ops: ('select', n) | ('read', part, first, count) | ('erase', part, group) | ('write', part, first,
    count) | ('flush',); an op that fails carries a trailing 'error'.
    corrupt_sector: a stored-region sector (1..N) whose write lands with its first byte inverted.
    corrupt_header: 'inactive' (the first header write, active byte 0; in 'factory' the only header
    write) or 'active' (the rewrite with the active byte 1) lands with byte 4 inverted; True = 'inactive'.
    fail: one of WRITE_FAILS. A failed erase or write stores nothing, except a target header write, which
    stores the header in full.
    cut_after: power cut after that many erase/write operations; the result is then 'cut'.
    tear: (k, fn): the k-th erase/write is torn: fn(emmc, part, first_byte, old_bytes, new_bytes) stores
    the torn result, then power is cut (result 'cut')."""
    if profile not in PROFILES:
        raise ValueError(profile)
    if corrupt_header is True:
        corrupt_header = 'inactive'
    ops, muts = [], [0]

    class _Cut(Exception):
        pass

    def op(o, point=None):
        failed = point is not None and fail == point
        ops.append(o + ('error',) if failed else o)
        if o[0] in ('erase', 'write'):
            muts[0] += 1
            if cut_after is not None and muts[0] >= cut_after:
                raise _Cut()
        return failed

    def mutate(o, apply, point=None):
        """Issue erase/write `o`; `apply()` stores its effect unless the op fails (or is torn)."""
        k = muts[0] + 1
        if tear is not None and tear[0] == k:
            part = o[1]
            if o[0] == 'erase':
                a = o[2] * ERASE_GROUP * SECTOR
                b = min(a + ERASE_GROUP * SECTOR, emmc.B)
            else:
                a, b = o[2] * SECTOR, (o[2] + o[3]) * SECTOR
            before = bytes(emmc.boot[part][a:b])
            apply()
            after = bytes(emmc.boot[part][a:b])
            tear[1](emmc, part, a, before, after)
            ops.append(o)
            raise _Cut()
        failed = point is not None and fail == point
        if not failed or point in ('write_header', 'write_header_active'):
            apply()
        return op(o, point)

    def end(result):                                   # S-03.007, S-03.063
        if (profile == 'two_step' and result == 'failed' and muts[0] and
                ops[-1] != ('flush', 'error')):          # S-03.068
            op(('flush',))
        op(('select', USER), 'select0')
        return result, ops, target

    length = len(image)
    if not acceptable(length, emmc.B):
        return 'refused', ops, None
    n = n_sectors(length)
    if deactivate not in DEACTIVATIONS:
        raise ValueError(deactivate)
    stored = bytes(image) + bytes(n * SECTOR - length)
    header = pack_header(3, length, crc32(image), 1, 0)
    inactive = pack_header(3, length, crc32(image), 0, 0)
    target = None

    def header_step(active, which, wpoint, rpoint):
        """Write the target header with `active`, read it back; True when it did not verify."""
        want = header if active else inactive
        h = bytearray(want)
        if corrupt_header == which:
            h[4] ^= 0xFF
        bad = mutate(('write', target, 0, 1), lambda: emmc.write(target, 0, bytes(h)), wpoint)
        if not bad:
            bad = op(('read', target, 0, 1), rpoint) or emmc.read(target, 0, 1) != want
        if bad:                                        # S-03.057
            if profile == 'factory':
                mutate(('erase', target, 0), lambda: emmc.erase_group(target, 0))
            else:
                mutate(('write', target, 0, 1), lambda: emmc.write(target, 0, inactive))
        return bad

    def header_inactive(check=False):
        """S-03.055, S-03.056: zero the target header if it reads active; with check, read it back and
        report whether it still reads active."""
        if profile != 'two_step':
            return False
        op(('read', target, 0, 1))
        if emmc.read(target, 0, 1)[0x0C]:
            mutate(('write', target, 0, 1), lambda: emmc.write(target, 0, bytes(SECTOR)))
            if check:
                op(('read', target, 0, 1))
                return emmc.read(target, 0, 1)[0x0C] != 0
        return False

    def failed_early():
        if muts[0]:
            header_inactive()
        return end('failed')

    try:
        if op(('select', BOOT1), 'select1'):
            return end('failed')
        if op(('read', BOOT1, 0, 1), 'read_header1'):
            return end('failed')
        target = BOOT2 if parse_header(emmc.read(BOOT1, 0, 1))['active'] else BOOT1
        other = BOOT1 if target == BOOT2 else BOOT2
        if target == BOOT2 and op(('select', BOOT2), 'select2'):
            return end('failed')
        if profile == 'two_step':
            if header_inactive(check=True):
                return failed_early()
            first = 1
        else:
            first = 0
        for g in range(first, n // ERASE_GROUP + 1):
            if mutate(('erase', target, g), lambda g=g: emmc.erase_group(target, g),
                      'erase' if g == first else None):
                return failed_early()
        data = bytearray(stored)
        if corrupt_sector is not None:
            data[(corrupt_sector - 1) * SECTOR] ^= 0xFF
        if mutate(('write', target, 1, n), lambda: emmc.write(target, 1, bytes(data)), 'write_image'):
            return failed_early()
        if op(('read', target, 1, n), 'read_image'):
            return failed_early()
        if emmc.read(target, 1, n)[:length] != stored[:length]:
            return failed_early()
        if profile == 'two_step':
            if header_step(0, 'inactive', 'write_header', 'read_header'):
                return end('failed')
            if op(('flush',), 'flush_image'):
                return end('failed')
            if header_step(1, 'active', 'write_header_active', 'read_header_active'):
                return end('failed')
            if op(('flush',), 'flush_header'):
                return end('failed')
        elif header_step(1, 'inactive', 'write_header', 'read_header'):
            return end('failed')
        if op(('select', other), 'select_other'):
            return end('failed')
        if profile == 'factory':
            if mutate(('erase', other, 0), lambda: emmc.erase_group(other, 0), 'deactivate'):
                return end('failed')
            return end('ok')
        if op(('read', other, 0, 1), 'read_other'):
            return end('failed')
        h = bytearray(emmc.read(other, 0, 1))
        h[0x0C] = 0
        if mutate(('write', other, 0, 1), lambda: emmc.write(other, 0, bytes(h)), 'deactivate'):
            return end('failed')
        if op(('flush',), 'flush_final'):
            return end('failed')
        return end('ok')
    except _Cut:
        return 'cut', ops, target


def build_os_partition(os_ldr, format=HEADER_FORMAT, active=1, encrypted=0, boot_size=BOOT_PARTITION_SIZE,
                       pad=0, reserved=0):
    """One boot partition's bytes (B bytes) holding `os_ldr` behind its header. `encrypted`
    sets the header byte only (see Emmc.install)."""
    e = Emmc(boot_size)
    e.install(BOOT1, os_ldr, active, encrypted, format=format, pad=pad, reserved=reserved)
    return bytes(e.boot[BOOT1])


def build_emmc(p1=None, p2=None, boot_size=BOOT_PARTITION_SIZE, user=None):
    """An eMMC with `p1`/`p2` each None (erased: inactive), B bytes of a whole partition (from
    build_os_partition), or a dict {'image': bytes, 'active': int, 'encrypted': int, 'format': int,
    'corrupt': bool} (`encrypted`: the header byte only); `corrupt` inverts the image's first byte after the
    header is computed. user: {sector: bytes} written to the user area."""
    e = Emmc(boot_size)
    for part, spec in ((BOOT1, p1), (BOOT2, p2)):
        if spec is None:
            continue
        if isinstance(spec, (bytes, bytearray)):
            if len(spec) != boot_size:
                raise ValueError('partition bytes must be boot_size long')
            e.boot[part][:] = spec
            continue
        img = spec.get('image') or synthetic_os(seed=part)
        e.install(part, img, spec.get('active', 1), spec.get('encrypted', 0), format=spec.get('format', 3))
        if spec.get('corrupt'):
            e.boot[part][SECTOR] ^= 0xFF
    for sector, data in (user or {}).items():
        e.write_user(sector, data)
    return e
