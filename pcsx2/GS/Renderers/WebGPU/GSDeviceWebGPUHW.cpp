// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The HW renderer's half of GSDeviceWebGPU (docs/web-port/WEBGPU-DESIGN.md §8, §10, step 2 of §15):
// TFX shaders generated from tfx.wgsl, their pipelines and bind groups, RenderHW with feedback
// through copies (the DX11 profile), stencil DATE, and the convert paths the texture cache uses.

#include "GS/Renderers/WebGPU/GSDeviceWebGPU.h"
#include "GS/Renderers/WebGPU/WGSLPreprocessor.h"
#include "GS/GSPerfMon.h"

#include "common/Console.h"
#include "common/HashCombine.h"
#include "common/ScopedGuard.h"

#include "fmt/format.h"

#include <emscripten/console.h>

#include <cstring>

namespace
{
	constexpr u32 UNIFORM_SLOT = 256;
	constexpr u32 VS_CB_SIZE = sizeof(GSHWDrawConfig::VSConstantBuffer);
	constexpr u32 PS_CB_SIZE = sizeof(GSHWDrawConfig::PSConstantBuffer);
	constexpr u32 VS_CB_SLOT = (VS_CB_SIZE + UNIFORM_SLOT - 1) / UNIFORM_SLOT * UNIFORM_SLOT;
	constexpr u32 PS_CB_SLOT = (PS_CB_SIZE + UNIFORM_SLOT - 1) / UNIFORM_SLOT * UNIFORM_SLOT;
	constexpr double TFX_PIPELINE_TIMEOUT_MS = 10000.0;
	constexpr size_t MAX_CACHED_GROUPS = 4096;

	static_assert(VS_CB_SIZE == 48 && PS_CB_SIZE == 272, "tfx.wgsl's uniform structs follow these sizes");

	WGPUStringView SV(const char* s)
	{
		return {s, WGPU_STRLEN};
	}

	// GSDevice::BlendFactor order, as GSDeviceVK's table.
	constexpr WGPUBlendFactor s_blend_factors[16] = {
		WGPUBlendFactor_Src, WGPUBlendFactor_OneMinusSrc, WGPUBlendFactor_Dst, WGPUBlendFactor_OneMinusDst,
		WGPUBlendFactor_Src1, WGPUBlendFactor_OneMinusSrc1, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha,
		WGPUBlendFactor_DstAlpha, WGPUBlendFactor_OneMinusDstAlpha, WGPUBlendFactor_Src1Alpha,
		WGPUBlendFactor_OneMinusSrc1Alpha, WGPUBlendFactor_Constant, WGPUBlendFactor_OneMinusConstant,
		WGPUBlendFactor_One, WGPUBlendFactor_Zero};
	constexpr WGPUBlendOperation s_blend_ops[3] = {
		WGPUBlendOperation_Add, WGPUBlendOperation_Subtract, WGPUBlendOperation_ReverseSubtract};
	constexpr WGPUCompareFunction s_ztst[4] = {
		WGPUCompareFunction_Never, WGPUCompareFunction_Always, WGPUCompareFunction_GreaterEqual, WGPUCompareFunction_Greater};

	bool HasStencil(WGPUTextureFormat f)
	{
		return f == WGPUTextureFormat_Depth32FloatStencil8;
	}

	/// Formats a float/filterable texture binding accepts (the others are unfilterable or integer).
	bool IsFilterableFloat(WGPUTextureFormat f)
	{
		switch (f)
		{
			case WGPUTextureFormat_Depth32Float:
			case WGPUTextureFormat_Depth32FloatStencil8:
			case WGPUTextureFormat_R32Float:
			case WGPUTextureFormat_R16Uint:
			case WGPUTextureFormat_R32Uint:
				return false;
			default:
				return true;
		}
	}

	bool IsUnfilterableCompatible(WGPUTextureFormat f)
	{
		return f != WGPUTextureFormat_R16Uint && f != WGPUTextureFormat_R32Uint;
	}

	void LogOnce(bool& flag, const std::string& msg)
	{
		if (!std::exchange(flag, true))
			Console.Warning(msg);
	}
} // namespace

size_t GSDeviceWebGPU::TFXPipelineKeyHash::operator()(const TFXPipelineKey& k) const
{
	size_t h = 0;
	HashCombine(h, k.ps.key_lo, k.ps.key_hi, k.blend, k.vs, k.dss, k.cms, k.topology, static_cast<u32>(k.rt_format),
		static_cast<u32>(k.ds_format));
	return h;
}

size_t GSDeviceWebGPU::TFXGroupKeyHash::operator()(const TFXGroupKey& k) const
{
	size_t h = 0;
	HashCombine(h, reinterpret_cast<uintptr_t>(k.views[0]), reinterpret_cast<uintptr_t>(k.views[1]),
		reinterpret_cast<uintptr_t>(k.views[2]), reinterpret_cast<uintptr_t>(k.sampler));
	return h;
}

// ----------------------------------------------------------------------------
// Resources
// ----------------------------------------------------------------------------

bool GSDeviceWebGPU::CreateHWResources()
{
	const std::optional<std::string> source = ReadShaderSource("shaders/webgpu/tfx.wgsl");
	if (!source.has_value())
	{
		Console.Error("WebGPU: failed to read shaders/webgpu/tfx.wgsl");
		return false;
	}
	m_tfx_source = *source;

	// §8.1: group 0 the per-draw constants and the vertex ring (VS expansion), group 1 the source
	// textures and sampler, group 2 the feedback copies.
	{
		WGPUBindGroupLayoutEntry e[3] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
			WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
		e[0].binding = 0;
		e[0].visibility = WGPUShaderStage_Vertex;
		e[0].buffer.type = WGPUBufferBindingType_Uniform;
		e[0].buffer.hasDynamicOffset = true;
		e[0].buffer.minBindingSize = VS_CB_SIZE;
		e[1].binding = 1;
		e[1].visibility = WGPUShaderStage_Fragment;
		e[1].buffer.type = WGPUBufferBindingType_Uniform;
		e[1].buffer.hasDynamicOffset = true;
		e[1].buffer.minBindingSize = PS_CB_SIZE;
		e[2].binding = 2;
		e[2].visibility = WGPUShaderStage_Vertex;
		e[2].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		d.label = SV("tfx constants");
		d.entryCount = 3;
		d.entries = e;
		m_tfx_bgl[0] = wgpuDeviceCreateBindGroupLayout(m_device, &d);
	}
	{
		WGPUBindGroupLayoutEntry e[4] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
			WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
		for (u32 i = 0; i < 3; i++)
		{
			e[i].binding = i;
			e[i].visibility = WGPUShaderStage_Fragment;
			e[i].texture.sampleType = (i == 2) ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
			e[i].texture.viewDimension = WGPUTextureViewDimension_2D;
		}
		e[3].binding = 3;
		e[3].visibility = WGPUShaderStage_Fragment;
		e[3].sampler.type = WGPUSamplerBindingType_Filtering;
		WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		d.label = SV("tfx textures");
		d.entryCount = 4;
		d.entries = e;
		m_tfx_bgl[1] = wgpuDeviceCreateBindGroupLayout(m_device, &d);
	}
	{
		WGPUBindGroupLayoutEntry e[3] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
			WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
		for (u32 i = 0; i < 3; i++)
		{
			e[i].binding = i;
			e[i].visibility = WGPUShaderStage_Fragment;
			e[i].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
			e[i].texture.viewDimension = WGPUTextureViewDimension_2D;
		}
		WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		d.label = SV("tfx feedback");
		d.entryCount = 3;
		d.entries = e;
		m_tfx_bgl[2] = wgpuDeviceCreateBindGroupLayout(m_device, &d);
	}
	{
		WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pd.label = SV("tfx");
		pd.bindGroupLayoutCount = 3;
		pd.bindGroupLayouts = m_tfx_bgl;
		m_tfx_layout = wgpuDeviceCreatePipelineLayout(m_device, &pd);
	}
	{
		WGPUBindGroupEntry e[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
		e[0].binding = 0;
		e[0].buffer = m_uniform_buffer.GetBuffer();
		e[0].size = VS_CB_SIZE;
		e[1].binding = 1;
		e[1].buffer = m_uniform_buffer.GetBuffer();
		e[1].size = PS_CB_SIZE;
		e[2].binding = 2;
		e[2].buffer = m_vertex_buffer.GetBuffer();
		e[2].size = m_vertex_buffer.GetSize();
		WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		d.label = SV("tfx constants");
		d.layout = m_tfx_bgl[0];
		d.entryCount = 3;
		d.entries = e;
		m_tfx_group0 = wgpuDeviceCreateBindGroup(m_device, &d);
	}
	{
		// The fixed index buffer for point and sprite expansion.
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.label = SV("expand index buffer");
		bd.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
		bd.size = Common::AlignUpPow2(EXPAND_BUFFER_SIZE, 4);
		m_expand_ib = wgpuDeviceCreateBuffer(m_device, &bd);
		std::vector<u8> data(bd.size);
		GenerateExpansionIndexBuffer(data.data());
		wgpuQueueWriteBuffer(m_queue, m_expand_ib, 0, data.data(), data.size());
	}
	{
		WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
		td.label = SV("null texture");
		td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
		td.size = {1, 1, 1};
		td.format = WGPUTextureFormat_RGBA8Unorm;
		m_null_texture = wgpuDeviceCreateTexture(m_device, &td);
		m_null_view = wgpuTextureCreateView(m_null_texture, nullptr);
		const u32 zero = 0;
		WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		dst.texture = m_null_texture;
		WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		layout.bytesPerRow = 4;
		layout.rowsPerImage = 1;
		const WGPUExtent3D size = {1, 1, 1};
		wgpuQueueWriteTexture(m_queue, &dst, &zero, sizeof(zero), &layout, &size);
	}
	m_null_group2 = GetTFXGroup(2, TFXGroupKey{{m_null_view, m_null_view, m_null_view}, nullptr});

	return m_tfx_bgl[0] && m_tfx_bgl[1] && m_tfx_bgl[2] && m_tfx_layout && m_tfx_group0 && m_expand_ib &&
		   m_null_texture && m_null_group2;
}

void GSDeviceWebGPU::DestroyHWResources()
{
	for (auto& it : m_tfx_pipelines)
	{
		if (it.second)
			wgpuRenderPipelineRelease(it.second);
	}
	m_tfx_pipelines.clear();
	for (auto& it : m_tfx_vs_modules)
	{
		if (it.second)
			wgpuShaderModuleRelease(it.second);
	}
	m_tfx_vs_modules.clear();
	for (auto& it : m_tfx_fs_modules)
	{
		if (it.second)
			wgpuShaderModuleRelease(it.second);
	}
	m_tfx_fs_modules.clear();
	for (auto& groups : m_tfx_groups)
	{
		for (auto& it : groups)
			wgpuBindGroupRelease(it.second);
		groups.clear();
	}
	m_null_group2 = nullptr;
	if (m_tfx_group0)
		wgpuBindGroupRelease(m_tfx_group0);
	m_tfx_group0 = nullptr;
	if (m_tfx_layout)
		wgpuPipelineLayoutRelease(m_tfx_layout);
	m_tfx_layout = nullptr;
	for (WGPUBindGroupLayout& l : m_tfx_bgl)
	{
		if (l)
			wgpuBindGroupLayoutRelease(l);
		l = nullptr;
	}
	if (m_expand_ib)
	{
		wgpuBufferDestroy(m_expand_ib);
		wgpuBufferRelease(m_expand_ib);
		m_expand_ib = nullptr;
	}
	if (m_null_view)
		wgpuTextureViewRelease(m_null_view);
	m_null_view = nullptr;
	if (m_null_texture)
	{
		wgpuTextureDestroy(m_null_texture);
		wgpuTextureRelease(m_null_texture);
		m_null_texture = nullptr;
	}
}

// ----------------------------------------------------------------------------
// Shaders and pipelines
// ----------------------------------------------------------------------------

WGPUShaderModule GSDeviceWebGPU::GetTFXVertexModule(GSHWDrawConfig::VSSelector sel)
{
	const auto it = m_tfx_vs_modules.find(sel.key);
	if (it != m_tfx_vs_modules.end())
		return it->second;

	const WGSLPreprocessor::Macros macros = {
		{"VERTEX_SHADER", "1"},
		{"VS_TME", std::to_string(sel.tme)},
		{"VS_FST", std::to_string(sel.fst)},
		{"VS_IIP", std::to_string(sel.iip)},
		{"VS_POINT_SIZE", std::to_string(sel.point_size)},
		{"VS_EXPAND", std::to_string(static_cast<int>(sel.expand))},
		{"VS_PROVOKING_VERTEX_LAST", "0"},
	};
	std::string error;
	const std::string code = WGSLPreprocessor::Process(m_tfx_source, macros, &error);
	if (!error.empty())
		Console.ErrorFmt("WebGPU: tfx.wgsl (vertex {:02x}): {}", sel.key, error);
	WGPUShaderModule mod = CreateShaderModule(fmt::format("tfx vs {:02x}", sel.key).c_str(), code);
	m_tfx_vs_modules.emplace(sel.key, mod);
	return mod;
}

WGPUShaderModule GSDeviceWebGPU::GetTFXFragmentModule(const GSHWDrawConfig::PSSelector& sel)
{
	const auto it = m_tfx_fs_modules.find(sel);
	if (it != m_tfx_fs_modules.end())
		return it->second;

	// The macro list of GSDeviceVK::GetTFXFragmentShader.
	WGSLPreprocessor::Macros macros;
	macros.reserve(64);
	auto add = [&macros](const char* n, u32 v) { macros.emplace_back(n, std::to_string(v)); };
	macros.emplace_back("FRAGMENT_SHADER", "1");
	add("PS_FST", sel.fst);
	add("PS_WMS", sel.wms);
	add("PS_WMT", sel.wmt);
	add("PS_ADJS", sel.adjs);
	add("PS_ADJT", sel.adjt);
	add("PS_AEM_FMT", sel.aem_fmt);
	add("PS_PAL_FMT", sel.pal_fmt);
	add("PS_DST_FMT", sel.dst_fmt);
	add("PS_DEPTH_FMT", sel.depth_fmt);
	add("PS_CHANNEL_FETCH", sel.channel);
	add("PS_URBAN_CHAOS_HLE", sel.urban_chaos_hle);
	add("PS_TALES_OF_ABYSS_HLE", sel.tales_of_abyss_hle);
	add("PS_AEM", sel.aem);
	add("PS_TFX", sel.tfx);
	add("PS_TCC", sel.tcc);
	add("PS_ATST", static_cast<u32>(sel.atst));
	add("PS_AFAIL", static_cast<u32>(sel.afail));
	add("PS_FOG", sel.fog);
	add("PS_BLEND_HW", sel.blend_hw);
	add("PS_A_MASKED", sel.a_masked);
	add("PS_FBA", sel.fba);
	add("PS_LTF", sel.ltf);
	add("PS_AUTOMATIC_LOD", sel.automatic_lod);
	add("PS_MANUAL_LOD", sel.manual_lod);
	add("PS_COLCLIP", sel.colclip);
	add("PS_DATE", sel.date);
	add("PS_TCOFFSETHACK", sel.tcoffsethack);
	add("PS_REGION_RECT", sel.region_rect);
	add("PS_BLEND_A", sel.blend_a);
	add("PS_BLEND_B", sel.blend_b);
	add("PS_BLEND_C", sel.blend_c);
	add("PS_BLEND_D", sel.blend_d);
	add("PS_BLEND_MIX", sel.blend_mix);
	add("PS_ROUND_INV", sel.round_inv);
	add("PS_FIXED_ONE_A", sel.fixed_one_a);
	add("PS_IIP", sel.iip);
	add("PS_SHUFFLE", sel.shuffle);
	add("PS_SHUFFLE_SAME", sel.shuffle_same);
	add("PS_PROCESS_BA", sel.process_ba);
	add("PS_PROCESS_RG", sel.process_rg);
	add("PS_SHUFFLE_ACROSS", sel.shuffle_across);
	add("PS_READ16_SRC", sel.real16src);
	add("PS_WRITE_RG", sel.write_rg);
	add("PS_FBMASK", sel.fbmask);
	add("PS_COLCLIP_HW", sel.colclip_hw);
	add("PS_RTA_CORRECTION", sel.rta_correction);
	add("PS_RTA_SRC_CORRECTION", sel.rta_source_correction);
	add("PS_DITHER", sel.dither);
	add("PS_DITHER_ADJUST", sel.dither_adjust);
	add("PS_ZCLAMP", sel.zclamp);
	add("PS_ZFLOOR", sel.zfloor);
	add("PS_PABE", sel.pabe);
	add("PS_SCANMSK", sel.scanmsk);
	add("PS_TEX_IS_FB", sel.tex_is_fb);
	add("PS_NO_COLOR", sel.no_color);
	add("PS_NO_COLOR1", sel.no_color1);
	add("PS_ZTST", sel.ztst);
	add("PS_AA1", static_cast<u32>(sel.aa1));
	add("PS_ABE", sel.abe);
	add("PS_ANISOTROPIC_FILTERING", sel.sw_aniso);
	add("PS_ROV_COLOR", 0);
	add("PS_ROV_DEPTH", 0);

	std::string error;
	const std::string code = WGSLPreprocessor::Process(m_tfx_source, macros, &error);
	if (!error.empty())
		Console.ErrorFmt("WebGPU: tfx.wgsl (fragment {:016x}_{:016x}): {}", sel.key_hi, sel.key_lo, error);
	WGPUShaderModule mod = CreateShaderModule(fmt::format("tfx ps {:016x}_{:016x}", sel.key_hi, sel.key_lo).c_str(), code);
	m_tfx_fs_modules.emplace(sel, mod);
	return mod;
}

GSDeviceWebGPU::TFXPipelineKey GSDeviceWebGPU::MakeTFXKey(const GSHWDrawConfig& config,
	const GSHWDrawConfig::PSSelector& ps, GSHWDrawConfig::BlendState bs, GSHWDrawConfig::ColorMaskSelector cms,
	GSHWDrawConfig::DepthStencilSelector dss, GSTextureWebGPU* rt, GSTextureWebGPU* ds) const
{
	TFXPipelineKey key;
	key.ps = ps;
	key.vs = config.vs.key;
	key.topology = static_cast<u8>(config.topology);
	key.rt_format = rt ? rt->GetNativeFormat() : WGPUTextureFormat_Undefined;
	key.ds_format = ds ? ds->GetNativeFormat() : WGPUTextureFormat_Undefined;

	if (!ds)
		dss = GSHWDrawConfig::DepthStencilSelector::NoDepth();
	if (!HasStencil(key.ds_format))
	{
		dss.date = 0;
		dss.date_one = 0;
	}
	if (!rt)
	{
		cms = GSHWDrawConfig::ColorMaskSelector(0);
		key.ps.no_color = 1;
	}

	// Normalisation as GSDeviceVK::CreateTFXPipeline, and the second output only when a factor reads it
	// (dual-source output needs exactly one colour target and is pointless otherwise).
	if (key.ps.no_color || !bs.IsEffective(cms))
	{
		bs = {};
		key.ps.no_color1 = 1;
	}
	else if (!bs.enable || (!IsDualSourceBlendFactor(bs.src_factor) && !IsDualSourceBlendFactor(bs.dst_factor) &&
							   !IsDualSourceBlendFactor(bs.src_factor_alpha) && !IsDualSourceBlendFactor(bs.dst_factor_alpha)))
	{
		key.ps.no_color1 = 1;
	}
	if (!bs.enable)
		bs = {};

	key.blend = bs.key;
	key.dss = dss.key;
	key.cms = cms.key;
	return key;
}

WGPURenderPipeline GSDeviceWebGPU::GetTFXPipeline(const TFXPipelineKey& key)
{
	const auto it = m_tfx_pipelines.find(key);
	if (it != m_tfx_pipelines.end())
		return it->second;

	WGPURenderPipeline pipeline = CreateTFXPipeline(key);
	m_tfx_pipelines.emplace(key, pipeline);
	return pipeline;
}

WGPURenderPipeline GSDeviceWebGPU::CreateTFXPipeline(const TFXPipelineKey& key)
{
	GSHWDrawConfig::VSSelector vs(key.vs);
	GSHWDrawConfig::BlendState bs;
	bs.key = key.blend;
	GSHWDrawConfig::ColorMaskSelector cms;
	cms.key = key.cms;
	const GSHWDrawConfig::DepthStencilSelector dss(key.dss);

	WGPUShaderModule vsm = GetTFXVertexModule(vs);
	WGPUShaderModule fsm = GetTFXFragmentModule(key.ps);
	if (!vsm || !fsm)
		return nullptr;

	static constexpr WGPUVertexAttribute attrs[] = {
		{nullptr, WGPUVertexFormat_Float32x2, 0, 0}, // ST
		{nullptr, WGPUVertexFormat_Uint8x4, 8, 1}, // RGBA
		{nullptr, WGPUVertexFormat_Float32, 12, 2}, // Q
		{nullptr, WGPUVertexFormat_Uint16x2, 16, 3}, // XY
		{nullptr, WGPUVertexFormat_Uint32, 20, 4}, // Z
		{nullptr, WGPUVertexFormat_Uint16x2, 24, 5}, // UV
		{nullptr, WGPUVertexFormat_Unorm8x4, 28, 6}, // FOG
	};
	WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
	vbl.stepMode = WGPUVertexStepMode_Vertex;
	vbl.arrayStride = sizeof(GSVertex);
	vbl.attributeCount = std::size(attrs);
	vbl.attributes = attrs;

	WGPUBlendState blend = {};
	blend.color.operation = s_blend_ops[std::min<u32>(bs.op, 2)];
	blend.color.srcFactor = s_blend_factors[bs.src_factor];
	blend.color.dstFactor = s_blend_factors[bs.dst_factor];
	blend.alpha.operation = WGPUBlendOperation_Add;
	blend.alpha.srcFactor = s_blend_factors[bs.src_factor_alpha];
	blend.alpha.dstFactor = s_blend_factors[bs.dst_factor_alpha];

	WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
	target.format = key.rt_format;
	target.writeMask = key.ps.no_color ? 0 : cms.wrgba;
	target.blend = bs.enable ? &blend : nullptr;

	WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
	fs.module = fsm;
	fs.entryPoint = SV("ps_main");
	fs.targetCount = (key.rt_format != WGPUTextureFormat_Undefined) ? 1 : 0;
	fs.targets = &target;

	WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
	ds.format = key.ds_format;
	const bool depth_test = (dss.ztst != ZTST_ALWAYS || dss.zwe);
	ds.depthWriteEnabled = dss.zwe ? WGPUOptionalBool_True : WGPUOptionalBool_False;
	ds.depthCompare = depth_test ? s_ztst[dss.ztst] : WGPUCompareFunction_Always;
	if (dss.date)
	{
		ds.stencilFront.compare = WGPUCompareFunction_Equal;
		ds.stencilFront.failOp = WGPUStencilOperation_Keep;
		ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
		ds.stencilFront.passOp = dss.date_one ? WGPUStencilOperation_Zero : WGPUStencilOperation_Keep;
		ds.stencilBack = ds.stencilFront;
		ds.stencilReadMask = 1;
		ds.stencilWriteMask = 1;
	}
	else
	{
		ds.stencilReadMask = 0;
		ds.stencilWriteMask = 0;
	}

	static constexpr WGPUPrimitiveTopology topologies[] = {
		WGPUPrimitiveTopology_PointList, WGPUPrimitiveTopology_LineList, WGPUPrimitiveTopology_TriangleList};

	WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	const std::string label = fmt::format("tfx {:02x}/{:016x}_{:016x} b{:08x} d{:02x} m{:x} t{} rt{} ds{}", key.vs,
		key.ps.key_hi, key.ps.key_lo, key.blend, key.dss, key.cms, key.topology, static_cast<int>(key.rt_format),
		static_cast<int>(key.ds_format));
	d.label = {label.data(), label.size()};
	d.layout = m_tfx_layout;
	d.vertex.module = vsm;
	d.vertex.entryPoint = SV("vs_main");
	d.vertex.bufferCount = (vs.expand == GSHWDrawConfig::VSExpand::None) ? 1 : 0;
	d.vertex.buffers = &vbl;
	d.primitive.topology = topologies[std::min<u32>(key.topology, 2)];
	d.primitive.cullMode = WGPUCullMode_None;
	d.depthStencil = (key.ds_format != WGPUTextureFormat_Undefined) ? &ds : nullptr;
	d.fragment = &fs;

	// §8.4: created asynchronously, then waited for (the hitch desktop PCSX2 has on a compile).
	using Request = WebGPUHost::RequestResult<WGPURenderPipeline>;
	Request* const req = new Request();
	WGPUCreateRenderPipelineAsyncCallbackInfo cb = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
	cb.mode = WGPUCallbackMode_WaitAnyOnly;
	cb.userdata1 = req;
	cb.callback = [](WGPUCreatePipelineAsyncStatus status, WGPURenderPipeline pipeline, WGPUStringView message, void* u1, void*) {
		Request* const r = Request::Arrive(u1);
		if (status != WGPUCreatePipelineAsyncStatus_Success || !r)
		{
			if (r)
			{
				const std::string text = fmt::format("WebGPU: TFX pipeline creation failed: {}", WebGPUHost::StringViewToString(message));
				emscripten_console_error(text.c_str());
			}
			if (pipeline)
				wgpuRenderPipelineRelease(pipeline);
			pipeline = nullptr;
		}
		if (r)
			r->value = pipeline;
	};
	const double t0 = WebGPUHost::Now();
	const WGPUFuture future = wgpuDeviceCreateRenderPipelineAsync(m_device, &d, cb);
	WebGPUHost::Wait(m_instance, future, TFX_PIPELINE_TIMEOUT_MS);
	WGPURenderPipeline pipeline = nullptr;
	if (!req->Take(&pipeline))
	{
		Console.ErrorFmt("WebGPU: timed out creating {}", label);
		MarkDeviceLost("a TFX pipeline creation");
		return nullptr;
	}
	if (!pipeline)
		Console.ErrorFmt("WebGPU: {} could not be created; its draws are skipped.", label);
	else if (WebGPUHost::Now() - t0 > 50.0)
		Console.WriteLnFmt("WebGPU: {} took {:.0f} ms", label, WebGPUHost::Now() - t0);
	return pipeline;
}

// ----------------------------------------------------------------------------
// Bind groups
// ----------------------------------------------------------------------------

WGPUBindGroup GSDeviceWebGPU::GetTFXGroup(u32 group, const TFXGroupKey& key)
{
	auto& cache = m_tfx_groups[group];
	const auto it = cache.find(key);
	if (it != cache.end())
		return it->second;

	if (cache.size() >= MAX_CACHED_GROUPS)
	{
		// Groups the open encoder used stay alive until it is submitted.
		for (auto& e : cache)
		{
			if (e.second == m_null_group2)
				continue;
			if (m_encoder)
				m_pending_release_groups.push_back(e.second);
			else
				wgpuBindGroupRelease(e.second);
		}
		cache.clear();
		for (WGPUBindGroup& g : m_pass_tfx_groups)
			g = nullptr;
		if (group == 2 && m_null_group2)
			cache.emplace(TFXGroupKey{{m_null_view, m_null_view, m_null_view}, nullptr}, m_null_group2);
	}

	WGPUBindGroupEntry e[4] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
		WGPU_BIND_GROUP_ENTRY_INIT};
	for (u32 i = 0; i < 3; i++)
	{
		e[i].binding = i;
		e[i].textureView = key.views[i];
	}
	u32 count = 3;
	if (group == 1)
	{
		e[3].binding = 3;
		e[3].sampler = key.sampler;
		count = 4;
	}
	WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	d.layout = m_tfx_bgl[group];
	d.entryCount = count;
	d.entries = e;
	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(m_device, &d);
	cache.emplace(key, bg);
	return bg;
}

void GSDeviceWebGPU::DropTFXGroupsUsing(std::initializer_list<WGPUTextureView> views)
{
	for (u32 g = 1; g < 3; g++)
	{
		auto& cache = m_tfx_groups[g];
		for (auto it = cache.begin(); it != cache.end();)
		{
			bool uses = false;
			for (WGPUTextureView v : views)
				uses |= (v && (it->first.views[0] == v || it->first.views[1] == v || it->first.views[2] == v));
			if (!uses)
			{
				++it;
				continue;
			}
			for (WGPUBindGroup& pg : m_pass_tfx_groups)
			{
				if (pg == it->second)
					pg = nullptr;
			}
			if (m_encoder)
				m_pending_release_groups.push_back(it->second);
			else
				wgpuBindGroupRelease(it->second);
			it = cache.erase(it);
		}
	}
}

WGPUBindGroup GSDeviceWebGPU::MakeTFXGroup1(const GSHWDrawConfig& config, GSTextureWebGPU* tex)
{
	TFXGroupKey key{{m_null_view, m_null_view, m_null_view}, m_samplers.Get(config.sampler)};
	if (tex)
	{
		static bool warned = false;
		const WGPUTextureFormat f = tex->GetNativeFormat();
		if (config.ps.depth_fmt != 0 && IsUnfilterableCompatible(f))
			key.views[2] = tex->GetView();
		else if (config.ps.depth_fmt == 0 && IsFilterableFloat(f))
			key.views[0] = tex->GetView();
		else
			LogOnce(warned, fmt::format("WebGPU: a {} source cannot be sampled by this draw (depth_fmt {}).",
								GSTexture::GetFormatName(tex->GetFormat()), static_cast<u32>(config.ps.depth_fmt)));
		tex->SetUseSerial(m_encoder_serial);
	}
	if (GSTextureWebGPU* pal = static_cast<GSTextureWebGPU*>(config.pal))
	{
		key.views[1] = pal->GetView();
		pal->SetUseSerial(m_encoder_serial);
	}
	return GetTFXGroup(1, key);
}

// ----------------------------------------------------------------------------
// Passes and state
// ----------------------------------------------------------------------------

void GSDeviceWebGPU::BeginAttachmentPass(GSTextureWebGPU* rt, GSTextureWebGPU* ds, int stencil_clear, WGPULoadOp colour_load_override)
{
	EndRenderPass();

	WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	if (rt)
	{
		GSVector4 clear_color = GSVector4::zero();
		ca.view = rt->GetRTView();
		ca.loadOp = LoadOpFor(rt, &clear_color);
		if (colour_load_override != WGPULoadOp_Undefined)
			ca.loadOp = colour_load_override;
		ca.storeOp = WGPUStoreOp_Store;
		ca.clearValue = {clear_color.x, clear_color.y, clear_color.z, clear_color.w};
	}

	WGPURenderPassDepthStencilAttachment da = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
	if (ds)
	{
		da.view = ds->GetRTView();
		const GSTexture::State state = ds->GetState();
		const bool cleared = (state == GSTexture::State::Cleared || state == GSTexture::State::Invalidated);
		da.depthLoadOp = cleared ? WGPULoadOp_Clear : WGPULoadOp_Load;
		da.depthClearValue = (state == GSTexture::State::Cleared) ? ds->GetClearDepth() : 0.0f;
		da.depthStoreOp = WGPUStoreOp_Store;
		if (HasStencil(ds->GetNativeFormat()))
		{
			da.stencilLoadOp = (stencil_clear >= 0 || cleared) ? WGPULoadOp_Clear : WGPULoadOp_Load;
			da.stencilClearValue = (stencil_clear >= 0) ? static_cast<u32>(stencil_clear) : 0;
			da.stencilStoreOp = WGPUStoreOp_Store;
		}
	}

	WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	rp.colorAttachmentCount = rt ? 1 : 0;
	rp.colorAttachments = rt ? &ca : nullptr;
	rp.depthStencilAttachment = ds ? &da : nullptr;
	m_pass = wgpuCommandEncoderBeginRenderPass(GetCommandEncoder(), &rp);
	m_pass_rt = rt;
	m_pass_ds = ds;
	m_pass_stencil_clear = stencil_clear;
	m_pass_is_present = false;
	m_pass_size = (rt ? rt : ds)->GetSize();
	if (rt)
	{
		rt->SetUseSerial(m_encoder_serial);
		rt->SetState(GSTexture::State::Dirty);
	}
	if (ds)
	{
		ds->SetUseSerial(m_encoder_serial);
		ds->SetState(GSTexture::State::Dirty);
	}
	InvalidatePassState();
	g_perfmon.Put(GSPerfMon::RenderPasses, 1);
}

bool GSDeviceWebGPU::ApplyTFXState(WGPURenderPipeline pipeline, const GSHWDrawConfig& config, WGPUBindGroup group1,
	WGPUBindGroup group2)
{
	if (!m_pass || !pipeline)
		return false;

	if (m_pass_pipeline != pipeline)
	{
		wgpuRenderPassEncoderSetPipeline(m_pass, pipeline);
		m_pass_pipeline = pipeline;
	}
	if (m_pass_tfx_groups[0] != m_tfx_group0 || m_pass_tfx_offsets[0] != m_vs_cb_offset ||
		m_pass_tfx_offsets[1] != m_ps_cb_offset)
	{
		const u32 offsets[2] = {m_vs_cb_offset, m_ps_cb_offset};
		wgpuRenderPassEncoderSetBindGroup(m_pass, 0, m_tfx_group0, 2, offsets);
		m_pass_tfx_groups[0] = m_tfx_group0;
		m_pass_tfx_offsets[0] = m_vs_cb_offset;
		m_pass_tfx_offsets[1] = m_ps_cb_offset;
		m_pass_uniform_offset = UINT32_MAX; // the utility group 0 must be set again
	}
	if (m_pass_tfx_groups[1] != group1)
	{
		wgpuRenderPassEncoderSetBindGroup(m_pass, 1, group1, 0, nullptr);
		m_pass_tfx_groups[1] = group1;
		m_pass_texture_group = nullptr;
	}
	if (m_pass_tfx_groups[2] != group2)
	{
		wgpuRenderPassEncoderSetBindGroup(m_pass, 2, group2, 0, nullptr);
		m_pass_tfx_groups[2] = group2;
	}

	if (!m_pass_vertex_bound)
	{
		wgpuRenderPassEncoderSetVertexBuffer(m_pass, 0, m_vertex_buffer.GetBuffer(), 0, m_vertex_buffer.GetSize());
		m_pass_vertex_bound = true;
	}
	WGPUBuffer ib = config.vs.UseFixedExpandIndexBuffer() ? m_expand_ib : m_index_buffer.GetBuffer();
	if (m_pass_index_buffer != ib)
	{
		wgpuRenderPassEncoderSetIndexBuffer(m_pass, ib, WGPUIndexFormat_Uint16, 0, WGPU_WHOLE_SIZE);
		m_pass_index_buffer = ib;
	}

	// WebGPU validates the scissor against the attachment.
	const GSVector4i pass_rect(0, 0, m_pass_size.x, m_pass_size.y);
	GSVector4i scissor = config.scissor.rintersect(pass_rect);
	if (scissor.rempty())
		scissor = GSVector4i::zero();
	if (!m_pass_scissor.eq(scissor))
	{
		wgpuRenderPassEncoderSetScissorRect(m_pass, static_cast<u32>(scissor.x), static_cast<u32>(scissor.y),
			static_cast<u32>(scissor.width()), static_cast<u32>(scissor.height()));
		m_pass_scissor = scissor;
	}
	if (!m_pass_viewport.eq(pass_rect))
	{
		wgpuRenderPassEncoderSetViewport(m_pass, 0.0f, 0.0f, static_cast<float>(pass_rect.z), static_cast<float>(pass_rect.w),
			0.0f, 1.0f);
		m_pass_viewport = pass_rect;
	}

	if (config.blend.constant_enable && m_pass_blend_constant != config.blend.constant)
	{
		const double c = static_cast<double>(config.blend.constant) / 128.0;
		const WGPUColor col = {c, c, c, c};
		wgpuRenderPassEncoderSetBlendConstant(m_pass, &col);
		m_pass_blend_constant = config.blend.constant;
	}
	if (m_pass_ds && HasStencil(m_pass_ds->GetNativeFormat()) && m_pass_stencil_ref != 1)
	{
		wgpuRenderPassEncoderSetStencilReference(m_pass, 1);
		m_pass_stencil_ref = 1;
	}
	return true;
}

void GSDeviceWebGPU::DrawTFX(const GSHWDrawConfig& config, u32 offset, u32 count)
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	if (config.vs.expand == GSHWDrawConfig::VSExpand::None)
	{
		wgpuRenderPassEncoderDrawIndexed(m_pass, count, 1, m_index_start + offset, static_cast<s32>(m_vertex_start), 0);
	}
	else
	{
		// §8.3: the base vertex travels as the first instance; vertex_index is the expanded index.
		const u32 first = config.vs.UseFixedExpandIndexBuffer() ? offset : (m_index_start + offset);
		wgpuRenderPassEncoderDrawIndexed(m_pass, count, 1, first, 0, m_vertex_start);
	}
}

void GSDeviceWebGPU::FeedbackCopy(const GSHWDrawConfig& config, GSTextureWebGPU* rt, GSTextureWebGPU* rt_clone,
	GSTextureWebGPU* ds, GSTextureWebGPU* ds_clone, const GSVector4i& area)
{
	if (rt_clone && rt)
	{
		const GSVector4i r = area.rintersect(rt->GetRect());
		if (!r.rempty())
			CopyRect(rt, rt_clone, r, r.left, r.top);
	}
	if (ds_clone && ds)
	{
		// Depth copies must cover the whole texture (§2); the clone has the depth buffer's size.
		CopyRect(ds, ds_clone, ds->GetRect(), 0, 0);
	}
}

void GSDeviceWebGPU::SendHWDraw(GSHWDrawConfig& config, WGPURenderPipeline pipeline, GSTextureWebGPU* draw_rt,
	GSTextureWebGPU* draw_ds, GSTextureWebGPU* rt_clone, GSTextureWebGPU* ds_clone, bool one_barrier, bool full_barrier,
	int stencil_clear)
{
	GSTextureWebGPU* tex = static_cast<GSTextureWebGPU*>(config.tex);
	if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_RT && rt_clone)
		tex = rt_clone;
	else if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_DEPTH && ds_clone)
		tex = ds_clone;

	const WGPUBindGroup group1 = MakeTFXGroup1(config, tex);
	WGPUBindGroup group2 = m_null_group2;
	if (rt_clone || ds_clone)
	{
		group2 = GetTFXGroup(2, TFXGroupKey{{rt_clone ? rt_clone->GetView() : m_null_view, m_null_view,
									  ds_clone ? ds_clone->GetView() : m_null_view},
									 nullptr});
		if (rt_clone)
			rt_clone->SetUseSerial(m_encoder_serial);
		if (ds_clone)
			ds_clone->SetUseSerial(m_encoder_serial);
	}

	auto ensure_pass = [&]() {
		if (!m_pass || m_pass_is_present || m_pass_rt != draw_rt || m_pass_ds != draw_ds)
		{
			BeginAttachmentPass(draw_rt, draw_ds, stencil_clear);
			stencil_clear = -1; // passes resumed after a copy keep the stencil
		}
	};

	if (full_barrier && config.drawlist && !config.drawlist->empty())
	{
		const u32 draw_list_size = static_cast<u32>(config.drawlist->size());
		const u32 indices_per_prim = config.indices_per_prim;
		pxAssert(config.drawlist_bbox && static_cast<u32>(config.drawlist_bbox->size()) == draw_list_size);

		if (config.tex_hazard != GSHWDrawConfig::TEX_HAZARD_NONE)
			FeedbackCopy(config, draw_rt, rt_clone, draw_ds, ds_clone, config.samplearea);

		for (u32 n = 0, p = 0; n < draw_list_size; n++)
		{
			const u32 count = static_cast<u32>(config.drawlist->at(n)) * indices_per_prim;
			const GSVector4i bbox = config.drawlist_bbox->at(n).rintersect(config.drawarea);
			FeedbackCopy(config, draw_rt, rt_clone, draw_ds, ds_clone, bbox);
			ensure_pass();
			if (ApplyTFXState(pipeline, config, group1, group2))
				DrawTFX(config, p, count);
			p += count;
		}
		return;
	}

	if (one_barrier || full_barrier)
	{
		// Choose the smaller copy: the union of draw and sample areas, or both separately (as GSDevice11).
		if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_RT)
		{
			const GSVector4i union_rect = config.drawarea.runion(config.samplearea);
			const u32 size_union = union_rect.width() * union_rect.height();
			const u32 size_indiv = config.drawarea.width() * config.drawarea.height() +
								   config.samplearea.width() * config.samplearea.height();
			if (size_union > size_indiv)
			{
				FeedbackCopy(config, draw_rt, rt_clone, draw_ds, ds_clone, config.drawarea);
				FeedbackCopy(config, draw_rt, rt_clone, nullptr, nullptr, config.samplearea);
			}
			else
			{
				FeedbackCopy(config, draw_rt, rt_clone, draw_ds, ds_clone, union_rect);
			}
		}
		else
		{
			FeedbackCopy(config, draw_rt, rt_clone, draw_ds, ds_clone, config.drawarea);
		}
	}

	ensure_pass();
	if (ApplyTFXState(pipeline, config, group1, group2))
		DrawTFX(config, 0, config.nindices);
}

void GSDeviceWebGPU::SetupDATE(GSTextureWebGPU* rt, GSTextureWebGPU* ds, SetDATM datm, const GSVector4i& bbox)
{
	// GSDevice11::SetupDATE: stencil cleared to 0, then 1 where the RT's alpha passes (the DATM shader).
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	PrepareForSampling(rt);

	const WGPURenderPipeline pipeline =
		GetUtilityPipeline(ConvertPipelineKey(ShaderConvertSelector(SetDATMShader(datm)), ds->GetNativeFormat()));
	if (!pipeline)
		return;

	BeginAttachmentPass(nullptr, ds, 0);
	wgpuRenderPassEncoderSetStencilReference(m_pass, 1);
	m_pass_stencil_ref = 1;
	SetUtilityTexture(rt, BindGroupKind::Point);
	SetPipeline(pipeline);
	const GSVector4 src = GSVector4(bbox) / GSVector4(ds->GetSize()).xyxy();
	DrawStretchRect(src, GSVector4(bbox), ds->GetSize());
}

// ----------------------------------------------------------------------------
// RenderHW
// ----------------------------------------------------------------------------

void GSDeviceWebGPU::RenderHW(GSHWDrawConfig& config)
{
	if (m_hw_draws++ == 0)
		Console.WriteLn("WebGPU: the hardware renderer draws its first primitives.");
	GSTextureWebGPU* const cfg_rt = static_cast<GSTextureWebGPU*>(config.rt);
	GSTextureWebGPU* cfg_ds = static_cast<GSTextureWebGPU*>(config.ds);
	const GSVector2i rtsize = (cfg_rt ? cfg_rt : cfg_ds)->GetSize();
	GSTextureWebGPU* colclip_rt = static_cast<GSTextureWebGPU*>(g_gs_device->GetColorClipTexture());
	GSTextureWebGPU* draw_rt = cfg_rt;
	GSTextureWebGPU* rt_clone = nullptr;
	GSTextureWebGPU* ds_clone = nullptr;

	ScopedGuard recycle_temp_textures([&]() {
		if (rt_clone)
			Recycle(rt_clone);
		if (ds_clone)
			Recycle(ds_clone);
	});

	// WebGPU attachments must all have the same size (DX11 and VK accept a larger one).
	if (cfg_rt && cfg_ds && cfg_rt->GetSize() != cfg_ds->GetSize())
	{
		static bool warned = false;
		const bool depth_unused = (config.depth.ztst == ZTST_ALWAYS && !config.depth.zwe &&
								   config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Stencil &&
								   config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::StencilOne);
		LogOnce(warned, fmt::format("WebGPU: colour {}x{} and depth {}x{} targets differ in size; {}.", cfg_rt->GetWidth(),
							cfg_rt->GetHeight(), cfg_ds->GetWidth(), cfg_ds->GetHeight(),
							depth_unused ? "the unused depth is left out" : "the draw is skipped"));
		if (!depth_unused)
		{
			m_skipped_draws++;
			return;
		}
		cfg_ds = nullptr;
	}
	GSTextureWebGPU* draw_ds = cfg_ds;

	if (colclip_rt)
	{
		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::EarlyResolve)
		{
			const GSVector2i size = config.rt->GetSize();
			const GSVector4 dRect(config.colclip_update_area);
			const GSVector4 sRect = dRect / GSVector4(size.x, size.y).xyxy();
			StretchRect(colclip_rt, sRect, config.rt, dRect, ShaderConvert::COLCLIP_RESOLVE, Nearest);
			Recycle(colclip_rt);
			g_gs_device->SetColorClipTexture(nullptr);
			colclip_rt = nullptr;
		}
		else
		{
			config.ps.colclip_hw = 1;
		}
	}

	if (config.ps.colclip_hw)
	{
		if (!colclip_rt)
		{
			config.colclip_update_area = config.drawarea;
			colclip_rt = static_cast<GSTextureWebGPU*>(CreateFeedbackTarget(rtsize.x, rtsize.y, GSTexture::Format::ColorClip, false));
			if (!colclip_rt)
			{
				Console.Warning("WebGPU: Failed to allocate ColorClip render target, aborting draw.");
				return;
			}
			g_gs_device->SetColorClipTexture(colclip_rt);
			const GSVector4 dRect = GSVector4((config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly) ?
												  GSVector4i::loadh(rtsize) :
												  config.drawarea);
			const GSVector4 sRect = dRect / GSVector4(rtsize.x, rtsize.y).xyxy();
			StretchRect(config.rt, sRect, colclip_rt, dRect, ShaderConvert::COLCLIP_INIT, Nearest);
		}
		draw_rt = colclip_rt;
	}

	// Destination alpha (§10.3): stencil only; PrimID tracking is not offered (primitive_id = 0).
	const bool need_barrier = config.require_one_barrier || (config.require_full_barrier && m_features.multidraw_fb_copy);
	int stencil_clear = -1;
	if (draw_ds && HasStencil(draw_ds->GetNativeFormat()))
	{
		if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::Stencil ||
			(config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne && !need_barrier))
		{
			SetupDATE(draw_rt, draw_ds, config.datm, config.drawarea);
		}
		else if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne)
		{
			stencil_clear = 1;
		}
	}

	// Keep the open pass when this draw needs only its colour or only its depth (GSDevice11/VK).
	if (!draw_ds && draw_rt && m_pass && !m_pass_is_present && m_pass_rt == draw_rt && m_pass_ds &&
		m_pass_ds->GetSize() == draw_rt->GetSize() && config.tex != m_pass_ds)
	{
		draw_ds = m_pass_ds;
		config.depth = GSHWDrawConfig::DepthStencilSelector::NoDepth();
		config.alpha_second_pass.depth = GSHWDrawConfig::DepthStencilSelector::NoDepth();
	}
	else if (!draw_rt && draw_ds && m_pass && !m_pass_is_present && m_pass_ds == draw_ds && m_pass_rt &&
			 m_pass_rt->GetSize() == draw_ds->GetSize() && config.tex != m_pass_rt)
	{
		draw_rt = m_pass_rt;
		config.ps.no_color = 1;
		config.alpha_second_pass.ps.no_color = 1;
	}

	// Vertices, indices and constants for every draw below go into the rings now, so that no submit
	// (ring full) can come between them and their draws.
	const u32 vertex_bytes = config.nverts * sizeof(GSVertex);
	const u32 index_bytes = config.vs.UseFixedExpandIndexBuffer() ? 0 : Common::AlignUpPow2(config.nindices * sizeof(u16), 4);
	auto reserve = [&]() {
		return m_vertex_buffer.ReserveMemory(vertex_bytes, sizeof(GSVertex)) &&
			   (index_bytes == 0 || m_index_buffer.ReserveMemory(index_bytes, 4)) &&
			   m_uniform_buffer.ReserveMemory(VS_CB_SLOT + 2 * PS_CB_SLOT, UNIFORM_SLOT);
	};
	if (!reserve())
	{
		ExecuteCommandBuffer("stream buffers full (HW draw)");
		if (!reserve())
		{
			Console.ErrorFmt("WebGPU: a draw of {} vertices and {} indices does not fit in the stream buffers.",
				config.nverts, config.nindices);
			return;
		}
	}
	m_vertex_start = m_vertex_buffer.GetCurrentOffset() / sizeof(GSVertex);
	std::memcpy(m_vertex_buffer.GetCurrentHostPointer(), config.verts, vertex_bytes);
	m_vertex_buffer.CommitMemory(vertex_bytes);
	m_index_start = 0;
	if (index_bytes)
	{
		m_index_start = m_index_buffer.GetCurrentOffset() / sizeof(u16);
		std::memcpy(m_index_buffer.GetCurrentHostPointer(), config.indices, config.nindices * sizeof(u16));
		m_index_buffer.CommitMemory(index_bytes);
	}
	if (m_cb_serial != m_encoder_serial)
	{
		m_cb_serial = m_encoder_serial;
		m_vs_cb_offset = UINT32_MAX;
		m_ps_cb_offset = UINT32_MAX;
	}
	if (m_vs_cb_offset == UINT32_MAX || std::memcmp(&m_vs_cb_cache, &config.cb_vs, sizeof(config.cb_vs)) != 0)
	{
		m_uniform_buffer.ReserveMemory(VS_CB_SLOT, UNIFORM_SLOT);
		m_vs_cb_offset = m_uniform_buffer.GetCurrentOffset();
		std::memcpy(m_uniform_buffer.GetCurrentHostPointer(), &config.cb_vs, VS_CB_SIZE);
		m_uniform_buffer.CommitMemory(VS_CB_SLOT);
		m_vs_cb_cache = config.cb_vs;
	}
	auto upload_ps_cb = [&]() {
		if (m_ps_cb_offset != UINT32_MAX && m_ps_cb_cache == config.cb_ps)
			return;
		m_uniform_buffer.ReserveMemory(PS_CB_SLOT, UNIFORM_SLOT);
		m_ps_cb_offset = m_uniform_buffer.GetCurrentOffset();
		std::memcpy(m_uniform_buffer.GetCurrentHostPointer(), &config.cb_ps, PS_CB_SIZE);
		m_uniform_buffer.CommitMemory(PS_CB_SLOT);
		m_ps_cb_cache = config.cb_ps;
	};
	upload_ps_cb();

	const WGPURenderPipeline pipeline =
		GetTFXPipeline(MakeTFXKey(config, config.ps, config.blend, config.colormask, config.depth, draw_rt, draw_ds));
	if (!pipeline)
	{
		m_skipped_draws++;
		return;
	}

	// Feedback through copies (§10.2).
	const bool rt_feedback = config.IsFeedbackLoopRT(config.ps) ||
							 (config.alpha_second_pass.enable && config.IsFeedbackLoopRT(config.alpha_second_pass.ps));
	const bool ds_feedback = config.IsFeedbackLoopDepth(config.ps) ||
							 (config.alpha_second_pass.enable && config.IsFeedbackLoopDepth(config.alpha_second_pass.ps));
	if (draw_rt && need_barrier && rt_feedback)
	{
		rt_clone = static_cast<GSTextureWebGPU*>(CreateTexture(rtsize.x, rtsize.y, 1, draw_rt->GetFormat(), true));
		if (!rt_clone)
			Console.Warning("WebGPU: Failed to allocate temp texture for RT copy.");
	}
	if (draw_ds && need_barrier && ds_feedback)
	{
		ds_clone = static_cast<GSTextureWebGPU*>(CreateTexture(draw_ds->GetWidth(), draw_ds->GetHeight(), 1, draw_ds->GetFormat(), true));
		if (!ds_clone)
			Console.Warning("WebGPU: Failed to allocate temp texture for DS copy.");
	}

	// Sources must hold defined data, and no source may be an attachment of the pass.
	GSTexture* tex_copy = nullptr;
	ScopedGuard recycle_tex_copy([&]() {
		if (tex_copy)
			Recycle(tex_copy);
	});
	if (config.tex && config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_NONE &&
		(config.tex == draw_rt || config.tex == draw_ds))
	{
		GSTexture* src = config.tex;
		tex_copy = CreateTexture(src->GetWidth(), src->GetHeight(), 1, src->GetFormat(), true);
		if (tex_copy)
			CopyRect(src, tex_copy, src->GetRect(), 0, 0);
		config.tex = tex_copy;
	}
	if (config.tex && config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_NONE)
		PrepareForSampling(static_cast<GSTextureWebGPU*>(config.tex));
	if (config.pal)
		PrepareForSampling(static_cast<GSTextureWebGPU*>(config.pal));

	SendHWDraw(config, pipeline, draw_rt, draw_ds, rt_clone, ds_clone, config.require_one_barrier,
		config.require_full_barrier && m_features.multidraw_fb_copy, stencil_clear);

	if (config.blend_multi_pass.enable)
	{
		GSHWDrawConfig::PSSelector ps = config.ps;
		ps.no_color1 = config.blend_multi_pass.no_color1;
		ps.blend_hw = config.blend_multi_pass.blend_hw;
		ps.dither = config.blend_multi_pass.dither;
		const WGPURenderPipeline mp = GetTFXPipeline(
			MakeTFXKey(config, ps, config.blend_multi_pass.blend, config.colormask, config.depth, draw_rt, draw_ds));
		GSHWDrawConfig mp_config = config;
		mp_config.ps = ps;
		mp_config.blend = config.blend_multi_pass.blend;
		if (mp)
			SendHWDraw(mp_config, mp, draw_rt, draw_ds, rt_clone, ds_clone, false, false, -1);
	}

	if (config.alpha_second_pass.enable)
	{
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			upload_ps_cb();
		}
		const WGPURenderPipeline ap = GetTFXPipeline(MakeTFXKey(config, config.alpha_second_pass.ps, config.blend,
			config.alpha_second_pass.colormask, config.alpha_second_pass.depth, draw_rt, draw_ds));
		GSHWDrawConfig ap_config = config;
		ap_config.ps = config.alpha_second_pass.ps;
		ap_config.colormask = config.alpha_second_pass.colormask;
		ap_config.depth = config.alpha_second_pass.depth;
		if (ap)
		{
			SendHWDraw(ap_config, ap, draw_rt, draw_ds, rt_clone, ds_clone,
				config.alpha_second_pass.require_one_barrier && m_features.multidraw_fb_copy,
				config.alpha_second_pass.require_full_barrier && m_features.multidraw_fb_copy, -1);
		}
	}

	if (colclip_rt)
	{
		config.colclip_update_area = config.colclip_update_area.runion(config.drawarea);
		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly ||
			config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve)
		{
			const GSVector2i size = config.rt->GetSize();
			const GSVector4 dRect(config.colclip_update_area);
			const GSVector4 sRect = dRect / GSVector4(size.x, size.y).xyxy();
			StretchRect(colclip_rt, sRect, config.rt, dRect, ShaderConvert::COLCLIP_RESOLVE, Nearest);
			Recycle(colclip_rt);
			g_gs_device->SetColorClipTexture(nullptr);
		}
	}
}

void GSDeviceWebGPU::ClearSamplerCache()
{
	// The sampler cache is keyed by every field a sampler depends on, so nothing goes stale.
}

// ----------------------------------------------------------------------------
// Texture cache conversions (GSDeviceVK's uniform layouts)
// ----------------------------------------------------------------------------

void GSDeviceWebGPU::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex,
	u32 dOffset, u32 dSize)
{
	struct alignas(16) Uniforms
	{
		u32 offsetX, offsetY, dOffset, pad1;
		float scale;
		float pad2[3];
	};
	Uniforms uniforms = {};
	uniforms.offsetX = offsetX;
	uniforms.offsetY = offsetY;
	uniforms.dOffset = dOffset;
	uniforms.scale = sScale;
	SetUtilityUniforms(&uniforms, sizeof(uniforms));

	const GSVector4 dRect(0, 0, dSize, 1);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	DoStretchRect(sTex, GSVector4::zero(), dTex, dRect, ShaderConvertSelector(shader), Nearest);
}

void GSDeviceWebGPU::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW,
	u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	struct alignas(16) Uniforms
	{
		u32 SBW;
		u32 DBW;
		u32 PSM;
		u32 pad1[1];
		float ScaleFactor;
		float pad2[3];
	};
	Uniforms uniforms = {};
	uniforms.SBW = SBW;
	uniforms.DBW = DBW;
	uniforms.PSM = SPSM;
	uniforms.ScaleFactor = sScale;
	SetUtilityUniforms(&uniforms, sizeof(uniforms));

	const ShaderConvert shader = ((SPSM & 0xE) == 0) ? ShaderConvert::RGBA_TO_8I : ShaderConvert::RGB5A1_TO_8I;
	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	DoStretchRect(sTex, GSVector4::zero(), dTex, dRect, ShaderConvertSelector(shader), Nearest);
}

void GSDeviceWebGPU::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
	const GSVector2i& clamp_min, const GSVector4& dRect)
{
	struct alignas(16) Uniforms
	{
		GSVector2i clamp_min;
		int downsample_factor;
		int pad0;
		float weight;
		float step_multiplier;
		float pad1[2];
	};
	Uniforms uniforms = {};
	uniforms.clamp_min = clamp_min;
	uniforms.downsample_factor = downsample_factor;
	uniforms.weight = static_cast<float>(downsample_factor * downsample_factor);
	uniforms.step_multiplier = (GSConfig.UserHacks_NativeScaling > GSNativeScaling::Aggressive) ? 2.0f : 1.0f;
	SetUtilityUniforms(&uniforms, sizeof(uniforms));

	DoStretchRect(sTex, GSVector4::zero(), dTex, dRect, ShaderConvertSelector(ShaderConvert::DOWNSAMPLE_COPY), Nearest);
}
