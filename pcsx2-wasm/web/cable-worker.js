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
//   box: {left, right}, the window's sides less a margin.
// Answer: {id, paths}, an SVG path per cable ('' for none).

importScripts('vendor/matter.min.js');

const CABLE_LINK = 5; // px between links: the cable is drawn through them
const CABLE_SLACK = 1.15; // the common length, against the longer of the two straight runs ...
const CABLE_EXTRA = 70; // ... plus this, in px
const CABLE_STEPS = 260; // simulation steps to settle

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

// Lays a rope of links links along a straight run and lets it settle. box: where it may lie; keepOut: rects it
// is pushed out of, sideways. Returns the links' places.
function settleRope(run, links, seed, box, keepOut) {
	const M = self.Matter;
	const pts = runPoints(run);
	const runLen = polyLength(pts);
	const rnd = seeded(seed);
	// The drop: the run swayed sideways by a few broad random waves, as far as makes it the rope's length.
	const waves = [1, 2, 3].map((k) => ({ k, amp: (rnd() - 0.5) * 2 / k, phase: rnd() * Math.PI * 2 }));
	const shape = (amp) => pts.map(([x, y], i) => {
		const u = i / (pts.length - 1);
		const off = waves.reduce((n, w) => n + w.amp * Math.sin(w.k * Math.PI * u + w.phase), 0) * Math.sin(Math.PI * u);
		return [x + amp * off, y];
	});
	const ropeLen = (links - 1) * CABLE_LINK;
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
		const [x, y] = i === 0 ? a : i === 1 ? [a[0], a[1] + CABLE_LINK] : i === links - 2 ? [z[0], z[1] - CABLE_LINK] :
			i === links - 1 ? z : pointAlong(drop, i * CABLE_LINK);
		bodies.push(M.Bodies.circle(x, y, 2, { isStatic: i < 2 || i > links - 3, frictionAir: 0.12, collisionFilter: { group: -1 } }));
	}
	const constraints = [];
	for (let i = 1; i < links; i++)
		constraints.push(M.Constraint.create({ bodyA: bodies[i - 1], bodyB: bodies[i], length: CABLE_LINK, stiffness: 1, damping: 0.05 }));
	// Stiffness against bending, over two and over four links.
	for (let i = 2; i < links; i++)
		constraints.push(M.Constraint.create({ bodyA: bodies[i - 2], bodyB: bodies[i], length: CABLE_LINK * 2, stiffness: 0.3, damping: 0.05 }));
	for (let i = 4; i < links; i++)
		constraints.push(M.Constraint.create({ bodyA: bodies[i - 4], bodyB: bodies[i], length: CABLE_LINK * 4, stiffness: 0.06, damping: 0.05 }));
	M.Composite.add(engine.world, [...bodies, ...constraints]);
	for (let step = 0; step < CABLE_STEPS; step++) {
		M.Engine.update(engine, 1000 / 60);
		for (const b of bodies) {
			if (b.isStatic)
				continue;
			let { x, y } = b.position;
			x = Math.min(box.right, Math.max(box.left, x));
			y = Math.min(box.bottom, Math.max(box.top, y));
			for (const r of keepOut) {
				if (x > r.left && x < r.right && y > r.top && y < r.bottom)
					x = x - r.left < r.right - x ? r.left : r.right;
			}
			if (x !== b.position.x || y !== b.position.y)
				M.Body.setPosition(b, { x, y }, false);
		}
	}
	// The links' own little kinks smoothed out (the pinned ends stay), then a few steps more so that every link is
	// its length again: both cables as long.
	let out = bodies.map((b) => [b.position.x, b.position.y]);
	for (let pass = 0; pass < 3; pass++) {
		out = out.map((p, i) => (i < 2 || i > out.length - 3 ? p :
			[(out[i - 1][0] + 2 * p[0] + out[i + 1][0]) / 4, (out[i - 1][1] + 2 * p[1] + out[i + 1][1]) / 4]));
	}
	bodies.forEach((b, i) => {
		if (!b.isStatic) {
			M.Body.setPosition(b, { x: out[i][0], y: out[i][1] }, false);
			M.Body.setVelocity(b, { x: 0, y: 0 });
		}
	});
	for (let step = 0; step < 12; step++)
		M.Engine.update(engine, 1000 / 60);
	return bodies.map((b) => [b.position.x, b.position.y]);
}

self.onmessage = (e) => {
	const { id, runs, arts, box } = e.data;
	const lengths = runs.map((run) => (run ? polyLength(runPoints(run)) : 0));
	const links = Math.round((Math.max(...lengths) * CABLE_SLACK + CABLE_EXTRA) / CABLE_LINK) + 1;
	const paths = runs.map((run, i) => {
		if (!run)
			return '';
		const end = run[run.length - 1][3];
		const area = { left: box.left, right: box.right, top: run[0][0][1] + CABLE_LINK, bottom: end[1] - CABLE_LINK };
		// Out of the other controller's way, and off its own but where it goes in.
		const keepOut = arts.map((r, j) => (j === i ? { ...r, top: end[1] + CABLE_LINK } : { ...r, top: r.top - CABLE_LINK }));
		const pts = settleRope(run, links, 0x5eed + i * 7919, area, keepOut);
		return 'M' + pts.map(([x, y]) => `${x.toFixed(2)} ${y.toFixed(2)}`).join(' L');
	});
	self.postMessage({ id, paths });
};
