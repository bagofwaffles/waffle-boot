#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Copy a file to the unit's /yaffs2 volume over MTP and prove it by reading it back.

    mtpcp.py ls                       list the volume's top level with sizes, and the free space
    mtpcp.py put FILE [--as os.ldr]   delete the old copy, upload FILE, read it back, compare bytes
    mtpcp.py verify FILE [--as os.ldr]  read the copy on the unit back and compare it with FILE
    mtpcp.py tree OUT.json [--hash]     list the whole volume (sizes, dates, and with --hash each
                                      file's SHA-256, read with GetObject) into OUT.json; read only
    mtpcp.py compare A.json B.json    the differences between two tree listings (no unit needed)
    mtpcp.py rm PATH                  delete the file /yaffs2/PATH (never a folder, never os.ldr)
    mtpcp.py mkdir PATH               make the folder /yaffs2/PATH (its parent must exist)
    mtpcp.py mv FROM TO               rename or move /yaffs2/FROM to /yaffs2/TO, or into the folder TO

PATH, FROM and TO are paths in the volume, `user/a.txt`; put and verify take a name at the top only.

Exit status 0 only when the bytes on the unit are FILE's bytes (put, verify) or the listing was read
(ls). Anything else prints FAILED and exits 1. The read-back is the proof that the bytes arrived; nothing
later in the chain is relied on to check them.

Needs Python 3 and libusb (`brew install libusb`; libmtp pulls it in too), no Python packages. Serves
the developer menu's MTP item (S-15.093, its interface by the MTP target of S-15.174): macOS, the vendor interface, and Windows, the
standard one, which macOS's camera daemon may hold first (wiki/Test-OS.md).

It is a careful MTP copy of its own, not a wrapper of libmtp: it sends only the operations any responder
for the device is required to handle, and checks what comes back rather than trusting it. Only these
operations are ever sent: GetDeviceInfo, OpenSession, CloseSession, GetStorageIDs, GetStorageInfo,
GetObjectHandles, GetObjectInfo, GetObject, DeleteObject, SendObjectInfo, SendObject, and for mv
MoveObject and SetObjectPropValue (ObjectFileName). In particular:

- never GetPartialObject: a file is read whole with GetObject, so nothing rests on partial reads
  (S-16.055);
- never GetObjectPropList (S-16.057), which libmtp-based hosts list folders with when it is offered
  (S-16.183);
- the root is listed with parent 0xFFFFFFFF, the form known hosts use (S-16.177);
- a container's length is not trusted to match what was sent (S-16.056 states the rule): a data phase
  ends at a short packet or at its length, whichever comes first;
- a data container left from an earlier transaction is dropped, since a data phase belongs only to its
  own transaction (S-16.064);
- a successful GetObject is not taken as proof of anything: the bytes are compared;
- a folder is never deleted, since folder deletes are recursive (S-16.067);
- nothing is changed on a responder whose store is read-only (AccessCapability not 0, S-10b.031);
- every change is checked afterwards from a new session's listing.

waffle-boot's developer menu serves level W (S-10b.021): every command works. A level-R responder lists and
reads, and put, rm, mkdir and mv stop before changing anything.
"""
import argparse
import ctypes
import ctypes.util
import datetime
import hashlib
import json
import os
import struct
import sys

VID = 0x2367                # S-10a.001
PID_BOOT = 0x0021           # the MTP sets, and the serial and disk sets
PID_MIDI = 0x8021

CMD, DATA, RESP, EVENT = 1, 2, 3, 4
OP_DEVICE_INFO, OP_OPEN, OP_CLOSE, OP_STORAGE_IDS = 0x1001, 0x1002, 0x1003, 0x1004
OP_STORAGE_INFO, OP_HANDLES, OP_OBJECT_INFO, OP_GET_OBJECT = 0x1005, 0x1007, 0x1008, 0x1009
OP_DELETE, OP_SEND_INFO, OP_SEND = 0x100B, 0x100C, 0x100D
OP_MOVE, OP_SET_PROP = 0x1019, 0x9804
PROP_FILE_NAME = 0xDC07
OK, ALREADY_OPEN = 0x2001, 0x201E
INVALID_HANDLE, INVALID_PARENT = 0x2009, 0x201A
ROOT = 0xFFFFFFFF
FORMAT_UNDEFINED, FORMAT_FOLDER = 0x3000, 0x3001

RESPONSES = {
    0x2001: 'OK', 0x2002: 'General_Error', 0x2003: 'Session_Not_Open',
    0x2005: 'Operation_Not_Supported', 0x2006: 'Parameter_Not_Supported',
    0x2007: 'Incomplete_Transfer', 0x2008: 'Invalid_StorageID', 0x2009: 'Invalid_ObjectHandle',
    0x200C: 'Store_Full', 0x200D: 'Object_WriteProtected', 0x200F: 'Access_Denied',
    0x2015: 'No_Valid_ObjectInfo', 0x2019: 'Device_Busy', 0x201A: 'Invalid_ParentObject',
    0x201D: 'Invalid_Parameter', 0x201E: 'Session_Already_Open', 0xA806: 'Invalid_Dataset',
}

TIMEOUT_MS = 60000          # one bulk transfer, or the wait for a response
READ_SIZE = 65536           # a multiple of both packet sizes (S-10a), so a transfer cannot overflow
WRITE_SIZE = 65536


class Failure(Exception):
    """The run cannot give its answer; the message says what happened."""


class Timeout(Failure):
    pass


# ---------------------------------------------------------------- libusb, through ctypes

class DeviceDescriptor(ctypes.Structure):
    _fields_ = [('bLength', ctypes.c_uint8), ('bDescriptorType', ctypes.c_uint8),
                ('bcdUSB', ctypes.c_uint16), ('bDeviceClass', ctypes.c_uint8),
                ('bDeviceSubClass', ctypes.c_uint8), ('bDeviceProtocol', ctypes.c_uint8),
                ('bMaxPacketSize0', ctypes.c_uint8), ('idVendor', ctypes.c_uint16),
                ('idProduct', ctypes.c_uint16), ('bcdDevice', ctypes.c_uint16),
                ('iManufacturer', ctypes.c_uint8), ('iProduct', ctypes.c_uint8),
                ('iSerialNumber', ctypes.c_uint8), ('bNumConfigurations', ctypes.c_uint8)]


class EndpointDescriptor(ctypes.Structure):
    _fields_ = [('bLength', ctypes.c_uint8), ('bDescriptorType', ctypes.c_uint8),
                ('bEndpointAddress', ctypes.c_uint8), ('bmAttributes', ctypes.c_uint8),
                ('wMaxPacketSize', ctypes.c_uint16), ('bInterval', ctypes.c_uint8),
                ('bRefresh', ctypes.c_uint8), ('bSynchAddress', ctypes.c_uint8),
                ('extra', ctypes.c_void_p), ('extra_length', ctypes.c_int)]


class InterfaceDescriptor(ctypes.Structure):
    _fields_ = [('bLength', ctypes.c_uint8), ('bDescriptorType', ctypes.c_uint8),
                ('bInterfaceNumber', ctypes.c_uint8), ('bAlternateSetting', ctypes.c_uint8),
                ('bNumEndpoints', ctypes.c_uint8), ('bInterfaceClass', ctypes.c_uint8),
                ('bInterfaceSubClass', ctypes.c_uint8), ('bInterfaceProtocol', ctypes.c_uint8),
                ('iInterface', ctypes.c_uint8), ('endpoint', ctypes.POINTER(EndpointDescriptor)),
                ('extra', ctypes.c_void_p), ('extra_length', ctypes.c_int)]


class Interface(ctypes.Structure):
    _fields_ = [('altsetting', ctypes.POINTER(InterfaceDescriptor)),
                ('num_altsetting', ctypes.c_int)]


class ConfigDescriptor(ctypes.Structure):
    _fields_ = [('bLength', ctypes.c_uint8), ('bDescriptorType', ctypes.c_uint8),
                ('wTotalLength', ctypes.c_uint16), ('bNumInterfaces', ctypes.c_uint8),
                ('bConfigurationValue', ctypes.c_uint8), ('iConfiguration', ctypes.c_uint8),
                ('bmAttributes', ctypes.c_uint8), ('MaxPower', ctypes.c_uint8),
                ('interface', ctypes.POINTER(Interface)),
                ('extra', ctypes.c_void_p), ('extra_length', ctypes.c_int)]


LIBUSB_ERROR_ACCESS, LIBUSB_ERROR_BUSY, LIBUSB_ERROR_TIMEOUT = -3, -6, -7
LIBUSB_ERROR_PIPE = -9
MTP_CLASSES = ((0x06, 0x01, 0x01), (0xFF, 0x01, 0x01))  # S-10a: standard, vendor


def load_libusb():
    candidates = [ctypes.util.find_library('usb-1.0'), '/opt/homebrew/lib/libusb-1.0.dylib',
                  '/usr/local/lib/libusb-1.0.dylib', 'libusb-1.0.so.0']
    for name in candidates:
        if not name:
            continue
        try:
            lib = ctypes.CDLL(name)
        except OSError:
            continue
        lib.libusb_init.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        lib.libusb_exit.argtypes = [ctypes.c_void_p]
        lib.libusb_exit.restype = None
        lib.libusb_get_device_list.argtypes = [ctypes.c_void_p,
                                               ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))]
        lib.libusb_get_device_list.restype = ctypes.c_ssize_t
        lib.libusb_free_device_list.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_int]
        lib.libusb_free_device_list.restype = None
        lib.libusb_get_device_descriptor.argtypes = [ctypes.c_void_p,
                                                     ctypes.POINTER(DeviceDescriptor)]
        lib.libusb_get_active_config_descriptor.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.POINTER(ConfigDescriptor))]
        lib.libusb_free_config_descriptor.argtypes = [ctypes.POINTER(ConfigDescriptor)]
        lib.libusb_free_config_descriptor.restype = None
        lib.libusb_open.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        lib.libusb_close.argtypes = [ctypes.c_void_p]
        lib.libusb_close.restype = None
        lib.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libusb_set_auto_detach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libusb_bulk_transfer.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_char_p,
                                             ctypes.c_int, ctypes.POINTER(ctypes.c_int),
                                             ctypes.c_uint]
        lib.libusb_error_name.argtypes = [ctypes.c_int]
        lib.libusb_error_name.restype = ctypes.c_char_p
        return lib
    raise Failure('libusb was not found: brew install libusb')


def find_mtp_interface(cfg):
    """(interface number, bulk IN, bulk OUT, packet size) of the MTP function, or None."""
    for i in range(cfg.bNumInterfaces):
        ifc = cfg.interface[i]
        for a in range(ifc.num_altsetting):
            d = ifc.altsetting[a]
            if (d.bInterfaceClass, d.bInterfaceSubClass, d.bInterfaceProtocol) not in MTP_CLASSES:
                continue
            ep_in = ep_out = None
            mps = 0
            for e in range(d.bNumEndpoints):
                ep = d.endpoint[e]
                if ep.bmAttributes & 3 != 2:
                    continue
                mps = ep.wMaxPacketSize & 0x7FF
                if ep.bEndpointAddress & 0x80:
                    ep_in = ep.bEndpointAddress
                else:
                    ep_out = ep.bEndpointAddress
            if ep_in is not None and ep_out is not None:
                return d.bInterfaceNumber, ep_in, ep_out, mps
    return None


class UsbTransport:
    """The MTP interface's two bulk endpoints. read() ends at a short packet, as a bulk transfer does."""

    def __init__(self):
        self.lib = load_libusb()
        self.ctx = ctypes.c_void_p()
        self.handle = None
        self.claimed = None
        r = self.lib.libusb_init(ctypes.byref(self.ctx))
        if r:
            raise Failure('libusb_init: %s' % self.error(r))
        try:
            self._open()
        except Exception:
            self.close()
            raise

    def error(self, r):
        return self.lib.libusb_error_name(r).decode()

    def _open(self):
        lib = self.lib
        devices = ctypes.POINTER(ctypes.c_void_p)()
        n = lib.libusb_get_device_list(self.ctx, ctypes.byref(devices))
        if n < 0:
            raise Failure('libusb_get_device_list: %s' % self.error(n))
        chosen = None
        other = None
        try:
            for i in range(n):
                dd = DeviceDescriptor()
                if lib.libusb_get_device_descriptor(devices[i], ctypes.byref(dd)) or dd.idVendor != VID:
                    continue
                other = other or dd.idProduct
                cfg = ctypes.POINTER(ConfigDescriptor)()
                if lib.libusb_get_active_config_descriptor(devices[i], ctypes.byref(cfg)):
                    continue
                found = find_mtp_interface(cfg.contents)
                lib.libusb_free_config_descriptor(cfg)
                if found is not None:
                    chosen = devices[i]
                    self.number, self.ep_in, self.ep_out, self.mps = found
                    break
            if chosen is None:
                if other is None:
                    raise Failure('no OP-XY on USB')
                raise Failure('the OP-XY on USB (product %04x) is not offering MTP: press digit 2 '
                              'in its developer menu' % other)
            handle = ctypes.c_void_p()
            r = lib.libusb_open(chosen, ctypes.byref(handle))
            if r:
                raise Failure('cannot open the OP-XY: %s' % self.error(r))
            self.handle = handle
        finally:
            lib.libusb_free_device_list(devices, 1)
        lib.libusb_set_auto_detach_kernel_driver(self.handle, 1)    # Linux; refused elsewhere, harmlessly
        r = lib.libusb_claim_interface(self.handle, self.number)
        if r in (LIBUSB_ERROR_ACCESS, LIBUSB_ERROR_BUSY):
            raise Failure('something else holds the unit\'s MTP interface (%s). Quit any MTP client '
                          '(Android File Transfer and its agent, OpenMTP, Image Capture); on the Windows target '
                          'macOS\'s ptpcamerad takes it by itself, so set the target to macOS'
                          % self.error(r))
        if r:
            raise Failure('cannot claim the MTP interface: %s' % self.error(r))
        self.claimed = self.number

    def write(self, data, timeout_ms=TIMEOUT_MS):
        done = ctypes.c_int()
        r = self.lib.libusb_bulk_transfer(self.handle, self.ep_out, bytes(data), len(data),
                                          ctypes.byref(done), timeout_ms)
        if r == LIBUSB_ERROR_TIMEOUT:
            raise Timeout('USB write timed out after %d of %d bytes' % (done.value, len(data)))
        if r or done.value != len(data):
            raise Failure('USB write: %s, %d of %d bytes' % (self.error(r), done.value, len(data)))

    def read(self, size, timeout_ms=TIMEOUT_MS):
        buf = ctypes.create_string_buffer(size)
        done = ctypes.c_int()
        r = self.lib.libusb_bulk_transfer(self.handle, self.ep_in, buf, size, ctypes.byref(done),
                                          timeout_ms)
        if r == LIBUSB_ERROR_TIMEOUT and done.value == 0:
            raise Timeout('USB read timed out')
        if r and r != LIBUSB_ERROR_TIMEOUT:
            raise Failure('USB read: %s' % self.error(r))
        return buf.raw[:done.value]

    def close(self):
        if self.claimed is not None:
            self.lib.libusb_release_interface(self.handle, self.claimed)
        if self.handle:
            self.lib.libusb_close(self.handle)
        self.lib.libusb_exit(self.ctx)


# ---------------------------------------------------------------- PTP/MTP containers and datasets

def mtp_string(text):
    """An MTP String (S-10b, Terms)."""
    if not text:
        return b'\0'
    units = text.encode('utf-16-le') + b'\0\0'
    return bytes([len(units) // 2]) + units


class Reader:
    """A dataset's fields; a field the dataset does not carry reads as zero or empty (S-16.056)."""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def num(self, fmt):
        n = struct.calcsize(fmt)
        chunk = self.data[self.pos:self.pos + n]
        self.pos += n
        return struct.unpack(fmt, chunk)[0] if len(chunk) == n else 0

    def string(self):
        n = self.num('<B')
        chunk = self.data[self.pos:self.pos + 2 * n]
        self.pos += 2 * n
        return chunk.decode('utf-16-le', 'replace').split('\0', 1)[0]

    def skip_array(self, size):
        n = self.num('<I')
        self.pos += n * size


class Object:
    def __init__(self, handle, data):
        r = Reader(data)
        self.handle = handle
        r.num('<I')                     # StorageID
        self.format = r.num('<H')
        r.num('<H')                     # ProtectionStatus
        self.size = r.num('<I')
        r.pos += 2 + 6 * 4              # the thumb and image fields
        self.parent = r.num('<I')
        r.pos += 2 + 4 + 4              # AssociationType, AssociationDesc, SequenceNumber
        self.name = r.string()
        r.string()                      # DateCreated
        self.modified = r.string()

    @property
    def is_folder(self):
        return self.format == FORMAT_FOLDER


def object_info_dataset(storage, name, size, modified, folder=False):
    """The ObjectInfo dataset (S-10b.049) of a file or folder to send."""
    return (struct.pack('<IHHIHIIIIIIIHII', storage, FORMAT_FOLDER if folder else FORMAT_UNDEFINED, 0,
                        size, 0, 0, 0, 0, 0, 0, 0, 0, 1 if folder else 0, 0, 0)
            + mtp_string(name) + mtp_string(modified) + mtp_string(modified) + mtp_string(''))


class Mtp:
    """Transactions over a transport with write(), read() and mps."""

    def __init__(self, transport):
        self.t = transport
        self.tid = 0
        self.session = 0
        self.stale = 0

    def drain(self):
        """Drop whatever an earlier client left in the device's IN queue (S-16.064)."""
        try:
            while True:
                self.t.read(READ_SIZE, 100)
        except Timeout:
            pass

    def _container(self):
        """The next container: (type, code, tid, payload). Zero-length packets between are skipped."""
        chunk = b''
        while not chunk:
            chunk = self.t.read(READ_SIZE)
        if len(chunk) < 12:
            raise Failure('short container from the device: %s' % chunk.hex())
        total, kind, code, tid = struct.unpack_from('<IHHI', chunk)
        if kind not in (DATA, RESP, EVENT) or total < 12:
            raise Failure('not a device container: %s' % chunk[:12].hex())
        parts = [chunk]
        got = len(chunk)
        # The length is not trusted (S-16.056 states the rule): a short packet ends the phase too.
        while kind == DATA and got < total and len(chunk) % self.t.mps == 0:
            chunk = self.t.read(READ_SIZE)
            if not chunk:
                break
            parts.append(chunk)
            got += len(chunk)
        return kind, code, tid, b''.join(parts)[12:]

    def transaction(self, code, params=(), send=None):
        """Runs one operation; (response code, response parameters, data or None if no data phase)."""
        self.tid += 1
        self.t.write(struct.pack('<IHHI%dI' % len(params), 12 + 4 * len(params), CMD, code, self.tid,
                                 *params))
        if send is not None:
            self._send_data(code, send)
        data = None
        while True:
            kind, rcode, tid, payload = self._container()
            if kind == EVENT:
                continue
            if kind == DATA:
                if tid != self.tid:
                    self.stale += 1             # S-16.064: an earlier transaction's
                    continue
                data = payload
                continue
            if tid != self.tid:
                raise Failure('response to transaction %d during transaction %d' % (tid, self.tid))
            n = min(len(payload) // 4, 5)
            return rcode, struct.unpack('<%dI' % n, payload[:4 * n]), data

    def _send_data(self, code, payload):
        blob = struct.pack('<IHHI', 12 + len(payload), DATA, code, self.tid) + payload
        step = WRITE_SIZE - WRITE_SIZE % self.t.mps
        for off in range(0, len(blob), step):
            self.t.write(blob[off:off + step])
        if len(blob) % self.t.mps == 0:
            self.t.write(b'')                   # S-10b.003

    def checked(self, name, code, params=(), send=None):
        rcode, rparams, data = self.transaction(code, params, send)
        if rcode != OK:
            raise Failure('%s answered %s' % (name, response_name(rcode)))
        return rparams, data

    def read_op(self, name, code, params=()):
        """An operation with a data phase from the device. OK without one is not an empty answer."""
        data = self.checked(name, code, params)[1]
        if data is None:
            raise Failure('%s answered OK with no data phase' % name)
        return data

    def open_session(self):
        self.session += 1
        code, _, _ = self.transaction(OP_OPEN, (self.session,))
        if code not in (OK, ALREADY_OPEN):      # S-16.176
            raise Failure('OpenSession answered %s' % response_name(code))

    def close_session(self):
        code, _, _ = self.transaction(OP_CLOSE)
        return code == OK

    def device_info(self):
        r = Reader(self.read_op('GetDeviceInfo', OP_DEVICE_INFO))
        r.pos += 2 + 4 + 2
        r.string()
        r.pos += 2
        for _ in range(5):
            r.skip_array(2)
        return r.string(), r.string(), r.string()       # manufacturer, model, version

    def storage(self):
        r = Reader(self.read_op('GetStorageIDs', OP_STORAGE_IDS))
        if r.num('<I') == 0:
            raise Failure('the device presents no storage')
        return r.num('<I')

    def storage_info(self, storage):
        """(access capability, total bytes, free bytes)."""
        r = Reader(self.read_op('GetStorageInfo', OP_STORAGE_INFO, (storage,)))
        r.pos += 4
        return r.num('<H'), r.num('<Q'), r.num('<Q')

    def list_root(self, storage):
        return self.list_children(storage, ROOT)

    def list_children(self, storage, parent):
        r = Reader(self.read_op('GetObjectHandles', OP_HANDLES, (storage, 0, parent)))
        out = []
        for _ in range(r.num('<I')):
            h = r.num('<I')
            out.append(Object(h, self.read_op('GetObjectInfo', OP_OBJECT_INFO, (h,))))
        return out

    def get_object(self, handle):
        return self.read_op('GetObject', OP_GET_OBJECT, (handle,))

    def send_info(self, storage, info, parent=ROOT):
        # MTP 1.1's root; a responder that reads it as a handle gets 0, S-10b.080's other form.
        code, _, _ = self.transaction(OP_SEND_INFO, (storage, parent), info)
        if parent == ROOT and code in (INVALID_HANDLE, INVALID_PARENT):
            code, _, _ = self.transaction(OP_SEND_INFO, (storage, 0), info)
        if code != OK:
            raise Failure('SendObjectInfo answered %s' % response_name(code))

    def send_file(self, storage, name, content, modified):
        self.send_info(storage, object_info_dataset(storage, name, len(content), modified))
        self.checked('SendObject', OP_SEND, (), content)


def response_name(code):
    return '%s (0x%04x)' % (RESPONSES.get(code, 'an unknown code'), code)


# ---------------------------------------------------------------- the commands

def print_listing(objects, access_total_free, out):
    for o in sorted(objects, key=lambda o: o.name):
        print('%12s  %-15s  %s' % ('<dir>' if o.is_folder else o.size, o.modified,
                                   o.name + ('/' if o.is_folder else '')), file=out)
    _, total, free = access_total_free
    print('%d objects, %d bytes free of %d' % (len(objects), free, total), file=out)


def named(objects, name):
    return [o for o in objects if o.name == name]


def compare(mtp, storage, name, content, out):
    """Raises Failure unless the one object of that name on the unit holds exactly `content`."""
    found = named(mtp.list_root(storage), name)
    if not found:
        raise Failure('/yaffs2/%s is not on the unit' % name)
    if len(found) > 1:
        raise Failure('/yaffs2/%s is listed %d times' % (name, len(found)))
    obj = found[0]
    if obj.is_folder:
        raise Failure('/yaffs2/%s is a folder' % name)
    back = mtp.get_object(obj.handle)
    want = hashlib.sha256(content).hexdigest()
    have = hashlib.sha256(back).hexdigest()
    if back != content or obj.size != len(content):
        first = next((i for i, (a, b) in enumerate(zip(content, back)) if a != b),
                     min(len(content), len(back)))
        raise Failure('MISMATCH on /yaffs2/%s: local %d bytes, listed %d, read back %d, first '
                      'difference at offset %d\n  local   sha256 %s\n  on unit sha256 %s'
                      % (name, len(content), obj.size, len(back), first, want, have))
    print('verified /yaffs2/%s: %d bytes read back, identical, sha256 %s'
          % (name, len(back), have), file=out)


def check_name(name):
    if not name or '/' in name or name in ('.', '..') or len(name.encode('utf-16-le')) > 2 * 254:
        raise Failure('%r is not a name for the top of the volume' % name)


def split_path(path):
    """A path in the volume as its names, each one checked."""
    parts = path.strip('/').split('/')
    for name in parts:
        if not name or name in ('.', '..') or len(name.encode('utf-16-le')) > 2 * 254 or \
                len(name.encode('utf-8')) > 255:
            raise Failure('%r is not a path in the volume' % path)
    return parts


def resolve(mtp, storage, parts):
    """The object a path names, or None when it is not there."""
    parent, obj = ROOT, None
    for i, name in enumerate(parts):
        found = named(mtp.list_children(storage, parent), name)
        if not found:
            return None
        if len(found) > 1:
            raise Failure('/yaffs2/%s is listed %d times' % ('/'.join(parts[:i + 1]), len(found)))
        obj = found[0]
        if i + 1 < len(parts) and not obj.is_folder:
            raise Failure('/yaffs2/%s is not a folder' % '/'.join(parts[:i + 1]))
        parent = obj.handle
    return obj


def folder_handle(mtp, storage, parts):
    """The handle of the folder a path names, ROOT for the top; Failure when it is not a folder."""
    if not parts:
        return ROOT
    obj = resolve(mtp, storage, parts)
    if obj is None or not obj.is_folder:
        raise Failure('/yaffs2/%s is not a folder on the unit: nothing was changed' % '/'.join(parts))
    return obj.handle


def require_writable(mtp, storage):
    access, total, _ = mtp.storage_info(storage)
    if total == 0:
        raise Failure('the unit reports no volume (S-10b.031)')
    if access != 0:
        raise Failure('this responder is read-only (AccessCapability %d, S-10b.021): nothing was changed'
                      % access)


def begin(mtp, out):
    mtp.drain()
    manufacturer, model, version = mtp.device_info()
    print('%s %s, bootloader %s' % (manufacturer, model, version), file=out)
    mtp.open_session()
    return mtp.storage()


def cmd_ls(mtp, out):
    storage = begin(mtp, out)
    print_listing(mtp.list_root(storage), mtp.storage_info(storage), out)
    mtp.close_session()


def cmd_verify(mtp, name, content, out):
    storage = begin(mtp, out)
    compare(mtp, storage, name, content, out)
    mtp.close_session()


def cmd_put(mtp, path, name, content, out):
    if not content:
        raise Failure('%s is empty' % path)
    if len(content) >= 1 << 32:
        raise Failure('%s is too large for MTP\'s 32-bit object size' % path)
    modified = datetime.datetime.fromtimestamp(os.path.getmtime(path), datetime.timezone.utc)

    storage = begin(mtp, out)
    objects = mtp.list_root(storage)
    space = mtp.storage_info(storage)
    print_listing(objects, space, out)
    access, total, free = space
    old = named(objects, name)
    if total == 0:
        raise Failure('the unit reports no volume (S-10b.031)')
    if access != 0:
        raise Failure('this responder is read-only (AccessCapability %d, S-10b.021): nothing was changed'
                      % access)
    if any(o.is_folder for o in old):
        raise Failure('/yaffs2/%s is a folder, and a folder delete is recursive: nothing was '
                      'changed' % name)
    if len(content) > free + sum(o.size for o in old):
        raise Failure('%d bytes do not fit in %d free: nothing was changed' % (len(content), free))

    changed = False
    try:
        for o in old:
            changed = True
            mtp.checked('DeleteObject', OP_DELETE, (o.handle, 0))
            print('deleted /yaffs2/%s (%d bytes)' % (name, o.size), file=out)
        if old and named(mtp.list_root(storage), name):
            raise Failure('/yaffs2/%s is still listed after its delete' % name)

        changed = True
        print('uploading %s as /yaffs2/%s, %d bytes' % (path, name, len(content)), file=out)
        mtp.send_file(storage, name, content, modified.strftime('%Y%m%dT%H%M%S'))

        # A session of its own, so that the read-back starts from a new listing.
        mtp.close_session()
        mtp.open_session()
        compare(mtp, storage, name, content, out)
    except Failure as e:
        if changed:
            raise Failure('%s\n/yaffs2/%s is not a good copy now: do NOT run the OS write. A failed '
                          'upload may leave an empty or short file.' % (e, name))
        raise
    print_listing(mtp.list_root(storage), mtp.storage_info(storage), out)
    mtp.close_session()


TREE_MAX_OBJECTS, TREE_MAX_DEPTH = 100000, 32


def walk_tree(mtp, storage, hash_files, out):
    """[{path, folder, size, modified, sha256}] of every object, folders first by name."""
    entries, todo, seen = [], [(ROOT, '', 0)], set()
    while todo:
        parent, prefix, depth = todo.pop()
        for o in sorted(mtp.list_children(storage, parent), key=lambda o: o.name):
            if o.handle in seen or len(entries) >= TREE_MAX_OBJECTS:
                raise Failure('object %#x listed twice, or too many objects' % o.handle)
            seen.add(o.handle)
            path = prefix + o.name
            e = {'path': path, 'folder': o.is_folder, 'size': o.size, 'modified': o.modified}
            if o.is_folder:
                if depth + 1 >= TREE_MAX_DEPTH:
                    raise Failure('%s is too deep' % path)
                todo.append((o.handle, path + '/', depth + 1))
            elif hash_files:
                data = mtp.get_object(o.handle)
                e['sha256'] = hashlib.sha256(data).hexdigest()
                e['read'] = len(data)
            entries.append(e)
            if hash_files and len(entries) % 50 == 0:
                print('%d objects' % len(entries), file=out)
    return sorted(entries, key=lambda e: e['path'])


def cmd_tree(mtp, path, hash_files, out):
    storage = begin(mtp, out)
    entries = walk_tree(mtp, storage, hash_files, out)
    access, total, free = mtp.storage_info(storage)
    mtp.close_session()
    with open(path, 'w') as f:
        json.dump({'entries': entries, 'total': total, 'free': free}, f, indent=0)
    print('%d objects, %d files, %d bytes in files, %d bytes free of %d' % (
        len(entries), sum(1 for e in entries if not e['folder']),
        sum(e['size'] for e in entries if not e['folder']), free, total), file=out)


def compare_trees(a, b):
    """Lines naming each path only in a, only in b, or different between them."""
    ea = {e['path']: e for e in a['entries']}
    eb = {e['path']: e for e in b['entries']}
    lines = []
    for p in sorted(set(ea) | set(eb)):
        x, y = ea.get(p), eb.get(p)
        if y is None:
            lines.append('only in A: %s' % p)
        elif x is None:
            lines.append('only in B: %s' % p)
        elif x['folder'] != y['folder']:
            lines.append('folder in one only: %s' % p)
        elif not x['folder']:
            why = [k for k in ('size', 'sha256', 'modified') if k in x and k in y and x[k] != y[k]]
            if why:
                lines.append('differs (%s): %s' % (', '.join(why), p))
    return lines


def cmd_rm(mtp, name, out):
    storage = begin(mtp, out)
    parts = split_path(name)
    parent = folder_handle(mtp, storage, parts[:-1])
    found = named(mtp.list_children(storage, parent), parts[-1])
    if not found:
        raise Failure('/yaffs2/%s is not on the unit' % name)
    if any(o.is_folder for o in found):
        raise Failure('/yaffs2/%s is a folder, and a folder delete is recursive: nothing was '
                      'changed' % name)
    require_writable(mtp, storage)
    for o in found:
        mtp.checked('DeleteObject', OP_DELETE, (o.handle, 0))
        print('deleted /yaffs2/%s (%d bytes)' % (name, o.size), file=out)
    mtp.close_session()
    mtp.open_session()
    if resolve(mtp, storage, parts) is not None:
        raise Failure('/yaffs2/%s is still listed after its delete' % name)
    print_listing(mtp.list_root(storage), mtp.storage_info(storage), out)
    mtp.close_session()


def cmd_mkdir(mtp, path, out):
    storage = begin(mtp, out)
    parts = split_path(path)
    parent = folder_handle(mtp, storage, parts[:-1])
    if named(mtp.list_children(storage, parent), parts[-1]):
        raise Failure('/yaffs2/%s is already on the unit: nothing was changed' % path)
    require_writable(mtp, storage)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S')
    mtp.send_info(storage, object_info_dataset(storage, parts[-1], 0, stamp, folder=True), parent)
    mtp.close_session()
    mtp.open_session()
    made = resolve(mtp, storage, parts)
    if made is None or not made.is_folder:
        raise Failure('/yaffs2/%s is not listed as a folder after it was made' % path)
    print('made /yaffs2/%s/' % path, file=out)
    mtp.close_session()


def cmd_mv(mtp, src, dst, out):
    """Moves with MoveObject when the folder changes, renames with SetObjectPropValue when the name does."""
    storage = begin(mtp, out)
    sp, dp = split_path(src), split_path(dst)
    obj = resolve(mtp, storage, sp)
    if obj is None:
        raise Failure('/yaffs2/%s is not on the unit' % src)
    there = resolve(mtp, storage, dp)
    if there is not None and there.is_folder and there.handle != obj.handle:
        dp = dp + [sp[-1]]                      # into that folder
        there = resolve(mtp, storage, dp)
    if there is not None:
        raise Failure('/yaffs2/%s is already on the unit: nothing was changed' % '/'.join(dp))
    if dp[:len(sp)] == sp:
        raise Failure('/yaffs2/%s cannot go beneath itself: nothing was changed' % src)
    parent = folder_handle(mtp, storage, dp[:-1])
    require_writable(mtp, storage)
    if dp[:-1] != sp[:-1]:
        if dp[-1] != sp[-1] and named(mtp.list_children(storage, parent), sp[-1]):
            raise Failure('/yaffs2/%s is already on the unit: nothing was changed'
                          % '/'.join(dp[:-1] + [sp[-1]]))
        mtp.checked('MoveObject', OP_MOVE, (obj.handle, storage, parent))
    if dp[-1] != sp[-1]:
        mtp.checked('SetObjectPropValue', OP_SET_PROP, (obj.handle, PROP_FILE_NAME), mtp_string(dp[-1]))
    mtp.close_session()
    mtp.open_session()
    moved = resolve(mtp, storage, dp)
    if moved is None or moved.is_folder != obj.is_folder or moved.size != obj.size:
        raise Failure('/yaffs2/%s is not listed as it should be after the move' % dst)
    if resolve(mtp, storage, sp) is not None:
        raise Failure('/yaffs2/%s is still listed after the move' % src)
    print('moved /yaffs2/%s to /yaffs2/%s' % (src, '/'.join(dp)), file=out)
    mtp.close_session()


def load(a):
    """The local file, read and checked before the unit is touched; None for ls."""
    if a.command in ('ls', 'tree'):
        return None
    if a.command == 'rm':
        split_path(a.name)
        if a.name.strip('/') == 'os.ldr':
            raise Failure('os.ldr is replaced with put, not removed')
        return None
    if a.command == 'mkdir':
        split_path(a.path)
        return None
    if a.command == 'mv':
        split_path(a.src)
        split_path(a.dst)
        return None
    check_name(a.name)
    with open(a.file, 'rb') as f:
        return f.read()


def run(a, content, mtp, out):
    if a.command == 'ls':
        cmd_ls(mtp, out)
    elif a.command == 'tree':
        cmd_tree(mtp, a.out, a.hash, out)
    elif a.command == 'rm':
        cmd_rm(mtp, a.name, out)
    elif a.command == 'mkdir':
        cmd_mkdir(mtp, a.path, out)
    elif a.command == 'mv':
        cmd_mv(mtp, a.src, a.dst, out)
    elif a.command == 'verify':
        cmd_verify(mtp, a.name, content, out)
    else:
        cmd_put(mtp, a.file, a.name, content, out)
    if mtp.stale:
        print('note: dropped %d data block(s) left from earlier transactions (S-16.064)' % mtp.stale,
              file=out)


def main(argv=None):
    ap = argparse.ArgumentParser(description='Copy a file to the unit\'s /yaffs2 over MTP and read it back.')
    sub = ap.add_subparsers(dest='command', required=True)
    sub.add_parser('ls', help='list the top of the volume and the free space')
    p = sub.add_parser('tree', help='list the whole volume into a JSON file (read only)')
    p.add_argument('out')
    p.add_argument('--hash', action='store_true', help='read every file and record its SHA-256')
    p = sub.add_parser('rm', help='delete one file')
    p.add_argument('name')
    p = sub.add_parser('mkdir', help='make one folder')
    p.add_argument('path')
    p = sub.add_parser('mv', help='rename or move a file or folder')
    p.add_argument('src')
    p.add_argument('dst')
    p = sub.add_parser('compare', help='compare two tree listings (no unit needed)')
    p.add_argument('a')
    p.add_argument('b')
    for command, text in (('put', 'replace a file on the unit and read it back'),
                          ('verify', 'read a file on the unit back and compare it')):
        p = sub.add_parser(command, help=text)
        p.add_argument('file')
        p.add_argument('--as', dest='name', default='os.ldr',
                       help='its name at the top of /yaffs2 (default os.ldr)')
    a = ap.parse_args(argv)
    if a.command == 'compare':
        with open(a.a) as fa, open(a.b) as fb:
            lines = compare_trees(json.load(fa), json.load(fb))
        print('\n'.join(lines) if lines else 'no differences')
        return 1 if lines else 0
    transport = None
    try:
        content = load(a)
        transport = UsbTransport()
        run(a, content, Mtp(transport), sys.stdout)
    except Timeout as e:
        print('FAILED: %s\nThe unit did not end that transaction (S-16.179): turn MTP off and '
              'on, or power-cycle.' % e, file=sys.stderr)
        return 1
    except (Failure, OSError) as e:
        print('FAILED: %s' % e, file=sys.stderr)
        return 1
    finally:
        if transport is not None:
            transport.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
