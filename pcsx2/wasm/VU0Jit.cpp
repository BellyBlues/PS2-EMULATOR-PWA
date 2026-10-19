// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "VU0Jit.h"

#include "VU1JitAnalysis.h"
#include "VU1JitEmit.h"
#include "VU1JitLockstep.h"
#include "WasmJitRuntime.h"

#include "common/FPControl.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <vector>

WasmVU0 CpuWasmVU0;

namespace VU0Jit
{
	using namespace VU1Jit;

	static bool s_lockstep = false;

	void SetLockstep(bool enabled)
	{
		s_lockstep = enabled;
	}

	namespace
	{
		struct Program;
		struct ExitSlot
		{
			BlockExit x;
			Program* next = nullptr;
			u32 next_epoch = 0;
		};

		struct Program
		{
			u32 pc;
			Signature sig;
			u32 size;
			std::vector<u8> bytes;
			bool needs_compare;
			WasmJit::LoadedFunction fn;
			std::vector<ExitSlot> exits;
			u32 last_start;
		};

		static constexpr u32 NUM_PCS = VU0_PROGSIZE / 8;
		static constexpr u32 MAX_PROGRAMS = 1000;
		static constexpr u32 MAX_VERSIONS_PER_PC = 16;

		// Everything runs on the EE thread: VU0 micro mode, Clear and Reset.
		class State
		{
		public:
			~State() { Flush(); }

			void Execute(u32 cycles);
			void ExecuteLockstep(u32 cycles);
			void Flush();
			void Dirty() { m_dirty = true; }
			void LogSummary() const;

		private:
			void Dispatch(u32 cycles);
			Program* Find(u32 pc, const Signature& sig);
			Program* Compile(u32 pc, const Signature& sig);
			void Drop(Program* p);

			std::array<std::vector<Program*>, NUM_PCS> m_by_pc;
			std::vector<std::unique_ptr<Program>> m_programs;
			u32 m_epoch = 1;
			bool m_dirty = true;
			bool m_usable = true;
			uptr m_key_micro = 0;
			bool m_key_overflow = false;
			bool m_key_hack = false;

			std::unique_ptr<RunState> m_ls_start, m_ls_jit, m_ls_ref;
			u64 m_ls_runs = 0, m_ls_diffs = 0;

			u64 m_executes = 0, m_compiles = 0, m_entries = 0, m_jit_instrs = 0, m_steps = 0, m_flushes = 0;
			double m_compile_ms = 0;
		};

		State& GetState()
		{
			static thread_local State s; // generated code lives in the EE thread's table
			return s;
		}
	} // namespace

	void State::Flush()
	{
		for (auto& p : m_programs)
			WasmJit::Unload(&p->fn);
		m_programs.clear();
		for (auto& l : m_by_pc)
			l.clear();
		m_epoch++;
		m_flushes++;
	}

	void State::Drop(Program* p)
	{
		auto& list = m_by_pc[p->pc >> 3];
		list.erase(std::find(list.begin(), list.end(), p));
		WasmJit::Unload(&p->fn);
		m_programs.erase(std::find_if(m_programs.begin(), m_programs.end(), [p](const auto& q) { return q.get() == p; }));
		m_epoch++;
	}

	Program* State::Find(u32 pc, const Signature& sig)
	{
		auto& list = m_by_pc[pc >> 3];
		for (size_t i = 0; i < list.size(); i++)
		{
			Program* p = list[i];
			if (p->sig != sig)
				continue;
			if (p->needs_compare)
			{
				if (std::memcmp(p->bytes.data(), &VU0.Micro[pc], p->size) != 0)
					continue;
				p->needs_compare = false;
			}
			if (i != 0)
				std::rotate(list.begin(), list.begin() + i, list.begin() + i + 1);
			return p;
		}
		if (list.size() >= MAX_VERSIONS_PER_PC)
			Drop(list.back());
		return Compile(pc, sig);
	}

	Program* State::Compile(u32 pc, const Signature& sig)
	{
		if (m_programs.size() >= MAX_PROGRAMS)
			Flush();
		const auto t0 = std::chrono::steady_clock::now();
		CompiledBlock block;
		const bool have_code = CompileBlock(0, pc, sig, &block);
		auto p = std::make_unique<Program>();
		p->pc = pc;
		p->sig = sig;
		p->size = have_code ? block.instrs * 8 : 8;
		p->bytes.assign(&VU0.Micro[pc], &VU0.Micro[pc] + p->size);
		p->needs_compare = false;
		p->last_start = block.last_start;
		for (const BlockExit& x : block.exits)
			p->exits.push_back(ExitSlot{x, nullptr, 0});
		if (have_code)
		{
			if (WasmJit::Load(block.wasm, "run", &p->fn) != WasmJit::LoadResult::Ok)
			{
				Console.Error("VU0 JIT: a module failed to load; VU0 runs on the interpreter");
				m_usable = false;
				return nullptr;
			}
			m_compiles++;
		}
		m_compile_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		Program* raw = p.get();
		m_programs.push_back(std::move(p));
		m_by_pc[pc >> 3].insert(m_by_pc[pc >> 3].begin(), raw);
		return raw;
	}

	// InterpVU0::Execute (VU0microInterp.cpp:249-300), calling generated code where it can.
	void State::Dispatch(u32 cycles)
	{
		const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU0FPCR);
		m_executes++;

		VU0.VI[REG_TPC].UL <<= 3;
		VU0.flags &= ~VUFLAG_MFLAGSET;
		const u64 startcycles = VU0.cycle;
		Program* next = nullptr;
		bool force_step = false;
		while ((VU0.cycle - startcycles) < cycles)
		{
			if (!(VU0.VI[REG_VPU_STAT].UL & 0x1))
			{
				if (VU0.branch)
				{
					VU0.VI[REG_TPC].UL = VU0.branchpc;
					VU0.branch = 0;
				}
				break;
			}
			if (VU0.flags & VUFLAG_MFLAGSET)
				break;

			Program* p = next;
			next = nullptr;
			if (!force_step && !p && VU0.branch == 0 && VU0.ebit == 0)
			{
				Signature sig;
				if (DeriveSignature(VU0, &sig) == Refusal::None)
					p = Find(VU0.VI[REG_TPC].UL & VU0_PROGMASK, sig);
			}
			if (force_step || !m_usable || !p || !p->fn.IsLoaded() || (VU0.cycle - startcycles) + p->last_start >= cycles)
			{
				force_step = false;
				CpuIntVU0.Step();
				m_steps++;
				continue;
			}

			m_entries++;
			NormalizeRings(VU0);
			const u64 base = VU0.cycle;
			g_block_base0 = base;
			const u32 id = static_cast<u32>(reinterpret_cast<s32 (*)(s32)>(static_cast<uptr>(p->fn.slot))(0));
			pxAssertRel(id < p->exits.size(), "VU0 JIT: generated code returned a bad exit");
			ExitSlot& slot = p->exits[id];
			const BlockExit& x = slot.x;
			ApplyExit(VU0, x, base);
			m_jit_instrs += x.instrs;
			if (x.kind == ExitKind::End)
				continue;
			if (x.reason != ExitReason::Block && x.reason != ExitReason::Cap)
			{
				force_step = true;
				continue;
			}
			if (x.chain)
			{
				const u32 tpc = VU0.VI[REG_TPC].UL & VU0_PROGMASK;
				if (x.dynamic_tpc)
					next = Find(tpc, x.sig);
				else if (slot.next && slot.next_epoch == m_epoch)
					next = slot.next;
				else
				{
					const u64 flushes = m_flushes;
					next = Find(tpc, x.sig);
					if (m_flushes == flushes)
					{
						slot.next = next;
						slot.next_epoch = m_epoch;
					}
				}
			}
		}
		VU0.VI[REG_TPC].UL >>= 3;

		// The rest of InterpVU0::Execute (VU0microInterp.cpp:274-299), word for word.
		if (EmuConfig.Speedhacks.EECycleRate != 0 && (!EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Speedhacks.EECycleRate < 0))
		{
			u64 cycle_change = VU0.cycle - startcycles;
			VU0.cycle -= cycle_change;
			switch (std::min(static_cast<int>(EmuConfig.Speedhacks.EECycleRate), static_cast<int>(cycle_change)))
			{
				case -3: cycle_change *= 2.0f; break;
				case -2: cycle_change *= 1.6666667f; break;
				case -1: cycle_change *= 1.3333333f; break;
				case 1: cycle_change /= 1.3f; break;
				case 2: cycle_change /= 1.8f; break;
				case 3: cycle_change /= 3.0f; break;
				default: break;
			}
			VU0.cycle += cycle_change;
		}
		VU0.nextBlockCycles = (VU0.cycle - cpuRegs.cycle) + 1;
	}

	void State::Execute(u32 cycles)
	{
		// Settings generated code depends on (VU1JitEmit.cpp): both overflow clamps, the ADDi hack, the memory.
		if (!EmuConfig.Cpu.Recompiler.vu0Overflow || !EmuConfig.Cpu.Recompiler.vu1Overflow || !m_usable)
			return CpuIntVU0.Execute(cycles);
		if (m_key_micro != reinterpret_cast<uptr>(VU0.Micro) || m_key_overflow != EmuConfig.Cpu.Recompiler.vu0Overflow ||
			m_key_hack != EmuConfig.Gamefixes.VuAddSubHack)
		{
			Flush();
			m_key_micro = reinterpret_cast<uptr>(VU0.Micro);
			m_key_overflow = EmuConfig.Cpu.Recompiler.vu0Overflow;
			m_key_hack = EmuConfig.Gamefixes.VuAddSubHack;
		}
		if (m_dirty)
		{
			m_dirty = false;
			for (auto& p : m_programs)
				p->needs_compare = true;
			m_epoch++;
		}
		if (s_lockstep)
			ExecuteLockstep(cycles);
		else
			Dispatch(cycles);
	}

	static void Capture(RunState* s)
	{
		std::memcpy(static_cast<void*>(&s->regs), &VU0, sizeof(VURegs));
		std::memset(s->mem, 0, sizeof(s->mem));
		std::memcpy(s->mem, VU0.Mem, VU0_MEMSIZE);
		s->running = (VU0.VI[REG_VPU_STAT].UL & 1) != 0;
		s->path1.Reset();
		s->interrupts = 0;
	}

	static void Restore(const RunState& s)
	{
		std::memcpy(static_cast<void*>(&VU0), &s.regs, sizeof(VURegs));
		std::memcpy(VU0.Mem, s.mem, VU0_MEMSIZE);
	}

	void State::ExecuteLockstep(u32 cycles)
	{
		if (!m_ls_start)
		{
			m_ls_start = std::make_unique<RunState>();
			m_ls_jit = std::make_unique<RunState>();
			m_ls_ref = std::make_unique<RunState>();
		}
		Capture(m_ls_start.get());
		Dispatch(cycles);
		Capture(m_ls_jit.get());
		Restore(*m_ls_start);
		CpuIntVU0.Execute(cycles);
		Capture(m_ls_ref.get());
		m_ls_runs++;
		const std::string diff = CompareRunStates(*m_ls_jit, *m_ls_ref);
		if (!diff.empty() && m_ls_diffs++ < 20)
			Console.Error(fmt::format("VU0 JIT lockstep: run {} from pc {:#06x} differs: {}", m_ls_runs,
				(m_ls_start->regs.VI[REG_TPC].UL << 3) & VU0_PROGMASK, diff));
	}

	void State::LogSummary() const
	{
		Console.WriteLn(fmt::format("VU0 JIT: {} executes, {} blocks compiled ({:.1f} ms), {} flushes, {} entries; {} instructions in "
									"generated code, {} interpreter steps",
			m_executes, m_compiles, m_compile_ms, m_flushes, m_entries, m_jit_instrs, m_steps));
		if (s_lockstep)
			Console.WriteLn(fmt::format("VU0 JIT lockstep: {} runs compared, {} differences", m_ls_runs, m_ls_diffs));
	}

	void LogReport()
	{
		GetState().LogSummary();
	}
} // namespace VU0Jit

WasmVU0::WasmVU0()
{
	m_Idx = 0;
	IsInterpreter = false;
}

void WasmVU0::Shutdown()
{
	VU0Jit::GetState().Flush();
}

void WasmVU0::Reset()
{
	CpuIntVU0.Reset();
	VU0Jit::GetState().Flush();
}

void WasmVU0::SetStartPC(u32 startPC)
{
	VU0.start_pc = startPC;
}

void WasmVU0::Step()
{
	CpuIntVU0.Step();
}

void WasmVU0::Clear(u32 addr, u32 size)
{
	VU0Jit::GetState().Dirty();
}

void WasmVU0::Execute(u32 cycles)
{
	VU0Jit::GetState().Execute(cycles);
}
