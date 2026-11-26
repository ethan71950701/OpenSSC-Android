/* Host test for hooks.cpp: the hook library is replaced by a recorder, the "Bluetooth functions" by test
 * functions. Checks that every hook forwards its arguments and result unchanged, that nothing is installed when a
 * required target is missing, and that a failing install rolls everything back. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "guard.h"
#include "diag.h"
#include "hooks.h"
#include "translate.h"
#include "shadowhook.h"

static int g_logcat_lines;
int __android_log_write(int, const char *, const char *) { return ++g_logcat_lines; }

/* --- recorder in place of ShadowHook --- */
struct rec { void *func, *repl, **orig; int live; };
static rec g_rec[32];
static int g_nrec, g_fail_at = -1, g_init_rc, g_unhooked;
int shadowhook_init(shadowhook_mode_t, bool) { return g_init_rc; }
void *shadowhook_hook_func_addr(void *f, void *n, void **o) {
    if (g_nrec == g_fail_at) { g_fail_at = -1; return nullptr; } /* fails once */
    g_rec[g_nrec] = {f, n, o, 1};
    *o = f; /* the "original" is the function itself */
    return &g_rec[g_nrec++];
}
int shadowhook_unhook(void *stub) { static_cast<rec *>(stub)->live = 0; g_unhooked++; return 0; }
int shadowhook_get_errno(void) { return 99; }
const char *shadowhook_to_errmsg(int) { return "stub"; }

/* --- fake Bluetooth functions --- */
static int calls[16];
extern "C" uintptr_t f_diag_a(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { calls[10]++; return a + b + c + d + e + f; }
extern "C" uintptr_t f_diag_b(uintptr_t a, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) { calls[11]++; return a ^ 1; }
static uint8_t last_sig, seen_p[32];
static int bld_mode, capture_p; /* 0: nine test bytes, 1: Opus config list, 2: AAC config list */
extern "C" {
uint8_t f_prs(void *cfg, uint8_t *p, uint16_t len, uint8_t *e, uint8_t sig) { (void)cfg; (void)p; (void)e; calls[0]++; last_sig = sig; if (p && capture_p) memcpy(seen_p, p, len < 32 ? len : 32); return (uint8_t)(len + 1); }
void f_bld(uint8_t **p, void *cfg) {
    (void)cfg; calls[1]++;
    static const uint8_t opus[] = {0x01, 0x00, 0x07, 0x09, 0x00, 0xff, 0xe0, 0, 0, 0, 0x01, 0x00, 0x92, 0x08, 0x00};
    static const uint8_t aac[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x02, 0x80, 0x01, 0x8c, 0x84, 0xe2, 0x08, 0x00};
    if (bld_mode == 1) { memcpy(*p, opus, sizeof opus); *p += sizeof opus; }
    else if (bld_mode == 2) { memcpy(*p, aac, sizeof aac); *p += sizeof aac; }
    else for (int i = 0; i < 9; i++) *(*p)++ = (uint8_t)(0x10 + i);
}
void f_init(const void *a, void *b, void *c, void *d) { (void)a; (void)b; (void)c; (void)d; calls[2]++; }
void f_cleanup(void) { calls[3]++; }
void f_reset(void) { calls[4]++; }
void f_flush(void) { calls[5]++; }
uint64_t f_interval(void) { calls[6]++; return 20; }
void f_send(uint64_t ts) { calls[7]++; assert(ts == 123456789012ULL); }
extern "C" uintptr_t f_ucfg(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t h) { calls[9]++; return h; }
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
    t[9] = T("A2dpCodecs::setCodecUserConfig", (void *)f_ucfg, 0);
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
    sr_target t[10]; fill(t); t[3].matches = 2;
    assert(hooks_install(0, t, 10, log, st) == -1 && g_nrec == 0);
    fill(t); t[7].matches = 0;
    assert(hooks_install(0, t, 10, log, st) == -1 && g_nrec == 0);

    /* 3. hook library cannot initialise: nothing installed */
    fill(t); g_init_rc = 5;
    assert(hooks_install(0, t, 10, log, st) == -2 && g_nrec == 0);
    g_init_rc = 0;

    /* 4. a required hook fails midway: everything installed so far is removed */
    fill(t); g_fail_at = 4;
    assert(hooks_install(0, t, 10, log, st) == -3);
    assert(g_unhooked == 4);
    for (int i = 0; i < g_nrec; i++) assert(!g_rec[i].live);
    g_nrec = 0; g_fail_at = -1; g_unhooked = 0;

    /* 5. optional target missing: still installs the 8 required hooks */
    fill(t); t[8].matches = 0; t[9].matches = 0;
    assert(hooks_install(0, t, 10, log, st) == 0 && g_nrec == 8);
    g_nrec = 0;

    /* 6. full install, then every wrapper forwards unchanged */
    fill(t);
    assert(hooks_install(0, t, 10, log, st) == 0 && g_nrec == 10);
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

    /* 9. wire-level translation helpers, with the capability bytes the Buds 3 Pro sent a Xiaomi 13 */
    {
        const uint8_t caps[] = {0x01, 0x00, 0x07, 0x09, 0x00, 0xff, 0x75, 0, 0, 0, 0x03, 0x01, 0x3e, 0x08, 0x00};
        const uint8_t want[] = {0x01, 0x00, 0x07, 0x09, 0x00, 0xff, 0xe0, 0, 0, 0, 0x01, 0x00, 0x92, 0x08, 0x00};
        uint8_t b2[sizeof caps]; memcpy(b2, caps, sizeof caps);
        assert(tr_caps_ssc_to_opus(b2, sizeof b2) == 1 && memcmp(b2, want, sizeof want) == 0);
        assert(tr_caps_ssc_to_opus(b2, sizeof b2) == 0); /* already Opus: nothing to do */
        memcpy(b2, caps, sizeof caps);
        assert(tr_caps_ssc_to_opus(b2, 11) == 0 && memcmp(b2, caps, sizeof caps) == 0); /* truncated: untouched */
        assert(tr_caps_ssc_to_opus(nullptr, 5) == 0 && tr_caps_ssc_to_opus(b2, 0) == 0);
        b2[12] = 0x00; /* SSC element without any SSC mode bit: refused, untouched */
        assert(tr_caps_ssc_to_opus(b2, sizeof b2) == 0 && b2[6] == 0x75);
        uint8_t cfg[] = {0x01, 0x00, 0x07, 0x09, 0x00, 0xff, 0xe0, 0, 0, 0, 0x01, 0x00, 0x92, 0x08, 0x00};
        int other = 9;
        assert(tr_config_opus_to_ssc(cfg, sizeof cfg, TR_SSC_CONFIG_BYTE, &other) == 1 && other == 0);
        assert(cfg[6] == 0x75 && cfg[10] == 0x03 && cfg[11] == 0x01 && cfg[12] == 0x0c && cfg[13] == 0x08);
        uint8_t aac2[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x02, 0x80, 0x01, 0x8c, 0x84, 0xe2, 0x08, 0x00};
        assert(tr_hide_aac(aac2, sizeof aac2) == 1 && aac2[5] == 0x0e && aac2[3] == 0x06 && aac2[6] == 0x80);
        assert(tr_hide_aac(aac2, sizeof aac2) == 0);
        uint8_t sbc[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x3f, 0xff, 0x02, 0x34, 0x08, 0x00};
        assert(tr_hide_aac(sbc, sizeof sbc) == 0 && sbc[5] == 0x00);
        uint8_t cfg10[] = {0x07, 0x09, 0x00, 0xff, 0xe0, 0, 0, 0, 0x01, 0x00, 0x8a}; /* 10 ms: unsupported */
        assert(tr_config_opus_to_ssc(cfg10, sizeof cfg10, 0x0c, &other) == 0 && cfg10[4] == 0xe0 && other == 0);
        uint8_t aac[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x02, 0x80, 0x01, 0x8c, 0x84, 0xe2};
        assert(tr_config_opus_to_ssc(aac, sizeof aac, 0x0c, &other) == 0 && other == 1); /* AAC: other codec */
    }

    /* 10. translate mode in the hooks: rewrite before parse, rewrite after build, mute, unmute */
    {
        typedef uint8_t (*prs_t)(void *, uint8_t *, uint16_t, uint8_t *, uint8_t);
        typedef void (*bld_t)(uint8_t **, void *);
        typedef void (*send_t)(uint64_t);
        prs_t prs = (prs_t)find((void *)f_prs)->repl;
        bld_t bld = (bld_t)find((void *)f_bld)->repl;
        send_t send = (send_t)find((void *)f_send)->repl;
        uint8_t peer[] = {0x01, 0x00, 0x07, 0x09, 0x00, 0xff, 0x75, 0, 0, 0, 0x03, 0x01, 0x3e, 0x08, 0x00}, e;
        uint8_t buf2[40], *w;
        int sends_before;

        capture_p = 1;
        /* off (default): nothing is rewritten and frames flow */
        prs(cfg, peer, sizeof peer, &e, 0x0c);
        assert(seen_p[6] == 0x75 && peer[6] == 0x75);
        bld_mode = 1; w = buf2; bld(&w, cfg);
        assert(buf2[6] == 0xe0 && buf2[12] == 0x92);
        sends_before = calls[7]; send(123456789012ULL); assert(calls[7] == sends_before + 1);

        hooks_set_translate(1);
        /* build before any capability was rewritten: not an SSC peer, untouched */
        bld_mode = 1; w = buf2; bld(&w, cfg);
        assert(buf2[6] == 0xe0);
        sends_before = calls[7]; send(123456789012ULL); assert(calls[7] == sends_before + 1);

        /* Buds capability: the stack's parser sees Opus; the sighting is persisted */
        const char *seen_file = "/tmp/openssc_test_ssc_seen";
        remove(seen_file);
        hooks_set_seen_path(seen_file);
        prs(cfg, peer, sizeof peer, &e, 0x0c);
        {
            FILE *sf = fopen(seen_file, "r");
            long tv = 0;
            assert(sf && fscanf(sf, "%ld", &tv) == 1 && tv > 1000000000L);
            fclose(sf);
        }
        assert(seen_p[6] == 0xe0 && seen_p[10] == 0x01 && seen_p[12] == 0x92 && seen_p[13] == 0x08);
        /* A later AAC capability list (next connection) is hidden before parsing; SBC is left alone */
        uint8_t aaclist[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x02, 0x80, 0x01, 0x8c, 0x84, 0xe2, 0x08, 0x00};
        prs(cfg, aaclist, sizeof aaclist, &e, 0x0c);
        assert(seen_p[5] == 0x0e);
        uint8_t sbclist[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x3f, 0xff, 0x02, 0x34, 0x08, 0x00};
        prs(cfg, sbclist, sizeof sbclist, &e, 0x0c);
        assert(seen_p[5] == 0x00);
        /* Set Configuration: wire bytes become SSC 0x0C, frames are muted */
        bld_mode = 1; w = buf2; bld(&w, cfg);
        assert(w == buf2 + 15 && buf2[6] == 0x75 && buf2[10] == 0x03 && buf2[12] == 0x0c && buf2[13] == 0x08);
        sends_before = calls[7]; send(123456789012ULL); send(123456789012ULL);
        assert(calls[7] == sends_before); /* muted: original not called */
        /* a configuration for another codec (e.g. AAC to other earbuds) lifts the mute */
        bld_mode = 2; w = buf2; bld(&w, cfg);
        assert(buf2[3] == 0x06 && buf2[6] == 0x80); /* AAC bytes untouched */
        send(123456789012ULL); assert(calls[7] == sends_before + 1);
        /* the Opus-disable request is refused while an SSC peer is active; other requests pass */
        typedef uintptr_t (*ucfg_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
        ucfg_t ucfg = (ucfg_t)find((void *)f_ucfg)->repl;
        int32_t opus_off[4] = {6, -1, 0, 0}, sbc_on[4] = {0, 1001, 0, 0};
        int before = calls[9];
        assert(ucfg(0, (uintptr_t)opus_off, 0, 0, 0, 0, 0, 1) == 0 && calls[9] == before);
        assert(ucfg(0, (uintptr_t)sbc_on, 0, 0, 0, 0, 0, 1) == 1 && calls[9] == before + 1);
        bld_mode = 0;
        hooks_set_translate(0);
        assert(ucfg(0, (uintptr_t)opus_off, 0, 0, 0, 0, 0, 1) == 1 && calls[9] == before + 2);
    }

    /* 11. diagnostics hooks: names from a file, forwarding, skipping, failure isolation */
    {
        const char txt[] = "# comment\n  a2dp::setup_codec  \n\nbtif_x\r\nmissing_fn\nambiguous_fn\nnot_scanned\n";
        char names[DIAG_MAX][DIAG_NAME_MAX];
        size_t n = diag_parse_names(txt, sizeof txt - 1, names);
        assert(n == 5 && strcmp(names[0], "a2dp::setup_codec") == 0 && strcmp(names[1], "btif_x") == 0);
        char many[600] = {0};
        for (int i = 0; i < 30; i++) strcat(many, "f\n");
        assert(diag_parse_names(many, strlen(many), names) == DIAG_MAX); /* capped */
        n = diag_parse_names(txt, sizeof txt - 1, names);

        sr_target t[4] = {};
        t[0] = T("a2dp::setup_codec", (void *)f_diag_a, 0);
        t[1] = T("btif_x", (void *)f_diag_b, 0);
        t[2] = T("missing_fn", nullptr, 0, 0);
        t[3] = T("ambiguous_fn", nullptr, 0, 2);
        g_nrec = 0;
        assert(diag_install(0, t, 4, names, n) == 2 && g_nrec == 2);
        typedef uintptr_t (*d6_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
        d6_t ha = (d6_t)find((void *)f_diag_a)->repl, hb = (d6_t)find((void *)f_diag_b)->repl;
        assert(ha(1, 2, 3, 4, 5, 0xdeadbeefcafe0000ULL) == 15 + 0xdeadbeefcafe0000ULL && calls[10] == 1);
        assert(hb(8, 0, 0, 0, 0, 0) == 9 && calls[11] == 1);
        for (int i = 0; i < 100; i++) ha(0, 0, 0, 0, 0, 0); /* log is capped per slot; calls still go through */
        assert(calls[10] == 101);
        /* one failing diag hook must not stop the others */
        g_nrec = 0; g_fail_at = 0;
        assert(diag_install(0, t, 4, names, n) == 1 && g_nrec == 1);
        g_fail_at = -1;
    }

    /* 12. stream guard upkeep: armed by init, cleared once 60 s have passed, even without any frames */
    {
        typedef void (*init_t)(const void *, void *, void *, void *);
        sr_target t2[10]; fill(t2); g_nrec = 0; g_fail_at = -1; /* the diag step above reused the recorder */
        assert(hooks_install(0, t2, 10, log, st) == 0);
        assert(guard_count(st) >= 0);
        ((void (*)(void))find((void *)f_cleanup)->repl)(); /* may reset an earlier marker */
        guard_success(st);
        ((init_t)find((void *)f_init)->repl)(cfg, 0, 0, 0);
        assert(guard_count(st) == 1);
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        long now = (long)ts.tv_sec + 1;
        hooks_stream_tick_at(now + 30); assert(guard_count(st) == 1); /* too early */
        hooks_stream_tick_at(now + 61); assert(guard_count(st) == 0); /* survived */
        hooks_stream_tick_at(now + 200); assert(guard_count(st) == 0);
        /* repeated starts with no frames, as in the field, must not pile up once ticks run */
        for (int i = 0; i < 5; i++) {
            ((init_t)find((void *)f_init)->repl)(cfg, 0, 0, 0);
            hooks_stream_tick_at(now + 100 + i * 100);
        }
        assert(guard_count(st) == 0);
    }
    printf("hooks host test: OK (%d log lines)\n", n);
    return 0;
}
