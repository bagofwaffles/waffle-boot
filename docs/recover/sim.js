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

// The boot menu's developer link simulator (?sim=devlink:CASE), and a file it takes.
export const simDevlink = () => simulated() && (params.get('sim') || '').startsWith('devlink:');

export async function simInstallerFile() {
	const r = await fetch('/sim/installer-file');
	return new Uint8Array(await r.arrayBuffer());
}

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
