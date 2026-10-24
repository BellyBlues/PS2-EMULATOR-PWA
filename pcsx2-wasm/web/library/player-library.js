// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The player's disc library (UI-SPEC.md §15), the page's home: it opens by itself on load (asking for the
// BIOS first when there is none), and "Disc library…" on the slate and in the disc menu, Eject and the empty
// tray open it again. It is a dialog over the page with the shelf and the PS2. Putting a disc in chooses it and
// queues its boot in app.js (window.PCSX2WebLibrary) inside the same press; the insert then plays, the room goes
// dark, the PS2 slides down out of it once it starts, and the dialog fades away once the game's first frame is
// up, so the boot itself is never seen and the player comes in under it. When the game stops (Reset, or Eject,
// which stops it), the dialog fades back in over the player and the PS2 slides back up.

import { createDiscLibrary } from './disc-library.js';

const host = window.PCSX2WebLibrary;
const dialog = document.getElementById('library');
const root = document.getElementById('library-root');
const closeBtn = document.getElementById('library-close');
const openers = [document.getElementById('slate-library'), document.getElementById('disc-menu-library')];
// On the console's front (UI-SPEC §16): Eject, and the empty tray.
const consoleOpeners = [document.getElementById('btn-eject'), document.getElementById('tray-library')];
const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');

let lib = null;
let leaving = false;

// rise: back from a game: the dialog fades in over the player and the PS2 slides up into it.
function open({ rise = false } = {}) {
	if (!host || !host.canOpen() || dialog.open)
		return;
	if (!lib)
		// ?clean=1: every disc has to be cleaned first (the Easter egg, for trying it).
		lib = createDiscLibrary(root, { host, onBooted: leave, cleanChance: new URLSearchParams(location.search).get('clean') === '1' ? 1 : 0.1 });
	lib.reset();
	dialog.classList.remove('is-dark');
	dialog.classList.toggle('is-leaving', rise && !reducedMotion.matches);
	dialog.showModal();
	lib.shown();
	if (rise && !reducedMotion.matches) {
		requestAnimationFrame(() => requestAnimationFrame(() => dialog.classList.remove('is-leaving')));
		lib.rise();
	}
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
	}, reducedMotion.matches ? 0 : 700);
}

if (host && dialog && root) {
	for (const b of openers) {
		b?.addEventListener('click', () => {
			if (b.getAttribute('aria-disabled') !== 'true')
				open();
		});
	}
	for (const b of consoleOpeners) {
		b?.addEventListener('click', () => {
			const view = host.status().view;
			if (host.canOpen())
				open();
			else if (view === 'running' || view === 'starting')
				host.stop(); // the library comes back once it has stopped (watch)
			else
				host.say(host.blocked() || 'Stop the PS2 first to change the disc.');
		});
	}
	// A gamepad plays the shelf (app.js reads it while the dialog is open): D-pad or left stick to move, Cross to put in.
	host.onPad((action) => {
		if (lib && dialog.open)
			lib.pad(action);
	});
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

	// The library is the home: it opens by itself on load (with a BIOS or without: it asks for one), when the
	// BIOS goes away (Forget), and when a game stops, rising back; never over a boot the address asked for
	// (?bios=, ?disc=, ?elf=) and never over another dialog. Close (Esc) leaves the player's own slate.
	const params = new URLSearchParams(location.search);
	const addressBoots = ['bios', 'disc', 'elf'].some((k) => params.has(k));
	let last = null;
	let opened = false;
	const watch = () => {
		const view = host.status().view;
		const free = host.canOpen() && !dialog.open && !document.querySelector('dialog[open]') && document.visibilityState === 'visible';
		if (free && !addressBoots && (!opened ? ['nobios', 'ready'].includes(view) : view === 'nobios' && last !== 'nobios')) {
			opened = true;
			open();
		} else if (free && view === 'stopped' && (last === 'running' || last === 'starting')) {
			opened = true;
			open({ rise: true });
		}
		last = view;
	};
	setInterval(watch, 400);
	watch();
}
