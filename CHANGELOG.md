# Changelog

Each `## <tag>` section below is copied into the GitHub release with the same tag.

## 3.1.0

**Recommended version.** Fixes the broken Intel UHD iGPU on Comet Lake GT1 laptops
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
macOS 15 only and needs `-gt1urbbeta` on other versions. Superseded by 3.1.0.
