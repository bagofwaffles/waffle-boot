# SPDX-License-Identifier: GPL-2.0-or-later
"""yaffs2 volume builder (the public yaffs2 on-media format), in-band tags only: each chunk is its data
followed by a 16-byte tags record {seq_number, obj_id, chunk_id, n_bytes}, little-endian u32 each.
`op_xy_params()` is the eMMC volume's configuration.

    y = Yaffs2Builder(op_xy_params(n_blocks=8))
    y.mkdir('user')
    y.write_file('user/a.txt', b'hello')
    disk = y.image()                     # a disk.SparseDisk, one unit per chunk
"""
import struct
from dataclasses import dataclass, replace

from .disk import SparseDisk

# object types and fixed object ids
FILE, SYMLINK, DIR, HARDLINK, SPECIAL = 1, 2, 3, 4, 5
TYPE_NAMES = {FILE: 'file', SYMLINK: 'symlink', DIR: 'dir', HARDLINK: 'hardlink', SPECIAL: 'special'}
OBJ_ROOT, OBJ_LOST_FOUND, OBJ_UNLINKED, OBJ_DELETED, OBJ_SUMMARY = 1, 2, 3, 4, 0x10
# chunk_id flag bits of a header chunk's tags; obj_id carries the type in bits 28..31
EXTRA_HEADER_INFO, EXTRA_SHRINK, EXTRA_SHADOWS, EXTRA_SPARE = 0x80000000, 0x40000000, 0x20000000, 0x10000000
EXTRA_FLAGS = 0xF0000000
# S-12.047
SEQ_LOWEST, SEQ_HIGHEST = 0x00001000, 0xEFFFFEFF
SEQ_CHECKPOINT, SEQ_BAD_BLOCK = 0x21, 0xFFFF0000
# S-12.050 rule 2
OBJ_ID_MAX, CHUNK_ID_MAX = 0x3FFFF, 0x0FFFFFFF
TAGS_SIZE = 16
SUMMARY_VERSION = 1

# yaffs_obj_hdr field offsets (the header occupies the first 0x200 bytes of the chunk's data)
OH_TYPE, OH_PARENT, OH_NAME, OH_MODE, OH_UID, OH_GID, OH_ATIME, OH_MTIME, OH_CTIME = \
    0x00, 0x04, 0x0A, 0x10C, 0x110, 0x114, 0x118, 0x11C, 0x120
OH_SIZE_LOW, OH_EQUIV, OH_ALIAS, OH_RDEV = 0x124, 0x128, 0x12C, 0x1CC
OH_INBAND_SHADOWED, OH_INBAND_SHRINK, OH_SIZE_HIGH, OH_SHADOWS, OH_SHRINK = 0x1E8, 0x1EC, 0x1F0, 0x1F8, 0x1FC
OH_SIZE = 0x200
NAME_MAX, ALIAS_MAX = 255, 159


@dataclass
class Yaffs2Params:
    chunk_size: int = 4096                 # total bytes per chunk, tags included
    chunks_per_block: int = 128
    n_blocks: int = 16                     # blocks in the image the builder produces
    erased_byte: int = 0x00                # what an erased chunk reads as
    block_offset: int = 1                  # yaffs block number of physical block 0
    first_seq: int = 0x1001                # sequence number of the first block written
    summary: bool = True                   # a block that is filled ends with a summary chunk
    header_fill: int = 0xFF                # unused header bytes (the header buffer is pre-filled)
    write_inband_shrink: bool = False      # also write inband_is_shrink (0 or 1); False leaves the fill
    data_fill: int = 0x00                  # padding after the last data byte of a chunk
    file_mode: int = 0o100666
    dir_mode: int = 0o40777
    symlink_mode: int = 0o120777
    uid: int = 0
    gid: int = 0
    times: tuple = (0, 0, 0)               # atime, mtime, ctime written into every header
    first_obj_id: int = 0x101

    @property
    def data_bytes(self):
        return self.chunk_size - TAGS_SIZE

    @property
    def erased_word(self):
        return self.erased_byte * 0x01010101

    @property
    def summary_entries(self):
        return self.chunks_per_block - 1


def op_xy_params(**kw):
    """The eMMC volume's configuration (the full volume is OP_XY_DEVICE_BLOCKS blocks)."""
    return replace(Yaffs2Params(), **kw)


OP_XY_DEVICE_BLOCKS = 0x1200


def pack_tags(seq, obj_id, chunk_id, n_bytes):
    return struct.pack('<IIII', seq, obj_id, chunk_id, n_bytes)


def unpack_tags(raw, erased_word=0):
    """The 16-byte in-band tags as a dict, or None for an unused chunk (seq_number == erased word)."""
    seq, oid, cid, nb = struct.unpack('<IIII', raw)
    if seq == erased_word:
        return None
    t = dict(seq=seq, raw_obj_id=oid, raw_chunk_id=cid, n_bytes=nb, is_header=bool(cid & EXTRA_HEADER_INFO))
    if t['is_header']:
        t.update(obj_id=oid & 0x0FFFFFFF, type=oid >> 28, chunk_id=0, parent=cid & ~EXTRA_FLAGS & 0xFFFFFFFF,
                 shrink=bool(cid & EXTRA_SHRINK), shadows=bool(cid & EXTRA_SHADOWS))
    else:
        t.update(obj_id=oid, type=None, chunk_id=cid, parent=None, shrink=False, shadows=False)
    return t


def object_header(p, otype, parent, name, size=0, equiv=-1, alias=None, mode=None, shrink=False, shadows=0,
                  times=None, uid=None, gid=None):
    """The chunk data of an object header: the header fields, the rest `header_fill`."""
    h = bytearray([p.header_fill]) * p.data_bytes
    nm = name.encode('utf-8')
    if len(nm) > NAME_MAX:
        raise ValueError('name too long')
    if mode is None:
        mode = {DIR: p.dir_mode, SYMLINK: p.symlink_mode}.get(otype, p.file_mode)
    struct.pack_into('<II', h, OH_TYPE, otype, parent)
    h[OH_NAME:OH_NAME + 256] = nm.ljust(256, b'\0')
    at, mt, ct = times if times is not None else p.times
    struct.pack_into('<IIIIII', h, OH_MODE, mode, p.uid if uid is None else uid, p.gid if gid is None else gid,
                     at, mt, ct)
    struct.pack_into('<I', h, OH_SIZE_LOW, size & 0xFFFFFFFF)
    struct.pack_into('<i', h, OH_EQUIV, equiv)
    if alias is not None:
        a = alias.encode('utf-8')
        if len(a) > ALIAS_MAX:
            raise ValueError('alias too long')
        h[OH_ALIAS:OH_ALIAS + ALIAS_MAX + 1] = a.ljust(ALIAS_MAX + 1, b'\0')
    struct.pack_into('<I', h, OH_INBAND_SHADOWED, shadows)
    if p.write_inband_shrink:
        struct.pack_into('<I', h, OH_INBAND_SHRINK, 1 if shrink else 0)
    struct.pack_into('<I', h, OH_SIZE_HIGH, size >> 32)
    struct.pack_into('<iI', h, OH_SHADOWS, shadows, 1 if shrink else 0)
    return bytes(h)


def parse_header(data):
    g = lambda o, f='<I': struct.unpack_from(f, data, o)[0]
    return dict(type=g(OH_TYPE), parent=g(OH_PARENT), name=data[OH_NAME:OH_NAME + 256].split(b'\0')[0]
                .decode('utf-8', 'replace'), mode=g(OH_MODE), uid=g(OH_UID), gid=g(OH_GID),
                times=(g(OH_ATIME), g(OH_MTIME), g(OH_CTIME)),
                size=g(OH_SIZE_LOW) | ((g(OH_SIZE_HIGH) if g(OH_SIZE_HIGH) != 0xFFFFFFFF else 0) << 32),
                equiv=g(OH_EQUIV, '<i'), alias=data[OH_ALIAS:OH_ALIAS + 160].split(b'\0')[0]
                .decode('utf-8', 'replace'), inband_shadowed=g(OH_INBAND_SHADOWED))


class Yaffs2Builder:
    """Writes chunks in yaffs's order: blocks are filled one after another, each new block gets the next
    sequence number, a file's data chunks are written before its header. Higher-level calls take paths;
    `header()`, `data()` and `raw_chunk()` write single chunks."""

    def __init__(self, params):
        self.p = params
        self.disk = SparseDisk(params.n_blocks * params.chunks_per_block * params.chunk_size,
                               params.chunk_size, params.erased_byte)
        self.block, self.page = None, 0
        self.seq = params.first_seq - 1
        self.sum_tags = []
        self.next_id = params.first_obj_id
        self.objs = {OBJ_ROOT: dict(type=DIR, parent=OBJ_ROOT, name=''),
                     OBJ_LOST_FOUND: dict(type=DIR, parent=OBJ_ROOT, name='lost+found')}
        self.used_blocks = set()

    # ---- placement
    def new_block(self, block=None, seq=None):
        """Starts writing at the first chunk of `block` (default: the next unused one)."""
        if block is None:
            block = 0
            while block in self.used_blocks:
                block += 1
        if block >= self.p.n_blocks:
            raise ValueError('image full')
        self.seq = self.seq + 1 if seq is None else seq
        self.block, self.page, self.sum_tags = block, 0, []
        self.used_blocks.add(block)

    def _put(self, data, tags):
        p = self.p
        if self.block is None:
            self.new_block()
        last = p.chunks_per_block - (1 if p.summary else 0)
        if self.page >= last:
            self.new_block()
        self.raw_chunk(self.block, self.page, data, tags)
        self.sum_tags.append(tags)
        self.page += 1
        loc = (self.block, self.page - 1)
        if p.summary and self.page == last:
            self._summary()
        return loc

    def _summary(self):
        p = self.p
        ent = b''.join(t[4:16] for t in self.sum_tags)
        s = sum(ent) & 0xFFFFFFFF
        hdr = struct.pack('<IIII', SUMMARY_VERSION, self.block + p.block_offset, self.seq, s)
        self.raw_chunk(self.block, p.chunks_per_block - 1, hdr + ent,
                       pack_tags(self.seq, OBJ_SUMMARY, 1, len(ent) + 16))
        self.page = p.chunks_per_block

    def raw_chunk(self, block, page, data, tags):
        p = self.p
        data = bytes(data)
        if len(data) > p.data_bytes or len(tags) != TAGS_SIZE:
            raise ValueError('chunk data or tags too large')
        c = data + bytes([p.data_fill]) * (p.data_bytes - len(data)) + tags
        self.disk.write((block * p.chunks_per_block + page) * p.chunk_size, c)
        self.used_blocks.add(block)

    def header(self, obj_id, otype, parent, name, size=0, equiv=None, alias=None, shrink=False, shadows=0,
               **kw):
        data = object_header(self.p, otype, parent, name, size=size, alias=alias, shrink=shrink, shadows=shadows,
                             equiv=equiv if otype == HARDLINK else -1, **kw)
        cid = EXTRA_HEADER_INFO | parent | (EXTRA_SHRINK if shrink else 0) | (EXTRA_SHADOWS if shadows else 0)
        nb = equiv if otype == HARDLINK else (size & 0xFFFFFFFF if otype == FILE else 0)
        return self._put(data, pack_tags(self.seq_for_put(), obj_id | (otype << 28), cid, nb))

    def data(self, obj_id, chunk_id, payload):
        return self._put(payload, pack_tags(self.seq_for_put(), obj_id, chunk_id, len(payload)))

    def seq_for_put(self):
        p = self.p
        if self.block is None or self.page >= p.chunks_per_block - (1 if p.summary else 0):
            self.new_block()
        return self.seq

    # ---- paths
    def _resolve(self, path):
        oid = OBJ_ROOT
        for part in [x for x in path.split('/') if x]:
            for k, o in self.objs.items():
                if o['parent'] == oid and o['name'] == part and k != OBJ_ROOT:
                    oid = k
                    break
            else:
                raise KeyError(path)
        return oid

    def _new(self, path, otype, obj_id):
        head, _, name = path.strip('/').rpartition('/')
        parent = self._resolve(head)
        if any(o['parent'] == parent and o['name'] == name for k, o in self.objs.items() if k != OBJ_ROOT):
            raise ValueError(f'{path} exists')
        if obj_id is None:
            obj_id = self.next_id
            while obj_id in self.objs:
                obj_id += 1
            self.next_id = obj_id + 1
        self.objs[obj_id] = dict(type=otype, parent=parent, name=name, size=0)
        return obj_id, parent, name

    def mkdir(self, path, obj_id=None, **kw):
        oid, parent, name = self._new(path, DIR, obj_id)
        self.header(oid, DIR, parent, name, **kw)
        return oid

    def write_file(self, path, data, obj_id=None, **kw):
        oid, parent, name = self._new(path, FILE, obj_id)
        n = self.p.data_bytes
        for k in range(0, len(data), n):
            self.data(oid, k // n + 1, data[k:k + n])
        self.objs[oid]['size'] = len(data)
        self.header(oid, FILE, parent, name, size=len(data), **kw)
        return oid

    def symlink(self, path, alias, obj_id=None, **kw):
        oid, parent, name = self._new(path, SYMLINK, obj_id)
        self.objs[oid]['alias'] = alias
        self.header(oid, SYMLINK, parent, name, alias=alias, **kw)
        return oid

    def hardlink(self, path, target, obj_id=None, **kw):
        eq = self._resolve(target)
        oid, parent, name = self._new(path, HARDLINK, obj_id)
        self.objs[oid]['equiv'] = eq
        self.header(oid, HARDLINK, parent, name, equiv=eq, **kw)
        return oid

    def rewrite(self, path, data_chunks=None, new_path=None, size=None, shrink=False, shadows=0, **kw):
        """A newer copy of an existing object: optional data chunks {chunk_id: bytes}, then a header that
        may rename/move it (`new_path`), change a file's size, or carry the shrink/shadows flags."""
        oid = self._resolve(path)
        o = self.objs[oid]
        for cid, payload in sorted((data_chunks or {}).items()):
            self.data(oid, cid, payload)
        if new_path is not None:
            head, _, name = new_path.strip('/').rpartition('/')
            o['parent'], o['name'] = self._resolve(head), name
        if size is not None:
            o['size'] = size
        self.header(oid, o['type'], o['parent'], o['name'], size=o.get('size', 0), shrink=shrink, shadows=shadows,
                    equiv=o.get('equiv'), alias=o.get('alias'), **kw)
        return oid

    def delete(self, path):
        """Deletes a file the way yaffs records it: a shrink header of size 0 in the deleted directory."""
        oid = self._resolve(path)
        o = self.objs.pop(oid)
        self.header(oid, o['type'], OBJ_DELETED, o['name'], size=0, shrink=o['type'] == FILE)
        return oid

    def image(self, big_endian=False):
        """The volume as a SparseDisk; big_endian=True gives it byte-swapped (S-12.052)."""
        return byteswap_image(self.disk, self.p) if big_endian else self.disk


# ---- altered images (S-12.051, S-12.052, S-16.071, S-16.078)

def byteswap_image(img, p):
    """A copy with every 32-bit word of each used chunk's tags, header record and summary byte-swapped;
    names, link targets and file data keep their bytes."""
    out = SparseDisk(img.size, p.chunk_size, p.erased_byte)
    for u in sorted(img.written_units()):
        c = bytearray(img.read(u * p.chunk_size, p.chunk_size))
        t = unpack_tags(bytes(c[-TAGS_SIZE:]), p.erased_word)
        words = list(range(p.chunk_size - TAGS_SIZE, p.chunk_size, 4))
        if t and t['is_header']:
            words += [o for o in range(0, OH_SIZE, 4) if not (OH_NAME <= o < OH_MODE or OH_ALIAS <= o < OH_RDEV)]
        elif t and t['obj_id'] == OBJ_SUMMARY:
            words += list(range(0, 16 + 12 * p.summary_entries, 4))
        for o in words:
            c[o:o + 4] = c[o:o + 4][::-1]
        out.write(u * p.chunk_size, bytes(c))
    return out


def chunk_tags(img, p, idx):
    """The four tag words of chunk `idx`."""
    return struct.unpack('<IIII', img.read((idx + 1) * p.chunk_size - TAGS_SIZE, TAGS_SIZE))


def set_chunk_tags(img, p, idx, seq=None, obj_word=None, chunk_word=None, n_bytes=None):
    """Rewrites some of chunk `idx`'s tag words in place (the data area is untouched)."""
    w = list(chunk_tags(img, p, idx))
    for k, v in enumerate((seq, obj_word, chunk_word, n_bytes)):
        if v is not None:
            w[k] = v & 0xFFFFFFFF
    img.write((idx + 1) * p.chunk_size - TAGS_SIZE, struct.pack('<IIII', *w))


def corrupt_header(img, p, idx, otype=None, parent=None, name=None, size=None):
    """Rewrites fields of the object-header record in chunk `idx`, leaving its tags as they are (S-12.051)."""
    c = bytearray(img.read(idx * p.chunk_size, p.chunk_size))
    if otype is not None:
        struct.pack_into('<I', c, OH_TYPE, otype)
    if parent is not None:
        struct.pack_into('<I', c, OH_PARENT, parent)
    if name is not None:
        c[OH_NAME:OH_NAME + 256] = name.encode('utf-8').ljust(256, b'\0')
    if size is not None:
        struct.pack_into('<I', c, OH_SIZE_LOW, size & 0xFFFFFFFF)
        struct.pack_into('<I', c, OH_SIZE_HIGH, size >> 32)
    img.write(idx * p.chunk_size, bytes(c))


def set_summary_entry(img, p, block, page, obj_word, chunk_word, n_bytes, fix_sum=True):
    """Rewrites entry `page` of block `block`'s summary (its last chunk) and, with fix_sum, its byte sum
    (S-12.049)."""
    idx = (block + 1) * p.chunks_per_block - 1
    s = bytearray(img.read(idx * p.chunk_size, p.chunk_size))
    struct.pack_into('<III', s, 16 + 12 * page, obj_word & 0xFFFFFFFF, chunk_word & 0xFFFFFFFF, n_bytes & 0xFFFFFFFF)
    if fix_sum:
        struct.pack_into('<I', s, 12, sum(s[16:16 + 12 * p.summary_entries]) & 0xFFFFFFFF)
    img.write(idx * p.chunk_size, bytes(s))
