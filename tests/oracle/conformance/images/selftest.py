# SPDX-License-Identifier: GPL-2.0-or-later
"""Self-test of the images package. Run: python3 -I tests/oracle/conformance/images/selftest.py
Prints one line per group and a summary; exits nonzero on any failure."""

import hashlib
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from images import aes, crc, emmc, fwzip, ldr, otp, p224, records, spiflash, tfw  # noqa: E402

RESULTS = []


def group(name):
    def deco(fn):
        try:
            fn()
            RESULTS.append((name, None))
        except Exception as e:  # report and continue
            line = e.__traceback__.tb_next.tb_lineno if e.__traceback__.tb_next else 0
            RESULTS.append((name, '%s: %s (selftest.py line %d)' % (type(e).__name__, e, line)))
        return fn
    return deco


@group('crc')
def _():
    assert crc.crc32(b'123456789') == 0xCBF43926
    assert crc.crc16_xmodem(b'123456789') == 0x31C3


@group('reboot record (S-04)')
def _():
    b = records.build_reboot_record(30)
    assert b == struct.pack('<IHH', 0xDEADF00D, 30, 0) and len(b) == 8
    assert records.REBOOT_RECORD_ADDR == 0x11B01F00
    mem = bytearray(b)
    assert records.consume_reboot_record(mem) == 30 and mem[:4] == bytes(4)
    assert records.consume_reboot_record(mem) == 20          # magic gone: the default
    assert records.consume_reboot_record(bytearray(8)) == 20
    for r in (10, 30, 40, 60):
        assert not records.reason_boots_os(r)
    for r in (0, 20, 50, 0xFFFF):
        assert records.reason_boots_os(r)
    assert records.reason_menu(30) == records.reason_menu(40) != records.reason_menu(10)


@group('flash records (S-02)')
def _():
    sel = records.Selector(1, 0)
    assert sel.pack() == b'\x01\x00' and sel.slot == 1 and not sel.trusted
    assert records.Selector(7, 1).slot == 1 and records.Selector(0, 1).trusted
    img = bytes(range(256)) * 4
    rec = records.SlotRecord.for_image(img)
    assert rec.pack() == struct.pack('<III', crc.crc32(img), len(img), 0xB004)
    assert records.slot_record_check(rec, img) == 'ok'
    assert records.slot_record_check(records.SlotRecord(rec.crc, rec.length, 0xB005), img) == 'bad_tag'
    assert records.slot_record_check(rec, b'\x01' + img[1:]) == 'bad_crc'
    m = records.MetadataRecord(serial=b'TESTSER1', sku=3, factory=1, os_version=(1, 2, 3, 4),
                               sw_version=(5, 6, 7, 8), state=0xAB0D, cv_gain=0x11223344, cv_zero=0x5566,
                               extra=(0x77, 0x88))
    b = m.pack()
    assert len(b) == 0x34
    assert struct.unpack_from('<I', b, 0)[0] == 0xDEADBABE and b[4:14] == b'TESTSER1\0\0'
    assert struct.unpack_from('<HH', b, 0xE) == (3, 1)
    assert struct.unpack_from('<4H', b, 0x12) == (1, 2, 3, 4) and struct.unpack_from('<4H', b, 0x1A) == (5, 6, 7, 8)
    assert struct.unpack_from('<IIH2H', b, 0x24) == (0xAB0D, 0x11223344, 0x5566, 0x77, 0x88)
    assert records.MetadataRecord.unpack(b) == m
    bad = bytearray(b)
    bad[0] ^= 1
    assert records.read_metadata(bytes(bad)).pack() == bytes(0x34)


@group('records rules (S-04)')
def _():
    beh = records.reason_behavior
    assert beh(10) == {'boots': False, 'screen': 'main', 'cdc': True, 'transport': '-'}
    assert beh(30)['transport'] == 'msc' and beh(40)['transport'] == 'midi' and beh(0x1003C)['screen'] == 'main'
    assert [records.host_mode_reason(p) for p in (b'', b'\x01', b'\x02', b'\x04\x01', b'\x03')] == [40, 30, 10, 50, 40]
    assert records.nordic_step(20) == 'exit' and records.nordic_step(0) == 'none' and records.nordic_step(30) == 'enter'
    assert sorted(records.WRITERS) == sorted('W%d' % i for i in range(1, 13) if i not in (5, 8))
    assert records.writer_reason('W10', taken=30) == 70 and records.reason_behavior(70)['boots']
    assert records.writer_reason('W11', taken=30) == 80 and records.reason_behavior(80)['boots']
    assert records.writer_reason('W12', taken=30) == 90 and records.reason_behavior(90)['boots']
    assert records.writer_reason('W1', mode_payload=b'\x01') == 30 and records.writer_reason('W7', taken=60) == 60
    assert records.reboot_record(30) == records.build_reboot_record(30)
    b = records.build_metadata(serial='ABCDEFGH', sku=5, state='upgrading', sw_version=(1, 2, 3, 4))
    d = records.decode_metadata(b)
    assert d['valid'] and d['serial'] == 'ABCDEFGH' and d['state'] == 0xAB0D and d['sw_version'] == (1, 2, 3, 4)
    odd = bytearray(b)
    struct.pack_into('<I', odd, 0x24, 0x1234)
    odd[0x0C:0x0E] = b'XY'                       # +0x0C/+0x0D are not part of the serial
    d = records.decode_metadata(bytes(odd))
    assert d['state'] == 0 and d['serial'] == 'ABCDEFGH'
    bad = bytearray(b)
    bad[0] ^= 1
    assert records.decode_metadata(bytes(bad))['sku'] == 0
    # write_metadata: raw bytes kept under an invalid magic; serial, sku and factory only in factory_mode
    w = records.write_metadata(bytes(bad), (9, 9, 9, 9), (8, 8, 8, 8), 0)
    assert w[0:4] == b'\xbe\xba\xad\xde' and w[0x28:] == bytes(bad[0x28:]) and w[0x22:0x24] == bytes(bad[0x22:0x24])
    assert w[4:0x12] == bytes(bad[4:0x12]) and struct.unpack_from('<I', w, 0x24)[0] == 0
    f = records.write_metadata(bytes(0x34), (1, 0, 0, 0), (0, 0, 0, 0), 0xAB0D, True, 'NEWSER01', 7, 1)
    assert f[4:0x0D] == b'NEWSER01\0' and f[0x0D] == 0 and struct.unpack_from('<HH', f, 0x0E) == (7, 1)
    try:
        records.write_metadata(b, (0,) * 4, (0,) * 4, 5)
        raise AssertionError('state 5 written')
    except ValueError:
        pass


@group('ldr')
def _():
    data, blocks = ldr.synthetic_app(3, regions=[(0x11A00000, 0x40), (0x08000000, 0x123)], fill=(0x80000000, 0x20, 7))
    for b in blocks:
        assert ldr.header_ok(ldr.pack_header(b.flags, b.target, b.count, b.arg, b.dmacode))
    assert ldr.entry_point(blocks) == 0x11A00000
    first, last = blocks[0], blocks[-1]
    assert first.arg == last.offset - first.offset and 'FINAL' in last.flags
    assert ldr.stream_length(data) == len(data)
    assert ldr.pack_blocks(ldr.parse(data)) == data
    mem = ldr.load(blocks)
    assert mem[0x80000000] == struct.pack('<I', 7) * 8 and len(mem[0x08000000]) == 0x123
    assert len(ldr.synthetic_app_of_size(0x1234, 9)) == 0x1234
    bad = bytearray(data)
    bad[1] ^= 0x10
    try:
        ldr.parse(bytes(bad))
        raise AssertionError('corrupt header accepted')
    except ValueError:
        pass


@group('spi flash')
def _():
    f = spiflash.build_flash((0, 0), ('good', 'good'))
    assert len(f) == 0x100000 and not spiflash.areas_outside_layout(f)
    p = spiflash.parse_flash(f)
    assert p['selector'].pack() == b'\0\0' and p['records'][0].tag == 0xB004
    o = spiflash.first_stage_boot(f)
    assert (o.slot, o.check, o.selector_after, o.boot_address) == (0, 'ok', b'\0\x01', 0x40010000)
    assert o.writes == [('erase', 0xD000), ('program', 0xD000, 2)]
    o1, o2 = spiflash.boot_chain(spiflash.build_flash((1, 0), ('good', 'badcrc')))
    assert (o1.slot, o1.check, o1.selector_after) == (1, 'bad_crc', b'\0\x01')
    assert (o2.slot, o2.check, o2.writes, o2.record_read) == (0, None, [], None)
    o = spiflash.first_stage_boot(spiflash.build_flash((5, 1), ('empty', 'empty')))
    assert (o.slot, o.writes, o.record_read) == (1, [], None)
    o = spiflash.first_stage_boot(spiflash.build_flash(None, ('good', 'good')))      # erased selector
    assert o.slot == 1 and o.check == 'ok' and o.selector_after == b'\xff\x01'
    for st in spiflash.SLOT_STATES:                       # state names round-trip
        f = spiflash.build_flash((0, 0), (st, 'good'))
        assert spiflash.slot_state(f, 0) == st and spiflash.slot_good(f, 0) == (st == 'good'), st
        assert not spiflash.areas_outside_layout(f)
    # empty {0, 0, B004}: slot_status passes it, slot_good does not
    f = spiflash.build_spi_flash(b'\0\0', 'empty', 'good')
    assert spiflash.slot_status(f, 0) == 'ok' and not spiflash.slot_good(f, 0)
    e = spiflash.expected_boot_slot(f, 'factory')
    assert (e['boot'], e['selector_after'], e['result']) == (0, [b'\0\x01'], 'pass')
    e = spiflash.expected_boot_slot(f, 'conforming')
    assert (e['boot'], e['selector_after'], e['checked']) == (1, [b'\x01\x01'], [0, 1])
    e = spiflash.expected_boot_slot(spiflash.build_spi_flash(b'\x02\0', 'good', 'hugelen'), 'factory')
    assert e['boot'] == 'unspecified'
    assert spiflash.first_stage_boot(spiflash.build_spi_flash(b'\x02\0', 'good', 'hugelen')).undefined
    e = spiflash.expected_boot_slot(spiflash.build_spi_flash(b'\0\0', 'badtag', 'erased'), 'conforming')
    assert e['boot'] == 'open'
    assert spiflash.SLOT_OFF[1] + spiflash.SLOT_MAX == spiflash.FLASH_SIZE
    # stock_upgrade_write: index byte 1 -> slot 0 with {0,0}; else slot 1 with {1,0}
    img = spiflash.slot_image(0, seed=99, size=0x2100)
    fl = spiflash.stock_upgrade_write(spiflash.build_flash((1, 1)), img)
    assert fl.read(0xD000, 2) == b'\0\0' and spiflash.slot_status(fl.bytes(), 0) == 'ok'
    fl = spiflash.stock_upgrade_write(spiflash.build_flash((7, 1)), img)
    assert fl.read(0xD000, 2) == b'\x01\0' and spiflash.slot_status(fl.bytes(), 1) == 'ok'
    assert fl.ops[-2:] == [('erase', 0xD000), ('program', 0xD000, 2)]



@group('spi metadata mirror')
def _():
    old = records.MetadataRecord(serial=b'SN000123', sku=42, factory=1, sw_version=(1, 2, 3, 4),
                                 cv_gain=0x12345678, cv_zero=0x0800).pack()
    new = old[:0x1A] + bytes.fromhex('0100020004000000') + old[0x22:]
    m = spiflash.mirror_encode(old)
    assert len(m) == 0x68 and spiflash.mirror_decode(m) == old
    for i in range(0x68):                                 # any one raised bit breaks it (a torn erase)
        for k in range(8):
            if not m[i] >> k & 1:
                bad = bytearray(m)
                bad[i] |= 1 << k
                assert spiflash.mirror_decode(bytes(bad)) is None, (i, k)
    assert spiflash.mirror_decode(b'\xff' * 0x68) is None
    f = spiflash.build_spi_flash(metadata=old, mirror=True)
    assert spiflash.mirror_decode(f) == old
    fl = spiflash.FlashModel(f)
    assert spiflash.write_metadata_record(fl, new) is True
    d = fl.bytes()
    assert d[0xC000:0xC034] == new and d[0xB000:0xC000] == b'\xff' * 0x1000   # the mirror erased last
    ops = [(o[1], o[2]) for o in fl.op_log]
    assert ops == ([('read', 0xC000)] + [('read', 0xC000 + p) for p in range(0, 0x1000, 0x100)] +
                   [('erase', 0xB000), ('program', 0xB000), ('read', 0xB000),
                    ('erase', 0xC000), ('program', 0xC000), ('read', 0xC000), ('erase', 0xB000)]), ops
    # the two reads before the first erase differ: nothing is erased
    fl = spiflash.FlashModel(f)
    first = fl.read
    reads = []

    def garbled(addr, n):
        reads.append(addr)
        b = first(addr, n)
        return bytes([b[0] ^ 1]) + b[1:] if len(reads) == 2 else b
    fl.read = garbled
    assert spiflash.write_metadata_record(fl, new) is False
    assert all(o[1] == 'read' for o in fl.op_log) and fl.bytes() == f
    # a cut after the 0xC000 erase: the restore brings the new record back; a second restore does nothing
    fl = spiflash.FlashModel(f).cut_after(21)
    try:
        spiflash.write_metadata_record(fl, new)
        raise AssertionError('no cut')
    except spiflash.PowerCut:
        pass
    fl2 = spiflash.FlashModel(fl.bytes())
    assert spiflash.metadata_restore(fl2) == 'restored' and fl2.read(0xC000, 0x34) == new
    assert all(o[2] != 0xB000 or o[1] == 'read' for o in fl2.op_log)      # the mirror is never written
    assert spiflash.metadata_restore(spiflash.FlashModel(fl2.bytes())) == 'none'
    # profile 'factory': no mirror, no restore; a cut after its erase loses the record
    fl = spiflash.FlashModel(f).cut_after(1)
    try:
        spiflash.write_metadata_record(fl, new, 'factory')
    except spiflash.PowerCut:
        pass
    assert spiflash.metadata_restore(spiflash.FlashModel(fl.bytes()), 'factory') == 'none'
    assert fl.bytes()[0xC000:0xC004] == b'\xff' * 4
    # an adversarial torn program (only the magic's bits) leaves no valid mirror
    fl = spiflash.FlashModel(spiflash.build_spi_flash(metadata=old)).cut_inside(19, mask=lambda i: 0xFF if i < 4 else 0)
    try:
        spiflash.write_metadata_record(fl, new)
    except spiflash.PowerCut:
        pass
    assert fl.read(0xB000, 4) == old[:4] and spiflash.mirror_decode(fl.bytes()) is None
    try:
        spiflash.build_spi_flash(metadata=old, mirror=True, first_stage=b'\0' * 0xB001)
        raise AssertionError('mirror over a long first stage')
    except ValueError:
        pass

def assert_read_extent(reads, length, part_bytes):
    """S-03.043: contiguous reads from sector 1 covering sectors 1..N, none past N + 1 or the partition's
    last sector (N = ceil(length / 512))."""
    n = -(-length // 512)
    nxt = 1
    for first, count in reads:
        assert first == nxt and count > 0, reads
        nxt = first + count
    assert n <= nxt - 1 <= min(n + 1, part_bytes // 512 - 1), (reads, n)


@group('emmc (S-03)')
def _():
    os_img = emmc.synthetic_os(seed=1)
    h = emmc.OsHeader.for_image(os_img, active=1)
    b = h.pack()
    assert len(b) == 512 and b[0x0E:] == bytes(512 - 0x0E)
    assert struct.unpack_from('<III', b, 0) == (3, len(os_img), crc.crc32(os_img)) and b[0xC] == 1 and b[0xD] == 0
    assert emmc.OsHeader.unpack(b) == h
    ldr.parse(os_img)
    e = emmc.build_emmc({'image': os_img, 'active': 1}, {'image': emmc.synthetic_os(2), 'active': 1})
    r = emmc.load_os(e)
    assert r.ok and r.partition == emmc.BOOT1 and r.selects == (1, 0)
    assert_read_extent(r.reads, len(os_img), len(e.boot[emmc.BOOT1]))
    e = emmc.build_emmc({'image': os_img, 'active': 0}, {'image': emmc.synthetic_os(2), 'active': 1})
    r = emmc.load_os(e)
    assert r.ok and r.partition == emmc.BOOT2 and r.selects == (1, 2, 0)
    assert not emmc.load_os(emmc.build_emmc(None, None)).ok
    assert emmc.load_os(emmc.build_emmc({'active': 0}, {'active': 0})).reason == 'no_active'
    r = emmc.load_os(emmc.build_emmc({'active': 1, 'corrupt': True}, {'active': 1}))
    assert r.reason == 'crc_mismatch' and r.partition == emmc.BOOT1       # no fallback
    assert emmc.load_os(emmc.build_emmc({'active': 1}), max_len=16).reason == 'too_long'
    big = emmc.synthetic_os(3, size=0x30000)
    e = emmc.build_emmc({'image': big, 'active': 1})
    r = emmc.load_os(e)
    assert r.ok
    assert_read_extent(r.reads, 0x30000, len(e.boot[emmc.BOOT1]))
    full = emmc.synthetic_os(4, size=0x8000 - 512)                         # length B - 512: sector N is the last
    e = emmc.build_emmc({'image': full, 'active': 1}, boot_size=0x8000)
    r = emmc.load_os(e)
    assert r.ok
    assert_read_extent(r.reads, len(full), 0x8000)
    assert emmc.write_target(emmc.build_emmc({'active': 1})) == emmc.BOOT2
    assert emmc.write_target(emmc.build_emmc({'active': 0})) == emmc.BOOT1
    assert emmc.os_image_acceptable(bytes(10000)) and not emmc.os_image_acceptable(bytes(9000))
    assert emmc.os_image_acceptable(bytes(10001))                           # S-03.051
    assert emmc.acceptable(emmc.OS_MAX, 0x1000000) and not emmc.acceptable(emmc.OS_MAX + 1, 0x1000000)
    assert emmc.acceptable(0x4000 - 512, 0x4000) and not emmc.acceptable(0x4000 - 511, 0x4000)
    # the reader: zero length, format < 2 ignores `encrypted`, format 2/3 marked encrypted fails with no
    # image read (S-03.048), partition bound
    e = emmc.Emmc()
    e.install(emmc.BOOT1, b'', header=emmc.pack_header(3, 0, 0, 1, 0))
    assert emmc.load_os(e).reason == 'zero_length'
    for fmt in (0, 1, 2, 3):
        e = emmc.Emmc()
        e.install(emmc.BOOT1, os_img, 1, 1, format=fmt)
        assert e.boot[emmc.BOOT1][512:512 + len(os_img)] == os_img
        r = emmc.load_os(e)
        if fmt < 2:
            assert r.ok and r.image == os_img, fmt
        else:
            assert (r.ok, r.reason, r.reads, r.partition) == (False, 'encrypted', (), emmc.BOOT1), fmt
    e = emmc.build_emmc({'image': os_img, 'encrypted': 0x7F}, {'image': emmc.synthetic_os(2), 'active': 1})
    assert emmc.load_os(e).reason == 'encrypted' and emmc.load_os(e).selects == (1, 0)    # no fallback
    e = emmc.Emmc(0x4000)
    e.install(emmc.BOOT1, b'', header=emmc.pack_header(3, 0x4000 - 511, 0, 1, 0))
    assert emmc.load_os(e).reason == 'past_partition'
    # the writer
    e = emmc.build_emmc({'active': 1})
    res, ops, tgt = emmc.write_os(e, os_img + bytes(0x3000))
    assert (res, tgt) == ('ok', emmc.BOOT2) and ops[-1] == ('select', emmc.USER)
    assert emmc.load_os(e).partition == emmc.BOOT1 or not e.header(emmc.BOOT1).active
    assert emmc.load_os(e).ok and emmc.load_os(e).partition == emmc.BOOT2
    # S-03.055, S-03.058: header inactive then active on the target, the other rewritten inactive
    hw = [o for o in ops if o[0] == 'write' and o[2] == 0]
    assert hw == [('write', emmc.BOOT2, 0, 1)] * 2 + [('write', emmc.BOOT1, 0, 1)], hw
    assert not any(o[0] == 'erase' and o[1] == emmc.BOOT1 for o in ops)
    assert e.header(emmc.BOOT1).active == 0 and e.read(emmc.BOOT1, 0, 1)[:12] != bytes(12)
    e = emmc.build_emmc({'active': 1})
    res, ops, tgt = emmc.write_os(e, os_img + bytes(0x3000), profile='factory')
    assert res == 'ok' and ops[-2] == ('erase', emmc.BOOT1, 0) and e.read(emmc.BOOT1, 0, 1) == bytes(512)
    assert emmc.load_os(e).partition == emmc.BOOT2
    e = emmc.build_emmc({'active': 1})
    assert emmc.write_os(e, os_img + bytes(0x3000), fail='write_header_active')[0] == 'failed'
    assert not e.header(emmc.BOOT2).active and e.header(emmc.BOOT1).active   # S-03.057
    assert emmc.write_os(emmc.Emmc(), bytes(100))[0] == 'refused'
    assert emmc.write_os(emmc.Emmc(), bytes(20000), cut_after=2)[0] == 'cut'
    # S-03.033: format 3, encrypted 0, the image at sector 1
    e = emmc.build_emmc({'active': 1})
    assert emmc.write_os(e, os_img + bytes(0x3000))[0] == 'ok'
    h = e.read(emmc.BOOT2, 0, 1)
    assert (h[0], h[0xD]) == (3, 0) and e.read(emmc.BOOT2, 1, 1) == (os_img + bytes(0x3000))[:512]
    part = emmc.build_os_partition(os_img, format=3, encrypted=1)
    assert emmc.load_os(emmc.build_emmc(part)).reason == 'encrypted'
    assert emmc.fat_to_physical(0) == 0x1000000 and emmc.fat_to_physical(0xCF8000) == 0xCF8000
    assert emmc.fat_to_physical(0xFFFFFF) == 0xFFFFFF
    for s in (0, 0xCF7FFF, 0xCF8000, 0xFFFFFF):
        assert emmc.physical_to_fat(emmc.fat_to_physical(s)) == s
    assert emmc.FAT_LAST - emmc.FAT_FIRST + 1 == emmc.FAT_SECTORS


@group('aes / p224')
def _():
    assert aes.selftest()
    key, iv = bytes(range(32)), bytes(16)
    pt = os.urandom(64)
    assert aes.cbc_decrypt(key, iv, aes.cbc_encrypt(key, iv, pt)) == pt
    assert p224.on_curve(p224.G) and p224.mul(p224.N) is None
    d, pub = p224.test_key()
    h = hashlib.sha256(b'm').digest()
    r, s = p224.sign(d, h)
    assert p224.verify(pub, h, r, s) and not p224.verify(pub, h, r, (s + 1) % p224.N)


@group('firmware zip')
def _():
    os_img = emmc.synthetic_os(4)
    z = fwzip.build({'os': os_img, 'spiboot': (b'S' * 64, (2, 0, 1))}, version=(1, 2, 0), githash='abc123',
                    dlc={'pack_a': {'name': 'Pack A'}}, nordic_bootloaders=(b'n0', b'n1'))
    p = fwzip.parse(z)
    assert p['version'] == {'version': (1, 2, 0), 'build': 0, 'hash': 'abc123'}
    assert p['components']['os']['data'] == os_img and p['components']['spiboot']['version']['version'] == (2, 0, 1)
    assert p['dlc'] == {'pack_a': {'name': 'Pack A'}} and p['nordic_bootloaders'] == (b'n0', b'n1')
    assert not p['problems'] and not p['outside_root'] and not p['partial']
    assert all(n.startswith('firmware/') for n in zipfile.ZipFile(__import__('io').BytesIO(z)).namelist())
    assert fwzip.extract_path('firmware/blackfin/bin/os.ldr') == 'upgrade/blackfin/bin/os.ldr'
    assert fwzip.version_from_text('1000.0.20+0@deadbeef\n')['version'] == (1000, 0, 20)
    p = fwzip.parse(fwzip.build({'spiboot': b'x' * 0xB001}, extra={'firmware/partial_upgrade': b''}))
    assert p['partial'] and not p['components']['spiboot']['size_ok']


@group('tfw')
def _():
    z = fwzip.build({'os': emmc.synthetic_os(5)})
    for kw in ({}, {'preamble': (0, 1, 0, 0)}, {'aes_key_index': tfw.KEY_PLAINTEXT}):
        t = tfw.build(z, **kw)
        p = tfw.parse(t)
        assert p.ok, p.checks
        assert p.zip_bytes == z and p.name == b'firmware.zip'
        assert p.manifest_offset == (0x80 if 'preamble' in kw else 0x40)
        assert len(t) == p.manifest_offset + 0x300 + p.manifest.payload_length
    t = tfw.build(z, preamble=(0, 1, 0, 0))
    assert tfw.parse(t).preamble.forced
    assert t[0:4] == b'\xba\xbe\xca\xfe' and t[0x40:0x44] == b'\xbe\xef\xca\xfe'
    m = t[0x80:0x380]
    assert m[0x84:0x88] == b'TESG' and m[0x88] == 1 and struct.unpack_from('<I', m, 0x80)[0] == len(z)
    assert struct.unpack_from('>I', t, 0x40 + 0x17)[0] == tfw.product_word()
    # plaintext: the name block is visible; encrypted: it is not
    assert b'firmware.zip' in tfw.build(z, aes_key_index=tfw.KEY_PLAINTEXT)[0x340:0x440]
    assert b'firmware.zip' not in tfw.build(z)[0x340:0x440]
    base = tfw.build(z)

    def tamper(off, checks):
        bad = bytearray(base)
        bad[off] ^= 1
        c = tfw.parse(bytes(bad)).checks
        failed = {k for k, v in c.items() if not v}
        assert checks <= failed, (hex(off), failed)

    tamper(0x40 + 0x300 + 0x150, {'header_crc', 'payload_sha256'})       # payload byte
    tamper(0x40 + 0x20, {'header_crc', 'signature'})                       # signature byte
    tamper(0x40 + 0x9C, {'header_crc', 'signature', 'payload_sha256'})     # stored hash
    tamper(0x40 + 0x84, {'tag', 'signature'})
    bad = bytearray(base)
    struct.pack_into('>I', bad, 0x17, tfw.product_word(0x22))
    assert not tfw.parse(bytes(bad)).checks['product']
    other = tfw.test_keys(b'another test key')
    assert not tfw.parse(base, keys=other).checks['signature']
    nm = tfw.parse(tfw.build(z, name='firmware.bin', aes_key_index=tfw.KEY_PLAINTEXT))
    assert not nm.checks['name']
    # per-index test keys: K0, K1 and Q0 are distinct; index 0x01 decrypts with K1 only
    ks = tfw.test_key_set()
    assert ks.k0 == tfw.test_keys()[0] and ks.k1 == tfw.test_keys(b'opxy-conformance-k1')[0] != ks.k0
    assert ks.sign_for(0)[1] == tfw.test_keys()[2] and ks.sign_for(1)[1] != ks.sign_for(0)[1]
    p1 = tfw.parse(tfw.build(z, aes_key_index=tfw.KEY_OTP1))
    assert p1.ok and p1.zip_bytes == z
    assert tfw.parse(tfw.build(z, aes_key_index=tfw.KEY_OTP1), keys=(ks.k0, ks.q0_d, ks.q0)).zip_bytes != z
    assert tfw.parse(base).checks['te_signed']
    s1 = tfw.parse(tfw.build(z, sig_key_index=tfw.SIG_DEVICE)).checks
    assert s1['signature'] and not s1['te_signed']
    assert not tfw.parse(tfw.build(z, aes_key_index=tfw.KEY_DEV)).checks['payload_key']
    # r and s are the last 28 bytes of their fields: a nonzero pad still verifies, the pad is kept
    pad = tfw.build(z, sig_pad=b'\xaa\xbb\xcc\xdd\x11\x22\x33\x44')
    assert pad[0x50:0x54] == b'\xaa\xbb\xcc\xdd' and pad[0x70:0x74] == b'\x11\x22\x33\x44'
    assert tfw.parse(pad).checks['signature'] and tfw.parse(pad).manifest.sig_pad == pad[0x50:0x54] + pad[0x70:0x74]
    for how in tfw.SIGNATURE_FORMS[1:]:
        c = tfw.parse(tfw.build_bad_signature(z, how)).checks
        assert not c['signature'] and not c['te_signed'], how
        assert c['header_crc'] and c['payload_sha256'] and c['tag'], how


# P-224 known answers from RFC 6979 appendix A.2.4 (SHA-256 rows).
RFC6979_P224 = dict(
    d=0xF220266E1105BFE3083E03EC7A3A654651F45E37167E88600BF257C1,
    U=(0x00CF08DA5AD719E42707FA431292DEA11244D64FC51610D94B130D6C,
       0xEEAB6F3DEBE455E3DBF85416F7030CBD94F34F2D6F232C69F3C1385A),
    rows=[(b'sample', 0xAD3029E0278F80643DE33917CE6908C70A8FF50A411F06E41DEDFCDC,
           0x61AA3DA010E8E8406C656BC477A7A7189895E7E840CDFE8FF42307BA,
           0xBC814050DAB5D23770879494F9E0A680DC1AF7161991BDE692B10101),
          (b'test', 0xFF86F57924DA248D6E44E8154EB69F0AE2AEBAEE9931D0B5A969F904,
           0xAD04DDE87B84747A243A631EA47A1BA6D1FAA059149AD2440DE6FBA6,
           0x178D49B1AE90E3D8B629BE3DB5683915F4E8C99FDF6E666CF37ADCFD)])


@group('p224 KAT (RFC 6979 A.2.4)')
def _():
    import hashlib
    kat = RFC6979_P224
    d, U = kat['d'], kat['U']
    assert p224.on_curve(U) and p224.mul(d) == U
    for msg, k, r, s in kat['rows']:
        h = hashlib.sha256(msg).digest()
        assert p224.mul(k)[0] % p224.N == r, msg
        assert pow(k, -1, p224.N) * (p224._z(h) + r * d) % p224.N == s, msg
        assert p224.verify(U, h, r, s), msg
        assert not p224.verify(U, h, r, (s + 1) % p224.N) and not p224.verify(U, hashlib.sha256(msg + b'!').digest(), r, s)


@group('otp (S-16.095)')
def _():
    ks = tfw.test_key_set()
    o = otp.build_otp()
    assert o.aes_key(0) == ks.k0 and o.aes_key(1) == ks.k1 and o.public_key(0) == ks.q0
    assert o.public_key(1) == ks.sign_for(tfw.SIG_DEVICE)[1] != ks.q0
    assert o.serial() == 'TESTSERIAL00000001' and len(o.fsn) == 16 and o.provisioned()
    assert o.read(otp.ITEM_AES[0][0]) == (True, ks.k0[:16])
    assert not otp.build_otp(gp1_nonzero=14).provisioned() and otp.build_otp(gp1_nonzero=15).provisioned()
    assert not otp.build_otp(gp1_nonzero=None).provisioned()
    assert otp.is_provisioned(bytes(28) + b'\xff' * 4) is False
    assert otp.build_otp(k0=False).keyset() == {'K0': None, 'K1': ks.k1, 'Q0': ks.q0}
    half = otp.build_otp()
    half.failing.add(4)                                  # one item of K0 fails: no K0
    assert half.aes_key(0) is None and half.aes_key(1) == ks.k1
    assert otp.build_otp(q0=False).public_key(0) is None
    # a package built with the test keys verifies under the OTP's Q0 and decrypts with its K0
    z = fwzip.build({'os': b'\x11' * 300})
    t = tfw.build(z)
    k = o.keyset()
    assert tfw.parse(t, keys=(k['K0'], 0, k['Q0'])).zip_bytes == z
    assert tfw.verify_signature(t[0x40:0x340], k['Q0'])
    # word order, and a public key in a longer item
    assert otp.item_bytes([0x44332211, 0x88776655]) == bytes.fromhex('1122334455667788')
    assert otp.item_words(bytes.fromhex('1122334455')) == [0x44332211, 0x55]
    assert o.read_words(otp.ITEM_AES[0][0]) == (True, otp.item_words(ks.k0[:16]))
    long9 = otp.build_otp()
    long9.items[otp.ITEM_Q0] = p224.pub_bytes(ks.q0) + b'\xa5' * 8
    assert long9.public_key(0) == ks.q0
    # the FSN through the ADI-area read
    ret, w = o.adi_read(otp.ADI_FSN)
    assert ret == 1 and len(w) == 4 and otp.item_bytes(w) == o.fsn
    assert o.adi_read(0x12) == (0, []) and otp.build_otp(fsn=None).adi_read(otp.ADI_FSN) == (0, [])
    # the configuration entry changes no read
    fsn = o.adi_read(otp.ADI_FSN)
    fresh = otp.build_otp()
    assert fresh.cfg_calls == 0 and fresh.keyset() == o.keyset() and fresh.adi_read(otp.ADI_FSN) == fsn
    bad = otp.build_otp()
    bad.cfg_return = 0
    assert bad.configure() == 0 and bad.configure() == 0 and bad.cfg_calls == 2
    assert bad.keyset() == o.keyset() and bad.serial() == o.serial() and bad.adi_read(otp.ADI_FSN) == fsn

    def gated(m, call):                                  # wrong reading: reads only after a nonzero configure
        if not call or m.configure() == 0:
            return {'K0': None, 'K1': None, 'Q0': None}, (0, [])
        return m.keyset(), m.adi_read(otp.ADI_FSN)
    assert gated(otp.build_otp(), True) == (o.keyset(), fsn)
    assert gated(otp.build_otp(), False) != (o.keyset(), fsn) and gated(bad, True) != (o.keyset(), fsn)
    # S-04.047: a serial with a ';' or a byte outside 0x20-0x7E reads as '', serial_read still True
    semi = otp.build_otp(serial='TESTSERIAL;0000001')
    assert semi.serial() == '' and semi.serial_read() and otp.build_otp(serial=None).serial_read() is False


@group('otp vectors (S-16.095)')
def _():
    # vectors/otp_items.json is gen_otp_vectors.py's output; decode() reads it independently of otp.py
    import json
    from images import gen_otp_vectors
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, 'vectors', 'otp_items.json'), encoding='utf-8') as f:
        vec = json.load(f)
    assert json.dumps(vec, indent=1) + '\n' == gen_otp_vectors.text(), 'otp_items.json is not the generator output'

    def decode(u, msb_first=False, key_tail=False, swap_halves=False, q0_item=9, serial_whole=False):
        def b(i):
            return b''.join(int(w, 16).to_bytes(4, 'big' if msb_first else 'little') for w in u['items']['0x%x' % i])

        def key(i, j):
            lo, hi = b(i)[:16], b(j)[:16]
            return (hi + lo) if swap_halves else (lo + hi)

        def pub(i):
            k = b(i)[-0x38:] if key_tail else b(i)[:0x38]
            return {'x': k[:28].hex(), 'y': k[28:].hex()}
        s = b(1) if serial_whole else b(1)[:18]
        return {'serial_hex': s.split(b'\0', 1)[0].hex(), 'k0': key(3, 4).hex(), 'k1': key(5, 6).hex(),
                'q0': pub(q0_item), 'q1': pub(0x13 - q0_item),
                'provisioned': sum(1 for x in b(0xE)[:28] if x) > 14}

    assert vec['requirement'] == 'S-16.095' and len(vec['units']) >= 4
    ks = tfw.test_key_set()
    digest = bytes(range(32))
    r, s = p224.sign(ks.q0_d, digest)
    for u in vec['units']:
        e = u['expect']
        assert decode(u) == e, u['name']
        o = otp.Otp({int(k, 16): otp.item_bytes([int(w, 16) for w in v]) for k, v in u['items'].items()})
        assert o.aes_key(0).hex() == e['k0'] and o.aes_key(1).hex() == e['k1'], u['name']
        q0 = (int(e['q0']['x'], 16), int(e['q0']['y'], 16))
        q1 = (int(e['q1']['x'], 16), int(e['q1']['y'], 16))
        assert o.public_key(0) == q0 and o.provisioned() == e['provisioned'], u['name']
        assert o.serial().encode('latin-1').hex() == e['serial_hex'], u['name']
        assert p224.on_curve(o.public_key(1)), u['name']
        # Q0 verifies a signature-index-0 signature; the item 0xA key does not
        assert p224.verify(q0, digest, r, s) and not p224.verify(q1, digest, r, s), u['name']
    # each wrong reading of the words fails at least one unit
    for kw in ({'msb_first': True}, {'key_tail': True}, {'swap_halves': True}, {'q0_item': 0xA},
               {'serial_whole': True}):
        assert any(decode(u, **kw) != u['expect'] for u in vec['units']), 'reading %r is not caught' % kw


def main():
    fails = [(n, e) for n, e in RESULTS if e]
    for n, e in RESULTS:
        print('%-22s %s' % (n, 'ok' if not e else 'FAIL ' + e))
    print('selftest: %d groups, %d passed, %d failed' % (len(RESULTS), len(RESULTS) - len(fails), len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
