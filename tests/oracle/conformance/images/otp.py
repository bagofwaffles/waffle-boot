# SPDX-License-Identifier: GPL-2.0-or-later
"""A model of the unit's OTP items as read through the boot ROM's OTP interface (S-16.095), holding
only test keys (`tfw.test_key_set`)."""

from dataclasses import dataclass, field

from . import p224, tfw

ITEM_HUK = 0x1
ITEM_AES = {0: (0x3, 0x4), 1: (0x5, 0x6)}
ITEM_PUBKEY = (0x9, 0xA)
ITEM_Q0 = ITEM_PUBKEY[0]
ITEM_GP1 = 0xE

HUK_SIZE = 32
AES_HALF = 16
PUBKEY_SIZE = 0x38
GP1_SIZE = 32                 # a test choice
GP1_COUNTED = 28
PROVISIONED_MIN = 15          # nonzero bytes among the first 28
SERIAL_LEN = 18
FSN_SIZE = 16
ROM_OTP_ADI_GET = 0x0400004C    # the ADI-area read's ROM entry
ROM_OTP_CFG = 0x04000054        # the OTP configuration entry
ADI_FSN = 0x13                # its FSN command


def item_bytes(words):
    """An item's bytes from its 32-bit words, each least significant byte first."""
    return b''.join((w & 0xFFFFFFFF).to_bytes(4, 'little') for w in words)


def item_words(b):
    """The inverse of `item_bytes` (`b` zero-padded to a whole word)."""
    b = bytes(b) + bytes(-len(b) % 4)
    return [int.from_bytes(b[i:i + 4], 'little') for i in range(0, len(b), 4)]


def gp1(nonzero, size=GP1_SIZE, fill=0xA5):
    """GP1 bytes with exactly `nonzero` nonzero bytes among the first 28 (the rest of the item zero)."""
    if not 0 <= nonzero <= GP1_COUNTED:
        raise ValueError('0..28 nonzero bytes')
    return bytes([fill] * nonzero + [0] * (size - nonzero))


def is_provisioned(gp1_bytes):
    """At least PROVISIONED_MIN of the first 28 GP1 bytes are nonzero."""
    return gp1_bytes is not None and sum(1 for b in gp1_bytes[:GP1_COUNTED] if b) >= PROVISIONED_MIN


@dataclass
class Otp:
    """item number -> bytes; an absent item fails to read. `fsn` is the ADI-area read (None: it fails)."""
    items: dict = field(default_factory=dict)
    fsn: bytes = None
    failing: set = field(default_factory=set)     # items whose read fails although present
    cfg_return: int = 1                           # what the configuration entry returns
    cfg_calls: int = 0

    def configure(self):
        """The OTP configuration entry: counts the call and returns `cfg_return`; reads ignore it."""
        self.cfg_calls += 1
        return self.cfg_return

    def read(self, item):
        """(ok, bytes): the ROM interface's read of one item."""
        if item in self.failing or item not in self.items:
            return False, b''
        return True, bytes(self.items[item])

    def read_words(self, item):
        """(ok, words): the same read as 32-bit words."""
        ok, b = self.read(item)
        return ok, item_words(b) if ok else []

    def adi_read(self, command):
        """(ret, words): the ADI-area read. Command 0x13 returns 1 and the FSN's four words; a missing FSN or
        any other command returns 0."""
        if command != ADI_FSN or self.fsn is None:
            return 0, []
        return 1, item_words(self.fsn)

    def aes_key(self, index):
        """K0 or K1 (32 bytes), or None when either of its items does not read."""
        parts = [self.read(i) for i in ITEM_AES[index]]
        if not all(ok for ok, _ in parts):
            return None
        if any(len(b) < AES_HALF for _, b in parts):
            return None
        return b''.join(b[:AES_HALF] for _, b in parts)

    def public_key(self, slot=0):
        """The P-224 point in secure-boot slot `slot` (0: item 9, Q0; 1: item 0xA), or None."""
        ok, b = self.read(ITEM_PUBKEY[slot])
        if not ok or len(b) < PUBKEY_SIZE:
            return None
        pt = p224.pub_from_bytes(b[:PUBKEY_SIZE])         # the first 0x38 bytes
        return pt if p224.on_curve(pt) else None

    def serial(self):
        """The chip serial (S-04.047): item 1's first 18 bytes up to the first 00; '' when item 1 does not read
        or a byte is outside 0x20-0x7E or is ';'."""
        ok, b = self.read(ITEM_HUK)
        s = b[:SERIAL_LEN].split(b'\0', 1)[0] if ok else b''
        if any(not 0x20 <= x <= 0x7E or x == 0x3B for x in s):
            return ''
        return s.decode('ascii')

    def serial_read(self):
        """Whether item 1 reads (S-17.015, S-08.045)."""
        return self.read(ITEM_HUK)[0]

    def provisioned(self):
        ok, b = self.read(ITEM_GP1)
        return ok and is_provisioned(b)

    def keyset(self):
        """{'K0', 'K1', 'Q0'}, None for each absent."""
        return {'K0': self.aes_key(0), 'K1': self.aes_key(1), 'Q0': self.public_key(0)}


def build_otp(keys=None, k0=True, k1=True, q0=True, q1=True, gp1_nonzero=GP1_COUNTED, serial='TESTSERIAL00000001',
              fsn=bytes(range(0x10, 0x10 + FSN_SIZE))):
    """An OTP holding the conformance test keys (`keys`: a tfw.TestKeySet, default tfw.test_key_set()).
    k0/k1/q0/q1 False leaves that key's items absent; gp1_nonzero None leaves GP1 absent; serial None leaves
    the HUK absent; fsn None makes the ADI-area read fail. Item 0xA holds signature index 0x01's test key."""
    ks = keys or tfw.test_key_set()
    items = {}
    if serial is not None:
        s = serial.encode('latin-1') if isinstance(serial, str) else bytes(serial)
        if len(s) > SERIAL_LEN:
            raise ValueError('serial over 18 characters')
        items[ITEM_HUK] = s.ljust(HUK_SIZE, b'\0')
    for idx, present, key in ((0, k0, ks.k0), (1, k1, ks.k1)):
        if present:
            lo, hi = ITEM_AES[idx]
            items[lo], items[hi] = key[:AES_HALF], key[AES_HALF:]
    if q0:
        items[ITEM_PUBKEY[0]] = p224.pub_bytes(ks.q0)
    if q1:
        items[ITEM_PUBKEY[1]] = p224.pub_bytes(ks.sign_for(tfw.SIG_DEVICE)[1])
    if gp1_nonzero is not None:
        items[ITEM_GP1] = gp1(gp1_nonzero)
    return Otp(items, fsn)
