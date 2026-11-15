# SPDX-License-Identifier: GPL-2.0-or-later
"""Self-test of the USB conformance oracles on synthetic vectors. Exit status 0 = all passed."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_pkg = __import__(os.path.basename(os.path.dirname(os.path.abspath(__file__))))
D, E, M, S = _pkg.descriptors, _pkg.expected, _pkg.mtp, _pkg.scsi

PASS, FAIL = [0], []


def t(name, cond, detail=''):
    if cond:
        PASS[0] += 1
    else:
        FAIL.append(f'{name} {detail}')


def rules(findings):
    return sorted({f.rule for f in findings})


def raises(fn, *a, exc=ValueError, **k):
    try:
        fn(*a, **k)
    except exc:
        return True
    return False


# ------------------------------------------------------------------------------ synthetic composite
def composite(speed='high', other=False):
    """IAD+CDC ACM (if 0,1), MSC (if 2), Audio AC + MIDIStreaming with IAD (if 3,4), still image (if 5)."""
    bulk = 512 if speed == 'high' else 64
    b = bytearray()
    b += D.iad_bytes(0, 2, 2, 2, 1)
    cdc_cs = bytes([5, 0x24, 0x00, 0x20, 0x01]) + bytes([5, 0x24, 0x01, 0x00, 0x01]) + \
        bytes([4, 0x24, 0x02, 0x02]) + bytes([5, 0x24, 0x06, 0x00, 0x01])
    b += D.interface_bytes(0, 0, 1, 2, 2, 1) + cdc_cs + D.endpoint_bytes(0x83, 3, 16, 9 if speed == 'high' else 16)
    b += D.interface_bytes(1, 0, 2, 0x0A, 0, 0) + D.endpoint_bytes(0x01, 2, bulk, 0) + D.endpoint_bytes(0x81, 2, bulk, 0)
    b += D.interface_bytes(2, 0, 2, 8, 6, 0x50) + D.endpoint_bytes(0x02, 2, bulk, 0) + D.endpoint_bytes(0x82, 2, bulk, 0)
    b += D.iad_bytes(3, 2, 1, 1, 0)
    ac = bytes([9, 0x24, 0x01, 0x00, 0x01, 9, 0, 1, 4])
    b += D.interface_bytes(3, 0, 0, 1, 1, 0) + ac
    ms_cs = bytes([6, 0x24, 0x02, 1, 1, 0]) + bytes([6, 0x24, 0x02, 2, 2, 0]) + \
        bytes([9, 0x24, 0x03, 1, 3, 1, 2, 1, 0]) + bytes([9, 0x24, 0x03, 2, 4, 1, 1, 1, 0])
    ep_out = D.endpoint_bytes(0x05, 2, bulk, 0, audio=True) + bytes([5, 0x25, 0x01, 1, 1])
    ep_in = D.endpoint_bytes(0x85, 2, bulk, 0, audio=True) + bytes([5, 0x25, 0x01, 1, 3])
    total = 7 + len(ms_cs) + len(ep_out) + len(ep_in)
    b += D.interface_bytes(4, 0, 2, 1, 3, 0) + bytes([7, 0x24, 0x01, 0x00, 0x01]) + struct.pack('<H', total) + \
        ms_cs + ep_out + ep_in
    b += D.interface_bytes(5, 0, 3, 6, 1, 1) + D.endpoint_bytes(0x06, 2, bulk, 0) + D.endpoint_bytes(0x86, 2, bulk, 0) + \
        D.endpoint_bytes(0x87, 3, 64, 6 if speed == 'high' else 32)
    return D.build_configuration(dict(bNumInterfaces=6, bmAttributes=0xC0, bMaxPower=100), b, other_speed=other)


def test_descriptors():
    dev = D.build_device(bcdUSB=0x200, bDeviceClass=0xEF, bDeviceSubClass=2, bDeviceProtocol=1, idVendor=0x1111,
                         idProduct=0x2222, bcdDevice=0x0100, iManufacturer=1, iProduct=2, iSerialNumber=3)
    t('device ok', D.check_device(dev) == [], D.check_device(dev))
    t('device mps0 hs', 'ep0_mps' in rules(D.check_device(D.build_device(bMaxPacketSize0=8))))
    t('device misc class', 'iad_class' in rules(D.check_device(D.build_device(bDeviceClass=0xEF))))
    t('device bad length', raises(D.parse_device, dev[:17]))
    t('device bad type', raises(D.parse_device, dev[:1] + b'\x02' + dev[2:]))
    q = bytes([10, 6]) + struct.pack('<H', 0x200) + bytes([0xEF, 2, 1, 64, 1, 0])
    t('qualifier ok', D.check_qualifier(q, dev) == [], D.check_qualifier(q, dev))
    t('qualifier reserved', 'reserved' in rules(D.check_qualifier(q[:9] + b'\x01')))
    s = D.build_string('Abé')
    t('string roundtrip', D.parse_string(s) == 'Abé')
    t('string odd', raises(D.parse_string, bytes([5, 3, 0x41, 0, 0x42])))
    t('langid', D.parse_string(bytes([4, 3, 0x09, 0x04]), langids=True) == [0x0409])

    hs = composite('high')
    cfg = D.parse_configuration(hs)
    t('composite parse', len(cfg.alternates) == 6 and len(cfg.iads) == 2)
    f = D.check_configuration(cfg, 'high')
    t('composite hs clean', f == [], f)
    fs = composite('full', other=True)
    f = D.check_configuration(D.parse_configuration(fs), 'full')
    t('composite fs other-speed clean', f == [], f)
    t('hs config at fs', 'bulk_mps' in rules(D.check_configuration(D.parse_configuration(hs), 'full')))
    t('summary', D.tree_summary(cfg)[1] == (1, 0, 2, [0x01, 0x81], 0, [0, 0]))

    # truncation (9.4.3): a reply cut to wLength parses as truncated, without wLength it is malformed
    for n in (9, 10, 30, 60, len(hs) - 1):
        c = D.parse_configuration(hs[:n], wlength=n)
        t(f'truncated {n}', c.truncated and D.check_configuration(c) == [], D.check_configuration(c))
    t('cut without wLength', raises(D.parse_configuration, hs[:30], exc=D.MalformedError))
    t('first descriptor partial', D.parse_configuration(hs[:4], wlength=4).truncated)

    # structural rejections
    t('bLength 0', raises(D.parse_configuration, hs[:9] + b'\x00\x04' + hs[11:], exc=D.MalformedError))
    t('bLength overrun', raises(D.parse_configuration, hs + b'\x09\x04\x00', exc=D.MalformedError))
    t('wrong first type', raises(D.parse_configuration, hs[:1] + b'\x03' + hs[2:], exc=D.MalformedError))
    i0 = hs.index(D.interface_bytes(2, 0, 2, 8, 6, 0x50))
    bad = bytearray(hs)
    bad[i0 + 4] = 3
    t('bNumEndpoints mismatch', raises(D.parse_configuration, bytes(bad), exc=D.MalformedError))
    t('endpoint before interface',
      raises(D.parse_configuration, D.build_configuration({}, D.endpoint_bytes(0x81, 2, 512, 0)), exc=D.MalformedError))
    t('cs ep without ep', raises(D.parse_configuration, D.build_configuration({'bNumInterfaces': 1},
      D.interface_bytes(0, 0, 0, 0xFF, 0, 0) + bytes([4, 0x25, 1, 0])), exc=D.MalformedError))
    t('cs if after ep', raises(D.parse_configuration, D.build_configuration({'bNumInterfaces': 1},
      D.interface_bytes(0, 0, 1, 0xFF, 0, 0) + D.endpoint_bytes(0x81, 2, 512, 0) + bytes([4, 0x24, 1, 0])),
      exc=D.MalformedError))
    t('trailing IAD', raises(D.parse_configuration, D.build_configuration({}, D.iad_bytes(0, 1, 2, 2, 1)),
                             exc=D.MalformedError))
    t('device inside config', raises(D.parse_configuration, D.build_configuration({}, D.build_device()),
                                     exc=D.MalformedError))

    # semantic rules
    def hit(rule, data, speed='high'):
        got = rules(D.check_configuration(D.parse_configuration(data), speed))
        t(f'rule {rule}', rule in got, got)
    body = hs[9:]
    hit('total_length', hs[:2] + struct.pack('<H', 0) + hs[4:])
    hit('num_interfaces', D.build_configuration({'bNumInterfaces': 7}, body))
    hit('config_attributes', D.build_configuration({'bNumInterfaces': 6, 'bmAttributes': 0x40}, body))
    hit('config_value', D.build_configuration({'bNumInterfaces': 6, 'bConfigurationValue': 0}, body))
    one = lambda e, cls=0xFF: D.build_configuration({'bNumInterfaces': 1}, D.interface_bytes(0, 0, len(e) // 7, cls, 0, 0) + e)
    hit('dup_ep_addr', one(D.endpoint_bytes(0x81, 2, 512, 0) + D.endpoint_bytes(0x81, 2, 512, 0)))
    hit('ep_address', one(D.endpoint_bytes(0x80, 2, 512, 0)))
    hit('ep_address', one(D.endpoint_bytes(0x91, 2, 512, 0)))
    hit('ep_attributes', one(D.endpoint_bytes(0x81, 0x0E, 512, 0)))
    hit('bulk_mps', one(D.endpoint_bytes(0x81, 2, 64, 0)))
    hit('bulk_mps', one(D.endpoint_bytes(0x81, 2, 0x0B00, 0)))
    hit('ep_high_bandwidth', one(D.endpoint_bytes(0x81, 2, 0x0B00, 0)))
    hit('ep_high_bandwidth', one(D.endpoint_bytes(0x81, 3, 0x0800 | 100, 4)))
    t('hb ok', D.check_configuration(D.parse_configuration(one(D.endpoint_bytes(0x81, 1, 0x1000 | 1024, 1))), 'high') == [])
    hit('ep_interval', one(D.endpoint_bytes(0x81, 3, 64, 0)))
    hit('ep_interval', one(D.endpoint_bytes(0x81, 3, 64, 17)))
    hit('int_mps', one(D.endpoint_bytes(0x81, 3, 65, 10)), 'full')
    hit('iso_mps', one(D.endpoint_bytes(0x81, 1, 1024, 1)), 'full')
    two = D.build_configuration({'bNumInterfaces': 2}, D.interface_bytes(0, 0, 0, 0xFF, 0, 0) +
                                D.interface_bytes(1, 0, 0, 0xFF, 0, 0) + D.interface_bytes(0, 1, 0, 0xFF, 0, 0))
    hit('interface_order', two)
    hit('interface_numbers', D.build_configuration({'bNumInterfaces': 1}, D.interface_bytes(1, 0, 0, 0xFF, 0, 0)))
    hit('alt_numbers', D.build_configuration({'bNumInterfaces': 1}, D.interface_bytes(0, 0, 0, 0xFF, 0, 0) +
                                             D.interface_bytes(0, 2, 0, 0xFF, 0, 0)))
    hit('iad_range', D.build_configuration({'bNumInterfaces': 1}, D.iad_bytes(0, 2, 2, 2, 1) + D.interface_bytes(0, 0, 0, 0xFF, 0, 0)))
    hit('iad_position', D.build_configuration({'bNumInterfaces': 2}, D.interface_bytes(0, 0, 0, 0xFF, 0, 0) +
                                              D.iad_bytes(0, 2, 2, 2, 1) + D.interface_bytes(1, 0, 0, 0xFF, 0, 0)))
    # class rules
    def patch(old, new, rule):
        assert hs.count(old) == 1, old
        hit(rule, hs.replace(old, new))
    patch(bytes([5, 0x24, 0x00, 0x20, 0x01]), bytes([5, 0x24, 0x09, 0x20, 0x01]), 'cdc_header')
    patch(bytes([5, 0x24, 0x06, 0x00, 0x01]), bytes([5, 0x24, 0x06, 0x00, 0x09]), 'cdc_union')
    patch(D.interface_bytes(2, 0, 2, 8, 6, 0x50), D.interface_bytes(2, 0, 2, 8, 6, 0x62), 'msc_protocol')
    patch(D.endpoint_bytes(0x82, 2, 512, 0), D.endpoint_bytes(0x82, 3, 512, 1), 'msc_endpoints')
    patch(D.interface_bytes(5, 0, 3, 6, 1, 1), D.interface_bytes(5, 0, 3, 6, 1, 2), 'ptp_protocol')
    patch(D.endpoint_bytes(0x87, 3, 64, 6), D.endpoint_bytes(0x87, 2, 512, 0), 'ptp_endpoints')
    patch(bytes([9, 0x24, 0x01, 0x00, 0x01, 9, 0, 1, 4]), bytes([9, 0x24, 0x01, 0x00, 0x01, 10, 0, 1, 4]), 'audio_total_length')
    patch(bytes([9, 0x24, 0x01, 0x00, 0x01, 9, 0, 1, 4]), bytes([9, 0x24, 0x01, 0x00, 0x01, 9, 0, 1, 9]), 'audio_collection')
    patch(bytes([9, 0x24, 0x03, 1, 3, 1, 2, 1, 0]), bytes([9, 0x24, 0x03, 1, 3, 1, 7, 1, 0]), 'midi_jack')
    patch(bytes([5, 0x25, 0x01, 1, 1]), bytes([5, 0x25, 0x01, 1, 8]), 'midi_jack')
    patch(bytes([5, 0x25, 0x01, 1, 3]), bytes([5, 0x25, 0x02, 1, 3]), 'midi_ep_general')
    patch(bytes([6, 0x24, 0x02, 2, 2, 0]), bytes([6, 0x24, 0x02, 2, 1, 0]), 'midi_jack')
    i4 = hs.index(D.interface_bytes(4, 0, 2, 1, 3, 0)) + 9
    bad = bytearray(hs)
    bad[i4 + 5] += 1
    hit('midi_total_length', bytes(bad))

    # declarative expected values
    tmpl = E.template_from_configuration(hs)
    t('template self-match', E.match_configuration(hs, tmpl) == [])
    t('template catches byte', E.match_configuration(hs.replace(bytes([4, 0x24, 0x02, 0x02]), bytes([4, 0x24, 0x02, 0x06])), tmpl) != [])
    partial = dict(fields=dict(bNumInterfaces=6, bmAttributes=0xC0),
                   interfaces=[dict(bInterfaceClass=2), dict(), dict(bInterfaceClass=8, endpoints=[
                       dict(bEndpointAddress=0x02), dict(bEndpointAddress=0x82, wMaxPacketSize=512)]), {}, {}, {}])
    t('partial template', E.match_configuration(hs, partial) == [])
    partial['interfaces'][2]['endpoints'][1]['wMaxPacketSize'] = 64
    t('partial template mismatch', len(E.match_configuration(hs, partial)) == 1)
    replies = dict(device=dev, configuration=hs, strings={1: D.build_string('Maker')})
    t('match_all', E.match_all(replies, dict(device=dict(idVendor=0x1111), strings={1: 'Maker'})) == [])
    t('match_all vid', len(E.match_all(replies, dict(device=dict(idVendor=0x1112)))) == 1)
    t('match_all string', len(E.match_all(replies, dict(strings={1: 'Other', 2: 'X'}))) == 2)


# ------------------------------------------------------------------------------ MTP
def test_mtp():
    t('tables', M.OPERATIONS[0x1001] == 'GetDeviceInfo' and M.RESPONSES[0x2001] == 'OK' and
      M.EVENTS[0x4002] == 'ObjectAdded' and M.OBJECT_PROPS[0xDC07] == 'ObjectFileName')
    s = M.MtpString('ab')
    t('string encode', s.encode() == b'\x03a\x00b\x00\x00\x00')
    t('empty string', M.MtpString('').encode() == b'\x00')
    t('string read', M.read_string(b'\x03a\x00b\x00\x00\x00', 0)[0].text == 'ab')
    t('string overrun', raises(M.read_string, b'\x03a\x00', 0, exc=M.MalformedError))
    di = dict(StandardVersion=100, VendorExtensionID=6, VendorExtensionVersion=100,
              VendorExtensionDesc='microsoft.com: 1.0;', FunctionalMode=0,
              OperationsSupported=[0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1007, 0x1008, 0x1009, 0x9801],
              EventsSupported=[0x4002, 0x4003], DevicePropertiesSupported=[0x5001, 0xD402],
              CaptureFormats=[], PlaybackFormats=[0x3000, 0x3001, 0x3008],
              Manufacturer='Maker', Model='Model', DeviceVersion='1.0', SerialNumber='0001')
    b = M.encode_device_info(**di)
    v, f = M.decode_device_info(b)
    t('DeviceInfo clean', f == [], f)
    t('DeviceInfo roundtrip', M.encode_device_info(**v) == b and v['Model'] == 'Model')
    v, f = M.decode_device_info(b[:-11])           # ends after DeviceVersion
    t('DeviceInfo missing serial', rules(f) == ['missing_fields'] and v['SerialNumber'] is None, f)
    t('DeviceInfo short roundtrip', M.encode_device_info(**v) == b[:-11])
    t('DeviceInfo cut mid-field', raises(M.decode_device_info, b[:-3], exc=M.MalformedError))
    v, f = M.decode_device_info(b + b'\x00')
    t('DeviceInfo trailing', rules(f) == ['trailing_bytes'] and M.encode_device_info(**v) == b + b'\x00')
    t('DeviceInfo unknown op', 'unknown_code' in rules(M.decode_device_info(M.encode_device_info(
        **dict(di, OperationsSupported=[0x1001, 0x1002, 0x1003, 0x1077])))[1]))
    t('DeviceInfo required op', 'required_operation' in rules(M.decode_device_info(M.encode_device_info(
        **dict(di, OperationsSupported=[0x1001])))[1]))
    t('DeviceInfo version', 'standard_version' in rules(M.decode_device_info(M.encode_device_info(
        **dict(di, StandardVersion=110)))[1]))
    t('string_nul', 'string_nul' in rules(M.decode_device_info(M.encode_device_info(
        **dict(di, Model=M.MtpString([0x41, 0x42]))))[1]))

    si = dict(StorageType=3, FilesystemType=2, AccessCapability=0, MaxCapacity=1 << 30, FreeSpaceInBytes=1 << 20,
              FreeSpaceInObjects=0xFFFFFFFF, StorageDescription='Store', VolumeIdentifier='')
    b = M.encode_storage_info(**si)
    v, f = M.decode_storage_info(b)
    t('StorageInfo clean', f == [] and M.encode_storage_info(**v) == b, f)
    t('StorageInfo free', 'free_space' in rules(M.decode_storage_info(M.encode_storage_info(
        **dict(si, FreeSpaceInBytes=1 << 31)))[1]))
    t('StorageInfo type', 'storage_type' in rules(M.decode_storage_info(M.encode_storage_info(
        **dict(si, StorageType=9)))[1]))
    t('StorageInfo access', 'access_capability' in rules(M.decode_storage_info(M.encode_storage_info(
        **dict(si, AccessCapability=3)))[1]))

    oi = dict(StorageID=0x00010001, ObjectFormat=0x3008, ProtectionStatus=0, ObjectCompressedSize=1234,
              ThumbFormat=0, ThumbCompressedSize=0, ThumbPixWidth=0, ThumbPixHeight=0, ImagePixWidth=0,
              ImagePixHeight=0, ImageBitDepth=0, ParentObject=0, AssociationType=0, AssociationDesc=0,
              SequenceNumber=0, Filename='a.wav', DateCreated='20240102T030405', DateModified='', Keywords='')
    b = M.encode_object_info(**oi)
    v, f = M.decode_object_info(b)
    t('ObjectInfo clean', f == [] and M.encode_object_info(**v) == b, f)
    for k, val, rule in (('DateCreated', '2024-01-02', 'datetime_format'), ('DateCreated', '20241302T000000', 'datetime_range'),
                         ('ParentObject', 0xFFFFFFFF, 'parent_object'), ('StorageID', 0x00010000, 'storage_id'),
                         ('ProtectionStatus', 7, 'protection_status'), ('Filename', '', 'filename')):
        t(f'ObjectInfo {rule}', rule in rules(M.decode_object_info(M.encode_object_info(**dict(oi, **{k: val})))[1]))
    t('datetime tz', M.check_datetime('20240102T030405.5+0100') == [])
    t('folder assoc', 'association_type' in rules(M.decode_object_info(M.encode_object_info(
        **dict(oi, ObjectFormat=0x3001, AssociationType=2)))[1]))

    hb = M.encode_handles([1, 2, 3])
    t('handles', M.decode_handles(hb) == ([1, 2, 3], []))
    t('handles dup', 'duplicate_handle' in rules(M.decode_handles(M.encode_handles([1, 1]))[1]))
    t('handles reserved', 'reserved_handle' in rules(M.decode_handles(M.encode_handles([0]))[1]))
    t('handles overrun', raises(M.decode_handles, struct.pack('<II', 2, 1), exc=M.MalformedError))
    t('storage ids', M.decode_storage_ids(M.encode_handles([0x10001, 0x20001])) == ([0x10001, 0x20001], []))

    c = M.encode_container(2, 0x1001, 7, b)
    cc, v, f = M.decode_data_reply(c, 0x1001, M.decode_object_info, tid=7)
    t('data container', f == [] and cc.length == len(c), f)
    _, _, f = M.decode_data_reply(c[:4] + struct.pack('<H', 3) + c[6:], 0x1001, M.decode_object_info, 7)
    t('data container type', 'container_type' in rules(f))
    _, _, f = M.decode_data_reply(struct.pack('<I', len(c) + 2) + c[4:], 0x1001, M.decode_object_info, 7)
    t('data container length', 'container_length' in rules(f))
    r = M.encode_container(3, 0x2001, 7, params=[5])
    cc, f = M.check_response(r, tid=7, want_code=0x2001, nparams=1)
    t('response', f == [] and M.params_of(cc) == [5], f)
    t('response tid', 'transaction_id' in rules(M.check_response(r, tid=8)[1]))
    t('response code', 'unknown_code' in rules(M.check_response(M.encode_container(3, 0x2077, 1))[1]))
    t('response params', 'container_params' in rules(M.check_response(M.encode_container(3, 0x2001, 1, b'\0' * 6))[1]))
    t('container short', raises(M.decode_container, b'\0' * 11, exc=M.MalformedError))
    t('container type 9', raises(M.decode_container, struct.pack('<IHHI', 12, 9, 0x2001, 0), exc=M.MalformedError))


# ------------------------------------------------------------------------------ SCSI / BOT
def test_scsi():
    dev = S.BlockDevice(1000, 512)
    tg = S.Target(dev, vendor='VEND', product='PROD', revision='0001', sense_valid=False)
    r = tg.execute(S.cdb_inquiry(36))
    t('inquiry', r.status == S.GOOD and len(r.data_in) == 36 and r.data_in[8:16] == b'VEND    ')
    t('inquiry check', S.check_inquiry(r.data_in, 36, vendor='VEND', product='PROD', rmb=True) == [])
    t('inquiry trunc', tg.execute(S.cdb_inquiry(5)).data_in == r.data_in[:5])
    t('inquiry trunc check', S.check_inquiry(r.data_in[:5], 5) == [])
    t('inquiry short', 'alloc_length' in rules(S.check_inquiry(r.data_in[:20], 36)))
    t('inquiry long', 'alloc_length' in rules(S.check_inquiry(r.data_in, 20)))
    t('inquiry ascii', 'inquiry_ascii' in rules(S.check_inquiry(r.data_in[:8] + b'\x01' + r.data_in[9:], 36)))
    t('inquiry value', 'inquiry_value' in rules(S.check_inquiry(r.data_in, 36, product='OTHER')))
    r = tg.execute(S.cdb_inquiry(36, page=0x80))
    t('inquiry page w/o evpd', r.status == S.CHECK_CONDITION and r.sense[2] == S.ILLEGAL_REQUEST and r.sense[12] == 0x24)
    tg.vpd[0x80] = b'SER1'
    t('vpd 0', tg.execute(S.cdb_inquiry(255, evpd=True)).data_in == bytes([0, 0, 0, 2, 0, 0x80]))
    t('vpd 80', tg.execute(S.cdb_inquiry(255, evpd=True, page=0x80)).data_in == bytes([0, 0x80, 0, 4]) + b'SER1')
    t('vpd unknown', tg.execute(S.cdb_inquiry(255, evpd=True, page=0x83)).status == S.CHECK_CONDITION)

    r = tg.execute(S.cdb_read_capacity10())
    t('capacity', r.data_in == struct.pack('>II', 999, 512) and S.check_read_capacity10(r.data_in, dev) == [])
    t('capacity wrong', 'capacity_value' in rules(S.check_read_capacity10(struct.pack('>II', 1000, 512), dev)))
    big = S.Target(S.BlockDevice(1 << 33, 512))
    t('capacity clamp', big.execute(S.cdb_read_capacity10()).data_in[:4] == b'\xff\xff\xff\xff')

    t('tur', tg.execute(S.cdb_tur()).status == S.GOOD)
    tg.set_present(False)
    r = tg.execute(S.cdb_tur())
    t('tur not ready', r.status == S.CHECK_CONDITION)
    r = tg.execute(S.cdb_request_sense(18))
    t('sense not present', S.check_request_sense(r.data_in, 18, S.SENSE_MEDIUM_NOT_PRESENT) == [], r.data_in.hex())
    t('sense cleared', S.check_request_sense(tg.execute(S.cdb_request_sense(18)).data_in, 18, S.SENSE_NONE) == [])
    t('capacity not ready', tg.execute(S.cdb_read_capacity10()).status == S.CHECK_CONDITION)
    tg.set_present(True)
    t('unit attention', tg.execute(S.cdb_tur()).sense[2] == S.UNIT_ATTENTION)
    t('after UA', tg.execute(S.cdb_tur()).status == S.GOOD)

    r = tg.execute(S.cdb_request_sense(8))
    t('sense trunc', len(r.data_in) == 8 and S.check_request_sense(r.data_in, 8) == [])
    t('sense format', 'sense_format' in rules(S.check_request_sense(b'\x72' + bytes(17), 18)))
    t('sense short', 'alloc_length' in rules(S.check_request_sense(r.data_in, 18)))

    data = bytes(range(256)) * 4
    t('write', tg.execute(S.cdb_write10(10, 2), data).status == S.GOOD)
    t('read back', tg.execute(S.cdb_read10(10, 2)).data_in == data)
    t('read blank', tg.execute(S.cdb_read10(0, 1)).data_in == bytes(512))
    t('read zero blocks', tg.execute(S.cdb_read10(5, 0)) == (S.GOOD, b'', 0, tg.sense_bytes()))
    r = tg.execute(S.cdb_read10(999, 2))
    t('read out of range', r.status == S.CHECK_CONDITION and r.sense[12] == 0x21 and r.sense[0] == 0x70)
    s = tg.execute(S.cdb_request_sense(18)).data_in
    t('sense info', S.check_request_sense(s, 18, S.SENSE_LBA_OUT_OF_RANGE, info=999) == [], s.hex())
    dev.write_protect = True
    r = tg.execute(S.cdb_write10(0, 1), bytes(512))
    t('write protect', r.sense[2] == S.DATA_PROTECT and r.sense[12] == 0x27)
    t('read while wp', tg.execute(S.cdb_read10(10, 1)).data_in == data[:512])

    r = tg.execute(S.cdb_mode_sense6())
    t('mode sense', r.data_in == bytes([11, 0, 0x80, 8, 0, 0, 3, 0xE8, 0, 0, 2, 0]), r.data_in.hex())
    t('mode sense check', S.check_mode_sense6(r.data_in, 0xFF, dev) == [], S.check_mode_sense6(r.data_in, 0xFF, dev))
    dev.write_protect = False
    t('mode sense wp', 'write_protect' in rules(S.check_mode_sense6(r.data_in, 0xFF, dev)))
    tg.mode_pages[0x08] = bytes([0x08, 0x0A]) + bytes([4]) + bytes(9)
    r = tg.execute(S.cdb_mode_sense6(dbd=True, alloc=4))
    t('mode sense trunc', r.data_in == bytes([15, 0, 0, 0]) and S.check_mode_sense6(r.data_in, 4, dev) == [])
    r = tg.execute(S.cdb_mode_sense6(page=0x08, dbd=True))
    t('mode page', r.data_in[4:] == tg.mode_pages[0x08] and S.check_mode_sense6(r.data_in, 0xFF, dev) == [])
    t('mode page changeable', tg.execute(S.cdb_mode_sense6(page=0x08, pc=1, dbd=True)).data_in[6] == 0)
    t('mode page unknown', tg.execute(S.cdb_mode_sense6(page=0x1C)).sense[12] == 0x24)
    t('mode saved', tg.execute(S.cdb_mode_sense6(pc=3)).sense[12] == 0x39)
    t('mode bad page', 'mode_page' in rules(S.check_mode_sense6(bytes([7, 0, 0, 0, 0x08, 0x0A, 0, 0]), 0xFF)))

    t('invalid opcode', tg.execute(bytes([0xE7]) + bytes(5)).sense[12] == 0x20)
    t('lun', tg.execute(S.cdb_tur(), lun=1).sense[12] == 0x25)
    t('lun inquiry', tg.execute(S.cdb_inquiry(36), lun=1).data_in[0] == 0x7F)

    # BOT wrappers and the thirteen cases
    cbw = S.encode_cbw(0x1234, 36, True, 0, S.cdb_inquiry(36))
    c, f = S.decode_cbw(cbw)
    t('cbw', f == [] and c.tag == 0x1234 and c.cb_length == 6)
    t('cbw short', raises(S.decode_cbw, cbw[:30], exc=S.MalformedError))
    t('cbw long', raises(S.decode_cbw, cbw + b'\0', exc=S.MalformedError))
    t('cbw signature', raises(S.decode_cbw, b'USBD' + cbw[4:], exc=S.MalformedError))
    t('cbw lun', 'cbw_lun' in rules(S.decode_cbw(cbw[:13] + b'\x02' + cbw[14:])[1]))
    t('cbw flags', 'cbw_reserved' in rules(S.decode_cbw(cbw[:12] + b'\x81' + cbw[13:])[1]))
    t('cbw cblen', 'cbw_cb_length' in rules(S.decode_cbw(cbw[:14] + b'\x00' + cbw[15:])[1]))
    csw = S.encode_csw(0x1234, 0, 0)
    t('csw', S.decode_csw(csw, c) == (S.CSW(S.CSW_SIGNATURE, 0x1234, 0, 0), []))
    t('csw tag', 'csw_tag' in rules(S.decode_csw(S.encode_csw(1, 0, 0), c)[1]))
    t('csw residue', 'csw_residue' in rules(S.decode_csw(S.encode_csw(0x1234, 37, 0), c)[1]))
    t('csw status', 'csw_status' in rules(S.decode_csw(S.encode_csw(0x1234, 0, 3), c)[1]))
    t('csw short', raises(S.decode_csw, csw[:12], exc=S.MalformedError))
    cases = {(0, True, 0, True): 1, (0, True, 5, True): 2, (0, True, 5, False): 3, (9, True, 0, True): 4,
             (9, True, 5, True): 5, (5, True, 5, True): 6, (4, True, 5, True): 7, (5, True, 5, False): 8,
             (9, False, 0, True): 9, (9, False, 5, True): 10, (9, False, 5, False): 11, (5, False, 5, False): 12,
             (4, False, 5, False): 13}
    for k, n in cases.items():
        o = S.bot_expect(*k)
        t(f'bot case {n}', o.case == n)
    t('case 5 residue', S.bot_expect(9, True, 5, True) == S.BotOutcome(5, 5, 0, 4, 'in'))
    t('case 7 phase', S.bot_expect(4, True, 5, True).status == S.CSW_PHASE_ERROR)
    t('failed command', S.bot_expect(0, True, 0, True, command_ok=False).status == S.CSW_FAILED)
    data, csw, o = S.run_bot(tg, S.encode_cbw(5, 0x24, True, 0, S.cdb_inquiry(0x24)))
    t('run_bot inquiry', o.case == 6 and len(data) == 36 and S.decode_csw(csw)[0].residue == 0)
    data, csw, o = S.run_bot(tg, S.encode_cbw(6, 0x200, True, 0, S.cdb_inquiry(0xFF)))
    t('run_bot case 5', o.case == 5 and S.decode_csw(csw)[0].residue == 0x200 - 36)
    data, csw, o = S.run_bot(tg, S.encode_cbw(7, 0, True, 0, S.cdb_tur()))
    t('run_bot tur', o.case == 1 and S.decode_csw(csw)[0].status == 0)
    data, csw, o = S.run_bot(tg, S.encode_cbw(8, 512, False, 0, S.cdb_write10(1, 1)), bytes(512))
    t('run_bot write', o.case == 12 and S.decode_csw(csw)[0].status == 0)
    data, csw, o = S.run_bot(tg, S.encode_cbw(9, 512, True, 0, S.cdb_read10(5000, 1)))
    t('run_bot read error', o.case == 4 and S.decode_csw(csw)[0].status == S.CSW_FAILED)


# ------------------------------------------------------------------------------ robustness
def test_integration():
    """Integration pass: VPD 0x00 / 0x83, READ(6)/WRITE(6), READ/WRITE(10) and MODE
    SENSE(6) edges, the medium-change model, compare_reply, DevicePropDesc and ObjectPropList."""
    # -- VPD 0x83 (S-16.050): an SPC-valid page whose PAGE LENGTH equals the bytes after the header
    naa = S.designation_descriptor(S.DESIG_NAA, bytes([0x50, 1, 2, 3, 4, 5, 6, 7]))
    t10 = S.designation_descriptor(S.DESIG_T10, b'VENDORXXSERIAL01')
    page = S.build_vpd83([naa, t10])
    descs, f = S.decode_vpd83(page, 255)
    t('vpd83 clean', f == [] and len(descs) == 2 and descs[0].type == S.DESIG_NAA, f)
    t('vpd83 page length', struct.unpack_from('>H', page, 2)[0] == len(page) - 4)
    t('vpd83 alloc cut', S.decode_vpd83(page[:18], 18)[1] == [] and len(S.decode_vpd83(page[:18], 18)[0]) == 1)
    t('vpd83 header cut ok', S.decode_vpd83(page[:4], 4)[1] == [])
    t('vpd83 short raises', raises(S.decode_vpd83, page[:3], exc=S.MalformedError))
    bad = bytearray(page); bad[3] += 7; bad += bytes(7)          # PAGE LENGTH from stray memory
    t('vpd83 leftover bytes', 'vpd_page_length' in rules(S.decode_vpd83(bytes(bad), 255)[1]))
    bad = bytearray(page); bad[3] -= 3                           # last descriptor past PAGE LENGTH
    t('vpd83 desc past page raises', raises(S.decode_vpd83, bytes(bad[:4 + bad[3]]) + bytes(3), exc=S.MalformedError))
    t('vpd83 stray after page', 'vpd_page_length' in rules(S.decode_vpd83(page + b'\x00', 255)[1]))
    bad = bytearray(page); bad[4 + 2] = 0x55                     # reserved byte written
    t('vpd83 reserved', 'desig_reserved' in rules(S.decode_vpd83(bytes(bad), 255)[1]))
    t('vpd83 page code', 'vpd_page_code' in rules(S.decode_vpd83(bytes([0, 0x80]) + page[2:], 255)[1]))
    t('vpd83 alloc over', 'alloc_length' in rules(S.decode_vpd83(page, 8)[1]))
    t('vpd83 alloc under', 'alloc_length' in rules(S.decode_vpd83(page[:9], 255)[1]))
    for desc, rule in ((S.designation_descriptor(S.DESIG_NAA, bytes([0x50, 1, 2, 3])), 'desig_naa'),
                       (S.designation_descriptor(S.DESIG_NAA, bytes([0x60]) + bytes(7)), 'desig_naa'),
                       (S.designation_descriptor(S.DESIG_T10, b'SHORT'), 'desig_length'),
                       (S.designation_descriptor(S.DESIG_T10, b'VENDOR\x01XYZ'), 'desig_ascii'),
                       (S.designation_descriptor(S.DESIG_EUI64, bytes(9)), 'desig_length'),
                       (S.designation_descriptor(S.DESIG_REL_PORT, bytes(4)), 'desig_association'),
                       (S.designation_descriptor(S.DESIG_MD5, bytes(15)), 'desig_length'),
                       (S.designation_descriptor(S.DESIG_NAME_STRING, b'abc'), 'desig_length'),
                       (S.designation_descriptor(9, b'x'), 'desig_reserved'),
                       (S.designation_descriptor(S.DESIG_NAA, bytes([0x50]) + bytes(7), code_set=0), 'desig_code_set'),
                       (S.designation_descriptor(S.DESIG_NAA, bytes([0x50]) + bytes(7), piv=True), 'desig_reserved')):
        t(f'vpd83 rule {rule}', rule in rules(S.decode_vpd83(S.build_vpd83([naa, desc]), 255)[1]),
          rules(S.decode_vpd83(S.build_vpd83([naa, desc]), 255)[1]))
    t('vpd83 lu_name', 'lu_name' in rules(S.decode_vpd83(S.build_vpd83(
        [S.designation_descriptor(S.DESIG_VENDOR, b'abcd')]), 255)[1]))
    t('vpd83 duplicate', 'designator_duplicate' in rules(S.decode_vpd83(S.build_vpd83([naa, naa]), 255)[1]))
    t('vpd83 port assoc ok', S.decode_vpd83(S.build_vpd83([naa, S.designation_descriptor(
        S.DESIG_REL_PORT, bytes([0, 0, 0, 1]), association=S.ASSOC_PORT, piv=True, protocol=6)]), 255)[1] == [])
    # -- page 0x83 offered and not offered (S-16.050), page 0x00 / offer consistency
    dev = S.BlockDevice(1000)
    for offered in (True, False):
        tg = S.Target(dev, vendor='V', product='P', revision='1', vpd={0x80: b'SER1', **({0x83: page[4:]} if offered else {})})
        sup = tg.execute(S.cdb_inquiry(255, evpd=True)).data_in
        pages, f = S.decode_vpd00(sup, 255)
        t(f'vpd00 clean offered={offered}', f == [] and (0x83 in pages) == offered, f)
        reps = {p: tg.execute(S.cdb_inquiry(255, evpd=True, page=p)) for p in (0x80, 0x83, 0xB0)}
        t(f'vpd offer consistent offered={offered}', S.check_vpd_offer(sup, reps) == [], S.check_vpd_offer(sup, reps))
        if offered:
            t('vpd83 via target', reps[0x83].data_in == page and S.decode_vpd83(reps[0x83].data_in, 255)[1] == [])
    t('vpd offer listed fails', 'vpd_listed_fails' in rules(S.check_vpd_offer(
        bytes([0, 0, 0, 2, 0, 0x83]), {0x83: (S.CHECK_CONDITION, b'', S.SENSE_INVALID_FIELD_IN_CDB)})))
    t('vpd offer unlisted answers', 'vpd_unlisted_answers' in rules(S.check_vpd_offer(
        bytes([0, 0, 0, 1, 0]), {0x83: (S.GOOD, page, None)})))
    t('vpd offer unlisted sense', 'vpd_unlisted_sense' in rules(S.check_vpd_offer(
        bytes([0, 0, 0, 1, 0]), {0x83: (S.CHECK_CONDITION, b'', S.SENSE_INVALID_OPCODE)})))
    t('vpd00 order', 'vpd_list_order' in rules(S.decode_vpd00(bytes([0, 0, 0, 3, 0, 0x83, 0x80]))[1]))
    t('vpd00 dup', 'vpd_list_order' in rules(S.decode_vpd00(bytes([0, 0, 0, 3, 0, 0x80, 0x80]))[1]))
    t('vpd00 zero', 'vpd_list_zero' in rules(S.decode_vpd00(bytes([0, 0, 0, 1, 0x80]))[1]))
    t('vpd00 length', 'vpd_page_length' in rules(S.decode_vpd00(bytes([0, 0, 0, 1, 0, 0x80]))[1]))
    t('vpd00 short raises', raises(S.decode_vpd00, b'\0\0', exc=S.MalformedError))
    # -- READ(6) / WRITE(6) (S-16.051): 21-bit LBA, length 0 = 256 blocks
    big = S.BlockDevice(0x200000 + 512)
    tg = S.Target(big)
    t('rw6 decode lba', S.rw_lba_length(bytes([S.READ6, 0x1F, 0xFF, 0xFF, 0, 0])) == (0x1FFFFF, 256))
    t('rw6 decode bits 7:5 ignored', S.rw_lba_length(bytes([S.READ6, 0xE1, 0, 2, 1, 0])) == (0x10002, 1))
    t('rw6 cdb', S.cdb_read6(0x12345, 256) == bytes([S.READ6, 0x01, 0x23, 0x45, 0, 0]))
    t('rw6 cdb range', raises(S.cdb_read6, 1 << 21, 1) and raises(S.cdb_read6, 0, 0) and raises(S.cdb_write6, 0, 257))
    t('rw decode short raises', raises(S.rw_lba_length, bytes([S.READ10, 0, 0]), exc=S.MalformedError)
      and raises(S.rw_lba_length, bytes([S.TEST_UNIT_READY]) + bytes(5), exc=S.MalformedError))
    blk = bytes(range(256)) * 2
    data = b''.join(bytes([i]) * 512 for i in range(256))
    r = tg.execute(S.cdb_write6(0x10000, 256), data)
    t('write6 256 blocks', r.status == S.GOOD and r.data_out_len == 256 * 512)
    r = tg.execute(S.cdb_read10(0x10000, 256))
    t('write6 landed at the 21-bit LBA', r.data_in == data and big.read(0, 1) == bytes(512))
    r = tg.execute(S.cdb_read6(0x10000 + 255, 1))
    t('read6 one block', r.status == S.GOOD and r.data_in == bytes([255]) * 512)
    r = tg.execute(S.cdb_read6(0x1FFFFF, 256))
    t('read6 0=256 near the end', r.status == S.GOOD and len(r.data_in) == 256 * 512)
    small = S.Target(S.BlockDevice(100))
    r = small.execute(S.cdb_read6(0, 256))
    t('read6 256 past end', r.status == S.CHECK_CONDITION and S.sense_triple(r.sense) == S.SENSE_LBA_OUT_OF_RANGE)
    r = small.execute(S.cdb_read6(0x10000, 1))
    t('read6 high LBA out of range', S.sense_triple(r.sense) == S.SENSE_LBA_OUT_OF_RANGE)
    small.dev.write_protect = True
    t('write6 protected', S.sense_triple(small.execute(S.cdb_write6(0, 1), blk).sense) == S.SENSE_WRITE_PROTECTED)
    small.dev.write_protect = False
    # -- READ/WRITE(10) edges
    r = small.execute(S.cdb_read10(100, 0))
    t('read10 zero length at capacity', r.status == S.GOOD and r.data_in == b'')
    t('read10 zero length past capacity', S.sense_triple(small.execute(S.cdb_read10(101, 0)).sense) == S.SENSE_LBA_OUT_OF_RANGE)
    r = small.execute(S.cdb_write10(5, 0), b'')
    t('write10 zero length', r.status == S.GOOD and r.data_out_len == 0)
    r = small.execute(S.cdb_read10(99, 2))
    t('read10 straddles end', S.sense_triple(r.sense) == S.SENSE_LBA_OUT_OF_RANGE and r.data_in == b'')
    t('read10 info = LBA', struct.unpack_from('>I', r.sense, 3)[0] == 99 and r.sense[0] & 0x80)
    t('read10 lba 2^32-1', S.sense_triple(small.execute(S.cdb_read10(0xFFFFFFFF, 1)).sense) == S.SENSE_LBA_OUT_OF_RANGE)
    cdb = bytearray(S.cdb_read10(0, 1)); cdb[1] = 0x20
    t('read10 rdprotect', S.sense_triple(small.execute(bytes(cdb)).sense) == S.SENSE_INVALID_FIELD_IN_CDB)
    cdb = bytearray(S.cdb_write10(0, 1)); cdb[1] = 0x08          # FUA: allowed
    t('write10 fua ok', small.execute(bytes(cdb), blk).status == S.GOOD)
    t('short cdb no crash', S.sense_triple(small.execute(bytes([S.READ10, 0, 0])).sense) == S.SENSE_INVALID_FIELD_IN_CDB
      and S.sense_triple(small.execute(b'').sense) == S.SENSE_INVALID_OPCODE)
    # -- MODE SENSE(6) edges: SBC short LBA block descriptor, subpage, allocation length 0
    huge = S.Target(S.BlockDevice(0x1234567), mode_pages={0x08: bytes([0x08, 0x0A]) + bytes(10)})
    r = huge.execute(S.cdb_mode_sense6(page=0x08))
    t('mode bd 4-byte count', r.data_in[3] == 8 and r.data_in[4:8] == (0x1234567).to_bytes(4, 'big')
      and r.data_in[9:12] == (512).to_bytes(3, 'big'))
    t('mode bd check', S.check_mode_sense6(r.data_in, 0xFF, huge.dev) == [], S.check_mode_sense6(r.data_in, 0xFF, huge.dev))
    bad = bytearray(r.data_in); bad[7] ^= 1
    t('mode bd mismatch', 'block_descriptor' in rules(S.check_mode_sense6(bytes(bad), 0xFF, huge.dev)))
    t('mode subpage', S.sense_triple(huge.execute(S.cdb_mode_sense6(subpage=1)).sense) == S.SENSE_INVALID_FIELD_IN_CDB)
    r = huge.execute(S.cdb_mode_sense6(alloc=0))
    t('mode alloc 0', r.status == S.GOOD and r.data_in == b'' and S.check_mode_sense6(r.data_in, 0, huge.dev) == [])
    # -- TEST UNIT READY and the medium change (S-12.018): absent, then unit attention, then ready
    tg = S.Target(S.BlockDevice(100))
    seq = [tg.execute(S.cdb_tur())]
    tg.set_present(False)
    seq += [tg.execute(S.cdb_tur()) for _ in range(2)]
    tg.set_present(True)
    seq += [tg.execute(S.cdb_tur()) for _ in range(2)]
    obs = [(r.status, r.sense) for r in seq]
    t('tur sequence', [S.sense_triple(r.sense) if r.status else 'GOOD' for r in seq] ==
      ['GOOD', S.SENSE_MEDIUM_NOT_PRESENT, S.SENSE_MEDIUM_NOT_PRESENT, S.SENSE_MEDIUM_CHANGED, 'GOOD'])
    t('medium change clean', S.check_medium_change(obs, require_absent=True) == [], S.check_medium_change(obs))
    tg.medium_change()
    obs2 = [(r.status, r.sense) for r in (tg.execute(S.cdb_tur()), tg.execute(S.cdb_tur()))]
    t('medium_change(): UA only', S.check_medium_change(obs2) == [] and
      'medium_absent_not_seen' in rules(S.check_medium_change(obs2, require_absent=True)))
    t('medium change no ua', 'medium_change_no_ua' in rules(S.check_medium_change([obs[1], (S.GOOD, None)])))
    t('medium change not seen', 'medium_change_not_seen' in rules(S.check_medium_change([(S.GOOD, None)] * 3)))
    t('tur bad sense', 'tur_sense' in rules(S.check_medium_change([(S.CHECK_CONDITION, S.SENSE_LBA_OUT_OF_RANGE), obs[3]])))
    t('tur no sense', 'tur_sense' in rules(S.check_medium_change([(S.CHECK_CONDITION, None), obs[3]])))
    noua = S.Target(S.BlockDevice(10), unit_attention=False)
    noua.medium_change()
    t('model without UA', noua.execute(S.cdb_tur()).status == S.GOOD)
    # -- compare_reply: the model as a differential oracle
    m = S.Target(S.BlockDevice(100))
    t('compare equal', S.compare_reply(m, S.cdb_tur(), (S.GOOD, b'')) == [])
    t('compare status', 'reply_status' in rules(S.compare_reply(m, S.cdb_read10(200, 1), (S.GOOD, b''))))
    t('compare sense', 'reply_sense' in rules(S.compare_reply(m, S.cdb_read10(200, 1),
                                                              (S.CHECK_CONDITION, b'', S.SENSE_INVALID_FIELD_IN_CDB))))
    t('compare data', 'reply_data' in rules(S.compare_reply(m, S.cdb_read10(0, 1), (S.GOOD, bytes(511)))))
    t('compare sense bytes', S.compare_reply(m, S.cdb_read10(200, 1), (S.CHECK_CONDITION, b'',
                                             S.Target(S.BlockDevice(100))._fail(S.SENSE_LBA_OUT_OF_RANGE).sense)) == [])
    t('sense_triple other', S.sense_triple(b'\x72\x00') is None and S.sense_triple(None) is None)

    # -- DevicePropDesc (S-16.058)
    dp = M.encode_device_prop_desc(0x5011, M.DT_STR, 1, '', '20261027T120000')
    v, f = M.decode_device_prop_desc(dp)
    t('dpd datetime clean', f == [] and v['CurrentValue'].text == '20261027T120000' and v['Form'] is None, f)
    v, f = M.decode_device_prop_desc(M.encode_device_prop_desc(0x5011, M.DT_STR, 1, '', ''))
    t('dpd empty datetime one byte', f == [] and len(M.encode_device_prop_desc(0x5011, M.DT_STR, 1, '', '')) == 8)
    four = struct.pack('<HHB', 0x5011, M.DT_STR, 1) + b'\0' + b'\0\0\0\0' + b'\0'
    t('dpd four zero bytes', 'trailing_bytes' in rules(M.decode_device_prop_desc(four)[1]))
    t('dpd datetime format', 'datetime_format' in rules(M.decode_device_prop_desc(
        M.encode_device_prop_desc(0x5011, M.DT_STR, 1, '', '2026-10-27'))[1]))
    t('dpd datatype', 'prop_datatype' in rules(M.decode_device_prop_desc(
        M.encode_device_prop_desc(0x5011, M.DT_UINT32, 1, 0, 0))[1]))
    rng_ok = M.encode_device_prop_desc(0x5001, M.DT_UINT8, 0, 100, 50, M.FORM_RANGE, {'min': 0, 'max': 100, 'step': 10})
    v, f = M.decode_device_prop_desc(rng_ok)
    t('dpd range clean', f == [] and v['Form'] == {'min': 0, 'max': 100, 'step': 10}, f)
    t('dpd range value', 'value_in_form' in rules(M.decode_device_prop_desc(M.encode_device_prop_desc(
        0x5001, M.DT_UINT8, 0, 100, 55, M.FORM_RANGE, {'min': 0, 'max': 100, 'step': 10}))[1]))
    t('dpd range bad', 'form_range' in rules(M.decode_device_prop_desc(M.encode_device_prop_desc(
        0x5001, M.DT_UINT8, 0, 1, 1, M.FORM_RANGE, {'min': 9, 'max': 1, 'step': 1}))[1]))
    t('dpd range over string', 'form_datatype' in rules(M.decode_device_prop_desc(M.encode_device_prop_desc(
        0xD402, M.DT_STR, 1, 'a', 'b', M.FORM_RANGE, {'min': 'a', 'max': 'z', 'step': 'b'}))[1]))
    en = M.encode_device_prop_desc(0xD402, M.DT_STR, 1, 'A', 'B', M.FORM_ENUM, {'values': ['A', 'B']})
    t('dpd enum clean', M.decode_device_prop_desc(en)[1] == [])
    t('dpd enum value', 'value_in_form' in rules(M.decode_device_prop_desc(M.encode_device_prop_desc(
        0xD402, M.DT_STR, 1, 'A', 'C', M.FORM_ENUM, {'values': ['A', 'B']}))[1]))
    t('dpd enum empty', 'form_enum' in rules(M.decode_device_prop_desc(M.encode_device_prop_desc(
        0xD402, M.DT_STR, 1, 'A', 'A', M.FORM_ENUM, {'values': []}))[1]))
    t('dpd getset', 'getset' in rules(M.decode_device_prop_desc(M.encode_device_prop_desc(0xD402, M.DT_STR, 2, 'a', 'a'))[1]))
    t('dpd unknown code', 'unknown_code' in rules(M.decode_device_prop_desc(M.encode_device_prop_desc(0x5099, M.DT_UINT8, 0, 0, 0))[1]))
    t('dpd vendor code ok', M.decode_device_prop_desc(M.encode_device_prop_desc(0xD5FF, M.DT_UINT8, 0, 0, 0))[1] == [])
    t('dpd form flag', 'form_flag' in rules(M.decode_device_prop_desc(rng_ok[:-3][:-1] + b'\x07\x01\x02')[1]))
    v, f = M.decode_device_prop_desc(M.encode_device_prop_desc(0xD5F0, 0x0009, 0, -5, 1 << 100))
    t('dpd int128', f == [] and v['FactoryDefaultValue'] == -5 and v['CurrentValue'] == 1 << 100)
    v, f = M.decode_device_prop_desc(M.encode_device_prop_desc(0xD5F1, 0x400A, 0, [1, 2], []))
    t('dpd au128', f == [] and v['FactoryDefaultValue'] == [1, 2])
    for cut in range(len(dp)):
        if not raises(M.decode_device_prop_desc, dp[:cut], exc=M.MalformedError):
            t(f'dpd cut {cut} raises', False)
            break
    else:
        t('dpd every cut raises', True)
    t('dpd unknown datatype raises', raises(M.decode_device_prop_desc, struct.pack('<HHB', 0x5011, 0x0000, 0) + bytes(4),
                                            exc=M.MalformedError))
    # -- ObjectPropList (S-16.057)
    long_name = 'a-file-name-well-over-eighteen-characters.wav'
    els = [(5, 0xDC07, M.DT_STR, long_name), (5, 0xDC04, M.DT_UINT64, 1 << 33), (5, 0xDC01, M.DT_UINT32, 0x00010001),
           (5, 0xDC09, M.DT_STR, '20261027T101500'), (5, 0xDC41, M.DT_UINT128, 0x1234 << 64), (6, 0xDC07, M.DT_STR, 'x')]
    pl = M.encode_object_prop_list(els)
    got, f = M.decode_object_prop_list(pl, handles={5, 6})
    t('opl clean', f == [] and got[0].value.text == long_name and got[4].value == 0x1234 << 64, f)
    t('opl empty', M.decode_object_prop_list(M.encode_object_prop_list([]))[1] == [])
    for e, kw, rule in (((5, 0xDC07, M.DT_UINT16, 1), {}, 'prop_datatype'), ((5, 0x1234, M.DT_UINT8, 0), {}, 'unknown_code'),
                        ((0, 0xDC07, M.DT_STR, 'a'), {}, 'reserved_handle'), ((7, 0xDC07, M.DT_STR, 'a'), {'handles': {5}}, 'element_handle'),
                        ((5, 0xDC04, M.DT_UINT64, 1), {'prop_code': 0xDC07}, 'element_prop'),
                        ((5, 0xDC07, M.DT_STR, ''), {}, 'filename'), ((5, 0xDC08, M.DT_STR, '2026'), {}, 'datetime_format'),
                        ((5, 0xDC01, M.DT_UINT32, 0x00010000), {}, 'storage_id'),
                        ((5, 0xDC07, M.DT_STR, M.MtpString([0x61])), {}, 'string_nul')):
        fr = rules(M.decode_object_prop_list(M.encode_object_prop_list([e]), **kw)[1])
        t(f'opl rule {rule}', rule in fr, fr)
    t('opl duplicate', 'duplicate_element' in rules(M.decode_object_prop_list(M.encode_object_prop_list([els[0], els[0]]))[1]))
    t('opl all props', M.decode_object_prop_list(pl, prop_code=M.ALL_PROPS)[1] == [])
    t('opl trailing', 'trailing_bytes' in rules(M.decode_object_prop_list(pl + b'\0')[1]))
    over = bytearray(pl); over[0] += 1
    t('opl count over raises', raises(M.decode_object_prop_list, bytes(over), exc=M.MalformedError))
    t('opl huge count raises', raises(M.decode_object_prop_list, b'\xff\xff\xff\xff', exc=M.MalformedError))
    t('opl cut raises', all(raises(M.decode_object_prop_list, pl[:c], exc=M.MalformedError) for c in range(len(pl))))
    A = M.allowed_get_object_prop_list_responses
    t('opl responses', A(True, True) == {0x2001} and A(True, False) == {0xA801} and A(False, True) == {0x2009}
      and A(False, False) == {0x2009, 0xA801})
    c = M.encode_container(3, 0xA801, 9)
    t('opl response check', M.check_response(c, tid=9, want_code=min(A(True, False)))[1] == [])


def test_robust_commands():
    """Random CDBs with implemented opcodes through the model, BOT and the differential oracle, and
    byte mutations of valid VPD, DevicePropDesc and ObjectPropList encodings: never a crash."""
    import random
    rng = random.Random(11)
    ops = [S.TEST_UNIT_READY, S.REQUEST_SENSE, S.INQUIRY, S.MODE_SENSE6, S.READ_CAPACITY10, S.READ6, S.WRITE6,
           S.READ10, S.WRITE10]
    naa = S.designation_descriptor(S.DESIG_NAA, bytes([0x50]) + bytes(7))
    seeds = [S.build_vpd83([naa, S.designation_descriptor(S.DESIG_T10, b'VENDORXXSN')]),
             M.encode_device_prop_desc(0x5011, M.DT_STR, 1, '', '20261027T120000'),
             M.encode_device_prop_desc(0x5001, M.DT_UINT8, 0, 5, 5, M.FORM_RANGE, {'min': 0, 'max': 9, 'step': 1}),
             M.encode_device_prop_desc(0xD402, M.DT_STR, 1, 'A', 'B', M.FORM_ENUM, {'values': ['A', 'B']}),
             M.encode_object_prop_list([(5, 0xDC07, M.DT_STR, 'name.wav'), (5, 0xDC04, M.DT_UINT64, 9),
                                        (5, 0xDC41, M.DT_UINT128, 3)])]
    decoders = [lambda b: S.decode_vpd83(b, rng.choice([None, len(b), 255])), M.decode_device_prop_desc,
                M.decode_device_prop_desc, M.decode_device_prop_desc, M.decode_object_prop_list]
    crashes, seen_status = [], set()
    for i in range(3000):
        nb = rng.choice([1, 8, 300])
        tg = S.Target(S.BlockDevice(nb), mode_pages={0x08: bytes([8, 2, 0, 0])}, vpd={0x83: seeds[0][4:]},
                      max_lun=rng.randrange(2))
        if rng.random() < .3:
            tg.set_present(rng.random() < .5)
        op = rng.choice(ops + [rng.randrange(256)])
        cdb = bytes([op]) + rng.randbytes(rng.randrange(16))
        out = bytes(nb * 512) if rng.random() < .7 else rng.randbytes(rng.randrange(1024))
        try:
            r = tg.execute(cdb, out, rng.randrange(2))
            seen_status.add(r.status)
            S.compare_reply(S.Target(S.BlockDevice(nb)), cdb, (r.status, r.data_in, r.sense), out)
            if 1 <= len(cdb) <= 16:
                cbw = S.encode_cbw(i, rng.choice([0, 6, 512, nb * 512, 70000]), rng.random() < .5, rng.randrange(2), cdb)
                S.run_bot(tg, cbw, out)
            S.check_medium_change([(r.status, r.sense)] * rng.randrange(3))
        except (S.MalformedError, M.MalformedError):
            pass
        except Exception as e:
            crashes.append(f'cmd {cdb.hex()}: {type(e).__name__}: {e}')
        k = rng.randrange(len(seeds))
        b = bytearray(seeds[k])
        for _ in range(rng.randrange(1, 4)):
            act = rng.random()
            if act < .5 and b:
                b[rng.randrange(len(b))] = rng.randrange(256)
            elif act < .75:
                b = b[:rng.randrange(len(b) + 1)]
            else:
                b += rng.randbytes(rng.randrange(1, 5))
        try:
            decoders[k](bytes(b))
        except (S.MalformedError, M.MalformedError):
            pass
        except Exception as e:
            crashes.append(f'decoder {k} {bytes(b).hex()}: {type(e).__name__}: {e}')
    t('robust commands: no crashes', not crashes, crashes[:3])
    t('robust commands: both outcomes reached', seen_status == {S.GOOD, S.CHECK_CONDITION}, seen_status)


def test_robust():
    """Random input never crashes a checker: it is either MalformedError or a list of findings."""
    import random
    rng = random.Random(7)
    crashes = []
    for i in range(3000):
        cls, sub = rng.choice([(1, 1), (1, 2), (1, 3), (2, 2), (0xA, 0), (8, 6), (6, 1), (0xFF, 0)])
        body = bytearray()
        for _ in range(rng.randrange(4)):
            n = rng.randrange(3, 12)
            body += bytes([n, 0x24, rng.randrange(10)]) + rng.randbytes(n - 3)
        nep, eps = rng.randrange(3), b''
        for _ in range(nep):
            eps += D.endpoint_bytes(rng.choice([1, 0x81, 2, 0x82]), rng.randrange(4), rng.choice([8, 64, 512, 1024]),
                                    rng.randrange(20), audio=rng.random() < .5)
            if rng.random() < .6:
                n = rng.randrange(3, 9)
                eps += bytes([n, 0x25, rng.randrange(3)]) + rng.randbytes(n - 3)
        data = D.build_configuration({'bNumInterfaces': 1}, D.interface_bytes(0, 0, nep, cls, sub, rng.randrange(3)) +
                                     bytes(body) + eps)
        if rng.random() < .3:
            data = bytearray(data)
            data[rng.randrange(len(data))] = rng.randrange(256)
            data = bytes(data)
        for fn in (lambda: D.check_configuration(D.parse_configuration(data), rng.choice(['high', 'full'])),
                   lambda: M.decode_container(data), lambda: M.decode_device_info(data[9:]),
                   lambda: M.decode_storage_info(data[9:]), lambda: M.decode_object_info(data[9:]),
                   lambda: M.decode_handles(data[9:]), lambda: S.decode_cbw(data[:31]),
                   lambda: S.check_inquiry(data[:36], 36), lambda: S.check_request_sense(data[:18], 18),
                   lambda: S.check_mode_sense6(data[:12], 0xFF),
                   lambda: S.decode_vpd83(data[5:], rng.choice([None, 4, 16, 255])),
                   lambda: S.decode_vpd83(bytes([0, 0x83, 0, len(data) - 9]) + data[9:], 255),
                   lambda: S.decode_vpd00(data[5:], 255),
                   lambda: S.check_vpd_offer(data[5:20], {0x83: (rng.choice([0, 2]), data[3:9], data[:18])}),
                   lambda: S.rw_lba_length(data[:rng.randrange(12)]),
                   lambda: S.Target(S.BlockDevice(64)).execute(data[:rng.randrange(17)], bytes(64 * 512)),
                   lambda: S.check_medium_change([(data[i] & 2, data[i:i + 18]) for i in range(0, min(30, len(data)), 6)]),
                   lambda: S.compare_reply(S.Target(S.BlockDevice(8)), data[:10], (data[0] & 2, data[10:20], data[:18])),
                   lambda: M.decode_device_prop_desc(data[9:]),
                   lambda: M.decode_device_prop_desc(struct.pack('<HHB', 0x5011, rng.choice(list(M.DATATYPE_TAGS)), 1) + data[9:]),
                   lambda: M.decode_object_prop_list(data[9:]),
                   lambda: M.decode_object_prop_list(bytes([rng.randrange(4), 0, 0, 0]) + data[9:])):
            try:
                fn()
            except (D.MalformedError, M.MalformedError, S.MalformedError):
                pass
            except Exception as e:
                crashes.append(f'{type(e).__name__}: {e}')
    t('robust: no crashes on random input', not crashes, crashes[:3])


def main():
    for fn in (test_descriptors, test_mtp, test_scsi, test_integration, test_robust_commands, test_robust):
        try:
            fn()
        except Exception as e:                   # a crash is a failure of that group, not of the run
            FAIL.append(f'{fn.__name__} raised {type(e).__name__}: {e}')
    print(f'usb selftest: {PASS[0]} passed, {len(FAIL)} failed')
    for x in FAIL[:20]:
        print('  FAIL', x)
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
