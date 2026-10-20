#!/usr/bin/env node
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Tests the players: the keyboard and gamepads going to PS2 port 1 or 2 (docs/web-port/UI-SPEC.md,
// "Keys drawer"). Boots a BIOS in headless Chrome, plays a mocked Gamepad API (navigator.getGamepads
// and the gamepadconnected event) and the keyboard, and reads what each port's pad holds through
// pcsx2web_test_pad_state. No real controller is involved: that stays a manual check. No dependencies:
// Node 22+ (global WebSocket), Python 3 (tools/wasm/serve.py) and Chrome. A BIOS comes from you.
//
//   node tools/wasm/pad-test.mjs --root build-wasm/bin --bios-dir DIR --bios FILE
//        [--port 8171] [--timeout 120] [--chrome PATH]
//
// Passes (exit 0) when every check holds. Chrome runs muted with a throwaway profile, removed at exit.

import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "../..");

function parseArgs(argv) {
	const guess = ["/usr/bin/google-chrome", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"].find(existsSync);
	const opts = { root: path.join(REPO, "build-wasm/bin"), biosDir: null, bios: null, port: 8171, timeout: 120,
		chrome: process.env.CHROME || guess || "google-chrome" };
	for (let i = 0; i < argv.length; i++) {
		const a = argv[i];
		if (a === "--root") opts.root = path.resolve(argv[++i]);
		else if (a === "--bios-dir") opts.biosDir = path.resolve(argv[++i]);
		else if (a === "--bios") opts.bios = argv[++i];
		else if (a === "--port") opts.port = Number(argv[++i]);
		else if (a === "--timeout") opts.timeout = Number(argv[++i]);
		else if (a === "--chrome") opts.chrome = argv[++i];
		else throw new Error(`unknown argument ${a}`);
	}
	if (!opts.biosDir || !opts.bios) throw new Error("--bios-dir and --bios are required");
	if (!existsSync(path.join(opts.root, "index.html"))) throw new Error("--root must hold a build (index.html)");
	return opts;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Runs in the page once the PS2 runs. Returns { name: ok } for every check.
const TEST = `(async () => {
	const wait = (ms) => new Promise((r) => setTimeout(r, ms));
	const NAMES = ['Up','Right','Down','Left','Triangle','Circle','Cross','Square','Select','Start','L1','L2','R1','R2','L3','R3','Analog','LUp','LRight','LDown','LLeft','RUp','RRight','RDown','RLeft'];
	const state = (port) => {
		const p = Module._pcsx2web_test_pad_state(port) >>> 0;
		const bytes = new Uint8Array(Module.HEAPU8.buffer, p, 32);
		const o = {};
		NAMES.forEach((n, i) => { if (bytes[i]) o[n] = bytes[i]; });
		o.connected = (new Uint32Array(Module.HEAPU8.buffer, p, 8)[7] & 1) === 1;
		return o;
	};
	const only = (s, expected) => {
		const got = Object.keys(s).filter((k) => k !== 'connected').sort().join();
		return got === Object.keys(expected).sort().join() && Object.entries(expected).every(([k, v]) => Math.abs(s[k] - v) <= 2);
	};
	const mk = (index, id) => ({ index, id, connected: true, mapping: 'standard',
		buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })), axes: [0, 0, 0, 0] });
	const set = (g, i, v) => { g.buttons[i] = { pressed: v > 0, touched: v > 0, value: v }; };
	const key = (type, code, k) => window.dispatchEvent(new KeyboardEvent(type, { code, key: k, bubbles: true, cancelable: true }));
	const r = {};

	const gps = [mk(0, 'Xbox 360 Controller (STANDARD GAMEPAD Vendor: 045e Product: 028e)')];
	navigator.getGamepads = () => gps.concat([null, null, null]).slice(0, 4);
	window.dispatchEvent(Object.assign(new Event('gamepadconnected'), { gamepad: gps[0] }));
	await wait(400);
	r['both ports have a connected DualShock 2'] = state(0).connected && state(1).connected;
	r['the gamepad is Player 1 and announced'] = /Xbox 360 Controller, Player 1/.test(document.getElementById('announce').textContent);

	set(gps[0], 0, 1); set(gps[0], 6, 0.6); gps[0].axes = [0.8, 0, 0, -1];
	await wait(500);
	r['gamepad: button, trigger pressure and sticks reach port 1'] = only(state(0), { Cross: 255, L2: 153, LRight: 198, RUp: 255 }) && only(state(1), {});
	set(gps[0], 0, 0); set(gps[0], 6, 0); gps[0].axes = [0, 0, 0, 0];
	await wait(500);
	r['gamepad: released'] = only(state(0), {});

	key('keydown', 'KeyK', 'k'); key('keydown', 'KeyW', 'w');
	await wait(400);
	r['keyboard plays Player 1 by default'] = only(state(0), { Cross: 255, LUp: 255 }) && only(state(1), {});
	set(gps[0], 1, 1);
	await wait(400);
	r['keyboard and gamepad share port 1'] = only(state(0), { Cross: 255, LUp: 255, Circle: 255 });
	key('keyup', 'KeyK', 'k'); key('keyup', 'KeyW', 'w'); set(gps[0], 1, 0);
	await wait(400);
	r['keyboard and gamepad released'] = only(state(0), {});

	gps.push(mk(1, '045e-028e-Second Pad'));
	await wait(400);
	r['a second gamepad is Player 2'] = /Second Pad, Player 2/.test(document.getElementById('announce').textContent);
	set(gps[0], 2, 1); set(gps[1], 1, 1);
	await wait(400);
	r['two gamepads, two players'] = only(state(0), { Square: 255 }) && only(state(1), { Circle: 255 });

	document.getElementById('btn-keys').click();
	await wait(300);
	document.getElementById('tab-gamepad').click();
	document.getElementById('gp-swap').click();
	await wait(500);
	r['swap lets go of what was held'] = only(state(0), {}) && only(state(1), {});
	set(gps[0], 2, 0); set(gps[1], 1, 0);
	await wait(200);
	set(gps[0], 2, 1); set(gps[1], 1, 1);
	await wait(400);
	r['swap exchanges the ports'] = only(state(0), { Circle: 255 }) && only(state(1), { Square: 255 });
	r['the first gamepad\\'s port is remembered'] = localStorage.getItem('pcsx2web.padFirst') === '2';
	set(gps[0], 2, 0); set(gps[1], 1, 0);
	await wait(300);

	document.querySelector('input[name="kb-player"][value="2"]').click();
	document.getElementById('screen').focus();
	await wait(100);
	key('keydown', 'KeyJ', 'j'); key('keydown', 'ArrowUp', 'ArrowUp');
	await wait(400);
	r['the keyboard can play Player 2'] = only(state(0), {}) && only(state(1), { Square: 255, Up: 255 });
	r['the keyboard\\'s port is remembered'] = localStorage.getItem('pcsx2web.padKeyboard') === '2';
	window.dispatchEvent(new Event('blur'));
	await wait(400);
	r['losing focus lets go of the keyboard'] = only(state(1), {});
	key('keyup', 'KeyJ', 'j'); key('keyup', 'ArrowUp', 'ArrowUp');

	set(gps[1], 3, 1);
	await wait(300);
	r['a gamepad on port 1 plays while the keyboard is Player 2'] = only(state(0), { Triangle: 255 });
	gps.pop();
	await wait(500);
	r['unplugging lets go'] = only(state(0), {}) && /Second Pad/.test(document.getElementById('announce').textContent) === true;
	// A gamepad that is not laid out in the standard way, like an 8BitDo Ultimate in D mode in Firefox:
	// a hat D-pad on axis 0 (rest 3.29), triggers on axes 5 and 6 (rest -1) and on buttons 8 and 9.
	gps.length = 0;
	await wait(500);
	const nm = mk(0, '2dc8-3013-8BitDo Ultimate wireless Controller for PC');
	nm.mapping = '';
	nm.buttons = Array.from({ length: 28 }, () => ({ pressed: false, touched: false, value: 0 }));
	nm.axes = [3.2857, 0, 0, 0, 0, -1, -1];
	gps.push(nm);
	window.dispatchEvent(Object.assign(new Event('gamepadconnected'), { gamepad: nm }));
	await wait(500);
	r['an unmapped gamepad is announced as needing a mapping'] = /map its controls/.test(document.getElementById('announce').textContent);
	set(nm, 3, 1);
	await wait(300);
	const both = () => { const a = state(0), b = state(1), o = { connected: true }; for (const k of NAMES) { if (a[k] || b[k]) o[k] = a[k] || b[k]; } return o; };
	r['an unmapped gamepad plays nothing'] = only(both(), {});
	set(nm, 3, 0);
	const startBtn = document.querySelector('#gp-list .gp-map-start');
	r['the Map controls button is shown'] = !!startBtn && !startBtn.closest('.gp-map').hidden;
	startBtn.click();
	const press = async (apply, release) => { await wait(450); apply(); await wait(450); release(); await wait(200); };
	const btn = (i) => [() => set(nm, i, 1), () => set(nm, i, 0)];
	const axis = (i, v, rest) => [() => { nm.axes[i] = v; }, () => { nm.axes[i] = rest; }];
	const both2 = (a, b) => [() => { a[0](); b[0](); }, () => { a[1](); b[1](); }];
	const run = [btn(3), btn(1), btn(4), btn(0), btn(6), btn(7),
		both2(axis(6, 1, -1), btn(8)), both2(axis(5, 1, -1), btn(9)), btn(10), btn(11),
		axis(0, -1, 3.2857), // D-pad Up on the hat: the other three directions are skipped
		btn(13), btn(14), axis(1, 1, 0), axis(2, -1, 0), axis(3, 1, 0), axis(4, -1, 0), btn(12)];
	for (const [apply, release] of run) await press(apply, release);
	await wait(300);
	const saved = JSON.parse(localStorage.getItem('pcsx2web.padMaps') || '{}');
	const map = Object.values(saved)[0] || {};
	r['the mapping is saved'] = !!map.buttons && map.buttons.Cross && map.buttons.Cross.i === 3 && map.buttons.Up && map.buttons.Up.k === 'h' && map.buttons.Left.k === 'h' &&
		map.buttons.L2 && map.buttons.L2.k === 'a' && map.buttons.L2.i === 6 && map.sticks.L.y.s === 1;
	r['mapping ended and says so'] = /is mapped/.test(document.getElementById('announce').textContent) && document.querySelector('#gp-list .gp-map-run').hidden;
	const check = async (label, apply, undo, expected) => { apply(); await wait(350); r[label] = only(both(), expected); undo(); await wait(250); };
	await check('mapped: bottom face button is Cross', () => set(nm, 3, 1), () => set(nm, 3, 0), { Cross: 255 });
	await check('mapped: Triangle, L1 and Start', () => { set(nm, 0, 1); set(nm, 6, 1); set(nm, 11, 1); }, () => { set(nm, 0, 0); set(nm, 6, 0); set(nm, 11, 0); }, { Triangle: 255, L1: 255, Start: 255 });
	await check('mapped: the hat is the D-pad (right)', () => { nm.axes[0] = -3 / 7; }, () => { nm.axes[0] = 3.2857; }, { Right: 255 });
	await check('mapped: the hat is the D-pad (down-left)', () => { nm.axes[0] = 3 / 7; }, () => { nm.axes[0] = 3.2857; }, { Down: 255, Left: 255 });
	await check('mapped: a trigger on an axis passes its pressure', () => { nm.axes[6] = 0; }, () => { nm.axes[6] = -1; }, { L2: 127 });
	await check('mapped: left stick right and right stick up', () => { nm.axes[1] = 1; nm.axes[4] = -1; }, () => { nm.axes[1] = 0; nm.axes[4] = 0; }, { LRight: 255, RUp: 255 });
	await check('mapped: left stick up and right stick right', () => { nm.axes[2] = -1; nm.axes[3] = 1; }, () => { nm.axes[2] = 0; nm.axes[3] = 0; }, { LUp: 255, RRight: 255 });
	await check('mapped: the center button is Analog', () => set(nm, 12, 1), () => set(nm, 12, 0), { Analog: 255 });
	await check('a trigger at rest plays nothing', () => {}, () => {}, {});
	document.querySelector('#gp-list .gp-map-clear').click();
	set(nm, 3, 1);
	await wait(350);
	r['clearing the mapping stops it playing'] = only(both(), {});
	set(nm, 3, 0);
	gps.length = 0;
	await wait(300);
	localStorage.removeItem('pcsx2web.padMaps');
	document.querySelector('input[name="kb-player"][value="1"]').click();
	localStorage.removeItem('pcsx2web.padFirst'); localStorage.removeItem('pcsx2web.padKeyboard');
	return r;
})()`;

async function main() {
	const opts = parseArgs(process.argv.slice(2));
	const work = mkdtempSync(path.join(tmpdir(), "tabstationtoo-pad-"));
	const children = [];
	const cleanup = () => {
		for (const c of children) if (c.exitCode === null && c.signalCode === null) c.kill("SIGKILL");
		rmSync(work, { recursive: true, force: true });
	};
	process.on("SIGINT", () => { cleanup(); process.exit(130); });
	let status = 1;
	try {
		const origin = `http://127.0.0.1:${opts.port}`;
		const server = spawn("python3", [path.join(REPO, "tools/wasm/serve.py"), "--port", String(opts.port), "--quiet",
			"--bios-dir", opts.biosDir, opts.root], { stdio: ["ignore", "ignore", "inherit"] });
		children.push(server);
		const chrome = spawn(opts.chrome, ["--headless=new", `--user-data-dir=${path.join(work, "profile")}`, "--remote-debugging-port=0",
			"--no-first-run", "--no-default-browser-check", "--disable-extensions", "--disable-background-networking",
			"--mute-audio", "--autoplay-policy=no-user-gesture-required", "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
		children.push(chrome);
		let stderr = "";
		const wsUrl = await new Promise((resolve, reject) => {
			const timer = setTimeout(() => reject(new Error("Chrome did not print its DevTools URL")), 15000);
			chrome.stderr.on("data", (d) => {
				stderr += d.toString();
				const m = stderr.match(/DevTools listening on (ws:\/\/\S+)/);
				if (m) { clearTimeout(timer); resolve(m[1]); }
			});
			chrome.on("exit", (code) => reject(new Error(`Chrome exited early (${code}): ${stderr}`)));
		});
		const ws = new WebSocket(wsUrl);
		await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
		let nextId = 1;
		const pending = new Map();
		const errors = [];
		ws.addEventListener("message", (ev) => {
			const msg = JSON.parse(ev.data);
			if (msg.id && pending.has(msg.id)) {
				const { resolve, reject } = pending.get(msg.id);
				pending.delete(msg.id);
				if (msg.error) reject(new Error(msg.error.message)); else resolve(msg.result);
			} else if (msg.method === "Runtime.exceptionThrown") {
				const p = msg.params;
				errors.push(`uncaught: ${p.exceptionDetails.exception?.description ?? p.exceptionDetails.text}`);
			}
		});
		const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
			const id = nextId++;
			pending.set(id, { resolve, reject });
			ws.send(JSON.stringify({ id, method, params, sessionId }));
		});
		for (let i = 0; i < 100; i++) {
			try { if ((await fetch(`${origin}/index.html`)).ok) break; } catch { /* not up yet */ }
			await sleep(100);
		}
		const { targetId } = await send("Target.createTarget", { url: "about:blank" });
		const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
		await send("Runtime.enable", {}, sessionId);
		await send("Page.navigate", { url: `${origin}/index.html?bios=dev-bios/${encodeURIComponent(opts.bios)}&renderer=sw&mute=1` }, sessionId);

		const deadline = Date.now() + opts.timeout * 1000;
		let running = false;
		while (Date.now() < deadline && !running) {
			await sleep(500);
			try {
				const r = await send("Runtime.evaluate", { expression: "document.body && document.body.dataset.state", returnByValue: true }, sessionId);
				const state = r.result.value;
				running = state === "running";
				if (state === "failed" || state === "dead" || state === "unsupported") throw new Error(`the page went to ${state}`);
			} catch (e) {
				if (/went to/.test(e.message)) throw e;
			}
		}
		if (!running) throw new Error("the PS2 did not start in time");
		await sleep(3000); // the BIOS needs a moment before its pads answer
		const r = await send("Runtime.evaluate", { expression: TEST, awaitPromise: true, returnByValue: true }, sessionId);
		if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
		const results = Object.entries(r.result.value);
		results.push(["no uncaught exceptions", errors.length === 0]);
		for (const [name, ok] of results) console.log(`  ${ok ? "pass" : "FAIL"}  ${name}`);
		for (const e of errors) console.log(`  error: ${e}`);
		status = results.every(([, ok]) => ok) ? 0 : 1;
		await send("Browser.close").catch(() => {});
		ws.close();
		await Promise.race([new Promise((res) => chrome.on("exit", res)), sleep(5000)]);
	} finally {
		cleanup();
	}
	process.exit(status);
}

main().catch((e) => { console.error(e); process.exit(2); });
