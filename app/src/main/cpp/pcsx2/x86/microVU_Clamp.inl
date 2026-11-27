// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "arm64/ClampTracker.h"

#include <cstring>

//------------------------------------------------------------------
// Micro VU - Clamp Functions
//------------------------------------------------------------------

//alignas(16) const u32 sse4_minvals[2][4] = {
//	{0xff7fffff, 0xffffffff, 0xffffffff, 0xffffffff}, //1000
//	{0xff7fffff, 0xff7fffff, 0xff7fffff, 0xff7fffff}, //1111
//};
//alignas(16) const u32 sse4_maxvals[2][4] = {
//	{0x7f7fffff, 0x7fffffff, 0x7fffffff, 0x7fffffff}, //1000
//	{0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff}, //1111
//};

// Used for Result Clamping
// Note: This function will not preserve NaN values' sign.
// The theory behind this is that when we compute a result, and we've
// gotten a NaN value, then something went wrong; and the NaN's sign
// is not to be trusted. Games like positive values better usually,
// and its faster... so just always make NaNs into positive infinity.
// In microprograms the clamp constants live in xmmClampMax/xmmClampMin (outside the allocator's range) and are
// loaded at the first clamp after a barrier: a bound label, a call, or a block start. That takes the two loads
// out of every clamp (with "extra" clamping, a third of the code of a typical VU1 program was those loads).
// COP2 (macro mode) code runs inside EE blocks, so it keeps loading them each time.
static thread_local u32 s_mVUclampConstsBarrier = 0xffffffffu;

// Registers known to hold clamped values (see ClampTracker.h), brought up to date with the code emitted since
// the last call. Reset at a barrier, or if the code buffer changed or moved backwards.
static thread_local u32 s_mVUclampedMask = 0;
static thread_local u32 s_mVUclampedBarrier = 0xffffffffu;
static thread_local const u8* s_mVUclampedBuffer = nullptr;
static thread_local ptrdiff_t s_mVUclampedOffset = 0;

static u32 mVUsyncClampedRegs()
{
	const ptrdiff_t offset = armAsm->GetCursorOffset();
	const u8* const buffer = armAsm->GetCursorAddress<const u8*>() - offset;
	if (s_mVUclampedBarrier != armCodegenBarrier || s_mVUclampedBuffer != buffer || offset < s_mVUclampedOffset)
	{
		s_mVUclampedMask = 0;
		s_mVUclampedBarrier = armCodegenBarrier;
		s_mVUclampedBuffer = buffer;
		s_mVUclampedOffset = offset;
		return 0;
	}

	u32 mask = s_mVUclampedMask;
	for (ptrdiff_t at = s_mVUclampedOffset; at + 4 <= offset; at += 4)
	{
		u32 insn;
		std::memcpy(&insn, buffer + at, sizeof(insn));
		mask = ClampTracker::Apply(insn, mask);
	}
	s_mVUclampedMask = mask;
	s_mVUclampedOffset = offset;
	return mask;
}

static __fi bool mVUloadClampConsts(microVU& mVU)
{
	if (isCOP2)
		return false;

	if (s_mVUclampConstsBarrier != armCodegenBarrier)
	{
		armAsm->Ldr(xmmClampMax, PTR_CPU(mVUglob.maxvals));
		armAsm->Ldr(xmmClampMin, PTR_CPU(mVUglob.minvals));
		s_mVUclampConstsBarrier = armCodegenBarrier;
	}
	return true;
}

void mVUclamp1(microVU& mVU, const xmm& reg, const xmm& regT1, int xyzw, bool bClampE = 0)
{
	if (((!clampE && CHECK_VU_OVERFLOW(mVU.index)) || (clampE && bClampE)) && mVU.regAlloc->checkVFClamp(reg.GetCode()))
	{
		if (mVUloadClampConsts(mVU))
		{
			// Nothing wrote it since it was clamped: clamping again would not change it.
			const u32 bit = 1u << reg.GetCode();
			if (mVUsyncClampedRegs() & bit)
				return;

			if (xyzw == 1 || xyzw == 2 || xyzw == 4 || xyzw == 8)
			{
				armAsm->Fminnm(reg.S(), reg.S(), xmmClampMax.S());
				armAsm->Fmaxnm(reg.S(), reg.S(), xmmClampMin.S());
			}
			else
			{
				armAsm->Fminnm(reg.V4S(), reg.V4S(), xmmClampMax.V4S());
				armAsm->Fmaxnm(reg.V4S(), reg.V4S(), xmmClampMin.V4S());
			}

			// Both forms leave every lane in range: the scalar ones zero lanes 1-3.
			s_mVUclampedMask = mVUsyncClampedRegs() | bit;
			return;
		}

		switch (xyzw)
		{
			case 1: case 2: case 4: case 8:
//				xMIN.SS(reg, ptr32[mVUglob.maxvals]);
                armAsm->Fminnm(reg.S(), reg.S(), armLoadPtrV(PTR_CPU(mVUglob.maxvals)).S());
//				xMAX.SS(reg, ptr32[mVUglob.minvals]);
                armAsm->Fmaxnm(reg.S(), reg.S(), armLoadPtrV(PTR_CPU(mVUglob.minvals)).S());
				break;
			default:
//				xMIN.PS(reg, ptr32[mVUglob.maxvals]);
                armAsm->Fminnm(reg.V4S(), reg.V4S(), armLoadPtrV(PTR_CPU(mVUglob.maxvals)).V4S());
//				xMAX.PS(reg, ptr32[mVUglob.minvals]);
                armAsm->Fmaxnm(reg.V4S(), reg.V4S(), armLoadPtrV(PTR_CPU(mVUglob.minvals)).V4S());
				break;
		}
	}
}

// Used for Operand Clamping
// Note 1: If 'preserve sign' mode is on, it will preserve the sign of NaN values.
// Note 2: Using regalloc here seems to contaminate some regs in certain games.
// Must be some specific case I've overlooked (or I used regalloc improperly on an opcode)
// so we just use a temporary mem location for our backup for now... (non-sse4 version only)
void mVUclamp2(microVU& mVU, const xmm& reg, const xmm& regT1in, int xyzw, bool bClampE = 0)
{
	if (((!clampE && CHECK_VU_SIGN_OVERFLOW(mVU.index)) || (clampE && bClampE && CHECK_VU_SIGN_OVERFLOW(mVU.index))) && mVU.regAlloc->checkVFClamp(reg.GetCode()))
	{
		int i = (xyzw == 1 || xyzw == 2 || xyzw == 4 || xyzw == 8) ? 0 : 1;
//		xPMIN.SD(reg, ptr128[&sse4_maxvals[i][0]]);
        armAsm->Smin(reg.V4S(), reg.V4S(), armLoadPtrV(PTR_CPU(mVUss4.sse4_maxvals[i][0])).V4S());
//		xPMIN.UD(reg, ptr128[&sse4_minvals[i][0]]);
        armAsm->Umin(reg.V4S(), reg.V4S(), armLoadPtrV(PTR_CPU(mVUss4.sse4_minvals[i][0])).V4S());
		return;
	}
	else
		mVUclamp1(mVU, reg, regT1in, xyzw, bClampE);
}

// Used for operand clamping on every SSE instruction (add/sub/mul/div)
void mVUclamp3(microVU& mVU, const xmm& reg, const xmm& regT1, int xyzw)
{
	if (clampE && mVU.regAlloc->checkVFClamp(reg.GetCode()))
		mVUclamp2(mVU, reg, regT1, xyzw, 1);
}

// Used for result clamping on every SSE instruction (add/sub/mul/div)
// Note: Disabled in "preserve sign" mode because in certain cases it
// makes too much code-gen, and you get jump8-overflows in certain
// emulated opcodes (causing crashes). Since we're clamping the operands
// with mVUclamp3, we should almost never be getting a NaN result,
// but this clamp is just a precaution just-in-case.
void mVUclamp4(microVU& mVU, const xmm& reg, const xmm& regT1, int xyzw)
{
	if (clampE && !CHECK_VU_SIGN_OVERFLOW(mVU.index) && mVU.regAlloc->checkVFClamp(reg.GetCode()))
		mVUclamp1(mVU, reg, regT1, xyzw, 1);
}
