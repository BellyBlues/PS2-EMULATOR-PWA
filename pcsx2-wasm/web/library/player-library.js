// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The player's disc library (UI-SPEC.md §15), the page's home: it opens by itself once a BIOS is ready
// (on load, or when one is chosen), and "Disc library…" on the slate and in the disc menu opens it
// again. It is a dialog over the page with the shelf and the PS2. Putting a disc in chooses it and queues its boot
// in app.js (window.PCSX2WebLibrary) inside the same press; the insert then plays while the PS2 starts,
// the room goes dark, and the dialog fades away once the game's first frame is up, so the boot itself
// is never seen.

import { createDiscLibrary } from './disc-library.js';

const host = window.PCSX2WebLibrary;
const dialog = document.getElementById('library');
const root = document.getElementById('library-root');
const closeBtn = document.getElementById('library-close');
const openers = [document.getElementById('slate-library'), document.getElementById('disc-menu-library')];
const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');

let lib = null;
let leaving = false;

function open() {
	if (!host || !host.canOpen() || dialog.open)
		return;
	if (!lib)
		// ?clean=1: every disc has to be cleaned first (the Easter egg, for trying it).
		lib = createDiscLibrary(root, { host, onBooted: leave, cleanChance: new URLSearchParams(location.search).get('clean') === '1' ? 1 : 0.1 });
	lib.reset();
	dialog.classList.remove('is-dark', 'is-leaving');
	dialog.showModal();
	lib.shown();
}

// The boot is covered (its first frame is up, or it failed and the slate has the reason): the dialog
// fades away over the picture and the page takes the keyboard.
function leave() {
	if (leaving || !dialog.open)
		return;
	leaving = true;
	dialog.classList.add('is-leaving');
	setTimeout(() => {
		dialog.close();
		dialog.classList.remove('is-dark', 'is-leaving');
		leaving = false;
		lib.reset();
		host.focusScreen();
	}, reducedMotion.matches ? 0 : 500);
}

if (host && dialog && root) {
	for (const b of openers) {
		b?.addEventListener('click', () => {
			if (b.getAttribute('aria-disabled') !== 'true')
				open();
		});
	}
	closeBtn.addEventListener('click', () => {
		if (!lib?.busy)
			dialog.close();
	});
	// Esc closes the library, except while a disc goes in: the boot it started is under way.
	dialog.addEventListener('cancel', (e) => {
		if (lib?.busy)
			e.preventDefault();
	});
	// Keys and drops in the library are the library's: the player's shortcuts, Esc handling and drop
	// target (window listeners in app.js) never see them.
	for (const type of ['keydown', 'keyup', 'dragenter', 'dragover', 'dragleave', 'drop'])
		dialog.addEventListener(type, (e) => e.stopPropagation());
	// The dialog's own head and credit fade with the room's lights.
	new MutationObserver(() => {
		dialog.classList.toggle('is-dark', root.dataset.lights === 'down');
	}).observe(root, { attributes: true, attributeFilter: ['data-lights'] });

	// The library is the home: it opens by itself when the page becomes ready to boot a disc (a BIOS
	// remembered or just chosen), once each time, never over a boot the address asked for (?bios=,
	// ?disc=, ?elf=) and never over another dialog. Close (Esc) leaves the player's own slate.
	const params = new URLSearchParams(location.search);
	const addressBoots = ['bios', 'disc', 'elf'].some((k) => params.has(k));
	let wasReady = false;
	const watch = () => {
		const ready = host.canOpen() && host.status().view === 'ready';
		if (ready && !wasReady && !addressBoots && !dialog.open && !document.querySelector('dialog[open]') &&
			document.visibilityState === 'visible')
			open();
		// Ready again only after the BIOS goes away (Forget) or a boot: not each time Close leaves it.
		if (!host.canOpen() || ['nobios', 'starting', 'running'].includes(host.status().view))
			wasReady = false;
		else if (ready)
			wasReady = true;
	};
	setInterval(watch, 400);
	watch();
}
