#!/usr/bin/env python3
"""thumbrec.py: static recompiler for Demon Front's ARM7 (IGS027A) Thumb code -> C++ (gen/arm/).

Input: the functions arm_discover.py finds in rom/arm_ext.bin. Each game function becomes
    void a_XXXXXXXX(arm::Ctx* c)
with the guest registers and the N/Z/C/V flags held in C++ locals (stored back to c->r[] around calls, returns
and dispatches; the flags are never live across a call in compiled code, so they stay local and the C++
compiler drops every flag computation nothing reads).

Control flow:
  - branches inside the function: goto;
  - bl T: c->r[14] = return address | 1; a_T(c);  then continue after the bl;
  - bx rN / pop {.., pc}: if the target is the return address the function was entered with, return;
    otherwise arm::dispatch(c, target) (a tail call through a veneer, or a non-local return) and return;
  - the compiler's switch tables (add pc, r3; see arm_discover.switch_targets) become a C++ switch;
  - every backward branch counts down c->budget; at zero arm::yield(c) lets the other CPUs run.
Literal-pool loads (ldr rX, [pc, #n]) read the ROM at translation time: the ROM is never written.

    tools/thumbrec.py        -> gen/arm/arm_funcs_NN.cpp, gen/arm/arm_funcs.h, gen/arm/arm_dispatch.cpp
"""
import os, sys, io, contextlib, struct
from capstone import arm as A

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(P, 'tools'))
import arm_discover as D

OUT = os.path.join(P, 'gen', 'arm')
FUNCS_PER_FILE = 60
# ARM-mode C-library routines reached through 'bx pc': done natively in host/arm_native.cpp
NATIVE = {0x08017ca0: 'semihosting (svc 0x123456)'}
# Idle loops: backward branches where the ARM only waits for the 68000; taking them ends the ARM's slice.
IDLE = {0x08000ffa: 'command wait: latch from the 68000 is not the next command'}

REG = {getattr(A, 'ARM_REG_R%d' % i): 'r%d' % i for i in range(13)}
REG.update({A.ARM_REG_SP: 'sp', A.ARM_REG_LR: 'lr', A.ARM_REG_PC: 'pc'})
REGS = ['r%d' % i for i in range(13)] + ['sp', 'lr']

COND = {
    A.ARM_CC_EQ: 'Z', A.ARM_CC_NE: '!Z', A.ARM_CC_HS: 'C', A.ARM_CC_LO: '!C', A.ARM_CC_MI: 'N', A.ARM_CC_PL: '!N',
    A.ARM_CC_VS: 'V', A.ARM_CC_VC: '!V', A.ARM_CC_HI: '(C && !Z)', A.ARM_CC_LS: '(!C || Z)',
    A.ARM_CC_GE: '(N == V)', A.ARM_CC_LT: '(N != V)', A.ARM_CC_GT: '(!Z && N == V)', A.ARM_CC_LE: '(Z || N != V)',
}

class Unsupported(Exception):
    pass

def r(op):
    if op.type != A.ARM_OP_REG:
        raise Unsupported('expected register')
    return REG[op.reg]

class Gen:
    def __init__(s, f, known):
        s.f, s.known = f, known
        s.out = []
        s.labels = set()
        s.used_dispatch = False

    def emit(s, line):
        s.out.append('    ' + line)

    def val(s, op, pc):
        if op.type == A.ARM_OP_REG:
            return '0x%08xu' % pc if op.reg == A.ARM_REG_PC else REG[op.reg]
        if op.type == A.ARM_OP_IMM:
            return '0x%xu' % (op.imm & 0xFFFFFFFF)
        raise Unsupported('operand')

    # flags
    def nz(s, v):
        s.emit('N = (int32_t)(%s) < 0; Z = (%s) == 0;' % (v, v))

    def add_flags(s, d, a, b, carry_in='0'):
        s.emit('{ uint64_t t_ = (uint64_t)%s + %s + %s; uint32_t r_ = (uint32_t)t_; C = (t_ >> 32) != 0; '
               'V = ((~(%s ^ %s) & (%s ^ r_)) >> 31) != 0; %s = r_; N = (int32_t)r_ < 0; Z = r_ == 0; }'
               % (a, b, carry_in, a, b, a, d))

    def sub_flags(s, d, a, b):
        d_ = ('%s = r_; ' % d) if d else ''
        s.emit('{ uint32_t a_ = %s, b_ = %s, r_ = a_ - b_; C = a_ >= b_; V = (((a_ ^ b_) & (a_ ^ r_)) >> 31) != 0; '
               '%sN = (int32_t)r_ < 0; Z = r_ == 0; }' % (a, b, d_))

    def ret_or_dispatch(s, tgt):
        s.emit('if (%s == entry_lr) { STORE(); return; }' % tgt)
        s.emit('STORE(); arm::dispatch(c, %s); return;' % tgt)

    def call(s, tgt, ret):
        s.emit('lr = 0x%08xu; STORE(); %s(c); LOAD();' % (ret | 1, fname(tgt)))

    def branch(s, tgt, pc):
        if tgt in s.f.insns:
            if tgt <= pc:
                if pc in IDLE:
                    s.emit('c->budget = 1;   // idle loop: %s' % IDLE[pc])
                s.emit('if (--c->budget <= 0) { STORE(); arm::yield(c); LOAD(); }')
            s.emit('goto L_%08x;' % tgt)
            s.labels.add(tgt)
        elif (tgt, True) in s.known:   # tail call by branch
            s.emit('STORE(); %s(c); return;' % fname(tgt))
        else:
            raise Unsupported('branch out of function to %08x' % tgt)

    def mem(s, ins, pc):
        m = ins.operands[1].mem
        base = '0x%08xu' % (((pc + 4) & ~3)) if m.base == A.ARM_REG_PC else REG[m.base]
        if m.index:
            return '%s + %s' % (base, REG[m.index])
        return '%s + 0x%xu' % (base, m.disp & 0xFFFFFFFF) if m.disp else base

    def insn(s, ins):
        pc, mn, ops = ins.address, ins.mnemonic, ins.operands
        if mn == 'adr':
            s.emit('%s = 0x%08xu;' % (r(ops[0]), (((pc + 4) & ~3) + ops[1].imm) & 0xFFFFFFFF))
            return
        if mn == 'ldr' and ops[1].type == A.ARM_OP_MEM and ops[1].mem.base == A.ARM_REG_PC:
            la = ((pc + 4) & ~3) + ops[1].mem.disp
            s.emit('%s = 0x%08xu;   // literal at %08x' % (r(ops[0]), D.rd32(la), la))
            return
        if mn in ('ldr', 'ldrh', 'ldrb', 'ldrsh', 'ldrsb'):
            fn = {'ldr': 'arm::rd32(%s)', 'ldrh': 'arm::rd16(%s)', 'ldrb': 'arm::rd8(%s)',
                  'ldrsh': '(uint32_t)(int32_t)(int16_t)arm::rd16(%s)', 'ldrsb': '(uint32_t)(int32_t)(int8_t)arm::rd8(%s)'}[mn]
            s.emit('%s = %s;' % (r(ops[0]), fn % s.mem(ins, pc)))
            return
        if mn in ('str', 'strh', 'strb'):
            w = {'str': 'wr32', 'strh': 'wr16', 'strb': 'wr8'}[mn]
            v = r(ops[0])
            if mn == 'strh': v = '(uint16_t)' + v
            if mn == 'strb': v = '(uint8_t)' + v
            s.emit('arm::%s(%s, %s);' % (w, s.mem(ins, pc), v))
            return
        if mn == 'push':
            regs = [r(o) for o in ops]
            s.emit('sp -= %d;' % (4 * len(regs)))
            for i, x in enumerate(regs):
                s.emit('arm::wr32(sp + %d, %s);' % (4 * i, x))
            return
        if mn == 'pop':
            regs = [o for o in ops]
            pc_pop = any(o.reg == A.ARM_REG_PC for o in regs)
            i = 0
            for o in regs:
                if o.reg == A.ARM_REG_PC:
                    s.emit('{ uint32_t t_ = arm::rd32(sp + %d);' % (4 * i))
                else:
                    s.emit('%s = arm::rd32(sp + %d);' % (REG[o.reg], 4 * i))
                i += 1
            s.emit('sp += %d;' % (4 * i))
            if pc_pop:
                s.emit('  // pop {pc}: Thumb state stays (ARMv4T)')
                s.ret_or_dispatch('(t_ | 1)')
                s.emit('}')
            return
        if mn in ('stm', 'ldm'):
            base = ops[0]
            regs = [REG[o.reg] for o in ops[1:]]
            b = REG[base.reg]
            s.emit('{ uint32_t a_ = %s;' % b)
            for i, x in enumerate(regs):
                if mn == 'stm':
                    s.emit('  arm::wr32(a_ + %d, %s);' % (4 * i, x))
                else:
                    s.emit('  %s = arm::rd32(a_ + %d);' % (x, 4 * i))
            wb = ins.writeback if hasattr(ins, 'writeback') else True
            if mn == 'stm' or b not in regs:
                s.emit('  %s = a_ + %d;' % (b, 4 * len(regs)))
            s.emit('}')
            return
        if mn in ('movs', 'mov'):
            d, v = r(ops[0]), s.val(ops[1], pc)
            if ops[0].reg == A.ARM_REG_PC:
                raise Unsupported('mov pc')
            s.emit('%s = %s;' % (d, v))
            if mn == 'movs':
                s.nz(d)
            return
        if mn == 'mvns':
            d = r(ops[0]); s.emit('%s = ~%s;' % (d, s.val(ops[1], pc))); s.nz(d); return
        if mn in ('adds', 'add', 'subs', 'sub', 'adcs'):
            if len(ops) == 2:
                d, a_, b_ = r(ops[0]), r(ops[0]), s.val(ops[1], pc)
            else:
                d, a_, b_ = r(ops[0]), s.val(ops[1], pc), s.val(ops[2], pc)
            if ops[0].reg == A.ARM_REG_PC:
                raise Unsupported('add pc')
            if a_ == 'pc':
                a_ = '0x%08xu' % (((pc + 4) & ~3))
            if mn == 'add':
                s.emit('%s = %s + %s;' % (d, a_, b_))
            elif mn == 'sub':
                s.emit('%s = %s - %s;' % (d, a_, b_))
            elif mn == 'adds':
                s.add_flags(d, a_, b_)
            elif mn == 'adcs':
                s.add_flags(d, a_, b_, '(uint32_t)C')
            else:
                s.sub_flags(d, a_, b_)
            return
        if mn == 'rsbs':   # rsbs rd, rm, #0 (neg)
            d, m = r(ops[0]), s.val(ops[1], pc)
            s.sub_flags(d, '0u', m)
            return
        if mn == 'cmp':
            s.sub_flags(None, s.val(ops[0], pc), s.val(ops[1], pc)); return
        if mn == 'cmn':
            s.emit('{ uint32_t t__;'); s.add_flags('t__', s.val(ops[0], pc), s.val(ops[1], pc)); s.emit('}'); return
        if mn in ('ands', 'orrs', 'eors', 'bics', 'muls'):
            d = r(ops[0])
            a_, b_ = (d, s.val(ops[1], pc)) if len(ops) == 2 else (s.val(ops[1], pc), s.val(ops[2], pc))
            e = {'ands': '%s & %s', 'orrs': '%s | %s', 'eors': '%s ^ %s', 'bics': '%s & ~%s', 'muls': '%s * %s'}[mn]
            s.emit('%s = %s;' % (d, e % (a_, b_)))
            s.nz(d)
            return
        if mn in ('lsls', 'lsrs', 'asrs'):
            d = r(ops[0])
            if len(ops) == 3:
                src, amt = s.val(ops[1], pc), ops[2]
            else:
                src, amt = d, ops[1]
            if amt.type == A.ARM_OP_IMM:
                n = amt.imm
                if mn == 'lsls':
                    if n == 0:
                        s.emit('%s = %s;' % (d, src))
                    else:
                        s.emit('C = ((%s) >> %d) & 1; %s = %s << %d;' % (src, 32 - n, d, src, n))
                elif mn == 'lsrs':
                    if n == 32:
                        s.emit('C = (%s) >> 31; %s = 0;' % (src, d))
                    else:
                        s.emit('C = ((%s) >> %d) & 1; %s = %s >> %d;' % (src, n - 1, d, src, n))
                else:
                    if n == 32:
                        s.emit('C = (%s) >> 31; %s = (uint32_t)((int32_t)%s >> 31);' % (src, d, src))
                    else:
                        s.emit('C = ((%s) >> %d) & 1; %s = (uint32_t)((int32_t)%s >> %d);' % (src, n - 1, d, src, n))
            else:
                fn = {'lsls': 'lsl', 'lsrs': 'lsr', 'asrs': 'asr'}[mn]
                s.emit('%s = arm::%s_c(%s, %s & 0xff, C);' % (d, fn, src, REG[amt.reg]))
            s.nz(d)
            return
        raise Unsupported('insn %s %s' % (mn, ins.op_str))

    def run(s):
        f = s.f
        addrs = sorted(f.insns)
        # labels: branch targets inside, switch targets
        for a in addrs:
            ins = f.insns[a]
            if ins.mnemonic == 'b' or ins.mnemonic.startswith('b') and ins.id == A.ARM_INS_B:
                t = ins.operands[0].imm
                if t in f.insns:
                    s.labels.add(t)
        for tg in f.switches.values():
            s.labels.update(tg)
        body = []
        s.out = body
        prev_end = None
        for a in addrs:
            ins = f.insns[a]
            if prev_end is not None and a != prev_end:
                s.emit('/* gap: fell off code at %08x */ STORE(); arm::fatal_fell(c, 0x%08xu); return;' % (prev_end, prev_end))
            s.out.append('L_%08x:' % a)
            prev_end = a + ins.size
            s.emit('// %s %s' % (ins.mnemonic, ins.op_str))
            mn = ins.mnemonic
            if ins.id == A.ARM_INS_B:
                tgt = ins.operands[0].imm
                if ins.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                    s.branch(tgt, a)
                    prev_end = None
                else:
                    s.emit('if (%s) {' % COND[ins.cc])
                    s.branch(tgt, a)
                    s.emit('}')
                continue
            if ins.id == A.ARM_INS_BL and a in f.bl_jumps:
                s.emit('lr = 0x%08xu;   // bl used as a long jump inside the function' % ((a + 4) | 1))
                s.branch(ins.operands[0].imm, a)
                prev_end = None
                continue
            if ins.id == A.ARM_INS_BL:
                s.call(ins.operands[0].imm, a + 4)
                continue
            if ins.id == A.ARM_INS_BX:
                rg = ins.operands[0].reg
                if rg == A.ARM_REG_PC:
                    raise Unsupported('bx pc')
                s.ret_or_dispatch(REG[rg])
                prev_end = None
                continue
            if a in f.switches:
                s.emit('switch (r3 >> 1) {')
                for i, t in enumerate(f.switches[a]):
                    # case value: (target - (a + 4)) / 2 == r3 >> 1
                    pass
                cases = {}
                for t in f.switches[a]:
                    cases[(t - (a + 4)) // 2] = t
                for k in sorted(cases):
                    s.emit('case %d: goto L_%08x;' % (k, cases[k]))
                s.emit('default: STORE(); arm::fatal_switch(c, 0x%08xu, r3); return;' % a)
                s.emit('}')
                prev_end = None
                continue
            s.insn(ins)
            if mn == 'pop' and any(o.reg == A.ARM_REG_PC for o in ins.operands):
                prev_end = None
        if prev_end is not None:
            s.emit('STORE(); arm::fatal_fell(c, 0x%08xu); return;' % prev_end)
        return body

def fname(a):
    return 'a_%08x' % a

def gen_func(f, known):
    g = Gen(f, known)
    body = g.run()
    out = ['void %s(arm::Ctx* c) {' % fname(f.addr)]
    out.append('    uint32_t %s;' % ', '.join(REGS))
    out.append('    bool N = false, Z = false, C = false, V = false; (void)N; (void)Z; (void)C; (void)V;')
    out.append('#define LOAD() do { %s } while (0)' % ' '.join('%s = c->r[%d];' % (x, i) for i, x in enumerate(REGS)))
    out.append('#define STORE() do { %s } while (0)' % ' '.join('c->r[%d] = %s;' % (i, x) for i, x in enumerate(REGS)))
    out.append('    DF_TRACE_ARM(0x%08xu);' % f.addr)
    out.append('    LOAD();')
    out.append('    const uint32_t entry_lr = lr;')
    out.append('    goto L_%08x;' % f.addr)
    used = set()
    lines = []
    for ln in body:
        lines.append(ln)
    # drop labels never jumped to (keeps -Wunused-label quiet)
    targets = g.labels | {f.addr}
    for ln in lines:
        if ln.startswith('L_') and ln.endswith(':') and int(ln[2:10], 16) not in targets:
            continue
        out.append(ln)
    out.append('#undef LOAD')
    out.append('#undef STORE')
    out.append('}')
    return out

def main():
    sys.argv = ['arm_discover', '--ptrscan', '--gapsweep']
    with contextlib.redirect_stdout(io.StringIO()):
        D.main()
    funcs = [f for f in D.funcs.values() if f.thumb and not f.bad and f.addr not in NATIVE]
    known = {(f.addr, True) for f in funcs}
    os.makedirs(OUT, exist_ok=True)
    ok, failed = [], []
    for f in sorted(funcs, key=lambda f: f.addr):
        try:
            ok.append((f, gen_func(f, known)))
        except Unsupported as e:
            failed.append((f, str(e)))
    # files
    for old in os.listdir(OUT):
        if old.startswith('arm_funcs_') and old.endswith('.cpp'):
            os.remove(os.path.join(OUT, old))
    nfiles = 0
    for i in range(0, len(ok), FUNCS_PER_FILE):
        with open(os.path.join(OUT, 'arm_funcs_%02d.cpp' % nfiles), 'w') as o:
            o.write('// generated by tools/thumbrec.py from rom/arm_ext.bin - do not edit\n#include "arm_funcs.h"\n\n')
            for f, lines in ok[i:i + FUNCS_PER_FILE]:
                o.write('\n'.join(lines) + '\n\n')
        nfiles += 1
    with open(os.path.join(OUT, 'arm_funcs.h'), 'w') as o:
        o.write('// generated by tools/thumbrec.py - do not edit\n#pragma once\n#include "arm_rt.h"\n\n')
        for f, _ in ok:
            o.write('void %s(arm::Ctx* c);\n' % fname(f.addr))
        for f, _ in failed:
            o.write('void %s(arm::Ctx* c);   // not translated (stub in arm_dispatch.cpp)\n' % fname(f.addr))
        for a, what in sorted(NATIVE.items()):
            o.write('void %s(arm::Ctx* c);   // native: %s (host/arm_native.cpp)\n' % (fname(a), what))
    with open(os.path.join(OUT, 'arm_dispatch.cpp'), 'w') as o:
        o.write('// generated by tools/thumbrec.py - do not edit\n#include "arm_funcs.h"\n\n')
        for f, why in failed:
            o.write('void %s(arm::Ctx* c) { arm::fatal_untranslated(c, 0x%08xu, "%s"); }\n' % (fname(f.addr), f.addr, why.replace('"', "'")))
        o.write('\nconst arm::FuncEntry arm::func_table[] = {\n')
        rows = [(x.addr, fname(x.addr)) for x, _ in ok + failed] + [(a, fname(a)) for a in NATIVE]
        for a, n in sorted(rows):
            o.write('    {0x%08xu, %s},\n' % (a, n))
        o.write('};\nconst unsigned arm::func_count = %d;\n' % (len(ok) + len(failed) + len(NATIVE)))
    print('translated %d functions into %d files; %d not translated' % (len(ok), nfiles, len(failed)))
    from collections import Counter
    for why, n in Counter(w.split(' 0x')[0].split(' to ')[0] for _, w in failed).most_common(20):
        print('  %4d  %s' % (n, why))

if __name__ == '__main__':
    main()
