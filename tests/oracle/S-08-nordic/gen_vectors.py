#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes the S-08 vectors (vectors/*.json) and timed transcripts (transcripts/*.json). Expected values
are written out by hand, not computed by s08_model.py; header bytes are assembled field by field and the
CRC-16 comes from binascii.crc_hqx, independently of the conformance package.

    python3 -I gen_vectors.py          # rewrites vectors/*.json and transcripts/*.json
"""
import binascii
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'conformance'))
from serial.frame import Frame  # noqa: E402

AB0D = '0xAB0D'
EXIT_ANS = {'id': '0x0d', 'payload': '18'}
ENTER_ANS = {'id': '0x0d', 'payload': 'ee'}
ACCEPT = {'id': '0x06', 'payload': '40'}


# ---- vectors -------------------------------------------------------------------------------------

def greeting():
    # The table of S-08.011, by hand: (state, flag) -> answer. None flag: no payload.
    table = {
        (None, f): None for f in (None, 0x00, 0xAA, 0xAB, 0xBA, 0xBB, 0xFF)}
    table.update({(0, None): ACCEPT, (0, 0x00): ACCEPT, (0, 0xAA): ACCEPT, (0, 0xAB): ACCEPT,
                  (0, 0xBA): ACCEPT, (0, 0xBB): EXIT_ANS, (0, 0xFF): ACCEPT,
                  (AB0D, None): ACCEPT, (AB0D, 0x00): ACCEPT, (AB0D, 0xAA): ENTER_ANS, (AB0D, 0xAB): ACCEPT,
                  (AB0D, 0xBA): ACCEPT, (AB0D, 0xBB): ENTER_ANS, (AB0D, 0xFF): ACCEPT})
    rows = []
    for (state, flag), ans in table.items():
        rows.append({'state': state, 'payload': '' if flag is None else '%02x' % flag, 'answer': ans,
                     'revision': None})
    # payload lengths (S-08.010): the revision byte is byte 1; bytes after it are not interpreted
    rows += [
        {'state': 0, 'payload': 'bb02', 'answer': EXIT_ANS, 'revision': 2},
        {'state': AB0D, 'payload': 'aa5a', 'answer': ENTER_ANS, 'revision': 0x5A},
        {'state': 0, 'payload': 'aaff010203', 'answer': ACCEPT, 'revision': 255},
        {'state': AB0D, 'payload': '0007', 'answer': ACCEPT, 'revision': 7},
        {'state': None, 'payload': 'bb02', 'answer': None, 'revision': None},
        {'state': None, 'payload': 'aa09ffff', 'answer': None, 'revision': None},
    ]
    sequence = [  # S-08.013: the state in force when each greeting is handled
        {'state': 0, 'payload': 'bb02', 'answer': EXIT_ANS, 'note': 'held mode'},
        {'state': AB0D, 'payload': 'bb02', 'answer': ENTER_ANS, 'note': 'after M1'},
        {'state': 0, 'payload': 'bb02', 'answer': EXIT_ANS, 'note': 'after M3'},
        {'state': 0, 'payload': 'aa02', 'answer': ACCEPT, 'note': 'application greeting'},
    ]
    # S-08.031 step 4, S-08.033: which ordinary 0x06 messages count as the Nordic's greeting (byte 0 only;
    # an empty payload never counts, whatever frame came before it)
    accept = [
        {'id': '0x06', 'payload': '40', 'counts': True},
        {'id': '0x06', 'payload': '4041', 'counts': True},
        {'id': '0x06', 'payload': '40ffffff', 'counts': True},
        {'id': '0x06', 'payload': '', 'counts': False, 'previous_byte0': '40'},
        {'id': '0x06', 'payload': '41', 'counts': False},
        {'id': '0x06', 'payload': '0040', 'counts': False},
        {'id': '0x0a', 'payload': '40', 'counts': False},
    ]
    return {
            'rows': rows, 'sequence': sequence, 'accept': accept}


def queries():
    P = 'mode:application;sw_version:1.0.7;serial:PSOC0001'
    mode = [
        ('mode:bootloader', None, 'dfu'),
        ('mode:application', None, 'not'),
        ('mode:', None, 'not'),
        ('mode:Bootloader', None, 'not'),
        ('mode:bootloader ', None, 'not'),
        ('mode:bootloader;sw_version:1.0.1;bl_version:1000.0.10;boot_slot:0', None, 'dfu'),
        ('modex:bootloader;mode:application', None, 'not'),
        ('modex:bootloader', None, None),
        ('mod:bootloader', None, None),
        ('x:1;mode:bootloader', None, 'dfu'),
        ('mode:application;mode:bootloader', None, 'not'),
        ('sw_version:1.0.1', None, None),
        ('', None, None),
        (None, P, None),
        (None, 'mode:bootloader', None),
    ]
    slot = [
        ('boot_slot:0', False, 0), ('boot_slot:1', False, 1), ('boot_slot:2', False, 2),
        ('boot_slot:7', False, 7), ('boot_slot:00', False, 0), ('boot_slot:01', False, 1),
        ('boot_slot:10', False, 10), ('boot_slot:255', False, 255), ('boot_slot:', False, None),
        ('boot_slot:x', False, None), ('boot_slot:-1', False, None), ('boot_slot:+1', False, None),
        ('boot_slot: 1', False, None), ('boot_slot:1 ', False, None), ('boot_slot:1:2', False, None),
        ('boot_slotx:0', False, None), ('mode:bootloader;sw_version:1.2.3', False, None), (None, False, None),
        ('serial:' + 'N' * 53 + ';boot_slot:1', False, 1),
        ('boot_slot:' + '0' * 20 + '1', False, 1), ('boot_slot:4294967296', False, 'other'),
        ('boot_slot:4294967297', False, 'other'), ('boot_slot:' + '9' * 30, False, 'other'),
        ('boot_slot:- 1', False, None), ('boot_slot:+-1', False, None), ('boot_slot:0x', False, None),
        ('mode:bootloader;sw_version:1.2.3;bl_version:0.4.5;boot_slot:0;serial:NRF00001', True, 0),
        ('boot_slot:1', True, 1),
    ]
    return {
            'mode': [{'reply': r, 'previous_reply': p, 'expected': e} for r, p, e in mode],
            'boot_slot': [{'reply': r, 'after_failure': f, 'expected': e} for r, f, e in slot]}


def dfu_replies():
    rows = [
        (0x02, '0x0e', '54', 'ok', None),
        (0x03, '0x0e', '54', 'ok', None),
        (0x05, '0x0e', '54', 'ok', None),
        (0x03, '0x0e', '54ffff', 'ok', None),
        (0x02, '0x0e', '92', 'busy', None),
        (0x05, '0x0e', '92', 'busy', None),
        (0x02, '0x05', '92', 'busy', None),
        (0x03, '0x0e', '92', 'fail', '') ,
        (0x03, '0x0e', '46' + b'flash error'.hex(), 'fail', b'flash error'.hex()),
        (0x02, '0x0e', '46' + b'bad %s %n'.hex(), 'fail', b'bad %s %n'.hex()),
        (0x03, '0x0a', '54', 'fail', None),
        (0x03, '0x06', '40', 'fail', None),
        (0x02, '0x0e', '', 'fail', None),
        (0x03, '0x0e', '', 'fail', None),
        (0x05, '0x0e', '', 'fail', None),
        (0x05, '0x0e', '46', 'fail', None),
    ]
    out = []
    for cmd, rid, pay, cls, text in rows:
        if text == '':
            text = None     # a one-byte busy to a chunk carries no text
        out.append({'command': cmd, 'reply_id': rid, 'payload': pay, 'class': cls, 'error_text': text})
    return {'rows': out}


def _image(n):
    return bytes((i * 7 + 3) & 0xFF for i in range(n))


def transfers():
    prepare = [
        {'version': [1, 2, 3, 4], 'type': '0xb0', 'product_word': '0x00084005',
         'payload': '02' '0001' '0002' '0003' '0004' 'b0' '00084005'},
        {'version': [1000, 0, 10, 0], 'type': '0xb1', 'product_word': '0x00084000',
         'payload': '02' '03e8' '0000' '000a' '0000' 'b1' '00084000'},
        {'version': [65535, 1, 0, 65535], 'type': '0xb0', 'product_word': '0x000843e7',
         'payload': '02' 'ffff' '0001' '0000' 'ffff' 'b0' '000843e7'},
    ]
    headers = []
    for n, ver, pw in ((500, (2, 0, 0, 0), 0x00084000), (1, (1, 0, 1, 0), 0x000843E7), (248, (1, 1, 0, 7), 0x00084001)):
        img = _image(n)
        crc = binascii.crc_hqx(img, 0)
        h = (bytes.fromhex('beefcafe') + b'\x03' + struct.pack('>I', n) + struct.pack('>H', crc)
             + b''.join(struct.pack('>H', v) for v in ver) + bytes.fromhex('0001f000') + struct.pack('>I', pw)
             + bytes(0x25))
        assert len(h) == 64
        headers.append({'image_len': n, 'version': list(ver), 'product_word': '0x%08x' % pw,
                        'crc16': '0x%04x' % crc, 'header': h.hex()})
    product = [
        {'sku': 0, 'read_ok': True, 'word': '0x00084000'},
        {'sku': 1, 'read_ok': True, 'word': '0x00084001'},
        {'sku': 999, 'read_ok': True, 'word': '0x000843e7'},
        {'sku': 1000, 'read_ok': True, 'word': '0x00084000'},
        {'sku': 0xFFFF, 'read_ok': True, 'word': '0x00084000'},
        {'sku': 5, 'read_ok': False, 'failure': 'record', 'word': '0x00000000'},
        {'sku': 5, 'read_ok': False, 'failure': 'chip_serial', 'word': '0x00000000'},
    ]
    power_ons = [  # S-08.045: the sku is read once, at start-up
        {'power_on': 1, 'boot_sku': 3, 'set_sku_during': 7, 'word': '0x%08x' % 0x00084003,
         'prepare': (bytes([0x02]) + struct.pack('>4HBI', 1, 1, 0, 0, 0xB0, 0x00084003)).hex()},
        {'power_on': 2, 'boot_sku': 7, 'set_sku_during': None, 'word': '0x%08x' % 0x00084007,
         'prepare': (bytes([0x02]) + struct.pack('>4HBI', 1, 1, 0, 0, 0xB0, 0x00084007)).hex()},
    ]
    chunks = [  # S-08.046: payload lengths of the image chunks (each is 1 + data)
        {'image_len': 1, 'chunk_payload_lens': [2]},
        {'image_len': 247, 'chunk_payload_lens': [248]},
        {'image_len': 248, 'chunk_payload_lens': [249]},
        {'image_len': 249, 'chunk_payload_lens': [249, 2]},
        {'image_len': 496, 'chunk_payload_lens': [249, 249]},
        {'image_len': 500, 'chunk_payload_lens': [249, 249, 5]},
        {'image_len': 1000, 'chunk_payload_lens': [249, 249, 249, 249, 9]},
    ]
    return {
            'crc16_check': {'input': '123456789', 'crc16': '0x31c3'},
            'prepare': prepare, 'header': headers, 'product_word': product,
            'product_word_power_ons': power_ons, 'chunks': chunks}


def messages():
    out = {}
    for name, fid, pay in (('enter', 0x0D, 'ee'), ('exit', 0x0D, '18'), ('accept', 0x06, '40'),
                           ('info_request_slot0', 0x09, ''), ('link_greeting_slot0', 0x05, '0103')):
        r = 0x83 if name.endswith('slot0') else 0x13
        f = Frame(r, 0, fid, bytes.fromhex(pay))
        out[name] = {'route': '0x%02x' % r, 'seq': 0, 'id': '0x%02x' % fid, 'payload': pay,
                     'raw': f.raw().hex(), 'wire': f.encode().hex()}
    return {
            'frames': out}


ALL = {'greeting': greeting, 'queries': queries, 'dfu_replies': dfu_replies, 'transfers': transfers,
       'messages': messages}


# ---- transcripts ---------------------------------------------------------------------------------

def info_rule(text, delay=1.0, **kw):
    return dict({'on': {'id': '0x09', 'kind': 'request'},
                 'send': [{'kind': 'reply', 'id': '0x0a', 'text': text, 'delay_ms': delay}]}, **kw)


def silent_rule(on, **kw):
    return dict({'on': on, 'send': []}, **kw)


APP = 'mode:application;sw_version:1.0.1;bl_version:1000.0.10;boot_slot:0'
PSOC_APP = 'mode:application;sw_version:1.0.7;serial:PSOC0001'
BOOT = 'mode:bootloader;sw_version:1.0.1;bl_version:1000.0.10;boot_slot:0'
INFO_Q = {'id': '0x09', 'kind': 'request', 'payload': ''}
DFU_T = {'on': {'id': '0x0d', 'kind': 'request'}, 'send': [{'kind': 'reply', 'id': '0x0e', 'payload': '54', 'delay_ms': 2}]}
CMD_ENTER = {'id': '0x0d', 'payload': 'ee', 'kind': 'message'}
CMD_EXIT = {'id': '0x0d', 'payload': '18', 'kind': 'message'}
PREP = {'id': '0x0d', 'prefix': '02', 'len': 14, 'kind': 'request'}
HDR = {'id': '0x0d', 'prefix': '03beefcafe03', 'len': 65, 'kind': 'request'}
PERF = {'id': '0x0d', 'payload': '05', 'kind': 'request'}


def chunk(n):
    return {'id': '0x0d', 'prefix': '03', 'len': n, 'kind': 'request'}


PRIOR_PSOC = ('Before t = 0 the last info reply the bootloader received from any peer is the PSoC\'s reply to '
              '0x09, with the entry mode:application (prior_info): the PSoC responder answers 0x09 with a mode '
              'entry, and the Nordic sent no info reply after the PSoC\'s last one. Only then does a bootloader '
              'that reads a query without a reply from the last info reply (N1) fail this check.')


def tr(name, about, procedure, expect, result, end_ms=None, state=0, peer=None, args=None, reqs=(),
       prior_info=None):
    d = {'name': name, 'requirements': list(reqs), 'procedure': procedure, 'state': state,
         'peer': peer or {}, 'expect': expect, 'result': result}
    if prior_info is not None:
        d['precondition'] = PRIOR_PSOC
        d['prior_info'] = prior_info
    if args:
        d['args'] = args
    if end_ms:
        d['end_ms'] = end_ms
    return d


def transcripts():
    T = []
    T.append(tr('link_greeting_answered', 'The Nordic answers the third link-greeting request after 2 ms.',
                'link_greeting',
                [{'dut': {'id': '0x05', 'payload': '0103', 'kind': 'request'}, 'count': 3, 't_ms': [0, 0],
                  'gap_ms': [5, 6]}], True, [12, 14],
                peer={'rules': [{'on': {'id': '0x05'}, 'skip': 2, 'times': 1,
                                 'send': [{'kind': 'reply', 'id': '0x06', 'payload': '40', 'delay_ms': 2}]}]},
                reqs=['S-08.002']))
    T.append(tr('link_greeting_silent', 'The Nordic never answers: 1000 requests, each waiting 5 ms.',
                'link_greeting',
                [{'dut': {'id': '0x05', 'payload': '0103', 'kind': 'request'}, 'count': 1000, 'gap_ms': [5, 6]}],
                False, [5000, 6200], reqs=['S-08.002']))
    T.append(tr('link_greeting_reply_05', 'The Nordic answers the first request with id 0x05 {0xBB, 0x02} in the '
                'request\'s reply slot: the request is complete, and no greeting answer follows.',
                'link_greeting',
                [{'dut': {'id': '0x05', 'payload': '0103', 'kind': 'request'}, 'count': 1}], True, [1, 3],
                peer={'rules': [{'on': {'id': '0x05'},
                                 'send': [{'kind': 'reply', 'id': '0x05', 'payload': 'bb02', 'delay_ms': 1}]}]},
                reqs=['S-08.002', 'S-08.010']))
    T.append(tr('greeting_idle', 'Held mode, state 0: greetings {0xAA,2} at 10 ms and {0xBB,2} at 50 ms.',
                'idle', [{'dut': dict(ACCEPT, kind='message'), 't_ms': [10, 60]},
                         {'dut': dict(EXIT_ANS, kind='message'), 't_ms': [50, 100]}], True,
                peer={'greet': [{'at_ms': 10, 'payload': 'aa02'}, {'at_ms': 50, 'payload': 'bb02'}]},
                args={'ms': 120}, reqs=['S-08.011', 'S-08.014']))
    T.append(tr('greeting_two_queued', 'Held mode, state 0xAB0D: two greetings {0xBB,2} arrive together at 10 ms, '
                'both delivered before the bootloader handles either: one answer, the second greeting is dropped.',
                'idle', [{'dut': dict(ENTER_ANS, kind='message'), 'count': 1, 't_ms': [10, 60]}], True, state=AB0D,
                peer={'greet': [{'at_ms': 10, 'payload': 'bb02'}, {'at_ms': 10, 'payload': 'bb02'}]},
                args={'ms': 100}, reqs=['S-08.014']))
    T.append(tr('greeting_after_answer', 'Held mode, state 0: a greeting {0xBB,2} at 10 ms is answered; one at '
                '20 ms, 10 ms after that answer, is dropped; one at 130 ms, 120 ms after it, is answered.',
                'idle', [{'dut': dict(EXIT_ANS, kind='message'), 'count': 1, 't_ms': [10, 60]},
                         {'dut': dict(EXIT_ANS, kind='message'), 'count': 1, 't_ms': [130, 180]}], True,
                peer={'greet': [{'at_ms': 10, 'payload': 'bb02'}, {'at_ms': 20, 'payload': 'bb02'},
                                {'at_ms': 130, 'payload': 'bb02'}]},
                args={'ms': 200}, reqs=['S-08.014']))
    T.append(tr('greeting_enter_burst', 'State 0xAB0D. The Nordic greets {0xBB,2} every 100 ms from 100 ms, and '
                'is silent until 450 ms, then reports mode:bootloader. The greetings within 250 ms are discarded; '
                'those at 300 and 400 ms queue during the queries: the first is answered after DFU is reported, '
                'the second, which arrived before that answer, is dropped.',
                'enter_dfu',
                [{'dut': CMD_ENTER, 't_ms': [0, 0]},
                 {'dut': INFO_Q, 'count': 3, 't_ms': [250, 300]},
                 {'dut': dict(ENTER_ANS, kind='message'), 'count': 1, 't_ms': [450, 470]}], True, [450, 480],
                state=AB0D,
                peer={'rules': [silent_rule({'id': '0x09'}, until_ms=450), info_rule(BOOT)],
                      'greet': [{'at_ms': 100 * k, 'payload': 'bb02'} for k in range(1, 7)]},
                reqs=['S-08.014', 'S-08.031']))
    T.append(tr('enter_dfu_ok', 'State 0xAB0D. The Nordic is silent until 450 ms, then reports mode:bootloader; '
                'it greets {0xBB,2} at 300 ms. DFU on the third query; the queued greeting is answered.',
                'enter_dfu',
                [{'dut': CMD_ENTER, 't_ms': [0, 0]},
                 {'dut': INFO_Q, 'count': 3, 't_ms': [250, 300]},
                 {'dut': dict(ENTER_ANS, kind='message'), 'after_ms': [0, 520]}], True, [450, 3000], state=AB0D,
                peer={'rules': [silent_rule({'id': '0x09'}, until_ms=450), info_rule(BOOT)],
                      'greet': [{'at_ms': 300, 'payload': 'bb02'}]}, reqs=['S-08.030', 'S-08.031']))
    T.append(tr('enter_dfu_never', 'The Nordic keeps answering mode:application: 20 queries, failure, nothing more.',
                'enter_dfu', [{'dut': CMD_ENTER, 't_ms': [0, 0]},
                              {'dut': INFO_Q, 'count': 20, 't_ms': [250, 300]}], False, [250, 3000], state=AB0D,
                peer={'rules': [info_rule(APP)]}, reqs=['S-08.031', 'S-08.034']))
    T.append(tr('enter_dfu_no_greeting', 'DFU reported on the first query, no greeting: failure 500-520 ms later.',
                'enter_dfu', [{'dut': CMD_ENTER}, {'dut': INFO_Q, 'count': 1, 't_ms': [250, 300]}], False,
                [750, 822], state=AB0D, peer={'rules': [info_rule(BOOT)]}, reqs=['S-08.031']))
    T.append(tr('enter_dfu_empty_06', 'DFU reported on the first query; the Nordic then sends 0x06 with an empty '
                'payload, right after a 0x06 {0x40} that arrived in the discarded window: it is not the greeting, '
                'failure 500-520 ms after DFU is reported.',
                'enter_dfu', [{'dut': CMD_ENTER}, {'dut': INFO_Q, 'count': 1, 't_ms': [250, 300]}], False,
                [750, 822], state=AB0D,
                peer={'rules': [info_rule(BOOT)],
                      'greet': [{'at_ms': 100, 'id': '0x06', 'payload': '40'},
                                {'at_ms': 400, 'id': '0x06', 'payload': ''}]},
                reqs=['S-08.031']))
    T.append(tr('enter_dfu_late_greeting', 'DFU reported at 251 ms, greeting at 740 ms (489 ms later): success.',
                'enter_dfu', [{'dut': CMD_ENTER}, {'dut': INFO_Q, 'count': 1, 't_ms': [250, 250]},
                              {'dut': dict(ENTER_ANS, kind='message'), 't_ms': [740, 742]}], True, [740, 760],
                state=AB0D, peer={'rules': [info_rule(BOOT)], 'greet': [{'at_ms': 740, 'payload': 'bb02'}]},
                reqs=['S-08.031']))
    T.append(tr('enter_dfu_discard', 'A greeting 100 ms after the command is discarded; the one at 400 ms is '
                'answered (state 0xAB0D, answer 0x0D {0xEE}).',
                'enter_dfu', [{'dut': CMD_ENTER}, {'dut': INFO_Q, 'count': 1, 't_ms': [250, 300]},
                              {'dut': dict(ENTER_ANS, kind='message'), 'count': 1, 't_ms': [400, 410]}], True,
                [400, 420], state=AB0D,
                peer={'rules': [info_rule(BOOT)], 'greet': [{'at_ms': 100, 'payload': 'bb02'},
                                                            {'at_ms': 400, 'payload': 'aa02'}]},
                reqs=['S-08.015', 'S-08.031']))
    T.append(tr('reboot_enter_dfu_state0', 'A reboot request that puts the Nordic into DFU, state 0. The Nordic '
                'greets {0xBB,2} 300 ms after the command, from DFU, and is answered 0x0D {0x18} (open question 1).',
                'enter_dfu', [{'dut': CMD_ENTER}, {'dut': INFO_Q, 'count': 1},
                              {'dut': dict(EXIT_ANS, kind='message'), 't_ms': [300, 302]}], True, [300, 320], state=0,
                peer={'rules': [info_rule(BOOT),
                                {'on': CMD_ENTER, 'send': [{'kind': 'message', 'id': '0x05', 'payload': 'bb02',
                                                             'delay_ms': 300}]}]},
                reqs=['S-08.011', 'S-08.031']))
    T.append(tr('exit_dfu_ok', 'The Nordic is silent for 300 ms after the command, then reports mode:application.',
                'exit_dfu', [{'dut': CMD_EXIT, 't_ms': [0, 0]},
                             {'dut': INFO_Q, 'count': [3, 4], 't_ms': [0, 10], 'gap_ms': [100, 103]}], True,
                [300, 420], peer={'rules': [silent_rule({'id': '0x09'}, until_ms=300), info_rule(APP)]},
                reqs=['S-08.032', 'S-08.033']))
    T.append(tr('exit_dfu_greeting', 'mode:bootloader until 200 ms, silent until 400 ms, then mode:application; the '
                'application greets {0xAA,2} at 350 ms (answered 0x06 {0x40}); phase 2 succeeds on its first query.',
                'exit_dfu', [{'dut': CMD_EXIT}, {'dut': INFO_Q, 'count': [60, 110], 'gap_ms': [1, 104]},
                             {'dut': dict(ACCEPT, kind='message'), 't_ms': [350, 405]},
                             {'dut': INFO_Q, 'count': 1, 't_ms': [400, 410]}], True, [400, 420],
                peer={'rules': [info_rule(BOOT, until_ms=200), silent_rule({'id': '0x09'}, until_ms=400),
                                info_rule(APP)], 'greet': [{'at_ms': 350, 'payload': 'aa02'}]},
                reqs=['S-08.033']))
    T.append(tr('exit_dfu_stuck', 'The Nordic answers mode:bootloader within 1 ms, always: 2500 queries in phase 1, '
                '20 in phase 2, failure, nothing more.',
                'exit_dfu', [{'dut': CMD_EXIT}, {'dut': INFO_Q, 'count': 2520}], False, [5000, 12500],
                peer={'rules': [info_rule(BOOT)]}, reqs=['S-08.033', 'S-08.034']))
    T.append(tr('exit_dfu_silent', 'No reply at all: phase 1 ends after 100 queries with no reply, phase 2 makes '
                '20, failure about 12.1 s after the command. A bootloader that reads the PSoC\'s last reply '
                '(mode:application) as the Nordic\'s answer succeeds at once and fails the check.',
                'exit_dfu', [{'dut': CMD_EXIT}, {'dut': INFO_Q, 'count': 120, 'gap_ms': [100, 103]}], False,
                [12000, 12300], reqs=['S-08.020', 'S-08.033'], prior_info=PSOC_APP))
    T.append(tr('exit_dfu_slow_stuck', 'The Nordic answers mode:bootloader 5 ms after each query, always: 2500 '
                'queries in phase 1 (about 15 s, past any 10 s cut), 20 in phase 2, failure.',
                'exit_dfu', [{'dut': CMD_EXIT}, {'dut': INFO_Q, 'count': 2520}], False, [15000, 17700],
                peer={'rules': [info_rule(BOOT, delay=5.0)]}, reqs=['S-08.033']))
    img1 = {'image_len': 1, 'version': [1, 1, 0, 0], 'type': '0xb0', 'product_word': '0x00084000'}
    T.append(tr('transfer_ok', 'A 500-byte Nordic application: query, prepare, header chunk, 3 image chunks, '
                'perform, each after the previous success.',
                'transfer', [{'dut': INFO_Q}, {'dut': PREP, 'after_ms': [0, 1002]}, {'dut': HDR, 'after_ms': [2, 1002]},
                             {'dut': chunk(249), 'count': 2, 'after_ms': [2, 1002], 'gap_ms': [2, 1002]},
                             {'dut': chunk(5), 'after_ms': [2, 1002]}, {'dut': PERF, 'after_ms': [2, 1002]}], True,
                state=AB0D, peer={'rules': [info_rule(BOOT), DFU_T]},
                args={'image_len': 500, 'version': [1, 1, 0, 0], 'type': '0xb0', 'product_word': '0x00084000'},
                reqs=['S-08.042', 'S-08.043', 'S-08.044', 'S-08.046', 'S-08.047']))
    log = {'kind': 'message', 'id': '0x03', 'route': '0x32', 'text': 'dfu: request', 'delay_ms': 0.5}
    T.append(tr('transfer_log_messages', 'transfer_ok with a Nordic that sends a log message (id 0x03, to '
                'destination 2) before each reply: the same requests in the same order, and success.',
                'transfer', [{'dut': INFO_Q}, {'dut': PREP, 'after_ms': [0, 1002]}, {'dut': HDR, 'after_ms': [2, 1002]},
                             {'dut': chunk(249), 'count': 2, 'after_ms': [2, 1002], 'gap_ms': [2, 1002]},
                             {'dut': chunk(5), 'after_ms': [2, 1002]}, {'dut': PERF, 'after_ms': [2, 1002]}], True,
                state=AB0D,
                peer={'rules': [dict(info_rule(BOOT), send=[log, {'kind': 'reply', 'id': '0x0a', 'text': BOOT,
                                                                 'delay_ms': 1.0}]),
                                dict(DFU_T, send=[log] + DFU_T['send'])]},
                args={'image_len': 500, 'version': [1, 1, 0, 0], 'type': '0xb0', 'product_word': '0x00084000'},
                reqs=['S-08.004', 'S-08.047']))
    T.append(tr('transfer_not_in_dfu', 'The Nordic answers mode:application: one query, failure, no DFU request.',
                'transfer', [{'dut': INFO_Q}], False, state=AB0D, peer={'rules': [info_rule(APP), DFU_T]},
                args=img1, reqs=['S-08.042']))
    T.append(tr('transfer_query_silent', 'No answer to the first two queries, mode:bootloader to the third. A '
                'bootloader that reads the PSoC\'s last reply (mode:application) for the first query fails the '
                'transfer with no DFU request and fails the check.',
                'transfer', [{'dut': INFO_Q, 'count': 3}, {'dut': PREP}, {'dut': HDR}, {'dut': chunk(2)},
                             {'dut': PERF}], True, state=AB0D,
                peer={'rules': [silent_rule({'id': '0x09'}, times=2), info_rule(BOOT), DFU_T]},
                args=img1, reqs=['S-08.042'], prior_info=PSOC_APP))
    T.append(tr('prepare_busy', 'The prepare is answered busy, busy again 1 s later, then T 2 s after it.',
                'transfer', [{'dut': INFO_Q}, {'dut': PREP}, {'dut': HDR, 'after_ms': [2000, 3002]},
                             {'dut': chunk(2)}, {'dut': PERF}], True, state=AB0D,
                peer={'rules': [info_rule(BOOT),
                                {'on': {'id': '0x0d', 'first': '0x02'},
                                 'send': [{'kind': 'reply', 'id': '0x0e', 'payload': '92', 'delay_ms': 2},
                                          {'kind': 'reply', 'id': '0x0e', 'payload': '92', 'delay_ms': 1000},
                                          {'kind': 'reply', 'id': '0x0e', 'payload': '54', 'delay_ms': 2000}]},
                                DFU_T]},
                args=img1, reqs=['S-08.041']))
    T.append(tr('prepare_busy_endless', 'The prepare is answered busy, and busy again every 4.5 s: the request '
                'fails 60 s after the first busy reply, nothing more sent.',
                'transfer', [{'dut': INFO_Q}, {'dut': PREP}], False, [60000, 60100], state=AB0D,
                peer={'rules': [info_rule(BOOT),
                                {'on': {'id': '0x0d', 'first': '0x02'},
                                 'send': [{'kind': 'reply', 'id': '0x0e', 'payload': '92', 'delay_ms': 2 + 4500 * i}
                                          for i in range(20)]},
                                DFU_T]},
                args=img1, reqs=['S-08.041']))
    T.append(tr('perform_busy', 'The perform is answered busy, then T 2 s later: success. A greeting during the '
                'wait may be dropped; if it is kept it is answered after the transfer.',
                'transfer', [{'dut': INFO_Q}, {'dut': PREP}, {'dut': HDR}, {'dut': chunk(2)}, {'dut': PERF},
                             {'dut': dict(ENTER_ANS, kind='message'), 'count': [0, 1]}], True, [2000, 2100],
                state=AB0D,
                peer={'rules': [info_rule(BOOT),
                                {'on': {'id': '0x0d', 'first': '0x05'},
                                 'send': [{'kind': 'reply', 'id': '0x0e', 'payload': '92', 'delay_ms': 2},
                                          {'kind': 'message', 'id': '0x05', 'payload': 'bb02', 'delay_ms': 500},
                                          {'kind': 'reply', 'id': '0x0e', 'payload': '54', 'delay_ms': 2000}]},
                                DFU_T]},
                args=img1, reqs=['S-08.015', 'S-08.041']))
    T.append(tr('perform_busy_0d', 'The perform is answered busy, then T 2 s later as a message with id 0x0D: '
                'success (S-08.041, open question 4).',
                'transfer', [{'dut': INFO_Q}, {'dut': PREP}, {'dut': HDR}, {'dut': chunk(2)}, {'dut': PERF}], True,
                [2000, 2100], state=AB0D,
                peer={'rules': [info_rule(BOOT),
                                {'on': {'id': '0x0d', 'first': '0x05'},
                                 'send': [{'kind': 'reply', 'id': '0x0e', 'payload': '92', 'delay_ms': 2},
                                          {'kind': 'message', 'id': '0x0d', 'payload': '54', 'delay_ms': 2000}]},
                                DFU_T]},
                args=img1, reqs=['S-08.040', 'S-08.041']))
    for name, delay, ok in (('prepare_late_ok', 1450, True), ('prepare_too_late', 1550, False)):
        exp = [{'dut': INFO_Q}, {'dut': PREP}]
        if ok:
            exp += [{'dut': HDR, 'after_ms': [1450, 2452]}, {'dut': chunk(2)}, {'dut': PERF}]
        T.append(tr(name, 'T arrives %d ms after the prepare.' % delay, 'transfer', exp, ok, state=AB0D,
                    peer={'rules': [info_rule(BOOT),
                                    {'on': {'id': '0x0d', 'first': '0x02'},
                                     'send': [{'kind': 'reply', 'id': '0x0e', 'payload': '54', 'delay_ms': delay}]},
                                    DFU_T]},
                    args=img1, reqs=['S-08.041', 'S-08.047']))
    T.append(tr('chunk_silent', 'A 1000-byte image; no reply to the fourth chunk: failure 1 s later, nothing more.',
                'transfer', [{'dut': INFO_Q}, {'dut': PREP}, {'dut': HDR}, {'dut': chunk(249), 'count': 3}], False,
                state=AB0D,
                peer={'rules': [info_rule(BOOT), silent_rule({'id': '0x0d', 'first': '0x03'}, skip=3, times=1),
                                DFU_T]},
                args=dict(img1, image_len=1000), reqs=['S-08.041', 'S-08.047']))
    T.append(tr('chunk_error_text', 'The second chunk is answered 0x0E {"F", text holding %s}: failure.',
                'transfer', [{'dut': INFO_Q}, {'dut': PREP}, {'dut': HDR}, {'dut': chunk(249)}], False, state=AB0D,
                peer={'rules': [info_rule(BOOT),
                                {'on': {'id': '0x0d', 'first': '0x03'}, 'skip': 1, 'times': 1,
                                 'send': [{'kind': 'reply', 'id': '0x0e',
                                           'payload': '46' + b'write failed %s'.hex(), 'delay_ms': 2}]},
                                DFU_T]},
                args=dict(img1, image_len=1000), reqs=['S-08.040', 'S-08.047']))
    T.append(tr('chunk_empty_reply', 'The second chunk is answered 0x0E with an empty payload, after a T to the '
                'first: failure, nothing more sent.',
                'transfer', [{'dut': INFO_Q}, {'dut': PREP}, {'dut': HDR}, {'dut': chunk(249)}], False, state=AB0D,
                peer={'rules': [info_rule(BOOT),
                                {'on': {'id': '0x0d', 'first': '0x03'}, 'skip': 1, 'times': 1,
                                 'send': [{'kind': 'reply', 'id': '0x0e', 'payload': '', 'delay_ms': 2}]},
                                DFU_T]},
                args=dict(img1, image_len=1000), reqs=['S-08.040', 'S-08.047']))
    T.append(tr('perform_busy_after_greeting', 'A greeting arrives during the second chunk of a 500-byte image; '
                'the perform is then answered busy, then T. The greeting may be discarded by the busy wait; if '
                'kept it is answered after the transfer.',
                'transfer', [{'dut': INFO_Q}, {'dut': PREP}, {'dut': HDR}, {'dut': chunk(249), 'count': 2},
                             {'dut': chunk(5)}, {'dut': PERF},
                             {'dut': dict(ENTER_ANS, kind='message'), 'count': [0, 1]}], True, [1000, 1100],
                state=AB0D,
                peer={'rules': [info_rule(BOOT),
                                {'on': {'id': '0x0d', 'first': '0x03'}, 'skip': 1, 'times': 1,
                                 'send': [{'kind': 'message', 'id': '0x05', 'payload': 'bb02', 'delay_ms': 1},
                                          {'kind': 'reply', 'id': '0x0e', 'payload': '54', 'delay_ms': 2}]},
                                {'on': {'id': '0x0d', 'first': '0x05'},
                                 'send': [{'kind': 'reply', 'id': '0x0e', 'payload': '92', 'delay_ms': 2},
                                          {'kind': 'reply', 'id': '0x0e', 'payload': '54', 'delay_ms': 1000}]},
                                DFU_T]},
                args={'image_len': 500, 'version': [1, 1, 0, 0], 'type': '0xb0', 'product_word': '0x00084000'},
                reqs=['S-08.015', 'S-08.041']))
    T.append(tr('run_start', 'Start of a run with a Nordic image, state 0xAB0D: enter DFU, then the version query '
                '(a mode query and, the Nordic being in DFU, a second info exchange).',
                'run_start', [{'dut': CMD_ENTER}, {'dut': INFO_Q, 'count': 1},
                              {'dut': dict(ENTER_ANS, kind='message')}, {'dut': INFO_Q, 'count': 2}], True,
                state=AB0D, peer={'rules': [info_rule(BOOT),
                                            {'on': CMD_ENTER, 'send': [{'kind': 'message', 'id': '0x05',
                                                                         'payload': 'bb02', 'delay_ms': 300}]}]},
                reqs=['S-08.023', 'S-08.035']))
    T.append(tr('run_end_in_dfu', 'End of a run, state 0: the Nordic answers mode:bootloader, so the exit exchange '
                'follows; it reports mode:application from 50 ms.',
                'run_end', [{'dut': INFO_Q, 'count': 1}, {'dut': CMD_EXIT},
                            {'dut': INFO_Q, 'count': [1, 40]}], True, [50, 60],
                peer={'rules': [info_rule(BOOT, until_ms=50), info_rule(APP)]}, reqs=['S-08.035']))
    T.append(tr('run_fails_at_entry', 'A whole run whose package carries a Nordic image, state 0xAB0D: the Nordic keeps '
                'answering mode:application, so the enter exchange fails after 20 queries and the run fails at its '
                'entry (S-11.061 item 2); no end-of-run DFU-mode query follows.',
                'run', [{'dut': CMD_ENTER, 't_ms': [0, 0]}, {'dut': INFO_Q, 'count': 20, 't_ms': [250, 300]}], False,
                [250, 3000], state=AB0D, peer={'rules': [info_rule(APP)]}, reqs=['S-08.035', 'S-08.031']))
    T.append(tr('run_end_silent', 'End of a run: the query gets no reply, so no exit exchange is made.',
                'run_end', [{'dut': INFO_Q, 'count': 1}], True, [100, 103], reqs=['S-08.035']))
    return {t['name']: t for t in T}


def write():
    for name, fn in ALL.items():
        with open(os.path.join(HERE, 'vectors', name + '.json'), 'w') as f:
            json.dump(fn(), f, indent=1)
            f.write('\n')
    for name, t in transcripts().items():
        with open(os.path.join(HERE, 'transcripts', name + '.json'), 'w') as f:
            json.dump(t, f, indent=1)
            f.write('\n')


if __name__ == '__main__':
    write()
