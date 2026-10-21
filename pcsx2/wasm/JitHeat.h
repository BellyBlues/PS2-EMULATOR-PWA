// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// When the EE and IOP JITs (EEJit.cpp, IopJit.cpp) compile code. Compiling a block costs about as much as
// interpreting it a few hundred times, and much of a game's code runs only a few times (boot, loading, the setup of
// a scene or a cutscene), so new code runs on the interpreter, which the generated code matches exactly, until a
// block entry has been reached `threshold` times; it is compiled then, with the neighbours in its page that have
// run at least `neighbour` times (0: all of them, as when every block was compiled on its first run). Both JITs
// compile on the EE thread, synchronously: a per-vsync budget of compile time spreads a burst of newly hot code
// over several vsyncs, the rest running on the interpreter meanwhile.

#pragma once

#include "common/Pcsx2Types.h"

namespace JitHeat
{
	struct Policy
	{
		u32 threshold; // block entries compile on this run; 1: on the first (no counting)
		u32 neighbour; // the least runs of a neighbour compiled with them; 0: every neighbour
		u32 batch; // entries of any page that ran this many times go into the next module compiled; 0: none
	};

	// Measured on a Guitar Hero boot into a song (headless, Node 24, M1 Max): compiling every block on its first run
	// built 54,700 EE blocks in 4,100 modules, 1.4-2 s of compiling with up to 170 ms in one vsync after boot; these
	// values build 13,500 in 460 modules, 0.3-0.6 s, and with the budget at most 5.5 ms per vsync after boot, at the
	// same speed in the song. Fewer, larger modules matter more than fewer blocks: each module costs about 0.3 ms.
	inline Policy ee = {128, 1, 64};
	inline Policy iop = {128, 1, 64};
	// EE and IOP compile time allowed per vsync, in nanoseconds (0: no limit). A module started within the budget
	// is finished, so a vsync can go over it by one module.
	inline u64 budget_ns = 4000000;

	// Whether a compile may start now (the budget for this vsync is not spent).
	bool CanCompile();
	// Adds a compile's time to this vsync's.
	void Spend(u64 ns);
} // namespace JitHeat
