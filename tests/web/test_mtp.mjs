// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tests of docs/web/mtp.js against tools/mtpcp.py and the responders of tests/tools/test_mtpcp.py:
// its writable fake with each of its faults, a read-only one, and waffle-boot's own responder.
//
//     node --test tests/web/test_mtp.mjs
//
// mtp_bridge.py puts the responders on a pipe. Each put runs twice on the same responder: once in
// the Python tool and once here, and every bulk OUT transfer the two send must be the same bytes,
// in the same order, with the same outcome. Without python3 only the tests on a scripted
// transport run, and without a C compiler the cases on waffle-boot's own responder are left out.

import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';
import * as mtp from '../../docs/web/mtp.js';

const here = (name) => fileURLToPath(new URL(name, import.meta.url));
const b64 = (b) => Buffer.from(b.buffer, b.byteOffset, b.byteLength).toString('base64');
const unb64 = (s) => new Uint8Array(Buffer.from(s, 'base64'));
const hex = (b) => Buffer.from(b.buffer, b.byteOffset, b.byteLength).toString('hex');

const python = spawnSync('python3', ['--version']);
const needPython = !python.error && python.status === 0 ? {} : { skip: 'python3 is not installed' };

const ALLOWED = new Set([0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1007, 0x1008, 0x1009, 0x100B, 0x100C,
    0x100D]);
const CHANGES = [0x100B, 0x100C, 0x100D];
const NEW = Uint8Array.from({ length: 293900 }, (_, k) => (k * 7) % 256);
const MTIME = 1794000000;

// --- the bridge ---

class Bridge {
    constructor() {
        this.proc = spawn('python3', ['-I', '-B', here('mtp_bridge.py')], { stdio: ['pipe', 'pipe', 'inherit'] });
        this.waiting = [];
        createInterface({ input: this.proc.stdout }).on('line', (line) => this.waiting.shift()(JSON.parse(line)));
    }

    ask(req) {
        return new Promise((resolve) => {
            this.waiting.push(resolve);
            this.proc.stdin.write(JSON.stringify(req) + '\n');
        });
    }

    close() {
        this.proc.stdin.end();
    }
}

let bridge = null;
const theBridge = () => bridge || (bridge = new Bridge());
test.after(() => bridge && bridge.close());

// The responder the bridge holds, as mtp.js's transport; every bulk OUT is kept.
class BridgeTransport {
    constructor(b, mps) {
        this.b = b;
        this.mps = mps;
        this.writes = [];
    }

    async write(bytes) {
        this.writes.push(hex(bytes));
        const r = await this.b.ask({ op: 'w', b: b64(bytes) });
        if (r.err)
            throw new Error(r.err);
    }

    async read(size) {
        const r = await this.b.ask({ op: 'r', n: size });
        return r.t ? null : unb64(r.b);
    }

    async close() {}
}

const enc = (files) => Object.fromEntries(Object.entries(files).map(([n, v]) =>
    [n, v === null ? null : b64(typeof v === 'string' ? new TextEncoder().encode(v) : v)]));

// Runs the Python tool's put, then mtp.js's, each on a new responder made from spec.
async function bothPut(spec, content = NEW, name = 'os.ldr') {
    const b = theBridge();
    const req = { ...spec, files: enc(spec.files || {}) };
    const py = await b.ask({ op: 'python_put', ...req, content: b64(content), name, mtime: MTIME });
    if (py.skip)
        return { skip: py.skip };
    const made = await b.ask({ op: 'new', ...req });
    assert.ok(made.ok);
    const t = new BridgeTransport(b, spec.mps || 512);
    const js = await mtp.put(t, content, { name, modified: py.modified, fileName: 'FILE' });
    const st = await b.ask({ op: 'state', get: ['/' + name] });
    return { py, js, writes: t.writes, st };
}

// What the Python tool printed, in mtp.js's words: no volume path, the device's version as a version.
const pyLines = (out) => out.trimEnd().split('\n').map((l) => l.replace(/\/yaffs2\//g, '')
    .replace(/, bootloader /, ', version ').replace(/^uploading \S+ as /, 'uploading FILE as '));

const sameWrites = (r) => {
    assert.equal(r.writes.length, r.py.writes.length, 'the number of bulk OUT transfers');
    r.writes.forEach((w, k) => assert.equal(w, Buffer.from(r.py.writes[k], 'base64').toString('hex'),
        `bulk OUT ${k}`));
};
const opsOk = (ops) => assert.ok(ops.every((o) => ALLOWED.has(o)), `only the allowed operations: ${ops}`);
const fileOf = (st, name = 'os.ldr') => st.files[name] === null ? null : unb64(st.files[name]);
const textOf = (st, name) => Buffer.from(st.files[name], 'base64').toString();

// --- byte-exact containers ---

const cmdOf = (w) => {
    const b = Buffer.from(w, 'hex');
    return b.length >= 12 && b.readUInt16LE(4) === 1 ? b.readUInt16LE(6) : null;
};

test('the containers are the Python tool\'s, byte for byte', needPython, async () => {
    const r = await bothPut({ files: { 'os.ldr': 'old image', 'lost+found': null, 'keep.wav': 'k' } });
    assert.equal(r.py.error, null);
    assert.equal(r.js.outcome, 'verified');
    sameWrites(r);
    assert.equal(r.js.lines[0], 'teenage engineering OP-XY, version 9.9.9');
    assert.deepEqual(r.js.lines, pyLines(r.py.out));
    // OpenSession with session 1, in transaction 2 after GetDeviceInfo; GetStorageIDs in 3
    assert.equal(r.writes[1], '10000000010002100200000001000000');
    assert.equal(r.writes[2], '0c0000000100041003000000');
    const at = r.writes.findIndex((w) => cmdOf(w) === 0x100C);
    assert.match(r.writes[at], /^1400000001000c10[0-9a-f]{8}01000100ffffffff$/);
    // the ObjectInfo dataset: storage, format 0x3000, size, then the name and dates
    const info = Buffer.from(r.writes[at + 1], 'hex');
    assert.equal(info.readUInt16LE(4), 2);
    assert.equal(info.readUInt32LE(12 + 8), NEW.length);
    assert.equal(hex(mtp.objectInfoDataset(0x10001, 'os.ldr', NEW.length, r.py.modified)),
        info.subarray(12).toString('hex'));
    const send = r.writes.findIndex((w) => cmdOf(w) === 0x100D);
    assert.match(r.writes[send], /^0c00000001000d10[0-9a-f]{8}$/);
    assert.equal(Buffer.from(r.writes[send + 1], 'hex').readUInt32LE(0), 12 + NEW.length);
    assert.deepEqual(fileOf(r.st), NEW);
    assert.equal(textOf(r.st, 'keep.wav'), 'k');
    assert.ok('lost+found' in r.st.files);
    opsOk(r.st.ops);
    assert.ok(r.js.lines.includes('deleted os.ldr (9 bytes)'));
    assert.ok(r.js.lines.some((l) => l.startsWith('verified os.ldr: 293900 bytes read back, identical')));
    // the read-back comes after a new session
    const ops = r.st.ops;
    assert.ok(ops.indexOf(0x100D) < ops.indexOf(0x1003));
    assert.ok(ops.indexOf(0x100D) < ops.lastIndexOf(0x1009));
});

// --- every fault of the Python tests' fake, against the Python tool ---

const CASES = [
    ['nothing to delete', {}, 'verified'],
    ['full speed', { mps: 64 }, 'verified', NEW.subarray(0, 5000)],
    ['a container of 500 bytes, ended by a zero-length packet', {}, 'verified', new Uint8Array(500)],
    ['a container of 1012 bytes', {}, 'verified', new Uint8Array(1012)],
    ['a container of 1524 bytes', {}, 'verified', new Uint8Array(1524)],
    ['a container of 65524 bytes', {}, 'verified', new Uint8Array(512 * 128 - 12)],
    ['an ObjectInfo longer than its length says', { files: { 'os.ldr': 'old' }, faults: { info_length_off: -10 } },
        'verified'],
    ['an ObjectInfo short of its length, ended by a short packet',
        { files: { 'os.ldr': 'old' }, faults: { info_length_off: 40 } }, 'verified'],
    ['a stale data container before a response', { files: { 'os.ldr': 'old' }, faults: { stale_after_list: true } },
        'verified', NEW, (r) => assert.ok(r.js.stale > 0)],
    ['the root as 0', { faults: { root_is_zero: true } }, 'verified'],
    ['a flipped byte on the write', { faults: { corrupt_write: true } }, 'mismatch', NEW,
        (r) => assert.ok(r.js.lines.some((l) => /offset 9\b/.test(l)))],
    ['a flipped byte on the read', { faults: { corrupt_read: true } }, 'mismatch'],
    ['a short file', { faults: { truncate: 65536 } }, 'mismatch', NEW,
        (r) => assert.ok(r.js.lines.some((l) => /listed 65536/.test(l)))],
    ['a successful GetObject with no bytes', { faults: { cannot_open: true } }, 'mismatch', NEW,
        (r) => assert.ok(r.js.lines.some((l) => /read back 0/.test(l)))],
    ['a refused SendObject', { faults: { send_refused: true } }, 'refused', NEW, (r) => {
        assert.equal(r.js.response, 'General_Error (0x2002)');
        assert.equal(r.js.text, 'The unit refused SendObject: it answered General_Error (0x2002).');
        assert.equal(fileOf(r.st).length, 0);
    }],
    ['a refused SendObjectInfo', { faults: { send_info_refused: true } }, 'refused'],
    ['a refused delete', { files: { 'os.ldr': 'old' }, faults: { delete_refused: true } }, 'refused'],
    ['a delete that is ignored', { files: { 'os.ldr': 'old' }, faults: { delete_ignored: true } }, 'failed', NEW,
        (r) => assert.match(r.js.text, /still listed/)],
    ['OK with no data phase', { files: { 'os.ldr': 'old' }, faults: { no_list_data: true } }, 'failed', NEW,
        (r) => assert.match(r.js.text, /no data phase/)],
    ['a folder of that name', { files: { 'os.ldr': null, other: '1' } }, 'failed', NEW, (r) => {
        assert.match(r.js.text, /recursive: nothing was changed/);
        assert.ok(!r.st.ops.includes(0x100B));
    }],
    ['no room', { files: { 'os.ldr': new Uint8Array(10) }, free: 1000 }, 'failed', NEW, (r) => {
        assert.match(r.js.text, /do not fit/);
        assert.ok(!CHANGES.some((o) => r.st.ops.includes(o)));
    }],
    ['room counts the file replaced', { files: { 'os.ldr': new Uint8Array(NEW.length) }, free: NEW.length + 5 },
        'verified'],
    ['only the named file goes', { files: { 'os.ldr': 'old', 'os.ldr.bak': 'keep', 'OS.LDR': 'keep' } },
        'verified', NEW, (r) => {
            assert.equal(textOf(r.st, 'os.ldr.bak'), 'keep');
            assert.equal(textOf(r.st, 'OS.LDR'), 'keep');
        }],
    ['a read-only store', { kind: 'readonly', files: { 'os.ldr': 'old' } }, 'readonly', NEW, (r) => {
        assert.equal(r.js.text, "The unit's storage is read-only: nothing was changed.");
        assert.ok(!CHANGES.some((o) => r.st.ops.includes(o)));
        assert.equal(textOf(r.st, 'os.ldr'), 'old');
        assert.equal(r.js.changed, false);
    }],
];

for (const [title, spec, want, content = NEW, more] of CASES)
    test(`put, as the Python tool: ${title}`, needPython, async () => {
        const r = await bothPut(spec, content);
        sameWrites(r);
        opsOk(r.st.ops);
        assert.equal(r.js.outcome, want, r.js.lines.join('\n'));
        assert.equal(r.py.error === null, want === 'verified', r.py.error || '');
        if (want === 'verified') {
            // the Python tool notes dropped containers after the command, outside put
            assert.deepEqual(r.js.lines.filter((l) => !l.startsWith('note: dropped')), pyLines(r.py.out));
            assert.deepEqual(fileOf(r.st), content);
            assert.equal(r.js.sha256, Buffer.from(await crypto.subtle.digest('SHA-256', content)).toString('hex'));
        } else {
            // the Python tool warns of a bad copy exactly when mtp.js says a change was sent
            assert.equal(/do NOT run the OS write/.test(r.py.error), r.js.changed);
            assert.equal(r.js.advice.some((a) => /not a good copy/.test(a)), r.js.changed);
        }
        if (more)
            await more(r);
    });

// --- waffle-boot's own responder ---

test('put and verify against waffle-boot\'s responder', needPython, async (t) => {
    const content = Uint8Array.from({ length: 70000 }, (_, k) => (k * 5) % 256);
    for (const mps of [512, 64]) {
        const r = await bothPut({ kind: 'engine', level: 1, mps,
            files: { 'os.ldr': 'old image', 'lost+found': null, 'keep.wav': 'k' } }, content);
        if (r.skip)
            return t.skip(r.skip);
        sameWrites(r);
        opsOk(r.st.ops);
        assert.equal(r.js.outcome, 'verified', r.js.lines.join('\n'));
        assert.deepEqual(unb64(r.st.get['/os.ldr']), content);
    }
    // level R: read only
    const r = await bothPut({ kind: 'engine', level: 0, files: { 'os.ldr': 'old' } }, content);
    sameWrites(r);
    assert.equal(r.js.outcome, 'readonly');
    assert.ok(!CHANGES.some((o) => r.st.ops.includes(o)));
    assert.equal(Buffer.from(unb64(r.st.get['/os.ldr'])).toString(), 'old');
    // verify, in every framing that needs a zero-length packet or not
    for (const mps of [512, 64])
        for (const size of [1, 52, 500, 1012, 1024, 16372]) {
            const body = Uint8Array.from({ length: size }, (_, k) => k % 251);
            await theBridge().ask({ op: 'new', kind: 'engine', mps, files: enc({ 'os.ldr': body }) });
            const v = await mtp.verify(new BridgeTransport(theBridge(), mps), body);
            assert.equal(v.outcome, 'verified', `${mps} ${size}: ${v.lines.join('\n')}`);
        }
    await theBridge().ask({ op: 'new', kind: 'engine', files: enc({ 'os.ldr': new Uint8Array(1000) }) });
    const bad = new Uint8Array(1000);
    bad[700] = 1;
    const v = await mtp.verify(new BridgeTransport(theBridge(), 512), bad);
    assert.equal(v.outcome, 'mismatch');
    assert.ok(v.lines.some((l) => /offset 700/.test(l)));
});

// --- the device going away, and a transfer that does not end ---

class Failing extends BridgeTransport {
    constructor(b, mps, after, how) {
        super(b, mps);
        this.after = after;
        this.how = how;
        this.sent = 0;
    }

    async write(bytes) {
        this.sent += bytes.length;
        if (this.sent > this.after)
            throw Object.assign(new Error('The device was disconnected.'), this.how);
        return super.write(bytes);
    }
}

test('the device going away during the copy', needPython, async () => {
    const b = theBridge();
    await b.ask({ op: 'new', files: enc({ 'os.ldr': 'old' }) });
    const r = await mtp.put(new Failing(b, 512, 100000, { gone: true }), NEW);
    assert.equal(r.outcome, 'lost');
    assert.equal(r.text, 'The unit went away during the copy.');
    assert.equal(r.changed, true);
    assert.match(r.advice[0], /not a good copy now: do not write it as the OS/);
    assert.ok(r.lines.some((l) => /USB write: The device was disconnected\./.test(l)));
});

test('a write that times out, before anything is changed', needPython, async () => {
    const b = theBridge();
    await b.ask({ op: 'new', files: enc({ 'os.ldr': 'old' }) });
    const r = await mtp.put(new Failing(b, 512, 30, { timeout: true }), NEW);
    assert.equal(r.outcome, 'timeout');
    assert.equal(r.changed, false);
    assert.deepEqual(r.advice, [mtp.WORDS.unchanged, mtp.WORDS.timeoutAdvice]);
});

// --- a scripted transport: the framing rules on their own ---

class Scripted {
    constructor(packets, mps = 512) {
        this.q = packets.map((p) => Uint8Array.from(p));
        this.mps = mps;
        this.writes = [];
    }

    async write(b) {
        this.writes.push(hex(b));
    }

    // one bulk transfer: packets up to size, ended early by a short one
    async read(size) {
        if (!this.q.length)
            return null;
        const parts = [];
        let n = 0;
        while (this.q.length && n < size) {
            const p = this.q.shift();
            parts.push(p);
            n += p.length;
            if (p.length < this.mps)
                break;
        }
        return Uint8Array.from(parts.flatMap((p) => Array.from(p)));
    }

    async close() {}
}

const container = (kind, code, tid, payload = [], length = null) => {
    const b = Buffer.alloc(12 + payload.length);
    b.writeUInt32LE(length === null ? 12 + payload.length : length, 0);
    b.writeUInt16LE(kind, 4);
    b.writeUInt16LE(code, 6);
    b.writeUInt32LE(tid, 8);
    Buffer.from(payload).copy(b, 12);
    return Array.from(b);
};

test('a data phase ends at a short packet even when its length says more', async () => {
    const s = new Scripted([container(2, 0x1009, 1, new Array(100).fill(7), 5000), container(3, 0x2001, 1)], 64);
    // 112 bytes in one packet of a 64-byte endpoint would be two: split them as a device sends them
    s.q = [s.q[0].subarray(0, 64), s.q[0].subarray(64), s.q[1]];
    const r = await new mtp.Mtp(s).transaction(0x1009, [1]);
    assert.equal(r.code, 0x2001);
    assert.equal(r.data.length, 100);
});

test('a data phase ends at its length when it fills whole packets', async () => {
    const data = container(2, 0x1009, 1, new Array(500).fill(1));      // 512 bytes: a zero-length packet follows
    const s = new Scripted([data, [], container(3, 0x2001, 1)]);
    const r = await new mtp.Mtp(s).transaction(0x1009, [1]);
    assert.equal(r.data.length, 500);
});

test('a stale data container is dropped, and events are skipped', async () => {
    const s = new Scripted([container(2, 0x1008, 7, [9, 9]), container(4, 0x4004, 0), container(2, 0x1004, 1, [1, 0, 0, 0]),
        container(3, 0x2001, 1)]);
    const m = new mtp.Mtp(s);
    const r = await m.transaction(0x1004);
    assert.deepEqual(Array.from(r.data), [1, 0, 0, 0]);
    assert.equal(m.stale, 1);
});

test('OK with no data phase is not an empty answer, and a response to another transaction fails', async () => {
    await assert.rejects(new mtp.Mtp(new Scripted([container(3, 0x2001, 1)])).readOp('GetStorageIDs', 0x1004),
        /no data phase/);
    await assert.rejects(new mtp.Mtp(new Scripted([container(3, 0x2001, 4)])).transaction(0x1004),
        /response to transaction 4 during transaction 1/);
    await assert.rejects(new mtp.Mtp(new Scripted([])).transaction(0x1004), (e) => e.kind === 'timeout');
});

test('a data phase that is a multiple of the packet size is ended by a zero-length packet', async () => {
    const s = new Scripted([container(3, 0x2001, 1)]);
    await new mtp.Mtp(s).checked('SendObject', 0x100D, [], new Uint8Array(500));
    assert.equal(s.writes.length, 3);
    assert.equal(s.writes[2], '');
});

test('names, strings and the name the file gets', () => {
    assert.equal(mtp.INSTALL_NAME, 'os.ldr');
    for (const name of ['', 'a/b', '.', '..', 'x'.repeat(255)])
        assert.ok(mtp.checkName(name));
    assert.equal(mtp.checkName('x'.repeat(254)), null);
    assert.deepEqual(Array.from(mtp.mtpString('')), [0]);
    assert.deepEqual(Array.from(mtp.mtpString('A')), [2, 0x41, 0, 0, 0]);
    const o = mtp.parseObject(5, mtp.objectInfoDataset(0x10001, 'os.ldr', 293900, '20261106T000000'));
    assert.deepEqual([o.name, o.size, o.format, o.modified, o.parent], ['os.ldr', 293900, 0x3000, '20261106T000000', 0]);
    const cut = Buffer.alloc(12);
    cut.writeUInt32LE(0x10001, 0);
    cut.writeUInt16LE(0x3000, 4);
    cut.writeUInt32LE(7, 8);
    const c = mtp.parseObject(5, new Uint8Array(cut));
    assert.deepEqual([c.size, c.name, c.modified], [7, '', '']);
    assert.equal(mtp.responseName(0x2002), 'General_Error (0x2002)');
    assert.equal(mtp.responseName(0x2999), 'an unknown code (0x2999)');
});

test('an empty file is refused before the unit is touched', async () => {
    const s = new Scripted([]);
    const r = await mtp.put(s, new Uint8Array(0), { fileName: 'waffle-install.ldr' });
    assert.equal(r.outcome, 'failed');
    assert.equal(r.text, 'The copy stopped: waffle-install.ldr is empty.');
    assert.deepEqual(s.writes, []);
});

// --- usb.js, over a stand-in for navigator.usb ---

const usb = await import('../../docs/web/usb.js');

const bulk = (n, direction, packetSize = 512) => ({ endpointNumber: n, direction, type: 'bulk', packetSize });
const MTP_CONFIG = { interfaces: [
    { interfaceNumber: 0, alternates: [{ alternateSetting: 0, interfaceClass: 0x08, interfaceSubclass: 6,
        interfaceProtocol: 0x50, endpoints: [bulk(1, 'in'), bulk(1, 'out')] }] },
    { interfaceNumber: 2, alternates: [{ alternateSetting: 0, interfaceClass: 0xFF, interfaceSubclass: 1,
        interfaceProtocol: 1, endpoints: [bulk(3, 'in'), bulk(4, 'out'), { endpointNumber: 5, direction: 'in',
            type: 'interrupt', packetSize: 64 }] }] },
] };

class FakeDevice {
    constructor({ config = MTP_CONFIG, fail = {}, ins = [] } = {}) {
        this.manufacturerName = 'teenage engineering';
        this.productName = 'OP-XY';
        this.vendorId = 0x2367;
        this.productId = 0x0021;
        this.config = config;
        this.configuration = null;
        this.fail = fail;
        this.ins = ins;
        this.claimed = null;
        this.closed = false;
        this.inCalls = 0;
    }

    async open() {
        if (this.fail.open)
            throw this.fail.open;
    }

    async selectConfiguration() {
        this.configuration = this.config;
    }

    async claimInterface(n) {
        if (this.fail.claim)
            throw this.fail.claim;
        this.claimed = n;
    }

    async releaseInterface() {}

    async close() {
        this.closed = true;
    }

    async clearHalt() {}

    transferIn() {
        this.inCalls++;
        const next = this.ins.shift();
        if (next === undefined)
            return new Promise(() => {});          // nothing comes
        if (next instanceof Error)
            return Promise.reject(next);
        return Promise.resolve({ status: 'ok', data: new DataView(Uint8Array.from(next).buffer) });
    }

    transferOut(ep, bytes) {
        if (this.fail.out === 'hang')
            return new Promise(() => {});
        return Promise.resolve({ status: 'ok', bytesWritten: bytes.length });
    }
}

const named = (name, message) => Object.assign(new Error(message), { name });

async function withUsb(fake, run) {
    const before = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
    const asked = [];
    const usbApi = fake === null ? undefined : {
        async requestDevice(opts) {
            asked.push(opts);
            if (fake instanceof Error)
                throw fake;
            return fake;
        },
        addEventListener() {},
        removeEventListener() {},
    };
    Object.defineProperty(globalThis, 'navigator', { value: usbApi ? { usb: usbApi } : {}, configurable: true,
        writable: true });
    try {
        return await run(asked);
    } finally {
        if (before)
            Object.defineProperty(globalThis, 'navigator', before);
        else
            delete globalThis.navigator;
    }
}

test('the MTP interface is found by its class, not by its number', () => {
    assert.deepEqual(usb.findMtpInterface(MTP_CONFIG), { number: 2, alternate: 0, epIn: 3, epOut: 4, mps: 512 });
    const std = { interfaces: [{ interfaceNumber: 0, alternates: [{ alternateSetting: 0, interfaceClass: 6,
        interfaceSubclass: 1, interfaceProtocol: 1, endpoints: [bulk(1, 'in', 64), bulk(2, 'out', 64)] }] }] };
    assert.deepEqual(usb.findMtpInterface(std), { number: 0, alternate: 0, epIn: 1, epOut: 2, mps: 64 });
    assert.equal(usb.findMtpInterface({ interfaces: [MTP_CONFIG.interfaces[0]] }), null);
    assert.equal(usb.findMtpInterface(null), null);
});

test('the prompt filters on the vendor only', async () => {
    assert.deepEqual(usb.USB_FILTERS, [{ vendorId: 0x2367 }]);
    await withUsb(new FakeDevice(), async (asked) => {
        const r = await usb.openMtp();
        assert.equal(r.ok, true);
        assert.deepEqual(asked, [{ filters: [{ vendorId: 0x2367 }] }]);
        assert.equal(r.transport.mps, 512);
        assert.equal(r.device.number, 2);
        assert.deepEqual(r.device.classes, ['08/06/50', 'ff/01/01']);
    });
});

test('each way of failing to open the unit is its own outcome, with the raw error', async () => {
    const cases = [
        [null, 'unsupported', null],
        [named('NotFoundError', 'No device selected.'), 'none', 'request'],
        [new FakeDevice({ fail: { open: named('SecurityError', 'Access denied.') } }), 'held', 'open'],
        [new FakeDevice({ fail: { claim: named('NetworkError', 'Unable to claim interface.') } }), 'held', 'claim'],
        [new FakeDevice({ fail: { claim: named('SecurityError', 'The requested interface implements a protected class.') } }),
            'blocked', 'claim'],
        [new FakeDevice({ fail: { open: named('NotFoundError', 'The device was disconnected.') } }), 'lost', 'open'],
        [new FakeDevice({ config: { interfaces: [MTP_CONFIG.interfaces[0]] } }), 'no-mtp', null],
    ];
    for (const [fake, outcome, step] of cases)
        await withUsb(fake, async () => {
            const r = await usb.openMtp();
            assert.equal(r.ok, false);
            assert.equal(r.outcome, outcome);
            assert.equal(r.step, step);
            assert.equal(r.text, usb.OPEN_WORDS[outcome][0]);
            if (step && fake instanceof FakeDevice) {
                assert.ok(r.error && r.error.name && r.error.message);
                if (step === 'claim')
                    assert.ok(fake.closed);
            }
        });
});

test('a read with nothing to give returns null, and the transfer in flight is kept', async () => {
    const dev = new FakeDevice();
    await withUsb(dev, async () => {
        const t = (await usb.openMtp()).transport;
        assert.equal(await t.read(65536, 20), null);
        assert.equal(await t.read(65536, 20), null);
        assert.equal(dev.inCalls, 1);
        dev.ins.push([1, 2, 3]);
        const t2 = (await usb.openMtp()).transport;
        assert.deepEqual(Array.from(await t2.read(65536, 20)), [1, 2, 3]);
        await assert.rejects(t.write(new Uint8Array(4), 50).then(() => {
            dev.fail.out = 'hang';
            return t.write(new Uint8Array(4), 20);
        }), (e) => e.timeout === true);
        const gone = new FakeDevice({ ins: [named('NetworkError', 'The device was disconnected.')] });
        await withUsb(gone, async () => {
            const g = (await usb.openMtp()).transport;
            await assert.rejects(g.read(65536, 20), (e) => e.gone === true && /NetworkError/.test(e.message));
        });
    });
});
