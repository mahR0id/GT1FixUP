//
//  GT1URBFix: make macOS treat a Comet Lake GT1 iGPU (e.g. 0x9BA4, i5-10200H)
//  as what it is, so the Intel Metal/GL drivers stop overflowing its URB.
//
//  THE PROBLEM
//  macOS has no driver for GT1 ids, so OpenCore spoofs a supported GT2 id
//  (0x9BC4). The kernel driver (AppleIntelKBLGraphics) then reports "sku 1"
//  (GT2) to the user-space drivers, which size the URB (the on-chip buffer for
//  vertex-shader output) for GT2: 256 KB. A GT1 only has 128 KB, so vertices
//  past the end are lost and most triangles are never drawn (broken text,
//  missing UI, flicker).
//
//  THE FIX
//  IntelAccelerator::initHardwareCaps() reads the sku from the accelerator and
//  copies that row of its g_cSkuGlobals table into the hardware caps that user
//  space reads. We wrap that function (found by symbol name) and set the sku to
//  0 (GT1) first, when the GPU's real device-id is a Comet Lake GT1. The kernel
//  then reports genuine GT1 caps and the drivers pick the GT1 URB themselves.
//
//  No per-version data: the sku field offset is read from the function's own
//  first instruction. Checked on macOS 15.7.9 and every stable 26.x KDK
//  (26.0 to 26.7.2); there is no macOS version limit.
//
//  The Metal driver also keeps the GT2 URB size as constants in its own tables
//  (3DSTATE_URB_VS payload 0x08030380 = 896 entries). The sku fix recomputes the
//  common pipeline, but other pipeline configurations (used by Chrome, for
//  example) still take these constants, so we patch them to 0x08030100 (256
//  entries) as the driver's pages are loaded. The payload is a hardware command
//  encoding, so the same bytes are expected on every macOS version; patterns
//  that are not found do nothing.
//
//  Boot-args:
//    -gt1urboff     disable the plugin
//    -gt1urbdbg     debug logging (DEBUG builds of Lilu)
//    -gt1urbforce   apply the fix even if the real device-id is not a known GT1
//
//  Check that it worked (no root needed):
//    ioreg -p IOService -n IOResources -l | grep GT1URBFix
//  Expected: "GT1URBFix-hook" = "routed initHardwareCaps", sku-before 1, sku-after 0.
//

#include <IOKit/IOService.h>
#include <Headers/kern_api.hpp>
#include <Headers/kern_user.hpp>
#include <Headers/plugin_start.hpp>

// ---------------------------------------------------------------------------
// Part 1: URB constants in the Metal driver
// ---------------------------------------------------------------------------

// Each pair: stock bytes, replacement (only 0x0380 -> 0x0100 differs).
struct UrbPatch { const char *name; uint8_t find[20]; uint8_t repl[20]; size_t len; };
static const UrbPatch urbPatches[] {
	// IGRenderStateURB::writeInit: movabsq $0x0803038078300000, %rcx
	{"writeInit", {0x48,0xB9,0x00,0x00,0x30,0x78,0x80,0x03,0x03,0x08}, {0x48,0xB9,0x00,0x00,0x30,0x78,0x00,0x01,0x03,0x08}, 10},
	// command template: URB_VS header + payload, then the URB_HS header
	{"template",  {0x00,0x00,0x30,0x78,0x80,0x03,0x03,0x08,0x00,0x00,0x31,0x78}, {0x00,0x00,0x30,0x78,0x00,0x01,0x03,0x08,0x00,0x00,0x31,0x78}, 12},
	// default VS/HS/DS/GS payloads
	{"defaults",  {0x80,0x03,0x03,0x08,0,0,0,0x08,0,0,0,0x08,0,0,0,0x08}, {0x00,0x01,0x03,0x08,0,0,0,0x08,0,0,0,0x08,0,0,0,0x08}, 16},
	// per-pipeline-config payloads (tessellation variant, HS/DS at 256 KB)
	{"payloads",  {0x80,0x03,0x03,0x08,0,0,0,0,0,0,0,0x40,0,0,0,0,0,0,0,0x40}, {0x00,0x01,0x03,0x08,0,0,0,0,0,0,0,0x40,0,0,0,0,0,0,0,0x40}, 20},
};

static mach_vm_address_t orgCsValidatePage {};

// Runs for every code page the kernel validates. After the original, patch the
// URB constants in pages of the dyld shared cache or of an on-disk Intel driver.
static void csValidatePage(vnode_t vp, memory_object_t pager, memory_object_offset_t offset, const void *data, int *validated, int *tainted, int *nx) {
	FunctionCast(csValidatePage, orgCsValidatePage)(vp, pager, offset, data, validated, tainted, nx);

	char path[PATH_MAX];
	int len = PATH_MAX;
	if (vn_getpath(vp, path, &len) != 0)
		return;
	if (!UserPatcher::matchSharedCachePath(path) && !strstr(path, "AppleIntelKBLGraphics"))
		return;

	auto page = static_cast<uint8_t *>(const_cast<void *>(data));
	for (auto &p : urbPatches) {
		for (size_t i = 0; i + p.len <= PAGE_SIZE; i++) {
			if (page[i] == p.find[0] && memcmp(page + i, p.find, p.len) == 0) {
				memcpy(page + i, p.repl, p.len);
				DBGLOG("gt1urb", "patched %s in %s", p.name, path);
				i += p.len - 1;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Part 2: kernel sku hook
// ---------------------------------------------------------------------------

static const char *kblPath[] { "/System/Library/Extensions/AppleIntelKBLGraphics.kext/Contents/MacOS/AppleIntelKBLGraphics" };
static KernelPatcher::KextInfo kextKBL { "com.apple.driver.AppleIntelKBLGraphics", kblPath, arrsize(kblPath), {true}, {}, KernelPatcher::KextInfo::Unloaded };

static constexpr const char *symInitHardwareCaps = "__ZN16IntelAccelerator16initHardwareCapsEv";

static mach_vm_address_t orgInitHardwareCaps {};  // original function
static uint32_t skuOffset {};                     // sku field in IntelAccelerator (0x10F8 so far)
static bool forceGT1 {};                          // -gt1urbforce

// Write a status value where `ioreg` can see it (IOResources, and the accelerator).
static void setStatus(IOService *service, const char *key, const char *value) {
	if (auto res = IOService::getResourceService()) res->setProperty(key, value);
	if (service) service->setProperty(key, value);
}
static void setStatus(IOService *service, const char *key, uint32_t value) {
	if (auto res = IOService::getResourceService()) res->setProperty(key, value, 32);
	if (service) service->setProperty(key, value, 32);
}

// Real device-ids of Comet Lake GT1 iGPUs (Linux i915 CML_GT1 and CML_U_GT1).
static bool isCometLakeGT1(uint32_t id) {
	switch (id) {
		case 0x9BA2: case 0x9BA4: case 0x9BA5: case 0x9BA8:   // CML-H/S GT1
		case 0x9B21: case 0x9BAA: case 0x9BAC:                // CML-U GT1
			return true;
		default:
			return false;
	}
}

// The real device-id. Reading PCI config space returns the spoofed id (0x9BC4),
// but the device's "compatible" property keeps the hardware one as
// "pci8086,xxxx". Returns 0 if not found.
static uint32_t realDeviceId(IORegistryEntry *pci) {
	auto data = pci ? OSDynamicCast(OSData, pci->getProperty("compatible")) : nullptr;
	if (!data)
		return 0;
	auto p = static_cast<const char *>(data->getBytesNoCopy());
	size_t n = data->getLength();
	static const char prefix[] = "pci8086,";
	for (size_t i = 0; i + sizeof(prefix) - 1 < n; i++) {
		if (memcmp(p + i, prefix, sizeof(prefix) - 1) != 0)
			continue;
		uint32_t id = 0;
		for (size_t j = i + sizeof(prefix) - 1; j < n; j++) {
			char c = p[j];
			if      (c >= '0' && c <= '9') id = id * 16 + static_cast<uint32_t>(c - '0');
			else if (c >= 'a' && c <= 'f') id = id * 16 + static_cast<uint32_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') id = id * 16 + static_cast<uint32_t>(c - 'A' + 10);
			else break;
		}
		return id;
	}
	return 0;
}

// initHardwareCaps starts by loading the sku: mov r32, dword [rdi + disp32]
// (optional REX, opcode 8B, ModRM mod=10 rm=rdi). Return that disp32, or 0 if
// the code does not look as expected (then we do nothing).
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

// Our version of initHardwareCaps: on a GT1, set sku = 0, then run the original.
static bool initHardwareCaps(void *that) {
	auto accel = static_cast<IOService *>(that);
	auto sku = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(that) + skuOffset);
	uint32_t realId = realDeviceId(accel->getProvider());

	setStatus(accel, "GT1URBFix-real-device-id", realId);
	setStatus(accel, "GT1URBFix-sku-before", *sku);
	if ((forceGT1 || isCometLakeGT1(realId)) && *sku != 0 && *sku < 4) {
		SYSLOG("gt1urb", "real device-id 0x%04X, sku %u -> 0 (GT1)", realId, *sku);
		*sku = 0;
	}
	setStatus(accel, "GT1URBFix-sku-after", *sku);

	return FunctionCast(initHardwareCaps, orgInitHardwareCaps)(that);
}

// Called when AppleIntelKBLGraphics loads: find the function and wrap it.
static void onKBLLoaded(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (index != kextKBL.loadIndex)
		return;

	auto fn = patcher.solveSymbol(index, symInitHardwareCaps, address, size);
	if (!fn) {
		patcher.clearError();
		setStatus(nullptr, "GT1URBFix-hook", "initHardwareCaps not found");
		return;
	}
	skuOffset = findSkuOffset(reinterpret_cast<const uint8_t *>(fn));
	if (!skuOffset) {
		setStatus(nullptr, "GT1URBFix-hook", "unexpected initHardwareCaps code");
		return;
	}

	KernelPatcher::RouteRequest route(symInitHardwareCaps, initHardwareCaps, orgInitHardwareCaps);
	if (patcher.routeMultiple(index, &route, 1, address, size)) {
		setStatus(nullptr, "GT1URBFix-hook", "routed initHardwareCaps");
	} else {
		patcher.clearError();
		setStatus(nullptr, "GT1URBFix-hook", "failed to route initHardwareCaps");
	}
}

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
		forceGT1 = checkKernelArgument("-gt1urbforce");
		setStatus(nullptr, "GT1URBFix-hook", "waiting for AppleIntelKBLGraphics");
		lilu.onKextLoadForce(&kextKBL, 1, [](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
			onKBLLoaded(patcher, index, address, size);
		});
		lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
			KernelPatcher::RouteRequest route("_cs_validate_page", csValidatePage, orgCsValidatePage);
			if (patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1)) {
				setStatus(nullptr, "GT1URBFix-urb-patches", "routed _cs_validate_page");
			} else {
				patcher.clearError();
				setStatus(nullptr, "GT1URBFix-urb-patches", "failed to route _cs_validate_page");
			}
		});
	}
};
