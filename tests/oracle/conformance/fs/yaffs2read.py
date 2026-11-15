# SPDX-License-Identifier: GPL-2.0-or-later
"""Reader and consistency checker for yaffs2 volumes with in-band tags (S-12.047, S-12.049 to S-12.052).
It rebuilds the tree with a backward scan, newest block and chunk first.

    r = Yaffs2Reader(img, op_xy_params())
    r.tree()  -> {'user': {'type': 'dir'}, 'user/a.txt': {'type': 'file', 'data': b'..'}, 'lost+found': ...}
    r.check() -> [] when consistent
"""
import struct

from .disk import source
from .yaffs2 import (unpack_tags, parse_header, TAGS_SIZE, SEQ_LOWEST, SEQ_HIGHEST, OBJ_ROOT, OBJ_LOST_FOUND,
                     OBJ_UNLINKED, OBJ_DELETED, OBJ_SUMMARY, OBJ_ID_MAX, CHUNK_ID_MAX, SUMMARY_VERSION, FILE,
                     SYMLINK, DIR, HARDLINK, TYPE_NAMES, EXTRA_HEADER_INFO)

OWN = 'own'          # a summary entry that defers to the chunk's own tags
SUMMARY_TAGS = (OBJ_SUMMARY, 1, 1540)


def consistent_header(tags_words, data, fmt):
    """S-12.052: True if four tag words and a chunk's data, decoded with `fmt` ('<' or '>'), form a
    consistent header."""
    seq, ow, cw, _nb = tags_words
    if seq == 0 or not cw & EXTRA_HEADER_INFO or not 1 <= ow >> 28 <= 5:
        return False
    rtype, rparent = struct.unpack_from(fmt + 'II', data, 0)
    return rtype == ow >> 28 and rparent == cw & 0x0FFFFFFF


class Yaffs2Reader:
    def __init__(self, img, params, n_blocks=None):
        self.p = p = params
        self.src = source(img)
        bsize = p.chunk_size * p.chunks_per_block
        self.n_blocks = n_blocks if n_blocks is not None else self.src.size // bsize
        self.problems = []
        self.blocks = {}                  # block -> dict(state, seq, summary)
        self._tags = {}
        self._scan()

    # ---- raw access
    def chunk_index(self, block, page):
        return block * self.p.chunks_per_block + page

    def read_chunk(self, idx):
        p = self.p
        off = idx * p.chunk_size
        if off + p.chunk_size > self.src.size:
            return bytes([p.erased_byte]) * p.chunk_size
        return self.src.read(off, p.chunk_size)

    def tags(self, idx):
        if idx not in self._tags:
            c = self.read_chunk(idx)
            self._tags[idx] = unpack_tags(c[-TAGS_SIZE:], self.p.erased_word)
        return self._tags[idx]

    def _summary(self, block, seq, report=True):
        """S-12.049: the block's summary as per-chunk tags (OWN = the chunk's own), or None if not valid."""
        p = self.p
        last = self.chunk_index(block, p.chunks_per_block - 1)
        t = self.tags(last)
        if not t or (t['raw_obj_id'], t['raw_chunk_id'], t['n_bytes']) != SUMMARY_TAGS:
            return None
        d = self.read_chunk(last)
        ver, _blk, sseq, ssum = struct.unpack_from('<IIII', d, 0)
        ent = d[16:16 + 12 * p.summary_entries]
        why = None
        if ver != SUMMARY_VERSION:
            why = f'version {ver}'
        elif sseq != seq:
            why = f'seq {sseq:#x}'
        elif sum(ent) & 0xFFFFFFFF != ssum:
            why = 'checksum'
        if why:
            if report:
                self.problems.append(f'block {block}: summary rejected ({why}); chunk tags used')
            return None
        out = []
        for k in range(p.summary_entries):
            e = ent[12 * k:12 * k + 12]
            if struct.unpack_from('<I', e, 0)[0] == 0:
                out.append(OWN)
            else:
                out.append(unpack_tags(struct.pack('<I', seq) + e, p.erased_word))
        return out

    def illegal(self, t, block_seq):
        """S-12.050 rule 2: True if the scan ignores this chunk."""
        if t['obj_id'] > OBJ_ID_MAX or t['obj_id'] == OBJ_SUMMARY:
            return True
        if not t['is_header'] and (t['raw_chunk_id'] > CHUNK_ID_MAX or t['n_bytes'] > self.p.data_bytes):
            return True
        return t['seq'] != block_seq

    def chunks(self, block):
        """The chunks of a data block that the scan reads, last to first: (page, chunk index, effective
        tags) (S-12.049, S-12.050 rule 2)."""
        p = self.p
        info = self.blocks[block]
        seq = info['seq']
        summ = info.get('summ')
        n = p.chunks_per_block
        for page in range(n - 1, -1, -1):
            idx = self.chunk_index(block, page)
            if summ is not None:
                if page == n - 1:
                    continue                                      # the summary belongs to no object
                t = summ[page]
                if t is OWN:
                    t = self.tags(idx)
            else:
                t = self.tags(idx)
            if t is None or self.illegal(t, seq):
                continue
            yield page, idx, t

    def big_endian_evidence(self):
        """S-12.052: (count of consistent little-endian headers, count of consistent byte-swapped ones)."""
        return self._le_count(), self._swapped_count()

    def _le_count(self, stop_at=None):
        le = 0
        for b, info in self.blocks.items():
            if info['state'] == 'data':
                for _page, idx, t in self.chunks(b):
                    if consistent_header((t['seq'], t['raw_obj_id'], t['raw_chunk_id'], t['n_bytes']),
                                         self.read_chunk(idx), '<'):
                        le += 1
                        if stop_at is not None and le >= stop_at:
                            return le
        return le

    def _swapped_count(self):
        sw = 0
        if hasattr(self.src, 'written_units') and getattr(self.src, 'unit', None) == self.p.chunk_size:
            units = self.src.written_units()
        else:
            units = range(self.n_blocks * self.p.chunks_per_block)
        for u in sorted(units):
            if u >= self.n_blocks * self.p.chunks_per_block or self.tags(u) is None:
                continue
            raw = self.read_chunk(u)
            w = struct.unpack('>IIII', raw[-TAGS_SIZE:])
            oid, parent = w[1] & 0x0FFFFFFF, w[2] & 0x0FFFFFFF
            if consistent_header(w, raw, '>') and SEQ_LOWEST <= w[0] <= SEQ_HIGHEST and \
                    1 <= oid <= OBJ_ID_MAX and oid != OBJ_SUMMARY and 1 <= parent <= OBJ_ID_MAX:
                sw += 1
        return sw

    # ---- scan
    def _scan(self):
        p = self.p
        data_blocks = []
        for b in range(self.n_blocks):
            t = self.tags(self.chunk_index(b, 0))
            if t is None:
                self.blocks[b] = dict(state='empty', seq=None)
            elif SEQ_LOWEST <= t['seq'] <= SEQ_HIGHEST:
                self.blocks[b] = dict(state='data', seq=t['seq'])
                data_blocks.append(b)
            else:
                self.blocks[b] = dict(state='skipped', seq=t['seq'])
        for b in data_blocks:
            summ = self._summary(b, self.blocks[b]['seq']) if p.summary else None
            self.blocks[b]['summary'] = summ is not None
            self.blocks[b]['summ'] = summ
        # the swapped sweep reads every used chunk: run it only when no little-endian header exists
        self.refused = self._le_count(stop_at=1) == 0 and self._swapped_count() > 0
        data_blocks.sort(key=lambda b: (-self.blocks[b]['seq'], -b))
        objs = {}
        order = 0
        shadows = []

        def obj(oid):
            return objs.setdefault(oid, dict(oid=oid, hdr=None, hdr_order=None, limit=None, data={},
                                             newer_end=0, headers=0))

        for b in ([] if self.refused else data_blocks):
            for page, idx, t in self.chunks(b):
                order += 1
                o = obj(t['obj_id'])
                if t['is_header']:
                    o['headers'] += 1
                    h = parse_header(self.read_chunk(idx))
                    own = self.tags(idx)                            # S-12.051: the chunk's own tags
                    trusted = (own is not None and own['is_header'] and own['obj_id'] == t['obj_id'] and
                               own['type'] == t['type'] and h['type'] == t['type'])
                    size = t['n_bytes'] if t['type'] == FILE else 0
                    if trusted and t['type'] == FILE:
                        size = h['size']
                    if t['type'] == FILE and (t['shrink'] or t['parent'] in (OBJ_UNLINKED, OBJ_DELETED)):
                        o['limit'] = size if o['limit'] is None else min(o['limit'], size)
                    if trusted and t['shadows'] and h['inband_shadowed']:
                        shadows.append((order, h['inband_shadowed']))
                    if o['hdr'] is None:
                        named = trusted and (h['parent'] == t['parent'] or
                                             (h['parent'] == OBJ_UNLINKED and t['parent'] == OBJ_DELETED))
                        o.update(hdr=idx, hdr_order=order, type=t['type'], parent=t['parent'], tags=t, oh=h,
                                 size=size, equiv=t['n_bytes'] if t['type'] == HARDLINK else None,
                                 trusted=trusted, name=h['name'] if named else 'obj%d' % t['obj_id'])
                else:
                    key = t['chunk_id']
                    if key in o['data']:
                        continue                                  # an older copy
                    if o['limit'] is not None and (key - 1) * p.data_bytes >= o['limit']:
                        continue                                  # cut by a newer shrink or deleting header
                    o['data'][key] = (idx, t['n_bytes'])
                    if o['hdr'] is None:
                        o['newer_end'] = max(o['newer_end'], (key - 1) * p.data_bytes + t['n_bytes'])
        for sorder, victim in shadows:
            v = objs.get(victim)
            if v and v['hdr'] is not None and v['hdr_order'] > sorder:
                v['shadowed'] = True
        self.objects = objs
        live = {}
        for oid, o in objs.items():
            if oid in (OBJ_ROOT, OBJ_LOST_FOUND, OBJ_UNLINKED, OBJ_DELETED):
                continue
            if o['hdr'] is None:
                self.problems.append(f'object {oid}: data chunks but no header')
                continue
            if o['parent'] in (OBJ_UNLINKED, OBJ_DELETED) or o.get('shadowed'):
                continue
            if o['type'] == FILE:
                o['size'] = max(o['size'], o['newer_end'])
            live[oid] = o
        self.live = live

    def attributes(self, oid):
        """S-12.051: name, mode, owner and times from a trusted current header, else None."""
        o = self.live.get(oid)
        if o is None or not o['trusted']:
            return None
        h = o['oh']
        return dict(name=o['name'], mode=h['mode'], uid=h['uid'], gid=h['gid'], times=h['times'])

    # ---- tree
    def path_of(self, oid, _depth=0):
        if oid == OBJ_ROOT:
            return ''
        if oid == OBJ_LOST_FOUND:
            return 'lost+found'
        o = self.live.get(oid)
        if o is None or _depth > 100:
            return None
        parent = o['parent']
        if parent not in (OBJ_ROOT, OBJ_LOST_FOUND) and (parent not in self.live or self.live[parent]['type'] != DIR):
            return 'lost+found/' + o['name']
        pre = self.path_of(parent, _depth + 1)
        if pre is None:
            return None
        return (pre + '/' if pre else '') + o['name']

    def file_bytes(self, o):
        p = self.p
        out = bytearray()
        size = o['size']
        n = (size + p.data_bytes - 1) // p.data_bytes
        for cid in range(1, n + 1):
            loc = o['data'].get(cid)
            if loc is None:
                part = b''
            else:
                part = self.read_chunk(loc[0])[:loc[1]]
            out += part.ljust(p.data_bytes, b'\0') if cid < n else part
        out = out[:size]
        return bytes(out.ljust(size, b'\0'))

    def tree(self):
        """The tree (S-12.050), or None for a refused big-endian volume (S-12.052)."""
        if self.refused:
            return None
        out = {'lost+found': {'type': 'dir'}}
        for oid, o in sorted(self.live.items()):
            path = self.path_of(oid)
            t = o['type']
            if t == DIR:
                ent = {'type': 'dir'}
            elif t == FILE:
                ent = {'type': 'file', 'data': self.file_bytes(o)}
            elif t == SYMLINK:
                ent = {'type': 'symlink', 'alias': o['oh']['alias']}
            elif t == HARDLINK:
                ent = {'type': 'hardlink', 'target': self.path_of(o['equiv'])}
            else:
                ent = {'type': TYPE_NAMES.get(t, str(t))}
            out[path] = ent
        return out

    # ---- checker
    def check(self):
        """Consistency problems as strings; [] means the volume is consistent."""
        P = list(self.problems)
        p = self.p
        seqs = {}
        for b, info in self.blocks.items():
            if info['state'] != 'data':
                continue
            if info['seq'] in seqs:
                P.append(f'blocks {seqs[info["seq"]]} and {b} share sequence number {info["seq"]:#x}')
            seqs[info['seq']] = b
            erased_seen = None
            for page in range(p.chunks_per_block):
                t = self.tags(self.chunk_index(b, page))
                if t is None:
                    erased_seen = page if erased_seen is None else erased_seen
                    continue
                if erased_seen is not None:
                    P.append(f'block {b}: chunk {page} written after unused chunk {erased_seen}')
                    erased_seen = None
                if t['seq'] != info['seq']:
                    P.append(f'block {b} chunk {page}: seq {t["seq"]:#x}, block seq {info["seq"]:#x}')
                if not t['is_header'] and t['obj_id'] != OBJ_SUMMARY:
                    if t['chunk_id'] < 1 or t['n_bytes'] > p.data_bytes:
                        P.append(f'block {b} chunk {page}: data tags out of range')
            if info.get('summary'):
                summ = info['summ']
                for page in range(p.summary_entries):
                    t = self.tags(self.chunk_index(b, page))
                    s = summ[page]
                    if s is OWN:
                        continue
                    if t is None or (t and s and (t['raw_obj_id'], t['raw_chunk_id'], t['n_bytes'])
                                                       != (s['raw_obj_id'], s['raw_chunk_id'], s['n_bytes'])):
                        P.append(f'block {b} chunk {page}: summary disagrees with the chunk tags')
        for oid, o in self.live.items():
            t, h = o['tags'], o['oh']
            if h['type'] != t['type']:
                P.append(f'object {oid}: header type {h["type"]}, tags type {t["type"]}')
            if h['parent'] != t['parent']:
                P.append(f'object {oid}: header parent {h["parent"]}, tags parent {t["parent"]}')
            if not h['name']:
                P.append(f'object {oid}: empty name')
            if t['type'] == FILE and (h['size'] & 0xFFFFFFFF) != t['n_bytes']:
                P.append(f'object {oid}: header size {h["size"]}, tags size {t["n_bytes"]}')
            if t['type'] == HARDLINK:
                if h['equiv'] != t['n_bytes']:
                    P.append(f'object {oid}: header equiv {h["equiv"]}, tags equiv {t["n_bytes"]}')
                if o['equiv'] not in self.live:
                    P.append(f'object {oid}: hard link to missing object {o["equiv"]}')
            if t['type'] not in (FILE, SYMLINK, DIR, HARDLINK, 5):
                P.append(f'object {oid}: unknown type {t["type"]}')
            par = o['parent']
            if par not in (OBJ_ROOT, OBJ_LOST_FOUND) and (par not in self.live or self.live[par]['type'] != DIR):
                P.append(f'object {oid}: parent {par} is not a live directory (shown in lost+found)')
        seen = {}
        for oid, o in self.live.items():
            key = (o['parent'], o['oh']['name'])
            if key in seen:
                P.append(f'objects {seen[key]} and {oid}: same name in the same directory')
            seen[key] = oid
        return P
