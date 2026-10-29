#!/bin/bash
# Stage 7.3: build libsweetgl.a for iOS arm64: the sweetgl frontend, Mesa's
# public GLES entry points (glClear etc., dispatching through glapi to the
# current context) and the renderonly stub. A program links it with the Mesa
# libraries from mesa/build.sh (libmesa, libglapi, ...) and libsweetdrm.a.
# Usage: build.sh <mesa source> <mesa build dir> <output library>
set -euo pipefail

SRC=$(cd "$1" && pwd)
B=$(cd "$2" && pwd)
OUT=$3
HERE=$(cd "$(dirname "$0")" && pwd)
W="$B/sweetgl"
mkdir -p "$W"

# The GLESv2 library's entry points, as src/mesa/glapi/es2api builds them; meson
# only goes there with EGL or GLX, which we have neither of.
python3 "$SRC/src/mesa/glapi/new/gen_gldispatch_mapi.py" glesv2 "$SRC/src/mesa/glapi/glapi/registry/gl.xml" \
  > "$W/es2_glapi_mapi_tmp.h"

# Compile with st_manager.c's command from meson: the state tracker's include dirs
bash "$HERE/../mesa/mesa-cc.sh" "$SRC" "$B" mesa/state_tracker/st_manager.c > "$W/cc.sh"
cat "$W/cc.sh"
bash "$W/cc.sh" "$HERE/sweetgl.c" "$W/sweetgl.o"
bash "$W/cc.sh" "$HERE/renderonly_stub.c" "$W/renderonly_stub.o"
bash "$W/cc.sh" "$SRC/src/mesa/glapi/es2api/libgles2_public.c" "$W/gles2_public.o" "-I$W"
# The IOSurface import (7.5) needs freedreno's own headers: freedreno_resource.c's command
bash "$HERE/../mesa/mesa-cc.sh" "$SRC" "$B" drivers/freedreno/freedreno_resource.c > "$W/cc-fd.sh"
bash "$W/cc-fd.sh" "$HERE/sweetgl_import.c" "$W/sweetgl_import.o"

AR=$(xcrun --sdk iphoneos -f ar)
rm -f "$OUT"
"$AR" rcs "$OUT" "$W/sweetgl.o" "$W/sweetgl_import.o" "$W/renderonly_stub.o" "$W/gles2_public.o"
