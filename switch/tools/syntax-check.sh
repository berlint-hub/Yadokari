#!/usr/bin/env bash
# Compile-checks core/Switch sources for aarch64 with __SWITCH__ defined, without devkitPro: catches syntax and
# type errors (and evaluates the Switch-only code paths) before a ~13 minute CI build. It does not link, and a
# host GCC is not devkitA64's, so CI stays the real build.
#
#   LIBNX_INCLUDE=/path/to/libnx/nx/include switch/tools/syntax-check.sh pcsx2/x86/microVU.cpp pcsx2/VMManager.cpp
#
# Paths are relative to app/src/main/cpp. LIBNX_INCLUDE: the nx/include directory of a libnx checkout
# (git clone --depth 1 https://github.com/switchbrew/libnx). Needs aarch64-linux-gnu-g++.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
CPP=$(cd "$HERE/../../app/src/main/cpp" && pwd)
: "${LIBNX_INCLUDE:?set LIBNX_INCLUDE to the nx/include directory of libnx}"
CXX=${CXX:-aarch64-linux-gnu-g++}

# newlib's <sys/lock.h>, which libnx includes, is not in the Linux toolchain.
STUB=${TMPDIR:-/tmp}/armsx2-nxstub
mkdir -p "$STUB/sys"
cat > "$STUB/sys/lock.h" << 'EOF'
#pragma once
#include <stdint.h>
typedef uint32_t _LOCK_T;
typedef struct { uint32_t lock, thread_tag, counter; } _LOCK_RECURSIVE_T;
typedef uint32_t _COND_T;
EOF

INC=(-I"$CPP" -I"$CPP/pcsx2" -I"$STUB" -I"$LIBNX_INCLUDE")
for d in include ccc/src vixl/include fmt/include rapidyaml/include cpuinfo/include xxhash/include fast_float/include \
	imgui/include simpleini/include lzma/include zlib libchdr/include rcheevos/include vulkan/include; do
	INC+=(-I"$CPP/3rdparty/$d")
done
INC+=(-I"$CPP/3rdparty")

status=0
for f in "$@"; do
	echo "== $f"
	# -fpermissive / -flax-vector-conversions: host GCC is stricter than devkitA64 about a few existing constructs
	# (NEON vector casts, PTR_CPU offsets with a runtime index).
	if ! out=$("$CXX" -std=c++20 -fsyntax-only -fpermissive -flax-vector-conversions -w -D__SWITCH__ \
		-DVIXL_INCLUDE_TARGET_A64 -DPCSX2_CORE -D_M_ARM64 -DARCH_ARM64 -DENABLE_VULKAN "${INC[@]}" "$CPP/$f" 2>&1); then
		printf '%s\n' "$out" | grep -E 'error' || printf '%s\n' "$out" | tail -5
		status=1
	fi
done
exit $status
