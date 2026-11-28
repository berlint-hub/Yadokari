// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Tracks which host vector registers hold a value that is already clamped to [-FLT_MAX, FLT_MAX] in every lane,
// by reading the A64 instructions that were actually emitted. microVU uses it to skip a clamp of a register
// that nothing has written since it was clamped, or a range-preserving MOV/DUP/INS/AND/BIC/FABS/FNEG transform
// (or a self-EOR that produces zero): with
// "extra" VU clamping about a third of the clamps in GT4's VU1 code were of that kind.
//
// Everything is conservative: an instruction that may write a vector register clears it, and unconditional
// control flow clears everything (code after it is only reached through a label, which microVU treats as a
// barrier anyway, or it is data such as a literal pool).

#pragma once

#include <cstdint>

namespace ClampTracker
{
	// Bit r of the mask: v<r> is known to be clamped in every lane. Returns the mask after `insn`.
	inline uint32_t Apply(uint32_t insn, uint32_t mask)
	{
		// B, BL
		if ((insn & 0x7C000000u) == 0x14000000u)
			return 0;
		// BR, BLR, RET and the other unconditional branch (register) forms
		if ((insn & 0xFE000000u) == 0xD6000000u)
			return 0;
		// SVC, HVC, SMC, BRK, HLT, DCPS
		if ((insn & 0xFF000000u) == 0xD4000000u)
			return 0;

		const uint32_t rd = insn & 31;

		// Data processing (immediate) and data processing (register) only write general registers.
		if ((insn & 0x1C000000u) == 0x10000000u || (insn & 0x0E000000u) == 0x0A000000u)
			return mask;

		// Conditional branches, system instructions (MRS writes a general register; MSR FPCR does not change
		// register contents), hints, barriers.
		if ((insn & 0x1C000000u) == 0x14000000u)
			return mask;

		// Loads and stores (scalar, pair, and LD1-LD4 structures): Rt, Rt2 and up to four consecutive registers.
		if ((insn & 0x0A000000u) == 0x08000000u)
		{
			uint32_t clear = 1u << ((insn >> 10) & 31);
			for (uint32_t i = 0; i < 4; i++)
				clear |= 1u << ((rd + i) & 31);
			return mask & ~clear;
		}

		// SIMD and floating point data processing: the destination is Rd.
		if ((insn & 0x0E000000u) == 0x0E000000u)
		{
			const uint32_t rn = (insn >> 5) & 31;

			// MOV Vd.<8B|16B>, Vn (ORR Vd, Vn, Vn): a copy (the 8B form zeroes the upper half).
			if ((insn & 0xBFE0FC00u) == 0x0EA01C00u && ((insn >> 16) & 31) == rn)
				return (mask & (1u << rn)) ? (mask | (1u << rd)) : (mask & ~(1u << rd));

			// DUP Vd.<T>, Vn.<Ts>[i]: lanes of Vn (the 64-bit forms zero the upper half).
			if ((insn & 0xBFE0FC00u) == 0x0E000400u)
				return (mask & (1u << rn)) ? (mask | (1u << rd)) : (mask & ~(1u << rd));

			// INS Vd.<T>[i], Vn.<T>[j]: if both vectors were clamped, replacing one
			// lane preserves the range of every lane (unlike a MOV from just one source).
			const uint32_t ins_imm5 = (insn >> 16) & 31;
			if ((insn & 0x3FE08400u) == 0x2E000400u && ins_imm5 != 0 && (ins_imm5 & 3) == 0)
				return (mask & (1u << rn)) && (mask & (1u << rd)) ? mask : (mask & ~(1u << rd));

			// AND Vd.16B, Vn.16B, Vm.16B: every output bit is a subset of either input.
			// In particular, a finite FP32 input with exponent != 255 cannot become Inf/NaN
			// or grow past FLT_MAX by clearing bits. This covers the VU ABS sign-bit mask.
			const uint32_t logical_op = insn & 0xBFE0FC00u;
			if (logical_op == 0x0E201C00u)
			{
				const uint32_t rm = (insn >> 16) & 31;
				const uint32_t sources = (1u << rn) | (1u << rm);
				return (mask & sources) ? (mask | (1u << rd)) : (mask & ~(1u << rd));
			}

			// BIC Vd, Vn, Vm is Vn & ~Vm, so it only clears bits from Vn.
			if (logical_op == 0x0E601C00u)
				return (mask & (1u << rn)) ? (mask | (1u << rd)) : (mask & ~(1u << rd));

			// EOR Vd, Vn, Vn is all zero, which is clamped irrespective of the input.
			if (logical_op == 0x2E201C00u && rn == ((insn >> 16) & 31))
				return mask | (1u << rd);

			// FABS/FNEG Vd.<2S|4S>, Vn.<2S|4S>: these preserve finite values in
			// [-FLT_MAX, FLT_MAX]. Restrict to FP32 so half/double operations are not
			// mistaken for range-preserving transforms of the VU's single-precision values.
			const uint32_t fp_misc = insn & 0xBF3FFC00u;
			if ((insn & 0x00400000u) == 0 &&
				(fp_misc == 0x0E20F800u || fp_misc == 0x2E20F800u))
				return (mask & (1u << rn)) ? (mask | (1u << rd)) : (mask & ~(1u << rd));

			return mask & ~(1u << rd);
		}

		// Anything not recognised above (reserved/unallocated space, SVE): know nothing.
		return 0;
	}
} // namespace ClampTracker
