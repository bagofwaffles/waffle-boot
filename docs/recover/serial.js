// SPDX-License-Identifier: GPL-2.0-or-later
// The Web Serial transport for core.js's Link: write(bytes), read(timeoutMs)
// and close(), over the port the user picks in the browser's own prompt.

// Recovery's USB vendor and product (S-10a.001), which the boot menu's serial port
// shares: the prompt lists both, and the HELLO exchange tells them apart.
export const USB_FILTERS = [{ usbVendorId: 0x2367, usbProductId: 0x0021 }];

export function serialSupported() {
	return typeof navigator !== 'undefined' && 'serial' in navigator;
}

export class SerialTransport {
	constructor(port) {
		this.port = port;
		this.reader = null;
		this.writer = null;
		this.pending = null;	// a read in flight, kept across a timeout
		this.onLost = null;	// called once when the port goes away
	}

	// Asks the user for the port. filters: USB_FILTERS, or [] to list every port.
	static async request(filters) {
		const port = await navigator.serial.requestPort(filters.length ? { filters } : {});
		const t = new SerialTransport(port);
		await t.open();
		return t;
	}

	async open() {
		// CDC ACM ignores the rate; the API wants one
		await this.port.open({ baudRate: 115200, bufferSize: 65536 });
		this.reader = this.port.readable.getReader();
		this.writer = this.port.writable.getWriter();
		this.port.addEventListener('disconnect', () => {
			if (this.onLost)
				this.onLost();
		});
	}

	async write(bytes) {
		await this.writer.write(bytes);
	}

	// The next chunk, or null when none came within timeoutMs. A read that
	// timed out stays pending and is the first to be waited on again.
	async read(timeoutMs) {
		if (!this.pending)
			this.pending = this.reader.read();
		let timer;
		const timeout = new Promise((resolve) => {
			timer = setTimeout(() => resolve(null), timeoutMs);
		});
		const got = await Promise.race([this.pending, timeout]);
		clearTimeout(timer);
		if (got === null)
			return null;
		this.pending = null;
		if (got.done)
			throw new Error('the port closed');
		return got.value;
	}

	async close() {
		try {
			if (this.reader) {
				await this.reader.cancel().catch(() => {});
				this.reader.releaseLock();
			}
			if (this.writer) {
				await this.writer.close().catch(() => {});
				this.writer.releaseLock();
			}
			await this.port.close();
		} catch (e) {
			// already gone
		}
	}
}
