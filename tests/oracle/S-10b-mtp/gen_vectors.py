#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-10b vectors: writes vectors/*.json from model.py over synthetic yaffs2 volumes.

    python3 -I tests/oracle/S-10b-mtp/gen_vectors.py          # writes tests/oracle/S-10b-mtp/vectors/

Objects are named by path; a vector with handle-dependent bytes also holds its handle map (`handles`), for
re-binding through a device's listing. Synthetic content only.
"""
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import model as M  # noqa: E402

S = M.STORAGE_ID
A = M.ALL


def content(n, seed=0):
    return bytes((i * 7 + 3 + seed) & 0xFF for i in range(n))


def digest(b):
    return hashlib.sha256(b).hexdigest()


def session(vol, level='R', **kw):
    r = M.Responder(vol, level, **kw)
    assert r.op(0x1002, [1]).code == M.OK
    return r


def reply_json(rep, small=4096):
    out = {'code': rep.code, 'params': rep.params}
    if rep.data is not None:
        out['data_len'] = len(rep.data)
        if len(rep.data) <= small:
            out['data'] = rep.data.hex()
        else:
            out['data_sha256'] = digest(rep.data)
    return out


# ---------------------------------------------------------------- volumes
def tree_basic():
    return {'a': None, 'a/b': None, 'a/b/deep.txt': content(10), 'a/kick.wav': content(1012),
            'a/empty': None, 'zero.bin': b'', 'song.aiff': content(500), 'notes.txt': content(1024)}


def tree_deep():
    t, path = {}, ''
    for k in range(1, 12):
        t[path + 'f%d' % k] = content(k, k)
        path += 'd%d' % k
        t[path] = None
        path += '/'
    return t


def long_names():
    a, b1, b2 = 'a' * 250, 'b' * 249, 'c' * 250          # '/' + a + '/' + b1 = 501 bytes; with c: 502
    return a, b1, b2


def tree_long():
    a, b1, b2 = long_names()
    return {a: None, a + '/' + b1: None, a + '/' + b1 + '/f': content(3), a + '/' + b2: None,
            a + '/' + b2 + '/f': content(4), a + '/sibling.txt': content(5), 'top.txt': content(6)}


NAMES = ['star*', 'what?', '[x]', '.hidden', 'with space', 'Kick', 'kick', 'x', 'n' * 255]


def tree_names():
    t = {'dir': None}
    for n in NAMES:
        t['dir/' + n] = content(len(n) % 7)
    return t


TREES = {'tree_basic': tree_basic, 'tree_deep': tree_deep, 'tree_long': tree_long, 'tree_names': tree_names}


def vol(name):
    return M.Volume.build(TREES[name]())


# ---------------------------------------------------------------- vectors
def v_device_info():
    out = {'strings': dict(manufacturer='Manufacturer', model='Model', version='1.0.0', serial='SERIAL0001')}
    for lv in ('R', 'W'):
        r = M.Responder(M.Volume({}), lv)
        out[lv] = {'data': r.device_info().hex(), 'operations': (M.OPS_W if lv == 'W' else M.OPS_R),
                   'events': [], 'device_props': M.DEVICE_PROPS, 'capture': M.CAPTURE_FORMATS,
                   'playback': M.PLAYBACK_FORMATS}
    return out


def v_storage_info():
    cases = []
    for lv in ('R', 'W'):
        for free in (0, 1023, 1024, 123456789, 2403870720):
            r = session(M.Volume({}, free=free), lv)
            cases.append(dict(level=lv, total=2403870720, free=free, data=r.storage_info().hex(),
                              max_capacity=2403870720, free_bytes=free // 1024 * 1024))
        r = session(M.Volume(None), lv)
        cases.append(dict(level=lv, total=None, free=None, data=r.storage_info().hex(), max_capacity=0,
                          free_bytes=0, root_handles=r.op(0x1007, [S, 0, A]).data.hex()))
    bad = session(M.Volume({}))
    return {'cases': cases, 'invalid': [dict(storage=x, code=bad.op(0x1005, [x]).code)
                                        for x in (0, A, 0x00010002, 0x00020001)]}


def listing(name):
    r = session(vol(name))
    folders = ['/'] + [p for p in r.by_path if r.v.entries[p]['type'] == 'dir']
    handles = []
    for f in folders:
        parent = A if f == '/' else r.handle(f)
        for fmt in (0, 0x3001, 0x3008, 0x3004):
            rep = r.op(0x1007, [S, fmt, parent])
            num = r.op(0x1006, [S, fmt, parent])
            paths = sorted(r.handles[h] for h in M.struct.unpack('<%dI' % (len(rep.data) // 4 - 1), rep.data[4:]))
            handles.append(dict(parent=f, format=fmt, paths=paths, count=num.params[0]))
    rep = r.op(0x1007, [S, 0, 0])
    every = sorted(r.handles[h] for h in M.struct.unpack('<%dI' % (len(rep.data) // 4 - 1), rep.data[4:]))
    refs = {p: sorted(r.handles[h] for h in M.struct.unpack('<%dI' % (len(rr.data) // 4 - 1), rr.data[4:]))
            for p in r.by_path for rr in [r.op(0x9810, [r.handle(p)])]}
    files = [p for p in r.by_path if r.v.entries[p]['type'] == 'file']
    reads = {p: digest(r.op(0x1009, [r.handle(p)]).data) == digest(r.v.entries[p]['data']) for p in files}
    fails = []
    some_file = files[0]
    for parent, want in ((r.handle(some_file), M.INVALID_PARENT), (0x7FFF0000, M.INVALID_HANDLE)):
        fails.append(dict(parent=parent if parent == 0x7FFF0000 else some_file, code=r.op(0x1007, [S, 0, parent]).code,
                          num_code=r.op(0x1006, [S, 0, parent]).code, want=want))
    return dict(volume={k: (None if v is None else len(v)) for k, v in TREES[name]().items()},
                listed=sorted(r.by_path), every=every, handles=handles, references=refs, reads_back=reads,
                failures=fails, storage_forms=[dict(storage=x, code=r.op(0x1007, [x, 0, A]).code)
                                               for x in (S, A, 0x00010002)])


def v_listings():
    out = {n: listing(n) for n in TREES}
    big = M.Volume.build({'big': None, **{'big/f%04d' % i: b'' for i in range(5000)}}, n_blocks=64)
    r = session(big)
    rep = r.op(0x1007, [S, 0, r.handle('/big')])
    out['big_folder'] = dict(files=5000, container_length=12 + len(rep.data), count=len(rep.data) // 4 - 1,
                             num=r.op(0x1006, [S, 0, r.handle('/big')]).params[0])
    return out


FORMAT_CASES = ['a.jpg', 'a.JPEG', 'a.Jpeg', 'b.tif', 'b.TIFF', 'c.bmp', 'd.png', 'e.gif', 'e.GIF', 'f.exe',
                'g.txt', 'h.htm', 'h.html', 'kick.wav', 'kick.WAV', 'kick.wav.bak', 'i.aiff', 'j.mp3', 'k.avi',
                'l.mpeg', 'm.asf', 'x.aif', 'x.mpg', 'x.jpe', 'noext', 'a.png.txt', 'a.txt.png', 'a.txt/b',
                'a.wav/b.mp3', '.wav', 'wav', 'a.WaV']


def v_formats():
    return {'cases': [dict(path='/' + p, format=M.format_of('/' + p)) for p in FORMAT_CASES],
            'folder': 0x3001}


OBJINFO_TREE = {'é.txt': {'data': content(3), 'times': (0, 1000, 0)},
                'euro€ 🎵.wav': {'data': content(17), 'times': (0, 951782400, 951782401)},
                'p' * 254: {'data': b'', 'times': (0, 978307199, 978307199)},
                'q' * 255: {'data': b'x', 'times': (0, 2147483647, 1)},
                'é' * 127 + 'z': {'data': b'', 'times': (0, 4102444800, 0)},
                'folder': {'data': None, 'times': (0, 1, 0)},
                'folder/inner.gif': {'data': content(40), 'times': (0, 0, 0)}}


def v_object_info():
    r = session(M.Volume.build(OBJINFO_TREE))
    cases = []
    for p in sorted(r.by_path):
        e = r.v.entries[p]
        cases.append(dict(path=p, data=r.op(0x1008, [r.handle(p)]).data.hex(),
                          name=M.presented_name(p.rsplit('/', 1)[1].encode('utf-8')),
                          created=M.datetime_text(e['ctime'] or e['mtime']), modified=M.datetime_text(e['mtime'])))
    return {'handles': r.by_path, 'cases': cases, 'invalid': r.op(0x1008, [0x7FFF0000]).code,
            'non_utf8_names': [dict(name=b.hex(), presented=M.mtp_string(M.presented_name(b)).hex())
                               for b in NON_UTF8_NAMES]}


# S-10b.047: names that are not well-formed UTF-8, as raw name bytes
NON_UTF8_NAMES = [b'a\x80b', b'\xe2\x82', b'\xe2\x82x', b'\xc0\x80', b'\xed\xa0\x80', b'\xf5', b'\xff',
                  b'ok\xe2\x82\xac\xf0\x9f\x8e\xb5\xfe', b'\xff' * 255, b'a\xff', b'a\xfe']


def v_partial():
    n = 3 * 16384 + 77
    v = M.Volume.build({'big.bin': content(n), 'other.bin': content(100, 9), 'empty.bin': b''})
    r = session(v)
    cases = []
    for path, size in (('/big.bin', n), ('/empty.bin', 0)):
        for off in sorted({0, 1, 0x3FFF, 0x4000, 0x8001, max(size - 1, 0), size, size + 1}):
            for mx in (0, 1, 2, 0x4000, A):
                rep = r.op(0x101B, [r.handle(path), off, mx])
                c = dict(path=path, offset=off, max=mx, **reply_json(rep, small=64))
                cases.append(c)
                other = r.op(0x101B, [r.handle('/other.bin'), 5, 10])            # interleaved
                assert other.data == content(100, 9)[5:15]
    return {'files': {'/big.bin': dict(size=n, sha256=digest(content(n))), '/empty.bin': dict(size=0)},
            'cases': cases}


RTC_TIMES = ['19700101T000000', '20001229T235959', '20010101T000000', '20261027T120000', '20590920T235959']


def v_device_props():
    out = {'battery': [], 'datetime_desc': None, 'unknown_desc': None, 'clock': []}
    for ch in (0, 57, 100):
        r = session(M.Volume({}), battery=ch)
        out['battery'].append(dict(charge=ch, desc=r.op(0x1014, [0x5001]).data.hex(),
                                   value=r.op(0x1015, [0x5001]).data.hex()))
    r = session(M.Volume({}))
    out['datetime_desc'] = r.op(0x1014, [0x5011]).data.hex()
    out['unknown_desc'] = r.op(0x1014, [0xD4FF]).code
    out['unknown_value'] = r.op(0x1015, [0xD4FF]).code
    for t in RTC_TIMES:
        count = M.rtc_count(M.parse_datetime(t))
        r.clock = count
        out['clock'].append(dict(count=count, value=r.op(0x1015, [0x5011]).data.hex(), text=t))
    return out


DT_CASES = ['20261027T120000', '20261027T120000.5', '20261027T120000Z', '20261027T120000.0Z',
            '20261027T120000+0100', '20261027T120000-0830', '20261027T120000.5+0100', '20261327T120000',
            '20260027T120000', '20261000T120000', '20230229T000000', '20240229T000000', '20240230T000000',
            '20261027T240000', '20261027T126000', '20261027T120060', '2026102AT120000', '2026102T120000',
            '202610277T120000', '20261027t120000', '19691231T235959', '19700101T000000', '20590920T235959',
            '20590921T000000', '20001230T000000', '20001231T235959', '', '20261027T120000.', '20261027T120000Z0']


def v_datetime():
    cases = []
    for t in DT_CASES:
        r = session(M.Volume({}), clock=0x11)
        rep = r.op(0x1016, [0x5011], M.mtp_string(t))
        c = dict(value=t, code=rep.code)
        if rep.code == M.OK:
            c['count'] = r.clock
            c['reads_back'] = M.decode_string(r.op(0x1015, [0x5011]).data)
        else:
            c['clock_unchanged'] = r.clock == 0x11
        cases.append(c)
    stuck = []                                          # S-10b.062: a clock register that takes no write
    for t in ('20261027T120000', '20261327T120000'):
        r = session(M.Volume({}), clock=0x11, clock_writable=False)
        rep = r.op(0x1016, [0x5011], M.mtp_string(t))
        stuck.append(dict(value=t, code=rep.code, count_after=r.clock,
                          reads_back=M.decode_string(r.op(0x1015, [0x5011]).data)))
    other = session(M.Volume({}))
    return {'cases': cases, 'clock_unwritable': stuck,
            'battery_set': other.op(0x1016, [0x5001], b'\x32').code,
            'unknown_set': other.op(0x1016, [0xD4FF], b'\x00').code}


def v_object_props():
    out = {}
    r = session(vol('tree_basic'))
    out['handles'] = r.by_path
    out['supported'] = {hex(f): r.op(0x9801, [f]).data.hex() for f in (0x3000, 0x3001, 0x3008, 0xB900)}
    desc = []
    for lv in ('R', 'W'):
        rl = session(vol('tree_basic'), lv)
        for code in list(M.PROPS) + [M.DISPLAY_NAME, 0xDC06]:
            for fmt in (0x3000, 0x3001, 0x3008, 0x3801, 0xB900, 0x0000):
                rep = rl.op(0x9802, [code, fmt])
                desc.append(dict(level=lv, prop=code, format=fmt, code=rep.code,
                                 data=rep.data.hex() if rep.data is not None else None))
    out['desc'] = desc
    vals = []
    for p in ('/a/kick.wav', '/a/b', '/notes.txt', '/a/b/deep.txt'):
        for code in list(M.PROPS) + [M.DISPLAY_NAME, 0xDC06]:
            rep = r.op(0x9803, [r.handle(p), code])
            vals.append(dict(path=p, prop=code, code=rep.code, data=rep.data.hex() if rep.data else None))
    vals.append(dict(path=None, prop=0xDC01, code=r.op(0x9803, [0x7FFF0000, 0xDC01]).code, data=None))
    out['value'] = vals
    lists = []
    for p, depth in (('/a/kick.wav', 0), ('/a', 1), ('/a/kick.wav', 1)):
        for prop in (A, 0, 0xDC07, 0xDC04, M.DISPLAY_NAME, 0xDC06):
            rep = r.op(0x9805, [r.handle(p), 0, prop, 0, depth])
            lists.append(dict(handle=p, depth=depth, prop=prop, format=0, **reply_json(rep)))
    out['list'] = lists
    sel = []
    rows = [(0, 1), (0, 0), (0, A), (A, 0), (A, 1), (A, A), (S, 0), (S, 1), (S, A), ('/a', 1), ('/a', 0),
            ('/a', A), ('/a', 2), (0, 2), (0x00010002, 0), (0x7FFF0000, 0)]
    for h, depth in rows:
        hv = r.handle(h) if isinstance(h, str) else h
        for fmt in (0, 0x3001):
            rep = r.op(0x9805, [hv, fmt, 0xDC0B, 0, depth])
            objs = None
            if rep.code == M.OK:
                n = M.struct.unpack_from('<I', rep.data)[0]
                objs = sorted(r.handles[M.struct.unpack_from('<I', rep.data, 4 + 12 * i)[0]] for i in range(n))
            sel.append(dict(handle=h, depth=depth, format=fmt, code=rep.code, objects=objs,
                            data_len=None if rep.data is None else len(rep.data)))
    out['select'] = sel
    long = session(M.Volume.build({'n' * 40: b'', 'w' * 254: b''}))
    out['long_names'] = [dict(path=p, **reply_json(long.op(0x9805, [long.handle(p), 0, 0xDC07, 0, 0])))
                         for p in sorted(long.by_path) if p != '/lost+found']
    out['long_names_handles'] = long.by_path
    big = session(M.Volume.build({'big': None, **{'big/f%04d' % i: b'' for i in range(5000)}}, n_blocks=64))
    rep = big.op(0x9805, [big.handle('/big'), 0, A, 0, 1])
    out['big_list'] = dict(objects=5000, elements=M.struct.unpack_from('<I', rep.data)[0],
                           container_length=12 + len(rep.data))
    return out


SWEEP = list(range(0x1000, 0x1100)) + list(range(0x9800, 0x9900)) + list(range(0x95C0, 0x95D0)) + \
    [0x0000, 0x2001, 0x7FFF, 0xFFFF]


def v_op_sweep():
    out = {}
    for lv in ('R', 'W'):
        rows = []
        for code in SWEEP:
            for in_session in (False, True):
                r = M.Responder(vol('tree_basic'), lv)
                if in_session:
                    r.op(0x1002, [1])
                supported = code in (M.OPS_W if lv == 'W' else M.OPS_R)
                data = b'\x00' if code in M.DATA_OUT else None
                rep = r.op(code, [], data) if code in M.DATA_OUT else r.op(code, [])
                rows.append([code, int(in_session), int(supported), rep.code, int(code in M.DATA_OUT),
                             int(rep.data is not None)])
        out[lv] = rows
    out['columns'] = ['op', 'in_session', 'listed', 'response', 'host_data_phase', 'device_data_phase']
    return out


def v_transcripts():
    cases = {}
    r = M.Responder(vol('tree_basic'))
    t = [('GetDeviceInfo', 0x1001, []), ('CloseSession', 0x1003, []), ('OpenSession0', 0x1002, [0]),
         ('OpenSession', 0x1002, [7]), ('OpenAgain', 0x1002, [8]), ('CloseSession', 0x1003, []),
         ('CloseAgain', 0x1003, [])]
    cases['session_open_close'] = [dict(step=n, op=c, args=p, **reply_json(r.op(c, p))) for n, c, p in t]
    r = M.Responder(vol('tree_basic'))
    cases['session_closed_ops'] = [dict(op=c, **reply_json(r.op(c, [S, 0, A], b'\x00') if c in M.DATA_OUT
                                                          else r.op(c, [S, 0, A])))
                                   for c in M.OPS_R if c not in M.NO_SESSION_OK]
    r = session(vol('tree_basic'))
    cases['storage_ids'] = [dict(op=0x1004, **reply_json(r.op(0x1004, [])))]
    gets = []
    sizes = [0, 1, 52, 116, 499, 500, 1012, 1024, 16372, 16384, 3 * 16384 + 77, 1 << 20]
    tree = {'s%07d.bin' % n: content(n, n & 0xFF) for n in sizes}
    tree['folder'] = None
    rg = session(M.Volume.build(tree, n_blocks=64))
    for n in sizes:
        rep = rg.op(0x1009, [rg.handle('/s%07d.bin' % n)])
        gets.append(dict(path='/s%07d.bin' % n, size=n, code=rep.code, container_length=12 + len(rep.data),
                         sha256=digest(rep.data), zlp_at_512=(12 + n) % 512 == 0, zlp_at_64=(12 + n) % 64 == 0))
    gets.append(dict(path='/folder', code=rg.op(0x1009, [rg.handle('/folder')]).code))
    gets.append(dict(path=None, code=rg.op(0x1009, [0x7FFF0000]).code))
    cases['get_object_sizes'] = gets
    r = session(vol('tree_basic'))
    cases['devprop_set_battery'] = [dict(op=0x1016, args=[0x5001], **reply_json(r.op(0x1016, [0x5001], b'\x10')))]
    cases['devprop_set_unknown'] = [dict(op=0x1016, args=[0xD4FF], **reply_json(r.op(0x1016, [0xD4FF], b'\x00')))]
    return cases


CANCEL = struct.pack('<HI', 0x4001, 5)
CLASS_CASES = [                                     # name, transaction in progress, bmRequestType, bRequest, wLength, data
    ('cancel_in_progress', True, 0x21, 0x64, 6, CANCEL),
    ('cancel_other_tid', True, 0x21, 0x64, 6, struct.pack('<HI', 0x4001, 99)),
    ('cancel_idle', False, 0x21, 0x64, 6, CANCEL),
    ('cancel_code_4002', True, 0x21, 0x64, 6, struct.pack('<HI', 0x4002, 5)),
    ('cancel_wlength_0', True, 0x21, 0x64, 0, b''),
    ('cancel_wlength_8', True, 0x21, 0x64, 8, CANCEL + b'\x00\x00'),
    ('cancel_in', True, 0xA1, 0x64, 6, b''),
    ('reset_in_progress', True, 0x21, 0x66, 0, b''),
    ('reset_idle', False, 0x21, 0x66, 0, b''),
    ('reset_with_data', True, 0x21, 0x66, 4, b'\x00' * 4),
    ('reset_in', True, 0xA1, 0x66, 0, b''),
    ('status_busy', True, 0xA1, 0x67, 4, b''),
    ('status_idle', False, 0xA1, 0x67, 4, b''),
    ('status_wlength_64', False, 0xA1, 0x67, 64, b''),
    ('status_out', True, 0x21, 0x67, 0, b''),
    ('ext_event_idle', False, 0xA1, 0x65, 8, b''),
    ('ext_event_in_progress', True, 0xA1, 0x65, 8, b''),
    ('ext_event_wlength_64', False, 0xA1, 0x65, 64, b''),
    ('ext_event_out', False, 0x21, 0x65, 8, b'\x00' * 8),
    ('unknown_0x63_out', True, 0x21, 0x63, 0, b''),
    ('unknown_0x63_in', True, 0xA1, 0x63, 4, b''),
]


def class_result(x):
    return x if x == M.STALL else x.hex()


def v_class_requests():
    """S-10b.005, S-10b.009: each class request with a session open, a GetObject in progress or none; then
    Get Device Status, and GetObjectInfo of a handle given before the request."""
    cases = []
    for name, busy, bm, req, wl, data in CLASS_CASES:
        r = session(vol('tree_basic'))
        h = r.handle('/notes.txt')
        if busy:
            r.begin(0x1009, [h])
        res = r.class_request(bm, req, wl, data)
        c = dict(name=name, in_progress=busy, bmRequestType=bm, bRequest=req, wLength=wl, data=data.hex(),
                 result=class_result(res), in_progress_after=r.in_progress is not None,
                 status_after=r.class_request(0xA1, 0x67, 4).hex())
        if name == 'reset_with_data':                   # S-10b.005
            c['also_allowed'] = [dict(result='', in_progress_after=False)]
        r.in_progress = None
        c['session_open_after'] = r.session != 0
        c['old_handle_code'] = r.op(0x1008, [h]).code
        cases.append(c)
    return {'cases': cases, 'stall': M.STALL}


def floor_name(kind, i, n):
    """An ASCII name of exactly n bytes: kind, the index, then '-' (and '.bin' for a file)."""
    tail = '.bin' if kind == 'f' else ''
    head = '%s%05d' % (kind, i)
    return head + '-' * (n - len(head) - len(tail)) + tail


def floor_tree():
    """S-10b.014: a volume of exactly LIST_OBJECTS objects (lost+found counted) with names totaling exactly
    LIST_NAME_BYTES: 63 level-1 folders holding 129 files each, and one level-1 file. Names are 32 bytes,
    the first 22 are 33, lost+found's 10."""
    n_obj = M.LIST_OBJECTS - 1                          # without lost+found
    extra = M.LIST_NAME_BYTES - 10 - 32 * n_obj
    lens = [33 if k < extra else 32 for k in range(n_obj)]
    folders = [floor_name('d', i, lens[i]) for i in range(63)]
    tree = {d: None for d in folders}
    for k in range(n_obj - 63):
        name = floor_name('f', k, lens[63 + k])
        tree[name if k == n_obj - 64 else folders[k // 129] + '/' + name] = b''
    return tree


def v_capacity():
    """S-10b.014: the floor volume is listed in full."""
    tree = floor_tree()
    entries = {'/lost+found': dict(type='dir', data=None, mtime=0, ctime=0)}
    for path, v in tree.items():
        entries['/' + path] = dict(type='dir' if v is None else 'file', data=v, mtime=0, ctime=0)
    r = session(M.Volume(entries))
    paths = sorted(r.by_path)
    return dict(objects=M.LIST_OBJECTS, name_bytes=M.LIST_NAME_BYTES,
                paths_sha256=digest('\n'.join(paths).encode()),
                num_objects=r.op(0x1006, [S, 0, 0]).params, level1=r.op(0x1006, [S, 0, A]).params,
                handles=len(set(struct.unpack_from('<%dI' % len(paths), r.op(0x1007, [S, 0, 0]).data, 4))),
                growth=capacity_growth())


def capacity_growth():
    """S-10b.014: sessions within the bound that create objects (SendObjectInfo, one name each)."""
    rows = []
    for name, start, lens in (('reach_floor', (8191, 262112), [32]),
                              ('past_floor_objects', (8192, 262143), [1]),
                              ('past_floor_bytes', (8100, 262100), [45]),
                              ('reach_then_pass', (8190, 262000), [40, 40, 40])):
        objects, nb, steps = start[0], start[1], []
        for n in lens:
            steps.append(dict(name_len=n, before=[objects, nb], outcome=M.creation_outcome(objects, nb, n)))
            objects, nb = objects + 1, nb + n
        rows.append(dict(name=name, start=list(start), creates=steps))
    return rows


def v_bus_events():
    """S-10b.007: each event with a session open, a GetObject in progress or none; then Get Device Status
    and GetObjectInfo of a handle given before the event."""
    cases = []
    for kind in M.Responder.BUS_EVENTS:
        for busy in (True, False):
            r = session(vol('tree_basic'))
            h = r.handle('/notes.txt')
            if busy:
                r.begin(0x1009, [h])
            r.bus_event(kind)
            cases.append(dict(event=kind, in_progress=busy, in_progress_after=r.in_progress is not None,
                              status_after=r.class_request(0xA1, 0x67, 4).hex(),
                              session_open_after=r.session != 0, old_handle_code=r.op(0x1008, [h]).code))
    return {'cases': cases}


VECTORS = {'device_info': v_device_info, 'storage_info': v_storage_info, 'listings': v_listings,
           'formats': v_formats, 'object_info': v_object_info, 'partial': v_partial,
           'device_props': v_device_props, 'datetime': v_datetime, 'object_props': v_object_props,
           'op_sweep': v_op_sweep, 'transcripts': v_transcripts, 'class_requests': v_class_requests,
           'bus_events': v_bus_events, 'capacity': v_capacity}


def main():
    out = os.path.join(HERE, 'vectors')
    os.makedirs(out, exist_ok=True)
    for name, fn in VECTORS.items():
        with open(os.path.join(out, name + '.json'), 'w') as f:
            json.dump(fn(), f, indent=None if name == 'op_sweep' else 1, ensure_ascii=False, sort_keys=True)
            f.write('\n')
        print(name)


if __name__ == '__main__':
    main()
