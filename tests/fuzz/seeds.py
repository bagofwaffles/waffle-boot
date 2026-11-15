# SPDX-License-Identifier: GPL-2.0-or-later
"""Write the fuzzers' seed corpora.

    python3 -I tests/fuzz/seeds.py OUTDIR

OUTDIR/<fuzzer>/ receives inputs in each fuzzer's format (tests/fuzz/
fuzz_*.c) made from tests/oracle/: the S-12 volumes from its generator, the
zips and package signatures of S-11, and deflate streams.
The inputs of past findings, kept in tests/fuzz/regressions/<fuzzer>/, are
copied in as they are.
"""

import hashlib
import json
import os
import struct
import sys
import zlib

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = os.path.join(TOP, 'tests', 'oracle')
REGRESSIONS = os.path.join(TOP, 'tests', 'fuzz', 'regressions')
sys.path.insert(0, SPEC)
sys.path.insert(0, os.path.join(SPEC, 'S-12-filesystems'))

import gen_vectors as s12  # noqa: E402
from conformance.images import p224, tfw  # noqa: E402

FAT_BACKED = 16384 * 512          # fuzz_fatfs.c: BACKED_SECTORS
YAFFS_CHUNKS = 8 * 128            # fuzz_yaffs2.c: CHUNKS


def put(out, name, data):
    os.makedirs(os.path.dirname(os.path.join(out, name)), exist_ok=True)
    with open(os.path.join(out, name), 'wb') as f:
        f.write(data)


def fat_seed(disk):
    """The disk's first sectors, up to the last one written that fuzz_fatfs keeps."""
    last = max((n for n in disk.units if (n + 1) * disk.unit <= FAT_BACKED), default=-1)
    return disk.to_bytes()[:(last + 1) * disk.unit]


def yaffs_seed(img):
    """fuzz_yaffs2's records for every written chunk of a yaffs2 image."""
    out = bytearray()
    for n in sorted(img.units):
        if n >= YAFFS_CHUNKS:
            continue
        chunk = img.units[n]
        data = chunk[:-16].rstrip(b'\0')
        out += struct.pack('<HH', n, len(data)) + data + chunk[-16:]
    return bytes(out)


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: seeds.py OUTDIR')
    out = sys.argv[1]

    put(out, 'fatfs/fresh', fat_seed(s12.fresh_volume().build()))
    put(out, 'fatfs/populated', fat_seed(s12.populated_volume()[1]))
    for case in s12.HOST_CASES:
        put(out, 'fatfs/host_%s' % case['kind'], fat_seed(s12.host_volume(case)))

    for case in s12.YAFFS_CASES:
        img, _ = s12.yaffs_image(case)
        put(out, 'yaffs2/%s' % case['name'], yaffs_seed(img))
    put(out, 'yaffs2/trust', yaffs_seed(s12.trust_image(s12.yp())[0]))

    samples = [b'', b'hello, world\n' * 9, bytes(range(256)) * 40]
    for i, data in enumerate(samples):
        for mode, wbits in enumerate((-15, 15, 31)):
            c = zlib.compressobj(6, zlib.DEFLATED, wbits)
            put(out, 'inflate/%d_%d' % (i, mode), bytes([mode]) + c.compress(data) + c.flush())

    with open(os.path.join(SPEC, 'S-11-packages', 'vectors', 'zips.json')) as f:
        for case in json.load(f)['cases']:
            put(out, 'unzip/%s' % case['case'], bytes.fromhex(case['zip_hex']))

    ks = tfw.test_key_set()
    q0 = p224.pub_bytes(ks.q0)
    with open(os.path.join(SPEC, 'S-11-packages', 'vectors', 'packages.json')) as f:
        for case in json.load(f)['cases'][:20]:
            with open(os.path.join(SPEC, 'S-11-packages', 'vectors', case['file']), 'rb') as g:
                data = g.read()
            mofs = 0x80 if case['form'] == 'file' else 0x40
            mb = data[mofs:mofs + 0x300]
            if len(mb) < 0x300:
                continue
            seed = q0 + mb[0x14:0x30] + mb[0x34:0x50] + tfw.signed_digest(mb)
            put(out, 'p224/%s' % case['case'], seed)
    put(out, 'p224/abc', q0 + bytes(56) + hashlib.sha256(b'abc').digest())

    for fuzzer in sorted(os.listdir(REGRESSIONS)):
        for name in sorted(os.listdir(os.path.join(REGRESSIONS, fuzzer))):
            with open(os.path.join(REGRESSIONS, fuzzer, name), 'rb') as f:
                put(out, '%s/regression_%s' % (fuzzer, name), f.read())


if __name__ == '__main__':
    main()
