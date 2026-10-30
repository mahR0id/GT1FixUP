#!/bin/sh
# Build the GPU render tests (Command Line Tools only). Copy the binaries to the G15 and run them there.
set -e
D=$(cd "$(dirname "$0")" && pwd); O=$D/out; mkdir -p "$O"
clang -O2 -Wno-#warnings -framework OpenGL "$D/gl_render_test.c" -o "$O/gl_render_test"
for t in metal_render_sweep metal_blend_aa_test urb_patch_check metal_hwcaps_urb; do
  clang -fobjc-arc -framework Metal -framework Foundation -framework OpenGL -I"$D" "$D/$t.m" -o "$O/$t"
done
echo built in "$O"
