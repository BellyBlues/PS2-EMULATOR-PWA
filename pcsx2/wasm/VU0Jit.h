// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU0 micro mode on the VU1 JIT's code generator (pcsx2/wasm/VU1JitEmit.h): modules of blocks that go to each other
// inside generated code, compiled for VU0's interpreter orchestration (VU0microInterp.cpp), run on the EE thread by
// WasmVU0::Execute in place of InterpVU0::Execute. Memory ops are native on VU0's own memory; at its 0x4000 mapping of
// VU1's registers, and in delay slots, the interpreter takes them. Everything else without native code calls the
// interpreter's op bodies; D/T bits, branches in delay slots and the like run on the interpreter. Selected by VMManager when
// EmuCore/CPU/Recompiler/EnableVU0 is set. VU0 macro mode (COP2) is not covered.

#pragma once

#include "VUmicro.h"

class WasmVU0 final : public BaseVUmicroCPU
{
public:
	WasmVU0();
	~WasmVU0() override = default;

	const char* GetShortName() const override { return "wVU0"; }
	const char* GetLongName() const override { return "VU0 WebAssembly JIT"; }

	void Shutdown() override;
	void Reset() override;
	void SetStartPC(u32 startPC) override;
	void Execute(u32 cycles) override;
	void Step() override;
	void Clear(u32 addr, u32 size) override;
};

extern WasmVU0 CpuWasmVU0;

namespace VU0Jit
{
	void SetLockstep(bool enabled); // every VU0 Execute on the JIT and on the interpreter, compared
	void LogReport();

	// Whether WasmVU0::Execute runs VU0 on its own dispatcher (not the interpreter, not lockstep): it can then tell, of
	// a run to the program's end, whether one Execute up to cycle `end` would have ended it (BeginWindow, EndWindow):
	// the program's last instruction started before `end` and no M bit stopped it before. _vu0SyncFinish (VU0.cpp).
	bool CanTrackWindow();
	void BeginWindow(u64 end);
	bool EndWindow();
} // namespace VU0Jit
