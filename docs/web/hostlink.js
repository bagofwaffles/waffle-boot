// SPDX-License-Identifier: GPL-2.0-or-later
// waffle-boot's host link: a client for the boot menu's USB serial port (Web Serial) or its USB MIDI
// port (Web MIDI, SysEx). One file, no dependencies: another site may copy it. The protocol is
// wiki/Developer-Host-Link.md.
//
//     import { connectSerial, HostLink, A } from './hostlink.js';
//     const hl = new HostLink(await connectSerial());
//     const hello = await hl.hello();              // versions, calls, actions, roots
//     if (await hl.requestApproval() !== 'approved') ...   // "hold 1 on your device"
//     await hl.put('/yaffs2/pkg.bin', bytes, { onProgress });
//     const rec = await hl.runAction(A.INSTALL, { path: '/yaffs2/pkg.bin', parts: 0xff, onProgress });
//
// Every call rejects with a HostLinkError carrying the unit's status (S below); E.APPROVAL means the
// session needs the hold on the unit.

export const VERSION = 1;
export const FRAME_ID = 0x7c;
export const C = {
	HELLO: 0x00, SESSION: 0x01, APPROVE: 0x02, END: 0x03, INFO: 0x04, SET: 0x05, JOB: 0x06,
	STAT: 0x10, LIST: 0x11, GET: 0x12, HASH: 0x13, PUT_OPEN: 0x14, PUT_DATA: 0x15, PUT_END: 0x16,
	REMOVE: 0x17, MKDIR: 0x18, RENAME: 0x19, RUN: 0x20, EVENT: 0x7f,
};
export const A = { INSTALL: 1, OS_WRITE: 2, OS_ONCE: 3, RESTART: 4, CONTINUE_OS: 5, RESTART_MODE: 6 };
export const E = {
	OK: 0, OFF: 1, APPROVAL: 2, NEVER: 3, BAD: 4, UNKNOWN: 5, PATH: 6, NOT_FOUND: 7, EXISTS: 8,
	IO: 9, BUSY: 10, CHECKSUM: 11, OFFSET: 12, UNREACHABLE: 13, READ_ONLY: 14, REFUSED: 15,
};
export const STATUS_TEXT = ['ok', 'host link off', 'hold 1 on your device to allow', 'never from a host',
	'malformed', 'not in this build', 'path refused', 'not found', 'exists', 'I/O error', 'busy',
	'checksum mismatch', 'offset', 'volume not answering', 'read only', 'refused by the unit'];
export const SESSIONS = ['none', 'pending', 'approved', 'refused', 'timed out'];
export const JOB_STATES = ['none', 'queued', 'running', 'done', 'failed'];
export const CHUNK = 224;
export const USB_FILTERS = [{ usbVendorId: 0x2367, usbProductId: 0x0021 },
	{ usbVendorId: 0x2367, usbProductId: 0x0102 }];

export class HostLinkError extends Error {
	constructor(call, status, data) {
		super(`call 0x${call.toString(16)}: ${STATUS_TEXT[status] ?? status}`);
		this.call = call;
		this.status = status;
		this.data = data;
	}
}

// ---- S-06 framing: route seq id len payload crc8, COBS, a zero byte

export function crc8(bytes) {
	let crc = 0;
	for (const b of bytes) {
		crc ^= b ^ 0xa5;
		for (let i = 0; i < 8; i++)
			crc = crc & 1 ? (crc >> 1) ^ 0xb2 : crc >> 1;
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
				out.push(0xff, ...block);
				block = [];
			}
		}
	}
	out.push(block.length + 1, ...block);
	return Uint8Array.from(out);
}

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
		if (code !== 0xff && i < enc.length)
			out.push(0);
	}
	return Uint8Array.from(out);
}

export function frame(payload, seq, id = FRAME_ID, route = 0x21) {
	const raw = Uint8Array.from([route, seq & 0xff, id, payload.length, ...payload]);
	const enc = cobsEncode(Uint8Array.from([...raw, crc8(raw)]));
	return Uint8Array.from([...enc, 0]);
}

// {id, payload} of one frame's bytes without its delimiter, or null when it does not check.
export function unframe(wire) {
	const raw = cobsDecode(wire);
	if (!raw || raw.length < 5 || raw[3] !== raw.length - 5 || crc8(raw.subarray(0, raw.length - 1)) !== raw[raw.length - 1])
		return null;
	return { id: raw[2], payload: raw.subarray(4, raw.length - 1) };
}

// ---- the MIDI carrier (S-10c.020): F0 00 20 76 device 66, the stream bytes packed, F7

export function pack7(data) {
	const out = [];
	for (let i = 0; i < data.length; i += 7) {
		const g = data.subarray(i, i + 7);
		let hdr = 0;
		g.forEach((b, k) => { hdr |= (b >> 7) << k; });
		out.push(hdr, ...Array.from(g, (b) => b & 0x7f));
	}
	return Uint8Array.from(out);
}

export function unpack7(data) {
	const out = [];
	for (let i = 0; i < data.length; i += 8) {
		const hdr = data[i] & 0x7f;
		for (let k = 1; k < 8 && i + k < data.length; k++)
			out.push((data[i + k] & 0x7f) | (((hdr >> (k - 1)) & 1) << 7));
	}
	return Uint8Array.from(out);
}

export function carrierMessages(stream, device = 0x21) {
	const msgs = [];
	for (let i = 0; i < stream.length; i += 245)
		msgs.push(Uint8Array.from([0xf0, 0x00, 0x20, 0x76, device, 0x66,
			...pack7(stream.subarray(i, i + 245)), 0xf7]));
	return msgs;
}

// The stream bytes of a carrier message, or null for any other message.
export function carrierStream(msg, device = 0x21) {
	if (msg.length < 7 || msg[0] !== 0xf0 || msg[1] !== 0 || msg[2] !== 0x20 || msg[3] !== 0x76 ||
	    msg[4] !== device || msg[5] !== 0x66 || msg[msg.length - 1] !== 0xf7)
		return null;
	return unpack7(msg.subarray(6, msg.length - 1));
}

// ---- transports: write(bytes), read(timeoutMs) -> bytes or null, close()

class Queue {
	constructor() {
		this.chunks = [];
		this.waiter = null;
	}

	push(b) {
		this.chunks.push(b);
		if (this.waiter) {
			this.waiter();
			this.waiter = null;
		}
	}

	async read(timeoutMs) {
		if (!this.chunks.length) {
			await new Promise((resolve) => {
				const t = setTimeout(() => { this.waiter = null; resolve(); }, timeoutMs);
				this.waiter = () => { clearTimeout(t); resolve(); };
			});
		}
		return this.chunks.length ? this.chunks.shift() : null;
	}
}

export async function connectSerial(filters = USB_FILTERS) {
	const port = await navigator.serial.requestPort(filters.length ? { filters } : {});
	await port.open({ baudRate: 115200, bufferSize: 65536 });	// CDC ignores the rate
	const reader = port.readable.getReader(), writer = port.writable.getWriter();
	const q = new Queue();
	(async () => {
		try {
			for (;;) {
				const { value, done } = await reader.read();
				if (done)
					break;
				q.push(value);
			}
		} catch (e) {
			// the port went away
		}
	})();
	return {
		write: (b) => writer.write(b),
		read: (ms) => q.read(ms),
		close: async () => {
			await reader.cancel().catch(() => {});
			reader.releaseLock();
			writer.releaseLock();
			await port.close().catch(() => {});
		},
	};
}

// device: the carrier's device byte, 0x21 on the OP-XY, 0x02 on the OP-1 Field.
export async function connectMidi(device = 0x21, match = /OP-XY|OP-1/i) {
	const access = await navigator.requestMIDIAccess({ sysex: true });
	const input = [...access.inputs.values()].find((p) => match.test(p.name));
	const output = [...access.outputs.values()].find((p) => match.test(p.name));
	if (!input || !output)
		throw new Error('no MIDI port of the unit');
	const q = new Queue();
	input.onmidimessage = (ev) => {
		const s = carrierStream(ev.data, device);
		if (s)
			q.push(s);
	};
	return {
		write: async (b) => { for (const m of carrierMessages(b, device)) output.send(m); },
		read: (ms) => q.read(ms),
		close: async () => { input.onmidimessage = null; },
	};
}

// ---- the client

const le32 = (b, o) => (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0;
const le16 = (b, o) => b[o] | b[o + 1] << 8;
const u32 = (v) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
const utf8 = (s) => new TextEncoder().encode(s);
const text = (b) => new TextDecoder().decode(b);

export function jobRecord(b) {
	const end = b.indexOf(0, 6);
	return { id: b[0], action: b[1], state: JOB_STATES[b[2]] ?? b[2], step: b[3],
		percent: b[4] === 0xff ? null : b[4], error: b[5], text: text(b.subarray(6, end < 0 ? b.length : end)) };
}

export function infoRecord(b) {
	const f = le16(b, 14);
	const v = (o) => `${le16(b, o)}.${le16(b, o + 2)}.${le16(b, o + 4)}+${le16(b, o + 6)}`;
	return { bootloader: v(0), front: le16(b, 8), device: ['OP-XY', 'OP-1 Field'][b[10]] ?? 'unknown',
		runningSlot: b[11] === 0xff ? null : b[11] + 1, reason: le16(b, 12), os: v(16),
		protectedWrites: !!(f & 1), developerMode: !!(f & 2), color: !!(f & 4), mtpMacos: !!(f & 8),
		startInBootMenu: !!(f & 16), hostLink: !!(f & 32), secondProcessor: !!(f & 64), driveMounted: !!(f & 128) };
}

async function sha256(bytes) {
	return new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
}

export class HostLink {
	constructor(link) {
		this.link = link;
		this.rx = [];
		this.seq = 0;
		this.tag = 0;
		this.onEvent = null;	// every job record the unit sends
		this.events = [];
	}

	async *frames(timeoutMs) {
		const end = Date.now() + timeoutMs;
		for (;;) {
			let z;
			while ((z = this.rx.indexOf(0)) >= 0) {
				const wire = Uint8Array.from(this.rx.slice(0, z));
				this.rx = this.rx.slice(z + 1);
				const f = unframe(wire);
				if (f && f.id === FRAME_ID)
					yield f.payload;
			}
			const left = end - Date.now();
			if (left <= 0)
				return;
			const got = await this.link.read(Math.min(left, 100));
			if (got)
				this.rx.push(...got);
		}
	}

	event(payload) {
		const rec = jobRecord(payload.subarray(3));
		this.events.push(rec);
		if (this.onEvent)
			this.onEvent(rec);
	}

	// [status, data] of one call; a status not in ok rejects with HostLinkError.
	async call(op, args = [], { timeoutMs = 5000, ok = [E.OK] } = {}) {
		this.seq = (this.seq + 1) & 0xff;
		this.tag = (this.tag + 1) & 0xff;
		await this.link.write(frame(Uint8Array.from([op, this.tag, ...args]), this.seq));
		for await (const p of this.frames(timeoutMs)) {
			if (p.length >= 3 && p[0] === C.EVENT)
				this.event(p);
			else if (p.length >= 3 && p[0] === op && p[1] === this.tag) {
				if (!ok.includes(p[2]))
					throw new HostLinkError(op, p[2], p.slice(3));
				return [p[2], p.slice(3)];
			}
		}
		throw new Error('no answer: is the unit in the boot menu with Host link on?');
	}

	async hello() {
		const [, d] = await this.call(C.HELLO, [VERSION]);
		if (le32(d, 0) !== 0x4c484257)
			throw new Error('not a waffle-boot host link');
		const out = { version: d[4], oldestClient: d[5], hostLink: !!d[6] };
		if (d[4] < VERSION || d[5] > VERSION)
			throw new Error(`the unit speaks host link version ${d[4]} (clients from ${d[5]}); this client ${VERSION}`);
		if (!out.hostLink)
			return out;
		out.session = SESSIONS[d[7]];
		out.maxPayload = d[8];
		out.chunk = d[9];
		out.info = infoRecord(d.subarray(10, 34));
		let at = 34;
		out.calls = Array.from(d.subarray(at + 1, at + 1 + d[at]));
		at += 1 + d[at];
		out.actions = Array.from(d.subarray(at + 1, at + 1 + d[at]));
		at += 1 + d[at];
		const n = d[at++];
		out.roots = [];
		for (let i = 0; i < n; i++) {
			const z = d.indexOf(0, at + 2);
			out.roots.push({ id: d[at], read: !!(d[at + 1] & 1), write: !!(d[at + 1] & 2),
				openList: !!(d[at + 1] & 4), name: text(d.subarray(at + 2, z)) });
			at = z + 1;
		}
		this.info = out;
		return out;
	}

	has(call) {
		return !!this.info?.calls?.includes(call);
	}

	async session() {
		return SESSIONS[(await this.call(C.SESSION))[1][0]];
	}

	// Asks the unit, then waits for the hold: 'approved', 'refused', 'timed out' (or 'pending'
	// after waitMs). onPending(): show "hold 1 on your device".
	async requestApproval({ waitMs = 70000, onPending = null } = {}) {
		let state = SESSIONS[(await this.call(C.APPROVE))[1][0]];
		if (state === 'pending' && onPending)
			onPending();
		const end = Date.now() + waitMs;
		while (state === 'pending' && Date.now() < end) {
			await new Promise((r) => setTimeout(r, 250));
			state = await this.session();
		}
		return state;
	}

	async end() {
		await this.call(C.END);
	}

	async getInfo() {
		return infoRecord((await this.call(C.INFO))[1]);
	}

	async stat(path) {
		const [, d] = await this.call(C.STAT, utf8(path));
		return { type: d[0] === 2 ? 'dir' : 'file', size: le32(d, 1), mtime: le32(d, 5) };
	}

	async list(path) {
		const out = [];
		for (let index = 0; index !== 0xffff;) {
			const [, d] = await this.call(C.LIST, [index & 0xff, index >> 8, ...utf8(path)], { timeoutMs: 15000 });
			let at = 2;
			while (at < d.length) {
				const n = d[at + 5];
				out.push({ type: d[at] === 2 ? 'dir' : 'file', size: le32(d, at + 1),
					name: text(d.subarray(at + 6, at + 6 + n)) });
				at += 6 + n;
			}
			index = le16(d, 0);
		}
		return out;
	}

	// The file from offset `start` on, in chunks by offset.
	async get(path, { start = 0, onProgress = null } = {}) {
		const parts = [];
		let off = start;
		for (;;) {
			const [, d] = await this.call(C.GET, [...u32(off), CHUNK, ...utf8(path)], { timeoutMs: 15000 });
			const data = d.slice(4);
			parts.push(data);
			off += data.length;
			if (onProgress)
				onProgress(off);
			if (data.length < CHUNK)
				break;
		}
		const out = new Uint8Array(off - start);
		let at = 0;
		for (const p of parts) {
			out.set(p, at);
			at += p.length;
		}
		return out;
	}

	async hash(path) {
		const [, d] = await this.call(C.HASH, utf8(path), { timeoutMs: 120000 });
		return { size: le32(d, 0), sha256: d.slice(4, 36) };
	}

	// To the path's temporary name, from where the unit stands, then checked whole on the unit.
	async put(path, bytes, { onProgress = null } = {}) {
		const digest = await sha256(bytes);
		let [, d] = await this.call(C.PUT_OPEN, [...u32(bytes.length), ...digest, ...utf8(path)], { timeoutMs: 30000 });
		let have = le32(d, 0);
		while (have < bytes.length) {
			[, d] = await this.call(C.PUT_DATA, [...u32(have), ...bytes.subarray(have, have + CHUNK)],
				{ timeoutMs: 15000, ok: [E.OK, E.OFFSET] });
			have = le32(d, 0);
			if (onProgress)
				onProgress(have);
		}
		await this.call(C.PUT_END, [], { timeoutMs: 120000 });
	}

	async remove(path) {
		await this.call(C.REMOVE, utf8(path), { timeoutMs: 15000 });
	}

	async mkdir(path) {
		await this.call(C.MKDIR, utf8(path), { timeoutMs: 15000 });
	}

	async rename(from, to) {
		await this.call(C.RENAME, [...utf8(from), 0, ...utf8(to)], { timeoutMs: 15000 });
	}

	async setHostLink(on) {
		await this.call(C.SET, [7, on ? 1 : 0]);
	}

	// Runs an action; onProgress(record) for each change. The final record: state 'done' or
	// 'failed' (or 'done' with text 'restarting' before a restart takes the port away).
	async runAction(action, { path = null, parts = 0xff, mode = 0, onProgress = null, timeoutMs = 900000 } = {}) {
		const args = [action];
		if (action === A.INSTALL)
			args.push(parts);
		if (action === A.RESTART_MODE)
			args.push(mode);
		if (path)
			args.push(...utf8(path));
		const [, d] = await this.call(C.RUN, args, { timeoutMs: 30000 });
		let rec = jobRecord(d);
		const mine = (r) => r.id === rec.id;
		const end = Date.now() + timeoutMs;
		const take = () => {
			while (this.events.length) {
				const r = this.events.shift();
				if (mine(r)) {
					rec = r;
					if (onProgress)
						onProgress(r);
				}
			}
		};
		take();
		while ((rec.state === 'queued' || rec.state === 'running') && Date.now() < end) {
			for await (const p of this.frames(1000)) {
				if (p[0] === C.EVENT)
					this.event(p);
			}
			take();
		}
		return rec;
	}
}
