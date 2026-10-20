// SPDX-FileCopyrightText: 2026 TabStationToo contributors
// SPDX-License-Identifier: MIT

// TabStationToo's benchmark: a PS2 program with no menu and no input that draws a crowd of
// animated figures the way games draw characters. Each frame the EE poses every figure's
// skeleton (VU0 macro-mode matrix products), builds one VIF1 DMA chain that points at the
// meshes, and VU1 (bench_vu1.vsm) transforms, lights and projects the vertices and kicks them
// to the GS: textured, Gouraud-shaded, Z-buffered triangle strips, a floor and see-through
// shadows. The number of figures steps through PHASE_COUNTS, PHASE_FRAMES frames each, and the
// program prints "BENCH phase N=<figures> frame=<frame>" at every step so a runner can tell
// the phases apart. It draws at most one frame per vsync and sleeps on interrupts while it waits,
// like a game.

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
#define MAX_FIGURES 16
#define PHASE_FRAMES 400
static const int PHASE_COUNTS[] = {1, 2, 4, 8, 16};
#define PHASE_TOTAL ((int)(sizeof(PHASE_COUNTS) / sizeof(PHASE_COUNTS[0])))

// VU1 memory: two buffers of 496 quadwords from address 8. A batch needs 9 header quadwords,
// 3 per input vertex, 1 for the GIF tag and 3 per output vertex.
#define VU_HEADER_QW 9
#define MAX_BATCH_VERTS 80
#define MAX_BATCHES 16

// Z = ZK / depth, stored by FTOI4 (x16): ZK makes the nearest drawn depth (3.0) fill 24 bits.
#define ZK (16777215.0f / 16.0f * 3.0f)
#define FOCAL_X 554.0f // 60 degrees across 640 pixels
#define FOCAL_Y 591.0f // PAL pixels are taller than wide

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

static const float FIGURE_COLOURS[4][3] = {
	{128.0f, 72.0f, 40.0f}, {48.0f, 96.0f, 128.0f}, {112.0f, 112.0f, 56.0f}, {96.0f, 56.0f, 120.0f}};

// Per-frame data the DMA chain points at, one set per frame in flight.
// Each block: colour, light, screen offset, matrix (7 quadwords, VU1 addresses 2 to 8).
#define BLOCK_QW 7
#define MAX_BLOCKS (MAX_FIGURES * (BONES + 1) + 1)
static qword_t* s_blocks[2];
static packet2_t* s_chain[2];
static packet2_t* s_gs_setup;

static mesh_t s_ellipsoid;
static mesh_t s_floor;
static MATRIX s_view_screen;
static float s_light[3];

static framebuffer_t s_frame[2];
static zbuffer_t s_z;
static texbuffer_t s_tex;
static clutbuffer_t s_clut;
static lod_t s_lod;
static blend_t s_blend;
static texwrap_t s_wrap;

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

// A unit sphere as 8 latitude strips of 12 segments: 192 triangles.
static void build_ellipsoid(void)
{
	const int bands = 8, segments = 12;
	mesh_builder_t b;
	mesh_begin(&b, &s_ellipsoid);
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
	mesh_finish(&s_ellipsoid);
}

// The floor: 16 x 16 tiles, 512 triangles.
static void build_floor(void)
{
	const int tiles = 16;
	const float x0 = -4.5f, x1 = 4.5f, z0 = -1.5f, z1 = 7.5f;
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
				mesh_vertex(&b, x, 0.0f, z, 0.0f, 1.0f, 0.0f, x * 0.5f, z * 0.5f);
			}
		}
	}
	mesh_finish(&s_floor);
}

// ------------------------------------------------------------------ matrices (row vectors: v * M)

static void matrix_rotation_xy(MATRIX m, float ax, float ay, const float* translate)
{
	const float cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay);
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
	// Camera at (0, 1.9, -5) looking at (0, 0.9, 3).
	const float eye[3] = {0.0f, 1.9f, -5.0f};
	const float pitch = atan2f(1.0f, 8.0f);
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

// ------------------------------------------------------------------ frame building

static void block_fill(qword_t* q, const float* colour, float alpha, const MATRIX bone_world, const MATRIX local_screen, float ambient)
{
	float* f = (float*)q;
	f[0] = colour[0];
	f[1] = colour[1];
	f[2] = colour[2];
	f[3] = alpha;
	if (bone_world)
	{
		// The light in the bone's space: rows of the bone's rotation dotted with the light.
		for (int i = 0; i < 3; i++)
			f[4 + i] = bone_world[4 * i] * s_light[0] + bone_world[4 * i + 1] * s_light[1] + bone_world[4 * i + 2] * s_light[2];
	}
	else
	{
		f[4] = f[5] = f[6] = 0.0f;
	}
	f[7] = ambient;
	f[8] = 2048.0f;
	f[9] = 2048.0f;
	f[10] = 0.0f;
	f[11] = 0.0f;
	memcpy(f + 12, local_screen, sizeof(MATRIX));
}

static void chain_mesh(packet2_t* chain, const mesh_t* mesh, int kind, qword_t* block)
{
	for (int i = 0; i < mesh->batches; i++)
	{
		packet2_utils_vu_add_unpack_data(chain, 0, mesh->tags[kind] + 2 * i, 2, 1);
		packet2_utils_vu_add_unpack_data(chain, 2, block, 7, 1);
		packet2_utils_vu_add_unpack_data(chain, VU_HEADER_QW, mesh->data[i], 3 * mesh->verts[i], 1);
		packet2_chain_open_cnt(chain, 0, 0, 0);
		packet2_vif_nop(chain, 0);
		packet2_vif_mscal(chain, 0, 0);
		packet2_chain_close_tag(chain);
	}
}

// Figure slots: a 4 x 4 crowd, filled front row first, middle columns first.
static void figure_position(int index, float* out)
{
	static const int column_order[4] = {1, 2, 0, 3};
	const int row = index / 4, col = column_order[index % 4];
	out[0] = (col - 1.5f) * 1.3f;
	out[1] = 0.0f;
	out[2] = row * 1.6f;
}

// Game-style EE work per figure: bounding points through VU0 and a crowd separation check.
static VECTOR s_probe_in[32] __attribute__((aligned(16)));
static VECTOR s_probe_out[32] __attribute__((aligned(16)));
static float s_bounds[MAX_FIGURES][4];

static void figure_logic(int figure, int figures, const MATRIX root_screen)
{
	calculate_vertices(s_probe_out, 32, s_probe_in, (float*)root_screen);
	float min_x = 1e9f, max_x = -1e9f;
	for (int i = 0; i < 32; i++)
	{
		min_x = fminf(min_x, s_probe_out[i][0]);
		max_x = fmaxf(max_x, s_probe_out[i][0]);
	}
	s_bounds[figure][0] = min_x;
	s_bounds[figure][1] = max_x;
	int overlaps = 0;
	for (int other = 0; other < figure && other < figures; other++)
		overlaps += (s_bounds[other][0] < max_x && s_bounds[other][1] > min_x);
	s_bounds[figure][2] = (float)overlaps;
}

static void build_frame(int ctx, int frame, int figures)
{
	packet2_t* chain = s_chain[ctx];
	qword_t* blocks = s_blocks[ctx];
	int used = 0;
	packet2_reset(chain, 0);

	const float t = frame / 50.0f;
	static const float white[3] = {128.0f, 128.0f, 128.0f};
	static const float black[3] = {0.0f, 0.0f, 0.0f};

	// Floor, lit from straight above in its own space.
	{
		MATRIX identity;
		matrix_unit(identity);
		block_fill(&blocks[BLOCK_QW * used], white, 128.0f, identity, s_view_screen, 0.35f);
		chain_mesh(chain, &s_floor, KIND_LIT_TEXTURED, &blocks[BLOCK_QW * used]);
		used++;
	}

	for (int f = 0; f < figures; f++)
	{
		float pos[3];
		figure_position(f, pos);
		const float phase = f * 0.83f;
		const float facing = 3.1415927f + 0.6f * sinf(t * 0.7f + phase);

		MATRIX world[BONES];
		for (int b = 0; b < BONES; b++)
		{
			const bone_t* bone = &SKELETON[b];
			MATRIX local;
			if (bone->parent < 0)
			{
				const float root[3] = {pos[0] + bone->offset[0], pos[1] + bone->offset[1] + 0.03f * sinf(t * 6.0f + phase), pos[2] + bone->offset[2]};
				matrix_rotation_xy(local, bone->swing * sinf(t * 3.0f + phase), facing, root);
				memcpy(world[b], local, sizeof(MATRIX));
			}
			else
			{
				matrix_rotation_xy(local, bone->swing * sinf(t * 3.0f + phase + bone->phase), 0.0f, bone->offset);
				matrix_multiply(world[b], local, world[bone->parent]);
			}

			MATRIX mesh_local, bone_screen, local_screen;
			matrix_unit(mesh_local);
			mesh_local[0] = bone->scale[0];
			mesh_local[5] = bone->scale[1];
			mesh_local[10] = bone->scale[2];
			mesh_local[12] = bone->centre[0];
			mesh_local[13] = bone->centre[1];
			mesh_local[14] = bone->centre[2];
			matrix_multiply(bone_screen, world[b], s_view_screen);
			matrix_multiply(local_screen, mesh_local, bone_screen);

			block_fill(&blocks[BLOCK_QW * used], FIGURE_COLOURS[f % 4], 128.0f, world[b], local_screen, 0.3f);
			chain_mesh(chain, &s_ellipsoid, KIND_LIT_TEXTURED, &blocks[BLOCK_QW * used]);
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
			block_fill(&blocks[BLOCK_QW * used], black, 72.0f, NULL, shadow_screen, 1.0f);
			chain_mesh(chain, &s_ellipsoid, KIND_SHADOW, &blocks[BLOCK_QW * used]);
			used++;
		}

		MATRIX root_screen;
		matrix_multiply(root_screen, world[0], s_view_screen);
		figure_logic(f, figures, root_screen);
	}

	// Wait for VU1 and PATH1 before the DMA ends, so "DMA done" means "frame drawn".
	packet2_chain_open_end(chain, 0, 0);
	packet2_vif_flush(chain, 0);
	packet2_vif_nop(chain, 0);
	packet2_chain_close_tag(chain);
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

static void upload_texture(void)
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
	packet2_update(p, draw_texture_transfer(p->next, pixels, 64, 64, GS_PSM_32, s_tex.address, s_tex.width));
	packet2_update(p, draw_texture_flush(p->next));
	dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);
	packet2_free(p);
	free(pixels);
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
	s_z.zsm = GS_ZBUF_24;
	s_z.address = graph_vram_allocate(SCREEN_W, SCREEN_H, GS_ZBUF_24, GRAPH_ALIGN_PAGE);

	s_tex.width = 64;
	s_tex.psm = GS_PSM_32;
	s_tex.address = graph_vram_allocate(64, 64, GS_PSM_32, GRAPH_ALIGN_BLOCK);
	s_tex.info.width = draw_log2(64);
	s_tex.info.height = draw_log2(64);
	s_tex.info.components = TEXTURE_COMPONENTS_RGBA;
	s_tex.info.function = TEXTURE_FUNCTION_MODULATE;

	s_clut.storage_mode = CLUT_STORAGE_MODE1;
	s_clut.start = 0;
	s_clut.psm = 0;
	s_clut.load_method = CLUT_NO_LOAD;
	s_clut.address = 0;

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
	packet2_update(p, draw_primitive_xyoffset(p->next, 0, 2048 - SCREEN_W / 2, 2048 - SCREEN_H / 2));
	packet2_update(p, draw_finish(p->next));
	dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);
	packet2_free(p);
}

// Points the GS at the frame to draw, clears it and sets the texture and blending state.
static void begin_gs_frame(int draw)
{
	packet2_t* p = s_gs_setup;
	packet2_reset(p, 0);
	packet2_update(p, draw_framebuffer(p->next, 0, &s_frame[draw]));
	packet2_update(p, draw_disable_tests(p->next, 0, &s_z));
	packet2_update(p, draw_clear(p->next, 0, 2048.0f - SCREEN_W / 2, 2048.0f - SCREEN_H / 2, SCREEN_W, SCREEN_H, 0x30, 0x38, 0x48));
	packet2_update(p, draw_enable_tests(p->next, 0, &s_z));
	packet2_update(p, draw_texturebuffer(p->next, 0, &s_tex, &s_clut));
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

static s32 vsync_handler(s32 cause)
{
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

int main(int argc, char* argv[])
{
	printf("BENCH start: TabStationToo crowd benchmark, %d bones per figure\n", BONES);

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);

	upload_vu1_program();
	init_gs();
	init_interrupts();
	upload_texture();
	build_ellipsoid();
	build_floor();
	build_view_screen();

	for (int i = 0; i < 32; i++)
	{
		s_probe_in[i][0] = sinf(i * 0.7f) * 0.3f;
		s_probe_in[i][1] = i * 0.06f;
		s_probe_in[i][2] = cosf(i * 0.7f) * 0.2f;
		s_probe_in[i][3] = 1.0f;
	}

	// Per batch 4 tags; per frame at most 1 + MAX_FIGURES * (BONES + 1) meshes.
	const int max_batches = s_floor.batches + MAX_FIGURES * (BONES + 1) * s_ellipsoid.batches;
	for (int i = 0; i < 2; i++)
	{
		s_blocks[i] = (qword_t*)memalign(64, sizeof(qword_t) * BLOCK_QW * MAX_BLOCKS);
		s_chain[i] = packet2_create(4 * max_batches + 4, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	}
	s_gs_setup = packet2_create(64, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);

	int ctx = 0, draw = 0, last_figures = 0;
	for (int frame = 0;; frame++)
	{
		const int figures = PHASE_COUNTS[(frame / PHASE_FRAMES) % PHASE_TOTAL];
		if (figures != last_figures)
		{
			printf("BENCH phase N=%d frame=%d\n", figures, frame);
			last_figures = figures;
		}

		// Build this frame while VU1 and the GS still draw the previous one.
		build_frame(ctx, frame, figures);
		FlushCache(0);

		if (frame > 0)
			wait_vif1_idle();
		wait_vsync();
		if (frame > 0)
			graph_set_framebuffer_filtered(s_frame[draw].address, SCREEN_W, GS_PSM_32, 0, 0);
		draw ^= 1;
		begin_gs_frame(draw);
		send_chain(s_chain[ctx]);
		ctx ^= 1;
	}
	return 0;
}
