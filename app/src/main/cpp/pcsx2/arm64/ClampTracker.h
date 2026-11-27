// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Tracks which host vector registers hold a value that is already clamped to [-FLT_MAX, FLT_MAX] in every lane,
// by reading the A64 instructions that were actually emitted. microVU uses it to skip a clamp of a register
// that nothing has written since it was clamped (or that is a MOV/DUP copy of a clamped register): with
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

			return mask & ~(1u << rd);
		}

		// Anything not recognised above (reserved/unallocated space, SVE): know nothing.
		return 0;
	}
} // namespace ClampTracker
