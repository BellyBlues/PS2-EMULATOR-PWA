// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The disc library in 3D (library-3d.html, kept as a copy of the first study; library.html is the
// 2D one with a photo of the console): a folder of disc images becomes a shelf of discs, and the chosen
// one goes into the console's tray. A standalone study for now; nothing boots. On the player page
// the boot would start when a disc is chosen, and the insert (about 2.5 s, then the lights going
// down) would cover the BIOS logos and the game's loading until its first frame (see onInserted).
//
// The scene is one CSS 3D world: the console, its tray and every disc share it, so a disc can fly
// from the shelf into the tray by interpolating one transform. Every transform in the world has the
// same shape,
//     translate3d(A) rotateX(B) rotateY(C) translate3d(D) rotateX(E) rotateZ(F)
// (a place, the frame of the console or the shelf, a place in that frame, then the disc's own tilt
// and spin), so the browser interpolates it part by part and a flight never takes a short cut.

import { readFolder, filesFromDrop, onlineCover, discMeta, noDiscsText, shelfAnnouncement, demoDiscs, fontsReady } from './folder.js';
import { discSvg, hash } from './disc-art.js';

const $ = (id) => document.getElementById(id);
const body = document.body;
const els = {
	room: $('room'), stage: $('stage'), world: $('world'), console: $('console'), tray: $('tray'), shelf: $('shelf'),
	info: $('info'), infoTitle: $('info-title'), infoMeta: $('info-meta'), infoFile: $('info-file'),
	intake: $('intake'), intakeTitle: $('intake-title'), intakeBtn: $('intake-btn'), intakeAside: $('intake-aside'),
	intakeError: $('intake-error'), started: $('started'), startedTitle: $('started-title'), eject: $('eject-btn'),
	folderBtn: $('folder-btn'), folderInput: $('folder-input'), count: $('count'), announce: $('announce'),
};

const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
const ms = (t) => (reducedMotion.matches ? 0 : t);
const sleep = (t) => new Promise((r) => setTimeout(r, t));

// ------------------------------------------------------------------ geometry
// Stage units: the stage is drawn at this size and scaled to fit the room. Wide rooms get the
// console under a row of discs; tall ones (phones) the same with less room on the sides.
const LAYOUTS = {
	wide: {
		w: 1280, h: 820, persp: 1150, originY: 190, disc: 236,
		shelfY: -215, gap1: 250, gapN: 104, depth1: 210, depthN: 30, tilt: 62, span: 6,
		con: { x: 0, y: 262, z: -330, rx: -6, ry: -12, w: 600, d: 380, h: 84 },
		infoY: 352, cardY: 0.14,
	},
	tall: {
		w: 720, h: 1180, persp: 1250, originY: 330, disc: 236,
		shelfY: -330, gap1: 196, gapN: 56, depth1: 240, depthN: 34, tilt: 66, span: 4,
		con: { x: 0, y: 330, z: -300, rx: -8, ry: -10, w: 560, d: 360, h: 80 },
		infoY: 420, cardY: 0.12,
	},
};
const TRAY = { w: 290, d: 290, h: 18, top: 14, open: 262 };

let G = LAYOUTS.wide;

const fmt = (n) => `${+n.toFixed(2)}`;
function tf(a, b, c, d, e, f) {
	return `translate3d(${fmt(a[0])}px, ${fmt(a[1])}px, ${fmt(a[2])}px) rotateX(${fmt(b)}deg) rotateY(${fmt(c)}deg) ` +
		`translate3d(${fmt(d[0])}px, ${fmt(d[1])}px, ${fmt(d[2])}px) rotateX(${fmt(e)}deg) rotateZ(${fmt(f)}deg)`;
}

const conPlace = () => [G.con.x, G.con.y, G.con.z];
// The tray's centre, in the console's frame: its plate is TRAY.top below the console's top, flush
// with the front when closed.
const trayCentre = (open) => [0, -G.con.h / 2 + TRAY.top, G.con.d / 2 - TRAY.d / 2 + (open ? TRAY.open : 0)];
const trayTransform = (open) => {
	const c = trayCentre(open);
	return `translate3d(0px, ${fmt(c[1])}px, ${fmt(c[2])}px)`;
};
// A disc lying on the tray (lift raises it above the plate).
function inTray(open, spin, lift = 0, tilt = 90) {
	const c = trayCentre(open);
	return tf(conPlace(), G.con.rx, G.con.ry, [c[0], c[1] - 1.5 - lift, c[2]], tilt, spin);
}

// A disc on the shelf, `d` places from the one in front.
function onShelf(d, angle) {
	const ad = Math.abs(d), s = Math.sign(d), near = Math.min(ad, 1), far = Math.max(ad - 1, 0);
	const x = s * (near * G.gap1 + far * G.gapN);
	const z = -(near * G.depth1 + far * G.depthN);
	return tf([x, G.shelfY, z], 0, -s * near * G.tilt, [0, 0, 0], 0, angle * near);
}

function applyLayout() {
	const room = els.room.getBoundingClientRect();
	const tall = room.width / Math.max(room.height, 1) < 0.95;
	G = tall ? LAYOUTS.tall : LAYOUTS.wide;
	const k = Math.min(room.width / G.w, room.height / G.h);
	const s = els.stage.style;
	s.setProperty('--stage-w', `${G.w}px`);
	s.setProperty('--stage-h', `${G.h}px`);
	s.setProperty('--persp', `${G.persp}px`);
	s.setProperty('--origin-y', `${G.originY}px`);
	s.setProperty('--k', k);
	const r = document.documentElement.style;
	r.setProperty('--disc', `${G.disc}px`);
	r.setProperty('--cw', `${G.con.w}px`);
	r.setProperty('--cd', `${G.con.d}px`);
	r.setProperty('--ch', `${G.con.h}px`);
	r.setProperty('--tw', `${TRAY.w}px`);
	r.setProperty('--td', `${TRAY.d}px`);
	r.setProperty('--th', `${TRAY.h}px`);
	r.setProperty('--tray-top', `${TRAY.top}px`);
	// The text sits in the room, unscaled, at its place on the stage.
	const top = room.height / 2 + (G.infoY - G.h / 2) * k;
	els.info.style.setProperty('--info-top', `${Math.round(top)}px`);
	els.intake.style.setProperty('--card-top', `${Math.round(room.height * G.cardY)}px`);
	els.console.style.transform = `translate3d(${G.con.x}px, ${G.con.y}px, ${G.con.z}px) rotateX(${G.con.rx}deg) rotateY(${G.con.ry}deg)`;
	if (!trayBusy)
		els.tray.style.transform = trayTransform(body.dataset.tray === 'open');
	placeDiscs();
	if (mode === 'inserted' && current)
		current.el.style.transform = inTray(false, current.spinIn);
}

// ------------------------------------------------------------------ state

let mode = 'empty'; // empty | scanning | shelf | inserting | inserted | ejecting
let discs = [];
let pos = 0; // the shelf's position, in discs (fractional while it moves)
let target = 0; // the disc it settles on
let current = null; // the disc in the console
let trayBusy = false;

function setMode(m) {
	mode = m;
	body.dataset.mode = m;
	els.intake.hidden = !(m === 'empty' || m === 'scanning');
	els.started.hidden = m !== 'inserted';
	els.folderBtn.hidden = !(discs.length && (m === 'shelf'));
	els.count.hidden = !discs.length;
}

function announce(text) {
	els.announce.textContent = '';
	requestAnimationFrame(() => (els.announce.textContent = text));
}

// ------------------------------------------------------------------ the shelf

function discLabel(d) {
	return [d.title, d.serial, d.region].filter(Boolean).join(', ');
}

function buildDiscs() {
	els.shelf.replaceChildren();
	discs.forEach((d, i) => {
		const el = document.createElement('div');
		el.className = 'disc';
		el.id = `disc-${i}`;
		el.setAttribute('role', 'option');
		el.setAttribute('aria-selected', 'false');
		el.setAttribute('aria-label', discLabel(d));
		el.dataset.index = i;
		el.style.visibility = 'hidden';
		d.el = el;
		d.angle = (hash(d.fileName) % 360) - 180;
		d.spinIn = 200 + (hash(d.title) % 140);
		els.shelf.append(el);
	});
	els.count.textContent = `${discs.length} ${discs.length === 1 ? 'disc' : 'discs'}`;
}

function ensureArt(d) {
	if (d.built)
		return;
	d.built = true;
	d.el.append(discSvg(d));
	if (!d.cover)
		onlineCover(d.serial).then((url) => {
			if (url) {
				d.cover = url;
				d.el.querySelector('.disc-svg')?.replaceWith(discSvg(d));
			}
		});
}

function placeDiscs() {
	for (let i = 0; i < discs.length; i++) {
		const d = discs[i];
		if (d === current && mode !== 'shelf')
			continue;
		const off = i - pos;
		const ad = Math.abs(off);
		if (ad > G.span + 1) {
			if (d.el.style.visibility !== 'hidden')
				d.el.style.visibility = 'hidden';
			continue;
		}
		if (ad < G.span + 2)
			ensureArt(d);
		d.el.style.visibility = '';
		d.el.style.transform = onShelf(off, d.angle);
		d.el.style.setProperty('--fade', ad > G.span ? Math.max(0, G.span + 1 - ad).toFixed(3) : '1');
	}
}

// The shelf eases towards `target`, about 90% of the way in 200 ms.
let rafId = 0;
let lastT = 0;
let dragging = null;
function tick(t) {
	rafId = 0;
	const dt = lastT ? Math.min(t - lastT, 50) : 16;
	lastT = t;
	if (!dragging) {
		const k = reducedMotion.matches ? 1 : 1 - Math.exp(-dt / 85);
		pos += (target - pos) * k;
		if (Math.abs(target - pos) < 0.0015)
			pos = target;
	}
	placeDiscs();
	if (pos !== target || dragging)
		rafId = requestAnimationFrame(tick);
	else
		lastT = 0;
}
function kick() {
	if (!rafId)
		rafId = requestAnimationFrame(tick);
}

function select(i, { instant = false } = {}) {
	if (!discs.length)
		return;
	i = Math.max(0, Math.min(discs.length - 1, i));
	if (discs[target])
		discs[target].el.setAttribute('aria-selected', 'false');
	target = i;
	const d = discs[i];
	d.el.setAttribute('aria-selected', 'true');
	els.shelf.setAttribute('aria-activedescendant', d.el.id);
	if (instant)
		pos = i;
	renderInfo(d);
	kick();
}

function renderInfo(d) {
	els.infoTitle.textContent = d.title;
	els.infoMeta.textContent = discMeta(d);
	els.infoFile.textContent = d.fileName;
}

// ------------------------------------------------------------------ reading a folder

async function scan(files) {
	if (mode !== 'empty' && mode !== 'shelf' && mode !== 'scanning')
		return;
	els.intakeError.hidden = true;
	const previous = mode;
	setMode('scanning');
	const result = await readFolder(files, (done, total) => {
		els.intakeAside.textContent = `Reading ${done} of ${total}…`;
	});
	if (!result.discs.length) {
		els.intakeAside.textContent = 'or drop it here';
		els.intakeError.textContent = noDiscsText(result.compressed);
		els.intakeError.hidden = false;
		setMode(previous === 'shelf' ? 'shelf' : 'empty');
		if (mode === 'empty')
			els.intakeTitle.focus();
		return;
	}
	discs = result.discs;
	showShelf(result.compressed);
}

function showShelf(compressed = 0) {
	current = null;
	buildDiscs();
	els.intakeAside.textContent = 'or drop it here';
	setMode('shelf');
	pos = -Math.min(discs.length, G.span); // they slide in from the left
	select(Math.floor((discs.length - 1) / 2));
	els.shelf.focus({ preventScroll: true });
	announce(shelfAnnouncement(discs.length, compressed));
}

// ------------------------------------------------------------------ the insert

function animate(el, frames, opts) {
	const a = el.animate(frames, { fill: 'forwards', ...opts });
	return a.finished.then(() => {
		try {
			a.commitStyles();
		} catch {
			// The element left the rendering tree: nothing to keep.
		}
		a.cancel();
	});
}

function moveTray(open, duration, easing, withDisc) {
	trayBusy = true;
	const opts = { duration, easing };
	const jobs = [animate(els.tray, [{ transform: trayTransform(!open) }, { transform: trayTransform(open) }], opts)];
	if (withDisc)
		jobs.push(animate(withDisc.el, [{ transform: inTray(!open, withDisc.spinIn) }, { transform: inTray(open, withDisc.spinIn) }], opts));
	body.dataset.tray = open ? 'open' : 'closed';
	return Promise.all(jobs).then(() => (trayBusy = false));
}

// The player page will boot here and resolve once the first frame is up; the lights stay down
// until then. In this study the "boot" is just the time the insert takes.
async function onInserted(_disc) {
	await sleep(ms(400));
}

async function insert() {
	if (mode !== 'shelf' || !discs.length)
		return;
	const d = discs[target];
	current = d;
	pos = target;
	placeDiscs();
	setMode('inserting');
	announce(`${d.title} goes in.`);
	for (const o of discs)
		if (o !== d)
			o.el.classList.add('is-away');

	const opening = moveTray(true, ms(640), 'cubic-bezier(.3,.0,.15,1)');
	await sleep(ms(160));
	// Up and over the tray, turning flat, then down into it with a part turn as it settles.
	await animate(d.el, [
		{ transform: onShelf(0, d.angle), easing: 'cubic-bezier(.35,0,.25,1)' },
		{ transform: inTray(true, d.spinIn * 0.55, 150, 72), offset: 0.64, easing: 'cubic-bezier(.55,0,.85,.45)' },
		{ transform: inTray(true, d.spinIn) },
	], { duration: ms(1000) });
	await opening;
	await sleep(ms(140));
	await moveTray(false, ms(700), 'cubic-bezier(.5,0,.25,1)', d);
	body.dataset.disc = 'in';
	const booted = onInserted(d);
	await sleep(ms(420));
	body.dataset.lights = 'down';
	await Promise.all([booted, sleep(ms(900))]);
	if (mode !== 'inserting')
		return;
	els.startedTitle.textContent = `Starting ${d.title}.`;
	setMode('inserted');
	els.startedTitle.focus({ preventScroll: true });
	announce(`${d.title} is in. Starting.`);
}

async function eject() {
	if (mode !== 'inserted' || !current)
		return;
	const d = current;
	setMode('ejecting');
	body.dataset.lights = 'up';
	body.dataset.disc = '';
	await sleep(ms(300));
	await moveTray(true, ms(620), 'cubic-bezier(.3,0,.15,1)', d);
	await sleep(ms(100));
	await animate(d.el, [
		{ transform: inTray(true, d.spinIn), easing: 'cubic-bezier(.15,.55,.45,1)' },
		{ transform: inTray(true, d.spinIn * 0.55, 150, 72), offset: 0.36, easing: 'cubic-bezier(.35,0,.25,1)' },
		{ transform: onShelf(0, d.angle) },
	], { duration: ms(900) });
	for (const o of discs)
		o.el.classList.remove('is-away');
	const closing = moveTray(false, ms(560), 'cubic-bezier(.5,0,.25,1)');
	current = null;
	setMode('shelf');
	renderInfo(d);
	els.shelf.focus({ preventScroll: true });
	announce(`${d.title} is back on the shelf.`);
	await closing;
}

// ------------------------------------------------------------------ input

els.intakeBtn.addEventListener('click', () => els.folderInput.click());
els.folderBtn.addEventListener('click', () => els.folderInput.click());
els.folderInput.addEventListener('change', () => {
	const files = els.folderInput.files;
	if (files && files.length)
		scan(files);
	els.folderInput.value = '';
});
els.eject.addEventListener('click', eject);

const typeahead = { text: '', at: 0 };
els.shelf.addEventListener('keydown', (e) => {
	if (mode !== 'shelf' || e.altKey || e.ctrlKey || e.metaKey)
		return;
	const steps = { ArrowLeft: -1, ArrowRight: 1, ArrowUp: -1, ArrowDown: 1, PageUp: -5, PageDown: 5 };
	if (e.key in steps) {
		select(target + steps[e.key]);
	} else if (e.key === 'Home') {
		select(0);
	} else if (e.key === 'End') {
		select(discs.length - 1);
	} else if (e.key === 'Enter' || e.key === ' ') {
		insert();
	} else if (e.key.length === 1 && /\S/.test(e.key)) {
		// Type a title's first letters to jump to it.
		const now = performance.now();
		typeahead.text = (now - typeahead.at > 700 ? '' : typeahead.text) + e.key.toLowerCase();
		typeahead.at = now;
		const from = typeahead.text.length === 1 ? target + 1 : target;
		for (let n = 0; n < discs.length; n++) {
			const i = (from + n) % discs.length;
			if (discs[i].title.toLowerCase().startsWith(typeahead.text)) {
				select(i);
				break;
			}
		}
	} else {
		return;
	}
	e.preventDefault();
});

document.addEventListener('keydown', (e) => {
	if (e.key === 'Escape' && mode === 'inserted') {
		e.preventDefault();
		eject();
	}
});

// Wheel and trackpad: about one disc per notch, or per 60 px of a trackpad swipe.
let wheelAcc = 0;
let wheelAt = 0;
els.room.addEventListener('wheel', (e) => {
	if (mode !== 'shelf' || !discs.length)
		return;
	e.preventDefault();
	const now = performance.now();
	if (now - wheelAt > 200)
		wheelAcc = 0;
	wheelAt = now;
	const delta = Math.abs(e.deltaX) > Math.abs(e.deltaY) ? e.deltaX : e.deltaY;
	wheelAcc += e.deltaMode === 1 ? delta * 40 : delta;
	const step = Math.trunc(wheelAcc / 60);
	if (step) {
		wheelAcc -= step * 60;
		select(target + step);
	}
}, { passive: false });

// Drag the shelf; a tap on a disc selects it, and on the one in front puts it in.
els.room.addEventListener('pointerdown', (e) => {
	if (mode !== 'shelf' || !discs.length || e.button !== 0 || e.target.closest('button, a, input'))
		return;
	dragging = { x: e.clientX, pos, moved: false, id: e.pointerId, samples: [[performance.now(), e.clientX]], disc: e.target.closest('.disc') };
	els.room.setPointerCapture(e.pointerId);
	body.classList.add('dragging');
});
els.room.addEventListener('pointermove', (e) => {
	if (!dragging || e.pointerId !== dragging.id)
		return;
	const dx = e.clientX - dragging.x;
	if (Math.abs(dx) > 6)
		dragging.moved = true;
	if (!dragging.moved)
		return;
	const k = parseFloat(els.stage.style.getPropertyValue('--k')) || 1;
	pos = Math.max(-0.4, Math.min(discs.length - 0.6, dragging.pos - dx / (G.gap1 * k)));
	dragging.samples.push([performance.now(), e.clientX]);
	if (dragging.samples.length > 6)
		dragging.samples.shift();
	kick();
});
function endDrag(e) {
	if (!dragging || e.pointerId !== dragging.id)
		return;
	const drag = dragging;
	dragging = null;
	body.classList.remove('dragging');
	if (!drag.moved) {
		if (drag.disc) {
			const i = +drag.disc.dataset.index;
			if (i === target)
				insert();
			else
				select(i);
		}
		return;
	}
	const [t0, x0] = drag.samples[0];
	const [t1, x1] = drag.samples[drag.samples.length - 1];
	const k = parseFloat(els.stage.style.getPropertyValue('--k')) || 1;
	const v = t1 > t0 ? (x1 - x0) / (t1 - t0) : 0; // px per ms
	const fling = (-v * 220) / (G.gap1 * k);
	const i = Math.round(pos + Math.max(-6, Math.min(6, fling)));
	select(i);
}
els.room.addEventListener('pointerup', endDrag);
els.room.addEventListener('pointercancel', endDrag);

// Drop a folder (or disc images) anywhere.
let dragDepth = 0;
document.addEventListener('dragenter', (e) => {
	if (![...(e.dataTransfer?.types || [])].includes('Files'))
		return;
	dragDepth++;
	body.dataset.drag = mode === 'empty' || mode === 'shelf' ? 'ok' : '';
});
document.addEventListener('dragleave', () => {
	if (--dragDepth <= 0) {
		dragDepth = 0;
		body.dataset.drag = '';
	}
});
document.addEventListener('dragover', (e) => e.preventDefault());
document.addEventListener('drop', async (e) => {
	e.preventDefault();
	dragDepth = 0;
	body.dataset.drag = '';
	if (mode !== 'empty' && mode !== 'shelf')
		return;
	const files = await filesFromDrop(e.dataTransfer);
	if (files.length)
		scan(files);
});

new ResizeObserver(applyLayout).observe(els.room);

applyLayout();
if (new URLSearchParams(location.search).has('demo')) {
	discs = demoDiscs();
	fontsReady().then(() => showShelf());
} else {
	setMode('empty');
}
