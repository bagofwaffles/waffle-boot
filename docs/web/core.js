// SPDX-License-Identifier: GPL-2.0-or-later
//
// Recovery's protocol and decisions for a browser: what tools/wbrecover.py does, with no DOM and
// no Web Serial in it. It runs unchanged as <script type="module"> and in Node 20 or later, and
// needs only crypto.subtle. The words and tables are in words.js, exported again from here.
//
// EXPORTED API
//
// Framing
//   crc32(bytes, crc = 0) -> u32                       zlib's CRC-32, continued from crc
//   frame(type, seq, payload = empty) -> Uint8Array    "WR", type, seq, u16 length, payload, CRC-32
//   takeFrame(rx) -> { type, seq, payload } | null     rx = { bytes: Uint8Array }; removes the first
//                                                      whole frame from rx.bytes and drops what
//                                                      does not start a good one
//
// The link
//   transport = { write(Uint8Array): Promise<void>,
//                 read(timeoutMs): Promise<Uint8Array | null>,   the next chunk; null on timeout
//                 close(): Promise<void> }
//       write() and read() must reject when the port is gone.
//   new Link(transport, { timeout = 5000, retries = 4, longTimeout = 30000, longRetries = 3,
//                         poll = 200, deadline = 900000 })       all in ms
//       .request(type, payload?, { timeout?, retries? }) -> Promise<{ payload, attempt }>
//                                                      rejects with a Failure
//       .close() -> Promise<void>
//       .front    the unit's front version, after hello()        .maxData   bytes in one DATA
//       poll is the pause between INFO requests while a write runs; deadline is how long a write
//       may take after its countdown.
//   class Failure extends Error    .kind = 'link' (no answer, the port went away) | 'protocol'
//   hello(link) -> Promise<hello>                      HELLO; sets link.front and link.maxData
//   getInfo(link) -> Promise<info>                     INFO, parsed, with info.front = link.front
//   connect(link) -> Promise<{ hello, info }>          both, in that order
//
// Parsing (fields under the Python tool's names; byte strings are Uint8Array)
//   parseHello(payload) -> { proto, front_version, max_data, why, own_desc, version: [4], pin }
//   parseInfo(payload) -> info = {
//       v6, v11, why, running, sel_index, sel_verified, target, refusal, phase, len, got, progress,
//       result, image_rc, written, established, selector_ok, own_desc, moved, spoiled, held,
//       read_error, unsettled, run_way, run_refusal, way, batt, count, act, tier,
//       acts: [4] | null, forced: [4] | null, keep, refused, version: [4], pin, readback,
//       slots: [2 x { good, has_desc, has_front, tried, started, te_prefix, length, crc, version: [4],
//                     front_crc, pin }] }
//       front (the front's version) is added by getInfo(); set it yourself on a parsed payload.
//   battery(batt) -> [state: 'charged' | 'low' | 'absent' | 'unknown', words]
//   describe(info) -> {
//       why, frontVersion, running: 0 | 1 | null, established,
//       selector: { read, slot, verified, settled } ,
//       front: { version: '1.2.3', pin: hex } | null,          the running image's
//       slots: [2 x { index, name: 'Slot 1' | 'Slot 2', version: '1.2.3' | null,
//                     state: 'checked' | 'check failed' | 'tried' | 'tried, started', kind: 'WB 0.9.36' |
//                     'TE 1000.0.20' | 'TE' | '??' | null (S-02.106), good, tried, started,
//                     running, selected, hasDesc, hasFront, frontMatches, length, crc, pin: hex }],
//       battery: { state, text } | null,                        null from a version 5 front
//       tier, tierText,                                         the other slot against this front
//       commands: { write:   { allowed, target, code, text },
//                   running: { allowed, way, code, text } | null,
//                   try, switch, keep, putBack: { allowed, forceWouldLift, slot, code, text } | null,
//                   restart: { allowed, forceWouldLift, text } },
//       phase: { code, name, text }, countdown: seconds | 0, since: text | null,
//       damaged, held, readError, moved,
//       stateLines: string[],     the tried slot's and the damaged state's lines (held, ways out)
//       lines: string[] }         the Python tool's info, line for line
//
// The image
//   checkImage(bytes, { knownPins } = {}) -> Promise<image> = {
//       bytes, length, sha256, crc32, problems: string[], noFrontOnly, ok,
//       front: { entry, front_crc, version: [4], pin, pin_calc, pin_ok } | null,
//       version: '1.2.3' | null, pin: hex | null, frontVersion: number | null,
//       warnings: string[],       none: kept for the page's notes
//       lines: string[] }         what the Python tool's check prints
//       ok: no problems and the descriptor's pin is the front's own. noFrontOnly: every problem
//       is a missing or damaged front, which a forced write alone takes. frontVersion comes only
//       from knownPins = { pinHex: frontVersion | null }, the front pins the page knows as signed:
//       an image does not carry its front's version.
//
// Decisions: pure, from INFO; nothing is sent
//   planWrite(info, image, { mode = 'plain' | 'running' | 'new-front', force = false, knownPins })
//   planSelect(info, image, { force = false })
//   planAct(info, 'try' | 'switch' | 'keep' | 'put-back', { force = false })
//   planRestart(info, { force = false })
//   each -> plan = {
//       action: 'write' | 'select' | 'try' | 'switch' | 'keep' | 'put-back' | 'restart',
//       options,                  the options given, kept for the run
//       allowed: bool,
//       refusal: { code, text, lines } | null,    code: the unit's status number where the refusal
//                                 is one, else 'image', 'pin', 'front-too-old', 'no-front',
//                                 'front-unknown', 'other-front', 'old-front-file', 'nothing-tried',
//                                 'tried-not-here', 'no-slot'
//       forceWouldLift: bool,     refused now, allowed with force
//       force: bool,              the force flag goes out (false when asked for and not needed)
//       way: number | null,       enum fr_way the unit is expected to announce; null: BEGIN says
//       target: 0 | 1 | null,     the slot written or acted on; null: BEGIN says
//       request: Uint8Array | null,   the payload of BEGIN, ACT or RESTART
//       said: string[],           what the Python tool prints before its first prompt
//       confirmations: [{ phrase: string | null, lines: string[], danger }],   in order; every
//                                 one is to be shown, and its phrase typed where it has one
//       phrase: string | null,    the first phrase to type: 'yes', 'erase the running slot in
//                                 place', 'try the new front', 'make it the boot slot', 'force the <command>'
//       lines: string[],          every confirmation's lines, in order
//       countdown: bool,          the unit counts down 15 s before its first erase
//       danger: 'none' | 'caution' | 'no-fallback' }
//       no-fallback: an in-place write, a forced in-place write, a switch to a bad slot, a forced
//       restart. caution: a forced request, a typed phrase other than yes, a warning, a good image
//       replaced, a write in the damaged state.
//       A write's plan also carries trial and signed, for its run. The last confirmation of a
//       write or a select has way and target too: it is the warning for BEGIN's answer.
//   refusalBefore(info, image, target, way, force) -> 0 | 12 | 13      what END would refuse
//   sameVersion(info, image) -> [[slot, version]]      slots with the file's version, other bytes
//   endLost(front, before, info, way, sha256) -> 'info' | 'refused' | 'unknown'
//   actChecks(act, before, info, slot) -> [[what, ok]]  what INFO must show after an action
//
// Execution: no prompting; each starts with HELLO and INFO and plans again
//   runWrite(link, plan, image, hooks) | runSelect(link, plan, image, hooks)
//   runAct(link, plan, hooks) | runRestart(link, plan, hooks)
//   hooks = { onStage(name), onProgress(done, total), onCountdown(secondsLeft), onInfo(info),
//             confirm(confirmation) -> Promise<bool>, signal: AbortSignal }      all optional
//       stages: 'hello', 'begin', 'confirm', 'sending', 'end', 'countdown', 'writing', 'done'.
//       onProgress: bytes sent of the file while 'sending', then 0..1000 while 'writing'.
//       confirm: called after BEGIN when the unit announces a write whose warning is not, word
//       for word, the last confirmation of the plan (always when plan.target is null, and for an
//       in-place write when the battery report asked at BEGIN differs). confirmation =
//       { phrase, lines, danger, way, target }. Without the hook, or when it resolves false, the
//       run stops with no image byte sent.
//   each -> Promise<result> = {
//       outcome: 'verified' | 'done' | 'refused' | 'stopped' | 'failed' | 'lost',
//       code,                     the unit's status number, or 'changed', 'declined', 'aborted',
//                                 'not-verified', 'phase', 'timeout', 'reply-lost', 'link',
//                                 'protocol', 'unknown', null
//       text,                     the result's line, the Python tool's
//       info,                     INFO after, or null
//       next: string[],           what to do next, the Python tool's words
//       said: string[],           every line the Python tool prints after its last prompt
//       plan }                    only when the run stopped at its own planning (refused there, or
//                                 code 'changed' before BEGIN): the plan as it is now
//   verified: a write or a select, read back and compared. done: an action or a restart.
//   refused: the unit, or the plan, refused. stopped: the countdown was stopped on the unit, the
//   plan changed, a confirmation was declined, or the signal aborted. failed: the write failed,
//   or what the unit reports does not match. lost: no answer, or the port went away.
//   An abort stops this side only: after END or ACT was sent the unit goes on, and its countdown
//   is stopped only by switching the unit off or unplugging USB.
//
// Texts
//   statusText(st, { imageRc = 0, hint = true, info = null }), refusalText(st, info = null),
//   actText(act, st, info, hint = true), STATUS, IMAGE_RC, WHY, slotName(k), version(v), and the
//   rest of words.js.

import * as W from './words.js';
export * from './words.js';

const { HELLO, INFO, BEGIN, DATA, END, RESTART, ACT, REPLY, MAX_DATA, MAX_PAYLOAD, HEAD,
    FRAME_MAX, NO_SLOT, SLOT_CAP, WB_IMAGE_MAX, WAY_NONE, WAY_PLAIN, WAY_SELECT_FIRST, WAY_IN_PLACE,
    WAY_TRIAL, WAY_SELECT, ACT_TRY, ACT_SWITCH, ACT_KEEP, ACT_PUT_BACK, TRIAL_FRONT, KEEPS_WRITE,
    ACTS_FRONT, SAME_FRONT, COUNTDOWN, IDLE, WRITING, DONE, FAILED, COUNTING, STATUS,
    slotName, slotOf, sameBytes, statusText, refusalText, damaged, hex, hex2, selected } = W;

const EMPTY = new Uint8Array(0);
const isSlot = (k) => k === 0 || k === 1;

// --- frames ---

const CRC_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
        t[n] = c >>> 0;
    }
    return t;
})();

export function crc32(bytes, crc = 0) {
    let c = ~crc >>> 0;
    for (let k = 0; k < bytes.length; k++)
        c = CRC_TABLE[(c ^ bytes[k]) & 0xFF] ^ (c >>> 8);
    return ~c >>> 0;
}

function concat(...parts) {
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let at = 0;
    for (const p of parts) {
        out.set(p, at);
        at += p.length;
    }
    return out;
}

const view = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);
const u16 = (b, o) => view(b).getUint16(o, true);
const u32 = (b, o) => view(b).getUint32(o, true);
function le32(n) {
    const b = new Uint8Array(4);
    view(b).setUint32(0, n >>> 0, true);
    return b;
}

export function frame(type, seq, payload = EMPTY) {
    const out = new Uint8Array(HEAD + payload.length + 4);
    out[0] = 0x57;
    out[1] = 0x52;
    out[2] = type;
    out[3] = seq & 0xFF;
    view(out).setUint16(4, payload.length, true);
    out.set(payload, HEAD);
    view(out).setUint32(HEAD + payload.length, crc32(out.subarray(2, HEAD + payload.length)), true);
    return out;
}

// The first whole frame in rx.bytes, which it removes, or null. Bytes that do not start a good
// frame are dropped up to the next "WR".
export function takeFrame(rx) {
    let buf = rx.bytes;
    try {
        for (;;) {
            let at = -1;
            for (let k = 0; k + 1 < buf.length; k++) {
                if (buf[k] === 0x57 && buf[k + 1] === 0x52) {
                    at = k;
                    break;
                }
            }
            if (at < 0) {
                buf = buf.subarray(Math.max(0, buf.length - 1));
                return null;
            }
            buf = buf.subarray(at);
            if (buf.length < HEAD)
                return null;
            const n = u16(buf, 4);
            if (n > MAX_PAYLOAD) {
                buf = buf.subarray(1);
                continue;
            }
            if (buf.length < HEAD + n + 4)
                return null;
            if (crc32(buf.subarray(2, HEAD + n)) !== u32(buf, HEAD + n)) {
                buf = buf.subarray(1);
                continue;
            }
            const got = { type: buf[2], seq: buf[3], payload: buf.slice(HEAD, HEAD + n) };
            buf = buf.subarray(HEAD + n + 4);
            return got;
        }
    } finally {
        rx.bytes = buf;
    }
}

// --- the link ---

export class Failure extends Error {
    constructor(message, kind = 'protocol') {
        super(message);
        this.name = 'Failure';
        this.kind = kind;
    }
}

class Aborted extends Error {}

const GONE = 'the port went away: was the unit restarted or unplugged?';
const SLOW = [HELLO, INFO, BEGIN, END, RESTART, ACT];
const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());

// The recovery port: one request at a time, each with a timeout and retries.
export class Link {
    constructor(transport, { timeout = 5000, retries = 4, longTimeout = 30000, longRetries = 3,
        poll = 200, deadline = 900000 } = {}) {
        this.transport = transport;
        this.timeout = timeout;
        this.retries = retries;
        this.longTimeout = longTimeout;
        this.longRetries = longRetries;
        this.poll = poll;
        this.deadline = deadline;
        this.seq = 0;
        this.rx = { bytes: EMPTY };
        this.maxData = MAX_DATA;
        this.front = 0;             // the front's version, from HELLO
        this.synced = false;
        this.sent = 0;              // the attempts of the last request that went out whole
        this.signal = null;         // an AbortSignal, while a run has one
    }

    close() {
        return this.transport.close();
    }

    check() {
        if (this.signal && this.signal.aborted)
            throw new Aborted();
    }

    async send(data) {
        try {
            await this.transport.write(data);
        } catch (e) {
            throw new Failure(GONE, 'link');
        }
    }

    // Puts the front's parser back in step before the first request. The parser keeps a partial
    // frame until its stated length has come, so a frame an earlier host left cut off would take
    // the start of this host's first request as its tail. FRAME_MAX zero bytes complete any such
    // frame and every frame that could start inside its bytes; their CRCs fail and the parser
    // drops them byte by byte. Zeros hold no "WR", so it then drops the zeros too.
    async sync() {
        if (!this.synced) {
            await this.send(new Uint8Array(FRAME_MAX));
            this.synced = true;
        }
    }

    async reply(type, seq, end) {
        for (;;) {
            const got = takeFrame(this.rx);
            if (got !== null) {
                if (got.type === (type | REPLY) && got.seq === seq)
                    return got.payload;
                continue;               // a late reply to an earlier attempt
            }
            const left = end - now();
            if (left <= 0)
                return null;
            this.check();
            let data;
            try {
                data = await this.transport.read(Math.min(left, 50));
            } catch (e) {
                throw new Failure(GONE, 'link');
            }
            if (data && data.length)
                this.rx.bytes = concat(this.rx.bytes, data);
        }
    }

    // The reply's payload and the number of the attempt that got it (1 = the first). Every
    // attempt has a sequence number of its own, so a late reply to an earlier attempt is not
    // taken. The requests the unit may answer only after reading the flash wait the long timeout
    // before they are sent again.
    async request(type, payload = EMPTY, { timeout = null, retries = null } = {}) {
        const slow = SLOW.includes(type);
        if (timeout === null)
            timeout = slow ? this.longTimeout : this.timeout;
        if (retries === null)
            retries = slow ? this.longRetries : this.retries;
        this.check();
        await this.sync();
        this.sent = 0;
        for (let attempt = 1; attempt <= retries; attempt++) {
            this.seq = (this.seq + 1) & 0xFF;
            await this.send(frame(type, this.seq, payload));
            this.sent = attempt;
            const got = await this.reply(type, this.seq, now() + timeout);
            if (got !== null)
                return { payload: got, attempt };
        }
        throw new Failure(`no answer from the unit after ${retries} tries: is it still in recovery? ` +
            'Unplug and replug USB. Unless a write of the running slot failed in this recovery, ' +
            'you may also restart it with Power + M4', 'link');
    }
}

function sleep(link, ms) {
    return new Promise((resolve, reject) => {
        const signal = link.signal;
        if (signal && signal.aborted) {
            reject(new Aborted());
            return;
        }
        const stop = () => {
            clearTimeout(timer);
            reject(new Aborted());
        };
        const timer = setTimeout(() => {
            if (signal)
                signal.removeEventListener('abort', stop);
            resolve();
        }, ms);
        if (signal)
            signal.addEventListener('abort', stop, { once: true });
    });
}

// --- the replies ---

// r, a reply's payload, once it holds the n bytes its type carries.
function sized(r, n, what) {
    if (r.length < n)
        throw new Failure(`${what}'s reply is ${r.length} bytes, short of the ${n} it carries: the ` +
            'unit did not answer as its protocol says; run info');
    return r;
}

const versionAt = (p, o) => [u16(p, o), u16(p, o + 2), u16(p, o + 4), u16(p, o + 6)];

export function parseHello(p) {
    if (p.length < W.HELLO_LEN)
        throw new Failure(`HELLO reply too short (${p.length} bytes)`);
    return { proto: p[0], front_version: u16(p, 1), max_data: u16(p, 3), why: p[5],
        own_desc: !!(p[6] & W.F_OWN_DESC), version: versionAt(p, W.HELLO_O_VERSION),
        pin: p.slice(W.HELLO_O_PIN, W.HELLO_O_PIN + 32) };
}

// INFO's fields; v6 is true when the version 6 fields came, v11 the version 11 ones.
export function parseInfo(p) {
    if (p.length < W.INFO_V5)
        throw new Failure(`INFO reply too short (${p.length} bytes)`);
    const v6 = p.length >= W.INFO_V6, v11 = p.length >= W.INFO_V11;
    const flags = p[1];
    const i = { v6, v11, why: p[0], running: p[2], sel_index: p[3], sel_verified: p[4],
        target: p[5], refusal: p[6], phase: p[7], len: u32(p, 8), got: u32(p, 12),
        progress: u16(p, 16), result: p[18], image_rc: p[19], written: p[20],
        established: !!(flags & W.F_ESTABLISHED), selector_ok: !!(flags & W.F_SELECTOR),
        own_desc: !!(flags & W.F_OWN_DESC), moved: v6 && !!(flags & W.F_MOVED),
        spoiled: v6 && !!(flags & W.F_SPOILED), held: v6 && !!(flags & W.F_HELD),
        read_error: v6 && !!(flags & W.F_READ_ERROR), unsettled: v6 && !!(flags & W.F_UNSETTLED),
        run_way: v6 ? p[199] : 0, run_refusal: v6 ? p[200] : 0, way: v6 ? p[201] : 0,
        batt: v6 ? p.slice(203, 203 + Math.min(p[202], W.BATT_MAX)) : EMPTY,
        count: v11 ? p[219] : 0, act: v11 ? p[220] : 0, tier: v11 ? p[221] : 0,
        acts: v11 ? Array.from(p.subarray(222, 226)) : null,     // by enum fr_act, from 1
        forced: v11 ? Array.from(p.subarray(226, 230)) : null,
        keep: v11 ? p[230] : NO_SLOT, refused: v11 ? p[231] : 0,
        version: versionAt(p, 21), pin: p.slice(29, 61), readback: p.slice(61, 93), slots: [] };
    for (let k = 0; k < 2; k++) {
        const o = 93 + 53 * k, sf = p[o];
        i.slots.push({ good: !!(sf & W.S_GOOD), has_desc: !!(sf & W.S_HAS_DESC),
            has_front: !!(sf & W.S_HAS_FRONT), tried: !!(sf & W.S_TRIED),
            started: !!(sf & W.S_STARTED), te_prefix: !!(sf & W.S_TE_PREFIX),
            length: u32(p, o + 1), crc: u32(p, o + 5),
            version: versionAt(p, o + 9), front_crc: u32(p, o + 17), pin: p.slice(o + 21, o + 53) });
    }
    return i;
}

// HELLO, and what the unit understands, by its front's version: protocol 1 from fronts 5 to 10,
// protocol 2 (ACT, the force flag, the countdown) from front 11.
export async function hello(link) {
    const h = parseHello((await link.request(HELLO)).payload);
    const want = h.front_version >= ACTS_FRONT ? W.PROTO : W.OLD_PROTO;
    if (h.proto !== want)
        throw new Failure(`the unit speaks recovery protocol ${h.proto} with front version ` +
            `${h.front_version}; this tool speaks protocol ${W.OLD_PROTO} to fronts 5 to ` +
            `${ACTS_FRONT - 1} and protocol ${W.PROTO} from front ${ACTS_FRONT}`);
    link.maxData = Math.max(1, Math.min(h.max_data, MAX_DATA));
    link.front = h.front_version;
    return h;
}

export async function getInfo(link, timeout = null) {
    const i = parseInfo((await link.request(INFO, EMPTY, { timeout })).payload);
    if ((link.front >= 6 && !i.v6) || (link.front >= ACTS_FRONT && !i.v11))
        throw new Failure(`INFO reply too short for a version ${link.front} front`);
    i.front = link.front;
    return i;
}

export async function connect(link) {
    const h = await hello(link);
    return { hello: h, info: await getInfo(link) };
}

// INFO as data for a page, with the Python tool's lines beside it.
export function describe(i) {
    const sel = i.selector_ok ? selected(i.sel_index) : null;
    const a = i.established && isSlot(i.running) ? i.running : null;
    const actOf = (act) => {
        if (!i.v11 || W.otherOf(i) === NO_SLOT)
            return null;
        const un = i.acts[act - 1], fo = i.forced[act - 1];
        const slot = W.actSlot(i, act);
        return { allowed: un === 0, forceWouldLift: un !== 0 && fo === 0,
            slot: isSlot(slot) ? slot : null, code: un,
            text: un === 0 ? W.actGoal(i, act)
                : fo === 0 ? `with --force only (${W.actText(act, un, i, false)}): ${W.actGoal(i, act)}`
                    : `refused: ${W.actText(act, fo, i, false)}` };
    };
    const lines = W.describeLines(i);
    const line = (label) => (lines.find((l) => l.startsWith(label)) || '').slice(label.length);
    const failed = i.phase === FAILED && i.way === WAY_TRIAL && [9, 10, 11].includes(i.result);
    const [battState, battText] = W.battery(i.batt);
    const hold = W.blocked(i);
    return {
        why: W.WHY[i.why] || `reason ${i.why}`,
        frontVersion: i.front || 0,
        running: a,
        established: i.established,
        selector: { read: i.selector_ok, slot: sel, verified: i.selector_ok ? i.sel_verified : null,
            settled: i.selector_ok && i.sel_verified === 1 },
        front: i.own_desc ? { version: W.version(i.version), pin: hex(i.pin) } : null,
        slots: i.slots.map((s, k) => ({
            index: k, name: slotName(k), version: s.has_desc ? W.version(s.version) : null,
            state: W.slotState(s), kind: W.slotKind(s) ? W.kindText(W.slotKind(s)) : null,
            good: s.good, tried: s.tried, started: s.tried && s.started,
            running: a === k, selected: sel === k, hasDesc: s.has_desc, hasFront: s.has_front,
            frontMatches: W.waffle(s) && i.own_desc && sameBytes(s.pin, i.pin),
            length: s.length, crc: s.crc, pin: hex(s.pin) })),
        battery: i.v6 ? { state: battState, text: battText } : null,
        tier: i.tier,
        tierText: W.TIER_WORDS[i.tier] || null,
        commands: {
            write: { allowed: isSlot(i.target), target: isSlot(i.target) ? i.target : null,
                code: isSlot(i.target) ? 0 : i.refusal, text: line('Write:    ') },
            running: i.v6 ? { allowed: i.run_way !== WAY_NONE, way: i.run_way,
                code: i.run_way !== WAY_NONE ? 0 : i.run_refusal, text: W.runWords(i) } : null,
            try: actOf(ACT_TRY), switch: actOf(ACT_SWITCH), keep: actOf(ACT_KEEP),
            putBack: actOf(ACT_PUT_BACK),
            restart: { allowed: !hold, forceWouldLift: hold && W.v11(i),
                text: hold ? `refused: ${STATUS[0x44][0]}` : 'the unit restarts' },
        },
        phase: { code: i.phase, name: W.PHASES[i.phase] || `phase ${i.phase}`, text: W.phaseWords(i) },
        countdown: i.phase === COUNTING ? i.count : 0,
        since: i.refused ? line('Since:    ') : null,
        damaged: !!damaged(i),
        held: !!hold,
        readError: i.read_error,
        moved: i.moved,
        stateLines: W.triedLines(i, failed).concat(W.retryLines(i)),
        lines,
    };
}

// --- the image ---

// The LDR block format (S-05) and the memory map of tests/oracle/S-01-boot-chain/memory_map.json.
const LDR_FINAL = 1 << 15, LDR_FIRST = 1 << 14, LDR_IGNORE = 1 << 12, LDR_INIT = 1 << 11,
    LDR_FILL = 1 << 8;
const SPACES = [[0x08000000, 0x08040000], [0x11800000, 0x11808000], [0x11900000, 0x11908000],
    [0x11A00000, 0x11A0C000], [0x11B00000, 0x11B02000], [0x80000000, 0x90000000]];
const L1 = [SPACES[1], SPACES[2], SPACES[3], SPACES[4]];
const L1_CODE = SPACES[3], DDR = SPACES[5];
const ROM_BOOT_RESERVE = [0x08000000, 0x08002000];
const RECORD_AND_ROM_STACK = [0x11B01F00, 0x11B02000];
const FIRST_STAGE_STACK = [0x11807000, 0x11808000];
const BCODE = 4;
// src/front/fr_handoff.h, as tools/mkldr.py has them
const FR_WINDOWS = [[0x11A04000, 0x11A0C000], [0x11802000, 0x11804000], [0x08002000, 0x08030000]];
const FR_SHARED = [0x08030000, 0x08031000];
const FR_DESC_ADDR = 0x08030000, FR_DESC_MAGIC = 0x4D494257, FR_DESC_VERSION = 1,
    FR_DESC_HEAD = 0x40, FR_DESC_REGIONS = 24;
const DESC_MAX = FR_DESC_HEAD + 8 * FR_DESC_REGIONS + 4;
// for the tests, which compare them with the Python tools' own
export const LAYOUT = { SPACES, ROM_BOOT_RESERVE, RECORD_AND_ROM_STACK, FIRST_STAGE_STACK, BCODE,
    FR_WINDOWS, FR_SHARED, FR_DESC_ADDR, FR_DESC_MAGIC, FR_DESC_VERSION, FR_DESC_HEAD,
    FR_DESC_REGIONS };

const meets = (a, b) => a[0] < b[1] && b[0] < a[1];
const insideAny = (r, spaces) => spaces.some((s) => s[0] <= r[0] && r[1] <= s[1]);
const fmt = (r) => `${W.hex8(r[0])}-${W.hex8(r[1])}`;
const x = (n) => n.toString(16);

// Blocks up to and including the FINAL block; throws an Error with the parser's words.
function parseBlocks(data) {
    const blocks = [];
    let off = 0;
    for (;;) {
        if (off + 16 > data.length)
            throw new Error(`truncated header at 0x${x(off)}`);
        const code = u32(data, off), target = u32(data, off + 4), count = u32(data, off + 8),
            argument = u32(data, off + 12);
        if (code >>> 24 !== 0xAD)
            throw new Error(`bad signature 0x${W.hex8(code)} at 0x${x(off)}`);
        let sum = 0;
        for (let k = 0; k < 16; k++)
            sum ^= data[off + k];
        if (sum !== 0)
            throw new Error(`bad header checksum at 0x${x(off)}`);
        const flags = code & 0xFFF0;
        const hasPayload = !(flags & LDR_FILL);
        const blk = { offset: off, code, flags, target, count, argument,
            payload_offset: hasPayload ? off + 16 : null };
        blocks.push(blk);
        off += 16 + (hasPayload ? count : 0);
        if (hasPayload && off > data.length)
            throw new Error(`truncated payload of block at 0x${x(blk.offset)}`);
        if (flags & LDR_FINAL)
            return blocks;
    }
}

// A stream's destination: the merged ranges of every block with a count that is not IGNORE.
function destinations(blocks) {
    const rs = blocks.filter((b) => b.count && !(b.flags & LDR_IGNORE))
        .map((b) => [b.target, b.target + b.count])
        .sort((p, q) => p[0] - q[0] || p[1] - q[1]);
    const merged = [];
    for (const [s, e] of rs) {
        const last = merged[merged.length - 1];
        if (last && s <= last[1])
            last[1] = Math.max(last[1], e);
        else
            merged.push([s, e]);
    }
    return merged;
}

// S-02.091's rules for a slot image, as tests/oracle/S-01-boot-chain/check_stream.py checks a
// bootloader stream: [rule, ok, detail] for each, or the parse failure alone.
function checkStream(data) {
    let blocks;
    try {
        blocks = parseBlocks(data);
    } catch (e) {
        return { rules: [['parse', false, e.message]], blocks: null };
    }
    const dest = destinations(blocks);
    const out = [];
    const list = (rs) => rs.map(fmt).join(', ');
    let bad = dest.filter((r) => !insideAny(r, SPACES));
    out.push(['inside_spaces', !bad.length, list(bad)]);
    bad = dest.filter((r) => meets(r, ROM_BOOT_RESERVE));
    out.push(['rom_boot_reserve', !bad.length, list(bad)]);
    bad = dest.filter((r) => meets(r, RECORD_AND_ROM_STACK));
    out.push(['record_and_rom_stack', !bad.length, list(bad)]);
    const codes = [];
    blocks.forEach((b, k) => {
        if ((b.code & 0xF) !== BCODE)
            codes.push(`block ${k}: ${x(b.code & 0xF)}`);
    });
    out.push(['bcode', !codes.length, codes.join(', ')]);
    const inits = [];
    blocks.forEach((b, k) => {
        if (b.flags & LDR_INIT)
            inits.push(k);
    });
    out.push(['one_init_block', inits.length === 1, `INIT blocks at [${inits.join(', ')}]`]);
    if (inits.length === 1) {
        const k = inits[0];
        const pre = destinations(blocks.slice(0, k + 1));
        bad = pre.filter((r) => !insideAny(r, L1));
        const t = blocks[k].target;
        const okT = L1_CODE[0] <= t && t < L1_CODE[1];
        out.push(['init_in_l1', !bad.length && okT,
            list(bad) + (okT ? '' : ` target ${W.hex8(t)} not in L1 code`)]);
        bad = pre.filter((r) => meets(r, DDR));
        out.push(['init_before_ddr', !bad.length, list(bad)]);
    }
    bad = dest.filter((r) => meets(r, FIRST_STAGE_STACK));
    out.push(['spares_first_stage_stack', !bad.length, list(bad)]);
    return { rules: out, blocks };
}

// The front's part of the device's scan() (src/front/fr_slots.c); the two must agree. Blocks in
// stream order up to FINAL; a block with a destination that meets the shared block must be the
// descriptor; one that meets a front window must be data inside that window, and then counts,
// in stream order, toward the front's CRC and pin. Anything else there: no waffle-boot front.
function frontScan(data, blocks) {
    const out = { descs: [], bad: '', first: null, final: null, init: null, crc: 0, pin: [] };
    for (const b of blocks) {
        const t = b.target, n = b.count, fl = b.flags;
        const end = t + n;
        const dataBlk = !(fl & LDR_FILL);
        if (fl & LDR_FIRST && out.first === null)
            out.first = t;
        if (fl & LDR_INIT && out.init === null)
            out.init = t;
        if (n && !(fl & LDR_IGNORE)) {
            if (meets([t, end], FR_SHARED)) {
                if (dataBlk && t === FR_DESC_ADDR && end <= FR_SHARED[1])
                    out.descs.push(b);
                else if (!out.bad)
                    out.bad = `a block at ${W.hex8(t)} meets the shared block and is not the descriptor`;
            } else {
                for (const [lo, hi] of FR_WINDOWS) {
                    if (!meets([t, end], [lo, hi]))
                        continue;
                    if (dataBlk && lo <= t && end <= hi) {
                        const payload = data.subarray(b.payload_offset, b.payload_offset + n);
                        out.crc = crc32(payload, out.crc);
                        out.pin.push(le32(t), le32(n), payload);
                    } else if (!out.bad) {
                        out.bad = `a ${dataBlk ? 'data' : 'FILL'} block at ${W.hex8(t)}+${x(n)} ` +
                            `meets the front's window ${W.hex8(lo)}-${W.hex8(hi)} and is not data ` +
                            'inside it';
                    }
                    break;
                }
            }
        }
        if (fl & LDR_FINAL) {
            out.final = t;
            break;
        }
    }
    return out;
}

function parseDesc(raw) {
    if (raw.length < FR_DESC_HEAD + 4)
        return null;
    const magic = u32(raw, 0), ver = u16(raw, 4), size = u16(raw, 6), entry = u32(raw, 8),
        n = u32(raw, 12), frontCrc = u32(raw, 20);
    if (magic !== FR_DESC_MAGIC || ver !== FR_DESC_VERSION || n > FR_DESC_REGIONS ||
            size !== FR_DESC_HEAD + 8 * n + 4 || size > raw.length ||
            crc32(raw.subarray(0, size - 4)) !== u32(raw, size - 4))
        return null;
    return { entry, front_crc: frontCrc, version: versionAt(raw, 0x18), pin: raw.slice(0x20, 0x40) };
}

async function sha256(bytes) {
    return new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
}

// [problems, front] for an image as a slot image: S-02.044's size, S-02.091's rules, and a
// waffle-boot front as the unit's scan() judges one.
async function imageProblems(data) {
    if (!(data.length >= 1 && data.length <= SLOT_CAP))
        return [[`the file is ${data.length} bytes; a slot image is 1 to ${SLOT_CAP} bytes (S-02.044)`], null];
    if (data.length > WB_IMAGE_MAX)
        return [[`the file is ${data.length} bytes; a waffle-boot image is at most ${WB_IMAGE_MAX} ` +
            `(0x${x(WB_IMAGE_MAX)}) bytes, since the slot's last two sectors hold its settings and ` +
            'its start log (S-02.104, S-02.095)'], null];
    const { rules, blocks } = checkStream(data);
    const probs = rules.filter((r) => !r[1])
        .map((r) => `stream rule ${r[0]} failed${r[2] ? ': ' + r[2] : ''}`);
    if (blocks === null)
        return [probs, null];
    const scan = frontScan(data, blocks);
    if (scan.descs.length !== 1) {
        probs.push(`no waffle-boot front: ${scan.descs.length} descriptor blocks at ${W.hex8(FR_DESC_ADDR)}`);
        return [probs, null];
    }
    const b = scan.descs[0];
    const front = parseDesc(data.subarray(b.payload_offset, b.payload_offset + Math.min(b.count, DESC_MAX)));
    if (front === null) {
        probs.push('no waffle-boot front: the descriptor does not parse');
        return [probs, null];
    }
    if (scan.bad)
        probs.push(`no waffle-boot front: ${scan.bad}`);
    const l2 = FR_WINDOWS[2], l1c = FR_WINDOWS[0];
    const within = (v, w) => v !== null && w[0] <= v && v < w[1];
    if (!within(scan.first, l2) || !within(scan.final, l2))
        probs.push("no waffle-boot front: the entry is not in the front's L2 window");
    if (!within(scan.init, l1c))
        probs.push("no waffle-boot front: the INIT target is not in the front's L1 code window");
    if (scan.crc !== front.front_crc)
        probs.push("no waffle-boot front: the front's bytes do not match the descriptor's CRC");
    front.pin_calc = await sha256(concat(...scan.pin));
    front.pin_ok = sameBytes(front.pin_calc, front.pin);
    return [probs, front];
}

// The descriptor's pin is a claim: a file whose front hashes to another pin is refused.
const pinRefusal = (front) => "Refused before sending: the file's descriptor claims front pin " +
    `${W.shortPin(front.pin)}, but its front's own bytes give ${W.shortPin(front.pin_calc)}: the ` +
    "file is not a build of this tree's tools. Nothing was sent.";

export async function checkImage(bytes, { knownPins = null } = {}) {
    const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    const [problems, front] = await imageProblems(data);
    const image = { bytes: data, length: data.length, sha256: await sha256(data), crc32: crc32(data),
        problems, front,
        noFrontOnly: problems.length > 0 && problems.every((p) => p.startsWith('no waffle-boot front')),
        ok: !problems.length && front !== null && front.pin_ok,
        version: front ? W.version(front.version) : null,
        pin: front ? hex(front.pin) : null,
        frontVersion: front && knownPins && knownPins[hex(front.pin)] != null
            ? knownPins[hex(front.pin)] : null,
        warnings: [], lines: [] };
    const lines = problems.map((p) => '  ' + p);
    if (problems.length) {
        lines.push('The file is not a slot image recovery writes.');
    } else if (!front.pin_ok) {
        lines.push(pinRefusal(front).replace('Refused before sending: ', '  ')
            .replace(' Nothing was sent.', ''));
        lines.push('The file is not a slot image recovery writes.');
    } else {
        lines.push(`OK: ${data.length} bytes, a bootable stream with a waffle-boot front; version ` +
            `${W.version(front.version)}, front pin ${hex(front.pin)}.`);
    }
    image.lines = lines;
    return image;
}

// --- the decisions ---

// What the unit would refuse in the image at END, judged from INFO (S-02.084): 12 when its
// front is not the running slot's (not for a trial, and not forced); for a front before version
// 14, 13 when the slot that is kept already holds it, or the target is the checked running slot
// and holds it. The unit checks again on the slots' bytes.
export function refusalBefore(i, image, target, way = WAY_PLAIN, force = false) {
    if (!i.selector_ok || !i.established || !isSlot(i.running))
        return 0;
    const a = slotOf(i, i.running);
    const front = image.front;
    if (way === WAY_TRIAL || force) {
        // another front, by trial or forced: no front rule
    } else if (a.has_front) {
        if (front === null || !sameBytes(front.pin, a.pin) || front.front_crc !== a.front_crc)
            return 12;
    } else if (i.own_desc) {       // a retry: the front the running slot had
        if (front === null || !sameBytes(front.pin, i.pin))
            return 12;
    }
    if ((i.front || 0) >= SAME_FRONT)
        return 0;                   // both slots may hold the same image
    const kept = slotOf(i, 1 - target);
    if (kept.length === image.length && kept.crc === image.crc32)
        return 13;
    // not in the damaged state, where the good running slot's own image settles the selector
    if (target === i.running && a.good && a.length === image.length && a.crc === image.crc32 &&
            !damaged(i))
        return 13;
    return 0;
}

// [slot, version] for each slot that carries the file's descriptor version but holds other bytes.
export function sameVersion(i, image) {
    const front = image.front;
    if (front === null)
        return [];
    const out = [];
    i.slots.forEach((s, k) => {
        if (s.has_desc && s.version.every((v, n) => v === front.version[n]) &&
                !(s.length === image.length && s.crc === image.crc32))
            out.push([k, s.version]);
    });
    return out;
}

// [state, the front's version or null, words] for a front pin, from the pins the page knows as
// signed: { pinHex: frontVersion | null }, or null when it has no such list.
function signedWords(pin, knownPins) {
    if (!knownPins)
        return ['unknown', null, 'this page cannot tell whether it is a signed pin: it has no ' +
            'list of signed front pins'];
    const key = hex(pin);
    if (Object.prototype.hasOwnProperty.call(knownPins, key)) {
        const v = knownPins[key];
        return ['signed', v == null ? null : v, 'a signed pin, by this page\'s list ' +
            `(${v == null ? 'its front version not known' : `front v${v}`})`];
    }
    return ['unsigned', null, "NOT a pin on this page's list of signed front pins"];
}

const DANGER = ['none', 'caution', 'no-fallback'];
const worst = (list) => DANGER[Math.max(0, ...list.map((d) => DANGER.indexOf(d)))];
const frontOf = (i) => i.front || 0;
const tooOld = (i, what) => `This unit runs front version ${frontOf(i)}: ${what} needs a version ` +
    `${ACTS_FRONT} front. Nothing was sent.`;
const notNeeded = (name) => `--force is not needed: the unit takes this ${name} without it, so it ` +
    'is sent without the flag.';

function newPlan(action, options) {
    return { action, options, allowed: false, refusal: null, forceWouldLift: false, force: false,
        way: null, target: null, request: null, said: [], confirmations: [], phrase: null,
        lines: [], countdown: false, danger: 'none' };
}

function refuse(plan, code, lines) {
    plan.said.push(...lines);
    plan.refusal = { code, text: lines.join('\n'), lines };
    return plan;
}

function allow(plan, i, confirmations) {
    plan.allowed = true;
    plan.confirmations = confirmations;
    const typed = confirmations.find((c) => c.phrase !== null);
    plan.phrase = typed ? typed.phrase : null;
    plan.lines = confirmations.flatMap((c) => c.lines);
    plan.danger = worst(confirmations.map((c) => c.danger));
    plan.countdown = plan.action !== 'restart' && W.v11(i) && !i.held;
    return plan;
}

// forceWouldLift: the same request, forced, is one the plan allows
function lift(plan, again) {
    if (!plan.allowed && !plan.options.force)
        plan.forceWouldLift = again().allowed;
    return plan;
}

// A refusal of the start condition, before BEGIN or after it.
function refusedLines(st, i, before) {
    if (st === 15)
        return [`Refused: the other slot failed its check, and ${STATUS[15][0]}. ${STATUS[15][1]} ` +
            (before ? 'Nothing was sent.' : 'Nothing was sent beyond BEGIN.')];
    const text = before ? refusalText(st, i) : statusText(st, { info: i });
    const lines = [`Refused${before ? ' before sending' : ''}: ${text}`];
    if (st === 18)
        lines.push('Ways out: ' + W.waysOut(i));
    return lines;
}

// The warning for a write the unit announced, or is expected to announce: this way, to this slot.
function writeConfirmation(i, way, target, image, signed) {
    const lines = [`Writing ${image.length} bytes to ${slotName(target)}` + ({
        [WAY_SELECT_FIRST]: `, the running slot, after the selector names ${slotName(1 - target)}`,
        [WAY_IN_PLACE]: ', the running slot, in place',
        [WAY_TRIAL]: ', for one trial start' }[way] || '') + '.'];
    lines.push(W.SETTINGS_LINE);
    if (way === WAY_TRIAL) {
        lines.push(...W.trialLines(i, image.front, target, signed), ...W.countdownNote(i));
        return { phrase: W.TRIAL_PHRASE, lines, danger: 'caution', way, target };
    }
    // a version 5 front: a warning only when a good image is replaced
    const warn = i.v6 || slotOf(i, target).good ? W.confirmLines(i, way, target) : [];
    if (warn.length)
        lines.push(...warn, ...W.countdownNote(i));
    const danger = way === WAY_IN_PLACE ? 'no-fallback'
        : damaged(i) || way === WAY_SELECT_FIRST || slotOf(i, target).good ? 'caution' : 'none';
    return { phrase: way === WAY_IN_PLACE ? W.PHRASE : warn.length ? 'yes' : null, lines, danger,
        way, target };
}

export function planWrite(i, image, options = {}) {
    const mode = options.mode || 'plain';
    const knownPins = options.knownPins || null;
    const plan = newPlan('write', { mode, force: !!options.force, knownPins });
    return lift(writePlan(plan, i, image), () => planWrite(i, image, { mode, knownPins, force: true }));
}

function writePlan(plan, i, image) {
    const running = plan.options.mode === 'running', newFront = plan.options.mode === 'new-front';
    let force = plan.options.force;
    let front = image.front;
    if (image.problems.length) {
        // only a forced write takes a bootable stream with no waffle-boot front (S-02.084)
        if (!force || !image.noFrontOnly)
            return refuse(plan, 'image', image.problems.map((p) => '  ' + p).concat(
                ['The file is not a slot image recovery writes; nothing was sent.']));
        front = null;
    }
    if (front !== null && !front.pin_ok)
        return refuse(plan, 'pin', [pinRefusal(front)]);
    if (force && frontOf(i) < ACTS_FRONT)
        return refuse(plan, 'front-too-old', [tooOld(i, '--force')]);
    if (running && !i.v6)
        return refuse(plan, 'front-too-old', [`This unit runs front version ${frontOf(i)}, which ` +
            'writes only the slot the selector does not name: --running needs a version 6 front. ' +
            'Nothing was sent.']);
    plan.said.push(...W.retryLines(i));
    if (newFront && front === null)
        return refuse(plan, 'no-front', ['Refused before sending: --new-front writes another ' +
            'waffle-boot front for one trial start, and this file carries none. Nothing was sent.']);
    // S-02.084: the front the unit runs, by the pin it reports; the same front needs no flag
    if (newFront && !i.own_desc)
        return refuse(plan, 'front-unknown', ['Refused before sending: the unit does not report ' +
            "the front it runs (the running image's descriptor is not valid), so the tool cannot " +
            "tell whether the file's front differs; and the unit takes a trial only with the " +
            'running slot checked. Run info. Nothing was sent.']);
    const other = front !== null && i.own_desc && !sameBytes(front.pin, i.pin);
    let trial = other && !running;
    // INFO's prediction, to refuse early; the unit decides at BEGIN and again at END.
    let way, target, refusal;
    if (running) {
        way = i.run_way;
        refusal = i.run_refusal;
        target = way !== WAY_NONE ? i.running : NO_SLOT;
    } else {
        way = trial ? WAY_TRIAL : WAY_PLAIN;
        target = i.target;
        refusal = i.refusal;
    }
    const risks = [];
    if (force) {
        if (front === null) {
            risks.push(W.WRITE_RISK.none);
        } else if (other && !newFront) {
            risks.push(W.WRITE_RISK.front);
            trial = false;
            way = running ? way : WAY_PLAIN;
        }
        // S-02.084: no write of the running slot under a selector that names a bad slot
        if (refusal === 3 && !slotOf(i, 1 - i.running).good)
            return refuse(plan, 5, [`Refused before sending, even with --force: ${STATUS[3][0]}, ` +
                'and that slot failed its check (status 5). Nothing was sent.']);
        if (!isSlot(target) && [3, 4, 5, 12, 15].includes(refusal)) {
            risks.push(W.WRITE_RISK[refusal]);
            target = null;              // BEGIN's answer says where it goes
        }
        if (trial && !slotOf(i, i.running).good)
            risks.push(W.WRITE_RISK[19]);
        if (!risks.length) {
            plan.said.push(notNeeded('write'));
            force = false;
        }
    }
    if (other && !force && (!newFront || frontOf(i) < TRIAL_FRONT)) {
        // another front, refused before BEGIN: with no trial asked for, or on a front before 7
        if (frontOf(i) < TRIAL_FRONT && newFront)
            return refuse(plan, 'other-front', ['Refused before sending: the file carries another ' +
                `front, and this unit runs front version ${frontOf(i)}: a front before version ` +
                `${TRIAL_FRONT} cannot take another front in recovery. The way is the test OS ` +
                'installer (wiki/Test-OS.md). Nothing was sent.']);
        let line = 'Refused before sending: the file carries another front (pin ' +
            `${W.shortPin(front.pin)}; the unit runs pin ${W.shortPin(i.pin)}). Recovery writes ` +
            'another front only for one trial start, with write --new-front IMAGE.';
        if (frontOf(i) < TRIAL_FRONT)
            line += ` This unit runs front version ${frontOf(i)}, which cannot: the way is the ` +
                'test OS installer (wiki/Test-OS.md).';
        return refuse(plan, 'other-front', [line + ' Nothing was sent.']);
    }
    if (target !== null && !isSlot(target))
        return refuse(plan, refusal, refusedLines(refusal, i, true));
    const signed = trial ? signedWords(front.pin, plan.options.knownPins) : null;
    if (trial && signed[0] === 'signed' && signed[1] !== null && signed[1] < TRIAL_FRONT)
        return refuse(plan, 'old-front-file', ["Refused before sending: the file's front is " +
            `version ${signed[1]} (this page's list of signed pins has it at that version). A ` +
            `front older than version ${TRIAL_FRONT} leaves no mark in its trial start and has no ` +
            'select of its own, so its trial could never be kept, and ' +
            `${slotName(1 - i.running)}'s image would be lost for nothing. Put an older front on ` +
            'with the test OS installer (wiki/Test-OS.md). Nothing was sent.']);
    if (trial && (damaged(i) || i.moved || (!slotOf(i, i.running).good && !force)))
        return refuse(plan, 19, [`Refused before sending: ${statusText(19, { info: i })}`]);
    if (target !== null && W.onlySelect(i, target) &&
            (slotOf(i, target).length !== image.length || slotOf(i, target).crc !== image.crc32))
        // status 18's case, judged from INFO's record of the target before BEGIN
        return refuse(plan, 18, [`Refused before sending: ${slotName(target)} is the only checked ` +
            `slot, so the unit accepts only the image it holds (${W.recordWords(slotOf(i, target))}), ` +
            'to settle the selector on it, and this file is not it (status 18).',
            'Ways out: ' + W.waysOut(i)]);
    const why = target !== null ? refusalBefore(i, image, target, way, force) : 0;
    if (why === 13 && W.leadsToOther(i))
        return refuse(plan, 13, [`Refused before sending: ${slotName(1 - target)} already holds ` +
            'this image and the selector leads to it, so there is nothing to do with it (status ' +
            `13). Write ${slotName(target)} again.`]);
    if (why)
        return refuse(plan, why, [`Refused before sending: ${statusText(why, { info: i })}`]);
    for (const [k, v] of sameVersion(i, image))
        plan.said.push(`Warning: ${slotName(k)} holds a different image with the same version ` +
            `${W.version(v)}; make bump-version gives the next one.`);
    const flags = (trial ? W.BEGIN_F_TRIAL : running ? W.BEGIN_F_RUNNING : 0) |
        (force ? W.BEGIN_F_FORCE : 0);
    plan.request = concat(le32(image.length), image.sha256, flags ? Uint8Array.of(flags) : EMPTY);
    plan.force = force;
    plan.way = target === null ? null : way;
    plan.target = target;
    plan.trial = trial;
    plan.signed = signed;
    const confirmations = [];
    if (force)
        confirmations.push({ phrase: W.forcePhrase('write'), lines: [W.FORCE_HEAD, ...risks],
            danger: risks.includes(W.WRITE_RISK[15]) ? 'no-fallback' : 'caution' });
    if (target !== null)
        confirmations.push(writeConfirmation(i, way, target, image, signed));
    return allow(plan, i, confirmations);
}

function selectConfirmation(i, k, image) {
    return { phrase: 'yes', lines: [`Selecting ${slotName(k)}: sending its image again ` +
        `(${image.length} bytes).`, ...W.selectLines(i, k), ...W.countdownNote(i)],
    danger: i.why === 2 && W.trialStart(i) ? 'caution' : 'none', way: WAY_SELECT, target: k };
}

// select: the image sent again with BEGIN flag bit 2, to select the slot tried with it.
export function planSelect(i, image, options = {}) {
    const plan = newPlan('select', { force: !!options.force });
    return lift(selectPlan(plan, i, image), () => planSelect(i, image, { force: true }));
}

function selectPlan(plan, i, image) {
    let force = plan.options.force;
    if (image.problems.length)
        return refuse(plan, 'image', image.problems.map((p) => '  ' + p).concat(
            ['The file is not a slot image recovery writes; nothing was sent.']));
    if (image.front !== null && !image.front.pin_ok)
        return refuse(plan, 'pin', [pinRefusal(image.front)]);
    if (force && frontOf(i) < ACTS_FRONT)
        return refuse(plan, 'front-too-old', [tooOld(i, '--force')]);
    if (frontOf(i) < TRIAL_FRONT)
        return refuse(plan, 'front-too-old', [`This unit runs front version ${frontOf(i)}: select ` +
            `needs a version ${TRIAL_FRONT} front. Nothing was sent.`]);
    plan.said.push(...W.retryLines(i));
    const a = i.running;
    if (!i.established || !isSlot(a))
        return refuse(plan, i.refusal, refusedLines(i.refusal, i, true));
    const here = W.trialStart(i);
    if (slotOf(i, a).tried && !here)
        return refuse(plan, 'tried-not-here', [`Refused before sending: ${W.triedLines(i).join(' ')} ` +
            'Nothing was sent.']);
    const k = here ? a : 1 - a;
    const s = slotOf(i, k);
    if (!here && !s.tried)
        return refuse(plan, 'nothing-tried', ['Refused before sending: no slot is tried, so there ' +
            'is nothing to select. A trial write comes first: write --new-front IMAGE, then a ' +
            'restart for its trial start. Nothing was sent.']);
    if (!here && !isSlot(i.target)) {
        const lines = [refusalText(i.refusal, i).replace(/\.+$/, '') + '.'].concat(W.triedLines(i));
        const sel = W.selKnown(i);
        if (sel !== null && sel[0] === k && sel[1] !== 1)
            lines.push('Restart the unit first, with no key held, for that trial start; then ' +
                'switch off and on and select here. Or try holding key 4 through the restart and ' +
                'select in its recovery.');
        return refuse(plan, i.refusal, [`Refused before sending: ${lines.join(' ')} Nothing was sent.`]);
    }
    const risks = [];
    if (force) {
        if (!here && !s.started)
            risks.push(W.SELECT_RISK[21]);
        if (W.trialBegun(i))
            risks.push(W.SELECT_RISK[19]);
        if (!risks.length) {
            plan.said.push(notNeeded('select'));
            force = false;
        }
    }
    // S-02.084: the new front's mark of its trial start
    if (!here && !s.started && !force)
        return refuse(plan, 21, [`Refused before sending: ${slotName(k)} is tried, but its front ` +
            `has left no mark of a trial start, so a select is refused. ${W.NOT_STARTED_HINT} ` +
            'Nothing was sent.']);
    if (s.length !== image.length || (s.crc !== image.crc32 && s.crc !== (image.crc32 ^ 0xFFFFFFFF) >>> 0))
        return refuse(plan, 20, [`Refused before sending: ${STATUS[20][0]}. ${slotName(k)} holds ` +
            `${s.length} bytes as tried, with another CRC-32 than this file: send the file written ` +
            'with --new-front. Nothing was sent.']);
    plan.request = concat(le32(image.length), image.sha256,
        Uint8Array.of(W.BEGIN_F_SELECT | (force ? W.BEGIN_F_FORCE : 0)));
    plan.force = force;
    plan.way = WAY_SELECT;
    plan.target = k;
    const confirmations = [];
    if (force)
        confirmations.push({ phrase: W.forcePhrase('select'), lines: [W.FORCE_HEAD, ...risks],
            danger: 'caution' });
    confirmations.push(selectConfirmation(i, k, image));
    return allow(plan, i, confirmations);
}

const ACTS = { try: ACT_TRY, switch: ACT_SWITCH, keep: ACT_KEEP, 'put-back': ACT_PUT_BACK };

// try, switch, keep, put-back (front 11): ACT on the image a slot holds, with no image sent. The
// unit's own answer in INFO decides whether force is needed, and whether it helps.
export function planAct(i, name, options = {}) {
    const plan = newPlan(name, { force: !!options.force });
    return lift(actPlan(plan, i), () => planAct(i, name, { force: true }));
}

function actPlan(plan, i) {
    const name = plan.action, act = ACTS[name], force = plan.options.force;
    if (!act)
        throw new Error(`no such action: ${name}`);
    if (frontOf(i) < ACTS_FRONT)
        return refuse(plan, 'front-too-old', [tooOld(i, name)]);
    plan.said.push(...W.retryLines(i));
    if (W.otherOf(i) === NO_SLOT)
        return refuse(plan, 2, [`Refused before sending: ${statusText(2, { info: i })} Nothing was sent.`]);
    const un = i.acts[act - 1], fo = i.forced[act - 1];
    const k = W.actSlot(i, act);
    let flag = 0;
    if (un === 0 && force) {
        plan.said.push(notNeeded(name));
    } else if (un && !force) {
        let line = `Refused before sending: ${W.actText(act, un, i).replace(/\.+$/, '')}.`;
        if (fo === 0)
            line += ' --force lifts this refusal, after a warning.';
        return refuse(plan, un, [line + ' Nothing was sent.']);
    } else if (un && fo) {
        return refuse(plan, fo, ['Refused before sending, even with --force: ' +
            `${W.actText(act, fo, i).replace(/\.+$/, '')}. Nothing was sent.`]);
    } else if (un) {
        flag = W.BEGIN_F_FORCE;
    }
    if (!isSlot(k))
        return refuse(plan, 'no-slot', [`INFO names no slot for the ${name}; run info`]);
    plan.said.push({ [ACT_TRY]: `Trying ${slotName(k)} once.`,
        [ACT_SWITCH]: `Making ${slotName(k)} the boot slot.`, [ACT_KEEP]: `Keeping ${slotName(k)}.`,
        [ACT_PUT_BACK]: `Putting ${slotName(k)} back.` }[act]);
    const lines = W.actConfirmLines(i, act, k).concat(W.countdownNote(i));
    const bad = act === ACT_SWITCH && i.tier === W.TIER_BAD;
    const warned = bad || (act !== ACT_PUT_BACK && (i.tier === W.TIER_FRONT || i.tier === W.TIER_FOREIGN)) ||
        (act === ACT_KEEP && i.why === 2 && k === i.running);
    plan.request = Uint8Array.of(act, flag);
    plan.force = !!flag;
    plan.way = W.ACT_WAYS[act];
    plan.target = k;
    if (flag)
        return allow(plan, i, [{ phrase: W.forcePhrase(name),
            lines: [W.FORCE_HEAD, ...W.forceLines(act, un, i), ...lines],
            danger: bad ? 'no-fallback' : 'caution' }]);
    // a switch is for good: a typed phrase, as for every dangerous request
    return allow(plan, i, [{ phrase: act === ACT_SWITCH ? W.SWITCH_PHRASE : 'yes', lines,
        danger: bad ? 'no-fallback' : act === ACT_SWITCH || warned ? 'caution' : 'none' }]);
}

export function planRestart(i, options = {}) {
    const plan = newPlan('restart', { force: !!options.force });
    return lift(restartPlan(plan, i), () => planRestart(i, { force: true }));
}

function restartPlan(plan, i) {
    const force = plan.options.force;
    if (force && frontOf(i) < ACTS_FRONT)
        return refuse(plan, 'front-too-old', [tooOld(i, '--force')]);
    if (i.read_error)
        plan.said.push(W.READ_ERROR);
    plan.request = EMPTY;
    if (W.blocked(i) && force) {
        plan.request = Uint8Array.of(W.BEGIN_F_FORCE);
        plan.force = true;
        return allow(plan, i, [{ phrase: W.forcePhrase('restart'), danger: 'no-fallback',
            lines: [W.FORCE_HEAD, `Without --force the unit refuses this: ${STATUS[0x44][0]}.`,
                W.stateLine(i), ...W.heldLines(i), 'Ways out: ' + W.waysOut(i), W.RESTART_RISK] }]);
    }
    if (W.blocked(i)) {
        plan.request = null;
        return refuse(plan, 0x44, [`Not restarted: ${STATUS[0x44][0]}.`, W.stateLine(i),
            ...W.heldLines(i), 'Ways out: ' + W.waysOut(i)]);
    }
    if (force)
        plan.said.push(notNeeded('restart'));
    if (damaged(i))
        plan.said.push(`${W.stateLine(i)} But ${W.reachWords(i)}.`);
    return allow(plan, i, []);
}

// --- carrying a plan out ---

function context(link, hooks) {
    const h = hooks || {};
    return { link, said: [], sent: false, pending: false, way: null,
        say(line) {
            this.said.push(line);
        },
        stage(name) {
            if (name !== this.at) {
                this.at = name;
                if (h.onStage)
                    h.onStage(name);
            }
        },
        progress: (done, total) => h.onProgress && h.onProgress(done, total),
        count: (left) => h.onCountdown && h.onCountdown(left),
        info: (i) => h.onInfo && h.onInfo(i),
        confirm: h.confirm || null, signal: h.signal || null };
}

function result(ctx, outcome, code, text, { info = null, next = [], lines = [], plan } = {}) {
    if (text)
        ctx.say(text);
    for (const line of lines.concat(next))
        ctx.say(line);
    const r = { outcome, code, text, info, next, said: ctx.said };
    if (plan)
        r.plan = plan;
    return r;
}

const AFTER_SENT = 'Stopped here: this page no longer follows the unit, and the unit was not ' +
    'stopped. Its countdown is stopped only by switching the unit off or unplugging USB in those ' +
    `${COUNTDOWN} s; otherwise the unit goes on and writes. Run info to see how it ended.`;
const IN_PLACE_NOTE = ' Once an in-place write has begun, do not switch the unit off or pull the cable.';

async function guarded(link, hooks, body) {
    const ctx = context(link, hooks);
    link.signal = ctx.signal;
    try {
        return await body(ctx);
    } catch (e) {
        if (e instanceof Aborted) {
            // END or ACT went out, or was going out with at least one attempt on the wire
            if (ctx.sent || (ctx.pending && link.sent > 0))
                return result(ctx, 'stopped', 'aborted',
                    AFTER_SENT + (ctx.way === WAY_IN_PLACE ? IN_PLACE_NOTE : ''));
            return result(ctx, 'stopped', 'aborted', 'Stopped here before the request that starts ' +
                'the write was sent: nothing was written.');
        }
        if (e instanceof Failure)
            return result(ctx, e.kind === 'link' ? 'lost' : 'failed', e.kind, `FAILED: ${e.message}`);
        throw e;
    } finally {
        link.signal = null;
    }
}

const planKey = (p) => JSON.stringify([p.action, p.allowed, p.force, p.way, p.target,
    p.request ? hex(p.request) : null, p.refusal ? p.refusal.code : null,
    p.confirmations.map((c) => [c.phrase, c.lines])]);

// HELLO and INFO, and the plan made again from them: the run goes on only when the unit's state
// still gives the plan that was confirmed.
async function replan(ctx, plan, make) {
    ctx.stage('hello');
    await hello(ctx.link);
    const i = await getInfo(ctx.link);
    ctx.info(i);
    const fresh = make(i);
    if (!fresh.allowed)
        return { stop: result(ctx, 'refused', fresh.refusal.code, fresh.refusal.lines[0],
            { info: i, lines: fresh.refusal.lines.slice(1), plan: fresh }) };
    if (planKey(fresh) !== planKey(plan))
        return { stop: result(ctx, 'stopped', 'changed', "The unit's state is not what it was " +
            'when this was confirmed, so what would happen now is not what was confirmed. Nothing ' +
            'was sent.', { info: i, plan: fresh }) };
    return { i, fresh };
}

// The warning the unit's own BEGIN answer gives, against the one that was confirmed.
async function confirmed(ctx, plan, conf) {
    const last = plan.confirmations[plan.confirmations.length - 1];
    if (last && last.way === conf.way && last.target === conf.target && last.phrase === conf.phrase &&
            JSON.stringify(last.lines) === JSON.stringify(conf.lines))
        return true;
    ctx.stage('confirm');
    return ctx.confirm !== null && !!(await ctx.confirm(conf));
}

function unverified(k, checks, tip) {
    const bad = checks.filter((c) => !c[1]).map((c) => c[0]);
    return bad.length ? `NOT VERIFIED: what the unit reports of ${slotName(k)} does not match: ` +
        `${bad.join(', ')}. ${tip}` : null;
}

// After an accepted END or ACT: from front 11 the countdown, then the write or the action, until
// it ends; in the damaged state the unit begins at once. { stop } or { info, counted }.
async function follow(ctx, way, before) {
    const link = ctx.link;
    const end = now() + link.deadline + COUNTDOWN * 1000;
    let seen = null, left = null, i, ph;
    try {
        for (;;) {
            i = await getInfo(link);
            ctx.info(i);
            ph = i.phase;
            if (seen === null && ph !== COUNTING && W.v11(i) && before != null && before.held)
                ctx.say('The unit began at once, with no countdown: it holds the power-off while a ' +
                    'write of this recovery has left no checked slot to start.');
            if (ph === COUNTING) {
                if (seen !== COUNTING) {
                    ctx.say(W.COUNTDOWN_LINE);
                    ctx.stage('countdown');
                }
                if (i.count !== left) {
                    left = i.count;
                    ctx.count(left);
                }
            } else if (ph !== WRITING && ph !== DONE && ph !== FAILED) {
                return { stop: result(ctx, 'failed', 'phase',
                    `The unit is not writing (phase ${ph}); run info.`, { info: i }) };
            }
            if (ph === WRITING) {
                ctx.stage('writing');
                ctx.progress(i.progress, 1000);
            }
            seen = ph;
            if (ph === DONE || ph === FAILED)
                break;
            if (now() > end)
                return { stop: result(ctx, 'failed', 'timeout', 'The write did not finish in ' +
                    `${Math.round(link.deadline / 1000) + COUNTDOWN} s; run info.`, { info: i }) };
            await sleep(link, link.poll);
        }
    } catch (e) {
        if (!(e instanceof Failure))
            throw e;
        if (seen === COUNTING)
            throw new Failure(`${e.message}. The unit was counting down: if it was switched off or ` +
                'USB was unplugged, it stopped, and nothing was written (status 24); else it goes ' +
                `on when the countdown ends${way === WAY_IN_PLACE
                    ? ', in place: then do not switch the unit off or pull the cable' : ''}. Run ` +
                'info once the unit answers again', e.kind);
        if (way === WAY_IN_PLACE)
            throw new Failure(`${e.message}. The in-place write may still be running: do not ` +
                'switch the unit off or pull the cable; wait, then run info', e.kind);
        throw e;
    }
    const counted = left !== null;
    if (counted && ph === FAILED && i.result === 24)
        return { stop: result(ctx, 'stopped', 24, `Stopped: ${statusText(24, { info: i })}`, { info: i }) };
    if (counted && i.refused === 24)
        // S-02.093: a stop leaves the write this recovery made before in INFO, and says so
        return { stop: result(ctx, 'stopped', 24, `Stopped: ${statusText(24, { info: i })} The ` +
            'unit still reports the write it made earlier in this recovery ' +
            `(${W.madeWords(i)}); nothing of this one was written.`, { info: i }) };
    return { info: i, counted };
}

// END's reply was lost and END sent again found no transfer open: whether INFO now reports this
// transfer ('info': its write, or its refusal), a refusal while INFO shows a write made earlier
// in this recovery ('refused'), or cannot tell ('unknown'). From version 8 a refused END leaves
// that earlier write in INFO (S-02.093); a write that ran shows the way BEGIN announced and,
// done, this file's read-back. A failed write with that way is this one or the earlier one,
// unless INFO before BEGIN, idle or ended, showed something else.
export function endLost(front, before, i, way, sha) {
    if (front < KEEPS_WRITE)
        return 'info';
    if (front >= ACTS_FRONT)        // INFO keeps the refusal that follows a write
        return i.refused || (i.phase === FAILED && i.way === WAY_NONE) ? 'refused' : 'info';
    if (i.way !== way || (i.phase === DONE && !sameBytes(i.readback, sha)))
        return 'refused';
    if (i.phase === DONE)
        return 'info';
    if (before != null && [IDLE, DONE, FAILED].includes(before.phase) &&
            (before.phase !== i.phase || before.result !== i.result || before.way !== i.way))
        return 'info';
    return 'unknown';
}

// DATA, END and the write's progress: { stop } or { info }. way is the write BEGIN announced,
// before INFO from before BEGIN.
async function transfer(ctx, image, i, way, before) {
    const link = ctx.link, data = image.bytes;
    let off = 0;
    ctx.stage('sending');
    while (off < data.length) {
        const chunk = data.subarray(off, off + link.maxData);
        let r;
        try {
            r = (await link.request(DATA, concat(le32(off), chunk))).payload;
        } catch (e) {
            if (!(e instanceof Failure))
                throw e;
            // S-02.084: a transfer the host left ends with nothing written (status 24)
            const said = /[.?]$/.test(e.message) ? e.message : e.message + '.';
            throw new Failure(`${said} The transfer stopped at byte ${off} of ${data.length}, ` +
                'before END: nothing was written. Once the unit is back, run the command again: ' +
                'it begins anew.', e.kind);
        }
        r = sized(r, W.DATA_REPLY_LEN, 'DATA');
        const st = r[0], nxt = u32(r, 1);
        if (st === 0x42 && nxt <= data.length) {    // out of step: go on where the unit is
            off = nxt;
            continue;
        }
        if (st)
            return { stop: result(ctx, 'refused', st, `Refused at byte ${off}: ${statusText(st)}`) };
        if (nxt <= off)
            throw new Failure(`DATA's reply names byte ${nxt} as the next after byte ${off} was ` +
                'taken: the unit did not answer as its protocol says. No END was sent, and nothing ' +
                'was written; run info');
        off = nxt;
        ctx.progress(off, data.length);
    }
    ctx.stage('end');
    ctx.pending = true;
    const { payload, attempt } = await link.request(END);
    ctx.pending = false;
    ctx.sent = true;
    let st = sized(payload, W.END_REPLY_LEN, 'END')[0];
    const imageRc = payload[1];
    const lost = st === 0x41 && attempt > 1;        // taken and ended, or refused: endLost()
    if (st && attempt > 1 && (st === 1 || st === 0x41))
        st = 0;                                     // an earlier END got through
    if (st) {
        ctx.sent = false;
        return { stop: result(ctx, 'refused', st, `Refused: ${statusText(st, { imageRc, info: i })}`,
            { lines: st === 18 ? ['Ways out: ' + W.waysOut(i)] : [] }) };
    }
    const f = await follow(ctx, way, before);
    if (f.stop)
        return f;
    i = f.info;
    // a countdown seen: END was taken, whatever its lost reply said
    const how = lost && !f.counted ? endLost(link.front, before, i, way, image.sha256) : 'info';
    if (how === 'refused') {
        if (i.refused)              // front 11: INFO keeps the reason
            return { stop: result(ctx, 'refused', i.refused,
                `Refused: ${statusText(i.refused, { info: i })}`, { info: i,
                    lines: ["END's reply was lost; the unit still reports the write made earlier " +
                        `in this recovery (${W.madeWords(i)}). Nothing of this file was written.`]
                        .concat(i.refused === 18 ? ['Ways out: ' + W.waysOut(i)] : []) }) };
        if (i.phase === FAILED && i.way === WAY_NONE)       // no earlier write: INFO has it
            return { stop: result(ctx, 'refused', i.result,
                `Refused: ${statusText(i.result, { imageRc: i.image_rc, info: i })}`,
                { info: i, lines: i.result === 18 ? ['Ways out: ' + W.waysOut(i)] : [] }) };
        return { stop: result(ctx, 'refused', null, "Refused: the unit did not take this write. " +
            "END's reply was lost, and the unit still reports the write made earlier in this " +
            `recovery (${W.madeWords(i)}), as a refused END leaves it (S-02.093); the reason was ` +
            'only in the lost reply. Nothing of this file was written. Run info, then write again.',
        { info: i }) };
    }
    if (how === 'unknown')
        return { stop: result(ctx, 'failed', 'unknown', "END's reply was lost, and the unit " +
            'reports a failed write with the way this one was announced: ' +
            `${statusText(i.result, { hint: false })}. That is either a write made earlier in ` +
            'this recovery, which a refused END leaves in place (S-02.093), or this write, failed: ' +
            'what the unit reports cannot tell the two apart. Run info to see the slots and the ' +
            'selector before writing again.', { info: i, lines: W.retryLines(i) }) };
    ctx.progress(1000, 1000);
    if (i.phase === FAILED)
        return { stop: result(ctx, 'failed', i.result, 'FAILED: ' +
            statusText(i.result, { imageRc: i.image_rc, hint: W.plainHint(i), info: i }),
        { info: i, lines: (way === WAY_TRIAL ? W.triedLines(i, true) : []).concat(W.retryLines(i)) }) };
    return { info: i };
}

// After a trial write: the slot tried, its record marked, the selector as S-02.084 leaves it.
function trialDone(ctx, i, image) {
    const k = i.written;
    const s = isSlot(k) ? i.slots[k] : null;
    const want = k === 0 ? [0x00, 0xFE] : [0xFF, 0xFF];
    const crc = image.crc32;
    const no = unverified(k, [
        ['the read-back SHA-256', sameBytes(i.readback, image.sha256)],
        ["the slot's record length", s !== null && s.length === image.length],
        ["the slot's record CRC-32", s !== null && (s.crc === crc || s.crc === (crc ^ 0xFFFFFFFF) >>> 0)],
        ['the slot is tried', s !== null && s.tried],
        [`the selector, which should read ${hex2(want[0])} ${hex2(want[1])}`,
            i.selector_ok && i.sel_index === want[0] && i.sel_verified === want[1]]],
    `Run info first: it says whether a trial start of ${slotName(k)} would come. Do not select it; ` +
        'write --new-front again if info does not show it tried.');
    if (no)
        return result(ctx, 'failed', 'not-verified', no, { info: i });
    const a = slotName(1 - k), m = slotName(k);
    const sel = W.v11(i) ? 'keep (or select IMAGE)' : 'select IMAGE';
    return result(ctx, 'verified', 0, `Verified: ${m} holds this file for one trial start ` +
        `(${image.length} bytes, SHA-256 ${hex(image.sha256)}), read back by the unit; the selector ` +
        `reads ${hex2(want[0])} ${hex2(want[1])}.`, { info: i, next: [
        'Next: restart the unit (wbrecover.py restart) with no key held and let it come up: that ' +
            `start tries ${m} once and records itself in ${m}. If it does not come up, switch it ` +
            `off and on: ${a} boots again.`,
        'To keep the new front, select it: after that start, switch the unit off and on and run ' +
            `wbrecover.py ${sel} from ${a}'s recovery (Power + M4). Or try holding key 4 (M4) ` +
            "through the restart (not yet proven on a unit): if the new front's recovery opens, " +
            `run wbrecover.py ${sel} there. A trial start into recovery that is not selected there ` +
            'records nothing: the trial write must then be repeated.'] });
}

export function runWrite(link, plan, image, hooks) {
    return guarded(link, hooks, async (ctx) => {
        const got = await replan(ctx, plan, (info) => planWrite(info, image, plan.options));
        if (got.stop)
            return got.stop;
        let i = got.i;
        const before = i, fresh = got.fresh;
        const { trial, force } = fresh;
        let way = fresh.way, target = fresh.target;
        ctx.stage('begin');
        if (i.v6) {
            // the confirmation is on BEGIN's answer: the way and the slot the unit will write
            const r = sized((await link.request(BEGIN, fresh.request)).payload, W.BEGIN_REPLY_LEN, 'BEGIN');
            if (r[0]) {
                const lines = refusedLines(r[0], i, false);
                return result(ctx, 'refused', r[0], lines[0], { info: i, lines: lines.slice(1) });
            }
            way = r[1];
            target = r[2];
            const ways = trial ? [WAY_TRIAL] : [WAY_PLAIN, WAY_SELECT_FIRST, WAY_IN_PLACE];
            if (!ways.includes(way) || !isSlot(target) || (trial && target === i.running))
                throw new Failure(`BEGIN's reply announces way ${way} to slot index ${target}, not ` +
                    `the ${trial ? 'trial write of the other slot' : 'write'} the tool asked for. ` +
                    "Nothing was written: no END was sent. Likely the unit's state changed, for " +
                    'instance this recovery already wrote the selector or began a write; run info');
            i = await getInfo(link);    // the state now; for an in-place write, the battery it asked
            ctx.info(i);
            const why = refusalBefore(i, image, target, way, force);
            if (why)
                return result(ctx, 'refused', why, `Refused: ${statusText(why, { info: i })}. ` +
                    'Nothing was sent beyond BEGIN.', { info: i });
            if (!(await confirmed(ctx, plan, writeConfirmation(i, way, target, image, fresh.signed))))
                return result(ctx, 'stopped', ctx.confirm ? 'declined' : 'changed',
                    'Stopped: no image byte was sent, ' + (way === WAY_TRIAL ? 'no END, ' : '') +
                    'and nothing was written.', { info: i });
        }
        const settle = damaged(i);      // S-02.084: the write settles the selector on its target
        ctx.way = way;
        if (!i.v6) {
            const st = sized((await link.request(BEGIN, fresh.request)).payload, 1, 'BEGIN')[0];
            if (st)
                return result(ctx, 'refused', st, `Refused: ${statusText(st, { info: i })}`, { info: i });
        }
        const t = await transfer(ctx, image, i, way, before);
        if (t.stop)
            return t.stop;
        i = t.info;
        ctx.stage('done');
        if (way === WAY_TRIAL)
            return trialDone(ctx, i, image);
        const k = i.written;
        const s = isSlot(k) ? i.slots[k] : null;
        // S-02.084: an in-place write, and every write in the damaged state, ends with the
        // selector settled on its target, {k, 01}; where it names slot 1 already, it is left
        const settled = settle || i.way === WAY_IN_PLACE;
        const sel = settled
            ? [`the selector, which should name ${slotName(k)}${k === 1 ? '' : ' with verified 01'}`,
                i.selector_ok && selected(i.sel_index) === k && (i.sel_verified === 1 || k === 1)]
            : [`the selector, which should read ${hex2(k)} 00`,       // S-02.048
                i.selector_ok && i.sel_index === k && i.sel_verified === 0];
        const no = unverified(k, [
            ['the read-back SHA-256', sameBytes(i.readback, image.sha256)],
            ["the slot's record length", s !== null && s.length === image.length],
            ["the slot's record CRC-32", s !== null && s.crc === image.crc32],
            ['the slot is checked', s !== null && s.good], sel],
        settle ? 'Run info first, and write again as it says.' : W.SAFE_HINT);
        if (no)
            return result(ctx, 'failed', 'not-verified', no, { info: i });
        let next;
        if (settled && i.sel_verified === 1)
            next = `The selector names ${slotName(k)} with verified 01: the next start takes it as ` +
                'it is. Restart the unit (wbrecover.py restart) without holding a key.';
        else if (settled)
            next = `The selector names ${slotName(k)} (verified ${hex2(i.sel_verified)}): the next ` +
                'start checks it and settles it. Restart the unit (wbrecover.py restart) without ' +
                'holding a key.';
        else
            next = `The selector now names ${slotName(k)}, to be checked at the next start. ` +
                'Restart the unit (wbrecover.py restart) without holding a key.';
        return result(ctx, 'verified', 0, `Verified: ${slotName(k)} holds this file ` +
            `(${image.length} bytes, SHA-256 ${hex(image.sha256)}), read back by the unit.`,
        { info: i, next: [next] });
    });
}

export function runSelect(link, plan, image, hooks) {
    return guarded(link, hooks, async (ctx) => {
        const got = await replan(ctx, plan, (info) => planSelect(info, image, plan.options));
        if (got.stop)
            return got.stop;
        let i = got.i;
        const before = i, want = got.fresh.target;
        ctx.stage('begin');
        const r = sized((await link.request(BEGIN, got.fresh.request)).payload, W.BEGIN_REPLY_LEN, 'BEGIN');
        if (r[0]) {
            const lines = refusedLines(r[0], i, false);
            return result(ctx, 'refused', r[0], lines[0], { info: i, lines: lines.slice(1) });
        }
        if (r[1] !== WAY_SELECT || r[2] !== want)
            throw new Failure(`BEGIN's reply announces way ${r[1]} to slot index ${r[2]}, not a ` +
                `select of ${slotName(want)}. Nothing was written: no END was sent. Likely the ` +
                "unit's state changed since info; run info");
        i = await getInfo(link);
        ctx.info(i);
        if (!(await confirmed(ctx, plan, selectConfirmation(i, want, image))))
            return result(ctx, 'stopped', ctx.confirm ? 'declined' : 'changed',
                'Stopped: no image byte was sent, no END, and nothing was written.', { info: i });
        ctx.way = WAY_SELECT;
        const t = await transfer(ctx, image, i, WAY_SELECT, before);
        if (t.stop)
            return t.stop;
        i = t.info;
        ctx.stage('done');
        const k = i.written;
        const s = isSlot(k) ? i.slots[k] : null;
        const no = unverified(k, [
            ['the read-back SHA-256', sameBytes(i.readback, image.sha256)],
            ["the slot's record length", s !== null && s.length === image.length],
            ["the slot's record CRC-32", s !== null && s.crc === image.crc32],
            ['the slot is checked', s !== null && s.good && !s.tried],
            [`the selector, which should read ${hex2(k)} 00`,
                i.selector_ok && i.sel_index === k && i.sel_verified === 0]], W.SAFE_HINT);
        if (no)
            return result(ctx, 'failed', 'not-verified', no, { info: i });
        return result(ctx, 'verified', 0, `Selected: ${slotName(k)} holds this file ` +
            `(${image.length} bytes, SHA-256 ${hex(image.sha256)}), read back by the unit, and the ` +
            'selector names it, to be checked at the next start. Restart the unit (wbrecover.py ' +
            'restart) without holding a key.', { info: i });
    });
}

// What INFO must show after an action on slot k ended well (S-02.084).
export function actChecks(act, before, i, k) {
    const s = isSlot(k) ? i.slots[k] : null;
    const was = isSlot(k) ? before.slots[k] : null;
    const sel = i.selector_ok ? [i.sel_index, i.sel_verified] : null;
    const settled = damaged(before);
    const reads = (a, b) => sel !== null && sel[0] === a && sel[1] === b;
    if (act === ACT_TRY) {
        const want = k === 0 ? [0x00, 0xFE] : [0xFF, 0xFF];
        const checks = [[`the selector, which should read ${hex2(want[0])} ${hex2(want[1])}`,
            reads(want[0], want[1])]];
        if (was !== null && (was.good || was.tried))
            checks.push(['the slot is tried', s !== null && s.tried]);
        return checks;
    }
    const valid = ['the slot is checked and not tried', s !== null && s.good && !s.tried];
    if (act === ACT_PUT_BACK) {
        const checks = [valid];
        const old = W.selKnown(before);
        if (old !== null && old[0] === k && old[1] !== 1 && !settled)
            checks.push([`the selector, which should name ${slotName(1 - k)} again`,
                sel !== null && selected(sel[0]) === 1 - k]);
        return checks;
    }
    const checks = [];
    if (act === ACT_KEEP || was.good || was.tried)
        checks.push(valid);
    if (settled || (act === ACT_SWITCH && !was.good && !was.tried))
        // the selector settled on k, or a bad slot named with verified 01
        checks.push([`the selector, which should name ${slotName(k)} with verified 01`,
            sel !== null && selected(sel[0]) === k && (sel[1] === 1 || (k === 1 && settled))]);
    else
        checks.push([`the selector, which should read ${hex2(k)} 00`, reads(k, 0)]);
    return checks;
}

export function runAct(link, plan, hooks) {
    return guarded(link, hooks, async (ctx) => {
        const name = plan.action, act = ACTS[name];
        const got = await replan(ctx, plan, (info) => planAct(info, name, plan.options));
        if (got.stop)
            return got.stop;
        const i = got.i, k = got.fresh.target;
        ctx.stage('begin');
        ctx.way = W.ACT_WAYS[act];
        ctx.pending = true;
        const { payload, attempt } = await link.request(ACT, got.fresh.request);
        ctx.pending = false;
        ctx.sent = true;
        const r = sized(payload, W.ACT_REPLY_LEN, 'ACT');
        let way = r[1], slot = r[2];
        if (r[0] === 1 && attempt > 1) {
            way = W.ACT_WAYS[act];      // an earlier ACT got through
            slot = k;
        } else if (r[0]) {
            ctx.sent = false;
            return result(ctx, 'refused', r[0], `Refused: ${W.actText(act, r[0], i)}`, { info: i });
        }
        if (way !== W.ACT_WAYS[act] || slot !== k)
            return result(ctx, 'failed', 'protocol', `The unit took the ${name} as way ${way} to ` +
                `slot index ${slot}, not as the tool asked. ` + (i.held ? 'Run info.'
                : 'It counts down before it writes: switch the unit off or unplug USB now to stop ' +
                    'it with the flash untouched; then run info.'), { info: i });
        const f = await follow(ctx, way, i);
        if (f.stop)
            return f.stop;
        ctx.progress(1000, 1000);
        ctx.stage('done');
        const after = f.info;
        const K = slotName(k), other = slotName(1 - k);
        if (after.phase === FAILED)
            return result(ctx, 'failed', after.result, 'FAILED: ' +
                statusText(after.result, { hint: W.plainHint(after), info: after }),
            { info: after, lines: W.triedLines(after).concat(W.retryLines(after)) });
        const no = unverified(k, actChecks(act, i, after, k), 'Run info first.');
        if (no)
            return result(ctx, 'failed', 'not-verified', no, { info: after });
        const reads = `${hex2(after.sel_index)} ${hex2(after.sel_verified)}`;
        if (act === ACT_TRY)
            return result(ctx, 'done', 0, `Done: ${K} is marked for one trial start; the selector ` +
                `reads ${reads}.`, { info: after, next: ['Next: restart the unit (wbrecover.py ' +
                    `restart) with no key held: that start runs ${K} once. If it does not come up, ` +
                    `switch it off and on: ${other} boots again. To keep ${K} once it has come up: ` +
                    `wbrecover.py keep, from its own recovery in that start or from ${other}'s ` +
                    `after it, ${W.SLOT_MENU}.`] });
        if (act === ACT_SWITCH)
            return result(ctx, 'done', 0, `Done: ${K} is the boot slot (the selector reads ` +
                `${reads}). Restart the unit (wbrecover.py restart) without holding a key: it ` +
                `starts ${K}.`, { info: after });
        if (act === ACT_KEEP)
            return result(ctx, 'done', 0, `Kept: ${K} is the boot slot, to be checked again at the ` +
                'next start. Restart the unit (wbrecover.py restart) without holding a key.',
            { info: after });
        return result(ctx, 'done', 0, `Put back: ${K}'s record is good again and it is no longer ` +
            `tried; the selector reads ${reads}.`, { info: after });
    });
}

export function runRestart(link, plan, hooks) {
    return guarded(link, hooks, async (ctx) => {
        const got = await replan(ctx, plan, (info) => planRestart(info, plan.options));
        if (got.stop)
            return got.stop;
        const i = got.i;
        ctx.stage('begin');
        let r;
        try {
            r = (await link.request(RESTART, got.fresh.request)).payload;
        } catch (e) {
            if (!(e instanceof Failure) || !link.sent)
                throw e;
            // the unit resets once its reply is out: a reply lost after the request went out
            // most likely means it restarted
            return result(ctx, 'done', 'reply-lost', 'The restart request went out, but no reply ' +
                'came: the unit most likely restarted. Check that it starts; if it is still in ' +
                'recovery, run restart again.');
        }
        r = sized(r, 1, 'RESTART');
        if (r[0])
            return result(ctx, 'refused', r[0], `Refused: ${statusText(r[0], { info: i })}`,
                { info: i, lines: r[0] === 0x44 ? ['Ways out: ' + W.waysOut(i)] : [] });
        ctx.stage('done');
        return result(ctx, 'done', 0, 'The unit restarts.', { info: i });
    });
}
