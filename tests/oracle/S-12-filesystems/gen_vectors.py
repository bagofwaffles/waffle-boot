#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-12 vector generator: every vector of `vectors/`, built with tests/oracle/conformance/fs; with
`--images DIR` it also writes the disk and volume images as sparse files.

    python3 -I tests/oracle/S-12-filesystems/gen_vectors.py            # rewrite vectors/*.json
    python3 -I tests/oracle/S-12-filesystems/gen_vectors.py --images DIR

Synthetic content only. `display_name`, `fat_stamp`, `find_fat_volume`, `yaffs_illegal`, `valid_summary`,
`spec_normalize` and `be_evidence` implement S-12.011, S-12.016, S-12.020, S-12.047, S-12.049, S-12.050
rule 2 and S-12.052 independently of the conformance reader. `spec_normalize` resolves each chunk's
effective tags and writes an image without summaries; the reader then applies rules 3-8 to it.
"""
import hashlib
import json
import os
import struct
import sys
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'conformance'))
from fs import fat, fatread, yaffs2, yaffs2read  # noqa: E402
from fs.disk import SparseDisk  # noqa: E402

SECTOR = 512
FIXED_CLOCK = (fat.fat_date(1980, 1, 1), 0)
DISK_SECTORS = 454656
# S-12.008: the OP-1F's disk (device.ramdisk_blocks), clusters of 8 sectors, the label device.product_text
DISK_SECTORS_1F = 260096


def hx(b):
    return bytes(b).hex()


def digest(b):
    return {'len': len(b), 'sha256': hashlib.sha256(bytes(b)).hexdigest()}


def pattern(n, seed=1):
    return bytes((i * 7 + seed) % 251 for i in range(n))


# --------------------------------------------------------------------------------------- the rules

def fat_stamp(year, month, day, hour, minute, second):
    """S-12.016: UTC calendar fields to (date, time); None above 2107."""
    if year > 2107:
        return None
    if year < 1980:
        year = 1980
    return ((year - 1980) << 9) | (month << 5) | day, (hour << 11) | (minute << 5) | (second // 2)


def display_name(entries):
    """S-12.011 over a directory's raw 32-byte entries: the names of its files and directories, in order."""
    out, lfn = [], []
    for e in entries:
        if e[0] == 0x00:
            break
        if e[0] == 0xE5:
            lfn = []
            continue
        attr = e[11]
        if attr & 0x3F == 0x0F:
            lfn.append(e)
            continue
        if attr & 0x08 or e[0:2] in (b'. ', b'..'):
            lfn = []
            continue
        name = None
        if lfn:
            s = fat.sfn_checksum(e[0:11])
            seqs = [x[0] & 0x3F for x in lfn]
            n = len(lfn)
            if lfn[0][0] & 0x40 and seqs == list(range(n, 0, -1)) and all(x[13] == s for x in lfn):
                units = b''
                for x in reversed(lfn):
                    units += b''.join(x[o:o + 2] for o in fat.LFN_OFFSETS)
                text = units.decode('utf-16-le')
                name = text.split('\0')[0]
        lfn = []
        if name is None:
            raw = bytearray(e[0:11])
            if raw[0] == 0x05:
                raw[0] = 0xE5
            base, ext = raw[0:8].rstrip(b' '), raw[8:11].rstrip(b' ')
            b, x = base.decode('cp850'), ext.decode('cp850')
            low = lambda t: ''.join(c.lower() if 'A' <= c <= 'Z' else c for c in t)
            if e[12] & 0x08:
                b = low(b)
            if e[12] & 0x10:
                x = low(x)
            name = b + ('.' + x if x else '')
        out.append(name)
    return out


def is_fat_vbr(s):
    return len(s) == SECTOR and s[510:512] == b'\x55\xAA' and s[0] in (0xEB, 0xE9, 0xE8) and \
        (s[0x36:0x39] == b'FAT' or s[0x52:0x57] == b'FAT32')


def find_fat_volume(disk):
    """S-12.020: the sector where the volume the bootloader reads starts, or None."""
    s0 = disk.read(0, SECTOR)
    if is_fat_vbr(s0):
        return 0
    if s0[510:512] != b'\x55\xAA':
        return None
    for i in range(4):
        e = s0[446 + 16 * i:462 + 16 * i]
        if e[4] == 0:
            continue
        lba = struct.unpack_from('<I', e, 8)[0]
        if lba and lba * SECTOR + SECTOR <= disk.size and is_fat_vbr(disk.read(lba * SECTOR, SECTOR)):
            return lba
    return None


SEQ_DATA_LO, SEQ_DATA_HI = 0x1000, 0xEFFFFEFF        # S-12.047: the data range, both ends included
MAX_OBJ, MAX_CHUNK = 0x3FFFF, 0x0FFFFFFF            # S-12.050 rule 2


def yaffs_illegal(tags, block_seq):
    """S-12.050 rule 2 for one used chunk's tags (seq, object word, chunk word, byte count)."""
    seq, ow, cw, nb = tags
    header = bool(cw & 0x80000000)
    oid = ow & 0x0FFFFFFF if header else ow
    if oid > MAX_OBJ or oid == yaffs2.OBJ_SUMMARY:
        return True
    if not header and (cw > MAX_CHUNK or nb > 4080):
        return True
    return seq != block_seq


# ------------------------------------------------------------------------------------ the RAM disk

def fresh_volume():
    v = fat.FatVolume(fat.ramdisk_params())
    v.add_label('OP-XY')
    return v


def fresh_volume_1f():
    """S-12.008: the rule of S-12.003 to S-12.005 on 260,096 sectors."""
    v = fat.FatVolume(fat.ramdisk_params(disk_sectors=DISK_SECTORS_1F, sectors_per_cluster=8))
    v.add_label('OP-1')
    return v


def ramdisk_format_1f():
    """S-12.008's table, as sectors: the same names as ramdisk_format.json, the label in sector 191."""
    v = fresh_volume_1f()
    d = v.build()
    info = v.info()
    out = ramdisk_format(v)
    root = info['rootbase']
    assert (info['fatbase'], info['fat_sectors'], root, info['database']) == (64, 127, 191, 223), info
    assert info['n_clusters'] == 32484, info
    out.pop('sector_175')
    out['sector_191'] = hx(d.read(root * SECTOR, SECTOR))
    out['requirements'] = ['S-12.008']
    out['disk'] = {'sectors': DISK_SECTORS_1F, 'sector_size': SECTOR, 'bytes': DISK_SECTORS_1F * SECTOR}
    out['written_sectors'] = [[0, 0], [63, 63], [64, 190], [191, 222]]
    s0 = d.read(0, SECTOR)
    assert s0[0x1C3:0x1C6] == bytes([0xFE, 0x3F, 0x10]), s0[0x1C3:0x1C6].hex()
    assert struct.unpack_from('<I', s0, 0x1CA)[0] == 260033
    return out


def ramdisk_format(v=None):
    v = v or fresh_volume()
    d = v.build()
    info = v.info()
    s63 = d.read(63 * SECTOR, SECTOR)
    bpb = dict(zip(('bytes_per_sector', 'sectors_per_cluster', 'reserved', 'n_fats', 'root_entries',
                    'total16', 'media', 'fat_sectors', 'sectors_per_track', 'heads', 'hidden', 'total32'),
                   struct.unpack_from('<HBHBHHBHHHII', s63, 11)))
    bpb.update(jump=hx(s63[0:3]), oem=s63[3:11].decode(), drive=s63[36], ext_sig=s63[38],
               volume_id=struct.unpack_from('<I', s63, 39)[0], label=s63[43:54].decode(),
               fs_type=s63[54:62].decode())
    return {
        'requirements': ['S-12.001', 'S-12.003', 'S-12.004', 'S-12.005', 'S-12.006', 'S-12.007'],
        'disk': {'sectors': DISK_SECTORS, 'sector_size': SECTOR, 'bytes': DISK_SECTORS * SECTOR},
        'sector_0': hx(d.read(0, SECTOR)),
        'sector_63': hx(s63),
        'bpb': bpb,
        'sector_64_head': hx(d.read(64 * SECTOR, 4)),
        'sector_175': hx(d.read(175 * SECTOR, SECTOR)),
        'layout': {k: info[k] for k in ('fat_type', 'n_clusters', 'fat_sectors', 'fatbase', 'rootbase', 'database',
                                        'vol_sectors', 'free')},
        'written_sectors': [[0, 0], [63, 63], [64, 174], [175, 206]],
    }


def raw_dir(entries):
    v = fat.FatVolume(fat.ramdisk_params())
    v.mkdir('d')
    for e in entries:
        v.add_raw_entry('d', e)
    return v


def names():
    cp850 = [{'byte': b, 'char': bytes([b]).decode('cp850')} for b in (0x80, 0x81, 0x9A, 0xE1, 0xE5, 0xA4, 0xFF)]
    lfn_name = 'op-xy firmware.tfw'
    sfn_l = b'OP-XYF~1TFW'
    good = fat.lfn_entries(lfn_name, sfn_l)
    bad = [bytearray(x) for x in fat.lfn_entries('wrong checksum.tfw', b'WRONGC~1TFW')]
    entries = [
        fat.dirent(b'FIRMWARETFW', fat.ATTR_ARC, 0, 0),
        fat.dirent(b'OPXY    TFW', fat.ATTR_ARC, 0, 0, nt=0x10),
        fat.dirent(b'README     ', fat.ATTR_ARC, 0, 0, nt=0x08),
        fat.dirent(b'M\x9aSIK   TXT', fat.ATTR_ARC, 0, 0, nt=0x08),
        fat.dirent(b'\x05BC     TXT', fat.ATTR_ARC, 0, 0),
        *good, fat.dirent(sfn_l, fat.ATTR_ARC, 0, 0),
        *bad, fat.dirent(b'OTHER   TFW', fat.ATTR_ARC, 0, 0),
        b'\xe5' + fat.dirent(b'GONE    TFW', fat.ATTR_ARC, 0, 0)[1:],
        fat.dirent(b'LABEL      ', fat.ATTR_VOL, 0, 0),
        fat.dirent(b'CDC_PL~1   ', fat.ATTR_DIR, 0, 0),
    ]
    display = {'entries': [hx(e) for e in entries], 'names': display_name(entries)}
    create = [
        {'name': 'VERSION.TXT', 'forms': ['sfn']},
        {'name': 'version.txt', 'forms': ['sfn_nt', 'lfn']},
        {'name': 'boot.log', 'forms': ['sfn_nt', 'lfn']},
        {'name': 'logs', 'forms': ['sfn_nt', 'lfn']},
        {'name': 'systeminfo', 'forms': ['lfn']},
        {'name': 'licenses.txt', 'forms': ['sfn_nt', 'lfn']},
        {'name': 'Readme.txt', 'forms': ['lfn']},
        {'name': 'boot 2026-10-26.log', 'forms': ['lfn']},
        {'name': 'müsik.wav', 'forms': ['lfn']},
        {'name': 'a:b.log', 'forms': ['refused']},
        {'name': 'q?.log', 'forms': ['refused']},
        {'name': 'x' * 256, 'forms': ['refused']},
        {'name': 'tab\there', 'forms': ['refused']},
        {'name': 'boot.log.', 'forms': ['refused']},
        {'name': 'trailing ', 'forms': ['refused']},
        {'name': 'dots..', 'forms': ['refused']},
        {'name': '...', 'forms': ['refused']},
    ]
    return {'requirements': ['S-12.010', 'S-12.011', 'S-12.012'], 'cp850': cp850, 'display': display,
            'create': create}


def stamps():
    cases = []
    for t in [(1970, 1, 1, 0, 0, 0), (1970, 6, 15, 12, 34, 57), (1979, 12, 31, 23, 59, 59), (1980, 1, 1, 0, 0, 1),
              (2024, 2, 29, 6, 7, 8), (2026, 10, 26, 13, 45, 31), (2107, 12, 31, 23, 59, 59),
              (2108, 1, 1, 0, 0, 0)]:
        epoch = int(datetime(*t, tzinfo=timezone.utc).timestamp())
        r = fat_stamp(*t)
        cases.append({'utc': '%04d-%02d-%02dT%02d:%02d:%02dZ' % t, 'epoch': epoch,
                      'date': None if r is None else r[0], 'time': None if r is None else r[1]})
    return {'requirements': ['S-12.015', 'S-12.016'], 'fixed_clock': {'date': FIXED_CLOCK[0], 'time': FIXED_CLOCK[1]},
            'cases': cases}


SYSINFO = {
    'os_version': [1, 1, 2, 0], 'sw_version': [1, 1, 2, 4512], 'bl_version': [2, 0, 0, 0],
    'psoc_version': [3, 4, 0, 0], 'nordic_version': [1, 2, 3, 7],
    'unit_serial': 'XY123456', 'dsp_serial': '0123456789ABCDEF', 'psoc_serial': 'PS000001',
    'rtc_utc': [2026, 10, 26, 10, 0, 0],
    'logs': [{'name': 'boot.log', 'mtime_utc': [2026, 10, 25, 9, 8, 7], 'pattern': 3000}],
    'licenses': 'Third-party notices (synthetic).\n',
}


def vtext(v):
    return '%d.%d.%d' % tuple(v[:3]) + ('+%d' % v[3] if v[3] else '')


def sysinfo_files(si):
    version = ('[firmware: v. %s]\nblackfin: v. %s\nbootloader: v. %s\nbackplane: v. %s\nnordic: v. %s\n' % (
        vtext(si['os_version']), vtext(si['sw_version']), vtext(si['bl_version']), vtext(si['psoc_version']),
        vtext(si['nordic_version']))).encode()
    serial = ('unit: %s\ndsp: %s\nbackplane: %s\n' % (si['unit_serial'], si['dsp_serial'],
                                                     si['psoc_serial'])).encode()
    return version, serial


def populated_volume(si=SYSINFO):
    version, serial = sysinfo_files(si)
    v = fresh_volume()
    v.mkdir('systeminfo')
    v.add_file('systeminfo/version.txt', version)
    v.add_file('systeminfo/serial.txt', serial)
    v.add_file('systeminfo/licenses.txt', si['licenses'].encode())
    v.mkdir('logs')
    explicit = {'systeminfo/version.txt': si['rtc_utc'], 'systeminfo/serial.txt': si['rtc_utc'],
                'systeminfo/licenses.txt': si['rtc_utc']}
    for lg in si['logs']:
        v.add_file('logs/' + lg['name'], pattern(lg['pattern']))
        explicit['logs/' + lg['name']] = lg['mtime_utc']
    d = v.build()
    # S-12.015/.016: creation stays the fixed clock, the write stamp becomes the explicit time
    r = fatread.FatReader(d)
    for path, t in explicit.items():
        sect, off = entry_location(r, v, path)
        date, time = fat_stamp(*t)
        s = bytearray(d.read(sect * SECTOR, SECTOR))
        struct.pack_into('<HH', s, off + 22, time, date)
        d.write(sect * SECTOR, bytes(s))
    return v, d


def entry_location(r, v, path):
    """(sector, offset) of the short entry of `path` in the built volume."""
    info = v.info()
    head, _, name = path.rpartition('/')
    dcl = info['entries'][head + '/']['clusters']
    node = v._lookup(path)
    for c in dcl:
        first = r.cluster_sector(c)
        for k in range(16):
            s = r.src.read((first + k) * SECTOR, SECTOR)
            for off in range(0, SECTOR, 32):
                if s[off:off + 11] == node.sfn and s[off + 11] & 0x3F != 0x0F:
                    return first + k, off
    raise KeyError(path)


def populated():
    v, d = populated_volume()
    r = fatread.FatReader(d)
    version, serial = sysinfo_files(SYSINFO)
    ents = []
    for path in ('systeminfo', 'logs', 'systeminfo/version.txt', 'systeminfo/serial.txt',
                 'systeminfo/licenses.txt', 'logs/boot.log'):
        sect, off = entry_location(r, v, path) if '/' in path else root_entry(d, v, path)
        e = d.read(sect * SECTOR + off, 32)
        ents.append({'path': path, 'sfn': e[0:11].decode('cp850'), 'attr': e[11], 'nt': e[12],
                     'crt_tenths': e[13], 'crt_time': struct.unpack_from('<H', e, 14)[0],
                     'crt_date': struct.unpack_from('<H', e, 16)[0], 'acc_date': struct.unpack_from('<H', e, 18)[0],
                     'wrt_time': struct.unpack_from('<H', e, 22)[0], 'wrt_date': struct.unpack_from('<H', e, 24)[0],
                     'size': struct.unpack_from('<I', e, 28)[0]})
    tree = {}
    for path, ent in sorted(r.tree().items()):
        tree[path] = {'type': ent['type']} if ent['type'] == 'dir' else {'type': 'file', **digest(ent['data'])}
    return {
        'requirements': ['S-12.012', 'S-12.015', 'S-12.016', 'S-12.017', 'S-12.019'],
        'inputs': SYSINFO,
        'files': {'systeminfo/version.txt': version.decode(), 'systeminfo/serial.txt': serial.decode()},
        'tree': tree,
        'entries': ents,
        'check': r.check(),
    }


def root_entry(d, v, name):
    node = v._lookup(name)
    for k in range(32):
        s = d.read((175 + k) * SECTOR, SECTOR)
        for off in range(0, SECTOR, 32):
            if s[off:off + 11] == node.sfn and s[off + 11] & 0x3F != 0x0F:
                return 175 + k, off
    raise KeyError(name)


# ------------------------------------------------------------------------------------ host volumes

HOST_TREE = [('mkdir', 'firmware'), ('file', 'op-xy_1.1.2.tfw', 5000), ('file', 'cdc_please', 0),
             ('file', 'firmware/notes.txt', 100)]


def host_volume(case):
    k = case['kind']
    if k == 'fat32_preset':
        p = fat.ramdisk_fat32_params()
    elif k == 'fat12_superfloppy':
        p = fat.FatParams(fat_type=12, disk_sectors=DISK_SECTORS, sectors_per_cluster=128, n_fats=2)
    elif k == 'fat16_two_fats':
        p = fat.FatParams(fat_type=16, disk_sectors=DISK_SECTORS, volume_start=2048, sectors_per_cluster=8,
                          n_fats=2)
    elif k in ('fat32_second_partition', 'exfat', 'gpt'):
        p = fat.FatParams(fat_type=32, disk_sectors=DISK_SECTORS, volume_start=4096, sectors_per_cluster=1,
                          reserved_sectors=32, n_fats=2, root_entries=0)
    elif k in ('fat16_superfloppy', 'bad_jump'):
        p = fat.FatParams(fat_type=16, disk_sectors=DISK_SECTORS, sectors_per_cluster=16, n_fats=2)
    elif k == 'blank':
        return SparseDisk(DISK_SECTORS * SECTOR, SECTOR)
    else:
        raise ValueError(k)
    v = fat.FatVolume(p)
    v.add_label('HOSTVOL')
    for op in HOST_TREE:
        if op[0] == 'mkdir':
            v.mkdir(op[1])
        else:
            v.add_file(op[1], pattern(op[2], 5))
    d = v.build()
    if k == 'fat32_second_partition':
        s0 = bytearray(d.read(0, SECTOR))
        e2 = bytes(s0[446:462])
        e1 = bytearray(16)
        e1[4] = 0x83
        struct.pack_into('<II', e1, 8, 2048, 2048)
        s0[446:462], s0[462:478] = e1, e2
        d.write(0, bytes(s0))
        junk = bytearray(SECTOR)
        junk[0:3], junk[3:11] = b'\xEB\x58\x90', b'NOTFAT  '
        junk[510:512] = b'\x55\xAA'
        d.write(2048 * SECTOR, bytes(junk))
    elif k == 'exfat':
        s = bytearray(SECTOR)
        s[0:11] = b'\xEB\x76\x90EXFAT   '
        s[510:512] = b'\x55\xAA'
        d.write(4096 * SECTOR, bytes(s))
    elif k == 'gpt':
        s0 = bytearray(SECTOR)
        e = bytearray(16)
        e[4] = 0xEE
        struct.pack_into('<II', e, 8, 1, DISK_SECTORS - 1)
        s0[446:462] = e
        s0[510:512] = b'\x55\xAA'
        d.write(0, bytes(s0))
        d.write(SECTOR, b'EFI PART' + bytes(SECTOR - 8))
    elif k == 'bad_jump':
        s = bytearray(d.read(0, SECTOR))
        s[0] = 0x00
        d.write(0, bytes(s))
    return d


HOST_CASES = [
    {'kind': 'fat32_preset', 'read': True, 'note': 'a host reformat as FAT32 (MBR, partition at 63, type 0x0C)'},
    {'kind': 'fat12_superfloppy', 'read': True, 'note': 'FAT12, 128-sector clusters, no partition table'},
    {'kind': 'fat16_two_fats', 'read': True, 'note': 'FAT16, two FATs, partition at 2048'},
    {'kind': 'fat16_superfloppy', 'read': True, 'note': 'FAT16, two FATs, no partition table'},
    {'kind': 'fat32_second_partition', 'read': True,
     'note': 'entry 1 points at a sector that is not a FAT boot sector; entry 2 is the volume'},
    {'kind': 'exfat', 'read': False, 'note': 'the partition holds an exFAT boot sector (need not be read)'},
    {'kind': 'gpt', 'read': False, 'note': 'a protective MBR and a GUID partition table (need not be read)'},
    {'kind': 'blank', 'read': False, 'note': 'all zero: no signature at sector 0'},
    {'kind': 'bad_jump', 'read': False, 'note': 'a FAT16 boot sector at 0 whose first byte is 0x00'},
]


def host_volumes():
    read, unreadable = [], []
    for c in HOST_CASES:
        d = host_volume(c)
        start = find_fat_volume(d)
        row = dict(c, volume_start=start)
        if c['read']:
            r = fatread.FatReader(d, volume_start=start)
            row['fat_type'] = r.fat_type
            row['tree'] = {p: ({'type': 'dir'} if e['type'] == 'dir' else {'type': 'file', **digest(e['data'])})
                           for p, e in sorted(r.tree().items())}
            row['check'] = r.check()
            read.append(row)
        else:
            unreadable.append(row)
    return {'requirements': ['S-12.019', 'S-12.020', 'S-12.021'], 'read': read, 'unreadable': unreadable}


# --------------------------------------------------------------------------------------------- yaffs2

def yp(n=8, **kw):
    return yaffs2.op_xy_params(n_blocks=n, **kw)


def yaffs_layout():
    p = yp()
    tags = [
        {'what': 'data chunk 3 of object 0x101, 100 bytes', 'seq': 0x1001,
         'hex': hx(yaffs2.pack_tags(0x1001, 0x101, 3, 100))},
        {'what': 'header of file 0x101 in root, size 10000', 'seq': 0x1001,
         'hex': hx(yaffs2.pack_tags(0x1001, 0x101 | yaffs2.FILE << 28, 0x80000000 | 1, 10000))},
        {'what': 'shrink header of file 0x101, deleted (parent 4), size 0',
         'hex': hx(yaffs2.pack_tags(0x1002, 0x101 | yaffs2.FILE << 28, 0x80000000 | 0x40000000 | 4, 0))},
        {'what': 'header of directory 0x102 in 0x105, shadowing', 'hex':
         hx(yaffs2.pack_tags(0x1003, 0x102 | yaffs2.DIR << 28, 0x80000000 | 0x20000000 | 0x105, 0))},
        {'what': 'header of hard link 0x103 to 0x101', 'hex':
         hx(yaffs2.pack_tags(0x1003, 0x103 | yaffs2.HARDLINK << 28, 0x80000000 | 1, 0x101))},
    ]
    for t in tags:
        t['decoded'] = {k: v for k, v in yaffs2.unpack_tags(bytes.fromhex(t['hex'])).items()}
    header = {'type': yaffs2.OH_TYPE, 'parent': yaffs2.OH_PARENT, 'name': yaffs2.OH_NAME, 'mode': yaffs2.OH_MODE,
              'uid': yaffs2.OH_UID, 'gid': yaffs2.OH_GID, 'atime': yaffs2.OH_ATIME, 'mtime': yaffs2.OH_MTIME,
              'ctime': yaffs2.OH_CTIME, 'size_low': yaffs2.OH_SIZE_LOW, 'equiv': yaffs2.OH_EQUIV,
              'alias': yaffs2.OH_ALIAS, 'inband_shadowed': yaffs2.OH_INBAND_SHADOWED,
              'inband_is_shrink': yaffs2.OH_INBAND_SHRINK, 'size_high': yaffs2.OH_SIZE_HIGH,
              'shadows': yaffs2.OH_SHADOWS, 'is_shrink': yaffs2.OH_SHRINK, 'record_size': yaffs2.OH_SIZE}
    sample = yaffs2.object_header(p, yaffs2.FILE, 1, 'a.txt', size=10000, times=(1, 2, 3))
    # a block filled by one file: 127 data/header chunks, then the summary
    y = yaffs2.Yaffs2Builder(p)
    y.write_file('big.bin', pattern(126 * p.data_bytes, 9))
    img = y.image()
    c127 = img.read(127 * p.chunk_size, p.chunk_size)
    return {
        'requirements': ['S-12.041', 'S-12.042', 'S-12.044', 'S-12.045', 'S-12.049'],
        'geometry': {'chunk_bytes': 4096, 'chunk_sectors': 8, 'data_bytes': 4080, 'tags_offset': 0xFF0,
                     'chunks_per_block': 128, 'block_sectors': 0x400, 'blocks': 0x1200, 'last_chunk': 0x8FFFF,
                     'last_sector': 0x47FFFF, 'summary_block_number_offset': 1},
        'tags': tags,
        'header': {'offsets': header, 'sample_file_header_first_0x200': hx(sample[:0x200])},
        'summary': {'block': 0, 'seq': 0x1001, 'chunk_127_tags': hx(c127[-16:]), 'chunk_127_head': hx(c127[:16]),
                    'entries_digest': digest(c127[16:1540]),
                    'byte_sum': sum(c127[16:1540]) & 0xFFFFFFFF},
    }


# Each case: ops for the builder, then the expected tree (from the reader). Ops:
#   ['mkdir', path] ['file', path, n, seed] ['symlink', path, alias] ['hardlink', path, target]
#   ['delete', path] ['rename', path, new_path, shadows_id_or_0] ['shrink', path, size]
#   ['header', obj_id, type, parent, name, size] ['new_block', block, seq] ['corrupt_summary', block]
#   ['raw_header_block', block, seq(, name, obj_id)]  (a block with one header chunk of a directory in the root;
#                                                     default 'ghost', 0x1FF)
#   ['data', path, chunk_word, n, seed]  (one data chunk of an existing object, any chunk word)
#   ['summary_loose', block, entry]  (block word 0x7777 and summary-tag sequence 0x1FFF; entry's object word
#                                     0x40000, so rule 2 ignores that chunk; the sum fixed: still valid)
#   ['summary_obj0', block, [entry, ...]]  (each entry's object word 0, its other words left or zeroed as the
#                                           op says; the sum fixed)
# 'expect' holds facts checked without the reader.
YAFFS_CASES = [
    {'name': 'empty', 'ops': [], 'reqs': ['S-12.043', 'S-12.057']},
    {'name': 'basic', 'reqs': ['S-12.044', 'S-12.045', 'S-12.050'],
     'ops': [['mkdir', 'user'], ['mkdir', 'logs'], ['file', 'user/a.txt', 5, 1], ['file', 'logs/boot.log', 10000, 2],
             ['file', 'user/empty', 0, 0], ['symlink', 'user/link', 'a.txt'], ['hardlink', 'user/hl', 'user/a.txt'],
             ['mkdir', 'content'], ['mkdir', 'content/dlc'], ['file', 'bootloader.settings', 0, 0]]},
    {'name': 'deleted', 'reqs': ['S-12.046', 'S-12.050'],
     'ops': [['mkdir', 'user'], ['file', 'user/x', 9000, 3], ['file', 'user/y', 10, 4], ['delete', 'user/x']]},
    {'name': 'shadows', 'reqs': ['S-12.050'],
     'ops': [['file', 'a', 100, 5], ['file', 'b', 200, 6], ['rename_over', 'a', 'b']]},
    {'name': 'shrink', 'reqs': ['S-12.045', 'S-12.050'],
     'ops': [['file', 'f', 10000, 7], ['shrink', 'f', 100]]},
    {'name': 'shrink_extend', 'reqs': ['S-12.050'],
     'ops': [['file', 'f', 10000, 7], ['shrink', 'f', 100], ['resize', 'f', 5000]]},
    {'name': 'orphan', 'reqs': ['S-12.050', 'S-12.053'],
     'ops': [['header', 0x150, yaffs2.FILE, 0x999, 'stray', 0]]},
    {'name': 'summary_bad', 'reqs': ['S-12.049'],
     'ops': [['file', 'big', 126 * 4080, 8], ['file', 'small', 10, 9], ['corrupt_summary', 0]]},
    {'name': 'seq_classes', 'reqs': ['S-12.047', 'S-12.055'],
     'ops': [['file', 'kept', 10, 10], ['raw_header_block', 3, 0x21], ['raw_header_block', 4, 0xFFFF0000],
             ['raw_header_block', 5, 0xF0000000], ['raw_header_block', 6, 0x0FFF]]},
    {'name': 'names', 'reqs': ['S-12.058'],
     'ops': [['file', 'Upper', 1, 1], ['file', 'upper', 2, 2], ['file', 'müsik.wav', 3, 3],
             ['file', 'n' * 255, 4, 4]]},
    {'name': 'big_endian', 'reqs': ['S-12.052'], 'big_endian': True,
     'ops': [['mkdir', 'user', 0x100], ['file', 'user/a.txt', 5000, 11]]},
    {'name': 'big_endian_filled', 'reqs': ['S-12.052'], 'big_endian': True,
     'ops': [['mkdir', 'user'], ['file', 'user/big', 125 * 4080, 14], ['file', 'user/s', 10, 15]]},
    {'name': 'summary_loose', 'reqs': ['S-12.049', 'S-12.050'],
     'ops': [['file', 'big', 126 * 4080, 8], ['file', 'small', 10, 9], ['summary_loose', 0, 5]],
     'expect': {'big': 'pattern(126 * 4080, 8) with bytes 20400..24479 (data chunk 6, chunk 5) zero',
                'small': 'pattern(10, 9)'}},
    {'name': 'summary_obj0', 'reqs': ['S-12.049'],
     'ops': [['file', 'big', 126 * 4080, 8], ['summary_obj0', 0, [[3, 'zero'], [4, 'keep']]]],
     'expect': {'big': 'pattern(126 * 4080, 8), whole'}},
    {'name': 'seq_top', 'reqs': ['S-12.047'],
     'ops': [['file', 'kept', 10, 10], ['raw_header_block', 3, 0xEFFFFF00, 'ghost', 0x1FF],
             ['raw_header_block', 4, 0xEFFFFEFF, 'top', 0x1FE]],
     'expect': {'present': ['kept', 'top'], 'absent': ['ghost']}},
    {'name': 'chunk_id_bound', 'reqs': ['S-12.044', 'S-12.050'],
     'ops': [['file', 'f', 3 * 4080, 13], ['data', 'f', 0x10000002, 4080, 14], ['resize', 'f', 3 * 4080]],
     'expect': {'f': 'pattern(3 * 4080, 13), whole'}},
]


def run_yaffs_ops(ops, p=None):
    p = p or yp()
    y = yaffs2.Yaffs2Builder(p)
    for op in ops:
        k = op[0]
        if k == 'mkdir':
            y.mkdir(op[1], obj_id=op[2] if len(op) > 2 else None)
        elif k == 'file':
            y.write_file(op[1], pattern(op[2], op[3]))
        elif k == 'symlink':
            y.symlink(op[1], op[2])
        elif k == 'hardlink':
            y.hardlink(op[1], op[2])
        elif k == 'delete':
            y.delete(op[1])
        elif k == 'rename_over':
            victim = y._resolve(op[2])
            y.objs.pop(victim)
            y.rewrite(op[1], new_path=op[2], shadows=victim)
        elif k == 'shrink':
            y.rewrite(op[1], size=op[2], shrink=True)
        elif k == 'resize':
            y.rewrite(op[1], size=op[2])
        elif k == 'header':
            y.header(op[1], op[2], op[3], op[4], size=op[5])
        elif k == 'corrupt_summary':
            b = op[1]
            idx = (b * p.chunks_per_block + p.chunks_per_block - 1) * p.chunk_size
            c = bytearray(y.disk.read(idx, p.chunk_size))
            c[12] ^= 0xFF                                    # the byte sum
            y.disk.write(idx, bytes(c))
        elif k == 'raw_header_block':
            b, seq = op[1], op[2]
            name, oid = (op[3], op[4]) if len(op) > 3 else ('ghost', 0x1FF)
            data = yaffs2.object_header(p, yaffs2.DIR, yaffs2.OBJ_ROOT, name)
            tags = yaffs2.pack_tags(seq, oid | yaffs2.DIR << 28, 0x80000000 | yaffs2.OBJ_ROOT, 0)
            y.raw_chunk(b, 0, data, tags)
        elif k == 'data':
            y.data(y._resolve(op[1]), op[2], pattern(op[3], op[4]))
        elif k in ('summary_loose', 'summary_obj0'):
            idx = (op[1] * p.chunks_per_block + p.chunks_per_block - 1) * p.chunk_size
            c = bytearray(y.disk.read(idx, p.chunk_size))
            if k == 'summary_loose':
                struct.pack_into('<I', c, 4, 0x7777)                          # the block word
                struct.pack_into('<I', c, p.chunk_size - 16, 0x1FFF)          # chunk 127's own sequence number
                struct.pack_into('<I', c, 16 + 12 * op[2], 0x40000)          # entry: object id above 0x3FFFF
            else:
                for e, how in op[2]:
                    struct.pack_into('<I', c, 16 + 12 * e, 0)
                    if how == 'zero':
                        struct.pack_into('<II', c, 16 + 12 * e + 4, 0, 0)
                    else:
                        struct.pack_into('<II', c, 16 + 12 * e + 4, 0x1234, 99)
            struct.pack_into('<I', c, 12, sum(c[16:1540]) & 0xFFFFFFFF)
            y.disk.write(idx, bytes(c))
        else:
            raise ValueError(k)
    return y.image()


def byteswap_words(img, p):
    """The image as a big-endian writer would have stored it (conformance `yaffs2.byteswap_image`)."""
    return yaffs2.byteswap_image(img, p)


def lost_found_files(tree):
    """S-12.050 rule 8: the contents of the regular files beneath lost+found, as a sorted multiset."""
    return sorted((digest(e['data']) for path, e in tree.items()
                   if path.startswith('lost+found/') and e['type'] == 'file'), key=lambda d: (d['sha256'], d['len']))


def tree_json(tree):
    """The tree outside lost+found (S-12.050: exact there); lost+found itself is always listed."""
    out = {}
    for path, e in sorted(tree.items()):
        if path.startswith('lost+found/'):
            continue
        if e['type'] == 'file':
            out[path] = {'type': 'file', **digest(e['data'])}
        elif e['type'] == 'symlink':
            out[path] = {'type': 'symlink', 'alias': e['alias']}
        elif e['type'] == 'hardlink':
            out[path] = {'type': 'hardlink', 'target': e['target']}
        else:
            out[path] = {'type': e['type']}
    return out


def u32s(raw, fmt='<IIII'):
    return struct.unpack(fmt, raw)


def valid_summary(img, p, block, seq):
    """S-12.049: the 127 entries (object word, chunk word, byte count) of the block's summary if it is valid,
    else None. Neither the block word nor chunk 127's own sequence number is checked."""
    c = img.read((block * p.chunks_per_block + p.chunks_per_block - 1) * p.chunk_size, p.chunk_size)
    tseq, ow, cw, nb = u32s(c[-16:])
    if tseq == 0 or (ow, cw, nb) != (yaffs2.OBJ_SUMMARY, 1, 1540):
        return None
    ver, _blk, sseq, ssum = u32s(c[:16])
    if ver != 1 or sseq != seq or ssum != sum(c[16:1540]) & 0xFFFFFFFF:
        return None
    return [struct.unpack_from('<III', c, 16 + 12 * k) for k in range(p.chunks_per_block - 1)]


def spec_chunks(img, p):
    """Every used chunk of a data block that rule 2 keeps: (chunk index, data area, effective tags), its tags
    taken as S-12.049 says."""
    n_blocks = img.size // (p.chunk_size * p.chunks_per_block)
    for b in range(n_blocks):
        base = b * p.chunks_per_block
        seq = u32s(img.read(base * p.chunk_size + p.chunk_size - 16, 16))[0]
        if not SEQ_DATA_LO <= seq <= SEQ_DATA_HI:
            continue                                         # S-12.047: empty, or holds nothing
        summ = valid_summary(img, p, b, seq)
        for page in range(p.chunks_per_block):
            if summ is not None and page == p.chunks_per_block - 1:
                continue                                     # the summary belongs to no object
            raw = img.read((base + page) * p.chunk_size, p.chunk_size)
            own = u32s(raw[-16:])
            if summ is not None and summ[page][0] != 0:
                tags = (seq,) + summ[page]
            else:
                tags = own
            if tags[0] == 0 or yaffs_illegal(tags, seq):
                continue
            yield base + page, raw[:-16], tags


def spec_normalize(img, p):
    """An image whose chunks are those of spec_chunks with their effective tags, every other chunk of a data
    block replaced by a filler the conformance reader skips (tags: block seq, object 0x10, chunk 1), and no
    summary left."""
    out = SparseDisk(img.size, p.chunk_size, 0)
    kept = {idx: (data, tags) for idx, data, tags in spec_chunks(img, p)}
    n_blocks = img.size // (p.chunk_size * p.chunks_per_block)
    for b in range(n_blocks):
        base = b * p.chunks_per_block
        seq = u32s(img.read(base * p.chunk_size + p.chunk_size - 16, 16))[0]
        if not SEQ_DATA_LO <= seq <= SEQ_DATA_HI:
            continue
        for page in range(p.chunks_per_block):
            idx = base + page
            if idx in kept:
                data, tags = kept[idx]
                out.write(idx * p.chunk_size, bytes(data) + struct.pack('<IIII', *tags))
            elif u32s(img.read(idx * p.chunk_size + p.chunk_size - 16, 16))[0] != 0 or page == 0:
                out.write(idx * p.chunk_size, bytes(p.data_bytes) + yaffs2.pack_tags(seq, yaffs2.OBJ_SUMMARY, 1, 0))
    return out


def consistent_header(tags, data, fmt):
    """S-12.052: tags and header words decoded with `fmt` ('<' or '>') form a consistent header."""
    seq, ow, cw, nb = tags
    if seq == 0 or not cw & 0x80000000 or not 1 <= ow >> 28 <= 5:
        return False
    rtype, rparent = struct.unpack_from(fmt + 'II', data, 0)
    return rtype == ow >> 28 and rparent == cw & 0x0FFFFFFF


def be_evidence(img, p):
    """S-12.052: (consistent little-endian headers kept by S-12.049/S-12.050 rule 2,
    used chunks that are consistent headers read big-endian with legal sequence, object and parent ids)."""
    le = sum(1 for _, data, tags in spec_chunks(img, p) if consistent_header(tags, data, '<'))
    sw = 0
    for u in sorted(img.written_units()):
        raw = img.read(u * p.chunk_size, p.chunk_size)
        if u32s(raw[-16:])[0] == 0:
            continue                                         # unused
        t = u32s(raw[-16:], '>IIII')
        oid, parent = t[1] & 0x0FFFFFFF, t[2] & 0x0FFFFFFF
        if consistent_header(t, raw, '>') and SEQ_DATA_LO <= t[0] <= SEQ_DATA_HI and \
                1 <= oid <= MAX_OBJ and oid != yaffs2.OBJ_SUMMARY and 1 <= parent <= MAX_OBJ:
            sw += 1
    return le, sw


def yaffs_image(case):
    p = yp()
    img = run_yaffs_ops(case['ops'], p)
    if case.get('big_endian'):
        img = byteswap_words(img, p)
    return img, p


def expected_tree(img, p):
    """S-12.050's tree of an image: spec_normalize, then the conformance reader (rules 3-8)."""
    t = yaffs2read.Yaffs2Reader(spec_normalize(img, p), yp(p.n_blocks)).tree()
    return tree_json(t), lost_found_files(t)


def yaffs_trees():
    rows = []
    for c in YAFFS_CASES:
        img, p = yaffs_image(c)
        row = {'name': c['name'], 'requirements': c['reqs'], 'ops': c['ops']}
        le, sw = be_evidence(img, p)
        row['refused'] = le == 0 and sw > 0
        if c.get('big_endian'):
            row['big_endian'] = True
            row['le_consistent_headers'], row['swapped_consistent_headers'] = le, sw
            row['rule2_kept_chunks'] = sum(1 for _ in spec_chunks(img, p))
            row['note'] = ('refused by S-12.052: not mounted, so there is no tree; rule2_kept_chunks counts the '
                           'swapped chunks that rule 2 would still keep')
        else:
            row['tree'], row['lost_found_files'] = expected_tree(img, p)
            r = yaffs2read.Yaffs2Reader(img, p)
            row['check'] = r.check()
            row['blocks'] = {str(b): {'state': i['state'], 'seq': i['seq']} for b, i in sorted(r.blocks.items())
                             if i['state'] != 'empty'}
        if 'expect' in c:
            row['expect'] = c['expect']
        rows.append(row)
    ff = run_yaffs_ops(YAFFS_CASES[1]['ops'], yp(erased_byte=0xFF))
    le, sw = be_evidence(ff, yp())
    rows.append({'name': 'erased_ff', 'requirements': ['S-12.043', 'S-12.047', 'S-12.052'],
                 'ops': YAFFS_CASES[1]['ops'], 'erased_byte': 0xFF, 'refused': le == 0 and sw > 0,
                 'note': 'the basic case written on flash that erases to 0xFF: unused blocks read as sequence '
                         '0xFFFFFFFF (hold nothing) and unused chunks of written blocks as chunks of the wrong '
                         'sequence (ignored), so the tree is the basic tree and nothing is taken as erased',
                 'tree': next(r for r in rows if r['name'] == 'basic')['tree'],
                 'lost_found_files': []})
    return {'requirements': ['S-12.040', 'S-12.043', 'S-12.044', 'S-12.046', 'S-12.047', 'S-12.049', 'S-12.050',
                             'S-12.052', 'S-12.053', 'S-12.055', 'S-12.057', 'S-12.058'],
            'params': {'n_blocks': 8, 'first_seq': 0x1001, 'first_obj_id': 0x101},
            'cases': rows}


def trust_image(p):
    y = yaffs2.Yaffs2Builder(p)
    y.write_file('real', pattern(2 * p.data_bytes, 12))        # chunks 0, 1 data; 2 header
    for k in range(3, 127):
        y.write_file('f%03d' % k, b'')                          # fill the block up to its summary
    img = y.image()
    idx = (p.chunks_per_block - 1) * p.chunk_size
    s = bytearray(img.read(idx, p.chunk_size))
    ent = 16 + 12 * 1
    struct.pack_into('<III', s, ent, 0x300 | yaffs2.FILE << 28, 0x80000000 | yaffs2.OBJ_ROOT, 77)
    struct.pack_into('<I', s, 12, sum(s[16:1540]) & 0xFFFFFFFF)
    img.write(idx, bytes(s))
    return img, s, ent


def yaffs_trust():
    """S-12.051: a block whose summary says chunk 1 is the header of file 0x300 (in the root,
    size 77), while chunk 1 actually holds a data chunk of file 0x101."""
    p = yp()
    img, s, ent = trust_image(p)
    r = yaffs2read.Yaffs2Reader(img, p)
    return {
        'requirements': ['S-12.051'],
        'build': 'file "real" (2 data chunks, then its header), 124 empty files, summary; then summary entry 1 '
                 'rewritten to describe a header of file 0x300 in the root with size 77, and the sum fixed',
        'summary_entry_1': hx(s[ent:ent + 12]),
        'chunk_1_tags': hx(img.read(1 * p.chunk_size + p.chunk_size - 16, 16)),
        'expect': {
            'mounts': True,
            'object_0x300': {'present': True, 'parent': 1, 'type': 'file', 'size': 77,
                             'name': 'not taken from chunk 1; listed under another name or not listed'},
            'chunk_1_name_bytes': hx(img.read(1 * p.chunk_size + yaffs2.OH_NAME, 32)),
            'others_unchanged': ['f%03d' % k for k in range(3, 127)],
            'real': 'present; its chunk 1 is not seen as its data (the scan took the summary\'s entry), so '
                    'bytes 4080..8159 read as 0x00',
        },
        'conformance_check': r.check(),
    }


VECTORS = {
    'ramdisk_format': ramdisk_format, 'ramdisk_format_1f': ramdisk_format_1f, 'names': names, 'stamps': stamps,
    'ramdisk_populated': populated,
    'host_volumes': host_volumes, 'yaffs_layout': yaffs_layout, 'yaffs_trees': yaffs_trees,
    'yaffs_trust': yaffs_trust,
}


def write_images(out):
    os.makedirs(out, exist_ok=True)
    fresh_volume().build().save(os.path.join(out, 'ramdisk_fresh.img'))
    fresh_volume_1f().build().save(os.path.join(out, 'ramdisk_fresh_1f.img'))
    populated_volume()[1].save(os.path.join(out, 'ramdisk_populated.img'))
    for c in HOST_CASES:
        host_volume(c).save(os.path.join(out, 'host_%s.img' % c['kind']))
    for c in YAFFS_CASES:
        yaffs_image(c)[0].save(os.path.join(out, 'yaffs_%s.img' % c['name']))
    erased = run_yaffs_ops(YAFFS_CASES[1]['ops'], yp(erased_byte=0xFF))
    erased.save(os.path.join(out, 'yaffs_erased_ff.img'))
    trust_image(yp())[0].save(os.path.join(out, 'yaffs_trust.img'))


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--images':
        write_images(sys.argv[2])
        return
    vdir = os.path.join(HERE, 'vectors')
    os.makedirs(vdir, exist_ok=True)
    for name, fn in VECTORS.items():
        with open(os.path.join(vdir, name + '.json'), 'w') as f:
            json.dump(fn(), f, indent=1, ensure_ascii=False)
            f.write('\n')


if __name__ == '__main__':
    main()
