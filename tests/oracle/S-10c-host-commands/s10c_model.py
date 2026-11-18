# SPDX-License-Identifier: GPL-2.0-or-later
"""S-10c model: the host command protocol, playing the device, used to build and self-check the vectors.
Python 3, standard library only.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'conformance'))
from serial.frame import Frame, swap_route  # noqa: E402  (the conformance package)

# ---- S-10c.001, .002: packing ----


def pack7(data):
    """8-to-7 packing (S-10c.001)."""
    data = bytes(data)
    out = bytearray()
    for i in range(0, len(data), 7):
        group = data[i:i + 7]
        hdr = 0
        for k, b in enumerate(group):
            hdr |= (b >> 7) << k
        out.append(hdr)
        out.extend(b & 0x7F for b in group)
    return bytes(out)


def packed_len(n):
    return n + (n + 6) // 7


def unpack7(data):
    """Unpacking (S-10c.002): m bytes give m - ceil(m/8); bit 7 of every received byte is ignored."""
    data = bytes(data)
    out = bytearray()
    for i in range(0, len(data), 8):
        group = data[i:i + 8]
        hdr = group[0] & 0x7F
        for k, b in enumerate(group[1:]):
            out.append((b & 0x7F) | (((hdr >> k) & 1) << 7))
    return bytes(out)


def unpacked_len(m):
    return m - (m + 7) // 8


# ---- S-10c.003, .004: USB-MIDI event packets ----

MAX_SENT = 288           # S-10c.005
MAX_RECEIVED = 8192      # S-10c.006


def to_packets(msg):
    """S-10c.003: a whole SysEx message as 4-byte event packets on cable 0."""
    msg = bytes(msg)
    out = bytearray()
    i = 0
    while i < len(msg):
        left = len(msg) - i
        if left > 3:
            out += bytes([0x04]) + msg[i:i + 3]
        elif left == 3:
            out += bytes([0x07]) + msg[i:i + 3]
        elif left == 2:
            out += bytes([0x06]) + msg[i:i + 2] + b'\x00'
        else:
            out += bytes([0x05]) + msg[i:i + 1] + b'\x00\x00'
        i += 3
    return bytes(out)


def reassemble(packets):
    """S-10c.004 and S-10c.006: received event packets to the list of complete messages handled."""
    packets = bytes(packets)
    msgs, cur, overlong = [], bytearray(), False

    def end(extra):
        nonlocal cur, overlong
        cur += extra
        m = bytes(cur)
        cur, was_long = bytearray(), overlong
        overlong = False
        if was_long or len(m) > MAX_RECEIVED:
            return
        if len(m) >= 2 and m[0] == 0xF0 and m[-1] == 0xF7:
            msgs.append(m)

    for i in range(0, len(packets) - 3, 4):
        p = packets[i:i + 4]
        if p[0] == 0x04:
            cur += p[1:4]
        elif p[0] in (0x05, 0x06, 0x07):
            end(p[1:1 + (p[0] - 0x04)])
            continue
        elif p[0] == 0x0F:
            if p[1] < 0x80:
                cur.append(p[1])
        else:
            continue
        if len(cur) > MAX_RECEIVED:
            overlong = True
            cur = cur[-1:]  # keep counting; the message is discarded at its end
    return msgs


# ---- S-10c.007, .010, .011: classification and identity ----

IDENTITY_REPLY = bytes.fromhex('f07e2106020020762100010000000000f7')
TE_ID = bytes.fromhex('002076')


def is_te_request(m):
    """S-10c.081."""
    if len(m) < 10 or m[0] != 0xF0 or m[1:4] != TE_ID or m[5] != 0x40 or m[-1] != 0xF7:
        return False
    if not m[6] & 0x40 and len(m) < 11:
        return False
    return True


def classify(m):
    m = bytes(m)
    if is_te_request(m):
        return 'te'
    if m in (bytes.fromhex('f07e210601f7'), bytes.fromhex('f07e7f0601f7')):
        return 'identity'
    if len(m) >= 7 and m[:6] == bytes.fromhex('f00020762166') and m[-1] == 0xF7:
        return 'carrier'
    return 'ignored'


# ---- S-10c.020-.022: the S-06 carrier ----

CARRIER_HEAD = bytes.fromhex('f00020762166')
CARRIER_MAX_STREAM = 245
CARRIER_MAX_RX = 512


def carrier(stream):
    return CARRIER_HEAD + pack7(stream) + b'\xf7'


def carrier_messages_for_frame(encoded):
    """S-10c.022: one frame's encoded bytes (delimiter included) as carrier messages."""
    encoded = bytes(encoded)
    return [carrier(encoded[i:i + CARRIER_MAX_STREAM]) for i in range(0, len(encoded), CARRIER_MAX_STREAM)]


def carrier_stream(m):
    """S-10c.021: the stream bytes of a received carrier message, or None when it is ignored."""
    body = m[6:-1]
    if unpacked_len(len(body)) > CARRIER_MAX_RX:
        return None
    return unpack7(body)


# ---- S-10c.080: TE requests and replies ----


def te_request(cmd, payload=b'', req_id=None, device=0x21, packed=None):
    flags, lo = 0x40, 0
    if req_id is not None:
        flags |= 0x20 | (req_id >> 7) & 0x1F
        lo = req_id & 0x7F
    body = pack7(payload) if packed is None else bytes(packed)
    return bytes([0xF0, 0x00, 0x20, 0x76, device, 0x40, flags, lo, cmd]) + body + b'\xf7'


def parse_te(m):
    """The fields of a TE request or reply: dict(cmd, req_id, status, payload, overflow)."""
    flags = m[6]
    req_id = ((flags & 0x1F) << 7 | m[7] & 0x7F) if flags & 0x20 else None
    reply_form = not flags & 0x40
    start = 10 if reply_form else 9
    body = m[start:-1]
    return {'cmd': m[8], 'req_id': req_id, 'status': m[9] if reply_form else None,
            'payload': unpack7(body), 'overflow': unpacked_len(len(body)) > 4096}


def te_reply(req_cmd, req_id, status, payload=b''):
    if req_id is None:
        flags, lo = 0x00, 0x00
    else:
        flags, lo = 0x20 | (req_id >> 7) & 0x1F, req_id & 0x7F
    return bytes([0xF0, 0x00, 0x20, 0x76, 0x21, 0x40, flags, lo, req_cmd, status]) + pack7(payload) + b'\xf7'


# ---- S-10c.054, .055, .088: texts ----


def version_text(v):
    major, minor, patch, build = v
    s = f'{major}.{minor}.{patch}'
    return s + (f'+{build}' if build else '')


def info_text(serial='', dsp_serial='', hw_rev=255, sku=0, sw=(0, 0, 0, 0), bl=(1, 0, 0, 0),
              os_version=(0, 0, 0, 0), factory=0):
    k = f'TE033AS{sku if sku <= 999 else 0:03d}'
    return (f'mode:bootloader;serial:{serial};dsp_serial:{dsp_serial};hw_rev:{hw_rev};product:OP-XY;sku:{k};'
            f'sw_version:{version_text(sw)};bl_version:{version_text(bl)};os_version:{version_text(os_version)};'
            f'factory:{factory}')


def te_code(w):
    product = (w >> 14) & 0x3FF
    region = (w >> 10) & 0xF
    sku = w & 0x3FF
    product = 0 if product > 999 else product
    sku = 0 if sku > 999 else sku
    return f'TE{product:03d}{ {0: "AS", 1: "XS"}.get(region, "__") }{sku:03d}'


def product_ok(w):
    """S-11.007 product word: product 0x21 in bits 23-14 (0 when over 999), variant bits 13-10 zero."""
    p = (w >> 14) & 0x3FF
    p = 0 if p > 999 else p
    return p == 0x21 and (w >> 10) & 0xF == 0


# ---- S-10c.056: set metadata ----


def kv_lookup(text, key):
    """The value of key in a set-metadata text, or None when absent (S-10c.056): the text is split into
    entries at every ';'; an entry with no ':' has no key, an empty entry is skipped."""
    entries = text.split(';')
    for i, e in enumerate(entries):
        colon = e.find(':')
        if colon < 0 or e[:colon] != key:
            continue
        value = e[colon + 1:]
        if ':' in value or (value == '' and i == len(entries) - 1):
            return None
        return value[:23]
    return None


def kv_lookup_quirk(text, key):
    """The value of key under the S-16.187 reading of a set-metadata text (for quirk_outcome and the
    negative control): kv_lookup's rule, except that an empty entry hides the entry after it."""
    entries = text.split(';')
    i = 0
    while i < len(entries):
        e = entries[i]
        colon = e.find(':')
        if colon >= 0 and e[:colon] == key:
            value = e[colon + 1:]
            if ':' in value or (value == '' and i == len(entries) - 1):
                return None
            return value[:23]
        i += 2 if e == '' else 1
    return None


def parse_u32_decimal(s):
    i = 0
    while i < len(s) and s[i] in ' \t\n\v\f\r':
        i += 1
    neg = False
    if i < len(s) and s[i] in '+-':
        neg = s[i] == '-'
        i += 1
    j = i
    while j < len(s) and s[j].isdigit() and s[j] in '0123456789':
        j += 1
    if j == i:
        return 0
    v = int(s[i:j])
    if v > 0xFFFFFFFF:
        return 0xFFFFFFFF
    return (-v) & 0xFFFFFFFF if neg else v


def set_metadata(payload, factory_mode, meta, write_ok=True, lookup=None):
    """S-10c.056 with S-04.046: returns (reply byte, new meta dict, wrote). factory_mode: factory mode on
    and the session upgrade option on (S-10c.061)."""
    lookup = lookup or kv_lookup
    meta = dict(meta)
    if not factory_mode:
        return 0x04, meta, False
    payload = bytes(payload)
    if len(payload) == 249:
        text = ''
    else:
        text = payload.split(b'\x00', 1)[0].decode('latin-1')
    serial = lookup(text, 'serial')
    if serial is not None:
        if len(serial) != 8:
            return 0x04, meta, False
        meta['serial'] = serial
    sku = lookup(text, 'sku_variant')
    if sku is not None:
        v = parse_u32_decimal(sku)
        meta['sku'] = v if v <= 999 else 0
    fac = lookup(text, 'factory')
    if fac is not None:
        meta['factory'] = parse_u32_decimal(fac) & 0xFFFF
    return (0x03 if write_ok else 0x04), meta, True


# ---- S-10c.041, .042: service and routes ----

SERVED = {
    'cdc': {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09, 0x0B, 0x0D, 0x1F, 0xB0},
    'midi': {0x01, 0x02, 0x09, 0x0D, 0x1F},
}


# S-10c.045: on a coprocessor link the ids of S-07 table 2 and S-08.004 are that link's; every other id is
# served as on CDC.
PEER = {'cdc': 2, 'midi': 2, 'psoc': 4, 'nordic': 3}
COPROCESSOR_LINKS = ('psoc', 'nordic')
CLAIMED = {'psoc': {0x00, 0x05, 0x06, 0x31, 0x32, 0x35, 0x37, 0x42, 0x45, 0x4D},
           'nordic': {0x00, 0x05, 0x31, 0x35, 0x37}}
for _l in COPROCESSOR_LINKS:
    SERVED[_l] = SERVED['cdc'] - CLAIMED[_l]


def reply_route(req_route, cmd_id, link='cdc'):
    """S-10c.042, S-10c.045: a pong on the exchanged route; every other reply from 1 to the link's peer."""
    return swap_route(req_route) if cmd_id == 0x01 else 0x10 | PEER[link]


def progress_target(submitted_by):
    """S-10c.100, S-10c.045: (link, form, route) of the progress reports for a package submitted by a finish
    on `submitted_by` ('cdc', 'midi', 'psoc', 'nordic': the firmware command; 'dfu': the MIDI transfer) or
    from 'ramdisk'."""
    if submitted_by == 'dfu':
        return ('midi', 'te', None)
    if submitted_by == 'ramdisk':
        return ('cdc', 's06', 0x12)
    return (submitted_by, 's06', 0x10 | PEER[submitted_by])


# ---- S-10c.101-.103: progress ----


def percent(done, total):
    if total == 0:
        return 100
    assert done <= total
    return (200 * done + total) // (2 * total)


def progress_s06(done, total, label):
    if done == 0 and total == 0:
        text = label
    else:
        text = f'{percent(done, total)}%{label}'
    return bytes([0x92]) + text.encode('ascii')


def te_percent(done, total, outcome=False):
    """S-10c.103 in the TE reply form: no counts (0, 0) carries 0, except the outcome report (S-10c.106)."""
    if done == 0 and total == 0:
        return 100 if outcome else 0
    return percent(done, total)


STEP_LABELS = ['[content]', '[main dsp]', '[bootloader]', '[ble]', '[ble bootloader]', '[backplane]', '[spiboot]']
SKIP_REASONS = ('option off', 'not confirmed', 'switched off', 'already written')


def skip_label(step, reason):
    """S-10c.106: the label of the report after a step's Skipped."""
    assert reason in SKIP_REASONS
    return '[skipped] ' + STEP_LABELS[step][1:-1] + ': ' + reason


def progress_dfu_payload(done, total, label, outcome=False):
    """S-10c.102: the percent, then the label cut to 62 bytes; no NUL."""
    lab = label.encode('ascii')[:62]
    return bytes([te_percent(done, total, outcome)]) + lab


# ---- the commands, as a small device model ----


class Device:
    """The master's command handling at the host links, for building transcripts (S-10c.040-.091).
    Package checking and runs are outside it: a finish is answered from `next_outcome`."""

    def __init__(self, fsn=bytes(range(16)), fsn_readable=True, meta=None, dsp_serial='', hw_rev=255,
                 bl=(1, 0, 0, 0)):
        self.fsn = bytes(fsn)
        self.fsn_readable = fsn_readable
        self.factory = False
        self.option = False           # the session upgrade option (S-02.055), set at the unit
        self.meta = dict(meta or {'serial': 'AB123456', 'sku': 7, 'factory': 1})
        self.dsp_serial = dsp_serial
        self.hw_rev = hw_rev
        self.bl = tuple(bl)
        self.transfer = None          # None, or dict(bytes, got_chunk)
        self.expected = 0
        self.next_outcome = ('ok', '')
        self.seq = {'cdc': 0, 'midi': 0, 'psoc': 0, 'nordic': 0}
        # S-10c.045: False while the master handles a coprocessor's messages before held mode
        # (S-07.023 steps 1-6a).
        self.held = True
        self.reset = None             # the reason of a reboot or restart (S-04.020), once it has run
        self.slave = True             # False: the slave is absent or has stopped answering (S-09.008)
        self.busy = False             # a scan, run or OS write under way (S-10c.076); a transfer is .transfer

    @property
    def info(self):
        """S-10c.054: the info text from the current record (a set-metadata write shows at once)."""
        m = self.meta
        return info_text(serial=m['serial'], dsp_serial=self.dsp_serial, hw_rev=self.hw_rev, sku=m['sku'],
                         sw=tuple(m.get('sw', (0, 0, 0, 0))), bl=self.bl,
                         os_version=tuple(m.get('os', (0, 0, 0, 0))), factory=m['factory'])

    def _reply(self, link, route, msg_id, payload):
        f = Frame(route, self.seq[link], msg_id, payload)
        self.seq[link] = (self.seq[link] + 1) % 256
        return f

    def s06(self, link, frame):
        """The frames the bootloader writes in answer to one delivered command frame."""
        i = frame.id
        if self.reset is not None or i not in SERVED[link]:   # S-10c.074, S-10c.076
            return []
        r = reply_route(frame.route, i, link)
        p = frame.payload
        if i == 0x01:
            return [self._reply(link, r, 0x02, p)]
        if i == 0x05:
            return [self._reply(link, r, 0x06, b'\x40')]
        if i == 0x07:
            self.factory = self.option and p == bytes.fromhex('cafebabe')      # S-10c.053, S-10c.061
            return [self._reply(link, r, 0x08, b'\x03' if self.factory else b'\x04')]
        if i == 0x09:
            return [self._reply(link, r, 0x0A, self.info.encode('ascii'))]
        if i == 0x0B:
            b, self.meta, _ = set_metadata(p, self.factory and self.option, self.meta)   # S-10c.061
            return [self._reply(link, r, 0x0C, bytes([b]))]
        if i == 0x1F:
            return [self._reply(link, r, 0x20, self.fsn if self.fsn_readable else b'')]
        if i == 0x0D:
            out = self.firmware(p)
            return [] if out is None else [self._reply(link, r, 0x0E, out)]
        return []

    def firmware(self, p):
        sub = p[0] if p else None
        if sub == 0x02:
            w = int.from_bytes(p[10:14], 'big') if len(p) >= 14 else 0
            if not product_ok(w):
                return b'\x84PRODUCT'
            if not self.held:                   # S-10c.045: no transfer before held mode
                return b'\x84'
            self.transfer = {'bytes': b'', 'got_chunk': False}
            return b'\x54'
        if sub == 0x03:
            if self.transfer is None:
                return b'\x84No update in progress!'
            self.transfer['bytes'] += p[1:]
            self.transfer['got_chunk'] = True
            return b'\x54'
        if sub == 0x05:
            if self.transfer is None:
                return b'\x84No update in progress!'
            self.transfer = None
            kind, text = self.next_outcome
            return b'\x54' if kind == 'ok' else b'\x84' + text.encode('ascii')
        if sub in (0x18, 0xEE):
            if self.held:                       # S-04.022 (reason 20), S-04.021; none before held mode
                mode = p[1] if sub == 0xEE and len(p) > 1 else None
                self.reset = 20 if sub == 0x18 else {1: 30, 2: 10, 4: 50}.get(mode, 40)
            return None
        if sub == 0x19:                         # S-10c.076: continue to the OS
            if not self.held or self.busy or self.transfer is not None:
                return b'\x84'
            # the reply first, then the restart: W11, or W2 without the slave (S-04.020);
            # nothing more is served
            self.reset = 80 if self.slave else 20
            return b'\x54'
        if sub == 0xAA:
            return b'\x84compressed firmwares not implemented'
        if sub == 0xAB:
            return b'\x84signed firmwares not implemented'
        return b'\x84'

    def te(self, m):
        """The TE replies to one TE request (S-10c.080-.091); progress replies are not modeled."""
        f = parse_te(m)
        cmd, rid, p = f['cmd'], f['req_id'], f['payload']
        if f['overflow']:
            self.transfer = None
            return [te_reply(cmd, rid, 0x01, b'buffer overflow')]
        if cmd == 0x01:
            return [te_reply(cmd, rid, 0x00, self.info.encode('ascii'))]
        if cmd == 0x02:
            if len(p) > 242:
                return [te_reply(cmd, rid, 0x01, b'buffer overflow')]
            return [te_reply(cmd, rid, 0x00, p)]
        if cmd != 0x03:
            return [te_reply(cmd, rid, 0x02)]
        if len(p) >= 2 and p[0] == 0x03:
            n = p[1]
            if n != self.expected:
                return [te_reply(cmd, rid, 0x01, f'unexpected chunk {n} (exp {self.expected})'.encode())]
            if self.transfer is None:
                return [te_reply(cmd, rid, 0x01, b'unexpected error when handling chunk')]
            self.expected = (n + 1) % 256
            self.transfer['bytes'] += p[2:]
            self.transfer['got_chunk'] = True
            return [te_reply(cmd, rid, 0x00)]
        if len(p) >= 18 and p[0] == 0x02:
            if self.transfer is not None and not self.transfer['got_chunk']:
                return [te_reply(cmd, rid, 0x01, b'dfu already in progress')]
            w = int.from_bytes(p[10:14], 'big')
            if not product_ok(w):
                return [te_reply(cmd, rid, 0x01, b'incorrect sku: ' + te_code(w).encode())]
            self.transfer = {'bytes': b'', 'got_chunk': False}
            self.expected = 0
            return [te_reply(cmd, rid, 0x00, b'\x10\x00')]
        if p and p[0] == 0x04:
            had = self.transfer is not None
            self.transfer = None
            kind, text = self.next_outcome if had else ('err', 'No update in progress!')
            if kind == 'ok':
                return [te_reply(cmd, rid, 0x00)]
            msg = f"error when doing dfu: '{text}'".encode('ascii')[:127]
            return [te_reply(cmd, rid, 0x01, msg)]
        if p and p[0] == 0x01:
            return [te_reply(cmd, rid, 0x00)]
        if p and p[0] == 0x05:
            return [te_reply(cmd, rid, 0x00, b'\x09')]
        return [te_reply(cmd, rid, 0x03)]


# ---- FSN (S-10c.057) ----

def fsn_from_words(words):
    """S-10c.057: the four 32-bit words in order, each least significant byte first."""
    return b''.join((w & 0xFFFFFFFF).to_bytes(4, 'little') for w in words)


# ---- the MIDI link as a whole (S-10c.007, .010, .021, .022) ----


def midi_message(dev, m):
    """The SysEx messages the device sends in answer to one complete received message."""
    from serial.receiver import Receiver
    m = bytes(m)
    kind = classify(m)
    if kind == 'te':
        return dev.te(m)
    if kind == 'identity':
        return [IDENTITY_REPLY]
    if kind != 'carrier':
        return []
    stream = carrier_stream(m)
    if stream is None:
        return []
    rx = Receiver(1)                      # each carrier message starts the receiver afresh
    out = []
    for ev in rx.feed(stream):
        if ev.kind == 'message':
            for r in dev.s06('midi', ev.frame):
                out += carrier_messages_for_frame(r.encode())
    return out
