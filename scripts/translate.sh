#!/bin/sh
# Translate the game's code: ARM (Thumb) -> gen/arm/, 68000 -> gen/m68k/. Needs rom/ (tools/extract_roms.py).
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
cd "$R"
mkdir -p notes
python3 tools/thumbrec.py
python3 tools/m68krec.py
