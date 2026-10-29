// The scheduler: runs the recompiled 68000 and ARM7 side by side, one scanline slice at a time, each on its own
// fiber, and raises the IGS023's interrupts (IRQ6 at vblank, IRQ4 every 62 lines).
//
// Timing (PGM): pixel clock 50 MHz / 5, 640 clocks x 264 lines per frame (59.17 Hz), so one line is 64 us:
// 1280 cycles of the 68000 (20 MHz) and ~1408 of the ARM (22 MHz). The ARM budget counts loop iterations
// (thumbrec.py), not cycles.
#include "scheduler.h"
#include "arm_rt.h"
#include "board.h"
#include "fiber.h"
#include "m68k_rt.h"
#include "video.h"
#include "sound.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <vector>

namespace board { int32_t arm_budget = 64; void (*arm_yield_hook)(); }

namespace sched {
static fiber::Fiber *main_f, *m68k_f, *arm_f;
m68k::Ctx m68k_ctx;
arm::Ctx arm_ctx;
Stats stats;
void (*on_vblank)();
static int irq4_count;          // lines since the last IRQ4 tick (the divider runs continuously)
static std::map<uint32_t, uint32_t> pc_hist;

void m68k_yield() {
    if (stats.sample_pcs) pc_hist[m68k_ctx.pc]++;
    fiber::switch_to(main_f);
}
static void arm_hook() { fiber::switch_to(main_f); }
// Called from inside an I/O write: the CPU's fiber stops in the middle of the instruction and continues there
// when the scheduler next runs it; its registers live on that fiber's stack, so nothing has to be stored.
void m68k_sync() { m68k_ctx.budget = 0; stats.syncs++; fiber::switch_to(main_f); }
void arm_sync() { stats.syncs++; fiber::switch_to(main_f); }

static void m68k_main(void*) {
    m68k::Ctx* c = &m68k_ctx;
    c->a[7] = m68k::rd32(0);       // reset: SSP and PC from the BIOS vectors, supervisor, interrupts masked
    c->pc = m68k::rd32(4);
    c->sr_hi = 0x27;
    c->budget = 0;
    for (;;) {                     // a function returning to an address outside any call chain lands here
        uint32_t t = c->pc;
        c->link = 0xffffffff;
        m68k::dispatch(c, t);
        if (c->pc == 0xffffffff) m68k::fatal(c, t, "returned from the top level");
    }
}

static void arm_main(void*) {
    arm::Ctx* c = &arm_ctx;
    c->r[13] = 0x10000400;         // what the undumped internal ROM does: sp, then jump to the external ROM
    c->r[14] = 0xfffffff1;
    c->budget = board::arm_budget;
    arm::dispatch(c, arm::rd32(0x08017ce4 + 8));
    std::printf("[sched] ARM main returned (lr %08x)\n", c->r[14]);
    for (;;) fiber::switch_to(main_f);
}

void init() {
    main_f = fiber::current();
    board::arm_yield_hook = arm_hook;
    // MAME seeds the region word (shared RAM +0x158 = 5) in both banks; the internal ROM probably supplies it
    board::share[0][0x158] = 5; board::share[1][0x158] = 5;
    sound::reset();
    m68k_f = fiber::create(m68k_main, nullptr, 1 << 20);
    arm_f = fiber::create(arm_main, nullptr, 1 << 20);
}

void run_frame() {
    for (int line = 0; line < LINES; line++) {
        board::vpos = line;
        if (line == 220) video::sprite_dma();
        if (line == VBLANK_LINE) {
            video::latch_sprites();
            if (on_vblank) on_vblank();
            if (board::vregs.ctrl & 8) { board::irq6_pending = true; stats.irq6++; }
        }
        if (++irq4_count >= 62) {
            irq4_count = 0;
            if (board::vregs.ctrl & 4) { board::irq4_pending = true; stats.irq4++; }
        }
        m68k_ctx.budget = std::min(m68k_ctx.budget, 0) + M68K_CYCLES_PER_LINE;
        if (stats.timing) {
            auto t0 = std::chrono::steady_clock::now();
            fiber::switch_to(m68k_f);
            auto t1 = std::chrono::steady_clock::now();
            fiber::switch_to(arm_f);
            auto t2 = std::chrono::steady_clock::now();
            sound::run_line();
            auto t3 = std::chrono::steady_clock::now();
            stats.ns_68k += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            stats.ns_arm += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
            stats.ns_sound += std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
        } else {
            fiber::switch_to(m68k_f);
            fiber::switch_to(arm_f);
            sound::run_line();
        }
    }
    stats.frames++;
}

std::vector<std::pair<uint32_t, uint32_t>> take_pc_hist() {
    std::vector<std::pair<uint32_t, uint32_t>> v(pc_hist.begin(), pc_hist.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    pc_hist.clear();
    return v;
}
}
