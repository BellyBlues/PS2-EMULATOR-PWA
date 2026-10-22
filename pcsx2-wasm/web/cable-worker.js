// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// TabStationToo cable worker: lays the controllers' cables on the floor under the console (UI-SPEC.md §17),
// off the page's thread. Each cable is a rope that matter-js (vendor/matter.min.js, MIT) lets settle: a chain of
// links, both cables with as many and as long, so both are the same length, longer than either needs; pinned at
// the plug (leaving downwards) and at the controller (entering from above), stiff over a few links so it bends
// gently. It is dropped in a few broad random curves along its straight run, as long as the rope, and settles
// from there. No gravity: it lies flat. It stays on the screen, between the console and its controller's top,
// and out of the other controller's way. Seeded, so the same layout always lies the same way.
//
// Message from the page: {id, runs, arts, box}
//   runs: per cable, its straight run from the plug to the controller as cubics [[p0, p1, p2, p3], ...]
//         (page coordinates), or null;
//   arts: per controller, its drawing's box {left, right, top, bottom};
//   avoid: rects to keep out of as well (the controllers' heading);
//   barBottom: the console's lower edge: only the first links (from the plug) may lie above it;
//   box: {left, right}, the window's sides less a margin.
//   params: optional, any of CABLE_DEFAULTS below (the page's cable tuner, ?tune-cables, sends them).
// Answer: {id, paths}, an SVG path per cable ('' for none).

importScripts('vendor/matter.min.js');

// How the cables lie. The page's cable tuner (?tune-cables) changes them live and copies them out.
const CABLE_DEFAULTS = {
	// Chosen by the owner with the tuner (2026-10-22).
	seed1: 24301, seed2: 32220, // each cable's random draw
	slack: 1.5, extra: 115, // the common length: the longer straight run times slack, plus extra px
	link: 9, // px between links: the cable is drawn through them
	bendMin: 0, bendMax: 0.34, // stiffness against bending over two links, at random between these per link
	bend4Min: 0.14, bend4Max: 0.09, // and over four links
	memory: 0.55, memoryWaves: 5, // how much it remembers being coiled, and how many changes along it
	dropWaves: 7, dropDetail: 1.4, // the drop's random curves: how many, and how fast the smaller ones fade
	friction: 0.12, steps: 380, // the air's drag on each link, and how long it settles
	smooth: 6, finalSmooth: 4, // smoothing passes after settling, and after the last steps
};
const CABLE_CLEAR = 10; // px kept above the controllers' drawings
const CABLE_LANE = 0.11; // either side of where a cable goes in, as a share of its controller's width: its way in, between the shoulders
let P = CABLE_DEFAULTS;

const lerp = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t];

function cubicPoint(c, t) {
	const [p0, p1, p2, p3] = c;
	const a = lerp(p0, p1, t), b = lerp(p1, p2, t), d = lerp(p2, p3, t);
	return lerp(lerp(a, b, t), lerp(b, d, t), t);
}

// A run of cubics as points about 1 px apart.
function runPoints(run) {
	const pts = [run[0][0]];
	for (const c of run) {
		const n = Math.max(8, Math.ceil(Math.hypot(c[3][0] - c[0][0], c[3][1] - c[0][1]) * 1.5));
		for (let i = 1; i <= n; i++)
			pts.push(cubicPoint(c, i / n));
	}
	return pts;
}

function polyLength(pts) {
	let len = 0;
	for (let i = 1; i < pts.length; i++)
		len += Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]);
	return len;
}

// The point at a distance along points.
function pointAlong(pts, at) {
	let left = at;
	for (let i = 1; i < pts.length; i++) {
		const step = Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]);
		if (left <= step)
			return lerp(pts[i - 1], pts[i], step ? left / step : 0);
		left -= step;
	}
	return pts[pts.length - 1];
}

// mulberry32: a small seeded generator, so a cable always lies the same way.
function seeded(seed) {
	let a = seed >>> 0;
	return () => {
		a = (a + 0x6d2b79f5) >>> 0;
		let t = a;
		t = Math.imul(t ^ (t >>> 15), t | 1);
		t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
		return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
	};
}

// Lays a rope of links links along a straight run and lets it settle, held only at its two ends (the plug, and
// the controller's strain relief). box: where the rest may lie; keepOut: rects it is pushed out of, sideways (up, those marked up).
// Returns the links' places.
function settleRope(run, links, seed, box, keepOut) {
	const M = self.Matter;
	const pts = runPoints(run);
	const runLen = polyLength(pts);
	const rnd = seeded(seed);
	// The drop: the run pushed out to its side by random waves, broad ones and smaller ones on them, as far as
	// makes it the rope's length.
	const waves = [1, 2, 3, 4, 5, 6, 8, 10, 12].slice(0, P.dropWaves).map((k) => ({ k, amp: (rnd() - 0.5) * 2 * k ** -P.dropDetail, phase: rnd() * Math.PI * 2 }));
	const normals = pts.map((p, i) => {
		const a = pts[Math.max(0, i - 2)], b = pts[Math.min(pts.length - 1, i + 2)];
		const m = Math.hypot(b[0] - a[0], b[1] - a[1]) || 1;
		return [(b[1] - a[1]) / m, -(b[0] - a[0]) / m];
	});
	const shape = (amp) => pts.map(([x, y], i) => {
		const u = i / (pts.length - 1);
		const off = amp * waves.reduce((n, w) => n + w.amp * Math.sin(w.k * Math.PI * u + w.phase), 0) * Math.sin(Math.PI * u);
		return [x + off * normals[i][0], y + off * normals[i][1]];
	});
	const ropeLen = (links - 1) * P.link;
	let lo = 0, hi = 600;
	for (let k = 0; k < 30; k++) {
		const mid = (lo + hi) / 2;
		if (polyLength(shape(mid)) < ropeLen)
			lo = mid;
		else
			hi = mid;
	}
	const drop = shape(lo);
	const engine = M.Engine.create({ gravity: { x: 0, y: 0, scale: 0 } });
	const bodies = [];
	const a = run[0][0], z = run[run.length - 1][3];
	for (let i = 0; i < links; i++) {
		const [x, y] = i === 0 ? a : i === links - 1 ? z : pointAlong(drop, i * P.link);
		bodies.push(M.Bodies.circle(x, y, 2, { isStatic: i === 0 || i === links - 1, frictionAir: P.friction, collisionFilter: { group: -1 } }));
	}
	const constraints = [];
	for (let i = 1; i < links; i++)
		constraints.push(M.Constraint.create({ bodyA: bodies[i - 1], bodyB: bodies[i], length: P.link, stiffness: 1, damping: 0.05 }));
	// Stiffness against bending, over two and over four links. A real cable remembers being coiled: each stretch
	// of it would rather bend a little (memory, a random bend per link, varying smoothly along it), and some
	// stretches are stiffer than others. Without that, every turn settles into the same perfect arc.
	const memoryWaves = [2, 3, 4, 6, 8, 10, 12, 15].slice(0, P.memoryWaves).map((k) => ({ k, amp: (rnd() - 0.5) * P.memory * k ** -0.6, phase: rnd() * Math.PI * 2 }));
	const memory = (i) => Math.min(0.4, Math.abs(memoryWaves.reduce((n, w) => n + w.amp * Math.sin(w.k * Math.PI * i / links + w.phase), 0)));
	for (let i = 2; i < links; i++) {
		const t = memory(i - 1);
		constraints.push(M.Constraint.create({ bodyA: bodies[i - 2], bodyB: bodies[i], length: 2 * P.link * Math.cos(t / 2),
			stiffness: P.bendMin + (P.bendMax - P.bendMin) * rnd(), damping: 0.05 }));
	}
	for (let i = 4; i < links; i++) {
		const t = memory(i - 2);
		constraints.push(M.Constraint.create({ bodyA: bodies[i - 4], bodyB: bodies[i],
			length: t > 1e-3 ? P.link * Math.sin(2 * t) / Math.sin(t / 2) : 4 * P.link, stiffness: P.bend4Min + (P.bend4Max - P.bend4Min) * rnd(), damping: 0.05 }));
	}
	M.Composite.add(engine.world, [...bodies, ...constraints]);
	// Back where it may lie: in the box, out of the keep-out rects (sideways, or up out of those marked up).
	const hold = () => {
		bodies.forEach((b, i) => {
			if (b.isStatic)
				return;
			let { x, y } = b.position;
			x = Math.min(box.right, Math.max(box.left, x));
			y = Math.min(box.bottom, Math.max(i <= (box.near || 0) ? box.nearTop : box.top, y));
			for (const r of keepOut) {
				if (x > r.left && x < r.right && y > r.top && y < r.bottom) {
					if (r.up)
						y = r.top;
					else
						x = x - r.left < r.right - x ? r.left : r.right;
				}
			}
			if (x !== b.position.x || y !== b.position.y)
				M.Body.setPosition(b, { x, y }, false);
		});
	};
	for (let step = 0; step < P.steps; step++) {
		M.Engine.update(engine, 1000 / 60);
		hold();
	}
	// The links' own little kinks smoothed out (the pinned ends stay), then a few steps more so that every link is
	// its length again: both cables as long.
	let out = bodies.map((b) => [b.position.x, b.position.y]);
	for (let pass = 0; pass < P.smooth; pass++) {
		out = out.map((p, i) => (bodies[i].isStatic ? p :
			[(out[i - 1][0] + 2 * p[0] + out[i + 1][0]) / 4, (out[i - 1][1] + 2 * p[1] + out[i + 1][1]) / 4]));
	}
	bodies.forEach((b, i) => {
		if (!b.isStatic) {
			M.Body.setPosition(b, { x: out[i][0], y: out[i][1] }, false);
			M.Body.setVelocity(b, { x: 0, y: 0 });
		}
	});
	for (let step = 0; step < 12; step++) {
		M.Engine.update(engine, 1000 / 60);
		hold();
	}
	// Smoothed once more where the window's sides held it (they leave it jagged); toLength restores the length.
	let rope = bodies.map((b) => [b.position.x, b.position.y]);
	for (let pass = 0; pass < P.finalSmooth; pass++) {
		rope = rope.map((p, i) => (bodies[i].isStatic ? p :
			[(rope[i - 1][0] + 2 * p[0] + rope[i + 1][0]) / 4, (rope[i - 1][1] + 2 * p[1] + rope[i + 1][1]) / 4]));
	}
	return rope;
}

// The rope made exactly length long: its way out from the straight run scaled by a hair (the ends stay).
function toLength(rope, run, length) {
	const pts = runPoints(run);
	const runLen = polyLength(pts);
	const along = rope.map((_, i) => pointAlong(pts, runLen * i / (rope.length - 1)));
	const scaled = (k) => rope.map((p, i) => [along[i][0] + (p[0] - along[i][0]) * k, along[i][1] + (p[1] - along[i][1]) * k]);
	let lo = 0.9, hi = 1.1;
	for (let n = 0; n < 40; n++) {
		const mid = (lo + hi) / 2;
		if (polyLength(scaled(mid)) < length)
			lo = mid;
		else
			hi = mid;
	}
	return scaled((lo + hi) / 2);
}

self.onmessage = (e) => {
	const { id, runs, arts, avoid, barBottom, box, params } = e.data;
	P = { ...CABLE_DEFAULTS, ...(params || {}) };
	const lengths = runs.map((run) => (run ? polyLength(runPoints(run)) : 0));
	const links = Math.round((Math.max(...lengths) * P.slack + P.extra) / P.link) + 1;
	const ropes = runs.map((run, i) => {
		if (!run)
			return null;
		const end = run[run.length - 1][3];
		const own = arts[i];
		const lane = (own.right - own.left) * CABLE_LANE;
		// From just under the console down to where it goes in; off its own controller (its shoulders and their
		// labels) but for a lane down the middle, and out of the other controller's way.
		// The first links may lie on the console's face (from the plug's centre); the rest only below it.
		const near = Math.ceil(((barBottom || run[0][0][1]) - run[0][0][1]) / P.link) + 1;
		const area = { left: box.left, right: box.right, top: (barBottom || run[0][0][1]) + 1, nearTop: run[0][0][1] + 1, near, bottom: end[1] };
		const keepOut = [
			{ ...own, top: own.top - CABLE_CLEAR, right: end[0] - lane, up: true },
			{ ...own, top: own.top - CABLE_CLEAR, left: end[0] + lane, up: true },
			...arts.filter((_, j) => j !== i).map((r) => ({ ...r, top: r.top - CABLE_CLEAR })),
			...(avoid || []),
		];
		return settleRope(run, links, i === 0 ? P.seed1 : P.seed2, area, keepOut);
	});
	// The links come out within a hair of their length; both cables are made exactly the same.
	const settled = ropes.filter(Boolean);
	const common = settled.reduce((n, pts) => n + polyLength(pts), 0) / (settled.length || 1);
	const paths = ropes.map((pts, i) => (pts ?
		'M' + toLength(pts, runs[i], common).map(([x, y]) => `${x.toFixed(2)} ${y.toFixed(2)}`).join(' L') : ''));
	self.postMessage({ id, paths });
};
