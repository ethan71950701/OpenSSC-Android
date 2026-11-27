#include "hooks.h"

#include <android/log.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "guard.h"
#include <time.h>

#include "shadowhook.h"
#include "ssc_sender.h"
#include "translate.h"

#define TAG "OpenSSCProbe"
#define MAX_LOG_LINES 4000
#define CODEC_DUMP 20 /* AVDT_CODEC_SIZE in AOSP; the Xiaomi layout is unverified, so we only dump bytes */
#define STREAM_OK_SENDS 1500 /* about 30 s of 20 ms ticks */

#define hlog hooks_log
static int g_fd = -1;
static const char *g_stream_state;
static volatile int g_lines;
static volatile unsigned g_sends, g_interval_calls;
static volatile int g_stream_armed;
static volatile long g_stream_armed_at;
static volatile int g_translate;     /* set once before install */
static volatile int g_pcm_applied;   /* rate written by the last getHalPcmConfiguration call, 0 if untouched */
static volatile int g_ssc_active;    /* the configuration we last built was rewritten to SSC: mute frames */
static volatile long g_caps_seen_at; /* CLOCK_MONOTONIC seconds of the last SSC capability rewrite, 0 = never */
#define CONFIG_WINDOW_S 1800         /* capability exchange happens at connect; a codec change picked later (developer options) still needs it */

void hooks_set_translate(int on) { g_translate = on; }

/* 0.4: while an SSC configuration is active, send real SSC audio instead of muting (unless ssc_mute exists). */
static volatile int g_ssc_audio = 1;
static volatile int g_ssc_streaming; /* sender started for the current stream */
static sscs_alloc_fn g_osi_malloc;   /* the stack's allocator: the stack frees our packets with osi_free */
void hooks_set_ssc_audio(int on) { g_ssc_audio = on; }
static int g_ssc_interval_ms = 18; /* one 864-sample frame per media tick at 48 kHz; 0 keeps the stack's 20 ms */
void hooks_set_ssc_interval(int ms) { g_ssc_interval_ms = ms; }
static int g_ssc_config = TR_SSC_CONFIG_BYTE; /* 0x0C HiFi 48 kHz, or 0x0A UHQ 96 kHz */
void hooks_set_ssc_config(int byte) { g_ssc_config = byte; }

static long mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec + 1; /* never 0 */
}

/* The SSC peer's capability list is fetched AFTER the AAC one on a fresh connection, so AAC can only be hidden
 * if "an SSC peer exists" is remembered across Bluetooth restarts. We keep the wall-clock time of the last SSC
 * sighting in a small file; a sighting younger than SEEN_PERSIST_S re-arms the hiding right at process start. */
#define SEEN_PERSIST_S (24 * 3600)
static const char *g_seen_path;
static long g_seen_age; /* persisted sighting age in seconds + 1, 0 = none */

static long wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long)ts.tv_sec;
}

void hooks_set_seen_path(const char *path) {
    g_seen_path = path;
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    long t = 0;
    int ok = fscanf(f, "%ld", &t) == 1;
    fclose(f);
    long now = wall_s();
    if (ok && t > 0 && now >= t && now - t <= SEEN_PERSIST_S) {
        __atomic_store_n(&g_caps_seen_at, mono_s(), __ATOMIC_RELAXED);
        g_seen_age = now - t + 1; /* logged by hooks_install once the log file is open */
    }
}

static void save_seen(void) {
    if (!g_seen_path) return;
    FILE *f = fopen(g_seen_path, "w");
    if (!f) return;
    fprintf(f, "%ld\n", wall_s());
    fclose(f);
}

void hooks_hex(const uint8_t *p, size_t n, char *out, size_t out_cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 2 < out_cap; i++) o += (size_t)snprintf(out + o, out_cap - o, "%02x", p[i]);
    if (out_cap) out[o < out_cap ? o : out_cap - 1] = 0;
}

__attribute__((format(printf, 1, 2))) void hooks_log(const char *fmt, ...) {
    if (__atomic_fetch_add(&g_lines, 1, __ATOMIC_RELAXED) >= MAX_LOG_LINES) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    __android_log_write(ANDROID_LOG_INFO, TAG, line);
    if (g_fd >= 0) {
        line[n] = '\n';
        if (write(g_fd, line, (size_t)n + 1) < 0) { /* best effort */ }
    }
}

/* ---- AVDTP ---------------------------------------------------------------------------------- */

typedef uint8_t (*prs_cfg_t)(void *cfg, uint8_t *p, uint16_t len, uint8_t *p_elem, uint8_t sig_id);
typedef void (*bld_cfg_t)(uint8_t **p, void *cfg);
static prs_cfg_t o_prs;
static bld_cfg_t o_bld;

static uint8_t h_prs(void *cfg, uint8_t *p, uint16_t len, uint8_t *p_elem, uint8_t sig_id) {
    if (g_translate && p && len > 0) {
        /* Rewrite the received list before the stack parses it, so it sees an Opus-capable peer. */
        char before[2 * 24 + 1];
        hooks_hex(p, len < 24 ? len : 24, before, sizeof(before));
        /* The stack ranks AAC above Opus. Once an SSC peer was seen, hide AAC from later capability lists, so
         * the next connection of that peer ends up on the Opus slot. Side effect while translate mode is on:
         * other headsets also lose AAC (they use SBC). */
        long seen = __atomic_load_n(&g_caps_seen_at, __ATOMIC_RELAXED);
        if (seen != 0 && mono_s() - seen <= CONFIG_WINDOW_S) {
            int h = tr_hide_aac(p, len);
            if (h > 0) hlog("TRANSLATE hid %d AAC capability element(s) (sig=0x%02x)", h, sig_id);
        }
        int n = tr_caps_ssc_to_opus(p, len);
        if (n > 0) {
            __atomic_store_n(&g_caps_seen_at, mono_s(), __ATOMIC_RELAXED);
            save_seen();
            char after[2 * 24 + 1];
            hooks_hex(p, len < 24 ? len : 24, after, sizeof(after));
            hlog("TRANSLATE caps: %d SSC element(s) -> Opus (sig=0x%02x) %s => %s", n, sig_id, before, after);
        }
    }
    uint8_t r = o_prs(cfg, p, len, p_elem, sig_id);
    if (cfg) {
        char hex[2 * CODEC_DUMP + 1];
        hooks_hex(static_cast<const uint8_t *>(cfg), CODEC_DUMP, hex, sizeof(hex));
        hlog("avdt_msg_prs_cfg sig=0x%02x len=%u ret=%u cfg[0..%d]=%s", sig_id, len, r, CODEC_DUMP - 1, hex);
    }
    return r;
}

static void h_bld(uint8_t **pp, void *cfg) {
    uint8_t *start = pp ? *pp : nullptr;
    o_bld(pp, cfg);
    if (start && *pp >= start && *pp - start <= 64) {
        char hex[2 * 64 + 1];
        size_t n = (size_t)(*pp - start);
        hooks_hex(start, n, hex, sizeof(hex));
        hlog("avdt_msg_bld_cfg wrote %zu bytes: %s", n, hex);
    }
    if (g_translate && start && *pp > start && *pp - start <= 256) {
        /* Patch the bytes just written (not the stack's own structure): Opus config -> SSC config. */
        long seen = __atomic_load_n(&g_caps_seen_at, __ATOMIC_RELAXED);
        int recent = seen != 0 && mono_s() - seen <= CONFIG_WINDOW_S;
        int n = recent ? tr_config_opus_to_ssc(start, (size_t)(*pp - start), g_ssc_config, nullptr) : 0;
        if (n > 0) {
            g_ssc_active = 1;
            char hx[2 * 64 + 1];
            hooks_hex(start, (size_t)(*pp - start) < 64 ? (size_t)(*pp - start) : 64, hx, sizeof(hx));
            hlog("TRANSLATE config: %d Opus element(s) -> SSC 0x%02x, frames muted from now: %s", n,
                 g_ssc_config, hx);
        } else {
            /* Not a rewritten SSC configuration: if it is a config for another codec, unmute. */
            int is_other = 0;
            tr_config_opus_to_ssc(start, (size_t)(*pp - start), -1, &is_other);
            if (is_other && g_ssc_active) {
                g_ssc_active = 0;
                hlog("TRANSLATE config for another codec built, mute cleared");
            }
        }
    }
}

/* ---- Opus encoder interface ----------------------------------------------------------------- */

typedef void (*enc_init_t)(const void *peer, void *cfg, void *read_cb, void *enq_cb);
typedef void (*void_fn_t)(void);
typedef uint64_t (*interval_t)(void);
typedef void (*send_t)(uint64_t ts);
typedef void (*mtu_t)(uintptr_t handle, const void *addr, uint16_t mtu);
static enc_init_t o_init;
static void_fn_t o_cleanup, o_reset, o_flush;
static interval_t o_interval;
static send_t o_send;
static mtu_t o_mtu;

static void stream_ok(void) {
    if (__atomic_exchange_n(&g_stream_armed, 0, __ATOMIC_ACQ_REL) && g_stream_state) {
        guard_success(g_stream_state);
        hlog("stream survived, stream crash counter reset");
    }
}

void hooks_stream_tick_at(long now) {
    if (g_stream_armed && now - g_stream_armed_at >= 60) stream_ok();
}
void hooks_stream_tick(void) { hooks_stream_tick_at(mono_s()); }

static void h_init(const void *peer, void *cfg, void *read_cb, void *enq_cb) {
    /* tA2DP_ENCODER_INIT_PEER_PARAMS: bool is_peer_edr; bool peer_supports_3mbps; uint16_t peer_mtu */
    const uint8_t *pp = static_cast<const uint8_t *>(peer);
    if (pp)
        hlog("opus_encoder_init edr=%u 3mbps=%u mtu=%u", pp[0], pp[1], (unsigned)(pp[2] | (pp[3] << 8)));
    else
        hlog("opus_encoder_init peer=null");
    __atomic_store_n(&g_sends, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_interval_calls, 0, __ATOMIC_RELAXED);
    if (g_stream_state && guard_begin(g_stream_state) > 0) {
        g_stream_armed_at = mono_s();
        g_stream_armed = 1;
    }
    o_init(peer, cfg, read_cb, enq_cb);
    g_ssc_streaming = 0;
    if (g_translate && g_ssc_active && g_ssc_audio) {
        uint16_t mtu = pp ? (uint16_t)(pp[2] | (pp[3] << 8)) : 0;
        sscs_set_pcm_rate(g_pcm_applied); /* true 96 kHz input only if the PCM rate override really took effect */
        int r = g_osi_malloc ? sscs_start(reinterpret_cast<sscs_read_fn>(read_cb), reinterpret_cast<sscs_enqueue_fn>(enq_cb),
                                         g_osi_malloc, mtu)
                             : -9;
        if (r > 0) {
            g_ssc_streaming = 1;
            sscs_stats st;
            sscs_get_stats(&st);
            hlog("SSC audio ON: %d kb/s, %d Hz, frame %d bytes, %d frames per packet, ts step %d, mtu %u", st.bitrate / 1000,
                 st.sample_rate, st.frame_bytes, st.frames_per_packet, st.ts_step, mtu);
        } else {
            hlog("SSC audio not started (%d): frames stay muted", r);
        }
    }
}

static void log_ssc_stats(const char *when) {
    sscs_stats st;
    sscs_get_stats(&st);
    hlog("SSC %s: ticks=%u frames=%u packets=%u underflows=%u enqueue_failed=%u", when, st.ticks, st.frames, st.packets,
         st.underflows, st.enqueue_failed);
}

static void h_cleanup(void) {
    unsigned sends = __atomic_load_n(&g_sends, __ATOMIC_RELAXED);
    hlog("opus_encoder_cleanup (frames sent since init: %u)", sends);
    if (g_ssc_streaming) {
        g_ssc_streaming = 0;
        sscs_stop();
        log_ssc_stats("stream end");
    }
    o_cleanup();
    if (sends > 0) stream_ok();
}
static void h_reset(void) {
    hlog("opus_feeding_reset");
    if (g_ssc_streaming) sscs_reset();
    o_reset();
}
static void h_flush(void) {
    hlog("opus_feeding_flush");
    if (g_ssc_streaming) sscs_reset();
    o_flush();
}
static uint64_t h_interval(void) {
    uint64_t r = o_interval();
    unsigned n = __atomic_fetch_add(&g_interval_calls, 1, __ATOMIC_RELAXED);
    /* SSC sends one frame per packet (Buds firmware rule). The stack moves at most one packet per media tick, so the
     * tick must match the frame duration (18 ms) or the queue backs up and the stream gets suspended. */
    uint64_t out = g_ssc_streaming && g_ssc_interval_ms > 0 ? (uint64_t)g_ssc_interval_ms : r;
    if (n < 3) hlog("opus_get_encoder_interval_ms -> %llu%s", (unsigned long long)out, out != r ? " (SSC override)" : "");
    return out;
}
static void h_send(uint64_t ts) {
    unsigned n = __atomic_fetch_add(&g_sends, 1, __ATOMIC_RELAXED) + 1;
    int ssc = g_translate && g_ssc_active;
    if (ssc && g_ssc_streaming) {
        int k = sscs_tick(ts);
        /* every 100 ticks (2 s): input level per frame, so gaps can be placed before or after the encoder */
        if (n % 250 == 0 && n <= 20000) {
            char tr[128];
            sscs_take_trace(tr, sizeof tr);
            hlog("SSC level #%u [%s]", n, tr);
        }
        if (n <= 3 || n % 500 == 0) {
            hlog("opus_send_frames #%u ts=%llu SSC sent %d packet(s)", n, (unsigned long long)ts, k);
            log_ssc_stats("running");
        }
    } else {
        if (n <= 3 || n % 500 == 0)
            hlog("opus_send_frames #%u ts=%llu%s", n, (unsigned long long)ts, ssc ? " MUTED (original not called)" : "");
        if (!ssc) o_send(ts);
    }
    if (n == STREAM_OK_SENDS) stream_ok();
}
/* bool A2dpCodecs::setCodecUserConfig(const btav_a2dp_codec_config_t& cfg, peer_params*, peer_caps*, result*,
 * bool* restart_input, bool* restart_output, bool* config_updated). btav_a2dp_codec_config_t starts with
 * int32 codec_type, int32 codec_priority. On this phone something disables the Opus slot (priority -1) seconds after
 * it is chosen; while an SSC peer is active we refuse that one request (return false: the caller treats it as a
 * failed preference change and keeps the current codec). */
typedef uintptr_t (*ucfg_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static ucfg_t o_ucfg;
#define OPUS_CODEC_TYPE 6 /* matches mCodecType 6 in dumpsys */
static uintptr_t h_ucfg(uintptr_t self, uintptr_t cfg, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6,
                        uintptr_t a7) {
    int32_t type = -99, prio = -99;
    if (cfg) {
        memcpy(&type, (const void *)cfg, 4);
        memcpy(&prio, (const void *)(cfg + 4), 4);
    }
    long seen = __atomic_load_n(&g_caps_seen_at, __ATOMIC_RELAXED);
    int recent = seen != 0 && mono_s() - seen <= CONFIG_WINDOW_S;
    if (g_translate && recent && type == OPUS_CODEC_TYPE && prio == -1) {
        hlog("TRANSLATE blocked setCodecUserConfig(type=%d prio=%d): keeping the Opus slot for the SSC peer", type, prio);
        return 0;
    }
    uintptr_t r = o_ucfg(self, cfg, a2, a3, a4, a5, a6, a7);
    hlog("setCodecUserConfig(type=%d prio=%d) -> %lu", type, prio, (unsigned long)(r & 0xff));
    return r;
}

/* bool A2DP_VendorBuildCodecHeader(const uint8_t* codec_info, BT_HDR* p_buf, uint16_t frames). For Opus it prepends a
 * 1-byte media payload header; SSC packets carry frames only (S25+ capture: 12 + 2 x 484 = 980), so skip it. */
typedef uintptr_t (*bhdr_t)(const uint8_t *, void *, uint16_t);
static bhdr_t o_bhdr;
static uintptr_t h_bhdr(const uint8_t *info, void *buf, uint16_t frames) {
    if (g_ssc_streaming && info && info[2] == 0xff && info[3] == 0xe0 && info[4] == 0 && info[5] == 0 && info[6] == 0)
        return 1;
    return o_bhdr(info, buf, frames);
}

/* Base constructor of every codec config: the name passed here is what the stack reports (dumpsys, the codec types
 * handed to Java). In translate mode the Opus source slot (index 6) carries SSC, so it is named "SSC". */
typedef void (*ccfg_t)(void *, int, uint64_t, const void *, int);
static ccfg_t o_ccfg;
static const char *g_codec_name = "SSC";
static void h_ccfg(void *self, int index, uint64_t codec_id, const void *name, int prio) {
    /* libc++ short string, as the caller builds it inline: byte 0 = size << 1, characters from byte 1. */
    alignas(8) unsigned char ssc[24] = {0};
    const unsigned char *n = static_cast<const unsigned char *>(name);
    if (g_translate && index == 6 && n && n[0] == 8 && memcmp(n + 1, "Opus", 4) == 0) {
        size_t len = strlen(g_codec_name);
        ssc[0] = (unsigned char)(len << 1);
        memcpy(ssc + 1, g_codec_name, len);
        hlog("codec name: Opus slot reported as %s", g_codec_name);
        o_ccfg(self, index, codec_id, ssc, prio);
        return;
    }
    o_ccfg(self, index, codec_id, name, prio);
}

/* The constructor above runs once, when the stack builds its codec list. If that happens before these hooks are in,
 * the Opus config object already exists with its original name. The two Opus config methods below run on every
 * connect and codec change with that object as `self`, so rename it in place there. Layout (verified in the
 * constructor): codec index at +0x30, libc++ std::string name at +0x40. */
static void rename_opus_self(void *self) {
    if (!g_translate || !self) return;
    unsigned char *o = static_cast<unsigned char *>(self);
    int32_t idx;
    memcpy(&idx, o + 0x30, 4);
    unsigned char *n = o + 0x40;
    if (idx != 6 || n[0] != 8 || memcmp(n + 1, "Opus", 4) != 0) return;
    size_t len = strlen(g_codec_name);
    unsigned char ssc[24] = {0};
    ssc[0] = (unsigned char)(len << 1);
    memcpy(ssc + 1, g_codec_name, len);
    memcpy(n, ssc, 24);
    hlog("codec name: existing Opus config renamed to %s", g_codec_name);
}
typedef uintptr_t (*ocfg_t)(void *, uintptr_t, uintptr_t, uintptr_t);
static ocfg_t o_ocset, o_ocpeer;
static uintptr_t h_ocset(void *self, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    rename_opus_self(self);
    return o_ocset(self, a1, a2, a3);
}
static uintptr_t h_ocpeer(void *self, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    rename_opus_self(self);
    return o_ocpeer(self, a1, a2, a3);
}

/* bool codec::getHalPcmConfiguration(const ahal_codec_configuration&, PcmConfiguration* out): the PCM format of the
 * stack for the audio HAL (sampleRateHz at +0, channelMode +4, bitsPerSample +5, dataIntervalUs +8). The A2DP output
 * thread follows what is reported here (dynamic rates), so overriding the rate changes what AudioFlinger delivers. */
static volatile int g_pcm_hz;
void hooks_set_pcm_hz(int hz) { g_pcm_hz = hz; }
typedef uintptr_t (*hpcm_t)(const void *, void *);
static hpcm_t o_hpcm;
static uintptr_t h_hpcm(const void *cfg, void *out) {
    uintptr_t r = o_hpcm(cfg, out);
    int hz = g_pcm_hz;
    g_pcm_applied = 0;
    /* Only while the SSC (Opus slot) configuration is the active one, and only when the stack describes a 48 kHz
     * source (sample rate enum 2 at +0x48 of the codec configuration, which Opus always is). A fallback codec such as
     * SBC must keep its own rate, or it would be fed 96 kHz data and play slowly. */
    int32_t src_rate = 0;
    if (cfg) memcpy(&src_rate, static_cast<const uint8_t *>(cfg) + 0x48, 4);
    if (g_translate && g_ssc_active && src_rate == 2 && hz > 0 && out && (r & 0xff)) {
        int32_t before;
        memcpy(&before, out, 4);
        int32_t now = hz;
        memcpy(out, &now, 4);
        g_pcm_applied = hz;
        hlog("PCM config: sample rate %d -> %d Hz for the audio HAL", before, now);
    }
    return r;
}

static void h_mtu(uintptr_t handle, const void *addr, uint16_t mtu) {
    hlog("bta_av_co_audio_update_mtu mtu=%u", mtu); /* address deliberately not logged */
    o_mtu(handle, addr, mtu);
}

/* ---- installation --------------------------------------------------------------------------- */

struct hook_def {
    const char *name;
    int required;
    void *replacement;
    void **original;
    void *stub;
};

static hook_def g_defs[] = {
    {"avdt_msg_prs_cfg", 1, (void *)h_prs, (void **)&o_prs, nullptr},
    {"avdt_msg_bld_cfg", 1, (void *)h_bld, (void **)&o_bld, nullptr},
    {"a2dp_vendor_opus_encoder_init", 1, (void *)h_init, (void **)&o_init, nullptr},
    {"a2dp_vendor_opus_encoder_cleanup", 1, (void *)h_cleanup, (void **)&o_cleanup, nullptr},
    {"a2dp_vendor_opus_feeding_reset", 1, (void *)h_reset, (void **)&o_reset, nullptr},
    {"a2dp_vendor_opus_feeding_flush", 1, (void *)h_flush, (void **)&o_flush, nullptr},
    {"a2dp_vendor_opus_get_encoder_interval_ms", 1, (void *)h_interval, (void **)&o_interval, nullptr},
    {"a2dp_vendor_opus_send_frames", 1, (void *)h_send, (void **)&o_send, nullptr},
    {"bta_av_co_audio_update_mtu", 0, (void *)h_mtu, (void **)&o_mtu, nullptr},
    {"A2dpCodecs::setCodecUserConfig", 0, (void *)h_ucfg, (void **)&o_ucfg, nullptr},
    {"_ZN15A2dpCodecConfigC2E23btav_a2dp_codec_index_tN9bluetooth4a2dp7CodecIdERKNSt3__112basic_stringIcNS4_11char_traitsIcEENS4_9allocatorIcEEEE26btav_a2dp_codec_priority_t", 0, (void *)h_ccfg, (void **)&o_ccfg, nullptr},
    {"A2dpCodecConfigOpusBase::setCodecConfig", 0, (void *)h_ocset, (void **)&o_ocset, nullptr},
    {"A2dpCodecConfigOpusBase::setPeerCodecCapabilities", 0, (void *)h_ocpeer, (void **)&o_ocpeer, nullptr},
    {"bluetooth::audio::aidl::a2dp::codec::getHalPcmConfiguration", 0, (void *)h_hpcm, (void **)&o_hpcm, nullptr},
    {"A2DP_VendorBuildCodecHeader", 0, (void *)h_bhdr, (void **)&o_bhdr, nullptr},
};
#define DEF_COUNT (sizeof(g_defs) / sizeof(g_defs[0]))

static const sr_target *find_target(const sr_target *t, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(t[i].name, name) == 0) return &t[i];
    return nullptr;
}

static void rollback(void) {
    for (size_t i = 0; i < DEF_COUNT; i++)
        if (g_defs[i].stub) {
            shadowhook_unhook(g_defs[i].stub);
            g_defs[i].stub = nullptr;
        }
}

int hooks_install(uintptr_t bias, const sr_target *targets, size_t count, const char *log_path,
                  const char *stream_state_path) {
    g_stream_state = stream_state_path;
    g_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600);
    if (g_fd < 0) hlog("warning: cannot write hook log file, logcat only");
    hlog("hooks: translate mode %s", g_translate ? "ON" : "off");

    /* Decide first, install second: nothing is touched unless every required target is unique. */
    uintptr_t addr[DEF_COUNT] = {};
    for (size_t i = 0; i < DEF_COUNT; i++) {
        const sr_target *t = find_target(targets, count, g_defs[i].name);
        if (!t || t->matches != 1) {
            if (g_defs[i].required) {
                hlog("hooks: NOT installed, required target %s is %s", g_defs[i].name,
                     !t ? "not in the list" : t->matches == 0 ? "missing" : "ambiguous");
                return -1;
            }
            hlog("hooks: optional target %s skipped", g_defs[i].name);
            continue;
        }
        addr[i] = bias + (uintptr_t)t->match[0].value;
    }

    {
        const sr_target *m = find_target(targets, count, "osi_malloc");
        g_osi_malloc = m && m->matches == 1 ? reinterpret_cast<sscs_alloc_fn>(bias + (uintptr_t)m->match[0].value) : nullptr;
        if (!g_osi_malloc) hlog("hooks: osi_malloc not resolved, SSC audio unavailable (frames stay muted)");
    }

    int rc = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, true) /* debuggable: ShadowHook logs to logcat (tag "shadowhook") */;
    if (rc != 0) {
        hlog("hooks: NOT installed, shadowhook_init failed: %d (%s)", rc, shadowhook_to_errmsg(rc));
        return -2;
    }
    for (size_t i = 0; i < DEF_COUNT; i++) {
        if (!addr[i]) continue;
        g_defs[i].stub = shadowhook_hook_func_addr((void *)addr[i], g_defs[i].replacement, g_defs[i].original);
        if (!g_defs[i].stub) {
            int e = shadowhook_get_errno();
            hlog("hooks: %s failed: %d (%s)", g_defs[i].name, e, shadowhook_to_errmsg(e));
            if (g_defs[i].required) {
                rollback();
                hlog("hooks: rolled back, nothing active");
                return -3;
            }
            continue;
        }
        hlog("hooked %s at base+0x%llx", g_defs[i].name, (unsigned long long)(addr[i] - bias));
    }
    hlog("hooks: installed (log-only, original behaviour preserved)");
    if (g_seen_age) hlog("SSC peer seen %lds ago (persisted): AAC hiding armed from the first connection", g_seen_age - 1);
    return 0;
}
