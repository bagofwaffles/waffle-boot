#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes S-18's vectors. The first stages and the decisions come from first_stage_kind.py; the
features, profile values and supported versions are written out by hand.

    python3 -I gen_vectors.py          # rewrites vectors/*.json
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import first_stage_kind as K  # noqa: E402


def tries():
    """S-18.003's tries: a fault per try (first_stage_kind.try_read()), and what the read then decides."""
    by = {c['name']: c for c in K.cases()}
    rows = []
    for name, stage, faults in (
            ('whole_first_try', 'xy_row', []),
            ('failed_then_row', '1f_row', ['later']),
            ('failed_head_then_row', 'xy_row', ['head']),
            ('two_failed_then_row', '1f_row', ['head', 'later']),
            ('flip_then_row', '1f_row', ['flip']),
            ('three_failed_head', 'xy_row', ['head', 'head', 'head']),
            ('three_failed_later', '1f_row', ['later', 'later', 'later']),
            ('busy_head_no_further_try', '1f_row', ['busy_head']),
            ('busy_later_no_further_try', '1f_row', ['busy_later']),
            ('failed_then_busy', '1f_row', ['later', 'busy_head']),
            ('unlisted_three_tries', 'xy_length_other_crc', []),
            ('out_of_range_three_tries', 'n_c001', []),
            ('erased_three_tries', 'erased', [])):
        dev, how, n, c, made = K.tries(K.build(by[stage]), faults)
        rows.append({'name': name, 'first_stage': stage, 'faults': faults, 'tries': made, 'device': dev,
                     'how': how, 'n': n, 'c': None if c is None else '0x%08X' % c})
    return rows


def first_stage():
    return {'table': [{'n': n, 'c': '0x%08X' % c, 'device': d} for (n, c), d in K.TABLE.items()],
            'arg_offset': K.ARG_OFF, 'head': K.HEAD, 'n_max': K.N_MAX, 'n_saturated': K.N_SATURATED,
            'most_tries': K.TRIES, 'cases': K.cases(), 'tries': tries()}


def identify():
    by = {c['name']: c for c in K.cases()}
    rows = []

    def row(kind, name, boot_mode, pa2, stage):
        flash = K.build(by[stage]) if stage else b''
        r = {'kind': kind, 'name': name, 'boot_mode': boot_mode, 'pa2': pa2, 'first_stage': stage}
        r.update(K.identify(boot_mode, pa2, flash))
        rows.append(r)

    # S-18.002: the boot mode first, then the table; the strap does not decide the role
    for stage in ('xy_row', '1f_row'):
        for pa2 in (1, 0):
            row('role', 'main_%s_pa2_%d' % (stage, pa2), 1, pa2, stage)
    for bm in (0, 2, 3):
        row('role', 'second_boot_mode_%d' % bm, bm, 0, None)
    row('role', 'second_with_xy_flash', 2, 0, 'xy_row')
    # S-18.002's fallback: a boot mode other than 1 with PA2 = 1 is a main CPU all the same, flagged
    for stage in ('xy_row', '1f_row', 'xy_length_other_crc'):
        for bm in (0, 2):
            row('fallback', 'fallback_main_%s_boot_mode_%d' % (stage, bm), bm, 1, stage)
    # S-18.004: every first stage in no row
    for c in K.cases():
        if c['device'] is None:
            row('unknown', 'unknown_' + c['name'], 1, 1, c['name'])
    return {'rows': rows}


def features():
    return {'devices': list(K.DEVICES),
            'features': [{'name': n, 'OP-XY': xy, 'OP-1F': f1} for n, xy, f1 in K.FEATURES]}


def _r(name, xy, f1):
    return {'name': name, 'OP-XY': xy, 'OP-1F': f1}


def profile():
    """S-18.030, every row, per device. Pins are named (PA5); a key is its number."""
    identity = [
        _r('device.product_number', 0x21, 0x02),
        _r('device.product_base', 0x00084000, 0x00008000),
        _r('device.product_text', 'OP-XY', 'OP-1'),
        _r('device.sku_prefix', 'TE033AS', 'TE002AS'),
        _r('device.usb_pid', 0x0021, 0x0102),
        _r('device.midi_pid', 0x8021, 0x8102),
        _r('device.midi_byte', 0x21, 0x02),
        _r('device.midi_family', [0x21, 0x00], [0x02, 0x00]),
        _r('device.midi_member', [0x01, 0x00],
           {'from_sku': {'bytes': 2, 'bits': 7, 'low_first': True}, 'sku_unset': 0xFFFF,
            'when_unset': 2}),
    ]
    memory = [
        _r('device.metadata_size', 0x34, 0x28),
        _r('device.ramdisk_regions', {'free': True}, {'regions': [[0x80004000, 0x07F00000]]}),
        _r('device.ramdisk_blocks', 454656, 260096),
        _r('device.emmc_width', 8, 4),
        _r('device.emmc_sectors', {'exactly': 0x1CF8000}, {'at_least': 0x1CF8000}),
        _r('device.reset_removes', {'as': 'S-15', 'fat_erase': True},
           {'only': ['/yaffs2/user', '/yaffs2/logs'], 'fat_erase': False}),
    ]
    pins = [
        _r('device.porta_mux', 0x30002000, 0x30280000),
        _r('device.portb_mux', 0x1400FFFF, 0x1400FFFF),
        _r('device.portc_mux', 0x00855100, 0x000AAA00),
        _r('device.porta_fer', 0x00004040, 0x00004620),
        _r('device.portb_fer', 0x0000FFFF, 0x0000FFFF),
        _r('device.portc_fer', 0x00000BD3, 0x000017F3),
        _r('device.gpio_outputs', [['PA3', 'low'], ['PC11', 'high']],
           [['PA0', 'keep'], ['PA3', 'keep'], ['PA4', 'keep'], ['PA6', 'keep'], ['PC13', 'keep']]),
        _r('device.gpio_inputs', ['PA6', 'PA7', 'PC2', 'PC5'], ['PA1', 'PA2', 'PA7', 'PC2', 'PC11', 'PC14']),
        _r('device.panel_dc', 'PA5', 'PA11'),
    ]
    keys = [
        _r('device.key.count', 72, 58),
        _r('device.key.bitmap_bytes', 9, 8),
        _r('device.key.hold', {'either': [0x18, 0x0D]}, {'either': [0x39, 0x2C]}),
        _r('device.key.debug', {'either': [0x0A, 0x0B]}, {'both': [0x0A, 0x0C]}),
        _r('device.key.precomp', 0x17, None),
        _r('device.key.digit1', [0x06, 0x0A, 0x0E], [0x06, 0x2D, 0x36]),
        _r('device.key.digit2', [0x07, 0x0B, 0x0F], [0x07, 0x2E, 0x37]),
        _r('device.key.digit3', [0x08, 0x0C, 0x10], [0x08, 0x2F, 0x38]),
        _r('device.key.digit4', [0x09, 0x0D, 0x11], [0x09, 0x10, 0x39]),
        _r('device.key.digit5to8', [0x12, 0x13, 0x14, 0x15], [0x11, 0x12, 0x13, 0x14]),
        _r('device.key.shift', 0x37, 0x26),
        _r('device.key.confirm', 0x0D, 0x39),
        _r('device.key.com', 0x18, 0x2C),
        _r('device.key.recovery', 0x09, 0x09),
    ]
    screen = [
        _r('device.logo', {'as': 'S-15.132'}, {'free': True}),
    ]
    return {
        'devices': list(K.DEVICES),
        'tables': {'identity': identity, 'memory': memory, 'pins': pins, 'keys': keys, 'screen': screen},
        # the knob's click among a digit's three keys (0 first)
        'digit_knob': {'OP-XY': 1, 'OP-1F': 2},
        'provisional': [['device.key.precomp', 'OP-1F'], ['device.key.recovery', 'OP-1F']],
        # S-18.031 and S-05.023 / S-05.039: not rows of S-18.030, but per device all the same
        'device_id': {'OP-XY': 'xy', 'OP-1F': '1f', 'unknown': K.UNKNOWN_ID},
        'contract': {'OP-XY': 'XY-HANDOFF-2', 'OP-1F': '1F-HANDOFF-1'},
    }


def supported():
    """S-18.050."""
    (n, c), = [r for r, d in K.TABLE.items() if d == 'OP-1F']
    return {
        'device': 'OP-1F',
        'first_stage': {'n': n, 'c': '0x%08X' % c},
        'parts': [
            {'part': 'OS', 'version': '1.7.3'},
            {'part': 'PSoC (backplane)', 'version': '1.3.5'},
            {'part': 'Nordic application', 'version': '1.3.3'},
            {'part': 'Nordic bootloader', 'version': '1000.0.9'},
        ],
        'claimed_by_a_release': False,
    }


ALL = {'first_stage': first_stage, 'identify': identify, 'features': features, 'profile': profile,
       'supported': supported}


def main():
    os.makedirs(os.path.join(HERE, 'vectors'), exist_ok=True)
    for name, fn in ALL.items():
        with open(os.path.join(HERE, 'vectors', name + '.json'), 'w') as f:
            json.dump(fn(), f, indent=1)
            f.write('\n')


if __name__ == '__main__':
    main()
