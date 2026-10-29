#!/bin/bash
# Stage 7.3: build sweetgl-gltri for iOS arm64 against libsweetgl.a
# (frontend/build.sh), the Mesa build from mesa/build.sh and libsweetdrm.a.
# Usage: build-gltri.sh <mesa source> <mesa build dir> <dir with the static libraries> <output binary>
set -euo pipefail

SRC=$(cd "$1" && pwd)
B=$(cd "$2" && pwd)
L=$(cd "$3" && pwd)
OUT=$4
HERE=$(cd "$(dirname "$0")" && pwd)

# Compile with freedreno_screen.c's command from meson, plus Khronos' GLES
# headers (Mesa's include/) and sweetgl.h
bash "$HERE/../mesa/mesa-cc.sh" "$SRC" "$B" drivers/freedreno/freedreno_screen.c > "$B/sweetgl-gltri-cc.sh"
bash "$B/sweetgl-gltri-cc.sh" "$HERE/sweetgl-gltri.c" "$B/sweetgl-gltri.o" "-I$SRC/include" "-I$HERE/../frontend"

# As in build-tri.sh: archive order does not matter to ld64, and libz is for freedreno_rd_output.
# The GLSL compiler is C++, hence clang++ as the linker driver.
CXX=$(xcrun --sdk iphoneos -f clang++)
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
"$CXX" -arch arm64 -isysroot "$SDK" -mios-version-min=16.0 "$B/sweetgl-gltri.o" "$L"/*.a \
  -framework IOKit -framework CoreFoundation -lz -o "$OUT"
codesign -s - -f "$OUT"
codesign -dv "$OUT"
