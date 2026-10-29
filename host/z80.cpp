// Z80 interpreter (see z80.h).
#include "z80.h"

namespace {
enum : uint8_t { FC = 0x01, FN = 0x02, FP = 0x04, FX = 0x08, FH = 0x10, FY = 0x20, FZ = 0x40, FS = 0x80 };

struct Tables {
    uint8_t sz53[256], sz53p[256];
    Tables() {
        for (int i = 0; i < 256; i++) {
            uint8_t f = (uint8_t)(i & (FS | FX | FY));
            if (i == 0) f |= FZ;
            sz53[i] = f;
            int bits = 0;
            for (int k = 0; k < 8; k++) bits += (i >> k) & 1;
            sz53p[i] = f | ((bits & 1) ? 0 : FP);
        }
    }
};
const Tables T;

// base T-states of the unprefixed opcodes (conditional extras added in the code)
const uint8_t cyc_main[256] = {
    4,10, 7, 6, 4, 4, 7, 4, 4,11, 7, 6, 4, 4, 7, 4,
    8,10, 7, 6, 4, 4, 7, 4,12,11, 7, 6, 4, 4, 7, 4,
    7,10,16, 6, 4, 4, 7, 4, 7,11,16, 6, 4, 4, 7, 4,
    7,10,13, 6,11,11,10, 4, 7,11,13, 6, 4, 4, 7, 4,
    4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
    4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
    4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
    7, 7, 7, 7, 7, 7, 4, 7, 4, 4, 4, 4, 4, 4, 7, 4,
    4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
    4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
    4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
    4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
    5,10,10,10,10,11, 7,11, 5,10,10, 0,10,17, 7,11,
    5,10,10,11,10,11, 7,11, 5, 4,10,11,10, 0, 7,11,
    5,10,10,19,10,11, 7,11, 5, 4,10, 4,10, 0, 7,11,
    5,10,10, 4,10,11, 7,11, 5, 6,10, 4,10, 0, 7,11,
};
}

void Z80::reset() {
    a = f = b = c = d = e = h = l = 0xff;
    af_ = bc_ = de_ = hl_ = 0xffff;
    ix = iy = 0xffff;
    sp = 0xffff; pc = 0;
    i = r = 0;
    iff1 = iff2 = halted = false;
    im = 0; ei_delay = 0;
    nmi_pending = irq_line = false;
}

namespace {
// The instruction executor. 'idx' selects HL (0), IX (1) or IY (2) for DD/FD-prefixed instructions.
struct Exec {
    Z80& z;
    int cycles = 0;
    int idx = 0;
    uint16_t addr_d = 0;   // (IX+d) address when idx != 0 and the instruction uses (HL)

    explicit Exec(Z80& zz) : z(zz) {}

    uint8_t rd(uint16_t a) { return z.read(a); }
    void wr(uint16_t a, uint8_t v) { z.write(a, v); }
    uint8_t fetch() { return z.read(z.pc++); }
    uint16_t fetch16() { uint16_t lo = fetch(); return (uint16_t)(lo | (fetch() << 8)); }
    uint16_t rd16(uint16_t a) { return (uint16_t)(rd(a) | (rd((uint16_t)(a + 1)) << 8)); }
    void wr16(uint16_t a, uint16_t v) { wr(a, (uint8_t)v); wr((uint16_t)(a + 1), (uint8_t)(v >> 8)); }
    void push(uint16_t v) { z.sp -= 2; wr16(z.sp, v); }
    uint16_t pop() { uint16_t v = rd16(z.sp); z.sp += 2; return v; }

    uint16_t& ixy() { return idx == 1 ? z.ix : z.iy; }
    uint16_t hl() { return idx == 0 ? (uint16_t)((z.h << 8) | z.l) : ixy(); }
    void set_hl(uint16_t v) { if (idx == 0) { z.h = (uint8_t)(v >> 8); z.l = (uint8_t)v; } else ixy() = v; }
    uint16_t bc() const { return (uint16_t)((z.b << 8) | z.c); }
    uint16_t de() const { return (uint16_t)((z.d << 8) | z.e); }
    uint16_t af() const { return (uint16_t)((z.a << 8) | z.f); }

    // rp table: BC DE HL SP; rp2: BC DE HL AF
    uint16_t rp(int p) { switch (p) { case 0: return bc(); case 1: return de(); case 2: return hl(); default: return z.sp; } }
    void set_rp(int p, uint16_t v) {
        switch (p) {
        case 0: z.b = (uint8_t)(v >> 8); z.c = (uint8_t)v; break;
        case 1: z.d = (uint8_t)(v >> 8); z.e = (uint8_t)v; break;
        case 2: set_hl(v); break;
        default: z.sp = v;
        }
    }
    uint16_t rp2(int p) { return p == 3 ? af() : rp(p); }
    void set_rp2(int p, uint16_t v) { if (p == 3) { z.a = (uint8_t)(v >> 8); z.f = (uint8_t)v; } else set_rp(p, v); }

    // (IX+d): fetch the displacement once per instruction
    uint16_t mem_hl() {
        if (idx == 0) return hl();
        return addr_d;
    }
    void fetch_disp() { if (idx) { int8_t d = (int8_t)fetch(); addr_d = (uint16_t)(ixy() + d); } }

    // 8-bit register by index (B C D E H L (HL) A); 'plain' = H/L are never IXH/IXL (when (IX+d) is also used)
    uint8_t reg(int k, bool plain = false) {
        switch (k) {
        case 0: return z.b; case 1: return z.c; case 2: return z.d; case 3: return z.e;
        case 4: return (idx && !plain) ? (uint8_t)(ixy() >> 8) : z.h;
        case 5: return (idx && !plain) ? (uint8_t)ixy() : z.l;
        case 6: return rd(mem_hl());
        default: return z.a;
        }
    }
    void set_reg(int k, uint8_t v, bool plain = false) {
        switch (k) {
        case 0: z.b = v; break; case 1: z.c = v; break; case 2: z.d = v; break; case 3: z.e = v; break;
        case 4: if (idx && !plain) ixy() = (uint16_t)((ixy() & 0x00ff) | (v << 8)); else z.h = v; break;
        case 5: if (idx && !plain) ixy() = (uint16_t)((ixy() & 0xff00) | v); else z.l = v; break;
        case 6: wr(mem_hl(), v); break;
        default: z.a = v;
        }
    }

    bool cond(int y) {
        switch (y) {
        case 0: return !(z.f & FZ); case 1: return z.f & FZ;
        case 2: return !(z.f & FC); case 3: return z.f & FC;
        case 4: return !(z.f & FP); case 5: return z.f & FP;
        case 6: return !(z.f & FS); default: return z.f & FS;
        }
    }

    // ---- ALU ----
    void add8(uint8_t v, int carry) {
        int r = z.a + v + carry;
        uint8_t res = (uint8_t)r;
        z.f = (uint8_t)(T.sz53[res] | (((z.a & 0xf) + (v & 0xf) + carry) > 0xf ? FH : 0) |
                        ((~(z.a ^ v) & (z.a ^ res) & 0x80) ? FP : 0) | (r > 0xff ? FC : 0));
        z.a = res;
    }
    uint8_t sub8(uint8_t v, int carry) {   // returns the result, sets flags
        int r = z.a - v - carry;
        uint8_t res = (uint8_t)r;
        z.f = (uint8_t)(T.sz53[res] | FN | (((z.a & 0xf) - (v & 0xf) - carry) < 0 ? FH : 0) |
                        (((z.a ^ v) & (z.a ^ res) & 0x80) ? FP : 0) | (r < 0 ? FC : 0));
        return res;
    }
    void alu(int y, uint8_t v) {
        switch (y) {
        case 0: add8(v, 0); break;
        case 1: add8(v, z.f & FC); break;
        case 2: z.a = sub8(v, 0); break;
        case 3: z.a = sub8(v, z.f & FC); break;
        case 4: z.a &= v; z.f = (uint8_t)(T.sz53p[z.a] | FH); break;
        case 5: z.a ^= v; z.f = T.sz53p[z.a]; break;
        case 6: z.a |= v; z.f = T.sz53p[z.a]; break;
        default: sub8(v, 0); z.f = (uint8_t)((z.f & ~(FX | FY)) | (v & (FX | FY))); break;   // CP: X/Y from the operand
        }
    }
    uint8_t inc8(uint8_t v) {
        uint8_t r = (uint8_t)(v + 1);
        z.f = (uint8_t)((z.f & FC) | T.sz53[r] | ((r & 0xf) == 0 ? FH : 0) | (r == 0x80 ? FP : 0));
        return r;
    }
    uint8_t dec8(uint8_t v) {
        uint8_t r = (uint8_t)(v - 1);
        z.f = (uint8_t)((z.f & FC) | T.sz53[r] | FN | ((r & 0xf) == 0xf ? FH : 0) | (r == 0x7f ? FP : 0));
        return r;
    }
    uint8_t rot(int y, uint8_t v) {   // CB rotates/shifts
        uint8_t r, c;
        switch (y) {
        case 0: c = v >> 7; r = (uint8_t)((v << 1) | c); break;                    // RLC
        case 1: c = v & 1; r = (uint8_t)((v >> 1) | (c << 7)); break;             // RRC
        case 2: c = v >> 7; r = (uint8_t)((v << 1) | (z.f & FC)); break;           // RL
        case 3: c = v & 1; r = (uint8_t)((v >> 1) | ((z.f & FC) << 7)); break;    // RR
        case 4: c = v >> 7; r = (uint8_t)(v << 1); break;                          // SLA
        case 5: c = v & 1; r = (uint8_t)((v >> 1) | (v & 0x80)); break;           // SRA
        case 6: c = v >> 7; r = (uint8_t)((v << 1) | 1); break;                    // SLL (undocumented)
        default: c = v & 1; r = (uint8_t)(v >> 1); break;                          // SRL
        }
        z.f = (uint8_t)(T.sz53p[r] | c);
        return r;
    }
    void add16(uint16_t v) {
        uint16_t a = hl();
        uint32_t r = (uint32_t)a + v;
        z.f = (uint8_t)((z.f & (FS | FZ | FP)) | (((a & 0xfff) + (v & 0xfff)) > 0xfff ? FH : 0) |
                        ((r >> 8) & (FX | FY)) | (r > 0xffff ? FC : 0));
        set_hl((uint16_t)r);
    }
    void adc16(uint16_t v) {
        uint16_t a = hl();
        int cy = z.f & FC;
        uint32_t r = (uint32_t)a + v + cy;
        uint16_t res = (uint16_t)r;
        z.f = (uint8_t)(((res >> 8) & (FS | FX | FY)) | (res == 0 ? FZ : 0) |
                        (((a & 0xfff) + (v & 0xfff) + cy) > 0xfff ? FH : 0) |
                        ((~(a ^ v) & (a ^ res) & 0x8000) ? FP : 0) | (r > 0xffff ? FC : 0));
        set_hl(res);
    }
    void sbc16(uint16_t v) {
        uint16_t a = hl();
        int cy = z.f & FC;
        int r = (int)a - v - cy;
        uint16_t res = (uint16_t)r;
        z.f = (uint8_t)(((res >> 8) & (FS | FX | FY)) | (res == 0 ? FZ : 0) | FN |
                        (((a & 0xfff) - (v & 0xfff) - cy) < 0 ? FH : 0) |
                        (((a ^ v) & (a ^ res) & 0x8000) ? FP : 0) | (r < 0 ? FC : 0));
        set_hl(res);
    }
    void daa() {
        uint8_t a = z.a, corr = 0;
        bool c = z.f & FC;
        if ((z.f & FH) || (a & 0xf) > 9) corr |= 0x06;
        if (c || a > 0x99) { corr |= 0x60; c = true; }
        uint8_t r = (z.f & FN) ? (uint8_t)(a - corr) : (uint8_t)(a + corr);
        bool hf = (z.f & FN) ? ((z.f & FH) && (a & 0xf) < 6) : ((a & 0xf) > 9);
        z.f = (uint8_t)(T.sz53p[r] | (z.f & FN) | (hf ? FH : 0) | (c ? FC : 0));
        z.a = r;
    }

    // ---- prefixed groups ----
    void cb() {
        // with DD/FD the displacement comes before the opcode: DD CB d op
        if (idx) fetch_disp();
        uint8_t op = fetch();
        int x = op >> 6, y = (op >> 3) & 7, k = op & 7;
        if (idx) {
            uint16_t a = addr_d;
            uint8_t v = rd(a);
            if (x == 1) {   // BIT
                uint8_t bit = v & (1 << y);
                z.f = (uint8_t)((z.f & FC) | FH | (bit ? 0 : (FZ | FP)) | (y == 7 && bit ? FS : 0) | ((a >> 8) & (FX | FY)));
                cycles += 20;
                return;
            }
            uint8_t r = x == 0 ? rot(y, v) : x == 2 ? (uint8_t)(v & ~(1 << y)) : (uint8_t)(v | (1 << y));
            wr(a, r);
            if (k != 6) set_reg(k, r, true);   // undocumented: the result is also copied to a register
            cycles += 23;
            return;
        }
        uint8_t v = reg(k);
        if (x == 1) {
            uint8_t bit = v & (1 << y);
            z.f = (uint8_t)((z.f & FC) | FH | (bit ? 0 : (FZ | FP)) | (y == 7 && bit ? FS : 0) | (v & (FX | FY)));
            cycles += k == 6 ? 12 : 8;
            return;
        }
        uint8_t r = x == 0 ? rot(y, v) : x == 2 ? (uint8_t)(v & ~(1 << y)) : (uint8_t)(v | (1 << y));
        set_reg(k, r);
        cycles += k == 6 ? 15 : 8;
    }

    void ed() {
        uint8_t op = fetch();
        int x = op >> 6, y = (op >> 3) & 7, k = op & 7, p = y >> 1, q = y & 1;
        idx = 0;   // ED instructions never use IX/IY
        if (x == 1) {
            switch (k) {
            case 0: {   // IN r,(C)
                uint8_t v = z.in(bc());
                if (y != 6) set_reg(y, v);
                z.f = (uint8_t)((z.f & FC) | T.sz53p[v]);
                cycles += 12; return;
            }
            case 1: z.out(bc(), y == 6 ? 0 : reg(y)); cycles += 12; return;   // OUT (C),r
            case 2: if (q) adc16(rp(p)); else sbc16(rp(p)); cycles += 15; return;
            case 3: {
                uint16_t nn = fetch16();
                if (q) set_rp(p, rd16(nn)); else wr16(nn, rp(p));
                cycles += 20; return;
            }
            case 4: { uint8_t v = z.a; z.a = 0; z.a = sub8(v, 0); cycles += 8; return; }   // NEG
            case 5: z.iff1 = z.iff2; z.pc = pop(); cycles += 14; return;                    // RETN / RETI
            case 6: z.im = (y & 3) == 0 ? 0 : (y & 3) == 2 ? 1 : (y & 3) == 3 ? 2 : 0; cycles += 8; return;
            default:
                switch (y) {
                case 0: z.i = z.a; cycles += 9; return;
                case 1: z.r = z.a; cycles += 9; return;
                case 2: z.a = z.i; z.f = (uint8_t)((z.f & FC) | T.sz53[z.a] | (z.iff2 ? FP : 0)); cycles += 9; return;
                case 3: z.a = z.r; z.f = (uint8_t)((z.f & FC) | T.sz53[z.a] | (z.iff2 ? FP : 0)); cycles += 9; return;
                case 4: {   // RRD
                    uint16_t a = hl(); uint8_t m = rd(a);
                    wr(a, (uint8_t)((z.a << 4) | (m >> 4)));
                    z.a = (uint8_t)((z.a & 0xf0) | (m & 0x0f));
                    z.f = (uint8_t)((z.f & FC) | T.sz53p[z.a]); cycles += 18; return;
                }
                case 5: {   // RLD
                    uint16_t a = hl(); uint8_t m = rd(a);
                    wr(a, (uint8_t)((m << 4) | (z.a & 0x0f)));
                    z.a = (uint8_t)((z.a & 0xf0) | (m >> 4));
                    z.f = (uint8_t)((z.f & FC) | T.sz53p[z.a]); cycles += 18; return;
                }
                default: cycles += 8; return;   // ED NOPs
                }
            }
        }
        if (x == 2 && y >= 4 && k <= 3) {   // block instructions
            bool dec = y & 1, rep = y >= 6;
            uint16_t step = dec ? 0xffff : 1;
            uint16_t hl_ = hl(), bc_ = bc(), de_ = de();
            switch (k) {
            case 0: {   // LDI LDD LDIR LDDR
                uint8_t v = rd(hl_);
                wr(de_, v);
                hl_ += step; de_ += step; bc_--;
                uint8_t n = (uint8_t)(v + z.a);
                z.f = (uint8_t)((z.f & (FS | FZ | FC)) | (bc_ ? FP : 0) | (n & FX) | ((n << 4) & FY));
                set_hl(hl_); z.d = (uint8_t)(de_ >> 8); z.e = (uint8_t)de_; z.b = (uint8_t)(bc_ >> 8); z.c = (uint8_t)bc_;
                cycles += 16;
                if (rep && bc_) { z.pc -= 2; cycles += 5; }
                return;
            }
            case 1: {   // CPI CPD CPIR CPDR
                uint8_t v = rd(hl_);
                uint8_t r = (uint8_t)(z.a - v);
                bool hf = ((z.a & 0xf) - (v & 0xf)) < 0;
                hl_ += step; bc_--;
                uint8_t n = (uint8_t)(r - (hf ? 1 : 0));
                z.f = (uint8_t)((z.f & FC) | FN | (r & FS) | (r == 0 ? FZ : 0) | (hf ? FH : 0) | (bc_ ? FP : 0) |
                                (n & FX) | ((n << 4) & FY));
                set_hl(hl_); z.b = (uint8_t)(bc_ >> 8); z.c = (uint8_t)bc_;
                cycles += 16;
                if (rep && bc_ && r) { z.pc -= 2; cycles += 5; }
                return;
            }
            case 2: {   // INI IND INIR INDR
                uint8_t v = z.in(bc());
                wr(hl_, v);
                hl_ += step; z.b--;
                z.f = (uint8_t)(T.sz53[z.b] | FN);
                set_hl(hl_);
                cycles += 16;
                if (rep && z.b) { z.pc -= 2; cycles += 5; }
                return;
            }
            default: {  // OUTI OUTD OTIR OTDR
                uint8_t v = rd(hl_);
                z.b--;
                z.out(bc(), v);
                hl_ += step;
                z.f = (uint8_t)(T.sz53[z.b] | FN);
                set_hl(hl_);
                cycles += 16;
                if (rep && z.b) { z.pc -= 2; cycles += 5; }
                return;
            }
            }
        }
        cycles += 8;   // undefined ED opcodes act as NOPs
    }

    // ---- one instruction ----
    void exec(uint8_t op) {
        int x = op >> 6, y = (op >> 3) & 7, k = op & 7, p = y >> 1, q = y & 1;
        z.r = (uint8_t)((z.r & 0x80) | ((z.r + 1) & 0x7f));
        if (op == 0xdd || op == 0xfd) {   // prefix: the next opcode uses IX/IY (a later prefix overrides)
            idx = op == 0xdd ? 1 : 2;
            cycles += 4;
            exec(fetch());
            return;
        }
        if (op == 0xcb) { cb(); if (!idx) {} return; }
        if (op == 0xed) { ed(); return; }
        cycles += cyc_main[op];
        // instructions using (HL) with an index register read the displacement first and cost more
        bool uses_m = (x == 1 && (k == 6 || y == 6) && op != 0x76) || (x == 2 && k == 6) ||
                      (x == 0 && (k == 4 || k == 5 || k == 6) && y == 6);
        if (idx && uses_m) {
            fetch_disp();
            cycles += (x == 0 && k == 6) ? 8 : (x == 0) ? 15 : 12;   // LD (IX+d),n 19; INC/DEC (IX+d) 23; others 19
        }
        switch (x) {
        case 0:
            switch (k) {
            case 0:
                switch (y) {
                case 0: return;                                                         // NOP
                case 1: { uint16_t t = af(); z.a = (uint8_t)(z.af_ >> 8); z.f = (uint8_t)z.af_; z.af_ = t; return; }
                case 2: { int8_t dd = (int8_t)fetch(); if (--z.b) { z.pc = (uint16_t)(z.pc + dd); cycles += 5; } return; }
                case 3: { int8_t dd = (int8_t)fetch(); z.pc = (uint16_t)(z.pc + dd); return; }
                default: { int8_t dd = (int8_t)fetch(); if (cond(y - 4)) { z.pc = (uint16_t)(z.pc + dd); cycles += 5; } return; }
                }
            case 1: if (q) add16(rp(p)); else set_rp(p, fetch16()); return;
            case 2:
                switch (y) {
                case 0: wr(bc(), z.a); return;
                case 1: z.a = rd(bc()); return;
                case 2: wr(de(), z.a); return;
                case 3: z.a = rd(de()); return;
                case 4: wr16(fetch16(), hl()); return;
                case 5: set_hl(rd16(fetch16())); return;
                case 6: wr(fetch16(), z.a); return;
                default: z.a = rd(fetch16()); return;
                }
            case 3: set_rp(p, (uint16_t)(rp(p) + (q ? -1 : 1))); return;
            case 4: { uint8_t v = inc8(reg(y)); set_reg(y, v); return; }
            case 5: { uint8_t v = dec8(reg(y)); set_reg(y, v); return; }
            case 6: { uint8_t n = fetch(); set_reg(y, n); return; }
            default:
                switch (y) {
                case 0: { uint8_t cy = z.a >> 7; z.a = (uint8_t)((z.a << 1) | cy); z.f = (uint8_t)((z.f & (FS | FZ | FP)) | (z.a & (FX | FY)) | cy); return; }
                case 1: { uint8_t cy = z.a & 1; z.a = (uint8_t)((z.a >> 1) | (cy << 7)); z.f = (uint8_t)((z.f & (FS | FZ | FP)) | (z.a & (FX | FY)) | cy); return; }
                case 2: { uint8_t cy = z.a >> 7; z.a = (uint8_t)((z.a << 1) | (z.f & FC)); z.f = (uint8_t)((z.f & (FS | FZ | FP)) | (z.a & (FX | FY)) | cy); return; }
                case 3: { uint8_t cy = z.a & 1; z.a = (uint8_t)((z.a >> 1) | ((z.f & FC) << 7)); z.f = (uint8_t)((z.f & (FS | FZ | FP)) | (z.a & (FX | FY)) | cy); return; }
                case 4: daa(); return;
                case 5: z.a = (uint8_t)~z.a; z.f = (uint8_t)((z.f & (FS | FZ | FP | FC)) | FH | FN | (z.a & (FX | FY))); return;
                case 6: z.f = (uint8_t)((z.f & (FS | FZ | FP)) | (z.a & (FX | FY)) | FC); return;
                default: z.f = (uint8_t)(((z.f & (FS | FZ | FP | FC)) | ((z.f & FC) ? FH : 0) | (z.a & (FX | FY))) ^ FC); return;
                }
            }
        case 1:
            if (op == 0x76) { z.halted = true; z.pc--; return; }   // HALT: stays on this instruction
            // LD r,r' (with (IX+d), the other operand is the plain H/L)
            if (y == 6) wr(mem_hl(), reg(k, true));
            else if (k == 6) set_reg(y, rd(mem_hl()), true);
            else set_reg(y, reg(k));
            return;
        case 2:
            alu(y, k == 6 ? rd(mem_hl()) : reg(k));
            return;
        default:
            switch (k) {
            case 0: if (cond(y)) { z.pc = pop(); cycles += 6; } return;
            case 1:
                if (!q) { set_rp2(p, pop()); return; }
                switch (p) {
                case 0: z.pc = pop(); return;
                case 1: {   // EXX
                    uint16_t t;
                    t = bc(); z.b = (uint8_t)(z.bc_ >> 8); z.c = (uint8_t)z.bc_; z.bc_ = t;
                    t = de(); z.d = (uint8_t)(z.de_ >> 8); z.e = (uint8_t)z.de_; z.de_ = t;
                    t = (uint16_t)((z.h << 8) | z.l); z.h = (uint8_t)(z.hl_ >> 8); z.l = (uint8_t)z.hl_; z.hl_ = t;
                    return;
                }
                case 2: z.pc = hl(); return;
                default: z.sp = hl(); return;
                }
            case 2: { uint16_t nn = fetch16(); if (cond(y)) z.pc = nn; return; }
            case 3:
                switch (y) {
                case 0: z.pc = fetch16(); return;
                case 2: { uint8_t n = fetch(); z.out((uint16_t)((z.a << 8) | n), z.a); return; }
                case 3: { uint8_t n = fetch(); z.a = z.in((uint16_t)((z.a << 8) | n)); return; }
                case 4: { uint16_t v = rd16(z.sp); wr16(z.sp, hl()); set_hl(v); return; }
                case 5: { uint16_t t = de(); z.d = z.h; z.e = z.l; z.h = (uint8_t)(t >> 8); z.l = (uint8_t)t; return; }   // EX DE,HL (never IX)
                case 6: z.iff1 = z.iff2 = false; return;
                default: z.iff1 = z.iff2 = true; z.ei_delay = 1; return;
                }
            case 4: { uint16_t nn = fetch16(); if (cond(y)) { push(z.pc); z.pc = nn; cycles += 7; } return; }
            case 5:
                if (!q) { push(rp2(p)); return; }
                { uint16_t nn = fetch16(); push(z.pc); z.pc = nn; return; }   // CALL nn (p = 0)
            case 6: alu(y, fetch()); return;
            default: push(z.pc); z.pc = (uint16_t)(y * 8); return;   // RST
            }
        }
    }
};
}

int Z80::step() {
    if (nmi_pending) {
        nmi_pending = false;
        if (halted) { halted = false; pc++; }
        iff2 = iff1; iff1 = false;
        sp -= 2; write(sp, (uint8_t)pc); write((uint16_t)(sp + 1), (uint8_t)(pc >> 8));
        pc = 0x66;
        return 11;
    }
    if (irq_line && iff1 && !ei_delay) {
        if (halted) { halted = false; pc++; }
        iff1 = iff2 = false;
        sp -= 2; write(sp, (uint8_t)pc); write((uint16_t)(sp + 1), (uint8_t)(pc >> 8));
        if (im == 2) {   // vector from I and the bus (0xff here)
            uint16_t v = (uint16_t)((i << 8) | 0xff);
            pc = (uint16_t)(read(v) | (read((uint16_t)(v + 1)) << 8));
            return 19;
        }
        pc = 0x38;       // IM 1 (IM 0 with 0xff on the bus = RST 38h)
        return 13;
    }
    if (ei_delay) ei_delay = 0;
    if (halted) return 4;
    Exec ex(*this);
    ex.exec(read(pc++));
    return ex.cycles;
}

int Z80::run(int cycles) {
    int done = 0;
    while (done < cycles) done += step();
    return done;
}
