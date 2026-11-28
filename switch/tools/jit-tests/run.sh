#!/usr/bin/env bash
# Runs the ARM64 JIT sequence tests on a Linux host, without a Switch: each test emits code with the
# project's vixl, executes it under qemu-aarch64 and compares it with a C++ reference (usually the
# interpreter's code) on random and special-value inputs.
#
#   switch/tools/jit-tests/run.sh             # all tests
#   switch/tools/jit-tests/run.sh test_lwr    # one test
#
# Needs aarch64-linux-gnu-g++ and qemu-aarch64 (Debian/Ubuntu: g++-aarch64-linux-gnu qemu-user).
# vixl is built once into $OUT (default: $TMPDIR/armsx2-jit-tests).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
CPP=$ROOT/app/src/main/cpp
VIXL=$CPP/3rdparty/vixl
OUT=${OUT:-${TMPDIR:-/tmp}/armsx2-jit-tests}
CXX=${CXX:-aarch64-linux-gnu-g++}
QEMU=${QEMU:-qemu-aarch64}
FLAGS=(-std=c++17 -O1 -DVIXL_INCLUDE_TARGET_A64 -I"$VIXL/include" -I"$HERE" -I"$CPP/pcsx2/arm64")

mkdir -p "$OUT/obj"
if [ ! -f "$OUT/libvixl.a" ]; then
	echo "Building vixl into $OUT ..."
	for src in "$VIXL"/src/*.cc "$VIXL"/src/aarch64/*.cc; do
		"$CXX" "${FLAGS[@]}" -I"$VIXL/include/vixl" -I"$VIXL/include/vixl/aarch64" -DVIXL_CODE_BUFFER_MALLOC -w \
			-c "$src" -o "$OUT/obj/$(basename "$src").o"
	done
	aarch64-linux-gnu-ar rcs "$OUT/libvixl.a" "$OUT"/obj/*.o
fi

tests=("$@")
if [ ${#tests[@]} -eq 0 ]; then
	for t in "$HERE"/test_*.cpp; do tests+=("$(basename "$t" .cpp)"); done
fi

failed=0
for t in "${tests[@]}"; do
	echo "== $t"
	"$CXX" "${FLAGS[@]}" "$HERE/$t.cpp" "$OUT/libvixl.a" -static -o "$OUT/$t"
	if ! "$QEMU" "$OUT/$t"; then
		echo "FAILED: $t"
		failed=1
	fi
done
exit $failed
