// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Write tracking for the EE and IOP JITs (EEJit.cpp, IopJit.cpp), so that a compiled block compares its code with
// the words it was compiled from only after a write to its page.
//
// Every write to directly mapped guest memory (RAM, scratchpad, IOP RAM) zeroes the generation of its 4 KB page of
// wasm memory, indexed by host address: the stores of generated code, the interpreters' stores (vtlb.cpp,
// IopMem.cpp) and the DMA channels' writes. A block keeps the non-zero generation its page had when its code was
// last compared; on entry it compares its code again only when the page's generation differs, and then gives the
// page a new generation. Every vsync also zeroes the generations of the pages that hold code, a safety net for a
// writer that is not tracked.

#pragma once

#include "common/Pcsx2Types.h"

#include <cstddef>

namespace CodeWrites
{
	extern u32 g_gen[1u << 20]; // by host address >> 12

	inline void Note(const void* host, size_t bytes)
	{
		if (!bytes)
			return;
		const uptr p = reinterpret_cast<uptr>(host);
		for (uptr page = p >> 12; page <= (p + bytes - 1) >> 12; page++)
			g_gen[page] = 0;
	}

	// A write of at most 4 KB.
	inline void NoteSmall(const void* host, size_t bytes)
	{
		const uptr p = reinterpret_cast<uptr>(host);
		g_gen[p >> 12] = 0;
		g_gen[(p + bytes - 1) >> 12] = 0;
	}

	// The current generation of a host page, after giving it a new one if a write zeroed it.
	u32 Current(u32 page);
	// Records that a JIT compiled code from this host page (for NoteAll()).
	void AddCodePage(u32 page);
	// Zeroes the generations of all the pages that hold code (every vsync).
	void NoteAll();
	// The address of the generation counter, for generated code.
	u32* Counter();

	// Generated code (a WasmJit::Function). In a block's entry checks, which branch to depth 0 when its code changed:
	// branches to depth 1 (the code is the same) when no write hit the block's host bytes [host, host + bytes) since
	// its last compare, keeping the page's generation (never 0) in local `gen`. False if the block spans two host
	// pages: it then always compares. With record false (code generated on another thread, EEJit.cpp), the caller
	// does AddCodePage() and sets *checked on the thread that installs the code, once it found the code unchanged.
	template <typename F>
	bool EmitSkipCompare(F& f, u32 host, u32 bytes, u32* checked, u32 gen, bool record = true)
	{
		const u32 page = host >> 12;
		if (page != (host + bytes - 1) >> 12)
			return false;
		if (record)
		{
			AddCodePage(page);
			*checked = Current(page);
		}
		const u32 page_addr = static_cast<u32>(reinterpret_cast<uptr>(&g_gen[page]));
		const u32 counter = static_cast<u32>(reinterpret_cast<uptr>(Counter()));
		f.i32_const(0);
		f.i32_load(page_addr);
		f.local_tee(gen);
		f.i32_const(0);
		f.i32_load(static_cast<u32>(reinterpret_cast<uptr>(checked)));
		f.i32_eq();
		f.br_if(1);
		// A write zeroed the page's generation: give it a new one.
		f.local_get(gen);
		f.i32_eqz();
		f.if_();
		f.i32_const(0);
		f.i32_load(counter);
		f.i32_const(1);
		f.i32_add();
		f.local_tee(gen);
		f.local_get(gen);
		f.i32_eqz();
		f.i32_add();
		f.local_set(gen);
		f.i32_const(0);
		f.local_get(gen);
		f.i32_store(counter);
		f.i32_const(0);
		f.local_get(gen);
		f.i32_store(page_addr);
		f.end();
		return true;
	}

	// After the compare found the same code (EmitSkipCompare() returned true).
	template <typename F>
	void EmitCompared(F& f, u32* checked, u32 gen)
	{
		f.i32_const(0);
		f.local_get(gen);
		f.i32_store(static_cast<u32>(reinterpret_cast<uptr>(checked)));
	}

	// After a generated store of at most its alignment: zeroes the generation of the host address on the stack.
	template <typename F>
	void EmitNoteWrite(F& f)
	{
		f.i32_const(12);
		f.i32_shr_u();
		f.i32_const(2);
		f.i32_shl();
		f.i32_const(0);
		f.i32_store(static_cast<u32>(reinterpret_cast<uptr>(&g_gen[0])));
	}
} // namespace CodeWrites
