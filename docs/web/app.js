// SPDX-License-Identifier: GPL-2.0-or-later
// The recovery page: the DOM over core.js. Every decision and every sentence
// about a command comes from core.js, which is a port of tools/wbrecover.py;
// this file shows them, asks, and reports.
import {
	Link, connect, getInfo, describe, checkImage, planWrite, planSelect, planAct, planRestart,
	runWrite, runSelect, runAct, runRestart,
} from './core.js';
import {
	detect, installerHello, poll, isBusy, readReport, describeInstall, FERRET_URL, BUSY, GONE_TRIAL, GONE_END,
} from './install.js';
import * as IW from './install_words.js';
import { SerialTransport, USB_FILTERS, serialSupported } from './serial.js';
import { put as mtpPut, INSTALL_NAME } from './mtp.js';
import { openMtp, openFailure, usbSupported } from './usb.js';
import {
	simulated, simInstaller, SimTransport, simImage, simAct, simMtp, simOpenMtp,
} from './sim.js';

const $ = (id) => document.getElementById(id);
const el = (tag, cls, text) => {
	const e = document.createElement(tag);
	if (cls)
		e.className = cls;
	if (text !== undefined)
		e.textContent = text;
	return e;
};

const state = {
	transport: null, link: null, info: null, desc: null,
	image: null,	// the guided tab's checked image
	aimage: null,	// the advanced tab's
	busy: false,	// a command is under way
	written: false,	// the guided write was verified
};

// ---- the log ----

function log(text, cls) {
	const line = el('div');
	const t = new Date();
	line.append(el('span', 't', t.toTimeString().slice(0, 8) + '  '), el('span', cls || '', text));
	$('log').append(line);
	$('log').scrollTop = $('log').scrollHeight;
}

const hex = (b) => Array.from(b.slice(0, 48), (x) => x.toString(16).padStart(2, '0')).join(' ') +
	(b.length > 48 ? ' ... (' + b.length + ' bytes)' : '');

// A transport that also writes what passes to the log, when asked to.
function logged(t) {
	return {
		write: async (b) => {
			if ($('rawlog').checked)
				log('> ' + hex(b), 'tx');
			return t.write(b);
		},
		read: async (ms) => {
			const b = await t.read(ms);
			if (b && $('rawlog').checked)
				log('< ' + hex(b), 'rx');
			return b;
		},
		close: () => t.close(),
	};
}

// ---- steps ----

function step(id, cls, note) {
	const s = $('s-' + id);
	s.classList.remove('pending', 'active', 'done', 'fail');
	s.classList.add(cls);
	if (note !== undefined)
		$('sn-' + id).textContent = note;
}

// core.js speaks as the command-line tool does. Its commands, by this page's names.
const NAMES = [
	[/\s*\(wbrecover\.py restart\)/g, ''],
	[/\bwrite --new-front IMAGE\b/g, 'Try an image with another front'],
	[/\bRun info\b/g, 'Read again'],
	[/wbrecover\.py write --running(?: IMAGE(?:\.ldr)?)?/g, 'Write the running slot'],
	[/wbrecover\.py write --new-front(?: IMAGE(?:\.ldr)?)?/g, 'Try an image with another front'],
	[/wbrecover\.py write(?: IMAGE(?:\.ldr)?)?/g, 'Write the other slot'],
	[/wbrecover\.py select(?: IMAGE(?:\.ldr)?)?/g, 'Select'],
	[/wbrecover\.py put-back/g, 'Put back'],
	[/wbrecover\.py info/g, 'Read again'],
	[/wbrecover\.py (try|switch|keep|restart)/g, (m, c) => c[0].toUpperCase() + c.slice(1)],
	[/wbrecover\.py/g, 'the Advanced tab'],
	[/--force/g, 'Force'],
	[/Power \+ 4/g, 'Power + M4'],
	// where to find M4, as the installer's key screen says it
	[/\bkey 4 \(M4\)/g, 'M4'],
	[/\bkey 4\b/g, 'M4'],
	[/\bM4\b(?! \(4 under)/g, 'M4 (4 under the display)'],
];
const named = (text) => NAMES.reduce((t, [re, to]) => t.replace(re, to), text);
// A refusal for a command's card: without the words every one of them carries.
const brief = (text) => named(text).replace(/^Refused before sending: /, '')
	.replace(/ Nothing was sent\.$/, '');
const cap = (text) => text.charAt(0).toUpperCase() + text.slice(1);

function says(list, lines, bad) {
	list.replaceChildren(...lines.filter(Boolean).map((l) => el('li', '', named(l))));
	list.classList.toggle('bad', !!bad);
}

function chips(box, items) {
	box.replaceChildren(...items.map(([text, cls]) => el('span', 'chip ' + (cls || ''), text)));
}

// ---- the browser check ----

function checkBrowser() {
	const serial = serialSupported() || simulated();
	const secure = window.isSecureContext;
	chips($('support'), [
		[serial ? 'Web Serial' : 'No Web Serial', serial ? 'on' : 'off'],
		[secure ? 'Secure page' : 'Not a secure page', secure ? 'on' : 'off'],
		[usbSupported() || simMtp() ? 'WebUSB' : 'No WebUSB', usbSupported() || simMtp() ? 'on' : 'warn'],
	]);
	if (serial && secure) {
		$('checkhint').textContent = simulated() ?
			'A simulated unit answers this page: nothing here reaches a real OP-XY.' :
			'This browser can talk to the unit.';
		step('check', 'done', 'ready');
		step('menu', 'active');
		$('connect').disabled = false;
		$('i-connect').disabled = false;
		step('iconn', 'active');
	} else {
		$('checkhint').textContent = !serial ?
			'This browser has no Web Serial. Use Chrome or Edge on a computer.' :
			'Open this page over https, or from localhost.';
		step('check', 'fail', 'not supported');
	}
}

// ---- connecting ----

async function doConnect() {
	$('connect').disabled = true;
	try {
		const t = simulated() ? await SimTransport.open() : await SerialTransport.request(USB_FILTERS);
		t.onLost = lost;
		state.transport = t;
		state.link = new Link(logged(t));
		log('Port open. Asking the unit...');
		const found = await detect(state.link);
		if (found.kind === 'installer') {
			const link = state.link;
			state.transport = state.link = null;
			$('connect').disabled = false;
			log('The installer answers, not recovery: showing the Install tab.', 'warn');
			tab('install');
			await installerFound(t, link, found.hello);
			return;
		}
		const { hello, info } = found;
		log('Recovery answers: front version ' + hello.front_version + '.', 'ok');
		$('disconnect').hidden = false;
		$('refresh').hidden = false;
		step('menu', 'done', 'connected');
		show(info);
	} catch (e) {
		await drop();
		if (e && e.name === 'NotFoundError') {
			log('No port was chosen.', 'warn');
		} else {
			log('Could not connect: ' + (e && e.message ? e.message : e), 'err');
			$('connhint').textContent = 'No answer from recovery on that port. Check that the red Recovery screen is ' +
				'showing, then connect again.';
			step('menu', 'fail', 'no answer');
		}
		$('connect').disabled = false;
	}
}

async function drop() {
	const t = state.transport;
	state.transport = state.link = state.info = state.desc = null;
	if (t) {
		t.onLost = null;
		await t.close().catch(() => {});
	}
}

async function disconnected(note) {
	await drop();
	$('disconnect').hidden = true;
	$('refresh').hidden = true;
	$('unit').hidden = true;
	says($('unitnotes'), []);
	$('unithint').hidden = false;
	$('connect').disabled = false;
	step('menu', 'active', '');
	step('unit', 'pending', note || '');
	render();
}

function lost() {
	if (!state.transport)
		return;
	log('The port went away.', 'warn');
	disconnected('disconnected');
}

async function refresh() {
	if (!state.link || state.busy)
		return;
	try {
		show(await getInfo(state.link));
	} catch (e) {
		log('No answer: ' + e.message, 'err');
		disconnected('no answer');
	}
}

// ---- the unit ----

function show(info) {
	state.info = info;
	const d = state.desc = describe(info);
	const rows = d.slots.map((s) => {
		const row = el('div', 'slot');
		row.append(el('span', 'sname', s.name), el('span', 'sver', s.kind || 'no image'),
			   el('span', 'chip ' + (s.good ? 'on' : s.tried ? 'warn' : 'off'), s.state));
		if (s.running)
			row.append(el('span', 'chip warn', 'running'));
		if (s.selected)
			row.append(el('span', 'chip', 'boot slot'));
		if (s.hasDesc && !s.hasFront)
			row.append(el('span', 'chip off', 'no recovery'));
		return row;
	});
	$('slots').replaceChildren(...rows);
	$('u-batt').textContent = d.battery ? d.battery.text : 'not reported';
	$('u-front').textContent = 'front version ' + d.frontVersion + (d.front ? ', image v' + d.front.version : '');
	$('unit').hidden = false;
	$('unithint').hidden = true;
	says($('unitnotes'), d.stateLines, d.damaged || d.held);
	step('unit', d.damaged ? 'fail' : 'done', d.established ?
		'runs ' + d.slots[d.running].name : 'running slot unknown');
	render();
}

// ---- images ----

async function loadImage(bytes, name, which) {
	const adv = which === 'adv';
	const drop = $(adv ? 'adrop' : 'drop'), sub = $(adv ? 'adropsub' : 'dropsub');
	let image;
	try {
		image = await checkImage(bytes);
	} catch (e) {
		log('Could not check ' + name + ': ' + e.message, 'err');
		return;
	}
	image.name = name;
	if (adv)
		state.aimage = image;
	else
		state.image = image;
	drop.classList.toggle('ok', image.ok);
	drop.classList.toggle('bad', !image.ok);
	sub.textContent = name + ', ' + image.length.toLocaleString('en-US') + ' bytes';
	const c = [];
	// the switch names the latest release's version already
	if (image.version && (adv || $('img-own').checked))
		c.push(['v' + image.version, 'on']);
	c.push([image.ok ? 'checked' : 'not usable', image.ok ? 'on' : 'off']);
	if (image.warnings.length)
		c.push(['see the note', 'warn']);
	chips($(adv ? 'aimgchips' : 'imgchips'), c);
	says($(adv ? 'aimgnotes' : 'imgnotes'), image.problems.concat(image.warnings), !image.ok);
	log(name + ': ' + (image.ok ? 'a usable image' : image.problems.join('; ')) +
	    (image.version ? ', v' + image.version : '') + ', SHA-256 ' +
	    Array.from(image.sha256.slice(0, 8), (x) => x.toString(16).padStart(2, '0')).join('') + '...',
	    image.ok ? 'ok' : 'err');
	render();
}

// which: 'guided' or 'advanced' for a bootloader image, or a function that takes the bytes and the name
function wireDrop(dropId, fileId, which) {
	const drop = $(dropId), file = $(fileId);
	const take = async (f) => {
		if (!f)
			return;
		const bytes = new Uint8Array(await f.arrayBuffer());
		if (typeof which === 'function')
			await which(bytes, f.name);
		else
			await loadImage(bytes, f.name, which);
	};
	file.addEventListener('change', () => take(file.files[0]));
	drop.addEventListener('dragover', (e) => {
		e.preventDefault();
		drop.classList.add('hover');
	});
	drop.addEventListener('dragleave', () => drop.classList.remove('hover'));
	drop.addEventListener('drop', (e) => {
		e.preventDefault();
		drop.classList.remove('hover');
		take(e.dataTransfer.files[0]);
	});
}

// ---- confirming ----

// One confirmation of a plan: its lines, and its phrase typed where it has
// one. Resolves true when confirmed.
function confirmOne(title, c) {
	const dlg = $('confirm');
	const phrase = c.phrase && c.phrase !== 'yes' ? c.phrase : null;
	$('c-title').textContent = title;
	dlg.classList.toggle('danger', c.danger === 'no-fallback');
	says($('c-lines'), c.lines, c.danger === 'no-fallback');
	$('c-phrasewrap').hidden = !phrase;
	$('c-phrase').value = '';
	$('c-label').textContent = phrase ? 'Type "' + phrase + '" to go on' : '';
	$('c-go').disabled = !!phrase;
	$('c-go').className = 'btn ' + (c.danger === 'no-fallback' ? 'danger' : 'primary');
	return new Promise((resolve) => {
		const typed = () => {
			$('c-go').disabled = $('c-phrase').value.trim() !== phrase;
		};
		const done = (ok) => {
			$('c-phrase').removeEventListener('input', typed);
			$('c-go').onclick = $('c-cancel').onclick = dlg.oncancel = null;
			dlg.close();
			resolve(ok);
		};
		if (phrase)
			$('c-phrase').addEventListener('input', typed);
		$('c-go').onclick = () => done(true);
		$('c-cancel').onclick = () => done(false);
		dlg.oncancel = (e) => {
			e.preventDefault();
			done(false);
		};
		dlg.showModal();
		(phrase ? $('c-phrase') : $('c-cancel')).focus();
	});
}

async function confirmPlan(title, plan) {
	for (const c of plan.confirmations)
		if (!await confirmOne(title, c))
			return false;
	return true;
}

// ---- running a command ----

const STAGES = {
	hello: 'Asking the unit again...', begin: 'Announcing the image...', confirm: 'Waiting for you...',
	sending: 'Sending the image. Nothing is written yet.', end: 'The unit is checking the image...',
	countdown: 'The unit is counting down.', writing: 'Writing. Keep the unit on and the cable in.',
	done: '',
};

function hooksFor(ui, title) {
	return {
		onStage(name) {
			ui.stage.textContent = STAGES[name] !== undefined ? STAGES[name] : name;
			if (name !== 'countdown')
				ui.count.hidden = true;
			if (name !== 'done')
				log(STAGES[name] || name);
		},
		onProgress(done, total) {
			ui.bar.style.width = (total ? Math.round(100 * done / total) : 0) + '%';
		},
		onCountdown(left) {
			ui.count.hidden = false;
			ui.countn.textContent = left;
		},
		confirm: (c) => confirmOne(title, c),
	};
}

function report(box, r) {
	const good = r.outcome === 'verified' || r.outcome === 'done';
	const word = { verified: 'Verified', done: 'Done', refused: 'Refused', stopped: 'Stopped',
		       failed: 'FAILED', lost: 'No answer' }[r.outcome] || r.outcome;
	box.className = 'result ' + (good ? 'ok' : 'bad');
	const list = el('ul', 'says');
	const text = r.text.startsWith(word + ': ') ? cap(r.text.slice(word.length + 2)) : r.text;
	says(list, [text].concat(r.next || []), !good);
	box.replaceChildren(el('b', '', word), list);
	box.hidden = false;
	log(word + ': ' + named(text), good ? 'ok' : 'err');
	for (const line of r.said || [])
		log('  ' + named(line));
}

// Carries a confirmed plan out and shows what came of it. ui: the run box's
// parts. Returns the result.
async function carryOut(plan, image, ui, title) {
	const run = { write: runWrite, select: runSelect, restart: runRestart }[plan.action] || runAct;
	state.busy = true;
	render();
	ui.box.hidden = false;
	ui.result.hidden = true;
	ui.bar.style.width = '0';
	ui.stage.textContent = '';
	let r;
	try {
		const hooks = hooksFor(ui, title);
		r = plan.action === 'write' || plan.action === 'select' ?
			await run(state.link, plan, image, hooks) : await run(state.link, plan, hooks);
	} catch (e) {
		r = { outcome: 'lost', code: 'link', text: e && e.message ? e.message : String(e), next: [], info: null };
	}
	ui.box.hidden = true;
	state.busy = false;
	report(ui.result, r);
	if (plan.action === 'restart' && r.outcome === 'done')
		await disconnected('restarted');
	else if (r.info)
		show(r.info);
	else if (r.outcome === 'lost')
		await disconnected('no answer');
	else
		await refresh();
	render();
	return r;
}

// A refusal's lines; its one-line text when it has none.
const refusalLines = (r) => (r.lines && r.lines.length ? r.lines : [r.text]).map((l) => cap(brief(l)));

// ---- the guided tab ----

const GUIDED_UI = () => ({ box: $('run'), result: $('writeresult'), bar: $('bar'), stage: $('stage'),
			   count: $('count'), countn: $('countn') });

// The plain write as the guided tab offers it: only the case with nothing
// unusual in it. Anything else is named and sent to the advanced tab.
function guidedPlan() {
	if (!state.info || !state.image)
		return null;
	return planWrite(state.info, state.image, { mode: 'plain' });
}

function renderGuided() {
	const plan = guidedPlan();
	const simple = plan && plan.allowed && plan.danger !== 'no-fallback' && plan.confirmations.length === 1 &&
		(plan.phrase === 'yes' || plan.phrase === null);
	step('image', !state.info ? 'pending' : state.image && state.image.ok ? 'done' : 'active',
	     state.image ? (state.image.ok ? 'ready' : 'not usable') : '');
	$('toadvanced').hidden = true;
	$('agreewrap').hidden = true;
	$('write').textContent = state.image && state.image.ok && state.image.version ?
		'Write v' + state.image.version : 'Write the image';
	if (state.written) {
		step('write', 'done', 'verified');
		$('writeplan').textContent = 'Written, and read back by the unit.';
		says($('writenotes'), []);
		$('write').disabled = true;
	} else if (!plan) {
		step('write', 'pending', '');
		says($('writenotes'), []);
		$('write').disabled = true;
	} else if (simple) {
		const d = state.desc, target = d.slots[plan.target], keep = d.slots[1 - plan.target];
		step('write', 'active', '');
		$('writeplan').textContent = 'v' + state.image.version + ' goes into ' + target.name +
			(target.version ? ', replacing v' + target.version : '') + '. ' + keep.name +
			', which the unit runs from, is not touched, so the unit still starts if this is interrupted.';
		says($('writenotes'), plan.danger === 'caution' ? plan.lines : []);
		$('agreetext').textContent = 'Replace ' + target.name + ' with this image.';
		$('agreewrap').hidden = false;
		$('write').disabled = state.busy || !$('agree').checked;
	} else {
		step('write', 'fail', plan.allowed ? 'needs Advanced' : 'refused');
		$('writeplan').textContent = plan.allowed ?
			'This unit is not in the simple case: writing here needs a choice the Advanced tab asks you to make.' :
			'The unit would refuse this write.';
		says($('writenotes'), plan.refusal ? refusalLines(plan.refusal) : plan.lines, true);
		$('write').disabled = true;
		$('toadvanced').hidden = false;
	}
	const rplan = state.info ? planRestart(state.info, {}) : null;
	step('restart', state.written ? 'active' : 'pending');
	$('restart').disabled = state.busy || !rplan || !rplan.allowed || !state.written;
}

async function guidedWrite() {
	const plan = guidedPlan();
	if (!plan || !plan.allowed)
		return;
	$('agree').checked = false;
	const r = await carryOut(plan, state.image, GUIDED_UI(), 'Write the image');
	if (r.outcome === 'verified') {
		state.written = true;
		render();
	} else {
		step('write', 'fail', r.outcome);
	}
}

async function guidedRestart() {
	const plan = planRestart(state.info, {});
	const ui = { ...GUIDED_UI(), result: $('restartresult') };
	const r = await carryOut(plan, null, ui, 'Restart');
	if (r.outcome === 'done') {
		state.written = false;
		step('restart', 'done', 'restarted');
	}
}

// ---- the advanced tab ----

const ADV_UI = () => ({ box: $('arun'), result: $('aresult'), bar: $('abar'), stage: $('astage'),
			count: $('acount'), countn: $('acountn') });

function advancedPlan(cmd) {
	const info = state.info, image = state.aimage, force = $('force').checked;
	if (!info)
		return null;
	switch (cmd) {
	case 'write': return image ? planWrite(info, image, { mode: 'plain', force }) : null;
	case 'write-running': return image ? planWrite(info, image, { mode: 'running', force }) : null;
	case 'write-new-front': return image ? planWrite(info, image, { mode: 'new-front', force }) : null;
	case 'select': return image ? planSelect(info, image, { force }) : null;
	case 'restart': return planRestart(info, { force });
	default: return planAct(info, cmd, { force });
	}
}

const NEEDS_IMAGE = ['write', 'write-running', 'write-new-front', 'select'];

// The Advanced tab's slot words: Slot 1 or Slot 2 once the unit says which
// it runs from, as its screens name them.
function slotWords() {
	const i = state.info;
	const known = i && i.established && (i.running === 0 || i.running === 1);
	for (const s of document.querySelectorAll('#advanced [data-slot]')) {
		const word = s.dataset.slot === 'running' ? 'the running slot' : 'the other slot';
		const name = known ? IW.slotName(s.dataset.slot === 'running' ? i.running : 1 - i.running) : null;
		s.textContent = name || (s.hasAttribute('data-cap') ? cap(word) : word);
	}
}

function renderAdvanced() {
	slotWords();
	// one write option's card shows: the one chosen above them
	const wopt = document.querySelector('input[name="wopt"]:checked').value;
	for (const card of $('wcmds').children)
		card.hidden = card.dataset.cmd !== wopt;
	for (const card of document.querySelectorAll('.cmd')) {
		const cmd = card.dataset.cmd, plan = advancedPlan(cmd);
		const why = card.querySelector('.why'), btn = card.querySelector('button');
		let text = '';
		if (!state.info)
			text = 'Connect a unit first.';
		else if (!plan && NEEDS_IMAGE.includes(cmd))
			text = 'Choose an image first.';
		else if (plan && !plan.allowed)
			text = cap(brief(plan.refusal.text)) + (plan.forceWouldLift ? ' Force lifts this.' : '');
		why.textContent = text;
		why.hidden = !text;
		btn.disabled = state.busy || !plan || !plan.allowed;
		btn.className = 'btn small' + (plan && plan.allowed && plan.danger === 'no-fallback' ? ' danger' : '');
		card.classList.toggle('blocked', !plan || !plan.allowed);
	}
}

async function advancedRun(cmd, title) {
	const plan = advancedPlan(cmd);
	if (!plan || !plan.allowed || state.busy)
		return;
	for (const line of plan.said)
		log(named(line));
	if (!await confirmPlan(title, plan)) {
		log(title + ': not sent.', 'warn');
		return;
	}
	await carryOut(plan, state.aimage, ADV_UI(), title);
}

// ---- the Install tab: the installer image ----
// The installer only reports over USB. This page polls its state, shows the checks and the
// phase, and says what to do on the unit; it never asks for a write.

const POLL_MS = 500, RETRY_MS = 1500;
const inst = {
	transport: null, link: null, hello: null, tests: {}, desc: null,
	timer: null, queue: Promise.resolve(), waiting: false, quietSince: 0,
};
// each STATE waits this long; a request with no answer while the port stays is the unit busy
const POLL_REQ = { timeout: 2000, retries: 1 };

// One request at a time on the link: polls and the report read take turns.
function serial(fn) {
	const run = inst.queue.then(fn, fn);
	inst.queue = run.catch(() => {});
	return run;
}

async function openTransport(pick) {
	if (simulated())
		return SimTransport.open();
	if (pick)
		return SerialTransport.request(USB_FILTERS);
	// after a restart: a port this page was already allowed, if the browser lists it again
	for (const port of await navigator.serial.getPorts()) {
		const i = port.getInfo();
		if (USB_FILTERS.some((f) => f.usbVendorId === i.usbVendorId && f.usbProductId === i.usbProductId)) {
			const t = new SerialTransport(port);
			try {
				await t.open();
				return t;
			} catch (e) {
				await t.close();
			}
		}
	}
	return null;
}

async function instConnect() {
	$('i-connect').disabled = true;
	stopWaiting();
	let t = null;
	try {
		t = await openTransport(true);
		const link = new Link(logged(t));
		log('Port open. Asking the unit...');
		const found = await detect(link);
		if (found.kind === 'recovery') {
			log('Recovery answers, not the installer.', 'warn');
			$('i-connhint').textContent = 'That is the recovery menu, not the installer. Switch the unit off and ' +
				'on with no key held so that it starts the installer, then connect again. The recovery menu is ' +
				'under Recovery.';
			step('iconn', 'fail', 'recovery menu');
			await t.close().catch(() => {});
			$('i-connect').disabled = false;
			return;
		}
		await installerFound(t, link, found.hello);
	} catch (e) {
		if (t)
			await t.close().catch(() => {});
		if (e && e.name === 'NotFoundError') {
			log('No port was chosen.', 'warn');
		} else {
			log('Could not connect: ' + (e && e.message ? e.message : e), 'err');
			$('i-connhint').textContent = 'No answer from the installer on that port. Check that the unit shows ' +
				'the installer, then connect again.';
			step('iconn', 'fail', 'no answer');
		}
		$('i-connect').disabled = false;
	}
}

async function installerFound(t, link, hello) {
	t.onLost = instLost;
	inst.transport = t;
	inst.link = link;
	if (!inst.hello || inst.hello.crc !== hello.crc)
		inst.tests = {};
	inst.hello = hello;
	inst.waiting = false;
	log('The installer answers: it carries v' + hello.version.slice(0, 3).join('.') + '.', 'ok');
	$('i-connect').disabled = true;
	$('i-disconnect').hidden = false;
	$('i-report').hidden = false;
	$('i-connhint').textContent = 'Connected to the installer. Leave this page open: it follows the unit.';
	step('iconn', 'done', 'connected');
	step('iwrite', 'done', '');
	pollSoon(0);
}

function pollSoon(ms) {
	clearTimeout(inst.timer);
	inst.timer = setTimeout(pollNow, ms);
}

async function pollNow() {
	if (!inst.link)
		return;
	const link = inst.link;
	try {
		const { state: s, changed } = await serial(() => poll(link, inst.hello, inst.tests, POLL_REQ));
		if (link !== inst.link)
			return;
		inst.quietSince = 0;
		$('i-busy').hidden = true;
		for (const k of changed) {
			const t = inst.tests[k];
			log(t.name + ': ' + ['not run', 'pass', 'FAIL', 'not tested', 'not passed'][t.state] +
				(t.detail ? ', ' + t.detail : ''), t.state === 2 ? 'err' : t.state === 1 ? 'ok' : '');
		}
		const before = inst.desc ? inst.desc.phase.name : null;
		inst.desc = describeInstall(s, inst.hello, inst.tests);
		if (inst.desc.phase.name !== before)
			log('Installer: ' + inst.desc.phase.text + '.', inst.desc.tone === 'bad' ? 'err' : '');
		renderInstall();
		pollSoon(POLL_MS);
	} catch (e) {
		if (link !== inst.link)
			return;
		if (isBusy(e)) {
			// the installer does not serve its port while it writes and in some long checks
			if (!inst.quietSince) {
				inst.quietSince = Date.now();
				log('The unit is busy: no answer for now. Waiting.', 'warn');
			}
			const secs = Math.round((Date.now() - inst.quietSince) / 1000);
			$('i-busy').textContent = BUSY + (secs >= 5 ? ' (' + secs + ' s so far)' : '');
			$('i-busy').hidden = false;
			pollSoon(0);
			return;
		}
		instLost();
	}
}

// The port went away: a switch off and on, or the cable. The page waits for the installer.
async function instLost() {
	if (!inst.transport)
		return;
	const t = inst.transport;
	inst.transport = inst.link = null;
	t.onLost = null;
	clearTimeout(inst.timer);
	await t.close().catch(() => {});
	$('i-busy').hidden = true;
	inst.quietSince = 0;
	log('The installer\'s port went away.', 'warn');
	$('i-disconnect').hidden = true;
	$('i-connect').disabled = false;
	const phase = inst.desc ? inst.desc.phase.code : -1;
	const os = inst.desc ? inst.desc.os.code : -1;
	if ((phase === 9 || phase === 12) && os === 6) {
		// the end: switched off, the unit starts its OS and no installer comes back
		$('i-connhint').textContent = GONE_END;
		step('iconn', 'done', 'switched off');
		return;
	}
	startWaiting(phase === 6 ? GONE_TRIAL : null);
}

function startWaiting(words) {
	inst.waiting = true;
	step('iconn', 'active', 'waiting');
	$('i-connhint').textContent = (words || 'The port went away. Switch the unit on if it is off: when the ' +
		'installer is back, this page connects again.') + ' If it does not, press Connect.';
	pollWait();
}

function stopWaiting() {
	inst.waiting = false;
	clearTimeout(inst.timer);
}

async function pollWait() {
	if (!inst.waiting)
		return;
	let t = null;
	try {
		t = await openTransport(false);
		if (t && inst.waiting) {
			const link = new Link(logged(t), { timeout: 800, retries: 2 });
			const h = await installerHello(link, { timeout: 800, retries: 2 });
			if (h && inst.waiting) {
				await installerFound(t, new Link(logged(t)), h);
				return;
			}
		}
	} catch (e) {
		// not back yet
	}
	if (t)
		await t.close().catch(() => {});
	if (inst.waiting)
		inst.timer = setTimeout(pollWait, RETRY_MS);
}

async function instDisconnect() {
	stopWaiting();
	const t = inst.transport;
	inst.transport = inst.link = null;
	if (t) {
		t.onLost = null;
		await t.close().catch(() => {});
	}
	$('i-disconnect').hidden = true;
	$('i-connect').disabled = false;
	$('i-connhint').textContent = 'Disconnected. The installer goes on by itself on the unit.';
	step('iconn', 'active', '');
}

async function saveReport() {
	if (!inst.link)
		return;
	$('i-report').disabled = true;
	$('i-reporthint').textContent = 'Reading check.txt...';
	try {
		const text = await serial(() => readReport(inst.link, {
			onProgress: (n, total) => {
				$('i-reporthint').textContent = 'Reading check.txt: ' + n + ' of ' + total + ' bytes';
			},
		}));
		const a = document.createElement('a');
		a.href = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
		a.download = 'check.txt';
		document.body.append(a);
		a.click();
		a.remove();
		setTimeout(() => URL.revokeObjectURL(a.href), 10000);
		$('i-reporthint').textContent = 'check.txt saved, ' + text.length.toLocaleString('en-US') + ' bytes.';
		log('check.txt read: ' + text.length + ' bytes.', 'ok');
	} catch (e) {
		$('i-reporthint').textContent = 'Could not read check.txt: ' + (e && e.message ? e.message : e);
	}
	$('i-report').disabled = false;
}

const STATE_CHIP = ['', 'on', 'off', 'warn', 'warn'];
const TONE_STEP = { ok: 'done', bad: 'fail', act: 'active', wait: 'active' };

function renderInstall() {
	const d = inst.desc;
	if (!d)
		return;
	// the checks
	const rows = d.checks.map((c) => {
		const row = el('div', 'crow' + (c.current ? ' now' : '') + (c.state === 2 ? ' bad' : ''));
		row.append(el('span', 'cname', c.name),
			   el('span', 'chip ' + (c.current && !c.state ? 'warn' : STATE_CHIP[c.state] || ''),
			      c.current && !c.state ? 'running' : c.stateText),
			   el('span', 'cdetail', c.detail));
		if (c.note && c.state === 2)
			row.append(el('span', 'cnote', c.note));
		return row;
	});
	$('i-checks').replaceChildren(...rows);
	$('i-checks').hidden = false;
	const failed = d.checks.filter((c) => c.state === 2).length;
	const ran = d.checks.filter((c) => c.state).length;
	$('i-checkhint').textContent = d.verdict.code === 0xFF ?
		'Checks run so far: ' + ran + ' of ' + d.checks.length + '.' :
		'Verdict: ' + d.verdict.text + (failed ? ', ' + failed + (failed > 1 ? ' checks' : ' check') +
			' failed.' : '.');
	step('ichecks', d.verdict.code === 0xFF ? 'active' : d.lock ? 'fail' : 'done',
	     d.verdict.code === 0xFF ? ran + ' of ' + d.checks.length : d.verdict.text);
	// the image
	const im = d.image;
	if (im) {
		$('i-ver').textContent = im.version;
		$('i-len').textContent = im.length.toLocaleString('en-US') + ' bytes';
		$('i-crc').textContent = im.crc;
		$('i-pin').textContent = im.pinShort;
		$('i-pin').title = im.pin;
		$('i-target').textContent = d.target || 'not chosen yet';
		$('i-verified').textContent = im.verified ? 'yes: its CRC-32 and front pin match' : 'NO';
		$('i-image').hidden = false;
		$('i-imagehint').textContent = 'Front pin ' + im.pin + '.';
		says($('i-imagenotes'), [im.breadcrumb ? 'This installer is a breadcrumb build: it writes the trial, ' +
			'and refuses the commit.' : '', im.verified ? '' : 'The installer could not verify the image it ' +
			'carries: it will not write it.'], !im.verified);
		step('iimage', im.verified ? 'done' : 'fail', im.version);
	}
	// the phase
	const box = $('i-phasebox');
	box.className = 'phase ' + d.tone;
	box.hidden = false;
	$('i-ptitle').textContent = d.phase.text + (d.failure && d.phase.code === 10 ? ': ' + d.failure.text : '');
	says($('i-lines'), d.lines.filter((l) => !l.startsWith('Holding ')), d.tone === 'bad');
	says($('i-advice'), d.advice);
	$('i-holdbox').hidden = !d.hold;
	if (d.hold) {
		$('i-holdtext').textContent = 'Holding ' + d.hold.key + ' on the unit: keep holding until the bar is ' +
			'full (' + Math.round(d.hold.progress * 100) + ' %).';
		$('i-hold').style.width = Math.round(d.hold.progress * 100) + '%';
	}
	$('i-phasehint').hidden = true;
	step('iphase', TONE_STEP[d.tone], d.phase.name);
	// the slots and the way so far
	const slotRows = d.slots.map((sl) => {
		const row = el('div', 'slot');
		row.append(el('span', 'sname', sl.name));
		for (const w of sl.words.length ? sl.words : ['empty or not good'])
			row.append(el('span', 'chip ' + (w === 'good' || w.startsWith('holds this image, kept') ||
				w === 'trial reached the boot menu' ? 'on' : w === 'trial spent' &&
				!(sl.bits & 0x80) ? 'off' : ''), w));
		return row;
	});
	const starts = el('div', 'unitrow');
	starts.append(el('span', '', 'Starts'), el('b', '', d.starts));
	const os = el('div', 'unitrow');
	os.append(el('span', '', 'The OS'), el('b', '', d.os.text));
	$('i-slots').replaceChildren(...slotRows, starts, os);
	$('i-slots').hidden = d.phase.code < 3;
	const order = ['ready', 'trial written', 'commit ready', 'done'];
	const reached = { 4: 1, 5: 1, 6: 2, 7: 3, 8: 3, 9: 4, 11: 2, 10: 1 }[d.phase.code] || 0;
	for (const li of $('i-trail').children) {
		const at = order.indexOf(li.dataset.p);
		li.className = at < reached ? 'on' : '';
		if (d.phase.code === 11 && li.dataset.p === 'commit ready')
			li.className = 'bad';
	}
	$('i-trail').hidden = reached === 0 && d.phase.code !== 3;
	if (d.phase.code === 3)
		$('i-trail').firstElementChild.className = 'bad';
}

// ---- the latest release ----
// release/ beside this script holds the latest release's files; latest.json names them, the
// version, and each file's size and SHA-256.
const at = (name) => new URL('release/' + name, import.meta.url);
let latest = null;
const releaseMeta = () => latest || (latest = fetch(at('latest.json'), { cache: 'no-cache' }).then((r) => r.json()));

// the Install tab's download link; without latest.json it stays the releases page
async function releaseLink() {
	const meta = await releaseMeta();
	$('i-getfile').href = at(meta.file);
	$('i-getfile').download = meta.file;
	$('i-getver').textContent = ' (' + meta.version + ')';
}

// The Guided tab's image: the latest release's bootloader, held against latest.json, or the
// person's own file when the box is ticked.
const release = { version: null, bytes: null, name: '' };

async function releaseImage() {
	const meta = await releaseMeta();
	const r = await fetch(at(meta.boot.file), { cache: 'no-cache' });
	if (!r.ok)
		throw new Error(String(r.status));
	const bytes = new Uint8Array(await r.arrayBuffer());
	const sha = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)),
		(x) => x.toString(16).padStart(2, '0')).join('');
	if (bytes.length !== meta.boot.size || sha !== meta.boot.sha256)
		throw new Error('size or SHA-256');
	Object.assign(release, { version: meta.version, bytes, name: meta.boot.file });
}

async function imageSource() {
	const own = $('img-own').checked;
	$('drop').hidden = !own;
	if (!own)
		return loadImage(release.bytes, release.name, 'guided');
	state.image = null;
	$('drop').classList.remove('ok', 'bad');
	$('dropsub').textContent = '.ldr, at most 487 KB';
	$('file').value = '';
	chips($('imgchips'), []);
	$('imgnotes').replaceChildren();
	render();
}

function startRelease() {
	$('img-latest').addEventListener('change', imageSource);
	$('img-own').addEventListener('change', imageSource);
	// a simulated unit takes its own sample image, not a release
	const got = simulated() ? Promise.reject(new Error('simulated')) : releaseImage();
	got.then(() => {
		$('relver').textContent = 'waffle-boot ' + release.version;
	}, () => {
		// no release here: the person's own file is the only choice
		$('relver').textContent = 'not available here';
		$('img-latest').disabled = true;
		$('img-own').checked = true;
	}).then(imageSource);
}

// ---- the Install tab's send: the installer file to the unit's MTP mode ----
// The hosted latest release's installer, held against latest.json's size and SHA-256.
let installerFile = null;
async function installerBytes() {
	if (installerFile)
		return installerFile;
	const meta = await releaseMeta();
	const r = await fetch(at(meta.file), { cache: 'no-cache' });
	if (!r.ok)
		throw new Error(meta.file + ': HTTP ' + r.status);
	const bytes = new Uint8Array(await r.arrayBuffer());
	const sha = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)),
		(x) => x.toString(16).padStart(2, '0')).join('');
	if (bytes.length !== meta.size || sha !== meta.sha256)
		throw new Error(meta.file + ' is ' + bytes.length + ' bytes, SHA-256 ' + sha + '; latest.json says ' +
			meta.size + ' bytes, SHA-256 ' + meta.sha256);
	installerFile = { bytes, name: meta.file, version: meta.version, sha };
	return installerFile;
}

// The person's own installer file, in place of the hosted one. Nothing here vouches for it.
let ownInstaller = null;
async function takeOwnInstaller(bytes, name) {
	const drop = $('i-drop');
	drop.classList.remove('ok', 'bad');
	if (!bytes.length) {
		ownInstaller = null;
		drop.classList.add('bad');
		$('i-dropsub').textContent = name + ' is empty';
	} else {
		const sha = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)),
			(x) => x.toString(16).padStart(2, '0')).join('');
		ownInstaller = { bytes, name, version: '', sha };
		drop.classList.add('ok');
		$('i-dropsub').textContent = name + ', ' + bytes.length + ' bytes';
		log(name + ': ' + bytes.length + ' bytes, SHA-256 ' + sha + '.');
	}
	installFile();
}

const canSend = () => usbSupported() || simMtp();

let sending = false;
function installFile() {
	const own = $('ifile-own').checked;
	$('i-drop').hidden = !own;
	$('i-send').textContent = own ? 'Send ' + (ownInstaller ? ownInstaller.name : 'your file') :
		'Send the installer' + (latestVersion ? ' (' + latestVersion + ')' : '');
	$('i-send').disabled = sending || !canSend() || (own && !ownInstaller);
	$('i-sendhint').textContent = simMtp() ?
		'A simulated unit answers this page: nothing here reaches a real OP-XY.' :
		own ? 'Your file is sent as it is, then read back from the unit and compared.' :
		'The file is checked here, sent, then read back from the unit and compared.';
}
let latestVersion = '';

function installSource() {
	const send = $('isrc-send').checked;
	$('i-sendbox').hidden = !send;
	$('i-getbox').hidden = send;
}

// The send's result box. tone: 'ok' or 'bad'.
function sendResult(tone, title, lines, advice) {
	$('i-sendresult').className = 'phase ' + tone;
	$('i-sendtitle').textContent = title;
	says($('i-sendlines'), lines, tone === 'bad');
	says($('i-sendadvice'), advice);
	$('i-sendresult').hidden = false;
	log(title + ': ' + lines.join(' '), tone === 'ok' ? 'ok' : 'err');
	for (const a of advice)
		log('  ' + a);
}

const SEND_NEXT = [
	'On the unit, press these keys in order: 2, 5, 2, 6.',
	'2 leaves MTP mode.',
	'5 writes ' + INSTALL_NAME + '.',
	'2 goes back.',
	'6 restarts: the installer starts.',
	'Then go on to step 2.',
];

async function sendInstaller() {
	sending = true;
	installFile();
	$('i-sendresult').hidden = true;
	$('i-sendrun').hidden = false;
	$('i-sendbar').style.width = '0';
	const stage = (text) => {
		$('i-sendstage').textContent = text;
		if (text)
			log(text);
	};
	let opened = null;
	try {
		stage('Loading the installer file...');
		let file;
		const own = $('ifile-own').checked;
		try {
			file = own ? ownInstaller : await installerBytes();
		} catch (e) {
			log('Installer file: ' + (e && e.message ? e.message : e), 'err');
			sendResult('bad', 'Not sent', ['The installer file here could not be loaded or does not match ' +
				'latest.json: nothing was sent.'], ['Choose Download it, or reload the page.']);
			step('iwrite', 'fail', 'not sent');
			return;
		}
		log(own ? 'Sending your own file, ' + file.name + ': ' + file.bytes.length + ' bytes, SHA-256 ' +
			file.sha + '.' : file.name + ' ' + file.version + ': ' + file.bytes.length +
			' bytes, SHA-256 ' + file.sha + ', as latest.json says.');
		stage('Waiting for you to pick the OP-XY...');
		opened = simMtp() ? await simOpenMtp(openFailure) : await openMtp();
		const d = opened.device;
		if (d)
			log('Device: ' + d.name + ', USB ' + d.vendorId.toString(16).padStart(4, '0') + ':' +
				d.productId.toString(16).padStart(4, '0') + (d.classes.length ? ', interfaces ' +
				d.classes.join(' ') : '') + (d.number !== null ? ', MTP interface ' + d.number : '') + '.');
		if (!opened.ok) {
			if (opened.error)
				log('USB ' + opened.step + ': ' + opened.error.name + ': ' + opened.error.message, 'err');
			sendResult('bad', 'Not sent', [opened.text], opened.advice);
			step('iwrite', 'fail', 'not sent');
			return;
		}
		if (IW.isOp1f(d)) {
			sendResult('bad', 'Not sent', [IW.NOT_FOR_1F[0]], IW.NOT_FOR_1F.slice(1));
			step('iwrite', 'fail', 'not sent');
			return;
		}
		const t = opened.transport;
		t.onLost = () => log('The unit went away.', 'warn');
		const shown = { n: 0 };
		const r = await mtpPut(t, file.bytes, { name: INSTALL_NAME, fileName: file.name, hooks: {
			onStage: (name, text) => stage(text),
			onProgress: (done, total) => {
				$('i-sendbar').style.width = (total ? Math.round(100 * done / total) : 0) + '%';
			},
			onLine: (line) => {
				shown.n++;
				log('  ' + line);
			},
		} });
		for (const line of r.lines.slice(shown.n))
			log('  ' + line, r.outcome === 'verified' ? '' : 'err');
		if (r.outcome === 'verified') {
			sendResult('ok', 'Sent and verified', [r.text], SEND_NEXT);
			step('iwrite', 'done', 'sent');
		} else {
			sendResult('bad', r.changed ? 'Not a good copy' : 'Not sent', [r.text], r.advice);
			step('iwrite', 'fail', r.changed ? 'not a good copy' : 'not sent');
		}
	} catch (e) {
		log('Send: ' + (e && e.name ? e.name + ': ' : '') + (e && e.message ? e.message : e), 'err');
		sendResult('bad', 'Not sent', ['The copy stopped: ' + (e && e.message ? e.message : e) + '.'], []);
		step('iwrite', 'fail', 'not sent');
	} finally {
		if (opened && opened.ok)
			await opened.transport.close();
		$('i-sendrun').hidden = true;
		sending = false;
		installFile();
	}
}

function startSend() {
	$('isrc-send').addEventListener('change', installSource);
	$('isrc-get').addEventListener('change', installSource);
	$('i-send').addEventListener('click', sendInstaller);
	if (!canSend()) {
		$('isrc-send').disabled = true;
		$('isrc-get').checked = true;
		$('i-sendver').textContent = 'needs WebUSB: Chrome or Edge';
	}
	$('ifile-latest').addEventListener('change', installFile);
	$('ifile-own').addEventListener('change', installFile);
	wireDrop('i-drop', 'i-file', takeOwnInstaller);
	installSource();
	installFile();
	releaseMeta().then((meta) => {
		latestVersion = meta.version;
		$('i-relver').textContent = 'waffle-install.ldr ' + meta.version;
	}, () => {
		// no release here: the person's own file is the only one to send
		$('i-relver').textContent = 'not available here';
		$('ifile-latest').disabled = true;
		$('ifile-own').checked = true;
	}).then(installFile);
}

function startInstall() {
	releaseLink().catch(() => {});
	startSend();
	const f = $('i-ferret');
	f.replaceChildren(el('b', '', 'Developer mode never turned on? '),
			  'Turn it on with the ');
	if (FERRET_URL) {
		const a = el('a', '', 'ferret tool');
		a.href = FERRET_URL;
		a.target = '_blank';
		a.rel = 'noopener';
		f.append(a);
	} else {
		f.append(el('b', '', 'ferret tool'));
	}
	f.append(' first. This page does nothing for that step.');
	$('i-connect').addEventListener('click', instConnect);
	$('i-disconnect').addEventListener('click', instDisconnect);
	$('i-report').addEventListener('click', saveReport);
	if (simInstaller()) {
		$('i-simbox').hidden = false;
		$('i-connhint').textContent = 'A simulated installer answers this page. The Simulator buttons stand ' +
			'for what you do on the unit.';
		for (const b of $('i-simbox').querySelectorAll('button'))
			b.addEventListener('click', async () => {
				log('Simulator: ' + b.textContent + '.', 'warn');
				if (!await simAct(b.dataset.sim))
					log('Simulator: not now.', 'warn');
			});
	}
}

// ---- wiring ----

function render() {
	renderGuided();
	renderAdvanced();
}

// which: 'install', 'guided' or 'advanced'; Recovery opens the last of its two modes
let recoveryMode = 'guided';
function tab(which) {
	$('main').hidden = false;
	if (which !== 'install')
		recoveryMode = which;
	for (const name of ['guided', 'advanced', 'install']) {
		$(name).hidden = name !== which;
		$('tab-' + name).setAttribute('aria-selected', String(name === which));
	}
	$('tab-recover').setAttribute('aria-selected', String(which !== 'install'));
	$('recmodes').hidden = which === 'install';
	// the recovery menu's connect and unit steps belong to Guided and Advanced
	$('s-menu').hidden = $('s-unit').hidden = which === 'install';
	if (which === 'advanced')
		$('d-log').open = true;
}

function start() {
	$('tab-guided').addEventListener('click', () => tab('guided'));
	$('tab-advanced').addEventListener('click', () => tab('advanced'));
	$('tab-install').addEventListener('click', () => tab('install'));
	$('tab-recover').addEventListener('click', () => tab(recoveryMode));
	startInstall();
	$('connect').addEventListener('click', doConnect);
	$('disconnect').addEventListener('click', () => disconnected(''));
	$('refresh').addEventListener('click', refresh);
	$('agree').addEventListener('change', render);
	$('force').addEventListener('change', render);
	for (const r of document.querySelectorAll('input[name="wopt"]'))
		r.addEventListener('change', render);
	$('write').addEventListener('click', guidedWrite);
	$('restart').addEventListener('click', guidedRestart);
	$('toadvanced').addEventListener('click', () => tab('advanced'));
	$('clear').addEventListener('click', () => $('log').replaceChildren());
	wireDrop('drop', 'file', 'guided');
	startRelease();
	wireDrop('adrop', 'afile', 'adv');
	for (const card of document.querySelectorAll('.cmd'))
		card.querySelector('button').addEventListener('click', () =>
			advancedRun(card.dataset.cmd, card.querySelector('h3').textContent));
	if (simulated())
		for (const [id, which] of [['dropsub', 'guided'], ['adropsub', 'adv']]) {
			const b = el('button', 'btn small', 'Use the simulator\'s sample image');
			b.type = 'button';
			b.addEventListener('click', async (e) => {
				e.preventDefault();
				await loadImage(await simImage(), 'sample.ldr', which);
			});
			$(id).parentElement.after(b);
		}
	checkBrowser();
	render();
	// nothing is chosen at first, and the steps show once a choice is picked;
	// a simulated unit opens its own tab
	// ?mode=install, or ?mode=upgrade or ?mode=recover, opens that choice
	const mode = new URLSearchParams(location.search).get('mode');
	if (mode === 'install' || mode === 'upgrade' || mode === 'recover')
		tab(mode === 'install' ? 'install' : 'guided');
	else if (simulated())
		tab(simInstaller() || simMtp() ? 'install' : 'guided');
}

start();
