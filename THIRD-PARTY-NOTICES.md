# Third-party components

Linked into `eboot.bin` or packed in the VPK:

| Component | Licence | Source |
|---|---|---|
| vitaGL (Rinnegatamante) | LGPL-3.0 | https://github.com/Rinnegatamante/vitaGL |
| vitaShaRK (Rinnegatamante) | LGPL-3.0 | https://github.com/Rinnegatamante/vitaShaRK |
| SceShaccCgExt (bythos14) | LGPL-2.1 | https://github.com/bythos14/SceShaccCgExt |
| math-neon (Lachlan Tychsen-Smith) | MIT | https://github.com/Rinnegatamante/math-neon |
| zlib (Jean-loup Gailly, Mark Adler) | zlib licence | https://zlib.net |
| MAME ARM ROM decryption key table and bit pattern (David Haywood, ElSemi) | BSD-3-Clause, LICENSE-MAME.txt | https://github.com/mamedev/mame |
| VitaSDK stubs (vita-headers, vita-toolchain) | MIT | https://github.com/vitasdk |
| newlib (C library) | BSD-style licences (COPYING.NEWLIB) | https://github.com/vitasdk/newlib |
| libstdc++, libgcc | GPL-3.0 with the GCC Runtime Library Exception | https://gcc.gnu.org |

vitaGL, vitaShaRK and SceShaccCgExt are linked statically. The complete source of this port and its build scripts
are in this repository, so the PS Vita build can be rebuilt and relinked with modified versions of those libraries
(`scripts/build-vita.sh`).

`vita/shader_cache/` holds the two vitaGL fixed-function shaders this game uses: compiled from vitaGL's shader
code (LGPL-3.0) with libshacccg, and copied to `ux0:data/shader_cache/v31/` at start. Not distributed: `libshacccg.suprx` (Sony).
