# Changelog

Each `## <tag>` section below is copied into the GitHub release with the same tag.

## 4.0.0

**Recommended version, and the first one that works on macOS 26 (Tahoe).**
**If you installed 3.1.0 on macOS 26, replace it: 3.1.0 hangs the GPU there.**

### What changed since 3.1.0

- **The kernel fix is gone.** 3.1.0 made the kernel report the GPU as a GT1
  ("sku 0"). On macOS 26 that hangs the GPU: compute work in WindowServer never
  finishes, the screen freezes, and after 2 minutes the watchdog panics and
  reboots the Mac. Reporting sku 0 with the GT2 layout (tested as 3.2.0, never
  released) hung it right at login instead.
- **Back to patching only the drivers.** The kernel keeps reporting macOS's own
  values, and the plugin patches the six places where the Metal and OpenGL
  drivers size the URB, so they use the GT1 size. These six patterns were found
  unchanged in macOS 15.7.9 and 26.7.2.
- **Kept from 3.x:** no macOS version limit and the `ioreg` status check. Drivers
  stored on disk (macOS 10.15 and older) are patched too.
- **Removed boot-args:** `-gt1urbforce` (not needed any more).

### Install

1. Unzip and copy `GT1URBFix.kext` to `EFI/OC/Kexts/`, replacing the old one
   (the `config.plist` entry stays the same, after Lilu).
2. Keep `device-id = C49B0000`, `AAPL,ig-platform-id = 0900A53E` and the
   framebuffer patches. Remove `-igfxvesa` and `-gt1urboff` from boot-args if present.
3. Reboot and check:
   ```sh
   ioreg -p IOService -n IOResources -l | grep GT1URBFix
   ```
   Expect `"GT1URBFix-urb-patches" = "routed _cs_validate_page"`.

### Tested

- macOS 26.7.2 (25G332) on a Dell G15 5510: all six patterns patched, stable.
- macOS 15.7.9: same six patterns as 1/2.0.1, 100% of triangles drawn, Chrome normal.

## 3.1.0

**Do not use on macOS 26 (Tahoe): it hangs the GPU there. Use 4.0.0.**

Fixes the broken Intel UHD iGPU on Comet Lake GT1 laptops
(e.g. Dell G15 5510, i5-10200H, device-id `0x9BA4`) without per-macOS-version patches.

### What changed since 1

- **New kernel fix.** The plugin now makes the kernel report the GPU as what it
  really is (a GT1, "sku 0") instead of the spoofed GT2. The Metal and OpenGL
  drivers then size their buffers correctly on their own.
  - It finds the function by name and reads the field offset from the code at
    runtime, so it is not tied to one macOS build. The layout was checked
    identical on macOS 15.7.9 and every stable macOS 26 build (26.0 to 26.7.2).
  - It reads the real device-id from the PCI `compatible` property, because the
    normal PCI read returns the spoofed `0x9BC4`.
- **Fewer driver patches.** Only the four GT2 URB-size constants in the Metal
  driver are still patched (needed for Chrome and other apps that use more than
  the basic pipeline). The two code patches from 1 are gone; the kernel fix
  replaces them, including the OpenGL one.
- **No macOS version limit.** It loads on any version Lilu supports;
  `-gt1urbbeta` is no longer needed.
- **Easy to check.** No root or debug Lilu needed:
  ```sh
  ioreg -p IOService -n IOResources -l | grep GT1URBFix
  ```
  Expect `"GT1URBFix-hook" = "routed initHardwareCaps"`,
  `"GT1URBFix-urb-patches" = "routed _cs_validate_page"`, sku-before `1`,
  sku-after `0`.
- **New boot-arg** `-gt1urbforce`: apply the kernel fix even if the real
  device-id is not a known Comet Lake GT1.

### Install

1. Unzip and copy `GT1URBFix.kext` to `EFI/OC/Kexts/`, replacing the old one.
2. In `config.plist`, `Kernel -> Add` must list it **after Lilu** (same entry as before).
3. Keep your iGPU properties: `device-id = C49B0000` (without it the Intel driver
   does not load at all), `AAPL,ig-platform-id = 0900A53E` and the framebuffer patches.
4. Reboot and run the `ioreg` check above.

### Tested

- macOS 15.7.9 on a Dell G15 5510: 100% of triangles drawn in Metal and OpenGL,
  Chrome and the desktop render normally.
- **Not yet tested** on macOS 26 or 10.15. Reports are welcome.

## 1

First release (GT1URBFix 1.0.0). Patches six byte patterns in the macOS 15
(Sequoia) Intel Metal and OpenGL drivers so they use a GT1-sized URB. Works on
macOS 15 only and needs `-gt1urbbeta` on other versions. Superseded by 4.0.0.
