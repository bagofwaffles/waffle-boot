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
//   parseIHello(payload) -> { magic, proto, phase, flags, tests, version: [4], length, crc, pin,
//       device, how, imageDevice, survey }          null when the payload is not the installer's
//   parseSurvey(payload) -> { status, device, how, bootMode, n, c, selRead, selIndex, selVerified,
//       pa2, secCount, bootKiB, extRead, partConfig, os, slots: [2 x { bits, crc, length, tag,
//       log }] }                                    a survey build's facts; status 0xFF elsewhere
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
//   readState(link, n) / readTest(link, index) / readSurvey(link) -> Promise<parsed reply>
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

import { Failure, connect } from './core.js';
import * as IW from './install_words.js';
export * from './install_words.js';

const { IHELLO, STATE, TEST, REPORT, SURVEY, MAGIC, IHELLO_LEN, STATE_HEAD, NONE } = IW;

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
    const o = IW.IHELLO_O_DEVICE;
    return { magic: 'WBIN', proto: p[4], phase: p[5], flags: p[6], tests: p[7],
        version: [u16(p, 8), u16(p, 10), u16(p, 12), u16(p, 14)], length: u32(p, 16),
        crc: u32(p, 20), pin: p.slice(24, 56), device: p[o], how: p[o + 1], imageDevice: p[o + 2],
        survey: p[o + 3] === 1 };
}

export function parseSurvey(p) {
    if (p.length < 1)
        throw new Failure("the installer's survey reply is empty");
    if (p[0] !== 0)
        return { status: p[0] };
    if (p.length < IW.SURVEY_LEN)
        throw new Failure(`the installer's survey reply is ${p.length} bytes, short of ${IW.SURVEY_LEN}`);
    const slot = (at) => ({ bits: p[at], crc: u32(p, at + 4), length: u32(p, at + 8),
        tag: u32(p, at + 12), log: Array.from(p.subarray(at + 16, at + 24)) });
    return { status: 0, device: p[1], how: p[2], bootMode: p[3], n: u32(p, 4), c: u32(p, 8),
        selRead: p[12] === 1, selIndex: p[13], selVerified: p[14], pa2: p[15], secCount: u32(p, 16),
        bootKiB: u32(p, 20), extRead: p[24] === 1, partConfig: p[25], os: p[26],
        slots: [slot(28), slot(52)] };
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

export async function readSurvey(link, opts = {}) {
    return parseSurvey((await link.request(SURVEY, undefined, opts)).payload);
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
        breadcrumb: !!(flags & IW.F_BREADCRUMB), survey: !!(flags & IW.F_SURVEY),
        device: hello.imageDevice === undefined ? null : IW.deviceName(hello.imageDevice) };
}

// The unit's device as the installer decided it at its entry (S-18), or null.
function deviceOf(hello) {
    if (!hello || hello.how === undefined)
        return null;
    if (hello.how === IW.HOW_UNKNOWN)
        return { name: null, identified: false, text: 'not identified' };
    return { name: IW.deviceName(hello.device), identified: hello.how === IW.HOW_TABLE,
        text: IW.deviceName(hello.device) };
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
        device: deviceOf(hello),
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
        A.push(d.device && d.device.name && d.device.name !== 'OP-XY' ?
            'The key check: press the com key, then the recovery key, on the unit, to continue; ' +
            'its screen shows the number of each key pressed.' :
            'The key check: press com, then M4 (4 under the display), on the unit, to continue.');
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
        L.push('Checks passed.' + (target ? ` The trial goes into ${target}.` : ''));
        L.push('Nothing is written until you hold the key on the unit.');
        if (d.image && d.image.breadcrumb)
            L.push(IW.BREADCRUMB);
        A.push(`Hold ${key} on the unit to write the trial.`);
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
        A.push(`Hold ${key} on the unit to install waffle-boot.`);
        A.push(IW.RETRIAL);
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
        A.push(`Hold ${key} on the unit to write the trial again.`);
        A.push(IW.SAVE_REPORT);
        break;
    case IW.LEFT:
        L.push('Nothing written: returned to the old OS without installing.');
        L.push(osLine(s.os));
        break;
    case IW.SURVEYED:
        d.tone = 'ok';
        L.push(IW.SURVEY_DONE);
        L.push(s.lock === IW.LOCK_SURVEY ? 'An install by the installer would be offered on this unit.' :
            `An install would not be offered: ${d.lock ? d.lock.text : 'the checks'}.`);
        L.push(osLine(s.os));
        A.push(IW.SAVE_REPORT);
        break;
    default:
        L.push(`The installer reports phase ${s.phase}, which this page does not know.`);
    }
    if (d.device && !d.device.identified && d.device.name === null)
        L.push(IW.NOT_IDENTIFIED);
    if (d.leave)
        A.push(IW.LEAVE);
    if (d.hold)
        L.push(`Holding ${d.hold.key}: ${Math.round(d.hold.progress * 100)} %.`);
    return d;
}
