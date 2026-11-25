#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VK_HDR_DIR="${VK_HDR_DIR:-$ROOT/build/vk-deps/vulkan-headers/1.4.363.0}"
BUNDLE_SYSCONFDIR="/Library/GPUBundles/XnuCpuMetal.bundle/Contents/Resources"

export PATH="$ROOT/build/pyvenv/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"

if [ ! -f "$VK_HDR_DIR/share/cmake/VulkanHeaders/VulkanHeadersConfig.cmake" ]; then
  if [ -d "$ROOT/src/Vulkan-Headers" ]; then
    rm -rf "$ROOT/build/vkheaders"
    cmake -G Ninja -S "$ROOT/src/Vulkan-Headers" -B "$ROOT/build/vkheaders" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$VK_HDR_DIR" \
      -DVULKAN_HEADERS_ENABLE_TESTS=OFF
    ninja -C "$ROOT/build/vkheaders" install
  else
    mkdir -p "$VK_HDR_DIR/include"
    ln -sfn "$ROOT/src/mesa/include/vulkan" "$VK_HDR_DIR/include/vulkan"
    ln -sfn "$ROOT/src/mesa/include/vk_video" "$VK_HDR_DIR/include/vk_video"
  fi
fi

build_arch() {
  local ARCH="$1"
  local BUILD_DIR="$ROOT/build/vkloader-${ARCH}"
  local PREFIX="$ROOT/build/vkloader-${ARCH}-install"
  rm -rf "$BUILD_DIR"
  cmake -G Ninja -S "$ROOT/src/Vulkan-Loader" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=/usr/bin/clang \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$VK_HDR_DIR" \
    -DSYSCONFDIR="$BUNDLE_SYSCONFDIR" \
    -DVULKAN_HEADERS_INSTALL_DIR="$VK_HDR_DIR" \
    -DBUILD_TESTS=OFF
  ninja -C "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"
  ninja -C "$BUILD_DIR" install
  install_name_tool -id "@rpath/libvulkan.1.dylib" "$PREFIX/lib/libvulkan.1.dylib"
  codesign -s - --force "$PREFIX/lib/libvulkan.1.dylib" 2>/dev/null || true
  file "$PREFIX/lib/libvulkan.1.dylib"
  otool -L "$PREFIX/lib/libvulkan.1.dylib"
}

build_arch arm64
build_arch arm64e
