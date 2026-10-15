// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// No HTTP downloader in the WebAssembly build yet. Callers (achievements, cover
// downloads) already handle a null downloader. A fetch()-based one could go here.

#include "common/HTTPDownloader.h"

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string user_agent)
{
	return {};
}
