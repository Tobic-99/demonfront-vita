// Host side of the recompiled IGS027A ARM code (gen/arm/): its memory map, the I/O it touches, dispatch of
// indirect calls, and native stand-ins for the few ARM-mode C-library routines in the ROM.
//
// ARM map (from the board's wiring as documented by MAME's driver; our own implementation):
//   0x00000000  internal ROM (16 KB, never dumped; the game never calls into it after boot)
//   0x08000000  external ROM (rom/arm_ext.bin, decrypted, 4 MB)
//   0x10000000  RAM, 1 KB            0x18000000  RAM, 256 KB            0x50000000  RAM, 1 KB
//   0x38000000  shared RAM with the 68000: two 64 KB banks... (128 KB each, the ARM sees bank ram_sel)
//   0x40000018  shared-RAM bank select (bit 0)          0x48000000  latch: read = 68k's word, write = to 68k
#include "arm_rt.h"
#include "board.h"
#include "trace.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace arm {
uint8_t* page[0x10000];
}

namespace board {
std::vector<uint8_t> arm_rom, arm_ram0(0x400), arm_ram1(0x40000), arm_ram2(0x400);
uint8_t share[2][0x20000];
int ram_sel = 1;                 // MAME starts Demon Front with bank 1 selected for the ARM
uint32_t latch_to_68k, latch_to_arm;
uint64_t arm_yields;

static void map(uint32_t base, uint8_t* p, uint32_t size) {
    for (uint32_t a = 0; a < size; a += 0x10000) arm::page[(base + a) >> 16] = p + a;
}
// The ARM sees bank ram_sel at 0x38000000 (64 KB window; MAME maps 0x38000000-0x3800ffff), the 68000 the other.
void arm_map_share() { arm::page[0x3800] = share[ram_sel & 1]; m68k_map_share(); }

void arm_init() {
    std::fill(std::begin(arm::page), std::end(arm::page), nullptr);
    map(0x08000000, arm_rom.data(), (uint32_t)arm_rom.size());
    // RAMs smaller than a page: give each a full 64 KB host page so the fast path needs no size check
    static uint8_t ram0[0x10000], ram2[0x10000];
    map(0x10000000, ram0, 0x10000);
    map(0x18000000, arm_ram1.data(), 0x40000);
    map(0x50000000, ram2, 0x10000);
    arm_map_share();
}
}

namespace arm {
uint32_t io_rd(uint32_t a, int size) {
    if ((a & ~3u) == 0x48000000) return board::latch_to_arm;
    std::printf("[arm] unmapped read%d %08x\n", size * 8, a);
    return 0;
}
void io_wr(uint32_t a, uint32_t v, int size) {
    if ((a & ~3u) == 0x48000000) {
        board::latch_to_68k = v;
        if (std::getenv("DF_LATCHLOG")) {
            std::printf("[latch] arm -> 68k %08x (size %d)\n", v, size);
#ifdef DF_TRACE
            dftrace::dump(0, 24);
#endif
        }
        sched::arm_sync();    // the 68000 must see each reply before the ARM polls the latch again
        return;
    }
    if ((a & ~3u) == 0x40000018) { board::ram_sel = v & 1; board::arm_map_share(); sched::arm_sync(); return; }
    std::printf("[arm] unmapped write%d %08x <- %08x\n", size * 8, a, v);
}

void dispatch(Ctx* c, uint32_t target) {
    uint32_t a = target & ~1u;
    if (!(target & 1)) { std::printf("[arm] dispatch to ARM-mode %08x (lr %08x)\n", target, c->r[14]); std::exit(1); }
    const FuncEntry* e = std::lower_bound(func_table, func_table + func_count, a, [](const FuncEntry& x, uint32_t v) { return x.addr < v; });
    if (e == func_table + func_count || e->addr != a) {
        std::printf("[arm] dispatch: no function at %08x (lr %08x, sp %08x)\n", target, c->r[14], c->r[13]);
#ifdef DF_TRACE
        dftrace::dump(0, 40);
#endif
        std::exit(1);
    }
    e->fn(c);
}

void yield(Ctx* c) {
    c->budget = board::arm_budget;
    board::arm_yields++;
    board::arm_yield_hook();
}

void fatal_fell(Ctx* c, uint32_t pc) { std::printf("[arm] fell off translated code at %08x (lr %08x)\n", pc, c->r[14]); std::exit(1); }
void fatal_switch(Ctx* c, uint32_t pc, uint32_t v) { std::printf("[arm] switch at %08x: value %08x has no case\n", pc, v); (void)c; std::exit(1); }
void fatal_untranslated(Ctx* c, uint32_t pc, const char* why) {
    std::printf("[arm] call to untranslated %08x (%s), lr %08x r0 %08x\n", pc, why, c->r[14], c->r[0]);
    std::exit(1);
}
}
