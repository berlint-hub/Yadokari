// Minimal JIT harness: emit a body with the project's vixl, run it natively (under qemu-aarch64).
#pragma once

#include "vixl/aarch64/macro-assembler-aarch64.h"
#include "vixl/aarch64/disasm-aarch64.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sys/mman.h>

namespace a64 = vixl::aarch64;
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s64 = int64_t;

// Same scratch registers as pcsx2/arm64/AsmHelpers.h
#define RQSCRATCH a64::q30
#define RQSCRATCH2 a64::q31
#define RQSCRATCH3 a64::q29
#define RSTATE_CPU a64::x27

inline a64::MacroAssembler* armAsm = nullptr;

#include "ShuffleLanes.h"

struct State
{
	alignas(16) ShuffleLanes shuffle;
	alignas(16) u32 movmsk_weights[4] = {1, 2, 4, 8};
	alignas(16) u32 movmsk_weights_rev[4] = {8, 4, 2, 1};
	alignas(16) u32 sa = 0;
};
#define ARM_FIELD_OFFSET(type, field) (static_cast<size_t>(reinterpret_cast<uintptr_t>(&reinterpret_cast<type*>(0)->field)))
#define PTR_CPU(field) a64::MemOperand(RSTATE_CPU, ARM_FIELD_OFFSET(State, field))

struct alignas(16) Io
{
	u32 vin[8][4];
	u32 vout[8][4];
	u32 win[4];
	u32 wout[4];
	u64 fpcr;
};

// Inputs: q0..q7 = vin, w0..w3 = win. Outputs: q0..q7 -> vout, w0..w3 -> wout.
struct Jit
{
	void* mem = nullptr;
	size_t size = 0;
	size_t insns = 0;
	ptrdiff_t body_start = 0;
	void (*fn)(Io*, State*) = nullptr;

	explicit Jit(const std::function<void()>& body, bool print = false)
	{
		a64::MacroAssembler masm;
		armAsm = &masm;
		masm.Stp(a64::x27, a64::x28, a64::MemOperand(a64::sp, -16, a64::PreIndex));
		masm.Mov(a64::x28, a64::x0);
		masm.Mov(a64::x27, a64::x1);
		masm.Mrs(a64::x9, a64::FPCR);
		masm.Ldr(a64::x10, a64::MemOperand(a64::x28, offsetof(Io, fpcr)));
		masm.Msr(a64::FPCR, a64::x10);
		for (int i = 0; i < 8; i++)
			masm.Ldr(a64::VRegister(i, 128), a64::MemOperand(a64::x28, offsetof(Io, vin) + i * 16));
		for (int i = 0; i < 4; i++)
			masm.Ldr(a64::WRegister(i), a64::MemOperand(a64::x28, offsetof(Io, win) + i * 4));
		masm.Str(a64::x9, a64::MemOperand(a64::sp, -16, a64::PreIndex));

		const ptrdiff_t body_offset = masm.GetCursorOffset();
		body();
		insns = (masm.GetCursorOffset() - body_offset) / 4;
		body_start = body_offset;

		masm.Ldr(a64::x9, a64::MemOperand(a64::sp, 16, a64::PostIndex));
		for (int i = 0; i < 8; i++)
			masm.Str(a64::VRegister(i, 128), a64::MemOperand(a64::x28, offsetof(Io, vout) + i * 16));
		for (int i = 0; i < 4; i++)
			masm.Str(a64::WRegister(i), a64::MemOperand(a64::x28, offsetof(Io, wout) + i * 4));
		masm.Msr(a64::FPCR, a64::x9);
		masm.Ldp(a64::x27, a64::x28, a64::MemOperand(a64::sp, 16, a64::PostIndex));
		masm.Ret();
		masm.FinalizeCode();
		armAsm = nullptr;

		size = masm.GetSizeOfCodeGenerated();
		mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		memcpy(mem, masm.GetBuffer()->GetStartAddress<void*>(), size);
		mprotect(mem, size, PROT_READ | PROT_EXEC);
		__builtin___clear_cache((char*)mem, (char*)mem + size);
		fn = reinterpret_cast<void (*)(Io*, State*)>(mem);

		if (print)
		{
			a64::PrintDisassembler dis(stdout);
			const auto* p = reinterpret_cast<const a64::Instruction*>((const u8*)mem + body_offset);
			dis.DisassembleBuffer(p, insns * 4);
		}
	}
	~Jit() { munmap(mem, size); }
	void run(Io& io, State& st) const { fn(&io, &st); }
};
