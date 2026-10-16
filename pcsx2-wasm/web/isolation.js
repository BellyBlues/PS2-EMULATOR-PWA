// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Cross-origin isolation on hosts that cannot send headers, such as GitHub Pages.
//
// The emulator's threads share memory (SharedArrayBuffer), which browsers only allow on a
// cross-origin isolated page. tools/wasm/serve.py sends the COOP/COEP headers itself. GitHub Pages
// cannot, so coi-serviceworker.js (MIT, loaded by index.html right after this file) registers
// itself as a service worker that adds the headers to every response from this site, then reloads
// the page once so that the worker serves it. On a page that is already isolated (the server sent
// the headers, or the worker served it) it does nothing. It caches nothing: files still come from
// the network and the HTTP cache, so updates behave as without it.
//
// This file configures it (window.coi) and tells app.js what is happening, in
// window.TabStationTooIsolation.status:
//   'isolated'     nothing to do
//   'pending'      the worker is being installed and the page will reload by itself
//   'unavailable'  the worker cannot help; .reason says why, and a 'tabstationtoo-isolation' event
//                  on window announces it when this happens after the page started
// It must run before coi-serviceworker.js and app.js, so it is loaded without defer or async.

(function () {
	'use strict';

	const RELOADS_KEY = 'tabstationtoo.isolationReloads';
	// One reload is the normal case. A second one covers a reload that raced the worker's
	// activation. Past that, stop instead of reloading forever.
	const MAX_RELOADS = 2;
	const RELOAD_WINDOW_MS = 60000;
	// The worker is a 3 KB script: when no reload has come by then, its registration failed.
	const GIVE_UP_MS = 15000;

	const state = { status: window.crossOriginIsolated ? 'isolated' : 'pending', reason: '' };
	window.TabStationTooIsolation = state;

	// sessionStorage counts this tab's reloads across them. Without it there is no loop guard.
	let storage = null;
	try {
		storage = window.sessionStorage;
		storage.getItem(RELOADS_KEY); // throws where site data is blocked
	} catch {
		storage = null;
	}

	function recentReloads() {
		try {
			const saved = JSON.parse(storage.getItem(RELOADS_KEY) || 'null');
			if (saved && Date.now() - saved.at < RELOAD_WINDOW_MS)
				return saved.count;
		} catch {
			// unreadable: start over
		}
		return 0;
	}

	function fail(reason) {
		if (state.status === 'unavailable')
			return;
		state.status = 'unavailable';
		state.reason = reason;
		window.dispatchEvent(new Event('tabstationtoo-isolation'));
	}

	window.coi = {
		// COEP require-corp, the policy serve.py sends. Every file is same-origin, so nothing needs
		// "credentialless", which Safari lacks and which this worker forgets whenever the browser
		// restarts it.
		coepCredentialless: () => false,
		// Only asked while the page is not isolated (crossOriginIsolated === false).
		shouldRegister: () => {
			if (!window.isSecureContext) {
				fail('insecure context');
				return false;
			}
			if (!navigator.serviceWorker) {
				fail('no service workers in this browser or window');
				return false;
			}
			if (!storage) {
				fail('site data is blocked');
				return false;
			}
			if (navigator.serviceWorker.controller) {
				fail('the service worker serves the page, but the page is still not isolated');
				return false;
			}
			if (recentReloads() >= MAX_RELOADS) {
				fail(`still not isolated after ${MAX_RELOADS} reloads`);
				return false;
			}
			return true;
		},
		doReload: () => {
			try {
				storage.setItem(RELOADS_KEY, JSON.stringify({ count: recentReloads() + 1, at: Date.now() }));
			} catch {
				fail('site data is blocked');
				return;
			}
			window.location.reload();
		},
	};

	if (window.crossOriginIsolated) {
		if (storage)
			storage.removeItem(RELOADS_KEY);
	} else if (window.crossOriginIsolated === false) {
		// Also covers coi-serviceworker.js failing to load or to register.
		setTimeout(() => fail('the service worker did not install'), GIVE_UP_MS);
	} else {
		fail('this browser has no cross-origin isolation');
	}
})();
