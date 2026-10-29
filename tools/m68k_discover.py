#!/usr/bin/env python3
"""m68k_discover.py [--check]: find the 68000 code in the PGM BIOS (rom/m68k_bios.bin at 0) and Demon Front's game
ROM (rom/m68k_game.bin at 0x100000), both converted to big-endian by tools/m68k_split.py.

Recursive descent (our decoder, tools/m68k_isa.py) from the BIOS vectors and the game ROM's header (a table of
long pointers at 0x100000, like a vector table: the BIOS calls into the game through it), following
bsr/jsr/bra/Bcc/DBcc and absolute jsr/jmp targets. Writes notes/m68k_functions.txt and prints statistics.
--check: compare every instruction's length and mnemonic with capstone's decoder.
"""
import os, sys, struct, collections
import m68k_isa as I

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIOS = open(os.path.join(P, 'rom', 'm68k_bios.bin'), 'rb').read()
GAME = open(os.path.join(P, 'rom', 'm68k_game.bin'), 'rb').read()

# extra entry points found by hand (indirect targets), address -> why
EXTRA = {}


def region(a):
    if 0 <= a < len(BIOS):
        return BIOS, 0
    if 0x100000 <= a < 0x100000 + len(GAME):
        return GAME, 0x100000
    return None, None


def rd16(a):
    data, base = region(a)
    if data is None or a - base + 2 > len(data):
        return None
    return struct.unpack_from('>H', data, a - base)[0]


def rd32(a):
    data, base = region(a)
    return struct.unpack_from('>I', data, a - base)[0]


def decode(a):
    if a & 1 or region(a)[0] is None:
        return None
    return I.decode(rd16, a)


def static_target(e):
    """Static address of a jsr/jmp operand, or None."""
    if e.kind in ('absw', 'absl', 'pcdisp'):
        return e.val & 0xFFFFFF
    return None


class Func:
    def __init__(s, addr, why):
        s.addr, s.why = addr, why
        s.insns = {}
        s.bad = False
        s.ind = 0
        s.switches = {}      # jmp address -> list of case targets (index order)


funcs = {}
pending = collections.deque()
indirect = collections.Counter()


def add(a, why):
    if region(a)[0] is None or a & 1:
        return
    if a not in funcs:
        funcs[a] = Func(a, why)
        pending.append(a)


def switch_targets(f, ins):
    """GCC's switch: moveq #N,d1; cmp.l d1,d0; bhi default; add.l d0,d0; move.w T(pc,d0.l),d0; jmp T(pc,d0.w).
    Returns the N+1 case targets (T + table word), or None."""
    e = ins.ops[0]
    if e.kind != 'pcidx':
        return None
    prev = sorted(x for x in f.insns if x < ins.addr)[-6:]
    seq = [f.insns[x] for x in prev]
    if len(seq) < 5 or seq[-1].op != 'move' or seq[-1].ops[0].kind != 'pcidx' or seq[-1].ops[0].val != e.val:
        return None
    n = next((x.ops[0].val for x in reversed(seq) if x.op == 'moveq'), None)
    if n is None or n > 256:
        return None
    return [(e.val + I.sext(rd16(e.val + 2 * i), 16)) & 0xFFFFFF for i in range(n + 1)]


def walk(f):
    work = [f.addr]
    while work:
        a = work.pop()
        while True:
            if a in f.insns:
                break
            ins = decode(a)
            if ins is None:
                f.bad = True
                break
            f.insns[a] = ins
            nxt = a + ins.size
            op = ins.op
            if op in ('jsr', 'bsr'):
                t = ins.target if op == 'bsr' else static_target(ins.ops[0])
                if t is None:
                    f.ind += 1; indirect['call'] += 1
                else:
                    add(t, op)
                a = nxt
                continue
            if op in ('rts', 'rte', 'rtr', 'stop', 'illegal', 'reset'):
                break
            if op == 'jmp':
                t = static_target(ins.ops[0])
                sw = switch_targets(f, ins)
                if sw is not None:
                    f.switches[a] = sw
                    work.extend(sw)
                    break
                if t is None:
                    f.ind += 1; indirect['jmp'] += 1
                else:
                    work.append(t)
                break
            if op == 'bra':
                work.append(ins.target)
                break
            if op in ('bcc', 'dbcc'):
                work.append(ins.target)
            a = nxt


def run():
    for i in range(1, 64):
        v = rd32(4 * i)
        if v:
            add(v, 'vector%d' % i)
    for i in range(1, 16):   # the game ROM's own pointer table after its SSP word
        v = rd32(0x100000 + 4 * i)
        if 0x100000 <= v < 0x100000 + len(GAME):
            add(v, 'gamehdr%d' % i)
    for i in range(1, 64):   # the BIOS's default handlers (copied to the RAM vector table at 0x801400)
        v = rd32(0x100 + 4 * i)
        if v:
            add(v, 'biosvec%d' % i)
    for i in range(7):       # the BIOS service table the game calls through (jsr via $238.w ... $250.w)
        add(rd32(0x238 + 4 * i), 'service%d' % i)
    for t in (0xe140, 0xe1e2):   # BIOS function-pointer tables (jsr via table + index)
        for i in range(7):
            add(rd32(t + 4 * i), 'table%x' % t)
    for i in range(16, 64):  # the rest of the game's vector table
        v = rd32(0x100000 + 4 * i)
        if 0x100000 <= v < 0x100000 + len(GAME):
            add(v, 'gamehdr%d' % i)
    add(rd32(0x10023a), 'gamehook')   # the BIOS calls the game through this header pointer (jsr via $10023a)
    for a, why in EXTRA.items():
        add(a, why)
    while pending:
        walk(funcs[pending.popleft()])
    # code pointers passed as operands (pea fn / pea fn(pc) / lea fn / move.l #fn): callbacks the game registers at run time.
    # Only values inside the code area found so far count; repeat until nothing new turns up.
    while True:
        lo_b, hi_b = code_range(0, 0x100000)
        lo_g, hi_g = code_range(0x100000, 0x400000)
        n = 0
        for f in list(funcs.values()):
            for ins in f.insns.values():
                if ins.op not in ('pea', 'lea', 'move', 'movea'):
                    continue
                for e in ins.ops:
                    if e.kind in ('imm', 'absl') and ins.sz == 4 or e.kind in ('absl', 'pcdisp') and ins.op in ('pea', 'lea'):
                        v = e.val & 0xFFFFFFFF
                        if (lo_b <= v < hi_b or lo_g <= v < hi_g) and not v & 1 and v not in funcs and decode(v) is not None:
                            add(v, 'ptr-ref'); n += 1
        if not n:
            break
        while pending:
            walk(funcs[pending.popleft()])


def code_range(lo, hi):
    xs = [a for f in funcs.values() for a in f.insns if lo <= a < hi]
    return (min(xs), max(xs)) if xs else (0, 0)


def check():
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_000)
    md.detail = True
    bad = 0
    for f in funcs.values():
        for a, ins in f.insns.items():
            data, base = region(a)
            c = next(md.disasm(data[a - base:a - base + 10], a, 1), None)
            cm = c.mnemonic.split('.')[0] if c else None
            om = repr(ins).split()[0].split('.')[0]
            if c is None or c.size != ins.size or cm != om:
                bad += 1
                if bad <= 40:
                    print('%06x ours %-30r (%d)  capstone %s %s (%s)' % (a, ins, ins.size, cm, c.op_str if c else '', c.size if c else '-'))
    print('check: %d differences' % bad)


def main():
    run()
    n = sum(len(f.insns) for f in funcs.values())
    nb = sum(len(f.insns) for a, f in funcs.items() if a < 0x100000)
    print('functions: %d, instructions %d (BIOS %d, game %d), bad %d' % (len(funcs), n, nb, n - nb, sum(f.bad for f in funcs.values())))
    print('indirect:', dict(indirect), 'switches %d' % sum(len(f.switches) for f in funcs.values()))
    mn = collections.Counter(i.op for f in funcs.values() for i in f.insns.values())
    print('operations (%d):' % len(mn), ', '.join('%s %d' % kv for kv in mn.most_common()))
    with open(os.path.join(P, 'notes', 'm68k_functions.txt'), 'w') as o:
        for a in sorted(funcs):
            f = funcs[a]
            o.write('%06x %5d insns ind %d %s%s\n' % (a, len(f.insns), f.ind, f.why, ' BAD' if f.bad else ''))
    if '--check' in sys.argv:
        check()


if __name__ == '__main__':
    main()
