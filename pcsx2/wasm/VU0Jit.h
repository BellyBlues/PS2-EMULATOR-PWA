// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU0 micro mode on the VU1 JIT's code generator (pcsx2/wasm/VU1JitEmit.h): blocks compiled for VU0's interpreter
// orchestration (VU0microInterp.cpp), run on the EE thread by WasmVU0::Execute in place of InterpVU0::Execute.
// Memory ops (VU0's 0x4000 mapping of VU1's registers) and everything without native code call the interpreter's op
// bodies; D/T bits, branches in delay slots and the like run on the interpreter. Selected by VMManager when
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
} // namespace VU0Jit
