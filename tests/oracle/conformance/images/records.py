# SPDX-License-Identifier: GPL-2.0-or-later
"""Persistent records (little-endian): the reboot record, and the SPI-flash metadata, selector and slot
records, with their builders, parsers and reader/writer rules."""

import struct
from dataclasses import dataclass, field

from .crc import crc32

# ---------------------------------------------------------------------------------------------
# The reboot record

REBOOT_RECORD_ADDR = 0x11B01F00
REBOOT_RECORD_SIZE = 8            # u32 magic, u16 reason, 2 pad bytes
REBOOT_RECORD_USED = 6            # the bytes a writer stores
REBOOT_MAGIC = 0xDEADF00D
REASON_DEFAULT = 20               # read when the magic is absent

# Reasons (S-04.006)
REASON_POWER_OFF = 0              # boots the OS
REASON_STAY_CDC = 10              # stay, main menu, CDC port added
REASON_BOOT_OS = 20               # boot the OS
REASON_UPGRADE_MSC = 30           # stay, upgrade screen over mass storage
REASON_UPGRADE_MIDI = 40          # stay, upgrade screen over USB MIDI
REASON_HOST_MODE4 = 50            # boots the OS
REASON_STAY_MAIN = 60             # stay, main menu
STAY_REASONS = frozenset({10, 30, 40, 60})


@dataclass
class RebootRecord:
    magic: int = REBOOT_MAGIC
    reason: int = REASON_BOOT_OS
    pad: int = 0

    def pack(self):
        return struct.pack('<IHH', self.magic & 0xFFFFFFFF, self.reason & 0xFFFF, self.pad & 0xFFFF)

    @classmethod
    def unpack(cls, b):
        if len(b) < REBOOT_RECORD_USED:
            raise ValueError('reboot record needs 6 bytes')
        b = bytes(b[:REBOOT_RECORD_SIZE]).ljust(REBOOT_RECORD_SIZE, b'\0')
        m, r, p = struct.unpack('<IHH', b)
        return cls(m, r, p)

    @property
    def valid(self):
        return self.magic == REBOOT_MAGIC


def build_reboot_record(reason):
    """The 6 bytes a writer stores (magic then reason), padded to the 8-byte record."""
    return RebootRecord(REBOOT_MAGIC, reason).pack()


def consume_reboot_record(mem):
    """The reader's rule, applied to a mutable 8-byte buffer: with the magic present the magic is
    zeroed and the reason returned; otherwise the buffer is left as it is and 20 is returned."""
    rec = RebootRecord.unpack(mem)
    if rec.valid:
        mem[0:4] = b'\0\0\0\0'
        return rec.reason
    return REASON_DEFAULT


def reason_boots_os(reason):
    """True when a boot that consumed `reason` goes on to boot the OS."""
    return reason not in STAY_REASONS


def reason_menu(reason):
    """The screen a staying boot opens: 'upgrade' for 30 and 40, 'main' otherwise."""
    return 'upgrade' if reason in (30, 40) else 'main'


REASONS_DEFINED = (0, 10, 20, 30, 40, 50, 60)
REASONS_OS_WRITES = (0, 10, 20, 30, 40)   # S-04.030


def reason_behavior(reason):
    """S-04.006 for the low 16 bits of `reason`: whether it boots the OS, the first held screen, whether
    it adds the CDC port, and the upgrade transport ('msc', 'midi' or '-'). Unnamed values act as 20."""
    reason &= 0xFFFF
    return {'boots': reason_boots_os(reason), 'screen': reason_menu(reason), 'cdc': reason == REASON_STAY_CDC,
            'transport': {REASON_UPGRADE_MSC: 'msc', REASON_UPGRADE_MIDI: 'midi'}.get(reason, '-')}


def host_mode_reason(mode_payload):
    """S-04.021: the reason for a host reboot request's mode bytes: first byte 1 -> 30, 2 -> 10, 4 -> 50,
    anything else or none -> 40."""
    if not mode_payload:
        return REASON_UPGRADE_MIDI
    return {1: REASON_UPGRADE_MSC, 2: REASON_STAY_CDC, 4: REASON_HOST_MODE4}.get(mode_payload[0], REASON_UPGRADE_MIDI)


def nordic_step(reason):
    """S-04.027: 'exit' for 20, 'none' for 0, else 'enter'."""
    if reason == REASON_BOOT_OS:
        return 'exit'
    return 'none' if reason == REASON_POWER_OFF else 'enter'


# S-04.020: the reboot record writers. reason: the value written ('host_mode' = host_mode_reason(payload),
# 'taken' = the reason this boot took). reset: 'hard' | 'soft'. nordic: nordic_step's result.
# shutdown (S-04.063), psoc_reset (S-04.065), slave_off (S-04.032): steps before the reset; None = n/a.
# min_delay_ms: least time from the end of the event to the reset.
WRITERS = {
    'W1': dict(event='host reboot request with a mode byte', reason='host_mode', reset='hard', nordic='enter',
               shutdown=True, psoc_reset=False, min_delay_ms=0,
               slave_off=True),
    'W2': dict(event='host reboot-to-OS request, or Continue to the OS while the slave is absent',
               reason=20, reset='soft', nordic='exit', shutdown=True,
               psoc_reset=True, min_delay_ms=0,
               slave_off=True),
    'W3': dict(event='MIDI transfer reboot sub-command 05 after an upload', reason=20, reset='hard', nordic='exit',
               shutdown=True, psoc_reset=False, min_delay_ms=10,
               slave_off=True),
    'W4': dict(event='firmware scan finds the CDC request file', reason=10, reset='hard', nordic='enter',
               shutdown=True, psoc_reset=False, min_delay_ms=0,
               slave_off=True),
    'W6': dict(event='CPU-link power-off request ends held mode, last PSoC power state 0x16', reason=20,
               reset='hard', nordic='exit', shutdown=True, psoc_reset=False, min_delay_ms=0,
               slave_off=True),
    'W7': dict(event="the developer menu's restart, digit 6", reason='taken', reset='hard', nordic='none',
               shutdown=True, psoc_reset=False, min_delay_ms=0,
               slave_off=True),
    'W10': dict(event='developer request for a calibrated start', reason=70, reset='hard', nordic='none',
                shutdown=True, psoc_reset=False, min_delay_ms=0,
                slave_off=True),
    'W11': dict(event='Continue to the OS, the slave present', reason=80, reset='hard', nordic='exit',
                shutdown=True, psoc_reset=False, min_delay_ms=0,
                slave_off=True),
    'W12': dict(event='Start /os.ldr once, the slave present', reason=90, reset='hard', nordic='exit',
                shutdown=True, psoc_reset=False, min_delay_ms=0,
                slave_off=True),
    'W9': dict(event='slave power-off request, sent by every master shutdown (slave record)', reason=0,
               reset='hard', nordic='none', shutdown=None, psoc_reset=False, min_delay_ms=0,
               slave_off=None),
}


def writer_reason(writer_id, taken=REASON_DEFAULT, mode_payload=b''):
    """The reason writer `writer_id` stores, given the reason this boot took and, for W1, the mode bytes."""
    r = WRITERS[writer_id]['reason']
    if r == 'host_mode':
        return host_mode_reason(mode_payload)
    if r == 'taken':
        return taken & 0xFFFF
    return r


reboot_record = build_reboot_record


# ---------------------------------------------------------------------------------------------
# SPI flash records

SECTOR = 0x1000
METADATA_OFF = 0xC000
SELECTOR_OFF = 0xD000
SLOT_RECORD_OFF = (0xE000, 0xF000)
SLOT_OFF = (0x10000, 0x88000)
SLOT_MAX = 0x78000
SLOT_TAG = 0xB004

METADATA_SIZE = 0x34
METADATA_MAGIC = 0xDEADBABE
METADATA_STATE_IDLE = 0
METADATA_STATE_UPGRADING = 0xAB0D


def _ver(v):
    v = tuple(v) + (0,) * (4 - len(tuple(v)))
    return tuple(int(x) & 0xFFFF for x in v[:4])


@dataclass
class MetadataRecord:
    """The 0x34-byte metadata record at flash offset 0xC000."""
    magic: int = METADATA_MAGIC
    serial: bytes = b''                 # up to 8 characters, NUL-terminated in a 10-byte field
    sku: int = 1
    factory: int = 0
    os_version: tuple = (0, 0, 0, 0)    # major, minor, patch, build
    sw_version: tuple = (0, 0, 0, 0)
    state: int = METADATA_STATE_IDLE
    cv_gain: int = 0
    cv_zero: int = 0
    extra: tuple = (0xFFFF, 0xFFFF)     # the second pair at +0x2e

    FMT = '<I10sHH4H4H2xIIH2H2x'   # 2 pad bytes at +0x22 and at +0x32

    def pack(self):
        serial = bytes(self.serial)[:9].ljust(10, b'\0')
        return struct.pack(self.FMT, self.magic & 0xFFFFFFFF, serial, self.sku & 0xFFFF,
                           self.factory & 0xFFFF, *_ver(self.os_version), *_ver(self.sw_version),
                           self.state & 0xFFFFFFFF, self.cv_gain & 0xFFFFFFFF, self.cv_zero & 0xFFFF,
                           *(x & 0xFFFF for x in self.extra))

    @classmethod
    def unpack(cls, b):
        b = bytes(b[:METADATA_SIZE])
        if len(b) != METADATA_SIZE:
            raise ValueError('metadata record needs 0x34 bytes')
        f = struct.unpack(cls.FMT, b)
        return cls(f[0], f[1].split(b'\0', 1)[0], f[2], f[3], tuple(f[4:8]), tuple(f[8:12]),
                   f[12], f[13], f[14], tuple(f[15:17]))


assert struct.calcsize(MetadataRecord.FMT) == METADATA_SIZE


def read_metadata(b):
    """S-04.042: the reader's view as a MetadataRecord: zeros without the magic; serial +0x04..+0x0B up
    to the first NUL; a state other than 0xAB0D reads as 0."""
    rec = MetadataRecord.unpack(b)
    if rec.magic != METADATA_MAGIC:
        return MetadataRecord.unpack(bytes(METADATA_SIZE))
    rec.serial = bytes(b[0x04:0x0C]).split(b'\0', 1)[0]
    if rec.state != METADATA_STATE_UPGRADING:
        rec.state = METADATA_STATE_IDLE
    return rec


def decode_metadata(b):
    """read_metadata as a dict: serial (str), sku, factory, os_version, sw_version (4-tuples), state,
    and `valid` (the magic was present)."""
    rec = read_metadata(b)
    return {'valid': MetadataRecord.unpack(b).magic == METADATA_MAGIC, 'serial': rec.serial.decode('latin-1'),
            'sku': rec.sku, 'factory': rec.factory, 'os_version': tuple(rec.os_version),
            'sw_version': tuple(rec.sw_version), 'state': rec.state}


def build_metadata(serial=b'', sku=1, factory=0, os_version=(0, 0, 0, 0), sw_version=(0, 0, 0, 0),
                   state=METADATA_STATE_IDLE, cv_gain=0xFFFFFFFF, cv_zero=0, cv_pair2=(0xFFFF, 0xFFFF),
                   magic=METADATA_MAGIC):
    """A 0x34-byte metadata record (S-04.040). `state` may be 'idle', 'upgrading' or a number; the serial
    may be str or bytes (8 characters, stored with a NUL)."""
    state = {'idle': METADATA_STATE_IDLE, 'upgrading': METADATA_STATE_UPGRADING}.get(state, state)
    if isinstance(serial, str):
        serial = serial.encode('latin-1')
    return MetadataRecord(magic, serial, sku, factory, tuple(os_version), tuple(sw_version), state, cv_gain,
                          cv_zero, tuple(cv_pair2)).pack()


def write_metadata(before, os_version, sw_version, state, factory_mode=False, serial=b'', sku=0, factory=0):
    """S-04.043: the 0x34 bytes a write stores over `before`. Sets magic, both versions and state; with
    `factory_mode` also serial (+0x04..+0x0C), sku and factory. Every other byte is kept from `before`."""
    if state not in (METADATA_STATE_IDLE, METADATA_STATE_UPGRADING):
        raise ValueError('a write stores state 0 or 0xAB0D')
    b = bytearray(bytes(before[:METADATA_SIZE]).ljust(METADATA_SIZE, b'\xff'))
    struct.pack_into('<I', b, 0x00, METADATA_MAGIC)
    if factory_mode:
        if isinstance(serial, str):
            serial = serial.encode('latin-1')
        b[0x04:0x0D] = bytes(serial)[:8].ljust(9, b'\0')
        struct.pack_into('<HH', b, 0x0E, sku & 0xFFFF, factory & 0xFFFF)
    struct.pack_into('<4H', b, 0x12, *_ver(os_version))
    struct.pack_into('<4H', b, 0x1A, *_ver(sw_version))
    struct.pack_into('<I', b, 0x24, state)
    return bytes(b)


@dataclass
class Selector:
    """The two bytes at 0xD000: the slot index and the `verified` flag."""
    index: int = 0
    verified: int = 0

    def pack(self):
        return bytes([self.index & 0xFF, self.verified & 0xFF])

    @classmethod
    def unpack(cls, b):
        return cls(b[0], b[1])

    @property
    def slot(self):
        """Index 0 selects slot 0; any other index selects slot 1."""
        return 0 if self.index == 0 else 1

    @property
    def trusted(self):
        return self.verified == 1


ERASED_SELECTOR = Selector(0xFF, 0xFF)


@dataclass
class SlotRecord:
    """A 12-byte slot record: CRC-32 of the first `length` slot bytes, the length, the tag."""
    crc: int = 0
    length: int = 0
    tag: int = SLOT_TAG

    def pack(self):
        return struct.pack('<III', self.crc & 0xFFFFFFFF, self.length & 0xFFFFFFFF, self.tag & 0xFFFFFFFF)

    @classmethod
    def unpack(cls, b):
        return cls(*struct.unpack('<III', bytes(b[:12])))

    @classmethod
    def for_image(cls, image):
        return cls(crc32(image), len(image), SLOT_TAG)


def slot_record_check(rec, slot_bytes):
    """'ok' when the tag is 0xB004 and the CRC-32 of the first `length` slot bytes equals `crc`, else
    'bad_tag' or 'bad_crc'."""
    if rec.tag != SLOT_TAG:
        return 'bad_tag'
    if crc32(slot_bytes[:rec.length]) != rec.crc:
        return 'bad_crc'
    return 'ok'
