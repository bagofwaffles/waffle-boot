#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes S-04's OP-1F vectors through s04_model.py: the metadata record of 0x28 bytes without factory-word
or cv-calibration (S-04.040, S-04.043, S-04.046) and the shutdown reports without second-cpu or key-leds
(S-04.066). Synthetic data.

    python3 -I -B tests/oracle/S-04-records/gen_vectors_1f.py     # rewrites vectors/*_1f.json
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import s04_model as m  # noqa: E402

SIZE = m.META_SIZE_1F
DEV = {'size': SIZE, 'factory_word': False}


def sample():
    """M-1F's record: 0x28 bytes, +0x10..11 = C3 3C (no field there)."""
    b = bytearray(m.pack({'serial': b'XY000123', 'sku': 7, 'os_version': (1, 2, 1, 0),
                          'sw_version': (1, 2, 1, 42), 'state': m.UPGRADING})[:SIZE])
    b[0x10:0x12] = b'\xc3\x3c'
    return bytes(b)


def fields_json(f):
    out = dict(f)
    out['state'] = '0x%X' % f['state']
    return out


def current(serial, sku, factory, os_v, sw_v, state):
    return {'serial': serial, 'sku': sku, 'factory': factory, 'os_version': list(os_v),
            'sw_version': list(sw_v), 'state': state}


def metadata():
    s = sample()
    reads = []
    for name, b in [('sample', s), ('bad_magic', b'\xbf' + s[1:]), ('erased', b'\xff' * SIZE),
                    ('factory_bytes_ffff', s[:0x10] + b'\xff\xff' + s[0x12:])]:
        reads.append({'name': name, 'bytes': b.hex(), 'fields': fields_json(m.read(b, **DEV))})
    writes = []
    pads = bytearray(s)
    pads[0x0D] = 0x5A
    pads[0x22:0x24] = b'\x77\x88'
    for name, before, fm, cur, note in [
            ('clear_state', s, False, current('ZZ999999', 9, 5, (1, 2, 1, 0), (1, 2, 1, 42), 0),
             'outside factory mode serial and sku are not stored'),
            ('factory_fields', bytes(pads), True, current('AB123456', 12, 3, (2, 0, 0, 0), (2, 0, 0, 7), 0),
             'serial and sku stored; +0x0D, +0x10..+0x11 and +0x22..+0x23 kept'),
            ('erased_before', b'\xff' * SIZE, False, current('', 0, 0, (0, 0, 0, 0), (0, 0, 0, 0), m.UPGRADING),
             'raw bytes kept under an invalid magic'),
            ('pads_kept', bytes(pads), False, current('AB123456', 12, 3, (1, 2, 1, 0), (0, 0, 0, 0), m.UPGRADING),
             None)]:
        after = m.write_record(before, cur, fm, **DEV)
        assert len(after) == SIZE and after[0x10:0x12] == before[0x10:0x12]
        r = {'name': name, 'before': before.hex(), 'factory_mode': fm, 'current': fields_json(cur),
             'after': after.hex()}
        if note:
            r['note'] = note
        writes.append(r)
    setm = []
    old = {'serial': 'OLD00000', 'sku': 77, 'factory': 0, 'os_version': [0] * 4, 'sw_version': [0] * 4,
           'state': 0}
    for name, req, fm, opt in [
            ('factory_alone', {'factory': '2'}, True, True),
            ('factory_and_serial', {'serial': 'QW000001', 'factory': '2'}, True, True),
            ('all', {'serial': 'QW000001', 'sku_variant': '42', 'factory': '2'}, True, True),
            ('sku_1000', {'sku_variant': '1000'}, True, True),
            ('no_keys', {}, True, True),
            ('not_factory', {'serial': 'AB123456', 'factory': '2'}, False, True),
            ('option_off', {'serial': 'AB123456', 'factory': '2'}, True, False)]:
        w, new = m.set_metadata(old, req, fm, opt, factory_word=False)
        result = {k: v for k, v in new.items() if v != old[k]} if w else {}
        setm.append({'name': name, 'factory_mode': fm, 'option': opt, 'request': req, 'writes': w,
                     'result': result})
    enc_fields = fields_json(m.read(s, **DEV))
    return {'size': '0x%02X' % SIZE, 'magic': '0x%08X' % m.META_MAGIC,
            'note': 'S-04.040: no factory or CV field; +0x10..+0x11 kept by every write, and the sector bytes '
                    'from 0xC028 on are outside the record',
            'encode': [{'fields': enc_fields, 'bytes': s.hex()}],
            'read': reads, 'write': writes, 'set_metadata': setm}


def psoc_status():
    return {'reports': {
        'held': {'messages': [{'id': '0x51', 'payload': '3cff02'}],
                 'note': 'no 0x40 01 and no bar message on the first service-loop pass (S-04.061, S-07.046)'},
        'handover': {'messages': [{'id': '0x51', 'payload': '3cff32'}],
                     'note': 'no PMIC sequence before it (S-07.071)'},
        'shutdown': {'messages': [{'id': '0x51', 'payload': '000002'}],
                     'note': 'S-04.066: one 0x51 00 00 02, no 0x40 01, no power-off request; with the volume '
                             'mounted the eMMC shutdown of S-03.013 comes first'},
        'reset_request': {'messages': [{'id': '0x97', 'payload': 'deadbeef'}, {'id': '0x51', 'payload': '000002'}],
                          'before': 'the reason-20 write of W2'}}}


def main():
    out = {'metadata_1f.json': metadata(), 'psoc_status_1f.json': psoc_status()}
    for name, data in out.items():
        with open(os.path.join(HERE, 'vectors', name), 'w') as f:
            f.write(json.dumps(data, indent=1) + '\n')
    assert struct.calcsize('<H') == 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
