// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "Pcsx2Defs.h"
#include <cstdint>
#include <cstddef>

#if defined(ARCH_WASM)

#include <csetjmp>

// Wasm code cannot save and restore registers, so this is setjmp/longjmp, which
// -sSUPPORT_LONGJMP=wasm lowers to wasm exception handling. fastjmp_set has to be
// a macro: setjmp must run in the frame that gets jumped back into.
struct fastjmp_buf
{
	std::jmp_buf buf;
};

#define fastjmp_set(b) setjmp((b)->buf)
#define fastjmp_jmp(b, ret) longjmp(const_cast<fastjmp_buf*>(b)->buf, (ret))

#else

struct fastjmp_buf
{
#if defined(_WIN32)
	static constexpr std::size_t BUF_SIZE = 240;
#elif defined(ARCH_ARM64)
	static constexpr std::size_t BUF_SIZE = 168;
#else
	static constexpr std::size_t BUF_SIZE = 64;
#endif

	alignas(16) std::uint8_t buf[BUF_SIZE];
};

extern "C" {
int fastjmp_set(fastjmp_buf* buf);
__noreturn void fastjmp_jmp(const fastjmp_buf* buf, int ret);
}

#endif
