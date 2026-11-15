# SPDX-License-Identifier: GPL-2.0-or-later
"""S-06 framing: src/s06_frame.c, s06_rx.c and s06_link.c (through h_s06) against serial's codec,
receiver and node model and tests/oracle/S-06-framing (vectors, tables); the CRC-8 is test glue (h_s06.c)."""

import json
import random
import unittest

from harness import requirement, run, scenario, spec_path, tsv
from conformance.serial import cobs, frame, receiver

from conformance.serial.link import Node

LINKS = ['midi', 'cdc', 'psoc', 'nordic', 'cpu']        # S06_LINK_* order
DROP = {'partial': 3, 'crc': 4, 'overflow': 5, 'route-overflow': 6, 'no-route': 16, 'no-room': 17}
RX_KIND = {'message': 'deliver', 'forward': 'forward', 'partial': 'partial', 'crc': 'crc',
           'overflow': 'long', 'route-overflow': 'fwdlong'}


def vectors(name):
    with open(spec_path('S-06-framing', 'vectors', name), encoding='utf-8') as f:
        return json.load(f)['vectors']


def h(b):
    return bytes(b).hex() or '-'


def link_table(core='master'):
    return {r['link']: (int(r['own']), int(r['peer'])) for r in tsv('S-06-framing', 'links.tsv')
            if r['core'] == core}


def node_setup(core='master', room=0):
    cmds = ['node %d%s' % (room, '' if core == 'master' else ' slave')]
    for name, (own, peer) in link_table(core).items():
        cmds.append('link %d %d %d' % (LINKS.index(name), own, peer))
    return cmds


def fmt_frame(f, slot=None):
    head = '' if slot is None else '%d:' % slot
    return '%s0x%x:%d:0x%x:%s' % (head, f.route, f.seq, f.id, h(f.payload))


def rx_model(own, data):
    out = []
    for ev in receiver.Receiver(own).feed(data):
        if ev.kind == 'message':
            out.append('deliver:' + ev.frame.raw().hex())
        else:
            out.append('%s:%s' % (RX_KIND[ev.kind], h(ev.data)))
    return out + ['end']


def random_payload(rng):
    return rng.randbytes(rng.choice([0, 1, 2, 5, rng.randrange(250), 249, 254 - 5]))


def random_frame_bytes(rng, dsts):
    """One encoded frame from a peer, sometimes broken: a bad check, cut short, a length field past 249,
    junk, a delimiter alone."""
    r = rng.random()
    if r < 0.04:
        return rng.randbytes(rng.randrange(1, 40)).replace(b'\0', b'\1') + b'\0'
    if r < 0.06:
        return b'\0'
    f = frame.Frame((rng.randrange(16) << 4) | rng.choice(dsts), rng.randrange(256), rng.randrange(256),
                    random_payload(rng)[:249])
    if r < 0.12:
        return f.encode(bad_crc=True)
    if r < 0.17:
        enc = f.encode()
        return enc[:rng.randrange(1, len(enc))] + b'\0'
    if r < 0.2:
        raw = bytearray(f.raw())
        raw[3] = rng.randrange(250, 256)
        raw += rng.randbytes(rng.randrange(260))
        return cobs.encode(bytes(raw)) + b'\0'
    if r < 0.23:                                  # a forward that never ends
        return bytes([0x05, 0x44]) + rng.randbytes(300).replace(b'\0', b'\7') + b'\0'
    return f.encode()


class Codec(unittest.TestCase):
    @requirement('S-06.003')
    def test_S06_003_crc_glue(self):
        """The harness's CRC-8 is S-06's (crc.json), so that the frames it checks are S-06 frames."""
        vs = vectors('crc.json')
        out = run('s06', ['crc %s' % h(bytes.fromhex(v['data'])) for v in vs])
        self.assertEqual([int(x, 0) for x in out], [int(v['crc'], 0) for v in vs])

    @scenario('S-06/FR-EVERY')
    def test_FR_EVERY_encode_vectors(self):
        vs = vectors('encode.json')
        cmds = []
        for v in vs:
            f = v['frame']
            args = '%s %d %s %s' % (f['route'], f['seq'], f['id'], h(bytes.fromhex(f['payload'])))
            cmds += ['raw ' + args, 'encode ' + args]
        out = run('s06', cmds)
        for i, v in enumerate(vs):
            self.assertEqual(out[2 * i], v['raw'], v['name'])
            self.assertEqual(out[2 * i + 1], v['wire'], v['name'])

    @scenario('S-06/FR-EVERY')
    def test_FR_EVERY_encode_random(self):
        rng = random.Random(6)
        frames = [frame.Frame(rng.randrange(256), rng.randrange(256), rng.randrange(256),
                              random_payload(rng)[:249]) for _ in range(800)]
        out = run('s06', ['encode 0x%x %d 0x%x %s' % (f.route, f.seq, f.id, h(f.payload)) for f in frames])
        for f, line in zip(frames, out):
            self.assertEqual(line, f.encode().hex())
            self.assertLessEqual(len(line) // 2, 256)             # S-06.005

    @requirement('S-06.004', 'S-06.006')
    def test_S06_004_cobs(self):
        rng = random.Random(7)
        data = [bytes(rng.choice([0, rng.randrange(256)]) for _ in range(rng.randrange(255)))
                for _ in range(500)]
        data += [bytes(254), b'\1' * 254, b'\1' * 253 + b'\0']
        out = run('s06', ['cobs %s' % h(d) for d in data])
        for d, line in zip(data, out):
            self.assertEqual(bytes.fromhex(line) if line != '-' else b'', cobs.encode(d), d.hex())


class Receive(unittest.TestCase):
    @scenario('S-06/FR-EVERY', 'S-06/FR-FWD-SEQ')
    def test_FR_EVERY_receive_vectors(self):
        vs = vectors('receive.json')
        out = run('s06', ['rx %d %s' % (v['address'], v['wire']) for v in vs])
        for v, line in zip(vs, out):
            wire = bytes.fromhex(v['wire'])
            got = line.split()
            self.assertEqual(got, rx_model(v['address'], wire), v['name'])
            delivered = [frame.parse_raw(bytes.fromhex(e.split(':')[1])) for e in got if e.startswith('deliver')]
            want = [frame.Frame(int(d['route'], 0), d['seq'], int(d['id'], 0), bytes.fromhex(d['payload']))
                    for k, d in (e for e in v['required'] if len(e) == 2) if k == 'deliver']
            self.assertEqual(delivered, want, v['name'])
            fwd = [e.split(':')[1] for e in got if e.startswith('forward')]
            self.assertEqual(fwd, [e[1] for e in v['required'] if e[0] == 'forward'], v['name'])
            self.assertEqual(len(got) - 1, len(v['required']), v['name'])

    @requirement('S-06.040', 'S-06.041', 'S-06.042', 'S-06.043', 'S-06.044', 'S-06.047', 'S-06.048')
    def test_S06_040_receive_random(self):
        rng = random.Random(61)
        cases = []
        for _ in range(400):
            own = rng.choice([1, 2, 3, 4, 5])
            dsts = [own, own, 8, 9, 11, 12, 15, 0, rng.randrange(16)]
            data = b''.join(random_frame_bytes(rng, dsts) for _ in range(rng.randrange(1, 6)))
            if rng.random() < 0.2:
                data = data[rng.randrange(len(data)):]           # joined mid-frame
            cases.append((own, data))
        out = run('s06', ['rx %d %s' % (own, h(d)) for own, d in cases])
        for (own, d), line in zip(cases, out):
            self.assertEqual(line.split(), rx_model(own, d), d.hex())

    @requirement('S-06.013')
    def test_S06_013_for_node(self):
        pairs = [(own, dst) for own in range(16) for dst in range(16)]
        out = run('s06', ['fornode %d %d' % p for p in pairs])
        for (own, dst), line in zip(pairs, out):
            self.assertEqual(bool(int(line)), receiver.Receiver(own).accepts(dst), (own, dst))


def parse_in(line):
    parts = line.split('|')
    drops = parts[0].split()[1:]
    msgs = parts[1].split()[1:]
    replies = sorted(parts[2].split()[1:])
    return drops, msgs, replies


def model_in(happened):
    drops, msgs, replies = [], [], []
    for kind, what in happened:
        if kind == 'message':
            msgs.append(fmt_frame(what))
        elif kind == 'reply':
            replies.append(fmt_frame(what[1], what[0]))
        elif kind in ('no-route', 'no-room'):
            drops.append('%d:%s' % (DROP[kind], h(what)))
        elif kind == 'forward':
            pass                                  # seen in the outgoing link's bytes
        else:
            drops.append('%d:%s' % (DROP[kind], h(what.data)))
    return drops, msgs, sorted(replies)


class Routing(unittest.TestCase):
    rows = tsv('S-06-framing', 'routing.tsv')

    def master_forward(self):
        return {(r['incoming link'], int(r['dst'])): r['verdict'].split(':')[1] for r in self.rows
                if r['core'] == 'master' and r['verdict'].startswith('forward')}

    @scenario('S-06/FR-FWD-SEQ', 'S-06/FR-FWD-BACK')
    def test_FR_FWD_SEQ_routes(self):
        got = run('s06', node_setup() + ['routes'])[-1].split()
        want = sorted('%d:%d:%d' % (LINKS.index(i), d, LINKS.index(o)) for (i, d), o in
                      self.master_forward().items())
        self.assertEqual(sorted(got), want)

    @scenario('S-06/FR-FWD-SEQ', 'S-06/FR-FWD-BACK')
    def test_FR_FWD_SEQ_verdicts(self):
        for core in ('master', 'slave'):
            links = link_table(core)
            rows = [r for r in self.rows if r['core'] == core]
            cmds = node_setup(core)
            frames = []
            for i, r in enumerate(rows):
                link = r['incoming link']
                own, peer = links[link]
                f = frame.Frame(frame.route(peer, int(r['dst'])), i, 0x7a, b'\x0a\x0b')
                frames.append(f)
                cmds.append('in %d %s' % (LINKS.index(link), f.encode().hex()))
                cmds += ['out %d' % LINKS.index(n) for n in links]
            out = run('s06', cmds)[len(node_setup(core)):]
            step = 1 + len(links)
            for i, (r, f) in enumerate(zip(rows, frames)):
                drops, msgs, replies = parse_in(out[i * step])
                outs = {n: out[i * step + 1 + k] for k, n in enumerate(links)}
                verdict = r['verdict']
                if verdict == 'local':
                    self.assertEqual((drops, msgs, replies), ([], [fmt_frame(f)], []), r)
                elif verdict == 'drop':
                    self.assertEqual((drops, msgs), (['16:' + f.encode().hex()], []), r)
                else:
                    self.assertEqual((drops, msgs), ([], []), r)
                    to = verdict.split(':')[1]
                    self.assertEqual(outs[to], f.encode().hex(), r)     # S-06.022: unchanged
                for n, o in outs.items():
                    if verdict != 'forward:' + n:
                        self.assertEqual(o, '-', (r, n))


class NodeModel(unittest.TestCase):
    """Random runs of the master node next to serial.link.Node, compared after every step: the bytes on
    every link, what is delivered, completed, dropped, and each call's result."""

    def run_model(self, seed, room, steps=400):
        rng = random.Random(seed)
        links = link_table()
        fwd = {(r['incoming link'], int(r['dst'])): r['verdict'].split(':')[1]
               for r in tsv('S-06-framing', 'routing.tsv')
               if r['core'] == 'master' and r['verdict'].startswith('forward')}
        m = Node(links, forward=fwd, room=room or None)
        cmds, checks = node_setup(room=room), []
        now = 0
        raw = {n: False for n in LINKS}

        def step(cmd, check):
            cmds.append(cmd)
            checks.append(check)
            for k, n in enumerate(LINKS):
                cmds.append('out %d' % k)
                checks.append(('out', n))

        for _ in range(steps):
            link = rng.choice(LINKS)
            k = LINKS.index(link)
            r = rng.random()
            if r < 0.2:
                src, dst = rng.choice([0, 0, 1, rng.randrange(16)]), rng.choice([0, 0, rng.randrange(16)])
                p = random_payload(rng)[:249]
                step('send %d 0x%x 0x%x %s' % (k, src << 4 | dst, 0x40, h(p)), ('send', link, 0x40, p, dst, src))
            elif r < 0.3:
                p = random_payload(rng)[:249]
                step('log %d 3 %s' % (k, h(p)), ('log', link, p))
            elif r < 0.45:
                p = random_payload(rng)[:40]
                t = rng.randrange(1, 5000)
                step('req %d 0x31 %s %d' % (k, h(p), t), ('req', link, 0x31, p, t))
            elif r < 0.52:
                now += rng.randrange(0, 3000)
                step('at %d' % now, ('at', now))
            elif r < 0.9:
                own, peer = links[link]
                dsts = [own, 8, 9, 10, 11, 12, 2, 3, 4, 5, 0]
                data = b''.join(random_frame_bytes(rng, dsts) for _ in range(rng.randrange(1, 4)))
                if raw[link]:
                    step('rawin %d %s' % (k, h(data)), ('rawin', link, data))
                else:
                    step('in %d %s' % (k, h(data)), ('in', link, data))
            elif r < 0.95:
                if raw[link]:
                    step('rawread %d' % k, ('rawread', link))
                else:
                    p = rng.randbytes(rng.randrange(1, 30))
                    step('rawwrite %d %s' % (k, h(p)), ('rawwrite', link, p))
            else:
                # Only real changes: Node.set_raw discards unread raw bytes even when the mode stays,
                # which S-06.060 asks only on entering or leaving.
                on = not raw[link]
                raw[link] = on
                step('rawmode %d %d' % (k, on), ('rawmode', link, on))
        return cmds, checks, Node(links, forward=fwd, room=room or None)

    def model_step(self, m, check, line):
        """Apply one step to the model and compare with the program's line."""
        kind = check[0]
        if kind == 'out':
            link = check[1]
            self.assertEqual(line, h(m.out[link]), check)
            m.out[link] = bytearray()
        elif kind == 'send':
            _, link, id, p, dst, src = check
            m.send(link, id, p, dst, src)
        elif kind == 'log':
            _, link, p = check
            self.assertEqual(int(line), m.log(link, p) is not None, check)
        elif kind == 'req':
            _, link, id, p, t = check
            slot = int(line)
            if slot < 0:
                self.assertTrue(all(m.pending[(link, s)] for s in range(4)), check)
            else:
                # S-06.030 leaves the free slot's choice open: the model takes the one the program took.
                self.assertFalse(m.pending[(link, slot)], check)
                m.next_slot = slot
                self.assertEqual(m.request(link, id, p, timeout=t, now=self.now), slot)
        elif kind == 'at':
            self.now = check[1]
            m.expire(self.now)
        elif kind == 'in':
            _, link, data = check
            self.assertEqual(parse_in(line), model_in(m.receive(link, data)), (check[1], data.hex()))
        elif kind == 'rawin':
            m.receive(check[1], check[2])
        elif kind == 'rawread':
            link = check[1]
            self.assertEqual(line, h(m.raw_in[link]), check)
            m.raw_in[link] = bytearray()
        elif kind == 'rawwrite':
            _, link, p = check
            fits = m.room is None or len(m.out[link]) + len(p) <= m.room
            self.assertEqual(int(line), fits, check)
            if fits:
                m.write_raw(link, p)
        elif kind == 'rawmode':
            m.set_raw(check[1], check[2])

    def check_run(self, seed, room):
        cmds, checks, m = self.run_model(seed, room)
        out = run('s06', cmds)[len(node_setup(room=room)):]
        self.now = 0
        for check, line in zip(checks, out):
            self.model_step(m, check, line)

    @scenario('S-06/FR-EVERY', 'S-06/FR-FWD-SEQ', 'S-06/FR-FWD-BACK', 'S-06/FR-REPLY-FORM',
              'S-06/FR-REPLY-SLOT', 'S-06/FR-REPLY-LINK', 'S-06/FR-REPLY-LATE', 'S-06/FR-RAW',
              'S-06/FR-RAW-RX', 'S-06/FR-RAW-FWD')
    def test_FR_EVERY_node(self):
        for seed in range(6):
            self.check_run(seed, 0)

    @scenario('S-06/FR-FULL')
    def test_FR_FULL_node(self):
        for seed in range(6):
            self.check_run(100 + seed, random.Random(seed).choice([40, 260, 600]))


if __name__ == '__main__':
    unittest.main()
