// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>

// CPU core placement for threads on the Tegra X1.
//
// Horizon starts every thread created through pthread_create (std::thread included) on the process'
// default core, core 0, and doesn't let it leave that core. The EE thread runs on core 0 at a higher
// priority, so audio, ISO reading, USB, input and the Vulkan driver's helper threads all only got CPU
// time when the EE was idle. These "helper" threads are now moved, as they start, to the cores that the
// EE and VU1 threads don't use: the GS core, plus core 3 when the process is allowed to use it (e.g. a
// forwarder whose NPDM allows cores 0-3). VMManager still pins the EE/VU1/GS threads themselves.
namespace HorizonCores
{
	// Call first thing in main(), before any thread is created.
	void Initialize();

	// Cores the process may use (InfoType_CoreMask), e.g. 0x7 for cores 0-2 or 0xF for cores 0-3.
	std::uint64_t GetAllowedCoreMask();

	// Core mask new helper threads are moved to, or 0 when they are left alone.
	std::uint32_t GetHelperCoreMask();
} // namespace HorizonCores
