# Context: macOS on Dell G15 5510 (i5-10200H iGPU 0x9BA4)

Last updated: 2026-10-18

## Goal

Run macOS on the Dell G15 5510 with working Metal acceleration on the Intel iGPU, and debug the iGPU rendering bug live from the XPS 13.

## Machines

| | Target: Dell G15 5510 | Debug host: Dell XPS 13 9380 |
|---|---|---|
| Address | 192.168.88.7 (Windows, Wi-Fi) | `mah@192.168.88.33` (en1); earlier seen at .8 |
| CPU | i5-10200H (Comet Lake-H) | i7-8565U (Whiskey Lake) |
| iGPU | Intel UHD, device-id **0x9BA4** (GT1, 12 EU), `PciRoot(0x0)/Pci(0x2,0x0)` | UHD 620 |
| dGPU | GTX 1650 at `PciRoot(0x0)/Pci(0x1,0x0)/Pci(0x0,0x0)`: never works in macOS, disable with `-wegnoegpu` | none |
| Screen | BOE 1920×1080 @ 120 Hz on the iGPU (eDP) | |
| Wi-Fi / BT | Intel AX201 (AirportItlwm/itlwm + IntelBluetoothFirmware) | Atheros (ath10wm) |
| Ethernet | Realtek RTL8111 (RealtekRTL8111.kext has **no KDP support**) | en3 |
| OS | Windows 11 now; macOS **15.7.9 (24G830)** planned | macOS 15.7.7 (24G720), SMBIOS MacBookPro16,2 |
| BIOS | Dell 1.43.0; storage already in AHCI/NVMe mode (no RST/VMD) | |

### G15 NVMe drives (both Samsung PM981, which forums report as unreliable on macOS)

| Drive | Firmware | Slot / OpenCore path | Current use |
|---|---|---|---|
| MZVLB1T0HALR-00000, 1 TB | EXA7301Q (Samsung generic OEM) | RP09: `PciRoot(0x0)/Pci(0x1D,0x0)/Pci(0x0,0x0)` | D: + 329 GB unknown partition |
| PM981 512 GB | EXA74D1Q (Dell OEM, "D1") | RP21: `PciRoot(0x0)/Pci(0x1B,0x0)/Pci(0x0,0x0)` | Windows C: (boot disk) |

No newer firmware was found for either drive. Firmware is unlikely to fix macOS compatibility anyway.

### External storage

- **Crucial X6 500 GB (USB):**
  - EFI: `disk5s1` on the XPS
  - APFS volume "Untitled": holds the user's **work files** and `Install macOS Sequoia.app` (15.7.7, 24G720)
  - exFAT "Untitled 3": holds Windows ISOs
- **USB stick "MACOS_EFI":** the XPS's own working OpenCore EFI.

## Rules: do not break these

- **Do not edit the MACOS_EFI USB.** It is the XPS's boot EFI. A G15 EFI must live somewhere else.
- **Do not modify the Crucial X6 APFS volume.** It holds the user's work files.
- `sudo` on the XPS needs the user's password, and this Windows account is **not admin**. Ask the user before any admin step.
- Ask before any long or slow operation (large downloads, USB 2 reads). The user has interrupted these before.

## Tools already on the XPS

- **KDK:** `/Library/Developer/KDKs/KDK_15.7.9_24G830.kdk`, installed by the user. It matches the planned G15 build.
  - The kernel loads in lldb with full symbols (source-level).
  - It includes an **unstripped `AppleIntelKBLGraphics.kext`** (5230 symbols; the shipped copy has 0).
- `lldb` and `otool` from the Command Line Tools, plus python3. No Xcode, no Homebrew.
- Work folder `~/g15dbg/` holds:
  - `kbl_kdk.s`: full `otool -tV` disassembly of the KDK AppleIntelKBLGraphics
  - `getGPUInfo.s`, `initHardwareCaps.s`, `probe.s`: extracted functions
  - `kdk/`: a partial dmg download that can be deleted

## Findings so far

1. **The rendering bug (acidanthera/bugtracker#1249):**
   - Spoofing 0x9BA4 → 0x9BC4 gives Metal, but the output is corrupted.
   - The user confirmed the corruption also appears over remote desktop. So the drawing (accelerator / Metal) is broken, not the screen output (framebuffer). Platform-id and framebuffer patches cannot fix it.
   - Acidanthera won't investigate.
   - WhateverGreen's FAQ says Pentium/Celeron-class (GT1) graphics "can't be enabled".
2. **Static analysis of the KDK `AppleIntelKBLGraphics` (15.7.9):**
   - **EU count comes from hardware.** `IntelAccelerator::getGPUInfo()` takes the popcount of MMIO `0xA260/0xA264/0xA268` (masked 0xFFFFFF) and stores it at `this+0x10fc` (`getEUCount()`). On the G15 this should read as 12.
   - **The subslice layout comes from a table.** `IntelAccelerator::initHardwareCaps()` copies it from `g_cSkuGlobals[sku]` (4 entries × 0x3c bytes), where `sku = this+0x10f8`. The fields land at `+0x1100..+0x1134`; `fNumSubSlices` is at `+0x1120`.

     | sku | GT | slices | subslices | EU/subslice | EUs |
     |---|---|---|---|---|---|
     | 0 | GT1 | 1 | 2 | 6 | **12, matches 0x9BA4** |
     | 1 | GT2 | 1 | 3 | 8 | 24 |
     | 2 | GT3 | 2 | 6 | 8 | 48 |
     | 3 | GT4 | 3 | 9 | 8 | 72 |

   - **`IntelAccelerator::probe()` picks the sku from the device-id:**
     - sku 1 (GT2): 9BC4, 9BC5, 9BC8, 9BCA, 9B41, 87C0, 3E98, 3E9B, 5912, 5916, 591A, 591B, 591C, 591D, 591E, 5921
     - sku 0 (GT1): **5902, 5906, 590A, 590B, 590E, 5913, 5915, 5917**
     - sku 2: 3EA5, 3EA6, 592B
     - sku 3: 593A, 593B, 593D, 5932
     - anything else: `.cold.1` path, sku 4 (out of bounds)
   - **Hypothesis:** the 9BC4 spoof selects the GT2 layout (3×8) on 12-EU GT1 hardware, which breaks how work is spread across EUs. Forcing sku 0 while staying on a supported driver path may fix rendering.
   - `IGAccelDevice::get_hw_caps()` reads `this+0x10fc`, so user space (the Metal driver) gets caps from the kernel. Not yet verified whether `AppleIntelKBLGraphicsMTLDriver.bundle` also has its own device-id tables.
3. **No KDP on the G15:** the Realtek driver lacks polled-mode debugger support and Wi-Fi never supports KDP. Live work goes over SSH plus a Lilu plugin, unless KDP support is added to RealtekRTL8111.

## Sources

- https://github.com/acidanthera/bugtracker/issues/1249
- https://github.com/acidanthera/WhateverGreen/blob/master/Manual/FAQ.IntelHD.en.md
- https://dortania.github.io/OpenCore-Install-Guide/config-laptop.plist/coffee-lake-plus.html
- KDK mirror: https://github.com/dortania/KdkSupportPkg/releases/tag/24G830
