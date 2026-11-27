#include "ssc_sender.h"

#include <math.h>
#include <string.h>

#include "ssc_bridge.h"
#include "ssc_encoder.h"

#define CH 2
#define BYTES_PER_SAMPLE_FRAME (CH * 2)            /* 16-bit stereo */
#define MAX_TICK_US 100000                         /* catch-up cap after a stall */
#define PCM_CAP (SSC_ENC_FRAME_SAMPLES * 4)        /* buffered input sample frames (about 72 ms at 48 kHz) */
#define UP_K 16                                    /* half-band upsampler: taps per side of the odd phase */
#define UHQ_RATE 96000
#define UHQ_BITRATE 584000                         /* config 0x0A, rate code E3, 660-byte frames (S25+ capture) */
#define MAX_FPP 4
#define DEFAULT_FPP 1 /* Buds 3 Pro firmware accepts exactly one SSC frame per A2DP packet at 48 kHz */

/* BT_HDR as the stack lays it out: uint16 event, len, offset, layer_specific; then data[]. */
struct bt_hdr {
    uint16_t event, len, offset, layer_specific;
    uint8_t data[];
};

static ssc_encoder *g_enc, *g_enc48, *g_enc96;
static int g_opt_uhq, g_uhq, g_in_frame, g_bitrate, g_hist; /* g_in_frame: input samples per SSC frame */
static float g_upc[UP_K];          /* odd-phase coefficients */
static float g_up[SSC_ENC_FRAME_SAMPLES * 2];
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

void sscs_set_mode(int uhq) { g_opt_uhq = uhq; }
static int g_opt_bitrate;
void sscs_set_bitrate(int bps) { g_opt_bitrate = bps; }
static int g_opt_pcm_hz, g_in_rate = SSCS_SAMPLE_RATE, g_true96;
void sscs_set_pcm_rate(int hz) { g_opt_pcm_hz = hz; }
/* 32-bit PCM input (true 96 kHz only): int32 full scale, 8 bytes per stereo sample frame, kept as floats. */
static int g_opt_pcm_bits, g_in32;
static int32_t g_raw32[PCM_CAP * CH];
static float g_pcmf[PCM_CAP * CH];
static int g_pcmf_n;
void sscs_set_pcm_bits(int bits) { g_opt_pcm_bits = bits; }

static void up_init(void) {
    double sum = 0;
    for (int k = 0; k < UP_K; k++) {
        double x = k + 0.5, t = M_PI * x;
        double w = 0.42 + 0.5 * cos(M_PI * x / UP_K) + 0.08 * cos(2 * M_PI * x / UP_K); /* Blackman, centred */
        g_upc[k] = (float)(sin(t) / t * w);
        sum += 2 * g_upc[k];
    }
    for (int k = 0; k < UP_K; k++) g_upc[k] = (float)(g_upc[k] / sum);
}

/* 2x upsampling of in[0 .. n) (UP_K samples of history before, UP_K after) into float out[2n] (full scale 1.0). */
static void upsample2(const int16_t *in, int n, float *out) {
    const float s = 1.0f / 32768.0f;
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < CH; c++) {
            out[(2 * i) * CH + c] = in[i * CH + c] * s;
            float acc = 0;
            for (int k = 0; k < UP_K; k++) acc += g_upc[k] * (float)(in[(i - k) * CH + c] + in[(i + 1 + k) * CH + c]);
            out[(2 * i + 1) * CH + c] = acc * s;
        }
    }
}

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
    g_pcm_n = g_hist; /* UHQ: the upsampler's history starts as silence */
    memset(g_pcm, 0, (size_t)g_hist * CH * sizeof(int16_t));
    g_pcmf_n = 0;
    g_pending = 0;
    g_bytes_read = 0;
    g_last_us = 0;
    g_frac = 0;
    g_ts = 0;
}

int sscs_start(sscs_read_fn read, sscs_enqueue_fn enqueue, sscs_alloc_fn alloc, uint16_t peer_mtu) {
    g_started = 0;
    if (!read || !enqueue || !alloc) return -1;
    g_uhq = g_opt_uhq;
    ssc_encoder **slot = g_uhq ? &g_enc96 : &g_enc48;
    if (!*slot) *slot = ssc_encoder_create(g_uhq ? UHQ_RATE : SSCS_SAMPLE_RATE);
    g_enc = *slot;
    if (!g_enc) return -2;
    ssc_encoder_reset(g_enc);
    /* True 96 kHz input (UHQ only): the stack hands over 96 kHz PCM, so no upsampling. Otherwise the input is 48 kHz. */
    g_true96 = g_uhq && g_opt_pcm_hz == UHQ_RATE;
    g_in_rate = g_true96 ? UHQ_RATE : SSCS_SAMPLE_RATE;
    g_in32 = g_true96 && g_opt_pcm_bits == 32;
    g_pcmf_n = 0;
    if (g_uhq && !g_true96) up_init();
    g_in_frame = g_true96 ? SSC_ENC_FRAME_SAMPLES : g_uhq ? SSC_ENC_FRAME_SAMPLES / 2 : SSC_ENC_FRAME_SAMPLES;
    g_hist = g_uhq && !g_true96 ? UP_K : 0;
    g_bitrate = g_opt_bitrate > 0 ? g_opt_bitrate : g_uhq ? UHQ_BITRATE : SSCS_BITRATE;
    g_frame_bytes = ssc_encoder_frame_bytes(g_uhq ? UHQ_RATE : SSCS_SAMPLE_RATE, g_bitrate);
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
    g_st.sample_rate = g_uhq ? UHQ_RATE : SSCS_SAMPLE_RATE;
    g_st.bitrate = g_bitrate;
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
    uint64_t want_e6 = dt * (uint64_t)g_in_rate + g_frac;
    int want = (int)(want_e6 / 1000000);
    g_frac = want_e6 % 1000000;
    if (g_in32) {
        if (want > PCM_CAP - g_pcmf_n) want = PCM_CAP - g_pcmf_n;
        if (want > 0) {
            uint32_t len = (uint32_t)want * (CH * 4);
            uint8_t *dst = reinterpret_cast<uint8_t *>(g_raw32);
            uint32_t got = g_read(dst, len);
            if (got == 0) {
                g_st.underflows++;
                return 0;
            }
            if (got > len) got = len;
            if (got < len) {
                g_st.underflows++;
                memset(dst + got, 0, len - got);
            }
            g_bytes_read += got;
            const float sc = 1.0f / 2147483648.0f;
            for (int i = 0; i < want * CH; i++) g_pcmf[g_pcmf_n * CH + i] = (float)g_raw32[i] * sc;
            g_pcmf_n += want;
        }
        int sent32 = 0;
        while (g_pcmf_n >= g_in_frame) {
            uint8_t *dst = g_frames + g_pending * g_frame_bytes;
            int r = ssc_encode_float(g_enc, g_pcmf, g_bitrate, 0, dst, g_frame_bytes);
            g_pcmf_n -= g_in_frame;
            memmove(g_pcmf, g_pcmf + g_in_frame * CH, (size_t)g_pcmf_n * CH * sizeof(float));
            if (r != g_frame_bytes) continue;
            g_st.frames++;
            if (++g_pending == g_fpp) {
                int s = send_packet();
                if (s < 0) return sent32;
                sent32 += s;
            }
        }
        return sent32;
    }
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
    while (g_pcm_n >= 2 * g_hist + g_in_frame) {
        const int16_t *in = g_pcm + g_hist * CH;
        if (g_trace_n < TRACE_CAP) g_trace[g_trace_n++] = level_char(in, g_in_frame * CH);
        uint8_t *dst = g_frames + g_pending * g_frame_bytes;
        int r;
        if (g_true96) {
            const float sc = 1.0f / 32768.0f;
            for (int i = 0; i < g_in_frame * CH; i++) g_up[i] = in[i] * sc;
            r = ssc_encode_float(g_enc, g_up, g_bitrate, 0, dst, g_frame_bytes);
        } else if (g_uhq) {
            upsample2(in, g_in_frame, g_up);
            r = ssc_encode_float(g_enc, g_up, g_bitrate, 0, dst, g_frame_bytes);
        } else {
            r = ssc_encode(g_enc, in, g_bitrate, 0, dst, g_frame_bytes);
        }
        g_pcm_n -= g_in_frame;
        memmove(g_pcm, g_pcm + g_in_frame * CH, (size_t)g_pcm_n * CH * sizeof(int16_t));
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
