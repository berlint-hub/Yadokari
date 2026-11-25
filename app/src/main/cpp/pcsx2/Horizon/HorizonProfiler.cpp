// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Horizon/HorizonProfiler.h"

#include "GS/GSPerfMon.h"
#include "Config.h"
#include "Host.h"
#include "Memory.h"
#include "MTGS.h"
#include "MTVU.h"
#include "PerformanceMetrics.h"
#include "R5900.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Threading.h"
#include "common/Horizon/Horizon.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace HorizonProfiler
{
	std::atomic<u64> g_counters[static_cast<u32>(Counter::Count)] = {};
	thread_local u32 ScopedCompile::s_depth = 0;
} // namespace HorizonProfiler

namespace
{
	using namespace HorizonProfiler;

	constexpr u32 COUNTER_COUNT = static_cast<u32>(Counter::Count);
	constexpr u32 PERF_COUNT = GSPerfMon::CounterLast;

	enum Target : u32
	{
		TARGET_EE,
		TARGET_VU1,
		TARGET_GS,
		NUM_TARGETS
	};
	constexpr const char* TARGET_NAMES[NUM_TARGETS] = {"EE", "VU1", "GS"};

	// Higher than the emulation threads (EE 44) so samples are taken on time; it sleeps between them.
	constexpr s32 PROFILER_THREAD_PRIORITY = 0x28;
	constexpr u32 MAX_STACK_FRAMES = 32;
	constexpr u32 MAX_HITCHES_PER_WINDOW = 8;
	constexpr u32 SELF_TOP = 25;
	constexpr u32 INCL_TOP = 20;
	constexpr u32 CALLER_TOP = 10;
	constexpr u32 GUEST_TOP = 12;
	constexpr u32 LOG_TOP = 6;

	struct Settings
	{
		bool log = true;
		bool sample = true;
		u32 log_interval_s = 5;
		u32 report_interval_s = 30;
		u32 sample_period_us = 4000;
	};

	Settings s_settings;
	std::atomic<bool> s_gpu_timing_requested{false};

	std::thread s_thread;
	std::atomic<bool> s_running{false};
	std::atomic<bool> s_stop{false};
	std::atomic<u32> s_target_handles[NUM_TARGETS] = {};

	u64 Counters(Counter c, const u64* values) { return values[static_cast<u32>(c)]; }

	u64 GetThreadCpuUs(Handle handle)
	{
		if (handle == 0)
			return 0;

		u64 ticks = 0;
		if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, handle, TickCountInfo_Total)) &&
			R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, handle, TickCountInfo_Total)))
		{
			return 0;
		}

		return armTicksToNs(ticks) / 1000;
	}

	//////////////////////////////////////////////////////////////////////////
	// Per-frame accounting (GS thread)
	//////////////////////////////////////////////////////////////////////////

	struct HitchRecord
	{
		u64 frame;
		float frame_ms;
		float avg_ms;
		u64 counters[COUNTER_COUNT];
		double perf[PERF_COUNT];
		u64 cpu_us[NUM_TARGETS];
	};

	struct FrameWindow
	{
		u32 vsyncs = 0;
		u32 presented = 0;
		double frame_ms_sum = 0.0;
		float frame_ms_max = 0.0f;
		double perf[PERF_COUNT] = {};
		std::vector<HitchRecord> hitches;
		u32 hitches_dropped = 0;

		void Clear()
		{
			vsyncs = 0;
			presented = 0;
			frame_ms_sum = 0.0;
			frame_ms_max = 0.0f;
			std::memset(perf, 0, sizeof(perf));
			hitches.clear();
			hitches_dropped = 0;
		}
	};

	std::mutex s_frame_mutex;
	FrameWindow s_frame_window;

	// Only touched by the GS thread.
	struct GSFrameState
	{
		bool valid = false;
		u64 frames = 0;
		float ema_ms = 0.0f;
		u64 counters[COUNTER_COUNT];
		double perf[PERF_COUNT];
		u64 cpu_us[NUM_TARGETS];
	};
	GSFrameState s_gs_state;

	//////////////////////////////////////////////////////////////////////////
	// Symbols and code regions
	//////////////////////////////////////////////////////////////////////////

	struct Symbol
	{
		u32 addr;
		u32 size;
		const char* name;
	};

	struct CodeRegion
	{
		uptr start;
		uptr end;
		const char* name;
	};

	constexpr u32 KEY_UNKNOWN = 0xFFFFFFFFu;
	constexpr u32 KEY_REGION_BASE = 0xFFFFFF00u;
	constexpr u32 KEY_RAW_FLAG = 0x80000000u; // NRO offset >> 2, when there is no symbol table

	std::string s_symbol_data;
	std::vector<Symbol> s_symbols;
	std::vector<CodeRegion> s_regions;
	std::unordered_map<u32, std::string> s_name_cache;
	uptr s_module_base = 0;
	uptr s_module_end = 0;
	u32 s_ee_jit_key = KEY_UNKNOWN;

	bool ParseHex(const char*& p, const char* end, u64* value)
	{
		u64 v = 0;
		const char* const start = p;
		while (p < end)
		{
			const char c = *p;
			u32 digit;
			if (c >= '0' && c <= '9')
				digit = c - '0';
			else if (c >= 'a' && c <= 'f')
				digit = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				digit = c - 'A' + 10;
			else
				break;
			v = (v << 4) | digit;
			p++;
		}
		*value = v;
		return p != start;
	}

	// romfs:/symbols.txt is `nm -S -n --defined-only` of the ELF, function symbols only (see
	// switch/app/cmake/gen_symbols.cmake): "<addr> <size> <type> <mangled name>" per line, sorted, or
	// "<addr> <type> <name>" for assembly without a size (the svc wrappers, which is where waiting threads are).
	void LoadSymbols()
	{
		s_symbols.clear();
		s_symbol_data.clear();

		std::optional<std::string> data = FileSystem::ReadFileToString("romfs:/symbols.txt");
		if (!data.has_value() || data->empty())
		{
			WARNING_LOG("Profiler: romfs:/symbols.txt is missing, functions are reported as NRO offsets");
			return;
		}

		s_symbol_data = std::move(data.value());
		char* p = s_symbol_data.data();
		char* const end = p + s_symbol_data.size();
		s_symbols.reserve(40000);

		while (p < end)
		{
			char* const line_end = static_cast<char*>(std::memchr(p, '\n', end - p));
			char* const le = line_end ? line_end : end;
			const char* q = p;
			u64 addr = 0, size = 0;
			if (ParseHex(q, le, &addr) && q < le && *q == ' ')
			{
				// The type letters (t, T, w, W) are not hex digits, so a missing size is easy to tell apart.
				const char* r = q + 1;
				if (ParseHex(r, le, &size))
					q = r;
				else
					size = 0;

				if ((le - q) > 3 && q[0] == ' ' && q[2] == ' ' && addr <= 0xFFFFFFFFull)
				{
					char* const name = const_cast<char*>(q + 3);
					*le = '\0';
					// Aliases share an address; keep one, preferring a symbol with a size.
					if (!s_symbols.empty() && s_symbols.back().addr == static_cast<u32>(addr))
					{
						if (s_symbols.back().size == 0 && size != 0)
							s_symbols.back() = {static_cast<u32>(addr), static_cast<u32>(size), name};
					}
					else
					{
						s_symbols.push_back({static_cast<u32>(addr), static_cast<u32>(size), name});
					}
				}
			}
			p = le + 1;
		}

		std::sort(s_symbols.begin(), s_symbols.end(), [](const Symbol& a, const Symbol& b) { return a.addr < b.addr; });
		INFO_LOG("Profiler: loaded {} function symbols", s_symbols.size());
	}

	void SetupCodeRegions()
	{
		s_regions.clear();
		s_regions.push_back({reinterpret_cast<uptr>(SysMemory::GetEERec()), reinterpret_cast<uptr>(SysMemory::GetEERecEnd()), "[EE JIT]"});
		s_regions.push_back({reinterpret_cast<uptr>(SysMemory::GetIOPRec()), reinterpret_cast<uptr>(SysMemory::GetIOPRecEnd()), "[IOP JIT]"});
		s_regions.push_back({reinterpret_cast<uptr>(SysMemory::GetVU0Rec()), reinterpret_cast<uptr>(SysMemory::GetVU0RecEnd()), "[VU0 JIT]"});
		s_regions.push_back({reinterpret_cast<uptr>(SysMemory::GetVU1Rec()), reinterpret_cast<uptr>(SysMemory::GetVU1RecEnd()), "[VU1 JIT]"});
		s_regions.push_back({reinterpret_cast<uptr>(SysMemory::GetVIFUnpackRec()), reinterpret_cast<uptr>(SysMemory::GetVIFUnpackRecEnd()), "[VIF unpack JIT]"});
		s_regions.push_back({reinterpret_cast<uptr>(SysMemory::GetCodePtr(0)), reinterpret_cast<uptr>(SysMemory::GetCodePtr(HostMemoryMap::CodeSize)), "[other JIT]"});
		s_ee_jit_key = KEY_REGION_BASE + 0;

		// The NRO's text segment starts at offset 0 of the module, which is where the ELF's addresses start.
		MemoryInfo mi = {};
		u32 page_info = 0;
		if (R_SUCCEEDED(svcQueryMemory(&mi, &page_info, reinterpret_cast<u64>(&GetThreadCpuUs))))
		{
			s_module_base = mi.addr;
			s_module_end = mi.addr + mi.size;
		}
	}

	u32 Resolve(uptr addr)
	{
		for (u32 i = 0; i < static_cast<u32>(s_regions.size()); i++)
		{
			if (addr >= s_regions[i].start && addr < s_regions[i].end)
				return KEY_REGION_BASE + i;
		}

		if (addr >= s_module_base && addr < s_module_end)
		{
			const u32 offset = static_cast<u32>(addr - s_module_base);
			if (s_symbols.empty())
				return KEY_RAW_FLAG | (offset >> 2);

			auto it = std::upper_bound(s_symbols.begin(), s_symbols.end(), offset,
				[](u32 value, const Symbol& sym) { return value < sym.addr; });
			if (it != s_symbols.begin())
				return static_cast<u32>(std::distance(s_symbols.begin(), it) - 1);
		}

		return KEY_UNKNOWN;
	}

	// "void Foo<int>::Bar(int, char const*) const" -> "void Foo<int>::Bar", without GCC clone suffixes.
	std::string SimplifyName(const char* mangled)
	{
		std::string base(mangled);
		const size_t dot = base.find('.');
		if (dot != std::string::npos && dot > 0)
			base.resize(dot);

		std::string name;
		int status = -1;
		char* const demangled = abi::__cxa_demangle(base.c_str(), nullptr, nullptr, &status);
		if (status == 0 && demangled)
			name = demangled;
		else
			name = std::move(base);
		std::free(demangled);

		// Drop the parameter list of the function itself (the last top-level parentheses).
		std::string_view view(name);
		for (const std::string_view qualifier : {std::string_view(" const"), std::string_view(" &"), std::string_view(" &&")})
		{
			if (view.size() > qualifier.size() && view.substr(view.size() - qualifier.size()) == qualifier)
				view.remove_suffix(qualifier.size());
		}
		if (!view.empty() && view.back() == ')')
		{
			int depth = 0;
			for (size_t i = view.size(); i-- > 0;)
			{
				if (view[i] == ')')
					depth++;
				else if (view[i] == '(' && --depth == 0)
				{
					if (i > 0)
						name.resize(i);
					break;
				}
			}
		}

		constexpr size_t MAX_NAME = 90;
		if (name.size() > MAX_NAME)
		{
			name.resize(MAX_NAME - 3);
			name += "...";
		}
		return name;
	}

	// Samples in libnx's svc wrappers: the thread was waiting (or sleeping) in the kernel.
	bool IsKernelWait(u32 key)
	{
		return key < s_symbols.size() && std::strncmp(s_symbols[key].name, "svc", 3) == 0;
	}

	const std::string& KeyName(u32 key)
	{
		auto it = s_name_cache.find(key);
		if (it != s_name_cache.end())
			return it->second;

		std::string name;
		if (key == KEY_UNKNOWN)
			name = "[outside the NRO]";
		else if (key >= KEY_REGION_BASE)
			name = s_regions[key - KEY_REGION_BASE].name;
		else if (key & KEY_RAW_FLAG)
			name = fmt::format("nro+0x{:x}", (key & ~KEY_RAW_FLAG) << 2);
		else
			name = SimplifyName(s_symbols[key].name);

		return s_name_cache.emplace(key, std::move(name)).first->second;
	}

	//////////////////////////////////////////////////////////////////////////
	// Sampling
	//////////////////////////////////////////////////////////////////////////

	struct RawSample
	{
		u32 count;
		u32 guest_pc;
		u64 lr;
		u64 frames[MAX_STACK_FRAMES];
	};

	struct StackRange
	{
		uptr lo = 0;
		uptr hi = 0;
	};

	struct TargetProfile
	{
		u32 samples = 0;
		u32 failures = 0;
		std::unordered_map<u32, u32> self;
		std::unordered_map<u32, u32> incl;
		std::unordered_map<u64, u32> callers; // (self << 32) | caller
		std::unordered_map<u32, u32> guest_pcs;

		void Clear()
		{
			samples = 0;
			failures = 0;
			self.clear();
			incl.clear();
			callers.clear();
			guest_pcs.clear();
		}

		void Merge(const TargetProfile& other)
		{
			samples += other.samples;
			failures += other.failures;
			for (const auto& [k, v] : other.self)
				self[k] += v;
			for (const auto& [k, v] : other.incl)
				incl[k] += v;
			for (const auto& [k, v] : other.callers)
				callers[k] += v;
			for (const auto& [k, v] : other.guest_pcs)
				guest_pcs[k] += v;
		}
	};

	StackRange s_stacks[NUM_TARGETS];
	TargetProfile s_window_profile[NUM_TARGETS];
	TargetProfile s_session_profile[NUM_TARGETS];

	// While the target is paused, nothing here may take a lock or allocate: the target could hold the lock.
	bool CaptureSample(u32 target, RawSample* out)
	{
		const Handle handle = s_target_handles[target].load(std::memory_order_relaxed);
		if (handle == 0)
			return false;

		if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused)))
			return false;

		ThreadContext ctx;
		const bool ok = R_SUCCEEDED(svcGetThreadContext3(&ctx, handle));
		if (ok)
		{
			out->count = 0;
			out->frames[out->count++] = ctx.pc.x;
			out->lr = ctx.lr;

			// Walk the frame records (x29 chain). JIT code uses x29 for state, which fails the range check.
			StackRange& range = s_stacks[target];
			const uptr sp = ctx.sp;
			if (sp < range.lo || sp >= range.hi)
			{
				MemoryInfo mi = {};
				u32 page_info = 0;
				if (R_SUCCEEDED(svcQueryMemory(&mi, &page_info, sp)) && (mi.perm & Perm_R))
				{
					range.lo = mi.addr;
					range.hi = mi.addr + mi.size;
				}
				else
				{
					range.lo = range.hi = 0;
				}
			}

			uptr fp = ctx.fp;
			while (out->count < MAX_STACK_FRAMES && fp >= sp && fp >= range.lo && (fp + 16) <= range.hi && (fp & 7) == 0)
			{
				const u64* const record = reinterpret_cast<const u64*>(fp);
				const u64 next = record[0];
				const u64 ret = record[1];
				if (ret == 0)
					break;
				out->frames[out->count++] = ret;
				if (next <= fp)
					break;
				fp = next;
			}

			out->guest_pc = (target == TARGET_EE) ? cpuRegs.pc : 0;
		}

		// Always let it run again.
		for (u32 attempt = 0; attempt < 1000; attempt++)
		{
			if (R_SUCCEEDED(svcSetThreadActivity(handle, ThreadActivity_Runnable)))
				break;
			svcSleepThread(10000);
		}

		return ok;
	}

	void AggregateSample(u32 target, const RawSample& sample)
	{
		TargetProfile& prof = s_window_profile[target];
		prof.samples++;

		u32 keys[MAX_STACK_FRAMES + 1];
		u32 key_count = 0;
		keys[key_count++] = Resolve(sample.frames[0]);

		// The link register is the caller in leaf functions and prologues, where the frame chain skips it.
		const u32 first_return = (sample.count > 1) ? Resolve(sample.frames[1] - 1) : KEY_UNKNOWN;
		if (sample.lr != 0)
		{
			const u32 lr_key = Resolve(sample.lr - 1);
			if (lr_key != KEY_UNKNOWN && lr_key != keys[0] && lr_key != first_return)
				keys[key_count++] = lr_key;
		}
		for (u32 i = 1; i < sample.count; i++)
			keys[key_count++] = (i == 1) ? first_return : Resolve(sample.frames[i] - 1);

		prof.self[keys[0]]++;

		for (u32 i = 0; i < key_count; i++)
		{
			bool seen = false;
			for (u32 j = 0; j < i && !seen; j++)
				seen = (keys[j] == keys[i]);
			if (!seen)
				prof.incl[keys[i]]++;
		}

		u32 caller = KEY_UNKNOWN;
		for (u32 i = 1; i < key_count; i++)
		{
			if (keys[i] != keys[0])
			{
				caller = keys[i];
				break;
			}
		}
		if (caller != KEY_UNKNOWN)
			prof.callers[(static_cast<u64>(keys[0]) << 32) | caller]++;

		if (target == TARGET_EE && sample.guest_pc != 0 && (keys[0] == s_ee_jit_key || caller == s_ee_jit_key))
			prof.guest_pcs[sample.guest_pc]++;
	}

	template <typename K>
	std::vector<std::pair<K, u32>> TopEntries(const std::unordered_map<K, u32>& map, size_t count)
	{
		std::vector<std::pair<K, u32>> entries(map.begin(), map.end());
		const size_t n = std::min(count, entries.size());
		std::partial_sort(entries.begin(), entries.begin() + n, entries.end(),
			[](const auto& a, const auto& b) { return a.second > b.second; });
		entries.resize(n);
		return entries;
	}

	double KernelWaitPercent(const TargetProfile& prof)
	{
		u32 waiting = 0;
		for (const auto& [key, count] : prof.self)
		{
			if (IsKernelWait(key))
				waiting += count;
		}
		return prof.samples ? (100.0 * waiting / prof.samples) : 0.0;
	}

	void FormatProfile(std::string& out, const char* title, const TargetProfile* profiles, double seconds)
	{
		out += fmt::format("==================== {} ({:.0f} s) ====================\n", title, seconds);
		for (u32 t = 0; t < NUM_TARGETS; t++)
		{
			const TargetProfile& prof = profiles[t];
			if (prof.samples == 0)
				continue;

			const double total = static_cast<double>(prof.samples);
			out += fmt::format("\n--- {} thread: {} samples ({:.0f}/s){} ---\n", TARGET_NAMES[t], prof.samples,
				seconds > 0.0 ? total / seconds : 0.0,
				prof.failures ? fmt::format(", {} failed", prof.failures) : std::string());
			const double waiting = KernelWaitPercent(prof);
			out += fmt::format("  waiting in the kernel (svc*): {:.1f}%, busy {:.1f}%\n", waiting, 100.0 - waiting);

			out += "  self%  incl%  function (self = running in it, incl = it or something it called)\n";
			for (const auto& [key, count] : TopEntries(prof.self, SELF_TOP))
			{
				const auto incl = prof.incl.find(key);
				out += fmt::format("  {:5.1f}  {:5.1f}  {}\n", 100.0 * count / total,
					incl != prof.incl.end() ? 100.0 * incl->second / total : 0.0, KeyName(key));
			}

			out += "\n  top inclusive:\n";
			for (const auto& [key, count] : TopEntries(prof.incl, INCL_TOP))
				out += fmt::format("  {:5.1f}  {}\n", 100.0 * count / total, KeyName(key));

			// For the hottest functions, who called them.
			std::unordered_map<u32, std::vector<std::pair<u32, u32>>> callers_by_self;
			for (const auto& [pair, count] : prof.callers)
				callers_by_self[static_cast<u32>(pair >> 32)].emplace_back(static_cast<u32>(pair), count);

			std::string caller_lines;
			for (const auto& [key, count] : TopEntries(prof.self, CALLER_TOP))
			{
				auto it = callers_by_self.find(key);
				if (it == callers_by_self.end())
					continue;
				auto& list = it->second;
				std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
				const size_t shown = std::min<size_t>(3, list.size());
				caller_lines += fmt::format("  {} <-", KeyName(key));
				for (size_t i = 0; i < shown; i++)
					caller_lines += fmt::format(" {} {:.0f}%{}", KeyName(list[i].first), 100.0 * list[i].second / count, (i + 1 < shown) ? "," : "");
				caller_lines += '\n';
			}
			if (!caller_lines.empty())
			{
				out += "\n  callers of the top functions:\n";
				out += caller_lines;
			}

			if (!prof.guest_pcs.empty())
			{
				u32 guest_total = 0;
				for (const auto& [pc, count] : prof.guest_pcs)
					guest_total += count;
				out += fmt::format("\n  EE guest code (PC of the running block, {} samples in EE JIT or its helpers):\n", guest_total);
				for (const auto& [pc, count] : TopEntries(prof.guest_pcs, GUEST_TOP))
					out += fmt::format("  {:5.1f}  0x{:08x}\n", 100.0 * count / total, pc);
			}
		}
		out += '\n';
	}

	void LogProfileSummary(const TargetProfile* profiles)
	{
		for (u32 t = 0; t < NUM_TARGETS; t++)
		{
			const TargetProfile& prof = profiles[t];
			if (prof.samples == 0)
				continue;

			std::string line = fmt::format("[Prof] {} (waiting {:.0f}%):", TARGET_NAMES[t], KernelWaitPercent(prof));
			bool first = true;
			for (const auto& [key, count] : TopEntries(prof.self, LOG_TOP))
			{
				const double pct = 100.0 * count / prof.samples;
				if (pct < 1.0)
					break;
				std::string name = KeyName(key);
				if (name.size() > 48)
				{
					name.resize(45);
					name += "...";
				}
				line += fmt::format("{} {:.0f}% {}", first ? "" : ",", pct, name);
				first = false;
			}
			INFO_LOG("{}", line);
		}
	}

	//////////////////////////////////////////////////////////////////////////
	// Periodic summary
	//////////////////////////////////////////////////////////////////////////

	struct Snapshot
	{
		u64 time_ns = 0;
		u64 paused_ns = 0;
		u64 counters[COUNTER_COUNT] = {};
		u64 cpu_us[NUM_TARGETS] = {};
	};

	void TakeSnapshot(Snapshot* snap, u64 paused_ns)
	{
		snap->time_ns = GetTimeNs();
		snap->paused_ns = paused_ns;
		for (u32 i = 0; i < COUNTER_COUNT; i++)
			snap->counters[i] = g_counters[i].load(std::memory_order_relaxed);
		for (u32 t = 0; t < NUM_TARGETS; t++)
			snap->cpu_us[t] = GetThreadCpuUs(s_target_handles[t].load(std::memory_order_relaxed));
	}

	double NsToMs(u64 ns) { return static_cast<double>(ns) / 1.0e6; }

	std::string FormatHitch(const HitchRecord& h)
	{
		const u64* c = h.counters;
		std::string line = fmt::format("[Hitch] frame {}: {:.1f} ms (avg {:.1f}) | CPU EE {:.1f} VU1 {:.1f} GS {:.1f} ms",
			h.frame, h.frame_ms, h.avg_ms, h.cpu_us[TARGET_EE] / 1000.0, h.cpu_us[TARGET_VU1] / 1000.0,
			h.cpu_us[TARGET_GS] / 1000.0);

		auto add_compile = [&](const char* name, Counter blocks, Counter ns) {
			if (Counters(blocks, c) != 0)
				line += fmt::format(" | {} compile {} blk {:.1f} ms", name, Counters(blocks, c), NsToMs(Counters(ns, c)));
		};
		add_compile("EE", Counter::EEBlocks, Counter::EECompileNs);
		add_compile("VU0", Counter::VU0Blocks, Counter::VU0CompileNs);
		add_compile("VU1", Counter::VU1Blocks, Counter::VU1CompileNs);
		if (Counters(Counter::EEResets, c) != 0)
			line += fmt::format(" | EE rec reset x{}", Counters(Counter::EEResets, c));
		if (Counters(Counter::VU0CacheResets, c) + Counters(Counter::VU1CacheResets, c) != 0)
			line += " | mVU cache reset";
		if (Counters(Counter::VKPipelines, c) != 0)
			line += fmt::format(" | VK {} pipelines {:.1f} ms", Counters(Counter::VKPipelines, c), NsToMs(Counters(Counter::VKPipelineNs, c)));
		line += fmt::format(" | EE wait VU {:.1f} GS {:.1f} vsync {:.1f}", NsToMs(Counters(Counter::EEWaitVUNs, c)),
			NsToMs(Counters(Counter::EEWaitGSNs, c)), NsToMs(Counters(Counter::EEWaitVsyncNs, c)));
		line += fmt::format(" | GS wait idle {:.1f} swap {:.1f} gpu {:.1f}", NsToMs(Counters(Counter::GSWaitWorkNs, c)),
			NsToMs(Counters(Counter::GSWaitSwapNs, c)), NsToMs(Counters(Counter::GSWaitGPUNs, c)));
		line += fmt::format(" | {:.0f} draws {:.0f} readbacks {} syncs {:.0f} uploads", h.perf[GSPerfMon::Draw],
			h.perf[GSPerfMon::Readbacks], Counters(Counter::GPUSyncs, c), h.perf[GSPerfMon::TextureUploads]);
		return line;
	}

	void LogSummary(const Snapshot& a, const Snapshot& b, const FrameWindow& fw, u64 vm_start_ns)
	{
		const double wall_s = static_cast<double>(b.time_ns - a.time_ns) / 1.0e9;
		const double paused_s = static_cast<double>(b.paused_ns - a.paused_ns) / 1.0e9;
		const double run_s = std::max(wall_s - paused_s, 0.001);
		if (fw.vsyncs == 0 || wall_s <= 0.0)
			return;

		u64 d[COUNTER_COUNT];
		for (u32 i = 0; i < COUNTER_COUNT; i++)
			d[i] = b.counters[i] - a.counters[i];

		const double vsyncs = static_cast<double>(fw.vsyncs);
		const float frame_rate = VMManager::GetFrameRate();
		const double speed = (frame_rate > 0.0f) ? (100.0 * vsyncs / (run_s * frame_rate)) : 0.0;
		const double fps = static_cast<double>(fw.presented) / run_s;
		const double frame_ms = (run_s * 1000.0) / vsyncs;
		const double avg_present_ms = fw.presented ? fw.frame_ms_sum / fw.presented : 0.0;

		double cpu_pct[NUM_TARGETS], cpu_ms[NUM_TARGETS];
		for (u32 t = 0; t < NUM_TARGETS; t++)
		{
			const double us = static_cast<double>(b.cpu_us[t] - a.cpu_us[t]);
			cpu_pct[t] = 100.0 * us / (wall_s * 1.0e6);
			cpu_ms[t] = us / 1000.0 / vsyncs;
		}

		auto per_frame_ms = [&](Counter c) { return NsToMs(Counters(c, d)) / vsyncs; };
		const double ee_wait_vu = per_frame_ms(Counter::EEWaitVUNs);
		const double ee_wait_gs = per_frame_ms(Counter::EEWaitGSNs);
		const double ee_wait_vsync = per_frame_ms(Counter::EEWaitVsyncNs);
		const double gs_wait_work = per_frame_ms(Counter::GSWaitWorkNs);
		const double gs_wait_swap = per_frame_ms(Counter::GSWaitSwapNs);
		const double gs_wait_gpu = per_frame_ms(Counter::GSWaitGPUNs);
		const double gpu_ms = Counters(Counter::GPUTimedFrames, d) ?
		                          static_cast<double>(Counters(Counter::GPUTimeUs, d)) / 1000.0 / Counters(Counter::GPUTimedFrames, d) :
		                          -1.0;

		// Same idea as PerformanceMetrics: the bottleneck is the stage nobody waits for.
		const char* limiter;
		const double ee_waits = ee_wait_vu + ee_wait_gs + ee_wait_vsync;
		const double gs_busy = std::max(0.0, frame_ms - gs_wait_work - gs_wait_swap - gs_wait_gpu);
		if (speed >= 97.0)
			limiter = "full speed";
		else if (ee_waits < 0.2 * frame_ms)
			limiter = "EE";
		else if ((ee_wait_gs + ee_wait_vsync) >= ee_wait_vu)
			limiter = ((gs_wait_gpu + gs_wait_swap) >= gs_busy) ? "GPU" : "GS thread";
		else
			limiter = "VU1";

		const double t_start = static_cast<double>(a.time_ns - vm_start_ns) / 1.0e9;
		const double t_end = static_cast<double>(b.time_ns - vm_start_ns) / 1.0e9;
		INFO_LOG("[Perf] {:.0f}-{:.0f}s{}: speed {:.0f}% | {:.1f} fps (game {:.0f}) | frame avg {:.1f} max {:.1f} ms | limiter {}",
			t_start, t_end, paused_s > 0.05 ? fmt::format(" (paused {:.1f}s)", paused_s) : std::string(), speed, fps,
			PerformanceMetrics::GetInternalFPS(), avg_present_ms, fw.frame_ms_max, limiter);

		std::string cpu = fmt::format("[Perf]   EE {:.0f}% {:.1f}ms/f (wait VU {:.1f} GS {:.1f} vsync {:.1f})", cpu_pct[TARGET_EE],
			cpu_ms[TARGET_EE], ee_wait_vu, ee_wait_gs, ee_wait_vsync);
		if (s_target_handles[TARGET_VU1].load(std::memory_order_relaxed) != 0)
			cpu += fmt::format(" | VU1 {:.0f}% {:.1f}ms/f", cpu_pct[TARGET_VU1], cpu_ms[TARGET_VU1]);
		else
			cpu += " | VU1 on EE thread";
		cpu += fmt::format(" | GS {:.0f}% {:.1f}ms/f (wait idle {:.1f} swap {:.1f} gpu {:.1f})", cpu_pct[TARGET_GS],
			cpu_ms[TARGET_GS], gs_wait_work, gs_wait_swap, gs_wait_gpu);
		if (gpu_ms >= 0.0)
			cpu += fmt::format(" | GPU {:.1f}ms/f", gpu_ms);
		INFO_LOG("{}", cpu);

		const double* perf = fw.perf;
		INFO_LOG("[Perf]   GS per vsync: {:.0f} draws {:.0f} prims {:.0f} draw calls {:.1f} passes {:.1f} barriers {:.1f} uploads "
				 "{:.1f} copies {:.2f} readbacks {:.2f} GPU syncs",
			perf[GSPerfMon::Draw] / vsyncs, perf[GSPerfMon::Prim] / vsyncs, perf[GSPerfMon::DrawCalls] / vsyncs,
			perf[GSPerfMon::RenderPasses] / vsyncs, perf[GSPerfMon::Barriers] / vsyncs,
			perf[GSPerfMon::TextureUploads] / vsyncs, perf[GSPerfMon::TextureCopies] / vsyncs,
			perf[GSPerfMon::Readbacks] / vsyncs, static_cast<double>(Counters(Counter::GPUSyncs, d)) / vsyncs);

		INFO_LOG("[Perf]   JIT: EE {} blk {:.1f}ms, {} clears ({} hit) {} discards {} resets | VU0 {} blk {:.1f}ms {} progs | "
				 "VU1 {} blk {:.1f}ms {} progs {} resets | VK {} pipelines {:.1f}ms",
			Counters(Counter::EEBlocks, d), NsToMs(Counters(Counter::EECompileNs, d)), Counters(Counter::EEClears, d),
			Counters(Counter::EEClearHits, d), Counters(Counter::EEDiscards, d), Counters(Counter::EEResets, d),
			Counters(Counter::VU0Blocks, d), NsToMs(Counters(Counter::VU0CompileNs, d)), Counters(Counter::VU0Programs, d),
			Counters(Counter::VU1Blocks, d), NsToMs(Counters(Counter::VU1CompileNs, d)), Counters(Counter::VU1Programs, d),
			Counters(Counter::VU0CacheResets, d) + Counters(Counter::VU1CacheResets, d), Counters(Counter::VKPipelines, d),
			NsToMs(Counters(Counter::VKPipelineNs, d)));

		for (const HitchRecord& h : fw.hitches)
			INFO_LOG("{}", FormatHitch(h));
		if (fw.hitches_dropped > 0)
			INFO_LOG("[Hitch] ... and {} more long frames", fw.hitches_dropped);
	}

	//////////////////////////////////////////////////////////////////////////
	// Thread
	//////////////////////////////////////////////////////////////////////////

	void WriteReport(std::FILE* fp, const char* title, const TargetProfile* profiles, double seconds)
	{
		if (!fp)
			return;
		std::string text;
		FormatProfile(text, title, profiles, seconds);
		std::fwrite(text.data(), 1, text.size(), fp);
		std::fflush(fp);
	}

	void ThreadMain()
	{
		svcSetThreadPriority(CUR_THREAD_HANDLE, PROFILER_THREAD_PRIORITY);

		const Settings settings = s_settings;
		const u64 vm_start_ns = GetTimeNs();

		std::FILE* report_fp = nullptr;
		if (settings.sample)
		{
			LoadSymbols();
			SetupCodeRegions();
			s_name_cache.clear();
			for (u32 t = 0; t < NUM_TARGETS; t++)
			{
				s_window_profile[t].Clear();
				s_session_profile[t].Clear();
				s_stacks[t] = {};
			}

			const std::string path = Path::Combine(EmuFolders::Logs, "profile.txt");
			report_fp = FileSystem::OpenCFile(path.c_str(), "wb");
			if (report_fp)
			{
				const std::string header = fmt::format("ARMSX2 sampling profile: {} [{}]\nSampled every {} us: EE, VU1 and GS threads.\n"
													   "JIT code is shown as [EE JIT], [VU1 JIT], ... Symbols: {}\n\n",
					VMManager::GetTitle(false), VMManager::GetDiscSerial(), settings.sample_period_us,
					s_symbols.empty() ? "none (NRO offsets)" : fmt::format("{} functions", s_symbols.size()));
				std::fwrite(header.data(), 1, header.size(), report_fp);
				std::fflush(report_fp);
			}
			else
			{
				WARNING_LOG("Profiler: could not open {}", path);
			}
		}

		INFO_LOG("Profiler: started (summary every {} s{}). Turn off with [Horizon] PerformanceLog / Profiler = false",
			settings.log_interval_s,
			settings.sample ? fmt::format(", sampling every {} us, report to logs/profile.txt every {} s", settings.sample_period_us,
								  settings.report_interval_s) :
							  std::string());

		u64 paused_ns = 0;
		Snapshot window_start;
		TakeSnapshot(&window_start, paused_ns);
		u64 last_iteration = window_start.time_ns;
		u64 next_log = window_start.time_ns + settings.log_interval_s * 1000000000ull;
		u64 report_start = window_start.time_ns;
		u64 next_report = window_start.time_ns + settings.report_interval_s * 1000000000ull;
		FrameWindow fw;
		fw.hitches.reserve(MAX_HITCHES_PER_WINDOW);
		RawSample sample;

		auto flush_summary = [&](u64 now) {
			{
				std::lock_guard lock(s_frame_mutex);
				std::swap(fw.vsyncs, s_frame_window.vsyncs);
				std::swap(fw.presented, s_frame_window.presented);
				std::swap(fw.frame_ms_sum, s_frame_window.frame_ms_sum);
				std::swap(fw.frame_ms_max, s_frame_window.frame_ms_max);
				std::swap(fw.perf, s_frame_window.perf);
				fw.hitches.swap(s_frame_window.hitches);
				std::swap(fw.hitches_dropped, s_frame_window.hitches_dropped);
				s_frame_window.Clear();
			}
			Snapshot window_end;
			TakeSnapshot(&window_end, paused_ns);
			if (settings.log)
				LogSummary(window_start, window_end, fw, vm_start_ns);
			fw.Clear();
			window_start = window_end;
		};

		auto flush_report = [&](u64 now, bool final) {
			const double seconds = static_cast<double>(now - report_start) / 1.0e9;
			const double t_start = static_cast<double>(report_start - vm_start_ns) / 1.0e9;
			const double t_end = static_cast<double>(now - vm_start_ns) / 1.0e9;
			const std::string title = fmt::format("{:.0f}-{:.0f}s", t_start, t_end);
			u32 samples = 0;
			for (u32 t = 0; t < NUM_TARGETS; t++)
				samples += s_window_profile[t].samples;
			if (samples > 0)
			{
				WriteReport(report_fp, title.c_str(), s_window_profile, seconds);
				LogProfileSummary(s_window_profile);
				for (u32 t = 0; t < NUM_TARGETS; t++)
				{
					s_session_profile[t].Merge(s_window_profile[t]);
					s_window_profile[t].Clear();
				}
			}
			if (final)
				WriteReport(report_fp, "Whole session", s_session_profile, static_cast<double>(now - vm_start_ns) / 1.0e9);
			report_start = now;
		};

		while (!s_stop.load(std::memory_order_acquire))
		{
			const VMState state = VMManager::GetState();
			if (settings.sample && state == VMState::Running)
			{
				for (u32 t = 0; t < NUM_TARGETS; t++)
				{
					if (CaptureSample(t, &sample))
						AggregateSample(t, sample);
					else if (s_target_handles[t].load(std::memory_order_relaxed) != 0)
						s_window_profile[t].failures++;
				}
			}

			svcSleepThread(static_cast<s64>(settings.sample ? settings.sample_period_us : 100000u) * 1000);

			const u64 now = GetTimeNs();
			if (state == VMState::Paused)
				paused_ns += now - last_iteration;
			last_iteration = now;

			if (now >= next_log)
			{
				flush_summary(now);
				next_log = now + settings.log_interval_s * 1000000000ull;
			}

			if (settings.sample && now >= next_report)
			{
				flush_report(now, false);
				next_report = now + settings.report_interval_s * 1000000000ull;
			}
		}

		const u64 now = GetTimeNs();
		if (now - window_start.time_ns > 1000000000ull)
			flush_summary(now);
		if (settings.sample)
		{
			flush_report(now, true);
			if (report_fp)
				std::fclose(report_fp);
			s_symbols.clear();
			s_symbol_data = std::string();
			s_name_cache.clear();
			for (u32 t = 0; t < NUM_TARGETS; t++)
			{
				s_window_profile[t].Clear();
				s_session_profile[t].Clear();
			}
		}
		INFO_LOG("Profiler: stopped");
	}

	void LoadSettings()
	{
		Settings s;
		s.log = Host::GetBaseBoolSettingValue("Horizon", "PerformanceLog", true);
		s.sample = Host::GetBaseBoolSettingValue("Horizon", "Profiler", true);
		s.log_interval_s = static_cast<u32>(std::clamp(Host::GetBaseIntSettingValue("Horizon", "PerformanceLogInterval", 5), 1, 600));
		s.report_interval_s = static_cast<u32>(std::clamp(Host::GetBaseIntSettingValue("Horizon", "ProfilerReportInterval", 30), 5, 3600));
		s.sample_period_us = static_cast<u32>(std::clamp(Host::GetBaseIntSettingValue("Horizon", "ProfilerPeriodUs", 4000), 1000, 100000));
		s_settings = s;
	}

	void StopThread()
	{
		s_running.store(false, std::memory_order_release);
		if (s_thread.joinable())
		{
			s_stop.store(true, std::memory_order_release);
			s_thread.join();
		}
		for (u32 t = 0; t < NUM_TARGETS; t++)
			s_target_handles[t].store(0, std::memory_order_relaxed);
	}
} // namespace

u64 HorizonProfiler::GetTimeNs()
{
	return armTicksToNs(armGetSystemTick());
}

bool HorizonProfiler::WantsGPUTiming()
{
	const bool wanted = Host::GetBaseBoolSettingValue("Horizon", "PerformanceLog", true);
	s_gpu_timing_requested.store(wanted, std::memory_order_relaxed);
	return wanted;
}

bool HorizonProfiler::IsGPUTimingRequested()
{
	return s_gpu_timing_requested.load(std::memory_order_relaxed);
}

void HorizonProfiler::OnVMStarted()
{
	StopThread();

	LoadSettings();
	if (!s_settings.log && !s_settings.sample)
		return;

	// Host::OnVMStarted() runs on the CPU thread, which is the EE thread.
	s_target_handles[TARGET_EE].store(threadGetCurHandle(), std::memory_order_relaxed);
	s_target_handles[TARGET_VU1].store(THREAD_VU1 ? Threading::GetHorizonKernelHandle(vu1Thread.GetThreadHandle()) : 0,
		std::memory_order_relaxed);
	s_target_handles[TARGET_GS].store(Threading::GetHorizonKernelHandle(MTGS::GetThreadHandle()), std::memory_order_relaxed);

	{
		std::lock_guard lock(s_frame_mutex);
		s_frame_window.Clear();
		s_frame_window.hitches.reserve(MAX_HITCHES_PER_WINDOW);
	}

	s_stop.store(false, std::memory_order_release);
	s_running.store(true, std::memory_order_release);
	s_thread = std::thread(&ThreadMain);
}

void HorizonProfiler::OnVMDestroyed()
{
	StopThread();
}

void HorizonProfiler::OnFrame(float frame_time_ms)
{
	GSFrameState& st = s_gs_state;
	if (!s_running.load(std::memory_order_acquire))
	{
		st.valid = false;
		return;
	}

	u64 counters[COUNTER_COUNT];
	for (u32 i = 0; i < COUNTER_COUNT; i++)
		counters[i] = g_counters[i].load(std::memory_order_relaxed);
	double perf[PERF_COUNT];
	for (u32 i = 0; i < PERF_COUNT; i++)
		perf[i] = g_perfmon.GetTotal(static_cast<GSPerfMon::counter_t>(i));
	u64 cpu_us[NUM_TARGETS];
	for (u32 t = 0; t < NUM_TARGETS; t++)
		cpu_us[t] = GetThreadCpuUs(s_target_handles[t].load(std::memory_order_relaxed));

	if (!st.valid)
	{
		st.valid = true;
		st.frames = 0;
		st.ema_ms = 0.0f;
		std::memcpy(st.counters, counters, sizeof(counters));
		std::memcpy(st.perf, perf, sizeof(perf));
		std::memcpy(st.cpu_us, cpu_us, sizeof(cpu_us));
		return;
	}

	// GSPerfMon::Reset() on a GS reset starts the totals again.
	double perf_delta[PERF_COUNT];
	for (u32 i = 0; i < PERF_COUNT; i++)
		perf_delta[i] = (perf[i] >= st.perf[i]) ? (perf[i] - st.perf[i]) : perf[i];

	st.frames++;
	bool hitch = false;
	const float avg_ms = st.ema_ms;
	if (frame_time_ms > 0.0f)
	{
		const float frame_rate = VMManager::GetFrameRate();
		const float target_ms = (frame_rate > 0.0f) ? (1000.0f / frame_rate) : (1000.0f / 60.0f);
		if (st.ema_ms <= 0.0f)
			st.ema_ms = frame_time_ms;
		else if (st.frames > 30 && frame_time_ms > std::max(target_ms * 3.0f, st.ema_ms * 2.0f))
			hitch = true;

		if (!hitch)
			st.ema_ms += (frame_time_ms - st.ema_ms) * 0.1f;
	}

	{
		std::lock_guard lock(s_frame_mutex);
		FrameWindow& w = s_frame_window;
		w.vsyncs++;
		if (frame_time_ms > 0.0f)
		{
			w.presented++;
			w.frame_ms_sum += frame_time_ms;
			w.frame_ms_max = std::max(w.frame_ms_max, frame_time_ms);
		}
		for (u32 i = 0; i < PERF_COUNT; i++)
			w.perf[i] += perf_delta[i];

		if (hitch)
		{
			if (w.hitches.size() < MAX_HITCHES_PER_WINDOW)
			{
				HitchRecord& h = w.hitches.emplace_back();
				h.frame = PerformanceMetrics::GetFrameNumber();
				h.frame_ms = frame_time_ms;
				h.avg_ms = avg_ms;
				for (u32 i = 0; i < COUNTER_COUNT; i++)
					h.counters[i] = counters[i] - st.counters[i];
				std::memcpy(h.perf, perf_delta, sizeof(perf_delta));
				for (u32 t = 0; t < NUM_TARGETS; t++)
					h.cpu_us[t] = cpu_us[t] - st.cpu_us[t];
			}
			else
			{
				w.hitches_dropped++;
			}
		}
	}

	std::memcpy(st.counters, counters, sizeof(counters));
	std::memcpy(st.perf, perf, sizeof(perf));
	std::memcpy(st.cpu_us, cpu_us, sizeof(cpu_us));
}
