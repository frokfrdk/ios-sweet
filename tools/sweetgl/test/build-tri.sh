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

# Compile with freedreno_screen.c's command from meson (defines, generated
# include dirs, target flags) minus its source and outputs, plus the winsys
# and driver dirs for freedreno_drm_public.h and driinfo_freedreno.h.
python3 - "$B" "$SRC" > "$B/sweetgl-cc.sh" <<'EOF'
import json, shlex, sys
b, mesa = sys.argv[1:]
for e in json.load(open(b + "/compile_commands.json")):
    if e["file"].endswith("drivers/freedreno/freedreno_screen.c"):
        args = e["arguments"] if "arguments" in e else shlex.split(e["command"])
        break
else:
    sys.exit("freedreno_screen.c is not in compile_commands.json")
out, skip = [], False
for a in args:
    if skip:
        skip = False
    elif a in ("-o", "-MF", "-MQ", "-MT"):
        skip = True
    elif a not in ("-c", "-MD") and not a.endswith("freedreno_screen.c"):
        out.append(a)
extra = ["-I" + mesa + "/src/gallium/winsys", "-I" + mesa + "/src/gallium/drivers", "-Wno-error"]
print("cd " + shlex.quote(e["directory"]) + " && " + shlex.join(out + extra) + ' -c "$1" -o "$2"')
EOF
cat "$B/sweetgl-cc.sh"
OBJS=()
for f in sweetgl-tri renderonly_stub; do
  bash "$B/sweetgl-cc.sh" "$HERE/$f.c" "$B/$f.o"
  OBJS+=("$B/$f.o")
done

# ld64 searches every archive for what is still undefined, so their order does not matter.
# Mesa's -Dzlib=disabled still leaves freedreno_rd_output's gz* calls: the SDK's libz has them.
CXX=$(xcrun --sdk iphoneos -f clang++)
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
"$CXX" -arch arm64 -isysroot "$SDK" -mios-version-min=16.0 "${OBJS[@]}" "$L"/*.a \
  -framework IOKit -framework CoreFoundation -lz -o "$OUT"
codesign -s - -f "$OUT"
codesign -dv "$OUT"
