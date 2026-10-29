// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// EE (R5900) JIT for WebAssembly.
//
// Guest code is compiled block by block into WebAssembly functions, several blocks of one 4 KB page per module,
// compiled and instantiated synchronously on the EE thread and called through its function table
// (WasmJitRuntime.h). A block is a superblock: it runs from its entry to an unconditional jump, a branch the JIT
// leaves to the interpreter, the end of the page or a length cap; taken conditional branches leave it. New code runs
// on the interpreter until it is hot: JitHeat.h says when a block is compiled and what goes into its module.
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
//   DIV but not SQRT, RSQRT) are emitted; everything else (COP0, other FPU and COP2 ops, MMI, divides, other unaligned
//   and I/O accesses, traps, syscalls) calls the interpreter's function for that opcode with cpuRegs.pc and cpuRegs.code set as execI() sets
//   them. After such a call, a changed PC (an exception) leaves the block.
// - Dispatch: a block that ends goes on to the next one itself (Leave()): a tail call to the target's function when
//   it is in the same module, else a lookup of the dispatcher's cache in wasm, so that the C++ dispatcher (Execute())
//   only runs for compiles, invalidations and the straight-line event test. -eejit-link 0 turns that off.
// - Registers: the low doublewords of the GPRs, HI and LO that a block uses live in wasm locals (RegCache.h), loaded
//   on first use and written back to cpuRegs before every call into C++ (interpreter functions, slow paths of
//   memory accesses, which may raise an exception or longjmp out, the branch end and event test) and at every exit;
//   after a call, the locals of the registers it may write are dropped. 128-bit accesses go through cpuRegs.
// - Self-modifying code: each block checks, on entry, its page's vtlb mapping (TLB changes) and, after a write to
//   its page (write tracking, CodeWrites.h), its instruction words against the values it was compiled from, and is
//   recompiled when they differ. Code that changes the rest of its own block while it runs is not detected (the
//   interpreter would see the new instruction); needs proper testing with games that do that.

#include "Common.h"
#include "Counters.h"
#include "wasm/PerfCounters.h"
#include "R5900OpcodeTables.h"
#include "vtlb.h"
#include "VUmicro.h"
#include "R3000A.h"

#include "wasm/CodeWrites.h"
#include "wasm/EEJit.h"
#include "wasm/JitHeat.h"
#include "wasm/RegCache.h"
#include "wasm/WasmEmitter.h"
#include "wasm/WasmJitRuntime.h"

#include "common/Console.h"
#include "common/Threading.h"
#include "common/Timer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace R5900;
using namespace R5900::Interpreter::OpcodeImpl;
using namespace vtlb_private;

#define EEJIT_CHECK(c) WASMJIT_CHECK(c)

namespace EEJit
{
	int g_mode = 1;
	bool g_link = true;
	bool g_trace = false;
	u32 g_trace_shift = 16;
	u64 g_trace_from = ~0ull, g_trace_to = 0;
	bool g_stats = false;
	u32 g_dbgmask = 0;
	bool g_spin = true;
	bool g_ahead = true;
	bool g_ahead_test = false;
	bool g_background = true;
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
	constexpr u32 kMaxGroupBlocks = 32; // blocks of the entry's page
	constexpr u32 kMaxModuleBlocks = 64; // with warm entries of other pages
	constexpr size_t kMaxWarm = 1u << 16;
	constexpr size_t kMaxGroups = 6000; // about 20 KB each in V8; flush everything beyond

	using BlockFn = int (*)();
	constexpr u32 kAnyPc = 1; // Leave(): an exit to whatever cpuRegs.pc is

	// Each module's first functions: the branch end and the branch test's condition (EmitBranchEndFunction() and
	// EmitBranchDueFunction()), then its blocks, then the lookup (EmitLookup()).
	constexpr u32 kFnBranchEnd = 0;
	constexpr u32 kFnBranchDue = 1;
	constexpr u32 kFnFirstBlock = 2;

	struct Group
	{
		WasmJit::LoadedFunction first; // module and instance handles
		std::unique_ptr<u32[]> checked; // per block: the page generation its code was last compared at
		std::vector<std::unique_ptr<u32[]>> cmp_descs; // the blocks' CompareCode() descriptors
		std::vector<int> slots;
		int live = 0;
	};

	// Compiled blocks by guest PC: s_pages[pc >> 12][(pc >> 2) & 1023] is a table slot, 0 when not compiled.
	int* s_pages[1u << 20];
	std::unordered_map<int, Group*> s_slot_group;
	std::vector<std::unique_ptr<Group>> s_groups;
	std::vector<u32> s_warm; // entries that ran JitHeat::ee.batch times, compiled with the next module, oldest first
	bool s_flush_requested = false;
	bool s_link = false; // block linking (tail calls): set by Execute() when the engine has them
	bool s_failed = false;
	uptr s_vmap_base = 0;

	// Statistics.
	u64 s_dispatches = 0; // blocks called by the C++ dispatcher
	u64 s_blocks_compiled = 0, s_groups_compiled = 0, s_invalidations = 0, s_flushes = 0, s_interp_steps = 0;
	u64 s_compile_ns = 0, s_max_module_ns = 0, s_wasm_bytes = 0;
	std::atomic<u64> s_guest_instrs{0}, s_native_instrs{0}; // also counted by the code generation thread
	u64 s_gen_ns = 0, s_engine_ns = 0, s_interp_ns = 0, s_max_frame_compile_ns = 0, s_frame_compile_ns = 0;
	u64 s_ahead_compiles = 0, s_ahead_ns = 0;
	u64 s_bg_jobs = 0, s_bg_installed = 0, s_bg_dropped = 0, s_bg_stale = 0, s_bg_snapshot_ns = 0;
	u32 s_frame_compile_frame = 0;
	std::vector<std::string> s_stat_names;
	std::unordered_map<std::string, u32> s_stat_index;
	u64 s_stat_counts[512];
	// -eejit-stats: runs of each compiled block (entry PC, count), and the guest instructions they ran.
	constexpr u32 kMaxBlockStats = 1u << 16;
	u32 s_block_stat_pc[kMaxBlockStats];
	u64 s_block_stat_count[kMaxBlockStats];
	u32 s_block_stats = 0;
	std::unordered_map<u32, u64> s_interp_pcs; // -eejit-stats: PCs run by the interpreter

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
	// A block's code compare, called by its entry checks when its page's generation changed (EmitEntryChecks()).
	// desc: the block's host address, its length in words, the result (1 when the words differ), then the words it
	// was compiled from. Kept out of the blocks: inline, the compare made every block much larger.
	void CompareCode(u32* desc)
	{
		const u32* host = reinterpret_cast<const u32*>(static_cast<uptr>(desc[0]));
		desc[2] = std::memcmp(host, &desc[3], desc[1] * 4) != 0;
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
		MFHI1, MFLO1, MTHI1, MTLO1, MTSAB, MTSAH, ADDI, MFC0, MTC0,
		// MMI ops on whole registers (EmitMmi()), and QFSRV.
		Mmi, QFSRV,
		// rt = rs op imm.
		ADDIU, DADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI,
		// Memory.
		LB, LBU, LH, LHU, LW, LWU, LD, LQ, SB, SH, SW, SD, SQ, LWC1, SWC1, LDL, LDR, SDL, SDR, LQC2, SQC2,
		// COP2: VU0 macro mode (the ops EmitCop2() knows; the rest call the interpreter).
		COP2,
		// COP1 (FPU.cpp).
		MFC1, MTC1, CFC1, CTC1, ADD_S, SUB_S, MUL_S, ADDA_S, SUBA_S, MULA_S, MADD_S, MSUB_S, MADDA_S, MSUBA_S,
		ABS_S, NEG_S, MOV_S, MAX_S, MIN_S, C_F, C_EQ, C_LT, C_LE, CVT_S, CVT_W, DIV_S,
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

	// MMI ops (MMI.cpp) that generated code runs on whole 128-bit registers: rd = rs op rt lane by lane, rt shifted
	// lane by lane by sa, or a byte shuffle of rt (lanes 0-15) and rs (16-31). Each does nothing when rd is $zero.
	struct MmiDesc
	{
		void (*f)();
		enum Form : u8 { Bin, Nor, Shift, Shuffle } form;
		WasmJit::Simd op; // Bin, Shift
		u8 sa_mask; // Shift
		u8 lanes[16]; // Shuffle
	};

	const MmiDesc* MmiOp(void (*f)())
	{
		using S = WasmJit::Simd;
		static const MmiDesc table[] = {
			{MMI::PADDW, MmiDesc::Bin, S::I32x4Add}, {MMI::PSUBW, MmiDesc::Bin, S::I32x4Sub},
			{MMI::PADDH, MmiDesc::Bin, S::I16x8Add}, {MMI::PSUBH, MmiDesc::Bin, S::I16x8Sub},
			{MMI::PADDB, MmiDesc::Bin, S::I8x16Add}, {MMI::PSUBB, MmiDesc::Bin, S::I8x16Sub},
			{MMI::PCGTW, MmiDesc::Bin, S::I32x4GtS}, {MMI::PCGTH, MmiDesc::Bin, S::I16x8GtS},
			{MMI::PCGTB, MmiDesc::Bin, S::I8x16GtS}, {MMI::PCEQW, MmiDesc::Bin, S::I32x4Eq},
			{MMI::PCEQH, MmiDesc::Bin, S::I16x8Eq}, {MMI::PCEQB, MmiDesc::Bin, S::I8x16Eq},
			{MMI::PMAXW, MmiDesc::Bin, S::I32x4MaxS}, {MMI::PMINW, MmiDesc::Bin, S::I32x4MinS},
			{MMI::PMAXH, MmiDesc::Bin, S::I16x8MaxS}, {MMI::PMINH, MmiDesc::Bin, S::I16x8MinS},
			{MMI::PAND, MmiDesc::Bin, S::V128And}, {MMI::POR, MmiDesc::Bin, S::V128Or},
			{MMI::PXOR, MmiDesc::Bin, S::V128Xor}, {MMI::PNOR, MmiDesc::Nor, S::V128Or},
			{MMI::PSLLH, MmiDesc::Shift, S::I16x8Shl, 0xf}, {MMI::PSRLH, MmiDesc::Shift, S::I16x8ShrU, 0xf},
			{MMI::PSRAH, MmiDesc::Shift, S::I16x8ShrS, 0xf}, {MMI::PSLLW, MmiDesc::Shift, S::I32x4Shl, 0x1f},
			{MMI::PSRLW, MmiDesc::Shift, S::I32x4ShrU, 0x1f}, {MMI::PSRAW, MmiDesc::Shift, S::I32x4ShrS, 0x1f},
			{MMI::PEXTLB, MmiDesc::Shuffle, {}, 0, {0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23}},
			{MMI::PEXTUB, MmiDesc::Shuffle, {}, 0, {8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31}},
			{MMI::PEXTLH, MmiDesc::Shuffle, {}, 0, {0, 1, 16, 17, 2, 3, 18, 19, 4, 5, 20, 21, 6, 7, 22, 23}},
			{MMI::PEXTUH, MmiDesc::Shuffle, {}, 0, {8, 9, 24, 25, 10, 11, 26, 27, 12, 13, 28, 29, 14, 15, 30, 31}},
			{MMI::PEXTLW, MmiDesc::Shuffle, {}, 0, {0, 1, 2, 3, 16, 17, 18, 19, 4, 5, 6, 7, 20, 21, 22, 23}},
			{MMI::PEXTUW, MmiDesc::Shuffle, {}, 0, {8, 9, 10, 11, 24, 25, 26, 27, 12, 13, 14, 15, 28, 29, 30, 31}},
			{MMI::PPACB, MmiDesc::Shuffle, {}, 0, {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30}},
			{MMI::PPACH, MmiDesc::Shuffle, {}, 0, {0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 28, 29}},
			{MMI::PPACW, MmiDesc::Shuffle, {}, 0, {0, 1, 2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 24, 25, 26, 27}},
			{MMI::PCPYLD, MmiDesc::Shuffle, {}, 0, {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23}},
			{MMI::PCPYUD, MmiDesc::Shuffle, {}, 0, {24, 25, 26, 27, 28, 29, 30, 31, 8, 9, 10, 11, 12, 13, 14, 15}},
			{MMI::PCPYH, MmiDesc::Shuffle, {}, 0, {0, 1, 0, 1, 0, 1, 0, 1, 8, 9, 8, 9, 8, 9, 8, 9}},
			{MMI::PEXCH, MmiDesc::Shuffle, {}, 0, {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15}},
			{MMI::PEXCW, MmiDesc::Shuffle, {}, 0, {0, 1, 2, 3, 8, 9, 10, 11, 4, 5, 6, 7, 12, 13, 14, 15}},
			{MMI::PEXEH, MmiDesc::Shuffle, {}, 0, {4, 5, 2, 3, 0, 1, 6, 7, 12, 13, 10, 11, 8, 9, 14, 15}},
			{MMI::PEXEW, MmiDesc::Shuffle, {}, 0, {8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3, 12, 13, 14, 15}},
			{MMI::PREVH, MmiDesc::Shuffle, {}, 0, {6, 7, 4, 5, 2, 3, 0, 1, 14, 15, 12, 13, 10, 11, 8, 9}},
			{MMI::PROT3W, MmiDesc::Shuffle, {}, 0, {4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3, 12, 13, 14, 15}},
		};
		for (const MmiDesc& e : table)
		{
			if (e.f == f)
				return &e;
		}
		return nullptr;
	}

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
			{MFHI1, Kind::MFHI1}, {MFLO1, Kind::MFLO1}, {MTHI1, Kind::MTHI1}, {MTLO1, Kind::MTLO1},
			{MTSAB, Kind::MTSAB}, {MTSAH, Kind::MTSAH}, {ADDI, Kind::ADDI}, {COP0::MFC0, Kind::MFC0},
			{COP0::MTC0, Kind::MTC0}, {MMI::QFSRV, Kind::QFSRV},
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
			{COP1::CVT_W, Kind::CVT_W}, {COP1::DIV_S, Kind::DIV_S},
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
		if (MmiOp(op.interpret))
			return Kind::Mmi;
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
	// Idle loops
	//
	// Games wait for interrupts in loops that only read memory and compare: GTA San Andreas spends a third of the EE
	// thread polling two counters that a DMA interrupt advances. Memory changes only in event tests (DMA, interrupts,
	// the IOP; all on the EE thread), and events only run at branch tests once cpuRegs.cycle reaches nextEventCycle.
	// So when one iteration of such a loop leaves every register as it found it, every further iteration until the
	// next event is the same iteration, and only the cycle counters move: they are advanced by what those iterations
	// would add (SpinSkip()), and the iteration whose branch test runs the event runs normally. Nothing else changes:
	// the state at every branch test is the one the loop would have had (-eejit-trace hashes each of them).
	//
	// A site is a taken backward branch whose loop [target, branch + 4] has at most 32 words of register ALU ops,
	// loads, branches and jumps (BlockCompiler::AddSpinSite()). At the site, SpinTry() runs one iteration on the
	// interpreter (intJitExecOne()), checked instruction by instruction: ALU ops, conditional branches on GPRs and
	// aligned loads of main RAM or the scratchpad only. If it changed nothing, the site remembers the registers it
	// started from and the values it loaded: the same registers and the same values make the same iteration, so the
	// next time it skips without running it.

	struct SpinWord
	{
		u32 code;
		Kind kind;
		u32 cycles; // opcode table cycles, before the Config factor
	};

	// The part of cpuRegs a branch test hashes (EEJit::TraceStep()). An iteration changes only the GPRs, HI and LO
	// (kSpinRegs bytes), pc and code; it reads COP0 Config (the cycle factor). Event tests change other COP0
	// registers (Count), which the loop does not read.
	constexpr size_t kSpinSnap = offsetof(cpuRegisters, code) + 4;
	constexpr size_t kSpinRegs = offsetof(cpuRegisters, CP0);
	using SpinSnap = std::array<u8, kSpinSnap>;

	// memcmp() == 0 for multiples of 4 bytes (musl's memcmp() goes byte by byte).
	bool SpinEqual(const void* a, const void* b, size_t n)
	{
		u32 diff = 0;
		for (size_t i = 0; i < n; i += 4)
		{
			u32 x, y;
			std::memcpy(&x, static_cast<const u8*>(a) + i, 4);
			std::memcpy(&y, static_cast<const u8*>(b) + i, 4);
			diff |= x ^ y;
		}
		return diff == 0;
	}

	// Whether the iteration-relevant state in `snap` is the current one.
	bool SpinSameRegs(const SpinSnap& snap)
	{
		const cpuRegisters& r = *reinterpret_cast<const cpuRegisters*>(snap.data());
		return SpinEqual(&cpuRegs, snap.data(), kSpinRegs) && cpuRegs.pc == r.pc && cpuRegs.code == r.code &&
			   cpuRegs.CP0.n.Config == r.CP0.n.Config;
	}

	// A branch test in the iteration.
	struct SpinTest
	{
		u32 add; // cpuBlockCycles added since the previous test
		bool end; // a taken branch's end (else the test of a branch not taken)
		bool twice; // the end updates the cycles twice (a wait loop not skipped, _doBranch_afterDelaySlot())
	};

	struct SpinLoad
	{
		u32 addr;
		u32 size;
		u64 value;
	};

	struct SpinSite
	{
		u32 target = 0;
		std::vector<SpinWord> words; // target .. branch + 4
		std::vector<u32> codes; // their instruction words
		u32 fails = 0;
		// The last iteration found to change nothing: its registers, tests, the states at its tests (with -eejit-trace)
		// and loads.
		bool memo = false;
		bool wait_loop = false; // EmuConfig.Speedhacks.WaitLoop when it was made
		s8 cycle_rate = 0; // EmuConfig.Speedhacks.EECycleRate when it was made (delta, next_bc)
		SpinSnap start;
		std::vector<SpinTest> tests;
		std::vector<SpinSnap> snaps;
		std::vector<SpinLoad> loads;
		// For each value of cpuBlockCycles (below 8) at the loop's start: the cycles one iteration adds, and
		// cpuBlockCycles after it.
		u32 delta[8];
		u8 next_bc[8];
		u64 calls = 0, tries = 0, skips = 0, iters = 0; // -eejit-stats
	};

	constexpr size_t kMaxSpinSites = 1u << 16;
	// Per site: the calls to let pass after a failed try. Generated code counts them down itself and calls SpinTry() at
	// 0, so that a site that keeps failing (GTA's, Tekken Tag Tournament's random number loop at a vsync wait: 133,000
	// calls a vsync) costs no call into C++ while it waits.
	u32 s_spin_wait[kMaxSpinSites] = {};
	std::vector<std::unique_ptr<SpinSite>> s_spin_sites = [] {
		std::vector<std::unique_ptr<SpinSite>> v;
		v.reserve(kMaxSpinSites);
		return v;
	}();
	std::mutex s_spin_mutex; // AddSpinSite() on the EE and code generation threads
	std::unordered_map<u32, u32> s_spin_index; // branch PC -> s_spin_sites index
	bool s_spin_off = false;
	u64 s_spin_tries = 0, s_spin_hits = 0, s_spin_skips = 0, s_spin_iters = 0, s_spin_cycles = 0, s_spin_why[7] = {};

	enum class SpinClass : u8
	{
		Other,
		Alu,
		Load,
		Branch, // conditional, no link
		Jump,
	};

	SpinClass SpinClassOf(Kind k)
	{
		switch (k)
		{
			case Kind::Nop:
			case Kind::SLL: case Kind::SRL: case Kind::SRA: case Kind::DSLL: case Kind::DSRL: case Kind::DSRA:
			case Kind::DSLL32: case Kind::DSRL32: case Kind::DSRA32: case Kind::SLLV: case Kind::SRLV: case Kind::SRAV:
			case Kind::DSLLV: case Kind::DSRLV: case Kind::DSRAV: case Kind::ADDU: case Kind::SUBU: case Kind::DADDU:
			case Kind::DSUBU: case Kind::AND: case Kind::OR: case Kind::XOR: case Kind::NOR: case Kind::SLT:
			case Kind::SLTU: case Kind::MOVZ: case Kind::MOVN: case Kind::MFHI: case Kind::MFLO: case Kind::ADDIU:
			case Kind::DADDIU: case Kind::SLTI: case Kind::SLTIU: case Kind::ANDI: case Kind::ORI: case Kind::XORI:
			case Kind::LUI:
				return SpinClass::Alu;
			case Kind::LB: case Kind::LBU: case Kind::LH: case Kind::LHU: case Kind::LW: case Kind::LWU: case Kind::LD:
				return SpinClass::Load;
			case Kind::BEQ: case Kind::BNE: case Kind::BLEZ: case Kind::BGTZ: case Kind::BEQL: case Kind::BNEL:
			case Kind::BLEZL: case Kind::BGTZL: case Kind::BLTZ: case Kind::BGEZ: case Kind::BLTZL: case Kind::BGEZL:
				return SpinClass::Branch;
			case Kind::J: case Kind::JAL: case Kind::JR: case Kind::JALR:
				return SpinClass::Jump;
			default:
				return SpinClass::Other;
		}
	}

	// The host address of the instruction word at pc, if it is in memory (not a handler).
	const u32* SpinWordPtr(u32 pc)
	{
		const VTLBVirtual vmv = vtlbdata.vmap[pc >> VTLB_PAGE_BITS];
		if (vmv.isHandler(pc))
			return nullptr;
		return reinterpret_cast<const u32*>(vmv.assumePtr(pc));
	}

	// Reads what the load `w` reads with the current registers, if that is aligned main RAM or scratchpad (no
	// handler, no exception).
	bool SpinRead(const SpinWord& w, SpinLoad* load)
	{
		load->addr = cpuRegs.GPR.r[(w.code >> 21) & 31].UL[0] + static_cast<s16>(w.code & 0xffff);
		switch (w.kind)
		{
			case Kind::LB: case Kind::LBU: load->size = 1; break;
			case Kind::LH: case Kind::LHU: load->size = 2; break;
			case Kind::LW: case Kind::LWU: load->size = 4; break;
			default: load->size = 8; break;
		}
		if (load->addr & (load->size - 1))
			return false;
		const VTLBVirtual vmv = vtlbdata.vmap[load->addr >> VTLB_PAGE_BITS];
		if (vmv.isHandler(load->addr))
			return false;
		const uptr host = vmv.assumePtr(load->addr);
		const uptr main = reinterpret_cast<uptr>(eeMem->Main), scratch = reinterpret_cast<uptr>(eeMem->Scratch);
		if ((host - main) >= Ps2MemSize::MainRam && (host - scratch) >= Ps2MemSize::Scratch)
			return false;
		load->value = 0;
		std::memcpy(&load->value, reinterpret_cast<const void*>(host), load->size);
		return true;
	}

	// The same address still reads the same value.
	bool SpinSame(const SpinLoad& l)
	{
		const VTLBVirtual vmv = vtlbdata.vmap[l.addr >> VTLB_PAGE_BITS];
		if (vmv.isHandler(l.addr))
			return false;
		const uptr host = vmv.assumePtr(l.addr);
		const uptr main = reinterpret_cast<uptr>(eeMem->Main), scratch = reinterpret_cast<uptr>(eeMem->Scratch);
		if ((host - main) >= Ps2MemSize::MainRam && (host - scratch) >= Ps2MemSize::Scratch)
			return false;
		u64 v = 0;
		std::memcpy(&v, reinterpret_cast<const void*>(host), l.size);
		return v == l.value;
	}

	bool SpinCond(Kind k, u32 code)
	{
		const s64 rs = cpuRegs.GPR.r[(code >> 21) & 31].SD[0], rt = cpuRegs.GPR.r[(code >> 16) & 31].SD[0];
		switch (k)
		{
			case Kind::BEQ: case Kind::BEQL: return rs == rt;
			case Kind::BNE: case Kind::BNEL: return rs != rt;
			case Kind::BLEZ: case Kind::BLEZL: return rs <= 0;
			case Kind::BGTZ: case Kind::BGTZL: return rs > 0;
			case Kind::BLTZ: case Kind::BLTZL: return rs < 0;
			default: return rs >= 0;
		}
	}

	// intUpdateCPUCycles() (pcsx2/Interpreter.cpp) on (cycle, bc), for EE cycle rates from -3 to 1, which keep the low
	// 3 bits of bc (SpinTry() refuses the others): the same scaling, step by step.
	void SpinUpdate(u64& cycle, u32& bc)
	{
		const s8 rate = EmuConfig.Speedhacks.EECycleRate;
		u32 c;
		if (rate == 0 || bc <= 40 || rate < -99)
			c = bc >> 3;
		else if (rate == 1)
			c = static_cast<u32>((bc >> 3) / 1.3f);
		else if (rate == -1)
			c = (bc <= 80 || bc > 168 ? 5 : 7) * bc / 32;
		else
			c = ((5 + (-2 * (rate + 1))) * bc) >> 5;
		cycle += c ? c : 1;
		bc &= 7;
	}

	bool SpinDue(u64 cycle) { return static_cast<s64>(cycle - cpuRegs.nextEventCycle) >= 0; }

	// One iteration's cycles from (cycle, bc), as its tests would leave them.
	void SpinIterate(const SpinSite& site, u64& cycle, u32& bc)
	{
		for (const SpinTest& t : site.tests)
		{
			bc += t.add;
			if (t.end)
			{
				if (t.twice)
					SpinUpdate(cycle, bc);
				SpinUpdate(cycle, bc);
			}
		}
	}

	// From the loop's start with the site's memo state: skips the iterations whose last branch test comes before the
	// next event (a test with an earlier cycle cannot be due when the last one is not).
	void SpinSkip(SpinSite& site)
	{
		u32* const bcp = intJitBlockCycles();
		const u64 cycle0 = cpuRegs.cycle;
		const u32 bc0 = *bcp;
		if (bc0 >= 8) // always below 8 after a branch end
			return;
		u64 cycle = cycle0;
		u32 bc = bc0;
		u64 n = 0;
		// One iteration more, unless its last test would run the event.
		auto step = [&] {
			const u64 c = cycle + site.delta[bc];
			const u32 b = site.next_bc[bc];
			if (SpinDue(c))
				return false;
			cycle = c;
			bc = b;
			n++;
			return true;
		};
		// cpuBlockCycles' low bits after each iteration are a function of their value before it, so after 7
		// iterations they repeat with a period of at most 8, each period adding the same cycles: whole periods are
		// skipped at once.
		bool more = true;
		for (int i = 0; i < 8 && more; i++)
			more = step();
		if (more)
		{
			u64 c = cycle;
			u32 b = bc;
			u32 period = 0;
			do
			{
				c += site.delta[b];
				b = site.next_bc[b];
				period++;
			} while (b != bc && period < 8);
			if (b == bc)
			{
				const u64 periods = (cpuRegs.nextEventCycle - 1 - cycle) / (c - cycle);
				cycle += periods * (c - cycle);
				n += periods * period;
			}
			while (n < (1u << 30) && step())
			{
			}
		}
		if (!n)
			return;

		if (EEJit::g_trace)
		{
			// Each skipped test with the state the loop would have had there, for the trace's hashes; the
			// iterations one by one must end where the periods did.
			SpinSnap entry;
			std::memcpy(entry.data(), &cpuRegs, kSpinSnap);
			u64 c = cycle0;
			u32 b = bc0;
			for (u64 k = 0; k < n; k++)
			{
				for (size_t i = 0; i < site.tests.size(); i++)
				{
					const SpinTest& t = site.tests[i];
					b += t.add;
					if (t.end)
					{
						if (t.twice)
							SpinUpdate(c, b);
						SpinUpdate(c, b);
					}
					const cpuRegisters& r = *reinterpret_cast<const cpuRegisters*>(site.snaps[i].data());
					std::memcpy(&cpuRegs, &r, kSpinRegs);
					cpuRegs.pc = r.pc;
					cpuRegs.code = r.code;
					cpuRegs.cycle = c;
					EEJit::TraceStep();
				}
			}
			std::memcpy(&cpuRegs, entry.data(), kSpinSnap);
			if (c != cycle || b != bc || SpinDue(c))
			{
				Console.Error("EE JIT: idle loop skip: %llu iterations end at %llu/%u, not %llu/%u.",
					static_cast<unsigned long long>(n), static_cast<unsigned long long>(c), b,
					static_cast<unsigned long long>(cycle), bc);
				s_spin_off = true;
			}
		}
		cpuRegs.cycle = cycle;
		*bcp = bc;
		s_spin_skips++;
		s_spin_iters += n;
		s_spin_cycles += cycle - cycle0;
		site.skips++;
		site.iters += n;
	}

	// Called by generated code after the end of a taken branch that is a site (index `index`).
	void SpinTry(u32 index)
	{
		if (s_spin_off || !EEJit::g_spin || *intJitBiosHooksActive() || EmuConfig.Speedhacks.EECycleRate > 1 ||
			CHECK_CACHE)
			return;
		SpinSite& site = *s_spin_sites[index];
		site.calls++;
		const u32 L = site.target;
		if (cpuRegs.pc != L || cpuRegs.branch != 0 || *intJitLastBranchTo() != L)
			return;
		// The same code (in at most two pages).
		const size_t nwords = site.codes.size();
		const size_t first = std::min<size_t>(nwords, (4096 - (L & 4095)) / 4);
		const u32* words = SpinWordPtr(L);
		if (!words || !SpinEqual(words, site.codes.data(), first * 4))
			return;
		if (first < nwords)
		{
			words = SpinWordPtr(L + static_cast<u32>(first) * 4);
			if (!words || !SpinEqual(words, site.codes.data() + first, (nwords - first) * 4))
				return;
		}

		// The remembered iteration: same registers, same loaded values.
		if (site.memo && site.wait_loop == EmuConfig.Speedhacks.WaitLoop &&
			site.cycle_rate == EmuConfig.Speedhacks.EECycleRate && SpinSameRegs(site.start) &&
			std::all_of(site.loads.begin(), site.loads.end(), SpinSame))
		{
			s_spin_hits++;
			SpinSkip(site);
			return;
		}
		s_spin_tries++;
		site.tries++;
		site.memo = false;
		auto fail = [&](int why) {
			s_spin_why[why]++;
			site.fails = std::min<u32>(site.fails + 1, 8);
			s_spin_wait[index] = 1u << site.fails;
		};

		std::memcpy(site.start.data(), &cpuRegs, kSpinSnap);
		u32* const bcp = intJitBlockCycles();
		const u32 factor = 2 - ((cpuRegs.CP0.n.Config >> 18) & 1);
		const bool trace = EEJit::g_trace;
		u64 cycle = cpuRegs.cycle;
		u32 bc = *bcp;
		u32 add = 0;
		site.tests.clear();
		site.snaps.clear();
		site.loads.clear();

		// One iteration, for real, on the interpreter.
		for (u32 steps = 0;; steps++)
		{
			const u32 pc = cpuRegs.pc;
			const u32 i = (pc - L) >> 2;
			if (steps >= 64 || (pc & 3) || i >= site.words.size())
				return fail(0);
			const SpinWord& w = site.words[i];
			const SpinClass cls = SpinClassOf(w.kind);
			if (cls == SpinClass::Alu || cls == SpinClass::Load)
			{
				if (cls == SpinClass::Load)
				{
					SpinLoad l;
					if (!SpinRead(w, &l))
						return fail(1);
					site.loads.push_back(l);
				}
				bc += w.cycles * factor;
				add += w.cycles * factor;
				intJitExecOne();
				continue;
			}
			if (cls != SpinClass::Branch)
				return fail(2);

			const bool likely = w.kind == Kind::BEQL || w.kind == Kind::BNEL || w.kind == Kind::BLEZL ||
								w.kind == Kind::BGTZL || w.kind == Kind::BLTZL || w.kind == Kind::BGEZL;
			const bool taken = SpinCond(w.kind, w.code);
			const u32 target = pc + 4 + (static_cast<s16>(w.code & 0xffff) << 2);
			SpinTest t = {};
			bc += w.cycles * factor;
			add += w.cycles * factor;
			if (taken)
			{
				// The delay slot runs inside the branch's step: check it now (the branch changes no register).
				if (i + 1 >= site.words.size())
					return fail(3);
				const SpinWord& ds = site.words[i + 1];
				const SpinClass dcls = SpinClassOf(ds.kind);
				if (dcls == SpinClass::Load)
				{
					SpinLoad l;
					if (!SpinRead(ds, &l))
						return fail(3);
					site.loads.push_back(l);
				}
				else if (dcls != SpinClass::Alu)
					return fail(3);
				bc += ds.cycles * factor;
				add += ds.cycles * factor;
				t.end = true;
				if (*intJitLastBranchTo() == target && EmuConfig.Speedhacks.WaitLoop)
				{
					// _doBranch_afterDelaySlot()'s wait loop: skipped (to the next event) or one more update.
					const u32 after = pc + 8;
					bool can_skip = true;
					if (target != 0x81fc0)
					{
						if ((after - target) < 4 * 10)
						{
							for (u32 a = target; a < after; a += 4)
							{
								if (PSM(a) != 0)
								{
									can_skip = false;
									break;
								}
							}
						}
						else
							can_skip = false;
					}
					if (can_skip)
						return fail(5); // the interpreter skips this one to the next event itself
					t.twice = true;
					SpinUpdate(cycle, bc);
				}
				SpinUpdate(cycle, bc);
			}
			else if (!likely && w.kind != Kind::BEQ && w.kind != Kind::BNE)
			{
				// BLEZ, BGTZ, BLTZ, BGEZ not taken: no test.
				intJitExecOne();
				continue;
			}
			if (SpinDue(cycle))
				return void(s_spin_why[6]++); // the event runs: left to generated code
			intJitExecOne();
			if (cpuRegs.cycle != cycle || *bcp != bc)
			{
				Console.Error("EE JIT: idle loop at %08x: cycle model mismatch, idle loops off.", pc);
				s_spin_off = true;
				return;
			}
			t.add = add;
			add = 0;
			site.tests.push_back(t);
			if (trace)
			{
				site.snaps.emplace_back();
				std::memcpy(site.snaps.back().data(), &cpuRegs, kSpinSnap);
			}
			if (taken && target == L)
				break;
		}
		if (!SpinEqual(site.start.data(), &cpuRegs, kSpinSnap) || cpuRegs.branch != 0 ||
			*intJitLastBranchTo() != L)
			return fail(4);
		site.fails = 0;
		for (u32 b = 0; b < 8; b++)
		{
			u64 c = 0;
			u32 b2 = b;
			SpinIterate(site, c, b2);
			site.delta[b] = static_cast<u32>(c);
			site.next_bc[b] = static_cast<u8>(b2);
		}
		site.memo = true;
		site.wait_loop = EmuConfig.Speedhacks.WaitLoop;
		site.cycle_rate = EmuConfig.Speedhacks.EECycleRate;
		SpinSkip(site);
	}

	// ------------------------------------------------------------------------------------------------------------
	// Code generation

	struct CodeSnapshot;
	// The word at pc in a snapshot (CodeSnapshot), if its page is in it.
	const u32* SnapWord(const CodeSnapshot* snap, u32 pc);

	class BlockCompiler
	{
	public:
		// host_page: the page's words to compile; host_addr: where the page is in memory (the same unless the words are
		// a copy, taken for code generated on another thread: `deferred`, see CompileJob).
		BlockCompiler(u32 type_v_v, u32 type_i_v, u32 page_vaddr, const u32* host_page, const u32* host_addr,
			u64 vmap_raw, u32* checked, const CodeSnapshot* snap)
			: m_tvv(type_v_v)
			, m_tiv(type_i_v)
			, m_page(page_vaddr)
			, m_host(host_page)
			, m_host_addr(host_addr)
			, m_vmap_raw(vmap_raw)
			, m_deferred(snap != nullptr)
			, m_snap(snap)
			, m_checked(checked)
			, m_fn(0)
			, m_regs(m_fn, ValType::I64, kNumRegs, &RegAddr)
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
			const u32 host = Addr(m_host_addr) + (start - m_page);
			const u32 nwords = (end - start) >> 2;
			m_region_start = start;
			m_region_words = nwords;
			// No write to the page since the last compare (CodeWrites.h).
			const bool tracked = CodeWrites::EmitSkipCompare(f, host, nwords * 4, m_checked, m_a, !m_deferred);
			m_tracked = tracked;
			// Same code (CompareCode()).
			m_cmp_desc.reset(new u32[3 + nwords]);
			m_cmp_desc[0] = host;
			m_cmp_desc[1] = nwords;
			m_cmp_desc[2] = 0;
			std::memcpy(&m_cmp_desc[3], &m_host[(start - m_page) >> 2], nwords * 4);
			f.i32_const(static_cast<s32>(Addr(m_cmp_desc.get())));
			f.i32_const(static_cast<s32>(FnIndex(&CompareCode)));
			f.call_indirect(m_tiv, 0);
			f.i32_const(0);
			f.i32_load(Addr(&m_cmp_desc[2]));
			f.br_if(0);
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

		// ---- guest registers in locals (RegCache.h) -------------------------------------------------------
		// The low doublewords (UD[0]) of the GPRs, HI and LO live in i64 locals while the block runs. The upper
		// doublewords stay in cpuRegs: instructions using all 128 bits go through it (Writeback() before a read,
		// Invalidate() after a write).
		static constexpr u32 kHI = 32, kLO = 33, kNumRegs = 34;
		using RegState = WasmJit::RegCache::State;

		static u32 RegAddr(u32 r)
		{
			return r < 32 ? GPR(r) : r == kHI ? Addr(&cpuRegs.HI.UD[0]) : Addr(&cpuRegs.LO.UD[0]);
		}
		RegState& Regs() { return m_regs.state(); }
		void Get64(u32 r) { m_regs.Get(r); }
		void Get32(u32 r)
		{
			m_regs.Get(r);
			m_fn.i32_wrap_i64();
		}
		void Ensure(u32 r) { m_regs.Ensure(r); }
		void Set64(u32 r) { m_regs.Set(r); }
		void Writeback(u32 r) { m_regs.Writeback(r); }
		void WritebackAll() { m_regs.WritebackAll(); }
		void Invalidate(u32 r) { m_regs.Invalidate(1ull << r); }
		void InvalidateAll() { m_regs.Invalidate(~0ull); }
		void Reconcile(const RegState& target) { m_regs.Reconcile(target); }

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

		// Leaves the block if an exception moved the PC (after a call: every register is written back).
		void CheckPc(u32 expected)
		{
			Function& f = m_fn;
			EEJIT_CHECK(Regs().dirty == 0);
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
			WritebackAll();
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
			WritebackAll();
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

		// The cached registers (bits as in RegState) the interpreter's function for `code` may write: its rd, rt,
		// HI/LO or $ra by instruction format; all of them for anything else (SYSCALL changes argument registers).
		static u64 FallbackWrites(u32 code)
		{
			const u64 rt = 1ull << Rt(code), rd = 1ull << Rd(code), ra = 1ull << 31;
			const u64 hilo = (1ull << kHI) | (1ull << kLO);
			const u32 op = code >> 26;
			switch (op)
			{
				case 0x00: // SPECIAL
				{
					const u32 fn = code & 0x3f;
					if (fn == 0x0c || fn == 0x0d) // SYSCALL, BREAK
						return ~0ull;
					return rd | hilo;
				}
				case 0x01: return ra; // REGIMM: BxxAL, traps, MTSAB/MTSAH
				case 0x02: case 0x03: return ra; // J, JAL
				case 0x04: case 0x05: case 0x06: case 0x07: case 0x14: case 0x15: case 0x16: case 0x17: return 0;
				case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
				case 0x18: case 0x19: return rt; // immediate ALU
				case 0x10: case 0x11: case 0x12: return rt; // COP0/1/2: MFC0, MFC1, CFC1, QMFC2, CFC2
				case 0x1c: return rd | hilo; // MMI
				case 0x1a: case 0x1b: case 0x1e: return rt; // LDL, LDR, LQ
				case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: return rt;
				case 0x31: case 0x36: case 0x37: return rt; // LWC1, LQC2, LD
				case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f: return 0; // stores, CACHE
				case 0x33: case 0x39: case 0x3e: case 0x3f: return 0; // PREF, SWC1, SQC2, SD
				default: return ~0ull;
			}
		}

		void EmitFallback(u32 pc, u32 code, const Decoded& d, bool in_delay_slot)
		{
			m_pending += d.op->cycles;
			CountStat(d.op->Name);
			StorePcCode(pc, code);
			FlushCycles();
			WritebackAll();
			CallVoid(FnIndex(d.op->interpret));
			const u64 writes = FallbackWrites(code);
			m_regs.Invalidate(writes); // not dirty: written back before the call
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

		void LoadGPR64(u32 r) { Get64(r); }
		void LoadGPR32(u32 r) { Get32(r); }

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
				expr();
				Set64(r);
			};
			// rd.SD = (s32)<i32 expression>
			auto store32s = [&](u32 r, auto&& expr) {
				expr();
				f.i64_extend_i32_s();
				Set64(r);
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
					// rd = (rt == 0) != MOVN ? rs : rd
					LoadGPR64(rs);
					LoadGPR64(rd);
					LoadGPR64(rt);
					f.i64_eqz();
					if (d.kind == Kind::MOVN)
						f.i32_eqz();
					f.select();
					Set64(rd);
					break;
				case Kind::MULT: case Kind::MULTU:
				{
					LoadGPR32(rs);
					if (d.kind == Kind::MULT) f.i64_extend_i32_s(); else f.i64_extend_i32_u();
					LoadGPR32(rt);
					if (d.kind == Kind::MULT) f.i64_extend_i32_s(); else f.i64_extend_i32_u();
					f.i64_mul();
					f.local_tee(m_v);
					f.i64_extend32_s();
					Set64(kLO);
					f.local_get(m_v);
					f.i64_const(32);
					f.i64_shr_s();
					f.i64_extend32_s();
					Set64(kHI);
					if (rd)
						store64(rd, [&] { Get64(kLO); });
					break;
				}
				case Kind::MFHI: case Kind::MFLO:
					if (dst0(rd)) return;
					store64(rd, [&] { Get64(d.kind == Kind::MFHI ? kHI : kLO); });
					break;
				case Kind::MTHI: case Kind::MTLO:
					LoadGPR64(rs);
					Set64(d.kind == Kind::MTHI ? kHI : kLO);
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

				case Kind::MFHI1: case Kind::MFLO1:
					if (dst0(rd)) return;
					store64(rd, [&] {
						f.i32_const(0);
						f.i64_load(d.kind == Kind::MFHI1 ? Addr(&cpuRegs.HI.UD[1]) : Addr(&cpuRegs.LO.UD[1]));
					});
					break;
				case Kind::MTHI1: case Kind::MTLO1:
					f.i32_const(0);
					LoadGPR64(rs);
					f.i64_store(d.kind == Kind::MTHI1 ? Addr(&cpuRegs.HI.UD[1]) : Addr(&cpuRegs.LO.UD[1]));
					break;
				case Kind::MTSAB: case Kind::MTSAH:
				{
					const u32 m = d.kind == Kind::MTSAB ? 0xf : 0x7;
					f.i32_const(0);
					LoadGPR32(rs);
					f.i32_const(m);
					f.i32_and();
					f.i32_const(imm & m);
					f.i32_xor();
					if (d.kind == Kind::MTSAH)
					{
						f.i32_const(1);
						f.i32_shl();
					}
					f.i32_store(Addr(&cpuRegs.sa));
					break;
				}
				case Kind::MFC0:
					if (!EmitMfc0(rt, rd))
					{
						EmitFallback(pc, code, d, in_delay_slot);
						return;
					}
					break;
				case Kind::MTC0:
					if (!EmitMtc0(rt, rd))
					{
						EmitFallback(pc, code, d, in_delay_slot);
						return;
					}
					break;
				case Kind::Mmi:
					EmitMmi(code, *MmiOp(d.op->interpret));
					break;
				case Kind::ADDI:
					EmitAddi(pc, code, d, in_delay_slot);
					return;
				case Kind::QFSRV:
					EmitQfsrv(pc, code, d, in_delay_slot);
					return;

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
				case Kind::C_LE: case Kind::CVT_S: case Kind::CVT_W: case Kind::DIV_S:
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

		// ---- COP0 moves (COP0.cpp) ----------------------------------------------------------------------------
		// MFC0 of anything but the debug and performance counter registers (24, 25). False: the interpreter.
		bool EmitMfc0(u32 rt, u32 rd)
		{
			Function& f = m_fn;
			if (rd == 24 || rd == 25)
				return false;
			if (rd == 9)
			{
				// Count += max(cycle - lastCOP0Cycle, 1); lastCOP0Cycle = cycle (even with rt $zero)
				const u32 count = Addr(&cpuRegs.CP0.n.Count), last = Addr(&cpuRegs.lastCOP0Cycle);
				f.i32_const(0);
				f.i32_const(0);
				f.i64_load(Addr(&cpuRegs.cycle));
				f.i32_const(0);
				f.i64_load(last);
				f.i64_sub();
				f.local_tee(m_k);
				f.i32_wrap_i64();
				f.local_get(m_k);
				f.i64_eqz();
				f.i32_add();
				f.i32_const(0);
				f.i32_load(count);
				f.i32_add();
				f.i32_store(count);
				f.i32_const(0);
				f.i32_const(0);
				f.i64_load(Addr(&cpuRegs.cycle));
				f.i64_store(last);
				if (rt == 0)
					return true;
				f.i32_const(0);
				f.i64_load32_s(count);
				Set64(rt);
				return true;
			}
			if (rt == 0)
				return true;
			f.i32_const(0);
			f.i32_load(Addr(&cpuRegs.CP0.r[rd]));
			if (rd == 12)
			{
				f.i32_const(static_cast<s32>(0xf0c79c1fu));
				f.i32_and();
			}
			f.i64_extend_i32_s();
			Set64(rt);
			return true;
		}

		// MTC0 to registers that are plain stores (not Status, Config, 24, 25). False: the interpreter.
		bool EmitMtc0(u32 rt, u32 rd)
		{
			Function& f = m_fn;
			if (rd == 12 || rd == 16 || rd == 24 || rd == 25)
				return false;
			if (rd == 9)
			{
				f.i32_const(0);
				f.i32_const(0);
				f.i64_load(Addr(&cpuRegs.cycle));
				f.i64_store(Addr(&cpuRegs.lastCOP0Cycle));
			}
			f.i32_const(0);
			LoadGPR32(rt);
			f.i32_store(Addr(&cpuRegs.CP0.r[rd]));
			return true;
		}

		// ---- MMI (MMI.cpp) ------------------------------------------------------------------------------------
		// Through cpuRegs: the operands written back, rd stored whole and dropped from the locals.
		void LoadQ(u32 r)
		{
			m_fn.i32_const(0);
			m_fn.v128_load(GPR(r));
		}

		void EmitMmi(u32 code, const MmiDesc& m)
		{
			using S = WasmJit::Simd;
			Function& f = m_fn;
			const u32 rs = Rs(code), rt = Rt(code), rd = Rd(code);
			if (rd == 0)
				return;
			bool uses_rs = m.form != MmiDesc::Shift;
			if (m.form == MmiDesc::Shuffle)
				uses_rs = std::any_of(std::begin(m.lanes), std::end(m.lanes), [](u8 l) { return l >= 16; });
			if (uses_rs)
				Writeback(rs);
			Writeback(rt);
			f.i32_const(0);
			switch (m.form)
			{
				case MmiDesc::Bin:
					LoadQ(rs);
					LoadQ(rt);
					SimdOp(m.op);
					break;
				case MmiDesc::Nor:
					LoadQ(rs);
					LoadQ(rt);
					SimdOp(S::V128Or);
					SimdOp(S::V128Not);
					break;
				case MmiDesc::Shift:
					LoadQ(rt);
					f.i32_const(static_cast<s32>(Sa(code) & m.sa_mask));
					SimdOp(m.op);
					break;
				case MmiDesc::Shuffle:
					LoadQ(rt);
					LoadQ(uses_rs ? rs : rt);
					f.shuffle(m.lanes);
					break;
			}
			f.v128_store(GPR(rd));
			Invalidate(rd);
		}

		// The slow path of an instruction emitted with a fast path in `block block <fast> br 1 end`: the interpreter's
		// function, from the register state `pre` the fast path started from, joining the fast path's state.
		// `writes`: the cached registers the function may write.
		void EmitSlowPath(u32 pc, u32 code, const Decoded& d, bool in_delay_slot, const RegState& pre, u64 writes)
		{
			const RegState fast = Regs();
			Regs() = pre;
			StorePcCode(pc, code);
			FlushCyclesSlowPath();
			WritebackAll();
			CallVoid(FnIndex(d.op->interpret));
			if (!in_delay_slot)
				CheckPc(pc + 4);
			m_regs.Invalidate(writes);
			Reconcile(fast);
		}

		// ADDI: the interpreter only when the 32-bit sum overflows (it raises the exception).
		void EmitAddi(u32 pc, u32 code, const Decoded& d, bool in_delay_slot)
		{
			Function& f = m_fn;
			const u32 rs = Rs(code), rt = Rt(code);
			m_pending += d.op->cycles;
			s_native_instrs++;
			Ensure(rs);
			const RegState pre = Regs();
			f.block(); // done
			f.block(); // slow
			LoadGPR32(rs);
			f.i64_extend_i32_s();
			f.i64_const(Imm(code));
			f.i64_add();
			f.local_tee(m_v);
			f.local_get(m_v);
			f.i64_extend32_s();
			f.i64_ne();
			f.br_if(0);
			EEJIT_CHECK(Regs() == pre);
			if (rt != 0)
			{
				f.local_get(m_v);
				Set64(rt);
			}
			f.br(1);
			f.end(); // slow
			EmitSlowPath(pc, code, d, in_delay_slot, pre, 1ull << rt);
			f.end(); // done
			if (m_pending || m_cyc_dirty)
				m_cyc_dirty = true;
		}

		// QFSRV: rd = bytes sa..sa+15 of rt:rs (rt first) for sa < 16; the interpreter for any other sa (MTSA).
		void EmitQfsrv(u32 pc, u32 code, const Decoded& d, bool in_delay_slot)
		{
			using S = WasmJit::Simd;
			Function& f = m_fn;
			const u32 rs = Rs(code), rt = Rt(code), rd = Rd(code);
			m_pending += d.op->cycles;
			s_native_instrs++;
			if (rd == 0)
				return;
			Writeback(rs);
			Writeback(rt);
			const RegState pre = Regs();
			const u32 idx = VLocal(0);
			f.block(); // done
			f.block(); // slow
			Ld32(Addr(&cpuRegs.sa));
			f.local_tee(m_a);
			f.i32_const(16);
			f.i32_ge_u();
			f.br_if(0);
			f.i32_const(0);
			static constexpr u8 iota[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
			f.v128_pooled(iota);
			f.local_get(m_a);
			SimdOp(S::I8x16Splat);
			SimdOp(S::I8x16Add);
			f.local_set(idx);
			LoadQ(rt);
			f.local_get(idx);
			SimdOp(S::I8x16Swizzle); // indices 16 and up give 0
			LoadQ(rs);
			f.local_get(idx);
			Splat(0x10101010u);
			SimdOp(S::I8x16Sub); // indices below 16 wrap to 240 and up
			SimdOp(S::I8x16Swizzle);
			SimdOp(S::V128Or);
			f.v128_store(GPR(rd));
			Invalidate(rd);
			f.br(1);
			f.end(); // slow
			EmitSlowPath(pc, code, d, in_delay_slot, pre, 1ull << rd);
			f.end(); // done
			if (m_pending || m_cyc_dirty)
				m_cyc_dirty = true;
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
					f.i64_load32_s(FPR(fs));
					Set64(rt);
					return;
				case Kind::MTC1:
					f.i32_const(0);
					LoadGPR32(rt);
					f.i32_store(FPR(fs));
					return;
				case Kind::CFC1:
					if (rt == 0)
						return;
					if (fs == 31)
					{
						f.i32_const(0);
						f.i64_load32_s(FCR31());
					}
					else
						f.i64_const(fs == 0 ? 0x2E00 : 0);
					Set64(rt);
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
				case Kind::DIV_S:
				{
					// checkDivideByZero(): a divisor with exponent 0 gives the signed maximum and sets D | SD (I | SI
					// for a dividend with exponent 0 too); else fpuDouble(fs) / fpuDouble(ft) through checkOverflow()
					// and checkUnderflow() without flags. The divide-by-zero path needs proper testing (GTA hashes only).
					const u32 sv = ILocal(2), tv = ILocal(3);
					f.i32_const(0);
					f.i32_load(FPR(fs));
					f.local_set(sv);
					f.i32_const(0);
					f.i32_load(FPR(ft));
					f.local_set(tv);
					f.local_get(tv);
					f.i32_const(0x7f800000);
					f.i32_and();
					f.i32_eqz();
					f.if_();
					{
						f.i32_const(0);
						f.i32_const(0);
						f.i32_load(FCR31());
						f.i32_const(0x00020000 | 0x00000040); // I | SI
						f.i32_const(0x00010000 | 0x00000020); // D | SD
						f.local_get(sv);
						f.i32_const(0x7f800000);
						f.i32_and();
						f.i32_eqz();
						f.select();
						f.i32_or();
						f.i32_store(FCR31());
						f.i32_const(0);
						f.local_get(tv);
						f.local_get(sv);
						f.i32_xor();
						f.i32_const(static_cast<s32>(0x80000000u));
						f.i32_and();
						f.i32_const(0x7f7fffff);
						f.i32_or();
						f.i32_store(FPR(fd));
					}
					f.else_();
					{
						f.local_get(sv);
						FpuDouble();
						f.local_get(tv);
						FpuDouble();
						f.f32_div();
						f.i32_reinterpret_f32();
						f.local_set(sv);
						f.i32_const(0);
						// infinity: sign | max; exponent 0 with a mantissa: the signed zero; else as is
						f.local_get(sv);
						f.i32_const(static_cast<s32>(0x80000000u));
						f.i32_and();
						f.local_tee(tv);
						f.i32_const(0x7f7fffff);
						f.i32_or();
						f.local_get(tv);
						f.local_get(sv);
						f.local_get(sv);
						f.i32_const(0x7f800000);
						f.i32_and();
						f.i32_eqz();
						f.local_get(sv);
						f.i32_const(0x007fffff);
						f.i32_and();
						f.i32_const(0);
						f.i32_ne();
						f.i32_and();
						f.select();
						f.local_get(sv);
						f.i32_const(0x7fffffff);
						f.i32_and();
						f.i32_const(0x7f800000);
						f.i32_eq();
						f.select();
						f.i32_store(FPR(fd));
					}
					f.end();
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
						Invalidate(rt); // all 128 bits written
					}
					return;
				case C2::QMTC2:
					if (fs != 0)
					{
						EEJIT_CHECK(!m_regs.IsDirty(rt)); // EmitCop2() wrote it back
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
						// UL[0] only
						Get64(rt);
						f.i64_const(static_cast<s64>(0xffffffff00000000ull));
						f.i64_and();
						Ld32(Addr(&VU0.VI[REG_R].UL));
						f.i32_const(0x7fffff);
						f.i32_and();
						f.i64_extend_i32_u();
						f.i64_or();
						Set64(rt);
					}
					else
					{
						f.i32_const(0);
						f.i64_load32_s(Addr(&VU0.VI[fs].UL));
						Set64(rt);
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
			// The registers both paths need, so that the slow path starts from the state of every br_if to it.
			const u32 rt = Rt(code);
			if (lq || sq)
				Ensure(Rs(code));
			else if (o.type == C2::QMTC2)
				Writeback(rt);
			else if (o.type == C2::CTC2 || (o.type == C2::CFC2 && rt != 0))
				Ensure(rt);
			const RegState pre = Regs();
			f.block(); // done
			f.block(); // slow
			Ld32(Addr(&VU0.VI[REG_VPU_STAT].UL));
			f.i32_const(1);
			f.i32_and();
			f.br_if(0);
			if (lq || sq)
			{
				const u32 rs = Rs(code);
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
				EEJIT_CHECK(Regs() == pre);
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
			const RegState fast = Regs();
			Regs() = pre;
			StorePcCode(pc, code);
			FlushCyclesSlowPath();
			WritebackAll();
			CallVoid(FnIndex(d.op->interpret));
			if (!in_delay_slot)
				CheckPc(pc + 4);
			if (o.type == C2::QMFC2 || o.type == C2::CFC2) // the GPRs the interpreter's function writes
				Invalidate(rt);
			Reconcile(fast);
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
					Set64(rt);
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

		// ---- runs of accesses through one base register ---------------------------------------------------------
		// Consecutive loads and stores off the same base register (a stack frame's saves and restores, the kernel's
		// thread context) are tested once: when every address of the run is aligned and in one directly mapped
		// page, which is when each access would take its own fast path, they all use that page's host address with
		// constant offsets. Otherwise the run is emitted access by access (EmitMemory()).
		struct RunOp
		{
			u32 code;
			Decoded d;
			s32 delta; // address - the first access's address
			u32 size;
		};

		static bool RunKind(const Decoded& d, u32 code, u32* size, u32* mask, bool* quad)
		{
			*quad = false;
			*mask = 0;
			switch (d.kind)
			{
				case Kind::LB: case Kind::LBU: case Kind::LH: case Kind::LHU: case Kind::LW: case Kind::LWU:
				case Kind::LD: case Kind::LQ:
					if (Rt(code) == 0) // the interpreter's (EmitInstruction())
						return false;
					break;
				case Kind::SB: case Kind::SH: case Kind::SW: case Kind::SD: case Kind::SQ: case Kind::LWC1:
				case Kind::SWC1:
					break;
				default:
					return false;
			}
			switch (d.kind)
			{
				case Kind::LB: case Kind::LBU: case Kind::SB: *size = 1; break;
				case Kind::LH: case Kind::LHU: case Kind::SH: *size = 2; *mask = 1; break;
				case Kind::LD: case Kind::SD: *size = 8; *mask = 7; break;
				case Kind::LQ: case Kind::SQ: *size = 16; *quad = true; break;
				default: *size = 4; *mask = 3; break;
			}
			return true;
		}

		static bool WritesGpr(Kind k) { return k >= Kind::LB && k <= Kind::LQ; }

		// Instructions EmitInstruction() emits without a call or a slow path, which a run may hold between its
		// accesses (when they do not write its base register).
		static bool RunFiller(Kind k)
		{
			return k == Kind::Nop || (k >= Kind::SLL && k <= Kind::MTSAH) || k == Kind::Mmi ||
				   (k >= Kind::ADDIU && k <= Kind::LUI) || (k >= Kind::MFC1 && k <= Kind::CVT_W);
		}

		// The accesses from pc that EmitMemoryRun() can test together (at most `limit`), or fewer than 2.
		std::vector<RunOp> FindRun(u32 pc, u32 limit)
		{
			std::vector<RunOp> run;
			u32 size, mask;
			bool quad;
			const u32 code0 = Word(pc);
			const Decoded d0 = Decode(code0);
			if (!RunKind(d0, code0, &size, &mask, &quad))
				return run;
			const u32 rs = Rs(code0);
			const bool quad0 = quad;
			s32 lo = 0, hi = 0;
			for (u32 a = pc; a < m_page + 4096 && run.size() < limit; a += 4)
			{
				const u32 code = Word(a);
				const Decoded d = Decode(code);
				if (RunFiller(d.kind))
				{
					if (FallbackWrites(code) & (1ull << rs))
						break;
					run.push_back({code, d, 0, 0});
					continue;
				}
				if (!RunKind(d, code, &size, &mask, &quad) || Rs(code) != rs || quad != quad0)
					break;
				const s32 delta = Imm(code) - Imm(code0);
				// The same alignment as the first access, so that testing its address tests them all.
				if (static_cast<u32>(delta) & (quad ? 15u : mask))
					break;
				const s32 nlo = std::min(lo, delta), nhi = std::max(hi, delta + static_cast<s32>(size));
				if (nhi - nlo > 4096)
					break;
				lo = nlo;
				hi = nhi;
				run.push_back({code, d, delta, size});
				if (WritesGpr(d.kind) && Rt(code) == rs) // the base changes
					break;
			}
			while (!run.empty() && run.back().size == 0)
				run.pop_back(); // fillers after the last access
			if (std::count_if(run.begin(), run.end(), [](const RunOp& o) { return o.size != 0; }) < 2)
				run.clear();
			return run;
		}

		void EmitMemoryRun(u32 pc, const std::vector<RunOp>& run)
		{
			Function& f = m_fn;
			const u32 code0 = run[0].code, rs = Rs(code0);
			const bool quad = run[0].d.kind == Kind::LQ || run[0].d.kind == Kind::SQ;
			u32 mask = 0;
			s32 lo = 0, hi = 0;
			bool stores = false;
			for (const RunOp& o : run)
			{
				if (o.size == 0)
					continue;
				u32 size, m;
				bool q;
				RunKind(o.d, o.code, &size, &m, &q);
				mask |= m;
				lo = std::min(lo, o.delta);
				hi = std::max(hi, o.delta + static_cast<s32>(o.size));
				stores |= !WritesGpr(o.d.kind) && o.d.kind != Kind::LWC1;
			}
			if (!m_hb_ok)
			{
				m_hb = f.local(ValType::I32);
				m_hb_ok = true;
			}
			Ensure(rs);
			const RegState pre = Regs();
			const u32 pending0 = m_pending;
			const bool cyc_dirty0 = m_cyc_dirty;
			f.block(); // done
			f.block(); // access by access
			// a = the first access's address; aligned, and [a + lo, a + hi) in one page
			LoadGPR32(rs);
			f.i32_const(Imm(code0));
			f.i32_add();
			if (quad)
			{
				f.i32_const(~0xf);
				f.i32_and();
			}
			f.local_tee(m_a);
			if (mask)
			{
				f.i32_const(static_cast<s32>(mask));
				f.i32_and();
				f.br_if(0);
				f.local_get(m_a);
			}
			f.i32_const(lo);
			f.i32_add();
			f.local_tee(m_a); // a + lo
			f.local_get(m_a);
			f.i32_const(hi - lo - 1);
			f.i32_add();
			f.i32_xor();
			f.i32_const(12);
			f.i32_shr_u();
			f.br_if(0);
			f.local_get(m_a);
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
			EEJIT_CHECK(Regs() == pre);
			f.local_get(m_v);
			f.i32_wrap_i64();
			f.local_set(m_hb); // the host address of a + lo
			for (u32 i = 0; i < run.size(); i++)
			{
				const RunOp& o = run[i];
				if (o.size == 0)
				{
					EmitInstruction(pc + i * 4, o.code, o.d, false);
					continue;
				}
				const u32 rt = Rt(o.code), off = static_cast<u32>(o.delta - lo);
				m_pending += o.d.op->cycles;
				s_native_instrs++;
				auto host = [&] { f.local_get(m_hb); };
				switch (o.d.kind)
				{
					case Kind::LB: host(); f.i32_load8_s(off); f.i64_extend_i32_s(); Set64(rt); break;
					case Kind::LBU: host(); f.i64_load8_u(off); Set64(rt); break;
					case Kind::LH: host(); f.i32_load16_s(off); f.i64_extend_i32_s(); Set64(rt); break;
					case Kind::LHU: host(); f.i32_load16_u(off); f.i64_extend_i32_u(); Set64(rt); break;
					case Kind::LW: host(); f.i64_load32_s(off); Set64(rt); break;
					case Kind::LWU: host(); f.i64_load32_u(off); Set64(rt); break;
					case Kind::LD: host(); f.i64_load(off); Set64(rt); break;
					case Kind::LQ: f.i32_const(0); host(); f.v128_load(off); f.v128_store(GPR(rt)); Invalidate(rt); break;
					case Kind::LWC1: f.i32_const(0); host(); f.i32_load(off); f.i32_store(Addr(&fpuRegs.fpr[rt].UL)); break;
					case Kind::SB: host(); LoadGPR32(rt); f.i32_store8(off); break;
					case Kind::SH: host(); LoadGPR32(rt); f.i32_store16(off); break;
					case Kind::SW: host(); LoadGPR32(rt); f.i32_store(off); break;
					case Kind::SD: host(); LoadGPR64(rt); f.i64_store(off); break;
					case Kind::SQ:
						Writeback(rt); // stored from cpuRegs, all 128 bits
						host(); f.i32_const(0); f.v128_load(GPR(rt)); f.v128_store(off);
						break;
					case Kind::SWC1: host(); f.i32_const(0); f.i32_load(Addr(&fpuRegs.fpr[rt].UL)); f.i32_store(off); break;
					default: EEJIT_CHECK(false); break;
				}
			}
			if (stores)
			{
				f.local_get(m_hb); // one page
				CodeWrites::EmitNoteWrite(f);
			}
			f.br(1);
			f.end(); // access by access
			const RegState fast = Regs();
			const u32 pending = m_pending;
			Regs() = pre;
			m_pending = pending0;
			m_cyc_dirty = cyc_dirty0;
			for (u32 i = 0; i < run.size(); i++)
			{
				if (run[i].size == 0)
					EmitInstruction(pc + i * 4, run[i].code, run[i].d, false);
				else
					EmitMemory(pc + i * 4, run[i].code, run[i].d, false);
			}
			EEJIT_CHECK(m_pending == pending);
			Reconcile(fast);
			f.end(); // done
			if (m_pending || m_cyc_dirty)
				m_cyc_dirty = true;
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

			// The registers both paths need, so that the slow path starts from the state of every br_if to it.
			const bool is_load = (d.kind >= Kind::LB && d.kind <= Kind::LQ) || d.kind == Kind::LDL || d.kind == Kind::LDR;
			Ensure(rs);
			if (d.kind == Kind::SQ)
				Writeback(rt); // stored from cpuRegs, all 128 bits
			else if (!is_load || d.kind == Kind::LDL || d.kind == Kind::LDR)
			{
				if (d.kind != Kind::LWC1 && d.kind != Kind::SWC1)
					Ensure(rt);
			}
			const RegState pre = Regs();
			f.block(); // done
			f.block(); // slow
			LoadGPR32(rs);
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
			EEJIT_CHECK(Regs() == pre);

			auto host = [&] { f.local_get(m_v); f.i32_wrap_i64(); };
			switch (d.kind)
			{
				case Kind::LB: host(); f.i32_load8_s(0); f.i64_extend_i32_s(); Set64(rt); break;
				case Kind::LBU: host(); f.i64_load8_u(0); Set64(rt); break;
				case Kind::LH: host(); f.i32_load16_s(0); f.i64_extend_i32_s(); Set64(rt); break;
				case Kind::LHU: host(); f.i32_load16_u(0); f.i64_extend_i32_u(); Set64(rt); break;
				case Kind::LW: host(); f.i64_load32_s(0); Set64(rt); break;
				case Kind::LWU: host(); f.i64_load32_u(0); Set64(rt); break;
				case Kind::LD: host(); f.i64_load(0); Set64(rt); break;
				case Kind::LQ: f.i32_const(0); host(); f.v128_load(0); f.v128_store(GPR(rt)); Invalidate(rt); break;
				case Kind::LWC1: f.i32_const(0); host(); f.i32_load(0); f.i32_store(Addr(&fpuRegs.fpr[rt].UL)); break;
				case Kind::SB: host(); LoadGPR32(rt); f.i32_store8(0); break;
				case Kind::SH: host(); LoadGPR32(rt); f.i32_store16(0); break;
				case Kind::SW: host(); LoadGPR32(rt); f.i32_store(0); break;
				case Kind::SD: host(); LoadGPR64(rt); f.i64_store(0); break;
				case Kind::SQ:
					EEJIT_CHECK(!m_regs.IsDirty(rt));
					host(); f.i32_const(0); f.v128_load(GPR(rt)); f.v128_store(0);
					break;
				case Kind::SWC1: host(); f.i32_const(0); f.i32_load(Addr(&fpuRegs.fpr[rt].UL)); f.i32_store(0); break;
				case Kind::LDL: case Kind::LDR: case Kind::SDL: case Kind::SDR: EmitUnaligned64(d.kind, rt); break;
				default: break;
			}
			if ((d.kind >= Kind::SB && d.kind <= Kind::SQ) || d.kind == Kind::SWC1 || d.kind == Kind::SDL ||
				d.kind == Kind::SDR)
				EmitNoteWrite();
			f.br(1);
			f.end(); // slow
			const RegState fast = Regs();
			Regs() = pre;
			StorePcCode(pc, code);
			FlushCyclesSlowPath();
			WritebackAll();
			CallVoid(FnIndex(d.op->interpret));
			if (!in_delay_slot)
				CheckPc(pc + 4);
			if (is_load) // the interpreter's function wrote rt
				Invalidate(rt);
			Reconcile(fast);
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

		// Whether the backward branch at pc to target closes a loop that may be idle (see SpinTry()): up to 32 words
		// of register ALU ops, loads, branches and jumps (this page and the one before). Records it as a site.
		s32 AddSpinSite(u32 pc, u32 target)
		{
			// 0x81fc0: the kernel's idle loop, which the interpreter's wait loop skip handles.
			if (target > pc || pc + 4 >= m_page + 4096 || pc + 8 - target > 4 * 32 || target == 0x81fc0)
				return -1;
			std::vector<SpinWord> words;
			for (u32 a = target; a <= pc + 4; a += 4)
			{
				// From the snapshot when the block is built on another thread (with the page before its own).
				const u32* word = m_snap ? SnapWord(m_snap, a) : SpinWordPtr(a);
				if (!word)
					return -1;
				const u32 code = *word;
				const Decoded d = Decode(code);
				if (SpinClassOf(d.kind) == SpinClass::Other)
					return -1;
				words.push_back({code, d.kind, d.op->cycles});
			}
			// The code generation thread adds sites too (CompileJob): under a lock, into room reserved so that
			// SpinTry() on the EE thread never sees the vector move, and a site is never changed once made: other
			// code for the same loop gets a site of its own.
			std::lock_guard lock(s_spin_mutex);
			const auto it = s_spin_index.find(pc);
			if (it != s_spin_index.end())
			{
				const SpinSite& site = *s_spin_sites[it->second];
				if (site.target == target && site.words.size() == words.size() &&
					std::equal(words.begin(), words.end(), site.words.begin(),
						[](const SpinWord& a, const SpinWord& b) { return a.code == b.code; }))
					return static_cast<s32>(it->second);
			}
			if (s_spin_sites.size() >= kMaxSpinSites)
				return -1;
			auto site = std::make_unique<SpinSite>();
			site->target = target;
			for (const SpinWord& w : words)
				site->codes.push_back(w.code);
			site->words = std::move(words);
			const u32 index = static_cast<u32>(s_spin_sites.size());
			s_spin_sites.push_back(std::move(site));
			s_spin_index[pc] = index;
			return static_cast<s32>(index);
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
				f.i64_const(static_cast<s64>(static_cast<u64>(pc + 8)));
				Set64(r);
			};
			// Taken: delay slot, then the interpreter's branch end, then leave. `target`: btarget, jtarget (also in
			// m_j), or kAnyPc for m_j's.
			auto taken = [&](u32 target) {
				const u32 saved_pending = m_pending;
				const bool saved_dirty = m_cyc_dirty;
				const RegState saved_regs = Regs();
				EmitDelaySlot(pc + 4);
				FlushCycles();
				WritebackAll();
				EmitBranchEnd(target);
				if (const s32 site = target == btarget && SpinClassOf(d.kind) == SpinClass::Branch ? AddSpinSite(pc, btarget) : -1;
					site >= 0)
				{
					// The site's wait (s_spin_wait) counted down here; SpinTry() when it is 0.
					const u32 wait = static_cast<u32>(reinterpret_cast<uptr>(&s_spin_wait[site]));
					f.i32_const(0);
					f.i32_load(wait);
					f.if_();
					f.i32_const(0);
					f.i32_const(0);
					f.i32_load(wait);
					f.i32_const(1);
					f.i32_sub();
					f.i32_store(wait);
					f.else_();
					f.i32_const(site);
					f.i32_const(static_cast<s32>(FnIndex(&SpinTry)));
					f.call_indirect(m_tiv, 0);
					f.end();
				}
				Leave(target, false);
				m_pending = saved_pending;
				m_cyc_dirty = saved_dirty;
				Regs() = saved_regs;
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
				// intJitBranchTest() when it has something to do (EmitBranchDueFunction()), with the registers
				// written back before and read again after (BIOS hooks, savestates).
				f.call(kFnBranchDue);
				f.if_unlikely();
				const RegState saved_regs = Regs();
				WritebackAll();
				CallVoid(FnIndex(&intJitBranchTest));
				CheckPc(resume);
				InvalidateAll();
				Reconcile(saved_regs);
				f.end();
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
			if (EEJit::g_stats && s_block_stats < kMaxBlockStats)
			{
				const u32 a = Addr(&s_block_stat_count[s_block_stats]);
				s_block_stat_pc[s_block_stats++] = start;
				f.i32_const(0);
				f.i32_const(0);
				f.i64_load(a);
				f.i64_const(1);
				f.i64_add();
				f.i64_store(a);
			}

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
				const std::vector<RunOp> run = FindRun(pc, kMaxBlockInstrs - count + 1);
				if (!run.empty())
				{
					EmitMemoryRun(pc, run);
					const u32 n = static_cast<u32>(run.size());
					count += n - 1;
					s_guest_instrs += n - 1;
					pc += 4 * n;
					continue;
				}
				EmitInstruction(pc, code, d, false);
				pc += 4;
			}
			f.i32_const(0);
			return f;
		}

	public:
		u32 m_tvv, m_tiv;
		u32 m_page;
		const u32* m_host;
		const u32* m_host_addr;
		u64 m_vmap_raw;
		bool m_deferred;
		const CodeSnapshot* m_snap; // what a block built on another thread reads (CompileJob), else null
		u32* m_checked; // the page generation this block's code was last compared at
		std::unique_ptr<u32[]> m_cmp_desc; // CompareCode()'s descriptor, kept by the module (Group::cmp_descs)
		u32 m_region_start = 0, m_region_words = 0; // the words its entry checks compare
		bool m_tracked = false; // ... skipping the compare when the page was not written (CodeWrites.h)

	private:
		Function m_fn;
		WasmJit::RegCache m_regs;
		u32 m_a, m_j, m_cyc, m_f;
		u32 m_vloc[8] = {};
		u32 m_t = 0;
		u32 m_iloc[4] = {};
		bool m_iloc_ok[4] = {};
		bool m_t_ok = false;
		bool m_vloc_ok[8] = {};
		u32 m_v, m_k;
		u32 m_hb = 0; // EmitMemoryRun(): a host address
		bool m_hb_ok = false;
		u32 m_pending = 0; // cycles not yet written, before the Config factor
		bool m_cyc_dirty = false; // m_cyc may be non-zero
	};

	// ------------------------------------------------------------------------------------------------------------
	// Code lists (EEJit.h): the log of compiled blocks, and a list from an earlier run to compile early.

	EEJit::CodeListBlock s_code_log;
	EEJit::CodeListBlock s_code_slot;
	std::unordered_set<u32> s_logged; // PCs in this session's log
	struct ListEntry
	{
		u32 pc, hash;
	};
	std::vector<ListEntry> s_list; // the list in use
	size_t s_list_cursor = 0; // CompileAhead()'s next entry
	bool s_running = false; // Execute() runs: CompileAhead() may compile
	bool s_ahead_off = false; // out of executable memory while compiling ahead: stop
	u32 s_ahead_frame = ~0u;
	u64 s_generation = 0; // FlushAll() count: a module built on another thread before a flush is dropped

	// FNV-1a of the guest code at pc: up to 8 words, not past its page. False if pc is not in memory.
	bool CodeHash(u32 pc, u32* hash)
	{
		const VTLBVirtual vmv = vtlbdata.vmap[pc >> VTLB_PAGE_BITS];
		if ((pc & 3) || vmv.isHandler(pc))
			return false;
		const u32* host = reinterpret_cast<const u32*>(vmv.assumePtr(pc));
		const u32 n = std::min<u32>(8, (4096 - (pc & 4095)) / 4);
		u32 h = 2166136261u;
		for (u32 i = 0; i < n; i++)
			h = (h ^ host[i]) * 16777619u;
		*hash = h;
		return true;
	}

	// ------------------------------------------------------------------------------------------------------------
	// Cache

	// A page holds the slots of its words, then their heat (JitHeat.h): how many times the dispatcher reached each
	// one as a block entry while it was not compiled.
	int* PageRef(u32 pc)
	{
		int*& page = s_pages[pc >> 12];
		if (!page)
			page = new int[kPageWords * 2]();
		return page;
	}

	int& SlotRef(u32 pc)
	{
		return PageRef(pc)[(pc >> 2) & (kPageWords - 1)];
	}

	int& HeatRef(u32 pc)
	{
		return PageRef(pc)[kPageWords + ((pc >> 2) & (kPageWords - 1))];
	}

	// Counts an entry at pc that is not compiled; true once it is hot.
	bool Warm(u32 pc)
	{
		int& heat = HeatRef(pc);
		if (static_cast<u32>(heat) + 1 < JitHeat::ee.threshold)
		{
			heat++;
			if (static_cast<u32>(heat) == JitHeat::ee.batch && s_warm.size() < kMaxWarm)
				s_warm.push_back(pc);
			return false;
		}
		heat = static_cast<int>(JitHeat::ee.threshold - 1);
		return JitHeat::CanCompile();
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

	// keep_heat: when the cache is full, so that only hot code is compiled again.
	void FlushAll(bool keep_heat)
	{
		for (int*& page : s_pages)
		{
			if (!page)
				continue;
			if (keep_heat)
			{
				std::fill(page, page + kPageWords, 0);
				continue;
			}
			delete[] page;
			page = nullptr;
		}
		if (!keep_heat)
		{
			s_warm.clear();
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
		s_generation++;
	}

	// intUpdateCPUCycles(). With an EE cycle rate of 0, here: cpuRegs.cycle += max(cpuBlockCycles >> 3, 1), and
	// cpuBlockCycles keeps its low 3 bits. With another rate (the EE underclock or overclock speedhack), the
	// interpreter's own function. `tmp`: an i32 local.
	void EmitUpdateCycles(Function& f, u32 tmp, u32 type_v_v)
	{
		const u32 bc = Addr(intJitBlockCycles());
		f.i32_const(0);
		f.i32_load8_s(Addr(&EmuConfig.Speedhacks.EECycleRate));
		f.if_();
		f.i32_const(static_cast<s32>(FnIndex(&intUpdateCPUCycles)));
		f.call_indirect(type_v_v, 0);
		f.else_();
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
		f.end();
	}

	// Pushes whether intBranchTest() has something to do: the trace, an event due, or the BIOS hooks.
	void EmitBranchDue(Function& f)
	{
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
	}

	// EmitBranchDue() as a function. () -> (i32).
	Function EmitBranchDueFunction()
	{
		Function f(0);
		EmitBranchDue(f);
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
	// the delay slot took an exception (cpuRegs.branch is 0), with the BIOS hooks active, or on a wait loop (the same
	// target as the last branch, with the WaitLoop speedhack) that may be skipped: the BIOS idle loop at 0x81fc0, a
	// branch to the word after its delay slot, or a loop of up to 10 words whose first one has no physical pointer
	// (PSM(target) == 0, vtlb_GetPhyPtr()). The JIT only runs from intExecute(), so Cpu is the interpreter there.
	// (target) -> ().
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
			EmitUpdateCycles(f, t, type_v_v);
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
		EmitUpdateCycles(f, t, type_v_v);
		// intBranchTest(), as intJitBranchTest(), when it has something to do.
		EmitBranchDue(f);
		f.if_();
		f.i32_const(static_cast<s32>(FnIndex(&intJitBranchTest)));
		f.call_indirect(type_v_v, 0);
		f.end();
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

	// Compiles the block at entry, its neighbours in the same page and the warm entries of any page (JitHeat.h) into
	// one module. False if entry cannot be compiled.
	// What a module is compiled from when its code is generated on another thread (CompileJob): copies taken on the
	// EE thread of the pages it may read (their words, mapping, and which of their blocks may join the module as
	// neighbours) and of the warm entries it may take.
	struct CodeSnapshot
	{
		struct Page
		{
			u32 vpage;
			u64 raw;
			const u32* host;
			std::unique_ptr<u32[]> words;
			u64 eligible[kPageWords / 64]; // not compiled and ran JitHeat::ee.neighbour times
		};
		std::vector<Page> pages;
		std::vector<u32> warm; // warm entries that may join, in s_warm's order

		const Page* Find(u32 pc) const
		{
			for (const Page& p : pages)
			{
				if (p.vpage == (pc & ~4095u))
					return &p;
			}
			return nullptr;
		}
	};

	const u32* SnapWord(const CodeSnapshot* snap, u32 pc)
	{
		const CodeSnapshot::Page* p = snap->Find(pc);
		return p ? &p->words[(pc & 4095) >> 2] : nullptr;
	}

	// A module built and not yet compiled by the engine.
	struct GroupBuild
	{
		std::vector<u8> bytes;
		std::vector<u32> pcs;
		std::unique_ptr<u32[]> checked;
		std::vector<std::unique_ptr<u32[]>> cmp_descs;
		struct Region
		{
			u32 pc, words;
			const u32* host; // in memory
			std::vector<u32> code; // the words compiled from
			bool tracked;
		};
		std::vector<Region> regions; // deferred builds: what InstallGroup() checks
		size_t warm_taken = 0;
		u64 gen_ns = 0;
		bool deferred = false;
	};

	// Builds the module of entry, its neighbours in the same page and the warm entries of any page (JitHeat.h). From
	// memory and the JIT's state (snap null, on the EE thread), or from a snapshot (on the code generation thread).
	bool BuildGroup(u32 entry, const CodeSnapshot* snap, GroupBuild* out)
	{
		Common::Timer timer;
		const bool deferred = snap != nullptr;
		WasmJit::Module m;
		const u32 t_v_v = m.type({}, {});
		const u32 t_i_v = m.type({ValType::I32}, {});
		const u32 t_v_i = m.type({}, {ValType::I32});
		m.import_memory("env", "mem", 0, 65536, true);
		m.import_table("env", "table", 0);

		std::unique_ptr<u32[]> checked = std::make_unique<u32[]>(kMaxModuleBlocks);
		std::vector<u32>& pcs = out->pcs;
		std::vector<std::unique_ptr<BlockCompiler>> blocks;
		std::vector<u32> work{entry};
		u32 first = entry; // the entry whose neighbours `work` holds
		size_t warm_taken = 0;
		const std::vector<u32>& warm = deferred ? snap->warm : s_warm;
		auto eligible = [&](u32 pc) {
			if (!deferred)
				return SlotRef(pc) == 0 && static_cast<u32>(HeatRef(pc)) >= JitHeat::ee.neighbour;
			const CodeSnapshot::Page* p = snap->Find(pc);
			const u32 w = (pc >> 2) & (kPageWords - 1);
			return p && (p->eligible[w / 64] >> (w % 64)) & 1;
		};
		for (;;)
		{
			// The entry's page first, then each warm entry with its own warm neighbours.
			if (work.empty() || pcs.size() >= kMaxGroupBlocks)
			{
				work.clear();
				while (warm_taken < warm.size() && work.empty())
				{
					const u32 pc = warm[warm_taken++];
					// A snapshot's warm entries were picked under the same rules (SubmitGroup()).
					bool ok = deferred && snap->Find(pc);
					if (!deferred)
						ok = SlotRef(pc) == 0 && static_cast<u32>(HeatRef(pc)) >= JitHeat::ee.batch &&
						     !vtlbdata.vmap[pc >> VTLB_PAGE_BITS].isHandler(pc);
					if (ok && std::find(pcs.begin(), pcs.end(), pc) == pcs.end())
					{
						work.push_back(pc);
						first = pc;
					}
				}
				if (work.empty() || pcs.size() >= kMaxModuleBlocks)
					break;
			}
			const u32 pc = work.front();
			work.erase(work.begin());
			if (std::find(pcs.begin(), pcs.end(), pc) != pcs.end())
				continue;
			if (pc != first && !eligible(pc))
				continue;
			const u32 page = pc & ~4095u;
			const u32* host;
			const u32* host_addr;
			u64 raw;
			if (deferred)
			{
				const CodeSnapshot::Page* p = snap->Find(pc);
				if (!p)
					continue;
				host = p->words.get();
				host_addr = p->host;
				raw = p->raw;
			}
			else
			{
				const VTLBVirtual vmv = vtlbdata.vmap[pc >> VTLB_PAGE_BITS];
				host = host_addr = reinterpret_cast<const u32*>(vmv.assumePtr(page));
				raw = vmv.raw();
			}
			std::vector<u32> next;
			blocks.push_back(std::make_unique<BlockCompiler>(t_v_v, t_i_v, page, host, host_addr, raw, &checked[pcs.size()], snap));
			blocks.back()->Run(pc, &next);
			pcs.push_back(pc);
			for (u32 n : next)
				work.push_back(n);
		}
		out->warm_taken = warm_taken;
		out->deferred = deferred;
		if (pcs.empty())
			return false;
		for (const auto& bc : blocks)
		{
			if (!deferred)
				continue;
			GroupBuild::Region r;
			r.pc = bc->m_region_start;
			r.words = bc->m_region_words;
			r.host = bc->m_host_addr + ((r.pc - bc->m_page) >> 2);
			const u32* from = bc->m_host + ((r.pc - bc->m_page) >> 2);
			r.code.assign(from, from + r.words);
			r.tracked = bc->m_tracked;
			out->regions.push_back(std::move(r));
		}
		// Block i is function kFnFirstBlock + i; the next one returns 0 (links to blocks outside the module).
		const u32 nblocks = static_cast<u32>(pcs.size());
		m.add_function(t_i_v, EmitBranchEndFunction(t_v_v, t_i_v));
		m.add_function(t_v_i, EmitBranchDueFunction());
		if (EEJit::g_stats)
		{
			m.name_function(kFnBranchEnd, "ee_branch_end");
			m.name_function(kFnBranchDue, "ee_branch_due");
		}
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
			if (EEJit::g_stats)
			{
				std::snprintf(name, sizeof(name), "ee_%08x", pcs[i]);
				m.name_function(index, name);
			}
		}
		const u32 lookup = m.add_function(t_v_i, EmitLookup(t_v_i));
		if (EEJit::g_stats)
			m.name_function(lookup, "ee_lookup");
		for (const auto& bc : blocks)
			out->cmp_descs.push_back(std::move(bc->m_cmp_desc));
		blocks.clear();
		out->bytes = m.finish();
		out->checked = std::move(checked);
		out->gen_ns = static_cast<u64>(timer.GetTimeNanoseconds());
		return true;
	}

	// Compiles a built module with the engine and puts its blocks in the cache (EE thread). A module built on
	// another thread is first checked against memory: its blocks' words must still be those it was compiled from,
	// which they then compare with as a compile here would have (CodeWrites.h); otherwise it is dropped.
	bool InstallGroup(u32 entry, GroupBuild& build, bool allow_flush)
	{
		Common::Timer timer;
		for (const GroupBuild::Region& r : build.regions)
		{
			if (std::memcmp(r.host, r.code.data(), r.words * 4) != 0)
			{
				s_bg_dropped++;
				return false;
			}
		}
		for (size_t i = 0; i < build.regions.size(); i++)
		{
			const GroupBuild::Region& r = build.regions[i];
			if (!r.tracked)
				continue;
			const u32 page = static_cast<u32>(reinterpret_cast<uptr>(r.host)) >> 12;
			CodeWrites::AddCodePage(page);
			build.checked[i] = CodeWrites::Current(page);
		}
		const std::vector<u8>& bytes = build.bytes;
		const std::vector<u32>& pcs = build.pcs;
		s_wasm_bytes += bytes.size();
		s_gen_ns += build.gen_ns;
		auto g = std::make_unique<Group>();
		g->checked = std::move(build.checked);
		g->cmp_descs = std::move(build.cmp_descs);
		const int module = wasmjit_compile(bytes.data(), bytes.size());
		const int instance = module > 0 ? wasmjit_instantiate(module) : 0;
		if (module <= 0 || instance <= 0)
		{
			if (module > 0)
				wasmjit_release(module);
			if (module < 0 || instance < 0)
			{
				// Out of executable memory: start again with an empty cache (not under running code: CompileAhead()).
				if (allow_flush)
					FlushAll(true);
				else
					s_ahead_off = true;
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
			// The log, in the order blocks are compiled (the page reads count, then the entries before it).
			const u32 n = s_code_log.count.load(std::memory_order_relaxed);
			u32 hash;
			if (n < EEJit::kCodeListCapacity && CodeHash(pcs[i], &hash) && s_logged.insert(pcs[i]).second)
			{
				s_code_log.entries[2 * n] = pcs[i];
				s_code_log.entries[2 * n + 1] = hash;
				s_code_log.count.store(n + 1, std::memory_order_release);
			}
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
		// The EE thread's own time: the engine's compile, and the build when it was not done on another thread.
		const u64 engine_ns = static_cast<u64>(timer.GetTimeNanoseconds());
		s_engine_ns += engine_ns;
		const u64 ns = engine_ns + (build.regions.empty() && !build.deferred ? build.gen_ns : 0);
		if (s_frame_compile_frame != g_FrameCount)
		{
			s_frame_compile_frame = g_FrameCount;
			s_frame_compile_ns = 0;
		}
		s_frame_compile_ns += ns;
		s_max_frame_compile_ns = std::max(s_max_frame_compile_ns, s_frame_compile_ns);
		s_compile_ns += ns;
		s_max_module_ns = std::max(s_max_module_ns, ns);
		PerfCounters::Add(PerfCounters::jit_compile_ns, ns);
		PerfCounters::Add(PerfCounters::ee_modules, 1);
		JitHeat::Spend(ns);
		return true;
	}


	// Builds and installs on the EE thread.
	bool CompileGroup(u32 entry, bool allow_flush = true)
	{
		if (vtlbdata.vmap[entry >> VTLB_PAGE_BITS].isHandler(entry))
			return false;
		GroupBuild build;
		const bool built = BuildGroup(entry, nullptr, &build);
		s_warm.erase(s_warm.begin(), s_warm.begin() + build.warm_taken);
		return built && InstallGroup(entry, build, allow_flush);
	}

	// ------------------------------------------------------------------------------------------------------------
	// Code generation on another thread (EEJit::g_background). Building a module (BlockCompiler, the module's
	// bytes) is about three quarters of compiling it; the engine's compile and instantiation must stay on the EE
	// thread, which runs the code (WasmJitRuntime.h). When a block is hot, the EE thread copies what the build reads
	// (CodeSnapshot) and goes on interpreting; a thread builds the module, and the dispatcher installs it once it
	// is ready (InstallGroup(), which checks the code is still the same). One job at a time.

	struct CompileJob
	{
		u32 entry;
		u64 generation;
		CodeSnapshot snap;
		GroupBuild build;
		bool built = false;
		bool ahead = false; // from the code list (SubmitAhead()): kept in s_ready until needed or idle time
	};

	std::mutex s_job_mutex;
	std::condition_variable s_job_cv;
	CompileJob* s_job_in = nullptr; // handed to the thread (under s_job_mutex)
	std::atomic<CompileJob*> s_job_done{nullptr}; // built, for the dispatcher
	CompileJob* s_job_busy = nullptr; // EE thread: the job in flight
	bool s_worker_started = false;

	void CompileThread()
	{
		Threading::SetNameOfCurrentThread("EE JIT build");
		for (;;)
		{
			CompileJob* job;
			{
				std::unique_lock lock(s_job_mutex);
				s_job_cv.wait(lock, [] { return s_job_in != nullptr; });
				job = s_job_in;
				s_job_in = nullptr;
			}
			Common::Timer timer;
			job->built = BuildGroup(job->entry, &job->snap, &job->build);
			PerfCounters::Add(PerfCounters::ee_build_ns, static_cast<u64>(timer.GetTimeNanoseconds()));
			s_job_done.store(job, std::memory_order_release);
		}
	}

	// Copies the page of pc (once) into the snapshot, and the page before it (an idle loop that ends in this page may
	// start there, AddSpinSite()). False if pc is not in memory.
	bool SnapPage(CodeSnapshot& snap, u32 pc, bool with_previous = true)
	{
		if (snap.Find(pc))
			return true;
		if (with_previous && pc >= 4096)
			SnapPage(snap, pc - 4096, false);
		const VTLBVirtual vmv = vtlbdata.vmap[pc >> VTLB_PAGE_BITS];
		if (vmv.isHandler(pc))
			return false;
		CodeSnapshot::Page p;
		p.vpage = pc & ~4095u;
		p.raw = vmv.raw();
		p.host = reinterpret_cast<const u32*>(vmv.assumePtr(p.vpage));
		p.words = std::make_unique<u32[]>(kPageWords);
		std::memcpy(p.words.get(), p.host, 4096);
		std::fill(std::begin(p.eligible), std::end(p.eligible), 0);
		if (const int* page = s_pages[pc >> 12])
		{
			for (u32 w = 0; w < kPageWords; w++)
			{
				if (page[w] == 0 && static_cast<u32>(page[kPageWords + w]) >= JitHeat::ee.neighbour)
					p.eligible[w / 64] |= 1ull << (w % 64);
			}
		}
		snap.pages.push_back(std::move(p));
		return true;
	}

	// Hands the build of entry's module to the code generation thread. False if a job is already in flight.
	bool SubmitGroup(u32 entry, bool ahead = false)
	{
		if (s_job_busy)
			return false;
		if (!s_worker_started)
		{
			std::thread(CompileThread).detach();
			s_worker_started = true;
		}
		Common::Timer timer;
		auto job = std::make_unique<CompileJob>();
		job->entry = entry;
		job->generation = s_generation;
		job->ahead = ahead;
		if (!SnapPage(job->snap, entry))
			return false;
		// The warm entries the build may take (a module holds kMaxModuleBlocks blocks at most).
		for (size_t i = 0; i < s_warm.size() && job->snap.warm.size() < 2 * kMaxModuleBlocks; i++)
		{
			const u32 pc = s_warm[i];
			if (SlotRef(pc) == 0 && static_cast<u32>(HeatRef(pc)) >= JitHeat::ee.batch && SnapPage(job->snap, pc))
				job->snap.warm.push_back(pc);
		}
		s_bg_snapshot_ns += static_cast<u64>(timer.GetTimeNanoseconds());
		s_bg_jobs++;
		s_job_busy = job.release();
		{
			std::lock_guard lock(s_job_mutex);
			s_job_in = s_job_busy;
		}
		s_job_cv.notify_one();
		return true;
	}

	// Modules built ahead from the code list (SubmitAhead()), not installed yet: the engine's compile and the
	// instantiation are the EE thread's, so they wait for the game to reach one of their blocks (TakeReady()) or for
	// the frame limiter's spare time (EEJit::CompileAhead()). At most kMaxReady, oldest first.
	constexpr size_t kMaxReady = 16;
	std::vector<std::unique_ptr<CompileJob>> s_ready;

	bool InReady(u32 pc)
	{
		for (const auto& job : s_ready)
		{
			if (std::find(job->build.pcs.begin(), job->build.pcs.end(), pc) != job->build.pcs.end())
				return true;
		}
		return false;
	}

	// Installs a module built ahead. False if it is stale (a flush since) or its code changed.
	bool InstallReady(std::unique_ptr<CompileJob> job, bool allow_flush)
	{
		if (job->generation != s_generation)
		{
			s_bg_stale++;
			return false;
		}
		if (!InstallGroup(job->entry, job->build, allow_flush))
			return false;
		s_bg_installed++;
		s_ahead_compiles++;
		PerfCounters::Add(PerfCounters::ee_ahead_modules, 1);
		return true;
	}

	// The dispatcher wants pc compiled: installs the module built ahead that holds it, if there is one.
	bool TakeReady(u32 pc)
	{
		for (size_t i = 0; i < s_ready.size(); i++)
		{
			const std::vector<u32>& pcs = s_ready[i]->build.pcs;
			if (std::find(pcs.begin(), pcs.end(), pc) == pcs.end())
				continue;
			std::unique_ptr<CompileJob> job = std::move(s_ready[i]);
			s_ready.erase(s_ready.begin() + i);
			return InstallReady(std::move(job), true) && SlotRef(pc) != 0;
		}
		return false;
	}

	bool ListedAndUncompiled(const struct ListEntry& e);

	// Keeps the code generation thread busy with the code list while it has nothing else to build: the next listed
	// blocks whose code is in memory, as in EEJit::CompileAhead(), built into a module that waits in s_ready.
	void SubmitAhead()
	{
		// Modules built before a flush would be dropped when installed: free their places.
		std::erase_if(s_ready, [](const std::unique_ptr<CompileJob>& job) { return job->generation != s_generation; });
		if (!EEJit::g_ahead || s_list.empty() || s_job_busy || s_ahead_off || s_ready.size() >= kMaxReady)
			return;
		const size_t n = s_list.size();
		for (size_t scanned = 0; scanned < std::min<size_t>(n, 256); scanned++)
		{
			const ListEntry& e = s_list[s_list_cursor];
			s_list_cursor = (s_list_cursor + 1) % n;
			if (!ListedAndUncompiled(e) || InReady(e.pc))
				continue;
			std::vector<u32> more;
			for (size_t i = 1; i < std::min<size_t>(n, 512) && more.size() + 1 < kMaxModuleBlocks; i++)
			{
				const ListEntry& m = s_list[(s_list_cursor + i - 1) % n];
				if (ListedAndUncompiled(m) && !InReady(m.pc))
					more.push_back(m.pc);
			}
			for (u32 pc : more)
				HeatRef(pc) = std::max(HeatRef(pc), static_cast<int>(JitHeat::ee.batch));
			HeatRef(e.pc) = std::max(HeatRef(e.pc), static_cast<int>(JitHeat::ee.batch));
			s_warm.insert(s_warm.begin(), more.begin(), more.end());
			SubmitGroup(e.pc, true);
			return;
		}
	}

	// At the dispatcher: installs the job's module once it is built (a module built ahead waits in s_ready), then
	// gives the thread its next job from the code list.
	void PollGroup()
	{
		CompileJob* const job = s_job_done.load(std::memory_order_acquire);
		if (!job)
			return;
		s_job_done.store(nullptr, std::memory_order_relaxed);
		s_job_busy = nullptr;
		std::unique_ptr<CompileJob> owned(job);
		if (job->generation != s_generation || !job->built)
		{
			s_bg_stale++;
			SubmitAhead();
			return;
		}
		// The warm entries it took leave the warm list, as CompileGroup() does.
		for (u32 pc : job->build.pcs)
		{
			const auto it = std::find(s_warm.begin(), s_warm.end(), pc);
			if (it != s_warm.end())
				s_warm.erase(it);
		}
		if (job->ahead)
			s_ready.push_back(std::move(owned));
		else if (InstallGroup(job->entry, job->build, true))
			s_bg_installed++;
		SubmitAhead();
	}

	// A list entry whose code is in memory, as listed, and not compiled.
	bool ListedAndUncompiled(const ListEntry& e)
	{
		const int* page = s_pages[e.pc >> 12];
		u32 hash;
		return !(page && page[(e.pc >> 2) & (kPageWords - 1)]) && CodeHash(e.pc, &hash) && hash == e.hash;
	}

	// Once a vsync, at the dispatcher: takes a list the page handed over.
	void CodeListVsync()
	{
		s_ahead_frame = g_FrameCount;
		if (s_code_slot.word0.load(std::memory_order_acquire) == EEJit::CODE_SLOT_READY)
		{
			const u32 n = std::min(s_code_slot.count.load(std::memory_order_relaxed), EEJit::kCodeListCapacity);
			if (EEJit::g_ahead && s_code_slot.version[0] == EEJit::kCodeListVersion)
			{
				s_list.clear();
				for (u32 i = 0; i < n; i++)
					s_list.push_back({s_code_slot.entries[2 * i], s_code_slot.entries[2 * i + 1]});
				s_list_cursor = 0;
				Console.WriteLn("EE JIT: a code list of %u blocks from an earlier run.", n);
			}
			s_code_slot.word0.store(EEJit::CODE_SLOT_FREE, std::memory_order_release);
		}
		if (EEJit::g_background && !EEJit::g_stats)
			SubmitAhead();
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
		FlushAll(!s_flush_requested && s_vmap_base == reinterpret_cast<uptr>(vtlbdata.vmap));
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

	s_running = true;
	u32* const block_cycles = intJitBlockCycles();
	// Whether the PC was reached by a jump, a taken branch or an exception, or by leaving compiled code: the
	// places blocks start, whose runs count towards compiling them (JitHeat.h).
	bool entry = true;
	for (;;)
	{
		if (g_FrameCount != s_ahead_frame) [[unlikely]]
			CodeListVsync();
		if (s_job_busy) [[unlikely]]
			PollGroup();
		const u32 pc = cpuRegs.pc;
		int* const page = s_pages[pc >> 12];
		int slot = page ? page[(pc >> 2) & (kPageWords - 1)] : 0;
		if (slot)
		{
			entry = true;
			s_dispatches++;
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
			// The new code is new: it has to become hot again.
			page[kPageWords + ((pc >> 2) & (kPageWords - 1))] = 0;
		}
		if (s_groups.size() > kMaxGroups)
			FlushAll(true);
		const bool aligned = (pc & 3) == 0;
		const bool compile = JitHeat::ee.threshold <= 1 ? aligned && JitHeat::CanCompile() : entry && aligned && Warm(pc);
		// On the code generation thread unless counting statistics (which the build updates without locks).
		const bool background = g_background && !g_stats;
		if (compile && background && TakeReady(pc))
			continue;
		if (!compile || (background ? (SubmitGroup(pc), true) : !CompileGroup(pc)))
		{
			if (s_failed)
				return;
			s_interp_steps++;
			if (g_stats)
				s_interp_pcs[pc]++;
			if (g_stats && (s_interp_steps & 63) == 0) // one step in 64 timed, less the timer's own cost
			{
				const u64 t0 = Common::Timer::GetCurrentValue();
				const u64 t1 = Common::Timer::GetCurrentValue();
				intJitExecOne();
				const u64 t2 = Common::Timer::GetCurrentValue();
				const s64 d = static_cast<s64>(t2 - t1) - static_cast<s64>(t1 - t0);
				s_interp_ns += 64 * static_cast<u64>(std::max<double>(0, Common::Timer::ConvertValueToNanoseconds(static_cast<u64>(std::max<s64>(d, 0)))));
			}
			else
				intJitExecOne();
			entry = cpuRegs.pc != pc + 4;
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

u64 EEJit::CompileNs()
{
	return s_compile_ns;
}

namespace
{
	uint s_budget_frame = 0;
	u64 s_budget_spent = 0;
} // namespace

bool JitHeat::CanCompile()
{
	if (!budget_ns)
		return true;
	if (s_budget_frame != g_FrameCount)
	{
		s_budget_frame = g_FrameCount;
		s_budget_spent = 0;
	}
	return s_budget_spent < budget_ns;
}

void JitHeat::Spend(u64 ns)
{
	if (s_budget_frame != g_FrameCount)
	{
		s_budget_frame = g_FrameCount;
		s_budget_spent = 0;
	}
	s_budget_spent += ns;
}

void EEJit::PrintStats()
{
	Console.WriteLn("EE JIT: %s, %llu blocks in %llu modules (%llu KB of wasm, %.1f ms to build and compile, at most %.1f ms for one), "
					"%llu invalidations, %llu flushes, %llu interpreted steps; %llu of %llu instructions native",
		g_mode ? "on" : "off", static_cast<unsigned long long>(s_blocks_compiled),
		static_cast<unsigned long long>(s_groups_compiled), static_cast<unsigned long long>(s_wasm_bytes / 1024),
		s_compile_ns / 1e6, s_max_module_ns / 1e6, static_cast<unsigned long long>(s_invalidations), static_cast<unsigned long long>(s_flushes),
		static_cast<unsigned long long>(s_interp_steps), static_cast<unsigned long long>(s_native_instrs.load()),
		static_cast<unsigned long long>(s_guest_instrs.load()));
	if (s_bg_jobs)
		Console.WriteLn("EE JIT: built on another thread: %llu modules, %llu installed, %llu dropped (code changed), %llu stale; "
						"%.1f ms copying for them",
			static_cast<unsigned long long>(s_bg_jobs), static_cast<unsigned long long>(s_bg_installed),
			static_cast<unsigned long long>(s_bg_dropped), static_cast<unsigned long long>(s_bg_stale), s_bg_snapshot_ns / 1e6);
	if (!s_list.empty() || s_ahead_compiles)
		Console.WriteLn("EE JIT: code list of %zu blocks: %llu modules compiled ahead in %.1f ms; log of %u blocks",
			s_list.size(), static_cast<unsigned long long>(s_ahead_compiles),
			s_ahead_ns / 1e6, s_code_log.count.load(std::memory_order_relaxed));
	Console.WriteLn("EE JIT: idle loops: %zu sites, %llu tries, %llu remembered, %llu skips of %llu iterations (%llu cycles)%s",
		s_spin_sites.size(), static_cast<unsigned long long>(s_spin_tries), static_cast<unsigned long long>(s_spin_hits),
		static_cast<unsigned long long>(s_spin_skips),
		static_cast<unsigned long long>(s_spin_iters), static_cast<unsigned long long>(s_spin_cycles),
		g_spin ? (s_spin_off ? ", turned off" : "") : ", off");
	if (!g_stats)
		return;
	Console.WriteLn("EE JIT: %llu blocks called by the dispatcher", static_cast<unsigned long long>(s_dispatches));
	Console.WriteLn("EE JIT: compile split: %.1f ms generating, %.1f ms engine compile+instantiate; worst vsync %.1f ms; %.1f ms interpreting (timed per step)",
		s_gen_ns / 1e6, s_engine_ns / 1e6, s_max_frame_compile_ns / 1e6, s_interp_ns / 1e6);
	Console.WriteLn("EE JIT: idle loop fails: %llu left, %llu load, %llu op, %llu delay slot, %llu changed, %llu wait loops; %llu events due",
		static_cast<unsigned long long>(s_spin_why[0]), static_cast<unsigned long long>(s_spin_why[1]),
		static_cast<unsigned long long>(s_spin_why[2]), static_cast<unsigned long long>(s_spin_why[3]),
		static_cast<unsigned long long>(s_spin_why[4]), static_cast<unsigned long long>(s_spin_why[5]),
		static_cast<unsigned long long>(s_spin_why[6]));
	{
		std::vector<std::pair<u64, u32>> sv;
		for (const auto& [pc, index] : s_spin_index)
			sv.emplace_back(s_spin_sites[index]->calls, pc);
		std::sort(sv.rbegin(), sv.rend());
		for (size_t i = 0; i < std::min<size_t>(sv.size(), 10); i++)
		{
			const SpinSite& site = *s_spin_sites[s_spin_index[sv[i].second]];
			Console.WriteLn("  idle loop %08x -> %08x: %llu calls, %llu tries, %llu skips of %llu iterations", sv[i].second,
				site.target, static_cast<unsigned long long>(site.calls), static_cast<unsigned long long>(site.tries),
				static_cast<unsigned long long>(site.skips), static_cast<unsigned long long>(site.iters));
		}
	}
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
	std::vector<std::pair<u64, u32>> ip;
	for (const auto& [pc, n] : s_interp_pcs)
		ip.emplace_back(n, pc);
	std::sort(ip.rbegin(), ip.rend());
	for (size_t i = 0; i < std::min<size_t>(ip.size(), 16); i++)
		Console.WriteLn("  interpreted %08x %12llu %s", ip[i].second, static_cast<unsigned long long>(ip[i].first),
			vtlbdata.vmap[ip[i].second >> VTLB_PAGE_BITS].isHandler(ip[i].second) ? "(handler)" : "");
	// The blocks run most, by entries (a block compiled again counts separately), with their instructions as they
	// are in memory now.
	std::vector<std::pair<u64, u32>> b;
	for (u32 i = 0; i < s_block_stats; i++)
		b.emplace_back(s_block_stat_count[i], s_block_stat_pc[i]);
	std::sort(b.rbegin(), b.rend());
	for (size_t i = 0; i < std::min<size_t>(b.size(), 12); i++)
	{
		const u32 pc = b[i].second;
		std::string ops;
		const VTLBVirtual vmv = vtlbdata.vmap[pc >> VTLB_PAGE_BITS];
		if (!vmv.isHandler(pc))
		{
			const u32* host = reinterpret_cast<const u32*>(vmv.assumePtr(pc));
			for (u32 k = 0; k < 24 && ((pc + k * 4) & 4095) >= (pc & 4095); k++)
			{
				const OPCODE& op = R5900::GetInstruction(host[k]);
				ops += ' ';
				ops += op.Name;
				if (k > 0 && (op.flags & IS_BRANCH) == 0 && IsNativeBranch(Decode(host[k - 1]).kind))
					break;
			}
		}
		Console.WriteLn("  block %08x %12llu:%s", pc, static_cast<unsigned long long>(b[i].first), ops.c_str());
	}
}

EEJit::CodeListBlock* EEJit::CodeLog()
{
	return &s_code_log;
}

EEJit::CodeListBlock* EEJit::CodeListSlot()
{
	return &s_code_slot;
}

void EEJit::NewCodeLogSession()
{
	s_list.clear(); // a list is for one game: the page hands it over again once it knows the game
	s_list_cursor = 0;
	s_logged.clear();
	s_code_log.version[0] = kCodeListVersion;
	s_code_log.version[1] = 0;
	s_code_log.count.store(0, std::memory_order_release);
	s_code_log.word0.fetch_add(1, std::memory_order_acq_rel);
}

void EEJit::CompileAhead(u64 deadline)
{
	if (!g_ahead || !s_running || s_failed || s_ahead_off || s_list.empty() || !g_mode)
		return;
	if (g_background && !g_stats)
	{
		// The code generation thread builds the list's modules (SubmitAhead()); spare time installs them, about
		// 0.5 ms each (the engine's compile). Under running code (the limiter runs in the event test): no flush.
		const u64 install_margin = GetTickFrequency() / 1000;
		Common::Timer timer;
		while (!s_ready.empty() && GetCPUTicks() + install_margin < deadline && s_groups.size() + 1 < kMaxGroups)
		{
			std::unique_ptr<CompileJob> job = std::move(s_ready.front());
			s_ready.erase(s_ready.begin());
			InstallReady(std::move(job), false);
		}
		s_ahead_ns += static_cast<u64>(timer.GetTimeNanoseconds());
		return;
	}
	// A module takes about 2 ms to build and compile (at most about 15): start one only with 3 ms left.
	const u64 margin = GetTickFrequency() * 3 / 1000;
	Common::Timer timer;
	const size_t n = s_list.size();
	for (size_t scanned = 0; scanned < n; scanned++)
	{
		if (GetCPUTicks() + margin >= deadline || s_groups.size() + 1 >= kMaxGroups)
			break;
		const ListEntry& e = s_list[s_list_cursor];
		s_list_cursor = (s_list_cursor + 1) % n;
		if (!ListedAndUncompiled(e))
			continue; // compiled, or its code is not in memory (yet)
		// The next listed blocks in memory go into the same module, as warm entries (CompileGroup() takes them
		// after the entry's page). Counted as warm, they are also taken as neighbours.
		std::vector<u32> more;
		for (size_t i = 1; i < n && more.size() + 1 < kMaxModuleBlocks; i++)
		{
			const ListEntry& m = s_list[(s_list_cursor + i - 1) % n];
			if (ListedAndUncompiled(m))
				more.push_back(m.pc);
		}
		for (u32 pc : more)
			HeatRef(pc) = std::max(HeatRef(pc), static_cast<int>(JitHeat::ee.batch));
		HeatRef(e.pc) = std::max(HeatRef(e.pc), static_cast<int>(JitHeat::ee.batch));
		s_warm.insert(s_warm.begin(), more.begin(), more.end());
		// Under running code (the limiter runs in the event test): no flush, which would release it.
		if (CompileGroup(e.pc, false))
		{
			s_ahead_compiles++;
			PerfCounters::Add(PerfCounters::ee_ahead_modules, 1);
		}
	}
	s_ahead_ns += static_cast<u64>(timer.GetTimeNanoseconds());
}

bool EEJit::LoadCodeList(const char* path)
{
	FILE* fp = std::fopen(path, "rb");
	if (!fp)
		return false;
	s_list.clear();
	u32 w[2];
	while (s_list.size() < kCodeListCapacity && std::fread(w, 4, 2, fp) == 2)
		s_list.push_back({w[0], w[1]});
	std::fclose(fp);
	s_list_cursor = 0;
	Console.WriteLn("EE JIT: a code list of %zu blocks from %s.", s_list.size(), path);
	return true;
}

bool EEJit::SaveCodeLog(const char* path)
{
	FILE* fp = std::fopen(path, "wb");
	if (!fp)
		return false;
	const u32 n = s_code_log.count.load(std::memory_order_acquire);
	const bool ok = std::fwrite(s_code_log.entries, 8, n, fp) == n;
	std::fclose(fp);
	Console.WriteLn("EE JIT: the code log of %u blocks written to %s.", n, path);
	return ok;
}
