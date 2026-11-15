# SPDX-License-Identifier: GPL-2.0-or-later
"""Reader and consistency checker for FAT12/16/32 volumes, with or without an MBR in front.

    r = FatReader(img)                 # bytes, a SparseDisk or a path
    r.tree()   -> {'SUB': {'type': 'dir'}, 'SUB/INNER.TXT': {'type': 'file', 'data': b'...'}, ...}
    r.check()  -> [] when consistent, else a list of problem strings
"""
import struct

from .disk import source
from .fat import sfn_checksum, type_for_clusters, LFN_OFFSETS, ATTR_VOL, ATTR_DIR, ATTR_LFN, \
    NT_LOWER_BASE, NT_LOWER_EXT


class FatError(Exception):
    pass


def _is_bpb(s):
    if len(s) < 512 or s[510:512] != b'\x55\xAA':
        return False
    if not (s[0] == 0xE9 or (s[0] == 0xEB and s[2] == 0x90)):
        return False
    ss = struct.unpack_from('<H', s, 11)[0]
    return ss in (512, 1024, 2048, 4096) and s[13] in (1, 2, 4, 8, 16, 32, 64, 128) and s[16] >= 1


class FatReader:
    def __init__(self, img, volume_start=None, type_rule='microsoft', codepage='cp850'):
        self.codepage = codepage               # S-12.010
        self.src = source(img)
        self.problems = []
        s0 = self.src.read(0, 512)
        self.mbr = None
        if volume_start is None:
            if _is_bpb(s0):
                volume_start = 0
            elif s0[510:512] == b'\x55\xAA':
                parts = [struct.unpack_from('<B3sB3sII', s0, 446 + 16 * i) for i in range(4)]
                live = [x for x in parts if x[2] != 0]
                if not live:
                    raise FatError('MBR without a partition')
                self.mbr = dict(type=live[0][2], lba=live[0][4], sectors=live[0][5], entry=live[0])
                volume_start = live[0][4]
            else:
                raise FatError('neither a boot sector nor an MBR at sector 0')
        self.volbase = volume_start
        b = self.src.read(volume_start * 512, 512)
        if not _is_bpb(b):
            raise FatError(f'no FAT boot sector at sector {volume_start}')
        self.boot = b
        (self.ss, self.spc, self.rsvd, self.n_fats, self.root_entries, tot16, self.media, fsz16,
         self.spt, self.heads, self.hidden, tot32) = struct.unpack_from('<HBHBHHBHHHII', b, 11)
        if self.ss != 512:
            b = self.src.read(volume_start * self.ss, self.ss)
            self.boot = b
        self.tot = tot16 or tot32
        self.fsz = fsz16 or struct.unpack_from('<I', b, 36)[0]
        self.root_sectors = (self.root_entries * 32 + self.ss - 1) // self.ss
        self.fatbase = volume_start + self.rsvd
        self.rootbase = self.fatbase + self.n_fats * self.fsz
        self.database = self.rootbase + self.root_sectors
        data_sectors = self.tot - (self.rsvd + self.n_fats * self.fsz + self.root_sectors)
        if data_sectors <= 0 or self.fsz == 0:
            raise FatError('BPB leaves no data region')
        self.n_clusters = data_sectors // self.spc
        self.fat_type = type_for_clusters(self.n_clusters, type_rule)
        self.cb = self.spc * self.ss
        ext = 64 if self.fat_type == 32 else 36
        self.volume_id = struct.unpack_from('<I', b, ext + 3)[0] if b[ext + 2] == 0x29 else None
        self.bpb_label = b[ext + 7:ext + 18] if b[ext + 2] == 0x29 else None
        if self.fat_type == 32:
            (self.ext_flags, self.fs_ver, self.root_cluster, self.fsinfo_sector,
             self.backup_boot) = struct.unpack_from('<HHIHH', b, 40)
        else:
            self.root_cluster = 0
        self._fats = [self.src.read((self.fatbase + k * self.fsz) * self.ss, self.fsz * self.ss)
                      for k in range(self.n_fats)]
        self.label = None
        try:
            self.listdir(0)                    # sets the label
        except FatError:
            pass
        self.problems = []

    # ---- FAT
    def entry(self, n, copy=0):
        f = self._fats[copy]
        if self.fat_type == 16:
            return struct.unpack_from('<H', f, n * 2)[0]
        if self.fat_type == 32:
            return struct.unpack_from('<I', f, n * 4)[0] & 0x0FFFFFFF
        w = struct.unpack_from('<H', f, n * 3 // 2)[0]
        return w >> 4 if n & 1 else w & 0xFFF

    def is_eoc(self, v):
        return v >= {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.fat_type]

    def chain(self, start):
        if start == 0:
            return []
        out, seen, c = [], set(), start
        bad = {12: 0xFF7, 16: 0xFFF7, 32: 0x0FFFFFF7}[self.fat_type]
        while True:
            if not 2 <= c < self.n_clusters + 2:
                raise FatError(f'chain from {start}: cluster {c:#x} out of range')
            if c in seen:
                raise FatError(f'chain from {start}: loops at {c}')
            seen.add(c)
            out.append(c)
            v = self.entry(c)
            if self.is_eoc(v):
                return out
            if v == 0:
                raise FatError(f'chain from {start}: cluster {c} links to a free cluster')
            if v == bad:
                raise FatError(f'chain from {start}: cluster {c} links to a bad cluster')
            c = v

    def cluster_sector(self, c):
        return self.database + (c - 2) * self.spc

    def read_chain(self, start, size=None):
        out = bytearray()
        for c in self.chain(start):
            out += self.src.read(self.cluster_sector(c) * self.ss, self.cb)
        return bytes(out if size is None else out[:size])

    # ---- directories
    def _dir_bytes(self, cluster):
        if cluster == 0 and self.fat_type != 32:
            return self.src.read(self.rootbase * self.ss, self.root_sectors * self.ss)
        return self.read_chain(cluster or self.root_cluster)

    def listdir(self, cluster):
        """Live entries of the directory at `cluster` (0 = root): dicts with name, sfn, attr, cluster,
        size, nt, lfn (None if none), pos. LFN sets that do not check out are reported and ignored."""
        raw = self._dir_bytes(cluster)
        out, lfn = [], []
        for pos in range(0, len(raw), 32):
            e = raw[pos:pos + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            attr = e[11]
            if attr & 0x3F == ATTR_LFN:
                lfn.append(e)
                continue
            sfn = e[0:11]
            if sfn[0] == 0x05:
                sfn = b'\xE5' + sfn[1:]
            name_lfn = self._join_lfn(lfn, e[0:11], pos) if lfn else None
            lfn = []
            cl = struct.unpack_from('<H', e, 26)[0] | (struct.unpack_from('<H', e, 20)[0] << 16
                                                         if self.fat_type == 32 else 0)
            if attr & ATTR_VOL and not attr & ATTR_DIR:
                if cluster in (0, self.root_cluster):
                    self.label = sfn.rstrip().decode(self.codepage)
                continue
            if sfn in (b'.          ', b'..         '):
                out.append(dict(name=sfn.strip().decode(), sfn=sfn, attr=attr, cluster=cl, size=0, nt=e[12],
                                lfn=None, pos=pos, dot=True))
                continue
            out.append(dict(name=name_lfn or self._sfn_display(sfn, e[12]), sfn=sfn, attr=attr, cluster=cl,
                            size=struct.unpack_from('<I', e, 28)[0], nt=e[12], lfn=name_lfn, pos=pos,
                            dot=False, date=struct.unpack_from('<H', e, 24)[0],
                            time=struct.unpack_from('<H', e, 22)[0]))
        return out

    def _sfn_display(self, sfn, nt):
        """S-12.011, S-12.010: the display name of a short entry (bytes.lower is ASCII-only)."""
        base, ext = sfn[:8].rstrip(b' '), sfn[8:].rstrip(b' ')
        if nt & NT_LOWER_BASE:
            base = base.lower()
        if nt & NT_LOWER_EXT:
            ext = ext.lower()
        base, ext = base.decode(self.codepage), ext.decode(self.codepage)
        return base + ('.' + ext if ext else '')

    def _join_lfn(self, ents, sfn, pos):
        chk = sfn_checksum(sfn)
        n = len(ents)
        ok = ents[0][0] & 0x40 and (ents[0][0] & 0x3F) == n
        units = []
        for i, e in enumerate(ents):
            if (e[0] & 0x3F) != n - i or e[13] != chk:
                ok = False
        if not ok:
            self.problems.append(f'directory entry at {pos}: LFN set does not match its short name')
            return None
        for e in reversed(ents):
            units += [struct.unpack_from('<H', e, o)[0] for o in LFN_OFFSETS]
        if 0 in units:
            units = units[:units.index(0)]
        return bytes(b for u in units for b in struct.pack('<H', u)).decode('utf-16-le', 'replace')

    def walk(self):
        """(path, entry) for every file and directory, depth first, in directory order."""
        stack = [('', 0)]
        while stack:
            pre, cl = stack.pop()
            ents = [e for e in self.listdir(cl) if not e['dot']]
            subs = []
            for e in ents:
                path = pre + e['name']
                yield path, e
                if e['attr'] & ATTR_DIR:
                    subs.append((path + '/', e['cluster']))
            stack += reversed(subs)

    def tree(self):
        out = {}
        for path, e in self.walk():
            if e['attr'] & ATTR_DIR:
                out[path] = {'type': 'dir'}
            else:
                out[path] = {'type': 'file', 'data': self.read_chain(e['cluster'], e['size'])}
        return out

    def read_file(self, path):
        want = path.strip('/').upper()
        for p, e in self.walk():
            if p.upper() == want and not e['attr'] & ATTR_DIR:
                return self.read_chain(e['cluster'], e['size'])
        raise FileNotFoundError(path)

    def free_count(self):
        return sum(1 for c in range(2, self.n_clusters + 2) if self.entry(c) == 0)

    # ---- checker
    def check(self):
        """Every inconsistency found, as strings. An empty list means the volume is consistent."""
        P = self.problems = []
        n = self.n_clusters
        if n in (4084, 4085, 65524, 65525):
            P.append(f'{n} clusters: FAT type differs between the Microsoft and FatFs thresholds')
        for k in range(1, self.n_fats):
            if self._fats[k] != self._fats[0]:
                P.append(f'FAT copy {k} differs from FAT 0')
        if not self.is_eoc(self.entry(1)):
            P.append('FAT[1] is not an end-of-chain mark')
        if self.entry(0) & 0xFF != self.media:
            P.append('FAT[0] does not carry the media byte')
        if self.mbr:
            if self.mbr['lba'] != self.hidden:
                P.append('BPB hidden sectors differ from the partition start')
            if self.mbr['sectors'] < self.tot:
                P.append('volume is larger than its partition')
        if (self.volbase + self.tot) * self.ss > self.src.size:
            P.append('volume extends past the end of the image')
        owner = {}

        def claim(chain, who):
            for c in chain:
                if c in owner:
                    P.append(f'cluster {c} cross-linked: {owner[c]} and {who}')
                owner[c] = who

        try:
            if self.fat_type == 32:
                claim(self.chain(self.root_cluster), '/')
            stack = [('', 0, 0)]
            while stack:
                pre, cl, parent = stack.pop()
                for e in self.listdir(cl):
                    path = pre + e['name']
                    if e['dot']:
                        want = cl if e['name'] == '.' else parent
                        if self.fat_type == 32 and want == self.root_cluster and e['name'] == '..':
                            want = 0
                        if e['cluster'] != want:
                            P.append(f'{pre or "/"}: "{e["name"]}" points at {e["cluster"]}, not {want}')
                        continue
                    try:
                        ch = self.chain(e['cluster'])
                    except FatError as x:
                        P.append(f'{path}: {x}')
                        continue
                    claim(ch, path)
                    if e['attr'] & ATTR_DIR:
                        if not ch:
                            P.append(f'{path}: directory without a cluster')
                            continue
                        if e['size']:
                            P.append(f'{path}: directory with a nonzero size')
                        stack.append((path + '/', e['cluster'], cl))
                    else:
                        need = (e['size'] + self.cb - 1) // self.cb
                        if len(ch) != need:
                            P.append(f'{path}: {len(ch)} clusters for {e["size"]} bytes (need {need})')
        except FatError as x:
            P.append(str(x))
        lost = [c for c in range(2, n + 2) if self.entry(c) != 0 and c not in owner
                and self.entry(c) != {12: 0xFF7, 16: 0xFFF7, 32: 0x0FFFFFF7}[self.fat_type]]
        if lost:
            P.append(f'{len(lost)} lost clusters (allocated, in no chain), first {lost[:5]}')
        if self.fat_type == 32:
            f = self.src.read((self.volbase + self.fsinfo_sector) * self.ss, self.ss)
            if (struct.unpack_from('<I', f, 0)[0] != 0x41615252 or struct.unpack_from('<I', f, 484)[0] != 0x61417272
                    or struct.unpack_from('<I', f, 508)[0] != 0xAA550000):
                P.append('FSInfo signatures wrong')
            free, nxt = struct.unpack_from('<II', f, 488)
            if free != 0xFFFFFFFF and free != self.free_count():
                P.append(f'FSInfo free count {free}, FAT says {self.free_count()}')
            if nxt != 0xFFFFFFFF and not 2 <= nxt < n + 2:
                P.append(f'FSInfo next-free hint {nxt} out of range')
            if self.backup_boot:
                bk = self.src.read((self.volbase + self.backup_boot) * self.ss, self.ss)
                if bk != self.boot:
                    P.append('backup boot sector differs from the boot sector')
        return P
