/* __clear_cache for arm64 (same sequence as compiler-rt's). The NDK build gets it from the builtins
 * archive; this copy only matters for builds without the NDK, where nothing else defines it. */
#include <stddef.h>
#include <stdint.h>

__attribute__((visibility("hidden"))) void __clear_cache(void *begin, void *end) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t dline = (uintptr_t)4 << ((ctr >> 16) & 15), iline = (uintptr_t)4 << (ctr & 15);
    uintptr_t s = (uintptr_t)begin, e = (uintptr_t)end, a;
    for (a = s & ~(dline - 1); a < e; a += dline) __asm__ volatile("dc cvau, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (a = s & ~(iline - 1); a < e; a += iline) __asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}
