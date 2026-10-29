#!/bin/bash
# Stage 7.2: build sweetgl-tri for iOS arm64 against the Mesa build from
# mesa/build.sh and libsweetdrm.a.
# Usage: build-tri.sh <mesa source> <mesa build dir> <dir with the static libraries> <output binary>
set -euo pipefail

SRC=$(cd "$1" && pwd)
B=$(cd "$2" && pwd)
L=$(cd "$3" && pwd)
OUT=$4
HERE=$(cd "$(dirname "$0")" && pwd)

# Compile with freedreno_screen.c's command from meson
bash "$HERE/../mesa/mesa-cc.sh" "$SRC" "$B" drivers/freedreno/freedreno_screen.c > "$B/sweetgl-cc.sh"
cat "$B/sweetgl-cc.sh"
OBJS=()
for f in "$HERE/sweetgl-tri.c" "$HERE/../frontend/renderonly_stub.c"; do
  o="$B/$(basename "$f" .c).o"
  bash "$B/sweetgl-cc.sh" "$f" "$o"
  OBJS+=("$o")
done

# ld64 searches every archive for what is still undefined, so their order does not matter.
# Mesa's -Dzlib=disabled still leaves freedreno_rd_output's gz* calls: the SDK's libz has them.
CXX=$(xcrun --sdk iphoneos -f clang++)
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
"$CXX" -arch arm64 -isysroot "$SDK" -mios-version-min=16.0 "${OBJS[@]}" "$L"/*.a \
  -framework IOKit -framework CoreFoundation -lz -o "$OUT"
codesign -s - -f "$OUT"
codesign -dv "$OUT"
