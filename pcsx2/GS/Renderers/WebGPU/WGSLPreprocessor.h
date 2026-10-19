// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A small C-style preprocessor for the TFX WGSL template (docs/web-port/WEBGPU-DESIGN.md §9.1). WGSL has
// none, and tfx.wgsl keeps the #if structure of vulkan/tfx.glsl. Supported: object-like #define and
// #undef, #if/#ifdef/#ifndef/#elif/#else/#endif with integer expressions (defined(), ! ~ - + * / % << >>
// < <= > >= == != & ^ | && || and parentheses), and substitution of defined names in code lines (their
// values are integer literals in practice). No function-like macros, no token pasting, no #include.
namespace WGSLPreprocessor
{
	using Macros = std::vector<std::pair<std::string, std::string>>;

	/// Returns the preprocessed source; on a syntax error, sets *error and returns what it has.
	std::string Process(std::string_view source, const Macros& macros, std::string* error);
} // namespace WGSLPreprocessor
