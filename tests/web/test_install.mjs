// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tests of docs/recover/install.js, the installer's protocol and what the page says about it:
//
//     node --test tests/web/test_install.mjs
//
// The parsers against frames built by hand from the protocol's tables, byte for byte, and each
// case of the simulated installer (installer_model.py, over unit_bridge.py's socket) walked from
// its first check to its end: the phases in order and the page's lines at each. Without python3
// the walks are skipped.

import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';
import net from 'node:net';
import * as core from '../../docs/recover/core.js';
import * as inst from '../../docs/recover/install.js';

const here = (name) => fileURLToPath(new URL(name, import.meta.url));
const unhex = (s) => Uint8Array.from(s.match(/../g) || [], (b) => parseInt(b, 16));
const hexOf = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const TIMES = { timeout: 400, retries: 3 };
const TICK = 0.05;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// --- frames built by hand: "WR", type, seq, u16 length, payload, CRC-32 (zlib) over type..payload

const REQ = {
    ihello: '57524001000016ed92bb',                 // 0x40, seq 1, no payload
    state: '5752410200002a346801',                  // 0x41, seq 2
    test5: '5752420301000562cd0c87',                // 0x42, seq 3, index 5
    report1024: '57524304040000040000bed4e7aa',     // 0x43, seq 4, offset 1024
};
// IHELLO's reply: WBIN, protocol 1, phase 4 (ready), flags 0x07, 20 tests, version 1.0.3+7,
// length 0x5A3C0, CRC-32 0x8E1F4C27, pin a0..bf, 8 reserved zeros
const IHELLO_REPLY = '5752c00140005742494e010407140100000003000700c0a30500274c1f8ea0a1a2a3a4a5a6a7a8' +
    'a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf000000000000000019248595';
// STATE's reply: phase 7, flags 7, verdict PASS, no test under way, no lock, selector Slot 1
// settled, slots 0x01 and 0x92, target Slot 2, result done, OS found and checked, hold 640 of
// key 1, trial reached the boot menu, next key 1; 18 tests pass, then FAIL, then not tested
const STATE_REPLY = '5752c1023400070700ff0000010192010001800201020100000000000000000000000000000001' +
    '010101010101010101010101010101010102034859f2ac';
// TEST's reply for index 5
const TEST_REPLY = '5752c203450000050201666c6173682d6c61796f757400466c617368206c61796f757400736' +
    '56c6563746f7220756e7265616461626c6500726561642032206f662032206661696c656400d46230ea';
// REPORT's reply: status 0, total 1030, "ab\n"
const REPORT_REPLY = '5752c3040800000604000061620af9c9abf9';
// the installer's answer to a type it does not know (recovery's HELLO, say)
const UNKNOWN_REPLY = '5752c0090100fff6d5ff7e';

const one = (h) => {
    const f = core.takeFrame({ bytes: unhex(h) });
    assert.ok(f, 'a whole frame with a good CRC');
    return f;
};

test('requests go out as the protocol builds them', () => {
    assert.equal(hexOf(core.frame(inst.IHELLO, 1)), REQ.ihello);
    assert.equal(hexOf(core.frame(inst.STATE, 2)), REQ.state);
    assert.equal(hexOf(core.frame(inst.TEST, 3, Uint8Array.of(5))), REQ.test5);
    assert.equal(hexOf(core.frame(inst.REPORT, 4, unhex('00040000'))), REQ.report1024);
    assert.deepEqual([inst.IHELLO, inst.STATE, inst.TEST, inst.REPORT], [0x40, 0x41, 0x42, 0x43]);
});

test('IHELLO: every field, and a reply that is not the installer', () => {
    const f = one(IHELLO_REPLY);
    assert.deepEqual([f.type, f.seq, f.payload.length], [0xC0, 1, 64]);
    const h = inst.parseIHello(f.payload);
    assert.deepEqual({ ...h, pin: hexOf(h.pin) }, { magic: 'WBIN', proto: 1, phase: 4, flags: 7,
        tests: 20, version: [1, 0, 3, 7], length: 0x5A3C0, crc: 0x8E1F4C27,
        pin: 'a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf' });
    assert.equal(inst.parseIHello(Uint8Array.of(0x40)), null, "recovery's status byte");
    assert.equal(inst.parseIHello(Uint8Array.of(0xFF)), null);
    assert.throws(() => inst.parseIHello(f.payload.subarray(0, 40)), core.Failure);
    const u = one(UNKNOWN_REPLY);
    assert.deepEqual([u.type, u.seq, Array.from(u.payload)], [0xC0, 9, [0xFF]]);
});

test('STATE: every field and each test', () => {
    const f = one(STATE_REPLY);
    assert.deepEqual([f.type, f.seq, f.payload.length], [0xC1, 2, 52]);
    const s = inst.parseState(f.payload, 20);
    assert.deepEqual(s, { phase: 7, flags: 7, verdict: 0, current: 0xFF, lock: 0, selIndex: 0,
        selVerified: 1, slots: [0x01, 0x92], target: 1, result: 0, os: 1, hold: 640, holdKey: 1,
        trial: 2, nextKey: 1, tests: [...Array(18).fill(1), 2, 3] });
    assert.throws(() => inst.parseState(f.payload, 21), core.Failure);
});

test('TEST and REPORT', () => {
    const t = inst.parseTest(one(TEST_REPLY).payload);
    assert.deepEqual(t, { status: 0, index: 5, state: 2, kind: 1, id: 'flash-layout',
        name: 'Flash layout', detail: 'selector unreadable', note: 'read 2 of 2 failed' });
    assert.deepEqual(inst.parseTest(Uint8Array.of(1)), { status: 1 });
    const r = inst.parseReport(one(REPORT_REPLY).payload);
    assert.deepEqual([r.status, r.total, new TextDecoder().decode(r.data)], [0, 1030, 'ab\n']);
});

test('a frame is found after noise and a damaged frame, as recovery resyncs', () => {
    const bad = unhex(IHELLO_REPLY);
    bad[10] ^= 1;
    const rx = { bytes: new Uint8Array([...unhex('005752ff'), ...bad, ...unhex(STATE_REPLY)]) };
    const f = core.takeFrame(rx);
    assert.equal(f.type, 0xC1);
    assert.equal(rx.bytes.length, 0);
});

test('describeInstall: the words for the state above', () => {
    const h = inst.parseIHello(one(IHELLO_REPLY).payload);
    const s = inst.parseState(one(STATE_REPLY).payload, 20);
    const d = inst.describeInstall(s, h, null);
    assert.equal(d.phase.name, 'commit ready');
    assert.equal(d.tone, 'act');
    assert.deepEqual(d.image, { version: 'v1.0.3', length: 0x5A3C0, crc: '8e1f4c27',
        pin: 'a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf', pinShort: 'a0a1a2a3',
        verified: true, breadcrumb: false });
    assert.equal(d.target, 'Slot 2');
    assert.deepEqual(d.hold, { key: 1, progress: 0.64 });
    assert.deepEqual(d.slots[1].words, ['holds this image for a trial', 'trial started',
        'trial reached the boot menu']);
    assert.deepEqual(d.lines, ['The trial start reached the boot menu.', 'Holding 1: 64 %.']);
    assert.deepEqual(d.advice, ['Hold 1 on the unit for 5 s to keep the new bootloader.', inst.LEAVE]);
    assert.equal(inst.describeInstall({ ...s, os: 6 }).leave, false, 'leave only while the OS can be made active');
    assert.equal(d.checks[18].stateText, 'FAIL');
    // each lock reason and failure code has words, and unknown ones are named
    for (const r of [1, 2, 3, 4, 5, 6, 7])
        assert.ok(inst.LOCK[r] && inst.LOCK_ADVICE[r]);
    assert.equal(inst.failureText(0x42), 'failure code 0x42');
    assert.equal(inst.describeInstall({ ...s, phase: 11, trial: 4 }).lines[0],
        'The trial failed: the trial start did not run the new bootloader.');
    assert.equal(inst.describeInstall({ ...s, phase: 3, lock: 9, os: 6 }).lines[1], 'Why: lock reason 9.');
});

test('the ferret link is a named constant, empty until the maintainer fills it in', () => {
    assert.equal(typeof inst.FERRET_URL, 'string');
});

// --- the simulated installer ---

const python = spawnSync('python3', ['--version']);
const needPython = !python.error && python.status === 0 ? {} : { skip: 'python3 is not installed' };

class SocketTransport {
    constructor(sock) {
        this.sock = sock;
        this.chunks = [];
        this.waiter = null;
        this.closed = false;
        sock.on('data', (d) => {
            this.chunks.push(new Uint8Array(d));
            this.wake();
        });
        const end = () => {
            this.closed = true;
            this.wake();
        };
        sock.on('close', end);
        sock.on('error', end);
    }

    wake() {
        if (this.waiter) {
            const w = this.waiter;
            this.waiter = null;
            w();
        }
    }

    write(bytes) {
        return new Promise((resolve, reject) => {
            if (this.closed)
                reject(new Error('closed'));
            else
                this.sock.write(bytes, (err) => (err ? reject(err) : resolve()));
        });
    }

    async read(ms) {
        if (!this.chunks.length && !this.closed)
            await new Promise((resolve) => {
                const timer = setTimeout(() => {
                    this.waiter = null;
                    resolve();
                }, ms);
                this.waiter = () => {
                    clearTimeout(timer);
                    resolve();
                };
            });
        if (this.chunks.length)
            return this.chunks.shift();
        if (this.closed)
            throw new Error('closed');
        return null;
    }

    async close() {
        this.sock.destroy();
    }
}

class Bridge {
    static async start() {
        const b = new Bridge();
        b.proc = spawn('python3', ['-I', '-B', here('unit_bridge.py')], { stdio: ['pipe', 'pipe', 'inherit'] });
        b.lines = createInterface({ input: b.proc.stdout })[Symbol.asyncIterator]();
        b.path = (await b.next()).socket;
        b.transport = null;
        return b;
    }

    async next() {
        const { value, done } = await this.lines.next();
        if (done)
            throw new Error('the bridge ended');
        return JSON.parse(value);
    }

    async call(req) {
        this.proc.stdin.write(JSON.stringify(req) + '\n');
        const r = await this.next();
        if (r.error)
            throw new Error(`bridge: ${r.error}`);
        return r;
    }

    async socket() {
        if (this.transport)
            await this.transport.close();
        const sock = net.connect(this.path);
        await new Promise((resolve, reject) => {
            sock.once('connect', resolve);
            sock.once('error', reject);
        });
        this.transport = new SocketTransport(sock);
        return new core.Link(this.transport, TIMES);
    }

    async unit(spec) {
        const link = await this.socket();
        await this.call({ op: 'unit', spec });
        return link;
    }

    async reconnect() {
        const link = await this.socket();
        await this.call({ op: 'reconnect' });
        return link;
    }

    act(what) {
        return this.call({ op: 'act', do: what });
    }

    async stop() {
        if (this.transport)
            await this.transport.close();
        this.proc.stdin.write('{"op": "quit"}\n');
        this.proc.stdin.end();
        await new Promise((resolve) => this.proc.once('exit', resolve));
    }
}

async function withBridge(fn) {
    const bridge = await Bridge.start();
    try {
        await fn(bridge);
    } finally {
        await bridge.stop();
    }
}

// Follows the installer as the page does, polling STATE; at each phase in `acts` does what a
// person does on the unit. Ends at a phase in `end`. Returns each phase met, with the page's
// lines and advice when it was first met.
async function walk(bridge, kase, acts, end) {
    let link = await bridge.unit({ kind: 'installer', case: kase, tick: TICK });
    const found = await inst.detect(link);
    assert.equal(found.kind, 'installer');
    const hello = found.hello;
    const tests = {};
    const seen = [];
    const done = new Set();
    let busy = 0;
    for (let round = 0; round < 2000; round++) {
        let polled;
        try {
            polled = await inst.poll(link, hello, tests, { timeout: 150, retries: 1 });
        } catch (e) {
            if (inst.isBusy(e)) {
                busy++;         // a write under way: the port is quiet, and the unit is still there
                continue;
            }
            assert.equal(e.kind, 'link');
            await sleep(1600 * TICK + 50);
            link = await bridge.reconnect();
            assert.ok(await inst.installerHello(link), 'the installer is back after the restart');
            continue;
        }
        const s = polled.state;
        const d = inst.describeInstall(s, hello, tests);
        if (!seen.length || seen[seen.length - 1].phase !== d.phase.name)
            seen.push({ phase: d.phase.name, lines: d.lines.filter((l) => !l.startsWith('Holding')),
                advice: d.advice, desc: d });
        if (end.includes(d.phase.name))
            return { seen, last: d, link, hello, tests, busy };
        const what = acts[d.phase.name];
        const key = d.phase.name + ':' + s.trial;
        if (what && !done.has(key) && !d.hold) {
            done.add(key);
            await bridge.act(what);
        }
        await sleep(20);
    }
    throw new Error('the installer did not reach ' + end.join(' or '));
}

// 'starting' lasts a moment, and a poll may or may not meet it
const phases = (r) => r.seen.map((x) => x.phase).filter((p) => p !== 'starting');
const at = (r, name) => r.seen.find((x) => x.phase === name);

test('detect: the installer answers IHELLO, recovery falls back to HELLO', needPython, () => withBridge(async (b) => {
    let link = await b.unit({ kind: 'installer', case: 'pass', tick: TICK });
    let r = await inst.detect(link);
    assert.equal(r.kind, 'installer');
    assert.equal(r.hello.tests, 18);
    // the installer answers recovery's HELLO with "unknown"
    const u = await link.request(core.HELLO);
    assert.deepEqual(Array.from(u.payload), [0xFF]);
    link = await b.unit({ kind: 'model', front: 14, slots: [{ make: { seed: 1 } }, { make: { seed: 2 } }],
        sel: [1, 1], running: 1 });
    r = await inst.detect(link);
    assert.equal(r.kind, 'recovery');
    assert.equal(r.hello.front_version, 14);
}));

test('pass: checks, trial, restart, commit, done', needPython, () => withBridge(async (b) => {
    const r = await walk(b, 'pass', { ready: 'hold', 'trial written': 'restart', 'commit ready': 'hold' },
        ['done']);
    assert.deepEqual(phases(r), ['checking', 'guided', 'ready', 'writing trial', 'trial written',
        'commit ready', 'committing', 'done']);
    assert.deepEqual(at(r, 'ready').lines, ['Every check passed. The trial goes into Slot 2.',
        'Nothing is written until you hold the key on the unit.']);
    assert.deepEqual(at(r, 'ready').advice, ['Hold 1 on the unit for 5 s to write the trial.', inst.LEAVE]);
    assert.ok(r.busy > 0, 'the port was quiet while the unit wrote');
    assert.deepEqual(at(r, 'trial written').advice[0], 'Switch off, then on: the new bootloader starts once.');
    assert.deepEqual(at(r, 'commit ready').lines, ['The trial start reached the boot menu.']);
    assert.equal(r.last.tone, 'ok');
    assert.deepEqual(r.last.lines, ['Done: the new bootloader is kept in Slot 2.',
        'The OS is active again: switch the unit off and on and it starts its OS.']);
    assert.equal(r.last.verdict.text, 'PASS');
    assert.ok(r.last.checks.every((c) => c.stateText === 'pass' && c.detail));
    assert.equal(r.last.starts, 'The selector names Slot 2, settled.');
    const text = await inst.readReport(r.link);
    assert.ok(text.length > 1024, 'check.txt took more than one chunk');
    assert.match(text, /^waffle-boot installer check\nverdict: PASS\n/);
    assert.match(text, /\nkeys +pass +passed before the trial, not asked again\n/);
    // switched off and on after the end: the OS starts, and no installer answers
    await b.act('restart');
    await sleep(1600 * TICK + 50);
    const after = await b.reconnect();
    await assert.rejects(inst.installerHello(after, { timeout: 150, retries: 1 }), { kind: 'link' });
}));

test('check-fail: a check fails, the install is locked, the OS is active again', needPython, () => withBridge(async (b) => {
    const r = await walk(b, 'check-fail', { locked: 'leave' }, ['left']);
    assert.deepEqual(phases(r), ['checking', 'guided', 'locked', 'left']);
    const d = at(r, 'locked').desc;
    assert.equal(d.tone, 'bad');
    assert.equal(d.verdict.text, 'FAIL');
    assert.deepEqual(d.lines, ['A check did not pass: nothing was written, and the install is not offered.',
        'Why: a check failed.', 'The installer is still the active OS: the unit starts it again at the next start.']);
    assert.deepEqual(d.advice, [inst.LOCK_ADVICE[1], inst.SAVE_REPORT, inst.LEAVE]);
    const failed = d.checks.filter((c) => c.stateText === 'FAIL');
    assert.deepEqual(failed.map((c) => [c.id, c.name, c.detail]),
        [['emmc-volume', 'eMMC volume', 'the volume does not mount']]);
    assert.equal(failed[0].note, 'mount returned an I/O error on the second read');
    assert.equal(d.os.text, 'found and checked');
    assert.deepEqual(r.last.lines, ['Nothing written: the installer was left without installing.',
        'The OS is active again: switch the unit off and on and it starts its OS.']);
    assert.equal(r.last.os.text, 'made active again');
}));

test('trial-fail: the trial start did not reach the boot menu', needPython, () => withBridge(async (b) => {
    const r = await walk(b, 'trial-fail', { ready: 'hold', 'trial written': 'restart' }, ['trial failed']);
    assert.deepEqual(phases(r), ['checking', 'guided', 'ready', 'writing trial', 'trial written',
        'trial failed']);
    const d = r.last;
    assert.equal(d.tone, 'bad');
    assert.deepEqual(d.lines, ['The trial failed: the trial start ran but did not reach the boot menu.',
        'Nothing was kept. The selector names Slot 1, settled.']);
    assert.deepEqual(d.advice, ['Hold 1 on the unit for 5 s to try again.',
        inst.SAVE_REPORT, inst.LEAVE]);
    assert.deepEqual(d.slots[1].words, ['holds this image for a trial', 'trial started', 'trial spent']);
}));

test('no-os: the OS was not found, so the install is locked', needPython, () => withBridge(async (b) => {
    const r = await walk(b, 'no-os', {}, ['locked']);
    const d = r.last;
    assert.equal(d.lock.code, 5);
    assert.equal(d.verdict.text, 'PASS');
    assert.deepEqual(d.lines, ['A check did not pass: nothing was written, and the install is not offered.',
        "Why: the unit's OS was not found, or fails its check.",
        'The installer is still the active OS: the unit starts it again at the next start.']);
    assert.deepEqual(d.advice, [inst.LOCK_ADVICE[5], inst.SAVE_REPORT]);
    assert.equal(d.leave, false, 'no leave: there is no OS to make active');
    assert.deepEqual(d.checks[0].detail, 'no OS in the other boot partition');
}));

test('write-fail: the trial write fails', needPython, () => withBridge(async (b) => {
    const r = await walk(b, 'write-fail', { ready: 'hold' }, ['failed']);
    assert.deepEqual(phases(r).slice(-3), ['ready', 'writing trial', 'failed']);
    assert.deepEqual(r.last.lines, ['A write failed: a read-back did not match what was written.',
        'The selector names Slot 1, settled.',
        'The installer is still the active OS: the unit starts it again at the next start.']);
    assert.ok(r.last.advice.includes(inst.LEAVE));
}));

test('keys-ended: the key check ended with 8 locks the install (lock 6)', needPython, () => withBridge(async (b) => {
    const r = await walk(b, 'keys-ended', {}, ['locked']);
    const d = r.last;
    assert.equal(d.lock.code, 6);
    assert.equal(d.verdict.text, 'PASS WITH GAPS');
    assert.equal(d.lines[1], 'Why: the key check (com and M4) was ended with 8.');
    assert.equal(d.advice[0], 'Start the installer again and press com, then M4 (4 under the display), on the unit.');
    assert.deepEqual(d.checks.filter((c) => c.kind === 2).map((c) => [c.id, c.stateText]),
        [['keys', 'not tested']]);
}));

// --- the boot menu's developer link: the installer file sent as tools/testos_mon.py dev-put sends it

// frames from testos_mon.frame(): route 0x21, seq, id 0x7D, length, payload, CRC-8; COBS; 00
const DEV = {
    open1: '0621017d0819086f732e6c6472c800',        // PUT open "os.ldr", seq 1
    close3: '0821037d021902b200',                    // PUT close, seq 3
    sha4: '0d21047d071a6f732e6c6472cd00',            // SHA "os.ldr", seq 4
    // a reply of route 0x12, seq 9: status 0, then 01..27
    reply9: '0512097d28290102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20212223242526271800',
};
const enc = (s) => new TextEncoder().encode(s);
const cat = (...p) => Uint8Array.from(p.flatMap((x) => Array.from(x)));

test('the developer link frames as testos_mon.py does', () => {
    assert.equal(hexOf(inst.devFrame(cat([25, 0], enc('os.ldr')), 1)), DEV.open1);
    assert.equal(hexOf(inst.devFrame(Uint8Array.of(25, 2), 3)), DEV.close3);
    assert.equal(hexOf(inst.devFrame(cat([26], enc('os.ldr')), 4)), DEV.sha4);
    const w = unhex(DEV.reply9);
    const got = inst.devUnframe(w.subarray(0, w.length - 1));
    assert.equal(got.id, 0x7D);
    assert.deepEqual(Array.from(got.payload), [0, ...Array.from({ length: 39 }, (_, k) => k + 1)]);
    // COBS across a 254-byte run, and back
    const long = Uint8Array.from({ length: 600 }, (_, k) => (k % 300 === 0 ? 0 : k & 0xFF || 1));
    assert.deepEqual(Array.from(inst.cobsDecode(inst.cobsEncode(long))), Array.from(long));
    w[6] ^= 1;
    assert.equal(inst.devUnframe(w.subarray(0, w.length - 1)), null, 'a bad CRC-8');
    assert.equal(inst.devUnframe(Uint8Array.of(0)), null);
    assert.deepEqual([inst.DEV_PUT, inst.DEV_SHA, inst.PUT_CHUNK, inst.DEV_ID, inst.DEV_ROUTE],
        [25, 26, 240, 0x7D, 0x21]);
});

test('a file that is not an LDR stream is refused before anything is sent', () => {
    const ldr = Uint8Array.from([0x04, 0x80, 0x00, 0xAD, ...Array(12).fill(0), 1, 2, 3]);
    let x = 0;
    for (let k = 0; k < 16; k++)
        x ^= ldr[k];
    ldr[2] = x;
    assert.deepEqual(inst.checkInstallerFile(ldr).problems, []);
    assert.deepEqual(inst.checkInstallerFile(ldr, 'other.ldr').notes, [inst.devOtherName('other.ldr')]);
    assert.deepEqual(inst.checkInstallerFile(new Uint8Array(0)).problems, [inst.DEV_EMPTY]);
    assert.deepEqual(inst.checkInstallerFile(enc('PK\x03\x04 a zip, not a boot stream')).problems,
        [inst.DEV_NOT_LDR]);
    const bad = ldr.slice();
    bad[5] ^= 1;
    assert.deepEqual(inst.checkInstallerFile(bad).problems, [inst.DEV_NOT_LDR], 'header checksum');
    const big = new Uint8Array(inst.LDR_MAX + 1);
    big.set(ldr);
    assert.deepEqual(inst.checkInstallerFile(big).problems, [inst.devTooLarge(inst.LDR_MAX + 1)]);
});

const dev = spawnSync('python3', ['-I', '-B', here('devlink_model.py'), 'cases'],
    { encoding: 'utf8', maxBuffer: 64 << 20 });
const DEV_CASES = !needPython.skip && dev.status === 0 ? JSON.parse(dev.stdout) : [];
const OUTCOME = { good: 'verified', mismatch: 'mismatch', drop: 'lost', 'dev-off': 'refused' };
const TOOL_EXIT = { good: 0, mismatch: 1, drop: 'no answer from the monitor: is the unit hung? power-cycle it',
    'dev-off': 'monitor: refused' };

test('devlink cases came from testos_mon.py', needPython, () => {
    assert.equal(dev.status, 0, dev.stderr);
    assert.deepEqual(DEV_CASES.map((c) => c.case), ['good', 'good', 'good', 'mismatch', 'drop', 'dev-off']);
});

for (const [n, c] of DEV_CASES.entries()) {
    test(`devPut, ${c.case} (${c.file.length / 2} bytes): the wire bytes are testos_mon.py dev-put's`,
        needPython, () => withBridge(async (b) => {
            await b.unit({ kind: 'devlink', case: c.case });
            const link = new inst.DevLink(b.transport);
            const stages = [];
            let last = 0;
            const r = await inst.devPut(link, unhex(c.file), 'waffle-install.ldr', {
                timeout: 250, shaTimeout: 1000, onStage: (s) => stages.push(s),
                onProgress: (sent, total) => {
                    assert.ok(sent > last && sent <= total);
                    last = sent;
                },
            });
            const st = await b.call({ op: 'state' });
            assert.equal(st.rx, c.rx, 'byte for byte what the tool sent');
            assert.equal(c.exit, TOOL_EXIT[c.case], `the tool's own verdict (${n})`);
            assert.equal(r.outcome, OUTCOME[c.case]);
            assert.equal(r.sha256, c.sha256);
            if (c.case === 'good') {
                assert.deepEqual(stages, ['open', 'sending', 'close', 'sha']);
                assert.equal(last, c.file.length / 2);
                assert.equal(r.unitSha256, c.sha256);
                assert.deepEqual(r.lines, [inst.DEV_VERIFIED]);
                assert.deepEqual(r.advice, inst.DEV_VERIFIED_ADVICE);
                assert.match(r.advice[0], /press 5, Write \/os\.ldr to boot partition/);
                assert.match(c.printed, new RegExp(`sha256 ${r.unitSha256}: verified`));
            } else if (c.case === 'mismatch') {
                assert.notEqual(r.unitSha256, r.sha256);
                assert.equal(r.unitSize, c.file.length / 2);
                assert.match(c.printed, new RegExp(`sha256 ${r.unitSha256}: MISMATCH`));
                assert.deepEqual(r.advice, [inst.DEV_DONT_WRITE, inst.DEV_AGAIN]);
                assert.match(r.advice[0], /^Do not press 5/);
            } else if (c.case === 'drop') {
                assert.equal(r.sent, 4 * inst.PUT_CHUNK, 'the four chunks answered');
                assert.deepEqual(r.lines, [inst.DEV_NO_ANSWER]);
                assert.deepEqual(r.advice, [inst.DEV_PARTIAL]);
                assert.equal(r.unitSha256, null);
            } else {
                assert.equal(r.status, 1);
                assert.deepEqual(r.lines, [inst.DEV_REFUSED[1]]);
                assert.deepEqual(r.advice, [], 'refused at the open: nothing on the unit');
            }
        }));
}

test('devPut: a refused file sends nothing', needPython, () => withBridge(async (b) => {
    await b.unit({ kind: 'devlink', case: 'good' });
    const link = new inst.DevLink(b.transport);
    const r = await inst.devPut(link, enc('not a boot stream at all, sixteen bytes and more'));
    assert.equal(r.outcome, 'not-a-file');
    assert.deepEqual(r.lines, [inst.DEV_NOT_LDR]);
    await sleep(100);
    const st = await b.call({ op: 'state' });
    assert.equal(st.rx, '');
    assert.deepEqual(st.files, {});
}));
