/* OpenSSC probe (stage 1): runs inside com.android.bluetooth only, waits for the Bluetooth stack
 * library, looks up the functions the hook design needs and writes a report. It installs no hooks,
 * sets no properties and changes no Bluetooth data. */
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
#include "symres.h"
#include "zygisk.hpp"

#define TAG "OpenSSCProbe"
#define VERSION "0.1"
#define TARGET_PROCESS "com.android.bluetooth"
#define TARGET_UID 1002
#define TARGET_LIB "libbluetooth_jni.so"
#define DATA_DIR "/data/user_de/0/com.android.bluetooth"
#define STATE_FILE DATA_DIR "/openssc_probe_state"
#define REPORT_FILE DATA_DIR "/openssc_probe_report.txt"
#define WAIT_SECONDS 120
#define SURVIVE_SECONDS 60

static sr_target g_targets[] = {
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
};
#define TARGET_COUNT (sizeof(g_targets) / sizeof(g_targets[0]))

static int g_report_fd = -1;

__attribute__((format(printf, 1, 2))) static void out(const char *fmt, ...) {
    char line[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    __android_log_write(ANDROID_LOG_INFO, TAG, line);
    if (g_report_fd >= 0) dprintf(g_report_fd, "%s\n", line);
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

static void resolve(const lib_info &li) {
    out("library: %s", li.path);
    print_build_id(li);

    int fd = open(li.path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
        out("RESULT: FAIL cannot open library file");
        if (fd >= 0) close(fd);
        return;
    }
    size_t len = static_cast<size_t>(st.st_size);
    void *map = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        out("RESULT: FAIL mmap");
        return;
    }
    int rc = sr_scan(static_cast<const uint8_t *>(map), len, g_targets, TARGET_COUNT);
    munmap(map, len);
    if (rc != SR_OK) {
        out("RESULT: FAIL symbol scan error %d", rc);
        return;
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
}

static void *probe_thread(void *) {
    lib_info li = {};
    int waited = 0;
    while (!dl_iterate_phdr(find_lib, &li) && waited < WAIT_SECONDS * 2) {
        usleep(500 * 1000);
        waited++;
    }
    g_report_fd = open(REPORT_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    out("OpenSSC probe %s, pid %d (no hooks installed)", VERSION, getpid());
    if (g_report_fd < 0) out("warning: cannot write report file, logcat only");
    if (li.path[0])
        resolve(li);
    else
        out("RESULT: FAIL %s not loaded after %d s", TARGET_LIB, WAIT_SECONDS);
    if (g_report_fd >= 0) close(g_report_fd);
    g_report_fd = -1;

    sleep(SURVIVE_SECONDS);
    if (guard_success(STATE_FILE) == 0)
        out("process stable for %d s, crash counter reset", SURVIVE_SECONDS);
    else
        out("warning: could not reset crash counter");
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
