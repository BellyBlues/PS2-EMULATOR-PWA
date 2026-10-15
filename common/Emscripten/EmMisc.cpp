// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Platform glue for the WebAssembly (Emscripten) build. Browsers expose no
// screensaver, global mouse or sound-playing APIs to wasm, so those are no-ops.

#include "common/Pcsx2Types.h"
#include "common/HostSys.h"
#include "common/Threading.h"

#include <emscripten/heap.h>

#include <ctime>
#include <string>
#include <unistd.h>

// Wasm has no "physical memory"; report the heap ceiling the module was built with.
u64 GetPhysicalMemory()
{
	return static_cast<u64>(emscripten_get_heap_max());
}

u64 GetAvailablePhysicalMemory()
{
	return static_cast<u64>(emscripten_get_heap_max() - emscripten_get_heap_size());
}

u64 GetTickFrequency()
{
	return 1000000000; // nanoseconds, matching CLOCK_MONOTONIC
}

u64 GetCPUTicks()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (static_cast<u64>(ts.tv_sec) * 1000000000ULL) + ts.tv_nsec;
}

std::string GetOSVersionString()
{
	return "Web (Emscripten)";
}

bool Common::InhibitScreensaver(bool inhibit)
{
	// TODO: the Screen Wake Lock API could do this from the main thread.
	return false;
}

void Common::SetMousePosition(int x, int y)
{
}

bool Common::AttachMousePositionCb(std::function<void(int, int)> cb)
{
	return false;
}

void Common::DetachMousePositionCb()
{
}

bool Common::PlaySoundAsync(const char* path)
{
	return false;
}

void Threading::Sleep(int ms)
{
	usleep(1000 * ms);
}

void Threading::SleepUntil(u64 ticks)
{
	const u64 now = GetCPUTicks();
	if (ticks <= now)
		return;

	const u64 delta = ticks - now;
	struct timespec ts;
	ts.tv_sec = static_cast<time_t>(delta / 1000000000ULL);
	ts.tv_nsec = static_cast<long>(delta % 1000000000ULL);
	nanosleep(&ts, nullptr);
}
