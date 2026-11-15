# SPDX-License-Identifier: GPL-2.0-or-later
"""The 1 MiB SPI-flash image: layout, flash models, a builder for any selector/record/slot state, and
models of the first-stage boot decision, the slot upgrade writer and the metadata mirror.

Layout (all else reads 0xFF): 0x00000 first stage, 0x0B000 metadata mirror, 0x0C000 metadata record,
0x0D000 selector, 0x0E000 and 0x0F000 slot records, 0x10000 slot 0, 0x88000 slot 1 to the end.
"""
import random

from dataclasses import dataclass, field

from . import ldr
from .crc import crc32
from .records import (SECTOR, METADATA_OFF, METADATA_SIZE, METADATA_MAGIC, SELECTOR_OFF, SLOT_RECORD_OFF, SLOT_OFF, SLOT_MAX, SLOT_TAG,
                      MetadataRecord, Selector, SlotRecord, slot_record_check)

FLASH_SIZE = 0x100000
FIRST_STAGE_MAX = 0xC000         # the first-stage area (S-02.002)
MIRROR_OFF = 0xB000              # S-02.063: the metadata mirror sector
MIRROR_LEN = 0x68                # the 0x34-byte record, then its bitwise complement
FIRST_STAGE_LIMIT = MIRROR_OFF   # S-02.070
PAGE = 0x100
BADLEN = SLOT_MAX + 1            # S-02.016 badlen
HUGELEN = 0x80000000             # S-02.016 hugelen; S-02.026
BOOT_BASE = 0x40000000          # memory-mapped flash: a slot boots at BOOT_BASE + its offset
BADCRC_OFFSET = 0x200           # where `slot_state='badcrc'` inverts one slot byte

AREAS = [('first_stage', 0x0, FIRST_STAGE_MAX), ('metadata', METADATA_OFF, SECTOR),
         ('selector', SELECTOR_OFF, SECTOR), ('slot0_record', SLOT_RECORD_OFF[0], SECTOR),
         ('slot1_record', SLOT_RECORD_OFF[1], SECTOR), ('slot0', SLOT_OFF[0], SLOT_MAX),
         ('slot1', SLOT_OFF[1], SLOT_MAX)]
assert sum(a[2] for a in AREAS) == FLASH_SIZE      # S-02.002

SLOT_STATES = ('good', 'badcrc', 'badtag', 'erased', 'badlen', 'hugelen', 'empty')
PROFILES = ('factory', 'conforming')


class Flash:
    """A NOR flash image: 4 KB erases to 0xFF, programs clear bits; an operation log."""

    def __init__(self, data=None):
        self.data = bytearray(b'\xff' * FLASH_SIZE if data is None else data)
        if len(self.data) != FLASH_SIZE:
            raise ValueError('flash image must be 1 MiB')
        self.ops = []

    def erase(self, addr):
        if addr % SECTOR:
            raise ValueError('erase address not sector aligned')
        self.data[addr:addr + SECTOR] = b'\xff' * SECTOR
        self.ops.append(('erase', addr))

    def program(self, addr, b):
        for i, v in enumerate(b):
            self.data[addr + i] &= v
        self.ops.append(('program', addr, len(b)))

    def read(self, addr, n):
        return bytes(self.data[addr:addr + n])

    def bytes(self):
        return bytes(self.data)


class PowerCut(Exception):
    """Raised by FlashModel when power is cut; the flash keeps what the model left."""


class FlashModel(Flash):
    """Flash plus

    - a page check: a program must not cross a 256-byte page;
    - a log of every operation, reads included: `op_log` = [(seq, op, offset, length)], seq from 1;
    - power-cut injection: `cut_after(n)` completes operations 1..n and raises PowerCut instead of
      starting operation n+1 (reads count); `cut_inside(n, seed)` tears operation n (an erase leaves each
      bit of the sector old or 1, a program leaves each bit it would clear old or 0; seeded) and raises
      PowerCut;
    - fault injection: `fail_op(n)` makes operation n do nothing and return False;
      `corrupt_program(n, offset=0, xor=0x01)` makes program n store one byte XORed.
    Operations return True when they did what was asked."""

    def __init__(self, data=None):
        super().__init__(data)
        self.op_log = []
        self._cut = None            # ('after', n) or ('inside', n, seed)
        self._fail = set()
        self._corrupt = {}

    def cut_after(self, n):
        self._cut = ('after', n)
        return self

    def cut_inside(self, n, seed=0, mask=None):
        """Tear operation n. The bits that change are chosen by `random.Random(seed).getrandbits(8)` per
        byte, or by `mask(i)` for byte i of the operation when given (an erase raises the mask's bits, a
        program clears the mask's bits among those it would clear)."""
        self._cut = ('inside', n, seed, mask)
        return self

    def _tear_bits(self):
        mask = self._cut[3] if len(self._cut) > 3 else None
        if mask is not None:
            return mask
        rnd = random.Random(self._cut[2])
        return lambda i: rnd.getrandbits(8)

    def fail_op(self, n):
        self._fail.add(n)
        return self

    def corrupt_program(self, n, offset=0, xor=0x01):
        self._corrupt[n] = (offset, xor)
        return self

    def _begin(self, op, off, ln):
        seq = len(self.op_log) + 1
        if self._cut and self._cut[0] == 'after' and seq > self._cut[1]:
            raise PowerCut('cut before operation %d (%s 0x%x)' % (seq, op, off))
        self.op_log.append((seq, op, off, ln))
        return seq

    def _torn(self, seq):
        return self._cut is not None and self._cut[0] == 'inside' and self._cut[1] == seq

    def erase(self, addr):
        if addr % SECTOR:
            raise ValueError('erase address not sector aligned')
        seq = self._begin('erase', addr, SECTOR)
        if seq in self._fail:
            return False
        if self._torn(seq):
            bits = self._tear_bits()
            for i in range(SECTOR):
                self.data[addr + i] |= bits(i) & 0xFF        # each bit old or 1
            self.ops.append(('erase', addr))
            raise PowerCut('cut inside operation %d (erase 0x%x)' % (seq, addr))
        super().erase(addr)
        return True

    def program(self, addr, b):
        b = bytes(b)
        if b and addr // PAGE != (addr + len(b) - 1) // PAGE:
            raise ValueError('program at 0x%x of %d bytes crosses a page' % (addr, len(b)))
        seq = self._begin('program', addr, len(b))
        if seq in self._fail:
            return False
        if seq in self._corrupt:
            o, x = self._corrupt[seq]
            b = bytearray(b)
            b[o] ^= x
            b = bytes(b)
        if self._torn(seq):
            bits = self._tear_bits()
            for i, v in enumerate(b):
                clear = self.data[addr + i] & ~v & 0xFF      # the bits this program would clear
                self.data[addr + i] &= ~(clear & bits(i)) & 0xFF
            self.ops.append(('program', addr, len(b)))
            raise PowerCut('cut inside operation %d (program 0x%x)' % (seq, addr))
        super().program(addr, b)
        return True

    def read(self, addr, n):
        self._begin('read', addr, n)
        return super().read(addr, n)


def slot_image(slot, seed=None, size=None):
    """A synthetic slot stream (an LDR application), seeded per slot."""
    seed = (0x5107 + slot) if seed is None else seed
    size = (0x6000 + 0x1234 * slot) if size is None else size
    return ldr.synthetic_app_of_size(size, seed=seed, entry=0x11A00000)


def _state_record(state, img, base_rec, slot_bytes):
    """The record for slot state `state` (S-02.016), or None for an erased record."""
    if state == 'erased':
        return None
    if state == 'badtag':
        return SlotRecord(base_rec.crc, base_rec.length, SLOT_TAG + 1)
    if state == 'badlen':                   # tag matches, length 0x78001, CRC differs from the bytes read
        return SlotRecord(crc32(slot_bytes[:BADLEN]) ^ 1, BADLEN, SLOT_TAG)
    if state == 'hugelen':
        return SlotRecord(0x12345678, HUGELEN, SLOT_TAG)
    if state == 'empty':
        return SlotRecord(0, 0, SLOT_TAG)
    return base_rec                          # good, badcrc


def build_flash(selector=(0, 0), slots=('good', 'good'), images=(None, None), metadata=None,
                first_stage=None, records=(None, None), mirror=None):
    """Build a 1 MiB image.

    selector: (index, verified), 2 bytes, or None for an erased selector (0xFF 0xFF).
    slots: per slot one of SLOT_STATES (S-02.016):
        good     the image and its correct record
        badcrc   the correct record, with the slot byte at +0x200 inverted
        badtag   the correct record with tag 0xB005
        erased   the image, the 12 record bytes 0xFF
        badlen   the image; record {crc != CRC of the 0x78001 bytes read, 0x78001, 0xB004}
        hugelen  the image; record {0x12345678, 0x80000000, 0xB004}
        empty    the image; record {0, 0, 0xB004}
    images: per slot bytes or None for `slot_image(slot)`.
    records: per slot an explicit SlotRecord overriding the state's record.
    metadata: a MetadataRecord, 0x34 bytes, or None (left erased).
    mirror: None (the mirror sector left erased), True (the mirror of `metadata`), or a
        MetadataRecord / 0x34 bytes to mirror. A first stage over 0xB000 bytes cannot have a mirror.
    first_stage: bytes placed at 0 (a synthetic stream by default).
    Everything outside the defined content reads 0xFF."""
    fl = Flash()
    fs = first_stage if first_stage is not None else ldr.synthetic_app_of_size(0x800, seed=0xF5, entry=0x11A00000)
    if len(fs) > FIRST_STAGE_MAX:
        raise ValueError('first stage over 0xC000 bytes')
    fl.data[0:len(fs)] = fs
    if metadata is not None:
        mb = metadata if isinstance(metadata, (bytes, bytearray)) else metadata.pack()
        fl.data[METADATA_OFF:METADATA_OFF + len(mb)] = mb
    if mirror is not None:
        if len(fs) > FIRST_STAGE_LIMIT:
            raise ValueError('a first stage over 0xB000 bytes leaves no mirror sector')
        src = metadata if mirror is True else mirror
        if src is None:
            raise ValueError('mirror=True needs metadata')
        mb = bytes(src if isinstance(src, (bytes, bytearray)) else src.pack())[:METADATA_SIZE]
        fl.data[MIRROR_OFF:MIRROR_OFF + MIRROR_LEN] = mirror_encode(mb)
    if selector is not None:
        fl.data[SELECTOR_OFF:SELECTOR_OFF + 2] = bytes([selector[0] & 0xFF, selector[1] & 0xFF])
    for s in (0, 1):
        state = slots[s]
        if state not in SLOT_STATES:
            raise ValueError('unknown slot state %r' % state)
        img = images[s] if images[s] is not None else slot_image(s)
        if len(img) > SLOT_MAX:
            raise ValueError('slot image over 0x78000 bytes')
        base = SLOT_OFF[s]
        fl.data[base:base + len(img)] = img
        if state == 'badcrc':
            if len(img) <= BADCRC_OFFSET:
                raise ValueError('image too short for badcrc')
            fl.data[base + BADCRC_OFFSET] ^= 0xFF
        rec = _state_record(state, img, records[s] or SlotRecord.for_image(img), bytes(fl.data[base:]))
        if records[s] is not None and state in ('good', 'badcrc'):
            rec = records[s]
        if rec is not None:
            fl.data[SLOT_RECORD_OFF[s]:SLOT_RECORD_OFF[s] + 12] = rec.pack()
    return fl.bytes()


def build_spi_flash(selector=b'\x00\x00', slot0='good', slot1='good', *, image0=None, image1=None, metadata=None,
                    first_stage=None, mirror=None):
    """A 1 MiB image (also S-01's master flash).
    selector: 2 bytes, an (index, verified) pair, or None (erased). slot0/slot1: an S-02.016 state name,
    or bytes (taken as the image of a `good` slot). image0/image1: the image a named state uses.
    metadata: 0x34 bytes or a MetadataRecord. first_stage: bytes at offset 0. mirror: as build_flash."""
    imgs, states = [image0, image1], []
    for k, v in enumerate((slot0, slot1)):
        if isinstance(v, (bytes, bytearray)):
            imgs[k], v = bytes(v), 'good'
        states.append(v)
    sel = None if selector is None else (selector[0], selector[1])
    return build_flash(sel, tuple(states), tuple(imgs), metadata, first_stage, mirror=mirror)


def parse_flash(data):
    """The records of an image: selector, both slot records, the metadata record (raw)."""
    data = bytes(data)
    return {'selector': Selector.unpack(data[SELECTOR_OFF:SELECTOR_OFF + 2]),
            'records': [SlotRecord.unpack(data[o:o + 12]) for o in SLOT_RECORD_OFF],
            'metadata': MetadataRecord.unpack(data[METADATA_OFF:METADATA_OFF + 0x34])}


def _record(data, s):
    return SlotRecord.unpack(data[SLOT_RECORD_OFF[s]:SLOT_RECORD_OFF[s] + 12])


def slot_status(data, s):
    """S-02.021: slot s by tag and the CRC of `length` bytes, no length bound: 'ok', 'bad_tag' or
    'bad_crc'. See also `slot_good`."""
    data = bytes(data)
    rec = _record(data, s)
    if rec.tag != SLOT_TAG:
        return 'bad_tag'
    return slot_record_check(rec, data[SLOT_OFF[s]:SLOT_OFF[s] + min(rec.length, FLASH_SIZE)])


def slot_good(data, s):
    """S-02.016, S-02.031: tag matches, 1 <= length <= 0x78000, and the CRC of the slot's first `length`
    bytes equals the record's crc32."""
    data = bytes(data)
    rec = _record(data, s)
    if rec.tag != SLOT_TAG or not 1 <= rec.length <= SLOT_MAX:
        return False
    return crc32(data[SLOT_OFF[s]:SLOT_OFF[s] + rec.length]) == rec.crc


def slot_state(data, s):
    """Classify slot s by SLOT_STATES: 'erased' (12 record bytes 0xFF), 'badtag', 'empty' (length 0),
    'hugelen' (length >= 0x80000000), 'badlen' (0x78000 < length < 0x80000000), 'good', else 'badcrc'."""
    data = bytes(data)
    raw = data[SLOT_RECORD_OFF[s]:SLOT_RECORD_OFF[s] + 12]
    if raw == b'\xff' * 12:
        return 'erased'
    rec = SlotRecord.unpack(raw)
    if rec.tag != SLOT_TAG:
        return 'badtag'
    if rec.length == 0:
        return 'empty'
    if rec.length >= HUGELEN:
        return 'hugelen'
    if rec.length > SLOT_MAX:
        return 'badlen'
    return 'good' if slot_good(data, s) else 'badcrc'


def _factory_check(data, s):
    """S-02.021, S-02.026: 'pass', 'fail' or 'unspecified'."""
    rec = _record(data, s)
    if rec.tag != SLOT_TAG:
        return 'fail'
    if rec.length >= HUGELEN:
        return 'unspecified'
    return 'pass' if crc32(data[SLOT_OFF[s]:SLOT_OFF[s] + rec.length]) == rec.crc else 'fail'


def expected_boot_slot(data, profile='factory'):
    """What one reset of a first stage of `profile` ('factory' or 'conforming') does on `data`.
    Returns {'boot': 0 | 1 | 'open' | 'unspecified', 'selector_after': [acceptable 2-byte values]
    (or ['open'] / ['unspecified']), 'writes': 0 | 1 | 'open' | 'unspecified', 'checked': [slots],
    'result': 'pass' | 'fail' | 'unspecified' | '-'}."""
    if profile not in PROFILES:
        raise ValueError(profile)
    data = bytes(data)
    index, verified = data[SELECTOR_OFF], data[SELECTOR_OFF + 1]
    c = 0 if index == 0 else 1                                   # S-02.011
    if verified == 1:                                            # S-02.020, S-02.030
        return dict(boot=c, selector_after=[bytes([index, verified])], writes=0, checked=[], result='-')
    if profile == 'factory':
        r = _factory_check(data, c)
        if r == 'unspecified':
            return dict(boot='unspecified', selector_after=['unspecified'], writes='unspecified', checked=[c],
                        result=r)
        new = index if r == 'pass' else (1 if index == 0 else 0)        # S-02.022
        return dict(boot=c, selector_after=[bytes([new, 1])], writes=1, checked=[c], result=r)  # S-02.023
    if slot_good(data, c):                                       # S-02.031, S-02.032
        after = [b'\x00\x01'] if c == 0 else sorted({bytes([index, 1]), b'\x01\x01'})
        return dict(boot=c, selector_after=after, writes=1, checked=[c], result='pass')
    o = 1 - c
    if slot_good(data, o):                                       # S-02.033
        return dict(boot=o, selector_after=[bytes([o, 1])], writes=1, checked=[c, o], result='fail')
    return dict(boot='open', selector_after=['open'], writes='open', checked=[c, o], result='fail')  # S-02.034


@dataclass
class Outcome:
    """What one reset of a conforming first stage does."""
    index: int                    # selector bytes before
    verified: int
    slot: int                     # the slot booted
    boot_address: int             # BOOT_BASE + the booted slot's offset
    record_read: object = None    # None, or the slot whose record was read
    check: object = None          # None (trusted), 'ok', 'bad_tag' or 'bad_crc'
    selector_after: bytes = b''
    writes: list = field(default_factory=list)
    log: list = field(default_factory=list)   # 'verify', 'crc_match', 'crc_mismatch', 'bad_header', 'switch', ('boot', index, address)
    flash_after: bytes = b''
    undefined: object = None      # set for a record length >= 0x80000000 (S-02.026)


def first_stage_boot(data):
    """One first-stage boot on `data` (S-02.011, S-02.020-S-02.023): slot 0 for index 0, else 1; with
    verified == 0, check the slot's record and write {index, 1} on success or {other index, 1} on
    failure. The selected slot boots either way. The boot is logged as ('boot', index, address)."""
    fl = Flash(data)
    sel = Selector.unpack(fl.read(SELECTOR_OFF, 2))
    slot = sel.slot
    addr = BOOT_BASE + SLOT_OFF[slot]
    out = Outcome(sel.index, sel.verified, slot, addr)
    if sel.trusted:
        out.log.append(('boot', sel.index, addr))
    else:
        out.log.append('verify')
        out.record_read = slot
        rec = SlotRecord.unpack(fl.read(SLOT_RECORD_OFF[slot], 12))
        if rec.tag != SLOT_TAG:
            out.check = 'bad_tag'
            out.log.append('bad_header')
        else:
            if rec.length >= HUGELEN:     # S-02.026
                out.undefined = 'record length 0x%x >= 0x80000000 (S-02.026)' % rec.length
            ok = slot_record_check(rec, fl.read(SLOT_OFF[slot], rec.length))
            out.check = 'ok' if ok == 'ok' else 'bad_crc'
            out.log.append('crc_match' if ok == 'ok' else 'crc_mismatch')
        new_index = sel.index if out.check == 'ok' else (1 if sel.index == 0 else 0)
        if out.check != 'ok':
            out.log.append('switch')
        fl.erase(SELECTOR_OFF)
        fl.program(SELECTOR_OFF, bytes([new_index, 1]))
        out.writes = list(fl.ops)
        out.log.append(('boot', new_index, addr))
    out.selector_after = fl.read(SELECTOR_OFF, 2)
    out.flash_after = fl.bytes()
    return out


def boot_chain(data, resets=2):
    """`resets` consecutive first-stage boots, each on the flash the previous one left."""
    outs = []
    for _ in range(resets):
        o = first_stage_boot(data)
        outs.append(o)
        data = o.flash_after
    return outs


def stock_upgrade_write(data, image):
    """A slot upgrade writer for selector tests: index byte 1 writes slot 0 with selector {0, 0}, any
    other writes slot 1 with selector {1, 0}. Order: the slot (erase and program per 4 KB), the record,
    the selector. Returns a Flash (with its op log)."""
    if len(image) > SLOT_MAX:
        raise ValueError('image over 0x78000 bytes')
    fl = Flash(data)
    s = 0 if fl.data[SELECTOR_OFF] == 1 else 1
    base = SLOT_OFF[s]
    for off in range(0, len(image), SECTOR):
        fl.erase(base + off)
        fl.program(base + off, image[off:off + SECTOR])
    fl.erase(SLOT_RECORD_OFF[s])
    fl.program(SLOT_RECORD_OFF[s], SlotRecord.for_image(image).pack())
    fl.erase(SELECTOR_OFF)
    fl.program(SELECTOR_OFF, bytes([s, 0]))
    return fl


def areas_outside_layout(data):
    """Names of the metadata, selector and record sectors holding non-0xFF bytes past their content."""
    data = bytes(data)
    bad = []
    for name, off, used in (('metadata', METADATA_OFF, 0x34), ('selector', SELECTOR_OFF, 2),
                            ('slot0_record', SLOT_RECORD_OFF[0], 12), ('slot1_record', SLOT_RECORD_OFF[1], 12)):
        end = off + SECTOR
        tail = data[off + used:end]
        if tail.count(0xFF) != len(tail):
            bad.append(name)
    return bad


# ---------------------------------------------------------------- the metadata mirror (S-02.060-.065)

METADATA_PROFILES = ('factory', 'conforming')


def mirror_encode(record):
    """S-02.063: the 0x34-byte record followed by its bitwise complement."""
    r = bytes(record.pack() if hasattr(record, 'pack') else record)
    if len(r) != METADATA_SIZE:
        raise ValueError('a metadata record is 0x34 bytes')
    return r + bytes(~b & 0xFF for b in r)


def mirror_decode(b):
    """S-02.063: the mirrored record when the 0x68 bytes are a valid mirror (the magic at +0, and every byte
    at +0x34+i the complement of the byte at +i), else None. A full image may be passed."""
    b = bytes(b)
    if len(b) == FLASH_SIZE:
        b = b[MIRROR_OFF:MIRROR_OFF + MIRROR_LEN]
    r, c = b[:METADATA_SIZE], b[METADATA_SIZE:MIRROR_LEN]
    if len(c) != METADATA_SIZE or int.from_bytes(r[:4], 'little') != METADATA_MAGIC:
        return None
    if any((x ^ y) != 0xFF for x, y in zip(r, c)):
        return None
    return r


def _ok(v):
    return v is not False                 # Flash returns None, FlashModel True/False


def _program_sector(fl, base, content):
    """Program each page of `content` (a sector image) that holds a byte other than 0xFF, in page order."""
    for p in range(0, SECTOR, PAGE):
        chunk = bytes(content[p:p + PAGE])
        if chunk.count(0xFF) != len(chunk) and not _ok(fl.program(base + p, chunk)):
            return False
    return True


def write_metadata_record(fl, record, profile='conforming'):
    """One metadata write of the 0x34-byte `record` to a Flash or FlashModel.

    conforming (S-02.061, S-02.064): write and read back the mirror, then rewrite the 0xC000 sector with
    the record and its other bytes as read, and read it back. Returns True when both read-backs matched.
    factory: erase 0xC000 and program the 0x34 bytes, no mirror."""
    new = bytes(record.pack() if hasattr(record, 'pack') else record)
    if len(new) != METADATA_SIZE:
        raise ValueError('a metadata record is 0x34 bytes')
    if profile == 'factory':
        return _ok(fl.erase(METADATA_OFF)) and _ok(fl.program(METADATA_OFF, new))
    if profile != 'conforming':
        raise ValueError('unknown metadata profile %r' % profile)
    sector = bytearray(fl.read(METADATA_OFF, SECTOR))
    sector[:METADATA_SIZE] = new
    m = mirror_encode(new)
    if not (_ok(fl.erase(MIRROR_OFF)) and _ok(fl.program(MIRROR_OFF, m))):
        return False
    if fl.read(MIRROR_OFF, MIRROR_LEN) != m:
        return False                      # 0xC000 left untouched
    if not (_ok(fl.erase(METADATA_OFF)) and _program_sector(fl, METADATA_OFF, sector)):
        return False
    return fl.read(METADATA_OFF, SECTOR) == bytes(sector)


def metadata_restore(fl, profile='conforming'):
    """The boot-time restore (S-02.065). conforming: when the mirror is valid and differs from the record
    at 0xC000, rewrite that sector from the mirror and read it back. factory: nothing.
    Returns 'none', 'restored', or 'failed'."""
    if profile == 'factory':
        return 'none'
    m = mirror_decode(fl.read(MIRROR_OFF, MIRROR_LEN))
    if m is None:
        return 'none'
    sector = bytearray(fl.read(METADATA_OFF, SECTOR))
    if bytes(sector[:METADATA_SIZE]) == m:
        return 'none'
    sector[:METADATA_SIZE] = m
    if not (_ok(fl.erase(METADATA_OFF)) and _program_sector(fl, METADATA_OFF, sector)):
        return 'failed'
    return 'restored' if fl.read(METADATA_OFF, SECTOR) == bytes(sector) else 'failed'
