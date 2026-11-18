// SPDX-License-Identifier: GPL-2.0-or-later
//
// Copying a file to the unit over MTP and proving it by reading it back: what tools/mtpcp.py's put
// and verify do, with no DOM and no WebUSB in it (usb.js is the transport). It runs unchanged as
// <script type="module"> and in Node 20 or later, and needs only crypto.subtle.
//
// It sends only the operations any responder is required to handle, and checks what comes back
// rather than trusting it: GetDeviceInfo, OpenSession, CloseSession, GetStorageIDs, GetStorageInfo,
// GetObjectHandles, GetObjectInfo, GetObject, DeleteObject, SendObjectInfo and SendObject.
//   - never GetPartialObject: a file is read whole with GetObject (S-16.055);
//   - never GetObjectPropList (S-16.057);
//   - the root is listed with parent 0xFFFFFFFF (S-16.177);
//   - a data phase ends at a short packet or at its length, whichever comes first (S-16.056);
//   - a data container left from an earlier transaction is dropped (S-16.064);
//   - a successful GetObject is not taken as proof of anything: the bytes are compared;
//   - a folder is never deleted, since folder deletes are recursive (S-16.067);
//   - nothing is changed on a store that is read-only (AccessCapability not 0, S-10b.031);
//   - the copy is checked afterwards from a new session's listing.
//
// EXPORTED API
//
//   INSTALL_NAME                 the name the file gets at the top of the unit's storage
//   transport = { mps,                                   the bulk endpoints' packet size
//                 write(bytes, timeoutMs): Promise<void>,   one bulk OUT transfer; empty: a
//                                                           zero-length packet
//                 read(size, timeoutMs): Promise<Uint8Array | null>,  one bulk IN transfer, ended
//                                                           by a short packet; null when nothing
//                                                           came within timeoutMs
//                 close(): Promise<void> }
//       write() and read() reject when USB fails: an error with .gone = true is the device gone,
//       one with .timeout = true a write that did not finish in time.
//   class MtpFailure extends Error   .kind = 'refused' | 'failed' | 'mismatch' | 'readonly' |
//       'timeout' | 'lost', .op (the operation's name, for refused), .response (its name)
//   class Mtp(transport)          the transactions; .stale counts dropped data containers
//   mtpString(text) -> Uint8Array          parseObject(handle, data) -> { handle, format, size,
//   objectInfoDataset(storage, name,           parent, name, modified, isFolder }
//       size, modified, folder) -> Uint8Array   responseName(code) -> 'General_Error (0x2002)'
//   checkName(name) -> string | null      why a name cannot go at the top of the storage
//
//   list(transport, { hooks }) -> Promise<{ device, objects, access, total, free, lines }>
//       rejects with an MtpFailure
//   verify(transport, content, { name, hooks }) -> Promise<outcome>
//   put(transport, content, { name, fileName, modified, hooks }) -> Promise<outcome>
//       deletes the old copy, sends content, reads it back in a new session and compares.
//       modified: 'YYYYMMDDTHHMMSS' (now, in UTC, by default)
//   outcome = { outcome: 'verified' | 'mismatch' | 'readonly' | 'refused' | 'failed' | 'timeout' |
//       'lost', text, response: string | null, changed: bool, sha256: hex | null, stale,
//       lines: string[], advice: string[] }
//       lines: what was done, as the Python tool prints it; advice: what to do now. changed: a
//       change was sent to the unit, so the copy there may be empty or short unless verified.
//   hooks = { onStage(name, text), onProgress(done, total), onLine(text) }   all optional

// The name the unit's developer menu writes as the OS.
export const INSTALL_NAME = 'os.ldr';

// --- words ---

export const STAGES = {
    drain: 'Clearing what an earlier program left...',
    device: 'Asking the unit what it is...',
    session: 'Opening a session...',
    list: 'Reading the list of files...',
    delete: 'Deleting the old copy...',
    send: 'Sending the file...',
    readback: 'Reading the copy back to compare it...',
    done: '',
};

export const WORDS = {
    verified: (name, n) => `Verified: ${name} on the unit holds the file's ${n} bytes, read back and compared.`,
    mismatch: (name) => `The copy of ${name} on the unit does not match the file.`,
    readonly: "The unit's storage is read-only: nothing was changed.",
    refused: (op, response) => `The unit refused ${op}: it answered ${response}.`,
    timeout: 'The unit did not finish a transfer in time.',
    lost: 'The unit went away during the copy.',
    failed: (message) => `The copy stopped: ${message}.`,
    notGood: (name) => `${name} on the unit is not a good copy now: do not write it as the OS. A failed ` +
        'copy may leave an empty or short file.',
    unchanged: 'Nothing was changed on the unit.',
    again: 'Send it again.',
    timeoutAdvice: 'Turn MTP mode off and on in the developer menu, or switch the unit off and on, then ' +
        'send it again.',
    lostAdvice: 'Check the cable, open MTP mode in the developer menu again, and send it again.',
};

// --- the protocol's numbers ---

export const CMD = 1, DATA = 2, RESP = 3, EVENT = 4;
export const OP_DEVICE_INFO = 0x1001, OP_OPEN = 0x1002, OP_CLOSE = 0x1003, OP_STORAGE_IDS = 0x1004;
export const OP_STORAGE_INFO = 0x1005, OP_HANDLES = 0x1007, OP_OBJECT_INFO = 0x1008, OP_GET_OBJECT = 0x1009;
export const OP_DELETE = 0x100B, OP_SEND_INFO = 0x100C, OP_SEND = 0x100D;
export const OK = 0x2001, ALREADY_OPEN = 0x201E, INVALID_HANDLE = 0x2009, INVALID_PARENT = 0x201A;
export const ROOT = 0xFFFFFFFF;
export const FORMAT_UNDEFINED = 0x3000, FORMAT_FOLDER = 0x3001;

export const RESPONSES = {
    0x2001: 'OK', 0x2002: 'General_Error', 0x2003: 'Session_Not_Open',
    0x2005: 'Operation_Not_Supported', 0x2006: 'Parameter_Not_Supported',
    0x2007: 'Incomplete_Transfer', 0x2008: 'Invalid_StorageID', 0x2009: 'Invalid_ObjectHandle',
    0x200C: 'Store_Full', 0x200D: 'Object_WriteProtected', 0x200F: 'Access_Denied',
    0x2015: 'No_Valid_ObjectInfo', 0x2019: 'Device_Busy', 0x201A: 'Invalid_ParentObject',
    0x201D: 'Invalid_Parameter', 0x201E: 'Session_Already_Open', 0xA806: 'Invalid_Dataset',
};

export const TIMEOUT_MS = 60000;    // one bulk transfer, or the wait for a response
export const READ_SIZE = 65536;     // a multiple of both packet sizes, so a transfer cannot overflow
export const WRITE_SIZE = 65536;
const DRAIN_MS = 100;

export const responseName = (code) =>
    `${RESPONSES[code] || 'an unknown code'} (0x${code.toString(16).padStart(4, '0')})`;

export class MtpFailure extends Error {
    constructor(kind, message, { op = null, response = null } = {}) {
        super(message);
        this.name = 'MtpFailure';
        this.kind = kind;
        this.op = op;
        this.response = response;
    }
}

const failed = (message) => new MtpFailure('failed', message);

// --- containers and datasets ---

const dv = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);

function concat(parts) {
    let n = 0;
    for (const p of parts)
        n += p.length;
    const out = new Uint8Array(n);
    let at = 0;
    for (const p of parts) {
        out.set(p, at);
        at += p.length;
    }
    return out;
}

const hexOf = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');

// An MTP String: a count of UTF-16 units with the terminating NUL, then the units.
export function mtpString(text) {
    if (!text)
        return Uint8Array.of(0);
    const out = new Uint8Array(1 + 2 * (text.length + 1));
    out[0] = text.length + 1;
    for (let k = 0; k < text.length; k++) {
        const c = text.charCodeAt(k);
        out[1 + 2 * k] = c & 0xFF;
        out[2 + 2 * k] = c >> 8;
    }
    return out;
}

// A dataset's fields; a field the dataset does not carry reads as zero or empty (S-16.056).
class Reader {
    constructor(data) {
        this.data = data;
        this.pos = 0;
    }

    num(size) {
        const at = this.pos;
        this.pos += size;
        if (at + size > this.data.length)
            return 0;
        const v = dv(this.data);
        if (size === 1)
            return v.getUint8(at);
        if (size === 2)
            return v.getUint16(at, true);
        if (size === 4)
            return v.getUint32(at, true);
        return Number(v.getBigUint64(at, true));
    }

    string() {
        const n = this.num(1);
        const chunk = this.data.subarray(Math.min(this.pos, this.data.length),
            Math.min(this.pos + 2 * n, this.data.length));
        this.pos += 2 * n;
        let s = '';
        for (let k = 0; k + 1 < chunk.length; k += 2)
            s += String.fromCharCode(chunk[k] | chunk[k + 1] << 8);
        const nul = s.indexOf('\0');
        return nul < 0 ? s : s.slice(0, nul);
    }

    skipArray(size) {
        const n = this.num(4);
        this.pos += n * size;
    }
}

export function parseObject(handle, data) {
    const r = new Reader(data);
    r.num(4);                       // StorageID
    const format = r.num(2);
    r.num(2);                       // ProtectionStatus
    const size = r.num(4);
    r.pos += 2 + 6 * 4;             // the thumb and image fields
    const parent = r.num(4);
    r.pos += 2 + 4 + 4;             // AssociationType, AssociationDesc, SequenceNumber
    const name = r.string();
    r.string();                     // DateCreated
    const modified = r.string();
    return { handle, format, size, parent, name, modified, isFolder: format === FORMAT_FOLDER };
}

// The ObjectInfo dataset of a file or folder to send.
export function objectInfoDataset(storage, name, size, modified, folder = false) {
    const head = new Uint8Array(52);
    const v = dv(head);
    v.setUint32(0, storage, true);
    v.setUint16(4, folder ? FORMAT_FOLDER : FORMAT_UNDEFINED, true);
    v.setUint32(8, size, true);
    v.setUint16(40, folder ? 1 : 0, true);      // AssociationType
    return concat([head, mtpString(name), mtpString(modified), mtpString(modified), mtpString('')]);
}

export function checkName(name) {
    if (!name || name.includes('/') || name === '.' || name === '..' || name.length > 254)
        return `${JSON.stringify(name)} is not a name for the top of the storage`;
    return null;
}

// --- transactions ---

// A transport's rejection, as the failure it is.
function usbFailure(e, what) {
    if (e instanceof MtpFailure)
        return e;
    const text = e && e.message ? e.message : String(e);
    if (e && e.gone)
        return new MtpFailure('lost', `${what}: ${text}`);
    if (e && e.timeout)
        return new MtpFailure('timeout', `${what} timed out`);
    return failed(`${what}: ${text}`);
}

export class Mtp {
    constructor(transport) {
        this.t = transport;
        this.tid = 0;
        this.session = 0;
        this.stale = 0;
        this.onProgress = null;
    }

    async write(bytes) {
        try {
            await this.t.write(bytes, TIMEOUT_MS);
        } catch (e) {
            throw usbFailure(e, 'USB write');
        }
    }

    async read(timeoutMs = TIMEOUT_MS) {
        let got;
        try {
            got = await this.t.read(READ_SIZE, timeoutMs);
        } catch (e) {
            throw usbFailure(e, 'USB read');
        }
        return got;
    }

    // Drops whatever an earlier client left in the device's IN queue (S-16.064).
    async drain() {
        while (await this.read(DRAIN_MS) !== null)
            ;
    }

    // The next container: { kind, code, tid, payload }. Zero-length packets between are skipped.
    async container(expect = 0) {
        let chunk = new Uint8Array(0);
        while (!chunk.length) {
            chunk = await this.read();
            if (chunk === null)
                throw new MtpFailure('timeout', 'USB read timed out');
        }
        if (chunk.length < 12)
            throw failed(`short container from the device: ${hexOf(chunk)}`);
        const v = dv(chunk);
        const total = v.getUint32(0, true), kind = v.getUint16(4, true);
        const code = v.getUint16(6, true), tid = v.getUint32(8, true);
        if (![DATA, RESP, EVENT].includes(kind) || total < 12)
            throw failed(`not a device container: ${hexOf(chunk.subarray(0, 12))}`);
        const parts = [chunk];
        let got = chunk.length;
        // The length is not trusted (S-16.056 states the rule): a short packet ends the phase too.
        while (kind === DATA && got < total && chunk.length % this.t.mps === 0) {
            chunk = await this.read();
            if (chunk === null)
                throw new MtpFailure('timeout', 'USB read timed out');
            if (!chunk.length)
                break;
            parts.push(chunk);
            got += chunk.length;
            if (this.onProgress && expect)
                this.onProgress(Math.min(got - 12, expect), expect);
        }
        return { kind, code, tid, payload: concat(parts).subarray(12) };
    }

    // Runs one operation: { code, params, data (null when there was no data phase) }.
    async transaction(code, params = [], send = null, expect = 0) {
        this.tid = (this.tid + 1) >>> 0;
        const cmd = new Uint8Array(12 + 4 * params.length);
        const v = dv(cmd);
        v.setUint32(0, cmd.length, true);
        v.setUint16(4, CMD, true);
        v.setUint16(6, code, true);
        v.setUint32(8, this.tid, true);
        params.forEach((p, k) => v.setUint32(12 + 4 * k, p >>> 0, true));
        await this.write(cmd);
        if (send !== null)
            await this.sendData(code, send);
        let data = null;
        for (;;) {
            const c = await this.container(expect);
            if (c.kind === EVENT)
                continue;
            if (c.kind === DATA) {
                if (c.tid !== this.tid) {
                    this.stale++;               // S-16.064: an earlier transaction's
                    continue;
                }
                data = c.payload;
                continue;
            }
            if (c.tid !== this.tid)
                throw failed(`response to transaction ${c.tid} during transaction ${this.tid}`);
            const n = Math.min(c.payload.length >> 2, 5);
            const pv = dv(c.payload);
            return { code: c.code, params: Array.from({ length: n }, (_, k) => pv.getUint32(4 * k, true)), data };
        }
    }

    async sendData(code, payload) {
        const head = new Uint8Array(12);
        const v = dv(head);
        v.setUint32(0, 12 + payload.length, true);
        v.setUint16(4, DATA, true);
        v.setUint16(6, code, true);
        v.setUint32(8, this.tid, true);
        const blob = concat([head, payload]);
        const step = WRITE_SIZE - WRITE_SIZE % this.t.mps;
        for (let off = 0; off < blob.length; off += step) {
            await this.write(blob.subarray(off, off + step));
            if (this.onProgress && code === OP_SEND)
                this.onProgress(Math.min(off + step, blob.length), blob.length);
        }
        if (blob.length % this.t.mps === 0)
            await this.write(new Uint8Array(0));          // S-10b.003
    }

    async checked(name, code, params = [], send = null, expect = 0) {
        const r = await this.transaction(code, params, send, expect);
        if (r.code !== OK)
            throw new MtpFailure('refused', `${name} answered ${responseName(r.code)}`,
                { op: name, response: responseName(r.code) });
        return r;
    }

    // An operation with a data phase from the device. OK without one is not an empty answer.
    async readOp(name, code, params = [], expect = 0) {
        const { data } = await this.checked(name, code, params, null, expect);
        if (data === null)
            throw failed(`${name} answered OK with no data phase`);
        return data;
    }

    async openSession() {
        this.session++;
        const { code } = await this.transaction(OP_OPEN, [this.session]);
        if (code !== OK && code !== ALREADY_OPEN)       // S-16.176
            throw new MtpFailure('refused', `OpenSession answered ${responseName(code)}`,
                { op: 'OpenSession', response: responseName(code) });
    }

    async closeSession() {
        return (await this.transaction(OP_CLOSE)).code === OK;
    }

    async deviceInfo() {
        const r = new Reader(await this.readOp('GetDeviceInfo', OP_DEVICE_INFO));
        r.pos += 2 + 4 + 2;
        r.string();
        r.pos += 2;
        for (let k = 0; k < 5; k++)
            r.skipArray(2);
        return { manufacturer: r.string(), model: r.string(), version: r.string() };
    }

    async storage() {
        const r = new Reader(await this.readOp('GetStorageIDs', OP_STORAGE_IDS));
        if (r.num(4) === 0)
            throw failed('the device presents no storage');
        return r.num(4);
    }

    // { access, total, free }
    async storageInfo(storage) {
        const r = new Reader(await this.readOp('GetStorageInfo', OP_STORAGE_INFO, [storage]));
        r.pos += 4;
        return { access: r.num(2), total: r.num(8), free: r.num(8) };
    }

    async listRoot(storage) {
        const r = new Reader(await this.readOp('GetObjectHandles', OP_HANDLES, [storage, 0, ROOT]));
        const out = [];
        const n = r.num(4);
        for (let k = 0; k < n; k++) {
            const h = r.num(4);
            out.push(parseObject(h, await this.readOp('GetObjectInfo', OP_OBJECT_INFO, [h])));
        }
        return out;
    }

    async getObject(handle, expect = 0) {
        return this.readOp('GetObject', OP_GET_OBJECT, [handle], expect);
    }

    async sendInfo(storage, info, parent = ROOT) {
        // MTP 1.1's root; a responder that reads it as a handle gets 0, S-10b.080's other form.
        let { code } = await this.transaction(OP_SEND_INFO, [storage, parent], info);
        if (parent === ROOT && (code === INVALID_HANDLE || code === INVALID_PARENT))
            ({ code } = await this.transaction(OP_SEND_INFO, [storage, 0], info));
        if (code !== OK)
            throw new MtpFailure('refused', `SendObjectInfo answered ${responseName(code)}`,
                { op: 'SendObjectInfo', response: responseName(code) });
    }

    async sendFile(storage, name, content, modified) {
        await this.sendInfo(storage, objectInfoDataset(storage, name, content.length, modified));
        await this.checked('SendObject', OP_SEND, [], content);
    }
}

// --- the commands ---

async function sha256(bytes) {
    return hexOf(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)));
}

function listing(objects, space) {
    const lines = objects.slice().sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0))
        .map((o) => `${String(o.isFolder ? '<dir>' : o.size).padStart(12)}  ${o.modified.padEnd(15)}  ` +
            o.name + (o.isFolder ? '/' : ''));
    lines.push(`${objects.length} objects, ${space.free} bytes free of ${space.total}`);
    return lines;
}

const named = (objects, name) => objects.filter((o) => o.name === name);
const nowStamp = () => new Date().toISOString().replace(/[-:]/g, '').slice(0, 15);

function stage(hooks, name) {
    if (hooks.onStage)
        hooks.onStage(name, STAGES[name]);
}

// Raises unless the one object of that name on the unit holds exactly content.
async function compare(mtp, storage, name, content, say, hooks) {
    stage(hooks, 'readback');
    const found = named(await mtp.listRoot(storage), name);
    if (!found.length)
        throw failed(`${name} is not on the unit`);
    if (found.length > 1)
        throw failed(`${name} is listed ${found.length} times`);
    const obj = found[0];
    if (obj.isFolder)
        throw failed(`${name} is a folder`);
    const back = await mtp.getObject(obj.handle, obj.size);
    const want = await sha256(content);
    const have = await sha256(back);
    let first = -1;
    const n = Math.min(content.length, back.length);
    for (let k = 0; k < n; k++)
        if (content[k] !== back[k]) {
            first = k;
            break;
        }
    if (first < 0 && back.length !== content.length)
        first = n;
    if (first >= 0 || obj.size !== content.length) {
        const e = new MtpFailure('mismatch', `MISMATCH on ${name}: local ${content.length} bytes, listed ` +
            `${obj.size}, read back ${back.length}, first difference at offset ${first < 0 ? n : first}`);
        e.detail = [`local   sha256 ${want}`, `on unit sha256 ${have}`];
        throw e;
    }
    say(`verified ${name}: ${back.length} bytes read back, identical, sha256 ${have}`);
    return have;
}

async function begin(mtp, say, hooks) {
    stage(hooks, 'drain');
    await mtp.drain();
    stage(hooks, 'device');
    const d = await mtp.deviceInfo();
    say(`${d.manufacturer} ${d.model}, version ${d.version}`);
    stage(hooks, 'session');
    await mtp.openSession();
    return { device: d, storage: await mtp.storage() };
}

function sayer(lines, hooks) {
    return (text) => {
        lines.push(text);
        if (hooks.onLine)
            hooks.onLine(text);
    };
}

// The outcome of a failure, with what to do now.
function outcomeOf(e, name, changed, lines, stale) {
    const f = e instanceof MtpFailure ? e : usbFailure(e, 'USB');
    lines.push(f.message, ...(f.detail || []));
    const kind = f.kind;
    const text = kind === 'mismatch' ? WORDS.mismatch(name) : kind === 'readonly' ? WORDS.readonly :
        kind === 'refused' ? WORDS.refused(f.op, f.response) : kind === 'timeout' ? WORDS.timeout :
            kind === 'lost' ? WORDS.lost : WORDS.failed(f.message);
    const advice = [];
    if (changed)
        advice.push(WORDS.notGood(name));
    else if (kind !== 'readonly')
        advice.push(WORDS.unchanged);
    if (kind === 'timeout')
        advice.push(WORDS.timeoutAdvice);
    else if (kind === 'lost')
        advice.push(WORDS.lostAdvice);
    else if (kind !== 'readonly' && changed)
        advice.push(WORDS.again);
    return { outcome: kind, text, response: f.response, changed, sha256: null, stale, lines, advice };
}

export async function list(transport, { hooks = {} } = {}) {
    const mtp = new Mtp(transport);
    const lines = [];
    const say = sayer(lines, hooks);
    const { device, storage } = await begin(mtp, say, hooks);
    stage(hooks, 'list');
    const objects = await mtp.listRoot(storage);
    const space = await mtp.storageInfo(storage);
    listing(objects, space).forEach(say);
    await mtp.closeSession();
    return { device, objects, ...space, lines };
}

export async function verify(transport, content, { name = INSTALL_NAME, hooks = {} } = {}) {
    const mtp = new Mtp(transport);
    mtp.onProgress = hooks.onProgress || null;
    const lines = [];
    const say = sayer(lines, hooks);
    try {
        const { storage } = await begin(mtp, say, hooks);
        const sha = await compare(mtp, storage, name, content, say, hooks);
        await mtp.closeSession();
        if (mtp.stale)
            say(`note: dropped ${mtp.stale} data block(s) left from earlier transactions`);
        stage(hooks, 'done');
        return { outcome: 'verified', text: WORDS.verified(name, content.length), response: null,
            changed: false, sha256: sha, stale: mtp.stale, lines, advice: [] };
    } catch (e) {
        return outcomeOf(e, name, false, lines, mtp.stale);
    }
}

export async function put(transport, content, { name = INSTALL_NAME, fileName = 'the file',
    modified = nowStamp(), hooks = {} } = {}) {
    const mtp = new Mtp(transport);
    mtp.onProgress = hooks.onProgress || null;
    const lines = [];
    const say = sayer(lines, hooks);
    let changed = false;
    let sha = null;
    let storage = 0;
    try {
        const bad = checkName(name);
        if (bad)
            throw failed(bad);
        if (!content.length)
            throw failed(`${fileName} is empty`);
        if (content.length >= 2 ** 32)
            throw failed(`${fileName} is too large for MTP's 32-bit object size`);

        ({ storage } = await begin(mtp, say, hooks));
        stage(hooks, 'list');
        const objects = await mtp.listRoot(storage);
        const space = await mtp.storageInfo(storage);
        listing(objects, space).forEach(say);
        const old = named(objects, name);
        if (space.total === 0)
            throw failed('the unit reports no storage');                    // S-10b.031
        if (space.access !== 0)                                                // S-10b.021
            throw new MtpFailure('readonly', `the store is read-only (AccessCapability ${space.access}): ` +
                'nothing was changed');
        if (old.some((o) => o.isFolder))
            throw failed(`${name} is a folder, and a folder delete is recursive: nothing was changed`);
        if (content.length > space.free + old.reduce((n, o) => n + o.size, 0))
            throw failed(`${content.length} bytes do not fit in ${space.free} free: nothing was changed`);

        for (const o of old) {
            stage(hooks, 'delete');
            changed = true;
            await mtp.checked('DeleteObject', OP_DELETE, [o.handle, 0]);
            say(`deleted ${name} (${o.size} bytes)`);
        }
        if (old.length && named(await mtp.listRoot(storage), name).length)
            throw failed(`${name} is still listed after its delete`);

        changed = true;
        stage(hooks, 'send');
        say(`uploading ${fileName} as ${name}, ${content.length} bytes`);
        await mtp.sendFile(storage, name, content, modified);

        // A session of its own, so that the read-back starts from a new listing.
        await mtp.closeSession();
        await mtp.openSession();
        sha = await compare(mtp, storage, name, content, say, hooks);
    } catch (e) {
        return outcomeOf(e, name, changed, lines, mtp.stale);
    }
    try {
        listing(await mtp.listRoot(storage), await mtp.storageInfo(storage)).forEach(say);
        await mtp.closeSession();
    } catch (e) {
        // the copy is verified; what failed after it is said, and the copy stands
        lines.push(e && e.message ? e.message : String(e));
    }
    if (mtp.stale)
        say(`note: dropped ${mtp.stale} data block(s) left from earlier transactions`);
    stage(hooks, 'done');
    return { outcome: 'verified', text: WORDS.verified(name, content.length), response: null, changed: true,
        sha256: sha, stale: mtp.stale, lines, advice: [] };
}
