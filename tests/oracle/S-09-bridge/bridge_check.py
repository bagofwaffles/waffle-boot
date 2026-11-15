#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-09 capture checks: bridge and boot-link bytes against the vectors and the master's rules.
Each check returns a list of failure strings; empty is a pass.

    python3 -I -B tests/oracle/S-09-bridge/bridge_check.py CHECK [--side S] [--vector V] CAPTURE.json

Exit 0 on a pass, 1 on a failure, 2 on a bad capture. Captures are JSON, byte strings in hex:
  stream       {"sessions": [H, ...]}  the boot link's MOSI sessions in order (--vector)
  request      {"items": [{"name", "hex"}]} (--side master) or [{"name", "rx_len", "session_len"}] (slave)
  reply        {"items": [{"name", "hex"}]}
  file_read    {"items": [{"name", "pieces": [H, ...]}]}
  record       {"items": [{"kind", "hex"}]}
  master       {"sessions": [{"mosi", "miso", "late"?, "rung"?}, ...]}  (--absent, --room-gaps)
  session_end  {"items": [{"name", "expect", "moved", "rose", "fell_again"}]}
  pad_trail    {"items": [{"name", "executed"}]}
"""
import argparse
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bridge_model as m  # noqa: E402

V = os.path.join(HERE, 'vectors')


def _load(name):
    with open(os.path.join(V, name)) as f:
        return json.load(f)


def _sha(b):
    return hashlib.sha256(bytes(b)).hexdigest()


def _b(h):
    return bytes.fromhex(h)


# ---- vector checks ----

def check_stream(vector, sessions):
    """S-09.004-S-09.006: the boot link's sessions against slave_stream_<vector>.json."""
    exp = _load('slave_stream_%s.json' % vector)['sessions']
    got = [bytes(s) for s in sessions]
    fails = []
    if len(got) != len(exp):
        fails.append('stream %s: %d sessions, expected %d' % (vector, len(got), len(exp)))
    for i, (g, e) in enumerate(zip(got, exp)):
        if len(g) != e['len'] or _sha(g) != e['sha256']:
            fails.append('stream %s: session %d differs (len %d, expected %d; head %s, expected %s)'
                         % (vector, i, len(g), e['len'], g[:20].hex(), e['head_hex']))
    return fails


def check_requests(items, side='master'):
    """S-09.020, S-09.021, S-09.023: request sessions against requests.json."""
    vec = {it['name']: it for it in _load('requests.json')}
    fails = []
    for it in items:
        e = vec.get(it['name'])
        if e is None:
            fails.append('request %s: no such vector item' % it['name'])
            continue
        if side == 'master':
            g = bytes(it['hex'])
            if len(g) != e['session_len']:
                fails.append('request %s: master session %d bytes, expected %d' % (it['name'], len(g),
                                                                                  e['session_len']))
            elif ('session_hex' in e and g.hex() != e['session_hex']) or \
                    ('session_sha256' in e and _sha(g) != e['session_sha256']):
                fails.append('request %s: master session bytes differ' % it['name'])
        else:
            if it['rx_len'] != e['slave_rx_len']:
                fails.append('request %s: slave took %d bytes as the request, expected %d' % (
                    it['name'], it['rx_len'], e['slave_rx_len']))
            if 'session_len' in it:
                if it['session_len'] != e['slave_session_len']:
                    fails.append('request %s: slave moved %d bytes in the session before its ready line rose, '
                                 'expected %d (S-09.012, S-09.021)' % (it['name'], it['session_len'],
                                                                       e['slave_session_len']))
            elif e['slave_session_len'] != e['slave_rx_len']:
                fails.append('request %s: no session_len in the capture; it is required for this item '
                             '(S-09.021)' % it['name'])
    return fails


def check_pad_trails(items):
    """S-09.021, S-09.013: cut request sessions against pad_trail.json. items: [{'name', 'executed'}]."""
    vec = {c['name']: c for c in _load('pad_trail.json')}
    fails = []
    for it in items:
        c = vec.get(it['name'])
        if c is None:
            fails.append('pad_trail %s: no such vector case' % it['name'])
        elif bool(it['executed']) != c['executed']:
            fails.append('pad_trail %s: session ended after %d of %d bytes (request %d): %s, expected %s' % (
                it['name'], c['moved'], c['session_len'], c['request_len'],
                'executed' if it['executed'] else 'dropped', 'executed' if c['executed'] else 'dropped'))
    return fails


def check_replies(items):
    """S-09.030-S-09.036: reply sessions against replies.json."""
    vec = {it['name']: it for it in _load('replies.json')}
    fails = []
    for it in items:
        e = vec.get(it['name'])
        if e is None:
            fails.append('reply %s: no such vector item' % it['name'])
        elif bytes(it['hex']).hex() != e['reply_hex']:
            fails.append('reply %s: %s, expected %s' % (it['name'], bytes(it['hex'])[:24].hex(),
                                                        e['reply_hex'][:48]))
    return fails


def _piece(b):
    """(r, data, errno) of a read piece reply (S-09.035 step 2), or a failure string."""
    if len(b) < 8 or len(b) % 4:
        return 'piece of %d bytes' % len(b)
    hdr, r = struct.unpack_from('<Ii', b, 0)
    if hdr & 0xFF != m.READ or hdr >> 16:
        return 'piece header %08x' % hdr
    if r > 0:
        if len(b) != 8 + m.pad4(r):
            return 'piece r=%d of %d bytes' % (r, len(b))
        return r, b[8:8 + r], 0
    if r == 0:
        return (0, b'', 0) if len(b) == 8 else 'piece r=0 of %d bytes' % len(b)
    if len(b) != 12:
        return 'piece r=%d of %d bytes' % (r, len(b))
    return r, b'', struct.unpack_from('<I', b, 8)[0]


def check_file_reads(items):
    """S-09.035: an implementation's piece replies against file_read.json."""
    vec = {c['name']: c for c in _load('file_read.json')}
    fails = []
    for it in items:
        c = vec.get(it['name'])
        if c is None:
            fails.append('file_read %s: no such case' % it['name'])
            continue
        got = [bytes(p) for p in it['pieces']]
        if len(got) != len(c['pieces']):
            fails.append('file_read %s: %d pieces, expected %d' % (it['name'], len(got), len(c['pieces'])))
        for k, (g, w) in enumerate(zip(got, c['pieces'])):
            p = _piece(g)
            if isinstance(p, str):
                fails.append('file_read %s piece %d: %s' % (it['name'], k, p))
                continue
            r, data, err = p
            if r != w['r'] or len(g) != w['reply_len']:
                fails.append('file_read %s piece %d: r=%d len %d, expected r=%d len %d'
                             % (it['name'], k, r, len(g), w['r'], w['reply_len']))
            elif r >= 0 and _sha(data) != w['sha256']:
                fails.append('file_read %s piece %d: data differs' % (it['name'], k))
            elif r < 0 and err != w['errno']:
                fails.append('file_read %s piece %d: errno %d, expected %d' % (it['name'], k, err, w['errno']))
    return fails


def check_records(items):
    """S-09.043: record bytes against records.json."""
    vec = {r['kind']: r for r in _load('records.json')}
    fails = []
    for it in items:
        e = vec.get(it['kind'])
        if e is None:
            fails.append('record %s: no such kind' % it['kind'])
        elif bytes(it['hex']).hex() != e['hex']:
            fails.append('record %s: bytes differ' % it['kind'])
    return fails


# ---- the master's protocol rules over a whole run ----

NEVER_SENT = {m.CLOCK_SYNC, 0x1F, 0x36}
RUNG_MODE = {0: m.PLAIN, 1: m.CHECKED, 2: m.CHECKED, 3: m.QUAD, 4: m.QUAD}


def check_master(sessions, absent_run=False, room_gaps=False, link_test_run=False):
    """A master run's bridge sessions against S-09.010-S-09.023, S-09.030-S-09.035, S-09.045, S-09.050,
    S-09.052 and S-09.070-S-09.078. sessions: [{'mosi': bytes, 'miso': bytes}], each with optional 'rung'
    (0-4, from SPI0) and 'late' (a wait in it ran out).
    absent_run: the slave stops answering (S-09.017, S-09.060); the run may end awaiting a reply.
    room_gaps: the receive room ran out (S-09.045); a request may go with no pull outstanding.
    link_test_run: a developer's link test, not a raise; link tests may follow a keep."""
    fails = []
    held = {}                    # slot -> request dict
    expect = None                # ('imm' | 'pull' | 'poll', request) when the next session is a reply
    n_req = 0
    pull_started = False
    released_ok = False
    absent = False               # S-09.017, S-09.052: no further session once the slave is absent
    late_at = None               # S-09.013: the session whose end ran out; nothing follows it
    mode = m.PLAIN               # S-09.070: the master's mode for its request sessions
    trial = None                 # S-09.073: the trial under way
    kept = 0                     # the rung kept, 0 for none
    failed_rungs = set()         # S-09.075
    fb = None                    # S-09.074: the fall-back under way
    last_seq = 0                 # S-09.076: the last number given
    used = False                 # a request other than the link's own since the keep
    gate_tests = 0               # S-09.075: the gate's tests since the keep
    unsettled = False            # S-09.074: an echo answered, no 0x19 since
    sent = {}                    # number -> the session that carried it
    repeats = {}                 # number -> times sent again
    prec = None                  # S-09.013 rule 4: a plain failure the slave survives, under way

    def plain_stake(expect, sess):
        """S-09.013 rule 4: the slots a failed plain session may have cost, or None when it is not
        recoverable (a request outside PLAIN_SAFE, the pull, or the start-up)."""
        if n_req < 3:                                            # S-09.017
            return None
        others = {k: r for k, r in held.items() if r['cmd'] != m.POLL}
        if any(r['cmd'] != m.PULL and r['cmd'] not in m.PLAIN_SAFE for r in others.values()):
            return None
        pulls = {k for k, r in others.items() if r['cmd'] == m.PULL}
        if expect is None:                                       # its request session
            mo = sess.get('mosi') or b''
            if not mo or mo[0] not in m.PLAIN_SAFE:
                return None
            return {k for k in others if k not in pulls} | ({mo[1]} if len(mo) > 1 else set())
        kind, rq = expect
        if kind == 'imm':
            return None if rq['cmd'] not in m.PLAIN_SAFE else set(others) - pulls
        if kind != 'poll':
            return None
        mi = sess.get('miso') or b''
        first = mi[:4] if len(mi) >= 4 else None
        if first == m.POLL_NONE:
            handed = None
        elif first is not None and first[0] in (others[k]['cmd'] for k in others if k == first[1]):
            handed = first[1]
        else:
            handed = 'any'
        if pulls and (handed == 'any' or handed in pulls):
            return None                                          # the pull may have lost tunnel bytes
        return (set(others) - pulls) | {rq['slot']}

    def fall_back(lost=None):
        """S-09.074; lost: the numbered request session to be sent again (S-09.076), or None."""
        nonlocal mode, trial, kept, fb
        r = trial['rung'] if trial else kept
        if r and (trial is not None or r != 1):  # a kept rung 1 is not marked failed
            failed_rungs.add(r)
        if lost is not None and (len(lost) < 12 or lost[0] in m.UNNUMBERED):
            lost = None
        if lost is None and fb is not None:
            lost = fb['lost']
        if lost is not None:                     # S-09.022: its slot stays held for it
            w0, a0, a1 = struct.unpack_from('<III', lost)
            held[w0 >> 8 & 0xFF] = {'cmd': w0 & 0xFF, 'slot': w0 >> 8 & 0xFF, 'len': w0 >> 16, 'arg0': a0,
                                    'arg1': a1, 'got': 0, 'mode': m.QUAD, 'hdr': lost[:12], 'sess': lost}
        mode, trial, kept = m.PLAIN, None, 0
        fb = {'echoes': 0, 'answered': False, 'lost': lost}

    for i, s in enumerate(sessions):
        where = 'session %d' % i

        def fail(msg):
            fails.append('%s: %s' % (where, msg))
        lens = {len(v) for k, v in s.items() if k in ('mosi', 'miso')}
        if len(lens) != 1:
            fail('mosi and miso of different lengths')
            continue
        length = lens.pop()
        if length % 4:
            fail('%d bytes, not whole words' % length)
        if released_ok:
            fail('a session after a release reply of 0')
            continue
        if absent:
            fail('a session after the slave became absent')
            continue
        if late_at is not None:
            fail('a session after session %d, whose end ran out (S-09.013)' % late_at)
            continue
        if s.get('late'):                                        # S-09.013: a failed, possibly short session
            b = s.get('mosi') or b''
            if fb is not None and not fb['answered']:            # S-09.074: an echo not answered
                if expect is None:
                    fb['echoes'] += 1
                elif fb['echoes'] >= m.ECHO_TRIES:
                    absent = True
                expect = None
                continue
            if mode != m.PLAIN or (expect is not None and expect[1].get('mode', m.PLAIN) != m.PLAIN):
                if expect is None:
                    lost = b                                     # the request session itself
                else:
                    lost = expect[1]['sess']                     # the request whose reply broke
                    held.pop(expect[1]['slot'], None)
                expect = None
                fall_back(lost)
                continue
            if prec is not None and not prec['echoed']:          # S-09.013 rule 4: an echo not answered
                if prec['tries'] >= m.ECHO_TRIES:
                    absent = True
                expect = None
                continue
            stake = plain_stake(expect, s) if prec is None else None
            if stake is not None:                                # S-09.013 rule 4: its slot held
                if expect is None and len(b) >= 2:                   # until the drain
                    w0, a0, a1 = struct.unpack_from('<III', bytes(b[:12]) + bytes(max(0, 12 - len(b))))
                    held[b[1]] = {'cmd': b[0], 'slot': b[1], 'len': w0 >> 16, 'arg0': a0, 'arg1': a1,
                                  'got': 0, 'mode': mode, 'hdr': b[:12], 'sess': b}
                prec = {'echoed': False, 'tries': 0, 'polls': 0, 'stale': stake}
                expect = None
                continue
            late_at = i
            expect = None
            continue
        rung = s.get('rung')
        if expect is not None:                                   # a reply session
            kind, rq = expect
            expect = None
            b = s.get('miso')
            if b is None:
                fail('reply session without miso')
                continue
            if rung not in (None, 0):
                fail('reply session on rung %d (S-09.070)' % rung)
            checked = rq.get('mode', m.PLAIN) != m.PLAIN
            if checked:                                          # S-09.072: the reply's check word last
                if length < 8 or b[-4:] != struct.pack('<I', m.check_word(b[:-4])):
                    held.pop(rq['slot'], None)                   # a failed exchange (S-09.076)
                    fall_back(rq['sess'])
                    continue
                b = b[:-4]
                length -= 4
            if kind == 'imm':
                imm_len = m.STATUS_LEN if rq['cmd'] == m.LINK_STATUS else 8      # S-09.079
                if length != imm_len or struct.unpack_from('<I', b)[0] != rq['cmd'] | rq['slot'] << 8:
                    del held[rq['slot']]
                    if rq['cmd'] == m.LINK_STATUS and mode == m.PLAIN:
                        continue                                 # S-09.079: no fault, not sent again
                    if fb is not None and rq['cmd'] == m.ECHO and not fb['answered']:
                        if fb['echoes'] >= m.ECHO_TRIES:
                            absent = True                        # S-09.074, S-09.013
                        continue
                    if prec is not None and rq['cmd'] == m.ECHO and not prec['echoed']:
                        if prec['tries'] >= m.ECHO_TRIES:
                            absent = True                        # S-09.013 rule 4
                        continue
                    if checked:                                  # S-09.074: the reply did not come
                        fall_back(rq['sess'])
                        continue
                    fail('immediate reply to %02x: %s' % (rq['cmd'], b.hex()))
                    continue
                v = struct.unpack_from('<I', b, 4)[0]
                del held[rq['slot']]
                if prec is not None and rq['cmd'] == m.ECHO and not prec['echoed']:
                    if v == (~rq['arg0']) & 0xFFFFFFFF:
                        prec['echoed'] = True                    # S-09.013 rule 4: the drain next
                    elif prec['tries'] >= m.ECHO_TRIES:
                        absent = True
                    continue
                if fb is not None and rq['cmd'] == m.ECHO and not fb['answered']:
                    if v == (~rq['arg0']) & 0xFFFFFFFF:
                        fb['answered'] = True
                        unsettled = True                         # S-09.074: until a 0x19 is answered
                    elif fb['echoes'] >= m.ECHO_TRIES:
                        absent = True
                    continue
                if rq['cmd'] == 0x01 and v != 0xFEDEBEDA:
                    absent = True
                if rq['cmd'] == 0x0A and n_req <= 3 and v != (~rq['arg0']) & 0xFFFFFFFF:
                    absent = True
                if rq['cmd'] == 0x07:
                    released_ok = v == 0
                    absent = v not in (0, 1)
                if rq['cmd'] == m.LINK_TEST and trial is not None and \
                        v != m.link_test_reply_value(rq['hdr'], rq['payload']):
                    trial['wrong'] = True                        # S-09.073: the test failed
                if rq['cmd'] == m.LINK_MODE:
                    unsettled = False
                    want, how = rq['arg0'], rq['arg1']
                    if want == m.PLAIN and v == m.PLAIN:
                        if trial is not None and trial['rung']:
                            failed_rungs.add(trial['rung'])
                        elif trial is None and kept and not used:
                            failed_rungs.add(kept)               # S-09.075: left at its gate
                        mode, trial, kept = m.PLAIN, None, 0
                    elif want in (m.CHECKED, m.QUAD) and how != m.KEEP and v == want:
                        mode = want
                        trial = {'mode': want, 'rung': None, 'lengths': set(), 'patterns': set(), 'wrong': False}
                    elif want in (m.CHECKED, m.QUAD) and how == m.KEEP and v == want and trial is not None:
                        if trial['wrong']:
                            fail('a mode kept after a test whose value was wrong (S-09.073)')
                        missing = sorted(m.TEST_LENGTHS - trial['lengths'])
                        pats = sorted(m.TEST_PATTERNS - trial['patterns'])
                        if missing or pats:
                            fail('a mode kept without the tests of S-09.073: lengths %s, patterns %s missing'
                                 % (', '.join('0x%x' % x for x in missing) or '-', ', '.join(pats) or '-'))
                        kept, trial = trial['rung'] or 0, None
                        used = False
                        gate_tests = 0
            elif kind == 'pull':
                hdr, w = struct.unpack_from('<II', b) if length >= 8 else (None, None)
                n = (w or 0) & 0xFFFF
                if hdr != m.PULL | rq['slot'] << 8 or n > rq['len'] or length != 8 + m.pad4(n):
                    if checked:                                  # S-09.076: sent again, its number
                        del held[rq['slot']]
                        fall_back(rq['sess'])
                        continue
                    fail('pull reply %s for capacity %d' % (b[:8].hex(), rq['len']))
                    continue
                if n:
                    del held[rq['slot']]
            else:                                                # a poll's reply
                del held[rq['slot']]
                lost = rq['sess']
                if b[:4] == m.POLL_NONE:
                    if length != 4:
                        if checked:
                            fall_back(lost)
                            continue
                        fail('"none" poll reply of %d bytes' % length)
                    for r in held.values():                      # S-09.076: never taken
                        if r['cmd'] not in m.IMMEDIATE | {m.PULL, m.POLL} and not r['got']:
                            r['nothing'] = True
                    continue
                hdr = struct.unpack_from('<I', b)[0]
                slot = hdr >> 8 & 0xFF
                tgt = held.get(slot)
                if hdr >> 16 or tgt is None or tgt['cmd'] != hdr & 0xFF:
                    if checked:                                  # S-09.074: the reply did not come
                        fall_back(lost)
                        continue
                    fail('poll reply %08x for no outstanding request in that slot' % hdr)
                    continue
                c = tgt['cmd']
                if c == m.PULL:
                    n = struct.unpack_from('<I', b, 4)[0] & 0xFFFF if length >= 8 else 0
                    if not 1 <= n <= tgt['len'] or length != 8 + m.pad4(n):
                        fail('parked pull reply with n=%d in %d bytes' % (n, length))
                    del held[slot]
                elif c == m.READ:
                    p = _piece(b)
                    if isinstance(p, str):
                        fail(p)
                        continue
                    r = p[0]
                    if r == tgt['len'] and tgt['got'] + r < tgt['arg1']:
                        tgt['got'] += r                          # S-09.035 step 3: stays outstanding
                    else:
                        del held[slot]
                else:
                    want = m.deferred_reply_len(c, tgt['arg1'])
                    if length != want:
                        if checked:
                            fall_back(lost)
                            continue
                        fail('deferred reply to %02x of %d bytes, expected %d' % (c, length, want))
                    if prec is None or slot not in prec['stale']:
                        del held[slot]                           # S-09.013 rule 4: drained ones stay held
            continue
        b = s.get('mosi')                                        # a request session
        if b is None or length < 12:
            fail('request session without a 12-byte header')
            continue
        w0, a0, a1 = struct.unpack_from('<III', b)
        rq = {'cmd': w0 & 0xFF, 'slot': w0 >> 8 & 0xFF, 'len': w0 >> 16, 'arg0': a0, 'arg1': a1, 'got': 0,
              'mode': mode, 'hdr': b[:12], 'sess': b}
        c = rq['cmd']
        if rung is not None and RUNG_MODE.get(rung) != mode:
            fail('request %02x on rung %s in mode %d (S-09.075)' % (c, rung, mode))
        if trial is not None and rung:
            if trial['rung'] is None:
                trial['rung'] = rung
                if rung in failed_rungs:
                    fail('rung %d tried again after it failed (S-09.075)' % rung)
            elif trial['rung'] != rung:
                fail('a trial on rungs %d and %d' % (trial['rung'], rung))
        resend = False
        if prec is not None:                                     # S-09.013 rule 4
            if not prec['echoed']:
                if c != m.ECHO:
                    fail('request %02x after a failed plain one before an echo was answered '
                         '(S-09.013 rule 4)' % c)
                prec['tries'] += 1
                if prec['tries'] > m.ECHO_TRIES:
                    fail('more than %d echoes after a failed plain request (S-09.013 rule 4)' % m.ECHO_TRIES)
            elif c == m.POLL:
                prec['polls'] += 1
                if prec['polls'] > m.PLAIN_DRAIN:
                    fail('more than %d polls in a drain (S-09.013 rule 4)' % m.PLAIN_DRAIN)
            else:
                for k in prec['stale']:                          # the failed request's slots, freed
                    held.pop(k, None)
                prec = None
        lost = fb['lost'] if fb is not None else None
        if fb is not None:                                       # S-09.074, S-09.076
            if not fb['answered']:
                if c != m.ECHO:
                    fail('request %02x after a fall-back before an echo was answered (S-09.074)' % c)
                elif a0 != m.ECHO_RECOVER or a1 or rq['len']:
                    fail('an echo in a fall-back that is not the recovery echo (S-09.074)')
                fb['echoes'] += 1
                if fb['echoes'] > m.ECHO_TRIES:
                    fail('more than %d echoes in a fall-back (S-09.074)' % m.ECHO_TRIES)
            elif c in m.UNNUMBERED:
                pass                                             # the way back to a checked rung
            elif lost is not None and mode != m.PLAIN and b[:len(lost)] == lost:
                resend = True                                    # the lost request, its number again
                fb = None
            elif lost is not None:
                fail('request %02x after a fall-back before the lost %02x was sent again on a checked '
                     'rung (S-09.076)' % (c, lost[0]))
                fb = None
            else:
                fb = None
        if mode != m.PLAIN and c not in m.UNNUMBERED and len(b) >= 12 + m.CHECK_LEN:
            seq = m.seq_of(b)                                    # S-09.076
            held_rq = held.get(rq['slot'])
            if seq in sent:
                if sent[seq] != b:
                    fail('number %d carried by another request (S-09.076)' % seq)
                elif not resend and not (held_rq is not None and held_rq.get('nothing') and
                                         held_rq['sess'] == b):
                    fail('request %02x number %d sent again with nothing lost (S-09.076)' % (c, seq))
                resend = True
                repeats[seq] = repeats.get(seq, 0) + 1
                if repeats[seq] > m.REPEATS:
                    fail('request number %d sent again more than %d times (S-09.076)' % (seq, m.REPEATS))
                if held_rq is not None:
                    held_rq['nothing'] = False
            elif seq != last_seq + 1:
                fail('request %02x number %d after %d: each new request the next (S-09.076)'
                     % (c, seq, last_seq))
            last_seq = max(last_seq, seq)
            sent.setdefault(seq, b)
        if c not in m.UNNUMBERED:
            used = True
            if mode == m.PLAIN and unsettled:
                fail('request %02x in the plain mode before a 0x19 settled the slave (S-09.074)' % c)
        if c == 0x07 and mode != m.PLAIN:
            fail('the release outside the plain mode (S-09.078)')
        free = [k for k in range(4) if k not in held]
        if not free:
            fail('request %02x sent with all four slots held' % c)
            continue
        if rq['slot'] != free[0] and not resend:
            fail('request %02x in slot %d, lowest free is %d' % (c, rq['slot'], free[0]))
        if len(free) == 1 and c not in m.IMMEDIATE | m.PUSH | {m.POLL} and not resend:
            fail('request %02x took the last free slot, which is kept for a poll (S-09.022)' % c)
        if c not in m.DEFINED or c in NEVER_SENT:
            fail('the master sent command %02x' % c)
            continue
        n = m.slave_payload_len(b[:12])
        if mode != m.PLAIN:
            if not m.checked_form_ok(b):
                fail('request %02x outside the plain mode: %d bytes, expected %d with its check (S-09.072)'
                     % (c, length, 12 + m.pad4(n) + m.CHECK_LEN))
            elif any(b[12 + n:12 + m.pad4(n)]):
                fail('request %02x: nonzero pad bytes' % c)
        elif length != 12 + m.wire_payload_len(n):
            fail('request %02x of %d bytes, expected %d (S-09.021, the master)' % (c, length,
                                                                                    12 + m.wire_payload_len(n)))
        elif any(b[12 + n:]):
            fail('request %02x: nonzero pad bytes' % c)
        if c == m.WRITE:
            if not 1 <= a1 <= m.WRITE_MAX or (a1 > m.PLAIN_WRITE_MAX and mode == m.PLAIN and not resend):
                fail('file write of %d bytes in mode %d (S-09.023)' % (a1, mode))
        if c == m.LINK_TEST and not 4 <= a1 <= m.TEST_MAX:
            fail('link test of %d bytes (S-09.071)' % a1)
        if trial is not None:
            if c not in (m.LINK_MODE, m.LINK_TEST):
                fail('request %02x during a trial (S-09.073)' % c)
            if trial['wrong'] and not (c == m.LINK_MODE and a0 == m.PLAIN):
                fail('request %02x after a wrong test value; 0x19 with mode 0 comes next (S-09.073)' % c)
            if c == m.LINK_TEST:
                rq['payload'] = b[12:12 + n]
                trial['lengths'].add(a1)
                trial['patterns'].add(m.test_pattern(b[12:12 + a1]))
        elif c == m.LINK_TEST:
            rq['payload'] = b[12:12 + n]
            if kept and not used and not link_test_run:          # S-09.075's gate, nothing else
                gate_tests += 1
                if kept == 1 or a1 != m.TEST_MAX or gate_tests > m.GATE_TIMES:
                    fail('a link test of %d bytes after the keep on rung %d outside the gate '
                         '(S-09.073, S-09.075)' % (a1, kept))
        if c == 0x14 and (a1 or rq['len']):
            fail('erase with arg1 %08x len %d (S-09.040)' % (a1, rq['len']))
        start = [(0x01, 0), (0x0A, 0x12345678), (m.PULL, None)]
        if n_req < 3:
            ec, ea = start[n_req]
            if c != ec or (ea is not None and a0 != ea) or (c == 0x01 and (a0 or a1 or rq['len'])):
                fail('start-up request %d is %02x arg0 %08x, expected %02x' % (n_req + 1, c, a0, ec))
        n_req += 1
        pulls = [r for r in held.values() if r['cmd'] == m.PULL]
        if c == m.PULL:
            if pulls and not resend:
                fail('a second pull while one is outstanding')
            if not 1 <= rq['len'] <= 0x1000:
                fail('pull capacity %d' % rq['len'])
            pull_started = True
        elif pull_started and not pulls and not room_gaps and fb is None:
            fail('request %02x with no pull outstanding (S-09.045)' % c)
        if c in m.PUSH:
            if not 1 <= rq['len'] <= 0x1000:
                fail('push of %d bytes' % rq['len'])
            continue                                             # no reply; its slot is free again
        held[rq['slot']] = rq
        if c in m.IMMEDIATE:
            expect = ('imm', rq)
        elif c == m.PULL:
            expect = ('pull', rq)
        elif c == m.POLL:
            if a0 or a1 or rq['len']:
                fail('poll with nonzero fields')
            expect = ('poll', rq)
    if expect is not None and not absent_run and late_at is None:
        fails.append('the run ends before the reply to %02x' % expect[1]['cmd'])
    if fb is not None and not fb['answered'] and not absent_run:
        fails.append('the run ends in a fall-back with no echo answered (S-09.074)')
    if fb is not None and fb['lost'] is not None and not absent_run:
        fails.append('the run ends before the lost request was sent again (S-09.076)')
    return fails


def check_session_ends(items):
    """S-09.011, S-09.013: how the slave ended each session. items: [{'name'?, 'expect', 'moved', 'rose',
    'fell_again'}]."""
    fails = []
    for i, it in enumerate(items):
        name = it.get('name', 'item %d' % i)
        e, mv, r, f = it['expect'], it['moved'], bool(it['rose']), bool(it['fell_again'])
        if m.session_end_ok(e, mv, r, f):
            continue
        if not 0 <= mv <= e or (f and not r):
            fails.append('%s: bad record (moved %r of %r, rose %r, fell_again %r)' % (name, mv, e, r, f))
        elif f:
            fails.append('%s: the ready line fell again while PB15 read low, after %d of %d bytes (S-09.013)'
                         % (name, mv, e))
        else:
            fails.append('%s: complete session (%d bytes) whose ready line never rose (S-09.011)' % (name, e))
    return fails


# ---- command line ----

def _hexify(obj):
    """The capture's hex strings as bytes, for the fields that carry bytes."""
    if isinstance(obj, dict):
        return {k: (_b(v) if k in ('hex', 'mosi', 'miso') and isinstance(v, str) else
                    [_b(x) for x in v] if k in ('pieces',) else _hexify(v)) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_hexify(x) for x in obj]
    return obj


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('check', choices=['stream', 'request', 'reply', 'file_read', 'record', 'master',
                                      'session_end', 'pad_trail'])
    ap.add_argument('capture')
    ap.add_argument('--side', choices=['master', 'slave'], default='master')
    ap.add_argument('--vector', choices=['final', 'badsig', 'truncated'])
    ap.add_argument('--absent', action='store_true', help='master: the run may end awaiting a reply')
    ap.add_argument('--room-gaps', action='store_true',
                    help='master: the receive room ran out, so a request may go with no pull outstanding')
    a = ap.parse_args(argv)
    try:
        with open(a.capture) as f:
            cap = json.load(f)
        if a.check == 'stream':
            if not a.vector:
                ap.error('stream needs --vector')
            fails = check_stream(a.vector, [_b(x) for x in cap['sessions']])
        elif a.check == 'master':
            fails = check_master(_hexify(cap['sessions']), a.absent, a.room_gaps)
        else:
            items = _hexify(cap['items'])
            fails = {'request': lambda: check_requests(items, a.side), 'reply': lambda: check_replies(items),
                     'file_read': lambda: check_file_reads(items), 'record': lambda: check_records(items),
                     'session_end': lambda: check_session_ends(items),
                     'pad_trail': lambda: check_pad_trails(items)}[a.check]()
    except (OSError, ValueError, KeyError, TypeError) as e:
        print('bad capture: %s' % e)
        return 2
    for x in fails:
        print('FAIL', x)
    print('PASS' if not fails else '%d failure(s)' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
