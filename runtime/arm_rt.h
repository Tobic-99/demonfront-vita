// Runtime for the recompiled IGS027A (ARM7TDMI) Thumb code (gen/arm/, tools/thumbrec.py).
//
// Guest memory is reached through a table of 64 KB pages (arm::page[addr >> 16]): a page that is plain memory
// (ROM, the three RAMs, the selected shared-RAM bank) points at its host bytes; anything else (the latch to the
// 68000, the RAM-bank select) is null and goes to the slow path in host/arm_mem.cpp. Both sides are
// little-endian, so words are read directly. The compiled game never makes unaligned word accesses.
#pragma once
#include <cstdint>
#include <cstring>

// Function-entry trace for debugging (build with -DDF_TRACE): a ring of the last entered functions,
// dumped by the host when something goes wrong (host/trace.cpp).
#ifdef DF_TRACE
namespace dftrace { void enter(int cpu, uint32_t addr); }
#define DF_TRACE_ARM(a) dftrace::enter(0, a)
#else
#define DF_TRACE_ARM(a) ((void)0)
#endif

namespace arm {
struct Ctx {
    uint32_t r[16];
    int32_t budget;        // counts down at backward branches; at zero, yield() lets the other CPUs run
};

extern uint8_t* page[0x10000];
uint32_t io_rd(uint32_t a, int size);
void io_wr(uint32_t a, uint32_t v, int size);

inline uint32_t rd32(uint32_t a) { uint8_t* p = page[a >> 16]; if (p) { uint32_t v; std::memcpy(&v, p + (a & 0xfffc), 4); return v; } return io_rd(a, 4); }
inline uint32_t rd16(uint32_t a) { uint8_t* p = page[a >> 16]; if (p) { uint16_t v; std::memcpy(&v, p + (a & 0xfffe), 2); return v; } return io_rd(a, 2); }
inline uint32_t rd8(uint32_t a)  { uint8_t* p = page[a >> 16]; if (p) return p[a & 0xffff]; return io_rd(a, 1); }
inline void wr32(uint32_t a, uint32_t v) { uint8_t* p = page[a >> 16]; if (p) std::memcpy(p + (a & 0xfffc), &v, 4); else io_wr(a, v, 4); }
inline void wr16(uint32_t a, uint16_t v) { uint8_t* p = page[a >> 16]; if (p) std::memcpy(p + (a & 0xfffe), &v, 2); else io_wr(a, v, 2); }
inline void wr8(uint32_t a, uint8_t v)   { uint8_t* p = page[a >> 16]; if (p) p[a & 0xffff] = v; else io_wr(a, v, 1); }

// register-specified shifts (amount 0..255) with the ARM carry rules
inline uint32_t lsl_c(uint32_t v, uint32_t n, bool& C) {
    if (n == 0) return v;
    if (n < 32) { C = (v >> (32 - n)) & 1; return v << n; }
    C = n == 32 ? (v & 1) : 0; return 0;
}
inline uint32_t lsr_c(uint32_t v, uint32_t n, bool& C) {
    if (n == 0) return v;
    if (n < 32) { C = (v >> (n - 1)) & 1; return v >> n; }
    C = n == 32 ? (v >> 31) : 0; return 0;
}
inline uint32_t asr_c(uint32_t v, uint32_t n, bool& C) {
    if (n == 0) return v;
    if (n < 32) { C = (v >> (n - 1)) & 1; return (uint32_t)((int32_t)v >> n); }
    C = v >> 31; return (uint32_t)((int32_t)v >> 31);
}

struct FuncEntry { uint32_t addr; void (*fn)(Ctx*); };
extern const FuncEntry func_table[];
extern const unsigned func_count;

void dispatch(Ctx* c, uint32_t target);          // call the function at target (bit 0 = Thumb)
void yield(Ctx* c);                              // budget used up: let the 68000 / sound / video run
[[noreturn]] void fatal_fell(Ctx* c, uint32_t pc);
[[noreturn]] void fatal_switch(Ctx* c, uint32_t pc, uint32_t value);
[[noreturn]] void fatal_untranslated(Ctx* c, uint32_t pc, const char* why);
}
