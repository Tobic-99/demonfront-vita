#!/usr/bin/env python3
"""decrypt_arm.py: decrypt Demon Front's external ARM ROM (v105_32m.u26, IGS027A) to rom/arm_ext.bin.

The ROM is a list of 16-bit little-endian words. Each word is XORed with:
  - single bits chosen by the word index (IGS's address-line scheme, the variant Demon Front uses), and
  - a per-game byte table, indexed by (word index >> 1) & 0xFF, in the high byte.
The scheme was documented by the MAME project (src/mame/igs/pgmcrypt.cpp, BSD-3-Clause, D. Haywood/ElSemi);
this is our own implementation. The 256-byte key ("dfront_tab", MAME, BSD-3-Clause) is read from host/df_key.inc.

    tools/decrypt_arm.py            (paths relative to the project folder)
"""
import os, re, struct, sys

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def key_table():
    src = open(os.path.join(P, 'host', 'df_key.inc')).read()
    m = re.search(r'kDfrontKey\[256\]\s*=\s*\{(.*?)\};', src, re.S)
    t = [int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', m.group(1))]
    assert len(t) == 256
    return t

# (mask, value, xor_bit, equal): xor when (i & mask) == value if equal, else when != value
BITS = [
    (0x040080, 0x000080, 0x0001, False),
    (0x104008, 0x104008, 0x0002, True),
    (0x080030, 0x080010, 0x0004, True),
    (0x000042, 0x000042, 0x0008, False),
    (0x008100, 0x008000, 0x0010, True),
    (0x002004, 0x000004, 0x0020, False),
    (0x011800, 0x010000, 0x0040, False),
    (0x004820, 0x004820, 0x0080, True),
]

def decrypt(data, tab):
    n = len(data) // 2
    w = list(struct.unpack('<%dH' % n, data))
    for i in range(n):
        x = w[i]
        for mask, val, bit, eq in BITS:
            if ((i & mask) == val) == eq:
                x ^= bit
        x ^= tab[(i >> 1) & 0xFF] << 8
        w[i] = x
    return struct.pack('<%dH' % n, *w)

def main():
    src = os.path.join(P, 'rom', 'v105_32m.u26')
    out = os.path.join(P, 'rom', 'arm_ext.bin')
    data = open(src, 'rb').read()
    assert len(data) == 0x400000, len(data)
    dec = decrypt(data, key_table())
    open(out, 'wb').write(dec)
    first = struct.unpack('<8I', dec[:32])
    print('wrote %s; first words: %s' % (out, ' '.join('%08x' % v for v in first)))

if __name__ == '__main__':
    main()
