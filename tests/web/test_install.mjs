// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tests of docs/web/install.js, the installer's protocol and what the page says about it:
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
import { readFileSync } from 'node:fs';
import * as core from '../../docs/web/core.js';
import * as inst from '../../docs/web/install.js';

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
        pin: 'a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf', device: 0, how: 0,
        imageDevice: 0, survey: false });
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
        verified: true, breadcrumb: false, survey: false, device: 'OP-XY' });
    assert.deepEqual(d.device, { name: 'OP-XY', identified: true, text: 'OP-XY' });
    assert.equal(d.target, 'Slot 2');
    assert.deepEqual(d.hold, { key: 1, progress: 0.64 });
    assert.deepEqual(d.slots[1].words, ['holds this image for a trial', 'trial started',
        'trial reached the boot menu']);
    assert.deepEqual(d.lines, ['The trial start reached the boot menu.', 'Holding 1: 64 %.']);
    assert.deepEqual(d.advice, ['Hold 1 on the unit to install waffle-boot.', inst.RETRIAL,
        inst.LEAVE]);
    assert.equal(inst.describeInstall({ ...s, os: 6 }).leave, false, 'leave only while the OS can be made active');
    assert.equal(d.checks[18].stateText, 'FAIL');
    // each lock reason and failure code has words, and unknown ones are named
    for (const r of [1, 2, 3, 4, 7, 8, 9, 10, 11])
        assert.ok(inst.LOCK[r] && inst.LOCK_ADVICE[r]);
    for (const r of [inst.LOCK_RESERVED, inst.LOCK_RESERVED_6])
        assert.ok(/never sets/.test(inst.LOCK[r]) && !inst.LOCK_ADVICE[r]);
    assert.equal(inst.failureText(0x42), 'failure code 0x42');
    assert.equal(inst.describeInstall({ ...s, phase: 11, trial: 4 }).lines[0],
        'The trial failed: the trial start did not run the new bootloader.');
    assert.equal(inst.describeInstall({ ...s, phase: 3, lock: 12, os: 6 }).lines[1], 'Why: lock reason 12.');
});

// STATE's lock codes as installer.h's enum inst_lock numbers them, each with the page's words
test('the lock codes are the header\'s', () => {
    const h = readFileSync(here('../../src/include/waffle/board/installer.h'), 'utf8');
    const body = h.match(/enum inst_lock \{([^}]*)\}/)[1];
    const names = [...body.matchAll(/INST_LOCK_([A-Z0-9_]+)/g)].map((m) => m[1]);
    assert.equal(names[0], 'NONE');
    assert.equal(names.indexOf('RESERVED'), inst.LOCK_RESERVED);
    assert.equal(names.indexOf('RESERVED_6'), inst.LOCK_RESERVED_6);
    assert.equal(names.indexOf('SURVEY'), inst.LOCK_SURVEY);
    assert.equal(inst.LOCK_NONE, 0);
    assert.deepEqual(Object.keys(inst.LOCK).map(Number), names.slice(1).map((_, k) => k + 1));
    assert.match(inst.lockText(inst.LOCK_RESERVED), /never sets/);
    assert.equal(inst.LOCK_ADVICE[inst.LOCK_RESERVED], undefined);
    assert.equal(inst.lockText(2), 'an automatic check could not run');
});

// The Install tab sends to an OP-XY only: an OP-1F is refused before anything is sent
test('an OP-1 Field is told apart from an OP-XY', () => {
    assert.equal(inst.isOp1f({ name: 'teenage engineering OP-XY', vendorId: 0x2367, productId: 0x0021 }), false);
    assert.equal(inst.isOp1f({ name: 'a USB device', vendorId: 0x2367, productId: 0x0102 }), true);
    assert.equal(inst.isOp1f({ name: 'a USB device', vendorId: 0x2367, productId: 0x8102 }), true);
    assert.equal(inst.isOp1f({ name: 'teenage engineering OP-1 Field', vendorId: 0x2367, productId: 0x1234 }), true);
    assert.equal(inst.isOp1f({ name: 'OP-12', vendorId: 0x2367, productId: 0x1234 }), false);
    assert.equal(inst.isOp1f(null), false);
    assert.match(inst.NOT_FOR_1F[0], /Nothing was sent/);
    assert.ok(inst.NOT_FOR_1F[1].includes(inst.HOW_TO_INSTALL_1F));
    assert.match(inst.HOW_TO_INSTALL_1F, /How-to-Install#on-an-op-1-field$/);
});

// S-18: HELLO's device bytes, the survey's reply and its phase, the OP-1F's key check
test('the device in HELLO, the survey, and the one-CPU words', () => {
    const p = one(IHELLO_REPLY).payload.slice();
    p.set([1, 0, 1, 1], inst.IHELLO_O_DEVICE);
    const h = inst.parseIHello(p);
    assert.deepEqual([h.device, h.how, h.imageDevice, h.survey], [1, 0, 1, true]);
    const s = { ...inst.parseState(one(STATE_REPLY).payload, 20), phase: inst.GUIDED, holdKey: 0 };
    assert.match(inst.describeInstall(s, h).advice[0], /the com key, then the recovery key/);
    assert.match(inst.describeInstall(s, inst.parseIHello(one(IHELLO_REPLY).payload)).advice[0], /com, then M4/);
    const sv = inst.describeInstall({ ...s, phase: inst.SURVEYED, lock: 11, os: 6 }, h);
    assert.equal(sv.tone, 'ok');
    assert.deepEqual(sv.lines, [inst.SURVEY_DONE, 'An install by the installer would be offered on this unit.',
        inst.OS_BACK]);
    assert.match(inst.describeInstall({ ...s, phase: inst.SURVEYED, lock: 10, os: 3 }, h).lines[1],
        /^An install would not be offered: the trial would write over/);
    p.set([0, 2], inst.IHELLO_O_DEVICE);
    const u = inst.describeInstall({ ...s, phase: inst.LOCKED, lock: 8 }, inst.parseIHello(p));
    assert.deepEqual(u.device, { name: null, identified: false, text: 'not identified' });
    assert.ok(u.lines.includes(inst.NOT_IDENTIFIED));
    assert.equal(inst.testState(5), 'n/a');
    assert.equal(inst.failureText(0x46), "trial refused: it would write over the unit's own bootloader");
    // the survey's reply: 76 bytes, little-endian
    const b = new Uint8Array(inst.SURVEY_LEN);
    const dv = new DataView(b.buffer);
    b.set([0, 1, 0, 1]);
    dv.setUint32(4, 34304, true);
    dv.setUint32(8, 0x49267EB8, true);
    b.set([1, 0, 1, 0xFF], 12);
    dv.setUint32(16, 0x1D5A000, true);
    dv.setUint32(20, 4096, true);
    b.set([1, 0x48, 6], 24);
    b[52] = 0x0F;
    dv.setUint32(52 + 8, 0x5A3C0, true);
    b[52 + 23] = 0xC3;
    const r = inst.parseSurvey(b);
    assert.deepEqual([r.status, r.device, r.how, r.bootMode, r.n, r.c], [0, 1, 0, 1, 34304, 0x49267EB8]);
    assert.deepEqual([r.selRead, r.selIndex, r.selVerified, r.pa2, r.secCount, r.bootKiB, r.extRead,
        r.partConfig, r.os], [true, 0, 1, 0xFF, 0x1D5A000, 4096, true, 0x48, 6]);
    assert.deepEqual([r.slots[1].bits, r.slots[1].length, r.slots[1].log[7]], [0x0F, 0x5A3C0, 0xC3]);
    assert.deepEqual(inst.parseSurvey(Uint8Array.of(0xFF)), { status: 0xFF });
    assert.throws(() => inst.parseSurvey(b.subarray(0, 40)), core.Failure);
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
        // an act per phase, or a list of them, one for each time the phase is entered
        const visits = seen.filter((x) => x.phase === d.phase.name).length;
        const what = Array.isArray(acts[d.phase.name]) ? acts[d.phase.name][visits - 1] : acts[d.phase.name];
        const key = d.phase.name + ':' + s.trial + ':' + visits;
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
    assert.deepEqual(at(r, 'ready').lines, ['Checks passed. The trial goes into Slot 2.',
        'Nothing is written until you hold the key on the unit.']);
    assert.deepEqual(at(r, 'ready').advice, ['Hold 1 on the unit to write the trial.', inst.LEAVE]);
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

test('retrial: 2 on the commit screen writes the same trial again, then the commit', needPython, () => withBridge(async (b) => {
    const r = await walk(b, 'pass', { ready: 'hold', 'trial written': 'restart',
        'commit ready': ['retrial', 'hold'] }, ['done']);
    assert.deepEqual(phases(r), ['checking', 'guided', 'ready', 'writing trial', 'trial written',
        'commit ready', 'writing trial', 'trial written', 'commit ready', 'committing', 'done']);
    const commits = r.seen.filter((x) => x.phase === 'commit ready');
    assert.ok(commits.every((x) => x.advice.includes(inst.RETRIAL)));
    assert.deepEqual(r.last.lines, ['Done: the new bootloader is kept in Slot 2.',
        'The OS is active again: switch the unit off and on and it starts its OS.']);
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
    assert.deepEqual(r.last.lines, ['Nothing written: returned to the old OS without installing.',
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
    assert.deepEqual(d.advice, ['Hold 1 on the unit to write the trial again.',
        inst.SAVE_REPORT, inst.LEAVE]);
    assert.deepEqual(d.slots[1].words, ['holds this image for a trial', 'trial started', 'trial spent']);
}));

test('no-os: no OS in the other partition does not lock the install, and there is no leaving', needPython,
    () => withBridge(async (b) => {
    const r = await walk(b, 'no-os', {}, ['ready']);
    const d = r.last;
    assert.equal(d.lock, null);
    assert.equal(d.verdict.text, 'PASS');
    assert.ok(!d.advice.includes(inst.LEAVE));
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

test('keys-ended: 8 held in the key check returns to the old OS, with no install offered', needPython,
    () => withBridge(async (b) => {
    const r = await walk(b, 'keys-ended', { guided: 'leave' }, ['left']);
    assert.deepEqual(phases(r), ['checking', 'guided', 'left']);
    assert.ok(at(r, 'guided').advice.includes(inst.LEAVE), 'the key check offers 8');
    const d = r.last;
    assert.equal(d.lock, null);
    assert.equal(d.verdict.text, 'PASS WITH GAPS');
    assert.deepEqual(d.lines, ['Nothing written: returned to the old OS without installing.',
        'The OS is active again: switch the unit off and on and it starts its OS.']);
    assert.deepEqual(d.checks.filter((c) => c.kind === 2).map((c) => [c.id, c.stateText]),
        [['keys', 'not tested']]);
}));
