// SPDX-License-Identifier: GPL-2.0-or-later
// A simulated unit in place of Web Serial, for testing the page with no unit:
// tests/web/sim_server.py serves the page from localhost and answers a
// WebSocket as a unit answers its recovery port. Used only when the page is
// opened from localhost with ?sim in its address.

const params = new URLSearchParams(location.search);

export function simulated() {
	return params.has('sim') && ['localhost', '127.0.0.1'].includes(location.hostname);
}

// Which unit: ?sim=KIND:CASE, as sim_server.py lists them.
const which = () => encodeURIComponent(params.get('sim') || '');

// The installer simulator (?sim=installer:CASE): what a person does on the unit.
export const simInstaller = () => simulated() && (params.get('sim') || '').startsWith('installer:');

export async function simAct(what) {
	const r = await fetch('/sim/act?unit=' + which() + '&do=' + encodeURIComponent(what));
	return r.ok;
}

export async function simImage() {
	const r = await fetch('/sim/image?unit=' + which());
	return new Uint8Array(await r.arrayBuffer());
}

export class SimTransport {
	constructor(ws) {
		this.ws = ws;
		this.chunks = [];
		this.wake = null;
		this.gone = false;
		this.onLost = null;
		ws.binaryType = 'arraybuffer';
		ws.onmessage = (e) => {
			this.chunks.push(new Uint8Array(e.data));
			if (this.wake)
				this.wake();
		};
		ws.onclose = () => {
			this.gone = true;
			if (this.wake)
				this.wake();
			if (this.onLost)
				this.onLost();
		};
	}

	static open() {
		return new Promise((resolve, reject) => {
			const ws = new WebSocket('ws://' + location.host + '/sim/unit?unit=' + which());
			ws.onopen = () => resolve(new SimTransport(ws));
			ws.onerror = () => reject(new Error('the simulator did not answer'));
		});
	}

	async write(bytes) {
		if (this.gone)
			throw new Error('the port closed');
		this.ws.send(bytes);
	}

	async read(timeoutMs) {
		if (!this.chunks.length && !this.gone)
			await new Promise((resolve) => {
				const timer = setTimeout(resolve, timeoutMs);
				this.wake = () => {
					clearTimeout(timer);
					resolve();
				};
			});
		this.wake = null;
		if (this.chunks.length)
			return this.chunks.shift();
		if (this.gone)
			throw new Error('the port closed');
		return null;
	}

	async close() {
		this.onLost = null;
		this.ws.close();
	}
}

// The simulated MTP unit (?sim=mtp:CASE), in place of WebUSB: sim_server.py's responder answers
// each bulk transfer over a WebSocket. mtp:busy stands for an interface another program holds.
export const simMtp = () => simulated() && (params.get('sim') || '').startsWith('mtp:');

export async function simOpenMtp(openFailure) {
	const device = { name: 'teenage engineering OP-XY (simulated)', vendorId: 0x2367, productId: 0x0021,
		number: 0, classes: ['ff/01/01'] };
	if (params.get('sim') === 'mtp:busy')
		return openFailure('claim', { name: 'NetworkError', message: 'Unable to claim interface.' }, device);
	const ws = await new Promise((resolve, reject) => {
		const w = new WebSocket('ws://' + location.host + '/sim/mtp?unit=' + which());
		w.binaryType = 'arraybuffer';
		w.onopen = () => resolve(w);
		w.onerror = () => reject(new Error('the simulator did not answer'));
	});
	return { ok: true, transport: new SimMtpTransport(ws), device };
}

// Each request ('W' and the bytes, or 'R' and a u32 size) has one answer: 'K', 'D' and the bytes,
// or 'T' when nothing is queued.
class SimMtpTransport {
	constructor(ws) {
		this.ws = ws;
		this.mps = 512;
		this.waiting = null;
		this.gone = false;
		ws.onmessage = (e) => {
			const w = this.waiting;
			this.waiting = null;
			if (w)
				w.resolve(new Uint8Array(e.data));
		};
		ws.onclose = () => {
			this.gone = true;
			const w = this.waiting;
			this.waiting = null;
			if (w)
				w.reject(Object.assign(new Error('NetworkError: The device was disconnected.'), { gone: true }));
		};
	}

	ask(bytes) {
		if (this.gone)
			return Promise.reject(Object.assign(new Error('NetworkError: The device was disconnected.'),
				{ gone: true }));
		return new Promise((resolve, reject) => {
			this.waiting = { resolve, reject };
			this.ws.send(bytes);
		});
	}

	async write(bytes) {
		const m = new Uint8Array(1 + bytes.length);
		m[0] = 0x57;
		m.set(bytes, 1);
		await this.ask(m);
	}

	async read(size) {
		const m = new Uint8Array(5);
		m[0] = 0x52;
		new DataView(m.buffer).setUint32(1, size, true);
		const r = await this.ask(m);
		return r[0] === 0x44 ? r.subarray(1) : null;
	}

	async close() {
		this.ws.onclose = null;
		this.ws.close();
	}
}
