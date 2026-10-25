//
//  GT1URBFix: shrink the hard-coded GT2 vertex-shader URB allocation in the
//  Intel KBL/CFL Metal (and GL) user-space drivers so it fits a GT1 iGPU.
//
//  THE PROBLEM
//  The Dell G15's Comet Lake iGPU is a GT1 (12 EUs, device-id 0x9BA4), but macOS
//  has no driver for it, so OpenCore spoofs a supported GT2 id (0x9BC4). The
//  Metal/GL drivers then assume GT2 resources. The URB (Unified Return Buffer,
//  the on-chip memory that carries vertex-shader output to the next stage) is
//  hard-coded for GT2:
//
//    3DSTATE_URB_VS = 0x08030380  -> start 32 KB, 896 entries x 256 B, ends at 256 KB
//
//  A GT1 has only 2 L3 banks, and the driver's fixed L3 config (L3CNTLREG =
//  0x80000040) leaves it about 128 KB of URB. Vertex data past that is lost, so
//  most triangles are never drawn (screen corruption, missing text, flicker).
//  Measured: 384 entries (128 KB) is the last value that draws 100% of triangles;
//  448 already drops some.
//
//  THE FIX HAS TWO PARTS
//
//  A) Kernel hook (version independent, new in v2).
//     The drivers only pick GT2 sizes because the kernel reports "sku 1" (GT2) for
//     the spoofed id. IntelAccelerator::initHardwareCaps() in AppleIntelKBLGraphics
//     reads the sku from the accelerator object and copies that row of the
//     g_cSkuGlobals table (subslices, L3 banks, ...) into the hardware caps that
//     user space reads. We wrap that function by its symbol name and, when the
//     real PCI device-id is a Comet Lake GT1, set the sku to 0 first. The kernel
//     then reports genuine GT1 caps and the drivers choose the GT1 URB themselves.
//     The sku field offset is read from the function's own first instruction, so
//     no per-version offsets are hard-coded. Checked identical on macOS 15.7.9 and
//     every stable 26.x KDK from 26.0 to 26.7.2.
//
//  B) User-space byte patches (macOS 15 Sequoia drivers, from v1).
//     Patch the driver code so it programs 256 entries (0x100: 32 KB..96 KB)
//     instead of 896, and make the drivers' own "how big is the URB" helpers
//     return the GT1 size. Several places must all agree (see the tables below).
//     The drivers live in the dyld shared cache, which cannot be edited on disk,
//     so we route the kernel's _cs_validate_page (which code-signs each page as
//     it is mapped) and patch the bytes right after validation, the same
//     technique RestrictEvents uses. Patterns that are not found do nothing.
//
//  Boot-args:
//    -gt1urboff     disable the plugin
//    -gt1urbdbg     debug logging (DEBUG builds of Lilu)
//    -gt1urbnosku   disable part A (kernel sku hook)
//    -gt1urbnouser  disable part B (user-space byte patches)
//    -gt1urbforce   apply part A even if the real device-id is not a known GT1
//
//  There is no macOS version limit: the plugin loads on any version Lilu runs on.
//

#include <IOKit/IOService.h>
#include <Headers/kern_api.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/kern_user.hpp>
#include <Headers/plugin_start.hpp>

// Pointer to the original kernel _cs_validate_page, filled in by Lilu when routing.
static mach_vm_address_t orgCsValidatePage {};

// Each patch below is a pair of byte arrays: "find..." is what the stock driver
// contains and "repl..." is what we write over it. Lengths are equal, only the
// bytes that differ matter (the URB entry count, or a setne -> nop).

// 1) Metal: IGRenderStateURB::writeInit loads the 3DSTATE_URB_VS command as one
//    64-bit immediate: movabsq $0x0803038078300000, %rcx
//    (0x78300000 = command header, 0x08030380 = payload; 0x0380 = 896 entries).
//    Replacement payload 0x08030100 = 256 entries.
static const uint8_t findWriteInit[]  { 0x48, 0xB9, 0x00, 0x00, 0x30, 0x78, 0x80, 0x03, 0x03, 0x08 };
static const uint8_t replWriteInit[]  { 0x48, 0xB9, 0x00, 0x00, 0x30, 0x78, 0x00, 0x01, 0x03, 0x08 };

// 2) Metal: static command templates in __const, the URB_VS header + payload
//    followed by the start of the URB_HS header (used as context to avoid false hits).
static const uint8_t findTemplate[]   { 0x00, 0x00, 0x30, 0x78, 0x80, 0x03, 0x03, 0x08, 0x00, 0x00, 0x31, 0x78 };
static const uint8_t replTemplate[]   { 0x00, 0x00, 0x30, 0x78, 0x00, 0x01, 0x03, 0x08, 0x00, 0x00, 0x31, 0x78 };

// 3) Metal: default payload table (VS, HS, DS, GS dwords) that writeInit reads from.
static const uint8_t findDefaults[]   { 0x80, 0x03, 0x03, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08 };
static const uint8_t replDefaults[]   { 0x00, 0x01, 0x03, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08 };

// 4) Metal: per-pipeline-config payload table (VS, HS, DS with an 8-byte stride).
//    This is the tessellation variant, with HS/DS placed at 256 KB. Version 1 of
//    the plugin missed this one, which left one pipeline kind corrupted.
static const uint8_t findPayloads[]   { 0x80, 0x03, 0x03, 0x08, 0, 0, 0, 0, 0x00, 0x00, 0x00, 0x40, 0, 0, 0, 0, 0x00, 0x00, 0x00, 0x40 };
static const uint8_t replPayloads[]   { 0x00, 0x01, 0x03, 0x08, 0, 0, 0, 0, 0x00, 0x00, 0x00, 0x40, 0, 0, 0, 0, 0x00, 0x00, 0x00, 0x40 };

// 5) Metal: -[MTLIGRenderPipelineState getUrbSizeInUrbAllocations] returns
//    16 + (hwCaps->sku != 0) * 16 (in 8 KB units), and setupURBState: recomputes
//    every pipeline's URB layout from it, overriding patches 1-4.
//    The kernel reports sku 1 (GT2) for the 9BC4 spoof, so force the GT1 result
//    (16 = 128 KB). Only the setne is replaced:
// xor eax,eax; cmp dword [rcx+0x10],0; setne al -> nop; shl eax,4; add eax,0x10
static const uint8_t findUrbSize[]    { 0x31, 0xC0, 0x83, 0x79, 0x10, 0x00, 0x0F, 0x95, 0xC0, 0xC1, 0xE0, 0x04, 0x83, 0xC0, 0x10 };
static const uint8_t replUrbSize[]    { 0x31, 0xC0, 0x83, 0x79, 0x10, 0x00, 0x90, 0x90, 0x90, 0xC1, 0xE0, 0x04, 0x83, 0xC0, 0x10 };

// 6) OpenGL: updateURBState(GenContext*, ...) calls setupURBState(ctx, (ctx->sku != 0) << 4 | 0xC),
// i.e. 28 (256 KB - push constants) for GT2 or 12 (128 KB - push constants) for GT1. Force 12:
// xor esi,esi; cmp dword [rbx+0x2F40],0; setne sil -> nop x4; shl esi,4; or esi,0xC
static const uint8_t findGLUrbSize[]  { 0x31, 0xF6, 0x83, 0xBB, 0x40, 0x2F, 0x00, 0x00, 0x00, 0x40, 0x0F, 0x95, 0xC6, 0xC1, 0xE6, 0x04, 0x83, 0xCE, 0x0C };
static const uint8_t replGLUrbSize[]  { 0x31, 0xF6, 0x83, 0xBB, 0x40, 0x2F, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0xC1, 0xE6, 0x04, 0x83, 0xCE, 0x0C };

// Replace every occurrence of `find` (len bytes) with `repl` inside one page.
// `name` and `path` are only used for the log line.
static void replaceAll(void *data, vm_size_t size, const uint8_t *find, const uint8_t *repl, size_t len, const char *name, const char *path) {
	auto p = static_cast<uint8_t *>(data);
	for (vm_size_t i = 0; i + len <= size; i++) {
		if (p[i] == find[0] && memcmp(p + i, find, len) == 0) {
			memcpy(p + i, repl, len);
			SYSLOG("gt1urb", "patched %s at page offset 0x%llx in %s", name, static_cast<unsigned long long>(i), path);
			i += len - 1;
		}
	}
}

// Replacement for the kernel's _cs_validate_page. Let the original run first (so
// the page is validated and mapped), then, if the page belongs to the dyld shared
// cache or to an on-disk Intel driver bundle (older macOS), apply all patches to
// the in-memory copy.
static void csValidatePage(vnode_t vp, memory_object_t pager, memory_object_offset_t page_offset, const void *data, int *validated_p, int *tainted_p, int *nx_p) {
	FunctionCast(csValidatePage, orgCsValidatePage)(vp, pager, page_offset, data, validated_p, tainted_p, nx_p);

	char path[PATH_MAX];
	int pathlen = PATH_MAX;
	if (vn_getpath(vp, path, &pathlen) != 0)
		return;
	if (!UserPatcher::matchSharedCachePath(path) && !strstr(path, "AppleIntelKBLGraphics"))
		return;

	auto page = const_cast<void *>(data);
	replaceAll(page, PAGE_SIZE, findWriteInit, replWriteInit, sizeof(findWriteInit), "writeInit", path);
	replaceAll(page, PAGE_SIZE, findTemplate, replTemplate, sizeof(findTemplate), "urb template", path);
	replaceAll(page, PAGE_SIZE, findDefaults, replDefaults, sizeof(findDefaults), "urb defaults", path);
	replaceAll(page, PAGE_SIZE, findPayloads, replPayloads, sizeof(findPayloads), "urb payloads", path);
	replaceAll(page, PAGE_SIZE, findUrbSize, replUrbSize, sizeof(findUrbSize), "getUrbSizeInUrbAllocations", path);
	replaceAll(page, PAGE_SIZE, findGLUrbSize, replGLUrbSize, sizeof(findGLUrbSize), "GL updateURBState", path);
}

// ---------------------------------------------------------------------------
// Part A: kernel sku hook in AppleIntelKBLGraphics
// ---------------------------------------------------------------------------

static const char *kblPath[] { "/System/Library/Extensions/AppleIntelKBLGraphics.kext/Contents/MacOS/AppleIntelKBLGraphics" };
static KernelPatcher::KextInfo kextKBL { "com.apple.driver.AppleIntelKBLGraphics", kblPath, arrsize(kblPath), {true}, {}, KernelPatcher::KextInfo::Unloaded };

static constexpr const char *symInitHardwareCaps = "__ZN16IntelAccelerator16initHardwareCapsEv";

// Original IntelAccelerator::initHardwareCaps, and the offset of the sku field
// inside IntelAccelerator (0x10F8 in every build checked; found at runtime).
static mach_vm_address_t orgInitHardwareCaps {};
static uint32_t skuOffset {};
static bool forceGT1 {};

// Real (not spoofed) device-ids of Comet Lake GT1 iGPUs (Linux i915 CML_GT1 and CML_U_GT1).
static bool isCometLakeGT1(uint32_t id) {
	switch (id) {
		case 0x9BA2: case 0x9BA4: case 0x9BA5: case 0x9BA8:   // CML-H/S GT1
		case 0x9B21: case 0x9BAA: case 0x9BAC:                // CML-U GT1
			return true;
		default:
			return false;
	}
}

// Find the sku field offset from the start of initHardwareCaps, which begins by
// loading it: mov r32, dword [rdi + disp32] (optional REX, opcode 8B, ModRM with
// mod=10 and rm=rdi). Returns 0 if the instruction is not where we expect it.
static uint32_t findSkuOffset(const uint8_t *code) {
	for (size_t i = 0; i < 48; i++) {
		size_t j = i;
		if ((code[j] & 0xF0) == 0x40) {
			if (code[j] & 0x01) continue;  // REX.B set: base is r15, not rdi
			j++;
		}
		if (code[j] == 0x8B && (code[j + 1] & 0xC7) == 0x87) {
			uint32_t disp;
			memcpy(&disp, code + j + 2, sizeof(disp));
			if (disp >= 0x100 && disp < 0x10000)
				return disp;
		}
	}
	return 0;
}

// Wrapper: on a GT1, make the accelerator's sku 0 before the original copies
// the caps row for that sku. Returns what the original returns.
static bool initHardwareCaps(void *that) {
	auto accel = static_cast<IOService *>(that);
	auto sku = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(that) + skuOffset);
	auto pci = accel->getProvider();
	uint32_t realId = pci ? WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigDeviceID) : 0;

	if ((forceGT1 || isCometLakeGT1(realId)) && *sku != 0 && *sku < 4) {
		SYSLOG("gt1urb", "real device-id 0x%04X, sku %u -> 0 (GT1 caps)", realId, *sku);
		*sku = 0;
	} else {
		DBGLOG("gt1urb", "initHardwareCaps: real device-id 0x%04X, sku %u, not changed", realId, *sku);
	}
	return FunctionCast(initHardwareCaps, orgInitHardwareCaps)(that);
}

static void onKBLLoaded(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (index != kextKBL.loadIndex)
		return;

	auto fn = patcher.solveSymbol(index, symInitHardwareCaps, address, size);
	if (!fn) {
		SYSLOG("gt1urb", "%s not found, kernel sku hook disabled", symInitHardwareCaps);
		patcher.clearError();
		return;
	}
	skuOffset = findSkuOffset(reinterpret_cast<const uint8_t *>(fn));
	if (!skuOffset) {
		SYSLOG("gt1urb", "unexpected initHardwareCaps code, kernel sku hook disabled");
		return;
	}
	SYSLOG("gt1urb", "sku field at IntelAccelerator+0x%X", skuOffset);

	KernelPatcher::RouteRequest route(symInitHardwareCaps, initHardwareCaps, orgInitHardwareCaps);
	if (!patcher.routeMultiple(index, &route, 1, address, size))
		SYSLOG("gt1urb", "failed to route initHardwareCaps");
	else
		SYSLOG("gt1urb", "routed initHardwareCaps");
}

// ---------------------------------------------------------------------------
// Lilu plugin boilerplate
// ---------------------------------------------------------------------------

static const char *bootargOff[]   { "-gt1urboff" };
static const char *bootargDebug[] { "-gt1urbdbg" };
static const char *bootargBeta[]  { "-gt1urbbeta" };

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	bootargBeta,
	arrsize(bootargBeta),
	// No version limit: the oldest and newest values Lilu can express.
	static_cast<KernelVersion>(0),
	static_cast<KernelVersion>(255),
	[]() {
		SYSLOG("gt1urb", "GT1URBFix loaded");
		forceGT1 = checkKernelArgument("-gt1urbforce");

		// Part A: wrap initHardwareCaps when AppleIntelKBLGraphics loads.
		if (!checkKernelArgument("-gt1urbnosku")) {
			lilu.onKextLoadForce(&kextKBL, 1, [](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
				onKBLLoaded(patcher, index, address, size);
			});
		}

		// Part B: once Lilu's kernel patcher is ready, route _cs_validate_page to ours.
		if (!checkKernelArgument("-gt1urbnouser")) {
			lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
				KernelPatcher::RouteRequest route("_cs_validate_page", csValidatePage, orgCsValidatePage);
				if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1))
					SYSLOG("gt1urb", "failed to route _cs_validate_page");
				else
					SYSLOG("gt1urb", "routed _cs_validate_page");
			});
		}
	}
};
