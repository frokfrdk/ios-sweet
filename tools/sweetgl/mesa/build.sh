#!/bin/bash
# Stage 7.0: configure and build Mesa's freedreno gallium driver for iOS arm64
# (iPhoneOS SDK, Xcode's clang) against sweetdrm, our libdrm subset.
# Usage: build.sh <patched mesa source> <build dir>
# Needs meson >= 1.4, ninja, python3 with mako/pyyaml/packaging, bison >= 3, flex.
set -euo pipefail

SRC=$(cd "$1" && pwd)
mkdir -p "$2"
OUT=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
SWEETDRM_INC=$(cd "$HERE/../sweetdrm/include" && pwd)

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
CXX=$(xcrun --sdk iphoneos -f clang++)
AR=$(xcrun --sdk iphoneos -f ar)
STRIP=$(xcrun --sdk iphoneos -f strip)
FLAGS="'-arch', 'arm64', '-isysroot', '$SDK', '-mios-version-min=16.0'"

cat > "$OUT/ios-arm64.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
objc = '$CC'
objcpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[built-in options]
c_args = [$FLAGS, '-DSWEETDRM', '-I$SWEETDRM_INC']
cpp_args = [$FLAGS, '-DSWEETDRM', '-I$SWEETDRM_INC']
objc_args = [$FLAGS]
objcpp_args = [$FLAGS]
c_link_args = [$FLAGS]
cpp_link_args = [$FLAGS]
objc_link_args = [$FLAGS]
objcpp_link_args = [$FLAGS]
default_library = 'static'
EOF

meson setup "$OUT/build" "$SRC" --cross-file "$OUT/ios-arm64.ini" --buildtype=release \
  -Dgallium-drivers=freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dplatforms= \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
  -Dllvm=disabled -Dshader-cache=disabled -Dzlib=disabled -Dzstd=disabled -Dexpat=disabled \
  -Dxmlconfig=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dperfetto=false -Dbuild-tests=false

# No target links the driver (test/build-tri.sh does, 7.2; frontend/build.sh, 7.3), so build its
# static libraries by name. For GL (7.3): glapi is the dispatch (current context and table) libmesa
# calls, glcpp the GLSL preprocessor libglsl calls, and vtn the SPIR-V to NIR translator behind
# glSpecializeShader / ARB_gl_spirv in libmesa.
meson compile -C "$OUT/build" \
  freedreno freedreno_drm freedrenowinsys freedreno_common freedreno_ir3 freedreno_layout \
  freedreno_perfcntrs ir3encode ir3decode gallium mesa compiler glsl nir mesa_util mesa_util_simd xmlconfig \
  mesa_util_c11 blake3 isaspec freedreno_ir2 glapi glcpp vtn
