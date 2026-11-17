#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-13b vectors, from the tables check.py holds. Synthetic only.

    python3 -I tests/oracle/S-13b-buses-and-pins/gen_vectors.py          writes vectors/*.json
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check as C  # noqa: E402


def w(reg, val):
    return {'op': 'w', 'reg': reg, 'val': val}


def r(reg, val=0):
    return {'op': 'r', 'reg': reg, 'val': val}


def gpio(name, output):
    p, b = C._pin(name)
    return [w('PORT%s_DIR_SET' % p, b), w('PORT%s_INEN_CLR' % p, b)] if output else \
        [w('PORT%s_DIR_CLR' % p, b), w('PORT%s_INEN_SET' % p, b)]


OWNER_SETUP = {  # pins set after the first SPI/UART/MSI access by their owners (S-13b.006 table)
    'master': [w('PORTA_DIR_CLR', 0x40), w('PORTA_INEN_SET', 0x40),                        # PA6, slave boot link
               w('PORTC_FER_CLR', 0x800), w('PORTC_DATA_SET', 0x800), w('PORTC_DIR_SET', 0x800),
               w('PORTC_INEN_CLR', 0x800),                                                 # PC11, bridge select
               w('PORTC_DIR_CLR', 0x20), w('PORTC_INEN_SET', 0x20)],                       # PC5, attention
    'slave': [w('PORTA_DATA_SET', 0x10), w('PORTA_DIR_SET', 0x10), w('PORTA_INEN_CLR', 0x10),   # PA4
              w('PORTB_DATA_SET', 0x4), w('PORTB_DIR_SET', 0x4), w('PORTB_INEN_CLR', 0x4),      # PB2
              w('PORTB_DIR_CLR', 0x8000), w('PORTB_INEN_SET', 0x8000)],                         # PB15
}


def port_trace(role, strap=0x004):
    t = [w('PORTA_DIR_CLR', 0x105), w('PORTA_INEN_CLR', 0x105), w('PORTA_INEN_SET', 0x105),
         r('PORTA_DATA', strap), w('PORTA_INEN_CLR', 0x105)]
    t += [w('PORTB_DIR_CLR', 0x400), w('PORTB_INEN', 0x400)]
    if role == 'master':
        t += [w('PORTC_DIR_CLR', 0x10), w('PORTC_INEN', 0x10)]
    t += [w('PORT%s_MUX' % p, C.PINMUX[role][p][0]) for p in C.PORTS]
    t += [w('PORT%s_FER' % p, C.PINMUX[role][p][1]) for p in C.PORTS]
    low, high, _, ins = C.GPIO[role]
    owned = {'P' + e['reg'][4] + str(e['val'].bit_length() - 1) for e in OWNER_SETUP[role]}
    for name in low + high:
        if name not in owned:
            t += gpio(name, True)
    for name in ins:
        if name not in owned:
            t += gpio(name, False)
    for name in high:
        if name not in owned:
            p, b = C._pin(name)
            t.append(w('PORT%s_DATA_SET' % p, b))
    return t + [{'op': 'mark'}] + OWNER_SETUP[role]


def pin_traces():
    rows = []
    for role in ('master', 'slave'):
        for s in (0x000, 0x001, 0x100, 0x101):
            rows.append({'name': '%s_strap_%03x' % (role, s), 'role': role, 'trace': port_trace(role, s | 0x4 * (role == 'master')),
                         'expect': []})
    m = port_trace('master')
    i_mux, i_fer = 9, 12
    fer_first = m[:i_mux] + [m[i_fer]] + m[i_mux:i_fer] + m[i_fer + 1:]
    rows.append({'name': 'fer_before_mux', 'role': 'master', 'trace': fer_first, 'expect': ['mux_before_fer']})
    late = m[5:i_mux + 3] + m[:5] + m[i_mux + 3:]
    rows.append({'name': 'strap_after_mux', 'role': 'master', 'trace': late, 'expect': ['strap_before_mux']})
    nopb10 = [e for e in m if e.get('reg') not in ('PORTB_DIR_CLR', 'PORTB_INEN')]
    rows.append({'name': 'no_pb10_input', 'role': 'master', 'trace': nopb10, 'expect': ['mux_before_fer']})
    wrong = [dict(e, val=0x30002001) if e.get('reg') == 'PORTA_MUX' else e for e in m]
    rows.append({'name': 'wrong_mux', 'role': 'master', 'trace': wrong, 'expect': ['mux_before_fer', 'pinmux_end_state']})
    # PA6's input enable written to the whole register, which clears PA7's (S-13b.006)
    whole = [w('PORTA_INEN', 0x40) if e.get('reg') == 'PORTA_INEN_SET' and e['val'] == 0x40 else e for e in m]
    rows.append({'name': 'pa7_dropped_by_whole_write', 'role': 'master', 'trace': whole, 'expect': ['gpio_directions']})
    rows.append({'name': 'master_pa3_high', 'role': 'master', 'trace': m + [w('PORTA_DATA_SET', 0x8)],
                 'expect': ['gpio_directions']})
    rows.append({'name': 'master_pc11_low', 'role': 'master', 'trace': m + [w('PORTC_DATA_CLR', 0x800)],
                 'expect': ['gpio_directions']})
    # the bridge's FER clear of PC11 after the mark leaves S-13b.003's end state intact
    nomark = [e for e in m if e['op'] != 'mark']
    rows.append({'name': 'master_no_mark', 'role': 'master', 'trace': nomark, 'expect': ['pinmux_end_state']})
    s = port_trace('slave')
    nohigh = [e for e in s if e.get('reg') != 'PORTC_DATA_SET']
    rows.append({'name': 'slave_pc2_low', 'role': 'slave', 'trace': nohigh, 'expect': ['gpio_directions']})
    return rows


def strap_timing():
    """S-13b.002, .008: t_ns on the strap writes and the sample."""
    def tr(release, enable, sample):
        return [dict(w('PORTA_DIR_CLR', 0x105), t_ns=release), dict(w('PORTA_INEN_CLR', 0x105), t_ns=release + 10),
                dict(w('PORTA_INEN_SET', 0x105), t_ns=enable), dict(r('PORTA_DATA', 0x4), t_ns=sample),
                dict(w('PORTA_INEN_CLR', 0x105), t_ns=sample + 10)]
    rows = [('settled', tr(0, 20_000, 21_000), []),
            ('enable_early_settled', tr(0, 50, 12_000), []),
            ('sample_too_soon_after_release', tr(0, 4_000, 5_000), ['strap_settle']),
            ('sample_right_after_enable', tr(0, 25_000, 25_010), ['strap_inen_floor']),
            ('no_settle_at_all', tr(0, 20, 30), ['strap_inen_floor', 'strap_settle'])]
    return {'settle_ns': C.STRAP_SETTLE_NS, 'inen_ns': C.STRAP_INEN_NS,
            'rows': [{'name': n, 'trace': t, 'expect': x} for n, t, x in rows]}


def bridge_pins():
    """S-13b.043 on the slave: PA4 between GPIO (high) and SPI2."""
    setup = [w('PORTA_DATA_SET', 0x10), w('PORTA_DIR_SET', 0x10), w('PORTA_INEN_CLR', 0x10)]
    rx = [r('PORTB_DATA', 0x8000), w('SPI2_CTL', 0x2511), w('PORTA_FER_SET', 0x10)]
    done = [w('PORTA_FER_CLR', 0x10)]
    rows = [('rx_then_tx', setup + rx + done + rx + done, []),
            ('fer_before_enable', setup + [r('PORTB_DATA', 0x8000), w('PORTA_FER_SET', 0x10), w('SPI2_CTL', 0x2511)],
             ['pa4_handover']),
            ('fer_without_master_ready', setup + [r('PORTB_DATA', 0), w('SPI2_CTL', 0x2511), w('PORTA_FER_SET', 0x10)],
             ['pa4_handover']),
            ('pa4_low_as_gpio', setup + rx + done + [w('PORTA_DATA_CLR', 0x10)], ['pa4_handover'])]
    # after an SPI2 error or a given-up transfer, PA4 goes back to SPI2 only after PB15 has read high again
    hand = [w('SPI2_CTL', 0x206511), w('PORTA_FER_SET', 0x10)]
    for why in ('spi2_error', 'give_up'):
        failed = setup + rx + [{'op': 'ev', 'ev': why}] + done
        rows += [('rehand_after_%s' % why, failed + [r('PORTB_DATA', 0), r('PORTB_DATA', 0x8000)] + hand, []),
                 ('rehand_before_pb15_high_%s' % why, failed + hand, ['pa4_rehand']),
                 ('rehand_while_pb15_low_%s' % why, failed + [r('PORTB_DATA', 0)] + hand,
                  ['pa4_handover', 'pa4_rehand'])]
    # within a session the slave may raise its line and hand it back while PB15 is low (S-09.011)
    rows.append(('hold_within_session', setup + rx + done + hand, []))
    # after a release reply of 0 the server stops and PA4 is held low as a GPIO until the OS starts (S-05.020)
    stop, start, low = {'op': 'ev', 'ev': 'server_stop'}, {'op': 'ev', 'ev': 'os_start'}, w('PORTA_DATA_CLR', 0x10)
    served = setup + rx + done
    rows += [('server_stop_low', served + [stop, low, start], []),
             ('server_stop_left_high', served + [stop, start], ['pa4_stop']),
             ('server_stop_released_early', served + [stop, low, w('PORTA_DATA_SET', 0x10), start], ['pa4_stop']),
             ('server_stop_handed_to_spi2', served + [stop, low, r('PORTB_DATA', 0x8000)] + hand + [start],
              ['pa4_stop'])]
    return {'rows': [{'name': n, 'trace': t, 'expect': x} for n, t, x in rows]}


def slave_tx():
    """S-13b.045: the end and the failure of the slave's transmit in a reply session (four words)."""
    def st(**flags):
        return dict({'ev': 'status'}, **flags)

    def word(k):
        return [{'ev': 'start', 'word': k}, {'ev': 'end', 'word': k}]

    hand, take, stop = {'ev': 'hand'}, {'ev': 'take'}, {'ev': 'stop'}
    hold = st(tfifo_empty=True, spif=True)       # the feed fell behind: the master is held (S-13b.044)
    finish = st(rx_words=4)                      # all four words received: the end
    words = word(0) + word(1) + word(2) + word(3)
    rows = [
        ('reply', [hand] + words + [finish, take, stop], []),
        # no TF at all: the count ends it
        ('reply_no_tf', [hand, st(spif=True)] + word(0) + word(1) + word(2) + word(3)
         + [st(spif=True, twc_zero=True, rx_words=4), take, stop], []),
        ('reply_with_hold', [hand] + word(0) + word(1) + [hold] + word(2) + [hold] + word(3)
         + [finish, take, stop], []),
        # S-13b.045: PB15 high ends the session whatever the slave has sent
        ('reply_master_gave_up', [hand] + word(0) + [{'ev': 'pb15_high'}, take, stop], []),
        ('reply_underrun', [hand] + word(0) + [st(tur=True)] + [take, stop], []),
        ('reply_collision', [hand] + word(0) + word(1) + [st(tc=True)] + [take, stop], []),
        ('reply_underrun_with_finish', [hand] + word(0) + [st(tur=True, tf=True), take, stop], []),
        ('reply_collision_last_word', [hand] + word(0) + word(1) + word(2) + [{'ev': 'start', 'word': 3},
                                       st(tc=True), take, stop, {'ev': 'end', 'word': 3}], []),
        # wrong readings
        ('rx_count_before_last_word', [hand] + word(0) + word(1) + word(2) + [st(rx_words=3), take, stop],
         ['tx_end_early']),
        # a stale word left in the receive FIFO brings the count up one word early
        ('rx_count_stale_word', [hand] + word(0) + word(1) + word(2) + [st(rx_words=4), take, stop],
         ['tx_end_early']),
        ('end_on_fifo_empty', [hand] + word(0) + word(1) + [st(tfifo_empty=True), take, stop], ['tx_end_early']),
        ('end_on_fifo_empty_spif', [hand] + word(0) + word(1) + [hold, take, stop], ['tx_end_early']),
        # SPIF reads 1 from the hand-over, before any clock (S-13b.045)
        ('end_on_spif_at_hand_over', [hand, st(spif=True), take, stop], ['tx_end_early']),
        # SPI_TWC counts a word as it leaves the transmit FIFO (S-13b.045)
        ('end_on_twc_zero', [hand] + word(0) + word(1) + word(2) + [{'ev': 'start', 'word': 3},
                             st(twc_zero=True), take, stop, {'ev': 'end', 'word': 3}], ['tx_end_early']),
        ('end_before_last_word', [hand] + word(0) + word(1) + word(2) + [hold, take, stop], ['tx_end_early']),
        ('stop_within_last_word', [hand] + word(0) + word(1) + word(2) + [{'ev': 'start', 'word': 3}, stop,
                                   {'ev': 'end', 'word': 3}, finish, take], ['tx_end_early']),
        ('never_ends', [hand] + words + [hold], ['tx_end_missing']),
        # a receive overrun lost a word from the count: the end is not seen
        ('rx_count_short', [hand] + words + [st(rx_words=3)], ['tx_end_missing']),
        # ... unless taken from the transmit side once the overrun is seen: every word gone from the
        # transmit FIFO and none received for the quiet time
        ('overrun_end', [hand] + word(0) + [st(ror=True, rx_words=0)] + word(1) + word(2)
         + [{'ev': 'start', 'word': 3}, {'ev': 'end', 'word': 3, 't_us': 100},
            st(ror=True, twc_zero=True, rx_words=3, quiet_us=C.REPLY_QUIET_US),
            {'ev': 'take', 't_us': 100 + C.REPLY_QUIET_US + 10}, stop], []),
        ('overrun_end_too_soon', [hand] + word(0) + [st(ror=True, rx_words=0)] + word(1) + word(2) + word(3)
         + [st(ror=True, twc_zero=True, rx_words=3, quiet_us=C.REPLY_QUIET_US // 10), take, stop],
         ['tx_end_no_count']),
        ('quiet_without_overrun', [hand] + word(0) + word(1) + word(2) + word(3)
         + [st(twc_zero=True, rx_words=3, quiet_us=C.REPLY_QUIET_US), take, stop], ['tx_end_no_count']),
        # a master paused before the last word's clocks longer than the quiet time
        ('overrun_end_before_last_word', [hand] + word(0) + [st(ror=True, rx_words=0)] + word(1) + word(2)
         + [st(ror=True, twc_zero=True, rx_words=2, quiet_us=C.REPLY_QUIET_US), take, stop]
         + word(3), ['tx_end_early']),
        ('take_without_stop', [hand] + words + [finish, take], ['tx_no_stop']),
        # TF taken as the end
        ('end_on_tf', [hand] + words + [st(tf=True), take, stop], ['tx_end_no_count']),
        ('end_on_tf_early', [hand] + word(0) + word(1) + word(2) + [st(tf=True), take, stop], ['tx_end_early']),
        # a send that waits for TF: the take-back at the bound, about 500 ms after the reply
        ('waits_for_tf_to_bound', [hand] + word(0) + word(1) + word(2) + [{'ev': 'start', 'word': 3},
                                   {'ev': 'end', 'word': 3, 't_us': 100}, finish,
                                   {'ev': 'take', 't_us': 500100}, stop], ['tx_end_late']),
        ('underrun_ignored', [hand] + word(0) + [st(tur=True)] + word(1) + word(2) + word(3) + [finish, take],
         ['tx_error_ignored']),
        # S-13b.041's quad I/O option: TF, read after the last word's end, is the end; no received-word count
        ('tf_after_dma', [hand] + word(0) + word(1) + [{'ev': 'dma_done'}] + word(2) + word(3)
         + [st(tf=True), take, stop], [], 'quad'),
        ('quad_tf_early', [hand] + word(0) + word(1) + word(2) + [st(tf=True), take, stop], ['tx_end_early'], 'quad'),
        ('quad_rx_channel_on', [hand] + words + [finish, take, stop], ['tx_end_no_count', 'tx_quad_rx_channel'],
         'quad'),
        ('collision_ignored', [hand] + word(0) + [st(tc=True)] + word(1) + word(2) + word(3) + [hold],
         ['tx_error_ignored']),
        # reads no finish after the error and takes PA4 back only after the last word
        ('underrun_ignored_no_finish', [hand] + word(0) + [st(tur=True)] + word(1) + word(2) + word(3) + [take, stop],
         ['tx_error_ignored']),
        ('last_word_error_waited', [hand] + word(0) + word(1) + word(2) + [{'ev': 'start', 'word': 3},
                                    st(tc=True), {'ev': 'end', 'word': 3}, take, stop], ['tx_error_ignored']),
    ]
    out = []
    for n, t, x, *io in rows:
        row = {'name': n, 'trace': t, 'expect': x}
        if io:
            row['io'] = io[0]
        out.append(row)
    return {'words': 4, 'rows': out}


def reply_setup():
    """S-13b.044: SPI2's whole reply set-up at the hand-over of PA4."""
    def s(link, ctl, txctl, rxctl, words=2, twc=None, queued=2, **kw):
        return dict({'link': link, 'ctl': ctl, 'txctl': txctl, 'rxctl': rxctl, 'words': words,
                     'twc': words if twc is None else twc, 'queued': queued}, **kw)

    good_tx, good_rx = C.REPLY_TXCTL, 0x1
    last = C.REPLY_ORDER
    dma = ['stat', 'dma', 'twc', 'txctl', 'rxctl', 'en']
    first = ['en', 'stat', 'twc', 'txctl', 'rxctl']
    rows = [
        ('reply_pre', s('pre', 0x6511, good_tx, good_rx), []),
        ('reply_post', s('post', 0x6511, good_tx, good_rx), []),
        ('reply_long', s('post', 0x6511, good_tx, good_rx, words=0x102), []),
        # this CTL with neither the transmit set-up nor the queued words
        ('unit_line_stuck', s('pre', 0x6511, 0x9, 0x1, queued=0), ['reply_no_pad', 'reply_txctl']),
        ('ctl_only', s('post', 0x6511, 0x0, 0x0, twc=0, queued=0),
         ['reply_no_pad', 'reply_rx_off', 'reply_twc', 'reply_txctl']),
        # no padding words queued
        ('no_pad', s('post', 0x6511, good_tx, good_rx, queued=0), ['reply_no_pad']),
        ('pad_long', s('post', 0x6511, good_tx, good_rx, queued=3), ['reply_no_pad']),
        ('pad_counted', s('post', 0x6511, good_tx, good_rx, twc=4), ['reply_twc']),
        ('tti_clear', s('post', 0x6511, good_tx & ~4, good_rx), ['reply_txctl']),
        # REN on: no received-word count to end the reply by
        ('rx_off', s('pre', 0x6511, good_tx, 0x10014), ['reply_rx_off']),
        # quad I/O after the link with REN on
        ('quad_post', s('post', 0x206511, good_tx, 0x10014), ['reply_ctl', 'reply_rx_off']),
        # a second set-up with flow control on the receive channel, in its two forms
        ('withdrawn_fallback', s('pre', 0x2511, 0x9, 0x1, queued=0), ['reply_ctl', 'reply_no_pad', 'reply_txctl']),
        ('withdrawn_fallback_tti', s('post', 0x202511, 0xD, 0x0, queued=0),
         ['reply_ctl', 'reply_no_pad', 'reply_rx_off', 'reply_txctl']),
        # the write order, EN last: required in quad I/O (S-13b.041's option), allowed in single I/O
        ('enable_last', s('quad', 0x206511, good_tx, 0x0, order=list(last)), []),
        ('dma_tf_order', s('quad', 0x206511, good_tx, 0x0, order=dma + ['dma_done', 'tf_unmask']), []),
        ('single_enable_last', s('post', 0x6511, good_tx, good_rx, order=list(last)), []),
        # single I/O, SPI2 enabled first, then SPI_STAT, SPI_TWC and SPI2_TXCTL
        ('single_enable_first', s('post', 0x6511, good_tx, good_rx, order=first), []),
        ('quad_enable_first', s('quad', 0x206511, good_tx, 0x0, order=first), ['reply_enable_last']),
        ('quad_twc_before_dma', s('quad', 0x206511, good_tx, 0x0,
                                  order=['stat', 'twc', 'dma', 'txctl', 'rxctl', 'en']), ['reply_order']),
        ('quad_tf_before_dma_done', s('quad', 0x206511, good_tx, 0x0, order=dma + ['tf_unmask', 'dma_done']),
         ['reply_tf_unmask']),
        ('quad_rx_on', s('quad', 0x206511, good_tx, good_rx, order=list(last)), ['reply_rx_quad']),
        # S-13b.084, the quad reply test: its TF candidates' set-up is this order's, the core feed's
        # words written once EN is set; its REN variant departs from S-13b.044 knowingly
        ('quad_test_tf_dma', s('quad', 0x206511, good_tx, 0x0, words=0x4000,
                               order=dma + ['dma_done', 'tf_unmask']), []),
        ('quad_test_tf_core', s('quad', 0x206511, good_tx, 0x0, words=3,
                                order=['stat', 'twc', 'txctl', 'rxctl', 'en', 'tf_unmask']), []),
        ('quad_test_ren', s('quad', 0x206511, good_tx, good_rx, order=dma + ['dma_done', 'tf_unmask']),
         ['reply_rx_quad']),
    ]
    return {'rows': [dict(name=n, setup=x, expect=e) for n, x, e in rows]}


def bridge_sessions():
    """S-13b.040: the master's reset of SPI0 between sessions."""
    sel, desel = {'ev': 'select', 'on': True}, {'ev': 'select', 'on': False}
    off, on = {'ev': 'en', 'on': False}, {'ev': 'en', 'on': True}
    tx, rx = {'ev': 'word', 'dir': 'tx'}, {'ev': 'word', 'dir': 'rx'}
    quad, single = dict(off, miom=2), dict(off, miom=0)

    def su(reg):
        return {'ev': 'setup', 'reg': reg}

    rows = [
        # good: when the reset falls between the sessions is free
        ('reset_after_last_word', [on, sel, tx, tx, off, desel, on, sel, rx, rx, off, desel, on, sel, tx, tx, off,
                                   desel], []),
        ('reset_after_select_rises', [sel, tx, tx, desel, off, on, sel, rx, rx, desel, off, on, sel, tx, tx, desel,
                                      off, on], []),
        ('reset_before_next_select', [sel, tx, desel, off, on, sel, rx, desel, off, on, sel, tx, desel], []),
        ('long_session', [sel, tx, tx, tx, desel, off, on, sel, rx, rx, rx, rx, desel, off, on], []),
        # wrong readings
        ('no_reset', [sel, tx, tx, desel, sel, rx, rx, desel, sel, tx, tx, desel], ['spi0_not_reset']),
        # no reset after a receive
        ('no_reset_after_receive', [sel, tx, desel, off, on, sel, rx, desel, sel, tx, desel], ['spi0_not_reset']),
        ('reset_within_session', [sel, rx, off, on, rx, desel, off, on, sel, tx, desel], ['spi0_reset_in_session']),
        ('word_while_disabled', [sel, tx, desel, off, sel, tx, desel], ['spi0_not_reset', 'spi0_word_disabled']),
        # the order of a session's set-up: EN last in every session (S-13b.083), MIOM only with EN clear
        ('enable_last', [on, sel, tx, tx, off, desel, su('SPI_RWC'), on, sel, rx, rx, off, desel,
                         su('SPI_TWC'), su('SPI_TXCTL'), on, sel, tx, desel], []),
        # the reset just after PC11 rises, then each session's set-up with EN clear and EN last
        ('reset_then_enable_last', [sel, tx, desel, off, on, off, su('SPI_RWC'), on, sel, rx, desel, off, on, off,
                                    su('SPI_TWC'), on, sel, tx, desel], []),
        # that reset, then the next session's set-up with EN set (S-13b.083)
        ('setup_after_reset_single', [sel, tx, desel, off, on, su('SPI_RWC'), sel, rx, desel, off, on, su('SPI_TWC'),
                                      sel, tx, desel], ['enable_last']),
        # S-13b.041's quad I/O option: MIOM written with EN clear, every session's set-up before EN
        ('enable_last_quad', [off, quad, su('SPI_TWC'), su('SPI_TXCTL'), on, sel, tx, tx, desel, off, su('SPI_RWC'),
                              su('SPI_RXCTL'), on, sel, rx, rx, desel, off, su('SPI_TWC'), on, sel, tx, desel], []),
        # a reply taken in parts: a new word count between two parts of one session is not covered
        ('quad_in_parts', [off, quad, su('SPI_RWC'), on, sel, rx, su('SPI_RWC'), rx, rx, desel, off, su('SPI_TWC'),
                           on, sel, tx, desel], []),
        # requests in quad I/O, replies in single I/O, MIOM switched at each reset with EN clear
        ('quad_request_single_reply', [off, quad, su('SPI_TWC'), on, sel, tx, tx, tx, desel, off, single,
                                       su('SPI_RWC'), on, sel, rx, rx, desel, off, quad, su('SPI_TWC'), on, sel,
                                       tx, desel], []),
        ('quad_setup_after_en', [off, quad, su('SPI_TWC'), on, sel, tx, desel, off, on, su('SPI_RWC'), sel, rx,
                                 desel], ['enable_last']),
        ('miom_while_enabled', [sel, tx, desel, off, on, dict(on, miom=2), sel, tx, desel], ['miom_while_enabled']),
        ('miom_with_en', [sel, tx, desel, off, dict(on, miom=2), sel, tx, desel], ['miom_while_enabled']),
        # the reset form SPI_CTL written 0 and then its value with EN, in quad I/O
        ('quad_reset_written_zero', [off, quad, on, sel, tx, desel, dict(off, miom=0), dict(on, miom=2), sel, rx,
                                     desel], ['miom_while_enabled']),
    ]
    return {'rows': [{'name': n, 'trace': t, 'expect': x} for n, t, x in rows]}


def bridge_dma():
    """S-13b.083: the bridge's sessions by DMA, each kind's register trace."""
    def ev(name, **kw):
        return dict({'op': 'ev', 'ev': name}, **kw)

    sel, desel = ev('select', on=True), ev('select', on=False)
    TF, RF, PA4 = C.SPI_TF, C.SPI_RF, C.PA4_BIT
    tx, rx = C.DMA_CFG_SEND, C.DMA_CFG_RECEIVE

    # the master's send: a request of 19 words (12 + 64 bytes), plain
    def send(words=19, cfg=tx, txctl=C.MASTER_TXCTL, twc=None, pre=(), desc=None):
        return list(pre) + [
            sel, w('SPI0_STAT', 0xFFFFFFFF), w('DMA4_STAT', 1),
            ev('desc', ch=4, bytes=desc or [12, 4 * words - 12]), w('DMA4_DSCPTR_CUR', 0x11800100),
            w('DMA4_CFG', cfg), w('SPI0_TWC', words if twc is None else twc), w('SPI0_TXCTL', txctl),
            w('SPI0_CTL', 0x2593)]

    done4 = [r('DMA4_STAT', 1), w('DMA4_STAT', 1), w('SPI0_IMSK_SET', TF)]
    end4 = [r('SPI0_ILAT', TF), w('SPI0_IMSK_CLR', TF), w('SPI0_STAT', TF), w('SPI0_TXCTL', 0x1001C),
            w('SPI0_CTL', 0x2592), desel]
    rung = [w('SPI0_CTL', 0), w('SPI0_CLK', 1), w('SPI0_CTL', 0x242592)]
    s = send()
    core_send = [sel, w('SPI0_STAT', 0xFFFFFFFF), w('SPI0_TWC', 19), w('SPI0_TXCTL', 0x1D), w('SPI0_CTL', 0x2593)] \
        + [w('SPI0_TFIFO', 0)] * 19 + [w('SPI0_TXCTL', 0x1C), w('SPI0_CTL', 0x2592), desel]
    sends = [
        ('send', s + done4 + end4, []),
        ('send_quad_rung', [x if x != w('SPI0_CTL', 0x2593) else w('SPI0_CTL', 0x242593)
                            for x in send(pre=rung)] + done4 + end4[:4] + [w('SPI0_CTL', 0x242592), desel], []),
        ('send_one_buffer', send(desc=[76]) + done4 + end4, []),
        ('send_word_at_a_time', core_send, ['dma_core', 'dma_desc', 'dma_order', 'send_end', 'send_txctl']),
        # one work unit in stop mode with SYNC set, this TXCTL, SPI0 enabled first, the end read from the
        # DMA's and the word count's state
        ('send_first_unit', [sel, w('SPI0_CTL', 0x2593), w('SPI0_STAT', 0xFFFFFFFF), w('DMA4_STAT', 1),
                             ev('desc', ch=4, bytes=[76]), w('DMA4_CFG', 0x100225), w('SPI0_TWC', 19),
                             w('SPI0_TXCTL', 0x1D), r('DMA4_STAT', 1), w('SPI0_TXCTL', 0x1C),
                             w('SPI0_CTL', 0x2592), desel],
         ['dma_cfg', 'dma_enable_last', 'dma_order', 'send_end']),
        ('send_sync_set', send(cfg=tx | 4) + done4 + end4, ['dma_cfg']),
        ('send_register_mode', send(cfg=0x21) + done4 + end4, ['dma_cfg']),
        ('send_no_watermark', send(txctl=0x1D) + done4 + end4, ['send_txctl']),
        ('send_twc_short', send(twc=18) + done4 + end4, ['send_twc']),
        ('send_desc_short', send(desc=[12, 60]) + done4 + end4, ['dma_desc']),
        ('send_en_first', [sel, w('SPI0_CTL', 0x2593)] + s[1:-1] + done4 + end4, ['dma_enable_last', 'dma_order']),
        ('send_twc_before_dma', s[:2] + [s[7]] + s[2:7] + s[8:] + done4 + end4, ['dma_order']),
        ('send_no_stat_clear', [s[0]] + s[2:] + done4 + end4, ['dma_order']),
        ('send_tf_unmasked_at_start', s + [w('SPI0_IMSK_SET', TF)] + done4[:2] + end4, ['tf_unmask']),
        ('send_end_on_dma_done', s + done4[:2] + end4[3:], ['send_end']),
        ('send_end_on_twc_zero', s + [r('SPI0_TWC', 0)] + end4[3:], ['send_end']),
        ('send_en_before_ten', s + done4 + end4[:3] + [end4[4], end4[3], desel], ['send_end']),
        ('send_select_first', s + done4 + end4[:3] + [desel] + end4[3:5], ['send_end']),
        ('send_never_ends', s + done4, ['send_end']),
    ]

    # the master's receive: a poll's reply in two parts (4 bytes, then 520), and an immediate reply in one
    def part(n, first, cfg=rx, rxctl=C.MASTER_RXCTL, rwc=None):
        return [ev('desc', ch=5, bytes=[n]), w('DMA5_DSCPTR_CUR', 0x11800100), w('DMA5_CFG', cfg),
                w('SPI0_RWC', n // 4 if rwc is None else rwc), w('SPI0_RXCTL', rxctl)] + \
            ([w('SPI0_CTL', 0x2593)] if first else [])

    done5 = [r('DMA5_STAT', 1), w('DMA5_STAT', 1), w('SPI0_RXCTL', 0x1001C)]
    end5 = [w('SPI0_CTL', 0x2592), desel]
    one = [sel] + part(8, True) + done5 + end5
    two = [sel] + part(4, True) + done5 + part(520, False) + done5 + end5
    core_recv = [sel, w('SPI0_RWC', 2), w('SPI0_RXCTL', 0xD), w('SPI0_CTL', 0x2593), r('SPI0_RFIFO', 0),
                 r('SPI0_RFIFO', 0), w('SPI0_RXCTL', 0xC), w('SPI0_CTL', 0x2592), desel]
    receives = [
        ('receive', (one, [8]), []),
        ('receive_in_parts', (two, [4, 520]), []),
        ('receive_parts_en_written_again', ([sel] + part(4, True) + done5 + part(520, False)
                                            + [w('SPI0_CTL', 0x2593)] + done5 + end5, [4, 520]), []),
        ('receive_word_at_a_time', (core_recv, [8]), ['dma_core', 'dma_desc', 'dma_order', 'recv_part',
                                                      'recv_rxctl']),
        ('receive_sync_set', ([sel] + part(8, True, cfg=rx | 4) + done5 + end5, [8]), ['dma_cfg']),
        ('receive_no_count', ([sel] + part(8, True, rxctl=0x10015) + done5 + end5, [8]), ['recv_rxctl']),
        ('receive_rwc_wrong', ([sel] + part(8, True, rwc=3) + done5 + end5, [8]), ['recv_rwc']),
        ('receive_en_first', ([sel, w('SPI0_CTL', 0x2593)] + part(8, False) + done5 + end5, [8]),
         ['dma_enable_last', 'dma_order']),
        ('receive_rwc_before_dma', ([sel] + [part(8, True)[k] for k in (0, 3, 1, 2, 4, 5)] + done5 + end5, [8]),
         ['dma_order']),
        ('receive_en_cleared_between_parts', ([sel] + part(4, True) + done5 + [w('SPI0_CTL', 0x2592)]
                                              + part(520, True) + done5 + end5, [4, 520]), ['recv_part']),
        ('receive_miom_between_parts', ([sel] + part(4, True) + done5 + [w('SPI0_CTL', 0x242593)]
                                        + part(520, False) + done5 + end5, [4, 520]), ['recv_part']),
        ('receive_ren_left_set', ([sel] + part(4, True) + done5[:2] + part(520, False) + done5 + end5, [4, 520]),
         ['recv_part']),
        ('receive_one_part_for_two', (one, [4, 520]), ['dma_desc', 'recv_part', 'recv_rwc']),
        ('receive_en_before_ren', ([sel] + part(8, True) + done5[:2] + [end5[0], done5[2], desel], [8]),
         ['recv_end', 'recv_part']),
        ('receive_select_first', ([sel] + part(8, True) + done5 + [desel, end5[0]], [8]), ['recv_end']),
    ]

    # the slave's receive: a request of 12 + 64 bytes, the header and then the rest; and a header alone
    def listen(cfg=rx, rxctl=C.SLAVE_RXCTL, en=True, n=12):
        return [w('SPI2_CTL', 0x22510), w('DMA9_CFG', 0), w('SPI2_IMSK_CLR', RF), ev('desc', ch=9, bytes=[n]),
                w('DMA9_DSCPTR_CUR', 0x11800100), w('DMA9_CFG', cfg), w('SPI2_RXCTL', rxctl)] + \
            ([w('SPI2_CTL', 0x22511)] if en else []) + [w('PORTA_FER_SET', PA4)]

    def rest(n=64):
        return [r('DMA9_STAT', 1), w('DMA9_STAT', 1), ev('desc', ch=9, bytes=[n]),
                w('DMA9_DSCPTR_CUR', 0x11800100), w('DMA9_CFG', rx)]

    end9 = [r('DMA9_STAT', 1), w('DMA9_STAT', 1), w('PORTA_FER_CLR', PA4), w('SPI2_CTL', 0x22510),
            w('SPI2_RXCTL', 0x10014)]
    req = listen() + rest() + end9
    slave = [
        ('request', (req, [12, 64]), []),
        ('request_header_alone', (listen() + end9, [12]), []),
        # outside the plain mode the rest is the padded payload and the trailer
        ('request_with_trailer', (listen() + rest(72) + end9, [12, 72]), []),
        # SPI2 left enabled since the reply before: EN is set already
        ('request_en_already_set', ([w('SPI2_CTL', 0x6511), w('SPI2_CTL', 0x22511)] + listen(en=False)[1:]
                                    + rest() + end9, [12, 64]), []),
        ('request_word_at_a_time', ([w('SPI2_CTL', 0x22510), w('SPI2_RXCTL', 1), w('SPI2_CTL', 0x22511),
                                     w('PORTA_FER_SET', PA4)] + [r('SPI2_RFIFO', 0)] * 19
                                    + [w('PORTA_FER_CLR', PA4), w('SPI2_CTL', 0x22510), w('SPI2_RXCTL', 0)],
                                    [12, 64]), ['dma_core', 'dma_desc', 'dma_order', 'recv_end', 'recv_part',
                                                'recv_rxctl']),
        ('request_one_part', (listen(n=76) + end9, [12, 64]), ['dma_desc', 'recv_end', 'recv_header', 'recv_part']),
        ('request_header_8', (listen(n=8) + rest(68) + end9, [12, 64]), ['dma_desc', 'recv_header']),
        ('request_word_count', (listen()[:6] + [w('SPI2_RWC', 3), w('SPI2_RXCTL', 0x1001D)] + listen()[7:]
                                + rest() + end9, [12, 64]), ['recv_rxctl']),
        ('request_sync_set', (listen(cfg=rx | 4) + rest() + end9, [12, 64]), ['dma_cfg']),
        ('request_pa4_before_dma', ([listen()[0], listen()[-1]] + listen()[1:-1] + rest() + end9, [12, 64]),
         ['dma_order', 'recv_part']),
        ('request_en_mid_setup', (listen()[:3] + [w('SPI2_CTL', 0x22511)] + listen(en=False)[3:] + rest() + end9,
                                  [12, 64]), ['dma_order']),
        ('request_dma_not_stopped', (listen()[:1] + listen()[2:] + rest() + end9, [12, 64]), ['dma_order']),
        ('request_pa4_taken_between_parts', (listen() + rest()[:2] + [w('PORTA_FER_CLR', PA4)] + rest()[2:]
                                             + [w('PORTA_FER_SET', PA4)] + end9, [12, 64]), ['recv_part']),
        ('request_en_cleared_between_parts', (listen() + rest()[:2] + [w('SPI2_CTL', 0x22510)] + rest()[2:]
                                              + [w('SPI2_CTL', 0x22511)] + end9, [12, 64]), ['recv_part']),
        ('request_ren_cleared_between_parts', (listen() + rest()[:2] + [w('SPI2_RXCTL', 0x10014)] + rest()[2:]
                                               + [w('SPI2_RXCTL', 0x10015)] + end9, [12, 64]), ['recv_part']),
        ('request_en_left_set', (listen() + rest() + end9[:3], [12, 64]), ['recv_end']),
        ('request_en_before_pa4', (listen() + rest() + end9[:2] + [end9[3], end9[2], end9[4]], [12, 64]),
         ['recv_end']),
    ]

    # the slave's reply: the feed only. A two-word reply; in single I/O the received words are read and counted
    def reply(desc, cfg=tx, feed=()):
        return [w('SPI2_CTL', 0x6510), w('SPI2_STAT', 0xFFFFFFFF), w('DMA8_STAT', 1), ev('desc', ch=8, bytes=desc),
                w('DMA8_DSCPTR_CUR', 0x11800100), w('DMA8_CFG', cfg), w('SPI2_TWC', 2),
                w('SPI2_TXCTL', C.REPLY_TXCTL), w('SPI2_RXCTL', 1), w('SPI2_CTL', 0x6511),
                w('PORTA_FER_SET', PA4)] + list(feed) + [r('SPI2_RFIFO', 0), r('SPI2_RFIFO', 0),
                                                         w('PORTA_FER_CLR', PA4), w('SPI2_TXCTL', 0x1011C),
                                                         w('DMA8_CFG', 0)]
    core_reply = [e for e in reply([8, 8]) if 'DMA8' not in e.get('reg', '') and e.get('ev') != 'desc']
    replies = [
        ('reply_feed', reply([8, 8]), []),
        ('reply_feed_data', reply([8, 512, 8]), ['dma_desc']),           # 130 words' descriptors for 2 words
        ('reply_core_feed', core_reply[:9] + [w('SPI2_TFIFO', 0)] * 4 + core_reply[9:],
         ['dma_core', 'dma_desc']),
        ('reply_nothing_queued', reply([8]), ['dma_desc']),
        ('reply_sync_set', reply([8, 8], cfg=tx | 4), ['dma_cfg']),
        ('reply_core_tops_up', reply([8, 8], feed=[w('SPI2_TFIFO', 0)]), ['dma_core']),
    ]

    rows = [dict(name=n, kind='master_send', words=19, trace=t, expect=x) for n, t, x in sends]
    rows += [dict(name=n, kind='master_receive', parts=p, trace=t, expect=x) for n, (t, p), x in receives]
    rows += [dict(name=n, kind='slave_receive', parts=p, trace=t, expect=x) for n, (t, p), x in slave]
    rows += [dict(name=n, kind='slave_reply', words=2, trace=t, expect=x) for n, t, x in replies]
    return {'rows': rows}


def spi_configs():
    rows = [
        ('flash_25mhz', 'flash', 0x0003, 3, True), ('flash_50mhz', 'flash', 0x0003, 1, True),
        ('flash_mode3', 'flash', 0x0033, 1, True), ('flash_100mhz', 'flash', 0x0003, 0, False),
        ('flash_lsb_first', 'flash', 0x1003, 3, False), ('flash_mode1', 'flash', 0x0013, 3, False),
        ('flash_disabled_clk0', 'flash', 0x0002, 0, True),
        ('slave_boot', 'slave_boot', 0x0013, 2, True), ('slave_boot_mode0', 'slave_boot', 0x0003, 2, False),
        ('slave_boot_50mhz', 'slave_boot', 0x0013, 1, False),
        ('slave_boot_fc', 'slave_boot', 0x2013, 2, True),
        ('slave_boot_fc_active_high', 'slave_boot', 0xA013, 2, False),
        ('bridge_master_pre', 'bridge_master_pre', 0x2593, 3, True),
        ('bridge_master_post', 'bridge_master_post', 0x2593, 3, True),
        ('bridge_master_post_quad', 'bridge_master_post', 0x242593, 1, False),
        ('bridge_master_post_fast', 'bridge_master_post', 0x2593, 1, False),
        ('bridge_master_pre_fast', 'bridge_master_pre', 0x2593, 1, False),
        ('bridge_slave_pre', 'bridge_slave_pre', 0x2511, 0, True),
        ('bridge_slave_post', 'bridge_slave_post', 0x2511, 0, True),
        ('bridge_slave_post_quad', 'bridge_slave_post', 0x202511, 0, False),
        ('bridge_slave_8bit', 'bridge_slave_pre', 0x2111, 0, False),
        # S-13b.044: SPI2_CTL while PA4 is handed to SPI2, by session direction
        ('bridge_slave_reply_pre', 'bridge_slave_reply_pre', 0x6511, 0, True),
        ('bridge_slave_reply_post', 'bridge_slave_reply_post', 0x6511, 0, True),
        ('bridge_slave_request_pre', 'bridge_slave_request_pre', 0x22511, 0, True),
        ('bridge_slave_request_post', 'bridge_slave_request_post', 0x22511, 0, True),
        ('bridge_slave_reply_rx_channel', 'bridge_slave_reply_post', 0x2511, 0, False),
        ('bridge_slave_reply_rx_channel_pre', 'bridge_slave_reply_pre', 0x2511, 0, False),
        ('bridge_slave_reply_half_empty', 'bridge_slave_reply_post', 0x26511, 0, False),
        ('bridge_slave_reply_active_high', 'bridge_slave_reply_post', 0xe511, 0, False),
        ('bridge_slave_request_tx_channel', 'bridge_slave_request_post', 0x6511, 0, False),
        ('bridge_slave_request_full', 'bridge_slave_request_post', 0x2511, 0, False),
        ('bridge_slave_reply_quad', 'bridge_slave_reply_post', 0x206511, 0, False),
        ('bridge_slave_request_quad', 'bridge_slave_request_post', 0x222511, 0, False),
        ('bridge_slave_reply_disabled', 'bridge_slave_reply_post', 0x6510, 0, False),
        ('bridge_slave_request_rx_full_pre', 'bridge_slave_request_pre', 0x2511, 0, False),
        # S-13b.041, .042, .044: the quad I/O option after the link, off by default
        ('bridge_master_quad_opt', 'bridge_master_quad_opt', 0x242593, 1, True),
        ('bridge_master_quad_opt_100mhz', 'bridge_master_quad_opt', 0x242593, 0, False),
        ('bridge_master_quad_opt_no_fast_mode', 'bridge_master_quad_opt', 0x202593, 1, False),
        ('bridge_slave_quad_opt', 'bridge_slave_quad_opt', 0x202511, 0, True),
        ('bridge_slave_quad_opt_single', 'bridge_slave_quad_opt', 0x2511, 0, False),
        ('bridge_slave_reply_quad_opt', 'bridge_slave_reply_quad_opt', 0x206511, 0, True),
        ('bridge_slave_request_quad_opt', 'bridge_slave_request_quad_opt', 0x222511, 0, True),
        # S-13b.041, S-09.075: the master's request sessions on the link's rungs
        ('bridge_master_rung1', 'bridge_master_rung1', 0x2593, 3, True),
        ('bridge_master_rung1_50mhz', 'bridge_master_rung1', 0x2593, 1, False),
        ('bridge_master_rung2', 'bridge_master_rung2', 0x42593, 1, True),
        ('bridge_master_rung2_no_fast_mode', 'bridge_master_rung2', 0x2593, 1, False),
        ('bridge_master_rung3', 'bridge_master_rung3', 0x242593, 3, True),
        ('bridge_master_rung3_single', 'bridge_master_rung3', 0x42593, 3, False),
        ('bridge_master_rung4', 'bridge_master_rung4', 0x242593, 1, True),
        ('bridge_master_rung4_no_fast_mode', 'bridge_master_rung4', 0x202593, 1, False),
        ('bridge_master_rung4_100mhz', 'bridge_master_rung4', 0x242593, 0, False),
    ]
    return {'sclk0_hz': C.SCLK0, 'init_spi2_clk': 1,
            'rows': [{'name': n, 'kind': k, 'ctl': c, 'clk': d, 'ok': ok} for n, k, c, d, ok in rows]}


def _f(mosi, miso=''):
    return {'mosi': mosi, 'miso': miso}


RDY = _f('0500', '0000')
BUSY = _f('0500', '0001')


def flash_traces():
    rec = '00' * 0x34
    good = {
        'metadata_read': [RDY, _f('0300c000' + rec)],
        'selector_read': [RDY, _f('0300d000' + '0000')],
        'metadata_rewrite': [RDY, _f('0300c000' + rec), RDY, _f('06'), _f('2000c000'), BUSY, BUSY, RDY,
                             _f('06'), _f('0200c000' + 'a5' * 0x34), BUSY, RDY],
        'slot_page_writes': [RDY, _f('06'), _f('20010000'), BUSY, RDY, _f('06'), _f('02010000' + '11' * 256),
                             RDY, _f('06'), _f('02010100' + '22' * 256), RDY],
        'identify': [RDY, _f('90000000' + '0000', '00000000' + '9d13'), _f('9f' + '0000000000', '009d601400')],
        'rdsr_busy_then_ready': [RDY, _f('06'), _f('2000c000'), _f('050000', '000100'), _f('0300c000' + '00')],
    }
    bad = {
        'unknown_opcode': ([RDY, _f('06'), _f('d8010000')], ['opcode_set']),
        'address_past_flash': ([RDY, _f('03100000' + '00')], ['address_range']),
        'short_address': ([RDY, _f('06'), _f('2000c0')], ['frame_shape']),
        'pp_without_wren': ([RDY, _f('0200c000' + '00' * 4)], ['wren_before_write']),
        'read_between_wren_and_se': ([RDY, _f('06'), _f('0300c000' + '00'), _f('2000c000')],
                                     ['wren_before_write']),
        'read_while_busy': ([RDY, _f('06'), _f('2000c000'), BUSY, _f('0300c000' + '00')], ['busy_wait']),
        'wren_while_busy': ([RDY, _f('06'), _f('2000c000'), _f('06'), _f('0200c000' + '00')], ['busy_wait']),
        'pp_crosses_page': ([RDY, _f('06'), _f('0200c0f0' + '00' * 0x20), RDY], ['alignment']),
        'se_unaligned': ([RDY, _f('06'), _f('2000c100'), RDY], ['alignment']),
        'rdsr_ready_then_busy': ([RDY, _f('06'), _f('2000c000'), _f('050000', '000001'), _f('0300c000' + '00')],
                                 ['busy_wait']),
    }
    rows = [{'name': k, 'frames': v, 'expect': []} for k, v in good.items()]
    rows += [{'name': k, 'frames': v[0], 'expect': v[1]} for k, v in bad.items()]
    # S-13b.015: a stage's trace up to its reset, power-off request or hand-off
    se, pp = [RDY, _f('06'), _f('2000d000')], [_f('06'), _f('0200d000' + '0101')]
    end = [
        ('end_after_read', good['metadata_read'], []),
        ('end_after_write_waited', se + [BUSY, RDY] + pp + [BUSY, BUSY, RDY], []),
        ('end_after_write_then_read', se + [BUSY, RDY] + pp + [RDY, _f('0300d000' + '0000')], []),
        ('end_right_after_program', se + [BUSY, RDY] + pp, ['busy_at_end']),
        ('end_right_after_erase', se, ['busy_at_end']),
        ('end_while_still_busy', se + [BUSY, RDY] + pp + [BUSY, BUSY], ['busy_at_end']),
        ('end_ready_then_busy', se + [BUSY, RDY] + pp + [_f('050000', '000001')], ['busy_at_end']),
    ]
    return {'opcodes': sorted(C.OPCODES), 'rows': rows,
            'at_end': [{'name': n, 'frames': f, 'expect': x} for n, f, x in end]}


def spi_start():
    """S-13b.019 (the first SPI2_CTL write of a stage) and S-13b.010, .020 (SPI_SLVSEL written twice)."""
    def sel(spi, val):
        return [w(spi + '_SLVSEL', val), w(spi + '_SLVSEL', val)]

    frame = {'op': 'frame'}
    idle, low = 0xFE02, 0xFC02          # SEL1 enabled; its level bit high, low
    # a reference set of first SPI2 writes, then one frame
    reference = ([w('SPI2_CLK', 1), w('SPI2_DLY', 0x301)] + sel('SPI2', 0x202)
                 + [w('SPI2_CTL', 0x2), w('SPI2_TXCTL', 0x10C), w('SPI2_RXCTL', 0x8), w('SPI2_TXCTL', 0x10D),
                    w('SPI2_RXCTL', 0x9), w('SPI2_CTL', 0x3)] + sel('SPI2', 0x002) + [frame] + sel('SPI2', 0x202))
    zero_first = ([w('SPI2_CTL', 0)] + sel('SPI2', idle) + [w('SPI2_CLK', 3), w('SPI2_CTL', 0x0033)]
                  + sel('SPI2', low) + [frame] + sel('SPI2', idle))
    rows = [
        ('reference_first_writes', 'SPI2', reference, []),
        ('ctl_zero_first', 'SPI2', zero_first, []),
        ('fast_mode_later', 'SPI2', zero_first + [w('SPI2_CTL', 0), w('SPI2_CLK', 1), w('SPI2_CTL', 0x40033)]
         + sel('SPI2', low) + [frame] + sel('SPI2', idle), []),
        ('first_ctl_keeps_enable', 'SPI2', [w('SPI2_CTL', 0x0033)] + zero_first[1:], ['first_ctl']),
        ('first_ctl_keeps_window', 'SPI2', [w('SPI2_CTL', 0x80000002)] + zero_first, ['first_ctl']),
        ('first_ctl_keeps_fast_mode', 'SPI2', [w('SPI2_CTL', 0x00040002)] + zero_first, ['first_ctl']),
        ('first_ctl_keeps_quad', 'SPI2', [w('SPI2_CTL', 0x00200002)] + zero_first, ['first_ctl']),
        ('frame_before_ctl', 'SPI2', sel('SPI2', low) + [frame] + sel('SPI2', idle) + zero_first, ['first_ctl']),
        ('select_written_once', 'SPI2', zero_first[:-2] + [w('SPI2_SLVSEL', idle)], ['slvsel_twice']),
        ('select_two_values', 'SPI2', [w('SPI2_CTL', 0), w('SPI2_SLVSEL', idle), w('SPI2_SLVSEL', low), frame]
         + sel('SPI2', idle), ['slvsel_twice']),
        ('boot_link_select', 'SPI0', [w('SPI0_CLK', 2), w('SPI0_DLY', 0x301)] + sel('SPI0', 0x808)
         + [w('SPI0_CTL', 0x12), w('SPI0_CTL', 0x13)] + sel('SPI0', 0x008) + [frame] + sel('SPI0', 0x808), []),
        ('boot_link_select_once', 'SPI0', [w('SPI0_CTL', 0x12), w('SPI0_CTL', 0x13), w('SPI0_SLVSEL', 0x008), frame]
         + sel('SPI0', 0x808), ['slvsel_twice']),
    ]
    return {'rows': [{'name': n, 'spi': s, 'trace': t, 'expect': x} for n, s, t, x in rows]}


def cmd(idx, arg=0, resp=0, t=0.0):
    return {'ev': 'cmd', 'idx': idx, 'arg': arg, 'resp': resp, 't_ms': t}


TRAN_READY = 0x900
RCA16 = C.RCA << 16


def reference_init(hs=0x01, extra=()):
    e = [{'ev': 'clock', 'hz': 396825}, {'ev': 'width', 'bits': 1}, cmd(0), cmd(0),
         cmd(1, C.OCR_ARG, 0x00FF8080, 1.0), cmd(1, C.OCR_ARG, 0x00FF8080, 2.0), cmd(1, C.OCR_ARG, 0xC0FF8080, 3.0),
         cmd(2), cmd(3, RCA16), cmd(9, RCA16), cmd(7, RCA16, 0x700),
         cmd(6, 0x03B70200, 0x800), cmd(13, RCA16, TRAN_READY), {'ev': 'width', 'bits': 8}, cmd(8, 0, TRAN_READY)]
    if hs is not None:
        e += [cmd(6, 0x03B90000 | hs << 8, 0x800), {'ev': 'busy_end'}, {'ev': 'clock', 'hz': 50_000_000},
              cmd(8, 0, TRAN_READY)]
    for idx, val in ((34, 1), (33, 1), (163, 2)):
        e += [cmd(6, 0x03000000 | idx << 16 | val << 8, 0x800), cmd(13, RCA16, TRAN_READY), cmd(8, 0, TRAN_READY)]
    return e + list(extra)


def emmc():
    good = reference_init()
    seq = [{'name': 'reference', 'events': good, 'expect': []},
           {'name': 'no_high_speed_emmc', 'events': reference_init(None), 'expect': []}]

    def mut(name, events, expect):
        seq.append({'name': name, 'events': events, 'expect': expect})

    mut('no_cmd1_retry', [e for e in good if not (e['ev'] == 'cmd' and e['idx'] == 1 and not e['resp'] >> 31)][:6]
        + [cmd(1, C.OCR_ARG, 0x00FF8080, 1.0)], ['emmc_init'])
    mut('wrong_ocr', [dict(e, arg=0x00FF8000) if e['ev'] == 'cmd' and e['idx'] == 1 else e for e in good],
        ['emmc_init'])
    w8 = good.index({'ev': 'width', 'bits': 8})
    # CMD8 after the eMMC went to 8 bits but before the host did: a width mismatch on a data transfer
    mut('width_after_ext_csd', good[:w8] + [good[w8 + 1], good[w8]] + good[w8 + 2:], ['bus_width_match'])
    i7 = next(i for i, e in enumerate(good) if e['ev'] == 'cmd' and e['idx'] == 7)
    # the order after CMD7 is free: EXT_CSD read on the 1-bit bus before the width switch
    mut('ext_csd_before_width', good[:i7 + 1] + [cmd(8, 0, TRAN_READY)] + good[i7 + 1:w8 + 1] + good[w8 + 2:], [])
    # CMD9 is required: S-03.001's erase group is the CSD's
    mut('no_csd_read', [e for e in good if not (e['ev'] == 'cmd' and e['idx'] == 9)], ['emmc_init'])
    mut('fast_clock_in_ident', [{'ev': 'clock', 'hz': 25_000_000}] + good[1:], ['ident_clock'])
    mut('fast_clock_after_select', good[:i7 + 1] + [{'ev': 'clock', 'hz': 50_000_000}] + good[i7 + 1:],
        ['compat_clock'])
    mut('clock_26_after_select', good[:i7 + 1] + [{'ev': 'clock', 'hz': 25_000_000}] + good[i7 + 1:], [])
    mut('no_width_switch', [e for e in good if not (e['ev'] == 'cmd' and e['arg'] == 0x03B70200)
                            and e != {'ev': 'width', 'bits': 8}], ['emmc_init'])
    stuck = good[:4] + [cmd(1, C.OCR_ARG, 0x00FF8080, t) for t in (1.0, 100.0, 300.0)] + [{'ev': 'fail'}]
    mut('cmd1_gives_up_early', stuck, ['cmd1_bound'])
    stuck_ok = good[:4] + [cmd(1, C.OCR_ARG, 0x00FF8080, t) for t in (1.0, 600.0, 1200.0)] + [{'ev': 'fail'}]
    mut('cmd1_gives_up_after_1s', stuck_ok, [])
    i6 = next(i for i, e in enumerate(good) if e['ev'] == 'cmd' and e['arg'] == 0x03220100)
    mut('switch_no_busy_wait', good[:i6 + 1] + good[i6 + 2:], ['emmc_switch'])
    mut('switch_other_index', good + [cmd(6, 0x03AF0100), cmd(13, RCA16, TRAN_READY)], ['emmc_switch'])
    ih = next(i for i, e in enumerate(good) if e['ev'] == 'cmd' and e['arg'] == 0x03B90100)
    assert good[ih + 1]['ev'] == 'busy_end' and good[ih + 2]['ev'] == 'clock'
    mut('clock_before_hs_busy_end', good[:ih + 1] + [good[ih + 2], good[ih + 1]] + good[ih + 3:], ['compat_clock'])
    # a repeated sequence, or the wake after an unmount: the host is where the earlier sequence left it
    earlier = [{'ev': 'clock', 'hz': 50_000_000}, {'ev': 'width', 'bits': 8}]
    assert good[0]['ev'] == 'clock' and good[1]['ev'] == 'width'
    mut('wake_from_step_1', earlier + good, [])
    mut('wake_clock_kept', earlier + good[1:], ['ident_clock'])
    mut('wake_width_kept', earlier + [good[0]] + good[2:], ['ident_width'])

    timing = [
        {'name': 'hs200_driver3', 'device_type': 0x57, 'driver_strength': 0x0F, 'hs_timing': 0x32,
         'clock_hz': 50_000_000, 'expect': []},
        {'name': 'hs200_emmc_as_high_speed', 'device_type': 0x57, 'driver_strength': 0x0F, 'hs_timing': 0x01,
         'clock_hz': 50_000_000, 'expect': []},
        {'name': 'hs200_without_driver3', 'device_type': 0x57, 'driver_strength': 0x01, 'hs_timing': 0x32,
         'clock_hz': 50_000_000, 'expect': ['timing_vs_clock']},
        {'name': 'hs200_without_device_bit', 'device_type': 0x03, 'driver_strength': 0x0F, 'hs_timing': 0x32,
         'clock_hz': 50_000_000, 'expect': ['timing_vs_clock']},
        {'name': 'hs200_above_50', 'device_type': 0x57, 'driver_strength': 0x0F, 'hs_timing': 0x32,
         'clock_hz': 52_000_000, 'expect': ['timing_vs_clock']},
        {'name': 'high_speed_52', 'device_type': 0x57, 'driver_strength': 0x01, 'hs_timing': 0x01,
         'clock_hz': 50_000_000, 'expect': []},
        {'name': 'high_speed_26_only', 'device_type': 0x01, 'driver_strength': 0x01, 'hs_timing': 0x01,
         'clock_hz': 50_000_000, 'expect': ['timing_vs_clock']},
        {'name': 'no_switch_slow', 'device_type': 0x00, 'driver_strength': 0x00, 'hs_timing': None,
         'clock_hz': 396825, 'expect': []},
        {'name': 'no_switch_fast', 'device_type': 0x00, 'driver_strength': 0x00, 'hs_timing': None,
         'clock_hz': 50_000_000, 'expect': ['timing_vs_clock']},
        {'name': 'above_50', 'device_type': 0x57, 'driver_strength': 0x01, 'hs_timing': 0x01,
         'clock_hz': 100_000_000, 'expect': ['timing_vs_clock']},
        {'name': 'high_speed_26_only_25', 'device_type': 0x01, 'driver_strength': 0x01, 'hs_timing': 0x01,
         'clock_hz': 25_000_000, 'expect': []},
        {'name': 'hs200_only_driver1', 'device_type': 0x10, 'driver_strength': 0x01, 'hs_timing': 0x01,
         'clock_hz': 50_000_000, 'expect': []},
    ]
    poff = [cmd(6, 0x03A30000, 0x800), cmd(13, RCA16, TRAN_READY), cmd(6, C.POWER_OFF_LONG, 0x800)]
    power_off = [
        {'name': 'dat0_wait', 'events': poff + [{'ev': 'busy_end'}], 'expect': []},
        {'name': 'status_and_read_back', 'events': poff + [cmd(13, RCA16, TRAN_READY), cmd(8, 0, TRAN_READY)],
         'expect': ['power_off_quiet']},
        {'name': 'status_poll_only', 'events': poff + [cmd(13, RCA16, 0x800), cmd(13, RCA16, TRAN_READY)],
         'expect': ['power_off_quiet']},
    ]

    def tr(name, cmds, expect):
        return {'name': name, 'events': cmds, 'expect': expect}

    transfer = [
        tr('read_single', [cmd(17, 0x1000)], []),
        tr('read_multi', [cmd(18, 0x1001), cmd(12)], []),
        tr('write_multi', [cmd(25, 0x2000), cmd(12), cmd(13, RCA16, 0xE00), cmd(13, RCA16, TRAN_READY),
                           cmd(17, 0x2000)], []),
        tr('write_reliable', [cmd(23, 0x80000080), cmd(25, 0x2000), cmd(13, RCA16, TRAN_READY)], []),
        tr('erase_group', [cmd(35, 0x400), cmd(36, 0x400), cmd(38, 0), cmd(13, RCA16, TRAN_READY)], []),
        tr('read_multi_unended', [cmd(18, 0x1001), cmd(17, 0x0)], ['emmc_transfer']),
        tr('erase_range', [cmd(35, 0x400), cmd(36, 0x7FF), cmd(38, 0), cmd(13, RCA16, TRAN_READY)],
           ['emmc_transfer']),
        tr('read_after_write_busy', [cmd(24, 0x10), cmd(17, 0x10)], ['emmc_transfer']),
    ]
    clocks = {'source_hz': C.SCLK0 // 2, 'ident_clkdiv': 63, 'ident_clkdiv_too_fast': 62, 'hs_clkdiv': 0}
    # S-13b.055: what ends a busy period. R1: READY_FOR_DATA bit 8, CURRENT_STATE bits 12:9 (4 transfer,
    # 7 programming), SWITCH_ERROR bit 7
    prg_ready, sw_err = 0x100 | 7 << 9, TRAN_READY | 0x80
    sw = cmd(6, 0x03210100, 0x800)
    busy_end = [
        tr('switch_status', [sw, cmd(13, RCA16, 0xE00), cmd(13, RCA16, TRAN_READY), cmd(17, 0)], []),
        tr('switch_dat0', [sw, {'ev': 'busy_end'}, cmd(17, 0)], []),
        tr('write_status', [cmd(25, 0x2000), cmd(12), cmd(13, RCA16, 0xE00), cmd(13, RCA16, TRAN_READY),
                            cmd(17, 0x2000)], []),
        tr('switch_error_fails', [sw, cmd(13, RCA16, sw_err), {'ev': 'fail'}], []),
        tr('ready_in_programming_state', [cmd(24, 0x10), cmd(13, RCA16, prg_ready), cmd(17, 0x10)],
           ['emmc_busy_end']),
        tr('switch_ready_in_programming_state', [sw, cmd(13, RCA16, prg_ready), cmd(8, 0, TRAN_READY)],
           ['emmc_busy_end']),
        tr('erase_ready_in_programming_state', [cmd(35, 0x400), cmd(36, 0x400), cmd(38, 0),
                                                cmd(13, RCA16, prg_ready), cmd(17, 0x400)], ['emmc_busy_end']),
        tr('switch_error_ignored', [sw, cmd(13, RCA16, sw_err), cmd(8, 0, TRAN_READY)], ['emmc_switch_error']),
    ]
    return {'rca': C.RCA, 'ocr_arg': C.OCR_ARG, 'switch_indexes': sorted(C.SWITCH_INDEXES), 'clocks': clocks,
            'sequences': seq, 'timing': timing, 'transfer': transfer, 'power_off': power_off,
            'busy_end': busy_end}


def uart():
    rows = [('uart0', 'uart0', 0x301, 0x80000064, True), ('uart1', 'uart1', 0x18000301, 0x80000064, True),
            ('uart1_no_acts', 'uart1', 0x08000301, 0x80000064, False),
            ('uart0_flow', 'uart0', 0x18000301, 0x80000064, False),
            ('uart0_x16', 'uart0', 0x301, 0x64, False), ('uart0_parity', 'uart0', 0x4301, 0x80000064, False),
            ('uart0_7bit', 'uart0', 0x201, 0x80000064, False), ('uart0_two_stop', 'uart0', 0x1301, 0x80000064, False)]
    return {'rows': [{'name': n, 'link': l, 'ctl': c, 'clk': k, 'ok': ok} for n, l, c, k, ok in rows]}


def uart_close():
    """S-13b.072: the transmitter is empty before a UART is disabled."""
    temt, busy = 0x80, 0x00

    def u(n, events):
        return [dict(e, reg='UART%d_%s' % (n, e['reg'])) if 'reg' in e else e for e in events]

    on, off = [w('CTL', 0x301)], [w('CTL', 0)]
    send = [w('THR', 0x7E), w('THR', 0x01)]
    rows = [
        ('drained', u(0, on + send + [r('STAT', busy), r('STAT', temt)] + off), []),
        ('nothing_sent', u(0, on + [r('STAT', temt)] + off), []),
        ('wait_ran_out', u(1, on + send + [r('STAT', busy), {'op': 'ev', 'ev': 'tx_wait_expired', 'uart': 'UART1'}]
                           + off), []),
        ('never_enabled', u(1, off), []),
        ('disabled_twice', u(0, on + [r('STAT', temt)] + off + off), []),
        ('cut_mid_byte', u(0, on + send + [r('STAT', busy)] + off), ['uart_cut']),
        ('no_status_read', u(0, on + send + off), ['uart_cut']),
        ('byte_after_empty', u(0, on + [r('STAT', temt)] + send + off), ['uart_cut']),
        ('other_uart_read', u(0, on + send) + u(1, [r('STAT', temt)]) + u(0, off), ['uart_cut']),
    ]
    return {'rows': [{'name': n, 'trace': t, 'expect': x} for n, t, x in rows]}


def boot_dma():
    """S-13b.023: a part of the slave boot stream by DMA4 under SPI0's flow control, as register traces
    (check_boot_dma): the control value with flow control, the select, a 64-byte part, its end."""
    def ev(name, **kw):
        return dict({'op': 'ev', 'ev': name}, **kw)

    TF = C.SPI_TF
    sel, desel = ev('select', on=True), ev('select', on=False)
    head = [w('SPI0_CTL', 0), w('SPI0_CTL', C.BOOT_CTL_FC), sel]

    def setup(n=64, cfg=C.BOOT_DMA_CFG, desc=None):
        return [w('SPI0_TXCTL', 0), w('SPI0_RXCTL', 0), w('SPI0_STAT', 0xB0000FF0), w('DMA4_STAT', 7),
                ev('desc', ch=4, bytes=desc or [n]), w('DMA4_DSCPTR_CUR', 0x11800100), w('DMA4_CFG', cfg),
                w('SPI0_TWC', n), w('SPI0_TXCTL', C.BOOT_TXCTL), w('SPI0_TXCTL', C.BOOT_TXCTL | 1)]

    end = [r('DMA4_STAT', 0), r('DMA4_STAT', 1), w('DMA4_STAT', 1), r('SPI0_STAT', 0x40000),
           r('SPI0_STAT', 0x40000 | TF), w('SPI0_TXCTL', 0), w('SPI0_STAT', 0xB0000FF0), w('DMA4_CFG', 0),
           w('DMA4_STAT', 7), desel]
    good = head + setup() + end
    s_ = setup()
    rows = [
        ('part', good, []),
        ('part_two_descriptors', head + setup(desc=[60, 4]) + end, []),
        ('part_memory_words', head + setup(cfg=C.BOOT_DMA_CFG | 0x200) + end, []),
        ('no_flow_control', [w('SPI0_CTL', 0), w('SPI0_CTL', 0x13), sel] + setup() + end, ['boot_ctl']),
        ('ready_active_high', [w('SPI0_CTL', 0), w('SPI0_CTL', C.BOOT_CTL_FC | 0x8000), sel] + setup() + end,
         ['boot_ctl']),
        ('en_cleared_in_part', head + [w('SPI0_CTL', 0x2012)] + setup() + [w('SPI0_CTL', C.BOOT_CTL_FC)] + end,
         ['boot_ctl']),
        ('bridge_peripheral_words', head + setup(cfg=C.DMA_CFG_SEND) + end, ['boot_cfg']),
        ('sync_set', head + setup(cfg=C.BOOT_DMA_CFG | 4) + end, ['boot_cfg']),
        ('desc_short', head + setup(desc=[60]) + end, ['boot_desc']),
        ('twc_wrong', head + s_[:7] + [w('SPI0_TWC', 16)] + s_[8:] + end, ['boot_twc']),
        # TEN set before SPI_TXCTL's other bits: the write after it clears TEN again
        ('ten_first', head + s_[:8] + [s_[9], s_[8]] + end, ['boot_end', 'boot_order']),
        ('twc_before_dma', head + s_[:4] + [s_[7]] + s_[4:7] + s_[8:] + end, ['boot_order']),
        ('no_stat_clear', head + s_[:2] + s_[3:] + end, ['boot_order']),
        ('rx_left_on', head + [s_[0]] + s_[2:] + end, ['boot_order']),
        ('byte_by_core', head + setup() + [w('SPI0_TFIFO', 0x5A)] + end, ['boot_core']),
        ('end_on_dma_done', head + setup() + end[:3] + end[5:], ['boot_end']),
        ('select_before_end', head + setup() + end[:5] + [desel] + end[5:9], ['boot_end']),
        ('dma_stopped_first', head + setup() + end[:5] + [end[7], end[5], end[6], end[8], desel], ['boot_end']),
        ('never_ends', head + setup(), ['boot_end']),
    ]
    return [{'name': name, 'n': 64, 'trace': t, 'violations': v} for name, t, v in rows]


def pinmux():
    return {role: {p: {'mux': m, 'fer': f} for p, (m, f) in ports.items()} for role, ports in C.PINMUX.items()}


ALL = {'pinmux': pinmux, 'pin_traces': pin_traces, 'strap_timing': strap_timing, 'bridge_pins': bridge_pins,
       'slave_tx': slave_tx, 'bridge_sessions': bridge_sessions,
       'reply_setup': reply_setup, 'bridge_dma': bridge_dma, 'boot_dma': boot_dma,
       'spi_configs': spi_configs, 'flash_traces': flash_traces, 'emmc_init': emmc, 'uart': uart,
       'spi_start': spi_start, 'uart_close': uart_close}


def main():
    os.makedirs(os.path.join(HERE, 'vectors'), exist_ok=True)
    for name, fn in ALL.items():
        with open(os.path.join(HERE, 'vectors', name + '.json'), 'w') as f:
            json.dump(fn(), f, indent=1, sort_keys=True)
            f.write('\n')


if __name__ == '__main__':
    main()
