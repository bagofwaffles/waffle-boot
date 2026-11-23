# SPDX-License-Identifier: GPL-2.0-or-later
"""A simulated installer (waffle-install.ldr) for the recovery page's Install flow: it speaks the
installer's read-only USB protocol (recovery's framing, types 0x40 to 0x43) and walks its phases on
a timer of its own and on the controls a person would use on the unit:

    act('hold')      the step's key held for 5 s (the hold bar fills, then the step runs)
    act('leave')     8 held for 5 s: return to the old OS without installing, where it is offered
                     (the key check included)
    act('retrial')   2 held for 5 s on the commit screen: the same trial written again
    act('release')   the key let go before the 5 s
    act('restart')   switched off and on: the port closes, and is gone for a moment
    act('reset')     the unit as it was at the start

A spec (a dict) picks the case and the speed: {"kind": "installer", "case": CASE, "tick": 1.0}.
tick scales every wait (0.05 runs the flow twenty times as fast). CASES lists the cases.
attach(fd) serves the byte stream on fd; an earlier stream is closed. Python 3, standard library
only.
"""
import os
import socket
import struct
import threading
import time
import zlib

IHELLO, STATE, TEST, REPORT = 0x40, 0x41, 0x42, 0x43
MAX_PAYLOAD = 1032
NONE = 0xFF

STARTING, CHECKING, GUIDED, LOCKED, READY, WRITING_TRIAL, TRIAL_WRITTEN, COMMIT_READY, \
    COMMITTING, DONE, FAILED, TRIAL_FAILED, LEFT = range(13)

F_CHECKS_DONE, F_UNLOCKED, F_IMAGE_OK, F_BREADCRUMB = 1, 2, 4, 8
SB_GOOD, SB_TRIAL_IMAGE, SB_COMMITTED, SB_PENDING, SB_STARTED, SB_SPENT, SB_ODD, SB_REACHED = \
    1, 2, 4, 8, 16, 32, 64, 128
PASS, FAIL, NOT_TESTED = 1, 2, 3

# id, name, kind (0 OS, 1 automatic, 2 guided), detail when it passes
TESTS = [
    ('os-partition', 'OS partition', 0, 'the OS is in the other boot partition'),
    ('clocks', 'Clocks', 1, 'core 400 MHz, system 200 MHz'),
    ('ddr', 'DDR', 1, '64 MB, every pattern read back'),
    ('core-state', 'Core state', 1, 'as the boot ROM leaves it'),
    ('flash-id', 'Flash id', 1, 'SPI flash answers, 1 MB'),
    ('flash-layout', 'Flash layout', 1, 'both slots and the selector read'),
    ('psoc', 'PSoC', 1, 'answers'),
    ('slave', 'Second processor', 1, 'started and answers'),
    ('bridge', 'Bridge', 1, 'round trip 1.2 ms'),
    ('emmc-config', 'eMMC config', 1, 'the eMMC answers'),
    ('emmc-boot', 'eMMC boot', 1, 'both boot partitions read'),
    ('emmc-volume', 'eMMC volume', 1, 'the volume mounts'),
    ('emmc-file', 'eMMC file', 1, 'a file written, read and removed'),
    ('panel', 'Panel', 1, 'the display answers'),
    ('rtc', 'Clock chip', 1, 'ticks'),
    ('nordic', 'Radio', 1, 'answers'),
    ('timing', 'Timing', 1, 'start to menu 6.3 s'),
    ('keys', 'Keys', 2, 'com and M4 pressed'),
]
N = len(TESTS)
KEYS = [t[0] for t in TESTS].index('keys')

CASES = {
    'pass': {},
    'check-fail': {'fail': 'emmc-volume', 'detail': 'the volume does not mount',
                   'note': 'mount returned an I/O error on the second read'},
    'trial-fail': {'trial': 3},
    'no-os': {'os': 3},
    'write-fail': {'write_fail': 4},
    'breadcrumb': {'breadcrumb': True},
    'keys-ended': {'keys_ended': True},
}

VERSION = (1, 0, 3, 0)
IMAGE_LEN = 0x5A3C0
IMAGE_CRC = 0x8E1F4C27
PIN = bytes(range(0xA0, 0xC0))
TARGET = 1


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def frame(typ, seq, payload):
    head = bytes([typ, seq]) + struct.pack('<H', len(payload)) + payload
    return b'WR' + head + struct.pack('<I', crc32(head))


def take_frame(buf):
    """(frame or None, the bytes left): bytes that do not start a good frame are dropped up to the
    next "WR", as recovery's parser drops them."""
    while True:
        at = buf.find(b'WR')
        if at < 0:
            return None, buf[-1:] if buf.endswith(b'W') else b''
        buf = buf[at:]
        if len(buf) < 6:
            return None, buf
        n = struct.unpack_from('<H', buf, 4)[0]
        if n > MAX_PAYLOAD:
            buf = buf[1:]
            continue
        if len(buf) < 6 + n + 4:
            return None, buf
        if crc32(buf[2:6 + n]) != struct.unpack_from('<I', buf, 6 + n)[0]:
            buf = buf[1:]
            continue
        return (buf[2], buf[3], buf[6:6 + n]), buf[6 + n + 4:]


class InstallerUnit:
    def __init__(self, spec, fd=None):
        self.case = spec.get('case', 'pass')
        self.cfg = CASES.get(self.case, CASES['pass'])
        self.tick = float(spec.get('tick', 1.0))
        self.lock = threading.RLock()
        self.sock = None
        self.off_until = 0.0
        self.os_started = False     # switched on after an end: the OS runs, not the installer
        self.asked = []             # every request type, in order
        self.phases = []            # every phase entered, in order
        self.stop = threading.Event()
        self.reset()
        threading.Thread(target=self.run, daemon=True).start()
        if fd is not None:
            self.attach(fd)

    # --- the unit's state ---

    def reset(self):
        with self.lock:
            self.phase = None
            self.os_started = False
            self.quiet_from = 0.0
            self.enter(STARTING)
            self.flags = F_IMAGE_OK | (F_BREADCRUMB if self.cfg.get('breadcrumb') else 0)
            self.verdict = NONE
            self.current = NONE
            self.lock_reason = 0
            self.sel = [0, 1]
            self.slots = [SB_GOOD, 0]
            self.target = NONE
            self.result = NONE
            self.os = 0
            self.hold = 0
            self.hold_key = 0
            self.holding = False
            self.trial = 0
            self.tests = [0] * N
            self.details = [('', '')] * N
            self.next_at = time.monotonic() + 0.8 * self.tick

    def enter(self, phase):
        self.phase = phase
        self.phases.append(phase)

    def wait(self, s):
        self.next_at = time.monotonic() + s * self.tick

    def can_leave(self):
        return self.phase in (GUIDED, LOCKED, READY, COMMIT_READY, DONE, FAILED, TRIAL_FAILED) and \
            self.os in (1, 2)

    def write_for(self, s):
        """A write of s ticks; after its first moment the port is not served until it ends."""
        self.wait(s)
        self.quiet_from = time.monotonic() + 0.6 * self.tick

    def quiet(self):
        return self.phase in (WRITING_TRIAL, COMMITTING) and time.monotonic() >= self.quiet_from

    def next_key(self):
        return 1 if self.phase in (READY, COMMIT_READY, TRIAL_FAILED) else 0

    def set_test(self, k, st, detail='', note=''):
        self.tests[k] = st
        self.details[k] = (detail, note)

    def finish_checks(self):
        self.flags |= F_CHECKS_DONE
        self.current = NONE
        hw = self.tests[1:]
        self.verdict = 2 if FAIL in hw else 1 if NOT_TESTED in hw else 0
        # what the other boot partition holds plays no part (S-16.027); locks 5 and 6 are never set
        lock = 0
        if FAIL in hw:
            lock = 1
        elif NOT_TESTED in hw:
            lock = 2
        elif not self.flags & F_IMAGE_OK:
            lock = 3
        if lock:
            self.lock_reason = lock
            self.enter(LOCKED)
        else:
            self.flags |= F_UNLOCKED
            self.target = TARGET
            self.enter(READY)

    def step(self):
        """One move of the timer: the checks, then the writes that follow a hold."""
        p = self.phase
        if p == STARTING:
            self.enter(CHECKING)
            self.current = 0
            self.wait(0.3)
        elif p in (CHECKING, GUIDED):
            k = self.current
            tid, name, kind, ok = TESTS[k]
            if k == 0:
                self.os = self.cfg.get('os', 1)
                if self.os == 1:
                    self.set_test(0, PASS, ok)
                else:
                    self.set_test(0, FAIL, 'no OS in the other boot partition')
            elif tid == 'keys' and self.cfg.get('keys_ended'):
                # the key check waits for the person: act('leave') returns to the old OS
                self.wait(0.3)
                return
            elif tid == self.cfg.get('fail'):
                self.set_test(k, FAIL, self.cfg['detail'], self.cfg.get('note', ''))
            else:
                self.set_test(k, PASS, ok)
            if k + 1 < N:
                self.current = k + 1
                if TESTS[k + 1][2] == 2 and p == CHECKING:
                    self.flags |= F_CHECKS_DONE
                    self.enter(GUIDED)
                self.wait(1.0 if TESTS[k + 1][2] == 2 else 0.3)
            else:
                self.finish_checks()
        elif p == WRITING_TRIAL:
            code = self.cfg.get('write_fail')
            if code:
                self.result = code
                self.enter(FAILED)
            else:
                self.result = 0
                self.trial = 1
                self.slots[TARGET] = SB_TRIAL_IMAGE | SB_PENDING
                self.enter(TRIAL_WRITTEN)
        elif p == COMMITTING:
            self.result = 0
            self.slots[TARGET] = SB_GOOD | SB_COMMITTED
            self.sel = [TARGET, 1]
            self.os = 6
            self.enter(DONE)

    def held(self):
        """The key was held its 5 s: the step it starts."""
        key = self.hold_key
        self.hold, self.hold_key, self.holding = 0, 0, False
        if key == 8:
            if self.phase == GUIDED:
                # the key check left with 8 held: no install offered
                self.set_test(KEYS, NOT_TESTED, 'returned with 8; com not pressed, M4 not pressed')
                self.flags |= F_CHECKS_DONE
                self.current = NONE
                self.verdict = 1
            self.os = 6
            self.enter(LEFT)
        elif key == 2 and self.phase == COMMIT_READY:
            # the first trial's write again, over the same slot
            self.enter(WRITING_TRIAL)
            self.write_for(3.0)
        elif self.phase in (READY, TRIAL_FAILED):
            self.enter(WRITING_TRIAL)
            self.write_for(3.0)
        elif self.phase == COMMIT_READY:
            if self.flags & F_BREADCRUMB:
                self.result = 0x40
                self.enter(FAILED)
            else:
                self.enter(COMMITTING)
                self.write_for(3.0)

    def run(self):
        while not self.stop.wait(0.05):
            with self.lock:
                now = time.monotonic()
                if self.holding:
                    self.hold = min(1000, self.hold + int(1000 * 0.05 / (5.0 * self.tick)) + 1)
                    if self.hold >= 1000:
                        self.held()
                elif now >= self.next_at and self.phase in (STARTING, CHECKING, GUIDED,
                                                            WRITING_TRIAL, COMMITTING):
                    self.step()

    def act(self, what):
        with self.lock:
            if what == 'hold' and self.next_key():
                self.holding, self.hold_key, self.hold = True, self.next_key(), 0
            elif what == 'leave' and self.can_leave():
                self.holding, self.hold_key, self.hold = True, 8, 0
            elif what == 'retrial' and self.phase == COMMIT_READY:
                self.holding, self.hold_key, self.hold = True, 2, 0
            elif what == 'release':
                self.holding, self.hold_key, self.hold = False, 0, 0
            elif what == 'reset':
                self.detach()
                self.reset()
            elif what == 'restart':
                self.restart()
            else:
                return False
            return True

    def restart(self):
        """Switched off and on: the port goes away; a written trial starts once, its boot menu
        restarts into the OS, and the installer is back."""
        self.detach()
        self.off_until = time.monotonic() + 1.5 * self.tick
        self.holding, self.hold, self.hold_key = False, 0, 0
        if self.phase == TRIAL_WRITTEN:
            b = self.slots[TARGET] & ~SB_PENDING | SB_STARTED | SB_SPENT
            self.trial = self.cfg.get('trial', 2)
            # back after the trial start: the key check is not asked again
            self.set_test(KEYS, PASS, 'passed before the trial, not asked again')
            if self.trial == 2:
                self.slots[TARGET] = b | SB_REACHED
                self.enter(COMMIT_READY)
            else:
                self.slots[TARGET] = b
                self.enter(TRIAL_FAILED)
        elif self.os == 6:
            self.os_started = True      # the OS is active: no installer port until a reset
        elif self.phase in (LOCKED, FAILED):
            self.reset()                # the installer again, from its first check

    # --- the protocol ---

    def state_reply(self):
        b = bytes([self.phase, self.flags, self.verdict, self.current, self.lock_reason,
                   self.sel[0], self.sel[1], self.slots[0], self.slots[1], self.target, self.result,
                   self.os]) + struct.pack('<H', self.hold) + \
            bytes([self.hold_key, self.trial, self.next_key()]) + bytes(15)
        return b + bytes(self.tests)

    def report(self):
        lines = ['waffle-boot installer check', 'verdict: %s' % {0: 'PASS', 1: 'PASS WITH GAPS',
                                                                  2: 'FAIL', NONE: 'not yet'}[self.verdict]]
        words = ['not run', 'pass', 'FAIL', 'not tested', 'not passed']
        for k, (tid, _, _, _) in enumerate(TESTS):
            d, note = self.details[k]
            lines.append('%-14s %-10s %s' % (tid, words[self.tests[k]], d))
            if note:
                lines.append('               %s' % note)
        lines += ['# filler so that the report needs more than one chunk'] * 30
        return ('\n'.join(lines) + '\n').encode('ascii')

    def answer(self, typ, pl):
        with self.lock:
            self.asked.append(typ)
            if typ == IHELLO:
                return b'WBIN' + bytes([1, self.phase, self.flags, N]) + \
                    struct.pack('<4HII', *VERSION, IMAGE_LEN, IMAGE_CRC) + PIN + bytes(8)
            if typ == STATE:
                return self.state_reply()
            if typ == TEST:
                if len(pl) != 1 or pl[0] >= N:
                    return b'\x01'
                k = pl[0]
                tid, name, kind, _ = TESTS[k]
                d, note = self.details[k]
                return bytes([0, k, self.tests[k], kind]) + b'\0'.join(
                    s.encode('ascii') for s in (tid, name, d, note)) + b'\0'
            if typ == REPORT:
                if len(pl) != 4:
                    return b'\x01'
                off = struct.unpack('<I', pl)[0]
                text = self.report()
                return b'\0' + struct.pack('<I', len(text)) + text[off:off + 1024]
            return b'\xff'

    def attach(self, fd):
        """Serves the stream on fd; a stream attached while the unit is off is closed at once."""
        s = socket.socket(fileno=os.dup(fd))
        with self.lock:
            if time.monotonic() < self.off_until or self.os_started:
                s.shutdown(socket.SHUT_RDWR)
                s.close()
                return
            self.detach()
            self.sock = s
        threading.Thread(target=self.serve, args=(s,), daemon=True).start()

    def detach(self):
        s, self.sock = self.sock, None
        if s is not None:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def serve(self, s):
        buf = b''
        try:
            while True:
                data = s.recv(65536)
                if not data:
                    break
                buf += data
                while True:
                    got, buf = take_frame(buf)
                    if got is None:
                        break
                    typ, seq, pl = got
                    if self.quiet():
                        continue        # a write under way: the port is not served
                    s.sendall(frame(typ | 0x80, seq, self.answer(typ, pl)))
        except OSError:
            pass
        finally:
            s.close()

    def set(self, attrs):
        with self.lock:
            for k, v in attrs.items():
                setattr(self, k, v)

    def state(self):
        with self.lock:
            return {'phase': self.phase, 'phases': list(self.phases), 'asked': list(self.asked)}

    def close(self):
        self.stop.set()
        with self.lock:
            self.detach()
