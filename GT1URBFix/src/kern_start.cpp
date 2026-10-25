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
#include <Headers/plugin_start.hpp>

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
	}
};
