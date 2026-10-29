#!/bin/bash
# Stage 7.2: build sweetgl-tri for iOS arm64 against the Mesa build from
# mesa/build.sh and libsweetdrm.a.
# Usage: build-tri.sh <mesa build dir> <dir with the static libraries> <output binary>
set -euo pipefail

B=$(cd "$1" && pwd)
L=$(cd "$2" && pwd)
OUT=$3
HERE=$(cd "$(dirname "$0")" && pwd)

# Compile with freedreno_screen.c's command from meson (defines, generated
# include dirs, target flags), swapping in our source and object.
python3 - "$B" "$HERE/sweetgl-tri.c" "$B/sweetgl-tri.o" > "$B/sweetgl-tri-cc.sh" <<'EOF'
import json, shlex, sys
b, src, obj = sys.argv[1:]
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
print("cd " + shlex.quote(e["directory"]) + " && " + shlex.join(out + ["-Wno-error", "-c", src, "-o", obj]))
EOF
cat "$B/sweetgl-tri-cc.sh"
bash "$B/sweetgl-tri-cc.sh"

# ld64 searches every archive for what is still undefined, so their order does not matter
CXX=$(xcrun --sdk iphoneos -f clang++)
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
"$CXX" -arch arm64 -isysroot "$SDK" -mios-version-min=16.0 "$B/sweetgl-tri.o" "$L"/*.a \
  -framework IOKit -framework CoreFoundation -o "$OUT"
codesign -s - -f "$OUT"
codesign -dv "$OUT"
