# SPDX-License-Identifier: GPL-2.0-or-later
"""Addresses, sequence numbers, request slots and forwarding between links.

Model rules, referred to below as S1..S7:

S1  One sequence counter per link, mod 256, for every originated frame; forwarded frames keep their
    seq (S-06.021). A log line not sent takes no number (S-14.027); other unsent messages may leave a
    gap (S-06.020, S-06.062).
S2  A nibble of 0 means the default: peer address for the destination, own address for the source.
S3  A request goes out with source nibble 8 + n, n (0..3) a free reply slot.
S4  Payload at most 249 bytes.
S5  A delivered frame with destination 8..11 completes the pending request in slot dst - 8 on its link
    (S-06.032..S-06.034), else it is an ordinary message. ``slot12=True`` maps 12 to slot 0. A timed-out
    request frees its slot (S-06.035).
S6  A forwarded frame goes out unchanged on the registered link, whole or not at all (S-06.046); no
    link, or a link in raw mode: dropped.
S7  Raw mode (S-06.060..S-06.063): received bytes pass up uninterpreted, no frames are sent; entering
    or leaving discards a partial frame and unread raw bytes.
"""
from .frame import Frame, MAX_PAYLOAD, route
from .receiver import Receiver, FACTORY

REPLY_SLOT0 = 8
NUM_SLOTS = 4


def reply_slot(dst, slot12=False):
    """The request slot a delivered frame's destination completes, or None (S5). ``slot12=True``:
    destination 12 names slot 0."""
    if REPLY_SLOT0 <= dst < REPLY_SLOT0 + NUM_SLOTS:
        return dst - REPLY_SLOT0
    if slot12 and dst == REPLY_SLOT0 + NUM_SLOTS:
        return 0
    return None


class Sender:
    """The sending half of one link (S1..S4)."""

    def __init__(self, own, peer, seq=0):
        self.own = own & 0x0F
        self.peer = peer & 0x0F
        self.seq = seq & 0xFF

    def next_seq(self):
        s = self.seq
        self.seq = (self.seq + 1) & 0xFF
        return s

    def message(self, id, payload=b'', dst=0, src=0):
        payload = bytes(payload)
        if len(payload) > MAX_PAYLOAD:
            raise ValueError(f'payload {len(payload)} > {MAX_PAYLOAD}')
        return Frame(route(src or self.own, dst or self.peer), self.next_seq(), id, payload)

    def request(self, id, payload=b'', slot=0):
        if not 0 <= slot < NUM_SLOTS:
            raise ValueError('slot 0..3')
        return self.message(id, payload, src=REPLY_SLOT0 + slot)


class Node:
    """A node with several links (S1..S7): what it writes on each link, what it delivers, forwards, drops.

    ``links`` maps a link name to (own address, peer address). ``forward`` maps (incoming link,
    destination nibble) to an outgoing link name. ``room``: bytes each link's transmit side can still
    take (None: unlimited; the test drains ``out`` to make room). ``rx`` holds Receiver switches;
    ``factory=True`` selects the ``FACTORY`` switches and ``slot12`` together.

    State a test reads: ``out[link]`` bytes written; ``inbox[link]`` ordinary messages delivered;
    ``replies[(link, slot)]`` completed requests; ``pending[(link, slot)]``; ``raw_in[link]`` bytes
    received in raw mode; ``dropped`` what was dropped and why; ``seq[link]`` the next sequence number."""

    def __init__(self, links, forward=None, room=None, reply_mask=0x08, rx=None, slot12=None, factory=False):
        opts = dict(FACTORY) if factory else {}
        opts.update(rx or {})
        self.slot12 = factory if slot12 is None else bool(slot12)
        self.room = room
        self.links = dict(links)
        self.forward = dict(forward or {})
        self.rx = {k: Receiver(own, reply_mask, **opts) for k, (own, _) in self.links.items()}
        self.seq = {k: 0 for k in self.links}
        self.raw_mode = {k: False for k in self.links}
        self.pending = {(k, s): False for k in self.links for s in range(NUM_SLOTS)}
        self.deadline = {}
        self.replies = {}
        self.next_slot = 0
        self.out = {k: bytearray() for k in self.links}
        self.inbox = {k: [] for k in self.links}
        self.raw_in = {k: bytearray() for k in self.links}
        self.dropped = []

    def set_forward(self, link, dst, out_link):
        self.forward[(link, dst & 0x0F)] = out_link

    # ---- sending (S1..S4) ----

    def _fits(self, link, n):
        return self.room is None or len(self.out[link]) + n <= self.room

    def _originate(self, link, id, payload, dst, src):
        payload = bytes(payload)
        if len(payload) > MAX_PAYLOAD:
            raise ValueError(f'payload {len(payload)} > {MAX_PAYLOAD}')
        own, peer = self.links[link]
        f = Frame(route(src or own, dst or peer), self.seq[link], id, payload)
        self.seq[link] = (self.seq[link] + 1) & 0xFF
        enc = f.encode()
        if not self.raw_mode[link] and self._fits(link, len(enc)):
            self.out[link] += enc           # otherwise not sent; the number stays used (S-06.020)
        return f

    def send(self, link, id, payload=b'', dst=0, src=0):
        """Originate a message; returns the Frame (whether or not the link could take it)."""
        return self._originate(link, id, payload, dst, src)

    def log(self, link, payload, id=0x03):
        """A log line (S-14.027): dropped without a number in raw mode or without room. Returns the
        Frame sent, or None."""
        own, peer = self.links[link]
        f = Frame(route(own, peer), self.seq[link], id, bytes(payload))
        if self.raw_mode[link] or not self._fits(link, len(f.encode())):
            return None
        self.seq[link] = (self.seq[link] + 1) & 0xFF
        self.out[link] += f.encode()
        return f

    def request(self, link, id, payload=b'', timeout=None, now=0.0):
        """Send a request in the next free slot (round robin); returns the slot, or None if all four are
        pending. With ``timeout`` the request expires at ``now + timeout`` (see ``expire``)."""
        for i in range(NUM_SLOTS):
            slot = (self.next_slot + i) % NUM_SLOTS
            if not self.pending[(link, slot)]:
                self.next_slot = slot + 1
                self.pending[(link, slot)] = True
                self.replies.pop((link, slot), None)
                if timeout is not None:
                    self.deadline[(link, slot)] = now + timeout
                self._originate(link, id, payload, 0, REPLY_SLOT0 + slot)
                return slot
        return None

    def timeout(self, link, slot):
        """Time out a request, freeing its slot (S-06.035)."""
        self.pending[(link, slot)] = False
        self.deadline.pop((link, slot), None)

    def expire(self, now):
        """Time out every pending request whose deadline is at or before ``now``; returns their keys."""
        gone = [k for k, t in self.deadline.items() if t <= now and self.pending[k]]
        for link, slot in gone:
            self.timeout(link, slot)
        return gone

    # ---- receiving (receiver.py, S5, S6, S7) ----

    def receive(self, link, data):
        """Bytes arriving on ``link``. Returns what happened, in order: ('message', Frame),
        ('reply', (slot, Frame)), ('forward', (out_link, bytes)), ('no-route', bytes), ('no-room', bytes),
        or (drop kind, Event)."""
        if self.raw_mode[link]:
            self.raw_in[link] += data
            return [('raw', bytes(data))]
        happened = []
        for ev in self.rx[link].feed(data):
            if ev.kind == 'message':
                f = ev.frame
                slot = reply_slot(f.dst, self.slot12)
                if slot is not None and self.pending[(link, slot)]:
                    self.pending[(link, slot)] = False
                    self.deadline.pop((link, slot), None)
                    self.replies[(link, slot)] = f
                    happened.append(('reply', (slot, f)))
                else:
                    self.inbox[link].append(f)
                    happened.append(('message', f))
            elif ev.kind == 'forward':
                to = self.forward.get((link, ev.dst))
                if to is None or self.raw_mode[to]:
                    self.dropped.append(('no-route', link, ev.data))
                    happened.append(('no-route', ev.data))
                elif not self._fits(to, len(ev.data)):
                    self.dropped.append(('no-room', link, ev.data))     # S-06.046: whole or not at all
                    happened.append(('no-room', ev.data))
                else:
                    self.out[to] += ev.data
                    happened.append(('forward', (to, ev.data)))
            else:
                self.dropped.append((ev.kind, link))
                happened.append((ev.kind, ev))
        return happened

    def set_raw(self, link, on):
        """Enter or leave raw mode, discarding a partial frame and unread raw bytes (S-06.060)."""
        self.raw_mode[link] = bool(on)
        self.rx[link].reset()
        self.raw_in[link] = bytearray()

    def write_raw(self, link, data):
        """Write bytes unchanged in raw mode (S-06.063)."""
        self.out[link] += bytes(data)
