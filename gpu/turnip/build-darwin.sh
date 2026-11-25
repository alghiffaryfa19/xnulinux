#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# On a Mac: build Mesa's Turnip (freedreno) Vulkan driver for the hosted macOS on Qualcomm Adreno (e.g. Xiaomi Pad 6S Pro /
# SM8550 Adreno 740), talking to the Linux host's /dev/dri/renderD128 (msm DRM driver) over the DRM bridge
# (gpu/xnu-drm: libxnudrm in the driver, kexts/XnuDrm in the hosted kernel, xnudrmd on Linux).
# Output: $OUT/lib/libvulkan_freedreno.dylib (universal arm64 + arm64e: the Metal plugin loads it into system processes
# and into ordinary arm64 ones).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MESA=26.3.1
ARCHS=${ARCHS:-arm64 arm64e}
W=${W:-$HOME/w/turnip-darwin}; OUT=${OUT:-$W/out}; mkdir -p "$W"; cd "$W"
export PATH=$W/venv/bin:/opt/homebrew/opt/bison/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
# Mesa wants Python >= 3.10 (macOS ships 3.9): uv's own when there is uv, else Homebrew's
if ! { [ -x venv/bin/meson ] && venv/bin/python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))'; }; then
  rm -rf venv
  if command -v uv >/dev/null; then uv venv -q -p 3.13 venv && VIRTUAL_ENV=$W/venv uv pip install -q meson ninja mako pyyaml packaging
  else /opt/homebrew/bin/python3 -m venv venv && venv/bin/pip -q install meson ninja mako pyyaml packaging; fi
fi
# Turnip needs glslangValidator for its internal float32/float64/bvh SPIR-V shaders
if ! command -v glslangValidator >/dev/null; then
  if [ -x "$HERE/../xnu-cpu-metal/build/vk-deps/bin/glslangValidator" ]; then
    export PATH="$HERE/../xnu-cpu-metal/build/vk-deps/bin:$PATH"
  elif command -v brew >/dev/null; then
    brew install -q glslang
  fi
fi
if [ -f "$HERE/../xnu-cpu-metal/build/mesa-$MESA.tar.xz" ]; then
  cp -f "$HERE/../xnu-cpu-metal/build/mesa-$MESA.tar.xz" "mesa-$MESA.tar.xz"
fi
[ -f mesa-$MESA.tar.xz ] || curl -sSLo mesa-$MESA.tar.xz https://archive.mesa3d.org/mesa-$MESA.tar.xz
rm -rf mesa && mkdir mesa && tar -C mesa --strip-components=1 -xf mesa-$MESA.tar.xz
for p in "$HERE"/patches/*.patch; do patch -d mesa -p1 --no-backup-if-mismatch -s < "$p"; done
mkdir -p mesa/src/freedreno/vulkan/xnudrm
cp "$HERE"/../xnu-drm/darwin/* "$HERE"/../xnu-drm/xnudrm_proto.h mesa/src/freedreno/vulkan/xnudrm/
for ARCH in $ARCHS; do
  cat > cross-$ARCH.txt <<X
[binaries]
c = ['/usr/bin/clang', '-arch', '$ARCH']
cpp = ['/usr/bin/clang++', '-arch', '$ARCH']
objc = ['/usr/bin/clang', '-arch', '$ARCH']
ar = '/usr/bin/ar'
strip = '/usr/bin/strip'
[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = '$ARCH'
endian = 'little'
X
  meson setup mesa/build-$ARCH mesa --cross-file cross-$ARCH.txt --prefix="$W/out-$ARCH" --buildtype=release \
    -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm -Dgallium-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled \
    -Dgbm=disabled -Dllvm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dvideo-codecs= -Dzstd=disabled \
    -Dxmlconfig=disabled -Dlibunwind=disabled -Dvalgrind=disabled -Dbuild-tests=false >/dev/null || { tail -40 mesa/build-$ARCH/meson-logs/meson-log.txt; exit 1; }
  ninja -C mesa/build-$ARCH install > ninja-$ARCH.log 2>&1 || { grep -E "error|FAILED|Undefined" ninja-$ARCH.log | head -40; exit 1; }
done
mkdir -p "$OUT"/lib
lipo -create $(for a in $ARCHS; do echo "$W/out-$a/lib/libvulkan_freedreno.dylib"; done) -output "$OUT"/lib/libvulkan_freedreno.dylib
lipo -info "$OUT"/lib/libvulkan_freedreno.dylib

