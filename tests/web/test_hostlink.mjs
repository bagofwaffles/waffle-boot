// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tests of docs/web/hostlink.js, the host link's client:
//
//     node --test tests/web/test_hostlink.mjs
//
// Its framing and the MIDI carrier against bytes tools/testos_mon.py makes, then each call against the
// host link's own C (src/core/host/hostlink.c, over volumes in memory) through hl_bridge.py. Without python3 or
// a C compiler the calls are skipped.

import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import * as hl from '../../docs/web/hostlink.js';

const here = (name) => fileURLToPath(new URL(name, import.meta.url));
const hexOf = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');

test('frames as the Python tool makes them', () => {
	// testos_mon.frame(bytes([0, 1, 1]), 1, ident=0x7C)
	assert.equal(hexOf(hl.frame(Uint8Array.from([0, 1, 1]), 1)), '0521017c03040101d100');
	const big = hl.frame(new Uint8Array(249), 7);
	assert.equal(big.length, 256);
	assert.equal(hexOf(big.subarray(0, 20)), '0521077cf9010101010101010101010101010101');
	const back = hl.unframe(big.subarray(0, big.length - 1));
	assert.equal(back.id, 0x7c);
	assert.equal(back.payload.length, 249);
	// a byte wrong: no frame
	const bad = big.slice(0, big.length - 1);
	bad[10] ^= 1;
	assert.equal(hl.unframe(bad), null);
	assert.equal(hl.crc8(Uint8Array.from([0x21, 1, 0x7c, 3, 0, 1, 1])), 0xd1);
});

test('the MIDI carrier packs as S-10c.001 says', () => {
	assert.equal(hexOf(hl.pack7(Uint8Array.from([0x80, 1, 2, 3, 4, 5, 6, 0xff, 9]))), '0100010203040506017f09');
	const s = Uint8Array.from({ length: 300 }, (_, i) => (i * 37) & 0xff);
	const msgs = hl.carrierMessages(s, 0x02);
	assert.equal(msgs.length, 2);
	assert.deepEqual(Array.from(msgs[0].subarray(0, 6)), [0xf0, 0, 0x20, 0x76, 0x02, 0x66]);
	const back = [...hl.carrierStream(msgs[0], 0x02), ...hl.carrierStream(msgs[1], 0x02)];
	assert.deepEqual(back, Array.from(s));
	assert.equal(hl.carrierStream(msgs[0], 0x21), null);	// another device's
});

// A transport over hl_bridge.py's standard input and output.
function bridge(args = []) {
	const p = spawn('python3', ['-I', '-B', here('hl_bridge.py'), ...args], { stdio: ['pipe', 'pipe', 'inherit'] });
	const q = [];
	let waiter = null;
	p.stdout.on('data', (b) => {
		q.push(new Uint8Array(b));
		if (waiter) {
			waiter();
			waiter = null;
		}
	});
	return {
		proc: p,
		write: async (b) => { p.stdin.write(b); },
		read: async (ms) => {
			if (!q.length)
				await new Promise((r) => { const t = setTimeout(r, ms); waiter = () => { clearTimeout(t); r(); }; });
			return q.length ? q.shift() : null;
		},
		close: () => p.kill(),
	};
}

const haveC = spawnSync('python3', ['-I', '-B', '-c',
	`import sys; sys.path.insert(0, ${JSON.stringify(here('../tools'))}); import hl_sim, tempfile;` +
	' sys.exit(0 if hl_sim.build(tempfile.mkdtemp()) else 1)']).status === 0;

test('hello, approval, files and jobs against the unit\'s own code', { skip: !haveC && 'no python3 or C compiler' }, async () => {
	const t = bridge(['--user', '--seed', '/yaffs2/pkg.bin=package']);
	try {
		const c = new hl.HostLink(t);
		const h = await c.hello();
		assert.equal(h.version, 2);
		assert.equal(h.hostLink, true);
		assert.equal(h.session, 'none');
		assert.equal(h.info.bootloader, '0.9.49+0');
		assert.equal(h.info.device, 'OP-XY');
		assert.equal(h.info.runningSlot, 2);
		assert.ok(c.has(hl.C.PUT_OPEN) && c.has(hl.C.RUN) && !c.has(0x6e));
		assert.deepEqual(h.actions.sort(), [1, 2, 3, 4, 5, 6, 7]);
		assert.deepEqual(h.roots.map((r) => [r.name, r.read, r.write, r.openList]),
			[['yaffs2', true, true, true], ['os', true, false, false], ['user', true, true, false]]);
		// unapproved: list on the drive is open, a change is not
		assert.deepEqual((await c.list('/yaffs2')).map((e) => e.name).sort(), ['lost+found', 'pkg.bin']);
		await assert.rejects(c.mkdir('/yaffs2/d'), (e) => e.status === hl.E.APPROVAL);
		await assert.rejects(c.stat('/os/boot1'), (e) => e.status === hl.E.APPROVAL);
		let shown = false;
		assert.equal(await c.requestApproval({ onPending: () => { shown = true; } }), 'approved');
		assert.equal(shown, true);	// pending first: the page says to hold
		// a put, checked whole; then read back by offset
		const data = Uint8Array.from({ length: 1000 }, (_, i) => (i * 13) & 0xff);
		const seen = [];
		await c.put('/yaffs2/a.bin', data, { onProgress: (n) => seen.push(n) });
		assert.equal(seen[seen.length - 1], 1000);
		assert.deepEqual(await c.get('/yaffs2/a.bin'), data);
		assert.deepEqual(await c.get('/yaffs2/a.bin', { start: 900 }), data.subarray(900));
		const hh = await c.hash('/yaffs2/a.bin');
		assert.equal(hh.size, 1000);
		assert.equal(hexOf(hh.sha256), createHash('sha256').update(data).digest('hex'));
		assert.deepEqual(await c.stat('/yaffs2/a.bin'), { type: 'file', size: 1000, mtime: 0 });
		await c.mkdir('/yaffs2/d');
		await c.rename('/yaffs2/a.bin', '/yaffs2/d/b.bin');
		assert.deepEqual(await c.list('/yaffs2/d'), [{ type: 'file', size: 1000, name: 'b.bin' }]);
		await assert.rejects(c.rename('/yaffs2/d/b.bin', '/os/boot1'), (e) => e.status === hl.E.PATH);
		await c.remove('/yaffs2/d/b.bin');
		// the OS copy: read only
		const os = await c.get('/os/boot1');
		assert.equal(os.length, 518);
		assert.equal(new TextDecoder().decode(os.subarray(512)), 'OS one');
		await assert.rejects(c.put('/os/boot1', data), (e) => e.status === hl.E.READ_ONLY);
		await assert.rejects(c.get('/yaffs2/../os/boot1'), (e) => e.status === hl.E.PATH);
		// the user drive: written, read back, removed
		await c.mkdir('/user/s');
		await c.put('/user/s/a.wav', data);
		assert.deepEqual(await c.get('/user/s/a.wav'), data);
		assert.deepEqual(await c.list('/user/s'), [{ type: 'file', size: 1000, name: 'a.wav' }]);
		await c.rename('/user/s/a.wav', '/user/b.wav');
		await c.remove('/user/b.wav');
		await c.remove('/user/s');
		assert.deepEqual(await c.list('/user'), []);
		// a job with its progress
		const progress = [];
		const rec = await c.runAction(hl.A.INSTALL, { path: '/yaffs2/pkg.bin', parts: 0x03,
			onProgress: (r) => progress.push(r.state) });
		assert.equal(rec.state, 'done');
		assert.equal(rec.text, '[done]');
		assert.deepEqual(progress, ['running', 'done']);
		await assert.rejects(c.call(hl.C.RUN, [0x10]), (e) => e.status === hl.E.NEVER);
		// S-10c.135, S-10c.136: an image into RAM, then its start; no file written
		const img = Uint8Array.from({ length: 3000 }, (_, i) => (i === 0 ? 0xad : (i * 7) & 0xff));
		const before = (await c.list('/yaffs2')).map((e) => e.name).sort();
		await c.ramLoad(img);
		assert.deepEqual((await c.list('/yaffs2')).map((e) => e.name).sort(), before);
		const ram = await c.runAction(hl.A.RAM_START);
		assert.equal(ram.action, hl.A.RAM_START);
		await assert.rejects(c.call(hl.C.RUN, [hl.A.RAM_START]), (e) => e.status === hl.E.NOT_FOUND);
		await assert.rejects(c.ramLoad(Uint8Array.from([1, 2, 3])),
			(e) => e.status === hl.E.REFUSED && hl.REFUSALS[e.data[0]] === 'not an LDR stream');
		await c.end();
		assert.equal(await c.session(), 'none');
	} finally {
		t.close();
	}
});

test('a refusal, and a unit with the host link off', { skip: !haveC && 'no python3 or C compiler' }, async () => {
	let t = bridge(['--answer', '3']);
	try {
		const c = new hl.HostLink(t);
		await c.hello();
		assert.equal(await c.requestApproval(), 'refused');
		await assert.rejects(c.mkdir('/yaffs2/d'), (e) => e.status === hl.E.APPROVAL &&
			/hold 1 on the unit/.test(e.message));
	} finally {
		t.close();
	}
	t = bridge(['--off']);
	try {
		const c = new hl.HostLink(t);
		assert.deepEqual(await c.hello(), { version: 2, oldestClient: 1, hostLink: false });
		await assert.rejects(c.list('/yaffs2'), (e) => e.status === hl.E.OFF);
	} finally {
		t.close();
	}
});

test('an older or newer unit fails cleanly', async () => {
	// a HELLO reply of version 3 that serves clients from 3 on
	const reply = Uint8Array.from([0, 1, 0, 0x57, 0x42, 0x48, 0x4c, 3, 3, 1]);
	const wire = hl.frame(reply, 1, 0x7c, 0x12);
	const fake = { sent: 0, write: async () => {}, read: async () => wire };
	const c = new hl.HostLink(fake);
	await assert.rejects(c.hello(), /version 3 \(clients from 3\)/);
});
