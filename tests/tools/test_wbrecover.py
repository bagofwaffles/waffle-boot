#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of tools/wbrecover.py: its image checks, and its commands over a
pseudo-terminal against a Python model of recovery's protocol
(src/front/recovery/proto.h), with damaged replies and refusals. When a C compiler
($CC, default cc) builds the front's protocol and writer into a shared
library, the same commands also run against the real C code. Python 3,
standard library only."""
import ctypes
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(TOP, 'tools')
sys.path.insert(0, TOOLS)
import mkldr  # noqa: E402
import wbrecover as w  # noqa: E402

FRONT_ENTRY = 0x08002000
FRONT_INIT = 0x11A04000


def make_image(front_crc_ok=True, entry=FRONT_ENTRY, size=0, seed=1, version=(1, 2, 3, 4), front=1,
               pin_ok=True):
    """A slot image shaped as tools/mkldr.py's merged() makes one: front runs in
    L1 code and L2, the descriptor block, a main run in L2, INIT at the front's,
    FIRST and FINAL at the front's entry. size pads it with zero blocks' worth of
    main bytes in DDR; seed varies main's bytes, front the front's. pin_ok false: the
    descriptor claims another pin than its front's own."""
    fl1 = bytes((front + i) & 0xFF for i in range(32))
    fl2 = bytes((front * 3 + i) & 0xFF for i in range(64))
    main = [(0x08031000, bytes((seed * 5 + i) & 0xFF for i in range(64)))]
    if size:
        main.append((0x88000000, bytes(size)))
    pin = hashlib.sha256()
    fcrc = 0
    for a, b in ((FRONT_INIT, fl1), (FRONT_ENTRY, fl2)):
        pin.update(struct.pack('<II', a, len(b)) + b)
        fcrc = zlib.crc32(b, fcrc)
    if not front_crc_ok:
        fcrc ^= 1
    mcrc = 0
    for _, b in main:
        mcrc = zlib.crc32(b, mcrc)
    dsize = mkldr.FR_DESC_HEAD + 8 * len(main) + 4
    d = struct.pack('<IHHIIII', mkldr.FR_DESC_MAGIC, mkldr.FR_DESC_VERSION, dsize, 0x08031000,
                    len(main), mcrc, fcrc & 0xFFFFFFFF) + struct.pack('<4H', *version) + \
        (pin.digest() if pin_ok else hashlib.sha256(b'another front').digest())
    for a, b in main:
        d += struct.pack('<II', a, len(b))
    d += struct.pack('<I', zlib.crc32(d) & 0xFFFFFFFF)
    body = mkldr.header(0, FRONT_INIT, len(fl1), 0) + fl1
    body += mkldr.header(mkldr.INIT, FRONT_INIT, 0, 0)
    for a, b in sorted([(FRONT_ENTRY, fl2), (mkldr.FR_DESC_ADDR, d)] + main):
        body += mkldr.header(0, a, len(b), 0) + b
    body += mkldr.header(mkldr.FINAL, entry, 0, 0)
    return mkldr.header(mkldr.FIRST | mkldr.IGNORE, entry, 0, len(body)) + body


def image_of(n, **kw):
    """make_image() padded to exactly n bytes."""
    img = make_image(size=n - (len(make_image(size=1, **kw)) - 1), **kw)
    assert len(img) == n
    return img


def with_block(img, header, payload=b''):
    """img with one more block, before its INIT block; FIRST's argument follows."""
    blocks = w.blocks_of(img)
    at = next(b['offset'] for b in blocks if b['flags'] & mkldr.INIT)
    body = img[16:at] + header + payload + img[at:]
    return mkldr.header(mkldr.FIRST | mkldr.IGNORE, blocks[0]['target'], 0, len(body)) + body


FILL = w.check_stream.ldr.FILL
# blocks the unit's scan() refuses where the front is: they must fail the tool's check too
STRADDLE = with_block(make_image(), mkldr.header(0, 0x11A03F00, 0x200, 0), bytes(0x200))
FILLED = with_block(make_image(), mkldr.header(FILL, 0x11A04000, 0x20, 0))


class ImageChecks(unittest.TestCase):
    def test_good(self):
        probs, front = w.check_image(make_image())
        self.assertEqual(probs, [])
        self.assertEqual(front['version'], (1, 2, 3, 4))
        self.assertTrue(front['pin_ok'])

    def test_size(self):
        self.assertIn('S-02.044', w.check_image(b'')[0][0])
        self.assertIn('S-02.044', w.check_image(bytes(w.SLOT_CAP + 1))[0][0])
        # S-02.104: the unit takes up to 0x78000, a waffle-boot image is at most 0x76000
        self.assertIn('S-02.104', w.check_image(make_image(size=w.WB_IMAGE_MAX))[0][0])
        img = make_image(size=w.WB_IMAGE_MAX - (len(make_image(size=1)) - 1))
        self.assertEqual(len(img), w.WB_IMAGE_MAX)
        self.assertEqual(w.check_image(img)[0], [])

    def test_not_a_stream(self):
        probs, _ = w.check_image(b'\x00' * 64)
        self.assertTrue(any('parse' in p for p in probs))

    def test_no_front(self):
        probs, _ = w.check_image(make_image(front_crc_ok=False))
        self.assertTrue(any("front's bytes" in p for p in probs))
        probs, _ = w.check_image(make_image(entry=0x08031000))
        self.assertTrue(any('entry' in p for p in probs))

    def test_scan_rules(self):
        """S-02.084's front as the unit's scan() takes it: a block that runs into a front window
        from outside, or a FILL block in one, leaves no waffle-boot front, though the front's
        own bytes are unchanged."""
        base = w.check_image(make_image())[1]['pin_calc']
        for img, words in ((STRADDLE, 'a data block at 11a03f00+200 meets'),
                           (FILLED, 'a FILL block at 11a04000+20 meets')):
            probs, front = w.check_image(img)
            self.assertTrue(any(words in p for p in probs), probs)
            self.assertEqual(front['pin_calc'], base)
            self.assertEqual(w.cmd_check(img, Lines()), 1)
        # a block in the shared block that is not the descriptor
        img = with_block(make_image(), mkldr.header(0, 0x08030800, 4, 0), bytes(4))
        self.assertTrue(any('shared block' in p for p in w.check_image(img)[0]))

    def test_check_pin_mismatch(self):
        """check refuses what write refuses: a descriptor pin that is not the front's own."""
        out = Lines()
        self.assertEqual(w.cmd_check(make_image(pin_ok=False), out), 1)
        self.assertIn("the file is not a build of this tree's tools", out.text)

    def test_cli_check(self):
        with tempfile.TemporaryDirectory() as d:
            good, bad = os.path.join(d, 'good.ldr'), os.path.join(d, 'bad.ldr')
            with open(good, 'wb') as f:
                f.write(make_image())
            with open(bad, 'wb') as f:
                f.write(b'\x01' * 100)
            r = subprocess.run([sys.executable, '-I', '-B', os.path.join(TOOLS, 'wbrecover.py'),
                                'check', good], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)
            r = subprocess.run([sys.executable, '-I', '-B', os.path.join(TOOLS, 'wbrecover.py'),
                                '--port', '/nonexistent', 'write', bad], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn('nothing was sent', r.stdout)

    def test_image_limit(self):
        """S-02.104: up to 0x76000 bytes the file passes; one byte more, and anything up to
        recovery's own 0x77000, is refused."""
        out = Lines()
        self.assertEqual(w.cmd_check(image_of(w.WB_IMAGE_MAX), out), 0, out.text)
        self.assertIn('OK: %d bytes' % w.WB_IMAGE_MAX, out.text)
        for n in (w.WB_IMAGE_MAX + 1, w.IMAGE_MAX, w.IMAGE_MAX + 1):
            out = Lines()
            self.assertEqual(w.cmd_check(image_of(n), out), 1)
            self.assertIn('the file is %d bytes; a waffle-boot image is at most 483328 (0x76000) '
                          "bytes, since the slot's last two sectors hold its settings and its start "
                          'log (S-02.104, S-02.095)' % n, out.text)

    def test_check_without_termios(self):
        """check needs no serial port, so it runs where termios does not exist."""
        with tempfile.TemporaryDirectory() as d:
            good = os.path.join(d, 'good.ldr')
            with open(good, 'wb') as f:
                f.write(make_image())
            code = ('import sys; sys.modules["termios"] = None; sys.path.insert(0, sys.argv[1]); '
                    'import wbrecover; sys.exit(wbrecover.main(["check", sys.argv[2]]))')
            r = subprocess.run([sys.executable, '-I', '-B', '-c', code, TOOLS, good],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('OK:', r.stdout)


# --- a model of the device's protocol ---

def foreign_image(seed=1):
    """A stream that passes S-02.091's rules and carries no waffle-boot front: no descriptor."""
    main = bytes((seed * 7 + i) & 0xFF for i in range(64))
    body = mkldr.header(0, FRONT_INIT, 32, 0) + bytes(32)
    body += mkldr.header(mkldr.INIT, FRONT_INIT, 0, 0)
    body += mkldr.header(0, 0x08031000, len(main), 0) + main
    body += mkldr.header(mkldr.FINAL, 0x08031000, 0, 0)
    return mkldr.header(mkldr.FIRST | mkldr.IGNORE, 0x08031000, 0, len(body)) + body


def bootable(img):
    """S-02.091's five rules, as the unit's scan reads a slot's stream."""
    if img is None:
        return False
    res = w.check_stream.check('bootloader', img, first_stage_stack=(0x11807000, 0x11808000))
    return all(ok for _, ok, _ in res)


class Model:
    """Recovery's protocol as front/recovery/proto.h states it, over the pty's master end.
    Its writer takes ten steps; the flash is two slot images and a selector.
    front is the version it plays: below 8, a refusal leaves phase failed even
    after a write has run. From 9 it speaks protocol 2: ACT and its rules, the
    force flag, and the countdown, which here runs one second for each INFO
    answered, so that a host polling INFO sees phase 5 for 15 polls."""

    def __init__(self, fd, slots, sel=(0, 1), running=0, established=True, why=1, front=6,
                 batt=b''):
        self.fd = fd
        self.slots = list(slots)
        self.sel = list(sel)
        self.running, self.established, self.why = running, established, why
        self.front, self.batt = front, batt     # a version 5 front: 199-byte INFO, no flags
        self.flags = 0
        self.way = w.WAY_NONE
        self.phase, self.len, self.got, self.buf = w.IDLE, 0, 0, bytearray()
        self.sha = b''
        self.result = self.image_rc = 0
        self.steps = 0
        self.written = w.NO_SLOT
        self.readback = bytes(32)
        self.corrupt = 0            # replies to damage before sending
        self.silent = 0             # requests to leave unanswered
        self.bad_selector = False   # a write that reports done but leaves the selector as it was
        self.restarted = False
        self.hold = False           # a write that makes no step
        self.other_failed = False   # a write of the other slot failed: in place is offered
        self.device, self.dev_how, self.fs = 0, 0, (0x86C0, 0xF3C93780)    # front 20's INFO
        self.spoiled = False        # INFO's flag bit 4
        self.held = False           # bit 5
        self.unsettled = False      # bit 7
        self.run_says = None        # (way, refusal) INFO and BEGIN give for the flag instead
        self.late = {}              # request type: seconds before its reply
        self.cut = {}               # request type: bytes its reply payload is cut to
        self.unanswered = set()     # request types it never answers
        self.begin_says = None      # (way, slot) BEGIN answers instead of its own plan
        self.end_says = None        # a status END answers instead, writing nothing
        self.restart_says = None    # a status RESTART answers instead
        self.begin_st = None        # a status BEGIN answers instead
        self.tried = set()          # slots whose record carries the trial mark (version 7)
        self.started = set()        # tried slots whose log's entry 0 reads finished: started
        self.trial_begun = False    # this recovery began a trial write
        self.fail_with = 0          # a status the write ends with at its last step
        self.moved = False          # INFO's flag bit 3
        self.chosen = w.NO_SLOT     # the slot a select names
        self.made = None            # the last write that ran: what a refusal leaves in INFO
        self.refused = 0            # version 11: a refusal or a stop since that write
        self.lose = {}              # request type: replies to it handled but not sent
        self.bad = set()            # slots holding bytes under a record that fails the check
        self.act = 0                # version 11: the action under way or ended, 0 for a write
        self.count = 0              # the countdown's seconds left
        self.drop_at = None         # the countdown's seconds left at which the host leaves
        self.hangup_after = None    # DATA requests after which the port goes away (unanswered)
        self.gone = False           # the port went away: the test gives the model a new one
        self.force = False          # the request under way was forced
        self.frames = []            # (type, payload) of every request
        self.phases = []            # (phase, seconds left) of every INFO answered
        self.seen = []
        self.rx = bytearray()
        self.stop = False
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def close(self):
        self.stop = True
        self.t.join(2)

    def target(self, force=False):
        """The default write's target, or its refusal; forced (version 11), the slot the selector
        does not name, whatever the selector reads, but the running slot only with the selected
        slot good (status 5)."""
        selected = w.selected(self.sel[0])
        if not self.established:
            return w.NO_SLOT, 2
        if force and selected != self.running:
            return (self.running, 0) if self.good(selected) else (w.NO_SLOT, 5)
        if force:
            return 1 - selected, 0
        if self.running != selected:
            return w.NO_SLOT, 3
        if self.sel[1] != 1:
            return w.NO_SLOT, 4
        if not self.good(selected):
            return w.NO_SLOT, 5
        return 1 - selected, 0

    def good(self, k):
        return self.slots[k] is not None and k not in self.tried and k not in self.bad

    def front_of(self, k):
        img = self.slots[k]
        if img is None:
            return None
        probs, front = w.check_image(img)
        return None if probs else front

    def tier(self, b):
        """The other slot against the running slot's front (enum fr_tier)."""
        if not self.good(b) and b not in self.tried:
            return w.TIER_BAD
        fb, fa = self.front_of(b), self.front_of(1 - b)
        if fb is None:
            return w.TIER_FOREIGN
        if fa is None or (fa['pin'], fa['front_crc']) != (fb['pin'], fb['front_crc']):
            return w.TIER_FRONT
        return w.TIER_SAME

    def damaged(self):
        return self.spoiled and not self.good(self.running)

    def act_plan(self, act, force):
        """An ACT's (status, slot, tier, way), by the rules the notes on S-02.084 give."""
        if not self.established:
            return 2, w.NO_SLOT, 0, w.WAY_NONE
        a, b = self.running, 1 - self.running
        s = w.selected(self.sel[0])
        home, trusted = (s == a and self.sel[1] == 1), (s == b and self.sel[1] == 1)
        b_tried, a_front = b in self.tried, self.front_of(a) is not None
        begun = self.moved or self.spoiled
        tier = self.tier(b)

        def no(st, t=tier):
            return st, w.NO_SLOT, t, w.WAY_NONE
        if act == w.ACT_TRY:
            if begun:
                return no(19)
            if trusted:
                return no(22)
            if not force:
                if not home:
                    return no(3 if s != a else 4)
                if not self.good(a) or not a_front:
                    return no(19)
                if not self.good(b) and not b_tried:
                    return no(23)
            return 0, b, tier, w.WAY_TRY
        if act == w.ACT_SWITCH:
            if trusted:
                return no(22)
            if not bootable(self.slots[b]):
                return no(7)
            if not force:
                if begun:
                    return no(19)
                if not home:
                    return no(3 if s != a else 4)
                if not self.good(b) and not b_tried:
                    return no(23)
                if b_tried and b not in self.started:
                    return no(21)
                if tier != w.TIER_SAME:
                    return no(12)
            return 0, b, tier, w.WAY_SWITCH
        if act == w.ACT_KEEP:
            if b_tried:
                if not force:
                    if begun or self.trial_begun:
                        return no(19)
                    if not home or not a_front:
                        return no(20)
                    if b not in self.started:
                        return no(21)
                return 0, b, tier, w.WAY_KEEP
            if a in self.tried:
                if not force and (begun or self.trial_begun):
                    return no(19, w.TIER_SAME)
                if not force and (not trusted or not self.good(b)):
                    return no(20, w.TIER_SAME)
                return 0, a, w.TIER_SAME, w.WAY_KEEP
            return no(20, 0)
        if not b_tried:
            return no(20, 0)
        return 0, b, 0, w.WAY_PUT_BACK

    def accept(self):
        """S-02.084: an accepted END or ACT counts down, but where the power-off is held."""
        self.steps, self.refused = 0, 0
        if self.front >= 11 and not self.held:
            self.phase, self.count = w.COUNTING, w.COUNTDOWN
        else:
            self.phase = w.WRITING

    def select_plan(self):
        """Flag bit 2: (status, the slot selected)."""
        a, b = self.running, 1 - self.running
        if self.spoiled or self.unsettled or (self.trial_begun and not self.force):
            return 19, w.NO_SLOT
        if a in self.tried and list(self.sel) == [b, 1] and self.good(b):
            return 0, a                 # in a trial start
        t, refusal = self.target()
        if t == w.NO_SLOT:
            return refusal, w.NO_SLOT
        if b not in self.tried:
            return 20, w.NO_SLOT
        return (0, b) if b in self.started or self.force else (21, w.NO_SLOT)

    def slot_bytes(self, k):
        img = self.slots[k]
        out = bytearray(53)
        if img is None:
            struct.pack_into('<BII', out, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF)
            return bytes(out)
        _, front = w.check_image(img)
        flags = (w.S_TRIED | (w.S_STARTED if k in self.started else 0) if k in self.tried
                 else 0 if k in self.bad else w.S_GOOD) | \
            (w.S_HAS_DESC | w.S_HAS_FRONT if front else 0)
        crc = zlib.crc32(img) ^ (0xFFFFFFFF if k in self.tried else 1 if k in self.bad else 0)
        struct.pack_into('<BII', out, 0, flags, len(img), crc & 0xFFFFFFFF)
        if front:
            struct.pack_into('<4HI', out, 9, *front['version'], front['front_crc'])
            out[21:53] = front['pin']
        return bytes(out)

    def info(self):
        t, refusal = self.target()
        flags = (w.F_ESTABLISHED if self.established else 0) | w.F_SELECTOR | w.F_OWN_DESC | \
            (w.F_SPOILED if self.spoiled else 0) | (w.F_HELD if self.held else 0) | \
            (w.F_UNSETTLED if self.unsettled else 0) | (w.F_MOVED if self.moved else 0)
        prog = 1000 if self.phase == w.DONE else self.steps * 100 if self.phase == w.WRITING else 0
        p = struct.pack('<BBBBBBBBIIHBBB', self.why, flags, self.running, self.sel[0], self.sel[1], t,
                        refusal, self.phase, self.len, self.got, prog, self.result, self.image_rc,
                        self.written if self.phase == w.DONE else w.NO_SLOT)
        img = self.slots[self.running]
        front = w.check_image(img)[1] if img else None
        p += struct.pack('<4H', *front['version']) + front['pin'] if front else bytes(40)
        p += self.readback if self.phase == w.DONE else bytes(32)
        p += self.slot_bytes(0) + self.slot_bytes(1)
        if self.front < 6:
            return p
        run_way, run_refusal = self.run_plan()
        p += bytes([run_way, run_refusal, self.way, len(self.batt)])
        p += self.batt + bytes(16 - len(self.batt))
        if self.front < 11:
            return p
        plans = [self.act_plan(act, False) for act in range(1, 5)]
        forced = [self.act_plan(act, True) for act in range(1, 5)]
        keep = forced[w.ACT_KEEP - 1]
        p += bytes([self.count if self.phase == w.COUNTING else 0, self.act,
                    self.tier(1 - self.running) if self.established else 0] +
                   [x[0] for x in plans] + [x[0] for x in forced] +
                   [keep[1] if keep[0] == 0 else w.NO_SLOT, self.refused])
        if self.front < 20:
            return p
        return p + bytes([self.device, self.dev_how]) + struct.pack('<II', *self.fs)

    def run_plan(self, force=False):
        if self.run_says:
            return self.run_says
        t, refusal = self.target()
        if force and self.established:
            return (w.WAY_SELECT_FIRST if self.good(1 - self.running) else w.WAY_IN_PLACE), 0
        if t == w.NO_SLOT:
            return w.WAY_NONE, refusal
        if self.good(1 - self.running):
            return w.WAY_SELECT_FIRST, 0
        if self.other_failed:
            return w.WAY_IN_PLACE, 0
        return w.WAY_NONE, 15

    def no_write(self, refusal, image_rc=0, at_end=False):
        """S-02.093: a transfer ends with no write. From version 8, once a write has run, INFO
        keeps that write's phase, status, slot, read-back and way, from version 11 its action
        too, with the refusal at byte 231; otherwise phase failed."""
        if self.made and self.front >= 8:
            (self.phase, self.result, self.way, self.written, self.readback,
             self.flags, act) = self.made
            if self.front >= 11:
                self.act, self.refused = act, refusal
            self.image_rc = 0
            return
        self.phase, self.result, self.image_rc = w.FAILED, refusal, image_rc
        if at_end:
            self.way = w.WAY_NONE

    def begin_reply(self, st, way=w.WAY_NONE, slot=w.NO_SLOT):
        return bytes([st]) if self.front < 6 else bytes([st, way, slot])

    def stop_countdown(self):
        """The host left during the countdown: nothing is written (status 24)."""
        self.way = w.WAY_NONE
        self.no_write(24)

    def host_left(self):
        """fr_proto_drop(): a transfer or a countdown under way ends with status 24 (version 11)."""
        if self.front >= 11 and self.phase in (w.RECEIVING, w.COUNTING):
            self.stop_countdown()

    def act_request(self, pl):
        """ACT {act, flags}: {status, way, slot, tier}."""
        if self.phase in (w.WRITING, w.COUNTING):
            return bytes([1, 0, w.NO_SLOT, 0])
        if len(pl) != 2 or not 1 <= pl[0] <= 4 or pl[1] & 0x7F:
            return bytes([0x40, 0, w.NO_SLOT, 0])
        st, t, tier, way = self.act_plan(pl[0], bool(pl[1] & w.BEGIN_F_FORCE))
        self.act, self.len, self.got = pl[0], 0, 0
        if st:
            self.no_write(st)
            return bytes([st, 0, w.NO_SLOT, tier])
        self.way, self.wt, self.flags = way, t, pl[1]
        self.accept()
        return bytes([0, way, t, tier])

    def handle(self, typ, pl):
        in_place = self.phase == w.WRITING and self.way == w.WAY_IN_PLACE
        busy = self.phase == w.COUNTING
        if (in_place or busy) and typ not in (w.HELLO, w.INFO):
            return bytes([1, 0, w.NO_SLOT, 0]) if typ == w.ACT else b'\x01'
        if typ == w.HELLO:
            return struct.pack('<BHHBB', 2 if self.front >= 11 else 1, self.front, w.MAX_DATA,
                               self.why, w.F_OWN_DESC) + self.info()[21:61]
        if typ == w.INFO:
            reply = self.info()
            self.phases.append((self.phase, reply[w.INFO_O_COUNT] if self.front >= 11 else 0))
            if self.phase == w.COUNTING:        # one second for each INFO answered
                self.count -= 1
                if self.drop_at is not None and self.count <= self.drop_at:
                    self.stop_countdown()
                elif self.count == 0:
                    self.phase = w.WRITING
            return reply
        if typ == w.ACT and self.front >= 11:
            return self.act_request(pl)
        if typ == w.RESTART and self.front >= 11 and \
                (len(pl) > 1 or (len(pl) == 1 and pl[0] & 0x7F)):
            return b'\x40'
        if typ == w.RESTART and pl and self.front < 11:
            return b'\x40'
        if typ == w.BEGIN:
            if self.phase == w.WRITING:
                return b'\x01'
            ok = (0, 1, 2, 4) if self.front >= 7 else (0, 1)
            mode = pl[36] & (0x7F if self.front >= 11 else 0xFF) if len(pl) == 37 else 0
            if len(pl) != 36 and not (self.front >= 6 and len(pl) == 37 and mode in ok):
                return b'\x40'
            self.flags = mode
            self.force = len(pl) == 37 and pl[36] != mode
            self.act = 0
            t, refusal = self.target(self.force)
            way = w.WAY_PLAIN
            if self.flags == w.BEGIN_F_RUNNING:
                way, refusal = self.run_plan(self.force)
                t = self.running if way else w.NO_SLOT
            elif self.flags == w.BEGIN_F_TRIAL and t != w.NO_SLOT:
                way = w.WAY_TRIAL
                if not self.good(self.running) and not self.force or self.spoiled or \
                        self.unsettled:
                    t, refusal = w.NO_SLOT, 19
            elif self.flags == w.BEGIN_F_SELECT:
                way = w.WAY_SELECT
                refusal, t = self.select_plan()
                self.chosen = t
            if self.begin_st:
                t, refusal = w.NO_SLOT, self.begin_st
            self.len = self.got = 0
            if t == w.NO_SLOT:
                self.no_write(refusal)
                return self.begin_reply(refusal)
            n = struct.unpack_from('<I', pl)[0]
            if not 1 <= n <= (w.IMAGE_MAX if self.front >= 6 else w.SLOT_CAP):
                self.no_write(6, 1)
                return self.begin_reply(6)
            self.len, self.sha, self.phase, self.buf = n, pl[4:36], w.RECEIVING, bytearray(n)
            return self.begin_reply(0, *(self.begin_says or (way, t)))
        if typ == w.DATA:
            if self.hangup_after is not None:
                self.hangup_after -= 1
                if self.hangup_after < 0:
                    self.hangup_after, self.gone = None, True
                    self.host_left()
                    return None
            off, data = struct.unpack_from('<I', pl)[0], pl[4:]
            st = 0
            if self.phase != w.RECEIVING:
                st = 0x41
            elif off == self.got and off + len(data) <= self.len:
                self.buf[off:off + len(data)] = data
                self.got += len(data)
            elif not (off + len(data) <= self.got and self.buf[off:off + len(data)] == data):
                st = 0x42
            return struct.pack('<BI', st, self.got)
        if typ == w.END:
            if self.phase == w.WRITING:
                return b'\x01\x00'
            if self.phase != w.RECEIVING:
                return b'\x41\x00'
            if self.got != self.len:
                return b'\x43\x00'
            probs, _ = w.check_image(bytes(self.buf))
            # forced (version 11): a bootable stream with no waffle-boot front is written
            if probs and not (self.force and bootable(bytes(self.buf)) and
                              all(p.startswith('no waffle-boot front') for p in probs)):
                self.no_write(7, 7, True)
                return b'\x07\x07'
            if hashlib.sha256(self.buf).digest() != self.sha:
                self.no_write(8, 0, True)
                return b'\x08\x00'
            if self.end_says:
                self.no_write(self.end_says, 0, True)
                return bytes([self.end_says, 0])
            t = {w.BEGIN_F_RUNNING: self.running, w.BEGIN_F_TRIAL: 1 - self.running,
                 w.BEGIN_F_SELECT: self.chosen}.get(self.flags, self.target(self.force)[0])
            have = w.check_image(self.slots[self.running])[1]
            new = w.check_image(bytes(self.buf))[1]
            same = have and new and (have['pin'], have['front_crc']) == (new['pin'], new['front_crc'])
            if self.flags == w.BEGIN_F_SELECT:
                if bytes(self.buf) != self.slots[t]:
                    self.no_write(20, 0, True)
                    return b'\x14\x00'
            elif not same and self.flags != w.BEGIN_F_TRIAL and not self.force:
                self.no_write(12, 0, True)
                return b'\x0c\x00'
            if self.front < w.SAME_FRONT and bytes(self.buf) == self.slots[1 - t]:
                self.no_write(13, 0, True)
                return b'\x0d\x00'
            self.way = {0: w.WAY_PLAIN, w.BEGIN_F_TRIAL: w.WAY_TRIAL,
                        w.BEGIN_F_SELECT: w.WAY_SELECT}.get(
                self.flags, w.WAY_SELECT_FIRST if self.good(1 - t) else w.WAY_IN_PLACE)
            self.trial_begun |= self.flags == w.BEGIN_F_TRIAL
            self.wt = t
            self.accept()
            return b'\x00\x00'
        if typ == w.RESTART:
            if self.phase == w.WRITING:
                return b'\x01'
            if self.restart_says:
                return bytes([self.restart_says])
            if self.front >= 11 and self.held and pl != bytes([w.BEGIN_F_FORCE]):
                return b'\x44'
            self.restarted = True
            return b'\x00'
        return b'\x40'

    def step(self):
        if self.phase != w.WRITING or self.hold:
            return
        self.steps += 1
        if self.steps == 10 and self.fail_with:
            # a failed trial write: 9 before the record, 10 and 11 after it; revision 2 settles
            # the selector back on the running slot before it answers 11
            t = self.wt
            if self.way <= w.WAY_SELECT:        # an action writes no slot's bytes
                self.slots[t] = None if self.fail_with == 9 else bytes(self.buf)
            if self.fail_with != 9 and self.way == w.WAY_TRIAL:
                self.tried.add(t)
            self.phase, self.result = w.FAILED, self.fail_with
            self.made = (self.phase, self.result, self.way, w.NO_SLOT, bytes(32), self.flags,
                         self.act)
            return
        if self.steps == 10 and self.way in (w.WAY_TRY, w.WAY_SWITCH, w.WAY_KEEP, w.WAY_PUT_BACK):
            self.act_ends()
            return
        if self.steps == 10:
            t = self.wt
            self.slots[t] = bytes(self.buf)
            self.readback = hashlib.sha256(self.buf).digest()
            if self.way == w.WAY_TRIAL:         # the trial mark; the selector as S-02.084 leaves it
                self.tried.add(t)
                self.started.discard(t)
                self.sel = [0, 0xFE] if t == 0 else [0xFF, 0xFF]
            elif self.way == w.WAY_SELECT:
                self.tried.discard(t)
                self.started.discard(t)
                self.sel = [t, 0]
            elif self.unsettled:                # S-02.084: the settling on the target
                self.sel, self.unsettled, self.held = [t, 1], False, False
            elif not self.bad_selector and self.way != w.WAY_IN_PLACE:
                self.sel = [t, 0]
            self.written, self.phase, self.result = t, w.DONE, 0
            self.made = (self.phase, self.result, self.way, t, self.readback, self.flags, self.act)

    def act_ends(self):
        """An action's writes, as the notes on S-02.084 give them."""
        t, a = self.wt, self.running
        s = w.selected(self.sel[0])
        if self.way == w.WAY_TRY:
            self.trial_begun = True
            if self.good(t):
                self.tried.add(t)
            self.started.discard(t)
            self.sel = [0, 0xFE] if t == 0 else [0xFF, 0xFF]
        elif self.way == w.WAY_PUT_BACK:
            if s == t and self.sel[1] != 1 and not self.damaged():
                self.sel = [a, 1]
            self.tried.discard(t)
            self.started.discard(t)
        else:
            bad = not self.good(t) and t not in self.tried
            self.tried.discard(t)
            self.started.discard(t)
            self.sel = [t, 1] if self.damaged() or bad else [t, 0]
        self.written, self.phase, self.result = t, w.DONE, 0
        self.readback = bytes(32)
        self.made = (self.phase, self.result, self.way, t, self.readback, self.flags, self.act)

    def run(self):
        import select
        while not self.stop:
            if self.fd is None:             # the port went away: wait for the next one
                time.sleep(0.005)
                continue
            r, _, _ = select.select([self.fd], [], [], 0.01)
            self.step()
            if not r:
                continue
            try:
                self.rx += os.read(self.fd, 4096)
            except OSError:
                return
            while True:
                got = w.take_frame(self.rx)
                if got is None:
                    break
                typ, seq, pl = got
                self.seen.append(typ)
                self.frames.append((typ, pl))
                if self.silent or typ in self.unanswered:
                    self.silent -= bool(self.silent)
                    continue
                if typ in self.late:            # a survey on the unit, some seconds
                    time.sleep(self.late[typ])
                reply = self.handle(typ, pl)
                if self.gone:               # None first: the test sets the next port once it sees this
                    fd, self.fd, self.gone, self.rx = self.fd, None, False, bytearray()
                    os.close(fd)
                    break
                if self.lose.get(typ):
                    self.lose[typ] -= 1
                    continue
                if typ in self.cut:
                    reply = reply[:self.cut[typ]]
                wire = bytearray(w.frame(typ | w.REPLY, seq, reply))
                if self.corrupt:
                    self.corrupt -= 1
                    wire[-1] ^= 0xFF
                os.write(self.fd, bytes(wire))


class Lines:
    def __init__(self):
        self.text = ''

    def __call__(self, *a, end='\n'):
        self.text += ' '.join(str(x) for x in a) + end


class Ask:
    """Stands in for the user at the tool's prompts: the answers in order; a prompt with no
    answer left fails the test."""

    def __init__(self, *answers):
        self.answers, self.prompts = list(answers), []

    def __call__(self, prompt):
        self.prompts.append(prompt)
        if not self.answers:
            raise AssertionError('unexpected prompt: %s' % prompt)
        return self.answers.pop(0)


def no_prompt(prompt):
    raise AssertionError('unexpected prompt: %s' % prompt)


def stale_frame():
    """A DATA frame of the largest size the parser takes, whose payload holds frame starts of its own ("WR" with
    a length up to the largest), for an earlier host to leave cut off."""
    inner = b'WR\x04\x09' + struct.pack('<H', w.MAX_PAYLOAD) + b'WR\x02\x01\x00'
    body = (inner * (w.MAX_PAYLOAD // len(inner) + 1))[:w.MAX_PAYLOAD - 4]
    f = w.frame(w.DATA, 7, struct.pack('<I', 0) + body)
    assert len(f) == w.FRAME_MAX
    return f


class Sync(unittest.TestCase):
    def test_every_cut(self):
        """Whatever part of a frame an earlier host left, the zeros sent before the first request
        leave the parser (take_frame(), as front/recovery/proto.c's next_frame()) in step: the next frame is
        the request, whole."""
        f, hello = stale_frame(), w.frame(w.HELLO, 1)
        for k in range(len(f)):
            buf = bytearray(f[:k] + bytes(w.FRAME_MAX) + hello)
            got = []
            while True:
                t = w.take_frame(buf)
                if t is None:
                    break
                got.append(t)
            self.assertEqual(got, [(w.HELLO, 1, b'')], k)


class SlotKind(unittest.TestCase):
    """S-02.106: a slot's kind from INFO's slot fields and flag bit 5, against the oracle's synthetic
    vectors (tests/oracle/S-02-spi-flash/vectors/slot_kinds.json)."""
    ORACLE = os.path.join(TOP, 'tests', 'oracle', 'S-02-spi-flash')

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, cls.ORACLE)
        import slot_kind
        cls.model = slot_kind
        with open(os.path.join(cls.ORACLE, 'vectors', 'slot_kinds.json'), encoding='utf-8') as f:
            cls.cases = json.load(f)['cases']

    def slot(self, case, tried):
        """INFO's slot fields for a case's image as a good or a tried slot, bit 5 as the front sets it."""
        data = self.model.build(case)
        crc = int(case['crc32'], 16)
        prefix = len(data) >= self.model.PREFIX_LEN and \
            zlib.crc32(data[:self.model.PREFIX_LEN]) == self.model.PREFIX_CRC
        return {'good': not tried, 'tried': tried, 'started': False, 'has_desc': case['descriptor_valid'],
                'version': tuple(case['descriptor_version'] or (0, 0, 0, 0)), 'length': len(data),
                'crc': crc ^ 0xFFFFFFFF if tried else crc, 'te_prefix': prefix}

    def test_table(self):
        self.assertEqual(w.TE_BUILDS, self.model.TE_BUILDS)

    def test_vectors(self):
        self.assertEqual(len(self.cases), 15)
        for case in self.cases:
            want = (case['kind'], tuple(case['version']) if case['version'] else None)
            for tried in (False, True):
                with self.subTest(case=case['name'], tried=tried):
                    self.assertEqual(w.slot_kind(self.slot(case, tried)), want)
                    if case['text'] is not None:
                        self.assertEqual(w.kind_text(want), 'TE v' + case['text'])

    def test_no_kind(self):
        """A slot that is neither good nor tried has no kind, and its line shows none."""
        s = self.slot(self.cases[-1], False)
        s['good'] = False
        self.assertIsNone(w.slot_kind(s))

    def test_made_up_row(self):
        """A row the table gains names its build, with +build when build is not 0."""
        s = {'good': True, 'tried': False, 'has_desc': False, 'version': (0, 0, 0, 0),
             'length': 0x1234, 'crc': 0xCAFEF00D, 'te_prefix': True}
        self.assertEqual(w.kind_text(w.slot_kind(s)), 'TE')
        with mock.patch.dict(w.TE_BUILDS, {(0x1234, 0xCAFEF00D): (2, 1, 0, 7)}):
            self.assertEqual(w.kind_text(w.slot_kind(s)), 'TE v2.1.0+7')
            s['te_prefix'] = False
            self.assertEqual(w.kind_text(w.slot_kind(s)), 'TE v2.1.0+7')
        self.assertEqual(w.kind_text(w.slot_kind(s)), '??')
        s['has_desc'], s['version'] = True, (0, 9, 37, 0)
        self.assertEqual(w.kind_text(w.slot_kind(s)), 'WB v0.9.37')

    def test_info_bit5(self):
        """INFO's slot flag bit 5 reaches the slot as te_prefix; the state words."""
        p = bytearray(w.INFO_V11)
        for k, sf in enumerate((w.S_GOOD | w.S_TE_PREFIX, w.S_GOOD)):
            struct.pack_into('<BII', p, w.INFO_O_SLOT + w.INFO_SLOT_LEN * k, sf, 0x40000, 0x12345678)
        i = w.parse_info(bytes(p))
        self.assertEqual([w.kind_text(w.slot_kind(x)) for x in i['slots']], ['TE', '??'])
        s = {'good': True, 'tried': False, 'started': False, 'has_desc': False, 'has_front': False,
             'version': (0, 0, 0, 0), 'length': 0x40000, 'crc': 0x12345678, 'te_prefix': True,
             'pin': bytes(32)}
        self.assertEqual(w.slot_state(s), 'checked')
        self.assertEqual(w.kind_text(w.slot_kind(s)), 'TE')
        s['tried'], s['started'] = True, True
        self.assertEqual(w.slot_state(s), 'tried, started')
        s['tried'], s['good'] = False, False
        self.assertEqual(w.slot_state(s), 'check failed')


class Words(unittest.TestCase):
    def test_status_texts(self):
        self.assertIn('not the running slot with its front intact', w.status_text(5))
        self.assertIn('slot that is kept already holds', w.status_text(13))
        self.assertIn('no slot or record was written', w.status_text(14))
        self.assertIn('three starts in a row', w.WHY[3])
        self.assertIn('front is not intact', w.refusal_text(12))
        self.assertIn('only after a write of the other slot has failed', w.status_text(15))
        self.assertIn('nothing was written', w.status_text(16))
        self.assertIn('refuses a restart', w.status_text(0x44))
        self.assertNotIn('restart the unit', w.status_text(11))
        self.assertEqual(w.status_text(17), 'the selector or a slot could not be read for the start '
                         'condition; nothing was written. Run info and try again.')
        self.assertIn('the target is the only checked slot, so the unit accepts only the image it '
                      'holds, to settle the selector on it', w.status_text(18))
        self.assertIn('that slot is already the boot slot', w.status_text(22))
        self.assertIn('failed its check and is not tried', w.status_text(23))
        self.assertEqual(w.status_text(24), 'the write was stopped before its first erase: the unit '
                         'was switched off or USB was unplugged, and nothing was written. Run info, '
                         'then start the command again if it is still wanted.')
        for st, (what, tip) in w.STATUS.items():
            self.assertTrue(st == 0 or tip, st)      # every refusal says what to do
        spoiled = {'spoiled': True}
        for st in w.RESTART_TIPS:
            self.assertNotIn('estart', w.status_text(st, i=spoiled))
            self.assertNotIn('Power +', w.status_text(st, i=spoiled))

    def test_battery(self):
        """S-07.053: byte 0 the status, 1 the charge, 5-6 the voltage, big-endian."""
        self.assertEqual(w.battery(b'')[0], 'unknown')
        self.assertEqual(w.battery(b'\x00' * 7)[0], 'absent')
        self.assertEqual(w.battery(b'\x02\x50'),
                         ('unknown', 'present, charging, 80 %, its voltage not reported'))
        st, words = w.battery(bytes([1, 80, 25, 0, 0, 0x0F, 0x3C]))
        self.assertEqual((st, words), ('charged', 'present and charged (not charging, 80 %, 3900 mV)'))
        self.assertEqual(w.battery(bytes([2, 5, 25, 0, 0, 0x0F, 0x3C]))[0], 'low')
        self.assertEqual(w.battery(bytes([2, 50, 25, 0, 0, 0x0D, 0xAC]))[0], 'low')     # 3500 mV


# END's reply lost, and END sent again finds no transfer open (S-02.093)
REFUSED = ("Refused: the unit did not take this write. END's reply was lost, and the unit still "
           'reports the write made earlier in this recovery (%s), as a refused END leaves it '
           '(S-02.093); the reason was only in the lost reply. Nothing of this file was written. '
           'Run info, then write again.')
# front 11: INFO keeps the reason of a refusal that follows a write (byte 231)
REFUSED_V11 = ("END's reply was lost; the unit still reports the write made earlier in this recovery "
              '(%s). Nothing of this file was written.')
CANNOT_TELL = ("END's reply was lost, and the unit reports a failed write with the way this one was "
               'announced: a flash erase, program or read of the write failed. That is either a write '
               'made earlier in this recovery, which a refused END leaves in place (S-02.093), or '
               'this write, failed: what the unit reports cannot tell the two apart.')


class PtyCase(unittest.TestCase):
    def setUp(self):
        self.master, slave = os.openpty()
        self.link = w.Link(os.ttyname(slave), timeout=0.3, retries=4, long_timeout=1.0)
        os.close(slave)
        self.models = []

    def tearDown(self):
        for m in self.models:
            m.close()
        self.link.close()
        os.close(self.master)

    def model(self, **kw):
        kw.setdefault('slots', (make_image(seed=1), None))
        m = Model(self.master, **kw)
        self.models.append(m)
        return m


class OverPty(PtyCase):
    def test_info(self):
        self.model(slots=(make_image(seed=1), make_image(seed=2, version=(0, 9, 0, 0))))
        out = Lines()
        self.assertEqual(w.cmd_info(self.link, out), 0)
        self.assertIn('Power + M4 was held', out.text)
        self.assertIn('Running:  Slot 1', out.text)
        self.assertIn('Selector: names Slot 1, verified 01', out.text)
        self.assertIn('Slot 1:   checked', out.text)
        self.assertIn('[running, boot slot]', out.text)
        self.assertIn('version 0.9.0', out.text)
        self.assertIn('Write:    would go to Slot 2', out.text)
        self.assertIn('State:    idle', out.text)

    def test_info_refusal(self):
        self.model(sel=(1, 1), running=1, slots=(make_image(), None))
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('Slot 2:   check failed', out.text)
        self.assertIn('refused: the boot slot failed its check', out.text)
        self.assertNotIn('Power + 1', out.text)
        self.assertIn('if it is checked and holds a stream the boot ROM can start (S-02.091)', out.text)

    def test_info_not_established(self):
        """S-01.002: the running slot is the one slot whose stream matches whole."""
        self.model(established=False)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('Running:  not established (the loaded image matches neither slot whole, or '
                      'both alike)', out.text)
        self.assertIn('could not establish which slot it is running from', out.text)

    def test_write(self):
        m = self.model()
        img = make_image(seed=7, size=3000)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.01, ask=Ask('yes')), 0, out.text)
        self.assertEqual(m.slots[1], img)
        self.assertEqual(m.sel, [1, 0])
        self.assertIn('Sending: 100.0%', out.text)
        self.assertIn('Writing: 100.0%', out.text)
        self.assertIn('Verified: Slot 2 holds this file', out.text)
        self.assertGreaterEqual(m.seen.count(w.DATA), 4)    # 1 KiB chunks

    def test_write_same_version_warns(self):
        """Another image with the version of a slot on the unit: a warning, then the write."""
        m = self.model()
        img = make_image(seed=7)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.01, ask=Ask('yes')), 0, out.text)
        self.assertIn('Warning: Slot 1 holds a different image with the same version 1.2.3+4; '
                      'make bump-version gives the next one.', out.text)
        self.assertEqual(m.slots[1], img)

    def test_write_other_version_no_warning(self):
        m = self.model()
        img = make_image(seed=7, version=(1, 2, 4, 4))
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.01, ask=Ask('yes')), 0, out.text)
        self.assertNotIn('Warning', out.text)
        self.assertEqual(m.slots[1], img)

    def test_write_refused(self):
        m = self.model(sel=(0, 0))
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=3), out), 1)
        self.assertIn('not settled', out.text)
        self.assertIn('Restart once without holding a key', out.text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_write_other_front(self):
        """S-02.084: an image whose front is not the running one is refused before anything is
        sent, with the way a new front goes in."""
        m = self.model()
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=7, front=2), out), 1)
        self.assertIn('the file carries another front', out.text)
        self.assertIn('write --new-front IMAGE', out.text)
        self.assertIn('the test OS installer (wiki/Test-OS.md)', out.text)
        self.assertNotIn(w.BEGIN, m.seen)
        self.assertEqual(m.slots[1], None)

    def test_write_same(self):
        """The selected slot's own image: refused before sending."""
        m = self.model()
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=1), out), 1)
        self.assertIn('already holds this image', out.text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_status_texts(self):
        self.assertIn('no trial was asked for', w.status_text(12))
        self.assertIn('--new-front', w.status_text(12))
        self.assertIn('already holds', w.status_text(13))
        self.assertIn('SHA-256', w.status_text(10))
        self.assertIn('did not read back', w.status_text(11))

    def test_write_wrong_selector(self):
        """A unit that reports the write done, read back, with the selector not {target, 00}:
        not reported as verified (S-02.048)."""
        m = self.model()
        m.bad_selector = True
        img = make_image(seed=7)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.01, ask=Ask('yes')), 1, out.text)
        self.assertEqual(m.slots[1], img)
        self.assertIn('NOT VERIFIED', out.text)
        self.assertIn('the selector, which should read 01 00', out.text)
        self.assertNotIn('Verified:', out.text)

    def test_damaged_reply_retried(self):
        m = self.model()
        m.corrupt = 2
        out = Lines()
        self.assertEqual(w.cmd_info(self.link, out), 0)
        self.assertEqual(m.seen[:3], [w.HELLO, w.HELLO, w.HELLO])
        m.corrupt, m.silent = 1, 1
        img = make_image(seed=9, size=2048)
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.01, ask=Ask('yes')), 0, out.text)
        self.assertEqual(m.slots[1], img)

    def test_no_answer(self):
        m = self.model()
        m.silent = 100
        with self.assertRaises(w.Failure):
            w.cmd_info(self.link, Lines())

    def test_short_replies(self):
        """A reply shorter than its type's layout is a clear failure, not an index error or a
        loop."""
        img = make_image(seed=9)
        for typ, n, words in ((w.DATA, 1, "DATA's reply is 1 bytes, short of the 5"),
                              (w.END, 1, "END's reply is 1 bytes, short of the 2"),
                              (w.BEGIN, 1, "BEGIN's reply is 1 bytes, short of the 3"),
                              (w.RESTART, 0, "RESTART's reply is 0 bytes, short of the 1")):
            m = self.model()
            m.cut = {typ: n}
            with self.assertRaises(w.Failure) as e:
                if typ == w.RESTART:
                    w.cmd_restart(self.link, Lines())
                else:
                    w.cmd_write(self.link, img, Lines(), poll=0.01, ask=Ask('yes'))
            self.assertIn(words, str(e.exception))
            m.close()
            self.models.remove(m)

    def test_short_writes(self):
        """A write to the port that takes only a few bytes: the rest of the frame follows."""
        m = self.model()
        real, fd = os.write, self.link.fd

        def short(f, b):
            return real(f, bytes(b[:3]) if f == fd else b)
        img = make_image(seed=9, size=2048)
        out = Lines()
        with mock.patch.object(w.os, 'write', short):
            self.assertEqual(w.cmd_write(self.link, img, out, poll=0.01, ask=Ask('yes')), 0, out.text)
        self.assertEqual(m.slots[1], img)

    def test_restart(self):
        m = self.model()
        out = Lines()
        self.assertEqual(w.cmd_restart(self.link, out), 0)
        self.assertTrue(m.restarted)
        self.assertIn('restarts', out.text)

    def test_restart_reply_lost(self):
        """RESTART went out and no reply came: most likely the unit restarted, which is reported
        as such, not as a failure."""
        m = self.model()
        m.unanswered = {w.RESTART}
        out = Lines()
        self.assertEqual(w.cmd_restart(self.link, out), 0)
        self.assertIn('most likely restarted', out.text)
        self.assertEqual(m.seen.count(w.RESTART), w.LONG_RETRIES)

    def test_restart_not_sent(self):
        """A port that fails before RESTART goes out is a failure."""
        m = self.model()
        real = os.write

        def gone(fd, b):
            if fd == self.link.fd and bytes(b[:3]) == b'WR' + bytes([w.RESTART]):
                raise OSError('gone')
            return real(fd, b)
        with mock.patch.object(w.os, 'write', gone), self.assertRaises(w.Failure):
            w.cmd_restart(self.link, Lines())
        self.assertNotIn(w.RESTART, m.seen)
        self.assertIn(w.INFO, m.seen)

    def test_info_v6(self):
        m = self.model(why=3, batt=bytes([0, 0, 0, 0, 0, 0, 0]))
        m.other_failed = True
        out = Lines()
        self.assertEqual(w.cmd_info(self.link, out), 0)
        self.assertIn('Front version 6', out.text)
        self.assertIn('three starts in a row did not finish', out.text)
        self.assertIn('with --running: would go to Slot 1, the running slot, in place', out.text)
        self.assertIn('Battery:  absent', out.text)

    def test_v5(self):
        """A version 5 front: 199 bytes of INFO, no flags. info is as before, a plain write goes
        through, --running is refused before anything is sent."""
        m = self.model(front=5)
        out = Lines()
        self.assertEqual(w.cmd_info(self.link, out), 0)
        self.assertIn('Front version 5', out.text)
        self.assertIn('Write:    would go to Slot 2\n', out.text)
        self.assertNotIn('--running', out.text)
        self.assertNotIn('Battery', out.text)
        out = Lines()
        img = make_image(seed=7)
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.01, running=True, ask=no_prompt), 1)
        self.assertIn('front version 5', out.text)
        self.assertIn('--running needs a version 6 front', out.text)
        self.assertNotIn(w.BEGIN, m.seen)
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.01, ask=no_prompt), 0, out.text)
        self.assertEqual(m.slots[1], img)
        self.assertEqual(m.sel, [1, 0])

    def test_v5_begin_flags(self):
        """The model's version 5 front answers BEGIN with flags as a malformed request."""
        self.model(front=5)
        img = make_image(seed=7)
        w.hello(self.link)
        r = self.link.request(w.BEGIN, struct.pack('<I', len(img)) + hashlib.sha256(img).digest() + b'\x01')
        self.assertEqual(r[0][0], 0x40)

    def test_in_place_busy(self):
        """During an in-place write the tool polls INFO only, and a request but HELLO and INFO is
        answered busy."""
        m = self.model(batt=bytes([1, 90, 20, 0, 0, 0x10, 0x00]))
        m.other_failed = True
        img = make_image(seed=7)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.01, running=True,
                                     ask=Ask(w.PHRASE)), 0, out.text)
        self.assertIn('present and charged', out.text)
        self.assertEqual(m.slots[0], img)
        self.assertEqual(m.sel, [0, 1])
        after = m.seen[m.seen.index(w.END) + 1:]
        self.assertEqual(set(after), {w.INFO})
        m.hold = True
        m.phase, m.way, m.steps = w.WRITING, w.WAY_IN_PLACE, 0
        for typ in (w.BEGIN, w.DATA, w.END, w.RESTART):
            self.assertEqual(self.link.request(typ, b'')[0], b'\x01')
        self.assertEqual(w.parse_hello(self.link.request(w.HELLO)[0])['front_version'], 6)
        self.assertEqual(w.parse_info(self.link.request(w.INFO)[0])['phase'], w.WRITING)

    def test_confirms_on_begin(self):
        """INFO predicts a plain write of Slot 2; BEGIN answers way 2 for Slot 1. The tool warns
        for BEGIN's answer, and a decline sends nothing more."""
        m = self.model(slots=(make_image(seed=1), make_image(seed=2, version=(0, 9, 0, 0))))
        m.begin_says = (w.WAY_SELECT_FIRST, 0)
        out = Lines()
        ask = Ask('no')
        self.assertEqual(w.cmd_write(self.link, make_image(seed=7), out, ask=ask), 1)
        self.assertIn('Writing 348 bytes to Slot 1, the running slot, after the selector names '
                      'Slot 2.', out.text)
        self.assertIn('This erases and rewrites Slot 1, the running slot.', out.text)
        self.assertNotIn('which this write replaces', out.text)
        self.assertEqual(m.seen, [w.HELLO, w.INFO, w.BEGIN, w.INFO])
        self.assertIn('Stopped: no image byte was sent', out.text)

    def test_begin_15(self):
        """INFO offers in place (patched here), BEGIN refuses it with 15."""
        m = self.model()
        out = Lines()
        orig = w.get_info

        def info_says_in_place(link, timeout=None):
            i = orig(link, timeout)
            i['run_way'], i['run_refusal'] = w.WAY_IN_PLACE, 0
            return i
        w.get_info = info_says_in_place
        try:
            self.assertEqual(w.cmd_write(self.link, make_image(seed=7), out, running=True,
                                         ask=no_prompt), 1)
        finally:
            w.get_info = orig
        self.assertIn('Try a plain write first', out.text)
        self.assertIn('Nothing was sent beyond BEGIN.', out.text)
        self.assertNotIn(w.DATA, m.seen)

    def test_end_16(self):
        m = self.model()
        m.end_says = 16
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=7), out, ask=Ask('yes')), 1)
        self.assertIn("the unit's state changed since the write was confirmed", out.text)
        self.assertIn('nothing was written', out.text)
        self.assertEqual(m.slots[1], None)

    def failed_write(self, front):
        """A model of that front version after a plain write of Slot 2 that failed (status 9)."""
        m = self.model(front=front)
        m.fail_with = 9
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=7), out, poll=0.01, ask=Ask('yes')),
                         1, out.text)
        self.assertIn('FAILED: a flash erase, program or read of the write failed', out.text)
        m.fail_with = 0
        return m

    def drop_model(self, m):
        m.close()
        self.models.remove(m)

    def test_refusal_keeps_write(self):
        """S-02.093: from version 8, once a write has run, a refused BEGIN or END leaves INFO
        showing that write, the refusal in the reply alone; version 7 leaves phase failed with
        the refusal."""
        for front in (7, 8):
            with self.subTest(front=front):
                m = self.failed_write(front)
                kept = (w.FAILED, 9, w.WAY_PLAIN)
                m.begin_st = 17
                out = Lines()
                self.assertEqual(w.cmd_write(self.link, make_image(seed=8), out, ask=no_prompt), 1)
                self.assertIn('Refused: the selector or a slot could not be read', out.text)
                i = w.get_info(self.link)
                self.assertEqual((i['phase'], i['result'], i['way']),
                                 kept if front == 8 else (w.FAILED, 17, w.WAY_PLAIN))
                m.begin_st, m.end_says = None, 16
                out = Lines()
                self.assertEqual(w.cmd_write(self.link, make_image(seed=8), out, ask=Ask('yes')), 1)
                self.assertIn("Refused: the unit's state changed", out.text)
                i = w.get_info(self.link)
                self.assertEqual((i['phase'], i['result'], i['way']),
                                 kept if front == 8 else (w.FAILED, 16, w.WAY_NONE))
                self.drop_model(m)

    def test_end_lost_after_write(self):
        """A failed write, then a second whose END is refused (16) with its reply lost. Version 8
        keeps the failed write in INFO, with the same way, so the tool cannot tell a refusal from
        this write failed alike; version 7 shows the refusal, and the tool reports it."""
        for front in (7, 8):
            with self.subTest(front=front):
                m = self.failed_write(front)
                m.end_says, m.lose = 16, {w.END: 1}
                out = Lines()
                self.assertEqual(w.cmd_write(self.link, make_image(seed=8), out, poll=0.01,
                                             ask=Ask('yes')), 1)
                self.assertEqual(m.seen.count(w.END), 3)
                if front == 8:
                    self.assertIn(CANNOT_TELL, out.text)
                else:
                    self.assertIn("FAILED: the unit's state changed", out.text)
                self.drop_model(m)

    def test_late_reply(self):
        """A reply that comes after the short timeout but within the long one: INFO, BEGIN and
        RESTART are sent once, so the unit starts no second survey."""
        m = self.model()
        m.late = {w.INFO: 0.6, w.RESTART: 0.6}
        t = time.monotonic()
        self.assertEqual(w.cmd_info(self.link, Lines()), 0)
        self.assertGreater(time.monotonic() - t, 0.5)
        self.assertEqual(m.seen.count(w.INFO), 1)
        self.assertEqual(w.cmd_restart(self.link, Lines()), 0)
        self.assertEqual(m.seen.count(w.RESTART), 1)

    def test_late_reply_dropped(self):
        """A reply to an earlier attempt is not taken for the later one."""
        m = self.model()
        m.late = {w.INFO: 1.2}          # longer than the long timeout here (1 s)
        with self.assertRaises(w.Failure):
            w.cmd_info(self.link, Lines())
        self.assertEqual(m.seen.count(w.INFO), w.LONG_RETRIES)

    def test_unsettled(self):
        """Bit 7: the running slot read back good, the selector not settled on it, the other slot
        bad. The unit holds; write --running with the running slot's own image only settles the
        selector, another image is refused before BEGIN (status 18's case), write writes the other
        slot; restart is refused."""
        m = self.model()
        m.unsettled, m.held, m.run_says = True, True, (w.WAY_PLAIN, 0)
        m.sel = [1, 0xFF]
        a_img = m.slots[0]
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('A write of this recovery read Slot 1, the running slot, back checked but did not '
                      'settle the selector on it, and Slot 2 failed its check.', out.text)
        self.assertIn('with --running: would go to Slot 1, which takes only the image it holds '
                      '(version 1.2.3+4, %d bytes, CRC-32 %08x), to settle the selector on it: the '
                      'slot is not erased, only the selector is written'
                      % (len(a_img), zlib.crc32(a_img)), out.text)
        self.assertIn('Ways out: write writes Slot 2. write --running with the image Slot 1 holds '
                      '(version 1.2.3+4, %d bytes, CRC-32 %08x) settles the selector on Slot 1: the '
                      'slot is not erased, only the selector is written; another image is refused. '
                      'Each write ends by settling the selector on the slot it wrote.'
                      % (len(a_img), zlib.crc32(a_img)), out.text)
        m.seen = []
        out = Lines()
        self.assertEqual(w.cmd_restart(self.link, out), 1)
        self.assertNotIn(w.RESTART, m.seen)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=7), out, running=True,
                                     ask=no_prompt), 1)
        self.assertIn('Refused before sending: Slot 1 is the only checked slot, so the unit accepts only '
                      'the image it holds', out.text)
        self.assertNotIn(w.BEGIN, m.seen)
        out = Lines()
        ask = Ask('yes')
        self.assertEqual(w.cmd_write(self.link, a_img, out, poll=0.01, running=True, ask=ask), 0,
                         out.text)
        self.assertIn('This settles the selector on Slot 1, which already holds this image (version '
                      '1.2.3+4). The slot is not erased: the unit reads it back and then writes only '
                      'the selector, to name Slot 1.', out.text)
        self.assertEqual(ask.prompts, ['Type yes to go on: '])

    def test_restart_0x44(self):
        """A unit that answers RESTART 0x44 (here with INFO not showing why): the tool says so."""
        m = self.model()
        m.restart_says = 0x44
        out = Lines()
        self.assertEqual(w.cmd_restart(self.link, out), 1)
        self.assertIn('Refused: the unit refuses a restart', out.text)
        self.assertFalse(m.restarted)
        m.spoiled, m.held, m.slots[0] = True, True, None     # flag bits 4 and 5
        out = Lines()
        m.seen = []
        self.assertEqual(w.cmd_restart(self.link, out), 1)
        self.assertIn('Not restarted', out.text)
        self.assertNotIn(w.RESTART, m.seen)

    # --- version 7: another front, by trial, and its select ---

    NEW = make_image(seed=3, front=2, version=(1, 3, 0, 0))

    @staticmethod
    def pins(*known, git=True, now=None):
        """An injected lookup: these (pin, front version) pairs signed by commits."""
        return lambda: {'git': git, 'pins': dict(known) if git else {}, 'now': now}

    def trial(self, ask, running=0, **kw):
        """A version 7 unit running slot `running`, the other slot empty; write --new-front."""
        slots = (make_image(seed=1), None) if running == 0 else (None, make_image(seed=1))
        m = self.model(front=7, slots=slots, sel=(running, 1), running=running)
        out = Lines()
        kw.setdefault('pins', self.pins())
        rc = w.cmd_write(self.link, self.NEW, out, poll=0.01, ask=ask, new_front=True, **kw)
        return m, rc, out.text

    def test_new_front_same_front(self):
        """The file's front is the running one: no flag, a plain write, with or without the option."""
        for new_front in (False, True):
            m = self.model(front=7)
            img = make_image(seed=7 + new_front)
            out = Lines()
            self.assertEqual(w.cmd_write(self.link, img, out, poll=0.01, ask=Ask('yes'),
                                         new_front=new_front, pins=no_prompt), 0, out.text)
            self.assertEqual(m.flags, 0)
            self.assertEqual((m.slots[1], m.sel), (img, [1, 0]))
            self.assertNotIn('trial', out.text)
            m.close()
            self.models.remove(m)

    def test_other_front_without_option(self):
        m = self.model(front=7)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, self.NEW, out, ask=no_prompt), 1)
        self.assertIn('Refused before sending: the file carries another front (pin ', out.text)
        self.assertIn('Recovery writes another front only for one trial start, with write --new-front '
                      'IMAGE. Nothing was sent.', out.text)
        self.assertNotIn('installer', out.text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_other_front_v6(self):
        """A version 6 front cannot take another front: refused before BEGIN, the installer named."""
        m = self.model(front=6)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, self.NEW, out, ask=no_prompt, new_front=True), 1)
        self.assertIn('this unit runs front version 6: a front before version 7 cannot take another '
                      'front in recovery. The way is the test OS installer (wiki/Test-OS.md).', out.text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_new_front_no_reported_front(self):
        """The running image's descriptor is not valid: --new-front is refused, not dropped."""
        m = self.model(front=7)
        orig = w.get_info

        def no_desc(link, timeout=None):
            i = orig(link, timeout)
            i['own_desc'] = False
            return i
        w.get_info = no_desc
        try:
            out = Lines()
            self.assertEqual(w.cmd_write(self.link, self.NEW, out, ask=no_prompt, new_front=True,
                                         pins=self.pins()), 1)
        finally:
            w.get_info = orig
        self.assertIn("the unit does not report the front it runs (the running image's descriptor "
                      "is not valid), so the tool cannot tell whether the file's front differs",
                      out.text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_trial_refused_moved(self):
        """INFO flag bit 3: this recovery moved the selector; a trial is 19, before BEGIN."""
        m = self.model(front=7)
        m.moved = True
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, self.NEW, out, ask=no_prompt, new_front=True,
                                     pins=self.pins()), 1)
        self.assertIn('Refused before sending: a trial needs the running slot checked', out.text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_trial_begin_other_way(self):
        """BEGIN answers a plain write for a trial: nothing is written, and the tool says so."""
        m = self.model(front=7)
        m.begin_says = (w.WAY_PLAIN, 1)
        with self.assertRaises(w.Failure) as e:
            w.cmd_write(self.link, self.NEW, Lines(), ask=no_prompt, new_front=True,
                        pins=self.pins())
        self.assertIn("BEGIN's reply announces way 1 to slot index 1, not the trial write of the "
                      'other slot the tool asked for. Nothing was written: no END was sent.',
                      str(e.exception))
        self.assertNotIn(w.END, m.seen)

    def test_trial_failed(self):
        """A trial write that fails with 9, 10 or 11: the slot's trial mark, and no trial start."""
        for st in (9, 10, 11):
            m = self.model(front=7)
            m.fail_with = st
            out = Lines()
            self.assertEqual(w.cmd_write(self.link, self.NEW, out, poll=0.01, new_front=True,
                                         ask=Ask(w.TRIAL_PHRASE), pins=self.pins()), 1)
            self.assertIn('FAILED: %s' % w.STATUS[st][0], out.text)
            if st == 9:
                self.assertNotIn('carrying the trial mark', out.text)
            else:
                self.assertIn('The failed trial write left Slot 2 carrying the trial mark: it cannot '
                              'be selected.\nSlot 2 is tried, its front has left no mark of a trial '
                              'start, and the selector trusts Slot 1, so no trial start will come: a '
                              'select is refused. Write it again with --new-front, then restart with '
                              'no key held.', out.text)
            self.assertNotIn('runs it once', out.text)
            m.close()
            self.models.remove(m)

    def test_pin_mismatch(self):
        """The descriptor's pin is not its front's own: refused before anything is sent."""
        m = self.model(front=7)
        img = make_image(seed=3, front=2, pin_ok=False)
        for cmd in (lambda out: w.cmd_write(self.link, img, out, ask=no_prompt, new_front=True),
                    lambda out: w.cmd_select(self.link, img, out, ask=no_prompt)):
            out = Lines()
            self.assertEqual(cmd(out), 1)
            self.assertIn("the file is not a build of this tree's tools. Nothing was sent.", out.text)
        self.assertEqual(m.seen, [])

    def test_trial_slot2(self):
        ask = Ask(w.TRIAL_PHRASE)
        pin = w.check_image(self.NEW)[1]['pin']
        run = w.check_image(make_image(seed=1))[1]['pin']
        m, rc, text = self.trial(ask, pins=self.pins((pin, 7)))
        self.assertEqual(rc, 0, text)
        self.assertEqual(m.flags, w.BEGIN_F_TRIAL)
        self.assertEqual(ask.prompts, ['Type "try the new front" to go on: '])
        self.assertIn('Writing %d bytes to Slot 2, for one trial start.\n'
                      "The slot's settings start at their defaults unless it held this same image.\n"
                      "The file's front is not the one this unit runs.\n"
                      '  running: front 7, pin %s...   file: pin %s..., a pin this checkout has '
                      'signed: a commit set it in src/front/front.pin (front v7)\n'
                      'Slot 2 holds no checked image, so nothing checked is lost.\n'
                      'It will be written to Slot 2 and started ONCE. If the unit does not come up, '
                      'switch it off and on: it boots Slot 1 again. Nothing is selected until a '
                      'select (wbrecover.py select IMAGE).\n'
                      'Slot 2 has two short windows: a power cut during the selector erase at the end '
                      'of this write, or in the first instant of the trial start, can leave Slot 2 '
                      'trusted before its front has run; if that front then does not start, the unit '
                      'is lost.\n' % (len(self.NEW), run[:8].hex(), pin[:8].hex()), text)
        self.assertEqual((m.slots[1], m.tried, m.sel), (self.NEW, {1}, [0xFF, 0xFF]))
        self.assertIn('Verified: Slot 2 holds this file for one trial start', text)
        self.assertIn('the selector reads ff ff.', text)
        self.assertIn('Next: restart the unit (wbrecover.py restart) with no key held and let it come '
                      'up: that start tries Slot 2 once and records itself in Slot 2. If it does not '
                      'come up, switch it off and on: Slot 1 boots again.\n'
                      'To keep the new front, select it: after that start, switch the unit off and on '
                      "and run wbrecover.py select IMAGE from Slot 1's recovery (Power + M4). Or try "
                      'holding key 4 (M4) through the restart (not yet proven on a unit): if the new '
                      "front's recovery opens, run wbrecover.py select IMAGE there. A trial start "
                      'into recovery that is not selected there records nothing: the trial write must '
                      'then be repeated.', text)
        m.seen = []
        # the same recovery takes no select after a trial write
        m.sel = [0, 1]
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, ask=no_prompt), 1)
        self.assertIn('Slot 2 is tried, but its front has left no mark of a trial start', out.text)
        m.started = {1}
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, ask=no_prompt), 1)
        self.assertIn('Refused: a trial needs the running slot checked, and neither a trial nor a select '
                      'is taken once this recovery has begun a write of the running slot, written the '
                      'selector to the other slot, or begun a trial write (S-02.084). Run info to see '
                      'the state; after a trial write, restart first.', out.text)
        self.assertNotIn(w.DATA, m.seen)
        m.started, m.sel = set(), [0xFF, 0xFF]
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('Slot 2:   tried, WB v1.3.0', out.text)
        self.assertIn('[boot slot]', out.text)
        self.assertIn('Slot 2 is tried: the next start runs it once, its trial start.', out.text)
        self.assertNotIn('trial mark', out.text)     # the select's refusal is no failed trial write

    def test_trial_slot1(self):
        """Target slot index 0: no window sentence; the selector ends {00, FE}."""
        m, rc, text = self.trial(Ask(w.TRIAL_PHRASE), running=1)
        self.assertEqual(rc, 0, text)
        self.assertIn('It will be written to Slot 1 and started ONCE. If the unit does not come up, '
                      'switch it off and on: it boots Slot 2 again.', text)
        self.assertNotIn('short windows', text)
        self.assertIn('Slot 1 holds no checked image, so nothing checked is lost.', text)
        self.assertEqual((m.slots[0], m.tried, m.sel), (self.NEW, {0}, [0, 0xFE]))
        self.assertIn('the selector reads 00 fe.', text)

    def test_trial_wrong_phrase(self):
        for answer in ('yes', 'try the new front!', ''):
            m, rc, text = self.trial(Ask(answer))
            self.assertEqual(rc, 1)
            self.assertIn('Stopped: no image byte was sent, no END, and nothing was written.', text)
            self.assertNotIn(w.DATA, m.seen)
            self.assertNotIn(w.END, m.seen)
            self.assertEqual(m.slots[1], None)
            m.close()
            self.models.remove(m)

    def test_trial_signed_words(self):
        """The signed-pin check with an injected lookup: signed, unsigned, cannot tell, and a
        front before version 7."""
        pin = w.check_image(self.NEW)[1]['pin']
        signed = 'a pin this checkout has signed: a commit set it in src/front/front.pin'
        risk = ("A front without the trial mark (any front older than version 7) can never be "
                "selected: Slot 2's image is then lost for nothing.")
        cases = [
            (self.pins((pin, 7)), signed + ' (front v7)', None),
            (self.pins((pin, None)), signed + ' (its front version not known)', risk),
            (self.pins((b'\x01' * 32, 7)), 'NOT a pin this checkout has signed',
             'WARNING: this checkout never pinned that front'),
            (self.pins((b'\x01' * 32, 7), now=pin), "NOT a pin this checkout has signed: the working "
             "tree's pin, not in a commit", 'WARNING: this checkout never pinned that front'),
            (self.pins(git=False, now=pin), 'cannot tell whether this checkout has signed it: no git '
             'history of src/front/front.pin; it is the pin this checkout has now',
             'Make sure of where the file comes from'),
            (lambda: None, 'cannot tell whether this checkout has signed it',
             'Make sure of where the file comes from'),
        ]
        for lookup, words, extra in cases:
            m, rc, text = self.trial(Ask('no'), pins=lookup)
            self.assertEqual(rc, 1)
            self.assertIn(words, text)
            if extra is None:
                self.assertNotIn('WARNING', text)
                self.assertNotIn('Make sure', text)
                self.assertNotIn('lost for nothing', text)
            else:
                self.assertIn(extra, text)
            if 'NOT' in words or 'cannot tell' in words:
                self.assertIn(risk, text)
            m.close()
            self.models.remove(m)

    def test_trial_older_front_refused(self):
        """A pin a commit set at a version before 7: the trial is refused before BEGIN."""
        pin = w.check_image(self.NEW)[1]['pin']
        m, rc, text = self.trial(no_prompt, pins=self.pins((pin, 6)))
        self.assertEqual(rc, 1)
        self.assertIn("Refused before sending: the file's front is version 6 (a commit of this "
                      'checkout set its pin at that version). A front older than version 7 leaves no '
                      'mark in its trial start and has no select of its own, so its trial could never '
                      "be kept, and Slot 2's image would be lost for nothing. Put an older front on "
                      'with the test OS installer (wiki/Test-OS.md). Nothing was sent.', text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_trial_start_image_check_failed(self):
        """In a trial start entered because the image check failed: a warning before the yes."""
        m = self.tried_unit(True)
        m.why = 2
        ask = Ask('no')
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, ask=ask), 1)
        self.assertIn('WARNING: recovery runs because the image check failed: the new front runs, but '
                      'its image does not pass its own check. Selecting it leaves a unit that starts '
                      'only into recovery.\n', out.text)
        self.assertEqual(ask.prompts, ['Type yes to go on: '])

    def info_text(self, **kw):
        """describe() of a version 7 unit, Slot 2 holding NEW: running, sel, tried, started, and
        INFO's way, phase, result, moved."""
        running, sel = kw.get('running', 0), kw.get('sel', (0, 1))
        m = self.model(front=7, slots=(make_image(seed=1), self.NEW), sel=sel, running=running)
        m.tried, m.started = {1}, set(kw.get('started', ()))
        m.way, m.phase, m.result = kw.get('way', w.WAY_NONE), kw.get('phase', w.IDLE), \
            kw.get('result', 0)
        m.moved = kw.get('moved', False)
        out = Lines()
        w.cmd_info(self.link, out)
        m.close()
        self.models.remove(m)
        return out.text

    def test_started_select_depends(self):
        """'select IMAGE keeps it' only where the old front takes a select."""
        keeps = 'Slot 2 is tried and its front marked its trial start: select IMAGE keeps it.'
        self.assertIn(keeps, self.info_text(started={1}))
        t = self.info_text(started={1}, way=w.WAY_TRIAL, phase=w.DONE)
        self.assertNotIn(keeps, t)
        self.assertIn('a select is not taken now: this recovery began a trial write, so a select '
                      'needs a restart first (status 19).', t)
        t = self.info_text(started={1}, moved=True)
        self.assertIn('a select is not taken now: this recovery wrote the selector or began a write '
                      'of Slot 1 (status 19).', t)
        t = self.info_text(started={1}, sel=(0, 0))
        self.assertIn('a select is not taken now: the selector does not read Slot 1 with verified '
                      '01.', t)
        # a failed trial write that did not reach the record: the earlier mark stands
        t = self.info_text(started={1}, way=w.WAY_TRIAL, phase=w.FAILED, result=9)
        self.assertIn("The failed trial write did not reach Slot 2's record: the earlier trial's mark "
                      'still stands for the image Slot 2 held before. A select of that image needs a '
                      'restart first (in this recovery it is refused, status 19).', t)
        self.assertNotIn('cannot be selected', t)
        self.assertNotIn(keeps, t)

    def test_failed_trial_unverified(self):
        """A failure that left the selector naming the tried slot unverified: what the next start
        does, not 'cannot be selected'."""
        t = self.info_text(sel=(1, 0), way=w.WAY_TRIAL, phase=w.FAILED, result=11)
        self.assertIn('The failed trial write left Slot 2 tried, with the selector naming it '
                      'unverified: the next start runs it once, a trial start, and a select comes '
                      'after that start (in this recovery it is refused).', t)
        self.assertNotIn('cannot be selected', t)

    def test_not_started_after_start(self):
        """Tried, not started, the selector on the old slot: a front that did not come up, or a
        trial a newer front marked spent."""
        t = self.info_text()
        self.assertIn('If the unit did not come up in that trial start, do not write the same image '
                      'again: its front does not start; rebuild it. A newer front in Slot 1 also marks '
                      'such a trial as spent on its next start, which the tool sees only as not '
                      'started: either way the trial write must be repeated to try again.', t)

    def test_running_tried_selector(self):
        """The running slot tried: an unread selector, and a torn trial start."""
        orig = w.get_info

        def unread(link, timeout=None):
            i = orig(link, timeout)
            i['selector_ok'] = False
            return i
        w.get_info = unread
        try:
            t = self.info_text(running=1, sel=(0, 1))
        finally:
            w.get_info = orig
        self.assertIn('Slot 2 is tried and running, but the selector could not be read, so the tool '
                      'cannot tell whether this is a trial start: run info again before a select.', t)
        t = self.info_text(running=1, sel=(0xFF, 0xFE))
        self.assertIn('Slot 2 is tried and running, and the selector still names it unverified '
                      '(verified fe): this trial start did not finish its selector step. The next '
                      'start is another trial start of Slot 2, and the select works there.', t)
        self.assertIn('Slot 2 is tried and running, but the selector names Slot 1 with verified 00, '
                      'not 01, so this is not a trial start', self.info_text(running=1, sel=(0, 0)))

    def test_trial_refused_damaged(self):
        """A trial needs the running slot good and no write of it begun: 19, before BEGIN."""
        m = self.model(front=7)
        m.spoiled = True
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, self.NEW, out, ask=no_prompt, new_front=True,
                                     pins=self.pins()), 1)
        self.assertIn('Refused before sending: a trial needs the running slot checked', out.text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_status_19_20_from_unit(self):
        m = self.model(front=7)
        m.begin_st = 19
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, self.NEW, out, ask=no_prompt, new_front=True,
                                     pins=self.pins()), 1)
        self.assertIn('Refused: a trial needs the running slot checked', out.text)
        self.assertNotIn(w.DATA, m.seen)
        m.begin_st = 20
        m.slots[1] = self.NEW
        m.tried, m.started = {1}, {1}
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, ask=no_prompt), 1)
        self.assertIn('Refused: no slot holds this image as tried where a select can take it', out.text)
        m.begin_st = None
        m.end_says = 20
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, poll=0.01, ask=Ask('yes')), 1)
        self.assertIn('Refused: no slot holds this image as tried', out.text)
        self.assertEqual(m.sel, [0, 1])

    def test_status_21_from_unit(self):
        """INFO shows the slot started, the unit refuses with 21 all the same: said plainly."""
        m = self.tried_unit(False)
        m.started = {1}
        m.begin_st = 21
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, ask=no_prompt), 1)
        self.assertIn('Refused: the tried slot has not started: its front has left no mark of a trial '
                      'start (S-02.084). Write it again with --new-front and restart with no key held, '
                      'so that it starts once; then switch off and on and select. Or select from its '
                      'own recovery in the trial start (try holding key 4 through the restart).',
                      out.text)
        self.assertNotIn(w.DATA, m.seen)

    def tried_unit(self, trial_start):
        """Slot 2 tried with NEW: in its trial start (running, the selector back on Slot 1), or
        after it, from the old front."""
        m = self.model(front=7, slots=(make_image(seed=1), self.NEW), sel=(0, 1),
                       running=1 if trial_start else 0)
        m.tried = {1}
        m.started = set() if trial_start else {1}
        return m

    def test_select_in_trial_start(self):
        m = self.tried_unit(True)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('Slot 2:   tried, WB v1.3.0', out.text)
        self.assertIn('This is the trial start of Slot 2: select IMAGE keeps it', out.text)
        ask = Ask('yes')
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, poll=0.01, ask=ask), 0, out.text)
        self.assertEqual(ask.prompts, ['Type yes to go on: '])
        self.assertEqual(m.flags, w.BEGIN_F_SELECT)
        self.assertIn('This is the trial start of Slot 2, the new front, and it came up.', out.text)
        self.assertEqual((m.sel, m.tried), ([1, 0], set()))
        self.assertIn('Selected: Slot 2 holds this file', out.text)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('State:    done: Slot 2 selected and read back', out.text)

    def test_select_from_old_front(self):
        """The tried slot started (per-slot bit 4): yes, and the select."""
        m = self.tried_unit(False)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn(':   tried, started, ', out.text)
        self.assertIn('Slot 2 is tried and its front marked its trial start: select IMAGE keeps it.',
                      out.text)
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, ask=Ask('no')), 1)
        self.assertNotIn(w.END, m.seen)
        self.assertEqual(m.sel, [0, 1])
        self.assertIn("Slot 2 holds the new front, tried, and the unit recorded the new front's start "
                      'in its trial.', out.text)
        self.assertNotIn('loses the unit', out.text)
        ask = Ask('yes')
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, poll=0.01, ask=ask), 0, out.text)
        self.assertEqual(ask.prompts, ['Type yes to go on: '])
        self.assertEqual((m.sel, m.tried), ([1, 0], set()))

    def test_select_not_started(self):
        """From the old front, a tried slot with no mark of a trial start: refused before BEGIN."""
        m = self.tried_unit(False)
        m.started = set()
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('Slot 2:   tried, WB', out.text)
        self.assertNotIn('started]', out.text)
        self.assertIn('Slot 2 is tried, its front has left no mark of a trial start, and the '
                      'selector trusts Slot 1, so no trial start will come: a select is refused.',
                      out.text)
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, ask=no_prompt), 1)
        self.assertIn('Refused before sending: Slot 2 is tried, but its front has left no mark of a '
                      'trial start, so a select is refused. Write it again with --new-front and '
                      'restart with no key held, so that it starts once; then switch off and on and '
                      'select. Or select from its own recovery in the trial start (try holding key 4 '
                      'through the restart). A front older than version 7 never leaves that mark: put '
                      'it on with the test OS installer (wiki/Test-OS.md). Nothing was sent.', out.text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_select_before_trial_start(self):
        """Right after the trial write the selector reads ff ff: the old front's start condition
        refuses, and the tool says the trial start comes first."""
        m = self.tried_unit(False)
        m.sel, m.started = [0xFF, 0xFF], set()
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, ask=no_prompt), 1)
        self.assertIn('Slot 2 is tried: the next start runs it once, its trial start. Restart the '
                      'unit first, with no key held, for that trial start', out.text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_tried_selector_cases(self):
        """What info says of a tried slot follows the selector, not the tried flag alone."""
        def text(running, sel, tried, way=w.WAY_NONE, phase=w.IDLE, result=0):
            m = self.model(front=7, slots=(make_image(seed=1), self.NEW) if tried == 1
                           else (self.NEW, make_image(seed=1)), sel=sel, running=running)
            m.tried, m.way, m.phase, m.result = {tried}, way, phase, result
            out = Lines()
            w.cmd_info(self.link, out)
            m.close()
            self.models.remove(m)
            return out.text
        # B = index 0, its index program failed: {FF, FE} names A unverified; the start boots A
        t = text(1, (0xFF, 0xFE), 0, w.WAY_TRIAL, w.FAILED, 11)
        self.assertIn('The failed trial write left Slot 1 carrying the trial mark: it cannot be '
                      'selected.\nSlot 1 is tried, but the selector names Slot 2 (verified fe): the '
                      'next start checks Slot 2 and boots it, so no trial start will come. Write it '
                      'again with --new-front, then restart with no key held.', t)
        self.assertNotIn('runs it once', t)
        # after status 10 or 11, the selector still {A, 01}
        for st in (10, 11):
            t = text(0, (0, 1), 1, w.WAY_TRIAL, w.FAILED, st)
            self.assertIn('so no trial start will come', t)
            self.assertNotIn('once its front has come up', t)
        # {B, 01}, B tried: every start boots it
        t = text(0, (1, 1), 1)
        self.assertIn('Slot 2 is tried, but the selector trusts it although it never passed a trial: '
                      'the next start, and every one after, boots it.', t)
        self.assertNotIn('Power + 1', t)
        # the same, from B's own recovery: not a trial start
        t = text(1, (1, 1), 1)
        self.assertNotIn('This is the trial start', t)
        self.assertIn('Slot 2 is tried, but the selector trusts it although it never passed a trial: '
                      'every start boots it, and a select is refused. From its recovery, write '
                      '--new-front IMAGE again for a trial, or a plain write replaces Slot 2.', t)
        self.assertNotIn('Power + 1', t)
        # a trial start: the running slot tried, the selector on the other slot with 01
        self.assertIn('This is the trial start of Slot 2: select IMAGE keeps it; switching off and '
                      'on starts Slot 1 again.', text(1, (0, 1), 1))

    def test_select_trusted_tried(self):
        """From the tried slot's recovery with the selector trusting it: refused before BEGIN."""
        m = self.model(front=7, slots=(make_image(seed=1), self.NEW), sel=(1, 1), running=1)
        m.tried = {1}
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, ask=no_prompt), 1)
        self.assertIn('Refused before sending: Slot 2 is tried, but the selector trusts it', out.text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_select_nothing_tried(self):
        m = self.model(front=7, slots=(make_image(seed=1), self.NEW))
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, ask=no_prompt), 1)
        self.assertIn('no slot is tried, so there is nothing to select', out.text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_select_other_file(self):
        m = self.tried_unit(False)
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, make_image(seed=4, front=2), out, ask=no_prompt), 1)
        self.assertIn('Slot 2 holds %d bytes as tried' % len(self.NEW), out.text)
        self.assertNotIn(w.BEGIN, m.seen)

    def test_select_v6(self):
        m = self.model(front=6)
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.NEW, out, ask=no_prompt), 1)
        self.assertIn('select needs a version 7 front', out.text)
        self.assertNotIn(w.BEGIN, m.seen)


class Front11(PtyCase):
    """Front version 11 (protocol 2) against the model: the slot actions, the force flag, the
    countdown; and a version 8 or 10 front, which is never sent what it does not know."""

    B = make_image(seed=2, version=(0, 9, 0, 0))
    OTHER = make_image(seed=3, front=2, version=(1, 3, 0, 0))

    def unit(self, b=None, front=11, **kw):
        """Slot 1 running and selected; Slot 2 holds b (the same front, by default). The
        models before it are closed: one model at a time reads the port."""
        for m in list(self.models):
            self.drop_model(m)
        return self.model(front=front, slots=(make_image(seed=1), self.B if b is None else b), **kw)

    def act(self, act, *answers, force=False):
        out = Lines()
        ask = Ask(*answers)
        rc = w.cmd_act(self.link, act, out, poll=0.001, ask=ask, force=force)
        return rc, out.text, ask

    @staticmethod
    def sent(m, typ):
        return [pl for t, pl in m.frames if t == typ]

    def test_same_image_by_version(self):
        """The image the kept slot holds: not sent to a front before version 14, which refuses
        it (13); written by a version 14 front, where both slots may hold the same image."""
        img = make_image(seed=1)
        m = self.unit(front=11)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.001, ask=Ask('yes')), 1)
        self.assertIn('Refused before sending: the slot that is kept already holds this image',
                      out.text)
        self.assertEqual(self.sent(m, w.BEGIN), [])
        m = self.unit(front=14)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.001, ask=Ask('yes')), 0, out.text)
        self.assertEqual(m.slots, [img, img])
        self.assertEqual(m.sel, [1, 0])

    def test_frame_bytes(self):
        """ACT's frame as the header gives it: "WR", 0x07, the sequence number, a length of 2,
        {act, flags} and the CRC-32 over type to payload's end."""
        body = bytes([0x07, 0x05, 0x02, 0x00, 0x02, 0x80])
        self.assertEqual(w.frame(w.ACT, 5, b'\x02\x80'),
                         b'WR' + body + struct.pack('<I', zlib.crc32(body)))

    def test_act_frames(self):
        """Each command's request on the wire, exactly, unforced and forced."""
        m = self.unit()
        self.assertEqual(self.act(w.ACT_TRY, 'yes')[0], 0)
        self.assertEqual(self.sent(m, w.ACT), [b'\x01\x00'])
        m = self.unit()
        self.assertEqual(self.act(w.ACT_SWITCH, w.SWITCH_PHRASE)[0], 0)
        self.assertEqual(self.sent(m, w.ACT), [b'\x02\x00'])
        m = self.unit()
        m.tried, m.started = {1}, {1}
        self.assertEqual(self.act(w.ACT_KEEP, 'yes')[0], 0)
        self.assertEqual(self.sent(m, w.ACT), [b'\x03\x00'])
        self.assertEqual((m.sel, m.tried), ([1, 0], set()))
        m = self.unit()
        m.tried = {1}
        self.assertEqual(self.act(w.ACT_PUT_BACK, 'yes')[0], 0)
        self.assertEqual(self.sent(m, w.ACT), [b'\x04\x00'])
        # forced: the flag in ACT's flags byte, only where INFO says it is needed
        m = self.unit()
        m.bad.add(1)
        rc, text, _ = self.act(w.ACT_TRY, 'force the try', force=True)
        self.assertEqual(rc, 0, text)
        self.assertEqual(self.sent(m, w.ACT), [b'\x01\x80'])
        m = self.unit()
        m.tried = {1}
        rc, text, _ = self.act(w.ACT_KEEP, 'force the keep', force=True)
        self.assertEqual(rc, 0, text)
        self.assertEqual(self.sent(m, w.ACT), [b'\x03\x80'])
        m = self.unit(b=self.OTHER)
        rc, text, _ = self.act(w.ACT_SWITCH, 'force the switch', force=True)
        self.assertEqual(rc, 0, text)
        self.assertEqual(self.sent(m, w.ACT), [b'\x02\x80'])

    def test_force_not_needed(self):
        """--force where the unit takes the action without it: the flag is not sent."""
        m = self.unit()
        rc, text, ask = self.act(w.ACT_TRY, 'yes', force=True)
        self.assertEqual(rc, 0, text)
        self.assertIn('--force is not needed: the unit takes this try without it, so it is sent '
                      'without the flag.', text)
        self.assertEqual(ask.prompts, ['Type yes to go on: '])
        self.assertEqual(self.sent(m, w.ACT), [b'\x01\x00'])

    def test_force_phrases(self):
        """Each forced request asks for its own phrase; a wrong one sends nothing past HELLO and
        INFO, and never the flag."""
        cases = [
            (w.ACT_TRY, lambda m: m.bad.add(1), 'force the try'),
            (w.ACT_SWITCH, lambda m: m.bad.add(1), 'force the switch'),
            (w.ACT_KEEP, lambda m: m.tried.add(1), 'force the keep'),
        ]
        for act, setup, phrase in cases:
            for answer in ('yes', w.SWITCH_PHRASE, phrase.upper(), phrase + '!', 'force', ''):
                with self.subTest(act=act, answer=answer):
                    m = self.unit()
                    setup(m)
                    rc, text, ask = self.act(act, answer, force=True)
                    self.assertEqual(rc, 1)
                    self.assertEqual(ask.prompts, ['Type "%s" to go on: ' % phrase])
                    self.assertIn('Stopped: nothing was sent, and nothing was written.', text)
                    self.assertEqual(m.seen, [w.HELLO, w.INFO])
                    self.drop_model(m)
            m = self.unit()
            setup(m)
            self.assertEqual(self.act(act, phrase, force=True)[0], 0)
            self.assertEqual(self.sent(m, w.ACT), [bytes([act, w.BEGIN_F_FORCE])])

    def test_refused_without_force(self):
        """Without --force a refusal INFO predicts is refused before sending; the flag is never
        sent on its own; a refusal force cannot lift is said so."""
        m = self.unit()
        m.bad.add(1)
        rc, text, _ = self.act(w.ACT_TRY)
        self.assertEqual(rc, 1)
        self.assertIn('Refused before sending: Slot 2 failed its check and is not tried: nothing checks it at a '
                      'start. Write an image to it first (wbrecover.py write IMAGE). --force lifts '
                      'this refusal, after a warning. Nothing was sent.', text)
        self.assertEqual(m.seen, [w.HELLO, w.INFO])
        m = self.unit()
        m.sel = [1, 1]
        rc, text, _ = self.act(w.ACT_SWITCH, force=True)
        self.assertEqual(rc, 1)
        self.assertIn('Refused before sending, even with --force: Slot 2 is already the boot '
                      'slot (verified 01). Nothing to do', text)
        self.assertEqual(m.seen, [w.HELLO, w.INFO])
        m = self.unit()
        rc, text, _ = self.act(w.ACT_PUT_BACK)
        self.assertIn('Slot 2 is not tried: there is no trial mark to take off. Nothing to do.', text)
        self.assertEqual(m.seen, [w.HELLO, w.INFO])

    def test_switch_phrase(self):
        """A switch asks for its phrase, not yes; a wrong answer sends nothing."""
        for answer in ('yes', 'y', 'switch', ''):
            m = self.unit()
            rc, text, ask = self.act(w.ACT_SWITCH, answer)
            self.assertEqual(rc, 1)
            self.assertEqual(ask.prompts, ['Type "make it the boot slot" to go on: '])
            self.assertEqual(m.seen, [w.HELLO, w.INFO])
            self.drop_model(m)
        m = self.unit()
        rc, text, _ = self.act(w.ACT_SWITCH, w.SWITCH_PHRASE)
        self.assertEqual(rc, 0, text)
        self.assertEqual(m.sel, [1, 0])
        self.assertIn('Done: Slot 2 is the boot slot (the selector reads 01 00).', text)

    SAME = ('Slot 2 holds an image with the front this unit runs (version 0.9.0). A switch makes it '
            'the boot slot: every start boots Slot 2 from now on. A try starts it once first: wbrecover.py '
            'try.')
    FRONT = ('WARNING: Slot 2 holds another waffle-boot front (version 1.3.0). Once switched, recovery '
             'may not be reachable from it: if that front does not start, or its recovery is not this '
             'one, there may be no way back to Slot 1. A try starts it once first: wbrecover.py try.')
    FOREIGN = ('WARNING: Slot 2 holds no waffle-boot front. Once switched, there is no recovery from '
               'there: Power + M4 does nothing, and nothing on the unit brings waffle-boot back. A try '
               'starts it once first: wbrecover.py try.')
    BAD = ('DANGER: Slot 2 failed its check: its record does not match its bytes. Switched to, the '
           'selector trusts it, and nothing will ever check this slot again: if its image does not start, there may be no way '
           'back without a flash programmer. A try starts it once first: wbrecover.py try --force.')

    def test_tier_warnings(self):
        """The switch's warning by the tier INFO reports for the other slot."""
        for b, bad, force, want, others in (
                (None, False, False, self.SAME, ('WARNING', 'DANGER')),
                (self.OTHER, False, True, self.FRONT, ('DANGER', 'no waffle-boot front')),
                (foreign_image(), False, True, self.FOREIGN, ('DANGER', 'another waffle-boot')),
                (None, True, True, self.BAD, ('WARNING',))):
            with self.subTest(want=want[:20]):
                m = self.unit(b=b)
                if bad:
                    m.bad.add(1)
                rc, text, _ = self.act(w.ACT_SWITCH, 'no', force=force)
                self.assertEqual(rc, 1)
                self.assertIn(want, text)
                for word in others:
                    self.assertNotIn(word, text)
                self.assertEqual(m.seen, [w.HELLO, w.INFO])
                self.drop_model(m)

    def test_tier_from_info(self):
        """The warning follows INFO's tier byte, not the tool's own reading of the slots."""
        self.unit()
        orig = w.get_info

        def says_bad(link, timeout=None):
            i = orig(link, timeout)
            i['tier'] = w.TIER_BAD
            return i
        with mock.patch.object(w, 'get_info', says_bad):
            rc, text, _ = self.act(w.ACT_SWITCH, 'no')
        self.assertEqual(rc, 1)
        self.assertIn(self.BAD.split(' A try')[0], text)

    def test_try_tiers(self):
        """A try says how the trial start comes back, by tier."""
        m = self.unit(b=foreign_image())
        rc, text, _ = self.act(w.ACT_TRY, 'no')
        self.assertIn('Slot 2 holds no waffle-boot front: there is no recovery in that start. Switch '
                      'the unit off and on to come back to Slot 1.', text)
        self.assertIn('Slot 2 has two short windows', text)
        m = self.unit(b=self.OTHER)
        rc, text, _ = self.act(w.ACT_TRY, 'no')
        self.assertIn('Slot 2 holds another waffle-boot front: its recovery may not open in that '
                      'start.', text)

    def test_countdown(self):
        """An accepted ACT: the tool says the unit counts down, what stops it, the seconds left
        from INFO, and waits it out: phase 5 in 15 INFOs, then the action."""
        m = self.unit()
        rc, text, _ = self.act(w.ACT_TRY, 'yes')
        self.assertEqual(rc, 0, text)
        self.assertIn('Once confirmed, the unit counts down 15 s before its first erase: switching it '
                      'off or unplugging USB in that time stops it, with the flash untouched.', text)
        self.assertIn(w.COUNTDOWN_LINE, text)
        self.assertIn('stopping this tool does not', text)
        counting = [c for ph, c in m.phases if ph == w.COUNTING]
        self.assertEqual(counting, list(range(15, 0, -1)))
        for k in range(15, 0, -1):
            self.assertIn('\rCountdown: %2d s left' % k, text)
        self.assertLess(text.index('Countdown:  1 s left'), text.index('Writing: 100.0%'))
        self.assertEqual((m.sel, m.tried), ([0xFF, 0xFF], {1}))
        self.assertIn('Done: Slot 2 is marked for one trial start; the selector reads ff ff.', text)
        # the same for a write
        m = self.unit()
        out = Lines()
        img = make_image(seed=7)
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.001, ask=Ask('yes')), 0, out.text)
        self.assertEqual([c for ph, c in m.phases if ph == w.COUNTING], list(range(15, 0, -1)))
        self.assertIn('Countdown:  1 s left', out.text)
        self.assertIn('Verified: Slot 2 holds this file', out.text)

    def test_countdown_timeout(self):
        """The wait allows the countdown on top of the write's own time."""
        m = self.unit()
        m.hold = True
        out = Lines()
        rc = w.cmd_act(self.link, w.ACT_TRY, out, poll=0.001, deadline=0.05, ask=Ask('yes'))
        self.assertEqual(rc, 1)
        self.assertIn('The write did not finish in 15 s; run info.', out.text)
        self.assertIn('Countdown:  1 s left', out.text)

    def test_no_countdown_when_held(self):
        """The damaged state holds the power-off: the unit begins at once, and the tool says so."""
        m = self.unit()
        m.tried, m.held = {1}, True
        rc, text, _ = self.act(w.ACT_PUT_BACK, 'yes')
        self.assertEqual(rc, 0, text)
        self.assertIn('The unit begins at once, with no countdown: it holds the power-off while a '
                      'write of this recovery has left no checked slot to start.', text)
        self.assertIn('The unit began at once, with no countdown', text)
        self.assertNotIn('Countdown', text)
        self.assertFalse([ph for ph, _ in m.phases if ph == w.COUNTING])

    def test_countdown_stopped(self):
        """The host left during the countdown: INFO shows status 24, and the tool says plainly that
        it was stopped and nothing was written."""
        m = self.unit()
        m.drop_at = 9
        rc, text, _ = self.act(w.ACT_TRY, 'yes')
        self.assertEqual(rc, 1)
        self.assertIn('Countdown: 10 s left\nStopped: the write was stopped before its first erase: the '
                      'unit was switched off or USB was unplugged, and nothing was written. Run info, '
                      'then start the command again if it is still wanted.', text)
        self.assertEqual((m.sel, m.tried), ([0, 1], set()))
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('State:    failed: the write was stopped before its first erase', out.text)
        # after a write of this recovery, a stop leaves that write in INFO (S-02.093)
        m = self.unit()
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=7), out, poll=0.001,
                                     ask=Ask('yes')), 0, out.text)
        m.sel, m.drop_at = [0, 1], 5
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=8), out, poll=0.001,
                                     ask=Ask('yes')), 1)
        self.assertIn('Stopped: the write was stopped before its first erase: the unit was switched off '
                      'or USB was unplugged, and nothing was written. Run info, then start the '
                      'command again if it is still wanted. The unit still reports the write it made '
                      'earlier in this recovery (Slot 2 written and read back); nothing of this one '
                      'was written.', out.text)
        self.assertNotIn('VERIFIED', out.text)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('State:    done: Slot 2 written and read back', out.text)
        self.assertIn('Since:    the next write or action: the write was stopped before its first '
                      'erase', out.text)

    def test_second_action_stopped(self):
        """A second action of the same way, stopped in its countdown, after the first was done:
        reported as stopped (byte 231), never as done."""
        m = self.unit()
        m.tried = {1}
        self.assertEqual(self.act(w.ACT_TRY, 'yes')[0], 0)
        self.assertEqual(m.sel, [0xFF, 0xFF])
        m.sel, m.drop_at = [0, 1], 3            # a restart later, as far as the model goes
        rc, text, _ = self.act(w.ACT_TRY, 'yes')
        self.assertEqual(rc, 1, text)
        self.assertIn('Stopped: the write was stopped before its first erase', text)
        self.assertIn('The unit still reports the write it made earlier in this recovery (Slot 2 '
                      'marked for one trial start)', text)
        self.assertNotIn('Done:', text)
        i = w.get_info(self.link)
        self.assertEqual((i['phase'], i['way'], i['act'], i['refused']),
                         (w.DONE, w.WAY_TRY, w.ACT_TRY, 24))

    def test_refused_after_write_in_info(self):
        """A request refused after a write of this recovery: info shows the write, and the
        refusal after it."""
        m = self.unit()
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=7), out, poll=0.001,
                                     ask=Ask('yes')), 0, out.text)
        r = self.link.request(w.ACT, bytes([w.ACT_TRY, 0]))[0]
        self.assertEqual(r[0], 3)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('State:    done: Slot 2 written and read back', out.text)
        self.assertIn('Since:    the last request after it was refused: restart before another '
                      'write', out.text)
        # an accepted request clears it
        m.sel = [0, 1]
        self.assertEqual(self.act(w.ACT_TRY, 'yes')[0], 0)
        self.assertEqual(w.get_info(self.link)['refused'], 0)

    def test_port_gone_in_countdown(self):
        """The port goes away during the countdown: what the unit does then, in plain words."""
        self.unit()
        orig, calls = w.get_info, []

        def gone(link, timeout=None):
            calls.append(1)
            if len(calls) > 4:
                raise w.Failure('the port went away: was the unit restarted or unplugged?')
            return orig(link, timeout)
        with mock.patch.object(w, 'get_info', gone), self.assertRaises(w.Failure) as e:
            self.act(w.ACT_TRY, 'yes')
        self.assertIn('The unit was counting down: if it was switched off or USB was unplugged, it '
                      'stopped, and nothing was written (status 24); else it goes on when the '
                      'countdown ends.', str(e.exception))

    def test_keep_and_put_back(self):
        """keep from the old front (the other slot tried and started), keep in a trial start (the
        running slot tried), put-back of a try not yet started: verified from INFO."""
        m = self.unit()
        m.tried, m.started = {1}, {1}
        rc, text, _ = self.act(w.ACT_KEEP, 'yes')
        self.assertEqual(rc, 0, text)
        self.assertIn('Keeping Slot 2.\nSlot 2 is tried; the keep makes it the boot slot', text)
        self.assertIn('Kept: Slot 2 is the boot slot, to be checked again at the next start.', text)
        m = self.unit(running=1, sel=(0, 1))
        m.tried = {1}
        rc, text, _ = self.act(w.ACT_KEEP, 'yes')
        self.assertEqual(rc, 0, text)
        self.assertIn('This is the trial start of Slot 2. The keep makes Slot 2 the boot slot',
                      text)
        self.assertEqual((m.sel, m.tried), ([1, 0], set()))
        m = self.unit(sel=(0xFF, 0xFF))
        m.tried = {1}
        rc, text, _ = self.act(w.ACT_PUT_BACK, 'yes')
        self.assertEqual(rc, 0, text)
        self.assertIn('The selector names Slot 2 for a try that has not started; it is first settled '
                      'back on Slot 1.', text)
        self.assertEqual((m.sel, m.tried), ([0, 1], set()))
        self.assertIn("Put back: Slot 2's record is good again and it is no longer tried; the "
                      'selector reads 00 01.', text)

    def test_act_not_verified(self):
        """A unit that reports a try done with the selector not as the trial leaves it."""
        m = self.unit()
        orig = m.act_ends

        def no_selector():
            orig()
            m.sel = [0, 1]
        m.act_ends = no_selector
        rc, text, _ = self.act(w.ACT_TRY, 'yes')
        self.assertEqual(rc, 1)
        self.assertIn('NOT VERIFIED: what the unit reports of Slot 2 does not match: the selector, '
                      'which should read ff ff.', text)

    def test_info_device(self):
        """info from front 20: the device and how it was decided, n and c when not identified."""
        m = self.unit(b=self.OTHER)
        m.front = 20
        self.assertEqual(len(m.info()), w.INFO_V20)
        i = w.parse_info(m.info())
        self.assertEqual((i['v20'], i['device'], i['dev_how'], i['fs_n'], i['fs_c']),
                         (True, 0, 0, 0x86C0, 0xF3C93780))
        out = Lines()
        self.assertEqual(w.cmd_info(self.link, out), 0)
        self.assertIn('Front version 20, protocol 2.\nRecovery runs because ', out.text)
        self.assertIn('.\nDevice:   OP-XY, identified by its first stage\n', out.text)
        m.device, m.dev_how, m.fs = 0, 2, (34000, 0x12345678)
        out = Lines()
        self.assertEqual(w.cmd_info(self.link, out), 0)
        self.assertIn('Device:   not identified (first stage n 34000, c 12345678): recovery runs as on '
                      'the OP-XY\n', out.text)
        m.device, m.dev_how = 1, 0
        self.assertEqual(w.device_line(w.parse_info(m.info())),
                         'Device:   OP-1F, identified by its first stage')
        # each defined way by its name (front/handoff.h's FR_HO_HOW_*), and one it does not know
        m.dev_how = 1
        self.assertEqual(w.device_line(w.parse_info(m.info())),
                         'Device:   OP-1F, identified by the second CPU (first stage n 34000, c 12345678)')
        m.dev_how = 7
        self.assertEqual(w.device_line(w.parse_info(m.info())),
                         'Device:   OP-1F, decided by way 7, which this tool does not know (first stage '
                         'n 34000, c 12345678)')
        # an older front: no line
        m.front = 11
        self.assertIsNone(w.parse_info(m.info())['device'])
        out = Lines()
        self.assertEqual(w.cmd_info(self.link, out), 0)
        self.assertNotIn('Device:', out.text)

    def test_info(self):
        """info from front 11: the other slot's tier, each action now and with --force, a
        countdown under way."""
        m = self.unit(b=self.OTHER)
        self.assertEqual(len(m.info()), w.INFO_V11)
        out = Lines()
        self.assertEqual(w.cmd_info(self.link, out), 0)
        self.assertIn('Front version 11, protocol 2.', out.text)
        self.assertIn('Other:    Slot 2 holds another waffle-boot front: its recovery may not be this '
                      'one, or may not open\n', out.text)
        self.assertIn('Try:      would start Slot 2 once, at the next start\n', out.text)
        self.assertIn('Switch:   with --force only (Slot 2 does not hold the front this unit runs: a '
                      'switch to it would make it the boot slot with no trial): would make Slot 2 the '
                      'boot slot\n',
                      out.text)
        self.assertIn('Keep:     refused: no slot is tried where a keep can take it', out.text)
        self.assertIn('Put back: refused: Slot 2 is not tried', out.text)
        self.assertNotIn('keep is the action', out.text)
        m.tried, m.started = {1}, {1}
        m.phase, m.count, m.act = w.COUNTING, 7, w.ACT_KEEP
        out = Lines()
        w.cmd_info(self.link, out)
        # Slot 2 came up from its trial: its keep, not the switch, is the action
        self.assertRegex(out.text, r'Switch: .*; after its trial, keep is the action\n')
        self.assertIn(':   tried, started, ', out.text)
        self.assertIn('State:    counting down: 7 s left before the first erase of the keep of Slot 2. '
                      'Switching the unit off or unplugging USB now stops it, with nothing written',
                      out.text)
        self.assertIn('Slot 2 is tried and its front marked its trial start: wbrecover.py keep (or '
                      'select IMAGE) keeps it.', out.text)

    def test_no_chords(self):
        """The hints name the slot commands and the recovery menu, never a key chord."""
        self.model(front=11, sel=(1, 1), running=1, slots=(make_image(seed=1), None))
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertNotIn('Power + 1', out.text)
        self.assertNotIn('Power + 2', out.text)
        self.assertIn('wbrecover.py try starts it once, wbrecover.py switch makes it the boot slot; or key 1, '
                      '"Change the boot slot", in the recovery menu.', out.text)
        for front in (8, 10, 11):
            self.assertNotIn('Power + 1', w.status_text(5, i={'front': front, 'spoiled': False}))

    def test_after_write_wording(self):
        """Statuses 3 and 4 once this recovery has made a write: restart before another one."""
        m = self.unit()
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=7), out, poll=0.001,
                                     ask=Ask('yes')), 0, out.text)
        self.assertEqual(m.sel, [1, 0])
        for cmd in (lambda o: w.cmd_write(self.link, make_image(seed=8), o, ask=no_prompt),
                    lambda o: w.cmd_act(self.link, w.ACT_TRY, o, ask=no_prompt)):
            out = Lines()
            self.assertEqual(cmd(out), 1)
            self.assertIn('restart before another write: this recovery has made a write, and the '
                          'unit takes the next one only after a start. Restart the unit '
                          '(wbrecover.py restart) with no key held, then hold Power + M4 again.',
                          out.text)
            self.assertNotIn('the running slot is not the slot the selector names', out.text)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('Write:    refused: restart before another write', out.text)
        # status 4: a write of the running slot leaves the selector {A, 00}
        m = self.unit()
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=7), out, poll=0.001, running=True,
                                     ask=Ask('yes')), 0, out.text)
        self.assertEqual(m.sel, [0, 0])
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=8), out, ask=no_prompt), 1)
        self.assertIn('Refused before sending: restart before another write', out.text)
        # and from the unit's own reply to a request
        i = w.get_info(self.link)
        for st in (3, 4):
            self.assertTrue(w.status_text(st, i=i).startswith('restart before another write'))
        # with no write made, the selector is described as before
        self.unit(sel=(1, 0))
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=8), out, ask=no_prompt), 1)
        self.assertIn('the running slot is not the slot the selector names', out.text)

    def test_forced_write(self):
        """write --force: another front with no trial, and a stream with no waffle-boot front;
        the force phrase before BEGIN, then the write's own confirmation."""
        m = self.unit(b=None)
        m.slots[1] = None
        ask = Ask('force the write', 'yes')
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, self.OTHER, out, poll=0.001, ask=ask, force=True,
                                     pins=no_prompt), 0, out.text)
        self.assertEqual(ask.prompts, ['Type "force the write" to go on: ', 'Type yes to go on: '])
        self.assertIn(w.WRITE_RISK['front'], out.text)
        self.assertEqual(self.sent(m, w.BEGIN)[0][36], w.BEGIN_F_FORCE)
        self.assertEqual((m.slots[1], m.sel), (self.OTHER, [1, 0]))
        # a stream with no waffle-boot front: only with --force
        img = foreign_image(seed=4)
        m = self.unit(b=None)
        m.slots[1] = None
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, img, out, ask=no_prompt), 1)
        self.assertIn('The file is not a slot image recovery writes; nothing was sent.', out.text)
        self.assertEqual(m.seen, [])
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.001, force=True,
                                     ask=Ask('force the write', 'yes')), 0, out.text)
        self.assertIn(w.WRITE_RISK['none'], out.text)
        self.assertEqual(m.slots[1], img)
        # a wrong phrase: no BEGIN at all
        m = self.unit(b=None)
        m.slots[1] = None
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, self.OTHER, out, ask=Ask('yes'), force=True,
                                     pins=no_prompt), 1)
        self.assertEqual(m.seen, [w.HELLO, w.INFO])
        # with nothing to lift: no flag
        m = self.unit()
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=7), out, poll=0.001, force=True,
                                     ask=Ask('yes')), 0, out.text)
        self.assertIn('--force is not needed: the unit takes this write without it', out.text)
        self.assertEqual(len(self.sent(m, w.BEGIN)[0]), 36)

    def test_forced_write_selector(self):
        """write --force over a selector a write of this recovery left (status 3): the unit's
        BEGIN answer says where it goes."""
        m = self.unit(sel=(1, 0))
        ask = Ask('force the write', 'yes')
        out = Lines()
        img = make_image(seed=8)
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.001, ask=ask, force=True), 0,
                         out.text)
        self.assertIn(w.WRITE_RISK[3], out.text)
        self.assertEqual(self.sent(m, w.BEGIN)[0][36], w.BEGIN_F_FORCE)
        self.assertIn('Writing %d bytes to Slot 1.' % len(img), out.text)
        self.assertEqual(m.slots[0], img)

    def test_forced_after_slot1(self):
        """S-02.090: after a write of Slot 1 in this recovery the selector reads 00 00; a forced
        second write is refused before sending, with a restart asked first."""
        m = self.unit(sel=(1, 1), running=1)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=7), out, poll=0.001,
                                     ask=Ask('yes')), 0, out.text)
        self.assertEqual(list(m.sel), [0, 0])
        self.assertTrue(w.slot1_window(w.get_info(self.link)))
        begins = len(self.sent(m, w.BEGIN))
        out = Lines()
        ask = Ask('force the write', 'yes')
        self.assertEqual(w.cmd_write(self.link, make_image(seed=8), out, poll=0.001, ask=ask,
                                     force=True), 1)
        self.assertIn(w.FORCE_AFTER_SLOT1, out.text)
        self.assertEqual(ask.prompts, [])
        self.assertEqual(len(self.sent(m, w.BEGIN)), begins)
        # a write of Slot 2 leaves 01 00: not this window
        self.unit()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=7), Lines(), poll=0.001,
                                     ask=Ask('yes')), 0)
        self.assertFalse(w.slot1_window(w.get_info(self.link)))

    def test_forced_write_selected_bad(self):
        """write --force with the selector naming a bad Slot 2: the unit refuses it with status 5,
        forced too (S-02.084), so the tool refuses before sending."""
        m = self.unit(sel=(1, 1))
        m.bad.add(1)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=8), out, poll=0.001, force=True,
                                     ask=Ask('force the write', 'yes')), 1)
        self.assertIn('Refused before sending, even with --force: the running slot is not the slot '
                      'the selector names, and that slot failed its check (status 5). Nothing was sent.',
                      out.text)
        self.assertEqual(m.seen, [w.HELLO, w.INFO])
        self.assertEqual(m.slots[0], make_image(seed=1))

    def test_port_gone_in_transfer(self):
        """The port goes away in the middle of a transfer: the tool says where it stopped and
        that nothing was written; the unit ends the transfer (status 24), and the command run
        again once the port is back begins anew and writes the file."""
        m = self.unit()
        m.hangup_after = 2
        img = make_image(seed=8, size=5000)
        with self.assertRaises(w.Failure) as e:
            w.cmd_write(self.link, img, Lines(), poll=0.001, ask=Ask('yes'))
        self.assertIn('the port went away', str(e.exception))
        self.assertIn('The transfer stopped at byte 2048 of %d, before END: nothing was written. '
                      'Once the unit is back, run the command again: it begins anew.' % len(img),
                      str(e.exception))
        self.assertEqual(m.slots[1], self.B)
        # the port comes back
        self.link.close()
        self.master, slave = os.openpty()
        self.link = w.Link(os.ttyname(slave), timeout=0.3, retries=4, long_timeout=1.0)
        os.close(slave)
        m.fd = self.master
        out = Lines()
        self.assertEqual(w.cmd_info(self.link, out), 0)
        self.assertIn('State:    failed: the write was stopped before its first erase', out.text)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.001, ask=Ask('yes')), 0, out.text)
        self.assertEqual(m.slots[1], img)

    def test_forced_restart(self):
        """restart --force in the damaged state: the warning, its phrase, RESTART {0x80}; a wrong
        phrase sends no RESTART; without --force RESTART goes as before, with no payload."""
        m = self.unit()
        m.spoiled, m.held, m.slots[0] = True, True, None
        out = Lines()
        self.assertEqual(w.cmd_restart(self.link, out, ask=Ask('yes'), force=True), 1)
        self.assertNotIn(w.RESTART, m.seen)
        self.assertIn('Without --force the unit refuses this: the unit refuses a restart', out.text)
        self.assertIn(w.RESTART_RISK, out.text)
        out = Lines()
        self.assertEqual(w.cmd_restart(self.link, out, ask=Ask('force the restart'), force=True), 0,
                         out.text)
        self.assertEqual(self.sent(m, w.RESTART), [b'\x80'])
        self.assertTrue(m.restarted)
        m = self.unit()
        self.assertEqual(w.cmd_restart(self.link, Lines(), ask=no_prompt), 0)
        self.assertEqual(self.sent(m, w.RESTART), [b''])

    def test_forced_select(self):
        """select --force lifts 21 (no mark of a trial start) after the phrase."""
        m = self.model(front=11, slots=(make_image(seed=1), self.OTHER))
        m.tried = {1}
        out = Lines()
        self.assertEqual(w.cmd_select(self.link, self.OTHER, out, ask=no_prompt), 1)
        self.assertIn('has left no mark of a trial start', out.text)
        out = Lines()
        ask = Ask('force the select', 'yes')
        self.assertEqual(w.cmd_select(self.link, self.OTHER, out, poll=0.001, ask=ask, force=True),
                         0, out.text)
        self.assertIn(w.SELECT_RISK[21], out.text)
        self.assertEqual(self.sent(m, w.BEGIN)[-1][36], w.BEGIN_F_SELECT | w.BEGIN_F_FORCE)

    def test_v8_new_commands(self):
        """A version 8 or 10 front (protocol 1): the slot commands and --force are refused by the
        tool, with nothing sent but HELLO."""
        for front in (8, 10):
            for act in (w.ACT_TRY, w.ACT_SWITCH, w.ACT_KEEP, w.ACT_PUT_BACK):
                m = self.unit(front=front)
                rc, text, _ = self.act(act, force=act == w.ACT_SWITCH)
                self.assertEqual(rc, 1)
                self.assertIn('This unit runs front version %d: %s needs a version 11 front. Nothing '
                              'was sent.' % (front, w.ACT_NAMES[act]), text)
                self.assertEqual(m.seen, [w.HELLO])
                self.drop_model(m)
            for cmd in (lambda o: w.cmd_write(self.link, make_image(seed=7), o, ask=no_prompt,
                                              force=True),
                        lambda o: w.cmd_select(self.link, self.OTHER, o, ask=no_prompt, force=True),
                        lambda o: w.cmd_restart(self.link, o, ask=no_prompt, force=True)):
                m = self.unit(front=front)
                out = Lines()
                self.assertEqual(cmd(out), 1)
                self.assertIn('This unit runs front version %d: --force needs a version 11 front. '
                              'Nothing was sent.' % front, out.text)
                self.assertEqual(m.seen, [w.HELLO])
                self.drop_model(m)

    def test_v8_frames(self):
        """Against a version 8 or 10 front the old commands send what they always did: BEGIN
        without the force bit, RESTART with no payload; INFO of 219 bytes; no countdown."""
        for front in (8, 10):
            m = self.unit(front=front)
            out = Lines()
            self.assertEqual(w.cmd_info(self.link, out), 0)
            self.assertIn('Front version %d, protocol 1.' % front, out.text)
            self.assertNotIn('Try:', out.text)
            self.assertEqual(len(m.info()), w.INFO_V6)
            out = Lines()
            self.assertEqual(w.cmd_write(self.link, make_image(seed=7), out, poll=0.001,
                                         ask=Ask('yes')), 0, out.text)
            self.assertNotIn('ountdown', out.text)
            self.assertEqual([len(pl) for pl in self.sent(m, w.BEGIN)], [36])
            self.assertEqual(w.cmd_restart(self.link, Lines()), 0)
            self.assertEqual(self.sent(m, w.RESTART), [b''])
            self.assertNotIn(w.ACT, m.seen)
            # the model's older front answers what it does not know as malformed
            self.assertEqual(self.link.request(w.ACT, b'\x01\x00')[0], b'\x40')
            self.drop_model(m)

    def test_protocol_mismatch(self):
        """A front that names a protocol its version does not speak: refused, nothing more sent."""
        m = self.unit()
        orig = w.parse_hello

        def proto1(p):
            h = orig(p)
            h['proto'] = 1
            return h
        with mock.patch.object(w, 'parse_hello', proto1), self.assertRaises(w.Failure) as e:
            w.cmd_info(self.link, Lines())
        self.assertIn('the unit speaks recovery protocol 1 with front version 11', str(e.exception))
        self.assertEqual(m.seen, [w.HELLO])

    def drop_model(self, m):
        m.close()
        self.models.remove(m)


class Settings(PtyCase):
    """S-02.097, S-02.104: a write says once that the slot's settings start at their defaults
    unless it held the same image, for either slot and every front version; an image over 0x76000
    bytes is refused before anything is sent."""

    def write(self, img, ask, **kw):
        out = Lines()
        rc = w.cmd_write(self.link, img, out, poll=0.001, ask=ask, **kw)
        return rc, out.text

    def test_settings_line(self):
        for front in (5, 6, 8, 11):
            with self.subTest(front=front):
                m = self.model(front=front)
                img = make_image(seed=7)
                rc, text = self.write(img, Ask('yes') if front > 5 else no_prompt)
                self.assertEqual(rc, 0, text)
                self.assertEqual(m.slots[1], img)
                self.assertEqual(text.count(w.SETTINGS_LINE), 1)
                self.assertIn('Writing %d bytes to Slot 2.\n%s\n' % (len(img), w.SETTINGS_LINE),
                              text)
                m.close()
                self.models.remove(m)
        m = self.model(front=11, slots=(None, make_image(seed=1)), sel=(1, 1), running=1)
        img = make_image(seed=7)
        rc, text = self.write(img, Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertEqual(m.slots[0], img)
        self.assertIn('Writing %d bytes to Slot 1.\n%s\n' % (len(img), w.SETTINGS_LINE), text)

    def test_large_image_refused(self):
        img = image_of(w.WB_IMAGE_MAX + 1, seed=7)
        for front in (5, 11):
            with self.subTest(front=front):
                m = self.model(front=front)
                ask = Ask('yes')
                rc, text = self.write(img, ask if front > 5 else no_prompt)
                self.assertEqual(rc, 1, text)
                self.assertIn('nothing was sent', text)
                self.assertEqual(ask.prompts, [])
                self.assertNotEqual(m.slots[1], img)
                m.close()
                self.models.remove(m)


def put(d, name, text):
    path = os.path.join(d, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)


def pin_text(pin):
    return '%s 1234\n' % pin.hex()


def proto_text(v):
    return '#define FR_FRONT_VERSION  %du\t/* x */\n' % v


def header(name):
    """{name: value} of a front header's plain number and string #defines, and of its enums:
    {enum name: [member names in order]} under the key 'enum <name>'."""
    with open(os.path.join(TOP, 'src', 'front', name)) as f:
        text = f.read()
    out = {}
    for m in re.finditer(r'^#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)u?\b', text, re.M):
        out[m.group(1)] = int(m.group(2), 0)
    for m in re.finditer(r'^#define\s+(\w+)\s+"([^"]*)"', text, re.M):
        out[m.group(1)] = m.group(2)
    for m in re.finditer(r'^#define\s+(\w+)\((\w+)\)\s+\((\d+)u \+ (\w+) \* \(\2\)\)', text, re.M):
        out[m.group(1)] = (int(m.group(3)), m.group(4))
    code = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    for m in re.finditer(r'\benum\s+(\w+)\s*\{([^}]*)\}', code):
        out['enum ' + m.group(1)] = re.findall(r'(?:^|,)\s*([A-Za-z_]\w*)', m.group(2))
    return out


class AgreesWithHeaders(unittest.TestCase):
    """The tool's protocol constants are the front's headers'."""

    @classmethod
    def setUpClass(cls):
        cls.h = {}
        for name in ('recovery/proto.h', 'slots/slots.h', 'recovery/usb.h', 'handoff.h'):
            cls.h.update(header(name))

    def same(self, pairs):
        for c, mine in pairs:
            self.assertIn(c, self.h)
            self.assertEqual(self.h[c], mine, c)

    def test_frames(self):
        h = self.h
        self.same([('FR_PROTO_VERSION', w.PROTO), ('FR_PROTO_MAX_DATA', w.MAX_DATA),
                   ('FR_PROTO_HEAD', w.HEAD), ('FR_T_HELLO', w.HELLO), ('FR_T_INFO', w.INFO),
                   ('FR_T_BEGIN', w.BEGIN), ('FR_T_DATA', w.DATA), ('FR_T_END', w.END),
                   ('FR_T_RESTART', w.RESTART), ('FR_T_ACT', w.ACT), ('FR_T_REPLY', w.REPLY),
                   ('FR_BEGIN_REPLY_LEN', w.BEGIN_REPLY_LEN), ('FR_DATA_REPLY_LEN', w.DATA_REPLY_LEN),
                   ('FR_ACT_REPLY_LEN', w.ACT_REPLY_LEN),
                   ('FR_END_REPLY_LEN', w.END_REPLY_LEN), ('FR_NO_SLOT', w.NO_SLOT),
                   ('FR_SLOT_CAP', w.SLOT_CAP), ('FR_LOG_IMAGE_MAX', w.IMAGE_MAX),
                   ('FR_USB_VID', w.VID), ('FR_USB_PID', w.PID), ('FR_USB_PRODUCT', w.PRODUCT),
                   ('FR_USB_PID_OP1F', w.PID_1F), ('FR_USB_PRODUCT_OP1F', w.PRODUCT_1F)])
        self.assertEqual(w.PORTS, ((w.PID, w.PRODUCT), (w.PID_1F, w.PRODUCT_1F)))
        # FR_PROTO_MAX is FR_PROTO_MAX_DATA + 8u, a sum the number pattern does not take
        self.assertIn('#define FR_PROTO_MAX      (FR_PROTO_MAX_DATA + 8u)',
                      open(os.path.join(TOP, 'src', 'front', 'recovery', 'proto.h')).read())
        self.assertEqual(w.MAX_PAYLOAD, h['FR_PROTO_MAX_DATA'] + 8)
        self.assertEqual(w.FRAME_MAX, h['FR_PROTO_HEAD'] + w.MAX_PAYLOAD + 4)

    def test_hello(self):
        self.same([('FR_HELLO_LEN', w.HELLO_LEN), ('FR_HELLO_O_VERSION', w.HELLO_O_VERSION),
                   ('FR_HELLO_O_PIN', w.HELLO_O_PIN)])
        # parse_hello()'s '<BHHBB' at 0: proto, front, max_data, why, flags
        offs = [0, 1, 3, 5, 6]
        self.assertEqual([self.h[n] for n in ('FR_HELLO_O_PROTO', 'FR_HELLO_O_FRONT',
                                              'FR_HELLO_O_MAX_DATA', 'FR_HELLO_O_WHY',
                                              'FR_HELLO_O_FLAGS')], offs)
        self.assertEqual([struct.calcsize('<' + 'BHHBB'[:k]) for k in range(5)], offs)

    def test_info(self):
        h = self.h
        self.same([('FR_INFO_LEN', w.INFO_V20), ('FR_INFO_O_COUNT', w.INFO_V6),
                   ('FR_INFO_O_DEVICE', w.INFO_O_DEVICE), ('FR_INFO_O_DEV_HOW', w.INFO_O_DEV_HOW),
                   ('FR_INFO_O_FS_N', w.INFO_O_FS_N), ('FR_INFO_O_FS_C', w.INFO_O_FS_C),
                   ('FR_INFO_O_COUNT', w.INFO_O_COUNT), ('FR_INFO_O_ACT', w.INFO_O_ACT),
                   ('FR_INFO_O_TIER', w.INFO_O_TIER), ('FR_INFO_O_ACTS', w.INFO_O_ACTS),
                   ('FR_INFO_O_FORCED', w.INFO_O_FORCED), ('FR_INFO_O_KEEP', w.INFO_O_KEEP),
                   ('FR_INFO_O_REFUSED', w.INFO_O_REFUSED),
                   ('FR_INFO_O_VERSION', w.INFO_O_VERSION),
                   ('FR_INFO_O_PIN', w.INFO_O_PIN), ('FR_INFO_O_READBACK', w.INFO_O_READBACK),
                   ('FR_INFO_SLOT_LEN', w.INFO_SLOT_LEN), ('FR_INFO_S_VERSION', w.INFO_S_VERSION),
                   ('FR_INFO_S_FRONT_CRC', w.INFO_S_FRONT_CRC), ('FR_INFO_S_PIN', w.INFO_S_PIN),
                   ('FR_INFO_O_RUN_WAY', w.INFO_O_RUN_WAY), ('FR_INFO_O_RUN_REFUSAL', w.INFO_O_RUN_REFUSAL),
                   ('FR_INFO_O_WAY', w.INFO_O_WAY), ('FR_INFO_O_BATT_LEN', w.INFO_O_BATT_LEN),
                   ('FR_INFO_O_BATT', w.INFO_O_BATT), ('FR_BATT_MAX', w.BATT_MAX)])
        self.assertEqual(h['FR_INFO_O_SLOT'], (w.INFO_O_SLOT, 'FR_INFO_SLOT_LEN'))
        # a version 5 front's INFO ends where the version 6 fields begin
        self.assertEqual(w.INFO_V5, h['FR_INFO_O_RUN_WAY'])
        self.assertEqual(w.INFO_O_SLOT + 2 * w.INFO_SLOT_LEN, w.INFO_V5)
        self.assertEqual(w.INFO_O_BATT + w.BATT_MAX, w.INFO_V6)
        self.assertEqual(w.INFO_O_REFUSED + 1, w.INFO_V11)
        self.assertEqual((w.INFO_V11, w.INFO_O_FS_C + 4), (w.INFO_O_DEVICE, w.INFO_V20))
        self.assertEqual(w.INFO_O_FORCED, w.INFO_O_ACTS + 4)
        self.assertEqual(h['FR_COUNTDOWN_US'], w.COUNTDOWN * 1000000)
        # parse_info()'s INFO_HEAD, field by field
        names = ['WHY', 'FLAGS', 'RUNNING', 'SEL_INDEX', 'SEL_VERIFIED', 'TARGET', 'REFUSAL', 'PHASE',
                 'LEN', 'GOT', 'PROGRESS', 'RESULT', 'IMAGE_RC', 'WRITTEN']
        fmt = w.INFO_HEAD[1:]
        self.assertEqual(len(fmt), len(names))
        self.assertEqual([struct.calcsize('<' + fmt[:k]) for k in range(len(names))],
                         [h['FR_INFO_O_' + n] for n in names])
        self.assertEqual(struct.calcsize(w.INFO_HEAD), w.INFO_O_VERSION)

    def test_flags(self):
        self.same([('FR_INFO_F_' + n, getattr(w, 'F_' + n)) for n in
                   ('ESTABLISHED', 'SELECTOR', 'OWN_DESC', 'MOVED', 'SPOILED', 'HELD', 'READ_ERROR',
                    'UNSETTLED')])
        self.same([('FR_SLOT_F_' + n, getattr(w, 'S_' + n)) for n in
                   ('GOOD', 'HAS_DESC', 'HAS_FRONT', 'TRIED', 'STARTED')])
        self.same([('FR_BEGIN_F_' + n, getattr(w, 'BEGIN_F_' + n)) for n in
                   ('RUNNING', 'TRIAL', 'SELECT', 'FORCE')])
        self.assertEqual(self.h['FR_BEGIN_F_ALL'], w.BEGIN_F_RUNNING | w.BEGIN_F_TRIAL | w.BEGIN_F_SELECT)

    def test_codes(self):
        """Every status the unit can send has its words, and only those; the ways, phases and
        image codes are the enums' order."""
        h = self.h
        rcs = h['enum fr_write_rc']
        self.assertEqual(rcs[0], 'FR_WR_OK')
        st = {k: v for k, v in h.items() if k.startswith('FR_ST_')}
        self.assertEqual(set(w.STATUS), set(range(len(rcs))) | set(st.values()))
        self.assertEqual(sorted(st.values()), list(range(0x40, 0x40 + len(st))))
        self.assertTrue(all(len(rcs) <= v for v in st.values()))
        self.assertEqual(h['enum fr_way'], ['FR_WAY_NONE', 'FR_WAY_PLAIN', 'FR_WAY_SELECT_FIRST',
                                            'FR_WAY_IN_PLACE', 'FR_WAY_TRIAL', 'FR_WAY_SELECT',
                                            'FR_WAY_TRY', 'FR_WAY_SWITCH', 'FR_WAY_KEEP',
                                            'FR_WAY_PUT_BACK'])
        self.assertEqual([w.WAY_NONE, w.WAY_PLAIN, w.WAY_SELECT_FIRST, w.WAY_IN_PLACE, w.WAY_TRIAL,
                          w.WAY_SELECT, w.WAY_TRY, w.WAY_SWITCH, w.WAY_KEEP, w.WAY_PUT_BACK],
                         list(range(10)))
        self.assertEqual(h['enum fr_phase'], ['FR_PH_IDLE', 'FR_PH_RECEIVING', 'FR_PH_WRITING',
                                              'FR_PH_DONE', 'FR_PH_FAILED', 'FR_PH_COUNTDOWN'])
        self.assertEqual(w.PHASES, ['idle', 'receiving', 'writing', 'done', 'failed',
                                    'counting down'])
        self.assertEqual(w.COUNTING, 5)
        self.assertEqual(h['enum fr_act'], ['FR_ACT_NONE', 'FR_ACT_TRY', 'FR_ACT_SWITCH',
                                            'FR_ACT_KEEP', 'FR_ACT_PUT_BACK'])
        self.assertEqual([w.ACT_TRY, w.ACT_SWITCH, w.ACT_KEEP, w.ACT_PUT_BACK], [1, 2, 3, 4])
        self.assertEqual(h['enum fr_tier'], ['FR_TIER_NONE', 'FR_TIER_SAME', 'FR_TIER_FRONT',
                                             'FR_TIER_FOREIGN', 'FR_TIER_BAD'])
        self.assertEqual([w.TIER_NONE, w.TIER_SAME, w.TIER_FRONT, w.TIER_FOREIGN, w.TIER_BAD],
                         list(range(5)))
        self.assertEqual(rcs[22:], ['FR_WR_ALREADY', 'FR_WR_TARGET_BAD', 'FR_WR_STOPPED'])
        img = h['enum fr_image_rc']
        self.assertEqual(img[0], 'FR_IMG_OK')
        self.assertEqual(set(w.IMAGE_RC), set(range(1, len(img))))
        self.same([('FR_WHY_KEYS', 1), ('FR_WHY_CHECK', 2), ('FR_WHY_COUNT', 3)])
        self.assertEqual(set(w.WHY), {1, 2, 3})


class SignedPins(unittest.TestCase):
    P1, P2, P3 = b'\x11' * 32, b'\x22' * 32, b'\x33' * 32

    def test_no_git(self):
        """Outside a git checkout: no pin is signed, and none gets a version; the current pin is
        named only as the one this checkout has now."""
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(w.signed_pins(d))
            put(d, w.PIN_FILE, pin_text(self.P1))
            put(d, w.PROTO_H, proto_text(7))
            got = w.signed_pins(d)
            self.assertEqual(got, {'git': False, 'pins': {}, 'now': self.P1})
            state, ver, words = w.signed_words(self.P1, lambda: got)
            self.assertEqual((state, ver), ('unknown', None))
            self.assertIn('it is the pin this checkout has now', words)
            self.assertIn('it is not the pin', w.signed_words(self.P2, lambda: got)[2])

    @unittest.skipUnless(shutil.which('git'), 'no git')
    def test_git(self):
        """A temporary repository: two commits that set the pin, then a front.pin not committed.
        Only a committed pin is signed, each with the version of its own commit."""
        env = {k: v for k, v in os.environ.items()
               if k not in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE')}
        env.update(GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='', GIT_COMMITTER_NAME='t',
                   GIT_COMMITTER_EMAIL='', GIT_CONFIG_GLOBAL=os.devnull,
                   GIT_CONFIG_NOSYSTEM='1')

        def git(d, *args):
            subprocess.run(['git', '-C', d] + list(args), check=True, capture_output=True,
                           env=env, timeout=30)
        with tempfile.TemporaryDirectory() as d:
            git(d, 'init', '-q')
            for pin, v in ((self.P1, 6), (self.P2, 7)):
                put(d, w.PIN_FILE, pin_text(pin))
                put(d, w.PROTO_H, proto_text(v))
                git(d, 'add', '.')
                git(d, 'commit', '-q', '--no-verify', '-m', 'c')
            put(d, w.PIN_FILE, pin_text(self.P3))
            put(d, w.PROTO_H, proto_text(8))
            bogus = os.path.join(d, 'nowhere')
            with mock.patch.dict(os.environ, {
                    'GIT_DIR': bogus, 'GIT_WORK_TREE': bogus, 'GIT_INDEX_FILE': bogus,
                    'GIT_COMMON_DIR': bogus, 'GIT_OBJECT_DIRECTORY': bogus,
                    'GIT_ALTERNATE_OBJECT_DIRECTORIES': bogus, 'GIT_CEILING_DIRECTORIES': d,
                    'GIT_CONFIG_COUNT': '1', 'GIT_CONFIG_KEY_0': 'core.bare',
                    'GIT_CONFIG_VALUE_0': 'true', 'GIT_EXEC_PATH': bogus}):
                got = w.signed_pins(d)
            self.assertEqual(got, {'git': True, 'pins': {self.P1: 6, self.P2: 7}, 'now': self.P3})
            self.assertEqual(w.signed_words(self.P1, lambda: got)[:2], ('signed', 6))
            self.assertEqual(w.signed_words(self.P2, lambda: got)[:2], ('signed', 7))
            state, ver, words = w.signed_words(self.P3, lambda: got)
            self.assertEqual((state, ver), ('unsigned', None))
            self.assertIn("the working tree's pin, not in a commit", words)

    @unittest.skipUnless(shutil.which('git'), 'no git')
    def test_git_older_header_path(self):
        """A commit from before the front's folders has the protocol header at its older path:
        its pin still gets that commit's version, beside a later commit's at the path now."""
        env = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
        env.update(GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='', GIT_COMMITTER_NAME='t',
                   GIT_COMMITTER_EMAIL='', GIT_CONFIG_GLOBAL=os.devnull,
                   GIT_CONFIG_NOSYSTEM='1')

        def git(d, *args):
            subprocess.run(['git', '-C', d] + list(args), check=True, capture_output=True,
                           env=env, timeout=30)
        self.assertNotEqual(w.PROTO_H, w.PROTO_H_OLD)
        with tempfile.TemporaryDirectory() as d:
            git(d, 'init', '-q')
            put(d, w.PIN_FILE, pin_text(self.P1))
            put(d, w.PROTO_H_OLD, proto_text(6))
            git(d, 'add', '.')
            git(d, 'commit', '-q', '-m', 'c')
            os.makedirs(os.path.dirname(os.path.join(d, w.PROTO_H)), exist_ok=True)
            git(d, 'mv', w.PROTO_H_OLD, w.PROTO_H)
            put(d, w.PIN_FILE, pin_text(self.P2))
            put(d, w.PROTO_H, proto_text(7))
            git(d, 'add', '.')
            git(d, 'commit', '-q', '-m', 'c')
            got = w.signed_pins(d)
            self.assertEqual(got, {'git': True, 'pins': {self.P1: 6, self.P2: 7}, 'now': self.P2})
            self.assertEqual(w.signed_words(self.P1, lambda: got)[:2], ('signed', 6))

    def test_lookup_fails(self):
        def boom():
            raise OSError('no')
        self.assertEqual(w.signed_words(b'\x00' * 32, boom)[0], 'unknown')


# --- the C protocol, through a small shared library ---

SHIM = r'''
#define _POSIX_C_SOURCE 199309L
#include <string.h>
#include <time.h>
#include "front/recovery/proto.h"

static uint8_t flash[0x100000];
static uint8_t staging[FR_SLOT_CAP];
static uint8_t in[1 << 16], out[1 << 16];
static size_t in_n, in_at, out_n;
static struct fr_rec_status st;
static struct fr_proto proto;
static uint64_t now_us;

static size_t rd(void *c, uint8_t *b, size_t m)
{ size_t k = in_n - in_at < m ? in_n - in_at : m; (void)c; memcpy(b, in + in_at, k); in_at += k;
  if (in_at == in_n) in_at = in_n = 0; return k; }
static size_t wr(void *c, const uint8_t *b, size_t n)
{ size_t k = sizeof out - out_n < n ? sizeof out - out_n : n; (void)c; memcpy(out + out_n, b, k);
  out_n += k; return k; }
static bool idle(void *c) { (void)c; return true; }
static uint32_t bad_read_at; static int bad_reads;
static int f_read(void *c, uint32_t o, uint8_t *b, uint32_t n)
{ (void)c; if (bad_reads && bad_read_at >= o && bad_read_at < o + n) { bad_reads--; return -1; }
  memcpy(b, flash + o, n); return 0; }
void shim_fail_reads(uint32_t at, int count) { bad_read_at = at; bad_reads = count; }
static long erase_ns;
static int erases;
static uint32_t erase_log[4096];
int shim_erases(void) { return erases; }
uint32_t shim_erase_at(int k) { return erase_log[k & 4095]; }
static int f_erase(void *c, uint32_t o)
{ struct timespec t = { 0, erase_ns }; (void)c; if (erase_ns) nanosleep(&t, NULL);
  erase_log[erases++ & 4095] = o;
  memset(flash + o, 0xFF, FR_SECTOR); return 0; }
static int ignore_selector;
static uint32_t fail_lo, fail_hi;
static int f_prog(void *c, uint32_t o, const uint8_t *b, uint32_t n)
{ (void)c; if (ignore_selector && o == 0xD000u) return 0;
  if (o >= fail_lo && o < fail_hi) return -1;
  for (uint32_t i = 0; i < n; i++) flash[o + i] &= b[i]; return 0; }
void shim_ignore_selector(int on) { ignore_selector = on; }
void shim_fail(uint32_t lo, uint32_t hi) { fail_lo = lo; fail_hi = hi; }
void shim_erase_ns(long ns) { erase_ns = ns; }
static uint8_t batt_next[16], batt_next_len;
static int batt_asks;
static void batt_cb(void *c)
{ (void)c; batt_asks++; memcpy(st.batt, batt_next, batt_next_len); st.batt_len = batt_next_len; }
void shim_battery(const uint8_t *b, unsigned n) { memcpy(batt_next, b, n); batt_next_len = (uint8_t)n; }
int shim_batt_asks(void) { return batt_asks; }
static const struct fr_bytes io = { rd, wr, idle, NULL };
static const struct fr_flash fl = { f_read, f_erase, f_prog, NULL };

void shim_flash(uint32_t off, const uint8_t *b, uint32_t n) { memcpy(flash + off, b, n); }
uint8_t shim_byte(uint32_t off) { return flash[off]; }
void shim_start(int running, int why)
{
	memset(&st, 0, sizeof st);
	st.why = (uint8_t)why;
	st.running.established = true;
	st.running.slot = (uint8_t)running;
	st.selector_ok = fr_read_selector(&fl, &st.sel) == 0;
	for (unsigned k = 0; k < 2; k++)
		fr_slot_info(&fl, k, &st.slot[k]);
	st.own_desc = st.slot[running].has_desc;
	st.desc = st.slot[running].desc;
	fr_proto_init(&proto, &io, &st, &fl, staging);
	now_us = 0;
	fr_proto_clock(&proto, now_us);
	proto.battery = batt_cb;
	batt_asks = 0;
	in_n = in_at = out_n = 0;
}
void shim_rx(const uint8_t *b, size_t n) { memcpy(in + in_n, b, n); in_n += n; }
size_t shim_tx(uint8_t *b, size_t m) { size_t k = out_n < m ? out_n : m; memcpy(b, out, k);
  memmove(out, out + k, out_n - k); out_n -= k; return k; }
void shim_poll(void) { fr_proto_poll(&proto); }
/* the countdown's clock: the test moves it on by whole seconds */
void shim_tick(unsigned s) { now_us += 1000000ull * s; fr_proto_clock(&proto, now_us); }
void shim_drop(void) { fr_proto_drop(&proto); }
int shim_restart(void) { return proto.restart; }
'''


def build_shim(d):
    srcs = ['recovery/proto.c', 'slots/slots.c', 'slots/image.c', 'start/util.c']
    if not all(os.path.exists(os.path.join(TOP, 'src', 'front', s)) for s in srcs):
        return None
    shim = os.path.join(d, 'shim.c')
    with open(shim, 'w') as f:
        f.write(SHIM)
    out = os.path.join(d, 'shim.so')
    subprocess.check_call([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-shared', '-fPIC',
                           '-I', os.path.join(TOP, 'src'), '-I', os.path.join(TOP, 'src', 'include'),
                           '-o', out, shim] + [os.path.join(TOP, 'src', 'front', s) for s in srcs])
    lib = ctypes.CDLL(out)
    lib.shim_flash.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32]
    lib.shim_byte.argtypes = [ctypes.c_uint32]
    lib.shim_byte.restype = ctypes.c_uint8
    lib.shim_start.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.shim_fail.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    lib.shim_erase_ns.argtypes = [ctypes.c_long]
    lib.shim_fail_reads.argtypes = [ctypes.c_uint32, ctypes.c_int]
    lib.shim_erase_at.argtypes = [ctypes.c_int]
    lib.shim_erase_at.restype = ctypes.c_uint32
    lib.shim_battery.argtypes = [ctypes.c_char_p, ctypes.c_uint]
    lib.shim_rx.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.shim_tx.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.shim_tx.restype = ctypes.c_size_t
    lib.shim_ignore_selector.argtypes = [ctypes.c_int]
    lib.shim_tick.argtypes = [ctypes.c_uint]
    lib.fr_image_valid.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_void_p]
    lib.fr_image_valid.restype = ctypes.c_int
    return lib


@unittest.skipUnless(shutil.which(os.environ.get('CC', 'cc')), 'no C compiler')
class AgainstC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.lib = build_shim(cls.dir.name)
        if cls.lib is None:
            raise unittest.SkipTest('src/front/slots/slots.c is not there yet')

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    A_IMG = make_image(seed=1)
    B_IMG = make_image(seed=2, version=(0, 9, 0, 0))

    def setUp(self):
        self.master, slave = os.openpty()
        self.link = w.Link(os.ttyname(slave), timeout=0.5, retries=4, long_timeout=10.0)
        os.close(slave)
        self.lock = threading.Lock()
        self.sniff, self.seen, self.frames = bytearray(), [], []
        self.drop_after = None
        self.start()
        self.stop = False
        self.free = True            # false: the front polls only when bytes came
        self.lost = {}              # request type: replies to it dropped on their way to the host
        self.t = threading.Thread(target=self.pump, daemon=True)
        self.t.start()

    def tearDown(self):
        self.stop = True
        self.t.join(2)
        self.link.close()
        os.close(self.master)

    def place(self, k, img, good=True):
        """img in slot k with its record; a bad slot's record CRC is off by one bit, so that
        its front stays intact (S-02.084)."""
        self.lib.shim_flash(0x10000 + 0x78000 * k, img, len(img))
        crc = zlib.crc32(img) ^ (0 if good else 1)
        self.lib.shim_flash(0xE000 + 0x1000 * k, struct.pack('<III', crc, len(img), 0xB004), 12)

    def start(self, a='good', b=None, why=1, batt=b'', run=0, sel=None):
        """Slot 1 (0) running and selected, verified 01, or the selector sel; a and b say whether
        each slot is good, bad, or (b only) erased."""
        c = self.lib
        with self.lock:
            c.shim_flash(0, b'\xff' * 0x100000, 0x100000)
            self.place(run, self.A_IMG, a == 'good')
            if b:
                self.place(1 - run, self.B_IMG, b == 'good')
            c.shim_flash(0xD000, bytes(sel or [run, 1]), 2)
            c.shim_ignore_selector(0)
            c.shim_fail(0, 0)
            c.shim_erase_ns(0)
            c.shim_battery(batt, len(batt))
            c.shim_fail_reads(0, 0)
            c.shim_start(run, why)
            self.seen = []
            self.infos = 0

    def slot(self, k, n):
        return bytes(self.lib.shim_byte(0x10000 + 0x78000 * k + i) for i in range(n))

    def selector(self):
        return (self.lib.shim_byte(0xD000), self.lib.shim_byte(0xD001))

    def pump(self):
        import select
        buf = ctypes.create_string_buffer(4096)
        while not self.stop:
            r, _, _ = select.select([self.master], [], [], 0.002)
            if not r and not self.free:
                continue
            with self.lock:
                infos = 0
                if r:
                    data = os.read(self.master, 4096)
                    self.sniff += data
                    while True:
                        got = w.take_frame(self.sniff)
                        if got is None:
                            break
                        self.seen.append(got[0])
                        self.frames.append(got[::2])
                        infos += got[0] == w.INFO
                    self.lib.shim_rx(data, len(data))
                self.lib.shim_poll()
                # the countdown runs one second for each INFO answered
                self.lib.shim_tick(infos)
                self.infos += infos
                if self.drop_after is not None and self.infos >= self.drop_after:
                    self.lib.shim_drop()        # the host leaves: USB's configured state changed
                    self.drop_after = None
                n = self.lib.shim_tx(buf, 4096)
            if n and not self.drop(buf.raw[:n]):
                os.write(self.master, buf.raw[:n])

    def drop(self, data):
        """Whether data, a reply the front sent, is one of those self.lost drops."""
        got = w.take_frame(bytearray(data))
        typ = got[0] & ~w.REPLY if got else None
        if self.lost.get(typ):
            self.lost[typ] -= 1
            return True
        return False

    def write(self, img, running=False, ask=no_prompt):
        out = Lines()
        rc = w.cmd_write(self.link, img, out, poll=0.01, running=running, ask=ask)
        return rc, out.text

    # the four start states, A the running and selected slot, B the other; both commands

    def fail_b(self, img=None):
        """A plain write of B that ends in a flash failure: it opens the in-place way
        (S-02.084)."""
        self.lib.shim_fail(0x88000, 0x100000)
        rc, text = self.write(img or make_image(seed=6), ask=Ask('yes'))
        self.assertEqual(rc, 1, text)
        self.assertIn('FAILED: a flash erase, program or read of the write failed', text)
        self.lib.shim_fail(0, 0)
        return text

    def test_stale_partial_frame(self):
        """S-02.093: a frame an earlier host left cut off in the front's buffer does not take the
        first HELLO: it is answered at the first try."""
        f = stale_frame()
        self.link.long_timeout = 1.0
        for k in (1, 2, 3, 5, 6, 7, 40, 600, len(f) - 1):
            self.start()
            with self.lock:
                self.lib.shim_rx(f[:k], k)
            self.link.synced = False
            self.sniff.clear()
            out = Lines()
            self.assertEqual(w.cmd_info(self.link, out), 0, k)
            self.assertEqual(self.seen.count(w.HELLO), 1, k)

    def test_write_a_good_b_good(self):
        self.start('good', 'good')
        img = make_image(seed=5, size=3000)
        ask = Ask('yes')
        rc, text = self.write(img, ask=ask)
        self.assertEqual(rc, 0, text)
        self.assertIn('Slot 2 holds a checked image (version 0.9.0), which this write replaces', text)
        self.assertNotIn('only good image', text)
        self.assertEqual(ask.prompts, ['Type yes to go on: '])
        self.assertEqual(self.seen[:4], [w.HELLO, w.INFO, w.BEGIN, w.INFO])
        self.assertEqual(self.slot(1, len(img)), img)
        self.assertEqual(self.selector(), (1, 0))

    def test_trial_then_select(self):
        """S-02.084: another front by a trial and its select, against the front's code: both
        slot orders, the select from the old front and from the tried slot's own front. The
        first stage's part of the trial start (S-02.022) is done here."""
        img = make_image(seed=7, front=2, size=3000)
        for run in (0, 1):
            b = 1 - run
            for own in (False, True):
                self.start('good', 'good', run=run)
                before = self.slot(run, 200)
                out, ask = Lines(), Ask('try the new front')
                rc = w.cmd_write(self.link, img, out, poll=0.01, ask=ask, new_front=True,
                                 pins=lambda: {'git': True, 'pins': {}, 'now': None})
                self.assertEqual(rc, 0, out.text)
                self.assertIn('NOT a pin this checkout has signed', out.text)
                self.assertEqual(self.slot(b, len(img)), img)
                self.assertEqual(self.slot(run, 200), before)
                self.assertEqual(self.selector(), (0x00, 0xFE) if b == 0 else (0xFF, 0xFF))
                rec = bytes(self.lib.shim_byte(0xE000 + 0x1000 * b + i) for i in range(12))
                self.assertEqual(struct.unpack('<III', rec),
                                 (zlib.crc32(img) ^ 0xFFFFFFFF, len(img), 0xB004))
                # no select before the trial start has put the selector back
                out = Lines()
                self.assertEqual(w.cmd_select(self.link, img, out, ask=no_prompt), 1, out.text)
                # the trial start: the first stage's selector, then recovery in the tried slot,
                # or, a power cycle later, in the old one
                with self.lock:
                    self.lib.shim_flash(0xD000, bytes([run, 1]), 2)
                    self.lib.shim_start(b if own else run, 1)
                    self.seen = []
                if not own:
                    # no mark of a trial start in B's log: the old front's select is refused
                    out = Lines()
                    self.assertEqual(w.cmd_select(self.link, img, out, ask=no_prompt), 1, out.text)
                    self.assertIn('has left no mark of a trial start', out.text)
                    self.assertEqual(self.selector(), (run, 1))
                    # the new front's mark of its trial start: entry 0 of B's log finished
                    with self.lock:
                        self.lib.shim_flash(0x10000 + 0x78000 * b + 0x77000, b'\xfc', 1)
                        self.lib.shim_start(run, 1)
                        self.seen = []
                out = Lines()
                rc = w.cmd_select(self.link, img, out, poll=0.01, ask=Ask('yes'))
                self.assertEqual(rc, 0, out.text)
                self.assertEqual(self.selector(), (b, 0))
                rec = bytes(self.lib.shim_byte(0xE000 + 0x1000 * b + i) for i in range(12))
                self.assertEqual(struct.unpack('<III', rec), (zlib.crc32(img), len(img), 0xB004))
                self.assertEqual(self.slot(b, len(img)), img)
                self.assertEqual(self.slot(run, 200), before)

    def test_trial_refused(self):
        """S-02.093: another front with no trial asked for is status 12 from the front's code,
        and a trial with the running slot bad is status 19; nothing is written."""
        img = make_image(seed=7, front=2, size=3000)
        self.start('good', 'good')
        reply = self.link.request(w.BEGIN, struct.pack('<I', len(img)) + hashlib.sha256(img).digest())
        self.assertEqual(reply[0][0], 0)
        for off in range(0, len(img), 1024):
            self.link.request(w.DATA, struct.pack('<I', off) + img[off:off + 1024])
        self.assertEqual(self.link.request(w.END)[0][0], 12)
        self.start('bad', 'good')
        before = self.lib.shim_erases()
        reply = self.link.request(w.BEGIN, struct.pack('<I', len(img)) +
                                  hashlib.sha256(img).digest() + b'\x02')
        self.assertEqual(reply[0][0], 0)
        for off in range(0, len(img), 1024):
            self.link.request(w.DATA, struct.pack('<I', off) + img[off:off + 1024])
        self.assertEqual(self.link.request(w.END)[0][0], 19)
        self.assertEqual(self.lib.shim_erases(), before)
        self.assertEqual(self.selector(), (0, 1))

    def test_running_a_good_b_good(self):
        """Way 2: the selector names B first, then A is written; it ends {A, 00}."""
        self.start('good', 'good')
        img = make_image(seed=5, size=3000)
        rc, text = self.write(img, running=True, ask=Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertIn('This erases and rewrites Slot 1, the running slot. The unit first selects '
                      "Slot 2: if the write is interrupted, the unit starts Slot 2's image "
                      '(version 0.9.0).', text)
        self.assertNotIn('without this recovery', text)
        self.assertIn('Verified: Slot 1', text)
        self.assertEqual(self.slot(0, len(img)), img)
        self.assertEqual(self.slot(1, len(self.B_IMG)), self.B_IMG)
        self.assertEqual(self.selector(), (0, 0))

    def test_write_a_good_b_bad(self):
        """A plain write that loses nothing still states it and asks for yes."""
        self.start('good', 'bad')
        img = make_image(seed=5)
        ask = Ask('yes')
        rc, text = self.write(img, ask=ask)
        self.assertEqual(rc, 0, text)
        self.assertIn('Slot 2 holds no checked image, so nothing checked is lost. Slot 1, the running slot, '
                      'is left as it is: if the write is interrupted, the unit starts as before.', text)
        self.assertEqual(ask.prompts, ['Type yes to go on: '])
        self.assertEqual(self.slot(1, len(img)), img)
        self.assertEqual(self.selector(), (1, 0))

    def test_in_place_gate(self):
        """S-02.084: with B bad, --running is refused (15) until a write of B has failed in this
        recovery; then BEGIN announces way 3, the battery is asked, the phrase is typed, and A is
        written in place with no selector write."""
        self.start('good', 'bad', batt=bytes([2, 40, 25, 0, 0, 0x0F, 0xA0]))
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('with --running: refused: an in-place write of the running slot becomes '
                      'possible only after a write of the other slot has failed in this recovery',
                      out.text)
        img = make_image(seed=5, size=3000)
        rc, text = self.write(img, running=True)
        self.assertEqual(rc, 1)
        self.assertIn('Refused: the other slot failed its check, and an in-place write of the running slot '
                      'becomes possible only after a write of the other slot has failed in this '
                      'recovery (S-02.084). Try a plain write first (wbrecover.py write IMAGE): it is '
                      'safe and loses nothing. Nothing was sent.', text)
        self.assertNotIn(w.BEGIN, self.seen)
        # the unit's own answer, past the tool's prediction
        w.hello(self.link)
        r = self.link.request(w.BEGIN, struct.pack('<I', len(img)) + hashlib.sha256(img).digest() + b'\x01')[0]
        self.assertEqual(r, bytes([15, 0, 0xFF]))
        self.assertEqual(self.lib.shim_batt_asks(), 0)
        self.fail_b()
        self.seen = []
        ask = Ask(w.PHRASE)
        rc, text = self.write(img, running=True, ask=ask)
        self.assertEqual(rc, 0, text)
        self.assertEqual(ask.prompts, ['Type "%s" to go on: ' % w.PHRASE])
        self.assertIn('DANGER: Slot 2 holds no checked image, so this write erases Slot 1, the running '
                      'slot, and writes it in place.', text)
        self.assertIn('If the write is interrupted (a power cut, a pulled cable with no charged '
                      'battery, the power switch), the unit cannot be repaired without a flash '
                      'programmer.', text)
        self.assertIn('The unit offers this only because a write of Slot 2 failed in this recovery.', text)
        self.assertIn('Battery: present and charged (charging, 40 %, 4000 mV).', text)
        self.assertIn('The battery carries the unit if the cable comes out', text)
        self.assertEqual(self.lib.shim_batt_asks(), 1)
        self.assertEqual(self.seen[:4], [w.HELLO, w.INFO, w.BEGIN, w.INFO])
        self.assertEqual(self.slot(0, len(img)), img)
        self.assertEqual(self.selector(), (0, 1))
        self.assertIn('The selector names Slot 1', text)

    def test_write_a_bad_b_good(self):
        """A bad with its front intact: the default write goes to B, the only good image."""
        self.start('bad', 'good')
        img = make_image(seed=5)
        rc, text = self.write(img, ask=Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertIn('Slot 2 holds a checked image (version 0.9.0), which this write replaces', text)
        self.assertIn('Slot 1, the running slot, failed its check, so Slot 2 holds the only checked image. '
                      'write --running is the safer choice in this state: it selects Slot 2 first '
                      'and rewrites Slot 1, so an interruption starts Slot 2.', text)
        self.assertEqual(self.slot(1, len(img)), img)
        self.assertEqual(self.selector(), (1, 0))

    def test_running_a_bad_b_good(self):
        self.start('bad', 'good')
        img = make_image(seed=5)
        rc, text = self.write(img, running=True, ask=Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertIn("starts Slot 2's image (version 0.9.0)", text)
        self.assertEqual(self.slot(0, len(img)), img)
        self.assertEqual(self.selector(), (0, 0))

    def test_write_a_bad_b_bad(self):
        self.start('bad', 'bad')
        img = make_image(seed=5)
        rc, text = self.write(img, ask=Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertIn('Slot 2 holds no checked image, so nothing checked is lost', text)
        self.assertEqual(self.slot(1, len(img)), img)
        self.assertEqual(self.selector(), (1, 0))

    def test_running_a_bad_b_bad(self):
        self.start('bad', 'bad', batt=bytes([0, 0, 0, 0, 0, 0, 0]))
        img = make_image(seed=5)
        rc, text = self.write(img, running=True)
        self.assertEqual(rc, 1)
        self.assertIn('Try a plain write first', text)
        self.fail_b()
        rc, text = self.write(img, running=True, ask=Ask(w.PHRASE))
        self.assertEqual(rc, 0, text)
        self.assertIn('Battery: absent: the cable alone powers the unit.', text)
        self.assertIn('Keep the cable in and do not touch the power switch', text)
        self.assertEqual(self.slot(0, len(img)), img)
        self.assertEqual(self.selector(), (0, 1))

    def test_running_b_not_waffle(self):
        """Way 2 with B a good image that is not waffle-boot's."""
        self.start('good')
        other = b'\x00' * 4000
        self.lib.shim_flash(0x88000, other, len(other))
        self.lib.shim_flash(0xF000, struct.pack('<III', zlib.crc32(other), len(other), 0xB004), 12)
        with self.lock:
            self.lib.shim_start(0, 1)
        rc, text = self.write(make_image(seed=5), running=True, ask=Ask('no'))
        self.assertEqual(rc, 1, text)
        self.assertIn("starts Slot 2's image (not a waffle-boot image)", text)
        self.assertIn('The unit is then on that bootloader, without this recovery.', text)

    # declined prompts and the phrase

    def test_declined_yes(self):
        """The confirmation follows BEGIN; declined, nothing more is sent."""
        self.start('good', 'good')
        for running in (False, True):
            with self.subTest(running=running):
                self.seen = []
                rc, text = self.write(make_image(seed=5), running=running, ask=Ask('y'))
                self.assertEqual(rc, 1)
                self.assertIn('Stopped: no image byte was sent, and nothing was written.', text)
                self.assertEqual(self.seen, [w.HELLO, w.INFO, w.BEGIN, w.INFO])
        self.assertEqual(self.slot(0, len(self.A_IMG)), self.A_IMG)
        self.assertEqual(self.slot(1, len(self.B_IMG)), self.B_IMG)
        self.assertEqual(self.selector(), (0, 1))

    def test_phrase_mismatch(self):
        """The in-place phrase must be typed exactly; yes is not enough. BEGIN was sent, DATA
        never; nothing is written."""
        for answer in ('yes', 'erase the running slot', w.PHRASE.upper(), ''):
            with self.subTest(answer=answer):
                self.start('good', 'bad')
                self.fail_b()
                self.seen = []
                rc, text = self.write(make_image(seed=5), running=True, ask=Ask(answer))
                self.assertEqual(rc, 1, text)
                self.assertIn('Stopped: no image byte was sent, and nothing was written.', text)
                self.assertEqual(self.seen, [w.HELLO, w.INFO, w.BEGIN, w.INFO])
                self.assertEqual(self.slot(0, len(self.A_IMG)), self.A_IMG)
                self.assertEqual(self.selector(), (0, 1))
                i = w.get_info(self.link)
                self.assertEqual(i['phase'], w.RECEIVING)

    def test_end_16(self):
        """B made good in the flash between BEGIN's way 3 and END: END answers 16 and nothing is
        written."""
        self.start('good', 'bad')
        self.fail_b()

        def change(prompt):
            self.place(1, self.B_IMG, True)
            return w.PHRASE
        img = make_image(seed=5)
        rc, text = self.write(img, running=True, ask=change)
        self.assertEqual(rc, 1, text)
        self.assertIn("Refused: the unit's state changed since the write was confirmed: it would not "
                      "go as BEGIN's answer said, and nothing was written. Run info, then write "
                      'again.', text)
        self.assertEqual(self.slot(0, len(self.A_IMG)), self.A_IMG)
        self.assertEqual(self.selector(), (0, 1))

    # END's reply lost after a write of this recovery has run (S-02.093)

    def lose_end(self, change=None):
        """The confirmation, which first makes change() to the flash, if any, and has END's
        first reply dropped: the tool sends END again and gets 0x41, no transfer open."""
        def ask(prompt):
            if change:
                change()
            self.lost[w.END] = 1
            return w.PHRASE if 'erase' in prompt else 'yes'
        self.link.long_timeout = 1.0
        return ask

    def unsettle(self):
        """The selector made {A, FF}: END is refused with status 4."""
        self.lib.shim_flash(0xD000, b'\x00\xff', 2)

    def test_end_lost_after_failed_write(self):
        """A write of B failed, then END of an in-place write is refused (16) and its reply lost:
        INFO shows the failed write of B and, at byte 231, the refusal: the tool reports it, not
        that failure."""
        self.start('good', 'bad')
        self.fail_b()
        self.seen = []
        rc, text = self.write(make_image(seed=5), running=True,
                              ask=self.lose_end(lambda: self.place(1, self.B_IMG, True)))
        self.assertEqual(rc, 1, text)
        self.assertEqual(self.seen.count(w.END), 2)
        self.assertIn("Refused: the unit's state changed since the write was confirmed", text)
        self.assertIn(REFUSED_V11 % 'a write that failed: a flash erase, program or read of the '
                      'write failed', text)
        self.assertNotIn('FAILED', text)
        self.assertEqual(self.slot(0, len(self.A_IMG)), self.A_IMG)
        self.assertEqual(self.selector(), (0, 1))

    def test_end_lost_after_done_write(self):
        """An in-place write done, then END of a plain write refused (4) with its reply lost: INFO
        shows the write of A, done, and the refusal; the tool does not take it for this one."""
        self.start('good', 'bad')
        self.fail_b()
        img = make_image(seed=5)
        rc, text = self.write(img, running=True, ask=Ask(w.PHRASE))
        self.assertEqual(rc, 0, text)
        rc, text = self.write(make_image(seed=8), ask=self.lose_end(self.unsettle))
        self.assertEqual(rc, 1, text)
        self.assertIn('Refused: restart before another write', text)
        self.assertIn(REFUSED_V11 % 'Slot 1 written and read back', text)
        self.assertNotIn('VERIFIED', text)
        self.assertEqual(self.slot(0, len(img)), img)

    def test_end_lost_written(self):
        """After a failed write of B, a plain write whose END reply is lost and which ran: INFO
        shows this write, done, and the tool verifies it."""
        self.start('good', 'bad')
        self.fail_b()
        self.seen = []
        img = make_image(seed=8)
        rc, text = self.write(img, ask=self.lose_end())
        self.assertEqual(rc, 0, text)
        self.assertIn('Verified: Slot 2 holds this file', text)
        self.assertEqual(self.seen.count(w.END), 2)

    def test_end_lost_told_apart(self):
        """After a failed write of B, END of a second plain write is lost. Refused (4), INFO shows
        the first write's failure and the refusal at byte 231, which a front before 9 could not
        tell from this write failed alike. Taken, END sent again finds the countdown (busy): the
        write is this one, and its failure is reported as such."""
        for refused in (True, False):
            with self.subTest(refused=refused):
                self.start('good', 'bad')
                self.fail_b()
                if not refused:
                    self.lib.shim_fail(0x88000, 0x100000)
                rc, text = self.write(make_image(seed=8),
                                      ask=self.lose_end(self.unsettle if refused else None))
                self.assertEqual(rc, 1, text)
                self.assertNotIn(CANNOT_TELL, text)
                if refused:
                    self.assertIn('Refused: restart before another write', text)
                    self.assertIn(REFUSED_V11 % 'a write that failed: a flash erase, program or '
                                  'read of the write failed', text)
                else:
                    self.assertIn('Countdown:  1 s left', text)
                    self.assertIn('FAILED: a flash erase, program or read of the write failed',
                                  text)

    # A's record erased or torn (S-02.084)

    def record_a(self, rec, b):
        """A holds A_IMG with record rec, the selector {A, 01}; B as start() takes it."""
        self.start('good', b)
        with self.lock:
            self.lib.shim_flash(0xE000, rec, 12)
            self.lib.shim_start(0, 1)
            self.seen = []

    def test_record_erased(self):
        """A in-place write cut between A's record erase and program: A holds the image with its
        record erased. Recovery reads A's front within the slot and takes writes; with B good,
        write --running writes A again whole with the same image, B selected first; with B bad,
        --running is refused (15) and a plain write of another image goes to B."""
        self.record_a(b'\xff' * 12, 'good')
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('Slot 1:   check failed, version 1.2.3+4, front present', out.text)
        self.assertIn('Write:    would go to Slot 2', out.text)
        rc, text = self.write(self.A_IMG, running=True, ask=Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertIn('This erases and rewrites Slot 1, the running slot. The unit first selects '
                      'Slot 2', text)
        self.assertEqual(self.slot(1, len(self.B_IMG)), self.B_IMG)
        rec = bytes(self.lib.shim_byte(0xE000 + i) for i in range(12))
        self.assertEqual(rec, struct.pack('<III', zlib.crc32(self.A_IMG), len(self.A_IMG), 0xB004))
        self.assertEqual(self.selector(), (0, 0))
        self.record_a(b'\xff' * 12, 'bad')
        rc, text = self.write(self.A_IMG, running=True)
        self.assertEqual(rc, 1, text)
        self.assertIn('Try a plain write first', text)
        img = make_image(seed=5)
        rc, text = self.write(img, ask=Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertEqual(self.slot(1, len(img)), img)
        self.assertEqual(self.selector(), (1, 0))

    def test_record_torn(self):
        """Another tag is written over like an erased record; the tag with a length of 2^31 or
        more counts as A's front not intact (5)."""
        self.record_a(struct.pack('<III', 0, 0x1000, 0xB005), 'good')
        self.assertEqual(w.get_info(self.link)['target'], 1)
        self.record_a(struct.pack('<III', 0, 0x80000000, 0xB004), 'good')
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('Write:    refused: the boot slot failed its check, and is not the running slot '
                      'with its front intact', out.text)

    # statuses 5, 6, 13, 14

    def test_status_5(self):
        """A bad and its front not intact: refused before sending."""
        img = bytearray(self.A_IMG)
        fl1 = bytes((1 + i) & 0xFF for i in range(32))
        img[img.find(fl1)] ^= 0xFF
        self.start('good')
        self.lib.shim_flash(0x10000, bytes(img), len(img))
        with self.lock:
            self.lib.shim_start(0, 1)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('refused: the boot slot failed its check, and is not the running slot with its '
                      'front intact', out.text)
        rc, text = self.write(make_image(seed=5), running=True)
        self.assertEqual(rc, 1)
        self.assertIn('Refused before sending: the boot slot failed its check', text)
        self.assertNotIn(w.BEGIN, self.seen)

    def test_status_6(self):
        """The unit itself refuses an image over 0x76000 bytes."""
        w.hello(self.link)
        r = self.link.request(w.BEGIN, struct.pack('<I', w.WB_IMAGE_MAX + 1) + bytes(32))[0]
        self.assertEqual(r[0], 6)
        self.assertIn('at most 0x76000 bytes', w.status_text(r[0]))
        self.assertEqual(self.link.request(w.BEGIN, struct.pack('<I', w.WB_IMAGE_MAX) + bytes(32))[0][0], 0)

    def test_same_image(self):
        """Two slots may hold the same image: the unit takes B's image for --running and A's
        for either write, and a plain write of A's own image is written and read back."""
        for img, flags in ((self.B_IMG, b'\x01'), (self.A_IMG, b'\x01'), (self.A_IMG, b'')):
            self.start('good', 'good')
            self.assertEqual(self.end_status(img, flags), 0)
        self.start('good', 'good')
        rc, text = self.write(self.A_IMG, ask=Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertIn('Verified: Slot 2 holds this file', text)
        self.assertEqual(self.slot(0, len(self.A_IMG)), self.A_IMG)
        self.assertEqual(self.slot(1, len(self.A_IMG)), self.A_IMG)
        self.assertEqual(self.selector(), (1, 0))

    def test_status_14(self):
        """The selector write before a write of the running slot does not store: 14, and
        nothing else is written."""
        self.start('good', 'good')
        self.lib.shim_ignore_selector(1)
        rc, text = self.write(make_image(seed=5), running=True, ask=Ask('yes'))
        self.assertEqual(rc, 1, text)
        self.assertIn('FAILED: the selector write before the write of the running slot failed; no '
                      'slot or record was written', text)
        self.assertEqual(self.slot(0, len(self.A_IMG)), self.A_IMG)
        self.assertEqual(self.slot(1, len(self.B_IMG)), self.B_IMG)

    # retries

    def test_retry_after_select_first(self):
        """A failed way-2 write: the selector leads to B, so either request writes A again (way 1),
        with no selector write before it, and the write settles the selector on A."""
        self.start('good', 'good')
        self.lib.shim_fail(0x10000 + 0x1000, 0x88000)      # A's second sector on
        img = make_image(seed=5, size=6000)
        rc, text = self.write(img, running=True, ask=Ask('yes'))
        self.assertEqual(rc, 1, text)
        self.assertIn('FAILED: a flash erase, program or read of the write failed', text)
        self.assertNotIn('the selector was not changed', text)
        self.assertIn('The failed write left Slot 1, the running slot, damaged.\nBut a restart starts '
                      'Slot 2, which the selector names. restart is accepted.', text)
        self.assertEqual(self.selector(), (1, 1))
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('(this recovery selected it, before a write of the running slot)', out.text)
        self.assertIn('Write:    would go to Slot 1, the running slot, again (way 1), with no '
                      'selector write before it', out.text)
        self.assertIn('Ways out: write and write --running both write Slot 1 again (way 1), with no '
                      'selector write before it, and end by settling the selector on Slot 1. Slot 2 '
                      'is not touched.', out.text)
        self.lib.shim_fail(0, 0)
        ask = Ask('yes')
        rc, text = self.write(img, running=True, ask=ask)
        self.assertEqual(rc, 0, text)
        self.assertIn('Writing %d bytes to Slot 1.' % len(img), text)
        self.assertIn('The unit announced a write of Slot 1, the running slot, with no selector write '
                      'before it (way 1): the selector leads to Slot 2, which is checked and is not '
                      'touched.\nThe write ends by settling the selector on Slot 1.', text)
        self.assertEqual(len(ask.prompts), 1)
        self.assertEqual(self.slot(0, len(img)), img)
        self.assertEqual(self.selector(), (0, 1))
        self.assertIn('The selector names Slot 1 with verified 01: the next start takes it as it is.',
                      text)
        self.assertNotIn('to be checked at the next start', text)

    def damage_a(self, run=0):
        """A failed in-place write: A is damaged and still selected."""
        self.start('good', 'bad', run=run)
        b = 1 - run
        self.lib.shim_fail(0x10000 + 0x78000 * b, 0x10000 + 0x78000 * (b + 1))
        rc, text = self.write(make_image(seed=6), ask=Ask('yes'))
        self.assertEqual(rc, 1, text)
        self.lib.shim_fail(0x10000 + 0x78000 * run + 0x1000, 0x10000 + 0x78000 * (run + 1))
        img = make_image(seed=5, size=6000)
        rc, text = self.write(img, running=True, ask=Ask(w.PHRASE))
        self.assertEqual(rc, 1, text)
        self.lib.shim_fail(0, 0)
        return img, text

    HELD = ("The unit holds the power switch's power-off and refuses a restart. Keep the cable in "
            'until a write has settled the selector (the ways out below); with no host, keep the '
            'unit powered.')
    NOTHING = 'The unit does not start at all if it is restarted or loses power now: no slot is checked.'

    @staticmethod
    def cut(slot):
        return ("A start that is not cut short would reach %s, but a cut of the first stage's own "
                'selector write could leave a selector that leads to no checked slot, so a restart is '
                'not safe yet.' % slot)

    def test_retry_after_in_place(self):
        """A failed in-place write: the unit refuses a restart and holds the power-off; a
        retry in place asks for the phrase again."""
        img, text = self.damage_a()
        self.assertIn('FAILED: a flash erase, program or read of the write failed\n', text)
        self.assertIn('The failed write left Slot 1, the running slot, damaged.\n' + self.NOTHING +
                      '\n' + self.HELD + '\nWays out: write writes Slot 2. write --running writes '
                      'Slot 1 again, in place. Each write ends by settling the selector on the slot '
                      'it wrote.', text)
        self.assertNotIn('Restart', text)
        self.assertNotIn('Power +', text)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('The failed write left Slot 1, the running slot, damaged', out.text)
        self.assertIn('with --running: would go to Slot 1, the running slot, in place', out.text)
        self.assertNotIn('the unit still starts as before', out.text)
        ask = Ask(w.PHRASE)
        rc, text = self.write(img, running=True, ask=ask)
        self.assertEqual(rc, 0, text)
        self.assertEqual(ask.prompts, ['Type "%s" to go on: ' % w.PHRASE])
        self.assertIn('Slot 1 is already damaged by a failed write of this recovery.\n' +
                      self.NOTHING + '\n' + self.HELD +
                      '\nThe write ends by settling the selector on Slot 1.\n', text)
        self.assertNotIn('The risk lasts', text)            # the selector reads 00 01
        self.assertNotIn('offers this only', text)
        self.assertEqual(self.slot(0, len(img)), img)
        self.assertEqual(self.selector(), (0, 1))
        self.assertEqual(w.cmd_restart(self.link, Lines()), 0)

    def test_plain_write_while_damaged(self):
        """A plain write of B while A is damaged: it states that an interruption leaves a unit
        that does not start, asks for yes, and succeeds; then a restart is taken."""
        self.damage_a()
        ask = Ask('yes')
        img = make_image(seed=8)
        rc, text = self.write(img, ask=ask)
        self.assertEqual(rc, 0, text)
        self.assertEqual(ask.prompts, ['Type yes to go on: '])
        self.assertIn('The failed write left Slot 1, the running slot, damaged. An interruption of '
                      'this write leaves no settled selector either: until a write has ended well, a '
                      'restart is not safe.\n' + self.NOTHING + '\n' + self.HELD +
                      '\nThe write ends by settling the selector on Slot 2.', text)
        self.assertEqual(self.slot(1, len(img)), img)
        self.assertEqual(self.selector(), (1, 1))
        out = Lines()
        self.assertEqual(w.cmd_restart(self.link, out), 0, out.text)
        time.sleep(0.05)
        self.assertTrue(self.lib.shim_restart())

    def test_restart_refused_while_damaged(self):
        """The tool sends no RESTART while bit 5 is set; the unit answers 0x44."""
        self.damage_a()
        self.seen = []
        out = Lines()
        self.assertEqual(w.cmd_restart(self.link, out), 1)
        self.assertIn('Not restarted: the unit refuses a restart: a write of this recovery left the '
                      'running slot damaged, or did not settle the selector on it, and a restart is '
                      'not safe yet.\nThe failed write left Slot 1, the running slot, damaged.\n' +
                      self.NOTHING + '\n' + self.HELD + '\nWays out: write writes Slot 2. write '
                      '--running writes Slot 1 again, in place.', out.text)
        self.assertNotIn(w.RESTART, self.seen)
        self.assertEqual(self.link.request(w.RESTART)[0][0], 0x44)
        time.sleep(0.05)
        self.assertFalse(self.lib.shim_restart())


    def test_in_place_busy(self):
        """S-02.084, S-02.093: during an in-place write every request but HELLO and INFO is
        answered busy, in its own reply layout. While the requests go, the front polls only when
        bytes come, so the write makes about one step a request, far fewer than its own steps,
        however slow the host is."""
        self.start('good', 'bad')
        self.fail_b()
        img = make_image(seed=5, size=60000)
        link = self.link
        w.hello(link)
        self.assertEqual(link.request(w.BEGIN, struct.pack('<I', len(img)) +
                                      hashlib.sha256(img).digest() + b'\x01')[0], bytes([0, 3, 0]))
        for off in range(0, len(img), 1024):
            self.assertEqual(link.request(w.DATA, struct.pack('<I', off) + img[off:off + 1024])[0][0], 0)
        self.free = False
        self.assertEqual(link.request(w.END)[0][0], 0)
        busy = ((w.BEGIN, struct.pack('<I', 10) + bytes(32)), (w.DATA, struct.pack('<I', 0) + b'x'),
                (w.END, b''), (w.RESTART, b''), (w.ACT, bytes([w.ACT_TRY, 0])))
        # the countdown first (S-02.084): busy too, and seen for 15 INFOs
        for typ, pl in busy:
            self.assertEqual(link.request(typ, pl)[0][0], 1)
        counts = []
        for _ in range(w.COUNTDOWN):
            i = w.get_info(link)
            counts.append((i['phase'], i['count']))
        self.assertEqual(counts, [(w.COUNTING, k) for k in range(w.COUNTDOWN, 0, -1)])
        for typ, pl in busy:
            self.assertEqual(link.request(typ, pl)[0][0], 1)
        # the C front's own version, as its header has it
        version = int(re.search(r'^#define FR_FRONT_VERSION\s+(\d+)u', open(os.path.join(
            TOP, 'src', 'front', 'recovery', 'proto.h')).read(), re.M).group(1))
        self.assertEqual(w.parse_hello(link.request(w.HELLO)[0])['front_version'], version)
        i = w.get_info(link)
        self.assertEqual((i['phase'], i['way']), (w.WRITING, w.WAY_IN_PLACE))
        self.assertLess(i['progress'], 500)
        self.assertIn('writing in place', '\n'.join(w.describe(i)))
        self.free = True
        for _ in range(500):
            i = w.get_info(link)
            if i['phase'] != w.WRITING:
                break
            time.sleep(0.01)
        self.assertEqual(i['phase'], w.DONE)
        self.assertFalse(self.lib.shim_restart())

    # the damaged state, decided from the flash (S-02.084)

    def case_1a(self):
        """Run 16's state, written into the flash: Slot 2 running and damaged by a failed in-place
        write, Slot 1 good, the selector FF FF. INFO's text is returned."""
        self.damage_a(run=1)
        good = make_image(seed=9, size=2000)
        self.place(0, good, True)
        self.lib.shim_flash(0xD000, b'\xff\xff', 2)
        out = Lines()
        w.cmd_info(self.link, out)
        return good, out.text

    def test_run16_held(self):
        """Run 16: a fallback to slot 0 does not count, so the unit holds and refuses a restart;
        the ways out name the selecting write and write --running."""
        good, text = self.case_1a()
        self.assertIn('The failed write left Slot 2, the running slot, damaged.\n' +
                      self.cut('Slot 1') + '\n' + self.HELD, text)
        self.assertIn('Ways out: write with the image Slot 1 holds (version 1.2.3+4) settles the '
                      'selector on Slot 1: the slot is not erased, only the selector is written. write '
                      '--running settles the selector on Slot 1 first and rewrites Slot 2. Each write '
                      'ends by settling the selector on the slot it wrote.', text)
        self.assertNotIn('Run info first', text)
        self.assertNotIn('Restart', text)
        i = w.get_info(self.link)
        self.assertEqual((i['spoiled'], i['held'], i['read_error']), (True, True, False))
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('Write:    would go to Slot 1, which takes only the image it holds (version '
                      '1.2.3+4, %d bytes, CRC-32 %08x)' % (len(good), zlib.crc32(good)), out.text)
        self.assertIn('and is then selected: the slot is not erased, only the selector is written',
                      out.text)
        self.assertNotIn('nothing is erased', out.text)
        self.assertEqual(w.cmd_restart(self.link, Lines()), 1)
        self.assertEqual(self.link.request(w.RESTART)[0][0], 0x44)

    def test_damaged_select_only(self):
        """Run 16: write with Slot 1's own image, from FF FF: no erase at all, the selector
        programmed to 00 01, and then a restart is accepted."""
        good, _ = self.case_1a()
        before = self.lib.shim_erases()
        ask = Ask('yes')
        rc, text = self.write(good, ask=ask)
        self.assertEqual(rc, 0, text)
        self.assertEqual(ask.prompts, ['Type yes to go on: '])
        self.assertIn('This settles the selector on Slot 1, which already holds this image (version '
                      '1.2.3+4). The slot '
                      'is not erased: the unit reads it back and then writes only the selector, to '
                      'name Slot 1. Slot 1 keeps its image if this is interrupted.', text)
        self.assertIn('The write ends by settling the selector on Slot 1.', text)
        self.assertNotIn('nothing is erased', text)
        self.assertEqual(self.lib.shim_erases(), before)    # S-02.084: programs alone
        self.assertEqual(self.slot(0, len(good)), good)
        self.assertEqual(self.selector(), (0, 1))
        self.assertIn('The selector names Slot 1 with verified 01: the next start takes it as it is.',
                      text)
        i = w.get_info(self.link)
        self.assertFalse(i['held'])
        self.assertEqual(w.cmd_restart(self.link, Lines()), 0)

    def test_damaged_other_image(self):
        """Run 16: write with another image is refused before BEGIN, with the two moves."""
        self.case_1a()
        self.seen = []
        rc, text = self.write(make_image(seed=11), ask=no_prompt)
        self.assertEqual(rc, 1)
        self.assertIn('Refused before sending: Slot 1 is the only checked slot, so the unit accepts only '
                      'the image it holds (version 1.2.3+4, ', text)
        self.assertIn('to settle the selector on it, and this file is not it (status 18).\nWays out: '
                      'write with the image Slot 1 holds', text)
        self.assertNotIn(w.BEGIN, self.seen)
        # the unit's own 18, past the tool's check
        img = make_image(seed=11)
        self.assertEqual(self.end_status(img), 18)

    def test_damaged_running_way_2(self):
        """Run 16: write --running goes by way 2, selector {00, 01} first, then slot 1, and ends
        with the selector settled on it."""
        self.case_1a()
        img = make_image(seed=12, size=3000)
        rc, text = self.write(img, running=True, ask=Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertIn('Writing %d bytes to Slot 2, the running slot, after the selector names Slot 1.'
                      % len(img), text)
        self.assertIn('The write ends by settling the selector on Slot 2.', text)
        self.assertEqual(self.slot(1, len(img)), img)
        self.assertEqual(self.selector(), (1, 1))
        self.assertIn('The selector names Slot 2 with verified 01', text)

    def run19(self):
        """Run 19's state: slot 1 damaged by a failed in-place write, slot 0 good, the selector
        00 00."""
        self.damage_a(run=1)
        self.place(0, self.B_IMG, True)
        self.lib.shim_flash(0xD000, b'\x00\x00', 2)

    def test_run19_held(self):
        """Run 19: slot 0 named with verified not 01 does not count while slot 1 is bad
        (S-02.084), so bit 5 is set and RESTART is refused."""
        self.run19()
        i = w.get_info(self.link)
        self.assertTrue(i['held'])
        self.assertEqual(self.link.request(w.RESTART)[0][0], 0x44)

    def test_run19(self):
        """Run 19: write --running is way 1 with no selector erase before slot 1 is written;
        the write ends with 01 01, and a restart is accepted."""
        self.run19()
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('The failed write left Slot 2, the running slot, damaged', out.text)
        self.assertIn('Ways out: write and write --running both write Slot 2 again (way 1), with no '
                      'selector write before it, and end by settling the selector on Slot 2.',
                      out.text)
        before = self.lib.shim_erases()
        img = make_image(seed=13, size=6000)
        ask = Ask('yes')
        rc, text = self.write(img, running=True, ask=ask)
        self.assertEqual(rc, 0, text)
        self.assertIn('The unit announced a write of Slot 2, the running slot, with no selector write '
                      'before it (way 1): the selector leads to Slot 1, which is checked and is not '
                      'touched.', text)
        self.assertIn('The write ends by settling the selector on Slot 2.', text)
        erased = [self.lib.shim_erase_at(k) for k in range(before, self.lib.shim_erases())]
        slot1 = [k for k, a in enumerate(erased) if 0x88000 <= a < 0x100000]
        self.assertTrue(slot1)
        self.assertNotIn(0xD000, erased[:slot1[-1] + 1])
        self.assertEqual(self.slot(1, len(img)), img)
        self.assertEqual(self.selector(), (1, 1))
        self.assertIn('The selector names Slot 2 with verified 01', text)
        self.assertEqual(w.cmd_restart(self.link, Lines()), 0)

    def test_run19_mirror(self):
        """Run 19 mirrored: slot 0 damaged, slot 1 good, the selector 01 00: not held, and a
        restart is accepted at once."""
        self.damage_a(run=0)
        self.place(1, self.B_IMG, True)
        self.lib.shim_flash(0xD000, b'\x01\x00', 2)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('The failed write left Slot 1, the running slot, damaged.\nBut a restart starts '
                      'Slot 2, which the selector names. restart is accepted.', out.text)
        out = Lines()
        self.assertEqual(w.cmd_restart(self.link, out), 0, out.text)
        time.sleep(0.05)
        self.assertTrue(self.lib.shim_restart())

    def test_case_1_held_both_mirrors(self):
        """S-02.084: the selector names the damaged A with verified 00 and A's record is sure to fail
        its check, so a first stage falls back to the good B: the selector leads to B. Both writes
        are announced as way 1 to A, and the unit holds."""
        for run in (0, 1):
            with self.subTest(run=run):
                self.damage_a(run=run)
                self.place(1 - run, self.B_IMG, True)
                self.lib.shim_flash(0xD000, bytes([run, 0]), 2)
                out = Lines()
                w.cmd_info(self.link, out)
                i = w.get_info(self.link)
                self.assertEqual((i['spoiled'], i['held']), (True, True))
                A, B = w.slot_name(run), w.slot_name(1 - run)
                self.assertIn('The failed write left %s, the running slot, damaged.\n%s\n%s'
                              % (A, self.cut(B), self.HELD), out.text)
                self.assertIn('Write:    would go to %s, the running slot, again (way 1), with no '
                              'selector write before it' % A, out.text)
                self.assertIn('Ways out: write and write --running both write %s again (way 1)' % A,
                              out.text)
                img = make_image(seed=14)
                sha = hashlib.sha256(img).digest()
                for flags in (b'', b'\x01'):
                    r = self.link.request(w.BEGIN, struct.pack('<I', len(img)) + sha + flags)[0]
                    self.assertEqual(r, bytes([0, w.WAY_PLAIN, run]))
                self.assertEqual(self.selector(), (run, 0))
                self.assertEqual(w.cmd_restart(self.link, Lines()), 1)

    def test_held_nothing_starts(self):
        """Bit 5, case (a): the selector trusts the damaged slot, so nothing starts at all."""
        self.damage_a(run=0)
        self.place(1, self.B_IMG, True)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('The failed write left Slot 1, the running slot, damaged.\nThe unit does not '
                      'start at all if it is restarted or loses power now: the selector trusts Slot '
                      '1, which is damaged.\n' + self.HELD, out.text)
        self.assertIn('Ways out: write with the image Slot 2 holds (version 0.9.0) settles the '
                      'selector on Slot 2', out.text)

    def run22(self):
        """Run 22: slot 0 running, slot 1 bad; a failed in-place write, then an in-place write
        whose settling takes the selector's erase and stores no index program: 11, slot 0 good,
        the selector FF FF."""
        self.damage_a(run=0)
        self.lib.shim_flash(0xD000, b'\x01\x00', 2)        # settling then erases the sector
        img = make_image(seed=21, size=4000)
        self.lib.shim_ignore_selector(1)
        rc, text = self.write(img, running=True, ask=Ask(w.PHRASE))
        self.lib.shim_ignore_selector(0)
        self.assertEqual(rc, 1, text)
        self.assertIn('FAILED: the slot was written and read back, but the selector write failed', text)
        self.assertEqual(self.selector(), (0xFF, 0xFF))
        return img, text

    def test_run22(self):
        img, text = self.run22()
        self.assertIn('A write of this recovery read Slot 1, the running slot, back checked but did not '
                      'settle the selector on it, and Slot 2 failed its check.\n' + self.cut('Slot 1') + '\n' +
                      self.HELD, text)
        i = w.get_info(self.link)
        self.assertEqual((i['spoiled'], i['held'], i['unsettled']), (False, True, True))
        self.assertEqual(w.cmd_restart(self.link, Lines()), 1)
        self.assertEqual(self.link.request(w.RESTART)[0][0], 0x44)
        # another image with the flag: refused before BEGIN, and 18 from the unit
        rc, out = self.write(make_image(seed=22), running=True, ask=no_prompt)
        self.assertEqual(rc, 1)
        self.assertIn('Refused before sending: Slot 1 is the only checked slot', out)
        self.assertEqual(self.end_status(make_image(seed=22), b'\x01'), 18)
        # without the flag: a write of slot 1
        sha = hashlib.sha256(img).digest()
        r = self.link.request(w.BEGIN, struct.pack('<I', len(img)) + sha)[0]
        self.assertEqual(r, bytes([0, w.WAY_PLAIN, 1]))
        # the flag with slot 0's own image: way 1, no erase, the selector settled
        before = self.lib.shim_erases()
        ask = Ask('yes')
        rc, out = self.write(img, running=True, ask=ask)
        self.assertEqual(rc, 0, out)
        self.assertIn('This settles the selector on Slot 1, which already holds this image (version '
                      '1.2.3+4). The slot is not erased', out)
        erased = [self.lib.shim_erase_at(k) for k in range(before, self.lib.shim_erases())]
        self.assertFalse([a for a in erased if a < 0x88000 and a != 0xD000])
        self.assertEqual(self.slot(0, len(img)), img)
        self.assertEqual(self.selector(), (0, 1))
        self.assertIn('The selector names Slot 1 with verified 01', out)
        self.assertFalse(w.get_info(self.link)['held'])
        self.assertEqual(w.cmd_restart(self.link, Lines()), 0)

    def test_run23(self):
        """Run 23: slot 1 running and damaged, slot 0 good, the selector 00 03: slot 0's own image
        is accepted and settles the selector by one program, no erase; another image is refused
        18; the flag writes slot 1 with no selector write first."""
        self.damage_a(run=1)
        self.place(0, self.B_IMG, True)
        self.lib.shim_flash(0xD000, b'\x00\x03', 2)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('Ways out: write with the image Slot 1 holds (version 0.9.0) settles the '
                      'selector on Slot 1: the slot is not erased, only the selector is written. '
                      'write --running writes Slot 2 again (way 1), with no selector write before it.',
                      out.text)
        rc, text = self.write(make_image(seed=23), ask=no_prompt)
        self.assertEqual(rc, 1)
        self.assertIn('Refused before sending: Slot 1 is the only checked slot', text)
        img = make_image(seed=23)
        r = self.link.request(w.BEGIN, struct.pack('<I', len(img)) + hashlib.sha256(img).digest() +
                              b'\x01')[0]
        self.assertEqual(r, bytes([0, w.WAY_PLAIN, 1]))
        before = self.lib.shim_erases()
        rc, text = self.write(self.B_IMG, ask=Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertIn('This settles the selector on Slot 1, which already holds this image (version '
                      '0.9.0)', text)
        self.assertEqual(self.lib.shim_erases(), before)
        self.assertEqual(self.selector(), (0, 1))
        self.assertEqual(w.cmd_restart(self.link, Lines()), 0)

    def test_run24(self):
        """Run 24: slot 0 running and damaged, slot 1 good, the selector 00 FE: not held; the
        restart text says the first restart starts the damaged slot; the flag is way 2, with the
        selector erased and written before slot 0's first erase."""
        self.damage_a(run=0)
        self.place(1, self.B_IMG, True)
        self.lib.shim_flash(0xD000, b'\x00\xfe', 2)
        i = w.get_info(self.link)
        self.assertEqual((i['held'], i['target'], i['run_way']), (False, 1, w.WAY_SELECT_FIRST))
        out = Lines()
        w.cmd_info(self.link, out)
        reach = ('But the first restart starts Slot 1, which the selector names and which is '
                 'damaged: it may hang or open recovery again. The restart after it starts Slot 2. '
                 'restart is accepted.')
        self.assertIn(reach, out.text)
        self.assertIn('write --running settles the selector on Slot 2 first and rewrites Slot 1.',
                      out.text)
        before = self.lib.shim_erases()
        img = make_image(seed=24, size=5000)
        rc, text = self.write(img, running=True, ask=Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertIn('after the selector names Slot 2', text)
        erased = [self.lib.shim_erase_at(k) for k in range(before, self.lib.shim_erases())]
        slot0 = [k for k, a in enumerate(erased) if 0x10000 <= a < 0x88000]
        self.assertLess(erased.index(0xD000), slot0[0])
        self.assertEqual(self.selector(), (0, 1))

    def test_run24_restart_text(self):
        self.damage_a(run=0)
        self.place(1, self.B_IMG, True)
        self.lib.shim_flash(0xD000, b'\x00\xfe', 2)
        out = Lines()
        self.assertEqual(w.cmd_restart(self.link, out), 0, out.text)
        self.assertIn('The failed write left Slot 1, the running slot, damaged. But the first restart '
                      'starts Slot 1, which the selector names and which is damaged: it may hang or '
                      'open recovery again. The restart after it starts Slot 2.', out.text)

    def test_in_place_settles_selector(self):
        """In place again with the selector torn: the warning says the risk lasts to the selector's
        settling, and the write ends with 00 01."""
        self.damage_a()
        self.lib.shim_flash(0xD000, b'\x00\x3f', 2)
        rc, text = self.write(make_image(seed=5, size=6000), running=True, ask=Ask(w.PHRASE))
        self.assertEqual(rc, 0, text)
        self.assertIn("The write ends by settling the selector on Slot 1.\nThe risk lasts until then, "
                      "not only to the slot's last program: the selector reads 00 3f now.", text)
        self.assertEqual(self.selector(), (0, 1))

    def test_read_error(self):
        """A read of the selector that fails once: bit 6, then normal at the next INFO."""
        self.damage_a()
        self.lib.shim_fail_reads(0xD000, 1)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn(w.READ_ERROR, out.text)
        self.assertIn('every 5 s', w.READ_ERROR)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertNotIn('A read of the selector', out.text)
        i = w.get_info(self.link)
        self.assertEqual((i['read_error'], i['held']), (False, True))


    def test_why_count(self):
        self.start('good', 'good', why=3)
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('Recovery runs because three starts in a row did not finish', out.text)
        self.assertIn('Write:    would go to Slot 2, replacing a checked image (version 0.9.0)', out.text)
        self.assertIn("with --running: would go to Slot 1, the running slot, after the selector "
                      "names Slot 2", out.text)
        self.assertIn('Battery:  unknown', out.text)

    def test_info_write_restart(self):
        out = Lines()
        self.assertEqual(w.cmd_info(self.link, out), 0, out.text)
        self.assertIn('Slot 1:   checked', out.text)
        self.assertIn('version 1.2.3+4', out.text)
        self.assertIn('would go to Slot 2', out.text)
        img = make_image(seed=5, size=5000)
        self.assertEqual(w.cmd_write(self.link, img, out, poll=0.01, ask=Ask('yes')), 0, out.text)
        self.assertIn('Verified: Slot 2', out.text)
        got = bytes(self.lib.shim_byte(0x88000 + i) for i in range(len(img)))
        self.assertEqual(got, img)
        self.assertEqual((self.lib.shim_byte(0xD000), self.lib.shim_byte(0xD001)), (1, 0))
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn('refused', out.text)                 # the selector is not settled now
        self.assertEqual(w.cmd_restart(self.link, out), 0)
        time.sleep(0.05)
        self.assertTrue(self.lib.shim_restart())

    def end_status(self, img, flags=b''):
        """BEGIN, DATA and END sent as they are, past the tool's own checks: END's status."""
        link = self.link
        w.hello(link)
        self.assertEqual(link.request(w.BEGIN, struct.pack('<I', len(img)) + hashlib.sha256(img).digest()
                                      + flags)[0][0], 0)
        for off in range(0, len(img), 1024):
            self.assertEqual(link.request(w.DATA, struct.pack('<I', off) + img[off:off + 1024])[0][0], 0)
        return link.request(w.END)[0][0]

    # front version 11: the actions on a slot's image, the force flag and the countdown

    def act(self, act, *answers, force=False):
        out = Lines()
        rc = w.cmd_act(self.link, act, out, poll=0.001, ask=Ask(*answers), force=force)
        return rc, out.text

    def record(self, k):
        return struct.unpack('<III', bytes(self.lib.shim_byte(0xE000 + 0x1000 * k + i)
                                           for i in range(12)))

    def test_try_put_back_switch(self):
        """try marks Slot 2 tried and writes the trial's selector; put-back makes its record good
        and settles the selector back on Slot 1; switch makes Slot 2 the boot slot. Each waits out the
        countdown."""
        self.start('good', 'good')
        n, crc = len(self.B_IMG), zlib.crc32(self.B_IMG)
        rc, text = self.act(w.ACT_TRY, 'yes')
        self.assertEqual(rc, 0, text)
        self.assertIn('Countdown:  1 s left', text)
        self.assertIn('Done: Slot 2 is marked for one trial start; the selector reads ff ff.', text)
        self.assertEqual(self.selector(), (0xFF, 0xFF))
        self.assertEqual(self.record(1), (crc ^ 0xFFFFFFFF, n, 0xB004))
        self.assertEqual([pl for t, pl in self.frames if t == w.ACT], [b'\x01\x00'])
        rc, text = self.act(w.ACT_PUT_BACK, 'yes')
        self.assertEqual(rc, 0, text)
        self.assertIn('it is first settled back on Slot 1', text)
        self.assertEqual(self.selector(), (0, 1))
        self.assertEqual(self.record(1), (crc, n, 0xB004))
        self.start('good', 'good')
        rc, text = self.act(w.ACT_SWITCH, w.SWITCH_PHRASE)
        self.assertEqual(rc, 0, text)
        self.assertEqual(self.selector(), (1, 0))
        self.assertEqual(self.slot(1, n), self.B_IMG)
        # a further action after it: restart before another write
        rc, text = self.act(w.ACT_TRY)
        self.assertEqual(rc, 1)
        self.assertIn('restart before another write', text)

    def test_keep_after_trial_start(self):
        """try, then the trial start as the first stage and the tried front leave it (the selector
        back on Slot 1, Slot 2's log finished), then keep from Slot 1's recovery."""
        self.start('good', 'good')
        self.assertEqual(self.act(w.ACT_TRY, 'yes')[0], 0)
        rc, text = self.act(w.ACT_KEEP)
        self.assertEqual(rc, 1)
        with self.lock:
            self.lib.shim_flash(0xD000, b'\x00\x01', 2)
            self.lib.shim_flash(0x10000 + 0x78000 + 0x77000, b'\xfc', 1)
            self.lib.shim_start(0, 1)
            self.seen = []
        out = Lines()
        w.cmd_info(self.link, out)
        self.assertIn(':   tried, started, ', out.text)
        self.assertIn('Keep:     would keep Slot 2, with no image sent', out.text)
        rc, text = self.act(w.ACT_KEEP, 'yes')
        self.assertEqual(rc, 0, text)
        self.assertIn('Kept: Slot 2 is the boot slot', text)
        self.assertEqual(self.selector(), (1, 0))
        self.assertEqual(self.record(1), (zlib.crc32(self.B_IMG), len(self.B_IMG), 0xB004))

    def test_forced_switch_to_bad(self):
        """A switch to a bad slot: refused without --force (23); forced, the DANGER warning, the
        phrase, and the selector names it with verified 01."""
        self.start('good', 'bad')
        rc, text = self.act(w.ACT_SWITCH)
        self.assertEqual(rc, 1)
        self.assertIn('Refused before sending: Slot 2 failed its check and is not tried', text)
        self.assertNotIn(w.ACT, self.seen)
        rc, text = self.act(w.ACT_SWITCH, 'force the switch', force=True)
        self.assertEqual(rc, 0, text)
        self.assertIn('DANGER: Slot 2 failed its check', text)
        self.assertEqual([pl for t, pl in self.frames if t == w.ACT], [b'\x02\x80'])
        self.assertEqual(self.selector(), (1, 1))

    def test_transfer_left(self):
        """The host leaves in the middle of a transfer (fr_proto_drop()): the front ends it with
        status 24 and nothing written, takes no more of it, and the next write begins anew."""
        self.start('good', 'good')
        img = make_image(seed=9, size=3000)
        r = self.link.request(w.BEGIN, struct.pack('<I', len(img)) + hashlib.sha256(img).digest())
        self.assertEqual(r[0][0], 0)
        self.assertEqual(self.link.request(w.DATA, struct.pack('<I', 0) + img[:1024])[0][0], 0)
        with self.lock:
            self.lib.shim_drop()
        i = w.get_info(self.link)
        self.assertEqual((i['phase'], i['result']), (w.FAILED, 24))
        self.assertEqual(self.link.request(w.DATA, struct.pack('<I', 1024) + img[1024:2048])[0][0],
                         0x41)
        self.assertEqual(self.slot(1, len(self.B_IMG)), self.B_IMG)
        rc, text = self.write(img, ask=Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertEqual(self.slot(1, len(img)), img)

    def test_forced_write_selected_bad(self):
        """S-02.084: a write of the running slot under a selector that names a bad slot is refused
        with 5, forced too; with that slot good, forced, it goes to the running slot."""
        img = make_image(seed=9, size=3000)
        begin = struct.pack('<I', len(img)) + hashlib.sha256(img).digest()
        self.start('good', 'bad', sel=(1, 1))
        self.assertEqual(self.link.request(w.BEGIN, begin)[0][0], 3)
        self.assertEqual(self.link.request(w.BEGIN, begin + b'\x80')[0][0], 5)
        self.start('good', 'good', sel=(1, 1))
        self.assertEqual(tuple(self.link.request(w.BEGIN, begin + b'\x80')[0][:3]),
                         (0, w.WAY_PLAIN, 0))

    def test_countdown_stopped(self):
        """The host leaves during the countdown (fr_proto_drop()): nothing is written, INFO shows
        status 24, and the tool says so."""
        self.start('good', 'good')
        self.drop_after = 5
        rc, text = self.act(w.ACT_TRY, 'yes')
        self.assertEqual(rc, 1, text)
        self.assertIn('Stopped: the write was stopped before its first erase', text)
        self.assertEqual(self.selector(), (0, 1))
        self.assertEqual(self.record(1), (zlib.crc32(self.B_IMG), len(self.B_IMG), 0xB004))
        i = w.get_info(self.link)
        self.assertEqual((i['phase'], i['result']), (w.FAILED, 24))

    def test_second_try_stopped(self):
        """A second try, forced over the first one's selector and stopped in its countdown: the
        front keeps the first try in INFO with byte 231 at 24, and the tool reports the stop,
        not the first try as done."""
        self.start('good', 'good')
        self.assertEqual(self.act(w.ACT_TRY, 'yes')[0], 0)
        self.drop_after = self.infos + 3
        rc, text = self.act(w.ACT_TRY, 'force the try', force=True)
        self.assertEqual(rc, 1, text)
        self.assertIn('Stopped: the write was stopped before its first erase', text)
        self.assertIn('(Slot 2 marked for one trial start); nothing of this one was written.', text)
        self.assertNotIn('Done:', text)
        i = w.get_info(self.link)
        self.assertEqual((i['phase'], i['way'], i['act'], i['refused']),
                         (w.DONE, w.WAY_TRY, w.ACT_TRY, 24))

    def test_damaged_no_countdown(self):
        """While the damaged state holds the power-off, a write begins at once."""
        self.damage_a()
        img = make_image(seed=8)
        rc, text = self.write(img, ask=Ask('yes'))
        self.assertEqual(rc, 0, text)
        self.assertIn('The unit begins at once, with no countdown', text)
        self.assertIn('The unit began at once, with no countdown', text)
        self.assertNotIn('Countdown', text)
        self.assertEqual(self.slot(1, len(img)), img)

    def test_forced_restart(self):
        """restart --force while the unit holds: RESTART {0x80}, and the unit restarts."""
        self.damage_a()
        out = Lines()
        self.assertEqual(w.cmd_restart(self.link, out, ask=Ask('force the restart'), force=True), 0,
                         out.text)
        self.assertEqual([pl for t, pl in self.frames if t == w.RESTART], [b'\x80'])
        time.sleep(0.05)
        self.assertTrue(self.lib.shim_restart())

    def test_scan_agrees(self):
        """The tool's front check and the unit's scan() agree on the crafted files."""
        for img, want in ((make_image(), 0), (STRADDLE, 7), (FILLED, 7)):
            self.assertEqual(self.lib.fr_image_valid(img, len(img), None), want)
            self.assertEqual(w.check_image(img)[0] == [], want == 0)

    def test_refused_by_the_unit(self):
        """S-02.084 at END: another front (12); nothing written."""
        self.assertEqual(self.end_status(make_image(seed=5, front=2)), 12)
        self.assertEqual(self.lib.shim_byte(0x88000), 0xFF)
        self.assertEqual((self.lib.shim_byte(0xD000), self.lib.shim_byte(0xD001)), (0, 1))
        i = w.parse_info(self.link.request(w.INFO)[0])
        self.assertIn("the image's front is not the one this unit runs", w.status_text(i['result']))

    def test_data_size(self):
        """S-02.093: 1024 image bytes a DATA; 1025 is a malformed request."""
        img = make_image(seed=5, size=3000)
        w.hello(self.link)
        self.assertEqual(self.link.request(w.BEGIN, struct.pack('<I', len(img)) +
                                           hashlib.sha256(img).digest())[0][0], 0)
        r = self.link.request(w.DATA, struct.pack('<I', 0) + img[:1025])[0]
        self.assertEqual((r[0], struct.unpack_from('<I', r, 1)[0]), (0x40, 0))
        r = self.link.request(w.DATA, struct.pack('<I', 0) + img[:1024])[0]
        self.assertEqual((r[0], struct.unpack_from('<I', r, 1)[0]), (0, 1024))

    def test_selector_not_written(self):
        """The flash takes the selector program without storing it: the unit reports 11 and the
        tool says the write failed."""
        self.lib.shim_ignore_selector(1)
        out = Lines()
        self.assertEqual(w.cmd_write(self.link, make_image(seed=5, size=2000), out, poll=0.01, ask=Ask('yes')), 1, out.text)
        self.assertIn('selector write failed or did not read back', out.text)
        self.assertNotIn('Verified:', out.text)


if __name__ == '__main__':
    unittest.main()
