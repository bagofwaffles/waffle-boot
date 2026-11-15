# SPDX-License-Identifier: GPL-2.0-or-later
"""Stimulus helpers for panel-bus traces (format: panel.py): an identification-read responder, a TE edge
generator, a tap that turns PSoC-link message 0x51 into trace events, and a time-ordered merge of trace lines.
"""

# Read-strobe replies per command; the first strobe returns a dummy byte (ST7796S datasheet, 04h and D3h).
# The defaults are synthetic; a responder for a specific module passes its own table.
DEFAULT_ID_REPLIES = {0x04: bytes([0x00, 0x77, 0x96, 0x00]), 0xD3: bytes([0x00, 0x77, 0x96, 0x00])}


class IdResponder:
    """The panel side of an 8080 read: after a command byte, each RD rising edge with CS low returns the
    next byte of that command's reply; past the end, or for a command without a reply, it returns 0x00.
    CS high or a new command restarts the count."""

    def __init__(self, replies=None, fill=0x00):
        self.replies = dict(DEFAULT_ID_REPLIES if replies is None else replies)
        self.fill = fill
        self._cmd = None
        self._n = 0

    def command(self, byte):
        self._cmd, self._n = byte, 0

    def cs(self, level):
        if level:
            self._cmd, self._n = None, 0

    def read(self):
        reply = self.replies.get(self._cmd, b'')
        b = reply[self._n] if self._n < len(reply) else self.fill
        self._n += 1
        return b


def te_times(t0_ns, t1_ns, hz=60.0, phase_ns=0):
    """Rising-edge times of a periodic TE output in [t0_ns, t1_ns): the first at t0_ns + phase_ns."""
    period = 1e9 / hz
    out, k = [], 0
    while True:
        t = int(round(t0_ns + phase_ns + k * period))
        if t >= t1_ns:
            return out
        out.append(t)
        k += 1


def te_lines(t0_ns, t1_ns, hz=60.0, phase_ns=0):
    """The same edges as panel-bus trace lines (`T\\tTE\\t1`)."""
    return ['%d\tTE\t1' % t for t in te_times(t0_ns, t1_ns, hz, phase_ns)]


class Msg51Tap:
    """Hook for a PSoC-link decoder: call `message(t_ns, msg_id, payload)` for every decoded message the
    bootloader sends, timed when its first byte leaves. Messages with id 0x51 become `MSG51` lines with
    their payload bytes; others are ignored."""

    MSG_ID = 0x51

    def __init__(self):
        self.lines = []

    def message(self, t_ns, msg_id, payload):
        if msg_id == self.MSG_ID:
            self.lines.append('%d\tMSG51\t%s' % (t_ns, ' '.join('%02X' % b for b in payload)))


def merge_lines(*streams):
    """Trace lines from several sources in time order; lines with equal times keep their source order."""
    allx = [l for s in streams for l in s]
    return sorted(allx, key=lambda l: int(l.split(None, 1)[0]))
