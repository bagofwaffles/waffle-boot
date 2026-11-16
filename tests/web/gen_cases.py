#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""What tools/wbrecover.py does in a set of scenarios, as JSON on stdout, for
tests/web/test_core.mjs to hold docs/web/core.js against.

    gen_cases.py [--list]

Each case is a unit (units.py's spec) and steps: a command of the Python tool, run here against
that unit with every prompt answered as asked, or a change to the unit. For every command the
output holds HELLO's and INFO's bytes from before it, describe()'s lines for that INFO, what the
tool printed and asked, its exit status, and the unit's flash afterwards. The Node test makes
the same unit over unit_bridge.py, plans and runs the same command with core.js, and compares.

Also in the output: the tool's tables and the layout constants core.js copies, and for every
image used its bytes and the tool's check of it. Python 3, standard library only; the shim cases
need a C compiler and are left out without one.
"""
import base64
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import units  # noqa: E402

T, w = units.T, units.w
ACTS = {v: k for k, v in w.ACT_NAMES.items()}

A = {'make': {'seed': 1}}
B = {'make': {'seed': 2, 'version': [0, 9, 0, 0]}}
A2 = {'make': {'seed': 2}}                          # A's version, other bytes
NEW = {'make': {'seed': 7, 'size': 3000}}
NEW2 = {'make': {'seed': 8, 'size': 2000}}
SAMEV = {'make': {'seed': 5}}
OTHER = {'make': {'seed': 7, 'front': 2, 'size': 3000}}
FOREIGN = {'foreign': 1}
BAD_CRC = {'make': {'front_crc_ok': False}}
BAD_PIN = {'make': {'pin_ok': False}}
NO_ENTRY = {'make': {'entry': 0x08031000}}
TOO_LARGE = {'of': 0x77001, 'make': {}}
OVER_SLOT = {'of': 0x78001, 'make': {}}
OVER_STORE = {'of': 0x76010, 'make': {'seed': 9}}
STRADDLE = {'hex': T.STRADDLE.hex()}
FILLED = {'hex': T.FILLED.hex()}
NOT_A_STREAM = {'hex': (b'not a stream at all ' * 4).hex()}
EMPTY = {'hex': ''}
CHARGED = bytes([2, 40, 25, 0, 0, 0x0F, 0xA0]).hex()
ABSENT = bytes(7).hex()


def write(img, mode='plain', force=False):
    return {'cmd': 'write', 'image': img, 'mode': mode, 'force': force}


def select(img, force=False):
    return {'cmd': 'select', 'image': img, 'force': force}


def act(name, force=False):
    return {'cmd': name, 'force': force}


def model(front, slots=(A, B), **kw):
    return dict(kind='model', front=front, slots=list(slots), **kw)


def shim(slots=(A, B), good=(True, True), **kw):
    return dict(kind='shim', slots=list(slots), good=list(good), **kw)


FRONTS = (5, 6, 9, 11, 14)
SLOT = units.SLOT


def model_cases(f):
    """The Model at front version f: (name, unit, steps)."""
    held = dict(spoiled=True, held=True, bad=[0])
    return [
        ('info', model(f), [{'cmd': 'info'}]),
        ('info-slot2-bad', model(f, (A, None), running=1, sel=[1, 1]), [{'cmd': 'info'}]),
        ('info-not-established', model(f, established=False), [{'cmd': 'info'}, write(NEW)]),
        ('info-battery', model(f, batt=CHARGED, why=3), [{'cmd': 'info'}]),
        ('write-empty', model(f, (A, None)), [write(NEW)]),
        ('write-good', model(f), [write(NEW), write(NEW2), act('restart')]),
        ('write-from-slot2', model(f, (B, A), running=1, sel=[1, 1]), [write(NEW)]),
        ('write-same-version', model(f, (A, A2)), [write(SAMEV)]),
        ('write-selector-other', model(f, sel=[1, 1]), [write(NEW), write(NEW, force=True)]),
        ('write-selector-other-bad', model(f, (A, None), sel=[1, 1]), [write(NEW, force=True)]),
        ('write-unsettled', model(f, sel=[0, 0]), [write(NEW), write(NEW, force=True)]),
        ('write-selected-bad', model(f, (A, None), running=1, sel=[1, 1]), [write(NEW)]),
        ('write-other-front', model(f), [write(OTHER), write(OTHER, force=True)]),
        ('write-new-front', model(f), [write(OTHER, 'new-front'), select(OTHER),
                                       select(OTHER, force=True)]),
        ('write-new-front-slot1', model(f, (B, A), running=1, sel=[1, 1]),
         [write(OTHER, 'new-front')]),
        ('write-new-front-same', model(f), [write(NEW, 'new-front')]),
        ('write-new-front-foreign', model(f), [write(FOREIGN, 'new-front')]),
        ('write-new-front-running-bad', model(f, set=dict(bad=[0])),
         [write(OTHER, 'new-front'), write(OTHER, 'new-front', force=True)]),
        ('write-kept-image', model(f), [write(A)]),
        ('write-target-image', model(f), [write(B)]),
        ('write-identical-slots', model(f, (A, A)), [write(NEW)]),
        ('write-identical-own', model(f, (A, A)), [write(A)]),
        ('running-other-good', model(f), [write(NEW, 'running')]),
        ('running-own-image', model(f), [write(A, 'running')]),
        ('running-other-foreign', model(f, (A, FOREIGN)), [write(NEW, 'running')]),
        ('running-other-bad', model(f, (A, None)),
         [write(NEW, 'running'), write(NEW, 'running', force=True)]),
        ('running-in-place', model(f, (A, None), batt=CHARGED, set=dict(other_failed=True)),
         [write(NEW, 'running')]),
        ('running-in-place-no-battery', model(f, (A, None), batt=ABSENT, set=dict(other_failed=True)),
         [write(NEW, 'running')]),
        ('failed-then-in-place', model(f, (A, None), set=dict(fail_with=9)),
         [write(NEW), {'set': dict(fail_with=0, other_failed=True)}, {'cmd': 'info'},
          write(NEW, 'running')]),
        ('failed-readback', model(f, set=dict(fail_with=10)), [write(NEW), {'cmd': 'info'}]),
        ('failed-selector', model(f, set=dict(fail_with=11)), [write(NEW)]),
        ('failed-trial', model(f, set=dict(fail_with=10)), [write(OTHER, 'new-front'), {'cmd': 'info'}]),
        ('selector-not-written', model(f, set=dict(bad_selector=True)), [write(NEW)]),
        ('image-bad-front-crc', model(f), [write(BAD_CRC), write(BAD_CRC, force=True)]),
        ('image-bad-pin', model(f), [write(BAD_PIN), select(BAD_PIN)]),
        ('image-too-large', model(f), [write(TOO_LARGE), write(TOO_LARGE, force=True)]),
        ('image-not-a-stream', model(f), [write(NOT_A_STREAM), select(NOT_A_STREAM)]),
        ('image-foreign', model(f), [write(FOREIGN), write(FOREIGN, force=True)]),
        ('image-over-store', model(f, (A, None)), [write(OVER_STORE)]),
        ('image-over-store-slot1', model(f, (None, A), running=1, sel=[1, 1]), [write(OVER_STORE)]),
        ('force-not-needed', model(f), [write(NEW, force=True)]),
        ('end-16', model(f, set=dict(end_says=16)), [write(NEW)]),
        ('begin-15', model(f, set=dict(begin_st=15)), [write(NEW, 'running')]),
        ('begin-other-way', model(f, set=dict(begin_says=[w.WAY_IN_PLACE, 0])), [write(NEW)]),
        ('end-lost', model(f, set=dict(lose={str(w.END): 1})), [write(NEW)]),
        ('end-lost-after-write', model(f),
         [write(NEW), {'set': dict(sel=[0, 1], lose={str(w.END): 1}, end_says=16)}, write(NEW2)]),
        ('countdown-stopped', model(f, set=dict(drop_at=5)), [write(NEW), {'cmd': 'info'}]),
        ('end-lost-refused', model(f, set=dict(lose={str(w.END): 1}, end_says=16)), [write(NEW)]),
        ('end-lost-after-failed', model(f, set=dict(fail_with=9)),
         [write(NEW), {'set': dict(fail_with=0, lose={str(w.END): 1}, end_says=16)}, write(NEW2)]),
        ('begin-trial-way', model(f, set=dict(begin_says=[w.WAY_TRIAL, 1])), [write(NEW)]),
        ('try-reply-lost', model(f, set=dict(lose={str(w.ACT): 1})), [act('try')]),
        ('select-begin-refused', model(f, (A, OTHER), set=dict(tried=[1], started=[1], begin_st=19)),
         [select(OTHER)]),
        ('select-begin-other', model(f, (A, OTHER), set=dict(tried=[1], started=[1],
                                                           begin_says=[w.WAY_SELECT, 0])),
         [select(OTHER)]),
        ('try', model(f), [act('try'), {'cmd': 'info'}, act('put-back'), act('try', True)]),
        ('try-force-not-needed', model(f), [act('try', True)]),
        ('try-slot1', model(f, (B, A), running=1, sel=[1, 1]), [act('try')]),
        ('try-stopped', model(f, set=dict(drop_at=9)), [act('try'), {'cmd': 'info'}]),
        ('switch', model(f), [act('switch'), act('try'), act('restart')]),
        ('switch-foreign', model(f, (A, FOREIGN)), [act('try'), act('switch'), act('switch', True)]),
        ('switch-other-front', model(f, (A, OTHER)), [act('switch'), act('switch', True)]),
        ('switch-bad', model(f, (A, NEW), set=dict(bad=[1])),
         [act('try'), act('switch'), act('switch', True)]),
        ('switch-empty', model(f, (A, None)), [act('switch'), act('switch', True), act('try', True)]),
        ('acts-unsettled', model(f, sel=[0, 0]), [act('try'), act('switch'), act('try', True)]),
        ('acts-not-established', model(f, established=False), [act('try'), act('keep')]),
        ('keep', model(f, set=dict(tried=[1], started=[1])), [{'cmd': 'info'}, act('keep')]),
        ('keep-not-started', model(f, set=dict(tried=[1])),
         [{'cmd': 'info'}, act('keep'), act('keep', True)]),
        ('keep-nothing-tried', model(f), [act('keep'), act('keep', True), act('put-back')]),
        ('keep-in-trial-start', model(f, (B, OTHER), running=1, sel=[0, 1], set=dict(tried=[1])),
         [{'cmd': 'info'}, act('keep')]),
        ('keep-check-failed', model(f, (B, OTHER), running=1, sel=[0, 1], why=2, set=dict(tried=[1])),
         [act('keep')]),
        ('put-back', model(f, set=dict(tried=[1])), [act('put-back')]),
        ('put-back-pending', model(f, sel=[0xFF, 0xFF], set=dict(tried=[1])),
         [{'cmd': 'info'}, act('put-back')]),
        ('select', model(f, (A, OTHER), set=dict(tried=[1], started=[1])), [select(OTHER)]),
        ('select-not-started', model(f, (A, OTHER), set=dict(tried=[1])),
         [select(OTHER), select(OTHER, force=True)]),
        ('select-nothing-tried', model(f, (A, OTHER)), [select(OTHER)]),
        ('select-other-file', model(f, (A, OTHER), set=dict(tried=[1], started=[1])), [select(NEW)]),
        ('select-in-trial-start', model(f, (B, OTHER), running=1, sel=[0, 1], set=dict(tried=[1])),
         [select(OTHER)]),
        ('select-trusted-tried', model(f, (B, OTHER), running=1, sel=[1, 1], set=dict(tried=[1])),
         [{'cmd': 'info'}, select(OTHER)]),
        ('select-force-not-needed', model(f, (A, OTHER), set=dict(tried=[1], started=[1])),
         [select(OTHER, force=True)]),
        ('restart', model(f), [act('restart')]),
        ('restart-force-not-needed', model(f), [act('restart', True)]),
        ('restart-held', model(f, set=held), [{'cmd': 'info'}, act('restart'), act('restart', True)]),
        ('restart-refused', model(f, set=dict(restart_says=0x44)), [act('restart')]),
        ('held-write', model(f, set=dict(unsettled=True, held=True)), [{'cmd': 'info'}, write(NEW)]),
        ('held-try', model(f, set=held), [act('try'), act('try', True)]),
        ('moved', model(f, sel=[1, 1], set=dict(moved=True)), [{'cmd': 'info'}, write(NEW)]),
    ]


def shim_cases():
    """The front's own code, at its own version: (name, unit, steps)."""
    b_only = [SLOT[1], 0x100000]
    damage = [{'set': dict(fail=b_only)}, write({'make': {'seed': 6}}),
              {'set': dict(fail=[SLOT[0] + 0x1000, SLOT[1]])},
              write({'make': {'seed': 5, 'size': 6000}}, 'running'), {'set': dict(fail=[0, 0])}]
    started = lambda run: {'set': dict(flash=[[0xD000, bytes([run, 1]).hex()],   # noqa: E731
                                              [SLOT[1 - run] + 0x77000, 'fc']], start=[run, 1])}
    return [
        ('info', shim(), [{'cmd': 'info'}]),
        ('info-why-count', shim(why=3, batt=CHARGED), [{'cmd': 'info'}]),
        ('write-good-good', shim(), [write(NEW), {'cmd': 'info'}, write(NEW2), act('restart')]),
        ('write-good-bad', shim(good=(True, False)), [write(SAMEV)]),
        ('write-good-empty', shim((A, None)), [write(NEW)]),
        ('write-bad-good', shim(good=(False, True)), [write(SAMEV)]),
        ('write-bad-bad', shim(good=(False, False)), [write(SAMEV)]),
        ('write-from-slot2', shim(running=1), [write(NEW)]),
        ('write-over-store', shim((A, None)), [write(OVER_STORE)]),
        ('running-good-good', shim(), [write(NEW, 'running')]),
        ('running-bad-good', shim(good=(False, True)), [write(SAMEV, 'running')]),
        ('running-other-foreign', shim((A, FOREIGN)), [write(NEW, 'running')]),
        ('in-place-gate', shim(good=(True, False), batt=CHARGED),
         [{'cmd': 'info'}, write(NEW, 'running'), {'set': dict(fail=b_only)},
          write({'make': {'seed': 6}}), {'set': dict(fail=[0, 0])}, {'cmd': 'info'},
          write(NEW, 'running')]),
        ('in-place-no-battery', shim(good=(False, False), batt=ABSENT),
         [write(SAMEV, 'running'), {'set': dict(fail=b_only)}, write({'make': {'seed': 6}}),
          {'set': dict(fail=[0, 0])}, write(SAMEV, 'running')]),
        ('in-place-forced', shim(good=(True, False), batt=CHARGED), [write(NEW, 'running', True)]),
        ('failed-erase', shim(set=dict(fail=b_only)), [write(NEW), {'cmd': 'info'}]),
        ('selector-not-written', shim(set=dict(ignore_selector=1)), [write(NEW)]),
        ('status-14', shim(set=dict(ignore_selector=1)), [write(SAMEV, 'running')]),
        ('retry-after-select-first', shim(),
         [{'set': dict(fail=[SLOT[0] + 0x1000, SLOT[1]])},
          write({'make': {'seed': 5, 'size': 6000}}, 'running'), {'cmd': 'info'},
          {'set': dict(fail=[0, 0])}, write({'make': {'seed': 5, 'size': 6000}}, 'running')]),
        ('damaged-retry-in-place', shim(good=(True, False)),
         damage + [{'cmd': 'info'}, act('restart'),
                   write({'make': {'seed': 5, 'size': 6000}}, 'running'), act('restart')]),
        ('damaged-plain-write', shim(good=(True, False)),
         damage + [act('try'), write({'make': {'seed': 8}}), act('restart')]),
        ('damaged-forced-restart', shim(good=(True, False)), damage + [act('restart', True)]),
        ('identical-slots', shim((A, A)), [{'cmd': 'info'}, write(NEW)]),
        ('identical-own-image', shim(), [write(A)]),
        ('identical-running', shim(), [write(B, 'running')]),
        ('other-front', shim(), [write(OTHER), write(OTHER, force=True)]),
        ('foreign', shim(), [write(FOREIGN), write(FOREIGN, force=True)]),
        ('selector-other', shim(sel=[1, 1]), [write(NEW), write(NEW, force=True)]),
        ('selector-other-bad', shim(good=(True, False), sel=[1, 1]), [write(NEW, force=True)]),
        ('selector-unsettled', shim(sel=[0, 0]), [write(NEW), write(NEW, force=True)]),
        ('trial-then-select', shim(),
         [write(OTHER, 'new-front'), select(OTHER), started(0), {'cmd': 'info'}, select(OTHER)]),
        ('trial-not-started', shim(),
         [write(OTHER, 'new-front'), {'set': dict(flash=[[0xD000, '0001']], start=[0, 1])},
          {'cmd': 'info'}, select(OTHER), select(OTHER, force=True)]),
        ('trial-select-in-own-recovery', shim(running=1),
         [write(OTHER, 'new-front'), {'set': dict(flash=[[0xD000, '0101']], start=[0, 1])},
          {'cmd': 'info'}, select(OTHER)]),
        ('trial-running-bad', shim(good=(False, True)),
         [write(OTHER, 'new-front'), write(OTHER, 'new-front', force=True)]),
        ('try-put-back-switch', shim(),
         [act('try'), {'cmd': 'info'}, act('keep'), act('put-back'), act('switch'), act('try')]),
        ('keep-after-trial-start', shim(), [act('try'), started(0), {'cmd': 'info'}, act('keep')]),
        ('switch-bad', shim(good=(True, False)), [act('switch'), act('switch', True)]),
        ('switch-empty', shim((A, None)), [act('switch'), act('switch', True), act('try', True)]),
        ('switch-foreign', shim((A, FOREIGN)), [act('try'), act('switch'), act('switch', True)]),
        ('try-other-front', shim((A, OTHER)), [act('switch'), act('try')]),
        ('countdown-stopped', shim(set=dict(drop_after=5)), [act('try'), {'cmd': 'info'}]),
        ('write-stopped', shim(set=dict(drop_after=6)), [write(NEW), {'cmd': 'info'}]),
        ('second-try-stopped', shim(), [act('try'), {'set': dict(drop_after=4)}, act('try', True)]),
        ('end-lost', shim(set=dict(lost={str(w.END): 1})), [write(NEW)]),
        ('restart', shim(), [act('restart')]),
        ('restart-force-not-needed', shim(), [act('restart', True)]),
    ]


def run_cmd(unit, step, images):
    """One command of the Python tool against unit: what it printed, asked and returned."""
    link = units.Direct(unit)
    events = []

    def out(*a, end='\n'):
        text = ' '.join(str(x) for x in a)
        if text and not text.startswith('\r'):
            events.append(['out', text])

    def ask(prompt):
        events.append(['ask', prompt])
        m = re.match(r'Type "(.*)" to go on: $', prompt)
        return m.group(1) if m else 'yes'

    cmd, force = step['cmd'], step.get('force', False)
    data = units.image(step['image']) if 'image' in step else None
    try:
        if cmd == 'info':
            rc = w.cmd_info(link, out)
        elif cmd in ('write', 'select'):
            probs, front = w.check_image(data)      # as main() checks the file first
            nofront = probs and all(p.startswith('no waffle-boot front') for p in probs)
            if probs and not (cmd == 'write' and force and nofront):
                for line in probs:
                    out('  ' + line)
                out('The file is not a slot image recovery writes; nothing was sent.')
                rc = 1
            elif not probs and not front['pin_ok']:
                rc = w.pin_refusal(out, front)
            elif cmd == 'write':
                rc = w.cmd_write(link, data, out, poll=0, ask=ask, running=step['mode'] == 'running',
                                 new_front=step['mode'] == 'new-front', pins=lambda: None, force=force)
            else:
                rc = w.cmd_select(link, data, out, poll=0, ask=ask, force=force)
        elif cmd == 'restart':
            rc = w.cmd_restart(link, out, ask=ask, force=force)
        else:
            rc = w.cmd_act(link, ACTS[cmd], out, poll=0, ask=ask, force=force)
    except w.Failure as e:
        out('FAILED: %s' % e)
        rc = 1
    sent = [[t, r.hex()] for t, r in link.replies if t in (w.BEGIN, w.END, w.ACT, w.RESTART)]
    return {'events': events, 'rc': rc, 'replies': sent}


def image_key(spec, images):
    """spec's name in the output's image table, where its bytes and the tool's check go."""
    key = json.dumps(spec, sort_keys=True)
    if key not in images:
        data = units.image(spec)
        probs, front = w.check_image(data)
        lines = []
        rc = w.cmd_check(data, lambda *a, **k: lines.append(' '.join(str(x) for x in a)))
        images[key] = {
            'b64': base64.b64encode(data).decode(), 'sha256': hashlib.sha256(data).hexdigest(),
            'crc32': T.zlib.crc32(data) & 0xFFFFFFFF, 'problems': probs, 'check': lines, 'rc': rc,
            'front': None if front is None else {
                'entry': front['entry'], 'front_crc': front['front_crc'],
                'version': list(front['version']), 'pin': front['pin'].hex(),
                'pin_calc': front['pin_calc'].hex(), 'pin_ok': front['pin_ok']}}
    return key


def plain(v):
    """v as JSON carries it: byte strings as hex, tuples as lists."""
    if isinstance(v, (bytes, bytearray)):
        return bytes(v).hex()
    if isinstance(v, dict):
        return {k: plain(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [plain(x) for x in v]
    return v


def run_case(name, spec, steps, images):
    unit = units.make_unit(spec)
    if unit is None:
        return None
    out = []
    try:
        for step in steps:
            if 'set' in step:
                unit.set(step['set'])
                out.append(step)
                continue
            hello = unit.answer(w.HELLO, b'')[0]
            raw = unit.answer(w.INFO, b'')[0]
            info = w.parse_info(raw)
            info['front'] = w.parse_hello(hello)['front_version']
            rec = dict(step, hello=hello.hex(), info=raw.hex(), parsed=plain(info),
                       describe=w.describe(info))
            if 'image' in step:
                rec['image'] = image_key(step['image'], images)
            rec.update(run_cmd(unit, step, images))
            state = unit.state()
            state.pop('requests', None)
            state.pop('phases', None)
            state.pop('batt_asks', None)
            rec['state'] = state
            out.append(rec)
    finally:
        unit.close()
    return {'name': name, 'unit': spec, 'steps': out}


def all_cases():
    for f in FRONTS:
        for name, spec, steps in model_cases(f):
            yield 'model-f%d-%s' % (f, name), spec, steps
    for name, spec, steps in shim_cases():
        yield 'shim-%s' % name, spec, steps


def tables():
    import mkldr
    m = w.check_stream.load_map()
    return {
        'STATUS': {str(k): list(v) for k, v in w.STATUS.items()},
        'IMAGE_RC': {str(k): v for k, v in w.IMAGE_RC.items()},
        'WHY': {str(k): v for k, v in w.WHY.items()},
        'status_text': {str(st): w.status_text(st) for st in list(w.STATUS) + [0x7F]},
        'refusal_text': {str(st): w.refusal_text(st) for st in w.STATUS},
        'constants': {k: getattr(w, k) for k in (
            'VID', 'PID', 'PRODUCT', 'PROTO', 'OLD_PROTO', 'MAX_DATA', 'MAX_PAYLOAD', 'HEAD',
            'FRAME_MAX', 'NO_SLOT', 'SLOT_CAP', 'IMAGE_MAX', 'STORE_MAX', 'HELLO_LEN', 'INFO_V5',
            'INFO_V6', 'INFO_V11', 'BATT_MAX', 'TRIAL_FRONT', 'KEEPS_WRITE', 'ACTS_FRONT',
            'SAME_FRONT', 'COUNTDOWN', 'PHRASE', 'TRIAL_PHRASE', 'SWITCH_PHRASE', 'PHASES',
            'RESTART_TIPS', 'READ_ERROR', 'COUNTDOWN_LINE', 'DEFAULTS_LINE', 'RESTART_RISK',
            'RESET_HINT', 'SAFE_HINT', 'FRONT_HINT', 'OTHER_HINT', 'NOT_STARTED_HINT',
            'SLOT_MENU')},
        'WRITE_RISK': {str(k): v for k, v in w.WRITE_RISK.items()},
        'SELECT_RISK': {str(k): v for k, v in w.SELECT_RISK.items()},
        'TIER_WORDS': {str(k): v for k, v in w.TIER_WORDS.items()},
        'battery': [[b.hex(), list(w.battery(b))] for b in (
            b'', bytes(7), b'\x02\x50', bytes([1, 80, 25, 0, 0, 0x0F, 0x3C]),
            bytes([2, 5, 25, 0, 0, 0x0F, 0x3C]), bytes([2, 50, 25, 0, 0, 0x0D, 0xAC]), b'\x01')],
        'frames': [[t, s, p.hex(), w.frame(t, s, p).hex()] for t, s, p in (
            (w.HELLO, 1, b''), (w.DATA, 255, bytes(range(200))),
            (w.ACT | w.REPLY, 0x1FF, b'\x00\x06\x01\x01'))],
        'stale_frame': T.stale_frame().hex(),
        'layout': {
            'SPACES': [list(v) for v in m['spaces'].values()],
            'ROM_BOOT_RESERVE': list(m['rom_boot_reserve']),
            'RECORD_AND_ROM_STACK': list(m['record_and_rom_stack']),
            'FIRST_STAGE_STACK': [0x11807000, 0x11808000], 'BCODE': m['bcode'],
            'FR_WINDOWS': [list(v) for v in mkldr.FR_WINDOWS], 'FR_SHARED': list(mkldr.FR_SHARED),
            'FR_DESC_ADDR': mkldr.FR_DESC_ADDR, 'FR_DESC_MAGIC': mkldr.FR_DESC_MAGIC,
            'FR_DESC_VERSION': mkldr.FR_DESC_VERSION, 'FR_DESC_HEAD': mkldr.FR_DESC_HEAD,
            'FR_DESC_REGIONS': mkldr.FR_DESC_REGIONS},
    }


def main(argv):
    if '--list' in argv:
        for name, _, steps in all_cases():
            print(name, len(steps))
        return 0
    images, cases = {}, []
    for spec in (A, B, NEW, OTHER, FOREIGN, BAD_CRC, BAD_PIN, NO_ENTRY, TOO_LARGE, OVER_SLOT,
                 OVER_STORE, STRADDLE, FILLED, NOT_A_STREAM, EMPTY):
        image_key(spec, images)
    for name, spec, steps in all_cases():
        try:
            case = run_case(name, spec, steps, images)
        except Exception:
            sys.stderr.write('in case %s:\n' % name)
            raise
        if case is not None:
            cases.append(case)
    json.dump({'shim': units.shim() is not None, 'tables': tables(), 'images': images,
               'cases': cases}, sys.stdout)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
