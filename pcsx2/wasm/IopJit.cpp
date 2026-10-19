// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// IOP (R3000A) JIT for WebAssembly.
//
// Built like the EE JIT (EEJit.cpp): blocks of one 4 KB page compiled together into one module, compiled and
// instantiated synchronously on the EE thread (which runs the IOP) and called through its function table. A block
// runs from its entry to an unconditional jump, a branch the JIT leaves to the interpreter, the end of the page or
// a length cap; taken conditional branches leave it, branches not taken go on with their delay slot, as in the
// interpreter (pcsx2/R3000AInterpreter.cpp), whose behaviour the code reproduces exactly:
// - Cycles: each instruction adds one to psxRegs.cycle before it runs, as execI() does. The count is kept at
//   compile time over a local copy of the cycle at block entry, and written before anything that can read it.
// - Runs: intExecuteBlock() runs instructions until a taken branch has ended, or 0x10000 cycles without one. Run()
//   does the same with blocks, falling back to one instruction at a time near that limit so that it ends at the same
//   instruction.
// - Taken branches: psxJitBranchStart(), the delay slot, then psxJitBranchEnd(): the interpreter's own doBranch()
//   around its delay slot (the BIOS hooks, the idle loop skip, the event test).
// - Other instructions: ALU, shifts, multiplies, HI/LO and COP0 moves, and the loads and stores of main RAM are
//   emitted; everything else (divides, unaligned and I/O accesses, GTE, RFE, SYSCALL, BREAK, J to an import stub)
//   calls the interpreter's function with psxRegs.pc and psxRegs.code as execI() sets them. After such a call, a
//   changed PC (an exception) leaves the block.
// - Self-modifying code (module loads): each block checks, on entry, its instruction words against the values it
//   was compiled from, and is recompiled when they differ. Code that changes the rest of its own block while it
//   runs is not detected (the interpreter would see the new instruction); needs proper testing with games that do.

#include "Common.h"
#include "Config.h"
#include "IopMem.h"
#include "R3000A.h"

#include "wasm/IopJit.h"
#include "wasm/WasmEmitter.h"
#include "wasm/WasmJitRuntime.h"

#include "common/Console.h"
#include "common/Timer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace IopJit
{
	int g_mode = 1;
} // namespace IopJit

namespace
{
	using WasmJit::Function;
	using WasmJit::ValType;

	constexpr u32 kPageWords = 1024;
	constexpr u32 kMaxBlockInstrs = 256;
	constexpr u32 kMaxGroupBlocks = 32;
	constexpr size_t kMaxGroups = 3000;
	// A block runs at most kMaxBlockInstrs + 1 instructions (a delay slot after the last).
	constexpr u64 kRunLimit = 0x10000;
	constexpr u64 kGuardMargin = kMaxBlockInstrs + 8;

	using BlockFn = int (*)();

	struct Group
	{
		int module = 0;
		int instance = 0;
		int live = 0;
	};

	// Compiled blocks by IOP PC: s_pages[pc >> 12][(pc >> 2) & 1023] is a table slot, 0 when not compiled.
	int* s_pages[1u << 20];
	std::unordered_map<int, Group*> s_slot_group;
	std::vector<std::unique_ptr<Group>> s_groups;
	bool s_flush_requested = false;
	bool s_failed = false;
	const uptr* s_rlut = nullptr;
	const uptr* s_wlut = nullptr;

	u64 s_blocks_compiled = 0, s_groups_compiled = 0, s_invalidations = 0, s_flushes = 0, s_interp_steps = 0;
	u64 s_compile_ns = 0, s_wasm_bytes = 0, s_guest_instrs = 0, s_native_instrs = 0;

	template <typename T>
	u32 Addr(const T* p)
	{
		return static_cast<u32>(reinterpret_cast<uptr>(p));
	}
	template <typename F>
	u32 FnIndex(F* f)
	{
		return static_cast<u32>(reinterpret_cast<uptr>(f));
	}

	u32 GPR(u32 r) { return Addr(&psxRegs.GPR.r[r]); }

	enum class Kind
	{
		Fallback,
		SLL, SRL, SRA, SLLV, SRLV, SRAV,
		ADDU, SUBU, AND, OR, XOR, NOR, SLT, SLTU,
		MULT, MULTU, MFHI, MFLO, MTHI, MTLO,
		ADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI,
		MFC0, MTC0,
		LB, LBU, LH, LHU, LW, SB, SH, SW,
		// Branches.
		J, JAL, JR, JALR, BEQ, BNE, BLEZ, BGTZ, BLTZ, BGEZ, BLTZAL, BGEZAL,
	};

	Kind Decode(u32 code)
	{
		switch (code >> 26)
		{
			case 0:
				switch (code & 0x3f)
				{
					case 0: return Kind::SLL;
					case 2: return Kind::SRL;
					case 3: return Kind::SRA;
					case 4: return Kind::SLLV;
					case 6: return Kind::SRLV;
					case 7: return Kind::SRAV;
					case 8: return Kind::JR;
					case 9: return Kind::JALR;
					case 16: return Kind::MFHI;
					case 17: return Kind::MTHI;
					case 18: return Kind::MFLO;
					case 19: return Kind::MTLO;
					case 24: return Kind::MULT;
					case 25: return Kind::MULTU;
					case 32: case 33: return Kind::ADDU; // ADD: no overflow exception in the interpreter
					case 34: case 35: return Kind::SUBU;
					case 36: return Kind::AND;
					case 37: return Kind::OR;
					case 38: return Kind::XOR;
					case 39: return Kind::NOR;
					case 42: return Kind::SLT;
					case 43: return Kind::SLTU;
					default: return Kind::Fallback;
				}
			case 1:
				switch ((code >> 16) & 31)
				{
					case 0: return Kind::BLTZ;
					case 1: return Kind::BGEZ;
					case 16: return Kind::BLTZAL;
					case 17: return Kind::BGEZAL;
					default: return Kind::Fallback;
				}
			case 2: return Kind::J;
			case 3: return Kind::JAL;
			case 4: return Kind::BEQ;
			case 5: return Kind::BNE;
			case 6: return Kind::BLEZ;
			case 7: return Kind::BGTZ;
			case 8: case 9: return Kind::ADDIU; // ADDI: no overflow exception in the interpreter
			case 10: return Kind::SLTI;
			case 11: return Kind::SLTIU;
			case 12: return Kind::ANDI;
			case 13: return Kind::ORI;
			case 14: return Kind::XORI;
			case 15: return Kind::LUI;
			case 16:
				switch ((code >> 21) & 31)
				{
					case 0: case 2: return Kind::MFC0; // MFC0, CFC0
					case 4: case 6: return Kind::MTC0; // MTC0, CTC0
					default: return Kind::Fallback;
				}
			case 32: return Kind::LB;
			case 33: return Kind::LH;
			case 35: return Kind::LW;
			case 36: return Kind::LBU;
			case 37: return Kind::LHU;
			case 40: return Kind::SB;
			case 41: return Kind::SH;
			case 43: return Kind::SW;
			default: return Kind::Fallback;
		}
	}

	bool IsBranch(Kind k) { return k >= Kind::J; }

	// Jumps, and the branches that are always taken (B, BAL: BEQ/BLEZ/BGEZ/BGEZAL on $zero): the block ends there.
	bool AlwaysTaken(u32 code, Kind k)
	{
		const u32 rs = (code >> 21) & 31, rt = (code >> 16) & 31;
		switch (k)
		{
			case Kind::J: case Kind::JAL: case Kind::JR: case Kind::JALR: return true;
			case Kind::BEQ: return rs == rt;
			case Kind::BLEZ: case Kind::BGEZ: case Kind::BGEZAL: return rs == 0;
			default: return false;
		}
	}

	// execI() patches memory when it reaches 0x1630 with an IRX set: that instruction is always interpreted.
	bool IsIrxHookPc(u32 pc) { return pc == 0x1630 && EmuConfig.CurrentIRX.length() > 3; }

	// Host address of the code at pc (RAM or ROM: where execI()'s iopMemRead32() reads plain memory), or null.
	const u32* CodeHost(u32 pc)
	{
		const u32 m = pc & 0x1fffffff;
		const u32 t = m >> 16;
		if (!(t < 0x80 || (t >= 0x1fc0 && t < 0x2000)))
			return nullptr;
		const uptr p = s_rlut[t];
		return p ? reinterpret_cast<const u32*>(p + (m & 0xffff)) : nullptr;
	}

	class BlockCompiler
	{
	public:
		BlockCompiler(u32 type_v_v, u32 type_i_v, u32 page_vaddr, const u32* host_page)
			: m_tvv(type_v_v)
			, m_tiv(type_i_v)
			, m_page(page_vaddr)
			, m_host(host_page)
			, m_fn(0)
		{
			m_a = m_fn.local(ValType::I32);
			m_m = m_fn.local(ValType::I32);
			m_j = m_fn.local(ValType::I32);
			m_c0 = m_fn.local(ValType::I64);
			m_v = m_fn.local(ValType::I64);
		}

		// Compiles the block at `start`, and adds to `next` the PCs worth compiling with it (branch targets and
		// continuations in the same page).
		Function& Run(u32 start, std::vector<u32>* next)
		{
			Function& f = m_fn;
			const u32 end = Scan(start);
			EmitEntryChecks(start, end);
			f.i32_const(0);
			f.i64_load(Addr(&psxRegs.cycle));
			f.local_set(m_c0);

			u32 pc = start;
			u32 count = 0;
			for (;;)
			{
				if (pc >= m_page + 4096 || count >= kMaxBlockInstrs || (count && IsIrxHookPc(pc)))
				{
					if (pc < m_page + 4096)
						next->push_back(pc);
					ExitTo(pc);
					break;
				}
				const u32 code = Word(pc);
				const Kind k = Decode(code);
				count++;
				s_guest_instrs++;
				if (IsBranch(k))
				{
					if (NativeBranch(pc, code, k))
					{
						const bool ends = EmitBranch(pc, code, k, next);
						if (ends)
							break;
					}
					else
					{
						EmitFallback(pc, code, false, true);
						if (AlwaysTaken(code, k))
						{
							// Not taken only when irxImportExec() ran an import stub's function: on with the
							// delay slot, outside this block (what follows a stub is its table, not code).
							ExitTo(pc + 4);
							break;
						}
					}
					pc += 4;
					continue;
				}
				EmitInstruction(pc, code, k, false);
				pc += 4;
			}
			f.i32_const(0);
			return f;
		}

	private:
		u32 Word(u32 pc) const { return m_host[(pc - m_page) >> 2]; }

		// A branch the JIT emits: its delay slot is in the page, is not a branch and is not the IRX hook, and a J
		// is not to an import stub (psxJ() then calls irxImportExec(), which may skip the jump).
		bool NativeBranch(u32 pc, u32 code, Kind k) const
		{
			if (pc + 4 >= m_page + 4096 || IsIrxHookPc(pc + 4) || IsBranch(Decode(Word(pc + 4))))
				return false;
			if (k == Kind::J && (Word(pc + 4) >> 16) == 0x2400)
				return false;
			return true;
		}

		// The furthest word the block reads (exclusive), following the same rules as Run().
		u32 Scan(u32 start) const
		{
			u32 pc = start;
			u32 count = 0;
			for (;;)
			{
				if (pc >= m_page + 4096 || count >= kMaxBlockInstrs || (count && IsIrxHookPc(pc)))
					return pc;
				const u32 code = Word(pc);
				const Kind k = Decode(code);
				count++;
				// (A conditional branch's taken path reads the delay slot; not taken goes on with it.)
				if (IsBranch(k) && AlwaysTaken(code, k))
					return NativeBranch(pc, code, k) ? pc + 8 : pc + 4;
				pc += 4;
			}
		}

		void EmitEntryChecks(u32 start, u32 end)
		{
			Function& f = m_fn;
			f.block();
			f.block();
			const u32 host = Addr(m_host) + (start - m_page);
			const u32 nwords = (end - start) >> 2;
			u32 i = 0;
			for (; i + 2 <= nwords; i += 2)
			{
				u64 v;
				std::memcpy(&v, &m_host[((start - m_page) >> 2) + i], 8);
				f.i32_const(0);
				f.i64_load(host + i * 4, 2);
				f.i64_const(static_cast<s64>(v));
				f.i64_ne();
				f.br_if(0);
			}
			if (i < nwords)
			{
				f.i32_const(0);
				f.i32_load(host + i * 4);
				f.i32_const(static_cast<s32>(m_host[((start - m_page) >> 2) + i]));
				f.i32_ne();
				f.br_if(0);
			}
			f.br(1);
			f.end();
			f.i32_const(1);
			f.return_();
			f.end();
		}

		// ---- cycles: psxRegs.cycle is m_c0 + m_k ------------------------------------------------------------
		void StoreCycle()
		{
			Function& f = m_fn;
			f.i32_const(0);
			f.local_get(m_c0);
			if (m_k)
			{
				f.i64_const(m_k);
				f.i64_add();
			}
			f.i64_store(Addr(&psxRegs.cycle));
		}
		// After a call, which may have moved psxRegs.cycle.
		void ReloadCycle()
		{
			Function& f = m_fn;
			f.i32_const(0);
			f.i64_load(Addr(&psxRegs.cycle));
			if (m_k)
			{
				f.i64_const(m_k);
				f.i64_sub();
			}
			f.local_set(m_c0);
		}

		// ---- helpers ------------------------------------------------------------------------------------------
		void StorePcCode(u32 pc, u32 code)
		{
			Function& f = m_fn;
			f.i32_const(0);
			f.i32_const(static_cast<s32>(pc + 4));
			f.i32_store(Addr(&psxRegs.pc));
			f.i32_const(0);
			f.i32_const(static_cast<s32>(code));
			f.i32_store(Addr(&psxRegs.code));
		}

		void CallVoid(u32 fn)
		{
			m_fn.i32_const(static_cast<s32>(fn));
			m_fn.call_indirect(m_tvv, 0);
		}

		// Leaves the block (returning 0) if psxRegs.pc is not `expected`.
		void CheckPc(u32 expected)
		{
			Function& f = m_fn;
			f.i32_const(0);
			f.i32_load(Addr(&psxRegs.pc));
			f.i32_const(static_cast<s32>(expected));
			f.i32_ne();
			f.if_();
			f.i32_const(0);
			f.return_();
			f.end();
		}

		void ExitTo(u32 pc)
		{
			Function& f = m_fn;
			StoreCycle();
			f.i32_const(0);
			f.i32_const(static_cast<s32>(pc));
			f.i32_store(Addr(&psxRegs.pc));
			if (m_has_code)
			{
				f.i32_const(0);
				f.i32_const(static_cast<s32>(m_last_code));
				f.i32_store(Addr(&psxRegs.code));
			}
			f.i32_const(0);
			f.return_();
		}

		// The interpreter's function for the instruction (its cycle already counted).
		void CallInterpreter(u32 pc, u32 code)
		{
			StorePcCode(pc, code);
			StoreCycle();
			CallVoid(FnIndex(psxBSC[code >> 26]));
			ReloadCycle();
		}

		// A whole instruction through the interpreter. A branch (`branch`) that was taken has run its delay slot and
		// ended the run: leave. Out of a delay slot, an exception (the PC moved) leaves too.
		void EmitFallback(u32 pc, u32 code, bool in_delay_slot, bool branch = false)
		{
			Function& f = m_fn;
			m_k++;
			Note(code);
			CallInterpreter(pc, code);
			if (branch)
			{
				f.i32_const(0);
				f.i32_load8_u(Addr(psxJitBranchFlag()));
				f.if_();
				f.i32_const(0);
				f.return_();
				f.end();
			}
			if (!in_delay_slot)
				CheckPc(pc + 4);
		}

		void Note(u32 code)
		{
			m_last_code = code;
			m_has_code = true;
		}

		static u32 Rs(u32 c) { return (c >> 21) & 31; }
		static u32 Rt(u32 c) { return (c >> 16) & 31; }
		static u32 Rd(u32 c) { return (c >> 11) & 31; }
		static u32 Sa(u32 c) { return (c >> 6) & 31; }
		static s32 Imm(u32 c) { return static_cast<s16>(c & 0xffff); }

		void Load(u32 r) { m_fn.i32_const(0); m_fn.i32_load(GPR(r)); }

		// Emits one non-branch instruction.
		void EmitInstruction(u32 pc, u32 code, Kind k, bool in_delay_slot)
		{
			Function& f = m_fn;
			const u32 rs = Rs(code), rt = Rt(code), rd = Rd(code), sa = Sa(code);
			const s32 imm = Imm(code);

			// r = <i32 expression>, nothing when r is $zero (the interpreter's "if (!_Rd_) return;").
			auto set = [&](u32 r, auto&& expr) {
				if (r == 0)
					return;
				f.i32_const(0);
				expr();
				f.i32_store(GPR(r));
			};

			switch (k)
			{
				case Kind::SLL: case Kind::SRL: case Kind::SRA:
					set(rd, [&] {
						Load(rt);
						f.i32_const(sa);
						if (k == Kind::SLL) f.i32_shl(); else if (k == Kind::SRL) f.i32_shr_u(); else f.i32_shr_s();
					});
					break;
				case Kind::SLLV: case Kind::SRLV: case Kind::SRAV:
					// C++'s shift on wasm is the i32 shift (count mod 32), as the interpreter runs it.
					set(rd, [&] {
						Load(rt);
						Load(rs);
						if (k == Kind::SLLV) f.i32_shl(); else if (k == Kind::SRLV) f.i32_shr_u(); else f.i32_shr_s();
					});
					break;
				case Kind::ADDU: case Kind::SUBU: case Kind::AND: case Kind::OR: case Kind::XOR: case Kind::NOR:
				case Kind::SLT: case Kind::SLTU:
					set(rd, [&] {
						Load(rs);
						Load(rt);
						switch (k)
						{
							case Kind::ADDU: f.i32_add(); break;
							case Kind::SUBU: f.i32_sub(); break;
							case Kind::AND: f.i32_and(); break;
							case Kind::OR: f.i32_or(); break;
							case Kind::XOR: f.i32_xor(); break;
							case Kind::NOR: f.i32_or(); f.i32_const(-1); f.i32_xor(); break;
							case Kind::SLT: f.i32_lt_s(); break;
							default: f.i32_lt_u(); break;
						}
					});
					break;
				case Kind::MULT: case Kind::MULTU:
					f.i32_const(0);
					if (k == Kind::MULT) f.i64_load32_s(GPR(rs)); else f.i64_load32_u(GPR(rs));
					f.i32_const(0);
					if (k == Kind::MULT) f.i64_load32_s(GPR(rt)); else f.i64_load32_u(GPR(rt));
					f.i64_mul();
					f.local_set(m_v);
					// lo (r[33]) then hi (r[32]): one 64-bit store of hi:lo does not fit (hi is first)
					f.i32_const(0);
					f.local_get(m_v);
					f.i32_wrap_i64();
					f.i32_store(GPR(33));
					f.i32_const(0);
					f.local_get(m_v);
					f.i64_const(32);
					f.i64_shr_u();
					f.i32_wrap_i64();
					f.i32_store(GPR(32));
					break;
				case Kind::MFHI: set(rd, [&] { Load(32); }); break;
				case Kind::MFLO: set(rd, [&] { Load(33); }); break;
				case Kind::MTHI: f.i32_const(0); Load(rs); f.i32_store(GPR(32)); break;
				case Kind::MTLO: f.i32_const(0); Load(rs); f.i32_store(GPR(33)); break;

				case Kind::ADDIU: set(rt, [&] { Load(rs); f.i32_const(imm); f.i32_add(); }); break;
				case Kind::SLTI: set(rt, [&] { Load(rs); f.i32_const(imm); f.i32_lt_s(); }); break;
				case Kind::SLTIU: set(rt, [&] { Load(rs); f.i32_const(imm); f.i32_lt_u(); }); break;
				case Kind::ANDI: set(rt, [&] { Load(rs); f.i32_const(code & 0xffff); f.i32_and(); }); break;
				case Kind::ORI: set(rt, [&] { Load(rs); f.i32_const(code & 0xffff); f.i32_or(); }); break;
				case Kind::XORI: set(rt, [&] { Load(rs); f.i32_const(code & 0xffff); f.i32_xor(); }); break;
				case Kind::LUI: set(rt, [&] { f.i32_const(static_cast<s32>(code << 16)); }); break;

				case Kind::MFC0: set(rt, [&] { f.i32_const(0); f.i32_load(Addr(&psxRegs.CP0.r[rd])); }); break;
				case Kind::MTC0: f.i32_const(0); Load(rt); f.i32_store(Addr(&psxRegs.CP0.r[rd])); break;

				case Kind::LB: case Kind::LBU: case Kind::LH: case Kind::LHU: case Kind::LW:
				case Kind::SB: case Kind::SH: case Kind::SW:
					EmitMemory(pc, code, k, in_delay_slot);
					return;

				default:
					EmitFallback(pc, code, in_delay_slot);
					return;
			}
			m_k++;
			Note(code);
			s_native_instrs++;
		}

		// Main RAM (0x00000000-0x007fffff physical, the RAM and its mirrors) through psxMemRLUT/psxMemWLUT as
		// iopMemRead*/iopMemWrite* do; stores also need the cache not isolated (Status bit 16). Anything else, and
		// loads to $zero, calls the interpreter.
		void EmitMemory(u32 pc, u32 code, Kind k, bool in_delay_slot)
		{
			Function& f = m_fn;
			const u32 rs = Rs(code), rt = Rt(code);
			const bool store = k == Kind::SB || k == Kind::SH || k == Kind::SW;
			if (!store && rt == 0)
			{
				EmitFallback(pc, code, in_delay_slot);
				return;
			}
			m_k++;
			Note(code);
			s_native_instrs++;

			Load(rs);
			f.i32_const(Imm(code));
			f.i32_add();
			f.i32_const(0x1fffffff);
			f.i32_and();
			f.local_tee(m_m);
			f.i32_const(0x00800000);
			f.i32_lt_u();
			if (store)
			{
				f.i32_const(0);
				f.i32_load(Addr(&psxRegs.CP0.n.Status));
				f.i32_const(0x10000);
				f.i32_and();
				f.i32_eqz();
				f.i32_and();
			}
			f.if_();
			{
				// host = lut[m >> 16] + (m & 0xffff)
				f.local_get(m_m);
				f.i32_const(16);
				f.i32_shr_u();
				f.i32_const(2);
				f.i32_shl();
				f.i32_load(Addr(store ? s_wlut : s_rlut));
				f.local_get(m_m);
				f.i32_const(0xffff);
				f.i32_and();
				f.i32_add();
				if (store)
				{
					f.local_set(m_a);
					f.local_get(m_a);
					Load(rt);
					switch (k)
					{
						case Kind::SB: f.i32_store8(0); break;
						case Kind::SH: f.i32_store16(0, 0); break;
						default: f.i32_store(0, 0); break;
					}
				}
				else
				{
					f.local_set(m_a);
					f.i32_const(0);
					f.local_get(m_a);
					switch (k)
					{
						case Kind::LB: f.i32_load8_s(0); break;
						case Kind::LBU: f.i32_load8_u(0); break;
						case Kind::LH: f.i32_load16_s(0, 0); break;
						case Kind::LHU: f.i32_load16_u(0, 0); break;
						default: f.i32_load(0, 0); break;
					}
					f.i32_store(GPR(rt));
				}
			}
			f.else_();
			// I/O and the rest: the interpreter, which raises no exceptions for these.
			CallInterpreter(pc, code);
			f.end();
		}

		// Emits the branch at pc. Returns true if the block ends (an unconditional jump).
		bool EmitBranch(u32 pc, u32 code, Kind k, std::vector<u32>* next)
		{
			Function& f = m_fn;
			const u32 rs = Rs(code), rt = Rt(code), rd = Rd(code);
			const u32 btarget = pc + 4 + (Imm(code) << 2);
			const u32 jtarget = ((code & 0x03ffffff) << 2) + ((pc + 4) & 0xf0000000);
			m_k++;
			Note(code);
			s_native_instrs++;

			auto in_page = [&](u32 a) { return a >= m_page && a < m_page + 4096 && !(a & 3); };
			auto link = [&](u32 r) {
				f.i32_const(0);
				f.i32_const(static_cast<s32>(pc + 8));
				f.i32_store(GPR(r));
			};
			// Taken: doBranch() with the delay slot in between, then leave.
			auto taken = [&](bool from_local, u32 target) {
				const u32 saved_k = m_k;
				const u32 saved_code = m_last_code;
				auto push_target = [&] {
					if (from_local)
						f.local_get(m_j);
					else
						f.i32_const(static_cast<s32>(target));
				};
				push_target();
				f.i32_const(static_cast<s32>(FnIndex(&psxJitBranchStart)));
				f.call_indirect(m_tiv, 0);
				const u32 ds = pc + 4, ds_code = Word(ds);
				const Kind dk = Decode(ds_code);
				s_guest_instrs++;
				EmitInstruction(ds, ds_code, dk, true);
				if (dk != Kind::Fallback)
					StorePcCode(ds, ds_code); // what execI() leaves (a fallback set them itself, and may raise)
				StoreCycle();
				push_target();
				f.i32_const(static_cast<s32>(FnIndex(&psxJitBranchEnd)));
				f.call_indirect(m_tiv, 0);
				f.i32_const(0);
				f.return_();
				m_k = saved_k;
				m_last_code = saved_code;
			};

			switch (k)
			{
				case Kind::J:
				case Kind::JAL:
					if (k == Kind::JAL)
						link(31);
					taken(false, jtarget);
					if (in_page(jtarget))
						next->push_back(jtarget);
					if (in_page(pc + 8))
						next->push_back(pc + 8);
					return true;
				case Kind::JR:
				case Kind::JALR:
					// psxJALR(): the link first, then the target (rd may be rs).
					if (k == Kind::JALR && rd)
						link(rd);
					Load(rs);
					f.local_set(m_j);
					taken(true, 0);
					if (in_page(pc + 8))
						next->push_back(pc + 8);
					return true;
				default:
					break;
			}

			if (k == Kind::BLTZAL || k == Kind::BGEZAL)
				link(31); // before the condition, as psxBLTZAL()/psxBGEZAL()
			if (AlwaysTaken(code, k))
			{
				taken(false, btarget);
				if (in_page(btarget))
					next->push_back(btarget);
				if (in_page(pc + 8))
					next->push_back(pc + 8);
				return true;
			}
			Load(rs);
			switch (k)
			{
				case Kind::BEQ: Load(rt); f.i32_eq(); break;
				case Kind::BNE: Load(rt); f.i32_ne(); break;
				case Kind::BLEZ: f.i32_const(0); f.i32_le_s(); break;
				case Kind::BGTZ: f.i32_const(0); f.i32_gt_s(); break;
				case Kind::BLTZ: case Kind::BLTZAL: f.i32_const(0); f.i32_lt_s(); break;
				default: f.i32_const(0); f.i32_ge_s(); break;
			}
			f.if_();
			taken(false, btarget);
			f.end();
			if (in_page(btarget))
				next->push_back(btarget);
			return false;
		}

		u32 m_tvv, m_tiv;
		u32 m_page;
		const u32* m_host;
		Function m_fn;
		u32 m_a, m_m, m_j, m_c0, m_v;
		u32 m_k = 0; // instructions counted so far (psxRegs.cycle = m_c0 + m_k)
		u32 m_last_code = 0;
		bool m_has_code = false;
	};

	// ------------------------------------------------------------------------------------------------------------
	// Cache

	int& SlotRef(u32 pc)
	{
		int*& page = s_pages[pc >> 12];
		if (!page)
			page = new int[kPageWords]();
		return page[(pc >> 2) & (kPageWords - 1)];
	}

	void ReleaseSlot(int slot)
	{
		auto it = s_slot_group.find(slot);
		if (it == s_slot_group.end())
			return;
		Group* g = it->second;
		s_slot_group.erase(it);
		wasmjit_remove_function(slot);
		if (--g->live == 0)
		{
			wasmjit_release(g->instance);
			wasmjit_release(g->module);
			auto git = std::find_if(s_groups.begin(), s_groups.end(), [g](const auto& p) { return p.get() == g; });
			if (git != s_groups.end())
				s_groups.erase(git);
		}
	}

	void FlushAll()
	{
		for (int*& page : s_pages)
		{
			delete[] page;
			page = nullptr;
		}
		for (auto& [slot, g] : s_slot_group)
			wasmjit_remove_function(slot);
		s_slot_group.clear();
		for (auto& g : s_groups)
		{
			wasmjit_release(g->instance);
			wasmjit_release(g->module);
		}
		s_groups.clear();
		s_flushes++;
	}

	// Compiles the block at entry and its neighbours in the same page into one module. False if it cannot.
	bool CompileGroup(u32 entry)
	{
		if ((entry & 3) || IsIrxHookPc(entry))
			return false;
		const u32 page = entry & ~4095u;
		const u32* host = CodeHost(page);
		if (!host)
			return false;

		Common::Timer timer;
		WasmJit::Module m;
		const u32 t_v_v = m.type({}, {});
		const u32 t_i_v = m.type({ValType::I32}, {});
		const u32 t_v_i = m.type({}, {ValType::I32});
		m.import_memory("env", "mem", 0, 65536, true);
		m.import_table("env", "table", 0);

		std::vector<u32> pcs;
		std::vector<u32> work{entry};
		while (!work.empty() && pcs.size() < kMaxGroupBlocks)
		{
			const u32 pc = work.front();
			work.erase(work.begin());
			if (std::find(pcs.begin(), pcs.end(), pc) != pcs.end() || IsIrxHookPc(pc))
				continue;
			if (pc != entry && SlotRef(pc) != 0)
				continue;
			std::vector<u32> next;
			BlockCompiler bc(t_v_v, t_i_v, page, host);
			Function& fn = bc.Run(pc, &next);
			const u32 index = m.add_function(t_v_i, fn);
			char name[16];
			std::snprintf(name, sizeof(name), "b%u", static_cast<u32>(pcs.size()));
			m.export_function(name, index);
			pcs.push_back(pc);
			for (u32 n : next)
				work.push_back(n);
		}

		const std::vector<u8> bytes = m.finish();
		s_wasm_bytes += bytes.size();
		const int module = wasmjit_compile(bytes.data(), bytes.size());
		const int instance = module > 0 ? wasmjit_instantiate(module) : 0;
		if (module <= 0 || instance <= 0)
		{
			if (module > 0)
				wasmjit_release(module);
			if (module < 0 || instance < 0)
			{
				FlushAll(); // out of executable memory: start again with an empty cache
				return false;
			}
			Console.Error("IOP JIT: generated code for %08x did not compile; using the interpreter.", entry);
			s_failed = true;
			return false;
		}
		auto g = std::make_unique<Group>();
		g->module = module;
		g->instance = instance;
		for (u32 i = 0; i < pcs.size(); i++)
		{
			char name[16];
			std::snprintf(name, sizeof(name), "b%u", i);
			const int slot = wasmjit_add_function(instance, name);
			if (!slot)
				continue;
			int& ref = SlotRef(pcs[i]);
			if (ref)
				ReleaseSlot(ref);
			ref = slot;
			g->live++;
			s_slot_group.emplace(slot, g.get());
		}
		if (g->live == 0)
		{
			wasmjit_release(instance);
			wasmjit_release(module);
			return false;
		}
		s_blocks_compiled += pcs.size();
		s_groups_compiled++;
		s_groups.push_back(std::move(g));
		s_compile_ns += static_cast<u64>(timer.GetTimeNanoseconds());
		return true;
	}
} // namespace

void IopJit::RequestFlush()
{
	s_flush_requested = true;
}

void IopJit::Run(u64 last_cycle)
{
	if (s_failed)
	{
		while (!psxJitExecOne(last_cycle))
			;
		return;
	}
	if (s_flush_requested || s_groups.size() > kMaxGroups || s_rlut != psxMemRLUT || s_wlut != psxMemWLUT)
	{
		FlushAll();
		s_flush_requested = false;
		s_rlut = psxMemRLUT;
		s_wlut = psxMemWLUT;
	}

	const bool* const branched = psxJitBranchFlag();
	for (;;)
	{
		// Near the end of a run without a branch: one instruction at a time, so that it ends where it would.
		if (psxRegs.cycle - last_cycle >= kRunLimit - kGuardMargin) [[unlikely]]
		{
			s_interp_steps++;
			if (psxJitExecOne(last_cycle))
				return;
			continue;
		}
		const u32 pc = psxRegs.pc;
		int* const page = s_pages[pc >> 12];
		const int slot = page ? page[(pc >> 2) & (kPageWords - 1)] : 0;
		if (slot)
		{
			if (reinterpret_cast<BlockFn>(static_cast<uptr>(slot))() == 0) [[likely]]
			{
				if (*branched)
					return;
				continue;
			}
			// The code changed.
			s_invalidations++;
			ReleaseSlot(slot);
			page[(pc >> 2) & (kPageWords - 1)] = 0;
		}
		if (s_groups.size() > kMaxGroups)
			FlushAll();
		if (!CompileGroup(pc))
		{
			if (s_failed)
			{
				while (!psxJitExecOne(last_cycle))
					;
				return;
			}
			s_interp_steps++;
			if (psxJitExecOne(last_cycle))
				return;
		}
	}
}

void IopJit::PrintStats()
{
	Console.WriteLn("IOP JIT: %s, %llu blocks in %llu modules (%llu KB of wasm, %.1f ms to build and compile), "
					"%llu invalidations, %llu flushes, %llu interpreted steps; %llu of %llu instructions native",
		g_mode ? "on" : "off", static_cast<unsigned long long>(s_blocks_compiled),
		static_cast<unsigned long long>(s_groups_compiled), static_cast<unsigned long long>(s_wasm_bytes / 1024),
		s_compile_ns / 1e6, static_cast<unsigned long long>(s_invalidations), static_cast<unsigned long long>(s_flushes),
		static_cast<unsigned long long>(s_interp_steps), static_cast<unsigned long long>(s_native_instrs),
		static_cast<unsigned long long>(s_guest_instrs));
}
