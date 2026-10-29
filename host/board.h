// The PGM board with the IGS027A (type-3 protection): state shared by the CPUs and devices.
#pragma once
#include <cstdint>
#include <vector>

namespace board {
// ---- ARM7 (IGS027A) ----
extern std::vector<uint8_t> arm_rom, arm_ram0, arm_ram1, arm_ram2;
extern uint8_t share[2][0x20000];   // shared RAM banks; the ARM sees share[ram_sel], the 68000 the other
extern int ram_sel;
extern uint32_t latch_to_68k, latch_to_arm;
extern uint64_t arm_yields;
extern int32_t arm_budget;          // ARM work between yields (backward branches)
extern void (*arm_yield_hook)();    // called at every ARM yield (the scheduler)
void arm_init();
void arm_map_share();               // after ram_sel changes: remaps both CPUs' view

// ---- 68000 ----
// All 68000-side memory is kept as native 16-bit words (see runtime/m68k_rt.h).
extern std::vector<uint16_t> bios, game;   // ROMs as loaded from the files (they are little-endian words)
extern uint16_t main_ram[0x10000];         // 128 KB at 0x800000 (mirrored up to 0x8fffff)
extern uint16_t video_ram[0x4000];         // 32 KB at 0x900000 (IGS023: bg, text, row scroll)
extern uint16_t palette[0x1000];           // 0xa00000 (0x1400 bytes used; 8 KB host block)
extern uint16_t sprite_buf[0x800];         // 0xb00000-0xb00fff (IGS023 sprite buffer)
extern uint8_t z80_ram[0x10000];           // the Z80's program/data RAM, 0xc10000 on the 68000
struct VideoRegs {
    uint16_t zoom[0x20];
    uint16_t bg_yscroll, bg_xscroll, bg_scale, tx_yscroll, tx_xscroll, ctrl;
};
extern VideoRegs vregs;
extern bool irq4_pending, irq6_pending;
extern uint16_t in_p1p2, in_p3p4, in_service, in_dsw;   // active low
extern uint8_t sound_latch[3];
extern int vpos;                           // current scanline (for 0xb07000)
extern uint64_t m68k_fiq_writes;
void m68k_init();                          // after loading bios/game
void m68k_map_share();
}

namespace sched {
void m68k_yield();      // the 68000's slice is used up: back to the scheduler
void m68k_sync();       // end the 68000's slice now (it wrote something the ARM must react to)
void arm_sync();        // end the ARM's slice now
}
