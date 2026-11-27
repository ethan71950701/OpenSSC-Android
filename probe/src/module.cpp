/* OpenSSC probe (stage 3 adds an opt-in translate mode, see hooks.cpp; stage 2): runs inside com.android.bluetooth only, waits for the Bluetooth stack
 * library, looks up the functions the hook design needs and writes a report. When every required
 * function is found it installs log-only hooks (see hooks.cpp): they call the original function with
 * the original arguments and return its result unchanged. It sets no properties and changes no
 * Bluetooth data. */
#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "guard.h"
#include "diag.h"
#include "hooks.h"
#include "ssc_sender.h"
#include "symres.h"
#include "zygisk.hpp"

#define TAG "OpenSSCProbe"
#define VERSION "0.5.7"
#define TARGET_PROCESS "com.android.bluetooth"
#define TARGET_UID 1002
#define TARGET_LIB "libbluetooth_jni.so"
#define DATA_DIR "/data/user_de/0/com.android.bluetooth"
#define STATE_FILE DATA_DIR "/openssc_probe_state"
#define REPORT_FILE DATA_DIR "/openssc_probe_report.txt"
#define HOOKLOG_FILE DATA_DIR "/openssc_probe_hooklog.txt"
#define STREAM_STATE_FILE DATA_DIR "/openssc_probe_stream_state"
#define WAIT_SECONDS 120
#define SURVIVE_SECONDS 15 /* was 60: quick Bluetooth off/on cycles during testing locked the probe */

#define BASE_TARGETS 42
static sr_target g_targets[BASE_TARGETS + DIAG_MAX] = {
    /* required by the first hook version */
    {"avdt_msg_prs_cfg", 1},
    {"avdt_msg_bld_cfg", 1},
    {"a2dp_vendor_opus_encoder_init", 1},
    {"a2dp_vendor_opus_encoder_cleanup", 1},
    {"a2dp_vendor_opus_feeding_reset", 1},
    {"a2dp_vendor_opus_feeding_flush", 1},
    {"a2dp_vendor_opus_get_encoder_interval_ms", 1},
    {"a2dp_vendor_opus_send_frames", 1},
    /* informational */
    {"a2dp_vendor_opus_get_effective_frame_size", 0},
    {"a2dp_vendor_opus_set_transmit_queue_length", 0},
    {"a2dp_opus_encode_frames", 0},
    {"A2DP_ParseInfoOpus", 0},
    {"A2DP_BuildInfoOpus", 0},
    {"A2DP_IsCodecValidOpus", 0},
    {"A2DP_IsVendorSinkCodecSupportedOpus", 0},
    {"A2DP_VendorCodecTypeEqualsOpus", 0},
    {"A2DP_VendorCodecEqualsOpus", 0},
    {"A2DP_VendorGetBitRateOpus", 0},
    {"A2DP_VendorGetFrameSizeOpus", 0},
    {"A2DP_VendorGetTrackSampleRateOpus", 0},
    {"A2DP_VendorGetTrackChannelCountOpus", 0},
    {"A2DP_VendorGetTrackBitsPerSampleOpus", 0},
    {"A2DP_VendorCodecInfoStringOpus", 0},
    {"A2dpCodecConfigOpusBase::setCodecConfig", 0},
    {"A2dpCodecConfigOpusBase::setPeerCodecCapabilities", 0},
    {"A2DP_VendorSourceCodecIndex", 0},
    {"A2DP_VendorCodecTypeEquals", 0},
    {"A2DP_VendorGetEncoderInterface", 0},
    {"A2DP_VendorBuildCodecHeader", 0},
    {"A2DP_VendorUsesRtpHeader", 0},
    {"A2DP_VendorCodecGetCodecId", 0},
    {"A2DP_SourceCodecIndex", 0},
    {"bta_av_co_audio_getconfig", 0},
    {"bta_av_co_audio_setconfig", 0},
    {"bta_av_co_save_codec", 0},
    {"bta_av_co_audio_update_mtu", 0},
    {"A2dpCodecs::setCodecUserConfig", 0},
    {"osi_malloc", 0},
    /* A2dpCodecConfig::A2dpCodecConfig(index, CodecId, const std::string& name, priority) (base constructor) */
    {"_ZN15A2dpCodecConfigC2E23btav_a2dp_codec_index_tN9bluetooth4a2dp7CodecIdERKNSt3__112basic_stringIcNS4_11char_traitsIcEENS4_9allocatorIcEEEE26btav_a2dp_codec_priority_t", 0},
    {"A2dpCodecConfigOpusBase::setCodecConfig", 0},
    {"A2dpCodecConfigOpusBase::setPeerCodecCapabilities", 0},
    {"bluetooth::audio::aidl::a2dp::codec::getHalPcmConfiguration", 0},
};
static size_t g_target_count = BASE_TARGETS; /* grows by the diagnostic names read from diag_targets.txt */
#define TARGET_COUNT g_target_count
static char g_diag_names[DIAG_MAX][DIAG_NAME_MAX];
static size_t g_diag_count;

static int g_report_fd = -1;
static bool g_translate_allowed = false; /* enable_translate file present */
static int g_ssc_ts_step = -1, g_ssc_fpp = 0, g_ssc_interval = 18, g_ssc_uhq = 0, g_ssc_kbps = 0, g_ssc_pcm_hz = 0; /* ssc_ts_step / ssc_fpp files (0.4.2 experiments) */
static bool g_ssc_mute = false;

/* Bitrates the Buds 3 Pro firmware accepts (frame length table in the Linux port's notes); anything else: default. */
static int ssc_allowed_bps(int uhq, int kbps) {
    static const int hifi[] = {88, 96, 128, 192, 229}, uhq_k[] = {152, 291, 584};
    const int *t = uhq ? uhq_k : hifi;
    int n = uhq ? 3 : 5;
    for (int i = 0; i < n; i++)
        if (t[i] == kbps) return kbps * 1000;
    return 0;
}

/* Small decimal number from a module file; def when missing or unreadable. */
static int read_int_file(int dir, const char *name, int def) {
    if (dir < 0) return def;
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return def;
    char b[16] = {0};
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r <= 0 || b[0] < '0' || b[0] > '9') return def;
    int v = 0;
    for (ssize_t i = 0; i < r && b[i] >= '0' && b[i] <= '9' && v < 100000; i++) v = v * 10 + (b[i] - '0');
    return v;
}          /* ssc_mute file present: keep muting instead of sending SSC */
static bool g_hooks_allowed = false; /* set in preAppSpecialize, read by the probe thread */

__attribute__((format(printf, 1, 2))) static void out(const char *fmt, ...) {
    char line[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    __android_log_write(ANDROID_LOG_INFO, TAG, line);
    if (g_report_fd >= 0) dprintf(g_report_fd, "%s\n", line);
}

/* Logs a line and appends it to the report file (the report is closed by the time hooks are decided). */
static void note(const char *line) {
    __android_log_write(ANDROID_LOG_INFO, TAG, line);
    int fd = open(REPORT_FILE, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd >= 0) {
        dprintf(fd, "%s\n", line);
        close(fd);
    }
}

struct lib_info {
    char path[512];
    uintptr_t bias;
    const ElfW(Phdr) *phdr;
    int phnum;
};

static int find_lib(struct dl_phdr_info *info, size_t, void *arg) {
    const char *name = info->dlpi_name;
    if (!name) return 0;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (strcmp(base, TARGET_LIB) != 0) return 0;
    lib_info *li = static_cast<lib_info *>(arg);
    snprintf(li->path, sizeof(li->path), "%s", name);
    li->bias = info->dlpi_addr;
    li->phdr = info->dlpi_phdr;
    li->phnum = info->dlpi_phnum;
    return 1;
}

static void print_build_id(const lib_info &li) {
    for (int i = 0; i < li.phnum; i++) {
        if (li.phdr[i].p_type != PT_NOTE) continue;
        const uint8_t *p = reinterpret_cast<const uint8_t *>(li.bias + li.phdr[i].p_vaddr);
        const uint8_t *end = p + li.phdr[i].p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const ElfW(Nhdr) *n = reinterpret_cast<const ElfW(Nhdr) *>(p);
            const uint8_t *name = p + sizeof(*n);
            const uint8_t *desc = name + ((n->n_namesz + 3) & ~3u);
            const uint8_t *next = desc + ((n->n_descsz + 3) & ~3u);
            if (next > end || next <= p) break;
            if (n->n_type == NT_GNU_BUILD_ID && n->n_namesz == 4 && memcmp(name, "GNU", 4) == 0) {
                char hex[129] = {0};
                for (unsigned k = 0; k < n->n_descsz && k < 64; k++) snprintf(hex + 2 * k, 3, "%02x", desc[k]);
                out("build-id: %s", hex);
                return;
            }
            p = next;
        }
    }
    out("build-id: not found");
}

static int in_exec_segment(const lib_info &li, uint64_t vaddr) {
    for (int i = 0; i < li.phnum; i++)
        if (li.phdr[i].p_type == PT_LOAD && (li.phdr[i].p_flags & PF_X) && vaddr >= li.phdr[i].p_vaddr &&
            vaddr - li.phdr[i].p_vaddr < li.phdr[i].p_memsz)
            return 1;
    return 0;
}

static bool resolve(const lib_info &li) {
    out("library: %s", li.path);
    print_build_id(li);

    int fd = open(li.path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
        out("RESULT: FAIL cannot open library file");
        if (fd >= 0) close(fd);
        return false;
    }
    size_t len = static_cast<size_t>(st.st_size);
    void *map = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        out("RESULT: FAIL mmap");
        return false;
    }
    int rc = sr_scan(static_cast<const uint8_t *>(map), len, g_targets, TARGET_COUNT);
    munmap(map, len);
    if (rc != SR_OK) {
        out("RESULT: FAIL symbol scan error %d", rc);
        return false;
    }

    int required = 0, required_ok = 0, found = 0;
    for (size_t i = 0; i < TARGET_COUNT; i++) {
        const sr_target &t = g_targets[i];
        const char *state = t.matches == 0 ? "MISSING" : t.matches == 1 ? "OK" : "AMBIGUOUS";
        out("%-9s %s%s (matches=%d)", state, t.required ? "* " : "  ", t.name, t.matches);
        int unique_exec = 0;
        for (int k = 0; k < t.matches && k < SR_MAX_MATCH; k++) {
            uint64_t v = t.match[k].value;
            int exec = in_exec_segment(li, v);
            out("    off=0x%llx size=%llu exec=%d %s", (unsigned long long)v,
                (unsigned long long)t.match[k].size, exec, t.match[k].symbol);
            if (t.matches == 1) unique_exec = exec;
        }
        found += unique_exec;
        if (t.required) {
            required++;
            required_ok += unique_exec;
        }
    }
    out("RESULT: %s required=%d/%d all=%d/%zu", required_ok == required ? "PASS" : "INCOMPLETE", required_ok,
        required, found, TARGET_COUNT);
    return required_ok == required;
}

static void *probe_thread(void *) {
    lib_info li = {};
    int waited = 0;
    while (!dl_iterate_phdr(find_lib, &li) && waited < WAIT_SECONDS * 2) {
        usleep(500 * 1000);
        waited++;
    }
    g_report_fd = open(REPORT_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    out("OpenSSC probe %s, pid %d", VERSION, getpid());
    if (g_report_fd < 0) out("warning: cannot write report file, logcat only");
    for (size_t k = 0; k < g_diag_count; k++) {
        sr_target &t = g_targets[g_target_count++];
        t = sr_target();
        t.name = g_diag_names[k];
        t.required = 0;
    }
    bool pass = false;
    if (li.path[0])
        pass = resolve(li);
    else
        out("RESULT: FAIL %s not loaded after %d s", TARGET_LIB, WAIT_SECONDS);
    if (g_report_fd >= 0) close(g_report_fd);
    g_report_fd = -1;

    if (!pass)
        note("hooks: not installed (symbol report is not PASS)");
    else if (!g_hooks_allowed)
        note("hooks: not installed (disable_hooks file present)");
    else if (guard_count(STREAM_STATE_FILE) < 0 || guard_count(STREAM_STATE_FILE) >= GUARD_LIMIT)
        note("hooks: not installed (stream crash limit reached or state unreadable; "
             "delete openssc_probe_stream_state to re-enable)");
    else
    {
        hooks_set_translate(g_translate_allowed);
        hooks_set_ssc_audio(!g_ssc_mute);
        sscs_set_options(g_ssc_ts_step, g_ssc_fpp);
        hooks_set_ssc_interval(g_ssc_interval);
        sscs_set_mode(g_ssc_uhq);
        sscs_set_bitrate(ssc_allowed_bps(g_ssc_uhq, g_ssc_kbps));
        hooks_set_ssc_config(g_ssc_uhq ? 0x0A : 0x0C);
        hooks_set_pcm_hz(g_ssc_pcm_hz);
        if (g_translate_allowed) hooks_set_seen_path(DATA_DIR "/openssc_probe_ssc_seen");
        __android_log_print(ANDROID_LOG_INFO, TAG, "translate mode: %s", g_translate_allowed ? (g_ssc_mute ? "ON (SSC<->Opus rewrite, frames muted)" : "ON (SSC<->Opus rewrite, SSC audio)") : "off (log only)");
        if (hooks_install(li.bias, g_targets, TARGET_COUNT, HOOKLOG_FILE, STREAM_STATE_FILE) == 0 && g_diag_count > 0)
            diag_install(li.bias, g_targets, TARGET_COUNT, g_diag_names, g_diag_count);
    }

    sleep(SURVIVE_SECONDS);
    if (guard_success(STATE_FILE) == 0)
        out("process stable for %d s, crash counter reset", SURVIVE_SECONDS);
    else
        out("warning: could not reset crash counter");
    /* Stay alive for the life of the process: a stream that has run for 60 s counts as survived. */
    for (;;) {
        sleep(10);
        hooks_stream_tick();
    }
    return nullptr;
}

class ProbeModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (args->uid % 100000 == TARGET_UID && args->nice_name) {
            const char *name = env->GetStringUTFChars(args->nice_name, nullptr);
            if (name) {
                target = strcmp(name, TARGET_PROCESS) == 0;
                env->ReleaseStringUTFChars(args->nice_name, name);
            }
        }
        if (target) {
            int dir = api->getModuleDir();
            if (dir < 0 || faccessat(dir, "disable_probe", F_OK, 0) == 0) {
                __android_log_write(ANDROID_LOG_WARN, TAG, "disabled by disable_probe (or no module dir)");
                target = false;
            }
            if (dir >= 0) {
                int fd = openat(dir, "diag_targets.txt", O_RDONLY | O_CLOEXEC);
                if (fd >= 0) {
                    char buf[4096];
                    ssize_t r = read(fd, buf, sizeof(buf));
                    close(fd);
                    if (r > 0) g_diag_count = diag_parse_names(buf, (size_t)r, g_diag_names);
                }
            }
            g_translate_allowed = dir >= 0 && faccessat(dir, "enable_translate", F_OK, 0) == 0;
            g_ssc_mute = dir >= 0 && faccessat(dir, "ssc_mute", F_OK, 0) == 0;
            g_ssc_ts_step = read_int_file(dir, "ssc_ts_step", -1);
            g_ssc_fpp = read_int_file(dir, "ssc_fpp", 0);
            g_ssc_uhq = dir >= 0 && faccessat(dir, "ssc_uhq", F_OK, 0) == 0;
            g_ssc_kbps = read_int_file(dir, "ssc_kbps", 0);
            g_ssc_pcm_hz = read_int_file(dir, "ssc_pcm_hz", 0); /* 0 = untouched; 48000 or 96000 (96000 needs ssc_uhq) */
            if (g_ssc_pcm_hz != 48000 && g_ssc_pcm_hz != 96000) g_ssc_pcm_hz = 0;
            if (g_ssc_pcm_hz == 96000 && !g_ssc_uhq) g_ssc_pcm_hz = 0;
            g_ssc_interval = read_int_file(dir, "ssc_interval_ms", g_ssc_uhq ? 9 : 18); /* one frame per tick */
            g_hooks_allowed = dir >= 0 && faccessat(dir, "disable_hooks", F_OK, 0) != 0;
            if (dir >= 0) close(dir);
        }
        if (!target) api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!target) return;
        int n = guard_begin(STATE_FILE);
        if (n <= 0) {
            __android_log_print(ANDROID_LOG_WARN, TAG, "not running: %s",
                                n == 0 ? "crash limit reached" : "cannot access state file");
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        pthread_t thread;
        if (pthread_create(&thread, nullptr, probe_thread, nullptr) != 0) {
            guard_success(STATE_FILE);
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        pthread_detach(thread);
        __android_log_print(ANDROID_LOG_INFO, TAG, "loaded in %s (run %d)", TARGET_PROCESS, n);
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *) override {
        api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    bool target = false;
};

REGISTER_ZYGISK_MODULE(ProbeModule)
