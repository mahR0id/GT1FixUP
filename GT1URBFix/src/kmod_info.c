#include <mach/mach_types.h>
#include <libkern/libkern.h>

extern kern_return_t GT1URBFix_kern_start(kmod_info_t *, void *);
extern kern_return_t GT1URBFix_kern_stop(kmod_info_t *, void *);
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);

__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.mah.GT1URBFix, "3.1.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = GT1URBFix_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = GT1URBFix_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
