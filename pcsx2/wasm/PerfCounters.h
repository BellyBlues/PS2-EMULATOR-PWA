// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Running totals of time spent compiling, for the page's per-vsync recorder (pcsx2web_perf_ring() in
// pcsx2-wasm/Main.cpp): a vsync during which one grows was slowed by compiling, the usual cause of a
// short drop in speed. Each total is added to by one thread and read by the CPU thread at vsync.

#pragma once

#include "common/Pcsx2Types.h"

#include <atomic>

namespace PerfCounters
{
	// The EE and IOP JITs (CPU thread) and the VU1 JIT (VU thread): building and compiling modules.
	inline std::atomic<u64> jit_compile_ns{0};
	// The EE JIT's code generation thread: building modules (EEJit::g_background), off the EE thread.
	inline std::atomic<u64> ee_build_ns{0};
	// EE JIT modules installed (EE thread), and those of them built or compiled ahead from a code list.
	inline std::atomic<u64> ee_modules{0};
	inline std::atomic<u64> ee_ahead_modules{0};
	// The WebGPU device (GS thread): render pipelines it waited for.
	inline std::atomic<u64> gpu_pipeline_ns{0};
	inline std::atomic<u32> gpu_pipelines{0};

	inline void Add(std::atomic<u64>& total, u64 ns)
	{
		total.fetch_add(ns, std::memory_order_relaxed);
	}
} // namespace PerfCounters
