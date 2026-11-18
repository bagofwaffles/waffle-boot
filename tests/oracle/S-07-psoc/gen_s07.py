#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates S-07's vectors, tables and timed transcripts through s07_model.py. Synthetic data.

    python3 -I -B tests/oracle/S-07-psoc/gen_s07.py --write     # rewrite vectors/, transcripts/, .tsv files
    python3 -I -B tests/oracle/S-07-psoc/gen_s07.py             # check that the stored files are current
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import s07_model as m  # noqa: E402
from s07_model import cypress  # noqa: E402

SYNTH_UNLOCK = bytes.fromhex('0a1b2c3d4e5f')    # synthetic: not the PSoC unlock

# ---- tables ----

KEYS = (['help', 'tempo', 'synth', 'drum', 'tape', 'mixer', 'M1', 'M2', 'M3', 'M4',
         'knob 1 click', 'knob 2 click', 'knob 3 click', 'knob 4 click']
        + [f'sound {i}' for i in range(1, 9)] + ['seq', 'mic', 'usb', 'bar']
        + ['piano ' + n for n in ['F3', 'F#3', 'G3', 'G#3', 'A3', 'A#3', 'B3', 'C4', 'C#4', 'D4', 'D#4', 'E4',
                                  'F4', 'F#4', 'G4', 'G#4', 'A4', 'A#4', 'B4', 'C5', 'C#5', 'D5', 'D#5', 'E5']]
        + ['record', 'play', 'stop', 'left', 'right', 'shift'] + [f'step {i}' for i in range(1, 17)])
DIGITS = {0x06: 1, 0x0A: 1, 0x0E: 1, 0x07: 2, 0x0B: 2, 0x0F: 2, 0x08: 3, 0x0C: 3, 0x10: 3,
          0x09: 4, 0x0D: 4, 0x11: 4}


def keys_tsv():
    assert len(KEYS) == 72
    out = ['# S-07.030',
           'number\tcontrol\tdigit']
    for k, name in enumerate(KEYS):
        out.append(f'{k:#04x}\t{name}\t{DIGITS.get(k, "")}')
    return '\n'.join(out) + '\n'


def charge_led_tsv():
    out = ['# S-07.042',
           'state\tcharge\trgb\tforced']
    for s in [None, 0x10, 0x12, 0x14, 0x16, 0x18, 0x20]:
        for c in [0, 5, 14, 15, 50, 90, 91, 100]:
            rgb, forced = m.charge_led(s, c)
            out.append(f'{"none" if s is None else f"{s:#04x}"}\t{c}\t03{rgb.hex()}\t{int(forced)}')
    return '\n'.join(out) + '\n'


# ---- vectors ----

def catalog():
    t1 = [{'id': f'{i:#04x}', 'kind': k, 'payloads': p} for i, k, p in m.TABLE1]
    return {
            'table1': t1, 'table2': [f'{i:#04x}' for i in m.TABLE2],
            'psoc_accepts': [f'{i:#04x}' for i in m.PSOC_ACCEPTS]}


def bm(*keys):
    b = bytearray(9)
    for k in keys:
        b[k >> 3] |= 1 << (k & 7)
    return bytes(b)


def keys_vectors():
    seqs = [[bm(0x10)], [bm(0x10), bm(0x10), bm()], [bm(0x47, 0x00), bm(0x01)], [bm(*range(72)), bm()],
            [bm(0x0D, 0x18), bm(0x18)],
            [bm(0x10), bm(0x10, 0x11)[:8], bm()],             # S-07.079: an 8-byte bitmap is ignored
            [bm(0x05) + b'\xff']]                              # S-07.079: a tenth byte is not read
    bitmaps = []
    for s in seqs:
        known = [False] * 72
        evs = []
        for b in s:
            known, ev = m.bitmap_events(known, b)
            evs.append([[k, int(d)] for k, d in ev])
        bitmaps.append({'bitmaps': [b.hex() for b in s], 'events': evs})
    status = []
    for p in ['00000000', '00000001', '00200000', '00040000', '00080000', '00008000', '00ff00fe', '002c8001',
              '00000001000000000000', '40000000000000000000', '00000000000000000100', None]:   # 10 bytes: S-07.032
        st = m.key_status(None if p is None else bytes.fromhex(p))
        status.append({'payload': p if p is not None else 'none', **{k: int(v) for k, v in st.items()}})
    # S-07.031, S-07.032: the key-status reply is the known state the bitmaps are told against, keys held at
    # power-on included: the white knob held, then com pressed and released, then the knob let go
    after = []
    for st, seq in [(bm(0x0D, 0x18) + b'\x00', [bm(0x0D), bm(0x0D, 0x06), bm(0x0D), bm()]),
                    (bm(0x40)[:4], [bm(0x40), bm()]),
                    (None, [bm(0x0D)])]:
        known = m.known_from_status(st)
        evs = []
        for b in seq:
            known, ev = m.bitmap_events(known, b)
            evs.append([[k, int(d)] for k, d in ev])
        after.append({'status': st.hex() if st is not None else 'none', 'bitmaps': [b.hex() for b in seq],
                      'events': evs})
    return {'bitmaps': bitmaps, 'status': status, 'after_status': after}


def leds_vectors():
    cases = [
        [['bar', 0], ['bar', 0], ['bar', 5]],
        [['key', 3, '00ff'], ['key', 3, '00ff'], ['key', 4, '0000'], ['key', 4, 'fe00']],
        [['rgb', '000000'], ['rgb', 'ff0000'], ['rgb', 'ff0000']],
        [['rgb', '00c800'], ['rgb', 'c80000', 'forced'], ['rgb', '00c800'], ['rgb', 'c80000']],
    ]
    out = []
    for c in cases:
        sent = []
        leds = m.Leds(lambda p: sent.append(p.hex()))
        for op in c:
            if op[0] == 'bar':
                leds.set_bar(op[1])
            elif op[0] == 'key':
                leds.set_key(op[1], bytes.fromhex(op[2]))
            else:
                leds.set_rgb(bytes.fromhex(op[1]), forced=len(op) > 2)
        out.append({'requests': c, 'sent': sent})
    return {'cases': out}


def power_vectors():
    states = [{'state': f'{s:#04x}', 'kind': m.power_kind(s)} for s in [0x10, 0x12, 0x14, 0x16, 0x18, 0x20]]
    trans = []
    for prev in [None, 0x10, 0x12, 0x14, 0x16]:
        for s in [0x10, 0x12, 0x14, 0x16]:
            trans.append({'prev': 'none' if prev is None else f'{prev:#04x}', 'state': f'{s:#04x}',
                          'power_off': int(m.power_off_trigger(prev, s))})
    batt = []
    for p in ['00641900641068', '01641900641068', '02051e01f40dad', '03061e01f40dad', '01641900640dac',
              '01509681f41068',                           # S-07.053: unsigned temperature, big-endian values
              '00', '02', '0164', '']:                    # S-07.079: missing bytes read as 00
        b = m.battery_report(bytes.fromhex(p))
        batt.append({'payload': p, **{k: (int(v) if isinstance(v, bool) else v) for k, v in b.items()},
                     'usable': int(m.battery_usable(b))})
    vbus = []
    for applied, target, rep in [(0, 500, '000a'), (100, 500, '000a'), (0, 0, '000a'), (500, 500, '0032'),
                                 (0, 1000, '0000'), (0, 500, '010a'), (0, 505, '0000'), (200, 100, '0014'),
                                 (0, 505, '0032'), (0, 2, '0000'), (0, 100, '0000'),
                                 (0, 510, '0000'),                                # S-07.055: the largest grant
                                 (0, 1000, ''), (0, 500, '00'), (0, 500, '01')]:  # S-07.079: short replies
        a, p = m.vbus_update(applied, target, bytes.fromhex(rep))
        vbus.append({'applied': applied, 'target': target, 'reply': rep, 'applied_after': a,
                     'send_34': p.hex() if p is not None else None})
    return {
            'initial': 'none' if m.Sim.INITIAL_STATE is None else f'{m.Sim.INITIAL_STATE:#04x}',
            'states': states, 'transitions': trans, 'battery': batt,
            'vbus': vbus}


def pmic_vectors():
    seqs = []
    for v in [0x00, 0x9F, 0x60, 0xFF, None]:
        seqs.append({'read': 'none' if v is None else f'{v:#04x}',
                     'frames': [[f'{i:#04x}', k, p.hex()] for i, k, p in m.pmic_sequence(v)]})
    return {
            'messages': [{'id': '0x70', 'payloads': ['01', '00']}, {'id': '0x73', 'payloads': ['03..']},
                         {'id': '0x71', 'payloads': ['03....']}], 'sequence': seqs}


def cyacd_text(rows=3, row_len=128, silicon=0x2E123069, rev=0, ctype=0, arr=0, first=0, bad_line=None):
    rs = []
    for r in range(rows):
        data = bytes((r * 37 + i * 11 + 5) & 0xFF for i in range(row_len))
        rs.append((arr, first + r, data))
    t = cypress.build_cyacd(silicon, rev, rs, ctype)
    if bad_line is not None:
        lines = t.split('\r\n')
        l = lines[1 + bad_line]
        cc = (int(l[-2:], 16) + 1) & 0xFF
        lines[1 + bad_line] = l[:-2] + f'{cc:02X}'
        t = '\r\n'.join(lines)
    return t


def cypress_vectors():
    enter = {'unlock': SYNTH_UNLOCK.hex(), 'packet': m.enter_packet(SYNTH_UNLOCK).hex()}
    ok = cypress.packet(0, (0x2E123069).to_bytes(4, 'little') + bytes([0, 1, 30, 1]))
    bad_ck = ok[:12] + bytes([ok[12] ^ 0xFF]) + ok[13:]
    probe = [
        {'reply': ok.hex(), 'ok': 1, 'why': 'success, 8 data bytes'},
        {'reply': bad_ck.hex(), 'ok': 'either', 'why': 'wrong checksum: accepting and rejecting are both allowed'},
        {'reply': 'ffee' + ok.hex(), 'ok': 1, 'why': 'bytes before the first 01 skipped'},
        {'reply': cypress.packet(1, bytes(8)).hex(), 'ok': 0, 'why': 'status 01 (unlock mismatch)'},
        {'reply': cypress.packet(0, bytes(4)).hex(), 'ok': 0, 'why': '4 data bytes'},
        {'reply': ok[:10].hex(), 'ok': 0, 'why': 'truncated'},
        {'reply': (ok[:14] + b'\x00' + bytes(25)).hex(), 'ok': 0, 'why': '40 bytes, no 17 at byte 15'},
        {'reply': '', 'ok': 0, 'why': 'no reply'},
        {'wait': 'fffe', 'reply': ok.hex(), 'ok': 1,
         'why': 'bytes received during the 250 ms wait, none of them 01: accepted whether they are discarded or judged'},
        {'wait': '0102', 'reply': ok.hex(), 'ok': 'either',
         'why': 'an 01 received during the wait: discarding it accepts, judging it with the reply may reject'},
    ]
    for p in probe:
        assert p['ok'] == 'either' or m.probe_verdict(bytes.fromhex(p['reply'])) == bool(p['ok']), p
        if 'wait' in p and p['ok'] != 'either':
            assert m.probe_verdict(bytes.fromhex(p['wait'] + p['reply'])) == bool(p['ok']), p
    packets = []
    for code, data in [(0x38, SYNTH_UNLOCK), (0x32, b'\x00'), (0x37, bytes(range(55))),
                       (0x39, b'\x00\x05\x00' + bytes(range(18))), (0x3A, b'\x00\x05\x00'), (0x31, b''),
                       (0x3B, b'')]:
        packets.append({'code': f'{code:#04x}', 'data': data.hex(), 'sum': cypress.packet(code, data, 'sum').hex(),
                        'crc': cypress.packet(code, data, 'crc').hex()})
    checksum = [{'bytes': b.hex(), 'sum': f'{cypress.checksum(b, "sum"):#06x}', 'crc': f'{cypress.checksum(b, "crc"):#06x}'}
                for b in [b'', b'\x01\x3b\x00\x00', b'123456789', bytes(range(256))]]
    text = cyacd_text()
    bl = cypress.Bootloader(row_size=None)
    okr, err, sent = m.run_direct(text, bl, SYNTH_UNLOCK)
    assert okr, err
    runs = [{'cyacd': text, 'unlock': SYNTH_UNLOCK.hex(), 'packets': [p.hex() for p in sent]}]
    for rows, row_len in [(2, 108), (1, 109)]:      # 53 and 54 bytes left after the first send data
        t = cyacd_text(rows=rows, row_len=row_len)
        okr, err, sent = m.run_direct(t, cypress.Bootloader(row_size=row_len), SYNTH_UNLOCK)
        assert okr, err
        runs.append({'cyacd': t, 'unlock': SYNTH_UNLOCK.hex(), 'row_len': row_len, 'packets': [p.hex() for p in sent],
                     'why': f'{row_len}-byte rows: send data 55 and {row_len - 55}, then program row with no row byte'})
    runs[0]['row_len'] = 128
    t = cypress.build_cyacd(0x2E123069, 0, [(0, 0, bytes(range(32))), (0x40, 0, bytes(range(32, 64))),
                                            (0x40, 1, bytes(range(64, 96)))])
    okr, err, sent = m.run_direct(t, eeprom_bootloader(), SYNTH_UNLOCK)
    assert okr, err
    runs.append({'cyacd': t, 'unlock': SYNTH_UNLOCK.hex(), 'row_len': 32, 'eeprom': 1,
                 'packets': [p.hex() for p in sent],
                 'why': 'rows on array 00 and on array 40: get flash size for array 00 only'})
    ok7 = cypress.packet(0, b'')
    replies = [
        {'cmd': '0x37', 'reply': ok7.hex(), 'ok': 1, 'why': 'success, no data'},
        {'cmd': '0x37', 'reply': (ok7[:4] + bytes([ok7[4] ^ 0xFF]) + ok7[5:]).hex(), 'ok': 'either',
         'why': 'wrong checksum: checking it is allowed, not required'},
        {'cmd': '0x37', 'reply': (b'\x01\x00\x01\x00' + ok7[4:]).hex(), 'ok': 0, 'why': 'length field 1, not 0'},
        {'cmd': '0x37', 'reply': (b'\x02' + ok7[1:]).hex(), 'ok': 0, 'why': 'first byte not 01'},
        {'cmd': '0x37', 'reply': (ok7[:-1] + b'\x18').hex(), 'ok': 0, 'why': 'last byte not 17'},
        {'cmd': '0x37', 'reply': cypress.packet(0x05).hex(), 'ok': 0, 'why': 'status 05'},
        {'cmd': '0x3a', 'reply': ok7.hex(), 'ok': 0,
         'why': 'a complete packet but 7 bytes, not 8: not complete, fails when the 5 s are up at the latest'},
        {'cmd': '0x38', 'reply': cypress.packet(0x01).hex(), 'ok': 0,
         'why': 'a 7-byte error reply to enter bootloader: not complete (15 bytes), the run fails'},
    ]
    good = cyacd_text(rows=1, row_len=8)
    h, row = good.split('\r\n')[:2]
    images = [
        {'cyacd': good, 'bad': 0, 'why': 'well formed'},
        {'cyacd': '# a comment\r\n' + h + '\r\n# another\r\n' + row + '\r\n', 'bad': 0, 'why': 'comment lines skipped'},
        {'cyacd': (h + '\n' + row).lower(), 'bad': 0, 'why': 'LF line ends, no final line end, lower-case hex'},
        {'cyacd': cyacd_text(rows=1, row_len=8, bad_line=0), 'bad': 0,
         'why': 'a wrong line check is not a bad file: the run fails at verify row'},
        {'cyacd': h[:10] + '\r\n' + row + '\r\n', 'bad': 1, 'why': 'header of 10 hex digits'},
        {'cyacd': h[:10] + '02\r\n' + row + '\r\n', 'bad': 1, 'why': 'checksum type 02'},
        {'cyacd': h[:10] + '01\r\n' + row + '\r\n', 'bad': 1,
         'why': 'checksum type 01: the enter packet can enter only a sum-checking PSoC bootloader'},
        {'cyacd': h + '\r\n', 'bad': 1, 'why': 'no row line'},
        {'cyacd': h + '\r\n' + row[1:] + '\r\n', 'bad': 1, 'why': 'row line without the colon'},
        {'cyacd': h + '\r\n' + row[:-1] + '\r\n', 'bad': 1, 'why': 'odd number of hex digits'},
        {'cyacd': h + '\r\n' + row[:-2] + 'G0\r\n', 'bad': 1, 'why': 'a character that is not a hex digit'},
        {'cyacd': h + '\r\n' + row[:-4] + row[-2:] + '\r\n', 'bad': 1, 'why': 'length field 8, 7 data bytes'},
        {'cyacd': h + '\r\n' + row + '\r\n\r\n' + row + '\r\n', 'bad': 1, 'why': 'an empty line'},
        {'cyacd': cyacd_text(rows=1, row_len=762), 'bad': 1, 'why': 'a row line of 1537 characters (762 data bytes)'},
        {'cyacd': cyacd_text(rows=1, row_len=761), 'bad': 0, 'why': 'a row line of 1535 characters (761 data bytes)'},
        {'cyacd': build_arrays(9), 'bad': 'either', 'why': 'rows of 9 array ids: a master may refuse more than 8'},
        {'cyacd': build_arrays(8), 'bad': 0, 'why': 'rows of 8 array ids'},
    ]
    for i in images:
        p = m.image_problem(i['cyacd'])
        assert i['bad'] == 'either' or (p is not None) == bool(i['bad']), (i['why'], p)
    assert m.image_problem(build_arrays(9), max_arrays=8) == 'arrays'
    return {
            'enter': enter, 'probe': probe, 'packets': packets, 'checksum': checksum,
            'exit_packet': m.EXIT_PACKET.hex(), 'runs': runs, 'replies': replies, 'images': images}


def eeprom_bootloader():
    """A bootloader with flash array 00 and array 40 that refuses get flash size for array 40."""
    return cypress.Bootloader(row_size=32, arrays={0: (0, 0xFF), 0x40: (0, 0x3F)},
                              hook=lambda c, d: cypress.ERR_ARRAY if c == 0x32 and d[:1] == b'\x40' else None)


def build_arrays(n):
    return cypress.build_cyacd(0x2E123069, 0, [(a, 0, bytes(range(8))) for a in range(n)])


# ---- transcripts ----

def fm(ev, mid, payload=None, nth=1, d='M', after=-1):
    n = 0
    for i, e in enumerate(ev):
        if i <= after or e[1] != d or e[2] != 'frame' or e[4] != mid:
            continue
        if payload is not None and e[5] != bytes.fromhex(payload):
            continue
        n += 1
        if n == nth:
            return i
    raise LookupError(f'frame {mid:#x} {payload} #{nth} ({d})')


def fall(ev, mid, d='M', payload=None):
    return [i for i, e in enumerate(ev) if e[1] == d and e[2] == 'frame' and e[4] == mid
            and (payload is None or e[5] == bytes.fromhex(payload))]


def raws(ev, d='M'):
    return [i for i, e in enumerate(ev) if e[1] == d and e[2] == 'raw']


def gaps(idx, lo=None, hi=None, why=''):
    c = []
    for a, b in zip(idx, idx[1:]):
        x = {'from': a, 'to': b, 'why': why}
        if lo is not None:
            x['min'] = lo
        if hi is not None:
            x['max'] = hi
        c.append(x)
    return c


def chk(a, b, lo=None, hi=None, why=''):
    x = {'from': a, 'to': b, 'why': why}
    if lo is not None:
        x['min'] = lo
    if hi is not None:
        x['max'] = hi
    return [x]


def greet_checks(ev):
    g = fall(ev, 0x05)
    c = gaps(g, 10, None, 'S-07.020: greetings at least 10 ms apart')
    return c


HELD = {'rules': [{'on': '0x41', 'send': [{'id': '0x42', 'payload': '00000001' + '00' * 6}]}]}
PING_LATE_T = 12                # boot path: after the battery wait's requests, while the key status is asked
PROMPT_T = 100                  # a time while the boot prompt waits (S-05.025)
GREET_LATE_T = 400              # 300 ms after the exit packet, which follows ten greetings 10 ms apart
HELD_T = 400                    # a time in held mode, after the first pass, in every held transcript
CYACD = cyacd_text()


def held(extra_rules=(), inject=(), bootloader=None, defaults=True):
    p = {'rules': list(extra_rules) + HELD['rules'], 'inject': list(inject), 'defaults': defaults}
    if bootloader is not None:
        p['bootloader'] = bootloader
    return p


def T(name, reqs, about, setup, psoc, checks=None):
    return {'name': name, 'requirements': reqs, 'setup': setup, 'psoc': psoc,
            'checks_fn': checks or (lambda ev: [])}


def flat_battery_rules():
    return [{'on': '0x36', 'send': [{'id': '0x37', 'payload': '01031900640dac'}]},
            {'on': '0x30', 'send': [{'id': '0x31', 'payload': '10'}]},
            {'on': '0x33', 'send': [{'id': '0x33', 'payload': '000a'}]}]


def battery_loop_checks(ev, charge=False):
    """Timing checks for the first two rounds of the S-07.054 wait loop, or of the charge wait (charge)."""
    start = fm(ev, 0x30, '', nth=2) if charge else fm(ev, 0x42, d='P')
    first = [i for i in fall(ev, 0x36) if i > start]
    c = (chk(start, first[0], 0, 20, 'S-07.054: the first round as the %s begins' % ('charge wait' if charge
                                                                                      else 'loop'))
         + chk(first[0], first[1], 1000, 1100, 'S-07.054: queries again after more than 1 s'))
    return c


def transcripts():
    ts = []
    ts.append(T('boot_normal', ['S-07.001', 'S-07.012', 'S-07.022', 'S-07.023', 'S-07.061', 'S-07.071'],
                'PSoC answers everything; keys not held; boot allowed; the OS loads.',
                {'timestamp': True}, {}, greet_checks))
    ts.append(T('boot_no_timestamp', ['S-07.066'], 'As boot_normal, but the first stage left no panel-reset time.',
                {'timestamp': False}, {}))
    ts.append(T('boot_held', ['S-07.023', 'S-07.032', 'S-07.043', 'S-07.077'],
                'Key status byte 3 bit 0: held mode.', {}, HELD))
    ts.append(T('boot_noos', ['S-07.023', 'S-07.071'], 'Boot path taken, the OS image does not load: held mode.',
                {'os_loads': False}, {}))
    ts.append(T('boot_late_keys', ['S-07.011'], 'The key status is answered after 80 ms: used (not held).', {},
                {'rules': [{'on': '0x41', 'delay_ms': 80, 'send': [{'id': '0x42', 'payload': '00' * 10}]}]},
                lambda ev: chk(fm(ev, 0x41), fm(ev, 0x70, '01'), 80, 100, 'S-07.011: waits for a reply in 100 ms')))
    ts.append(T('boot_keys_timeout', ['S-07.011', 'S-07.032', 'S-07.076'],
                'The key status is answered after 150 ms: not used; held mode; the late reply sends nothing.', {},
                {'rules': [{'on': '0x41', 'delay_ms': 150, 'send': [{'id': '0x42', 'payload': '00' * 10}]}]},
                lambda ev: chk(fm(ev, 0x41), fm(ev, 0x09), 100, 149, 'S-07.011: gives up after 100 ms')))
    ts.append(T('boot_psoc_silent', ['S-07.032'], 'No reply to the key status: held.', {},
                {'rules': [{'on': '0x41', 'send': []}]}))
    ts.append(T('greet_fourth', ['S-07.020'], 'The fourth greeting is accepted.', {},
                {'rules': [{'on': '0x05', 'nth': [1, 2, 3], 'send': []}]}, greet_checks))
    ts.append(T('greet_rejected', ['S-07.020'], '0x06 {41} does not greet the link; the next 0x06 {40} does.', {},
                {'rules': [{'on': '0x05', 'nth': [1], 'send': [{'id': '0x06', 'payload': '41'}]}]}, greet_checks))
    ts.append(T('greet_fail', ['S-07.020', 'S-07.021', 'S-07.086'],
                'No answer to ten greetings: the exit packet, at least 1.5 s of silence, then the power queries.', {},
                {'rules': [{'on': '0x05', 'send': []}]},
                lambda ev: greet_checks(ev) + chk(fall(ev, 0x05)[0], fall(ev, 0x05)[-1], None, 200,
                                                  'S-07.020: ten greetings within 200 ms')
                + chk(raws(ev)[0], fm(ev, 0x30), 1500, None, 'S-07.021: 1.5 s wait')))
    ts.append(T('greet_late', ['S-07.021', 'S-07.024'],
                'No answer to ten greetings: the exit packet; the PSoC greets 300 ms into the wait, which ends '
                'then: the greeting answered 0x06 {40}, then the power queries.', {},
                {'rules': [{'on': '0x05', 'send': []}], 'inject': [{'t_ms': GREET_LATE_T, 'id': '0x05',
                                                                     'payload': '01'}]},
                lambda ev: chk(raws(ev)[0], fm(ev, 0x05, d='P'), 300, 300,
                               'S-07.021: the PSoC greets 300 ms into the wait')
                + chk(fm(ev, 0x05, d='P'), fm(ev, 0x30), 0, 5, 'S-07.021: the wait ends at the greeting')))
    ts.append(T('greet_by_psoc', ['S-07.024'], 'The PSoC greets first, during the start-up sequence.', {},
                {'rules': [{'on': '0x05', 'send': []}], 'inject': [{'t_ms': 3, 'id': '0x05', 'payload': '01'}]}))
    ts.append(T('regreet', ['S-07.024', 'S-07.025', 'S-07.065'], 'A PSoC greeting in held mode.', {'end_ms': 600},
                held(inject=[{'t_ms': HELD_T, 'id': '0x05', 'payload': '01'}])))
    ts.append(T('regreet_unanswered', ['S-07.024'], 'A PSoC greeting in held mode; the re-greeting goes unanswered.',
                {'end_ms': 700},
                held([{'on': '0x05', 'after_ms': HELD_T - 1, 'send': []}],
                     inject=[{'t_ms': HELD_T, 'id': '0x05', 'payload': '01'}])))
    ts.append(T('regreet_repeated', ['S-07.024'],
                'Five PSoC greetings in a row in held mode: one re-greeting, which the later ones end, and one '
                'answer; a greeting after it starts one more.', {'end_ms': 700},
                held(inject=[{'t_ms': HELD_T + i, 'id': '0x05', 'payload': '01'} for i in range(5)]
                     + [{'t_ms': HELD_T + 100, 'id': '0x05', 'payload': '01'}])))
    other =[{'on': i, 'kind': 'request', 'send': [{'id': '0x05', 'payload': p}]} for i, p in
             [('0x30', '10'), ('0x36', '01641900641068'), ('0x41', '00000000'), ('0x73', '031000'), ('0x01', ''),
              ('0x33', '000a'), ('0x09', '')]]
    ts.append(T('reply_ids_other', ['S-07.012'],
                'Every reply has id 0x05: 0x33, 0x41, 0x73, 0x01 replies are used; the waited 0x30/0x36 replies are '
                'not greetings and set nothing, so the battery wait runs until the limit reaches 1000 mA.', {},
                {'rules': other}))
    ts.append(T('psoc_dead', ['S-07.055', 'S-07.058'],
                'The PSoC sends nothing at all and no USB host is attached: the greeting fails, the battery wait '
                'ends through the 3 s rule (0x34 {00 64}), the keys count as held; the first held pass raises the '
                'limit again (an unanswered 0x33 counts as {00 00}), then the link is idle.', {},
                {'defaults': False},
                lambda ev: chk(fm(ev, 0x41), fm(ev, 0x09), 3000, 4000,
                               'S-07.058: the wait loop ends after 3 s and within 4 s')))
    kt = [{'t_ms': HELD_T + 150 + 20 * i, 'id': '0x42', 'payload': bm(*k).hex()} for i, k in
          enumerate([[0x10], [], [0x20], [0x20, 0x21], [0x20], [], [0x10], []])]
    ts.append(T('key_test', ['S-07.031', 'S-07.040', 'S-07.045', 'S-07.067'],
                'Held mode; the key test runs; key 0x10 alone twice, 0x20 with 0x21.',
                {'end_ms': 1000, 'actions': [{'t_ms': HELD_T, 'do': 'key_test_start'},
                                             {'t_ms': HELD_T + 400, 'do': 'key_test_stop'}]},
                held(inject=kt)))
    ts.append(T('power_off_14', ['S-07.004', 'S-07.051', 'S-07.072'], 'Held mode; the PSoC reports 0x14.',
                {'end_ms': 1000}, held(inject=[{'t_ms': HELD_T, 'id': '0x31', 'payload': '14'}])))
    ts.append(T('power_off_10_16', ['S-07.051'], 'Held mode in state 0x10; the PSoC reports 0x16.',
                {'end_ms': 1000}, held(inject=[{'t_ms': HELD_T, 'id': '0x32', 'payload': '16'}])))
    ts.append(T('power_off_once', ['S-07.051', 'S-07.072'], 'Held mode; the PSoC reports 0x14, then 0x14 again '
                'and pings: the power-off runs once and nothing after it is seen.',
                {'end_ms': 1000}, held(inject=[{'t_ms': HELD_T, 'id': '0x31', 'payload': '14'},
                                              {'t_ms': HELD_T + 50, 'id': '0x31', 'payload': '14'},
                                              {'t_ms': HELD_T + 60, 'id': '0x01', 'payload': 'dd'}])))
    def no_power_off(ev):
        assert not fall(ev, 0x32), 'S-07.051: no power-off while the first-stage window stays open'
        return []
    fs = [{'t_ms': HELD_T, 'do': 'fs_window_open'}]
    fs_close = [{'t_ms': HELD_T + 200, 'do': 'fs_window_close'}]
    ts.append(T('fs_hold_off', ['S-07.051', 'S-02.071', 'S-02.072'],
                'Held mode; a first-stage write: the PSoC reports 0x14 after its first erase; the power-off is '
                'kept and runs, with s 14, once the read-back has matched.',
                {'end_ms': 1000, 'actions': fs + fs_close},
                held(inject=[{'t_ms': HELD_T + 20, 'id': '0x31', 'payload': '14'}]),
                lambda ev: chk(fm(ev, 0x31, '14', d='P'), fm(ev, 0x32, '14'), 170, 200,
                               'S-07.051: the kept power-off runs when the window ends')))
    ts.append(T('fs_hold_dropped', ['S-07.051'],
                'Held mode; a first-stage write: 0x14, then 0x10 within the window: the kept power-off is '
                'dropped, and nothing follows the read-back.',
                {'end_ms': 800, 'actions': fs + fs_close},
                held(inject=[{'t_ms': HELD_T + 20, 'id': '0x31', 'payload': '14'},
                             {'t_ms': HELD_T + 60, 'id': '0x31', 'payload': '10'}]), no_power_off))
    ts.append(T('fs_hold_failed', ['S-07.051', 'S-02.071'],
                'Held mode; a first-stage write that fails: 0x14 after its first erase; the window stays open '
                'after the failure, so no power-off follows.',
                {'end_ms': 1000, 'actions': fs},
                held(inject=[{'t_ms': HELD_T + 20, 'id': '0x31', 'payload': '14'}]), no_power_off))
    ts.append(T('fs_hold_rekept', ['S-07.051'],
                'Held mode; a first-stage write: 0x14, 0x10, then 0x16 within the window: the last one is '
                'kept, and the power-off runs with s 16 when the window ends.',
                {'end_ms': 1000, 'actions': fs + fs_close},
                held(inject=[{'t_ms': HELD_T + 20, 'id': '0x31', 'payload': '14'},
                             {'t_ms': HELD_T + 60, 'id': '0x31', 'payload': '10'},
                             {'t_ms': HELD_T + 100, 'id': '0x31', 'payload': '16'}]),
                lambda ev: chk(fm(ev, 0x31, '16', d='P'), fm(ev, 0x32, '16'), 90, 120,
                               'S-07.051: the kept power-off runs when the window ends')))
    ts.append(T('power_16_from_other', ['S-07.042', 'S-07.051'], '0x12 then 0x16: no power-off; the charge LED.',
                {'end_ms': 600}, held(inject=[{'t_ms': HELD_T, 'id': '0x31', 'payload': '12'},
                                              {'t_ms': HELD_T + 50, 'id': '0x31', 'payload': '16'}])))
    ts.append(T('power_misc', ['S-07.052', 'S-07.075'], '0x35, 0x00, 0x45 and 0x4D in held mode: nothing sent.',
                {'end_ms': 600}, held(inject=[{'t_ms': HELD_T, 'id': '0x35', 'payload': '00'},
                                              {'t_ms': HELD_T + 10, 'id': '0x00', 'payload': '01'},
                                              {'t_ms': HELD_T + 20, 'id': '0x45', 'payload': '0102'},
                                              {'t_ms': HELD_T + 30, 'id': '0x4d', 'payload': '000102'}])))
    ts.append(T('battery_wait_flat', ['S-07.054', 'S-07.057'],
                'Battery at 3 %: the wait loop; a USB host grants 500 mA at 1.5 s; the limit reaches 500 mA.',
                {'actions': [{'t_ms': 1500, 'do': 'usb_config', 'ma': 500}]}, {'rules': flat_battery_rules()},
                battery_loop_checks))
    ts.append(T('battery_wait_charge', ['S-07.042', 'S-07.054'], 'State 0x16 at the start, then 0x10.', {},
                {'rules': [{'on': '0x30', 'kind': 'request', 'send': [{'id': '0x31', 'payload': '16'}]},
                           {'on': '0x30', 'kind': 'plain', 'after_ms': 500, 'send': [{'id': '0x31', 'payload': '10'}]},
                           {'on': '0x30', 'kind': 'plain', 'send': []},
                           {'on': '0x36', 'send': [{'id': '0x37', 'payload': '01321900641068'}]}]},
                lambda ev: battery_loop_checks(ev, charge=True)))
    ts.append(T('charge_wait_flat', ['S-07.054', 'S-07.057'],
                'State 0x16 for 5 s, battery at 3 %, no host: the charge wait asks for 500 mA and never 1000 mA; '
                'then 0x10: the wait loop, and 1000 mA after 3 s.', {},
                {'rules': [{'on': '0x36', 'send': [{'id': '0x37', 'payload': '01031900640dac'}]},
                           {'on': '0x30', 'kind': 'request', 'send': [{'id': '0x31', 'payload': '16'}]},
                           {'on': '0x30', 'kind': 'plain', 'after_ms': 5000, 'send': [{'id': '0x31', 'payload': '10'}]},
                           {'on': '0x30', 'kind': 'plain', 'send': []},
                           {'on': '0x33', 'nth': [1], 'send': [{'id': '0x33', 'payload': '0000'}]},
                           {'on': '0x33', 'send': [{'id': '0x33', 'payload': '000a'}]}]},
                lambda ev: chk(fm(ev, 0x31, '10', d='P'), fm(ev, 0x34, '0064'), 3000, 3200,
                               'S-07.054: the 3 s rule only once the switch is on')))
    ts.append(T('charge_wait_unplug', ['S-07.051', 'S-07.054'],
                'State 0x16, then 0x14 at 1.5 s (the cable pulled): the power-off from the charge wait.', {},
                {'rules': [{'on': '0x36', 'send': [{'id': '0x37', 'payload': '01321900641068'}]},
                           {'on': '0x30', 'kind': 'request', 'send': [{'id': '0x31', 'payload': '16'}]},
                           {'on': '0x30', 'kind': 'plain', 'after_ms': 1500, 'send': [{'id': '0x31', 'payload': '14'}]},
                           {'on': '0x30', 'kind': 'plain', 'send': []}]}))
    ts.append(T('battery_wait_charger', ['S-07.054', 'S-07.057'],
                'Battery at 3 %, no USB host: the target becomes 1000 mA after 3 s.', {},
                {'rules': flat_battery_rules()},
                lambda ev: battery_loop_checks(ev) + chk(fm(ev, 0x42, d='P'), fm(ev, 0x34), 3000, 3100,
                                                         'S-07.054: 1000 mA after more than 3 s')))
    ts.append(T('battery_wait_host_unconfigured', ['S-07.054', 'S-07.057'],
                'Battery at 3 %; a USB host resets the bus at 500 ms and configures the device for 500 mA only at '
                '5 s: 100 mA meanwhile, never 1000 mA.',
                {'actions': [{'t_ms': 500, 'do': 'usb_reset'}, {'t_ms': 5000, 'do': 'usb_config', 'ma': 500}]},
                {'rules': flat_battery_rules()}, battery_loop_checks))
    ts.append(T('vbus_raise', ['S-07.055', 'S-07.056'], 'Held mode; a USB host grants 500 mA; applied 100 mA.',
                {'end_ms': 700, 'actions': [{'t_ms': HELD_T, 'do': 'usb_config', 'ma': 500}]}, held()))
    ts.append(T('usb_reset_100ma', ['S-07.057'],
                'Held mode; a bus reset with no configuration: target 100 mA; VBUS status {00 00}: 0x34 {00 0A}.',
                {'end_ms': HELD_T + 200, 'actions': [{'t_ms': HELD_T, 'do': 'usb_reset'}]},
                held([{'on': '0x33', 'send': [{'id': '0x33', 'payload': '0000'}]}])))
    ts.append(T('vbus_refused', ['S-07.056'], 'Held mode; target 500 mA; VBUS status 1: a 0x33 on each pass.',
                {'end_ms': HELD_T + 60, 'actions': [{'t_ms': HELD_T, 'do': 'usb_config', 'ma': 500}]},
                held([{'on': '0x33', 'send': [{'id': '0x33', 'payload': '010a'}]}])))
    ts.append(T('pmic_set', ['S-07.061'], 'The PMIC read returns 0x9F: written 0xFF.', {},
                {'rules': [{'on': '0x73', 'send': [{'id': '0x73', 'payload': '03109f'}]}]}))
    ts.append(T('pmic_silent', ['S-07.061'], 'The PMIC read gets no reply.', {}, {'rules': [{'on': '0x73', 'send': []}]},
                lambda ev: chk(fm(ev, 0x73), fm(ev, 0x51, '3cff32'), 100, None, 'S-07.061: the read waits 100 ms')))
    ts.append(T('pmic_short', ['S-07.061', 'S-07.079'], 'The PMIC read is answered with a 2-byte payload: as no '
                'reply.', {}, {'rules': [{'on': '0x73', 'send': [{'id': '0x73', 'payload': '0310'}]}]}))
    ts.append(T('short_payloads', ['S-07.051', 'S-07.079'], 'Held mode in state 0x10; the PSoC sends 0x31 with an '
                'empty payload (no state recorded), then 0x32 {16}: the power-off.',
                {'end_ms': 1000}, held(inject=[{'t_ms': HELD_T, 'id': '0x31', 'payload': ''},
                                              {'t_ms': HELD_T + 50, 'id': '0x32', 'payload': '16'}])))
    ts.append(T('soft_reboot', ['S-07.070', 'S-07.072'], 'Held mode; the host asks for a soft reboot.',
                {'end_ms': 1000, 'actions': [{'t_ms': HELD_T, 'do': 'soft_reboot'}]}, held()))
    ts.append(T('continue_restart', ['S-07.045', 'S-07.067', 'S-07.071', 'S-07.072'],
                'Held mode; the key test runs, key 0x10 is pressed; Continue to the OS with the slave present '
                '(S-05.033): the key test\'s end, then the shutdown reports and nothing after them.',
                {'end_ms': HELD_T + 1500, 'actions': [{'t_ms': HELD_T, 'do': 'key_test_start'},
                                                      {'t_ms': HELD_T + 200, 'do': 'continue_os'}]},
                held(inject=kt[:2])))
    ts.append(T('volcal', ['S-07.078'], 'Held mode; the volume-knob calibration reset is chosen.',
                {'end_ms': 600, 'actions': [{'t_ms': HELD_T, 'do': 'volcal'}]}, held()))
    ts.append(T('psoc_ping', ['S-07.076'], 'Held mode; the PSoC pings the master.', {'end_ms': 600},
                held(inject=[{'t_ms': HELD_T, 'id': '0x01', 'payload': 'aa'}])))
    ts.append(T('psoc_ping_greeting', ['S-07.076'], 'Boot path; the PSoC pings the master during the start-up '
                'greeting sequence: answered there, before 0x30.', {'timestamp': True},
                {'inject': [{'t_ms': 0, 'id': '0x01', 'payload': 'bb'}]}))
    # S-05.025: key 0x06 is down from before the prompt opens
    prompt = [{'t_ms': PING_LATE_T, 'id': '0x42', 'payload': bm(0x06).hex()},
              {'t_ms': PROMPT_T, 'id': '0x01', 'payload': 'dd'},
              {'t_ms': PROMPT_T + 10, 'id': '0x42', 'payload': bm(0x06, 0x08).hex()},
              {'t_ms': PROMPT_T + 20, 'id': '0x42', 'payload': bm().hex()}]
    ts.append(T('boot_prompt_start', ['S-07.023', 'S-07.076'], 'A build with the boot prompt, a decision to boot; a key '
                'down before the prompt opens does not act, another key does nothing, a ping is answered, digit 1 '
                'goes on to step 7.', {'timestamp': True, 'boot_prompt': True, 'end_ms': PROMPT_T + 200},
                {'inject': prompt + [{'t_ms': PROMPT_T + 30, 'id': '0x42', 'payload': bm(0x06).hex()}]}))
    ts.append(T('boot_prompt_held', ['S-07.023'], 'As boot_prompt_start, but digit 2: step 8 and no step 7.',
                {'timestamp': True, 'boot_prompt': True, 'end_ms': PROMPT_T + 200},
                {'inject': prompt + [{'t_ms': PROMPT_T + 30, 'id': '0x42', 'payload': bm(0x07).hex()}]}))
    ts.append(T('boot_prompt_off', ['S-07.051'], 'A build with the boot prompt: the PSoC reports a power-off while it '
                'waits.', {'timestamp': True, 'boot_prompt': True, 'end_ms': PROMPT_T + 200},
                {'inject': [{'t_ms': PROMPT_T, 'id': '0x31', 'payload': '14'}]}))
    ts.append(T('psoc_ping_boot_late', ['S-07.076'], 'Boot path, battery usable; the PSoC pings the master after '
                'the battery wait: the master handles no PSoC message again before the OS is entered, so no answer '
                '.', {'timestamp': True},
                {'inject': [{'t_ms': PING_LATE_T, 'id': '0x01', 'payload': 'cc'}]}))

    def bp(name, reqs, about, bl, end=HELD_T + 2000, checks=None, inject=(), cyacd=CYACD):
        ts.append(T(name, reqs, about, {'end_ms': end, 'cyacd': cyacd,
                                        'actions': [{'t_ms': HELD_T, 'do': 'backplane'}]},
                    held(bootloader=bl, inject=inject), checks))

    def enter_checks(n):
        def f(ev):
            e = [i for i in raws(ev) if ev[i][3][:2] == b'\x01\x38'][:n]
            ee = fm(ev, 0xEE)
            return (chk(ee, e[0], 250, None, 'S-07.080: 250 ms after 0xEE')
                    + gaps(e, 250, None, 'S-07.080: 250 ms per attempt'))
        return f

    bp('backplane_ok', ['S-07.080', 'S-07.083', 'S-07.085'], 'Entered on the first attempt; one run; the PSoC '
       'greets 300 ms after the exit.', {'enter_ok_from': 1, 'greet_after_ms': 300}, checks=enter_checks(1))
    bp('backplane_enter_retry', ['S-07.080'], 'Entered on the third attempt.', {'enter_ok_from': 3, 'greet_after_ms': 300},
       checks=enter_checks(3))
    def enter_fail_checks(to_end):
        def f(ev):
            r = raws(ev)
            wait = [i for i in r if ev[i][3] == m.EXIT_PACKET]
            last = [i for i in r if i < wait[0]][-1]     # the fourth enter packet
            c = (enter_checks(4)(ev) + chk(last, wait[0], 1000, 1100, 'S-07.085: exit packets from about 1 s')
                 + gaps(wait, None, 15, 'S-07.085: about every 10 ms'))
            if to_end:
                c += chk(last, wait[-1], 1950, None, 'S-07.085: the wait lasts at least 2 s')
            return c
        return f
    bp('backplane_enter_fail', ['S-07.080', 'S-07.085'], 'The PSoC ignores 0xEE: four attempts, then framed mode '
       '(a ping is answered framed) and the return wait to its end.', {'ignore_ee': True},
       inject=[{'t_ms': HELD_T + 1300, 'id': '0x01', 'payload': 'aa'}], end=HELD_T + 3600,
       checks=enter_fail_checks(True))
    bp('backplane_enter_silent', ['S-07.080', 'S-07.085'], 'The PSoC enters its bootloader on 0xEE but answers no '
       'enter packet: four attempts, then the return wait, whose first exit packet starts the application; it '
       'greets 300 ms later and is re-greeted, which ends the wait.',
       {'enter_ok_from': 1, 'silent': True, 'greet_after_ms': 300}, checks=enter_fail_checks(False))
    bp('backplane_bad_file', ['S-07.083'], 'The .cyacd header names checksum type 02: the file is bad, the step '
       'fails before entry; no 0xEE, no raw byte, and a ping is answered framed.', {'enter_ok_from': 1},
       inject=[{'t_ms': HELD_T + 300, 'id': '0x01', 'payload': 'aa'}], end=HELD_T + 600,
       cyacd=cyacd_text(ctype=2))
    bp('backplane_run_retry', ['S-07.084'], 'The first run fails at verify row: no exit packet (the PSoC stays '
       'in its bootloader); the second run succeeds.',
       {'enter_ok_from': 1, 'fail_runs': [1], 'fail_cmd': '0x3A', 'greet_after_ms': 300})
    bp('backplane_run_retry_valid', ['S-07.084'], 'The first run fails at get flash size, before any row is '
       'programmed, so the PSoC application still validates and would start on an exit packet: no exit packet; '
       'the second run succeeds.',
       {'enter_ok_from': 1, 'fail_runs': [1], 'fail_cmd': '0x32', 'greet_after_ms': 300})

    def runfail(ev):
        r = raws(ev)
        wait = [i for i in r if ev[i][3] == m.EXIT_PACKET]
        last = [i for i in r if i < wait[0]][-1]         # the third run's failing packet: no exit after it
        return (chk(last, wait[0], 1000, 1100, 'S-07.085: exit packets from about 1 s')
                + gaps(wait, None, 15, 'S-07.085: about every 10 ms')
                + chk(last, wait[-1], 1950, None, 'S-07.085: the wait lasts at least 2 s'))
    bp('backplane_run_fail', ['S-07.084', 'S-07.085'], 'All three runs fail at verify row: no exit packet between '
       'them or after the third; the PSoC stays in its bootloader and never greets, so the return wait runs '
       'to its end.',
       {'enter_ok_from': 1, 'fail_runs': [1, 2, 3], 'fail_cmd': '0x3A', 'greet_after_ms': 300},
       end=HELD_T + 3600, checks=runfail)

    def nogreet(ev):
        r = raws(ev)
        ex = [i for i in r if ev[i][3] == m.EXIT_PACKET]
        run_exit, wait = ex[0], ex[1:]
        return (chk(run_exit, wait[0], 1000, 1100, 'S-07.085: exit packets from about 1 s')
                + gaps(wait, None, 15, 'S-07.085: about every 10 ms')
                + chk(run_exit, wait[-1], 1950, None, 'S-07.085: the wait lasts at least 2 s'))
    bp('backplane_no_greet', ['S-07.085', 'S-07.086'], 'One run succeeds; the PSoC never greets.',
       {'enter_ok_from': 1, 'greet_after_ms': None}, end=HELD_T + 2600, checks=nogreet)
    return ts


def build_transcript(t):
    ev = m.Sim(t['setup'], t['psoc'], unlock=SYNTH_UNLOCK).run()
    out = {k: v for k, v in t.items() if k != 'checks_fn'}
    out['events'] = [m.ev_to_json(e) for e in ev]
    try:
        out['checks'] = t['checks_fn'](ev)
    except LookupError as e:
        raise LookupError(f"{t['name']}: {e}")
    return out


def outputs():
    files = {'keys.tsv': keys_tsv(), 'charge_led.tsv': charge_led_tsv()}
    for name, fn in [('catalog', catalog), ('keys', keys_vectors), ('leds', leds_vectors),
                     ('power', power_vectors), ('pmic', pmic_vectors), ('cypress', cypress_vectors)]:
        files[f'vectors/{name}.json'] = json.dumps(fn(), indent=1) + '\n'
    for t in transcripts():
        files[f'transcripts/{t["name"]}.json'] = json.dumps(build_transcript(t), indent=1) + '\n'
    return files


def main(argv):
    files = outputs()
    stale = []
    for rel, text in files.items():
        p = os.path.join(HERE, rel)
        if '--write' in argv:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'w') as f:
                f.write(text)
        else:
            cur = None
            if os.path.exists(p):
                with open(p) as f:
                    cur = f.read()
            if cur != text:
                stale.append(rel)
    if stale:
        print('stale:', ' '.join(stale))
        return 1
    print(f'{len(files)} files {"written" if "--write" in argv else "current"}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
