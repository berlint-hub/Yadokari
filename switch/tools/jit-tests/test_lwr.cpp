// recLWR mask/merge sequence (non-zero offset path + shift-0 path) vs the interpreter, under qemu-aarch64.
#include "vixl/aarch64/macro-assembler-aarch64.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <sys/mman.h>
namespace a64 = vixl::aarch64;
using u32 = uint32_t; using u64 = uint64_t; using s32 = int32_t; using s64 = int64_t;
a64::MacroAssembler* armAsm;
#define EAX a64::w0
#define ECX a64::w1
#define EDX a64::w2

// io[0] = rt in/out, io[1] = loaded word (upper half garbage on purpose), io[2] = addr
static void emit(bool fixed)
{
	const int treg = 6;
	const a64::WRegister temp(5);
	armAsm->Ldr(a64::x6, a64::MemOperand(a64::x9, 0));
	armAsm->Ldr(a64::x0, a64::MemOperand(a64::x9, 8));
	armAsm->Ldr(temp, a64::MemOperand(a64::x9, 16));
	armAsm->And(temp, temp, 3);
	a64::Label nomask, end;
	armAsm->Cbz(temp, &nomask);
	armAsm->Lsl(temp, temp, 3);
	armAsm->Mov(ECX, 24);
	armAsm->Sub(ECX, ECX, temp);
	if (fixed)
	{
		armAsm->Mov(EDX.X(), ~UINT64_C(0xff));
		armAsm->Lsl(EDX.X(), EDX.X(), ECX.X());
		armAsm->And(a64::XRegister(treg), a64::XRegister(treg), EDX.X());
		armAsm->Mov(ECX, temp);
		armAsm->Lsr(EAX, EAX, ECX);
		armAsm->Orr(a64::XRegister(treg), a64::XRegister(treg), EAX.X());
	}
	else
	{
		auto reg32 = a64::WRegister(treg);
		armAsm->Mov(EDX, 0xffffff00);
		armAsm->Lsl(EDX, EDX, ECX);
		armAsm->And(reg32, reg32, EDX);
		armAsm->Mov(ECX, temp);
		armAsm->Lsr(EAX, EAX, ECX);
		armAsm->Orr(reg32, reg32, EAX);
	}
	armAsm->B(&end);
	armAsm->Bind(&nomask);
	armAsm->Sxtw(a64::XRegister(treg), EAX);
	armAsm->Bind(&end);
	armAsm->Str(a64::x6, a64::MemOperand(a64::x9, 0));
}

typedef void (*Fn)(u64*);
static Fn build(bool fixed)
{
	a64::MacroAssembler masm; armAsm = &masm;
	masm.Mov(a64::x9, a64::x0);
	emit(fixed);
	masm.Ret();
	masm.FinalizeCode();
	size_t sz = masm.GetSizeOfCodeGenerated();
	void* mem = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memcpy(mem, masm.GetBuffer()->GetStartAddress<void*>(), sz);
	mprotect(mem, 4096, PROT_READ | PROT_EXEC);
	__builtin___clear_cache((char*)mem, (char*)mem + sz);
	return (Fn)mem;
}

static u64 interp(u64 rt, u32 mem, u32 addr)
{
	static const u32 LWR_MASK[4] = {0x000000, 0xff000000, 0xffff0000, 0xffffff00};
	static const u32 LWR_SHIFT[4] = {0, 8, 16, 24};
	const u32 shift = addr & 3;
	const u32 m = ((u32)rt & LWR_MASK[shift]) | (mem >> LWR_SHIFT[shift]);
	if (shift == 0) return (u64)(s64)(s32)m;
	return (rt & 0xFFFFFFFF00000000ull) | m;
}

int main()
{
	Fn f_new = build(true), f_old = build(false);
	std::mt19937_64 rng(1);
	u64 bad_new = 0, bad_old = 0, n = 0;
	for (int i = 0; i < 1000000; i++)
	{
		u64 rt = rng(); u32 mem = (u32)rng(); u32 addr = (u32)rng();
		if (i & 1) rt = (u64)(s64)(s32)rt; // sign-extended values too
		u64 garbage = rng() & 0xFFFFFFFF00000000ull;
		const u64 want = interp(rt, mem, addr);
		u64 io[3] = {rt, garbage | mem, addr};
		f_new(io); bad_new += io[0] != want;
		u64 io2[3] = {rt, garbage | mem, addr};
		f_old(io2); bad_old += io2[0] != want;
		n++;
	}
	std::printf("%llu cases: fixed sequence %llu mismatches, old sequence %llu mismatches\n",
		(unsigned long long)n, (unsigned long long)bad_new, (unsigned long long)bad_old);
	return bad_new != 0;
}
