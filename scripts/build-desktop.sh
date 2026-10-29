#!/bin/sh
# Linux build: build/desktop/df_desktop. Needs scripts/translate.sh first.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
cmake -S "$R" -B "$R/build/desktop" -G Ninja -DCMAKE_CXX_COMPILER=clang++
ninja -C "$R/build/desktop" ${DF_JOBS:+-j$DF_JOBS}
