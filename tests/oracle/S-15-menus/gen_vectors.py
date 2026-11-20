# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates the model-derived vectors of S-15: vectors/handover_frame.json (S-15.132) and
vectors/select_keys.json (S-15.116). The other vectors are written by hand.
Run: python3 -I -B tests/oracle/S-15-menus/gen_vectors.py"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import s15_model as m  # noqa: E402


def select_cases():
    """S-15.116: (case, pickable steps, down_at_open, events as (ms, key, 'down' | 'up'), end_ms)."""
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
    out = []
    for name, pickable, down, events, end in cases:
        choice, at, sel = m.select_choice([(t, k, e == D) for t, k, e in events], pickable, down, end)
        out.append({'case': name, 'pickable': pickable, 'down_at_open': ['0x%02X' % k for k in down],
                    'events': [[t, '0x%02X' % k, e] for t, k, e in events], 'end_ms': end,
                    'expect': choice, 'at_ms': at, 'selected': sel})
    return {'hold_ms': m.HOLD_MS, 'timeout_ms': m.SELECT_TIMEOUT_MS, 'row_steps': {str(d): s for d, s in
                                                                                 sorted(m.ROW_STEPS.items())},
            'cases': out}


def main():
    with open(os.path.join(HERE, 'vectors', 'select_keys.json'), 'w') as f:
        json.dump(select_cases(), f, indent=1)
        f.write('\n')
    with open(os.path.join(HERE, 'vectors', 'handover_frame.json'), 'w') as f:
        v = m.handover_vector()
        json.dump(v, f, indent=1)
        f.write('\n')


if __name__ == '__main__':
    main()
