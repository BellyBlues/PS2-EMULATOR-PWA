// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Guest registers in WebAssembly locals, for the EE and IOP JITs (EEJit.cpp, IopJit.cpp).
//
// A block keeps the guest registers it uses in locals of its function: each is loaded from the CPU's register file
// on first use, and written back when it changed (dirty) before anything else can read or write the register file:
// every call into C++ (which may also longjmp out of the block: TLB misses, the event test) and every exit. Which
// registers are loaded and dirty is known at compile time. Where two paths join (a memory access's fast path and
// its slow path through the interpreter), Reconcile() at the end of the second path brings it to the state of the
// first: it writes back what the first has clean and loads what the first has loaded.

#pragma once

#include "common/Pcsx2Types.h"
#include "wasm/WasmEmitter.h"

#include <cstdio>
#include <cstdlib>

namespace WasmJit
{
	// A generator invariant, checked when guest code is compiled (in every build).
#define WASMJIT_CHECK(c) \
	do \
	{ \
		if (!(c)) [[unlikely]] \
		{ \
			std::fprintf(stderr, "JIT: check failed: %s (%s:%d)\n", #c, __FILE__, __LINE__); \
			std::abort(); \
		} \
	} while (0)

	// Up to 64 registers of one type (I32 or I64) at the wasm addresses addr(r).
	class RegCache
	{
	public:
		struct State
		{
			u64 loaded = 0; // the local holds the register's value
			u64 dirty = 0; // ... which the register file does not have yet
			bool operator==(const State& o) const { return loaded == o.loaded && dirty == o.dirty; }
		};

		using AddrFn = u32 (*)(u32);

		RegCache(Function& f, ValType type, u32 count, AddrFn addr)
			: m_f(f)
			, m_type(type)
			, m_count(count)
			, m_addr(addr)
		{
		}

		State& state() { return m_s; }
		bool IsDirty(u32 r) const { return m_s.dirty & Bit(r); }

		// Pushes register r.
		void Get(u32 r)
		{
			if (!(m_s.loaded & Bit(r)))
			{
				EmitLoad(r);
				m_f.local_tee(Local(r));
				m_s.loaded |= Bit(r);
				return;
			}
			m_f.local_get(Local(r));
		}
		// Loads r into its local if it is not there (before control flow whose paths must agree).
		void Ensure(u32 r)
		{
			if (m_s.loaded & Bit(r))
				return;
			EmitLoad(r);
			m_f.local_set(Local(r));
			m_s.loaded |= Bit(r);
		}
		// Pops r's new value.
		void Set(u32 r)
		{
			m_f.local_set(Local(r));
			m_s.loaded |= Bit(r);
			m_s.dirty |= Bit(r);
		}
		void Writeback(u32 r)
		{
			if (!(m_s.dirty & Bit(r)))
				return;
			m_f.i32_const(0);
			m_f.local_get(Local(r));
			if (m_type == ValType::I64)
				m_f.i64_store(m_addr(r));
			else
				m_f.i32_store(m_addr(r));
			m_s.dirty &= ~Bit(r);
		}
		void WritebackAll()
		{
			for (u32 r = 0; r < m_count; r++)
				Writeback(r);
		}
		// The register file has the value of every register in `mask` (written there by C++, or by a wider store):
		// their locals are stale. Only for registers that are not dirty, or whose dirty value was overwritten.
		void Invalidate(u64 mask)
		{
			m_s.loaded &= ~mask;
			m_s.dirty &= ~mask;
		}
		// At the end of a path joining another whose state is `target`.
		void Reconcile(const State& target)
		{
			for (u32 r = 0; r < m_count; r++)
			{
				const u64 b = Bit(r);
				if ((m_s.dirty & b) && (!(target.dirty & b) || !(target.loaded & b)))
					Writeback(r);
				if ((target.loaded & b) && !(m_s.loaded & b))
					Ensure(r);
			}
			m_s = target;
		}

		static u64 Bit(u32 r) { return 1ull << r; }

	private:
		u32 Local(u32 r)
		{
			if (!(m_has_local & Bit(r)))
			{
				m_local[r] = m_f.local(m_type);
				m_has_local |= Bit(r);
			}
			return m_local[r];
		}
		void EmitLoad(u32 r)
		{
			m_f.i32_const(0);
			if (m_type == ValType::I64)
				m_f.i64_load(m_addr(r));
			else
				m_f.i32_load(m_addr(r));
		}

		Function& m_f;
		ValType m_type;
		u32 m_count;
		AddrFn m_addr;
		State m_s;
		u64 m_has_local = 0;
		u32 m_local[64] = {};
	};
} // namespace WasmJit
