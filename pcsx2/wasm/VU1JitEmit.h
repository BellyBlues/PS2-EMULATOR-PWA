// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU1 JIT code generation (docs/web-port/VU1-JIT-DESIGN.md sections 4, 5, 7). One block per module: a straight run
// of instructions from an entry (PC, pipeline signature) to a branch's delay slot, the program end, or the first
// instruction generated code does not run. The timing model decides every stall, write-back and pipe entry at
// compile time; generated code does only the data work, on the interpreter's own state in VURegs (VF, VI, ACC, the
// pending flags and the pipe entries' value snapshots stay in memory), so that ops without native code call the
// interpreter's op bodies directly and every exit leaves a state the interpreter continues from. The timing
// fields of the pipes are written at the exit, from the model (ApplyExit). A block whose branch goes back to its own
// entry with the entry's signature (a vertex loop) does ApplyExit's and the dispatcher's work for that exit itself
// and runs again without leaving generated code.

#pragma once

#include "VU1JitAnalysis.h"

#include <vector>

namespace VU1Jit
{
	enum class ExitKind : u8
	{
		Boundary, // the state is complete at the start of the instruction at tpc
		End, // the program ended
	};

	enum class ExitReason : u8
	{
		Fallback, // an instruction generated code does not run
		Budget,
		BusyKick, // an XGKICK found the previous kick running: its cycle cost is data dependent
		DTBit,
		UnknownTarget,
		Cap,
		Block, // the block ended (branch, fall-through): the next one may follow at once
		Count
	};

	// The timing fields the model holds at an exit; times relative to TIMING_MODEL_BASE.
	struct PipeImage
	{
		fmacPipe fmac[4];
		u32 fmacreadpos, fmacwritepos, fmaccount;
		fdivPipe fdiv;
		efuPipe efu;
		ialuPipe ialu[4];
		u32 ialureadpos, ialuwritepos, ialucount;
		u8 VIBackupCycles;
		u32 VIRegNumber;
		u64 cycle;
	};

	struct BlockExit
	{
		ExitKind kind = ExitKind::Boundary;
		ExitReason reason = ExitReason::Block;
		u32 instrs = 0; // instructions generated code ran before it
		u32 tpc = 0; // TPC as the interpreter leaves it (unmasked), unless dynamic_tpc
		bool dynamic_tpc = false; // JR/JALR: generated code stored TPC
		u32 ebit = 0;
		PipeImage pipes = {};
		s32 efu_cycle_adjust = 0; // an EFU entry pending since the entry: efu.Cycle changes (VUops.cpp:270)
		bool mbit = false; // VU0: the last instruction had the M bit (VUFLAG_MFLAGSET)
		bool chain = false; // sig is valid: the dispatcher may enter the next block without deriving it
		Signature sig = {};
	};

	struct CompiledBlock
	{
		std::vector<u8> wasm;
		std::vector<BlockExit> exits;
		u32 instrs = 0;
		u32 last_start = 0; // cycles from the entry to the start of the last instruction (the budget test)
		u32 native_ops = 0;
		u32 helper_ops = 0;
		bool loops = false; // a taken branch back to the entry with the entry's signature stays in generated code
	};

	// Compiles the block at pc (byte address) for an entry with signature sig and no branch or E bit pending,
	// reading the VU's micro memory (vu: 0 or 1). False when there is nothing to compile: the first instruction must run on the interpreter.
	bool CompileBlock(int vu, u32 pc, const Signature& sig, CompiledBlock* out);

	// After generated code returned exit x: writes the timing state, TPC and the control state. base is VU1.cycle at
	// the entry.
	void ApplyExit(VURegs& R, const BlockExit& x, u64 base);

	// Rotates the FMAC and IALU rings so that their read positions are 0, as blocks expect at their entry. Ring
	// positions are not observable (the interpreter only uses them relative to each other).
	void NormalizeRings(VURegs& R);

	// VU1.cycle at the entry of the running block, for the helpers. A block that loops (CompiledBlock::loops) moves it
	// to the start of each pass, so after it returns it is the base of the pass that exited.
	extern u64 g_block_base;
	extern u64 g_block_base0; // the same for VU0

	// The dispatcher's budget (VU1): VU1.cycle at the start of its Execute plus its cycles. A looping block leaves
	// before a pass whose last instruction would start at or after it, as the dispatcher would not enter it.
	extern u64 g_budget_end;
	// Passes a looping block made back to its start since the dispatcher entered it (VU1).
	extern u32 g_block_loops;

	// Debugging: log every block and interpreter step of the dispatcher.
	extern bool g_trace;
} // namespace VU1Jit
