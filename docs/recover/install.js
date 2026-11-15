// SPDX-License-Identifier: GPL-2.0-or-later
//
// The installer's USB protocol and what the page says about it: no DOM and no Web Serial. The
// installer (waffle-install.ldr) reuses recovery's framing, so frames, the link and resync are
// core.js's own. Every request to the installer only reads: the writes are started and confirmed
// on the unit. The one thing the page sends is the installer file itself, to a boot menu's
// developer link; the unit writes it to a boot partition only when the person presses 5 there.
//
// EXPORTED API
//
//   parseIHello(payload) -> { magic, proto, phase, flags, tests, version: [4], length, crc, pin }
//                                                   null when the payload is not the installer's
//   parseState(payload, n) -> { phase, flags, verdict, current, lock, selIndex, selVerified,
//       slots: [2 bits], target, result, os, hold (0..1000), holdKey, trial, nextKey,
//       tests: number[n] }
//   parseTest(payload) -> { status, index, state, kind, id, name, detail, note }   status 1: no
//       such test, and nothing else
//   parseReport(payload) -> { status, total, data: Uint8Array }
//
//   installerHello(link, opts) -> Promise<hello | null>   IHELLO; null when the unit answers it
//       but is not the installer (recovery answers with a status byte)
//   detect(link, { probe }) -> Promise<{ kind: 'installer', hello } | { kind: 'recovery', hello,
//       info }>   IHELLO first, then recovery's HELLO and INFO (core.js's connect)
//   readState(link, n) / readTest(link, index) -> Promise<parsed reply>
//   readReport(link, { onProgress }) -> Promise<string>      check.txt, read in chunks
//   poll(link, hello, tests, opts) -> Promise<{ state, changed: number[] }>   STATE, then TEST
//       for each test whose state differs from tests[index]; tests (index -> TEST reply) is
//       updated. opts ({ timeout, retries }) go to each request.
//   isBusy(error) -> bool    a request that got no answer while the port stayed: the installer
//       does not serve its port while it writes and in some long checks, for up to about 30 s
//
//   describeInstall(state, hello, tests) -> {
//       phase: { code, name, text }, tone: 'wait' | 'act' | 'ok' | 'bad',
//       verdict: { code, text } , checks: [{ index, id, name, kind, state, stateText, detail,
//       note, current }], image: { version, length, crc, pin, pinShort, verified, breadcrumb },
//       target: 'Slot 1' | 'Slot 2' | null, hold: { key, progress } | null, nextKey,
//       trial: { code, text }, os: { code, text }, lock: { code, text } | null,
//       failure: { code, text } | null, slots: [2 x { name, bits, words }], starts: string,
//       leave: bool (hold 8 is offered), busy: bool (a phase in which the port may go quiet),
//       lines: string[], advice: string[] }
//       lines: what is happening; advice: what to do now. hello and tests may be null.
//
// The boot menu's developer link (docs/devhost.md), for a unit already on waffle-boot with
// developer mode on: the installer file sent as /yaffs2/os.ldr, as tools/testos_mon.py dev-put does
//   devFrame(payload, seq) -> Uint8Array / devUnframe(wire) -> { id, payload } | null
//       COBS frames: route 0x21, seq, id 0x7D, length, payload, CRC-8 (devCrc8); 00 ends each
//   new DevLink(transport).request(payload, timeoutMs) -> Promise<Uint8Array>   the first reply
//       of id 0x7D; no retry; rejects with a Failure of kind 'link'
//   checkInstallerFile(bytes, name) -> { ok, length, problems, notes, lines }   not empty, at
//       most LDR_MAX, and a first LDR block header
//   devPut(link, bytes, name, { onStage, onProgress, timeout, shaTimeout }) -> Promise<{ outcome:
//       'verified' | 'mismatch' | 'refused' | 'lost' | 'failed' | 'not-a-file', status, sent,
//       sha256, unitSha256, unitSize, lines, advice }>

import { Failure, connect } from './core.js';
import * as IW from './install_words.js';
export * from './install_words.js';

const { IHELLO, STATE, TEST, REPORT, MAGIC, IHELLO_LEN, STATE_HEAD, NONE } = IW;

const view = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);
const u16 = (b, o) => view(b).getUint16(o, true);
const u32 = (b, o) => view(b).getUint32(o, true);
const hexOf = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

// --- parsing ---

export function parseIHello(p) {
    if (p.length < 4 || MAGIC.some((m, k) => p[k] !== m))
        return null;
    if (p.length < IHELLO_LEN)
        throw new Failure(`the installer's hello is ${p.length} bytes, short of ${IHELLO_LEN}`);
    return { magic: 'WBIN', proto: p[4], phase: p[5], flags: p[6], tests: p[7],
        version: [u16(p, 8), u16(p, 10), u16(p, 12), u16(p, 14)], length: u32(p, 16),
        crc: u32(p, 20), pin: p.slice(24, 56) };
}

export function parseState(p, n) {
    if (p.length < STATE_HEAD + n)
        throw new Failure(`the installer's state is ${p.length} bytes, short of ${STATE_HEAD + n}`);
    return { phase: p[0], flags: p[1], verdict: p[2], current: p[3], lock: p[4], selIndex: p[5],
        selVerified: p[6], slots: [p[7], p[8]], target: p[9], result: p[10], os: p[11],
        hold: u16(p, 12), holdKey: p[14], trial: p[15], nextKey: p[16],
        tests: Array.from(p.subarray(STATE_HEAD, STATE_HEAD + n)) };
}

// NUL-terminated ASCII strings from at; a missing NUL ends the payload.
function strings(p, at, count) {
    const out = [];
    for (let k = 0; k < count; k++) {
        let end = p.indexOf(0, at);
        if (end < 0)
            end = p.length;
        out.push(String.fromCharCode(...p.subarray(at, end)));
        at = Math.min(end + 1, p.length);
    }
    return out;
}

export function parseTest(p) {
    if (p.length < 1)
        throw new Failure("the installer's test reply is empty");
    if (p[0] !== 0)
        return { status: p[0] };
    if (p.length < 4)
        throw new Failure(`the installer's test reply is ${p.length} bytes`);
    const [id, name, detail, note] = strings(p, 4, 4);
    return { status: 0, index: p[1], state: p[2], kind: p[3], id, name, detail, note };
}

export function parseReport(p) {
    if (p.length < 1)
        throw new Failure("the installer's report reply is empty");
    if (p[0] !== 0)
        return { status: p[0], total: 0, data: new Uint8Array(0) };
    if (p.length < 5)
        throw new Failure(`the installer's report reply is ${p.length} bytes`);
    return { status: 0, total: u32(p, 1), data: p.slice(5) };
}

// --- requests ---

export async function installerHello(link, opts = {}) {
    const { payload } = await link.request(IHELLO, undefined, opts);
    const h = parseIHello(payload);
    if (h && h.proto !== IW.IPROTO)
        throw new Failure(`the installer speaks protocol ${h.proto}; this page speaks ${IW.IPROTO}`);
    return h;
}

// The installer answers IHELLO; recovery answers it with its unknown-request status. A port that
// answers neither within the probe's time is asked recovery's HELLO all the same.
export async function detect(link, { probe = { timeout: 1500, retries: 2 } } = {}) {
    let h = null;
    try {
        h = await installerHello(link, probe);
    } catch (e) {
        if (!(e instanceof Failure) || e.kind !== 'link')
            throw e;
    }
    if (h)
        return { kind: 'installer', hello: h };
    const r = await connect(link);
    return { kind: 'recovery', hello: r.hello, info: r.info };
}

export async function readState(link, n, opts = {}) {
    return parseState((await link.request(STATE, undefined, opts)).payload, n);
}

export async function readTest(link, index, opts = {}) {
    return parseTest((await link.request(TEST, Uint8Array.of(index & 0xFF), opts)).payload);
}

// core.js's Link says "no answer" when the port stayed and nothing came back, and names the
// port going away otherwise.
export const isBusy = (e) => e instanceof Failure && e.kind === 'link' && /^no answer/.test(e.message);

// check.txt in chunks. The text may grow while the checks run: when its length changes between
// chunks the read starts over, at most three times.
export async function readReport(link, { onProgress } = {}) {
    for (let round = 0; round < 3; round++) {
        const parts = [];
        let at = 0, total = null, again = false;
        for (;;) {
            const req = new Uint8Array(4);
            view(req).setUint32(0, at, true);
            const r = parseReport((await link.request(REPORT, req)).payload);
            if (r.status !== 0)
                throw new Failure(`the installer refused the report read (status ${r.status})`);
            if (total !== null && r.total !== total) {
                again = true;
                break;
            }
            total = r.total;
            parts.push(r.data);
            at += r.data.length;
            if (onProgress)
                onProgress(at, total);
            if (at >= total)
                break;
            if (!r.data.length)
                throw new Failure('the installer sent no bytes of check.txt before its end');
        }
        if (!again) {
            const all = new Uint8Array(at);
            let o = 0;
            for (const p of parts) {
                all.set(p, o);
                o += p.length;
            }
            return new TextDecoder('latin1').decode(all.subarray(0, total));
        }
    }
    throw new Failure('check.txt kept changing while it was read: read it again once the checks end');
}

export async function poll(link, hello, tests, opts = {}) {
    const state = await readState(link, hello.tests, opts);
    const changed = [];
    for (let k = 0; k < state.tests.length; k++) {
        const known = tests[k];
        if (known && known.state === state.tests[k])
            continue;
        const t = await readTest(link, k, opts);
        if (t.status === 0) {
            tests[k] = t;
            changed.push(k);
        }
    }
    return { state, changed };
}

// --- what the page says ---

const vtext = (v) => `v${v[0]}.${v[1]}.${v[2]}`;
const isSlot = (k) => k === 0 || k === 1;

function imageOf(hello, state) {
    if (!hello)
        return null;
    const flags = state ? state.flags : hello.flags;
    const pin = hexOf(hello.pin);
    return { version: vtext(hello.version), length: hello.length, crc: IW.hex8(hello.crc), pin,
        pinShort: pin.slice(0, 8), verified: !!(flags & IW.F_IMAGE_OK),
        breadcrumb: !!(flags & IW.F_BREADCRUMB) };
}

// What the unit starts at its next start, from the selector and the slots' bits.
function startsText(s) {
    if (!isSlot(s.selIndex))
        return 'The selector names no slot it can read.';
    let t = `The selector names ${IW.slotName(s.selIndex)}` +
        (s.selVerified === 1 ? ', settled.' : ', not settled.');
    for (const k of [0, 1]) {
        const b = s.slots[k];
        if (b & IW.SB_PENDING && !(b & IW.SB_SPENT) && !(b & IW.SB_STARTED))
            t += ` The next start tries ${IW.slotName(k)} once.`;
    }
    return t;
}

function osLine(os) {
    if (os === 6)
        return IW.OS_BACK;
    if (os === 7)
        return IW.OS_BACK_FAILED;
    return IW.OS_STAYS;
}

export function describeInstall(s, hello = null, tests = null) {
    const [name, text] = IW.PHASE[s.phase] || [`phase ${s.phase}`, `Phase ${s.phase}`];
    const target = isSlot(s.target) ? IW.slotName(s.target) : null;
    const checks = s.tests.map((st, k) => {
        const t = tests && tests[k];
        return { index: k, id: t ? t.id : '', name: t ? t.name : `Test ${k + 1}`,
            kind: t ? t.kind : null, state: st, stateText: IW.testState(st),
            detail: t && t.state === st ? t.detail : '', note: t && t.state === st ? t.note : '',
            current: s.current === k };
    });
    const current = s.current !== NONE && checks[s.current] ? checks[s.current].name : null;
    const d = {
        phase: { code: s.phase, name, text },
        tone: 'wait',
        verdict: { code: s.verdict, text: IW.VERDICT[s.verdict] || `verdict ${s.verdict}` },
        checks,
        image: imageOf(hello, s),
        target,
        hold: s.holdKey ? { key: s.holdKey, progress: s.hold / 1000 } : null,
        nextKey: s.nextKey || 0,
        trial: { code: s.trial, text: IW.trialText(s.trial) },
        os: { code: s.os, text: IW.osText(s.os) },
        lock: s.lock ? { code: s.lock, text: IW.lockText(s.lock) } : null,
        failure: s.result !== 0 && s.result !== NONE ?
            { code: s.result, text: IW.failureText(s.result) } : null,
        slots: [0, 1].map((k) => ({ name: IW.slotName(k), bits: s.slots[k],
            words: IW.slotBits(s.slots[k]) })),
        starts: startsText(s),
        leave: IW.LEAVE_PHASES.includes(s.phase) && (s.os === 1 || s.os === 2),
        busy: [IW.STARTING, IW.CHECKING, IW.GUIDED, IW.WRITING_TRIAL, IW.COMMITTING].includes(s.phase),
        lines: [],
        advice: [],
    };
    const key = d.nextKey || 1;
    const L = d.lines, A = d.advice;
    switch (s.phase) {
    case IW.STARTING:
        L.push('Starting the checks.');
        A.push(IW.STEADY);
        break;
    case IW.CHECKING:
        L.push(current ? `Running the checks: ${current}.` : 'Running the checks.');
        A.push(IW.STEADY);
        break;
    case IW.GUIDED:
        L.push('The key check.');
        A.push('The key check: press com, then M4, on the unit.');
        break;
    case IW.LOCKED:
        d.tone = 'bad';
        L.push('A check did not pass: nothing was written, and the install is not offered.');
        if (d.lock)
            L.push(`Why: ${d.lock.text}.`);
        L.push(osLine(s.os));
        if (d.lock && IW.LOCK_ADVICE[d.lock.code])
            A.push(IW.LOCK_ADVICE[d.lock.code]);
        A.push(IW.SAVE_REPORT);
        break;
    case IW.READY:
        d.tone = 'act';
        L.push('Every check passed.' + (target ? ` The trial goes into ${target}.` : ''));
        L.push('Nothing is written until you hold the key on the unit.');
        if (d.image && d.image.breadcrumb)
            L.push(IW.BREADCRUMB);
        A.push(`Hold ${key} on the unit for 5 s to write the trial.`);
        break;
    case IW.WRITING_TRIAL:
        L.push('Writing the trial' + (target ? ` into ${target}.` : '.'));
        A.push(IW.STEADY);
        break;
    case IW.TRIAL_WRITTEN:
        d.tone = 'act';
        L.push('The trial is written.');
        A.push('Switch off, then on: the new bootloader starts once.');
        A.push('When its trial start reaches the boot menu, the unit comes back to the installer, ' +
            'and this page connects again.');
        break;
    case IW.COMMIT_READY:
        d.tone = 'act';
        L.push('The trial start reached the boot menu.');
        if (d.image && d.image.breadcrumb)
            L.push(IW.BREADCRUMB);
        A.push(`Hold ${key} on the unit for 5 s to keep the new bootloader.`);
        break;
    case IW.COMMITTING:
        L.push('Writing: keeping the new bootloader.');
        A.push(IW.STEADY);
        break;
    case IW.DONE:
        d.tone = 'ok';
        L.push('Done: the new bootloader is kept' + (target ? ` in ${target}.` : '.'));
        L.push(osLine(s.os));
        A.push('Switch the unit off and on.');
        break;
    case IW.FAILED:
        d.tone = 'bad';
        L.push('A write failed' + (d.failure ? `: ${d.failure.text}.` : '.'));
        L.push(d.starts);
        L.push(osLine(s.os));
        A.push("Read the unit's screen before you switch it off: it says what failed and what " +
            'it starts.');
        A.push(IW.SAVE_REPORT);
        break;
    case IW.TRIAL_FAILED:
        d.tone = 'bad';
        L.push(`The trial failed: ${d.trial.text}.`);
        L.push('Nothing was kept. ' + d.starts);
        A.push(`Hold ${key} on the unit for 5 s to try again.`);
        A.push(IW.SAVE_REPORT);
        break;
    case IW.LEFT:
        L.push('Nothing written: the installer was left without installing.');
        L.push(osLine(s.os));
        break;
    default:
        L.push(`The installer reports phase ${s.phase}, which this page does not know.`);
    }
    if (d.leave)
        A.push(IW.LEAVE);
    if (d.hold)
        L.push(`Holding ${d.hold.key}: ${Math.round(d.hold.progress * 100)} %.`);
    return d;
}

// --- the boot menu's developer link: sending the installer as /yaffs2/os.ldr ---
// tools/testos_mon.py is the reference: frame(), unframe(), crc8(), the COBS pair, Monitor.call()
// and its 'dev-put' command. One request at a time and no retry: a PUT_DATA sent twice would be
// written twice, so a request with no answer ends the send.

const { DEV_ROUTE, DEV_ID, DEV_PUT, DEV_SHA, PUT_OPEN, PUT_DATA, PUT_CLOSE, PUT_CHUNK, DEV_NAME } = IW;

// S-06.003: reflected 0x4D, every input byte XORed with 0xA5.
export function devCrc8(data) {
    let crc = 0;
    for (const b of data) {
        crc ^= b ^ 0xA5;
        for (let k = 0; k < 8; k++)
            crc = crc & 1 ? (crc >> 1) ^ 0xB2 : crc >> 1;
    }
    return crc;
}

export function cobsEncode(raw) {
    const out = [];
    let block = [];
    for (const b of raw) {
        if (b === 0) {
            out.push(block.length + 1, ...block);
            block = [];
        } else {
            block.push(b);
            if (block.length === 254) {
                out.push(0xFF, ...block);
                block = [];
            }
        }
    }
    out.push(block.length + 1, ...block);
    return Uint8Array.from(out);
}

// null when the bytes are not COBS
export function cobsDecode(enc) {
    const out = [];
    let i = 0;
    while (i < enc.length) {
        const code = enc[i];
        if (code === 0 || i + code > enc.length)
            return null;
        for (let k = i + 1; k < i + code; k++)
            out.push(enc[k]);
        i += code;
        if (code !== 0xFF && i < enc.length)
            out.push(0);
    }
    return Uint8Array.from(out);
}

// route, seq, id, length, payload, CRC-8 over all of them; COBS, then the zero delimiter
export function devFrame(payload, seq, ident = DEV_ID, route = DEV_ROUTE) {
    if (payload.length > 255)
        throw new Failure(`a developer request of ${payload.length} bytes does not fit a frame`);
    const raw = new Uint8Array(4 + payload.length + 1);
    raw.set([route, seq & 0xFF, ident, payload.length]);
    raw.set(payload, 4);
    raw[raw.length - 1] = devCrc8(raw.subarray(0, raw.length - 1));
    const enc = cobsEncode(raw);
    const wire = new Uint8Array(enc.length + 1);
    wire.set(enc);
    return wire;
}

// { id, payload } of a frame's bytes without its delimiter, or null when it is not a good frame
export function devUnframe(wire) {
    const raw = cobsDecode(wire);
    if (!raw || raw.length < 5 || raw[3] !== raw.length - 5 ||
            devCrc8(raw.subarray(0, raw.length - 1)) !== raw[raw.length - 1])
        return null;
    return { id: raw[2], payload: raw.slice(4, raw.length - 1) };
}

const nowMs = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());

// The developer link over a transport (core.js's: write, read, close). request() sends one frame
// and gives the payload of the first good frame of the developer id that comes back; the log
// shares the port, so frames of other ids are passed over.
export class DevLink {
    constructor(transport) {
        this.transport = transport;
        this.seq = 0;
        this.rx = new Uint8Array(0);
    }

    close() {
        return this.transport.close();
    }

    async request(payload, timeout = IW.DEV_TIMEOUT) {
        this.seq = (this.seq + 1) & 0xFF;
        try {
            await this.transport.write(devFrame(payload, this.seq));
        } catch (e) {
            throw new Failure(IW.DEV_GONE, 'link');
        }
        const end = nowMs() + timeout;
        for (;;) {
            let at;
            while ((at = this.rx.indexOf(0)) >= 0) {
                const got = devUnframe(this.rx.subarray(0, at));
                this.rx = this.rx.slice(at + 1);
                if (got && got.id === DEV_ID)
                    return got.payload;
            }
            const left = end - nowMs();
            if (left <= 0)
                throw new Failure(IW.DEV_NO_ANSWER, 'link');
            let data;
            try {
                data = await this.transport.read(Math.min(left, 50));
            } catch (e) {
                throw new Failure(IW.DEV_GONE, 'link');
            }
            if (data && data.length) {
                const both = new Uint8Array(this.rx.length + data.length);
                both.set(this.rx);
                both.set(data, this.rx.length);
                this.rx = both;
            }
        }
    }
}

// A refused request carries the boot menu's status
class Refused extends Failure {
    constructor(status) {
        super(IW.devRefused(status));
        this.status = status;
    }
}

async function checked(link, payload, timeout) {
    const r = await link.request(payload, timeout);
    if (!r.length)
        throw new Failure('the boot menu answered with an empty reply');
    if (r[0] !== 0)
        throw new Refused(r[0]);
    return r.subarray(1);
}

// The page's own check before a send: not empty, at most what a unit loads, and a first LDR block
// header (S-05: signature 0xAD in the block code's top byte, the 16 bytes XOR to zero).
export function checkInstallerFile(bytes, name = IW.INSTALLER_FILE) {
    const problems = [], notes = [];
    if (!bytes.length)
        problems.push(IW.DEV_EMPTY);
    else if (bytes.length > IW.LDR_MAX)
        problems.push(IW.devTooLarge(bytes.length));
    else {
        let sum = 0;
        for (let k = 0; k < 16 && k < bytes.length; k++)
            sum ^= bytes[k];
        if (bytes.length < 16 || bytes[3] !== 0xAD || sum !== 0)
            problems.push(IW.DEV_NOT_LDR);
    }
    if (!problems.length && name !== IW.INSTALLER_FILE)
        notes.push(IW.devOtherName(name));
    return { ok: !problems.length, length: bytes.length, problems, notes,
        lines: problems.length ? problems : [IW.devFileOk(bytes.length)].concat(notes) };
}

const textEncoder = new TextEncoder();
const cat = (...parts) => {
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let o = 0;
    for (const p of parts) {
        out.set(p, o);
        o += p.length;
    }
    return out;
};

// testos_mon.py dev-put FILE: OPEN with the name, the file in PUT_CHUNK-byte DATA requests, CLOSE,
// then SHA with the name (30 s): the unit's SHA-256 (32 bytes) and size (u32) of its copy, held
// against the file's. hooks: { onStage(name), onProgress(sent, total), timeout, shaTimeout }.
// -> { outcome: 'verified' | 'mismatch' | 'refused' | 'lost' | 'failed' | 'not-a-file',
//      status (the boot menu's, when refused), sent, sha256, unitSha256, unitSize, lines, advice }
// Nothing is sent for a file checkInstallerFile() refuses.
export async function devPut(link, bytes, name = IW.INSTALLER_FILE, hooks = {}) {
    const { onStage, onProgress, timeout = IW.DEV_TIMEOUT, shaTimeout = IW.DEV_SHA_TIMEOUT } = hooks;
    const stage = (s) => onStage && onStage(s);
    const file = checkInstallerFile(bytes, name);
    const r = { outcome: null, status: null, sent: 0, sha256: null, unitSha256: null, unitSize: null,
        lines: [], advice: [] };
    if (!file.ok) {
        r.outcome = 'not-a-file';
        r.lines = file.problems;
        return r;
    }
    r.sha256 = hexOf(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)));
    const target = textEncoder.encode(DEV_NAME);
    let opened = false;
    try {
        stage('open');
        await checked(link, cat(Uint8Array.of(DEV_PUT, PUT_OPEN), target), timeout);
        opened = true;
        stage('sending');
        for (let off = 0; off < bytes.length; off += PUT_CHUNK) {
            const chunk = bytes.subarray(off, off + PUT_CHUNK);
            await checked(link, cat(Uint8Array.of(DEV_PUT, PUT_DATA), chunk), timeout);
            r.sent = off + chunk.length;
            if (onProgress)
                onProgress(r.sent, bytes.length);
        }
        stage('close');
        await checked(link, Uint8Array.of(DEV_PUT, PUT_CLOSE), timeout);
        stage('sha');
        const got = await checked(link, cat(Uint8Array.of(DEV_SHA), target), shaTimeout);
        if (got.length < 36)
            throw new Failure(`the boot menu's SHA reply is ${got.length + 1} bytes, short of 37`);
        r.unitSha256 = hexOf(got.subarray(0, 32));
        r.unitSize = u32(got, 32);
    } catch (e) {
        if (!(e instanceof Failure))
            throw e;
        if (e instanceof Refused) {
            r.outcome = 'refused';
            r.status = e.status;
        } else {
            r.outcome = e.kind === 'link' ? 'lost' : 'failed';
        }
        r.lines = [e.message];
        if (opened)
            r.advice = [IW.DEV_PARTIAL];
        return r;
    }
    if (r.unitSha256 === r.sha256 && r.unitSize === bytes.length) {
        r.outcome = 'verified';
        r.lines = [IW.DEV_VERIFIED];
        r.advice = IW.DEV_VERIFIED_ADVICE.slice();
    } else {
        r.outcome = 'mismatch';
        r.lines = [IW.devMismatch(r.unitSha256, r.unitSize, r.sha256, bytes.length)];
        r.advice = [IW.DEV_DONT_WRITE, IW.DEV_AGAIN];
    }
    return r;
}
