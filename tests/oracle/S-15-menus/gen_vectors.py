# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates the model-derived vectors of S-15: vectors/handover_frame.json (S-15.132) and
vectors/select_keys.json (S-15.116), vectors/sig_prompt.json (S-15.197), and the OP-1F's (S-15.016, S-15.052, S-15.196): digits_1f.tsv,
keytest_1f.json and select_keys_1f.json (the OP-XY's vectors with each key replaced by S-15.016's, and
controls: a hold started by 0x36, a 59th key, the precomp exception), factory_reset_1f.json and
mtp_item_1f.json. The other vectors are written by hand.
Run: python3 -I -B tests/oracle/S-15-menus/gen_vectors.py"""
import csv
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import s15_model as m  # noqa: E402


def select_cases(dev='xy'):
    """S-15.116: (case, pickable steps, down_at_open, events as (ms, key, 'down' | 'up'), end_ms). On the
    OP-1F each key is S-15.016's for its role, with the controls added."""
    D, U = 'down', 'up'
    ALL = [0, 1, 2, 3, 4, 5]
    OS = [(0, 0x07, D), (10, 0x07, U)]              # digit 2: the Main DSP row to Yes
    cases = [
        ('opens_all_no', ALL, [], [], 1000),
        ('nothing_selected_no_hold', ALL, [], [(0, 0x06, D)], 10000),
        ('toggle_each_row', ALL, [], [(0, 0x07, D), (100, 0x08, D), (200, 0x09, D), (300, 0x12, D),
                                      (400, 0x13, D), (500, 0x14, D)], 1000),
        ('toggle_twice_back_to_no', ALL, [], [(0, 0x07, D), (100, 0x07, U), (200, 0x07, D)], 1000),
        ('toggle_by_knob_click', ALL, [], [(0, 0x0B, D), (100, 0x0C, D), (200, 0x0D, D)], 1000),
        ('toggle_by_other_digit_key', ALL, [], [(0, 0x0F, D), (100, 0x10, D), (200, 0x11, D)], 1000),
        ('not_pickable_rows_do_nothing', [1], [], [(0, 0x08, D), (100, 0x09, D), (200, 0x12, D), (300, 0x13, D),
                                                   (400, 0x14, D)], 1000),
        ('not_pickable_then_hold_nothing', [1], [], [(0, 0x14, D), (100, 0x14, U), (200, 0x06, D)], 10000),
        ('hold_m1_5s', ALL, [], OS + [(1000, 0x06, D)], 6000),
        ('hold_sound1_5s', ALL, [], OS + [(20, 0x0E, D)], 5020),
        ('hold_4999ms_waits', ALL, [], OS + [(1000, 0x06, D)], 5999),
        ('released_early', ALL, [], OS + [(1000, 0x06, D), (5999, 0x06, U)], 20000),
        ('released_then_again', ALL, [], OS + [(1000, 0x06, D), (4000, 0x06, U), (5000, 0x06, D)], 10000),
        ('released_then_again_short', ALL, [], OS + [(1000, 0x06, D), (4000, 0x06, U), (5000, 0x06, D)], 9999),
        ('released_after_5s', ALL, [], OS + [(1000, 0x06, D), (6500, 0x06, U)], 7000),
        ('knob1_click_no_hold', ALL, [], OS + [(100, 0x0A, D)], 20000),
        ('digit8_cancels', ALL, [], OS + [(2500, 0x15, D)], 3000),
        ('digit8_during_hold_ends_it', ALL, [], OS + [(100, 0x06, D), (4000, 0x15, D)], 10000),
        ('toggle_during_hold_ends_it', ALL, [], OS + [(100, 0x06, D), (2000, 0x08, D)], 10000),
        ('other_keys_no_effect', ALL, [], [(0, k, D) for k in (0x00, 0x18, 0x37, 0x40)], 59999),
        ('timeout', ALL, [], [], 60000),
        ('timeout_not_before', ALL, [], [], 59999),
        ('timeout_after_selection', ALL, [], OS, 60010),
        ('key_event_restarts_time', ALL, [], [(50000, 0x40, D), (50500, 0x40, U)], 110499),
        ('key_event_restarts_time_then_cancels', ALL, [], [(50000, 0x40, D), (50500, 0x40, U)], 110500),
        ('stale_key_event_restarts_time', ALL, [0x40], [(50000, 0x40, U)], 109999),
        ('hold_not_ended_by_time', ALL, [], OS + [(59000, 0x06, D)], 64000),
        ('hold_late_install', ALL, [], OS + [(59000, 0x06, D)], 90000),
        ('release_restarts_time', ALL, [], OS + [(1000, 0x06, D), (3000, 0x06, U)], 62999),
        ('release_restarts_time_then_cancels', ALL, [], OS + [(1000, 0x06, D), (3000, 0x06, U)], 63000),
        ('stale_key_ignored', ALL, [0x06], [], 20000),
        ('stale_digit_no_toggle', ALL, [0x07], [(1000, 0x07, U)], 2000),
        ('stale_digit_up_then_down', ALL, [0x07], [(1000, 0x07, U), (2000, 0x07, D)], 3000),
        ('stale_key1_up_then_hold', ALL, [0x06], OS + [(1000, 0x06, U), (2000, 0x06, D)], 7000),
        ('stale_digit8_ignored', ALL, [0x15], OS + [(1000, 0x15, U), (1500, 0x0E, D)], 6500),
        ('stale_digit8_still_down_blocks_hold', ALL, [0x15], OS + [(1000, 0x0E, D)], 6000),
        ('stale_digit8_up_then_down', ALL, [0x15], [(1000, 0x15, U), (1500, 0x15, D)], 2000),
        ('second_digit1_key_ends_hold', ALL, [], OS + [(100, 0x06, D), (1000, 0x0E, D), (2000, 0x0E, U)], 7000),
        ('knob_click_ends_hold', ALL, [], OS + [(100, 0x06, D), (1000, 0x0A, D), (1500, 0x0A, U)], 7000),
        ('step_key_ends_hold', ALL, [], OS + [(100, 0x06, D), (2000, 0x40, D)], 10000),
        ('step_key_down_blocks_hold', ALL, [], OS + [(100, 0x47, D), (1000, 0x06, D)], 10000),
        ('digit_key_down_blocks_hold', ALL, [], [(0, 0x07, D), (1000, 0x06, D)], 10000),
        ('other_key_released_then_hold', ALL, [], OS + [(100, 0x40, D), (500, 0x40, U), (1000, 0x06, D)], 6000),
        ('hold_again_after_break', ALL, [], OS + [(100, 0x06, D), (1000, 0x40, D), (1500, 0x40, U),
                                                  (2000, 0x06, U), (3000, 0x06, D)], 8000),
        ('stale_other_key_blocks_hold', ALL, [0x40], OS + [(1000, 0x06, D)], 7000),
        ('precomp_key_ignored', ALL, [], OS + [(100, 0x17, D), (1000, 0x06, D), (2000, 0x17, U), (3000, 0x17, D)],
         6000),
        ('selection_kept_after_break', ALL, [], OS + [(100, 0x14, D), (200, 0x14, U), (300, 0x06, D),
                                                      (1000, 0x06, U), (2000, 0x06, D)], 7000),
        ('after_choice_ignored', ALL, [], [(0, 0x15, D), (10, 0x07, D)], 10000),
    ]
    if dev == '1f':
        cases = [(n, p, [m.to_1f(k) for k in dn], [(t, m.to_1f(k), e) for t, k, e in ev], end)
                 for n, p, dn, ev, end in cases]
        cases += [
            ('control_hold_by_knob_0x36', ALL, [], [(0, 0x07, D), (10, 0x07, U), (100, 0x36, D)], 20000),
            ('control_59th_key', ALL, [], [(0, 0x07, D), (10, 0x07, U), (100, 0x3A, D), (200, 0x06, D)], 6000),
            ('control_0x17_ends_hold', ALL, [], [(0, 0x07, D), (10, 0x07, U), (100, 0x06, D), (1000, 0x17, D)],
             10000),
            ('control_shift_0x37_is_digit_2', ALL, [], [(0, 0x37, D)], 1000),
        ]
    out = []
    for name, pickable, down, events, end in cases:
        choice, at, sel = m.select_choice([(t, k, e == D) for t, k, e in events], pickable, down, end, dev)
        out.append({'case': name, 'pickable': pickable, 'down_at_open': ['0x%02X' % k for k in down],
                    'events': [[t, '0x%02X' % k, e] for t, k, e in events], 'end_ms': end,
                    'expect': choice, 'at_ms': at, 'selected': sel})
    return {'hold_ms': m.HOLD_MS, 'timeout_ms': m.SELECT_TIMEOUT_MS, 'row_steps': {str(d): s for d, s in
                                                                                 sorted(m.ROW_STEPS.items())},
            'cases': out}


def sig_prompt_cases():
    """S-15.197: for each signature state whether the prompt shows and whether the package is shown as TE's;
    the prompt's keys and time, as the selection's with no row to pick."""
    D, U = 'down', 'up'
    cases = [
        ('waits', [], [], 30000),
        ('hold_m1_5s', [], [(0, 0x06, D)], 5000),
        ('hold_sound1_5s', [], [(100, 0x0E, D)], 5100),
        ('hold_4999ms_waits', [], [(0, 0x06, D)], 4999),
        ('released_early', [], [(0, 0x06, D), (4000, 0x06, U)], 20000),
        ('released_then_again', [], [(0, 0x06, D), (4000, 0x06, U), (5000, 0x06, D)], 10000),
        ('knob1_click_no_hold', [], [(0, 0x0A, D)], 20000),
        ('digit8_cancels', [], [(2500, 0x15, D)], 3000),
        ('digit8_during_hold_ends_it', [], [(100, 0x06, D), (4000, 0x15, D)], 10000),
        ('other_key_during_hold_ends_it', [], [(100, 0x06, D), (2000, 0x40, D)], 10000),
        ('digit_rows_do_nothing', [], [(0, 0x07, D), (10, 0x07, U), (100, 0x14, D), (110, 0x14, U)], 1000),
        ('other_key_down_blocks_hold', [], [(0, 0x40, D), (100, 0x06, D)], 10000),
        ('precomp_ignored', [], [(0, 0x17, D), (100, 0x06, D)], 5100),
        ('stale_key1_ignored', [0x06], [], 20000),
        ('stale_key1_up_then_hold', [0x06], [(1000, 0x06, U), (2000, 0x06, D)], 7000),
        ('timeout', [], [], 60000),
        ('timeout_not_before', [], [], 59999),
        ('hold_not_ended_by_time', [], [(59000, 0x06, D)], 64000),
    ]
    out = []
    for name, down, events, end in cases:
        choice, at = m.sig_prompt_choice([(t, k, e == D) for t, k, e in events], down, end)
        out.append({'case': name, 'pickable': [], 'down_at_open': ['0x%02X' % k for k in down],
                    'events': [[t, '0x%02X' % k, e] for t, k, e in events], 'end_ms': end,
                    'expect': choice, 'at_ms': at, 'selected': []})
    return {'requirement': 'S-15.197', 'hold_ms': m.HOLD_MS, 'timeout_ms': m.SELECT_TIMEOUT_MS,
            'states': [{'state': s, 'prompt': m.sig_prompt_shown(s), 'sig_valid': m.sig_valid_shown(s)}
                       for s in m.SIG_STATES],
            'cases': out}


def keytest_1f():
    """S-15.064, S-15.065 on the OP-1F: keytest.json's sequences with S-15.016's keys, all 58 keys, and a
    59th key that is none."""
    with open(os.path.join(HERE, 'vectors', 'keytest.json'), encoding='utf-8') as f:
        xy = json.load(f)
    seqs = []
    for s in xy['sequences']:
        ev = [[m.to_1f(k), d] for k, d in s['events']]
        tested, end = m.keytest([(k, bool(d)) for k, d in ev], '1f')[-1]
        seqs.append({'name': s['name'], 'events': ev, 'tested': tested, 'end': end})
    for name, ev in (('control_old_shift_0x37', [[0x37, 1], [0x14, 1]]),
                     ('control_59th_key', [[0x3A, 1], [0x3A, 0]])):
        tested, end = m.keytest([(k, bool(d)) for k, d in ev], '1f')[-1]
        seqs.append({'name': name, 'events': ev, 'tested': tested, 'end': end})
    every = [(k, d) for k in range(58) for d in (True, False)]
    assert m.keytest(every, '1f')[-1][1] and not m.keytest(every[:-1], '1f')[-1][1]
    return {'device': 'OP-1F', 'colors': xy['colors'], 'sequences': seqs,
            'all_keys': {'events': 'every key 0..57 pressed and released alone, in order', 'end': True},
            'nkeys': 58}


def write_digits_1f():
    rows = sorted((k, d) for k, d in m.DEV['1f']['key_digit'].items())
    with open(os.path.join(HERE, 'vectors', 'digits_1f.tsv'), 'w', newline='') as f:
        f.write('# S-15.011 with S-15.016: the OP-1F\n')
        w = csv.writer(f, delimiter='\t', lineterminator='\n')
        w.writerow(['key', 'digit'])
        for k, d in rows:
            w.writerow(['0x%02x' % k, d])


def factory_reset_1f():
    """S-15.052 on the OP-1F: user/ and logs/ removed with everything in them, nothing else, no FAT erase; on
    a refused volume only the settings' clear."""
    tree = {'user/a.op1': b'u', 'user/sub/b': b'b', 'logs/boot.log': b'l', 'content/dlc/A/manifest.json': b'{}',
            'content/op1.json': b'c', 'os.ldr': b'o'}
    cases = []
    for name, refused in (('readable', False), ('refused', True)):
        after, ops = m.factory_reset(tree, '1f', refused)
        cases.append({'case': name, 'refused': refused, 'tree_before': {k: v.hex() for k, v in sorted(tree.items())},
                      'tree_after': {k: v.hex() for k, v in sorted(after.items())},
                      'operations': [list(o) for o in ops]})
    assert all(['erase_os_fat'] not in c['operations'] for c in cases)
    return {'device': 'OP-1F', 'requirement': 'S-15.052', 'removes': ['/yaffs2/' + d for d in m.RESET_REMOVES['1f']],
            'cases': cases}


def mtp_item_1f():
    """S-15.196: shift's events and digit 2's key down, with the USB set started."""
    S, K = 0x26, 0x07
    cases = [('held', [(S, True), (K, True)]),
             ('not_held', [(K, True)]),
             ('released_just_before', [(S, True), (S, False), (K, True)]),
             ('pressed_just_after', [(K, True), (S, True)]),
             ('held_then_released_after', [(S, True), (K, True), (S, False)]),
             ('by_another_digit_2_key', [(S, True), (0x2E, True)]),
             ('old_shift_0x37_is_digit_2', [(0x37, True)])]
    out = []
    for name, ev in cases:
        out.append({'case': name, 'events': [['0x%02X' % k, 'down' if d else 'up'] for k, d in ev],
                    'interface': m.mtp_item(ev, '1f')})
    return {'device': 'OP-1F', 'requirement': 'S-15.196', 'shift': '0x26', 'cases': out}


def main():
    with open(os.path.join(HERE, 'vectors', 'select_keys.json'), 'w') as f:
        json.dump(select_cases(), f, indent=1)
        f.write('\n')
    for name, fn in (('sig_prompt', sig_prompt_cases),
                     ('select_keys_1f', lambda: dict(select_cases('1f'), device='OP-1F')),
                     ('keytest_1f', keytest_1f), ('factory_reset_1f', factory_reset_1f),
                     ('mtp_item_1f', mtp_item_1f)):
        with open(os.path.join(HERE, 'vectors', name + '.json'), 'w') as f:
            json.dump(fn(), f, indent=1)
            f.write('\n')
    write_digits_1f()
    with open(os.path.join(HERE, 'vectors', 'handover_frame.json'), 'w') as f:
        v = m.handover_vector()
        json.dump(v, f, indent=1)
        f.write('\n')


if __name__ == '__main__':
    main()
