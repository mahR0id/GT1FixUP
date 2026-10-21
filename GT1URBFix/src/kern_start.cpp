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
//  THE FIX
//  Patch the driver code so it programs 256 entries (0x100: 32 KB..96 KB) instead
//  of 896, and make the drivers' own "how big is the URB" helpers return the GT1
//  size. There are several places that must all agree (see the tables below);
//  missing any one leaves one pipeline configuration broken.
//
//  HOW IT HOOKS IN
//  The drivers live in the dyld shared cache, which cannot be edited on disk. So
//  this plugin (built on Lilu) routes the kernel's _cs_validate_page, the function
//  that code-signs each page as it is mapped, and patches the page bytes right
//  after it is validated. This is the same technique RestrictEvents uses.
//
//  Boot-args: -gt1urboff disables the plugin, -gt1urbdbg enables debug logging,
//  -gt1urbbeta allows loading on an unsupported macOS version.
//

#include <IOKit/IOService.h>
#include <Headers/kern_api.hpp>
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
// cache, apply all patches to the in-memory copy.
static void csValidatePage(vnode_t vp, memory_object_t pager, memory_object_offset_t page_offset, const void *data, int *validated_p, int *tainted_p, int *nx_p) {
	FunctionCast(csValidatePage, orgCsValidatePage)(vp, pager, page_offset, data, validated_p, tainted_p, nx_p);

	char path[PATH_MAX];
	int pathlen = PATH_MAX;
	if (vn_getpath(vp, path, &pathlen) != 0 || !UserPatcher::matchSharedCachePath(path))
		return;

	auto page = const_cast<void *>(data);
	replaceAll(page, PAGE_SIZE, findWriteInit, replWriteInit, sizeof(findWriteInit), "writeInit", path);
	replaceAll(page, PAGE_SIZE, findTemplate, replTemplate, sizeof(findTemplate), "urb template", path);
	replaceAll(page, PAGE_SIZE, findDefaults, replDefaults, sizeof(findDefaults), "urb defaults", path);
	replaceAll(page, PAGE_SIZE, findPayloads, replPayloads, sizeof(findPayloads), "urb payloads", path);
	replaceAll(page, PAGE_SIZE, findUrbSize, replUrbSize, sizeof(findUrbSize), "getUrbSizeInUrbAllocations", path);
	replaceAll(page, PAGE_SIZE, findGLUrbSize, replGLUrbSize, sizeof(findGLUrbSize), "GL updateURBState", path);
}

// Lilu plugin boilerplate: boot-args and supported macOS range (Sequoia only,
// since the byte patterns are specific to the 15.x drivers).
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
	KernelVersion::Sequoia,
	KernelVersion::Sequoia,
	[]() {
		SYSLOG("gt1urb", "GT1URBFix loaded");
		// Once Lilu's kernel patcher is ready, route _cs_validate_page to ours.
		lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
			KernelPatcher::RouteRequest route("_cs_validate_page", csValidatePage, orgCsValidatePage);
			if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1))
				SYSLOG("gt1urb", "failed to route _cs_validate_page");
			else
				SYSLOG("gt1urb", "routed _cs_validate_page");
		});
	}
};
