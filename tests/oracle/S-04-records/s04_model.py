# SPDX-License-Identifier: GPL-2.0-or-later
"""S-04 rules as functions, one requirement each, for the conformance tests. Written independently of
tests/oracle/conformance/images/records.py so the tests can check the package against them.
"""
import datetime
import struct

# ---- S-04.001-S-04.004 the reboot record -----------------------------------------------------------

RECORD_ADDR = 0x11B01F00
RECORD_SIZE = 8
MAGIC = 0xDEADF00D
ABSENT_REASON = 20
STAY = (10, 30, 40, 60)
OS_WRITES = (0, 10, 20, 30, 40)
DEFINED = (0, 10, 20, 30, 40, 50, 60)


def present(rec):
    return len(rec) >= 6 and struct.unpack_from('<I', rec, 0)[0] == MAGIC


def write(mem, reason):
    """S-04.003: store magic and reason at +0..+5; +6..+7 untouched."""
    struct.pack_into('<IH', mem, 0, MAGIC, reason & 0xFFFF)


def consume(mem):
    """S-04.004: present -> zero the magic, return the reason; absent -> 20, no write."""
    if present(mem):
        reason = struct.unpack_from('<H', mem, 4)[0]
        mem[0:4] = bytes(4)
        return reason
    return ABSENT_REASON


def behavior(reason):
    """S-04.006 as a function of the reason taken."""
    reason &= 0xFFFF
    return {
        'boots': reason not in STAY,
        'screen': 'upgrade' if reason in (30, 40) else 'main',
        'cdc': reason == 10,
        'transport': {30: 'msc', 40: 'midi'}.get(reason, '-'),
    }


def host_mode_reason(mode_payload):
    """S-04.021: the payload after the sub-command byte; only its first byte matters."""
    if not mode_payload:
        return 40
    return {1: 30, 2: 10, 4: 50}.get(mode_payload[0], 40)


def nordic_step(reason, no_app=False):
    """S-04.027, for the reboot requests W1-W4. no_app: after a failed application transfer in this boot
    (S-08.048), when the exit is left out."""
    if reason == 20:
        return 'none' if no_app else 'exit'
    return 'enter' if reason != 0 else 'none'


# ---- S-04.040-S-04.046 the metadata record ---------------------------------------------------------

META_SIZE = 0x34
META_MAGIC = 0xDEADBABE
UPGRADING = 0xAB0D
# (name, offset, struct format)
FIELDS = [('magic', 0x00, '<I'), ('serial', 0x04, '10s'), ('sku', 0x0E, '<H'), ('factory', 0x10, '<H'),
          ('os_version', 0x12, '<4H'), ('sw_version', 0x1A, '<4H'), ('state', 0x24, '<I'),
          ('cv_gain', 0x28, '<I'), ('cv_zero', 0x2C, '<H'), ('cv_pair2', 0x2E, '<2H')]


def pack(f):
    """S-04.040: fields -> 0x34 bytes (unused bytes zero)."""
    b = bytearray(META_SIZE)
    struct.pack_into('<I', b, 0x00, f.get('magic', META_MAGIC))
    b[0x04:0x0E] = f.get('serial', b'')[:8].ljust(10, b'\0')
    struct.pack_into('<HH', b, 0x0E, f.get('sku', 0), f.get('factory', 0))
    struct.pack_into('<4H', b, 0x12, *f.get('os_version', (0, 0, 0, 0)))
    struct.pack_into('<4H', b, 0x1A, *f.get('sw_version', (0, 0, 0, 0)))
    struct.pack_into('<IIH', b, 0x24, f.get('state', 0), f.get('cv_gain', 0), f.get('cv_zero', 0))
    struct.pack_into('<2H', b, 0x2E, *f.get('cv_pair2', (0, 0)))
    return bytes(b)


def read(b):
    """S-04.042: the reader's view."""
    b = bytes(b[:META_SIZE])
    if struct.unpack_from('<I', b, 0)[0] != META_MAGIC:
        return {'serial': '', 'sku': 0, 'factory': 0, 'os_version': [0, 0, 0, 0], 'sw_version': [0, 0, 0, 0],
                'state': 0}
    state = struct.unpack_from('<I', b, 0x24)[0]
    return {
        'serial': b[0x04:0x0C].split(b'\0', 1)[0].decode('latin-1'),
        'sku': struct.unpack_from('<H', b, 0x0E)[0],
        'factory': struct.unpack_from('<H', b, 0x10)[0],
        'os_version': list(struct.unpack_from('<4H', b, 0x12)),
        'sw_version': list(struct.unpack_from('<4H', b, 0x1A)),
        'state': state if state == UPGRADING else 0,
    }


def write_record(before, current, factory_mode):
    """S-04.043: the 0x34 bytes a write stores, given the flash bytes just before it."""
    assert current['state'] in (0, UPGRADING)
    b = bytearray(before[:META_SIZE])
    struct.pack_into('<I', b, 0x00, META_MAGIC)
    if factory_mode:
        b[0x04:0x0D] = current['serial'].encode('latin-1')[:8].ljust(9, b'\0')   # +0x0D kept
        struct.pack_into('<HH', b, 0x0E, current['sku'] & 0xFFFF, current['factory'] & 0xFFFF)
    struct.pack_into('<4H', b, 0x12, *current['os_version'])
    struct.pack_into('<4H', b, 0x1A, *current['sw_version'])
    struct.pack_into('<I', b, 0x24, current['state'])
    return bytes(b)


def _udec(s):
    """S-10c.056's unsigned 32-bit reading (S-04.046): leading white space, one optional sign, then the decimal
    digits up to the first non-digit; no digits read 0; beyond 32 bits reads 2^32 - 1 whatever the sign; a '-'
    otherwise negates modulo 2^32."""
    i = 0
    while i < len(s) and s[i] in ' \t\n\v\f\r':
        i += 1
    neg = i < len(s) and s[i] == '-'
    if i < len(s) and s[i] in '+-':
        i += 1
    j = i
    while j < len(s) and s[j].isdigit() and s[j] in '0123456789':
        j += 1
    if j == i:
        return 0
    v = int(s[i:j], 10)
    if v > 0xFFFFFFFF:
        return 0xFFFFFFFF
    return (-v) & 0xFFFFFFFF if neg else v


def set_metadata(fields, request, factory_mode, option):
    """S-04.046: returns (writes, new fields). Applied only with factory mode and the session upgrade option
    both on (S-10c.061)."""
    new = dict(fields)
    if not (factory_mode and option):
        return False, new
    if 'serial' in request:
        if len(request['serial']) != 8:
            return False, dict(fields)
        new['serial'] = request['serial']
    if 'sku_variant' in request:
        v = _udec(request['sku_variant'])
        new['sku'] = v if v <= 999 else 0
    if 'factory' in request:
        new['factory'] = _udec(request['factory']) & 0xFFFF
    return True, new


def m6_writes(sw_version):
    """S-04.045 M6: a failed OS load writes the record exactly when sw_version is not 0.0.0.0 (any of the
    four fields nonzero, build included)."""
    return any(x != 0 for x in sw_version)


# ---- S-04.070 the RTC ------------------------------------------------------------------------------

EPOCH = datetime.date(1970, 1, 1)
SHIFT_FROM = datetime.date(2001, 1, 1)
SHIFT_COUNT = 11321


def rtc_days(d):
    n = (d - EPOCH).days
    return n - 2 if d >= SHIFT_FROM else n


def rtc_encode(dt):
    n = rtc_days(dt.date())
    if not 0 <= n < 1 << 15:
        raise ValueError('date outside the register')
    return (dt.second & 0x3F) | (dt.minute & 0x3F) << 6 | (dt.hour & 0x1F) << 12 | n << 17


def rtc_decode_days(n):
    return EPOCH + datetime.timedelta(days=n + (2 if n >= SHIFT_COUNT else 0))


# ---- S-04.060-S-04.065 status reports --------------------------------------------------------------

def contains(messages, report):
    """True when report's messages occur in messages (a list of (id, payload)) in order, with any others
    between them."""
    want = [(int(m['id'], 16), bytes.fromhex(m['payload'])) for m in report['messages']]
    i = 0
    for m in messages:
        if i < len(want) and m == want[i]:
            i += 1
    return i == len(want)


# ---- S-04.071 the fatal-halt record ------------------------------------------------------------------

FATAL_ADDR = 0x0803DFD4
FATAL_SIZE = 16
DDR_ALIAS = 0x84000000


def fatal_record(general, specific, value, address):
    """S-04.071: four little-endian u32 words at +0, +4, +8, +0xC."""
    return struct.pack('<4I', general, specific, value, address)


def fatal_cause(rec):
    """S-04.071 read backwards: the cause named by (general, specific), or None for no row."""
    g, s, v, _ = struct.unpack('<4I', rec[:FATAL_SIZE])
    if (g, s, v) == (0xB, 1, DDR_ALIAS):
        return 'ddr_check'
    return None


def chip_serial(item1, item1_reads=True, record_reads=True):
    """S-04.047: (text, read) from item 1's bytes. read is 'made', 'failed' (item 1 unreadable) or 'not made'
    (the metadata record unreadable at start-up, so item 1 is not read)."""
    if not record_reads:
        return '', 'not made'
    if not item1_reads:
        return '', 'failed'
    s = bytes(item1[:18]).split(b'\0', 1)[0]
    if any(not 0x20 <= x <= 0x7E or x == 0x3B for x in s):
        return '', 'made'
    return s.decode('ascii'), 'made'


# --- S-04.032 the slave's power-off before every master hard reset (inside S-04.063's shutdown) ---
T_POWER_MIN_MS = 1000       # S-09.060
SLAVE_ARM_MS = 30           # S-04.025: the answer to the slave's arming
HARD_RESET_WRITERS = ('W1', 'W10', 'W11', 'W12', 'W3', 'W4', 'W6', 'W7')
TRACE_EVENTS = ('event', 'absent', 'bridge', 'poweroff', 'answer', 'report', 'link', 'record', 'arm', 'reset',
                'slave_arm', 'slave_reset', 'boot_link')


def read_reset_trace(lines):
    """One event per line, `[@MS] EVENT`; blank lines and `#` comments are ignored. Returns [(ms or None, event)]."""
    events = []
    for line in lines:
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        at, ev = (line.split(None, 1) if line.startswith('@') else (None, line))
        ev = ev.strip()
        if ev not in TRACE_EVENTS:
            raise ValueError('unknown event: ' + ev)
        events.append((int(at[1:]) if at else None, ev))
    return events


def check_reset(writer, events):
    """The rules of S-04.032, S-04.063 and S-04.028 over one master hard reset (a writer of HARD_RESET_WRITERS),
    from its triggering event to the next boot if the trace reaches it. Returns the names of the rules broken;
    a rule needing times is skipped when the events have none."""
    if writer not in HARD_RESET_WRITERS:
        raise ValueError('not a master hard reset: ' + writer)
    names = [e for _, e in events]
    at = {}
    for t, e in events:
        at.setdefault(e, t)

    def idx(name):
        return names.index(name) if name in names else None

    def ms(a, b):
        return None if at.get(a) is None or at.get(b) is None else at[b] - at[a]

    broken = []
    off, rec, arm, ans, rst = idx('poweroff'), idx('record'), idx('arm'), idx('answer'), idx('reset')
    # Only a slave absent before the record excuses the request: one found absent by the next boot is the failure.
    gone = idx('absent')
    absent = gone is not None and (rec is None or gone < rec)
    if rec is None or arm is None or rec > arm:
        broken.append('record_then_arm')
    if rst is None or (arm is not None and rst < arm):
        broken.append('no_reset_after_arm')
    if off is None or (rec is not None and off > rec):
        if not absent:
            broken.append('no_request')
    elif absent:
        broken.append('request_while_absent')
    reports = [i for i, e in enumerate(names) if e == 'report' and (rec is None or i < rec)]
    if len(reports) != 2:
        broken.append('reports_count')                 # S-04.063: each report once
    if reports and off is not None and reports[0] < off:
        broken.append('reports_before_request')
    if reports and ans is not None and reports[0] < ans:
        broken.append('reports_before_answer')
    if reports and off is not None and ans is None:
        d = None if at.get('poweroff') is None else (events[reports[0]][0] - at['poweroff']
                                                      if events[reports[0]][0] is not None else None)
        if d is not None and d < T_POWER_MIN_MS:
            broken.append('no_wait_for_answer')
    if rec is not None and any(e in names[rec:] for e in ('bridge', 'poweroff', 'report', 'link')):
        broken.append('link_after_record')
    if ans is not None:
        d = ms('answer', 'slave_arm')
        if d is not None and d > SLAVE_ARM_MS:
            broken.append('slave_arm_late')
        d, ds = ms('arm', 'reset'), ms('slave_arm', 'slave_reset')
        if d is not None and ds is not None and d < ds:
            broken.append('master_delay_short')
        if 'boot_link' in names and 'slave_reset' not in names[:idx('boot_link')]:
            broken.append('slave_not_reset')
    return broken
