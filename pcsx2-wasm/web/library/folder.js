// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// What both disc library pages (library.html, library-3d.html) share: a folder of disc images read
// into a list of discs, folders dropped on the page, disc art from the internet, and the ?demo shelf.

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

// { discs, compressed } for the files of a folder: every disc image with an ISO 9660 volume, named
// from its serial, with the folder's own image for it if there is one. onProgress(done, total).
export async function readFolder(files, onProgress = () => {}) {
	const list = [...files].filter((f) => !/(^|\/)\._/.test(f.webkitRelativePath || f.name) && !f.name.startsWith('._'));
	const candidates = list.filter((f) => DISC_RE.test(f.name));
	const compressed = list.filter((f) => COMPRESSED_RE.test(f.name)).length;
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
		return { discs: [], compressed };

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
			cover: null,
		};
		const image = findCover(images, d);
		if (image) {
			d.cover = URL.createObjectURL(image);
			objectUrls.push(d.cover);
		}
		return d;
	});
	discs.sort((a, b) => a.title.localeCompare(b.title, undefined, { sensitivity: 'base' }) || a.fileName.localeCompare(b.fileName));
	return { discs, compressed };
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

export const noDiscsText = (compressed) => (compressed
	? 'No disc images TabStationToo can read in that folder. Compressed images (.chd, .cso, .zso, .gz) aren’t supported yet.'
	: 'No disc images in that folder. TabStationToo reads .iso, .bin and .img files.');

export function shelfAnnouncement(count, compressed) {
	let text = `${count} ${count === 1 ? 'disc' : 'discs'} on the shelf.`;
	if (compressed)
		text += ` ${compressed} compressed ${compressed === 1 ? 'image was' : 'images were'} left out; they aren’t supported yet.`;
	return text;
}

// ?demo: a shelf of made-up games, for looking at the pages without disc images.
const DEMO = [
	['Harbor Lights - Night Shift', 'NTSC-U'], ['Rally Cinder', 'PAL'], ['Moth and Lantern', 'NTSC-J'],
	['Kiln', 'PAL'], ['Orbital Tennis 2', 'NTSC-U'], ['The Quiet Tower - Book One', 'PAL'],
	['Pocket Sumo', 'NTSC-J'], ['Glass Canyon', 'NTSC-U'], ['Ferro Drift: Street Legal', 'PAL'],
	['Lighthouse Keeper', 'PAL'], ['Signal Lost', 'NTSC-U'], ['Paper Samurai - Second Fold', 'NTSC-J'],
	['Tidepool', 'PAL'], ['Velvet Engine', 'NTSC-U'],
];

export function demoDiscs() {
	return DEMO.map(([title, region], i) => ({
		file: null, fileName: `${title.replace(/[:]/g, '')}.iso`, size: (1.2 + ((i * 7) % 30) / 10) * 1e9,
		serial: `DEMO-${String(10001 + i * 37).padStart(5, '0')}`, platform: 'ps2', media: i % 4 === 3 ? 'CD' : 'DVD',
		title, region, cover: null,
	})).sort((a, b) => a.title.localeCompare(b.title));
}
