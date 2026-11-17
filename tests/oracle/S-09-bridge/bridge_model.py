# SPDX-License-Identifier: GPL-2.0-or-later
"""S-09 reference model: the SPI bridge's byte-level rules and the slave boot stream.
Independent of make_vectors.py, which builds the vectors literally."""
import struct

# ---- S-09.020 / S-09.021: requests ----

INIT = 0x01
LINK_MODE = 0x19
LINK_TEST = 0x1A
LINK_STATUS = 0x1B                       # S-09.079
STATUS_LEN = 36                          # S-09.079: its reply session's bytes
QUAD_TEST = 0x1C                         # S-09.080: a developer's quad reply test
IMMEDIATE = {0x01, 0x03, 0x05, 0x07, 0x0A, LINK_MODE, LINK_TEST, LINK_STATUS, QUAD_TEST}
PUSH = {0x1E, 0x1F}
PULL = 0x20
POLL = 0x06
ECHO = 0x0A
WRITE = 0x2B
READ = 0x2A
CLOCK_SYNC = 0x0B
DEFINED = ({0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x11, 0x12, 0x13, 0x14, 0x15,
            0x16, 0x18, LINK_MODE, LINK_TEST, LINK_STATUS, QUAD_TEST, 0x1E, 0x1F, 0x20} | set(range(0x21, 0x33)) |
           {0x34, 0x35, 0x36})
TEST_MAX = 0x10000                       # S-09.071: a link test's most bytes, the largest request's
GATE_TIMES = 3                           # S-09.075: the gate's timed tests of TEST_MAX bytes
WRITE_MAX = 0x8000                       # S-09.023: a file write's most bytes
PLAIN_WRITE_MAX = 0x1000                 # S-09.023: ... as a master sends it in the plain mode
PATH_CMDS = {0x21, 0x22, 0x23, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2E, 0x31, 0x32, 0x35}
EINVAL = 22


def pad4(n):
    return (n + 3) & ~3


def wire_payload_len(n):
    """S-09.021, the master: the number of payload bytes the master sends for a payload of n bytes."""
    p = pad4(n)
    return 0x4C if p in (0x44, 0x48) else p


def slave_rx_payload_len(n):
    """S-09.021, the slave: the number of payload bytes the slave takes as the request for a payload of n
    bytes. The slave does not lengthen 0x44 or 0x48."""
    return pad4(n)


def slave_trail_len(n):
    """S-09.021, the slave: the further bytes the slave receives after the request in the same session and
    ignores: 8 for a padded payload length of 0x44, 4 for 0x48, else 0."""
    return wire_payload_len(n) - pad4(n)


def pad_request_whole(n, moved):
    """S-09.021, S-09.013: whether a request session of an n-byte payload, cut after `moved` bytes, holds
    the whole 12 + pad4(n) byte request."""
    return moved >= 12 + slave_rx_payload_len(n)


def header(cmd, slot, length=0, arg0=0, arg1=0):
    """S-09.020."""
    w0 = (cmd & 0xFF) | (slot & 0xFF) << 8 | (length & 0xFFFF) << 16
    return struct.pack('<III', w0, arg0 & 0xFFFFFFFF, arg1 & 0xFFFFFFFF)


def request(cmd, slot, arg0=0, arg1=0, length=None, payload=b''):
    """A whole request session: header, payload, zero pad (S-09.021). length defaults to the payload
    length."""
    payload = bytes(payload)
    if length is None:
        length = len(payload)
    return header(cmd, slot, length, arg0, arg1) + payload + bytes(wire_payload_len(len(payload)) - len(payload))


def path_request(cmd, slot, path, arg0=0, arg1=0):
    p = path.encode('latin-1') + b'\0'
    return request(cmd, slot, arg0, arg1, len(p), p)


def rename_request(slot, old, new):
    o = old.encode('latin-1') + b'\0'
    n = new.encode('latin-1') + b'\0'
    return request(0x24, slot, 0, len(o), len(o) + len(n), o + n)


def sector_write_request(cmd, slot, sector, data):
    """0x13 / 0x16 (n sectors; len = n*512 mod 2^16) and 0x15 (one sector, arg1 0)."""
    n = len(data) // 512
    assert len(data) == n * 512 and 1 <= n <= 128
    if cmd == 0x15:
        assert n == 1
        return request(cmd, slot, sector, 0, 512, data)
    return request(cmd, slot, sector, n, (n * 512) & 0xFFFF, data)


def slave_payload_len(hdr12):
    """The payload length the slave derives from a header (S-09.023), before padding."""
    w0, a0, a1 = struct.unpack('<III', hdr12[:12])
    cmd, length = w0 & 0xFF, w0 >> 16
    if cmd in (0x13, 0x16):
        return a1 * 512                 # S-09.023, S-09.037
    if cmd == 0x12 and not 1 <= a1 <= 128:
        return length                   # S-09.037
    if cmd in (0x15, 0x1E, 0x1F, 0x24) or cmd in PATH_CMDS:
        return length
    if cmd == WRITE:
        return a1                       # S-09.023
    if cmd == LINK_TEST:
        return a1 if 4 <= a1 <= TEST_MAX else 0     # S-09.071, S-09.037
    if cmd in DEFINED:
        return 0
    return length                       # S-09.037: an undefined command


def malformed_reply(hdr12, payload=b''):
    """S-09.037: the reply a slave finishes for a malformed request of a defined command, or None when
    the request is well formed. payload: the request's payload bytes (len of them)."""
    w0, a0, a1 = struct.unpack('<III', hdr12[:12])
    cmd, slot, length = w0 & 0xFF, (w0 >> 8) & 0xFF, w0 >> 16
    payload = bytes(payload)[:length]
    if cmd in (0x12, 0x13, 0x16) and not 1 <= a1 <= 128:
        return deferred_reply(cmd, slot, -1, EINVAL)                    # no sector data
    if cmd == READ and not 1 <= length <= 0x1000:
        return read_piece_reply(slot, -1, err=EINVAL)                   # one piece, complete
    if cmd == 0x24:
        ok = (1 <= a1 <= length and payload[a1 - 1:a1] == b'\0' and length > a1
              and payload[length - 1:length] == b'\0')
    elif cmd in PATH_CMDS:
        ok = length >= 1 and payload[length - 1:length] == b'\0'
    else:
        return None
    if ok:
        return None
    result = 0 if cmd in (0x2E, 0x31, 0x32) else -1
    n = REPLY_DATA_LEN.get(cmd)
    return deferred_reply(cmd, slot, result, EINVAL, bytes(n(a1)) if n else None)


def slave_request_len(hdr12, mode=0):
    """S-09.021: the bytes the slave receives as the request: header, payload and pad4; outside the plain
    mode the check's 8 bytes follow (S-09.072)."""
    return 12 + slave_rx_payload_len(slave_payload_len(hdr12)) + (CHECK_LEN if mode != PLAIN else 0)


def slave_session_len(hdr12, mode=0):
    """S-09.012, S-09.021, S-09.072: the bytes the slave expects in the request session: the request plus
    any trailing bytes in the plain mode, 12 + pad4(n) + 8 outside it."""
    n = slave_payload_len(hdr12)
    if mode != PLAIN:
        return 12 + pad4(n) + CHECK_LEN
    return 12 + slave_rx_payload_len(n) + slave_trail_len(n)


# ---- S-09.070 - S-09.075: link modes, the check, the link test, the trial, the fall-back, the rungs ----

PLAIN, CHECKED, QUAD = 0, 1, 2           # S-09.070
TRY, KEEP = 0, 1                         # 0x19's arg1
CHECK_LEN = 8                            # S-09.072: the trailer, the number then the check
CHECK_POLY = 0x04C11DB7                  # S-09.072: CRC-32/BZIP2's
TEST_START = 0x61C88646                  # S-09.071
M32 = 0xFFFFFFFF
ECHO_TRIES = 8                           # S-09.074
ECHO_RECOVER = 0x6B3D90E5                # S-09.074: the recovery echo's arg0 (arg1 0, no payload)
REPEATS = 3                              # S-09.076: a request sent again at most
UNNUMBERED = {0x0A, 0x19, 0x1A, 0x1B}    # S-09.076: the requests with no number
# S-09.013 rule 4: plain requests whose failed exchange is recovered by echo and drain
PLAIN_SAFE = {0x03, 0x05, 0x11, 0x12, 0x18, 0x21, 0x22, 0x23, 0x31, 0x32, QUAD_TEST}
PLAIN_DRAIN = 5                          # S-09.013 rule 4: the polls of the drain, at most
EIO = 5                                  # S-09.013 rule 4: the master's errno for such a request


def _sums(b, a, s):
    """The two running sums over the little-endian words of b: a += w, then s += a, each mod 2^32."""
    assert len(b) % 4 == 0
    for (w,) in struct.iter_unpack('<I', bytes(b)):
        a = (a + w) & M32
        s = (s + a) & M32
    return a, s


def check_word(b):
    """S-09.072: CRC-32/BZIP2 over the little-endian words of b, each word most significant bit first
    (S-13b.040)."""
    assert len(b) % 4 == 0
    c = M32
    for (w,) in struct.iter_unpack('<I', bytes(b)):
        for sh in (24, 16, 8, 0):
            c = (c << 8 & M32) ^ _CHECK_TABLE[(c >> 24 ^ w >> sh) & 0xFF]
    return c ^ M32


def _check_byte(i):
    """The register after shifting in the byte i, most significant bit first."""
    c = i << 24
    for _ in range(8):
        c = (c << 1 ^ CHECK_POLY) & M32 if c & 0x80000000 else c << 1 & M32
    return c


_CHECK_TABLE = [_check_byte(i) for i in range(256)]


def trailer(body, seq):
    """S-09.072, S-09.076: the 8-byte trailer: the number, then the check word over body and number."""
    num = struct.pack('<I', seq & M32)
    return num + struct.pack('<I', check_word(bytes(body) + num))


def reply_in_mode(mode, reply):
    """S-09.072: the reply session in a mode: outside the plain mode, followed by its check word."""
    reply = bytes(reply)
    return reply if mode == PLAIN else reply + struct.pack('<I', check_word(reply))


def check(body, seq=0):
    """The trailer of body with the number seq."""
    return trailer(body, seq)


def default_seq(cmd):
    """S-09.076: 0 for an unnumbered request, else 1."""
    return 0 if cmd in UNNUMBERED else 1


def seq_of(sess):
    """The number a session outside the plain mode carries."""
    return struct.unpack_from('<I', sess, len(sess) - 8)[0]


def test_value(padded):
    """S-09.071: a link test's reply value over its pad4(n) payload bytes as received."""
    a, b = _sums(padded, TEST_START, len(padded))
    return a ^ ((b << 16 | b >> 16) & M32)


def request_in_mode(mode, cmd, slot, arg0=0, arg1=0, length=None, payload=b'', seq=None):
    """A whole request session in a link mode (S-09.021, S-09.072); seq defaults to default_seq(cmd)."""
    payload = bytes(payload)
    if mode == PLAIN:
        return request(cmd, slot, arg0, arg1, length, payload)
    if length is None:
        length = len(payload)
    if seq is None:
        seq = default_seq(cmd)
    body = header(cmd, slot, length, arg0, arg1) + payload + bytes(pad4(len(payload)) - len(payload))
    return body + trailer(body, seq)


def link_test_request(slot, payload, arg0=0, mode=PLAIN):
    """S-09.071: 0x1A with arg1 = n, len = n mod 2^16; no number."""
    n = len(payload)
    return request_in_mode(mode, LINK_TEST, slot, arg0, n, n & 0xFFFF, payload)


def link_test_reply_value(hdr12, payload):
    """S-09.071, S-09.037: the slave's reply value for a 0x1A request."""
    n = slave_payload_len(hdr12)
    return test_value(bytes(payload)[:pad4(n)].ljust(pad4(n), b'\0'))


def checked_form_ok(sess):
    """S-09.072, S-09.076: whether a session outside the plain mode has its length, check and number right."""
    if len(sess) < 12 + CHECK_LEN:
        return False
    n = slave_payload_len(sess[:12])
    body = 12 + pad4(n)
    if len(sess) != body + CHECK_LEN or sess[body:] != trailer(sess[:body], seq_of(sess)):
        return False
    return (seq_of(sess) == 0) == (sess[0] in UNNUMBERED)


class LinkSlave:
    """S-09.070, S-09.073, S-09.074, S-09.076: the slave's link state and its numbering memory."""

    def __init__(self):
        self.mode, self.trial, self.notice = PLAIN, False, False
        self.recovering = False          # S-09.074: the recovery echo taken, no 0x19 yet
        self.last = None                 # (number, check word): the last numbered request but a poll
        self.last_poll = None            # ... and the last numbered poll, while it is the last numbered
        self.last_reply = None

    def state(self):
        return {'mode': self.mode, 'trial': self.trial, 'notice': self.notice}

    def fall(self):
        """S-09.074: the fall-back to the plain mode."""
        if self.mode != PLAIN:
            self.mode, self.trial, self.notice, self.recovering = PLAIN, False, True, False

    def numbered(self, sess, reply=None):
        """S-09.076: classify a whole checked session: 'new', 'repeat' or 'fault' (falls back). reply: the
        reply a new request is given."""
        seq, chk = struct.unpack_from('<II', sess, len(sess) - 8)
        poll = sess[0] == POLL
        mine, other = (self.last_poll, self.last) if poll else (self.last, self.last_poll)
        if (seq == 0) != (sess[0] in UNNUMBERED) or (seq and mine and mine[0] == seq and mine[1] != chk) or \
                (seq and other and other[0] == seq):
            self.fall()
            return 'fault'
        if seq and mine == (seq, chk):
            return 'repeat'
        if seq and poll:
            self.last_poll = (seq, chk)
        elif seq:
            self.last, self.last_poll, self.last_reply = (seq, chk), None, reply
        return 'new'

    def start_again(self):
        """S-09.015: reset to the start state."""
        self.__init__()

    def link_mode(self, m, how):
        """S-09.073: 0x19; returns the reply's value."""
        if m > QUAD:
            return self.mode
        if m == PLAIN:
            self.mode, self.trial = PLAIN, False
        elif how == KEEP:
            if self.mode == m:
                self.trial = False
        else:
            self.mode, self.trial = m, True
        return self.mode

    def event(self, ev, cmd=None, arg0=0, arg1=0):
        """One event; returns (executed, value): value is 0x19's reply value, else None.
        ev: 'request' (a whole request with its check right), 'bad_check', 'short', 'ignored',
        'rx_fail', 'trial_timeout'."""
        if ev == 'request':
            if self.mode != PLAIN and self.trial and cmd not in (LINK_MODE, LINK_TEST):
                self.fall()
                return False, None
            if self.mode == PLAIN and cmd == INIT and arg0 == 0 and arg1 == 0:
                self.start_again()       # S-09.015
                return True, None
            recovery_echo = cmd == ECHO and arg0 == ECHO_RECOVER and arg1 == 0
            if self.notice and not recovery_echo:
                return False, None       # S-09.074: dropped
            if self.recovering and not recovery_echo and not (cmd == LINK_MODE and arg0 <= QUAD and
                                                              arg1 == 0) and \
                    not (cmd == LINK_STATUS and arg0 == 0 and arg1 == 0):
                return False, None       # S-09.074, S-09.079: recovering
            self.recovering = self.notice or (self.recovering and cmd != LINK_MODE)
            self.notice = False
            if cmd == LINK_MODE:
                return True, self.link_mode(arg0, arg1)
            return True, None
        if ev == 'trial_timeout':
            if self.trial:
                self.fall()
            return False, None
        self.fall()
        return False, None


# S-09.075: the rungs, slowest first; (mode, quad I/O, SPI0_CTL without EN, SPI0_CLK) per S-13b.041
RUNGS = {0: (PLAIN, False, 0x00002592, 3), 1: (CHECKED, False, 0x00002592, 3),
         2: (CHECKED, False, 0x00042592, 1), 3: (QUAD, True, 0x00242592, 3), 4: (QUAD, True, 0x00242592, 1)}
TOP = 4


def rung_mode(r):
    return RUNGS[r][0]


def gate_passes(big_us, words, plain_word_ns=1280):
    """S-09.075: whether a rung passes the gate, given one largest request's time in microseconds."""
    plain_us = (words * plain_word_ns + 999) // 1000
    return big_us * 16 <= plain_us * 15


def _down(top, carries, failed, gains):
    """The rungs from top down to 2, a failed one skipped: (tried, kept or 0)."""
    tried = []
    for r in range(top, 1, -1):
        if r in failed:
            continue
        tried.append(r)
        if carries(r) and gains(r):
            return tried, r
        failed.add(r)
    return tried, 0


def master_raise(ceiling, carries, failed=frozenset(), gains=lambda r: True):
    """S-09.075: the master's raise from the plain mode. carries(r): rung r's sessions get through;
    gains(r): rung r passes its gate. Returns (rungs tried in order, rung kept or 0, rungs failed)."""
    failed = set(failed)
    tried, kept = _down(ceiling, carries, failed, gains)
    if not kept and ceiling >= 1 and 1 not in failed:
        tried.append(1)
        if carries(1):
            kept = 1
        else:
            failed.add(1)
    return tried, kept, failed


def master_recover(failed_from, ceiling, carries, failed=frozenset(), gains=lambda r: True):
    """S-09.075, S-09.076: the rungs the master tries after a failure on rung failed_from, ending at rung 1.
    Returns as master_raise()."""
    failed = set(failed) | ({failed_from} if failed_from > 1 else set())
    tried, kept = _down(min(ceiling, failed_from - 1), carries, failed, gains)
    if not kept:
        tried.append(1)
        if carries(1):
            kept = 1
    return tried, kept, failed


# S-09.073: the lengths and patterns a trial's tests must cover
TEST_LENGTHS = {4, 61, 0x1000, 0x1004, 0x7FFC, 0x8000, 0x10000}
TEST_PATTERNS = {'00', 'ff', 'aa', '55', '0f', 'f0', 'walk', 'walk0', 'count', 'random'}


def _walks(ws):
    """One bit set in each word, a different bit from the word before, every bit of the word in turn."""
    bits = [w.bit_length() - 1 for w in ws]
    return (len(ws) >= 32 and all(w and w & (w - 1) == 0 for w in ws) and
            all(bits[j] != bits[j - 1] for j in range(1, len(bits))) and len(set(bits)) == 32)


def test_pattern(p):
    """The pattern class of a link test's payload (S-09.073): a constant byte as hex, 'walk', 'walk0',
    'count' or 'random'."""
    p = bytes(p)
    if p and p.count(p[0]) == len(p):
        return '%02x' % p[0]
    ws = [w for (w,) in struct.iter_unpack('<I', p)] if len(p) % 4 == 0 else []
    if ws and _walks(ws):
        return 'walk'
    if ws and _walks([~w & M32 for w in ws]):
        return 'walk0'
    if len(p) > 1 and all((p[i + 1] - p[i]) % 256 in (1, 2) for i in range(len(p) - 1)):
        return 'count'
    return 'random'


# ---- S-09.030 .. S-09.035: replies ----

def hdr_word(cmd, slot):
    return struct.pack('<I', (cmd & 0xFF) | (slot & 0xFF) << 8)


def w32(v):
    return struct.pack('<I', v & 0xFFFFFFFF)


def immediate_reply(cmd, slot, value):
    return hdr_word(cmd, slot) + w32(value)


def status_reply(slot, fell, why, cmd, mode, length, got, words=(0, 0, 0, 0)):
    """S-09.079: 0x1B's reply."""
    return (hdr_word(LINK_STATUS, slot) + w32(fell) + w32(why | cmd << 8 | mode << 16) + w32(length) +
            w32(got) + b''.join(w32(w) for w in words))


def deferred_reply(cmd, slot, result, err=0, data=None):
    """S-09.032: [hdr][result][errno] and data/pad4."""
    out = hdr_word(cmd, slot) + w32(result) + w32(err)
    if data is not None:
        data = bytes(data)
        out += data + bytes(pad4(len(data)) - len(data))
    return out


POLL_NONE = bytes([0x06, 0xFF, 0x00, 0x00])

REPLY_DATA_LEN = {0x11: lambda a1: 512, 0x12: lambda a1: a1 * 512, 0x22: lambda a1: 0x34,
                  0x23: lambda a1: 0x0C, 0x2F: lambda a1: 0x10C, 0x31: lambda a1: 8, 0x32: lambda a1: 8}


def deferred_reply_len(cmd, arg1=0):
    n = REPLY_DATA_LEN.get(cmd)
    return 12 + (pad4(n(arg1)) if n else 0)


def pull_reply(cmd_slot, data, overflow=False):
    """S-09.034: [hdr][n | f<<16] data/pad4."""
    slot = cmd_slot
    data = bytes(data)
    w = len(data) | (0x10000 if overflow else 0)
    return hdr_word(PULL, slot) + w32(w) + data + bytes(pad4(len(data)) - len(data))


def read_piece_reply(slot, r, data=b'', err=0):
    """S-09.035 step 2."""
    if r > 0:
        assert len(data) == r
        return hdr_word(READ, slot) + w32(r) + bytes(data) + bytes(pad4(r) - r)
    if r == 0:
        return hdr_word(READ, slot) + w32(0)
    return hdr_word(READ, slot) + w32(r) + w32(err)


def file_read(file_bytes, pos, n, chunk=0x1000, fail_piece=None, fail_errno=19):
    """S-09.035: the pieces [(r, data, errno)] a read of n bytes at pos yields, and the operation's
    result. fail_piece: index of a piece that fails (r = -1)."""
    pieces, received, k = [], 0, 0
    while True:
        want = min(chunk, n - received)
        if fail_piece is not None and k == fail_piece:
            pieces.append((-1, b'', fail_errno))
            return pieces, (-1, fail_errno)
        data = file_bytes[pos + received: pos + received + want]
        r = len(data)
        pieces.append((r, data, 0))
        received += r
        k += 1
        if not (r == chunk and received < n):
            return pieces, (received, 0)


# ---- S-09.002 .. S-09.006: the slave boot stream ----

def stream_sent(record_crc, computed_crc, trial_start):
    """S-09.002: whether the running slot's stream goes on the boot link: its record's crc32 equals the
    CRC of its bytes (S-02.016), or is that CRC's complement, the trial mark, while this start is the
    slot's trial start."""
    if record_crc == computed_crc:
        return True
    return bool(trial_start) and record_crc == computed_crc ^ 0xFFFFFFFF


def slave_stream(slot):
    """The chip-select sessions on the boot link for a running slot whose stored stream is `slot`
    (bytes), after a 0xFF answer to 0x03. Returns (sessions, end) with sessions a list of bytes, the
    first being b'\\x03', and end one of 'final', 'badsig', 'slot_end'."""
    slot = bytes(slot)
    sessions = [b'\x03']
    off = 0
    while True:
        if off + 16 > len(slot):
            return sessions, 'slot_end'
        h = slot[off:off + 16]
        if h[3] != 0xAD:
            return sessions, 'badsig'
        code, _, count, _ = struct.unpack('<IIII', h)
        flags = code & 0xFFF0
        header_only = bool(flags & 0x100) or count == 0
        end = off + 16 + (0 if header_only else count)
        if end > len(slot):
            return sessions, 'slot_end'
        if header_only:
            sessions.append(h)
        else:
            payload = slot[off + 16:end]
            pieces = [payload[i:i + 1024] for i in range(0, count, 1024)]
            sessions.append(h + pieces[0])
            sessions.extend(pieces[1:])
        off = end
        if flags & 0x8000:
            return sessions, 'final'


NO_OPEN_SESSION = b'\x03' + b'\x0f' * 11     # S-09.007, as the master sends it


def fast_parts(sessions):
    """S-09.009: the parts of each stream session (the opening one left out) as the option sends them: a
    block's 16-byte header alone, then the payload of that session after it. A list per session of the
    parts' lengths; their sum is the session's length."""
    out, have, left, head = [], 0, 0, b''
    for s in sessions[1:]:
        parts, i = [], 0
        while i < len(s):
            if left:
                k = min(left, len(s) - i)
                left -= k
            else:
                k = min(16 - have, len(s) - i)
                head += s[i:i + k]
                have += k
                if have == 16:
                    code, _, count, _ = struct.unpack('<IIII', head)
                    left = 0 if code & 0x100 else count
                    have, head = 0, b''
            parts.append(k)
            i += k
        out.append(parts)
    return out


# ---- S-09.040: the erase request ----

ERASE_ARG = 0                                    # CMD38's erase argument: the only one a 0x14 may carry


def slave_erase(slot, sector, arg):
    """S-09.040: a slave's 0x14. Returns (the eMMC commands [(cmd, argument)], reply bytes)."""
    if arg & 0xFFFFFFFF != ERASE_ARG:
        return [], deferred_reply(0x14, slot, -1, 0)
    return [(35, sector), (36, sector), (38, ERASE_ARG)], deferred_reply(0x14, slot, 0, 0)


# ---- S-09.046: the clock message's value ----

def clock_value(clk):
    """S-09.046: seconds since 1970-01-01 UTC of the date and time S-04.070 decodes from RTC0 CLK."""
    sec, mins, hrs, days = clk & 0x3F, clk >> 6 & 0x3F, clk >> 12 & 0x1F, clk >> 17 & 0x7FFF
    if days >= 11321:
        days += 2
    return days * 86400 + hrs * 3600 + mins * 60 + sec


def clock_message(clk):
    """The 0xBD payload: four bytes, little-endian."""
    return struct.pack('<I', clock_value(clk))


def master_clock_messages(reads):
    """S-09.046: the 0xBD payloads for a sequence of CLK reads: the first, then each change."""
    out, last = [], None
    for clk in reads:
        v = clock_value(clk)
        if v != last:
            out.append(struct.pack('<I', v))
            last = v
    return out


# ---- S-09.042 / S-09.043 ----

ERRNO = {'ENOENT': 2, 'EBADF': 9, 'ENOMEM': 12, 'EACCES': 13, 'EFAULT': 14, 'EBUSY': 16, 'EEXIST': 17,
         'EXDEV': 18, 'ENODEV': 19, 'ENOTDIR': 20, 'EISDIR': 21, 'EINVAL': 22, 'ENFILE': 23, 'ENOSPC': 28,
         'EROFS': 30, 'ERANGE': 34, 'ELOOP': 40, 'ENODATA': 61, 'ENAMETOOLONG': 78, 'ENOTEMPTY': 93}


def stat_record(dev, ino, mode, nlink, uid, gid, rdev, size, blksize, blocks, atime, mtime, ctime):
    return struct.pack('<13I', dev, ino, mode, nlink, uid, gid, rdev, size, blksize, blocks, atime, mtime, ctime)


def statfs_record(bsize, blocks, bfree):
    return struct.pack('<III', bsize, blocks, bfree)


def dirent_record(ino, off, dtype, name):
    nm = name.encode('latin-1')
    assert len(nm) <= 256
    return struct.pack('<IIHB', ino, off, 0x10C, dtype) + nm + bytes(257 - len(nm))


# ---- S-09.011 / S-09.013: how a slave session ends ----

def session_end_ok(expect, moved, rose, fell_again):
    """S-09.011, S-09.013: whether a slave session ended right. expect / moved: bytes; rose: PA4 rose after
    the last move while PB15 read low; fell_again: PA4 then fell before PB15 rose."""
    if not 0 <= moved <= expect or (fell_again and not rose):
        return False
    if fell_again:
        return False
    return rose if moved == expect else True


# ---- S-09.080: the quad reply test ----

QT_ARM, QT_SEND, QT_RECORD = 0, 1, 2     # arg0's operation, bits 3-0
QT_TF_DMA, QT_TF_CORE, QT_SPIF = 1, 2, 3  # the candidates, arg0 bits 5-4
QT_REN = 0x40                            # arg0 bit 6, with a TF candidate only
QT_PATS = 6                              # arg0 bits 11-8; bits 31-16 the seed
QT_MIN, QT_MAX = 12, 0x10000             # the reply's bytes, whole words
QT_ARMED, QT_BAD, QT_NOT_PLAIN, QT_NOT_ARMED, QT_NO_SLAVE = 0, 1, 2, 3, 4
QT_REC_WORDS = 18                        # RECORD's reply, the header word first
QT_REC_LEN = 4 * QT_REC_WORDS


def qt_arg0(cand, ren, pat, seed):
    """ARM's arg0."""
    return QT_ARM | (cand & 3) << 4 | (QT_REN if ren else 0) | (pat & 0xF) << 8 | (seed & 0xFFFF) << 16


def qt_args_ok(arg0, arg1):
    """S-09.080: ARM's arguments in range."""
    cand, pat = arg0 >> 4 & 3, arg0 >> 8 & 0xF
    return (arg0 & 0xF == QT_ARM and QT_TF_DMA <= cand <= QT_SPIF and not arg0 & 0x80 and
            not (arg0 & QT_REN and cand == QT_SPIF) and pat < QT_PATS and not arg0 & 0xF000 and
            QT_MIN <= arg1 <= QT_MAX and arg1 % 4 == 0)


def qt_word(pat, seed, j):
    """Word j of a pattern's payload, k = j + seed."""
    k = (j + seed) & M32
    if pat == 0:
        return 1 << (k % 32)
    if pat == 1:
        return ~(1 << (k % 32)) & M32
    if pat == 2:
        return 0x11111111 << (k % 4) & M32
    if pat == 3:
        return ~(0x11111111 << (k % 4)) & M32
    if pat == 4:
        return 0xAAAAAAAA if k & 1 else 0x55555555
    x = (k * 0x9E3779B9 & M32) ^ (seed << 7 & M32)
    x ^= x >> 16
    x = x * 0x85EBCA6B & M32
    x ^= x >> 13
    x = x * 0xC2B2AE35 & M32
    return x ^ x >> 16


def qt_reply(slot, arg0, n):
    """S-09.080: SEND's test reply of n bytes: 0x1C's header word, the pattern, the check word of the rest
    (S-09.072)."""
    pat, seed = arg0 >> 8 & 0xF, arg0 >> 16
    body = hdr_word(QUAD_TEST, slot) + b''.join(struct.pack('<I', qt_word(pat, seed, j))
                                                for j in range(n // 4 - 2))
    return body + struct.pack('<I', check_word(body))


def qt_lines(diff):
    """The quad data lines a word's wrong bits were on: line k carries bit k of each nibble."""
    lines = 0
    for i in range(8):
        lines |= diff >> (4 * i)
    return lines & 0xF

