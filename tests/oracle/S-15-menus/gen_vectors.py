# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates the model-derived vectors of S-15: vectors/handover_frame.json (S-15.132) and
vectors/confirm_keys.json (S-15.116). The other vectors are written by hand.
Run: python3 -I -B tests/oracle/S-15-menus/gen_vectors.py"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import s15_model as m  # noqa: E402


def confirm_cases():
    """S-15.116: (case, readable, down_at_open, events as (ms, key, 'down' | 'up'), end_ms)."""
    D, U = 'down', 'up'
    cases = [
        ('hold_m1_5s', True, [], [(1000, 0x06, D)], 6000),
        ('hold_sound1_5s', True, [], [(0, 0x0E, D)], 5000),
        ('hold_4999ms_waits', True, [], [(1000, 0x06, D)], 5999),
        ('released_early', True, [], [(1000, 0x06, D), (5999, 0x06, U)], 20000),
        ('released_then_again', True, [], [(1000, 0x06, D), (4000, 0x06, U), (5000, 0x06, D)], 10000),
        ('released_then_again_short', True, [], [(1000, 0x06, D), (4000, 0x06, U), (5000, 0x06, D)], 9999),
        ('released_after_5s', True, [], [(1000, 0x06, D), (6500, 0x06, U)], 7000),
        ('knob1_click_no_effect', True, [], [(0, 0x0A, D)], 20000),
        ('knob_clicks_no_effect', True, [], [(0, 0x0A, D), (10, 0x0B, D), (20, 0x0C, D), (30, 0x0D, D)], 59999),
        ('digit8_skips', True, [], [(2500, 0x15, D)], 3000),
        ('digit8_during_hold', True, [], [(0, 0x06, D), (4000, 0x15, D)], 10000),
        ('other_keys_no_effect', True, [], [(0, k, D) for k in (0x00, 0x07, 0x09, 0x12, 0x14, 0x18, 0x37)], 59999),
        ('timeout', True, [], [], 60000),
        ('timeout_not_before', True, [], [], 59999),
        ('hold_reaches_5s_at_60s', True, [], [(55000, 0x06, D)], 60000),
        ('hold_reaches_5s_after_60s', True, [], [(55001, 0x06, D)], 61000),
        ('hold_late_after_timeout', True, [], [(59000, 0x06, D)], 65000),
        ('stale_key_ignored', True, [0x06], [], 20000),
        ('stale_key_up_then_down', True, [0x06], [(1000, 0x06, U), (2000, 0x06, D)], 7000),
        ('stale_digit8_ignored', True, [0x15], [(1000, 0x15, U), (1500, 0x0E, D)], 6500),
        ('stale_digit8_still_down_blocks_hold', True, [0x15], [(1000, 0x0E, D)], 6000),
        ('stale_digit8_up_then_down', True, [0x15], [(1000, 0x15, U), (1500, 0x15, D)], 2000),
        ('second_digit1_key_ends_hold', True, [], [(0, 0x06, D), (1000, 0x0E, D), (2000, 0x0E, U)], 7000),
        ('knob_click_ends_hold', True, [], [(0, 0x06, D), (1000, 0x0A, D), (1500, 0x0A, U)], 7000),
        ('step_key_ends_hold', True, [], [(0, 0x06, D), (2000, 0x40, D)], 10000),
        ('step_key_down_blocks_hold', True, [], [(0, 0x47, D), (1000, 0x06, D)], 10000),
        ('other_key_released_then_hold', True, [], [(0, 0x07, D), (500, 0x07, U), (1000, 0x06, D)], 6000),
        ('hold_again_after_break', True, [], [(0, 0x06, D), (1000, 0x07, D), (1500, 0x07, U), (2000, 0x06, U),
                                              (3000, 0x06, D)], 8000),
        ('stale_other_key_blocks_hold', True, [0x07], [(1000, 0x06, D)], 7000),
        ('precomp_key_ignored', True, [], [(0, 0x17, D), (1000, 0x06, D), (2000, 0x17, U), (3000, 0x17, D)], 6000),
        ('unreadable', False, [], [(0, 0x06, D)], 6000),
        ('after_choice_ignored', True, [], [(0, 0x15, D), (10, 0x06, D)], 10000),
    ]
    out = []
    for name, readable, down, events, end in cases:
        choice, at = m.confirm_choice([(t, k, e == D) for t, k, e in events], down, readable, end)
        out.append({'case': name, 'readable': readable, 'down_at_open': ['0x%02X' % k for k in down],
                    'events': [[t, '0x%02X' % k, e] for t, k, e in events], 'end_ms': end,
                    'expect': choice, 'at_ms': at})
    return {
            'hold_ms': m.CONFIRM_HOLD_MS, 'timeout_ms': m.CONFIRM_TIMEOUT_MS, 'cases': out}


def main():
    with open(os.path.join(HERE, 'vectors', 'confirm_keys.json'), 'w') as f:
        json.dump(confirm_cases(), f, indent=1)
        f.write('\n')
    with open(os.path.join(HERE, 'vectors', 'handover_frame.json'), 'w') as f:
        v = m.handover_vector()
        json.dump(v, f, indent=1)
        f.write('\n')


if __name__ == '__main__':
    main()
