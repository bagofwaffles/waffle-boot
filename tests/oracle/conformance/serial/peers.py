# SPDX-License-Identifier: GPL-2.0-or-later
"""Rule-based coprocessor peers: a model that answers by rules instead of a fixed script order.

A ``RulePeer`` stands where a ``Script`` does (``S06Responder(RulePeer(...))``). It holds no message
catalog: the ids and payloads come from the test.

Rule semantics: every rule whose matcher accepts a DUT item counts the match. A rule fires on its
matches number ``skip + 1`` .. ``skip + times`` (``times`` None: from then on, every time), and each
firing rule's replies are sent in rule order (wrapped in ``Delayed`` when the rule has a ``delay``).
Items no rule matches are logged; with ``strict=True`` they are failures.
"""
from .transcript import Delayed, TranscriptError


class Rule:
    def __init__(self, matcher, replies=(), delay=0.0, skip=0, times=None, note=''):
        self.matcher = matcher
        self.replies = list(replies) if isinstance(replies, (list, tuple)) else [replies]
        self.delay = float(delay)
        self.skip = int(skip)
        self.times = times
        self.note = note
        self.matched = 0
        self.fired = 0

    def __repr__(self):
        return f'Rule({self.matcher!r}{", " + self.note if self.note else ""})'


class RulePeer:
    def __init__(self, rules=(), start=(), ignore=None, strict=False, name='peer', require=()):
        self.rules = list(rules)
        self._start = list(start)
        self.ignore = ignore
        self.strict = strict
        self.name = name
        self.require = list(require)    # rules that must have fired at least once by check()
        self.log = []
        self.failures = []

    def on(self, matcher, replies=(), delay=0.0, skip=0, times=None, note='', required=False):
        """Add a rule; returns it (``rule.matched``, ``rule.fired`` count what happened)."""
        r = Rule(matcher, replies, delay, skip, times, note)
        self.rules.append(r)
        if required:
            self.require.append(r)
        return r

    def greet(self, items, poll=None, at_poll=0, repeat=1, delay=0.0, note='greeting'):
        """Send ``items`` unprompted: with ``poll`` None at the start; otherwise in answer to the DUT's
        matches of ``poll`` number ``at_poll + 1`` .. ``at_poll + repeat`` (``repeat`` None: every one)."""
        if poll is None:
            self._start += list(items) if isinstance(items, (list, tuple)) else [items]
            return None
        return self.on(poll, items, delay, at_poll, repeat, note)

    def start(self):
        for it in self._start:
            self.log.append(('peer', it, 'start'))
        return list(self._start)

    def on_item(self, item):
        out, hit = [], False
        for r in self.rules:
            if not r.matcher(item):
                continue
            hit = True
            r.matched += 1
            n = r.matched - r.skip
            if n < 1 or (r.times is not None and n > r.times):
                continue
            r.fired += 1
            for rep in r.replies:
                it = rep(item) if callable(rep) else rep
                for x in (it if isinstance(it, list) else [it]):
                    if x is None:
                        continue
                    self.log.append(('peer', x, f'{r!r} #{r.matched}' + (f' after {r.delay}s' if r.delay else '')))
                    out.append(Delayed(x, r.delay) if r.delay else x)
        if hit:
            self.log.insert(len(self.log) - len(out), ('dut', item, 'matched'))
        elif self.ignore is not None and self.ignore(item):
            self.log.append(('dut', item, 'ignored'))
        else:
            self.log.append(('dut', item, 'unmatched'))
            if self.strict:
                self.failures.append(f'{self.name}: unexpected {item!r}')
        return out

    def fail(self, msg):
        self.log.append(('dut', None, 'FAIL: ' + msg))
        self.failures.append(f'{self.name}: {msg}')

    @property
    def done(self):
        return all(r.fired for r in self.require)

    def check(self):
        problems = list(self.failures)
        problems += [f'{self.name}: {r!r} never fired' for r in self.require if not r.fired]
        if problems:
            raise TranscriptError('\n'.join(problems) + '\n' + self.render())

    def render(self):
        lines = [f'-- peer {self.name}']
        for d, it, note in self.log:
            lines.append(f'{"DUT ->" if d == "dut" else "   <-"} {it!r}  {note}')
        return '\n'.join(lines)
