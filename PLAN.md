# Plan: live debugging of the G15 iGPU (0x9BA4) on macOS

See [CONTEXT.md](CONTEXT.md) for the hardware, the rules and the findings so far.

**Important:** the current Claude session runs on the G15's Windows install. When the G15 boots macOS, that session is gone. Live debugging must be driven **from the XPS** (`mah@192.168.88.33`): either run Claude Code there, or SSH from another machine.

## Phase 0: Prepare (before booting macOS on the G15)

1. **Choose where the G15 EFI goes.** Options: the Crucial X6 EFI partition (`disk5s1`) or a separate USB stick. **Never the MACOS_EFI USB** (that is the XPS's EFI).
2. **Build the G15 EFI** in `~/g15dbg/EFI` on the XPS, then copy it to the chosen place.
   - **Kexts:** Lilu, VirtualSMC, WhateverGreen, AppleALC, NVMeFix, AirportItlwm, IntelBluetoothFirmware, RealtekRTL8111, VoodooPS2/VoodooI2C. Use **DEBUG builds of Lilu and WhateverGreen**.
   - **Boot-args:** `-v keepsyms=1 debug=0x100 msgbuf=1048576 -wegnoegpu -liludbgall`
   - **iGPU at `PciRoot(0x0)/Pci(0x2,0x0)`:** `AAPL,ig-platform-id = 0900A53E`, `framebuffer-patch-enable = 01000000`, `framebuffer-stolenmem = 00003001`, `framebuffer-fbmem = 00009000`. **No device-id spoof for the first boot.**
   - **NVMe:** properties at RP09/RP21 if macOS goes on a PM981. A supported SSD or the USB SSD is preferred.
   - A USB map built on the G15 itself.
3. **Install macOS 15.7.9 (24G830)** on the G15. It must match the installed KDK; if the build differs, get the matching KDK.
4. **Enable Remote Login** on the G15 (System Settings → General → Sharing). Then copy the XPS key: `ssh-copy-id <user>@<g15-ip>` from the XPS.

## Phase 1: Baseline, no acceleration

From the XPS, over SSH:
- `sw_vers`, `kextstat | grep -i -E "lilu|whatever|intel"`, `ioreg -l -p IODeviceTree -n GFX0`
- `log show --last boot --predicate 'sender CONTAINS "Lilu" OR sender CONTAINS "WhateverGreen" OR eventMessage CONTAINS "[IGPU]"'`
- Save everything to `~/g15dbg/live/baseline/` on the XPS.

## Phase 2: Reproduce the bug (spoof 0x9BC4)

1. Add `device-id = C49B0000` and reboot. Confirm the corruption, including over screen sharing.
2. Collect:
   - kernel log lines with `[IGPU]` (e.g. `HWCAPS ... fNumSubSlices`)
   - `ioreg -r -c IntelAccelerator -l`
   - `system_profiler SPDisplaysDataType`
3. **Metal probe tool:** a small compute program. Each thread writes its threadgroup/thread IDs into a buffer, plus a test render to a texture. It shows exactly which work gets dropped. Build it on the XPS with the CLT `clang` + `-framework Metal`, copy it to the G15, run it over SSH.

## Phase 3: Experiments (ordered by effort)

| # | Experiment | How | What it tells us |
|---|---|---|---|
| A | **Spoof a GT1 KBL id**: device-id `02590000` (0x5902), or another sku-0 id (5906/590A/590B/590E/5913/5915/5917) | DeviceProperties only | Whether the GT1 table (2×6) fixes rendering. **First check** that the id is in the `IOPCIMatch` of `AppleIntelKBLGraphics` and of the framebuffer kext used. |
| B | **Lilu plugin "GT1Fix"**: keep 0x9BC4, hook `IntelAccelerator::probe` (or `initHardwareCaps`) and force `this+0x10f8 = 0` | New kext, built on the XPS | A kernel-only fix that keeps the native CFL framebuffer path |
| C | **Logging plugin**: log `getEUCount()`, `+0x10f8`, `+0x1120..+0x1134`, and the raw `0xA260/4/8` registers at start | Same Lilu plugin, logging only | Proves what the driver actually sees on the G15 |
| D | **Check the Metal driver**: find device-id / GT tables in `AppleIntelKBLGraphicsMTLDriver.bundle` and see whether it trusts `get_hw_caps` | Static analysis on the XPS | Whether a user-space patch is also needed |

Run C first: it costs nothing and confirms the hypothesis on real hardware. Then try A (easiest); fall back to B if A's id isn't matched.

## Phase 4 (optional): full kernel debugger (KDP)

Only if needed, for example to break into the driver live:
- Add polled-mode KDP support (`attachDebuggerClient`, `sendPacket`, `receivePacket`) to RealtekRTL8111. Build it on the XPS.
- Connect the G15 by Ethernet cable. Boot-args: `debug=0x144 kdp_match_name=enX`.
- On the XPS: `lldb /Library/Developer/KDKs/KDK_15.7.9_24G830.kdk/System/Library/Kernels/kernel`, then `kdp-remote <g15-ip>`.

## Open questions for the user

- Where should the G15 EFI live (Crucial X6 EFI or a separate USB)?
- Which disk will macOS be installed on (PM981, the USB SSD, or a new SSD)?
- Is it OK to install Claude Code on the XPS for the live phase (no sudo needed)?
