#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-05.042's model: where the OP-1F's own write of a top-level os.ldr reads the file into its heap, from
a listing of the volume, and the pad file that makes that block start on a 32-byte boundary. Provisional
(S-05 open question 19). Python 3, standard library only; tools/mtpcp.py prepare-1f uses this module, so
the tool and the vectors cannot drift.

A listing is a list of entries {'path': 'user/a.bin', 'folder': bool, 'size': int}, as
tools/mtpcp.py tree writes them; paths have no leading slash.

    python3 -I -B landing_1f.py LISTING.json IMAGE_SIZE [BUILD]   # print the plan
"""
import json
import sys

# S-05.042: the builds the model is stated for, and each one's heap base. Any other gets no verdict.
HEAP_BASE = {'1000.1.1': 0x88A1CA70, '1000.2.0': 0x88A22B3C}
HEAP_LENGTH = 0x01FFFFFC
HEAP_GROW = 0x200
RESTS = (96, 2768)              # the free cell above the constructors: plain start, charging-wait USB first
USB_HOST, USB_NONE = 9544, 6920  # the USB stack's block above the mount, a computer on the cable or not
CHUNK = 4080                    # data bytes per chunk
BATCH = 100                     # records and index nodes are allocated this many at a time
YAFFS_OVER = 35                 # a yaffs request over-allocates by this much

PAD_FIRST, PAD_LAST, PAD_STEP = 1000000, 40000000, 100000
PAD_MIN_RECORD_HEADROOM = 3
PAD_NAME = 'waffle-pad.bin'
PAD_PREFIX, PAD_SUFFIX = 'waffle-pad', '.bin'  # an earlier pad of this tool: waffle-pad*.bin at the top
IMAGE_NAME = 'os.ldr'
LOGS = 'logs'
LOST_FOUND = 'lost+found'


class Refused(Exception):
    """The model gives no verdict, or no arrangement lands; the message says why."""


def need(s):
    """The heap cell a request of s bytes takes: an 8-byte header and the payload, rounded to 8."""
    return max((s + 15) & ~7, 16)


def need_yaffs(s):
    return need(s + YAFFS_OVER)


def chunks(size):
    return -(-size // CHUNK)


def nodes(size):
    """Index nodes for a file of `size` bytes."""
    c = chunks(size)
    n = (c >> 4) + 1
    level = 7
    while c >> (level - 3):
        n += (c >> level) + 1
        level += 3
    return n


def batches(count):
    return -(-count // BATCH)


def headroom(count):
    """How many more the count can take before it fills its last batch and needs another."""
    return batches(count) * BATCH - count


def build_of(version):
    """The bootloader build in an MTP DeviceVersion string ('1000.2.0' or 'bootloader 1000.2.0'), or
    None when it is not one the model is stated for."""
    text = (version or '').strip()
    if text.lower().startswith('bootloader '):
        text = text[len('bootloader '):].strip()
    return text if text in HEAP_BASE else None


def top_level(entry):
    return '/' not in entry['path']


def counted(entry):
    """lost+found is made by the mount; its row is not counted."""
    return not (top_level(entry) and entry['path'] == LOST_FOUND)


def counts(entries):
    """(files, folders, records, index nodes) of a listing."""
    files = [e for e in entries if counted(e) and not e['folder']]
    folders = [e for e in entries if counted(e) and e['folder']]
    return (len(files), len(folders), 4 + len(files) + len(folders),
            sum(nodes(e['size']) for e in files))


# ---------------------------------------------------------------- the allocator

class Heap:
    """A first-fit free list, walked as the bootloader's allocator: a search from the rover to the list's
    end and then from its head, the cell taken at its low end and split when 16 bytes or more are left,
    the top raised by max(HEAP_GROW, need) when nothing fits, a freed region merged with a free neighbour.
    A small request served from a free hole does not raise the top of use."""

    def __init__(self, base, length=HEAP_LENGTH):
        self.start = (base + 0x23) & ~7  # rounded down
        self.limit = self.start + length
        self.brk = self.start
        self.free = []          # [address, size], by address
        self.rover = 0          # index into free where the last search ended
        self.top = self.start   # one past the highest byte of a live cell

    def alloc(self, size):
        while True:
            n = len(self.free)
            for i in list(range(min(self.rover, n), n)) + list(range(min(self.rover, n))):
                at, have = self.free[i]
                if have < size:
                    continue
                if have - size >= 16:
                    self.free[i] = [at + size, have - size]
                    self.rover = i
                else:
                    size = have
                    del self.free[i]
                    self.rover = i if i < len(self.free) else 0
                self.top = max(self.top, at + size)
                return at
            self._grow(max(HEAP_GROW, size))

    def _grow(self, amount):
        if self.brk + amount > self.limit:
            raise Refused('the heap would overflow')
        at = self.brk
        self.brk += amount
        if self.free and sum(self.free[-1]) == at:
            self.free[-1][1] += amount
        else:
            self.free.append([at, amount])

    def leave_rest(self, rest):
        """The heap as the constructors leave it: one free cell of `rest` bytes at the top of use."""
        self.free = [[self.top, rest]]
        self.brk = self.top + rest
        self.rover = 0


def constructor_requests():
    reqs = [0x10]
    for a, b in ((0x20000, 0x1000), (0x2000, 0x1000), (0x100, 0x200), (0x2000, 0x1000)):
        reqs += [a + 31, b + 31, 0x10]
    return reqs


def mount_cells(record_batches, node_batches):
    """The mount's heap cells, in order: the device record (plain), the rest of the fixed part (yaffs),
    then one pair per batch. Each size is one request."""
    cells = [need(0xE94)]
    cells += [need_yaffs(s) for s in [4096] * 6 + [10 * 0x1C] + [4096] * 10 + [
        128 * 4, 4608 * 8, 4608 * 16, 0x24, 100 * 0x88, 8, 127 * 12, 127 * 12, 4096, 4 * 289]]
    cells += [need_yaffs(100 * 40), need_yaffs(8)] * node_batches
    cells += [need_yaffs(100 * 0x88), need_yaffs(8)] * max(record_batches - 1, 0)
    return cells


def top_of_use(build, rest, records, index_nodes):
    """The top of use after the mount, for a start whose constructors left `rest` free bytes."""
    heap = Heap(HEAP_BASE[build])
    for s in constructor_requests():
        heap.alloc(need(s))
    heap.leave_rest(rest)
    for c in mount_cells(batches(records), batches(index_nodes)):
        heap.alloc(c)
    return heap.top


def block(build, rest, records, index_nodes, usb=USB_HOST):
    """The address of the block the write reads os.ldr into."""
    return top_of_use(build, rest, records, index_nodes) + usb + 8


def check_build(build):
    if build not in HEAP_BASE:
        raise Refused('bootloader %r: the model is stated only for %s' % (build, ', '.join(sorted(HEAP_BASE))))


def verdict_counts(build, records, index_nodes, image_size=None):
    """What the model says of a volume of `records` records and `index_nodes` index nodes."""
    check_build(build)
    tops = {rest: top_of_use(build, rest, records, index_nodes) for rest in RESTS}
    blocks = {rest: tops[rest] + USB_HOST + 8 for rest in RESTS}
    if image_size is not None:
        for b in blocks.values():
            if b + image_size > HEAP_BASE[build] + HEAP_LENGTH:
                raise Refused('an image of %d bytes does not fit in the heap' % image_size)
    return {
        'records': records, 'nodes': index_nodes,
        'record_batches': batches(records), 'node_batches': batches(index_nodes),
        'record_headroom': headroom(records), 'node_headroom': headroom(index_nodes),
        'nodes_exact_multiple': index_nodes % BATCH == 0,
        'tops': tops, 'blocks': blocks,
        'residues': {rest: blocks[rest] % 32 for rest in RESTS},
        'lands': all(b % 32 == 0 for b in blocks.values()),
    }


def verdict(build, entries, image_size=None):
    """What the model says of a listing as it stands."""
    check_build(build)
    files, folders, records, index_nodes = counts(entries)
    v = verdict_counts(build, records, index_nodes, image_size)
    v.update(files=files, folders=folders)
    return v


def pad_candidates_counts(build, records, index_nodes, limit=None):
    """(size, verdict) of each pad size searched that lands and does not end on a whole batch of nodes,
    for a volume of these counts without the pad: the pad adds one record and its own index nodes."""
    out = []
    for p in range(PAD_FIRST, PAD_LAST + 1, PAD_STEP):
        if limit is not None and p > limit:
            break
        v = verdict_counts(build, records + 1, index_nodes + nodes(p))
        if v['lands'] and not v['nodes_exact_multiple']:
            out.append((p, v))
    return out


def choose_pad(build, records, index_nodes, limit=None):
    """The pad S-05.042 picks: of those that land with a record margin of at least
    PAD_MIN_RECORD_HEADROOM, the largest index-node margin, then the smallest size."""
    found = pad_candidates_counts(build, records, index_nodes, limit)
    good = [c for c in found if c[1]['record_headroom'] >= PAD_MIN_RECORD_HEADROOM]
    if not good:
        raise Refused('no pad from %d to %d bytes lands with a record margin of at least %d'
                      % (PAD_FIRST, PAD_LAST if limit is None else min(PAD_LAST, limit),
                         PAD_MIN_RECORD_HEADROOM))
    return max(good, key=lambda c: (c[1]['node_headroom'], -c[0]))[0]


# ---------------------------------------------------------------- the arrangement

def is_old_pad(entry):
    name = entry['path']
    return (top_level(entry) and not entry['folder'] and name.startswith(PAD_PREFIX)
            and name.endswith(PAD_SUFFIX))


def removals(entries):
    """The files the arrangement removes: those directly in the top-level logs/, and earlier pads."""
    return [e for e in entries if not e['folder'] and (
        (e['path'].startswith(LOGS + '/') and e['path'].count('/') == 1) or is_old_pad(e))]


def arranged(entries, image_size, pad_size=None):
    """The listing after the arrangement: removals gone, os.ldr at the image's size, the pad if any."""
    gone = {e['path'] for e in removals(entries)}
    out = [dict(e) for e in entries if e['path'] not in gone and e['path'] != IMAGE_NAME]
    out.append({'path': IMAGE_NAME, 'folder': False, 'size': image_size})
    if pad_size is not None:
        out.append({'path': PAD_NAME, 'folder': False, 'size': pad_size})
    return sorted(out, key=lambda e: e['path'])


def plan(build, entries, image_size, pad_limit=None):
    """The arrangement for a listing and an image: what to remove, the pad (None when the volume lands
    without one), the listing the unit must then show, the verdict on it and any warnings."""
    check_build(build)
    for e in entries:
        if e['path'] in (IMAGE_NAME, PAD_NAME) and e['folder']:
            raise Refused('/%s is a folder' % e['path'])
    base = arranged(entries, image_size)
    warnings = []
    plain = verdict(build, base, image_size)
    pad = None
    if plain['nodes_exact_multiple']:
        warnings.append('without a pad the index nodes come to %d, an exact multiple of %d, which a '
                        'listing cannot place; a pad moves them off it' % (plain['nodes'], BATCH))
    if not plain['lands'] or plain['nodes_exact_multiple']:
        pad = choose_pad(build, plain['records'], plain['nodes'], pad_limit)
    final = arranged(entries, image_size, pad)
    v = verdict(build, final, image_size)
    if not v['lands']:
        raise Refused('the arrangement does not land')       # the search above makes this unreachable
    if v['record_headroom'] < PAD_MIN_RECORD_HEADROOM:
        warnings.append('only %d more records before another record batch moves the block'
                        % v['record_headroom'])
    return {'build': build, 'image_size': image_size, 'remove': [e['path'] for e in removals(entries)],
            'pad': pad, 'listing': final, 'unpadded': plain, 'verdict': v, 'warnings': warnings}


def rows(entries):
    """A listing as comparable rows."""
    return sorted((e['path'], bool(e['folder']), 0 if e['folder'] else int(e['size'])) for e in entries)


def describe(p):
    v = p['verdict']
    lines = ['bootloader %s, os.ldr %d bytes' % (p['build'], p['image_size']),
             'remove: %s' % (', '.join(p['remove']) or 'nothing'),
             'pad: %s' % ('none, the volume lands as it stands' if p['pad'] is None
                          else '%s, %d bytes' % (PAD_NAME, p['pad'])),
             '%d files, %d folders: records %d (%d batches), index nodes %d (%d batches)'
             % (v['files'], v['folders'], v['records'], v['record_batches'], v['nodes'], v['node_batches']),
             'block %s' % ', '.join('0x%08X (rest %d, %% 32 = %d)' % (v['blocks'][r], r, v['residues'][r])
                                    for r in RESTS),
             'margins: %d index nodes, %d records before the next batch'
             % (v['node_headroom'], v['record_headroom'])]
    return lines + ['warning: %s' % w for w in p['warnings']]


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[0]) as f:
        data = json.load(f)
    entries = data['entries'] if isinstance(data, dict) else data
    try:
        p = plan(argv[2] if len(argv) == 3 else '1000.2.0', entries, int(argv[1]))
    except Refused as e:
        print('refused: %s' % e)
        return 1
    print('\n'.join(describe(p)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
