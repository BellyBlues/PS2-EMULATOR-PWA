// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "VU1JitEmit.h"

#include "MTVU.h"
#include "VUops.h"
#include "WasmEmitter.h"

#include <emscripten/emscripten.h>

#include <cstddef>
#include <cstdlib>

extern bool vu1InterpMTVURunning;

namespace VU1Jit
{
	u64 g_block_base = 0;
	u64 g_block_base0 = 0;
	bool g_trace = false;
}

// Helpers generated code imports (WasmJitRuntime.js binds them as env.h_*). delta is VU1.cycle at the op relative to
// the block entry: the op bodies that read the cycle (XGKICK) and the kick transfer see the interpreter's value.
extern "C" EMSCRIPTEN_KEEPALIVE void vu1jit_h_kick(u32 delta)
{
	// _vuTestPipes' kick catch-up (VUops.cpp:203-209).
	VU1.cycle = VU1Jit::g_block_base + delta;
	if (VU1.xgkickenable)
		_vuXGKICKTransfer((VU1.cycle - VU1.xgkicklastcycle) - 1, false);
}

extern "C" EMSCRIPTEN_KEEPALIVE void vu1jit_h_upper(u32 code, u32 delta)
{
	VU1.cycle = VU1Jit::g_block_base + delta;
	VU1.code = code;
	VU1_UPPER_OPCODE[code & 0x3f]();
}

extern "C" EMSCRIPTEN_KEEPALIVE void vu1jit_h_lower(u32 code, u32 delta)
{
	VU1.cycle = VU1Jit::g_block_base + delta;
	VU1.code = code;
	VU1_LOWER_OPCODE[code >> 25]();
}

extern "C" EMSCRIPTEN_KEEPALIVE void vu1jit_h_upper0(u32 code, u32 delta)
{
	VU0.cycle = VU1Jit::g_block_base0 + delta;
	VU0.code = code;
	VU0_UPPER_OPCODE[code & 0x3f]();
}

extern "C" EMSCRIPTEN_KEEPALIVE void vu1jit_h_lower0(u32 code, u32 delta)
{
	VU0.cycle = VU1Jit::g_block_base0 + delta;
	VU0.code = code;
	VU0_LOWER_OPCODE[code >> 25]();
}

namespace VU1Jit
{
	using namespace WasmJit;
	using U = Upper;
	using L = Lower;

	static constexpr u32 BLOCK_CAP = 512; // instructions per block
	static constexpr u32 EXP = 0x7f800000u;
	static constexpr u32 SIGN = 0x80000000u;

	template <typename T>
	static u32 A(const T& x)
	{
		return static_cast<u32>(reinterpret_cast<uptr>(&x));
	}

	static PipeImage CaptureImage(const VURegs& m)
	{
		PipeImage p;
		std::memcpy(p.fmac, m.fmac, sizeof(p.fmac));
		p.fmacreadpos = m.fmacreadpos;
		p.fmacwritepos = m.fmacwritepos;
		p.fmaccount = m.fmaccount;
		p.fdiv = m.fdiv;
		p.efu = m.efu;
		std::memcpy(p.ialu, m.ialu, sizeof(p.ialu));
		p.ialureadpos = m.ialureadpos;
		p.ialuwritepos = m.ialuwritepos;
		p.ialucount = m.ialucount;
		p.VIBackupCycles = m.VIBackupCycles;
		p.VIRegNumber = m.VIRegNumber;
		p.cycle = m.cycle;
		return p;
	}

	void NormalizeRings(VURegs& R)
	{
		if (R.fmacreadpos)
		{
			fmacPipe t[4];
			for (u32 k = 0; k < 4; k++)
				t[k] = R.fmac[(R.fmacreadpos + k) & 3];
			std::memcpy(R.fmac, t, sizeof(t));
			R.fmacreadpos = 0;
			R.fmacwritepos = R.fmaccount & 3;
		}
		if (R.ialureadpos)
		{
			ialuPipe t[4];
			for (u32 k = 0; k < 4; k++)
				t[k] = R.ialu[(R.ialureadpos + k) & 3];
			std::memcpy(R.ialu, t, sizeof(t));
			R.ialureadpos = 0;
			R.ialuwritepos = R.ialucount & 3;
		}
	}

	void ApplyExit(VURegs& R, const BlockExit& x, u64 base)
	{
		const PipeImage& m = x.pipes;
		const auto rel = [base](u64 model_time) { return base + (model_time - TIMING_MODEL_BASE); };
		R.cycle = rel(m.cycle);

		// Entries pending since the entry (model times below the base) keep their own times; new ones get the model's.
		for (u32 i = 0; i < m.fmaccount; i++)
		{
			const u32 s = (m.fmacreadpos + i) & 3;
			const fmacPipe& p = m.fmac[s];
			if (p.sCycle < TIMING_MODEL_BASE)
				continue;
			fmacPipe& q = R.fmac[s];
			q.regupper = p.regupper;
			q.reglower = p.reglower;
			q.flagreg = p.flagreg;
			q.xyzwupper = p.xyzwupper;
			q.xyzwlower = p.xyzwlower;
			q.sCycle = rel(p.sCycle);
			q.Cycle = p.Cycle;
		}
		R.fmacreadpos = m.fmacreadpos;
		R.fmacwritepos = m.fmacwritepos;
		R.fmaccount = m.fmaccount;

		for (u32 i = 0; i < m.ialucount; i++)
		{
			const u32 s = (m.ialureadpos + i) & 3;
			const ialuPipe& p = m.ialu[s];
			if (p.sCycle < TIMING_MODEL_BASE)
				continue;
			R.ialu[s].reg = p.reg;
			R.ialu[s].sCycle = rel(p.sCycle);
			R.ialu[s].Cycle = p.Cycle;
		}
		R.ialureadpos = m.ialureadpos;
		R.ialuwritepos = m.ialuwritepos;
		R.ialucount = m.ialucount;

		R.fdiv.enable = m.fdiv.enable;
		if (m.fdiv.enable && m.fdiv.sCycle >= TIMING_MODEL_BASE)
		{
			R.fdiv.sCycle = rel(m.fdiv.sCycle);
			R.fdiv.Cycle = m.fdiv.Cycle;
		}
		R.efu.enable = m.efu.enable;
		if (m.efu.enable)
		{
			if (m.efu.sCycle >= TIMING_MODEL_BASE)
			{
				R.efu.sCycle = rel(m.efu.sCycle);
				R.efu.Cycle = m.efu.Cycle;
			}
			else
			{
				R.efu.Cycle += x.efu_cycle_adjust;
			}
		}

		R.VIBackupCycles = m.VIBackupCycles;
		R.VIRegNumber = m.VIRegNumber;
		if (!x.dynamic_tpc)
			R.VI[REG_TPC].UL = x.tpc;
		R.branch = 0;
		R.takedelaybranch = false;
		R.ebit = x.ebit;
		if (x.mbit)
			R.flags |= VUFLAG_MFLAGSET;
		if (x.kind == ExitKind::End)
		{
			if (&R == &VU1)
				vu1InterpMTVURunning = false;
			else
			{
				// _vu0Exec's program end (VU0microInterp.cpp:183-191).
				VU0.VI[REG_VPU_STAT].UL &= ~0x1;
				vif0Regs.stat.VEW = false;
			}
		}
	}

	namespace
	{
		struct Plan
		{
			u32 pc;
			Instruction ins;
			StepInfo info;
			Control pre_ctl;
			PipeImage pre; // the model before the instruction
			u32 cyc; // cycles from the entry to the start of the instruction
		};

		bool IsBranch(Lower op) { return (GetLowerFlags(op) & LOWER_BRANCH) != 0; }

		// Whether generated code may run this instruction in this control state (section 8).
		bool Allowed(const Plan& p, int vu)
		{
			const Instruction& ins = p.ins;
			if (!ins.IsValid() || ins.DBit() || ins.TBit())
				return false;
			const bool branch = !ins.IBit() && IsBranch(ins.lo);
			if (p.pre_ctl.branch != 0) // a branch delay slot
			{
				if (branch || ins.EBit() || (!ins.IBit() && ins.lo == L::XGKICK))
					return false;
			}
			if (p.pre_ctl.ebit != 0 && branch) // an E-bit delay slot
				return false;
			if (branch && (ins.EBit() || p.pc == (vu ? VU1_PROGSIZE : VU0_PROGSIZE) - 8))
				return false;
			// The M bit (VU0) ends the interpreter's Execute loop after the instruction: never with a branch pending.
			if (vu == 0 && ins.MBit() && (branch || p.pre_ctl.branch != 0))
				return false;
			return true;
		}

		class Emitter
		{
		public:
			Emitter(CompiledBlock* out, int vu)
				: m_out(out)
				, R(vu ? VU1 : VU0)
				, m_vu(vu)
				, m_f(1)
			{
				m_type_run = m_m.type({ValType::I32}, {ValType::I32});
				const u32 t1 = m_m.type({ValType::I32}, {});
				const u32 t2 = m_m.type({ValType::I32, ValType::I32}, {});
				m_m.import_memory("env", "mem", 0, 65536, true);
				m_h_kick = m_m.import_function("env", "h_kick", t1);
				m_h_upper = m_m.import_function("env", vu ? "h_upper" : "h_upper0", t2);
				m_h_lower = m_m.import_function("env", vu ? "h_lower" : "h_lower0", t2);
				for (u32& v : m_v)
					v = m_f.local(ValType::V128);
				for (u32& i : m_i)
					i = m_f.local(ValType::I32);
				m_hc_old = m_f.local(ValType::I32);
				m_hc_new = m_f.local(ValType::I32);
				m_hs = m_f.local(ValType::V128);
				m_hn = m_f.local(ValType::V128);
				m_mem = A(*R.Mem);
			}

			void EmitInstruction(const Plan& p, s32 busy_kick_exit);
			void FlushAllAtEnd(const Plan& p, const VURegs& after);
			void EmitBranchEnd(const Plan& branch, const Plan& delay, u32 exit_taken, u32 exit_fall, u32 exit_dyn);
			void EmitReturn(u32 exit_id)
			{
				m_f.i32_const(static_cast<s32>(exit_id));
				m_f.return_();
			}
			void Finish()
			{
				m_f.i32_const(0); // unreachable: every path returns
				m_m.export_function("run", m_m.add_function(m_type_run, m_f));
				m_out->wasm = m_m.finish();
			}

		private:
			// Memory access at absolute addresses.
			void LdV(u32 addr) { m_f.i32_const(0); m_f.v128_load(addr); }
			void Ld32(u32 addr) { m_f.i32_const(0); m_f.i32_load(addr); }
			void Ld16u(u32 addr) { m_f.i32_const(0); m_f.i32_load16_u(addr); }
			void Ld16s(u32 addr) { m_f.i32_const(0); m_f.i32_load16_s(addr); }
			template <typename F> void StV(u32 addr, F&& value) { m_f.i32_const(0); value(); m_f.v128_store(addr); }
			template <typename F> void St32(u32 addr, F&& value) { m_f.i32_const(0); value(); m_f.i32_store(addr); }
			template <typename F> void St16(u32 addr, F&& value) { m_f.i32_const(0); value(); m_f.i32_store16(addr); }
			void St32c(u32 addr, u32 v) { St32(addr, [&] { m_f.i32_const(static_cast<s32>(v)); }); }
			void Splat(u32 v) { m_f.v128_pooled_i32x4(v, v, v, v); }
			void Simd(Simd o) { m_f.simd(o); }
			static u32 DestMask(u32 dest, u32 lane) { return (dest & (8 >> lane)) ? 0xffffffffu : 0; }
			void DestConst(u32 dest)
			{
				m_f.v128_pooled_i32x4(DestMask(dest, 0), DestMask(dest, 1), DestMask(dest, 2), DestMask(dest, 3));
			}
			// Stores the vector in local v to VF/ACC at addr with the dest mask.
			void StoreMerged(u32 addr, u32 v, u32 dest)
			{
				if ((dest & 0xf) == 0)
					return;
				StV(addr, [&] {
					m_f.local_get(v);
					if ((dest & 0xf) != 0xf)
					{
						LdV(addr);
						DestConst(dest);
						Simd(Simd::V128Bitselect);
					}
				});
			}

			void Clamp(u32 v); // local v = vuDouble(v)
			void Flags(u32 r, u32 dest); // MAC and status from the raw result in local r (VU_MAC_UPDATE, VU_STAT_UPDATE)
			void Flushes(const Plan& p);
			void FmacWriteback(u32 slot, int flagreg);
			void KickCatchUp(u32 delta);
			bool Upper(const Plan& p, u32 delta);
			bool Lower(const Plan& p, u32 delta);
			void Backup(const Plan& p);
			void HelperUpper(const Plan& p, u32 delta)
			{
				m_f.i32_const(static_cast<s32>(p.ins.upper));
				m_f.i32_const(static_cast<s32>(delta));
				m_f.call(m_h_upper);
				m_out->helper_ops++;
			}
			void HelperLower(const Plan& p, u32 delta)
			{
				// Ops that call _vuBackupVI read the window state; the model knows it.
				if (BackupVIRegister(p.ins) >= 0)
				{
					m_f.i32_const(0);
					m_f.i32_const(p.info.vi_backup_cycles);
					m_f.i32_store8(A(R.VIBackupCycles));
					St32c(A(R.VIRegNumber), p.info.vi_backup_cycles ? p.info.vi_backup_reg : p.pre.VIRegNumber);
				}
				m_f.i32_const(static_cast<s32>(p.ins.lower));
				m_f.i32_const(static_cast<s32>(delta));
				m_f.call(m_h_lower);
				m_out->helper_ops++;
			}

			CompiledBlock* m_out;
			VURegs& R;
			int m_vu;
			Module m_m;
			Function m_f;
			u32 m_type_run;
			u32 m_h_kick, m_h_upper, m_h_lower;
			u32 m_v[8];
			u32 m_i[6];
			u32 m_hs, m_hn, m_hc_old, m_hc_new; // the upper/lower hazard copies
			u32 m_mem;
		};

		void Emitter::Clamp(u32 v)
		{
			// vuDouble (VUops.cpp:443-459) with the overflow clamp on, and VU_MAC_UPDATE's result (VUflags.cpp:15-46):
			// exponent 0 gives the signed zero, exponent 255 the signed maximum.
			const u32 e = m_v[6], sgn = m_v[7];
			m_f.local_get(v);
			Splat(EXP);
			Simd(Simd::V128And);
			m_f.local_set(e);
			m_f.local_get(v);
			Splat(SIGN);
			Simd(Simd::V128And);
			m_f.local_tee(sgn);
			Splat(0x7f7fffffu);
			Simd(Simd::V128Or);
			m_f.local_get(v);
			m_f.local_get(e);
			Splat(EXP);
			Simd(Simd::I32x4Eq);
			Simd(Simd::V128Bitselect); // exponent 255 ? sign | max : v
			m_f.local_set(v);
			m_f.local_get(sgn);
			m_f.local_get(v);
			m_f.local_get(e);
			Splat(0);
			Simd(Simd::I32x4Eq);
			Simd(Simd::V128Bitselect); // exponent 0 ? sign : ...
			m_f.local_set(v);
		}

		void Emitter::Flags(u32 r, u32 dest)
		{
			// Per lane of the raw result: zero (exponent 0), sign, underflow (exponent 0, mantissa not 0), overflow
			// (exponent 255). Lanes outside dest are cleared (VU_MACx_CLEAR). MAC bit order: x is bit 3 of each nibble.
			const u32 e = m_v[6], zm = m_v[7], sm = m_v[5], n = m_v[4];
			const bool all = (dest & 0xf) == 0xf;
			m_f.local_get(r);
			Splat(EXP);
			Simd(Simd::V128And);
			m_f.local_set(e);
			// zero
			m_f.local_get(e);
			Splat(0);
			Simd(Simd::I32x4Eq);
			if (!all)
			{
				DestConst(dest);
				Simd(Simd::V128And);
			}
			m_f.local_set(zm);
			// sign
			m_f.local_get(r);
			m_f.i32_const(31);
			Simd(Simd::I32x4ShrS);
			if (!all)
			{
				DestConst(dest);
				Simd(Simd::V128And);
			}
			m_f.local_set(sm);
			m_f.local_get(zm);
			m_f.local_get(sm);
			Simd(Simd::I16x8NarrowI32x4S);
			// underflow: zero and a mantissa
			m_f.local_get(zm);
			m_f.local_get(r);
			Splat(0x007fffffu);
			Simd(Simd::V128And);
			Splat(0);
			Simd(Simd::I32x4Eq);
			Simd(Simd::V128AndNot);
			// overflow
			m_f.local_get(e);
			Splat(EXP);
			Simd(Simd::I32x4Eq);
			if (!all)
			{
				DestConst(dest);
				Simd(Simd::V128And);
			}
			Simd(Simd::I16x8NarrowI32x4S);
			Simd(Simd::I8x16NarrowI16x8S); // bytes: zero xyzw, sign xyzw, underflow xyzw, overflow xyzw
			m_f.local_set(n);

			// macflag = (macflag & ~0xffff) | mac
			St32(A(R.macflag), [&] {
				Ld32(A(R.macflag));
				m_f.i32_const(static_cast<s32>(0xffff0000u));
				m_f.i32_and();
				m_f.local_get(n);
				m_f.local_get(n);
				static constexpr u8 rev[16] = {3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12};
				m_f.shuffle(rev);
				Simd(Simd::I8x16Bitmask);
				m_f.i32_or();
			});
			// statusflag = one bit per group with any lane set (VU_STAT_UPDATE)
			St32(A(R.statusflag), [&] {
				m_f.local_get(n);
				Splat(0);
				Simd(Simd::I32x4Ne);
				Simd(Simd::I32x4Bitmask);
			});
		}

		void Emitter::FmacWriteback(u32 slot, int flagreg)
		{
			// _vuFMACflush / _vuFlushAll for one entry (VUops.cpp:51-63).
			const fmacPipe& e = R.fmac[slot];
			if (flagreg & (1 << REG_CLIP_FLAG))
				St32(A(R.VI[REG_CLIP_FLAG]), [&] { Ld32(A(e.clipflag)); });
			const u32 st = A(R.VI[REG_STATUS_FLAG]);
			St32(st, [&] {
				const u32 sf = m_i[0];
				Ld32(A(e.statusflag));
				m_f.local_set(sf);
				Ld32(st);
				if (flagreg & (1 << REG_STATUS_FLAG))
				{
					m_f.i32_const(0x30);
					m_f.i32_and();
					m_f.local_get(sf);
					m_f.i32_const(0xFC0);
					m_f.i32_and();
					m_f.i32_or();
					m_f.local_get(sf);
					m_f.i32_const(0xF);
					m_f.i32_and();
					m_f.i32_or();
				}
				else
				{
					m_f.i32_const(0xFF0);
					m_f.i32_and();
					m_f.local_get(sf);
					m_f.i32_const(0xF);
					m_f.i32_and();
					m_f.local_tee(sf);
					m_f.i32_or();
					m_f.local_get(sf);
					m_f.i32_const(6);
					m_f.i32_shl();
					m_f.i32_or();
				}
			});
			St32(A(R.VI[REG_MAC_FLAG]), [&] { Ld32(A(e.macflag)); });
		}

		void Emitter::Flushes(const Plan& p)
		{
			// _vuTestPipes (VUops.cpp:188-209): FMAC entries in ring order, then FDIV, then EFU (IALU has no data).
			for (u32 k = 0; k < p.info.fmac_flushed; k++)
			{
				const u32 slot = (p.pre.fmacreadpos + k) & 3;
				FmacWriteback(slot, p.pre.fmac[slot].flagreg);
			}
			if (p.info.fdiv_flushed)
			{
				St32(A(R.VI[REG_Q]), [&] { Ld32(A(R.fdiv.reg)); });
				const u32 st = A(R.VI[REG_STATUS_FLAG]);
				St32(st, [&] {
					Ld32(st);
					m_f.i32_const(0xFCF);
					m_f.i32_and();
					Ld32(A(R.fdiv.statusflag));
					m_f.i32_const(0xC30);
					m_f.i32_and();
					m_f.i32_or();
				});
			}
			if (p.info.efu_flushed)
				St32(A(R.VI[REG_P]), [&] { Ld32(A(R.efu.reg)); });
		}

		void Emitter::FlushAllAtEnd(const Plan& p, const VURegs& after)
		{
			// _vuFlushAll (VUops.cpp:130-186).
			if (p.info.end_fdiv)
			{
				St32(A(R.VI[REG_Q]), [&] { Ld32(A(R.fdiv.reg)); });
				const u32 st = A(R.VI[REG_STATUS_FLAG]);
				St32(st, [&] {
					Ld32(st);
					m_f.i32_const(0xFCF);
					m_f.i32_and();
					Ld32(A(R.fdiv.statusflag));
					m_f.i32_const(0xC30);
					m_f.i32_and();
					m_f.i32_or();
				});
			}
			if (p.info.end_efu)
				St32(A(R.VI[REG_P]), [&] { Ld32(A(R.efu.reg)); });
			for (u32 k = 0; k < p.info.end_fmac_count; k++)
			{
				const u32 slot = (p.info.end_fmac_read + k) & 3;
				FmacWriteback(slot, after.fmac[slot].flagreg);
			}
		}

		void Emitter::KickCatchUp(u32 delta)
		{
			Ld32(A(R.xgkickenable));
			m_f.if_();
			m_f.i32_const(static_cast<s32>(delta));
			m_f.call(m_h_kick);
			m_f.end();
		}

		static int DebugMask()
		{
			static const int mask = [] { const char* e = std::getenv("VU1JIT_HELPERS"); return e ? std::atoi(e) : 0; }();
			return mask;
		}

		bool Emitter::Upper(const Plan& p, u32 delta)
		{
			if ((DebugMask() & 1) && p.ins.up != U::NOP)
			{
				HelperUpper(p, delta);
				return true;
			}
			const u32 code = p.ins.upper;
			const u32 fd = (code >> 6) & 0x1f, fs = (code >> 11) & 0x1f, ft = (code >> 16) & 0x1f;
			const u32 dest = (code >> 21) & 0xf;
			enum K { NONE, ADD, SUB, MUL, MADD, MSUB } k = NONE;
			bool acc = false;
			int bc = -1; // 0-3 lane of ft, 4 I, 5 Q, -1 vector
			switch (p.ins.up)
			{
				case U::NOP: return true;
#define VU1JIT_BC(name, kind, isacc) \
	case U::name##x: k = kind; acc = isacc; bc = 0; break; \
	case U::name##y: k = kind; acc = isacc; bc = 1; break; \
	case U::name##z: k = kind; acc = isacc; bc = 2; break; \
	case U::name##w: k = kind; acc = isacc; bc = 3; break; \
	case U::name##q: k = kind; acc = isacc; bc = 5; break; \
	case U::name: k = kind; acc = isacc; bc = -1; break;
				VU1JIT_BC(ADD, ADD, false)
				VU1JIT_BC(SUB, SUB, false)
				VU1JIT_BC(MUL, MUL, false)
				VU1JIT_BC(MADD, MADD, false)
				VU1JIT_BC(MSUB, MSUB, false)
				VU1JIT_BC(ADDA, ADD, true)
				VU1JIT_BC(SUBA, SUB, true)
				VU1JIT_BC(MULA, MUL, true)
				VU1JIT_BC(MADDA, MADD, true)
				VU1JIT_BC(MSUBA, MSUB, true)
#undef VU1JIT_BC
				case U::ADDi:
					if (CHECK_VUADDSUBHACK)
						break; // vuADD_TriAceHack
					k = ADD; bc = 4; break;
				case U::SUBi: k = SUB; bc = 4; break;
				case U::MULi: k = MUL; bc = 4; break;
				case U::MADDi: k = MADD; bc = 4; break;
				case U::MSUBi: k = MSUB; bc = 4; break;
				case U::ADDAi: k = ADD; acc = true; bc = 4; break;
				case U::SUBAi: k = SUB; acc = true; bc = 4; break;
				case U::MULAi: k = MUL; acc = true; bc = 4; break;
				case U::MADDAi: k = MADD; acc = true; bc = 4; break;
				case U::MSUBAi: k = MSUB; acc = true; bc = 4; break;
				case U::FTOI0: case U::FTOI4: case U::FTOI12: case U::FTOI15:
				{
					// floatToInt (VUops.cpp:878-891)
					if (ft == 0)
						return true;
					const u32 off = (p.ins.up == U::FTOI0) ? 0 : (p.ins.up == U::FTOI4) ? 4 : (p.ins.up == U::FTOI12) ? 12 : 15;
					const u32 u = m_v[0], r = m_v[1];
					LdV(A(R.VF[fs]));
					if (off)
					{
						Splat(0x3f800000u + (off << 23));
						Simd(Simd::F32x4Mul);
					}
					m_f.local_tee(u);
					Splat(0x7fffffffu);
					m_f.local_get(u);
					m_f.i32_const(31);
					Simd(Simd::I32x4ShrU);
					Simd(Simd::I32x4Add); // the saturated value by sign
					m_f.local_get(u);
					Simd(Simd::I32x4TruncSatF32x4S);
					m_f.local_get(u);
					Splat(EXP);
					Simd(Simd::V128And);
					Splat(0x4f000000u);
					Simd(Simd::I32x4GeU);
					Simd(Simd::V128Bitselect);
					m_f.local_set(r);
					StoreMerged(A(R.VF[ft]), r, dest);
					m_out->native_ops++;
					return true;
				}
				case U::ITOF0: case U::ITOF4: case U::ITOF12: case U::ITOF15:
				{
					// intToFloat (VUops.cpp:898-904)
					if (ft == 0)
						return true;
					const u32 off = (p.ins.up == U::ITOF0) ? 0 : (p.ins.up == U::ITOF4) ? 4 : (p.ins.up == U::ITOF12) ? 12 : 15;
					const u32 r = m_v[1];
					LdV(A(R.VF[fs]));
					Simd(Simd::F32x4ConvertI32x4S);
					if (off)
					{
						Splat(0x3f800000u - (off << 23));
						Simd(Simd::F32x4Mul);
					}
					m_f.local_set(r);
					StoreMerged(A(R.VF[ft]), r, dest);
					m_out->native_ops++;
					return true;
				}
				case U::ABS:
				{
					if (ft == 0)
						return true;
					const u32 r = m_v[1];
					LdV(A(R.VF[fs]));
					Splat(0x7fffffffu);
					Simd(Simd::V128And);
					m_f.local_set(r);
					StoreMerged(A(R.VF[ft]), r, dest);
					m_out->native_ops++;
					return true;
				}
				case U::CLIP:
				{
					// _vuCLIP (VUops.cpp:912-928)
					const u32 w = m_i[0], pm = m_i[1], nm = m_i[2], fsv = m_v[0];
					Ld32(A(R.VF[ft].i.w));
					m_f.local_tee(w);
					m_f.i32_const(0x7fffffff);
					m_f.i32_and();
					m_f.i32_const(0x007fffff);
					m_f.local_get(w);
					m_f.i32_const(static_cast<s32>(EXP));
					m_f.i32_and();
					m_f.select();
					Simd(Simd::I32x4Splat);
					m_f.local_set(m_v[1]);
					LdV(A(R.VF[fs]));
					m_f.local_tee(fsv);
					m_f.local_get(m_v[1]);
					Simd(Simd::I32x4GtS);
					Simd(Simd::I32x4Bitmask);
					m_f.local_set(pm);
					m_f.local_get(fsv);
					Splat(SIGN);
					Simd(Simd::V128Xor);
					m_f.local_get(m_v[1]);
					Simd(Simd::I32x4GtS);
					Simd(Simd::I32x4Bitmask);
					m_f.local_set(nm);
					St32(A(R.clipflag), [&] {
						Ld32(A(R.clipflag));
						m_f.i32_const(6);
						m_f.i32_shl();
						// pos x,y,z -> bits 0,2,4; neg -> 1,3,5
						for (u32 lane = 0; lane < 3; lane++)
						{
							m_f.local_get(pm);
							m_f.i32_const(1 << lane);
							m_f.i32_and();
							if (lane)
							{
								m_f.i32_const(static_cast<s32>(lane));
								m_f.i32_shl();
							}
							m_f.i32_or();
							m_f.local_get(nm);
							m_f.i32_const(1 << lane);
							m_f.i32_and();
							m_f.i32_const(static_cast<s32>(lane + 1));
							m_f.i32_shl();
							m_f.i32_or();
						}
						m_f.i32_const(0xFFFFFF);
						m_f.i32_and();
					});
					m_out->native_ops++;
					return true;
				}
				default: break;
			}
			if (k == NONE)
			{
				HelperUpper(p, delta);
				return true;
			}

			// applyBinaryMACOp / applyTernaryMACOp and their broadcasts (VUops.cpp:516-552, 681-710).
			const u32 a = m_v[0], b = m_v[1], c = m_v[2], r = m_v[3];
			LdV(A(R.VF[fs]));
			m_f.local_set(a);
			Clamp(a);
			if (bc < 0)
				LdV(A(R.VF[ft]));
			else
			{
				m_f.i32_const(0);
				m_f.simd_mem(Simd::V128Load32Splat, (bc < 4) ? A(R.VF[ft].UL[bc]) : A(R.VI[(bc == 4) ? REG_I : REG_Q]), 2);
			}
			m_f.local_set(b);
			Clamp(b);
			if (k == MADD || k == MSUB)
			{
				LdV(A(R.ACC));
				m_f.local_set(c);
				Clamp(c);
				m_f.local_get(c);
				m_f.local_get(a);
				m_f.local_get(b);
				Simd(Simd::F32x4Mul);
				Simd(k == MADD ? Simd::F32x4Add : Simd::F32x4Sub);
			}
			else
			{
				m_f.local_get(a);
				m_f.local_get(b);
				Simd(k == ADD ? Simd::F32x4Add : k == SUB ? Simd::F32x4Sub : Simd::F32x4Mul);
			}
			m_f.local_set(r);
			Flags(r, dest);
			if (acc || fd != 0) // fd 0 writes RDzero, which nothing reads
			{
				Clamp(r);
				StoreMerged(acc ? A(R.ACC) : A(R.VF[fd]), r, dest);
			}
			m_out->native_ops++;
			return true;
		}

		void Emitter::Backup(const Plan& p)
		{
			// _vuBackupVI (VUops.cpp:420-434): the window's cycles and register are the model's; only the value is data.
			if (p.info.backup_vi < 0 || p.info.backup_chained)
				return;
			St32(A(R.VIOldValue), [&] { Ld16u(A(R.VI[p.info.backup_vi])); });
		}

		bool Emitter::Lower(const Plan& p, u32 delta)
		{
			const u32 code = p.ins.lower;
			const u32 ft = (code >> 16) & 0x1f, fs = (code >> 11) & 0x1f;
			const u32 it = ft & 0xf, is = fs & 0xf, id = ((code >> 6) & 0x1f) & 0xf;
			const u32 dest = (code >> 21) & 0xf;
			const s32 imm11 = (code & 0x400) ? static_cast<s32>(0xfffffc00u | (code & 0x3ff)) : static_cast<s32>(code & 0x3ff);
			const s32 imm15 = static_cast<s32>(((code >> 10) & 0x7800) | (code & 0x7ff));
			const u32 v = m_v[0];
			const auto vi_op = [&](u32 dst, auto&& value) {
				Backup(p);
				St16(A(R.VI[dst]), value);
				m_out->native_ops++;
			};
			// VU memory address of a qword index on the stack (GET_VU_MEM for VU1: & 0x3fff after * 16).
			const auto qaddr = [&] {
				m_f.i32_const(4);
				m_f.i32_shl();
				m_f.i32_const(0x3ff0);
				m_f.i32_and();
			};
			const auto load_q = [&](u32 to_vf) { // VF[to_vf] <- mem[addr on stack], dest merged
				m_f.v128_load(m_mem);
				m_f.local_set(v);
				StoreMerged(A(R.VF[to_vf]), v, dest);
			};
			const auto store_q = [&](u32 from_vf) { // mem[addr in m_i[0]] <- VF[from_vf], dest merged
				if (dest == 0)
					return;
				m_f.local_get(m_i[0]);
				LdV(A(R.VF[from_vf]));
				if (dest != 0xf)
				{
					m_f.local_get(m_i[0]);
					m_f.v128_load(m_mem);
					DestConst(dest);
					Simd(Simd::V128Bitselect);
				}
				m_f.v128_store(m_mem);
			};

			const bool vu0_memory = m_vu == 0 && ((GetLowerFlags(p.ins.lo) & LOWER_STORE) || p.ins.lo == L::LQ ||
				p.ins.lo == L::LQI || p.ins.lo == L::LQD || p.ins.lo == L::ILW || p.ins.lo == L::ILWR);
			if (((DebugMask() & 2) && !IsBranch(p.ins.lo)) || vu0_memory) // VU0's 0x4000 mapping: GET_VU_MEM
			{
				HelperLower(p, delta);
				return true;
			}
			switch (p.ins.lo)
			{
				case L::MOVE:
					if (ft == 0 || dest == 0)
						return true;
					LdV(A(R.VF[fs]));
					m_f.local_set(v);
					StoreMerged(A(R.VF[ft]), v, dest);
					m_out->native_ops++;
					return true;
				case L::MR32:
				{
					if (ft == 0)
						return true;
					static constexpr u8 rot[16] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3};
					LdV(A(R.VF[fs]));
					m_f.shuffle1(rot, v);
					m_f.local_set(v);
					StoreMerged(A(R.VF[ft]), v, dest);
					m_out->native_ops++;
					return true;
				}
				case L::LQ:
					if (ft == 0)
						return true;
					Ld16s(A(R.VI[is]));
					m_f.i32_const(imm11);
					m_f.i32_add();
					qaddr();
					load_q(ft);
					m_out->native_ops++;
					return true;
				case L::SQ:
					Ld16s(A(R.VI[it]));
					m_f.i32_const(imm11);
					m_f.i32_add();
					qaddr();
					m_f.local_set(m_i[0]);
					store_q(fs);
					m_out->native_ops++;
					return true;
				case L::LQI:
					Backup(p);
					if (ft)
					{
						Ld16u(A(R.VI[is]));
						qaddr();
						load_q(ft);
					}
					if (fs != 0)
						St16(A(R.VI[is]), [&] { Ld16u(A(R.VI[is])); m_f.i32_const(1); m_f.i32_add(); });
					m_out->native_ops++;
					return true;
				case L::LQD:
					Backup(p);
					if (is != 0)
						St16(A(R.VI[is]), [&] { Ld16u(A(R.VI[is])); m_f.i32_const(1); m_f.i32_sub(); });
					if (ft)
					{
						Ld16u(A(R.VI[is]));
						qaddr();
						load_q(ft);
					}
					m_out->native_ops++;
					return true;
				case L::SQI:
					Backup(p);
					Ld16u(A(R.VI[it]));
					qaddr();
					m_f.local_set(m_i[0]);
					store_q(fs);
					if (ft != 0)
						St16(A(R.VI[it]), [&] { Ld16u(A(R.VI[it])); m_f.i32_const(1); m_f.i32_add(); });
					m_out->native_ops++;
					return true;
				case L::SQD:
					Backup(p);
					if (ft != 0)
						St16(A(R.VI[it]), [&] { Ld16u(A(R.VI[it])); m_f.i32_const(1); m_f.i32_sub(); });
					Ld16u(A(R.VI[it]));
					qaddr();
					m_f.local_set(m_i[0]);
					store_q(fs);
					m_out->native_ops++;
					return true;
				case L::IADD:
				case L::ISUB:
				case L::IAND:
				case L::IOR:
					if (id == 0)
						return true;
					vi_op(id, [&] {
						Ld16u(A(R.VI[is]));
						Ld16u(A(R.VI[it]));
						switch (p.ins.lo)
						{
							case L::IADD: m_f.i32_add(); break;
							case L::ISUB: m_f.i32_sub(); break;
							case L::IAND: m_f.i32_and(); break;
							default: m_f.i32_or(); break;
						}
					});
					return true;
				case L::IADDI:
				{
					if (it == 0)
						return true;
					const s32 imm5 = static_cast<s32>(((code >> 6) & 0x10) ? (0xfff0u | ((code >> 6) & 0xf)) : ((code >> 6) & 0xf));
					vi_op(it, [&] { Ld16u(A(R.VI[is])); m_f.i32_const(static_cast<s16>(imm5)); m_f.i32_add(); });
					return true;
				}
				case L::IADDIU:
				case L::ISUBIU:
					if (it == 0)
						return true;
					vi_op(it, [&] {
						Ld16u(A(R.VI[is]));
						m_f.i32_const(imm15);
						if (p.ins.lo == L::IADDIU)
							m_f.i32_add();
						else
							m_f.i32_sub();
					});
					return true;
				case L::FCAND:
				case L::FCEQ:
				case L::FCOR:
					St16(A(R.VI[1]), [&] {
						Ld32(A(R.VI[REG_CLIP_FLAG]));
						m_f.i32_const(0xFFFFFF);
						m_f.i32_and();
						m_f.i32_const(static_cast<s32>(code & 0xFFFFFF));
						if (p.ins.lo == L::FCAND)
						{
							m_f.i32_and();
							m_f.i32_const(0);
							m_f.i32_ne();
						}
						else if (p.ins.lo == L::FCEQ)
							m_f.i32_eq();
						else
						{
							m_f.i32_or();
							m_f.i32_const(0xFFFFFF);
							m_f.i32_eq();
						}
					});
					m_out->native_ops++;
					return true;
				case L::FCGET:
					if (it == 0)
						return true;
					St16(A(R.VI[it]), [&] { Ld32(A(R.VI[REG_CLIP_FLAG])); m_f.i32_const(0x0FFF); m_f.i32_and(); });
					m_out->native_ops++;
					return true;
				case L::B:
					m_f.i32_const(1);
					m_f.local_set(m_i[4]);
					m_out->native_ops++;
					return true;
				case L::BAL:
					if (it)
						St16(A(R.VI[it]), [&] { m_f.i32_const(static_cast<s32>((p.pc + 16) / 8)); });
					m_f.i32_const(1);
					m_f.local_set(m_i[4]);
					m_out->native_ops++;
					return true;
				case L::JR:
				case L::JALR:
					Ld16u(A(R.VI[is]));
					m_f.i32_const(3);
					m_f.i32_shl();
					m_f.local_set(m_i[5]);
					if (p.ins.lo == L::JALR && it)
						St16(A(R.VI[it]), [&] { m_f.i32_const(static_cast<s32>((p.pc + 16) / 8)); });
					m_out->native_ops++;
					return true;
				case L::IBEQ:
				case L::IBNE:
				case L::IBLTZ:
				case L::IBGTZ:
				case L::IBLEZ:
				case L::IBGEZ:
				{
					// _vuIBxx (VUops.cpp:1489-1600): s16 compares, VIOldValue in the VI branch delay window.
					const bool window = p.info.vi_backup_cycles > 0;
					const auto src = [&](u32 reg) {
						if (window && p.info.vi_backup_reg == reg)
							Ld16s(A(R.VIOldValue));
						else
							Ld16s(A(R.VI[reg]));
					};
					if (p.ins.lo == L::IBEQ || p.ins.lo == L::IBNE)
					{
						src(it);
						src(is);
						if (p.ins.lo == L::IBEQ)
							m_f.i32_eq();
						else
							m_f.i32_ne();
					}
					else
					{
						src(is);
						m_f.i32_const(0);
						switch (p.ins.lo)
						{
							case L::IBLTZ: m_f.i32_lt_s(); break;
							case L::IBGTZ: m_f.i32_gt_s(); break;
							case L::IBLEZ: m_f.i32_le_s(); break;
							default: m_f.i32_ge_s(); break;
						}
					}
					m_f.local_set(m_i[4]);
					m_out->native_ops++;
					return true;
				}
				default:
					HelperLower(p, delta);
					return true;
			}
		}

		void Emitter::EmitInstruction(const Plan& p, s32 busy_kick_exit)
		{
			const StepInfo& info = p.info;
			const u32 delta = p.cyc + info.op_cycle; // R.cycle at the op, relative to the entry

			if (busy_kick_exit >= 0)
			{
				// A kick still running at an XGKICK: the interpreter takes it (section 7.7).
				Ld32(A(R.xgkickenable));
				m_f.if_();
				EmitReturn(static_cast<u32>(busy_kick_exit));
				m_f.end();
			}

			Flushes(p);
			if (m_vu)
				KickCatchUp(delta);

			if (info.ibit)
			{
				Upper(p, delta);
				St32c(A(R.VI[REG_I]), p.ins.lower);
			}
			else
			{
				// The upper op's result is hidden from the lower op (VU1microInterp.cpp:130-185).
				const u32 old_vf = info.old_vf;
				const bool lower = info.lower_runs;
				if (lower && old_vf)
				{
					LdV(A(R.VF[old_vf]));
					m_f.local_set(m_hs);
				}
				if (lower && info.old_clip)
				{
					Ld32(A(R.VI[REG_CLIP_FLAG]));
					m_f.local_set(m_hc_old);
				}
				Upper(p, delta);
				if (lower)
				{
					if (old_vf)
					{
						LdV(A(R.VF[old_vf]));
						m_f.local_set(m_hn);
						StV(A(R.VF[old_vf]), [&] { m_f.local_get(m_hs); });
					}
					if (info.old_clip)
					{
						Ld32(A(R.VI[REG_CLIP_FLAG]));
						m_f.local_set(m_hc_new);
						St32(A(R.VI[REG_CLIP_FLAG]), [&] { m_f.local_get(m_hc_old); });
					}
					Lower(p, delta);
					if (old_vf)
						StV(A(R.VF[old_vf]), [&] { m_f.local_get(m_hn); });
					if (info.old_clip)
						St32(A(R.VI[REG_CLIP_FLAG]), [&] { m_f.local_get(m_hc_new); });
				}
			}

			// Pipe entries (VU1microInterp.cpp:187-192): their value snapshots, taken after both ops.
			if (info.fmac_added)
			{
				const fmacPipe& e = R.fmac[p.pre.fmacwritepos];
				St32(A(e.macflag), [&] { Ld32(A(R.macflag)); });
				St32(A(e.statusflag), [&] { Ld32(A(R.statusflag)); });
				St32(A(e.clipflag), [&] { Ld32(A(R.clipflag)); });
			}
			if (info.fdiv_added)
			{
				St32(A(R.fdiv.reg), [&] { Ld32(A(R.q)); });
				St32(A(R.fdiv.statusflag), [&] { Ld32(A(R.statusflag)); });
			}
			if (info.efu_added)
				St32(A(R.efu.reg), [&] { Ld32(A(R.p)); });
		}

		void Emitter::EmitBranchEnd(const Plan& branch, const Plan& delay, u32 exit_taken, u32 exit_fall, u32 exit_dyn)
		{
			(void)delay;
			const u16 flags = GetLowerFlags(branch.ins.lo);
			if (flags & LOWER_DYNAMIC)
			{
				St32(A(R.VI[REG_TPC]), [&] { m_f.local_get(m_i[5]); });
				EmitReturn(exit_dyn);
			}
			else if (flags & LOWER_COND)
			{
				m_f.local_get(m_i[4]);
				m_f.if_();
				EmitReturn(exit_taken);
				m_f.end();
				EmitReturn(exit_fall);
			}
			else
				EmitReturn(exit_taken);
		}
	} // namespace

	bool CompileBlock(int vu, u32 pc, const Signature& sig, CompiledBlock* out)
	{
		*out = {};
		VURegs& R = vu ? VU1 : VU0;
		const u32 progsize = vu ? VU1_PROGSIZE : VU0_PROGSIZE;
		TimingModel model(vu);
		model.Load(sig, Control{});
		const s32 efu_entry_cycle = static_cast<s32>(model.State().efu.Cycle);
		const bool efu_at_entry = model.State().efu.enable != 0;

		std::vector<Plan> plans;
		plans.reserve(64);
		bool stop_before = false; // the last planned instruction is followed by one generated code does not run
		bool ended = false;
		bool branch_end = false;
		bool wrapped = false;
		bool mbit_end = false;
		VURegs after_end; // the model after the program end, for _vuFlushAll's write-backs
		PipeImage stop_img = {};
		Control stop_ctl;
		u32 stop_pc = 0;
		u32 cur = pc;
		u32 cyc = 0;
		for (;;)
		{
			Plan p;
			p.pc = cur;
			const u32* words = reinterpret_cast<const u32*>(&R.Micro[cur]);
			p.ins = Decode(words[0], words[1]);
			p.pre_ctl = model.GetControl();
			p.pre = CaptureImage(model.State());
			p.cyc = cyc;
			if (!Allowed(p, vu))
			{
				if (p.pre_ctl.branch != 0)
				{
					// Not in a branch delay slot: stop before the branch.
					const Plan& b = plans.back();
					stop_img = b.pre;
					stop_ctl = b.pre_ctl;
					stop_pc = b.pc;
					plans.pop_back();
				}
				else
				{
					stop_img = p.pre;
					stop_ctl = p.pre_ctl;
					stop_pc = p.pc;
				}
				stop_before = true;
				break;
			}
			StepInput in;
			in.pc = cur;
			in.lower = words[0];
			in.upper = words[1];
			in.fbrst = 0; // no D/T bits here
			in.branch_taken = true;
			in.jump_target = 0;
			if (!model.Step(in, &p.info))
				return false;
			cyc += p.info.cycles;
			plans.push_back(p);
			if (p.info.end)
			{
				ended = true;
				after_end = model.State();
				break;
			}
			if (p.pre_ctl.branch == 1)
			{
				branch_end = true; // the delay slot of the previous instruction
				break;
			}
			if (vu == 0 && p.ins.MBit())
			{
				mbit_end = true; // InterpVU0::Execute stops after it (VU0microInterp.cpp:268)
				cur += 8;
				break;
			}
			cur += 8;
			if (cur >= progsize)
			{
				wrapped = true;
				break;
			}
			if (plans.size() >= BLOCK_CAP && model.GetControl().branch == 0)
				break;
		}
		if (plans.empty())
			return false;

		// Exits.
		const auto make_exit = [&](ExitKind kind, ExitReason reason, u32 instrs, const PipeImage& img, const Control& ctl,
								   u32 tpc) {
			BlockExit x;
			x.kind = kind;
			x.reason = reason;
			x.instrs = instrs;
			x.tpc = tpc;
			x.ebit = ctl.ebit;
			x.pipes = img;
			x.mbit = vu == 0 && instrs > 0 && plans[instrs - 1].ins.MBit(); // set at its fetch (VU0microInterp.cpp:40)
			if (efu_at_entry && img.efu.enable && img.efu.sCycle < TIMING_MODEL_BASE)
				x.efu_cycle_adjust = static_cast<s32>(img.efu.Cycle) - efu_entry_cycle;
			if (kind == ExitKind::Boundary && ctl.ebit == 0 && ctl.branch == 0)
			{
				VURegs tmp;
				std::memset(static_cast<void*>(&tmp), 0, sizeof(tmp));
				std::memcpy(tmp.fmac, img.fmac, sizeof(tmp.fmac));
				tmp.fmacreadpos = img.fmacreadpos;
				tmp.fmacwritepos = img.fmacwritepos;
				tmp.fmaccount = img.fmaccount;
				tmp.fdiv = img.fdiv;
				tmp.efu = img.efu;
				std::memcpy(tmp.ialu, img.ialu, sizeof(tmp.ialu));
				tmp.ialureadpos = img.ialureadpos;
				tmp.ialuwritepos = img.ialuwritepos;
				tmp.ialucount = img.ialucount;
				tmp.VIBackupCycles = img.VIBackupCycles;
				tmp.VIRegNumber = img.VIRegNumber;
				tmp.cycle = img.cycle;
				x.chain = DeriveSignature(tmp, &x.sig) == Refusal::None;
			}
			out->exits.push_back(x);
			return static_cast<u32>(out->exits.size() - 1);
		};

		Emitter em(out, vu);
		const u32 n = static_cast<u32>(plans.size());
		for (u32 i = 0; i < n; i++)
		{
			const Plan& p = plans[i];
			s32 busy = -1;
			if (vu && !p.ins.IBit() && p.ins.lo == L::XGKICK && !p.info.discarded)
			{
				// The interpreter's state before this instruction: TPC is its own address.
				busy = static_cast<s32>(make_exit(ExitKind::Boundary, ExitReason::BusyKick, i, p.pre, p.pre_ctl, p.pc));
			}
			em.EmitInstruction(p, busy);
			if (i + 1 == n && ended)
				em.FlushAllAtEnd(p, after_end);
		}

		const PipeImage final_img = CaptureImage(model.State());
		const Control final_ctl = model.GetControl();
		const Plan& last = plans.back();
		if (ended)
		{
			em.EmitReturn(make_exit(ExitKind::End, ExitReason::Block, n, final_img, final_ctl, last.pc + 8));
		}
		else if (branch_end)
		{
			const Plan& branch = plans[n - 2];
			const u32 taken = make_exit(ExitKind::Boundary, ExitReason::Block, n, final_img, final_ctl,
				StaticBranchTarget(branch.pc, branch.ins.lower) & (progsize - 1));
			const u32 fall = make_exit(ExitKind::Boundary, ExitReason::Block, n, final_img, final_ctl, last.pc + 8);
			const u32 dyn = make_exit(ExitKind::Boundary, ExitReason::Block, n, final_img, final_ctl, 0);
			out->exits[dyn].dynamic_tpc = true;
			em.EmitBranchEnd(branch, last, taken, fall, dyn);
		}
		else if (stop_before)
		{
			em.EmitReturn(make_exit(ExitKind::Boundary, ExitReason::Fallback, n, stop_img, stop_ctl, stop_pc));
		}
		else
		{
			const u32 id = make_exit(ExitKind::Boundary, (wrapped || mbit_end) ? ExitReason::Block : ExitReason::Cap, n, final_img, final_ctl, last.pc + 8);
			em.EmitReturn(id);
		}
		em.Finish();
		out->instrs = n;
		out->last_start = last.cyc;
		return true;
	}
} // namespace VU1Jit
