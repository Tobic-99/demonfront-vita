// The scheduler (host/scheduler.cpp): both recompiled CPUs, interleaved per scanline.
#pragma once
#include <cstdint>
#include <utility>
#include <vector>
#include "arm_rt.h"
#include "m68k_rt.h"

namespace sched {
constexpr int LINES = 264, VBLANK_LINE = 224, M68K_CYCLES_PER_LINE = 1280;
struct Stats {
    uint64_t frames, irq4, irq6, syncs;
    bool sample_pcs;
    bool timing;                     // measure host time per CPU (ns_*)
    uint64_t ns_68k, ns_arm, ns_sound;
};
extern Stats stats;
extern void (*on_vblank)();   // the frame is complete (render it now)
extern m68k::Ctx m68k_ctx;
extern arm::Ctx arm_ctx;
void init();          // after board::arm_init / m68k_init
void run_frame();
void m68k_yield();
std::vector<std::pair<uint32_t, uint32_t>> take_pc_hist();   // 68000 yield addresses since the last call
}
