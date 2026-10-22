// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The printed side of a disc, as one SVG on a 1000-unit square: a clear hub with its hole, and a
// label printed from radius 180 to 490. The print is the disc's own image when the folder has one,
// otherwise a label drawn here from the title and serial, with a palette and a motif picked from a
// hash of the serial so that a disc always looks the same.

const NS = 'http://www.w3.org/2000/svg';
const R_HOLE = 62;
const R_HUB = 180;
const R_PRINT = 490;
const R_EDGE = 498;

// [background, ink, accent]. Era-plausible print colours; no blues, which the house keeps away from.
const PALETTES = [
	['#e9e3d6', '#1a1714', '#c8442f'],
	['#1f3b2d', '#ece4d0', '#d9a441'],
	['#5b1e1e', '#f0e4d0', '#e7a23b'],
	['#d8a23a', '#1c1408', '#7a2e12'],
	['#2a2826', '#ece8e1', '#f2b544'],
	['#c4462e', '#fbefe3', '#2a1410'],
	['#3e2a47', '#ede3f0', '#e58f65'],
	['#1e4644', '#e6efe9', '#e9c46a'],
	['#b7b2a6', '#151413', '#8e2c1f'],
	['#0f0f0f', '#f4f1ea', '#e04e39'],
	['#f2b544', '#1a1306', '#121110'],
	['#6b4a2b', '#f3e7d3', '#e9d8b4'],
];
const MOTIFS = ['rings', 'band', 'split', 'glyph', 'stripes', 'sun'];

export function hash(text) {
	let h = 2166136261;
	for (let i = 0; i < text.length; i++)
		h = Math.imul(h ^ text.charCodeAt(i), 16777619);
	return h >>> 0;
}

const esc = (s) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// "Onimusha - Dawn of Dreams [Disc 1 of 2]" -> { main: "Onimusha", sub: "Dawn of Dreams", disc: "Disc 1 of 2" }.
export function splitTitle(title) {
	let disc = '';
	let t = title.replace(/\s*\[([^\]]*)\]/g, (_, inner) => {
		if (/disc/i.test(inner))
			disc = inner;
		return '';
	}).trim();
	const at = t.search(/\s[-–:]\s|:\s/);
	if (at > 0)
		return { main: t.slice(0, at).trim(), sub: t.slice(at).replace(/^\s*[-–:]\s*/, '').trim(), disc };
	return { main: t, sub: '', disc };
}

let measureCtx = null;
// Width of `text` at 100 units in the label's title face (Archivo, width 125).
function measure(text, weight, stretch) {
	if (!measureCtx) {
		measureCtx = document.createElement('canvas').getContext('2d');
	}
	measureCtx.font = `${weight} 100px Archivo, ui-sans-serif, sans-serif`;
	let scale = 1;
	if ('fontStretch' in measureCtx)
		measureCtx.fontStretch = stretch >= 125 ? 'expanded' : 'normal';
	else if (stretch >= 125)
		scale = 1.2;
	return measureCtx.measureText(text).width * scale;
}

// Lines of at most `max` balanced by length.
function wrap(text, max) {
	const words = text.split(/\s+/).filter(Boolean);
	if (words.length <= 1 || max === 1)
		return [text];
	for (let lines = 1; lines <= max; lines++) {
		const target = Math.ceil(text.length / lines);
		const out = [];
		let cur = '';
		for (const w of words) {
			if (cur && (cur + ' ' + w).length > target + 3 && out.length < lines - 1) {
				out.push(cur);
				cur = w;
			} else {
				cur = cur ? cur + ' ' + w : w;
			}
		}
		out.push(cur);
		if (out.every((l) => l.length <= Math.max(target + 4, 9)) || lines === max)
			return out;
	}
	return [text];
}

// Half the chord of the print circle at height y, less a margin.
const chordHalf = (y, margin = 40) => Math.max(0, Math.sqrt(R_PRINT * R_PRINT - (500 - y) * (500 - y)) - margin);

function titleBlock(main, sub, ink) {
	const out = [];
	const mainLines = wrap(main.toUpperCase(), sub ? 2 : 3);
	// Fit each line into the band above the hub, stacked upwards from its bottom.
	let bottom = sub ? 268 : 300;
	const parts = [];
	if (sub) {
		const subLines = wrap(sub, 2).reverse();
		for (const line of subLines) {
			const size = Math.min(46, (2 * chordHalf(bottom - 20)) / (measure(line, 480, 100) / 100));
			parts.push({ line, size, y: bottom, weight: 480, stretch: 100 });
			bottom -= size * 1.12;
		}
		bottom -= 10;
	}
	for (const line of mainLines.reverse()) {
		const y = bottom;
		const width = measure(line, 720, 125) / 100;
		let size = Math.min(mainLines.length === 1 ? 104 : 80, (2 * chordHalf(y - 50)) / width);
		size = Math.min(size, (2 * chordHalf(y - size * 0.8)) / width);
		parts.push({ line, size, y, weight: 720, stretch: 125 });
		bottom -= size * 0.98;
	}
	for (const p of parts)
		out.push(`<text x="500" y="${p.y.toFixed(1)}" text-anchor="middle" fill="${ink}" font-family="Archivo, ui-sans-serif, sans-serif" font-weight="${p.weight}" font-size="${p.size.toFixed(1)}" style="font-stretch:${p.stretch}%" letter-spacing="${p.stretch > 100 ? -0.01 * p.size : 0}">${esc(p.line)}</text>`);
	return out.join('');
}

function motif(kind, h, [bg, ink, accent], initial) {
	switch (kind) {
	case 'rings': {
		const cx = 640 + (h % 160), cy = 760;
		let s = '';
		for (let i = 0; i < 5; i++)
			s += `<circle cx="${cx}" cy="${cy}" r="${90 + i * 70}" fill="none" stroke="${accent}" stroke-width="${26 - i * 3}"/>`;
		return s;
	}
	case 'band':
		return `<rect x="0" y="600" width="1000" height="150" fill="${accent}"/><rect x="0" y="770" width="1000" height="6" fill="${ink}"/>`;
	case 'split':
		return `<path d="M0 ${560 + (h % 90)} L1000 ${380 + (h % 120)} L1000 1000 L0 1000 Z" fill="${accent}"/>`;
	case 'glyph':
		return `<text x="${560 + (h % 140)}" y="1080" text-anchor="middle" font-family="Archivo, ui-sans-serif, sans-serif" font-weight="800" font-size="760" style="font-stretch:125%" fill="${accent}">${esc(initial)}</text>`;
	case 'stripes': {
		let s = '';
		for (let i = -6; i < 14; i++)
			s += `<rect x="${i * 90}" y="560" width="34" height="700" fill="${accent}" transform="rotate(-28 ${i * 90} 560)"/>`;
		return s;
	}
	case 'sun':
	default:
		return `<circle cx="${380 + (h % 240)}" cy="830" r="260" fill="${accent}"/><rect x="0" y="826" width="1000" height="8" fill="${bg}"/><rect x="0" y="870" width="1000" height="14" fill="${bg}"/>`;
	}
}

// Text running along the bottom of the print, left to right.
function rimText(text, ink) {
	return `<path id="__rim" d="M ${500 - 432} 500 A 432 432 0 0 0 ${500 + 432} 500" fill="none"/>` +
		`<text font-family="Chivo Mono, ui-monospace, monospace" font-weight="500" font-size="27" letter-spacing="3" fill="${ink}"><textPath href="#__rim" startOffset="50%" text-anchor="middle">${esc(text)}</textPath></text>`;
}

let uid = 0;

// An <svg> for `disc`: { title, serial, region, media, cover (an object URL or null) }.
export function discSvg(disc) {
	const id = `d${++uid}`;
	const h = hash(disc.serial || disc.title);
	const pal = PALETTES[h % PALETTES.length];
	const kind = MOTIFS[(h >>> 8) % MOTIFS.length];
	const { main, sub, disc: discNo } = splitTitle(disc.title);
	const rim = [disc.serial, disc.region, disc.media && `${disc.media}-ROM`, discNo].filter(Boolean).join('  ·  ');

	let print;
	if (disc.cover) {
		print = `<rect width="1000" height="1000" fill="#1c1b19"/>` +
			`<image href="${esc(disc.cover)}" x="0" y="0" width="1000" height="1000" preserveAspectRatio="xMidYMid slice"/>`;
	} else {
		const initial = (main.match(/[A-Za-z0-9]/) || ['?'])[0].toUpperCase();
		print = `<rect width="1000" height="1000" fill="${pal[0]}"/>` + motif(kind, h, pal, initial) +
			titleBlock(main, sub, pal[1]) + rimText(rim, kind === 'band' || kind === 'split' || kind === 'sun' ? pal[0] : pal[1]);
	}
	// Ids must be unique in the document: rename the rim path per disc.
	print = print.replaceAll('__rim', `${id}r`);

	const svg = `<svg xmlns="${NS}" viewBox="0 0 1000 1000" class="disc-svg" aria-hidden="true" focusable="false">
<defs>
<clipPath id="${id}p"><path clip-rule="evenodd" d="M500 ${500 - R_PRINT}a${R_PRINT} ${R_PRINT} 0 1 1 0 ${2 * R_PRINT}a${R_PRINT} ${R_PRINT} 0 1 1 0 -${2 * R_PRINT}ZM500 ${500 - R_HUB}a${R_HUB} ${R_HUB} 0 1 0 0 ${2 * R_HUB}a${R_HUB} ${R_HUB} 0 1 0 0 -${2 * R_HUB}Z"/></clipPath>
</defs>
<path fill-rule="evenodd" fill="#8f8c86" d="M500 ${500 - R_EDGE}a${R_EDGE} ${R_EDGE} 0 1 1 0 ${2 * R_EDGE}a${R_EDGE} ${R_EDGE} 0 1 1 0 -${2 * R_EDGE}ZM500 ${500 - R_HUB}a${R_HUB} ${R_HUB} 0 1 0 0 ${2 * R_HUB}a${R_HUB} ${R_HUB} 0 1 0 0 -${2 * R_HUB}Z"/>
<path fill-rule="evenodd" fill="#d6d3cc" fill-opacity="0.34" d="M500 ${500 - R_HUB}a${R_HUB} ${R_HUB} 0 1 1 0 ${2 * R_HUB}a${R_HUB} ${R_HUB} 0 1 1 0 -${2 * R_HUB}ZM500 ${500 - R_HOLE}a${R_HOLE} ${R_HOLE} 0 1 0 0 ${2 * R_HOLE}a${R_HOLE} ${R_HOLE} 0 1 0 0 -${2 * R_HOLE}Z"/>
<circle cx="500" cy="500" r="138" fill="none" stroke="#ece8e1" stroke-opacity="0.32" stroke-width="5"/>
<circle cx="500" cy="500" r="${R_HOLE + 2}" fill="none" stroke="#ece8e1" stroke-opacity="0.4" stroke-width="3"/>
<g clip-path="url(#${id}p)">${print}</g>
<circle cx="500" cy="500" r="${R_HUB}" fill="none" stroke="#000" stroke-opacity="0.35" stroke-width="3"/>
<circle cx="500" cy="500" r="${R_EDGE - 1}" fill="none" stroke="#ece8e1" stroke-opacity="0.28" stroke-width="3"/>
</svg>`;
	const holder = document.createElement('div');
	holder.innerHTML = svg;
	return holder.firstElementChild;
}
