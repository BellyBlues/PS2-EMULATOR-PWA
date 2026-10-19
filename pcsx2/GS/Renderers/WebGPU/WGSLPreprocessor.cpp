// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/WebGPU/WGSLPreprocessor.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>

namespace
{
	using MacroMap = std::unordered_map<std::string, std::string>;

	bool IsIdentStart(char c)
	{
		return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
	}

	bool IsIdentChar(char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
	}

	/// Replaces defined names in text by their values, rescanning the result (at most depth levels).
	std::string Expand(std::string_view text, const MacroMap& macros, int depth, bool unknown_to_zero)
	{
		std::string out;
		out.reserve(text.size());
		size_t i = 0;
		while (i < text.size())
		{
			const char c = text[i];
			if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
			{
				out.append(text.substr(i)); // a comment: leave the rest of the line alone
				break;
			}
			if (IsIdentStart(c))
			{
				size_t j = i + 1;
				while (j < text.size() && IsIdentChar(text[j]))
					j++;
				const std::string name(text.substr(i, j - i));
				const auto it = macros.find(name);
				if (it != macros.end() && depth > 0)
					out += Expand(it->second, macros, depth - 1, unknown_to_zero);
				else if (unknown_to_zero && it == macros.end())
					out += '0';
				else
					out += name;
				i = j;
				continue;
			}
			if (std::isdigit(static_cast<unsigned char>(c)))
			{
				// Numbers (including suffixes such as 1u or 0x7Fu, and floats) are copied whole, so that a
				// suffix is not taken for a name.
				size_t j = i + 1;
				while (j < text.size() && (IsIdentChar(text[j]) || text[j] == '.'))
					j++;
				out.append(text.substr(i, j - i));
				i = j;
				continue;
			}
			out += c;
			i++;
		}
		return out;
	}

	/// Replaces defined(X) and defined X by 1 or 0.
	std::string ReplaceDefined(std::string_view text, const MacroMap& macros)
	{
		std::string out;
		size_t i = 0;
		while (i < text.size())
		{
			if (IsIdentStart(text[i]))
			{
				size_t j = i + 1;
				while (j < text.size() && IsIdentChar(text[j]))
					j++;
				const std::string_view name = text.substr(i, j - i);
				if (name == "defined")
				{
					size_t k = j;
					while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k])))
						k++;
					const bool paren = (k < text.size() && text[k] == '(');
					if (paren)
						k++;
					while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k])))
						k++;
					size_t e = k;
					while (e < text.size() && IsIdentChar(text[e]))
						e++;
					const std::string arg(text.substr(k, e - k));
					if (paren)
					{
						while (e < text.size() && text[e] != ')')
							e++;
						if (e < text.size())
							e++;
					}
					out += macros.count(arg) ? '1' : '0';
					i = e;
					continue;
				}
				out.append(name);
				i = j;
				continue;
			}
			out += text[i++];
		}
		return out;
	}

	class ExprParser
	{
	public:
		explicit ExprParser(std::string_view s)
			: m_s(s)
		{
		}

		bool Parse(int64_t* value)
		{
			*value = Ternary();
			Skip();
			return !m_error && m_pos == m_s.size();
		}

	private:
		void Skip()
		{
			while (m_pos < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_pos])))
				m_pos++;
		}

		bool Accept(std::string_view op)
		{
			Skip();
			if (m_s.substr(m_pos, op.size()) != op)
				return false;
			// Do not take "<" from "<<", "&" from "&&" and so on.
			if (op.size() == 1 && m_pos + 1 < m_s.size())
			{
				const char n = m_s[m_pos + 1];
				if ((op[0] == '<' || op[0] == '>') && (n == op[0] || n == '='))
					return false;
				if ((op[0] == '&' || op[0] == '|') && n == op[0])
					return false;
				if ((op[0] == '!' || op[0] == '=') && n == '=')
					return false;
			}
			m_pos += op.size();
			return true;
		}

		int64_t Ternary()
		{
			const int64_t c = LogicalOr();
			if (Accept("?"))
			{
				const int64_t a = Ternary();
				if (!Accept(":"))
					m_error = true;
				const int64_t b = Ternary();
				return c ? a : b;
			}
			return c;
		}

		int64_t LogicalOr()
		{
			int64_t v = LogicalAnd();
			while (Accept("||"))
			{
				const int64_t r = LogicalAnd();
				v = (v || r) ? 1 : 0;
			}
			return v;
		}

		int64_t LogicalAnd()
		{
			int64_t v = BitOr();
			while (Accept("&&"))
			{
				const int64_t r = BitOr();
				v = (v && r) ? 1 : 0;
			}
			return v;
		}

		int64_t BitOr()
		{
			int64_t v = BitXor();
			while (Accept("|"))
				v |= BitXor();
			return v;
		}

		int64_t BitXor()
		{
			int64_t v = BitAnd();
			while (Accept("^"))
				v ^= BitAnd();
			return v;
		}

		int64_t BitAnd()
		{
			int64_t v = Equality();
			while (Accept("&"))
				v &= Equality();
			return v;
		}

		int64_t Equality()
		{
			int64_t v = Relational();
			for (;;)
			{
				if (Accept("=="))
					v = (v == Relational());
				else if (Accept("!="))
					v = (v != Relational());
				else
					return v;
			}
		}

		int64_t Relational()
		{
			int64_t v = Shift();
			for (;;)
			{
				if (Accept("<="))
					v = (v <= Shift());
				else if (Accept(">="))
					v = (v >= Shift());
				else if (Accept("<"))
					v = (v < Shift());
				else if (Accept(">"))
					v = (v > Shift());
				else
					return v;
			}
		}

		int64_t Shift()
		{
			int64_t v = Additive();
			for (;;)
			{
				if (Accept("<<"))
					v <<= Additive();
				else if (Accept(">>"))
					v >>= Additive();
				else
					return v;
			}
		}

		int64_t Additive()
		{
			int64_t v = Multiplicative();
			for (;;)
			{
				if (Accept("+"))
					v += Multiplicative();
				else if (Accept("-"))
					v -= Multiplicative();
				else
					return v;
			}
		}

		int64_t Multiplicative()
		{
			int64_t v = Unary();
			for (;;)
			{
				if (Accept("*"))
				{
					v *= Unary();
				}
				else if (Accept("/"))
				{
					const int64_t d = Unary();
					v = d ? v / d : 0;
				}
				else if (Accept("%"))
				{
					const int64_t d = Unary();
					v = d ? v % d : 0;
				}
				else
				{
					return v;
				}
			}
		}

		int64_t Unary()
		{
			if (Accept("!"))
				return !Unary();
			if (Accept("~"))
				return ~Unary();
			if (Accept("-"))
				return -Unary();
			if (Accept("+"))
				return Unary();
			return Primary();
		}

		int64_t Primary()
		{
			Skip();
			if (Accept("("))
			{
				const int64_t v = Ternary();
				if (!Accept(")"))
					m_error = true;
				return v;
			}
			if (m_pos < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_pos])))
			{
				const std::string num(m_s.substr(m_pos));
				char* end = nullptr;
				const int64_t v = static_cast<int64_t>(std::strtoll(num.c_str(), &end, 0));
				m_pos += static_cast<size_t>(end - num.c_str());
				while (m_pos < m_s.size() && (m_s[m_pos] == 'u' || m_s[m_pos] == 'U' || m_s[m_pos] == 'l' || m_s[m_pos] == 'L'))
					m_pos++;
				return v;
			}
			m_error = true;
			if (m_pos < m_s.size())
				m_pos++;
			return 0;
		}

		std::string_view m_s;
		size_t m_pos = 0;
		bool m_error = false;
	};

	bool Evaluate(std::string_view expr, const MacroMap& macros, bool* ok)
	{
		const std::string expanded = Expand(ReplaceDefined(expr, macros), macros, 32, true);
		int64_t value = 0;
		*ok = ExprParser(expanded).Parse(&value);
		return value != 0;
	}

	std::string_view Trim(std::string_view s)
	{
		while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
			s.remove_prefix(1);
		while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
			s.remove_suffix(1);
		return s;
	}

	std::string_view StripComment(std::string_view s)
	{
		const size_t c = s.find("//");
		return (c == std::string_view::npos) ? s : s.substr(0, c);
	}
} // namespace

std::string WGSLPreprocessor::Process(std::string_view source, const Macros& macros, std::string* error)
{
	MacroMap map;
	for (const auto& [name, value] : macros)
		map[name] = value;

	struct Cond
	{
		bool parent_active; ///< the enclosing block is emitted
		bool active; ///< this branch is emitted
		bool taken; ///< some branch of this #if was taken
	};
	std::vector<Cond> stack;
	auto active = [&stack]() { return stack.empty() || (stack.back().parent_active && stack.back().active); };

	std::string out;
	out.reserve(source.size());
	size_t line_no = 0;
	size_t pos = 0;
	auto fail = [&](const char* what) {
		if (error && error->empty())
			*error = std::string(what) + " at line " + std::to_string(line_no);
	};

	while (pos <= source.size())
	{
		const size_t end = source.find('\n', pos);
		const std::string_view line = source.substr(pos, (end == std::string_view::npos ? source.size() : end) - pos);
		pos = (end == std::string_view::npos) ? source.size() + 1 : end + 1;
		line_no++;

		const std::string_view t = Trim(line);
		if (!t.empty() && t[0] == '#')
		{
			std::string_view d = Trim(t.substr(1));
			size_t k = 0;
			while (k < d.size() && IsIdentChar(d[k]))
				k++;
			const std::string_view directive = d.substr(0, k);
			const std::string_view rest = Trim(StripComment(d.substr(k)));

			if (directive == "if" || directive == "ifdef" || directive == "ifndef")
			{
				const bool parent = active();
				bool value = false;
				if (parent)
				{
					if (directive == "if")
					{
						bool ok;
						value = Evaluate(rest, map, &ok);
						if (!ok)
							fail("bad #if expression");
					}
					else
					{
						value = (map.count(std::string(rest)) != 0) == (directive == "ifdef");
					}
				}
				stack.push_back({parent, value, value});
			}
			else if (directive == "elif")
			{
				if (stack.empty())
				{
					fail("#elif without #if");
					continue;
				}
				Cond& c = stack.back();
				if (c.taken || !c.parent_active)
				{
					c.active = false;
				}
				else
				{
					bool ok;
					c.active = Evaluate(rest, map, &ok);
					if (!ok)
						fail("bad #elif expression");
					c.taken = c.active;
				}
			}
			else if (directive == "else")
			{
				if (stack.empty())
				{
					fail("#else without #if");
					continue;
				}
				Cond& c = stack.back();
				c.active = !c.taken;
				c.taken = true;
			}
			else if (directive == "endif")
			{
				if (stack.empty())
					fail("#endif without #if");
				else
					stack.pop_back();
			}
			else if (directive == "define")
			{
				if (active())
				{
					size_t n = 0;
					while (n < rest.size() && IsIdentChar(rest[n]))
						n++;
					map[std::string(rest.substr(0, n))] = std::string(Trim(rest.substr(n)));
				}
			}
			else if (directive == "undef")
			{
				if (active())
					map.erase(std::string(rest));
			}
			else if (active())
			{
				fail("unknown directive");
			}

			out += '\n'; // keep line numbers for compilation messages
			continue;
		}

		if (active())
			out += Expand(line, map, 32, false);
		out += '\n';
	}

	if (!stack.empty())
		fail("unterminated #if");
	return out;
}
