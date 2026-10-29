#!/usr/bin/env python3
"""arm_discover.py: find the code in Demon Front's decrypted ARM ROM (rom/arm_ext.bin, mapped at 0x08000000).

Recursive descent over ARM and Thumb code (capstone), from:
  - the boot entry 0x08000000 (ARM) and the vector table the boot stub reads (0x08017ce4: Thumb pointers),
  - every BL/BLX target, and literal-pool constants that are loaded and then used by BX/BLX/MOV PC,
  - (with --ptrscan) every aligned word in the ROM that points into the ROM with the Thumb bit set and decodes
    as plausible Thumb code: function-pointer tables and C++ vtables.
A function is a call target; its body is everything reachable from it by branches that stay inside it (a branch
to another known function start is a tail call). Writes notes/arm_functions.txt and prints statistics.

    tools/arm_discover.py [--ptrscan] [--gapsweep]
"""
import os, struct, sys, collections
import capstone
from capstone import arm as A

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x08000000
ROM = open(os.path.join(P, 'rom', 'arm_ext.bin'), 'rb').read()
END = BASE + len(ROM)

cs_arm = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
cs_thumb = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
for c in (cs_arm, cs_thumb):
    c.detail = True

def in_rom(a):
    return BASE <= a < END

def rd32(a):
    return struct.unpack_from('<I', ROM, a - BASE)[0]

_cache = {}
def decode(addr, thumb):
    """One instruction at addr (None if invalid)."""
    key = (addr, thumb)
    if key in _cache:
        return _cache[key]
    off = addr - BASE
    md = cs_thumb if thumb else cs_arm
    ins = next(md.disasm(ROM[off:off + 4], addr, 1), None)
    _cache[key] = ins
    return ins

class Func:
    def __init__(s, addr, thumb):
        s.addr, s.thumb = addr, thumb
        s.insns = {}          # addr -> insn
        s.calls = set()       # (target, thumb)
        s.indirect = 0        # unresolved indirect jumps/calls
        s.switches = {}       # add-pc address -> case targets
        s.bl_jumps = set()    # addresses of 'bl' used as a long jump inside this function
        s.bad = False

funcs = {}                    # (addr, thumb) -> Func
pending = collections.deque()
data_words = set()            # literal-pool addresses (not code)

def add_func(addr, thumb, why):
    if not in_rom(addr):
        return
    k = (addr, thumb)
    if k not in funcs:
        funcs[k] = Func(addr, thumb)
        funcs[k].why = why
        pending.append(k)

def literal(ins):
    """Address and value of an ldr rX, [pc, #imm] literal, else None."""
    if ins.id != A.ARM_INS_LDR or len(ins.operands) != 2:
        return None
    m = ins.operands[1]
    if m.type != A.ARM_OP_MEM or m.mem.base != A.ARM_REG_PC or m.mem.index != 0:
        return None
    pc = (ins.address + 4) & ~3 if ins.size == 2 or True else ins.address + 8
    pc = ((ins.address + 4) & ~3) if decode_mode_thumb(ins) else ins.address + 8
    la = pc + m.mem.disp
    if not in_rom(la) or la + 4 > END:
        return None
    return la, rd32(la)

def decode_mode_thumb(ins):
    return ins._cs.mode & capstone.CS_MODE_THUMB != 0 if hasattr(ins, '_cs') else ins.size == 2

def walk(f):
    md_thumb = f.thumb
    work = [f.addr]
    regval = {}
    while work:
        a = work.pop()
        while True:
            if a in f.insns or not in_rom(a):
                break
            ins = decode(a, md_thumb)
            if ins is None:
                f.bad = True
                break
            f.insns[a] = ins
            nxt = a + ins.size
            mn = ins.id
            lit = literal(ins)
            if lit:
                data_words.add(lit[0])
                regval[ins.operands[0].reg] = lit[1]
            if mn in (A.ARM_INS_BL, A.ARM_INS_BLX):
                op = ins.operands[0]
                if op.type == A.ARM_OP_IMM and mn == A.ARM_INS_BL and md_thumb and f.addr < op.imm and (op.imm < a or op.imm in f.insns):
                    # Thumb 'bl' used as a long jump inside a big function (lr is dead there): a branch, not a call
                    f.bl_jumps.add(a)
                    work.append(op.imm)
                    break
                if op.type == A.ARM_OP_IMM:
                    tgt_thumb = md_thumb if mn == A.ARM_INS_BL else not md_thumb
                    f.calls.add((op.imm, tgt_thumb))
                    add_func(op.imm, tgt_thumb, 'bl')
                else:
                    v = regval.get(op.reg)
                    if v is not None and in_rom(v & ~1):
                        add_func(v & ~1, bool(v & 1), 'blx-lit')
                    else:
                        f.indirect += 1
                a = nxt
                continue
            if mn == A.ARM_INS_BX:
                r = ins.operands[0].reg
                if r != A.ARM_REG_LR:
                    v = regval.get(r)
                    if v is not None and in_rom(v & ~1):
                        add_func(v & ~1, bool(v & 1), 'bx-lit')
                        f.calls.add((v & ~1, bool(v & 1)))
                    else:
                        f.indirect += 1
                if ins.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                    break
                a = nxt
                continue
            if mn == A.ARM_INS_B:
                tgt = ins.operands[0].imm
                work.append(tgt)
                if ins.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                    break
                a = nxt
                continue
            if mn in (A.ARM_INS_CBZ, A.ARM_INS_CBNZ):
                work.append(ins.operands[1].imm)
                a = nxt
                continue
            if md_thumb and mn == A.ARM_INS_ADD and len(ins.operands) == 2 and ins.operands[0].reg == A.ARM_REG_PC:
                tg = switch_targets(f, a)
                if tg is None:
                    f.indirect += 1
                else:
                    f.switches[a] = tg
                    work.extend(tg)
                break
            # writes to pc: pop {..pc}, ldm {..pc}, mov pc, ldr pc
            writes_pc = False
            if mn in (A.ARM_INS_POP, A.ARM_INS_LDM, A.ARM_INS_LDMIB, A.ARM_INS_LDMDA, A.ARM_INS_LDMDB):
                writes_pc = any(o.type == A.ARM_OP_REG and o.reg == A.ARM_REG_PC for o in ins.operands)
            elif ins.operands and ins.operands[0].type == A.ARM_OP_REG and ins.operands[0].reg == A.ARM_REG_PC \
                    and mn not in (A.ARM_INS_CMP, A.ARM_INS_CMN, A.ARM_INS_TST, A.ARM_INS_TEQ, A.ARM_INS_STR, A.ARM_INS_PUSH, A.ARM_INS_STM):
                writes_pc = True
                if mn not in (A.ARM_INS_POP,) and not (mn == A.ARM_INS_MOV and len(ins.operands) == 2 and
                                                        ins.operands[1].type == A.ARM_OP_REG and ins.operands[1].reg == A.ARM_REG_LR):
                    f.indirect += 1   # computed jump (switch table) or return through another register
            if writes_pc:
                if ins.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                    break
            if mn in (A.ARM_INS_SVC,):
                pass
            a = nxt

def switch_targets(f, a):
    """Targets of a Thumb 'add pc, r3' switch at a (the compiler's forms), or None.
      A: lsls r3,rX,#1; add pc,r3                         -> N two-byte 'b' slots at a+4
      B: adr r3,T; ldrb r3,[r3,rX]; lsls r3,r3,#1; add pc,r3 -> byte table T[N], target a+4+2*T[i]
      C: adr r3,T; adds r3,r3,rX; ldrh r3,[r3,rX]; add pc,r3 -> halfword table, target a+4+T16[i]
    each guarded by 'cmp rX,#N' + 'bhs default' somewhere in the few instructions before (the compiler may
    schedule other instructions in between)."""
    def at(x):
        return f.insns.get(x) or decode(x, True)
    pc = a + 4
    p1 = at(a - 2)
    if p1 is None:
        return None
    if p1.mnemonic == 'ldrh' and p1.op_str.startswith('r3, [r3, '):
        p2, p3 = at(a - 4), at(a - 6)
        if p2 is None or p3 is None or p2.mnemonic != 'adds' or p3.mnemonic != 'adr':
            return None
        table = ((a - 6 + 4) & ~3) + p3.operands[1].imm
        idx_reg = p1.operands[1].mem.index
        first = a - 6
        form = 'C'
    elif p1.mnemonic != 'lsls':
        return None
    else:
        p2 = at(a - 4)
        if p2 is not None and p2.mnemonic == 'ldrb' and p2.op_str.startswith('r3, [r3, '):
            p3 = at(a - 6)
            if p3 is None or p3.mnemonic != 'adr':
                return None
            table = ((a - 6 + 4) & ~3) + p3.operands[1].imm
            idx_reg = p2.operands[1].mem.index
            first = a - 6
            form = 'B'
        else:
            table = None
            idx_reg = p1.operands[1].reg
            first = a - 2
            form = 'A'
    # find 'bhs' and before it 'cmp idx,#N' among the preceding instructions of this function
    prev = sorted(x for x in f.insns if x < first)[-12:]
    n = None
    seen_bhs = False
    for x in reversed(prev):
        i = f.insns[x]
        if not seen_bhs:
            if i.mnemonic == 'bhs':
                seen_bhs = True
            elif i.mnemonic.startswith('b') or i.mnemonic in ('pop', 'bx'):
                return None
            continue
        if i.mnemonic == 'cmp' and i.operands[0].reg == idx_reg and i.operands[1].type == A.ARM_OP_IMM:
            n = i.operands[1].imm
            break
        if i.operands and i.operands[0].type == A.ARM_OP_REG and i.operands[0].reg == idx_reg and i.mnemonic not in ('cmp', 'tst', 'str', 'strb', 'strh'):
            return None   # the index register is changed after the compare
    if n is None:
        return None
    if form == 'A':
        tg = [pc + 2 * i for i in range(n)]
    elif form == 'B':
        tg = [pc + 2 * ROM[table - BASE + i] for i in range(n)]
        for i in range(0, n, 2):
            data_words.add((table + i) & ~1)
    else:
        tg = [pc + struct.unpack_from('<H', ROM, table - BASE + 2 * i)[0] for i in range(n)]
        for i in range(n):
            data_words.add(table + 2 * i)
    return tg

CODE_END = 0x08018000   # the ROM's code area (the rest is game data)

def ptrscan():
    """Aligned ROM words that point into the ROM's code area with the Thumb bit: candidate function pointers."""
    n = 0
    for off in range(0, len(ROM), 4):
        v = struct.unpack_from('<I', ROM, off)[0]
        if not (v & 1) or not in_rom(v & ~1):
            continue
        a = v & ~1
        ins = [decode(a + 2 * i, True) for i in range(3)]
        if any(i is None for i in ins):
            continue
        # a C function typically starts with push {..., lr}; leaf functions (no push) are accepted when the
        # pointer sits in a table of code pointers (a neighbouring word also points into the code area)
        def code_ptr(o):
            if o < 0 or o + 4 > len(ROM):
                return False
            w = struct.unpack_from('<I', ROM, o)[0]
            return bool(w & 1) and 0x08000000 <= w < CODE_END
        in_table = a < CODE_END and (code_ptr(off - 4) or code_ptr(off + 4))
        if in_table or ins[0].id == A.ARM_INS_PUSH or (ins[0].id == A.ARM_INS_SUB and ins[0].operands and ins[0].operands[0].reg == A.ARM_REG_SP):
            if (a, True) not in funcs:
                add_func(a, True, 'ptr')
                n += 1
    return n

def gapsweep(end=0x08018000):
    """Thumb push {..., lr} prologues in the parts of the code area nothing reached yet (functions only called
    through pointers that the scan did not see, e.g. pointers built at run time)."""
    code = set()
    for f in funcs.values():
        for a, i in f.insns.items():
            code.update(range(a, a + i.size, 2))
    n = 0
    for a in range(BASE, end, 2):
        if a in code or a in data_words or (a - 2) in data_words:
            continue
        i = decode(a, True)
        if i and i.id == A.ARM_INS_PUSH and any(o.reg == A.ARM_REG_LR for o in i.operands) and (a, True) not in funcs:
            add_func(a, True, 'sweep'); n += 1
    return n

def main():
    add_func(0x08000000, False, 'boot')
    vt = 0x08017ce4
    for i in range(8):
        v = rd32(vt + 4 * i)
        if v and in_rom(v & ~1):
            add_func(v & ~1, bool(v & 1), 'vector%d' % i)
    def run():
        while pending:
            walk(funcs[pending.popleft()])
    run()
    if '--ptrscan' in sys.argv:
        print('pointer scan added', ptrscan(), 'candidates')
        run()
    if '--gapsweep' in sys.argv:
        while True:
            n = gapsweep()
            if not n:
                break
            print('gap sweep added', n)
            run()
    code = set()
    for f in funcs.values():
        for a, i in f.insns.items():
            for b in range(a, a + i.size, 2):
                code.add(b)
    thumb = sum(1 for f in funcs.values() if f.thumb)
    ninsn = sum(len(f.insns) for f in funcs.values())
    lo = min(f.addr for f in funcs.values()); hi = max(max(f.insns) for f in funcs.values() if f.insns)
    print('functions: %d (%d Thumb, %d ARM), %d bad' % (len(funcs), thumb, len(funcs) - thumb, sum(f.bad for f in funcs.values())))
    print('instructions: %d, code bytes %d (%.1f KB), range %08x..%08x' % (ninsn, len(code) * 2, len(code) * 2 / 1024, lo, hi))
    print('unresolved indirect jumps/calls: %d in %d functions' % (sum(f.indirect for f in funcs.values()), sum(1 for f in funcs.values() if f.indirect)))
    why = collections.Counter(f.why for f in funcs.values())
    print('found by:', dict(why))
    with open(os.path.join(P, 'notes', 'arm_functions.txt'), 'w') as o:
        for k in sorted(funcs):
            f = funcs[k]
            o.write('%08x %s %5d insns %3d calls ind %d %s%s\n' % (f.addr, 'T' if f.thumb else 'A', len(f.insns), len(f.calls), f.indirect, f.why, ' BAD' if f.bad else ''))

if __name__ == '__main__':
    main()
