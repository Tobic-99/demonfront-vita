# Demon Front for the PS Vita

A static recompilation of Demon Front (IGS, PGM arcade board) to native ARM code. The game's two programs, the
68000 main CPU and the IGS027A ARM7 coprocessor (Thumb), are translated to C++ and linked with a native runtime
that replaces the board: memory maps, the 68000 ↔ ARM shared RAM and latches, the IGS023 video chip (software
renderer), and the Z80 + ICS2115 sound. There is no emulator, interpreter or JIT on the hot paths.

The repository contains no game code or data. You need your own copy of the MAME ROM set `dmnfrnt` (Demon Front
V105) and the PGM BIOS `pgm`.

This port is intended only for owners of the game. Do not ask how to get the ROMs: piracy is not supported.

The VPK holds the translated code and nothing of the ROMs: graphics, sound and the programs' data are read from
your ROM files.

How the port runs on the PS Vita: [docs/optimizations.md](docs/optimizations.md).

## Installing on the PS Vita

You need a PS Vita with HENkaku/Ensō and [VitaShell](https://github.com/TheOfficialFloW/VitaShell/releases).
No plugins and no `libshacccg.suprx` are needed.

1. Install `demonfront.vpk` from the [releases](../../releases) with VitaShell.
2. Copy your ROM files to `ux0:data/demonfront/rom/`:
   ```
   pgm_p02s.u20  pgm_t01s.rom  pgm_m01s.rom                     (PGM BIOS)
   v105_16m.u5   v105_32m.u26  t04501.u29   w04501.u5
   a04501.u3     a04502.u4     a04503.u6    b04501.u9   b04502.u11   (Demon Front)
   ```
   The MAME zips `dmnfrnt.zip` and `pgm.zip`, copied unchanged to `ux0:data/demonfront/`, work as well.
3. Start Demon Front.

The game runs in free play (START starts a game). The board's battery-backed RAM (settings, high scores) is
saved to `ux0:data/demonfront/sram.bin`. [Controls](docs/vita-controls.md).

## Building

Tested on x86-64 Linux. Every target needs Python 3 with [Capstone](https://www.capstone-engine.org)
(`pip install capstone`), CMake 3.16+, Ninja and Git.

- PS Vita: [VitaSDK](https://vitasdk.org) (`VITASDK`) with the vitaGL, vitaShaRK, SceShaccCgExt, math-neon and
  zlib packages (`vdpm`).
- Linux: clang, zlib.

### 1. Extract the ROMs

```sh
python3 tools/extract_roms.py /path/to/dmnfrnt.zip /path/to/pgm.zip     # or folders with the files
```

This writes `rom/` (the 68000 programs byte-swapped, the ARM program decrypted). `rom/` is not part of the
repository.

### 2. Translate

```sh
scripts/translate.sh
```

ARM (Thumb) code → `gen/arm/` (`tools/arm_discover.py`, `tools/thumbrec.py`), 68000 code → `gen/m68k/`
(`tools/m68k_discover.py`, `tools/m68krec.py`). `gen/` is not part of the repository.

### 3. Build

PS Vita (`build/vita/demonfront.vpk`):

```sh
scripts/build-vita.sh
```

LiveArea images are optional: `vita/sce_sys/icon0.png` (128x128), `vita/sce_sys/livearea/contents/bg.png`
(840x500) and `startup.png` (280x158) are packed when present; the PNGs must be 8-bit indexed.

Linux (`build/desktop/df_desktop`):

```sh
scripts/build-desktop.sh
DF_BASE=/folder/with/the/zips build/desktop/df_desktop 3000 300     # 3000 frames, a PNG every 300 in build/shots/
```

## Credits

Licences of the linked libraries: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

- [VitaSDK](https://vitasdk.org)
- [vitaGL](https://github.com/Rinnegatamante/vitaGL), [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) — Rinnegatamante (LGPL-3.0)
- [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) — bythos14 (LGPL-2.1)
- [math-neon](https://github.com/Rinnegatamante/math-neon) — Lachlan Tychsen-Smith (MIT)
- [zlib](https://zlib.net) — Jean-loup Gailly, Mark Adler (zlib licence)
- [MAME](https://github.com/mamedev/mame) (BSD-3-Clause): hardware reference for the PGM board, IGS027A, IGS023 and
  ICS2115 (no code taken), except the ARM ROM decryption: its key table (`host/df_key.inc`) and bit pattern
  (`host/romset.cpp`, `tools/decrypt_arm.py`) are from `src/mame/igs/pgmcrypt.cpp` (David Haywood, ElSemi;
  [LICENSE-MAME.txt](LICENSE-MAME.txt))
- [Capstone](https://github.com/capstone-engine/capstone) (BSD-3-Clause): ARM decoding in the translator

## License

The code in this repository is licensed under the GNU General Public License v3.0 (`LICENSE`), copyright
Tobic-99, except the MAME decryption key table and bit pattern (BSD-3-Clause, LICENSE-MAME.txt). Third-party components keep their own
licences. Demon Front is the property of its rights holders; no part of the game is included.
