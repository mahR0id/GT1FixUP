#!/bin/sh
# Build GT1URBFix.kext with the Command Line Tools only (no Xcode).
# Dependencies (override with env vars):
#   MACKERNELSDK  acidanthera/MacKernelSDK checkout
#   LILU_KEXT     Lilu DEBUG release Lilu.kext (provides the plugin SDK)
set -e
B=$(cd "$(dirname "$0")" && pwd)
SDK=${MACKERNELSDK:-$HOME/g15dbg/build/MacKernelSDK}
LILU=${LILU_KEXT:-$HOME/g15dbg/build/lilu_dl/Lilu.kext}/Contents/Resources
OUT=$B/out; mkdir -p "$OUT/obj"
CF="-target x86_64-apple-macos10.15 -mkernel -nostdinc -fno-builtin -fno-common -fno-stack-protector -mno-red-zone -msoft-float -O2 \
 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DPRODUCT_NAME=GT1URBFix -DMODULE_VERSION=3.0.0 \
 -isystem $SDK/Headers -I$LILU -I$B/src"
XF="-std=gnu++17 -fno-rtti -fno-exceptions -fapple-kext -Wno-inconsistent-missing-override"
clang   $CF -c "$B/src/kmod_info.c" -o "$OUT/obj/kmod_info.o"
clang++ $CF $XF -c "$B/src/kern_start.cpp" -o "$OUT/obj/kern_start.o"
clang++ $CF $XF -c "$LILU/Library/plugin_start.cpp" -o "$OUT/obj/plugin_start.o"
K=$OUT/GT1URBFix.kext; rm -rf "$K"; mkdir -p "$K/Contents/MacOS"
clang++ -target x86_64-apple-macos10.15 -nostdlib -Xlinker -kext -Xlinker -export_dynamic \
 -L$SDK/Library/x86_64 -lkmod -lkmodc++ \
 "$OUT"/obj/*.o "$(clang -print-resource-dir)/lib/darwin/libclang_rt.cc_kext.a" -o "$K/Contents/MacOS/GT1URBFix"
cp "$B/Info.plist" "$K/Contents/Info.plist"
echo built "$K"
