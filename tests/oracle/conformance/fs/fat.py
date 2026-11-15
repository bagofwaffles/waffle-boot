# SPDX-License-Identifier: GPL-2.0-or-later
"""FAT12/FAT16/FAT32 volume builder (Microsoft FAT on-disk format 1.03) with an optional MBR. Every layout
choice is a `FatParams` field; `ramdisk_params()` and `ramdisk_fat32_params()` are the RAM disk's two shapes.

    from fs import fat
    v = fat.FatVolume(fat.ramdisk_params())
    v.add_label('OP-XY')
    v.mkdir('logs')
    v.add_file('logs/boot.log', b'...')
    disk = v.build()            # a disk.SparseDisk
"""
import struct
from dataclasses import dataclass, field, replace
from typing import Optional

from .disk import SparseDisk

EOC = {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}
ATTR_RO, ATTR_HID, ATTR_SYS, ATTR_VOL, ATTR_DIR, ATTR_ARC, ATTR_LFN = 1, 2, 4, 8, 0x10, 0x20, 0x0F
NT_LOWER_BASE, NT_LOWER_EXT = 0x08, 0x10
SFN_CHARS = set(b'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&\'()-@^_`{}~') | set(range(0x80, 0x100))
LFN_OFFSETS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)


def fat_date(year, month, day):
    return ((year - 1980) << 9) | (month << 5) | day


def fat_time(hour, minute, sec):
    return (hour << 11) | (minute << 5) | (sec // 2)


@dataclass
class FatParams:
    """Every choice that shapes the volume. Sizes are in sectors unless named otherwise.

    Size: give `disk_sectors` (the whole disk; the volume is what follows `volume_start`) or
    `n_clusters` (the volume is then exactly large enough for that many data clusters).
    """
    fat_type: int = 16                      # 12, 16 or 32
    sector_size: int = 512
    sectors_per_cluster: int = 4
    reserved_sectors: int = 1               # FAT32 usually 32
    n_fats: int = 2
    root_entries: int = 512                 # FAT12/16 fixed root directory; 0 on FAT32
    disk_sectors: Optional[int] = None
    n_clusters: Optional[int] = None
    fat_sectors: Optional[int] = None       # None: the smallest FAT that covers every cluster
    volume_start: int = 0                   # 0: no partition table ("super floppy"); else MBR + 1 partition
    partition_type: Optional[int] = None    # None: 0x01 / 0x04 / 0x06 / 0x0C from the FAT type and size
    mbr_chs: str = 'cylinder'               # 'cylinder': end CHS = last head/sector of the end cylinder; 'exact'
    media: int = 0xF8
    jump: Optional[bytes] = None            # None: EB 3C 90 (FAT12/16), EB 58 90 (FAT32)
    oem_name: bytes = b'MSDOS5.0'
    volume_id: int = 0
    bpb_label: bytes = b'NO NAME    '
    fs_type_string: Optional[bytes] = None  # None: 'FAT12   ' / 'FAT16   ' / 'FAT32   '
    drive_number: int = 0x80
    sectors_per_track: int = 63
    heads: int = 255
    fat0: Optional[int] = None              # None: media in the low byte, all other bits set
    root_cluster: int = 2                   # FAT32
    fsinfo_sector: int = 1                  # FAT32
    backup_boot_sector: int = 6             # FAT32; boot sector and FSInfo are copied there
    fsinfo_free: Optional[int] = None       # None: the true free count
    fsinfo_next: Optional[int] = None       # None: the highest allocated cluster
    timestamp: tuple = (fat_date(1980, 1, 1), 0)   # (date, time) written as the entries' write stamp
    type_rule: str = 'microsoft'            # cluster-count thresholds: 'microsoft' (<4085, <65525) or 'fatfs' (<=)
    case_flags: bool = True                 # all-lowercase 8.3 names: SFN + NT case bits instead of an LFN
    file_crt_stamp: bool = False            # files (not directories) also get the stamp as creation time
    extra: dict = field(default_factory=dict)


def ramdisk_params(**kw):
    """The RAM disk's FAT16 shape: 454656 sectors, one MBR partition at 63, 16-sector clusters, one FAT."""
    p = FatParams(fat_type=16, disk_sectors=454656, volume_start=63, sectors_per_cluster=16,
                  reserved_sectors=1, n_fats=1, root_entries=512, jump=b'\xEB\xFE\x90',
                  volume_id=0x00210000, fs_type_string=b'FAT     ', file_crt_stamp=True)
    return replace(p, **kw)


def ramdisk_fat32_params(**kw):
    """The same disk as FAT32: partition type 0x0C, 1-sector clusters, 32 reserved sectors, 2 FATs."""
    p = FatParams(fat_type=32, disk_sectors=454656, volume_start=63, sectors_per_cluster=1,
                  reserved_sectors=32, n_fats=2, root_entries=0, volume_id=0x00210000)
    return replace(p, **kw)


def fat_entry_bytes(fat_type, n_entries):
    return {12: (n_entries * 3 + 1) // 2, 16: n_entries * 2, 32: n_entries * 4}[fat_type]


def type_for_clusters(n, rule='microsoft'):
    if rule == 'fatfs':
        return 12 if n <= 4085 else 16 if n <= 65525 else 32
    return 12 if n < 4085 else 16 if n < 65525 else 32


class Layout:
    """Sector numbers are absolute (from the start of the disk)."""

    def __init__(self, p):
        if p.fat_type not in (12, 16, 32):
            raise ValueError('fat_type must be 12, 16 or 32')
        ss, spc = p.sector_size, p.sectors_per_cluster
        if ss not in (512, 1024, 2048, 4096) or spc not in (1, 2, 4, 8, 16, 32, 64, 128):
            raise ValueError('bad sector size or cluster size')
        self.root_sectors = (p.root_entries * 32 + ss - 1) // ss if p.fat_type != 32 else 0
        fixed = p.reserved_sectors + self.root_sectors
        if p.n_clusters is not None:
            n = p.n_clusters
            f = p.fat_sectors or (fat_entry_bytes(p.fat_type, n + 2) + ss - 1) // ss
            vol = fixed + p.n_fats * f + n * spc
        else:
            if p.disk_sectors is None:
                raise ValueError('give disk_sectors or n_clusters')
            vol = p.disk_sectors - p.volume_start

            def clusters(f):
                return (vol - fixed - p.n_fats * f) // spc

            if p.fat_sectors:
                f = p.fat_sectors
            else:
                f = (fat_entry_bytes(p.fat_type, vol // spc + 2) + ss - 1) // ss
                while f > 1 and fat_entry_bytes(p.fat_type, clusters(f - 1) + 2) <= (f - 1) * ss:
                    f -= 1
            n = clusters(f)
        if n < 1:
            raise ValueError('volume too small')
        if fat_entry_bytes(p.fat_type, n + 2) > f * ss:
            raise ValueError('FAT too small for the cluster count')
        self.vol_sectors, self.fat_sectors, self.n_clusters = vol, f, n
        self.volbase = p.volume_start
        self.fatbase = p.volume_start + p.reserved_sectors
        self.rootbase = self.fatbase + p.n_fats * f          # FAT12/16 root directory
        self.database = self.rootbase + self.root_sectors
        self.disk_sectors = p.disk_sectors if p.n_clusters is None else p.volume_start + vol
        if self.disk_sectors < p.volume_start + vol:
            raise ValueError('volume does not fit the disk')
        self.cluster_bytes = spc * ss

    def cluster_sector(self, c, p):
        return self.database + (c - 2) * p.sectors_per_cluster


# ---------------------------------------------------------------------------------------------- names

def sfn_checksum(sfn11):
    s = 0
    for c in sfn11:
        s = (((s >> 1) | (s << 7)) + c) & 0xFF
    return s


NAME_FORBIDDEN = set('"*/:<>?\\|\x7f') | {chr(c) for c in range(0x20)}


def name_refused(name):
    """S-12.012: True if a writer refuses `name`."""
    return (not name or len(name.encode('utf-16-le')) // 2 > 255 or bool(set(name) & NAME_FORBIDDEN) or
            name[-1] in ' .')


def _split83(name):
    if name in ('.', '..'):
        return None
    if name.count('.') > 1 or name.startswith('.') or name.endswith('.'):
        return None
    base, _, ext = name.partition('.')
    if not 1 <= len(base) <= 8 or len(ext) > 3:
        return None
    return base, ext


def short_name(name, case_flags=True):
    """(11-byte SFN, NT case bits) if `name` can be stored without an LFN, else None."""
    parts = _split83(name)
    if parts is None:
        return None
    try:
        raw = [x.encode('ascii') for x in parts]
    except UnicodeEncodeError:
        return None
    nt = 0
    for i, x in enumerate(parts):
        up = x.upper()
        if any(c not in SFN_CHARS for c in up.encode('ascii')):
            return None
        if x == up:
            continue
        if case_flags and x == x.lower():
            nt |= NT_LOWER_BASE if i == 0 else NT_LOWER_EXT
        else:
            return None
    return (raw[0].upper().ljust(8) + raw[1].upper().ljust(3)), nt


def basis_name(name):
    """The uppercased, filtered 8.3 basis of a long name, and whether it lost nothing but case."""
    lossy = False
    up = name.upper()
    s = up.lstrip('.').replace(' ', '')
    if s != up:
        lossy = True
    if '.' in s:
        base, ext = s[:s.rfind('.')].replace('.', ''), s[s.rfind('.') + 1:]
        if base != s[:s.rfind('.')]:
            lossy = True
    else:
        base, ext = s, ''

    def filt(t):
        nonlocal lossy
        out = bytearray()
        for ch in t:
            b = ch.encode('ascii', 'replace')[0] if ord(ch) < 128 else ord('_')
            if b not in SFN_CHARS or ord(ch) >= 128:
                b = ord('_')
                lossy = True
            out.append(b)
        return bytes(out)

    base, ext = filt(base), filt(ext)
    if len(base) > 8 or len(ext) > 3 or not base:
        lossy = True
    return base[:8] or b'_', ext[:3], lossy


def lfn_entries(name, sfn11):
    """The LFN entries for `name`, in on-disk order (highest ordinal first)."""
    u = list(name.encode('utf-16-le'))
    units = [u[i] | (u[i + 1] << 8) for i in range(0, len(u), 2)]
    if len(units) > 255:
        raise ValueError('long name over 255 UTF-16 units')
    if len(units) % 13:
        units += [0] + [0xFFFF] * ((-len(units) - 1) % 13)
    n = len(units) // 13
    chk = sfn_checksum(sfn11)
    out = []
    for k in range(n, 0, -1):
        e = bytearray(32)
        e[0] = k | (0x40 if k == n else 0)
        e[11], e[12], e[13] = ATTR_LFN, 0, chk
        for i, ofs in enumerate(LFN_OFFSETS):
            struct.pack_into('<H', e, ofs, units[(k - 1) * 13 + i])
        out.append(bytes(e))
    return out


def dirent(sfn11, attr, cluster, size, nt=0, stamp=(0, 0), crt_stamp=None, acc_date=0):
    e = bytearray(32)
    e[0:11] = sfn11
    e[11], e[12] = attr, nt
    if crt_stamp:
        struct.pack_into('<HH', e, 14, crt_stamp[1], crt_stamp[0])
    struct.pack_into('<H', e, 18, acc_date)
    struct.pack_into('<H', e, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into('<HH', e, 22, stamp[1], stamp[0])
    struct.pack_into('<H', e, 26, cluster & 0xFFFF)
    struct.pack_into('<I', e, 28, size)
    return bytes(e)


# ---------------------------------------------------------------------------------------------- builder

class _Node:
    def __init__(self, kind, name, parent, **kw):
        self.kind, self.name, self.parent = kind, name, parent
        self.children = []
        self.clusters = []
        self.data = b''
        self.attr = kw.pop('attr', None)
        self.sfn = kw.pop('sfn', None)
        self.nt = 0
        self.stamp = kw.pop('stamp', None)
        self.raw = kw.pop('raw', None)
        self.__dict__.update(kw)


class FatVolume:
    """Builds a volume entry by entry. Entries appear in each directory in the order they are added.
    Clusters are allocated lowest-free-first as entries are added, unless given explicitly."""

    def __init__(self, params):
        self.p = params
        self.lay = Layout(params)
        self.fat = {}                                      # cluster -> next (or EOC)
        self.root = _Node('dir', '', None)
        if params.fat_type == 32:
            self.root.clusters = self._alloc(1, [params.root_cluster])

    # ---- allocation
    def _alloc(self, n, explicit=None):
        if explicit is not None:
            ch = list(explicit)
            for c in ch:
                if not 2 <= c < self.lay.n_clusters + 2 or c in self.fat:
                    raise ValueError(f'cluster {c} out of range or in use')
        else:
            ch, c = [], 2
            while len(ch) < n:
                if c >= self.lay.n_clusters + 2:
                    raise ValueError('volume full')
                if c not in self.fat:
                    ch.append(c)
                c += 1
        eoc = EOC[self.p.fat_type]
        for a, b in zip(ch, ch[1:] + [None]):
            self.fat[a] = b if b is not None else eoc
        return ch

    def _extend(self, node, n):
        new = self._alloc(n)
        if node.clusters:
            self.fat[node.clusters[-1]] = new[0]
        node.clusters += new

    # ---- tree
    def _lookup(self, path):
        node = self.root
        for part in [x for x in path.split('/') if x]:
            for ch in node.children:
                if ch.kind in ('dir', 'file') and ch.name.upper() == part.upper():
                    node = ch
                    break
            else:
                raise KeyError(path)
        return node

    def _place(self, path, kind, **kw):
        head, _, name = path.strip('/').rpartition('/')
        parent = self._lookup(head)
        if parent.kind != 'dir':
            raise ValueError(f'{head} is not a directory')
        if kind in ('dir', 'file') and any(c.kind in ('dir', 'file') and c.name.upper() == name.upper()
                                           for c in parent.children):
            raise ValueError(f'{path} exists')
        node = _Node(kind, name, parent, **kw)
        if kind in ('dir', 'file'):
            self._name(node)
        parent.children.append(node)
        return node

    def _name(self, node):
        sib = {c.sfn for c in node.parent.children if c.sfn}
        if node.sfn is not None:
            node.sfn = node.sfn if isinstance(node.sfn, bytes) else node.sfn.encode('ascii')
            node.lfn = node.lfn if hasattr(node, 'lfn') else None
            if node.sfn in sib:
                raise ValueError('duplicate short name')
            return
        if name_refused(node.name):
            raise ValueError(f'name refused (S-12.012): {node.name!r}')
        s = short_name(node.name, self.p.case_flags)
        if s:
            node.sfn, node.nt, node.lfn = s[0], s[1], None
            if node.sfn in sib:
                raise ValueError(f'duplicate short name for {node.name}')
            return
        node.lfn = node.name
        base, ext, lossy = basis_name(node.name)
        cand = base.ljust(8) + ext.ljust(3)
        if not lossy and cand not in sib:
            node.sfn = cand
            return
        for k in range(1, 1000000):
            tail = b'~%d' % k
            cand = base[:8 - len(tail)] + tail
            cand = cand.ljust(8) + ext.ljust(3)
            if cand not in sib:
                node.sfn = cand
                return
        raise ValueError('no free numeric tail')

    def mkdir(self, path, clusters=None, stamp=None, sfn=None, lfn=None, attr=ATTR_DIR):
        kw = {}
        if sfn is not None:
            kw.update(sfn=sfn, lfn=lfn)
        node = self._place(path, 'dir', attr=attr, stamp=stamp, **kw)
        node.clusters = self._alloc(1 if clusters is None else len(clusters), clusters)
        return node

    def add_file(self, path, data, clusters=None, stamp=None, sfn=None, lfn=None, attr=ATTR_ARC):
        kw = {}
        if sfn is not None:
            kw.update(sfn=sfn, lfn=lfn)
        node = self._place(path, 'file', attr=attr, stamp=stamp, data=bytes(data), **kw)
        need = (len(data) + self.lay.cluster_bytes - 1) // self.lay.cluster_bytes
        if clusters is not None and len(clusters) != need:
            raise ValueError(f'{path}: {need} clusters needed, {len(clusters)} given')
        node.clusters = self._alloc(need, clusters) if need else []
        return node

    def add_label(self, label, stamp=(0, 0)):
        """The volume-label entry (root directory only). The BPB label is FatParams.bpb_label."""
        raw = label.encode('ascii').upper()[:11].ljust(11) if isinstance(label, str) else label
        return self._place('/x', 'raw', raw=dirent(raw, ATTR_VOL, 0, 0, stamp=stamp))

    def add_deleted(self, path, attr=ATTR_ARC):
        """A deleted (0xE5) entry: the 8.3 name with its first byte replaced, no cluster, size 0."""
        head, _, name = path.strip('/').rpartition('/')
        s = short_name(name, False) or short_name(name.upper(), False)
        if not s:
            raise ValueError('deleted entries take an 8.3 name')
        e = bytearray(dirent(s[0], attr, 0, 0))
        e[0] = 0xE5
        return self._place((head + '/x').lstrip('/'), 'raw', raw=bytes(e))

    def add_raw_entry(self, dirpath, entry32):
        return self._place((dirpath.strip('/') + '/x').lstrip('/'), 'raw', raw=bytes(entry32))

    # ---- output
    def _stamp(self, node):
        return node.stamp if node.stamp is not None else self.p.timestamp

    def _dir_bytes(self, node):
        out = bytearray()
        if node is not self.root:
            parent_cl = 0 if node.parent is self.root else node.parent.clusters[0]
            st = self._stamp(node)
            out += dirent(b'.          ', ATTR_DIR, node.clusters[0], 0, stamp=st)
            out += dirent(b'..         ', ATTR_DIR, parent_cl, 0, stamp=st)
        for ch in node.children:
            if ch.kind == 'raw':
                out += ch.raw
                continue
            if ch.lfn:
                for e in lfn_entries(ch.lfn, ch.sfn):
                    out += e
            size = len(ch.data) if ch.kind == 'file' else 0
            st = self._stamp(ch)
            crt = st if (self.p.file_crt_stamp and ch.kind == 'file') else None
            out += dirent(ch.sfn, ch.attr, ch.clusters[0] if ch.clusters else 0, size, nt=ch.nt,
                          stamp=st, crt_stamp=crt)
        return bytes(out)

    def build(self):
        p, lay = self.p, self.lay
        ss = p.sector_size
        disk = SparseDisk(lay.disk_sectors * ss, ss)
        # directories, depth first; a directory that outgrows its clusters is extended
        dirs, stack = [], [self.root]
        while stack:
            d = stack.pop()
            dirs.append(d)
            stack += [c for c in reversed(d.children) if c.kind == 'dir']
        blobs = {}
        for d in dirs:
            b = self._dir_bytes(d)
            if d is self.root and p.fat_type != 32:
                if len(b) > p.root_entries * 32:
                    raise ValueError('root directory full')
            else:
                need = max(1, (len(b) + lay.cluster_bytes - 1) // lay.cluster_bytes)
                if need > len(d.clusters):
                    self._extend(d, need - len(d.clusters))
            blobs[id(d)] = b
        for d in dirs:
            b = blobs[id(d)]
            if d is self.root and p.fat_type != 32:
                disk.write(lay.rootbase * ss, b)
            else:
                self._write_chain(disk, d.clusters, b.ljust(len(d.clusters) * lay.cluster_bytes, b'\0'))
        stack = [self.root]
        while stack:
            d = stack.pop()
            for c in d.children:
                if c.kind == 'file' and c.clusters:
                    self._write_chain(disk, c.clusters, c.data)
                elif c.kind == 'dir':
                    stack.append(c)
        self._write_fats(disk)
        boot = self._boot_sector()
        disk.write(lay.volbase * ss, boot)
        if p.fat_type == 32:
            fsi = self._fsinfo()
            disk.write((lay.volbase + p.fsinfo_sector) * ss, fsi)
            if p.backup_boot_sector:
                disk.write((lay.volbase + p.backup_boot_sector) * ss, boot)
                disk.write((lay.volbase + p.backup_boot_sector + p.fsinfo_sector) * ss, fsi)
        if p.volume_start:
            disk.write(0, self._mbr())
        return disk

    def _write_chain(self, disk, chain, data):
        cb = self.lay.cluster_bytes
        for i, c in enumerate(chain):
            part = data[i * cb:(i + 1) * cb]
            if part:
                disk.write(self.lay.cluster_sector(c, self.p) * self.p.sector_size, part)

    def _write_fats(self, disk):
        p, lay = self.p, self.lay
        t = p.fat_type
        mask = EOC[t] if t != 32 else 0x0FFFFFFF
        fat0 = p.fat0 if p.fat0 is not None else (mask & ~0xFF) | p.media
        ent = dict(self.fat)
        ent[0], ent[1] = fat0, EOC[t]
        fb = bytearray(fat_entry_bytes(t, max(ent) + 1) + 2)
        for n, v in ent.items():
            if t == 16:
                struct.pack_into('<H', fb, n * 2, v)
            elif t == 32:
                struct.pack_into('<I', fb, n * 4, v)
            else:
                o = n * 3 // 2
                w = struct.unpack_from('<H', fb, o)[0]
                w = (w & 0xF000) | v if n % 2 == 0 else (w & 0x000F) | (v << 4)
                struct.pack_into('<H', fb, o, w)
        fb = bytes(fb[:lay.fat_sectors * p.sector_size])
        for k in range(p.n_fats):
            disk.write((lay.fatbase + k * lay.fat_sectors) * p.sector_size, fb.rstrip(b'\0') or b'\0')

    def free_clusters(self):
        return self.lay.n_clusters - len(self.fat)

    def _boot_sector(self):
        p, lay = self.p, self.lay
        t = p.fat_type
        b = bytearray(p.sector_size)
        b[0:3] = p.jump if p.jump is not None else (b'\xEB\x58\x90' if t == 32 else b'\xEB\x3C\x90')
        b[3:11] = p.oem_name.ljust(8)[:8]
        struct.pack_into('<HBHBH', b, 11, p.sector_size, p.sectors_per_cluster, p.reserved_sectors,
                         p.n_fats, p.root_entries if t != 32 else 0)
        small = lay.vol_sectors < 0x10000 and t != 32
        struct.pack_into('<H', b, 19, lay.vol_sectors if small else 0)
        b[21] = p.media
        struct.pack_into('<HHHI', b, 22, lay.fat_sectors if t != 32 else 0, p.sectors_per_track, p.heads,
                         p.volume_start)
        struct.pack_into('<I', b, 32, 0 if small else lay.vol_sectors)
        ext = 36
        if t == 32:
            struct.pack_into('<IHHIHH', b, 36, lay.fat_sectors, 0, 0, p.root_cluster, p.fsinfo_sector,
                             p.backup_boot_sector)
            ext = 64
        b[ext] = p.drive_number
        b[ext + 2] = 0x29
        struct.pack_into('<I', b, ext + 3, p.volume_id)
        b[ext + 7:ext + 18] = p.bpb_label.ljust(11)[:11]
        b[ext + 18:ext + 26] = (p.fs_type_string or b'FAT%d   ' % t).ljust(8)[:8]
        struct.pack_into('<H', b, 510, 0xAA55)
        return bytes(b)

    def _fsinfo(self):
        p = self.p
        f = bytearray(p.sector_size)
        free = p.fsinfo_free if p.fsinfo_free is not None else self.free_clusters()
        nxt = p.fsinfo_next if p.fsinfo_next is not None else max(self.fat)
        struct.pack_into('<I', f, 0, 0x41615252)
        struct.pack_into('<IIII', f, 484, 0x61417272, free, nxt, 0)
        struct.pack_into('<I', f, 508, 0xAA550000)
        return bytes(f)

    def partition_type(self):
        p, lay = self.p, self.lay
        if p.partition_type is not None:
            return p.partition_type
        if p.fat_type == 12:
            return 0x01
        if p.fat_type == 16:
            return 0x04 if lay.vol_sectors < 0x10000 else 0x06
        return 0x0C

    def _chs(self, lba, end=False):
        p = self.p
        spc = p.heads * p.sectors_per_track
        c = lba // spc
        if end and p.mbr_chs == 'cylinder':
            h, s = p.heads - 1, p.sectors_per_track
        else:
            h, s = (lba // p.sectors_per_track) % p.heads, lba % p.sectors_per_track + 1
        if c > 1023:
            c, h, s = 1023, p.heads - 1, p.sectors_per_track
        return bytes([h, s | ((c >> 2) & 0xC0), c & 0xFF])

    def _mbr(self):
        p, lay = self.p, self.lay
        m = bytearray(p.sector_size)
        e = bytearray(16)
        e[1:4] = self._chs(p.volume_start)
        e[4] = self.partition_type()
        e[5:8] = self._chs(p.volume_start + lay.vol_sectors - 1, end=True)
        struct.pack_into('<II', e, 8, p.volume_start, lay.vol_sectors)
        m[446:462] = e
        struct.pack_into('<H', m, 510, 0xAA55)
        return bytes(m)

    def info(self):
        """The volume's geometry and where things are, for tests."""
        p, lay = self.p, self.lay
        files = {}
        stack = [('', self.root)]
        while stack:
            pre, d = stack.pop()
            for c in d.children:
                if c.kind == 'file':
                    files[pre + c.name] = dict(clusters=list(c.clusters), size=len(c.data))
                elif c.kind == 'dir':
                    files[pre + c.name + '/'] = dict(clusters=list(c.clusters), size=0)
                    stack.append((pre + c.name + '/', c))
        return dict(fat_type=p.fat_type, sector_size=p.sector_size, sectors_per_cluster=p.sectors_per_cluster,
                    n_clusters=lay.n_clusters, fat_sectors=lay.fat_sectors, volbase=lay.volbase,
                    fatbase=lay.fatbase, rootbase=lay.rootbase, database=lay.database,
                    vol_sectors=lay.vol_sectors, disk_sectors=lay.disk_sectors, free=self.free_clusters(),
                    entries=files)
