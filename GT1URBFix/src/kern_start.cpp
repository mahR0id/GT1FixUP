//
//  GT1URBFix: stop the Intel Metal/GL drivers from overflowing the URB of a
//  Comet Lake GT1 iGPU (e.g. 0x9BA4, i5-10200H).
//
//  THE PROBLEM
//  macOS has no driver for GT1 ids, so OpenCore spoofs a supported GT2 id
//  (0x9BC4). The kernel driver then reports "sku 1" (GT2), and the user-space
//  Metal and OpenGL drivers size the URB (the on-chip buffer for vertex-shader
//  output) for GT2: 256 KB. A GT1 only has 128 KB, so vertices past the end are
//  lost and most triangles are never drawn (broken text, missing UI, flicker).
//
//  THE FIX
//  Patch only the URB sizes in the user-space drivers, as their code pages are
//  loaded. The kernel and the hardware caps it reports are left untouched.
//  (v2/v3 changed the reported sku instead; that hung the GPU on macOS 26, both
//  with the GT1 caps row and with sku 0 on the GT2 row.)
//
//  The drivers live in the dyld shared cache (macOS 11+), which cannot be
//  edited on disk, so we route the kernel's _cs_validate_page, which runs for
//  every code page as it is mapped, and patch the bytes right after validation
//  (the technique RestrictEvents uses). The six patterns below were checked to
//  exist unchanged in macOS 15.7.9 and 26.7.2. A pattern that is not found does
//  nothing, so an unknown macOS version just stays unpatched.
//
//  Boot-args:
//    -gt1urboff     disable the plugin
//    -gt1urbdbg     debug logging (DEBUG builds of Lilu)
//
//  Check that it is active (no root needed):
//    ioreg -p IOService -n IOResources -l | grep GT1URBFix
//  Expected: "GT1URBFix-urb-patches" = "routed _cs_validate_page".
//  tests/urb_patch_check shows which patterns are patched in the running drivers.
//

#include <IOKit/IOService.h>
#include <Headers/kern_api.hpp>
#include <Headers/kern_user.hpp>
#include <Headers/plugin_start.hpp>

// Each patch: stock bytes and their replacement (same length).
struct UrbPatch { const char *name; uint8_t find[20]; uint8_t repl[20]; size_t len; };

static const UrbPatch urbPatches[] {
	// Metal, IGRenderStateURB::writeInit: movabsq $0x0803038078300000, %rcx
	// 0x78300000 = 3DSTATE_URB_VS header, 0x08030380 = payload with 896 entries.
	// Replacement payload 0x08030100 = 256 entries (32 KB..96 KB).
	{"writeInit", {0x48,0xB9,0x00,0x00,0x30,0x78,0x80,0x03,0x03,0x08},
	              {0x48,0xB9,0x00,0x00,0x30,0x78,0x00,0x01,0x03,0x08}, 10},
	// Metal, command template: URB_VS header + payload, then the URB_HS header.
	{"template",  {0x00,0x00,0x30,0x78,0x80,0x03,0x03,0x08,0x00,0x00,0x31,0x78},
	              {0x00,0x00,0x30,0x78,0x00,0x01,0x03,0x08,0x00,0x00,0x31,0x78}, 12},
	// Metal, default VS/HS/DS/GS payloads.
	{"defaults",  {0x80,0x03,0x03,0x08,0,0,0,0x08,0,0,0,0x08,0,0,0,0x08},
	              {0x00,0x01,0x03,0x08,0,0,0,0x08,0,0,0,0x08,0,0,0,0x08}, 16},
	// Metal, per-pipeline-config payloads (tessellation variant, HS/DS at 256 KB).
	{"payloads",  {0x80,0x03,0x03,0x08,0,0,0,0,0,0,0,0x40,0,0,0,0,0,0,0,0x40},
	              {0x00,0x01,0x03,0x08,0,0,0,0,0,0,0,0x40,0,0,0,0,0,0,0,0x40}, 20},
	// Metal, -[MTLIGRenderPipelineState getUrbSizeInUrbAllocations] returns
	// 16 + (hwCaps->sku != 0) * 16 (8 KB units), and setupURBState: recomputes
	// every pipeline from it. Force the GT1 result (16 = 128 KB):
	// xor eax,eax; cmp dword [rcx+0x10],0; setne al -> nop x3; shl eax,4; add eax,0x10
	{"urbSize",   {0x31,0xC0,0x83,0x79,0x10,0x00,0x0F,0x95,0xC0,0xC1,0xE0,0x04,0x83,0xC0,0x10},
	              {0x31,0xC0,0x83,0x79,0x10,0x00,0x90,0x90,0x90,0xC1,0xE0,0x04,0x83,0xC0,0x10}, 15},
	// OpenGL, updateURBState: setupURBState(ctx, (ctx->sku != 0) << 4 | 0xC), i.e.
	// 28 for GT2 or 12 for GT1. Force 12:
	// xor esi,esi; cmp dword [rbx+0x2F40],0; setne sil -> nop x4; shl esi,4; or esi,0xC
	{"glUrbSize", {0x31,0xF6,0x83,0xBB,0x40,0x2F,0x00,0x00,0x00,0x40,0x0F,0x95,0xC6,0xC1,0xE6,0x04,0x83,0xCE,0x0C},
	              {0x31,0xF6,0x83,0xBB,0x40,0x2F,0x00,0x00,0x00,0x90,0x90,0x90,0x90,0xC1,0xE6,0x04,0x83,0xCE,0x0C}, 19},
};

static mach_vm_address_t orgCsValidatePage {};

// Runs for every code page the kernel validates. After the original, patch the
// URB patterns in pages of the dyld shared cache or of an on-disk Intel driver
// bundle (macOS 10.15 and older keep the drivers on disk).
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

static void setStatus(const char *key, const char *value) {
	if (auto res = IOService::getResourceService())
		res->setProperty(key, value);
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
		setStatus("GT1URBFix-urb-patches", "waiting for the kernel patcher");
		lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
			KernelPatcher::RouteRequest route("_cs_validate_page", csValidatePage, orgCsValidatePage);
			if (patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1)) {
				setStatus("GT1URBFix-urb-patches", "routed _cs_validate_page");
			} else {
				patcher.clearError();
				setStatus("GT1URBFix-urb-patches", "failed to route _cs_validate_page");
			}
		});
	}
};
