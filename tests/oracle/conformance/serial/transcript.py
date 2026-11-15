# SPDX-License-Identifier: GPL-2.0-or-later
"""Scripted responders: transcript machinery for the links to the coprocessors.

A script is an ordered list of steps:

    Expect(matcher, replies)   the device under test (DUT) must send an item the matcher accepts next;
                               the responder then sends the replies (items, or callables of the request)
    Inject(items)              the responder sends these unprompted when the script reaches them
                               (at the start, or right after the previous Expect matched)

    Raw(on)                    the link enters (on=True) or leaves raw mode when the script reaches it:
                               from then on the DUT's bytes are not framed (S-06.060..S-06.063)

Replies and injected items may be ``Frame``s, ``RawBytes`` (arbitrary bytes, sent as they are),
``OnLink(link, item)`` (sent on another link of a ``Peers`` group), ``Delayed(item, after)`` (sent
``after`` seconds later on the responder's clock) or callables of the request returning any of these.

Items are S-06 ``Frame``s on a UART-style link and ``BridgeRequest``/``BridgeReply`` records on the
SPI bridge. A DUT item that is not the next expected one is ignored (``ignore`` matcher) or recorded as
a failure; the log keeps everything in order for printing.
"""
import json

from . import frame as fr
from .cobs import split_stream
from .bridge import BridgeReply, BridgeRequest


# ---- matchers ----

class Match:
    """Accepts an item whose attributes equal the given values. Special keys: ``prefix`` (payload starts
    with), ``payload`` (payload equals), ``pred`` (a callable). A value may be a set/list/tuple of choices."""

    def __init__(self, **kw):
        self.pred = kw.pop('pred', None)
        self.prefix = kw.pop('prefix', None)
        if self.prefix is not None:
            self.prefix = bytes(self.prefix)
        if 'payload' in kw:
            kw['payload'] = bytes(kw['payload'])
        self.fields = kw

    def __call__(self, item):
        for k, want in self.fields.items():
            have = getattr(item, k, None)
            if isinstance(want, (set, frozenset, list, tuple)):
                if have not in want:
                    return False
            elif have != want:
                return False
        if self.prefix is not None and not getattr(item, 'payload', b'').startswith(self.prefix):
            return False
        return self.pred is None or bool(self.pred(item))

    def __repr__(self):
        parts = [f'{k}={_fmt(v)}' for k, v in self.fields.items()]
        if self.prefix is not None:
            parts.append(f'prefix={self.prefix.hex()}')
        if self.pred is not None:
            parts.append('pred=...')
        return 'Match(' + ', '.join(parts) + ')'


def _fmt(v):
    if isinstance(v, bytes):
        return v.hex()
    if isinstance(v, int):
        return f'{v:#04x}'
    if isinstance(v, (set, frozenset, list, tuple)):
        return '{' + ','.join(_fmt(x) for x in sorted(v)) + '}'
    return repr(v)


def any_of(*matchers):
    m = Match(pred=lambda item: any(x(item) for x in matchers))
    m.fields = {}
    return m


# ---- steps ----

class Expect:
    def __init__(self, matcher, replies=(), note=''):
        self.matcher = matcher
        self.replies = list(replies) if isinstance(replies, (list, tuple)) else [replies]
        self.note = note

    def __repr__(self):
        return f'Expect({self.matcher!r}{", " + self.note if self.note else ""})'


class Inject:
    def __init__(self, items, note=''):
        self.items = list(items) if isinstance(items, (list, tuple)) else [items]
        self.note = note

    def __repr__(self):
        return f'Inject({len(self.items)} items{", " + self.note if self.note else ""})'


class Raw:
    def __init__(self, on=True, note=''):
        self.on = bool(on)
        self.note = note

    def __repr__(self):
        return f'Raw({"on" if self.on else "off"}{", " + self.note if self.note else ""})'


class RawBytes(bytes):
    """Bytes sent or received as they are (raw mode, or deliberately malformed input)."""

    def encode(self):
        return bytes(self)

    def __repr__(self):
        return f'RawBytes({bytes(self).hex()})'


class OnLink:
    """An item to send on another link of a ``Peers`` group (S-06 FR-REPLY-LINK)."""

    def __init__(self, link, item):
        self.link = link
        self.item = item

    def __repr__(self):
        return f'OnLink({self.link}, {self.item!r})'


class Delayed:
    """An item sent ``after`` seconds later on the responder's clock (``feed(.., now=)``/``poll``)."""

    def __init__(self, item, after):
        self.item = item
        self.after = float(after)

    def __repr__(self):
        return f'Delayed({self.item!r}, {self.after})'


class _Mode:
    def __init__(self, on):
        self.on = on


def reply_to(id, payload=b'', seq=0, route=None, slot=None):
    """An S-06 reply builder: the request's route with the nibbles exchanged (unless ``route`` is
    given). ``slot`` (0..3) sends it to that reply slot instead of the request's (S-06 FR-REPLY-SLOT)."""
    def build(req):
        r = fr.swap_route(req.route) if route is None else route
        if slot is not None:
            r = (r & 0xF0) | (8 + slot)
        return fr.Frame(r, seq, id, payload)
    build.__name__ = f'reply_to({id:#04x})'
    return build


class TranscriptError(AssertionError):
    pass


class Script:
    """The engine: steps in order, an ignore matcher, the log and the failures."""

    def __init__(self, steps, ignore=None, name='link'):
        self.steps = list(steps)
        self.ignore = ignore
        self.name = name
        self.pos = 0
        self.log = []           # (direction 'dut'|'peer', item, note)
        self.failures = []

    def _injects(self):
        out = []
        while self.pos < len(self.steps) and isinstance(self.steps[self.pos], (Inject, Raw)):
            st = self.steps[self.pos]
            if isinstance(st, Raw):
                self.log.append(('peer', None, f'step {self.pos} {st!r}'))
                out.append(_Mode(st.on))
            else:
                for it in st.items:
                    self.log.append(('peer', it, f'step {self.pos} inject {st.note}'.strip()))
                    out.append(it)
            self.pos += 1
        return out

    def start(self):
        """Items to send before the DUT says anything (leading Inject steps)."""
        return self._injects()

    def on_item(self, item):
        """The DUT sent ``item``; returns the items to send back."""
        st = self.steps[self.pos] if self.pos < len(self.steps) else None
        if isinstance(st, Expect) and st.matcher(item):
            self.log.append(('dut', item, f'step {self.pos} {st.note}'.strip()))
            self.pos += 1
            out = []
            for r in st.replies:
                it = r(item) if callable(r) else r
                for x in (it if isinstance(it, list) else [it]):
                    if x is not None:
                        self.log.append(('peer', x, 'reply'))
                        out.append(x)
            return out + self._injects()
        if self.ignore is not None and self.ignore(item):
            self.log.append(('dut', item, 'ignored'))
            return []
        why = f'expected {st!r}' if st is not None else 'script finished'
        self.log.append(('dut', item, 'UNEXPECTED: ' + why))
        self.failures.append(f'{self.name}: unexpected {item!r}; {why}')
        return []

    def fail(self, msg):
        self.log.append(('dut', None, 'FAIL: ' + msg))
        self.failures.append(f'{self.name}: {msg}')

    @property
    def done(self):
        return self.pos >= len(self.steps)

    def check(self):
        """Raise TranscriptError unless every step was met and nothing unexpected arrived."""
        problems = list(self.failures)
        if not self.done:
            problems.append(f'{self.name}: stopped at step {self.pos}: {self.steps[self.pos]!r}')
        if problems:
            raise TranscriptError('\n'.join(problems) + '\n' + self.render())

    def render(self):
        lines = [f'-- transcript {self.name}']
        for d, it, note in self.log:
            lines.append(f'{"DUT ->" if d == "dut" else "   <-"} {it!r}  {note}')
        return '\n'.join(lines)


# ---- responders: bytes in, bytes out ----

class ForwardLedger:
    """The frames a runner put on another link of the DUT, which the DUT forwards byte for byte: a
    responder given this ledger leaves those copies out of its sequence check (S-06.021)."""

    def __init__(self):
        self._held = {}

    def put(self, data):
        """Record the encoded frames in ``data`` (delimiter-terminated) as expected forwards."""
        parts, _ = split_stream(bytes(data))
        for p in parts:
            if p:
                self._held[p] = self._held.get(p, 0) + 1

    def take(self, enc, frame=None):
        enc = bytes(enc)
        if enc.endswith(b'\x00'):
            enc = enc[:-1]
        if self._held.get(enc):
            self._held[enc] -= 1
            return True
        return False

    __call__ = take


class S06Responder:
    """A coprocessor on an S-06 link (UART or tunnel). ``feed`` takes the bytes the DUT sent and
    returns the bytes to send back.

    ``check_seq`` asserts link.py S1: each frame the DUT originates carries the previous one's seq + 1. Frames
    the ``forwarded`` predicate accepts (a ``ForwardLedger`` or a callable ``(encoded, frame) -> bool``)
    are byte copies the DUT forwarded: they take no number and are not offered to the script (they are
    kept in ``forwarded_frames``). In raw mode (``set_raw`` or a ``Raw`` step) the DUT's bytes are not
    framed; they go to ``raw_rx`` and to ``raw_handler(bytes) -> bytes or items`` if given (e.g. a Cypress
    bootloader model); a ``Raw(False)`` item among the handler's items leaves raw mode.
    ``stall()`` stops reading (bytes are held until ``resume()``). The clock for
    ``Delayed`` replies is ``now`` (``feed(.., now=)``, ``poll(now)``)."""

    def __init__(self, script, check_seq=False, forwarded=None, raw_handler=None, link=None):
        self.script = script
        self.check_seq = check_seq
        self.forwarded = forwarded
        self.raw_handler = raw_handler
        self.link = link
        self.raw = False
        self.raw_rx = bytearray()
        self.forwarded_frames = []
        self.cross = []         # (link, bytes) for other links (OnLink); a Peers group drains it
        self.now = 0.0
        self.stalled = False
        self._held = bytearray()
        self._later = []        # (due, n, item)
        self._n = 0
        self._buf = bytearray()
        self._last_seq = None

    def start(self):
        return self._emit(self.script.start())

    def set_raw(self, on):
        """Enter or leave raw mode; a partial frame received so far is discarded."""
        self.raw = bool(on)
        self._buf.clear()

    def stall(self):
        self.stalled = True

    def resume(self, now=None):
        self.stalled = False
        held, self._held = bytes(self._held), bytearray()
        return self.feed(held, now)

    def poll(self, now):
        """Advance the clock; returns the delayed items now due."""
        self.now = max(self.now, now)
        return self._due()

    def _due(self):
        due = sorted(x for x in self._later if x[0] <= self.now)
        self._later = [x for x in self._later if x[0] > self.now]
        return b''.join(self._emit([it]) for _, _, it in due)

    def _emit(self, items):
        out = bytearray()
        for it in items:
            if it is None:
                continue
            if isinstance(it, (_Mode, Raw)):
                self.raw = it.on        # the bytes after the switch follow the new mode
            elif isinstance(it, Delayed):
                self._n += 1
                self._later.append((self.now + it.after, self._n, it.item))
            elif isinstance(it, OnLink) and it.link != self.link:
                self.cross.append((it.link, self._emit([it.item])))
            elif isinstance(it, OnLink):
                out += self._emit([it.item])
            elif isinstance(it, (bytes, bytearray)):
                out += bytes(it)
            elif isinstance(it, list):
                out += self._emit(it)
            else:
                out += it.encode()
        return bytes(out)

    def feed(self, data, now=None):
        if now is not None:
            self.now = max(self.now, now)
        if self.stalled:
            self._held += data
            return self._due()
        out = bytearray(self._due())
        self._buf += data
        while self._buf:
            if self.raw:
                chunk = RawBytes(bytes(self._buf))
                self._buf.clear()
                self.raw_rx += chunk
                self.script.log.append(('dut', chunk, 'raw'))
                if self.raw_handler is not None:
                    r = self.raw_handler(bytes(chunk))
                    out += self._emit(r if isinstance(r, list) else [RawBytes(r or b'')])
                break
            i = self._buf.find(0)
            if i < 0:
                break
            enc = bytes(self._buf[:i])
            del self._buf[:i + 1]
            if enc:
                out += self._frame(enc)
        return bytes(out)

    def _frame(self, enc):
        try:
            f = fr.decode(enc)
        except fr.FrameError as e:
            self.script.fail(f'malformed frame from DUT ({e.reason}): {enc.hex()}')
            return b''
        if self.forwarded is not None and self.forwarded(enc, f):
            self.forwarded_frames.append(f)
            self.script.log.append(('dut', f, 'forwarded'))
            return b''
        if self.check_seq:
            if self._last_seq is not None and f.seq != (self._last_seq + 1) & 0xFF:
                self.script.fail(f'seq {f.seq} after {self._last_seq}')
            self._last_seq = f.seq
        return self._emit(self.script.on_item(f))


class Peers:
    """Several responders, one per link of the DUT, so a reply can go out on another link (``OnLink``).
    ``feed(link, data)`` and ``poll(now)`` return {link: bytes} for every link with something to send."""

    def __init__(self, responders):
        self.responders = dict(responders)
        for name, r in self.responders.items():
            r.link = name

    def _collect(self, first):
        out = {}
        for link, b in first:
            if b:
                out[link] = out.get(link, b'') + b
        for r in self.responders.values():
            for link, b in r.cross:
                if b:
                    out[link] = out.get(link, b'') + b
            r.cross.clear()
        return out

    def start(self):
        return self._collect([(k, r.start()) for k, r in self.responders.items()])

    def feed(self, link, data, now=None):
        return self._collect([(link, self.responders[link].feed(data, now))])

    def poll(self, now):
        return self._collect([(k, r.poll(now)) for k, r in self.responders.items()])

    def check(self):
        problems = []
        for r in self.responders.values():
            try:
                r.script.check()
            except TranscriptError as e:
                problems.append(str(e))
        if problems:
            raise TranscriptError('\n'.join(problems))


# ---- order checks across a whole stream (S-04.060..S-04.065) ----

def _items(stream):
    if isinstance(stream, Script):
        return [it for d, it, _ in stream.log if d == 'dut' and it is not None]
    if isinstance(stream, (bytes, bytearray)):
        return [f for f, _ in fr.decode_stream(bytes(stream))[0] if isinstance(f, fr.Frame)]
    return list(stream)


def first_index(stream, matcher, start=0):
    """Index of the first item at or after ``start`` that ``matcher`` accepts, or None. ``stream``: a list
    of items, encoded bytes (decoded with ``decode_stream``) or a Script (the DUT's items in its log)."""
    items = _items(stream)
    return next((i for i in range(start, len(items)) if matcher(items[i])), None)


def occurs_before(stream, first, then):
    """True if an item ``first`` accepts occurs, an item ``then`` accepts occurs, and the first of the
    former comes before the first of the latter."""
    items = _items(stream)
    a, b = first_index(items, first), first_index(items, then)
    return a is not None and b is not None and a < b


def in_order(stream, *matchers):
    """The indices of items matching ``matchers`` as a subsequence (each after the previous), or None."""
    items = _items(stream)
    idx, pos = [], 0
    for m in matchers:
        i = first_index(items, m, pos)
        if i is None:
            return None
        idx.append(i)
        pos = i + 1
    return idx


def assert_order(stream, *matchers, name='stream'):
    """Raise TranscriptError unless ``matchers`` match in this order (``in_order``); the message lists
    the stream with the matched positions marked."""
    items = _items(stream)
    if in_order(items, *matchers) is not None:
        return
    lines = [f'{name}: no subsequence matching ' + ' < '.join(map(repr, matchers))]
    for i, it in enumerate(items):
        marks = [str(k) for k, m in enumerate(matchers) if m(it)]
        lines.append(f'  {i:4} {it!r}' + (f'  matches {",".join(marks)}' if marks else ''))
    raise TranscriptError('\n'.join(lines))


class BridgeResponder:
    """The slave side of the SPI bridge at record level: ``feed_record`` takes one request record and
    returns the reply bytes. With ``tunnel`` (a S06Responder) push commands feed it and the pull
    command returns what it answered; ``push_cmds``/``pull_cmd`` name those commands."""

    def __init__(self, script, tunnel=None, push_cmds=(), pull_cmd=None):
        self.script = script
        self.tunnel = tunnel
        self.push_cmds = set(push_cmds)
        self.pull_cmd = pull_cmd
        self._tx = bytearray()

    def feed_record(self, rec):
        req = BridgeRequest.decode(rec)
        if self.tunnel is not None and req.cmd in self.push_cmds:
            self._tx += self.tunnel.feed(req.payload)
            self.script.log.append(('dut', req, 'tunnel push'))
            return b''
        if self.tunnel is not None and req.cmd == self.pull_cmd:
            n = min(len(self._tx), req.len_field)
            data = bytes(self._tx[:n])
            del self._tx[:n]
            rep = BridgeReply.pull(req, data)
            self.script.log.append(('dut', req, 'tunnel pull'))
            self.script.log.append(('peer', rep, f'tunnel {n} bytes'))
            return rep.encode()
        return b''.join(r.encode() for r in self.script.on_item(req) if not isinstance(r, _Mode))


# ---- JSON form of an S-06 script (for transcripts kept as data files) ----

def _num(v):
    return int(v, 16) if isinstance(v, str) else int(v)


def _matcher(d):
    kw = {}
    for k, v in d.items():
        if k in ('payload', 'prefix'):
            kw[k] = bytes.fromhex(v)
        elif isinstance(v, list):
            kw[k] = {_num(x) for x in v}
        else:
            kw[k] = _num(v)
    return Match(**kw)


def _frame(d, inject=False):
    if 'bytes' in d:
        return RawBytes(bytes.fromhex(d['bytes']))
    item = _frame1(d, inject)
    if 'delay' in d:
        return Delayed(item, d['delay'])
    return item


def _frame1(d, inject):
    payload = bytes.fromhex(d.get('payload', ''))
    seq = _num(d.get('seq', 0))
    if 'route' in d:
        return fr.Frame(_num(d['route']), seq, _num(d['id']), payload)
    if inject:
        raise ValueError('an injected frame needs a "route"')
    return reply_to(_num(d['id']), payload, seq, slot=d.get('slot'))


def load_s06_script(doc):
    """{"name", "ignore": [matcher...], "steps": [{"expect": matcher, "reply": [frame...], "note"} |
    {"inject": [frame...], "note"} | {"raw": true|false, "note"}]}. Numbers are ints or hex strings;
    payloads hex. A reply frame without "route" goes back on the request's route with the nibbles
    exchanged (to reply slot "slot" if given); {"bytes": hex} is sent as it is; "delay" (seconds)
    makes any item Delayed."""
    if isinstance(doc, (str, bytes)):
        doc = json.loads(doc)
    steps = []
    for s in doc['steps']:
        note = s.get('note', '')
        if 'raw' in s:
            steps.append(Raw(s['raw'], note))
        elif 'expect' in s:
            steps.append(Expect(_matcher(s['expect']), [_frame(r) for r in s.get('reply', [])], note))
        else:
            steps.append(Inject([_frame(r, inject=True) for r in s['inject']], note))
    ign = doc.get('ignore')
    ignore = any_of(*[_matcher(m) for m in ign]) if ign else None
    return Script(steps, ignore, doc.get('name', 'link'))
