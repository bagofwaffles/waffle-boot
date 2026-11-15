# SPDX-License-Identifier: GPL-2.0-or-later
"""S-06 model: receiving rules and node behavior, an adapter over tests/oracle/conformance/serial in the
event vocabulary the tests and gen_vectors.py use. ``factory`` is the package's compatibility preset.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'conformance'))
import serial as ts  # noqa: E402  (the conformance package, not pyserial)
from serial.crc8 import crc8  # noqa: E402,F401
from serial.frame import Frame, MAX_RAW  # noqa: E402,F401
from serial.link import REPLY_SLOT0, NUM_SLOTS  # noqa: E402,F401
from serial.receiver import ROUTE_COLLECT_MAX  # noqa: E402,F401


def is_local(dst, address):
    """S-06.013: a frame is for the node when its destination is the node's address or 8..15."""
    dst &= 0x0F
    return dst == address or dst >= 8


def _events(evs):
    """Package events -> ('deliver', Frame), ('forward', encoded bytes incl. delimiter), ('drop', reason)."""
    out = []
    for ev in evs:
        if ev.kind == 'message':
            out.append(('deliver', ev.frame))
        elif ev.kind == 'forward':
            out.append(('forward', ev.data))
        else:
            out.append(('drop', ev.kind))
    return out


class RequiredReceiver:
    """Bytes arriving on one framed link -> events (S-06.040..S-06.049): the package Receiver with its
    default (required) switches. Drop reasons are informative: 'crc', 'partial', 'overflow', 'route-overflow'."""

    def __init__(self, address):
        self.address = address & 0x0F
        self.rx = ts.Receiver(self.address)

    def reset(self):
        self.rx.reset()

    def feed(self, data):
        return _events(self.rx.feed(data))


def factory_events(wire, address, mask=0x08):
    """The package's compatibility preset (S-16), in this module's event vocabulary."""
    return _events(ts.Receiver.factory(address, mask).feed(wire))


def reply_slot(dst):
    """S-06.033, S-06.034: the request slot a delivered frame's destination names, or None."""
    return ts.reply_slot(dst)


# ---- the bootloader's links (S-06.011) and the master's forwarding table (S-06.050) ----

LINKS_MASTER = {'midi': (1, 2), 'cdc': (1, 2), 'psoc': (1, 4), 'nordic': (1, 3), 'cpu': (1, 5)}
LINKS_SLAVE = {'cpu': (5, 1)}
FORWARD_MASTER = {('midi', 5): 'cpu', ('cpu', 2): 'midi', ('cdc', 4): 'psoc', ('psoc', 2): 'cdc',
                  ('cdc', 3): 'nordic', ('nordic', 2): 'cdc'}


class Node(ts.Node):
    """One node (the package Node): links, forwarding table, sequence counters,
    request slots and link modes. ``out[link]`` collects the bytes the node writes on each link;
    ``inbox[link]`` the ordinary messages delivered; ``raw_in[link]`` the bytes received in raw mode;
    ``replies[(link, slot)]`` completed requests."""

    def __init__(self, links, forward=None, room=None):
        super().__init__(links, forward, room)


def forward_dst(encoded):
    """The destination nibble of a frame collected for forwarding: the low nibble of its route byte, the
    first decoded byte (S-06.047). A first code byte of 1 means the route byte is 0."""
    return 0 if encoded[0] == 1 else encoded[1] & 0x0F
