# Context: macOS on Dell G15 5510 (i5-10200H iGPU 0x9BA4)

Last updated: 2026-10-18

## Goal

Run macOS on the Dell G15 5510 with working Metal acceleration on the Intel iGPU, and debug the iGPU rendering bug live from the XPS 13.

## Machines

| | Target: Dell G15 5510 | Debug host: Dell XPS 13 9380 |
|---|---|---|
| Address | <ip> (Windows, Wi-Fi) | `<user>@<g15-ip>` (en1); earlier seen at .8 |
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
3. **Live session, 2026-10-18 (G15 on macOS 15.7.9 at `<user>@<g15-ip>`, booting OpenCore from the Crucial X6 EFI `disk0s1`):**
   - **No acceleration at first:** in `config.plist` the iGPU's `device-id` and `AAPL,ig-platform-id` had been swapped. Fixed to `device-id = C49B0000` and `AAPL,ig-platform-id = 0900A53E`. Metal 3 now comes up; backup at `EFI/OC/config.plist.bak-igpu-swap`.
   - **Corruption reproduced:** dropped or half-drawn quads, text rows drawn over other rows, big stray triangles, flicker during animations. A Metal compute test passes (0 wrong results out of 1M threads).
   - **Kernel sku patch did not help:** an OpenCore `Kernel → Patch` on `IntelAccelerator::probe` (`41C786F810000001000000` → `...00...`, which forces sku 0) is in the config and boots, but the corruption remains. Backup without it: `EFI/OC/config.plist.bak-pre-gt1patch`.
   - **GT1 device-ids can't be spoofed:** 5902/5906/590x are not in the `IOPCIPrimaryMatch` of `AppleIntelKBLGraphics`.
   - **`g_cSkuGlobals` also holds L3 data:** each row also carries L3 KB (384/768/1536/2304) and L3 banks (2/4/8/12). In the kernel the table is read only by `initHardwareCaps`.
   - **Root cause (likely): the Metal driver hard-codes a GT2 URB layout.** The driver is `AppleIntelKBLGraphicsMTLDriver`, extracted from the dyld shared cache to `~/g15dbg/mtl/MTLDriver` on the XPS.
     - `IGRenderStateURB::writeInit` emits `3DSTATE_URB_VS = 0x08030380`: starts at 32 KB, 896 entries × 256 B, so it ends at 256 KB. HS, DS and GS are `0x08000000`. The same values are stored as a default table at `0x7ffb00144220`.
     - L3CNTLREG (`0x7034`) is always `0x80000040`, a fixed partition.
     - A GT1 (2 L3 banks) has at most about 192 KB of URB, so VS entries wrap around and overwrite each other.
   - **Confirmed by measurement:** a Metal render test (a grid of quads, read back) draws only about 10–20% of triangles on the G15 and 100% on the XPS. The missing triangles are dropped cleanly, never misplaced.
     - Rewriting `URB_VS` entries in the test's own pipeline object gives 100% at ≤ 384 entries, 64% at 448, 48% at 512 and about 10% at 896.
     - So the usable URB is exactly 128 KB (32 KB of push constants + 384 × 256 B), which matches 32 L3CNTLREG ways × 4 KB on a 2-bank GT1.
   - **Fix: [GT1URBFix/](../GT1URBFix/), a Lilu plugin.** It hooks `_cs_validate_page` and patches shared-cache pages, setting `URB_VS` entries to 256.
     - It patches all 5 occurrences of `0x08030380` in the Metal driver: the `writeInit` immediate, 2 command templates, the 16-byte defaults table, and the per-config payload table (the tessellation variant).
     - Version 1 missed the payload table, so one of the four pipeline URB configs stayed at 896 and the corruption remained.
   - **Version 2 was still not enough:** `-[MTLIGRenderPipelineState generate3DStateURB:]` copies the template, then calls `setupURBState:([self getUrbSizeInUrbAllocations] - 4)`, which recomputes the URB for each pipeline.
     - `getUrbSizeInUrbAllocations` returns `16 + (mHWCaps->sku != 0) * 16` (8 KB units). The G15 reports sku 1, so it gets 256 KB.
     - Zeroing `mHWCaps+0x10` in-process gives URB_VS = 384 and 100% of triangles drawn.
     - The OpenCore kernel sku patch never took effect: the caps still show sku 1 and GT2-row values. The likely reason is that OpenCore only patches kexts in the Boot KC, while Apple's graphics kexts are in the System KC.
     - **Version 3** adds a 3-byte patch (`setne al` → `nop×3`) so the function always returns 16 (128 KB).
   - **The OpenGL driver had the same bug:** `updateURBState` calls `setupURBState(ctx, (ctx->sku != 0) << 4 | 0xC)`. **Version 4** forces 12 (`setne sil` → `nop×4`).
     - After a reboot with v4, OpenGL (legacy fixed-function, immediate mode, core 4.1) and Metal both draw 100% of triangles (`tests/`).
     - The user reports that UI transparency effects started working once the GL fix was in.
   - **Only Metal matters for WindowServer itself:** WindowServer loads only the Metal driver (no GL). The GL driver has no `0x08030380` constant.
   - **Test tools:** `/tmp/urbchk` on the G15 counts old and new patterns in the mapped driver. The render test, the URB A/B test (`/tmp/ab <entries>`) and the pipeline-object dumper are all built on the XPS.
   - **Status at the end of the session (2026-10-19):**
     - The G15 is reachable at `<user>@<g15-ip>` (earlier <ip>). It runs GT1URBFix v4; panic logging to the EFI is on (`Misc → Debug → ApplePanic`).
     - Old kext builds `GT1URBFix-v1/v2/v3.kext.bak` and `config.plist.bak-*` are still on the X6 EFI (`disk0s1`, `EFI/OC`).
     - **Not done:** remove the unused OpenCore `IntelAccelerator::probe` kernel patch, remove `/etc/sudoers.d/claude-tmp` on the G15, commit the repo.
     - **Text looks sharp on the G15:** this is not a GPU bug. The blend, linear-filter and MSAA tests match the XPS (`tests/metal_blend_aa_test.m`). It is normal macOS grayscale text on a non-Retina 1080p panel; options are `AppleFontSmoothing` or a HiDPI scaled mode.
   - **The GTX 1650 now works under the user's test of the NullMoth driver** ([nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver)). The boot-args changed: `-wegnoegpu` was removed and `nvfb=1 nvaccel=1 nvfbheads=4 -nvkmsnosmooth gfxrst=4` and others were added.
     - Both GPUs report Metal 3. The Metal tests pass on the 1650 (`GPU=NVIDIA ./metal_render_sweep`): 100% of triangles drawn, blend and MSAA within ±1 of Intel.
     - The Intel iGPU drives the panel, so it is the default Metal device. The NVIDIA has no display attached (the driver has no Optimus-style mux).
     - To run a game on the 1650: attach a monitor to a 1650-wired port and make it the main display, or use a game that lets you choose the GPU. A `DYLD_INSERT_LIBRARIES` shim forcing `MTLCreateSystemDefaultDevice` was offered but not written.
5. **No KDP on the G15:** the Realtek driver lacks polled-mode debugger support and Wi-Fi never supports KDP. Live work goes over SSH plus a Lilu plugin, unless KDP support is added to RealtekRTL8111.

## Sources

- https://github.com/acidanthera/bugtracker/issues/1249
- https://github.com/acidanthera/WhateverGreen/blob/master/Manual/FAQ.IntelHD.en.md
- https://dortania.github.io/OpenCore-Install-Guide/config-laptop.plist/coffee-lake-plus.html
- KDK mirror: https://github.com/dortania/KdkSupportPkg/releases/tag/24G830
