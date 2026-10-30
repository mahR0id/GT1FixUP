# GT1FixUP: macOS on the Dell G15 5510 (i5-10200H) iGPU

Fixes a broken Intel UHD iGPU on the Dell G15 5510 (Comet Lake-H, **GT1**, 12 EUs,
device-id `0x9BA4`) when running macOS 15 (Sequoia) with OpenCore.

## The problem

macOS has no driver for the GT1 id, so OpenCore spoofs a GT2 id (`0x9BC4`). The
kernel driver then reports "sku 1" (GT2), and the Metal and OpenGL drivers size
the vertex URB (on-chip buffer for vertex-shader output) for GT2: 256 KB. A GT1
only has 128 KB, so vertices past the end are dropped and most triangles are
never drawn: broken windows, missing text, flicker, no transparency effects.

## The fix: [GT1URBFix/](GT1URBFix/)

A small [Lilu](https://github.com/acidanthera/Lilu) plugin. As the Intel Metal
and OpenGL drivers are loaded, it patches the six places where they size the URB
for GT2, so they use the GT1 size (128 KB). The kernel and the hardware caps it
reports are left untouched. The code is explained in
[GT1URBFix/src/kern_start.cpp](GT1URBFix/src/kern_start.cpp).

- **Tested** on the Dell G15 5510:
  - macOS 15.7.9: 100% of triangles drawn in Metal and OpenGL, Chrome normal.
  - macOS 26.7.2 (Tahoe): all six patterns found and patched, no GPU hangs.
- **No macOS version limit.** If a future macOS changes these bytes, the patches
  simply don't apply (you see the old corruption, not a crash).
- **Keep your iGPU settings.** The `device-id = C49B0000` spoof and the
  framebuffer properties are still needed; without the spoof the Intel driver
  does not load at all.
- **Don't use `-igfxvesa`**: it turns graphics acceleration off completely.

| Boot-arg | Effect |
|---|---|
| `-gt1urboff` | disable the plugin |
| `-gt1urbdbg` | debug logging (with DEBUG Lilu) |

**Check that it works** (no root needed):

```sh
ioreg -p IOService -n IOResources -l | grep GT1URBFix
```

You should see `"GT1URBFix-urb-patches" = "routed _cs_validate_page"`. The
`urb_patch_check` test below shows which patterns are patched in the running drivers.

**Why not a kernel fix?** v2 and v3 made the kernel report the GPU as a GT1
(sku 0) instead of patching the drivers. That worked on macOS 15 but hung the GPU
on macOS 26 (compute hangs in WindowServer, then a watchdog panic), so v4 went
back to patching only the drivers. See [CHANGELOG.md](CHANGELOG.md).

### Build

Needs only the Xcode Command Line Tools, a checkout of
[MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) and a **DEBUG** release
of `Lilu.kext` (for its plugin headers).

```sh
MACKERNELSDK=/path/to/MacKernelSDK LILU_KEXT=/path/to/Lilu.kext sh GT1URBFix/build.sh
```

Output: `GT1URBFix/out/GT1URBFix.kext`. Copy it to `EFI/OC/Kexts/`, add it to
`Kernel -> Add` in `config.plist` **after Lilu**, and reboot.

## Tests: [tests/](tests/)

Small programs that draw a grid of triangles and count how many reach the screen
(100% when correct; about 10-20% before the fix). Build with `sh tests/build.sh`,
run on the target machine. Set `GPU=NVIDIA` to pick another Metal device.

| Test | Checks |
|---|---|
| `metal_render_sweep` | Metal triangle coverage |
| `gl_render_test` | OpenGL triangle coverage |
| `metal_blend_aa_test` | blending, filtering, MSAA |
| `metal_hwcaps_urb` | hardware caps and URB size the driver reports |
| `urb_patch_check` | counts patched vs. stock URB patterns in the loaded Metal and OpenGL drivers (with v4 all six show `new`) |

## Notes

[docs/CONTEXT.md](docs/CONTEXT.md) has the full investigation (hardware, findings,
measurements) and [docs/PLAN.md](docs/PLAN.md) the original debugging plan. Network
addresses were redacted.

## Caveats

- This patches Apple's drivers in memory. Use at your own risk; keep a backup EFI.
- The Lilu SDK is GPL-3.0; check licensing before redistributing binaries.
- No license has been chosen for this repository yet.
