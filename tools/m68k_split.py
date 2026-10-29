#!/usr/bin/env python3
"""m68k_split.py: byte-swap the 68000 ROMs (stored as little-endian 16-bit words) to big-endian images:
rom/pgm_p02s.u20 -> rom/m68k_bios.bin (BIOS v2, at 0), rom/v105_16m.u5 -> rom/m68k_game.bin (at 0x100000)."""
import os
P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for src, dst in (('pgm_p02s.u20', 'm68k_bios.bin'), ('v105_16m.u5', 'm68k_game.bin')):
    d = bytearray(open(os.path.join(P, 'rom', src), 'rb').read())
    d[0::2], d[1::2] = d[1::2], d[0::2]
    open(os.path.join(P, 'rom', dst), 'wb').write(d)
    print('wrote rom/%s (%d bytes)' % (dst, len(d)))
