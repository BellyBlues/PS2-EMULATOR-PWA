// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Disc harness: the real disc worker (pcsx2-wasm/web/disc-worker.js) with test hooks around it, so the
// harness can provoke what a real run only does by accident. The worker itself is unchanged; these hooks
// reach it through what any script in its scope sees (its top-level functions and fetch).
//   {type: 'test-kill-loop'}             the serving loop's next pass throws, as an escaping exception would
//   {type: 'test-hold-fetch', range?}    fetches (of that Range header, e.g. 'bytes=0-0'; any if omitted)
//                                        wait until 'test-release-fetch'
//   {type: 'test-release-fetch', fail?, notFound?}
//                                        lets them go on, fails them as a lost connection would, or sends
//                                        them to a missing file (the server answers 404)
// Each test message is answered {type: 'test-reply', id, ok}.

'use strict';

importScripts('../../../pcsx2-wasm/web/disc-worker.js');

const realFetch = self.fetch.bind(self);
const workerOnMessage = self.onmessage;
let hold = null; // {range, waiting: [{resolve, reject}]}

self.fetch = (input, init) => {
	const range = init?.headers?.Range;
	if (!hold || (hold.range && hold.range !== range))
		return realFetch(input, init);
	return new Promise((resolve, reject) => {
		const signal = init?.signal;
		const entry = {
			go: () => realFetch(input, init).then(resolve, reject),
			fail: () => reject(new TypeError('Failed to fetch (test)')),
			notFound: () => realFetch(`${input}.missing`, init).then(resolve, reject),
		};
		signal?.addEventListener('abort', () => reject(signal.reason || new DOMException('Aborted', 'AbortError')));
		hold.waiting.push(entry);
	});
};

self.onmessage = (event) => {
	const msg = event.data || {};
	if (typeof msg.type !== 'string' || !msg.type.startsWith('test-'))
		return workerOnMessage(event);
	switch (msg.type) {
	case 'test-kill-loop': {
		const realScan = self.scan;
		self.scan = async () => {
			self.scan = realScan;
			throw new Error('test: the serving loop was killed');
		};
		break;
	}
	case 'test-hold-fetch':
		hold = { range: msg.range || '', waiting: [] };
		break;
	case 'test-release-fetch': {
		const waiting = hold ? hold.waiting : [];
		hold = null;
		for (const w of waiting)
			(msg.fail ? w.fail : msg.notFound ? w.notFound : w.go)();
		break;
	}
	default:
		break;
	}
	self.postMessage({ type: 'test-reply', id: msg.id, ok: true });
};
