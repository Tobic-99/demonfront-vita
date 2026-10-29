// Runtime for the recompiled 68000 code (gen/m68k/, tools/m68krec.py).
//
// Memory: 24-bit address space in 4 KB pages (rpage/wpage[(a >> 12) & 0xfff]); a null page goes to the slow path
// (io_rd/io_wr in host/m68k_host.cpp). Host storage keeps every 68000 word as a native little-endian uint16_t (like
// the ROM files and the ARM's view of the shared RAM), so a word access is a plain load, a byte access flips
// address bit 0, and a long is two words (high word first).
//
// Control flow between translated functions (see m68krec.py): every call pushes the real return address on the
// guest stack and puts it in c->link; a function always returns with c->pc = the address the guest continues at,
// and the caller checks it is the expected one (otherwise the guest returned somewhere else: unwind further).
#pragma once
#include <cstdint>
#include <cstring>

// Function-entry trace for debugging (build with -DDF_TRACE): a ring of the last entered functions,
// dumped by the host when something goes wrong (host/trace.cpp).
#ifdef DF_TRACE
namespace dftrace { void enter(int cpu, uint32_t addr); }
#define DF_TRACE_68K(a) dftrace::enter(1, a)
#else
#define DF_TRACE_68K(a) ((void)0)
#endif

namespace m68k {
struct Ctx {
    uint32_t d[8], a[8];   // a[7] = the active stack pointer (the game always runs in supervisor mode)
    uint32_t usp;          // the other stack pointer (move usp)
    uint32_t pc;           // after a function returns: where the guest continues
    uint32_t link;         // the return address of the function being entered
    bool x, n, z, v, c;
    uint16_t sr_hi;        // SR bits 8-15: T, S, interrupt mask
    int32_t budget;        // cycles left in this slice; at <= 0 a backward branch yields to the scheduler
};

extern uint8_t* rpage[0x1000];
extern uint8_t* wpage[0x1000];
uint32_t io_rd(uint32_t a, int size);
void io_wr(uint32_t a, uint32_t v, int size);

inline uint32_t rd16(uint32_t a) {
    uint8_t* p = rpage[(a >> 12) & 0xfff];
    if (p) { uint16_t v; std::memcpy(&v, p + (a & 0xffe), 2); return v; }
    return io_rd(a & 0xffffff, 2);
}
inline uint32_t rd8(uint32_t a) {
    uint8_t* p = rpage[(a >> 12) & 0xfff];
    if (p) return p[(a & 0xfff) ^ 1];
    return io_rd(a & 0xffffff, 1);
}
inline uint32_t rd32(uint32_t a) {
    uint8_t* p = rpage[(a >> 12) & 0xfff];
    if (p && (a & 0xfff) != 0xffe) { uint32_t v; std::memcpy(&v, p + (a & 0xffe), 4); return (v << 16) | (v >> 16); }
    return (rd16(a) << 16) | rd16(a + 2);
}
inline void wr16(uint32_t a, uint32_t v) {
    uint8_t* p = wpage[(a >> 12) & 0xfff];
    if (p) { uint16_t w = (uint16_t)v; std::memcpy(p + (a & 0xffe), &w, 2); return; }
    io_wr(a & 0xffffff, v & 0xffff, 2);
}
inline void wr8(uint32_t a, uint32_t v) {
    uint8_t* p = wpage[(a >> 12) & 0xfff];
    if (p) { p[(a & 0xfff) ^ 1] = (uint8_t)v; return; }
    io_wr(a & 0xffffff, v & 0xff, 1);
}
inline void wr32(uint32_t a, uint32_t v) {
    uint8_t* p = wpage[(a >> 12) & 0xfff];
    if (p && (a & 0xfff) != 0xffe) { uint32_t w = (v << 16) | (v >> 16); std::memcpy(p + (a & 0xffe), &w, 4); return; }
    wr16(a, v >> 16); wr16(a + 2, v);
}

// ---- operand sizes (S = 1, 2, 4 bytes) and flags ----
template <int S> inline uint32_t M(uint32_t v) { return S == 1 ? (v & 0xff) : S == 2 ? (v & 0xffff) : v; }
template <int S> inline bool msb(uint32_t v) { return (v >> (S * 8 - 1)) & 1; }
template <int S> inline uint32_t merge(uint32_t reg, uint32_t v) {   // write the low S bytes of a data register
    return S == 1 ? ((reg & ~0xffu) | (v & 0xff)) : S == 2 ? ((reg & ~0xffffu) | (v & 0xffff)) : v;
}
inline uint32_t sx8(uint32_t v) { return (uint32_t)(int32_t)(int8_t)v; }
inline uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }

#define M68K_F bool& X, bool& N, bool& Z, bool& V, bool& C
template <int S> inline void nz(uint32_t r, bool& N, bool& Z) { N = msb<S>(r); Z = M<S>(r) == 0; }
template <int S> inline uint32_t logic(uint32_t r, bool& N, bool& Z, bool& V, bool& C) { r = M<S>(r); N = msb<S>(r); Z = r == 0; V = C = false; return r; }
template <int S> inline uint32_t add(uint32_t a, uint32_t b, M68K_F) {
    a = M<S>(a); b = M<S>(b);
    uint64_t t = (uint64_t)a + b; uint32_t r = M<S>((uint32_t)t);
    X = C = (t >> (S * 8)) & 1; V = msb<S>((a ^ r) & (b ^ r)); N = msb<S>(r); Z = r == 0; return r;
}
template <int S> inline uint32_t addx(uint32_t a, uint32_t b, M68K_F) {
    a = M<S>(a); b = M<S>(b);
    uint64_t t = (uint64_t)a + b + X; uint32_t r = M<S>((uint32_t)t);
    X = C = (t >> (S * 8)) & 1; V = msb<S>((a ^ r) & (b ^ r)); N = msb<S>(r); if (r) Z = false; return r;
}
template <int S> inline uint32_t sub(uint32_t a, uint32_t b, M68K_F) {   // a - b
    a = M<S>(a); b = M<S>(b);
    uint32_t r = M<S>(a - b);
    X = C = b > a; V = msb<S>((a ^ b) & (a ^ r)); N = msb<S>(r); Z = r == 0; return r;
}
template <int S> inline uint32_t subx(uint32_t a, uint32_t b, M68K_F) {
    a = M<S>(a); b = M<S>(b);
    uint64_t t = (uint64_t)a - b - X; uint32_t r = M<S>((uint32_t)t);
    X = C = ((uint64_t)b + X) > a; V = msb<S>((a ^ b) & (a ^ r)); N = msb<S>(r); if (r) Z = false; return r;
}
template <int S> inline void cmp(uint32_t a, uint32_t b, bool& N, bool& Z, bool& V, bool& C) {   // a - b
    a = M<S>(a); b = M<S>(b);
    uint32_t r = M<S>(a - b);
    C = b > a; V = msb<S>((a ^ b) & (a ^ r)); N = msb<S>(r); Z = r == 0;
}

// shifts and rotates; n = count (1..8 immediate, 0..63 from a register)
template <int S> inline uint32_t lsl(uint32_t v, uint32_t n, M68K_F) {
    const int B = S * 8; v = M<S>(v);
    if (n == 0) { C = false; }
    else if (n <= (uint32_t)B) { uint64_t t = (uint64_t)v << n; X = C = (t >> B) & 1; v = M<S>((uint32_t)t); }
    else { X = C = false; v = 0; }
    V = false; N = msb<S>(v); Z = v == 0; return v;
}
template <int S> inline uint32_t lsr(uint32_t v, uint32_t n, M68K_F) {
    const int B = S * 8; v = M<S>(v);
    if (n == 0) { C = false; }
    else if (n <= (uint32_t)B) { X = C = ((uint64_t)v >> (n - 1)) & 1; v = (uint32_t)((uint64_t)v >> n); }
    else { X = C = false; v = 0; }
    V = false; N = msb<S>(v); Z = v == 0; return v;
}
template <int S> inline uint32_t asr(uint32_t v, uint32_t n, M68K_F) {
    const int B = S * 8; int64_t sv = msb<S>(v) ? (int64_t)M<S>(v) - ((int64_t)1 << B) : (int64_t)M<S>(v);
    if (n == 0) { C = false; v = M<S>(v); }
    else if (n < (uint32_t)B) { X = C = (sv >> (n - 1)) & 1; v = M<S>((uint32_t)(sv >> n)); }
    else { X = C = sv < 0; v = sv < 0 ? M<S>(0xffffffffu) : 0; }
    V = false; N = msb<S>(v); Z = v == 0; return v;
}
template <int S> inline uint32_t asl(uint32_t v, uint32_t n, M68K_F) {
    const int B = S * 8; v = M<S>(v);
    if (n == 0) { C = false; V = false; }
    else if (n < (uint32_t)B) {
        uint32_t top = M<S>((uint32_t)(0xffffffffffffffffull << (B - 1 - n)));   // the n+1 bits that pass the sign
        V = (v & top) != 0 && (v & top) != top;
        uint64_t t = (uint64_t)v << n; X = C = (t >> B) & 1; v = M<S>((uint32_t)t);
    } else { V = v != 0; X = C = n == (uint32_t)B ? (v & 1) : 0; v = 0; }
    N = msb<S>(v); Z = v == 0; return v;
}
template <int S> inline uint32_t rol(uint32_t v, uint32_t n, bool& N, bool& Z, bool& V, bool& C) {
    const int B = S * 8; v = M<S>(v);
    if (n == 0) C = false;
    else { n %= B; if (n) v = M<S>((v << n) | (v >> (B - n))); C = v & 1; }
    V = false; N = msb<S>(v); Z = v == 0; return v;
}
template <int S> inline uint32_t ror(uint32_t v, uint32_t n, bool& N, bool& Z, bool& V, bool& C) {
    const int B = S * 8; v = M<S>(v);
    if (n == 0) C = false;
    else { n %= B; if (n) v = M<S>((v >> n) | (v << (B - n))); C = msb<S>(v); }
    V = false; N = msb<S>(v); Z = v == 0; return v;
}
template <int S> inline uint32_t roxl(uint32_t v, uint32_t n, M68K_F) {
    const int B = S * 8; v = M<S>(v);
    n %= (B + 1);
    for (uint32_t i = 0; i < n; i++) { bool out = msb<S>(v); v = M<S>((v << 1) | X); X = out; }
    C = X; V = false; N = msb<S>(v); Z = v == 0; return v;
}
template <int S> inline uint32_t roxr(uint32_t v, uint32_t n, M68K_F) {
    const int B = S * 8; v = M<S>(v);
    n %= (B + 1);
    for (uint32_t i = 0; i < n; i++) { bool out = v & 1; v = (v >> 1) | ((uint32_t)X << (B - 1)); X = out; }
    C = X; V = false; N = msb<S>(v); Z = v == 0; return v;
}
#undef M68K_F

struct FuncEntry { uint32_t addr; void (*fn)(Ctx*); };
extern const FuncEntry func_table[];
extern const unsigned func_count;
const FuncEntry* find(uint32_t addr);

void ret(Ctx* c, uint32_t target, uint32_t link);   // rts/rte/rtr to target: return, or jump if it is a function
void dispatch(Ctx* c, uint32_t target);             // call the function at target (c->link already set)
void yield(Ctx* c, uint32_t resume);                // slice used up at a backward branch to resume
void sr_changed(Ctx* c, uint32_t next);             // after a write to SR: take an interrupt now unmasked
[[noreturn]] void fatal(Ctx* c, uint32_t pc, const char* what);
}
