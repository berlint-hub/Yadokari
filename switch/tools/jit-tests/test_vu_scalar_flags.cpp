// Check the scalar VU flag fast path against the existing vector implementation, including
// signed zero, NaNs, infinities, denormals, and FPCR.FZ on/off.
#include "harness.h"
#include <random>

static State g_state;
static a64::VRegister V(int r) { return a64::VRegister(r, 128); }

static Jit MakeVectorFlagsJit()
{
	return Jit([&] {
		armAsm->Ldr(a64::q31, a64::MemOperand(a64::x27, offsetof(State, movmsk_weights)));
		armAsm->Sshr(V(1).V4S(), V(0).V4S(), 31);
		armAsm->Fcmeq(V(2).V4S(), V(0).V4S(), 0.0);
		armAsm->And(V(1).V16B(), V(1).V16B(), V(31).V16B());
		armAsm->And(V(2).V16B(), V(2).V16B(), V(31).V16B());
		armAsm->Sli(V(2).V4S(), V(1).V4S(), 4);
		armAsm->Addv(V(2).S(), V(2).V4S());
		armAsm->Fmov(a64::w0, V(2).S());
		armAsm->And(a64::w0, a64::w0, 0x11);
	});
}

static Jit MakeScalarFlagsJit()
{
	return Jit([&] {
		armAsm->Fmov(a64::w0, V(0).S());
		armAsm->Lsr(a64::w0, a64::w0, 31);
		armAsm->Fcmeq(V(1).S(), V(0).S(), 0.0);
		armAsm->Fmov(a64::w1, V(1).S());
		armAsm->Lsr(a64::w1, a64::w1, 31);
		armAsm->Orr(a64::w0, a64::w0, a64::Operand(a64::w1, a64::LSL, 4));
	});
}

int main()
{
	Jit vector_flags = MakeVectorFlagsJit();
	Jit scalar_flags = MakeScalarFlagsJit();
	std::mt19937 rng(0xA57u);
	const u32 special[] = {
		0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u,
		0x007fffffu, 0x807fffffu, 0x00800000u, 0x80800000u,
		0x3f800000u, 0xbf800000u, 0x7f7fffffu, 0xff7fffffu,
		0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u,
		0x7f800001u, 0xff800001u, 0x7fffffffu, 0xffffffffu,
	};

	for (u32 fz = 0; fz < 2; fz++)
	{
		for (u32 i = 0; i < 100000; i++)
		{
			Io io = {};
			io.fpcr = fz ? (1ull << 24) | (1ull << 25) : 0;
			io.vin[0][0] = (i < (sizeof(special) / sizeof(special[0]))) ? special[i] : rng();
			vector_flags.run(io, g_state);
			const u32 expected = io.wout[0];
			io.wout[0] = 0;
			scalar_flags.run(io, g_state);
			if (io.wout[0] != expected)
			{
				std::printf("Mismatch (FZ=%u, input=%08x): vector=%02x scalar=%02x\n",
					fz, io.vin[0][0], expected, io.wout[0]);
				return 1;
			}
		}
	}

	std::printf("200000 scalar flag cases matched the vector implementation\n");
	return 0;
}
