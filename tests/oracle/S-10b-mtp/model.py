# SPDX-License-Identifier: GPL-2.0-or-later
"""S-10b model: an MTP responder over a synthetic yaffs2 volume; the generator of the S-10b vectors.
Handles are bound by path through the listing, never by value.

    Volume.build(tree)            a yaffs2 image from {path: bytes | None (folder) | dict}, read back with
                                  tests/oracle/conformance/fs
    Responder(volume, level)      op(code, params, data) -> Reply(data, code, params); at level 'W' the
                                  operations of S-10b section 9 change the volume's entries
"""
import calendar
import codecs
import datetime
import os
import re
import struct
import sys
import time
from collections import namedtuple

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(SPEC, 'conformance'))
sys.path.insert(0, os.path.join(SPEC, 'S-04-records'))
from fs import yaffs2, yaffs2read  # noqa: E402
from usb import mtp  # noqa: E402
import s04_model  # noqa: E402

Reply = namedtuple('Reply', 'data code params')
STALL = 'STALL'

# ---------------------------------------------------------------- codes (MTP 1.1 appendix F)
OK, GENERAL_ERROR, SESSION_NOT_OPEN = 0x2001, 0x2002, 0x2003
OP_NOT_SUPPORTED, PARAM_NOT_SUPPORTED, INCOMPLETE = 0x2005, 0x2006, 0x2007
INVALID_STORAGE, INVALID_HANDLE, DEVPROP_NOT_SUPPORTED = 0x2008, 0x2009, 0x200A
ACCESS_DENIED, INVALID_PARENT, INVALID_DEVPROP_VALUE = 0x200F, 0x201A, 0x201C
STORE_FULL, WRITE_PROTECTED = 0x200C, 0x200D
INVALID_PARAMETER, SESSION_ALREADY_OPEN = 0x201D, 0x201E
DEVICE_BUSY = 0x2019
NO_VALID_OBJECT_INFO, INVALID_DATASET = 0x2015, 0xA806
INVALID_PROP_CODE, INVALID_PROP_FORMAT, INVALID_PROP_VALUE, DEPTH_UNSUPPORTED = 0xA801, 0xA802, 0xA803, 0xA808

STORAGE_ID = 0x00010001
ALL = 0xFFFFFFFF

OPS_R = [0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x1008, 0x1009, 0x1014, 0x1015, 0x1016,
         0x101B, 0x9801, 0x9802, 0x9803, 0x9805, 0x9810]                                 # S-10b.021
OPS_W = OPS_R + [0x100B, 0x100C, 0x100D, 0x1019, 0x9804]
CAPTURE = [0x100E, 0x1018, 0x101C]                                                         # S-10b.091
DATA_OUT = {0x100C, 0x100D, 0x1016, 0x9804, 0x9811, 0x95C2}                                # S-10b.006
NO_SESSION_OK = {0x1001, 0x1002}                                                           # S-10b.011

DEVICE_PROPS = [0x5001, 0x5011]
CAPTURE_FORMATS = [0x3000, 0x3001, 0x3007, 0x3008, 0xB900, 0x3004]
PLAYBACK_FORMATS = [0x3000, 0x3001, 0x3007, 0x3008, 0xB900]
VENDOR_EXT = 'microsoft.com: 1.0;'                # S-10b.020: 19 characters, count byte 20

# S-10b.046, in order
FORMAT_TABLE = [(('.JPG', '.JPEG'), 0x3801), (('.TIF',), 0x380D), (('.BMP',), 0x3804), (('.PNG',), 0x380B),
                (('.GIF',), 0x3807), (('.EXE',), 0x3003), (('.TXT',), 0x3004), (('.HTM',), 0x3005),
                (('.WAV',), 0x3008), (('.AIFF',), 0x3007), (('.MP3',), 0x3009), (('.AVI',), 0x300A),
                (('.MPEG',), 0x300B), (('.ASF',), 0x300C)]
STORE_FORMATS = {0x3000, 0x3001} | {f for _, f in FORMAT_TABLE}

# S-10b.070 / .071: code -> (datatype, GetSet at R, GetSet at W)
DT_U16, DT_U32, DT_U64, DT_U128, DT_STR = 0x0004, 0x0006, 0x0008, 0x000A, 0xFFFF
PROPS = {0xDC01: DT_U32, 0xDC02: DT_U16, 0xDC03: DT_U16, 0xDC04: DT_U64, 0xDC05: DT_U16, 0xDC07: DT_STR,
         0xDC08: DT_STR, 0xDC09: DT_STR, 0xDC0B: DT_U32, 0xDC41: DT_U128, 0xDC44: DT_STR}
DISPLAY_NAME = 0xDCE0
NAME_PROPS = {0xDC07, 0xDC44, DISPLAY_NAME}

MAX_LEVEL = 9                 # S-10b.041
MAX_READ_PATH = 501           # S-10b.042
NAME_UNITS = 254              # S-10b.047
LIST_OBJECTS = 8192           # S-10b.014: the listing floor, objects
LIST_NAME_BYTES = 262144      # S-10b.014: the listing floor, bytes of names as stored
NAME_MAX = 255                # S-12.058
FOLDER = 0x3001


def creation_outcome(objects, name_bytes, name_len):
    """S-10b.014: what a SendObjectInfo of a name of name_len bytes may do on a volume that stayed within the
    bound since the first listing and now presents `objects` objects with `name_bytes` bytes of names:
    'listed' within the bound, else 'listed_or_refused' (listed, or a failure code and nothing created)."""
    within = objects + 1 <= LIST_OBJECTS and name_bytes + name_len <= LIST_NAME_BYTES
    return 'listed' if within else 'listed_or_refused'


# ---------------------------------------------------------------- encodings
def u16(v):
    return struct.pack('<H', v)


def u32(v):
    return struct.pack('<I', v)


def arr16(xs):
    return u32(len(xs)) + b''.join(u16(x) for x in xs)


def arr32(xs):
    return u32(len(xs)) + b''.join(u32(x) for x in xs)


def mtp_string(text):
    """An MTP String; '' is the single byte 0x00."""
    if text == '':
        return b'\x00'
    b = text.encode('utf-16-le', 'surrogatepass') + b'\x00\x00'
    n = len(b) // 2
    if n > 255:
        raise ValueError('MTP string longer than 255 units')
    return bytes([n]) + b


def _one_fffd_per_byte(err):
    return '\ufffd' * (err.end - err.start), err.end


codecs.register_error('s10b_fffd_per_byte', _one_fffd_per_byte)


def decode_name(name_bytes):
    """S-10b.047: UTF-8 to text; every byte that is not part of a well-formed UTF-8 sequence is one U+FFFD."""
    return name_bytes.decode('utf-8', 's10b_fffd_per_byte')


def presented_name(name_bytes):
    """S-10b.047: UTF-16 of the UTF-8 name, cut to the longest prefix of whole characters within 254 units."""
    out, units = [], 0
    for ch in decode_name(name_bytes):
        n = 2 if ord(ch) > 0xFFFF else 1
        if units + n > NAME_UNITS:
            break
        out.append(ch)
        units += n
    return ''.join(out)


def datetime_text(secs):
    """S-10b.048: the UTC time of a count of seconds since 1970."""
    return time.strftime('%Y%m%dT%H%M%S', time.gmtime(secs))


def format_of(path):
    """S-10b.046: the first row whose pattern occurs anywhere in P, ASCII case ignored."""
    up = ''.join(c.upper() if 'a' <= c <= 'z' else c for c in path)
    for pats, fmt in FORMAT_TABLE:
        if any(p in up for p in pats):
            return fmt
    return 0x3000


DT_RE = re.compile(r'^([0-9]{4})([0-9]{2})([0-9]{2})T([0-9]{2})([0-9]{2})([0-9]{2})(\.[0-9])?(Z|[+-][0-9]{4})?$')
CLOCK_FIRST = datetime.datetime(1970, 1, 1, 0, 0, 0)
CLOCK_LAST = datetime.datetime(2059, 9, 20, 23, 59, 59)


def parse_datetime(text, with_range=True):
    """S-10b.062: the date and time of a well-formed value, else None."""
    m = DT_RE.match(text)
    if not m:
        return None
    y, mo, d, h, mi, s = (int(m.group(i)) for i in range(1, 7))
    if not (1 <= mo <= 12 and 1 <= d <= calendar.monthrange(y, mo)[1] and h <= 23 and mi <= 59 and s <= 59):
        return None
    t = datetime.datetime(y, mo, d, h, mi, s)
    if with_range and not CLOCK_FIRST <= t <= CLOCK_LAST:
        return None
    return t


def rtc_count(t):
    return s04_model.rtc_encode(t)


def secs_of(t):
    """Seconds since 1970 of a naive UTC datetime; None outside 0 to 2**32 - 1 (S-10b.085)."""
    if t.year < 1970:
        return None
    s = calendar.timegm(t.timetuple())
    return s if s < 1 << 32 else None


def host_name(units):
    """S-10b.080, S-10b.087: the name stored for a Filename's units without its final NUL, or None."""
    if 0 in units:
        return None
    try:
        text = struct.pack('<%dH' % len(units), *units).decode('utf-16-le')
    except UnicodeDecodeError:
        return None                                  # an unpaired surrogate
    b = text.encode('utf-8')
    if not b or len(b) > NAME_MAX or b'/' in b or b in (b'.', b'..'):
        return None
    return b


def string_units(s):
    """The units of an mtp.MtpString without its final NUL."""
    u = list(s.units)
    return u[:-1] if u and u[-1] == 0 else u


def host_time(units):
    """S-10b.081: DateModified's time in seconds, or None (the clock's time is used): not well-formed by
    S-10b.062's grammar without the range, on 1970-01-01, or not a u32 count."""
    try:
        text = struct.pack('<%dH' % len(units), *units).decode('utf-16-le')
    except UnicodeDecodeError:
        return None
    t = parse_datetime(text, with_range=False)
    if t is None or (t.year, t.month, t.day) == (1970, 1, 1):
        return None
    return secs_of(t)


def clock_datetime(count):
    """A clock count read back as S-04.070."""
    d = s04_model.rtc_decode_days(count >> 17)
    return datetime.datetime(d.year, d.month, d.day, (count >> 12) & 0x1F, (count >> 6) & 0x3F, count & 0x3F)


# ---------------------------------------------------------------- the volume
class Volume:
    """A yaffs2 image as S-12 presents it: {path P: dict(type, data, mtime, ctime)}; None = no volume."""

    def __init__(self, entries, total=2403870720, free=0):
        self.entries = entries
        self.total, self.free = total, free

    @classmethod
    def build(cls, tree, n_blocks=64, free=1 << 30, total=2403870720):
        """tree: {path without the leading '/': bytes (file) | None (folder) | dict(data=, times=(a, m, c))}."""
        p = yaffs2.op_xy_params(n_blocks=n_blocks)
        b = yaffs2.Yaffs2Builder(p)
        for path in sorted(tree, key=lambda s: s.count('/')):
            v = tree[path]
            kw = {}
            if isinstance(v, dict):
                kw['times'] = v.get('times', (0, 0, 0))
                v = v.get('data')
            if v is None:
                b.mkdir(path, **kw)
            else:
                b.write_file(path, v, **kw)
        r = yaffs2read.Yaffs2Reader(b.image(), p)
        entries = {'/lost+found': dict(type='dir', data=None, mtime=0, ctime=0, atime=0)}
        for oid, o in r.live.items():
            path = r.path_of(oid)
            if path is None:
                continue
            a = r.attributes(oid) or dict(times=(0, 0, 0))
            at, mt, ct = a['times']
            if o['type'] == yaffs2.DIR:
                entries['/' + path] = dict(type='dir', data=None, mtime=mt, ctime=ct, atime=at)
            elif o['type'] == yaffs2.FILE:
                entries['/' + path] = dict(type='file', data=r.file_bytes(o), mtime=mt, ctime=ct, atime=at)
        return cls(entries, total=total, free=free)


def parent_of(path):
    return path.rsplit('/', 1)[0] or '/'


def level_of(path):
    return path.count('/')


# ---------------------------------------------------------------- the responder
class Responder:
    def __init__(self, volume, level='R', battery=57, clock=0, handle_base=0x101, clock_writable=True):
        self.v = volume
        self.level = level
        self.battery = battery
        self.clock = clock                      # the RTC count register (S-04.070)
        self.clock_writable = clock_writable    # False: the register never accepts a write (S-10b.062)
        self.session = 0
        self.handle_base = handle_base
        self.handles = {}                       # handle -> path
        self.by_path = {}
        self.in_progress = None                 # (code, params) of a transaction in progress (S-10b.005)
        self.next_handle = handle_base
        self.prepared = None                    # the file a SendObjectInfo announced (S-10b.080)

    # ---- the class requests (S-10b.005, S-10b.009)
    def begin(self, code, params=()):
        """A command container the device has received, its data phase or response still to come."""
        self.in_progress = (code, list(params))

    def class_request(self, bm, req, wlength=0, data=b''):
        """A Still Image class request to the MTP interface: STALL, or the bytes of its data stage
        (b'' for an accepted host-to-device request). S-10b.005, S-10b.009."""
        if bm == 0x21 and req == 0x64:                                                  # Cancel Request
            if wlength != 6 or len(data) != 6 or struct.unpack_from('<H', data)[0] != 0x4001:
                return STALL
            self.in_progress = None
            return b''
        if bm == 0x21 and req == 0x66:                                                  # Device Reset Request
            if wlength != 0:
                return STALL                    # S-10b.005
            self.in_progress = None
            return b''
        if bm == 0xA1 and req == 0x67:                                                  # Get Device Status
            return (u16(4) + u16(DEVICE_BUSY if self.in_progress else OK))[:wlength]
        if bm == 0xA1 and req == 0x65:                                                  # Get Extended Event Data
            return bytes(8)[:wlength]
        return STALL

    BUS_EVENTS = ('bus_reset', 'set_configuration_0', 'bus_lost', 'set_interface_0', 'set_interface_1',
                  'clear_halt_bulk_in', 'clear_halt_bulk_out', 'clear_halt_interrupt')

    def bus_event(self, kind):
        """S-10b.007: a USB event on the MTP function (then, for the first three, configured again)."""
        assert kind in self.BUS_EVENTS
        if kind in ('bus_reset', 'set_configuration_0', 'bus_lost'):
            self.in_progress = None
            self.session = 0
            self.handles, self.by_path = {}, {}
            self.prepared = None
        elif kind in ('set_interface_0', 'clear_halt_bulk_in', 'clear_halt_bulk_out'):
            self.in_progress = None             # as a Cancel Request (S-10b.005); the session stays open
        # set_interface_1 (S-10a.032) and the interrupt endpoint: no effect

    # ---- listing (S-10b.040-.043)
    def listed_paths(self):
        if self.v is None or self.v.entries is None:
            return []
        out = set()
        for path in sorted(self.v.entries, key=level_of):
            e = self.v.entries[path]
            par = parent_of(path)
            if level_of(path) > MAX_LEVEL:
                continue
            if par != '/' and not (par in out and self.readable(par)):
                continue
            if e['type'] in ('dir', 'file'):
                out.add(path)
        return sorted(out)

    def readable(self, folder):
        return level_of(folder) < MAX_LEVEL and len(folder.encode('utf-8')) <= MAX_READ_PATH

    def open_listing(self):
        self.next_handle = self.handle_base
        self.relist()

    def new_handle(self):
        h = self.next_handle
        while h in (0, ALL) or h >> 16 == 1:
            h += 1
        self.next_handle = h + 1
        return h

    def relist(self):
        """Handles for listed objects that have none: the first listing, and objects a move or rename lets a
        folder list (S-10b.013, S-10b.087). Handles are never reused (S-10b.012)."""
        for path in self.listed_paths():
            if path not in self.by_path:
                h = self.new_handle()
                self.handles[h] = path
                self.by_path[path] = h

    def handle(self, path):
        return self.by_path[path]

    def obj(self, h):
        p = self.handles.get(h)
        return (p, self.v.entries[p]) if p is not None else (None, None)

    def children(self, folder):
        if folder != '/' and not self.readable(folder):
            return []
        return [p for p in self.by_path if parent_of(p) == folder]

    def fmt(self, path):
        return 0x3001 if self.v.entries[path]['type'] == 'dir' else format_of(path)

    def parent_handle(self, path):
        par = parent_of(path)
        return 0 if par == '/' else self.by_path[par]

    # ---- the transaction
    def op(self, code, params=(), data=None):
        params = list(params) + [0] * (5 - len(params))
        if code != 0x100D:
            prepared, self.prepared = self.prepared, None                               # S-10b.080
        else:
            prepared = self.prepared
        supported = OPS_W if self.level == 'W' else OPS_R
        if code not in supported and code not in CAPTURE:
            return Reply(None, OP_NOT_SUPPORTED, [])                                    # S-10b.090
        if code not in NO_SESSION_OK and not self.session:
            return Reply(None, SESSION_NOT_OPEN, [])                                    # S-10b.011
        fn = getattr(self, 'op_%04x' % code, None)
        if fn is None:                                                                  # capture: S-10b.091
            return Reply(None, OK, [])
        if code == 0x100D:
            self.prepared = None
            return fn(params, data, prepared)
        return fn(params, data) if code in DATA_OUT else fn(params)

    def op_1001(self, p):                                                               # S-10b.020
        return Reply(self.device_info(), OK, [])

    def device_info(self, manufacturer='Manufacturer', model='Model', version='1.0.0', serial='SERIAL0001'):
        ops = OPS_W if self.level == 'W' else OPS_R
        return (u16(100) + u32(6) + u16(100) + mtp_string(VENDOR_EXT) + u16(0) + arr16(ops) + arr16([])
                + arr16(DEVICE_PROPS) + arr16(CAPTURE_FORMATS) + arr16(PLAYBACK_FORMATS) + mtp_string(manufacturer)
                + mtp_string(model) + mtp_string(version) + mtp_string(serial))

    def op_1002(self, p):                                                               # S-10b.010
        if self.session:
            return Reply(None, SESSION_ALREADY_OPEN, [self.session])
        if p[0] == 0:
            return Reply(None, INVALID_PARAMETER, [])
        self.session = p[0]
        self.open_listing()
        return Reply(None, OK, [])

    def op_1003(self, p):
        self.session = 0
        self.handles, self.by_path = {}, {}
        self.prepared = None
        return Reply(None, OK, [])

    def op_1004(self, p):                                                               # S-10b.030
        return Reply(arr32([STORAGE_ID]), OK, [])

    def op_1005(self, p):                                                               # S-10b.031
        if p[0] != STORAGE_ID:
            return Reply(None, INVALID_STORAGE, [])
        return Reply(self.storage_info(), OK, [])

    def storage_info(self, product='Model'):
        mounted = self.v is not None and self.v.entries is not None
        total = (self.v.total // 1024 * 1024) if mounted else 0
        free = (self.v.free // 1024 * 1024) if mounted else 0
        return (u16(3) + u16(2) + u16(1 if self.level == 'R' else 0) + struct.pack('<QQI', total, free, ALL)
                + mtp_string(product) + mtp_string(''))

    def select_handles(self, p):                                                        # S-10b.044
        if p[0] not in (STORAGE_ID, ALL):
            return INVALID_STORAGE, None
        par = p[2]
        if par == ALL:
            paths = self.children('/')
        elif par == 0:
            paths = list(self.by_path)
        else:
            path, e = self.obj(par)
            if path is None:
                return INVALID_HANDLE, None
            if e['type'] != 'dir':
                return INVALID_PARENT, None
            paths = self.children(path)
        if p[1]:
            paths = [x for x in paths if self.fmt(x) == p[1]]
        return OK, sorted(paths)

    def op_1006(self, p):                                                               # S-10b.045
        rc, paths = self.select_handles(p)
        return Reply(None, rc, [len(paths)] if rc == OK else [])

    def op_1007(self, p):
        rc, paths = self.select_handles(p)
        if rc != OK:
            return Reply(None, rc, [])
        return Reply(arr32([self.by_path[x] for x in paths]), OK, [])

    def object_info(self, path):                                                        # S-10b.049
        e = self.v.entries[path]
        folder = e['type'] == 'dir'
        ctime = e['ctime'] or e['mtime']
        return (u32(STORAGE_ID) + u16(self.fmt(path)) + u16(0) + u32(0 if folder else len(e['data'])) + u16(0)
                + u32(0) * 6 + u32(self.parent_handle(path)) + u16(1 if folder else 0) + u32(0) + u32(0)
                + mtp_string(presented_name(path.rsplit('/', 1)[1].encode('utf-8')))
                + mtp_string(datetime_text(ctime)) + mtp_string(datetime_text(e['mtime'])) + mtp_string(''))

    def op_1008(self, p):
        path, e = self.obj(p[0])
        if path is None:
            return Reply(None, INVALID_HANDLE, [])
        return Reply(self.object_info(path), OK, [])

    def op_1009(self, p):                                                               # S-10b.050
        path, e = self.obj(p[0])
        if path is None or e['type'] != 'file':
            return Reply(None, INVALID_HANDLE, [])
        return Reply(e['data'], OK, [])

    def op_101b(self, p):                                                               # S-10b.051
        path, e = self.obj(p[0])
        if path is None or e['type'] != 'file':
            return Reply(None, INVALID_HANDLE, [])
        n, off = len(e['data']), p[1]
        if off > n:
            return Reply(None, INVALID_PARAMETER, [])
        out = e['data'][off:min(off + p[2], n)]
        return Reply(out, OK, [len(out)])

    def prop_desc(self, code, fmt):                                                     # S-10b.060
        if code == 0x5001:
            return u16(0x5001) + u16(0x0002) + bytes([0, 100, self.battery, 1, 0, 100, 1])
        if code == 0x5011:
            return u16(0x5011) + u16(0xFFFF) + bytes([1]) + mtp_string('') + mtp_string('') + bytes([0])
        return None

    def op_1014(self, p):
        d = self.prop_desc(p[0], None)
        return Reply(d, OK, []) if d is not None else Reply(None, DEVPROP_NOT_SUPPORTED, [])

    def op_1015(self, p):                                                               # S-10b.061
        if p[0] == 0x5001:
            return Reply(bytes([self.battery]), OK, [])
        if p[0] == 0x5011:
            return Reply(mtp_string(clock_datetime(self.clock).strftime('%Y%m%dT%H%M%S')), OK, [])
        return Reply(None, DEVPROP_NOT_SUPPORTED, [])

    def op_1016(self, p, data):                                                         # S-10b.062, .064
        if p[0] == 0x5001:
            return Reply(None, ACCESS_DENIED, [])
        if p[0] != 0x5011:
            return Reply(None, DEVPROP_NOT_SUPPORTED, [])
        text = decode_string(data)
        t = parse_datetime(text) if text is not None else None
        if t is None:
            return Reply(None, INVALID_DEVPROP_VALUE, [])
        if self.clock_writable:
            self.clock = rtc_count(t)
        return Reply(None, OK, [])              # the answer is the value's, not the write's (S-10b.062)

    def op_9801(self, p):                                                               # S-10b.070
        return Reply(arr16(list(PROPS)), OK, [])

    def object_prop_desc(self, code, fmt):                                              # S-10b.071
        tail = u32(0) + bytes([0])                       # GroupCode, FormFlag
        if code in NAME_PROPS:
            if fmt not in STORE_FORMATS:
                return INVALID_PROP_FORMAT, None
            getset = 1 if code == 0xDC07 and self.level == 'W' else 0
            return OK, u16(code) + u16(DT_STR) + bytes([getset]) + mtp_string('') + tail
        if code not in PROPS:
            return INVALID_PROP_CODE, None
        dt = PROPS[code]
        default = {0xDC02: fmt & 0xFFFF, 0xDC05: 1 if fmt == 0x3001 else 0}.get(code, 0)
        if dt == DT_STR:
            dv = mtp_string('')
        else:
            dv = default.to_bytes({DT_U16: 2, DT_U32: 4, DT_U64: 8, DT_U128: 16}[dt], 'little')
        return OK, u16(code) + u16(dt) + bytes([0]) + dv + tail

    def op_9802(self, p):
        rc, d = self.object_prop_desc(p[0], p[1])
        return Reply(d, rc, [])

    def prop_value(self, path, code):                                                   # S-10b.072
        e = self.v.entries[path]
        folder = e['type'] == 'dir'
        if code == 0xDC01:
            return u32(STORAGE_ID)
        if code == 0xDC02:
            return u16(self.fmt(path))
        if code == 0xDC03:
            return u16(0)
        if code == 0xDC04:
            return struct.pack('<Q', 0 if folder else len(e['data']))
        if code == 0xDC05:
            return u16(1 if folder else 0)
        if code in NAME_PROPS:
            return mtp_string(presented_name(path.rsplit('/', 1)[1].encode('utf-8')))
        if code == 0xDC08:
            return mtp_string(datetime_text(e['ctime'] or e['mtime']))
        if code == 0xDC09:
            return mtp_string(datetime_text(e['mtime']))
        if code == 0xDC0B:
            return u32(self.parent_handle(path))
        if code == 0xDC41:
            return u32(self.by_path[path]) + u32(STORAGE_ID) + u32(0) + u32(0)
        return None

    def op_9803(self, p):
        path, e = self.obj(p[0])
        if path is None:
            return Reply(None, INVALID_HANDLE, [])
        v = self.prop_value(path, p[1])
        return Reply(v, OK, []) if v is not None else Reply(None, INVALID_PROP_CODE, [])

    def select_prop_list(self, h, depth):                                               # S-10b.074
        if depth not in (0, 1, ALL):
            return DEPTH_UNSUPPORTED, None
        if h in (ALL, STORAGE_ID) or (h == 0 and depth == ALL):
            return OK, list(self.by_path)
        if h == 0:
            return OK, ([] if depth == 0 else self.children('/'))     # the root is not an object
        if h >> 16 == 1:
            return INVALID_STORAGE, None
        path, e = self.obj(h)
        if path is None:
            return INVALID_HANDLE, None
        if depth == 0:
            return OK, [path]
        if depth == ALL:
            return DEPTH_UNSUPPORTED, None
        return OK, self.children(path) if e['type'] == 'dir' else []

    def op_9805(self, p):                                                               # S-10b.073
        h, fmt, prop, _group, depth = p
        rc, paths = self.select_prop_list(h, depth)
        if rc != OK:
            return Reply(None, rc, [])
        if prop in (0, ALL):
            props = list(PROPS)
        elif prop in PROPS:
            props = [prop]
        else:
            return Reply(None, INVALID_PROP_CODE, [])
        if fmt:
            paths = [x for x in paths if self.fmt(x) == fmt]
        els = []
        for path in sorted(paths):
            for c in props:
                els.append(u32(self.by_path[path]) + u16(c) + u16(PROPS[c]) + self.prop_value(path, c))
        return Reply(u32(len(els)) + b''.join(els), OK, [])

    def op_9810(self, p):                                                               # S-10b.075
        path, e = self.obj(p[0])
        if path is None:
            return Reply(None, INVALID_HANDLE, [])
        kids = self.children(path) if e['type'] == 'dir' else []
        return Reply(arr32([self.by_path[x] for x in sorted(kids)]), OK, [])

    # ---- level W (S-10b.080-.087)
    def now(self):
        return secs_of(clock_datetime(self.clock)) or 0

    def lost_found(self, path):
        return path == '/lost+found'

    def child(self, folder, name):
        return (folder.rstrip('/') + '/' + name.decode('utf-8', 's10b_fffd_per_byte')) if folder != '/' else \
            '/' + name.decode('utf-8', 's10b_fffd_per_byte')

    def taken(self, folder, name):
        return any(parent_of(p) == folder and p.rsplit('/', 1)[1].encode('utf-8', 'surrogateescape') == name
                   for p in self.by_path)

    def keeps_listed(self, old, new):
        """S-10b.087: with `old` (and what is beneath it) at `new`, every listed object beneath is still in a
        folder whose children are listed."""
        for p in self.by_path:
            if p.startswith(old + '/'):
                q = new + p[len(old):]
                par = parent_of(q)
                if level_of(q) > MAX_LEVEL or not self.readable(par):
                    return False
        return True

    def move_entries(self, old, new):
        """Renames `old` and everything beneath it in the volume; handles follow (S-10b.012)."""
        ents = self.v.entries
        for p in sorted(ents):
            if p == old or p.startswith(old + '/'):
                q = new + p[len(old):]
                ents[q] = ents.pop(p)
                if p in self.by_path:
                    h = self.by_path.pop(p)
                    self.by_path[q] = h
                    self.handles[h] = q
        self.relist()

    def op_100b(self, p):                                                               # S-10b.082
        if p[0] == ALL:
            return Reply(None, PARAM_NOT_SUPPORTED, [])
        path, e = self.obj(p[0])
        if path is None:
            return Reply(None, INVALID_HANDLE, [])
        if self.lost_found(path):
            return Reply(None, WRITE_PROTECTED, [])
        for q in [q for q in self.v.entries if q == path or q.startswith(path + '/')]:
            del self.v.entries[q]
            if q in self.by_path:
                del self.handles[self.by_path.pop(q)]
        return Reply(None, OK, [])

    def op_100c(self, p, data):                                                         # S-10b.080
        if p[0] not in (STORAGE_ID, 0):
            return Reply(None, INVALID_STORAGE, [])
        folder = '/'
        if p[1] not in (0, ALL):
            path, e = self.obj(p[1])
            if path is None or e['type'] != 'dir' or not self.readable(path):          # S-10b.087
                return Reply(None, INVALID_PARENT, [])
            folder = path
        try:
            v, _ = mtp.decode_object_info(data or b'')
        except mtp.MalformedError:
            return Reply(None, INVALID_DATASET, [])
        if v.get('DateModified') is None:
            return Reply(None, INVALID_DATASET, [])
        name = host_name(string_units(v['Filename']))
        if name is None:
            return Reply(None, INVALID_DATASET, [])
        if self.taken(folder, name):
            return Reply(None, GENERAL_ERROR, [])                                       # Q-05
        new = self.child(folder, name)
        parent_h = ALL if folder == '/' else self.by_path[folder]
        if v['ObjectFormat'] == FOLDER:
            now = self.now()
            self.v.entries[new] = dict(type='dir', data=None, mtime=now, ctime=now, atime=now)
            h = self.new_handle()
            self.handles[h], self.by_path[new] = new, h
            return Reply(None, OK, [STORAGE_ID, parent_h, h])
        if v['ObjectCompressedSize'] > self.v.free:
            return Reply(None, STORE_FULL, [])
        h = self.new_handle()
        self.prepared = dict(path=new, size=v['ObjectCompressedSize'], handle=h,
                             mtime=host_time(string_units(v['DateModified'])))
        return Reply(None, OK, [STORAGE_ID, parent_h, h])

    def op_100d(self, p, data, prepared=None):                                          # S-10b.081
        if prepared is None:
            return Reply(None, NO_VALID_OBJECT_INFO, [])
        data = data or b''
        if len(data) != prepared['size']:
            return Reply(None, INCOMPLETE, [])
        now = self.now()
        path = prepared['path']
        mtime = prepared['mtime'] if prepared['mtime'] is not None else now
        self.v.entries[path] = dict(type='file', data=bytes(data), mtime=mtime, ctime=now, atime=now)
        self.handles[prepared['handle']] = path
        self.by_path[path] = prepared['handle']
        return Reply(None, OK, [])

    def op_1019(self, p):                                                               # S-10b.083
        if p[1] != STORAGE_ID:
            return Reply(None, INVALID_STORAGE, [])
        path, e = self.obj(p[0])
        if path is None:
            return Reply(None, INVALID_HANDLE, [])
        folder = '/'
        if p[2] not in (0, ALL):
            folder, fe = self.obj(p[2])
            if folder is None:
                return Reply(None, INVALID_HANDLE, [])
            if fe['type'] != 'dir' or folder == path or folder.startswith(path + '/'):
                return Reply(None, INVALID_PARENT, [])
        if self.lost_found(path):
            return Reply(None, WRITE_PROTECTED, [])
        name = path.rsplit('/', 1)[1]
        new = (folder if folder != '/' else '') + '/' + name
        if (folder != '/' and not self.readable(folder)) or not self.keeps_listed(path, new):
            return Reply(None, INVALID_PARENT, [])                                      # S-10b.087
        if self.taken(folder, name.encode('utf-8')):
            return Reply(None, GENERAL_ERROR, [])
        self.move_entries(path, new)
        return Reply(None, OK, [])

    def op_9804(self, p, data):                                                         # S-10b.084, .085
        path, e = self.obj(p[0])
        if path is None:
            return Reply(None, INVALID_HANDLE, [])
        if p[1] in NAME_PROPS:
            if self.lost_found(path):
                return Reply(None, WRITE_PROTECTED, [])
            try:
                s, _ = mtp.read_string(data or b'', 0)
            except mtp.MalformedError:
                return Reply(None, INVALID_PROP_VALUE, [])
            name = host_name(string_units(s))
            folder = parent_of(path)
            new = self.child(folder, name) if name is not None else None
            if name is None or not self.keeps_listed(path, new):
                return Reply(None, INVALID_PROP_VALUE, [])
            if self.taken(folder, name):
                return Reply(None, GENERAL_ERROR, [])                                   # Q-05
            self.move_entries(path, new)
            return Reply(None, OK, [])
        if p[1] == 0xDC09:
            try:
                s, _ = mtp.read_string(data or b'', 0)
                text = struct.pack('<%dH' % len(string_units(s)), *string_units(s)).decode('utf-16-le')
            except (mtp.MalformedError, UnicodeDecodeError):
                return Reply(None, INVALID_PROP_VALUE, [])
            t = parse_datetime(text, with_range=False)
            secs = secs_of(t) if t is not None else None
            if secs is None:
                return Reply(None, INVALID_PROP_VALUE, [])
            if secs != e['mtime']:                                                      # S-16.067
                e['mtime'], e['atime'] = secs, self.now()                               # S-16.068
            return Reply(None, OK, [])
        return Reply(None, ACCESS_DENIED, [])


def decode_string(b):
    if not b:
        return None
    n = b[0]
    if n == 0:
        return ''
    raw = b[1:1 + 2 * n]
    s = raw.decode('utf-16-le', 'surrogatepass')
    return s[:-1] if s.endswith('\x00') else s
