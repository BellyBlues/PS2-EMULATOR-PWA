#!/usr/bin/env node
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Runs the TabStationToo page in a real browser and measures the emulator's speed: it starts
// tools/wasm/serve.py, launches Zen (or Firefox) over WebDriver BiDi, or Chrome over the DevTools
// protocol, with a fresh temporary profile, opens index.html?bios=...[&elf=...], samples the speed,
// the VPS and the vsync count, and prints a table (per benchmark phase when the program prints
// "BENCH phase <name>" lines to the PS2's console) and writes JSON. No dependencies: Node 22+
// (global WebSocket), Python 3 and the browser.
//
//   node tools/wasm/bench/run-bench.mjs --bios FILE [--elf FILE] [options]
//
// Options:
//   --bios FILE          the PS2 BIOS (its directory is served read-only, serve.py --bios-dir)
//   --disc FILE          a disc image to boot (served read-only, serve.py --disc; ?disc=)
//   --keep-profile DIR   use DIR as the browser profile and keep it (a later run sees what this one
//                        stored in IndexedDB and the browser's caches)
//   --elf FILE           a PS2 program to boot (served read-only, serve.py --elf; ?elf=). Without it
//                        the BIOS boots to its menu
//   --browser zen|chrome default zen (WebDriver BiDi); chrome uses the DevTools protocol
//   --browser-path PATH  the browser's executable (defaults: /Applications/Zen.app/Contents/MacOS/zen,
//                        /Applications/Google Chrome.app/Contents/MacOS/Google Chrome)
//   --headless           run the browser headless (default: a window, so WebGPU uses the real GPU)
//   --seconds S          measure for S seconds once the VM runs (default 60)
//   --vsyncs N           or stop once N vsyncs were emulated after the VM started
//   --phases N           or stop when the (N+1)th "BENCH phase" line arrives (N whole phases)
//   --sample-ms MS       sample every MS milliseconds (default 1000)
//   --unlimited          frame limiter off (?unlimited=1), to measure speed above 100 %
//   --param K=V          extra query parameter for the page (repeatable), e.g. --param renderer=sw,
//                        --param gpu=1, --param set=EmuCore/GS/extrathreads=4
//   --capture N,M,...    at the end, hash (SHA-1, in the JSON) the first frame the page received at
//                        vsync N or later (?capture=), to compare output between runs and browsers
//   --query Q            extra query string, appended as is (e.g. "gpu=0&eejit=0")
//   --pref K=V           Zen/Firefox only: a preference for the fresh profile (repeatable; V is
//                        true, false, a number or a string), e.g. to compare JavaScript engine settings
//   --root DIR           the web root (default build-wasm/bin)
//   --port N             serve.py's port on 127.0.0.1 (default: a free one)
//   --out FILE           the JSON results (default: <tmp>/tst-bench-<browser>-<time>.json); the
//                        browser's console goes next to it, as FILE with .console.log
//   --boot-timeout S     give up if the VM is not running S seconds after the page loads (default 120)
//   --quiet              do not echo phase lines and page warnings
//
// The page runs with ?console=1 (its log is mirrored to the browser console, where the runner reads
// the phase lines) and ?mute=1. Each sample records the speed (pcsx2web_speed), the VPS
// (pcsx2web_vps), the vsync count (pcsx2web_vsync_count), how busy the EE, GS and VU1 threads were
// (pcsx2web_thread_usage: not waiting for another thread) and the page's stats (state, display path,
// renderer). A phase's speed is the median of its samples, leaving out the first one after each
// change of phase (its averaging window may still hold the previous phase); its VPS comes from the
// vsync count over the phase's sampled time. The browser and the server are stopped at exit and the
// profile is deleted; only the processes this script started are touched.

import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import net from "node:net";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "../../..");
const BROWSERS = {
	zen: "/Applications/Zen.app/Contents/MacOS/zen",
	chrome: "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
};
const PHASE_RE = /BENCH phase (\S+)/;

function usage() {
	return "usage: node tools/wasm/bench/run-bench.mjs --bios FILE [--elf FILE] [--browser zen|chrome] " +
		"[--headless] [--seconds S | --vsyncs N | --phases N] [--unlimited] [--param K=V]... (see the file's header)";
}

function parseArgs(argv) {
	const o = { bios: null, elf: null, disc: null, keepProfile: null, browser: "zen", browserPath: null, headless: false, seconds: 60, vsyncs: 0,
		phases: 0, sampleMs: 1000, unlimited: false, params: [], capture: [], prefs: [], query: "", root: path.join(REPO, "build-wasm/bin"),
		port: 0, out: null, bootTimeout: 120, quiet: false };
	for (let i = 0; i < argv.length; i++) {
		const a = argv[i];
		const v = () => {
			if (i + 1 >= argv.length) throw new Error(`${a} needs a value`);
			return argv[++i];
		};
		if (a === "--bios") o.bios = path.resolve(v());
		else if (a === "--elf") o.elf = path.resolve(v());
		else if (a === "--disc") o.disc = path.resolve(v());
		else if (a === "--keep-profile") o.keepProfile = path.resolve(v());
		else if (a === "--browser") o.browser = v();
		else if (a === "--browser-path") o.browserPath = v();
		else if (a === "--headless") o.headless = true;
		else if (a === "--seconds") o.seconds = Number(v());
		else if (a === "--vsyncs") o.vsyncs = Number(v());
		else if (a === "--phases") o.phases = Number(v());
		else if (a === "--sample-ms") o.sampleMs = Number(v());
		else if (a === "--unlimited") o.unlimited = true;
		else if (a === "--param") o.params.push(v());
		else if (a === "--capture") o.capture.push(...v().split(",").map(Number).filter((n) => n > 0));
		else if (a === "--query") o.query = v().replace(/^[?&]/, "");
		else if (a === "--pref") o.prefs.push(v());
		else if (a === "--root") o.root = path.resolve(v());
		else if (a === "--port") o.port = Number(v());
		else if (a === "--out") o.out = path.resolve(v());
		else if (a === "--boot-timeout") o.bootTimeout = Number(v());
		else if (a === "--quiet") o.quiet = true;
		else if (a === "--help" || a === "-h") {
			// The header comment, after the SPDX lines.
			const header = readFileSync(fileURLToPath(import.meta.url), "utf8").split("\n").filter((l) => l.startsWith("//"));
			console.log(header.slice(2).map((l) => l.replace(/^\/\/ ?/, "")).join("\n").trim());
			process.exit(0);
		} else throw new Error(`unknown argument ${a}\n${usage()}`);
	}
	if (!o.bios) throw new Error(`--bios is required\n${usage()}`);
	if (!BROWSERS[o.browser]) throw new Error("--browser must be zen or chrome");
	if (!(o.sampleMs >= 100)) throw new Error("--sample-ms must be 100 or more");
	o.browserPath ??= BROWSERS[o.browser];
	o.out ??= path.join(tmpdir(), `tst-bench-${o.browser}-${new Date().toISOString().replace(/[:.]/g, "-")}.json`);
	return o;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const within = (promise, ms, what) => Promise.race([promise,
	sleep(ms).then(() => { throw new Error(`${what}: no answer within ${ms} ms`); })]);

function freePort() {
	return new Promise((resolve, reject) => {
		const s = net.createServer();
		s.on("error", reject);
		s.listen(0, "127.0.0.1", () => {
			const { port } = s.address();
			s.close(() => resolve(port));
		});
	});
}

// Waits until the child prints a line matching re (on stdout or stderr); returns the match.
function waitForOutput(child, re, ms, what) {
	return new Promise((resolve, reject) => {
		let text = "";
		const timer = setTimeout(() => reject(new Error(`${what} did not start within ${ms / 1000} s:\n${text.slice(-2000)}`)), ms);
		const onData = (d) => {
			text += d.toString();
			const m = re.exec(text);
			if (m) {
				clearTimeout(timer);
				resolve(m);
			}
		};
		child.stdout?.on("data", onData);
		child.stderr?.on("data", onData);
		child.on("exit", (code) => {
			clearTimeout(timer);
			reject(new Error(`${what} exited early (${code}):\n${text.slice(-2000)}`));
		});
	});
}

// One JSON-RPC style connection (WebDriver BiDi and the DevTools protocol share the shape).
class Rpc {
	constructor(ws) {
		this.ws = ws;
		this.nextId = 1;
		this.pending = new Map();
		this.handlers = [];
		ws.addEventListener("message", (ev) => {
			const msg = JSON.parse(ev.data);
			if (msg.id !== undefined && this.pending.has(msg.id)) {
				const { resolve, reject } = this.pending.get(msg.id);
				this.pending.delete(msg.id);
				if (msg.error) reject(new Error(typeof msg.error === "string" ? `${msg.error}: ${msg.message}` :
					`${msg.error.message} (${msg.error.code})`));
				else resolve(msg.result);
			} else if (msg.method) {
				for (const h of this.handlers) h(msg);
			}
		});
	}
	send(method, params = {}, sessionId = undefined) {
		const id = this.nextId++;
		this.ws.send(JSON.stringify(sessionId ? { id, method, params, sessionId } : { id, method, params }));
		return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
	}
	on(h) {
		this.handlers.push(h);
	}
}

async function openSocket(url) {
	const ws = new WebSocket(url);
	await new Promise((resolve, reject) => {
		ws.onopen = resolve;
		ws.onerror = (e) => reject(new Error(`cannot connect to ${url}: ${e.message || "error"}`));
	});
	return ws;
}

// Zen and Firefox: WebDriver BiDi over --remote-debugging-port.
async function launchGecko(o, profile, onConsole, children) {
	// A fresh profile that skips first-run pages and stays offline from update and telemetry services.
	const prefs = {
		"browser.shell.checkDefaultBrowser": false,
		"browser.startup.homepage_override.mstone": "ignore",
		"browser.aboutwelcome.enabled": false,
		"startup.homepage_welcome_url": "",
		"browser.tabs.warnOnClose": false,
		"browser.sessionstore.resume_from_crash": false,
		"datareporting.policy.dataSubmissionEnabled": false,
		"toolkit.telemetry.reportingpolicy.firstRun": false,
		"app.update.disabledForTesting": true,
		"zen.welcome-screen.seen": true,
	};
	for (const p of o.prefs) {
		const eq = p.indexOf("=");
		if (eq <= 0) throw new Error(`--pref ${p}: expected K=V`);
		const v = p.slice(eq + 1);
		prefs[p.slice(0, eq)] = v === "true" ? true : v === "false" ? false : /^-?\d+$/.test(v) ? Number(v) : v;
	}
	writeFileSync(path.join(profile, "user.js"),
		Object.entries(prefs).map(([k, v]) => `user_pref(${JSON.stringify(k)}, ${JSON.stringify(v)});`).join("\n") + "\n");
	const port = await freePort();
	// -no-remote and -new-instance: never talk to (or open a window in) a browser already running.
	const args = ["-profile", profile, "-no-remote", "-new-instance", "--remote-debugging-port", String(port),
		"--remote-allow-hosts", "127.0.0.1", "--remote-allow-origins", "null", ...(o.headless ? ["--headless"] : []),
		"about:blank"];
	const browser = spawn(o.browserPath, args, { stdio: ["ignore", "pipe", "pipe"], detached: true });
	children.push(browser);
	const m = await waitForOutput(browser, /WebDriver BiDi listening on (ws:\/\/\S+)/, 30000, "the browser");
	const ws = await openSocket(`${m[1].replace(/\/$/, "")}/session`);
	const rpc = new Rpc(ws);
	const { capabilities } = await rpc.send("session.new", { capabilities: {} });
	await rpc.send("session.subscribe", { events: ["log.entryAdded"] });
	rpc.on((msg) => {
		if (msg.method === "log.entryAdded")
			onConsole(msg.params.text ?? "", msg.params.level ?? "info");
	});
	const { contexts } = await rpc.send("browsingContext.getTree", {});
	const context = contexts[0].context;
	return {
		name: `${capabilities.browserName} ${capabilities.browserVersion}`,
		navigate: (url) => rpc.send("browsingContext.navigate", { context, url, wait: "complete" }),
		evaluate: async (expression) => {
			const r = await rpc.send("script.evaluate", { expression, target: { context }, awaitPromise: false,
				resultOwnership: "none" });
			if (r.type === "exception") throw new Error(r.exceptionDetails?.text ?? "exception");
			return r.result?.value;
		},
		close: () => within(rpc.send("browser.close", {}), 5000, "browser.close").catch(() => {}),
	};
}

// Chrome: the DevTools protocol, as tools/wasm/run-app-headless.mjs.
async function launchChrome(o, profile, onConsole, children) {
	const args = [`--user-data-dir=${profile}`, "--remote-debugging-port=0", "--no-first-run",
		"--no-default-browser-check", "--disable-extensions", "--disable-background-networking", "--disable-sync",
		"--disable-component-update", "--disable-default-apps", "--password-store=basic", "--mute-audio",
		"--autoplay-policy=no-user-gesture-required", "--disable-background-timer-throttling",
		"--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows", "--window-size=1280,900",
		...(o.headless ? ["--headless=new"] : []), "about:blank"];
	const browser = spawn(o.browserPath, args, { stdio: ["ignore", "pipe", "pipe"], detached: true });
	children.push(browser);
	const m = await waitForOutput(browser, /DevTools listening on (ws:\/\/\S+)/, 30000, "Chrome");
	const rpc = new Rpc(await openSocket(m[1]));
	const { product } = await rpc.send("Browser.getVersion");
	const { targetId } = await rpc.send("Target.createTarget", { url: "about:blank" });
	const { sessionId } = await rpc.send("Target.attachToTarget", { targetId, flatten: true });
	rpc.on((msg) => {
		if (msg.method === "Runtime.consoleAPICalled" && msg.sessionId === sessionId) {
			const text = msg.params.args.map((a) => a.value ?? a.description ?? a.unserializableValue ?? "").join(" ");
			onConsole(text, msg.params.type === "log" ? "info" : msg.params.type);
		}
	});
	await rpc.send("Runtime.enable", {}, sessionId);
	await rpc.send("Page.enable", {}, sessionId);
	return {
		name: product,
		navigate: (url) => rpc.send("Page.navigate", { url }, sessionId),
		evaluate: async (expression) => {
			const r = await rpc.send("Runtime.evaluate", { expression, returnByValue: true }, sessionId);
			if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
			return r.result.value;
		},
		close: () => within(rpc.send("Browser.close"), 5000, "Browser.close").catch(() => {}),
	};
}

// Evaluated in the page; returns JSON text (BiDi and CDP both pass strings through unchanged).
const SAMPLE = `(() => {
	const M = window.Module;
	const s = window.PCSX2Web ? window.PCSX2Web.stats() : null;
	const has = (n) => !!(M && typeof M['_' + n] === 'function');
	return JSON.stringify({
		now: performance.now(),
		speed: has('pcsx2web_speed') ? M._pcsx2web_speed() : null,
		vps: has('pcsx2web_vps') ? M._pcsx2web_vps() : null,
		vsync: has('pcsx2web_vsync_count') ? M._pcsx2web_vsync_count() >>> 0 : null,
		threads: has('pcsx2web_thread_usage') ? [0, 1, 2].map((i) => M._pcsx2web_thread_usage(i)) : null,
		stats: s && { phase: s.phase, state: s.state, view: s.view, display: s.display, gpuMode: s.gpuMode,
			gpuFallback: s.gpuFallback, framesPresented: s.framesPresented, gpuFrames: s.gpuFrames,
			width: s.width, height: s.height },
		isolated: window.crossOriginIsolated,
		jspi: typeof WebAssembly === 'object' && typeof WebAssembly.Suspending === 'function',
		webgpu: !!navigator.gpu,
		userAgent: navigator.userAgent,
	});
})()`;

function rendererName(mode, display) {
	if (!mode) return display ? `unknown (display ${display})` : "unknown";
	const name = mode.hw ? "hardware renderer on WebGPU" : mode.webgpu ? "software renderer, WebGPU display" :
		"software renderer, 2D canvas";
	return mode.reason ? `${name} (${mode.reason})` : name;
}

const median = (xs) => {
	if (!xs.length) return null;
	const s = [...xs].sort((a, b) => a - b);
	const m = s.length >> 1;
	return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
};
const fmt = (x, d = 1) => (x === null || x === undefined || Number.isNaN(x) ? "-" : x.toFixed(d));

// Per-phase figures from the samples (each tagged with its phase and segment).
function summarise(samples) {
	const phases = new Map();
	const get = (name) => {
		if (!phases.has(name)) phases.set(name, { phase: name, speeds: [], threads: [[], [], []], seconds: 0, vsyncs: 0, segments: new Set() });
		return phases.get(name);
	};
	for (let i = 0; i < samples.length; i++) {
		const s = samples[i], prev = samples[i - 1];
		const p = get(s.phase ?? "(none)");
		p.segments.add(s.segment);
		// The first sample of a segment may still average the previous phase.
		if (prev && prev.segment === s.segment) {
			if (s.speed !== null) p.speeds.push(s.speed);
			if (s.threads) s.threads.forEach((u, k) => p.threads[k].push(u));
			if (s.vsync !== null && prev.vsync !== null) {
				p.seconds += s.t - prev.t;
				p.vsyncs += s.vsync - prev.vsync;
			}
		}
	}
	const rows = [...phases.values()].map((p) => ({
		phase: p.phase,
		segments: p.segments.size,
		samples: p.speeds.length,
		speedMedian: median(p.speeds),
		speedMin: p.speeds.length ? Math.min(...p.speeds) : null,
		speedMax: p.speeds.length ? Math.max(...p.speeds) : null,
		seconds: p.seconds,
		vps: p.seconds > 0 ? p.vsyncs / p.seconds : null,
		// Busy (not waiting for another thread), median percent: EE, GS, VU1.
		threads: p.threads.map((xs) => median(xs)),
	}));
	const all = samples.filter((s, i) => i > 0 && s.speed !== null && samples[i - 1].segment === s.segment).map((s) => s.speed);
	const first = samples.find((s) => s.vsync !== null), last = [...samples].reverse().find((s) => s.vsync !== null);
	const total = {
		phase: "all",
		segments: new Set(samples.map((s) => s.segment)).size,
		samples: all.length,
		speedMedian: median(all),
		speedMin: all.length ? Math.min(...all) : null,
		speedMax: all.length ? Math.max(...all) : null,
		seconds: first && last ? last.t - first.t : 0,
		vps: first && last && last.t > first.t ? (last.vsync - first.vsync) / (last.t - first.t) : null,
	};
	return { rows, total };
}

function printTable(rows) {
	const head = ["phase", "segments", "samples", "speed % (median)", "min", "max", "VPS", "seconds", "EE/GS/VU busy %"];
	const body = rows.map((r) => [r.phase, String(r.segments), String(r.samples), fmt(r.speedMedian), fmt(r.speedMin),
		fmt(r.speedMax), fmt(r.vps), fmt(r.seconds), r.threads ? r.threads.map((x) => fmt(x, 0)).join("/") : "-"]);
	const widths = head.map((h, i) => Math.max(h.length, ...body.map((b) => b[i].length)));
	const line = (cells) => cells.map((c, i) => (i === 0 ? c.padEnd(widths[i]) : c.padStart(widths[i]))).join("  ");
	console.log(line(head));
	console.log(widths.map((w) => "-".repeat(w)).join("  "));
	for (const b of body) console.log(line(b));
}

async function main() {
	const o = parseArgs(process.argv.slice(2));
	const children = [];
	if (o.keepProfile) mkdirSync(o.keepProfile, { recursive: true });
	const profile = o.keepProfile || mkdtempSync(path.join(tmpdir(), `tst-bench-${o.browser}-`));
	const consoleLines = [];
	const t0 = Date.now();
	const elapsed = () => (Date.now() - t0) / 1000;
	let browser = null;
	let cleaned = false;

	// Only the processes started here: each runs in its own process group (detached), killed whole.
	const killAll = async () => {
		for (const c of children.reverse()) {
			if (c.exitCode !== null || c.signalCode !== null) continue;
			try {
				process.kill(-c.pid, "SIGTERM");
			} catch {
				c.kill("SIGTERM");
			}
			for (let i = 0; i < 50 && c.exitCode === null && c.signalCode === null; i++) await sleep(100);
			if (c.exitCode === null && c.signalCode === null) {
				try {
					process.kill(-c.pid, "SIGKILL");
				} catch {
					c.kill("SIGKILL");
				}
			}
		}
	};
	const cleanup = async () => {
		if (cleaned) return;
		cleaned = true;
		if (browser) await browser.close();
		await killAll();
		if (!o.keepProfile) rmSync(profile, { recursive: true, force: true });
	};
	process.on("SIGINT", () => { cleanup().finally(() => process.exit(130)); });

	// Phases from the mirrored console.
	let phase = null, segment = 0, phaseLines = 0, stopReason = null;
	const phaseEvents = [];
	const onConsole = (text, level) => {
		for (const line of String(text).split("\n")) {
			consoleLines.push(`${elapsed().toFixed(3).padStart(9)} [${level}] ${line}`);
			const m = PHASE_RE.exec(line);
			if (m) {
				// The page's log holds a line once; a program may print the same phase twice (EE and IOP consoles).
				if (m[1] !== phase || !phaseEvents.length || elapsed() - phaseEvents.at(-1).t > 0.5) {
					phaseLines++;
					if (o.phases && phaseLines > o.phases) stopReason ??= `${o.phases} phases`;
					phase = m[1];
					segment++;
					phaseEvents.push({ t: elapsed(), phase, line: line.trim() });
				}
				if (!o.quiet) console.log(`${elapsed().toFixed(1).padStart(7)} s  ${line.trim()}`);
			} else if (!o.quiet && (level === "error" || level === "warn" || level === "warning") && /\S/.test(line) &&
				!/^\[page\]/.test(line)) {
				// PCSX2's stderr lines; the page's own warnings stay in the console file.
				if (/error|fail/i.test(line)) console.log(`${elapsed().toFixed(1).padStart(7)} s  ${line.trim()}`);
			}
		}
	};

	const results = { ok: false, browser: null, options: o, url: null, renderer: null, samples: [], phases: [], total: null,
		phaseEvents, errors: [] };
	let status = 1;
	try {
		const port = o.port || await freePort();
		const biosDir = path.dirname(o.bios);
		const server = spawn("python3", [path.join(REPO, "tools/wasm/serve.py"), "--port", String(port), "--quiet",
			"--bios-dir", biosDir, ...(o.elf ? ["--elf", o.elf] : []), ...(o.disc ? ["--disc", o.disc] : []), o.root], { stdio: ["ignore", "pipe", "pipe"], detached: true });
		children.push(server);
		await waitForOutput(server, /Press Ctrl\+C/, 10000, "serve.py");

		// Encoded twice, as serve.py prints them: the page decodes the query, then reads the value as a URL.
		const twice = (s) => encodeURIComponent(encodeURIComponent(s));
		const query = [`bios=dev-bios/${twice(path.basename(o.bios))}`];
		if (o.elf) query.push(`elf=dev-elf/${twice(path.basename(o.elf))}`);
		if (o.disc) query.push(`disc=dev-disc/${twice(path.basename(o.disc))}`);
		query.push("console=1", "mute=1");
		if (o.unlimited) query.push("unlimited=1");
		for (const p of o.params) {
			const eq = p.indexOf("=");
			query.push(eq < 0 ? encodeURIComponent(p) : `${encodeURIComponent(p.slice(0, eq))}=${encodeURIComponent(p.slice(eq + 1))}`);
		}
		if (o.capture.length) query.push(`capture=${o.capture.join(",")}`);
		if (o.query) query.push(o.query);
		const url = `http://127.0.0.1:${port}/index.html?${query.join("&")}`;
		results.url = url;

		browser = o.browser === "chrome" ? await launchChrome(o, profile, onConsole, children) :
			await launchGecko(o, profile, onConsole, children);
		results.browser = browser.name;
		console.log(`${browser.name}${o.headless ? " (headless)" : ""}`);
		console.log(url);
		await within(browser.navigate(url), 60000, "navigation");

		// Wait for the VM.
		const sample = async () => JSON.parse(await within(browser.evaluate(SAMPLE), 10000, "sample"));
		let s = null;
		const bootDeadline = elapsed() + o.bootTimeout;
		for (;;) {
			s = await sample();
			const state = s.stats?.state;
			if (state === "running") break;
			if (state === "failed" || s.stats?.phase === "dead" || s.stats?.phase === "unsupported")
				throw new Error(`the page did not boot (phase ${s.stats?.phase}, state ${state}); see the console file`);
			if (elapsed() > bootDeadline)
				throw new Error(`the VM was not running after ${o.bootTimeout} s (phase ${s.stats?.phase}, state ${state})`);
			await sleep(250);
		}
		const runStart = elapsed();
		results.bootSeconds = runStart;
		results.userAgent = s.userAgent;
		results.crossOriginIsolated = s.isolated;
		results.jspi = s.jspi;
		results.webgpu = s.webgpu;
		console.log(`VM running ${runStart.toFixed(1)} s after launch; isolated ${s.isolated}, JSPI ${s.jspi}, ` +
			`navigator.gpu ${s.webgpu}`);
		const vsync0 = s.vsync;
		let lastReport = 0;
		let shownRenderer = null;
		for (;;) {
			await sleep(o.sampleMs);
			if (stopReason) break; // --phases: the next phase has begun; it is not measured
			s = await sample();
			const t = elapsed() - runStart;
			const renderer = rendererName(s.stats?.gpuMode, s.stats?.display);
			if (renderer !== shownRenderer && s.stats?.gpuMode) {
				shownRenderer = renderer;
				console.log(`${elapsed().toFixed(1).padStart(7)} s  renderer: ${renderer}; display ${s.stats.display}`);
			}
			results.samples.push({ t, phase, segment, speed: s.speed, vps: s.vps, vsync: s.vsync, threads: s.threads, state: s.stats?.state,
				display: s.stats?.display, frames: Math.max(s.stats?.framesPresented ?? 0, s.stats?.gpuFrames ?? 0),
				size: s.stats ? `${s.stats.width}x${s.stats.height}` : null });
			results.renderer = renderer;
			results.gpuMode = s.stats?.gpuMode ?? null;
			results.display = s.stats?.display ?? null;
			results.gpuFallback = s.stats?.gpuFallback ?? null;
			if (!o.quiet && t - lastReport >= 10) {
				lastReport = t;
				console.log(`${elapsed().toFixed(1).padStart(7)} s  speed ${fmt(s.speed)} %, ${fmt(s.vps)} VPS, vsync ${s.vsync}` +
					(phase ? `, phase ${phase}` : ""));
			}
			if (s.stats?.state !== "running") {
				results.errors.push(`the VM left the running state (${s.stats?.state})`);
				break;
			}
			if (stopReason) break;
			if (o.vsyncs && s.vsync - vsync0 >= o.vsyncs) { stopReason = `${o.vsyncs} vsyncs`; break; }
			if (!o.vsyncs && !o.phases && t >= o.seconds) { stopReason = `${o.seconds} s`; break; }
			if ((o.vsyncs || o.phases) && t >= Math.max(o.seconds, 1) * 20) { stopReason = "20x --seconds safety limit"; break; }
		}
		results.stopReason = stopReason;
		try {
			const log = await within(browser.evaluate("window.PCSX2Web ? window.PCSX2Web.logText() : ''"), 10000, "log");
			results.graphicsLine = (log.match(/\[page\] Graphics: .*/g) || []).at(-1) ?? null;
		} catch { /* optional */ }
		if (o.capture.length) {
			results.captures = {};
			for (const n of o.capture) {
				const r = JSON.parse(await within(browser.evaluate(
					`JSON.stringify(window.PCSX2Web.capture ? window.PCSX2Web.capture(${n}) : null)`), 30000, "capture"));
				results.captures[n] = r ? { frame: r.frame, width: r.width, height: r.height,
					sha1: createHash("sha1").update(Buffer.from(r.rgba, "base64")).digest("hex") } : null;
				if (!r) results.errors.push(`no frame captured at vsync ${n} or later`);
				console.log(`capture ${n}: ${r ? `vsync ${r.frame}, ${r.width}x${r.height}, ${results.captures[n].sha1}` : "none"}`);
			}
		}

		const { rows, total } = summarise(results.samples);
		results.phases = rows;
		results.total = total;
		console.log("");
		console.log(`${browser.name}: ${results.renderer}${results.graphicsLine ? ` — ${results.graphicsLine.replace("[page] ", "")}` : ""}`);
		console.log(`stopped after ${stopReason ?? "an error"}; ${results.samples.length} samples every ${o.sampleMs} ms`);
		printTable(phaseEvents.length ? [...rows, total] : [total]);
		results.ok = results.errors.length === 0;
		status = results.ok ? 0 : 1;
	} catch (err) {
		results.errors.push(String(err.message || err));
		console.error(`error: ${err.message || err}`);
	} finally {
		writeFileSync(o.out, JSON.stringify(results, null, "\t") + "\n");
		const consoleFile = o.out.replace(/\.json$/, "") + ".console.log";
		writeFileSync(consoleFile, consoleLines.join("\n") + "\n");
		console.log(`results: ${o.out}\nconsole: ${consoleFile}`);
		await cleanup();
	}
	process.exit(status);
}

main().catch((err) => {
	console.error(err);
	process.exit(2);
});
