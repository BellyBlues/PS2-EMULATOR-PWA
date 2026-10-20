// SPDX-FileCopyrightText: 2026 TabStationToo contributors
// SPDX-License-Identifier: MIT

// TabStationToo's benchmark: a PS2 program with no menu and no input that draws a crowd of
// animated figures the way games draw characters, with the extra work games do around them.
//
// Each frame:
// - the EE runs per-figure game logic (dust particles, path searches on a grid), poses every
//   figure's skeleton (VU0 macro-mode matrix products) and builds one VIF1 DMA chain;
// - VU1 (bench_vu1.vsm) transforms, lights and projects the figures and the floor and kicks them
//   to the GS: textured, Gouraud-shaded, Z-buffered triangle strips, see-through shadows;
// - each figure uses its own palette (an 8-bit texture with a CLUT load): every VU1 batch sends
//   its TEX0 ahead of its primitives, as games switch textures inside their VU1 packets;
// - near figures kick up dust, drawn as sprites the EE positioned, in one PATH2 packet;
// - far figures use simpler limbs (level of detail), and path searches are spread over 8 frames,
//   so 300 figures still fit in one frame on a PS2;
// - the texture and its four palettes are uploaded again every frame through PATH3;
// - a glow pass renders the frame into a half-size target, blurs it twice and adds it back.
//
// The number of figures steps through PHASE_COUNTS, PHASE_FRAMES frames each, and the program
// prints "BENCH phase N=<figures> frame=<frame>" at every step so a runner can tell the phases
// apart. It draws at most one frame per vsync and sleeps on interrupts while it waits, like a game.

#include <dma.h>
#include <draw.h>
#include <gif_tags.h>
#include <graph.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include <kernel.h>
#include <malloc.h>
#include <math.h>
#include <math3d.h>
#include <packet2.h>
#include <packet2_utils.h>
#include <stdio.h>
#include <string.h>
#include <tamtypes.h>

extern u32 BenchVU1_CodeStart __attribute__((section(".vudata")));
extern u32 BenchVU1_CodeEnd __attribute__((section(".vudata")));

#define SCREEN_W 640
#define SCREEN_H 512
#define GLOW_W 320
#define GLOW_H 256
#define OFFSET_X (2048 - SCREEN_W / 2)
#define OFFSET_Y (2048 - SCREEN_H / 2)
#define MAX_FIGURES 300
#define NEAR_FIGURES 50 // full detail and dust; the rest of the crowd uses simpler limbs
#define PHASE_FRAMES 300
static const int PHASE_COUNTS[] = {1, 100, 200, 300};
#define PHASE_TOTAL ((int)(sizeof(PHASE_COUNTS) / sizeof(PHASE_COUNTS[0])))

// VU1 memory: two buffers of 496 quadwords from address 8. A batch needs 13 header quadwords,
// 3 per input vertex, 2 for the TEX0 packet, 1 for the GIF tag and 3 per output vertex
// (bench_vu1.vsm).
#define VU_HEADER_QW 13
#define MAX_BATCH_VERTS 80
#define MAX_BATCHES 16

// Z = ZK / depth, stored by FTOI4 (x16) in a 16-bit Z buffer: the nearest drawn depth (3.0)
// gets the largest value.
#define ZK (65535.0f / 16.0f * 3.0f)
#define FOCAL_X 554.0f // 60 degrees across 640 pixels
#define FOCAL_Y 591.0f // PAL pixels are taller than wide

#define PARTICLES 24
#define GRID 16

typedef struct
{
	int batches;
	int verts[MAX_BATCHES];
	qword_t* data[MAX_BATCHES]; // 3 quadwords per vertex, as bench_vu1.vsm reads them
	qword_t* tags[2]; // per batch: count and GIF tag (2 quadwords), for each primitive kind
} mesh_t;

enum
{
	KIND_LIT_TEXTURED,
	KIND_SHADOW,
};

typedef struct
{
	int parent;
	float offset[3]; // joint position in the parent's space
	float centre[3]; // mesh centre in the bone's space
	float scale[3]; // ellipsoid radii
	float swing; // animation amplitude around X, radians
	float phase;
} bone_t;

// A rough humanoid, 1.8 units tall, standing on y = 0.
static const bone_t SKELETON[] = {
	{-1, {0.00f, 1.00f, 0.00f}, {0.00f, 0.00f, 0.00f}, {0.18f, 0.12f, 0.11f}, 0.05f, 0.0f}, // pelvis
	{0, {0.00f, 0.08f, 0.00f}, {0.00f, 0.26f, 0.00f}, {0.20f, 0.28f, 0.12f}, 0.10f, 1.0f}, // chest
	{1, {0.00f, 0.56f, 0.00f}, {0.00f, 0.12f, 0.00f}, {0.11f, 0.13f, 0.11f}, 0.20f, 2.0f}, // head
	{1, {0.25f, 0.48f, 0.00f}, {0.00f, -0.15f, 0.00f}, {0.06f, 0.16f, 0.06f}, 0.90f, 0.0f}, // left upper arm
	{3, {0.00f, -0.30f, 0.00f}, {0.00f, -0.14f, 0.00f}, {0.05f, 0.15f, 0.05f}, 0.70f, 0.5f}, // left forearm
	{4, {0.00f, -0.29f, 0.00f}, {0.00f, -0.05f, 0.00f}, {0.04f, 0.06f, 0.03f}, 0.30f, 1.0f}, // left hand
	{1, {-0.25f, 0.48f, 0.00f}, {0.00f, -0.15f, 0.00f}, {0.06f, 0.16f, 0.06f}, 0.90f, 3.14f}, // right upper arm
	{6, {0.00f, -0.30f, 0.00f}, {0.00f, -0.14f, 0.00f}, {0.05f, 0.15f, 0.05f}, 0.70f, 3.6f}, // right forearm
	{7, {0.00f, -0.29f, 0.00f}, {0.00f, -0.05f, 0.00f}, {0.04f, 0.06f, 0.03f}, 0.30f, 4.1f}, // right hand
	{0, {0.10f, -0.05f, 0.00f}, {0.00f, -0.22f, 0.00f}, {0.08f, 0.23f, 0.08f}, 0.60f, 3.14f}, // left thigh
	{9, {0.00f, -0.45f, 0.00f}, {0.00f, -0.21f, 0.00f}, {0.065f, 0.22f, 0.065f}, 0.50f, 2.4f}, // left shin
	{10, {0.00f, -0.43f, 0.00f}, {0.00f, -0.03f, 0.05f}, {0.05f, 0.04f, 0.10f}, 0.20f, 2.0f}, // left foot
	{0, {-0.10f, -0.05f, 0.00f}, {0.00f, -0.22f, 0.00f}, {0.08f, 0.23f, 0.08f}, 0.60f, 0.0f}, // right thigh
	{12, {0.00f, -0.45f, 0.00f}, {0.00f, -0.21f, 0.00f}, {0.065f, 0.22f, 0.065f}, 0.50f, -0.7f}, // right shin
	{13, {0.00f, -0.43f, 0.00f}, {0.00f, -0.03f, 0.05f}, {0.05f, 0.04f, 0.10f}, 0.20f, -1.1f}, // right foot
};
#define BONES ((int)(sizeof(SKELETON) / sizeof(SKELETON[0])))

// Base hues of the four palettes (the texture modulates a light grey vertex colour).
static const int PALETTE_HUES[4][3] = {{255, 140, 70}, {80, 170, 255}, {230, 220, 90}, {190, 100, 240}};

// Per-frame data the DMA chain points at, one set per frame in flight.
// Each block: ambient colour, three light rows, screen offset, matrix, the TEX0 packet (11
// quadwords, VU1 addresses 2 to 12).
#define BLOCK_QW 11
#define MAX_BLOCKS (MAX_FIGURES * (BONES + 1) + 1)
// The dust of the near figures, one GIF packet sent through PATH2.
#define DIRECT_QW (1 + 3 * PARTICLES * NEAR_FIGURES)
static qword_t* s_blocks[2];
static qword_t* s_direct[2];
static packet2_t* s_chain[2];
static packet2_t* s_gs_setup;
static packet2_t* s_uploads;
static qword_t* s_post[2]; // the glow pass for each draw buffer
static int s_post_qw;

static mesh_t s_ellipsoid;
static mesh_t s_ellipsoid_far;
static mesh_t s_floor;
static MATRIX s_view_screen;
static float s_light[3];

static framebuffer_t s_frame[2];
static zbuffer_t s_z;
static texbuffer_t s_floor_tex;
static clutbuffer_t s_no_clut;
static lod_t s_lod;
static blend_t s_blend;
static texwrap_t s_wrap;
static u32 s_glow[2]; // glow targets' VRAM addresses
static u32 s_figure_tex; // 128 x 128, 8-bit indices
static u32 s_clut[4]; // 16 x 16 32-bit, one palette each
static u8* s_figure_pixels;
static u32* s_palettes; // 4 x 256 entries, rebuilt every frame

// ------------------------------------------------------------------ GIF packet helpers

static qword_t* gif_ad_tag(qword_t* q, int regs, int eop)
{
	q->dw[0] = GIF_SET_TAG(regs, eop, 0, 0, GIF_FLG_PACKED, 1);
	q->dw[1] = GIF_REG_AD;
	return q + 1;
}

static qword_t* gif_ad(qword_t* q, u64 reg, u64 value)
{
	q->dw[0] = value;
	q->dw[1] = reg;
	return q + 1;
}

// A sprite from pixel (x0, y0) to (x1, y1) of the current target, textured from texel (u0, v0)
// to (u1, v1) unless untextured.
static qword_t* gif_sprite(qword_t* q, int textured, int blend, float x0, float y0, float x1, float y1, float u0,
	float v0, float u1, float v1, int r, int g, int b)
{
	const u64 prim = GS_SET_PRIM(PRIM_SPRITE, 0, textured, 0, blend, 0, 1, 0, 0);
	q->dw[0] = GIF_SET_TAG(1, 1, 1, prim, GIF_FLG_PACKED, 5);
	q->dw[1] = ((u64)GIF_REG_RGBAQ) | ((u64)GIF_REG_UV << 4) | ((u64)GIF_REG_XYZ2 << 8) | ((u64)GIF_REG_UV << 12) |
			   ((u64)GIF_REG_XYZ2 << 16);
	q++;
	q->sw[0] = r;
	q->sw[1] = g;
	q->sw[2] = b;
	q->sw[3] = 0x80;
	q++;
	q->sw[0] = (int)(u0 * 16.0f);
	q->sw[1] = (int)(v0 * 16.0f);
	q->sw[2] = q->sw[3] = 0;
	q++;
	q->sw[0] = (int)((x0 + OFFSET_X) * 16.0f);
	q->sw[1] = (int)((y0 + OFFSET_Y) * 16.0f);
	q->sw[2] = q->sw[3] = 0;
	q++;
	q->sw[0] = (int)(u1 * 16.0f);
	q->sw[1] = (int)(v1 * 16.0f);
	q->sw[2] = q->sw[3] = 0;
	q++;
	q->sw[0] = (int)((x1 + OFFSET_X) * 16.0f);
	q->sw[1] = (int)((y1 + OFFSET_Y) * 16.0f);
	q->sw[2] = q->sw[3] = 0;
	q++;
	return q;
}

static int log2_ceil(int v)
{
	int n = 0;
	while ((1 << n) < v)
		n++;
	return n;
}

// ------------------------------------------------------------------ meshes

typedef struct
{
	mesh_t* mesh;
	qword_t* write;
	int strip_start;
} mesh_builder_t;

static void mesh_begin(mesh_builder_t* b, mesh_t* mesh)
{
	memset(mesh, 0, sizeof(*mesh));
	b->mesh = mesh;
	b->write = NULL;
}

// Starts a strip of n vertices, opening a new batch when it would not fit.
static void mesh_strip(mesh_builder_t* b, int n)
{
	mesh_t* m = b->mesh;
	if (m->batches == 0 || m->verts[m->batches - 1] + n > MAX_BATCH_VERTS)
	{
		m->data[m->batches] = (qword_t*)memalign(64, sizeof(qword_t) * 3 * MAX_BATCH_VERTS);
		m->verts[m->batches] = 0;
		m->batches++;
	}
	b->write = m->data[m->batches - 1] + 3 * m->verts[m->batches - 1];
	b->strip_start = m->verts[m->batches - 1];
}

static void mesh_vertex(mesh_builder_t* b, float x, float y, float z, float nx, float ny, float nz, float s, float t)
{
	mesh_t* m = b->mesh;
	const int index = m->verts[m->batches - 1]++;
	float* v = (float*)b->write;
	v[0] = x;
	v[1] = y;
	v[2] = z;
	v[3] = 1.0f;
	v[4] = nx;
	v[5] = ny;
	v[6] = nz;
	v[7] = (index - b->strip_start < 2) ? 32768.0f : 0.0f; // ADC: no triangle until the strip has 3 vertices
	v[8] = s;
	v[9] = t;
	v[10] = 1.0f;
	v[11] = 0.0f;
	b->write += 3;
}

static void mesh_finish(mesh_t* m)
{
	for (int kind = 0; kind < 2; kind++)
	{
		const int textured = (kind == KIND_LIT_TEXTURED);
		m->tags[kind] = (qword_t*)memalign(64, sizeof(qword_t) * 2 * m->batches);
		for (int i = 0; i < m->batches; i++)
		{
			qword_t* q = m->tags[kind] + 2 * i;
			memset(q, 0, sizeof(qword_t) * 2);
			q[0].sw[0] = m->verts[i];
			const u64 prim = GS_SET_PRIM(PRIM_TRIANGLE_STRIP, 1, textured, 0, 1, 0, 0, 0, 0);
			q[1].dw[0] = GIF_SET_TAG(m->verts[i], 1, 1, prim, GIF_FLG_PACKED, 3);
			q[1].dw[1] = DRAW_STQ2_REGLIST;
		}
	}
	FlushCache(0);
}

// A unit sphere as latitude strips: 4 x 9 segments (72 triangles, 80 vertices: one VU1 batch)
// for near figures, 2 x 6 (24 triangles, 28 vertices) for far ones.
static void build_sphere(mesh_t* mesh, int bands, int segments)
{
	mesh_builder_t b;
	mesh_begin(&b, mesh);
	for (int band = 0; band < bands; band++)
	{
		mesh_strip(&b, 2 * (segments + 1));
		for (int i = 0; i <= segments; i++)
		{
			const float phi = 6.2831853f * i / segments;
			for (int k = 0; k < 2; k++)
			{
				const float theta = 3.1415927f * (band + k) / bands;
				const float x = sinf(theta) * cosf(phi), y = cosf(theta), z = sinf(theta) * sinf(phi);
				mesh_vertex(&b, x, y, z, x, y, z, 2.0f * i / segments, 2.0f * (band + k) / bands);
			}
		}
	}
	mesh_finish(mesh);
}

static void build_ellipsoid(void)
{
	build_sphere(&s_ellipsoid, 4, 9);
	build_sphere(&s_ellipsoid_far, 2, 6);
}

// The floor: 16 x 16 tiles, 512 triangles.
static void build_floor(void)
{
	const int tiles = 16;
	const float x0 = -7.5f, x1 = 7.5f, z0 = -4.0f, z1 = 13.0f;
	mesh_builder_t b;
	mesh_begin(&b, &s_floor);
	for (int row = 0; row < tiles; row++)
	{
		mesh_strip(&b, 2 * (tiles + 1));
		for (int col = 0; col <= tiles; col++)
		{
			const float x = x0 + (x1 - x0) * col / tiles;
			for (int k = 0; k < 2; k++)
			{
				const float z = z0 + (z1 - z0) * (row + k) / tiles;
				mesh_vertex(&b, x, 0.0f, z, 0.0f, 1.0f, 0.0f, (x + 8.0f) * 0.5f, (z + 8.0f) * 0.5f);
			}
		}
	}
	mesh_finish(&s_floor);
}

// ------------------------------------------------------------------ matrices (row vectors: v * M)

// A sine table, as games use for animation instead of calling sinf() per bone.
#define SINE_STEPS 1024
static float s_sine[SINE_STEPS];

static void build_sine(void)
{
	for (int i = 0; i < SINE_STEPS; i++)
		s_sine[i] = sinf(6.2831853f * i / SINE_STEPS);
}

static inline float fast_sin(float a)
{
	return s_sine[(int)(a * (SINE_STEPS / 6.2831853f) + SINE_STEPS * 64) & (SINE_STEPS - 1)];
}

static inline float fast_cos(float a)
{
	return fast_sin(a + 1.5707963f);
}

static void matrix_rotation_xy(MATRIX m, float ax, float ay, const float* translate)
{
	const float cx = fast_cos(ax), sx = fast_sin(ax), cy = fast_cos(ay), sy = fast_sin(ay);
	// RotX(ax) * RotY(ay), then the translation in row 3.
	m[0] = cy;
	m[1] = 0.0f;
	m[2] = -sy;
	m[3] = 0.0f;
	m[4] = sx * sy;
	m[5] = cx;
	m[6] = sx * cy;
	m[7] = 0.0f;
	m[8] = cx * sy;
	m[9] = -sx;
	m[10] = cx * cy;
	m[11] = 0.0f;
	m[12] = translate[0];
	m[13] = translate[1];
	m[14] = translate[2];
	m[15] = 1.0f;
}

static void build_view_screen(void)
{
	// Camera at (0, 3, -9.5) looking at (0, 0.8, 5).
	const float eye[3] = {0.0f, 3.0f, -9.5f};
	const float pitch = atan2f(2.2f, 14.5f);
	MATRIX translate, rotate, screen, view;
	matrix_unit(translate);
	translate[12] = -eye[0];
	translate[13] = -eye[1];
	translate[14] = -eye[2];
	const float zero[3] = {0.0f, 0.0f, 0.0f};
	matrix_rotation_xy(rotate, -pitch, 0.0f, zero);
	matrix_multiply(view, translate, rotate);
	// (x, y, z) in view space -> (x * fx, -y * fy, ZK, z): VU1 divides by z.
	memset(screen, 0, sizeof(screen));
	screen[0] = FOCAL_X;
	screen[5] = -FOCAL_Y;
	screen[11] = 1.0f;
	screen[14] = ZK;
	matrix_multiply(s_view_screen, view, screen);

	const float l[3] = {0.35f, 0.85f, -0.4f};
	const float len = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
	for (int i = 0; i < 3; i++)
		s_light[i] = l[i] / len;
}

// ------------------------------------------------------------------ game logic on the EE

typedef struct
{
	float p[3];
	float v[3];
	int life;
} particle_t;

static particle_t s_particles[MAX_FIGURES][PARTICLES];
static u32 s_random = 12345;
static s16 s_dist[GRID * GRID];
static s16 s_queue[GRID * GRID];
static u8 s_blocked[GRID * GRID];
static int s_steps[MAX_FIGURES];

static float random_unit(void)
{
	s_random = s_random * 1664525u + 1013904223u;
	return (float)(s_random >> 8) / 16777216.0f;
}

static void particle_spawn(particle_t* p, const float* pos)
{
	p->p[0] = pos[0] + (random_unit() - 0.5f) * 0.3f;
	p->p[1] = 0.05f;
	p->p[2] = pos[2] + (random_unit() - 0.5f) * 0.3f;
	p->v[0] = (random_unit() - 0.5f) * 1.2f;
	p->v[1] = 1.0f + random_unit() * 1.5f;
	p->v[2] = (random_unit() - 0.5f) * 1.2f;
	p->life = 20 + (int)(random_unit() * 40.0f);
}

static void particles_step(int figure, const float* pos)
{
	const float dt = 1.0f / 50.0f;
	for (int i = 0; i < PARTICLES; i++)
	{
		particle_t* p = &s_particles[figure][i];
		if (p->life <= 0)
		{
			particle_spawn(p, pos);
			continue;
		}
		p->v[1] -= 9.8f * dt;
		for (int k = 0; k < 3; k++)
			p->p[k] += p->v[k] * dt;
		if (p->p[1] < 0.0f)
		{
			p->p[1] = 0.0f;
			p->v[1] = -p->v[1] * 0.4f;
			p->v[0] *= 0.7f;
			p->v[2] *= 0.7f;
		}
		p->life--;
	}
}

static int grid_cell(float x, float z)
{
	int gx = (int)((x + 7.5f) / 15.0f * GRID), gz = (int)((z + 4.0f) / 17.0f * GRID);
	gx = gx < 0 ? 0 : (gx >= GRID ? GRID - 1 : gx);
	gz = gz < 0 ? 0 : (gz >= GRID ? GRID - 1 : gz);
	return gz * GRID + gx;
}

// Breadth-first search on the floor grid from one cell to another; the path length, or -1.
static int path_length(int from, int to)
{
	for (int i = 0; i < GRID * GRID; i++)
		s_dist[i] = -1;
	int head = 0, tail = 0;
	s_dist[from] = 0;
	s_queue[tail++] = (s16)from;
	while (head < tail)
	{
		const int cell = s_queue[head++];
		if (cell == to)
			return s_dist[cell];
		const int x = cell % GRID, z = cell / GRID;
		const int next[4] = {x > 0 ? cell - 1 : -1, x < GRID - 1 ? cell + 1 : -1, z > 0 ? cell - GRID : -1,
			z < GRID - 1 ? cell + GRID : -1};
		for (int k = 0; k < 4; k++)
		{
			const int n = next[k];
			if (n >= 0 && s_dist[n] < 0 && !s_blocked[n])
			{
				s_dist[n] = s_dist[cell] + 1;
				s_queue[tail++] = (s16)n;
			}
		}
	}
	return -1;
}

static void block_crowd(int figures, int frame)
{
	// Fixed pillars, plus every figure's cell and its neighbours.
	for (int i = 0; i < GRID * GRID; i++)
		s_blocked[i] = ((i % 7) == 3 && ((i / GRID) % 5) == 2);
	for (int f = 0; f < figures; f++)
	{
		const int c = (f * 37 + frame) % (GRID * GRID);
		s_blocked[c] = 1;
	}
}

// ------------------------------------------------------------------ frame building

static void block_fill(qword_t* q, const float* colour, float alpha, const MATRIX bone_world, const MATRIX local_screen, float ambient, u64 tex0)
{
	float* f = (float*)q;
	for (int c = 0; c < 3; c++)
		f[c] = colour[c] * ambient;
	f[3] = alpha;
	// The light in the bone's space (rows of the bone's rotation dotted with the light), each
	// component times the base colour: VU1 adds them up per vertex, weighted by the normal.
	for (int i = 0; i < 3; i++)
	{
		const float l = bone_world ? bone_world[4 * i] * s_light[0] + bone_world[4 * i + 1] * s_light[1] +
										 bone_world[4 * i + 2] * s_light[2] :
									 0.0f;
		for (int c = 0; c < 3; c++)
			f[4 + 4 * i + c] = colour[c] * l;
		f[4 + 4 * i + 3] = 0.0f;
	}
	f[16] = 2048.0f;
	f[17] = 2048.0f;
	f[18] = 0.0f;
	f[19] = 0.0f;
	memcpy(f + 20, local_screen, sizeof(MATRIX));
	q[9].dw[0] = GIF_SET_TAG(1, 0, 0, 0, GIF_FLG_PACKED, 1);
	q[9].dw[1] = GIF_REG_AD;
	q[10].dw[0] = tex0;
	q[10].dw[1] = GS_REG_TEX0;
}

static void chain_mesh(packet2_t* chain, const mesh_t* mesh, int kind, qword_t* block)
{
	for (int i = 0; i < mesh->batches; i++)
	{
		packet2_utils_vu_add_unpack_data(chain, 0, mesh->tags[kind] + 2 * i, 2, 1);
		packet2_utils_vu_add_unpack_data(chain, 2, block, BLOCK_QW, 1);
		packet2_utils_vu_add_unpack_data(chain, VU_HEADER_QW, mesh->data[i], 3 * mesh->verts[i], 1);
		packet2_chain_open_cnt(chain, 0, 0, 0);
		packet2_vif_nop(chain, 0);
		packet2_vif_mscal(chain, 0, 0);
		packet2_chain_close_tag(chain);
	}
}

// Waits until VU1 has finished and its GIF packets are out, then sends GIF packets through PATH2.
static void chain_direct(packet2_t* chain, qword_t* data, int qwc)
{
	packet2_chain_open_cnt(chain, 0, 0, 0);
	packet2_vif_flush(chain, 0);
	packet2_vif_nop(chain, 0);
	packet2_chain_close_tag(chain);
	packet2_chain_ref(chain, data, qwc, 0, 0, 0);
	packet2_vif_nop(chain, 0);
	packet2_vif_open_direct(chain, 0);
	packet2_vif_close_direct_manual(chain, qwc);
}

// Figure slots: a 20 x 15 crowd, filled front row first, middle columns first.
static void figure_position(int index, float* out)
{
	const int row = index / 20, k = index % 20;
	const int col = (k & 1) ? 10 + k / 2 : 9 - k / 2;
	out[0] = (col - 9.5f) * 0.55f;
	out[1] = 0.0f;
	out[2] = row * 0.75f;
}

static u64 figure_tex0(int figure)
{
	return GS_SET_TEX0(s_figure_tex >> 6, 128 >> 6, GS_PSM_8, 7, 7, 1, TEXTURE_FUNCTION_MODULATE, s_clut[figure % 4] >> 6,
		GS_PSM_32, 0, 0, 1);
}

static u64 floor_tex0(void)
{
	return GS_SET_TEX0(s_floor_tex.address >> 6, 64 >> 6, GS_PSM_32, 6, 6, 1, TEXTURE_FUNCTION_MODULATE, 0, 0, 0, 0, 0);
}

static qword_t* figure_dust(qword_t* q, int figure)
{
	const float* m = s_view_screen;
	for (int i = 0; i < PARTICLES; i++)
	{
		const particle_t* p = &s_particles[figure][i];
		float o[4];
		for (int c = 0; c < 4; c++)
			o[c] = p->p[0] * m[c] + p->p[1] * m[4 + c] + p->p[2] * m[8 + c] + m[12 + c];
		const float inv = 1.0f / o[3];
		const float sx = o[0] * inv + 2048.0f, sy = o[1] * inv + 2048.0f;
		const float r = 0.035f * FOCAL_X * inv;
		const int z = (int)(o[2] * inv * 16.0f);
		q->sw[0] = 200;
		q->sw[1] = 180;
		q->sw[2] = 150;
		q->sw[3] = 0x40;
		q++;
		q->sw[0] = (int)((sx - r) * 16.0f);
		q->sw[1] = (int)((sy - r) * 16.0f);
		q->sw[2] = z;
		q->sw[3] = 0;
		q++;
		q->sw[0] = (int)((sx + r) * 16.0f);
		q->sw[1] = (int)((sy + r) * 16.0f);
		q->sw[2] = z;
		q->sw[3] = 0;
		q++;
	}
	return q;
}

// Builds the chain that draws frame number "frame" into draw buffer "target".
static void build_frame(int ctx, int frame, int figures, int target)
{
	packet2_t* chain = s_chain[ctx];
	qword_t* blocks = s_blocks[ctx];
	qword_t* direct = s_direct[ctx];
	int used = 0;
	packet2_reset(chain, 0);

	const float t = frame / 50.0f;
	static const float grey[3] = {128.0f, 128.0f, 128.0f};
	static const float black[3] = {0.0f, 0.0f, 0.0f};

	block_crowd(figures, frame);

	// Floor, lit from straight above in its own space (the GS has the floor texture).
	{
		MATRIX identity;
		matrix_unit(identity);
		block_fill(&blocks[BLOCK_QW * used], grey, 128.0f, identity, s_view_screen, 0.35f, floor_tex0());
		chain_mesh(chain, &s_floor, KIND_LIT_TEXTURED, &blocks[BLOCK_QW * used]);
		used++;
	}

	// The near figures' dust, one GIF packet: its tag first, NLOOP filled in at the end.
	qword_t* dust_tag = direct++;

	for (int f = 0; f < figures; f++)
	{
		float pos[3];
		figure_position(f, pos);
		const float phase = f * 0.83f;
		const int near = f < NEAR_FIGURES;
		const mesh_t* limb = near ? &s_ellipsoid : &s_ellipsoid_far;
		const u64 tex0 = figure_tex0(f);

		// Game logic: dust for near figures, and a path search (each figure every 8 frames, as games
		// spread their AI) that nudges where the figure looks.
		if (near)
			particles_step(f, pos);
		if (((f + frame) & 7) == 0)
			s_steps[f] = path_length(grid_cell(pos[0], pos[2]), (frame * 7 + f * 131) % (GRID * GRID));
		const float facing = 3.1415927f + 0.6f * fast_sin(t * 0.7f + phase) + 0.002f * (float)(s_steps[f] & 15);

		MATRIX world[BONES];
		for (int bone_index = 0; bone_index < BONES; bone_index++)
		{
			const bone_t* bone = &SKELETON[bone_index];
			MATRIX local;
			if (bone->parent < 0)
			{
				const float root[3] = {pos[0] + bone->offset[0], pos[1] + bone->offset[1] + 0.03f * fast_sin(t * 6.0f + phase), pos[2] + bone->offset[2]};
				matrix_rotation_xy(local, bone->swing * fast_sin(t * 3.0f + phase), facing, root);
				memcpy(world[bone_index], local, sizeof(MATRIX));
			}
			else
			{
				matrix_rotation_xy(local, bone->swing * fast_sin(t * 3.0f + phase + bone->phase), 0.0f, bone->offset);
				matrix_multiply(world[bone_index], local, world[bone->parent]);
			}

			MATRIX mesh_local, bone_screen, local_screen;
			matrix_unit(mesh_local);
			mesh_local[0] = bone->scale[0];
			mesh_local[5] = bone->scale[1];
			mesh_local[10] = bone->scale[2];
			mesh_local[12] = bone->centre[0];
			mesh_local[13] = bone->centre[1];
			mesh_local[14] = bone->centre[2];
			matrix_multiply(bone_screen, world[bone_index], s_view_screen);
			matrix_multiply(local_screen, mesh_local, bone_screen);

			block_fill(&blocks[BLOCK_QW * used], grey, 128.0f, world[bone_index], local_screen, 0.3f, tex0);
			chain_mesh(chain, limb, KIND_LIT_TEXTURED, &blocks[BLOCK_QW * used]);
			used++;
		}

		// A flat, half-transparent shadow under the figure.
		{
			MATRIX shadow, shadow_screen;
			matrix_unit(shadow);
			shadow[0] = 0.4f;
			shadow[5] = 0.01f;
			shadow[10] = 0.3f;
			shadow[12] = pos[0];
			shadow[13] = 0.02f;
			shadow[14] = pos[2];
			matrix_multiply(shadow_screen, shadow, s_view_screen);
			block_fill(&blocks[BLOCK_QW * used], black, 72.0f, NULL, shadow_screen, 1.0f, tex0);
			chain_mesh(chain, &s_ellipsoid_far, KIND_SHADOW, &blocks[BLOCK_QW * used]);
			used++;
		}

		if (near)
			direct = figure_dust(direct, f);
	}

	// The dust, once VU1 has drawn the crowd.
	const int dust_sprites = (int)(direct - dust_tag - 1) / 3;
	if (dust_sprites > 0)
	{
		const u64 prim = GS_SET_PRIM(PRIM_SPRITE, 0, 0, 0, 1, 0, 0, 0, 0);
		dust_tag->dw[0] = GIF_SET_TAG(dust_sprites, 1, 1, prim, GIF_FLG_PACKED, 3);
		dust_tag->dw[1] = ((u64)GIF_REG_RGBAQ) | ((u64)GIF_REG_XYZ2 << 4) | ((u64)GIF_REG_XYZ2 << 8);
		chain_direct(chain, dust_tag, (int)(direct - dust_tag));
	}

	// The glow pass, after everything else is drawn.
	chain_direct(chain, s_post[target], s_post_qw);

	// Wait for VU1 and PATH1 before the DMA ends, so "DMA done" means "frame drawn".
	packet2_chain_open_end(chain, 0, 0);
	packet2_vif_flush(chain, 0);
	packet2_vif_nop(chain, 0);
	packet2_chain_close_tag(chain);
}

// ------------------------------------------------------------------ the glow pass

static qword_t* post_target(qword_t* q, u32 address, int width, int height, u32 texture, int tex_width, int tex_height)
{
	q = gif_ad_tag(q, 4, 0);
	q = gif_ad(q, GS_REG_FRAME, GS_SET_FRAME(address >> 11, width >> 6, GS_PSM_32, 0));
	q = gif_ad(q, GS_REG_SCISSOR, GS_SET_SCISSOR(0, width - 1, 0, height - 1));
	q = gif_ad(q, GS_REG_TEX0, GS_SET_TEX0(texture >> 6, tex_width >> 6, GS_PSM_32, log2_ceil(tex_width), log2_ceil(tex_height), 0,
								   TEXTURE_FUNCTION_DECAL, 0, 0, 0, 0, 0));
	q = gif_ad(q, GS_REG_TEXFLUSH, 1);
	return q;
}

// Blurs the source target into the destination: clear, then four offset copies at a quarter each.
static qword_t* post_blur(qword_t* q, u32 dest, u32 source)
{
	q = post_target(q, dest, GLOW_W, GLOW_H, source, GLOW_W, GLOW_H);
	q = gif_sprite(q, 0, 0, 0, 0, GLOW_W, GLOW_H, 0, 0, 0, 0, 0, 0, 0);
	q = gif_ad_tag(q, 1, 0);
	q = gif_ad(q, GS_REG_ALPHA, GS_SET_ALPHA(0, 2, 2, 1, 0x20));
	static const float offsets[4][2] = {{-1.5f, -1.5f}, {1.5f, -1.5f}, {-1.5f, 1.5f}, {1.5f, 1.5f}};
	for (int i = 0; i < 4; i++)
	{
		const float dx = offsets[i][0], dy = offsets[i][1];
		q = gif_sprite(q, 1, 1, dx, dy, GLOW_W + dx, GLOW_H + dy, 0, 0, GLOW_W, GLOW_H, 128, 128, 128);
	}
	return q;
}

static void build_post(int draw)
{
	qword_t* q = s_post[draw];
	q = gif_ad_tag(q, 4, 0);
	q = gif_ad(q, GS_REG_TEST, GS_SET_TEST(0, 0, 0, 0, 0, 0, 1, ZTEST_METHOD_ALLPASS));
	q = gif_ad(q, GS_REG_ZBUF, GS_SET_ZBUF(s_z.address >> 11, s_z.zsm, 1));
	q = gif_ad(q, GS_REG_CLAMP, GS_SET_CLAMP(1, 1, 0, 0, 0, 0));
	q = gif_ad(q, GS_REG_TEX1, GS_SET_TEX1(1, 0, 1, 1, 0, 0, 0));

	// Half-size copy of the frame, then two blurs.
	q = post_target(q, s_glow[0], GLOW_W, GLOW_H, s_frame[draw].address, SCREEN_W, SCREEN_H);
	q = gif_sprite(q, 1, 0, 0, 0, GLOW_W, GLOW_H, 0, 0, SCREEN_W, SCREEN_H, 128, 128, 128);
	q = post_blur(q, s_glow[1], s_glow[0]);
	q = post_blur(q, s_glow[0], s_glow[1]);

	// Added back over the frame.
	q = post_target(q, s_frame[draw].address, SCREEN_W, SCREEN_H, s_glow[0], GLOW_W, GLOW_H);
	q = gif_ad_tag(q, 1, 0);
	q = gif_ad(q, GS_REG_ALPHA, GS_SET_ALPHA(0, 2, 2, 1, 0x30));
	q = gif_sprite(q, 1, 1, 0, 0, SCREEN_W, SCREEN_H, 0, 0, GLOW_W, GLOW_H, 128, 128, 128);

	// The 3D state for the next frame.
	q = gif_ad_tag(q, 5, 1);
	q = gif_ad(q, GS_REG_TEST, GS_SET_TEST(0, 0, 0, 0, 0, 0, 1, ZTEST_METHOD_GREATER_EQUAL));
	q = gif_ad(q, GS_REG_ZBUF, GS_SET_ZBUF(s_z.address >> 11, s_z.zsm, 0));
	q = gif_ad(q, GS_REG_ALPHA, GS_SET_ALPHA(0, 1, 0, 1, 0x80));
	q = gif_ad(q, GS_REG_CLAMP, GS_SET_CLAMP(0, 0, 0, 0, 0, 0));
	q = gif_ad(q, GS_REG_SCISSOR, GS_SET_SCISSOR(0, SCREEN_W - 1, 0, SCREEN_H - 1));
	s_post_qw = (int)(q - s_post[draw]);
}

// ------------------------------------------------------------------ GS and VU setup

static void upload_vu1_program(void)
{
	const u32 size = packet2_utils_get_packet_size_for_program(&BenchVU1_CodeStart, &BenchVU1_CodeEnd) + 1;
	packet2_t* p = packet2_create(size, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	packet2_vif_add_micro_program(p, 0, &BenchVU1_CodeStart, &BenchVU1_CodeEnd);
	packet2_utils_vu_add_end_tag(p);
	dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	packet2_free(p);

	p = packet2_create(2, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	packet2_utils_vu_add_double_buffer(p, 8, 496);
	packet2_utils_vu_add_end_tag(p);
	dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	packet2_free(p);
}

static void upload_floor_texture(void)
{
	// 64 x 64 RGBA: a two-tone check with a darker border on each tile.
	u32* pixels = (u32*)memalign(128, 64 * 64 * 4);
	for (int y = 0; y < 64; y++)
	{
		for (int x = 0; x < 64; x++)
		{
			const int check = ((x >> 4) ^ (y >> 4)) & 1;
			const int edge = ((x & 15) == 0) || ((y & 15) == 0);
			u32 v = check ? 0xF0 : 0xB0;
			if (edge)
				v = 0x60;
			pixels[y * 64 + x] = 0x80000000u | (v << 16) | (v << 8) | v;
		}
	}
	FlushCache(0);
	packet2_t* p = packet2_create(50, P2_TYPE_NORMAL, P2_MODE_CHAIN, 0);
	packet2_update(p, draw_texture_transfer(p->next, pixels, 64, 64, GS_PSM_32, s_floor_tex.address, s_floor_tex.width));
	packet2_update(p, draw_texture_flush(p->next));
	dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);
	packet2_free(p);
	free(pixels);
}

// The figures' texture: 8-bit indices (rings and stripes); its four palettes change every frame.
static void init_figure_texture(void)
{
	s_figure_pixels = (u8*)memalign(128, 128 * 128);
	for (int y = 0; y < 128; y++)
	{
		for (int x = 0; x < 128; x++)
		{
			const int dx = x - 64, dy = y - 64;
			const int ring = (int)(sqrtf((float)(dx * dx + dy * dy)) * 4.0f);
			s_figure_pixels[y * 128 + x] = (u8)((ring + ((x >> 3) & 1) * 64) & 255);
		}
	}
	s_palettes = (u32*)memalign(128, 4 * 256 * 4);
}

static void update_palettes(int frame)
{
	for (int p = 0; p < 4; p++)
	{
		for (int i = 0; i < 256; i++)
		{
			const int level = 96 + ((i + frame * 2) & 127);
			const u32 r = (u32)(PALETTE_HUES[p][0] * level) >> 8;
			const u32 g = (u32)(PALETTE_HUES[p][1] * level) >> 8;
			const u32 b = (u32)(PALETTE_HUES[p][2] * level) >> 8;
			s_palettes[p * 256 + i] = 0x80000000u | (b << 16) | (g << 8) | r;
		}
	}
}

static void init_gs(void)
{
	for (int i = 0; i < 2; i++)
	{
		s_frame[i].width = SCREEN_W;
		s_frame[i].height = SCREEN_H;
		s_frame[i].mask = 0;
		s_frame[i].psm = GS_PSM_32;
		s_frame[i].address = graph_vram_allocate(SCREEN_W, SCREEN_H, GS_PSM_32, GRAPH_ALIGN_PAGE);
	}
	s_z.enable = DRAW_ENABLE;
	s_z.mask = 0;
	s_z.method = ZTEST_METHOD_GREATER_EQUAL;
	s_z.zsm = GS_ZBUF_16;
	s_z.address = graph_vram_allocate(SCREEN_W, SCREEN_H, GS_ZBUF_16, GRAPH_ALIGN_PAGE);
	for (int i = 0; i < 2; i++)
		s_glow[i] = graph_vram_allocate(GLOW_W, GLOW_H, GS_PSM_32, GRAPH_ALIGN_PAGE);

	s_floor_tex.width = 64;
	s_floor_tex.psm = GS_PSM_32;
	s_floor_tex.address = graph_vram_allocate(64, 64, GS_PSM_32, GRAPH_ALIGN_BLOCK);
	s_floor_tex.info.width = draw_log2(64);
	s_floor_tex.info.height = draw_log2(64);
	s_floor_tex.info.components = TEXTURE_COMPONENTS_RGBA;
	s_floor_tex.info.function = TEXTURE_FUNCTION_MODULATE;
	s_figure_tex = graph_vram_allocate(128, 128, GS_PSM_8, GRAPH_ALIGN_BLOCK);
	for (int i = 0; i < 4; i++)
		s_clut[i] = graph_vram_allocate(16, 16, GS_PSM_32, GRAPH_ALIGN_BLOCK);

	s_no_clut.storage_mode = CLUT_STORAGE_MODE1;
	s_no_clut.start = 0;
	s_no_clut.psm = 0;
	s_no_clut.load_method = CLUT_NO_LOAD;
	s_no_clut.address = 0;

	s_lod.calculation = LOD_USE_K;
	s_lod.max_level = 0;
	s_lod.mag_filter = LOD_MAG_LINEAR;
	s_lod.min_filter = LOD_MIN_LINEAR;
	s_lod.l = 0;
	s_lod.k = 0;

	s_wrap.horizontal = WRAP_REPEAT;
	s_wrap.vertical = WRAP_REPEAT;
	s_wrap.minu = s_wrap.maxu = s_wrap.minv = s_wrap.maxv = 0;

	// (Cs - Cd) * As + Cd
	s_blend.color1 = BLEND_COLOR_SOURCE;
	s_blend.color2 = BLEND_COLOR_DEST;
	s_blend.alpha = BLEND_ALPHA_SOURCE;
	s_blend.color3 = BLEND_COLOR_DEST;
	s_blend.fixed_alpha = 0x80;

	graph_initialize(s_frame[0].address, SCREEN_W, SCREEN_H, GS_PSM_32, 0, 0);

	packet2_t* p = packet2_create(20, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
	packet2_update(p, draw_setup_environment(p->next, 0, &s_frame[0], &s_z));
	packet2_update(p, draw_primitive_xyoffset(p->next, 0, OFFSET_X, OFFSET_Y));
	packet2_update(p, draw_finish(p->next));
	dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);
	packet2_free(p);
}

// Uploads the figures' texture and palettes again (PATH3), points the GS at the frame to draw,
// clears it and sets the floor's texture and the blending state.
static void begin_gs_frame(int draw)
{
	packet2_t* u = s_uploads;
	packet2_reset(u, 0);
	packet2_update(u, draw_texture_transfer(u->next, s_figure_pixels, 128, 128, GS_PSM_8, s_figure_tex, 128));
	for (int i = 0; i < 4; i++)
		packet2_update(u, draw_texture_transfer(u->next, s_palettes + 256 * i, 16, 16, GS_PSM_32, s_clut[i], 64));
	packet2_update(u, draw_texture_flush(u->next));
	dma_channel_send_packet2(u, DMA_CHANNEL_GIF, 1);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);

	packet2_t* p = s_gs_setup;
	packet2_reset(p, 0);
	packet2_update(p, draw_framebuffer(p->next, 0, &s_frame[draw]));
	packet2_update(p, draw_disable_tests(p->next, 0, &s_z));
	packet2_update(p, draw_clear(p->next, 0, OFFSET_X, OFFSET_Y, SCREEN_W, SCREEN_H, 0x30, 0x38, 0x48));
	packet2_update(p, draw_enable_tests(p->next, 0, &s_z));
	packet2_update(p, draw_texturebuffer(p->next, 0, &s_floor_tex, &s_no_clut));
	packet2_update(p, draw_texture_sampling(p->next, 0, &s_lod));
	packet2_update(p, draw_texture_wrapping(p->next, 0, &s_wrap));
	packet2_update(p, draw_alpha_blending(p->next, 0, &s_blend));
	packet2_update(p, draw_finish(p->next));
	dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);
	draw_wait_finish();
}

// Games sleep until an interrupt instead of polling registers: the EE thread waits on a semaphore
// that the vblank and VIF1 DMA-end handlers signal, so the kernel's idle loop runs meanwhile.
static int s_vsync_sema = -1;
static int s_vif1_sema = -1;
static volatile u32 s_vsyncs;

static s32 vsync_handler(s32 cause)
{
	s_vsyncs++;
	iSignalSema(s_vsync_sema);
	ExitHandler();
	return 0;
}

static s32 vif1_handler(s32 channel)
{
	iSignalSema(s_vif1_sema);
	ExitHandler();
	return 0;
}

static int create_sema(void)
{
	ee_sema_t sema;
	memset(&sema, 0, sizeof(sema));
	sema.init_count = 0;
	sema.max_count = 1;
	return CreateSema(&sema);
}

static void init_interrupts(void)
{
	s_vsync_sema = create_sema();
	s_vif1_sema = create_sema();
	AddIntcHandler(INTC_VBLANK_S, vsync_handler, 0);
	EnableIntc(INTC_VBLANK_S);
	AddDmacHandler(DMAC_VIF1, vif1_handler, 0);
	EnableDmac(DMAC_VIF1);
}

// Sleeps until the next vblank starts.
static void wait_vsync(void)
{
	PollSema(s_vsync_sema); // drop a vblank that came while the frame was being built
	WaitSema(s_vsync_sema);
}

// Sleeps until the chain sent with send_chain() has been transferred, then waits the few cycles
// VIF1 may still need for the chain's final FLUSH (VU1 done, its last XGKICK sent).
static void wait_vif1_idle(void)
{
	WaitSema(s_vif1_sema);
	volatile u32* vif1_stat = (volatile u32*)0x10003C00;
	while ((*vif1_stat & 0x0F) || ((*vif1_stat >> 24) & 0x1F))
		;
}

static void send_chain(packet2_t* chain)
{
	PollSema(s_vif1_sema);
	dma_channel_send_packet2(chain, DMA_CHANNEL_VIF1, 0);
}

// The EE's cycle counter (COP0 Count), 294.912 MHz.
static inline u32 ee_cycles(void)
{
	u32 c;
	__asm__ __volatile__("mfc0 %0, $9" : "=r"(c));
	return c;
}

// Where the EE's time went during a phase, printed when the phase ends: building frames (game
// logic, skeletons, the DMA chain), waiting for VU1 and the GS to finish the previous frame, and
// waiting for vsync. No vsync wait and more than one vsync per frame mean the phase asks more
// than a PS2 can draw at 50 frames a second.
typedef struct
{
	u32 frames, vsync_start;
	u64 build, draw_wait, vsync_wait;
} phase_load_t;

static void print_load(int figures, const phase_load_t* load)
{
	const u32 vsyncs = s_vsyncs - load->vsync_start;
	const u64 total = (u64)vsyncs * (294912000u / 50u);
	if (!load->frames || !total)
		return;
	printf("BENCH load N=%d frames=%u vsyncs=%u (%u.%02u per frame) EE: build %u%%, waiting for VU1/GS %u%%, for vsync %u%%\n",
		figures, load->frames, vsyncs, vsyncs / load->frames, (vsyncs * 100u / load->frames) % 100u,
		(u32)(load->build * 100 / total), (u32)(load->draw_wait * 100 / total), (u32)(load->vsync_wait * 100 / total));
}

int main(int argc, char* argv[])
{
	printf("BENCH start: TabStationToo crowd benchmark, %d bones per figure\n", BONES);

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);

	upload_vu1_program();
	init_gs();
	init_interrupts();
	upload_floor_texture();
	init_figure_texture();
	build_sine();
	build_ellipsoid();
	build_floor();
	build_view_screen();

	// Per batch 4 tags; per figure 2 x 3 tags for the PATH2 packets; the glow pass; the end.
	const int max_batches = s_floor.batches + MAX_FIGURES * (BONES + 1) * s_ellipsoid.batches;
	for (int i = 0; i < 2; i++)
	{
		s_blocks[i] = (qword_t*)memalign(64, sizeof(qword_t) * BLOCK_QW * MAX_BLOCKS);
		s_direct[i] = (qword_t*)memalign(64, sizeof(qword_t) * DIRECT_QW);
		s_chain[i] = packet2_create(4 * max_batches + 6 * MAX_FIGURES + 8, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
		s_post[i] = (qword_t*)memalign(64, sizeof(qword_t) * 128);
	}
	build_post(0);
	build_post(1);
	s_gs_setup = packet2_create(64, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
	s_uploads = packet2_create(64, P2_TYPE_NORMAL, P2_MODE_CHAIN, 0);
	FlushCache(0);

	int ctx = 0, draw = 0, last_figures = 0;
	phase_load_t load = {0};
	for (int frame = 0;; frame++)
	{
		const int figures = PHASE_COUNTS[(frame / PHASE_FRAMES) % PHASE_TOTAL];
		if (figures != last_figures)
		{
			if (last_figures)
				print_load(last_figures, &load);
			printf("BENCH phase N=%d frame=%d\n", figures, frame);
			last_figures = figures;
			memset(&load, 0, sizeof(load));
			load.vsync_start = s_vsyncs;
		}

		// Build this frame while VU1 and the GS still draw the previous one.
		const u32 c0 = ee_cycles();
		build_frame(ctx, frame, figures, draw ^ 1);
		update_palettes(frame);
		FlushCache(0);

		const u32 c1 = ee_cycles();
		if (frame > 0)
			wait_vif1_idle();
		const u32 c2 = ee_cycles();
		wait_vsync();
		const u32 c3 = ee_cycles();
		load.frames++;
		load.build += c1 - c0;
		load.draw_wait += c2 - c1;
		load.vsync_wait += c3 - c2;
		if (frame > 0)
			graph_set_framebuffer_filtered(s_frame[draw].address, SCREEN_W, GS_PSM_32, 0, 0);
		draw ^= 1;
		begin_gs_frame(draw);
		send_chain(s_chain[ctx]);
		ctx ^= 1;
	}
	return 0;
}
