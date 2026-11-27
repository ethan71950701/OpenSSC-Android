/* Host test for the 0.4 SSC sender: PCM in, BT_HDR packets out, like the stack's Opus encoder. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssc_sender.h"

struct hdr { uint16_t event, len, offset, ls; uint8_t data[]; };
static double g_phase;
static int g_silent_source;
static uint32_t rd(uint8_t *buf, uint32_t len) {
    if (g_silent_source) return 0;
    int16_t *s = (int16_t *)buf;
    for (uint32_t i = 0; i < len / 4; i++) {
        int16_t v = (int16_t)(8000 * sin(g_phase));
        g_phase += 2 * M_PI * 1000 / 48000;
        s[2 * i] = v; s[2 * i + 1] = v;
    }
    return len;
}
static int g_uhq_test; static int g_pk; static int g_want_fpp = 2; static uint32_t g_want_step = 1728; static uint32_t g_last_ts; static int g_first = 1; static size_t g_frames_total;
static bool eq(void *p, size_t frames, uint32_t bytes) {
    hdr *h = (hdr *)p;
    uint32_t ts; memcpy(&ts, h->data, 4);
    const uint8_t *f = h->data + h->offset;
    if (g_uhq_test) { /* UHQ: one 660-byte E-family frame, rate code E3 (low 5 bits 03) */
        assert(h->offset == SSCS_BUF_OFFSET && h->ls == 1 && frames == 1 && h->len == 660);
        assert(f[0] == 0xff && (f[1] & 0xf0) == 0xe0 && (f[3] & 0x1f) == 0x03);
        g_pk++; free(p); return true;
    }
    assert(h->offset == SSCS_BUF_OFFSET && h->ls == g_want_fpp && (int)frames == g_want_fpp && h->len == 484 * g_want_fpp);
    assert(f[0] == 0xff && (f[1] & 0xf0) == 0xe0 && f[3] == 0xf3);       /* frame 1 header, rate code F3 = 192k */
    if (g_want_fpp == 2) assert(f[484] == 0xff && (f[485] & 0xf0) == 0xe0 && f[487] == 0xf3); /* frame 2 header */
    if (!g_first) assert(ts == g_last_ts + g_want_step);
    g_first = 0; g_last_ts = ts; g_pk++; g_frames_total += frames;
    (void)bytes;
    free(p);
    return true;
}
int main() {
    assert(sscs_tick(0) == -1); /* not started */
    sscs_set_options(-1, 2);
    assert(sscs_start(rd, eq, malloc, 1012) == 2);
    uint64_t t = 1000000;
    for (int i = 0; i < 100; i++) { sscs_tick(t); t += 20000; } /* 2 s */
    char tr[128]; int tn = sscs_take_trace(tr, sizeof tr);
    assert(tn >= 100 && tr[5] >= '7' && tr[5] <= '9'); /* 8000 peak sine -> level 7/8 */
    assert(sscs_take_trace(tr, sizeof tr) == 0);
    sscs_stats st; sscs_get_stats(&st);
    printf("sender: %u ticks, %u frames, %u packets\n", st.ticks, st.frames, st.packets);
    assert(st.frames >= 109 && st.frames <= 112 && g_pk == (int)st.packets && g_pk >= 54);
    /* the source runs dry: nothing is sent and nothing crashes */
    g_silent_source = 1; int before = g_pk;
    for (int i = 0; i < 10; i++) { sscs_tick(t); t += 20000; }
    assert(g_pk == before);
    /* a small MTU still fits one frame per packet; a tiny one is refused */
    assert(sscs_start(rd, eq, malloc, 600) == 1); /* asked for 2, MTU allows 1 */
    assert(sscs_start(rd, eq, malloc, 300) < 0 && sscs_tick(t) == -1);
    /* 0.4.2 knobs: one frame per packet, timestamp step 1 per frame */
    sscs_set_options(1, 1);
    g_silent_source = 0; g_first = 1; g_want_fpp = 1; g_want_step = 1; before = g_pk;
    assert(sscs_start(rd, eq, malloc, 1012) == 1);
    sscs_get_stats(&st);
    assert(st.ts_step == 1 && st.frames_per_packet == 1);
    for (int i = 0; i < 20; i++) { sscs_tick(t); t += 20000; }
    assert(g_pk - before >= 20);
    sscs_set_options(-1, 0);
    assert(sscs_start(rd, eq, malloc, 1012) == 1); /* default: one frame per packet */
    /* UHQ: 48 kHz in, 96 kHz / 584 kb/s out, 9 ms ticks -> about one packet per tick */
    sscs_set_mode(1); g_uhq_test = 1; before = g_pk;
    assert(sscs_start(rd, eq, malloc, 1012) == 1);
    sscs_get_stats(&st);
    assert(st.frame_bytes == 660 && st.sample_rate == 96000 && st.bitrate == 584000);
    for (int i = 0; i < 222; i++) { sscs_tick(t); t += 9000; } /* 2 s */
    printf("uhq: %d packets in 2 s\n", g_pk - before);
    assert(g_pk - before >= 218 && g_pk - before <= 223);
    sscs_set_mode(0); g_uhq_test = 0;
    /* 229 kb/s at 48 kHz: 576-byte frames */
    sscs_set_bitrate(229000);
    assert(sscs_start(rd, eq, malloc, 1012) == 1);
    sscs_get_stats(&st);
    assert(st.frame_bytes == 576 && st.bitrate == 229000);
    sscs_set_bitrate(0);
    sscs_stop();
    printf("sender test: OK\n");
    return 0;
}
