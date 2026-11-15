#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes S-17's vectors. Every expected value here is written out by hand, not computed by a model.

    python3 -I gen_vectors.py          # rewrites vectors/*.json
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

PINMUX_REF = 'S-13b-buses-and-pins/vectors/pinmux.json'

DIFFERS_ONLY_IN = ['the hw_rev entry of the info reply (S-17.013)',
                   'the panel reset path selected by PA8, master only (S-17.003)']


def revision_matrix():
    rows = []
    for role, pa2 in (('master', 1), ('slave', 0)):
        for pa8, pa0, decoded, info, panel in ((0, 0, 0xFF, 'hw_rev:255', 'bootloader_release'),
                                               (0, 1, 4, 'hw_rev:4', 'bootloader_release'),
                                               (1, 0, 5, 'hw_rev:5', 'handshake_or_pulse'),
                                               (1, 1, 6, 'hw_rev:6', 'handshake_or_pulse')):
            master = role == 'master'
            rows.append({
                'role': role, 'pa2': pa2, 'pa8': pa8, 'pa0': pa0,
                'strap_index': '%d%d' % (pa8, pa0),
                'decoded': decoded,
                'reported_before_greeting': decoded if master else None,
                'info_entry_before_greeting': info if master else None,
                'pinmux': {'ref': PINMUX_REF, 'variant': role},
                'panel_reset': panel if master else None,
                'refused': False,
            })
    return {'rows': rows, 'differs_only_in': DIFFERS_ONLY_IN,
            'panel_reset_paths': {
                'bootloader_release': 'RESET was low >= 10 us in the strap interval; the panel counts as reset '
                                      'at the bootloader\'s own release (S-13c.004 second case)',
                'handshake_or_pulse': 'RESET stayed high; the first stage\'s handshake is honored when present '
                                      'and valid, else one reset pulse (S-13c.004)'}}


def _seq(name, events, expect, req):
    return {'name': name, 'events': events, 'info_entries': expect, 'requirements': req}


def hw_rev_sequences():
    S = []
    for pa8, pa0, e in ((0, 0, 'hw_rev:255'), (0, 1, 'hw_rev:4'), (1, 0, 'hw_rev:5'), (1, 1, 'hw_rev:6')):
        S.append(_seq('strap_only_%d%d' % (pa8, pa0),
                      [['power_on', pa8, pa0], ['metadata_read'], ['info']], [e], ['S-17.010', 'S-17.013']))
    S += [
        _seq('backplane_overwrites', [['power_on', 0, 0], ['metadata_read'], ['info'],
                                      ['greeting', [0xBB, 0x02]], ['info']],
             ['hw_rev:255', 'hw_rev:2'], ['S-17.011', 'S-17.013']),
        _seq('flag_byte_irrelevant', [['power_on', 1, 1], ['metadata_read'],
                                      ['greeting', [0xAA, 0x07]], ['info'],
                                      ['greeting', [0x00, 0x03]], ['info'],
                                      ['greeting', [0xBB, 0xFF]], ['info']],
             ['hw_rev:7', 'hw_rev:3', 'hw_rev:255'], ['S-17.011']),
        _seq('latest_wins', [['power_on', 1, 0], ['metadata_read'], ['greeting', [0xBB, 0x02]],
                             ['greeting', [0xBB, 0x05]], ['info']],
             ['hw_rev:5'], ['S-17.011']),
        _seq('short_greeting', [['power_on', 1, 1], ['metadata_read'], ['greeting', [0xBB]], ['info'],
                                ['greeting', []], ['info']],
             ['hw_rev:6', 'hw_rev:6'], ['S-17.011']),
        _seq('short_after_long', [['power_on', 1, 1], ['metadata_read'], ['greeting', [0xBB, 0x02]],
                                  ['greeting', [0xAA]], ['info']],
             ['hw_rev:2'], ['S-17.011']),
        _seq('long_greeting', [['power_on', 0, 1], ['metadata_read'], ['greeting', [0xBB, 0x03, 0x99, 0x88]],
                               ['info']],
             ['hw_rev:3'], ['S-17.011']),
        _seq('zero_byte', [['power_on', 1, 0], ['metadata_read'], ['greeting', [0xBB, 0x00]], ['info']],
             ['hw_rev:0'], ['S-17.011', 'S-17.013']),
        _seq('before_metadata_ignored', [['power_on', 1, 0], ['greeting', [0xBB, 0x02]], ['metadata_read'],
                                         ['info']],
             ['hw_rev:5'], ['S-17.011']),
        _seq('reset_forgets_backplane', [['power_on', 0, 1], ['metadata_read'], ['greeting', [0xBB, 0x02]],
                                         ['info'], ['power_on', 0, 1], ['metadata_read'], ['info']],
             ['hw_rev:2', 'hw_rev:4'], ['S-17.012']),
        _seq('record_read_failed', [['power_on', 1, 1], ['metadata_read_failed', 'record'], ['info'],
                                    ['greeting', [0xBB, 0x02]], ['info']],
             ['hw_rev:0', 'hw_rev:0'], ['S-17.015']),
        _seq('serial_read_failed', [['power_on', 1, 1], ['metadata_read_failed', 'serial'], ['info'],
                                    ['greeting', [0xBB, 0x02]], ['info']],
             ['hw_rev:0', 'hw_rev:2'], ['S-17.015']),
        _seq('read_fail_then_reset', [['power_on', 1, 0], ['metadata_read_failed', 'record'], ['info'],
                                      ['power_on', 1, 0], ['metadata_read'], ['info']],
             ['hw_rev:0', 'hw_rev:5'], ['S-17.012', 'S-17.015']),
        _seq('reset_new_strap', [['power_on', 0, 0], ['metadata_read'], ['info'], ['power_on', 1, 1],
                                 ['metadata_read'], ['info']],
             ['hw_rev:255', 'hw_rev:6'], ['S-17.001', 'S-17.012']),
    ]
    # S-17.014: the byte's value in the log line, written by hand (two hexadecimal digits)
    log = [[0, '00'], [2, '02'], [3, '03'], [7, '07'], [90, '5a'], [156, '9c'], [255, 'ff']]
    return {'sequences': S, 'backplane_log': log}


U = '0.0.0'
INITIAL = {'psoc_version': U, 'psoc_serial': '', 'nordic_app': U, 'nordic_bl': U}


def _st(**kw):
    s = dict(INITIAL)
    s.update(kw)
    return s


# S-17.024's long values: 63 characters (kept whole), 64 (the last build digit cut), 69 (the second '.' cut)
V63 = '1.2.3+' + '0' * 52 + '12345'
V64 = '1.2.3+' + '0' * 53 + '12345'
V69 = '1.' + '0' * 61 + '.3+777'
assert (len(V63), len(V64), len(V69)) == (63, 64, 69)

# S-17.032: waffle-boot's own version is a version and never the forced marker 0.1.0
OWN_VERSION = {
    'first': '0.9.13',
    'never': '0.1.0',
    'accept': ['0.9.9', '0.9.13', '1.0.0', '1.0.0+3', '1.0.1', '1.2.3', '2.0.0'],
    'reject': ['0.1.0', '0.1.0+7', '1.0', 'abc', ''],
}


def version_reports():
    values = [
        ['1.0.7', '1.0.7'], ['1.0.1+0', '1.0.1'], ['1000.0.10', '1000.0.10'], ['1.2.3+4', '1.2.3+4'],
        ['1.2', None], ['abc', None], ['', None], ['1.2.x', '1.2.0'], [' 1.2.3', '1.2.3'],
        ['70000.1.2', '4464.1.2'], ['1.2.3+', '1.2.3'], ['1.2.3.4', '1.2.3'], ['1+5.2.3', '1.2.3'],
        ['\t1.\t2.3+\t9', '1.2.3+9'], ['..', '0.0.0'],
        # S-17.024: a value is read from its first 63 characters
        [V63, '1.2.3+12345'], [V64, '1.2.3+1234'], [V69, None],
    ]
    replies = [
        # step 1 (held mode entry, run end): one exchange per peer
        {'name': 'psoc_stock', 'step': 1, 'peer': 'psoc', 'before': INITIAL,
         'payload': 'mode:application;sw_version:1.0.7;serial:PSOC0001',
         'after': _st(psoc_version='1.0.7', psoc_serial='PSOC0001')},
        {'name': 'nordic_stock', 'step': 1, 'peer': 'nordic', 'before': INITIAL,
         'payload': 'mode:application;sw_version:1.0.1;bl_version:1000.0.10;boot_slot:0;serial:NRF00001',
         'after': _st(nordic_app='1.0.1')},
        {'name': 'app_sw_version_fallback', 'step': 1, 'peer': 'nordic', 'before': INITIAL,
         'payload': 'mode:application;app_sw_version:1.0.1', 'after': _st(nordic_app='1.0.1')},
        {'name': 'sw_version_preferred', 'step': 1, 'peer': 'nordic', 'before': INITIAL,
         'payload': 'app_sw_version:2.0.0;sw_version:1.0.1', 'after': _st(nordic_app='1.0.1')},
        {'name': 'exact_key_not_prefix', 'step': 1, 'peer': 'psoc', 'before': INITIAL,
         'payload': 'sw_version_old:0.9.0;sw_version:1.0.7', 'after': _st(psoc_version='1.0.7')},
        {'name': 'prefix_only_is_missing', 'step': 1, 'peer': 'psoc', 'before': _st(psoc_version='1.0.7'),
         'payload': 'sw_versionx:0.9.0', 'after': _st(psoc_version='1.0.7')},
        {'name': 'serial_exact_key', 'step': 1, 'peer': 'psoc', 'before': INITIAL,
         'payload': 'serialnumber:XYZ;serial:PSOC0001', 'after': _st(psoc_serial='PSOC0001')},
        {'name': 'serial_18_chars', 'step': 1, 'peer': 'psoc', 'before': INITIAL,
         'payload': 'serial:ABCDEFGHIJKLMNOPQR', 'after': _st(psoc_serial='ABCDEFGHIJKLMNOPQR')},
        {'name': 'serial_19_chars', 'step': 1, 'peer': 'psoc', 'before': INITIAL,
         'payload': 'serial:ABCDEFGHIJKLMNOPQRS', 'after': _st(psoc_serial='ABCDEFGHIJKLMNOPQR')},
        {'name': 'serial_26_chars', 'step': 1, 'peer': 'psoc', 'before': INITIAL,
         'payload': 'serial:ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'after': _st(psoc_serial='ABCDEFGHIJKLMNOPQR')},
        {'name': 'version_64_chars', 'step': 1, 'peer': 'psoc', 'before': INITIAL,
         'payload': 'sw_version:' + V64 + ';serial:PSOC0001',
         'after': _st(psoc_version='1.2.3+1234', psoc_serial='PSOC0001')},
        {'name': 'version_69_chars_keeps', 'step': 1, 'peer': 'nordic', 'before': _st(nordic_app='1.0.1'),
         'payload': 'sw_version:' + V69, 'after': _st(nordic_app='1.0.1')},
        {'name': 'unparseable_keeps', 'step': 1, 'peer': 'psoc', 'before': _st(psoc_version='1.0.7'),
         'payload': 'sw_version:1.0', 'after': _st(psoc_version='1.0.7')},
        {'name': 'missing_keeps', 'step': 1, 'peer': 'nordic', 'before': _st(nordic_app='1.0.1'),
         'payload': 'mode:application', 'after': _st(nordic_app='1.0.1')},
        {'name': 'no_reply_keeps', 'step': 1, 'peer': 'psoc',
         'before': _st(psoc_version='1.0.7', psoc_serial='PSOC0001'),
         'payload': None, 'after': _st(psoc_version='1.0.7', psoc_serial='PSOC0001')},
        {'name': 'value_with_colon', 'step': 1, 'peer': 'psoc', 'before': INITIAL,
         'payload': 'sw_version:1.0.7:extra', 'after': _st(psoc_version='1.0.7')},
        {'name': 'step1_never_sets_nordic_bl', 'step': 1, 'peer': 'nordic', 'before': INITIAL,
         'payload': 'sw_version:1.0.1;bl_version:1000.0.10', 'after': _st(nordic_app='1.0.1')},
        # step 2 (start of an upgrade run): a mode exchange, then the versions exchange
        {'name': 'dfu_both', 'step': 2, 'mode_reply': 'mode:bootloader;sw_version:1.0.1',
         'payload': 'mode:bootloader;sw_version:1.0.1;bl_version:1000.0.10', 'before': INITIAL,
         'after': _st(nordic_app='1.0.1', nordic_bl='1000.0.10')},
        {'name': 'dfu_not_in_dfu', 'step': 2, 'mode_reply': 'mode:application;sw_version:1.0.1',
         'payload': 'mode:application;sw_version:1.0.1;bl_version:1000.0.10', 'before': INITIAL,
         'after': INITIAL},
        {'name': 'dfu_mode_prefix', 'step': 2, 'mode_reply': 'mode:bootloaderx',
         'payload': 'sw_version:1.0.1;bl_version:1000.0.10', 'before': INITIAL, 'after': INITIAL},
        {'name': 'dfu_mode_key_exact', 'step': 2, 'mode_reply': 'modex:bootloader;mode:application',
         'payload': 'sw_version:1.0.1;bl_version:1000.0.10', 'before': INITIAL, 'after': INITIAL},
        {'name': 'dfu_no_mode_reply', 'step': 2, 'mode_reply': None,
         'payload': 'sw_version:1.0.1;bl_version:1000.0.10', 'before': INITIAL, 'after': INITIAL},
        {'name': 'dfu_sw_missing', 'step': 2, 'mode_reply': 'mode:bootloader',
         'payload': 'mode:bootloader;bl_version:1000.0.10', 'before': INITIAL, 'after': INITIAL},
        {'name': 'dfu_app_sw_version_not_used', 'step': 2, 'mode_reply': 'mode:bootloader',
         'payload': 'app_sw_version:1.0.1;bl_version:1000.0.10', 'before': INITIAL, 'after': INITIAL},
        {'name': 'dfu_bl_missing', 'step': 2, 'mode_reply': 'mode:bootloader',
         'payload': 'mode:bootloader;sw_version:1.0.2', 'before': _st(nordic_bl='1000.0.10'),
         'after': _st(nordic_app='1.0.2', nordic_bl='1000.0.10')},
        {'name': 'dfu_no_versions_reply', 'step': 2, 'mode_reply': 'mode:bootloader', 'payload': None,
         'before': _st(nordic_app='1.0.1'), 'after': _st(nordic_app='1.0.1')},
    ]
    run_end = [
        {'name': 'both_answer', 'before': _st(psoc_version='1.0.7', nordic_app='1.0.1', nordic_bl='1000.0.10'),
         's11_sets': {'nordic_app': '1.1.0'},
         'nordic_reply': 'sw_version:1.1.0', 'psoc_reply': 'sw_version:1.0.8;serial:PSOC0001',
         'after': _st(psoc_version='1.0.8', psoc_serial='PSOC0001', nordic_app='1.1.0', nordic_bl='1000.0.10')},
        {'name': 'nordic_reports_other', 'before': _st(nordic_app='1.0.1'), 's11_sets': {'nordic_app': '1.1.0'},
         'nordic_reply': 'sw_version:1.0.1', 'psoc_reply': None,
         'after': _st(nordic_app='1.0.1')},
        {'name': 'silent_keeps_run_value', 'before': _st(nordic_app='1.0.1'), 's11_sets': {'nordic_app': '1.1.0'},
         'nordic_reply': None, 'psoc_reply': None, 'after': _st(nordic_app='1.1.0')},
    ]
    sinks = {
        'metadata': {'os_version': '1.2.0', 'sw_version': '1.2.0+3', 'serial': 'UNIT0001',
                     'dsp_serial': 'CHIPSERIAL00000042'},
        'strap_index': '11',
        'nordic_greeting': [0xBB, 156],
        'info_reply_hw_rev': 'hw_rev:156',
        'bootloader_version': '<own>',
        'psoc_reply': 'mode:application;sw_version:1.0.7;serial:PSOC0001',
        'nordic_reply': 'mode:application;sw_version:1.0.1;bl_version:1000.0.10',
        'info_screen_contains': ['1.2.0', '1.2.0+3', '<own>', '1.0.1', '1.0.7', 'UNIT0001', 'CHIPSERIAL00000042',
                                 'PSOC0001'],
        'info_screen_excludes': ['1000.0.10', 'hw_rev', '156', '9c', '9C'],
        'version_txt': {'C': '<own>', 'D': '1.0.7', 'E': '1.0.1'},
        'serial_txt': {'P': 'PSOC0001'},
        'info_reply_excludes': ['1.0.7', '1.0.1', '1000.0.10', 'PSOC0001'],
        'info_reply_bl_version': '<own>',
    }
    return {'initial': INITIAL, 'values': values, 'replies': replies, 'run_end': run_end, 'sinks': sinks,
            'own_version': OWN_VERSION}


def supported():
    return {
        'revisions': [],
        'entry_fields': ['strap_index', 'decoded', 'reported_without_nordic', 'reported_with_nordic', 'run',
                         'evidence'],
        'coprocessors': [
            {'coprocessor': 'PSoC (backplane)', 'version': '1.0.7'},
            {'coprocessor': 'Nordic application', 'version': '1.0.1'},
            {'coprocessor': 'Nordic bootloader', 'version': '1000.0.10'},
        ],
    }


ALL = {'revision_matrix': revision_matrix, 'hw_rev_sequences': hw_rev_sequences,
       'version_reports': version_reports, 'supported': supported}


def main():
    os.makedirs(os.path.join(HERE, 'vectors'), exist_ok=True)
    for name, fn in ALL.items():
        with open(os.path.join(HERE, 'vectors', name + '.json'), 'w') as f:
            json.dump(fn(), f, indent=1)
            f.write('\n')


if __name__ == '__main__':
    main()
