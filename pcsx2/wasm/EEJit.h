// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// EE (R5900) JIT for WebAssembly: compiles guest code blocks to WebAssembly at run time and runs them in place of
// the interpreter's fetch/decode loop (pcsx2/Interpreter.cpp, intExecute()). The generated code reproduces the
// interpreter exactly: the same cycle accounting (cpuBlockCycles), the same branch ends and event tests, and the
// interpreter's own opcode functions for every instruction it does not emit natively. So emulation with the JIT on
// is the same, cycle for cycle, as with the interpreter. See EEJit.cpp.

#pragma once

#include "common/Pcsx2Types.h"

namespace EEJit
{
	// 0: interpreter only; 1: JIT (default). Read when the EE starts executing (intExecute()).
	extern int g_mode;
	// Block linking (-eejit-link 0 turns it off, for comparisons).
	extern bool g_link;

	// Compare mode (-eejit-trace in the headless runner): a hash of the EE state is updated at every branch test
	// and printed every 2^g_trace_shift tests, so that a JIT run and an interpreter run can be compared state by
	// state. Steps in [g_trace_from, g_trace_to] are printed one by one.
	extern bool g_trace;
	extern u32 g_trace_shift;
	extern u64 g_trace_from, g_trace_to;
	void TraceStep();
	void TraceFinish();

	// Statistics (-eejit-stats): counts executions of the instructions that fall back to the interpreter.
	extern bool g_stats;
	extern u32 g_dbgmask; // debugging: COP2 kinds (bit per EEJit.cpp C2 value, 0x10000 LQC2/SQC2) left to the interpreter
	void PrintStats();
	// Time spent building and compiling modules so far, in nanoseconds.
	u64 CompileNs();

	// Runs guest code until the interpreter's longjmp leaves intExecute(). Returns only if the JIT could not
	// start or had to be turned off (a generator bug); the caller then interprets.
	void Execute();

	// Drops every compiled block (on the next Execute(), on the EE thread). Called on VM reset.
	void RequestFlush();
} // namespace EEJit

// Interpreter entry points used by the JIT (pcsx2/Interpreter.cpp).
u32* intJitBlockCycles();
u32* intJitLastBranchTo();
bool* intJitBiosHooksActive();
void intJitExecOne();
void intJitBranchEnd(u32 target);
void intJitBranchTest();
void intJitStraightLineTest();
