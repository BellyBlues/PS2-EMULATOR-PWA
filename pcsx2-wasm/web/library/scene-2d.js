// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The disc library (library.html): a folder of disc images becomes a shelf of discs, and the chosen
// one goes into the tray of a photographed PS2. A standalone study for now; nothing boots. On the
// player page the boot would start when a disc is chosen, and the insert, then the lights going
// down, would cover the BIOS logos and the game's loading until its first frame (see onInserted).
//
// The scene is flat: one photo of the console (library/ps2.webp, Evan-Amos, public domain), and on
// top of it the layers that open the tray. The tray's front is the photo's own panel, cut out and
// slid along the console's depth; behind it the plate and the disc lie on the tray's plane, drawn
// with an affine transform measured from the photo, and are clipped to what is in front of the
// console's face. All coordinates are the photo's units (1100 across) on a stage scaled to fit.

import { readFolder, filesFromDrop, onlineCover, discMeta, noDiscsText, shelfAnnouncement, demoDiscs, fontsReady } from './folder.js';
import { discSvg, hash } from './disc-art.js';

const $ = (id) => document.getElementById(id);
const body = document.body;
const els = {
	room: $('room'), stage: $('stage'), console: $('console'), hole: $('hole'), trayClip: $('tray-clip'),
	trayMove: $('tray-move'), plate: $('plate'), plateSide: $('plate-side'), panel: $('panel'), shelf: $('shelf'),
	info: $('info'), infoTitle: $('info-title'), infoMeta: $('info-meta'), infoFile: $('info-file'),
	intake: $('intake'), intakeTitle: $('intake-title'), intakeBtn: $('intake-btn'), intakeAside: $('intake-aside'),
	intakeError: $('intake-error'), started: $('started'), startedTitle: $('started-title'), eject: $('eject-btn'),
	folderBtn: $('folder-btn'), folderInput: $('folder-input'), count: $('count'), announce: $('announce'),
};

const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
const ms = (t) => (reducedMotion.matches ? 0 : t);
const sleep = (t) => new Promise((r) => setTimeout(r, t));

// ------------------------------------------------------------------ geometry
// Measured on the photo (1100 × 662): the tray's front panel, and how far and which way it travels
// when the tray opens (along the console's side edges, about 136 mm of its 182 mm depth). The tray
// is about 133 mm across; a disc is 120 mm.
const TRAY = {
	tl: [436, 392], tr: [768, 478], br: [768, 526], bl: [436, 440],
	travel: [-168, 154],
	widthMm: 133, travelMm: 136, discMm: 120, sideH: 12,
};
const DISC_PX = 250; // a disc element, before any transform

const LAYOUTS = {
	wide: { w: 1240, h: 1090, photo: [70, 410], shelfY: 178, scale: 1, gap1: 236, gapN: 104, span: 5, infoY: 316, cardY: 0.14 },
	tall: { w: 1100, h: 1560, photo: [0, 880], shelfY: 380, scale: 1.6, gap1: 330, gapN: 80, span: 3, infoY: 640, cardY: 0.12 },
};
let G = LAYOUTS.wide;

const add = (a, b) => [a[0] + b[0], a[1] + b[1]];
const sub = (a, b) => [a[0] - b[0], a[1] - b[1]];
const mul = (a, k) => [a[0] * k, a[1] * k];
const len = (a) => Math.hypot(a[0], a[1]);
const onStage = (p) => add(p, G.photo);
const f = (n) => +n.toFixed(3);
const poly = (pts) => `polygon(${pts.map((p) => `${f(p[0])}px ${f(p[1])}px`).join(', ')})`;
const matrix = (m) => `matrix(${m.map(f).join(', ')})`;

// Pixels per millimetre along the tray's width (U) and its travel (V), on the photo.
function trayAxes() {
	const across = sub(TRAY.tr, TRAY.tl);
	return { U: mul(across, 1 / TRAY.widthMm), V: mul(TRAY.travel, 1 / TRAY.travelMm) };
}

// The linear part of a disc lying in the tray, spun by `spin` degrees in its own plane.
function trayLinear(spin) {
	const { U, V } = trayAxes();
	const k = TRAY.discMm / DISC_PX;
	const r = (spin * Math.PI) / 180, c = Math.cos(r), s = Math.sin(r);
	// [U V] · k · R(spin), as CSS matrix(a, b, c, d): columns (a, b) and (c, d).
	return [k * (U[0] * c + V[0] * s), k * (U[1] * c + V[1] * s), k * (-U[0] * s + V[0] * c), k * (-U[1] * s + V[1] * c)];
}

// The centre of a disc in the tray, on the stage.
function trayCentre(open) {
	const mid = mul(add(TRAY.tl, TRAY.tr), 0.5);
	return onStage(add(mid, mul(TRAY.travel, open ? 0.5 : -0.5)));
}

// A disc on the shelf, `d` places from the one in front: [linear(4), centre(2)].
function shelfPose(d, angle) {
	const ad = Math.abs(d), s = Math.sign(d), near = Math.min(ad, 1), far = Math.max(ad - 1, 0);
	const x = G.w / 2 + s * (near * G.gap1 + far * G.gapN);
	const scale = G.scale * (ad <= 1 ? 1 - 0.3 * ad : Math.max(0.5, 0.7 - 0.05 * far));
	const r = ((angle * near) * Math.PI) / 180;
	return [scale * Math.cos(r), scale * Math.sin(r), -scale * Math.sin(r), scale * Math.cos(r), x, G.shelfY];
}

function applyLayout() {
	const room = els.room.getBoundingClientRect();
	G = room.width / Math.max(room.height, 1) < 0.95 ? LAYOUTS.tall : LAYOUTS.wide;
	const k = Math.min(room.width / G.w, room.height / G.h);
	const s = els.stage.style;
	s.setProperty('--stage-w', `${G.w}px`);
	s.setProperty('--stage-h', `${G.h}px`);
	s.setProperty('--k', k);
	document.documentElement.style.setProperty('--disc', `${DISC_PX}px`);
	for (const el of [els.console, els.panel]) {
		el.style.left = `${G.photo[0]}px`;
		el.style.top = `${G.photo[1]}px`;
	}
	const corners = [TRAY.tl, TRAY.tr, TRAY.br, TRAY.bl];
	els.stage.style.setProperty('--hole', poly(corners));
	els.stage.style.setProperty('--panel', poly(corners));

	// What is in front of the console's face: the side of the tray's top edge the tray opens to.
	const tl = onStage(TRAY.tl), tr = onStage(TRAY.tr);
	const u = mul(sub(tr, tl), 1 / len(sub(tr, tl)));
	let n = [-u[1], u[0]];
	if (n[0] * TRAY.travel[0] + n[1] * TRAY.travel[1] < 0)
		n = mul(n, -1);
	const a = sub(tl, mul(u, 4000)), b = add(tr, mul(u, 4000));
	els.stage.style.setProperty('--outside', poly([a, b, add(b, mul(n, 4000)), add(a, mul(n, 4000))]));

	// The plate, and its right-hand side wall, in millimetres on the tray's plane.
	const { U, V } = trayAxes();
	els.plate.style.width = `${TRAY.widthMm}px`;
	els.plate.style.height = `${TRAY.travelMm}px`;
	els.plate.style.transform = matrix([U[0], U[1], V[0], V[1], tl[0], tl[1]]);
	els.plateSide.style.width = `${TRAY.travelMm}px`;
	els.plateSide.style.height = `${TRAY.sideH}px`;
	els.plateSide.style.transform = matrix([V[0], V[1], 0, 1, tr[0], tr[1]]);
	if (!trayBusy)
		setTray(body.dataset.tray === 'open');

	const top = room.height / 2 + (G.infoY - G.h / 2) * k;
	els.info.style.setProperty('--info-top', `${Math.round(top)}px`);
	els.intake.style.setProperty('--card-top', `${Math.round(room.height * G.cardY)}px`);
	placeDiscs();
	if (current && (mode === 'inserted' || mode === 'inserting'))
		current.el.style.transform = matrix([...trayLinear(current.spinIn), ...trayCentre(true)]);
}

const trayMoveT = (open) => (open ? 'translate(0px, 0px)' : `translate(${-TRAY.travel[0]}px, ${-TRAY.travel[1]}px)`);
const panelT = (open) => (open ? `translate(${TRAY.travel[0]}px, ${TRAY.travel[1]}px)` : 'translate(0px, 0px)');

function setTray(open) {
	els.trayMove.style.transform = trayMoveT(open);
	els.panel.style.transform = panelT(open);
}

// ------------------------------------------------------------------ state

let mode = 'empty'; // empty | scanning | shelf | inserting | inserted | ejecting
let discs = [];
let pos = 0;
let target = 0;
let current = null;
let trayBusy = false;

function setMode(m) {
	mode = m;
	body.dataset.mode = m;
	els.intake.hidden = !(m === 'empty' || m === 'scanning');
	els.started.hidden = m !== 'inserted';
	els.folderBtn.hidden = !(discs.length && m === 'shelf');
	els.count.hidden = !discs.length;
}

function announce(text) {
	els.announce.textContent = '';
	requestAnimationFrame(() => (els.announce.textContent = text));
}

// ------------------------------------------------------------------ the shelf

function buildDiscs() {
	els.shelf.replaceChildren();
	discs.forEach((d, i) => {
		const el = document.createElement('div');
		el.className = 'disc';
		el.id = `disc-${i}`;
		el.setAttribute('role', 'option');
		el.setAttribute('aria-selected', 'false');
		el.setAttribute('aria-label', [d.title, d.serial, d.region].filter(Boolean).join(', '));
		el.dataset.index = i;
		el.style.visibility = 'hidden';
		d.el = el;
		d.angle = (hash(d.fileName) % 300) - 150;
		d.spinIn = 160 + (hash(d.title) % 160);
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
		ensureArt(d);
		d.el.style.visibility = '';
		d.el.style.zIndex = String(50 - Math.round(ad * 4));
		d.el.style.transform = matrix(shelfPose(off, d.angle));
		d.el.style.setProperty('--fade', ad > G.span ? Math.max(0, G.span + 1 - ad).toFixed(3) : '1');
	}
}

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

function select(i) {
	if (!discs.length)
		return;
	i = Math.max(0, Math.min(discs.length - 1, i));
	discs[target]?.el.setAttribute('aria-selected', 'false');
	target = i;
	const d = discs[i];
	d.el.setAttribute('aria-selected', 'true');
	els.shelf.setAttribute('aria-activedescendant', d.el.id);
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
	pos = -Math.min(discs.length, G.span); // they roll in from the left
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

function moveTray(open, duration, easing) {
	trayBusy = true;
	body.dataset.tray = 'moving';
	const opts = { duration, easing };
	return Promise.all([
		animate(els.trayMove, [{ transform: trayMoveT(!open) }, { transform: trayMoveT(open) }], opts),
		animate(els.panel, [{ transform: panelT(!open) }, { transform: panelT(open) }], opts),
	]).then(() => {
		trayBusy = false;
		body.dataset.tray = open ? 'open' : 'closed';
	});
}

const easeInOut = (t) => (t < 0.5 ? 4 * t * t * t : 1 - (-2 * t + 2) ** 3 / 2);
const easeIn = (t) => t * t;
const lerp = (a, b, t) => a + (b - a) * t;

// Keyframes for a disc going from the shelf's front place up and over the open tray, turning flat
// onto the tray's plane, then down into it. Sampled here so the path can arc and the disc can
// spin while it flattens; the browser only joins the samples.
function flightFrames(d) {
	const shelf = shelfPose(0, d.angle);
	const start = [shelf[4], shelf[5]];
	const land = trayCentre(true);
	const hover = [land[0], land[1] - 190];
	const ctrl = [lerp(start[0], hover[0], 0.25), Math.min(start[1], hover[1]) - 70];
	const frames = [];
	const N = 32, split = 0.66;
	for (let i = 0; i <= N; i++) {
		const t = i / N;
		let c, flat, spin;
		if (t <= split) {
			const u = easeInOut(t / split);
			// A quadratic Bézier from the shelf to above the tray.
			c = [
				(1 - u) ** 2 * start[0] + 2 * (1 - u) * u * ctrl[0] + u * u * hover[0],
				(1 - u) ** 2 * start[1] + 2 * (1 - u) * u * ctrl[1] + u * u * hover[1],
			];
			flat = u;
			spin = d.spinIn * 0.6 * u;
		} else {
			const u = easeIn((t - split) / (1 - split));
			c = [lerp(hover[0], land[0], u), lerp(hover[1], land[1], u)];
			flat = 1;
			spin = d.spinIn * (0.6 + 0.4 * (1 - (1 - u) ** 2));
		}
		const tray = trayLinear(spin);
		const up = G.scale;
		// From an upright disc on the shelf to one lying in the tray, spinning as it goes.
		const r = (spin * Math.PI) / 180;
		const shelfLin = [up * Math.cos(r), up * Math.sin(r), -up * Math.sin(r), up * Math.cos(r)];
		const lin = shelfLin.map((v, k) => lerp(v, tray[k], flat));
		frames.push({ transform: matrix([...lin, ...c]), offset: t });
	}
	return frames;
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

	// The disc spins upright first, so the flight starts from the pose its frames start from.
	d.el.style.transform = matrix(shelfPose(0, d.angle));
	const opening = moveTray(true, ms(700), 'cubic-bezier(.3,0,.15,1)');
	await sleep(ms(180));
	d.el.classList.add('is-flying');
	const frames = flightFrames(d);
	await animate(d.el, frames, { duration: ms(1050) });
	await opening;
	// It lies in the tray now: from here it moves with the tray, behind the console's face.
	d.el.classList.remove('is-flying');
	d.el.style.transform = frames[frames.length - 1].transform;
	els.trayMove.append(d.el);
	await sleep(ms(160));
	await moveTray(false, ms(760), 'cubic-bezier(.5,0,.25,1)');
	body.dataset.disc = 'in';
	const booted = onInserted(d);
	await sleep(ms(520));
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
	await moveTray(true, ms(700), 'cubic-bezier(.3,0,.15,1)');
	await sleep(ms(120));
	// Out of the tray and back onto the shelf, the way it came.
	els.shelf.append(d.el);
	d.el.classList.add('is-flying');
	const frames = flightFrames(d).reverse().map((fr) => ({ ...fr, offset: 1 - fr.offset }));
	await animate(d.el, frames, { duration: ms(950) });
	d.el.classList.remove('is-flying');
	for (const o of discs)
		o.el.classList.remove('is-away');
	current = null;
	setMode('shelf');
	placeDiscs();
	renderInfo(d);
	els.shelf.focus({ preventScroll: true });
	announce(`${d.title} is back on the shelf.`);
	await moveTray(false, ms(620), 'cubic-bezier(.5,0,.25,1)');
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

const stageScale = () => parseFloat(els.stage.style.getPropertyValue('--k')) || 1;

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
	pos = Math.max(-0.4, Math.min(discs.length - 0.6, dragging.pos - dx / (G.gap1 * stageScale())));
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
	const v = t1 > t0 ? (x1 - x0) / (t1 - t0) : 0; // px per ms
	const fling = (-v * 220) / (G.gap1 * stageScale());
	select(Math.round(pos + Math.max(-6, Math.min(6, fling))));
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
const params = new URLSearchParams(location.search);
// ?tray=open shows the tray open, for checking the cut-out against the photo.
if (params.get('tray') === 'open') {
	body.dataset.tray = 'open';
	setTray(true);
}
if (params.has('demo')) {
	discs = demoDiscs();
	fontsReady().then(() => showShelf());
} else {
	setMode('empty');
}
