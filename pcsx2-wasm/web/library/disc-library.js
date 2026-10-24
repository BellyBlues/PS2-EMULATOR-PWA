// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The disc library: a folder of disc images becomes a shelf of discs, and the chosen one goes into
// the tray of a photographed PS2. It lives in the player's "Disc library" dialog (index.html, through
// player-library.js), where the insert covers the boot.
//
// The scene is flat: one photo of the console (library/ps2.webp, Evan-Amos, public domain), and on
// top of it the layers that open the tray. The tray's front is the photo's own panel, cut out and
// slid along the console's depth; behind it the plate and the disc lie on the tray's plane, drawn
// with an affine transform measured from the photo, and are clipped to what is in front of the
// console's face. All coordinates are the photo's units (1100 across) on a stage scaled to fit.
//
// createDiscLibrary(root, options) fills `root` and returns { shown, reset }. Options:
//   host: the player (window.PCSX2WebLibrary from app.js): blocked() gives why a disc can't go in now
//         (or ''), insert(file, handle) chooses the disc and queues its boot, status() reports the
//         page ({ view, error }); bios() says whether it has a BIOS ({ ok, reading, error }), and
//         chooseBios() and takeBios(file) give it one: without one, the library asks for it first.
//         Without a host the insert is only rehearsed.
//   onBooted(): with a host, called once the disc is in and the console has slid down out of the room
//         (sink): the player then takes the dialog away and its screen shows the boot. rise() brings the
//         console back up when the library returns.
//   cleanChance: how often a disc won't read and has to be cleaned first (the Easter egg, cleanDisc);
//         index.html?clean=1 makes it every time, and so does Shift held while a disc is put in.

import {
	readFolder, filesFromDrop, filesFromDirectory, onlineCover, discMeta, noDiscsText, shelfAnnouncement,
	canPickFolder, pickFolder, rememberFolder, rememberedFolder, folderPermission,
} from './folder.js';
import { discSvg, hash } from './disc-art.js';
import { makeDataSide, BOTTLE_SVG, BOTTLE_SIZE, NOZZLE, CLOTH_SVG, CLOTH_SIZE } from './disc-clean.js';

const PHOTO_URL = new URL('./ps2.webp', import.meta.url).href;

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
	wide: { w: 1240, h: 1090, photo: [70, 410], shelfY: 178, scale: 1, gap1: 236, gapN: 104, span: 5, infoY: 316, cardY: 0.14, bench: [600, 330, 1.5] },
	tall: { w: 1100, h: 1560, photo: [0, 880], shelfY: 380, scale: 1.6, gap1: 330, gapN: 80, span: 3, infoY: 640, cardY: 0.12, bench: [500, 560, 2.1] },
};


const COPY = {
	setup: 'Set up your PS2',
	biosTitle: 'BIOS',
	biosBody: 'Dumped from your own PS2 (.bin)',
	biosChoose: 'Choose file',
	biosChange: 'Change',
	biosReading: 'Reading…',
	intakeTitle: 'Games',
	intakeBody: 'A folder with your disc images (.iso)',
	choose: 'Choose folder',
	again: () => 'Open again',
	againTitle: () => 'Games',
	againBody: (name) => `In “${name}”; the browser asks once per visit`,
	another: 'Another folder…',
	drop: '',
	// Cover art comes from the folder (PCSX2's names, SLUS-20946.jpg) or ps2-covers by serial: the dialog's credit line.
	setupFine: 'Choose them or drop them here. They stay in this browser; nothing is uploaded.',
	reading: (done, total) => (total ? `Reading ${done} of ${total}…` : 'Reading…'),
	rehearsal: 'This page only rehearses the insert. On the player page, the game would boot while the disc goes in, and its first frame would come up here.',
	starting: (title) => `Starting ${title}.`,
	unreadable: 'Couldn’t read that folder. Try choosing it again.',
};

const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const add = (a, b) => [a[0] + b[0], a[1] + b[1]];
const sub = (a, b) => [a[0] - b[0], a[1] - b[1]];
const mul = (a, k) => [a[0] * k, a[1] * k];
const len = (a) => Math.hypot(a[0], a[1]);
const f = (n) => +n.toFixed(3);
const poly = (pts) => `polygon(${pts.map((p) => `${f(p[0])}px ${f(p[1])}px`).join(', ')})`;
const matrix = (m) => `matrix(${m.map(f).join(', ')})`;
const easeInOut = (t) => (t < 0.5 ? 4 * t * t * t : 1 - (-2 * t + 2) ** 3 / 2);
const easeIn = (t) => t * t;
const lerp = (a, b, t) => a + (b - a) * t;
const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');

// Pixels per millimetre along the tray's width (U) and its travel (V), on the photo.
function trayAxes() {
	return { U: mul(sub(TRAY.tr, TRAY.tl), 1 / TRAY.widthMm), V: mul(TRAY.travel, 1 / TRAY.travelMm) };
}

// The linear part of a disc lying in the tray, spun by `spin` degrees in its own plane:
// [U V] · (disc mm per px) · R(spin), as CSS matrix(a, b, c, d).
function trayLinear(spin) {
	const { U, V } = trayAxes();
	const k = TRAY.discMm / DISC_PX;
	const r = (spin * Math.PI) / 180, c = Math.cos(r), s = Math.sin(r);
	return [k * (U[0] * c + V[0] * s), k * (U[1] * c + V[1] * s), k * (-U[0] * s + V[0] * c), k * (-U[1] * s + V[1] * c)];
}

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

const MARKUP = `
<div class="lib-room" data-el="room">
  <div class="lib-stage" data-el="stage">
    <div class="lib-console" data-el="console" aria-hidden="true">
      <img class="lib-photo" src="${PHOTO_URL}" alt="" width="1100" height="662" draggable="false">
      <div class="lib-hole"></div>
      <div class="lib-led lib-led-power" data-el="ledPower"></div>
    </div>
    <div class="lib-tray-clip" aria-hidden="true">
      <div class="lib-tray-move" data-el="trayMove">
        <div class="lib-plate" data-el="plate"><div class="lib-well"></div><div class="lib-channel"></div></div>
        <div class="lib-plate-side" data-el="plateSide"></div>
      </div>
    </div>
    <div class="lib-panel" data-el="panel" aria-hidden="true"><img src="${PHOTO_URL}" alt="" width="1100" height="662" draggable="false"></div>
    <div class="lib-shelf" data-el="shelf" role="listbox" tabindex="0" aria-label="Discs" aria-orientation="horizontal"></div>
    <div class="lib-clean" data-el="clean" aria-hidden="true"></div>
  </div>
  <div class="lib-bar" data-el="bar" hidden>
    <p class="lib-count" data-el="count"></p>
    <button type="button" class="btn-quiet lib-folder-btn" data-el="folderBtn"><svg class="icon" aria-hidden="true" focusable="false"><use href="icons.svg#i-folder-open"/></svg><span>${COPY.another}</span></button>
  </div>
  <section class="lib-info" data-el="info">
    <h3 data-el="infoTitle"></h3>
    <p class="lib-meta" data-el="infoMeta"></p>
    <p class="lib-file" data-el="infoFile"></p>
    <p class="lib-hint"><kbd><svg class="icon" aria-hidden="true" focusable="false"><use href="icons.svg#i-arrow-left"/></svg><span class="vh">Left arrow</span></kbd><kbd><svg class="icon" aria-hidden="true" focusable="false"><use href="icons.svg#i-arrow-right"/></svg><span class="vh">Right arrow</span></kbd> browse <span class="lib-sep">·</span> <kbd>Enter</kbd> puts it in</p>
    <p class="lib-error" data-el="note" role="alert" hidden></p>
  </section>
  <section class="lib-setup" data-el="setup" aria-labelledby="lib-setup-title">
    <h3 class="vh" id="lib-setup-title" data-el="setupTitle" tabindex="-1">${COPY.setup}</h3>
    <div class="lib-steps">
      <div class="lib-step" data-el="biosCard">
        <span class="lib-step-icon" aria-hidden="true"><svg class="icon lib-step-todo" focusable="false"><use href="icons.svg#i-cpu"/></svg><svg class="icon lib-step-done" focusable="false"><use href="icons.svg#i-check-circle"/></svg></span>
        <h4 class="lib-step-title" data-el="biosTitle" tabindex="-1">${COPY.biosTitle}</h4>
        <p class="lib-step-sub" data-el="biosSub">${COPY.biosBody}</p>
        <div class="lib-step-actions">
          <button type="button" class="btn-primary" data-el="biosBtn">${COPY.biosChoose}</button>
          <span class="lib-step-aside" data-el="biosAside"></span>
        </div>
        <p class="lib-step-error" data-el="biosError" role="alert" hidden></p>
      </div>
      <div class="lib-step" data-el="intake">
        <span class="lib-step-icon" aria-hidden="true"><svg class="icon" focusable="false"><use href="icons.svg#i-folder-open"/></svg></span>
        <h4 class="lib-step-title" data-el="intakeTitle" tabindex="-1">${COPY.intakeTitle}</h4>
        <p class="lib-step-sub" data-el="intakeBody">${COPY.intakeBody}</p>
        <div class="lib-step-actions">
          <button type="button" class="btn-primary" data-el="intakeBtn"><span data-el="intakeLabel">${COPY.choose}</span></button>
          <button type="button" class="btn-quiet" data-el="intakeOther" hidden>${COPY.another}</button>
          <span class="lib-step-aside" data-el="intakeAside"></span>
        </div>
        <p class="lib-step-error" data-el="intakeError" hidden></p>
      </div>
    </div>
    <p class="lib-setup-fine">${esc(COPY.setupFine)}</p>
  </section>
  <section class="lib-card lib-card-dark" data-el="started" hidden>
    <h3 data-el="startedTitle" tabindex="-1"></h3>
    <p class="lib-body" data-el="startedBody"></p>
    <div class="lib-actions" data-el="startedActions">
      <button type="button" class="btn-primary" data-el="eject"><svg class="icon" aria-hidden="true" focusable="false"><use href="icons.svg#i-eject"/></svg>Eject<kbd class="lib-cap" aria-hidden="true">Esc</kbd></button>
    </div>
  </section>
  <p class="vh" data-el="announce" role="status"></p>
  <input type="file" data-el="input" webkitdirectory multiple hidden>
</div>`;

export function createDiscLibrary(root, { host = null, onBooted = () => {}, cleanChance = 0.1 } = {}) {
	root.classList.add('lib');
	root.innerHTML = MARKUP;
	const els = {};
	for (const el of root.querySelectorAll('[data-el]'))
		els[el.dataset.el] = el;
	const data = root.dataset;
	data.mode = 'empty';
	data.lights = 'up';
	data.tray = 'closed';
	data.disc = '';
	data.drag = '';

	let G = LAYOUTS.wide;
	let mode = 'empty'; // empty | scanning | shelf | inserting | inserted | ejecting
	let discs = [];
	let pos = 0;
	let target = 0;
	let current = null;
	let trayBusy = false;
	let folder = null; // the remembered folder handle (Chromium), if any
	let k = 1;

	const onStage = (p) => add(p, G.photo);

	// Three quick clicks (or Enter, Space, Esc) skip whatever is moving: the insert, the cleaning, the
	// eject, and the wait in the dark on the player, whose boot then shows at once. Every pause and
	// animation of the scene goes through dur() and wait(), so a skip finishes them all.
	let skipping = false;
	const timers = new Set();
	const dur = (t) => (skipping || reducedMotion.matches ? 0 : t);
	const wait = (t) => new Promise((resolve) => {
		if (skipping || t <= 0) {
			resolve();
			return;
		}
		const entry = { resolve };
		entry.id = setTimeout(() => {
			timers.delete(entry);
			resolve();
		}, t);
		timers.add(entry);
	});
	function skip() {
		if (skipping || !['inserting', 'inserted', 'ejecting'].includes(mode))
			return;
		skipping = true;
		for (const e of timers) {
			clearTimeout(e.id);
			e.resolve();
		}
		timers.clear();
		for (const a of root.getAnimations({ subtree: true }))
			a.finish();
	}
	const presses = [];
	function pressToSkip() {
		const now = performance.now();
		presses.push(now);
		while (presses.length && now - presses[0] > 900)
			presses.shift();
		if (presses.length >= 3) {
			presses.length = 0;
			skip();
		}
	}

	// ------------------------------------------------------------------ layout

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

	const trayMoveT = (open) => (open ? 'translate(0px, 0px)' : `translate(${-TRAY.travel[0]}px, ${-TRAY.travel[1]}px)`);
	const panelT = (open) => (open ? `translate(${TRAY.travel[0]}px, ${TRAY.travel[1]}px)` : 'translate(0px, 0px)');
	function setTray(open) {
		els.trayMove.style.transform = trayMoveT(open);
		els.panel.style.transform = panelT(open);
	}

	function applyLayout() {
		const room = els.room.getBoundingClientRect();
		if (room.width < 1 || room.height < 1)
			return; // hidden (a closed dialog)
		G = room.width / room.height < 0.95 ? LAYOUTS.tall : LAYOUTS.wide;
		k = Math.min(room.width / G.w, room.height / G.h);
		const s = els.stage.style;
		s.setProperty('--stage-w', `${G.w}px`);
		s.setProperty('--stage-h', `${G.h}px`);
		s.setProperty('--k', k);
		s.setProperty('--disc', `${DISC_PX}px`);
		for (const el of [els.console, els.panel]) {
			el.style.left = `${G.photo[0]}px`;
			el.style.top = `${G.photo[1]}px`;
		}
		const corners = poly([TRAY.tl, TRAY.tr, TRAY.br, TRAY.bl]);
		s.setProperty('--hole', corners);
		s.setProperty('--panel', corners);

		// What is in front of the console's face: the side of the tray's top edge the tray opens to.
		const tl = onStage(TRAY.tl), tr = onStage(TRAY.tr);
		const u = mul(sub(tr, tl), 1 / len(sub(tr, tl)));
		let n = [-u[1], u[0]];
		if (n[0] * TRAY.travel[0] + n[1] * TRAY.travel[1] < 0)
			n = mul(n, -1);
		const a = sub(tl, mul(u, 4000)), b = add(tr, mul(u, 4000));
		s.setProperty('--outside', poly([a, b, add(b, mul(n, 4000)), add(a, mul(n, 4000))]));

		// The plate, and its right-hand side wall, in millimetres on the tray's plane.
		const { U, V } = trayAxes();
		els.plate.style.width = `${TRAY.widthMm}px`;
		els.plate.style.height = `${TRAY.travelMm}px`;
		els.plate.style.transform = matrix([U[0], U[1], V[0], V[1], tl[0], tl[1]]);
		els.plateSide.style.width = `${TRAY.travelMm}px`;
		els.plateSide.style.height = `${TRAY.sideH}px`;
		els.plateSide.style.transform = matrix([V[0], V[1], 0, 1, tr[0], tr[1]]);
		if (!trayBusy)
			setTray(data.tray === 'open');

		const top = room.height / 2 + (G.infoY - G.h / 2) * k;
		els.info.style.setProperty('--info-top', `${Math.round(top)}px`);
		els.setup.style.setProperty('--card-top', `${Math.round(room.height * G.cardY)}px`);
		placeDiscs();
		if (current && (mode === 'inserted' || mode === 'inserting'))
			current.el.style.transform = matrix([...trayLinear(current.spinIn), ...trayCentre(true)]); // the tray moves it
	}

	// ------------------------------------------------------------------ state

	// Without a BIOS the library asks for one first: the folder and the shelf wait.
	const needBios = () => !!host && !!host.bios && !host.bios().ok;

	function setMode(m) {
		mode = m;
		data.mode = m;
		const bios = needBios();
		data.bios = bios ? 'none' : '';
		// The setup panel: the BIOS (done once there is one), then the games' folder (waiting until then).
		els.setup.hidden = !(bios || m === 'empty' || m === 'scanning');
		els.biosCard.dataset.done = String(!bios);
		els.intake.classList.toggle('is-waiting', bios);
		els.intakeBtn.disabled = bios;
		els.intakeOther.disabled = bios;
		els.started.hidden = m !== 'inserted';
		els.bar.hidden = !(discs.length && m === 'shelf');
		els.count.hidden = !discs.length;
	}

	function announce(text) {
		els.announce.textContent = '';
		requestAnimationFrame(() => (els.announce.textContent = text));
	}

	function note(text) {
		els.note.textContent = text;
		els.note.hidden = !text;
	}

	// ------------------------------------------------------------------ the shelf

	function buildDiscs() {
		els.shelf.replaceChildren();
		discs.forEach((d, i) => {
			const el = document.createElement('div');
			el.className = 'lib-disc';
			el.id = `${root.id || 'lib'}-disc-${i}`;
			el.setAttribute('role', 'option');
			el.setAttribute('aria-selected', 'false');
			el.setAttribute('aria-label', [d.title, d.serial, d.region].filter(Boolean).join(', '));
			el.dataset.index = i;
			el.style.visibility = 'hidden';
			d.el = el;
			d.built = false;
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
			const e = reducedMotion.matches ? 1 : 1 - Math.exp(-dt / 85);
			pos += (target - pos) * e;
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
		note('');
		kick();
	}

	function renderInfo(d) {
		els.infoTitle.textContent = d.title;
		els.infoMeta.textContent = discMeta(d);
		els.infoFile.textContent = d.fileName;
	}

	// ------------------------------------------------------------------ reading a folder

	async function scan(files, handles = null) {
		if (mode !== 'empty' && mode !== 'shelf' && mode !== 'scanning')
			return;
		els.intakeError.hidden = true;
		const previous = mode;
		setMode('scanning');
		const result = await readFolder(files, (done, total) => {
			els.intakeAside.textContent = COPY.reading(done, total);
		}, handles);
		if (!result.discs.length) {
			els.intakeAside.textContent = COPY.drop;
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
		els.intakeAside.textContent = COPY.drop;
		setMode('shelf');
		pos = -Math.min(discs.length, G.span); // they roll in from the left
		select(Math.floor((discs.length - 1) / 2));
		els.shelf.focus({ preventScroll: true });
		announce(shelfAnnouncement(discs.length, compressed));
	}

	async function readDirectory(dir) {
		setMode('scanning');
		els.intakeAside.textContent = COPY.reading(0, 0);
		let got;
		try {
			got = await filesFromDirectory(dir);
		} catch {
			setMode('empty');
			els.intakeAside.textContent = COPY.drop;
			els.intakeError.textContent = COPY.unreadable;
			els.intakeError.hidden = false;
			return;
		}
		setMode('empty');
		await scan(got.files, got.handles);
	}

	// The folder picker where the browser has one (Chromium; the folder is remembered for the next
	// visit), otherwise the plain input.
	async function chooseFolder() {
		if (!canPickFolder()) {
			els.input.click();
			return;
		}
		let dir;
		try {
			dir = await pickFolder();
		} catch {
			els.input.click();
			return;
		}
		if (!dir)
			return;
		folder = dir;
		rememberFolder(dir);
		await readDirectory(dir);
	}

	// A folder remembered from an earlier visit: read at once when the browser still allows it,
	// otherwise the intake offers it back (one press, the browser's question).
	async function restoreFolder() {
		if (!canPickFolder() || mode !== 'empty')
			return;
		const dir = await rememberedFolder();
		if (!dir || mode !== 'empty')
			return;
		folder = dir;
		if (await folderPermission(dir, false) === 'granted') {
			await readDirectory(dir);
			return;
		}
		els.intakeTitle.textContent = COPY.againTitle(dir.name);
		els.intakeBody.textContent = COPY.againBody(dir.name);
		els.intakeLabel.textContent = COPY.again(dir.name);
		els.intakeOther.hidden = false;
	}

	async function openRemembered() {
		if (!folder || await folderPermission(folder, true) !== 'granted') {
			chooseFolder();
			return;
		}
		await readDirectory(folder);
	}

	// ------------------------------------------------------------------ the insert

	function moveTray(open, duration, easing) {
		trayBusy = true;
		data.tray = 'moving';
		const opts = { duration, easing };
		return Promise.all([
			animate(els.trayMove, [{ transform: trayMoveT(!open) }, { transform: trayMoveT(open) }], opts),
			animate(els.panel, [{ transform: panelT(!open) }, { transform: panelT(open) }], opts),
		]).then(() => {
			trayBusy = false;
			data.tray = open ? 'open' : 'closed';
		});
	}

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
			const r = (spin * Math.PI) / 180;
			const up = G.scale;
			const shelfLin = [up * Math.cos(r), up * Math.sin(r), -up * Math.sin(r), up * Math.cos(r)];
			frames.push({ transform: matrix([...shelfLin.map((v, j) => lerp(v, tray[j], flat)), ...c]), offset: t });
		}
		return frames;
	}


	async function insert({ clean = false } = {}) {
		if (mode !== 'shelf' || !discs.length)
			return;
		const d = discs[target];
		skipping = false;
		// Decided now: a disc that has to be cleaned first holds the player's boot until it is clean.
		const cleaning = !reducedMotion.matches && (clean || Math.random() < cleanChance);
		if (host) {
			const why = host.blocked();
			if (why) {
				note(why);
				return;
			}
			// Inside the press: the player starts its sound here, then opens the disc and boots it while
			// the disc goes in.
			host.insert(d.file, d.handle || null, cleaning);
		}
		note('');
		current = d;
		pos = target;
		placeDiscs();
		setMode('inserting');
		announce(`${d.title} goes in.`);
		for (const o of discs)
			if (o !== d)
				o.el.classList.add('is-away');

		d.el.style.transform = matrix(shelfPose(0, d.angle));
		const opening = moveTray(true, dur(700), 'cubic-bezier(.3,0,.15,1)');
		await wait(dur(180));
		d.el.classList.add('is-flying');
		const frames = flightFrames(d);
		await animate(d.el, frames, { duration: dur(1050) });
		await opening;
		// It lies in the tray now: from here it moves with the tray, behind the console's face.
		d.el.classList.remove('is-flying');
		d.el.style.transform = frames[frames.length - 1].transform;
		els.trayMove.append(d.el);
		await wait(dur(160));
		await moveTray(false, dur(760), 'cubic-bezier(.5,0,.25,1)');
		if (cleaning) {
			await cleanDisc(d);
			if (host)
				host.release(); // the clean disc reads: now the game starts
		}
		data.disc = 'in';
		if (host) {
			// The PS2 slides down out of the room at once, and the player comes in under it: its screen shows the
			// boot.
			await wait(dur(260));
			await sink();
			onBooted();
			return;
		}
		await wait(dur(520));
		data.lights = 'down';
		els.startedTitle.textContent = COPY.starting(d.title);
		els.startedBody.textContent = host ? '' : COPY.rehearsal;
		els.startedBody.hidden = !!host;
		els.startedActions.hidden = !!host;
		await wait(dur(900));
		if (mode !== 'inserting')
			return;
		setMode('inserted');
		els.startedTitle.focus({ preventScroll: true });
		announce(COPY.starting(d.title));
	}


	// The console slides down out of the room, and back up into it.
	function sink() {
		return animate(els.room, [{ translate: '0 0' }, { translate: '0 110%' }],
			{ duration: dur(1100), easing: 'cubic-bezier(.55, 0, .8, .25)' });
	}

	function rise() {
		els.room.style.translate = '0 110%';
		return animate(els.room, [{ translate: '0 110%' }, { translate: '0 0' }],
			{ duration: dur(1000), easing: 'cubic-bezier(.2, .7, .25, 1)' }).then(() => {
			els.room.style.translate = '';
		});
	}

	// The BIOS step: its error, the read under way, and on to the folder once there is a BIOS.
	let biosWas = null;
	function renderBios() {
		if (!host || !host.bios)
			return;
		const b = host.bios();
		els.biosError.textContent = b.error;
		els.biosError.hidden = !b.error;
		els.biosAside.textContent = b.reading ? COPY.biosReading : '';
		els.biosSub.textContent = b.ok && b.name ? b.name : COPY.biosBody;
		els.biosBtn.textContent = b.ok ? COPY.biosChange : COPY.biosChoose;
		els.biosBtn.className = b.ok ? 'btn-quiet' : 'btn-primary';
		if (biosWas !== b.ok) {
			const first = biosWas === null;
			biosWas = b.ok;
			setMode(mode);
			if (!first && b.ok && root.isConnected && root.closest('dialog[open]'))
				(mode === 'shelf' ? els.shelf : els.intakeBtn.hidden ? els.intakeTitle : els.intakeBtn).focus({ preventScroll: true });
		}
	}

	async function eject() {
		if (mode !== 'inserted' || !current || host)
			return;
		const d = current;
		skipping = false;
		setMode('ejecting');
		data.lights = 'up';
		data.disc = '';
		await wait(dur(300));
		await moveTray(true, dur(700), 'cubic-bezier(.3,0,.15,1)');
		await wait(dur(120));
		els.shelf.append(d.el);
		d.el.classList.add('is-flying');
		const frames = flightFrames(d).reverse().map((fr) => ({ ...fr, offset: 1 - fr.offset }));
		await animate(d.el, frames, { duration: dur(950) });
		d.el.classList.remove('is-flying');
		for (const o of discs)
			o.el.classList.remove('is-away');
		current = null;
		setMode('shelf');
		placeDiscs();
		renderInfo(d);
		els.shelf.focus({ preventScroll: true });
		announce(`${d.title} is back on the shelf.`);
		await moveTray(false, dur(620), 'cubic-bezier(.5,0,.25,1)');
	}

	// Back to the shelf at once, with the disc that went in back in its place (the player's dialog,
	// opened again after a boot).
	function reset() {
		if (mode !== 'inserted' && mode !== 'inserting' && mode !== 'ejecting')
			return;
		if (current) {
			els.shelf.append(current.el);
			current.el.classList.remove('is-flying');
		}
		for (const o of discs)
			o.el.classList.remove('is-away');
		current = null;
		els.room.style.translate = '';
		data.lights = 'up';
		data.disc = '';
		data.tray = 'closed';
		setTray(false);
		setMode('shelf');
		placeDiscs();
		if (discs[target])
			renderInfo(discs[target]);
	}

	// ------------------------------------------------------------------ the Easter egg: a disc that won't read
	// It goes in, the power light blinks, the tray opens again, and the disc comes up to the front, turns
	// over to its dusty data side, is sprayed and wiped from the centre out (the right way), turns back
	// and goes in again. About 12 s; on the player the boot goes on underneath all the while.

	const smooth = (a, b, x) => {
		const t = Math.max(0, Math.min(1, (x - a) / (b - a)));
		return t * t * (3 - 2 * t);
	};

	// From lying in the open tray to upright at the front of the room (the bench), as frames.
	function benchFrames(d) {
		const from = trayCentre(true);
		const [bx, by, bs] = G.bench;
		const ctrl = [lerp(from[0], bx, 0.5), Math.min(from[1], by) - 170];
		const frames = [];
		const N = 30;
		for (let i = 0; i <= N; i++) {
			const t = i / N;
			const u = easeInOut(t);
			const c = [
				(1 - u) ** 2 * from[0] + 2 * (1 - u) * u * ctrl[0] + u * u * bx,
				(1 - u) ** 2 * from[1] + 2 * (1 - u) * u * ctrl[1] + u * u * by,
			];
			const flat = 1 - smooth(0.05, 0.7, u);
			const spin = d.spinIn * (1 - u);
			const scale = lerp(G.scale, G.scale * bs, u);
			const tray = trayLinear(spin);
			const r = (spin * Math.PI) / 180;
			const up = [scale * Math.cos(r), scale * Math.sin(r), -scale * Math.sin(r), scale * Math.cos(r)];
			frames.push({ transform: matrix([...up.map((v, j) => lerp(v, tray[j], flat)), ...c]), offset: t });
		}
		return frames;
	}

	// Turns the disc over: one face narrows to nothing, the other opens out.
	async function flip(from, to) {
		await from.animate([{ transform: 'scaleX(1)' }, { transform: 'scaleX(0)' }], { duration: dur(190), easing: 'cubic-bezier(.5,0,1,1)', fill: 'forwards' }).finished;
		from.style.visibility = 'hidden';
		to.getAnimations().forEach((a) => a.cancel());
		to.style.visibility = '';
		await to.animate([{ transform: 'scaleX(0)' }, { transform: 'scaleX(1)' }], { duration: dur(210), easing: 'cubic-bezier(0,0,.5,1)' }).finished;
	}

	function place(el, x, y, rotate = 0) {
		el.style.transform = `translate(${f(x)}px, ${f(y)}px) translate(-50%, -50%) rotate(${f(rotate)}deg)`;
	}

	async function spray(side, c, radius) {
		const bottle = document.createElement('div');
		bottle.className = 'lib-bottle';
		bottle.innerHTML = BOTTLE_SVG;
		const K = 1.4 * G.scale; // the drawing's units to the stage's
		bottle.style.width = `${BOTTLE_SIZE[0] * K}px`;
		bottle.style.height = `${BOTTLE_SIZE[1] * K}px`;
		els.clean.append(bottle);
		// The nozzle a little right of the disc, level with its upper half.
		const nozzle = [c[0] + radius * 1.3, c[1] - radius * 0.2];
		const left = nozzle[0] - NOZZLE[0] * K, top = nozzle[1] - NOZZLE[1] * K;
		const at = (dx) => `translate(${f(left + dx)}px, ${f(top)}px) rotate(-8deg)`;
		bottle.style.transformOrigin = `${(NOZZLE[0] + 54) * K}px ${(NOZZLE[1] + 70) * K}px`;
		await bottle.animate([{ transform: at(520), opacity: 0 }, { transform: at(0), opacity: 1 }], { duration: dur(520), easing: 'cubic-bezier(.2,0,0,1)', fill: 'forwards' }).finished;
		const shuffled = side.drops.slice().sort(() => Math.random() - 0.5);
		for (let pass = 0; pass < 2; pass++) {
			// The trigger, then the mist.
			bottle.animate([{ transform: at(0) }, { transform: `${at(0)} rotate(-5deg)` }, { transform: at(0) }], { duration: dur(220), easing: 'ease-out' });
			const mist = [];
			for (let i = 0; i < 30; i++) {
				const p = document.createElement('div');
				p.className = 'lib-mist';
				els.clean.append(p);
				const ang = Math.PI + (Math.random() - 0.5) * 0.75 + 0.18;
				const dist = radius * (0.7 + Math.random() * 1.3);
				const size = 3 + Math.random() * 7;
				p.style.width = p.style.height = `${f(size)}px`;
				mist.push(p.animate([
					{ transform: `translate(${f(nozzle[0])}px, ${f(nozzle[1])}px) scale(0.6)`, opacity: 0.95 },
					{ transform: `translate(${f(nozzle[0] + Math.cos(ang) * dist)}px, ${f(nozzle[1] + Math.sin(ang) * dist)}px) scale(2.6)`, opacity: 0 },
				], { duration: dur(520) + Math.random() * 260, delay: Math.random() * 120, easing: 'cubic-bezier(.1,.6,.3,1)' }).finished.then(() => p.remove()));
			}
			const half = shuffled.slice(pass * 13, pass * 13 + 13);
			for (const [drop] of half)
				drop.animate([{ opacity: 0 }, { opacity: 1 }], { duration: dur(260), delay: dur(200 + Math.random() * 260), fill: 'forwards' });
			await Promise.all(mist);
			await wait(120);
		}
		await bottle.animate([{ transform: at(0), opacity: 1 }, { transform: at(520), opacity: 0 }], { duration: dur(420), easing: 'cubic-bezier(.4,0,1,1)', fill: 'forwards' }).finished;
		bottle.remove();
	}

	async function wipe(side, c, radius) {
		const cloth = document.createElement('div');
		cloth.className = 'lib-cloth';
		cloth.innerHTML = CLOTH_SVG;
		cloth.style.width = `${CLOTH_SIZE[0] * 1.35 * G.scale}px`;
		cloth.style.height = `${CLOTH_SIZE[1] * 1.35 * G.scale}px`;
		els.clean.append(cloth);
		const strokes = 6;
		const start = Math.random() * 360;
		const angles = Array.from({ length: strokes }, (_, k) => start + k * (360 / strokes));
		// Each speck of dirt and drop goes with the stroke nearest to it.
		const owner = (a) => {
			let best = 0, bestD = 999;
			angles.forEach((s, k) => {
				const dd = Math.abs((((a - s) % 360) + 540) % 360 - 180);
				if (dd < bestD) {
					bestD = dd;
					best = k;
				}
			});
			return best;
		};
		const groups = angles.map(() => []);
		for (const [node, a] of [...side.dirt, ...side.drops])
			groups[owner(a)].push(node);
		const pos = (a, k) => [c[0] + Math.cos((a * Math.PI) / 180) * radius * k, c[1] + Math.sin((a * Math.PI) / 180) * radius * k];
		const enter = pos(angles[0], 0.3);
		await cloth.animate([
			{ transform: `translate(${f(enter[0] - 300)}px, ${f(enter[1] + 380)}px) translate(-50%, -50%) rotate(-30deg)`, opacity: 0 },
			{ transform: `translate(${f(enter[0])}px, ${f(enter[1])}px) translate(-50%, -50%) rotate(${f(angles[0] + 90)}deg)`, opacity: 1 },
		], { duration: dur(480), easing: 'cubic-bezier(.2,0,0,1)', fill: 'forwards' }).finished;
		for (let k = 0; k < strokes; k++) {
			const a = angles[k];
			const from = pos(a, 0.3), to = pos(a, 1.05);
			const rot = a + 90;
			const going = cloth.animate([
				{ transform: `translate(${f(from[0])}px, ${f(from[1])}px) translate(-50%, -50%) rotate(${f(rot)}deg)` },
				{ transform: `translate(${f(to[0])}px, ${f(to[1])}px) translate(-50%, -50%) rotate(${f(rot)}deg)` },
			], { duration: dur(300), easing: 'cubic-bezier(.4,0,.2,1)', fill: 'forwards' });
			for (const node of groups[k])
				node.animate([{ opacity: getComputedStyle(node).opacity }, { opacity: 0 }], { duration: dur(240), delay: dur(60), fill: 'forwards' });
			await going.finished;
			if (k + 1 < strokes) {
				const next = pos(angles[k + 1], 0.3);
				await cloth.animate([
					{ transform: `translate(${f(to[0])}px, ${f(to[1])}px) translate(-50%, -50%) rotate(${f(rot)}deg)` },
					{ transform: `translate(${f(next[0])}px, ${f(next[1])}px) translate(-50%, -50%) rotate(${f(angles[k + 1] + 90)}deg)` },
				], { duration: dur(150), easing: 'ease-in-out', fill: 'forwards' }).finished;
			}
		}
		const last = pos(angles[strokes - 1], 1.05);
		await cloth.animate([
			{ transform: `translate(${f(last[0])}px, ${f(last[1])}px) translate(-50%, -50%) rotate(${f(angles[strokes - 1] + 90)}deg)`, opacity: 1 },
			{ transform: `translate(${f(last[0] - 260)}px, ${f(last[1] + 360)}px) translate(-50%, -50%) rotate(-30deg)`, opacity: 0 },
		], { duration: dur(420), easing: 'cubic-bezier(.4,0,1,1)', fill: 'forwards' }).finished;
		cloth.remove();
	}

	async function cleanDisc(d) {
		announce('The disc won’t read. It needs a clean.');
		await wait(300);
		// The power light comes on and blinks: nothing reads.
		data.disc = 'in';
		await els.ledPower.animate([{ opacity: 1 }, { opacity: 0.1 }, { opacity: 1 }, { opacity: 0.1 }, { opacity: 1 }, { opacity: 0.1 }, { opacity: 1 }],
			{ duration: dur(1100) }).finished;
		data.disc = '';
		await wait(150);
		await moveTray(true, dur(700), 'cubic-bezier(.3,0,.15,1)');
		await wait(120);

		// Out of the tray and up to the front.
		els.shelf.append(d.el);
		d.el.classList.add('is-flying');
		const up = benchFrames(d);
		await animate(d.el, up, { duration: dur(950) });

		// Turned over: the data side, with what years on a shelf leave on it.
		const label = d.el.querySelector('.disc-svg');
		const side = makeDataSide(hash(d.fileName));
		side.el.style.visibility = 'hidden';
		d.el.append(side.el);
		await wait(150);
		await flip(label, side.el);
		await wait(350);

		const [bx, by, bs] = G.bench;
		const radius = (DISC_PX / 2) * G.scale * bs;
		await spray(side, [bx, by], radius);
		await wait(150);
		await wipe(side, [bx, by], radius);
		// Clean: it catches the light.
		await side.glint.animate([{ transform: 'translateX(-120%) rotate(20deg)' }, { transform: 'translateX(120%) rotate(20deg)' }],
			{ duration: dur(650), easing: 'ease-in-out' }).finished;
		await wait(200);

		// Back over, and back in.
		await flip(side.el, label);
		label.getAnimations().forEach((a) => a.cancel());
		label.style.visibility = '';
		side.el.remove();
		await wait(200);
		await animate(d.el, up.slice().reverse().map((fr) => ({ ...fr, offset: 1 - fr.offset })), { duration: dur(900) });
		d.el.classList.remove('is-flying');
		els.trayMove.append(d.el);
		await wait(160);
		await moveTray(false, dur(760), 'cubic-bezier(.5,0,.25,1)');
		announce('Clean. Second try.');
	}

	// ------------------------------------------------------------------ input

	els.intakeBtn.addEventListener('click', () => (folder && els.intakeOther.hidden === false ? openRemembered() : chooseFolder()));
	els.intakeOther.addEventListener('click', chooseFolder);
	els.folderBtn.addEventListener('click', chooseFolder);
	els.input.addEventListener('change', () => {
		const files = els.input.files;
		if (files && files.length)
			scan(files);
		els.input.value = '';
	});
	els.eject.addEventListener('click', eject);
	root.addEventListener('pointerdown', () => {
		if (mode === 'inserting' || mode === 'inserted' || mode === 'ejecting')
			pressToSkip();
	});
	root.addEventListener('keydown', (e) => {
		if ((mode === 'inserting' || mode === 'inserted' || mode === 'ejecting') && ['Enter', ' ', 'Escape'].includes(e.key) && !e.repeat)
			pressToSkip();
	});

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
			insert({ clean: e.shiftKey }); // Shift: the cleaning, every time (for trying it)
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

	root.addEventListener('keydown', (e) => {
		if (e.key === 'Escape' && mode === 'inserted' && !host) {
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
		dragging = { x: e.clientX, pos, moved: false, id: e.pointerId, samples: [[performance.now(), e.clientX]], disc: e.target.closest('.lib-disc') };
		els.room.setPointerCapture(e.pointerId);
		root.classList.add('dragging');
	});
	els.room.addEventListener('pointermove', (e) => {
		if (!dragging || e.pointerId !== dragging.id)
			return;
		const dx = e.clientX - dragging.x;
		if (Math.abs(dx) > 6)
			dragging.moved = true;
		if (!dragging.moved)
			return;
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
		root.classList.remove('dragging');
		if (!drag.moved) {
			if (drag.disc) {
				const i = +drag.disc.dataset.index;
				if (i === target)
					insert({ clean: e.shiftKey }); // Shift: the cleaning, every time (for trying it)
				else
					select(i);
			}
			return;
		}
		const [t0, x0] = drag.samples[0];
		const [t1, x1] = drag.samples[drag.samples.length - 1];
		const v = t1 > t0 ? (x1 - x0) / (t1 - t0) : 0; // px per ms
		const fling = (-v * 220) / (G.gap1 * k);
		select(Math.round(pos + Math.max(-6, Math.min(6, fling))));
	}
	els.room.addEventListener('pointerup', endDrag);
	els.room.addEventListener('pointercancel', endDrag);

	// Drop a folder (or disc images) on the library. These events stop here: the player's own drop
	// handling (a BIOS or a single disc) is for its page, not for the library.
	let dragDepth = 0;
	const filesIn = (e) => [...(e.dataTransfer?.types || [])].includes('Files');
	root.addEventListener('dragenter', (e) => {
		if (!filesIn(e))
			return;
		e.stopPropagation();
		e.preventDefault();
		dragDepth++;
		data.drag = needBios() || mode === 'empty' || mode === 'shelf' ? 'ok' : '';
	});
	root.addEventListener('dragleave', (e) => {
		if (!filesIn(e))
			return;
		e.stopPropagation();
		if (--dragDepth <= 0) {
			dragDepth = 0;
			data.drag = '';
		}
	});
	root.addEventListener('dragover', (e) => {
		if (!filesIn(e))
			return;
		e.stopPropagation();
		e.preventDefault();
	});
	root.addEventListener('drop', async (e) => {
		if (!filesIn(e))
			return;
		e.stopPropagation();
		e.preventDefault();
		dragDepth = 0;
		data.drag = '';
		if (needBios()) { // the BIOS file, first
			const file = e.dataTransfer.files && e.dataTransfer.files[0];
			if (file)
				host.takeBios(file);
			return;
		}
		if (mode !== 'empty' && mode !== 'shelf')
			return;
		const files = await filesFromDrop(e.dataTransfer);
		if (files.length)
			scan(files);
	});

	els.biosBtn.addEventListener('click', () => host.chooseBios());
	new ResizeObserver(applyLayout).observe(els.room);
	applyLayout();
	setMode('empty');
	restoreFolder();
	if (host && host.bios) {
		renderBios();
		setInterval(renderBios, 300);
	}

	return {
		// The library has just been shown (the dialog opened): lay it out and put focus in it.
		shown() {
			applyLayout();
			renderBios();
			if (needBios())
				els.biosBtn.focus({ preventScroll: true });
			else if (mode === 'shelf')
				els.shelf.focus({ preventScroll: true });
			else
				(els.intakeBtn.hidden ? els.intakeTitle : els.intakeBtn).focus({ preventScroll: true });
		},
		reset,
		rise,
		// A gamepad's press, read by app.js: { step: -1 | 1 } moves along the shelf like the arrows, { confirm: true }
		// puts the disc in front in like Enter, and counts as Enter for skipping while one goes in.
		pad({ step = 0, confirm = false }) {
			if (mode === 'shelf') {
				if (step)
					select(target + step);
				else if (confirm)
					insert();
			} else if (confirm && (mode === 'inserting' || mode === 'inserted' || mode === 'ejecting')) {
				pressToSkip();
			}
		},
		// Whether the insert is under way: the dialog then stays until the boot is covered.
		get busy() {
			return mode === 'inserting' || mode === 'inserted' || mode === 'ejecting';
		},
	};
}
