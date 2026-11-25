# Writes the function symbols of the linked ELF for the sampling profiler (pcsx2/Horizon/HorizonProfiler.cpp).
# Usage: cmake -DNM=<nm> -DELF=<armsx2.elf> -DOUT=<romfs/symbols.txt> -P gen_symbols.cmake
#
# Output: `nm -S -n --defined-only` lines of code symbols only, "<addr> <size> <type> <mangled name>".

if(NOT NM OR NOT ELF OR NOT OUT)
	message(FATAL_ERROR "gen_symbols.cmake: NM, ELF and OUT are required")
endif()

execute_process(
	COMMAND "${NM}" -S -n --defined-only "${ELF}"
	OUTPUT_FILE "${OUT}.nm"
	RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "gen_symbols.cmake: ${NM} failed (${result})")
endif()

file(STRINGS "${OUT}.nm" lines REGEX "^[0-9a-f]+ [0-9a-f]+ [tTwW] ")
file(REMOVE "${OUT}.nm")
list(LENGTH lines count)
list(JOIN lines "\n" text)
file(WRITE "${OUT}" "${text}\n")
message(STATUS "gen_symbols.cmake: ${count} function symbols")
