#!/bin/bash
# Stage 7.6: build sweetgl-eagldy, sweetgl-eagltri.m with -DSWEETEAGL_INSERTED,
# for iOS arm64. Linked with OpenGLES.framework, as backboardd is, and with
# neither Mesa nor the shim: those come only from /usr/lib/libsweeteagl.dylib
# through DYLD_INSERT_LIBRARIES (the kernel's sweetenv=).
# Usage: build-eagldy.sh <mesa source> <output binary>
set -euo pipefail

SRC=$(cd "$1" && pwd)
OUT=$2
HERE=$(cd "$(dirname "$0")" && pwd)

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
# Mesa's headers for the GL types and enums only; no gl* function is called directly
"$CC" -arch arm64 -isysroot "$SDK" -mios-version-min=16.0 -O2 -g -Wall -Wextra -fno-objc-arc -DSWEETEAGL_INSERTED \
  -I"$SRC/include" -c "$HERE/sweetgl-eagltri.m" -o "$OUT.o"
"$CC" -arch arm64 -isysroot "$SDK" -mios-version-min=16.0 "$OUT.o" \
  -framework OpenGLES -framework Foundation -framework IOKit -framework IOSurface -framework CoreFoundation -lobjc \
  -o "$OUT"
rm -f "$OUT.o"
codesign -s - -f "$OUT"
codesign -dv "$OUT"
