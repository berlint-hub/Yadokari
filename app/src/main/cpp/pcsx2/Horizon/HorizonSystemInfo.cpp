// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Horizon/HorizonSystemInfo.h"

#include "common/Horizon/Horizon.h" // libnx, with its u128/s128 renamed

#include <cstdio>
#include <mutex>

namespace HorizonSystemInfo
{
	namespace
	{
		std::once_flag s_name_once;
		char s_name[64];

		std::mutex s_clock_mutex;
		bool s_clock_opened = false; // tried to open the clock service
		bool s_clock_ok = false;
		bool s_clock_use_pcv = false; // pcv before 8.0.0, clkrst after
		ClkrstSession s_clock_session = {};
		u64 s_clock_last_tick = 0;
		u32 s_clock_last_mhz = 0;

		void BuildName()
		{
			const char* soc = "NVIDIA Tegra X1";
			const char* model = nullptr;
			if (R_SUCCEEDED(setsysInitialize()))
			{
				SetSysProductModel product = SetSysProductModel_Invalid;
				if (R_SUCCEEDED(setsysGetProductModel(&product)))
				{
					switch (product)
					{
						case SetSysProductModel_Nx:     model = "Erista"; break;
						case SetSysProductModel_Copper: model = "Erista, Copper"; break;
						case SetSysProductModel_Iowa:   soc = "NVIDIA Tegra X1+"; model = "Mariko"; break;
						case SetSysProductModel_Hoag:   soc = "NVIDIA Tegra X1+"; model = "Mariko, Lite"; break;
						case SetSysProductModel_Calcio: soc = "NVIDIA Tegra X1+"; model = "Mariko, Calcio"; break;
						case SetSysProductModel_Aula:   soc = "NVIDIA Tegra X1+"; model = "Mariko, OLED"; break;
						default: break;
					}
				}
				setsysExit();
			}
			if (model)
				std::snprintf(s_name, sizeof(s_name), "%s (%s)", soc, model);
			else
				std::snprintf(s_name, sizeof(s_name), "%s", soc);
		}

		// With s_clock_mutex held.
		void OpenClock()
		{
			s_clock_opened = true;
			if (hosversionAtLeast(8, 0, 0))
			{
				if (R_FAILED(clkrstInitialize()))
					return;
				if (R_FAILED(clkrstOpenSession(&s_clock_session, PcvModuleId_CpuBus, 3)))
				{
					clkrstExit();
					return;
				}
				s_clock_ok = true;
			}
			else
			{
				if (R_FAILED(pcvInitialize()))
					return;
				s_clock_use_pcv = true;
				s_clock_ok = true;
			}
		}
	} // namespace

	const char* GetCPUName()
	{
		std::call_once(s_name_once, BuildName);
		return s_name;
	}

	u32 GetCPUClockMHz()
	{
		std::lock_guard lock(s_clock_mutex);
		const u64 now = armGetSystemTick();
		if (s_clock_last_tick != 0 && armTicksToNs(now - s_clock_last_tick) < 250000000ull)
			return s_clock_last_mhz;
		s_clock_last_tick = now;

		if (!s_clock_opened)
			OpenClock();
		if (!s_clock_ok)
			return (s_clock_last_mhz = 0);

		u32 hz = 0;
		const Result rc = s_clock_use_pcv ? pcvGetClockRate(PcvModule_CpuBus, &hz) : clkrstGetClockRate(&s_clock_session, &hz);
		s_clock_last_mhz = R_SUCCEEDED(rc) ? (hz + 500000) / 1000000 : 0;
		return s_clock_last_mhz;
	}

	void Shutdown()
	{
		std::lock_guard lock(s_clock_mutex);
		if (s_clock_ok)
		{
			if (s_clock_use_pcv)
			{
				pcvExit();
			}
			else
			{
				clkrstCloseSession(&s_clock_session);
				clkrstExit();
			}
		}
		s_clock_ok = false;
		s_clock_last_mhz = 0;
		// Leave s_clock_opened set: nothing reopens the service after this.
	}
} // namespace HorizonSystemInfo
