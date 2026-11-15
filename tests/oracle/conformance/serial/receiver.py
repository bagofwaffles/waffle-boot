# SPDX-License-Identifier: GPL-2.0-or-later
"""A byte-at-a-time model of an S-06 receiver on one link: delivered, forwarded and dropped frames.

Rules per received byte, in order of precedence:

R1  Forwarding: if the deciding byte's low nibble is neither the node's address nor shares a bit with
    the reply mask, the encoded bytes up to and including the next 0x00 are collected unchanged as one
    forwarded frame; 256 bytes without a 0x00 drop it ('route-overflow'). ``route0='route-byte'``
    decides on the first decoded byte (S-06.047); ``'code-byte'`` on the second encoded byte. With
    ``early_delim='collect'`` a delimiter in second place counts as nibble 0 and starts a collection;
    ``'end'`` lets it end the frame (S-06.044).
R2  A 0x00 ends the frame; an incomplete one is dropped ('partial').
R3  COBS decoding group by group; with ``early_done=True`` (S-06.041) a frame completes when the decoded
    count reaches len + 5, without its delimiter. ``early_done=False`` completes at the delimiter and
    drops extra bytes before it ('length').
R4  Check byte equal to the CRC: delivered ('message'); otherwise dropped ('crc').
R5  254 decoded bytes without completion: dropped ('overflow').

``REQUIRED`` and ``FACTORY`` are the two switch sets; a 0xFF code also implies a zero (see R5).
"""
from dataclasses import dataclass

from .crc8 import crc8
from .frame import Frame, MAX_RAW

ROUTE_COLLECT_MAX = 256

REQUIRED = {'route0': 'route-byte', 'early_delim': 'end', 'early_done': True}
FACTORY = {'route0': 'code-byte', 'early_delim': 'collect', 'early_done': True}


@dataclass(frozen=True)
class Event:
    kind: str           # 'message', 'forward', 'crc', 'overflow', 'route-overflow', 'partial', 'length'
    frame: Frame = None     # for 'message'
    data: bytes = b''       # 'forward': the encoded bytes with the delimiter; others: decoded bytes so far
    dst: int = None         # 'forward': the destination nibble the decision was taken on

    def __repr__(self):
        if self.kind == 'message':
            return f'Event(message, {self.frame!r})'
        return f'Event({self.kind}, {self.data.hex()})'


class Receiver:
    def __init__(self, address, reply_mask=0x08, route0='route-byte', early_delim='end', early_done=True):
        if route0 not in ('route-byte', 'code-byte'):
            raise ValueError(f'route0 {route0!r}')
        if early_delim not in ('end', 'collect'):
            raise ValueError(f'early_delim {early_delim!r}')
        self.address = address & 0x0F
        self.reply_mask = reply_mask & 0x0F
        self.route0 = route0
        self.early_delim = early_delim
        self.early_done = bool(early_done)
        self.reset()

    @classmethod
    def factory(cls, address, reply_mask=0x08, **kw):
        """A receiver with the ``FACTORY`` switches."""
        opts = dict(FACTORY)
        opts.update(kw)
        return cls(address, reply_mask, **opts)

    @property
    def options(self):
        return {'route0': self.route0, 'early_delim': self.early_delim, 'early_done': self.early_done}

    def reset(self):
        self._code = None       # first code byte seen, or None at start of frame
        self._left = 0          # data bytes left in the current group
        self._raw = bytearray()     # decoded bytes
        self._fwd = None        # bytearray while collecting a frame for another node
        self._done = None       # early_done=False: the complete frame waiting for its delimiter
        self._extra = False

    def accepts(self, dst):
        """True if a frame with this destination nibble is for this node."""
        dst &= 0x0F
        return dst == self.address or (dst & self.reply_mask) != 0

    def feed(self, data):
        """Process received bytes; returns the list of Events they cause."""
        events = []
        for b in bytes(data):
            ev = self._byte(b)
            if ev is not None:
                events.append(ev)
        return events

    def _start_forward(self, b, dst):
        self._fwd = bytearray([self._code, b])
        self._fwd_dst = dst & 0x0F

    def _byte(self, b):
        if self._fwd is not None:                       # R1, collecting
            self._fwd.append(b)
            if b == 0:
                ev = Event('forward', data=bytes(self._fwd), dst=self._fwd_dst)
                self.reset()
                return ev
            if len(self._fwd) >= ROUTE_COLLECT_MAX:
                ev = Event('route-overflow', data=bytes(self._fwd))
                self.reset()
                return ev
            return None
        if self._code is not None and not self._raw and self._done is None:
            if b == 0 and self.early_delim == 'collect' and not self.accepts(0):
                self._start_forward(b, 0)               # early_delim='collect': a delimiter in second place
                return None
            if b != 0 and self.route0 == 'code-byte' and not self.accepts(b):
                self._start_forward(b, b)               # R1, the decision on the second encoded byte
                return None
        if b == 0:                                      # R2
            if self._done is not None:                  # early_done=False: the delimiter completes
                raw, extra = self._done, self._extra
                self.reset()
                if extra:
                    return Event('length', data=raw)
                return self._complete(raw)
            ev = Event('partial', data=bytes(self._raw)) if self._code is not None else None
            self.reset()
            return ev
        if self._code is None:                          # first code byte
            self._code = b
            self._left = b - 1
            return None
        if self._done is not None:                      # early_done=False: bytes after a complete frame
            self._extra = True
            return None
        if self._left:                                  # a data byte of the current group
            value = b
            self._left -= 1
        else:                                           # a code byte: the implied zero, a new group
            value = 0
            self._left = b - 1
        if self.route0 == 'route-byte' and not self._raw and not self.accepts(value):
            self._start_forward(b, value)               # R1, the decision on the route byte (S-06.047)
            return None
        self._raw.append(value)
        n = len(self._raw)
        if n >= 4 and n == self._raw[3] + 5:            # R3, R4
            raw = bytes(self._raw)
            if not self.early_done:
                self._done = raw
                return None
            self.reset()
            return self._complete(raw)
        if n >= MAX_RAW:                                # R5
            ev = Event('overflow', data=bytes(self._raw))
            self.reset()
            return ev
        return None

    @staticmethod
    def _complete(raw):
        if crc8(raw[:-1]) == raw[-1]:
            return Event('message', frame=Frame(raw[0], raw[1], raw[2], raw[4:-1]))
        return Event('crc', data=raw)
