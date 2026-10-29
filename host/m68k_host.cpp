// Host side of the recompiled 68000 code (gen/m68k/): the PGM memory map, I/O registers, interrupts, and the
// control-flow helpers the generated code calls (ret/dispatch/yield).
//
// 68000 map (PGM main board + IGS027A type 3; from MAME's driver as documented, our own implementation):
//   0x000000  BIOS ROM (128 KB)                    0x100000  game ROM (first 1 MB visible: MAME maps 0x100000-0x1fffff)
//   0x500000  shared RAM with the ARM (64 KB window of bank ram_sel ^ 1)
//   0x5c0000  FIQ to the ARM (write)               0x5c0300  latch: read = ARM's word, write = to the ARM
//   0x700006  watchdog                             0x800000  main RAM 128 KB, mirrored to 0x8fffff
//   0x900000  IGS023 video RAM 32 KB, mirrored     0xa00000  palette (xRGB555)
//   0xb00000  IGS023 registers (sprite buffer, zoom table, scrolls, 0xb07000 line counter, 0xb0e000 control)
//   0xc00002-0xc0000d  Z80 latches/reset/control, RTC   0xc08000  inputs, DIP switches   0xc10000  Z80 RAM
#include "m68k_rt.h"
#include "board.h"
#include "trace.h"
#include "sound.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace m68k {
uint8_t* rpage[0x1000];
uint8_t* wpage[0x1000];
}

namespace board {
std::vector<uint16_t> bios, game;
uint16_t main_ram[0x10000];
uint16_t video_ram[0x4000];
uint16_t palette[0x1000];
uint16_t sprite_buf[0x800];
uint8_t z80_ram[0x10000];
VideoRegs vregs;
bool irq4_pending, irq6_pending;
uint16_t in_p1p2 = 0xffff, in_p3p4 = 0xffff, in_service = 0xffff, in_dsw = 0xffff;
uint8_t sound_latch[3];
int vpos;
uint64_t m68k_fiq_writes;

static void map(uint32_t base, uint32_t size, void* p, bool writable) {
    for (uint32_t a = 0; a < size; a += 0x1000) {
        m68k::rpage[(base + a) >> 12] = (uint8_t*)p + a;
        m68k::wpage[(base + a) >> 12] = writable ? (uint8_t*)p + a : nullptr;
    }
}
// mirror a block of 'size' bytes over [base, base + span)
static void mirror(uint32_t base, uint32_t span, uint32_t size, void* p) {
    for (uint32_t a = 0; a < span; a += size) map(base + a, size, p, true);
}

void m68k_map_share() { map(0x500000, 0x10000, share[(ram_sel & 1) ^ 1], true); }

void m68k_init() {
    std::fill(std::begin(m68k::rpage), std::end(m68k::rpage), nullptr);
    std::fill(std::begin(m68k::wpage), std::end(m68k::wpage), nullptr);
    map(0x000000, (uint32_t)bios.size() * 2, bios.data(), false);
    map(0x100000, std::min<uint32_t>((uint32_t)game.size() * 2, 0x100000), game.data(), false);
    mirror(0x800000, 0x100000, sizeof main_ram, main_ram);
    mirror(0x900000, 0x100000, sizeof video_ram, video_ram);
    map(0xa00000, 0x2000, palette, true);
    map(0xb00000, 0x1000, sprite_buf, true);
    m68k_map_share();
}
}

namespace m68k {
using namespace board;

static uint32_t unmapped(uint32_t a, int size, const char* what) {
    static int n;
    if (n++ < 50) std::printf("[68k] unmapped %s%d %06x\n", what, size * 8, a);
    return size == 1 ? 0xff : 0xffff;
}

// the 16-bit value of an I/O word at a (even); byte reads take the right half
static uint32_t io_word(uint32_t a) {
    if (a == 0x5c0300) return latch_to_68k & 0xffff;
    if (a == 0x5c0000) return 0;
    if (a >= 0xb01000 && a < 0xb01040) return vregs.zoom[(a - 0xb01000) >> 1];
    switch (a) {
    case 0xb02000: return vregs.bg_yscroll;
    case 0xb03000: return vregs.bg_xscroll;
    case 0xb04000: return vregs.bg_scale;
    case 0xb05000: return vregs.tx_yscroll;
    case 0xb06000: return vregs.tx_xscroll;
    case 0xb07000: return (uint32_t)vpos;
    case 0xb0e000: return vregs.ctrl;
    case 0xc00002: return sound::latch_r(0);
    case 0xc00004: return sound::latch_r(1);
    case 0xc00006: return 0;                 // RTC (V3021): not emulated yet
    case 0xc0000c: return sound::latch_r(2);
    case 0xc08000: return in_p1p2;
    case 0xc08002: return in_p3p4;
    case 0xc08004: return in_service;
    case 0xc08006: return in_dsw;
    }
    if (a >= 0xc10000 && a < 0xc20000) {     // Z80 RAM: one byte per 68000 byte address
        uint32_t o = a - 0xc10000;
        return (z80_ram[o] << 8) | z80_ram[o + 1];
    }
    return unmapped(a, 2, "read");
}

uint32_t io_rd(uint32_t a, int size) {
    if (size == 4) return (io_rd(a, 2) << 16) | io_rd(a + 2, 2);
    uint32_t w = io_word(a & ~1u);
    if (size == 1) return (a & 1) ? (w & 0xff) : (w >> 8);
    return w;
}

static void ctrl_w(uint16_t v) {
    vregs.ctrl = v;
    if (!(v & 4)) irq4_pending = false;      // interrupts stay asserted until their enable bit is cleared
    if (!(v & 8)) irq6_pending = false;
}

static void io_word_w(uint32_t a, uint32_t v, uint32_t mask) {   // mask: which byte(s) of the word are written
    auto comb = [&](uint32_t old) { return (uint16_t)((old & ~mask) | (v & mask)); };
    if (a == 0x5c0300) {
        latch_to_arm = comb(latch_to_arm);
        if (std::getenv("DF_LATCHLOG")) {
            std::printf("[latch] 68k -> arm %04x (frame line %d)\n", latch_to_arm, vpos);
#ifdef DF_TRACE
            dftrace::dump(1, 12);
            dftrace::dump(0, 40);
#endif
        }
        sched::m68k_sync();   // let the ARM see it before the 68000 goes on (MAME interleaves tightly here)
        return;
    }
    if (a == 0x5c0000) { m68k_fiq_writes++; return; }        // FIQ to the ARM: the game never enables FIQs
    if (a == 0x700006) return;                               // watchdog
    if (a >= 0xb01000 && a < 0xb01040) { uint16_t& z = vregs.zoom[(a - 0xb01000) >> 1]; z = comb(z); return; }
    switch (a) {
    case 0xb02000: vregs.bg_yscroll = comb(vregs.bg_yscroll); return;
    case 0xb03000: vregs.bg_xscroll = comb(vregs.bg_xscroll); return;
    case 0xb04000: vregs.bg_scale = comb(vregs.bg_scale); return;
    case 0xb05000: vregs.tx_yscroll = comb(vregs.tx_yscroll); return;
    case 0xb06000: vregs.tx_xscroll = comb(vregs.tx_xscroll); return;
    case 0xb0e000: ctrl_w(comb(vregs.ctrl)); return;
    case 0xc00002: if (mask & 0xff) sound::latch_w(0, (uint8_t)v); return;   // + NMI to the Z80
    case 0xc00004: if (mask & 0xff) sound::latch_w(1, (uint8_t)v); return;
    case 0xc00006: return;                                         // RTC
    case 0xc00008: sound::z80_control_w((uint16_t)v); return;      // Z80 reset (0x5050) / halt
    case 0xc0000a: return;                                         // Z80 bus control
    case 0xc0000c: if (mask & 0xff) sound::latch_w(2, (uint8_t)v); return;
    case 0xc08006: return;                                         // coin counters
    }
    if (a >= 0xc10000 && a < 0xc20000) {
        uint32_t o = a - 0xc10000;
        if (mask & 0xff00) z80_ram[o] = v >> 8;
        if (mask & 0x00ff) z80_ram[o + 1] = v;
        return;
    }
    if (a < 0x200000) return;   // ROM
    unmapped(a, mask == 0xffff ? 2 : 1, "write");
}

void io_wr(uint32_t a, uint32_t v, int size) {
    if (size == 4) { io_wr(a, v >> 16, 2); io_wr(a + 2, v & 0xffff, 2); return; }
    if (size == 2) { io_word_w(a & ~1u, v, 0xffff); return; }
    if (a & 1) io_word_w(a & ~1u, v & 0xff, 0x00ff);
    else io_word_w(a, (v & 0xff) << 8, 0xff00);
}

// ---- control flow ----
const FuncEntry* find(uint32_t addr) {
    const FuncEntry* e = std::lower_bound(func_table, func_table + func_count, addr,
                                          [](const FuncEntry& x, uint32_t v) { return x.addr < v; });
    return (e != func_table + func_count && e->addr == addr) ? e : nullptr;
}

void dispatch(Ctx* c, uint32_t target) {
    const FuncEntry* e = find(target & 0xffffff);
    if (!e) fatal(c, target, "dispatch: no translated function here");
    e->fn(c);
}

void ret(Ctx* c, uint32_t target, uint32_t link) {
    c->pc = target;
    if (target == link) return;
    if (const FuncEntry* e = find(target & 0xffffff)) {   // 'push address; rts' = jump
        c->link = link;
        e->fn(c);
    }
    // else: a return to an outer frame (callers unwind until their return address matches)
}

static uint32_t full_sr(Ctx* c) {
    return (c->sr_hi << 8) | (c->x << 4) | (c->n << 3) | (c->z << 2) | (c->v << 1) | (uint32_t)c->c;
}

// Take the pending interrupts the mask allows; the guest resumes at 'resume' (c->pc == resume afterwards
// unless a handler returned somewhere else).
static void take_irqs(Ctx* c, uint32_t resume) {
    for (;;) {
        int level = irq6_pending ? 6 : irq4_pending ? 4 : 0;
        int mask = (c->sr_hi >> 0) & 7;
        if (level == 0 || level <= mask) return;
        uint32_t sr = full_sr(c);
        c->a[7] -= 4; wr32(c->a[7], resume);
        c->a[7] -= 2; wr16(c->a[7], sr);
        c->sr_hi = (uint16_t)((c->sr_hi & ~0xa7) | 0x20 | level);   // supervisor, no trace, mask = level
        uint32_t handler = rd32((24 + level) * 4);
        c->link = resume;
        c->pc = resume;
        dispatch(c, handler);
        if (c->pc != resume) return;
    }
}

void yield(Ctx* c, uint32_t resume) {
    c->pc = resume;
    sched::m68k_yield();
    take_irqs(c, resume);
}

void sr_changed(Ctx* c, uint32_t next) {
    c->pc = next;
    take_irqs(c, next);
}

void fatal(Ctx* c, uint32_t pc, const char* what) {
    std::printf("[68k] fatal at %06x: %s\n", pc, what);
    for (int i = 0; i < 8; i++) std::printf("  d%d %08x  a%d %08x\n", i, c->d[i], i, c->a[i]);
    std::printf("  sr %04x link %06x\n", full_sr(c), c->link);
    std::exit(1);
}
}
