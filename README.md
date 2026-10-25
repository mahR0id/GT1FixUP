# GT1FixUP: macOS on the Dell G15 5510 (i5-10200H) iGPU

Fixes a broken Intel UHD iGPU on the Dell G15 5510 (Comet Lake-H, **GT1**, 12 EUs,
device-id `0x9BA4`) when running macOS 15 (Sequoia) with OpenCore.

## The problem

macOS has no driver for the GT1 id, so OpenCore spoofs a GT2 id (`0x9BC4`). The
Metal and OpenGL drivers then assume GT2 resources. They hard-code a 256 KB vertex
URB (on-chip buffer for vertex-shader output), but a GT1 only has about 128 KB.
Vertices past the end are dropped, so most triangles are never drawn: broken
windows, missing text, flicker, no transparency effects.

## The fix: [GT1URBFix/](GT1URBFix/)

A [Lilu](https://github.com/acidanthera/Lilu) plugin (kext) with two parts. Both
are explained in [GT1URBFix/src/kern_start.cpp](GT1URBFix/src/kern_start.cpp).

1. **Kernel sku hook (v2, version independent).** The drivers only pick GT2 sizes
   because the kernel reports sku 1 for the spoofed id. The plugin wraps
   `IntelAccelerator::initHardwareCaps` in `AppleIntelKBLGraphics` by symbol name
   and, when the real PCI device-id is a Comet Lake GT1, sets the sku to 0. The
   kernel then reports genuine GT1 caps and the drivers size the URB themselves.
   The field offset is read from the function's own code at runtime. The layout
   was checked identical on macOS 15.7.9 and every stable 26.x KDK (26.0 to 26.7.2).
2. **User-space byte patches (v1, macOS 15 drivers).** It hooks the kernel's
   `_cs_validate_page` and patches the Intel Metal/GL drivers as their pages are
   loaded, so they use a GT1-sized URB. Patterns that are not found do nothing.

There is no macOS version limit.

| Boot-arg | Effect |
|---|---|
| `-gt1urboff` | disable the plugin |
| `-gt1urbdbg` | debug logging (with DEBUG Lilu) |
| `-gt1urbnosku` | disable part 1 (kernel sku hook) |
| `-gt1urbnouser` | disable part 2 (user-space byte patches) |
| `-gt1urbforce` | apply part 1 even if the real device-id is not a known GT1 |

**Status:** part 2 is tested on macOS 15.7.x (100% of triangles drawn). Part 1 is
built but **not yet tested on hardware**. To test it on its own, boot with
`-gt1urbnouser` and run the tests below; the kernel log should show
`real device-id 0x9BA4, sku 1 -> 0 (GT1 caps)`.

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
| `urb_patch_check` | counts patched vs. unpatched byte patterns in the loaded drivers |

## Notes

[docs/CONTEXT.md](docs/CONTEXT.md) has the full investigation (hardware, findings,
measurements) and [docs/PLAN.md](docs/PLAN.md) the original debugging plan. Network
addresses were redacted.

## Caveats

- This patches Apple's drivers in memory. Use at your own risk; keep a backup EFI.
- The Lilu SDK is GPL-3.0; check licensing before redistributing binaries.
- No license has been chosen for this repository yet.
