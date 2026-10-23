// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The HW renderer's TFX shaders for the WebGPU device (docs/web-port/WEBGPU-DESIGN.md §9.2), ported from
// vulkan/tfx.glsl: the same structure, names and macros, with D3D-style clip space as in dx11/tfx.fx (no
// final Y flip). The device runs this file through WGSLPreprocessor with the macro list of
// GSDeviceVK::GetTFXVertexShader/GetTFXFragmentShader plus VERTEX_SHADER or FRAGMENT_SHADER, then creates
// one module per selector; macro names used in code are substituted by their values.
//
// Not ported: the ROV paths (rov = 0), the PrimID DATE paths (primitive_id = 0: PS_DATE 1-3 compile to
// stand-ins), triangle AA1 expansion (aa1 = 0). Feedback reads go through copies (multidraw_fb_copy,
// textures in group 2), and depth feedback reads a copy of the depth buffer (depth_feedback = 1), so there
// is no DS-as-RT output.
//
// Bindings (one pipeline layout for every TFX pipeline, §8.1):
//   group 0: 0 VS constants (uniform, dynamic offset), 1 PS constants (uniform, dynamic offset),
//            2 the vertex ring as a storage buffer (VS expansion)
//   group 1: 0 Texture (float), 1 Palette (float), 2 TextureRaw (unfilterable-float: depth and other
//            sources read with textureLoad, used when PS_DEPTH_FMT > 0), 3 the sampler
//   group 2: 0 RtTexture (copy of the RT), 1 PrimMinTexture, 2 DepthTexture (copy of the depth buffer)
//
// Synced with vulkan/tfx.glsl as of upstream commit 2026-10 (the tree this fork tracks).

#if FRAGMENT_SHADER && !PS_NO_COLOR && !PS_NO_COLOR1 && PS_DATE != 1 && PS_DATE != 2
enable dual_source_blending;
#endif

diagnostic(off, derivative_uniformity);

#define VS_EXPAND_NONE 0
#define VS_EXPAND_POINT 1
#define VS_EXPAND_LINE 2
#define VS_EXPAND_SPRITE 3
#define VS_EXPAND_LINE_AA1 4
#define VS_EXPAND_TRIANGLE_AA1 5

struct VSConstants
{
	VertexScale: vec2f,
	VertexOffset: vec2f,
	TextureScale: vec2f,
	TextureOffset: vec2f,
	PointSize: vec2f,
	MaxDepth: u32,
	LineAA1Width: f32,
};

//////////////////////////////////////////////////////////////////////
// Vertex Shader
//////////////////////////////////////////////////////////////////////

#if VERTEX_SHADER

@group(0) @binding(0) var<uniform> vs_cb: VSConstants;

struct VSOutput
{
	@builtin(position) p: vec4f,
	@location(0) t: vec4f,
	@location(1) ti: vec4f,
#if VS_IIP != 0
	@location(2) c: vec4f,
#else
	@location(2) @interpolate(flat) c: vec4f,
#endif
	@location(3) inv_cov: f32, // We use the inverse to make it simpler to interpolate.
	@location(4) @interpolate(flat) interior: u32, // 1 for triangle interior; 0 for edge;
};

struct ProcessedVertex
{
	p: vec4f,
	t: vec4f,
	ti: vec4f,
	c: vec4f,
};

fn process_vertex(a_st: vec2f, a_c: vec4u, a_q: f32, a_p: vec2u, a_z: u32, a_uv: vec2u, a_f: vec4f) -> ProcessedVertex
{
	var vtx: ProcessedVertex;

	// Clamp to max depth, gs doesn't wrap
	let z = min(a_z, vs_cb.MaxDepth);

	// pos -= 0.05 (1/320 pixel) helps avoiding rounding problems (integral part of pos is usually 5 digits, 0.05 is about as low as we can go)
	// example: ceil(afterseveralvertextransformations(y = 133)) => 134 => line 133 stays empty
	// input granularity is 1/16 pixel, anything smaller than that won't step drawing up/left by one pixel
	// example: 133.0625 (133 + 1/16) should start from line 134, ceil(133.0625 - 0.05) still above 133
	var p = vec4f(vec2f(a_p), f32(z), 1.0f) - vec4f(0.05f, 0.05f, 0.0f, 0.0f);
	// D3D/WebGPU clip space (+Y up), as dx11/tfx.fx; the Vulkan shader flips Y once more.
	let xy = p.xy * vec2f(vs_cb.VertexScale.x, -vs_cb.VertexScale.y) - vec2f(vs_cb.VertexOffset.x, -vs_cb.VertexOffset.y);
	vtx.p = vec4f(xy, p.z * exp2(-32.0f), p.w); // integer->float depth

#if VS_TME
	let uv = vec2f(a_uv) - vs_cb.TextureOffset;
	let st = a_st - vs_cb.TextureOffset;

	// Integer nomalized
	let ti_xy = uv * vs_cb.TextureScale;

#if VS_FST
	// Integer integral
	let ti_zw = uv;
#else
	// float for post-processing in some games
	let ti_zw = st / vs_cb.TextureScale;
#endif
	vtx.ti = vec4f(ti_xy, ti_zw);

	// Float coords
	vtx.t = vec4f(st, a_f.r, a_q);
#else
	vtx.t = vec4f(0.0f, 0.0f, a_f.r, 1.0f);
	vtx.ti = vec4f(0.0f);
#endif

	vtx.c = vec4f(a_c);
	return vtx;
}

fn output_vertex(vtx: ProcessedVertex) -> VSOutput
{
	var o: VSOutput;
	o.p = vtx.p;
	o.t = vtx.t;
	o.ti = vtx.ti;
	o.c = vtx.c;
	o.inv_cov = 0.0f;
	o.interior = 0u;
	return o;
}

#if VS_EXPAND == VS_EXPAND_NONE

@vertex
fn vs_main(@location(0) a_st: vec2f, @location(1) a_c: vec4u, @location(2) a_q: f32, @location(3) a_p: vec2u,
	@location(4) a_z: u32, @location(5) a_uv: vec2u, @location(6) a_f: vec4f) -> VSOutput
{
	return output_vertex(process_vertex(a_st, a_c, a_q, a_p, a_z, a_uv, a_f));
}

#else // VS_EXPAND

struct RawVertex
{
	ST: vec2f,
	RGBA: u32,
	Q: f32,
	XY: u32,
	Z: u32,
	UV: u32,
	FOG: u32,
};

@group(0) @binding(2) var<storage, read> vertex_buffer: array<RawVertex>;

// The base vertex arrives as the draw's first instance (§8.3).
var<private> BaseVertex: u32;

fn load_vertex(index: u32) -> ProcessedVertex
{
	let rvtx = vertex_buffer[BaseVertex + index];
	let a_c = vec4u(extractBits(rvtx.RGBA, 0u, 8u), extractBits(rvtx.RGBA, 8u, 8u),
		extractBits(rvtx.RGBA, 16u, 8u), extractBits(rvtx.RGBA, 24u, 8u));
	let a_p = vec2u(extractBits(rvtx.XY, 0u, 16u), extractBits(rvtx.XY, 16u, 16u));
	let a_uv = vec2u(extractBits(rvtx.UV, 0u, 16u), extractBits(rvtx.UV, 16u, 16u));
	return process_vertex(rvtx.ST, a_c, rvtx.Q, a_p, rvtx.Z, a_uv, unpack4x8unorm(rvtx.FOG));
}

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32, @builtin(instance_index) instance_index: u32) -> VSOutput
{
	BaseVertex = instance_index;
	var vtx: ProcessedVertex;
	let vid = vertex_index;
	var inv_cov = 0.0f;

#if VS_EXPAND == VS_EXPAND_POINT

	vtx = load_vertex(vid >> 2u);

	vtx.p.x += select(0.0f, vs_cb.PointSize.x, (vid & 1u) != 0u);
	vtx.p.y += select(0.0f, vs_cb.PointSize.y, (vid & 2u) != 0u);

#elif (VS_EXPAND == VS_EXPAND_LINE) || (VS_EXPAND == VS_EXPAND_LINE_AA1)

	let vid_base = vid >> 2u;

	let is_bottom = (vid & 2u) != 0u;
	let is_right = (vid & 1u) != 0u;
	let vid_other = select(vid_base + 1u, vid_base - 1u, is_bottom);

	vtx = load_vertex(vid_base);
	let other = load_vertex(vid_other);

	// Use bottom minus top for delta regardless of which vertex we are expanding.
	let line_delta = select(other.p.xy - vtx.p.xy, vtx.p.xy - other.p.xy, is_bottom);
	let line_vector = normalize(line_delta / vs_cb.VertexScale);
	var line_expand = vec2f(line_vector.y, -line_vector.x);
#if VS_EXPAND == VS_EXPAND_LINE_AA1
	line_expand *= 2.0f * vs_cb.LineAA1Width;
#endif
	let line_width = (line_expand * vs_cb.PointSize) / 2.0f;
	let offset = select(-line_width, line_width, is_right);
	vtx.p = vec4f(vtx.p.xy + offset, vtx.p.zw);

#if VS_EXPAND == VS_EXPAND_LINE_AA1
	inv_cov = select(-1.0f, 1.0f, is_right);
#endif

	// Lines will be run as (0 1 2) (1 2 3)
	// This means that both triangles will have a point based off the top line point as their first point
	// So we don't have to do anything for !IIP

#elif VS_EXPAND == VS_EXPAND_SPRITE

	// Sprite points are always in pairs
	let vid_base = vid >> 1u;
	let vid_lt = vid_base & ~1u;
	let vid_rb = vid_base | 1u;

	let lt = load_vertex(vid_lt);
	let rb = load_vertex(vid_rb);
	vtx = rb;

	let is_right = ((vid & 1u) != 0u);
	vtx.p.x = select(vtx.p.x, lt.p.x, is_right);
	vtx.t.x = select(vtx.t.x, lt.t.x, is_right);
	vtx.ti.x = select(vtx.ti.x, lt.ti.x, is_right);
	vtx.ti.z = select(vtx.ti.z, lt.ti.z, is_right);

	let is_bottom = ((vid & 2u) != 0u);
	vtx.p.y = select(vtx.p.y, lt.p.y, is_bottom);
	vtx.t.y = select(vtx.t.y, lt.t.y, is_bottom);
	vtx.ti.y = select(vtx.ti.y, lt.ti.y, is_bottom);
	vtx.ti.w = select(vtx.ti.w, lt.ti.w, is_bottom);

#endif

	var o = output_vertex(vtx);
	o.inv_cov = inv_cov;
	return o;
}

#endif // VS_EXPAND

#endif // VERTEX_SHADER

//////////////////////////////////////////////////////////////////////
// Fragment Shader
//////////////////////////////////////////////////////////////////////

#if FRAGMENT_SHADER

#define FMT_32 0
#define FMT_24 1
#define FMT_16 2

#define SHUFFLE_READ  1
#define SHUFFLE_WRITE 2
#define SHUFFLE_READWRITE 3

#define ZTST_GEQUAL 2
#define ZTST_GREATER 3

#define AFAIL_KEEP 0
#define AFAIL_FB_ONLY 1
#define AFAIL_ZB_ONLY 2
#define AFAIL_RGB_ONLY 3
#define AFAIL_RGB_ONLY_DSB 4
#define AFAIL_RGB_ONLY_SW_Z 5

#define PS_ATST_NONE 0
#define PS_ATST_LEQUAL 1
#define PS_ATST_GEQUAL 2
#define PS_ATST_EQUAL 3
#define PS_ATST_NOTEQUAL 4

#define PS_AA1_NONE 0
#define PS_AA1_LINE 1
#define PS_AA1_TRIANGLE 2
#define PS_AA1_TRIANGLE_SW_Z 3

#define SW_BLEND (PS_BLEND_A || PS_BLEND_B || PS_BLEND_D)
#define SW_BLEND_NEEDS_RT (SW_BLEND && (PS_BLEND_A == 1 || PS_BLEND_B == 1 || PS_BLEND_C == 1 || PS_BLEND_D == 1))
#define SW_AD_TO_HW (PS_BLEND_C == 1 && PS_A_MASKED)
#define AFAIL_NEEDS_RT (PS_AFAIL == AFAIL_ZB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
#define AFAIL_NEEDS_DEPTH (PS_AFAIL == AFAIL_FB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
#define ZTST_NEEDS_DEPTH (PS_ZTST == ZTST_GEQUAL || PS_ZTST == ZTST_GREATER)
#define AA1_NEEDS_DEPTH (PS_AA1 == PS_AA1_TRIANGLE_SW_Z)

#define PS_FEEDBACK_LOOP_IS_NEEDED_RT (PS_TEX_IS_FB == 1 || AFAIL_NEEDS_RT || PS_FBMASK || SW_BLEND_NEEDS_RT || SW_AD_TO_HW || (PS_DATE >= 5))
#define PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH (AFAIL_NEEDS_DEPTH || ZTST_NEEDS_DEPTH || AA1_NEEDS_DEPTH)
#define ZWRITE (PS_ZCLAMP || PS_ZFLOOR || PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH)

#define PS_RETURN_COLOR (!PS_NO_COLOR)
#define PS_RETURN_DEPTH (ZWRITE)
#define FS_HAS_OUTPUT (PS_RETURN_COLOR || PS_RETURN_DEPTH)

#define NEEDS_TEX (PS_TFX != 4)

struct PSConstants
{
	FogColor: vec3f,
	AREF: f32,
	WH: vec4f,
	TA: vec2f,
	MaxDepthPS: f32,
	Af: f32,
	FbMask: vec4u,
	HalfTexel: vec4f,
	MinMax: vec4f,
	LODParams: vec4f,
	STRange: vec4f,
	ChannelShuffle: vec4i,
	ChannelShuffleOffset: vec2f,
	TC_OffsetHack: vec2f,
	STScale: vec2f,
	DitherMatrix: mat4x4f,
	ScaledScaleFactor: f32,
	RcpScaleFactor: f32,
	_pad0_cb1: f32,
	_pad1_cb1: f32,
	LineCovScale: f32,
	_pad2_cb1: f32,
	_pad3_cb1: f32,
	_pad4_cb1: f32,
};

@group(0) @binding(1) var<uniform> ps_cb: PSConstants;

@group(1) @binding(0) var Texture: texture_2d<f32>;
@group(1) @binding(1) var Palette: texture_2d<f32>;
@group(1) @binding(2) var TextureRaw: texture_2d<f32>;
@group(1) @binding(3) var TextureSampler: sampler;
@group(2) @binding(0) var RtTexture: texture_2d<f32>;
@group(2) @binding(1) var PrimMinTexture: texture_2d<f32>;
@group(2) @binding(2) var DepthTexture: texture_2d<f32>;

struct FSInput
{
	@builtin(position) p: vec4f,
	@location(0) t: vec4f,
	@location(1) ti: vec4f,
#if PS_IIP != 0
	@location(2) c: vec4f,
#else
	@location(2) @interpolate(flat) c: vec4f,
#endif
	@location(3) inv_cov: f32,
	@location(4) @interpolate(flat) interior: u32,
};

#if FS_HAS_OUTPUT
struct FSOutput
{
#if PS_RETURN_COLOR
#if PS_DATE == 1 || PS_DATE == 2
	@location(0) c0: f32,
#elif !PS_NO_COLOR1
	@location(0) @blend_src(0) c0: vec4f,
	@location(0) @blend_src(1) c1: vec4f,
#else
	@location(0) c0: vec4f,
#endif
#endif
#if PS_RETURN_DEPTH
	@builtin(frag_depth) depth: f32,
#endif
};
#endif

var<private> vsIn: FSInput;
var<private> o_col0: vec4f;
var<private> o_col1: vec4f;

fn sample_from_rt() -> vec4f
{
	return textureLoad(RtTexture, vec2i(vsIn.p.xy), 0);
}

fn sample_from_depth() -> f32
{
	return textureLoad(DepthTexture, vec2i(vsIn.p.xy), 0).r;
}

fn is_nan_or_inf2(v: vec2f) -> bool
{
	let b = bitcast<vec2u>(v) & vec2u(0x7FFFFFFFu);
	return any(b >= vec2u(0x7F800000u));
}

/// texelFetch(Texture, ...): depth and other unfilterable sources are bound as TextureRaw.
fn tex_fetch(xy: vec2i) -> vec4f
{
#if PS_DEPTH_FMT > 0
	return textureLoad(TextureRaw, xy, 0);
#else
	return textureLoad(Texture, xy, 0);
#endif
}

#if NEEDS_TEX

fn manual_lod(uv_w: f32) -> f32
{
	// FIXME add LOD: K - ( LOG2(Q) * (1 << L))
	let K = ps_cb.LODParams.x;
	let L = ps_cb.LODParams.y;
	let bias = ps_cb.LODParams.z;
	let max_lod = ps_cb.LODParams.w;

	let gs_lod = K - log2(abs(uv_w)) * L;
	// FIXME max useful ?
	//return max(min(gs_lod, max_lod) - bias, 0.0f);
	return min(gs_lod, max_lod) - bias;
}

#if PS_ANISOTROPIC_FILTERING > 1
fn sample_c_af(uv_in: vec2f, uv_w: f32) -> vec4f
{
	// HW sampler will reject bad UVs, match that here.
	var uv = select(uv_in, vec2f(0.0f, 0.0f), is_nan_or_inf2(uv_in));

	// Large floating point values risk NaN/Inf values.
	// Above this value floats lose decimal precision, so seems a resonable limit for UVs.
	uv = clamp(uv, vec2f(-8388608.0f), vec2f(8388608.0f));

	// Below taken from https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#7.18.11%20LOD%20Calculations
	// And https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_filter_anisotropic.txt
	// With guidance from https://pema.dev/2025/05/09/mipmaps-too-much-detail/
	let sz = vec2f(textureDimensions(Texture, 0));
	var dX = dpdx(uv) * sz;
	var dY = dpdy(uv) * sz;

	var length_x = length(dX);
	var length_y = length(dY);

	// Calculate Ellipse Transform
	let d_zero = length_x < 0.001f || length_y < 0.001f;
	let f0 = (dX.x * dY.y - dX.y * dY.x);
	let d_par = f0 < 0.001f;
	let d_per = dot(dX, dY) < 0.001f;
	var d_inf_nan = is_nan_or_inf2(dX) || is_nan_or_inf2(dY);

	if (!(d_zero || d_par || d_per || d_inf_nan))
	{
		let A = dX.y * dX.y + dY.y * dY.y;
		let B = -2.0f * (dX.x * dX.y + dY.x * dY.y);
		let C = dX.x * dX.x + dY.x * dY.x;
		let f = (dX.x * dY.y - dY.x * dX.y);
		let F = f * f;

		let p = A - C;
		let q = A + C;
		let t = sqrt(p * p + B * B);

		let signB = sign(B);
		let denom_plus  = t * (q + t);
		let denom_minus = t * (q - t);

		let sqrtA = sqrt(F * (t + p));
		let sqrtB = sqrt(F * (t - p));

		let inv_sqrt_denom_plus  = inverseSqrt(denom_plus);
		let inv_sqrt_denom_minus = inverseSqrt(denom_minus);

		let new_dX = vec2f(sqrtA * inv_sqrt_denom_plus, sqrtB * inv_sqrt_denom_plus * signB);
		let new_dY = vec2f(sqrtB * inv_sqrt_denom_minus * -signB, sqrtA * inv_sqrt_denom_minus);

		d_inf_nan = is_nan_or_inf2(new_dX) || is_nan_or_inf2(new_dY);
		if (!d_inf_nan)
		{
			dX = new_dX;
			dY = new_dY;
			length_x = length(dX);
			length_y = length(dY);
		}
	}

	// Compute AF values
	let is_major_x = length_x > length_y;
	let length_major = select(length_y, length_x, is_major_x);
	let length_minor = select(length_x, length_y, is_major_x);

	var aniso_ratio: f32;
	var length_lod: f32;
	var aniso_line: vec2f;
	if (length_major <= 1.0f)
	{
		// A zero length_major would result in NaN Lod and break sampling.
		// A small length_major would result in aniso_ratio getting clamped to 1.
		// Perform isotropic filtering instead.
		aniso_ratio = 1.0f;
		length_lod = length_major;
		aniso_line = vec2f(0.0f, 0.0f);
	}
	else
	{
		let aniso_line_dir = select(dY, dX, is_major_x);

		aniso_ratio = min(length_major / length_minor, f32(PS_ANISOTROPIC_FILTERING));
		length_lod = length_major / aniso_ratio;

		// clamp to top Lod
		if (length_lod < 1.0f)
		{
			aniso_ratio = max(1.0f, aniso_ratio * length_lod);
		}

		aniso_ratio = round(aniso_ratio);

		aniso_line = aniso_line_dir * 0.5f * (1.0f / sz);
	}

#if PS_AUTOMATIC_LOD == 1
	let lod = log2(length_lod);
#elif PS_MANUAL_LOD == 1
	let lod = manual_lod(uv_w);
#else
	let lod = 0.0f; // No Lod
#endif

	var colour: vec4f;
	if (aniso_ratio == 1.0f)
	{
		colour = textureSampleLevel(Texture, TextureSampler, uv, lod);
	}
	else
	{
		var num = vec4f(0.0f, 0.0f, 0.0f, 0.0f);
		let seg = (2.0f * aniso_line) / aniso_ratio;
		for (var i = 0; f32(i) < aniso_ratio; i++)
		{
			let d = -aniso_line + (0.5f + f32(i)) * seg;
			let uv_sample = uv + d;
			num += textureSampleLevel(Texture, TextureSampler, uv_sample, lod);
		}

		colour = num / aniso_ratio;
	}
	return colour;
}
#endif

fn sample_c(uv_in: vec2f) -> vec4f
{
#if PS_TEX_IS_FB
	return sample_from_rt();
#elif PS_REGION_RECT
	return tex_fetch(vec2i(uv_in));
#else
	var uv = uv_in;

#if !PS_ADJS && !PS_ADJT
	uv *= ps_cb.STScale;
#else
	#if PS_ADJS
		uv.x = (uv.x - ps_cb.STRange.x) * ps_cb.STRange.z;
	#else
		uv.x = uv.x * ps_cb.STScale.x;
	#endif
	#if PS_ADJT
		uv.y = (uv.y - ps_cb.STRange.y) * ps_cb.STRange.w;
	#else
		uv.y = uv.y * ps_cb.STScale.y;
	#endif
#endif

#if PS_ANISOTROPIC_FILTERING > 1
	return sample_c_af(uv, vsIn.t.w);
#elif PS_AUTOMATIC_LOD == 1
	return textureSample(Texture, TextureSampler, uv);
#elif PS_MANUAL_LOD == 1
	return textureSampleLevel(Texture, TextureSampler, uv, manual_lod(vsIn.t.w));
#else
	return textureSampleLevel(Texture, TextureSampler, uv, 0.0f); // No lod
#endif
#endif
}

fn sample_p(idx: u32) -> vec4f
{
	return textureLoad(Palette, vec2i(i32(idx), 0), 0);
}

fn sample_p_norm(u: f32) -> vec4f
{
	return sample_p(u32(u * 255.5f));
}

fn clamp_wrap_uv(uv_in: vec4f) -> vec4f
{
	var uv = uv_in;
	let tex_size = ps_cb.WH.xyxy;
	let MinMax = ps_cb.MinMax;

#if PS_WMS == PS_WMT
	#if PS_REGION_RECT == 1 && PS_WMS == 0
		uv = fract(uv);
	#elif PS_REGION_RECT == 1 && PS_WMS == 1
		uv = clamp(uv, vec4f(0.0f), vec4f(1.0f));
	#elif PS_WMS == 2
		uv = clamp(uv, MinMax.xyxy, MinMax.zwzw);
	#elif PS_WMS == 3
		#if PS_FST == 0
		// wrap negative uv coords to avoid an off by one error that shifted
		// textures. Fixes Xenosaga's hair issue.
		uv = fract(uv);
		#endif
		uv = vec4f((vec4u(uv * tex_size) & bitcast<vec4u>(MinMax.xyxy)) | bitcast<vec4u>(MinMax.zwzw)) / tex_size;
	#endif
#else
	var uxz = uv.xz;
	var uyw = uv.yw;
	#if PS_REGION_RECT == 1 && PS_WMS == 0
		uxz = fract(uxz);
	#elif PS_REGION_RECT == 1 && PS_WMS == 1
		uxz = clamp(uxz, vec2f(0.0f), vec2f(1.0f));
	#elif PS_WMS == 2
		uxz = clamp(uxz, MinMax.xx, MinMax.zz);
	#elif PS_WMS == 3
		#if PS_FST == 0
		uxz = fract(uxz);
		#endif
		uxz = vec2f((vec2u(uxz * tex_size.xx) & bitcast<vec2u>(MinMax.xx)) | bitcast<vec2u>(MinMax.zz)) / tex_size.xx;
	#endif
	#if PS_REGION_RECT == 1 && PS_WMT == 0
		uyw = fract(uyw);
	#elif PS_REGION_RECT == 1 && PS_WMT == 1
		uyw = clamp(uyw, vec2f(0.0f), vec2f(1.0f));
	#elif PS_WMT == 2
		uyw = clamp(uyw, MinMax.yy, MinMax.ww);
	#elif PS_WMT == 3
		#if PS_FST == 0
		uyw = fract(uyw);
		#endif
		uyw = vec2f((vec2u(uyw * tex_size.yy) & bitcast<vec2u>(MinMax.yy)) | bitcast<vec2u>(MinMax.ww)) / tex_size.yy;
	#endif
	uv = vec4f(uxz.x, uyw.x, uxz.y, uyw.y);
#endif

#if PS_REGION_RECT == 1
	// Normalized -> Integer Coordinates.
	uv = clamp(uv * ps_cb.WH.zwzw + ps_cb.STRange.xyxy, ps_cb.STRange.xyxy, ps_cb.STRange.zwzw);
#endif

	return uv;
}

fn sample_4c(uv: vec4f) -> mat4x4f
{
	var c: mat4x4f;

	c[0] = sample_c(uv.xy);
	c[1] = sample_c(uv.zy);
	c[2] = sample_c(uv.xw);
	c[3] = sample_c(uv.zw);

	return c;
}

fn sample_4_index(uv: vec4f) -> vec4u
{
	var c: vec4f;

	c.x = sample_c(uv.xy).a;
	c.y = sample_c(uv.zy).a;
	c.z = sample_c(uv.xw).a;
	c.w = sample_c(uv.zw).a;

	// Denormalize value

#if PS_RTA_SRC_CORRECTION
	let i = vec4u(round(c * 128.25f));
#else
	let i = vec4u(c * 255.5f);
#endif

#if PS_PAL_FMT == 1
	// 4HL
	return i & vec4u(0xFu);
#elif PS_PAL_FMT == 2
	// 4HH
	return i >> vec4u(4u);
#else
	// 8
	return i;
#endif
}

fn sample_4p(u: vec4u) -> mat4x4f
{
	var c: mat4x4f;

	c[0] = sample_p(u.x);
	c[1] = sample_p(u.y);
	c[2] = sample_p(u.z);
	c[3] = sample_p(u.w);

	return c;
}

fn fetch_raw_depth(xy: vec2i) -> u32
{
#if PS_TEX_IS_FB
	let col = sample_from_rt();
#else
	let col = tex_fetch(xy);
#endif
	return u32(col.r * exp2(32.0f));
}

fn fetch_raw_color(xy: vec2i) -> vec4f
{
#if PS_TEX_IS_FB
	return sample_from_rt();
#else
	return tex_fetch(xy);
#endif
}

fn fetch_c(uv: vec2i) -> vec4f
{
#if PS_TEX_IS_FB
	return sample_from_rt();
#else
	return tex_fetch(uv);
#endif
}

//////////////////////////////////////////////////////////////////////
// Depth sampling
//////////////////////////////////////////////////////////////////////

fn clamp_wrap_uv_depth(uv_in: vec2i) -> vec2i
{
	var uv = uv_in;
	let mask = bitcast<vec4i>(ps_cb.MinMax) << vec4u(4u);
#if (PS_WMS == PS_WMT)
	#if (PS_WMS == 2)
		uv = clamp(uv, mask.xy, mask.zw);
	#elif (PS_WMS == 3)
		uv = (uv & mask.xy) | mask.zw;
	#endif
#else
	#if (PS_WMS == 2)
		uv.x = clamp(uv.x, mask.x, mask.z);
	#elif (PS_WMS == 3)
		uv.x = (uv.x & mask.x) | mask.z;
	#endif
	#if (PS_WMT == 2)
		uv.y = clamp(uv.y, mask.y, mask.w);
	#elif (PS_WMT == 3)
		uv.y = (uv.y & mask.y) | mask.w;
	#endif
#endif
	return uv;
}

fn sample_depth(st: vec2f, pos: vec2i) -> vec4f
{
	var uv_f = vec2f(clamp_wrap_uv_depth(vec2i(st))) * vec2f(ps_cb.ScaledScaleFactor);

#if PS_REGION_RECT == 1
	uv_f = clamp(uv_f + ps_cb.STRange.xy, ps_cb.STRange.xy, ps_cb.STRange.zw);
#endif

	let uv = vec2i(uv_f);
	var t = vec4f(0.0f);

#if (PS_TALES_OF_ABYSS_HLE == 1)
	// Warning: UV can't be used in channel effect
	let depth = fetch_raw_depth(pos);

	// Convert msb based on the palette
	t = textureLoad(Palette, vec2i(i32((depth >> 8u) & 0xFFu), 0), 0) * 255.0f;
#elif (PS_URBAN_CHAOS_HLE == 1)
	// Depth buffer is read as a RGB5A1 texture. The game try to extract the green channel.
	// So it will do a first channel trick to extract lsb, value is right-shifted.
	// Then a new channel trick to extract msb which will shifted to the left.
	// OpenGL uses a vec32 format for the depth so it requires a couple of conversion.
	// To be faster both steps (msb&lsb) are done in a single pass.

	// Warning: UV can't be used in channel effect
	let depth = fetch_raw_depth(pos);

	// Convert lsb based on the palette
	t = textureLoad(Palette, vec2i(i32(depth & 0xFFu), 0), 0) * 255.0f;

	// Msb is easier
	var green = f32((depth >> 8u) & 0xFFu) * 36.0f;
	green = min(green, 255.0f);
	t.g += green;
#elif (PS_DEPTH_FMT == 1)
	// Based on ps_convert_depth32_rgba8 of convert

	// Convert a vec32 depth texture into a RGBA color texture
	let d = u32(fetch_c(uv).r * exp2(32.0f));
	t = vec4f(vec4u((d & 0xFFu), ((d >> 8u) & 0xFFu), ((d >> 16u) & 0xFFu), (d >> 24u)));
#elif (PS_DEPTH_FMT == 2)
	// Based on ps_convert_depth16_rgb5a1 of convert

	// Convert a vec32 (only 16 lsb) depth into a RGB5A1 color texture
	let d = u32(fetch_c(uv).r * exp2(32.0f));
	t = vec4f(vec4u((d & 0x1Fu), ((d >> 5u) & 0x1Fu), ((d >> 10u) & 0x1Fu), (d >> 15u) & 0x01u)) * vec4f(8.0f, 8.0f, 8.0f, 128.0f);
#elif (PS_DEPTH_FMT == 3)
	// Convert a RGBA/RGB5A1 color texture into a RGBA/RGB5A1 color texture
	t = fetch_c(uv) * 255.0f;
#endif

#if (PS_AEM_FMT == FMT_24)
	t.a = select(0.0f, 255.0f * ps_cb.TA.x, (PS_AEM == 0) || any(t.rgb != vec3f(0.0f)));
#elif (PS_AEM_FMT == FMT_16)
	t.a = select(select(0.0f, 255.0f * ps_cb.TA.x, (PS_AEM == 0) || any(t.rgb != vec3f(0.0f))), 255.0f * ps_cb.TA.y, t.a >= 128.0f);
#elif PS_PAL_FMT != 0 && !PS_TALES_OF_ABYSS_HLE && !PS_URBAN_CHAOS_HLE
	t = trunc(sample_4p(vec4u(t.aaaa))[0] * 255.0f + 0.05f);
#endif

	return t;
}

//////////////////////////////////////////////////////////////////////
// Fetch a Single Channel
//////////////////////////////////////////////////////////////////////

fn fetch_red(xy: vec2i) -> vec4f
{
#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
	let depth = (fetch_raw_depth(xy)) & 0xFFu;
	let rt = vec4f(f32(depth) / 255.0f);
#else
	let rt = fetch_raw_color(xy);
#endif

	return sample_p_norm(rt.r) * 255.0f;
}

fn fetch_green(xy: vec2i) -> vec4f
{
#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
	let depth = (fetch_raw_depth(xy) >> 8u) & 0xFFu;
	let rt = vec4f(f32(depth) / 255.0f);
#else
	let rt = fetch_raw_color(xy);
#endif

	return sample_p_norm(rt.g) * 255.0f;
}

fn fetch_blue(xy: vec2i) -> vec4f
{
#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
	let depth = (fetch_raw_depth(xy) >> 16u) & 0xFFu;
	let rt = vec4f(f32(depth) / 255.0f);
#else
	let rt = fetch_raw_color(xy);
#endif

	return sample_p_norm(rt.b) * 255.0f;
}

fn fetch_alpha(xy: vec2i) -> vec4f
{
	let rt = fetch_raw_color(xy);
	return sample_p_norm(rt.a) * 255.0f;
}

fn fetch_rgb(xy: vec2i) -> vec4f
{
	let rt = fetch_raw_color(xy);
	let c = vec4f(sample_p_norm(rt.r).r, sample_p_norm(rt.g).g, sample_p_norm(rt.b).b, 1.0f);
	return c * 255.0f;
}

fn fetch_gXbY(xy: vec2i) -> vec4f
{
#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
	let depth = fetch_raw_depth(xy);
	let bg = (depth >> (8u + u32(ps_cb.ChannelShuffle.w))) & 0xFFu;
	return vec4f(f32(bg));
#else
	let rt = vec4i(fetch_raw_color(xy) * 255.0f);
	let green = (rt.g >> u32(ps_cb.ChannelShuffle.w)) & ps_cb.ChannelShuffle.z;
	let blue = (rt.b << u32(ps_cb.ChannelShuffle.y)) & ps_cb.ChannelShuffle.x;
	return vec4f(f32(green | blue));
#endif
}

fn sample_color(st_in: vec2f) -> vec4f
{
	var st = st_in;
#if PS_TCOFFSETHACK
	st += ps_cb.TC_OffsetHack.xy;
#endif

	var t: vec4f;
	var c: mat4x4f;
	var dd = vec2f(0.0f);

#if PS_LTF == 0 && PS_AEM_FMT == FMT_32 && PS_PAL_FMT == 0 && PS_REGION_RECT == 0 && PS_WMS < 2 && PS_WMT < 2
	c[0] = sample_c(st);
#else
	var uv: vec4f;

	#if PS_LTF
		uv = st.xyxy + ps_cb.HalfTexel;
		dd = fract(uv.xy * ps_cb.WH.zw);

		#if PS_FST == 0
			dd = clamp(dd, vec2f(0.0f), vec2f(0.9999999f));
		#endif
	#else
		uv = st.xyxy;
	#endif

	uv = clamp_wrap_uv(uv);

	#if PS_PAL_FMT != 0
		c = sample_4p(sample_4_index(uv));
	#else
		c = sample_4c(uv);
	#endif
#endif

	for (var i = 0; i < 4; i++)
	{
	#if (PS_AEM_FMT == FMT_24)
		c[i].a = select(0.0f, ps_cb.TA.x, PS_AEM == 0 || any(c[i].rgb != vec3f(0.0f)));
	#elif (PS_AEM_FMT == FMT_16)
		c[i].a = select(select(0.0f, ps_cb.TA.x, PS_AEM == 0 || any((vec3i(c[i].rgb * 255.0f) & vec3i(0xF8)) != vec3i(0))), ps_cb.TA.y, c[i].a >= 0.5f);
	#endif
	}

#if PS_LTF
	t = mix(mix(c[0], c[1], dd.x), mix(c[2], c[3], dd.x), dd.y);
#else
	t = c[0];
#endif
#if PS_AEM_FMT == FMT_32 && PS_PAL_FMT == 0 && PS_RTA_SRC_CORRECTION
	t.a = t.a * (128.5f / 255.0f);
#endif
	return trunc(t * 255.0f + 0.05f);
}

#endif // NEEDS_TEX

fn tfx(T: vec4f, C: vec4f) -> vec4f
{
	var C_out: vec4f;
	let FxT = trunc((C * T) / 128.0f);

#if (PS_TFX == 0)
	C_out = FxT;
#elif (PS_TFX == 1)
	C_out = T;
#elif (PS_TFX == 2)
	C_out = vec4f(FxT.rgb + C.a, T.a + C.a);
#elif (PS_TFX == 3)
	C_out = vec4f(FxT.rgb + C.a, T.a);
#else
	C_out = C;
#endif

#if (PS_TCC == 0)
	C_out.a = C.a;
#endif

#if (PS_TFX == 0) || (PS_TFX == 2) || (PS_TFX == 3)
	// Clamp only when it is useful
	C_out = min(C_out, vec4f(255.0f));
#endif

	return C_out;
}

fn atst(C: vec4f) -> bool
{
	let a = C.a;

#if PS_ATST == PS_ATST_LEQUAL
	return (a <= ps_cb.AREF);
#elif PS_ATST == PS_ATST_GEQUAL
	return (a >= ps_cb.AREF);
#elif PS_ATST == PS_ATST_EQUAL
	return (abs(a - ps_cb.AREF) <= 0.5f);
#elif PS_ATST == PS_ATST_NOTEQUAL
	return (abs(a - ps_cb.AREF) >= 0.5f);
#else
	return true;
#endif
}

fn fog(c: vec4f, f: f32) -> vec4f
{
#if PS_FOG
	return vec4f(trunc(mix(ps_cb.FogColor, c.rgb, (f * 255.0f) / 256.0f)), c.a);
#else
	return c;
#endif
}

fn ps_color() -> vec4f
{
#if PS_FST == 0
	let st = vsIn.t.xy / vsIn.t.w;
	let st_int = vsIn.ti.zw / vsIn.t.w;
#else
	let st = vsIn.ti.xy;
	let st_int = vsIn.ti.zw;
#endif

#if !NEEDS_TEX
	var T = vec4f(0.0f);
#elif PS_CHANNEL_FETCH == 1
	var T = fetch_red(vec2i(vsIn.p.xy + ps_cb.ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 2
	var T = fetch_green(vec2i(vsIn.p.xy + ps_cb.ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 3
	var T = fetch_blue(vec2i(vsIn.p.xy + ps_cb.ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 4
	var T = fetch_alpha(vec2i(vsIn.p.xy + ps_cb.ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 5
	var T = fetch_rgb(vec2i(vsIn.p.xy + ps_cb.ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 6
	var T = fetch_gXbY(vec2i(vsIn.p.xy + ps_cb.ChannelShuffleOffset));
#elif PS_DEPTH_FMT > 0
	var T = sample_depth(st_int, vec2i(vsIn.p.xy));
#else
	var T = sample_color(st);
#endif

#if PS_SHUFFLE && !PS_READ16_SRC && !PS_SHUFFLE_SAME && !(PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE)
	let denorm_c_before = vec4u(T);
	#if (PS_PROCESS_BA & SHUFFLE_READ)
		T.r = f32((denorm_c_before.b << 3u) & 0xF8u);
		T.g = f32(((denorm_c_before.b >> 2u) & 0x38u) | ((denorm_c_before.a << 6u) & 0xC0u));
		T.b = f32((denorm_c_before.a << 1u) & 0xF8u);
		T.a = f32(denorm_c_before.a & 0x80u);
	#else
		T.r = f32((denorm_c_before.r << 3u) & 0xF8u);
		T.g = f32(((denorm_c_before.r >> 2u) & 0x38u) | ((denorm_c_before.g << 6u) & 0xC0u));
		T.b = f32((denorm_c_before.g << 1u) & 0xF8u);
		T.a = f32(denorm_c_before.g & 0x80u);
	#endif

	T.a = select(select(0.0f, ps_cb.TA.x, PS_AEM == 0 || any((vec3i(T.rgb) & vec3i(0xF8)) != vec3i(0))), ps_cb.TA.y, T.a >= 127.5f) * 255.0f;
#endif

	var C = tfx(T, vsIn.c);

	C = fog(C, vsIn.t.z);

	return C;
}

fn ps_fbmask(C: vec4f) -> vec4f
{
#if PS_FBMASK
	let multi_rgb = select(255.0f, 65535.0f, PS_COLCLIP_HW != 0);
	let multi_a = select(255.0f, 128.0f, PS_RTA_CORRECTION != 0);
	var RT = sample_from_rt();
	RT = vec4f(trunc(RT.rgb * multi_rgb + 0.1f), round(RT.a * multi_a));
	return vec4f((vec4u(C) & ~ps_cb.FbMask) | (vec4u(RT) & ps_cb.FbMask));
#else
	return C;
#endif
}

fn ps_dither(C: vec3f, As: f32) -> vec3f
{
#if PS_DITHER > 0 && PS_DITHER < 3
	#if PS_DITHER == 2
		let fpos = vec2i(vsIn.p.xy);
	#else
		let fpos = vec2i(vsIn.p.xy * ps_cb.RcpScaleFactor);
	#endif

	var value = ps_cb.DitherMatrix[fpos.y & 3][fpos.x & 3];

	// The idea here is we add on the dither amount adjusted by the alpha before it goes to the hw blend
	// so after the alpha blend the resulting value should be the same as (Cs - Cd) * As + Cd + Dither.
	#if PS_DITHER_ADJUST
		#if PS_BLEND_C == 2
			let Alpha = ps_cb.Af;
		#else
			let Alpha = As;
		#endif

		value *= select(1.0f, min(1.0f / Alpha, 1.0f), Alpha > 0.0f);
	#endif

	#if PS_ROUND_INV
		return C - value;
	#else
		return C + value;
	#endif
#else
	return C;
#endif
}

fn ps_color_clamp_wrap(C_in: vec3f) -> vec3f
{
	var C = C_in;
	// When dithering the bottom 3 bits become meaningless and cause lines in the picture
	// so we need to limit the color depth on dithered items
#if SW_BLEND || (PS_DITHER > 0 && PS_DITHER < 3) || PS_FBMASK

#if PS_DST_FMT == FMT_16 && PS_BLEND_MIX == 0 && PS_ROUND_INV
	C += 7.0f; // Need to round up, not down since the shader will invert
#endif

	// Correct the Color value based on the output format
#if PS_COLCLIP == 0 && PS_COLCLIP_HW == 0
	// Standard Clamp
	C = clamp(C, vec3f(0.0f), vec3f(255.0f));
#endif

	// FIXME rouding of negative float?
	// compiler uses trunc but it might need floor

	// Warning: normally blending equation is mult(A, B) = A * B >> 7. GPU have the full accuracy
	// GS: Color = 1, Alpha = 255 => output 1
	// GPU: Color = 1/255, Alpha = 255/255 * 255/128 => output 1.9921875
#if PS_DST_FMT == FMT_16 && PS_DITHER != 3 && (PS_BLEND_MIX == 0 || PS_DITHER > 0)
	// In 16 bits format, only 5 bits of colors are used. It impacts shadows computation of Castlevania
	C = vec3f(vec3i(C) & vec3i(0xF8));
#elif PS_COLCLIP == 1 || PS_COLCLIP_HW == 1
	C = vec3f(vec3i(C) & vec3i(0xFF));
#endif

#elif PS_DST_FMT == FMT_16 && PS_DITHER != 3 && PS_BLEND_MIX == 0 && PS_BLEND_HW == 0
	C = vec3f(vec3i(C) & vec3i(0xF8));
#endif
	return C;
}

fn ps_blend(Color: ptr<function, vec4f>, As_rgba: ptr<function, vec4f>)
{
	let As = (*As_rgba).a;

#if SW_BLEND

	// PABE
	#if PS_PABE
		// As_rgba needed for accumulation blend to manipulate Cd
		// No blending so early exit
		if (As < 1.0f)
		{
			*As_rgba = vec4f(vec3f(0.0f), (*As_rgba).a);
			return;
		}

		*As_rgba = vec4f(vec3f(1.0f), (*As_rgba).a);
	#endif

	#if SW_BLEND_NEEDS_RT
		var RT = sample_from_rt();
		#if PS_COLCLIP_HW
			let color_multi = 65535.0f;
		#else
			let color_multi = 255.0f;
		#endif
		#if PS_RTA_CORRECTION
			let alpha_multi = 128.0f;
		#else
			let alpha_multi = 255.0f;
		#endif
		RT = vec4f(trunc(RT.rgb * color_multi + 0.1f), trunc(RT.a * alpha_multi + 0.1f));
	#else
		// Not used, but we define it to make the selection below simpler.
		var RT = vec4f(0.0f);
	#endif

	#if PS_SHUFFLE && SW_BLEND_NEEDS_RT
		let denorm_rt = vec4u(RT);
		#if (PS_PROCESS_BA & SHUFFLE_WRITE)
			RT.r = f32((denorm_rt.b << 3u) & 0xF8u);
			RT.g = f32(((denorm_rt.b >> 2u) & 0x38u) | ((denorm_rt.a << 6u) & 0xC0u));
			RT.b = f32((denorm_rt.a << 1u) & 0xF8u);
			RT.a = f32(denorm_rt.a & 0x80u);
		#else
			RT.r = f32((denorm_rt.r << 3u) & 0xF8u);
			RT.g = f32(((denorm_rt.r >> 2u) & 0x38u) | ((denorm_rt.g << 6u) & 0xC0u));
			RT.b = f32((denorm_rt.g << 1u) & 0xF8u);
			RT.a = f32(denorm_rt.g & 0x80u);
		#endif
	#endif

	let Ad = RT.a / 128.0f;
	let Cd = RT.rgb;
	let Cs = (*Color).rgb;

	#if PS_BLEND_A == 0
		let A = Cs;
	#elif PS_BLEND_A == 1
		let A = Cd;
	#else
		let A = vec3f(0.0f);
	#endif

	#if PS_BLEND_B == 0
		let B = Cs;
	#elif PS_BLEND_B == 1
		let B = Cd;
	#else
		let B = vec3f(0.0f);
	#endif

	#if PS_BLEND_C == 0
		let C = As;
	#elif PS_BLEND_C == 1
		let C = Ad;
	#else
		let C = ps_cb.Af;
	#endif

	#if PS_BLEND_D == 0
		let D = Cs;
	#elif PS_BLEND_D == 1
		let D = Cd;
	#else
		let D = vec3f(0.0f);
	#endif

	// As/Af clamp alpha for Blend mix
	// We shouldn't clamp blend mix with blend hw 1 as we want alpha higher
	var C_clamped = C;
	#if PS_BLEND_MIX > 0 && PS_BLEND_HW != 1 && PS_BLEND_HW != 2
		C_clamped = min(C_clamped, 1.0f);
	#endif

	var rgb: vec3f;
	#if PS_BLEND_A == PS_BLEND_B
		rgb = D;
	// In blend_mix, HW adds on some alpha factor * dst.
	// Truncating here wouldn't quite get the right result because it prevents the <1 bit here from combining with a <1 bit in dst to form a ≥1 amount that pushes over the truncation.
	// Instead, apply an offset to convert HW's round to a floor.
	// Since alpha is in 1/128 increments, subtracting (0.5 - 0.5/128 == 127/256) would get us what we want if GPUs blended in full precision.
	// But they don't.  Details here: https://github.com/PCSX2/pcsx2/pull/6809#issuecomment-1211473399
	// Based on the scripts at the above link, the ideal choice for Intel GPUs is 126/256, AMD 120/256.  Nvidia is a lost cause.
	// 124/256 seems like a reasonable compromise, providing the correct answer 99.3% of the time on Intel (vs 99.6% for 126/256), and 97% of the time on AMD (vs 97.4% for 120/256).
	#elif PS_BLEND_MIX == 2
		rgb = ((A - B) * C_clamped + D) + (124.0f/256.0f);
	#elif PS_BLEND_MIX == 1
		rgb = ((A - B) * C_clamped + D) - (124.0f/256.0f);
	#else
		rgb = trunc((A - B) * C + D);
	#endif

	#if PS_BLEND_HW == 1
		// As or Af
		var as_rgb = vec3f(C);
		// Subtract 1 for alpha to compensate for the changed equation,
		// if c.rgb > 255.0f then we further need to adjust alpha accordingly,
		// we pick the lowest overflow from all colors because it's the safest,
		// we divide by 255 the color because we don't know Cd value,
		// changed alpha should only be done for hw blend.
		let alpha_compensate = max(vec3f(1.0f), rgb / vec3f(255.0f));
		as_rgb -= alpha_compensate;
		*As_rgba = vec4f(as_rgb, (*As_rgba).a);
	#elif PS_BLEND_HW == 2
		// Since we can't do Cd*(Aalpha + 1) - Cs*Alpha in hw blend
		// what we can do is adjust the Cs value that will be
		// subtracted, this way we can get a better result in hw blend.
		// Result is still wrong but less wrong than before.
		let division_alpha = 1.0f + C;
		rgb /= vec3f(division_alpha);
	#elif PS_BLEND_HW == 3
		// As, Ad or Af clamped.
		var as_rgb = vec3f(C_clamped);
		// Cs*(Alpha + 1) might overflow, if it does then adjust alpha value
		// that is sent on second output to compensate.
		let overflow_check = (rgb - vec3f(255.0f)) / 255.0f;
		let alpha_compensate = max(vec3f(0.0f), overflow_check);
		as_rgb -= alpha_compensate;
		*As_rgba = vec4f(as_rgb, (*As_rgba).a);
	#endif

	*Color = vec4f(rgb, (*Color).a);

#else

	#if PS_BLEND_C == 2
		var Alpha = vec3f(ps_cb.Af);
	#else
		var Alpha = vec3f(As);
	#endif

	var rgb = (*Color).rgb;
	#if PS_BLEND_HW == 1
		// Needed for Cd * (As/Ad/F + 1) blending modes
		rgb = vec3f(255.0f);
	#elif PS_BLEND_HW == 2
		// Cd*As,Cd*Ad or Cd*F

		rgb = max(vec3f(0.0f), (Alpha - vec3f(1.0f)));
		rgb *= vec3f(255.0f);
	#elif PS_BLEND_HW == 3 && PS_RTA_CORRECTION == 0
		// Needed for Cs*Ad, Cs*Ad + Cd, Cd - Cs*Ad
		// Multiply Color.rgb by (255/128) to compensate for wrong Ad/255 value when rgb are below 128.
		// When any color channel is higher than 128 then adjust the compensation automatically
		// to give us more accurate colors, otherwise they will be wrong.
		// The higher the value (>128) the lower the compensation will be.
		let max_color = max(max(rgb.r, rgb.g), rgb.b);
		let color_compensate = 255.0f / max(128.0f, max_color);
		rgb *= vec3f(color_compensate);
	#elif PS_BLEND_HW == 4
		// Needed for Cd * (1 - Ad) and Cd*(1 + Alpha).

		*As_rgba = vec4f(Alpha * vec3f(128.0f / 255.0f), (*As_rgba).a);
		rgb = vec3f(127.5f);
	#elif PS_BLEND_HW == 5
		// Needed for Cs*Alpha + Cd*(1 - Alpha).
		Alpha *= vec3f(128.0f / 255.0f);
		*As_rgba = vec4f(Alpha - vec3f(0.5f), (*As_rgba).a);
		rgb = (rgb * Alpha);
	#elif PS_BLEND_HW == 6
		// Needed for Cd*Alpha + Cs*(1 - Alpha).
		Alpha *= vec3f(128.0f / 255.0f);
		*As_rgba = vec4f(Alpha, (*As_rgba).a);
		rgb *= (Alpha - vec3f(0.5f));
	#endif
	*Color = vec4f(rgb, (*Color).a);
#endif
}

#if COMPUTE_SHADER
struct CSResult
{
	written: bool,
	c: vec4f,
};

// The compute variant of ps_main (cs_main below): the same body, returning instead of discarding.
#define PS_DISCARD return CSResult(false, vec4f(0.0f))
fn ps_main_cs(input: FSInput) -> CSResult
#elif FS_HAS_OUTPUT
#define PS_DISCARD discard
@fragment
fn ps_main(input: FSInput) -> FSOutput
#else
#define PS_DISCARD discard
@fragment
fn ps_main(input: FSInput)
#endif
{
	vsIn = input;
	var input_z = input.p.z;

	// Must floor before depth testing.
#if PS_ZFLOOR
	input_z = floor(input_z * exp2(32.0f)) * exp2(-32.0f);
#endif

#if PS_ZTST == ZTST_GEQUAL
	if (input_z < sample_from_depth())
	{
		PS_DISCARD;
	}
#elif PS_ZTST == ZTST_GREATER
	if (input_z <= sample_from_depth())
	{
		PS_DISCARD;
	}
#endif

#if PS_SCANMSK & 2
	// fail depth test on prohibited lines
	if ((i32(input.p.y) & 1) == (PS_SCANMSK & 1))
	{
		PS_DISCARD;
	}
#endif

#if PS_DATE >= 5

#if PS_WRITE_RG == 1
	// Pseudo 16 bits access.
	let rt_a = sample_from_rt().g;
#else
	let rt_a = sample_from_rt().a;
#endif

#if (PS_DATE & 3) == 1
	// DATM == 0: Pixel with alpha equal to 1 will failed
	#if PS_RTA_CORRECTION
		let bad = (254.5f / 255.0f) < rt_a;
	#else
		let bad = (127.5f / 255.0f) < rt_a;
	#endif
#elif (PS_DATE & 3) == 2
	// DATM == 1: Pixel with alpha equal to 0 will failed
	#if PS_RTA_CORRECTION
		let bad = rt_a < (254.5f / 255.0f);
	#else
		let bad = rt_a < (127.5f / 255.0f);
	#endif
#else
	let bad = false;
#endif

	if (bad)
	{
		PS_DISCARD;
	}

#endif // PS_DATE >= 5

	var C = ps_color();

#if PS_AA1
	#if PS_AA1 == PS_AA1_LINE
		// Blur only outer part of the line by scaling coverage.
		let cov = clamp(ps_cb.LineCovScale * (1.0f - abs(input.inv_cov)), 0.0f, 1.0f);
	#else
		let cov = clamp(1.0f - abs(input.inv_cov), 0.0f, 1.0f);
	#endif
	#if PS_ABE
		if (floor(C.a) == 128.0f) // The coverage is only used if the fragment alpha is 128.
		{
			C.a = 128.0f * cov;
		}
	#else
		C.a = 128.0f * cov;
	#endif
#elif PS_FIXED_ONE_A
	// AA (Fixed one) will output a coverage of 1.0 as alpha
	C.a = 128.0f;
#endif

	let atst_pass = atst(C);

#if PS_ATST != PS_ATST_NONE && PS_AFAIL == AFAIL_KEEP
	if (!atst_pass)
	{
		PS_DISCARD;
	}
#endif

#if SW_AD_TO_HW
	#if PS_RTA_CORRECTION
		let RT = trunc(sample_from_rt() * 128.0f + 0.1f);
	#else
		let RT = trunc(sample_from_rt() * 255.0f + 0.1f);
	#endif

	var alpha_blend = vec4f(RT.a / 128.0f);
#else
	var alpha_blend = vec4f(C.a / 128.0f);
#endif

	// Correct the ALPHA value based on the output format
#if (PS_DST_FMT == FMT_16)
	let A_one = 128.0f; // alpha output will be 0x80
	C.a = select(step(128.0f, C.a) * A_one, A_one, PS_FBA != 0);
#elif (PS_DST_FMT == FMT_32) && (PS_FBA != 0)
	if (C.a < 128.0f)
	{
		C.a += 128.0f;
	}
#endif

	// Get first primitive that will write a failling alpha value
#if PS_DATE == 1 || PS_DATE == 2

	// The PrimID DATE paths need primitive_index (primitive_id = 0 on this device): never selected.
	o_col0 = vec4f(0.0f);

#else
	ps_blend(&C, &alpha_blend);

	#if PS_SHUFFLE
		#if !PS_READ16_SRC && !PS_SHUFFLE_SAME && !(PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE)
			let denorm_c_after = vec4u(C);
			#if (PS_PROCESS_BA & SHUFFLE_READ)
				C.b = f32(((denorm_c_after.r >> 3u) & 0x1Fu) | ((denorm_c_after.g << 2u) & 0xE0u));
				C.a = f32(((denorm_c_after.g >> 6u) & 0x3u) | ((denorm_c_after.b >> 1u) & 0x7Cu) | (denorm_c_after.a & 0x80u));
			#else
				C.r = f32(((denorm_c_after.r >> 3u) & 0x1Fu) | ((denorm_c_after.g << 2u) & 0xE0u));
				C.g = f32(((denorm_c_after.g >> 6u) & 0x3u) | ((denorm_c_after.b >> 1u) & 0x7Cu) | (denorm_c_after.a & 0x80u));
			#endif
		#endif

		// Special case for 32bit input and 16bit output, shuffle used by The Godfather
		#if PS_SHUFFLE_SAME
			#if (PS_PROCESS_BA & SHUFFLE_READ)
				let denorm_c = vec4u(C);
				C = vec4f(f32((denorm_c.b & 0x7Fu) | (denorm_c.a & 0x80u)));
			#else
				C = vec4f(C.r, C.r, C.b, C.g); // C.ga = C.rg
			#endif
		// Copy of a 16bit source in to this target
		#elif PS_READ16_SRC
			let denorm_c = vec4u(C);
			let denorm_TA = vec2u(vec2f(ps_cb.TA.xy) * 255.0f + 0.5f);
			let rb = f32((denorm_c.r >> 3u) | (((denorm_c.g >> 3u) & 0x7u) << 5u));
			let ga = f32((denorm_c.g >> 6u) | ((denorm_c.b >> 3u) << 2u) | (denorm_TA.x & 0x80u));
			C = vec4f(rb, ga, rb, ga);
		// Write RB part. Mask will take care of the correct destination
		#elif PS_SHUFFLE_ACROSS
			#if (PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE)
				C = vec4f(C.b, C.a, C.r, C.g); // C.br = C.rb; C.ag = C.ga;
			#elif (PS_PROCESS_BA & SHUFFLE_READ)
				C = vec4f(C.b, C.a, C.b, C.a); // C.rb = C.bb; C.ga = C.aa;
			#else
				C = vec4f(C.r, C.g, C.r, C.g); // C.rb = C.rr; C.ga = C.gg;
			#endif // PS_PROCESS_BA
		#endif // PS_SHUFFLE_ACROSS
	#endif // PS_SHUFFLE

	C = vec4f(ps_dither(C.rgb, alpha_blend.a), C.a);

	// Color clamp/wrap needs to be done after sw blending and dithering
	C = vec4f(ps_color_clamp_wrap(C.rgb), C.a);

	C = ps_fbmask(C);

	#if (PS_AFAIL == AFAIL_RGB_ONLY_DSB) && !PS_NO_COLOR1
		// Use alpha blend factor to determine whether to update A.
		alpha_blend.a = select(0.0f, 1.0f, atst_pass);
	#endif

	// Output color scaling
	#if !PS_NO_COLOR
		#if PS_RTA_CORRECTION
			let out_a = C.a / 128.0f;
		#else
			let out_a = C.a / 255.0f;
		#endif
		#if PS_COLCLIP_HW == 1
			o_col0 = vec4f(C.rgb / 65535.0f, out_a);
		#else
			o_col0 = vec4f(C.rgb / 255.0f, out_a);
		#endif
		#if !PS_NO_COLOR1
			o_col1 = alpha_blend;
		#endif

		// Alpha test with feedback
		#if PS_AFAIL == AFAIL_FB_ONLY
			if (!atst_pass)
			{
				input_z = sample_from_depth();
			}
		#elif PS_AFAIL == AFAIL_ZB_ONLY
			if (!atst_pass)
			{
				o_col0 = sample_from_rt();
			}
		#elif (PS_AFAIL == AFAIL_RGB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
			if (!atst_pass)
			{
				o_col0.a = sample_from_rt().a; // discard alpha
			#if PS_AFAIL == AFAIL_RGB_ONLY_SW_Z
				input_z = sample_from_depth();
			#endif
			}
		#endif
	#endif

	#if PS_ZCLAMP
		input_z = min(input_z, ps_cb.MaxDepthPS);
	#endif

	#if PS_AA1 == PS_AA1_TRIANGLE_SW_Z
		if (input.interior == 0u)
		{
			input_z = sample_from_depth(); // No depth update for triangle edges.
		}
	#endif
#endif // PS_DATE

#if COMPUTE_SHADER
	return CSResult(true, o_col0);
#elif FS_HAS_OUTPUT
	var result: FSOutput;
	#if PS_RETURN_COLOR
		#if PS_DATE == 1 || PS_DATE == 2
			result.c0 = o_col0.r;
		#else
			result.c0 = o_col0;
			#if !PS_NO_COLOR1
				result.c1 = o_col1;
			#endif
		#endif
	#endif
	#if PS_RETURN_DEPTH
		result.depth = input_z;
	#endif
	return result;
#endif
}

#if COMPUTE_SHADER

// Feedback draws that read only their own pixel, run as compute dispatches (GSDeviceWebGPU::
// TryComputeFeedbackDraw()): every invocation reads the target as it was before the draw (RtTexture, as the
// render path reads its copy) and writes the whole target to cs_out, the draw's result where one of its
// rectangles covers the pixel and the fragment was not discarded (under the colour mask), the old value
// elsewhere. The two textures swap for the next draw. Only PS selectors whose fragment code uses nothing of
// the vertices but the pixel's position are run this way, so the inputs below are the whole of it.
struct CSParams
{
	rects: array<vec4i, 8>, // x0, y0, x1, y1: the pixels covered, half-open
	count: u32,
	colormask: u32,
	_pad0: u32,
	_pad1: u32,
};

@group(3) @binding(0) var cs_out: texture_storage_2d<rgba8unorm, write>;
@group(3) @binding(1) var<uniform> cs: CSParams;

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3u)
{
	let size = textureDimensions(RtTexture);
	if (gid.x >= size.x || gid.y >= size.y)
	{
		return;
	}
	let xy = vec2i(gid.xy);
	let old = textureLoad(RtTexture, xy, 0);
	var covered = false;
	for (var i = 0u; i < cs.count; i++)
	{
		let r = cs.rects[i];
		covered = covered || (xy.x >= r.x && xy.y >= r.y && xy.x < r.z && xy.y < r.w);
	}
	var result_c = old;
	if (covered)
	{
		var input: FSInput;
		input.p = vec4f(vec2f(gid.xy) + vec2f(0.5f), 0.0f, 1.0f);
		input.t = vec4f(0.0f);
		input.ti = vec4f(0.0f);
		input.c = vec4f(0.0f);
		input.inv_cov = 0.0f;
		input.interior = 0u;
		let res = ps_main_cs(input);
		if (res.written)
		{
			let m = cs.colormask;
			result_c = select(old, res.c, vec4<bool>((m & 1u) != 0u, (m & 2u) != 0u, (m & 4u) != 0u, (m & 8u) != 0u));
		}
	}
	textureStore(cs_out, xy, result_c);
}

#endif // COMPUTE_SHADER

#endif // FRAGMENT_SHADER
