#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The host's end of waffle-boot's host link (wiki/Developer-Host-Link.md): a client for the boot
menu's serial port, and a command line over it.

    hostlink.py [--port /dev/cu.usbmodemNNN] COMMAND...

    hello                 the protocol's version, the unit's versions, calls, actions and roots
    info                  versions, device, running slot, start reason and settings
    session               the session: none, pending, approved, refused, timed out
    approve [S]           ask the unit; wait up to S seconds (default 70) for the hold on it
    end                   end the session
    ls PATH               a directory: /yaffs2/... (the drive), /os (the OS copies, read only),
                          /user/... (the OS's FAT volume, on the OP-XY)
    stat PATH             one entry
    get PATH OUT          a file to OUT, resumed from OUT's length when it is shorter
    hash PATH             a file's SHA-256 and size, read on the unit
    put FILE PATH         FILE to PATH on /yaffs2 or /user, checked whole before it takes the name
    rm PATH, mkdir PATH, mv FROM TO
    install PATH [PARTS]  install the package PATH on the drive; PARTS the steps' bits (default
                          0xFF: every part the unit would offer). Protected writes applies;
                          a package whose signature does not match installs with no prompt
                          on the unit, its job's text ending in "(signature invalid)"
    os-write PATH         write the OS image PATH from the drive to the inactive boot partition
    os-once               restart once into /yaffs2/os.ldr (writes nothing)
    ram-boot FILE         send the OS image FILE into the unit's RAM, checked whole, and start it
                          from there: no file, boot partition or flash is written
    restart, continue-os, restart-mode MODE
    host-link-off         turn the host link off for this start

  The PSoC (the backplane) for a computer's own PSoC bootloader tool. These need Host link on but no
  approval: the unit shows a warning and counts down 15 s first; switching it off, or its key 8 while
  keys work, stops it. While the bridge is open the unit's keys do not work.

    psoc-open [--no-entry] [S]  give the computer the PSoC link, raw, once the countdown is over: the
                          unit first sends the PSoC's bootloader-entry message, or with --no-entry
                          nothing (a PSoC already in its bootloader); wait up to S seconds (default 30)
    psoc-raw-tx HEX [S]   send the bytes to the PSoC and print what comes back within S seconds
                          (default 0.5); HEX may hold spaces
    psoc-close            end the bridge: the link framed again and the PSoC greeted. The unit sends no
                          exit packet: send the bootloader's own first if the application is to start
    psoc-state            the bridge's state, its countdown and the bytes lost
    psoc-reset            reset the PSoC after the countdown, with the plain restart's reset request:
                          the whole unit is switched off and on, and the session ends. A PSoC that has
                          not greeted gets the request alone while the unit runs on; if the unit is
                          still running 10 s later the PSoC did not act on it, and the tool says so

  PsocBridge in this file is the same as a Python interface: open(), send(), recv(), exchange(), close().

Python 3, standard library only. Changes and every read but list and stat on the drive need the
session's approval: `approve` first. With Developer mode's "4 Host link without hold" on, the unit
approves at once ("approved, no hold"); an install still asks for the hold.
"""
import glob
import hashlib
import os
import select
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from testos_mon import frame, unframe  # noqa: E402

ID = 0x7C
VERSION = 2
OLDEST_UNIT = 1     # a unit of version 1 has no RAM calls; the rest is the same
HELLO, SESSION, APPROVE, END, INFO, SET, JOB = range(7)
STAT, LIST, GET, HASH, PUT_OPEN, PUT_DATA, PUT_END, REMOVE, MKDIR, RENAME = range(0x10, 0x1A)
RUN, EVENT = 0x20, 0x7F
RAM_OPEN, RAM_DATA, RAM_END = 0x21, 0x22, 0x23
PSOC_OPEN, PSOC_DATA, PSOC_CLOSE, PSOC_STATE, PSOC_RESET = range(0x30, 0x35)
PSOC_EVENT = 0x7E
PSOC_REC = 10
PSOC_TX_MAX = 247
PSOC_COUNT_S = 15
PSOC_RESET_WAIT_S = 10
PSOC_STATES = ['none', 'counting down', 'open', 'canceled', 'ended', 'resetting']
PSOC_WHAT = ['none', 'bridge, entry message first', 'bridge', 'reset']
A_INSTALL, A_OS_WRITE, A_OS_ONCE, A_RESTART, A_CONTINUE_OS, A_RESTART_MODE, A_RAM_START = range(1, 8)
SET_HOST_LINK = 7
CHUNK = 224
STATUS = ['ok', 'host link off', 'not approved: hold 1 on the unit', 'never from a host', 'malformed',
          'not in this build', 'path refused', 'not found', 'exists', 'I/O error', 'busy',
          'checksum mismatch', 'offset', 'volume not answering', 'read only', 'refused by the unit',
          'volume not readable by this build', 'the PSoC bridge is not open']
SESSIONS = ['none', 'pending', 'approved', 'refused', 'timed out', 'approved, no hold']
APPROVED = ('approved', 'approved, no hold')
# S-05.024's refusals, as RAM_END names them
REFUSALS = ['accepted', 'longer than the staging area', 'not an LDR stream', 'refused flags',
            'a destination outside L1, L2 and DDR', 'over the staging area', 'over the reserve',
            'over the record', "over the ROM's boot reserve"]
JOB_STATES = ['none', 'queued', 'running', 'done', 'failed']
DEVICES = {0: 'OP-XY', 1: 'OP-1 Field'}
ROOT_FLAGS = ((1, 'read'), (2, 'write'), (4, 'open list'))


class HostLinkError(Exception):
    def __init__(self, call, status, data=b''):
        super().__init__('call 0x%02x: %s' % (call, STATUS[status] if status < len(STATUS) else status))
        self.call, self.status, self.data = call, status, data


class SerialPort:
    """The boot menu's USB serial port, raw."""

    def __init__(self, path):
        import termios
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def write(self, data):
        os.write(self.fd, data)

    def read(self, timeout):
        ready, _, _ = select.select([self.fd], [], [], timeout)
        return os.read(self.fd, 4096) if ready else b''


def job_record(b):
    """id action state step percent error, then the text."""
    return {'id': b[0], 'action': b[1], 'state': JOB_STATES[b[2]] if b[2] < len(JOB_STATES) else b[2],
            'step': b[3], 'percent': None if b[4] == 0xFF else b[4], 'error': b[5],
            'text': bytes(b[6:]).split(b'\0')[0].decode('utf-8', 'replace')}


def info_record(b):
    """HL_INFO's 24 bytes."""
    v = struct.unpack_from('<4HHBBHH4H', b)
    f = v[8]
    return {'bootloader': '%d.%d.%d+%d' % v[0:4], 'front': v[4],
            'device': DEVICES.get(v[5], 'unknown'), 'running_slot': None if v[6] == 0xFF else v[6] + 1,
            'reason': v[7], 'os': '%d.%d.%d+%d' % v[9:13],
            'protected_writes': bool(f & 1), 'developer_mode': bool(f & 2), 'color': bool(f & 4),
            'mtp_macos': bool(f & 8), 'start_in_boot_menu': bool(f & 16), 'host_link': bool(f & 32),
            'second_processor': bool(f & 64), 'drive_mounted': bool(f & 128)}


def psoc_record(b):
    """state seconds flags overruns held, and what was asked."""
    st, what, secs, flags, over, held = struct.unpack_from('<BBBBIH', b)
    return {'state': PSOC_STATES[st] if st < len(PSOC_STATES) else st,
            'what': PSOC_WHAT[what] if what < len(PSOC_WHAT) else what, 'seconds': secs,
            'greeted': bool(flags & 1), 'greeted_when_asked': bool(flags & 2),
            'reset_alone': bool(flags & 4), 'not_reset': bool(flags & 8), 'overruns': over,
            'held': held}


class Client:
    """Requests and replies on a transport with write(bytes) and read(timeout) -> bytes."""

    def __init__(self, port):
        self.port, self.rx, self.seq, self.tag = port, bytearray(), 0, 0
        self.on_event = None
        self.last_event = None
        self.on_psoc = None
        self.last_psoc = None

    def _frames(self, end):
        while True:
            while b'\0' in self.rx:
                wire, _, rest = bytes(self.rx).partition(b'\0')
                self.rx = bytearray(rest)
                got = unframe(wire)
                if got and got[0] == ID:
                    yield got[1]
            left = end - time.monotonic()
            if left <= 0:
                return
            self.rx += self.port.read(min(left, 0.05))

    def call(self, op, args=b'', timeout=5.0, ok=(0,)):
        """(status, data) of one call; HostLinkError for a status not in ok."""
        self.seq = (self.seq + 1) & 0xFF
        self.tag = (self.tag + 1) & 0xFF
        self.port.write(frame(bytes([op, self.tag]) + bytes(args), self.seq, ident=ID))
        for p in self._frames(time.monotonic() + timeout):
            if len(p) >= 3 and p[0] == EVENT:
                self.event(job_record(p[3:]))
            elif len(p) >= 3 + PSOC_REC and p[0] == PSOC_EVENT:
                self.psoc_event(psoc_record(p[3:]))
            elif len(p) >= 3 and p[0] == op and p[1] == self.tag:
                if p[2] not in ok:
                    raise HostLinkError(op, p[2], bytes(p[3:]))
                return p[2], bytes(p[3:])
        raise SystemExit('no answer from the unit: is it in the boot menu, with Host link on?')

    def event(self, rec):
        self.last_event = rec
        if self.on_event:
            self.on_event(rec)

    def psoc_event(self, rec):
        self.last_psoc = rec
        if self.on_psoc:
            self.on_psoc(rec)

    # --- the session

    def hello(self):
        _, d = self.call(HELLO, bytes([VERSION]))
        magic, version, oldest, on = struct.unpack_from('<IBBB', d)
        if magic != 0x4C484257:
            raise SystemExit('not a waffle-boot host link')
        out = {'version': version, 'oldest_client': oldest, 'host_link': bool(on)}
        if version < OLDEST_UNIT or oldest > VERSION:
            raise SystemExit('the unit speaks host link version %d (clients from %d); this tool %d'
                             % (version, oldest, VERSION))
        if not on:
            return out
        out['session'] = SESSIONS[d[7]] if d[7] < len(SESSIONS) else d[7]
        out['max_payload'], out['chunk'] = d[8], d[9]
        out['info'] = info_record(d[10:34])
        at = 34
        out['calls'] = list(d[at + 1:at + 1 + d[at]])
        at += 1 + d[at]
        out['actions'] = list(d[at + 1:at + 1 + d[at]])
        at += 1 + d[at]
        roots, at = [], at + 1
        for _ in range(d[at - 1]):
            name = d[at + 2:].split(b'\0')[0].decode()
            roots.append({'id': d[at], 'name': name, 'flags': [w for b, w in ROOT_FLAGS if d[at + 1] & b]})
            at += 3 + len(name)
        out['roots'] = roots
        return out

    def session(self):
        return SESSIONS[self.call(SESSION)[1][0]]

    def approve(self, wait=70.0, asked=None):
        """Asks the unit, then waits for its answer: 'approved', 'refused' or 'timed out'; at once
        'approved, no hold' when Developer mode's item allows it (asked again, the hold)."""
        state = SESSIONS[self.call(APPROVE)[1][0]]
        if state == 'pending' and asked:
            asked()
        end = time.monotonic() + wait
        while state == 'pending' and time.monotonic() < end:
            time.sleep(0.25)
            state = self.session()
        return state

    def end(self):
        self.call(END)

    # --- an OS image into RAM

    def ram(self, data, progress=None):
        """data into the unit's RAM, from where the unit stands, then checked whole there: its hash,
        then the unit's own check of the image. Nothing is written to a file."""
        digest = hashlib.sha256(data).digest()
        have = struct.unpack('<I', self.call(RAM_OPEN, struct.pack('<I', len(data)) + digest, 30.0)[1])[0]
        while have < len(data):
            st, d = self.call(RAM_DATA, struct.pack('<I', have) + data[have:have + CHUNK], 15.0,
                              ok=(0, 12))
            have = struct.unpack('<I', d[:4])[0]	# offset: resumed from the unit's
            if progress:
                progress(have)
        try:
            self.call(RAM_END, b'', 60.0)
        except HostLinkError as e:
            if e.status == 15 and e.data:
                why = REFUSALS[e.data[0]] if e.data[0] < len(REFUSALS) else e.data[0]
                raise SystemExit('the unit refused the image: %s' % why)
            raise

    def info(self):
        return info_record(self.call(INFO)[1])

    def set_host_link(self, on):
        self.call(SET, bytes([SET_HOST_LINK, 1 if on else 0]))

    # --- files

    def stat(self, path):
        d = self.call(STAT, path.encode())[1]
        return {'type': 'dir' if d[0] == 2 else 'file', 'size': struct.unpack_from('<I', d, 1)[0],
                'mtime': struct.unpack_from('<I', d, 5)[0]}

    def ls(self, path):
        out, index = [], 0
        while index != 0xFFFF:
            d = self.call(LIST, struct.pack('<H', index) + path.encode(), 15.0)[1]
            nxt, at = struct.unpack_from('<H', d)[0], 2
            while at < len(d):
                n = d[at + 5]
                out.append({'type': 'dir' if d[at] == 2 else 'file',
                            'size': struct.unpack_from('<I', d, at + 1)[0],
                            'name': d[at + 6:at + 6 + n].decode('utf-8', 'replace')})
                at += 6 + n
            index = nxt
        return out

    def get(self, path, start=0, progress=None):
        """The file's bytes from start on, chunk by chunk, by offset."""
        out, off = bytearray(), start
        while True:
            d = self.call(GET, struct.pack('<IB', off, CHUNK) + path.encode(), 15.0)[1]
            data = d[4:]
            out += data
            off += len(data)
            if progress:
                progress(off)
            if len(data) < CHUNK:
                return bytes(out)

    def hash(self, path):
        d = self.call(HASH, path.encode(), 120.0)[1]
        return d[4:36], struct.unpack_from('<I', d)[0]

    def put(self, path, data, progress=None):
        """data to path: to its temporary name, from where the unit stands, then checked whole."""
        digest = hashlib.sha256(data).digest()
        have = struct.unpack('<I', self.call(PUT_OPEN, struct.pack('<I', len(data)) + digest
                                             + path.encode(), 30.0)[1])[0]
        while have < len(data):
            st, d = self.call(PUT_DATA, struct.pack('<I', have) + data[have:have + CHUNK], 15.0,
                              ok=(0, 12))
            have = struct.unpack('<I', d[:4])[0]	# offset: resumed from the unit's
            if progress:
                progress(have)
        self.call(PUT_END, b'', 120.0)

    def rm(self, path):
        self.call(REMOVE, path.encode(), 15.0)

    def mkdir(self, path):
        self.call(MKDIR, path.encode(), 15.0)

    def mv(self, src, dst):
        self.call(RENAME, src.encode() + b'\0' + dst.encode(), 15.0)

    # --- actions

    def run(self, action, arg=None, path=None):
        a = bytes([action]) + (bytes([arg]) if arg is not None else b'') + (path.encode() if path else b'')
        return job_record(self.call(RUN, a, 30.0)[1])

    # --- the PSoC (S-10c.131 - S-10c.134): no approval; the unit warns and counts down

    def psoc_state(self):
        return psoc_record(self.call(PSOC_STATE)[1])

    def psoc_open(self, entry=True):
        """Asks for the bridge; the record, counting down."""
        return psoc_record(self.call(PSOC_OPEN, bytes([1 if entry else 0]))[1])

    def psoc_reset(self):
        """Asks for the PSoC reset; the record, counting down."""
        return psoc_record(self.call(PSOC_RESET)[1])

    def psoc_data(self, data=b''):
        """(status, record, the PSoC's bytes): status 10 when the bytes found no room on the
        unit, which then wrote none of them."""
        st, d = self.call(PSOC_DATA, data, ok=(0, 10))
        return st, psoc_record(d), d[PSOC_REC:]

    def psoc_close(self):
        """(record, the PSoC's last bytes): the link framed again, the greeting in the record."""
        d = self.call(PSOC_CLOSE, b'', 10.0)[1]
        return psoc_record(d), d[PSOC_REC:]

    def psoc_wait(self, states, timeout=30.0, every=0.25):
        """Asks for the state until it is one of states, or the time is up."""
        end = time.monotonic() + timeout
        rec = self.psoc_state()
        while rec['state'] not in states and time.monotonic() < end:
            time.sleep(every)
            rec = self.psoc_state()
        return rec

    def wait(self, job, timeout=900.0, gone_ok=False):
        """The job's final record: from its events, or by asking."""
        end = time.monotonic() + timeout
        rec = job
        while rec['state'] in ('queued', 'running') and time.monotonic() < end:
            for p in self._frames(time.monotonic() + 1.0):
                if p[0] == EVENT:
                    self.event(job_record(p[3:]))
            if self.last_event and self.last_event['id'] == job['id']:
                rec = self.last_event
                if rec['text'] in ('restarting', 'starting') and gone_ok:
                    return rec
        return rec


class PsocError(Exception):
    pass


class PsocBridge:
    """The unit's PSoC link as a byte pipe, for a computer's own PSoC bootloader tool. The unit checks
    nothing it carries and sends no exit packet of its own.

        b = PsocBridge(Client(SerialPort(port)))
        b.open(entry=True)      # the unit's warning and countdown first; PsocError if it is stopped
        reply = b.exchange(packet, timeout=1.0, n=None)
        greeted = b.close()

    overruns is the count of the PSoC's bytes the unit lost since the bridge opened.
    """

    def __init__(self, client, every=0.25, poll=0.005):
        self.c, self.every, self.poll_s = client, every, poll
        self.rx, self.overruns = bytearray(), 0

    def _took(self, rec, data):
        self.rx += data
        self.overruns = rec['overruns']
        return rec

    def open(self, entry=True, wait=PSOC_COUNT_S + 15.0):
        rec = self.c.psoc_open(entry)
        if rec['state'] != 'open':
            rec = self.c.psoc_wait(('open', 'canceled', 'ended', 'none'), wait, self.every)
        if rec['state'] != 'open':
            raise PsocError('the bridge did not open: %s' % rec['state'])
        self.rx = bytearray()
        return rec

    def send(self, data, timeout=2.0):
        """data to the PSoC, as it is, in as many requests as it takes."""
        end = time.monotonic() + timeout
        data = bytes(data)
        while data:
            st, rec, got = self.c.psoc_data(data[:PSOC_TX_MAX])
            self._took(rec, got)
            if st == 0:
                data = data[PSOC_TX_MAX:]
            elif time.monotonic() > end:
                raise PsocError('the unit had no room for the bytes')
            else:
                time.sleep(self.poll_s)

    def poll(self):
        """What the PSoC has sent since the last call, kept in rx."""
        st, rec, got = self.c.psoc_data()
        self._took(rec, got)
        return rec

    def recv(self, timeout=1.0, n=None):
        """The bytes received: n of them, or what came within timeout with n None."""
        end = time.monotonic() + timeout
        while True:
            rec = self.poll()
            if n is not None and len(self.rx) >= n:
                out, self.rx = bytes(self.rx[:n]), self.rx[n:]
                return out
            if time.monotonic() >= end:
                out, self.rx = bytes(self.rx), bytearray()
                return out
            if rec['held'] == 0:
                time.sleep(self.poll_s)

    def exchange(self, data, timeout=1.0, n=None):
        self.send(data)
        return self.recv(timeout, n)

    def close(self):
        """The bridge ended; True when the PSoC greeted after it."""
        rec, got = self.c.psoc_close()
        self._took(rec, got)
        return rec['greeted']


def find_port():
    ports = sorted(glob.glob('/dev/cu.usbmodem*'))
    if not ports:
        raise SystemExit('no /dev/cu.usbmodem* port: is the unit in the boot menu with USB connected?')
    return ports[0]


def show_job(c, rec, gone_ok=False):
    c.on_event = lambda r: print('  %s %s%s' % (r['state'], '' if r['percent'] is None else
                                                '%d%% ' % r['percent'], r['text']))
    print('job %d: %s' % (rec['id'], rec['state']))
    rec = c.wait(rec, gone_ok=gone_ok)
    return 0 if rec['state'] == 'done' else 1


def command(c, a):
    cmd, a = a[0], a[1:]
    if cmd == 'hello':
        for k, v in c.hello().items():
            print('%-12s %s' % (k, v))
    elif cmd == 'info':
        for k, v in c.info().items():
            print('%-20s %s' % (k, v))
    elif cmd == 'session':
        print(c.session())
    elif cmd == 'approve':
        state = c.approve(float(a[0]) if a else 70.0,
                          asked=lambda: print('hold 1 on the unit to allow, 8 to cancel'))
        print(state)
        return 0 if state in APPROVED else 1
    elif cmd == 'end':
        c.end()
    elif cmd == 'ls':
        for e in c.ls(a[0]):
            print('%-4s %10s  %s' % (e['type'], '' if e['type'] == 'dir' else e['size'], e['name']))
    elif cmd == 'stat':
        print(c.stat(a[0]))
    elif cmd == 'get':
        start = os.path.getsize(a[1]) if os.path.exists(a[1]) else 0
        data = c.get(a[0], start)
        with open(a[1], 'ab') as f:
            f.write(data)
        print('%d bytes to %s' % (start + len(data), a[1]))
    elif cmd == 'hash':
        digest, size = c.hash(a[0])
        print('%s  %d bytes' % (digest.hex(), size))
    elif cmd == 'put':
        with open(a[0], 'rb') as f:
            data = f.read()
        c.put(a[1], data)
        print('%d bytes to %s, sha256 %s: checked' % (len(data), a[1], hashlib.sha256(data).hexdigest()))
    elif cmd == 'rm':
        c.rm(a[0])
    elif cmd == 'mkdir':
        c.mkdir(a[0])
    elif cmd == 'mv':
        c.mv(a[0], a[1])
    elif cmd == 'install':
        return show_job(c, c.run(A_INSTALL, int(a[1], 0) if len(a) > 1 else 0xFF, a[0]))
    elif cmd == 'os-write':
        return show_job(c, c.run(A_OS_WRITE, None, a[0]))
    elif cmd in ('os-once', 'restart', 'continue-os'):
        action = {'os-once': A_OS_ONCE, 'restart': A_RESTART, 'continue-os': A_CONTINUE_OS}[cmd]
        return show_job(c, c.run(action), gone_ok=True)
    elif cmd == 'ram-boot':
        if RAM_OPEN not in c.hello().get('calls', []):
            raise SystemExit('this unit takes no OS image into RAM: update waffle-boot')
        with open(a[0], 'rb') as f:
            data = f.read()
        c.ram(data)
        print('%d bytes in RAM, sha256 %s: checked' % (len(data), hashlib.sha256(data).hexdigest()))
        return show_job(c, c.run(A_RAM_START), gone_ok=True)
    elif cmd == 'restart-mode':
        return show_job(c, c.run(A_RESTART_MODE, int(a[0], 0)), gone_ok=True)
    elif cmd == 'host-link-off':
        c.set_host_link(False)
    elif cmd.startswith('psoc-'):
        return psoc_command(c, cmd, a)
    else:
        print(__doc__)
        return 2
    return 0


def show_psoc(rec):
    print('%s%s%s, overruns %d, held %d%s' % (
        rec['state'], '' if rec['what'] == 'none' else ' (%s)' % rec['what'],
        ', %d s left' % rec['seconds'] if rec['state'] in ('counting down', 'resetting') and
        rec['seconds'] else '',
        rec['overruns'], rec['held'],
        ', the PSoC did not act on the reset' if rec['not_reset'] else
        ', the PSoC greeted' if rec['state'] == 'ended' and rec['greeted'] else
        ', no greeting' if rec['state'] == 'ended' and rec['what'] != 'reset' else ''))


def psoc_countdown(c, wait, states):
    print('the unit warns and counts down %d s: switching it off, or 8 on it, stops it' % PSOC_COUNT_S)
    c.on_psoc = lambda r: print('  %d s' % r['seconds']) if r['state'] == 'counting down' else None
    return c.psoc_wait(states, wait)


def psoc_command(c, cmd, a):
    if cmd == 'psoc-state':
        show_psoc(c.psoc_state())
        return 0
    if cmd == 'psoc-open':
        entry = '--no-entry' not in a
        a = [x for x in a if x != '--no-entry']
        c.psoc_open(entry)
        rec = psoc_countdown(c, float(a[0]) if a else PSOC_COUNT_S + 15.0,
                             ('open', 'canceled', 'ended', 'none'))
        show_psoc(rec)
        if rec['state'] == 'open':
            print('the unit\'s keys do not work until psoc-close')
        return 0 if rec['state'] == 'open' else 1
    if cmd == 'psoc-raw-tx':
        b = PsocBridge(c)
        data = bytes.fromhex(a[0].replace(' ', '')) if a else b''
        got = b.exchange(data, float(a[1]) if len(a) > 1 else 0.5)
        print(got.hex(' ') if got else '(nothing back)')
        if b.overruns:
            print('bytes lost on the unit since the bridge opened: %d' % b.overruns)
        return 0
    if cmd == 'psoc-close':
        rec, got = c.psoc_close()
        if got:
            print('last bytes: %s' % got.hex(' '))
        show_psoc(rec)
        return 0
    if cmd == 'psoc-reset':
        print('the PSoC reset switches the whole unit off and on; the session ends')
        c.psoc_reset()
        try:
            rec = psoc_countdown(c, PSOC_COUNT_S + 15.0, ('resetting', 'canceled', 'none'))
            if rec['state'] == 'resetting' and rec['reset_alone']:
                print('the PSoC had not greeted: the reset request went alone, and the unit runs on; '
                      'waiting up to %d s for the PSoC to act on it' % PSOC_RESET_WAIT_S)
                rec = c.psoc_wait(('ended', 'none'), PSOC_RESET_WAIT_S + 10.0)
        except SystemExit:
            print('the unit has gone: resetting')
            return 0
        show_psoc(rec)
        if rec['not_reset']:
            print('the unit is running and usable: psoc-open reaches a PSoC in its bootloader')
        return 0 if rec['state'] == 'resetting' else 1
    print(__doc__)
    return 2


def main(argv):
    args = argv[1:]
    port = None
    if args[:1] == ['--port']:
        port, args = args[1], args[2:]
    if not args:
        print(__doc__)
        return 2
    try:
        return command(Client(SerialPort(port or find_port())), args)
    except HostLinkError as e:
        print('hostlink: %s' % e)
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
