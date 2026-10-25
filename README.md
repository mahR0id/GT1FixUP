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

A small [Lilu](https://github.com/acidanthera/Lilu) plugin with two parts. The
code is explained in [GT1URBFix/src/kern_start.cpp](GT1URBFix/src/kern_start.cpp).

1. **Kernel sku hook.** It wraps `IntelAccelerator::initHardwareCaps` in
   `AppleIntelKBLGraphics` (found by symbol name) and sets the sku to 0 when the
   GPU's real device-id is a Comet Lake GT1. The kernel then reports genuine GT1
   caps and the drivers size the URB for their main pipeline themselves.
2. **URB constants.** The Metal driver also keeps the GT2 URB size (payload
   `0x08030380`, 896 entries) in its own tables, used by other pipeline
   configurations. Chrome hits those, so without this part Chrome is corrupted.
   The plugin patches the four constants to 256 entries as the driver's pages
   are loaded. The payload is a hardware command encoding, so the same bytes are
   expected on every macOS version; patterns that are not found do nothing.

- **No per-version code patches.** The field offset is read from the function's
  own code at runtime. The layout is identical on macOS 15.7.9 and every stable 26.x
  KDK (26.0 to 26.7.2), and there is no macOS version limit.
- **Keep your iGPU settings.** The `device-id = C49B0000` spoof and the
  framebuffer properties are still needed; without the spoof the Intel driver
  does not load at all.
- **Tested** on the G15 with macOS 15.7.9: 100% of triangles drawn in Metal and
  OpenGL, Chrome renders normally. Not yet tested on macOS 26 or 10.15.

| Boot-arg | Effect |
|---|---|
| `-gt1urboff` | disable the plugin |
| `-gt1urbdbg` | debug logging (with DEBUG Lilu) |
| `-gt1urbforce` | apply the fix even if the real device-id is not a known GT1 |

**Check that it works** (no root needed):

```sh
ioreg -p IOService -n IOResources -l | grep GT1URBFix
```

You should see `"GT1URBFix-hook" = "routed initHardwareCaps"`,
`"GT1URBFix-urb-patches" = "routed _cs_validate_page"`,
`"GT1URBFix-sku-before" = 1` and `"GT1URBFix-sku-after" = 0`.

History: v1 patched six byte patterns in the Sequoia drivers. v2 added the kernel
hook. v3.0 tried the hook alone, which broke Chrome. v3.1 keeps the hook plus the
four URB constants, and drops the two code patches the hook makes unnecessary.

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
| `urb_patch_check` | counts patched vs. stock URB patterns in the loaded drivers (v3.1: writeInit, template, defaults and payloads patched, urbsize stock) |

## Notes

[docs/CONTEXT.md](docs/CONTEXT.md) has the full investigation (hardware, findings,
measurements) and [docs/PLAN.md](docs/PLAN.md) the original debugging plan. Network
addresses were redacted.

## Caveats

- This patches Apple's drivers in memory. Use at your own risk; keep a backup EFI.
- The Lilu SDK is GPL-3.0; check licensing before redistributing binaries.
- No license has been chosen for this repository yet.
