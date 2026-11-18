#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-07 model of the master's PSoC link: the message catalog, pure rules, a bootloader host, and ``Sim``, a
virtual-clock master against a scripted ``Psoc`` that produces and checks the timed transcripts (times in ms).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'conformance'))
from serial import cypress  # noqa: E402  (the conformance package, not pyserial)

MASTER, PSOC = 1, 4
PLAIN_ROUTE = MASTER << 4 | PSOC            # 0x14
PSOC_ROUTE = PSOC << 4 | MASTER             # 0x41
LOG_IDS = {0x03, 0x04}
REQUEST_MS = 100

# ---- the catalog (S-07 tables 1 and 2) ----

TABLE1 = [
    # id, kind, allowed payloads (hex; '.' any nibble)
    (0x01, 'request', ['']),
    (0x05, 'plain', ['0104']),
    (0x06, 'plain', ['40']),
    (0x09, 'request', ['']),
    (0x30, 'plain or request', ['']),
    (0x32, 'plain', ['10', '14', '16']),
    (0x33, 'request', ['']),
    (0x34, 'plain', ['00..']),
    (0x36, 'plain or request', ['']),
    (0x40, 'plain', ['00......', '01', '02..', '03......']),
    (0x41, 'request', ['']),
    (0x43, 'plain', ['0101']),
    (0x48, 'plain', ['01']),
    (0x51, 'plain', ['000000', '3cff02', '3cff32', '000002']),
    (0x70, 'plain', ['01', '00']),
    (0x71, 'plain', ['03....']),
    (0x73, 'request', ['03..']),
    (0x97, 'plain', ['deadbeef']),
    (0xEE, 'plain', ['']),
]
TABLE2 = [0x00, 0x05, 0x06, 0x31, 0x32, 0x35, 0x37, 0x42, 0x45, 0x4D]
PSOC_ACCEPTS = [0x01, 0x02, 0x05, 0x07, 0x09, 0x0B, 0x30, 0x31, 0x32, 0x33, 0x34, 0x36, 0x40, 0x41, 0x43,
                0x44, 0x46, 0x48, 0x4A, 0x4C, 0x4E, 0x4F, 0x51, 0x52, 0x53, 0x54, 0x55, 0x57, 0x60, 0x64,
                0x65, 0x66, 0x67, 0x6B, 0x70, 0x71, 0x73, 0x80, 0x89, 0x8A, 0x90, 0x91, 0x95, 0x96, 0x97,
                0xEE]
COMMON_ANSWERS = {0x02}      # what the master sends in answer to a PSoC message (S-07.076): the pong


def payload_allowed(pattern, payload):
    """A table-1 payload pattern: hex digits, '.' any nibble."""
    h = payload.hex()
    return len(h) == len(pattern) and all(p == '.' or p == c for p, c in zip(pattern, h))


def catalog_allows(mid, kind, payload):
    """True if a master frame (id, 'plain'/'request', payload) is in table 1 (or a pong, S-07.076)."""
    if mid in COMMON_ANSWERS and kind == 'plain':
        return True
    for i, k, pats, *_ in TABLE1:
        if i == mid:
            return kind in k.split(' or ') and any(payload_allowed(p, payload) for p in pats)
    return False


# ---- keys (S-07.030-.033) ----

def bitmap_events(known, bitmap):
    """(new known state, [(key, down)]) for a 9-byte 0x42 bitmap (S-07.031); a shorter one is ignored and
    bytes beyond the ninth are not read (S-07.079)."""
    if len(bitmap) < 9:
        return list(known), []
    new = list(known)
    ev = []
    for k in range(72):
        down = bool(bitmap[k >> 3] >> (k & 7) & 1)
        if down != known[k]:
            new[k] = down
            ev.append((k, down))
    return new, ev


def known_from_status(payload):
    """The known key state from a 0x41 reply payload (S-07.031, S-07.032): keys in bytes 0 to 8, a byte the
    reply does not carry 00 (S-07.079); all up for no reply."""
    p = (bytes(payload) if payload is not None else b'') + bytes(9)
    return [bool(p[k >> 3] >> (k & 7) & 1) for k in range(72)]


def key_status(payload):
    """{'hold', 'debug', 'precomp'} from a 0x41 reply payload, None for no reply (S-07.032)."""
    if payload is None:
        return {'hold': True, 'debug': False, 'precomp': False}
    p = bytes(payload) + bytes(4)
    return {'hold': bool(p[3] & 1 or p[1] & 0x20), 'debug': bool(p[1] & 0x0C), 'precomp': bool(p[2] & 0x80)}


# ---- LEDs (S-07.040-.045) ----

class Leds:
    """The change filter of S-07.041; ``send(payload)`` is called for each 0x40 message to send."""

    def __init__(self, send):
        self.send = send
        self.key = [b'\x00\x00'] * 72
        self.rgb = b'\x00\x00\x00'
        self.bar = None

    def set_key(self, k, ab):
        if self.key[k] != ab:
            self.key[k] = ab
            self.send(bytes([0, k]) + ab)

    def set_bar(self, n):
        if self.bar != n:
            self.bar = n
            self.send(bytes([2, n]))

    def set_rgb(self, rgb, forced=False):
        rgb = bytes(rgb)
        if forced:
            self.send(b'\x03' + rgb)
        elif self.rgb != rgb:
            self.rgb = rgb
            self.send(b'\x03' + rgb)


def charge_led(state, charge):
    """(rgb, forced) of S-07.042."""
    if state == 0x10:
        return (b'\xc8\x00\x00' if charge < 15 else b'\x00\xc8\x00'), False
    if state == 0x16:
        return (b'\x00\xc8\x00' if charge > 90 else b'\xc8\x00\x00'), True
    return b'\x00\x00\x00', False


# ---- power (S-07.050-.057) ----

def power_kind(s):
    return {0x10: 'on', 0x14: 'off', 0x16: 'charge'}.get(s, 'other')


def power_off_trigger(prev, s):
    """S-07.051: True when state s, after prev (None: none known), starts a power-off."""
    return s == 0x14 or (s == 0x16 and prev == 0x10)


def battery_report(payload):
    """S-07.053: the recorded battery values from a 0x37 payload; missing bytes read as 00 (S-07.079)."""
    p = bytes(payload) + bytes(7)
    if p[0] == 0:
        return {'present': False, 'charging': False, 'charge': 0, 'temp': 0, 'ma': 0, 'mv': 0}
    return {'present': True, 'charging': p[0] == 2, 'charge': p[1], 'temp': p[2],
            'ma': int.from_bytes(p[3:5], 'big'), 'mv': int.from_bytes(p[5:7], 'big')}


def battery_usable(b):
    return bool(b['present'] and b['charge'] > 5 and b['mv'] > 3500)


def vbus_update(applied, target, reply):
    """S-07.055: (new applied, payload of the 0x34 to send or None) for a 0x33 reply payload {st n};
    missing bytes read as 00 (S-07.079)."""
    st, n = (bytes(reply) + bytes(2))[:2]
    if st != 0:
        return applied, None
    if n < target // 10:                         # integer division: a target of 2 mA asks for nothing
        t = target // 10
        return t * 10, bytes([0, t])
    return n * 10, None


# ---- PMIC (S-07.060-.061) ----

def pmic_sequence(read_value):
    """The frames (id, kind, payload) of S-07.061 for a read reply value (None: no reply)."""
    out = [(0x70, 'plain', b'\x01'), (0x73, 'request', b'\x03\x10')]
    if read_value is None:
        return out
    return out + [(0x71, 'plain', bytes([3, 0x10, read_value | 0x60])), (0x70, 'plain', b'\x00'),
                  (0x01, 'request', b'')]


# ---- the bootloader-host protocol (S-07.080-.086) ----

EXIT_PACKET = bytes.fromhex('013b0000c4ff17')
MAX_PACKET = 62


def enter_packet(unlock, kind='sum'):
    """S-07.081: the enter packet, the PSoC unlock as its data."""
    return cypress.packet(0x38, bytes(unlock), kind)


def probe_verdict(data):
    """S-07.081: True when the bytes received after the enter packet hold the success reply."""
    data = bytes(data)
    i = data.find(b'\x01')
    if i < 0:
        return False
    r = data[i:i + 15]
    return len(r) == 15 and r[:4] == b'\x01\x00\x08\x00' and r[14] == 0x17


REPLY_DATA = {0x38: 8, 0x32: 4, 0x37: 0, 0x39: 0, 0x3A: 1, 0x31: 1}


class RunFailed(Exception):
    def __init__(self, at, why):
        super().__init__(f'{at:#04x}: {why}')
        self.at, self.why = at, why


class Host:
    """The master's side of one programming run (S-07.083) over ``xfer(packet, reply_len) -> bytes|None``
    (None: no complete reply within 5 s) and ``write(packet)`` (no reply read)."""

    def __init__(self, xfer, write, unlock, max_packet=MAX_PACKET):
        self.xfer, self.write, self.unlock, self.max = xfer, write, bytes(unlock), max_packet

    def _cmd(self, code, data, kind):
        p = cypress.packet(code, data, kind)
        assert len(p) <= self.max
        r = self.xfer(p, 7 + REPLY_DATA[code])
        if r is None:
            raise RunFailed(code, 'timeout')
        if r[1] != 0:
            raise RunFailed(code, f'status {r[1]:#04x}')
        if r[0] != 0x01 or int.from_bytes(r[2:4], 'little') != REPLY_DATA[code] or r[-1] != 0x17:
            raise RunFailed(code, 'malformed')               # S-07.083: the checksum is not checked
        return r[4:4 + REPLY_DATA[code]]

    def run(self, cyacd_text):
        f = parse_cyacd(cyacd_text)
        kind = 'crc' if f['checksum_type'] == 1 else 'sum'
        d = self._cmd(0x38, self.unlock, kind)
        if int.from_bytes(d[:4], 'little') != f['silicon_id'] or d[4] != f['silicon_rev']:
            raise RunFailed(0x38, 'device')
        if d[7] != 0x01:
            raise RunFailed(0x38, 'version')
        ranges = {}
        for arr, row, data, cc in f['rows']:
            if arr < 0x40:                           # S-07.083: arrays from 0x40 have no flash-size check
                if arr not in ranges:
                    fs = self._cmd(0x32, bytes([arr]), kind)
                    ranges[arr] = (int.from_bytes(fs[:2], 'little'), int.from_bytes(fs[2:4], 'little'))
                lo, hi = ranges[arr]
                if not lo <= row <= hi:
                    raise RunFailed(0x32, 'row')
            off = 0
            while len(data) - off > self.max - 10:
                n = min(self.max - 7, len(data) - off)      # S-07.083: only the row's bytes (53 or 54 left)
                self._cmd(0x37, data[off:off + n], kind)
                off += n
            self._cmd(0x39, bytes([arr]) + row.to_bytes(2, 'little') + data[off:], kind)
            chk = self._cmd(0x3A, bytes([arr]) + row.to_bytes(2, 'little'), kind)[0]
            if chk != cypress.verify_row_expected(arr, row, data, cc):
                raise RunFailed(0x3A, 'row check')
        valid = self._cmd(0x31, b'', kind)[0]
        self.write(cypress.packet(0x3B, b'', kind))
        if not valid:
            raise RunFailed(0x31, 'application invalid')
        return True


HEX = set('0123456789abcdefABCDEF')
MAX_LINE = 1535


def image_problem(text, max_arrays=None):
    """S-07.083: why the master finds the .cyacd file bad (and sends nothing for it), or None. ``max_arrays``:
    the limit on distinct array ids a master may apply (8 at most strict, None: no limit)."""
    lines = text.replace('\r', '').split('\n')
    if lines and lines[-1] == '':
        lines.pop()                                  # the end of the last line
    lines = [l for l in lines if not l.startswith('#')]
    if not lines:
        return 'no header'
    h = lines[0]
    if len(h) != 12 or not set(h) <= HEX:
        return 'header'
    if int(h[10:12], 16) not in (0, 1):
        return 'checksum type'
    if len(lines) < 2:
        return 'no row line'
    arrays = set()
    for l in lines[1:]:
        if len(l) > MAX_LINE or not l.startswith(':') or (len(l) - 1) % 2 or not set(l[1:]) <= HEX:
            return 'line'
        b = bytes.fromhex(l[1:])
        if len(b) < 6 or int.from_bytes(b[3:5], 'big') + 6 != len(b):
            return 'line length'
        arrays.add(b[0])
    if max_arrays is not None and len(arrays) > max_arrays:
        return 'arrays'
    return None


def parse_cyacd(text):
    """Like ``cypress.parse_cyacd`` but keeps a line whose line check is wrong (S-07.083)."""
    lines = [l.strip() for l in text.splitlines() if l.strip() and not l.startswith('#')]
    h = bytes.fromhex(lines[0])
    rows = []
    for l in lines[1:]:
        b = bytes.fromhex(l[1:])
        n = int.from_bytes(b[3:5], 'big')
        if len(b) != n + 6:
            raise ValueError('line length')
        rows.append((b[0], int.from_bytes(b[1:3], 'big'), b[5:5 + n], b[-1]))
    return {'silicon_id': int.from_bytes(h[:4], 'big'), 'silicon_rev': h[4],
            'checksum_type': h[5] if len(h) > 5 else 0, 'rows': rows}


def run_direct(cyacd_text, bootloader, unlock):
    """One run of ``Host`` straight against a ``cypress.Bootloader``: (ok, failure or None, packets sent)."""
    sent = []

    def xfer(p, n):
        sent.append(p)
        r = bootloader.feed(p)
        return r[:n] if len(r) >= n else None

    def write(p):
        sent.append(p)
        bootloader.feed(p)

    try:
        Host(xfer, write, unlock).run(cyacd_text)
        return True, None, sent
    except RunFailed as e:
        return False, e, sent


# ---- the scripted PSoC ----

DEFAULT_RULES = [
    {'on': '0x05', 'send': [{'id': '0x06', 'payload': '40'}]},
    {'on': '0x30', 'kind': 'request', 'send': [{'id': '0x31', 'payload': '10'}]},
    {'on': '0x36', 'kind': 'request', 'send': [{'id': '0x37', 'payload': '01641900641068'}]},
    {'on': '0x41', 'send': [{'id': '0x42', 'payload': '00' * 10}]},      # S-07.032: the reply's id and length
    {'on': '0x73', 'send': [{'id': '0x73', 'payload': '031000'}]},
    {'on': '0x01', 'send': [{'id': '0x02', 'payload': ''}]},
    {'on': '0x09', 'send': [{'id': '0x0a', 'payload': '66773a312e302e3700'}]},
    {'on': '0x33', 'send': [{'id': '0x33', 'payload': '000a'}]},
]


def h2i(s):
    return int(s, 16) if isinstance(s, str) else s


class Psoc:
    """The PSoC side of a transcript, from its JSON form::

        {"rules": [{"on": "0x41", "kind": "request"|"plain", "nth": [1], "after_ms": t, "before_ms": t,
                    "delay_ms": 1, "send": [{"id": "0x41", "payload": "...", "as": "reply"|"message"}]}],
         "defaults": true, "inject": [{"t_ms": t, "id": "0x42", "payload": "..."}],
         "bootloader": {"enter_ok_from": 1, "fail_runs": [1], "fail_cmd": "0x3A", "greet_after_ms": 300,
                        "silent": false}}

    The first rule that matches a master frame answers it (``send`` empty: no answer). A reply goes to the
    request's slot unless ``as`` is "message". In raw mode (after an ``0xEE``) the PSoC is its own bootloader
    (``serial.cypress.Bootloader``): the enter packets before the ``enter_ok_from``-th get no answer. Its
    application validates until a row is programmed, and again after a run that succeeds. An exit after a
    run that failed once a row was programmed leaves it in its bootloader; any other exit starts the
    application, which greets ``greet_after_ms`` later (in raw mode the greeting is not decoded) and then
    answers no raw byte."""

    def __init__(self, spec, unlock):
        spec = spec or {}
        self.rules = list(spec.get('rules', [])) + (DEFAULT_RULES if spec.get('defaults', True) else [])
        self.injects = [dict(i) for i in spec.get('inject', [])]
        self.bl_spec = spec.get('bootloader')
        self.seen = {}
        self.mode = 'app'
        self.enters = 0
        self.runs = 0
        self.unlock = unlock
        self.bl = None
        self.failed_run = False                # a run failed since the last enter
        self.programmed = False                # a row was programmed since the bootloader started: the
        self.raw_log = []                      # application does not validate until a run succeeds

    def initial(self):
        return [(h2i(i['t_ms']), self._frame(PSOC_ROUTE, i)) for i in self.injects]

    @staticmethod
    def _frame(route, item):
        return ('frame', h2i(item.get('route', route)), h2i(item['id']), bytes.fromhex(item.get('payload', '')))

    def on_frame(self, t, route, mid, payload):
        """[(t_arrival, item)] the PSoC sends in answer to a master frame."""
        if self.mode != 'app':
            return []
        kind = 'request' if route >> 4 >= 8 else 'plain'
        n = self.seen[mid] = self.seen.get(mid, 0) + 1
        if mid == 0xEE and self.bl_spec is not None and not self.bl_spec.get('ignore_ee'):
            self.mode = 'bootloader'
            return []
        for r in self.rules:
            if h2i(r['on']) != mid or r.get('kind', kind) != kind:
                continue
            if 'payload' in r and bytes.fromhex(r['payload']) != payload:
                continue
            if 'nth' in r and n not in r['nth']:
                continue
            if t < r.get('after_ms', -1) or t >= r.get('before_ms', 1e12):
                continue
            out = []
            for s in r.get('send', []):
                as_reply = kind == 'request' and s.get('as', 'reply') == 'reply'
                rt = (PSOC << 4 | route >> 4) if as_reply else PSOC_ROUTE
                out.append((t + r.get('delay_ms', 1) + s.get('extra_ms', 0), self._frame(rt, s)))
            return out
        return []

    def on_raw(self, t, data):
        """Raw bytes from the master: in the bootloader, answered by the bootloader model."""
        self.raw_log.append((t, bytes(data)))
        if self.mode != 'bootloader':
            return []
        spec = self.bl_spec
        if self.bl is None:
            fail_runs = spec.get('fail_runs', [])
            fail_cmd = h2i(spec.get('fail_cmd', '0x3A'))

            def hook(cmd, d):
                if cmd == 0x38:
                    self.enters += 1
                    if spec.get('silent') or self.enters < spec.get('enter_ok_from', 1):
                        return cypress.NO_REPLY
                    if self.enters > spec.get('enter_ok_from', 1):
                        self.runs += 1
                        self.failed_run = False
                if cmd == 0x39:
                    self.programmed = True
                if cmd == fail_cmd and self.runs in fail_runs:
                    self.failed_run = True
                    if cmd == 0x3A:                  # a wrong row check, well formed
                        good = cypress.row_checksum(self.bl.rows.get((d[0], int.from_bytes(d[1:3], 'little')), b''))
                        return cypress.packet(0, bytes([(good + 1) & 0xFF]))
                    return cypress.ERR_CHECKSUM
                return None
            arrays = {int(k): tuple(v) for k, v in spec.get('arrays', {'0': [0, 0xFF]}).items()}
            self.bl = cypress.Bootloader(silicon_id=h2i(spec.get('silicon_id', '0x2E123069')), arrays=arrays,
                                         row_size=None, hook=hook)
        before = self.bl.exited
        out = self.bl.feed(data)
        res = []
        if out:
            res.append((t + 1, ('raw', out)))
        if self.bl.exited and not before and not (self.failed_run and self.programmed):
            self.mode = 'app'                  # the application validates: the exit starts it
            self.bl = None
            self.programmed = False
            if spec.get('greet_after_ms') is not None:
                res.append((t + spec['greet_after_ms'], ('frame', PSOC_ROUTE, 0x05, b'\x01')))
        return res


# ---- the master ----

class PowerOff(Exception):
    pass


RESTART_HOLD_MS = 750              # the restarting view's hold before the shutdown (S-05.033)


class Halt(Exception):
    pass


class Sim:
    """The master's observable behavior (S-07), as one deterministic run on a virtual clock.

    ``setup``: ``boot_allowed`` (S-05.001; default true), ``os_loads`` (default true), ``timestamp`` (no
    effect on the link), ``end_ms`` (when held mode stops being observed), ``actions``
    ([{"t_ms", "do": "key_test_start"|"key_test_stop"|"volcal"|"soft_reboot"|"continue_os"|"backplane"|"usb_config"|
    "usb_reset", "ma": n}]), ``cyacd`` (the backplane file text). ``knobs`` mutate the model for negative controls."""

    KNOBS = {'greet_ms': 10, 'greet_tries': 10, 'request_ms': 100, 'greet_fail_wait_ms': 1500,
             'enter_wait_ms': 250, 'exit_from_ms': 1000, 'give_up_ms': 2000, 'send_exit_on_greet_fail': True,
             'subscribe_before_battery': False, 'skip_pmic_off': False, 'charger_ms': 3000,
             'resend_ee': False, 'first_round_late': False, 'exit_between_runs': False,
             'waited_reply_table2': False, 'stale_act_nothing': False, 'stale_readback': False, 'wait_guard_ms': 60000,
             'common_held_only': False, 'poll_before_os': False, 'prompt_acts_on_waiting': False,
             'no_charge_wait': False, 'charge_target': 500, 'greet_wait_full': False,
             'subscribe_after_loop': False, 'nested_regreet': False,
             'no_return_wait_after_enter_fail': False}
    INITIAL_STATE = None                       # S-07.050: no power state before the first report

    def __init__(self, setup=None, psoc=None, unlock=b'\x0a\x1b\x2c\x3d\x4e\x5f', knobs=None):
        self.setup = dict(setup or {})
        self.k = dict(self.KNOBS, **(knobs or {}))
        self.unlock = None if unlock is None else bytes(unlock)  # None: no PSoC unlock built in
        self.psoc = Psoc(psoc, self.unlock or bytes(6))
        self.t = 0.0
        self.events = []                       # [t, 'M'|'P', 'frame', route, id, payload] / [t, dir, 'raw', bytes]
        self.inbox = []                        # [(t, item)] in arrival order, not yet received
        self.queue = []                        # frames received in framed mode, not yet handled
        self.prompt_on = False
        self.in_held = False                   # negative control common_held_only
        self.raw_rx = bytearray()
        self.raw = False
        self.seq = 0
        self.slot = 0
        self.pending = None                    # (slot, reply holder) of the request waiting
        self.greeted = False
        self.greeting = False                  # a greeting sequence is running
        self.started = False
        self.state = self.INITIAL_STATE
        self.batt = battery_report(b'\x00')
        self.applied = 0
        self.target = 0
        self.last_34 = None
        self.usb_configured = False
        self.last_usb = 0.0
        self.keys = [False] * 72
        self.leds = Leds(lambda p: self.send(0x40, p))
        self.charge_led_enabled = True
        self.key_test = None
        self.fs_window = False                 # S-07.051: the first-stage window is open
        self.kept = None                       # S-07.051: the state of a kept power-off
        self.actions = sorted(self.setup.get('actions', []), key=lambda a: a['t_ms'])
        for t, item in self.psoc.initial():
            self._arrive(t, item)

    # -- the link --
    def _arrive(self, t, item):
        self.inbox.append((t, item))
        self.inbox.sort(key=lambda x: x[0])

    def _deliver_until(self, t):
        """Receive every PSoC item that arrives up to time t (in order)."""
        while self.inbox and self.inbox[0][0] <= t:
            ta, item = self.inbox.pop(0)
            if ta > self.t:
                self.t = ta
            if item[0] == 'raw':
                self.events.append([ta, 'P', 'raw', item[1]])
                if self.raw:
                    self.raw_rx += item[1]
                continue
            _, route, mid, payload = item
            self.events.append([ta, 'P', 'frame', route, mid, payload])
            if self.raw:
                continue                           # S-06.061: nothing is decoded in raw mode
            dst = route & 0x0F
            if self.pending is not None and dst == 8 + self.pending[0]:
                self.pending[1].append((mid, payload))
                self.pending = None
                continue
            self.queue.append((route, mid, payload))
        if t > self.t:
            self.t = t

    def _tx(self, route, mid, payload):
        self.seq = (self.seq + 1) & 0xFF
        if self.raw:
            return                                  # S-06.062: not transmitted (may use a number)
        self.events.append([self.t, 'M', 'frame', route, mid, bytes(payload)])
        for ta, item in self.psoc.on_frame(self.t, route, mid, bytes(payload)):
            self._arrive(ta, item)

    def send(self, mid, payload=b''):
        self._tx(PLAIN_ROUTE, mid, payload)

    def write_raw(self, data):
        self.events.append([self.t, 'M', 'raw', bytes(data)])
        for ta, item in self.psoc.on_raw(self.t, bytes(data)):
            self._arrive(ta, item)

    def set_raw(self, on):
        self._deliver_until(self.t)
        self.raw = on
        self.queue.clear()
        self.raw_rx.clear()

    def request(self, mid, payload=b'', timeout=None):
        """The reply (id, payload) or None after the request's wait (S-07.011)."""
        timeout = self.k['request_ms'] if timeout is None else timeout
        n = self.slot
        self.slot = (self.slot + 1) % 4
        holder = []
        self.pending = (n, holder)
        self._tx((8 + n) << 4 | PSOC, mid, payload)
        end = self.t + timeout
        while not holder and self.inbox and self.inbox[0][0] <= end:
            self._deliver_until(self.inbox[0][0])
        self.pending = None
        if not holder:
            self._deliver_until(end)
            return None
        return holder[0]

    def sleep(self, ms):
        """Wait, receiving but not handling (frames are queued)."""
        self._deliver_until(self.t + ms)

    def poll(self):
        """Handle the frames received so far (table 2)."""
        self._deliver_until(self.t)
        while self.queue:
            self.handle(*self.queue.pop(0))

    # -- table 2 --
    def update_charge_led(self):
        if self.charge_led_enabled:
            rgb, forced = charge_led(self.state, self.batt['charge'])
            self.leds.set_rgb(rgb, forced)

    def power_state(self, s):
        prev, self.state = self.state, s
        if self.kept is not None and s == 0x10:
            self.kept = None                        # S-07.051: the switch is on again, the power-off dropped
        if power_off_trigger(prev, s):
            if self.fs_window:
                self.kept = s                       # S-07.051: recorded and kept, nothing of steps 1 and 2
                return
            self.power_off(s)

    def power_off(self, s):
        self.shutdown()
        self.send(0x32, bytes([s]))
        raise PowerOff()

    def handle(self, route, mid, payload):
        if mid in (0x31, 0x32):
            if payload:                             # S-07.079: an empty payload records no state
                self.power_state(payload[0])
            self.update_charge_led()
        elif mid in (0x00, 0x35):
            self.update_charge_led()
        elif mid == 0x37:
            self.batt = battery_report(payload)
            self.update_charge_led()
        elif mid == 0x05:
            if self.started and self.greeting and not self.k['nested_regreet']:
                self.greeted = True                 # ends the running sequence, whose messages answer it
                return
            if self.started:
                if not self.greet_sequence():
                    pass                            # logged
                self.send(0x51, b'\x3c\xff\x02')
                self.send(0x43, b'\x01\x01')
            self.send(0x06, b'\x40')
            self.greeted = True
        elif mid == 0x06:
            if payload[:1] == b'\x40':
                self.greeted = True
        elif mid == 0x42:
            self.keys, ev = bitmap_events(self.keys, payload)
            for k, down in ev:
                if self.key_test is not None:
                    self.key_test_event(k, down)
                if self.prompt_on:
                    self.prompt_event(k, down)
        elif mid in (0x45, 0x4D):
            pass
        elif mid == 0x01:
            if self.k['common_held_only'] and not self.in_held:
                return                              # negative control: the common set served from held mode only
            self._tx(route << 4 & 0xF0 | route >> 4, 0x02, payload)
        # any other id: logged only

    def handle_reply(self, reply):
        """Handles the reply to a waited 0x30/0x36 (S-07.012): 0x31/0x32/0x37 by id, any other id updates the
        charge LED, no reply does nothing."""
        if reply is not None and self.k['waited_reply_table2']:
            self.handle(PSOC_ROUTE, *reply)         # negative control: the whole of table 2
        elif reply is not None and reply[0] in (0x31, 0x32, 0x37):
            self.handle(PSOC_ROUTE, *reply)
        elif reply is not None:
            self.update_charge_led()

    # -- start-up --
    def greet_sequence(self):
        self.greeted = False
        self.greeting = True
        try:
            for _ in range(self.k['greet_tries']):
                self.send(0x05, b'\x01\x04')
                self.sleep(self.k['greet_ms'])
                self.poll()
                if self.greeted:
                    return True
            return False
        finally:
            self.greeting = False

    def vbus(self):
        r = self.request(0x33)
        if r is None:
            if self.k['stale_act_nothing']:
                return
            r = (None, b'\x00\x00')                 # S-07.055: no reply counts as {00 00}
            if self.k['stale_readback'] and self.last_34 is not None:
                r = (None, self.last_34)             # negative control (S-16.191): {00 t} of the master's own last 0x34
        self.applied, p = vbus_update(self.applied, self.target, r[1])
        if p is not None:
            self.last_34 = p
            self.send(0x34, p)

    def limit_step(self):
        if self.applied < self.target:
            self.vbus()
            self.sleep(5)

    def charge_wait(self):
        """S-07.054's charge wait, with a fixed target (500 mA)."""
        start = self.t
        last_poll = start if self.k['first_round_late'] else None
        self.target = self.k['charge_target']
        while self.state == 0x16:
            if self.t - start > self.k['wait_guard_ms']:
                raise Halt()
            if last_poll is None or self.t - last_poll > 1000:
                last_poll = self.t
                self.send(0x36)
                self.send(0x30)
                self.vbus()
            if self.applied > 100:
                self.update_charge_led()
            self.sleep(10)
            self.poll()
            self.limit_step()
        self.target = 0
        self.applied = 0

    def battery_first(self):
        """S-07.054's first two requests and any charge wait."""
        self.handle_reply(self.request(0x36))
        self.handle_reply(self.request(0x30))
        if self.state == 0x16 and not self.k['no_charge_wait']:
            self.charge_wait()

    def battery_loop(self):
        """S-07.054's wait loop, after the key status (S-07.023), then the charge LED."""
        if self.state == 0x16 or not battery_usable(self.batt):
            start = self.t
            last_poll = start if self.k['first_round_late'] else None     # S-07.054: the first round at once
            self.last_usb = start
            while self.state == 0x16 or (not battery_usable(self.batt) and self.applied < 500):
                if self.t - start > self.k['wait_guard_ms']:
                    raise Halt()                     # the model's own guard: S-07.058 bounds the wait
                if last_poll is None or self.t - last_poll > 1000:
                    last_poll = self.t
                    self.send(0x36)
                    self.send(0x30)
                    if self.applied != self.target or self.applied < 500:
                        self.vbus()
                if not self.usb_configured and self.target == 0 and self.t - self.last_usb > self.k['charger_ms']:
                    self.target = 1000
                self.sleep(10)
                self.do_actions(only=('usb_config', 'usb_reset'))
                self.poll()
                self.limit_step()
            self.applied = 0
        self.update_charge_led()

    def greet_wait(self, ms):
        """S-07.021: at most ``ms``, ended by the PSoC's greeting (a 0x05, answered by S-07.024, or 0x06 {40}).
        Nothing else is handled or sent meanwhile: the other frames wait in the queue."""
        end = self.t + ms
        while not self.greeted and self.t < end:
            nxt = self.inbox[0][0] if self.inbox else end
            self._deliver_until(min(max(nxt, self.t), end))
            if self.k['greet_wait_full']:
                continue                            # negative control: the wait does not end at a greeting
            for i, (route, mid, payload) in enumerate(self.queue):
                if mid == 0x05 or (mid == 0x06 and payload[:1] == b'\x40'):
                    del self.queue[i]
                    self.handle(route, mid, payload)
                    break
        if not self.greeted:
            self._deliver_until(end)

    def run(self):
        try:
            self._run()
        except (PowerOff, Halt):
            pass
        return self.events

    def _run(self):
        if not self.greet_sequence():
            if self.k['send_exit_on_greet_fail']:
                self.write_raw(EXIT_PACKET)
            self.greet_wait(self.k['greet_fail_wait_ms'])
        self.send(0x30)
        self.send(0x36)
        self.send(0x51, b'\x00\x00\x00')
        if self.k['subscribe_before_battery']:
            self.send(0x43, b'\x01\x01')
        self.battery_first()
        if self.k['subscribe_after_loop']:
            self.battery_loop()                     # negative control: the order before the front
        self.send(0x43, b'\x01\x01')
        self.started = True
        r = self.request(0x41)
        if not self.k['subscribe_after_loop']:
            self.battery_loop()
        hold = key_status(None if r is None else r[1])['hold']
        if self.setup.get('boot_allowed', True) and not hold:
            if self.setup.get('boot_prompt') and self.boot_prompt() == 2:
                self.held()
                return
            self.pmic()
            self.send(0x51, b'\x3c\xff\x32')
            if self.setup.get('os_loads', True):
                self.sleep(self.setup.get('tail_ms', 50))
                if self.k['poll_before_os']:
                    self.poll()                     # negative control: waiting messages handled before the OS
                self.poll_inbox_only()
                return
        self.held()

    def boot_prompt(self):
        """S-05.025: wait for digit 1 or 2 and return it; keys already down when it opens do not count."""
        if not self.k['prompt_acts_on_waiting']:
            self.poll()
        self.prompt_choice = None
        self.prompt_down = {k for k in range(72) if self.keys[k]}
        self.prompt_on = True
        end = self.setup.get('end_ms', self.t + 200)
        while self.prompt_choice is None and self.t < end:
            self.poll()
            self.sleep(10)
        self.prompt_on = False
        if self.prompt_choice is None:
            raise Halt()
        return self.prompt_choice

    def prompt_event(self, k, down):
        if not down:
            self.prompt_down.discard(k)
        elif k in self.prompt_down:
            pass
        elif self.prompt_choice is None:
            self.prompt_choice = {0x06: 1, 0x0A: 1, 0x0E: 1, 0x07: 2, 0x0B: 2, 0x0F: 2}.get(k)

    def poll_inbox_only(self):
        self._deliver_until(self.t)

    def pmic(self):
        self.send(0x70, b'\x01')
        r = self.request(0x73, b'\x03\x10')
        if r is None or len(r[1]) < 3:              # S-07.079: a short reply counts as none
            return
        self.send(0x71, bytes([3, 0x10, r[1][2] | 0x60]))
        if not self.k['skip_pmic_off']:
            self.send(0x70, b'\x00')
        self.request(0x01)

    # -- held mode --
    def held(self):
        self.in_held = True
        self.request(0x09)
        self.send(0x51, b'\x3c\xff\x02')
        end = self.setup.get('end_ms', self.t + 200)
        first = True
        while self.t < end:
            if first:
                self.send(0x40, b'\x01')
                self.leds.set_bar(0)
                first = False
            self.poll()
            self.do_actions()
            self.limit_step()
            self.sleep(10)
        self.poll()

    def do_actions(self, only=None):
        while self.actions and self.actions[0]['t_ms'] <= self.t:
            if only is not None and self.actions[0]['do'] not in only:
                return
            a = self.actions.pop(0)
            getattr(self, 'act_' + a['do'])(a)

    def act_usb_config(self, a):
        self.usb_configured = True
        self.target = a['ma']
        self.last_usb = self.t

    def act_usb_reset(self, a):
        """SET_CONFIGURATION(0), a bus reset or the loss of VBUS: target 100 mA (S-07.057)."""
        self.usb_configured = False
        self.target = 100
        self.last_usb = self.t

    def act_fs_window_open(self, a):
        """S-07.051: the first erase of the first-stage area (S-02.071 step 3)."""
        self.fs_window = True

    def act_fs_window_close(self, a):
        """S-07.051: an image in the first-stage area has passed its read-back (S-02.071 step 5); a kept
        power-off runs now with its kept state."""
        self.fs_window = False
        if self.kept is not None:
            s, self.kept = self.kept, None
            self.power_off(s)

    def act_volcal(self, a):
        self.send(0x48, b'\x01')

    def shutdown(self):
        self.send(0x51, b'\x00\x00\x02')
        self.send(0x40, b'\x01')

    def act_soft_reboot(self, a):
        self.send(0x97, bytes.fromhex('deadbeef'))
        self.shutdown()
        raise Halt()

    def act_continue_os(self, a):
        """Continue to the OS with the slave present (S-05.033): ends a key test (S-07.045), holds, then the
        shutdown messages (S-07.072)."""
        if self.key_test is not None:
            self.key_end()
        self.sleep(RESTART_HOLD_MS)
        self.shutdown()
        raise Halt()

    # -- key test (S-07.045) --
    def act_key_test_start(self, a):
        self.sleep(100)
        self.key_test = {'down': 0, 'lone': 72, 'presses': 0, 'tested': set()}
        self.charge_led_enabled = False
        self.leds.set_bar(0x0E)
        self.key_test_rgb()

    def key_test_rgb(self):
        k = self.key_test['presses'] % 3
        self.leds.set_rgb(bytes([0xFF if k == 0 else 0, 0xFF if k == 1 else 0, 0xFF if k == 2 else 0]))

    def key_test_event(self, key, down):
        kt = self.key_test
        if down:
            if kt['down'] == 0:
                kt['lone'] = key
                kt['presses'] += 1
            else:
                kt['lone'] = 72
            self.leds.set_key(key, b'\xfe\x00')
            kt['down'] += 1
        else:
            kt['down'] = max(kt['down'] - 1, 0)
            self.leds.set_key(key, b'\x00\xff' if key in kt['tested'] else b'\x00\x00')
            if key == kt['lone']:
                kt['tested'].add(key)
                self.leds.set_key(key, b'\x00\xff')
        self.key_test_rgb()

    def act_key_test_stop(self, a):
        self.sleep(100)
        self.key_end()

    def key_end(self):
        self.key_test = None
        self.leds.set_rgb(b'\x00\x00\x00')
        self.leds.set_bar(0)
        self.charge_led_enabled = True
        self.update_charge_led()
        self.send(0x40, b'\x01')

    # -- backplane (S-07.080-.085) --
    def act_backplane(self, a):
        self.backplane_result = False
        if self.unlock is None or image_problem(self.setup['cyacd']) is not None:
            return                                   # S-07.081, S-07.083: fails before entry, nothing sent
        ok = False
        for attempt in range(4):
            if attempt == 0:
                self.poll()
                self.send(0xEE)
                self.set_raw(True)
            elif self.k['resend_ee']:
                self.set_raw(False)
                self.send(0xEE)
                self.set_raw(True)
            else:
                self._tx(PLAIN_ROUTE, 0xEE, b'')    # not transmitted: the link is raw (S-16.147)
            self.raw_rx.clear()
            self.sleep(self.k['enter_wait_ms'])
            self.write_raw(enter_packet(self.unlock))
            self.sleep(5)
            self.sleep(2)
            if probe_verdict(bytes(self.raw_rx)):
                ok = True
                break
        self.backplane_entered = ok
        if not ok:
            self.return_wait()
            return
        text = self.setup['cyacd']
        run_ok = False
        for _ in range(3):
            self.raw_rx.clear()
            host = Host(self._xfer, self.write_raw, self.unlock)
            try:
                run_ok = host.run(text)
            except RunFailed:
                run_ok = False
            if run_ok:
                break
            if self.k['exit_between_runs']:
                self.write_raw(EXIT_PACKET)          # mutant: an exit after every failed run (not S-07.084)
        self.return_wait()
        self.backplane_result = run_ok

    def return_wait(self):
        """S-07.085: framed again, wait for the application's greeting; exit packets from about 1 s."""
        self.set_raw(False)
        if self.k['no_return_wait_after_enter_fail'] and not self.backplane_entered:
            return                                   # mutant: S-07.080's failure left as framed mode alone
        self.greeted = False
        start = self.t
        while True:
            self.poll()
            if self.greeted:
                break                                # the wait ends: no exit packet after the greeting
            self.sleep(10)
            if self.t - start >= self.k['give_up_ms']:
                break
            if self.t - start >= self.k['exit_from_ms']:
                self.write_raw(EXIT_PACKET)

    def _xfer(self, p, n):
        self.raw_rx.clear()
        self.write_raw(p)
        end = self.t + 5000
        while len(self.raw_rx) < n and self.inbox and self.inbox[0][0] <= end:
            self._deliver_until(self.inbox[0][0])
        if len(self.raw_rx) < n:
            self._deliver_until(end)
            return None
        r = bytes(self.raw_rx[:n])
        del self.raw_rx[:n]
        return r


# ---- timed transcripts ----

def ev_to_json(e):
    t = round(e[0], 3)
    if e[2] == 'raw':
        return [t, e[1], 'raw', e[3].hex()]
    return [t, e[1], 'frame', f'{e[3]:#04x}', f'{e[4]:#04x}', e[5].hex()]


def ev_from_json(j):
    if j[2] == 'raw':
        return [j[0], j[1], 'raw', bytes.fromhex(j[3])]
    return [j[0], j[1], 'frame', int(j[3], 16), int(j[4], 16), bytes.fromhex(j[5])]


def kind_of(route):
    return 'request' if route >> 4 >= 8 and route & 0x0F == PSOC else 'plain'


def master_items(events):
    """The master's frames (log lines left out) and raw writes, as comparable keys, with their indices."""
    out = []
    for i, e in enumerate(events):
        if e[1] != 'M':
            continue
        if e[2] == 'raw':
            out.append((i, ('raw', e[3])))
        elif e[4] not in LOG_IDS:
            out.append((i, (kind_of(e[3]), e[4], e[5])))
    return out


def psoc_items(events):
    return [(i, e) for i, e in enumerate(events) if e[1] == 'P']


class CheckError(AssertionError):
    pass


def timed_check(observed, transcript):
    """Check an observed run (a list of events in the transcript's form) against a transcript:
    the master's frames and raw writes equal the transcript's, in order (log lines excepted), and every
    timing check holds. A check {"from": i, "to": j, "min": a, "max": b} names transcript event indices;
    master events map by order among the master's, PSoC events by order among the PSoC's."""
    exp = [ev_from_json(j) for j in transcript['events']]
    em, om = master_items(exp), master_items(observed)
    if [k for _, k in em] != [k for _, k in om]:
        for n, (a, b) in enumerate(zip([k for _, k in em], [k for _, k in om])):
            if a != b:
                raise CheckError(f"{transcript['name']}: master item {n}: expected {a}, observed {b}")
        raise CheckError(f"{transcript['name']}: {len(em)} master items expected, {len(om)} observed")
    ep, op = psoc_items(exp), psoc_items(observed)
    idx = {}
    for (a, _), (b, _) in zip(em, om):
        idx[a] = b
    for (a, _), (b, _) in zip(ep, op):
        idx[a] = b
    for c in transcript.get('checks', []):
        if c['from'] not in idx or c['to'] not in idx:
            raise CheckError(f"{transcript['name']}: check {c} refers to an event not observed")
        d = observed[idx[c['to']]][0] - observed[idx[c['from']]][0]
        if d < c.get('min', -1e12) - 1e-6 or d > c.get('max', 1e12) + 1e-6:
            raise CheckError(f"{transcript['name']}: {c.get('why', '')}: {d:.3f} ms not in "
                             f"[{c.get('min')}, {c.get('max')}]")
    return True


def run_transcript(transcript, knobs=None, unlock=None):
    kw = {} if unlock is None else {'unlock': unlock}
    return Sim(transcript.get('setup'), transcript.get('psoc'), knobs=knobs, **kw).run()
