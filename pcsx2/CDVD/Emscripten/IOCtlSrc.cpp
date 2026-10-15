// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Physical disc drives for the WebAssembly build. Browsers give pages no access to
// optical drives, so there are none and opening one always fails. Disc images are
// read through the normal ISO/CHD/CSO readers instead.

#include "CDVD/CDVDdiscReader.h"
#include "CDVD/CDVD.h"

#include "common/Error.h"

std::vector<std::string> GetOpticalDriveList()
{
	return {};
}

void GetValidDrive(std::string& drive)
{
	drive.clear();
}

IOCtlSrc::IOCtlSrc(std::string filename)
	: m_filename(std::move(filename))
{
}

IOCtlSrc::~IOCtlSrc() = default;

bool IOCtlSrc::Reopen(Error* error)
{
	Error::SetString(error, "Physical disc drives are not available in the browser.");
	return false;
}

void IOCtlSrc::SetSpindleSpeed(bool restore_defaults) const
{
}

u32 IOCtlSrc::GetSectorCount() const
{
	return m_sectors;
}

u32 IOCtlSrc::GetLayerBreakAddress() const
{
	return m_layer_break;
}

s32 IOCtlSrc::GetMediaType() const
{
	return m_media_type;
}

const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const
{
	return m_toc;
}

bool IOCtlSrc::ReadSectors2048(u32 sector, u32 count, u8* buffer) const
{
	return false;
}

bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
	return false;
}

bool IOCtlSrc::ReadDVDInfo()
{
	return false;
}

bool IOCtlSrc::ReadCDInfo()
{
	return false;
}

bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subq) const
{
	return false;
}

bool IOCtlSrc::DiscReady()
{
	return false;
}
