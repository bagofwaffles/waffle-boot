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
                          0xFF: every part the unit would offer). Protected writes applies
    os-write PATH         write the OS image PATH from the drive to the inactive boot partition
    os-once               restart once into /yaffs2/os.ldr (writes nothing)
    restart, continue-os, restart-mode MODE
    host-link-off         turn the host link off for this start

Python 3, standard library only. Changes and every read but list and stat on the drive need the
session's approval: `approve` first.
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
VERSION = 1
HELLO, SESSION, APPROVE, END, INFO, SET, JOB = range(7)
STAT, LIST, GET, HASH, PUT_OPEN, PUT_DATA, PUT_END, REMOVE, MKDIR, RENAME = range(0x10, 0x1A)
RUN, EVENT = 0x20, 0x7F
A_INSTALL, A_OS_WRITE, A_OS_ONCE, A_RESTART, A_CONTINUE_OS, A_RESTART_MODE = range(1, 7)
SET_HOST_LINK = 7
CHUNK = 224
STATUS = ['ok', 'host link off', 'not approved: hold 1 on the unit', 'never from a host', 'malformed',
          'not in this build', 'path refused', 'not found', 'exists', 'I/O error', 'busy',
          'checksum mismatch', 'offset', 'volume not answering', 'read only', 'refused by the unit',
          'volume not readable by this build']
SESSIONS = ['none', 'pending', 'approved', 'refused', 'timed out']
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


class Client:
    """Requests and replies on a transport with write(bytes) and read(timeout) -> bytes."""

    def __init__(self, port):
        self.port, self.rx, self.seq, self.tag = port, bytearray(), 0, 0
        self.on_event = None
        self.last_event = None

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
            elif len(p) >= 3 and p[0] == op and p[1] == self.tag:
                if p[2] not in ok:
                    raise HostLinkError(op, p[2], bytes(p[3:]))
                return p[2], bytes(p[3:])
        raise SystemExit('no answer from the unit: is it in the boot menu, with Host link on?')

    def event(self, rec):
        self.last_event = rec
        if self.on_event:
            self.on_event(rec)

    # --- the session

    def hello(self):
        _, d = self.call(HELLO, bytes([VERSION]))
        magic, version, oldest, on = struct.unpack_from('<IBBB', d)
        if magic != 0x4C484257:
            raise SystemExit('not a waffle-boot host link')
        out = {'version': version, 'oldest_client': oldest, 'host_link': bool(on)}
        if version < VERSION or oldest > VERSION:
            raise SystemExit('the unit speaks host link version %d (clients from %d); this tool %d'
                             % (version, oldest, VERSION))
        if not on:
            return out
        out['session'] = SESSIONS[d[7]]
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

    def approve(self, wait=70.0):
        """Asks the unit, then waits for its answer: 'approved', 'refused' or 'timed out'."""
        state = SESSIONS[self.call(APPROVE)[1][0]]
        end = time.monotonic() + wait
        while state == 'pending' and time.monotonic() < end:
            time.sleep(0.25)
            state = self.session()
        return state

    def end(self):
        self.call(END)

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
                if rec['text'] == 'restarting' and gone_ok:
                    return rec
        return rec


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
        print('hold 1 on the unit for 5 s to allow, 8 to cancel')
        state = c.approve(float(a[0]) if a else 70.0)
        print(state)
        return 0 if state == 'approved' else 1
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
    elif cmd == 'restart-mode':
        return show_job(c, c.run(A_RESTART_MODE, int(a[0], 0)), gone_ok=True)
    elif cmd == 'host-link-off':
        c.set_host_link(False)
    else:
        print(__doc__)
        return 2
    return 0


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
