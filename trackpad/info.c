#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t trackpadModulStart(kmod_info_t *ki, void *data);
extern kern_return_t trackpadModulStop(kmod_info_t *ki, void *data);

__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(local.trackpad, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = trackpadModulStart;   /* sysctl debug.trackpad anmelden */
__private_extern__ kmod_stop_func_t *_antimain = trackpadModulStop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
