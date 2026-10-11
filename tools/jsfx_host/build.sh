#!/usr/bin/env bash
# Builds libfurrow_jsfx_host.so from a pinned ysfx commit.
# Needs git, a C++17 compiler and cmake. Usage: tools/jsfx_host/build.sh [work dir]
set -euo pipefail

YSFX_URL=https://github.com/JoepVanlier/ysfx.git
YSFX_COMMIT=5c3452fee62583aa3d1b7e877d0c758c4024af89
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-$HERE/.build}"
SRC="$WORK/ysfx"

mkdir -p "$WORK"
if [ ! -d "$SRC/.git" ]; then
  git clone --recurse-submodules --shallow-submodules "$YSFX_URL" "$SRC"
fi
git -C "$SRC" fetch --depth 1 origin "$YSFX_COMMIT"
git -C "$SRC" checkout --quiet "$YSFX_COMMIT"
git -C "$SRC" submodule update --init --recursive --depth 1

cmake -S "$SRC" -B "$SRC/build" -DYSFX_GFX=OFF -DYSFX_PLUGIN=OFF -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null
cmake --build "$SRC/build" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null

WDL="$SRC/thirdparty/WDL/source"
c++ -std=c++17 -O2 -fPIC -shared -o "$HERE/libfurrow_jsfx_host.so" "$HERE/host.cpp" \
  -I"$SRC/include" -I"$SRC/sources" -I"$WDL" -I"$SRC/build" \
  -Wl,--whole-archive "$SRC/build/libysfx.a" -Wl,--no-whole-archive -lpthread -ldl
echo "$HERE/libfurrow_jsfx_host.so"
