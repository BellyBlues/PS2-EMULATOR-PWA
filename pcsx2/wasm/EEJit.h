// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// EE (R5900) JIT for WebAssembly: compiles guest code blocks to WebAssembly at run time and runs them in place of
// the interpreter's fetch/decode loop (pcsx2/Interpreter.cpp, intExecute()). The generated code reproduces the
// interpreter exactly: the same cycle accounting (cpuBlockCycles), the same branch ends and event tests, and the
// interpreter's own opcode functions for every instruction it does not emit natively. So emulation with the JIT on
// is the same, cycle for cycle, as with the interpreter. See EEJit.cpp.

#pragma once

#include "common/Pcsx2Types.h"

#include <atomic>

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
	// Idle loops (EEJit.cpp): iterations that change nothing are skipped up to the next event (-eejit-spin 0: off).
	extern bool g_spin;
	void PrintStats();
	// Time spent building and compiling modules so far, in nanoseconds.
	u64 CompileNs();

	// Runs guest code until the interpreter's longjmp leaves intExecute(). Returns only if the JIT could not
	// start or had to be turned off (a generator bug); the caller then interprets.
	void Execute();

	// Drops every compiled block (on the next Execute(), on the EE thread). Called on VM reset.
	void RequestFlush();

	// Code lists (EEJit.cpp, "Code lists"): the JIT logs each block it compiles (its PC and a hash of its first
	// words); a list from an earlier run of the same game, handed back, makes those blocks compile the first time
	// they run instead of after JitHeat::ee.threshold runs on the interpreter, and when the frame limiter has time
	// left (CompileAhead()), compiles the ones whose code is in memory before they run. Only when code is
	// compiled changes, never what it does. -eejit-ahead 0 (g_ahead) turns the use of lists off.
	extern bool g_ahead;
	// Modules are built on a thread of their own and installed when ready, the EE thread interpreting meanwhile
	// (EEJit.cpp, CompileJob); -eejit-bg 0 builds them on the EE thread, as the dispatcher reaches them.
	extern bool g_background;
	constexpr u32 kCodeListCapacity = 32768;
	constexpr u32 kCodeListVersion = 1;
	// The log and the slot a list is handed over in share this layout with the page (pcsx2web_ee_code_log(),
	// pcsx2web_ee_code_list()): a word that is the log's session or the slot's state (CODE_SLOT_*), the count, the
	// version (kCodeListVersion, 0), then count entries of two words: PC, hash.
	enum : u32
	{
		CODE_SLOT_FREE = 0,
		CODE_SLOT_WRITING = 1,
		CODE_SLOT_READY = 2,
	};
	struct CodeListBlock
	{
		std::atomic<u32> word0;
		std::atomic<u32> count;
		u32 version[2];
		u32 entries[2 * kCodeListCapacity];
	};
	CodeListBlock* CodeLog();
	CodeListBlock* CodeListSlot();
	// A new VM: the log starts over, in a new session (the page tells a stale log from the new one by it).
	void NewCodeLogSession();
	// The frame limiter has time until `deadline` (GetCPUTicks()): compiles listed blocks whose code is in memory.
	// EE thread only.
	void CompileAhead(u64 deadline);
	// Test mode (-eejit-ahead 2): CompileAhead() at every vsync with 5 ms, even when the frame is late (as in trace
	// runs, slower than real time), so that compiling ahead is covered by state hash comparisons.
	extern bool g_ahead_test;
	// Headless runs: a list from a file of entries (PC, hash as two little-endian words), and the log to a file.
	bool LoadCodeList(const char* path);
	bool SaveCodeLog(const char* path);
} // namespace EEJit

// Interpreter entry points used by the JIT (pcsx2/Interpreter.cpp).
u32* intJitBlockCycles();
u32* intJitLastBranchTo();
bool* intJitBiosHooksActive();
void intJitExecOne();
void intJitBranchEnd(u32 target);
void intJitBranchTest();
void intJitStraightLineTest();
