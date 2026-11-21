#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates the S-03 vectors from model.py and the conformance package. OS images are seeded synthetic
LDR streams; a partition marked encrypted is a header byte over a plaintext image (S-03.048).

    python3 -I tests/oracle/S-03-emmc/gen_vectors.py              # (re)writes vectors/*.json
    python3 -I tests/oracle/S-03-emmc/gen_vectors.py --images DIR # also DIR/<row>.boot1/.boot2 per load row
"""
import functools
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import model as M  # noqa: E402
from images import emmc  # noqa: E402
from images.crc import crc32  # noqa: E402

B = emmc.BOOT_PARTITION_SIZE          # 4 MiB, a test choice (S-03.008)
VEC = os.path.join(HERE, 'vectors')


@functools.lru_cache(maxsize=None)
def img(seed, size):
    return emmc.synthetic_os(seed=seed, size=size)


def sha(b):
    return hashlib.sha256(b).hexdigest()


# ---- headers.json (S-03.021-S-03.026) ----

def headers():
    rows = []

    def add(name, req, **kw):
        h = M.pack_header(**kw)
        p = M.parse_header(h)
        rows.append({'name': name, 'req': req, 'hex': h.hex(), 'expect': p})

    add('writer_plain', 'S-03.021', format=3, length=0x200100, crc=0x12345678, active=1, encrypted=0)
    add('format3_enc', 'S-03.024, S-03.048', format=3, length=0x200100, crc=0x12345678, active=1, encrypted=1)
    add('erased', 'S-03.022', format=0, length=0, crc=0, active=0, encrypted=0)
    for a in (2, 0x80, 0xFF):
        add('active_%02x' % a, 'S-03.022', format=3, length=0x3000, crc=1, active=a)
    add('inactive_valid', 'S-03.022', format=3, length=0x3000, crc=1, active=0)
    add('format0_enc_ignored', 'S-03.023', format=0, length=0x3000, crc=1, active=1, encrypted=1)
    add('format1_enc_ignored', 'S-03.023', format=1, length=0x3000, crc=1, active=1, encrypted=1)
    add('format2_enc', 'S-03.023', format=2, length=0x3000, crc=1, active=1, encrypted=1)
    add('format2_plain', 'S-03.023', format=2, length=0x3000, crc=1, active=1, encrypted=0)
    add('format3_enc_byte_7f', 'S-03.024', format=3, length=0x3000, crc=1, active=1, encrypted=0x7F)
    add('format4_refused', 'S-03.023, S-03.042', format=4, length=0x3000, crc=1, active=1, encrypted=1)
    add('format_max_refused', 'S-03.023, S-03.042', format=0xFFFFFFFF, length=0x3000, crc=1, active=1)
    add('reserved_ff', 'S-03.026', format=3, length=0x3000, crc=0xCBF43926, active=1, encrypted=0, reserved=0xFF)
    add('length_max', 'S-03.021', format=3, length=0xFFFFFFFF, crc=0xFFFFFFFF, active=1)
    return {
            'crc_check': {'data': '123456789', 'crc32': '%08x' % crc32(b'123456789')},
            'vectors': rows}


# ---- load_table.json (S-03.040-S-03.047) ----

L_A, L_B = 12288, 12544               # a 512-multiple and a non-multiple length
L_LONG = 0x20000 + 700                # 258 sectors: more than 256 sectors
L_TWO = 0x80000 + 0x1000              # past erase group 0

STATES = {
    # name: (description, installer(device, part, seed))
    'erased': ('header and image erased (all zero)', None),
    'good': ('active, valid image', lambda c, p, s: c.install(p, img(s, L_B))),
    'good512': ('active, valid image, length a multiple of 512', lambda c, p, s: c.install(p, img(s, L_A))),
    'inactive_good': ('valid image, active byte 0', lambda c, p, s: c.install(p, img(s, L_B), active=0)),
    'bad_crc': ('active, one image byte inverted after the CRC was taken', None),
    'zero_length': ('active, length 0, crc 0', lambda c, p, s: c.install(p, b'', header=M.pack_header(3, 0, 0, 1, 0))),
    'too_long': ('active, length 0x800001', lambda c, p, s: c.install(p, b'', header=M.pack_header(3, 0x800001, 0, 1, 0))),
    'past_partition': ('active, length B - 511: the stored region would end past the partition',
                       lambda c, p, s: c.install(p, b'', header=M.pack_header(3, B - 511, 0, 1, 0))),
    'pad_garbage': ('active, valid image; 0xA5 padding and 0xFF reserved bytes',
                    lambda c, p, s: c.install(p, img(s, L_B), pad=0xA5, reserved=0xFF)),
    'active_ff': ('active byte 0xFF, valid image',
                  lambda c, p, s: c.install(p, img(s, L_B), header=M.pack_header(3, L_B, crc32(img(s, L_B)), 0xFF, 0))),
    'enc_f3': ('active, format 3, encrypted byte 1 over a plaintext image whose CRC passes: not loaded (S-03.048)',
               lambda c, p, s: c.install(p, img(s, L_B), encrypted=1)),
    'enc_f2': ('active, format 2, encrypted byte 1 over a plaintext image whose CRC passes: not loaded (S-03.048)',
               lambda c, p, s: c.install(p, img(s, 0x10000 + 4096), encrypted=1, format=2)),
    'f0_enc_byte': ('format 0 with encrypted byte 1 over a plaintext image',
                    lambda c, p, s: c.install(p, img(s, L_B), header=M.pack_header(0, L_B, crc32(img(s, L_B)), 1, 1))),
    'partition_full': ('active, valid image of length B - 512: the stored region ends at the partition\'s last sector',
                       lambda c, p, s: c.install(p, img(s, B - 512))),
    'written_plain': ('the partition as the writer leaves its target (S-03.033): format 3, encrypted 0',
                      lambda c, p, s: _written(c, p, s)),
    'format4': ('active, format 4, encrypted 0, a valid plaintext image: fails (S-03.042)',
                lambda c, p, s: c.install(p, img(s, L_B), header=M.pack_header(4, L_B, crc32(img(s, L_B)), 1, 0))),
    'format_max': ('active, format 0xFFFFFFFF, encrypted 0, a valid plaintext image: fails (S-03.042)',
                   lambda c, p, s: c.install(p, img(s, L_B),
                                             header=M.pack_header(0xFFFFFFFF, L_B, crc32(img(s, L_B)), 1, 0))),
}


def _written(c, p, s):
    """Runs the model writer from an erased eMMC (target BP1) and puts its BP1 into `p`."""
    w = M.Emmc(c.B)
    result, _, target = M.write(w, img(s, L_B))
    assert (result, target) == ('ok', M.BP1)
    c.boot[p][:] = w.boot[M.BP1]

@functools.lru_cache(maxsize=None)
def _partition(state, seed):
    c = M.Emmc(B)
    if state == 'bad_crc':
        c.install(M.BP1, img(seed, L_B))
        c.boot[M.BP1][M.SECTOR + 100] ^= 0xFF
    elif state != 'erased':
        STATES[state][1](c, M.BP1, seed)
    return bytes(c.boot[M.BP1])


def build(state, part, device, seed):
    device.boot[part][:] = _partition(state, seed)


P1_STATES = ['erased', 'good', 'good512', 'inactive_good', 'bad_crc', 'zero_length', 'too_long',
             'past_partition', 'pad_garbage', 'active_ff', 'enc_f3', 'enc_f2', 'f0_enc_byte', 'partition_full',
             'written_plain', 'format4', 'format_max']
P2_BASE = ['erased', 'good', 'bad_crc', 'inactive_good']


def load_rows():
    """Every BP1 state against the base BP2 states, and every other BP2 state behind an inactive BP1."""
    rows = [(a, b) for a in P1_STATES for b in P2_BASE]
    rows += [(a, b) for b in P1_STATES if b not in P2_BASE for a in ('erased', 'inactive_good')]
    return rows


def load_table(images_dir=None):
    out = []
    for a, b in load_rows():
        c = M.Emmc(B)
        build(a, M.BP1, c, 1)
        build(b, M.BP2, c, 2)
        r = M.load(c)
        rf = M.load(c, fallback=True)
        name = 'p1_%s__p2_%s' % (a, b)
        row = {'name': name, 'p1': a, 'p2': b,
               'p1_header': c.read(M.BP1, 0, 1)[:16].hex(), 'p2_header': c.read(M.BP2, 0, 1)[:16].hex(),
               'p1_sha256': sha(bytes(c.boot[M.BP1])), 'p2_sha256': sha(bytes(c.boot[M.BP2])),
               'expect': r['outcome'], 'reason': r['reason'], 'expect_if_fallback': rf['outcome'],
               'selects': r['selects'], 'header_reads': r['header_reads'], 'extent': r['extent'],
               'loaded_length': len(r['image']) if r['image'] is not None else 0,
               'loaded_crc32': '%08x' % crc32(r['image']) if r['image'] is not None else None}
        out.append(row)
        if images_dir:
            os.makedirs(images_dir, exist_ok=True)
            for part, ext in ((M.BP1, 'boot1'), (M.BP2, 'boot2')):
                with open(os.path.join(images_dir, '%s.%s' % (name, ext)), 'wb') as f:
                    f.write(c.boot[part])
    return {
            'B': B, 'states': {k: v[0] for k, v in STATES.items()}, 'rows': out}


# ---- write_sequences.json (S-03.050-S-03.060) ----

def start_state(name, old):
    c = M.Emmc(B)
    if name == 'p1_active':
        c.install(M.BP1, old)
    elif name == 'p1_inactive_p2_active':
        c.install(M.BP1, img(9, L_B), active=0)
        c.install(M.BP2, old)
    elif name == 'both_active':
        c.install(M.BP1, old)
        c.install(M.BP2, img(9, L_B))
    elif name == 'neither':
        pass
    elif name == 'p1_bad':
        c.install(M.BP1, old)
        c.boot[M.BP1][M.SECTOR + 7] ^= 0xFF
    else:
        raise ValueError(name)
    return c


TORN_MAX = 0x100000          # torn outcomes are computed for cases up to this length (cost only)


def write_case(name, start, length, corrupt_sector=None, corrupt_header=None, deactivate='rewrite', fail=None):
    old = img(3, 16384)
    new = img(4, length) if length >= 0x30 else bytes(length)
    c = start_state(start, old)
    before = M.boots(c, old, new)
    kw = dict(corrupt_sector=corrupt_sector, corrupt_header=corrupt_header, deactivate=deactivate, fail=fail)
    result, ops, target = M.write(c, new, **kw)
    row = {'name': name, 'start': start, 'length': length,
           'corrupt_sector': corrupt_sector, 'corrupt_header': corrupt_header, 'deactivate': deactivate,
           'fail': fail, 'boots_before': before, 'result': result, 'target': target,
           'ops': [list(o) for o in ops]}
    if result == 'refused':
        return row
    row['last_writable_sector'] = M.last_writable_sector(length, B)
    hdr = M.pack_header(3, length, crc32(new), 1, 0)
    row['header'] = hdr[:16].hex()
    row['final'] = {'p1_active': M.parse_header(c.read(M.BP1, 0, 1))['active'],
                    'p2_active': M.parse_header(c.read(M.BP2, 0, 1))['active'],
                    'boots': M.boots(c, old, new),
                    'boots_if_fallback': M.boots(c, old, new, fallback=True)}
    muts = sum(1 for o in ops if o[0] in ('erase', 'write'))
    cuts = []
    for k in range(1, muts + 1):
        cc = start_state(start, old)
        res, cops, _ = M.write(cc, new, cut_after=k, **kw)
        cuts.append({'after': k, 'op': list(cops[-1]), 'boots': M.boots(cc, old, new),
                     'boots_if_fallback': M.boots(cc, old, new, fallback=True)})
    row['cuts'] = cuts
    if length <= TORN_MAX:
        torn = []
        for k in range(1, muts + 1):
            o, ofb, op = M.torn_outcomes(start_state(start, old), new, old, k, **kw)
            torn.append({'op_index': k, 'op': list(op), 'boots': o, 'boots_if_fallback': ofb})
        row['torn'] = torn
    return row


def write_sequences():
    cases = []
    for start in ('p1_active', 'p1_inactive_p2_active', 'both_active', 'neither', 'p1_bad'):
        for length in (L_A, L_B):
            cases.append(write_case('%s_%d' % (start, length), start, length))
    cases.append(write_case('large_two_groups', 'p1_active', L_TWO))
    cases.append(write_case('corrupt_sector5', 'p1_active', L_A, corrupt_sector=5))
    cases.append(write_case('corrupt_last_sector_512_multiple', 'p1_active', L_A, corrupt_sector=L_A // 512))
    cases.append(write_case('corrupt_last_partial_sector', 'p1_active', L_B, corrupt_sector=M.n_sectors(L_B)))
    cases.append(write_case('corrupt_sector5_target_p1', 'p1_inactive_p2_active', L_A, corrupt_sector=5))
    cases.append(write_case('corrupt_header', 'p1_inactive_p2_active', L_B, corrupt_header='inactive'))
    cases.append(write_case('corrupt_header_active', 'p1_inactive_p2_active', L_B, corrupt_header='active'))
    cases.append(write_case('corrupt_header_target_p2', 'p1_active', L_B, corrupt_header='inactive'))
    cases.append(write_case('corrupt_header_active_target_p2', 'p1_active', L_B, corrupt_header='active'))
    cases.append(write_case('refused_too_small', 'p1_active', 10000 - 512 - 4))
    cases.append(write_case('accepted_min', 'p1_active', 10000 - 512))
    cases.append(write_case('refused_past_partition', 'p1_active', B - 511))
    cases.append(write_case('accepted_partition_full', 'p1_active', B - 512))
    cases.append(write_case('extra_sector_group_edge', 'p1_active', (M.GROUP - 1) * 512))
    cases.append(write_case('p1_active_long', 'p1_active', L_LONG))
    cases.append(write_case('p1_inactive_p2_active_long', 'p1_inactive_p2_active', L_LONG))
    for f in M.WRITE_FAILS:
        for start in ('p1_active', 'p1_inactive_p2_active'):
            if f == 'select2' and start != 'p1_active':
                continue                    # no select 2 when the target is BP1
            # an erase only from group 1 on: the erase's failure needs an image past group 0
            cases.append(write_case('fail_%s__%s' % (f, start), start, L_TWO if f == 'erase' else L_B, fail=f))
    for f in ('select_other', 'read_other', 'deactivate'):   # step 8 from the other starting states (S-03.062)
        for start in ('both_active', 'neither', 'p1_bad'):
            cases.append(write_case('fail_%s__%s' % (f, start), start, L_B, fail=f))
    return {
            'B': B, 'cases': cases, 'erased_ff': erased_ff()}


def erased_ff():
    """S-03.055 on an eMMC whose erased bytes read 0xFF (ERASED_MEM_CONT 1): the boot after a cut at every
    erase or write boundary of a clean write, each start state, both lengths and one past group 0. The
    target's group 0 is never erased, so no cut leaves a header of 0xFF bytes that reads active."""
    out = []
    for start in ('p1_active', 'p1_inactive_p2_active', 'both_active', 'neither', 'p1_bad'):
        for length in (L_A, L_B, L_TWO):
            old, new = img(3, 16384), img(4, length)

            def dev():
                c = start_state(start, old)
                ff = M.Emmc(B, erased=0xFF)
                ff.boot = c.boot
                return ff
            c = dev()
            result, ops, _ = M.write(c, new)
            muts = sum(1 for o in ops if o[0] in ('erase', 'write'))
            boots = []
            for k in range(1, muts + 1):
                cc = dev()
                M.write(cc, new, cut_after=k)
                boots.append(M.boots(cc, old, new))
            out.append({'start': start, 'length': length, 'result': result, 'final': M.boots(c, old, new),
                        'cuts': boots})
    return out


# ---- user_area.json (S-03.001, S-03.070-S-03.073) ----

def user_area():
    pairs = [0, 1, 0xCF7FFF, 0xCF8000, 0xCF8001, 0xFFFFFF]
    return {'emmc': {'sectors': 0x1CF8000, 'erase_group_sectors': 0x400, 'write_size_divides': 8,
                     'refused': [{'sectors': 0x1D2F000, 'erase_group_sectors': 0x400, 'opt_write': 8},
                                 {'sectors': 0x1CF8000, 'erase_group_sectors': 0x200, 'opt_write': 8},
                                 {'sectors': 0x1CF8000, 'erase_group_sectors': 0x400, 'opt_write': 16}],
                     'accepted_opt_write': [1, 2, 4, 8]},
            'map': [{'region': r, 'first': f, 'last': l} for r, f, l in emmc.USER_LAYOUT],
            'yaffs2': {'first_block': 0, 'last_block': 0x11FF, 'sectors_per_block': 0x400,
                       'sectors_per_chunk': 8},
            'fat_map': [{'volume_sector': s, 'user_sector': emmc.fat_to_physical(s)} for s in pairs],
            'factory_reset_erase': {'group': 0x4000, 'first': 0x1000000, 'last': 0x10003FF}}


# ---- select_1f.json (S-03.012) ----

SELECT_1F_PRESETS = [0x00, 0x08, 0x48, 0x49, 0x4A, 0x38]


def select_1f():
    rows = []
    for cur in SELECT_1F_PRESETS:
        for n in (M.USER, M.BP1, M.BP2):
            v = M.select_1f(cur, n)
            rows.append({'before': '0x%02X' % cur, 'part': n,
                         'cmd6': None if v is None else '0x%02X' % v,
                         'after': '0x%02X' % (cur if v is None else v)})
    return {'index': 179, 'note': 'S-03.012: only bits 2-0 change; no CMD6 when they already equal the '
                                  'partition (cmd6 null)', 'rows': rows}


def main():
    images_dir = sys.argv[sys.argv.index('--images') + 1] if '--images' in sys.argv else None
    os.makedirs(VEC, exist_ok=True)
    for name, obj in (('headers', headers()), ('load_table', load_table(images_dir)),
                      ('write_sequences', write_sequences()), ('user_area', user_area()),
                      ('select_1f', select_1f())):
        with open(os.path.join(VEC, name + '.json'), 'w') as f:
            json.dump(obj, f, indent=1)
            f.write('\n')
        print('wrote vectors/%s.json' % name)


if __name__ == '__main__':
    main()
