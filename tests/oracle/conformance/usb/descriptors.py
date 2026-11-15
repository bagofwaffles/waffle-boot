# SPDX-License-Identifier: GPL-2.0-or-later
"""USB descriptor byte models (USB 2.0 ch. 9, IAD ECN, CDC 1.2, MSC BOT 1.0, Audio 1.0, USB-MIDI 1.0,
Still Image 1.0). A broken bLength chain or tree order raises MalformedError; check_* return a list of
Finding(rule, where, msg), empty when conformant."""
import struct
from collections import namedtuple

DEVICE, CONFIGURATION, STRING, INTERFACE, ENDPOINT = 1, 2, 3, 4, 5
DEVICE_QUALIFIER, OTHER_SPEED_CONFIGURATION, INTERFACE_POWER, OTG, DEBUG, IAD = 6, 7, 8, 9, 10, 11
BOS, DEVICE_CAPABILITY = 15, 16
CS_INTERFACE, CS_ENDPOINT = 0x24, 0x25

CLASS_AUDIO, CLASS_CDC, CLASS_HID, CLASS_STILL_IMAGE, CLASS_MSC = 0x01, 0x02, 0x03, 0x06, 0x08
CLASS_CDC_DATA, CLASS_MISC, CLASS_VENDOR = 0x0A, 0xEF, 0xFF

XFER_CONTROL, XFER_ISO, XFER_BULK, XFER_INTERRUPT = 0, 1, 2, 3

Finding = namedtuple('Finding', 'rule where msg')


class MalformedError(ValueError):
    """The bytes are not a well-formed descriptor (set)."""


Descriptor = namedtuple('Descriptor', 'offset length type subtype raw partial')


def split(data, allow_truncated=False):
    """Walk the bLength chain. Returns a list of Descriptor. A descriptor cut short by the end of the
    buffer is an error, unless allow_truncated (a reply cut to wLength), when it is returned with
    partial=True and ends the list."""
    data = bytes(data)
    out, off = [], 0
    while off < len(data):
        if len(data) - off < 2:
            if allow_truncated:
                out.append(Descriptor(off, data[off], None, None, data[off:], True))
                break
            raise MalformedError(f'@{off}: one stray byte after the last descriptor')
        ln, ty = data[off], data[off + 1]
        if ln < 2:
            raise MalformedError(f'@{off}: bLength {ln} < 2')
        if off + ln > len(data):
            if allow_truncated:
                raw = data[off:]
                out.append(Descriptor(off, ln, ty, raw[2] if len(raw) > 2 else None, raw, True))
                break
            raise MalformedError(f'@{off}: bLength {ln} runs past the end ({len(data) - off} bytes left)')
        raw = data[off:off + ln]
        out.append(Descriptor(off, ln, ty, raw[2] if ln > 2 else None, raw, False))
        off += ln
    return out


def _need(raw, ty, minlen, name, exact=None):
    if len(raw) < 2 or raw[1] != ty:
        raise MalformedError(f'{name}: bDescriptorType {raw[1] if len(raw) > 1 else None} != {ty}')
    if raw[0] != len(raw):
        raise MalformedError(f'{name}: bLength {raw[0]} != {len(raw)} bytes given')
    if exact is not None and raw[0] not in exact:
        raise MalformedError(f'{name}: bLength {raw[0]} not in {sorted(exact)}')
    if raw[0] < minlen:
        raise MalformedError(f'{name}: bLength {raw[0]} < {minlen}')


# ------------------------------------------------------------------------------ standard descriptors
DEVICE_FIELDS = ('bLength', 'bDescriptorType', 'bcdUSB', 'bDeviceClass', 'bDeviceSubClass', 'bDeviceProtocol',
                 'bMaxPacketSize0', 'idVendor', 'idProduct', 'bcdDevice', 'iManufacturer', 'iProduct',
                 'iSerialNumber', 'bNumConfigurations')
QUALIFIER_FIELDS = ('bLength', 'bDescriptorType', 'bcdUSB', 'bDeviceClass', 'bDeviceSubClass', 'bDeviceProtocol',
                    'bMaxPacketSize0', 'bNumConfigurations', 'bReserved')
CONFIG_FIELDS = ('bLength', 'bDescriptorType', 'wTotalLength', 'bNumInterfaces', 'bConfigurationValue',
                 'iConfiguration', 'bmAttributes', 'bMaxPower')
INTERFACE_FIELDS = ('bLength', 'bDescriptorType', 'bInterfaceNumber', 'bAlternateSetting', 'bNumEndpoints',
                    'bInterfaceClass', 'bInterfaceSubClass', 'bInterfaceProtocol', 'iInterface')
ENDPOINT_FIELDS = ('bLength', 'bDescriptorType', 'bEndpointAddress', 'bmAttributes', 'wMaxPacketSize', 'bInterval')
IAD_FIELDS = ('bLength', 'bDescriptorType', 'bFirstInterface', 'bInterfaceCount', 'bFunctionClass',
              'bFunctionSubClass', 'bFunctionProtocol', 'iFunction')


def parse_device(raw):
    raw = bytes(raw)
    _need(raw, DEVICE, 18, 'device', exact={18})
    return dict(zip(DEVICE_FIELDS, struct.unpack('<BBHBBBBHHHBBBB', raw)))


def build_device(**f):
    d = dict(bLength=18, bDescriptorType=DEVICE, bcdUSB=0x0200, bDeviceClass=0, bDeviceSubClass=0, bDeviceProtocol=0,
             bMaxPacketSize0=64, idVendor=0, idProduct=0, bcdDevice=0, iManufacturer=0, iProduct=0, iSerialNumber=0,
             bNumConfigurations=1)
    d.update(f)
    return struct.pack('<BBHBBBBHHHBBBB', *(d[k] for k in DEVICE_FIELDS))


def parse_qualifier(raw):
    raw = bytes(raw)
    _need(raw, DEVICE_QUALIFIER, 10, 'device qualifier', exact={10})
    return dict(zip(QUALIFIER_FIELDS, struct.unpack('<BBHBBBBBB', raw)))


def parse_config_header(raw):
    raw = bytes(raw)
    if len(raw) < 2 or raw[1] not in (CONFIGURATION, OTHER_SPEED_CONFIGURATION):
        raise MalformedError(f'configuration: bDescriptorType {raw[1] if len(raw) > 1 else None} not 2 or 7')
    _need(raw, raw[1], 9, 'configuration', exact={9})
    return dict(zip(CONFIG_FIELDS, struct.unpack('<BBHBBBBB', raw)))


def parse_interface(raw):
    raw = bytes(raw)
    _need(raw, INTERFACE, 9, 'interface', exact={9})
    return dict(zip(INTERFACE_FIELDS, struct.unpack('<BBBBBBBBB', raw)))


def parse_endpoint(raw):
    """7 bytes (USB 2.0 9.6.6) or 9 (Audio 1.0 4.6.1.1 / MIDI 1.0 6.2.1: bRefresh, bSynchAddress)."""
    raw = bytes(raw)
    _need(raw, ENDPOINT, 7, 'endpoint', exact={7, 9})
    d = dict(zip(ENDPOINT_FIELDS, struct.unpack('<BBBBHB', raw[:7])))
    if len(raw) == 9:
        d['bRefresh'], d['bSynchAddress'] = raw[7], raw[8]
    return d


def parse_iad(raw):
    raw = bytes(raw)
    _need(raw, IAD, 8, 'interface association', exact={8})
    return dict(zip(IAD_FIELDS, raw))


def parse_string(raw, langids=False):
    """String descriptor (9.6.7): UTF-16LE text, or (index 0) the LANGID array."""
    raw = bytes(raw)
    _need(raw, STRING, 2, 'string')
    if (len(raw) - 2) % 2:
        raise MalformedError(f'string: odd payload length {len(raw) - 2}')
    if langids:
        return list(struct.unpack(f'<{(len(raw) - 2) // 2}H', raw[2:]))
    return raw[2:].decode('utf-16-le')


def build_string(text):
    b = text.encode('utf-16-le')
    if len(b) + 2 > 255:
        raise ValueError('string descriptor longer than 255 bytes')
    return bytes([len(b) + 2, STRING]) + b


def ep_number(addr):
    return addr & 0x0F


def ep_is_in(addr):
    return bool(addr & 0x80)


def ep_type(attr):
    return attr & 3


def mps_decode(w):
    """(bytes per transaction, additional transactions per microframe) of wMaxPacketSize (table 9-13)."""
    return w & 0x7FF, (w >> 11) & 3


# ------------------------------------------------------------------------------ configuration tree
class Endpoint:
    def __init__(self, desc):
        self.desc = desc
        self.fields = parse_endpoint(desc.raw)
        self.cs = []                   # class-specific endpoint descriptors (Descriptor)


class Alternate:
    def __init__(self, desc):
        self.desc = desc
        self.fields = parse_interface(desc.raw)
        self.cs = []                   # class-specific interface descriptors
        self.endpoints = []
        self.iad = None                # the IAD that immediately preceded this interface, if any
        self.complete = True           # False: cut short by a truncated reply

    number = property(lambda s: s.fields['bInterfaceNumber'])
    alt = property(lambda s: s.fields['bAlternateSetting'])
    cls = property(lambda s: (s.fields['bInterfaceClass'], s.fields['bInterfaceSubClass'],
                              s.fields['bInterfaceProtocol']))


class Configuration:
    def __init__(self):
        self.header = None
        self.fields = None
        self.alternates = []           # in wire order
        self.iads = []
        self.extra = []                # descriptors before the first interface other than IADs (OTG, vendor)
        self.descriptors = []
        self.truncated = False
        self.length = 0

    def interfaces(self):
        """{bInterfaceNumber: [Alternate, ...]} in wire order."""
        out = {}
        for a in self.alternates:
            out.setdefault(a.number, []).append(a)
        return out

    def endpoints(self):
        return [e for a in self.alternates for e in a.endpoints]


def parse_configuration(data, wlength=None):
    """Structural parse of a GET_DESCRIPTOR(CONFIGURATION or OTHER_SPEED_CONFIGURATION) reply.
    wlength: the request's wLength; when the reply is exactly wlength bytes and shorter than
    wTotalLength, the reply is a legal truncation (9.4.3) and the last descriptor may be partial."""
    data = bytes(data)
    if len(data) < 2:
        raise MalformedError('configuration: fewer than 2 bytes')
    trunc_ok = wlength is not None and len(data) == wlength
    descs = split(data, allow_truncated=trunc_ok)
    cfg = Configuration()
    cfg.descriptors = descs
    cfg.length = len(data)
    first = descs[0]
    if first.partial:
        if first.type is not None and first.type not in (CONFIGURATION, OTHER_SPEED_CONFIGURATION):
            raise MalformedError(f'configuration: first descriptor type {first.type}')
        cfg.truncated = True
        return cfg
    cfg.header = first
    cfg.fields = parse_config_header(first.raw)
    if trunc_ok and cfg.fields['wTotalLength'] > len(data):
        cfg.truncated = True
    alt, ep, pending_iad = None, None, None
    for d in descs[1:]:
        if d.partial:
            cfg.truncated = True
            if alt is not None:
                alt.complete = False
            break
        if d.type == INTERFACE:
            if alt is not None:
                _close_alt(alt)
            alt = Alternate(d)
            alt.iad, pending_iad = pending_iad, None
            cfg.alternates.append(alt)
            ep = None
        elif d.type == IAD:
            if pending_iad is not None:
                raise MalformedError(f'@{d.offset}: two IADs with no interface between them')
            pending_iad = parse_iad(d.raw)
            cfg.iads.append((d, pending_iad))
        elif d.type == ENDPOINT:
            if alt is None:
                raise MalformedError(f'@{d.offset}: endpoint descriptor before any interface')
            ep = Endpoint(d)
            alt.endpoints.append(ep)
        elif d.type == CS_ENDPOINT:
            if ep is None:
                raise MalformedError(f'@{d.offset}: class-specific endpoint descriptor not after an endpoint')
            ep.cs.append(d)
        elif d.type == CS_INTERFACE:
            if alt is None:
                raise MalformedError(f'@{d.offset}: class-specific interface descriptor before any interface')
            if ep is not None:
                raise MalformedError(f'@{d.offset}: class-specific interface descriptor after an endpoint')
            alt.cs.append(d)
        elif d.type in (CONFIGURATION, OTHER_SPEED_CONFIGURATION, DEVICE, DEVICE_QUALIFIER, STRING):
            raise MalformedError(f'@{d.offset}: descriptor type {d.type} inside a configuration')
        else:
            if alt is None:
                cfg.extra.append(d)
            elif ep is not None:
                ep.cs.append(d)
            else:
                alt.cs.append(d)
    if pending_iad is not None and not cfg.truncated:
        raise MalformedError('IAD at the end of the configuration with no interface after it')
    if alt is not None and not cfg.truncated:
        _close_alt(alt)
    return cfg


def _close_alt(alt):
    n = alt.fields['bNumEndpoints']
    if len(alt.endpoints) != n:
        raise MalformedError(f'@{alt.desc.offset}: interface {alt.number} alt {alt.alt} declares bNumEndpoints {n}, '
                             f'{len(alt.endpoints)} endpoint descriptors follow')


def build_configuration(header_fields, body, other_speed=False):
    """Assemble a configuration: the 9-byte header with wTotalLength computed, then body (bytes)."""
    f = dict(bLength=9, bDescriptorType=OTHER_SPEED_CONFIGURATION if other_speed else CONFIGURATION,
             wTotalLength=9 + len(body), bNumInterfaces=0, bConfigurationValue=1, iConfiguration=0,
             bmAttributes=0x80, bMaxPower=50)
    f.update(header_fields)
    f['wTotalLength'] = 9 + len(body)
    return struct.pack('<BBHBBBBB', *(f[k] for k in CONFIG_FIELDS)) + bytes(body)


def interface_bytes(number, alt, nep, cls, sub, proto, istr=0):
    return struct.pack('<BBBBBBBBB', 9, INTERFACE, number, alt, nep, cls, sub, proto, istr)


def endpoint_bytes(addr, attr, mps, interval, audio=False, refresh=0, synch=0):
    if audio:
        return struct.pack('<BBBBHBBB', 9, ENDPOINT, addr, attr, mps, interval, refresh, synch)
    return struct.pack('<BBBBHB', 7, ENDPOINT, addr, attr, mps, interval)


def iad_bytes(first, count, cls, sub, proto, istr=0):
    return struct.pack('<BBBBBBBB', 8, IAD, first, count, cls, sub, proto, istr)


# ------------------------------------------------------------------------------ semantic rules
def check_device(raw, speed='high'):
    d = parse_device(raw)
    out = []
    if d['bcdUSB'] not in (0x0100, 0x0110, 0x0200, 0x0201, 0x0210):
        out.append(Finding('bcd_usb', 'device', f'bcdUSB {d["bcdUSB"]:#06x}'))
    if d['bMaxPacketSize0'] not in (8, 16, 32, 64):
        out.append(Finding('ep0_mps', 'device', f'bMaxPacketSize0 {d["bMaxPacketSize0"]}'))
    elif speed == 'high' and d['bMaxPacketSize0'] != 64:
        out.append(Finding('ep0_mps', 'device', 'high speed requires bMaxPacketSize0 64 (5.5.3)'))
    if d['bNumConfigurations'] < 1:
        out.append(Finding('num_configs', 'device', 'bNumConfigurations 0'))
    if d['bDeviceClass'] == CLASS_MISC and (d['bDeviceSubClass'], d['bDeviceProtocol']) != (2, 1):
        out.append(Finding('iad_class', 'device', 'class 0xEF needs subclass 2 protocol 1 (IAD ECN)'))
    return out


def check_qualifier(raw, device_raw=None):
    q = parse_qualifier(raw)
    out = []
    if q['bcdUSB'] < 0x0200:
        out.append(Finding('bcd_usb', 'qualifier', f'bcdUSB {q["bcdUSB"]:#06x} < 2.00'))
    if q['bMaxPacketSize0'] not in (8, 16, 32, 64):
        out.append(Finding('ep0_mps', 'qualifier', f'bMaxPacketSize0 {q["bMaxPacketSize0"]}'))
    if q['bReserved'] != 0:
        out.append(Finding('reserved', 'qualifier', 'bReserved not zero'))
    if device_raw is not None:
        d = parse_device(device_raw)
        for k in ('bcdUSB', 'bNumConfigurations'):
            if q[k] != d[k]:
                out.append(Finding('qualifier_match', 'qualifier', f'{k} {q[k]} != device {d[k]}'))
    return out


def check_endpoint(fields, speed='high', where='endpoint'):
    out = []
    a, at, w, iv = fields['bEndpointAddress'], fields['bmAttributes'], fields['wMaxPacketSize'], fields['bInterval']
    if a & 0x70:
        out.append(Finding('ep_address', where, f'reserved address bits set ({a:#04x})'))
    if ep_number(a) == 0:
        out.append(Finding('ep_address', where, 'endpoint 0 described in a configuration'))
    t = ep_type(at)
    if t == XFER_CONTROL:
        out.append(Finding('ep_attributes', where, 'control endpoints other than 0 are not supported by this model'))
    if at & 0xC0 or (t != XFER_ISO and at & 0x3C):
        out.append(Finding('ep_attributes', where, f'reserved bmAttributes bits ({at:#04x})'))
    if w & 0xE000:
        out.append(Finding('ep_mps', where, f'reserved wMaxPacketSize bits ({w:#06x})'))
    size, extra = mps_decode(w)
    if extra:
        if speed != 'high' or t not in (XFER_ISO, XFER_INTERRUPT):
            out.append(Finding('ep_high_bandwidth', where, 'additional transactions only for high-speed periodic'))
        elif extra == 3 or not ((extra == 1 and 513 <= size <= 1024) or (extra == 2 and 683 <= size <= 1024)):
            out.append(Finding('ep_high_bandwidth', where, f'{size} bytes x {extra + 1} not allowed (table 9-14)'))
    if t == XFER_BULK:
        ok = (512,) if speed == 'high' else (8, 16, 32, 64)
        if w not in ok:
            out.append(Finding('bulk_mps', where, f'bulk wMaxPacketSize {w:#x} at {speed} speed (5.8.3)'))
    elif t == XFER_INTERRUPT:
        lim = 1024 if speed == 'high' else 64
        if size > lim or size == 0:
            out.append(Finding('int_mps', where, f'interrupt packet size {size} at {speed} speed (5.7.3)'))
        if (speed == 'high' and not 1 <= iv <= 16) or (speed != 'high' and iv == 0):
            out.append(Finding('ep_interval', where, f'interrupt bInterval {iv} at {speed} speed'))
    elif t == XFER_ISO:
        lim = 1024 if speed == 'high' else 1023
        if size > lim:
            out.append(Finding('iso_mps', where, f'isochronous packet size {size} at {speed} speed (5.6.3)'))
        if not 1 <= iv <= 16:
            out.append(Finding('ep_interval', where, f'isochronous bInterval {iv}'))
    return out


def check_configuration(cfg, speed='high', check_class=True):
    """Semantic rules on a parsed Configuration. speed: the speed this configuration is reported for
    (for OTHER_SPEED_CONFIGURATION: the other speed)."""
    out = []
    if cfg.fields is None:
        return out
    f = cfg.fields
    if not cfg.truncated and f['wTotalLength'] != cfg.length:
        out.append(Finding('total_length', 'configuration', f'wTotalLength {f["wTotalLength"]} != {cfg.length} bytes'))
    if cfg.truncated and f['wTotalLength'] < cfg.length:
        out.append(Finding('total_length', 'configuration', 'reply longer than wTotalLength'))
    if not f['bmAttributes'] & 0x80 or f['bmAttributes'] & 0x1F:
        out.append(Finding('config_attributes', 'configuration', f'bmAttributes {f["bmAttributes"]:#04x}'))
    if f['bConfigurationValue'] == 0:
        out.append(Finding('config_value', 'configuration', 'bConfigurationValue 0 means "not configured"'))
    ifs = cfg.interfaces()
    if not cfg.truncated:
        if len(ifs) != f['bNumInterfaces']:
            out.append(Finding('num_interfaces', 'configuration',
                               f'bNumInterfaces {f["bNumInterfaces"]}, {len(ifs)} interfaces described'))
        if sorted(ifs) != list(range(len(ifs))):
            out.append(Finding('interface_numbers', 'configuration', f'interface numbers {sorted(ifs)}'))
    seen = []
    for a in cfg.alternates:
        if seen and seen[-1] != a.number and a.number in seen:
            out.append(Finding('interface_order', f'if{a.number}', 'alternates of one interface not contiguous'))
        seen.append(a.number)
    for n, alts in ifs.items():
        if [a.alt for a in alts] != list(range(len(alts))):
            out.append(Finding('alt_numbers', f'if{n}', f'alternate settings {[a.alt for a in alts]}'))
    for d, iad in cfg.iads:
        nxt = [a for a in cfg.alternates if a.iad is iad]
        if not nxt or nxt[0].number != iad['bFirstInterface'] or nxt[0].alt != 0:
            out.append(Finding('iad_position', f'iad@{d.offset}', 'IAD does not precede its first interface'))
        if iad['bInterfaceCount'] == 0 or (not cfg.truncated and
                                           iad['bFirstInterface'] + iad['bInterfaceCount'] > f['bNumInterfaces']):
            out.append(Finding('iad_range', f'iad@{d.offset}',
                               f'interfaces {iad["bFirstInterface"]}+{iad["bInterfaceCount"]} of {f["bNumInterfaces"]}'))
    # duplicate endpoint addresses: within one alternate, and across the default (alt 0) settings
    alt0 = []
    for a in cfg.alternates:
        addrs = [e.fields['bEndpointAddress'] for e in a.endpoints]
        if len(set(addrs)) != len(addrs):
            out.append(Finding('dup_ep_addr', f'if{a.number}.{a.alt}', f'addresses {[hex(x) for x in addrs]}'))
        if a.alt == 0:
            alt0 += addrs
    if len(set(alt0)) != len(alt0):
        out.append(Finding('dup_ep_addr', 'configuration', 'one address in two interfaces (alternate 0)'))
    for a in cfg.alternates:
        for k, e in enumerate(a.endpoints):
            out += check_endpoint(e.fields, speed, f'if{a.number}.{a.alt}.ep{k}')
        if check_class and a.complete:
            out += check_class_interface(a, cfg)
    return out


# ------------------------------------------------------------------------------ class-specific
def _cs(a, sub):
    return [d for d in a.cs if d.type == CS_INTERFACE and d.subtype == sub]


def _len_rule(out, where, d, need, name, exact=False):
    if (d.length != need) if exact else (d.length < need):
        out.append(Finding('class_desc_length', where, f'{name} bLength {d.length}, needs {"" if exact else ">= "}{need}'))
        return False
    return True


def check_class_interface(a, cfg=None):
    """Class rules for one alternate. Unknown classes are not checked."""
    cls, sub, proto = a.cls
    w = f'if{a.number}.{a.alt}'
    out = []
    eps = [e.fields for e in a.endpoints]
    bulk_in = [e for e in eps if ep_type(e['bmAttributes']) == XFER_BULK and ep_is_in(e['bEndpointAddress'])]
    bulk_out = [e for e in eps if ep_type(e['bmAttributes']) == XFER_BULK and not ep_is_in(e['bEndpointAddress'])]
    int_in = [e for e in eps if ep_type(e['bmAttributes']) == XFER_INTERRUPT and ep_is_in(e['bEndpointAddress'])]
    if cls == CLASS_CDC:
        # CDC 1.2 5.2.3: header first, then the functional descriptors; ACM (PSTN 1.2 5.3)
        if sub == 0x02:
            if not a.cs or a.cs[0].type != CS_INTERFACE or a.cs[0].subtype != 0x00:
                out.append(Finding('cdc_header', w, 'first functional descriptor is not the Header'))
            else:
                _len_rule(out, w, a.cs[0], 5, 'CDC Header', exact=True)
            for d in a.cs:
                if d.type != CS_INTERFACE:
                    continue
                need = {0x00: 5, 0x01: 5, 0x02: 4, 0x06: 5}.get(d.subtype)
                if need:
                    _len_rule(out, w, d, need, f'CDC functional {d.subtype:#04x}', exact=d.subtype != 0x06)
            un = _cs(a, 0x06)
            if not un:
                out.append(Finding('cdc_union', w, 'no Union functional descriptor'))
            elif cfg is not None and un[0].length >= 5:
                ifs = cfg.interfaces()
                for i in un[0].raw[3:]:
                    if i not in ifs and not cfg.truncated:
                        out.append(Finding('cdc_union', w, f'Union names interface {i}, not in the configuration'))
            if len(int_in) != 1 or len(eps) != 1:
                out.append(Finding('cdc_endpoints', w, 'ACM communication interface needs one interrupt IN endpoint'))
    elif cls == CLASS_CDC_DATA:
        if a.alt == 0 and len(eps) == 0:
            pass                        # a zero-bandwidth default setting is allowed (CDC 1.2 3.8.2)
        elif len(bulk_in) != 1 or len(bulk_out) != 1:
            out.append(Finding('cdc_endpoints', w, 'data interface needs one bulk IN and one bulk OUT endpoint'))
    elif cls == CLASS_MSC:
        if sub != 0x06 or proto != 0x50:
            out.append(Finding('msc_protocol', w, f'subclass {sub:#04x} protocol {proto:#04x} (SCSI transparent, BOT: 06/50)'))
        if len(bulk_in) != 1 or len(bulk_out) != 1 or len(eps) != 2:
            out.append(Finding('msc_endpoints', w, 'BOT needs exactly one bulk IN and one bulk OUT endpoint (BOT 4.3)'))
        if a.cs:
            out.append(Finding('msc_class_desc', w, 'BOT defines no class-specific descriptors'))
    elif cls == CLASS_STILL_IMAGE:
        if (sub, proto) != (1, 1):
            out.append(Finding('ptp_protocol', w, f'subclass {sub} protocol {proto} (still image: 1/1)'))
        if len(bulk_in) != 1 or len(bulk_out) != 1 or len(int_in) != 1:
            out.append(Finding('ptp_endpoints', w, 'needs bulk IN, bulk OUT and interrupt IN (Still Image 1.0 4.3)'))
    elif cls == CLASS_AUDIO:
        out += _check_audio(a, w, cfg)
    return out


def _check_audio(a, w, cfg):
    cls, sub, proto = a.cls
    out = []
    if sub == 0x01:                                   # AudioControl (Audio 1.0 4.3.2)
        hd = _cs(a, 0x01)
        if not a.cs or a.cs[0].subtype != 0x01:
            out.append(Finding('audio_header', w, 'AudioControl: first class descriptor is not the Header'))
        if hd and _len_rule(out, w, hd[0], 8, 'AC Header'):
            bcd, total, n = struct.unpack_from('<HHB', hd[0].raw, 3)
            if hd[0].length != 8 + n:
                out.append(Finding('class_desc_length', w, f'AC Header bLength {hd[0].length} for {n} streaming interfaces'))
            cs_total = sum(d.length for d in a.cs if d.type == CS_INTERFACE)
            if total != cs_total:
                out.append(Finding('audio_total_length', w, f'AC wTotalLength {total} != {cs_total}'))
            if cfg is not None and not cfg.truncated:
                ifs = cfg.interfaces()
                for i in hd[0].raw[8:8 + n]:
                    if i not in ifs:
                        out.append(Finding('audio_collection', w, f'baInterfaceNr {i} not in the configuration'))
        for e in a.endpoints:
            if e.cs:
                out.append(Finding('audio_ep_desc', w, 'AudioControl endpoint with class descriptors'))
    elif sub == 0x03:                                 # MIDIStreaming (USB-MIDI 1.0 6.1, 6.2)
        hd = _cs(a, 0x01)
        if not a.cs or a.cs[0].subtype != 0x01:
            out.append(Finding('midi_header', w, 'MIDIStreaming: first class descriptor is not the MS Header'))
        jacks = {}
        for d in a.cs:
            if d.type != CS_INTERFACE:
                continue
            if d.subtype == 0x01:
                _len_rule(out, w, d, 7, 'MS Header', exact=True)
            elif d.subtype == 0x02:
                if _len_rule(out, w, d, 6, 'MIDI IN Jack', exact=True):
                    if d.raw[3] not in (1, 2):
                        out.append(Finding('midi_jack', w, f'IN jack type {d.raw[3]}'))
                    jacks[d.raw[4]] = d
            elif d.subtype == 0x03:
                if _len_rule(out, w, d, 7, 'MIDI OUT Jack'):
                    p = d.raw[5]
                    if d.length != 7 + 2 * p:
                        out.append(Finding('class_desc_length', w, f'MIDI OUT Jack bLength {d.length} for {p} pins'))
                    if d.raw[3] not in (1, 2):
                        out.append(Finding('midi_jack', w, f'OUT jack type {d.raw[3]}'))
                    jacks[d.raw[4]] = d
            elif d.subtype == 0x04:
                _len_rule(out, w, d, 10, 'MIDI Element')
            else:
                out.append(Finding('midi_subtype', w, f'CS_INTERFACE subtype {d.subtype:#04x}'))
        if len(jacks) != len([d for d in a.cs if d.type == CS_INTERFACE and d.subtype in (2, 3)]):
            out.append(Finding('midi_jack', w, 'duplicate bJackID'))
        for d in a.cs:
            if d.type == CS_INTERFACE and d.subtype == 0x03 and d.length >= 7 and d.length >= 7 + 2 * d.raw[5]:
                for k in range(d.raw[5]):
                    if d.raw[6 + 2 * k] not in jacks:
                        out.append(Finding('midi_jack', w, f'OUT jack source {d.raw[6 + 2 * k]} undefined'))
        if hd and hd[0].length >= 7:
            total = struct.unpack_from('<H', hd[0].raw, 5)[0]
            cs = sum(d.length for d in a.cs)
            eps = sum(e.desc.length + sum(c.length for c in e.cs) for e in a.endpoints)
            # USB-MIDI 1.0 6.1.2.2 and appendix B.4.3: with or without the endpoint descriptors
            if total not in (cs, cs + eps):
                out.append(Finding('midi_total_length', w, f'MS wTotalLength {total}, expected {cs} or {cs + eps}'))
        for e in a.endpoints:
            if ep_type(e.fields['bmAttributes']) not in (XFER_BULK, XFER_INTERRUPT):
                out.append(Finding('midi_ep_type', w, 'MIDIStreaming endpoints are bulk or interrupt'))
            gen = [c for c in e.cs if c.type == CS_ENDPOINT]
            if len(gen) != 1 or gen[0].subtype != 0x01:
                out.append(Finding('midi_ep_general', w, 'endpoint needs one CS_ENDPOINT MS_GENERAL descriptor'))
            elif gen[0].length < 4 or gen[0].length != 4 + gen[0].raw[3]:
                out.append(Finding('class_desc_length', w, f'MS_GENERAL bLength {gen[0].length}'))
            else:
                for j in gen[0].raw[4:]:
                    if j not in jacks:
                        out.append(Finding('midi_jack', w, f'endpoint names undefined embedded jack {j}'))
    elif sub == 0x02:                                 # AudioStreaming (Audio 1.0 4.5.2)
        for d in a.cs:
            if d.type == CS_INTERFACE and d.subtype == 0x01:
                _len_rule(out, w, d, 7, 'AS General', exact=True)
            elif d.type == CS_INTERFACE and d.subtype == 0x02:
                _len_rule(out, w, d, 8, 'Format Type')
        for e in a.endpoints:
            for c in e.cs:
                if c.type == CS_ENDPOINT and c.subtype == 0x01:
                    _len_rule(out, w, c, 7, 'AS Isochronous Endpoint', exact=True)
    else:
        out.append(Finding('audio_subclass', w, f'audio subclass {sub:#04x}'))
    return out


def tree_summary(cfg):
    """A plain-data view of the parse: [(ifnum, alt, nep, [ep addr...], n_cs_if, [n_cs_ep...]), ...]."""
    return [(a.number, a.alt, len(a.endpoints), [e.fields['bEndpointAddress'] for e in a.endpoints], len(a.cs),
             [len(e.cs) for e in a.endpoints]) for a in cfg.alternates]
