// Function-entry trace of both recompiled CPUs (only in -DDF_TRACE builds; see runtime/*_rt.h).
#pragma once
#include <cstdint>

namespace dftrace {
void enter(int cpu, uint32_t addr);   // cpu 0 = ARM, 1 = 68000
void dump(int cpu, int n);            // print the last n functions entered by cpu
}
