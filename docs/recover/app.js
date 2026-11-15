// SPDX-License-Identifier: GPL-2.0-or-later
// The recovery page: the DOM over core.js. Every decision and every sentence
// about a command comes from core.js, which is a port of tools/wbrecover.py;
// this file shows them, asks, and reports.
import {
	Link, connect, getInfo, describe, checkImage, planWrite, planSelect, planAct, planRestart,
	runWrite, runSelect, runAct, runRestart,
} from './core.js';
import { SerialTransport, USB_FILTERS, serialSupported } from './serial.js';
import { simulated, SimTransport, simImage } from './sim.js';

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
	[/wbrecover\.py write --running(?: IMAGE(?:\.ldr)?)?/g, 'Write A'],
	[/wbrecover\.py write --new-front(?: IMAGE(?:\.ldr)?)?/g, 'Try an image with another front'],
	[/wbrecover\.py write(?: IMAGE(?:\.ldr)?)?/g, 'Write B'],
	[/wbrecover\.py select(?: IMAGE(?:\.ldr)?)?/g, 'Select'],
	[/wbrecover\.py put-back/g, 'Put back'],
	[/wbrecover\.py info/g, 'Read again'],
	[/wbrecover\.py (try|switch|keep|restart)/g, (m, c) => c[0].toUpperCase() + c.slice(1)],
	[/wbrecover\.py/g, 'the Advanced tab'],
	[/--force/g, 'Force'],
	[/Power \+ 4/g, 'Power + M4'],
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
	]);
	if (serial && secure) {
		$('checkhint').textContent = simulated() ?
			'A simulated unit answers this page: nothing here reaches a real OP-XY.' :
			'This browser can talk to the unit.';
		step('check', 'done', 'ready');
		step('menu', 'active');
		$('connect').disabled = false;
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
		const { hello, info } = await connect(state.link);
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
		row.append(el('span', 'sname', s.name), el('span', 'sver', s.version ? 'v' + s.version : 'no image'),
			   el('span', 'chip ' + (s.good ? 'on' : 'off'), s.state));
		if (s.running)
			row.append(el('span', 'chip warn', 'running'));
		if (s.selected)
			row.append(el('span', 'chip', 'selected'));
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
	if (image.version)
		c.push(['v' + image.version, 'on']);
	c.push([image.ok ? 'checked' : 'not usable', image.ok ? 'on' : 'off']);
	if (image.warnings.length)
		c.push(['see the note', 'warn']);
	chips($(adv ? 'aimgchips' : 'imgchips'), c);
	says($(adv ? 'aimgnotes' : 'imgnotes'), image.problems.concat(image.warnings), !image.ok);
	log(name + ': ' + (image.ok ? 'a usable image' : image.problems.join('; ')) +
	    (image.version ? ', v' + image.version : '') + ', SHA-256 ' + image.sha256.slice(0, 16) + '...',
	    image.ok ? 'ok' : 'err');
	render();
}

function wireDrop(dropId, fileId, which) {
	const drop = $(dropId), file = $(fileId);
	const take = async (f) => {
		if (f)
			await loadImage(new Uint8Array(await f.arrayBuffer()), f.name, which);
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
			if (name !== 'countdown') {
				ui.count.hidden = true;
				if (ui.fig)
					ui.fig.hidden = true;
			}
			if (name !== 'done')
				log(STAGES[name] || name);
		},
		onProgress(done, total) {
			ui.bar.style.width = (total ? Math.round(100 * done / total) : 0) + '%';
		},
		onCountdown(left) {
			ui.count.hidden = false;
			if (ui.fig)
				ui.fig.hidden = false;
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
			   count: $('count'), countn: $('countn'), fig: $('countfig') });

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
	     state.image ? (state.image.ok ? 'v' + (state.image.version || '?') : 'not usable') : '');
	$('toadvanced').hidden = true;
	$('agreewrap').hidden = true;
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
			count: $('acount'), countn: $('acountn'), fig: null });

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

function renderAdvanced() {
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

// ---- wiring ----

function render() {
	renderGuided();
	renderAdvanced();
}

function tab(which) {
	const adv = which === 'advanced';
	$('guided').hidden = adv;
	$('advanced').hidden = !adv;
	$('tab-guided').setAttribute('aria-selected', String(!adv));
	$('tab-advanced').setAttribute('aria-selected', String(adv));
	if (adv)
		$('d-log').open = true;
}

function start() {
	$('tab-guided').addEventListener('click', () => tab('guided'));
	$('tab-advanced').addEventListener('click', () => tab('advanced'));
	$('connect').addEventListener('click', doConnect);
	$('disconnect').addEventListener('click', () => disconnected(''));
	$('refresh').addEventListener('click', refresh);
	$('agree').addEventListener('change', render);
	$('force').addEventListener('change', render);
	$('write').addEventListener('click', guidedWrite);
	$('restart').addEventListener('click', guidedRestart);
	$('toadvanced').addEventListener('click', () => tab('advanced'));
	$('clear').addEventListener('click', () => $('log').replaceChildren());
	wireDrop('drop', 'file', 'guided');
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
}

start();
