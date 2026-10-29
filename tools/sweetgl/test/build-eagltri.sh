#!/bin/bash
# Stage 7.5: build sweetgl-eagltri for iOS arm64 against libsweeteagl.a
# (eagl/build.sh), libsweetgl.a, the Mesa build and libsweetdrm.a.
# Usage: build-eagltri.sh <mesa source> <dir with the static libraries> <output binary>
set -euo pipefail

SRC=$(cd "$1" && pwd)
L=$(cd "$2" && pwd)
OUT=$3
HERE=$(cd "$(dirname "$0")" && pwd)

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
CXX=$(xcrun --sdk iphoneos -f clang++)
"$CC" -arch arm64 -isysroot "$SDK" -mios-version-min=16.0 -O2 -g -Wall -Wextra -fno-objc-arc \
  -I"$SRC/include" -I"$HERE/../sweetdrm/include" -c "$HERE/sweetgl-eagltri.m" -o "$OUT.o"

# Not linked with OpenGLES.framework: the test dlopens it, so every gl* it names is Mesa's
"$CXX" -arch arm64 -isysroot "$SDK" -mios-version-min=16.0 "$OUT.o" "$L"/*.a \
  -framework Foundation -framework IOKit -framework IOSurface -framework CoreFoundation -lobjc -lz -o "$OUT"
rm -f "$OUT.o"
codesign -s - -f "$OUT"
codesign -dv "$OUT"
