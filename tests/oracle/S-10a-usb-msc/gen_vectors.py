#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-10a vector generator: every expected descriptor reply, built with tests/oracle/conformance/usb, written
under vectors/.

    python3 -I tests/oracle/S-10a-usb-msc/gen_vectors.py          # rewrite vectors/*.json
    python3 -I tests/oracle/S-10a-usb-msc/gen_vectors.py --check  # exit 1 if a file differs

Synthetic content only. String indexes use one assignment, "canonical" (S-10a.005): each string field gets
the next free index, from 1, in descriptor order (device, then each configuration). Tests compare the text
an index resolves to (`normalize`), not the index.

The OP-1F's files (`descriptors_1f.json`, `scsi_1f.json`) are the same cases built with its profile values
(S-18.030: device.usb_pid, device.midi_pid, device.product_text, device.ramdisk_blocks).
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'conformance'))
sys.path.insert(0, HERE)
from usb import descriptors as D  # noqa: E402

import s10a_model as M  # noqa: E402  (same directory)

VID = 0x2367
BCD_DEVICE = 0x0257
MANUFACTURER = 'teenage engineering'
SERIAL = 'XY0A12BC'                      # synthetic serial (S-04.040)

# ---------------------------------------------------------------- S-10a.012 functions (speed independent)
# An endpoint: (address, type, period_ms or None); type 'bulk' or 'interrupt'. Packet sizes: S-10a.021.
INT_MPS = 16


def midi_function(first, product='OP-XY'):
    istr_ac, istr_ms = product, product + ' Midi'
    ac = first
    ms = first + 1
    ac_header = bytes([9, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, ms])
    ms_cs = [bytes([7, 0x24, 0x01, 0x00, 0x01, 0x25, 0x00]),
             bytes([6, 0x24, 0x02, 0x01, 0x15, 0x00]),
             bytes([6, 0x24, 0x02, 0x02, 0x16, 0x00]),
             bytes([9, 0x24, 0x03, 0x01, 0x17, 0x01, 0x16, 0x01, 0x00]),
             bytes([9, 0x24, 0x03, 0x02, 0x18, 0x01, 0x15, 0x01, 0x00])]
    return [
        dict(number=ac, cls=(0x01, 0x01, 0x00), istr=istr_ac, cs=[ac_header], eps=[]),
        dict(number=ms, cls=(0x01, 0x03, 0x00), istr=istr_ms, cs=ms_cs, audio_eps=True,
             eps=[(0x08, 'bulk', None, bytes([5, 0x25, 0x01, 0x01, 0x15])),
                  (0x87, 'bulk', None, bytes([5, 0x25, 0x01, 0x01, 0x17]))]),
    ]


def cdc_function(first):
    comm, data = first, first + 1
    cs = [bytes([5, 0x24, 0x00, 0x00, 0x02]),            # Header, bcdCDC 0x0200
          bytes([5, 0x24, 0x06, comm, data]),            # Union
          bytes([5, 0x24, 0x01, 0x00, data]),            # Call Management
          bytes([4, 0x24, 0x02, 0x02])]                  # ACM
    return [
        dict(number=comm, cls=(0x02, 0x02, 0x01), istr='PORT0', cs=cs, eps=[(0x81, 'interrupt', 16, None)]),
        dict(number=data, cls=(0x0A, 0x00, 0x00), istr='PORT0', cs=[],
             eps=[(0x0A, 'bulk', None, None), (0x89, 'bulk', None, None)]),
    ]


def msc_function(first):
    return [dict(number=first, cls=(0x08, 0x06, 0x50), istr='MS Data Intf BulkOnly', cs=[],
                 eps=[(0x08, 'bulk', None, None), (0x87, 'bulk', None, None)])]


def mtp_function(first, vendor=False, cdc=False):
    """The CDC data pair's addresses alone; the mass-storage pair's beside the CDC port."""
    bulk_in, bulk_out = (0x87, 0x08) if cdc else (0x89, 0x0A)
    return [dict(number=first, cls=(0xFF, 0x01, 0x01) if vendor else (0x06, 0x01, 0x01),
                 istr='MTP TETP interface' if vendor else 'MTP Data Intf', cs=[],
                 eps=[(bulk_in, 'bulk', None, None), (bulk_out, 'bulk', None, None),
                      (0x83, 'interrupt', 64, None)])]


# ---------------------------------------------------------------- S-10a.010 the sets
def sets(device=M.XY):
    """The nine sets on a device: idProduct and every string built on the product's name follow
    device.usb_pid, device.midi_pid and device.product_text (S-10a.001)."""
    p, pid = device.product_text, device.usb_pid
    iad_midi = dict(cls=(0x01, 0x02, 0x00), istr=p + ' (Composite)')
    iad_cdc = dict(cls=(0x02, 0x02, 0x01), istr=p + ' Boot')
    return {
        'midi': dict(pid=device.midi_pid, product=p, dev_cls=(0, 0, 0), config_str='Audio Config',
                     powers=[250, 50, 1], interfaces=midi_function(0, p), iads=[]),
        'midi_cdc': dict(pid=pid, product=p + ' Boot', dev_cls=(0xEF, 0x02, 0x01), config_str='Composite Config',
                         powers=[250], interfaces=midi_function(0, p) + cdc_function(2),
                         iads=[dict(iad_midi, first=0, count=2), dict(iad_cdc, first=2, count=2)]),
        'msc': dict(pid=pid, product=p + ' Disk', dev_cls=(0, 0, 0), config_str='MS Config',
                    powers=[250, 50], interfaces=msc_function(0), iads=[]),
        'msc_cdc': dict(pid=pid, product=p + ' Boot', dev_cls=(0xEF, 0x02, 0x01), config_str='Composite Config',
                        powers=[250], interfaces=cdc_function(0) + msc_function(2),
                        iads=[dict(iad_cdc, first=0, count=2)]),
        'mtp': dict(pid=pid, product=p, dev_cls=(0, 0, 0), config_str='MTP Config',
                    powers=[250], interfaces=mtp_function(0), iads=[]),
        'mtp_vendor': dict(pid=pid, product=p, dev_cls=(0, 0, 0), config_str='MTP Config',
                           powers=[250], interfaces=mtp_function(0, vendor=True), iads=[]),
        'cdc': dict(pid=pid, product=p + ' Boot', dev_cls=(0x02, 0x00, 0x00), config_str='Serial Config',
                    powers=[250], interfaces=cdc_function(0), iads=[]),
        'mtp_cdc': dict(pid=pid, product=p + ' Boot', dev_cls=(0xEF, 0x02, 0x01), config_str='Composite Config',
                        powers=[250], interfaces=cdc_function(0) + mtp_function(2, cdc=True),
                        iads=[dict(iad_cdc, first=0, count=2)]),
        'mtp_vendor_cdc': dict(pid=pid, product=p + ' Boot', dev_cls=(0xEF, 0x02, 0x01),
                               config_str='Composite Config', powers=[250],
                               interfaces=cdc_function(0) + mtp_function(2, vendor=True, cdc=True),
                               iads=[dict(iad_cdc, first=0, count=2)]),
    }


SETS = sets()


# ---------------------------------------------------------------- S-10a.021 speed rules
def mps(kind, speed):
    if kind == 'bulk':
        return 512 if speed == 'high' else 64
    return INT_MPS


def interval(kind, period_ms, speed):
    if kind == 'bulk':
        return 0
    if speed == 'full':
        return period_ms
    # high speed: 2^(bInterval-1) microframes of 125 us equal the period
    n = 1
    while 2 ** (n - 1) < period_ms * 8:
        n += 1
    return n


# ---------------------------------------------------------------- canonical string assignment
class Strings:
    def __init__(self):
        self.texts = {}                     # index -> text

    def ref(self, text):
        if not text:
            return 0
        i = len(self.texts) + 1
        self.texts[i] = text
        return i


def build_set(name, speed, serial=SERIAL, device=M.XY):
    """Every reply of one set at one speed: device, qualifier, each configuration (index 0..n-1) and its
    other-speed form, and the strings ({index: text}, index 0 = the LANGID list)."""
    s = sets(device)[name]
    st = Strings()
    other = 'full' if speed == 'high' else 'high'
    dev = D.build_device(bcdUSB=0x0200, bDeviceClass=s['dev_cls'][0], bDeviceSubClass=s['dev_cls'][1],
                         bDeviceProtocol=s['dev_cls'][2], bMaxPacketSize0=64, idVendor=VID, idProduct=s['pid'],
                         bcdDevice=BCD_DEVICE, iManufacturer=st.ref(MANUFACTURER), iProduct=st.ref(s['product']),
                         iSerialNumber=st.ref(serial), bNumConfigurations=len(s['powers']))
    qual = struct.pack('<BBHBBBBBB', 10, 6, 0x0200, *s['dev_cls'], 64, len(s['powers']), 0)
    configs, others = [], []
    for ci, power in enumerate(s['powers']):
        icfg = st.ref(s['config_str'])
        iads = [(a, st.ref(a['istr'])) for a in s['iads']]
        ifs = [(f, st.ref(f['istr'])) for f in s['interfaces']]
        for sp, out in ((speed, configs), (other, others)):
            body = b''
            for f, istr in ifs:
                for a, astr in iads:
                    if a['first'] == f['number']:
                        body += D.iad_bytes(a['first'], a['count'], *a['cls'], istr=astr)
                body += D.interface_bytes(f['number'], 0, len(f['eps']), *f['cls'], istr=istr)
                for cs in f['cs']:
                    body += cs
                for ep in f['eps']:
                    addr, kind, period, cs_ep = ep
                    attr = 2 if kind == 'bulk' else 3
                    body += D.endpoint_bytes(addr, attr, mps(kind, sp), interval(kind, period, sp),
                                             audio=bool(f.get('audio_eps')))
                    if cs_ep:
                        body += cs_ep
            out.append(D.build_configuration(dict(bNumInterfaces=len(ifs), bConfigurationValue=ci + 1,
                                                  iConfiguration=icfg, bmAttributes=0xC0, bMaxPower=power),
                                             body, other_speed=(out is others)))
    strings = {0: [0x0409]}
    strings.update(st.texts)
    return dict(device=dev, qualifier=qual, configurations=configs, other_speed=others, strings=strings)


def string_bytes(index, text):
    if index == 0:
        return bytes([4, 3, 0x09, 0x04])
    return D.build_string(text)


def hexs(b):
    return bytes(b).hex()


def descriptors_vector(device=M.XY):
    out = {}
    for name in SETS:
        for speed in ('high', 'full'):
            for variant, serial in (('provisioned', SERIAL), ('no_serial', '')):
                r = build_set(name, speed, serial, device)
                out[f'{name}/{speed}/{variant}'] = dict(
                    device=hexs(r['device']), qualifier=hexs(r['qualifier']),
                    configurations=[hexs(c) for c in r['configurations']],
                    other_speed=[hexs(c) for c in r['other_speed']],
                    strings={str(i): (t if i else t) for i, t in r['strings'].items()},
                    string_bytes={str(i): hexs(string_bytes(i, t)) for i, t in r['strings'].items()})
    return out


def serial_vector():
    """S-10a.004: the serial string from the metadata serial."""
    cases = [('XY0A12BC', 'XY0A12BC'), ('', None), ('AB\x01CD\x7fEF', 'AB0CD0EF'), ('\xff\xff\xff\xff\xff\xff\xff\xff',
                                                                                   '00000000'),
             ('A B~', 'A B~')]
    return [dict(metadata_serial=s.encode('latin-1').hex(), usb_serial=u) for s, u in cases]


VECTORS = {
    'descriptors': descriptors_vector,
    'serial': serial_vector,
    'scsi': M.scsi_vector,
    'medium': M.medium_vector,
    'descriptors_1f': lambda: descriptors_vector(M.F1),
    'scsi_1f': lambda: M.scsi_vector(M.F1),
}


def main(argv):
    check = '--check' in argv
    bad = 0
    os.makedirs(os.path.join(HERE, 'vectors'), exist_ok=True)
    for name, fn in VECTORS.items():
        text = json.dumps(fn(), indent=1, sort_keys=True, ensure_ascii=False) + '\n'
        path = os.path.join(HERE, 'vectors', name + '.json')
        if check:
            with open(path, encoding='utf-8') as f:
                if f.read() != text:
                    print('differs:', path)
                    bad = 1
        else:
            with open(path, 'w', encoding='utf-8') as f:
                f.write(text)
    return bad


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
