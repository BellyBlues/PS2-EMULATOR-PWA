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

	// BeginWindow()/EndWindow(): the start cycle of the instruction that ended the program, and whether an M bit
	// stopped an Execute before.
	static bool s_window = false;
	static bool s_window_mbit = false;
	static u64 s_window_end = 0;
	static u64 s_window_last = ~0ull;

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

		// A module (VU1JitEmit.h, CompileModule): blocks that go to each other inside generated code, and the micro
		// memory they were compiled from. Without a function it marks a (PC, signature) whose first instruction runs on
		// the interpreter.
		struct Module
		{
			struct Range
			{
				u32 start;
				u32 size;
			};
			std::vector<Range> ranges; // one per block
			std::vector<u8> bytes; // the micro memory of the ranges, one after the other
			bool needs_compare;
			WasmJit::LoadedFunction fn;
			std::vector<ExitSlot> exits;
			u32 programs; // its blocks still listed: it is released with the last one
		};

		// A block of a module, listed per PC: the dispatcher can start the module's function there.
		struct Program
		{
			u32 pc;
			Signature sig;
			Module* module;
			u32 index; // the block's index in the module
			u32 need; // CompiledModule::Block::last_start: first_need with slices, the last start without
		};

		static constexpr u32 NUM_PCS = VU0_PROGSIZE / 8;
		static constexpr u32 MAX_PROGRAMS = 4000;
		static constexpr u32 MAX_MODULES = 1000;
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
			bool Usable() const { return m_usable; }
			void LogSummary() const;

		private:
			void Dispatch(u32 cycles);
			bool Matches(Module* m);
			Program* Find(u32 pc, const Signature& sig);
			Program* Compile(u32 pc, const Signature& sig);
			void Drop(Program* p);

			std::array<std::vector<Program*>, NUM_PCS> m_by_pc; // most recently used first
			std::vector<std::unique_ptr<Program>> m_programs;
			std::vector<std::unique_ptr<Module>> m_modules;
			u32 m_epoch = 1;
			bool m_dirty = true;
			bool m_usable = true;
			uptr m_key_micro = 0;
			bool m_key_overflow = false;
			bool m_key_hack = false;

			std::unique_ptr<RunState> m_ls_start, m_ls_jit, m_ls_ref;
			u64 m_ls_runs = 0, m_ls_diffs = 0;

			u64 m_executes = 0, m_compiles = 0, m_blocks = 0, m_entries = 0, m_edges = 0, m_jit_instrs = 0, m_steps = 0, m_flushes = 0;
			// Why the dispatcher stepped the interpreter: forced after an exit, a branch or E bit pending, the signature
			// refused, no code for the PC, the slice too short for the block's first instruction(s) (first_need).
			u64 m_why[6] = {};
			u64 m_force[static_cast<int>(ExitReason::Count)] = {};

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
		for (auto& m : m_modules)
			WasmJit::Unload(&m->fn);
		m_modules.clear();
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
		Module* m = p->module;
		m_programs.erase(std::find_if(m_programs.begin(), m_programs.end(), [p](const auto& q) { return q.get() == p; }));
		if (--m->programs == 0)
		{
			WasmJit::Unload(&m->fn);
			m_modules.erase(std::find_if(m_modules.begin(), m_modules.end(), [m](const auto& q) { return q.get() == m; }));
		}
		m_epoch++;
	}

	bool State::Matches(Module* m)
	{
		// The whole module: its blocks go to each other without the dispatcher.
		if (m->needs_compare)
		{
			const u8* bytes = m->bytes.data();
			for (const Module::Range& r : m->ranges)
			{
				if (std::memcmp(bytes, &VU0.Micro[r.start], r.size) != 0)
					return false;
				bytes += r.size;
			}
			m->needs_compare = false;
		}
		return true;
	}

	Program* State::Find(u32 pc, const Signature& sig)
	{
		auto& list = m_by_pc[pc >> 3];
		for (size_t i = 0; i < list.size(); i++)
		{
			Program* p = list[i];
			if (p->sig != sig || !Matches(p->module))
				continue;
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
		if (m_programs.size() >= MAX_PROGRAMS || m_modules.size() >= MAX_MODULES)
			Flush();
		const auto t0 = std::chrono::steady_clock::now();
		// Every block reachable from the entry goes into its module (no ModuleSource::known): VU0's programs are small,
		// and a block another module holds would otherwise cost a return to the dispatcher.
		CompiledModule cm;
		const bool have_code = CompileModule(0, pc, sig, ModuleSource{}, &cm);
		if (!have_code)
			cm.blocks.push_back(CompiledModule::Block{pc, sig, 1, 0, true}); // a marker: the interpreter runs its instruction
		auto m = std::make_unique<Module>();
		for (const CompiledModule::Block& b : cm.blocks)
		{
			const u32 size = b.instrs * 8;
			m->ranges.push_back(Module::Range{b.pc, size});
			m->bytes.insert(m->bytes.end(), &VU0.Micro[b.pc], &VU0.Micro[b.pc] + size);
		}
		m->needs_compare = false;
		m->programs = 0;
		for (const BlockExit& x : cm.exits)
			m->exits.push_back(ExitSlot{x, nullptr, 0});
		if (have_code)
		{
			if (WasmJit::Load(cm.wasm, "run", &m->fn) != WasmJit::LoadResult::Ok)
			{
				Console.Error("VU0 JIT: a module failed to load; VU0 runs on the interpreter");
				m_usable = false;
				return nullptr;
			}
			m_compiles++;
			m_blocks += cm.blocks.size();
		}
		Module* raw = m.get();
		m_modules.push_back(std::move(m));

		// Every block is an entry for the dispatcher, the module's entry block first (blocks of the module are always
		// entries on VU0: none is entered in a delay slot).
		Program* entry = nullptr;
		for (u32 k = 0; k < cm.blocks.size(); k++)
		{
			const CompiledModule::Block& b = cm.blocks[k];
			auto& list = m_by_pc[b.pc >> 3];
			if (list.size() >= MAX_VERSIONS_PER_PC)
			{
				if (k != 0 && list.back()->module == raw)
					continue; // reached inside the module only
				Drop(list.back());
			}
			auto p = std::make_unique<Program>();
			p->pc = b.pc;
			p->sig = b.sig;
			p->module = raw;
			p->index = k;
			p->need = b.last_start;
			raw->programs++;
			list.insert(list.begin(), p.get());
			if (k == 0)
				entry = p.get();
			m_programs.push_back(std::move(p));
		}
		m_compile_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		m_epoch++;
		return entry;
	}

	// InterpVU0::Execute (VU0microInterp.cpp:249-300), calling generated code where it can.
	void State::Dispatch(u32 cycles)
	{
		const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU0FPCR);
		m_executes++;

		VU0.VI[REG_TPC].UL <<= 3;
		VU0.flags &= ~VUFLAG_MFLAGSET;
		const u64 startcycles = VU0.cycle;
		g_budget_end0 = startcycles + cycles; // generated code leaves where this slice ends (g_vu0_slices)
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
			const bool pending = VU0.branch != 0 || VU0.ebit != 0;
			bool refused = false;
			if (!force_step && !p && !pending)
			{
				Signature sig;
				refused = DeriveSignature(VU0, &sig) != Refusal::None;
				if (!refused)
					p = Find(VU0.VI[REG_TPC].UL & VU0_PROGMASK, sig);
			}
			const bool loaded = p && p->module->fn.IsLoaded();
			// With slices, the block's code stops where the slice ends; without, the whole block must fit.
			const bool fits = loaded && (VU0.cycle - startcycles) + p->need < cycles;
			if (force_step || !m_usable || !fits)
			{
				int why = 4;
				if (force_step)
					why = 0;
				else if (pending)
					why = 1;
				else if (refused)
					why = 2;
				else if (!loaded)
					why = 3;
				m_why[why]++;
				force_step = false;
				const u64 before = VU0.cycle;
				CpuIntVU0.Step();
				m_steps++;
				if (s_window)
				{
					if (!(VU0.VI[REG_VPU_STAT].UL & 0x1))
						s_window_last = before;
					else if (VU0.flags & VUFLAG_MFLAGSET)
						s_window_mbit = true;
				}
				continue;
			}

			m_entries++;
			NormalizeRings(VU0);
			g_block_base0 = VU0.cycle; // generated code moves it to each block it goes to
			Module* m = p->module;
			const u32 id = static_cast<u32>(reinterpret_cast<s32 (*)(s32)>(static_cast<uptr>(m->fn.slot))(static_cast<s32>(p->index)));
			pxAssertRel(id < m->exits.size(), "VU0 JIT: generated code returned a bad exit");
			ExitSlot& slot = m->exits[id];
			const BlockExit& x = slot.x;
			ApplyExit(VU0, x, g_block_base0); // the base of the block that exited
			m_jit_instrs += x.instrs + g_chain_instrs0;
			m_edges += g_chain_edges0;
			if (s_window)
			{
				if (x.kind == ExitKind::End)
					s_window_last = g_block_base0 + x.last_cyc;
				else if (x.mbit)
					s_window_mbit = true;
			}
			if (x.kind == ExitKind::End)
				continue;
			// A slice's end (ExitReason::Budget): the loop's test stops it, or the interpreter takes the one instruction
			// before a delay slot that still fits (CompiledBlock::first_need of the block there).
			if (x.reason != ExitReason::Block && x.reason != ExitReason::Cap && x.reason != ExitReason::Budget)
			{
				m_force[static_cast<int>(x.reason)]++;
				force_step = true;
				continue;
			}
			if (x.chain)
			{
				const u32 tpc = VU0.VI[REG_TPC].UL & VU0_PROGMASK;
				const Signature sig = x.sig; // Find may drop the module x belongs to
				if (x.dynamic_tpc)
					next = Find(tpc, sig);
				else if (slot.next && slot.next_epoch == m_epoch)
					next = slot.next;
				else
				{
					const u32 epoch = m_epoch;
					next = Find(tpc, sig);
					if (m_epoch == epoch) // nothing was compiled, dropped or flushed: the slot is still there
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
			for (auto& m : m_modules)
				m->needs_compare = true;
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
		Console.WriteLn(fmt::format("VU0 JIT: {} executes, {} modules compiled ({} blocks, {:.1f} ms), {} flushes, {} entries, {} times a "
									"block went to the next without leaving generated code; {} instructions in generated code, {} "
									"interpreter steps",
			m_executes, m_compiles, m_blocks, m_compile_ms, m_flushes, m_entries, m_edges, m_jit_instrs, m_steps));
		Console.WriteLn(fmt::format("VU0 JIT: interpreter steps: {} after an exit ({} fallbacks, {} D/T bits), {} with a branch or E bit "
									"pending, {} signature refused, {} no code, {} slice too short",
			m_why[0], m_force[static_cast<int>(ExitReason::Fallback)], m_force[static_cast<int>(ExitReason::DTBit)], m_why[1], m_why[2],
			m_why[3], m_why[4]));
		if (s_lockstep)
			Console.WriteLn(fmt::format("VU0 JIT lockstep: {} runs compared, {} differences", m_ls_runs, m_ls_diffs));
	}

	void LogReport()
	{
		GetState().LogSummary();
	}

	bool CanTrackWindow()
	{
		return CpuVU0 == &CpuWasmVU0 && EmuConfig.Cpu.Recompiler.vu0Overflow && EmuConfig.Cpu.Recompiler.vu1Overflow &&
		       GetState().Usable() && !s_lockstep;
	}

	void BeginWindow(u64 end)
	{
		s_window = true;
		s_window_mbit = false;
		s_window_end = end;
		s_window_last = ~0ull;
	}

	bool EndWindow()
	{
		s_window = false;
		return !s_window_mbit && s_window_last < s_window_end;
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
