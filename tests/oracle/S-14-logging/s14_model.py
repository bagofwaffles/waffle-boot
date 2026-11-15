# SPDX-License-Identifier: GPL-2.0-or-later
"""S-14 logging: a byte model of the S-14 rules, used by gen_vectors.py. Python 3, standard library only.
Every function names the requirement it models.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'conformance'))
# S-14.001, S-14.002, S-14.004 - S-14.007: the line and tag rules are the conformance package's
# (serial.logline); this module re-exports them.
from serial.logline import (LINE_MAX, CUT_KEEP, NAME_MAX, LINE_NO_MAX,  # noqa: E402,F401
                            tag, parse_tag, build_line, parse_line)

EXPORT_LIMIT = 5 * 1024 * 1024   # S-14.051

LEVELS = {0: 'fatal', 1: 'error', 2: 'warning', 3: 'information', 4: 'debug'}   # S-14.010
DEFAULT_THRESHOLD = 3       # S-14.011

ADDR_SELF, ADDR_HOST, ADDR_PSOC = 1, 2, 4
ROUTE = {'psoc': ADDR_SELF << 4 | ADDR_PSOC, 'cdc': ADDR_SELF << 4 | ADDR_HOST}   # S-14.021, S-14.023
ID_LOG, ID_LOG_ERROR = 0x03, 0x04

# ---- S-14.010 - S-14.013: levels, threshold, the configuration request ---------------------------

def emitted(level, threshold):
    """S-14.011."""
    return level <= threshold


def initial_state():
    """S-14.011, S-14.022, S-14.024: at every start."""
    return {'threshold': DEFAULT_THRESHOLD, 'psoc': True, 'cdc': False}


def apply_log_config(state, payload):
    """The state after a log-configuration request with this payload. S-14.012, S-14.013."""
    payload = bytes(payload)
    st = dict(state)
    if not payload:
        return st
    if 0xA0 <= payload[0] <= 0xA3:
        st['threshold'] = payload[0] - 0xA0 + 1
    if len(payload) >= 2:
        st['psoc'] = bool(payload[1] & 0x01)
        st['cdc'] = bool(payload[1] & 0x02)
    return st


# ---- S-14.021, S-14.023: the frames -------------------------------------------------------------

def frame_id(level):
    """S-14.021: 0x04 for level 1 only."""
    return ID_LOG_ERROR if level == 1 else ID_LOG


def log_frame(sink, level, line, seq):
    """(route, seq, id, payload) of the frame that carries a line on a link sink."""
    return ROUTE[sink], seq % 256, frame_id(level), bytes(line)


# ---- S-14.040 - S-14.042: received log messages -------------------------------------------------

def relay(src, msg_id, payload):
    """(level, text) of the line the bootloader emits for a received 0x03/0x04, or None."""
    payload = bytes(payload)
    if msg_id not in (ID_LOG, ID_LOG_ERROR):
        return None
    if src == ADDR_PSOC:                             # S-14.041
        return None
    if len(payload) < 4:                             # S-14.042
        return None
    text = payload[4:min(len(payload), 248)]        # S-14.040: up to payload byte 248, not included
    text = text.split(b'\x00', 1)[0]
    return (3 if msg_id == ID_LOG else 1), text


# ---- S-14.051, S-14.052: the log export ---------------------------------------------------------

def export_key(e):
    """S-14.051's candidate order: newest first, then the name's bytes in ascending order."""
    return (-e['mtime'], e['name'].encode('utf-8'))


def export(entries, fails=()):
    """The names copied to the USB disk's logs/, in copy order. entries: dicts with name, type ('file' or
    'dir'), mtime, size. fails: names whose copy fails. Candidates go newest first; equal modification
    times go in ascending byte order of the name (S-14.051)."""
    names = [e['name'] for e in entries]
    cand = [e for e in entries if e['type'] == 'file' and e['name'] not in ('.', '..')]
    assert len(set(names)) == len(names)
    total, out = 0, []
    for e in sorted(cand, key=export_key):
        if total + e['size'] > EXPORT_LIMIT:
            break
        if e['size'] <= 0 or e['name'] in fails:
            continue
        total += e['size']
        out.append(e['name'])
    return out
