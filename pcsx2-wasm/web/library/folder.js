// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The disc library's files (disc-library.js): a folder of disc images read into a list of discs,
// folders dropped on the page or picked and remembered, and disc art from the internet.

import { readDiscInfo, DISC_RE, COMPRESSED_RE, IMAGE_RE, titleFromFileName, regionOf, loadTitleIndex, coverKey, findCover } from './disc-info.js';

// The owner allowed these pages to fetch disc art (2026-10-22), unlike the player, which never
// leaves its own origin. Only the serial goes out; the files never do. The collection is the one
// PCSX2's own cover downloader is usually pointed at; GitHub serves it with CORS and
// Cross-Origin-Resource-Policy: cross-origin, so it loads in a cross-origin isolated page.
const ONLINE_COVER = (serial) => `https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/${serial}.jpg`;
const REAL_SERIAL_RE = /^(?!DEMO)[A-Z]{4}-\d{5}$/;

let titleIndex = null;
let objectUrls = [];

export const fontsReady = () =>
	Promise.all([document.fonts.load('720 100px Archivo'), document.fonts.load('500 27px "Chivo Mono"')]).catch(() => {});

// { discs } for the files of a folder: every disc image (with an ISO 9660 volume, or compressed), named
// from its serial, with the folder's own image for it if there is one. onProgress(done, total).
// handles: each file's FileSystemFileHandle when the folder came from showDirectoryPicker(), so the
// player can remember the disc it boots.
export async function readFolder(files, onProgress = () => {}, handles = null) {
	const list = [...files].filter((f) => !/(^|\/)\._/.test(f.webkitRelativePath || f.name) && !f.name.startsWith('._'));
	const candidates = list.filter((f) => DISC_RE.test(f.name) || COMPRESSED_RE.test(f.name));
	const images = new Map();
	for (const f of list)
		if (IMAGE_RE.test(f.name))
			images.set(coverKey(f.name), f);

	const titlesReady = titleIndex ? Promise.resolve(titleIndex) : loadTitleIndex('game-titles.tsv').then((m) => (titleIndex = m));
	const found = [];
	let done = 0;
	let next = 0;
	onProgress(0, candidates.length);
	async function worker() {
		while (next < candidates.length) {
			const file = candidates[next++];
			try {
				const info = await readDiscInfo(file);
				if (info)
					found.push({ file, info });
			} catch {
				// Unreadable: left off the shelf.
			}
			onProgress(++done, candidates.length);
		}
	}
	await Promise.all([worker(), worker(), worker(), worker()]);
	const titles = await titlesReady;
	await fontsReady(); // labels are measured in the page's own faces
	if (!found.length)
		return { discs: [] };

	for (const u of objectUrls)
		URL.revokeObjectURL(u);
	objectUrls = [];
	const discs = found.map(({ file, info }) => {
		const db = info.serial ? titles.get(info.serial) : null;
		const d = {
			file, fileName: file.name, size: file.size,
			serial: info.serial, platform: info.platform, media: info.media,
			title: db?.title || titleFromFileName(file.name),
			region: regionOf(info.serial, db?.region),
			cover: null, handle: handles?.get(file) || null,
		};
		const image = findCover(images, d);
		if (image) {
			d.cover = URL.createObjectURL(image);
			objectUrls.push(d.cover);
		}
		return d;
	});
	discs.sort((a, b) => a.title.localeCompare(b.title, undefined, { sensitivity: 'base' }) || a.fileName.localeCompare(b.fileName));
	return { discs };
}

// Folders dropped on the page, read through the entries API (every browser has it).
export async function filesFromDrop(dt) {
	const entries = [...dt.items].map((it) => it.webkitGetAsEntry?.()).filter(Boolean);
	if (!entries.length)
		return [...dt.files];
	const out = [];
	async function walk(entry, depth) {
		if (entry.isFile) {
			out.push(await new Promise((res, rej) => entry.file(res, rej)));
		} else if (entry.isDirectory && depth < 4) {
			const reader = entry.createReader();
			for (;;) {
				const batch = await new Promise((res, rej) => reader.readEntries(res, rej));
				if (!batch.length)
					break;
				for (const e of batch)
					await walk(e, depth + 1);
			}
		}
	}
	for (const e of entries)
		await walk(e, 0);
	return out;
}

// ---- a folder from showDirectoryPicker() (Chromium), remembered in IndexedDB for the next visit

const FOLDER_DB = 'tabstationtoo-library';
const FOLDER_STORE = 'folder';

export const canPickFolder = () => typeof window.showDirectoryPicker === 'function';

// The folder the user picks, or null when they cancel. Throws when the picker fails.
export async function pickFolder() {
	try {
		return await window.showDirectoryPicker({ id: 'tabstationtoo-library', mode: 'read' });
	} catch (err) {
		if (err && err.name === 'AbortError')
			return null;
		throw err;
	}
}

// { files, handles } for the disc images and pictures in a folder and its subfolders (three deep).
export async function filesFromDirectory(dir) {
	const files = [];
	const handles = new Map();
	async function walk(d, depth) {
		for await (const h of d.values()) {
			if (h.kind === 'directory') {
				if (depth < 3)
					await walk(h, depth + 1);
			} else if (DISC_RE.test(h.name) || COMPRESSED_RE.test(h.name) || IMAGE_RE.test(h.name)) {
				const file = await h.getFile();
				files.push(file);
				handles.set(file, h);
			}
		}
	}
	await walk(dir, 0);
	return { files, handles };
}

function folderDb(mode, fn) {
	return new Promise((resolve, reject) => {
		if (!('indexedDB' in window)) {
			reject(new Error('IndexedDB is not available'));
			return;
		}
		const req = indexedDB.open(FOLDER_DB, 1);
		req.onupgradeneeded = () => req.result.createObjectStore(FOLDER_STORE);
		req.onerror = () => reject(req.error);
		req.onsuccess = () => {
			const db = req.result;
			const tx = db.transaction(FOLDER_STORE, mode);
			const r = fn(tx.objectStore(FOLDER_STORE));
			tx.oncomplete = () => {
				db.close();
				resolve(r && r.result);
			};
			tx.onerror = () => {
				db.close();
				reject(tx.error);
			};
		};
	});
}

export const rememberFolder = (handle) => folderDb('readwrite', (s) => s.put(handle, 'last')).catch(() => {});
export const rememberedFolder = () => folderDb('readonly', (s) => s.get('last')).catch(() => null);

// 'granted', 'prompt' or 'denied' for reading a remembered folder; ask=true asks (needs a gesture).
export async function folderPermission(handle, ask) {
	try {
		const opts = { mode: 'read' };
		return ask ? await handle.requestPermission(opts) : await handle.queryPermission(opts);
	} catch {
		return 'denied';
	}
}

// The URL of the game's cover from the internet once it has loaded, or null (no serial, not in the
// collection, offline). Each serial is asked for once per visit.
const onlineCovers = new Map();
export function onlineCover(serial) {
	if (!serial || !REAL_SERIAL_RE.test(serial))
		return Promise.resolve(null);
	if (!onlineCovers.has(serial)) {
		onlineCovers.set(serial, new Promise((resolve) => {
			const url = ONLINE_COVER(serial);
			const img = new Image();
			img.onload = () => resolve(url);
			img.onerror = () => resolve(null);
			img.src = url;
		}));
	}
	return onlineCovers.get(serial);
}

export function formatSize(bytes) {
	if (!bytes)
		return '';
	if (bytes >= 1e9)
		return `${(bytes / 1e9).toFixed(1)} GB`;
	return `${Math.round(bytes / 1e6)} MB`;
}

export function discMeta(d) {
	return [d.serial || 'No serial', d.region, d.platform === 'ps1' ? 'PS1 disc' : d.media, formatSize(d.size)].filter(Boolean).join(' · ');
}

export const noDiscsText = () => 'No disc images in that folder. TabStationToo reads .iso, .bin, .img, .chd, .cso, .zso and .gz files.';

export function shelfAnnouncement(count) {
	return `${count} ${count === 1 ? 'disc' : 'discs'} on the shelf.`;
}
