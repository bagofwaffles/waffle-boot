#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes vectors/landing_1f.json, S-05.042's cases: its reference values at the counts level, synthetic
listings (generated names only) and the run listing in vectors/landing_1f_run.json (a real volume with every
name replaced), each with its counts, batches, the block per rest and its residue, and the plan
landing_1f.py makes. The counts are made here
again, independently of the model, and must agree. Deterministic.

    python3 -I -B gen_landing_1f.py
"""
import hashlib
import json
import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import landing_1f as L  # noqa: E402

OUT = os.path.join(HERE, 'vectors', 'landing_1f.json')
RUN = os.path.join(HERE, 'vectors', 'landing_1f_run.json')
RUN_IMAGE = 2104908             # the run's os.ldr, as its listing shows it
RUN_PAD = 'pad-run.bin'         # the run's pad, 6,200,000 bytes, at the top


def nodes_by_hand(size):
    """Index nodes, stated as a loop over levels; independent of landing_1f.nodes()."""
    chunks = (size + 4079) // 4080
    total = chunks // 16 + 1
    shift = 7
    while chunks // (1 << (shift - 3)) > 0:
        total += chunks // (1 << shift) + 1
        shift += 3
    return total


def counts_by_hand(listing):
    files = folders = node_total = 0
    for path, folder, size in listing:
        if path == 'lost+found':
            continue
        if folder:
            folders += 1
        else:
            files += 1
            node_total += nodes_by_hand(size)
    return files, folders, 4 + files + folders, node_total


def entries(listing):
    return [{'path': p, 'folder': f, 'size': s} for p, f, s in listing]


def volume(rng, files, folders, logs=0, big=0, depth=4):
    """A synthetic listing: folders in a tree, small and some large files, a lost+found row."""
    listing = [['lost+found', False, 4080]]
    dirs = ['']
    if logs is not None:
        listing.append(['logs', True, 0])
        for i in range(logs):
            listing.append(['logs/log%03d.txt' % i, False, rng.randrange(100, 60000)])
    for i in range(folders - (logs is not None)):
        parent = rng.choice([d for d in dirs if d.count('/') < depth])
        path = (parent + '/' if parent else '') + 'd%03d' % i
        dirs.append(path)
        listing.append([path, True, 0])
    for i in range(files - (logs or 0)):
        parent = rng.choice(dirs)
        size = rng.randrange(5000000, 30000000) if i < big else rng.choice(
            (0, 4, rng.randrange(1, 4080), rng.randrange(4080, 200000), rng.randrange(200000, 2000000)))
        listing.append([(parent + '/' if parent else '') + 'f%04d.bin' % i, False, size])
    return sorted(listing)


def verdict_row(build, listing):
    v = L.verdict(build, entries(listing))
    assert (v['files'], v['folders'], v['records'], v['nodes']) == counts_by_hand(listing)
    return {'files': v['files'], 'folders': v['folders'], 'records': v['records'], 'nodes': v['nodes'],
            'record_batches': v['record_batches'], 'node_batches': v['node_batches'],
            'record_headroom': v['record_headroom'], 'node_headroom': v['node_headroom'],
            'blocks': {str(r): '0x%08X' % v['blocks'][r] for r in L.RESTS},
            'residues': {str(r): v['residues'][r] for r in L.RESTS}, 'lands': v['lands']}


def case(name, build, listing, image_size, note, listing_ref=None):
    row = {'name': name, 'note': note, 'build': build, 'image_size': image_size}
    if listing_ref:
        row.update(listing_ref)
    else:
        row['listing'] = listing
    try:
        p = L.plan(build, entries(listing), image_size)
    except L.Refused as e:
        row['refused'] = str(e)
        if build in L.HEAP_BASE:
            row['as_listed'] = verdict_row(build, listing)
        return row
    row['as_listed'] = verdict_row(build, listing)
    final = [[e['path'], e['folder'], e['size']] for e in p['listing']]
    row['plan'] = {'remove': p['remove'], 'pad': p['pad'], 'pad_name': L.PAD_NAME if p['pad'] else None,
                   'unpadded': verdict_row(build, [[e['path'], e['folder'], e['size']]
                                                   for e in L.arranged(entries(listing), image_size)]),
                   'arranged': verdict_row(build, final), 'warnings': p['warnings']}
    assert row['plan']['arranged']['lands']
    return row


# S-05.042's reference values (1000.2.0, a computer on the cable): name, records, index nodes, then the
# top of use and the block for each rest, and whether it lands. Written out by hand; the model must hit them.
REFERENCE = [
    ('open_field', 1117, 8701, {96: (0x88AF8C50, 0x88AFB1A0), 2768: (0x88AF8C70, 0x88AFB1C0)}, True),
    ('refused_volume', 1249, 9401, {96: (0x88B03218, 0x88B05768), 2768: (0x88B03238, 0x88B05788)}, False),
    ('refused_base_with_6300000_pad', 1249, 8788 + 113,
     {96: (0x88AFE1F0, 0x88B00740), 2768: (0x88AFE210, 0x88B00760)}, True),
]
# The refused volume before its pad, at the counts level: records and index nodes with os.ldr counted.
REFUSED_BASE = (1248, 8788)


def constructor_checkpoint():
    """S-05.042's checkpoint before the mount: the heap's start per build, and the top of use the static
    constructors leave on 1000.2.0."""
    starts = {b: L.Heap(a).start for b, a in L.HEAP_BASE.items()}
    assert starts == {'1000.1.1': 0x88A1CA90, '1000.2.0': 0x88A22B58}
    heap = L.Heap(L.HEAP_BASE['1000.2.0'])
    for s in L.constructor_requests():
        heap.alloc(L.need(s))
    assert heap.top == 0x88A4A010 == starts['1000.2.0'] + 160952
    return {'start': {b: '0x%08X' % a for b, a in sorted(starts.items())},
            'top_after_constructors': {'1000.2.0': '0x%08X' % heap.top}}


def reference_rows():
    out = []
    for name, records, index_nodes, want, lands in REFERENCE:
        v = L.verdict_counts('1000.2.0', records, index_nodes)
        for rest, (top, blk) in want.items():
            assert (v['tops'][rest], v['blocks'][rest]) == (top, blk), (name, rest, hex(v['blocks'][rest]))
        assert v['lands'] == lands
        out.append({'name': name, 'build': '1000.2.0', 'records': records, 'nodes': index_nodes,
                    'record_batches': v['record_batches'], 'node_batches': v['node_batches'],
                    'tops': {str(r): '0x%08X' % want[r][0] for r in L.RESTS},
                    'blocks': {str(r): '0x%08X' % want[r][1] for r in L.RESTS},
                    'residues': {str(r): want[r][1] % 32 for r in L.RESTS}, 'lands': lands})
    records, index_nodes = REFUSED_BASE
    pad = L.choose_pad('1000.2.0', records, index_nodes)
    v = L.verdict_counts('1000.2.0', records + 1, index_nodes + nodes_by_hand(pad))
    out.append({'name': 'refused_base_pad_chosen', 'build': '1000.2.0', 'base_records': records,
                'base_nodes': index_nodes, 'pad': pad, 'records': v['records'], 'nodes': v['nodes'],
                'record_headroom': v['record_headroom'], 'node_headroom': v['node_headroom'],
                'blocks': {str(r): '0x%08X' % v['blocks'][r] for r in L.RESTS},
                'residues': {str(r): v['residues'][r] for r in L.RESTS}, 'lands': v['lands']})
    return out


def find(rng, want, **kw):
    """The first synthetic volume for which want(volume) holds; files is a range to draw from."""
    first, last = kw.pop('files')
    for _ in range(5000):
        listing = volume(rng, rng.randrange(first, last), **kw)
        if want(listing):
            return listing
    raise SystemExit('gen_landing_1f: no volume found for a case')


def main():
    with open(RUN) as f:
        raw = f.read()
    run = [list(r) for r in json.loads(raw)['entries']]
    ref = {'listing_file': 'landing_1f_run.json',
           'listing_sha256': hashlib.sha256(raw.encode()).hexdigest()}
    no_pad = [r for r in run if r[0] != RUN_PAD]
    rng = random.Random(0x1F042)
    cases = [
        case('run_as_written', '1000.2.0', run, RUN_IMAGE,
             'the run listing as it stood at the write that landed: lands as it stands, no pad', ref),
        case('run_without_its_pad', '1000.2.0', no_pad, RUN_IMAGE,
             'the run listing with its 6,200,000-byte pad removed: the pad chosen is that size again',
             dict(ref, drop=[RUN_PAD])),
        case('run_on_1000_1_1', '1000.1.1', run, RUN_IMAGE,
             'the same listing on the other named build: the heap base moves the block', ref),
        case('refused_build', '1000.3.0', no_pad, RUN_IMAGE, 'a build the model is not stated for',
             dict(ref, drop=[RUN_PAD])),
        case('refused_empty_build', '', no_pad, RUN_IMAGE, 'no build in the device information',
             dict(ref, drop=[RUN_PAD])),
    ]
    small = volume(rng, 14, 4, logs=3)
    cases.append(case('small_volume', '1000.2.0', small, 877092,
                      'a volume of 18 objects with three log files: removed, then padded'))
    old = volume(rng, 900, 100, logs=12, big=4) + [['os.ldr', False, 2079016],
                                                   ['waffle-pad-old.bin', False, 3000000],
                                                   ['waffle-pad.bin', False, 6100000]]
    cases.append(case('logs_old_pads_old_image', '1000.2.0', sorted(old), 2104908,
                      'log files, two earlier pads of this tool and an old os.ldr are replaced'))

    def lands_plain(listing, image=1500000):
        v = L.verdict('1000.2.0', L.arranged(entries(listing), image))
        return v['lands'] and not v['nodes_exact_multiple'] and v['record_headroom'] >= 3
    cases.append(case('lands_as_it_stands', '1000.2.0', find(rng, lands_plain, files=(400, 600), folders=60, logs=0,
                                                              big=3), 1500000,
                      'a volume that lands with no pad: none is added'))

    def exact(listing, image=1500000):
        v = L.verdict('1000.2.0', L.arranged(entries(listing), image))
        return v['nodes_exact_multiple'] and v['lands']
    cases.append(case('nodes_exact_multiple', '1000.2.0', find(rng, exact, files=(300, 500), folders=50, logs=None),
                      1500000, 'index nodes at an exact multiple of 100 without a pad, where it would land: a '
                      'warning, and a pad moves them off it'))

    def thin_records(listing, image=1500000):
        v = L.verdict('1000.2.0', L.arranged(entries(listing), image))
        return v['record_headroom'] in (1, 2, 3) and not v['lands']
    cases.append(case('no_record_margin', '1000.2.0', find(rng, thin_records, files=(600, 800), folders=90, logs=None),
                      1500000, 'a pad adds a record: with a record margin of 3 or less no pad keeps 3'))
    deep = volume(rng, 1300, 200, logs=40, big=6, depth=6)
    cases.append(case('large_volume', '1000.2.0', deep, 2104908, 'a volume of 1500 objects in deeper folders'))

    sizes = [0, 1, 4080, 4081, 16 * 4080, 16 * 4080 + 1, 128 * 4080, 1024 * 4080, 1024 * 4080 + 1, 6200000,
             6300000, 34800000, 40000000, 8192 * 4080 + 5]
    for s in sizes:
        assert L.nodes(s) == nodes_by_hand(s)
    data = {
        'document': 'S-05', 'requirement': 'S-05.042', 'provisional': True,
        'model': 'landing_1f.py',
        'constants': {'heap_base': {b: '0x%08X' % a for b, a in sorted(L.HEAP_BASE.items())},
                      'heap_length': '0x%08X' % L.HEAP_LENGTH, 'grow': '0x%X' % L.HEAP_GROW,
                      'rests': list(L.RESTS), 'usb_host': L.USB_HOST, 'usb_none': L.USB_NONE,
                      'chunk': L.CHUNK, 'batch': L.BATCH,
                      'pad_search': [L.PAD_FIRST, L.PAD_LAST, L.PAD_STEP],
                      'pad_min_record_headroom': L.PAD_MIN_RECORD_HEADROOM, 'pad_name': L.PAD_NAME},
        'nodes': [[s, (s + 4079) // 4080, L.nodes(s)] for s in sizes],
        'before_the_mount': constructor_checkpoint(),
        'reference': reference_rows(),
        'cases': cases,
    }
    text = json.dumps(data, indent=1, sort_keys=False)
    # A listing row on one line keeps the file short and readable.
    text = re.sub(r'\[\n\s+("[^"\n]*"),\n\s+(true|false),\n\s+(\d+)\n\s+\]', r'[\1, \2, \3]', text)
    text = re.sub(r'\[\n\s+(\d+),\n\s+(\d+),\n\s+(\d+)\n\s+\]', r'[\1, \2, \3]', text)
    with open(OUT, 'w') as f:
        f.write(text + '\n')


if __name__ == '__main__':
    main()
