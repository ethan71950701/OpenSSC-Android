/* Host test for hooks.cpp: the hook library is replaced by a recorder, the "Bluetooth functions" by test
 * functions. Checks that every hook forwards its arguments and result unchanged, that nothing is installed when a
 * required target is missing, and that a failing install rolls everything back. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guard.h"
#include "hooks.h"
#include "shadowhook.h"

static int g_logcat_lines;
int __android_log_write(int, const char *, const char *) { return ++g_logcat_lines; }

/* --- recorder in place of ShadowHook --- */
struct rec { void *func, *repl, **orig; int live; };
static rec g_rec[32];
static int g_nrec, g_fail_at = -1, g_init_rc, g_unhooked;
int shadowhook_init(shadowhook_mode_t, bool) { return g_init_rc; }
void *shadowhook_hook_func_addr(void *f, void *n, void **o) {
    if (g_nrec == g_fail_at) return nullptr;
    g_rec[g_nrec] = {f, n, o, 1};
    *o = f; /* the "original" is the function itself */
    return &g_rec[g_nrec++];
}
int shadowhook_unhook(void *stub) { static_cast<rec *>(stub)->live = 0; g_unhooked++; return 0; }
int shadowhook_get_errno(void) { return 99; }
const char *shadowhook_to_errmsg(int) { return "stub"; }

/* --- fake Bluetooth functions --- */
static int calls[16];
static uint8_t last_sig;
extern "C" {
uint8_t f_prs(void *cfg, uint8_t *p, uint16_t len, uint8_t *e, uint8_t sig) { (void)cfg; (void)p; (void)e; calls[0]++; last_sig = sig; return (uint8_t)(len + 1); }
void f_bld(uint8_t **p, void *cfg) { (void)cfg; calls[1]++; for (int i = 0; i < 9; i++) *(*p)++ = (uint8_t)(0x10 + i); }
void f_init(const void *a, void *b, void *c, void *d) { (void)a; (void)b; (void)c; (void)d; calls[2]++; }
void f_cleanup(void) { calls[3]++; }
void f_reset(void) { calls[4]++; }
void f_flush(void) { calls[5]++; }
uint64_t f_interval(void) { calls[6]++; return 20; }
void f_send(uint64_t ts) { calls[7]++; assert(ts == 123456789012ULL); }
void f_mtu(uintptr_t h, const void *a, uint16_t m) { (void)a; calls[8]++; assert(h == 0xdeadbeef00000042ULL && m == 1005); }
}

static sr_target T(const char *n, void *fn, int req, int matches = 1) {
    sr_target t = {};
    t.name = n; t.required = req; t.matches = matches; t.match[0].value = (uint64_t)(uintptr_t)fn;
    return t;
}
static void fill(sr_target *t) {
    t[0] = T("avdt_msg_prs_cfg", (void *)f_prs, 1);
    t[1] = T("avdt_msg_bld_cfg", (void *)f_bld, 1);
    t[2] = T("a2dp_vendor_opus_encoder_init", (void *)f_init, 1);
    t[3] = T("a2dp_vendor_opus_encoder_cleanup", (void *)f_cleanup, 1);
    t[4] = T("a2dp_vendor_opus_feeding_reset", (void *)f_reset, 1);
    t[5] = T("a2dp_vendor_opus_feeding_flush", (void *)f_flush, 1);
    t[6] = T("a2dp_vendor_opus_get_encoder_interval_ms", (void *)f_interval, 1);
    t[7] = T("a2dp_vendor_opus_send_frames", (void *)f_send, 1);
    t[8] = T("bta_av_co_audio_update_mtu", (void *)f_mtu, 0);
}
static rec *find(void *fn) {
    for (int i = 0; i < g_nrec; i++) if (g_rec[i].func == fn) return &g_rec[i];
    return nullptr;
}

int main(int, char **argv) {
    char dir[] = "/tmp/openssc_hooks_XXXXXX";
    assert(mkdtemp(dir));
    char log[256], st[256];
    snprintf(log, sizeof log, "%s/log", dir); snprintf(st, sizeof st, "%s/stream", dir);
    (void)argv;

    /* 1. hex helper */
    uint8_t b[3] = {0x00, 0xab, 0xff}; char hex[16];
    hooks_hex(b, 3, hex, sizeof hex); assert(strcmp(hex, "00abff") == 0);
    hooks_hex(b, 3, hex, 5); assert(strcmp(hex, "00ab") == 0); /* truncates, stays terminated */

    /* 2. required target ambiguous: nothing may be touched */
    sr_target t[9]; fill(t); t[3].matches = 2;
    assert(hooks_install(0, t, 9, log, st) == -1 && g_nrec == 0);
    fill(t); t[7].matches = 0;
    assert(hooks_install(0, t, 9, log, st) == -1 && g_nrec == 0);

    /* 3. hook library cannot initialise: nothing installed */
    fill(t); g_init_rc = 5;
    assert(hooks_install(0, t, 9, log, st) == -2 && g_nrec == 0);
    g_init_rc = 0;

    /* 4. a required hook fails midway: everything installed so far is removed */
    fill(t); g_fail_at = 4;
    assert(hooks_install(0, t, 9, log, st) == -3);
    assert(g_unhooked == 4);
    for (int i = 0; i < g_nrec; i++) assert(!g_rec[i].live);
    g_nrec = 0; g_fail_at = -1; g_unhooked = 0;

    /* 5. optional target missing: still installs the 8 required hooks */
    fill(t); t[8].matches = 0;
    assert(hooks_install(0, t, 9, log, st) == 0 && g_nrec == 8);
    g_nrec = 0;

    /* 6. full install, then every wrapper forwards unchanged */
    fill(t);
    assert(hooks_install(0, t, 9, log, st) == 0 && g_nrec == 9);
    typedef uint8_t (*prs_t)(void *, uint8_t *, uint16_t, uint8_t *, uint8_t);
    uint8_t cfg[40] = {7, 0, 0xff, 0x75}, p[4], e;
    assert(((prs_t)find((void *)f_prs)->repl)(cfg, p, 41, &e, 0x0c) == 42 && calls[0] == 1 && last_sig == 0x0c);
    assert(((prs_t)find((void *)f_prs)->repl)(nullptr, p, 1, &e, 1) == 2); /* null cfg tolerated */
    uint8_t buf[32] = {0}, *w = buf;
    ((void (*)(uint8_t **, void *))find((void *)f_bld)->repl)(&w, cfg);
    assert(w == buf + 9 && buf[0] == 0x10 && buf[8] == 0x18 && calls[1] == 1);
    ((void (*)(const void *, void *, void *, void *))find((void *)f_init)->repl)(cfg, 0, 0, 0);
    ((void (*)(void))find((void *)f_cleanup)->repl)();
    ((void (*)(void))find((void *)f_reset)->repl)();
    ((void (*)(void))find((void *)f_flush)->repl)();
    assert(((uint64_t(*)(void))find((void *)f_interval)->repl)() == 20);
    for (int i = 0; i < 5; i++) ((void (*)(uint64_t))find((void *)f_send)->repl)(123456789012ULL);
    ((void (*)(uintptr_t, const void *, uint16_t))find((void *)f_mtu)->repl)(0xdeadbeef00000042ULL, 0, 1005);
    for (int i = 2; i <= 8; i++) assert(calls[i] == (i == 7 ? 5 : 1));

    /* 7. stream guard: init arms it (count 1), a clean cleanup after sends resets it */
    assert(guard_count(st) == 1); /* armed by the init in step 6; frames were sent but no cleanup yet */
    ((void (*)(void))find((void *)f_cleanup)->repl)(); /* clean stop after frames: marker reset */
    assert(guard_count(st) == 0);
    ((void (*)(const void *, void *, void *, void *))find((void *)f_init)->repl)(cfg, 0, 0, 0);
    assert(guard_count(st) == 1);
    ((void (*)(uint64_t))find((void *)f_send)->repl)(123456789012ULL);
    ((void (*)(void))find((void *)f_cleanup)->repl)();
    assert(guard_count(st) == 0);
    /* cleanup called from inside init (before any frame) must not reset it */
    ((void (*)(const void *, void *, void *, void *))find((void *)f_init)->repl)(cfg, 0, 0, 0);
    ((void (*)(void))find((void *)f_cleanup)->repl)();
    assert(guard_count(st) == 1);

    /* 8. the hook log has content, and never a MAC (the address argument is not read) */
    FILE *f = fopen(log, "r"); assert(f);
    char line[600]; int n = 0, saw_bld = 0, saw_mtu = 0;
    while (fgets(line, sizeof line, f)) { n++; if (strstr(line, "wrote 9 bytes: 101112131415161718")) saw_bld = 1; if (strstr(line, "update_mtu mtu=1005")) saw_mtu = 1; }
    fclose(f);
    assert(n > 10 && saw_bld && saw_mtu && g_logcat_lines > 10);
    printf("hooks host test: OK (%d log lines)\n", n);
    return 0;
}
