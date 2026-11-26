#include "ssc_sender.h"

#include <string.h>

#include "ssc_bridge.h"
#include "ssc_encoder.h"

#define CH 2
#define BYTES_PER_SAMPLE_FRAME (CH * 2)            /* 16-bit stereo */
#define MAX_TICK_US 100000                         /* catch-up cap after a stall */
#define PCM_CAP (SSC_ENC_FRAME_SAMPLES * 4)        /* buffered sample frames (about 72 ms) */
#define MAX_FPP 4
#define DEFAULT_FPP 1 /* 2 frames per packet gave regular dropouts on Buds 3 Pro (0.4.2 test); 1 plays cleanly */

/* BT_HDR as the stack lays it out: uint16 event, len, offset, layer_specific; then data[]. */
struct bt_hdr {
    uint16_t event, len, offset, layer_specific;
    uint8_t data[];
};

static ssc_encoder *g_enc;
static sscs_read_fn g_read;
static sscs_enqueue_fn g_enqueue;
static sscs_alloc_fn g_alloc;
static sscb_packer g_packer;
static int g_started, g_frame_bytes, g_fpp;
static int16_t g_pcm[PCM_CAP * CH];
static int g_pcm_n;                 /* sample frames buffered */
static uint8_t g_frames[MAX_FPP * SSC_ENC_MAX_FRAME_BYTES];
static int g_pending;               /* encoded frames waiting for a packet */
static uint32_t g_bytes_read;       /* PCM bytes read since the last enqueue (stack statistics) */
static uint64_t g_last_us;
static uint64_t g_frac;             /* sample remainder, in 1/1e6 samples */
static sscs_stats g_st;
static int g_opt_ts_step = -1, g_opt_fpp = 0;
static int g_ts_step;
static uint32_t g_ts;

void sscs_set_options(int ts_step, int max_fpp) {
    g_opt_ts_step = ts_step;
    g_opt_fpp = max_fpp;
}
#define TRACE_CAP 128
static char g_trace[TRACE_CAP];
static int g_trace_n;

int sscs_take_trace(char *out, int cap) {
    int n = g_trace_n < cap - 1 ? g_trace_n : cap - 1;
    if (n < 0) n = 0;
    memcpy(out, g_trace, (size_t)n);
    out[n] = 0;
    g_trace_n = 0;
    return n;
}

static char level_char(const int16_t *pcm, int n) {
    int peak = 0;
    for (int i = 0; i < n; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak) peak = v;
    }
    if (peak < 16) return ' ';
    int c = 0;
    while (peak > 32 && c < 9) { peak >>= 1; c++; } /* 32..32767 -> 0..9 */
    return (char)('0' + c);
}

void sscs_get_stats(sscs_stats *out) {
    if (out) *out = g_st;
}

void sscs_reset(void) {
    g_pcm_n = 0;
    g_pending = 0;
    g_bytes_read = 0;
    g_last_us = 0;
    g_frac = 0;
    g_ts = 0;
}

int sscs_start(sscs_read_fn read, sscs_enqueue_fn enqueue, sscs_alloc_fn alloc, uint16_t peer_mtu) {
    g_started = 0;
    if (!read || !enqueue || !alloc) return -1;
    if (!g_enc) g_enc = ssc_encoder_create(SSCS_SAMPLE_RATE);
    if (!g_enc) return -2;
    ssc_encoder_reset(g_enc);
    g_frame_bytes = ssc_encoder_frame_bytes(SSCS_SAMPLE_RATE, SSCS_BITRATE);
    if (g_frame_bytes <= 0) return -3;
    g_fpp = sscb_packer_init(&g_packer, g_frame_bytes, peer_mtu,
                             g_opt_fpp >= 1 && g_opt_fpp <= MAX_FPP ? g_opt_fpp : DEFAULT_FPP, 0);
    g_ts_step = g_opt_ts_step >= 0 ? g_opt_ts_step : SSC_ENC_FRAME_SAMPLES;
    if (g_fpp < 1) return -4;
    if (SSCS_BUF_OFFSET + g_fpp * g_frame_bytes > SSCS_BUF_SIZE - (int)sizeof(bt_hdr)) return -5;
    g_read = read;
    g_enqueue = enqueue;
    g_alloc = alloc;
    memset(&g_st, 0, sizeof(g_st));
    g_st.frame_bytes = g_frame_bytes;
    g_st.frames_per_packet = g_fpp;
    g_st.ts_step = g_ts_step;
    sscs_reset();
    g_trace_n = 0;
    g_started = 1;
    return g_fpp;
}

void sscs_stop(void) { g_started = 0; }

static int send_packet(void) {
    bt_hdr *p = static_cast<bt_hdr *>(g_alloc(SSCS_BUF_SIZE));
    if (!p) return 0;
    uint32_t ts = 0;
    int n = sscb_packer_payload(&g_packer, g_frames, g_pending, p->data + SSCS_BUF_OFFSET,
                                SSCS_BUF_SIZE - (int)sizeof(bt_hdr) - SSCS_BUF_OFFSET, &ts);
    if (n <= 0) return 0; /* cannot happen with the sizes checked in sscs_start; buffer intentionally kept by caller */
    ts = g_ts; /* own timestamp so the step per frame can be varied (packer default: 864) */
    g_ts += (uint32_t)(g_pending * g_ts_step);
    p->event = 0;
    p->len = (uint16_t)n;
    p->offset = SSCS_BUF_OFFSET;
    p->layer_specific = (uint16_t)g_pending;
    memcpy(p->data, &ts, sizeof(ts)); /* the stack reads the RTP timestamp from here */
    size_t frames = (size_t)g_pending;
    uint32_t br = g_bytes_read;
    g_pending = 0;
    g_bytes_read = 0;
    g_st.packets++;
    if (!g_enqueue(p, frames, br)) { /* the queue owns the buffer either way */
        g_st.enqueue_failed++;
        return -1;
    }
    return 1;
}

int sscs_tick(uint64_t now_us) {
    if (!g_started) return -1;
    g_st.ticks++;
    uint64_t dt = g_last_us && now_us > g_last_us ? now_us - g_last_us : 20000;
    if (dt > MAX_TICK_US) dt = MAX_TICK_US;
    g_last_us = now_us;
    uint64_t want_e6 = dt * SSCS_SAMPLE_RATE + g_frac;
    int want = (int)(want_e6 / 1000000);
    g_frac = want_e6 % 1000000;
    if (want > PCM_CAP - g_pcm_n) want = PCM_CAP - g_pcm_n;

    if (want > 0) {
        uint32_t len = (uint32_t)want * BYTES_PER_SAMPLE_FRAME;
        uint8_t *dst = reinterpret_cast<uint8_t *>(g_pcm + g_pcm_n * CH);
        uint32_t got = g_read(dst, len);
        if (got == 0) {
            g_st.underflows++;
            return 0; /* nothing to play: like the Opus encoder, send nothing */
        }
        if (got > len) got = len;
        if (got < len) {
            g_st.underflows++;
            memset(dst + got, 0, len - got); /* pad a short read with silence, as the Opus encoder does */
        }
        g_bytes_read += got;
        g_pcm_n += want;
    }

    int sent = 0;
    while (g_pcm_n >= SSC_ENC_FRAME_SAMPLES) {
        if (g_trace_n < TRACE_CAP) g_trace[g_trace_n++] = level_char(g_pcm, SSC_ENC_FRAME_SAMPLES * CH);
        int r = ssc_encode(g_enc, g_pcm, SSCS_BITRATE, 0, g_frames + g_pending * g_frame_bytes, g_frame_bytes);
        g_pcm_n -= SSC_ENC_FRAME_SAMPLES;
        memmove(g_pcm, g_pcm + SSC_ENC_FRAME_SAMPLES * CH, (size_t)g_pcm_n * CH * sizeof(int16_t));
        if (r != g_frame_bytes) continue; /* encoder error: drop the frame */
        g_st.frames++;
        if (++g_pending == g_fpp) {
            int s = send_packet();
            if (s < 0) return sent; /* queue refused: stop this tick */
            sent += s;
        }
    }
    return sent;
}
