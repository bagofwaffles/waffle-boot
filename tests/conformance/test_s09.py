# SPDX-License-Identifier: GPL-2.0-or-later
"""S-09 bridge: src/s09_master.c and s09_slave.c wired together (through h_s09), each session checked
by tests/oracle/S-09-bridge/bridge_check.py and its vectors; the slave's store is test glue."""

import json
import random
import struct
import unittest

from harness import expected_failure, hx, requirement, run, scenario, spec_module, spec_path, unhex

bc = spec_module('S-09-bridge', 'bridge_check')
bm = spec_module('S-09-bridge', 'bridge_model')

POLL, PULL, READ = 0x06, 0x20, 0x2A
PATH = b'x\x00'


def vectors(name):
    with open(spec_path('S-09-bridge', 'vectors', name), encoding='utf-8') as f:
        return json.load(f)


REQ = {it['name']: it for it in vectors('requests.json')}
REP = {it['name']: it for it in vectors('replies.json')}


def parse_sessions(line):
    """[(session, rx)]: session as bridge_check takes it, rx what the slave took of a request. The
    session's 'slave_len' is the bytes the slave moved in it before its ready line rose (S-09.021), 'rung'
    the master's rung for it (S-09.075), and 'late' marks one whose end ran out (S-09.013)."""
    out = []
    for f in line.split():
        mosi, miso, rx, sess, rung, late = f.split(':')
        s = {'mosi': unhex(mosi), 'miso': unhex(miso), 'slave_len': int(sess), 'rung': int(rung)}
        if late == '1':
            s['late'] = True
        out.append((s, int(rx)))
    return out


def header(b):
    w0, a0, a1 = struct.unpack_from('<III', b)
    return {'cmd': w0 & 0xFF, 'slot': w0 >> 8 & 0xFF, 'len': w0 >> 16, 'arg0': a0, 'arg1': a1}


def call(cmd, length=0, arg0=0, arg1=0, payload=b'', max_=0x10000):
    p = payload if isinstance(payload, str) else hx(payload)
    return 'call %d %d %d %d %s %d' % (cmd, length, arg0 & 0xFFFFFFFF, arg1 & 0xFFFFFFFF, p, max_)


def store(cmd, result=0, err=0, data=b''):
    return 'store %d %d %d %s' % (cmd, result, err, hx(data))


def run_rig(cmds):
    """Runs rig, start, cmds, sessions; returns (outputs of cmds, [(session, rx)])."""
    out = run('s09', ['rig'] + cmds + ['sessions'])
    return out[1:-1], parse_sessions(out[-1])


def path_payload(cmd):
    """PATH for the path commands of S-09.023, else none."""
    return PATH if cmd in bm.PATH_CMDS else b''


def replies_to(sessions, cmd):
    """The reply sessions that follow request sessions of cmd."""
    return [sessions[i + 1][0]['miso'] for i, (s, rx) in enumerate(sessions[:-1])
            if rx and header(s['mosi'])['cmd'] == cmd]


def requests(sessions):
    return [(s['mosi'], rx) for s, rx in sessions if rx]


def same_but_slot(got, want_hex, slot):
    """Reply bytes against a vector whose slot byte (byte 1) is illustrative (S-09.022 leaves the slot
    to the master): the slot must be the request's."""
    want = bytearray.fromhex(want_hex)
    want[1] = slot
    return bytes(got) == bytes(want)


class Start(unittest.TestCase):
    """S-09.015: init, echo, then the first pull, parked with nothing to carry."""

    def setUp(self):
        out, self.s = run_rig(['start'])
        self.assertEqual(out, ['1'])

    @scenario('S-09/M-START')
    def test_M_START(self):
        s = [x for x, _ in self.s]
        self.assertEqual(len(s), 6)
        self.assertEqual(bc.check_master(s), [])
        self.assertEqual(bc.check_requests([{'name': 'init', 'hex': s[0]['mosi']},
                                            {'name': 'echo', 'hex': s[2]['mosi']},
                                            {'name': 'pull', 'hex': s[4]['mosi']}]), [])
        self.assertEqual(bc.check_replies([{'name': 'imm_init', 'hex': s[1]['miso']},
                                           {'name': 'imm_echo', 'hex': s[3]['miso']},
                                           {'name': 'pull_none', 'hex': s[5]['miso']}]), [])

    @scenario('S-09/S-START')
    def test_S_START(self):
        rx = [{'name': n, 'rx_len': self.s[i][1], 'session_len': self.s[i][0]['slave_len']}
              for n, i in (('init', 0), ('echo', 2), ('pull', 4))]
        self.assertEqual(bc.check_requests(rx, side='slave'), [])
        self.assertEqual(bc.check_replies([{'name': 'imm_init', 'hex': self.s[1][0]['miso']},
                                           {'name': 'imm_echo', 'hex': self.s[3][0]['miso']}]), [])


def request_call(it):
    """The master call that sends vector item it: its header's fields and its payload."""
    hdr = bytes.fromhex(it.get('session_hex') or it['session_head_hex'])[:12]
    h = header(hdr)
    n = bm.slave_payload_len(hdr)
    if 'session_hex' in it:
        payload = hx(bytes.fromhex(it['session_hex'])[12:12 + n])
    else:
        payload = 'r7:%d' % n                    # data_rule (i*7)&0xff
    return call(h['cmd'], h['len'], h['arg0'], h['arg1'], payload)


SENT_BY_CALL = [n for n in REQ if n not in ('init', 'echo', 'pull', 'poll', 'release', 'push')]


class Requests(unittest.TestCase):
    """S-09.020-S-09.023: each request of requests.json sent through s09_master_call()."""

    def runs(self):
        for name in SENT_BY_CALL:
            out, s = run_rig(['start', request_call(REQ[name])])
            yield name, out, s

    @scenario('S-09/M-SESSION')
    @requirement('S-09.021', 'S-09.022')
    def test_M_SESSION(self):
        polls = []
        for name, out, s in self.runs():
            with self.subTest(name=name):
                self.assertEqual(bc.check_master([x for x, _ in s]), [])
                mosi = requests(s)[3][0]             # after init, echo and the pull
                self.assertEqual(bc.check_requests([{'name': name, 'hex': mosi}]), [])
                self.assertEqual(int(out[1].split()[0]), 0)
                polls += [m for m, _ in requests(s)[4:] if header(m)['cmd'] == POLL]
        self.assertTrue(polls)
        self.assertEqual(bc.check_requests([{'name': 'poll', 'hex': m} for m in polls]), [])

    @scenario('S-09/S-SESSION')
    def test_S_SESSION(self):
        for name, _, s in self.runs():
            with self.subTest(name=name):
                sess, rx = [x for x in s if x[1]][3]     # after init, echo and the pull
                self.assertEqual(bc.check_requests([{'name': name, 'rx_len': rx,
                                                     'session_len': sess['slave_len']}], side='slave'), [])


class Replies(unittest.TestCase):
    """S-09.030-S-09.032: the slave's replies of replies.json, collected by the master."""

    @scenario('S-09/S-CMD', 'S-09/M-DEFER')
    def test_S_CMD_immediate(self):
        for name, cmd in (('imm_erase_group', 0x03), ('imm_boot_size', 0x05)):
            e = REP[name]
            out, s = run_rig(['start', store(cmd, e['value']), call(cmd)])
            with self.subTest(name=name):
                self.assertEqual(out[2].split()[:2], [str(e['value']), '0'])
                self.assertEqual(bc.check_replies([{'name': name, 'hex': s[-1][0]['miso']}]), [])
                self.assertEqual(bc.check_master([x for x, _ in s]), [])

    @scenario('S-09/S-CMD', 'S-09/M-DEFER')
    @requirement('S-09.031', 'S-09.032')
    def test_S_CMD_deferred(self):
        names = [n for n, e in REP.items() if e['kind'] == 'deferred']
        self.assertTrue(names)
        for name in names:
            e = REP[name]
            data = bytes.fromhex(e['reply_hex'])
            self.assertEqual(len(data), e['reply_len'])
            data = data[12:]
            cmd = e['cmd']
            payload = path_payload(cmd)
            out, s = run_rig(['start', store(cmd, e['result'], e['errno'], data),
                              call(cmd, len(payload), 0, e.get('arg1', 0), payload)])
            with self.subTest(name=name):
                self.assertEqual(bc.check_master([x for x, _ in s]), [])
                result, err, got = out[2].split()
                self.assertEqual((int(result), int(err)), (e['result'], e['errno']))
                self.assertEqual(unhex(got), data[:len(unhex(got))])
                self.assertEqual(len(unhex(got)), bm.deferred_reply_len(cmd, e.get('arg1', 0)) - 12)
                slot = header(requests(s)[3][0])['slot']
                (reply,) = replies_to(s, POLL)
                self.assertTrue(same_but_slot(reply, e['reply_hex'], slot), reply[:16].hex())


class Malformed(unittest.TestCase):
    """S-09.037: malformed requests of defined commands (malformed.json), handed to the slave directly, as
    the bootloader's master sends none of them."""

    @scenario('S-09/S-CMD')
    @requirement('S-09.037')
    def test_S_CMD_malformed(self):
        items = vectors('malformed.json')
        self.assertTrue(items)
        # a long item (S-09.037: arg1 x 512 payload bytes, discarded) is handed over as its head: the slave
        # keeps the header only and reports taking the whole request
        heads = [it.get('request_hex', it.get('request_head_hex')) for it in items]
        cmds = ['start'] + ['inject %s 2' % h for h in heads]   # slot 0 the pull, 1 the request
        cmds += [store(0x03, 1024), call(0x03)]
        out, s = run_rig(cmds)
        for it, head, o in zip(items, heads, out[1:]):
            rx, ops, path, reply = o.split()
            req = bytes.fromhex(head)
            h = header(req)
            with self.subTest(name=it['name']):
                self.assertEqual(int(rx), it['slave_rx_len'])
                if it['reply_len'] == 0:                            # well formed: served as usual
                    name = req[12:12 + h['len']].split(b'\0')[0]     # ends at its first NUL; under /yaffs2/
                    self.assertEqual((int(ops), unhex(path)), (1, b'/yaffs2/' + name))   # (S-16.117)
                    self.assertEqual(header(unhex(reply))['cmd'], h['cmd'])
                    self.assertEqual(struct.unpack_from('<i', unhex(reply), 4)[0], 0)
                    continue
                self.assertEqual(int(ops), 0)                       # no eMMC or file operation
                self.assertEqual(len(unhex(reply)), it['reply_len'])
                self.assertTrue(same_but_slot(unhex(reply), it['reply_hex'], h['slot']), reply[:32])
        self.assertEqual(out[-1].split()[:2], ['1024', '0'])          # a defined request is served after


class Release(unittest.TestCase):
    """S-09.050-S-09.052 with the slave's scripted value."""

    def release(self, value, after=()):
        return run_rig(['start', store(0x07, value), 'release'] + list(after))

    @scenario('S-09/M-RELEASE', 'S-09/S-RELEASE')
    def test_M_RELEASE_reply_0(self):
        out, s = self.release(0, ['stake', 'stake'])
        self.assertEqual(out[2], '0')
        # S-09.052: the slave's hook gives the value once, after the reply session
        self.assertEqual(out[3:5], ['1 0', '0'])
        self.assertEqual(bc.check_master([x for x, _ in s]), [])
        self.assertEqual(bc.check_requests([{'name': 'release', 'hex': s[-2][0]['mosi']}]), [])
        self.assertEqual(bc.check_replies([{'name': 'imm_release_ok', 'hex': s[-1][0]['miso']}]), [])

    @scenario('S-09/M-RELEASE', 'S-09/S-RELEASE')
    def test_M_RELEASE_reply_1(self):
        e = REP['poll_access_enoent']
        out, s = self.release(1, ['stake', store(0x21, -1, 2), call(0x21, 2, 0, 0, PATH), 'stake'])
        self.assertEqual(out[2], '1')
        self.assertEqual(out[3], '1 1')                       # the slave's hook: keep serving
        self.assertEqual(out[5].split()[:2], ['-1', '2'])     # a later storage operation is served
        self.assertEqual(out[6], '0')
        self.assertEqual(bc.check_master([x for x, _ in s]), [])
        (reply,) = replies_to(s, 0x07)
        self.assertEqual(bc.check_replies([{'name': 'imm_release_noos', 'hex': reply}]), [])
        (reply,) = replies_to(s, POLL)
        self.assertTrue(same_but_slot(reply, e['reply_hex'], 1))

    @scenario('S-09/M-RELEASE')
    def test_M_RELEASE_reply_7(self):
        out, s = self.release(7, [call(0x21, 2, 0, 0, PATH)])
        self.assertEqual(out[2], '-1')
        self.assertEqual(out[3].split()[:2], ['-1', '19'])    # absent: ENODEV, no session
        self.assertEqual(header(s[-2][0]['mosi'])['cmd'], 0x07)
        self.assertEqual(bc.check_master([x for x, _ in s]), [])


def file_bytes(c):
    rnd = random.Random(c['file_seed'])
    return bytes(rnd.randrange(256) for _ in range(c['file_len']))


class FileRead(unittest.TestCase):
    """S-09.035: chunked reads, each case of file_read.json."""

    def read(self, f, pos, n, fail):
        out, s = run_rig(['start', 'file %s %d %d' % (hx(f), pos, -1 if fail is None else fail),
                          'read 3 %d' % n])
        return out[2].split(), s

    @scenario('S-09/M-READ', 'S-09/S-READ')
    def test_M_READ(self):
        for c in vectors('file_read.json'):
            f = file_bytes(c)
            (result, err, data), s = self.read(f, c['pos'], c['n'], c['fail_piece'])
            with self.subTest(name=c['name']):
                self.assertEqual(bc.check_master([x for x, _ in s]), [])
                r = header(requests(s)[3][0])
                self.assertEqual((r['cmd'], r['arg1'], r['len']), (READ, c['n'], c['chunk']))
                pieces = replies_to(s, POLL)
                self.assertEqual(bc.check_file_reads([{'name': c['name'], 'pieces': pieces}]), [])
                self.assertEqual([int(result), int(err)], c['result'])
                if c['result'][0] >= 0:
                    self.assertEqual(unhex(data), f[c['pos']:c['pos'] + c['result'][0]])

    @requirement('S-09.035')
    def test_S09_035_read_pieces(self):
        for name, f, n, fail in (('read_piece_5', bytes(range(5)), 5, None),
                                 ('read_piece_eof', b'', 1, None),
                                 ('read_piece_err', b'', 1, 0)):
            e = REP[name]
            _, s = self.read(f, 0, n, fail)
            with self.subTest(name=name):
                (piece,) = replies_to(s, POLL)
                self.assertTrue(same_but_slot(piece, e['reply_hex'], header(requests(s)[3][0])['slot']))


class Tunnel(unittest.TestCase):
    """S-09.034, S-09.045: the pull carrying channel 0's bytes."""

    @scenario('S-09/M-TUNNEL')
    @expected_failure('S-09/M-TUNNEL', 'when start-up step 5\'s pull completes at once with data, no next pull '
                      'is sent before the next request, though the master has room (S-09.045); send_pull() '
                      're-pulls only after a deferred call or in s09_master_service()')
    def test_M_TUNNEL_pull_now(self):
        e = REP['pull_now']
        out, s = run_rig(['tunnel ' + e['data_hex'], 'start', store(0x03, 1024), call(0x03)])
        self.assertEqual(out[1], '1')
        self.assertEqual(bc.check_master([x for x, _ in s]), [])
        self.assertEqual(bc.check_replies([{'name': 'pull_now', 'hex': s[5][0]['miso']}]), [])

    @scenario('S-09/M-TUNNEL')
    def test_M_TUNNEL_pull_later(self):
        e = REP['pull_later']
        out, s = run_rig(['start', 'tunnel ' + e['data_hex'], 'service', store(0x03, 1024), call(0x03)])
        self.assertEqual(bc.check_master([x for x, _ in s]), [])
        (reply,) = replies_to(s, POLL)
        self.assertEqual(bc.check_replies([{'name': 'pull_later', 'hex': reply}]), [])


class Clock(unittest.TestCase):
    """S-09.036: the slave's side of the clock sync, the scripted master's sessions handed to it."""

    @scenario('S-09/S-CLOCK')
    @requirement('S-09.036')
    def test_S_CLOCK(self):
        """The first exchange's reply is clock_sync's: [hdr][0] and the sample, the cycle count when the
        slave took the header (the counter moves on before the count's session); a count of 0: no
        reply session."""
        e = REP['clock_sync']
        hdr = struct.pack('<III', 0x0B | e['slot'] << 8, 0, 0).hex()
        out, _ = run_rig(['start', 'sample %d' % e['sample'], 'sreq ' + hdr, 'sample %d' % (e['sample'] + 999),
                          'sreq ' + struct.pack('<Q', 0x123456789A + e['sample']).hex(),
                          'sreq ' + hdr, 'sreq ' + bytes(8).hex()])
        self.assertEqual(out[2], '12 -')                       # the header alone, no reply yet
        rx, reply = out[4].split()
        self.assertEqual(rx, '8')
        self.assertEqual(bc.check_replies([{'name': 'clock_sync', 'hex': unhex(reply)}]), [])
        self.assertEqual(out[5:7], ['12 -', '8 -'])            # a count of 0: no reply session


def frames(line):
    """mtake's frames: [(id, route, payload)]."""
    out = []
    for f in line.split()[:-1]:
        i, r, p = f.split(':')
        out.append((int(i, 16), int(r, 16), unhex(p)))
    return out


SLAVE = 5


class CpuLink(unittest.TestCase):
    """S-09.033, S-09.034, S-09.045-S-09.047: S-06 nodes on both ends of channel 0, the master's pushes
    and the slave's answers through the parked pull. 'cpu 1' lets the slave handle its CPU link after
    each request session it is handed, as a slave running alongside would."""

    def master_ok(self, s):
        self.assertEqual(bc.check_master([x for x, _ in s]), [])

    @scenario('S-09/S-TUNNEL', 'S-09/M-TUNNEL')
    def test_S_TUNNEL_ping(self):
        out, s = run_rig(['cpu 1', 'start', 'msend 1 %d aa21' % SLAVE, 'service', 'mtake'])
        self.assertEqual(frames(out[4]), [(0x02, SLAVE << 4 | 1, b'\xaa\x21')])
        self.master_ok(s)
        pushes = [m for m, rx in requests(s) if header(m)['cmd'] == 0x1E]
        self.assertTrue(pushes)
        self.assertTrue(all(header(m)['len'] <= 0x1000 for m in pushes))

    @scenario('S-09/M-TUNNEL')
    def test_M_TUNNEL_frames_in_order(self):
        """Pass 2: the frames arrive at the slave whole and in order."""
        sent = [bytes(range(i, i + 1 + i % 40)) for i in range(40)]
        out, s = run_rig(['cpu 1', 'start'] + ['msend 64 %d %s' % (SLAVE, hx(p)) for p in sent] +
                         ['service', 'scpu'])
        self.assertEqual(out[2:42], ['1'] * 40)
        got = out[-1].split()[2].split(',')
        self.assertEqual(got, ['40:' + hx(p) for p in sent])
        self.master_ok(s)

    @scenario('S-09/M-TUNNEL')
    @requirement('S-09.046')
    def test_M_TUNNEL_clock(self):
        """Pass 3: 0xBD with the clock's 4 bytes, once per change."""
        out, s = run_rig(['cpu 1', 'start', 'mclock 1000', 'mclock 1000', 'mclock 1001', 'service',
                          'mclock 4000000000', 'service', 'scpu'])
        self.assertEqual(out[-1].split()[1], '1000,1001,4000000000')
        self.master_ok(s)

    @scenario('S-09/S-TUNNEL', 'S-09/M-TUNNEL')
    @requirement('S-09.047')
    def test_S_TUNNEL_power_off(self):
        out, s = run_rig(['cpu 1', 'start', 'poweroff', 'scpu'])
        self.assertEqual(out[2].split()[0], '1')                  # the 0x32 came
        self.assertEqual(out[3].split()[0], '1')                  # S-04.025, after the 0x32
        self.master_ok(s)

    @scenario('S-09/M-TUNNEL')
    def test_M_TUNNEL_power_silent(self):
        out, s = run_rig(['cpu 0', 'start', 'poweroff', 'scpu'])
        ok, elapsed = (int(x) for x in out[2].split())
        self.assertEqual(ok, 0)
        self.assertLessEqual(elapsed, T_POWER + 100000)
        self.assertTrue(1000000 <= T_POWER <= 5000000)
        self.master_ok(s)

    @scenario('S-09/S-TUNNEL')
    @requirement('S-16.009')
    def test_S_TUNNEL_time_len3(self):
        out, s = run_rig(['cpu 1', 'start', 'msend 189 %d 112233' % SLAVE, 'msend 1 %d aa21' % SLAVE,
                          'service', 'mtake', 'scpu'])
        self.assertEqual(frames(out[5]), [(0x02, SLAVE << 4 | 1, b'\xaa\x21')])   # only the ping's
        self.assertEqual(out[6].split()[1], '-')                  # no clock set
        self.master_ok(s)

    @scenario('S-09/S-TUNNEL')
    @requirement('S-09.045')
    def test_S_TUNNEL_overflow(self):
        """Pushes of 0x3000 bytes, the slave's receive stream holding 0x2000 and nothing draining it;
        then bytes for the parked pull: its reply is pull_overflow's (f bit 1), and the slave holds the
        first 0x2000 bytes pushed, in order."""
        e = REP['pull_overflow']
        data = bytes(random.Random(9).randrange(256) for _ in range(0x3000))
        out, s = run_rig(['cpu 0', 'start', 'mwrite ' + hx(data[:0x2000]), 'mwrite ' + hx(data[0x2000:]),
                          'tunnel ' + e['data_hex'], 'service', 'srx'])
        self.assertEqual(out[2:4], [str(0x2000), str(0x1000)])
        (reply,) = replies_to(s, POLL)
        self.assertEqual(bc.check_replies([{'name': 'pull_overflow', 'hex': reply}]), [])
        self.assertEqual(unhex(out[6]), data[:0x2000])
        self.master_ok(s)


def nest(cmd, length=0, arg0=0, arg1=0, payload=b'', max_=0x10000):
    return 'n' + call(cmd, length, arg0, arg1, payload, max_)[1:].replace('all', 'est', 1)


def poll_replies(s):
    """The poll reply sessions that carry a reply: [(cmd, slot, bytes)]."""
    out = []
    for r in replies_to(s, POLL):
        if r[:4] != b'\x06\xff\x00\x00':
            out.append((r[0], r[1], r))
    return out


def request_slots(s):
    """(cmd, slot) of every request session after the start-up."""
    return [(header(m)['cmd'], header(m)['slot']) for m, _ in requests(s)[3:]]


STAT, STATFS, ACCESS, OPENDIR, READDIR, CLOSEDIR = 0x22, 0x23, 0x21, 0x2E, 0x2F, 0x30


class Defer(unittest.TestCase):
    """S-09.022, S-09.031: two caller requests in flight. The bootloader's calls are synchronous, so the
    second is made from the first's PC5 read (h_s09 'nest', a test hook as the scenarios allow); the
    slave model (h_s09 'model') takes the real slave's finished replies and shows each once the next
    request session has come, collected oldest first (fifo) or newest first (lifo)."""

    def stores(self):
        return [store(STAT, 0, 0, unhex(REP['poll_stat']['data_hex'])),
                store(STATFS, 0, 0, unhex(REP['poll_statfs']['data_hex']))]

    @scenario('S-09/M-DEFER')
    def test_M_DEFER_out_of_order(self):
        """The second request's reply is finished and collected before the first's; each goes to the
        request in its slot and both operations return their own data; every poll found a reply."""
        out, s = run_rig(['start'] + self.stores() + ['model 1 lifo', nest(STATFS, 2, 0, 0, PATH),
                                                      call(STAT, 2, 0, 0, PATH), 'nested', 'modelstat'])
        self.assertEqual(bc.check_master([x for x, _ in s]), [])
        self.assertEqual(out[5].split(), ['0', '0', REP['poll_stat']['data_hex']])
        self.assertEqual(out[6].split(), ['0:0:' + REP['poll_statfs']['data_hex'], ';'])
        self.assertEqual(out[7].split(), ['2', '0', '0'])          # two polls, none answered 06 FF
        sent = request_slots(s)
        self.assertEqual(sent[:2], [(STAT, 1), (STATFS, 2)])
        got = poll_replies(s)
        self.assertEqual([(c, sl) for c, sl, _ in got], [(STATFS, 2), (STAT, 1)])
        self.assertTrue(same_but_slot(got[0][2], REP['poll_statfs']['reply_hex'], 2))
        self.assertTrue(same_but_slot(got[1][2], REP['poll_stat']['reply_hex'], 1))

    @scenario('S-09/M-SLOTS')
    @requirement('S-09.022')
    def test_M_SLOTS(self):
        """A directory listing of 18 entries (opendir, 18 readdirs, closedir: 20 requests), each pair
        with the second made while the first waits, so that the pull and two requests are held: each
        request takes the lowest free slot, no slot is reused while held, and every reply's slot is its
        request's (bridge_check master). The replies are collected newest first, as the calls return:
        the master frees a slot only when its caller takes the reply."""
        ent = bytes(268)
        cmds = ['start', store(OPENDIR, 7), store(READDIR, 1, 0, ent), store(CLOSEDIR, 0), 'model 1 lifo']
        pairs = [(OPENDIR, READDIR)] + [(READDIR, READDIR)] * 8 + [(READDIR, CLOSEDIR)]
        for first, second in pairs:
            p = PATH if first == OPENDIR else b''
            cmds += [nest(second, 0, 7), call(first, len(p), 7 if not p else 0, 0, p)]
        out, s = run_rig(cmds + ['nested'])
        self.assertEqual(bc.check_master([x for x, _ in s]), [])
        sent = [x for x in request_slots(s) if x[0] != POLL]
        self.assertEqual(len(sent), 20)
        self.assertEqual([sl for _, sl in sent], [1, 2] * 10)      # held: the pull (0) and two requests
        self.assertEqual(len(out[-1].split()), 11)                  # ten nested calls made

    @scenario('S-09/S-DEFER')
    @requirement('S-09.031')
    def test_S_DEFER(self):
        """Three deferred requests in slots 1, 2 and 3 without polling, then polls: PB2 falls once the
        first is finished and rises with the poll that takes the last; the polls return the replies in
        the order they finished, each whole, then poll_none."""
        reqs = [(STAT, 1), (STATFS, 2), (ACCESS, 3)]
        cmds = ['start'] + self.stores() + [store(ACCESS, -1, 2), 'satt']
        for cmd, slot in reqs:
            cmds += ['sreq ' + (struct.pack('<III', cmd | slot << 8 | 2 << 16, 0, 0) + PATH).hex(), 'satt']
        for slot in (1, 2, 3, 1):
            cmds += ['sreq ' + struct.pack('<III', POLL | slot << 8, 0, 0).hex(), 'satt']
        out, _ = run_rig(cmds)
        att = [out[i] for i in range(4, len(out), 2)]
        self.assertEqual(att, ['0', '1', '1', '1', '1', '1', '0', '0'])
        replies = [unhex(out[i].split()[1]) for i in range(11, len(out), 2)]
        for got, (name, slot) in zip(replies, (('poll_stat', 1), ('poll_statfs', 2), ('poll_access_enoent', 3))):
            self.assertTrue(same_but_slot(got, REP[name]['reply_hex'], slot), name)
        self.assertEqual(bc.check_replies([{'name': 'poll_none', 'hex': replies[3]}]), [])


T_BOOT_READY, T_STREAM = 500000, 2000000   # src/include/waffle/s09_master.h, us
T_POWER = 2000000
SLACK = 50000                               # the declared value + 50 ms


class BootStream(unittest.TestCase):
    """S-09.001-S-09.007: s09_boot_slave on the synthetic slots, against a boot-link model that answers
    FF to 03 and drives PA6 high for a seeded 0-50 us after each byte."""

    def boot(self, vector, crc=1, mode=0, seed=1, stuck=0):
        path = spec_path('S-09-bridge', 'vectors', 'slave_stream_%s.bin' % vector)
        f = run('s09', ['boot %s %d %d %d %d' % (path, crc, mode, seed, stuck)])[0].split()
        rc, end, high, release, elapsed = (int(x) for x in f[:5])
        return {'rc': rc, 'end': end, 'high': high, 'release': release, 'elapsed': elapsed,
                'sessions': [unhex(x) for x in f[5:]]}

    def bootx(self, vector, fc, late=0, crc=1, mode=0, seed=1, stuck=0, settle=0):
        """As boot(), with the stream's sessions through the ops' send() when fc is 1 (BOOT_LINK_FC), and a
        slave that raises PA6 only `late` us after a byte's end. 'early' counts the bytes started in that
        gap. `settle`: the ops' byte returns that many us after the byte, as the board's does."""
        path = spec_path('S-09-bridge', 'vectors', 'slave_stream_%s.bin' % vector)
        f = run('s09', ['bootx %s %d %d %d %d %d %d %d' % (path, crc, mode, seed, stuck, fc, late,
                                                           settle)])[0].split()
        rc, end, high, release, elapsed, early = (int(x) for x in f[:6])
        return {'rc': rc, 'end': end, 'high': high, 'release': release, 'elapsed': elapsed, 'early': early,
                'sessions': [unhex(x) for x in f[6:]]}

    @scenario('S-09/B-STREAM')
    def test_B_STREAM(self):
        for vector, end in (('final', 0), ('badsig', 1), ('truncated', 2)):
            for seed in (1, 2, 3):
                r = self.boot(vector, seed=seed)
                with self.subTest(vector=vector, seed=seed):
                    self.assertEqual((r['rc'], r['end']), (0, end))
                    self.assertEqual(r['sessions'][0], b'\x03')                         # S-09.004
                    self.assertEqual(bc.check_stream(vector, r['sessions']), [])         # S-09.005, .006
                    self.assertEqual(r['high'], 0)                                       # S-09.005

    @scenario('S-09/B-STREAM')
    def test_B_STREAM_variants(self):
        r = self.boot('final', crc=0)                           # bad_crc: S-09.002
        self.assertEqual((r['rc'], r['sessions']), (1, []))
        r = self.boot('final', mode=1)                          # no_ready: S-09.003
        self.assertEqual((r['rc'], r['sessions']), (2, []))
        self.assertLessEqual(r['elapsed'], T_BOOT_READY + 1000000)
        no_open = [bytes.fromhex(vectors('slave_stream_final.json')['no_open_session_hex'])]
        r = self.boot('final', mode=2)                          # no_open: S-09.007
        self.assertEqual((r['rc'], r['sessions']), (3, no_open))
        r = self.boot('final', mode=3)                          # no_open_stuck: S-09.007, S-09.060
        self.assertEqual((r['rc'], r['sessions']), (3, no_open))
        self.assertLessEqual(r['release'], T_BOOT_READY + SLACK)
        self.assertTrue(200000 <= T_BOOT_READY <= 1000000)

    @scenario('S-09/B-STREAM')
    def test_B_STREAM_slow_block(self):
        """slow_block: PA6 held high for 900 ms after the last byte of the stream's first FILL block
        header; the stream goes on and completes as in the base run (T_STREAM's floor)."""
        with open(spec_path('S-09-bridge', 'vectors', 'slave_stream_final.bin'), 'rb') as f:
            slot = f.read()
        off = 0
        while True:                                 # S-09.005: header, then count bytes unless FILL
            code, _, count = struct.unpack_from('<III', slot, off)
            if code & 0x0100:
                break
            off += 16 + (0 if count == 0 else count)
        r = self.boot('final', mode=4, stuck=1 + off + 16)   # the opening 03, then the slot's bytes
        self.assertEqual((r['rc'], r['end']), (0, 0))
        self.assertEqual(r['sessions'][0], b'\x03')
        self.assertEqual(bc.check_stream('final', r['sessions']), [])
        self.assertEqual(r['high'], 0)
        self.assertGreaterEqual(r['elapsed'], 900000)
        self.assertTrue(1000000 <= T_STREAM <= 5000000)

    @scenario('S-09/B-STREAM')
    def test_B_STREAM_controller_paced(self):
        """BOOT_LINK_FC: the stream's sessions moved by the SPI controller under its own flow control on
        PA6 (S-13b.022). The same sessions and bytes, the opening byte and S-09.007's still one at a time,
        no byte while PA6 reads high, and the same bounds."""
        for vector, end in (('final', 0), ('badsig', 1), ('truncated', 2)):
            for seed in (1, 2, 3):
                r = self.bootx(vector, 1, seed=seed)
                with self.subTest(vector=vector, seed=seed):
                    self.assertEqual((r['rc'], r['end']), (0, end))
                    self.assertEqual(r['sessions'][0], b'\x03')
                    self.assertEqual(bc.check_stream(vector, r['sessions']), [])
                    self.assertEqual((r['high'], r['early']), (0, 0))
                    self.assertEqual(r['sessions'], self.boot(vector, seed=seed)['sessions'])
        r = self.bootx('final', 1, crc=0)                       # bad_crc
        self.assertEqual((r['rc'], r['sessions']), (1, []))
        r = self.bootx('final', 1, mode=1)                      # no_ready
        self.assertEqual((r['rc'], r['sessions']), (2, []))
        no_open = [bytes.fromhex(vectors('slave_stream_final.json')['no_open_session_hex'])]
        for mode in (2, 3):                                     # no_open, no_open_stuck
            r = self.bootx('final', 1, mode=mode)
            self.assertEqual((r['rc'], r['sessions']), (3, no_open))
            self.assertLessEqual(r['release'], T_BOOT_READY + SLACK)
        r = self.bootx('final', 1, stuck=100)                   # stream_stuck: S-09.060
        self.assertEqual(r['rc'], 4)
        self.assertEqual(sum(len(s) for s in r['sessions']), 100)
        self.assertLessEqual(r['release'], T_STREAM + SLACK)

    @scenario('S-09/B-STREAM')
    def test_B_STREAM_controller_paced_late(self):
        """What BOOT_LINK_FC does with a slave that raises PA6 only some time after a byte's end: the
        controller looks at PA6 as the byte ends, reads it low and starts the next byte. No byte starts
        while PA6 reads high, and every byte is sent, but bytes go out that the slave meant to hold off.
        With no such gap there is no such byte."""
        r = self.bootx('final', 1, late=0)
        self.assertEqual((r['rc'], r['high'], r['early']), (0, 0, 0))
        r = self.bootx('final', 1, late=3)
        self.assertEqual((r['rc'], r['end'], r['high']), (0, 0, 0))
        self.assertEqual(bc.check_stream('final', r['sessions']), [])
        self.assertGreater(r['early'], 0)
        # the settle time is the byte-wise path's: it does nothing for the controller's sessions,
        # whatever it is set to, even for a raise inside it
        for settle in (0, 2):
            for late in (1, 2, 3):
                self.assertGreater(self.bootx('final', 1, late=late, settle=settle)['early'], 0)

    @scenario('S-09/B-STREAM')
    def test_B_STREAM_settle(self):
        """The settle time between a byte and the sample of PA6 that decides the next (the board's
        BOOT_LINK_SETTLE_US, 2 us by default), against a slave that raises PA6 only some time after a
        byte's end. A raise inside the settle time holds the next byte off: no byte goes out in the gap.
        With no settle time, or a raise later than it, bytes do. The stream, its sessions and its bounds
        are the same with it."""
        for late in (0, 1, 2):
            r = self.bootx('final', 0, late=late, settle=2)
            with self.subTest(late=late):
                self.assertEqual((r['rc'], r['end'], r['high'], r['early']), (0, 0, 0, 0))
                self.assertEqual(bc.check_stream('final', r['sessions']), [])
        self.assertGreater(self.bootx('final', 0, late=3, settle=2)['early'], 0)    # past the settle time
        self.assertEqual(self.bootx('final', 0, late=3, settle=5)['early'], 0)      # a longer one covers it
        r = self.bootx('final', 0, late=1, settle=0)                                # none: every byte early
        self.assertEqual((r['rc'], r['high']), (0, 0))
        self.assertEqual(r['early'], sum(len(s) for s in r['sessions']) - 1)
        self.assertEqual(self.bootx('final', 0, late=0, settle=0)['early'], 0)
        # the variants and bounds with the settle time
        for vector, end in (('final', 0), ('badsig', 1), ('truncated', 2)):
            r = self.bootx(vector, 0, settle=2)
            self.assertEqual((r['rc'], r['end'], r['high']), (0, end, 0))
            self.assertEqual(r['sessions'], self.boot(vector)['sessions'])
        r = self.bootx('final', 0, stuck=100, settle=2)                             # stream_stuck
        self.assertEqual(r['rc'], 4)
        self.assertEqual(sum(len(s) for s in r['sessions']), 100)
        self.assertLessEqual(r['release'], T_STREAM + SLACK)
        no_open = [bytes.fromhex(vectors('slave_stream_final.json')['no_open_session_hex'])]
        r = self.bootx('final', 0, mode=3, settle=2)                                # no_open_stuck
        self.assertEqual((r['rc'], r['sessions']), (3, no_open))
        self.assertLessEqual(r['release'], T_BOOT_READY + SLACK)

    def bootf(self, vector, stuck=0, late=0, bound=0, guard=20, fault=0, at=0):
        """The stream as BOOT_LINK_FC=2 sends it (S-09.009): s09_fast_send() as the boot ops' send(),
        its parts moved by the model controller GUARD us apart, after a fall-back one byte at a time.
        BOUND: the slave raises PA6 `late` us only after a header and after a piece (else after every
        byte). FAULT: the model controller's own failure (S09_PART_*) before its AT-th byte. 'parts': the
        parts' lengths grouped by session, the opening one left out."""
        path = spec_path('S-09-bridge', 'vectors', 'slave_stream_%s.bin' % vector)
        f = run('s09', ['bootf %s %d %d %d %d %d %d' % (path, stuck, late, bound, guard, fault,
                                                        at)])[0].split()
        rc, end, high, release, elapsed, early, fell, fell_at, slow, after, nparts = (int(x) for x in f[:11])
        flat = [int(x) for x in f[11:11 + nparts]]
        sessions = [unhex(x) for x in f[11 + nparts:]]
        parts, k = [], 0
        for s in sessions[1:]:
            group, n = [], 0
            while n < len(s) and k < len(flat):
                group.append(flat[k])
                n += flat[k]
                k += 1
            parts.append(group)
        return {'rc': rc, 'end': end, 'high': high, 'release': release, 'elapsed': elapsed, 'early': early,
                'fell': fell, 'fell_at': fell_at, 'slow': slow, 'after': after, 'nparts': nparts,
                'parts': parts, 'sessions': sessions}

    @scenario('S-09/B-STREAM')
    @requirement('S-09.009')
    def test_B_STREAM_fast(self):
        """BOOT_LINK_FC=2 (S-09.009): the same sessions and bytes as the byte path, each session in the
        parts of slave_stream_parts.json (a header alone, then its payload), the guard between parts, no
        byte while PA6 reads high, and the same bounds: a line that never falls ends the stream within
        T_STREAM, with no fall-back."""
        for vector, end in (('final', 0), ('badsig', 1), ('truncated', 2)):
            r = self.bootf(vector)
            with self.subTest(vector=vector):
                self.assertEqual((r['rc'], r['end'], r['high'], r['early'], r['fell']), (0, end, 0, 0, 0))
                self.assertEqual(bc.check_stream(vector, r['sessions']), [])
                self.assertEqual(bc.check_parts(vector, r['parts']), [])
                self.assertEqual(r['sessions'], self.boot(vector)['sessions'])
        r = self.bootf('final', stuck=100)                       # stream_stuck: S-09.060
        self.assertEqual((r['rc'], r['fell']), (4, 0))
        self.assertEqual(sum(len(s) for s in r['sessions']), 100)
        self.assertLessEqual(r['release'], T_STREAM + SLACK)

    @scenario('S-09/B-STREAM')
    @requirement('S-09.009')
    def test_B_STREAM_fast_guard(self):
        """The guard (S-09.009, S-13b.023) against a slave that raises PA6 late only where its boot ROM
        changes state, after a block header and after a piece: within the guard no byte goes out early;
        past it some do, as they would inside a part from a slave that raises late after any byte, which
        S-13b.023 leaves to the boot ROM's receive watermark."""
        for late in (1, 5, 19):
            r = self.bootf('final', late=late, bound=1, guard=20)
            with self.subTest(late=late):
                self.assertEqual((r['rc'], r['high'], r['early']), (0, 0, 0))
                self.assertEqual(bc.check_stream('final', r['sessions']), [])
        self.assertGreater(self.bootf('final', late=25, bound=1, guard=20)['early'], 0)
        self.assertEqual(self.bootf('final', late=25, bound=1, guard=40)['early'], 0)
        self.assertGreater(self.bootf('final', late=3, bound=0, guard=20)['early'], 0)
        # the byte path's settle time covers none of these past 2 us
        self.assertGreater(self.bootx('final', 0, late=5, settle=2)['early'], 0)

    @scenario('S-09/B-STREAM')
    @requirement('S-09.009')
    def test_B_STREAM_fast_fall_back(self):
        """A fault of the controller's own (S09_PART_DMA 2, STALL 3, NO_END 4, REFUSED 5) at the first byte,
        in a header, in a payload and at the last byte: the stream ends whole and the same, every byte
        once, the controller's bytes then the rest one at a time, and no part after the fault."""
        total = sum(len(s) for s in self.boot('final')['sessions'][1:])
        for fault in (2, 3, 4, 5):
            for at in (1, 16, 17, 600, 1060, total):
                r = self.bootf('final', fault=fault, at=at)
                with self.subTest(fault=fault, at=at):
                    self.assertEqual((r['rc'], r['end'], r['high'], r['fell']), (0, 0, 0, fault))
                    self.assertEqual(bc.check_stream('final', r['sessions']), [])
                    self.assertEqual((r['fell_at'], r['slow'], r['after']), (at - 1, total - (at - 1), 0))
        r = self.bootf('final', fault=3, at=20, stuck=40)        # held after the fall-back
        self.assertEqual(r['rc'], 4)
        self.assertEqual(sum(len(s) for s in r['sessions']), 40)

    @scenario('S-09/B-ABSENT')
    def test_B_ABSENT_stream_stuck(self):
        """B-ABSENT stream_stuck, the boot link's part: PA6 stays high after the 100th stream byte; no
        further byte, and the stream gives up (the slave absent) within T_STREAM of that byte."""
        r = self.boot('final', stuck=100)
        self.assertEqual(r['rc'], 4)
        self.assertEqual(sum(len(s) for s in r['sessions']), 100)
        self.assertLessEqual(r['release'], T_STREAM + SLACK)
        self.assertTrue(1000000 <= T_STREAM <= 5000000)


class Records(unittest.TestCase):
    """S-09.043: the stat, statfs and dirent records."""

    @requirement('S-09.043')
    def test_S09_043_records(self):
        cmds, kinds = [], []
        for r in vectors('records.json'):
            f = r['fields']
            if r['kind'] == 'stat':
                keys = ['dev', 'ino', 'mode', 'nlink', 'uid', 'gid', 'rdev', 'size', 'blksize', 'blocks',
                        'atime', 'mtime', 'ctime']
                cmds.append('stat ' + ' '.join(str(f[k]) for k in keys))
            elif r['kind'] == 'statfs':
                cmds.append('statfs %d %d %d' % (f['bsize'], f['blocks'], f['bfree']))
            else:
                cmds.append('dirent %d %d %d %s' % (f['ino'], f['off'], f['type'], hx(f['name'].encode())))
            kinds.append(r['kind'])
        out = run('s09', cmds)
        self.assertEqual(len(kinds), 3)
        self.assertEqual(bc.check_records([{'kind': k, 'hex': unhex(o)} for k, o in zip(kinds, out)]), [])



class QuadTest(unittest.TestCase):
    """S-09.080: the quad reply test's request and reply words (quad_test.json), and the slave's answer
    to 0x1C without a board test behind it."""

    @requirement('S-09.080')
    def test_S09_080_args_and_replies(self):
        import hashlib
        v = vectors('quad_test.json')
        cmds = ['qtargs %d %d' % (a['arg0'], a['arg1']) for a in v['args']]
        cmds += ['qtbuild %d %d %d' % (r['slot'], r['arg0'], r['n']) for r in v['replies']]
        cmds += ['qtlines %d' % ln['diff'] for ln in v['lines']]
        out = run('s09', cmds)
        na, nr = len(v['args']), len(v['replies'])
        for a, o in zip(v['args'], out[:na]):
            self.assertEqual(o, '1' if a['ok'] else '0', a['name'])
        for r, o in zip(v['replies'], out[na:na + nr]):
            if 'reply_hex' in r:
                self.assertEqual(o, r['reply_hex'], r['name'])
            else:
                self.assertEqual(hashlib.sha256(unhex(o)).hexdigest(), r['reply_sha256'], r['name'])
        for ln, o in zip(v['lines'], out[na + nr:]):
            self.assertEqual(int(o), ln['lines'], ln['diff'])

    @requirement('S-09.080', 'S-09.023')
    def test_S09_080_slave_without_test(self):
        """0x1C is defined and immediate: a slave with no test behind it answers each operation with the
        8-byte reply S09_QT_NO_SLAVE, in single I/O, and stays in the plain mode."""
        v = vectors('quad_test.json')
        out = run('s09', ['rig', 'start'] + ['sreq ' + r['hex'] for r in v['requests']] + ['smode'])
        for r, o in zip(v['requests'], out[2:5]):
            rx, rep = o.split()
            self.assertEqual(int(rx), 12, r['name'])
            rep = unhex(rep)
            self.assertEqual(len(rep), 8, r['name'])
            self.assertEqual(rep[0], bm.QUAD_TEST)
            self.assertEqual(struct.unpack_from('<I', rep, 4)[0], v['values']['no_slave'], r['name'])
        self.assertEqual(out[5].split()[0], '0')


# ---- the link modes (S-09.070-S-09.075) ----

LINK_REQ = {it['name']: it for it in vectors('link_requests.json')}
WRITE = 0x2B
T_TRIAL = 2000000                           # src/include/waffle/s09_master.h, us


def link_payload(rule, n):
    """A link_test.json payload from its rule."""
    kind = rule['kind']
    if kind == 'hex':
        return bytes.fromhex(rule['hex'])
    if kind == 'fill':
        return bytes([int(rule['byte'], 16)]) * n
    if kind == 'count':
        return bytes((i + (i >> 8)) & 0xFF for i in range(n))
    if kind == 'walk':
        return b''.join(struct.pack('<I', 1 << (j % 32)) for j in range(n // 4))
    rnd = random.Random(rule['seed'])
    return bytes(rnd.randrange(256) for _ in range(n))


def handed(b):
    """h_s09 commands that hand b to the slave as one session of exactly its bytes (sput, sgo)."""
    b = bytes(b)
    return ['sput -'] + ['sput ' + b[i:i + 0x4000].hex() for i in range(0, len(b), 0x4000)] + ['sgo']


def write_call(n):
    return call(WRITE, n & 0xFFFF, 4, n, 'r7:%d' % n, 0)


class Link(unittest.TestCase):
    """S-09.070-S-09.075: the real master and slave over h_s09's bus, which records each session's rung and
    can carry a rung badly (M-LINK); the slave's mode table driven directly (S-LINK)."""

    def raise_link(self, wire='0 0 0', ceiling=4, after=()):
        out, s = run_rig(['start', 'wire ' + wire, 'link %d' % ceiling] + list(after) + ['mrung', 'smode'])
        ok, rung, fell = (int(x) for x in out[2].split())
        self.assertEqual(ok, 1)
        return out, s, rung, fell

    def check_rung_sessions(self, s):
        """S-09.072: every request session on a rung ends with its check; S-09.070: replies on rung 0."""
        n = 0
        for x, rx in s:
            if x['rung'] and not x.get('late'):
                self.assertTrue(bm.checked_form_ok(x['mosi']), x['mosi'][:12].hex())
                n += 1
        return n

    def rungs_tried(self, s):
        """The rung of each trial, in order: the rungs of the link tests on a rung, each run of them once
        (a rung that failed is not tried again, so two trials never follow on one rung)."""
        out = []
        for x, _ in s:
            if x['mosi'][:1] == bytes([bm.LINK_TEST]) and x['rung'] and out[-1:] != [x['rung']]:
                out.append(x['rung'])
        return out

    @scenario('S-09/M-LINK')
    @requirement('S-09.070', 'S-09.072', 'S-09.073', 'S-09.075')
    def test_M_LINK_rungs(self):
        """Each case of link_rungs.json: the rungs tried, the one kept, the fall-backs, the check on every
        request session on a rung, and the hook over the whole run."""
        masks = {'all_good': '0 0 0', 'top_corrupt': '16 0 0', 'quad_deaf': '0 24 0',
                 'none_above_plain': '30 0 0', 'only_rung_1': '28 0 0', 'ceiling_2': '0 0 0',
                 'ceiling_1': '0 0 0', 'ceiling_1_bad': '2 0 0'}
        for c in vectors('link_rungs.json'):
            if c['name'] not in masks:
                continue
            with self.subTest(case=c['name']):
                out, s, rung, fell = self.raise_link(masks[c['name']], c['ceiling'])
                self.assertEqual((rung, fell), (c['kept'], c['fell']))
                self.assertEqual(self.rungs_tried(s), c['tried'])
                self.assertEqual(bc.check_master([x for x, _ in s]), [])
                if c['tried']:
                    self.assertGreater(self.check_rung_sessions(s), 12 if c['kept'] else 0)
                else:                                                               # rung 1: not tried
                    self.assertEqual(self.check_rung_sessions(s), 0)
                self.assertEqual(int(out[-1].split()[0]), bm.rung_mode(c['kept']))     # the slave's mode
                self.assertEqual(out[-1].split()[1:], ['0', '0'])                      # kept, PB2 high
                self.assertEqual(int(out[-2].split()[2]), 0x8000 if c['kept'] else 0x1000)
                self.assertEqual(out[-2].split()[3], '0')                              # not absent

    @scenario('S-09/M-LINK')
    @requirement('S-09.073')
    def test_M_LINK_value_wrong(self):
        """A test's value wrong on rung 4: 0x19 with mode 0 next, then rung 3 kept."""
        out, s, rung, fell = self.raise_link('0 0 16')
        self.assertEqual((rung, fell), (3, 1))
        self.assertEqual(bc.check_master([x for x, _ in s]), [])
        i = next(k for k, (x, _) in enumerate(s) if x['mosi'][:1] == bytes([bm.LINK_TEST]) and x['rung'] == 4)
        back = header(s[i + 2][0]['mosi'])
        self.assertEqual((back['cmd'], back['arg0'], s[i + 2][0]['rung']), (bm.LINK_MODE, 0, 4))

    @scenario('S-09/M-LINK')
    @requirement('S-09.023', 'S-09.072')
    def test_M_LINK_write(self):
        """On rung 4 a write of 0x7000 bytes is one session of 12 + 0x7000 + 8 bytes, served; the clock is the
        master's: rungs 3 and 4 put the slave in the same mode."""
        out, s, rung, fell = self.raise_link(after=[store(WRITE, 0x7000), write_call(0x7000), 'sops 43'])
        self.assertEqual(out[4].split()[:2], [str(0x7000), '0'])
        self.assertEqual(out[5], '1')
        (w,) = [x for x, rx in s if x['mosi'][:1] == bytes([WRITE])]
        self.assertEqual((len(w['mosi']), w['rung']), (12 + 0x7000 + 8, 4))
        self.assertEqual(bc.check_master([x for x, _ in s]), [])
        out3, _, rung3, _ = self.raise_link(ceiling=3)
        self.assertEqual((rung3, out3[-1].split()[0], out[-1].split()[0]), (3, '2', '2'))

    @scenario('S-09/M-LINK')
    @requirement('S-09.074', 'S-09.076')
    def test_M_LINK_write_once(self):
        """The write's session on rung 4 arrives with a bit wrong: not executed; plain echoes, the link back
        on rung 3, a poll that finds nothing, then the write once more, the same bytes with its number, in
        its slot; written exactly once."""
        out, s, _, _ = self.raise_link(after=[store(WRITE, 0x7000), 'flip 1', write_call(0x7000), 'sops 43'])
        self.assertEqual(out[5].split()[:2], [str(0x7000), '0'])
        self.assertEqual(out[6], '1')                                       # executed once
        self.assertEqual(out[-2].split()[:2], ['3', '1'])                   # rung 3, one fall-back
        self.assertEqual(out[-1].split(), ['2', '0', '0'])
        self.assertEqual(bc.check_master([x for x, _ in s]), [])
        writes = [x for x, _ in s if x['mosi'][:1] == bytes([WRITE])]
        self.assertEqual([(w['rung'], len(w['mosi'])) for w in writes],
                         [(4, 12 + 0x7000 + 8), (3, 12 + 0x7000 + 8)])
        self.assertEqual(writes[0]['mosi'], writes[1]['mosi'])
        self.assertNotEqual(bm.seq_of(writes[0]['mosi']), 0)
        echo = next(k for k, (x, _) in enumerate(s) if x['mosi'][:1] == bytes([0x0A]) and x['rung'] == 0 and k > 4)
        self.assertLess(echo, next(k for k, (x, _) in enumerate(s) if x['mosi'] == writes[1]['mosi'] and
                                   x['rung'] == 3))

    @scenario('S-09/M-LINK')
    @requirement('S-09.074', 'S-09.076')
    def test_M_LINK_slave_alone(self):
        """The slave falls back by itself, PB2 low; the master's next request finds it out, and an access is
        served once, on the rung below."""
        out, s, _, _ = self.raise_link(after=['sfault', 'smode', store(0x21, 0), call(0x21, 2, 0, 0, PATH),
                                              'sops 33'])
        self.assertEqual(out[4].split(), ['0', '0', '1'])                   # plain, PB2 low
        self.assertEqual(out[6].split()[:2], ['0', '0'])
        self.assertEqual(out[7], '1')
        self.assertEqual(out[-2].split()[:2], ['3', '1'])
        self.assertEqual(out[-1].split(), ['2', '0', '0'])                  # PB2 high after the echo
        self.assertEqual(bc.check_master([x for x, _ in s]), [])

    @scenario('S-09/M-LINK')
    @requirement('S-09.078')
    def test_M_LINK_lowered(self):
        """The release lowers the link first: 0x19 with mode 0 on rung 4, then the release in the plain
        mode."""
        out, s, _, _ = self.raise_link(after=[store(0x07, 1), 'release'])
        self.assertEqual(out[-2].split()[:2], ['0', '0'])
        self.assertEqual(out[-1].split()[0], '0')
        self.assertEqual(bc.check_master([x for x, _ in s]), [])
        k = next(i for i, (x, _) in enumerate(s) if x['mosi'][:1] == bytes([0x07]))
        self.assertEqual(s[k][0]['rung'], 0)
        self.assertEqual((header(s[k - 2][0]['mosi'])['cmd'], header(s[k - 2][0]['mosi'])['arg0'],
                          s[k - 2][0]['rung']), (bm.LINK_MODE, 0, 4))

    @scenario('S-09/M-LINK')
    @requirement('S-09.074', 'S-09.013')
    def test_M_LINK_echoes_unanswered(self):
        """As write_once, and PA6 stays high after the write: no echo answered, ENODEV, the slave absent and
        no further session."""
        out, s, _, _ = self.raise_link(after=[store(WRITE, 0x7000), 'flip 1', 'dead 1', write_call(0x7000),
                                              'sops 43', call(0x03)])
        self.assertEqual(out[6].split()[:2], ['-1', '19'])
        self.assertEqual(out[7], '0')
        self.assertEqual(out[8].split()[:2], ['-1', '19'])
        self.assertEqual(out[-2].split()[3], '1')
        self.assertEqual(header(s[-1][0]['mosi'])['cmd'], WRITE)
        self.assertEqual(bc.check_master([x for x, _ in s], absent_run=True), [])

    # -- the slave (S-LINK) --

    def to_state(self, state):
        """sreq commands that bring a fresh linked slave to a link_modes.json state."""
        q = lambda b: 'sreq ' + b.hex()
        if state == 'plain':
            return []
        if state == 'plain_fell':
            return [q(bm.request(bm.LINK_MODE, 1, 1, 0)), 'sraw ' + bm.header(0x21, 1, 2).hex()]
        if state == 'plain_recovering':
            return self.to_state('plain_fell') + [q(bm.request(0x0A, 1, bm.ECHO_RECOVER))]
        mode = 1 if state.startswith('checked') else 2
        cmds = [q(bm.request(bm.LINK_MODE, 1, mode, 0))]
        if state.endswith('kept'):
            cmds.append(q(bm.request_in_mode(mode, bm.LINK_MODE, 1, mode, 1)))
        return cmds

    def event(self, ev, mode):
        q = lambda b: 'sreq ' + b.hex()
        if ev.startswith('mode_'):
            _, m_, how = ev.split('_', 2)
            arg1 = 1 if how == 'keep' else (2 if how == 'arg1_2' else 0)
            return [q(bm.request_in_mode(mode, bm.LINK_MODE, 1, int(m_), arg1))]
        if ev == 'link_test':
            return [q(bm.link_test_request(1, b'abcd', 0, mode))]
        if ev == 'echo':
            return [q(bm.request_in_mode(mode, 0x0A, 1, 0x1234))]
        if ev == 'recovery_echo':
            return [q(bm.request_in_mode(mode, 0x0A, 1, bm.ECHO_RECOVER))]
        if ev == 'access':
            return [q(bm.request_in_mode(mode, 0x21, 1, 0, 0, 2, PATH))]
        if ev == 'init':                          # S-09.015: exactly as a master sends it, slot 0
            return [q(bm.request_in_mode(mode, bm.INIT, 0))]
        if ev == 'status':                        # S-09.079: exactly
            return [q(bm.request_in_mode(mode, bm.LINK_STATUS, 1))]
        if ev == 'bad_check':
            b = bytearray(bm.request_in_mode(mode, 0x0A, 1, 0x1234))
            b[-1] ^= 1
            return ['sraw ' + bytes(b).hex()]
        if ev == 'short':
            return ['sraw ' + bm.header(0x21, 1, 2).hex()]
        if ev == 'ignored':
            return ['sraw ' + bm.header(0x21, 4, 5).hex()]
        return ['sfault']                         # rx_fail; trial_timeout only on trial (below)

    @scenario('S-09/S-LINK')
    @requirement('S-09.073', 'S-09.074')
    def test_S_LINK_modes(self):
        """Each row of link_modes.json: executed or not, 0x19's value, then the mode, the trial and PB2; then a
        whole request clears the fall-back's PB2 when no finished reply waits."""
        for r in vectors('link_modes.json'):
            ev = r['event']
            if ev == 'trial_timeout' and not r['trial']:
                ev_cmds = []                      # the slave's bound runs only on trial
            else:
                ev_cmds = self.event(ev, r['mode'])
            nxt = r['next']
            clear = (bm.link_test_request(1, b'wxyz', 0, nxt['mode']) if nxt['trial'] else
                     bm.request_in_mode(nxt['mode'], 0x0A, 1, bm.ECHO_RECOVER))
            pre = self.to_state(r['state'])
            out, _ = run_rig(['start', store(0x21, 0)] + pre + ['smode', 'sops 33'] + ev_cmds +
                             ['sops 33', 'smode', 'sreq ' + clear.hex(), 'smode'])
            with self.subTest(state=r['state'], event=ev):
                k = 2 + len(pre)
                self.assertEqual(out[k].split(), [str(r['mode']), str(int(r['trial'])), str(int(r['fell']))])
                if ev_cmds:
                    reply = out[k + 2].split()[-1] if out[k + 2] != 'ok' else '-'
                else:
                    reply = '-'
                ran = int(out[k + 2 + len(ev_cmds)]) == 1 if ev == 'access' else reply != '-'
                self.assertEqual(ran, r['executed'])
                if r['value'] is not None:
                    self.assertEqual(struct.unpack_from('<I', unhex(reply), 4)[0], r['value'])
                waits = r['executed'] and ev == 'access'
                self.assertEqual(out[-3].split(), [str(nxt['mode']), str(int(nxt['trial'])),
                                                   str(int(nxt['fell'] or waits))])
                self.assertEqual(out[-1].split()[2], str(int(waits)))

    @scenario('S-09/S-LINK')
    @requirement('S-09.071', 'S-09.037')
    def test_S_LINK_test_value(self):
        """Each item of link_test.json, in the plain mode and in the quad mode on trial."""
        for mode in (0, 2):
            cmds = ['start'] + (['sreq ' + bm.request(bm.LINK_MODE, 1, 2, 0).hex()] if mode else [])
            items = vectors('link_test.json')
            ends = []
            for it in items:
                p = link_payload(it['rule'], it['taken'])
                b = bm.request_in_mode(mode, bm.LINK_TEST, 1, 0, it['n'], it['n'] & 0xFFFF, p)
                cmds += handed(b)
                ends.append(len(cmds) - 1)
            out, _ = run_rig(cmds + ['smode'])
            got = [out[k] for k in ends]
            for it, o in zip(items, got):
                with self.subTest(mode=mode, name=it['name']):
                    # S-09.072: outside the plain mode the reply ends with its check word
                    want = bm.reply_in_mode(mode, bytes.fromhex(it['reply_hex']))
                    self.assertEqual(o.split()[1], want.hex())
            self.assertEqual(out[-1].split()[:2], [str(mode), str(int(bool(mode)))])   # still on trial

    @scenario('S-09/S-LINK')
    @requirement('S-09.072', 'S-09.074')
    def test_S_LINK_checks(self):
        """Each item of link_requests.json outside the plain mode, the slave in its mode, kept: served; with a
        check byte wrong: not executed, the slave in the plain mode with PB2 low."""
        for it in vectors('link_requests.json'):
            if not it['mode']:
                continue
            mode = it['mode']
            b = bytes.fromhex(it['session_hex']) if 'session_hex' in it else \
                bytes.fromhex(it['session_head_hex'])[:12] + bytes((i * 7) & 0xFF for i in range(
                    it['request_len'] - 12)) + bytes.fromhex(it['check_hex'])
            bad = bytearray(b)
            bad[-5] ^= 0x80
            up = ['start', store(WRITE, 0x44), 'sreq ' + bm.request(bm.LINK_MODE, 1, mode, 0).hex(),
                  'sreq ' + bm.request_in_mode(mode, bm.LINK_MODE, 1, mode, 1).hex()]
            out, _ = run_rig(up + handed(b) + ['sops 43', 'smode'])
            out2, _ = run_rig(up + handed(bad) + ['sops 43', 'smode'])
            with self.subTest(name=it['name']):
                served = out[-3].split()[1] != '-' or out[-2] == '1'
                self.assertTrue(served)
                self.assertEqual(out[-1].split()[0], str(mode))
                self.assertEqual(out2[-3].split()[1], '-')
                self.assertEqual(out2[-2], '0')
                self.assertEqual(out2[-1].split(), ['0', '0', '1'])
        self.assertTrue(1000000 <= T_TRIAL <= 2000000)                 # S-09.060's T_TRIAL

    @scenario('S-09/S-LINK')
    @requirement('S-09.076', 'S-09.074')
    def test_S_LINK_seq(self):
        """Each case of link_seq.json, the slave kept in the checked mode: a new request executed, a repeat
        not executed and answered as before, a fault refused with the slave in the plain mode, PB2 low."""
        for c in vectors('link_seq.json'):
            up = ['start', store(0x26, 0), 'sreq ' + bm.request(bm.LINK_MODE, 1, 1, 0).hex(),
                  'sreq ' + bm.request_in_mode(1, bm.LINK_MODE, 1, 1, 1).hex(), 'sops 38']
            cmds = list(up)
            for st in c['steps']:
                cmds += ['sraw ' + st['session_hex'], 'sops 38', 'smode']
            out, _ = run_rig(cmds)
            last = {}
            for k, st in enumerate(c['steps']):
                with self.subTest(case=c['name'], step=k):
                    rx, ran, mode = out[len(up) + 3 * k:len(up) + 3 * k + 3]
                    b = bytes.fromhex(st['session_hex'])
                    reply = rx.split()[-1]
                    if st['outcome'] in ('fault', 'dropped'):
                        self.assertEqual((reply, ran), ('-', '0'))
                        self.assertEqual(mode.split(), ['0', '0', str(int(st.get('pb2_low', True)))])
                    elif st['outcome'] == 'repeat':
                        self.assertEqual(ran, '0')
                        self.assertEqual(reply, last.get(b, '-'))
                    else:
                        self.assertTrue(reply != '-' or ran == '1')
                    last.setdefault(b, reply)

    @requirement('S-09.023', 'S-09.037')
    def test_S_LINK_write_8000(self):
        """S-LINK row write_8000 through wire_s09 on yaffs2 (h_s11's loopback): 0x8000 bytes written whole,
        0x8001 refused with EINVAL and no file operation, in the plain mode and on rungs 2 and 4 (rung 1 is
        never kept, S-09.075)."""
        out = run('s11', ['vol'] + ['bwrite %d %d' % (n, r) for r in (0, 2, 4) for n in (0x8000, 0x8001)])
        self.assertEqual(out[1:], ['32768 0 32768 0', '-1 22 0 0', '32768 0 32768 1', '-1 22 0 1',
                                   '32768 0 32768 2', '-1 22 0 2'])

if __name__ == '__main__':
    unittest.main()
