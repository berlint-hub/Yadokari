// MMI fallbacks: NEON sequences from pcsx2/arm64/MMIEmitters.h vs the interpreter (pcsx2/MMI.cpp).
#include "harness.h"
#include "MMIEmitters.h"
#include <random>

using s16 = int16_t;
using u16 = uint16_t;
using s32 = int32_t;
union R128 { u64 UD[2]; u32 UL[4]; s32 SL[4]; u16 US[8]; s16 SS[8]; u8 UC[16]; };
struct Ctx { R128 rs, rt, rd, lo, hi; u32 sa; };
static State g_state;

// ---- reference: bodies copied from pcsx2/MMI.cpp, cpuRegs.GPR.r[_Rs_] -> c.rs etc., `if (_Rd_)` -> `if (wd)`
static void ref_PMULTH(Ctx& c, bool wd) {
	s32 temp;
	temp = (s32)c.rs.SS[0] * (s32)c.rt.SS[0]; c.lo.UL[0] = temp;
	temp = (s32)c.rs.SS[1] * (s32)c.rt.SS[1]; c.lo.UL[1] = temp;
	temp = (s32)c.rs.SS[2] * (s32)c.rt.SS[2]; c.hi.UL[0] = temp;
	temp = (s32)c.rs.SS[3] * (s32)c.rt.SS[3]; c.hi.UL[1] = temp;
	temp = (s32)c.rs.SS[4] * (s32)c.rt.SS[4]; c.lo.UL[2] = temp;
	temp = (s32)c.rs.SS[5] * (s32)c.rt.SS[5]; c.lo.UL[3] = temp;
	temp = (s32)c.rs.SS[6] * (s32)c.rt.SS[6]; c.hi.UL[2] = temp;
	temp = (s32)c.rs.SS[7] * (s32)c.rt.SS[7]; c.hi.UL[3] = temp;
	if (wd) { c.rd.UL[0] = c.lo.UL[0]; c.rd.UL[1] = c.hi.UL[0]; c.rd.UL[2] = c.lo.UL[2]; c.rd.UL[3] = c.hi.UL[2]; }
}
static void ref_PMADDH(Ctx& c, bool wd, bool sub) {
	s32 temp;
	auto op = [sub](u32 acc, s32 p) -> s32 { return sub ? (s32)(acc - (u32)p) : (s32)(acc + (u32)p); };
	temp = op(c.lo.UL[0], (s32)c.rs.SS[0] * (s32)c.rt.SS[0]); c.lo.UL[0] = temp;
	temp = op(c.lo.UL[1], (s32)c.rs.SS[1] * (s32)c.rt.SS[1]); c.lo.UL[1] = temp;
	temp = op(c.hi.UL[0], (s32)c.rs.SS[2] * (s32)c.rt.SS[2]); c.hi.UL[0] = temp;
	temp = op(c.hi.UL[1], (s32)c.rs.SS[3] * (s32)c.rt.SS[3]); c.hi.UL[1] = temp;
	temp = op(c.lo.UL[2], (s32)c.rs.SS[4] * (s32)c.rt.SS[4]); c.lo.UL[2] = temp;
	temp = op(c.lo.UL[3], (s32)c.rs.SS[5] * (s32)c.rt.SS[5]); c.lo.UL[3] = temp;
	temp = op(c.hi.UL[2], (s32)c.rs.SS[6] * (s32)c.rt.SS[6]); c.hi.UL[2] = temp;
	temp = op(c.hi.UL[3], (s32)c.rs.SS[7] * (s32)c.rt.SS[7]); c.hi.UL[3] = temp;
	if (wd) { c.rd.UL[0] = c.lo.UL[0]; c.rd.UL[1] = c.hi.UL[0]; c.rd.UL[2] = c.lo.UL[2]; c.rd.UL[3] = c.hi.UL[2]; }
}
static void ref_PHMADH(Ctx& c, bool wd) {
	auto lo = [&c](int dd, int n) { s32 f = (s32)c.rs.SS[n+1] * (s32)c.rt.SS[n+1]; s32 t = (s32)((u32)f + (u32)((s32)c.rs.SS[n] * (s32)c.rt.SS[n])); c.lo.UL[dd] = t; c.lo.UL[dd+1] = f; };
	auto hi = [&c](int dd, int n) { s32 f = (s32)c.rs.SS[n+1] * (s32)c.rt.SS[n+1]; s32 t = (s32)((u32)f + (u32)((s32)c.rs.SS[n] * (s32)c.rt.SS[n])); c.hi.UL[dd] = t; c.hi.UL[dd+1] = f; };
	lo(0, 0); hi(0, 2); lo(2, 4); hi(2, 6);
	if (wd) { c.rd.UL[0] = c.lo.UL[0]; c.rd.UL[1] = c.hi.UL[0]; c.rd.UL[2] = c.lo.UL[2]; c.rd.UL[3] = c.hi.UL[2]; }
}
static void ref_PHMSBH(Ctx& c, bool wd) {
	auto lo = [&c](int dd, int n) { s32 f = (s32)c.rs.SS[n+1] * (s32)c.rt.SS[n+1]; s32 t = (s32)((u32)f - (u32)((s32)c.rs.SS[n] * (s32)c.rt.SS[n])); c.lo.UL[dd] = t; c.lo.UL[dd+1] = ~f; };
	auto hi = [&c](int dd, int n) { s32 f = (s32)c.rs.SS[n+1] * (s32)c.rt.SS[n+1]; s32 t = (s32)((u32)f - (u32)((s32)c.rs.SS[n] * (s32)c.rt.SS[n])); c.hi.UL[dd] = t; c.hi.UL[dd+1] = ~f; };
	lo(0, 0); hi(0, 2); lo(2, 4); hi(2, 6);
	if (wd) { c.rd.UL[0] = c.lo.UL[0]; c.rd.UL[1] = c.hi.UL[0]; c.rd.UL[2] = c.lo.UL[2]; c.rd.UL[3] = c.hi.UL[2]; }
}
static void ref_PMULTUW(Ctx& c, bool wd, bool acc) {
	for (int dd = 0; dd < 2; dd++) {
		const int ss = dd * 2;
		u64 tempu = (u64)c.rs.UL[ss] * (u64)c.rt.UL[ss];
		if (acc) tempu += (u64)((u64)c.lo.UL[ss] | ((u64)c.hi.UL[ss] << 32));
		c.lo.UD[dd] = (u64)(int64_t)(s32)(tempu & 0xffffffff);
		c.hi.UD[dd] = (u64)(int64_t)(s32)(tempu >> 32);
		if (wd) c.rd.UD[dd] = tempu;
	}
}
static void ref_QFSRV(Ctx& c) {
	R128 Rd;
	u32 sa_amt = c.sa << 3;
	if (sa_amt == 0) { c.rd.UD[0] = c.rt.UD[0]; c.rd.UD[1] = c.rt.UD[1]; return; }
	if (sa_amt < 64) {
		Rd.UD[0] = c.rt.UD[0] >> sa_amt; Rd.UD[1] = c.rt.UD[1] >> sa_amt;
		Rd.UD[0] |= c.rt.UD[1] << (64 - sa_amt); Rd.UD[1] |= c.rs.UD[0] << (64 - sa_amt);
	} else {
		Rd.UD[0] = c.rt.UD[1] >> (sa_amt - 64); Rd.UD[1] = c.rs.UD[0] >> (sa_amt - 64);
		if (sa_amt != 64) { Rd.UD[0] |= c.rs.UD[0] << (128u - sa_amt); Rd.UD[1] |= c.rs.UD[1] << (128u - sa_amt); }
	}
	c.rd = Rd;
}

// ---- driver
static std::mt19937 rng(2026);
static const u16 kHalf[] = {0x0000, 0x0001, 0xFFFF, 0x7FFF, 0x8000, 0x8001, 0x4000, 0xC000};
static void fill(R128& r) { for (auto& h : r.US) h = (rng() % 3 == 0) ? kHalf[rng() % 8] : (u16)rng(); }

enum class Op { PMULTH, PMADDH, PMSUBH, PHMADH, PHMSBH, QFSRV, PMULTUW, PMADDUW };
static const char* kNames[] = {"PMULTH", "PMADDH", "PMSUBH", "PHMADH", "PHMSBH", "QFSRV", "PMULTUW", "PMADDUW"};

// alias: 0 = d separate (q3), 1 = d is s (q1), 2 = d is t (q2)
static int run(Op op, bool wd, int alias, int iters)
{
	const a64::VRegister S = a64::q1, T = a64::q2, LO = a64::q4, HI = a64::q5;
	const a64::VRegister D = alias == 1 ? a64::q1 : alias == 2 ? a64::q2 : a64::q3;
	Jit jit([&] {
		switch (op)
		{
			case Op::PMULTH: MMIEmit::PMULTH(D, wd, S, T, LO, HI); break;
			case Op::PMADDH: MMIEmit::PMADDH_PMSUBH(D, wd, S, T, LO, HI, false); break;
			case Op::PMSUBH: MMIEmit::PMADDH_PMSUBH(D, wd, S, T, LO, HI, true); break;
			case Op::PHMADH: MMIEmit::PHMADH(D, wd, S, T, LO, HI); break;
			case Op::PHMSBH: MMIEmit::PHMSBH(D, wd, S, T, LO, HI); break;
			case Op::QFSRV: MMIEmit::QFSRV(D, S, T, PTR_CPU(sa), PTR_CPU(shuffle.data[0xE4][0]), a64::w16); break;
			case Op::PMULTUW: MMIEmit::PMULTUW_PMADDUW(D, wd, S, T, LO, HI, false); break;
			case Op::PMADDUW: MMIEmit::PMULTUW_PMADDUW(D, wd, S, T, LO, HI, true); break;
		}
	});
	int bad = 0;
	for (int it = 0; it < iters; it++)
	{
		Ctx c{};
		fill(c.rs); fill(c.rt); fill(c.rd); fill(c.lo); fill(c.hi);
		c.sa = op == Op::QFSRV ? (u32)(it % 16) : 0;
		if (alias == 1) c.rd = c.rs;
		if (alias == 2) c.rd = c.rt;

		Io io{};
		memcpy(io.vin[1], &c.rs, 16); memcpy(io.vin[2], &c.rt, 16); memcpy(io.vin[3], &c.rd, 16);
		memcpy(io.vin[4], &c.lo, 16); memcpy(io.vin[5], &c.hi, 16);
		g_state.sa = c.sa;
		jit.run(io, g_state);

		Ctx e = c; // expected
		switch (op)
		{
			case Op::PMULTH: ref_PMULTH(e, wd); break;
			case Op::PMADDH: ref_PMADDH(e, wd, false); break;
			case Op::PMSUBH: ref_PMADDH(e, wd, true); break;
			case Op::PHMADH: ref_PHMADH(e, wd); break;
			case Op::PHMSBH: ref_PHMSBH(e, wd); break;
			case Op::QFSRV: ref_QFSRV(e); break;
			case Op::PMULTUW: ref_PMULTUW(e, wd, false); break;
			case Op::PMADDUW: ref_PMULTUW(e, wd, true); break;
		}
		// Expected host registers after the op: s/t keep their values unless they are d.
		R128 exp_s = alias == 1 ? e.rd : c.rs, exp_t = alias == 2 ? e.rd : c.rt;
		R128 exp_d = e.rd;
		const bool lohi = op != Op::QFSRV;
		bool ok = !memcmp(io.vout[1], &exp_s, 16) && !memcmp(io.vout[2], &exp_t, 16);
		if (alias == 0) ok = ok && !memcmp(io.vout[3], &exp_d, 16);
		if (lohi) ok = ok && !memcmp(io.vout[4], &e.lo, 16) && !memcmp(io.vout[5], &e.hi, 16);
		if (!ok && bad++ < 3)
		{
			printf("MISMATCH %s wd=%d alias=%d sa=%u\n", kNames[(int)op], wd, alias, c.sa);
			auto pr = [](const char* n, const void* a, const void* b) { const u32* x = (const u32*)a; const u32* y = (const u32*)b;
				printf("  %-3s got %08x %08x %08x %08x  want %08x %08x %08x %08x\n", n, x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3]); };
			pr("d", io.vout[alias == 1 ? 1 : alias == 2 ? 2 : 3], &exp_d);
			if (lohi) { pr("lo", io.vout[4], &e.lo); pr("hi", io.vout[5], &e.hi); }
		}
	}
	return bad;
}

int main()
{
	int failures = 0;
	for (int op = 0; op < 8; op++)
	{
		for (int wd = 0; wd < 2; wd++)
		{
			if (op == (int)Op::QFSRV && !wd) continue; // QFSRV with Rd = 0 is a no-op
			for (int alias = 0; alias < 3; alias++)
			{
				if (!wd && alias) continue;
				failures += run((Op)op, wd, alias, 20000);
			}
		}
		Jit j([&] {
			switch ((Op)op) {
				case Op::PMULTH: MMIEmit::PMULTH(a64::q3, true, a64::q1, a64::q2, a64::q4, a64::q5); break;
				case Op::PMADDH: case Op::PMSUBH: MMIEmit::PMADDH_PMSUBH(a64::q3, true, a64::q1, a64::q2, a64::q4, a64::q5, op == (int)Op::PMSUBH); break;
				case Op::PHMADH: MMIEmit::PHMADH(a64::q3, true, a64::q1, a64::q2, a64::q4, a64::q5); break;
				case Op::PHMSBH: MMIEmit::PHMSBH(a64::q3, true, a64::q1, a64::q2, a64::q4, a64::q5); break;
				case Op::QFSRV: MMIEmit::QFSRV(a64::q3, a64::q1, a64::q2, PTR_CPU(sa), PTR_CPU(shuffle.data[0xE4][0]), a64::w16); break;
				case Op::PMULTUW: case Op::PMADDUW: MMIEmit::PMULTUW_PMADDUW(a64::q3, true, a64::q1, a64::q2, a64::q4, a64::q5, op == (int)Op::PMADDUW); break;
			}
		});
		printf("%-7s %zu instructions\n", kNames[op], j.insns);
	}
	printf("%s (%d mismatching cases)\n", failures ? "FAILED" : "ALL EQUIVALENT", failures);
	return failures != 0;
}
