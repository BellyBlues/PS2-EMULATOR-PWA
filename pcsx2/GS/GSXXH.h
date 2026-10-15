// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "MultiISA.h"

#ifdef __wasm_simd128__
// On wasm, xxhash.h includes <arm_neon.h> (Emscripten's NEON-on-SIMD128 header) from
// inside extern "C", which breaks because that header pulls in C++ templates. Include
// it here first so the later include does nothing.
#include <arm_neon.h>
#endif

#ifndef XXH_versionNumber
	#define XXH_STATIC_LINKING_ONLY 1
	#define XXH_INLINE_ALL 1
	#include <xxhash.h>
#endif

MULTI_ISA_DEF(u64 GSXXH3_64_Long(const void* data, size_t len);)
MULTI_ISA_DEF(u32 GSXXH3_64_Update(void* state, const void* data, size_t len);)
MULTI_ISA_DEF(u64 GSXXH3_64_Digest(void* state);)

static inline u64 __forceinline GSXXH3_64bits(const void* data, size_t len)
{
	// XXH3 has optimized functions for small inputs and they aren't vectorized
	if (len <= XXH3_MIDSIZE_MAX)
		return XXH3_64bits(data, len);
	return MultiISAFunctions::GSXXH3_64_Long(data, len);
}

static inline XXH_errorcode __forceinline GSXXH3_64bits_update(XXH3_state_t* state, const void* input, size_t len)
{
	// XXH3 update has no optimized functions for small inputs
	return static_cast<XXH_errorcode>(MultiISAFunctions::GSXXH3_64_Update(static_cast<void*>(state), input, len));
}

static inline u64 __forceinline GSXXH3_64bits_digest(XXH3_state_t* state)
{
	return MultiISAFunctions::GSXXH3_64_Digest(static_cast<void*>(state));
}
