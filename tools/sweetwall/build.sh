#!/bin/bash
# Stage 7.7: build libsweetwall.dylib (the layered wallpaper in SpringBoard) for
# iOS arm64, plain arm64 as libsweeteagl.
# Usage: build.sh <out dir>
set -euo pipefail

mkdir -p "$1"
OUT=$(cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
"$CC" -arch arm64 -isysroot "$SDK" -mios-version-min=16.0 -O2 -g -Wall -Wextra -Werror -fno-objc-arc \
  -dynamiclib "$HERE/sweetwall.m" -install_name /usr/lib/libsweetwall.dylib -Wl,-exported_symbols_list,/dev/null \
  -framework Foundation -framework QuartzCore -framework UIKit -lobjc \
  -o "$OUT/libsweetwall.dylib"
codesign -s - -f "$OUT/libsweetwall.dylib"
