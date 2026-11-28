// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Host CPU information on the Switch, where cpuinfo cannot identify the processor.

#pragma once

#include "common/Pcsx2Types.h"

namespace HorizonSystemInfo
{
	/// e.g. "NVIDIA Tegra X1+ (Mariko, OLED)". Never empty.
	const char* GetCPUName();

	/// CPU cores of the SoC that run applications (the four Cortex-A57).
	static constexpr u32 CPU_CORES = 4;

	/// Current CPU clock in MHz, or 0 if it cannot be read. Reads it at most every 250 ms and returns the last value
	/// in between, so it can be called every frame. Thread-safe.
	u32 GetCPUClockMHz();

	/// Closes the clock service session. Called once before the process exits.
	void Shutdown();
} // namespace HorizonSystemInfo
