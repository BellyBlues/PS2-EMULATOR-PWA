// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// What the disc library knows about a disc image before it boots: its ISO 9660 volume, the boot file
// named in SYSTEM.CNF (and so the serial), and a title from PCSX2's game database. Only a few
// sectors at the start of each file are read; nothing is copied. Works on any Blob, so it also runs
// in Node (fs.openAsBlob) for tests.

// The layouts InputIsoFile::Detect() tries, as in app.js: [sector size, header bytes before sector 0,
// block offset]. A sector's 2048 user bytes start at header + n * size + 24 - block offset.
const LAYOUTS = [[2048, 0, 24], [2336, 0, 16], [2352, 0, 0], [2448, 0, 0],
	[2048, 150 * 2048, 24], [2352, 150 * 2048, 0], [2448, 150 * 2048, 0]];
const HEADER_BYTES = 150 * 2048 + 17 * 2448;
const MIN_SIZE = 17 * 2048;
// A CD holds at most about 800 MB; anything bigger in 2048-byte sectors is a DVD.
const CD_MAX_BYTES = 900 * 1024 * 1024;

export const DISC_RE = /\.(iso|bin|img)$/i;
export const COMPRESSED_RE = /\.(chd|cso|zso|gz)$/i;
export const IMAGE_RE = /\.(jpe?g|png|webp)$/i;

function findLayout(bytes) {
	for (const [size, offset, blockofs] of LAYOUTS) {
		const at = offset + 16 * size + 25 - blockofs;
		if (at + 5 <= bytes.length && bytes[at - 1] === 1 && bytes[at] === 0x43 && bytes[at + 1] === 0x44 &&
			bytes[at + 2] === 0x30 && bytes[at + 3] === 0x30 && bytes[at + 4] === 0x31)
			return { size, offset, data: 24 - blockofs };
	}
	return null;
}

const ascii = (bytes, from, to) => String.fromCharCode(...bytes.subarray(from, to));
const u32 = (bytes, at) => (bytes[at] | (bytes[at + 1] << 8) | (bytes[at + 2] << 16) | (bytes[at + 3] << 24)) >>> 0;

// `count` sectors from `lba`, user data only, concatenated.
async function readSectors(blob, layout, lba, count) {
	const out = new Uint8Array(count * 2048);
	if (layout.size === 2048) {
		const from = layout.offset + lba * 2048;
		out.set(new Uint8Array(await blob.slice(from, from + count * 2048).arrayBuffer()));
		return out;
	}
	const from = layout.offset + lba * layout.size;
	const raw = new Uint8Array(await blob.slice(from, from + count * layout.size).arrayBuffer());
	for (let i = 0; i < count && (i + 1) * layout.size <= raw.length; i++)
		out.set(raw.subarray(i * layout.size + layout.data, i * layout.size + layout.data + 2048), i * 2048);
	return out;
}

// A directory's records as { name, lba, size, dir }.
async function readDirectory(blob, layout, lba, size) {
	const sectors = Math.min(Math.ceil(size / 2048), 16);
	const bytes = await readSectors(blob, layout, lba, sectors);
	const entries = [];
	for (let at = 0; at < bytes.length;) {
		const len = bytes[at];
		if (len === 0) {
			at = (Math.floor(at / 2048) + 1) * 2048; // records never cross a sector
			continue;
		}
		const nameLen = bytes[at + 32];
		entries.push({
			name: ascii(bytes, at + 33, at + 33 + nameLen).replace(/;\d+$/, ''),
			lba: u32(bytes, at + 2),
			size: u32(bytes, at + 10),
			dir: (bytes[at + 25] & 2) !== 0,
		});
		at += len;
	}
	return entries;
}

// "cdrom0:\SLUS_209.46;1" -> "SLUS-20946".
export function serialFromBootPath(path) {
	const file = path.split(/[\\/:]/).pop().replace(/;\d+$/, '').trim();
	const m = /^([A-Z]{4})[_-]?(\d{3})\.?(\d{2})$/i.exec(file);
	return m ? `${m[1].toUpperCase()}-${m[2]}${m[3]}` : null;
}

export function parseSystemCnf(text) {
	const boot2 = /^\s*BOOT2\s*=\s*(\S+)/im.exec(text);
	if (boot2)
		return { platform: 'ps2', boot: boot2[1], serial: serialFromBootPath(boot2[1]) };
	const boot = /^\s*BOOT\s*=\s*(\S+)/im.exec(text);
	if (boot)
		return { platform: 'ps1', boot: boot[1], serial: serialFromBootPath(boot[1]) };
	return { platform: null, boot: null, serial: null };
}

// { media, volumeId, platform, serial } for a disc image, or null when it holds no ISO 9660 volume.
export async function readDiscInfo(blob) {
	if (blob.size < MIN_SIZE)
		return null;
	const head = new Uint8Array(await blob.slice(0, Math.min(blob.size, HEADER_BYTES)).arrayBuffer());
	const layout = findLayout(head);
	if (!layout)
		return null;
	const pvd = await readSectors(blob, layout, 16, 1);
	const info = {
		media: layout.size === 2048 && blob.size > CD_MAX_BYTES ? 'DVD' : 'CD',
		volumeId: ascii(pvd, 40, 72).trim(),
		platform: null,
		serial: null,
	};
	try {
		const root = await readDirectory(blob, layout, u32(pvd, 156 + 2), u32(pvd, 156 + 10));
		const cnf = root.find((e) => !e.dir && e.name.toUpperCase() === 'SYSTEM.CNF');
		if (cnf) {
			const bytes = await readSectors(blob, layout, cnf.lba, Math.min(Math.ceil(cnf.size / 2048), 2));
			Object.assign(info, parseSystemCnf(ascii(bytes, 0, Math.min(cnf.size, bytes.length))));
		}
	} catch {
		// A volume without a readable root still gets a disc on the shelf, named after its file.
	}
	return info;
}

// "Guitar Hero (Europe, Australia) (En,Fr,De,Es,It).iso" -> "Guitar Hero".
export function titleFromFileName(name) {
	let t = name.replace(/\.[^.]+$/, '');
	t = t.replace(/\s*[([][^)\]]*[)\]]/g, '');
	t = t.replace(/[_]+/g, ' ').replace(/\s{2,}/g, ' ').trim();
	return t || name;
}

// The database's region, else one from the serial's prefix.
export function regionOf(serial, dbRegion) {
	if (dbRegion)
		return dbRegion.replace(/-M\d+$/, '').replace(/^PAL-.*/, 'PAL');
	if (!serial)
		return '';
	const p = serial.slice(0, 4);
	if (/^(SLUS|SCUS|LSP)/.test(p))
		return 'NTSC-U';
	if (/^(SLES|SCES|SCED)/.test(p))
		return 'PAL';
	if (/^(SLPS|SLPM|SCPS|SCAJ|SLAJ|PAPX)/.test(p))
		return 'NTSC-J';
	if (/^(SLKA|SCKA)/.test(p))
		return 'NTSC-K';
	return '';
}

// game-titles.tsv (tools/wasm/make-game-titles.py): "SERIAL\tTitle\tRegion" lines from PCSX2's
// GameIndex.yaml. Resolves to an empty Map when the file is missing (the page served straight from
// pcsx2-wasm/web, without a build), so titles then come from the file names.
export async function loadTitleIndex(url) {
	const map = new Map();
	try {
		const res = await fetch(url, { cache: 'force-cache' });
		if (!res.ok)
			return map;
		for (const line of (await res.text()).split('\n')) {
			const [serial, title, region] = line.split('\t');
			if (serial && title)
				map.set(serial, { title, region: region || '' });
		}
	} catch {
		// No index: file names it is.
	}
	return map;
}

// Lower case, letters and digits only: how covers and discs are matched by name.
export const coverKey = (name) => name.replace(/\.[^.]+$/, '').toLowerCase().replace(/[^a-z0-9]+/g, '');

// A cover for a disc from the images in the chosen folder, named as PCSX2's own covers folder names
// them (the serial, or the title) or after the disc file.
export function findCover(images, disc) {
	const keys = [disc.serial, disc.fileName, titleFromFileName(disc.fileName), disc.title].filter(Boolean).map(coverKey);
	for (const k of keys) {
		const image = images.get(k);
		if (image)
			return image;
	}
	return null;
}
