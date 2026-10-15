// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Null/GSDeviceNull.h"

#include "common/Assertions.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <cstring>

// ----------------------------------------------------------------------------
// CPU helpers
// ----------------------------------------------------------------------------

namespace
{
	// ClearRenderTarget() only records the clear; GPU backends apply it at the next
	// render pass. Here it is applied to the memory before the texture is read or written.
	void CommitClearNull(GSTextureNull* t)
	{
		if (t->GetState() == GSTexture::State::Cleared)
		{
			if (t->GetFormat() == GSTexture::Format::Color)
			{
				const u32 color = t->GetClearColor();
				for (int y = 0; y < t->GetHeight(); y++)
				{
					u32* row = reinterpret_cast<u32*>(t->GetData() + t->GetOffset(0, y));
					std::fill(row, row + t->GetWidth(), color);
				}
			}
			else
			{
				std::memset(t->GetData(), 0, static_cast<size_t>(t->GetPitch()) * t->GetHeight());
			}
		}

		t->SetState(GSTexture::State::Dirty);
	}

	// Nearest-neighbour copy of a normalised source rectangle into a pixel destination
	// rectangle. RGBA8 only, no filtering, blending or shader effects: enough to see what
	// the GS displays (screenshots), not a renderer.
	void BlitColor(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect)
	{
		GSTextureNull* const src = static_cast<GSTextureNull*>(sTex);
		GSTextureNull* const dst = static_cast<GSTextureNull*>(dTex);
		if (!src || !dst || src->GetFormat() != GSTexture::Format::Color || dst->GetFormat() != GSTexture::Format::Color)
			return;

		CommitClearNull(src);
		CommitClearNull(dst);

		const float dw = dRect.z - dRect.x;
		const float dh = dRect.w - dRect.y;
		const int dx0 = std::max(0, static_cast<int>(dRect.x));
		const int dy0 = std::max(0, static_cast<int>(dRect.y));
		const int dx1 = std::min(dst->GetWidth(), static_cast<int>(dRect.z));
		const int dy1 = std::min(dst->GetHeight(), static_cast<int>(dRect.w));
		if (dw <= 0.0f || dh <= 0.0f || dx1 <= dx0 || dy1 <= dy0)
			return;

		const float sw = static_cast<float>(src->GetWidth());
		const float sh = static_cast<float>(src->GetHeight());
		const float u0 = sRect.x * sw;
		const float v0 = sRect.y * sh;
		const float du = (sRect.z - sRect.x) * sw / dw;
		const float dv = (sRect.w - sRect.y) * sh / dh;

		for (int y = dy0; y < dy1; y++)
		{
			const int sy = std::clamp(static_cast<int>(v0 + (static_cast<float>(y) - dRect.y + 0.5f) * dv), 0, src->GetHeight() - 1);
			const u32* srow = reinterpret_cast<const u32*>(src->GetData() + src->GetOffset(0, sy));
			u32* drow = reinterpret_cast<u32*>(dst->GetData() + dst->GetOffset(0, y));
			for (int x = dx0; x < dx1; x++)
			{
				const int sx = std::clamp(static_cast<int>(u0 + (static_cast<float>(x) - dRect.x + 0.5f) * du), 0, src->GetWidth() - 1);
				drow[x] = srow[sx];
			}
		}
	}
} // namespace

// ----------------------------------------------------------------------------
// GSTextureNull
// ----------------------------------------------------------------------------

GSTextureNull::GSTextureNull(Usage usage, Format format, int width, int height, int levels)
{
	m_usage = usage;
	m_format = format;
	m_size = GSVector2i(width, height);
	m_mipmap_levels = levels;

	// Only the top level is stored; nothing samples the mips without a GPU.
	m_pitch = CalcUploadPitch(static_cast<u32>(width));
	const u32 block_size = GetCompressedBlockSize();
	const u32 rows = (static_cast<u32>(height) + (block_size - 1)) / block_size;
	m_data.resize(static_cast<size_t>(m_pitch) * rows);
}

GSTextureNull::~GSTextureNull() = default;

void* GSTextureNull::GetNativeHandle() const
{
	return const_cast<u8*>(m_data.data());
}

u32 GSTextureNull::GetOffset(int x, int y) const
{
	const u32 block_size = GetCompressedBlockSize();
	return (static_cast<u32>(y) / block_size) * m_pitch + (static_cast<u32>(x) / block_size) * GetCompressedBytesPerBlock();
}

bool GSTextureNull::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	if (layer != 0)
		return true;

	const GSVector4i rc = r.rintersect(GetRect());
	if (rc.rempty())
		return true;

	if (m_state != State::Dirty)
		CommitClearNull(this);

	const u32 block_size = GetCompressedBlockSize();
	const u32 row_bytes = CalcUploadPitch(static_cast<u32>(rc.width()));
	const u32 rows = (static_cast<u32>(rc.height()) + (block_size - 1)) / block_size;
	const u8* src = static_cast<const u8*>(data);
	// The source pointer is for r's origin; skip to the clipped origin.
	src += ((rc.y - r.y) / static_cast<int>(block_size)) * pitch +
		   ((rc.x - r.x) / static_cast<int>(block_size)) * static_cast<int>(GetCompressedBytesPerBlock());

	StringUtil::StrideMemCpy(m_data.data() + GetOffset(rc.x, rc.y), m_pitch, src, static_cast<u32>(pitch), row_bytes, rows);
	m_needs_mipmaps_generated = true;
	return true;
}

bool GSTextureNull::Map(GSMap& m, const GSVector4i* r, int layer)
{
	if (layer != 0)
		return false;

	const GSVector4i rc = r ? *r : GetRect();
	m.bits = m_data.data() + GetOffset(rc.x, rc.y);
	m.pitch = static_cast<int>(m_pitch);
	return true;
}

void GSTextureNull::Unmap()
{
	m_needs_mipmaps_generated = true;
}

void GSTextureNull::GenerateMipmap()
{
}

#ifdef PCSX2_DEVBUILD
void GSTextureNull::SetDebugName(std::string_view name)
{
	m_debug_name = name;
}
#endif

// ----------------------------------------------------------------------------
// GSDownloadTextureNull
// ----------------------------------------------------------------------------

GSDownloadTextureNull::GSDownloadTextureNull(u32 width, u32 height, GSTexture::Format format)
	: GSDownloadTexture(width, height, format)
{
	m_current_pitch = GetTransferPitch(width, 1);
	m_buffer.resize(GetBufferSize(width, height, format, 1));
}

GSDownloadTextureNull::~GSDownloadTextureNull() = default;

void GSDownloadTextureNull::CopyFromTexture(
	const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	GSTextureNull* const tex = static_cast<GSTextureNull*>(stex);
	pxAssert(tex->GetFormat() == m_format);
	pxAssert(drc.width() == src.width() && drc.height() == src.height());

	// Keep the full-width pitch, so any rectangle reads back at its own offset.
	u32 copy_offset, copy_size, copy_rows;
	GetTransferSize(drc, &copy_offset, &copy_size, &copy_rows);
	if (src_level == 0)
	{
		StringUtil::StrideMemCpy(m_buffer.data() + copy_offset, m_current_pitch, tex->GetData() + tex->GetOffset(src.x, src.y),
			tex->GetPitch(), copy_size, copy_rows);
	}

	m_needs_flush = false;
}

bool GSDownloadTextureNull::Map(const GSVector4i& read_rc)
{
	m_map_pointer = m_buffer.data();
	return true;
}

void GSDownloadTextureNull::Unmap()
{
	m_map_pointer = nullptr;
}

void GSDownloadTextureNull::Flush()
{
	m_needs_flush = false;
}

#ifdef PCSX2_DEVBUILD
void GSDownloadTextureNull::SetDebugName(std::string_view name)
{
}
#endif

// ----------------------------------------------------------------------------
// GSDeviceNull
// ----------------------------------------------------------------------------

GSDeviceNull::GSDeviceNull() = default;

GSDeviceNull::~GSDeviceNull() = default;

bool GSDeviceNull::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	if (!GSDevice::Create(vsync_mode, allow_present_throttle))
		return false;

	// No GPU features at all: the HW renderer is never used with this device.
	m_features = FeatureSupport();
	m_max_texture_size = 8192;

	// A fixed virtual "window" so the OSD layout has a size to work with.
	m_window_info.type = WindowInfo::Type::Surfaceless;
	m_window_info.surface_width = 640;
	m_window_info.surface_height = 480;
	m_window_info.surface_scale = 1.0f;
	return true;
}

void GSDeviceNull::Destroy()
{
	GSDevice::Destroy();
}

RenderAPI GSDeviceNull::GetRenderAPI() const
{
	return RenderAPI::None;
}

bool GSDeviceNull::HasSurface() const
{
	return false;
}

void GSDeviceNull::DestroySurface()
{
}

bool GSDeviceNull::UpdateWindow()
{
	return true;
}

void GSDeviceNull::ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale)
{
}

bool GSDeviceNull::SupportsExclusiveFullscreen() const
{
	return false;
}

GSDevice::PresentResult GSDeviceNull::BeginPresent(bool frame_skip)
{
	// Nothing to present to.
	return PresentResult::FrameSkipped;
}

void GSDeviceNull::EndPresent()
{
}

void GSDeviceNull::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_vsync_mode = mode;
	m_allow_present_throttle = allow_present_throttle;
}

std::string GSDeviceNull::GetDriverInfo() const
{
	return "Null device (no GPU, textures in memory)";
}

bool GSDeviceNull::SetGPUTimingEnabled(bool enabled)
{
	return false;
}

float GSDeviceNull::GetAndResetAccumulatedGPUTime()
{
	return 0.0f;
}

bool GSDeviceNull::SetGPUPipelineStatisticsEnabled(bool enabled)
{
	return false;
}

GPUPipelineStatistics GSDeviceNull::GetAndResetAccumulatedGPUPipelineStatistics()
{
	return {};
}

void GSDeviceNull::PushDebugGroup(const char* fmt, ...)
{
}

void GSDeviceNull::PopDebugGroup()
{
}

void GSDeviceNull::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
}

GSTexture* GSDeviceNull::CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format)
{
	return new GSTextureNull(usage, format, width, height, levels);
}

std::unique_ptr<GSDownloadTexture> GSDeviceNull::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return std::make_unique<GSDownloadTextureNull>(width, height, format);
}

void GSDeviceNull::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	GSTextureNull* const src = static_cast<GSTextureNull*>(sTex);
	GSTextureNull* const dst = static_cast<GSTextureNull*>(dTex);
	if (src->GetFormat() != dst->GetFormat())
		return;

	// Clip against both textures.
	const GSVector4i src_rc = r.rintersect(src->GetRect());
	const GSVector4i dst_rc = GSVector4i(destX, destY, destX + src_rc.width(), destY + src_rc.height()).rintersect(dst->GetRect());
	if (src_rc.rempty() || dst_rc.rempty())
		return;

	CommitClearNull(src);
	CommitClearNull(dst);

	const u32 block_size = src->GetCompressedBlockSize();
	const u32 row_bytes = src->CalcUploadPitch(static_cast<u32>(dst_rc.width()));
	const u32 rows = (static_cast<u32>(dst_rc.height()) + (block_size - 1)) / block_size;
	StringUtil::StrideMemCpy(dst->GetData() + dst->GetOffset(dst_rc.x, dst_rc.y), dst->GetPitch(),
		src->GetData() + src->GetOffset(src_rc.x, src_rc.y), src->GetPitch(), row_bytes, rows);
}

void GSDeviceNull::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, Filter filter)
{
}

void GSDeviceNull::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvertSelector shader, Filter filter)
{
	BlitColor(sTex, sRect, dTex, dRect);
}

void GSDeviceNull::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex,
	u32 dOffset, u32 dSize)
{
}

void GSDeviceNull::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW,
	u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
}

void GSDeviceNull::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
	const GSVector2i& clamp_min, const GSVector4& dRect)
{
}

void GSDeviceNull::RenderHW(GSHWDrawConfig& config)
{
}

void GSDeviceNull::ClearSamplerCache()
{
}

void GSDeviceNull::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter)
{
	GSTextureNull* const dst = static_cast<GSTextureNull*>(dTex);
	if (!dst || dst->GetFormat() != GSTexture::Format::Color)
		return;

	// Background colour, then circuit 2 under circuit 1. PMODE alpha blending between the
	// circuits and the EXTBUF write-back are not done; needs proper testing.
	dst->SetClearColor(c);
	CommitClearNull(dst);
	if (sTex[1])
		BlitColor(sTex[1], sRect[1], dTex, dRect[1]);
	if (sTex[0])
		BlitColor(sTex[0], sRect[0], dTex, dRect[0]);
}

void GSDeviceNull::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb)
{
	// Straight copy: no field weaving or blending.
	BlitColor(sTex, sRect, dTex, dRect);
}

void GSDeviceNull::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
}

void GSDeviceNull::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
}

bool GSDeviceNull::DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only,
	const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	return false;
}
