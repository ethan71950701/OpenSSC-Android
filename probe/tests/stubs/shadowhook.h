/* Host-test stand-in: only the part of the ShadowHook API that hooks.cpp uses (real header is arm64-only). */
#pragma once
#include <stdbool.h>
typedef enum { SHADOWHOOK_MODE_SHARED = 0, SHADOWHOOK_MODE_UNIQUE = 1, SHADOWHOOK_MODE_MULTI = 2 } shadowhook_mode_t;
int shadowhook_init(shadowhook_mode_t default_mode, bool debuggable);
void *shadowhook_hook_func_addr(void *func_addr, void *new_addr, void **orig_addr);
int shadowhook_unhook(void *stub);
int shadowhook_get_errno(void);
const char *shadowhook_to_errmsg(int error_number);
