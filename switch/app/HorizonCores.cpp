// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "HorizonCores.h"

#include "common/Horizon/Horizon.h"

#include <atomic>
#include <new>
#include <pthread.h>

namespace HorizonCores
{
namespace
{
	u64 s_allowed_mask = 0;
	std::atomic<u32> s_helper_mask{0};
} // namespace

void Initialize()
{
	u64 allowed = 0;
	if (R_FAILED(svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || allowed == 0)
		allowed = 0x7;
	s_allowed_mask = allowed;

	// Same order as VMManager::SetEmuThreadAffinities() on Switch: the EE thread gets the first allowed
	// core, the VU1 thread the second, the GS thread the third. Helpers get everything after the VU1 core.
	u32 cores_seen = 0;
	u32 helper = 0;
	for (u32 core = 0; core < 4; core++)
	{
		if (!(allowed & (1ull << core)))
			continue;
		if (cores_seen++ >= 2)
			helper |= 1u << core;
	}
	s_helper_mask.store(helper, std::memory_order_release);
}

std::uint64_t GetAllowedCoreMask()
{
	return s_allowed_mask;
}

std::uint32_t GetHelperCoreMask()
{
	return s_helper_mask.load(std::memory_order_acquire);
}
} // namespace HorizonCores

//------------------------------------------------------------------------------------------------
// pthread_create is wrapped at link time (-Wl,--wrap=pthread_create, see CMakeLists.txt) so this covers
// std::thread, PCSX2's Threading::Thread and threads created inside static libraries (Mesa/NXVK,
// libusbhsfs).

namespace
{
	struct HelperThreadStart
	{
		void* (*start)(void*);
		void* arg;
		u32 mask;
	};

	void* HelperThreadEntry(void* param)
	{
		const HelperThreadStart info = *static_cast<HelperThreadStart*>(param);
		delete static_cast<HelperThreadStart*>(param);

		// Prefer the highest core in the mask (core 3 when available); the kernel can still run the
		// thread on the other cores in the mask.
		const s32 preferred_core = 31 - __builtin_clz(info.mask);
		svcSetThreadCoreMask(CUR_THREAD_HANDLE, preferred_core, info.mask);

		return info.start(info.arg);
	}
} // namespace

extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg)
{
	const u32 mask = HorizonCores::GetHelperCoreMask();
	if (mask == 0)
		return __real_pthread_create(thread, attr, start, arg);

	HelperThreadStart* const info = new (std::nothrow) HelperThreadStart{start, arg, mask};
	if (!info)
		return __real_pthread_create(thread, attr, start, arg);

	const int result = __real_pthread_create(thread, attr, &HelperThreadEntry, info);
	if (result != 0)
		delete info;
	return result;
}
