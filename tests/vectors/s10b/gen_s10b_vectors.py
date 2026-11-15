#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate test_s10b_vectors.h from tests/oracle/S-10b-mtp/vectors/.

Run from the repository root:  python3 -I -B tests/vectors/s10b/gen_s10b_vectors.py --out DIR

The expectations are the JSON files' (level R). The volumes they were made over are rebuilt here with
tests/oracle/S-10b-mtp/gen_vectors.py (model.Volume.build), so the C test's in-memory volume holds the same
paths, sizes, contents and times. Handles are never compared by value: every handle-dependent field is
written as a reference to the path it names, and the test substitutes the handle its engine gave that path.

    scripts   a fresh engine over one volume, optionally with a session open, then steps
    steps     one operation: parameters, a host data phase, and the expected response and data
"""
import json
import os
import struct
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import genout  # noqa: E402  (before anything the generator reads)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
SPEC = os.path.join(ROOT, 'tests', 'oracle', 'S-10b-mtp')
OUT = 'test_s10b_vectors.h'
sys.path.insert(0, SPEC)
import gen_vectors as G  # noqa: E402
import model as M  # noqa: E402

S, A = M.STORAGE_ID, M.ALL
SIZES = [0, 1, 52, 116, 499, 500, 1012, 1024, 16372, 16384, 3 * 16384 + 77, 1 << 20]


def load(name):
    with open(os.path.join(SPEC, 'vectors', name + '.json')) as f:
        return json.load(f)


def c_str(b):
    """A C string literal of bytes; octal escapes cannot run into the next character."""
    out = []
    for c in b:
        if c in (0x22, 0x5C) or c < 0x20 or c > 0x7E or c == 0x3F:
            out.append('\\%03o' % c)
        else:
            out.append(chr(c))
    return '"' + ''.join(out) + '"'


class Blob:
    def __init__(self):
        self.data = bytearray()

    def ref(self, b):
        b = bytes(b)
        off = len(self.data)
        self.data += b
        return '{ %d, %d }' % (off, len(b))


blob = Blob()
paths = []          # every path the vectors name, as bytes
path_index = {}


def pref(p):
    if isinstance(p, str):
        p = p.encode('utf-8')
    if p not in path_index:
        path_index[p] = len(paths)
        paths.append(p)
    return path_index[p]


# ---------------------------------------------------------------- volumes
volumes = []        # (name, present, total, free, entries, big)
entries = []        # C rows
vol_index = {}


def seed_of(data):
    for seed in range(256):
        if data == G.content(len(data), seed):
            return seed
    return -1


def add_volume(name, vol, big=0):
    first = len(entries)
    if vol is None or vol.entries is None:
        volumes.append((name, 0, 0, 0, first, 0, 0))
    else:
        rows = sorted(vol.entries.items())
        if big:
            # The 5000 files of `big/` are made by the test, not listed here.
            files = [p for p, e in rows if p.startswith('/big/')]
            assert len(files) == big
            assert all(vol.entries[p]['type'] == 'file' and not vol.entries[p]['data']
                       and vol.entries[p]['mtime'] == 0 and vol.entries[p]['ctime'] == 0 for p in files)
            rows = [(p, e) for p, e in rows if not p.startswith('/big/')]
        for p, e in rows:
            data = e['data']
            if data is None:
                entries.append('{ %d, 1, 0, -1, { 0, 0 }, %du, %du }' % (pref(p), e['mtime'], e['ctime']))
            else:
                seed = seed_of(data)
                ref = blob.ref(data) if seed < 0 else '{ 0, 0 }'
                entries.append('{ %d, 0, %du, %d, %s, %du, %du }' % (pref(p), len(data), seed, ref, e['mtime'],
                                                                    e['ctime']))
        volumes.append((name, 1, vol.total, vol.free, first, len(entries) - first, big))
    vol_index[name] = len(volumes) - 1
    return vol_index[name]


def volume(name, make, big=0):
    return vol_index[name] if name in vol_index else add_volume(name, make(), big)


def tree(name):
    return volume(name, lambda: M.Volume.build(G.TREES[name]()))


def big_volume():
    return volume('big', lambda: M.Volume.build({'big': None, **{'big/f%04d' % i: b'' for i in range(5000)}},
                                                n_blocks=64), big=5000)


# ---------------------------------------------------------------- scripts and steps
scripts = []        # C rows
steps = []
sets = []           # path indices of handle sets
patches = []        # (offset, path index)
elems = []          # expected ObjectPropList elements: v_bytes


class Script:
    def __init__(self, what, vol, open_session=True, battery=57, clock=0):
        self.first = len(steps)
        scripts.append(None)
        self.slot = len(scripts) - 1
        self.row = [what, vol, int(open_session), battery, clock]

    def done(self):
        what, vol, op, bat, clk = self.row
        scripts[self.slot] = '{ %s, %d, %d, %du, 0x%08Xu, %d, %d }' % (c_str(what.encode()), vol, op, bat, clk,
                                                                       self.first, len(steps) - self.first)


def param(v):
    """A literal, or ('h', path) for the handle of a path."""
    if isinstance(v, tuple):
        return '{ 1, %du }' % pref(v[1])
    return '{ 0, 0x%08Xu }' % v


def v_bytes(data, patch=()):
    first = len(patches)
    for off, p in patch:
        patches.append('{ %d, %d }' % (off, pref(p)))
    return '{ %s, %d, %d }' % (blob.ref(data), first, len(patches) - first)


NONE, BYTES, HANDLES, PROPLIST, DIGEST, OBJECTS, ANY = range(7)


def step(op, params=(), out=None, code=M.OK, reply=(), expect=NONE, data=b'', patch=(), handle_set=(),
         elements=(), digest=None, length=0, clock=None):
    params = list(params) + [0] * (5 - len(params))
    reply = list(reply)
    first = len(sets) if expect in (HANDLES, OBJECTS) else len(elems)
    if expect in (HANDLES, OBJECTS):
        for p in sorted(handle_set):
            sets.append(str(pref(p)))
        n = len(handle_set)
    elif expect == PROPLIST:
        for e in elements:
            elems.append(e)
        n = len(elements)
    else:
        n = 0
        first = 0
    dig = '{ %s }' % ', '.join('0x%02X' % b for b in (digest or bytes(32)))
    steps.append('{ 0x%04X, { %s }, %d, %s, 0x%04X, %d, { %s }, %d, %s, %d, %d, %s, %du, %d, 0x%08Xu }' % (
        op, ', '.join(param(p) for p in params), int(out is not None), blob.ref(out or b''), code, len(reply),
        ', '.join(param(p) for p in reply + [0] * (5 - len(reply))), expect,
        v_bytes(data, patch), first, n, dig, length, int(clock is not None), clock or 0))


def unhex(s):
    return bytes.fromhex(s) if s else b''


def handle_paths(handles):
    """The vector's handle map, handle -> path."""
    return {h: p for p, h in handles.items()}


PROP_TYPES = dict(M.PROPS)
PROP_TYPES[M.DISPLAY_NAME] = M.DT_STR


def value_len(dt, data, at):
    if dt == M.DT_STR:
        n = data[at]
        return 1 + 2 * n if n else 1
    return {M.DT_U16: 2, M.DT_U32: 4, M.DT_U64: 8, M.DT_U128: 16}[dt]


def value_patches(code, value, by_handle, base):
    """Handle-dependent words of a property value at byte `base`."""
    if code == 0xDC0B:
        h = struct.unpack_from('<I', value)[0]
        return [(base, by_handle[h])] if h else []
    if code == 0xDC41:
        return [(base, by_handle[struct.unpack_from('<I', value)[0]])]
    return []


def prop_list(data, by_handle):
    """An ObjectPropList as its elements, each with its handle-dependent words."""
    n = struct.unpack_from('<I', data)[0]
    at, out = 4, []
    for _ in range(n):
        h, code, dt = struct.unpack_from('<IHH', data, at)
        vl = value_len(dt, data, at + 8)
        el = data[at:at + 8 + vl]
        patch = [(0, by_handle[h])] + value_patches(code, el[8:], by_handle, 8)
        out.append(v_bytes(el, patch))
        at += 8 + vl
    assert at == len(data)
    return out


# ---------------------------------------------------------------- the vectors
def device_info():
    v = load('device_info')
    s = Script('device_info', volume('empty', lambda: M.Volume({})), open_session=False)
    step(0x1001, expect=BYTES, data=unhex(v['R']['data']))
    s.done()
    return v['strings']


def storage_info():
    v = load('storage_info')
    for c in v['cases']:
        if c['level'] != 'R':
            continue
        if c['total'] is None:
            vol = volume('none', lambda: None)
        else:
            vol = volume('empty_free_%d' % c['free'], lambda c=c: M.Volume({}, free=c['free']))
        s = Script('storage_info free %s' % c['free'], vol)
        step(0x1005, [S], expect=BYTES, data=unhex(c['data']))
        if c['total'] is None:
            step(0x1007, [S, 0, A], expect=BYTES, data=unhex(c['root_handles']))
        s.done()
    s = Script('storage_info invalid', volume('empty', lambda: M.Volume({})))
    for c in v['invalid']:
        step(0x1005, [c['storage']], code=c['code'])
    s.done()


def listings():
    v = load('listings')
    for name in G.TREES:
        t = v[name]
        s = Script('listing ' + name, tree(name))
        step(0x1007, [S, 0, 0], expect=HANDLES, handle_set=t['every'])
        assert sorted(t['every']) == sorted(t['listed'])
        for c in t['handles']:
            parent = A if c['parent'] == '/' else ('h', c['parent'])
            step(0x1007, [S, c['format'], parent], expect=HANDLES, handle_set=c['paths'])
            step(0x1006, [S, c['format'], parent], reply=[c['count']])
        for c in t['failures']:
            parent = c['parent'] if isinstance(c['parent'], int) else ('h', c['parent'])
            step(0x1007, [S, 0, parent], code=c['code'])
            step(0x1006, [S, 0, parent], code=c['num_code'])
        for c in t['storage_forms']:
            if c['code'] == M.OK:
                step(0x1007, [c['storage'], 0, A], expect=ANY)
            else:
                step(0x1007, [c['storage'], 0, A], code=c['code'])
        for p, refs in sorted(t['references'].items()):
            step(0x9810, [('h', p)], expect=HANDLES, handle_set=refs)
        s.done()
    b = v['big_folder']
    s = Script('listing big_folder', big_volume())
    step(0x1007, [S, 0, ('h', '/big')], expect=ANY, length=b['container_length'] - 12)
    step(0x1006, [S, 0, ('h', '/big')], reply=[b['num']])
    s.done()


def formats():
    v = load('formats')
    return [(c['path'], c['format']) for c in v['cases']]


def object_info():
    v = load('object_info')
    by_handle = handle_paths(v['handles'])
    s = Script('object_info', volume('objinfo', lambda: M.Volume.build(G.OBJINFO_TREE)))
    for c in v['cases']:
        data = unhex(c['data'])
        parent = struct.unpack_from('<I', data, 38)[0]
        step(0x1008, [('h', c['path'])], expect=BYTES, data=data,
             patch=[(38, by_handle[parent])] if parent else [])
    step(0x1008, [0x7FFF0000], code=v['invalid'])
    s.done()
    return [(unhex(c['name']), unhex(c['presented'])) for c in v['non_utf8_names']]


def partial():
    v = load('partial')
    vol = volume('partial', lambda: M.Volume.build({'big.bin': G.content(3 * 16384 + 77),
                                                    'other.bin': G.content(100, 9), 'empty.bin': b''}))
    s = Script('partial', vol)
    for c in v['cases']:
        if c['code'] != M.OK:
            step(0x101B, [('h', c['path']), c['offset'], c['max']], code=c['code'])
        elif 'data' in c:
            step(0x101B, [('h', c['path']), c['offset'], c['max']], reply=c['params'], expect=BYTES,
                 data=unhex(c['data']))
        else:
            step(0x101B, [('h', c['path']), c['offset'], c['max']], reply=c['params'], expect=DIGEST,
                 digest=unhex(c['data_sha256']), length=c['data_len'])
        step(0x101B, [('h', '/other.bin'), 5, 10], reply=[10], expect=BYTES, data=G.content(100, 9)[5:15])
    s.done()


def device_props():
    v = load('device_props')
    empty = volume('empty', lambda: M.Volume({}))
    for c in v['battery']:
        s = Script('battery %d' % c['charge'], empty, battery=c['charge'])
        step(0x1014, [0x5001], expect=BYTES, data=unhex(c['desc']))
        step(0x1015, [0x5001], expect=BYTES, data=unhex(c['value']))
        s.done()
    s = Script('datetime desc', empty)
    step(0x1014, [0x5011], expect=BYTES, data=unhex(v['datetime_desc']))
    step(0x1014, [0xD4FF], code=v['unknown_desc'])
    step(0x1015, [0xD4FF], code=v['unknown_value'])
    s.done()
    for c in v['clock']:
        s = Script('clock ' + c['text'], empty, clock=c['count'])
        step(0x1015, [0x5011], expect=BYTES, data=unhex(c['value']))
        s.done()


def datetime_sets():
    v = load('datetime')
    empty = volume('empty', lambda: M.Volume({}))
    for c in v['cases']:
        s = Script('datetime ' + repr(c['value']), empty, clock=0x11)
        if c['code'] == M.OK:
            step(0x1016, [0x5011], out=M.mtp_string(c['value']), clock=c['count'])
            step(0x1015, [0x5011], expect=BYTES, data=M.mtp_string(c['reads_back']))
        else:
            step(0x1016, [0x5011], out=M.mtp_string(c['value']), code=c['code'], clock=0x11)
        s.done()
    s = Script('datetime others', empty)
    step(0x1016, [0x5001], out=b'\x32', code=v['battery_set'])
    step(0x1016, [0xD4FF], out=b'\x00', code=v['unknown_set'])
    s.done()


def object_props():
    v = load('object_props')
    by_handle = handle_paths(v['handles'])
    s = Script('object_props', tree('tree_basic'))
    for fmt, data in sorted(v['supported'].items()):
        step(0x9801, [int(fmt, 16)], expect=BYTES, data=unhex(data))
    for c in v['desc']:
        if c['level'] != 'R':
            continue
        if c['code'] == M.OK:
            step(0x9802, [c['prop'], c['format']], expect=BYTES, data=unhex(c['data']))
        else:
            step(0x9802, [c['prop'], c['format']], code=c['code'])
    for c in v['value']:
        h = ('h', c['path']) if c['path'] else 0x7FFF0000
        if c['code'] == M.OK:
            data = unhex(c['data'])
            step(0x9803, [h, c['prop']], expect=BYTES, data=data,
                 patch=value_patches(c['prop'], data, by_handle, 0))
        else:
            step(0x9803, [h, c['prop']], code=c['code'])
    for c in v['list']:
        params = [('h', c['handle']), c['format'], c['prop'], 0, c['depth']]
        if c['code'] == M.OK:
            step(0x9805, params, expect=PROPLIST, elements=prop_list(unhex(c['data']), by_handle))
        else:
            step(0x9805, params, code=c['code'])
    for c in v['select']:
        h = ('h', c['handle']) if isinstance(c['handle'], str) else c['handle']
        params = [h, c['format'], 0xDC0B, 0, c['depth']]
        if c['code'] == M.OK:
            step(0x9805, params, expect=OBJECTS, handle_set=c['objects'], length=c['data_len'])
        else:
            step(0x9805, params, code=c['code'])
    s.done()
    long_tree = {'n' * 40: b'', 'w' * 254: b''}
    s = Script('object_props long names', volume('long_names', lambda: M.Volume.build(long_tree)))
    by_long = handle_paths(v['long_names_handles'])
    for c in v['long_names']:
        step(0x9805, [('h', c['path']), 0, 0xDC07, 0, 0], expect=PROPLIST,
             elements=prop_list(unhex(c['data']), by_long))
    s.done()
    b = v['big_list']
    s = Script('object_props big_list', big_volume())
    step(0x9805, [('h', '/big'), 0, A, 0, 1], expect=ANY, length=b['container_length'] - 12)
    s.done()
    return b['elements']


def op_sweep():
    v = load('op_sweep')
    rows = []
    for code, in_session, listed, response, host_data, device_data in v['R']:
        rows.append('{ 0x%04X, %d, 0x%04X, %d, %d }' % (code, in_session, response, host_data, device_data))
    return rows


def transcripts():
    v = load('transcripts')
    basic = tree('tree_basic')
    s = Script('session_open_close', basic, open_session=False)
    for c in v['session_open_close']:
        step(c['op'], c['args'], code=c['code'], reply=c['params'],
             expect=BYTES if 'data' in c else NONE, data=unhex(c.get('data')))
    s.done()
    s = Script('session_closed_ops', basic, open_session=False)
    for c in v['session_closed_ops']:
        out = b'\x00' if c['op'] in M.DATA_OUT else None
        step(c['op'], [S, 0, A], out=out, code=c['code'], reply=c['params'])
    s.done()
    s = Script('storage_ids', basic)
    for c in v['storage_ids']:
        step(c['op'], expect=BYTES, data=unhex(c['data']))
    s.done()
    tree_sizes = {'s%07d.bin' % n: G.content(n, n & 0xFF) for n in SIZES}
    tree_sizes['folder'] = None
    s = Script('get_object_sizes', volume('sizes', lambda: M.Volume.build(tree_sizes, n_blocks=64)))
    for c in v['get_object_sizes']:
        if c['path'] is None:
            step(0x1009, [0x7FFF0000], code=c['code'])
        elif c['code'] != M.OK:
            step(0x1009, [('h', c['path'])], code=c['code'])
        else:
            step(0x1009, [('h', c['path'])], expect=DIGEST, digest=unhex(c['sha256']),
                 length=c['container_length'] - 12)
    s.done()
    s = Script('devprop_set', basic)
    for c in v['devprop_set_battery'] + v['devprop_set_unknown']:
        step(c['op'], c['args'], out=b'\x10' if c['args'][0] == 0x5001 else b'\x00', code=c['code'])
    s.done()
    return [(c['path'], c['size'], c['zlp_at_512'], c['zlp_at_64']) for c in v['get_object_sizes']
            if c['path'] and c['code'] == M.OK]


def main():
    strings = device_info()
    storage_info()
    listings()
    fmts = formats()
    non_utf8 = object_info()
    partial()
    device_props()
    datetime_sets()
    big_elements = object_props()
    sweep = op_sweep()
    sizes = transcripts()

    o = ['/* SPDX-License-Identifier: GPL-2.0-or-later */',
         '/* Generated by tests/vectors/s10b/gen_s10b_vectors.py from tests/oracle/S-10b-mtp/vectors/; do not edit. */',
         '#ifndef TEST_S10B_VECTORS_H', '#define TEST_S10B_VECTORS_H', '',
         '#include <stdint.h>', '',
         'struct v_ref { uint32_t off, len; };',
         'struct v_patch { uint32_t off; uint16_t path; };',
         'struct v_bytes { struct v_ref b; uint16_t patch, n_patch; };',
         '/* kind 0 file, 1 folder; seed -1: the bytes are data, else byte i = (7 i + 3 + seed) mod 256 */',
         'struct v_entry { uint16_t path; uint8_t kind; uint32_t size; int16_t seed; struct v_ref data;',
         '                 uint32_t mtime, ctime; };',
         '/* big: that many empty files big/f0000... are added, mtime and ctime 0 */',
         'struct v_volume { uint8_t present; uint64_t total, free; uint16_t first, n; uint16_t big; };',
         'struct v_param { uint8_t handle; uint32_t v; };	/* handle: v is a path index */',
         'enum v_expect { X_NONE, X_BYTES, X_HANDLES, X_PROPLIST, X_DIGEST, X_OBJECTS, X_ANY };',
         'struct v_step {',
         '	uint16_t op; struct v_param p[5]; uint8_t has_out; struct v_ref out;',
         '	uint16_t code; uint8_t n_reply; struct v_param reply[5];',
         '	uint8_t expect; struct v_bytes data;',
         '	uint16_t first, n;	/* X_HANDLES, X_OBJECTS: v_sets; X_PROPLIST: v_elems */',
         '	uint8_t digest[32]; uint32_t length;	/* X_DIGEST; X_ANY, X_OBJECTS when not 0 */',
         '	uint8_t has_clock; uint32_t clock;	/* the clock after the step */',
         '};',
         'struct v_script { const char *what; uint8_t volume, open; unsigned battery; uint32_t clock;',
         '                  uint16_t first, n; };',
         'struct v_sweep { uint16_t op; uint8_t in_session; uint16_t code; uint8_t host_data, device_data; };',
         'struct v_format { const char *path; uint16_t format; };',
         'struct v_name { struct v_ref name, presented; };',
         'struct v_size { const char *path; uint32_t size; uint8_t zlp_512, zlp_64; };', '']
    o.append('static const char *const v_strings[4] = { %s };' % ', '.join(
        c_str(strings[k].encode()) for k in ('manufacturer', 'model', 'version', 'serial')))
    o.append('#define V_BIG_ELEMENTS %du' % big_elements)
    o.append('')
    o.append('static const char *const v_paths[%d] = {' % len(paths))
    o += ['\t%s,' % c_str(p) for p in paths]
    o.append('};')
    o.append('static const struct v_entry v_entries[%d] = {' % len(entries))
    o += ['\t%s,' % e for e in entries]
    o.append('};')
    for name, i in sorted(vol_index.items(), key=lambda kv: kv[1]):
        o.append('#define V_VOL_%s %d' % (name.upper(), i))
    o.append('static const struct v_volume v_volumes[%d] = {' % len(volumes))
    for name, present, total, free, first, n, big in volumes:
        o.append('\t{ %d, %du, %du, %d, %d, %d },	/* %s */' % (present, total, free, first, n, big, name))
    o.append('};')
    o.append('static const struct v_patch v_patches[%d] = {' % max(len(patches), 1))
    o += ['\t%s,' % p for p in patches] or ['\t{ 0, 0 },']
    o.append('};')
    o.append('static const uint16_t v_sets[%d] = {' % max(len(sets), 1))
    for i in range(0, len(sets), 16):
        o.append('\t' + ', '.join(sets[i:i + 16]) + ',')
    o.append('};')
    o.append('static const struct v_bytes v_elems[%d] = {' % max(len(elems), 1))
    o += ['\t%s,' % e for e in elems]
    o.append('};')
    o.append('static const struct v_step v_steps[%d] = {' % len(steps))
    o += ['\t%s,' % st for st in steps]
    o.append('};')
    o.append('static const struct v_script v_scripts[%d] = {' % len(scripts))
    o += ['\t%s,' % sc for sc in scripts]
    o.append('};')
    o.append('static const struct v_sweep v_sweep[%d] = {' % len(sweep))
    o += ['\t%s,' % r for r in sweep]
    o.append('};')
    o.append('static const struct v_format v_formats[%d] = {' % len(fmts))
    o += ['\t{ %s, 0x%04X },' % (c_str(p.encode()), f) for p, f in fmts]
    o.append('};')
    o.append('static const struct v_name v_names[%d] = {' % len(non_utf8))
    o += ['\t{ %s, %s },' % (blob.ref(n), blob.ref(p)) for n, p in non_utf8]
    o.append('};')
    o.append('static const struct v_size v_sizes[%d] = {' % len(sizes))
    o += ['\t{ %s, %du, %d, %d },' % (c_str(p.encode()), n, z5, z6) for p, n, z5, z6 in sizes]
    o.append('};')
    o.append('static const uint8_t v_blob[%d] = {' % max(len(blob.data), 1))
    for i in range(0, len(blob.data), 16):
        o.append('\t' + ', '.join('0x%02X' % b for b in blob.data[i:i + 16]) + ',')
    o.append('};')
    o += ['', '#endif', '']
    genout.write(OUT, '\n'.join(o))
    genout.finish()


if __name__ == '__main__':
    main()
