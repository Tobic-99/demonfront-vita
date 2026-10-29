#!/usr/bin/env python3
"""m68krec.py: static recompiler for the PGM BIOS + Demon Front 68000 code -> C++ (gen/m68k/).

Input: the functions m68k_discover.py finds (own decoder, tools/m68k_isa.py). Each becomes
    void m_XXXXXX(m68k::Ctx* c)
with d0-d7/a0-a7 and the X/N/Z/V/C flags in C++ locals, stored back to *c around calls, yields and returns
(STORE/LOAD). Memory goes through m68k::rd*/wr* (runtime/m68k_rt.h).

Control flow:
  - branches inside the function: goto (backward ones count down c->budget -> m68k::yield);
  - bsr/jsr T: push the return address R, c->link = R, call m_T, then continue only if c->pc == R
    (otherwise the guest returned somewhere else, e.g. an exception handler switched context: unwind);
  - rts/rte/rtr: pop, then m68k::ret(): plain return when the address is the entry's c->link, else a jump
    (the BIOS jumps through 'move.l vec,-(a7); rts');
  - GCC switch tables (jmp T(pc,d0.w)) become a C++ switch; other indirect jumps/calls go through
    m68k::dispatch (function table lookup).
Every instruction is translated on its own (flags computed in full; the C++ compiler drops the unused ones).

    tools/m68krec.py           -> gen/m68k/m68k_funcs_NN.cpp, m68k_funcs.h, m68k_dispatch.cpp
    tools/m68krec.py --single  -> also gen/m68k_test/single.cpp: every instruction as its own function, for
                                  the differential test against a reference 68000 core (tests/m68k_diff)
"""
import os, sys, io, contextlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m68k_isa as I
import m68k_discover as D

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(P, 'gen', 'm68k')
FUNCS_PER_FILE = 80
CYCLES_PER_INSN = 8      # rough 68000 average, for the budget at backward branches
# Idle loops: backward branches where the game only waits for an interrupt or the other CPU. When the condition
# holds, the slice ends at once (the scheduler goes on to the next CPU / scanline) instead of spinning.
IDLE = {
    0x101956: 'm68k::rd32(0x80a03cu) == 0',   # main loop: wait for the vblank flag 0x80a058 (no idle callback set)
}

REGS = ['d%d' % i for i in range(8)] + ['a%d' % i for i in range(8)]
COND = ['true', 'false', '(!C && !Z)', '(C || Z)', '!C', 'C', '!Z', 'Z', '!V', 'V', '!N', 'N',
        '(N == V)', '(N != V)', '(!Z && N == V)', '(Z || N != V)']
F = 'X, N, Z, V, C'
F4 = 'N, Z, V, C'


class Unsupported(Exception):
    pass


def h(v):
    return '0x%xu' % (v & 0xFFFFFFFF)


def fname(a):
    return 'm_%06x' % a


class Loc:
    """An operand location after its address (if any) has been computed: read() / write(v) expressions."""
    def __init__(s, kind, reg=None, addr=None, val=None):
        s.kind, s.reg, s.addr, s.val = kind, reg, addr, val


class Gen:
    def __init__(s, f, known, single=False):
        s.f, s.known, s.single = f, known, single
        s.out = []
        s.labels = set()
        s.ntmp = 0

    def emit(s, line):
        s.out.append('    ' + line)

    def tmp(s):
        s.ntmp += 1
        return 't%d_' % s.ntmp

    # ---- operands ----
    def index(s, e):
        r = e.xreg
        return r if e.xlong else 'm68k::sx16(%s)' % r

    def locate(s, e, sz):
        """Emit the address computation of e (with its pre/post increment) and return its Loc."""
        k = e.kind
        if k == 'dn': return Loc('dn', reg='d%d' % e.reg)
        if k == 'an': return Loc('an', reg='a%d' % e.reg)
        if k == 'imm': return Loc('imm', val=e.val)
        if k in ('sr', 'ccr', 'usp'): return Loc(k)
        inc = sz if not (sz == 1 and e.reg == 7) else 2
        if k in ('absw', 'absl', 'pcdisp'):
            return Loc('mem', addr=h(e.val))
        t = s.tmp()
        a = 'a%d' % e.reg
        if k == 'ind': s.emit('uint32_t %s = %s;' % (t, a))
        elif k == 'postinc': s.emit('uint32_t %s = %s; %s += %d;' % (t, a, a, inc))
        elif k == 'predec': s.emit('%s -= %d; uint32_t %s = %s;' % (a, inc, t, a))
        elif k == 'disp': s.emit('uint32_t %s = %s + %s;' % (t, a, h(e.disp)))
        elif k == 'idx': s.emit('uint32_t %s = %s + %s + %s;' % (t, a, h(e.disp), s.index(e)))
        elif k == 'pcidx': s.emit('uint32_t %s = %s + %s;' % (t, h(e.val), s.index(e)))
        else: raise Unsupported('ea ' + k)
        return Loc('mem', addr=t)

    def addr_only(s, e):
        """Effective address of a control operand (lea/pea/jmp/jsr), as an expression."""
        k = e.kind
        if k in ('absw', 'absl', 'pcdisp'): return h(e.val)
        a = 'a%d' % e.reg
        if k == 'ind': return a
        if k == 'disp': return '(%s + %s)' % (a, h(e.disp))
        if k == 'idx': return '(%s + %s + %s)' % (a, h(e.disp), s.index(e))
        if k == 'pcidx': return '(%s + %s)' % (h(e.val), s.index(e))
        raise Unsupported('control ea ' + k)

    def rd(s, l, sz):
        if l.kind == 'dn': return 'm68k::M<%d>(%s)' % (sz, l.reg)
        if l.kind == 'an': return 'm68k::M<%d>(%s)' % (sz, l.reg)
        if l.kind == 'imm': return h(l.val)
        if l.kind == 'mem': return 'm68k::rd%d(%s)' % (sz * 8, l.addr)
        raise Unsupported('read ' + l.kind)

    def wr(s, l, sz, v):
        if l.kind == 'dn': s.emit('%s = m68k::merge<%d>(%s, %s);' % (l.reg, sz, l.reg, v))
        elif l.kind == 'an': s.emit('%s = %s;' % (l.reg, v))
        elif l.kind == 'mem': s.emit('m68k::wr%d(%s, %s);' % (sz * 8, l.addr, v))
        else: raise Unsupported('write ' + l.kind)

    # ---- SR ----
    def sr_expr(s):
        return '(uint32_t)((c->sr_hi << 8) | (X << 4) | (N << 3) | (Z << 2) | (V << 1) | (uint32_t)C)'

    def set_ccr(s, v):
        s.emit('{ uint32_t s_ = %s; X = (s_ >> 4) & 1; N = (s_ >> 3) & 1; Z = (s_ >> 2) & 1; V = (s_ >> 1) & 1; C = s_ & 1; }' % v)

    def set_sr(s, v, nxt):
        s.emit('{ uint32_t q_ = %s; c->sr_hi = (q_ >> 8) & 0xa7;' % v)
        s.set_ccr('q_')
        s.emit('}')
        if s.single:
            return
        s.emit('STORE(); m68k::sr_changed(c, %s); LOAD(); if (c->pc != %s) return;' % (h(nxt), h(nxt)))

    # ---- control flow ----
    def call(s, target_expr, ret, static):
        s.emit('a7 -= 4; m68k::wr32(a7, %s);' % h(ret))
        if s.single:
            s.emit('c->pc = %s; STORE(); return;' % target_expr)
            return
        if static:
            if (static,) not in s.known:
                raise Unsupported('call to unknown %06x' % static)
            s.emit('STORE(); c->link = %s; %s(c); LOAD(); if (c->pc != %s) return;' % (h(ret), fname(static), h(ret)))
        else:
            s.emit('STORE(); c->link = %s; m68k::dispatch(c, %s); LOAD(); if (c->pc != %s) return;' % (h(ret), target_expr, h(ret)))

    def goto(s, tgt, pc):
        if s.single:
            s.emit('c->pc = %s; STORE(); return;' % h(tgt))
            return
        if tgt not in s.f.insns:
            raise Unsupported('branch out of function to %06x' % tgt)
        if tgt <= pc:
            if pc in IDLE:
                s.emit('if (%s) c->budget = 0;   // idle loop' % IDLE[pc])
            n = sum(1 for a in s.f.insns if tgt <= a <= pc)
            s.emit('if ((c->budget -= %d) <= 0) { STORE(); m68k::yield(c, %s); LOAD(); if (c->pc != %s) return; }'
                   % (n * CYCLES_PER_INSN, h(tgt), h(tgt)))
        s.emit('goto L_%06x;' % tgt)
        s.labels.add(tgt)

    def ret(s, expr):
        if s.single:
            s.emit('c->pc = %s; STORE(); return;' % expr)
            return
        s.emit('STORE(); m68k::ret(c, %s, link); return;' % expr)

    # ---- instructions ----
    def insn(s, ins):
        op, sz, ops, pc = ins.op, ins.sz, ins.ops, ins.addr
        nxt = pc + ins.size
        if op == 'nop':
            return
        if op == 'bra':
            s.goto(ins.target, pc); return
        if op == 'bcc':
            s.emit('if (%s) {' % COND[ins.cc]); s.goto(ins.target, pc); s.emit('}')
            if s.single: s.emit('c->pc = %s;' % h(nxt))
            return
        if op == 'dbcc':
            r = 'd%d' % ops[0].reg
            s.emit('if (!%s) { %s = (%s & 0xffff0000u) | ((%s - 1) & 0xffff); if ((%s & 0xffff) != 0xffff) {' % (COND[ins.cc], r, r, r, r))
            s.goto(ins.target, pc)
            s.emit('} }')
            if s.single: s.emit('c->pc = %s;' % h(nxt))
            return
        if op == 'bsr':
            s.call(h(ins.target), nxt, ins.target); return
        if op == 'jsr':
            t = D.static_target(ops[0])
            s.call(h(t) if t is not None else s.addr_only(ops[0]), nxt, t); return
        if op == 'jmp':
            t = D.static_target(ops[0])
            if t is not None:
                s.goto(t, pc); return
            if pc in s.f.switches and not s.single:
                tg = s.f.switches[pc]
                s.emit('switch (m68k::M<2>(%s)) {' % ops[0].xreg)
                seen = {}
                for i, t in enumerate(tg):
                    off = (t - ops[0].val) & 0xFFFF
                    if off not in seen:
                        seen[off] = t
                for off, t in sorted(seen.items()):
                    s.emit('case %s: goto L_%06x;' % (h(off), t))
                    s.labels.add(t)
                s.emit('default: STORE(); m68k::fatal(c, %s, "switch value"); }' % h(pc))
                return
            s.emit('{ uint32_t t_ = %s;' % s.addr_only(ops[0]))
            if s.single:
                s.emit('c->pc = t_; STORE(); return; }')
            else:
                s.emit('STORE(); c->link = link; m68k::dispatch(c, t_); return; }')
            return
        if op == 'rts':
            s.emit('{ uint32_t t_ = m68k::rd32(a7); a7 += 4;'); s.ret('t_'); s.emit('}'); return
        if op == 'rtr':
            s.emit('{ uint32_t r_ = m68k::rd16(a7); uint32_t t_ = m68k::rd32(a7 + 2); a7 += 6;')
            s.set_ccr('r_'); s.ret('t_'); s.emit('}'); return
        if op == 'rte':
            s.emit('{ uint32_t r_ = m68k::rd16(a7); uint32_t t_ = m68k::rd32(a7 + 2); a7 += 6;')
            s.emit('c->sr_hi = (r_ >> 8) & 0xa7;'); s.set_ccr('r_'); s.ret('t_'); s.emit('}'); return
        if op == 'scc':
            l = s.locate(ops[0], 1); s.wr(l, 1, '(%s ? 0xffu : 0u)' % COND[ins.cc]); return
        s.emit('{')
        s.body(ins, op, sz, ops, nxt)
        s.emit('}')

    def body(s, ins, op, sz, ops, nxt):
        if op == 'move':
            src, dst = ops
            if dst.kind == 'sr':
                v = s.rd(s.locate(src, 2), 2); s.set_sr(v, nxt); return
            if dst.kind == 'ccr':
                v = s.rd(s.locate(src, 2), 2); s.set_ccr(v); return
            if src.kind == 'sr':
                l = s.locate(dst, 2); s.wr(l, 2, s.sr_expr()); return
            if src.kind == 'usp':
                s.emit('a%d = c->usp;' % dst.reg); return
            if dst.kind == 'usp':
                s.emit('c->usp = a%d;' % src.reg); return
            v = s.rd(s.locate(src, sz), sz)
            s.emit('uint32_t v_ = %s;' % v)
            l = s.locate(dst, sz)
            s.wr(l, sz, 'v_')
            s.emit('m68k::logic<%d>(v_, %s);' % (sz, F4))
            return
        if op == 'movea':
            v = s.rd(s.locate(ops[0], sz), sz)
            s.emit('a%d = %s;' % (ops[1].reg, ('m68k::sx16(%s)' % v) if sz == 2 else v)); return
        if op == 'moveq':
            s.emit('d%d = %s; m68k::logic<4>(d%d, %s);' % (ops[1].reg, h(ops[0].val), ops[1].reg, F4)); return
        if op == 'lea':
            s.emit('a%d = %s;' % (ops[1].reg, s.addr_only(ops[0]))); return
        if op == 'pea':
            s.emit('uint32_t t_ = %s; a7 -= 4; m68k::wr32(a7, t_);' % s.addr_only(ops[0])); return
        if op == 'clr':
            l = s.locate(ops[0], sz); s.wr(l, sz, '0u'); s.emit('N = false; Z = true; V = C = false;'); return
        if op == 'tst':
            s.emit('m68k::logic<%d>(%s, %s);' % (sz, s.rd(s.locate(ops[0], sz), sz), F4)); return
        if op in ('add', 'sub', 'and', 'or', 'eor', 'cmp', 'addi', 'subi', 'andi', 'ori', 'eori', 'cmpi', 'addq', 'subq'):
            base = op.rstrip('iq') if op not in ('ori', 'eori') else op[:-1]
            if op in ('andi', 'ori', 'eori') and ops[1].kind in ('sr', 'ccr'):
                imm = ops[0].val
                cur = s.sr_expr() if ops[1].kind == 'sr' else '(%s & 0xffu)' % s.sr_expr()
                e = {'and': '&', 'or': '|', 'eor': '^'}[base]
                v = '(%s %s %s)' % (cur, e, h(imm))
                if ops[1].kind == 'sr':
                    s.emit('uint32_t n_ = %s;' % v); s.set_sr('n_', nxt)
                else:
                    s.set_ccr(v)
                return
            src_v = s.rd(s.locate(ops[0], sz), sz)
            s.emit('uint32_t s_ = %s;' % src_v)
            if op in ('addq', 'subq') and ops[1].kind == 'an':
                s.emit('a%d %s= s_;' % (ops[1].reg, '+' if op == 'addq' else '-')); return
            l = s.locate(ops[1], sz)
            d_v = s.rd(l, sz)
            if base == 'cmp':
                s.emit('m68k::cmp<%d>(%s, s_, %s);' % (sz, d_v, F4)); return
            if base in ('add', 'sub'):
                s.emit('uint32_t r_ = m68k::%s<%d>(%s, s_, %s);' % (base, sz, d_v, F))
            else:
                e = {'and': '&', 'or': '|', 'eor': '^'}[base]
                s.emit('uint32_t r_ = m68k::logic<%d>(%s %s s_, %s);' % (sz, d_v, e, F4))
            s.wr(l, sz, 'r_')
            return
        if op in ('adda', 'suba', 'cmpa'):
            v = s.rd(s.locate(ops[0], sz), sz)
            if sz == 2: v = 'm68k::sx16(%s)' % v
            a = 'a%d' % ops[1].reg
            if op == 'cmpa':
                s.emit('m68k::cmp<4>(%s, %s, %s);' % (a, v, F4))
            else:
                s.emit('%s %s= %s;' % (a, '+' if op == 'adda' else '-', v))
            return
        if op in ('addx', 'subx'):
            ls = s.locate(ops[0], sz)
            s.emit('uint32_t s_ = %s;' % s.rd(ls, sz))
            ld = s.locate(ops[1], sz)
            s.emit('uint32_t r_ = m68k::%s<%d>(%s, s_, %s);' % (op, sz, s.rd(ld, sz), F))
            s.wr(ld, sz, 'r_')
            return
        if op == 'cmpm':
            ls = s.locate(ops[0], sz); s.emit('uint32_t s_ = %s;' % s.rd(ls, sz))
            ld = s.locate(ops[1], sz); s.emit('m68k::cmp<%d>(%s, s_, %s);' % (sz, s.rd(ld, sz), F4)); return
        if op in ('neg', 'negx', 'not'):
            l = s.locate(ops[0], sz); v = s.rd(l, sz)
            if op == 'neg': s.emit('uint32_t r_ = m68k::sub<%d>(0, %s, %s);' % (sz, v, F))
            elif op == 'negx': s.emit('uint32_t r_ = m68k::subx<%d>(0, %s, %s);' % (sz, v, F))
            else: s.emit('uint32_t r_ = m68k::logic<%d>(~%s, %s);' % (sz, v, F4))
            s.wr(l, sz, 'r_'); return
        if op == 'ext':
            r = 'd%d' % ops[0].reg
            if sz == 2: s.emit('%s = m68k::merge<2>(%s, m68k::sx8(%s)); m68k::logic<2>(%s, %s);' % (r, r, r, r, F4))
            else: s.emit('%s = m68k::sx16(%s); m68k::logic<4>(%s, %s);' % (r, r, r, F4))
            return
        if op == 'swap':
            r = 'd%d' % ops[0].reg
            s.emit('%s = (%s << 16) | (%s >> 16); m68k::logic<4>(%s, %s);' % (r, r, r, r, F4)); return
        if op == 'exg':
            a, b = repr(ops[0]), repr(ops[1])
            s.emit('uint32_t t_ = %s; %s = %s; %s = t_;' % (a, a, b, b)); return
        if op in ('btst', 'bchg', 'bclr', 'bset'):
            n = h(ops[0].val) if ops[0].kind == 'imm' else 'd%d' % ops[0].reg
            w = 32 if ops[1].kind == 'dn' else 8
            l = s.locate(ops[1], 4 if w == 32 else 1)
            v = s.rd(l, 4 if w == 32 else 1)
            s.emit('uint32_t v_ = %s; uint32_t m_ = 1u << (%s & %d); Z = (v_ & m_) == 0;' % (v, n, w - 1))
            if op != 'btst':
                e = {'bchg': 'v_ ^ m_', 'bclr': 'v_ & ~m_', 'bset': 'v_ | m_'}[op]
                s.wr(l, 4 if w == 32 else 1, e)
            return
        if op in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr'):
            cnt = h(ops[0].val) if ops[0].kind == 'imm' else '(d%d & 63)' % ops[0].reg
            l = s.locate(ops[1], sz)
            fl = F4 if op in ('rol', 'ror') else F
            s.emit('uint32_t r_ = m68k::%s<%d>(%s, %s, %s);' % (op, sz, s.rd(l, sz), cnt, fl))
            s.wr(l, sz, 'r_'); return
        if op in ('mulu', 'muls'):
            v = s.rd(s.locate(ops[0], 2), 2)
            r = 'd%d' % ops[1].reg
            if op == 'mulu': s.emit('%s = (%s & 0xffff) * %s;' % (r, r, v))
            else: s.emit('%s = (uint32_t)((int32_t)(int16_t)%s * (int32_t)(int16_t)%s);' % (r, r, v))
            s.emit('m68k::logic<4>(%s, %s);' % (r, F4)); return
        if op in ('divu', 'divs'):
            v = s.rd(s.locate(ops[0], 2), 2)
            r = 'd%d' % ops[1].reg
            s.emit('uint32_t s_ = %s; if (s_ == 0) { STORE(); m68k::fatal(c, %s, "divide by zero"); }' % (v, h(ins.addr)))
            if op == 'divu':
                s.emit('uint32_t q_ = %s / s_, m_ = %s %% s_; C = false;' % (r, r))
                s.emit('if (q_ > 0xffff) { V = true; N = true; Z = false; } else { %s = (m_ << 16) | q_; V = false; N = (q_ >> 15) & 1; Z = q_ == 0; }' % r)
            else:
                s.emit('int64_t a_ = (int32_t)%s, b_ = (int16_t)s_; int64_t q_ = a_ / b_, m_ = a_ %% b_; C = false;' % r)
                s.emit('if (q_ < -32768 || q_ > 32767) { V = true; N = true; Z = false; } else { %s = ((uint32_t)(m_ & 0xffff) << 16) | (uint32_t)(q_ & 0xffff); V = false; N = (q_ >> 15) & 1; Z = (q_ & 0xffff) == 0; }' % r)
            return
        if op == 'movem':
            s.movem(ins, sz, ops[0]); return
        if op == 'link':
            a = 'a%d' % ops[0].reg
            s.emit('a7 -= 4; m68k::wr32(a7, %s); %s = a7; a7 += %s;' % (a, a, h(ops[1].val))); return
        if op == 'unlk':
            a = 'a%d' % ops[0].reg
            s.emit('a7 = %s; %s = m68k::rd32(a7); a7 += 4;' % (a, a)); return
        raise Unsupported('insn ' + repr(ins))

    def movem(s, ins, sz, e):
        mask = ins.extra
        to_mem = ins.cc == 0
        rd = 'm68k::rd%d' % (sz * 8)
        if to_mem and e.kind == 'predec':
            # mask bit 0 = a7 ... bit 15 = d0; stored from the highest register down
            regs = [REGS[15 - i] for i in range(16) if mask >> i & 1]
            a = 'a%d' % e.reg
            s.emit('uint32_t p_ = %s;' % a)
            for r in regs:
                s.emit('p_ -= %d; m68k::wr%d(p_, %s);' % (sz, sz * 8, r))
            s.emit('%s = p_;' % a)
            return
        regs = [REGS[i] for i in range(16) if mask >> i & 1]
        if e.kind == 'postinc':
            a = 'a%d' % e.reg
            s.emit('uint32_t p_ = %s;' % a)
        else:
            s.emit('uint32_t p_ = %s;' % s.addr_only(e))
        for r in regs:
            if to_mem:
                s.emit('m68k::wr%d(p_, %s); p_ += %d;' % (sz * 8, r, sz))
            else:
                v = '%s(p_)' % rd
                if sz == 2: v = 'm68k::sx16(%s)' % v
                s.emit('%s = %s; p_ += %d;' % (r, v, sz))
        if e.kind == 'postinc':
            s.emit('a%d = p_;' % e.reg)

    def run(s):
        f = s.f
        addrs = sorted(f.insns)
        prev_end = None
        for a in addrs:
            ins = f.insns[a]
            if prev_end is not None and a != prev_end:
                s.emit('STORE(); m68k::fatal(c, %s, "fell off translated code");' % h(prev_end))
            s.out.append('L_%06x:' % a)
            s.emit('// %06x  %r' % (a, ins))
            s.insn(ins)
            prev_end = a + ins.size
            if ins.op in ('bra', 'jmp', 'rts', 'rte', 'rtr'):
                prev_end = None
        if prev_end is not None:
            s.emit('STORE(); m68k::fatal(c, %s, "fell off translated code");' % h(prev_end))
        return s.out


def prologue(name):
    out = ['void %s(m68k::Ctx* c) {' % name]
    out.append('    uint32_t %s;' % ', '.join(REGS))
    out.append('    bool X, N, Z, V, C;')
    out.append('#define LOAD() do { %s X = c->x; N = c->n; Z = c->z; V = c->v; C = c->c; } while (0)'
               % ' '.join('%s = c->%s[%d];' % (r, r[0], int(r[1])) for r in REGS))
    out.append('#define STORE() do { %s c->x = X; c->n = N; c->z = Z; c->v = V; c->c = C; } while (0)'
               % ' '.join('c->%s[%d] = %s;' % (r[0], int(r[1]), r) for r in REGS))
    out.append('    LOAD();')
    return out


def gen_func(f, known):
    g = Gen(f, known)
    body = g.run()
    out = prologue(fname(f.addr))
    out.append('    DF_TRACE_68K(0x%06xu);' % f.addr)
    out.append('    const uint32_t link = c->link; (void)link;')
    out.append('    goto L_%06x;' % f.addr)
    targets = g.labels | {f.addr}
    for ln in body:
        if ln.startswith('L_') and ln.endswith(':') and int(ln[2:8], 16) not in targets:
            continue
        out.append(ln)
    out += ['#undef LOAD', '#undef STORE', '}']
    return out


def gen_single(ins):
    """One instruction as a function: runs it, sets c->pc to the next address (or the branch target)."""
    class F1:
        insns = {ins.addr: ins}
        switches = {}
    g = Gen(F1, set(), single=True)
    g.emit('// %06x  %r' % (ins.addr, ins))
    g.insn(ins)
    if ins.op not in ('bra', 'jmp', 'rts', 'rte', 'rtr', 'bsr', 'jsr', 'bcc', 'dbcc'):
        g.emit('c->pc = %s;' % h(ins.addr + ins.size))
    out = prologue('s_%06x' % ins.addr)
    out += g.out
    out += ['    STORE();', '#undef LOAD', '#undef STORE', '}']
    return out


def main():
    with contextlib.redirect_stdout(io.StringIO()):
        D.run()
    funcs = sorted(D.funcs.values(), key=lambda f: f.addr)
    known = {(f.addr,) for f in funcs}
    os.makedirs(OUT, exist_ok=True)
    ok, failed = [], []
    for f in funcs:
        try:
            if f.bad:
                raise Unsupported('discovery hit undecodable code')
            ok.append((f, gen_func(f, known)))
        except Unsupported as e:
            failed.append((f, str(e)))
    for old in os.listdir(OUT):
        if old.startswith('m68k_funcs_') and old.endswith('.cpp'):
            os.remove(os.path.join(OUT, old))
    nfiles = 0
    for i in range(0, len(ok), FUNCS_PER_FILE):
        with open(os.path.join(OUT, 'm68k_funcs_%02d.cpp' % nfiles), 'w') as o:
            o.write('// generated by tools/m68krec.py from the PGM BIOS + Demon Front 68000 ROM - do not edit\n#include "m68k_funcs.h"\n\n')
            for f, lines in ok[i:i + FUNCS_PER_FILE]:
                o.write('\n'.join(lines) + '\n\n')
        nfiles += 1
    with open(os.path.join(OUT, 'm68k_funcs.h'), 'w') as o:
        o.write('// generated by tools/m68krec.py - do not edit\n#pragma once\n#include "m68k_rt.h"\n\n')
        for f, _ in ok + failed:
            o.write('void %s(m68k::Ctx* c);\n' % fname(f.addr))
    with open(os.path.join(OUT, 'm68k_dispatch.cpp'), 'w') as o:
        o.write('// generated by tools/m68krec.py - do not edit\n#include "m68k_funcs.h"\n\n')
        for f, why in failed:
            o.write('void %s(m68k::Ctx* c) { m68k::fatal(c, 0x%06xu, "not translated: %s"); }\n' % (fname(f.addr), f.addr, why.replace('"', "'")))
        o.write('\nconst m68k::FuncEntry m68k::func_table[] = {\n')
        for f, _ in sorted(ok + failed, key=lambda x: x[0].addr):
            o.write('    {0x%06xu, %s},\n' % (f.addr, fname(f.addr)))
        o.write('};\nconst unsigned m68k::func_count = %d;\n' % (len(ok) + len(failed)))
    print('translated %d functions into %d files; %d not translated' % (len(ok), nfiles, len(failed)))
    from collections import Counter
    for why, n in Counter(w for _, w in failed).most_common(20):
        print('  %4d  %s' % (n, why))
    if '--single' in sys.argv:
        tdir = os.path.join(P, 'gen', 'm68k_test')
        os.makedirs(tdir, exist_ok=True)
        seen = {}
        for f in funcs:
            for a, ins in f.insns.items():
                seen[a] = ins
        n = 0
        with open(os.path.join(tdir, 'single.cpp'), 'w') as o:
            o.write('// generated by tools/m68krec.py --single - do not edit\n#include "m68k_rt.h"\n\n')
            rows = []
            for a in sorted(seen):
                try:
                    lines = gen_single(seen[a])
                except Unsupported:
                    continue
                o.write('\n'.join(lines) + '\n\n')
                rows.append(a)
            o.write('const m68k::FuncEntry single_table[] = {\n')
            for a in rows:
                o.write('    {0x%06xu, s_%06x},\n' % (a, a))
            o.write('};\nconst unsigned single_count = %d;\n' % len(rows))
        print('single-instruction functions: %d' % len(rows))


if __name__ == '__main__':
    main()
