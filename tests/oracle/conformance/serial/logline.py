# SPDX-License-Identifier: GPL-2.0-or-later
"""Log lines on an S-06 link: the S-14 line codec and a filter for log frames in a decoded stream.

A line (S-14.001) is a 4-byte big-endian timestamp in microseconds (mod 2^32), an optional tag
``name[line]: `` (S-14.004) and the text, at most 248 bytes; a longer line is cut to 246 bytes plus
``..`` (S-14.006); one trailing LF is dropped (S-14.007). ``LOG_IDS``: line and error line (S-14.021).
"""
import re

from . import frame as fr

LINE_MAX = 248
CUT_KEEP = 246
NAME_MAX = 16
LINE_NO_MAX = 2147483647
LOG_IDS = (0x03, 0x04)

_NAME_CHARS = b'abcdefghijklmnopqrstuvwxyz0123456789_-'
_TAG_RE = re.compile(rb'([a-z0-9_-]{0,16})\[((?:[1-9][0-9]{0,9})?)\]: ')


def tag(name, line_no):
    """The tag for a source name and a line number (0 or None: no number)."""
    name = name.encode() if isinstance(name, str) else bytes(name)
    if len(name) > NAME_MAX or any(c not in _NAME_CHARS for c in name):
        raise ValueError(f'name {name!r} outside the tag grammar')
    if line_no and not 1 <= line_no <= LINE_NO_MAX:
        raise ValueError(f'line {line_no} outside 1..{LINE_NO_MAX}')
    return name + b'[' + (str(line_no).encode() if line_no else b'') + b']: '


def parse_tag(body):
    """(name, line_no or None, rest) if the body starts with a tag, else None."""
    m = _TAG_RE.match(body)
    if not m:
        return None
    num = m.group(2)
    if num and int(num) > LINE_NO_MAX:
        return None
    return m.group(1).decode(), (int(num) if num else None), body[m.end():]


def build_line(timestamp, text, name=None, line_no=None):
    """The line for a timestamp (µs, any integer: taken mod 2^32), a text (bytes, no 0x00) and an
    optional tag (name None: no tag)."""
    text = text.encode() if isinstance(text, str) else bytes(text)
    if b'\x00' in text:
        raise ValueError('text contains 0x00')
    full = (timestamp % (1 << 32)).to_bytes(4, 'big')
    if name is not None:
        full += tag(name, line_no)
    full += text
    if len(full) > LINE_MAX:
        return full[:CUT_KEEP] + b'..'
    if text.endswith(b'\n'):                         # the text's own last byte only
        full = full[:-1]
    return full


def parse_line(line):
    """{'timestamp', 'tagged', 'name', 'line_no', 'text'} for a line; ValueError if it is not one."""
    line = bytes(line)
    if not 4 <= len(line) <= LINE_MAX:
        raise ValueError(f'line length {len(line)} outside 4..{LINE_MAX}')
    body = line[4:]
    if b'\x00' in body:
        raise ValueError('body contains 0x00')
    t = parse_tag(body)
    out = {'timestamp': int.from_bytes(line[:4], 'big'), 'tagged': t is not None}
    if t:
        out.update(name=t[0], line_no=t[1], text=t[2])
    else:
        out.update(name=None, line_no=None, text=body)
    return out


def is_log(frame, ids=LOG_IDS):
    return getattr(frame, 'id', None) in ids


def log_matcher(ids=LOG_IDS):
    """A transcript matcher for log frames (for a Script's ``ignore``)."""
    from .transcript import Match
    return Match(id=set(ids))


def split_logs(stream, ids=LOG_IDS):
    """(log frames, other items) of a decoded stream: a list of items, or encoded bytes (decoded with
    ``frame.decode_stream``; malformed pieces go to the other items as FrameErrors)."""
    if isinstance(stream, (bytes, bytearray)):
        stream = [f for f, _ in fr.decode_stream(bytes(stream))[0]]
    logs, other = [], []
    for it in stream:
        (logs if is_log(it, ids) else other).append(it)
    return logs, other


def log_lines(stream, ids=LOG_IDS):
    """The parsed lines of the log frames in ``stream``, each with its frame: [(frame, parse_line dict)]."""
    return [(f, parse_line(f.payload)) for f in split_logs(stream, ids)[0]]
