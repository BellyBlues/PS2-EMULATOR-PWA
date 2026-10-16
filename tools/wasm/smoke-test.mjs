#!/usr/bin/env node
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Smoke test for an assembled site (tools/wasm/assemble-site.py): opens it in headless Chrome
// without a BIOS and checks that it reaches its BIOS intake with the emulator runtime ready.
// No dependencies: Node 22+ (global WebSocket), Python 3 (tools/wasm/serve.py) and Chrome.
//
//   node tools/wasm/smoke-test.mjs --site DIR [--headers] [--port 8165] [--timeout 120]
//        [--chrome /usr/bin/google-chrome] [--work DIR] [--chrome-log] [-- extra Chrome flags]
//
// By default the site is served like GitHub Pages, without the cross-origin isolation headers
// (serve.py --no-isolation --max-age 600), so the page has to isolate itself through its service
// worker. --headers serves it with the headers instead (serve.py's default), where the service
// worker must stay out of the way. Passes (exit 0) when, within --timeout seconds:
//   - the page is cross-origin isolated, its runtime is ready and it shows the BIOS intake
//     (body[data-state=nobios], PCSX2Web.stats().phase "ready");
//   - it loaded once (--headers, and no service worker registered) or twice (one reload by the
//     service worker, which then controls the page);
//   - nothing was logged as an error: no console.error, uncaught exception, failed request or
//     browser error (COEP and CORP blocks among them);
//   - every request stayed on the site's origin.
// Chrome runs with a throwaway profile, removed at exit with the server and the temporary files.

import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "../..");

function parseArgs(argv) {
	const opts = { site: null, headers: false, port: 8165, timeout: 120, chrome: process.env.CHROME || "/usr/bin/google-chrome",
		work: tmpdir(), chromeLog: false, extra: [] };
	for (let i = 0; i < argv.length; i++) {
		const a = argv[i];
		if (a === "--") { opts.extra.push(...argv.slice(i + 1)); break; }
		else if (a === "--site") opts.site = path.resolve(argv[++i]);
		else if (a === "--headers") opts.headers = true;
		else if (a === "--port") opts.port = Number(argv[++i]);
		else if (a === "--timeout") opts.timeout = Number(argv[++i]);
		else if (a === "--chrome") opts.chrome = argv[++i];
		else if (a === "--work") opts.work = argv[++i];
		else if (a === "--chrome-log") opts.chromeLog = true;
		else throw new Error(`unknown argument ${a}`);
	}
	if (!opts.site || !existsSync(path.join(opts.site, "index.html")))
		throw new Error("--site must be a directory with an index.html (see tools/wasm/assemble-site.py)");
	return opts;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// What the page reports, read in the page. Fails (throws) while a navigation replaces the page.
const PROBE = `(async () => ({
	isolated: self.crossOriginIsolated === true,
	controlled: !!(navigator.serviceWorker && navigator.serviceWorker.controller),
	registrations: navigator.serviceWorker ? (await navigator.serviceWorker.getRegistrations()).length : -1,
	state: document.body ? document.body.dataset.state : '',
	runtime: document.body ? document.body.dataset.runtime : '',
	stats: window.PCSX2Web ? window.PCSX2Web.stats() : null,
	isolation: window.TabStationTooIsolation ? { ...window.TabStationTooIsolation } : null,
}))()`;

async function main() {
	const opts = parseArgs(process.argv.slice(2));
	const work = mkdtempSync(path.join(path.resolve(opts.work), "tabstationtoo-smoke-"));
	const children = [];
	const cleanup = () => {
		for (const c of children) if (c.exitCode === null && c.signalCode === null) c.kill("SIGKILL");
		rmSync(work, { recursive: true, force: true });
	};
	process.on("SIGINT", () => { cleanup(); process.exit(130); });

	const origin = `http://127.0.0.1:${opts.port}`;
	const url = `${origin}/index.html`;
	const errors = [];
	const foreign = new Set();
	let loads = 0;
	let status = 1;
	try {
		const serveArgs = [path.join(REPO, "tools/wasm/serve.py"), "--port", String(opts.port), "--quiet"];
		if (!opts.headers) serveArgs.push("--no-isolation", "--max-age", "600");
		const server = spawn("python3", [...serveArgs, opts.site], { stdio: ["ignore", "ignore", "inherit"] });
		children.push(server);

		const flags = ["--headless=new", `--user-data-dir=${path.join(work, "profile")}`, "--remote-debugging-port=0",
			"--no-first-run", "--no-default-browser-check", "--disable-extensions", "--disable-background-networking",
			"--disable-sync", "--disable-component-update", "--disable-default-apps", "--password-store=basic",
			...opts.extra, "about:blank"];
		const chrome = spawn(opts.chrome, flags, { stdio: ["ignore", "ignore", "pipe"] });
		children.push(chrome);
		let stderr = "";
		const wsUrl = await new Promise((resolve, reject) => {
			const timer = setTimeout(() => reject(new Error("Chrome did not print its DevTools URL")), 15000);
			chrome.stderr.on("data", (d) => {
				stderr += d.toString();
				if (opts.chromeLog) process.stderr.write(d);
				const m = stderr.match(/DevTools listening on (ws:\/\/\S+)/);
				if (m) { clearTimeout(timer); resolve(m[1]); }
			});
			chrome.on("exit", (code) => reject(new Error(`Chrome exited early (${code}): ${stderr}`)));
		});
		const { version } = await (async () => {
			const r = await fetch(wsUrl.replace(/^ws:/, "http:").replace(/\/devtools\/browser\/.*$/, "/json/version"));
			return { version: (await r.json()).Browser };
		})();

		const ws = new WebSocket(wsUrl);
		await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
		let nextId = 1;
		const pending = new Map();
		let mainFrame = null;
		ws.addEventListener("message", (ev) => {
			const msg = JSON.parse(ev.data);
			if (msg.id && pending.has(msg.id)) {
				const { resolve, reject } = pending.get(msg.id);
				pending.delete(msg.id);
				if (msg.error) reject(new Error(msg.error.message)); else resolve(msg.result);
				return;
			}
			const p = msg.params || {};
			switch (msg.method) {
			case "Runtime.consoleAPICalled":
				if (p.type === "error" || p.type === "assert")
					errors.push(`console.${p.type}: ${p.args.map((a) => a.value ?? a.description ?? "").join(" ")}`);
				break;
			case "Runtime.exceptionThrown":
				errors.push(`uncaught: ${p.exceptionDetails.exception?.description ?? p.exceptionDetails.text}`);
				break;
			case "Log.entryAdded":
				if (p.entry.level === "error")
					errors.push(`browser (${p.entry.source}): ${p.entry.text}${p.entry.url ? ` [${p.entry.url}]` : ""}`);
				break;
			case "Network.requestWillBeSent": {
				const u = p.request.url;
				if (!u.startsWith(origin + "/") && !/^(data|blob|about):/.test(u))
					foreign.add(u);
				break;
			}
			case "Network.loadingFailed":
				if (!p.canceled)
					errors.push(`request failed: ${p.errorText}${p.blockedReason ? ` (${p.blockedReason})` : ""}${p.corsErrorStatus ? ` (CORS ${p.corsErrorStatus.corsError})` : ""}`);
				break;
			case "Page.frameNavigated":
				if (!p.frame.parentId && p.frame.url.startsWith(origin)) {
					mainFrame = p.frame.id;
					loads++;
				}
				break;
			}
		});
		const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
			const id = nextId++;
			pending.set(id, { resolve, reject });
			ws.send(JSON.stringify({ id, method, params, sessionId }));
		});

		for (let i = 0; i < 100; i++) { // wait for the server
			try { if ((await fetch(url)).ok) break; } catch { /* not up yet */ }
			await sleep(100);
		}
		const { targetId } = await send("Target.createTarget", { url: "about:blank" });
		const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
		for (const domain of ["Runtime", "Log", "Network", "Page"])
			await send(`${domain}.enable`, {}, sessionId);
		const started = Date.now();
		await send("Page.navigate", { url }, sessionId);

		const deadline = started + opts.timeout * 1000;
		let last = null;
		while (Date.now() < deadline) {
			await sleep(250);
			try {
				const r = await send("Runtime.evaluate", { expression: PROBE, awaitPromise: true, returnByValue: true }, sessionId);
				if (r.exceptionDetails) continue;
				last = r.result.value;
			} catch {
				continue; // the page is being replaced (the service worker's reload)
			}
			if (last.stats && (last.stats.phase === "dead" || last.stats.phase === "unsupported"))
				break;
			if (last.isolated && last.stats && last.stats.phase === "ready" && last.state === "nobios")
				break;
		}
		const seconds = ((Date.now() - started) / 1000).toFixed(1);
		await sleep(1000); // let late errors arrive

		const checks = [];
		const check = (ok, text) => checks.push([ok, text]);
		const s = last || {};
		check(s.isolated === true, `cross-origin isolated (${s.isolated})`);
		check(s.stats && s.stats.phase === "ready", `runtime ready (phase ${s.stats ? s.stats.phase : "none"}, data-runtime ${s.runtime})`);
		check(s.state === "nobios", `shows the BIOS intake (data-state ${s.state})`);
		if (opts.headers) {
			check(loads === 1, `loaded once (${loads})`);
			check(s.registrations === 0 && !s.controlled, `no service worker registered (${s.registrations}, controlled ${s.controlled})`);
		} else {
			check(loads === 2, `loaded twice: the service worker reloaded the page once (${loads})`);
			check(s.controlled === true, `the service worker controls the page (${s.controlled})`);
		}
		check(errors.length === 0, `no errors (${errors.length})`);
		check(foreign.size === 0, `no requests to other origins (${foreign.size})`);

		console.log(`${version}, ${opts.headers ? "with isolation headers" : "without isolation headers (GitHub Pages)"}, ${seconds} s`);
		for (const [ok, text] of checks)
			console.log(`  ${ok ? "pass" : "FAIL"}  ${text}`);
		for (const e of errors) console.log(`  error: ${e}`);
		for (const u of foreign) console.log(`  foreign request: ${u}`);
		if (s.isolation && s.isolation.status === "unavailable") console.log(`  isolation.js: ${s.isolation.reason}`);
		if (mainFrame && checks.some(([ok]) => !ok)) {
			try {
				const r = await send("Runtime.evaluate", { expression: "window.PCSX2Web ? window.PCSX2Web.logText() : ''", returnByValue: true }, sessionId);
				console.log("  page log:\n" + String(r.result.value || "").split("\n").slice(-30).map((l) => "    " + l).join("\n"));
			} catch { /* gone */ }
		}
		status = checks.every(([ok]) => ok) ? 0 : 1;
		await send("Browser.close").catch(() => {});
		ws.close();
		await Promise.race([new Promise((r) => chrome.on("exit", r)), sleep(5000)]);
	} finally {
		cleanup();
	}
	process.exit(status);
}

main().catch((e) => { console.error(e); process.exit(2); });
