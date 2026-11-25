// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Switch performance diagnostics: a periodic summary in the log (speed, thread load, waits, GS and
// recompiler activity per frame), a line for every long frame with what happened in it, and a
// sampling profiler for the EE, VU1 and GS threads (logs/profile.txt).

#pragma once

#include "common/Pcsx2Defs.h"

#include <atomic>

namespace HorizonProfiler
{
	enum class Counter : u32
	{
		// EE recompiler (EE thread)
		EEBlocks,
		EECompileNs,
		EEClears, // recClear() calls that reached the block search (self-modifying code, DMA into code)
		EEClearHits, // ... that invalidated compiled blocks
		EEDiscards, // manual block integrity check failures
		EEResets, // full recompiler resets (cache full or code cleared)

		// microVU, VU0 then VU1 (same layout, see VUCounter())
		VU0Blocks,
		VU0CompileNs,
		VU0Programs, // new program instances (micro memory changed into an uncached program)
		VU0CacheResets,
		VU1Blocks,
		VU1CompileNs,
		VU1Programs,
		VU1CacheResets,

		// GS thread / Vulkan
		VKPipelines,
		VKPipelineNs,
		GPUSyncs, // command buffer submits that waited for the GPU to finish (readbacks)
		GPUTimeUs, // GPU time of presented frames (timestamp queries)
		GPUTimedFrames,

		// Waits, mirrored from PerformanceMetrics
		EEWaitVUNs,
		EEWaitGSNs,
		EEWaitVsyncNs,
		GSWaitWorkNs,
		GSWaitSwapNs,
		GSWaitGPUNs,

		Count
	};

	extern std::atomic<u64> g_counters[static_cast<u32>(Counter::Count)];

	__fi void Add(Counter counter, u64 value = 1)
	{
		g_counters[static_cast<u32>(counter)].fetch_add(value, std::memory_order_relaxed);
	}

	__fi Counter VUCounter(u32 vu_index, Counter vu0_counter)
	{
		constexpr u32 stride = static_cast<u32>(Counter::VU1Blocks) - static_cast<u32>(Counter::VU0Blocks);
		return static_cast<Counter>(static_cast<u32>(vu0_counter) + vu_index * stride);
	}

	u64 GetTimeNs();

	/// Counts one event and the time spent until the end of the scope. Nested scopes of the same kind
	/// on a thread (microVU compiling branch targets recursively) count the event but not the time again.
	class ScopedCompile
	{
	public:
		__fi ScopedCompile(Counter count, Counter time)
			: m_time(time)
			, m_outermost(s_depth++ == 0)
			, m_start(m_outermost ? GetTimeNs() : 0)
		{
			Add(count);
		}
		__fi ~ScopedCompile()
		{
			s_depth--;
			if (m_outermost)
				Add(m_time, GetTimeNs() - m_start);
		}

		ScopedCompile(const ScopedCompile&) = delete;
		ScopedCompile& operator=(const ScopedCompile&) = delete;

	private:
		static thread_local u32 s_depth;
		Counter m_time;
		bool m_outermost;
		u64 m_start;
	};

	/// Starts the profiler thread for a VM. Call on the CPU (EE) thread once the VM runs.
	void OnVMStarted();

	/// Stops the profiler thread and writes the final report.
	void OnVMDestroyed();

	/// Called by PerformanceMetrics on the GS thread for every vsync. frame_time_ms is 0 for skipped frames.
	void OnFrame(float frame_time_ms);

	/// Reads the setting: GPU timestamp queries are needed for the GPU time in the summary.
	bool WantsGPUTiming();

	/// The value WantsGPUTiming() last returned (cheap, for every frame).
	bool IsGPUTimingRequested();
} // namespace HorizonProfiler
