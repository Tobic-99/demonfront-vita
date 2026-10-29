#!/usr/bin/env python3
"""m68k_isa.py: our own 68000 instruction decoder (used by m68krec.py and m68k_discover.py).

decode(rd16, addr) -> Insn or None. rd16(a) returns the big-endian word at guest address a.
Every instruction is decoded into an operation name, a size (1, 2, 4 or 0) and a list of operands (Ea).
capstone is only used to cross-check lengths (m68k_discover.py --check): its M68K decoder mis-decodes a few
forms without detail mode.
"""

SZ = {1: 'b', 2: 'w', 4: 'l', 0: ''}
CC = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le']


class Ea:
    """One operand. kind: dn an ind postinc predec disp idx absw absl pcdisp pcidx imm
    reg: register number; disp: displacement; xreg ('d3' / 'a1'), xlong (index size .l); val: immediate or the
    resolved address of absw/absl/pcdisp, or the base of pcidx."""
    __slots__ = ('kind', 'reg', 'disp', 'xreg', 'xlong', 'val')

    def __init__(s, kind, reg=0, disp=0, xreg=None, xlong=False, val=0):
        s.kind, s.reg, s.disp, s.xreg, s.xlong, s.val = kind, reg, disp, xreg, xlong, val

    def __repr__(s):
        k = s.kind
        if k == 'dn': return 'd%d' % s.reg
        if k == 'an': return 'a%d' % s.reg
        if k == 'ind': return '(a%d)' % s.reg
        if k == 'postinc': return '(a%d)+' % s.reg
        if k == 'predec': return '-(a%d)' % s.reg
        if k == 'disp': return '%d(a%d)' % (s.disp, s.reg)
        if k == 'idx': return '%d(a%d,%s.%s)' % (s.disp, s.reg, s.xreg, 'l' if s.xlong else 'w')
        if k in ('absw', 'absl'): return '$%x.%s' % (s.val, k[3])
        if k == 'pcdisp': return '$%x(pc)' % s.val
        if k == 'pcidx': return '$%x(pc,%s.%s)' % (s.val, s.xreg, 'l' if s.xlong else 'w')
        if k == 'imm': return '#$%x' % s.val
        if k == 'sr': return 'sr'
        if k == 'ccr': return 'ccr'
        if k == 'usp': return 'usp'
        return k

    # addressing-mode classes
    def is_mem(s): return s.kind not in ('dn', 'an', 'imm', 'sr', 'ccr', 'usp')


class Insn:
    __slots__ = ('addr', 'size', 'op', 'sz', 'ops', 'cc', 'target', 'extra')

    def __init__(s, addr, op, sz=0, ops=(), cc=None, target=None, extra=None):
        s.addr, s.op, s.sz, s.ops, s.cc, s.target, s.extra = addr, op, sz, list(ops), cc, target, extra
        s.size = 0

    def __repr__(s):
        m = s.op + (('.' + SZ[s.sz]) if s.sz else '')
        if s.cc is not None and s.op in ('bcc', 'dbcc', 'scc'):
            m = s.op[0:-2] + CC[s.cc] + (('.' + SZ[s.sz]) if s.sz and s.op == 'scc' else '')
        o = ', '.join(repr(x) for x in s.ops)
        if s.target is not None:
            o = (o + ', ' if o else '') + '$%x' % s.target
        if s.op == 'movem':
            mask = s.extra
            if s.ops[0].kind == 'predec':   # -(An): bit 0 = a7 ... bit 15 = d0
                mask = int('{:016b}'.format(mask)[::-1], 2)
            o = ('%s, %s' % (regs_str(mask), o)) if s.extra_to_mem() else ('%s, %s' % (o, regs_str(mask)))
        return '%s %s' % (m, o)


def _to_mem(self):
    return self.cc == 0


Insn.extra_to_mem = _to_mem


def regs_str(mask):
    return '/'.join(('d%d' % i) if i < 8 else ('a%d' % (i - 8)) for i in range(16) if mask >> i & 1) or '-'


class Bad(Exception):
    pass


def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


class Reader:
    def __init__(s, rd16, addr):
        s.rd16, s.pc = rd16, addr

    def w(s):
        v = s.rd16(s.pc)
        if v is None:
            raise Bad('outside ROM')
        s.pc += 2
        return v

    def l(s):
        hi = s.w()
        return (hi << 16) | s.w()


def ea(r, mode, reg, sz):
    """Read one effective address (with its extension words) for an operand of size sz."""
    if mode == 0: return Ea('dn', reg)
    if mode == 1: return Ea('an', reg)
    if mode == 2: return Ea('ind', reg)
    if mode == 3: return Ea('postinc', reg)
    if mode == 4: return Ea('predec', reg)
    if mode == 5: return Ea('disp', reg, sext(r.w(), 16))
    if mode == 6:
        x = r.w()
        if x & 0x0700:
            raise Bad('68020 index')
        return Ea('idx', reg, sext(x, 8), ('a' if x & 0x8000 else 'd') + str((x >> 12) & 7), bool(x & 0x800))
    if reg == 0: return Ea('absw', val=sext(r.w(), 16) & 0xFFFFFFFF)
    if reg == 1: return Ea('absl', val=r.l())
    if reg == 2:
        base = r.pc
        return Ea('pcdisp', val=(base + sext(r.w(), 16)) & 0xFFFFFFFF)
    if reg == 3:
        base = r.pc
        x = r.w()
        if x & 0x0700:
            raise Bad('68020 index')
        return Ea('pcidx', val=(base + sext(x, 8)) & 0xFFFFFFFF, xreg=('a' if x & 0x8000 else 'd') + str((x >> 12) & 7), xlong=bool(x & 0x800))
    if reg == 4:
        if sz == 1: return Ea('imm', val=r.w() & 0xFF)
        if sz == 2: return Ea('imm', val=r.w())
        if sz == 4: return Ea('imm', val=r.l())
        raise Bad('imm size')
    raise Bad('ea mode 7/%d' % reg)


SIZE2 = {0: 1, 1: 2, 2: 4}          # the usual 2-bit size field
MOVESIZE = {1: 1, 3: 2, 2: 4}


def decode(rd16, addr):
    r = Reader(rd16, addr)
    try:
        ins = _decode(r, addr)
    except Bad:
        return None
    if ins is not None:
        ins.size = r.pc - addr
    return ins


def _alterable(e, data=True):
    return e.kind not in ('imm', 'pcdisp', 'pcidx') and (not data or e.kind != 'an')


def _decode(r, addr):
    op = r.w()
    line = op >> 12
    m, rg = (op >> 3) & 7, op & 7
    if line == 0:
        if op & 0x0100:
            if m == 1:
                raise Bad('movep')
            k = (op >> 6) & 3
            dst = ea(r, m, rg, 1)
            return Insn(addr, ['btst', 'bchg', 'bclr', 'bset'][k], 4 if m == 0 else 1, [Ea('dn', (op >> 9) & 7), dst])
        hi = (op >> 9) & 7
        if hi == 4:
            k = (op >> 6) & 3
            n = r.w() & 0xFF
            dst = ea(r, m, rg, 1)
            return Insn(addr, ['btst', 'bchg', 'bclr', 'bset'][k], 4 if m == 0 else 1, [Ea('imm', val=n), dst])
        name = {0: 'ori', 1: 'andi', 2: 'subi', 3: 'addi', 5: 'eori', 6: 'cmpi'}.get(hi)
        s = (op >> 6) & 3
        if name is None or s == 3:
            raise Bad('line0')
        sz = SIZE2[s]
        if m == 7 and rg == 4 and name in ('ori', 'andi', 'eori'):
            v = r.w()
            if sz == 1:
                return Insn(addr, name, 1, [Ea('imm', val=v & 0xFF), Ea('ccr')])
            if sz == 2:
                return Insn(addr, name, 2, [Ea('imm', val=v), Ea('sr')])
            raise Bad('to sr size')
        imm = ea(r, 7, 4, sz)
        dst = ea(r, m, rg, sz)
        if dst.kind == 'an':
            raise Bad('imm to an')
        return Insn(addr, name, sz, [imm, dst])
    if line in (1, 2, 3):
        sz = MOVESIZE[line]
        src = ea(r, m, rg, sz)
        dm, dr = (op >> 6) & 7, (op >> 9) & 7
        dst = ea(r, dm, dr, sz)
        if dst.kind == 'an':
            if sz == 1:
                raise Bad('movea.b')
            return Insn(addr, 'movea', sz, [src, dst])
        if not _alterable(dst):
            raise Bad('move dst')
        return Insn(addr, 'move', sz, [src, dst])
    if line == 4:
        if op & 0x01C0 == 0x01C0:
            src = ea(r, m, rg, 4)
            return Insn(addr, 'lea', 4, [src, Ea('an', (op >> 9) & 7)])
        if op & 0x01C0 == 0x0180:
            src = ea(r, m, rg, 2)
            return Insn(addr, 'chk', 2, [src, Ea('dn', (op >> 9) & 7)])
        if op & 0x0100:
            raise Bad('line4 bit8')
        sub = (op >> 8) & 0xF
        s = (op >> 6) & 3
        if sub in (0x0, 0x2, 0x4, 0x6):
            if s == 3:
                if sub == 0x0: return Insn(addr, 'move', 2, [Ea('sr'), ea(r, m, rg, 2)])
                if sub == 0x4: return Insn(addr, 'move', 2, [ea(r, m, rg, 2), Ea('ccr')])
                if sub == 0x6: return Insn(addr, 'move', 2, [ea(r, m, rg, 2), Ea('sr')])
                raise Bad('move from ccr (68010)')
            sz = SIZE2[s]
            return Insn(addr, {0: 'negx', 2: 'clr', 4: 'neg', 6: 'not'}[sub], sz, [ea(r, m, rg, sz)])
        if sub == 0x8:
            if s == 0: return Insn(addr, 'nbcd', 1, [ea(r, m, rg, 1)])
            if s == 1:
                if m == 0: return Insn(addr, 'swap', 4, [Ea('dn', rg)])
                return Insn(addr, 'pea', 4, [ea(r, m, rg, 4)])
            if m == 0: return Insn(addr, 'ext', 2 if s == 2 else 4, [Ea('dn', rg)])
            mask = r.w()
            return Insn(addr, 'movem', 2 if s == 2 else 4, [ea(r, m, rg, 2)], cc=0, extra=mask)
        if sub == 0xA:
            if op == 0x4AFC: return Insn(addr, 'illegal')
            if s == 3: return Insn(addr, 'tas', 1, [ea(r, m, rg, 1)])
            sz = SIZE2[s]
            return Insn(addr, 'tst', sz, [ea(r, m, rg, sz)])
        if sub == 0xC:
            if s in (2, 3):
                mask = r.w()
                return Insn(addr, 'movem', 2 if s == 2 else 4, [ea(r, m, rg, 2)], cc=1, extra=mask)
            raise Bad('line4c')
        if sub == 0xE:
            if s == 1:
                lo = op & 0x3F
                if lo < 0x10: return Insn(addr, 'trap', 0, [Ea('imm', val=lo & 0xF)])
                if lo < 0x18: return Insn(addr, 'link', 0, [Ea('an', rg), Ea('imm', val=sext(r.w(), 16) & 0xFFFFFFFF)])
                if lo < 0x20: return Insn(addr, 'unlk', 0, [Ea('an', rg)])
                if lo < 0x28: return Insn(addr, 'move', 4, [Ea('an', rg), Ea('usp')])
                if lo < 0x30: return Insn(addr, 'move', 4, [Ea('usp'), Ea('an', rg)])
                name = {0x30: 'reset', 0x31: 'nop', 0x32: 'stop', 0x33: 'rte', 0x35: 'rts', 0x36: 'trapv', 0x37: 'rtr'}.get(lo)
                if name is None:
                    raise Bad('line4e')
                if name == 'stop':
                    return Insn(addr, 'stop', 0, [Ea('imm', val=r.w())])
                return Insn(addr, name)
            if s in (2, 3):
                t = ea(r, m, rg, 4)
                if t.kind in ('dn', 'an', 'postinc', 'predec', 'imm'):
                    raise Bad('jmp ea')
                return Insn(addr, 'jsr' if s == 2 else 'jmp', 0, [t])
        raise Bad('line4')
    if line == 5:
        s = (op >> 6) & 3
        if s == 3:
            cc = (op >> 8) & 0xF
            if m == 1:
                d = sext(r.w(), 16)
                return Insn(addr, 'dbcc', 2, [Ea('dn', rg)], cc=cc, target=(addr + 2 + d) & 0xFFFFFFFF)
            return Insn(addr, 'scc', 1, [ea(r, m, rg, 1)], cc=cc)
        sz = SIZE2[s]
        n = (op >> 9) & 7 or 8
        dst = ea(r, m, rg, sz)
        if dst.kind == 'an' and sz == 1:
            raise Bad('addq.b an')
        return Insn(addr, 'subq' if op & 0x100 else 'addq', sz, [Ea('imm', val=n), dst])
    if line == 6:
        cc = (op >> 8) & 0xF
        d = sext(op, 8)
        if d == 0:
            d = sext(r.w(), 16)
            sz = 2
        elif op & 0xFF == 0xFF:
            raise Bad('bcc.l (68020)')
        else:
            sz = 1
        t = (addr + 2 + d) & 0xFFFFFFFF
        if cc == 0: return Insn(addr, 'bra', sz, target=t)
        if cc == 1: return Insn(addr, 'bsr', sz, target=t)
        return Insn(addr, 'bcc', sz, cc=cc, target=t)
    if line == 7:
        if op & 0x100:
            raise Bad('moveq bit8')
        return Insn(addr, 'moveq', 4, [Ea('imm', val=sext(op, 8) & 0xFFFFFFFF), Ea('dn', (op >> 9) & 7)])
    if line in (8, 9, 0xB, 0xC, 0xD):
        dn = (op >> 9) & 7
        opm = (op >> 6) & 7
        if line == 8 and opm in (3, 7):
            return Insn(addr, 'divu' if opm == 3 else 'divs', 2, [ea(r, m, rg, 2), Ea('dn', dn)])
        if line == 0xC and opm in (3, 7):
            return Insn(addr, 'mulu' if opm == 3 else 'muls', 2, [ea(r, m, rg, 2), Ea('dn', dn)])
        if line in (9, 0xB, 0xD) and opm in (3, 7):
            sz = 2 if opm == 3 else 4
            name = {9: 'suba', 0xB: 'cmpa', 0xD: 'adda'}[line]
            return Insn(addr, name, sz, [ea(r, m, rg, sz), Ea('an', dn)])
        sz = SIZE2[opm & 3]
        if opm >= 4 and m in (0, 1):
            if line == 8 and opm == 4:
                return Insn(addr, 'sbcd', 1, [Ea('dn' if m == 0 else 'predec', rg), Ea('dn' if m == 0 else 'predec', dn)])
            if line == 0xC and opm == 4:
                return Insn(addr, 'abcd', 1, [Ea('dn' if m == 0 else 'predec', rg), Ea('dn' if m == 0 else 'predec', dn)])
            if line == 0xC and opm in (5, 6):
                kind = {(5, 0): ('dn', 'dn'), (5, 1): ('an', 'an'), (6, 1): ('dn', 'an')}.get((opm, m))
                if kind is None:
                    raise Bad('exg')
                return Insn(addr, 'exg', 4, [Ea(kind[0], dn), Ea(kind[1], rg)])
            if line in (9, 0xD):
                k = 'dn' if m == 0 else 'predec'
                return Insn(addr, 'subx' if line == 9 else 'addx', sz, [Ea(k, rg), Ea(k, dn)])
            if line == 0xB and m == 1:
                return Insn(addr, 'cmpm', sz, [Ea('postinc', rg), Ea('postinc', dn)])
            if line == 0xB and m == 0:
                return Insn(addr, 'eor', sz, [Ea('dn', dn), Ea('dn', rg)])
            raise Bad('line %x x-form' % line)
        name = {8: 'or', 9: 'sub', 0xB: 'cmp', 0xC: 'and', 0xD: 'add'}[line]
        if line == 0xB and opm >= 4:
            name = 'eor'
        e = ea(r, m, rg, sz)
        if opm < 4:
            if e.kind == 'an' and (sz == 1 or name in ('or', 'and')):
                raise Bad('an byte source')
            return Insn(addr, name, sz, [e, Ea('dn', dn)])
        if not _alterable(e) or e.kind == 'dn' and name != 'eor':
            raise Bad('dst')
        return Insn(addr, name, sz, [Ea('dn', dn), e])
    if line == 0xE:
        s = (op >> 6) & 3
        left = bool(op & 0x100)
        if s == 3:
            if op & 0x0800:
                raise Bad('bitfield')
            t = (op >> 9) & 3
            name = ['as', 'ls', 'rox', 'ro'][t] + ('l' if left else 'r')
            dst = ea(r, m, rg, 2)
            if not dst.is_mem():
                raise Bad('shift mem')
            return Insn(addr, name, 2, [Ea('imm', val=1), dst])
        sz = SIZE2[s]
        t = (op >> 3) & 3
        name = ['as', 'ls', 'rox', 'ro'][t] + ('l' if left else 'r')
        cnt = (op >> 9) & 7
        src = Ea('dn', cnt) if op & 0x20 else Ea('imm', val=cnt or 8)
        return Insn(addr, name, sz, [src, Ea('dn', rg)])
    raise Bad('line %x' % line)   # A-line, F-line
