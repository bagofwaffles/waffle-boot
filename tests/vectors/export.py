# SPDX-License-Identifier: GPL-2.0-or-later
"""Export the vectors of tests/oracle/ for the C unit tests.

    python3 -I tests/vectors/export.py OUTDIR

Reads the JSON vectors and package files under tests/oracle/ and writes line-based
files the unit tests in tests/unit/ parse without a JSON reader. Where a
vector does not state the answer a component must give, it is computed here
with an independent implementation: the conformance package
(tests/oracle/conformance: AES, P-224), Python's hashlib and zipfile.

Every line is a keyword and space-separated fields; byte strings are hex,
"-" when empty or absent. The S-12 disk and volume images are written by
tests/oracle/S-12-filesystems/gen_vectors.py --images OUTDIR/s12 (see Makefile).
"""

import hashlib
import io
import json
import os
import sys
import zipfile
import zlib

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = os.path.join(TOP, 'tests', 'oracle')
sys.path.insert(0, SPEC)

from conformance.images import aes, p224, tfw  # noqa: E402


def hx(b):
    return b.hex() if b else '-'


def load(*path):
    with open(os.path.join(SPEC, *path)) as f:
        return json.load(f)


def pattern(n, seed):
    """S-02's synthetic slot data: byte i is ((i*7+seed) ^ (i>>8)) & 0xff."""
    return bytes(((i * 7 + seed) ^ (i >> 8)) & 0xFF for i in range(n))


# --- CRCs --------------------------------------------------------------------

def crcs():
    out = []
    for v in load('S-02-spi-flash', 'vectors', 'records.json')['crc']:
        if 'data_ascii' in v:
            data = v['data_ascii'].encode()
        elif 'data_hex' in v:
            data = bytes.fromhex(v['data_hex'])
        elif 'fill' in v:
            data = bytes([int(v['fill'], 16)]) * v['length']
        elif 'pattern' in v:
            assert v['pattern'] == '((i*7+seed) ^ (i>>8)) & 0xff'
            data = pattern(v['length'], v['seed'])
        else:
            data = bytes(v['length'])
        out.append(('crc32', data, int(v['crc32'], 16)))
    for v in load('S-11-packages', 'vectors', 'crc16.json'):
        out.append(('crc16_xmodem', bytes.fromhex(v['data_hex']), int(v['crc16'], 16)))
    for v in load('S-06-framing', 'vectors', 'crc.json')['vectors']:
        out.append(('crc8_frame', bytes.fromhex(v['data']), int(v['crc'], 16)))
    for v in load('S-07-psoc', 'vectors', 'cypress.json')['checksum']:
        out.append(('psoc_check', bytes.fromhex(v['bytes']), int(v['crc'], 16)))
    return ['crc %s %s %x' % (algo, hx(data), value) for algo, data, value in out]


# --- Packages: SHA-256, P-224, AES-256-CBC (S-11) ------------------------------

def packages():
    ks = tfw.test_key_set()
    q0 = p224.pub_bytes(ks.q0)
    keys = {0x00: ks.k0, 0x01: ks.k1}
    lines = ['q0 %s' % q0.hex()]
    for case in load('S-11-packages', 'vectors', 'packages.json')['cases']:
        path = os.path.join('S-11-packages', 'vectors', case['file'])
        with open(os.path.join(SPEC, path), 'rb') as f:
            data = f.read()
        mofs = tfw.PREAMBLE_SIZE + tfw.HEADER_SIZE if case['form'] == 'file' else tfw.HEADER_SIZE
        if len(data) < mofs + tfw.MANIFEST_SIZE:
            continue
        mb = data[mofs:mofs + tfw.MANIFEST_SIZE]
        man = tfw.Manifest.unpack(mb)
        payload = data[mofs + tfw.MANIFEST_SIZE:]
        digest = tfw.signed_digest(mb)
        valid = p224.verify(ks.q0, digest, man.sig_r, man.sig_s)
        index = mb[0x004]
        zip_len = int.from_bytes(mb[0x80:0x84], 'little')
        padded = (zip_len + 15) // 16 * 16
        key, zip_sha = None, None
        if index in keys and len(payload) >= tfw.NAME_BLOCK + padded:
            key = keys[index]
            plain = aes.cbc_decrypt(key, mb[0x70:0x80], payload[:tfw.NAME_BLOCK + padded])
            zip_sha = hashlib.sha256(plain[tfw.NAME_BLOCK:tfw.NAME_BLOCK + zip_len]).digest()
        elif index == 0xFE and len(payload) >= tfw.NAME_BLOCK + zip_len:
            zip_sha = hashlib.sha256(payload[tfw.NAME_BLOCK:tfw.NAME_BLOCK + zip_len]).digest()
        lines.append('pkg %s %s %d %d %s %s %s %s %d %02x %s %s %d %s' % (
            case['case'], path, mofs, len(payload), digest.hex(),
            hashlib.sha256(payload).hexdigest(), man.sig_r.to_bytes(28, 'big').hex(),
            man.sig_s.to_bytes(28, 'big').hex(), 1 if valid else 0, index, hx(key),
            mb[0x70:0x80].hex(), zip_len, hx(zip_sha)))
    return lines


# --- Zip archives (S-11.030) ----------------------------------------------------

def zips():
    lines = []
    for case in load('S-11-packages', 'vectors', 'zips.json')['cases']:
        raw = bytes.fromhex(case['zip_hex'])
        lines.append('zip %s %s' % (case['case'], raw.hex()))
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            for info in z.infolist():
                sha = None
                try:
                    if info.flag_bits & 1:
                        status = 'encrypted'
                    elif info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                        status = 'method'
                    else:
                        sha = hashlib.sha256(z.read(info)).digest()
                        status = 'ok'
                except zipfile.BadZipFile as e:
                    status = 'crc' if 'CRC' in str(e) else 'error'
                except Exception:  # noqa: BLE001 - any failure to inflate
                    status = 'error'
                lines.append('member %s %d %d %s %s' % (
                    info.filename.encode().hex(), info.compress_type, info.flag_bits, status, hx(sha)))
        lines.append('end')
    # Bare deflate streams for inflate itself: raw (as in a zip member,
    # window bits -15) and zlib-wrapped (15), from Python's compressor.
    rnd = bytes((i * 2654435761 >> 13) & 0xFF for i in range(70000))
    samples = [b'', b'a', b'hello, world\n' * 3, bytes(100000), rnd, pattern(200000, 7)]
    for wbits in (-15, 15):
        for level in (0, 1, 9):
            for data in samples:
                c = zlib.compressobj(level, zlib.DEFLATED, wbits)
                comp = c.compress(data) + c.flush()
                lines.append('inflate %d %s %d %s' % (wbits, comp.hex(), len(data),
                                                      hashlib.sha256(data).hexdigest()))
    return lines


# --- FAT (S-12 section 1) --------------------------------------------------------

def tree_lines(tree):
    lines = []
    for path in sorted(tree):
        e = tree[path]
        if e['type'] == 'file':
            lines.append('entry %s file %d %s' % (path.encode().hex(), e['len'], e['sha256']))
        elif e['type'] == 'dir':
            lines.append('entry %s dir' % path.encode().hex())
        elif e['type'] == 'symlink':
            lines.append('entry %s symlink %s' % (path.encode().hex(), e['alias'].encode().hex()))
        elif e['type'] == 'hardlink':
            lines.append('entry %s hardlink %s' % (path.encode().hex(), e['target'].encode().hex()))
        else:
            raise ValueError(e)
    return lines


def fat():
    v = load('S-12-filesystems', 'vectors', 'ramdisk_format.json')
    lines = ['disk %d' % v['disk']['sectors']]
    for n in (0, 63, 175):
        lines.append('sector %d %s' % (n, v['sector_%d' % n]))
    lines.append('head 64 %s' % v['sector_64_head'])
    for a, b in v['written_sectors']:
        lines.append('written %d %d' % (a, b))
    hv = load('S-12-filesystems', 'vectors', 'host_volumes.json')
    for case in hv['read']:
        lines.append('volume %s host_%s.img 1' % (case['kind'], case['kind']))
        lines += tree_lines(case['tree'])
        lines.append('end')
    for case in hv['unreadable']:
        lines += ['volume %s host_%s.img 0' % (case['kind'], case['kind']), 'end']
    pop = load('S-12-filesystems', 'vectors', 'ramdisk_populated.json')
    lines.append('volume populated ramdisk_populated.img 1')
    lines += tree_lines(pop['tree'])
    lines.append('end')
    return lines


# --- yaffs2 (S-12 section 2) -----------------------------------------------------

def yaffs():
    v = load('S-12-filesystems', 'vectors', 'yaffs_trees.json')
    lines = ['blocks %d' % v['params']['n_blocks']]
    for case in v['cases']:
        lines.append('volume %s yaffs_%s.img %d' % (case['name'], case['name'], 1 if case['refused'] else 0))
        lines += tree_lines(case.get('tree') or {})
        for f in case.get('lost_found_files') or []:
            lines.append('lostfound %d %s' % (f['len'], f['sha256']))
        lines.append('end')
    return lines


EXPORTS = {'crc.txt': crcs, 'packages.txt': packages, 'zips.txt': zips, 'fat.txt': fat, 'yaffs.txt': yaffs}


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: export.py OUTDIR')
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, fn in EXPORTS.items():
        with open(os.path.join(out, name), 'w') as f:
            for line in fn():
                f.write(line + '\n')


if __name__ == '__main__':
    main()
