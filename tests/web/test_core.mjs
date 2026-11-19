// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tests of docs/web/core.js against tools/wbrecover.py and the simulated units of
// tests/tools/test_wbrecover.py: its Model, and the front's own code in a shared library.
//
//     node --test tests/web/test_core.mjs
//
// gen_cases.py runs the Python tool through a table of scenarios and gives what it printed,
// asked, sent and left in the flash; unit_bridge.py puts the same units on a socket. The tests
// here check core.js's tables, framing, parsers and image checks against the Python tool's, its
// plans against the tool's decisions on the same INFO, and its runs over the socket against the
// tool's runs, line for line and by the flash afterwards. Without python3 the tests that need
// it are skipped, and without a C compiler the cases on the front's own code.

import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';
import net from 'node:net';
import * as core from '../../docs/web/core.js';

const here = (name) => fileURLToPath(new URL(name, import.meta.url));
const unhex = (s) => Uint8Array.from(s.match(/../g) || [], (b) => parseInt(b, 16));
const TIMES = { timeout: 400, retries: 4, longTimeout: 1200, longRetries: 3, poll: 1, deadline: 20000 };

// --- the Python side ---

const python = spawnSync('python3', ['--version']);
const havePython = !python.error && python.status === 0;
let gen = null;
if (havePython) {
    const r = spawnSync('python3', ['-I', '-B', here('gen_cases.py')],
        { maxBuffer: 1 << 30, encoding: 'utf8' });
    if (r.status !== 0)
        throw new Error(`gen_cases.py failed:\n${r.stderr}`);
    gen = JSON.parse(r.stdout);
}
const needPython = havePython ? {} : { skip: 'python3 is not installed' };
const needShim = !havePython ? needPython
    : gen.shim ? {} : { skip: "no C compiler: the cases on the front's own code are left out" };

const images = {};
async function imageOf(key) {
    if (!images[key])
        images[key] = await core.checkImage(new Uint8Array(Buffer.from(gen.images[key].b64, 'base64')));
    return images[key];
}
const make = (kw = {}) => {
    const sorted = {};
    for (const k of Object.keys(kw).sort())
        sorted[k] = kw[k];
    return `{"make": ${JSON.stringify(sorted).replace(/,/g, ', ').replace(/":/g, '": ')}}`;
};

// A simulated unit's byte stream as core.js's transport.
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

// unit_bridge.py: its control channel, and a connection to the unit it serves.
class Bridge {
    static async start() {
        const b = new Bridge();
        b.proc = spawn('python3', ['-I', '-B', here('unit_bridge.py')],
            { stdio: ['pipe', 'pipe', 'inherit'] });
        b.lines = createInterface({ input: b.proc.stdout })[Symbol.asyncIterator]();
        const first = await b.next();
        b.path = first.socket;
        b.shim = first.shim;
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

    // A new unit on a new connection, and a Link to it.
    async unit(spec, times = TIMES) {
        if (this.transport)
            await this.transport.close();
        const sock = net.connect(this.path);
        await new Promise((resolve, reject) => {
            sock.once('connect', resolve);
            sock.once('error', reject);
        });
        this.transport = new SocketTransport(sock);
        await this.call({ op: 'unit', spec });
        return new core.Link(this.transport, times);
    }

    set(attrs) {
        return this.call({ op: 'set', set: attrs });
    }

    async state() {
        const s = await this.call({ op: 'state' });
        delete s.ok;
        return s;
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

// --- one step of a case, as the Python tool and as core.js take it ---

const outLines = (step) => step.events.filter((e) => e[0] === 'out').map((e) => norm(e[1]));
const asked = (step) => step.events.filter((e) => e[0] === 'ask').map((e) => {
    const m = /^Type "(.*)" to go on: $/.exec(e[1]);
    return m ? m[1] : 'yes';
});
// what the tool says of the file's pin comes from its checkout's git history, which a page has not
const norm = (line) => line.replace(/^( {2}running: front .* file: pin [0-9a-f]{16}\.\.\.), .*$/, '$1');
const flat = (confs) => confs.flatMap((c) => c.lines.map(norm));

function planOf(info, step, image) {
    if (step.cmd === 'write')
        return core.planWrite(info, image, { mode: step.mode, force: step.force });
    if (step.cmd === 'select')
        return core.planSelect(info, image, { force: step.force });
    if (step.cmd === 'restart')
        return core.planRestart(info, { force: step.force });
    return core.planAct(info, step.cmd, { force: step.force });
}

function runOf(link, plan, step, image, hooks) {
    if (step.cmd === 'write')
        return core.runWrite(link, plan, image, hooks);
    if (step.cmd === 'select')
        return core.runSelect(link, plan, image, hooks);
    if (step.cmd === 'restart')
        return core.runRestart(link, plan, hooks);
    return core.runAct(link, plan, hooks);
}

const isFinal = (c) => c.way !== undefined;     // the warning that goes with BEGIN's answer

// The step with core.js over the link: every line it says, the phrases it asks for, the result.
async function take(link, step, image) {
    const { info } = await core.connect(link);
    const plan = planOf(info, step, image);
    if (!plan.allowed)
        return { plan, lines: plan.said.map(norm), phrases: [], ok: false, result: null };
    const afterBegin = (step.cmd === 'write' || step.cmd === 'select') && info.v6;
    const shown = plan.confirmations.filter((c) => !(afterBegin && isFinal(c)));
    let sending = false, final = null;
    const result = await runOf(link, plan, step, image, {
        onStage: (name) => {
            sending = sending || name === 'sending';
        },
        confirm: async (c) => {
            final = c;
            return true;
        },
    });
    if (afterBegin && final === null && sending)
        final = plan.confirmations.find(isFinal);
    if (final)
        shown.push(final);
    return { plan, result, lines: plan.said.map(norm).concat(flat(shown), result.said.map(norm)),
        phrases: shown.map((c) => c.phrase).filter(Boolean),
        ok: result.outcome === 'verified' || result.outcome === 'done' };
}

function infoOf(step) {
    const info = core.parseInfo(unhex(step.info));
    info.front = core.parseHello(unhex(step.hello)).front_version;
    return info;
}

function plain(v) {
    if (v instanceof Uint8Array)
        return core.hex(v);
    if (Array.isArray(v))
        return v.map(plain);
    if (v && typeof v === 'object')
        return Object.fromEntries(Object.entries(v).map(([k, x]) => [k, plain(x)]));
    return v;
}

const stepName = (c, step) =>
    `${c.name}: ${step.cmd}${step.mode ? ' ' + step.mode : ''}${step.force ? ' forced' : ''}`;

// --- tables, frames, parsers, images ---

test('the tables and constants are the Python tool\'s', needPython, () => {
    const t = gen.tables;
    const byKey = (o) => Object.fromEntries(Object.entries(o).map(([k, v]) => [String(k), v]));
    assert.deepEqual(byKey(core.STATUS), t.STATUS);
    assert.deepEqual(byKey(core.IMAGE_RC), t.IMAGE_RC);
    assert.deepEqual(byKey(core.WHY), t.WHY);
    assert.deepEqual(byKey(core.WRITE_RISK), t.WRITE_RISK);
    assert.deepEqual(byKey(core.SELECT_RISK), t.SELECT_RISK);
    assert.deepEqual(byKey(core.TIER_WORDS), t.TIER_WORDS);
    for (const [name, value] of Object.entries(t.constants))
        assert.deepEqual(core[name], value, name);
    for (const [st, text] of Object.entries(t.status_text))
        assert.equal(core.statusText(Number(st)), text);
    for (const [st, text] of Object.entries(t.refusal_text))
        assert.equal(core.refusalText(Number(st)), text);
    for (const [b, want] of t.battery)
        assert.deepEqual(core.battery(unhex(b)), want, b);
    assert.deepEqual(core.LAYOUT, t.layout);
    assert.equal(core.forcePhrase('write'), 'force the write');
});

test('frames', needPython, () => {
    for (const [type, seq, payload, wire] of gen.tables.frames) {
        const f = core.frame(type, seq, unhex(payload));
        assert.equal(core.hex(f), wire);
        const rx = { bytes: f };
        assert.deepEqual(plain(core.takeFrame(rx)), { type, seq: seq & 0xFF, payload });
        assert.equal(rx.bytes.length, 0);
    }
    assert.equal(core.crc32(new TextEncoder().encode('123456789')), 0xCBF43926);
    assert.equal(core.crc32(new TextEncoder().encode('6789'), core.crc32(new TextEncoder().encode('12345'))),
        0xCBF43926);
    // noise, a frame with a bad CRC, a frame cut short, then two good ones in one buffer
    const a = core.frame(core.INFO, 7, Uint8Array.of(1, 2, 3)), b = core.frame(core.HELLO, 8);
    const bad = core.frame(core.DATA, 1, Uint8Array.of(9));
    bad[bad.length - 1] ^= 0xFF;
    const rx = { bytes: Uint8Array.from([0x57, 0x00, 0x52, ...bad, ...a, ...b, ...a.subarray(0, 5)]) };
    assert.deepEqual(plain(core.takeFrame(rx)), { type: core.INFO, seq: 7, payload: '010203' });
    assert.deepEqual(plain(core.takeFrame(rx)), { type: core.HELLO, seq: 8, payload: '' });
    assert.equal(core.takeFrame(rx), null);
    assert.equal(core.hex(rx.bytes), core.hex(a.subarray(0, 5)));
    // a stated length over the largest payload is not waited for
    const long = { bytes: Uint8Array.from([0x57, 0x52, 4, 1, 0xFF, 0xFF, ...b]) };
    assert.deepEqual(plain(core.takeFrame(long)), { type: core.HELLO, seq: 8, payload: '' });
});

test('the zeros before the first request put a parser back in step, whatever was cut off',
    needPython, () => {
        const f = unhex(gen.tables.stale_frame), hello = core.frame(core.HELLO, 1);
        assert.equal(f.length, core.FRAME_MAX);
        for (let k = 0; k < f.length; k += k < 16 || k > f.length - 16 ? 1 : 37) {
            const rx = {
                bytes: Uint8Array.from([...f.subarray(0, k), ...new Uint8Array(core.FRAME_MAX), ...hello]),
            };
            const got = [];
            for (let g = core.takeFrame(rx); g !== null; g = core.takeFrame(rx))
                got.push(plain(g));
            assert.deepEqual(got, [{ type: core.HELLO, seq: 1, payload: '' }], `cut at ${k}`);
        }
    });

test('checkImage agrees with the Python tool on every image', needPython, async () => {
    for (const [name, want] of Object.entries(gen.images)) {
        const image = await imageOf(name);
        assert.deepEqual(image.problems, want.problems, name);
        assert.equal(core.hex(image.sha256), want.sha256, name);
        assert.equal(image.crc32, want.crc32, name);
        assert.deepEqual(image.front === null ? null : plain(image.front), want.front, name);
        assert.deepEqual(image.lines, want.check, name);
        assert.equal(image.ok, want.rc === 0, name);
    }
});

test('checkImage: good, bad CRC, too large, no front, another front, foreign', needPython, async () => {
    const good = await imageOf(make({ seed: 1 }));
    assert.ok(good.ok);
    assert.equal(good.version, '1.2.3+4');
    assert.equal(good.pin, core.hex(good.front.pin_calc));
    assert.equal(good.frontVersion, null);
    assert.match(good.lines[0], new RegExp('^OK: \\d+ bytes, a bootable stream with a waffle-boot' +
        ' front; version 1\\.2\\.3\\+4'));
    const known = await core.checkImage(good.bytes, { knownPins: { [good.pin]: 14 } });
    assert.equal(known.frontVersion, 14);

    const badCrc = await imageOf(make({ front_crc_ok: false }));
    assert.ok(!badCrc.ok && badCrc.noFrontOnly);
    assert.deepEqual(badCrc.problems,
        ["no waffle-boot front: the front's bytes do not match the descriptor's CRC"]);

    // S-02.104: one byte over 0x76000, and recovery's own 0x77000, are refused alike
    for (const key of ['{"make": {"seed": 9}, "of": 483344}', '{"make": {}, "of": 487425}']) {
        const tooLarge = await imageOf(key);
        assert.ok(!tooLarge.ok && !tooLarge.noFrontOnly);
        assert.match(tooLarge.problems[0], new RegExp('a waffle-boot image is at most 483328 \\(0x76000\\) ' +
            "bytes, since the slot's last two sectors hold its settings and its start log"));
    }
    const overSlot = await imageOf('{"make": {}, "of": 491521}');
    assert.match(overSlot.problems[0], /a slot image is 1 to 491520 bytes/);
    const empty = await imageOf('{"hex": ""}');
    assert.match(empty.problems[0], /^the file is 0 bytes/);

    const atLimit = await imageOf('{"make": {"seed": 9}, "of": 483328}');
    assert.ok(atLimit.ok);
    assert.deepEqual(atLimit.warnings, []);

    const foreign = await imageOf('{"foreign": 1}');
    assert.ok(!foreign.ok && foreign.noFrontOnly && foreign.front === null);
    assert.match(foreign.problems[0], /^no waffle-boot front: 0 descriptor blocks/);

    const other = await imageOf(make({ front: 2, seed: 7, size: 3000 }));
    assert.ok(other.ok);
    assert.notEqual(other.pin, good.pin);

    const badPin = await imageOf(make({ pin_ok: false }));
    assert.ok(!badPin.ok && badPin.problems.length === 0 && !badPin.front.pin_ok);
    assert.match(badPin.lines[0], /descriptor claims front pin .* but its front's own bytes give/);

    const noEntry = await imageOf(make({ entry: 0x08031000 }));
    assert.deepEqual(noEntry.problems, ["no waffle-boot front: the entry is not in the front's L2 window"]);
    const junk = await core.checkImage(new TextEncoder().encode('not a stream at all '.repeat(4)));
    assert.match(junk.problems[0], /^stream rule parse failed: bad signature/);
    for (const name of Object.keys(gen.images).filter((n) => n.startsWith('{"hex"') && n.length > 400))
        assert.ok((await imageOf(name)).problems.some((p) => p.startsWith('no waffle-boot front: a ')), name);
});

test('HELLO and INFO parse as the Python tool parses them, for every front', needPython, () => {
    const fronts = new Set(), shimFronts = new Set();
    let n = 0;
    for (const c of gen.cases) {
        for (const step of c.steps.filter((s) => s.cmd)) {
            const info = infoOf(step);
            (c.name.startsWith('shim-') ? shimFronts : fronts).add(info.front);
            const want = { ...step.parsed };
            assert.deepEqual(plain(info), want, c.name);
            assert.deepEqual(core.describe(info).lines, step.describe, c.name);
            n++;
        }
    }
    // the Model's fronts, and the front's own code at its version (FR_FRONT_VERSION)
    assert.deepEqual([...fronts].sort((a, b) => a - b), [5, 6, 9, 11, 14]);
    assert.equal(shimFronts.size, gen.shim ? 1 : 0);
    assert.ok(n > 500);
    assert.throws(() => core.parseInfo(new Uint8Array(198)), /INFO reply too short/);
    assert.throws(() => core.parseHello(new Uint8Array(46)), /HELLO reply too short/);
});

test('describe gives INFO as data', needPython, () => {
    const step = (name, k = 0) => gen.cases.find((c) => c.name === name).steps.filter((s) => s.cmd)[k];
    let d = core.describe(infoOf(step('model-f14-info')));
    assert.equal(d.running, 0);
    assert.deepEqual(d.selector, { read: true, slot: 0, verified: 1, settled: true });
    assert.deepEqual(d.slots.map((s) => [s.name, s.version, s.state, s.kind, s.running, s.selected,
        s.frontMatches]), [['Slot 1', '1.2.3+4', 'checked', 'WB 1.2.3+4', true, true, true],
        ['Slot 2', '0.9.0', 'checked', 'WB 0.9.0', false, false, true]]);
    assert.deepEqual(d.commands.write, { allowed: true, target: 1, code: 0,
        text: 'would go to Slot 2, replacing a checked image (version 0.9.0)' });
    assert.equal(d.commands.running.way, core.WAY_SELECT_FIRST);
    assert.deepEqual(d.commands.try, { allowed: true, forceWouldLift: false, slot: 1, code: 0,
        text: 'would start Slot 2 once, at the next start' });
    assert.equal(d.commands.switch.allowed, true);
    assert.equal(d.commands.keep.allowed, false);
    assert.deepEqual(d.commands.restart, { allowed: true, forceWouldLift: false, text: 'the unit restarts' });
    assert.equal(d.phase.name, 'idle');
    assert.ok(!d.damaged && !d.held);
    assert.equal(d.battery.state, 'unknown');

    d = core.describe(infoOf(step('model-f5-info')));
    assert.equal(d.battery, null);
    assert.equal(d.commands.running, null);
    assert.equal(d.commands.try, null);

    d = core.describe(infoOf(step('model-f14-keep-not-started')));
    assert.equal(d.slots[1].state, 'tried');
    assert.deepEqual([d.commands.keep.allowed, d.commands.keep.forceWouldLift, d.commands.keep.code],
        [false, true, 21]);
    d = core.describe(infoOf(step('model-f14-keep')));
    assert.equal(d.slots[1].state, 'tried, started');
    assert.ok(d.stateLines[0].startsWith('Slot 2 is tried and its front marked its trial start'));

    d = core.describe(infoOf(step('model-f14-restart-held')));
    assert.ok(d.damaged && d.held);
    assert.deepEqual(d.commands.restart.allowed, false);
    assert.equal(d.commands.restart.forceWouldLift, true);
    assert.ok(d.stateLines.some((l) => l.startsWith('Ways out: ')));
    assert.ok(d.stateLines.some((l) => l.startsWith("The unit holds the power switch's power-off")));

    d = core.describe(infoOf(step('model-f11-switch-foreign')));
    assert.equal(d.tier, core.TIER_FOREIGN);
    assert.equal(d.slots[1].frontMatches, false);
    assert.equal(d.slots[1].version, null);
});

// --- the decisions against the Python tool's, on the same INFO ---

test('each plan is the Python tool\'s decision on the same INFO', needPython, async () => {
    let n = 0, refused = 0;
    for (const c of gen.cases) {
        for (const step of c.steps.filter((s) => s.cmd && s.cmd !== 'info')) {
            const where = stepName(c, step);
            const info = infoOf(step);
            const image = step.image ? await imageOf(step.image) : null;
            const plan = planOf(info, step, image);
            const lines = outLines(step);
            const went = step.replies.length > 0;           // BEGIN, ACT or RESTART went out
            assert.equal(plan.allowed, went, where);
            n++;
            if (!plan.allowed) {
                refused++;
                assert.deepEqual(plan.said.map(norm), lines, where);
                assert.ok(plan.refusal.text.length > 0 && plan.request === null, where);
                assert.equal(step.rc, 1, where);
                continue;
            }
            assert.equal(plan.refusal, null, where);
            assert.deepEqual(lines.slice(0, plan.said.length), plan.said.map(norm), where);
            // the request as the tool sent it: the same flags, and the unit's answer as planned
            const [type, reply] = step.replies[0];
            const r = unhex(reply);
            const flags = plan.request.length === 37 ? plan.request[36] : plan.request.length === 2
                ? plan.request[1] : plan.request.length === 1 ? plan.request[0] : 0;
            assert.equal(!!(flags & 0x80), plan.force, where);
            assert.equal(type, { write: core.BEGIN, select: core.BEGIN, restart: core.RESTART }[step.cmd]
                || core.ACT, where);
            if (r[0] !== 0 || type === core.RESTART || r.length < 3)
                continue;
            if (plan.target === null) {
                assert.ok(plan.force && plan.way === null, where);
                continue;
            }
            if (r[1] !== plan.way || r[2] !== plan.target)
                continue;                                   // the unit announced another write
            // what the tool printed up to its last prompt is the plan's lines, and its prompts
            // the plan's phrases
            const last = step.events.map((e) => e[0]).lastIndexOf('ask');
            const before = step.events.slice(0, last + 1).filter((e) => e[0] === 'out').map((e) => norm(e[1]));
            const phrases = plan.confirmations.map((p) => p.phrase).filter(Boolean);
            if (plan.way === core.WAY_IN_PLACE && c.name.startsWith('shim-')) {
                // the battery report comes with BEGIN: the tool's lines carry it
                assert.deepEqual(asked(step), phrases, where);
                continue;
            }
            assert.deepEqual(asked(step), phrases, where);
            if (last >= 0)
                assert.deepEqual(before, plan.said.map(norm).concat(flat(plan.confirmations)), where);
            assert.equal(plan.phrase, phrases.length ? phrases[0] : null, where);
        }
    }
    assert.ok(n > 600 && refused > 200, `${n} plans, ${refused} refused`);
});

test('the phrases, the dangers and what force lifts', needPython, async () => {
    const step = (name, k = 0) => gen.cases.find((c) => c.name === name).steps
        .filter((s) => s.cmd && s.cmd !== 'info')[k];
    const plan = async (name, k = 0) => {
        const s = step(name, k);
        return planOf(infoOf(s), s, s.image ? await imageOf(s.image) : null);
    };
    let p = await plan('model-f14-write-good');
    assert.deepEqual([p.action, p.allowed, p.phrase, p.target, p.way, p.countdown, p.danger, p.force],
        ['write', true, 'yes', 1, core.WAY_PLAIN, true, 'caution', false]);
    assert.ok(p.lines.includes('Slot 2 holds a checked image (version 0.9.0), which this write replaces.'));
    p = await plan('model-f14-write-empty');
    assert.deepEqual([p.phrase, p.danger], ['yes', 'none']);
    p = await plan('model-f6-write-good');
    assert.equal(p.countdown, false);
    p = await plan('model-f5-write-empty');
    assert.equal(p.phrase, null);               // a version 5 front asks only when a good image goes
    p = await plan('model-f14-running-other-good');
    assert.deepEqual([p.phrase, p.target, p.way, p.danger], ['yes', 0, core.WAY_SELECT_FIRST, 'caution']);
    p = await plan('model-f14-running-in-place');
    assert.deepEqual([p.phrase, p.target, p.way, p.danger],
        ['erase the running slot in place', 0, core.WAY_IN_PLACE, 'no-fallback']);
    assert.ok(p.lines.some((l) => l.includes('cannot be repaired without a flash programmer')));
    assert.ok(p.lines.includes('Battery: present and charged (charging, 40 %, 4000 mV).'));
    p = await plan('model-f14-running-other-bad');
    assert.deepEqual([p.allowed, p.refusal.code, p.forceWouldLift], [false, 15, true]);
    assert.match(p.refusal.text, new RegExp('^Refused: the other slot failed its check, and an in-place write .*' +
        ' Nothing was sent\\.$'));
    p = await plan('model-f14-running-other-bad', 1);
    assert.deepEqual([p.allowed, p.phrase, p.target, p.way, p.danger, p.force],
        [true, 'force the write', null, null, 'no-fallback', true]);
    p = await plan('model-f9-running-other-bad', 1);
    assert.deepEqual([p.allowed, p.refusal.code, p.forceWouldLift], [false, 'front-too-old', false]);
    p = await plan('model-f9-running-other-bad');
    assert.deepEqual([p.refusal.code, p.forceWouldLift], [15, false]);
    p = await plan('model-f14-write-new-front');
    assert.deepEqual([p.phrase, p.target, p.way, p.danger],
        ['try the new front', 1, core.WAY_TRIAL, 'caution']);
    p = await plan('model-f14-write-other-front');
    assert.deepEqual([p.allowed, p.refusal.code, p.forceWouldLift], [false, 'other-front', true]);
    p = await plan('model-f14-write-other-front', 1);
    assert.deepEqual(p.confirmations.map((c) => c.phrase), ['force the write', 'yes']);
    p = await plan('model-f14-force-not-needed');
    assert.deepEqual([p.allowed, p.force, p.phrase, p.options.force], [true, false, 'yes', true]);
    assert.ok(p.said[0].startsWith('--force is not needed'));
    p = await plan('model-f14-try');
    assert.deepEqual([p.action, p.phrase, p.target, p.way, p.danger], ['try', 'yes', 1, core.WAY_TRY, 'none']);
    p = await plan('model-f14-switch');
    assert.deepEqual([p.phrase, p.danger], ['make it the boot slot', 'caution']);
    p = await plan('model-f14-switch-bad', 2);
    assert.deepEqual([p.phrase, p.danger, p.force], ['force the switch', 'no-fallback', true]);
    p = await plan('model-f14-keep');
    assert.deepEqual([p.action, p.phrase, p.target], ['keep', 'yes', 1]);
    p = await plan('model-f14-keep-not-started', 0);
    assert.deepEqual([p.allowed, p.refusal.code, p.forceWouldLift], [false, 21, true]);
    p = await plan('model-f14-keep-not-started', 1);
    assert.equal(p.phrase, 'force the keep');
    p = await plan('model-f14-put-back');
    assert.deepEqual([p.action, p.phrase], ['put-back', 'yes']);
    p = await plan('model-f14-select');
    assert.deepEqual([p.action, p.phrase, p.target, p.way], ['select', 'yes', 1, core.WAY_SELECT]);
    p = await plan('model-f14-select-not-started', 1);
    assert.deepEqual(p.confirmations.map((c) => c.phrase), ['force the select', 'yes']);
    p = await plan('model-f14-restart');
    assert.deepEqual([p.allowed, p.phrase, p.countdown, p.danger], [true, null, false, 'none']);
    p = await plan('model-f14-restart-held', 0);
    assert.deepEqual([p.allowed, p.refusal.code, p.forceWouldLift], [false, 0x44, true]);
    p = await plan('model-f14-restart-held', 1);
    assert.deepEqual([p.phrase, p.danger, core.hex(p.request)], ['force the restart', 'no-fallback', '80']);
    // a page that knows the signed front pins says so in the trial's warning
    const st = step('model-f14-write-new-front');
    const other = await imageOf(st.image);
    const trial = (knownPins) => core.planWrite(infoOf(st), other, { mode: 'new-front', knownPins });
    assert.ok(trial(null).lines.includes('Make sure of where the file comes from before going on.'));
    p = trial({ [other.pin]: 14 });
    assert.ok(p.allowed && p.lines.some((l) => l.endsWith("a signed pin, by this page's list (front v14)")));
    assert.ok(!p.lines.some((l) => l.startsWith('A front without the trial mark')));
    p = trial({});
    assert.ok(p.allowed &&
        p.lines.some((l) => l.startsWith('WARNING: that front is not among the front pins')));
    p = trial({ [other.pin]: 6 });
    assert.deepEqual([p.allowed, p.refusal.code], [false, 'old-front-file']);
    p = await plan('model-f9-try');
    assert.equal(p.refusal.text,
        'This unit runs front version 9: try needs a version 11 front. Nothing was sent.');
});

test('identical slots: accepted from front 14, refused before sending on front 11', needPython, async () => {
    const step = (name) => gen.cases.find((c) => c.name === name).steps.filter((s) => s.cmd)[0];
    for (const name of ['write-kept-image', 'write-identical-own']) {
        let s = step(`model-f11-${name}`);
        let p = planOf(infoOf(s), s, await imageOf(s.image));
        assert.deepEqual([p.allowed, p.refusal.code], [false, 13], name);
        assert.equal(p.refusal.text, 'Refused before sending: ' + core.statusText(13), name);
        assert.match(p.refusal.text, new RegExp('a front before version 14 refuses this; ' +
            'from version 14 both slots may hold the same image'));
        s = step(`model-f14-${name}`);
        p = planOf(infoOf(s), s, await imageOf(s.image));
        assert.ok(p.allowed, name);
    }
});

// --- the runs over the socket against the Python tool's runs ---

async function runCases(t, cases, workers = 4) {
    let at = 0, steps = 0;
    const failures = [];
    await Promise.all(Array.from({ length: workers }, () => withBridge(async (bridge) => {
        while (at < cases.length) {
            const c = cases[at++];
            let where = c.name;
            try {
                const link = await bridge.unit(c.unit);
                for (const step of c.steps) {
                    if (step.set) {
                        await bridge.set(step.set);
                        continue;
                    }
                    where = stepName(c, step);
                    if (step.cmd === 'info') {
                        await core.connect(link);
                        const { hello, info } = await core.connect(link);
                        assert.deepEqual([`Front version ${hello.front_version}, protocol ${hello.proto}.`]
                            .concat(core.describe(info).lines), outLines(step), where);
                    } else {
                        const got = await take(link, step, step.image ? await imageOf(step.image) : null);
                        assert.deepEqual(got.lines, outLines(step), where);
                        assert.deepEqual(got.phrases, asked(step), where);
                        assert.equal(got.ok, step.rc === 0, where);
                        if (got.result && got.result.next.length)
                            assert.ok(got.result.next.every((l) => got.result.said.includes(l)), where);
                    }
                    const state = await bridge.state();
                    for (const k of ['requests', 'phases', 'batt_asks'])
                        delete state[k];
                    assert.deepEqual(state, step.state, where);
                    steps++;
                }
            } catch (e) {
                failures.push(`${where}: ${e.message}`);
            }
        }
    })));
    t.diagnostic(`${cases.length} cases, ${steps} commands`);
    assert.deepEqual(failures.slice(0, 5), [], `${failures.length} cases differ`);
    return steps;
}

test('runs against the Model (fronts 5, 6, 9, 11, 14) say and leave what the Python tool does',
    needPython, async (t) => {
        const steps = await runCases(t, gen.cases.filter((c) => c.name.startsWith('model-')));
        assert.ok(steps > 500);
    });

test("runs against the front's own code say and leave what the Python tool does", needShim, async (t) => {
    // one flash and one recovery in the library: one case at a time
    const steps = await runCases(t, gen.cases.filter((c) => c.name.startsWith('shim-')), 1);
    assert.ok(steps > 80);
});

// --- scenarios by name: what a page relies on ---

const A = { make: {} }, B = { make: { seed: 2, version: [0, 9, 0, 0] } };
const NEW = make({ seed: 7, size: 3000 });
const model = (front, extra = {}) => ({ kind: 'model', front, slots: [A, B], ...extra });
const shim = (extra = {}) => ({ kind: 'shim', slots: [A, B], good: [true, true], ...extra });
const types = (state) => state.requests.map((r) => r[0]);
const yes = { confirm: async () => true };

test('a plain write: stages, progress, the countdown, the flash', needPython, async () => {
    await withBridge(async (bridge) => {
        for (const spec of [model(6), model(11), model(14), ...(bridge.shim ? [shim()] : [])]) {
            const link = await bridge.unit(spec);
            const { hello, info } = await core.connect(link);
            const image = await imageOf(NEW);
            const plan = core.planWrite(info, image);
            const stages = [], counts = [], sent = [], infos = [];
            let writing = 0;
            const r = await core.runWrite(link, plan, image, {
                onStage: (s) => stages.push(s),
                onProgress: (done, total) => (total === 1000 ? writing++ : sent.push([done, total])),
                onCountdown: (left) => counts.push(left),
                onInfo: (i) => infos.push(i.phase),
            });
            const what = `${spec.kind} ${hello.front_version}`;
            assert.equal(r.outcome, 'verified', what + ': ' + r.text);
            assert.equal(r.code, 0);
            assert.match(r.text, new RegExp('^Verified: Slot 2 holds this file \\(\\d+ bytes, SHA-256' +
                ' [0-9a-f]{64}\\), read back by the unit\\.$'));
            assert.match(r.next[0], new RegExp('^The selector now names Slot 2, to be checked at the' +
                ' next start\\. Restart'));
            assert.equal(r.info.phase, core.DONE);
            assert.ok(core.sameBytes(r.info.readback, image.sha256));
            assert.deepEqual(sent.at(-1), [image.length, image.length]);
            assert.deepEqual(sent.map((s) => s[0]),
                [1024, 2048, 3072, image.length].filter((n) => n <= image.length));
            assert.ok(writing > 0);
            const counted = hello.front_version >= 11;
            assert.equal(plan.countdown, counted);
            assert.deepEqual(stages, counted
                ? ['hello', 'begin', 'sending', 'end', 'countdown', 'writing', 'done']
                : ['hello', 'begin', 'sending', 'end', 'writing', 'done'], what);
            if (counted) {
                assert.equal(counts[0], 15);
                assert.equal(counts.at(-1), 1);
                assert.deepEqual(counts, [...counts].sort((a, b) => b - a));
                assert.ok(r.said.includes(core.COUNTDOWN_LINE));
            } else {
                assert.deepEqual(counts, []);
            }
            const state = await bridge.state();
            assert.deepEqual(state.sel, [1, 0]);
            if (spec.kind === 'model')
                assert.equal(state.slots[1], core.hex(image.sha256));
            else
                assert.equal(state.heads[1].slice(0, image.length * 2), core.hex(image.bytes));
            assert.deepEqual(types(state).slice(0, 6),
                [core.HELLO, core.INFO, core.HELLO, core.INFO, core.BEGIN, core.INFO]);
        }
    });
});

test('an abort stops this side only', needPython, async () => {
    await withBridge(async (bridge) => {
        const image = await imageOf(NEW);
        // in the transfer: END is never sent, and the flash is as it was
        let link = await bridge.unit(model(11));
        let plan = core.planWrite((await core.connect(link)).info, image);
        let stop = new AbortController();
        let before = await bridge.state();
        let r = await core.runWrite(link, plan, image, {
            signal: stop.signal, onProgress: (done) => done >= 2048 && stop.abort() });
        assert.deepEqual([r.outcome, r.code], ['stopped', 'aborted']);
        assert.match(r.text, /nothing was written/);
        let state = await bridge.state();
        assert.ok(!types(state).includes(core.END));
        assert.deepEqual([state.sel, state.slots], [before.sel, before.slots]);
        // the link is still good for the next request
        assert.equal((await core.connect(link)).info.phase, core.RECEIVING);

        // in the countdown: the result says what does stop the unit, and the unit goes on
        link = await bridge.unit(model(11));
        plan = core.planAct((await core.connect(link)).info, 'try');
        stop = new AbortController();
        r = await core.runAct(link, plan,
            { signal: stop.signal, onCountdown: (left) => left === 12 && stop.abort() });
        assert.deepEqual([r.outcome, r.code], ['stopped', 'aborted']);
        assert.match(r.text, new RegExp('the unit was not stopped\\. ' +
            'Its countdown is stopped only by switching the unit off or unplugging USB'));
        let info = (await core.connect(link)).info;
        assert.equal(info.phase, core.COUNTING);
        for (let k = 0; k < 40 && info.phase !== core.DONE; k++)
            info = await core.getInfo(link);
        assert.equal(info.phase, core.DONE);
        assert.deepEqual((await bridge.state()).sel, [0xFF, 0xFF]);

        // before anything was sent
        link = await bridge.unit(model(11));
        plan = core.planWrite((await core.connect(link)).info, image);
        stop = new AbortController();
        stop.abort();
        r = await core.runWrite(link, plan, image, { signal: stop.signal });
        assert.deepEqual([r.outcome, r.code], ['stopped', 'aborted']);
        assert.deepEqual(types(await bridge.state()), [core.HELLO, core.INFO]);
    });
});

test('a plan that no longer holds, a confirmation declined, a forced write with no one to ask',
    needPython, async () => {
        await withBridge(async (bridge) => {
            const image = await imageOf(NEW);
            let link = await bridge.unit(model(14));
            let plan = core.planWrite((await core.connect(link)).info, image);
            await bridge.set({ sel: [0, 0] });
            let r = await core.runWrite(link, plan, image, yes);
            assert.deepEqual([r.outcome, r.code], ['refused', 4]);
            assert.ok(!r.plan.allowed);
            assert.ok(!types(await bridge.state()).includes(core.BEGIN));

            // the other slot loses its image between the plan and the run: another warning
            link = await bridge.unit(model(14));
            plan = core.planWrite((await core.connect(link)).info, image);
            await bridge.set({ slots: [A, null] });
            r = await core.runWrite(link, plan, image, yes);
            assert.deepEqual([r.outcome, r.code], ['stopped', 'changed']);
            assert.ok(r.plan.allowed && r.plan.lines.join(' ') !== plan.lines.join(' '));
            assert.ok(!types(await bridge.state()).includes(core.BEGIN));

            // BEGIN announces another write than the plan's: the page is asked, and says no
            link = await bridge.unit(model(14, { slots: [A, null], batt: '02281900000fa0' }));
            plan = core.planWrite((await core.connect(link)).info, image, { mode: 'running', force: true });
            assert.equal(plan.target, null);
            let asked = null;
            r = await core.runWrite(link, plan, image, { confirm: async (c) => {
                asked = c;
                return false;
            } });
            assert.deepEqual([r.outcome, r.code], ['stopped', 'declined']);
            assert.deepEqual([asked.phrase, asked.way, asked.target, asked.danger],
                ['erase the running slot in place', core.WAY_IN_PLACE, 0, 'no-fallback']);
            assert.ok(asked.lines.some((l) => l.startsWith('DANGER: Slot 2 holds no checked image')));
            let state = await bridge.state();
            assert.ok(types(state).includes(core.BEGIN) && !types(state).includes(core.END));
            assert.equal(state.requests.find((q) => q[0] === core.BEGIN)[1].slice(72), '81');

            // the same with no confirm hook: stopped, nothing sent beyond BEGIN
            link = await bridge.unit(model(14, { slots: [A, null] }));
            plan = core.planWrite((await core.connect(link)).info, image, { mode: 'running', force: true });
            r = await core.runWrite(link, plan, image, {});
            assert.deepEqual([r.outcome, r.code], ['stopped', 'changed']);
            assert.match(r.text, /no image byte was sent/);
            assert.ok(!types(await bridge.state()).includes(core.END));

            // and said yes: written in place
            link = await bridge.unit(model(14, { slots: [A, null] }));
            plan = core.planWrite((await core.connect(link)).info, image, { mode: 'running', force: true });
            r = await core.runWrite(link, plan, image, yes);
            assert.equal(r.outcome, 'verified', r.text);
            assert.equal((await bridge.state()).slots[0], core.hex(image.sha256));
        });
    });

test('the link: a damaged reply, a late one, no answer, the port gone', needPython, async () => {
    await withBridge(async (bridge) => {
        const image = await imageOf(NEW);
        const quick = { ...TIMES, timeout: 150, longTimeout: 250 };
        // a reply with a bad CRC is not taken: the request goes out again
        let link = await bridge.unit(model(11, { set: { corrupt: 1 } }), quick);
        let got = await link.request(core.HELLO);
        assert.equal(got.attempt, 2);
        assert.equal(core.parseHello(got.payload).front_version, 11);
        // a reply that comes after its attempt's time is dropped, not taken for the next request
        await bridge.set({ late: { [core.INFO]: 0.35 } });
        const asking = link.request(core.INFO);
        await new Promise((resolve) => setTimeout(resolve, 100));
        await bridge.set({ late: {} });
        got = await asking;
        assert.equal(got.attempt, 2);
        assert.equal(core.parseInfo(got.payload).phase, core.IDLE);
        assert.equal(core.parseHello((await link.request(core.HELLO)).payload).front_version, 11);

        // no answer at all
        link = await bridge.unit(model(11, { set: { unanswered: [core.INFO] } }), quick);
        let plan = core.planRestart(infoOf(gen.cases.find((c) => c.name === 'model-f11-restart').steps[0]));
        let r = await core.runRestart(link, plan, {});
        assert.deepEqual([r.outcome, r.code], ['lost', 'link']);
        assert.match(r.text, /^FAILED: no answer from the unit after 3 tries: is it still in recovery\?/);
        await assert.rejects(link.request(core.INFO), (e) => e instanceof core.Failure && e.kind === 'link');

        // RESTART's reply lost: the unit most likely restarted
        link = await bridge.unit(model(11, { set: { unanswered: [core.RESTART] } }), quick);
        plan = core.planRestart((await core.connect(link)).info);
        r = await core.runRestart(link, plan, {});
        assert.deepEqual([r.outcome, r.code], ['done', 'reply-lost']);
        assert.match(r.text, /the unit most likely restarted/);

        // the port goes away in the transfer
        link = await bridge.unit(model(11, { set: { hangup_after: 1 } }), quick);
        plan = core.planWrite((await core.connect(link)).info, image);
        r = await core.runWrite(link, plan, image, {});
        assert.deepEqual([r.outcome, r.code], ['lost', 'link']);
        assert.match(r.text, new RegExp('^FAILED: the port went away: ' +
            'was the unit restarted or unplugged\\? The transfer stopped at byte 1024 of \\d+, ' +
            'before END: nothing was written\\.'));

        // the port goes away in the countdown: the result says what the unit does then
        link = await bridge.unit(model(11), quick);
        plan = core.planAct((await core.connect(link)).info, 'try');
        r = await core.runAct(link, plan,
            { onCountdown: (left) => left === 10 && bridge.set({ gone: true }) });
        assert.deepEqual([r.outcome, r.code], ['lost', 'link']);
        assert.match(r.text, new RegExp('The unit was counting down: ' +
            'if it was switched off or USB was unplugged, it stopped, and nothing was written ' +
            '\\(status 24\\); else it goes on when the countdown ends\\. Run info'));

        // a reply shorter than its type carries
        link = await bridge.unit(model(11, { set: { cut: { [core.BEGIN]: 2 } } }), quick);
        plan = core.planWrite((await core.connect(link)).info, image);
        r = await core.runWrite(link, plan, image, {});
        assert.deepEqual([r.outcome, r.code], ['failed', 'protocol']);
        assert.match(r.text, /BEGIN's reply is 2 bytes, short of the 3 it carries/);
    });
});

test('a protocol the tool does not speak is refused at HELLO', needPython, async () => {
    const hello = unhex(gen.cases[0].steps[0].hello);
    hello[0] = 2;                                   // front 5 claiming protocol 2
    const transport = {
        sent: [],
        async write(b) {
            this.sent.push(b);
        },
        async read() {
            const last = this.sent.at(-1);
            if (!last || last.length > 64 || this.answered === last)
                return null;
            this.answered = last;
            return core.frame(last[2] | core.REPLY, last[3], hello);
        },
        async close() {},
    };
    const link = new core.Link(transport, TIMES);
    await assert.rejects(core.hello(link), /speaks recovery protocol 2 with front version 5/);
    assert.equal(transport.sent[0].length, core.FRAME_MAX);       // the zeros, once
    assert.ok(transport.sent[0].every((b) => b === 0));
});

test("the front's own code: a failed erase, then the running slot in place", needShim, async () => {
    await withBridge(async (bridge) => {
        const image = await imageOf(NEW);
        const link = await bridge.unit(shim({ good: [true, false], batt: '02281900000fa0' }));
        let info = (await core.connect(link)).info;
        let plan = core.planWrite(info, image, { mode: 'running' });
        assert.deepEqual([plan.allowed, plan.refusal.code, plan.forceWouldLift], [false, 15, true]);
        await bridge.set({ fail: [0x88000, 0x100000] });
        plan = core.planWrite(info, image);
        let r = await core.runWrite(link, plan, image, yes);
        assert.deepEqual([r.outcome, r.code], ['failed', 9]);
        assert.match(r.text, /^FAILED: a flash erase, program or read of the write failed/);
        await bridge.set({ fail: [0, 0] });
        info = (await core.connect(link)).info;
        plan = core.planWrite(info, image, { mode: 'running' });
        assert.deepEqual([plan.allowed, plan.phrase, plan.way, plan.danger],
            [true, 'erase the running slot in place', core.WAY_IN_PLACE, 'no-fallback']);
        let shown = null;
        r = await core.runWrite(link, plan, image, { confirm: async (c) => {
            shown = c;
            return true;
        } });
        assert.equal(r.outcome, 'verified', r.text);
        // the battery report the unit asked for at BEGIN is in the warning that was confirmed
        assert.ok(shown.lines.includes('Battery: present and charged (charging, 40 %, 4000 mV).'));
        assert.equal(shown.phrase, 'erase the running slot in place');
        const state = await bridge.state();
        assert.deepEqual(state.sel, [0, 1]);
        assert.equal(state.heads[0].slice(0, image.length * 2), core.hex(image.bytes));
        assert.equal(state.batt_asks, 1);
    });
});

test("the front's own code: a lost END, and the host leaving in the countdown", needShim, async () => {
    await withBridge(async (bridge) => {
        const image = await imageOf(NEW);
        let link = await bridge.unit(shim({ set: { lost: { [core.END]: 1 } } }));
        let plan = core.planWrite((await core.connect(link)).info, image);
        let r = await core.runWrite(link, plan, image, yes);
        assert.equal(r.outcome, 'verified', r.text);
        let state = await bridge.state();
        assert.equal(types(state).filter((t) => t === core.END).length, 2);
        assert.equal(state.heads[1].slice(0, image.length * 2), core.hex(image.bytes));

        link = await bridge.unit(shim());
        plan = core.planAct((await core.connect(link)).info, 'try');
        await bridge.set({ drop_after: 5 });
        const before = await bridge.state();
        r = await core.runAct(link, plan, {});
        assert.deepEqual([r.outcome, r.code], ['stopped', 24]);
        assert.match(r.text, /^Stopped: the write was stopped before its first erase/);
        state = await bridge.state();
        assert.deepEqual([state.sel, state.records], [before.sel, before.records]);
    });
});

// S-02.015's CRC-32, bit by bit.
function crc32(b) {
    let c = 0xFFFFFFFF;
    for (const x of b) {
        c ^= x;
        for (let k = 0; k < 8; k++)
            c = c & 1 ? (c >>> 1) ^ 0xEDB88320 : c >>> 1;
    }
    return (c ^ 0xFFFFFFFF) >>> 0;
}

// S-02.106: a slot's kind from INFO's slot fields and flag bit 5, against the oracle's synthetic vectors.
test('slot kinds follow S-02.106', async () => {
    const { readFileSync } = await import('node:fs');
    const W = await import('../../docs/web/words.js');
    const dir = new URL('../oracle/S-02-spi-flash/vectors/slot_kinds.json', import.meta.url);
    const { cases } = JSON.parse(readFileSync(dir, 'utf8'));
    assert.equal(cases.length, 15);
    for (const c of cases) {
        const data = new Uint8Array(c.length);
        for (let i = 0; i < c.length; i++)
            data[i] = (i * 37 + c.seed) & 0xFF;
        for (const p of c.patches)
            data.set(Buffer.from(p.bytes, 'hex'), p.offset);
        const crc = parseInt(c.crc32, 16);
        const prefix = c.length >= 4468 && crc32(data.subarray(0, 4468)) === 0x31D25812;
        for (const tried of [false, true]) {
            const s = { good: !tried, tried, started: false, has_desc: c.descriptor_valid,
                version: c.descriptor_version || [0, 0, 0, 0], length: c.length,
                crc: tried ? (~crc) >>> 0 : crc, te_prefix: prefix };
            assert.deepEqual(W.slotKind(s), [c.kind, c.version], `${c.name}, tried ${tried}`);
            if (c.text !== null)
                assert.equal(W.kindText(W.slotKind(s)), `TE ${c.text}`);
        }
    }
    const s = { good: true, tried: false, has_desc: false, version: [0, 0, 0, 0], length: 0x1234,
        crc: 0xCAFEF00D, te_prefix: false };
    assert.equal(W.kindText(W.slotKind(s)), '??');
    W.TE_BUILDS.push([0x1234, 0xCAFEF00D, [2, 1, 0, 7]]);      // a made-up row
    try {
        assert.equal(W.kindText(W.slotKind(s)), 'TE 2.1.0+7');
    } finally {
        W.TE_BUILDS.pop();
    }
    assert.equal(W.kindText(W.slotKind({ ...s, te_prefix: true })), 'TE');
    assert.equal(W.slotKind({ ...s, good: false }), null);
    assert.deepEqual(W.TE_BUILDS.map((r) => r[2].join('.')),
        ['1000.0.12.0', '1000.0.13.0', '1000.0.20.0', '1000.0.23.0']);
});
