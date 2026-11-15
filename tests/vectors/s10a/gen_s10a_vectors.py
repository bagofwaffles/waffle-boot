#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate test_s10a_vectors.h from tests/oracle/S-10a-usb-msc/vectors/.

Run from the repository root:  python3 -I -B tests/vectors/s10a/gen_s10a_vectors.py --out DIR

Byte strings go into one blob array and are referenced as {present, offset,
length}. The descriptor, serial, SCSI and medium vectors are the JSON files'
contents. The Bulk-Only Transport cases are computed here with the
conformance package's run_bot (BOT 1.0 6.7) on s10a_model's unit, for a grid
of host lengths and directions around each command's intent (S-10a.040).
"""
import hashlib
import json
import os
import struct
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import genout  # noqa: E402  (before anything the generator reads)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
SPEC = os.path.join(ROOT, 'tests', 'oracle', 'S-10a-usb-msc')
OUT = 'test_s10a_vectors.h'
sys.path.insert(0, os.path.join(ROOT, 'tests', 'oracle', 'conformance'))
sys.path.insert(0, SPEC)
from usb import scsi as S  # noqa: E402
import s10a_model as M  # noqa: E402

SETS = {'midi': 'S10A_SET_MIDI', 'midi_cdc': 'S10A_SET_MIDI_CDC', 'msc': 'S10A_SET_DISK',
        'msc_cdc': 'S10A_SET_DISK_CDC', 'mtp': 'S10A_SET_MTP', 'mtp_vendor': 'S10A_SET_MTP_VENDOR',
        'cdc': 'S10A_SET_SERIAL', 'mtp_cdc': 'S10A_SET_MTP_CDC', 'mtp_vendor_cdc': 'S10A_SET_MTP_VENDOR_CDC'}
EVENTS = {None: 0, 'withdraw': 1, 'offer': 2, 'bus_reset': 3, 'set_configuration_same': 4,
          'set_configuration_0': 5, 'withdraw_own_write': 9}
EV_RELEASED, EV_NEW_OFFERED, EV_NEW_ABSENT = 6, 7, 8
MAX_STRINGS = 16


def load(name):
    with open(os.path.join(SPEC, 'vectors', name)) as f:
        return json.load(f)


class Blob:
    def __init__(self):
        self.data = bytearray()

    def ref(self, b):
        if b is None:
            return '{ 0, 0, 0 }'
        b = bytes(b)
        off = len(self.data)
        self.data += b
        return '{ 1, %d, %d }' % (off, len(b))

    def emit(self):
        out = ['static const uint8_t s10av_blob[] = {']
        for i in range(0, len(self.data), 16):
            out.append('\t' + ' '.join('0x%02x,' % b for b in self.data[i:i + 16]))
        out.append('};')
        return out


def c_str(s):
    if s is None:
        return 'NULL'
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def serials(blob):
    out = []
    for v in load('serial.json'):
        out.append('\t{ %s, %s },' % (blob.ref(bytes.fromhex(v['metadata_serial'])), c_str(v['usb_serial'])))
    return out


def descriptor_sets(blob):
    out = []
    for key, v in sorted(load('descriptors.json').items()):
        name, speed, variant = key.split('/')
        cfgs = [blob.ref(bytes.fromhex(c)) for c in v['configurations']]
        others = [blob.ref(bytes.fromhex(c)) for c in v['other_speed']]
        n = len(cfgs)
        cfgs += ['{ 0, 0, 0 }'] * (3 - n)
        others += ['{ 0, 0, 0 }'] * (3 - n)
        sb = v['string_bytes']
        assert len(sb) <= MAX_STRINGS
        strs = [blob.ref(bytes.fromhex(sb[str(i)])) if str(i) in sb else '{ 0, 0, 0 }'
                for i in range(MAX_STRINGS)]
        out.append('\t{ %s, %s, %d, %d, %s, %s,\n\t  { %s },\n\t  { %s }, %d,\n\t  { %s } },' % (
            c_str(key), SETS[name], speed == 'high', variant == 'provisioned',
            blob.ref(bytes.fromhex(v['device'])), blob.ref(bytes.fromhex(v['qualifier'])),
            ', '.join(cfgs), ', '.join(others), n, ',\n\t    '.join(strs)))
    return out


Z = '{ 0, 0, 0 }'


def step(seq, name, event, cdb=Z, data_out=Z, status=0, data_in_len=0, data_in=Z, sha=Z, sense=None,
         releases=0, serial=None):
    s3 = sense or [0, 0, 0]
    return ('\t{ %s, %s, %d, %s, %s, %d, %d, %s, %s, %d, { %d, %d, %d }, %d, %s },'
            % (c_str(seq), c_str(name), event, cdb, data_out, status, data_in_len, data_in, sha,
               sense is not None, s3[0], s3[1], s3[2], releases, c_str(serial)))


def step_line(blob, seq, s):
    if 'event' in s:
        return step(seq, s['name'], EVENTS[s['event']])
    if 'releases' in s:
        return step(seq, s['name'], EV_RELEASED, releases=s['releases'])
    if 'data_in' in s:
        din, sha = blob.ref(bytes.fromhex(s['data_in'])), Z
    else:
        din, sha = Z, blob.ref(bytes.fromhex(s['data_in_sha256']))
    return step(seq, s['name'], 0, blob.ref(bytes.fromhex(s['cdb'])), blob.ref(bytes.fromhex(s['data_out'])),
                s['status'], s['data_in_len'], din, sha, s['sense'])


def scsi_steps(blob):
    out = []
    starts = {'offered': (EV_NEW_OFFERED, M.SERIAL), 'no_serial': (EV_NEW_OFFERED, ''),
              'no_medium': (EV_NEW_ABSENT, M.SERIAL)}
    for fname in ('scsi.json', 'medium.json'):
        for seq, steps in sorted(load(fname).items()):
            ev, serial = starts.get(seq, (EV_NEW_OFFERED, M.SERIAL))
            if seq == 'stop_eject_states':
                ev = EV_NEW_ABSENT
            out.append(step(seq, 'new unit', ev, serial=serial))
            out += [step_line(blob, seq, s) for s in steps]
    return out


def bot_cases(blob):
    """Each command on a fresh unit whose first unit attention is consumed, under host lengths and
    directions around its intent."""
    cmds = [('TEST UNIT READY', S.cdb_tur(), 0, None),
            ('INQUIRY 36', S.cdb_inquiry(36), 36, True),
            ('READ(10) 1 block', S.cdb_read10(3, 1), 512, True),
            ('WRITE(10) 1 block', S.cdb_write10(5, 1), 512, False)]
    out = []
    tag = 0x1000
    for name, cdb, intent, dev_in in cmds:
        grid = [(0, True)]
        if intent:
            grid += [(intent, dev_in), (intent // 2 + 1, dev_in), (intent + 1024, dev_in), (intent, not dev_in)]
        else:
            grid += [(512, True), (512, False)]
        for host_len, host_in in grid:
            u = M.Unit()
            u.execute(S.cdb_tur())
            data_out = M.pattern(4, tag & 0xFF)[:host_len] if not host_in else b''
            cbw = S.encode_cbw(tag, host_len, host_in, 0, cdb)
            data, csw, o = S.run_bot(u, cbw, data_out)
            stall = {None: 0, 'in': 1, 'out': 2}[o.stall]
            written = u.dev.read(5, 1)
            out.append('\t{ %s, %d, %s, %s, %s, %s, %d, %d, %s },' % (
                c_str('%s, host %s %d' % (name, 'in' if host_in else 'out', host_len)), o.case,
                blob.ref(cbw), blob.ref(data_out), blob.ref(data), blob.ref(csw), stall, o.transfer,
                blob.ref(written)))
            tag += 1
    return out


def main():
    blob = Blob()
    ser = serials(blob)
    sets = descriptor_sets(blob)
    steps = scsi_steps(blob)
    bots = bot_cases(blob)
    lines = [
        '/* SPDX-License-Identifier: GPL-2.0-or-later */',
        '/* Generated by tests/vectors/s10a/gen_s10a_vectors.py from',
        ' * tests/oracle/S-10a-usb-msc/vectors/ and the conformance package. Do not edit. */',
        '#ifndef TEST_S10A_VECTORS_H',
        '#define TEST_S10A_VECTORS_H',
        '',
        '#include <stddef.h>',
        '#include <stdint.h>',
        '',
        '#include "waffle/s10a_desc.h"',
        '',
        '/* present (0 for null), offset into s10av_blob, length */',
        'struct s10av_ref { int present; size_t off, len; };',
        '/* S-10a.004: the metadata serial and the USB serial (NULL: none) */',
        'struct s10av_serial { struct s10av_ref meta; const char *usb; };',
        '/* one set at one speed with or without a serial; strings by index */',
        'struct s10av_set { const char *name; enum s10a_set set; int high, serial;',
        '\tstruct s10av_ref device, qualifier;',
        '\tstruct s10av_ref configs[3];',
        '\tstruct s10av_ref other[3]; int n_configs;',
        '\tstruct s10av_ref strings[%d]; };' % MAX_STRINGS,
        '/* event: 0 a command; 1 withdraw (a scan), 2 offer, 3 bus reset, 4',
        ' * SET_CONFIGURATION of the current value, 5 SET_CONFIGURATION(0); 6 the',
        ' * release count; 7 a new unit, offered (unit attention owed), 8 a new unit,',
        ' * no medium; 9 withdraw for an own write */',
        'struct s10av_step { const char *seq, *name; int event;',
        '\tstruct s10av_ref cdb, data_out; int status; uint32_t data_in_len;',
        '\tstruct s10av_ref data_in, data_in_sha256; int has_sense; uint8_t sense[3];',
        '\tint releases; const char *serial; };',
        '/* one BOT command: the CBW, the host\'s data-out, the data-in and CSW the',
        ' * host sees, the endpoint the device halts (0 none, 1 IN, 2 OUT), the bytes',
        ' * moved, and block 5 afterwards */',
        'struct s10av_bot { const char *name; int bot_case;',
        '\tstruct s10av_ref cbw, data_out, data_in, csw; int stall; uint32_t moved;',
        '\tstruct s10av_ref block5; };',
        '',
    ]
    lines += blob.emit()
    lines += ['', 'static const struct s10av_serial s10av_serials[] = {'] + ser + ['};']
    lines += ['', 'static const struct s10av_set s10av_sets[] = {'] + sets + ['};']
    lines += ['', 'static const struct s10av_step s10av_steps[] = {'] + steps + ['};']
    lines += ['', 'static const struct s10av_bot s10av_bots[] = {'] + bots + ['};']
    lines += ['', '#endif /* TEST_S10A_VECTORS_H */', '']
    genout.write(OUT, '\n'.join(lines))
    genout.finish()


if __name__ == '__main__':
    main()
