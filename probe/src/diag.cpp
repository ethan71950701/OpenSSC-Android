#include "diag.h"

#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include "hooks.h"
#include "shadowhook.h"

#define DIAG_MAX_LINES_PER_SLOT 40

typedef uintptr_t (*fn6_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static char g_slot_name[DIAG_MAX][DIAG_NAME_MAX];
static fn6_t g_slot_orig[DIAG_MAX];
static void *g_slot_stub[DIAG_MAX];
static volatile unsigned g_slot_calls[DIAG_MAX];

/* Reads up to len bytes at an arbitrary pointer without risking a crash (process_vm_readv on ourselves reports EFAULT
 * instead of faulting). Used to look inside config structs passed by reference. */
static size_t safe_read(uintptr_t p, void *out, size_t len) {
    p &= 0x00ffffffffffffffULL; /* drop the heap tag byte */
    if (p < 0x10000) return 0;
    struct iovec l = {out, len}, r = {(void *)p, len};
    long n = syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0);
    return n > 0 ? (size_t)n : 0;
}
static void dump_mem(const char *name, int idx, uintptr_t p) {
    unsigned char b[48];
    size_t n = safe_read(p, b, sizeof b);
    if (!n) return;
    char hex[sizeof b * 2 + 1];
    hooks_hex(b, n, hex, sizeof hex);
    hooks_log("DIAG   %s arg%d mem[0..%zu]=%s", name, idx, n, hex);
}
static bool wants_mem(const char *name) {
    return strstr(name, "Pcm") || strstr(name, "setup_codec") || strstr(name, "UpdateAudioConfig") ||
           strstr(name, "StartSession");
}

template <int N>
static uintptr_t slot_hook(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5) {
    uintptr_t r = g_slot_orig[N](a0, a1, a2, a3, a4, a5);
    unsigned c = __atomic_fetch_add(&g_slot_calls[N], 1, __ATOMIC_RELAXED);
    if (c < DIAG_MAX_LINES_PER_SLOT)
        hooks_log("DIAG %s(%llx,%llx,%llx,%llx,%llx,%llx) -> %llx", g_slot_name[N], (unsigned long long)a0,
                  (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)a4,
                  (unsigned long long)a5, (unsigned long long)r);
    if (c < DIAG_MAX_LINES_PER_SLOT && wants_mem(g_slot_name[N])) {
        dump_mem(g_slot_name[N], 0, a0);
        dump_mem(g_slot_name[N], 1, a1);
    }
    return r;
}

static void *const g_slot_fn[DIAG_MAX] = {
    (void *)slot_hook<0>, (void *)slot_hook<1>, (void *)slot_hook<2>, (void *)slot_hook<3>, (void *)slot_hook<4>,
    (void *)slot_hook<5>, (void *)slot_hook<6>, (void *)slot_hook<7>, (void *)slot_hook<8>, (void *)slot_hook<9>,
};

size_t diag_parse_names(const char *text, size_t len, char out[][DIAG_NAME_MAX]) {
    size_t n = 0, i = 0;
    while (i < len && n < DIAG_MAX) {
        size_t s = i;
        while (i < len && text[i] != '\n') i++;
        size_t e = i;
        if (i < len) i++;
        while (s < e && (text[s] == ' ' || text[s] == '\t' || text[s] == '\r')) s++;
        while (e > s && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r')) e--;
        if (s >= e || text[s] == '#' || e - s >= DIAG_NAME_MAX) continue;
        memcpy(out[n], text + s, e - s);
        out[n][e - s] = 0;
        n++;
    }
    return n;
}

int diag_install(uintptr_t bias, const sr_target *targets, size_t count, char names[][DIAG_NAME_MAX], size_t n) {
    int installed = 0;
    for (size_t k = 0; k < n && k < DIAG_MAX; k++) {
        const sr_target *t = nullptr;
        for (size_t i = 0; i < count; i++)
            if (strcmp(targets[i].name, names[k]) == 0) t = &targets[i];
        if (!t || t->matches != 1) {
            hooks_log("diag: %s skipped (%s)", names[k], !t ? "not scanned" : t->matches == 0 ? "missing" : "ambiguous");
            continue;
        }
        memcpy(g_slot_name[k], names[k], DIAG_NAME_MAX);
        g_slot_stub[k] = shadowhook_hook_func_addr((void *)(bias + (uintptr_t)t->match[0].value), g_slot_fn[k],
                                                   (void **)&g_slot_orig[k]);
        if (!g_slot_stub[k]) {
            hooks_log("diag: %s hook failed: %d (%s)", names[k], shadowhook_get_errno(),
                      shadowhook_to_errmsg(shadowhook_get_errno()));
            continue;
        }
        hooks_log("diag: hooked %s at base+0x%llx", names[k], (unsigned long long)t->match[0].value);
        installed++;
    }
    return installed;
}
