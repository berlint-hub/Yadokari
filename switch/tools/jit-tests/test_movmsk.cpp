// armMOVMSKPS: old (Movi constant) vs new (no constant, no state register), run under qemu-aarch64.
#include "harness.h"
#include <random>

static State g_state;

static void old_MOVMSKPS(const a64::Register& reg32, const a64::VRegister& regQ)
{
	armAsm->Movi(RQSCRATCH2.V2D(), 0x0000'0008'0000'0004, 0x0000'0002'0000'0001);
	armAsm->Sshr(RQSCRATCH.V4S(), regQ.V4S(), 31);
	armAsm->And(RQSCRATCH.V16B(), RQSCRATCH2.V16B(), RQSCRATCH.V16B());
	armAsm->Addv(RQSCRATCH.S(), RQSCRATCH.V4S());
	armAsm->Fmov(reg32, RQSCRATCH.S());
}

// Must match pcsx2/arm64/VixlHelpers.cpp
static void new_MOVMSKPS(const a64::Register& reg32, const a64::VRegister& regQ)
{
	armAsm->Ushr(RQSCRATCH.V4S(), regQ.V4S(), 31);
	armAsm->Usra(RQSCRATCH.V2D(), RQSCRATCH.V2D(), 31);
	armAsm->Xtn(RQSCRATCH.V2S(), RQSCRATCH.V2D());
	armAsm->Usra(RQSCRATCH.D(), RQSCRATCH.D(), 30);
	armAsm->Fmov(reg32, RQSCRATCH.S());
}

int main()
{
	// q0..q3 inputs; results to w0 (old) and w1 (new). Also new with source == RQSCRATCH-free regs and reg32 aliasing.
	Jit jold([] { old_MOVMSKPS(a64::w0, a64::q0); old_MOVMSKPS(a64::w1, a64::q1); old_MOVMSKPS(a64::w2, a64::q2); });
	Jit jnew([] {
		armAsm->Mov(a64::x27, 0); // the state register must not matter
		new_MOVMSKPS(a64::w0, a64::q0); new_MOVMSKPS(a64::w1, a64::q1); new_MOVMSKPS(a64::w2, a64::q2); }, true);
	std::printf("new: %zu instructions per call x3\n", jnew.insns);

	std::mt19937 rng(42);
	const u32 specials[] = {0, 0x80000000u, 0x7fffffffu, 0xffffffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x00000001u, 0x80000001u, 0x3f800000u, 0xbf800000u};
	u64 n = 0;
	for (int iter = 0; iter < 2000000; iter++)
	{
		Io a = {}, b = {};
		for (int r = 0; r < 3; r++)
			for (int l = 0; l < 4; l++)
			{
				const u32 v = (rng() & 3) ? rng() : specials[rng() % (sizeof(specials) / sizeof(specials[0]))];
				a.vin[r][l] = b.vin[r][l] = v;
			}
		a.fpcr = b.fpcr = (iter & 1) ? (1u << 24) | (1u << 25) : 0; // FZ/DN on and off
		jold.run(a, g_state);
		jnew.run(b, g_state);
		for (int r = 0; r < 3; r++)
		{
			u32 expect = 0;
			for (int l = 0; l < 4; l++)
				expect |= (a.vin[r][l] >> 31) << l;
			if (a.wout[r] != expect || b.wout[r] != expect)
			{
				std::printf("MISMATCH iter %d reg %d: old %x new %x expect %x\n", iter, r, a.wout[r], b.wout[r], expect);
				return 1;
			}
			// inputs must be preserved
			for (int l = 0; l < 4; l++)
				if (b.vout[r][l] != b.vin[r][l]) { std::printf("input clobbered\n"); return 1; }
		}
		n += 3;
	}
	std::printf("%llu MOVMSKPS results match (old, new, reference)\n", (unsigned long long)n);
	return 0;
}
