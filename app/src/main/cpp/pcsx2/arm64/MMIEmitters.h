// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// NEON sequences for R5900 MMI instructions that used to fall back to the interpreter.
// Kept free of recompiler state so the same code can be checked against the interpreter
// (pcsx2/MMI.cpp) in isolation. Needs `armAsm` (vixl::aarch64::MacroAssembler*) in scope.
//
// Register roles: s/t are Rs/Rt, d is Rd (may alias s or t), lo/hi are the LO/HI registers
// (never alias s, t or d). q29-q31 are codegen scratch and are clobbered.

#pragma once

namespace MMIEmit
{
	namespace a64 = vixl::aarch64;

	// p[i] = Rs.SS[i] * Rt.SS[i]; LO = {p0, p1, p4, p5}, HI = {p2, p3, p6, p7}, Rd = {p0, p2, p4, p6}
	inline void PMULTH(const a64::VRegister& d, bool write_d, const a64::VRegister& s, const a64::VRegister& t,
		const a64::VRegister& lo, const a64::VRegister& hi)
	{
		armAsm->Smull(a64::v29.V4S(), s.V4H(), t.V4H()); // p0..p3
		armAsm->Smull2(a64::v30.V4S(), s.V8H(), t.V8H()); // p4..p7
		armAsm->Zip1(lo.V2D(), a64::v29.V2D(), a64::v30.V2D());
		armAsm->Zip2(hi.V2D(), a64::v29.V2D(), a64::v30.V2D());
		if (write_d)
			armAsm->Uzp1(d.V4S(), a64::v29.V4S(), a64::v30.V4S());
	}

	// LO/HI += (or -=) the PMULTH products, wrapping; Rd = {LO0, HI0, LO2, HI2}
	inline void PMADDH_PMSUBH(const a64::VRegister& d, bool write_d, const a64::VRegister& s, const a64::VRegister& t,
		const a64::VRegister& lo, const a64::VRegister& hi, bool subtract)
	{
		armAsm->Smull(a64::v29.V4S(), s.V4H(), t.V4H());
		armAsm->Smull2(a64::v30.V4S(), s.V8H(), t.V8H());
		armAsm->Zip1(a64::v31.V2D(), a64::v29.V2D(), a64::v30.V2D()); // products for LO
		armAsm->Zip2(a64::v29.V2D(), a64::v29.V2D(), a64::v30.V2D()); // products for HI
		if (subtract)
		{
			armAsm->Sub(lo.V4S(), lo.V4S(), a64::v31.V4S());
			armAsm->Sub(hi.V4S(), hi.V4S(), a64::v29.V4S());
		}
		else
		{
			armAsm->Add(lo.V4S(), lo.V4S(), a64::v31.V4S());
			armAsm->Add(hi.V4S(), hi.V4S(), a64::v29.V4S());
		}
		if (write_d)
			armAsm->Trn1(d.V4S(), lo.V4S(), hi.V4S());
	}

	// LO = {p1+p0, p1, p5+p4, p5}, HI = {p3+p2, p3, p7+p6, p7}, Rd = {p0+p1, p2+p3, p4+p5, p6+p7}
	inline void PHMADH(const a64::VRegister& d, bool write_d, const a64::VRegister& s, const a64::VRegister& t,
		const a64::VRegister& lo, const a64::VRegister& hi)
	{
		armAsm->Smull(a64::v29.V4S(), s.V4H(), t.V4H());
		armAsm->Smull2(a64::v30.V4S(), s.V8H(), t.V8H());
		armAsm->Addp(a64::v31.V4S(), a64::v29.V4S(), a64::v30.V4S()); // pair sums
		armAsm->Uzp2(a64::v29.V4S(), a64::v29.V4S(), a64::v30.V4S()); // odd products
		armAsm->Trn1(lo.V4S(), a64::v31.V4S(), a64::v29.V4S());
		armAsm->Trn2(hi.V4S(), a64::v31.V4S(), a64::v29.V4S());
		if (write_d)
			armAsm->Mov(d.V16B(), a64::v31.V16B());
	}

	// LO = {p1-p0, ~p1, p5-p4, ~p5}, HI = {p3-p2, ~p3, p7-p6, ~p7}, Rd = {p1-p0, p3-p2, p5-p4, p7-p6}
	inline void PHMSBH(const a64::VRegister& d, bool write_d, const a64::VRegister& s, const a64::VRegister& t,
		const a64::VRegister& lo, const a64::VRegister& hi)
	{
		armAsm->Smull(a64::v29.V4S(), s.V4H(), t.V4H());
		armAsm->Smull2(a64::v30.V4S(), s.V8H(), t.V8H());
		armAsm->Uzp1(a64::v31.V4S(), a64::v29.V4S(), a64::v30.V4S()); // even products
		armAsm->Uzp2(a64::v29.V4S(), a64::v29.V4S(), a64::v30.V4S()); // odd products
		armAsm->Sub(a64::v31.V4S(), a64::v29.V4S(), a64::v31.V4S()); // odd - even
		armAsm->Mvn(a64::v30.V16B(), a64::v29.V16B()); // ~odd (undocumented hardware behaviour)
		armAsm->Trn1(lo.V4S(), a64::v31.V4S(), a64::v30.V4S());
		armAsm->Trn2(hi.V4S(), a64::v31.V4S(), a64::v30.V4S());
		if (write_d)
			armAsm->Mov(d.V16B(), a64::v31.V16B());
	}

	// p[k] = (u64)Rs.UL[2k] * Rt.UL[2k]; LO.SD[k] = (s32)p[k], HI.SD[k] = (s32)(p[k] >> 32), Rd.UD[k] = p[k].
	// PMADDUW first adds the 64-bit accumulator {HI.UL[2k], LO.UL[2k]}.
	inline void PMULTUW_PMADDUW(const a64::VRegister& d, bool write_d, const a64::VRegister& s, const a64::VRegister& t,
		const a64::VRegister& lo, const a64::VRegister& hi, bool accumulate)
	{
		armAsm->Xtn(a64::v29.V2S(), s.V2D()); // Rs.UL[0], Rs.UL[2]
		armAsm->Xtn(a64::v30.V2S(), t.V2D()); // Rt.UL[0], Rt.UL[2]
		if (accumulate)
		{
			armAsm->Trn1(a64::v31.V4S(), lo.V4S(), hi.V4S()); // {HI.UL[0]:LO.UL[0], HI.UL[2]:LO.UL[2]}
			armAsm->Umlal(a64::v31.V2D(), a64::v29.V2S(), a64::v30.V2S());
		}
		else
		{
			armAsm->Umull(a64::v31.V2D(), a64::v29.V2S(), a64::v30.V2S());
		}
		armAsm->Xtn(a64::v29.V2S(), a64::v31.V2D());
		armAsm->Shrn(a64::v30.V2S(), a64::v31.V2D(), 32);
		armAsm->Sxtl(lo.V2D(), a64::v29.V2S());
		armAsm->Sxtl(hi.V2D(), a64::v30.V2S());
		if (write_d)
			armAsm->Mov(d.V16B(), a64::v31.V16B());
	}

	// Rd = bytes [SA, SA + 16) of the 32-byte value {Rt (low), Rs (high)}, SA in bytes (0..15, as set by
	// MTSAB/MTSAH). sa is the SA register in memory, iota 16 bytes {0, 1, ..., 15}, tmp a scratch W register.
	inline void QFSRV(const a64::VRegister& d, const a64::VRegister& s, const a64::VRegister& t,
		const a64::MemOperand& sa, const a64::MemOperand& iota, const a64::Register& tmp)
	{
		armAsm->Ldr(tmp.W(), sa);
		armAsm->Ldr(a64::q29, iota);
		armAsm->Dup(a64::v30.V16B(), tmp.W());
		armAsm->Add(a64::v29.V16B(), a64::v29.V16B(), a64::v30.V16B()); // byte indices SA..SA+15
		armAsm->Mov(a64::v30.V16B(), t.V16B()); // TBL needs the table in consecutive registers
		armAsm->Mov(a64::v31.V16B(), s.V16B());
		armAsm->Tbl(d.V16B(), a64::v30.V16B(), a64::v31.V16B(), a64::v29.V16B());
	}
} // namespace MMIEmit
