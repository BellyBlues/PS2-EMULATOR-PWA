// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// GSCapture for builds without FFmpeg (the WebAssembly build). Capturing never starts.

#include "GS/GSCapture.h"

#include "common/Console.h"
#include "common/Threading.h"

bool GSCapture::BeginCapture(float fps, GSVector2i recommendedResolution, float aspect, std::string filename)
{
	Console.Error("Video capture is not available in this build.");
	return false;
}

bool GSCapture::DeliverVideoFrame(GSTexture* stex)
{
	return false;
}

void GSCapture::DeliverAudioPacket(const float* frames)
{
}

void GSCapture::EndCapture()
{
}

bool GSCapture::IsCapturing()
{
	return false;
}

bool GSCapture::IsCapturingVideo()
{
	return false;
}

bool GSCapture::IsCapturingAudio()
{
	return false;
}

TinyString GSCapture::GetElapsedTime()
{
	return {};
}

const Threading::ThreadHandle& GSCapture::GetEncoderThreadHandle()
{
	static Threading::ThreadHandle s_null_handle;
	return s_null_handle;
}

GSVector2i GSCapture::GetSize()
{
	return GSVector2i(0, 0);
}

std::string GSCapture::GetNextCaptureFileName()
{
	return {};
}

void GSCapture::Flush()
{
}

void GSCapture::FlushAudioOnly()
{
}

GSCapture::CodecList GSCapture::GetVideoCodecList(const char* container)
{
	return {};
}

GSCapture::CodecList GSCapture::GetAudioCodecList(const char* container)
{
	return {};
}

GSCapture::FormatList GSCapture::GetVideoFormatList(const char* codec)
{
	return {};
}
