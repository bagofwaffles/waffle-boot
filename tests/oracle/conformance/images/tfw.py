# SPDX-License-Identifier: GPL-2.0-or-later
"""The .tfw firmware package (optional preamble, TE header, 0x300-byte manifest, name block and zip):
builder, parser and verifier. Every key here is a test key derived from a seed (`test_keys()`)."""

import hashlib
import struct
from dataclasses import dataclass, field

from . import aes, p224
from .crc import crc16_xmodem

PREAMBLE_MAGIC = 0xBABECAFE
HEADER_MAGIC = 0xBEEFCAFE
PREAMBLE_SIZE = HEADER_SIZE = 0x40
MANIFEST_SIZE = 0x300
NAME_BLOCK = 0x100
UNIT = 0x80
NAME_ZIP_WINDOW = 0x7C
SIG_LO, SIG_HI = 0x10, 0x50
MANIFEST_TAG = b'TESG'
BLOCK_VERSION = 1

DEVICE_REGULAR, DEVICE_NORDIC, DEVICE_FORCED_CONTENT = 1, 3, 6
KEY_OTP0, KEY_OTP1, KEY_PLAINTEXT, KEY_DEV = 0, 1, 0xFE, 0xFF
SIG_CLOUD, SIG_DEVICE, SIG_DEV = 0, 1, 0xFF
PRODUCT_ID = 0x21
FORCED_VERSION = (0, 1, 0)


def product_word(product=PRODUCT_ID, variant=0, sku=0):
    return (product << 14) | (variant << 10) | sku


def split_product_word(w):
    product = w >> 14
    return {'product': 0 if product > 999 else product, 'variant': (w >> 10) & 0xF, 'sku': w & 0x3FF}


def test_keys(seed=b'opxy-conformance'):
    """(aes key 32 bytes, P-224 private int, public point): test keys derived from `seed`."""
    if isinstance(seed, str):
        seed = seed.encode()
    d, pub = p224.test_key(seed)
    return hashlib.sha256(b'aes test key' + seed).digest(), d, pub


@dataclass
class TestKeySet:
    """Test keys per key index (S-11.015, S-11.021): K0 for payload key index 0x00, K1 for 0x01, Q0
    (private d, public point) for signature key index 0x00. `aes_for`/`sign_for` give a distinct test
    key for any other index."""
    k0: bytes
    k1: bytes
    q0_d: int
    q0: tuple
    seed: bytes = b''

    def aes_for(self, index):
        if index == KEY_OTP0:
            return self.k0
        if index == KEY_OTP1:
            return self.k1
        return test_keys(self.seed + b'-aes-%02x' % index)[0]

    def sign_for(self, index):
        """(d, public point) for signature key index `index`."""
        if index == SIG_CLOUD:
            return self.q0_d, self.q0
        return p224.test_key(self.seed + b'-sig-%02x' % index)


def test_key_set(seed=b'opxy-conformance'):
    """The per-index test keys. With the default seed: K0 and Q0 are `test_keys()`, and K1 is
    `test_keys(b'opxy-conformance-k1')[0]`."""
    if isinstance(seed, str):
        seed = seed.encode()
    k0, d, pub = test_keys(seed)
    return TestKeySet(k0, test_keys(seed + b'-k1')[0], d, pub, seed)


@dataclass
class Preamble:
    type: int = 0
    crc16: int = 0
    version: tuple = (0, 0, 0, 0)

    def pack(self):
        return struct.pack('>IBH4H', PREAMBLE_MAGIC, self.type, self.crc16, *self.version).ljust(PREAMBLE_SIZE, b'\0')

    @classmethod
    def unpack(cls, b):
        magic, t, c, *v = struct.unpack_from('>IBH4H', b)
        if magic != PREAMBLE_MAGIC:
            raise ValueError('no preamble')
        return cls(t, c, tuple(v))

    @property
    def forced(self):
        return tuple(self.version[:3]) == FORCED_VERSION


@dataclass
class TeHeader:
    device_id: int = DEVICE_REGULAR
    length: int = 0
    crc16: int = 0
    version: tuple = (0, 0, 0, 0)
    address: int = 0
    product: int = product_word()

    FMT = '>IBIH4HII'

    def pack(self):
        return struct.pack(self.FMT, HEADER_MAGIC, self.device_id, self.length, self.crc16, *self.version,
                           self.address, self.product).ljust(HEADER_SIZE, b'\0')

    @classmethod
    def unpack(cls, b):
        magic, dev, ln, c, v0, v1, v2, v3, addr, prod = struct.unpack_from(cls.FMT, b)
        if magic != HEADER_MAGIC:
            raise ValueError('bad TE header magic')
        return cls(dev, ln, c, (v0, v1, v2, v3), addr, prod)


assert struct.calcsize(TeHeader.FMT) == 0x1B


@dataclass
class Manifest:
    aes_key_index: int = KEY_DEV
    sig_r: int = 0
    sig_s: int = 0
    iv: bytes = bytes(16)
    zip_length: int = 0
    block_version: int = BLOCK_VERSION
    sig_key_index: int = SIG_DEV
    product: int = product_word()
    payload_length: int = 0
    payload_sha256: bytes = bytes(32)
    tag: bytes = MANIFEST_TAG

    sig_pad: bytes = bytes(8)   # +0x10..+0x13 and +0x30..+0x33, outside r and s (S-11.012)

    def pack(self, with_signature=True):
        m = bytearray(MANIFEST_SIZE)
        m[0x04] = self.aes_key_index
        if with_signature:
            m[0x10:0x30] = self.sig_r.to_bytes(32, 'big')
            m[0x30:0x50] = self.sig_s.to_bytes(32, 'big')
            m[0x10:0x14] = bytes(self.sig_pad[:4]).ljust(4, b'\0')
            m[0x30:0x34] = bytes(self.sig_pad[4:8]).ljust(4, b'\0')
        m[0x70:0x80] = self.iv
        struct.pack_into('<I', m, 0x80, self.zip_length)
        m[0x84:0x88] = self.tag
        m[0x88] = self.block_version
        m[0x89] = self.sig_key_index
        struct.pack_into('<I', m, 0x8C, self.product)
        struct.pack_into('<I', m, 0x98, self.payload_length)
        m[0x9C:0xBC] = self.payload_sha256
        return bytes(m)

    @classmethod
    def unpack(cls, b):
        b = bytes(b[:MANIFEST_SIZE])
        return cls(b[0x04], int.from_bytes(b[0x14:0x30], 'big'), int.from_bytes(b[0x34:0x50], 'big'),
                   b[0x70:0x80], struct.unpack_from('<I', b, 0x80)[0], b[0x88], b[0x89],
                   struct.unpack_from('<I', b, 0x8C)[0], struct.unpack_from('<I', b, 0x98)[0],
                   b[0x9C:0xBC], b[0x84:0x88], b[0x10:0x14] + b[0x30:0x34])


def signed_digest(manifest_bytes):
    m = bytearray(manifest_bytes[:MANIFEST_SIZE])
    m[SIG_LO:SIG_HI] = bytes(SIG_HI - SIG_LO)
    return hashlib.sha256(m).digest()


def _pad(b, unit=UNIT):
    return b + bytes(-len(b) % unit)


SIGNATURE_FORMS = ('valid', 'unsigned', 'other_key', 'r_zero', 's_zero', 'r_n', 's_n', 'flip_r', 'flip_s',
                   'flip_manifest')


def build(zip_bytes, name='firmware.zip', version=(1, 0, 0, 0), device_id=DEVICE_REGULAR, product=None,
          preamble=None, aes_key_index=KEY_OTP0, sig_key_index=SIG_CLOUD, keys=None, iv=None, address=0,
          aes_key=None, signature='valid', sig_pad=bytes(8), sign_d=None):
    """A .tfw holding `zip_bytes`. `preamble`: None, or a version tuple for a preamble.

    `aes_key_index` 0xfe stores the payload in plaintext; any other index encrypts it under that index's
    test key (`aes_key` overrides it). The manifest is signed with the test key of `sig_key_index`.
    `keys`: a TestKeySet (default `test_key_set()`), or a legacy (aes, d, pub) tuple used for every index.
    `sign_d`: a P-224 private key to sign with in place of the index's test key.
    `signature`: one of SIGNATURE_FORMS; anything but 'valid' fails S-11.012 ('other_key': signed under
    an untrusted key; 'flip_manifest': a signed manifest byte changed after signing). `sig_pad`: 8 bytes
    for +0x10..+0x13 and +0x30..+0x33, outside r and s."""
    if signature not in SIGNATURE_FORMS:
        raise ValueError('signature form %r' % (signature,))
    ks = _keyset(keys)
    product = product_word() if product is None else product
    nm = name.encode() if isinstance(name, str) else name
    if len(nm) > NAME_BLOCK:
        raise ValueError('name over 0x100 bytes')
    plain = nm.ljust(NAME_BLOCK, b'\0') + _pad(zip_bytes)
    iv = iv if iv is not None else hashlib.sha256(b'iv' + zip_bytes).digest()[:16]
    if aes_key_index == KEY_PLAINTEXT:
        payload = plain
    else:
        payload = aes.cbc_encrypt(aes_key or ks.aes_for(aes_key_index), iv, plain)
    man = Manifest(aes_key_index, 0, 0, iv, len(zip_bytes), BLOCK_VERSION, sig_key_index, product,
                   len(payload), hashlib.sha256(payload).digest(), sig_pad=bytes(sig_pad))
    if signature != 'unsigned':
        d = ks.sign_for(sig_key_index)[0] if sign_d is None else sign_d
        if signature == 'other_key':
            d = p224.test_key(ks.seed + b'-not-a-unit-key')[0]
        man.sig_r, man.sig_s = p224.sign(d, signed_digest(man.pack(False)))
        man.sig_r, man.sig_s = {
            'r_zero': (0, man.sig_s), 's_zero': (man.sig_r, 0), 'r_n': (p224.N, man.sig_s),
            's_n': (man.sig_r, p224.N), 'flip_r': (man.sig_r ^ 1, man.sig_s),
            'flip_s': (man.sig_r, man.sig_s ^ 1)}.get(signature, (man.sig_r, man.sig_s))
    mb = bytearray(man.pack())
    if signature == 'flip_manifest':
        mb[0x200] ^= 0x01                       # a reserved, signed byte
    body = bytes(mb) + payload
    hdr = TeHeader(device_id, len(body), crc16_xmodem(body), tuple(version), address, product)
    out = hdr.pack() + body
    if preamble is not None:
        out = Preamble(0, crc16_xmodem(out), tuple(preamble)).pack() + out
    return out


def build_bad_signature(zip_bytes, how='flip_s', **kw):
    """`build(zip_bytes, **kw)` with a failing signature (S-11.012, S-11.022): `how` is any
    SIGNATURE_FORMS entry but 'valid'."""
    if how == 'valid':
        raise ValueError('a bad-signature form')
    return build(zip_bytes, signature=how, **kw)


def _keyset(keys):
    if keys is None:
        return test_key_set()
    if isinstance(keys, TestKeySet):
        return keys
    a, d, pub = keys                     # legacy: one key of each kind for every index
    return TestKeySet(a, a, d, pub, b'legacy')


def verify_signature(manifest_bytes, pub):
    """S-11.012: ECDSA-P224 over the manifest with +0x10..+0x4F zeroed; e = leftmost 224 bits of SHA-256;
    r and s are the last 28 bytes of their 32-byte fields; 0 < r, s < n."""
    m = Manifest.unpack(manifest_bytes)
    return pub is not None and p224.verify(pub, signed_digest(manifest_bytes), m.sig_r, m.sig_s)


@dataclass
class Parsed:
    preamble: object
    header: TeHeader
    manifest: Manifest
    manifest_offset: int
    payload: bytes
    checks: dict = field(default_factory=dict)
    name: bytes = b''
    zip_bytes: bytes = b''

    @property
    def ok(self):
        return all(self.checks.values())


def parse(data, keys=None):
    """Parse and verify against the test keys. `checks` holds each rule's result: header_crc, product, tag,
    payload_length, payload_sha256, signature (against the key the signature key index selects), te_signed
    (index 0x00 and valid under Q0, S-11.021), payload_key (index 0x00, 0x01 or 0xfe, S-11.015) and name.
    The name and zip are recovered with that index's test AES key (or as plaintext for 0xfe). `ok` needs
    every check."""
    data = bytes(data)
    ks = _keyset(keys)
    pre, ofs = None, 0
    if data[:4] == PREAMBLE_MAGIC.to_bytes(4, 'big'):
        pre, ofs = Preamble.unpack(data), PREAMBLE_SIZE
    hdr = TeHeader.unpack(data[ofs:ofs + HEADER_SIZE])
    mofs = ofs + HEADER_SIZE
    mb = data[mofs:mofs + MANIFEST_SIZE]
    man = Manifest.unpack(mb)
    payload = data[mofs + MANIFEST_SIZE:]
    pw = split_product_word(hdr.product)
    sig_ok = verify_signature(mb, ks.sign_for(man.sig_key_index)[1])
    c = {'header_crc': crc16_xmodem(data[mofs:]) == hdr.crc16,
         'product': pw['product'] == PRODUCT_ID and pw['variant'] == 0,
         'tag': man.tag == MANIFEST_TAG and man.block_version == BLOCK_VERSION,
         'payload_length': man.payload_length == len(payload),
         'payload_sha256': hashlib.sha256(payload).digest() == man.payload_sha256,
         'signature': sig_ok,
         'te_signed': sig_ok and man.sig_key_index == SIG_CLOUD,
         'payload_key': man.aes_key_index in (KEY_OTP0, KEY_OTP1, KEY_PLAINTEXT)}
    p = Parsed(pre, hdr, man, mofs, payload, c)
    if len(payload) >= NAME_BLOCK and len(payload) % 16 == 0:
        plain = payload if man.aes_key_index == KEY_PLAINTEXT else aes.cbc_decrypt(ks.aes_for(man.aes_key_index),
                                                                                   man.iv, payload)
        p.name = plain[:NAME_BLOCK].rstrip(b'\0')
        p.zip_bytes = plain[NAME_BLOCK:NAME_BLOCK + man.zip_length]
        c['name'] = b'.zip' in plain[:NAME_ZIP_WINDOW]
    else:
        c['name'] = False
    return p
