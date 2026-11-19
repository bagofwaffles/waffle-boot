#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes the S-09 vectors into vectors/, deterministically, built literally and independent of
bridge_model.py. Run: python3 -I -B tests/oracle/S-09-bridge/make_vectors.py"""
import hashlib
import json
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'conformance'))
from images import ldr  # noqa: E402

V = os.path.join(HERE, 'vectors')


def sha(b):
    return hashlib.sha256(bytes(b)).hexdigest()


def words(*ws):
    return b''.join(struct.pack('<I', w & 0xFFFFFFFF) for w in ws)


def dump(name, obj):
    with open(os.path.join(V, name), 'w') as f:
        json.dump(obj, f, indent=1, sort_keys=True)
        f.write('\n')


# ---- requests.json (S-09.020 - S-09.023) ----

def requests():
    items = []

    # slave_rx_len: the bytes the slave takes as the request (S-09.021); slave_session_len: the bytes it
    # expects in the session (S-09.012), the master's session length.
    def add(name, req, session, note='', slave_rx_len=None):
        items.append({'name': name, 'req': req, 'session_hex': session.hex(), 'session_len': len(session),
                      'slave_rx_len': len(session) if slave_rx_len is None else slave_rx_len, 'note': note})

    add('init', {'cmd': 0x01, 'slot': 0}, bytes.fromhex('01000000' '00000000' '00000000'))
    add('echo', {'cmd': 0x0A, 'slot': 0, 'arg0': 0x12345678}, bytes.fromhex('0a000000' '78563412' '00000000'))
    add('poll', {'cmd': 0x06, 'slot': 2}, bytes.fromhex('06020000' '00000000' '00000000'))
    add('release', {'cmd': 0x07, 'slot': 1}, bytes.fromhex('07010000' '00000000' '00000000'))
    # S-09.082: a file start's release, the file's length and its CRC-32 as the arguments
    add('release_file', {'cmd': 0x07, 'slot': 1, 'arg0': 742764, 'arg1': 0x1234ABCD},
        bytes.fromhex('07010000' '6c550b00' 'cdab3412'))
    add('select_bp1', {'cmd': 0x18, 'slot': 1, 'arg0': 1}, bytes.fromhex('18010000' '01000000' '00000000'))
    add('read_sectors', {'cmd': 0x12, 'slot': 1, 'arg0': 0x81, 'arg1': 0x80},
        bytes.fromhex('12010000' '81000000' '80000000'))
    add('pull', {'cmd': 0x20, 'slot': 0, 'len': 0x1000}, bytes.fromhex('20000010' '00000000' '00000000'),
        'len is the capacity')
    add('lseek_end', {'cmd': 0x2C, 'slot': 1, 'arg0': 3, 'arg1': -16, 'len': 2},
        bytes.fromhex('2c010200' '03000000' 'f0ffffff'), 'len is whence')
    add('read_file', {'cmd': 0x2A, 'slot': 1, 'arg0': 3, 'arg1': 0x2800, 'len': 0x1000},
        bytes.fromhex('2a010010' '03000000' '00280000'), 'len is the chunk size')
    add('access', {'cmd': 0x21, 'slot': 1, 'arg0': 0, 'path': 'user/empty.bin'},
        bytes.fromhex('21010f00' '00000000' '00000000') + b'user/empty.bin\0' + b'\0', 'path length 15 incl. NUL')
    add('opendir', {'cmd': 0x2E, 'slot': 1, 'path': 'logs'},
        bytes.fromhex('2e010500' '00000000' '00000000') + b'logs\0\0\0\0')
    add('open_creat', {'cmd': 0x29, 'slot': 1, 'arg0': 0x241, 'path': '/yaffs2/x'},
        bytes.fromhex('29010a00' '41020000' '00000000') + b'/yaffs2/x\0\0\0')
    # a 64-character path: payload 0x41, pad4 0x44, master sends 0x4C (S-09.021)
    p64 = ('dir/' + 'n' * 60).encode() + b'\0'
    add('open_path_64', {'cmd': 0x29, 'slot': 1, 'arg0': 0, 'path': p64[:-1].decode()},
        bytes.fromhex('29014100' '00000000' '00000000') + p64 + bytes(0x4C - len(p64)),
        'path length 0x41 incl. NUL: master session 0x58, request 0x50, 8 trailing bytes',
        slave_rx_len=12 + 0x44)
    add('rename', {'cmd': 0x24, 'slot': 1, 'old': 'a', 'new': 'bc'},
        bytes.fromhex('24010500' '00000000' '02000000') + b'a\0bc\0\0\0\0', 'arg1 = old length, len = total')
    sec = bytes(range(256)) * 2
    add('write_sector', {'cmd': 0x15, 'slot': 1, 'arg0': 7, 'data_hex': sec.hex()},
        bytes.fromhex('15010002' '07000000' '00000000') + sec)
    big = bytes((i * 7) & 0xFF for i in range(128 * 512))
    s = bytes.fromhex('13010000' '00100000' '80000000') + big
    items.append({'name': 'write_128_reliable', 'req': {'cmd': 0x13, 'slot': 1, 'arg0': 0x1000,
                  'data_rule': '(i*7)&0xff', 'count': 128}, 'session_sha256': sha(s), 'session_len': len(s),
                  'slave_rx_len': len(s),
                  'session_head_hex': s[:16].hex(), 'note': 'len = 128*512 mod 2^16 = 0'})
    s = bytes.fromhex('16010000' '00200000' '80000000') + big
    items.append({'name': 'write_128', 'req': {'cmd': 0x16, 'slot': 1, 'arg0': 0x2000,
                  'data_rule': '(i*7)&0xff', 'count': 128}, 'session_sha256': sha(s), 'session_len': len(s),
                  'slave_rx_len': len(s),
                  'session_head_hex': s[:16].hex(), 'note': 'len = 128*512 mod 2^16 = 0; the slave takes 128*512 '
                  'payload bytes from arg1'})
    s = bytes.fromhex('16010004' '10000000' '02000000') + big[:1024]
    items.append({'name': 'write_2', 'req': {'cmd': 0x16, 'slot': 1, 'arg0': 0x10, 'data_rule': '(i*7)&0xff',
                  'count': 2}, 'session_sha256': sha(s), 'session_len': len(s), 'slave_rx_len': len(s),
                  'session_head_hex': s[:16].hex(),
                  'note': 'len = 0x400'})
    data = bytes(range(10))
    add('push', {'cmd': 0x1E, 'slot': 3, 'data_hex': data.hex()},
        bytes.fromhex('1e030a00' '00000000' '00000000') + data + b'\0\0')
    add('erase', {'cmd': 0x14, 'slot': 1, 'arg0': 0x1000000, 'arg1': 0},
        bytes.fromhex('14010000' '00000001' '00000000'))
    # S-09.021 pad cases: a 0x2B write of n bytes 0x5A to fd 4; wire: master's payload bytes, rx: slave's
    for n, wire, rx in ((0x40, 0x40, 0x40), (0x41, 0x4C, 0x44), (0x44, 0x4C, 0x44), (0x45, 0x4C, 0x48),
                        (0x48, 0x4C, 0x48), (0x49, 0x4C, 0x4C), (0x4C, 0x4C, 0x4C)):
        pay = b'\x5a' * n
        s = words(0x2B | 1 << 8 | n << 16, 4, n) + pay + bytes(wire - n)
        add('pad_%02x' % n, {'cmd': 0x2B, 'slot': 1, 'arg0': 4, 'arg1': n, 'data_hex': pay.hex()}, s,
            'payload 0x%x: master sends 0x%x, slave takes 0x%x as the request and ignores %d trailing bytes'
            % (n, wire, rx, wire - rx), slave_rx_len=12 + rx)
    for it in items:                                     # S-09.012
        it['slave_session_len'] = it['session_len']
    return items


# ---- link_requests.json, link_test.json, link_modes.json, link_rungs.json (S-09.070 - S-09.075) ----

def closed_sums(b, start, s0):
    """The two sums of S-09.071 in closed form over k words w_i: a = start + sum w_i and
    s = s0 + k * start + sum (k - i) * w_i, both mod 2^32."""
    ws = [struct.unpack_from('<I', b, i)[0] for i in range(0, len(b), 4)]
    k = len(ws)
    a = (start + sum(ws)) % 2 ** 32
    s = (s0 + k * start + sum((k - i) * w for i, w in enumerate(ws))) % 2 ** 32
    return a, s


def crc32_bzip2(data):
    """CRC-32/BZIP2, bit by bit, over bytes taken most significant bit first: check('123456789') is
    0xFC891918."""
    c = 0xFFFFFFFF
    for b in data:
        c ^= b << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
    return c ^ 0xFFFFFFFF


def wire_order(b):
    """The bytes of b's little-endian words in the order the wire sends them: each word's most
    significant byte first (S-13b.040)."""
    return b''.join(b[i:i + 4][::-1] for i in range(0, len(b), 4))


def checks():
    """S-09.071-S-09.073, S-09.076: link requests in the plain mode, then checked sessions (request, number,
    CRC-32/BZIP2 in wire byte order). A long item gives its head, trailer and SHA-256."""
    assert crc32_bzip2(b'123456789') == 0xFC891918
    items = []

    def add(name, mode, body, note, seq=0):
        num = struct.pack('<I', seq)
        chk = num + struct.pack('<I', crc32_bzip2(wire_order(body + num)))
        sess = body + chk
        it = {'name': name, 'mode': mode, 'seq': seq, 'session_len': len(sess), 'request_len': len(body),
              'check_hex': chk.hex(), 'note': note}
        if len(sess) <= 160:
            it['session_hex'] = sess.hex()
        else:
            it.update(session_head_hex=sess[:16].hex(), session_sha256=sha(sess), data_rule='(i*7)&0xff')
        items.append(it)

    def plain(name, body, note):
        items.append({'name': name, 'mode': 0, 'session_len': len(body), 'request_len': len(body),
                      'check_hex': '', 'session_hex': body.hex(), 'note': note})

    # S-09.071, S-09.073: plain 0x19 (arg0 mode, arg1 0 try / 1 keep) and 0x1A (arg1 n, len n mod 2^16)
    plain('link_mode_try_quad', bytes.fromhex('19010000' '02000000' '00000000'), 'quad I/O, on trial')
    plain('link_mode_keep_checked', bytes.fromhex('19010000' '01000000' '01000000'),
          'single I/O with the check, kept')
    plain('link_mode_plain', bytes.fromhex('19020000' '00000000' '00000000'), 'back to the plain mode')
    plain('link_test_4', bytes.fromhex('1a010400' '00000000' '04000000' 'a55a0ff0'), 'n = 4')
    t61 = bytes((i * 37 + 11) & 0xFF for i in range(61))
    plain('link_test_61', bytes.fromhex('1a013d00' '05000000' '3d000000') + t61 + b'\0\0\0',
          'n = 61, three pad bytes')
    add('echo_checked', 1, bytes.fromhex('0a010000' '78563412' '00000000'),
        'a header alone, checked mode, no number')
    add('poll_quad', 2, bytes.fromhex('06020000' '00000000' '00000000'), 'a poll in quad I/O, number 7', 7)
    add('keep_quad', 2, bytes.fromhex('19010000' '02000000' '01000000'), '0x19 keep, on its trial')
    add('test_61', 2, bytes.fromhex('1a013d00' '05000000' '3d000000') + t61 + b'\0\0\0',
        'a padded payload: 61 bytes and three zeros, then the trailer')
    w44 = b'\x5a' * 0x44
    add('write_44', 1, bytes.fromhex('2b014400' '04000000' '44000000') + w44,
        'payload 0x44: not lengthened to 0x4C outside the plain mode; number 1', 1)
    big = bytes((i * 7) & 0xFF for i in range(0x8000))
    add('write_8000', 2, bytes.fromhex('2b010080' '04000000' '00800000') + big,
        'a file write of 0x8000 bytes: 12 + 0x8000 + 8; number 0x12345678', 0x12345678)
    return items


def test_payload(rule, n):
    """A link test payload from its rule (link_test.json)."""
    kind = rule['kind']
    if kind == 'hex':
        return bytes.fromhex(rule['hex'])
    if kind == 'fill':
        return bytes([int(rule['byte'], 16)]) * n
    if kind == 'count':
        return bytes((i + (i >> 8)) & 0xFF for i in range(n))
    if kind == 'walk':
        return b''.join(struct.pack('<I', 1 << (j % 32)) for j in range(n // 4))
    rnd = random.Random(rule['seed'])
    return bytes(rnd.randrange(256) for _ in range(n))


def link_tests():
    """S-09.071: 0x1A requests and each reply's value, by the closed form."""
    items = []
    cases = [('zeros_4', 4, {'kind': 'hex', 'hex': '00000000'}),
             ('hex_4', 4, {'kind': 'hex', 'hex': 'deadbeef'}),
             ('random_61', 61, {'kind': 'random', 'seed': 61}),
             ('count_1000', 0x1000, {'kind': 'count'}),
             ('random_1004', 0x1004, {'kind': 'random', 'seed': 0x1004}),
             ('fill_0f_7ffc', 0x7FFC, {'kind': 'fill', 'byte': '0f'}),
             ('fill_ff_8000', 0x8000, {'kind': 'fill', 'byte': 'ff'}),
             ('walk_8000', 0x8000, {'kind': 'walk'}),
             ('random_8000', 0x8000, {'kind': 'random', 'seed': 0x8000}),
             ('random_10000', 0x10000, {'kind': 'random', 'seed': 0x10000}),
             ('short_3', 3, {'kind': 'hex', 'hex': ''}),
             ('none_0', 0, {'kind': 'hex', 'hex': ''}),
             ('long_10001', 0x10001, {'kind': 'hex', 'hex': ''})]
    for name, n, rule in cases:
        taken = n if 4 <= n <= 0x10000 else 0
        p = test_payload(rule, taken) if taken else b''
        padded = p + bytes((-len(p)) % 4)
        a, s = closed_sums(padded, 0x61C88646, len(padded))
        value = a ^ ((s << 16 | s >> 16) & 0xFFFFFFFF)
        items.append({'name': name, 'n': n, 'taken': taken, 'rule': rule, 'payload_sha256': sha(p),
                      'value': value, 'reply_hex': (bytes([0x1A, 1, 0, 0]) + struct.pack('<I', value)).hex()})
    return items


def link_modes():
    """S-09.015, S-09.073, S-09.074, S-09.079: the slave's link state after each event in each state
    (mode, on trial, fallen back, recovering); executed, and value: 0x19's reply value."""
    states = {'plain': (0, False, False, False), 'plain_fell': (0, False, True, False),
              'plain_recovering': (0, False, False, True),
              'checked_trial': (1, True, False, False), 'checked_kept': (1, False, False, False),
              'quad_trial': (2, True, False, False), 'quad_kept': (2, False, False, False),
              'qdata_kept': (3, False, False, False)}
    events = ['mode_0_try', 'mode_0_keep', 'mode_1_try', 'mode_1_keep', 'mode_2_try', 'mode_2_keep',
              'mode_2_arg1_2', 'mode_3_try', 'mode_3_keep', 'link_test', 'echo', 'recovery_echo', 'access',
              'init', 'status', 'bad_check', 'short', 'ignored', 'rx_fail', 'trial_timeout']
    rows = []
    for sname, (mode, trial, fell, rec) in states.items():
        for ev in events:
            if ev == 'bad_check' and mode == 0:
                continue                              # no check in the plain mode
            nxt = (mode, trial, fell, rec)
            executed, value = False, None
            fault = ev in ('bad_check', 'short', 'ignored', 'rx_fail') or (ev == 'trial_timeout' and trial)
            other = ev in ('echo', 'recovery_echo', 'access', 'init', 'status')
            asks = ev in ('mode_0_try', 'mode_1_try', 'mode_2_try')
            if fault or (other and trial):
                nxt = (0, False, True, False) if mode != 0 else nxt
            elif ev == 'trial_timeout':
                pass
            elif ev == 'init' and mode == 0:
                executed = True                       # S-09.015
                nxt = (0, False, False, False)
            elif fell and ev != 'recovery_echo':
                pass                                  # S-09.074
            elif rec and ev not in ('recovery_echo', 'status') and not asks:
                pass                                  # S-09.074, S-09.079
            else:
                executed = True
                nxt = (mode, trial, False, (fell or rec) and not ev.startswith('mode_'))
                if ev.startswith('mode_'):
                    _, m, how = ev.split('_', 2)
                    m = int(m)
                    keep = how == 'keep'
                    if m == 0:
                        nxt = (0, False, False, False)
                    elif keep and not trial and (m, mode) in ((3, 1), (1, 3)):
                        nxt = (m, False, False, False)          # S-09.081
                    elif m <= 2 and keep:
                        nxt = (mode, False if mode == m else trial, False, False)
                    elif m <= 2:
                        nxt = (m, True, False, False)
                    value = nxt[0]
            rows.append({'state': sname, 'event': ev, 'mode': mode, 'trial': trial, 'fell': fell,
                         'recovering': rec, 'executed': executed, 'value': value,
                         'next': {'mode': nxt[0], 'trial': nxt[1], 'fell': nxt[2], 'recovering': nxt[3]}})
    return rows


def link_seq():
    """S-09.074, S-09.076: request sessions in turn to a slave in the kept checked mode, each step's outcome
    'new', 'repeat', 'fault' or 'dropped'."""
    def sess(cmd, slot, seq, payload=b'', arg0=0, arg1=0, mode=1):
        body = struct.pack('<III', cmd | slot << 8 | len(payload) << 16, arg0, arg1) + payload + \
            bytes(-len(payload) % 4)
        if mode == 0:
            return body
        num = struct.pack('<I', seq)
        return body + num + struct.pack('<I', crc32_bzip2(wire_order(body + num)))

    mk_a = sess(0x26, 1, 5, b'a\0')
    mk_b = sess(0x26, 2, 5, b'b\0')
    size5 = sess(0x05, 1, 5)
    back = [{'mode': 0, 'session_hex': sess(0x0A, 0, 0, arg0=9, mode=0).hex(), 'outcome': 'dropped',
             'note': 'an echo, not the recovery echo'},
            {'mode': 0, 'session_hex': sess(0x0A, 0, 0, arg0=0x6B3D90E5, mode=0).hex(), 'outcome': 'new',
             'note': 'the recovery echo'},
            {'mode': 0, 'session_hex': sess(0x19, 0, 0, arg0=1, mode=0).hex(), 'outcome': 'new',
             'note': '0x19 on trial'},
            {'mode': 1, 'session_hex': sess(0x19, 0, 0, arg0=1, arg1=1).hex(), 'outcome': 'new',
             'note': '0x19 keep'}]
    bad = bytearray(sess(0x1A, 0, 0, b'abcd', arg1=4))
    bad[13] ^= 0x01
    poll7 = sess(0x06, 0, 7)
    # S-09.074: steps after the recovery echo
    quiet = [{'mode': 0, 'session_hex': sess(0x26, 1, 0, b'a\0', mode=0).hex(), 'outcome': 'dropped',
              'pb2_low': False, 'note': 'an access after the recovery echo'},
             {'mode': 0, 'session_hex': sess(0x19, 0, 0, arg0=1, arg1=1, mode=0).hex(), 'outcome': 'dropped',
              'pb2_low': False, 'note': 'a keep: no mode asked for'},
             {'mode': 0, 'session_hex': sess(0x19, 0, 0, arg0=3, mode=0).hex(), 'outcome': 'dropped',
              'pb2_low': False, 'note': 'mode 3'},
             {'mode': 0, 'session_hex': sess(0x19, 0, 0, arg0=0, mode=0).hex(), 'outcome': 'new',
              'note': 'the plain mode asked for'},
             {'mode': 0, 'session_hex': sess(0x26, 1, 0, b'a\0', mode=0).hex(), 'outcome': 'new',
              'note': 'then anything'}]
    init = [{'mode': 0, 'session_hex': sess(0x01, 0, 0, mode=0).hex(), 'outcome': 'new',
             'note': 'the init exactly'},
            {'mode': 0, 'session_hex': sess(0x0A, 0, 0, arg0=0x12345678, mode=0).hex(), 'outcome': 'new',
             'note': "start-up's echo (S-09.015)"}]
    up = [{'mode': 0, 'session_hex': sess(0x19, 0, 0, arg0=1, mode=0).hex(), 'outcome': 'new',
           'note': '0x19 on trial'},
          {'mode': 1, 'session_hex': sess(0x19, 0, 0, arg0=1, arg1=1).hex(), 'outcome': 'new',
           'note': '0x19 keep'}]
    cases = [
        ('repeat_deferred', [(mk_a, 'new'), (mk_a, 'repeat')]),
        ('repeat_immediate', [(size5, 'new'), (size5, 'repeat')]),
        ('next_number', [(mk_a, 'new'), (sess(0x26, 2, 6, b'b\0'), 'new')]),
        ('same_number_other_check', [(mk_a, 'new'), (mk_b, 'fault')]),
        ('no_number', [(sess(0x26, 1, 0, b'a\0'), 'fault')]),
        ('link_numbered', [(sess(0x1A, 1, 3, b'abcd', arg1=4), 'fault')]),
        ('link_keeps_memory', [(size5, 'new'), (sess(0x1A, 2, 0, b'abcd', arg1=4), 'new'), (size5, 'repeat')]),
        ('fall_back_keeps_memory', [(size5, 'new'), (bytes(bad), 'fault')] + back + [(size5, 'repeat')]),
        ('poll_between', [(mk_a, 'new'), (poll7, 'new'), (mk_a, 'repeat')]),
        ('poll_repeat', [(mk_a, 'new'), (poll7, 'new'), (poll7, 'repeat')]),
        ('poll_after_request', [(poll7, 'new'), (size5, 'new'), (poll7, 'new')]),
        ('poll_with_request_number', [(mk_a, 'new'), (sess(0x06, 0, 5), 'fault')]),
        ('request_with_poll_number', [(poll7, 'new'), (sess(0x26, 1, 7, b'a\0'), 'fault')]),
        ('recovering', [(size5, 'new'), (bytes(bad), 'fault')] + back[:2] + quiet),
        # S-09.015
        ('init_after_fault', [(size5, 'new'), (bytes(bad), 'fault')] + init + up + [(size5, 'new')]),
        ('init_recovering', [(size5, 'new'), (bytes(bad), 'fault')] + back[:2] + init + up +
         [(size5, 'new')]),
    ]
    out = []
    for name, steps in cases:
        st = []
        for s in steps:
            if isinstance(s, dict):
                st.append(s)
            else:
                st.append({'mode': 1, 'session_hex': s[0].hex(), 'outcome': s[1]})
        out.append({'name': name, 'steps': st})
    return out


def link_rungs():
    """S-09.075: per case, the 'bad' and 'slow' rungs, the rungs the master tries from its ceiling, the one
    kept and the fall-backs; 'then': a fall-back from the kept rung and the rungs tried after it."""
    return [
        {'name': 'all_good', 'ceiling': 4, 'bad': [], 'tried': [4], 'kept': 4, 'fell': 0},
        {'name': 'top_corrupt', 'ceiling': 4, 'bad': [4], 'tried': [4, 3], 'kept': 3, 'fell': 1},
        {'name': 'quad_deaf', 'ceiling': 4, 'bad': [4, 3], 'tried': [4, 3, 2], 'kept': 2, 'fell': 2},
        {'name': 'none_above_plain', 'ceiling': 4, 'bad': [4, 3, 2, 1], 'tried': [4, 3, 2, 1], 'kept': 0,
         'fell': 4},
        {'name': 'only_rung_1', 'ceiling': 4, 'bad': [4, 3, 2], 'tried': [4, 3, 2, 1], 'kept': 1, 'fell': 3},
        {'name': 'ceiling_2', 'ceiling': 2, 'bad': [], 'tried': [2], 'kept': 2, 'fell': 0},
        {'name': 'ceiling_1', 'ceiling': 1, 'bad': [], 'tried': [1], 'kept': 1, 'fell': 0},
        {'name': 'ceiling_1_bad', 'ceiling': 1, 'bad': [1], 'tried': [1], 'kept': 0, 'fell': 1},
        {'name': 'top_slow', 'ceiling': 4, 'bad': [], 'slow': [4], 'tried': [4, 3], 'kept': 3, 'fell': 0},
        {'name': 'all_slow', 'ceiling': 4, 'bad': [], 'slow': [4, 3, 2], 'tried': [4, 3, 2, 1], 'kept': 1,
         'fell': 0},
        {'name': 'after_fall_back', 'ceiling': 4, 'bad': [], 'tried': [4], 'kept': 4, 'fell': 0,
         'then': {'fall_back_from': 4, 'tried': [3], 'kept': 3}},
        {'name': 'after_fall_back_slow', 'ceiling': 4, 'bad': [], 'slow': [3], 'tried': [4], 'kept': 4,
         'fell': 0, 'then': {'fall_back_from': 4, 'tried': [3, 2], 'kept': 2}},
        {'name': 'after_fall_back_2', 'ceiling': 2, 'bad': [], 'tried': [2], 'kept': 2, 'fell': 0,
         'then': {'fall_back_from': 2, 'tried': [1], 'kept': 1}},
        {'name': 'after_fall_back_1', 'ceiling': 1, 'bad': [], 'tried': [1], 'kept': 1, 'fell': 0,
         'then': {'fall_back_from': 1, 'tried': [1], 'kept': 1}},
    ]


# ---- pad_trail.json (S-09.021, S-09.013: a request session that ends before its trailing bytes) ----

def pad_trails():
    """0x2B writes of several payload lengths, and requests.json's open_path_64, with the session cut after
    `moved` bytes around the request's and session's ends. executed: the request arrived whole;
    complete: the slave moved every byte it expects."""
    rows = []
    reqs = [('w%02x' % n, 0x2B, 4, n) for n in (0x40, 0x41, 0x44, 0x45, 0x48, 0x49)] + [('open64', 0x29, 0, 0x41)]
    for tag, cmd, arg0, n in reqs:
        req = 12 + ((n + 3) & ~3)
        sess = 12 + (0x4C if ((n + 3) & ~3) in (0x44, 0x48) else (n + 3) & ~3)
        cuts = [req - 4, req] + ([req + 4] if req + 4 < sess else []) + ([sess] if sess != req else [])
        for k in cuts:
            rows.append({'name': '%s_at_%02x' % (tag, k), 'cmd': cmd, 'arg0': arg0, 'n': n, 'request_len': req,
                         'session_len': sess, 'moved': k, 'executed': k >= req, 'complete': k == sess})
    return rows


# ---- erase.json (S-09.040: the CMD38 argument of 0x14) ----

def erase():
    """A 0x14 at sector 0x800 in slot 1 for each argument: the eMMC commands and the reply."""
    items = []
    for name, arg in (('erase', 0), ('trim', 1), ('discard', 3), ('secure_erase', 0x80000000),
                      ('secure_trim_2', 0x80008000), ('undefined', 0x12345678), ('all_ones', 0xFFFFFFFF)):
        req = bytes.fromhex('14010000' '00080000') + struct.pack('<I', arg)
        if arg == 0:
            cmds = [[35, 0x800], [36, 0x800], [38, 0]]
            rep = bytes.fromhex('14010000' '00000000' '00000000')
        else:
            cmds = []
            rep = bytes.fromhex('14010000' 'ffffffff' '00000000')
        items.append({'name': name, 'arg1': arg, 'request_hex': req.hex(), 'emmc_commands': cmds,
                      'reply_hex': rep.hex()})
    return items


# ---- clock.json (S-09.046: the 0xBD value) ----

def clocks():
    """RTC0 CLK values (S-04.070), their dates and the 0xBD payloads (seconds since 1970-01-01 UTC). The
    register's days are the decoded days less 2 from 2001-01-01 on."""
    import datetime
    epoch = datetime.datetime(1970, 1, 1)
    items = []
    for when, reg_days in (('1970-01-01 00:00:00', 0), ('1999-12-31 23:59:59', 10956),
                           ('2000-12-29 12:00:00', 11320), ('2001-01-01 00:00:00', 11321),
                           ('2001-01-02 00:00:01', 11322), ('2025-10-21 12:34:56', 20380),
                           ('2059-09-20 23:59:59', 32767)):
        t = datetime.datetime.strptime(when, '%Y-%m-%d %H:%M:%S')
        clk = t.second | t.minute << 6 | t.hour << 12 | reg_days << 17
        value = int((t - epoch).total_seconds())
        items.append({'datetime': when, 'register_days': reg_days, 'clk': '0x%08x' % clk, 'value': value,
                      'payload_hex': struct.pack('<I', value).hex()})
    return items


def clock_runs():
    """Sequences of CLK reads and the 0xBD payloads for them: the first, then one per change."""
    t0 = 20 | 34 << 6 | 12 << 12 | 20380 << 17          # 2025-10-21 12:34:20 (decodes to day 20382)
    v0 = 20382 * 86400 + 12 * 3600 + 34 * 60 + 20
    return [
        {'name': 'ticking', 'reads': ['0x%08x' % (t0 + i) for i in range(4)],
         'payloads_hex': [struct.pack('<I', v0 + i).hex() for i in range(4)]},
        {'name': 'stopped', 'reads': ['0x%08x' % t0] * 6, 'payloads_hex': [struct.pack('<I', v0).hex()]},
        {'name': 'stopped_at_zero', 'reads': ['0x00000000'] * 6, 'payloads_hex': ['00000000']},
        {'name': 'stopped_then_set', 'reads': ['0x%08x' % t0] * 3 + ['0x00000005'] * 2,
         'payloads_hex': [struct.pack('<I', v0).hex(), '05000000']},
    ]


# ---- replies.json (S-09.030 - S-09.036) ----

def replies():
    items = []

    def add(name, kind, reply, **kw):
        d = {'name': name, 'kind': kind, 'reply_hex': reply.hex(), 'reply_len': len(reply)}
        d.update(kw)
        items.append(d)

    add('imm_init', 'immediate', bytes.fromhex('01000000' 'dabedefe'), cmd=0x01, slot=0, value=0xFEDEBEDA)
    add('imm_echo', 'immediate', bytes.fromhex('0a000000' '87a9cbed'), cmd=0x0A, slot=0, value=0xEDCBA987)
    add('imm_erase_group', 'immediate', bytes.fromhex('03010000' '00040000'), cmd=0x03, slot=1, value=0x400)
    add('imm_boot_size', 'immediate', bytes.fromhex('05010000' '00004000'), cmd=0x05, slot=1, value=0x400000)
    add('imm_release_ok', 'immediate', bytes.fromhex('07010000' '00000000'), cmd=0x07, slot=1, value=0)
    add('imm_release_noos', 'immediate', bytes.fromhex('07010000' '01000000'), cmd=0x07, slot=1, value=1)
    # S-09.079: two fall-backs, the last a failed quad receive of a 0x8000-byte write
    add('imm_status', 'status', bytes.fromhex('1b010000' '02000000' '042b0200' '00800000' 'f47f0000'
                                              'fdffffff' '00000600' '00100000' '07000000'),
        cmd=0x1B, slot=1, fell=2, why=4, why_cmd=0x2B, mode=2, len=0x8000, got=0x7FF4,
        words=[0xFFFFFFFD, 0x60000, 0x1000, 7])
    add('poll_none', 'poll_none', bytes.fromhex('06ff0000'))
    add('poll_select', 'deferred', bytes.fromhex('18010000' '00000000' '00000000'), cmd=0x18, slot=1, result=0,
        errno=0)
    add('poll_access_enoent', 'deferred', bytes.fromhex('21010000' 'ffffffff' '02000000'), cmd=0x21, slot=1,
        result=-1, errno=2)
    add('poll_rmdir_notempty', 'deferred', bytes.fromhex('27020000' 'ffffffff' '5d000000'), cmd=0x27, slot=2,
        result=-1, errno=93)
    sec = bytes((i * 3) & 0xFF for i in range(512))
    add('poll_read_sector', 'deferred', bytes.fromhex('11010000' '00000000' '00000000') + sec, cmd=0x11, slot=1,
        result=0, errno=0, data_hex=sec.hex())
    two = bytes((i * 5) & 0xFF for i in range(1024))
    add('poll_read_2', 'deferred', bytes.fromhex('12010000' '00000000' '00000000') + two, cmd=0x12, slot=1,
        result=0, errno=0, arg1=2, data_hex=two.hex())
    space = struct.pack('<Q', 2403870720)
    add('poll_totalspace', 'deferred', bytes.fromhex('32010000' '00000000' '00000000') + space, cmd=0x32, slot=1,
        result=0, errno=0, data_hex=space.hex())
    st = words(0, 0x101, 0x81FF, 1, 0, 0, 0, 13, 512, 1, 0x5F5E1000, 0x5F5E1000, 0x5F5E1000)
    add('poll_stat', 'deferred', bytes.fromhex('22010000' '00000000' '00000000') + st, cmd=0x22, slot=1, result=0,
        errno=0, data_hex=st.hex())
    sfs = words(512, 2403870720 // 512, 0x100000)
    add('poll_statfs', 'deferred', bytes.fromhex('23010000' '00000000' '00000000') + sfs, cmd=0x23, slot=1,
        result=0, errno=0, data_hex=sfs.hex())
    add('pull_none', 'pull', bytes.fromhex('20000000' '00000000'), slot=0, data_hex='', overflow=False)
    td = bytes.fromhex('0251000201aa21')            # 7 tunnel bytes
    add('pull_now', 'pull', bytes.fromhex('20000000' '07000000') + td + b'\0', slot=0, data_hex=td.hex(),
        overflow=False)
    add('pull_later', 'pull', bytes.fromhex('20000000' '07000000') + td + b'\0', slot=0, data_hex=td.hex(),
        overflow=False, note='the same bytes as the reply a poll returns for a parked pull')
    add('pull_overflow', 'pull', bytes.fromhex('20000000' '07000100') + td + b'\0', slot=0, data_hex=td.hex(),
        overflow=True)
    fd = bytes(range(5))
    add('read_piece_5', 'read_piece', bytes.fromhex('2a010000' '05000000') + fd + b'\0\0\0', slot=1, r=5,
        data_hex=fd.hex())
    add('read_piece_eof', 'read_piece', bytes.fromhex('2a010000' '00000000'), slot=1, r=0, data_hex='')
    add('read_piece_err', 'read_piece', bytes.fromhex('2a010000' 'ffffffff' '13000000'), slot=1, r=-1,
        errno=19, data_hex='')
    add('clock_sync', 'clock_sync', bytes.fromhex('0b010000' '00000000') + struct.pack('<Q', 0x123456789A),
        slot=1, sample=0x123456789A)
    return items


# ---- malformed.json (S-09.037: malformed requests of defined commands) ----

def malformed():
    """Each item: the slave's request bytes (header, payload, pad4 of len) and its deferred reply."""
    items = []

    def add(name, request, reply, note):
        items.append({'name': name, 'request_hex': request.hex(), 'slave_rx_len': len(request),
                      'reply_hex': reply.hex(), 'reply_len': len(reply), 'note': note})

    def add_long(name, head, fill, n, reply, note):
        """A long request: the 12-byte header, then n bytes of value fill; its SHA-256 is given."""
        request = head + bytes([fill]) * n
        items.append({'name': name, 'request_head_hex': head.hex(), 'payload_fill': {'byte': '%02x' % fill, 'len': n},
                      'request_sha256': sha(request), 'request_len': len(request), 'slave_rx_len': len(request),
                      'reply_hex': reply.hex(), 'reply_len': len(reply), 'note': note})

    einval_12 = lambda c: bytes([c, 1, 0, 0]) + bytes.fromhex('ffffffff' '16000000')
    add('bad_count_read_0', bytes.fromhex('12010000' '81000000' '00000000'), einval_12(0x12),
        '0x12 with count 0: no sector data')
    add('bad_count_read_129', bytes.fromhex('12010000' '81000000' '81000000'), einval_12(0x12),
        '0x12 with count 129: no sector data')
    add('bad_count_read_len4', bytes.fromhex('12010400' '81000000' '00000000') + b'\x5a' * 4, einval_12(0x12),
        '0x12 with count 0 and len 4: four payload bytes taken by len, discarded')
    add('bad_count_write_0', bytes.fromhex('16010000' '10000000' '00000000'), einval_12(0x16),
        '0x16 with count 0 and len 0: no payload')
    add('bad_count_write_0_len8', bytes.fromhex('16010800' '10000000' '00000000'), einval_12(0x16),
        '0x16 with count 0 and len 8: the payload is count x 512 = 0 bytes, len is not used')
    add_long('bad_count_write_129', bytes.fromhex('16010002' '10000000' '81000000'), 0x5A, 129 * 512, einval_12(0x16),
             '0x16 with count 129 and len 0x200 (129 x 512 mod 2^16): 129 x 512 = 0x10200 payload bytes taken '
             'from arg1, discarded')
    add_long('bad_count_rel_200', bytes.fromhex('13010500' '10000000' 'c8000000'), 0x5A, 200 * 512, einval_12(0x13),
             '0x13 with count 200 and len 5: 200 x 512 = 0x19000 payload bytes taken from arg1, discarded')
    add('read_chunk_0', bytes.fromhex('2a010000' '03000000' '00100000'), bytes.fromhex('2a010000' 'ffffffff' '16000000'),
        '0x2A with C = 0: one failed piece, the read is complete')
    add('read_chunk_2000', bytes.fromhex('2a010020' '03000000' '00100000'),
        bytes.fromhex('2a010000' 'ffffffff' '16000000'), '0x2A with C = 0x2000: one failed piece')
    add('path_no_nul', bytes.fromhex('21010400' '00000000' '00000000') + b'logs',
        bytes.fromhex('21010000' 'ffffffff' '16000000'), '0x21 path "logs" without its NUL')
    add('path_empty', bytes.fromhex('25010000' '00000000' '00000000'),
        bytes.fromhex('25010000' 'ffffffff' '16000000'), '0x25 with path length 0')
    add('opendir_no_nul', bytes.fromhex('2e010300' '00000000' '00000000') + b'abc\0',
        bytes.fromhex('2e010000' '00000000' '16000000'), 'opendir fails with 0')
    add('stat_no_nul', bytes.fromhex('22010200' '00000000' '00000000') + b'ab\0\0',
        bytes.fromhex('22010000' 'ffffffff' '16000000') + bytes(0x34), 'stat: the 52-byte record as zeros')
    add('space_no_nul', bytes.fromhex('31010100' '00000000' '00000000') + b'x\0\0\0',
        bytes.fromhex('31010000' '00000000' '16000000') + bytes(8), 'free space: result 0, errno 22, zeros')
    add('rename_old_long', bytes.fromhex('24010500' '00000000' '06000000') + b'a\0bc\0\0\0\0',
        bytes.fromhex('24010000' 'ffffffff' '16000000'), 'rename with arg1 above len')
    add('rename_new_no_nul', bytes.fromhex('24010400' '00000000' '02000000') + b'a\0bc',
        bytes.fromhex('24010000' 'ffffffff' '16000000'), 'rename whose new path lacks its NUL')
    add('path_inner_nul', bytes.fromhex('21010600' '00000000' '00000000') + b'ab\0cd\0\0\0', b'',
        'well formed: the path is "ab" (ends at its first NUL); no malformed reply')
    return items


# ---- file_read.json (S-09.035) ----

def file_reads():
    cases = []

    def case(name, flen, n, pos=0, fail_piece=None):
        rnd = random.Random(flen * 7 + n)
        f = bytes(rnd.randrange(256) for _ in range(flen))
        pieces = []
        got = 0
        k = 0
        result = None
        while result is None:
            ask = n - got if n - got < 0x1000 else 0x1000
            if k == fail_piece:
                pieces.append({'r': -1, 'errno': 19, 'reply_len': 12})
                result = [-1, 19]
                break
            avail = max(0, flen - pos - got)
            r = ask if ask < avail else avail
            seg = f[pos + got:pos + got + r]
            pieces.append({'r': r, 'sha256': sha(seg), 'reply_len': 8 + ((r + 3) // 4) * 4})
            got += r
            k += 1
            if r == 0x1000 and got < n:
                continue
            result = [got, 0]
        cases.append({'name': name, 'file_len': flen, 'file_seed': flen * 7 + n, 'pos': pos, 'n': n,
                      'chunk': 0x1000, 'fail_piece': fail_piece, 'pieces': pieces, 'result': result})

    case('n_0', 100, 0)
    case('n_1', 100, 1)
    case('n_fff', 0x3000, 0xFFF)
    case('n_1000', 0x3000, 0x1000)
    case('n_1001', 0x3000, 0x1001)
    case('n_2800', 0x3000, 0x2800)
    case('eof_on_boundary', 0x2000, 0x3000)
    case('eof_short', 0x1234, 0x3000)
    case('from_pos', 0x3000, 0x1800, pos=0x900)
    case('error_second_piece', 0x3000, 0x2800, fail_piece=1)
    return cases


# ---- slave streams (S-09.004 - S-09.006) ----

def stream_sessions(slot):
    """S-09.006: the sessions, by a walk over images.ldr's parsed blocks."""
    blocks = ldr.parse_blocks(slot)
    out = [b'\x03']
    for b in blocks:
        h = slot[b['offset']:b['offset'] + 16]
        if b['flags'] & ldr.FILL or b['count'] == 0:
            out.append(h)
            continue
        p = slot[b['payload_offset']:b['payload_offset'] + b['count']]
        out.append(h + p[:1024])
        rest = p[1024:]
        while rest:
            out.append(rest[:1024])
            rest = rest[1024:]
    return out


def stream_parts(slot):
    """S-09.009: per session after the opening one, its parts' lengths: a header-only block's session is its
    header; a block's first session is its header, then its first piece; each later piece is one part."""
    out = []
    for b in ldr.parse_blocks(slot):
        if b['flags'] & ldr.FILL or b['count'] == 0:
            out.append([16])
            continue
        out.append([16, min(1024, b['count'])])
        rest = b['count'] - 1024
        while rest > 0:
            out.append([min(1024, rest)])
            rest -= 1024
    return out


def streams():
    rnd = random.Random(9)

    def blob(n):
        return bytes(rnd.randrange(256) for _ in range(n))

    final = ldr.build_ldr([
        {'flags': ldr.FIRST | ldr.IGNORE, 'target': 0x11A00000, 'count': 0},
        {'target': 0x11A00000, 'payload': blob(3000)},          # 1024 + 1024 + 952
        {'flags': ldr.FILL, 'target': 0x11B00000, 'count': 0x100, 'fill': 0},
        {'target': 0x08000000, 'payload': blob(1024)},          # exactly one piece
        {'target': 0x08001000, 'payload': blob(1025)},          # 1024 + 1
        {'flags': ldr.FINAL, 'target': 0x11A00000, 'payload': blob(8)},
    ])
    out = {}
    sess = stream_sessions(final)
    slot = final + b'\xff' * 64                                  # erased tail after FINAL is never sent
    out['final'] = (slot, sess, 'final')
    # bad signature: the third block's header has byte 3 cleared; it and everything after are not sent
    good = ldr.build_ldr([{'flags': ldr.FIRST | ldr.IGNORE, 'target': 0x11A00000, 'count': 0},
                          {'target': 0x11A00000, 'payload': blob(100)}])
    bad = bytearray(ldr.header(0, 0x11A01000, 16, 0)) + blob(16)
    bad[3] = 0xFF
    slot = good + bytes(bad) + ldr.build_ldr([{'flags': ldr.FINAL, 'target': 0x11A00000, 'count': 0}])
    out['badsig'] = (slot, [b'\x03', good[:16], good[16:16 + 16 + 100]], 'badsig')
    # truncated: the slot ends inside the second data block's payload
    whole = ldr.build_ldr([{'target': 0x11A00000, 'payload': blob(40)},
                           {'target': 0x11A01000, 'payload': blob(2000)},
                           {'flags': ldr.FINAL, 'target': 0x11A00000, 'count': 0}])
    slot = whole[:16 + 40 + 16 + 1500]
    out['truncated'] = (slot, [b'\x03', whole[:56]], 'slot_end')
    files = {'final': 'slave_stream_final.bin', 'badsig': 'slave_stream_badsig.bin',
             'truncated': 'slave_stream_truncated.bin'}
    # S-09.009: the parts each session goes in with the option, the opening session left out: by the
    # block walk for 'final', as written for the other two
    dump('slave_stream_parts.json', {
        'final': {'session_parts': stream_parts(final)},
        'badsig': {'session_parts': [[16], [16, 100]]},
        'truncated': {'session_parts': [[16, 40]]}})
    for name, (slot, sess, end) in out.items():
        with open(os.path.join(V, files[name]), 'wb') as f:
            f.write(slot)
        dump('slave_stream_%s.json' % name, {
            'slot': files[name], 'slot_sha256': sha(slot), 'end': end,
            'sessions': [{'len': len(s), 'sha256': sha(s), 'head_hex': s[:20].hex()} for s in sess],
            'no_open_session_hex': (b'\x03' + b'\x0f' * 11).hex()})


def errno_table():
    return {'ENOENT': 2, 'EBADF': 9, 'ENOMEM': 12, 'EACCES': 13, 'EFAULT': 14, 'EBUSY': 16, 'EEXIST': 17,
            'EXDEV': 18, 'ENODEV': 19, 'ENOTDIR': 20, 'EISDIR': 21, 'EINVAL': 22, 'ENFILE': 23, 'ENOSPC': 28,
            'EROFS': 30, 'ERANGE': 34, 'ELOOP': 40, 'ENODATA': 61, 'ENAMETOOLONG': 78, 'ENOTEMPTY': 93}


def records():
    st = words(0, 0x101, 0x41FF, 1, 0, 0, 0, 0, 512, 0, 1, 2, 3)
    sfs = words(512, 4694982, 4694000)
    de = bytearray(0x10C)
    de[0:4] = words(0x101)
    de[4:8] = words(1)
    de[8:10] = b'\x0c\x01'
    de[10] = 8
    de[11:11 + 12] = b'progress.log'
    return [{'kind': 'stat', 'fields': {'dev': 0, 'ino': 0x101, 'mode': 0x41FF, 'nlink': 1, 'uid': 0, 'gid': 0,
             'rdev': 0, 'size': 0, 'blksize': 512, 'blocks': 0, 'atime': 1, 'mtime': 2, 'ctime': 3},
             'hex': st.hex(), 'len': len(st)},
            {'kind': 'statfs', 'fields': {'bsize': 512, 'blocks': 4694982, 'bfree': 4694000}, 'hex': sfs.hex(),
             'len': 12},
            {'kind': 'dirent', 'fields': {'ino': 0x101, 'off': 1, 'type': 8, 'name': 'progress.log'},
             'hex': bytes(de).hex(), 'len': 0x10C}]


def session_ends():
    """S-09.011, S-09.013: how a slave ends a session, one row per case. expect / moved: bytes; rose: PA4
    rose after the last move while PB15 read low; fell_again: PA4 then fell before PB15 rose."""
    rows = [
        # name, direction, expect, moved, rose, fell_again, conforms, note
        ('reply_complete', 'reply', 12, 12, True, False, True, 'a 12-byte deferred reply sent whole'),
        ('reply_sector_complete', 'reply', 524, 524, True, False, True, 'a 0x11 reply (12 + 512) sent whole'),
        ('request_complete', 'request', 12, 12, True, False, True, 'a request header received whole'),
        ('reply_complete_held', 'reply', 8, 8, False, False, False,
         'complete, but the line stays low: the master would find the slave absent (S-09.011)'),
        ('reply_complete_rearmed', 'reply', 8, 8, True, True, False,
         'complete, then ready again before PB15 rose: ready for a session while the master is in one'),
        ('reply_feed_stall', 'reply', 524, 260, True, False, True,
         'the feed stopped part-way; the line stays high and the master is held until its wait runs out'),
        ('reply_feed_stall_rearmed', 'reply', 524, 260, True, True, False,
         'the slave gave up and made itself ready again: the master would clock the rest into a new session'),
        ('reply_cut_short', 'reply', 524, 520, False, False, True,
         'the master ended the session one word early; the slave could still move the next word'),
        ('reply_none_held', 'reply', 8, 0, True, False, True, 'nothing sent; the line stays high'),
        ('request_rx_short', 'request', 76, 72, False, False, True,
         'a 0x2B write of 0x40 bytes that the master ended one word short; the slave was ready for it'),
        ('request_rx_stall_rearmed', 'request', 76, 72, True, True, False,
         'received one word short, then ready again before PB15 rose'),
    ]
    return [{'name': n, 'dir': d, 'expect': e, 'moved': mv, 'rose': r, 'fell_again': f, 'conforms': ok,
             'note': note} for n, d, e, mv, r, f, ok, note in rows]


# ---- quad_test.json (S-09.080) ----

def qt_payload_word(pat, seed, j):
    """Word j of a test reply's pattern, written out from each pattern's statement."""
    k = (j + seed) % 2 ** 32
    if pat in (0, 1):                    # one bit set (clear), moving up from bit seed mod 32
        w = int('1' + '0' * (k % 32), 2)
        return w if pat == 0 else 0xFFFFFFFF - w
    if pat in (2, 3):                    # one data line high (low): bit k mod 4 of every nibble
        w = int(('%x' % (1 << (k % 4))) * 8, 16)
        return w if pat == 2 else 0xFFFFFFFF - w
    if pat == 4:                         # adjacent lines at opposite levels, 0x55555555 first
        return int(('a' if k % 2 else '5') * 8, 16)
    x = ((k * 0x9E3779B9) % 2 ** 32) ^ ((seed * 128) % 2 ** 32)
    for mul, sh in ((0x85EBCA6B, 16), (0xC2B2AE35, 13)):
        x = ((x ^ (x >> sh)) * mul) % 2 ** 32
    return x ^ (x >> 16)


def quad_tests():
    """S-09.080: ARM's arguments, the test replies (header word, pattern, check word in the wire's bit
    order), the requests' headers, RECORD's length and the data lines of wrong bits."""
    def arg0(cand, ren, pat, seed, op=0):
        return op | cand << 4 | (0x40 if ren else 0) | pat << 8 | seed << 16

    args = []
    for name, a0, a1, ok in [
            ('tf_dma_12', arg0(1, 0, 0, 0), 12, True), ('tf_core_ren_max', arg0(2, 1, 5, 0xFFFF), 0x10000, True),
            ('spif', arg0(3, 0, 4, 7), 64, True), ('spif_ren', arg0(3, 1, 0, 0), 64, False),
            ('no_candidate', arg0(0, 0, 0, 0), 12, False), ('pattern_6', arg0(1, 0, 6, 0), 12, False),
            ('pattern_15', arg0(1, 0, 15, 0), 12, False), ('bit_7', arg0(1, 0, 0, 0) | 0x80, 12, False),
            ('bits_15_12', arg0(1, 0, 0, 0) | 0x1000, 12, False), ('short_8', arg0(1, 0, 0, 0), 8, False),
            ('not_words', arg0(1, 0, 0, 0), 14, False), ('long', arg0(1, 0, 0, 0), 0x10004, False),
            ('send_op', 1, 12, False), ('record_op', 2, 12, False)]:
        args.append({'name': name, 'arg0': a0, 'arg1': a1, 'ok': ok})
    replies = []
    for pat in range(6):
        for n, seed, slot in ((12, 0, 0), (64, 3, 1), (256, pat * 1000 + 31, 2)):
            ws = [0x1C | slot << 8] + [qt_payload_word(pat, seed, j) for j in range(n // 4 - 2)]
            body = words(*ws)
            reply = body + struct.pack('<I', crc32_bzip2(wire_order(body)))
            replies.append({'name': 'pattern_%d_%d' % (pat, n), 'slot': slot, 'arg0': arg0(1, 0, pat, seed),
                            'n': n, 'reply_hex': reply.hex()})
    big = [0x1C] + [qt_payload_word(5, 9, j) for j in range(0x10000 // 4 - 2)]
    body = words(*big)
    replies.append({'name': 'random_max', 'slot': 0, 'arg0': arg0(2, 0, 5, 9), 'n': 0x10000,
                    'reply_sha256': sha(body + struct.pack('<I', crc32_bzip2(wire_order(body))))})
    requests = [{'name': 'arm', 'hex': (words(0x1C, arg0(1, 0, 2, 5), 64)).hex()},
                {'name': 'send', 'hex': words(0x11C, 1, 0).hex()},
                {'name': 'record', 'hex': words(0x21C, 2, 0).hex()}]
    lines = [{'diff': d, 'lines': l} for d, l in ((0, 0), (1, 1), (0x80000000, 8), (0x22, 2),
                                                 (0x11111111, 1), (0xF0000000, 0xF), (0x48, 0xC))]
    return {'args': args, 'replies': replies, 'requests': requests, 'record_len': 72, 'lines': lines,
            'values': {'armed': 0, 'bad': 1, 'not_plain': 2, 'not_armed': 3, 'no_slave': 4}}


def stream_sources():
    """S-09.002: whether the running slot's stream is sent, by its record against the CRC of its bytes
    (S-02.016) and whether this start is that slot's trial start. Written out row by row."""
    crc = 0x1C291CA3
    rows = [
        ('good', crc, False, True),
        ('good_trial_start', crc, True, True),
        ('badcrc', crc ^ 1, False, False),
        ('badcrc_trial_start', crc ^ 1, True, False),
        ('trial_mark', crc ^ 0xFFFFFFFF, True, True),
        ('trial_mark_not_its_trial', crc ^ 0xFFFFFFFF, False, False),
    ]
    return {'_doc': 'S-09.002: the stream is sent when the record\'s crc32 equals the CRC of the slot\'s '
                    'bytes, or its complement (the trial mark, S-02.016) while this start is that '
                    'slot\'s trial start; otherwise no byte goes on the boot link.',
            'computed_crc': crc,
            'rows': [{'name': n, 'record_crc': r, 'trial_start': t, 'send': x} for n, r, t, x in rows]}


# ---- qdata.json (S-09.081) ----

def qdata():
    """S-09.081: a 0x12's finished reply in mode 3: the head session in single I/O, [hdr][result][errno]
    [the data's check word] and its check word, then the data session's bytes in quad I/O; and the same
    reply in mode 1, whole in one session. The data: byte i of sector s is (s * 7 + i * 13) mod 256; a
    failed read's are zeros, its errno 0 (S-09.040)."""
    items = []
    for name, slot, first, n, result in (('read_2', 1, 7, 2, 0), ('read_128', 2, 100, 128, 0),
                                         ('read_failed', 3, 9, 2, -1)):
        if result == 0:
            data = bytes((((first + i // 512) * 7 + (i % 512) * 13) & 0xFF) for i in range(n * 512))
        else:
            data = bytes(n * 512)                         # S-09.032: a failure's data are zeros
        hw = struct.pack('<III', 0x12 | slot << 8, result & 0xFFFFFFFF, 0)
        dchk = crc32_bzip2(wire_order(data))
        head = hw + struct.pack('<I', dchk)
        head += struct.pack('<I', crc32_bzip2(wire_order(head)))
        whole = hw + data
        whole += struct.pack('<I', crc32_bzip2(wire_order(whole)))
        items.append({'name': name, 'slot': slot, 'sector': first, 'count': n, 'result': result,
                      'head_hex': head.hex(), 'head_len': len(head), 'data_len': len(data),
                      'data_sha256': sha(data), 'data_check': dchk, 'mode1_len': len(whole),
                      'mode1_sha256': sha(whole)})
    return items


def main():
    os.makedirs(V, exist_ok=True)
    dump('slave_stream_source.json', stream_sources())
    dump('session_end.json', session_ends())
    dump('requests.json', requests())
    dump('pad_trail.json', pad_trails())
    dump('replies.json', replies())
    dump('file_read.json', file_reads())
    dump('malformed.json', malformed())
    dump('errno.json', errno_table())
    dump('records.json', records())
    dump('erase.json', erase())
    dump('clock.json', clocks())
    dump('clock_runs.json', clock_runs())
    dump('link_requests.json', checks())
    dump('link_test.json', link_tests())
    dump('link_modes.json', link_modes())
    dump('link_rungs.json', link_rungs())
    dump('link_seq.json', link_seq())
    dump('quad_test.json', quad_tests())
    dump('qdata.json', qdata())
    streams()


if __name__ == '__main__':
    main()
