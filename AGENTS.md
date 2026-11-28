# Notes for AI agents (and new contributors)

This is the working knowledge behind the Switch port: how the code is laid out, how to build and test without a
Switch, how to read the logs the owner sends from the console, the traps that already cost time, and where the
Gran Turismo 4 performance work stands. The user-facing documentation is [README.md](README.md); read it too,
it documents the log formats and settings referred to here.

The owner writes in Czech; answer in Czech. They test on their own console (a Switch OLED / Mariko, sometimes
overclocked to 2703 MHz with a CPU governor that can lower the clock mid-game; the `[Perf]` lines show the clock).
Nothing here can run on a Switch: every change is verified on the host as far as possible (below), built by CI,
and then tested by the owner, who sends `emulog.txt`, `profile.txt` and, after a crash, `crash.txt`.

## What this is

ARMSX2-NX: a PlayStation 2 emulator for the Nintendo Switch (Horizon OS, homebrew `.nro`). Its lineage:

- **PCSX2**, at **v2.3.431** (nightly of 2025-06-21, between v2.3 and v2.4). Identified by matching the git blob
  hashes of `pcsx2/` and `common/` against upstream commits; savestate version 0x9A54, shader cache version 67.
  There is no upstream git history in this repository.
- **ARMSX2** (Android): ARM64 recompilers for the EE, IOP, VU and VIF, written with vixl as a line-by-line
  translation of PCSX2's x86 recompilers. The x86 original of each line is kept as a comment above it, which
  makes porting upstream x86 fixes mechanical (see "Porting from upstream").
- **ARMSX2-NX** (PalindromicBreadLoaf): the Horizon port (libnx, NXVK Vulkan, Switch frontend).
- This repository: Vulkan-only Switch build, performance work on the recompilers and GS, diagnostics
  (performance log, sampling profiler, crash reports), self-updater, input shortcuts, cheat downloader.

The Android app (Gradle, React Native UI, `app/src/main/java`) is inherited and not built or maintained here.

## Repository map

| Path | What |
|---|---|
| `app/src/main/cpp/pcsx2/` | Emulator core (PCSX2 + ARMSX2). |
| `app/src/main/cpp/pcsx2/x86/` | Despite the name: the **ARM64** microVU (`microVU*.inl`) and EE recompiler (`ix86-32/iR5900*.cpp`), x86 originals in comments. |
| `app/src/main/cpp/pcsx2/arm64/` | ARM64 helpers: `VixlHelpers.*` (register names, `PTR_CPU`/`PTR_MVU`, memory helpers), `AsmHelpers.*`, VIF unpack dynarec, `ClampTracker.h`, `MMIEmitters.h`. |
| `app/src/main/cpp/pcsx2/Horizon/` | Switch-only core code: `HorizonProfiler` (perf log, sampling profiler, crash report), `HorizonSystemInfo` (CPU name/clock). |
| `app/src/main/cpp/pcsx2/GS/Renderers/Vulkan/` | Vulkan renderer (`GSDeviceVK`, `VKSwapChain`, pipeline prewarm). |
| `app/src/main/cpp/common/Horizon/` | libnx wrapper (`Horizon.h`), threads, memory. |
| `switch/app/` | Switch frontend: `main.cpp` (startup/exit, input), `HorizonHost`, `HorizonUpdater`, `HorizonException` (crash handler), `HorizonCores`, `HorizonHotkeys.h`, host tests in `tests/`. |
| `switch/tools/` | Host-side tools: `jit-tests/` (qemu JIT tests), `syntax-check.sh`, `logs/` (log and profile analysis). |
| `.github/workflows/switch_build.yml` | CI: NXVK portlib, `.nro` build, publishing to the update channel. |

## Build, CI and the update channel

- CI (`switch_build.yml`) runs on every push except Markdown-only changes. It builds the NXVK portlib from
  `PalindromicBreadLoaf/nxvk` (cached per NXVK commit), then the Release `.nro` in `devkitpro/devkita64`
  (about 8-13 minutes), uploads the `armsx2nx-vulkan` (`.nro`) and `armsx2nx-vulkan-elf` (unstripped ELF,
  14 days) artifacts, and publishes the `.nro` plus `version.txt` (`sha=`, `date=`, `title=`) to the pre-release
  **`nightly-<branch>`** (`nightly` for the default branch, `master`). Concurrency cancels the previous run of a branch.
- The app's **Update** menu downloads from the channel compiled into it (`ARMSX2_UPDATE_REPO`/`ARMSX2_UPDATE_TAG`),
  overridable with `[Horizon] UpdateRepository` / `UpdateChannel` in `sdmc:/switch/armsx2/armsx2.ini`. So a build of
  a new branch only reaches the owner if their installed build follows that branch's channel (or they install it by hand).
- To confirm a build is live: `curl -sL https://github.com/<repo>/releases/download/nightly-<branch>/version.txt`.
- Local build: see README "Building". There is no devkitPro in a typical agent container; use the checks below
  and let CI build.

## Verifying without a Switch

1. **Syntax/type check** of the files you touched, with `__SWITCH__` defined (catches most build breaks in a
   minute instead of a CI round trip):
   ```sh
   git clone --depth 1 https://github.com/switchbrew/libnx /tmp/libnx
   LIBNX_INCLUDE=/tmp/libnx/nx/include switch/tools/syntax-check.sh pcsx2/x86/microVU.cpp pcsx2/VMManager.cpp
   ```
   (`microVU.cpp` includes all `microVU*.inl`; `iR5900*.cpp` the EE recompiler.)
2. **Generated-code tests** (`switch/tools/jit-tests/run.sh`, needs `g++-aarch64-linux-gnu` and `qemu-user`):
   each test emits A64 with the project's vixl, runs it under `qemu-aarch64` and compares it with a C++
   reference, usually the interpreter's code, on random and special values (NaN, Inf, denormals, FZ on/off).
   `harness.h` is the minimal JIT runner. Write one for every non-trivial change to emitted code; examples:
   `test_lwr` (EE LWR vs the interpreter), `test_mmi` (MMI sequences from `arm64/MMIEmitters.h`),
   `test_clamptrack` (soundness of `ClampTracker` on random instruction streams), `test_movmsk`.
   A mutation (break the code on purpose, see the test fail) is a cheap check that a test tests something.
3. **Struct layout / offsets**: compile a probe that stores `offsetof(...)` values into a global array with the
   same flags as `syntax-check.sh`, then read them back from the object's `.rodata` (`objcopy -O binary`).
4. **Host unit tests** for pure frontend logic: `switch/app/tests/*.cpp` (build lines at the top of each file).

## Reading what the owner sends

All files are in `sdmc:/switch/armsx2/logs/`. README "Performance log and profiler" documents the formats.

- **`emulog.txt`**: `[Perf]` every 5 s (speed, fps, limiter, CPU clock; per-thread load and waits; GS work per
  vsync; recompiler activity), `[Hitch]` for frames much longer than their neighbours, `[Prof]` every 30 s.
  `switch/tools/logs/perf_summary.py old.txt new.txt` averages the intervals with a given limiter so builds
  can be compared. Scenes differ between runs: compare per-primitive costs and individual intervals, and check
  the CPU clock column before blaming the emulator.
- **`profile.txt`** (rewritten each game boot): per 30 s window and for the whole session, per thread: time in
  the kernel, self/inclusive functions, callers, stacks, malloc call sites, EE guest PCs, and the **hottest JIT
  code** as raw instruction words. `switch/tools/logs/jit_hot.py profile.txt [--thread VU1] [--windows N]`
  disassembles and classifies it (clamps, x27/x28 accesses, address building, float math, ...).
- **`crash.txt`** (from `HorizonException.cpp` via `HorizonProfiler::WriteCrashReport`): thread, exception,
  registers, a frame-pointer backtrace as `nro+0x...`, EE/VU1 PCs. The first line names the build. To symbolise,
  download that build's `armsx2nx-vulkan-elf` artifact (GitHub API: list the run's artifacts, then
  `download_workflow_run_artifact`); NRO offsets are ELF virtual addresses:
  `aarch64-linux-gnu-addr2line -f -C -i -e armsx2.elf 0x<offset - 4 for return addresses>`.
- Do not trust "fixed" without a log from the console: say what was verified on the host and what was not.

## ARM64 recompiler conventions

- Registers (`arm64/VixlHelpers.h`): `x27` = `RSTATE_CPU` (`cpuRegistersPack`: EE, VU and FPU registers,
  `mVUglob` constants), `x28` = `RSTATE_PSX` in EE/IOP code and `RSTATE_MVU` (`vuRegistersPack`: `microVU[2]`,
  `vu1Thread`) in microVU code, `x25` fastmem base, `x26` IOP memory, `x16` vixl scratch, `x17` `RSCRATCHADDR`,
  `w0`-`w4` = `EAX`, `ECX`, `EDX`, `EBX`, `EEX` (helper scratch). Vector scratch `q29`-`q31`.
- microVU vector registers: allocatable `q0`-`q26` (`q15` holds PQ), `q27`/`q28` = clamp constants
  (+FLT_MAX / -FLT_MAX), loaded lazily per `armCodegenBarrier` (bumped on bound labels, calls, jumps and at
  block start/end; code after a barrier may be reached from elsewhere, so register-state assumptions reset).
- `PTR_CPU(field)` / `PTR_MVU(field)` turn a field into `[x27/x28, #offset]`. On Switch the offset comes from
  `&((T*)0)->field` (GCC needs constants), so a path through a **reference** member dereferences address 0
  while compiling code: `microVU::prog` is a heap reference for that reason, address it with
  `armMemOperandPtr(&mVU.prog...)`.
- **Keep hot fields within immediate-offset reach**: A64 loads/stores encode at most 16 KB (32-bit) / 64 KB
  (128-bit) offsets; beyond that every access costs MOVZ+MOVK+access. `microVU[1]` sat 290 KB in until its
  program cache moved to the heap (8% of hot VU1 instructions were offset building). Do not put large members in
  front of hot ones in `cpuRegistersPack`/`vuRegistersPack`/`microVU`/`VU_Thread`.
- `mVUclamp1` skips registers `ClampTracker` knows are clamped (it decodes the instructions emitted since the last
  clamp). Anything that writes a vector register without going through emitted code invalidates that knowledge.
- `__fi` is only `always_inline`; with LTO, helpers defined in headers need `__forceinline_odr` / `inline`.

## Switch / Horizon traps (all hit before)

- libnx's `u128`/`s128` clash with PCSX2's: include `common/Horizon/Horizon.h`, never `<switch.h>` directly.
- newlib closes all still-open FILEs in `exit()` after `main()` returns, when romfs/sockets are already gone:
  `main.cpp` closes fds left on `romfs:`/`soc:` (`CloseFilesOnDevice`) before `romfsExit()`/`socketExit()`.
- NXVK advertises `VK_EXT_swapchain_maintenance1`, but its Horizon WSI has no `release_images`, so
  `vkReleaseSwapchainImagesEXT` jumps to 0 (crash on VSync/present mode changes). Disabled on Switch in
  `GSDeviceVK.cpp`. Check other WSI-level entry points before relying on them.
- `cpuinfo` cannot identify the CPU on Horizon: use `HorizonSystemInfo`.
- Newlib's `memalign` takes a global lock; hot allocations of fixed-size objects on one thread (GS texture cache
  `Source`) use a per-thread free list (`GSTextureCache.cpp`).
- Threads: EE on core 0, VU1 (MTVU) on core 1, GS on core 2, helpers on cores 2-3 (`HorizonCores`, README).
- Self-modifying-code write protection and code memory aliasing: `common/Horizon/HorizonHostSys.cpp`; the log
  line "Horizon SMC write protection enabled" confirms the fast path.

## Porting from upstream PCSX2

A full rebase is not realistic (about 2250 upstream commits since v2.3.431, x86-only recompiler changes that
would all need translating, heavy GS rework). Port individual fixes:

- Clone with history but without blobs: `git clone --filter=blob:none --no-checkout --shallow-since=2025-05-01
  https://github.com/PCSX2/pcsx2` (v2.3.431 = commit `08e68d9`).
- Interpreter/GS C++ changes usually apply with `git format-patch -1 <sha> -- <file>` and
  `git apply --directory=app/src/main/cpp -p1`.
- x86 recompiler changes: find the commented x86 lines in our ARM64 code and translate the change next to them;
  mind 32-bit vs 64-bit register writes (both ISAs zero the upper half on 32-bit writes).
- Already ported: EE `LWR` upper-half fix, microVU EATAN coefficient order, RSQRT divide flag, FBRST ordering,
  interpreter VU SQRT/RSQRT flags, CTC2 status sticky flags, EE FPU SQRT.S/RSQRT.S flags (commit `8285235`).
- Worth porting next: the GS triangle overlap / draw-list series (`a1173c5`, `4d93285`, `6463fc5`, `6e129a6`,
  `d912887`, `1213621`, ...), which lets `require_full_barrier` draws of triangles batch non-overlapping prims;
  today `GSDeviceVK::SendHWDraw` issues a barrier per triangle (GT4: 1000-4000 barriers per frame). Smaller:
  microVU MAC-flag block matching (`43e5ec2` + `4102a0b`), texture cache offset loops (`e550cf9`, `96ac35a`).
- Not worth it here: draw buffering (opt-in per game, not GT4), SPU accuracy changes (cost CPU), 64-bit cycle
  counters, desktop-GPU workarounds, DX12/Metal/Qt.

## Gran Turismo 4 (PAL SCES-51719): status

Settings that matter: GameDB gives VU clamp mode 2 (Extra), half pixel offset 4, auto flush 1,
`GSC_PolyphonyDigitalGames`. **Hardware Download Mode = Unsynchronized** removes per-frame GPU readbacks
(sun glare) and was the biggest single win (race 54-62% -> 84-102% at the time).

What was done (see `git log` for details): EE block list gap buffer (compile hitches 1.18 s -> 0.29 s), Vulkan
pipeline prewarm from earlier sessions, VU clamp constants in registers, redundant clamp skipping
(`ClampTracker`), VU1 state within immediate offsets, GS `Source` free list (GS thread 87% -> 72% in VU1-bound
intervals), crash fixes (car select VU1 compile, app exit, swapchain release).

Where it stands (last logs): **VU1 is the limiter** in races and especially the car selection screen
(37-47k primitives per frame, VU1 100% of its core, speed 74-92% even at 2703 MHz). In the hottest VU1 code
31% of the instructions are clamps against 7% float arithmetic, because Extra clamping clamps both operands and
the result of every operation. The GS thread (70-85%) is next in line, then GPU barriers.

Open threads, in order:
1. **VU1 Clamping Override** (`[EmuCore/CPU/Recompiler] vu1ClampOverride`, Advanced settings, applied after the
   GameDB in `VMManager::ApplyGameFixes`): waiting for the owner's test of *Normal* on GT4 (speed, and whether
   text in GT mode menus breaks; the GameDB comment for mode 2 is "Text in GT mode works"). If Normal breaks it,
   look for cheaper ways to keep Extra semantics (e.g. clamp operands only when they come from memory/LQ).
2. GS barriers per triangle (upstream draw-list series above).
3. Loading screens: EE-bound frames of 1-1.4 s with `psxRcntUpdate` / SPU2 mixing while the game spins at EE
   PC 0x00081fc0; not investigated.

## Conventions

- Commit messages: what was measured, why the change, how it was verified; no model names or identifiers.
- Keep README.md in sync with user-visible behaviour (settings, log formats).
- Do not push to `master` or open pull requests unless the owner asks.
