#include "trace.h"
#include <cstdio>

namespace dftrace {
static uint32_t ring[2][256];
static unsigned pos[2];
void enter(int cpu, uint32_t addr) { ring[cpu][pos[cpu]++ & 255] = addr; }
void dump(int cpu, int n) {
    std::printf("  [trace %s]", cpu ? "68k" : "arm");
    for (int i = n; i > 0; i--) std::printf(" %08x", ring[cpu][(pos[cpu] - i) & 255]);
    std::printf("\n");
}
}
