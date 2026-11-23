#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host tests of tools/mkldr.py and tools/layout.py on a synthetic ELF32 file
laid out like the bootloader image (bf703.ld), and of tools/testos_layout.py
and mkldr.py --no-init on one laid out like the test OS (bf703_testos.ld,
wiki/Test-OS.md), and --os-1f and --device both on one laid out like the installer, one
image for both devices (bf703_installer.ld). The streams and layouts they write are
judged by tests/oracle/S-01-boot-chain's checkers. Also tools/png2logo.py on PNG files made here
(S-15.132). Python 3, standard library only.

    test_image_tools.py
"""
import contextlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(TOP, 'tools')
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOP, 'tests', 'oracle', 'S-01-boot-chain'))
import elf32  # noqa: E402
import installer_image  # noqa: E402
import layout  # noqa: E402
import mkldr  # noqa: E402
import png2logo  # noqa: E402
import testos_layout  # noqa: E402
import testos_regs  # noqa: E402
import check_layout  # noqa: E402
import check_stream  # noqa: E402
import check_stack  # noqa: E402

LD = os.path.join(TOP, 'src', 'drivers', 'bf70x', 'bf703.ld')
TESTOS_LD = os.path.join(TOP, 'src', 'testos', 'bf703_testos.ld')
BOARD = os.path.join(TOP, 'src', 'include', 'waffle', 'board', 'opxy.h')
FLASH = os.path.join(TOP, 'src', 'include', 'waffle', 's02_flash.h')
MEMMAP = os.path.join(TOP, 'src', 'include', 'waffle', 's01_memmap.h')
# The Makefile's FIRST_STAGE_STACK (S-01.009, S-02.091 rule 4).
FIRST_STAGE_STACK = (0x11807000, 0x11808000)

AX = elf32.SHF_ALLOC | elf32.SHF_EXECINSTR
WA = elf32.SHF_ALLOC | elf32.SHF_WRITE
R_BFIN_BYTE4_DATA = 0x0A


def build_elf(sections, symbols, relocs, entry, lma=None):
    """sections: [(name, type, flags, addr, bytes or size)]; symbols:
    [(name, value, section name[, size])]; relocs: {section name: [(offset, symbol
    name, addend)]}; lma: {section name: load address} for a section loaded
    somewhere other than its run address, which gets a PT_LOAD program header
    saying so (the others have none, so their load address is their run
    address). Returns the bytes of an ET_EXEC little-endian ELF32."""
    lma = lma or {}
    secs = [('', 0, 0, 0, b'')] + list(sections)
    names = [s[0] for s in secs]
    for target in relocs:
        secs.append(('.rela' + target, elf32.SHT_RELA, 0, 0, None))
    secs += [('.symtab', elf32.SHT_SYMTAB, 0, 0, None), ('.strtab', elf32.SHT_STRTAB, 0, 0, None),
             ('.shstrtab', elf32.SHT_STRTAB, 0, 0, None)]
    allnames = [s[0] for s in secs]
    isym, istr, ishstr = len(secs) - 3, len(secs) - 2, len(secs) - 1

    strtab = bytearray(b'\0')
    symtab = bytearray(bytes(16))
    symidx = {}
    for i, sym in enumerate(symbols, 1):
        n, v, sec = sym[:3]
        symidx[n] = i
        off = len(strtab)
        strtab += n.encode() + b'\0'
        symtab += struct.pack('<IIIBBH', off, v, sym[3] if len(sym) > 3 else 0, 0x12, 0,
                              names.index(sec))
    shstr = bytearray(b'\0')
    name_off = []
    for n in allnames:
        name_off.append(len(shstr) if n else 0)
        if n:
            shstr += n.encode() + b'\0'

    body = bytearray()
    headers = []
    phdrs = []
    base = 52
    for i, (n, t, fl, addr, data) in enumerate(secs):
        link = info = entsize = 0
        if t == elf32.SHT_RELA:
            target = n[len('.rela'):]
            data = b''.join(struct.pack('<IIi', off, (symidx[s] << 8) | R_BFIN_BYTE4_DATA, a)
                            for off, s, a in relocs[target])
            link, info, entsize = isym, names.index(target), 12
        elif i == isym:
            data, link, info, entsize = bytes(symtab), istr, 1, 16
        elif i == istr:
            data = bytes(strtab)
        elif i == ishstr:
            data = bytes(shstr)
        if t == elf32.SHT_NOBITS:
            size, off = data, 0
        else:
            off, size = base + len(body), len(data)
            body += data
            body += bytes(-len(body) & 3)
        if n in lma:
            phdrs.append(struct.pack('<IIIIIIII', elf32.PT_LOAD, off, addr, lma[n], size, size, 5, 4))
        headers.append(struct.pack('<IIIIIIIIII', name_off[i], t, fl, addr, off if i else 0,
                                   size if i else 0, link, info, 4 if i else 0, entsize))
    shoff = base + len(body)
    phoff = shoff + 40 * len(secs) if phdrs else 0
    ident = b'\x7fELF' + bytes([1, 1, 1]) + bytes(9)
    eh = ident + struct.pack('<HHIIIIIHHHHHH', 2, 106, 1, entry, phoff, shoff, 0, 52,
                             32 if phdrs else 0, len(phdrs), 40, len(secs), ishstr)
    return eh + bytes(body) + b''.join(headers) + b''.join(phdrs)


TEXT = bytes(range(1, 31))           # 30 bytes: not a multiple of 4


def image(handover_reloc=None, ddr_size=None, l2=None, extra=(), symbols_extra=()):
    sections = [
        ('.l1.code', elf32.SHT_PROGBITS, AX, 0x11A00000, b'\x11' * 0x40),
        ('.l1.text', elf32.SHT_PROGBITS, AX, 0x11A00040, b'\x22' * 0x22),
        ('.l1.data', elf32.SHT_PROGBITS, WA, 0x11800000, b'\x33' * 8),
        ('.l1.stack', elf32.SHT_NOBITS, WA, 0x11800008, 0x400),
        ('.l1.bss', elf32.SHT_NOBITS, WA, 0x11900000, 0x100),
        ('.handover.text', elf32.SHT_PROGBITS, AX, 0x0803C000, b'\x44' * 0x20),
        ('.handover.stack', elf32.SHT_NOBITS, WA, 0x0803C020, 0x1000),
        ('.handover.data', elf32.SHT_PROGBITS, WA, 0x0803D020, bytes(0x268)),
        ('.text', elf32.SHT_PROGBITS, AX, 0x8803D010, TEXT),
        ('.rodata', elf32.SHT_PROGBITS, elf32.SHF_ALLOC, 0x8803D02E, b'\x55' * 3),
        ('.bss', elf32.SHT_NOBITS, WA, 0x8803D040, ddr_size or 0x1000),
    ]
    if l2 is not None:
        sections.append(('.l2.data', elf32.SHT_PROGBITS, WA, l2, b'\x66' * 8))
    sections += list(extra)
    symbols = [('_init', 0x11A00000, '.l1.code'), ('_wb_reset', 0x11A00040, '.l1.text'),
               ('_far', 0x8803D010, '.text'), ('_l1_data', 0x11800000, '.l1.data'),
               ('_ho', 0x0803C010, '.handover.text'),
               ('_ho_stack', 0x0803C020, '.handover.stack', 0x1000),
               ('_config_block', 0x0803D020, '.handover.data', 0x258),
               ('_header_buffer', 0x0803D278, '.handover.data', 0x10)]
    symbols += list(symbols_extra)
    relocs = {'.l1.code': [(0x11A00004, '_l1_data', 0)],
              '.handover.text': [(0x0803C004, handover_reloc or '_ho', 0)],
              '.text': [(0x8803D014, '_init', 0)]}
    return build_elf(sections, symbols, relocs, 0x11A00040)


def random_bytes(n, seed=1):
    """n bytes that deflate does not shorten."""
    import random
    return random.Random(seed).randbytes(n)


def front(l2=0x08002000):
    """A front as src/front/front.ld lays one out."""
    sections = [
        ('.fr_l1code', elf32.SHT_PROGBITS, AX, 0x11A04000, b'\x71' * 0x40),
        ('.fr_l1data', elf32.SHT_PROGBITS, WA, 0x11802000, b'\x72' * 8),
        ('.fr_l2', elf32.SHT_PROGBITS, AX, l2, b'\x73' * 0x30),
        ('.fr_bss', elf32.SHT_NOBITS, WA, l2 + 0x30, 0x100),
    ]
    symbols = [('_fr_init', 0x11A04000, '.fr_l1code'), ('_fr_entry', l2, '.fr_l2')]
    return build_elf(sections, symbols, {}, l2)


def stub(text=b'\x7a' * 0x40):
    """A stub as src/stub/stub.ld lays one out."""
    sections = [
        ('.stub', elf32.SHT_PROGBITS, AX, 0x08031000, text),
        ('.stub_bss', elf32.SHT_NOBITS, WA, 0x08031000 + len(text), 0x2400),
    ]
    return build_elf(sections, [('_wb_stub_entry', 0x08031000, '.stub')], {}, 0x08031000)


def payload(data, b):
    """Block b's payload in stream data."""
    off = data.index(mkldr.header(0, b['target'], b['count'], 0)) + 16
    return data[off:off + b['count']]


def unpacked(data):
    """The stream's packed block, unpacked: (entry, runs)."""
    blocks = check_stream.ldr.parse(data)
    pk = [b for b in blocks if b['target'] == mkldr.PACK_AT[0]]
    assert len(pk) == 1
    return mkldr.unpack(payload(data, pk[0]))


VERSION = struct.pack('<4H', 1, 2, 3, 4)


def fimage(version=True, **kw):
    """image() with the bootloader's version in it, as the descriptor needs."""
    extra = list(kw.pop('extra', ()))
    symbols = []
    if version:
        extra.append(('.wb.version', elf32.SHT_PROGBITS, elf32.SHF_ALLOC, 0x8803D034, VERSION))
        symbols.append(('_opxy_bl_version', 0x8803D034, '.wb.version'))
    return image(extra=extra, symbols_extra=symbols, **kw)


class FrontMerge(unittest.TestCase):
    """tools/mkldr.py --front: the front's blocks, its INIT and entry, and
    the descriptor (src/front/fr_handoff.h)."""

    def merged(self):
        return mkldr.merged(elf32.Elf(fimage()), elf32.Elf(front()), elf32.Elf(stub()))

    def test_entry_init_and_rules(self):
        data = self.merged()
        blocks = check_stream.ldr.parse(data)
        first = [b for b in blocks if b['flags'] & check_stream.ldr.FIRST]
        final = [b for b in blocks if b['flags'] & check_stream.ldr.FINAL]
        init = [b for b in blocks if b['flags'] & check_stream.ldr.INIT]
        self.assertEqual(first[0]['target'], 0x08002000)
        self.assertEqual(final[-1]['target'], 0x08002000)
        self.assertEqual([b['target'] for b in init], [0x11A04000])
        res = check_stream.check('bootloader', data, FIRST_STAGE_STACK)
        self.assertTrue(all(ok for _, ok, _ in res), res)

    def test_descriptor(self):
        data = self.merged()
        blocks = check_stream.ldr.parse(data)
        desc = [b for b in blocks if b['target'] == mkldr.FR_DESC_ADDR]
        self.assertEqual(len(desc), 1)
        d = payload(data, desc[0])
        magic, ver, size, entry, n, crc, fcrc = struct.unpack_from('<IHHIIII', d)
        self.assertEqual((magic, ver), (mkldr.FR_DESC_MAGIC, mkldr.FR_DESC_VERSION))
        self.assertEqual(entry, 0x08031000)              # the stub's entry
        self.assertEqual(zlib.crc32(d[:size - 4]), struct.unpack_from('<I', d, size - 4)[0])
        # every block but the front's, the descriptor and L1 code (S-01.023,
        # S-13a.040: the front cannot read L1 code as data), in stream order
        listed = [(b['target'], payload(data, b)) for b in blocks
                  if b['count'] and not b['flags'] & check_stream.ldr.IGNORE
                  and not mkldr.in_front(b['target']) and b['target'] != mkldr.FR_DESC_ADDR
                  and not 0x11A00000 <= b['target'] < 0x11A0C000]
        self.assertEqual([a for a, _ in listed], [0x11800000, 0x08031000, mkldr.PACK_AT[0]])
        self.assertEqual(n, len(listed))
        want = 0
        for i, (a, b) in enumerate(listed):
            self.assertEqual(struct.unpack_from('<II', d, 0x40 + 8 * i), (a, len(b)))
            want = zlib.crc32(b, want)
        self.assertEqual(crc, want)
        self.assertEqual(d[0x18:0x20], VERSION)          # the image's _opxy_bl_version
        self.assertEqual(d[0x20:0x40], mkldr.front_pin(elf32.Elf(front())))
        fwant = 0
        for _, b in mkldr.front_runs(elf32.Elf(front())):
            fwant = zlib.crc32(b, fwant)
        self.assertEqual(fcrc, fwant)

    def test_packed(self):
        # The image's runs outside L1 are in the packed block, L1's in the
        # stream as they are; the packed block enters the image's entry.
        data = self.merged()
        main = elf32.Elf(fimage())
        rs = mkldr.runs(main)
        entry, runs = unpacked(data)
        self.assertEqual(entry, 0x11A00040)              # the image's _wb_reset
        self.assertEqual(runs, [(a, bytes(b)) for a, b in rs if not mkldr.in_l1(a)])
        blocks = check_stream.ldr.parse(data)
        for a, b in rs:
            hits = [x for x in blocks if x['target'] == a]
            self.assertEqual(len(hits), 1 if mkldr.in_l1(a) else 0, '%08x' % a)
        # the stub's runs as they are
        self.assertEqual([payload(data, x) for x in blocks if x['target'] == 0x08031000],
                         [b'\x7a' * 0x40])
        # a spoiled block fails only its output's CRC
        bad = mkldr.merged(elf32.Elf(fimage()), elf32.Elf(front()), elf32.Elf(stub()), spoil=True)
        with self.assertRaises(SystemExit) as cm:
            unpacked(bad)
        self.assertIn("output's CRC-32", str(cm.exception))
        # --unpacked takes the image's runs and entry, nothing else
        raw = struct.pack('<I', entry) + b''.join(
            struct.pack('<II', a, len(b)) + b for a, b in runs)
        self.assertTrue(mkldr.unpacked_matches(elf32.Elf(fimage()), raw))
        self.assertFalse(mkldr.unpacked_matches(elf32.Elf(fimage()), raw[:-1] + bytes([raw[-1] ^ 1])))

    def test_pack_refusals(self):
        # Packed runs go only where the stub writes; neither the image nor the
        # stub may meet the other, the packed block or the stub's record.
        far = ('.far', elf32.SHT_PROGBITS, WA, 0x882BD000, b'\x68' * 8)
        with self.assertRaises(SystemExit):
            mkldr.merged(elf32.Elf(fimage(extra=[far])), elf32.Elf(front()), elf32.Elf(stub()))
        for addr in (0x882BD000, 0x8833CFF0, 0x08031000, 0x0803BF00):
            bss = ('.big.bss', elf32.SHT_NOBITS, WA, addr, 0x100)
            with self.assertRaises(SystemExit, msg='%08x' % addr):
                mkldr.merged(elf32.Elf(fimage(extra=[bss])), elf32.Elf(front()), elf32.Elf(stub()))
        with self.assertRaises(SystemExit):     # the stub over its record
            mkldr.merged(elf32.Elf(fimage()), elf32.Elf(front()),
                         elf32.Elf(stub(text=bytes(0xAFE0 - 0x2400 + 4))))
        with self.assertRaises(SystemExit):     # more runs than the block holds
            mkldr.pack([(0x8803D000 + 0x100 * i, bytes(4)) for i in range(mkldr.PACK_RUNS + 1)], 0)
        self.assertEqual(mkldr.unpack(mkldr.pack([(0x8803D000, bytes(8))], 7)),
                         (7, [(0x8803D000, bytes(8))]))

    def test_image_without_a_version_refused(self):
        # The front shows the descriptor's version with the early logo: an
        # image that does not carry one is not streamed with zeros.
        with self.assertRaises(SystemExit):
            mkldr.merged(elf32.Elf(fimage(version=False)), elf32.Elf(front()), elf32.Elf(stub()))
        bss = ('.wb.version', elf32.SHT_NOBITS, WA, 0x8803D034, 8)
        with self.assertRaises(SystemExit):
            mkldr.merged(elf32.Elf(image(extra=[bss], symbols_extra=[
                ('_opxy_bl_version', 0x8803D034, '.wb.version')])), elf32.Elf(front()),
                elf32.Elf(stub()))

    def test_pin_follows_every_byte(self):
        a = mkldr.front_pin(elf32.Elf(front()))
        self.assertEqual(a, mkldr.front_pin(elf32.Elf(front())))
        self.assertNotEqual(a, mkldr.front_pin(elf32.Elf(front(l2=0x08002100))))

    def test_image_meeting_the_front_refused(self):
        with self.assertRaises(SystemExit):
            mkldr.merged(elf32.Elf(fimage(l2=0x08002800)), elf32.Elf(front()), elf32.Elf(stub()))
        with self.assertRaises(SystemExit):
            mkldr.merged(elf32.Elf(fimage(l2=0x08030400)), elf32.Elf(front()), elf32.Elf(stub()))
        with self.assertRaises(SystemExit):
            mkldr.front_runs(elf32.Elf(front(l2=0x08031000)))

    def test_run_spanning_a_window_refused(self):
        # Both ends outside the front's L1 data window, the window inside.
        span = ('.l1.big', elf32.SHT_PROGBITS, WA, 0x11801000, b'\x67' * 0x4000)
        with self.assertRaises(SystemExit):
            mkldr.merged(elf32.Elf(fimage(extra=[span])), elf32.Elf(front()), elf32.Elf(stub()))

    def test_noload_over_the_front_refused(self):
        for addr in (0x08030800, 0x08030000, 0x08002000, 0x11803000):
            bss = ('.l2.bss', elf32.SHT_NOBITS, WA, addr, 0x100)
            with self.assertRaises(SystemExit, msg='%08x' % addr):
                mkldr.merged(elf32.Elf(fimage(extra=[bss])), elf32.Elf(front()), elf32.Elf(stub()))
        # In the handover reserve, above the stub's region: accepted.
        bss = ('.l2.bss', elf32.SHT_NOBITS, WA, 0x0803C000, 0x100)
        mkldr.merged(elf32.Elf(fimage(extra=[bss])), elf32.Elf(front()), elf32.Elf(stub()))

    def test_block_in_the_front_frame_refused(self):
        # S-01.023: no stream block meets the front's frame, 0x8FE00000-0x8FE34800;
        # a block just below it or just past it is taken.
        self.assertEqual(mkldr.FR_FRAME, (0x8FE00000, 0x8FE34800))
        blk = lambda a, n=0x10: ('.ddr.data', elf32.SHT_PROGBITS, WA, a, b'\x6a' * n)
        for a in (0x8FE00000, 0x8FDFFFF0 + 4, 0x8FE347FC, 0x8FE10000):
            with self.assertRaises(SystemExit, msg='%08x' % a) as cm:
                mkldr.merged(elf32.Elf(fimage(extra=[blk(a)])), elf32.Elf(front()), elf32.Elf(stub()))
            self.assertIn("front's frame (S-01.023)", str(cm.exception))
        mkldr.spare_frame([(0x8FDFFFF0, bytes(0x10)), (0x8FE34800, bytes(0x10))])
        # A NOLOAD section there, the main image's own frame buffer, is taken.
        bss = ('.frame', elf32.SHT_NOBITS, WA, 0x8FE00000, 0x34800)
        mkldr.merged(elf32.Elf(fimage(extra=[bss])), elf32.Elf(front()), elf32.Elf(stub()))
        with Tmp() as t:
            e = t.write('a.elf', image(extra=[blk(0x8FE00000)]))
            with self.assertRaises(SystemExit):
                mkldr.main(['mkldr.py', e, t.path('a.ldr'), '--front-only'])
            self.assertEqual(mkldr.main(['mkldr.py', e, t.path('b.ldr'), '--no-init']), 0)

    def test_image_over_the_start_log_refused(self):
        # S-02.095: the slot's last sector is the start log. A stream of
        # FR_LOG_IMAGE_MAX bytes is taken, one word more is refused for it.
        self.assertEqual(mkldr.FR_LOG_IMAGE_MAX, 0x77000)
        with self.assertRaises(SystemExit) as cm:
            mkldr.slot_size(bytes(mkldr.FR_LOG_IMAGE_MAX + 4))
        self.assertIn('start log (S-02.095)', str(cm.exception))
        self.assertIn('0x77000', str(cm.exception))
        # an image that does not deflate, past the limit, through the merge
        noise = random_bytes(mkldr.FR_LOG_IMAGE_MAX)
        fill = ('.fill', elf32.SHT_PROGBITS, WA, 0x8803E000, noise)
        with self.assertRaises(SystemExit) as cm:
            mkldr.merged(elf32.Elf(fimage(extra=[fill])), elf32.Elf(front()), elf32.Elf(stub()))
        self.assertIn('start log (S-02.095)', str(cm.exception))

    def test_image_reaching_the_settings_sector_refused(self):
        # S-02.104: the build's own slot image stops below the slot's settings
        # sector (0x86000 in Slot 1, 0xFE000 in Slot 2): the limit builds, one
        # word more does not.
        mkldr.slot_size(bytes(mkldr.STORE_IMAGE_MAX))
        with self.assertRaises(SystemExit) as cm:
            mkldr.slot_size(bytes(mkldr.STORE_IMAGE_MAX + 4))
        self.assertIn('settings and its start log (S-02.104)', str(cm.exception))
        self.assertIn('0x76000', str(cm.exception))
        self.assertEqual(mkldr.STORE_IMAGE_MAX, 0x76000)
        self.assertEqual(0x10000 + mkldr.STORE_IMAGE_MAX, 0x86000)
        self.assertEqual(0x88000 + mkldr.STORE_IMAGE_MAX, 0xFE000)
        noise = random_bytes(mkldr.STORE_IMAGE_MAX - len(self.merged()) + 0x100)
        fill = ('.fill', elf32.SHT_PROGBITS, WA, 0x8803E000, noise)
        with self.assertRaises(SystemExit) as cm:
            mkldr.merged(elf32.Elf(fimage(extra=[fill])), elf32.Elf(front()), elf32.Elf(stub()))
        self.assertIn('(S-02.104)', str(cm.exception))

    def test_installer_image_limit(self):
        # S-02.104: the installer carries at most 0x76000 bytes; one more is refused before
        # the pin is compared.
        self.assertEqual(installer_image.CAPACITY, 0x76000)
        with tempfile.TemporaryDirectory() as d:
            img, pin = os.path.join(d, 'a.ldr'), os.path.join(d, 'front.pin')
            with open(img, 'wb') as f:
                f.write(bytes(0x76001))
            with open(pin, 'w', encoding='ascii') as f:
                f.write('00' * 32 + '\n')
            with self.assertRaises(SystemExit) as cm:
                installer_image.main(['installer_image.py', img, pin, os.path.join(d, 'o.c')])
            self.assertIn('settings and its start log (S-02.104)', str(cm.exception))
            with open(img, 'wb') as f:
                f.write(bytes(0x76000))
            with self.assertRaises(SystemExit) as cm:
                installer_image.main(['installer_image.py', img, pin, os.path.join(d, 'o.c')])
            self.assertNotIn('S-02.104', str(cm.exception))

    def test_run_where_the_front_does_not_read_refused(self):
        far = ('.far', elf32.SHT_PROGBITS, WA, 0x88400000, b'\x68' * 8)
        with self.assertRaises(SystemExit):
            mkldr.merged(elf32.Elf(fimage(extra=[far])), elf32.Elf(front()), elf32.Elf(stub()))


MARK = 0x8803D03C                    # where fimage_marked() puts _opxy_l1_mark


def fimage_marked(word=mkldr.L1_MARK_EMPTY, **kw):
    """fimage() with the slave's L1 check's mark (S-09.019), as a BOOT_LINK_FC build has it."""
    mark = ('.rodata.opxy_l1_mark', elf32.SHT_PROGBITS, elf32.SHF_ALLOC, MARK,
            struct.pack('<IIII', word, 0, 0, 0))
    extra = list(kw.pop('extra', ())) + [mark]
    img = image(extra=extra + [('.wb.version', elf32.SHT_PROGBITS, elf32.SHF_ALLOC, 0x8803D034, VERSION)],
                symbols_extra=[('_opxy_bl_version', 0x8803D034, '.wb.version'),
                               ('_opxy_l1_mark', MARK, '.rodata.opxy_l1_mark', 16)], **kw)
    return img


class L1Mark(unittest.TestCase):
    """tools/mkldr.py's mark for the slave's check of its L1 code (S-09.019): filled with the run the stream
    loads into L1 code and its CRC-32 before the descriptor is made, so the front's check covers the
    filled mark; an image without the mark keeps every byte."""

    def stream_mark(self, data):
        for a, b in unpacked(data)[1]:
            if a <= MARK < a + len(b):
                return struct.unpack_from('<IIII', b, MARK - a)
        return None

    def test_filled(self):
        data = mkldr.merged(elf32.Elf(fimage_marked()), elf32.Elf(front()), elf32.Elf(stub()))
        magic, addr, n, crc = self.stream_mark(data)
        code = [(a, b) for a, b in mkldr.runs(elf32.Elf(fimage_marked()))
                if 0x11A00000 <= a < 0x11A0C000]
        self.assertEqual(len(code), 1)
        self.assertEqual((magic, addr, n, crc),
                         (mkldr.L1_MARK_FULL, code[0][0], len(code[0][1]), zlib.crc32(code[0][1])))
        # the L1 code block in the stream holds exactly those bytes
        blocks = check_stream.ldr.parse(data)
        l1 = [b for b in blocks if b['target'] == addr]
        self.assertEqual(len(l1), 1)
        off = data.index(mkldr.header(0, addr, l1[0]['count'], 0)) + 16
        self.assertEqual(zlib.crc32(data[off:off + n]), crc)
        # the packed block, which the descriptor's CRC covers, holds the
        # filled mark
        main = elf32.Elf(fimage_marked())
        mkldr.mark_l1(main)
        self.assertEqual(unpacked(data)[1], [(a, bytes(b)) for a, b in mkldr.runs(main)
                                             if not mkldr.in_l1(a)])
        res = check_stream.check('bootloader', data, FIRST_STAGE_STACK)
        self.assertTrue(all(ok for _, ok, _ in res), res)

    def test_unmarked_unchanged(self):
        a = mkldr.merged(elf32.Elf(fimage()), elf32.Elf(front()), elf32.Elf(stub()))
        self.assertEqual(a, mkldr.merged(elf32.Elf(fimage()), elf32.Elf(front()), elf32.Elf(stub())))
        self.assertIsNone(self.stream_mark(a))

    def test_refused(self):
        with self.assertRaises(SystemExit):     # a mark already filled, or not the compiled one
            mkldr.merged(elf32.Elf(fimage_marked(word=mkldr.L1_MARK_FULL)), elf32.Elf(front()), elf32.Elf(stub()))
        far = ('.l1.far', elf32.SHT_PROGBITS, AX, 0x11A02000, b'\x77' * 8)
        with self.assertRaises(SystemExit):     # two runs in L1 code
            mkldr.merged(elf32.Elf(fimage_marked(extra=[far])), elf32.Elf(front()), elf32.Elf(stub()))


class Tmp:
    def __enter__(self):
        self.d = tempfile.TemporaryDirectory()
        return self

    def __exit__(self, *a):
        self.d.cleanup()

    def path(self, name):
        return os.path.join(self.d.name, name)

    def write(self, name, data):
        p = self.path(name)
        with open(p, 'wb') as f:
            f.write(data)
        return p


def run_layout(elf_path, *extra):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = layout.main(['layout.py', elf_path, '--ld', LD, '--board', BOARD,
                          '--flash', FLASH, '--memmap', MEMMAP] + list(extra))
    return rc, out.getvalue()


class Stream(unittest.TestCase):
    def setUp(self):
        self.elf = elf32.Elf(image())
        self.data = mkldr.stream(self.elf, '_init')
        self.blocks = check_stream.ldr.parse(self.data)

    def test_s01_bootloader_profile(self):
        res = check_stream.check('bootloader', self.data)
        self.assertTrue(all(ok for _, ok, _ in res), res)

    def test_first_final_init(self):
        b = self.blocks
        self.assertEqual(b[0]['flags'], check_stream.ldr.FIRST | check_stream.ldr.IGNORE)
        self.assertEqual(b[0]['target'], 0x11A00040)
        self.assertEqual(b[0]['argument'], len(self.data) - 16)
        self.assertEqual(b[-1]['flags'], check_stream.ldr.FINAL)
        self.assertEqual(b[-1]['target'], 0x11A00040)
        inits = [x for x in b if x['flags'] & check_stream.ldr.INIT]
        self.assertEqual(len(inits), 1)
        self.assertEqual((inits[0]['target'], inits[0]['count']), (0x11A00000, 0))

    def test_words_and_dma_code(self):
        for x in self.blocks:
            self.assertEqual(x['target'] % 4, 0)
            self.assertEqual(x['count'] % 4, 0)
            self.assertEqual(x['code'] & 0xF, 4)

    def test_loaded_bytes(self):
        mem = {}
        for x in self.blocks:
            if x['count'] and not x['flags'] & check_stream.ldr.IGNORE:
                p = x['payload_offset']
                for i, v in enumerate(self.data[p:p + x['count']]):
                    mem[x['target'] + i] = v
        for i, v in enumerate(TEXT):
            self.assertEqual(mem[0x8803D010 + i], v)
        self.assertEqual(mem[0x8803D02E], 0x55)
        self.assertEqual(mem[0x11A00040 + 0x21], 0x22)
        # Nothing for the sections that are not loaded.
        self.assertNotIn(0x11800008, mem)
        self.assertNotIn(0x11900000, mem)
        self.assertNotIn(0x8803D040, mem)

    def test_l1_before_init_rest_after(self):
        k = next(i for i, x in enumerate(self.blocks) if x['flags'] & check_stream.ldr.INIT)
        for x in self.blocks[1:k]:
            self.assertTrue(mkldr.in_l1(x['target']))
        for x in self.blocks[k + 1:-1]:
            self.assertFalse(mkldr.in_l1(x['target']))

    def test_missing_init_symbol(self):
        with self.assertRaises(SystemExit):
            mkldr.stream(self.elf, '_nope')


class InitStack(unittest.TestCase):
    """S-01.007: the Makefile's check_stack.py declaration matches the
    INIT entry that mkldr.py streams (the front's, src/front/fr_entry.S)."""
    ENTRY_S = os.path.join(TOP, 'src', 'front', 'fr_entry.S')
    DECL = os.path.join(TOP, 'src', 'front', 'fr_entry.stack.json')
    FRONT_LD = os.path.join(TOP, 'src', 'front', 'front.ld')

    def test_declaration_passes(self):
        with open(self.DECL) as f:
            decl = json.load(f)
        res = check_stack.check(decl)
        self.assertTrue(all(ok for _, ok, _ in res), res)
        self.assertEqual('_' + decl['entry'], '_fr_init')   # mkldr.py --front
        self.assertEqual(decl['frames'], {'fr_init': 0})

    def test_nothing_on_the_entry_stack(self):
        # Every instruction of _fr_init before SP moves leaves SP and
        # the memory below it alone and calls nothing.
        with open(self.ENTRY_S) as f:
            text = f.read()
        body = text.split('_fr_init:', 1)[1].split('SP.L =', 1)[0]
        lines = [x.split('/*')[0].strip() for x in body.splitlines()]
        lines = [x for x in lines if x]
        self.assertTrue(lines)
        for x in lines:
            self.assertNotRegex(x, r'\[\s*--\s*SP|SP\s*[-+]?=|\bLINK\b|\bCALL\b|\bRTS\b', x)

    def test_stack_in_l1_data_a_with_cplbs_off(self):
        # S-01.021: the stack section is .fr_init_stack, which
        # front.ld places in fr_l1data (0x11802000-0x11804000, so clear of
        # the first stage's 0x11807000-0x11808000), 1 KiB, 8-byte aligned
        # and not loaded; and L1DM_DCTL.ENCPLB (bit 1) is cleared before the
        # first store and the saved value written back before the RTS.
        with open(self.ENTRY_S) as f:
            text = f.read()
        self.assertRegex(text, r'\.section\s+\.fr_init_stack,\s*"aw",\s*@nobits\s*\n\s*\.p2align 3\s*\n'
                               r'_fr_init_stack:\s*\n\s*\.space 1024\s*\n_fr_init_stack_end:')
        self.assertNotIn('.l1.bss', text)
        with open(self.FRONT_LD) as f:
            ld = f.read()
        self.assertRegex(ld, r'\.fr_l1stack \(NOLOAD\) :\s*\{[^}]*KEEP\(\*\(\.fr_init_stack\)\)[^}]*\}\s*> fr_l1data')
        self.assertRegex(ld, r'fr_l1data \(rw\)\s+:\s+ORIGIN = 0x11802000, LENGTH = 0x2000')
        body = text.split('_fr_init:', 1)[1].split('.size _fr_init', 1)[0]
        before, after = body.split('SP.L =', 1)
        self.assertRegex(before, r'P0\.L = DCTL_LO;\s*\n\s*P0\.H = CTL_HI;\s*\n\s*SSYNC;\s*\n\s*R1 = \[P0\];'
                                 r'\s*\n\s*R2 = R1;\s*\n\s*BITCLR\(R2, ENCPLB_BIT\);\s*\n\s*\[P0\] = R2;'
                                 r'\s*\n\s*SSYNC;\s*\n\s*CSYNC;')
        self.assertLess(before.index('[P0] = R2;'), before.index('[P0] = SP;'))
        self.assertIn('#define CTL_HI     0x1FC0', text)
        self.assertIn('#define DCTL_LO    0x0004', text)
        self.assertIn('#define ENCPLB_BIT 1', text)
        tail = after.split('RETS = [SP++];', 1)[1]
        self.assertRegex(tail, r'R1 = \[P0\];\s*\n\s*P0\.L = DCTL_LO;\s*\n\s*P0\.H = CTL_HI;\s*\n\s*SSYNC;'
                               r'\s*\n\s*\[P0\] = R1;\s*\n\s*SSYNC;\s*\n\s*CSYNC;\s*\n\s*RTS;')
        self.assertLess(tail.index('SP = [P0];'), tail.index('RTS;'))
        self.assertNotRegex(tail.split('SP = [P0];', 1)[1], r'\[\s*--\s*SP|SP\s*[-+]?=|\bCALL\b')


class Layout(unittest.TestCase):
    def test_good(self):
        with Tmp() as t:
            e = t.write('a.elf', image())
            s = t.write('a.ldr', mkldr.stream(elf32.read(e), '_init'))
            rc, out = run_layout(e, '--stream', s, '--json', t.path('a.json'))
            self.assertEqual(rc, 0, out)
            with open(t.path('a.json')) as f:
                lay = json.load(f)
        res = check_layout.check(lay)
        self.assertTrue(all(ok for r, ok, _ in res if r not in check_layout.INFORMATIVE), res)
        kinds = {r['name']: (r['kind'], r['handover']) for r in lay['ranges']}
        self.assertEqual(kinds['.handover.text'], ('code', True))
        self.assertEqual(kinds['.handover.stack'], ('stack', True))
        roles = {r['role']: r['name'] for r in lay['ranges'] if 'role' in r}
        self.assertEqual(roles, {'kernel_stack': 'ho_stack', 'config_block': 'config_block',
                                 'header_buffer': 'header_buffer'})
        self.assertTrue(dict((r, ok) for r, ok, _ in res)['handover_stack'])
        self.assertEqual(kinds['heap'], ('heap', False))
        self.assertEqual(kinds['ramdisk_0'], ('held_buffer', False))
        self.assertEqual(kinds['ramdisk_1'], ('held_buffer', False))
        self.assertEqual(kinds['frame_buffers'], ('held_buffer', False))
        self.assertTrue(dict((r, ok) for r, ok, _ in res)['held_room'])

    def test_handover_reaching_out(self):
        with Tmp() as t:
            rc, out = run_layout(t.write('a.elf', image(handover_reloc='_l1_data')))
        self.assertEqual(rc, 1)
        self.assertIn('handover: .handover.text+0x4 -> 0x11800000', out)

    def test_handover_reaching_ddr_reserve(self):
        # opxy_hard_reset() on a failed kernel call, in the image's DDR
        with Tmp() as t:
            rc, out = run_layout(t.write('a.elf', image(handover_reloc='_far')))
        self.assertEqual(rc, 0, out)

    def test_ddr_into_heap(self):
        with Tmp() as t:
            rc, out = run_layout(t.write('a.elf', image(ddr_size=0x00300000)))
        self.assertEqual(rc, 1)
        self.assertIn('ddr overrun', out)
        self.assertIn('outside the DDR image range', out)

    def test_stream_over_slot(self):
        with Tmp() as t:
            e = t.write('a.elf', image())
            s = t.write('a.ldr', bytes(0x77001))
            rc, out = run_layout(e, '--stream', s)
        self.assertEqual(rc, 1)
        self.assertIn('a slot holds 487424 before its start log', out)
        # S-02.104: the build's stream also stops below the settings sector.
        with Tmp() as t:
            e = t.write('a.elf', image())
            rc, out = run_layout(e, '--stream', t.write('a.ldr', bytes(0x76001)))
        self.assertEqual(rc, 1)
        self.assertIn('a slot holds 483328 before its settings sector (S-02.104)', out)
        with Tmp() as t:
            e = t.write('a.elf', image())
            rc, out = run_layout(e, '--stream', t.write('a.ldr', bytes(0x76000)))
        self.assertNotIn('a slot holds', out)

    def test_section_in_the_front_frame(self):
        # S-01.023: a loaded section in the front's frame fails; a NOLOAD one does not.
        with Tmp() as t:
            e = t.write('a.elf', image(extra=[
                ('.ddr.data', elf32.SHT_PROGBITS, WA, 0x8FE347F0, b'\x6b' * 0x10)]))
            rc, out = run_layout(e)
        self.assertEqual(rc, 1)
        self.assertIn("meets the front's frame (S-01.023)", out)
        with Tmp() as t:
            e = t.write('a.elf', image(extra=[('.frame', elf32.SHT_NOBITS, WA, 0x8FE00000, 0x34800)]))
            rc, out = run_layout(e)
        self.assertNotIn("front's frame", out)

    def test_rom_boot_reserve(self):
        with Tmp() as t:
            e = t.write('a.elf', image(l2=0x08001FFC))
            s = t.write('a.ldr', mkldr.stream(elf32.read(e), '_init'))
            rc, out = run_layout(e, '--stream', s)
        self.assertEqual(rc, 1)
        self.assertIn('.l2.data at 0x08001ffc-0x08002004 meets the ROM boot reserve', out)

    def test_l2_above_the_front(self):
        # L2 from the ROM boot reserve up to 0x08031000 is the front's
        # (src/front/fr_handoff.h): the image's lowest L2 is just above it.
        with Tmp() as t:
            e = t.write('a.elf', image(l2=0x08031000))
            s = t.write('a.ldr', mkldr.stream(elf32.read(e), '_init'))
            rc, out = run_layout(e, '--stream', s)
        self.assertEqual(rc, 0, out)
        with Tmp() as t:
            e = t.write('a.elf', image(l2=0x08002000))
            rc, out = run_layout(e)
        self.assertEqual(rc, 1)
        self.assertIn('sections outside every region: .l2.data', out)

    def test_stream_rules_of_the_build(self):
        # The Makefile's check_stream.py call: the ROM boot reserve
        # (S-01.008) and the first stage's stack (S-01.009) refuse a block.
        def fails(l2):
            data = mkldr.stream(elf32.Elf(image(l2=l2)), '_init')
            res = check_stream.check('bootloader', data, FIRST_STAGE_STACK)
            return sorted(r for r, ok, _ in res if not ok)
        self.assertEqual(fails(0x08002000), [])
        self.assertEqual(fails(0x08001FFC), ['rom_boot_reserve'])
        self.assertEqual(fails(0x11806FFC), ['spares_first_stage_stack'])
        # S-01.020: the bootloader profile checks every block's BCODE.
        data = mkldr.stream(elf32.Elf(image()), '_init')
        res = dict((r, ok) for r, ok, _ in check_stream.check('bootloader', data))
        self.assertTrue(res['bcode'])

    def test_regions_from_link_map(self):
        names = [r[0] for r in layout.memory_regions(LD)]
        self.assertEqual(names, ['l1_code', 'l1_data_a', 'l1_data_b', 'l2', 'handover', 'ddr'])


L1_TEXT = b'\x22' * 0x60


def testos_image(ddr_at=0x89000000, extra=None, entry=None, l1_lma=True):
    """A test-OS image (bf703_testos.ld): everything loaded in DDR above the
    handover reserve, in one run of bytes, the entry _wb_testos_entry its first
    byte; .l1.text runs in L1 but is loaded in the DDR run (its LMA right after
    .data), as the entry shim copies it; no INIT stage. With l1_lma False the
    .l1.text section is loaded at its run address in L1 (the layout the
    test OS must not have)."""
    sections = [
        ('.text', elf32.SHT_PROGBITS, AX, ddr_at, TEXT),
        ('.rodata', elf32.SHT_PROGBITS, elf32.SHF_ALLOC, ddr_at + 0x20, b'\x55' * 3),
        ('.data', elf32.SHT_PROGBITS, WA, ddr_at + 0x24, b'\x33' * 16),
        ('.l1.text', elf32.SHT_PROGBITS, AX, 0x11A00000, L1_TEXT),
        ('.bss', elf32.SHT_NOBITS, WA, ddr_at + 0x100, 0x1000),
        ('.stack', elf32.SHT_NOBITS, WA, ddr_at + 0x2000, 0x1000),
    ]
    if extra:
        sections += extra
    symbols = [('_wb_testos_entry', ddr_at, '.text'), ('_main', ddr_at + 8, '.text'),
               ('_opxy_testos_entry', ddr_at + 0x24, '.data'), ('_wb_cplb_setup', 0x11A00000, '.l1.text')]
    relocs = {'.text': [(ddr_at + 4, '_opxy_testos_entry', 0)]}
    return build_elf(sections, symbols, relocs, ddr_at if entry is None else entry,
                     lma={'.l1.text': ddr_at + 0x34} if l1_lma else None)


def run_testos_layout(elf_path, *extra):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = testos_layout.main(['testos_layout.py', elf_path, '--ld', TESTOS_LD, '--board', BOARD,
                                 '--memmap', MEMMAP] + list(extra))
    return rc, out.getvalue()


class TestOs(unittest.TestCase):
    """The test OS's stream (mkldr.py --no-init) and layout (testos_layout.py)
    against S-01's os profile and S-05.024 (wiki/Test-OS.md)."""

    def test_stream_has_no_init_and_passes_os_profile(self):
        elf = elf32.Elf(testos_image())
        data = mkldr.stream(elf, None)
        blocks = check_stream.ldr.parse(data)
        self.assertEqual(blocks[0]['flags'], check_stream.ldr.FIRST | check_stream.ldr.IGNORE)
        self.assertEqual(blocks[0]['target'], 0x89000000)
        self.assertEqual(blocks[-1]['flags'], check_stream.ldr.FINAL)
        self.assertFalse([b for b in blocks if b['flags'] & check_stream.ldr.INIT])
        # S-05.024's flags: only FIRST, IGNORE and FINAL appear
        for b in blocks:
            self.assertFalse(b['flags'] & ~(check_stream.ldr.FIRST | check_stream.ldr.IGNORE |
                                           check_stream.ldr.FINAL) & 0xFFF0)
        res = check_stream.check('os', data)
        self.assertTrue(all(ok for _, ok, _ in res), res)
        testos_layout.kernel_model.compatibility_check(data)
        # one data block, the DDR image, with .l1.text's bytes at its load address
        self.assertEqual([(b['flags'], b['target'], b['count']) for b in blocks[1:-1]],
                         [(0, 0x89000000, 0x94)])
        self.assertEqual(data[32 + 0x34:32 + 0x34 + 0x60], L1_TEXT)
        self.assertEqual(testos_layout.check_blocks(data, (0x89000000, 0x89300000)), [])

    def test_section_load_addresses(self):
        """elf32 reads a section's load address from the program headers: .l1.text runs in L1 and
        loads in DDR; the rest, with no header, load where they run."""
        elf = elf32.Elf(testos_image())
        l1 = elf.section('.l1.text')
        self.assertEqual((l1.addr, l1.lma, l1.lma_end), (0x11A00000, 0x89000034, 0x89000094))
        self.assertEqual([(s.name, s.lma == s.addr) for s in elf.sections if s.alloc and s.size],
                         [('.text', True), ('.rodata', True), ('.data', True), ('.l1.text', False),
                          ('.bss', True), ('.stack', True)])
        self.assertEqual(elf.segments, [(l1.offset, 0x11A00000, 0x89000034, 0x60, 0x60)])
        self.assertEqual(elf32.Elf(image()).segments, [])
        # a stream of the same image with .l1.text loaded in L1: an L1 block
        data = mkldr.stream(elf32.Elf(testos_image(l1_lma=False)), None)
        blocks = check_stream.ldr.parse(data)
        self.assertEqual([(b['target'], b['count']) for b in blocks[1:-1]], [(0x11A00000, 0x60), (0x89000000, 0x34)])
        fails = testos_layout.check_blocks(data, (0x89000000, 0x89300000))
        self.assertTrue(any('is in L1 or L2' in f for f in fails), fails)
        self.assertTrue(any('2 data blocks, wanted one' in f for f in fails), fails)

    def test_no_init_drops_only_the_init_block(self):
        elf = elf32.Elf(image())
        with_init = check_stream.ldr.parse(mkldr.stream(elf, '_init'))
        without = check_stream.ldr.parse(mkldr.stream(elf, None))
        kept = [(b['flags'], b['target'], b['count']) for b in with_init
                if not b['flags'] & check_stream.ldr.INIT]
        self.assertEqual([(b['flags'], b['target'], b['count']) for b in without], kept)
        self.assertEqual(len(with_init), len(without) + 1)

    def test_entry_in_first_and_final_blocks(self):
        """S-05.013 by every reading: the FIRST target (RCU0_SVECT0 as the kernel
        sets it), the FINAL target (kernel_model.py) and, with --no-init, the
        FINAL argument are the entry; the bootloader's FINAL argument stays 0."""
        data = mkldr.stream(elf32.Elf(testos_image()), None)
        first, final, arg = testos_layout.stream_entries(data)
        self.assertEqual((first, final, arg), (0x89000000, 0x89000000, 0x89000000))
        self.assertEqual(testos_layout.kernel_model.load(data, 0x0803C000)['svect0'], 0x89000000)
        normal = mkldr.stream(elf32.Elf(image()), '_init')
        self.assertEqual(testos_layout.stream_entries(normal)[2], 0)
        self.assertIsNone(testos_layout.stream_entries(data[:-16]))

    def test_layout_checks_the_entry(self):
        """The ELF entry must be the first byte of the DDR image, OPXY_TESTOS_IMAGE_START, where
        the lowest loaded byte is, and the stream's FIRST and FINAL blocks must name it."""
        good = testos_image()
        with Tmp() as t:
            rc, out = run_testos_layout(t.write('m.elf', testos_image(entry=0x89000010)))
            self.assertEqual(rc, 1)
            self.assertIn('the ELF entry 0x89000010 is not the first byte of the DDR image 0x89000000', out)
            # the entry right, but something loaded below it
            below = testos_image(extra=[('.early', elf32.SHT_PROGBITS, AX, 0x88FFF000, b'\x11' * 8)])
            rc, out = run_testos_layout(t.write('b.elf', below))
            self.assertEqual(rc, 1)
            self.assertIn('the lowest loaded byte 0x88fff000 is not the entry 0x89000000', out)
            e = t.write('g.elf', good)
            data = bytearray(mkldr.stream(elf32.read(e), None))
            # the FINAL block's target moved, its checksum fixed
            h = bytearray(data[-16:])
            h[4:8] = struct.pack('<I', 0x89000040)
            h[2] = 0
            x = 0
            for b in h:
                x ^= b
            h[2] = x
            data[-16:] = h
            rc, out = run_testos_layout(e, '--stream', t.write('g.ldr', bytes(data)))
            self.assertEqual(rc, 1)
            self.assertIn("FINAL block target 0x89000040 is not the entry 0x89000000", out)
            rc, out = run_testos_layout(e, '--stream', t.write('ok.ldr', mkldr.stream(elf32.read(e), None)))
            self.assertEqual(rc, 0, out)
            self.assertIn('entry      0x89000000', out)
            self.assertIn('.l1.text             0x11a00000       96 (loaded at 0x89000034)', out)

    def test_layout_refuses_a_stream_over_the_staging_area(self):
        """A test OS's stream is written as os.ldr: one longer than 0x800000 bytes, the most
        a unit loads (S-01.014, S-03.042), fails every variant's layout check."""
        with Tmp() as t:
            e = t.write('g.elf', testos_image())
            data = mkldr.stream(elf32.read(e), None)
            big = data + bytes(0x800001 - len(data))
            rc, out = run_testos_layout(e, '--stream', t.write('big.ldr', big))
            self.assertEqual(rc, 1)
            self.assertIn('the stream would be refused by S-05.024: image longer than the staging area',
                          out)
            self.assertEqual(testos_layout.check_stream_file(t.write('ok.ldr', data)), [])

    def test_layout_refuses_l1_and_l2_loads(self):
        """No block of the stream may have an L1 or L2 destination (a loading bootloader's
        hand-over may run there): a section loaded in L1 or in L2 fails the layout
        and the stream check."""
        with Tmp() as t:
            e = t.write('l1.elf', testos_image(l1_lma=False))
            rc, out = run_testos_layout(e, '--stream', t.write('l1.ldr', mkldr.stream(elf32.read(e), None)))
            self.assertEqual(rc, 1)
            self.assertIn('.l1.text loads at 0x11a00000-0x11a00060, outside the DDR image range', out)
            self.assertIn('block destination 0x11a00000-0x11a00060 is in L1 or L2', out)
            self.assertIn('2 data blocks, wanted one', out)
            self.assertIn('the lowest loaded byte 0x11a00000 is not the entry 0x89000000', out)
            extra = [('.l2.data', elf32.SHT_PROGBITS, WA, 0x08002000, b'\x66' * 8)]
            e = t.write('l2.elf', testos_image(extra=extra))
            rc, out = run_testos_layout(e, '--stream', t.write('l2.ldr', mkldr.stream(elf32.read(e), None)))
            self.assertEqual(rc, 1)
            self.assertIn('.l2.data loads at 0x08002000-0x08002008, outside the DDR image range', out)
            self.assertIn('block destination 0x08002000-0x08002008 is in L1 or L2', out)
            # a second DDR block, apart from the image
            extra = [('.far', elf32.SHT_PROGBITS, WA, 0x89200000, b'\x66' * 8)]
            e = t.write('far.elf', testos_image(extra=extra))
            rc, out = run_testos_layout(e, '--stream', t.write('far.ldr', mkldr.stream(elf32.read(e), None)))
            self.assertEqual(rc, 1)
            self.assertIn('2 data blocks, wanted one', out)
            self.assertNotIn('is in L1 or L2', out)

    def test_layout_good(self):
        with Tmp() as t:
            e = t.write('t.elf', testos_image())
            s = t.write('t.ldr', mkldr.stream(elf32.read(e), None))
            rc, out = run_testos_layout(e, '--stream', s, '--json', t.path('t.json'))
            self.assertEqual(rc, 0, out)
            with open(t.path('t.json')) as f:
                lay = json.load(f)
        res = check_layout.check(lay)
        self.assertTrue(all(ok for r, ok, _ in res if r not in check_layout.INFORMATIVE), res)
        kinds = {r['name']: (r['kind'], r['handover']) for r in lay['ranges']}
        self.assertEqual(kinds['.text'], ('code', False))
        self.assertEqual(kinds['.l1.text'], ('code', False))
        self.assertEqual(kinds['.stack'], ('stack', False))
        self.assertEqual(kinds['heap'], ('heap', False))
        self.assertFalse(any(h for _, h in kinds.values()))

    def test_layout_in_the_reserve(self):
        with Tmp() as t:
            rc, out = run_testos_layout(t.write('t.elf', testos_image(ddr_at=0x8803D000)))
        self.assertEqual(rc, 1)
        self.assertIn('meets the DDR handover reserve', out)
        self.assertIn('outside the test OS image range', out)

    def test_layout_refuses_init_stage(self):
        extra = [('.init.text', elf32.SHT_PROGBITS, AX, 0x11A01000, b'\x11' * 0x10)]
        with Tmp() as t:
            rc, out = run_testos_layout(t.write('t.elf', testos_image(extra=extra)))
        self.assertEqual(rc, 1)
        self.assertIn('an INIT stage in an OS image', out)

    def test_layout_not_loaded_section_in_staging(self):
        extra = [('.scratch', elf32.SHT_NOBITS, WA, 0x8A000000, 0x100)]
        with Tmp() as t:
            rc, out = run_testos_layout(t.write('t.elf', testos_image(extra=extra)))
        self.assertEqual(rc, 1)
        self.assertIn('meets the staging area', out)

    def test_layout_record_and_l2_reserve(self):
        extra = [('.rec', elf32.SHT_PROGBITS, WA, 0x11B01F00, b'\x77' * 8),
                 ('.l2', elf32.SHT_PROGBITS, WA, 0x0803C000, b'\x77' * 8)]
        with Tmp() as t:
            rc, out = run_testos_layout(t.write('t.elf', testos_image(extra=extra)))
        self.assertEqual(rc, 1)
        self.assertIn('meets the reboot record', out)
        self.assertIn('meets the L2 handover reserve', out)

    def test_stream_with_refused_flag(self):
        elf = elf32.Elf(testos_image())
        data = bytearray(mkldr.stream(elf, None))
        # the first data block's header: set CALLBACK (bit 13) and fix the checksum
        h = bytearray(data[16:32])
        h[1] |= 0x20
        h[2] = 0
        x = 0
        for b in h:
            x ^= b
        h[2] = x
        data[16:32] = h
        with Tmp() as t:
            e = t.write('t.elf', testos_image())
            s = t.write('t.ldr', bytes(data))
            rc, out = run_testos_layout(e, '--stream', s)
        self.assertEqual(rc, 1)
        self.assertIn('refused by S-05.024', out)

    def test_regions_from_link_map(self):
        names = [r[0] for r in layout.memory_regions(TESTOS_LD)]
        self.assertEqual(names, ['l1_code', 'ddr'])
        ddr = [r for r in layout.memory_regions(TESTOS_LD) if r[0] == 'ddr'][0]
        board = layout.defines(BOARD)
        self.assertEqual((ddr[1], ddr[2]), (board['OPXY_TESTOS_IMAGE_START'], board['OPXY_TESTOS_IMAGE_END']))
        s01 = layout.defines(MEMMAP)
        self.assertEqual(board['OPXY_TESTOS_IMAGE_START'], 0x89000000)     # well above the handover reserve
        self.assertGreater(board['OPXY_TESTOS_IMAGE_START'], s01['S01_RESERVE_DDR_END'])
        self.assertEqual(board['OPXY_TESTOS_HEAP_END'], s01['S01_STAGING_START'])

    def test_register_table(self):
        rows = testos_regs.rows()
        regs = {r[0] for r in rows}
        self.assertTrue({'CGU0_CTL', 'CGU0_DIV', 'DMC0_CFG', 'DMC0_TR0'} <= regs, regs)
        self.assertNotIn('RTC0_INIT', regs)          # a HELD-only row
        src = testos_regs.c_source(rows)
        self.assertIn('const unsigned opxy_testos_regs_n = %d;' % len(rows), src)
        self.assertEqual(src.count('OPXY_TESTOS_RULE_'), len(rows))
        self.assertIn('"S-13a.003"', src)
        tr1 = [r for r in rows if r[0] == 'DMC0_TR1' and r[2] == 'TREF'][0]
        self.assertEqual((tr1[5], tr1[6], tr1[7]), ('in', 1, 1560))


TESTOS_1F_LD = os.path.join(TOP, 'src', 'testos', 'bf703_installer.ld')
STUB = bytes(range(0x40, 0x98))                 # 88 bytes, eleven 8-byte words


def testos_1f_image(stub=STUB, ddr_at=0x8D000000, extra=None, entry=0x11A00000, symbols_extra=()):
    """An installer image (bf703_installer.ld): the entry stub loaded at 0x11A00000, its one L1
    block, and the test OS's DDR run at 0x8D000000 with .l1.text loaded in it."""
    sections = [
        ('.l1.entry', elf32.SHT_PROGBITS, AX, 0x11A00000, stub),
        ('.text', elf32.SHT_PROGBITS, AX, ddr_at, TEXT),
        ('.rodata', elf32.SHT_PROGBITS, elf32.SHF_ALLOC, ddr_at + 0x20, b'\x55' * 3),
        ('.data', elf32.SHT_PROGBITS, WA, ddr_at + 0x24, b'\x33' * 16),
        ('.l1.text', elf32.SHT_PROGBITS, AX, 0x11A00100, L1_TEXT),
        ('.bss', elf32.SHT_NOBITS, WA, ddr_at + 0x100, 0x1000),
        ('.stack', elf32.SHT_NOBITS, WA, ddr_at + 0x2000, 0x1000),
    ]
    if extra:
        sections += extra
    symbols = [('_wb_testos_l1_entry', 0x11A00000, '.l1.entry'), ('_wb_testos_entry', ddr_at, '.text'),
               ('_main', ddr_at + 8, '.text'), ('_opxy_testos_entry', ddr_at + 0x24, '.data'),
               ('_wb_cplb_setup', 0x11A00100, '.l1.text')]
    symbols += [(n, ddr_at + 0x10 + 2 * i, '.text') for i, n in enumerate(symbols_extra)]
    relocs = {'.text': [(ddr_at + 4, '_opxy_testos_entry', 0)]}
    return build_elf(sections, symbols, relocs, entry, lma={'.l1.text': ddr_at + 0x34})


def run_testos_layout_1f(elf_path, *extra):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = testos_layout.main(['testos_layout.py', elf_path, '--ld', TESTOS_1F_LD, '--board', BOARD,
                                 '--memmap', MEMMAP, '--device', 'both'] + list(extra))
    return rc, out.getvalue()


def fix_header(h):
    """The XOR of a block header's 16 bytes made zero again."""
    h = bytearray(h)
    h[2] = 0
    x = 0
    for b in h:
        x ^= b
    h[2] = x
    return bytes(h)


class TestOs1F(unittest.TestCase):
    """The installer's images, one for both devices (make installer, S-05.043): mkldr.py --os-1f and
    testos_layout.py --device both against S-05.040's shape of the OP-1F's OS 1.7.3, S-01's os profile,
    S-05.024 and both devices' RAM disks."""

    def test_stream_shape(self):
        data = mkldr.stream(elf32.Elf(testos_1f_image()), None, 0, mkldr.OS_1F_SPLIT)
        blocks = check_stream.ldr.parse(data)
        self.assertEqual([(b['flags'], b['target'], b['count'], b['argument']) for b in blocks],
                         [(check_stream.ldr.FIRST | check_stream.ldr.IGNORE, 0x11A00000, 0, len(data) - 16),
                          (0, 0x11A00000, len(STUB), 0), (0, 0x8D000000, 0x94, 0),
                          (check_stream.ldr.FINAL, 0x11A00000, 0, 0)])
        # S-05.041: a run over the limit becomes consecutive blocks no longer, each a multiple of 4
        cut = mkldr.stream(elf32.Elf(testos_1f_image()), None, 0, (0xC000, 0x40))
        self.assertEqual([(b['target'], b['count']) for b in check_stream.ldr.parse(cut)][2:-1],
                         [(0x8D000000, 0x40), (0x8D000040, 0x40), (0x8D000080, 0x14)])
        self.assertEqual(testos_layout.check_blocks_1f(cut, (0x8D000000, 0x8D300000)), [])
        # the limits and the first block's argument are checked
        keep = testos_layout.DDR_BLOCK_MAX_1F
        try:
            testos_layout.DDR_BLOCK_MAX_1F = 0x40
            self.assertTrue(any('over 0x40' in f for f in testos_layout.check_blocks_1f(
                data, (0x8D000000, 0x8D300000))))
        finally:
            testos_layout.DDR_BLOCK_MAX_1F = keep
        bad = bytearray(data)
        bad[12:16] = struct.pack('<I', len(data))
        self.assertTrue(any('length less 16' in f for f in testos_layout.check_blocks_1f(
            bytes(bad), (0x8D000000, 0x8D300000))))
        self.assertTrue(all(ok for _, ok, _ in check_stream.check('os', data)))
        testos_layout.kernel_model.compatibility_check(data)
        self.assertEqual(testos_layout.check_blocks_1f(data, (0x8D000000, 0x8D300000)), [])
        # every destination inside S-05.040's ranges
        ranges = testos_layout.os_1f_ranges()
        for b in blocks:
            if b['count']:
                self.assertTrue(any(lo <= b['target'] and b['target'] + b['count'] <= hi
                                    for lo, hi in ranges), hex(b['target']))
        # the OP-XY's --no-init form keeps the entry as the FINAL argument
        self.assertEqual(testos_layout.stream_entries(mkldr.stream(elf32.Elf(testos_1f_image()), None))[2],
                         0x11A00000)

    def test_survey_holds_no_flash_write(self):
        """--survey (make installer-survey): the flash driver's real erase and program gone from the
        link; the refusing wraps may be there or gone."""
        wraps = ('___wrap_wb_spiflash_erase', '___wrap_wb_spiflash_program')
        real = ('_wb_spiflash_erase', '_wb_spiflash_program')
        with Tmp() as t:
            for extra in ((), wraps):
                rc, out = run_testos_layout_1f(t.write('good.elf', testos_1f_image(symbols_extra=extra)),
                                               '--survey')
                self.assertEqual(rc, 0, out)
            # the plain installer keeps both and is not held to it
            self.assertEqual(run_testos_layout_1f(t.write('plain.elf', testos_1f_image(symbols_extra=real)))[0], 0)
            for name in real:
                bad = t.write('real.elf', testos_1f_image(symbols_extra=wraps + (name,)))
                rc, out = run_testos_layout_1f(bad, '--survey')
                self.assertEqual(rc, 1)
                self.assertIn('FAIL the survey still holds %s' % name, out)

    def test_main_mode(self):
        with Tmp() as t:
            e = t.write('t.elf', testos_1f_image())
            self.assertEqual(mkldr.main(['mkldr.py', e, t.path('t.ldr'), '--os-1f']), 0)
            with open(t.path('t.ldr'), 'rb') as f:
                data = f.read()
            self.assertEqual(testos_layout.stream_entries(data), (0x11A00000, 0x11A00000, 0))
            rc, out = run_testos_layout_1f(e, '--stream', t.path('t.ldr'), '--json', t.path('t.json'))
            self.assertEqual(rc, 0, out)
            self.assertIn('entry      0x11a00000', out)
            with open(t.path('t.json')) as f:
                names = [r['name'] for r in json.load(f)['ranges']]
            self.assertEqual(names[-6:], ['ramdisk_0', 'ramdisk_1', 'ramdisk_2', 'ramdisk_1f', 'os_frame',
                                          'frame_buffers'])
            # the one layout passes S-01's rules for each device's RAM disk (S-01.016, S-18.030)
            with open(t.path('t.json')) as f:
                lay = json.load(f)
            for device in ('xy', '1f'):
                res = check_layout.check(lay, device)
                self.assertTrue(all(ok for rule, ok, _ in res if rule not in check_layout.INFORMATIVE),
                                (device, res))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(mkldr.main(['mkldr.py', e, t.path('u.ldr'), '--os-1f', '--no-init']), 2)

    def test_link_map_and_board(self):
        regions = layout.memory_regions(TESTOS_1F_LD)
        self.assertEqual([r[0] for r in regions], ['l1_entry', 'l1_code', 'ddr'])
        board = layout.defines(BOARD)
        ddr = [r for r in regions if r[0] == 'ddr'][0]
        self.assertEqual((ddr[1], ddr[2]), (board['OPXY_TESTOS_SHARED_IMAGE_START'],
                                            board['OPXY_TESTOS_SHARED_IMAGE_END']))
        self.assertEqual(regions[0][1], 0x11A00000)
        # inside OS 1.7.3's second DDR range, above the OS frame, clear of the OP-1F's RAM disk
        lo, hi = testos_layout.os_1f_ranges()[-1]
        self.assertTrue(lo <= board['OPXY_TESTOS_SHARED_IMAGE_START'] and board['OPXY_TESTOS_SHARED_IMAGE_END'] <= hi)
        self.assertGreaterEqual(board['OPXY_TESTOS_SHARED_IMAGE_START'], board['OPXY_OS_FRAME_END'])
        self.assertGreaterEqual(board['OPXY_TESTOS_SHARED_IMAGE_START'], board['OPXY_1F_DISK_END'])
        self.assertLessEqual(board['OPXY_TESTOS_SHARED_HEAP_END'], board['OPXY_FRAMES_START'])
        # S-05.043: on the OP-XY the RAM disk stays clear of 0x8D000000-0x8E000000 and still holds
        # S-12.001's sectors
        self.assertEqual((board['OPXY_TESTOS_SHARED_IMAGE_START'], board['OPXY_TESTOS_SHARED_HEAP_END']),
                         (0x8D000000, 0x8E000000))
        parts = testos_layout.xy_shared_disk(board)
        self.assertFalse(any(layout.meets(lo, hi, (0x8D000000, 0x8E000000)) for _, lo, hi in parts))
        self.assertEqual(sum(hi - lo for _, lo, hi in parts), 454656 * 512)
        self.assertGreaterEqual(parts[1][1], layout.defines(MEMMAP)['S01_RESERVE_DDR_END'])

    def test_refusals(self):
        with Tmp() as t:
            # an entry stub of no whole 8-byte words
            e = t.write('odd.elf', testos_1f_image(stub=STUB[:84]))
            s = t.write('odd.ldr', mkldr.stream(elf32.read(e), None, 0))
            rc, out = run_testos_layout_1f(e, '--stream', s)
            self.assertEqual(rc, 1)
            self.assertIn('wanted one L1 code block at 0x11a00000 of whole 8-byte words', out)
            # the DDR image outside OS 1.7.3's ranges (below the OS frame's rows)
            e = t.write('low.elf', testos_1f_image(ddr_at=0x8C000000))
            s = t.write('low.ldr', mkldr.stream(elf32.read(e), None, 0))
            rc, out = run_testos_layout_1f(e, '--stream', s)
            self.assertEqual(rc, 1)
            self.assertIn("in none of OS 1.7.3's ranges (S-05.040)", out)
            self.assertIn('outside the test OS image range', out)
            # the FINAL argument the entry, as --no-init writes it
            e = t.write('g.elf', testos_1f_image())
            s = t.write('n.ldr', mkldr.stream(elf32.read(e), None))
            rc, out = run_testos_layout_1f(e, '--stream', s)
            self.assertEqual(rc, 1)
            self.assertIn('FINAL block argument 0x11a00000 is not 0x00000000', out)
            # the ELF entry elsewhere
            e = t.write('ent.elf', testos_1f_image(entry=0x8D000000))
            rc, out = run_testos_layout_1f(e)
            self.assertEqual(rc, 1)
            self.assertIn('the ELF entry 0x8d000000 is not 0x11a00000', out)
            # a flag OS 1.7.3 does not use (CALLBACK) on the DDR block
            data = bytearray(mkldr.stream(elf32.read(t.path('g.elf')), None, 0))
            at = 16 + 16 + len(STUB)
            h = bytearray(data[at:at + 16])
            h[1] |= 0x04
            data[at:at + 16] = fix_header(h)
            rc, out = run_testos_layout_1f(t.path('g.elf'), '--stream', t.write('cb.ldr', bytes(data)))
            self.assertEqual(rc, 1)
            self.assertIn('OS 1.7.3 uses FIRST, IGNORE, FILL, FINAL', out)
            # a second L1 block
            extra = [('.l1.more', elf32.SHT_PROGBITS, AX, 0x11A08000, b'\x11' * 8)]
            e = t.write('two.elf', testos_1f_image(extra=extra))
            s = t.write('two.ldr', mkldr.stream(elf32.read(e), None, 0))
            rc, out = run_testos_layout_1f(e, '--stream', s)
            self.assertEqual(rc, 1)
            self.assertIn('wanted one L1 code block', out)


def make_png(width, height, ctype, depth, rows, plte=None, trns=None, interlace=0, filt=0):
    """A PNG file; rows are each row's bytes, written with filter type filt."""
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''.join(bytes((filt,)) + r for r in rows)
    if filt == 1:                               # Sub on 8-bit samples of bpp bytes
        bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype] * depth // 8
        raw = b''
        for r in rows:
            raw += bytes((1,)) + bytes((r[k] - (r[k - bpp] if k >= bpp else 0)) & 0xFF for k in range(len(r)))
    out = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, depth, ctype, 0, 0,
                                                              interlace))
    if plte:
        out += chunk(b'PLTE', plte)
    if trns:
        out += chunk(b'tRNS', trns)
    return out + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')


def shown(png):
    """The frame's visible pixels as png2logo's records decode, by screen position: {(x, y): rgb565}."""
    data = png2logo.encode(png2logo.frame_pixels(png2logo.read_png(png)))
    px = png2logo.decode(data, 480 * 222)
    return lambda x, y: px[(479 - x) * 222 + y]


class Png2Logo(unittest.TestCase):
    W, H = 480, 222

    def rgb_rows(self, color, marks=()):
        rows = [bytearray(bytes(color) * self.W) for _ in range(self.H)]
        for (x, y), c in marks:
            rows[y][3 * x:3 * x + 3] = bytes(c)
        return [bytes(r) for r in rows]

    def test_positions(self):
        rows = self.rgb_rows((0, 0, 0), [((0, 0), (255, 0, 0)), ((479, 221), (0, 255, 0)), ((10, 200), (0, 0, 255))])
        for filt in (0, 1):
            px = shown(make_png(self.W, self.H, 2, 8, rows, filt=filt))
            self.assertEqual(px(0, 0), 0xF800)
            self.assertEqual(px(479, 221), 0x07E0)
            self.assertEqual(px(10, 200), 0x001F)
            self.assertEqual(px(1, 0), 0)

    def test_rounding(self):
        # 8C8C8C and 5A5A5A are the style guide's 8C71 and 5ACB
        for c, want in (((0x8C, 0x8C, 0x8C), 0x8C71), ((0x5A, 0x5A, 0x5A), 0x5ACB), ((0xFF, 0xB0, 0x20), 0xFD84)):
            self.assertEqual(shown(make_png(self.W, self.H, 2, 8, self.rgb_rows(c)))(5, 5), want)

    def test_alpha_on_black(self):
        row = bytes((255, 255, 255, 128)) * self.W        # white at half alpha: half gray
        self.assertEqual(shown(make_png(self.W, self.H, 6, 8, [row] * self.H))(0, 0),
                         png2logo.rgb565((128, 128, 128, 255, 255)))
        row16 = struct.pack('>HHHH', 65535, 0, 0, 0) * self.W   # transparent red: black
        self.assertEqual(shown(make_png(self.W, self.H, 6, 16, [row16] * self.H))(3, 3), 0)

    def test_palette_and_gray(self):
        plte = bytes((0, 0, 0, 255, 255, 255, 0, 0, 255))
        row = bytes((0x12,)) * (self.W // 2)            # 4-bit indices 1, 2
        px = shown(make_png(self.W, self.H, 3, 4, [row] * self.H, plte=plte, trns=bytes((255, 255, 0))))
        self.assertEqual((px(0, 0), px(1, 0)), (0xFFFF, 0))   # index 2 transparent: black
        gray = bytes((0xAA,)) * (self.W // 4)           # 2-bit gray 2, 2, 2, 2: 2/3 of white
        self.assertEqual(shown(make_png(self.W, self.H, 0, 2, [gray] * self.H))(7, 7),
                         png2logo.rgb565((2, 2, 2, 3, 3)))

    def test_refused(self):
        bad = [(make_png(479, 222, 2, 8, [bytes(3 * 479)] * 222), '479 x 222'),
               (make_png(self.W, self.H, 2, 8, self.rgb_rows((0, 0, 0)), interlace=1), 'interlac'),
               (b'GIF89a', 'not a PNG')]
        noisy = [bytes((x * 7 + y * 13) & 0xFF for x in range(3 * self.W)) for y in range(self.H)]
        bad.append((make_png(self.W, self.H, 2, 8, noisy), 'fewer colors'))
        for png, msg in bad:
            with self.assertRaises(png2logo.PngError) as e:
                png2logo.convert(png, 'x.png')
            self.assertIn(msg, str(e.exception))

    def test_records(self):
        pixels = [0] * 300 + [1, 2, 3] + [5] * 129 + list(range(200)) + [7, 7]
        data = png2logo.encode(pixels)
        self.assertEqual(png2logo.decode(data, len(pixels)), pixels)
        self.assertEqual(data[:3], bytes((0xFF, 0, 0)))  # 128 black pixels in one record
        self.assertEqual(png2logo.decode(data[:-1], len(pixels))[-2:], [0, 0])   # cut short: black

    def test_c_source(self):
        src = png2logo.convert(make_png(self.W, self.H, 2, 8, self.rgb_rows((0, 0, 0))), 'logo.png')
        self.assertIn('const struct wb_display_image wb_display_boot_image = {', src)
        self.assertIn('from logo.png', src)
        n = int(src.split('data[')[1].split(']')[0])
        self.assertEqual(n, 3 * (480 * 222 // 128) + 3)   # black in repeats of 128 and one of 64


if __name__ == '__main__':
    unittest.main(verbosity=1)
