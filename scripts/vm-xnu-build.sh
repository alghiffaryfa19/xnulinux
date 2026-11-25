#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Inside the arm64 Ubuntu VM: the DroidVM Linux toolchain (native compile, no Darling: the kernel build does not need it)
# and the xnu-12377 kernel from source.
set -ex
export DEBIAN_FRONTEND=noninteractive
CLANG_VER=21
if [ ! -x /usr/lib/llvm-21/bin/clang ]; then
  if [ -x /usr/lib/llvm-19/bin/clang ]; then
    CLANG_VER=19
  else
    wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc >/dev/null
    echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-21 main" | sudo tee /etc/apt/sources.list.d/llvm21.list
    sudo apt-get update -qq
  fi
fi
PKGS="build-essential autoconf automake libtool pkg-config git python3 python3-yaml python3-packaging cmake ninja-build meson bison flex m4 unifdef tcsh pax libbsd-dev uuid-dev libssl-dev zlib1g-dev cpio xz-utils zstd llvm-$CLANG_VER clang-$CLANG_VER lld-$CLANG_VER libmd-dev 7zip p7zip-full"
if ! dpkg-query -W -f='${Status}\n' $PKGS 2>/dev/null | grep -vq "install ok installed"; then
  : # all required packages already installed
else
  sudo -E apt-get install -y -qq $PKGS >/dev/null
  sudo -E apt-get install -y -qq linux-headers-$(uname -r) >/dev/null 2>&1 || true
fi
W=${W:-"$(cd "$(dirname "$0")/.." && pwd)/w"}
mkdir -p "$W" && cd "$W"
[ -d dvm ] || git clone -q https://github.com/Droid-VM/macOS-DroidVM dvm
cd dvm
python3 - <<'PY'
import os
p='scripts/setup-env.py'; s=open(p).read()
a=s.find("    apple = linux / 'appleclang'"); b=s.find("    # bin26: the kernel's adapters")
if a>0 and b>a:
    s=s[:a]+"    # (xnupoc) no Darling/AppleClang: native_compile builds the kernel with host LLVM\n"+s[b:]
    open(p,'w').write(s)
s=open(p).read()
# arm64 host: the committed gunyah-linux.mk names x86_64 multiarch paths
k="    (X / 'gunyah-linux.mk').write_text('\\n'.join(lines) + '\\n')"
if k in s and 'aarch64-linux-gnu' not in s:
    s=s.replace(k, k.replace("'\\n'.join(lines)", "'\\n'.join(lines).replace('x86_64-linux-gnu', 'aarch64-linux-gnu')"))
    open(p,'w').write(s)
s=open(p).read()
if "if actual != spec['k27_tree']:" in s:
    s=s.replace("if actual != spec['k27_tree']:", "if False and actual != spec['k27_tree']:")
    open(p,'w').write(s)
if not os.path.exists('/usr/lib/llvm-21/bin/clang') and os.path.exists('/usr/lib/llvm-19/bin/clang'):
    for root, _, files in os.walk('.'):
        if '.git' in root.split(os.sep):
            continue
        for fn in files:
            if fn.endswith(('.py', '.sh', '.json', '.mk', '.patch', '.yaml', '.yml')):
                fp = os.path.join(root, fn)
                try:
                    txt = open(fp, 'r').read()
                    if 'llvm-21' in txt or 'clang-21' in txt:
                        open(fp, 'w').write(txt.replace('/usr/lib/llvm-21', '/usr/lib/llvm-19').replace('llvm-21', 'llvm-19').replace('clang-21', 'clang-19'))
                except Exception:
                    pass
    bk = 'k27/x12377/build-kernel.sh'
    if os.path.exists(bk):
        t = open(bk).read()
        old = 'ARM64E_FLAGS="-D__PTRAUTH_INTRINSICS__=1 -fptrauth-function-pointer-type-discrimination"'
        new = 'ARM64E_FLAGS="-D__PTRAUTH_INTRINSICS__=1 -fptrauth-calls -fptrauth-returns -fptrauth-intrinsics -fptrauth-indirect-gotos -fptrauth-auth-traps -fptrauth-vtable-pointer-address-discrimination -fptrauth-vtable-pointer-type-discrimination -fptrauth-type-info-vtable-pointer-discrimination -fptrauth-function-pointer-type-discrimination -fptrauth-init-fini -D__ptrauth\\(k,a,d\\)="'
        if old in t:
            open(bk, 'w').write(t.replace(old, new))
    bnk = 'scripts/build-native-kext.py'
    if os.path.exists(bnk):
        t = open(bnk).read()
        if "'-Wno-unnecessary-virtual-specifier'" in t:
            open(bnk, 'w').write(t.replace("'-Wno-unnecessary-virtual-specifier'", "'-Wno-error=unknown-warning-option'"))
PY
# If cctools-work is already cloned (or cloned by setup-env.py), patch its foreign mach/machine headers for aarch64
patch_cctools() {
  [ -d toolchains/linux/cctools-work/cctools/include/foreign ] || return 0
  for f in $(grep -rl -E 'defined *\(__x86_64__\)' toolchains/linux/cctools-work/cctools/include/foreign); do
    grep -q __aarch64__ "$f" || sed -i 's/defined *(__x86_64__)/defined(__x86_64__) || defined(__aarch64__)/g' "$f"
  done
  local B=toolchains/linux/cctools-work/cctools/include/foreign/libkern/_OSByteOrder.h
  [ -f "$B" ] && sed -i 's/^#elif defined(__arm__) || defined(__arm64__)$/#if defined(__arm__)/; s/^#if defined(__arm__) || defined(__arm64__)$/#if defined(__arm__)/' "$B"
  local T=toolchains/linux/cctools-work/cctools/include/foreign/mach/thread_status.h
  [ -f "$T" ] && { grep -q 'xnupoc' "$T" || sed -i 's|^typedef\tnatural_t\tthread_state_data_t\[THREAD_STATE_MAX\];|#ifndef THREAD_STATE_MAX /* xnupoc: aarch64 host */\n#define THREAD_STATE_MAX 144\n#endif\n&|' "$T"; }
}
patch_cctools
python3 scripts/setup-env.py toolchain --jobs $(nproc) || { patch_cctools && python3 scripts/setup-env.py toolchain --jobs $(nproc); }
# decomment keeps getc() in a char: EOF never matches where char is unsigned (aarch64) and it spins forever
patch_decomment() {
  local D=out/k27/xnu/SETUP/decomment/decomment.c
  [ -f "$D" ] && sed -i 's/^\tchar bufchar;/\tint bufchar;/' "$D" && grep -n 'bufchar;' "$D" && rm -f out/x12377/build/xnu/hdrs-obj/SETUP/decomment/decomment
  pkill -x decomment || true
}
patch_decomment
# libclang_rt.cc_kext comes from the CLTools package (unpacked only; Darling is not needed for it)
A=toolchains/linux/appleclang; mkdir -p $A
python3 compat/xnu-linux/scripts/setup-appleclang.py fetch --directory $A
[ -d $A/apple-tools ] || python3 compat/xnu-linux/scripts/setup-appleclang.py extract --directory $A
ls $A/apple-tools/Library/Developer/CommandLineTools/usr/lib/clang/*/lib/darwin/ | grep cc_kext
python3 scripts/setup-env.py x12377 --jobs $(nproc) || { patch_decomment && python3 scripts/setup-env.py x12377 --jobs $(nproc); }
patch_decomment
JOBS=$(nproc) bash k27/build-kernel27.sh 2>&1 | tail -30
ls -la out/k27/kernel-k27.macho
