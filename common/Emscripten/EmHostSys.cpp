// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// HostSys for the WebAssembly (Emscripten) build.
//
// Wasm has one linear memory with no page protection, no address-space
// reservation and no way to map the same memory at two addresses. So:
//  - "shared memory" is a plain zeroed heap block, and the handle points to it;
//  - SharedMemoryMappingArea reserves nothing, and Map() returns where the
//    memory actually lives, ignoring map_base. Callers must use the returned
//    pointer. Aliased mappings (fastmem mirrors) cannot work, so fastmem must
//    stay off on this platform;
//  - MemProtect is a no-op and no page fault handler is installed: a wasm
//    out-of-bounds access traps, it never faults into a handler.
// Needs proper testing once the core runs.

#include "common/AlignedMalloc.h"
#include "common/Assertions.h"
#include "common/BitUtils.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/HostSys.h"

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace
{
	struct WasmSharedMemory
	{
		u8* data;
		size_t size;
	};

	// Anonymous mappings made through SharedMemoryMappingArea::Map(), keyed by returned pointer.
	std::mutex s_anon_mutex;
	std::unordered_map<void*, size_t> s_anon_mappings;
} // namespace

void HostSys::MemProtect(void* baseaddr, size_t size, const PageProtectionMode& mode)
{
	pxAssertMsg((size & (__pagesize - 1)) == 0, "Size is page aligned");
}

std::string HostSys::GetFileMappingName(const char* prefix)
{
	return prefix;
}

void* HostSys::CreateSharedMemory(const char* name, size_t size)
{
	u8* data = static_cast<u8*>(_aligned_malloc(size, __pagesize));
	if (!data)
	{
		Console.Error("CreateSharedMemory: failed to allocate %zu bytes", size);
		return nullptr;
	}

	std::memset(data, 0, size);
	return new WasmSharedMemory{data, size};
}

void HostSys::DestroySharedMemory(void* ptr)
{
	WasmSharedMemory* shm = static_cast<WasmSharedMemory*>(ptr);
	_aligned_free(shm->data);
	delete shm;
}

size_t HostSys::GetRuntimePageSize()
{
	// Wasm pages are 64KB, but nothing here is page-protected, so the compile-time
	// granularity the emulator was built with is the one that matters.
	return __pagesize;
}

size_t HostSys::GetRuntimeCacheLineSize()
{
	return __cachelinesize;
}

void HostSys::FlushInstructionCache(void* address, u32 size)
{
}

SharedMemoryMappingArea::SharedMemoryMappingArea(u8* base_ptr, size_t size, size_t num_pages)
	: m_base_ptr(base_ptr)
	, m_size(size)
	, m_num_pages(num_pages)
{
}

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
	pxAssertRel(m_num_mappings == 0, "No mappings left");
}

std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size, bool jit)
{
	pxAssertRel(Common::IsAlignedPow2(size, __pagesize), "Size is page aligned");

	// Nothing to reserve. The base pointer is only used for offset arithmetic by callers.
	return std::unique_ptr<SharedMemoryMappingArea>(new SharedMemoryMappingArea(nullptr, size, size / __pagesize));
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode)
{
	u8* ptr;
	if (file_handle)
	{
		WasmSharedMemory* shm = static_cast<WasmSharedMemory*>(file_handle);
		pxAssertRel((file_offset + map_size) <= shm->size, "Mapping is within shared memory");
		ptr = shm->data + file_offset;
	}
	else
	{
		ptr = static_cast<u8*>(_aligned_malloc(map_size, __pagesize));
		if (!ptr)
			return nullptr;

		std::memset(ptr, 0, map_size);
		std::unique_lock lock(s_anon_mutex);
		s_anon_mappings.emplace(ptr, map_size);
	}

	m_num_mappings++;
	return ptr;
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size, bool is_file)
{
	if (!is_file)
	{
		std::unique_lock lock(s_anon_mutex);
		const auto it = s_anon_mappings.find(map_base);
		if (it == s_anon_mappings.end())
			return false;

		_aligned_free(map_base);
		s_anon_mappings.erase(it);
	}

	m_num_mappings--;
	return true;
}

bool PageFaultHandler::Install(Error* error)
{
	return true;
}

bool PageFaultHandler::InstallSecondaryThread()
{
	return true;
}
