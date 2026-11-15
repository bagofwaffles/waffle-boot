# SPDX-License-Identifier: GPL-2.0-or-later
"""MTP 1.1 byte models: code tables, datatypes, the USB container and the datasets.

decode_* return (value, findings) and raise MalformedError on bytes that cannot be walked; fields
absent at a clean end decode as None. encode_* are the inverses: encode(decode(b)) == b.
"""
import re
import struct
from collections import namedtuple

Finding = namedtuple('Finding', 'rule where msg')


class MalformedError(ValueError):
    pass


# ------------------------------------------------------------------------------ code tables
OPERATIONS = {
    0x1001: 'GetDeviceInfo', 0x1002: 'OpenSession', 0x1003: 'CloseSession', 0x1004: 'GetStorageIDs',
    0x1005: 'GetStorageInfo', 0x1006: 'GetNumObjects', 0x1007: 'GetObjectHandles', 0x1008: 'GetObjectInfo',
    0x1009: 'GetObject', 0x100A: 'GetThumb', 0x100B: 'DeleteObject', 0x100C: 'SendObjectInfo',
    0x100D: 'SendObject', 0x100E: 'InitiateCapture', 0x100F: 'FormatStore', 0x1010: 'ResetDevice',
    0x1011: 'SelfTest', 0x1012: 'SetObjectProtection', 0x1013: 'PowerDown', 0x1014: 'GetDevicePropDesc',
    0x1015: 'GetDevicePropValue', 0x1016: 'SetDevicePropValue', 0x1017: 'ResetDevicePropValue',
    0x1018: 'TerminateOpenCapture', 0x1019: 'MoveObject', 0x101A: 'CopyObject', 0x101B: 'GetPartialObject',
    0x101C: 'InitiateOpenCapture',
    0x9801: 'GetObjectPropsSupported', 0x9802: 'GetObjectPropDesc', 0x9803: 'GetObjectPropValue',
    0x9804: 'SetObjectPropValue', 0x9805: 'GetObjectPropList', 0x9806: 'SetObjectPropList',
    0x9807: 'GetInterdependentPropDesc', 0x9808: 'SendObjectPropList', 0x9810: 'GetObjectReferences',
    0x9811: 'SetObjectReferences', 0x9812: 'Skip',
}
RESPONSES = {
    0x2001: 'OK', 0x2002: 'General_Error', 0x2003: 'Session_Not_Open', 0x2004: 'Invalid_TransactionID',
    0x2005: 'Operation_Not_Supported', 0x2006: 'Parameter_Not_Supported', 0x2007: 'Incomplete_Transfer',
    0x2008: 'Invalid_StorageID', 0x2009: 'Invalid_ObjectHandle', 0x200A: 'DeviceProp_Not_Supported',
    0x200B: 'Invalid_ObjectFormatCode', 0x200C: 'Store_Full', 0x200D: 'Object_WriteProtected',
    0x200E: 'Store_Read_Only', 0x200F: 'Access_Denied', 0x2010: 'No_Thumbnail_Present', 0x2011: 'SelfTest_Failed',
    0x2012: 'Partial_Deletion', 0x2013: 'Store_Not_Available', 0x2014: 'Specification_By_Format_Unsupported',
    0x2015: 'No_Valid_ObjectInfo', 0x2016: 'Invalid_Code_Format', 0x2017: 'Unknown_Vendor_Code',
    0x2018: 'Capture_Already_Terminated', 0x2019: 'Device_Busy', 0x201A: 'Invalid_ParentObject',
    0x201B: 'Invalid_DeviceProp_Format', 0x201C: 'Invalid_DeviceProp_Value', 0x201D: 'Invalid_Parameter',
    0x201E: 'Session_Already_Open', 0x201F: 'Transaction_Cancelled', 0x2020: 'Specification_of_Destination_Unsupported',
    0xA801: 'Invalid_ObjectPropCode', 0xA802: 'Invalid_ObjectProp_Format', 0xA803: 'Invalid_ObjectProp_Value',
    0xA804: 'Invalid_ObjectReference', 0xA805: 'Group_Not_Supported', 0xA806: 'Invalid_Dataset',
    0xA807: 'Specification_By_Group_Unsupported', 0xA808: 'Specification_By_Depth_Unsupported',
    0xA809: 'Object_Too_Large', 0xA80A: 'ObjectProp_Not_Supported',
}
EVENTS = {
    0x4001: 'CancelTransaction', 0x4002: 'ObjectAdded', 0x4003: 'ObjectRemoved', 0x4004: 'StoreAdded',
    0x4005: 'StoreRemoved', 0x4006: 'DevicePropChanged', 0x4007: 'ObjectInfoChanged', 0x4008: 'DeviceInfoChanged',
    0x4009: 'RequestObjectTransfer', 0x400A: 'StoreFull', 0x400B: 'DeviceReset', 0x400C: 'StorageInfoChanged',
    0x400D: 'CaptureComplete', 0x400E: 'UnreportedStatus',
    0xC801: 'ObjectPropChanged', 0xC802: 'ObjectPropDescChanged', 0xC803: 'ObjectReferencesChanged',
}
DEVICE_PROPS = {
    0x5001: 'BatteryLevel', 0x5002: 'FunctionalMode', 0x5003: 'ImageSize', 0x5004: 'CompressionSetting',
    0x5005: 'WhiteBalance', 0x5006: 'RGBGain', 0x5007: 'FNumber', 0x5008: 'FocalLength', 0x5009: 'FocusDistance',
    0x500A: 'FocusMode', 0x500B: 'ExposureMeteringMode', 0x500C: 'FlashMode', 0x500D: 'ExposureTime',
    0x500E: 'ExposureProgramMode', 0x500F: 'ExposureIndex', 0x5010: 'ExposureBiasCompensation', 0x5011: 'DateTime',
    0x5012: 'CaptureDelay', 0x5013: 'StillCaptureMode', 0x5014: 'Contrast', 0x5015: 'Sharpness',
    0x5016: 'DigitalZoom', 0x5017: 'EffectMode', 0x5018: 'BurstNumber', 0x5019: 'BurstInterval',
    0x501A: 'TimelapseNumber', 0x501B: 'TimelapseInterval', 0x501C: 'FocusMeteringMode', 0x501D: 'UploadURL',
    0x501E: 'Artist', 0x501F: 'CopyrightInfo',
    0xD401: 'SynchronizationPartner', 0xD402: 'DeviceFriendlyName', 0xD403: 'Volume',
    0xD404: 'SupportedFormatsOrdered', 0xD405: 'DeviceIcon', 0xD406: 'SessionInitiatorVersionInfo',
    0xD407: 'PerceivedDeviceType', 0xD410: 'PlaybackRate', 0xD411: 'PlaybackObject', 0xD412: 'PlaybackContainerIndex',
    0xD413: 'PlaybackPosition',
}
OBJECT_FORMATS = {
    0x3000: 'Undefined', 0x3001: 'Association', 0x3002: 'Script', 0x3003: 'Executable', 0x3004: 'Text',
    0x3005: 'HTML', 0x3006: 'DPOF', 0x3007: 'AIFF', 0x3008: 'WAV', 0x3009: 'MP3', 0x300A: 'AVI', 0x300B: 'MPEG',
    0x300C: 'ASF', 0x3800: 'Undefined_Image', 0x3801: 'EXIF_JPEG', 0x3802: 'TIFF_EP', 0x3803: 'FlashPix',
    0x3804: 'BMP', 0x3805: 'CIFF', 0x3807: 'GIF', 0x3808: 'JFIF', 0x3809: 'CD', 0x380A: 'PICT', 0x380B: 'PNG',
    0x380D: 'TIFF', 0x380E: 'TIFF_IT', 0x380F: 'JP2', 0x3810: 'JPX',
    0xB802: 'Undefined_Firmware', 0xB881: 'Windows_Image_Format', 0xB900: 'Undefined_Audio', 0xB901: 'WMA',
    0xB902: 'OGG', 0xB903: 'AAC', 0xB904: 'Audible', 0xB906: 'FLAC', 0xB980: 'Undefined_Video', 0xB981: 'WMV',
    0xB982: 'MP4_Container', 0xB983: 'MP2', 0xB984: '3GP_Container', 0xBA00: 'Undefined_Collection',
    0xBA05: 'Abstract_Audio_Album', 0xBA0B: 'Abstract_Audio_Playlist', 0xBA10: 'WPL_Playlist',
    0xBA11: 'M3U_Playlist', 0xBA14: 'PLS_Playlist', 0xBA82: 'XML_Document',
}
OBJECT_PROPS = {
    0xDC01: 'StorageID', 0xDC02: 'ObjectFormat', 0xDC03: 'ProtectionStatus', 0xDC04: 'ObjectSize',
    0xDC05: 'AssociationType', 0xDC06: 'AssociationDesc', 0xDC07: 'ObjectFileName', 0xDC08: 'DateCreated',
    0xDC09: 'DateModified', 0xDC0A: 'Keywords', 0xDC0B: 'ParentObject', 0xDC0C: 'AllowedFolderContents',
    0xDC0D: 'Hidden', 0xDC0E: 'SystemObject', 0xDC41: 'PersistentUniqueObjectIdentifier', 0xDC42: 'SyncID',
    0xDC43: 'PropertyBag', 0xDC44: 'Name', 0xDC45: 'CreatedBy', 0xDC46: 'Artist', 0xDC47: 'DateAuthored',
    0xDC48: 'Description', 0xDC4E: 'DateAdded', 0xDC4F: 'NonConsumable', 0xDC89: 'Duration',
    0xDC8B: 'Track', 0xDC8C: 'Genre', 0xDC9A: 'AlbumName', 0xDC9B: 'AlbumArtist', 0xDCE0: 'DisplayName',
}
STORAGE_TYPES = {0: 'Undefined', 1: 'FixedROM', 2: 'RemovableROM', 3: 'FixedRAM', 4: 'RemovableRAM'}
FILESYSTEM_TYPES = {0: 'Undefined', 1: 'GenericFlat', 2: 'GenericHierarchical', 3: 'DCF'}
ACCESS_CAPS = {0: 'ReadWrite', 1: 'ReadOnly_NoDeletion', 2: 'ReadOnly_WithDeletion'}
CONTAINER_TYPES = {1: 'Command', 2: 'Data', 3: 'Response', 4: 'Event'}
DATATYPES = {0x0001: 'INT8', 0x0002: 'UINT8', 0x0003: 'INT16', 0x0004: 'UINT16', 0x0005: 'INT32', 0x0006: 'UINT32',
             0x0007: 'INT64', 0x0008: 'UINT64', 0x0009: 'INT128', 0x000A: 'UINT128', 0x4001: 'AINT8',
             0x4002: 'AUINT8', 0x4003: 'AINT16', 0x4004: 'AUINT16', 0x4005: 'AINT32', 0x4006: 'AUINT32',
             0x4007: 'AINT64', 0x4008: 'AUINT64', 0x4009: 'AINT128', 0x400A: 'AUINT128', 0xFFFF: 'STR'}
ASSOCIATION_GENERIC_FOLDER = 1
FORMAT_ASSOCIATION = 0x3001
ROOT_PARENT = 0                       # ParentObject of an object in the storage root (5.3.1.4.12)


def is_vendor_extended(code):
    """PTP/MTP codes with bit 15 set are vendor-extended (MTP's own 0x98xx/0xA8xx/0xC8xx/0xDxxx are)."""
    return bool(code & 0x8000)


# ------------------------------------------------------------------------------ datatypes
_INT = {'u8': '<B', 'u16': '<H', 'u32': '<I', 'u64': '<Q', 'i8': '<b', 'i16': '<h', 'i32': '<i', 'i64': '<q'}


class MtpString:
    """An MTP String (3.2.3): count byte (UTF-16 units including the terminating NUL; 0 = empty), the
    units. text: the units before the first NUL. units: all of them, kept for byte-exact re-encoding."""
    __slots__ = ('units',)

    def __init__(self, text_or_units=''):
        if isinstance(text_or_units, str):
            b = text_or_units.encode('utf-16-le', 'surrogatepass')
            u = list(struct.unpack(f'<{len(b) // 2}H', b))
            self.units = u + [0] if u else []
        else:
            self.units = list(text_or_units)

    @property
    def text(self):
        u = self.units[:self.units.index(0)] if 0 in self.units else self.units
        return struct.pack(f'<{len(u)}H', *u).decode('utf-16-le', 'surrogatepass')

    def encode(self):
        if len(self.units) > 255:
            raise ValueError('MTP string longer than 255 units')
        return bytes([len(self.units)]) + struct.pack(f'<{len(self.units)}H', *self.units)

    def findings(self, where):
        out = []
        if self.units and self.units[-1] != 0:
            out.append(Finding('string_nul', where, 'last unit is not NUL'))
        if self.units and 0 in self.units[:-1]:
            out.append(Finding('string_nul', where, 'NUL before the last unit'))
        if self.units == [0]:
            out.append(Finding('string_empty', where, 'empty string sent as one NUL unit (count 0 is canonical)'))
        return out

    def __eq__(self, o):
        return isinstance(o, MtpString) and o.units == self.units or (isinstance(o, str) and o == self.text)

    def __repr__(self):
        return f'MtpString({self.text!r})'


def _get(fmt, b, off, where):
    n = struct.calcsize(fmt)
    if off + n > len(b):
        raise MalformedError(f'{where}: needs {n} bytes at {off}, {len(b) - off} left')
    return struct.unpack_from(fmt, b, off)[0], off + n


def read_string(b, off, where='string'):
    n, off = _get('<B', b, off, where)
    if off + 2 * n > len(b):
        raise MalformedError(f'{where}: {n} units run past the end')
    return MtpString(struct.unpack_from(f'<{n}H', b, off)), off + 2 * n


def read_array(b, off, et, where='array'):
    n, off = _get('<I', b, off, where)
    fmt = _INT[et]
    sz = struct.calcsize(fmt)
    if off + n * sz > len(b):
        raise MalformedError(f'{where}: {n} elements run past the end')
    return list(struct.unpack_from('<' + fmt[1] * n, b, off)), off + n * sz


def write_value(t, v):
    if t == 'str':
        return (v if isinstance(v, MtpString) else MtpString(v)).encode()
    if t.startswith('a'):
        fmt = _INT[t[1:]]
        return struct.pack('<I', len(v)) + struct.pack('<' + fmt[1] * len(v), *v)
    if t == 'u128':
        return int(v).to_bytes(16, 'little')
    return struct.pack(_INT[t], v)


def read_value(t, b, off, where):
    if t == 'str':
        return read_string(b, off, where)
    if t.startswith('a'):
        return read_array(b, off, t[1:], where)
    if t == 'u128':
        if off + 16 > len(b):
            raise MalformedError(f'{where}: UINT128 runs past the end')
        return int.from_bytes(b[off:off + 16], 'little'), off + 16
    return _get(_INT[t], b, off, where)


DATETIME_RE = re.compile(r'^(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})(\.\d)?(Z|[+-]\d{4})?$')


def check_datetime(s, where='DateTime'):
    """MTP 1.1 5.3.1.4 / PTP 5.3.5: "YYYYMMDDThhmmss[.s][Z|+hhmm|-hhmm]"; empty means unknown."""
    if s == '':
        return []
    m = DATETIME_RE.match(s)
    if not m:
        return [Finding('datetime_format', where, f'{s!r}')]
    y, mo, d, h, mi, se = (int(x) for x in m.groups()[:6])
    if not (1 <= mo <= 12 and 1 <= d <= 31 and h <= 23 and mi <= 59 and se <= 59):
        return [Finding('datetime_range', where, f'{s!r}')]
    return []


# ------------------------------------------------------------------------------ datasets
DEVICE_INFO = [('StandardVersion', 'u16'), ('VendorExtensionID', 'u32'), ('VendorExtensionVersion', 'u16'),
               ('VendorExtensionDesc', 'str'), ('FunctionalMode', 'u16'), ('OperationsSupported', 'au16'),
               ('EventsSupported', 'au16'), ('DevicePropertiesSupported', 'au16'), ('CaptureFormats', 'au16'),
               ('PlaybackFormats', 'au16'), ('Manufacturer', 'str'), ('Model', 'str'), ('DeviceVersion', 'str'),
               ('SerialNumber', 'str')]
STORAGE_INFO = [('StorageType', 'u16'), ('FilesystemType', 'u16'), ('AccessCapability', 'u16'),
                ('MaxCapacity', 'u64'), ('FreeSpaceInBytes', 'u64'), ('FreeSpaceInObjects', 'u32'),
                ('StorageDescription', 'str'), ('VolumeIdentifier', 'str')]
OBJECT_INFO = [('StorageID', 'u32'), ('ObjectFormat', 'u16'), ('ProtectionStatus', 'u16'),
               ('ObjectCompressedSize', 'u32'), ('ThumbFormat', 'u16'), ('ThumbCompressedSize', 'u32'),
               ('ThumbPixWidth', 'u32'), ('ThumbPixHeight', 'u32'), ('ImagePixWidth', 'u32'),
               ('ImagePixHeight', 'u32'), ('ImageBitDepth', 'u32'), ('ParentObject', 'u32'),
               ('AssociationType', 'u16'), ('AssociationDesc', 'u32'), ('SequenceNumber', 'u32'),
               ('Filename', 'str'), ('DateCreated', 'str'), ('DateModified', 'str'), ('Keywords', 'str')]
HANDLE_ARRAY = [('Handles', 'au32')]
STORAGE_IDS = [('StorageIDs', 'au32')]


def decode_dataset(layout, b, name='dataset'):
    b = bytes(b)
    out, off, fnd = {}, 0, []
    for i, (k, t) in enumerate(layout):
        if off == len(b) and i > 0:
            for k2, _ in layout[i:]:
                out[k2] = None
            fnd.append(Finding('missing_fields', name, f'ends before {k} ({len(layout) - i} fields absent)'))
            return out, fnd
        out[k], off = read_value(t, b, off, f'{name}.{k}')
        if t == 'str':
            fnd += out[k].findings(f'{name}.{k}')
    if off != len(b):
        fnd.append(Finding('trailing_bytes', name, f'{len(b) - off} bytes after the last field'))
        out['_trailing'] = b[off:]
    return out, fnd


def encode_dataset(layout, values):
    out = bytearray()
    for k, t in layout:
        v = values.get(k)
        if v is None:
            if any(values.get(k2) is not None for k2, _ in layout[layout.index((k, t)):]):
                raise ValueError(f'{k} absent but a later field is present')
            break
        out += write_value(t, v)
    return bytes(out) + values.get('_trailing', b'')


def decode_device_info(b):
    v, f = decode_dataset(DEVICE_INFO, b, 'DeviceInfo')
    if v.get('StandardVersion') is not None and v['StandardVersion'] != 100:
        f.append(Finding('standard_version', 'DeviceInfo', f'StandardVersion {v["StandardVersion"]} (MTP 1.x: 100)'))
    for k, table in (('OperationsSupported', OPERATIONS), ('EventsSupported', EVENTS),
                     ('DevicePropertiesSupported', DEVICE_PROPS), ('CaptureFormats', OBJECT_FORMATS),
                     ('PlaybackFormats', OBJECT_FORMATS)):
        xs = v.get(k) or []
        if len(set(xs)) != len(xs):
            f.append(Finding('duplicate_code', f'DeviceInfo.{k}', 'a code listed twice'))
        for c in xs:
            if c not in table and not c & 0x8000:
                f.append(Finding('unknown_code', f'DeviceInfo.{k}', f'{c:#06x}'))
    ops = v.get('OperationsSupported') or []
    for need in (0x1001, 0x1002, 0x1003):
        if v.get('OperationsSupported') is not None and need not in ops:
            f.append(Finding('required_operation', 'DeviceInfo', f'{OPERATIONS[need]} not listed'))
    return v, f


def encode_device_info(**v):
    return encode_dataset(DEVICE_INFO, v)


def decode_storage_info(b):
    v, f = decode_dataset(STORAGE_INFO, b, 'StorageInfo')
    if v.get('StorageType') is not None and v['StorageType'] not in STORAGE_TYPES:
        f.append(Finding('storage_type', 'StorageInfo', f'{v["StorageType"]:#06x}'))
    if v.get('FilesystemType') is not None and v['FilesystemType'] not in FILESYSTEM_TYPES and not v['FilesystemType'] & 0x8000:
        f.append(Finding('filesystem_type', 'StorageInfo', f'{v["FilesystemType"]:#06x}'))
    if v.get('AccessCapability') is not None and v['AccessCapability'] not in ACCESS_CAPS:
        f.append(Finding('access_capability', 'StorageInfo', f'{v["AccessCapability"]:#06x}'))
    if v.get('FreeSpaceInBytes') is not None and v['FreeSpaceInBytes'] > v['MaxCapacity']:
        f.append(Finding('free_space', 'StorageInfo', 'FreeSpaceInBytes > MaxCapacity'))
    return v, f


def encode_storage_info(**v):
    return encode_dataset(STORAGE_INFO, v)


def decode_object_info(b):
    v, f = decode_dataset(OBJECT_INFO, b, 'ObjectInfo')
    fmt = v.get('ObjectFormat')
    if fmt is not None and fmt not in OBJECT_FORMATS and not fmt & 0x8000:
        f.append(Finding('unknown_code', 'ObjectInfo.ObjectFormat', f'{fmt:#06x}'))
    if v.get('ProtectionStatus') is not None and v['ProtectionStatus'] not in (0, 1, 0x8002, 0x8003):
        f.append(Finding('protection_status', 'ObjectInfo', f'{v["ProtectionStatus"]:#06x}'))
    if v.get('ParentObject') == 0xFFFFFFFF:
        f.append(Finding('parent_object', 'ObjectInfo', 'ParentObject 0xFFFFFFFF (root is 0)'))
    if fmt == FORMAT_ASSOCIATION and v.get('AssociationType') not in (None, ASSOCIATION_GENERIC_FOLDER):
        f.append(Finding('association_type', 'ObjectInfo', 'folder with AssociationType other than GenericFolder'))
    if v.get('StorageID') is not None:
        f += check_storage_id(v['StorageID'], 'ObjectInfo.StorageID')
    for k in ('DateCreated', 'DateModified'):
        if v.get(k) is not None:
            f += check_datetime(v[k].text, f'ObjectInfo.{k}')
    if v.get('Filename') is not None and v['Filename'].text == '':
        f.append(Finding('filename', 'ObjectInfo', 'empty Filename'))
    return v, f


def encode_object_info(**v):
    return encode_dataset(OBJECT_INFO, v)


def check_storage_id(sid, where='StorageID'):
    """5.2.1: upper 16 bits the physical store, lower 16 the logical one; 0x0000 low = not usable."""
    out = []
    if sid in (0, 0xFFFFFFFF):
        out.append(Finding('storage_id', where, f'{sid:#010x} is reserved'))
    elif sid & 0xFFFF == 0:
        out.append(Finding('storage_id', where, f'{sid:#010x}: logical part 0 (no usable logical storage)'))
    return out


def decode_handles(b, name='ObjectHandles'):
    v, f = decode_dataset(HANDLE_ARRAY, b, name)
    hs = v['Handles'] or []
    if len(set(hs)) != len(hs):
        f.append(Finding('duplicate_handle', name, 'a handle listed twice'))
    for x in hs:
        if x in (0, 0xFFFFFFFF):
            f.append(Finding('reserved_handle', name, f'{x:#010x}'))
    return hs, f


def encode_handles(hs):
    return write_value('au32', hs)


def decode_storage_ids(b):
    v, f = decode_dataset(STORAGE_IDS, b, 'StorageIDs')
    ids = v['StorageIDs'] or []
    for s in ids:
        f += check_storage_id(s, 'StorageIDs')
    if len(set(ids)) != len(ids):
        f.append(Finding('duplicate_storage', 'StorageIDs', 'an ID listed twice'))
    return ids, f


# ------------------------------------------------------------------------------ container
Container = namedtuple('Container', 'length type code transaction_id payload')


def encode_container(ctype, code, tid, payload=b'', params=None):
    if params is not None:
        if len(params) > 5:
            raise ValueError('at most 5 parameters')
        payload = struct.pack(f'<{len(params)}I', *params)
    return struct.pack('<IHHI', 12 + len(payload), ctype, code, tid) + bytes(payload)


def decode_container(b, name='container'):
    """The 12-byte header and payload. Findings: container_length (dwLength != bytes given),
    container_type, and for command/response/event a payload that is not 0..5 UINT32 parameters."""
    b = bytes(b)
    if len(b) < 12:
        raise MalformedError(f'{name}: {len(b)} bytes, header is 12')
    ln, ty, code, tid = struct.unpack_from('<IHHI', b, 0)
    c = Container(ln, ty, code, tid, b[12:])
    f = []
    if ln < 12:
        raise MalformedError(f'{name}: dwLength {ln} < 12')
    if ln != len(b):
        f.append(Finding('container_length', name, f'dwLength {ln}, {len(b)} bytes'))
    if ty not in CONTAINER_TYPES:
        raise MalformedError(f'{name}: container type {ty}')
    if ty in (1, 3, 4):
        if len(c.payload) % 4 or len(c.payload) > 20:
            f.append(Finding('container_params', name, f'{len(c.payload)} payload bytes'))
    if ty == 3 and code not in RESPONSES and not code & 0x8000:
        f.append(Finding('unknown_code', name, f'response code {code:#06x}'))
    if ty == 1 and code not in OPERATIONS and not code & 0x8000:
        f.append(Finding('unknown_code', name, f'operation code {code:#06x}'))
    if ty == 4 and code not in EVENTS and not code & 0x8000:
        f.append(Finding('unknown_code', name, f'event code {code:#06x}'))
    return c, f


def params_of(c):
    n = len(c.payload) // 4
    return list(struct.unpack_from(f'<{n}I', c.payload))


def decode_data_reply(b, op_code, layout_decoder, tid=None):
    """A data-phase container carrying a dataset: (container, dataset value, findings)."""
    c, f = decode_container(b, 'data')
    if c.type != 2:
        f.append(Finding('container_type', 'data', f'type {c.type}, data phase needs 2'))
    if c.code != op_code:
        f.append(Finding('container_code', 'data', f'code {c.code:#06x}, operation {op_code:#06x}'))
    if tid is not None and c.transaction_id != tid:
        f.append(Finding('transaction_id', 'data', f'{c.transaction_id} != {tid}'))
    v, f2 = layout_decoder(c.payload)
    return c, v, f + f2


def check_response(b, tid=None, want_code=None, nparams=None):
    c, f = decode_container(b, 'response')
    if c.type != 3:
        f.append(Finding('container_type', 'response', f'type {c.type}'))
    if tid is not None and c.transaction_id != tid:
        f.append(Finding('transaction_id', 'response', f'{c.transaction_id} != {tid}'))
    if want_code is not None and c.code != want_code:
        f.append(Finding('response_code', 'response', f'{c.code:#06x} != {want_code:#06x}'))
    if nparams is not None and len(c.payload) != 4 * nparams:
        f.append(Finding('container_params', 'response', f'{len(c.payload) // 4} params, want {nparams}'))
    return c, f


# ------------------------------------------------------------------------------ typed values (3.2)
DATATYPE_TAGS = {0x0001: 'i8', 0x0002: 'u8', 0x0003: 'i16', 0x0004: 'u16', 0x0005: 'i32', 0x0006: 'u32',
                 0x0007: 'i64', 0x0008: 'u64', 0x0009: 'i128', 0x000A: 'u128', 0xFFFF: 'str'}
DATATYPE_TAGS.update({0x4000 | k: 'a' + v for k, v in DATATYPE_TAGS.items() if k < 0x4000})
DT_STR, DT_UINT8, DT_UINT16, DT_UINT32, DT_UINT64, DT_UINT128 = 0xFFFF, 0x0002, 0x0004, 0x0006, 0x0008, 0x000A


def _read_int128(t, b, off, where):
    if off + 16 > len(b):
        raise MalformedError(f'{where}: 128-bit value runs past the end')
    return int.from_bytes(b[off:off + 16], 'little', signed=t == 'i128'), off + 16


def read_typed(datatype, b, off, where='value'):
    """One value of an MTP datatype code (3.2.1). An unknown or undefined code cannot be walked."""
    t = DATATYPE_TAGS.get(datatype)
    if t is None:
        raise MalformedError(f'{where}: datatype {datatype:#06x}')
    if t in ('i128', 'u128'):
        return _read_int128(t, b, off, where)
    if t in ('ai128', 'au128'):
        n, off = _get('<I', b, off, where)
        if off + 16 * n > len(b):
            raise MalformedError(f'{where}: {n} elements run past the end')
        return [_read_int128(t[1:], b, off + 16 * i, where)[0] for i in range(n)], off + 16 * n
    return read_value(t, b, off, where)


def write_typed(datatype, v):
    t = DATATYPE_TAGS[datatype]
    if t in ('i128', 'u128'):
        return int(v).to_bytes(16, 'little', signed=t == 'i128')
    if t in ('ai128', 'au128'):
        return struct.pack('<I', len(v)) + b''.join(int(x).to_bytes(16, 'little', signed=t == 'ai128') for x in v)
    return write_value(t, v)


def _typed_findings(datatype, v, where):
    return v.findings(where) if datatype == DT_STR and isinstance(v, MtpString) else []


# Datatypes the specifications fix for a property (PTP 13, MTP 1.1 5.1.2 and 5.3.2).
DEVICE_PROP_TYPES = {0x5001: DT_UINT8, 0x5002: DT_UINT16, 0x5011: DT_STR, 0x501E: DT_STR, 0x501F: DT_STR,
                     0xD401: DT_STR, 0xD402: DT_STR, 0xD406: DT_STR, 0xD407: DT_UINT32}
OBJECT_PROP_TYPES = {0xDC01: DT_UINT32, 0xDC02: DT_UINT16, 0xDC03: DT_UINT16, 0xDC04: DT_UINT64,
                     0xDC05: DT_UINT16, 0xDC06: DT_UINT32, 0xDC07: DT_STR, 0xDC08: DT_STR, 0xDC09: DT_STR,
                     0xDC0A: DT_STR, 0xDC0B: DT_UINT32, 0xDC41: DT_UINT128, 0xDC44: DT_STR, 0xDCE0: DT_STR}
DATE_PROPS = {0x5011, 0xDC08, 0xDC09, 0xDC47, 0xDC4E}

FORM_NONE, FORM_RANGE, FORM_ENUM = 0x00, 0x01, 0x02


# ------------------------------------------------------------------------------ DevicePropDesc (5.1.2.1)
def decode_device_prop_desc(b):
    """DevicePropDesc dataset: (dict, findings). Keys: DevicePropertyCode, DataType, GetSet,
    FactoryDefaultValue, CurrentValue, FormFlag, Form (None; {'min','max','step'}; {'values'}).
    Structural: a field cut short or an unknown datatype raises MalformedError. Findings:
    unknown_code, prop_datatype, getset, form_flag (not 0/1/2; the rest is kept as '_form_raw'),
    form_datatype (a range over a string or array), form_range, form_enum, value_in_form,
    trailing_bytes, datetime_*, string_*."""
    b = bytes(b)
    name = 'DevicePropDesc'
    v, f = {}, []
    off = 0
    v['DevicePropertyCode'], off = _get('<H', b, off, f'{name}.DevicePropertyCode')
    v['DataType'], off = _get('<H', b, off, f'{name}.DataType')
    v['GetSet'], off = _get('<B', b, off, f'{name}.GetSet')
    code, dt = v['DevicePropertyCode'], v['DataType']
    for k in ('FactoryDefaultValue', 'CurrentValue'):
        v[k], off = read_typed(dt, b, off, f'{name}.{k}')
        f += _typed_findings(dt, v[k], f'{name}.{k}')
    v['FormFlag'], off = _get('<B', b, off, f'{name}.FormFlag')
    v['Form'] = None
    if code not in DEVICE_PROPS and not code & 0x8000:
        f.append(Finding('unknown_code', name, f'property {code:#06x}'))
    if code in DEVICE_PROP_TYPES and DEVICE_PROP_TYPES[code] != dt:
        f.append(Finding('prop_datatype', name, f'{DEVICE_PROPS.get(code, code)}: datatype {dt:#06x}'))
    if v['GetSet'] not in (0, 1):
        f.append(Finding('getset', name, f'Get/Set {v["GetSet"]}'))
    ff = v['FormFlag']
    if ff == FORM_RANGE:
        mn, off = read_typed(dt, b, off, f'{name}.Form.min')
        mx, off = read_typed(dt, b, off, f'{name}.Form.max')
        st, off = read_typed(dt, b, off, f'{name}.Form.step')
        v['Form'] = {'min': mn, 'max': mx, 'step': st}
    elif ff == FORM_ENUM:
        n, off = _get('<H', b, off, f'{name}.Form.count')
        vals = []
        for i in range(n):
            x, off = read_typed(dt, b, off, f'{name}.Form.values[{i}]')
            vals.append(x)
        v['Form'] = {'values': vals}
    elif ff != FORM_NONE:
        f.append(Finding('form_flag', name, f'form flag {ff:#04x}'))
        v['_form_raw'] = b[off:]
        off = len(b)
    f += _check_form(dt, v, name)
    if code in DATE_PROPS and dt == DT_STR:
        for k in ('FactoryDefaultValue', 'CurrentValue'):
            f += check_datetime(v[k].text, f'{name}.{k}')
    if off != len(b):
        f.append(Finding('trailing_bytes', name, f'{len(b) - off} bytes after the dataset'))
        v['_trailing'] = b[off:]
    return v, f


def _check_form(dt, v, name):
    form, out = v.get('Form'), []
    vals = [v.get('FactoryDefaultValue'), v.get('CurrentValue')]
    if form is None:
        return out
    if 'min' in form:
        if not isinstance(form['min'], int):
            return [Finding('form_datatype', name, 'range form over a non-integer datatype')]
        mn, mx, st = form['min'], form['max'], form['step']
        if mn > mx or (st <= 0 and mn != mx):
            out.append(Finding('form_range', name, f'min {mn} max {mx} step {st}'))
            return out
        for x in vals:
            if not mn <= x <= mx or (st > 0 and (x - mn) % st):
                out.append(Finding('value_in_form', name, f'{x} outside the range'))
    else:
        if not form['values']:
            out.append(Finding('form_enum', name, 'enumeration with no values'))
            return out
        for x in vals:
            if x not in form['values']:
                out.append(Finding('value_in_form', name, f'{x!r} not in the enumeration'))
    return out


def encode_device_prop_desc(code, datatype, getset, default, current, form_flag=FORM_NONE, form=None):
    out = struct.pack('<HHB', code, datatype, getset) + write_typed(datatype, default) + write_typed(datatype, current)
    out += bytes([form_flag])
    if form_flag == FORM_RANGE:
        out += b''.join(write_typed(datatype, form[k]) for k in ('min', 'max', 'step'))
    elif form_flag == FORM_ENUM:
        out += struct.pack('<H', len(form['values'])) + b''.join(write_typed(datatype, x) for x in form['values'])
    return out


# ------------------------------------------------------------------------------ ObjectPropList (MTP 1.1 GetObjectPropList)
PropElement = namedtuple('PropElement', 'handle code datatype value')
ALL_PROPS = 0xFFFFFFFF


def decode_object_prop_list(b, handles=None, prop_code=None):
    """ObjectPropList dataset (MTP 1.1, GetObjectPropList): NumberOfElements (UINT32), then per
    element ObjectHandle (UINT32), ObjectPropertyCode (UINT16), Datatype (UINT16), value. Returns
    (list of PropElement, findings). handles: the handles the request covers; prop_code: the
    requested ObjectPropCode (0xFFFFFFFF = all). Structural: an element cut short or an unknown
    datatype raises MalformedError. Findings: trailing_bytes, unknown_code, prop_datatype,
    reserved_handle, duplicate_element, element_handle, element_prop, filename, storage_id,
    datetime_*, string_*."""
    b = bytes(b)
    name = 'ObjectPropList'
    n, off = _get('<I', b, 0, f'{name}.NumberOfElements')
    if n * 9 > len(b):
        raise MalformedError(f'{name}: {n} elements cannot fit in {len(b)} bytes')
    els, f, seen = [], [], set()
    for i in range(n):
        w = f'{name}[{i}]'
        h, off = _get('<I', b, off, f'{w}.ObjectHandle')
        code, off = _get('<H', b, off, f'{w}.PropertyCode')
        dt, off = _get('<H', b, off, f'{w}.Datatype')
        val, off = read_typed(dt, b, off, f'{w}.value')
        els.append(PropElement(h, code, dt, val))
        f += _typed_findings(dt, val, w)
        if code not in OBJECT_PROPS and not code & 0x8000:
            f.append(Finding('unknown_code', w, f'property {code:#06x}'))
        if code in OBJECT_PROP_TYPES and OBJECT_PROP_TYPES[code] != dt:
            f.append(Finding('prop_datatype', w, f'{OBJECT_PROPS[code]}: datatype {dt:#06x}'))
        if h in (0, 0xFFFFFFFF):
            f.append(Finding('reserved_handle', w, f'{h:#010x}'))
        if (h, code) in seen:
            f.append(Finding('duplicate_element', w, f'handle {h:#x} property {code:#06x} twice'))
        seen.add((h, code))
        if handles is not None and h not in handles:
            f.append(Finding('element_handle', w, f'handle {h:#x} not requested'))
        if prop_code not in (None, ALL_PROPS) and code != prop_code:
            f.append(Finding('element_prop', w, f'property {code:#06x}, requested {prop_code:#06x}'))
        if dt == DT_STR and isinstance(val, MtpString):
            if code in DATE_PROPS:
                f += check_datetime(val.text, w)
            if code == 0xDC07 and val.text == '':
                f.append(Finding('filename', w, 'empty ObjectFileName'))
        if code == 0xDC01 and dt == DT_UINT32:
            f += check_storage_id(val, w)
    if off != len(b):
        f.append(Finding('trailing_bytes', name, f'{len(b) - off} bytes after the last element'))
    return els, f


def encode_object_prop_list(elements):
    out = struct.pack('<I', len(elements))
    for h, code, dt, val in elements:
        out += struct.pack('<IHH', h, code, dt) + write_typed(dt, val)
    return out


def allowed_get_object_prop_list_responses(handle_valid, prop_supported):
    """Response codes MTP 1.1 allows for GetObjectPropList (operation 0x9805) as far as its
    ObjectHandle and ObjectPropCode parameters go: OK (0x2001) when both are valid; an invalid
    handle Invalid_ObjectHandle (0x2009); a valid handle with an unsupported property code
    Invalid_ObjectPropCode (0xA801). With both failing either code is accepted. The other
    parameters (format, group, depth) are not modeled."""
    codes = set()
    if not handle_valid:
        codes.add(0x2009)
    if not prop_supported:
        codes.add(0xA801)
    return codes or {0x2001}
