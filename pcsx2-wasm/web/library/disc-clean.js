// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The pieces of the disc library's Easter egg (disc-library.js, cleanDisc): now and then a disc won't
// read, so it comes back out, is turned over to its data side, sprayed and wiped clean from the centre
// out, as discs were. Here: the data side with its dirt, the spray bottle and the cloth, all drawn
// in-house. The motion is in disc-library.js.

const NS = 'http://www.w3.org/2000/svg';
const R_HOLE = 62;
const R_DATA_IN = 200; // dirt and drops stay on the data area, between this and R_DATA_OUT
const R_DATA_OUT = 470;

// A small seeded generator, so a disc's dirt is the same each time it is cleaned.
function rng(seed) {
	let s = seed >>> 0 || 1;
	return () => {
		s ^= s << 13;
		s ^= s >>> 17;
		s ^= s << 5;
		return (s >>> 0) / 4294967296;
	};
}

// A point on the data area: [x, y, angle in degrees].
function spot(r) {
	const a = r() * Math.PI * 2;
	const d = R_DATA_IN + r() * (R_DATA_OUT - R_DATA_IN);
	return [500 + Math.cos(a) * d, 500 + Math.sin(a) * d, (a * 180) / Math.PI];
}

// The data side of a disc, on the same 1000-unit square as its label: { el, dirt, drops }, where dirt
// and drops are [element, angle] pairs (the wipes clear them by angle).
export function makeDataSide(seed) {
	const r = rng(seed);
	const el = document.createElement('div');
	el.className = 'lib-data';
	const svg = document.createElementNS(NS, 'svg');
	svg.setAttribute('viewBox', '0 0 1000 1000');
	svg.setAttribute('class', 'lib-dirt');
	svg.innerHTML = '<defs><filter id="lib-smudge" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="14"/></filter>' +
		'<filter id="lib-print" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="1.6"/></filter></defs>';
	const dirt = [];
	const add = (node, angle) => {
		svg.append(node);
		dirt.push([node, angle]);
	};
	const make = (tag, attrs) => {
		const n = document.createElementNS(NS, tag);
		for (const [k, v] of Object.entries(attrs))
			n.setAttribute(k, v);
		return n;
	};
	// Smudges.
	for (let i = 0; i < 6; i++) {
		const [x, y, a] = spot(r);
		add(make('ellipse', {
			cx: x, cy: y, rx: 50 + r() * 80, ry: 26 + r() * 40, fill: 'rgba(70, 60, 50, 0.32)',
			filter: 'url(#lib-smudge)', transform: `rotate(${(r() * 180).toFixed(1)} ${x} ${y})`,
		}), a);
	}
	// Fingerprints: a few rings of a whorl.
	for (let i = 0; i < 2; i++) {
		const [x, y, a] = spot(r);
		const g = make('g', { filter: 'url(#lib-print)', transform: `rotate(${(r() * 180).toFixed(1)} ${x} ${y})` });
		for (let k = 1; k <= 7; k++)
			g.append(make('ellipse', { cx: x, cy: y, rx: k * 9, ry: k * 6.5, fill: 'none', stroke: 'rgba(60, 50, 40, 0.38)', 'stroke-width': 3.2 }));
		add(g, a);
	}
	// Dust.
	for (let i = 0; i < 28; i++) {
		const [x, y, a] = spot(r);
		add(make('circle', { cx: x, cy: y, r: 2 + r() * 4.5, fill: 'rgba(50, 45, 40, 0.65)' }), a);
	}
	// A hair.
	{
		const [x, y, a] = spot(r);
		add(make('path', {
			d: `M${x} ${y} q ${40 + r() * 40} ${-30 + r() * 60} ${90 + r() * 50} ${-10 + r() * 40}`,
			fill: 'none', stroke: 'rgba(40, 35, 30, 0.55)', 'stroke-width': 2.2, 'stroke-linecap': 'round',
		}), a);
	}
	// The spray's drops, hidden until it sprays.
	const drops = [];
	for (let i = 0; i < 26; i++) {
		const [x, y, a] = spot(r);
		const d = make('circle', {
			cx: x, cy: y, r: 4 + r() * 9, fill: 'rgba(225, 238, 244, 0.7)', stroke: 'rgba(255, 255, 255, 0.85)', 'stroke-width': 1.5, opacity: 0,
		});
		svg.append(d);
		drops.push([d, a]);
	}
	const glint = document.createElement('div');
	glint.className = 'lib-glint';
	el.append(svg, glint);
	return { el, dirt, drops, glint };
}

// The spray bottle, nozzle to the left: its tip is at NOZZLE in the drawing's own units (120 × 260).
export const BOTTLE_SIZE = [120, 260];
export const NOZZLE = [6, 66];
export const BOTTLE_SVG = `<svg viewBox="0 0 120 260" aria-hidden="true" focusable="false">
<rect x="22" y="112" width="78" height="144" rx="16" fill="#e7e2d8"/>
<rect x="28" y="150" width="66" height="100" rx="11" fill="#a9d4cb" fill-opacity="0.6"/>
<rect x="22" y="112" width="12" height="144" rx="6" fill="#fff" fill-opacity="0.25"/>
<rect x="46" y="86" width="30" height="30" rx="4" fill="#d6d0c5"/>
<path d="M104 46 H44 Q30 46 30 58 V62 H16 V58 H6 V74 H16 V70 H30 Q32 90 48 90 H104 Z" fill="#2b2826"/>
<path class="lib-trigger" d="M60 90 Q52 112 64 130 L73 127 Q65 110 72 92 Z" fill="#2b2826"/>
</svg>`;

// The cloth: a soft square of microfibre, folded once.
export const CLOTH_SIZE = [190, 156];
export const CLOTH_SVG = `<svg viewBox="0 0 220 180" aria-hidden="true" focusable="false">
<path d="M30 40 Q20 10 70 18 Q120 2 170 22 Q215 30 202 80 Q214 130 176 158 Q120 182 70 166 Q16 152 22 100 Q10 70 30 40 Z" fill="#d9d2c4"/>
<path d="M58 52 Q110 72 162 56 M46 100 Q112 122 178 100 M70 142 Q116 154 160 136" fill="none" stroke="#bdb3a1" stroke-width="7" stroke-linecap="round"/>
<path d="M34 46 Q26 20 70 26 Q120 10 166 30 Q206 38 194 80 Q204 126 170 150 Q120 172 72 158 Q26 146 30 100 Q20 72 34 46 Z" fill="none" stroke="#c9c0ae" stroke-width="2" stroke-dasharray="5 6"/>
</svg>`;
