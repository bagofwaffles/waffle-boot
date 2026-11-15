#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Self-test of the fs package: every builder against its reader and checker.

    python3 -I tests/oracle/conformance/fs/selftest.py

Prints one line per case and a total; exit status 0 when every case passes.
"""
import os
import struct
import sys

if __package__ in (None, ''):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = 'fs'

from . import disk, fat, fatread, yaffs2, yaffs2read  # noqa: E402

RESULTS = []


def case(name):
    def wrap(fn):
        try:
            fn()
            RESULTS.append((name, True, ''))
        except Exception as e:  # noqa: BLE001 - a failed case is reported, not raised
            RESULTS.append((name, False, f'{type(e).__name__}: {e}'))
        return fn
    return wrap


def eq(a, b, what=''):
    if a != b:
        raise AssertionError(f'{what}: {a!r} != {b!r}'[:300])


def pattern(n, k=1):
    return bytes(((i * 7 + k * 13) ^ (i >> 8)) & 0xFF for i in range(n))


# ------------------------------------------------------------------------------------------------- FAT
def fat_sample(v):
    """A tree with short, long, lowercase, empty, nested and multi-cluster entries; returns the tree a
    reader must give back."""
    v.add_label('SELFTEST')
    files = {
        'README.TXT': b'hello\n',
        'notes.txt': b'lower case 8.3 name: SFN with NT case bits\n',
        'A long file name.data': pattern(5000, 1),
        'EMPTY': b'',
        'DIR1/INNER.BIN': pattern(1500, 2),
        'DIR1/deeper/x.y': pattern(33, 3),
        'Mixed Case Dir/file': b'z' * 512,
    }
    v.mkdir('DIR1')
    v.mkdir('DIR1/deeper')
    v.mkdir('Mixed Case Dir')
    for k, d in files.items():
        v.add_file(k, d)
    v.add_deleted('GONE.TXT')
    # enough entries to make DIR1 outgrow one cluster
    for i in range(40):
        files[f'DIR1/many_entries_file_{i:02d}.txt'] = bytes([i]) * i
        v.add_file(f'DIR1/many_entries_file_{i:02d}.txt', bytes([i]) * i)
    t = {k: {'type': 'file', 'data': d} for k, d in files.items()}
    for d in ('DIR1', 'DIR1/deeper', 'Mixed Case Dir'):
        t[d] = {'type': 'dir'}
    return t


def fat_roundtrip(params, expect_type):
    v = fat.FatVolume(params)
    want = fat_sample(v)
    img = v.build()
    r = fatread.FatReader(img)
    eq(r.fat_type, expect_type, 'fat type')
    eq(r.check(), [], 'check')
    eq(r.tree(), want, 'tree')
    eq(r.label, 'SELFTEST', 'label')
    eq(r.free_count(), v.free_clusters(), 'free count')
    return img, r


for ft, n in ((12, 2000), (16, 5000), (32, 66000)):
    @case(f'fat{ft} build/read/check, bare volume')
    def _(ft=ft, n=n):
        spc = 1 if ft == 32 else 4
        fat_roundtrip(fat.FatParams(fat_type=ft, n_clusters=n, sectors_per_cluster=spc,
                                    reserved_sectors=32 if ft == 32 else 1, root_entries=0 if ft == 32 else 512),
                      ft)


@case('ramdisk preset: FAT16 shape, MBR, one FAT')
def _():
    img, r = fat_roundtrip(fat.ramdisk_params(), 16)
    eq(img.size, 454656 * 512, 'disk size')
    mbr = img.read(0, 512)
    eq(mbr[510:], b'\x55\xaa', 'mbr signature')
    eq(mbr[0x1C2], 0x06, 'partition type')
    eq(struct.unpack_from('<II', mbr, 0x1C6), (63, 454656 - 63), 'partition extent')
    bpb = img.read(63 * 512, 512)
    eq(bpb[:3], b'\xEB\xFE\x90', 'jump')
    eq(struct.unpack_from('<HBHBHHBHHHII', bpb, 11),
       (512, 16, 1, 1, 512, 0, 0xF8, 111, 63, 255, 63, 454656 - 63), 'bpb')
    eq((r.spc, r.n_fats), (16, 1), 'reader geometry')


@case('ramdisk_fat32 preset')
def _():
    img, r = fat_roundtrip(fat.ramdisk_fat32_params(), 32)
    eq(img.read(0x1C2, 1), b'\x0C', 'partition type')
    eq(img.read(63 * 512, 512), img.read((63 + 6) * 512, 512), 'backup boot sector')


@case('fat names: 8.3, NT case bits, LFN, numeric tails')
def _():
    eq(fat.short_name('README.TXT', True)[0], b'README  TXT', 'plain')
    eq(fat.short_name('notes.txt', True), (b'NOTES   TXT', 0x18), 'lowercase')
    eq(fat.short_name('Long Name.txt', True), None, 'needs an LFN')
    v = fat.FatVolume(fat.FatParams(fat_type=16, n_clusters=5000, sectors_per_cluster=4))
    v.add_file('Long Name One.txt', b'1')
    v.add_file('Long Name Two.txt', b'2')
    r = fatread.FatReader(v.build())
    sfns = sorted(e['sfn'] for e in r.listdir(0))
    eq(sfns, [b'LONGNA~1TXT', b'LONGNA~2TXT'], 'tails')
    eq(sorted(e['name'] for e in r.listdir(0)), ['Long Name One.txt', 'Long Name Two.txt'], 'lfn names')


@case('fat explicit (fragmented) cluster chains')
def _():
    v = fat.FatVolume(fat.FatParams(fat_type=16, n_clusters=5000, sectors_per_cluster=1))
    v.add_file('FRAG.BIN', pattern(4 * 512, 9), clusters=[10, 3, 40, 41])
    r = fatread.FatReader(v.build())
    eq(r.chain(10), [10, 3, 40, 41], 'chain')
    eq(r.tree()['FRAG.BIN']['data'], pattern(4 * 512, 9), 'data')
    eq(r.check(), [], 'check')


def corrupted(build, mutate):
    img = bytearray(build().to_bytes())
    mutate(img)
    return fatread.FatReader(bytes(img)).check()


def fat16_small():
    v = fat.FatVolume(fat.FatParams(fat_type=16, n_clusters=5000, sectors_per_cluster=1, n_fats=2))
    v.add_file('A.BIN', pattern(3 * 512))
    return v.build()


def fat32_small():
    v = fat.FatVolume(fat.FatParams(fat_type=32, n_clusters=66000, sectors_per_cluster=1,
                                    reserved_sectors=32))
    v.add_file('A.BIN', pattern(3 * 512))
    return v.build()


@case('fat checker: broken chain, FAT copies, cross-link, FSInfo, backup boot')
def _():
    base = fat16_small()
    r = fatread.FatReader(base)
    fat0 = r.fatbase * 512
    a = r.listdir(0)[0]['cluster']

    def cut(img):           # first cluster of A.BIN marked free in both FATs: chain shorter than size
        for f in range(2):
            struct.pack_into('<H', img, fat0 + f * r.fsz * 512 + a * 2, 0)
    eq(bool(corrupted(fat16_small, cut)), True, 'broken chain')

    def copy1(img):
        struct.pack_into('<H', img, fat0 + r.fsz * 512 + 100 * 2, 0xFFFF)
    eq(any('FAT' in s for s in corrupted(fat16_small, copy1)), True, 'fat copies differ')

    def xlink(img):         # a second file entry pointing at A.BIN's chain
        root = r.rootbase * 512
        e = bytearray(img[root:root + 32])
        e[0:11] = b'B       BIN'
        img[root + 32:root + 64] = e
    eq(any('cross' in s.lower() for s in corrupted(fat16_small, xlink)), True, 'cross-link')

    r32 = fatread.FatReader(fat32_small())

    def fsinfo(img):
        struct.pack_into('<I', img, r32.fsinfo_sector * 512 + 488, 12345)
    eq(any('free' in s.lower() for s in corrupted(fat32_small, fsinfo)), True, 'fsinfo free count')

    def backup(img):
        img[6 * 512 + 3] ^= 0xFF
    eq(any('backup' in s.lower() for s in corrupted(fat32_small, backup)), True, 'backup boot')


@case('fat type thresholds: microsoft vs fatfs at 4085 clusters')
def _():
    v = fat.FatVolume(fat.FatParams(fat_type=16, n_clusters=4085, sectors_per_cluster=1))
    img = v.build()
    eq(fatread.FatReader(img).fat_type, 16, 'microsoft rule')
    eq(fatread.FatReader(img, type_rule='fatfs').fat_type, 12, 'fatfs rule')
    eq(any('thresholds' in s for s in fatread.FatReader(img).check()), True, 'boundary warning')


@case('disk: diff_units and assert_unmodified')
def _():
    a = fat16_small()
    b = bytearray(a.to_bytes())
    eq(disk.diff_units(a, bytes(b)), (0, []), 'equal')
    disk.assert_unmodified(a, bytes(b))
    b[512 * 7 + 3] ^= 1
    b[512 * 9] ^= 1
    eq(disk.diff_units(a, bytes(b)), (2, [7, 9]), 'two sectors')
    try:
        disk.assert_unmodified(a, bytes(b))
    except AssertionError:
        pass
    else:
        raise AssertionError('modification not detected')


# ----------------------------------------------------------------------------------------------- yaffs2
def yp(n=16, **kw):
    return yaffs2.op_xy_params(n_blocks=n, **kw)


def yread(y, p=None):
    r = yaffs2read.Yaffs2Reader(y.image(), p or y.p)
    return r, r.tree(), r.check()


@case('yaffs2 params: OP-XY geometry')
def _():
    p = yp()
    eq((p.chunk_size, p.data_bytes, p.chunks_per_block, p.erased_byte, p.block_offset),
       (4096, 4080, 128, 0, 1), 'geometry')
    eq(yaffs2.unpack_tags(bytes(16)), None, 'erased tags are unused')
    t = yaffs2.unpack_tags(yaffs2.pack_tags(0x1001, 0x101, 1, 7))
    eq((t['seq'], t['obj_id'], t['chunk_id'], t['n_bytes'], t['is_header']), (0x1001, 0x101, 1, 7, False), 'tags')


@case('yaffs2 tree round trip: dirs, files, symlink, hardlink')
def _():
    y = yaffs2.Yaffs2Builder(yp())
    y.mkdir('a')
    y.mkdir('a/b')
    y.write_file('a/one', pattern(10000, 1))
    y.write_file('a/b/two', pattern(4080, 2))
    y.write_file('zero', b'')
    y.symlink('ln', '/yaffs2/a/one')
    y.hardlink('a/hl', 'a/one')
    r, t, c = yread(y)
    eq(c, [], 'check')
    eq(t, {'lost+found': {'type': 'dir'}, 'a': {'type': 'dir'}, 'a/b': {'type': 'dir'},
           'a/one': {'type': 'file', 'data': pattern(10000, 1)}, 'a/b/two': {'type': 'file', 'data': pattern(4080, 2)},
           'zero': {'type': 'file', 'data': b''}, 'ln': {'type': 'symlink', 'alias': '/yaffs2/a/one'},
           'a/hl': {'type': 'hardlink', 'target': 'a/one'}}, 'tree')
    # the reader sees the volume the same with or without the trailing erased blocks
    eq(yaffs2read.Yaffs2Reader(y.image().to_bytes()[:3 * 128 * 4096], y.p, n_blocks=3).tree(), t, 'truncated')


@case('yaffs2 rename, overwrite, delete, shrink, shadows')
def _():
    y = yaffs2.Yaffs2Builder(yp())
    y.mkdir('d')
    y.write_file('d/f', pattern(9000, 1))
    y.write_file('d/g', b'gone soon')
    y.write_file('d/old', b'old cfg')
    y.new_block()
    y.rewrite('d/f', {1: pattern(4080, 5)}, new_path='f2')                  # move + overwrite chunk 1
    y.rewrite('f2', size=5000, shrink=True)                                  # truncate
    y.delete('d/g')
    new = y.write_file('d/new', b'new cfg')
    y.rewrite('d/new', new_path='d/old', shadows=y._resolve('d/old'))       # rename over: shadows
    r, t, c = yread(y)
    eq(c, [], 'check')
    want_f2 = (pattern(4080, 5) + pattern(9000, 1)[4080:])[:5000]
    eq(t.get('f2'), {'type': 'file', 'data': want_f2}, 'rename+overwrite+shrink')
    eq('d/f' in t or 'd/g' in t or 'd/new' in t, False, 'gone names')
    eq(t.get('d/old'), {'type': 'file', 'data': b'new cfg'}, 'shadowing rename')
    eq(new in r.live, True, 'new object live')


@case('yaffs2 full block: summary written and used; bad summary falls back')
def _():
    y = yaffs2.Yaffs2Builder(yp())
    big = pattern(126 * 4080, 4)
    y.write_file('big', big)                     # 126 data + 1 header = 127 chunks, then the summary
    p = y.p
    s = y.image().read((128 - 1) * 4096, 4096)
    eq(struct.unpack_from('<III', s, 0), (1, 0 + p.block_offset, p.first_seq), 'summary header')
    tags = s[p.data_bytes:]
    eq(yaffs2.unpack_tags(tags)['obj_id'], yaffs2.OBJ_SUMMARY, 'summary tags obj id')
    r, t, c = yread(y)
    eq((c, t['big']['data'] == big, r.blocks[0].get('summary')), ([], True, True), 'summary used')
    raw = bytearray(y.image().to_bytes())
    raw[127 * 4096 + 16 + 3] ^= 0x40               # corrupt an entry: checksum fails
    r2 = yaffs2read.Yaffs2Reader(bytes(raw), p)
    eq(r2.tree(), t, 'fallback tree')
    eq(r2.blocks[0].get('summary'), False, 'fallback used')
    eq(any('summary rejected' in s for s in r2.check()), True, 'reported')


@case('yaffs2 skipped blocks: checkpoint, bad, out-of-range seq')
def _():
    y = yaffs2.Yaffs2Builder(yp())
    y.write_file('keep', b'kept')
    y.raw_chunk(5, 0, b'checkpoint bytes', yaffs2.pack_tags(0x21, 0x101, 1, 16))
    y.raw_chunk(6, 0, b'', yaffs2.pack_tags(0xFFFF0000, 0, 0, 0))
    y.raw_chunk(7, 0, b'junk', yaffs2.pack_tags(5, 0x101, 1, 4))
    r, t, c = yread(y)
    eq(t, {'lost+found': {'type': 'dir'}, 'keep': {'type': 'file', 'data': b'kept'}}, 'tree')
    eq({b: r.blocks[b]['state'] for b in (5, 6, 7)}, {5: 'skipped', 6: 'skipped', 7: 'skipped'}, 'states')
    eq(c, [], 'check')


@case('yaffs2 checker: header/tags mismatch, gap, duplicate seq, dangling hardlink')
def _():
    def fresh():
        y = yaffs2.Yaffs2Builder(yp())
        y.mkdir('d')
        y.write_file('d/f', b'x' * 10)
        return y
    y = fresh()
    raw = bytearray(y.image().to_bytes())
    struct.pack_into('<I', raw, 0, yaffs2.FILE)        # header 0 says FILE, its tags say DIR
    c = yaffs2read.Yaffs2Reader(bytes(raw), y.p).check()
    eq(any('header' in s for s in c), True, f'type mismatch {c}')
    y = fresh()
    y.raw_chunk(0, 5, b'late', yaffs2.pack_tags(y.p.first_seq, 0x150, 1, 4))   # after unused pages 3, 4
    eq(any('unused' in s for s in yread(y)[2]), True, 'gap')
    y = fresh()
    y.raw_chunk(3, 0, b'dup', yaffs2.pack_tags(y.p.first_seq, 0x151, 1, 3))
    eq(any('sequence' in s for s in yread(y)[2]), True, 'duplicate seq')
    y = fresh()
    y.header(0x160, yaffs2.HARDLINK, 1, 'dangling', equiv=0x999)
    eq(any('hard link' in s for s in yread(y)[2]), True, 'dangling hardlink')


@case('fat names: code page 850 short names, refused names')
def _():
    v = fat.FatVolume(fat.FatParams(fat_type=16, n_clusters=5000))
    v.mkdir('D')
    v.add_raw_entry('D', fat.dirent(b'M\x9aSIK   TXT', fat.ATTR_ARC, 0, 0))      # 0x9A = U+00DC in cp850
    v.add_raw_entry('D', fat.dirent(b'\x05BC     TXT', fat.ATTR_ARC, 0, 0))      # 0x05 stands for 0xE5
    v.add_raw_entry('D', bytes(b'M\x9aSIK   TXT') + bytes([fat.ATTR_ARC, 0x08]) + bytes(20))
    r = fatread.FatReader(v.build())
    d = [e for e in r.listdir(0) if e['name'] == 'D'][0]
    names = [e['name'] for e in r.listdir(d['cluster']) if not e['dot']]
    eq(names, ['MÜSIK.TXT', 'ÕBC.TXT', 'mÜsik.TXT'], 'cp850 + NT case bits on A-Z only')
    for n in ('x.', 'x ', 'a:b', 'n' * 256, ''):
        eq(fat.name_refused(n), True, repr(n))
        if not n:
            continue
        try:
            fat.FatVolume(fat.ramdisk_params()).add_file(n, b'')
            raise AssertionError(f'{n!r} created')
        except ValueError:
            pass
    eq(fat.name_refused('op-xy firmware.tfw'), False, 'ordinary long name')


@case('yaffs2 scan: rule 2, sequence range, summary validity and object-word-0 entries')
def _():
    y = yaffs2.Yaffs2Builder(yp(4))
    y.write_file('f', pattern(3 * 4080, 3))
    fid = y._resolve('f')
    y.data(fid, 0x10000002, b'X' * 4080)            # chunk word with bit 28 set: ignored (rule 2)
    y.data(0x40000, 1, b'big id')                   # object id above 0x3FFFF: ignored
    y._put(b'Y' * 16, yaffs2.pack_tags(y.seq_for_put(), fid, 3, 4081))   # byte count above 4080: ignored
    _, t, _ = yread(y)
    eq(t['f']['data'], pattern(3 * 4080, 3), 'rule 2')
    y = yaffs2.Yaffs2Builder(yp(4))
    y.write_file('kept', b'k')
    y.new_block(seq=0xEFFFFF00)
    y.write_file('top', b't')
    r, t, _ = yread(y)
    eq(set(t), {'lost+found', 'kept'}, '0xEFFFFF00 holds nothing')
    eq(r.blocks[1]['state'], 'skipped', 'block state')
    y = yaffs2.Yaffs2Builder(yp(4))
    y.write_file('big', pattern(126 * 4080, 4))      # 126 data chunks, header in block 1
    img = y.image()
    p = y.p
    yaffs2.set_summary_entry(img, p, 0, 2, 0, 0, 0)      # object word 0: chunk 2's own tags apply
    ow, cw, nb = yaffs2.chunk_tags(img, p, 3)[1:]
    yaffs2.set_summary_entry(img, p, 0, 3, 0x40000, cw, nb)   # entry makes chunk 3 ignored
    s = bytearray(img.read(127 * 4096, 4096))
    struct.pack_into('<I', s, 4, 0x7777)            # block word: not part of validity
    img.write(127 * 4096, bytes(s))
    r = yaffs2read.Yaffs2Reader(img, p)
    want = pattern(126 * 4080, 4)
    want = want[:3 * 4080] + bytes(4080) + want[4 * 4080:]
    eq(r.blocks[0]['summary'], True, 'summary valid')
    eq(r.tree()['big']['data'], want, 'summary used: chunk 3 a hole, chunk 2 whole')


@case('yaffs2 trusted headers: corrupt type, parent, summary-described header')
def _():
    def build():
        y = yaffs2.Yaffs2Builder(yp(4))
        y.mkdir('d')
        y.write_file('d/f', b'hello')
        return y, y.image(), y._resolve('d/f')

    y, img, fid = build()
    hdr = max(i for i in img.written_units() if yaffs2.chunk_tags(img, y.p, i)[1] & 0x0FFFFFFF == fid)
    yaffs2.corrupt_header(img, y.p, hdr, otype=yaffs2.DIR, name='evil')
    r = yaffs2read.Yaffs2Reader(img, y.p)
    eq(r.tree()['d/obj%d' % fid], {'type': 'file', 'data': b'hello'}, 'untrusted type: tags size, no name')
    eq(r.attributes(fid), None, 'attributes not loaded')
    y, img, fid = build()
    yaffs2.corrupt_header(img, y.p, hdr, parent=0x777, name='evil')
    r = yaffs2read.Yaffs2Reader(img, y.p)
    eq('d/obj%d' % fid in r.tree(), True, 'parent mismatch: no name')
    eq(r.attributes(fid)['mode'], y.p.file_mode, 'parent mismatch: attributes kept')
    y, img, fid = build()
    yaffs2.set_chunk_tags(img, y.p, hdr, obj_word=0x333 | yaffs2.FILE << 28)
    r = yaffs2read.Yaffs2Reader(img, y.p)           # the chunk is now object 0x333's header (no data)
    eq((r.tree()['d/f'], fid in r.live, r.live[0x333]['trusted']), ({'type': 'file', 'data': bytes(5)}, False, True),
       'retagged: another object')


@case('yaffs2 big-endian volume refused; little-endian ones are not')
def _():
    y = yaffs2.Yaffs2Builder(yp(4))
    y.mkdir('user')
    y.write_file('user/a', b'abc')
    be = y.image(big_endian=True)
    before = be.to_bytes()
    r = yaffs2read.Yaffs2Reader(be, y.p)
    eq((r.refused, r.tree()), (True, None), 'refused')
    disk.assert_unmodified(be, before, 4096)
    eq(yaffs2read.Yaffs2Reader(y.image(), y.p).refused, False, 'little-endian')
    eq(yaffs2read.Yaffs2Reader(bytes(4 * 128 * 4096), y.p).refused, False, 'blank')


@case('yaffs2 unmodified check over a read')
def _():
    y = yaffs2.Yaffs2Builder(yp(4))
    y.write_file('f', b'abc')
    before = y.image().to_bytes()
    yaffs2read.Yaffs2Reader(before, y.p).tree()
    disk.assert_unmodified(y.image(), before, 4096)


def main():
    for name, ok, why in RESULTS:
        print(f'{"PASS" if ok else "FAIL"} {name}' + (f': {why}' if why else ''))
    bad = sum(not ok for _, ok, _ in RESULTS)
    print(f'{len(RESULTS) - bad}/{len(RESULTS)} pass')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
