# SPDX-License-Identifier: GPL-2.0-or-later
"""S-02.106: which bootloader a good or tried slot holds, and its synthetic vectors.

kind() applies the four rules in order and gives ('WB', version), ('TE', version), ('TE', None) or
('??', None). A vector's image is a fill pattern with four-byte patches that force a CRC-32, so no vector
holds a byte of a TE image; build() makes a case's bytes again from its fill and patches.
"""
import zlib

# S-02.106 rule 2: (length, CRC-32 of the slot's bytes) -> {major, minor, patch, build}.
TE_BUILDS = {
    (372144, 0x7AF4665C): (1000, 0, 12, 0),
    (374700, 0x0789B2D9): (1000, 0, 13, 0),
    (393216, 0x5DEE4B51): (1000, 0, 20, 0),
    (396452, 0xA930BEF3): (1000, 0, 23, 0),
}
# S-02.106 rule 3: the CRC-32 of the slot's bytes [0, PREFIX_LEN).
PREFIX_LEN = 4468
PREFIX_CRC = 0x31D25812


def crc32(data, crc=0):
    return zlib.crc32(data, crc) & 0xFFFFFFFF


def kind(n, c, prefix_crc, descriptor_valid, descriptor_version=None):
    """n and c as S-02.106 has them; prefix_crc None when n is below PREFIX_LEN."""
    if descriptor_valid:
        return ('WB', tuple(descriptor_version))
    v = TE_BUILDS.get((n, c))
    if v is not None:
        return ('TE', v)
    if n >= PREFIX_LEN and prefix_crc == PREFIX_CRC:
        return ('TE', None)
    return ('??', None)


def kind_of_bytes(data, descriptor_valid=False, descriptor_version=None):
    n = len(data)
    pc = crc32(data[:PREFIX_LEN]) if n >= PREFIX_LEN else None
    return kind(n, crc32(data), pc, descriptor_valid, descriptor_version)


def version_text(v):
    """major.minor.patch, with +build when build is not 0."""
    s = '%d.%d.%d' % tuple(v[:3])
    return s if v[3] == 0 else '%s+%d' % (s, v[3])


# ---- synthetic images ----

_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xEDB88320 if _c & 1 else _c >> 1
    _TABLE.append(_c)
# Each table entry's top byte is distinct, so it names its index.
_INV = {t >> 24: i for i, t in enumerate(_TABLE)}


def fill(length, seed):
    """Byte i is (i * 37 + seed) & 0xFF."""
    return bytearray(((i * 37 + seed) & 0xFF) for i in range(length))


def force(data, offset, target, end=None):
    """Rewrite data[offset:offset + 4] so that the CRC-32 of data[:end] (all of it when end is None) is
    target; the four bytes."""
    end = len(data) if end is None else end
    assert 0 <= offset and offset + 4 <= end
    # The register wanted right after the four bytes, from the target back over the tail.
    reg = target ^ 0xFFFFFFFF
    for b in reversed(data[offset + 4:end]):
        i = _INV[reg >> 24]
        reg = ((reg ^ _TABLE[i]) << 8 | (i ^ b)) & 0xFFFFFFFF
    idx = []
    for _ in range(4):
        i = _INV[reg >> 24]
        idx.append(i)
        reg = ((reg ^ _TABLE[i]) << 8) & 0xFFFFFFFF
    idx.reverse()
    reg = crc32(bytes(data[:offset])) ^ 0xFFFFFFFF
    out = bytearray()
    for i in idx:
        out.append((reg ^ i) & 0xFF)
        reg = (reg >> 8) ^ _TABLE[i]
    data[offset:offset + 4] = out
    assert crc32(bytes(data[:end])) == target
    return bytes(out)


def build(case):
    """A case's slot bytes: its fill, then its patches in order."""
    data = fill(case['length'], case['seed'])
    for p in case['patches']:
        off = p['offset']
        data[off:off + 4] = bytes.fromhex(p['bytes'])
    return bytes(data)


def _case(name, length, seed, forces, descriptor_valid=False, descriptor_version=None):
    """forces: (offset, target CRC-32, end or None), applied in order."""
    data = fill(length, seed)
    patches = [{'offset': off, 'bytes': force(data, off, target, end).hex()}
               for off, target, end in forces]
    k, v = kind_of_bytes(bytes(data), descriptor_valid, descriptor_version)
    return {
        'name': name, 'length': length, 'seed': seed, 'patches': patches,
        'crc32': '0x%08X' % crc32(bytes(data)),
        'descriptor_valid': descriptor_valid,
        'descriptor_version': list(descriptor_version) if descriptor_version else None,
        'kind': k, 'version': list(v) if v else None,
        'text': version_text(v) if v is not None and k == 'TE' else None,
    }


def cases():
    rows = []
    for k, ((n, c), v) in enumerate(sorted(TE_BUILDS.items(), key=lambda r: r[1])):
        rows.append(_case('te_row_%d_%d_%d' % v[:3], n, 100 + k, [(n // 2, c, None)]))
    by_version = {v: r for r, v in TE_BUILDS.items()}
    n13, c13 = by_version[(1000, 0, 13, 0)]
    n20, c20 = by_version[(1000, 0, 20, 0)]
    return rows + [
        # rule 2 takes the length and the CRC together
        _case('row_crc_wrong_length', n20 - 1, 3, [(1000, c20, None)]),
        _case('row_length_wrong_crc', n20, 4, []),
        # rule 3 alone, and at its shortest length
        _case('te_prefix_only', 0x40000, 5, [(PREFIX_LEN - 8, PREFIX_CRC, PREFIX_LEN)]),
        _case('te_prefix_shortest', PREFIX_LEN, 6, [(100, PREFIX_CRC, None)]),
        # rule 2 before rule 3
        _case('te_row_and_prefix', n13, 7,
              [(PREFIX_LEN - 8, PREFIX_CRC, PREFIX_LEN), (n13 // 2, c13, None)]),
        # rule 3's CRC over one byte fewer or more
        _case('prefix_span_short', 0x40000, 8, [(PREFIX_LEN - 9, PREFIX_CRC, PREFIX_LEN - 1)]),
        _case('prefix_span_long', 0x40000, 9, [(PREFIX_LEN - 7, PREFIX_CRC, PREFIX_LEN + 1)]),
        # an image one byte short of rule 3 whose CRC is the prefix's
        _case('prefix_too_short', PREFIX_LEN - 1, 10, [(100, PREFIX_CRC, None)]),
        # rule 1 first
        _case('wb_over_row', n20, 11,
              [(PREFIX_LEN - 8, PREFIX_CRC, PREFIX_LEN), (n20 // 2, c20, None)], True, (0, 9, 37, 0)),
        _case('wb_plain', 0x30000, 12, [], True, (1, 2, 3, 4)),
        _case('unknown', 0x30000, 13, []),
    ]
