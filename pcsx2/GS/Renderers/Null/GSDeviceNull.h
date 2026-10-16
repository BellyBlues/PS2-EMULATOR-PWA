// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"

#include <functional>
#include <vector>

// A GPU-less device for the WebAssembly bring-up (no GPU backend yet). Textures live
// in ordinary memory, so uploads, maps, copies and readbacks work. Stretch, merge,
// interlace and present are CPU nearest-neighbour versions of the GPU passes, enough
// to show the software renderer's output; presented frames go to a host callback.
// Hardware draws, post-processing and TV shaders are no-ops.

class GSTextureNull final : public GSTexture
{
public:
	GSTextureNull(Usage usage, Format format, int width, int height, int levels);
	~GSTextureNull() override;

	void* GetNativeHandle() const override;

	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) override;
	bool Map(GSMap& m, const GSVector4i* r = nullptr, int layer = 0) override;
	void Unmap() override;
	void GenerateMipmap() override;

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override;
#endif

	__fi u8* GetData() { return m_data.data(); }
	__fi u32 GetPitch() const { return m_pitch; }

	/// Byte offset of pixel (x, y), or of the block containing it for compressed formats.
	u32 GetOffset(int x, int y) const;

private:
	std::vector<u8> m_data;
	u32 m_pitch;
};

class GSDownloadTextureNull final : public GSDownloadTexture
{
public:
	GSDownloadTextureNull(u32 width, u32 height, GSTexture::Format format);
	~GSDownloadTextureNull() override;

	void CopyFromTexture(const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level,
		bool use_transfer_pitch = true) override;

	bool Map(const GSVector4i& read_rc) override;
	void Unmap() override;
	void Flush() override;

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override;
#endif

private:
	std::vector<u8> m_buffer;
};

class GSDeviceNull final : public GSDevice
{
public:
	/// Receives each presented frame on the GS thread: tightly packed RGBA8 rows,
	/// alpha forced to 255, valid only for the duration of the call.
	using PresentCallback = std::function<void(const u32* pixels, u32 width, u32 height)>;

	/// Set before the GS opens. Without a callback nothing is presented (headless).
	static void SetPresentCallback(PresentCallback callback);

	GSDeviceNull();
	~GSDeviceNull() override;

	bool Create(GSVSyncMode vsync_mode, bool allow_present_throttle) override;
	void Destroy() override;

	RenderAPI GetRenderAPI() const override;
	bool HasSurface() const override;
	void DestroySurface() override;
	bool UpdateWindow() override;
	void ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale) override;
	bool SupportsExclusiveFullscreen() const override;

	PresentResult BeginPresent(bool frame_skip) override;
	void EndPresent() override;
	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle) override;

	std::string GetDriverInfo() const override;

	bool SetGPUTimingEnabled(bool enabled) override;
	float GetAndResetAccumulatedGPUTime() override;
	bool SetGPUPipelineStatisticsEnabled(bool enabled) override;
	GPUPipelineStatistics GetAndResetAccumulatedGPUPipelineStatistics() override;

	void PushDebugGroup(const char* fmt, ...) override;
	void PopDebugGroup() override;
	void InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...) override;

	std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) override;

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) override;

	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		PresentShader shader, float shaderTime, Filter filter) override;

	void UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset,
		u32 dSize) override;
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM,
		GSTexture* dTex, u32 DBW, u32 DPSM) override;
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
		const GSVector2i& clamp_min, const GSVector4& dRect) override;

	void RenderHW(GSHWDrawConfig& config) override;
	void ClearSamplerCache() override;

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE,
		const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter) override;
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb) override;
	void DoFXAA(GSTexture* sTex, GSTexture* dTex) override;
	void DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4]) override;
	bool DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only,
		const std::array<u32, NUM_CAS_CONSTANTS>& constants) override;

protected:
	GSTexture* CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format) override;

private:
	std::vector<u32> m_present_buffer;
	bool m_presenting = false;

	void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvertSelector shader, Filter filter) override;
};
