// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// EE (R5900) JIT for WebAssembly.
//
// Guest code is compiled block by block into WebAssembly functions, several blocks of one 4 KB page per module,
// compiled and instantiated synchronously on the EE thread and called through its function table
// (WasmJitRuntime.h). A block is a superblock: it runs from its entry to an unconditional jump, a branch the JIT
// leaves to the interpreter, the end of the page or a length cap; taken conditional branches leave it.
//
// The code reproduces the interpreter (pcsx2/Interpreter.cpp) exactly, so that emulation does not change:
// - Cycles: every instruction adds its opcode table cycles times the Config.DIE factor to cpuBlockCycles before it
//   runs, as execI() does. The sum is kept at compile time and written before anything that can read it.
// - Branches: the delay slot runs with cpuRegs.branch set, then intJitBranchEnd() runs the interpreter's own branch
//   end (wait loop skip, intUpdateCPUCycles(), the event test and the BIOS hooks). Each module does its usual case
//   in wasm and calls the event test only when an event is due (EmitBranchEndFunction()). Branches not taken run the
//   interpreter's event test where it does (BEQ/BNE and the likely branches), also only when it has something to do.
//   (Inline at each branch, the branch end made the code 45% larger and SpiderMonkey 20% slower.)
// - Other instructions: ALU, shifts, HI/LO moves, multiplies, the aligned loads and stores of RAM/ROM/scratchpad and
//   LDL/LDR/SDL/SDR there, and with VU0 idle the common COP2 ops (FMAC ops with their flags, moves, LQC2/SQC2,
//   QMFC2/QMTC2, CFC2/CTC2) the FPU's moves, compares, BC1x branches and arithmetic (FPU.cpp's clamping and flags;
//   not DIV, SQRT, RSQRT) are emitted; everything else (COP0, other FPU and COP2 ops, MMI, divides, other unaligned
//   and I/O accesses, traps, syscalls) calls the interpreter's function for that opcode with cpuRegs.pc and cpuRegs.code set as execI() sets
//   them. After such a call, a changed PC (an exception) leaves the block.
// - Dispatch: a block that ends goes on to the next one itself (Leave()): a tail call to the target's function when
//   it is in the same module, else a lookup of the dispatcher's cache in wasm, so that the C++ dispatcher (Execute())
//   only runs for compiles, invalidations and the straight-line event test. -eejit-link 0 turns that off.
// - Self-modifying code: each block checks, on entry, its page's vtlb mapping (TLB changes) and, after a write to
//   its page (write tracking, CodeWrites.h), its instruction words against the values it was compiled from, and is
//   recompiled when they differ. Code that changes the rest of its own block while it runs is not detected (the
//   interpreter would see the new instruction); needs proper testing with games that do that.

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "vtlb.h"
#include "VUmicro.h"
#include "R3000A.h"

#include "wasm/CodeWrites.h"
#include "wasm/EEJit.h"
#include "wasm/WasmEmitter.h"
#include "wasm/WasmJitRuntime.h"

#include "common/Console.h"
#include "common/Timer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace R5900;
using namespace R5900::Interpreter::OpcodeImpl;
using namespace vtlb_private;

namespace EEJit
{
	int g_mode = 1;
	bool g_link = true;
	bool g_trace = false;
	u32 g_trace_shift = 16;
	u64 g_trace_from = ~0ull, g_trace_to = 0;
	bool g_stats = false;
	u32 g_dbgmask = 0;
} // namespace EEJit

namespace CodeWrites
{
	u32 g_gen[1u << 20];

	namespace
	{
		u32 s_counter = 0;
		std::vector<u32> s_code_pages;
		u8 s_is_code_page[1u << 20];
	} // namespace

	u32 Current(u32 page)
	{
		if (!g_gen[page])
		{
			if (++s_counter == 0)
				s_counter = 1;
			g_gen[page] = s_counter;
		}
		return g_gen[page];
	}

	void AddCodePage(u32 page)
	{
		if (!s_is_code_page[page])
		{
			s_is_code_page[page] = 1;
			s_code_pages.push_back(page);
		}
	}

	void NoteAll()
	{
		for (u32 page : s_code_pages)
			g_gen[page] = 0;
	}

	u32* Counter() { return &s_counter; }
} // namespace CodeWrites

namespace
{
	using WasmJit::Function;
	using WasmJit::ValType;

	constexpr u32 kPageWords = 1024;
	constexpr u32 kMaxBlockInstrs = 256;
	constexpr u32 kMaxGroupBlocks = 32;
	constexpr size_t kMaxGroups = 6000; // about 20 KB each in V8; flush everything beyond

	using BlockFn = int (*)();
	constexpr u32 kAnyPc = 1; // Leave(): an exit to whatever cpuRegs.pc is

	// Each module's first functions: the branch end and branch test (EmitBranchEndFunction() and
	// EmitBranchTestFunction()), then its blocks, then the lookup (EmitLookup()).
	constexpr u32 kFnBranchEnd = 0;
	constexpr u32 kFnBranchTest = 1;
	constexpr u32 kFnFirstBlock = 2;

	struct Group
	{
		WasmJit::LoadedFunction first; // module and instance handles
		std::unique_ptr<u32[]> checked; // per block: the page generation its code was last compared at
		std::vector<int> slots;
		int live = 0;
	};

	// Compiled blocks by guest PC: s_pages[pc >> 12][(pc >> 2) & 1023] is a table slot, 0 when not compiled.
	int* s_pages[1u << 20];
	std::unordered_map<int, Group*> s_slot_group;
	std::vector<std::unique_ptr<Group>> s_groups;
	bool s_flush_requested = false;
	bool s_link = false; // block linking (tail calls): set by Execute() when the engine has them
	bool s_failed = false;
	uptr s_vmap_base = 0;

	// Statistics.
	u64 s_blocks_compiled = 0, s_groups_compiled = 0, s_invalidations = 0, s_flushes = 0, s_interp_steps = 0;
	u64 s_compile_ns = 0, s_wasm_bytes = 0, s_guest_instrs = 0, s_native_instrs = 0;
	std::vector<std::string> s_stat_names;
	std::unordered_map<std::string, u32> s_stat_index;
	u64 s_stat_counts[512];

	u32 StatIndex(const char* name)
	{
		auto it = s_stat_index.find(name);
		if (it != s_stat_index.end())
			return it->second;
		const u32 i = static_cast<u32>(std::min<size_t>(s_stat_names.size(), std::size(s_stat_counts) - 1));
		if (s_stat_names.size() < std::size(s_stat_counts))
		{
			s_stat_names.emplace_back(name);
			s_stat_index.emplace(name, i);
		}
		return i;
	}

	template <typename T>
	u32 Addr(const T* p)
	{
		return static_cast<u32>(reinterpret_cast<uptr>(p));
	}
	template <typename F>
	u32 FnIndex(F* f)
	{
		return static_cast<u32>(reinterpret_cast<uptr>(f));
	}

	u32 GPR(u32 r) { return Addr(&cpuRegs.GPR.r[r].UD[0]); }
	u32 GPRHi(u32 r) { return Addr(&cpuRegs.GPR.r[r].UD[1]); }

	enum class Kind
	{
		Fallback,
		Nop,
		// Shifts by sa / by register.
		SLL, SRL, SRA, DSLL, DSRL, DSRA, DSLL32, DSRL32, DSRA32,
		SLLV, SRLV, SRAV, DSLLV, DSRLV, DSRAV,
		// rd = rs op rt.
		ADDU, SUBU, DADDU, DSUBU, AND, OR, XOR, NOR, SLT, SLTU, MOVZ, MOVN,
		MULT, MULTU, MFHI, MFLO, MTHI, MTLO, MFSA, MTSA,
		// rt = rs op imm.
		ADDIU, DADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI,
		// Memory.
		LB, LBU, LH, LHU, LW, LWU, LD, LQ, SB, SH, SW, SD, SQ, LWC1, SWC1, LDL, LDR, SDL, SDR, LQC2, SQC2,
		// COP2: VU0 macro mode (the ops EmitCop2() knows; the rest call the interpreter).
		COP2,
		// COP1 (FPU.cpp).
		MFC1, MTC1, CFC1, CTC1, ADD_S, SUB_S, MUL_S, ADDA_S, SUBA_S, MULA_S, MADD_S, MSUB_S, MADDA_S, MSUBA_S,
		ABS_S, NEG_S, MOV_S, MAX_S, MIN_S, C_F, C_EQ, C_LT, C_LE, CVT_S, CVT_W,
		// Branches.
		J, JAL, JR, JALR, BEQ, BNE, BLEZ, BGTZ, BEQL, BNEL, BLEZL, BGTZL,
		BLTZ, BGEZ, BLTZL, BGEZL, BLTZAL, BGEZAL, BLTZALL, BGEZALL,
		BC1F, BC1T, BC1FL, BC1TL,
	};

	struct Decoded
	{
		const OPCODE* op;
		Kind kind;
	};

	Kind Classify(const OPCODE& op)
	{
		using FP = void (*)();
		struct Entry
		{
			FP f;
			Kind k;
		};
		static const Entry table[] = {
			{SLL, Kind::SLL}, {SRL, Kind::SRL}, {SRA, Kind::SRA}, {DSLL, Kind::DSLL}, {DSRL, Kind::DSRL},
			{DSRA, Kind::DSRA}, {DSLL32, Kind::DSLL32}, {DSRL32, Kind::DSRL32}, {DSRA32, Kind::DSRA32},
			{SLLV, Kind::SLLV}, {SRLV, Kind::SRLV}, {SRAV, Kind::SRAV}, {DSLLV, Kind::DSLLV}, {DSRLV, Kind::DSRLV},
			{DSRAV, Kind::DSRAV},
			{ADDU, Kind::ADDU}, {SUBU, Kind::SUBU}, {DADDU, Kind::DADDU}, {DSUBU, Kind::DSUBU}, {AND, Kind::AND},
			{OR, Kind::OR}, {XOR, Kind::XOR}, {NOR, Kind::NOR}, {SLT, Kind::SLT}, {SLTU, Kind::SLTU},
			{MOVZ, Kind::MOVZ}, {MOVN, Kind::MOVN}, {MULT, Kind::MULT}, {MULTU, Kind::MULTU}, {MFHI, Kind::MFHI},
			{MFLO, Kind::MFLO}, {MTHI, Kind::MTHI}, {MTLO, Kind::MTLO}, {MFSA, Kind::MFSA}, {MTSA, Kind::MTSA},
			{ADDIU, Kind::ADDIU}, {DADDIU, Kind::DADDIU}, {SLTI, Kind::SLTI}, {SLTIU, Kind::SLTIU},
			{ANDI, Kind::ANDI}, {ORI, Kind::ORI}, {XORI, Kind::XORI}, {LUI, Kind::LUI},
			{LB, Kind::LB}, {LBU, Kind::LBU}, {LH, Kind::LH}, {LHU, Kind::LHU}, {LW, Kind::LW}, {LWU, Kind::LWU},
			{LD, Kind::LD}, {LQ, Kind::LQ}, {SB, Kind::SB}, {SH, Kind::SH}, {SW, Kind::SW}, {SD, Kind::SD},
			{SQ, Kind::SQ}, {LWC1, Kind::LWC1}, {SWC1, Kind::SWC1}, {LDL, Kind::LDL}, {LDR, Kind::LDR},
			{SDL, Kind::SDL}, {SDR, Kind::SDR},
			{LQC2, Kind::LQC2}, {SQC2, Kind::SQC2}, {COP2, Kind::COP2},
			{COP1::MFC1, Kind::MFC1}, {COP1::MTC1, Kind::MTC1}, {COP1::CFC1, Kind::CFC1}, {COP1::CTC1, Kind::CTC1},
			{COP1::ADD_S, Kind::ADD_S}, {COP1::SUB_S, Kind::SUB_S}, {COP1::MUL_S, Kind::MUL_S},
			{COP1::ADDA_S, Kind::ADDA_S}, {COP1::SUBA_S, Kind::SUBA_S}, {COP1::MULA_S, Kind::MULA_S},
			{COP1::MADD_S, Kind::MADD_S}, {COP1::MSUB_S, Kind::MSUB_S}, {COP1::MADDA_S, Kind::MADDA_S},
			{COP1::MSUBA_S, Kind::MSUBA_S}, {COP1::ABS_S, Kind::ABS_S}, {COP1::NEG_S, Kind::NEG_S},
			{COP1::MOV_S, Kind::MOV_S}, {COP1::MAX_S, Kind::MAX_S}, {COP1::MIN_S, Kind::MIN_S}, {COP1::C_F, Kind::C_F},
			{COP1::C_EQ, Kind::C_EQ}, {COP1::C_LT, Kind::C_LT}, {COP1::C_LE, Kind::C_LE}, {COP1::CVT_S, Kind::CVT_S},
			{COP1::CVT_W, Kind::CVT_W},
			{COP1::BC1F, Kind::BC1F}, {COP1::BC1T, Kind::BC1T}, {COP1::BC1FL, Kind::BC1FL}, {COP1::BC1TL, Kind::BC1TL},
			{SYNC, Kind::Nop}, {PREF, Kind::Nop},
			{J, Kind::J}, {JAL, Kind::JAL}, {JR, Kind::JR}, {JALR, Kind::JALR}, {BEQ, Kind::BEQ}, {BNE, Kind::BNE},
			{BLEZ, Kind::BLEZ}, {BGTZ, Kind::BGTZ}, {BEQL, Kind::BEQL}, {BNEL, Kind::BNEL}, {BLEZL, Kind::BLEZL},
			{BGTZL, Kind::BGTZL}, {BLTZ, Kind::BLTZ}, {BGEZ, Kind::BGEZ}, {BLTZL, Kind::BLTZL}, {BGEZL, Kind::BGEZL},
			{BLTZAL, Kind::BLTZAL}, {BGEZAL, Kind::BGEZAL}, {BLTZALL, Kind::BLTZALL}, {BGEZALL, Kind::BGEZALL},
		};
		for (const Entry& e : table)
		{
			if (op.interpret == e.f)
				return e.k;
		}
		return Kind::Fallback;
	}

	bool IsNativeBranch(Kind k) { return k >= Kind::J; }
		// Branches the JIT leaves to the interpreter (ERET, SYSCALL, BC0x, BC1x) end the block. COP2's BC2x are not
	// flagged; as calls they still behave (the PC moves when taken).
	bool IsBranch(const Decoded& d) { return IsNativeBranch(d.kind) || (d.op->flags & IS_BRANCH); }

	Decoded Decode(u32 code)
	{
		const OPCODE& op = GetInstruction(code);
		Kind k = Classify(op);
		// Game fixes that hook these jumps stay with the interpreter.
		if ((k == Kind::JAL || k == Kind::JR) && EmuConfig.Gamefixes.GoemonTlbHack)
			k = Kind::Fallback;
		// With the EE cache emulated, memory goes through the interpreter's cache model.
		if (k >= Kind::LB && k <= Kind::SQC2 && CHECK_CACHE)
			k = Kind::Fallback;
		return {&op, k};
	}

	// ------------------------------------------------------------------------------------------------------------
	// Code generation

	class BlockCompiler
	{
	public:
		BlockCompiler(u32 type_v_v, u32 type_i_v, u32 page_vaddr, const u32* host_page, u64 vmap_raw, u32* checked)
			: m_checked(checked)
			, m_tvv(type_v_v)
			, m_tiv(type_i_v)
			, m_page(page_vaddr)
			, m_host(host_page)
			, m_vmap_raw(vmap_raw)
			, m_fn(0)
		{
			m_a = m_fn.local(ValType::I32);
			m_j = m_fn.local(ValType::I32);
			m_cyc = m_fn.local(ValType::I32);
			m_f = m_fn.local(ValType::I32);
			m_v = m_fn.local(ValType::I64);
			m_k = m_fn.local(ValType::I64);
		}

		u32 m_next_pc = 0;

	private:
		u32 Word(u32 pc) const { return m_host[(pc - m_page) >> 2]; }

		// The furthest word the block can read (exclusive), following the same rules as Compile().
		u32 Scan(u32 start)
		{
			u32 pc = start;
			u32 count = 0;
			for (;;)
			{
				if (pc >= m_page + 4096 || count >= kMaxBlockInstrs)
					return pc;
				const Decoded d = Decode(Word(pc));
				count++;
				if (IsNativeBranch(d.kind))
				{
					const bool ds_ok = (pc + 4 < m_page + 4096) && !IsBranch(Decode(Word(pc + 4)));
					if (!ds_ok)
						return pc + 4;
					switch (d.kind)
					{
						case Kind::J:
						case Kind::JAL:
						case Kind::JR:
						case Kind::JALR:
							return pc + 8;
						case Kind::BEQL:
						case Kind::BNEL:
						case Kind::BLEZL:
						case Kind::BGTZL:
						case Kind::BLTZL:
						case Kind::BGEZL:
						case Kind::BLTZALL:
						case Kind::BGEZALL:
						case Kind::BC1FL:
						case Kind::BC1TL:
							pc += 8; // the delay slot is read by the taken path
							continue;
						default:
							pc += 4;
							continue;
					}
				}
				if (IsBranch(d))
					return pc + 4;
				pc += 4;
			}
		}

		void EmitEntryChecks(u32 start, u32 end)
		{
			Function& f = m_fn;
			f.block();
			f.block();
			// Same mapping (TLB changes).
			f.i32_const(0);
			f.i64_load(static_cast<u32>(s_vmap_base + (m_page >> 12) * sizeof(VTLBVirtual)));
			f.i64_const(static_cast<s64>(m_vmap_raw));
			f.i64_ne();
			f.br_if(0);
			const u32 host = Addr(m_host) + (start - m_page);
			const u32 nwords = (end - start) >> 2;
			// No write to the page since the last compare (CodeWrites.h).
			const bool tracked = CodeWrites::EmitSkipCompare(f, host, nwords * 4, m_checked, m_a);
			// Same code.
			u32 i = 0;
			for (; i + 2 <= nwords; i += 2)
			{
				u64 v;
				std::memcpy(&v, &m_host[((start - m_page) >> 2) + i], 8);
				f.i32_const(0);
				f.i64_load(host + i * 4, 2);
				f.i64_const(static_cast<s64>(v));
				f.i64_ne();
				f.br_if(0);
			}
			if (i < nwords)
			{
				f.i32_const(0);
				f.i32_load(host + i * 4);
				f.i32_const(static_cast<s32>(m_host[((start - m_page) >> 2) + i]));
				f.i32_ne();
				f.br_if(0);
			}
			if (tracked)
				CodeWrites::EmitCompared(f, m_checked, m_a);
			f.br(1);
			f.end();
			f.i32_const(1);
			f.return_();
			f.end();
		}

		// After a generated store to the host address in the low half of m_v (aligned: one page).
		void EmitNoteWrite()
		{
			m_fn.local_get(m_v);
			m_fn.i32_wrap_i64();
			CodeWrites::EmitNoteWrite(m_fn);
		}

		// ---- cycles -------------------------------------------------------------------------------------------
		void RefreshFactor()
		{
			Function& f = m_fn;
			f.i32_const(2);
			f.i32_const(0);
			f.i32_load(Addr(&cpuRegs.CP0.n.Config));
			f.i32_const(18);
			f.i32_shr_u();
			f.i32_const(1);
			f.i32_and();
			f.i32_sub();
			f.local_set(m_f);
		}

		void PushPending()
		{
			Function& f = m_fn;
			f.i32_const(m_pending);
			f.local_get(m_f);
			f.i32_mul();
		}

		// Writes every pending cycle to cpuBlockCycles.
		void FlushCycles()
		{
			if (!m_pending && !m_cyc_dirty)
				return;
			Function& f = m_fn;
			const u32 bc = Addr(intJitBlockCycles());
			f.i32_const(0);
			f.i32_const(0);
			f.i32_load(bc);
			if (m_cyc_dirty)
			{
				f.local_get(m_cyc);
				f.i32_add();
			}
			if (m_pending)
			{
				PushPending();
				f.i32_add();
			}
			f.i32_store(bc);
			if (m_cyc_dirty)
			{
				f.i32_const(0);
				f.local_set(m_cyc);
			}
			m_pending = 0;
			m_cyc_dirty = false;
		}

		// Inside a slow path: writes the pending cycles and leaves the runtime counter so that, where the slow and
		// fast paths join, m_cyc + m_pending * factor is what is still pending on both.
		void FlushCyclesSlowPath()
		{
			if (!m_pending && !m_cyc_dirty)
				return;
			Function& f = m_fn;
			const u32 bc = Addr(intJitBlockCycles());
			f.i32_const(0);
			f.i32_const(0);
			f.i32_load(bc);
			if (m_cyc_dirty)
			{
				f.local_get(m_cyc);
				f.i32_add();
			}
			if (m_pending)
			{
				PushPending();
				f.i32_add();
			}
			f.i32_store(bc);
			f.i32_const(0);
			if (m_pending)
			{
				PushPending();
				f.i32_sub();
			}
			f.local_set(m_cyc);
		}

		// ---- helpers ------------------------------------------------------------------------------------------
		void StorePcCode(u32 pc, u32 code)
		{
			Function& f = m_fn;
			f.i32_const(0);
			f.i32_const(static_cast<s32>(pc + 4));
			f.i32_store(Addr(&cpuRegs.pc));
			f.i32_const(0);
			f.i32_const(static_cast<s32>(code));
			f.i32_store(Addr(&cpuRegs.code));
		}

		void CallVoid(u32 fn)
		{
			m_fn.i32_const(static_cast<s32>(fn));
			m_fn.call_indirect(m_tvv, 0);
		}

		// Leaves the block if an exception moved the PC.
		void CheckPc(u32 expected)
		{
			Function& f = m_fn;
			f.i32_const(0);
			f.i32_load(Addr(&cpuRegs.pc));
			f.i32_const(static_cast<s32>(expected));
			f.i32_ne();
			f.if_();
			f.i32_const(0);
			f.return_();
			f.end();
		}

		void ExitTo(u32 pc)
		{
			Function& f = m_fn;
			FlushCycles();
			f.i32_const(0);
			f.i32_const(static_cast<s32>(pc));
			f.i32_store(Addr(&cpuRegs.pc));
			Leave(pc, true);
		}

		// Leaves the block for `target` (kAnyPc: whatever cpuRegs.pc is) with everything written, as the dispatcher
		// expects. Block linking: when the dispatcher would not run its straight-line event test (cpuBlockCycles
		// below 2^20), the next block runs without going back to it, its own entry checks running as they would
		// from the dispatcher: a target in this page (when the PC is that target: pc_known, or checked here, as a
		// branch end can take an exception) is tail-called directly; anything else goes through the module's
		// lookup function (EmitLookup()), the dispatcher's cache lookup in wasm. CompileGroup() patches the calls
		// with the index of the target's function in this module, or of the lookup function.
		void Leave(u32 target, bool pc_known)
		{
			Function& f = m_fn;
			if (!s_link)
			{
				f.i32_const(0);
				f.return_();
				return;
			}
			if (target >= m_page && target < m_page + 4096 && !(target & 3))
			{
				f.i32_const(0);
				f.i32_load(Addr(intJitBlockCycles()));
				f.i32_const(1 << 20);
				f.i32_lt_u();
				if (!pc_known)
				{
					f.i32_const(0);
					f.i32_load(Addr(&cpuRegs.pc));
					f.i32_const(static_cast<s32>(target));
					f.i32_eq();
					f.i32_and();
				}
				f.if_();
				m_links.push_back({f.return_call_patchable(), target});
				f.end();
			}
			m_links.push_back({f.return_call_patchable(), kAnyPc});
		}

		void CountStat(const char* name)
		{
			if (!EEJit::g_stats)
				return;
			Function& f = m_fn;
			const u32 a = Addr(&s_stat_counts[StatIndex(name)]);
			f.i32_const(0);
			f.i32_const(0);
			f.i64_load(a);
			f.i64_const(1);
			f.i64_add();
			f.i64_store(a);
		}

		void EmitFallback(u32 pc, u32 code, const Decoded& d, bool in_delay_slot)
		{
			m_pending += d.op->cycles;
			CountStat(d.op->Name);
			StorePcCode(pc, code);
			FlushCycles();
			CallVoid(FnIndex(d.op->interpret));
			if ((code >> 26) == 0x10) // COP0: MTC0 may change Config
				RefreshFactor();
			if (!in_delay_slot)
				CheckPc(pc + 4);
		}

		// ---- instructions -------------------------------------------------------------------------------------
		static u32 Rs(u32 c) { return (c >> 21) & 31; }
		static u32 Rt(u32 c) { return (c >> 16) & 31; }
		static u32 Rd(u32 c) { return (c >> 11) & 31; }
		static u32 Sa(u32 c) { return (c >> 6) & 31; }
		static s32 Imm(u32 c) { return static_cast<s16>(c & 0xffff); }
		static u32 ImmU(u32 c) { return c & 0xffff; }

		void LoadGPR64(u32 r) { m_fn.i32_const(0); m_fn.i64_load(GPR(r)); }
		void LoadGPR32(u32 r) { m_fn.i32_const(0); m_fn.i32_load(GPR(r)); }

		// Emits one non-branch instruction.
		void EmitInstruction(u32 pc, u32 code, const Decoded& d, bool in_delay_slot)
		{
			Function& f = m_fn;
			const u32 rs = Rs(code), rt = Rt(code), rd = Rd(code), sa = Sa(code);
			const s32 imm = Imm(code);

			// Instructions whose destination is $zero do nothing, except memory accesses (their side effects and
			// exceptions), which then go to the interpreter.
			auto dst0 = [&](u32 r) {
				if (r != 0)
					return false;
				m_pending += d.op->cycles;
				s_native_instrs++;
				return true;
			};

			// rd.UD = <i64 expression>
			auto store64 = [&](u32 r, auto&& expr) {
				f.i32_const(0);
				expr();
				f.i64_store(GPR(r));
			};
			// rd.SD = (s32)<i32 expression>
			auto store32s = [&](u32 r, auto&& expr) {
				f.i32_const(0);
				expr();
				f.i64_extend_i32_s();
				f.i64_store(GPR(r));
			};

			switch (d.kind)
			{
				case Kind::Nop:
					m_pending += d.op->cycles;
					s_native_instrs++;
					return;

				case Kind::SLL: case Kind::SRL: case Kind::SRA:
					if (dst0(rd)) return;
					store32s(rd, [&] {
						LoadGPR32(rt);
						f.i32_const(sa);
						if (d.kind == Kind::SLL) f.i32_shl(); else if (d.kind == Kind::SRL) f.i32_shr_u(); else f.i32_shr_s();
					});
					break;
				case Kind::SLLV: case Kind::SRLV: case Kind::SRAV:
					if (dst0(rd)) return;
					store32s(rd, [&] {
						LoadGPR32(rt);
						LoadGPR32(rs);
						f.i32_const(0x1f);
						f.i32_and();
						if (d.kind == Kind::SLLV) f.i32_shl(); else if (d.kind == Kind::SRLV) f.i32_shr_u(); else f.i32_shr_s();
					});
					break;
				case Kind::DSLL: case Kind::DSRL: case Kind::DSRA: case Kind::DSLL32: case Kind::DSRL32: case Kind::DSRA32:
				{
					if (dst0(rd)) return;
					const bool is32 = d.kind == Kind::DSLL32 || d.kind == Kind::DSRL32 || d.kind == Kind::DSRA32;
					store64(rd, [&] {
						LoadGPR64(rt);
						f.i64_const(sa + (is32 ? 32 : 0));
						if (d.kind == Kind::DSLL || d.kind == Kind::DSLL32) f.i64_shl();
						else if (d.kind == Kind::DSRL || d.kind == Kind::DSRL32) f.i64_shr_u();
						else f.i64_shr_s();
					});
					break;
				}
				case Kind::DSLLV: case Kind::DSRLV: case Kind::DSRAV:
					if (dst0(rd)) return;
					store64(rd, [&] {
						LoadGPR64(rt);
						LoadGPR32(rs);
						f.i32_const(0x3f);
						f.i32_and();
						f.i64_extend_i32_u();
						if (d.kind == Kind::DSLLV) f.i64_shl(); else if (d.kind == Kind::DSRLV) f.i64_shr_u(); else f.i64_shr_s();
					});
					break;

				case Kind::ADDU: case Kind::SUBU:
					if (dst0(rd)) return;
					store32s(rd, [&] {
						LoadGPR32(rs);
						LoadGPR32(rt);
						if (d.kind == Kind::ADDU) f.i32_add(); else f.i32_sub();
					});
					break;
				case Kind::DADDU: case Kind::DSUBU: case Kind::AND: case Kind::OR: case Kind::XOR: case Kind::NOR:
					if (dst0(rd)) return;
					store64(rd, [&] {
						LoadGPR64(rs);
						LoadGPR64(rt);
						switch (d.kind)
						{
							case Kind::DADDU: f.i64_add(); break;
							case Kind::DSUBU: f.i64_sub(); break;
							case Kind::AND: f.i64_and(); break;
							case Kind::OR: f.i64_or(); break;
							case Kind::XOR: f.i64_xor(); break;
							default: f.i64_or(); f.i64_const(-1); f.i64_xor(); break;
						}
					});
					break;
				case Kind::SLT: case Kind::SLTU:
					if (dst0(rd)) return;
					store64(rd, [&] {
						LoadGPR64(rs);
						LoadGPR64(rt);
						if (d.kind == Kind::SLT) f.i64_lt_s(); else f.i64_lt_u();
						f.i64_extend_i32_u();
					});
					break;
				case Kind::MOVZ: case Kind::MOVN:
					if (dst0(rd)) return;
					LoadGPR64(rt);
					f.i64_eqz();
					if (d.kind == Kind::MOVN)
						f.i32_eqz();
					f.if_();
					store64(rd, [&] { LoadGPR64(rs); });
					f.end();
					break;
				case Kind::MULT: case Kind::MULTU:
				{
					const u32 lo = Addr(&cpuRegs.LO.UD[0]), hi = Addr(&cpuRegs.HI.UD[0]);
					f.i32_const(0);
					if (d.kind == Kind::MULT) f.i64_load32_s(GPR(rs)); else f.i64_load32_u(GPR(rs));
					f.i32_const(0);
					if (d.kind == Kind::MULT) f.i64_load32_s(GPR(rt)); else f.i64_load32_u(GPR(rt));
					f.i64_mul();
					f.local_set(m_v);
					f.i32_const(0);
					f.local_get(m_v);
					f.i64_extend32_s();
					f.i64_store(lo);
					f.i32_const(0);
					f.local_get(m_v);
					f.i64_const(32);
					f.i64_shr_s();
					f.i64_extend32_s();
					f.i64_store(hi);
					if (rd)
						store64(rd, [&] { f.local_get(m_v); f.i64_extend32_s(); });
					break;
				}
				case Kind::MFHI: case Kind::MFLO:
					if (dst0(rd)) return;
					store64(rd, [&] {
						f.i32_const(0);
						f.i64_load(d.kind == Kind::MFHI ? Addr(&cpuRegs.HI.UD[0]) : Addr(&cpuRegs.LO.UD[0]));
					});
					break;
				case Kind::MTHI: case Kind::MTLO:
					f.i32_const(0);
					LoadGPR64(rs);
					f.i64_store(d.kind == Kind::MTHI ? Addr(&cpuRegs.HI.UD[0]) : Addr(&cpuRegs.LO.UD[0]));
					break;
				case Kind::MFSA:
					if (dst0(rd)) return;
					store64(rd, [&] { f.i32_const(0); f.i64_load32_u(Addr(&cpuRegs.sa)); });
					break;
				case Kind::MTSA:
					f.i32_const(0);
					LoadGPR32(rs);
					f.i32_store(Addr(&cpuRegs.sa));
					break;

				case Kind::ADDIU:
					if (dst0(rt)) return;
					store32s(rt, [&] { LoadGPR32(rs); f.i32_const(imm); f.i32_add(); });
					break;
				case Kind::DADDIU:
					if (dst0(rt)) return;
					store64(rt, [&] { LoadGPR64(rs); f.i64_const(imm); f.i64_add(); });
					break;
				case Kind::SLTI: case Kind::SLTIU:
					if (dst0(rt)) return;
					store64(rt, [&] {
						LoadGPR64(rs);
						f.i64_const(imm);
						if (d.kind == Kind::SLTI) f.i64_lt_s(); else f.i64_lt_u();
						f.i64_extend_i32_u();
					});
					break;
				case Kind::ANDI: case Kind::ORI: case Kind::XORI:
					if (dst0(rt)) return;
					store64(rt, [&] {
						LoadGPR64(rs);
						f.i64_const(ImmU(code));
						if (d.kind == Kind::ANDI) f.i64_and(); else if (d.kind == Kind::ORI) f.i64_or(); else f.i64_xor();
					});
					break;
				case Kind::LUI:
					if (dst0(rt)) return;
					store64(rt, [&] { f.i64_const(static_cast<s32>(code << 16)); });
					break;

				case Kind::LB: case Kind::LBU: case Kind::LH: case Kind::LHU: case Kind::LW: case Kind::LWU:
				case Kind::LD: case Kind::LQ: case Kind::LWC1: case Kind::LDL: case Kind::LDR:
				case Kind::SB: case Kind::SH: case Kind::SW: case Kind::SD: case Kind::SQ: case Kind::SWC1:
				case Kind::SDL: case Kind::SDR:
					if (rt == 0 && d.kind != Kind::SB && d.kind != Kind::SH && d.kind != Kind::SW && d.kind != Kind::SD &&
						d.kind != Kind::SQ && d.kind != Kind::LWC1 && d.kind != Kind::SWC1 && d.kind != Kind::SDL &&
						d.kind != Kind::SDR)
					{
						EmitFallback(pc, code, d, in_delay_slot);
						return;
					}
					EmitMemory(pc, code, d, in_delay_slot);
					return;

				case Kind::MFC1: case Kind::MTC1: case Kind::CFC1: case Kind::CTC1: case Kind::ADD_S: case Kind::SUB_S:
				case Kind::MUL_S: case Kind::ADDA_S: case Kind::SUBA_S: case Kind::MULA_S: case Kind::MADD_S:
				case Kind::MSUB_S: case Kind::MADDA_S: case Kind::MSUBA_S: case Kind::ABS_S: case Kind::NEG_S:
				case Kind::MOV_S: case Kind::MAX_S: case Kind::MIN_S: case Kind::C_F: case Kind::C_EQ: case Kind::C_LT:
				case Kind::C_LE: case Kind::CVT_S: case Kind::CVT_W:
					EmitFpu(code, d.kind);
					break;

				case Kind::LQC2: case Kind::SQC2: case Kind::COP2:
					if (!EmitCop2(pc, code, d, in_delay_slot))
						EmitFallback(pc, code, d, in_delay_slot);
					return;

				default:
					EmitFallback(pc, code, d, in_delay_slot);
					return;
			}
			m_pending += d.op->cycles;
			s_native_instrs++;
		}

		// ---- COP1 (FPU.cpp) --------------------------------------------------------------------------------
		u32 FPR(u32 r) { return Addr(&fpuRegs.fpr[r].UL); }
		u32 FCR31() { return Addr(&fpuRegs.fprc[31]); }
		u32 ILocal(int i)
		{
			if (!m_iloc_ok[i])
			{
				m_iloc[i] = m_fn.local(ValType::I32);
				m_iloc_ok[i] = true;
			}
			return m_iloc[i];
		}
		// fpuDouble() of the i32 bits on the stack, as f32: exponent 0 gives the signed zero, 255 the signed maximum.
		void FpuDouble()
		{
			Function& f = m_fn;
			const u32 x = ILocal(0), e = ILocal(1);
			f.local_tee(x);
			f.i32_const(0x7f800000);
			f.i32_and();
			f.local_set(e);
			// e == 0x7f800000 ? (x & sign) | max : x
			f.local_get(x);
			f.i32_const(static_cast<s32>(0x80000000u));
			f.i32_and();
			f.i32_const(0x7f7fffff);
			f.i32_or();
			f.local_get(x);
			f.local_get(e);
			f.i32_const(0x7f800000);
			f.i32_eq();
			f.select();
			// e == 0 ? x & sign : that
			f.local_set(x); // x now holds the clamped value; the sign is the same
			f.local_get(x);
			f.i32_const(static_cast<s32>(0x80000000u));
			f.i32_and();
			f.local_get(x);
			f.local_get(e);
			f.i32_eqz();
			f.select();
			f.f32_reinterpret_i32();
		}
		void FpuDoubleReg(u32 addr)
		{
			m_fn.i32_const(0);
			m_fn.i32_load(addr);
			FpuDouble();
		}
		// FCR31 = (FCR31 & and_mask) | or_value
		void Fcr31AndOr(u32 and_mask, u32 or_value)
		{
			Function& f = m_fn;
			f.i32_const(0);
			f.i32_const(0);
			f.i32_load(FCR31());
			if (and_mask != 0xffffffffu)
			{
				f.i32_const(static_cast<s32>(and_mask));
				f.i32_and();
			}
			if (or_value)
			{
				f.i32_const(static_cast<s32>(or_value));
				f.i32_or();
			}
			f.i32_store(FCR31());
		}
		// The f32 result on the stack goes to dst through checkOverflow(O | SO) and checkUnderflow(U | SU).
		void FpuStoreChecked(u32 dst)
		{
			Function& f = m_fn;
			const u32 r = ILocal(2);
			f.i32_reinterpret_f32();
			f.local_tee(r);
			f.i32_const(0x7fffffff);
			f.i32_and();
			f.i32_const(0x7f800000);
			f.i32_eq();
			f.if_();
			{
				f.local_get(r);
				f.i32_const(static_cast<s32>(0x80000000u));
				f.i32_and();
				f.i32_const(0x7f7fffff);
				f.i32_or();
				f.local_set(r);
				Fcr31AndOr(0xffffffffu, 0x00008000 | 0x00000010);
			}
			f.else_();
			{
				// clear O, then underflow: exponent 0 and a mantissa
				f.local_get(r);
				f.i32_const(0x7f800000);
				f.i32_and();
				f.i32_eqz();
				f.local_get(r);
				f.i32_const(0x007fffff);
				f.i32_and();
				f.i32_const(0);
				f.i32_ne();
				f.i32_and();
				f.if_();
				{
					f.local_get(r);
					f.i32_const(static_cast<s32>(0x80000000u));
					f.i32_and();
					f.local_set(r);
					Fcr31AndOr(~0x00008000u, 0x00004000 | 0x00000008);
				}
				f.else_();
				Fcr31AndOr(~(0x00008000u | 0x00004000u), 0);
				f.end();
			}
			f.end();
			f.i32_const(0);
			f.local_get(r);
			f.i32_store(dst);
		}

		void EmitFpu(u32 code, Kind k)
		{
			Function& f = m_fn;
			const u32 rt = Rt(code), ft = Rt(code), fs = Rd(code), fd = Sa(code);
			const u32 acc = Addr(&fpuRegs.ACC.UL);
			switch (k)
			{
				case Kind::MFC1:
					if (rt == 0)
						return;
					f.i32_const(0);
					f.i32_const(0);
					f.i64_load32_s(FPR(fs));
					f.i64_store(GPR(rt));
					return;
				case Kind::MTC1:
					f.i32_const(0);
					LoadGPR32(rt);
					f.i32_store(FPR(fs));
					return;
				case Kind::CFC1:
					if (rt == 0)
						return;
					f.i32_const(0);
					if (fs == 31)
					{
						f.i32_const(0);
						f.i64_load32_s(FCR31());
					}
					else
						f.i64_const(fs == 0 ? 0x2E00 : 0);
					f.i64_store(GPR(rt));
					return;
				case Kind::CTC1:
					if (fs != 31)
						return;
					f.i32_const(0);
					LoadGPR32(rt);
					f.i32_store(FCR31());
					return;
				case Kind::ADD_S: case Kind::SUB_S: case Kind::MUL_S:
				case Kind::ADDA_S: case Kind::SUBA_S: case Kind::MULA_S:
				{
					const bool a = k == Kind::ADDA_S || k == Kind::SUBA_S || k == Kind::MULA_S;
					FpuDoubleReg(FPR(fs));
					FpuDoubleReg(FPR(ft));
					if (k == Kind::ADD_S || k == Kind::ADDA_S) f.f32_add();
					else if (k == Kind::SUB_S || k == Kind::SUBA_S) f.f32_sub();
					else f.f32_mul();
					FpuStoreChecked(a ? acc : FPR(fd));
					return;
				}
				case Kind::MADD_S: case Kind::MSUB_S:
				{
					// temp = fpuDouble(fs) * fpuDouble(ft); fd = fpuDouble(ACC) +- fpuDouble(temp)
					FpuDoubleReg(acc);
					FpuDoubleReg(FPR(fs));
					FpuDoubleReg(FPR(ft));
					f.f32_mul();
					f.i32_reinterpret_f32();
					FpuDouble();
					if (k == Kind::MADD_S) f.f32_add(); else f.f32_sub();
					FpuStoreChecked(FPR(fd));
					return;
				}
				case Kind::MADDA_S: case Kind::MSUBA_S:
				{
					// ACC +-= fpuDouble(fs) * fpuDouble(ft), on the raw ACC and product
					f.i32_const(0);
					f.f32_load(acc);
					FpuDoubleReg(FPR(fs));
					FpuDoubleReg(FPR(ft));
					f.f32_mul();
					if (k == Kind::MADDA_S) f.f32_add(); else f.f32_sub();
					FpuStoreChecked(acc);
					return;
				}
				case Kind::ABS_S: case Kind::NEG_S:
					f.i32_const(0);
					f.i32_const(0);
					f.i32_load(FPR(fs));
					if (k == Kind::ABS_S)
					{
						f.i32_const(0x7fffffff);
						f.i32_and();
					}
					else
					{
						f.i32_const(static_cast<s32>(0x80000000u));
						f.i32_xor();
					}
					f.i32_store(FPR(fd));
					Fcr31AndOr(~(0x00008000u | 0x00004000u), 0);
					return;
				case Kind::MOV_S:
					f.i32_const(0);
					f.i32_const(0);
					f.i32_load(FPR(fs));
					f.i32_store(FPR(fd));
					return;
				case Kind::MAX_S: case Kind::MIN_S:
				{
					// fp_max/fp_min: both negative ? min/max : max/min, as s32
					const u32 a = ILocal(0), b = ILocal(1);
					const bool mx = k == Kind::MAX_S;
					f.i32_const(0);
					f.i32_load(FPR(fs));
					f.local_set(a);
					f.i32_const(0);
					f.i32_load(FPR(ft));
					f.local_set(b);
					f.i32_const(0);
					// smaller (signed) and larger
					auto smaller = [&] { f.local_get(a); f.local_get(b); f.local_get(a); f.local_get(b); f.i32_lt_s(); f.select(); };
					auto larger = [&] { f.local_get(a); f.local_get(b); f.local_get(a); f.local_get(b); f.i32_gt_s(); f.select(); };
					if (mx) smaller(); else larger();
					if (mx) larger(); else smaller();
					f.local_get(a);
					f.local_get(b);
					f.i32_and();
					f.i32_const(0);
					f.i32_lt_s(); // both negative
					f.select();
					f.i32_store(FPR(fd));
					Fcr31AndOr(~(0x00008000u | 0x00004000u), 0);
					return;
				}
				case Kind::C_F:
					Fcr31AndOr(~0x00800000u, 0);
					return;
				case Kind::C_EQ: case Kind::C_LT: case Kind::C_LE:
				{
					const u32 c = ILocal(2);
					FpuDoubleReg(FPR(fs));
					FpuDoubleReg(FPR(ft));
					if (k == Kind::C_EQ) f.f32_eq(); else if (k == Kind::C_LT) f.f32_lt(); else f.f32_le();
					f.local_set(c);
					f.i32_const(0);
					f.i32_const(0);
					f.i32_load(FCR31());
					f.i32_const(static_cast<s32>(~0x00800000u));
					f.i32_and();
					f.local_get(c);
					f.i32_const(23);
					f.i32_shl();
					f.i32_or();
					f.i32_store(FCR31());
					return;
				}
				case Kind::CVT_S:
					f.i32_const(0);
					f.i32_const(0);
					f.i32_load(FPR(fs));
					f.f32_convert_i32_s();
					f.f32_store(FPR(fd));
					return;
				case Kind::CVT_W:
				{
					// exponent <= 0x4e8: (s32)f; else 0x7fffffff or 0x80000000 by sign
					const u32 x = ILocal(0);
					f.i32_const(0);
					f.i32_const(0);
					f.i32_load(FPR(fs));
					f.local_tee(x);
					f.i32_const(0x7f800000);
					f.i32_and();
					f.i32_const(0x4e800000);
					f.i32_le_u();
					f.if_(static_cast<u8>(ValType::I32));
					f.local_get(x);
					f.f32_reinterpret_i32();
					f.i32_trunc_f32_s();
					f.else_();
					f.i32_const(0x7fffffff);
					f.local_get(x);
					f.i32_const(31);
					f.i32_shr_u();
					f.i32_add(); // 0x7fffffff + 1 = 0x80000000 when negative
					f.end();
					f.i32_store(FPR(fd));
					return;
				}
				default:
					return;
			}
		}

		// ---- COP2 (VU0 macro mode) ------------------------------------------------------------------------
		// Every COP2 instruction first runs VU0 if a micro program is running (vu0Sync(), _vu0FinishMicro(),
		// _vu0WaitMicro() in VU0.cpp): generated code handles only the case where it is not (VPU_STAT bit 0
		// clear, where those calls do nothing) and calls the interpreter's function otherwise. The FMAC ops follow
		// the VU1 JIT's (VU1JitEmit.cpp, Upper(), Clamp(), Flags()), which match VUops.cpp bit for bit, then
		// SYNCMSFLAGS() (VUops.cpp).

		u32 VLocal(int i)
		{
			if (!m_vloc_ok[i])
			{
				m_vloc[i] = m_fn.local(ValType::V128);
				m_vloc_ok[i] = true;
			}
			return m_vloc[i];
		}
		void Splat(u32 v) { m_fn.v128_pooled_i32x4(v, v, v, v); }
		void SimdOp(WasmJit::Simd o) { m_fn.simd(o); }
		void LdV(u32 addr) { m_fn.i32_const(0); m_fn.v128_load(addr); }
		void Ld32(u32 addr) { m_fn.i32_const(0); m_fn.i32_load(addr); }
		static u32 DestMask(u32 dest, u32 lane) { return (dest & (8 >> lane)) ? 0xffffffffu : 0; }
		void DestConst(u32 dest)
		{
			m_fn.v128_pooled_i32x4(DestMask(dest, 0), DestMask(dest, 1), DestMask(dest, 2), DestMask(dest, 3));
		}
		// VF/ACC at addr = local v, lanes outside dest kept.
		void StoreMerged(u32 addr, u32 v, u32 dest)
		{
			if ((dest & 0xf) == 0)
				return;
			m_fn.i32_const(0);
			m_fn.local_get(v);
			if ((dest & 0xf) != 0xf)
			{
				LdV(addr);
				DestConst(dest);
				SimdOp(WasmJit::Simd::V128Bitselect);
			}
			m_fn.v128_store(addr);
		}

		static constexpr u32 EXP = 0x7f800000u;
		static constexpr u32 SIGN = 0x80000000u;

		// local v = vuDouble(v) with the overflow clamp on (also VU_MAC_UPDATE's result).
		void Clamp(u32 v)
		{
			using S = WasmJit::Simd;
			Function& f = m_fn;
			const u32 e = VLocal(6), sgn = VLocal(7);
			f.local_get(v);
			Splat(EXP);
			SimdOp(S::V128And);
			f.local_set(e);
			f.local_get(v);
			Splat(SIGN);
			SimdOp(S::V128And);
			f.local_tee(sgn);
			Splat(0x7f7fffffu);
			SimdOp(S::V128Or);
			f.local_get(v);
			f.local_get(e);
			Splat(EXP);
			SimdOp(S::I32x4Eq);
			SimdOp(S::V128Bitselect);
			f.local_set(v);
			f.local_get(sgn);
			f.local_get(v);
			f.local_get(e);
			Splat(0);
			SimdOp(S::I32x4Eq);
			SimdOp(S::V128Bitselect);
			f.local_set(v);
		}

		// VU0.macflag and VU0.statusflag from the raw result in local r (VU_MAC_UPDATE, VU_MACx_CLEAR,
		// VU_STAT_UPDATE), then SYNCMSFLAGS().
		void Flags(u32 r, u32 dest)
		{
			using S = WasmJit::Simd;
			Function& f = m_fn;
			const u32 e = VLocal(6), zm = VLocal(7), sm = VLocal(5), n = VLocal(4);
			const bool all = (dest & 0xf) == 0xf;
			if (!m_t_ok)
			{
				m_t = f.local(ValType::I32); // not m_j: a delay slot runs between JR's target read and its use
				m_t_ok = true;
			}
			f.local_get(r);
			Splat(EXP);
			SimdOp(S::V128And);
			f.local_set(e);
			f.local_get(e);
			Splat(0);
			SimdOp(S::I32x4Eq);
			if (!all)
			{
				DestConst(dest);
				SimdOp(S::V128And);
			}
			f.local_set(zm);
			f.local_get(r);
			f.i32_const(31);
			SimdOp(S::I32x4ShrS);
			if (!all)
			{
				DestConst(dest);
				SimdOp(S::V128And);
			}
			f.local_set(sm);
			f.local_get(zm);
			f.local_get(sm);
			SimdOp(S::I16x8NarrowI32x4S);
			f.local_get(zm);
			f.local_get(r);
			Splat(0x007fffffu);
			SimdOp(S::V128And);
			Splat(0);
			SimdOp(S::I32x4Eq);
			SimdOp(S::V128AndNot);
			f.local_get(e);
			Splat(EXP);
			SimdOp(S::I32x4Eq);
			if (!all)
			{
				DestConst(dest);
				SimdOp(S::V128And);
			}
			SimdOp(S::I16x8NarrowI32x4S);
			SimdOp(S::I8x16NarrowI16x8S); // bytes: zero xyzw, sign xyzw, underflow xyzw, overflow xyzw
			f.local_set(n);
			// macflag = (macflag & ~0xffff) | mac; VI[MAC] = macflag
			f.i32_const(0);
			Ld32(Addr(&VU0.macflag));
			f.i32_const(static_cast<s32>(0xffff0000u));
			f.i32_and();
			f.local_get(n);
			f.local_get(n);
			static constexpr u8 rev[16] = {3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12};
			f.shuffle(rev);
			SimdOp(S::I8x16Bitmask);
			f.i32_or();
			f.local_tee(m_t);
			f.i32_store(Addr(&VU0.macflag));
			f.i32_const(0);
			f.local_get(m_t);
			f.i32_store(Addr(&VU0.VI[REG_MAC_FLAG].UL));
			// statusflag = one bit per group; VI[STATUS] = (VI[STATUS] & 0xfc0) | sf | (sf << 6)
			f.local_get(n);
			Splat(0);
			SimdOp(S::I32x4Ne);
			SimdOp(S::I32x4Bitmask);
			f.local_set(m_t);
			f.i32_const(0);
			f.local_get(m_t);
			f.i32_store(Addr(&VU0.statusflag));
			f.i32_const(0);
			Ld32(Addr(&VU0.VI[REG_STATUS_FLAG].UL));
			f.i32_const(0xfc0);
			f.i32_and();
			f.local_get(m_t);
			f.i32_or();
			f.local_get(m_t);
			f.i32_const(6);
			f.i32_shl();
			f.i32_or();
			f.i32_store(Addr(&VU0.VI[REG_STATUS_FLAG].UL));
		}

		void StoreVU0Code(u32 code)
		{
			m_fn.i32_const(0);
			m_fn.i32_const(static_cast<s32>(code));
			m_fn.i32_store(Addr(&VU0.code));
		}

		enum class C2 { None, Fmac, QMFC2, QMTC2, CFC2, CTC2, NOP, MOVE, MR32, ABS, ITOF, FTOI };
		enum FK { FADD, FSUB, FMUL, FMADD, FMSUB };
		struct Cop2Op
		{
			C2 type = C2::None;
			FK k = FADD;
			int bc = -1; // 0-3 lane of ft, 4 I, 5 Q, -1 vector
			bool acc = false;
			u32 sub = 0; // ITOF/FTOI: 0, 4, 12, 15
		};

		// The COP2 instructions generated code runs (with VU0 idle).
		static Cop2Op DecodeCop2(u32 code)
		{
			Cop2Op o;
			const u32 rs = Rs(code), fs = Rd(code);
			switch (rs)
			{
				case 1: o.type = C2::QMFC2; return o;
				case 5: o.type = C2::QMTC2; return o;
				case 2: o.type = C2::CFC2; return o;
				case 6:
					if (fs != REG_R && fs != REG_FBRST && fs != REG_CMSAR1)
						o.type = C2::CTC2;
					return o;
				default:
					if (rs < 16)
						return o;
					break;
			}
			const u32 fn = code & 0x3f;
			const bool sp2 = fn >= 0x3c;
			const u32 op = sp2 ? ((code & 3) | ((code >> 4) & 0x7c)) : fn;
			static constexpr u32 offs[4] = {0, 4, 12, 15};
			o.acc = sp2;
			auto fmac = [&](FK k, int bc) { o.type = C2::Fmac; o.k = k; o.bc = bc; };
			if (op < 0x10)
			{
				static constexpr FK kinds[4] = {FADD, FSUB, FMADD, FMSUB};
				fmac(kinds[op >> 2], static_cast<int>(op & 3));
				return o;
			}
			if (op >= 0x18 && op <= 0x1b)
			{
				fmac(FMUL, static_cast<int>(op & 3));
				return o;
			}
			switch (op)
			{
				case 0x1c: fmac(FMUL, 5); break;
				case 0x1e: fmac(FMUL, 4); break;
				case 0x20: fmac(FADD, 5); break;
				case 0x21: fmac(FMADD, 5); break;
				case 0x22:
					if (!sp2 && CHECK_VUADDSUBHACK)
						break; // vuADDbc_addsubhack
					fmac(FADD, 4);
					break;
				case 0x23: fmac(FMADD, 4); break;
				case 0x24: fmac(FSUB, 5); break;
				case 0x25: fmac(FMSUB, 5); break;
				case 0x26: fmac(FSUB, 4); break;
				case 0x27: fmac(FMSUB, 4); break;
				case 0x28: fmac(FADD, -1); break;
				case 0x29: fmac(FMADD, -1); break;
				case 0x2a: fmac(FMUL, -1); break;
				case 0x2c: fmac(FSUB, -1); break;
				case 0x2d: fmac(FMSUB, -1); break;
				default:
					if (!sp2)
						break;
					switch (op)
					{
						case 0x2f: o.type = C2::NOP; break;
						case 0x30: o.type = C2::MOVE; break;
						case 0x31: o.type = C2::MR32; break;
						case 0x1d: o.type = C2::ABS; break;
						case 0x10: case 0x11: case 0x12: case 0x13: o.type = C2::ITOF; o.sub = offs[op & 3]; break;
						case 0x14: case 0x15: case 0x16: case 0x17: o.type = C2::FTOI; o.sub = offs[op & 3]; break;
						default: break;
					}
					break;
			}
			// vuDouble and VU_MAC_UPDATE as generated here assume the VU0 overflow clamp.
			if (o.type == C2::Fmac && !CHECK_VU_OVERFLOW(0))
				o.type = C2::None;
			return o;
		}

		void EmitCop2Body(u32 code, const Cop2Op& o)
		{
			using S = WasmJit::Simd;
			Function& f = m_fn;
			const u32 rt = Rt(code);
			const u32 ft = rt, fs = Rd(code), fd = Sa(code), dest = (code >> 21) & 0xf;
			switch (o.type)
			{
				case C2::QMFC2:
					if (rt != 0)
					{
						f.i32_const(0);
						LdV(Addr(&VU0.VF[fs]));
						f.v128_store(GPR(rt));
					}
					return;
				case C2::QMTC2:
					if (fs != 0)
					{
						f.i32_const(0);
						LdV(GPR(rt));
						f.v128_store(Addr(&VU0.VF[fs]));
					}
					return;
				case C2::CFC2:
					if (rt == 0)
						return;
					if (fs == REG_R)
					{
						f.i32_const(0);
						Ld32(Addr(&VU0.VI[REG_R].UL));
						f.i32_const(0x7fffff);
						f.i32_and();
						f.i32_store(GPR(rt));
					}
					else
					{
						f.i32_const(0);
						f.i32_const(0);
						f.i64_load32_s(Addr(&VU0.VI[fs].UL));
						f.i64_store(GPR(rt));
					}
					return;
				case C2::CTC2:
					switch (fs)
					{
						case 0: case REG_MAC_FLAG: case REG_TPC: case REG_VPU_STAT:
							return; // read-only
						case REG_CLIP_FLAG:
							f.i32_const(0);
							LoadGPR32(rt);
							f.i32_store(Addr(&VU0.clipflag));
							break;
						default:
							break;
					}
					f.i32_const(0);
					LoadGPR32(rt);
					f.i32_store(Addr(&VU0.VI[fs].UL));
					return;
				case C2::NOP:
					StoreVU0Code(code);
					return;
				case C2::MOVE:
				case C2::MR32:
				case C2::ABS:
				case C2::ITOF:
				{
					StoreVU0Code(code);
					if (ft == 0)
						return;
					const u32 r = VLocal(1);
					LdV(Addr(&VU0.VF[fs]));
					if (o.type == C2::MR32)
					{
						static constexpr u8 rot[16] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3};
						f.shuffle1(rot, VLocal(2));
					}
					else if (o.type == C2::ABS)
					{
						Splat(0x7fffffffu);
						SimdOp(S::V128And);
					}
					else if (o.type == C2::ITOF)
					{
						SimdOp(S::F32x4ConvertI32x4S);
						if (o.sub)
						{
							Splat(0x3f800000u - (o.sub << 23));
							SimdOp(S::F32x4Mul);
						}
					}
					f.local_set(r);
					StoreMerged(Addr(&VU0.VF[ft]), r, dest);
					return;
				}
				case C2::FTOI:
				{
					StoreVU0Code(code);
					if (ft == 0)
						return;
					const u32 u = VLocal(0), r = VLocal(1);
					LdV(Addr(&VU0.VF[fs]));
					if (o.sub)
					{
						Splat(0x3f800000u + (o.sub << 23));
						SimdOp(S::F32x4Mul);
					}
					f.local_tee(u);
					Splat(0x7fffffffu);
					f.local_get(u);
					f.i32_const(31);
					SimdOp(S::I32x4ShrU);
					SimdOp(S::I32x4Add); // the saturated value by sign
					f.local_get(u);
					SimdOp(S::I32x4TruncSatF32x4S);
					f.local_get(u);
					Splat(EXP);
					SimdOp(S::V128And);
					Splat(0x4f000000u);
					SimdOp(S::I32x4GeU);
					SimdOp(S::V128Bitselect);
					f.local_set(r);
					StoreMerged(Addr(&VU0.VF[ft]), r, dest);
					return;
				}
				case C2::Fmac:
				{
					// applyBinaryMACOp / applyTernaryMACOp and their broadcasts (VUops.cpp).
					StoreVU0Code(code);
					const int bc = o.bc;
					const u32 a = VLocal(0), b = VLocal(1), c = VLocal(2), r = VLocal(3);
					LdV(Addr(&VU0.VF[fs]));
					f.local_set(a);
					Clamp(a);
					if (bc < 0)
						LdV(Addr(&VU0.VF[ft]));
					else
					{
						f.i32_const(0);
						f.simd_mem(S::V128Load32Splat,
							(bc < 4) ? Addr(&VU0.VF[ft].UL[bc]) : Addr(&VU0.VI[(bc == 4) ? REG_I : REG_Q].UL), 2);
					}
					f.local_set(b);
					Clamp(b);
					if (o.k == FMADD || o.k == FMSUB)
					{
						LdV(Addr(&VU0.ACC));
						f.local_set(c);
						Clamp(c);
						f.local_get(c);
						f.local_get(a);
						f.local_get(b);
						SimdOp(S::F32x4Mul);
						SimdOp(o.k == FMADD ? S::F32x4Add : S::F32x4Sub);
					}
					else
					{
						f.local_get(a);
						f.local_get(b);
						SimdOp(o.k == FADD ? S::F32x4Add : o.k == FSUB ? S::F32x4Sub : S::F32x4Mul);
					}
					f.local_set(r);
					Flags(r, dest);
					if (o.acc || fd != 0) // fd 0 writes RDzero, which nothing reads
					{
						Clamp(r);
						StoreMerged(o.acc ? Addr(&VU0.ACC) : Addr(&VU0.VF[fd]), r, dest);
					}
					return;
				}
				default:
					return;
			}
		}

		bool EmitCop2(u32 pc, u32 code, const Decoded& d, bool in_delay_slot)
		{
			Function& f = m_fn;
			const bool lq = d.kind == Kind::LQC2, sq = d.kind == Kind::SQC2;
			Cop2Op o;
			if (!lq && !sq)
			{
				o = DecodeCop2(code);
				if (o.type == C2::None)
					return false;
				if (EEJit::g_dbgmask & (1u << static_cast<int>(o.type)))
					return false;
			}
			else if (EEJit::g_dbgmask & 0x10000)
				return false;
			m_pending += d.op->cycles;
			s_native_instrs++;
			f.block(); // done
			f.block(); // slow
			Ld32(Addr(&VU0.VI[REG_VPU_STAT].UL));
			f.i32_const(1);
			f.i32_and();
			f.br_if(0);
			if (lq || sq)
			{
				const u32 rs = Rs(code), rt = Rt(code);
				LoadGPR32(rs);
				f.i32_const(Imm(code));
				f.i32_add();
				f.local_tee(m_a);
				f.i32_const(12);
				f.i32_shr_u();
				f.i32_const(3);
				f.i32_shl();
				f.i64_load(static_cast<u32>(s_vmap_base));
				f.local_get(m_a);
				f.i64_extend_i32_u();
				f.i64_add();
				f.local_tee(m_v);
				f.i64_const(0);
				f.i64_lt_s();
				f.br_if(0);
				if (lq)
				{
					if (rt != 0)
					{
						f.i32_const(0);
						f.local_get(m_v);
						f.i32_wrap_i64();
						f.v128_load(0);
						f.v128_store(Addr(&VU0.VF[rt]));
					}
				}
				else
				{
					f.local_get(m_v);
					f.i32_wrap_i64();
					LdV(Addr(&VU0.VF[rt]));
					f.v128_store(0);
					EmitNoteWrite();
				}
			}
			else
				EmitCop2Body(code, o);
			f.br(1);
			f.end(); // slow
			StorePcCode(pc, code);
			FlushCyclesSlowPath();
			CallVoid(FnIndex(d.op->interpret));
			if (!in_delay_slot)
				CheckPc(pc + 4);
			f.end(); // done
			if (m_pending || m_cyc_dirty)
				m_cyc_dirty = true;
			return true;
		}

		// LDL/LDR/SDL/SDR on the aligned doubleword at the host address in m_v, for the guest address in m_a.
		void EmitUnaligned64(Kind kind, u32 rt)
		{
			Function& f = m_fn;
			auto host = [&] { f.local_get(m_v); f.i32_wrap_i64(); };
			// m_k: LDL_SHIFT/SDL_SHIFT[addr & 7] (56 - 8 * (addr & 7)) or LDR_SHIFT/SDR_SHIFT (8 * (addr & 7)). The
			// masks are LDL_MASK = SDR_MASK = ~(~0 << m_k) and LDR_MASK = SDL_MASK = ~(~0 >> m_k).
			const bool left = kind == Kind::LDL || kind == Kind::SDL;
			f.local_get(m_a);
			if (left)
			{
				f.i32_const(7);
				f.i32_xor();
			}
			f.i32_const(7);
			f.i32_and();
			f.i32_const(3);
			f.i32_shl();
			f.i64_extend_i32_u();
			f.local_set(m_k);
			auto mask = [&](bool shl) {
				f.i64_const(-1);
				f.local_get(m_k);
				if (shl)
					f.i64_shl();
				else
					f.i64_shr_u();
				f.i64_const(-1);
				f.i64_xor();
			};
			auto mem = [&] {
				host();
				f.i64_load(0);
			};
			switch (kind)
			{
				case Kind::LDL: // rt = (rt & LDL_MASK) | (mem << LDL_SHIFT)
				case Kind::LDR: // rt = (rt & LDR_MASK) | (mem >> LDR_SHIFT)
					f.i32_const(0);
					LoadGPR64(rt);
					mask(kind == Kind::LDL);
					f.i64_and();
					mem();
					f.local_get(m_k);
					if (kind == Kind::LDL)
						f.i64_shl();
					else
						f.i64_shr_u();
					f.i64_or();
					f.i64_store(GPR(rt));
					break;
				case Kind::SDL: // mem = (rt >> SDL_SHIFT) | (mem & SDL_MASK)
				case Kind::SDR: // mem = (rt << SDR_SHIFT) | (mem & SDR_MASK)
					host();
					LoadGPR64(rt);
					f.local_get(m_k);
					if (kind == Kind::SDL)
						f.i64_shr_u();
					else
						f.i64_shl();
					mem();
					mask(kind == Kind::SDR);
					f.i64_and();
					f.i64_or();
					f.i64_store(0);
					break;
				default:
					break;
			}
		}

		// An aligned access to directly mapped memory inline, and the unaligned LDL/LDR/SDL/SDR (the aligned
		// doubleword they read or modify, with R5900OpcodeImpl.cpp's masks and shifts); anything else calls the
		// interpreter's function, which redoes the whole instruction (nothing has been written yet).
		void EmitMemory(u32 pc, u32 code, const Decoded& d, bool in_delay_slot)
		{
			Function& f = m_fn;
			const u32 rs = Rs(code), rt = Rt(code);
			m_pending += d.op->cycles;
			s_native_instrs++;

			u32 align_mask = 0;
			bool quad = false;
			const bool unaligned = d.kind == Kind::LDL || d.kind == Kind::LDR || d.kind == Kind::SDL || d.kind == Kind::SDR;
			switch (d.kind)
			{
				case Kind::LH: case Kind::LHU: case Kind::SH: align_mask = 1; break;
				case Kind::LW: case Kind::LWU: case Kind::SW: case Kind::LWC1: case Kind::SWC1: align_mask = 3; break;
				case Kind::LD: case Kind::SD: align_mask = 7; break;
				case Kind::LQ: case Kind::SQ: quad = true; break;
				default: break;
			}

			f.block(); // done
			f.block(); // slow
			f.i32_const(0);
			f.i32_load(GPR(rs));
			f.i32_const(Imm(code));
			f.i32_add();
			if (quad)
			{
				f.i32_const(~0xf);
				f.i32_and();
			}
			f.local_tee(m_a);
			if (align_mask)
			{
				f.i32_const(align_mask);
				f.i32_and();
				f.br_if(0);
				f.local_get(m_a);
			}
			f.i32_const(12);
			f.i32_shr_u();
			f.i32_const(3);
			f.i32_shl();
			f.i64_load(static_cast<u32>(s_vmap_base));
			f.local_get(m_a);
			if (unaligned)
			{
				f.i32_const(~7);
				f.i32_and();
			}
			f.i64_extend_i32_u();
			f.i64_add();
			f.local_tee(m_v);
			f.i64_const(0);
			f.i64_lt_s();
			f.br_if(0);

			auto host = [&] { f.local_get(m_v); f.i32_wrap_i64(); };
			switch (d.kind)
			{
				case Kind::LB: f.i32_const(0); host(); f.i32_load8_s(0); f.i64_extend_i32_s(); f.i64_store(GPR(rt)); break;
				case Kind::LBU: f.i32_const(0); host(); f.i64_load8_u(0); f.i64_store(GPR(rt)); break;
				case Kind::LH: f.i32_const(0); host(); f.i32_load16_s(0); f.i64_extend_i32_s(); f.i64_store(GPR(rt)); break;
				case Kind::LHU: f.i32_const(0); host(); f.i32_load16_u(0); f.i64_extend_i32_u(); f.i64_store(GPR(rt)); break;
				case Kind::LW: f.i32_const(0); host(); f.i64_load32_s(0); f.i64_store(GPR(rt)); break;
				case Kind::LWU: f.i32_const(0); host(); f.i64_load32_u(0); f.i64_store(GPR(rt)); break;
				case Kind::LD: f.i32_const(0); host(); f.i64_load(0); f.i64_store(GPR(rt)); break;
				case Kind::LQ: f.i32_const(0); host(); f.v128_load(0); f.v128_store(GPR(rt)); break;
				case Kind::LWC1: f.i32_const(0); host(); f.i32_load(0); f.i32_store(Addr(&fpuRegs.fpr[rt].UL)); break;
				case Kind::SB: host(); LoadGPR32(rt); f.i32_store8(0); break;
				case Kind::SH: host(); LoadGPR32(rt); f.i32_store16(0); break;
				case Kind::SW: host(); LoadGPR32(rt); f.i32_store(0); break;
				case Kind::SD: host(); LoadGPR64(rt); f.i64_store(0); break;
				case Kind::SQ: host(); f.i32_const(0); f.v128_load(GPR(rt)); f.v128_store(0); break;
				case Kind::SWC1: host(); f.i32_const(0); f.i32_load(Addr(&fpuRegs.fpr[rt].UL)); f.i32_store(0); break;
				case Kind::LDL: case Kind::LDR: case Kind::SDL: case Kind::SDR: EmitUnaligned64(d.kind, rt); break;
				default: break;
			}
			if ((d.kind >= Kind::SB && d.kind <= Kind::SQ) || d.kind == Kind::SWC1 || d.kind == Kind::SDL ||
				d.kind == Kind::SDR)
				EmitNoteWrite();
			f.br(1);
			f.end(); // slow
			StorePcCode(pc, code);
			FlushCyclesSlowPath();
			CallVoid(FnIndex(d.op->interpret));
			if (!in_delay_slot)
				CheckPc(pc + 4);
			f.end(); // done
			if (m_pending || m_cyc_dirty)
				m_cyc_dirty = true;
		}

		// The delay slot of a taken branch: cpuRegs.branch is set, and PC and code are those execI() leaves.
		void EmitDelaySlot(u32 pc)
		{
			const u32 code = Word(pc);
			const Decoded d = Decode(code);
			s_guest_instrs++;
			StorePcCode(pc, code);
			m_fn.i32_const(0);
			m_fn.i32_const(1);
			m_fn.i32_store(Addr(&cpuRegs.branch));
			EmitInstruction(pc, code, d, true);
		}

		// intBranchTest() where the interpreter runs it (EmitBranchTestFunction()).
		void EmitBranchTest() { m_fn.call(kFnBranchTest); }

		// A taken branch after its delay slot, with cpuBlockCycles written: intJitBranchEnd(target)
		// (EmitBranchEndFunction()). `target` is the branch target, or kAnyPc for m_j's.
		void EmitBranchEnd(u32 target)
		{
			if (target != kAnyPc)
				m_fn.i32_const(static_cast<s32>(target));
			else
				m_fn.local_get(m_j);
			m_fn.call(kFnBranchEnd);
		}

		// Emits the branch at pc. Returns true if the block goes on (at m_next_pc) after the branch not taken.
		bool EmitBranch(u32 pc, u32 code, const Decoded& d, std::vector<u32>* next)
		{
			Function& f = m_fn;
			const u32 rs = Rs(code), rt = Rt(code), rd = Rd(code);
			const u32 btarget = pc + 4 + (Imm(code) << 2);
			const u32 jtarget = ((code & 0x03ffffff) << 2) | ((pc + 4) & 0xf0000000);
			m_pending += d.op->cycles;
			s_native_instrs++;

			auto in_page = [&](u32 a) { return a >= m_page && a < m_page + 4096 && !(a & 3); };
			auto link = [&](u32 r) {
				f.i32_const(0);
				f.i64_const(static_cast<s64>(static_cast<u64>(pc + 8)));
				f.i64_store(GPR(r));
			};
			// Taken: delay slot, then the interpreter's branch end, then leave. `target`: btarget, jtarget (also in
			// m_j), or kAnyPc for m_j's.
			auto taken = [&](u32 target) {
				const u32 saved_pending = m_pending;
				const bool saved_dirty = m_cyc_dirty;
				EmitDelaySlot(pc + 4);
				FlushCycles();
				EmitBranchEnd(target);
				Leave(target, false);
				m_pending = saved_pending;
				m_cyc_dirty = saved_dirty;
			};

			switch (d.kind)
			{
				case Kind::J:
				case Kind::JAL:
					if (d.kind == Kind::JAL)
						link(31);
					f.i32_const(static_cast<s32>(jtarget));
					f.local_set(m_j);
					taken(jtarget);
					if (in_page(jtarget))
						next->push_back(jtarget);
					if (in_page(pc + 8))
						next->push_back(pc + 8);
					return false;
				case Kind::JR:
				case Kind::JALR:
					LoadGPR32(rs);
					f.local_set(m_j);
					if (d.kind == Kind::JALR && rd)
						link(rd);
					taken(kAnyPc);
					if (in_page(pc + 8))
						next->push_back(pc + 8);
					return false;
				default:
					break;
			}

			const bool likely = d.kind == Kind::BEQL || d.kind == Kind::BNEL || d.kind == Kind::BLEZL ||
								d.kind == Kind::BGTZL || d.kind == Kind::BLTZL || d.kind == Kind::BGEZL ||
								d.kind == Kind::BLTZALL || d.kind == Kind::BGEZALL || d.kind == Kind::BC1FL ||
								d.kind == Kind::BC1TL;
			const bool fpu = d.kind == Kind::BC1F || d.kind == Kind::BC1T || d.kind == Kind::BC1FL || d.kind == Kind::BC1TL;
			const bool links = d.kind == Kind::BLTZAL || d.kind == Kind::BGEZAL || d.kind == Kind::BLTZALL ||
							   d.kind == Kind::BGEZALL;
			if (links)
				link(31);

			// Condition (64-bit signed compares, as the interpreter does; BC1x: FCR31's C bit).
			if (fpu)
			{
				f.i32_const(0);
				f.i32_load(Addr(&fpuRegs.fprc[31]));
				f.i32_const(0x00800000);
				f.i32_and();
				if (d.kind == Kind::BC1F || d.kind == Kind::BC1FL)
					f.i32_eqz();
			}
			else
			{
			LoadGPR64(rs);
			switch (d.kind)
			{
				case Kind::BEQ: case Kind::BEQL: LoadGPR64(rt); f.i64_eq(); break;
				case Kind::BNE: case Kind::BNEL: LoadGPR64(rt); f.i64_ne(); break;
				case Kind::BLEZ: case Kind::BLEZL: f.i64_const(0); f.i64_le_s(); break;
				case Kind::BGTZ: case Kind::BGTZL: f.i64_const(0); f.i64_gt_s(); break;
				case Kind::BLTZ: case Kind::BLTZL: case Kind::BLTZAL: case Kind::BLTZALL: f.i64_const(0); f.i64_lt_s(); break;
				default: f.i64_const(0); f.i64_ge_s(); break;
			}
			}
			f.if_();
			taken(btarget);
			f.end();
			if (in_page(btarget))
				next->push_back(btarget);

			// BC1FL/BC1TL not taken skip the delay slot without the branch test (FPU.cpp, BC1L).
			const bool tests = (likely && !fpu) || d.kind == Kind::BEQ || d.kind == Kind::BNE;
			const u32 resume = likely ? pc + 8 : pc + 4;
			if (tests)
			{
				// The interpreter's PC is pc + 4 here, or pc + 8 once a likely branch skipped its delay slot.
				f.i32_const(0);
				f.i32_const(static_cast<s32>(resume));
				f.i32_store(Addr(&cpuRegs.pc));
				f.i32_const(0);
				f.i32_const(static_cast<s32>(code));
				f.i32_store(Addr(&cpuRegs.code));
				FlushCycles();
				EmitBranchTest();
				CheckPc(resume);
			}
			m_next_pc = resume;
			return true;
		}

	public:
		// Compiles the block at `start` into this compiler's function, and adds to `next` the PCs worth compiling
		// with it (branch targets and continuations in the same page).
		Function& Run(u32 start, std::vector<u32>* next)
		{
			return CompileLoop(start, next);
		}
		Function& Body() { return m_fn; }
		std::vector<std::pair<size_t, u32>> m_links; // tail calls to patch: body offset, target PC

	private:
		Function& CompileLoop(u32 start, std::vector<u32>* next)
		{
			Function& f = m_fn;
			const u32 end = Scan(start);
			EmitEntryChecks(start, end);
			RefreshFactor();

			u32 pc = start;
			u32 count = 0;
			for (;;)
			{
				if (pc >= m_page + 4096 || count >= kMaxBlockInstrs)
				{
					if (pc < m_page + 4096)
						next->push_back(pc);
					ExitTo(pc);
					break;
				}
				const u32 code = Word(pc);
				const Decoded d = Decode(code);
				count++;
				s_guest_instrs++;

				if (IsNativeBranch(d.kind))
				{
					const bool ds_ok = (pc + 4 < m_page + 4096) && !IsBranch(Decode(Word(pc + 4)));
					if (!ds_ok)
					{
						EmitFallback(pc, code, d, false);
						Leave(kAnyPc, false);
						break;
					}
					if (!EmitBranch(pc, code, d, next))
						break;
					pc = m_next_pc;
					continue;
				}
				if (IsBranch(d))
				{
					EmitFallback(pc, code, d, false);
					Leave(kAnyPc, false);
					if (pc + 8 < m_page + 4096)
						next->push_back(pc + 8);
					break;
				}
				EmitInstruction(pc, code, d, false);
				pc += 4;
			}
			f.i32_const(0);
			return f;
		}

		u32 m_tvv, m_tiv;
		u32 m_page;
		const u32* m_host;
		u64 m_vmap_raw;
		u32* m_checked; // the page generation this block's code was last compared at
		Function m_fn;
		u32 m_a, m_j, m_cyc, m_f;
		u32 m_vloc[8] = {};
		u32 m_t = 0;
		u32 m_iloc[4] = {};
		bool m_iloc_ok[4] = {};
		bool m_t_ok = false;
		bool m_vloc_ok[8] = {};
		u32 m_v, m_k;
		u32 m_pending = 0; // cycles not yet written, before the Config factor
		bool m_cyc_dirty = false; // m_cyc may be non-zero
	};

	// ------------------------------------------------------------------------------------------------------------
	// Cache

	int& SlotRef(u32 pc)
	{
		int*& page = s_pages[pc >> 12];
		if (!page)
			page = new int[kPageWords]();
		return page[(pc >> 2) & (kPageWords - 1)];
	}

	void ReleaseSlot(int slot)
	{
		auto it = s_slot_group.find(slot);
		if (it == s_slot_group.end())
			return;
		Group* g = it->second;
		s_slot_group.erase(it);
		wasmjit_remove_function(slot);
		if (--g->live == 0)
		{
			wasmjit_release(g->first.instance);
			wasmjit_release(g->first.module);
			g->first = {};
			auto git = std::find_if(s_groups.begin(), s_groups.end(), [g](const auto& p) { return p.get() == g; });
			if (git != s_groups.end())
				s_groups.erase(git);
		}
	}

	void FlushAll()
	{
		for (int*& page : s_pages)
		{
			if (!page)
				continue;
			delete[] page;
			page = nullptr;
		}
		for (auto& [slot, g] : s_slot_group)
			wasmjit_remove_function(slot);
		s_slot_group.clear();
		for (auto& g : s_groups)
		{
			if (g->first.instance)
				wasmjit_release(g->first.instance);
			if (g->first.module)
				wasmjit_release(g->first.module);
		}
		s_groups.clear();
		s_flushes++;
	}

	// intUpdateCPUCycles() with an EE cycle rate of 0: cpuRegs.cycle += max(cpuBlockCycles >> 3, 1), and
	// cpuBlockCycles keeps its low 3 bits. `tmp`: an i32 local.
	void EmitUpdateCycles(Function& f, u32 tmp)
	{
		const u32 bc = Addr(intJitBlockCycles());
		f.i32_const(0);
		f.i32_const(0);
		f.i64_load(Addr(&cpuRegs.cycle));
		f.i32_const(0);
		f.i32_load(bc);
		f.i32_const(3);
		f.i32_shr_u();
		f.local_tee(tmp);
		f.i32_const(1);
		f.local_get(tmp);
		f.select();
		f.i64_extend_i32_u();
		f.i64_add();
		f.i64_store(Addr(&cpuRegs.cycle));
		f.i32_const(0);
		f.i32_const(0);
		f.i32_load(bc);
		f.i32_const(7);
		f.i32_and();
		f.i32_store(bc);
	}

	// intBranchTest(), as intJitBranchTest(): called only when it has something to do (the trace, an event due, or
	// the BIOS hooks). () -> ().
	Function EmitBranchTestFunction(u32 type_v_v)
	{
		Function f(0);
		f.i32_const(0);
		f.i32_load8_u(Addr(&EEJit::g_trace));
		f.i32_const(0);
		f.i32_load8_u(Addr(intJitBiosHooksActive()));
		f.i32_or();
		f.i32_const(0);
		f.i64_load(Addr(&cpuRegs.cycle));
		f.i32_const(0);
		f.i64_load(Addr(&cpuRegs.nextEventCycle));
		f.i64_sub();
		f.i64_const(0);
		f.i64_ge_s();
		f.i32_or();
		f.if_();
		f.i32_const(static_cast<s32>(FnIndex(&intJitBranchTest)));
		f.call_indirect(type_v_v, 0);
		f.end();
		return f;
	}

	u32 WaitLoopBit()
	{
		static const u32 bit = [] {
			Pcsx2Config::SpeedhackOptions o;
			o.bitset = 0;
			o.WaitLoop = true;
			return o.bitset;
		}();
		return bit;
	}

	// intJitBranchEnd(target) after a taken branch's delay slot: its usual case here, the same steps in the same
	// order (_doBranch_afterDelaySlot(), intUpdateCPUCycles(), intBranchTest()), and intJitBranchEnd() itself when
	// the delay slot took an exception (cpuRegs.branch is 0), with an EE cycle rate other than 0, with the BIOS hooks
	// active, or on a wait loop (the same target as the last branch, with the WaitLoop speedhack) that may be
	// skipped: the BIOS idle loop at 0x81fc0, a branch to the word after its delay slot, or a loop of up to 10
	// words whose first one has no physical pointer (PSM(target) == 0, vtlb_GetPhyPtr()). The JIT only runs from
	// intExecute(), so Cpu is the interpreter there. (target) -> ().
	Function EmitBranchEndFunction(u32 type_v_v, u32 type_i_v)
	{
		Function f(1);
		const u32 target = 0;
		const u32 t = f.local(ValType::I32);
		const u32 v = f.local(ValType::I64);
		f.block(); // slow
		f.i32_const(0);
		f.i32_load(Addr(&cpuRegs.branch));
		f.i32_eqz();
		f.i32_const(0);
		f.i32_load8_s(Addr(&EmuConfig.Speedhacks.EECycleRate));
		f.i32_or();
		f.i32_const(0);
		f.i32_load8_u(Addr(intJitBiosHooksActive()));
		f.i32_or();
		f.br_if(0);
		// The wait loop test: intLastBranchTo == target && WaitLoop.
		f.i32_const(0);
		f.i32_load(Addr(intJitLastBranchTo()));
		f.local_get(target);
		f.i32_eq();
		f.i32_const(0);
		f.i32_load(Addr(&EmuConfig.Speedhacks.bitset));
		f.i32_const(static_cast<s32>(WaitLoopBit()));
		f.i32_and();
		f.i32_const(0);
		f.i32_ne();
		f.i32_and();
		f.if_();
		{
			f.local_get(target);
			f.i32_const(0x81fc0);
			f.i32_eq();
			f.br_if(1);
			// t = cpuRegs.pc - target (cpuRegs.pc: after the delay slot)
			f.i32_const(0);
			f.i32_load(Addr(&cpuRegs.pc));
			f.local_get(target);
			f.i32_sub();
			f.local_tee(t);
			f.i32_eqz();
			f.br_if(1);
			f.local_get(t);
			f.i32_const(4 * 10);
			f.i32_lt_u();
			f.if_();
			{
				// PSM(target) == 0: a handler, or a null pointer. (target & 0x1fffffff) < VTLB_PMAP_SZ.
				static_assert(VTLB_PMAP_SZ == 0x20000000);
				f.local_get(target);
				f.i32_const(0x1fffffff);
				f.i32_and();
				f.local_tee(t);
				f.i32_const(VTLB_PAGE_BITS);
				f.i32_shr_u();
				f.i32_const(3);
				f.i32_shl();
				f.i64_load(Addr(&vtlbdata.pmap[0]));
				f.local_tee(v);
				f.i64_const(0);
				f.i64_lt_s();
				f.local_get(v);
				f.i32_wrap_i64();
				f.local_get(t);
				f.i32_const(VTLB_PAGE_MASK);
				f.i32_and();
				f.i32_add();
				f.i32_eqz();
				f.i32_or();
				f.br_if(2);
			}
			f.end();
			// Not skipped: _doBranch_afterDelaySlot() still updates the cycles.
			EmitUpdateCycles(f, t);
		}
		f.end();
		// intLastBranchTo = cpuRegs.pc = target; cpuRegs.branch = 0.
		f.i32_const(0);
		f.local_get(target);
		f.i32_store(Addr(intJitLastBranchTo()));
		f.i32_const(0);
		f.local_get(target);
		f.i32_store(Addr(&cpuRegs.pc));
		f.i32_const(0);
		f.i32_const(0);
		f.i32_store(Addr(&cpuRegs.branch));
		EmitUpdateCycles(f, t);
		f.call(kFnBranchTest);
		f.return_();
		f.end(); // slow
		f.local_get(target);
		f.i32_const(static_cast<s32>(FnIndex(&intJitBranchEnd)));
		f.call_indirect(type_i_v, 0);
		return f;
	}

	// The dispatcher's lookup in wasm (Execute()), for linked exits: tail-calls the compiled block at cpuRegs.pc,
	// or returns 0 to the dispatcher when cpuBlockCycles calls for its event test or there is no block.
	Function EmitLookup(u32 type_v_i)
	{
		Function f(0);
		const u32 pc = f.local(ValType::I32), p = f.local(ValType::I32);
		auto ret0_if = [&] {
			f.if_();
			f.i32_const(0);
			f.return_();
			f.end();
		};
		f.i32_const(0);
		f.i32_load(Addr(intJitBlockCycles()));
		f.i32_const(1 << 20);
		f.i32_ge_u();
		ret0_if();
		f.i32_const(0);
		f.i32_load(Addr(&cpuRegs.pc));
		f.local_tee(pc);
		f.i32_const(3);
		f.i32_and();
		ret0_if();
		f.local_get(pc);
		f.i32_const(12);
		f.i32_shr_u();
		f.i32_const(2);
		f.i32_shl();
		f.i32_load(Addr(&s_pages[0]));
		f.local_tee(p);
		f.i32_eqz();
		ret0_if();
		f.local_get(p);
		f.local_get(pc);
		f.i32_const(0xffc);
		f.i32_and();
		f.i32_add();
		f.i32_load(0);
		f.local_tee(p);
		f.i32_eqz();
		ret0_if();
		f.local_get(p);
		f.return_call_indirect(type_v_i, 0);
		return f;
	}

	// Compiles the block at pc and its neighbours in the same page into one module. False if pc cannot be compiled.
	bool CompileGroup(u32 entry)
	{
		const u32 page = entry & ~4095u;
		const VTLBVirtual vmv = vtlbdata.vmap[entry >> VTLB_PAGE_BITS];
		if (vmv.isHandler(entry))
			return false;
		const u32* host = reinterpret_cast<const u32*>(vmv.assumePtr(page));

		Common::Timer timer;
		WasmJit::Module m;
		const u32 t_v_v = m.type({}, {});
		const u32 t_i_v = m.type({ValType::I32}, {});
		const u32 t_v_i = m.type({}, {ValType::I32});
		m.import_memory("env", "mem", 0, 65536, true);
		m.import_table("env", "table", 0);

		std::unique_ptr<u32[]> checked = std::make_unique<u32[]>(kMaxGroupBlocks);
		std::vector<u32> pcs;
		std::vector<std::unique_ptr<BlockCompiler>> blocks;
		std::vector<u32> work{entry};
		while (!work.empty() && pcs.size() < kMaxGroupBlocks)
		{
			const u32 pc = work.front();
			work.erase(work.begin());
			if (std::find(pcs.begin(), pcs.end(), pc) != pcs.end())
				continue;
			if (pc != entry && SlotRef(pc) != 0)
				continue;
			std::vector<u32> next;
			blocks.push_back(std::make_unique<BlockCompiler>(t_v_v, t_i_v, page, host, vmv.raw(), &checked[pcs.size()]));
			blocks.back()->Run(pc, &next);
			pcs.push_back(pc);
			for (u32 n : next)
				work.push_back(n);
		}
		// Block i is function kFnFirstBlock + i; the next one returns 0 (links to blocks outside the module).
		const u32 nblocks = static_cast<u32>(pcs.size());
		m.add_function(t_i_v, EmitBranchEndFunction(t_v_v, t_i_v));
		m.add_function(t_v_v, EmitBranchTestFunction(t_v_v));
		for (u32 i = 0; i < nblocks; i++)
		{
			Function& fn = blocks[i]->Body();
			for (const auto& [at, target] : blocks[i]->m_links)
			{
				const auto it = std::find(pcs.begin(), pcs.end(), target);
				// kFnFirstBlock + nblocks (the lookup) when not found
				fn.patch_func(at, kFnFirstBlock + static_cast<u32>(it - pcs.begin()));
			}
			const u32 index = m.add_function(t_v_i, fn);
			char name[16];
			std::snprintf(name, sizeof(name), "b%u", i);
			m.export_function(name, index);
		}
		m.add_function(t_v_i, EmitLookup(t_v_i));
		blocks.clear();

		const std::vector<u8> bytes = m.finish();
		s_wasm_bytes += bytes.size();
		auto g = std::make_unique<Group>();
		g->checked = std::move(checked);
		const int module = wasmjit_compile(bytes.data(), bytes.size());
		const int instance = module > 0 ? wasmjit_instantiate(module) : 0;
		if (module <= 0 || instance <= 0)
		{
			if (module > 0)
				wasmjit_release(module);
			if (module < 0 || instance < 0)
			{
				// Out of executable memory: start again with an empty cache.
				FlushAll();
				return false;
			}
			Console.Error("EE JIT: generated code for %08x did not compile; using the interpreter.", entry);
			if (FILE* fp = std::fopen("/tmp/eejit-bad.wasm", "wb"))
			{
				std::fwrite(bytes.data(), 1, bytes.size(), fp);
				std::fclose(fp);
			}
			s_failed = true;
			return false;
		}
		g->first.module = module;
		g->first.instance = instance;
		for (u32 i = 0; i < pcs.size(); i++)
		{
			char name[16];
			std::snprintf(name, sizeof(name), "b%u", i);
			const int slot = wasmjit_add_function(instance, name);
			if (!slot)
				continue;
			int& ref = SlotRef(pcs[i]);
			if (ref)
				ReleaseSlot(ref);
			ref = slot;
			g->slots.push_back(slot);
			g->live++;
			s_slot_group.emplace(slot, g.get());
		}
		if (g->live == 0)
		{
			wasmjit_release(instance);
			wasmjit_release(module);
			return false;
		}
		s_blocks_compiled += pcs.size();
		s_groups_compiled++;
		s_groups.push_back(std::move(g));
		s_compile_ns += static_cast<u64>(timer.GetTimeNanoseconds());
		return true;
	}

	u64 s_trace_step = 0, s_trace_hash = 0xcbf29ce484222325ull;

	void HashWords(u64& h, const void* p, size_t n)
	{
		const u8* b = static_cast<const u8*>(p);
		for (size_t i = 0; i + 8 <= n; i += 8)
		{
			u64 w;
			std::memcpy(&w, b + i, 8);
			h = (h ^ w) * 0x100000001b3ull;
			h ^= h >> 29;
		}
	}
} // namespace

void EEJit::RequestFlush()
{
	s_flush_requested = true;
}

void EEJit::Execute()
{
	if (s_failed)
		return;
	if (s_flush_requested || s_groups.size() > kMaxGroups || s_vmap_base != reinterpret_cast<uptr>(vtlbdata.vmap))
	{
		FlushAll();
		s_flush_requested = false;
		s_vmap_base = reinterpret_cast<uptr>(vtlbdata.vmap);
	}

	static const bool can_link = [] {
		// Tail calls (return_call): in every current engine, but check.
		WasmJit::Module m;
		const u32 t = m.type({}, {ValType::I32});
		Function fn(0);
		fn.patch_func(fn.return_call_patchable(), 0);
		m.add_function(t, fn);
		const std::vector<u8> bytes = m.finish();
		return wasmjit_validate(bytes.data(), bytes.size()) != 0;
	}();
	s_link = can_link && g_link;

	u32* const block_cycles = intJitBlockCycles();
	for (;;)
	{
		const u32 pc = cpuRegs.pc;
		int* const page = s_pages[pc >> 12];
		int slot = page ? page[(pc >> 2) & (kPageWords - 1)] : 0;
		if (slot)
		{
			if (reinterpret_cast<BlockFn>(static_cast<uptr>(slot))() == 0) [[likely]]
			{
				if (*block_cycles >= (1u << 20)) [[unlikely]]
					intJitStraightLineTest();
				continue;
			}
			// The code or its mapping changed. A linked block that failed its checks leaves the PC at its own
			// start: the dispatcher then calls it again through the cache and drops it below.
			if (cpuRegs.pc != pc)
				continue;
			s_invalidations++;
			ReleaseSlot(slot);
			page[(pc >> 2) & (kPageWords - 1)] = 0;
		}
		if (s_groups.size() > kMaxGroups)
			FlushAll();
		if ((pc & 3) || !CompileGroup(pc))
		{
			if (s_failed)
				return;
			s_interp_steps++;
			intJitExecOne();
		}
	}
}

void EEJit::TraceStep()
{
	u64 h = s_trace_hash;
	HashWords(h, &cpuRegs, offsetof(cpuRegisters, code) + 4); // GPRs, HI/LO, COP0, sa, pc, code
	HashWords(h, &cpuRegs.cycle, 8);
	HashWords(h, &fpuRegs, sizeof(fpuRegs));
	HashWords(h, &VU0.VF[0], sizeof(VU0.VF) + sizeof(VU0.VI) + sizeof(VU0.ACC)); // VF, VI, ACC (VU.h)
	HashWords(h, &VU0.macflag, 12); // macflag, statusflag, clipflag
	HashWords(h, &psxRegs, offsetof(psxRegisters, interrupt)); // IOP: GPRs, HI/LO, COP0, GTE, pc, code, cycle
	s_trace_hash = h;
	const u64 step = ++s_trace_step;
	if (step >= g_trace_from && step <= g_trace_to)
	{
		std::printf("EETRACE %llu pc=%08x cyc=%llu h=%016llx", static_cast<unsigned long long>(step), cpuRegs.pc,
			static_cast<unsigned long long>(cpuRegs.cycle), static_cast<unsigned long long>(h));
		for (int i = 0; i < 32; i++)
			std::printf(" %llx", static_cast<unsigned long long>(cpuRegs.GPR.r[i].UD[0]));
		std::printf(" hi=%llx lo=%llx\n", static_cast<unsigned long long>(cpuRegs.HI.UD[0]),
			static_cast<unsigned long long>(cpuRegs.LO.UD[0]));
	}
	else if ((step & ((1ull << g_trace_shift) - 1)) == 0)
	{
		std::printf("EETRACE %llu pc=%08x cyc=%llu h=%016llx\n", static_cast<unsigned long long>(step), cpuRegs.pc,
			static_cast<unsigned long long>(cpuRegs.cycle), static_cast<unsigned long long>(h));
	}
}

void EEJit::TraceFinish()
{
	if (g_trace)
		std::printf("EETRACE end %llu h=%016llx\n", static_cast<unsigned long long>(s_trace_step),
			static_cast<unsigned long long>(s_trace_hash));
}

void EEJit::PrintStats()
{
	Console.WriteLn("EE JIT: %s, %llu blocks in %llu modules (%llu KB of wasm, %.1f ms to build and compile), "
					"%llu invalidations, %llu flushes, %llu interpreted steps; %llu of %llu instructions native",
		g_mode ? "on" : "off", static_cast<unsigned long long>(s_blocks_compiled),
		static_cast<unsigned long long>(s_groups_compiled), static_cast<unsigned long long>(s_wasm_bytes / 1024),
		s_compile_ns / 1e6, static_cast<unsigned long long>(s_invalidations), static_cast<unsigned long long>(s_flushes),
		static_cast<unsigned long long>(s_interp_steps), static_cast<unsigned long long>(s_native_instrs),
		static_cast<unsigned long long>(s_guest_instrs));
	if (!g_stats)
		return;
	std::vector<std::pair<u64, std::string>> v;
	for (size_t i = 0; i < s_stat_names.size(); i++)
		v.emplace_back(s_stat_counts[i], s_stat_names[i]);
	std::sort(v.rbegin(), v.rend());
	u64 total = 0;
	for (auto& [c, n] : v)
		total += c;
	Console.WriteLn("EE JIT: %llu interpreter calls", static_cast<unsigned long long>(total));
	for (size_t i = 0; i < std::min<size_t>(v.size(), 40); i++)
		Console.WriteLn("  %-12s %12llu", v[i].second.c_str(), static_cast<unsigned long long>(v[i].first));
}
