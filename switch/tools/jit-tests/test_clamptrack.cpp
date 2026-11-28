// Soundness of ClampTracker: random A64 sequences, run under qemu; every register the tracker calls clamped
// must hold in-range (and, with FZ, non-denormal) values in all four lanes.
#include "harness.h"
#include "ClampTracker.h"
#include <random>
#include <vector>
struct CState { alignas(16) u32 maxvals[4] = {0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff}; alignas(16) u32 minvals[4] = {0xff7fffff, 0xff7fffff, 0xff7fffff, 0xff7fffff}; };
static CState g_c;
static std::mt19937 rng(77);
static a64::VRegister V(int r) { return a64::VRegister(r, 128); }
// Same bookkeeping as mVUclamp1/mVUsyncClampedRegs.
static u32 g_mask; static ptrdiff_t g_off; static u32 g_skipped, g_emitted;
static u32 Sync() {
	const ptrdiff_t off = armAsm->GetCursorOffset();
	const u8* buf = armAsm->GetCursorAddress<const u8*>() - off;
	for (ptrdiff_t at = g_off; at + 4 <= off; at += 4) { u32 w; std::memcpy(&w, buf + at, 4); g_mask = ClampTracker::Apply(w, g_mask); }
	g_off = off; return g_mask;
}
static void Clamp(int d, bool scalar) {
	const u32 bit = 1u << d;
	if (Sync() & bit) { g_skipped++; return; }
	if (scalar) { armAsm->Fminnm(V(d).S(), V(d).S(), a64::s27); armAsm->Fmaxnm(V(d).S(), V(d).S(), a64::s28); }
	else { armAsm->Fminnm(V(d).V4S(), V(d).V4S(), a64::v27.V4S()); armAsm->Fmaxnm(V(d).V4S(), V(d).V4S(), a64::v28.V4S()); }
	g_mask = Sync() | bit; g_emitted++;
}
static void EmitRandom(int n) {
	g_mask = 0; g_off = armAsm->GetCursorOffset();
	armAsm->Ldr(a64::q27, a64::MemOperand(a64::x27, offsetof(CState, maxvals)));
	armAsm->Ldr(a64::q28, a64::MemOperand(a64::x27, offsetof(CState, minvals)));
	for (int k = 0; k < n; k++) {
		const int d = rng() % 8, a = rng() % 8, b = rng() % 8;
		switch (rng() % 22) {
			case 0: case 1: case 2: Clamp(d, false); break;
			case 3: Clamp(d, true); break;
			case 4: armAsm->Fadd(V(d).V4S(), V(a).V4S(), V(b).V4S()); break;
			case 5: armAsm->Fmul(V(d).V4S(), V(a).V4S(), V(b).V4S()); break;
			case 6: armAsm->Mov(V(d).V16B(), V(a).V16B()); break;
			case 7: armAsm->Mov(V(d).V8B(), V(a).V8B()); break;
			case 8: armAsm->Dup(V(d).V4S(), V(a).V4S(), rng() % 4); break;
			case 9: armAsm->Dup(V(d).V2S(), V(a).V4S(), rng() % 4); break;
			case 10: armAsm->Ins(V(d).V4S(), rng() % 4, V(a).V4S(), rng() % 4); break;
			case 11: armAsm->Ldr(a64::QRegister(d), a64::MemOperand(a64::x28, offsetof(Io, vin) + 16 * (rng() % 8))); break;
			case 12: armAsm->Ld1(a64::VRegister(d, 128).V4S(), a64::VRegister((d + 1) % 32, 128).V4S(), a64::MemOperand(a64::x28, offsetof(Io, vin))); break;
			case 13: armAsm->Umov(a64::WRegister(d), V(a).V4S(), 0); armAsm->Add(a64::WRegister(d), a64::WRegister(d), 1); break;
			case 14: armAsm->Ldr(a64::w9, a64::MemOperand(a64::x28, offsetof(Io, vin) + 4 * (rng() % 32))); armAsm->Fmov(a64::SRegister(d), a64::w9); break;
			case 15: armAsm->Movi(V(d).V4S(), 0xff, a64::LSL, 24 * (rng() % 2)); break;
			case 16: armAsm->Fadd(V(d).S(), V(a).S(), V(b).S()); break;
			case 17: armAsm->Fneg(V(d).V4S(), V(a).V4S()); break;
			case 18: armAsm->Fabs(V(d).V4S(), V(a).V4S()); break;
			case 19: armAsm->And(V(d).V16B(), V(a).V16B(), V(b).V16B()); break;
			case 20: armAsm->Bic(V(d).V16B(), V(a).V16B(), V(b).V16B()); break;
			case 21: armAsm->Eor(V(d).V16B(), V(a).V16B(), V(a).V16B()); break;
		}
	}
	Sync();
}
static bool InRange(u32 v, bool fz) { if ((v & 0x7f800000u) == 0x7f800000u) return false; if (fz && (v & 0x7f800000u) == 0 && (v & 0x007fffffu)) return false; return true; }
static bool TestRangePreservingPropagation() {
	g_mask = 0; g_off = 0; g_skipped = g_emitted = 0;
	Jit j([&] {
		g_off = armAsm->GetCursorOffset();
		armAsm->Ldr(a64::q27, a64::MemOperand(a64::x27, offsetof(CState, maxvals)));
		armAsm->Ldr(a64::q28, a64::MemOperand(a64::x27, offsetof(CState, minvals)));
		Clamp(0, false);
		armAsm->Fneg(V(1).V4S(), V(0).V4S());
		Clamp(1, false);
		armAsm->Fabs(V(2).V4S(), V(1).V4S());
		Clamp(2, false);
		armAsm->And(V(3).V16B(), V(2).V16B(), V(4).V16B());
		Clamp(3, false);
		armAsm->Bic(V(5).V16B(), V(3).V16B(), V(4).V16B());
		Clamp(5, false);
		armAsm->Eor(V(6).V16B(), V(7).V16B(), V(7).V16B());
		Clamp(6, false);
		armAsm->Mov(V(7).V16B(), V(3).V16B());
		armAsm->Ins(V(7).V4S(), 1, V(5).V4S(), 2);
		Clamp(7, false);
	});
	constexpr u32 expected_mask = 0xefu;
	if ((g_mask & expected_mask) != expected_mask || g_emitted != 1 || g_skipped != 6) {
		std::printf("range-preserving clamp propagation failed: mask=%08x emitted=%u skipped=%u\n", g_mask, g_emitted, g_skipped);
		return false;
	}
	const u32 values[] = {0u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u,
		0x7f800001u, 0x7f7fffffu, 0xff7fffffu, 0x00000001u, 0x807fffffu};
	for (u32 value : values) {
		Io io = {};
		for (u32 lane = 0; lane < 4; lane++) {
			io.vin[0][lane] = value;
			io.vin[4][lane] = 0xffffffffu;
		}
		j.run(io, *reinterpret_cast<State*>(&g_c));
		for (u32 reg = 0; reg < 8; reg++) {
			if (!(expected_mask & (1u << reg))) continue;
			for (u32 lane = 0; lane < 4; lane++)
				if (!InRange(io.vout[reg][lane], false)) {
					std::printf("range-preserving operation produced out-of-range v%u lane %u = %08x\n", reg, lane, io.vout[reg][lane]);
					return false;
				}
		}
	}
	return true;
}
int main() {
	if (!TestRangePreservingPropagation()) return 1;
	const u32 sp[] = {0, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7f800001u, 0x7f7fffffu, 0xff7fffffu, 0x00000001u, 0x807fffffu, 0x3f800000u, 0x7f000000u};
	u64 checked = 0, claimed = 0;
	for (int prog = 0; prog < 4000; prog++) {
		const int n = 4 + rng() % 24;
		const u32 seed = rng();
		rng.seed(seed);
		Jit j([&] { EmitRandom(n); });
		const u32 mask = g_mask;
		rng.seed(seed * 2654435761u + 1);
		for (int run = 0; run < 50; run++) {
			Io io = {};
			for (int r = 0; r < 8; r++) for (int l = 0; l < 4; l++) io.vin[r][l] = (rng() & 1) ? rng() : sp[rng() % 13];
			const bool fz = rng() & 1;
			io.fpcr = fz ? (1u << 24) | (1u << 25) : 0;
			j.run(io, *reinterpret_cast<State*>(&g_c));
			for (int r = 0; r < 8; r++) {
				if (!(mask & (1u << r))) continue;
				claimed++;
				for (int l = 0; l < 4; l++)
					if (!InRange(io.vout[r][l], fz)) { std::printf("UNSOUND: prog %d run %d v%d lane %d = %08x (fz %d)\n", prog, run, r, l, io.vout[r][l], fz); j.~Jit(); return 1; }
			}
			checked++;
		}
		rng.seed(seed ^ 0x9e3779b9u);
	}
	std::printf("%llu runs, %llu clamped-register claims, all in range; clamps emitted %u, skipped as redundant %u\n", (unsigned long long)checked, (unsigned long long)claimed, g_emitted, g_skipped);
}
