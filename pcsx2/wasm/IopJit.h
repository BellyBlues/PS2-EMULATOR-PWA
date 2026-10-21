// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// IOP (R3000A) JIT for WebAssembly: compiles IOP code blocks to WebAssembly at run time and runs them in place of
// the interpreter's instruction loop (pcsx2/R3000AInterpreter.cpp, intExecuteBlock()). Like the EE JIT
// (pcsx2/wasm/EEJit.h), the generated code reproduces the interpreter exactly: one cycle per instruction, the
// interpreter's own branch code (event test, idle loop skip, BIOS hooks) and its opcode functions for every
// instruction it does not emit. See IopJit.cpp.

#pragma once

#include "common/Pcsx2Types.h"

namespace IopJit
{
	// 0: interpreter only; 1: JIT (default). -iopjit 0 (headless) or the page's ?iopjit=0 turn it off.
	extern int g_mode;

	// Runs IOP instructions from psxRegs.pc the way intExecuteBlock()'s inner loop does: until a taken branch has
	// ended (branch2) or, with no branch, 0x10000 cycles after last_cycle.
	void Run(u64 last_cycle);

	// Drops every compiled block (on the next Run()). Called on IOP reset.
	void RequestFlush();

	void PrintStats();
	// Time spent building and compiling modules so far, in nanoseconds.
	u64 CompileNs();
} // namespace IopJit

// Interpreter entry points used by the JIT (pcsx2/R3000AInterpreter.cpp).
void psxJitBranchStart(u32 tar);
void psxJitBranchEnd(u32 tar);
bool psxJitBranched();
bool* psxJitBranchFlag();
u32* psxJitBranchPC();
bool psxJitExecOne(u64 last_cycle);
