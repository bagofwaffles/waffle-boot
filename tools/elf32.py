# SPDX-License-Identifier: GPL-2.0-or-later
"""A minimal reader of little-endian ELF32 files (the bfin-elf link output):
section headers, symbols, RELA relocations and each section's load address
(from the PT_LOAD program headers, for a section the link script gives an
LMA apart from its run address). Python 3, standard library only.

Format from the System V ABI (ELF): file header, program header table,
section header table, SHT_SYMTAB entries and SHT_RELA entries.
"""
import struct

SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4

SHN_UNDEF = 0
SHN_ABS = 0xFFF1

PT_LOAD = 1


class ElfError(ValueError):
    pass


class Section:
    def __init__(self, name, type_, flags, addr, offset, size, link, info, entsize):
        self.name, self.type, self.flags = name, type_, flags
        self.addr, self.offset, self.size = addr, offset, size
        self.link, self.info, self.entsize = link, info, entsize
        self.lma = addr        # the load address; Elf sets it from the program headers

    @property
    def alloc(self):
        return bool(self.flags & SHF_ALLOC)

    @property
    def end(self):
        return self.addr + self.size

    @property
    def lma_end(self):
        return self.lma + self.size


class Elf:
    def __init__(self, data):
        if data[:4] != b'\x7fELF':
            raise ElfError('not an ELF file')
        if data[4] != 1 or data[5] != 1:
            raise ElfError('not a little-endian ELF32 file')
        self.data = data
        (self.type, self.machine, _, self.entry, phoff, shoff, _, _, phentsize, phnum,
         shentsize, shnum, shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
        if shentsize != 40:
            raise ElfError('unexpected section header size %d' % shentsize)
        if phnum and phentsize != 32:
            raise ElfError('unexpected program header size %d' % phentsize)
        self.segments = []      # [(offset, vaddr, paddr, filesz, memsz)] of the PT_LOAD headers
        for i in range(phnum):
            p_type, off, vaddr, paddr, filesz, memsz, _, _ = struct.unpack_from(
                '<IIIIIIII', data, phoff + i * 32)
            if p_type == PT_LOAD:
                self.segments.append((off, vaddr, paddr, filesz, memsz))
        raw = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * 40) for i in range(shnum)]
        names = raw[shstrndx]
        self.sections = []
        for r in raw:
            name = self._str(names[4], r[0])
            self.sections.append(Section(name, r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[9]))
        for s in self.sections:
            if s.alloc:
                s.lma = self._lma(s)
        self._symbols = None

    def _lma(self, s):
        """The load address of an allocated section: the PT_LOAD segment that holds its run
        address gives paddr - vaddr; without program headers (or a segment) it is the run
        address, as for every section of a link whose LMAs equal its VMAs."""
        for off, vaddr, paddr, filesz, memsz in self.segments:
            if vaddr <= s.addr and s.end <= vaddr + memsz and \
                    (s.type == SHT_NOBITS or (off <= s.offset and s.offset + s.size <= off + filesz)):
                return paddr + (s.addr - vaddr)
        return s.addr

    def _str(self, table_off, i):
        end = self.data.index(b'\0', table_off + i)
        return self.data[table_off + i:end].decode('ascii', 'replace')

    def section(self, name):
        for s in self.sections:
            if s.name == name:
                return s
        return None

    def contents(self, s):
        if s.type == SHT_NOBITS:
            return bytes(s.size)
        return bytes(self.data[s.offset:s.offset + s.size])

    def symbols(self):
        """[(name, value, size, shndx)] of the first SHT_SYMTAB."""
        if self._symbols is None:
            tab = next((s for s in self.sections if s.type == SHT_SYMTAB), None)
            out = []
            if tab is not None:
                strtab = self.sections[tab.link]
                for i in range(tab.size // 16):
                    name, value, size, _, _, shndx = struct.unpack_from(
                        '<IIIBBH', self.data, tab.offset + i * 16)
                    out.append((self._str(strtab.offset, name), value, size, shndx))
            self._symbols = out
        return self._symbols

    def symbol(self, name):
        for n, v, _, shndx in self.symbols():
            if n == name and shndx != SHN_UNDEF:
                return v
        return None

    def relocations(self, s):
        """[(offset, type, symbol index, addend)] of a SHT_RELA section."""
        if s.type != SHT_RELA:
            return []
        out = []
        for i in range(s.size // 12):
            off, info, add = struct.unpack_from('<IIi', self.data, s.offset + i * 12)
            out.append((off, info & 0xFF, info >> 8, add))
        return out


def read(path):
    with open(path, 'rb') as f:
        return Elf(f.read())
